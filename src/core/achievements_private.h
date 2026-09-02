// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "achievements.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

struct rc_client_t;
struct rc_client_achievement_t;
struct rc_client_subset_t;
struct rc_client_user_game_summary_t;

namespace Achievements {

inline constexpr float INDICATOR_FADE_IN_TIME = 0.2f;
inline constexpr float INDICATOR_FADE_OUT_TIME = 0.4f;

struct LeaderboardTrackerIndicator
{
  u32 tracker_id;
  std::string text;
  float time;
  bool active;
};

struct ActiveChallengeIndicator
{
  const rc_client_achievement_t* achievement;
  std::string badge_url;
  float time_remaining;
  float opacity;
  bool active;
};

struct AchievementProgressIndicator
{
  const rc_client_achievement_t* achievement;
  std::string badge_url;
  float time;
  bool active;
};

struct PinnedAchievementIndicator
{
  u32 achievement_id;
  std::string badge_url;
};

/// Returns the rc_client instance. Should have the lock held.
rc_client_t* GetClient();

/// Initializes global state.
void ProcessStartup();

/// Initializes the RetroAchievments client.
void Initialize();

/// Shuts down the RetroAchievements client.
void Shutdown();

/// Returns a summary of the user's points for the current game, including total points.
const rc_client_user_game_summary_t& GetGameSummary();

/// Returns the indicators for active leaderboard trackers. Should be called with the lock held.
std::vector<LeaderboardTrackerIndicator>& GetLeaderboardTrackerIndicators();

/// Returns the indicators for active challenges. Should be called with the lock held.
std::vector<ActiveChallengeIndicator>& GetActiveChallengeIndicators();

/// Returns the indicator for the achievement that has most recently had progress. Should be called with the lock held.
std::optional<AchievementProgressIndicator>& GetActiveProgressIndicator();

/// Returns the indicators for pinned achievements. Should be called with the lock held.
std::vector<PinnedAchievementIndicator>& GetPinnedAchievementIndicators();

/// Returns true if the specified achievement is pinned.
bool IsAchievementPinned(u32 achievement_id);

/// Pins or unpins the specified achievement.
void SetAchievementPinned(u32 achievement_id, bool pinned);

/// Resets a locally tracked unofficial achievement unlock.
void ResetUnofficialAchievementUnlock(u32 achievement_id);

/// Resets all locally tracked unofficial achievement unlocks for the current game.
void ResetAllUnofficialAchievementUnlocks();

/// Returns the URL for the badge of the specified achievement, using the locked or unlocked version as appropriate.
std::string_view GetAchievementBadgeURL(const rc_client_achievement_t* achievement, bool locked);

/// Returns the URL for the badge of the specified achievement, using the locked or unlocked version as appropriate.
std::string_view GetLeaderboardFormatIcon(u32 format);

/// Returns the URL for the badge of the specified game, using the game ID.
std::string GetUserBadgeURL(const char* username);

/// Returns the URL for the badge of the specified subset, using the subset ID.
std::string GetSubsetBadgeURL(const rc_client_subset_t* subset);

} // namespace Achievements

namespace FullscreenUI {

enum class AchievementNotificationNoteType : u8
{
  None,
  Text,
  IconText,
  Spinner,
  Image,
};

enum class AchievementNotificationCategory : u8
{
  None = 0,
  Missable = 1,
  Progression = 2,
  Win = 3,
  TypeMask = 0x0F,
  Unofficial = (1 << 4),
};

IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(AchievementNotificationCategory);

/// Returns the category bitmask for a given achievement.
AchievementNotificationCategory GetAchievementNotificationCategory(const rc_client_achievement_t* achievement);

/// Schedules an achievement notification to be shown.
void AddAchievementNotification(std::string key, float duration, std::string image_url, std::string title,
                                std::string text, std::string note = {},
                                AchievementNotificationNoteType note_type = AchievementNotificationNoteType::None,
                                AchievementNotificationCategory category = AchievementNotificationCategory::None,
                                u16 min_width = 0, bool small_font = false);

/// Clears all cached state used to render the UI.
void ClearAchievementsState();

/// Updates cached data for the last progress update.
void UpdateAchievementsLastProgressUpdate(const rc_client_achievement_t* achievement);

} // namespace FullscreenUI
