#pragma once
#include "common/progress_callback.h"
#include "common/timer.h"
#include <QtWidgets/QProgressDialog>

class QtProgressCallback final : public QObject, public BaseProgressCallback
{
  Q_OBJECT

public:
  QtProgressCallback(QWidget* parent_widget, float show_delay = 0.0f);
  ~QtProgressCallback();

  bool IsCancelled() const override;

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
  void checkForDelayedShow();

  QProgressDialog m_dialog;
  Common::Timer m_show_timer;
  float m_show_delay;
};