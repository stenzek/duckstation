#pragma once
#include "common/progress_callback.h"
#include <QtWidgets/QProgressDialog>

class QtProgressCallback final : public QObject, public BaseProgressCallback
{
  Q_OBJECT

public:
  QtProgressCallback(QWidget* parent_widget);
  ~QtProgressCallback();

  bool IsCancelled() const override;

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
  QProgressDialog m_dialog;
};