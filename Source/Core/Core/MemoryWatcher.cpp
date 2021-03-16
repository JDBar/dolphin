// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <fstream>
#include <iostream>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <fileapi.h>
#include <handleapi.h>
#else
#include <unistd.h>
#endif

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/HW/SystemTimers.h"
#include "Core/MemoryWatcher.h"
#include "Core/PowerPC/MMU.h"

MemoryWatcher::MemoryWatcher()
{
  m_running = false;
  if (!OpenSocket(File::GetUserPath(F_MEMORYWATCHERSOCKET_IDX)))
    return;
  LoadAddresses(File::GetUserPath(F_MEMORYWATCHERLOCATIONS_IDX));
  INFO_LOG_FMT(CORE, "MemoryWatcher is active.");
  m_running = true;
}

MemoryWatcher::~MemoryWatcher()
{
  if (!m_running)
    return;

  m_running = false;
  #ifdef _WIN32
    CloseHandle(m_pipe);
  #else
    close(m_fd);
  #endif
}

bool MemoryWatcher::LoadAddresses(const std::string& path)
{
  std::ifstream locations;
  File::OpenFStream(locations, path, std::ios_base::in);
  if (!locations)
    return false;

  std::string line;
  while (std::getline(locations, line))
    ParseLine(line);

  return !m_values.empty();
}

void MemoryWatcher::ParseLine(const std::string& line)
{
  m_values[line] = 0;
  m_addresses[line] = std::vector<u32>();

  std::istringstream offsets(line);
  offsets >> std::hex;
  u32 offset;
  while (offsets >> offset)
    m_addresses[line].push_back(offset);
}

bool MemoryWatcher::OpenSocket(const std::string& path)
{
  #ifdef _WIN32
    //m_pipe = CreateFile(
    //  L"\\\\.\\pipe\\Dolphin Emulator\\MemoryWatcher",
    //  GENERIC_READ | GENERIC_WRITE,
    //  0,
    //  nullptr,
    //  OPEN_EXISTING,
    //  FILE_ATTRIBUTE_NORMAL,
    //  nullptr
    //);

    m_pipe = CreateNamedPipe(
      TEXT("\\\\.\\pipe\\Dolphin Emulator\\MemoryWatcher"),
      PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT,
      1,
      1024 * 16,
      1024 * 16,
      NMPWAIT_USE_DEFAULT_WAIT,
      NULL
    );
    ConnectNamedPipe(m_pipe, NULL);

    return m_pipe != INVALID_HANDLE_VALUE;
  #else
    m_addr.sun_family = AF_UNIX;
    strncpy(m_addr.sun_path, path.c_str(), sizeof(m_addr.sun_path) - 1);

    m_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    return m_fd >= 0;
  #endif
}

u32 MemoryWatcher::ChasePointer(const std::string& line)
{
  u32 value = 0;
  for (u32 offset : m_addresses[line])
  {
    value = PowerPC::HostRead_U32(value + offset);
    if (!PowerPC::HostIsRAMAddress(value))
      break;
  }
  return value;
}

std::string MemoryWatcher::ComposeMessages()
{
  std::ostringstream message_stream;
  message_stream.imbue(std::locale::classic());
  message_stream << std::hex;

  for (auto& entry : m_values)
  {
    std::string address = entry.first;
    u32& current_value = entry.second;

    u32 new_value = ChasePointer(address);
    if (new_value != current_value)
    {
      // Update the value
      current_value = new_value;
      message_stream << address << '\n'
                     << std::setfill('0') << std::setw(8) << std::hex
                     << int(new_value) << '\n';
    }
  }

  return message_stream.str();
}

void MemoryWatcher::Step()
{
  if (!m_running)
    return;

  std::string memoryChanges = ComposeMessages();

  #ifdef _WIN32
    DWORD dwRead;

    if (m_pipe == INVALID_HANDLE_VALUE)
    {
      return;
    }

    // Receive messages.
    while (ReadFile(m_pipe, m_readBuffer, sizeof(m_readBuffer) - 1, &dwRead, NULL))
    {
      if (dwRead <= 0)
        continue;

      m_readBuffer[dwRead] = '\0';
      std::string message(m_readBuffer);

      INFO_LOG_FMT(CORE, "MemoryWatcher message recieved: {}", message);

      std::string ack = "ACK\n" + message;
      WriteFile(m_pipe, ack.c_str(), static_cast<DWORD>(ack.size() + 1), NULL, NULL);
    }

    // Send changes
    unsigned long written;
    if (memoryChanges.length())
    {
      std::string message = "CHANGED\n" + memoryChanges;
      WriteFile(m_pipe, message.c_str(), static_cast<DWORD>(message.length() + 1), &written, nullptr);
    }

    unsigned long error = GetLastError();
    if (error == m_lastError)
      return;
    m_lastError = error;
    switch (error)
    {
      case ERROR_BROKEN_PIPE:
        DisconnectNamedPipe(m_pipe);
        ConnectNamedPipe(m_pipe, NULL);
        break;
      case ERROR_NO_DATA:
        break;
      default:
        INFO_LOG_FMT(CORE, "MemoryWatcher pipe error code: {}", error);
        break;
    }

  #else
    sendto(m_fd, message.c_str(), message.size() + 1, 0, reinterpret_cast<sockaddr*>(&m_addr),
           sizeof(m_addr));
  #endif
}
