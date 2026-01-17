// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "window_info.h"

#include <optional>

class Error;

namespace PlatformMisc {

bool InitializeSocketSupport(Error* error);

} // namespace PlatformMisc

namespace Host {

/// Return the current window handle. Needed for DInput.
std::optional<WindowInfo> GetTopLevelWindowInfo();

} // namespace Host
