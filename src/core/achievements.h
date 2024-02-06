// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/small_string.h"
#include "common/types.h"

#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct rc_client_t;

class Error;
class StateWrapper;
class CDImage;

struct Settings;

namespace Achievements {

enum class LoginRequestReason
{
  UserInitiated,
  TokenInvalid,
};

/// Acquires the achievements lock. Must be held when accessing any achievement state from another thread.
std::unique_lock<std::recursive_mutex> GetLock();

/// Returns the rc_client instance. Should have the lock held.
rc_client_t* GetClient();

/// Initializes the RetroAchievments client.
bool Initialize();

/// Updates achievements settings.
void UpdateSettings(const Settings& old_config);

/// Resets the internal state of all achievement tracking. Call on system reset.
void ResetClient();

/// Called when the system is being reset. If it returns false, the reset should be aborted.
bool ConfirmSystemReset();

/// Called when the system is being shut down. If Shutdown() returns false, the shutdown should be aborted.
bool Shutdown(bool allow_cancel);

/// Called when the system is being paused and resumed.
void OnSystemPaused(bool paused);

/// Called once a frame at vsync time on the CPU thread.
void FrameUpdate();

/// Called when the system is paused, because FrameUpdate() won't be getting called.
void IdleUpdate();

/// Returns true if idle updates are necessary (e.g. outstanding requests).
bool NeedsIdleUpdate();

/// Saves/loads state.
bool DoState(StateWrapper& sw);

/// Attempts to log in to RetroAchievements using the specified credentials.
/// If the login is successful, the token returned by the server will be saved.
bool Login(const char* username, const char* password, Error* error);

/// Logs out of RetroAchievements, clearing any credentials.
void Logout();

/// Called when the system changes game, or is booting.
void GameChanged(const std::string& path, CDImage* image);

/// Re-enables hardcore mode if it is enabled in the settings.
bool ResetHardcoreMode();

/// Forces hardcore mode off until next reset.
void DisableHardcoreMode();

/// Prompts the user to disable hardcore mode, if they agree, returns true.
bool ConfirmHardcoreModeDisable(const char* trigger);
void ConfirmHardcoreModeDisableAsync(const char* trigger, std::function<void(bool)> callback);

/// Returns true if hardcore mode is active, and functionality should be restricted.
bool IsHardcoreModeActive();

/// RAIntegration only exists for Windows, so no point checking it on other platforms.
bool IsUsingRAIntegration();

/// Returns true if the achievement system is active. Achievements can be active without a valid client.
bool IsActive();

/// Returns true if RetroAchievements game data has been loaded.
bool HasActiveGame();

/// Returns the RetroAchievements ID for the current game.
u32 GetGameID();

/// Returns true if the current game has any achievements or leaderboards.
bool HasAchievementsOrLeaderboards();

/// Returns true if the current game has any leaderboards.
bool HasAchievements();

/// Returns true if the current game has any leaderboards.
bool HasLeaderboards();

/// Returns true if the game supports rich presence.
bool HasRichPresence();

/// Returns the current rich presence string.
/// Should be called with the lock held.
const std::string& GetRichPresenceString();

/// Returns the RetroAchievements title for the current game.
/// Should be called with the lock held.
const std::string& GetGameTitle();

/// Clears all cached state used to render the UI.
void ClearUIState();

/// Draws ImGui overlays when not paused.
void DrawGameOverlays();

/// Draws ImGui overlays when paused.
void DrawPauseMenuOverlays();

#ifndef __ANDROID__

/// Queries the achievement list, and if no achievements are available, returns false.
bool PrepareAchievementsWindow();

/// Renders the achievement list.
void DrawAchievementsWindow();

/// Queries the leaderboard list, and if no leaderboards are available, returns false.
bool PrepareLeaderboardsWindow();

/// Renders the leaderboard list.
void DrawLeaderboardsWindow();

#endif // __ANDROID__

#ifdef ENABLE_RAINTEGRATION
/// Prevents the internal implementation from being used. Instead, RAIntegration will be
/// called into when achievement-related events occur.
void SwitchToRAIntegration();

namespace RAIntegration {
void MainWindowChanged(void* new_handle);
void GameChanged();
std::vector<std::tuple<int, std::string, bool>> GetMenuItems();
void ActivateMenuItem(int item);
} // namespace RAIntegration
#endif
} // namespace Achievements

/// Functions implemented in the frontend.
namespace Host {
/// Called if the big picture UI requests achievements login, or token login fails.
void OnAchievementsLoginRequested(Achievements::LoginRequestReason reason);

/// Called when achievements login completes.
void OnAchievementsLoginSuccess(const char* display_name, u32 points, u32 sc_points, u32 unread_messages);

/// Called whenever game details or rich presence information is updated.
/// Implementers can assume the lock is held when this is called.
void OnAchievementsRefreshed();

/// Called whenever hardcore mode is toggled.
void OnAchievementsHardcoreModeChanged(bool enabled);
} // namespace Host
