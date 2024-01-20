// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/progress_callback.h"
#include "common/types.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class SmallStringBase;

struct Settings;

namespace FullscreenUI {
bool Initialize();
bool IsInitialized();
bool HasActiveWindow();
void CheckForConfigChanges(const Settings& old_settings);
void OnSystemStarted();
void OnSystemPaused();
void OnSystemResumed();
void OnSystemDestroyed();
void OnRunningGameChanged();

#ifndef __ANDROID__
void OpenPauseMenu();
void OpenAchievementsWindow();
bool IsAchievementsWindowOpen();
void OpenLeaderboardsWindow();
bool IsLeaderboardsWindowOpen();
void ReturnToMainWindow();
void ReturnToPreviousWindow();
#endif

void Shutdown();
void Render();
void InvalidateCoverCache();
void TimeToPrintableString(SmallStringBase* str, time_t t);

} // namespace FullscreenUI

// Host UI triggers from Big Picture mode.
namespace Host {
void OnCoverDownloaderOpenRequested();
} // namespace Host
