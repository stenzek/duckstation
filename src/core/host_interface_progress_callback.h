#pragma once
#include "common/progress_callback.h"
#include "host_interface.h"

class HostInterfaceProgressCallback : public BaseProgressCallback
{
public:
  HostInterfaceProgressCallback(HostInterface* intf);

  void PushState() override;
  void PopState() override;

  void SetCancellable(bool cancellable) override;
  void SetStatusText(const char* text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void DisplayError(const char* message) override;
  void DisplayWarning(const char* message) override;
  void DisplayInformation(const char* message) override;
  void DisplayDebugMessage(const char* message) override;

  void ModalError(const char* message) override;
  bool ModalConfirmation(const char* message) override;
  u32 ModalPrompt(const char* message, u32 num_options, ...) override;
  
private:
  void Redraw(bool force);

  HostInterface* m_host_interface;
  int m_last_progress_percent = -1;
};
