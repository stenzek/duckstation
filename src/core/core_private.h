// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "core.h"

namespace Core {

/// Based on the current configuration, determines what the data directory is.
std::string ComputeDataDirectory();

/// Sets the base settings layer. Should be called by the host at initialization time.
void SetBaseSettingsLayer(SettingsInterface* sif);

/// Sets the game settings layer. Called by System when the game changes.
void SetGameSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& lock);

/// Sets the input profile settings layer. Called by System when the game changes.
void SetInputSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& lock);

} // namespace Core
