// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "layered_settings_interface.h"
#include "common/assert.h"
#include <unordered_set>

LayeredSettingsInterface::LayeredSettingsInterface() = default;

LayeredSettingsInterface::~LayeredSettingsInterface() = default;

bool LayeredSettingsInterface::IsEmpty()
{
  return false;
}

bool LayeredSettingsInterface::LookupValue(const char* section, const char* key, std::string_view* value) const
{
  for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
  {
    if (SettingsInterface* sif = m_layers[layer]; sif != nullptr)
    {
      if (sif->LookupValue(section, key, value))
        return true;
    }
  }

  return false;
}

void LayeredSettingsInterface::StoreValue(const char* section, const char* key, std::string_view value)
{
  Panic("Attempt to call StoreValue() on layered settings interface");
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

void LayeredSettingsInterface::RemoveSection(const char* section)
{
  Panic("Attempt to call RemoveSection() on layered settings interface");
}

void LayeredSettingsInterface::RemoveEmptySections()
{
  Panic("Attempt to call RemoveEmptySections() on layered settings interface");
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
