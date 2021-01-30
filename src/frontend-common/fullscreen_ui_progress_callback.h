#pragma once
#include "common/progress_callback.h"
#include "common/string.h"

namespace FullscreenUI {

class ProgressCallback final : public BaseProgressCallback
{
public:
  ProgressCallback(String name);
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

private:
  void Redraw(bool force);

  String m_name;
  int m_last_progress_percent = -1;
};

} // namespace FullscreenUI