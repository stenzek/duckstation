#include "android_settings_interface.h"
#include "android_host_interface.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string.h"
#include "common/string_util.h"
#include <algorithm>
Log_SetChannel(AndroidSettingsInterface);

ALWAYS_INLINE TinyString GetSettingKey(const char* section, const char* key)
{
  return TinyString::FromFormat("%s/%s", section, key);
}

AndroidSettingsInterface::AndroidSettingsInterface(jobject java_context)
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jclass c_preference_manager = env->FindClass("androidx/preference/PreferenceManager");
  jclass c_set = env->FindClass("java/util/Set");
  jmethodID m_get_default_shared_preferences =
    env->GetStaticMethodID(c_preference_manager, "getDefaultSharedPreferences",
                           "(Landroid/content/Context;)Landroid/content/SharedPreferences;");
  Assert(c_preference_manager && c_set && m_get_default_shared_preferences);

  m_java_shared_preferences =
    env->CallStaticObjectMethod(c_preference_manager, m_get_default_shared_preferences, java_context);
  Assert(m_java_shared_preferences);
  m_java_shared_preferences = env->NewGlobalRef(m_java_shared_preferences);
  jclass c_shared_preferences = env->GetObjectClass(m_java_shared_preferences);

  m_get_boolean = env->GetMethodID(c_shared_preferences, "getBoolean", "(Ljava/lang/String;Z)Z");
  m_get_int = env->GetMethodID(c_shared_preferences, "getInt", "(Ljava/lang/String;I)I");
  m_get_float = env->GetMethodID(c_shared_preferences, "getFloat", "(Ljava/lang/String;F)F");
  m_get_string =
    env->GetMethodID(c_shared_preferences, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
  m_get_string_set =
    env->GetMethodID(c_shared_preferences, "getStringSet", "(Ljava/lang/String;Ljava/util/Set;)Ljava/util/Set;");
  m_set_to_array = env->GetMethodID(c_set, "toArray", "()[Ljava/lang/Object;");
  Assert(m_get_boolean && m_get_int && m_get_float && m_get_string && m_get_string_set && m_set_to_array);
}

AndroidSettingsInterface::~AndroidSettingsInterface()
{
  if (m_java_shared_preferences)
    AndroidHelpers::GetJNIEnv()->DeleteGlobalRef(m_java_shared_preferences);
}

void AndroidSettingsInterface::Clear()
{
  Log_ErrorPrint("Not implemented");
}

int AndroidSettingsInterface::GetIntValue(const char* section, const char* key, int default_value /*= 0*/)
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();

  // Some of these settings are string lists...
  jstring string_object = reinterpret_cast<jstring>(
    env->CallObjectMethod(m_java_shared_preferences, m_get_string, env->NewStringUTF(GetSettingKey(section, key)),
                          env->NewStringUTF(TinyString::FromFormat("%d", default_value))));
  if (env->ExceptionCheck())
  {
    env->ExceptionClear();

    // it might actually be an int (e.g. seek bar preference)
    const int int_value = static_cast<int>(env->CallIntMethod(m_java_shared_preferences, m_get_int,
                                                              env->NewStringUTF(GetSettingKey(section, key)), default_value));
    if (env->ExceptionCheck())
    {
      env->ExceptionClear();
      return default_value;
    }

    return int_value;
  }

  if (!string_object)
    return default_value;

  const char* data = env->GetStringUTFChars(string_object, nullptr);
  Assert(data != nullptr);

  std::optional<int> value = StringUtil::FromChars<int>(data);
  env->ReleaseStringUTFChars(string_object, data);
  return value.value_or(default_value);
}

float AndroidSettingsInterface::GetFloatValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
#if 0
  return static_cast<float>(env->CallFloatMethod(m_java_shared_preferences, m_get_float,
                                                 env->NewStringUTF(GetSettingKey(section, key)), default_value));
#else
  // Some of these settings are string lists...
  jstring string_object = reinterpret_cast<jstring>(
    env->CallObjectMethod(m_java_shared_preferences, m_get_string, env->NewStringUTF(GetSettingKey(section, key)),
                          env->NewStringUTF(TinyString::FromFormat("%f", default_value))));

  if (env->ExceptionCheck())
  {
    env->ExceptionClear();
    return default_value;
  }

  if (!string_object)
    return default_value;

  const char* data = env->GetStringUTFChars(string_object, nullptr);
  Assert(data != nullptr);

  std::optional<float> value = StringUtil::FromChars<float>(data);
  env->ReleaseStringUTFChars(string_object, data);
  return value.value_or(default_value);
