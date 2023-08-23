// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/string.h"
#include "common/types.h"

#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class SettingsInterface;
struct WindowInfo;
enum class AudioBackend : u8;
enum class AudioStretchMode : u8;
enum class RenderAPI : u32;
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

// Base setting retrieval, bypasses layers.
std::string GetBaseStringSettingValue(const char* section, const char* key, const char* default_value = "");
bool GetBaseBoolSettingValue(const char* section, const char* key, bool default_value = false);
s32 GetBaseIntSettingValue(const char* section, const char* key, s32 default_value = 0);
u32 GetBaseUIntSettingValue(const char* section, const char* key, u32 default_value = 0);
float GetBaseFloatSettingValue(const char* section, const char* key, float default_value = 0.0f);
double GetBaseDoubleSettingValue(const char* section, const char* key, double default_value = 0.0);
std::vector<std::string> GetBaseStringListSetting(const char* section, const char* key);

// Allows the emucore to write settings back to the frontend. Use with care.
// You should call CommitBaseSettingChanges() if you directly write to the layer (i.e. not these functions), or it may
// not be written to disk.
void SetBaseBoolSettingValue(const char* section, const char* key, bool value);
void SetBaseIntSettingValue(const char* section, const char* key, s32 value);
void SetBaseUIntSettingValue(const char* section, const char* key, u32 value);
void SetBaseFloatSettingValue(const char* section, const char* key, float value);
void SetBaseStringSettingValue(const char* section, const char* key, const char* value);
void SetBaseStringListSettingValue(const char* section, const char* key, const std::vector<std::string>& values);
bool AddValueToBaseStringListSetting(const char* section, const char* key, const char* value);
bool RemoveValueFromBaseStringListSetting(const char* section, const char* key, const char* value);
void DeleteBaseSettingValue(const char* section, const char* key);
void CommitBaseSettingChanges();

// Settings access, thread-safe.
std::string GetStringSettingValue(const char* section, const char* key, const char* default_value = "");
bool GetBoolSettingValue(const char* section, const char* key, bool default_value = false);
int GetIntSettingValue(const char* section, const char* key, s32 default_value = 0);
u32 GetUIntSettingValue(const char* section, const char* key, u32 default_value = 0);
float GetFloatSettingValue(const char* section, const char* key, float default_value = 0.0f);
double GetDoubleSettingValue(const char* section, const char* key, double default_value = 0.0);
std::vector<std::string> GetStringListSetting(const char* section, const char* key);

/// Direct access to settings interface. Must hold the lock when calling GetSettingsInterface() and while using it.
std::unique_lock<std::mutex> GetSettingsLock();
SettingsInterface* GetSettingsInterface();

/// Returns the settings interface that controller bindings should be loaded from.
/// If an input profile is being used, this will be the input layer, otherwise the layered interface.
SettingsInterface* GetSettingsInterfaceForBindings();

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

/// Enables "relative" mouse mode, locking the cursor position and returning relative coordinates.
void SetMouseMode(bool relative, bool hide_cursor);

/// Safely executes a function on the VM thread.
void RunOnCPUThread(std::function<void()> function, bool block = false);

/// Opens a URL, using the default application.
void OpenURL(const std::string_view& url);

/// Copies the provided text to the host's clipboard, if present.
bool CopyTextToClipboard(const std::string_view& text);

/// Requests shut down and exit of the hosting application. This may not actually exit,
/// if the user cancels the shutdown confirmation.
void RequestExit(bool allow_confirm);

/// Attempts to create the rendering device backend.
bool CreateGPUDevice(RenderAPI api);

/// Handles fullscreen transitions and such.
void UpdateDisplayWindow();

/// Called when the window is resized.
void ResizeDisplayWindow(s32 width, s32 height, float scale);

/// Destroys any active rendering device.
void ReleaseGPUDevice();

/// Called before drawing the OSD and other display elements.
void BeginPresentFrame();

/// Provided by the host; renders the display.
void RenderDisplay(bool skip_present);
void InvalidateDisplay();

namespace Internal {
/// Retrieves the base settings layer. Must call with lock held.
SettingsInterface* GetBaseSettingsLayer();

/// Retrieves the game settings layer, if present. Must call with lock held.
SettingsInterface* GetGameSettingsLayer();

/// Retrieves the input settings layer, if present. Must call with lock held.
SettingsInterface* GetInputSettingsLayer();

/// Sets the base settings layer. Should be called by the host at initialization time.
void SetBaseSettingsLayer(SettingsInterface* sif);

/// Sets the game settings layer. Called by VMManager when the game changes.
void SetGameSettingsLayer(SettingsInterface* sif);

/// Sets the input profile settings layer. Called by VMManager when the game changes.
void SetInputSettingsLayer(SettingsInterface* sif);

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
