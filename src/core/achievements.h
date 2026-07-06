// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/small_string.h"
#include "common/types.h"

#include <array>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

class Error;
class ProgressCallback;
class StateWrapper;
class CDImage;

struct Settings;

namespace Achievements {

enum class LoginRequestReason
{
  UserInitiated,
  TokenInvalid,
};

inline constexpr size_t GAME_HASH_LENGTH = 16;
using GameHash = std::array<u8, GAME_HASH_LENGTH>;

class ProgressDatabase
{
public:
  struct Entry
  {
    u32 game_id;
    u16 num_achievements;
    u16 num_achievements_unlocked;
    u16 num_hc_achievements_unlocked;
  };

  ProgressDatabase();
  ~ProgressDatabase();

  bool Load(Error* error);

  const Entry* LookupHash(const GameHash& hash) const;

private:
  struct HashEntry
  {
    GameHash hash;
    u32 game_id;
  };

  std::vector<HashEntry> m_hashes;
  std::vector<Entry> m_entries;
};

/// Acquires the achievements lock. Must be held when accessing any achievement state from another thread.
std::unique_lock<std::recursive_mutex> GetLock();

/// Converts a game hash to a string for display. If the hash is nullopt, returns "[NO HASH]".
TinyString GameHashToString(const std::optional<GameHash>& hash);

/// Updates achievements settings.
void UpdateSettings(const Settings& old_config);

/// Call to refresh the game database.
bool RefreshGameList(ProgressCallback* progress, Error* error);

/// Call to refresh the all-progress database.
bool RefreshAllProgressDatabase(ProgressCallback* progress, Error* error);

/// Called when the system is start. Engages hardcore mode if enabled.
void OnSystemStarting(bool disable_hardcore_mode);

/// Called when the system has finished starting. Identifies the game, assuming the system has set it.
void OnSystemStarted();

/// Called when the system is shutting down. If this returns false, the shutdown should be aborted.
void OnSystemDestroyed();

/// Called when the system is being reset. Resets the internal state of all achievement tracking.
void OnSystemReset();

/// Called when the system changes game.
void SetGameHash(const std::optional<GameHash>& hash);

/// Returns the game hash for the currently loaded game, or nullopt if no hash is set.
std::optional<GameHash> GetGameHash();

/// Called once a frame at vsync time on the CPU thread.
void FrameUpdate();

/// Called when the system is paused, because FrameUpdate() won't be getting called.
void IdleUpdate();

/// Saves/loads state.
bool DoState(StateWrapper& sw);

/// Attempts to log in to RetroAchievements using the specified credentials.
/// If the login is successful, the token returned by the server will be saved.
bool Login(const char* username, const char* password, Error* error);

/// Logs out of RetroAchievements, clearing any credentials.
void Logout();

/// Forces hardcore mode off until next reset.
void DisableHardcoreMode(bool show_message, bool display_game_summary);

/// Prompts the user to disable hardcore mode. Invokes callback with result.
void ConfirmHardcoreModeDisableAsync(std::string_view trigger, std::function<void(bool)> callback);

/// Returns true if hardcore mode is active, and functionality should be restricted.
bool IsHardcoreModeActive();

/// RAIntegration only exists for Windows, so no point checking it on other platforms.
bool IsUsingRAIntegration();
bool IsRAIntegrationAvailable();

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

/// Returns the URL for the current icon of the game
const std::string& GetCurrentGameBadgeURL();

/// Returns the RetroAchievements title for the current game.
/// Should be called with the lock held.
const std::string& GetCurrentGameTitle();

/// Returns true if the user has been successfully logged in.
bool IsLoggedIn();

/// Returns true if the user has been successfully logged in, or the request is in progress.
bool IsLoggedInOrLoggingIn();

/// Returns true if credentials have been saved for login.
bool HasSavedCredentials();

/// Returns the logged-in user name.
const std::string& GetLoggedInUserName();

/// Returns the path to the user's profile avatar.
/// Should be called with the lock held.
const std::string& GetLoggedInUserIconURL();

/// Returns a summary of the user's points.
/// Should be called with the lock held.
SmallString GetLoggedInUserPointsSummary();

/// Returns a URL for the given user's profile.
std::string GetProfileURL(std::string_view username);

/// Returns the URL for the specified game icon, using the game ID.
std::string GetGameBadgeURL(u32 game_id);

/// Returns 0 if pausing is allowed, otherwise the number of frames until pausing is allowed.
u32 GetPauseThrottleFrames();

/// Returns the number of unlocks that have not been synchronized with the server.
u32 GetPendingUnlockCount();

/// The name of the RetroAchievements icon, which can be used in notifications.
extern const char* const RA_LOGO_ICON_NAME;
extern const char* const RA_LOGO_SVG_ICON_NAME;

/// URL for registering accounts.
extern const char* const RA_REGISTER_URL;

} // namespace Achievements

/// Functions implemented in the frontend.
namespace Host {

/// Called if the big picture UI requests achievements login, or token login fails.
void OnAchievementsLoginRequested(Achievements::LoginRequestReason reason);

/// Called when achievements login completes.
void OnAchievementsLoginSuccess(const char* display_name, u32 points, u32 sc_points, u32 unread_messages);

/// Called when achievements login completes or they are disabled.
void OnAchievementsActiveChanged(bool active);

/// Called whenever hardcore mode is toggled.
void OnAchievementsHardcoreModeChanged(bool enabled);

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION

/// Called when the RAIntegration menu changes.
void OnRAIntegrationMenuChanged();

#endif

} // namespace Host
