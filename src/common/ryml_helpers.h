// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "log.h"
#include "types.h"
#include "string_util.h"

#include "ryml.hpp"

#include <string>
#include <string_view>

// RapidYAML utility routines.

static inline std::string_view to_stringview(const c4::csubstr& s)
{
  return std::string_view(s.data(), s.size());
}

static inline std::string_view to_stringview(const c4::substr& s)
{
  return std::string_view(s.data(), s.size());
}

static inline c4::csubstr to_csubstr(std::string_view sv)
{
  return c4::csubstr(sv.data(), sv.length());
}

static inline bool GetStringFromObject(const ryml::ConstNodeRef& object, std::string_view key, std::string* dest)
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

static inline bool GetStringFromObject(const ryml::ConstNodeRef& object, std::string_view key, std::string_view* dest)
{
  const ryml::ConstNodeRef member = object.find_child(to_csubstr(key));
  if (!member.valid())
  {
    *dest = std::string_view();
    return false;
  }

  *dest = to_stringview(member.val());
  return true;
}

template<typename T>
static inline bool GetUIntFromObject(const ryml::ConstNodeRef& object, std::string_view key, T* dest)
{
  *dest = 0;

  const ryml::ConstNodeRef member = object.find_child(to_csubstr(key));
  if (!member.valid())
    return false;

  const c4::csubstr val = member.val();
  if (val.empty())
  {
    Log::FastWrite(Log::Channel::Log, Log::Level::Error, Log::Color::StrongOrange, "Unexpected empty value in {}", key);
    return false;
  }

  const std::optional<T> opt_value = StringUtil::FromChars<T>(to_stringview(val));
  if (!opt_value.has_value())
  {
    Log::FastWrite(Log::Channel::Log, Log::Level::Error, Log::Color::StrongOrange, "Unexpected non-uint value in {}",
                   key);
    return false;
  }

  *dest = opt_value.value();
  return true;
}

template<typename T>
static inline std::optional<T> GetOptionalTFromObject(const ryml::ConstNodeRef& object, std::string_view key)
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
        if constexpr (std::is_same_v<T, bool>)
        {
          Log::FastWrite(Log::Channel::Log, Log::Level::Error, Log::Color::StrongOrange,
                         "Unexpected non-bool value in {}", key);
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
          Log::FastWrite(Log::Channel::Log, Log::Level::Error, Log::Color::StrongOrange,
                         "Unexpected non-float value in {}", key);
        }
        else if constexpr (std::is_integral_v<T>)
        {
          Log::FastWrite(Log::Channel::Log, Log::Level::Error, Log::Color::StrongOrange,
                         "Unexpected non-int value in {}", key);
        }
      }
    }
    else
    {
      Log::FastWrite(Log::Channel::Log, Log::Level::Error, Log::Color::StrongOrange, "Unexpected empty value in {}",
                     key);
    }
  }

  return ret;
}

template<typename T>
static inline std::optional<T> ParseOptionalTFromObject(const ryml::ConstNodeRef& object, std::string_view key,
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
        Log::FastWrite(Log::Channel::Log, Log::Level::Error, Log::Color::StrongOrange, "Unknown value for {}: {}", key,
                       to_stringview(val));
    }
    else
    {
      Log::FastWrite(Log::Channel::Log, Log::Level::Error, Log::Color::StrongOrange, "Unexpected empty value in {}",
                     key);
    }
  }

  return ret;
}

static inline void SetRymlCallbacks()
{
  ryml::Callbacks callbacks = ryml::get_callbacks();
  callbacks.m_error = [](const char* msg, size_t msg_len, ryml::Location loc, void* userdata) {
    Log::FastWrite(Log::Channel::Log, Log::Level::Error, Log::Color::StrongOrange,
                   "YAML parse error at {}:{} (bufpos={}): {}", loc.line, loc.col, loc.offset,
                   std::string_view(msg, msg_len));
  };
  ryml::set_callbacks(callbacks);
  c4::set_error_callback([](const char* msg, size_t msg_size) {
    Log::FastWrite(Log::Channel::Log, Log::Level::Error, Log::Color::StrongOrange, "C4 error: {}",
                   std::string_view(msg, msg_size));
  });
}
