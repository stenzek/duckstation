// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/small_string.h"
#include "common/types.h"

#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Host {

/// Formats a number according to the current locale.
enum class NumberFormatType : u8
{
  ShortDate,     // Date formatting
  LongDate,      // Date formatting
  ShortTime,     // Time formatting
  LongTime,      // Time formatting
  ShortDateTime, // Date and time formatting
  LongDateTime,  // Date and time formatting
  Number,        // Number formatting

  MaxCount,
};
std::string FormatNumber(NumberFormatType type, s64 value);
std::string FormatNumber(NumberFormatType type, double value);

/// Returns a localized version of the specified string within the specified context.
/// The pointer is guaranteed to be valid until the next language change.
const char* TranslateToCString(std::string_view context, std::string_view msg, std::string_view disambiguation = {});

/// Returns a localized version of the specified string within the specified context.
/// The view is guaranteed to be valid until the next language change.
/// NOTE: When passing this to fmt, positional arguments should be used in the base string, as
/// not all locales follow the same word ordering.
std::string_view TranslateToStringView(std::string_view context, std::string_view msg,
                                       std::string_view disambiguation = {});

/// Returns a localized version of the specified string within the specified context.
std::string TranslateToString(std::string_view context, std::string_view msg, std::string_view disambiguation = {});

/// Returns a localized version of the specified string within the specified context, adjusting for plurals using %n.
std::string TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count);
SmallString TranslatePluralToSmallString(const char* context, const char* msg, const char* disambiguation, int count);

/// Clears the translation cache. All previously used strings should be considered invalid.
void ClearTranslationCache();

namespace Internal {
/// Implementation to retrieve a translated string.
s32 GetTranslatedStringImpl(std::string_view context, std::string_view msg, std::string_view disambiguation, char* tbuf,
                            size_t tbuf_space);
} // namespace Internal
} // namespace Host

// Helper macros for retrieving translated strings.
#define TRANSLATE(context, msg) Host::TranslateToCString(context, msg)
#define TRANSLATE_SV(context, msg) Host::TranslateToStringView(context, msg)
#define TRANSLATE_STR(context, msg) Host::TranslateToString(context, msg)
#define TRANSLATE_FS(context, msg) fmt::runtime(Host::TranslateToStringView(context, msg))
#define TRANSLATE_DISAMBIG(context, msg, disambiguation) Host::TranslateToCString(context, msg, disambiguation)
#define TRANSLATE_DISAMBIG_SV(context, msg, disambiguation) Host::TranslateToStringView(context, msg, disambiguation)
#define TRANSLATE_DISAMBIG_STR(context, msg, disambiguation) Host::TranslateToString(context, msg, disambiguation)
#define TRANSLATE_DISAMBIG_FS(context, msg, disambiguation)                                                            \
  fmt::runtime(Host::TranslateToStringView(context, msg, disambiguation))
#define TRANSLATE_PLURAL_STR(context, msg, disambiguation, count)                                                      \
  Host::TranslatePluralToString(context, msg, disambiguation, count)
#define TRANSLATE_PLURAL_SSTR(context, msg, disambiguation, count)                                                     \
  Host::TranslatePluralToSmallString(context, msg, disambiguation, count)
#define TRANSLATE_PLURAL_FS(context, msg, disambiguation, count)                                                       \
  fmt::runtime(Host::TranslatePluralToSmallString(context, msg, disambiguation, count).view())

// Does not translate the string at runtime, but allows the UI to in its own way.
#define TRANSLATE_NOOP(context, msg) msg
#define TRANSLATE_DISAMBIG_NOOP(context, msg, disambiguation) msg
