// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

namespace FrontendCommon {
void SuspendScreensaver();
void ResumeScreensaver();

/// Abstracts platform-specific code for asynchronously playing a sound.
/// On Windows, this will use PlaySound(). On Linux, it will shell out to aplay. On MacOS, it uses NSSound.
bool PlaySoundAsync(const char* path);

#ifdef __APPLE__
/// Add a handler to be run when macOS changes between dark and light themes
void AddThemeChangeHandler(void* ctx, void(handler)(void* ctx));
/// Remove a handler previously added using AddThemeChangeHandler with the given context
void RemoveThemeChangeHandler(void* ctx);
#endif
} // namespace FrontendCommon
