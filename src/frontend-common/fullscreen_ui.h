#pragma once
#include "common/progress_callback.h"
#include "common/types.h"
#include <memory>
#include <string>

class HostDisplayTexture;

namespace FullscreenUI {
bool Initialize();
bool IsInitialized();
bool HasActiveWindow();
void OnSystemStarted();
void OnSystemPaused();
void OnSystemResumed();
void OnSystemDestroyed();
void OnRunningGameChanged();
void OpenPauseMenu();
bool OpenAchievementsWindow();
bool OpenLeaderboardsWindow();

void Shutdown();
void Render();

// Returns true if the message has been dismissed.
bool DrawErrorWindow(const char* message);
bool DrawConfirmWindow(const char* message, bool* result);

class ProgressCallback final : public BaseProgressCallback
{
public:
  ProgressCallback(std::string name);
  ~ProgressCallback() override;

  void PushState() override;
  void PopState() override;

  void SetCancellable(bool cancellable) override;
  void SetTitle(const char* title) override;
  void SetStatusText(const char* text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void DisplayError(const char* message) override;
  void DisplayWarning(const char* message) override;
  void DisplayInformation(const char* message) override;
  void DisplayDebugMessage(const char* message) override;

  void ModalError(const char* message) override;
  bool ModalConfirmation(const char* message) override;
  void ModalInformation(const char* message) override;

  void SetCancelled();

private:
  void Redraw(bool force);

  std::string m_name;
  int m_last_progress_percent = -1;
};
} // namespace FullscreenUI
