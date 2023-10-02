// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "host.h"

#include "common/assert.h"
#include "common/heterogeneous_containers.h"
#include "common/log.h"
#include "common/string_util.h"

#include <cstdarg>
#include <shared_mutex>

Log_SetChannel(Host);

namespace Host {
static std::pair<const char*, u32> LookupTranslationString(const std::string_view& context,
                                                           const std::string_view& msg);

static constexpr u32 TRANSLATION_STRING_CACHE_SIZE = 4 * 1024 * 1024;
using TranslationStringMap = PreferUnorderedStringMap<std::pair<u32, u32>>;
using TranslationStringContextMap = PreferUnorderedStringMap<TranslationStringMap>;
static std::shared_mutex s_translation_string_mutex;
static TranslationStringContextMap s_translation_string_map;
static std::vector<char> s_translation_string_cache;
static u32 s_translation_string_cache_pos;
} // namespace Host

std::pair<const char*, u32> Host::LookupTranslationString(const std::string_view& context, const std::string_view& msg)
{
  // TODO: TranslatableString, compile-time hashing.

  TranslationStringContextMap::iterator ctx_it;
  TranslationStringMap::iterator msg_it;
  std::pair<const char*, u32> ret;
  s32 len;

  // Shouldn't happen, but just in case someone tries to translate an empty string.
  if (msg.empty()) [[unlikely]]
  {
    ret.first = &s_translation_string_cache[0];
    ret.second = 0;
    return ret;
  }

  s_translation_string_mutex.lock_shared();
  ctx_it = s_translation_string_map.find(context);

  if (ctx_it == s_translation_string_map.end()) [[unlikely]]
    goto add_string;

  msg_it = ctx_it->second.find(msg);
  if (msg_it == ctx_it->second.end()) [[unlikely]]
    goto add_string;

  ret.first = &s_translation_string_cache[msg_it->second.first];
  ret.second = msg_it->second.second;
  s_translation_string_mutex.unlock_shared();
  return ret;

add_string:
  s_translation_string_mutex.unlock_shared();
  s_translation_string_mutex.lock();

  if (s_translation_string_cache.empty()) [[unlikely]]
  {
    // First element is always an empty string.
    s_translation_string_cache.resize(TRANSLATION_STRING_CACHE_SIZE);
    s_translation_string_cache[0] = '\0';
    s_translation_string_cache_pos = 0;
  }

  if ((len =
         Internal::GetTranslatedStringImpl(context, msg, &s_translation_string_cache[s_translation_string_cache_pos],
                                           TRANSLATION_STRING_CACHE_SIZE - 1 - s_translation_string_cache_pos)) < 0)
  {
    Log_ErrorPrint("WARNING: Clearing translation string cache, it might need to be larger.");
    s_translation_string_cache_pos = 0;
    if ((len =
           Internal::GetTranslatedStringImpl(context, msg, &s_translation_string_cache[s_translation_string_cache_pos],
                                             TRANSLATION_STRING_CACHE_SIZE - 1 - s_translation_string_cache_pos)) < 0)
    {
      Panic("Failed to get translated string after clearing cache.");
      len = 0;
    }
  }

  // New context?
  if (ctx_it == s_translation_string_map.end())
    ctx_it = s_translation_string_map.emplace(context, TranslationStringMap()).first;

  // Impl doesn't null terminate, we need that for C strings.
  // TODO: do we want to consider aligning the buffer?
  const u32 insert_pos = s_translation_string_cache_pos;
  s_translation_string_cache[insert_pos + static_cast<u32>(len)] = 0;

  ctx_it->second.emplace(msg, std::pair<u32, u32>(insert_pos, static_cast<u32>(len)));
  s_translation_string_cache_pos = insert_pos + static_cast<u32>(len) + 1;

  ret.first = &s_translation_string_cache[insert_pos];
  ret.second = static_cast<u32>(len);
  s_translation_string_mutex.unlock();
  return ret;
}

const char* Host::TranslateToCString(const std::string_view& context, const std::string_view& msg)
{
  return LookupTranslationString(context, msg).first;
}

std::string_view Host::TranslateToStringView(const std::string_view& context, const std::string_view& msg)
{
  const auto mp = LookupTranslationString(context, msg);
  return std::string_view(mp.first, mp.second);
}

std::string Host::TranslateToString(const std::string_view& context, const std::string_view& msg)
{
  return std::string(TranslateToStringView(context, msg));
}

void Host::ClearTranslationCache()
{
  s_translation_string_mutex.lock();
  s_translation_string_map.clear();
  s_translation_string_cache_pos = 0;
  s_translation_string_mutex.unlock();
}

void Host::ReportFormattedErrorAsync(const std::string_view& title, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string message(StringUtil::StdStringFromFormatV(format, ap));
  va_end(ap);
  ReportErrorAsync(title, message);
}

bool Host::ConfirmFormattedMessage(const std::string_view& title, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string message = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  return ConfirmMessage(title, message);
}
