// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/string.h"
#include "common/types.h"

#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct WindowInfo;
enum class AudioBackend : u8;
enum class AudioStretchMode : u8;
class AudioStream;
class CDImage;

namespace Host {  
/// Typical durations for OSD messages.
static constexpr float OSD_CRITICAL_ERROR_DURATION = 20.0f;
static constexpr float OSD_ERROR_DURATION = 15.0f;
static constexpr float OSD_WARNING_DURATION = 10.0f;
static constexpr float OSD_INFO_DURATION = 5.0f;
static constexpr float OSD_QUICK_DURATION = 2.5f;

/// Reads a file from the resources directory of the application.
/// This may be outside of the "normal" filesystem on platforms such as Mac.
std::optional<std::vector<u8>> ReadResourceFile(const char* filename);

/// Reads a resource file file from the resources directory as a string.
std::optional<std::string> ReadResourceFileToString(const char* filename);

/// Returns the modified time of a resource.
std::optional<std::time_t> GetResourceFileTimestamp(const char* filename);

/// Translates a string to the current language.
TinyString TranslateString(const char* context, const char* str, const char* disambiguation = nullptr, int n = -1);
std::string TranslateStdString(const char* context, const char* str, const char* disambiguation = nullptr, int n = -1);

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

std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend, u32 sample_rate, u32 channels, u32 buffer_ms,
                                               u32 latency_ms, AudioStretchMode stretch);

/// Returns the scale of OSD elements.
float GetOSDScale();

/// Adds OSD messages, duration is in seconds.
void AddOSDMessage(std::string message, float duration = 2.0f);
void AddKeyedOSDMessage(std::string key, std::string message, float duration = 2.0f);
void AddIconOSDMessage(std::string key, const char* icon, std::string message, float duration = 2.0f);
void AddFormattedOSDMessage(float duration, const char* format, ...);
void AddKeyedFormattedOSDMessage(std::string key, float duration, const char* format, ...);
void RemoveKeyedOSDMessage(std::string key);
void ClearOSDMessages();

/// Displays an asynchronous error on the UI thread, i.e. doesn't block the caller.
void ReportErrorAsync(const std::string_view& title, const std::string_view& message);
void ReportFormattedErrorAsync(const std::string_view& title, const char* format, ...);

/// Displays a synchronous confirmation on the UI thread, i.e. blocks the caller.
bool ConfirmMessage(const std::string_view& title, const std::string_view& message);
bool ConfirmFormattedMessage(const std::string_view& title, const char* format, ...);

/// Debugger feedback.
void ReportDebuggerMessage(const std::string_view& message);
void ReportFormattedDebuggerMessage(const char* format, ...);

/// Displays a loading screen with the logo, rendered with ImGui. Use when executing possibly-time-consuming tasks
/// such as compiling shaders when starting up.
void DisplayLoadingScreen(const char* message, int progress_min = -1, int progress_max = -1, int progress_value = -1);

/// Internal method used by pads to dispatch vibration updates to input sources.
/// Intensity is normalized from 0 to 1.
void SetPadVibrationIntensity(u32 pad_index, float large_or_single_motor_intensity, float small_motor_intensity);

/// Enables "relative" mouse mode, locking the cursor position and returning relative coordinates.
void SetMouseMode(bool relative, bool hide_cursor);

/// Safely executes a function on the VM thread.
void RunOnCPUThread(std::function<void()> function, bool block = false);

/// Opens a URL, using the default application.
void OpenURL(const std::string_view& url);

/// Copies the provided text to the host's clipboard, if present.
bool CopyTextToClipboard(const std::string_view& text);
} // namespace Host

// Helper macros for retrieving translated strings.
#define TRANSLATE(context, msg) Host::TranslateToCString(context, msg)
#define TRANSLATE_SV(context, msg) Host::TranslateToStringView(context, msg)
#define TRANSLATE_STR(context, msg) Host::TranslateToString(context, msg)
#define TRANSLATE_FS(context, msg) fmt::runtime(Host::TranslateToStringView(context, msg))

// Does not translate the string at runtime, but allows the UI to in its own way.
#define TRANSLATE_NOOP(context, msg) msg
