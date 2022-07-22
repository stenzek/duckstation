#pragma once

#include "types.h"
#include <optional>
#include <string>
#include <vector>

class SettingsInterface
{
public:
  virtual ~SettingsInterface() = default;

  virtual bool Save() = 0;
  virtual void Clear() = 0;

  virtual bool GetIntValue(const char* section, const char* key, s32* value) const = 0;
  virtual bool GetUIntValue(const char* section, const char* key, u32* value) const = 0;
  virtual bool GetFloatValue(const char* section, const char* key, float* value) const = 0;
  virtual bool GetDoubleValue(const char* section, const char* key, double* value) const = 0;
  virtual bool GetBoolValue(const char* section, const char* key, bool* value) const = 0;
  virtual bool GetStringValue(const char* section, const char* key, std::string* value) const = 0;

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

  virtual bool ContainsValue(const char* section, const char* key) const = 0;
  virtual void DeleteValue(const char* section, const char* key) = 0;
  virtual void ClearSection(const char* section) = 0;

  ALWAYS_INLINE s32 GetIntValue(const char* section, const char* key, s32 default_value = 0) const
  {
    s32 value;
    return GetIntValue(section, key, &value) ? value : default_value;
  }

  ALWAYS_INLINE u32 GetUIntValue(const char* section, const char* key, u32 default_value = 0) const
  {
    u32 value;
    return GetUIntValue(section, key, &value) ? value : default_value;
  }

  ALWAYS_INLINE float GetFloatValue(const char* section, const char* key, float default_value = 0.0f) const
  {
    float value;
    return GetFloatValue(section, key, &value) ? value : default_value;
  }

  ALWAYS_INLINE double GetDoubleValue(const char* section, const char* key, double default_value = 0.0) const
  {
    double value;
    return GetDoubleValue(section, key, &value) ? value : default_value;
  }

  ALWAYS_INLINE bool GetBoolValue(const char* section, const char* key, bool default_value = false) const
  {
    bool value;
    return GetBoolValue(section, key, &value) ? value : default_value;
  }

  ALWAYS_INLINE std::string GetStringValue(const char* section, const char* key, const char* default_value = "") const
  {
    std::string value;
    if (!GetStringValue(section, key, &value))
      value.assign(default_value);
    return value;
  }

  ALWAYS_INLINE std::optional<s32> GetOptionalIntValue(const char* section, const char* key,
                                                       std::optional<s32> default_value = std::nullopt)
  {
    s32 ret;
    return GetIntValue(section, key, &ret) ? std::optional<s32>(ret) : default_value;
  }

  ALWAYS_INLINE std::optional<u32> GetOptionalUIntValue(const char* section, const char* key,
                                                        std::optional<u32> default_value = std::nullopt)
  {
    u32 ret;
    return GetUIntValue(section, key, &ret) ? std::optional<u32>(ret) : default_value;
  }

  ALWAYS_INLINE std::optional<float> GetOptionalFloatValue(const char* section, const char* key,
                                                           std::optional<float> default_value = std::nullopt)
  {
    float ret;
    return GetFloatValue(section, key, &ret) ? std::optional<float>(ret) : default_value;
  }

  ALWAYS_INLINE std::optional<double> GetOptionalDoubleValue(const char* section, const char* key,
                                                             std::optional<double> default_value = std::nullopt)
  {
    double ret;
    return GetDoubleValue(section, key, &ret) ? std::optional<double>(ret) : default_value;
  }

  ALWAYS_INLINE std::optional<bool> GetOptionalBoolValue(const char* section, const char* key,
                                                         std::optional<bool> default_value = std::nullopt)
  {
    bool ret;
    return GetBoolValue(section, key, &ret) ? std::optional<bool>(ret) : default_value;
  }

  ALWAYS_INLINE std::optional<std::string>
  GetOptionalStringValue(const char* section, const char* key,
                         std::optional<const char*> default_value = std::nullopt) const
  {
    std::string ret;
    return GetStringValue(section, key, &ret) ? std::optional<std::string>(ret) : default_value;
  }

  ALWAYS_INLINE void SetOptionalIntValue(const char* section, const char* key, const std::optional<s32>& value)
  {
    value.has_value() ? SetIntValue(section, key, value.value()) : DeleteValue(section, key);
  }

  ALWAYS_INLINE void SetOptionalUIntValue(const char* section, const char* key, const std::optional<u32>& value)
  {
    value.has_value() ? SetUIntValue(section, key, value.value()) : DeleteValue(section, key);
  }

  ALWAYS_INLINE void SetOptionalFloatValue(const char* section, const char* key, const std::optional<float>& value)
  {
    value.has_value() ? SetFloatValue(section, key, value.value()) : DeleteValue(section, key);
  }

  ALWAYS_INLINE void SetOptionalDoubleValue(const char* section, const char* key, const std::optional<double>& value)
  {
    value.has_value() ? SetDoubleValue(section, key, value.value()) : DeleteValue(section, key);
  }

  ALWAYS_INLINE void SetOptionalBoolValue(const char* section, const char* key, const std::optional<bool>& value)
  {
    value.has_value() ? SetBoolValue(section, key, value.value()) : DeleteValue(section, key);
  }

  ALWAYS_INLINE void SetOptionalStringValue(const char* section, const char* key,
                                            const std::optional<const char*>& value)
  {
    value.has_value() ? SetStringValue(section, key, value.value()) : DeleteValue(section, key);
  }

  ALWAYS_INLINE void CopyBoolValue(const SettingsInterface& si, const char* section, const char* key)
  {
    bool value;
    if (si.GetBoolValue(section, key, &value))
      SetBoolValue(section, key, value);
    else
      DeleteValue(section, key);
  }

  ALWAYS_INLINE void CopyIntValue(const SettingsInterface& si, const char* section, const char* key)
  {
    s32 value;
    if (si.GetIntValue(section, key, &value))
      SetIntValue(section, key, value);
    else
      DeleteValue(section, key);
  }

  ALWAYS_INLINE void CopyUIntValue(const SettingsInterface& si, const char* section, const char* key)
  {
    u32 value;
    if (si.GetUIntValue(section, key, &value))
      SetUIntValue(section, key, value);
    else
      DeleteValue(section, key);
  }

  ALWAYS_INLINE void CopyFloatValue(const SettingsInterface& si, const char* section, const char* key)
  {
    float value;
    if (si.GetFloatValue(section, key, &value))
      SetFloatValue(section, key, value);
    else
      DeleteValue(section, key);
  }

  ALWAYS_INLINE void CopyDoubleValue(const SettingsInterface& si, const char* section, const char* key)
  {
    double value;
    if (si.GetDoubleValue(section, key, &value))
      SetDoubleValue(section, key, value);
    else
      DeleteValue(section, key);
  }

  ALWAYS_INLINE void CopyStringValue(const SettingsInterface& si, const char* section, const char* key)
  {
    std::string value;
    if (si.GetStringValue(section, key, &value))
      SetStringValue(section, key, value.c_str());
    else
      DeleteValue(section, key);
  }

  ALWAYS_INLINE void CopyStringListValue(const SettingsInterface& si, const char* section, const char* key)
  {
    std::vector<std::string> value(si.GetStringList(section, key));
    if (!value.empty())
      SetStringList(section, key, value);
    else
      DeleteValue(section, key);
  }
};
