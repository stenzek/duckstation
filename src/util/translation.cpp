// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "translation.h"

#include "common/assert.h"
#include "common/heterogeneous_containers.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/time_helpers.h"

#include <cstdarg>
#include <shared_mutex>

LOG_CHANNEL(Host);

namespace Host {

static std::pair<const char*, u32> LookupTranslationString(std::string_view context, std::string_view msg,
                                                           std::string_view disambiguation);

static constexpr u32 TRANSLATION_STRING_CACHE_SIZE = 4 * 1024 * 1024;
using TranslationStringMap = UnorderedStringMap<std::pair<u32, u32>>;
using TranslationStringContextMap = UnorderedStringMap<TranslationStringMap>;

struct TranslationLocals
{
  std::shared_mutex translation_string_mutex;
  TranslationStringContextMap translation_string_map;
  std::vector<char> translation_string_cache;
  u32 translation_string_cache_pos;
};

ALIGN_TO_CACHE_LINE static TranslationLocals s_locals;

} // namespace Host

std::pair<const char*, u32> Host::LookupTranslationString(std::string_view context, std::string_view msg,
                                                          std::string_view disambiguation)
{
  // TODO: TranslatableString, compile-time hashing.

  TranslationStringContextMap::iterator ctx_it;
  TranslationStringMap::iterator msg_it;
  std::pair<const char*, u32> ret;
  SmallString disambiguation_key;
  s32 len;

  // Shouldn't happen, but just in case someone tries to translate an empty string.
  if (msg.empty()) [[unlikely]]
  {
    ret.first = &s_locals.translation_string_cache[0];
    ret.second = 0;
    return ret;
  }

  if (!disambiguation.empty())
  {
    disambiguation_key.append(disambiguation);
    disambiguation_key.append(msg);
  }

  s_locals.translation_string_mutex.lock_shared();
  ctx_it = s_locals.translation_string_map.find(context);

  if (ctx_it == s_locals.translation_string_map.end()) [[unlikely]]
    goto add_string;

  msg_it = ctx_it->second.find(disambiguation.empty() ? msg : disambiguation_key.view());
  if (msg_it == ctx_it->second.end()) [[unlikely]]
    goto add_string;

  ret.first = &s_locals.translation_string_cache[msg_it->second.first];
  ret.second = msg_it->second.second;
  s_locals.translation_string_mutex.unlock_shared();
  return ret;

add_string:
  s_locals.translation_string_mutex.unlock_shared();
  s_locals.translation_string_mutex.lock();

  if (s_locals.translation_string_cache.empty()) [[unlikely]]
  {
    // First element is always an empty string.
    s_locals.translation_string_cache.resize(TRANSLATION_STRING_CACHE_SIZE);
    s_locals.translation_string_cache[0] = '\0';
    s_locals.translation_string_cache_pos = 0;
  }

  if ((len = Internal::GetTranslatedStringImpl(
         context, msg, disambiguation, &s_locals.translation_string_cache[s_locals.translation_string_cache_pos],
         TRANSLATION_STRING_CACHE_SIZE - 1 - s_locals.translation_string_cache_pos)) < 0)
  {
    ERROR_LOG("WARNING: Clearing translation string cache, it might need to be larger.");
    s_locals.translation_string_cache_pos = 0;
    if ((len = Internal::GetTranslatedStringImpl(
           context, msg, disambiguation, &s_locals.translation_string_cache[s_locals.translation_string_cache_pos],
           TRANSLATION_STRING_CACHE_SIZE - 1 - s_locals.translation_string_cache_pos)) < 0)
    {
      Panic("Failed to get translated string after clearing cache.");
      len = 0;
    }
  }

  // New context?
  if (ctx_it == s_locals.translation_string_map.end())
    ctx_it = s_locals.translation_string_map.emplace(context, TranslationStringMap()).first;

  // Impl doesn't null terminate, we need that for C strings.
  // TODO: do we want to consider aligning the buffer?
  const u32 insert_pos = s_locals.translation_string_cache_pos;
  s_locals.translation_string_cache[insert_pos + static_cast<u32>(len)] = 0;

  ctx_it->second.emplace(disambiguation.empty() ? msg : disambiguation_key.view(),
                         std::pair<u32, u32>(insert_pos, static_cast<u32>(len)));
  s_locals.translation_string_cache_pos = insert_pos + static_cast<u32>(len) + 1;

  ret.first = &s_locals.translation_string_cache[insert_pos];
  ret.second = static_cast<u32>(len);
  s_locals.translation_string_mutex.unlock();
  return ret;
}

