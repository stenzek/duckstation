// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "settings_interface.h"
#include "string_util.h"

SettingsInterface::~SettingsInterface() = default;

// LLVM LTO is way too eager to inline these at the call sites, and it ends up causing considerable code bloat.

NEVER_INLINE bool SettingsInterface::FindIntValue(const char* section, const char* key, s32* value) const
{
  std::string_view str_value;
  if (!LookupValue(section, key, &str_value))
    return false;

  const std::optional<s32> parsed = StringUtil::FromChars<s32>(str_value, 10);
  if (!parsed.has_value())
    return false;

  *value = parsed.value();
  return true;
}

NEVER_INLINE bool SettingsInterface::FindUIntValue(const char* section, const char* key, u32* value) const
{
  std::string_view str_value;
  if (!LookupValue(section, key, &str_value))
    return false;

  const std::optional<u32> parsed = StringUtil::FromChars<u32>(str_value, 10);
  if (!parsed.has_value())
    return false;

  *value = parsed.value();
  return true;
}

NEVER_INLINE bool SettingsInterface::FindFloatValue(const char* section, const char* key, float* value) const
{
  std::string_view str_value;
  if (!LookupValue(section, key, &str_value))
    return false;

  const std::optional<float> parsed = StringUtil::FromChars<float>(str_value);
  if (!parsed.has_value())
    return false;

  *value = parsed.value();
  return true;
}

NEVER_INLINE bool SettingsInterface::FindDoubleValue(const char* section, const char* key, double* value) const
{
  std::string_view str_value;
  if (!LookupValue(section, key, &str_value))
    return false;

  const std::optional<double> parsed = StringUtil::FromChars<double>(str_value);
  if (!parsed.has_value())
    return false;

  *value = parsed.value();
  return true;
}

NEVER_INLINE bool SettingsInterface::FindBoolValue(const char* section, const char* key, bool* value) const
{
  std::string_view str_value;
  if (!LookupValue(section, key, &str_value))
    return false;

  const std::optional<bool> parsed = StringUtil::FromChars<bool>(str_value);
  if (!parsed.has_value())
    return false;

  *value = parsed.value();
  return true;
}

bool SettingsInterface::FindStringValue(const char* section, const char* key, std::string_view* value) const
{
  return LookupValue(section, key, value);
}

s32 SettingsInterface::GetIntValue(const char* section, const char* key, s32 default_value /*= 0*/) const
{
  s32 value;
  return FindIntValue(section, key, &value) ? value : default_value;
}

u32 SettingsInterface::GetUIntValue(const char* section, const char* key, u32 default_value /*= 0*/) const
{
  u32 value;
  return FindUIntValue(section, key, &value) ? value : default_value;
}

float SettingsInterface::GetFloatValue(const char* section, const char* key, float default_value /*= 0.0f*/) const
{
  float value;
  return FindFloatValue(section, key, &value) ? value : default_value;
}

double SettingsInterface::GetDoubleValue(const char* section, const char* key, double default_value /*= 0.0*/) const
{
  double value;
  return FindDoubleValue(section, key, &value) ? value : default_value;
}

bool SettingsInterface::GetBoolValue(const char* section, const char* key, bool default_value /*= false*/) const
{
  bool value;
  return FindBoolValue(section, key, &value) ? value : default_value;
}

std::string_view SettingsInterface::GetStringViewValue(const char* section, const char* key,
                                                       std::string_view default_value /*= ""*/) const
{
  std::string_view value;
  return FindStringValue(section, key, &value) ? value : default_value;
}

std::string SettingsInterface::GetStringValue(const char* section, const char* key,
                                              std::string_view default_value /* = {}*/) const
{
  std::string_view vvalue;
  if (FindStringValue(section, key, &vvalue))
    return std::string(vvalue);

  return std::string(default_value);
}

SmallString SettingsInterface::GetSmallStringValue(const char* section, const char* key,
                                                   std::string_view default_value /* = {}*/) const
{
  SmallString value;
  std::string_view vvalue;
  if (FindStringValue(section, key, &vvalue))
    value.assign(vvalue);
  else
    value.assign(default_value);

  return value;
}

