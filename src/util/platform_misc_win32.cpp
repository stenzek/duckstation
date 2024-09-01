// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "platform_misc.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include <algorithm>
#include <cinttypes>
#include <memory>

#include "common/windows_headers.h"
#include <Psapi.h>
#include <WinSock2.h>
#include <mmsystem.h>

Log_SetChannel(PlatformMisc);

static bool s_screensaver_suspended = false;
static bool s_winsock_initialized = false;
static std::once_flag s_winsock_initializer;

bool PlatformMisc::InitializeSocketSupport(Error* error)
{
  std::call_once(s_winsock_initializer, [](Error* error) {
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
      Error::SetSocket(error, "WSAStartup() failed: ", WSAGetLastError());
      return false;
    }

    s_winsock_initialized = true;
    std::atexit([]() { WSACleanup(); });
    return true;
  }, error);

  return s_winsock_initialized;
}

static bool SetScreensaverInhibitWin32(bool inhibit)
{
  if (SetThreadExecutionState(ES_CONTINUOUS | (inhibit ? (ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : 0)) == NULL)
  {
    ERROR_LOG("SetThreadExecutionState() failed: {}", GetLastError());
    return false;
  }

  return true;
}

void PlatformMisc::SuspendScreensaver()
{
  if (s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibitWin32(true))
  {
    ERROR_LOG("Failed to suspend screensaver.");
    return;
  }

  s_screensaver_suspended = true;
}

void PlatformMisc::ResumeScreensaver()
{
  if (!s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibitWin32(false))
    ERROR_LOG("Failed to resume screensaver.");

  s_screensaver_suspended = false;
}

size_t PlatformMisc::GetRuntimePageSize()
{
  SYSTEM_INFO si = {};
  GetSystemInfo(&si);
  return si.dwPageSize;
}

bool PlatformMisc::PlaySoundAsync(const char* path)
{
  const std::wstring wpath(FileSystem::GetWin32Path(path));
  return PlaySoundW(wpath.c_str(), NULL, SND_ASYNC | SND_NODEFAULT);
}
