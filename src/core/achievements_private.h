// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "achievements.h"

#include "rc_client.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Achievements {

inline constexpr float INDICATOR_FADE_IN_TIME = 0.2f;
inline constexpr float INDICATOR_FADE_OUT_TIME = 0.4f;
extern const std::string_view NOTIFICATION_SPINNER_NOTE;

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
  std::string badge_path;
  float time_remaining;
  float opacity;
  bool active;
};

struct AchievementProgressIndicator
{
  const rc_client_achievement_t* achievement;
  std::string badge_path;
  float time;
  bool active;
};

/// Returns the rc_client instance. Should have the lock held.
rc_client_t* GetClient();

const rc_client_user_game_summary_t& GetGameSummary();

std::vector<LeaderboardTrackerIndicator>& GetLeaderboardTrackerIndicators();
std::vector<ActiveChallengeIndicator>& GetActiveChallengeIndicators();
std::optional<AchievementProgressIndicator>& GetActiveProgressIndicator();

std::string GetAchievementBadgePath(const rc_client_achievement_t* achievement, bool locked,
                                    bool download_if_missing = true);
std::string GetLeaderboardUserBadgePath(const rc_client_leaderboard_entry_t* entry);
std::string_view GetLeaderboardFormatIcon(u32 format);

std::string GetSubsetBadgePath(const rc_client_subset_t* subset);

} // namespace Achievements

#ifndef __ANDROID__

namespace FullscreenUI {

/// Clears all cached state used to render the UI.
void ClearAchievementsState();

/// Updates cached data for the last progress update.
void UpdateAchievementsLastProgressUpdate(const rc_client_achievement_t* achievement);

} // namespace FullscreenUI

#endif // __ANDROID__
