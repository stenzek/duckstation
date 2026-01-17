// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// Normally, system includes come last. But apparently some of our macro names are redefined...
#include <Cocoa/Cocoa.h>
#include <QuartzCore/QuartzCore.h>
#include <cinttypes>
#include <optional>
#include <vector>

#include "platform_misc.h"
#include "window_info.h"

#include "common/error.h"
#include "common/log.h"
#include "common/small_string.h"

LOG_CHANNEL(PlatformMisc);

#if __has_feature(objc_arc)
#error ARC should not be enabled.
#endif

bool PlatformMisc::InitializeSocketSupport(Error* error)
{
  return true;
}