TinyString SettingsInterface::GetTinyStringValue(const char* section, const char* key,
                                                 std::string_view default_value /* = {}*/) const
{
  TinyString value;
  std::string_view vvalue;
  if (FindStringValue(section, key, &vvalue))
    value.assign(vvalue);
  else
    value.assign(default_value);

  return value;
}

std::optional<s32> SettingsInterface::GetOptionalIntValue(const char* section, const char* key,
                                                          std::optional<s32> default_value /*= std::nullopt*/) const
{
  s32 ret;
  return FindIntValue(section, key, &ret) ? std::optional<s32>(ret) : default_value;
}

std::optional<u32> SettingsInterface::GetOptionalUIntValue(const char* section, const char* key,
                                                           std::optional<u32> default_value /*= std::nullopt*/) const
{
  u32 ret;
  return FindUIntValue(section, key, &ret) ? std::optional<u32>(ret) : default_value;
}

std::optional<float>
SettingsInterface::GetOptionalFloatValue(const char* section, const char* key,
                                         std::optional<float> default_value /*= std::nullopt*/) const
{
  float ret;
  return FindFloatValue(section, key, &ret) ? std::optional<float>(ret) : default_value;
}

std::optional<double>
SettingsInterface::GetOptionalDoubleValue(const char* section, const char* key,
                                          std::optional<double> default_value /*= std::nullopt*/) const
{
  double ret;
  return FindDoubleValue(section, key, &ret) ? std::optional<double>(ret) : default_value;
}

std::optional<bool> SettingsInterface::GetOptionalBoolValue(const char* section, const char* key,
                                                            std::optional<bool> default_value /*= std::nullopt*/) const
{
  bool ret;
  return FindBoolValue(section, key, &ret) ? std::optional<bool>(ret) : default_value;
}

std::optional<std::string>
SettingsInterface::GetOptionalStringValue(const char* section, const char* key,
                                          std::optional<const char*> default_value /*= std::nullopt*/) const
{
  std::string_view ret;
  return FindStringValue(section, key, &ret) ?
           std::optional<std::string>(ret) :
           (default_value.has_value() ? std::optional<std::string>(default_value.value()) :
                                        std::optional<std::string>());
}

std::optional<SmallString>
SettingsInterface::GetOptionalSmallStringValue(const char* section, const char* key,
                                               std::optional<const char*> default_value /*= std::nullopt*/) const
{
  std::string_view ret;
  return FindStringValue(section, key, &ret) ?
           std::optional<SmallString>(ret) :
           (default_value.has_value() ? std::optional<SmallString>(default_value.value()) :
                                        std::optional<SmallString>());
}

std::optional<TinyString>
SettingsInterface::GetOptionalTinyStringValue(const char* section, const char* key,
                                              std::optional<const char*> default_value /*= std::nullopt*/) const
{
  std::string_view ret;
  return FindStringValue(section, key, &ret) ?
           std::optional<TinyString>(ret) :
           (default_value.has_value() ? std::optional<TinyString>(default_value.value()) : std::optional<TinyString>());
}

template<typename T>
  requires std::is_integral_v<T>
T SettingsInterface::GetSaturatedIntValue(const char* section, const char* key, T default_value /*= 0*/) const
{
  s32 value;
  if (!FindIntValue(section, key, &value))
    return default_value;
  if (value < static_cast<s32>(std::numeric_limits<T>::min()))
    return std::numeric_limits<T>::min();
  if (value > static_cast<s32>(std::numeric_limits<T>::max()))
    return std::numeric_limits<T>::max();
  return static_cast<T>(value);
}

template u8 SettingsInterface::GetSaturatedIntValue(const char* section, const char* key, u8 default_value) const;
template s16 SettingsInterface::GetSaturatedIntValue(const char* section, const char* key, s16 default_value) const;
template u16 SettingsInterface::GetSaturatedIntValue(const char* section, const char* key, u16 default_value) const;

void SettingsInterface::SetIntValue(const char* section, const char* key, s32 value)
{
  StoreValue(section, key, StringUtil::ToChars(value).c_str());
}

