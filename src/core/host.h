// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/heap_array.h"
#include "common/types.h"

#include <ctime>
#include <functional>
#include <span>
#include <string_view>

class Error;

namespace Host {

/// Returns true if the specified resource file exists.
bool ResourceFileExists(std::string_view filename, bool allow_override);

/// Reads a file from the resources directory of the application.
/// This may be outside of the "normal" filesystem on platforms such as Mac.
std::optional<DynamicHeapArray<u8>> ReadResourceFile(std::string_view filename, bool allow_override,
                                                     Error* error = nullptr);

/// Reads a resource file file from the resources directory as a string.
std::optional<std::string> ReadResourceFileToString(std::string_view filename, bool allow_override,
                                                    Error* error = nullptr);

/// Returns the modified time of a resource.
std::optional<std::time_t> GetResourceFileTimestamp(std::string_view filename, bool allow_override);

/// Reports a fatal error on the main thread. This does not assume that the main window exists,
/// unlike ReportErrorAsync(), and will exit the application after the popup is closed.
void ReportFatalError(std::string_view title, std::string_view message);

/// Displays an asynchronous error on the UI thread, i.e. doesn't block the caller.
void ReportErrorAsync(std::string_view title, std::string_view message);

/// Debugger feedback.
void ReportDebuggerMessage(std::string_view message);

/// Displays an asynchronous confirmation on the UI thread, but does not block the caller.
/// The callback may be executed on a different thread. Use RunOnCPUThread() in the callback to ensure safety.
using ConfirmMessageAsyncCallback = std::function<void(bool)>;
void ConfirmMessageAsync(std::string_view title, std::string_view message, ConfirmMessageAsyncCallback callback,
                         std::string_view yes_text = std::string_view(), std::string_view no_text = std::string_view());

/// Opens a URL, using the default application.
void OpenURL(std::string_view url);

/// Returns the current contents of the clipboard as UTF-8 text, if any.
std::string GetClipboardText();

/// Copies the provided UTF-8 text to the host's clipboard, if present.
bool CopyTextToClipboard(std::string_view text);

/// Returns a list of supported languages and codes (suffixes for translation files).
std::span<const std::pair<const char*, const char*>> GetAvailableLanguageList();

/// Returns the localized language name for the specified language code.
const char* GetLanguageName(std::string_view language_code);

/// Refreshes the UI when the language is changed.
bool ChangeLanguage(const char* new_language);

/// Safely executes a function on the VM thread.
void RunOnCPUThread(std::function<void()> function, bool block = false);

/// Safely executes a function on the main/UI thread.
void RunOnUIThread(std::function<void()> function, bool block = false);

/// Commits any changes made to the base settings layer to the host.
void CommitBaseSettingChanges();

} // namespace Host
