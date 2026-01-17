// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "window_info.h"

#include <optional>

class Error;

namespace PlatformMisc {

bool InitializeSocketSupport(Error* error);

/// Sets the rounded corner state for a window.
/// Currently only supported on Windows.
bool SetWindowRoundedCornerState(void* window_handle, bool enabled, Error* error = nullptr);

} // namespace PlatformMisc

namespace Host {

/// Return the current window handle. Needed for DInput.
std::optional<WindowInfo> GetTopLevelWindowInfo();

} // namespace Host

// TODO: Move all the other Cocoa stuff in here.
namespace CocoaTools {

/// Returns the refresh rate of the display the window is placed on.
std::optional<float> GetViewRefreshRate(const WindowInfo& wi, Error* error);

} // namespace CocoaTools
