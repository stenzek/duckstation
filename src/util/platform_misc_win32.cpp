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

LOG_CHANNEL(PlatformMisc);

static bool s_screensaver_suspended = false;
static bool s_winsock_initialized = false;
static std::once_flag s_winsock_initializer;

bool PlatformMisc::InitializeSocketSupport(Error* error)
{
  std::call_once(
    s_winsock_initializer,
    [](Error* error) {
      WSADATA wsa = {};
      if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
      {
        Error::SetSocket(error, "WSAStartup() failed: ", WSAGetLastError());
        return false;
      }

      s_winsock_initialized = true;
      std::atexit([]() { WSACleanup(); });
      return true;
    },
    error);

  return s_winsock_initialized;
}

