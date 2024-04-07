// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "layered_settings_interface.h"
#include "common/assert.h"
#include <unordered_set>

LayeredSettingsInterface::LayeredSettingsInterface() = default;

LayeredSettingsInterface::~LayeredSettingsInterface() = default;

bool LayeredSettingsInterface::Save(Error* error /* = nullptr */)
{
  Panic("Attempting to save layered settings interface");
}

void LayeredSettingsInterface::Clear()
{
  Panic("Attempting to clear layered settings interface");
}

bool LayeredSettingsInterface::GetIntValue(const char* section, const char* key, s32* value) const
{
  for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
  {
    if (SettingsInterface* sif = m_layers[layer]; sif != nullptr)
    {
      if (sif->GetIntValue(section, key, value))
        return true;
    }
  }

  return false;
}

bool LayeredSettingsInterface::GetUIntValue(const char* section, const char* key, u32* value) const
{
  for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
  {
    if (SettingsInterface* sif = m_layers[layer]; sif != nullptr)
    {
      if (sif->GetUIntValue(section, key, value))
        return true;
    }
  }

  return false;
}

bool LayeredSettingsInterface::GetFloatValue(const char* section, const char* key, float* value) const
{
  for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
  {
    if (SettingsInterface* sif = m_layers[layer]; sif != nullptr)
    {
      if (sif->GetFloatValue(section, key, value))
        return true;
    }
  }

  return false;
}

bool LayeredSettingsInterface::GetDoubleValue(const char* section, const char* key, double* value) const
{
  for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
  {
    if (SettingsInterface* sif = m_layers[layer]; sif != nullptr)
    {
      if (sif->GetDoubleValue(section, key, value))
        return true;
    }
  }

  return false;
}

bool LayeredSettingsInterface::GetBoolValue(const char* section, const char* key, bool* value) const
{
  for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
  {
    if (SettingsInterface* sif = m_layers[layer]; sif != nullptr)
    {
      if (sif->GetBoolValue(section, key, value))
        return true;
    }
  }

  return false;
}

bool LayeredSettingsInterface::GetStringValue(const char* section, const char* key, std::string* value) const
{
  for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
  {
    if (SettingsInterface* sif = m_layers[layer]; sif != nullptr)
    {
      if (sif->GetStringValue(section, key, value))
        return true;
    }
  }

  return false;
}

bool LayeredSettingsInterface::GetStringValue(const char* section, const char* key, SmallStringBase* value) const
{
  for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
  {
    if (SettingsInterface* sif = m_layers[layer]; sif != nullptr)
    {
      if (sif->GetStringValue(section, key, value))
        return true;
    }
  }

  return false;
}

void LayeredSettingsInterface::SetIntValue(const char* section, const char* key, int value)
{
  Panic("Attempt to call SetIntValue() on layered settings interface");
}

void LayeredSettingsInterface::SetUIntValue(const char* section, const char* key, u32 value)
{
  Panic("Attempt to call SetUIntValue() on layered settings interface");
}

void LayeredSettingsInterface::SetFloatValue(const char* section, const char* key, float value)
{
  Panic("Attempt to call SetFloatValue() on layered settings interface");
}

void LayeredSettingsInterface::SetDoubleValue(const char* section, const char* key, double value)
{
  Panic("Attempt to call SetDoubleValue() on layered settings interface");
}

void LayeredSettingsInterface::SetBoolValue(const char* section, const char* key, bool value)
{
  Panic("Attempt to call SetBoolValue() on layered settings interface");
}

void LayeredSettingsInterface::SetStringValue(const char* section, const char* key, const char* value)
{
  Panic("Attempt to call SetStringValue() on layered settings interface");
}

bool LayeredSettingsInterface::ContainsValue(const char* section, const char* key) const
{
  for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
  {
    if (SettingsInterface* sif = m_layers[layer]; sif != nullptr)
    {
      if (sif->ContainsValue(section, key))
        return true;
    }
  }
  return false;
}

void LayeredSettingsInterface::DeleteValue(const char* section, const char* key)
{
  Panic("Attempt to call DeleteValue() on layered settings interface");
}

void LayeredSettingsInterface::ClearSection(const char* section)
{
  Panic("Attempt to call ClearSection() on layered settings interface");
}

std::vector<std::string> LayeredSettingsInterface::GetStringList(const char* section, const char* key) const
{
  std::vector<std::string> ret;

  for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
  {
    if (SettingsInterface* sif = m_layers[layer]; sif != nullptr)
    {
      ret = sif->GetStringList(section, key);
      if (!ret.empty())
        break;
    }
  }

  return ret;
}

void LayeredSettingsInterface::SetStringList(const char* section, const char* key,
                                             const std::vector<std::string>& items)
{
  Panic("Attempt to call SetStringList() on layered settings interface");
}

bool LayeredSettingsInterface::RemoveFromStringList(const char* section, const char* key, const char* item)
{
  Panic("Attempt to call RemoveFromStringList() on layered settings interface");
}

bool LayeredSettingsInterface::AddToStringList(const char* section, const char* key, const char* item)
{
  Panic("Attempt to call AddToStringList() on layered settings interface");
}

std::vector<std::pair<std::string, std::string>> LayeredSettingsInterface::GetKeyValueList(const char* section) const
{
  std::unordered_set<std::string_view> seen;
  std::vector<std::pair<std::string, std::string>> ret;
  for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
  {
    if (SettingsInterface* sif = m_layers[layer])
    {
      const size_t newly_added_begin = ret.size();
      std::vector<std::pair<std::string, std::string>> entries = sif->GetKeyValueList(section);
      for (std::pair<std::string, std::string>& entry : entries)
      {
        if (seen.find(entry.first) != seen.end())
          continue;
        ret.push_back(std::move(entry));
      }

      // Mark keys as seen after processing all entries in case the layer has multiple entries for a specific key
      for (auto cur = ret.begin() + newly_added_begin, end = ret.end(); cur < end; cur++)
        seen.insert(cur->first);
    }
  }

  return ret;
}

void LayeredSettingsInterface::SetKeyValueList(const char* section,
                                               const std::vector<std::pair<std::string, std::string>>& items)
{
  Panic("Attempt to call SetKeyValueList() on layered settings interface");
}