void SettingsInterface::SetUIntValue(const char* section, const char* key, u32 value)
{
  StoreValue(section, key, StringUtil::ToChars(value).c_str());
}

void SettingsInterface::SetFloatValue(const char* section, const char* key, float value)
{
  StoreValue(section, key, StringUtil::ToChars(value).c_str());
}

void SettingsInterface::SetDoubleValue(const char* section, const char* key, double value)
{
  StoreValue(section, key, StringUtil::ToChars(value).c_str());
}

void SettingsInterface::SetBoolValue(const char* section, const char* key, bool value)
{
  StoreValue(section, key, value ? "true" : "false");
}

void SettingsInterface::SetStringValue(const char* section, const char* key, std::string_view value)
{
  StoreValue(section, key, value);
}

void SettingsInterface::SetOptionalIntValue(const char* section, const char* key, const std::optional<s32>& value)
{
  value.has_value() ? SetIntValue(section, key, value.value()) : DeleteValue(section, key);
}

void SettingsInterface::SetOptionalUIntValue(const char* section, const char* key, const std::optional<u32>& value)
{
  value.has_value() ? SetUIntValue(section, key, value.value()) : DeleteValue(section, key);
}

void SettingsInterface::SetOptionalFloatValue(const char* section, const char* key, const std::optional<float>& value)
{
  value.has_value() ? SetFloatValue(section, key, value.value()) : DeleteValue(section, key);
}

void SettingsInterface::SetOptionalDoubleValue(const char* section, const char* key, const std::optional<double>& value)
{
  value.has_value() ? SetDoubleValue(section, key, value.value()) : DeleteValue(section, key);
}

void SettingsInterface::SetOptionalBoolValue(const char* section, const char* key, const std::optional<bool>& value)
{
  value.has_value() ? SetBoolValue(section, key, value.value()) : DeleteValue(section, key);
}

void SettingsInterface::SetOptionalStringValue(const char* section, const char* key,
                                               const std::optional<const char*>& value)
{
  value.has_value() ? SetStringValue(section, key, value.value()) : DeleteValue(section, key);
}

void SettingsInterface::CopyBoolValue(const SettingsInterface& si, const char* section, const char* key)
{
  bool value;
  if (si.FindBoolValue(section, key, &value))
    SetBoolValue(section, key, value);
  else
    DeleteValue(section, key);
}

void SettingsInterface::CopyIntValue(const SettingsInterface& si, const char* section, const char* key)
{
  s32 value;
  if (si.FindIntValue(section, key, &value))
    SetIntValue(section, key, value);
  else
    DeleteValue(section, key);
}

void SettingsInterface::CopyUIntValue(const SettingsInterface& si, const char* section, const char* key)
{
  u32 value;
  if (si.FindUIntValue(section, key, &value))
    SetUIntValue(section, key, value);
  else
    DeleteValue(section, key);
}

void SettingsInterface::CopyFloatValue(const SettingsInterface& si, const char* section, const char* key)
{
  float value;
  if (si.FindFloatValue(section, key, &value))
    SetFloatValue(section, key, value);
  else
    DeleteValue(section, key);
}

void SettingsInterface::CopyDoubleValue(const SettingsInterface& si, const char* section, const char* key)
{
  double value;
  if (si.FindDoubleValue(section, key, &value))
    SetDoubleValue(section, key, value);
  else
    DeleteValue(section, key);
}

void SettingsInterface::CopyStringValue(const SettingsInterface& si, const char* section, const char* key)
{
  std::string_view value;
  if (si.FindStringValue(section, key, &value))
    SetStringValue(section, key, value);
  else
    DeleteValue(section, key);
}

void SettingsInterface::CopyStringListValue(const SettingsInterface& si, const char* section, const char* key)
{
  std::vector<std::string> value(si.GetStringList(section, key));
  if (!value.empty())
    SetStringList(section, key, value);
  else
    DeleteValue(section, key);
}

void SettingsInterface::CopySection(const SettingsInterface& si, const char* section)
{
  ClearSection(section);

  for (const auto& [key, value] : si.GetKeyValueList(section))
    SetStringValue(section, key.c_str(), value);
}