const char* Host::TranslateToCString(std::string_view context, std::string_view msg, std::string_view disambiguation)
{
  return LookupTranslationString(context, msg, disambiguation).first;
}

std::string_view Host::TranslateToStringView(std::string_view context, std::string_view msg,
                                             std::string_view disambiguation)
{
  const auto mp = LookupTranslationString(context, msg, disambiguation);
  return std::string_view(mp.first, mp.second);
}

std::string Host::TranslateToString(std::string_view context, std::string_view msg, std::string_view disambiguation)
{
  return std::string(TranslateToStringView(context, msg, disambiguation));
}

void Host::ClearTranslationCache()
{
  s_locals.translation_string_mutex.lock();
  s_locals.translation_string_map.clear();
  s_locals.translation_string_cache_pos = 0;
  s_locals.translation_string_mutex.unlock();
}

TinyString Host::FormatRelativeDate(std::time_t timestamp, bool long_format /* = false */, bool for_title /* = false */)
{
  // Hack for zero.
  if (timestamp == 0)
    return TinyString(TranslateToStringView("Host", TRANSLATE_NOOP("Host", "Never")));

  // Avoid localtime call when more than two days have passed.
  const s64 current_time = static_cast<s64>(std::time(nullptr));
  if (current_time >= timestamp)
  {
    const s64 delta = current_time - timestamp;
    if (delta <= (2 * 24 * 60 * 60))
    {
      const std::optional<std::tm> ctime = Common::LocalTime(static_cast<std::time_t>(current_time));
      const std::optional<std::tm> ttime = Common::LocalTime(static_cast<std::time_t>(timestamp));
      if (ctime.has_value() && ttime.has_value() && ctime->tm_year == ttime->tm_year &&
          ctime->tm_yday == ttime->tm_yday)
      {
        return TinyString(
          TranslateToStringView("Host", for_title ? TRANSLATE_NOOP("Host", "Today") : TRANSLATE_NOOP("Host", "today")));
      }
      else if (ctime.has_value() && ttime.has_value() &&
               ((ctime->tm_year == ttime->tm_year && ctime->tm_yday == (ttime->tm_yday + 1)) ||
                (ctime->tm_yday == 0 && ctime->tm_mon == 0 && (ctime->tm_year - 1) == ttime->tm_year &&
                 ttime->tm_mon == 11 && ttime->tm_mday == 31)))
      {
        return TinyString(TranslateToStringView("Host", for_title ? TRANSLATE_NOOP("Host", "Yesterday") :
                                                                    TRANSLATE_NOOP("Host", "yesterday")));
      }
    }
    else if (delta < 7 * 24 * 60 * 60)
    {
      return TranslatePluralToTinyString("Host", TRANSLATE_PLURAL_NOOP("Host", "%n days ago", "Date difference"),
                                         "Date difference", static_cast<int>(delta) / (24 * 60 * 60));
    }
  }

  if (for_title)
    return FormatDate(timestamp, long_format);
  else
    return TinyString::from_format(TRANSLATE_FS("Host", "on {}"), FormatDate(timestamp, long_format));
}

