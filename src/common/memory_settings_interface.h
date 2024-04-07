// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "heterogeneous_containers.h"
#include "settings_interface.h"
#include <string>

class MemorySettingsInterface final : public SettingsInterface
{
public:
  MemorySettingsInterface();
  ~MemorySettingsInterface();

  bool Save(Error* error = nullptr) override;

  void Clear() override;

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

  std::vector<std::pair<std::string, std::string>> GetKeyValueList(const char* section) const override;
  void SetKeyValueList(const char* section, const std::vector<std::pair<std::string, std::string>>& items) override;

  bool ContainsValue(const char* section, const char* key) const override;
  void DeleteValue(const char* section, const char* key) override;
  void ClearSection(const char* section) override;

  std::vector<std::string> GetStringList(const char* section, const char* key) const override;
  void SetStringList(const char* section, const char* key, const std::vector<std::string>& items) override;
  bool RemoveFromStringList(const char* section, const char* key, const char* item) override;
  bool AddToStringList(const char* section, const char* key, const char* item) override;

  // default parameter overloads
  using SettingsInterface::GetBoolValue;
  using SettingsInterface::GetDoubleValue;
  using SettingsInterface::GetFloatValue;
  using SettingsInterface::GetIntValue;
  using SettingsInterface::GetStringValue;
  using SettingsInterface::GetUIntValue;

private:
  using KeyMap = PreferUnorderedStringMultimap<std::string>;
  using SectionMap = PreferUnorderedStringMap<KeyMap>;

  void SetValue(const char* section, const char* key, std::string value);

  SectionMap m_sections;
};
