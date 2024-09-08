// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: PolyForm-Strict-1.0.0

#include "types.h"

#include "ryml.hpp"

#include <string>
#include <string_view>

// RapidYAML utility routines.

[[maybe_unused]] ALWAYS_INLINE std::string_view to_stringview(const c4::csubstr& s)
{
  return std::string_view(s.data(), s.size());
}

[[maybe_unused]] ALWAYS_INLINE std::string_view to_stringview(const c4::substr& s)
{
  return std::string_view(s.data(), s.size());
}

[[maybe_unused]] ALWAYS_INLINE c4::csubstr to_csubstr(std::string_view sv)
{
  return c4::csubstr(sv.data(), sv.length());
}

[[maybe_unused]] static bool GetStringFromObject(const ryml::ConstNodeRef& object, std::string_view key,
                                                 std::string* dest)
{
  dest->clear();

  const ryml::ConstNodeRef member = object.find_child(to_csubstr(key));
  if (!member.valid())
    return false;

  const c4::csubstr val = member.val();
  if (!val.empty())
    dest->assign(val.data(), val.size());

  return true;
}

template<typename T>
[[maybe_unused]] static bool GetUIntFromObject(const ryml::ConstNodeRef& object, std::string_view key, T* dest)
{
  *dest = 0;

  const ryml::ConstNodeRef member = object.find_child(to_csubstr(key));
  if (!member.valid())
    return false;

  const c4::csubstr val = member.val();
  if (val.empty())
  {
    ERROR_LOG("Unexpected empty value in {}", key);
    return false;
  }

  const std::optional<T> opt_value = StringUtil::FromChars<T>(to_stringview(val));
  if (!opt_value.has_value())
  {
    ERROR_LOG("Unexpected non-uint value in {}", key);
    return false;
  }

  *dest = opt_value.value();
  return true;
}

template<typename T>
[[maybe_unused]] static std::optional<T> GetOptionalTFromObject(const ryml::ConstNodeRef& object, std::string_view key)
{
  std::optional<T> ret;

  const ryml::ConstNodeRef member = object.find_child(to_csubstr(key));
  if (member.valid())
  {
    const c4::csubstr val = member.val();
    if (!val.empty())
    {
      ret = StringUtil::FromChars<T>(to_stringview(val));
      if (!ret.has_value())
      {
        if constexpr (std::is_floating_point_v<T>)
          ERROR_LOG("Unexpected non-float value in {}", key);
        else if constexpr (std::is_integral_v<T>)
          ERROR_LOG("Unexpected non-int value in {}", key);
      }
    }
    else
    {
      ERROR_LOG("Unexpected empty value in {}", key);
    }
  }

  return ret;
}

template<typename T>
[[maybe_unused]] static std::optional<T>
ParseOptionalTFromObject(const ryml::ConstNodeRef& object, std::string_view key,
                         std::optional<T> (*from_string_function)(const char* str))
{
  std::optional<T> ret;

  const ryml::ConstNodeRef member = object.find_child(to_csubstr(key));
  if (member.valid())
  {
    const c4::csubstr val = member.val();
    if (!val.empty())
    {
      ret = from_string_function(TinyString(to_stringview(val)));
      if (!ret.has_value())
        ERROR_LOG("Unknown value for {}: {}", key, to_stringview(val));
    }
    else
    {
      ERROR_LOG("Unexpected empty value in {}", key);
    }
  }

  return ret;
}
