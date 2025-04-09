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

class GPUSwapChain;
class GPUTexture;

struct GPUSettings;

namespace FullscreenUI {
bool Initialize();
bool IsInitialized();
bool HasActiveWindow();
void CheckForConfigChanges(const GPUSettings& old_settings);
void OnSystemStarting();
void OnSystemPaused();
void OnSystemResumed();
void OnSystemDestroyed();
void OnRunningGameChanged(const std::string& path, const std::string& serial, const std::string& title, GameHash hash);

void Shutdown(bool clear_state);
void Render();
void InvalidateCoverCache();
void TimeToPrintableString(SmallStringBase* str, time_t t);

float GetBackgroundAlpha();

void OpenLoadingScreen(std::string_view image, std::string_view message, s32 progress_min = -1, s32 progress_max = -1,
                       s32 progress_value = -1);
void UpdateLoadingScreen(std::string_view image, std::string_view message, s32 progress_min = -1, s32 progress_max = -1,
                         s32 progress_value = -1);
void CloseLoadingScreen();

void SetTheme();
void UpdateRunIdleState();

static constexpr float SHORT_TRANSITION_TIME = 0.08f;
static constexpr float DEFAULT_TRANSITION_TIME = 0.15f;

enum class TransitionState : u8
{
  Inactive,
  Starting,
  Active,
};

using TransitionStartCallback = std::function<void()>;
void BeginTransition(TransitionStartCallback func, float time = DEFAULT_TRANSITION_TIME);
void BeginTransition(float time, TransitionStartCallback func);
void CancelTransition();
bool IsTransitionActive();
TransitionState GetTransitionState();
GPUTexture* GetTransitionRenderTexture(GPUSwapChain* swap_chain);
void RenderTransitionBlend(GPUSwapChain* swap_chain);

#ifndef __ANDROID__

std::vector<std::string_view> GetThemeNames();
std::span<const char* const> GetThemeConfigNames();

void OpenPauseMenu();
void OpenCheatsMenu();
void OpenDiscChangeMenu();
void OpenAchievementsWindow();
void OpenLeaderboardsWindow();
void ReturnToMainWindow();
void ReturnToPreviousWindow();
void SetStandardSelectionFooterText(bool back_instead_of_cancel);

class BackgroundProgressCallback final : public ProgressCallback
{
public:
  BackgroundProgressCallback(std::string name);
  ~BackgroundProgressCallback() override;

  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void ModalError(const std::string_view message) override;
  bool ModalConfirmation(const std::string_view message) override;
  void ModalInformation(const std::string_view message) override;

  void SetCancelled();

private:
  void Redraw(bool force);

  std::string m_name;
  int m_last_progress_percent = -1;
};

#endif // __ANDROID__

} // namespace FullscreenUI

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

/// Requests settings reset.
void RequestResetSettings(bool system, bool controller);

/// Requests shut down and exit of the hosting application. This may not actually exit,
/// if the user cancels the shutdown confirmation.
void RequestExitApplication(bool allow_confirm);

/// Requests Big Picture mode to be shut down, returning to the desktop interface.
void RequestExitBigPicture();

/// Requests the cover downloader be opened.
void OnCoverDownloaderOpenRequested();

#endif

} // namespace Host
