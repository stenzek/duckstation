// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/host.h"

#include "common/small_string.h"
#include "common/types.h"

#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Error;
class SettingsInterface;
struct WindowInfo;
enum class AudioBackend : u8;
enum class AudioStretchMode : u8;
enum class RenderAPI : u8;
class AudioStream;
class CDImage;

namespace Host {
// Base setting retrieval, bypasses layers.
std::string GetBaseStringSettingValue(const char* section, const char* key, const char* default_value = "");
SmallString GetBaseSmallStringSettingValue(const char* section, const char* key, const char* default_value = "");
TinyString GetBaseTinyStringSettingValue(const char* section, const char* key, const char* default_value = "");
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
SmallString GetSmallStringSettingValue(const char* section, const char* key, const char* default_value = "");
TinyString GetTinyStringSettingValue(const char* section, const char* key, const char* default_value = "");
bool GetBoolSettingValue(const char* section, const char* key, bool default_value = false);
int GetIntSettingValue(const char* section, const char* key, s32 default_value = 0);
u32 GetUIntSettingValue(const char* section, const char* key, u32 default_value = 0);
float GetFloatSettingValue(const char* section, const char* key, float default_value = 0.0f);
double GetDoubleSettingValue(const char* section, const char* key, double default_value = 0.0);
std::vector<std::string> GetStringListSetting(const char* section, const char* key);

/// Direct access to settings interface. Must hold the lock when calling GetSettingsInterface() and while using it.
std::unique_lock<std::mutex> GetSettingsLock();
SettingsInterface* GetSettingsInterface();

/// Debugger feedback.
void ReportDebuggerMessage(std::string_view message);

/// Returns a list of supported languages and codes (suffixes for translation files).
std::span<const std::pair<const char*, const char*>> GetAvailableLanguageList();

/// Refreshes the UI when the language is changed.
bool ChangeLanguage(const char* new_language);

/// Safely executes a function on the VM thread.
void RunOnCPUThread(std::function<void()> function, bool block = false);

/// Called when the core is creating a render device.
/// This could also be fullscreen transition.
std::optional<WindowInfo> AcquireRenderWindow(RenderAPI render_api, bool fullscreen, bool exclusive_fullscreen,
                                              Error* error);

/// Called when the core is finished with a render window.
void ReleaseRenderWindow();

/// Returns true if the hosting application is currently fullscreen.
bool IsFullscreen();

/// Alters fullscreen state of hosting application.
void SetFullscreen(bool enabled);

namespace Internal {

/// Returns true if the host should use portable mode.
bool ShouldUsePortableMode();

/// Based on the current configuration, determines what the data directory is.
std::string ComputeDataDirectory();

/// Retrieves the base settings layer. Must call with lock held.
SettingsInterface* GetBaseSettingsLayer();

/// Retrieves the game settings layer, if present. Must call with lock held.
SettingsInterface* GetGameSettingsLayer();

/// Retrieves the input settings layer, if present. Must call with lock held.
SettingsInterface* GetInputSettingsLayer();

/// Sets the base settings layer. Should be called by the host at initialization time.
void SetBaseSettingsLayer(SettingsInterface* sif);

/// Sets the game settings layer. Called by VMManager when the game changes.
void SetGameSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& lock);

/// Sets the input profile settings layer. Called by VMManager when the game changes.
void SetInputSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& lock);

} // namespace Internal

} // namespace Host
