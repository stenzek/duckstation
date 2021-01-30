#pragma once
#include "core/settings.h"

// being a pain here...
#ifdef WIN32
#include "common/windows_headers.h"
#endif
#include "SimpleIni.h"

class INISettingsInterface final : public SettingsInterface
{
public:
  INISettingsInterface(std::string filename);
  ~INISettingsInterface() override;

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
  void DeleteValue(const char* section, const char* key) override;
  void ClearSection(const char* section) override;

  std::vector<std::string> GetStringList(const char* section, const char* key) override;
  void SetStringList(const char* section, const char* key, const std::vector<std::string>& items) override;
  bool RemoveFromStringList(const char* section, const char* key, const char* item) override;
  bool AddToStringList(const char* section, const char* key, const char* item) override;

private:
  std::string m_filename;
  CSimpleIniA m_ini;
  bool m_dirty = false;
};
