#pragma once

#include "common/types.h"
#include <mutex>
#include <string>
#include <vector>

class SettingsInterface;

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