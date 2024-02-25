// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/types.h"

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Host {
/// Returns true if the specified resource file exists.
bool ResourceFileExists(std::string_view filename, bool allow_override);

/// Reads a file from the resources directory of the application.
/// This may be outside of the "normal" filesystem on platforms such as Mac.
std::optional<std::vector<u8>> ReadResourceFile(std::string_view filename, bool allow_override);

/// Reads a resource file file from the resources directory as a string.
std::optional<std::string> ReadResourceFileToString(std::string_view filename, bool allow_override);

/// Returns the modified time of a resource.
std::optional<std::time_t> GetResourceFileTimestamp(std::string_view filename, bool allow_override);

/// Reports a fatal error on the main thread. This does not assume that the main window exists,
/// unlike ReportErrorAsync(), and will exit the application after the popup is closed.
void ReportFatalError(const std::string_view& title, const std::string_view& message);

/// Displays an asynchronous error on the UI thread, i.e. doesn't block the caller.
void ReportErrorAsync(const std::string_view& title, const std::string_view& message);
void ReportFormattedErrorAsync(const std::string_view& title, const char* format, ...);

/// Displays a synchronous confirmation on the UI thread, i.e. blocks the caller.
bool ConfirmMessage(const std::string_view& title, const std::string_view& message);
bool ConfirmFormattedMessage(const std::string_view& title, const char* format, ...);

/// Returns the user agent to use for HTTP requests.
std::string GetHTTPUserAgent();

/// Opens a URL, using the default application.
void OpenURL(const std::string_view& url);

/// Copies the provided text to the host's clipboard, if present.
bool CopyTextToClipboard(const std::string_view& text);

/// Returns a localized version of the specified string within the specified context.
/// The pointer is guaranteed to be valid until the next language change.
const char* TranslateToCString(const std::string_view& context, const std::string_view& msg);

/// Returns a localized version of the specified string within the specified context.
/// The view is guaranteed to be valid until the next language change.
/// NOTE: When passing this to fmt, positional arguments should be used in the base string, as
/// not all locales follow the same word ordering.
std::string_view TranslateToStringView(const std::string_view& context, const std::string_view& msg);

/// Returns a localized version of the specified string within the specified context.
std::string TranslateToString(const std::string_view& context, const std::string_view& msg);

/// Clears the translation cache. All previously used strings should be considered invalid.
void ClearTranslationCache();

namespace Internal {
/// Implementation to retrieve a translated string.
s32 GetTranslatedStringImpl(const std::string_view& context, const std::string_view& msg, char* tbuf,
                            size_t tbuf_space);
} // namespace Internal
} // namespace Host

// Helper macros for retrieving translated strings.
#define TRANSLATE(context, msg) Host::TranslateToCString(context, msg)
#define TRANSLATE_SV(context, msg) Host::TranslateToStringView(context, msg)
#define TRANSLATE_STR(context, msg) Host::TranslateToString(context, msg)
#define TRANSLATE_FS(context, msg) fmt::runtime(Host::TranslateToStringView(context, msg))

// Does not translate the string at runtime, but allows the UI to in its own way.
#define TRANSLATE_NOOP(context, msg) msg
