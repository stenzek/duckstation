// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "small_string.h"
#include "types.h"

#include <limits>
#include <optional>
#include <string>
#include <vector>

class Error;

class SettingsInterface
{
public:
  virtual ~SettingsInterface();

  virtual bool IsEmpty() = 0;

  virtual bool GetIntValue(const char* section, const char* key, s32* value) const = 0;
  virtual bool GetUIntValue(const char* section, const char* key, u32* value) const = 0;
  virtual bool GetFloatValue(const char* section, const char* key, float* value) const = 0;
  virtual bool GetDoubleValue(const char* section, const char* key, double* value) const = 0;
  virtual bool GetBoolValue(const char* section, const char* key, bool* value) const = 0;
  virtual bool GetStringValue(const char* section, const char* key, std::string* value) const = 0;
  virtual bool GetStringValue(const char* section, const char* key, SmallStringBase* value) const = 0;

  virtual void SetIntValue(const char* section, const char* key, s32 value) = 0;
  virtual void SetUIntValue(const char* section, const char* key, u32 value) = 0;
  virtual void SetFloatValue(const char* section, const char* key, float value) = 0;
  virtual void SetDoubleValue(const char* section, const char* key, double value) = 0;
  virtual void SetBoolValue(const char* section, const char* key, bool value) = 0;
  virtual void SetStringValue(const char* section, const char* key, const char* value) = 0;

  virtual std::vector<std::string> GetStringList(const char* section, const char* key) const = 0;
  virtual void SetStringList(const char* section, const char* key, const std::vector<std::string>& items) = 0;
  virtual bool RemoveFromStringList(const char* section, const char* key, const char* item) = 0;
  virtual bool AddToStringList(const char* section, const char* key, const char* item) = 0;

  virtual std::vector<std::pair<std::string, std::string>> GetKeyValueList(const char* section) const = 0;
  virtual void SetKeyValueList(const char* section, const std::vector<std::pair<std::string, std::string>>& items) = 0;

  virtual bool ContainsValue(const char* section, const char* key) const = 0;
  virtual void DeleteValue(const char* section, const char* key) = 0;
  virtual void ClearSection(const char* section) = 0;
  virtual void RemoveSection(const char* section) = 0;
  virtual void RemoveEmptySections() = 0;

  s32 GetIntValue(const char* section, const char* key, s32 default_value = 0) const;
  u32 GetUIntValue(const char* section, const char* key, u32 default_value = 0) const;
  float GetFloatValue(const char* section, const char* key, float default_value = 0.0f) const;
  double GetDoubleValue(const char* section, const char* key, double default_value = 0.0) const;
  bool GetBoolValue(const char* section, const char* key, bool default_value = false) const;
  std::string GetStringValue(const char* section, const char* key, const char* default_value = "") const;
  SmallString GetSmallStringValue(const char* section, const char* key, const char* default_value = "") const;
  TinyString GetTinyStringValue(const char* section, const char* key, const char* default_value = "") const;

  template<typename T>
    requires std::is_integral_v<T>
  T GetSaturatedIntValue(const char* section, const char* key, T default_value = 0) const;

  std::optional<s32> GetOptionalIntValue(const char* section, const char* key,
                                         std::optional<s32> default_value = std::nullopt) const;
  std::optional<u32> GetOptionalUIntValue(const char* section, const char* key,
                                          std::optional<u32> default_value = std::nullopt) const;
  std::optional<float> GetOptionalFloatValue(const char* section, const char* key,
                                             std::optional<float> default_value = std::nullopt) const;
  std::optional<double> GetOptionalDoubleValue(const char* section, const char* key,
                                               std::optional<double> default_value = std::nullopt) const;
  std::optional<bool> GetOptionalBoolValue(const char* section, const char* key,
                                           std::optional<bool> default_value = std::nullopt) const;

  std::optional<std::string> GetOptionalStringValue(const char* section, const char* key,
                                                    std::optional<const char*> default_value = std::nullopt) const;
  std::optional<SmallString> GetOptionalSmallStringValue(const char* section, const char* key,
                                                         std::optional<const char*> default_value = std::nullopt) const;
  std::optional<TinyString> GetOptionalTinyStringValue(const char* section, const char* key,
                                                       std::optional<const char*> default_value = std::nullopt) const;

  void SetOptionalIntValue(const char* section, const char* key, const std::optional<s32>& value);
  void SetOptionalUIntValue(const char* section, const char* key, const std::optional<u32>& value);
  void SetOptionalFloatValue(const char* section, const char* key, const std::optional<float>& value);
  void SetOptionalDoubleValue(const char* section, const char* key, const std::optional<double>& value);
  void SetOptionalBoolValue(const char* section, const char* key, const std::optional<bool>& value);
  void SetOptionalStringValue(const char* section, const char* key, const std::optional<const char*>& value);

  void CopyBoolValue(const SettingsInterface& si, const char* section, const char* key);
  void CopyIntValue(const SettingsInterface& si, const char* section, const char* key);
  void CopyUIntValue(const SettingsInterface& si, const char* section, const char* key);
  void CopyFloatValue(const SettingsInterface& si, const char* section, const char* key);
  void CopyDoubleValue(const SettingsInterface& si, const char* section, const char* key);
  void CopyStringValue(const SettingsInterface& si, const char* section, const char* key);
  void CopyStringListValue(const SettingsInterface& si, const char* section, const char* key);

  // NOTE: Writes values as strings.
  void CopySection(const SettingsInterface& si, const char* section);
};
