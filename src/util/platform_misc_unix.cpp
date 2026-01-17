// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "input_manager.h"
#include "platform_misc.h"

#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/small_string.h"

#include <cinttypes>
#include <mutex>
#include <signal.h>
#include <unistd.h>

LOG_CHANNEL(PlatformMisc);

bool PlatformMisc::InitializeSocketSupport(Error* error)
{
  // Ignore SIGPIPE, we handle errors ourselves.
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
  {
    Error::SetErrno(error, "signal(SIGPIPE, SIG_IGN) failed: ", errno);
    return false;
  }

  return true;
}

bool PlatformMisc::SetWindowRoundedCornerState(void* window_handle, bool enabled, Error* error /* = nullptr */)
{
  Error::SetStringView(error, "Unsupported on this platform.");
  return false;
}
