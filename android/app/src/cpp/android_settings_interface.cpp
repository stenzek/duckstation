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
  m_set_class = reinterpret_cast<jclass>(env->NewGlobalRef(c_set));
  Assert(m_set_class);
  env->DeleteLocalRef(c_set);

  jobject shared_preferences =
    env->CallStaticObjectMethod(c_preference_manager, m_get_default_shared_preferences, java_context);
  Assert(shared_preferences);
  m_java_shared_preferences = env->NewGlobalRef(shared_preferences);
  Assert(m_java_shared_preferences);
  env->DeleteLocalRef(c_preference_manager);
  env->DeleteLocalRef(shared_preferences);

  jclass c_shared_preferences = env->GetObjectClass(m_java_shared_preferences);
  m_shared_preferences_class = reinterpret_cast<jclass>(env->NewGlobalRef(c_shared_preferences));
  Assert(m_shared_preferences_class);
  env->DeleteLocalRef(c_shared_preferences);

  m_get_boolean = env->GetMethodID(m_shared_preferences_class, "getBoolean", "(Ljava/lang/String;Z)Z");
  m_get_int = env->GetMethodID(m_shared_preferences_class, "getInt", "(Ljava/lang/String;I)I");
  m_get_float = env->GetMethodID(m_shared_preferences_class, "getFloat", "(Ljava/lang/String;F)F");
  m_get_string = env->GetMethodID(m_shared_preferences_class, "getString",
                                  "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
  m_get_string_set =
    env->GetMethodID(m_shared_preferences_class, "getStringSet", "(Ljava/lang/String;Ljava/util/Set;)Ljava/util/Set;");
  m_set_to_array = env->GetMethodID(m_set_class, "toArray", "()[Ljava/lang/Object;");
  Assert(m_get_boolean && m_get_int && m_get_float && m_get_string && m_get_string_set && m_set_to_array);
}

AndroidSettingsInterface::~AndroidSettingsInterface()
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  if (m_java_shared_preferences)
    env->DeleteGlobalRef(m_java_shared_preferences);
  if (m_shared_preferences_class)
    env->DeleteGlobalRef(m_shared_preferences_class);
  if (m_set_class)
    env->DeleteGlobalRef(m_set_class);
}

void AndroidSettingsInterface::Clear()
{
  Log_ErrorPrint("Not implemented");
}

int AndroidSettingsInterface::GetIntValue(const char* section, const char* key, int default_value /*= 0*/)
{
  // Some of these settings are string lists...
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  LocalRefHolder<jstring> default_value_string(env, env->NewStringUTF(TinyString::FromFormat("%d", default_value)));
  LocalRefHolder<jstring> string_object(
    env, reinterpret_cast<jstring>(env->CallObjectMethod(m_java_shared_preferences, m_get_string, key_string.Get(),
                                                         default_value_string.Get())));
  if (env->ExceptionCheck())
  {
    env->ExceptionClear();

    // it might actually be an int (e.g. seek bar preference)
    const int int_value =
      static_cast<int>(env->CallIntMethod(m_java_shared_preferences, m_get_int, key_string.Get(), default_value));
    if (env->ExceptionCheck())
    {
      env->ExceptionClear();
      Log_DevPrintf("GetIntValue(%s, %s) -> %d (exception)", section, key, default_value);
      return default_value;
    }

    Log_DevPrintf("GetIntValue(%s, %s) -> %d (int)", section, key, int_value);
    return int_value;
  }

  if (!string_object)
    return default_value;

  const char* data = env->GetStringUTFChars(string_object, nullptr);
  Assert(data != nullptr);
  Log_DevPrintf("GetIntValue(%s, %s) -> %s", section, key, data);

  std::optional<int> value = StringUtil::FromChars<int>(data);
  env->ReleaseStringUTFChars(string_object, data);
  return value.value_or(default_value);
}

float AndroidSettingsInterface::GetFloatValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  LocalRefHolder<jstring> default_value_string(env, env->NewStringUTF(TinyString::FromFormat("%f", default_value)));
  LocalRefHolder<jstring> string_object(
    env, reinterpret_cast<jstring>(env->CallObjectMethod(m_java_shared_preferences, m_get_string, key_string.Get(),
                                                         default_value_string.Get())));
  if (env->ExceptionCheck())
  {
    env->ExceptionClear();
    Log_DevPrintf("GetFloatValue(%s, %s) -> %f (exception)", section, key, default_value);
    return default_value;
  }

  if (!string_object)
  {
    Log_DevPrintf("GetFloatValue(%s, %s) -> %f (null)", section, key, default_value);
    return default_value;
  }

  const char* data = env->GetStringUTFChars(string_object, nullptr);
  Assert(data != nullptr);
  Log_DevPrintf("GetFloatValue(%s, %s) -> %s", section, key, data);

  std::optional<float> value = StringUtil::FromChars<float>(data);
  env->ReleaseStringUTFChars(string_object, data);
  return value.value_or(default_value);
}

bool AndroidSettingsInterface::GetBoolValue(const char* section, const char* key, bool default_value /*= false*/)
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  jboolean bool_value = static_cast<bool>(
    env->CallBooleanMethod(m_java_shared_preferences, m_get_boolean, key_string.Get(), default_value));
  if (env->ExceptionCheck())
  {
    Log_DevPrintf("GetBoolValue(%s, %s) -> %u (exception)", section, key, static_cast<unsigned>(default_value));
    env->ExceptionClear();
    return default_value;
  }

  Log_DevPrintf("GetBoolValue(%s, %s) -> %u", section, key, static_cast<unsigned>(bool_value));
  return bool_value;
}

std::string AndroidSettingsInterface::GetStringValue(const char* section, const char* key,
                                                     const char* default_value /*= ""*/)
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  LocalRefHolder<jstring> default_value_string(env, env->NewStringUTF(default_value));
  LocalRefHolder<jstring> string_object(
    env, reinterpret_cast<jstring>(env->CallObjectMethod(m_java_shared_preferences, m_get_string, key_string.Get(),
                                                         default_value_string.Get())));

  if (env->ExceptionCheck())
  {
    env->ExceptionClear();
    Log_DevPrintf("GetStringValue(%s, %s) -> %s (exception)", section, key, default_value);
    return default_value;
  }

  if (!string_object)
  {
    Log_DevPrintf("GetStringValue(%s, %s) -> %s (null)", section, key, default_value);
    return default_value;
  }

  const std::string ret(AndroidHelpers::JStringToString(env, string_object));
  Log_DevPrintf("GetStringValue(%s, %s) -> %s", section, key, ret.c_str());
  return ret;
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
  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  LocalRefHolder<jobject> values_set(
    env, env->CallObjectMethod(m_java_shared_preferences, m_get_string_set, key_string.Get(), nullptr));
  if (env->ExceptionCheck())
  {
    env->ExceptionClear();
    return {};
  }

  if (!values_set)
    return {};

  LocalRefHolder<jobjectArray> values_array(
    env, reinterpret_cast<jobjectArray>(env->CallObjectMethod(values_set, m_set_to_array)));
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
  {
    jstring str = reinterpret_cast<jstring>(env->GetObjectArrayElement(values_array, i));
    values.push_back(AndroidHelpers::JStringToString(env, str));
    env->DeleteLocalRef(str);
  }

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
