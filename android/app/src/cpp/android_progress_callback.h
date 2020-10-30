#pragma once
#include "common/progress_callback.h"
#include <jni.h>

class AndroidProgressCallback final : public BaseProgressCallback
{
public:
  AndroidProgressCallback(JNIEnv* env, jobject java_object);
  ~AndroidProgressCallback();

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
  jobject m_java_object;

  jmethodID m_set_title_method;
  jmethodID m_set_status_text_method;
  jmethodID m_set_progress_range_method;
  jmethodID m_set_progress_value_method;
  jmethodID m_modal_error_method;
  jmethodID m_modal_confirmation_method;
  jmethodID m_modal_information_method;
};
