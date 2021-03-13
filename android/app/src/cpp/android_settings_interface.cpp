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
  jclass c_preference_editor = env->FindClass("android/content/SharedPreferences$Editor");
  jclass c_set = env->FindClass("java/util/Set");
  jclass c_helper = env->FindClass("com/github/stenzek/duckstation/PreferenceHelpers");
  jmethodID m_get_default_shared_preferences =
    env->GetStaticMethodID(c_preference_manager, "getDefaultSharedPreferences",
                           "(Landroid/content/Context;)Landroid/content/SharedPreferences;");
  Assert(c_preference_manager && c_preference_editor && c_set && c_helper && m_get_default_shared_preferences);
  m_set_class = reinterpret_cast<jclass>(env->NewGlobalRef(c_set));
  m_shared_preferences_editor_class = reinterpret_cast<jclass>(env->NewGlobalRef(c_preference_editor));
  m_helper_class = reinterpret_cast<jclass>(env->NewGlobalRef(c_helper));
  Assert(m_set_class && m_shared_preferences_editor_class && m_helper_class);

  env->DeleteLocalRef(c_set);
  env->DeleteLocalRef(c_preference_editor);
  env->DeleteLocalRef(c_helper);

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

  m_edit = env->GetMethodID(m_shared_preferences_class, "edit", "()Landroid/content/SharedPreferences$Editor;");
  m_edit_set_string =
    env->GetMethodID(m_shared_preferences_editor_class, "putString",
                     "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;");
  m_edit_commit = env->GetMethodID(m_shared_preferences_editor_class, "commit", "()Z");
  m_edit_remove = env->GetMethodID(m_shared_preferences_editor_class, "remove",
                                   "(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;");
  Assert(m_edit && m_edit_set_string && m_edit_commit && m_edit_remove);

  m_helper_clear_section =
    env->GetStaticMethodID(m_helper_class, "clearSection", "(Landroid/content/SharedPreferences;Ljava/lang/String;)V");
  m_helper_add_to_string_list = env->GetStaticMethodID(
    m_helper_class, "addToStringList", "(Landroid/content/SharedPreferences;Ljava/lang/String;Ljava/lang/String;)Z");
  m_helper_remove_from_string_list =
    env->GetStaticMethodID(m_helper_class, "removeFromStringList",
                           "(Landroid/content/SharedPreferences;Ljava/lang/String;Ljava/lang/String;)Z");
  m_helper_set_string_list = env->GetStaticMethodID(
    m_helper_class, "setStringList", "(Landroid/content/SharedPreferences;Ljava/lang/String;[Ljava/lang/String;)V");
  Assert(m_helper_clear_section && m_helper_add_to_string_list && m_helper_remove_from_string_list &&
         m_helper_set_string_list);
}

AndroidSettingsInterface::~AndroidSettingsInterface()
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  if (m_java_shared_preferences)
    env->DeleteGlobalRef(m_java_shared_preferences);
  if (m_shared_preferences_editor_class)
    env->DeleteGlobalRef(m_shared_preferences_editor_class);
  if (m_shared_preferences_class)
    env->DeleteGlobalRef(m_shared_preferences_class);
  if (m_set_class)
    env->DeleteGlobalRef(m_set_class);
  if (m_helper_class)
    env->DeleteGlobalRef(m_helper_class);
}

