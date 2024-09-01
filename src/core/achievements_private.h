// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "achievements.h"

#include "rc_client.h"

namespace Achievements {

/// Returns the rc_client instance. Should have the lock held.
rc_client_t* GetClient();

const rc_client_user_game_summary_t& GetGameSummary();
const std::string& GetGameIconPath();

std::string GetAchievementBadgePath(const rc_client_achievement_t* achievement, int state,
                                    bool download_if_missing = true);
std::string GetLeaderboardUserBadgePath(const rc_client_leaderboard_entry_t* entry);

void OpenLeaderboard(const rc_client_leaderboard_t* lboard);
bool OpenLeaderboardById(u32 leaderboard_id);
u32 GetOpenLeaderboardId();
bool IsShowingAllLeaderboardEntries();
void FetchNextLeaderboardEntries();

const std::vector<rc_client_leaderboard_entry_list_t*>& GetLeaderboardEntryLists();
const rc_client_leaderboard_entry_list_t* GetLeaderboardNearbyEntries();

void CloseLeaderboard();

} // namespace Achievements
