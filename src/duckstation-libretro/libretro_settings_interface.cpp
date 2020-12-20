#include "libretro_settings_interface.h"
#include "common/log.h"
#include "common/string_util.h"
#include "libretro_host_interface.h"
#include <type_traits>
Log_SetChannel(LibretroSettingsInterface);

template<typename T, typename DefaultValueType>
static T GetVariable(const char* section, const char* key, DefaultValueType default_value)
{

  TinyString full_key;
  full_key.Format("duckstation_%s.%s", section, key);

  retro_variable rv = {full_key.GetCharArray(), nullptr};
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &rv) || !rv.value)
    return T(default_value);

  if constexpr (std::is_same_v<T, std::string>)
  {
    return T(rv.value);
  }
  else if constexpr (std::is_same_v<T, bool>)
  {
    return (StringUtil::Strcasecmp(rv.value, "true") == 0 || StringUtil::Strcasecmp(rv.value, "1") == 0);
  }
  else if constexpr (std::is_same_v<T, float>)
  {
    return std::strtof(rv.value, nullptr);
  }
  else
  {
    std::optional<T> parsed = StringUtil::FromChars<T>(rv.value);
    if (!parsed.has_value())
      return T(default_value);

    return parsed.value();
  }
}

void LibretroSettingsInterface::Clear()
{
  Log_WarningPrintf("Clear not implemented");
}

int LibretroSettingsInterface::GetIntValue(const char* section, const char* key, int default_value /*= 0*/)
{
  return GetVariable<int>(section, key, default_value);
}

float LibretroSettingsInterface::GetFloatValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  return GetVariable<float>(section, key, default_value);
}

bool LibretroSettingsInterface::GetBoolValue(const char* section, const char* key, bool default_value /*= false*/)
{
  return GetVariable<bool>(section, key, default_value);
}

std::string LibretroSettingsInterface::GetStringValue(const char* section, const char* key,
                                                      const char* default_value /*= ""*/)
{
  return GetVariable<std::string>(section, key, default_value);
}

void LibretroSettingsInterface::SetIntValue(const char* section, const char* key, int value)
{
  Log_ErrorPrintf("SetIntValue(\"%s\", \"%s\", %d) not implemented", section, key, value);
}

void LibretroSettingsInterface::SetFloatValue(const char* section, const char* key, float value)
{
  Log_ErrorPrintf("SetFloatValue(\"%s\", \"%s\", %f) not implemented", section, key, value);
}

void LibretroSettingsInterface::SetBoolValue(const char* section, const char* key, bool value)
{
  Log_ErrorPrintf("SetBoolValue(\"%s\", \"%s\", %u) not implemented", section, key, static_cast<unsigned>(value));
}

void LibretroSettingsInterface::SetStringValue(const char* section, const char* key, const char* value)
{
  Log_ErrorPrintf("SetStringValue(\"%s\", \"%s\", \"%s\") not implemented", section, key, value);
}

std::vector<std::string> LibretroSettingsInterface::GetStringList(const char* section, const char* key)
{
  std::string value = GetVariable<std::string>(section, key, "");
  if (value.empty())
    return {};

  return std::vector<std::string>({std::move(value)});
}

void LibretroSettingsInterface::SetStringList(const char* section, const char* key,
                                              const std::vector<std::string>& items)
{
  Log_ErrorPrintf("SetStringList(\"%s\", \"%s\") not implemented", section, key);
}

bool LibretroSettingsInterface::RemoveFromStringList(const char* section, const char* key, const char* item)
{
  Log_ErrorPrintf("RemoveFromStringList(\"%s\", \"%s\", \"%s\") not implemented", section, key, item);
  return false;
}

bool LibretroSettingsInterface::AddToStringList(const char* section, const char* key, const char* item)
{
  Log_ErrorPrintf("AddToStringList(\"%s\", \"%s\", \"%s\") not implemented", section, key, item);
  return false;
}

void LibretroSettingsInterface::DeleteValue(const char* section, const char* key)
{
  Log_ErrorPrintf("DeleteValue(\"%s\", \"%s\") not implemented", section, key);
}

void LibretroSettingsInterface::ClearSection(const char* section)
{
  Log_ErrorPrintf("ClearSection(\"%s\") not implemented", section);
}
