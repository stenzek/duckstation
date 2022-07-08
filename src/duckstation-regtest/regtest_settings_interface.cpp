#include "regtest_settings_interface.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
Log_SetChannel(RegTestSettingsInterface);

RegTestSettingsInterface::RegTestSettingsInterface() = default;

RegTestSettingsInterface::~RegTestSettingsInterface() = default;

bool RegTestSettingsInterface::Save()
{
  return false;
}

void RegTestSettingsInterface::Clear()
{
  m_keys.clear();
}

static std::string GetFullKey(const char* section, const char* key)
{
  return StringUtil::StdStringFromFormat("%s/%s", section, key);
}

bool RegTestSettingsInterface::GetIntValue(const char* section, const char* key, s32* value) const
{
  const std::string fullkey(GetFullKey(section, key));
  auto iter = m_keys.find(fullkey);
  if (iter == m_keys.end())
    return false;

  std::optional<s32> parsed = StringUtil::FromChars<s32>(iter->second, 10);
  if (!parsed.has_value())
    return false;

  *value = parsed.value();
  return true;
}

bool RegTestSettingsInterface::GetUIntValue(const char* section, const char* key, u32* value) const
{
  const std::string fullkey(GetFullKey(section, key));
  auto iter = m_keys.find(fullkey);
  if (iter == m_keys.end())
    return false;

  std::optional<u32> parsed = StringUtil::FromChars<u32>(iter->second, 10);
  if (!parsed.has_value())
    return false;

  *value = parsed.value();
  return true;
}

bool RegTestSettingsInterface::GetFloatValue(const char* section, const char* key, float* value) const
{
  const std::string fullkey(GetFullKey(section, key));
  auto iter = m_keys.find(fullkey);
  if (iter == m_keys.end())
    return false;

  std::optional<float> parsed = StringUtil::FromChars<float>(iter->second);
  if (!parsed.has_value())
    return false;

  *value = parsed.value();
  return true;
}

bool RegTestSettingsInterface::GetDoubleValue(const char* section, const char* key, double* value) const
{
  const std::string fullkey(GetFullKey(section, key));
  auto iter = m_keys.find(fullkey);
  if (iter == m_keys.end())
    return false;

  std::optional<double> parsed = StringUtil::FromChars<double>(iter->second);
  if (!parsed.has_value())
    return false;

  *value = parsed.value();
  return true;
}

bool RegTestSettingsInterface::GetBoolValue(const char* section, const char* key, bool* value) const
{
  const std::string fullkey(GetFullKey(section, key));
  auto iter = m_keys.find(fullkey);
  if (iter == m_keys.end())
    return false;

  std::optional<bool> parsed = StringUtil::FromChars<bool>(iter->second);
  if (!parsed.has_value())
    return false;

  *value = parsed.value();
  return true;
}

bool RegTestSettingsInterface::GetStringValue(const char* section, const char* key, std::string* value) const
{
  const std::string fullkey(GetFullKey(section, key));
  auto iter = m_keys.find(fullkey);
  if (iter == m_keys.end())
    return false;

  *value = iter->second;
  return true;
}

void RegTestSettingsInterface::SetIntValue(const char* section, const char* key, s32 value)
{
  const std::string fullkey(GetFullKey(section, key));
  m_keys[std::move(fullkey)] = std::to_string(value);
}

void RegTestSettingsInterface::SetUIntValue(const char* section, const char* key, u32 value)
{
  const std::string fullkey(GetFullKey(section, key));
  m_keys[std::move(fullkey)] = std::to_string(value);
}

void RegTestSettingsInterface::SetFloatValue(const char* section, const char* key, float value)
{
  const std::string fullkey(GetFullKey(section, key));
  m_keys[std::move(fullkey)] = std::to_string(value);
}

void RegTestSettingsInterface::SetDoubleValue(const char* section, const char* key, double value)
{
  const std::string fullkey(GetFullKey(section, key));
  m_keys[std::move(fullkey)] = std::to_string(value);
}

void RegTestSettingsInterface::SetBoolValue(const char* section, const char* key, bool value)
{
  const std::string fullkey(GetFullKey(section, key));
  m_keys[std::move(fullkey)] = std::string(value ? "true" : "false");
}

void RegTestSettingsInterface::SetStringValue(const char* section, const char* key, const char* value)
{
  const std::string fullkey(GetFullKey(section, key));
  m_keys[std::move(fullkey)] = value;
}

std::vector<std::string> RegTestSettingsInterface::GetStringList(const char* section, const char* key) const
{
  std::vector<std::string> ret;
  Panic("Not implemented");
  return ret;
}

void RegTestSettingsInterface::SetStringList(const char* section, const char* key,
                                             const std::vector<std::string>& items)
{
  Panic("Not implemented");
}

bool RegTestSettingsInterface::RemoveFromStringList(const char* section, const char* key, const char* item)
{
  Panic("Not implemented");
  return false;
}

bool RegTestSettingsInterface::AddToStringList(const char* section, const char* key, const char* item)
{
  Panic("Not implemented");
  return false;
}

bool RegTestSettingsInterface::ContainsValue(const char* section, const char* key) const
{
  const std::string fullkey(GetFullKey(section, key));
  return (m_keys.find(fullkey) != m_keys.end());
}

void RegTestSettingsInterface::DeleteValue(const char* section, const char* key)
{
  const std::string fullkey(GetFullKey(section, key));

  auto iter = m_keys.find(fullkey);
  if (iter != m_keys.end())
    m_keys.erase(iter);
}

void RegTestSettingsInterface::ClearSection(const char* section)
{
  const std::string start(StringUtil::StdStringFromFormat("%s/", section));
  for (auto iter = m_keys.begin(); iter != m_keys.end();)
  {
    if (StringUtil::StartsWith(iter->first, start.c_str()))
      iter = m_keys.erase(iter);
    else
      ++iter;
  }
}