TinyString Host::FormatRelativeDateTime(std::time_t timestamp, bool long_format /* = false */,
                                        bool for_title /* = false */, bool use_extended_relative_time /* = false */)
{
  // Hack for zero.
  if (timestamp == 0)
    return TinyString(TranslateToStringView("Host", TRANSLATE_NOOP("Host", "Never")));

  const s64 current_time = static_cast<s64>(std::time(nullptr));
  const s64 delta = current_time - timestamp;
  constexpr s64 WEEK = 7 * 24 * 60 * 60;
  if (delta < 5)
  {
    return TinyString(
      TranslateToStringView("Host", for_title ? TRANSLATE_NOOP("Host", "Now") : TRANSLATE_NOOP("Host", "now")));
  }
  else if (delta < 60)
  {
    return TranslatePluralToTinyString("Host", TRANSLATE_PLURAL_NOOP("Host", "%n seconds ago", "Time difference"),
                                       "Time difference", static_cast<int>(delta));
  }
  else if (delta < 60 * 60)
  {
    return TranslatePluralToTinyString("Host", TRANSLATE_PLURAL_NOOP("Host", "%n minutes ago", "Time difference"),
                                       "Time difference", static_cast<int>(delta) / 60);
  }
  else if (delta < 12 * 60 * 60)
  {
    return TranslatePluralToTinyString("Host", TRANSLATE_PLURAL_NOOP("Host", "%n hours ago", "Time difference"),
                                       "Time difference", static_cast<int>(delta) / (60 * 60));
  }
  else if (use_extended_relative_time && delta >= WEEK)
  {
    const std::tm current_tm = Common::LocalTime(static_cast<std::time_t>(current_time)).value_or(std::tm{});
    const std::optional<std::tm> timestamp_tm = Common::LocalTime(timestamp);
    if (!timestamp_tm.has_value())
      return TinyString();

    const int year_diff = current_tm.tm_year - timestamp_tm->tm_year;
    const int month_diff = current_tm.tm_mon - timestamp_tm->tm_mon;
    const int total_months = year_diff * 12 + month_diff;

    if (total_months == 0)
    {
      // Less than a month - use weeks
      const int weeks = static_cast<int>(delta / WEEK);
      return TranslatePluralToTinyString("Host", TRANSLATE_PLURAL_NOOP("Host", "%n weeks ago", "Time difference"),
                                         "Time difference", weeks);
    }

    if (total_months < 12)
    {
      return TranslatePluralToTinyString("Host", TRANSLATE_PLURAL_NOOP("Host", "%n months ago", "Time difference"),
                                         "Time difference", total_months);
    }

    // For years, adjust if we haven't reached the anniversary yet
    int years = year_diff;
    if (current_tm.tm_mon < timestamp_tm->tm_mon ||
        (current_tm.tm_mon == timestamp_tm->tm_mon && current_tm.tm_mday < timestamp_tm->tm_mday))
    {
      years--;
    }

    // Edge case: less than a full year but more than 11 months
    if (years < 1)
    {
      return TranslatePluralToTinyString("Host", TRANSLATE_PLURAL_NOOP("Host", "%n months ago", "Time difference"),
                                         "Time difference", total_months);
    }

    return TranslatePluralToTinyString("Host", TRANSLATE_PLURAL_NOOP("Host", "%n years ago", "Time difference"),
                                       "Time difference", years);
  }

  if (for_title)
  {
    return FormatDateTime(timestamp, long_format);
  }
  else
  {
    return TinyString::from_format(TRANSLATE_FS("Host", "{} at {}"), FormatRelativeDate(timestamp, long_format, false),
                                   FormatTime(timestamp, long_format));
  }
}

TinyString Host::FormatTimespan(std::time_t timespan, bool long_format /*= false*/)
{
  const u32 hours = static_cast<u32>(timespan / 3600);
  const u32 minutes = static_cast<u32>((timespan % 3600) / 60);
  const u32 seconds = static_cast<u32>((timespan % 3600) % 60);

  if (!long_format)
  {
    if (hours >= 100)
      return TinyString::from_format(TRANSLATE_FS("Host", "{}h {}m"), hours, minutes);
    else if (hours > 0)
      return TinyString::from_format(TRANSLATE_FS("Host", "{}h {}m {}s"), hours, minutes, seconds);
    else if (minutes > 0)
      return TinyString::from_format(TRANSLATE_FS("Host", "{}m {}s"), minutes, seconds);
    else if (seconds > 0)
      return TinyString::from_format(TRANSLATE_FS("Host", "{}s"), seconds);
    else
      return TinyString(TRANSLATE_SV("Host", "None"));
  }
  else
  {
    if (hours > 0)
      return TRANSLATE_PLURAL_SSTR("Host", "%n hours", "", hours);
    else if (minutes > 0)
      return TRANSLATE_PLURAL_SSTR("Host", "%n minutes", "", minutes);
    else if (seconds > 0)
      return TRANSLATE_PLURAL_SSTR("Host", "%n seconds", "", seconds);
    else
      return TinyString(TRANSLATE_SV("Host", "None"));
  }
}
