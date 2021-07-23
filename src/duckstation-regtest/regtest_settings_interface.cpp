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

int RegTestSettingsInterface::GetIntValue(const char* section, const char* key, int default_value /*= 0*/)
{
  int retval = default_value;

  const std::string fullkey(GetFullKey(section, key));
  auto iter = m_keys.find(fullkey);
  if (iter != m_keys.end())
    retval = StringUtil::FromChars<int>(iter->second, 10).value_or(default_value);

  Log_DevPrintf("GetIntValue(%s) -> %d", fullkey.c_str(), retval);
  return retval;
}

float RegTestSettingsInterface::GetFloatValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  float retval = default_value;

  const std::string fullkey(GetFullKey(section, key));
  auto iter = m_keys.find(fullkey);
  if (iter != m_keys.end())
    retval = StringUtil::FromChars<float>(iter->second).value_or(default_value);

  Log_DevPrintf("GetFloatValue(%s) -> %f", fullkey.c_str(), retval);
  return retval;
}

bool RegTestSettingsInterface::GetBoolValue(const char* section, const char* key, bool default_value /*= false*/)
{
  bool retval = default_value;

  const std::string fullkey(GetFullKey(section, key));
  auto iter = m_keys.find(fullkey);
  if (iter != m_keys.end())
    retval = StringUtil::FromChars<bool>(iter->second).value_or(default_value);

  Log_DevPrintf("GetBoolValue(%s) -> %s", fullkey.c_str(), retval ? "true" : "false");
  return retval;
}

std::string RegTestSettingsInterface::GetStringValue(const char* section, const char* key,
                                                     const char* default_value /*= ""*/)
{
  std::string retval;

  const std::string fullkey(GetFullKey(section, key));
  auto iter = m_keys.find(fullkey);
  if (iter != m_keys.end())
    retval = iter->second;
  else
    retval = default_value;

  Log_DevPrintf("GetStringValue(%s) -> %s", fullkey.c_str(), retval.c_str());
  return retval;
}

void RegTestSettingsInterface::SetIntValue(const char* section, const char* key, int value)
{
  const std::string fullkey(GetFullKey(section, key));
  Log_DevPrintf("SetIntValue(%s, %d)", fullkey.c_str(), value);
  m_keys[std::move(fullkey)] = std::to_string(value);
}

void RegTestSettingsInterface::SetFloatValue(const char* section, const char* key, float value)
{
  const std::string fullkey(GetFullKey(section, key));
  Log_DevPrintf("SetFloatValue(%s, %f)", fullkey.c_str(), value);
  m_keys[std::move(fullkey)] = std::to_string(value);
}

void RegTestSettingsInterface::SetBoolValue(const char* section, const char* key, bool value)
{
  const std::string fullkey(GetFullKey(section, key));
  Log_DevPrintf("SetBoolValue(%s, %s)", fullkey.c_str(), value ? "true" : "false");
  m_keys[std::move(fullkey)] = std::string(value ? "true" : "false");
}

void RegTestSettingsInterface::SetStringValue(const char* section, const char* key, const char* value)
{
  const std::string fullkey(GetFullKey(section, key));
  Log_DevPrintf("SetStringValue(%s, %s)", fullkey.c_str(), value);
  m_keys[std::move(fullkey)] = value;
}

std::vector<std::string> RegTestSettingsInterface::GetStringList(const char* section, const char* key)
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

void RegTestSettingsInterface::DeleteValue(const char* section, const char* key)
{
  const std::string fullkey(GetFullKey(section, key));
  Log_DevPrintf("DeleteValue(%s)", fullkey.c_str());

  auto iter = m_keys.find(fullkey);
  if (iter != m_keys.end())
    m_keys.erase(iter);
}

void RegTestSettingsInterface::ClearSection(const char* section)
{
  Log_DevPrintf("ClearSection(%s)", section);

  const std::string start(StringUtil::StdStringFromFormat("%s/", section));
  for (auto iter = m_keys.begin(); iter != m_keys.end();)
  {
    if (StringUtil::StartsWith(iter->first, start.c_str()))
      iter = m_keys.erase(iter);
    else
      ++iter;
  }
}
