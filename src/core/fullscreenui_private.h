// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "fullscreenui.h"
#include "fullscreenui_widgets.h"
#include "types.h"

namespace GameList {
struct Entry;
}

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

void SwitchToMainWindow(MainWindowType type);
void ReturnToMainWindow();
void ReturnToMainWindow(float transition_time);

void ExitFullscreenAndOpenURL(std::string_view url);
void CopyTextToClipboard(std::string title, std::string_view text);

FileSelectorFilters GetDiscImageFilters();
FileSelectorFilters GetImageFilters();

void SetCoverCacheEntry(std::string path, std::string cover_path);
void ClearCoverCache();

void InitializeHotkeyList();
void ClearSettingsState();
void SwitchToSettings();
bool SwitchToGameSettings(SettingsPage page = SettingsPage::Summary);
void SwitchToGameSettings(const GameList::Entry* entry, SettingsPage page = SettingsPage::Summary);
bool SwitchToGameSettingsForPath(const std::string& path, SettingsPage page = SettingsPage::Summary);
void SwitchToGameSettingsForSerial(std::string_view serial, GameHash hash, SettingsPage page = SettingsPage::Summary);
void DrawSettingsWindow();
SettingsPage GetCurrentSettingsPage();
bool IsInputBindingDialogOpen();

// TODO: Move to widgets or something
inline constexpr const char* DEFAULT_BACKGROUND_NAME = "StaticGray";
inline constexpr const char* NONE_BACKGROUND_NAME = "None";
ChoiceDialogOptions GetBackgroundOptions(const TinyString& current_value);
void LoadBackground();

} // namespace FullscreenUI
