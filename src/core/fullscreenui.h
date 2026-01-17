// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/progress_callback.h"

#include "types.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class SmallStringBase;

struct GPUSettings;

namespace FullscreenUI {
void Initialize(bool preserve_state = false);
bool IsInitialized();
bool HasActiveWindow();
void CheckForConfigChanges(const GPUSettings& old_settings);
void OnSystemStarting();
void OnSystemPaused();
void OnSystemResumed();
void OnSystemDestroyed();

void Shutdown(bool preserve_state);
void Render();
void InvalidateCoverCache(std::string path = {});

float GetBackgroundAlpha();

void UpdateTheme();
void UpdateRunIdleState();

#ifndef __ANDROID__

void OpenPauseMenu();
void OpenCheatsMenu();
void OpenDiscChangeMenu();
void OpenAchievementsWindow();
void OpenLeaderboardsWindow();

class BackgroundProgressCallback final : public ProgressCallback
{
public:
  explicit BackgroundProgressCallback(std::string name);
  ~BackgroundProgressCallback() override;

  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void SetCancelled();

private:
  void Redraw(bool force);

  std::string m_name;
  int m_last_progress_percent = -1;
};

#endif // __ANDROID__

// NOTE: Not in widgets.h so that clients can use it without pulling in imgui etc.
class LoadingScreenProgressCallback final : public ProgressCallback
{
public:
  LoadingScreenProgressCallback();
  ~LoadingScreenProgressCallback() override;

  ALWAYS_INLINE void SetOpenDelay(float delay) { m_open_delay = delay; }

  void Close();

  void PushState() override;
  void PopState() override;

  void SetCancellable(bool cancellable) override;
  void SetTitle(const std::string_view title) override;
  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

private:
  void Redraw(bool force);

  u64 m_open_time = 0;
  float m_open_delay = 1.0f;
  s32 m_last_progress_percent = -1;
  bool m_on_gpu_thread = false;
  std::string m_image;
  std::string m_title;
};

// Sound effect names.
extern const char* SFX_NAV_ACTIVATE;
extern const char* SFX_NAV_BACK;
extern const char* SFX_NAV_MOVE;

} // namespace FullscreenUI

// Host UI triggers from Big Picture mode.
namespace Host {

#ifndef __ANDROID__

/// Requests settings reset.
void RequestResetSettings(bool system, bool controller);

/// Requests shut down and exit of the hosting application. This may not actually exit,
/// if the user cancels the shutdown confirmation.
void RequestExitApplication(bool allow_confirm);

/// Requests Big Picture mode to be shut down, returning to the desktop interface.
void RequestExitBigPicture();

#endif

} // namespace Host
