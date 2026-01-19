// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "core.h"

class Error;

namespace Core {

/// Based on the current configuration, determines what the data directory is.
bool SetCriticalFolders(const char* resources_subdir, Error* error);

#ifndef __ANDROID__

/// Returns the path to the configuration file.
/// We split this out so it can be retrieved by the host for error message purposes,
/// and so that regtest can override with no-config.
std::string GetBaseSettingsPath();

/// Loads the configuration file.
bool InitializeBaseSettingsLayer(std::string settings_path, Error* error);

/// Saves the configuration file.
bool SaveBaseSettingsLayer(Error* error);

/// Restores default settings.
void SetDefaultSettings(bool host, bool system, bool controller);

#else

/// Sets the base settings layer. Should be called by the host at initialization time.
void SetBaseSettingsLayer(SettingsInterface* sif);

#endif // __ANDROID__

/// Sets the game settings layer. Called by System when the game changes.
void SetGameSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& lock);

/// Sets the input profile settings layer. Called by System when the game changes.
void SetInputSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& lock);

} // namespace Core

namespace Host {

#ifndef __ANDROID__

/// Sets host-specific default settings.
void SetDefaultSettings(SettingsInterface& si);

/// Called when settings have been reset.
void OnSettingsResetToDefault(bool host, bool system, bool controller);

#endif // __ANDROID__

} // namespace Host