#endif
}

bool AndroidSettingsInterface::GetBoolValue(const char* section, const char* key, bool default_value /*= false*/)
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jboolean bool_value = static_cast<bool>(env->CallBooleanMethod(m_java_shared_preferences, m_get_boolean,
                                                  env->NewStringUTF(GetSettingKey(section, key)), default_value));
  if (env->ExceptionCheck())
  {
    env->ExceptionClear();
    return default_value;
  }

  return bool_value;
}

std::string AndroidSettingsInterface::GetStringValue(const char* section, const char* key,
                                                     const char* default_value /*= ""*/)
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jobject string_object =
    env->CallObjectMethod(m_java_shared_preferences, m_get_string, env->NewStringUTF(GetSettingKey(section, key)),
                          env->NewStringUTF(default_value));

  if (env->ExceptionCheck())
  {
    env->ExceptionClear();
    return default_value;
  }

  if (!string_object)
    return default_value;

  return AndroidHelpers::JStringToString(env, reinterpret_cast<jstring>(string_object));
}

void AndroidSettingsInterface::SetIntValue(const char* section, const char* key, int value)
{
  Log_ErrorPrintf("SetIntValue(\"%s\", \"%s\", %d) not implemented", section, key, value);
}

void AndroidSettingsInterface::SetFloatValue(const char* section, const char* key, float value)
{
  Log_ErrorPrintf("SetFloatValue(\"%s\", \"%s\", %f) not implemented", section, key, value);
}

void AndroidSettingsInterface::SetBoolValue(const char* section, const char* key, bool value)
{
  Log_ErrorPrintf("SetBoolValue(\"%s\", \"%s\", %u) not implemented", section, key, static_cast<unsigned>(value));
}

void AndroidSettingsInterface::SetStringValue(const char* section, const char* key, const char* value)
{
  Log_ErrorPrintf("SetStringValue(\"%s\", \"%s\", \"%s\") not implemented", section, key, value);
}

void AndroidSettingsInterface::DeleteValue(const char* section, const char* key)
{
  Log_ErrorPrintf("DeleteValue(\"%s\", \"%s\") not implemented", section, key);
}

std::vector<std::string> AndroidSettingsInterface::GetStringList(const char* section, const char* key)
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jobject values_set = env->CallObjectMethod(m_java_shared_preferences, m_get_string_set,
                                             env->NewStringUTF(GetSettingKey(section, key)), nullptr);
  if (env->ExceptionCheck())
  {
    env->ExceptionClear();
    return {};
  }

  if (!values_set)
    return {};

  jobjectArray values_array = reinterpret_cast<jobjectArray>(env->CallObjectMethod(values_set, m_set_to_array));
  if (env->ExceptionCheck())
  {
    env->ExceptionClear();
    return {};
  }
  
  if (!values_array)
    return {};

  jsize size = env->GetArrayLength(values_array);
  std::vector<std::string> values;
  values.reserve(size);
  for (jsize i = 0; i < size; i++)
    values.push_back(
      AndroidHelpers::JStringToString(env, reinterpret_cast<jstring>(env->GetObjectArrayElement(values_array, i))));

  return values;
}

void AndroidSettingsInterface::SetStringList(const char* section, const char* key,
                                             const std::vector<std::string>& items)
{
  Log_ErrorPrintf("SetStringList(\"%s\", \"%s\") not implemented", section, key);
}

bool AndroidSettingsInterface::RemoveFromStringList(const char* section, const char* key, const char* item)
{
  Log_ErrorPrintf("RemoveFromStringList(\"%s\", \"%s\", \"%s\") not implemented", section, key, item);
  return false;
}

bool AndroidSettingsInterface::AddToStringList(const char* section, const char* key, const char* item)
{
  Log_ErrorPrintf("AddToStringList(\"%s\", \"%s\", \"%s\") not implemented", section, key, item);
  return false;
}
