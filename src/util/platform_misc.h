// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "window_info.h"

#include <optional>

namespace PlatformMisc {
void SuspendScreensaver();
void ResumeScreensaver();

/// Abstracts platform-specific code for asynchronously playing a sound.
/// On Windows, this will use PlaySound(). On Linux, it will shell out to aplay. On MacOS, it uses NSSound.
bool PlaySoundAsync(const char* path);
} // namespace PlatformMisc

namespace Host {
/// Return the current window handle. Needed for DInput.
std::optional<WindowInfo> GetTopLevelWindowInfo();
} // namespace Host