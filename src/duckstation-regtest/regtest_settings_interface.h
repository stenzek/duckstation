#pragma once
#include "core/settings.h"
#include <string>
#include <unordered_map>

class RegTestSettingsInterface final : public SettingsInterface
{
public:
  RegTestSettingsInterface();
  ~RegTestSettingsInterface();

  bool Save() override;
  void Clear() override;

  int GetIntValue(const char* section, const char* key, int default_value = 0) override;
  float GetFloatValue(const char* section, const char* key, float default_value = 0.0f) override;
  bool GetBoolValue(const char* section, const char* key, bool default_value = false) override;

  std::string GetStringValue(const char* section, const char* key, const char* default_value = "") override;
  void SetIntValue(const char* section, const char* key, int value) override;
  void SetFloatValue(const char* section, const char* key, float value) override;
  void SetBoolValue(const char* section, const char* key, bool value) override;
  void SetStringValue(const char* section, const char* key, const char* value) override;

  std::vector<std::string> GetStringList(const char* section, const char* key) override;
  void SetStringList(const char* section, const char* key, const std::vector<std::string>& items) override;
  bool RemoveFromStringList(const char* section, const char* key, const char* item) override;
  bool AddToStringList(const char* section, const char* key, const char* item) override;

  void DeleteValue(const char* section, const char* key) override;
  void ClearSection(const char* section) override;

private:
  using KeyMap = std::unordered_map<std::string, std::string>;
  KeyMap m_keys;
};