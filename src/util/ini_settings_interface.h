// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/settings_interface.h"

// being a pain here...
#ifdef _WIN32
#include "common/windows_headers.h"
#endif
#include "SimpleIni.h"

class INISettingsInterface final : public SettingsInterface
{
public:
  INISettingsInterface(std::string path);
  ~INISettingsInterface() override;

  const std::string& GetPath() const { return m_path; }

  bool Load(Error* error = nullptr);
  bool Load(std::string new_path, Error* error = nullptr);
  bool Save(Error* error = nullptr) override;

  void Clear() override;
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
  std::string m_path;
  CSimpleIniA m_ini;
  bool m_dirty = false;
};
