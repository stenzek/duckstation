#pragma once
#include "core/settings.h"

#include "SimpleIni.h"

class SDLSettingsInterface : public SettingsInterface
{
public:
  SDLSettingsInterface(const char* filename);
  ~SDLSettingsInterface();

  int GetIntValue(const char* section, const char* key, int default_value = 0) override;
  float GetFloatValue(const char* section, const char* key, float default_value = 0.0f) override;
  bool GetBoolValue(const char* section, const char* key, bool default_value = false) override;
  std::string GetStringValue(const char* section, const char* key, const char* default_value = "") override;

  void SetIntValue(const char* section, const char* key, int value) override;
  void SetFloatValue(const char* section, const char* key, float value) override;
  void SetBoolValue(const char* section, const char* key, bool value) override;
  void SetStringValue(const char* section, const char* key, const char* value) override;
  void DeleteValue(const char* section, const char* key) override;

private:
  std::string m_filename;
  CSimpleIniA m_ini;
};
