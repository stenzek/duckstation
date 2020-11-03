#include "android_progress_callback.h"
#include "android_host_interface.h"
#include "common/log.h"
#include "common/assert.h"
Log_SetChannel(AndroidProgressCallback);

AndroidProgressCallback::AndroidProgressCallback(JNIEnv* env, jobject java_object)
  : m_java_object(java_object)
{
  jclass cls = env->GetObjectClass(java_object);
  m_set_title_method = env->GetMethodID(cls, "setTitle", "(Ljava/lang/String;)V");
  m_set_status_text_method = env->GetMethodID(cls, "setStatusText", "(Ljava/lang/String;)V");
  m_set_progress_range_method = env->GetMethodID(cls, "setProgressRange", "(I)V");
  m_set_progress_value_method = env->GetMethodID(cls, "setProgressValue", "(I)V");
  m_modal_error_method = env->GetMethodID(cls, "modalError", "(Ljava/lang/String;)V");
  m_modal_information_method = env->GetMethodID(cls, "modalInformation", "(Ljava/lang/String;)V");
  m_modal_confirmation_method = env->GetMethodID(cls, "modalConfirmation", "(Ljava/lang/String;)Z");
  Assert(m_set_status_text_method && m_set_progress_range_method && m_set_progress_value_method && m_modal_error_method && m_modal_information_method && m_modal_confirmation_method);
}

AndroidProgressCallback::~AndroidProgressCallback() = default;

bool AndroidProgressCallback::IsCancelled() const
{
  return false;
}

void AndroidProgressCallback::SetCancellable(bool cancellable)
{
  if (m_cancellable == cancellable)
    return;

  BaseProgressCallback::SetCancellable(cancellable);
}

void AndroidProgressCallback::SetTitle(const char* title)
{
  Assert(title);
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jstring text_jstr = env->NewStringUTF(title);
  env->CallVoidMethod(m_java_object, m_set_title_method, text_jstr);
}

void AndroidProgressCallback::SetStatusText(const char* text)
{
  Assert(text);
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jstring text_jstr = env->NewStringUTF(text);
  env->CallVoidMethod(m_java_object, m_set_status_text_method, text_jstr);
}

void AndroidProgressCallback::SetProgressRange(u32 range)
{
  BaseProgressCallback::SetProgressRange(range);

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  env->CallVoidMethod(m_java_object, m_set_progress_range_method, static_cast<jint>(range));
}

void AndroidProgressCallback::SetProgressValue(u32 value)
{
  BaseProgressCallback::SetProgressValue(value);

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  env->CallVoidMethod(m_java_object, m_set_progress_value_method, static_cast<jint>(value));
}

void AndroidProgressCallback::DisplayError(const char* message)
{
  Log_ErrorPrintf("%s", message);
}

void AndroidProgressCallback::DisplayWarning(const char* message)
{
  Log_WarningPrintf("%s", message);
}

void AndroidProgressCallback::DisplayInformation(const char* message)
{
  Log_InfoPrintf("%s", message);
}

void AndroidProgressCallback::DisplayDebugMessage(const char* message)
{
  Log_DevPrintf("%s", message);
}

void AndroidProgressCallback::ModalError(const char* message)
{
  Assert(message);
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jstring message_jstr = env->NewStringUTF(message);
  env->CallVoidMethod(m_java_object, m_modal_error_method, message_jstr);
}

bool AndroidProgressCallback::ModalConfirmation(const char* message)
{
  Assert(message);
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jstring message_jstr = env->NewStringUTF(message);
  return env->CallBooleanMethod(m_java_object, m_modal_confirmation_method, message_jstr);
}

void AndroidProgressCallback::ModalInformation(const char* message)
{
  Assert(message);
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jstring message_jstr = env->NewStringUTF(message);
  env->CallVoidMethod(m_java_object, m_modal_information_method, message_jstr);
}
