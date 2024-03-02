// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "util/host.h"

#include "common/small_string.h"
#include "common/types.h"

#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
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
bool ContainsBaseSettingValue(const char* section, const char* key);
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



std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend, u32 sample_rate, u32 channels, u32 buffer_ms,
                                               u32 latency_ms, AudioStretchMode stretch);

/// Debugger feedback.
void ReportDebuggerMessage(const std::string_view& message);
void ReportFormattedDebuggerMessage(const char* format, ...);

/// Returns a list of supported languages and codes (suffixes for translation files).
std::span<const std::pair<const char*, const char*>> GetAvailableLanguageList();

/// Refreshes the UI when the language is changed.
bool ChangeLanguage(const char* new_language);

/// Displays a loading screen with the logo, rendered with ImGui. Use when executing possibly-time-consuming tasks
/// such as compiling shaders when starting up.
void DisplayLoadingScreen(const char* message, int progress_min = -1, int progress_max = -1, int progress_value = -1);

/// Safely executes a function on the VM thread.
void RunOnCPUThread(std::function<void()> function, bool block = false);

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
} // namespace Internal
} // namespace Host
