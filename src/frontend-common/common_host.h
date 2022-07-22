#pragma once
#include "core/system.h"
#include <mutex>

class SettingsInterface;

namespace CommonHost {
/// Initializes configuration.
void UpdateLogSettings();

void Initialize();
void Shutdown();

void SetDefaultSettings(SettingsInterface& si);
void SetDefaultControllerSettings(SettingsInterface& si);
void SetDefaultHotkeyBindings(SettingsInterface& si);
void LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock);
void CheckForSettingsChanges(const Settings& old_settings);
void OnSystemStarting();
void OnSystemStarted();
void OnSystemDestroyed();
void OnSystemPaused();
void OnSystemResumed();
void OnGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name);
void PumpMessagesOnCPUThread();
bool CreateHostDisplayResources();
void ReleaseHostDisplayResources();
} // namespace CommonHost

namespace ImGuiManager {
void RenderDebugWindows();
}

namespace Host {
/// Return the current window handle. Needed for DInput.
void* GetTopLevelWindowHandle();
} // namespace Host