bool AndroidSettingsInterface::Save()
{
  return true;
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

jobject AndroidSettingsInterface::GetPreferencesEditor(JNIEnv* env)
{
  return env->CallObjectMethod(m_java_shared_preferences, m_edit);
}

void AndroidSettingsInterface::CheckForException(JNIEnv* env, const char* task)
{
  if (!env->ExceptionCheck())
    return;

  Log_ErrorPrintf("JNI exception during %s", task);
  env->ExceptionClear();
}

void AndroidSettingsInterface::SetIntValue(const char* section, const char* key, int value)
{
  Log_DevPrintf("SetIntValue(\"%s\", \"%s\", %d)", section, key, value);

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jobject> editor(env, GetPreferencesEditor(env));
  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  LocalRefHolder<jstring> str_value(env, env->NewStringUTF(TinyString::FromFormat("%d", value)));

  LocalRefHolder<jobject> dummy(env,
                                env->CallObjectMethod(editor, m_edit_set_string, key_string.Get(), str_value.Get()));
  env->CallBooleanMethod(editor, m_edit_commit);

  CheckForException(env, "SetIntValue");
}

void AndroidSettingsInterface::SetFloatValue(const char* section, const char* key, float value)
{
  Log_DevPrintf("SetFloatValue(\"%s\", \"%s\", %f)", section, key, value);

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jobject> editor(env, GetPreferencesEditor(env));
  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  LocalRefHolder<jstring> str_value(env, env->NewStringUTF(TinyString::FromFormat("%f", value)));

  LocalRefHolder<jobject> dummy(env,
                                env->CallObjectMethod(editor, m_edit_set_string, key_string.Get(), str_value.Get()));
  env->CallBooleanMethod(editor, m_edit_commit);

  CheckForException(env, "SetFloatValue");
}

void AndroidSettingsInterface::SetBoolValue(const char* section, const char* key, bool value)
{
  Log_DevPrintf("SetBoolValue(\"%s\", \"%s\", %u)", section, key, static_cast<unsigned>(value));

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jobject> editor(env, GetPreferencesEditor(env));
  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  LocalRefHolder<jstring> str_value(env, env->NewStringUTF(value ? "true" : "false"));

  LocalRefHolder<jobject> dummy(env,
                                env->CallObjectMethod(editor, m_edit_set_string, key_string.Get(), str_value.Get()));
  env->CallBooleanMethod(editor, m_edit_commit);

  CheckForException(env, "SetBoolValue");
}

void AndroidSettingsInterface::SetStringValue(const char* section, const char* key, const char* value)
{
  Log_DevPrintf("SetStringValue(\"%s\", \"%s\", \"%s\")", section, key, value);

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jobject> editor(env, GetPreferencesEditor(env));
  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  LocalRefHolder<jstring> str_value(env, env->NewStringUTF(value));

  LocalRefHolder<jobject> dummy(env,
                                env->CallObjectMethod(editor, m_edit_set_string, key_string.Get(), str_value.Get()));
  env->CallBooleanMethod(editor, m_edit_commit);

  CheckForException(env, "SetStringValue");
}

void AndroidSettingsInterface::DeleteValue(const char* section, const char* key)
{
  Log_DevPrintf("DeleteValue(\"%s\", \"%s\")", section, key);

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jobject> editor(env, GetPreferencesEditor(env));
  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  LocalRefHolder<jobject> dummy(env, env->CallObjectMethod(editor, m_edit_remove, key_string.Get()));
  env->CallBooleanMethod(editor, m_edit_commit);

  CheckForException(env, "DeleteValue");
}

void AndroidSettingsInterface::ClearSection(const char* section)
{
  Log_DevPrintf("ClearSection(\"%s\")", section);

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jstring> str_section(env, env->NewStringUTF(section));
  env->CallStaticVoidMethod(m_helper_class, m_helper_clear_section, m_java_shared_preferences, str_section.Get());

  CheckForException(env, "ClearSection");
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

    // this might just be a string, not a string set
    LocalRefHolder<jstring> string_object(env, reinterpret_cast<jstring>(env->CallObjectMethod(
                                                 m_java_shared_preferences, m_get_string, key_string.Get(), nullptr)));

    if (!env->ExceptionCheck())
    {
      std::vector<std::string> ret;
      if (string_object)
        ret.push_back(AndroidHelpers::JStringToString(env, string_object));

      return ret;
    }

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
  Log_DevPrintf("SetStringList(\"%s\", \"%s\")", section, key);
  if (items.empty())
  {
    DeleteValue(section, key);
    return;
  }

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jobjectArray> items_array(
    env, env->NewObjectArray(static_cast<jsize>(items.size()), AndroidHelpers::GetStringClass(), nullptr));
  for (size_t i = 0; i < items.size(); i++)
  {
    LocalRefHolder<jstring> item_jstr(env, env->NewStringUTF(items[i].c_str()));
    env->SetObjectArrayElement(items_array, static_cast<jsize>(i), item_jstr);
  }

  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  env->CallStaticVoidMethod(m_helper_class, m_helper_set_string_list, m_java_shared_preferences, key_string.Get(),
                            items_array.Get());

  CheckForException(env, "SetStringList");
}

bool AndroidSettingsInterface::RemoveFromStringList(const char* section, const char* key, const char* item)
{
  Log_DevPrintf("RemoveFromStringList(\"%s\", \"%s\", \"%s\")", section, key, item);

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  LocalRefHolder<jstring> item_string(env, env->NewStringUTF(item));
  const bool result = env->CallStaticBooleanMethod(m_helper_class, m_helper_remove_from_string_list,
                                                   m_java_shared_preferences, key_string.Get(), item_string.Get());
  CheckForException(env, "RemoveFromStringList");
  return result;
}

bool AndroidSettingsInterface::AddToStringList(const char* section, const char* key, const char* item)
{
  Log_DevPrintf("AddToStringList(\"%s\", \"%s\", \"%s\")", section, key, item);

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  LocalRefHolder<jstring> key_string(env, env->NewStringUTF(GetSettingKey(section, key)));
  LocalRefHolder<jstring> item_string(env, env->NewStringUTF(item));
  const bool result = env->CallStaticBooleanMethod(m_helper_class, m_helper_add_to_string_list,
                                                   m_java_shared_preferences, key_string.Get(), item_string.Get());
  CheckForException(env, "AddToStringList");
  return result;
}
