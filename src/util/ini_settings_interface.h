// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/settings_interface.h"
#include "common/string_pool.h"

#include <string_view>
#include <vector>

class INISettingsInterface final : public SettingsInterface
{
public:
  struct PoolString
  {
    u32 offset;
    u32 length;
  };

  struct KeyValuePair
  {
    PoolString key;
    PoolString value;
  };
  using KeyValueList = std::vector<KeyValuePair>;

  struct Section
  {
    PoolString name;
    KeyValueList entries;
  };
  using SectionList = std::vector<Section>;

  INISettingsInterface();
  INISettingsInterface(std::string path);
  ~INISettingsInterface() override;

  ALWAYS_INLINE bool IsDirty() const { return m_dirty; }
  ALWAYS_INLINE const std::string& GetPath() const { return m_path; }
  void SetPath(std::string path);

  bool Load(Error* error = nullptr);
  bool Load(std::string new_path, Error* error = nullptr);
  bool LoadFromString(std::string_view data);
  bool Save(Error* error = nullptr);
  std::string SaveToString() const;

  void Clear();
  void ClearPathAndContents();
  void CompactStrings();

  bool IsEmpty() override;

  bool GetIntValue(const char* section, const char* key, s32* value) const override;
  bool GetUIntValue(const char* section, const char* key, u32* value) const override;
  bool GetFloatValue(const char* section, const char* key, float* value) const override;
  bool GetDoubleValue(const char* section, const char* key, double* value) const override;
  bool GetBoolValue(const char* section, const char* key, bool* value) const override;
  bool GetStringValue(const char* section, const char* key, std::string* value) const override;
  bool GetStringValue(const char* section, const char* key, SmallStringBase* value) const override;

  void SetIntValue(const char* section, const char* key, s32 value) override;
  void SetUIntValue(const char* section, const char* key, u32 value) override;
  void SetFloatValue(const char* section, const char* key, float value) override;
  void SetDoubleValue(const char* section, const char* key, double value) override;
  void SetBoolValue(const char* section, const char* key, bool value) override;
  void SetStringValue(const char* section, const char* key, const char* value) override;
  bool ContainsValue(const char* section, const char* key) const override;
  void DeleteValue(const char* section, const char* key) override;
  void ClearSection(const char* section) override;
  void RemoveSection(const char* section) override;
  void RemoveEmptySections() override;

  std::vector<std::string> GetStringList(const char* section, const char* key) const override;
  void SetStringList(const char* section, const char* key, const std::vector<std::string>& items) override;
  bool RemoveFromStringList(const char* section, const char* key, const char* item) override;
  bool AddToStringList(const char* section, const char* key, const char* item) override;

  std::vector<std::pair<std::string, std::string>> GetKeyValueList(const char* section) const override;
  void SetKeyValueList(const char* section, const std::vector<std::pair<std::string, std::string>>& items) override;

  // default parameter overloads
  using SettingsInterface::GetBoolValue;
  using SettingsInterface::GetDoubleValue;
  using SettingsInterface::GetFloatValue;
  using SettingsInterface::GetIntValue;
  using SettingsInterface::GetStringValue;
  using SettingsInterface::GetUIntValue;

private:
  std::string_view GetPoolStringView(const PoolString& ps) const;
  PoolString AddPoolString(std::string_view str);

  SectionList::const_iterator FindSection(std::string_view name) const;
  SectionList::iterator FindSection(std::string_view name);
  Section& GetOrCreateSection(std::string_view name);

  KeyValueList::const_iterator FindKey(const Section& section, std::string_view key) const;
  KeyValueList::iterator FindKey(Section& section, std::string_view key);
  KeyValueList::const_iterator FindKeyEnd(const Section& section, std::string_view key) const;
  KeyValueList::iterator FindKeyEnd(Section& section, std::string_view key);

  void InsertKeyValue(Section& section, std::string_view key, std::string_view value);

  // Returns a pointer to the raw value string for the first match, or nullptr.
  const KeyValuePair* FindFirstKeyValue(std::string_view section, std::string_view key) const;

  std::string m_path;
  BumpUniqueStringPool m_string_pool;
  SectionList m_sections;
  bool m_dirty = false;
};
