// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

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
void OnSystemResumed();
void OnSystemDestroyed();
void OnRunningGameChanged();

#ifndef __ANDROID__
void OpenPauseMenu();
void OpenCheatsMenu();
void OpenAchievementsWindow();
bool IsAchievementsWindowOpen();
void OpenLeaderboardsWindow();
bool IsLeaderboardsWindowOpen();
void ReturnToMainWindow();
void ReturnToPreviousWindow();
void SetStandardSelectionFooterText(bool back_instead_of_cancel);
#endif

void Shutdown();
void Render();
void InvalidateCoverCache();
void TimeToPrintableString(SmallStringBase* str, time_t t);

void OpenLoadingScreen(std::string_view image, std::string_view message, s32 progress_min = -1, s32 progress_max = -1,
                       s32 progress_value = -1);
void UpdateLoadingScreen(std::string_view image, std::string_view message, s32 progress_min = -1, s32 progress_max = -1,
                         s32 progress_value = -1);
void CloseLoadingScreen();

} // namespace FullscreenUI

class LoadingScreenProgressCallback final : public ProgressCallback
{
public:
  LoadingScreenProgressCallback();
  ~LoadingScreenProgressCallback() override;

  ALWAYS_INLINE void SetOpenDelay(float delay) { m_open_delay = delay; }

  void PushState() override;
  void PopState() override;

  void SetCancellable(bool cancellable) override;
  void SetTitle(const std::string_view title) override;
  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void ModalError(const std::string_view message) override;
  bool ModalConfirmation(const std::string_view message) override;

private:
  void Redraw(bool force);

  u64 m_open_time = 0;
  float m_open_delay = 1.0f;
  s32 m_last_progress_percent = -1;
  bool m_on_gpu_thread = false;
};

// Host UI triggers from Big Picture mode.
namespace Host {

#ifndef __ANDROID__

/// Called whenever fullscreen UI starts/stops.
void OnFullscreenUIStartedOrStopped(bool started);

/// Requests shut down and exit of the hosting application. This may not actually exit,
/// if the user cancels the shutdown confirmation.
void RequestExitApplication(bool allow_confirm);

/// Requests Big Picture mode to be shut down, returning to the desktop interface.
void RequestExitBigPicture();

/// Requests the cover downloader be opened.
void OnCoverDownloaderOpenRequested();

#endif

} // namespace Host
