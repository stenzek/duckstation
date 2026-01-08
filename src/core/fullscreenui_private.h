// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "fullscreenui.h"
#include "fullscreenui_widgets.h"
#include "types.h"

namespace GameList {
struct Entry;
}

#ifndef __ANDROID__

namespace FullscreenUI {

enum class MainWindowType : u8
{
  None,
  Landing,
  StartGame,
  Exit,
  GameList,
  Settings,
  PauseMenu,
  SaveStateSelector,
  Achievements,
  Leaderboards,
};

enum class SettingsPage : u8
{
  Summary,
  Interface,
  GameList,
  Console,
  Emulation,
  BIOS,
  Controller,
  Hotkey,
  MemoryCards,
  Graphics,
  PostProcessing,
  Audio,
  Achievements,
  Advanced,
  Patches,
  Cheats,
  Count
};

//////////////////////////////////////////////////////////////////////////
// Utility
//////////////////////////////////////////////////////////////////////////

void SwitchToMainWindow(MainWindowType type);
void ReturnToMainWindow();
void ReturnToMainWindow(float transition_time);
void ReturnToPreviousWindow();
bool AreAnyDialogsOpen();

void PauseForMenuOpen(bool set_pause_menu_open);
void ClosePauseMenu();
void ClosePauseMenuImmediately();

void ExitFullscreenAndOpenURL(std::string_view url);
void CopyTextToClipboard(std::string title, std::string_view text);

//////////////////////////////////////////////////////////////////////////
// Landing
//////////////////////////////////////////////////////////////////////////

inline constexpr const char* DEFAULT_BACKGROUND_NAME = "StaticGray";
inline constexpr const char* NONE_BACKGROUND_NAME = "None";
ImVec4 GetTransparentBackgroundColor(const ImVec4& no_background_color = UIStyle.BackgroundColor);
ChoiceDialogOptions GetBackgroundOptions(const TinyString& current_value);
void UpdateBackground();

//////////////////////////////////////////////////////////////////////////
// Save State List
//////////////////////////////////////////////////////////////////////////

bool OpenLoadStateSelectorForGameResume(const GameList::Entry* entry);
void OpenSaveStateSelector(const std::string& serial, const std::string& path, bool is_loading);
void DoResume();

//////////////////////////////////////////////////////////////////////////
// Game List
//////////////////////////////////////////////////////////////////////////

bool ShouldOpenToGameList();
FileSelectorFilters GetDiscImageFilters();
FileSelectorFilters GetImageFilters();

void ClearGameListState();
void SwitchToGameList();
void DrawGameListWindow();

void DoStartPath(std::string path, std::string state = std::string(), std::optional<bool> fast_boot = std::nullopt);

GPUTexture* GetCoverForCurrentGame(const std::string& game_path);
void SetCoverCacheEntry(std::string path, std::string cover_path);
void RemoveCoverCacheEntry(const std::string& path);

//////////////////////////////////////////////////////////////////////////
// Settings
//////////////////////////////////////////////////////////////////////////

void ClearSettingsState();
void SwitchToSettings();
bool SwitchToGameSettings(SettingsPage page = SettingsPage::Summary);
void SwitchToGameSettings(const GameList::Entry* entry, SettingsPage page = SettingsPage::Summary);
bool SwitchToGameSettingsForPath(const std::string& path, SettingsPage page = SettingsPage::Summary);
void DrawSettingsWindow();
SettingsPage GetCurrentSettingsPage();
bool IsInputBindingDialogOpen();

//////////////////////////////////////////////////////////////////////////
// Achievements
//////////////////////////////////////////////////////////////////////////

/// Schedules an achievement notification to be shown.
void AddAchievementNotification(std::string key, float duration, std::string image_path, std::string title,
                                std::string text, std::string note);

/// Draws ImGui overlays when ingame.
void DrawAchievementsOverlays();

/// Draws ImGui overlays when paused.
void DrawAchievementsPauseMenuOverlays(float start_pos_y);

/// Updates the stored most-recent and closest-to-completion achievements.
/// Call before calling DrawPauseMenuOverlays() for the first time.
void UpdateAchievementsRecentUnlockAndAlmostThere();

/// Switches to the achievements window.
void SwitchToAchievements();

/// Switches to the leaderboards window.
void SwitchToLeaderboards();

/// Renders the achievement list.
void DrawAchievementsWindow();

/// Renders the leaderboard list.
void DrawLeaderboardsWindow();

} // namespace FullscreenUI

#endif // __ANDROID__
