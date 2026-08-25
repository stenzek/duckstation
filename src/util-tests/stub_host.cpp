// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/core.h"

#include "util/translation.h"

#include "common/time_helpers.h"

bool Core::GetBaseBoolSettingValue(const char* section, const char* key, bool default_value /* = false */)
{
  return default_value;
}

TinyString Host::TranslatePluralToTinyString(const char* context, const char* msg, const char* disambiguation,
                                             int count)
{
  TinyString ret(msg);
  ret.replace("%n", TinyString::from_format("{}", count));
  return ret;
}

SmallString Host::TranslatePluralToSmallString(const char* context, const char* msg, const char* disambiguation,
                                               int count)
{
  SmallString ret(msg);
  ret.replace("%n", TinyString::from_format("{}", count));
  return ret;
}

s32 Host::Internal::GetTranslatedStringImpl(std::string_view context, std::string_view msg,
                                            std::string_view disambiguation, char* tbuf, size_t tbuf_space)
{
  if (msg.size() > tbuf_space)
    return -1;
  else if (msg.empty())
    return 0;

  std::memcpy(tbuf, msg.data(), msg.size());
  return static_cast<s32>(msg.size());
}

static TinyString FormatDateOrTime(const char* format, std::time_t timestamp)
{
  TinyString ret;
  if (const std::optional<std::tm> ltime = Common::LocalTime(timestamp))
    ret.resize(static_cast<u32>(std::strftime(ret.data(), ret.buffer_size(), format, &ltime.value())));
  else
    ret = "Invalid";

  return ret;
}

TinyString Host::FormatDate(std::time_t timestamp, bool long_format)
{
  return FormatDateOrTime(long_format ? "%A %B %e %Y" : "%x", timestamp);
}

TinyString Host::FormatTime(std::time_t timestamp, bool long_format)
{
  return FormatDateOrTime("%X", timestamp);
}

TinyString Host::FormatDateTime(std::time_t timestamp, bool long_format)
{
  return FormatDateOrTime(long_format ? "%c" : "%X %x", timestamp);
}