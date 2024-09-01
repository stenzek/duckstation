// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "window_info.h"

#include <optional>

class Error;

namespace PlatformMisc {
bool InitializeSocketSupport(Error* error);
void SuspendScreensaver();
void ResumeScreensaver();

/// Returns the size of pages for the current host.
size_t GetRuntimePageSize();

/// Abstracts platform-specific code for asynchronously playing a sound.
/// On Windows, this will use PlaySound(). On Linux, it will shell out to aplay. On MacOS, it uses NSSound.
bool PlaySoundAsync(const char* path);
} // namespace PlatformMisc

namespace Host {
/// Return the current window handle. Needed for DInput.
std::optional<WindowInfo> GetTopLevelWindowInfo();
} // namespace Host

// TODO: Move all the other Cocoa stuff in here.
namespace CocoaTools {
/// Returns the refresh rate of the display the window is placed on.
std::optional<float> GetViewRefreshRate(const WindowInfo& wi);
}
