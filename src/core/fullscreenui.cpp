// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "fullscreenui.h"
#include "achievements.h"
#include "bios.h"
#include "cheats.h"
#include "controller.h"
#include "fullscreenui_private.h"
#include "fullscreenui_widgets.h"
#include "game_list.h"
#include "gpu.h"
#include "gpu_backend.h"
#include "gpu_presenter.h"
#include "gpu_thread.h"
#include "gte_types.h"
#include "host.h"
#include "imgui_overlays.h"
#include "settings.h"
#include "system.h"
#include "system_private.h"

#include "scmversion/scmversion.h"

#include "util/cd_image.h"
#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/ini_settings_interface.h"
#include "util/input_manager.h"
#include "util/postprocessing.h"
#include "util/shadergen.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/progress_callback.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome6.h"
#include "IconsPromptFont.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <atomic>
#include <unordered_map>
#include <utility>
#include <vector>

LOG_CHANNEL(FullscreenUI);

#ifndef __ANDROID__

namespace FullscreenUI {

enum class PauseSubMenu : u8
{
  None,
  Exit,
  Achievements,
};

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////
static void PauseForMenuOpen(bool set_pause_menu_open);
static void ClosePauseMenu();
static void ClosePauseMenuImmediately();
static void DrawLandingTemplate(ImVec2* menu_pos, ImVec2* menu_size);
static void DrawLandingWindow();
static void DrawStartGameWindow();
static void DrawExitWindow();
static void DrawPauseMenu();
static void DrawAboutWindow();
static void FixStateIfPaused();

//////////////////////////////////////////////////////////////////////////
// Backgrounds
//////////////////////////////////////////////////////////////////////////

static bool HasBackground();
static bool LoadBackgroundShader(const std::string& path, Error* error);
static bool LoadBackgroundImage(const std::string& path, Error* error);
static void DrawBackground();
static void DrawShaderBackgroundCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd);

//////////////////////////////////////////////////////////////////////////
// Resources
//////////////////////////////////////////////////////////////////////////
static void LoadResources();
static void DestroyResources();
static GPUTexture* GetUserThemeableTexture(
  const std::string_view png_name, const std::string_view svg_name, bool* is_colorable = nullptr,
  const ImVec2& svg_size = LayoutScale(LAYOUT_HORIZONTAL_MENU_ITEM_IMAGE_SIZE, LAYOUT_HORIZONTAL_MENU_ITEM_IMAGE_SIZE));
static bool UserThemeableHorizontalButton(const std::string_view png_name, const std::string_view svg_name,
                                          std::string_view title, std::string_view description);
static void UpdateCurrentTimeString();

//////////////////////////////////////////////////////////////////////////
// Landing
//////////////////////////////////////////////////////////////////////////
static void DoStartFile();
static void DoStartBIOS();
static void DoStartDisc();
static void DoToggleFastForward();
static void ConfirmIfSavingMemoryCards(std::string action, std::function<void(bool)> callback);
static void RequestShutdown(bool save_state);
static void RequestReset();
static void BeginChangeDiscOnCPUThread(bool needs_pause);
static void StartChangeDiscFromFile();
static void DoRequestExit();
static void DoDesktopMode();
static void DoToggleFullscreen();
static void DoToggleAnalogMode();

//////////////////////////////////////////////////////////////////////////
// Save State List
//////////////////////////////////////////////////////////////////////////
struct SaveStateListEntry
{
  std::string title;
  std::string summary;
  std::string game_path;
  std::string state_path;
  std::unique_ptr<GPUTexture> preview_texture;
  time_t timestamp;
  s32 slot;
  bool global;
};

static void InitializePlaceholderSaveStateListEntry(SaveStateListEntry* li, s32 slot, bool global);
static bool InitializeSaveStateListEntryFromSerial(SaveStateListEntry* li, const std::string& serial, s32 slot,
                                                   bool global);
static bool InitializeSaveStateListEntryFromPath(SaveStateListEntry* li, std::string path, s32 slot, bool global);
static void ClearSaveStateEntryList();
static u32 PopulateSaveStateListEntries(const std::string& serial, std::optional<ExtendedSaveStateInfo> undo_save_state,
                                        bool is_loading);
static void DrawSaveStateSelector();
static void DrawResumeStateSelector();

//////////////////////////////////////////////////////////////////////////
// Achievements/Leaderboards
//////////////////////////////////////////////////////////////////////////
static void SwitchToAchievements();
static void SwitchToLeaderboards();

//////////////////////////////////////////////////////////////////////////
// Constants
//////////////////////////////////////////////////////////////////////////

static constexpr std::string_view RESUME_STATE_SELECTOR_DIALOG_NAME = "##resume_state_selector";
static constexpr std::string_view ABOUT_DIALOG_NAME = "##about_duckstation";

//////////////////////////////////////////////////////////////////////////
// State
//////////////////////////////////////////////////////////////////////////

namespace {

struct ALIGN_TO_CACHE_LINE WidgetsState
{
  // Main
  MainWindowType current_main_window = MainWindowType::None;
  PauseSubMenu current_pause_submenu = PauseSubMenu::None;
  MainWindowType previous_main_window = MainWindowType::None;
  bool initialized = false;
  bool pause_menu_was_open = false;
  bool was_paused_on_quick_menu_open = false;

  // Resources
  std::shared_ptr<GPUTexture> app_icon_texture;
  
  // Background
  std::unique_ptr<GPUTexture> app_background_texture;
  std::unique_ptr<GPUPipeline> app_background_shader;
  Timer::Value app_background_load_time = 0;

  // Pause Menu
  std::time_t current_time = 0;
  std::string current_time_string;

  // Save State List
  std::vector<SaveStateListEntry> save_state_selector_slots;
  bool save_state_selector_loading = true;
};

} // namespace

static WidgetsState s_state;

} // namespace FullscreenUI

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::Initialize()
{
  // some achievement callbacks fire early while e.g. there is a load state popup blocking system init
  if (s_state.initialized || !ImGuiManager::IsInitialized())
    return;

  s_state.initialized = true;

  LoadResources();
  LoadBackground();

  // in case we open the pause menu while the game is running
  if (s_state.current_main_window == MainWindowType::None && !GPUThread::HasGPUBackend() &&
      !GPUThread::IsGPUBackendRequested())
  {
    ReturnToMainWindow();
    ForceKeyNavEnabled();
  }
  else
  {
    UpdateRunIdleState();
  }

  INFO_LOG("Fullscreen UI initialized.");
}

bool FullscreenUI::IsInitialized()
{
  return s_state.initialized;
}

bool FullscreenUI::HasActiveWindow()
{
  return s_state.initialized && (s_state.current_main_window != MainWindowType::None ||
                                 GetTransitionState() != TransitionState::Inactive || AreAnyDialogsOpen());
}

bool FullscreenUI::AreAnyDialogsOpen()
{
  return (IsInputBindingDialogOpen() || IsAnyFixedPopupDialogOpen() || IsChoiceDialogOpen() || IsInputDialogOpen() ||
          IsFileSelectorOpen() || IsMessageBoxDialogOpen());
}

void FullscreenUI::CheckForConfigChanges(const GPUSettings& old_settings)
{
  // NOTE: Called on GPU thread.
}

void FullscreenUI::UpdateRunIdleState()
{
  GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::FullscreenUIActive, HasActiveWindow());
}

void FullscreenUI::OnSystemStarting()
{
  // NOTE: Called on CPU thread.
  if (!IsInitialized())
    return;

  GPUThread::RunOnThread([]() {
    if (!IsInitialized())
      return;

    BeginTransition(LONG_TRANSITION_TIME, []() {
      ClearSaveStateEntryList();
      s_state.current_main_window = MainWindowType::None;
      QueueResetFocus(FocusResetType::ViewChanged);
      UpdateRunIdleState();
    });
  });
}

void FullscreenUI::OnSystemPaused()
{
  // NOTE: Called on CPU thread.
  if (!IsInitialized())
    return;

  GPUThread::RunOnThread([]() {
    if (!IsInitialized())
      return;

    UpdateRunIdleState();
  });
}

void FullscreenUI::OnSystemResumed()
{
  // NOTE: Called on CPU thread.
  if (!IsInitialized())
    return;

  GPUThread::RunOnThread([]() {
    if (!IsInitialized())
      return;

    // get rid of pause menu if we unpaused another way
    if (s_state.current_main_window == MainWindowType::PauseMenu)
      ClosePauseMenuImmediately();

    UpdateRunIdleState();
  });
}

void FullscreenUI::OnSystemDestroyed()
{
  // NOTE: Called on CPU thread.
  if (!IsInitialized())
    return;

  GPUThread::RunOnThread([]() {
    if (!IsInitialized())
      return;

    s_state.pause_menu_was_open = false;
    s_state.was_paused_on_quick_menu_open = false;
    s_state.current_pause_submenu = PauseSubMenu::None;
    ReturnToMainWindow(LONG_TRANSITION_TIME);
  });
}

void FullscreenUI::PauseForMenuOpen(bool set_pause_menu_open)
{
  s_state.was_paused_on_quick_menu_open = GPUThread::IsSystemPaused();
  if (!s_state.was_paused_on_quick_menu_open)
    Host::RunOnCPUThread([]() { System::PauseSystem(true); });

  s_state.pause_menu_was_open |= set_pause_menu_open;
}

void FullscreenUI::OpenPauseMenu()
{
  if (!System::IsValid())
    return;

  GPUThread::RunOnThread([]() {
    Initialize();
    if (s_state.current_main_window != MainWindowType::None)
      return;

    PauseForMenuOpen(true);
    ForceKeyNavEnabled();

    Achievements::UpdateRecentUnlockAndAlmostThere();
    BeginTransition(SHORT_TRANSITION_TIME, []() {
      s_state.current_pause_submenu = PauseSubMenu::None;
      SwitchToMainWindow(MainWindowType::PauseMenu);
    });
  });
}

void FullscreenUI::OpenCheatsMenu()
{
  if (!System::IsValid())
    return;

  GPUThread::RunOnThread([]() {
    Initialize();
    if (s_state.current_main_window != MainWindowType::None)
      return;

    PauseForMenuOpen(false);
    ForceKeyNavEnabled();

    BeginTransition(SHORT_TRANSITION_TIME, []() {
      if (!SwitchToGameSettings(SettingsPage::Cheats))
        ClosePauseMenuImmediately();
    });
  });
}

void FullscreenUI::OpenDiscChangeMenu()
{
  if (!System::IsValid())
    return;

  DebugAssert(!GPUThread::IsOnThread());
  BeginChangeDiscOnCPUThread(true);
}

void FullscreenUI::FixStateIfPaused()
{
  if (!GPUThread::HasGPUBackend() || System::IsRunning())
    return;

  // When we're paused, we won't have trickled the key up event for escape yet. Do it now.
  ImGui::UpdateInputEvents(false);
}

void FullscreenUI::ClosePauseMenu()
{
  if (!GPUThread::HasGPUBackend())
    return;

  if (GPUThread::IsSystemPaused() && !s_state.was_paused_on_quick_menu_open)
    Host::RunOnCPUThread([]() { System::PauseSystem(false); });

  BeginTransition(SHORT_TRANSITION_TIME, []() {
    s_state.current_pause_submenu = PauseSubMenu::None;
    s_state.pause_menu_was_open = false;
    SwitchToMainWindow(MainWindowType::None);
  });
}

void FullscreenUI::ClosePauseMenuImmediately()
{
  if (!GPUThread::HasGPUBackend())
    return;

  CancelTransition();

  if (GPUThread::IsSystemPaused() && !s_state.was_paused_on_quick_menu_open)
    Host::RunOnCPUThread([]() { System::PauseSystem(false); });

  s_state.current_pause_submenu = PauseSubMenu::None;
  s_state.pause_menu_was_open = false;
  SwitchToMainWindow(MainWindowType::None);

  // Present frame with menu closed. We have to defer this for a frame so imgui loses keyboard focus.
  if (GPUThread::IsSystemPaused())
    GPUThread::PresentCurrentFrame();
}

void FullscreenUI::SwitchToMainWindow(MainWindowType type)
{
  if (s_state.current_main_window == type)
    return;

  s_state.previous_main_window = (type == MainWindowType::None) ? MainWindowType::None : s_state.current_main_window;
  s_state.current_main_window = type;
  if (!AreAnyDialogsOpen())
  {
    ImGui::SetWindowFocus(nullptr);
    QueueResetFocus(FocusResetType::ViewChanged);
  }

  UpdateRunIdleState();
  FixStateIfPaused();
}

void FullscreenUI::ReturnToPreviousWindow()
{
  if (s_state.previous_main_window == MainWindowType::None)
  {
    ReturnToMainWindow();
  }
  else
  {
    BeginTransition([window = s_state.previous_main_window]() {
      SwitchToMainWindow(window);

      // return stack is only one deep
      s_state.previous_main_window = window;
    });
  }
}

void FullscreenUI::ReturnToMainWindow()
{
  ReturnToMainWindow(GPUThread::HasGPUBackend() ? SHORT_TRANSITION_TIME : DEFAULT_TRANSITION_TIME);
}

void FullscreenUI::ReturnToMainWindow(float transition_time)
{
  if (GPUThread::IsSystemPaused() && !s_state.was_paused_on_quick_menu_open)
    Host::RunOnCPUThread([]() { System::PauseSystem(false); });

  BeginTransition(transition_time, []() {
    s_state.previous_main_window = MainWindowType::None;
    s_state.current_pause_submenu = PauseSubMenu::None;
    s_state.pause_menu_was_open = false;

    if (GPUThread::HasGPUBackend())
    {
      SwitchToMainWindow(MainWindowType::None);
    }
    else
    {
      if (ShouldOpenToGameList())
        SwitchToGameList();
      else
        SwitchToMainWindow(MainWindowType::Landing);
    }
  });
}

void FullscreenUI::Shutdown(bool clear_state)
{
  if (clear_state)
  {
    s_state.current_main_window = MainWindowType::None;
    s_state.current_pause_submenu = PauseSubMenu::None;
    s_state.pause_menu_was_open = false;
    s_state.was_paused_on_quick_menu_open = false;

    Achievements::ClearUIState();
    ClearSaveStateEntryList();
    ClearSettingsState();
    ClearGameListState();
    s_state.current_time_string = {};
    s_state.current_time = 0;
  }

  DestroyResources();

  s_state.initialized = false;
  UpdateRunIdleState();
}

void FullscreenUI::Render()
{
  if (!s_state.initialized)
  {
    RenderLoadingScreen();
    return;
  }

  UploadAsyncTextures();

  // draw background before any overlays
  if (!GPUThread::HasGPUBackend() && s_state.current_main_window != MainWindowType::None)
    DrawBackground();

  BeginLayout();

  // Primed achievements must come first, because we don't want the pause screen to be behind them.
  if (s_state.current_main_window == MainWindowType::None)
    Achievements::DrawGameOverlays();

  switch (s_state.current_main_window)
  {
    case MainWindowType::Landing:
      DrawLandingWindow();
      break;
    case MainWindowType::StartGame:
      DrawStartGameWindow();
      break;
    case MainWindowType::Exit:
      DrawExitWindow();
      break;
    case MainWindowType::GameList:
      DrawGameListWindow();
      break;
    case MainWindowType::Settings:
      DrawSettingsWindow();
      break;
    case MainWindowType::PauseMenu:
      DrawPauseMenu();
      break;
    case MainWindowType::SaveStateSelector:
      DrawSaveStateSelector();
      break;
    case MainWindowType::Achievements:
      Achievements::DrawAchievementsWindow();
      break;
    case MainWindowType::Leaderboards:
      Achievements::DrawLeaderboardsWindow();
      break;
    default:
      break;
  }

  if (IsFixedPopupDialogOpen(ABOUT_DIALOG_NAME))
    DrawAboutWindow();
  else if (IsFixedPopupDialogOpen(RESUME_STATE_SELECTOR_DIALOG_NAME))
    DrawResumeStateSelector();

  EndLayout();

  RenderLoadingScreen();

  ResetCloseMenuIfNeeded();
  UpdateTransitionState();
}

void FullscreenUI::InvalidateCoverCache()
{
  if (!GPUThread::IsFullscreenUIRequested())
    return;

  GPUThread::RunOnThread(&FullscreenUI::ClearCoverCache);
}

void FullscreenUI::LoadResources()
{
  s_state.app_icon_texture = LoadTexture("images/duck.png");

  InitializeHotkeyList();
}

void FullscreenUI::DestroyResources()
{
  s_state.app_background_texture.reset();
  s_state.app_background_shader.reset();
  s_state.app_icon_texture.reset();
}

GPUTexture* FullscreenUI::GetUserThemeableTexture(const std::string_view png_name, const std::string_view svg_name,
                                                  bool* is_colorable, const ImVec2& svg_size)
{
  GPUTexture* tex = FindCachedTexture(png_name);
  if (tex)
  {
    if (is_colorable)
      *is_colorable = false;

    return tex;
  }

  const u32 svg_width = static_cast<u32>(svg_size.x);
  const u32 svg_height = static_cast<u32>(svg_size.y);
  tex = FindCachedTexture(svg_name, svg_width, svg_height);
  if (tex)
  {
    if (is_colorable)
      *is_colorable = true;

    return tex;
  }

  // slow path, check filesystem for override
  if (EmuFolders::Resources != EmuFolders::UserResources &&
      FileSystem::FileExists(Path::Combine(EmuFolders::UserResources, png_name).c_str()))
  {
    // use the user's png
    if (is_colorable)
      *is_colorable = false;

    return GetCachedTexture(png_name);
  }

  // otherwise use the system/user svg
  if (is_colorable)
    *is_colorable = true;

  return GetCachedTexture(svg_name, svg_width, svg_height);
}

bool FullscreenUI::UserThemeableHorizontalButton(const std::string_view png_name, const std::string_view svg_name,
                                                 std::string_view title, std::string_view description)
{
  bool is_colorable;
  GPUTexture* icon = GetUserThemeableTexture(
    png_name, svg_name, &is_colorable,
    LayoutScale(LAYOUT_HORIZONTAL_MENU_ITEM_IMAGE_SIZE, LAYOUT_HORIZONTAL_MENU_ITEM_IMAGE_SIZE));
  return HorizontalMenuItem(icon, title, description,
                            is_colorable ? ImGui::GetColorU32(ImGuiCol_Text) : IM_COL32(255, 255, 255, 255));
}

void FullscreenUI::UpdateCurrentTimeString()
{
  const std::time_t current_time = std::time(nullptr);
  if (s_state.current_time == current_time)
    return;

  s_state.current_time = current_time;
  s_state.current_time_string = {};
  s_state.current_time_string = Host::FormatNumber(Host::NumberFormatType::ShortTime, static_cast<s64>(current_time));
}

//////////////////////////////////////////////////////////////////////////
// Utility
//////////////////////////////////////////////////////////////////////////


void FullscreenUI::DoStartPath(std::string path, std::string state, std::optional<bool> fast_boot)
{
  if (GPUThread::HasGPUBackend())
    return;

  // Stop running idle to prevent game list from being redrawn until we know if startup succeeded.
  GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::FullscreenUIActive, false);

  SystemBootParameters params;
  params.path = std::move(path);
  params.save_state = std::move(state);
  params.override_fast_boot = std::move(fast_boot);
  Host::RunOnCPUThread([params = std::move(params)]() mutable {
    if (System::IsValid())
      return;

    // This can "fail" if HC mode is enabled and the user cancels, or other startup cancel paths.
    Error error;
    if (!System::BootSystem(std::move(params), &error))
    {
      GPUThread::RunOnThread([error_desc = error.TakeDescription()]() {
        if (!IsInitialized())
          return;

        OpenInfoMessageDialog(TRANSLATE_STR("System", "Error"),
                              fmt::format(TRANSLATE_FS("System", "Failed to boot system: {}"), error_desc));
        ClearSaveStateEntryList();
        UpdateRunIdleState();
      });
    }
  });
}

void FullscreenUI::DoResume()
{
  std::string path = System::GetMostRecentResumeSaveStatePath();
  if (path.empty())
  {
    ShowToast({}, FSUI_STR("No resume save state found."));
    return;
  }

  SaveStateListEntry slentry;
  if (!InitializeSaveStateListEntryFromPath(&slentry, std::move(path), -1, false))
    return;

  ClearSaveStateEntryList();
  s_state.save_state_selector_slots.push_back(std::move(slentry));
  OpenFixedPopupDialog(RESUME_STATE_SELECTOR_DIALOG_NAME);
}

void FullscreenUI::DoStartFile()
{
  auto callback = [](const std::string& path) {
    if (!path.empty())
      DoStartPath(path);
  };

  OpenFileSelector(FSUI_ICONVSTR(ICON_EMOJI_OPTICAL_DISK, "Select Disc Image"), false, std::move(callback),
                   GetDiscImageFilters());
}

void FullscreenUI::DoStartBIOS()
{
  DoStartPath(std::string(), std::string(), std::nullopt);
}

void FullscreenUI::DoStartDisc()
{
  std::vector<std::pair<std::string, std::string>> devices = CDImage::GetDeviceList();
  if (devices.empty())
  {
    ShowToast(std::string(),
              FSUI_STR("Could not find any CD/DVD-ROM devices. Please ensure you have a drive connected and sufficient "
                       "permissions to access it."));
    return;
  }

  // if there's only one, select it automatically
  if (devices.size() == 1)
  {
    DoStartPath(std::move(devices.front().first), std::string(), std::nullopt);
    return;
  }

  ChoiceDialogOptions options;
  std::vector<std::string> paths;
  options.reserve(devices.size());
  paths.reserve(paths.size());
  for (auto& [path, name] : devices)
  {
    options.emplace_back(std::move(name), false);
    paths.push_back(std::move(path));
  }
  OpenChoiceDialog(FSUI_ICONVSTR(ICON_FA_COMPACT_DISC, "Select Disc Drive"), false, std::move(options),
                   [paths = std::move(paths)](s32 index, const std::string&, bool) mutable {
                     if (index < 0)
                       return;

                     DoStartPath(std::move(paths[index]), std::string(), std::nullopt);
                   });
}

void FullscreenUI::ConfirmIfSavingMemoryCards(std::string action, std::function<void(bool)> callback)
{
  Host::RunOnCPUThread([action = std::move(action), callback = std::move(callback)]() mutable {
    const bool was_saving = System::IsSavingMemoryCards();
    GPUThread::RunOnThread([action = std::move(action), callback = std::move(callback), was_saving]() mutable {
      if (!was_saving)
      {
        callback(true);
        return;
      }

      OpenConfirmMessageDialog(
        FSUI_ICONVSTR(ICON_PF_MEMORY_CARD, "Memory Card Busy"),
        fmt::format(
          FSUI_FSTR("WARNING: Your game is still saving to the memory card. Continuing to {0} may IRREVERSIBLY "
                    "DESTROY YOUR MEMORY CARD. We recommend resuming your game and waiting 5 seconds for it to "
                    "finish saving.\n\nDo you want to {0} anyway?"),
          action),
        std::move(callback),
        fmt::format(
          fmt::runtime(FSUI_ICONVSTR(ICON_FA_TRIANGLE_EXCLAMATION, "Yes, {} now and risk memory card corruption.")),
          action),
        FSUI_ICONSTR(ICON_FA_PLAY, "No, resume the game."));
    });
  });
}

void FullscreenUI::RequestShutdown(bool save_state)
{
  SwitchToMainWindow(MainWindowType::None);

  ConfirmIfSavingMemoryCards(FSUI_STR("shut down"), [save_state](bool result) {
    if (result)
      Host::RunOnCPUThread([save_state]() { Host::RequestSystemShutdown(false, save_state, false); });
    else
      ClosePauseMenuImmediately();
  });
}

void FullscreenUI::RequestReset()
{
  SwitchToMainWindow(MainWindowType::None);

  ConfirmIfSavingMemoryCards(FSUI_STR("reset"), [](bool result) {
    if (result)
      Host::RunOnCPUThread(System::ResetSystem);

    BeginTransition(LONG_TRANSITION_TIME, &ClosePauseMenuImmediately);
  });
}

void FullscreenUI::DoToggleFastForward()
{
  Host::RunOnCPUThread([]() {
    if (!System::IsValid())
      return;

    System::SetFastForwardEnabled(!System::IsFastForwardEnabled());
  });
}

void FullscreenUI::StartChangeDiscFromFile()
{
  auto callback = [](const std::string& path) {
    if (path.empty())
    {
      ReturnToPreviousWindow();
      return;
    }

    ConfirmIfSavingMemoryCards(FSUI_STR("change disc"), [path](bool result) {
      if (result)
      {
        if (!GameList::IsScannableFilename(path))
        {
          ShowToast({},
                    fmt::format(FSUI_FSTR("{} is not a valid disc image."), FileSystem::GetDisplayNameFromPath(path)));
        }
        else
        {
          Host::RunOnCPUThread([path]() { System::InsertMedia(path.c_str()); });
        }
      }

      ReturnToMainWindow();
    });
  };

  OpenFileSelector(FSUI_ICONVSTR(ICON_FA_COMPACT_DISC, "Select Disc Image"), false, std::move(callback),
                   GetDiscImageFilters(), std::string(Path::GetDirectory(GPUThread::GetGamePath())));
}

void FullscreenUI::BeginChangeDiscOnCPUThread(bool needs_pause)
{
  ChoiceDialogOptions options;

  auto pause_if_needed = [needs_pause]() {
    if (!needs_pause)
      return;

    PauseForMenuOpen(false);
    ForceKeyNavEnabled();
    UpdateRunIdleState();
    FixStateIfPaused();
  };

  if (System::HasMediaSubImages())
  {
    const u32 current_index = System::GetMediaSubImageIndex();
    const u32 count = System::GetMediaSubImageCount();
    options.reserve(count + 1);
    options.emplace_back(FSUI_STR("From File..."), false);

    for (u32 i = 0; i < count; i++)
      options.emplace_back(System::GetMediaSubImageTitle(i), i == current_index);

    GPUThread::RunOnThread([options = std::move(options), pause_if_needed = std::move(pause_if_needed)]() mutable {
      Initialize();

      auto callback = [](s32 index, const std::string& title, bool checked) {
        if (index == 0)
        {
          StartChangeDiscFromFile();
        }
        else if (index > 0)
        {
          ConfirmIfSavingMemoryCards(FSUI_STR("change disc"), [index](bool result) {
            if (result)
              System::SwitchMediaSubImage(static_cast<u32>(index - 1));

            ReturnToPreviousWindow();
          });
        }
        else
        {
          ReturnToPreviousWindow();
        }
      };

      OpenChoiceDialog(FSUI_ICONVSTR(ICON_FA_COMPACT_DISC, "Select Disc Image"), false, std::move(options),
                       std::move(callback));
      pause_if_needed();
    });

    return;
  }

  if (const GameDatabase::Entry* entry = System::GetGameDatabaseEntry(); entry && entry->disc_set)
  {
    const auto lock = GameList::GetLock();
    auto matches = GameList::GetEntriesInDiscSet(entry->disc_set, GameList::ShouldShowLocalizedTitles());
    if (matches.size() > 1)
    {
      options.reserve(matches.size() + 1);
      options.emplace_back(FSUI_STR("From File..."), false);

      std::vector<std::string> paths;
      paths.reserve(matches.size());

      const std::string& current_path = System::GetGamePath();
      for (auto& [title, glentry] : matches)
      {
        options.emplace_back(std::move(title), current_path == glentry->path);
        paths.push_back(glentry->path);
      }

      GPUThread::RunOnThread([options = std::move(options), paths = std::move(paths),
                              pause_if_needed = std::move(pause_if_needed)]() mutable {
        Initialize();

        auto callback = [paths = std::move(paths)](s32 index, const std::string& title, bool checked) mutable {
          if (index == 0)
          {
            StartChangeDiscFromFile();
          }
          else if (index > 0)
          {
            ConfirmIfSavingMemoryCards(FSUI_STR("change disc"), [paths = std::move(paths), index](bool result) {
              if (result)
                Host::RunOnCPUThread([path = std::move(paths[index - 1])]() { System::InsertMedia(path.c_str()); });

              ReturnToPreviousWindow();
            });
          }
          else
          {
            ReturnToPreviousWindow();
          }
        };

        OpenChoiceDialog(FSUI_ICONVSTR(ICON_FA_COMPACT_DISC, "Select Disc Image"), false, std::move(options),
                         std::move(callback));
        pause_if_needed();
      });

      return;
    }
  }

  GPUThread::RunOnThread([pause_if_needed = std::move(pause_if_needed)]() {
    Initialize();
    StartChangeDiscFromFile();
    pause_if_needed();
  });
}

void FullscreenUI::DoToggleAnalogMode()
{
  // hacky way to toggle analog mode
  Host::RunOnCPUThread([]() {
    for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    {
      Controller* const ctrl = System::GetController(i);
      if (!ctrl)
        continue;

      for (const Controller::ControllerBindingInfo& bi : Controller::GetControllerInfo(ctrl->GetType()).bindings)
      {
        if (std::strcmp(bi.name, "Analog") == 0)
        {
          ctrl->SetBindState(bi.bind_index, 1.0f);
          ctrl->SetBindState(bi.bind_index, 0.0f);
          break;
        }
      }
    }
  });
}

void FullscreenUI::DoRequestExit()
{
  Host::RunOnCPUThread([]() { Host::RequestExitApplication(true); });
}

void FullscreenUI::DoDesktopMode()
{
  Host::RunOnCPUThread([]() { Host::RequestExitBigPicture(); });
}

void FullscreenUI::DoToggleFullscreen()
{
  Host::RunOnCPUThread([]() { Host::SetFullscreen(!Host::IsFullscreen()); });
}

//////////////////////////////////////////////////////////////////////////
// Landing Window
//////////////////////////////////////////////////////////////////////////

bool FullscreenUI::HasBackground()
{
  return static_cast<bool>(s_state.app_background_texture || s_state.app_background_shader);
}

void FullscreenUI::LoadBackground()
{
  if (!IsInitialized())
    return;

  g_gpu_device->RecycleTexture(std::move(s_state.app_background_texture));
  s_state.app_background_shader.reset();

  const TinyString background_name =
    Host::GetBaseTinyStringSettingValue("Main", "FullscreenUIBackground", DEFAULT_BACKGROUND_NAME);
  if (background_name.empty() || background_name == "None")
    return;

  static constexpr std::pair<const char*, bool (*)(const std::string&, Error*)> loaders[] = {
    {"glsl", &FullscreenUI::LoadBackgroundShader},
    {"jpg", &FullscreenUI::LoadBackgroundImage},
    {"png", &FullscreenUI::LoadBackgroundImage},
    {"webp", &FullscreenUI::LoadBackgroundImage},
  };

  for (const auto& [extension, loader] : loaders)
  {
    static constexpr auto get_path = [](const std::string& dir, const TinyString& name, const char* extension) {
      return fmt::format("{}" FS_OSPATH_SEPARATOR_STR "fullscreenui" FS_OSPATH_SEPARATOR_STR
                         "backgrounds" FS_OSPATH_SEPARATOR_STR "{}.{}",
                         dir, name, extension);
    };

    // try user directory first
    std::string path = get_path(EmuFolders::UserResources, background_name, extension);
    if (!FileSystem::FileExists(path.c_str()))
    {
      path = get_path(EmuFolders::Resources, background_name, extension);
      if (!FileSystem::FileExists(path.c_str()))
        continue;
    }

    Error error;
    if (!loader(path, &error))
    {
      ERROR_LOG("Failed to load background '{}' with {} loader: {}", background_name, extension,
                error.GetDescription());
      return;
    }

    INFO_LOG("Loaded background '{}' with {} loader", background_name, extension);
    return;
  }

  ERROR_LOG("No loader or file found for background '{}'", background_name);
}

FullscreenUI::ChoiceDialogOptions FullscreenUI::GetBackgroundOptions(const TinyString& current_value)
{
  static constexpr const char* dir = FS_OSPATH_SEPARATOR_STR "fullscreenui" FS_OSPATH_SEPARATOR_STR "backgrounds";

  ChoiceDialogOptions options;
  options.emplace_back(FSUI_STR("None"), (current_value == NONE_BACKGROUND_NAME));

  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(Path::Combine(EmuFolders::UserResources, dir).c_str(), "*",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &results);
  FileSystem::FindFiles(Path::Combine(EmuFolders::Resources, dir).c_str(), "*",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_KEEP_ARRAY, &results);

  for (const auto& it : results)
  {
    const std::string_view name = Path::GetFileTitle(it.FileName);
    if (std::any_of(options.begin(), options.end(), [&name](const auto& it) { return it.first == name; }))
      continue;

    options.emplace_back(name, current_value == name);
  }

  return options;
}

bool FullscreenUI::LoadBackgroundShader(const std::string& path, Error* error)
{
  std::optional<std::string> shader_body = FileSystem::ReadFileToString(path.c_str(), error);
  if (!shader_body.has_value())
    return false;

  const std::string::size_type main_pos = shader_body->find("void main()");
  if (main_pos == std::string::npos)
  {
    Error::SetStringView(error, "main() definition not found in shader.");
    return false;
  }

  const ShaderGen shadergen(g_gpu_device->GetRenderAPI(), GPUShaderLanguage::GLSLVK, false, false);

  std::stringstream shader;
  shadergen.WriteHeader(shader);
  shadergen.DeclareVertexEntryPoint(shader, {}, 0, 1, {}, true);
  shader << R"( {
    v_tex0 = vec2(float((v_id << 1) & 2u), float(v_id & 2u));
    v_pos = vec4(v_tex0 * vec2(2.0f, -2.0f) + vec2(-1.0f, 1.0f), 0.0f, 1.0f);
    #if API_VULKAN
      v_pos.y = -v_pos.y;
    #endif
  })";
  std::unique_ptr<GPUShader> vs =
    g_gpu_device->CreateShader(GPUShaderStage::Vertex, GPUShaderLanguage::GLSLVK, shader.str(), error);
  if (!vs)
    return false;

  shader.str(std::string());
  shadergen.WriteHeader(shader, false, false, false);
  shadergen.DeclareUniformBuffer(shader, {"vec2 u_display_size", "vec2 u_rcp_display_size", "float u_time"}, true);
  if (main_pos > 0)
    shader << std::string_view(shader_body.value()).substr(0, main_pos);
  shadergen.DeclareFragmentEntryPoint(shader, 0, 1);
  shader << std::string_view(shader_body.value()).substr(main_pos + 11);

  std::unique_ptr<GPUShader> fs =
    g_gpu_device->CreateShader(GPUShaderStage::Fragment, GPUShaderLanguage::GLSLVK, shader.str(), error);
  if (!fs)
    return false;

  GPUPipeline::GraphicsConfig plconfig = {};
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.geometry_shader = nullptr;
  plconfig.depth_format = GPUTexture::Format::Unknown;
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
  plconfig.SetTargetFormats(g_gpu_device->HasMainSwapChain() ? g_gpu_device->GetMainSwapChain()->GetFormat() :
                                                               GPUTexture::Format::RGBA8);
  plconfig.vertex_shader = vs.get();
  plconfig.fragment_shader = fs.get();
  s_state.app_background_shader = g_gpu_device->CreatePipeline(plconfig, error);
  if (!s_state.app_background_shader)
    return false;

  return true;
}

void FullscreenUI::DrawShaderBackgroundCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd)
{
  if (!g_gpu_device->HasMainSwapChain())
    return;

  struct alignas(16) Uniforms
  {
    float u_display_size[2];
    float u_rcp_display_size[2];
    float u_time;
  };

  const GSVector2 display_size = GSVector2(g_gpu_device->GetMainSwapChain()->GetSizeVec());
  Uniforms uniforms;
  GSVector2::store<true>(uniforms.u_display_size, display_size);
  GSVector2::store<true>(uniforms.u_rcp_display_size, GSVector2::cxpr(1.0f) / display_size);
  uniforms.u_time =
    static_cast<float>(Timer::ConvertValueToSeconds(Timer::GetCurrentValue() - s_state.app_background_load_time));

  g_gpu_device->SetPipeline(s_state.app_background_shader.get());
  g_gpu_device->PushUniformBuffer(&uniforms, sizeof(uniforms));
  g_gpu_device->Draw(3, 0);
}

bool FullscreenUI::LoadBackgroundImage(const std::string& path, Error* error)
{
  Image image;
  if (!image.LoadFromFile(path.c_str(), error))
    return false;

  s_state.app_background_texture = g_gpu_device->FetchAndUploadTextureImage(image, GPUTexture::Flags::None, error);
  if (!s_state.app_background_texture)
    return false;

  return true;
}

void FullscreenUI::DrawBackground()
{
  if (s_state.app_background_shader)
  {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddCallback(&FullscreenUI::DrawShaderBackgroundCallback, nullptr);
  }
  else if (s_state.app_background_texture)
  {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const ImRect uv_rect = FitImage(size, ImVec2(static_cast<float>(s_state.app_background_texture->GetWidth()),
                                                 static_cast<float>(s_state.app_background_texture->GetHeight())));
    dl->AddImage(s_state.app_background_texture.get(), ImVec2(0.0f, 0.0f), size, uv_rect.Min, uv_rect.Max);
  }
}

ImVec4 FullscreenUI::GetTransparentBackgroundColor(const ImVec4& no_background_color /* = UIStyle.BackgroundColor */)
{
  // use transparent colour if background is visible for things like game list
  if (!HasBackground())
    return ModAlpha(no_background_color, GetBackgroundAlpha());
  else
    return ImVec4{};
}

bool FullscreenUI::ShouldOpenToGameList()
{
  return Host::GetBaseBoolSettingValue("Main", "FullscreenUIOpenToGameList", false);
}

void FullscreenUI::DrawLandingTemplate(ImVec2* menu_pos, ImVec2* menu_size)
{
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, UIStyle.LargeFontSize + (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));
  *menu_pos = ImVec2(0.0f, heading_size.y);
  *menu_size = ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y - LayoutScale(LAYOUT_FOOTER_HEIGHT));

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "landing_heading",
                            ModAlpha(UIStyle.PrimaryColor, GetBackgroundAlpha())))
  {
    ImFont* const heading_font = UIStyle.Font;
    const float heading_font_size = UIStyle.LargeFontSize;
    const float heading_font_weight = UIStyle.BoldFontWeight;
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    SmallString heading_str;

    const u32 text_color = ImGui::GetColorU32(UIStyle.PrimaryTextColor);

    // draw branding
    {
      const ImVec2 logo_pos = LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING);
      const ImVec2 logo_size = ImVec2(UIStyle.LargeFontSize, UIStyle.LargeFontSize);
      dl->AddImage(s_state.app_icon_texture.get(), logo_pos, logo_pos + logo_size);

      const std::string_view heading_text = "DuckStation";
      const ImVec2 text_size = heading_font->CalcTextSizeA(heading_font_size, heading_font_weight, FLT_MAX, 0.0f,
                                                           IMSTR_START_END(heading_text));
      const ImVec2 text_pos = ImVec2(logo_pos.x + logo_size.x + LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING), logo_pos.y);
      RenderShadowedTextClipped(heading_font, heading_font_size, heading_font_weight, text_pos, text_pos + text_size,
                                text_color, heading_text, &text_size);
    }

    // draw time
    ImVec2 time_pos;
    {
      UpdateCurrentTimeString();

      const ImVec2 time_size = heading_font->CalcTextSizeA(heading_font_size, heading_font_weight, FLT_MAX, 0.0f,
                                                           IMSTR_START_END(s_state.current_time_string));
      time_pos = ImVec2(heading_size.x - LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING) - time_size.x,
                        LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING));
      RenderShadowedTextClipped(heading_font, heading_font_size, heading_font_weight, time_pos, time_pos + time_size,
                                text_color, s_state.current_time_string, &time_size);
    }

    // draw achievements info
    if (Achievements::IsActive())
    {
      const auto lock = Achievements::GetLock();
      const char* username = Achievements::GetLoggedInUserName();
      if (username)
      {
        const ImVec2 name_size =
          heading_font->CalcTextSizeA(heading_font_size, heading_font_weight, FLT_MAX, 0.0f, username);
        const ImVec2 name_pos =
          ImVec2(time_pos.x - name_size.x - LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING), time_pos.y);
        RenderShadowedTextClipped(heading_font, heading_font_size, heading_font_weight, name_pos, name_pos + name_size,
                                  text_color, username, &name_size);

        if (const std::string& badge_path = Achievements::GetLoggedInUserBadgePath(); !badge_path.empty())
        {
          const ImVec2 badge_size = ImVec2(UIStyle.LargeFontSize, UIStyle.LargeFontSize);
          const ImVec2 badge_pos =
            ImVec2(name_pos.x - badge_size.x - LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING), time_pos.y);

          dl->AddImage(reinterpret_cast<ImTextureID>(GetCachedTextureAsync(badge_path)), badge_pos,
                       badge_pos + badge_size);
        }
      }
    }
  }
  EndFullscreenWindow();
}

void FullscreenUI::DrawLandingWindow()
{
  ImVec2 menu_pos, menu_size;
  DrawLandingTemplate(&menu_pos, &menu_size);

  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.BackgroundTextColor);

  if (BeginHorizontalMenu("landing_window", menu_pos, menu_size, GetTransparentBackgroundColor(), 4))
  {
    ResetFocusHere();

    if (UserThemeableHorizontalButton("fullscreenui/game-list.png", "fullscreenui/game-list.svg",
                                      FSUI_VSTR("Game List"),
                                      FSUI_VSTR("Launch a game from images scanned from your game directories.")))
    {
      BeginTransition(&SwitchToGameList);
    }

    ImGui::SetItemDefaultFocus();

    if (UserThemeableHorizontalButton(
          "fullscreenui/cdrom.png", "fullscreenui/start-disc.svg", FSUI_VSTR("Start Game"),
          FSUI_VSTR("Launch a game from a file, disc, or starts the console without any disc inserted.")))
    {
      BeginTransition([]() { SwitchToMainWindow(MainWindowType::StartGame); });
    }

    if (UserThemeableHorizontalButton("fullscreenui/settings.png", "fullscreenui/settings.svg", FSUI_VSTR("Settings"),
                                      FSUI_VSTR("Changes settings for the application.")))
    {
      BeginTransition(&SwitchToSettings);
    }

    if (UserThemeableHorizontalButton("fullscreenui/exit.png", "fullscreenui/exit.svg", FSUI_VSTR("Exit"),
                                      FSUI_VSTR("Return to desktop mode, or exit the application.")) ||
        (!AreAnyDialogsOpen() && WantsToCloseMenu()))
    {
      BeginTransition([]() { SwitchToMainWindow(MainWindowType::Exit); });
    }
  }
  EndHorizontalMenu();

  ImGui::PopStyleColor();

  if (!AreAnyDialogsOpen())
  {
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) || ImGui::IsKeyPressed(ImGuiKey_F1, false))
      OpenFixedPopupDialog(ABOUT_DIALOG_NAME);
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) || ImGui::IsKeyPressed(ImGuiKey_F3, false))
      DoResume();
    else if (ImGui::IsKeyPressed(ImGuiKey_NavGamepadMenu, false) || ImGui::IsKeyPressed(ImGuiKey_F11, false))
      DoToggleFullscreen();
  }

  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_SHARE_CAPTURE, FSUI_VSTR("About")),
                                       std::make_pair(ICON_PF_BURGER_MENU, FSUI_VSTR("Resume Last Session")),
                                       std::make_pair(ICON_PF_BUTTON_X, FSUI_VSTR("Toggle Fullscreen")),
                                       std::make_pair(ICON_PF_XBOX_DPAD_LEFT_RIGHT, FSUI_VSTR("Navigate")),
                                       std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Exit"))});
  }
  else
  {
    SetFullscreenFooterText(std::array{
      std::make_pair(ICON_PF_F1, FSUI_VSTR("About")), std::make_pair(ICON_PF_F3, FSUI_VSTR("Resume Last Session")),
      std::make_pair(ICON_PF_F11, FSUI_VSTR("Toggle Fullscreen")),
      std::make_pair(ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT, FSUI_VSTR("Navigate")),
      std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")), std::make_pair(ICON_PF_ESC, FSUI_VSTR("Exit"))});
  }
}

void FullscreenUI::DrawStartGameWindow()
{
  ImVec2 menu_pos, menu_size;
  DrawLandingTemplate(&menu_pos, &menu_size);

  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.BackgroundTextColor);

  if (BeginHorizontalMenu("start_game_window", menu_pos, menu_size, GetTransparentBackgroundColor(), 4))
  {
    ResetFocusHere();

    if (HorizontalMenuItem(GetUserThemeableTexture("fullscreenui/start-file.png", "fullscreenui/start-file.svg"),
                           FSUI_VSTR("Start File"), FSUI_VSTR("Launch a game by selecting a file/disc image.")))
    {
      DoStartFile();
    }

    if (HorizontalMenuItem(GetUserThemeableTexture("fullscreenui/start-disc.png", "fullscreenui/start-disc.svg"),
                           FSUI_VSTR("Start Disc"), FSUI_VSTR("Start a game from a disc in your PC's DVD drive.")))
    {
      DoStartDisc();
    }

    if (HorizontalMenuItem(GetUserThemeableTexture("fullscreenui/start-bios.png", "fullscreenui/start-bios.svg"),
                           FSUI_VSTR("Start BIOS"), FSUI_VSTR("Start the console without any disc inserted.")))
    {
      DoStartBIOS();
    }

    if (HorizontalMenuItem(GetUserThemeableTexture("fullscreenui/back-icon.png", "fullscreenui/back-icon.svg"),
                           FSUI_VSTR("Back"), FSUI_VSTR("Return to the previous menu.")) ||
        (!AreAnyDialogsOpen() && WantsToCloseMenu()))
    {
      BeginTransition([]() { SwitchToMainWindow(MainWindowType::Landing); });
    }
  }
  EndHorizontalMenu();

  ImGui::PopStyleColor();

  if (!AreAnyDialogsOpen())
  {
    if (ImGui::IsKeyPressed(ImGuiKey_NavGamepadMenu, false) || ImGui::IsKeyPressed(ImGuiKey_F1, false))
      OpenSaveStateSelector(std::string(), std::string(), true);
  }

  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_XBOX_DPAD_LEFT_RIGHT, FSUI_VSTR("Navigate")),
                                       std::make_pair(ICON_PF_BUTTON_Y, FSUI_VSTR("Load Global State")),
                                       std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Back"))});
  }
  else
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT, FSUI_VSTR("Navigate")),
                                       std::make_pair(ICON_PF_F1, FSUI_VSTR("Load Global State")),
                                       std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_ESC, FSUI_VSTR("Back"))});
  }
}

void FullscreenUI::DrawExitWindow()
{
  ImVec2 menu_pos, menu_size;
  DrawLandingTemplate(&menu_pos, &menu_size);

  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.BackgroundTextColor);

  if (BeginHorizontalMenu("exit_window", menu_pos, menu_size, GetTransparentBackgroundColor(), 3))
  {
    ResetFocusHere();

    if (HorizontalMenuItem(GetUserThemeableTexture("fullscreenui/back-icon.png", "fullscreenui/back-icon.svg"),
                           FSUI_VSTR("Back"), FSUI_VSTR("Return to the previous menu.")) ||
        WantsToCloseMenu())
    {
      BeginTransition([]() { SwitchToMainWindow(MainWindowType::Landing); });
    }

    if (HorizontalMenuItem(GetUserThemeableTexture("fullscreenui/exit.png", "fullscreenui/exit.svg"),
                           FSUI_VSTR("Exit DuckStation"),
                           FSUI_VSTR("Completely exits the application, returning you to your desktop.")))
    {
      DoRequestExit();
    }

    if (HorizontalMenuItem(GetUserThemeableTexture("fullscreenui/desktop-mode.png", "fullscreenui/desktop-mode.svg"),
                           FSUI_VSTR("Desktop Mode"),
                           FSUI_VSTR("Exits Big Picture mode, returning to the desktop interface.")))
    {
      DoDesktopMode();
    }
  }
  EndHorizontalMenu();

  ImGui::PopStyleColor();

  SetStandardSelectionFooterText(true);
}

float FullscreenUI::GetBackgroundAlpha()
{
  if (GPUThread::HasGPUBackend())
  {
    // reduce background opacity when changing postfx settings
    if (s_state.current_main_window == MainWindowType::Settings &&
        GetCurrentSettingsPage() == SettingsPage::PostProcessing)
    {
      return 0.50f;
    }
    else
    {
      return 0.90f;
    }
  }
  else
  {
    return HasBackground() ? 0.5f : 1.0f;
  }
}

void FullscreenUI::DrawPauseMenu()
{
  static constexpr auto switch_submenu = [](PauseSubMenu submenu) {
    s_state.current_pause_submenu = submenu;
    QueueResetFocus(FocusResetType::ViewChanged);
  };

  static constexpr float top_bar_height = 90.0f;
  static constexpr float top_bar_padding = 10.0f;

  SmallString buffer;

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImVec2 display_size(ImGui::GetIO().DisplaySize);
  const ImU32 title_text_color = ImGui::GetColorU32(UIStyle.BackgroundTextColor);
  const ImU32 text_color = ImGui::GetColorU32(DarkerColor(UIStyle.BackgroundTextColor, 0.85f));
  const ImU32 last_text_color = ImGui::GetColorU32(DarkerColor(DarkerColor(UIStyle.BackgroundTextColor, 0.85f)));

  // top bar
  const float scaled_top_bar_height = LayoutScale(top_bar_height);
  {
    const float scaled_text_spacing = LayoutScale(4.0f);
    const float scaled_top_bar_padding = LayoutScale(top_bar_padding);
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(display_size.x, scaled_top_bar_height),
                      ImGui::GetColorU32(ModAlpha(UIStyle.BackgroundColor, 0.95f)), 0.0f);

    const std::string& game_title = GPUThread::GetGameTitle();
    const std::string& game_serial = GPUThread::GetGameSerial();
    const std::string& game_path = GPUThread::GetGamePath();
    GPUTexture* const cover = GetCoverForCurrentGame(game_path);
    const float image_padding = LayoutScale(5.0f); // compensate for font baseline
    const float image_size = scaled_top_bar_height - scaled_top_bar_padding - scaled_top_bar_padding - image_padding;
    const ImRect image_rect(
      CenterImage(ImRect(scaled_top_bar_padding + image_padding, scaled_top_bar_padding + image_padding,
                         scaled_top_bar_padding + image_size, scaled_top_bar_padding + image_size),
                  ImVec2(static_cast<float>(cover->GetWidth()), static_cast<float>(cover->GetHeight()))));
    dl->AddImage(cover, image_rect.Min, image_rect.Max);

    if (!game_serial.empty())
      buffer.format("{} - {}", game_serial, Path::GetFileName(game_path));
    else
      buffer.assign(Path::GetFileName(game_path));

    ImVec2 text_pos = ImVec2(scaled_top_bar_padding + image_size + scaled_top_bar_padding, scaled_top_bar_padding);
    RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, text_pos, display_size,
                              title_text_color, game_title);
    text_pos.y += UIStyle.LargeFontSize + scaled_text_spacing;

    if (Achievements::IsActive())
    {
      const auto lock = Achievements::GetLock();
      if (const std::string& rp = Achievements::GetRichPresenceString(); !rp.empty())
      {
        RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos,
                                  display_size, text_color, rp);
        text_pos.y += UIStyle.MediumFontSize + scaled_text_spacing;
      }
    }

    RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, text_pos,
                              display_size, last_text_color, buffer);

    // current time / play time
    UpdateCurrentTimeString();

    ImVec2 text_size =
      UIStyle.Font->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight, std::numeric_limits<float>::max(),
                                  -1.0f, IMSTR_START_END(s_state.current_time_string));
    text_pos = ImVec2(display_size.x - scaled_top_bar_padding - text_size.x, scaled_top_bar_padding);
    RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, text_pos, display_size,
                              title_text_color, s_state.current_time_string);
    text_pos.y += UIStyle.LargeFontSize + scaled_text_spacing;

    if (!game_serial.empty())
    {
      const std::time_t cached_played_time = GameList::GetCachedPlayedTimeForSerial(game_serial);
      const std::time_t session_time = static_cast<std::time_t>(System::GetSessionPlayedTime());

      buffer.format(FSUI_FSTR("Session: {}"), GameList::FormatTimespan(session_time, true));
      text_size =
        UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, std::numeric_limits<float>::max(),
                                    -1.0f, buffer.c_str(), buffer.end_ptr());
      text_pos.x = display_size.x - scaled_top_bar_padding - text_size.x;
      RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, text_pos,
                                display_size, text_color, buffer);
      text_pos.y += UIStyle.MediumFontSize + scaled_text_spacing;

      buffer.format(FSUI_FSTR("All Time: {}"), GameList::FormatTimespan(cached_played_time + session_time, true));
      text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight,
                                              std::numeric_limits<float>::max(), -1.0f, IMSTR_START_END(buffer));
      text_pos.x = display_size.x - scaled_top_bar_padding - text_size.x;
      RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, text_pos,
                                display_size, text_color, buffer);
      text_pos.y += UIStyle.MediumFontSize + scaled_text_spacing;
    }
  }

  const ImVec2 window_size(LayoutScale(500.0f),
                           display_size.y - scaled_top_bar_height - LayoutScale(LAYOUT_FOOTER_HEIGHT));
  const ImVec2 window_pos(0.0f, scaled_top_bar_height);

  // background
  dl->AddRectFilled(ImVec2(0.0f, scaled_top_bar_height),
                    ImVec2(display_size.x, display_size.x - scaled_top_bar_height - LayoutScale(LAYOUT_FOOTER_HEIGHT)),
                    ImGui::GetColorU32(ModAlpha(UIStyle.BackgroundColor, 0.825f)));

  Achievements::DrawPauseMenuOverlays(scaled_top_bar_height);

  if (BeginFullscreenWindow(window_pos, window_size, "pause_menu", ImVec4(0.0f, 0.0f, 0.0f, 0.0f), 0.0f,
                            ImVec2(10.0f, 10.0f), ImGuiWindowFlags_NoBackground))
  {
    static constexpr u32 submenu_item_count[] = {
      11, // None
      4,  // Exit
      3,  // Achievements
    };

    // reduce spacing to fit all the buttons
    ResetFocusHere();
    BeginMenuButtons(submenu_item_count[static_cast<u32>(s_state.current_pause_submenu)], 1.0f,
                     LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING, 0.0f, 4.0f);

    switch (s_state.current_pause_submenu)
    {
      case PauseSubMenu::None:
      {
        // NOTE: Menu close must come first, because otherwise VM destruction options will race.
        const bool has_game = GPUThread::HasGPUBackend();

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_PLAY, "Resume Game")) || WantsToCloseMenu())
          ClosePauseMenu();
        ImGui::SetItemDefaultFocus();

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_PF_FAST_FORWARD, "Toggle Fast Forward")))
        {
          ClosePauseMenu();
          DoToggleFastForward();
        }

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_PF_DOWNLOAD, "Load State"), has_game))
        {
          BeginTransition([]() { OpenSaveStateSelector(GPUThread::GetGameSerial(), GPUThread::GetGamePath(), true); });
        }

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_PF_DISKETTE, "Save State"), has_game))
        {
          BeginTransition([]() { OpenSaveStateSelector(GPUThread::GetGameSerial(), GPUThread::GetGamePath(), false); });
        }

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_PF_GAMEPAD_ALT, "Toggle Analog")))
        {
          ClosePauseMenu();
          DoToggleAnalogMode();
        }

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_WRENCH, "Game Properties"), has_game))
          BeginTransition([]() { SwitchToGameSettings(); });

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_TROPHY, "Achievements"),
                                     Achievements::HasAchievementsOrLeaderboards()))
        {
          // skip second menu and go straight to cheevos if there's no lbs
          if (!Achievements::HasLeaderboards())
            BeginTransition(&SwitchToAchievements);
          else
            BeginTransition([]() { switch_submenu(PauseSubMenu::Achievements); });
        }

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_CAMERA, "Save Screenshot")))
        {
          Host::RunOnCPUThread([]() { System::SaveScreenshot(); });
          ClosePauseMenu();
        }

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_COMPACT_DISC, "Change Disc")))
        {
          BeginTransition(SHORT_TRANSITION_TIME, []() { s_state.current_main_window = MainWindowType::None; });
          Host::RunOnCPUThread([]() { BeginChangeDiscOnCPUThread(false); });
        }

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_SLIDERS, "Settings")))
          BeginTransition(&SwitchToSettings);

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_POWER_OFF, "Close Game")))
        {
          // skip submenu when we can't save anyway
          if (!has_game)
            BeginTransition(LONG_TRANSITION_TIME, []() { RequestShutdown(false); });
          else
            BeginTransition([]() { switch_submenu(PauseSubMenu::Exit); });
        }
      }
      break;

      case PauseSubMenu::Exit:
      {
        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_PF_NAVIGATION_BACK, "Back To Pause Menu")) ||
            WantsToCloseMenu())
          BeginTransition([]() { switch_submenu(PauseSubMenu::None); });
        else
          ImGui::SetItemDefaultFocus();

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_ARROWS_ROTATE, "Reset System")))
          RequestReset();

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_FLOPPY_DISK, "Exit And Save State")))
          BeginTransition(LONG_TRANSITION_TIME, []() { RequestShutdown(true); });

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_POWER_OFF, "Exit Without Saving")))
          BeginTransition(LONG_TRANSITION_TIME, []() { RequestShutdown(false); });
      }
      break;

      case PauseSubMenu::Achievements:
      {
        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_PF_NAVIGATION_BACK, "Back To Pause Menu")) ||
            WantsToCloseMenu())
          BeginTransition([]() { switch_submenu(PauseSubMenu::None); });
        else
          ImGui::SetItemDefaultFocus();

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_TROPHY, "Achievements")))
          BeginTransition(&SwitchToAchievements);

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_STOPWATCH, "Leaderboards")))
          BeginTransition(&SwitchToLeaderboards);
      }
      break;
    }

    EndMenuButtons();

    EndFullscreenWindow();
  }

  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, FSUI_VSTR("Change Selection")),
                                       std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Return To Game"))});
  }
  else
  {
    SetFullscreenFooterText(std::array{
      std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, FSUI_VSTR("Change Selection")),
      std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")), std::make_pair(ICON_PF_ESC, FSUI_VSTR("Return To Game"))});
  }
}

void FullscreenUI::InitializePlaceholderSaveStateListEntry(SaveStateListEntry* li, s32 slot, bool global)
{
  li->title = (global || slot > 0) ? fmt::format(global ? FSUI_FSTR("Global Slot {0}##global_slot_{0}") :
                                                          FSUI_FSTR("Game Slot {0}##game_slot_{0}"),
                                                 slot) :
                                     FSUI_STR("Quick Save");
  li->summary = FSUI_STR("No save present in this slot.");
  li->state_path = {};
  li->timestamp = 0;
  li->slot = slot;
  li->preview_texture = {};
  li->global = global;
}

bool FullscreenUI::InitializeSaveStateListEntryFromSerial(SaveStateListEntry* li, const std::string& serial, s32 slot,
                                                          bool global)
{
  const std::string path = (global ? System::GetGlobalSaveStatePath(slot) : System::GetGameSaveStatePath(serial, slot));
  if (!InitializeSaveStateListEntryFromPath(li, path.c_str(), slot, global))
  {
    InitializePlaceholderSaveStateListEntry(li, slot, global);
    return false;
  }

  return true;
}

bool FullscreenUI::InitializeSaveStateListEntryFromPath(SaveStateListEntry* li, std::string path, s32 slot, bool global)
{
  std::optional<ExtendedSaveStateInfo> ssi(System::GetExtendedSaveStateInfo(path.c_str()));
  if (!ssi.has_value())
    return false;

  if (global)
  {
    li->title = fmt::format(FSUI_FSTR("Global Slot {0} - {1}##global_slot_{0}"), slot, ssi->serial);
  }
  else
  {
    li->title = (slot > 0) ? fmt::format(FSUI_FSTR("Game Slot {0}##game_slot_{0}"), slot) : FSUI_STR("Game Quick Save");
  }

  li->summary = fmt::format(
    FSUI_FSTR("Saved {}"), Host::FormatNumber(Host::NumberFormatType::ShortDateTime, static_cast<s64>(ssi->timestamp)));
  li->timestamp = ssi->timestamp;
  li->slot = slot;
  li->state_path = std::move(path);
  li->game_path = std::move(ssi->media_path);
  li->global = global;
  if (ssi->screenshot.IsValid())
    li->preview_texture = g_gpu_device->FetchAndUploadTextureImage(ssi->screenshot);

  return true;
}

void FullscreenUI::ClearSaveStateEntryList()
{
  for (SaveStateListEntry& entry : s_state.save_state_selector_slots)
  {
    if (entry.preview_texture)
      g_gpu_device->RecycleTexture(std::move(entry.preview_texture));
  }
  s_state.save_state_selector_slots.clear();
}

u32 FullscreenUI::PopulateSaveStateListEntries(const std::string& serial,
                                               std::optional<ExtendedSaveStateInfo> undo_save_state, bool is_loading)
{
  ClearSaveStateEntryList();

  if (undo_save_state.has_value())
  {
    SaveStateListEntry li;
    li.title = FSUI_STR("Undo Load State");
    li.summary = FSUI_STR("Restores the state of the system prior to the last state loaded.");
    if (undo_save_state->screenshot.IsValid())
      li.preview_texture = g_gpu_device->FetchAndUploadTextureImage(undo_save_state->screenshot);
    s_state.save_state_selector_slots.push_back(std::move(li));
  }

  if (!serial.empty())
  {
    for (s32 i = 1; i <= System::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      SaveStateListEntry li;
      if (InitializeSaveStateListEntryFromSerial(&li, serial, i, false) || !is_loading)
        s_state.save_state_selector_slots.push_back(std::move(li));
    }
  }

  for (s32 i = 1; i <= System::GLOBAL_SAVE_STATE_SLOTS; i++)
  {
    SaveStateListEntry li;
    if (InitializeSaveStateListEntryFromSerial(&li, serial, i, true) || !is_loading)
      s_state.save_state_selector_slots.push_back(std::move(li));
  }

  return static_cast<u32>(s_state.save_state_selector_slots.size());
}

void FullscreenUI::OpenSaveStateSelector(const std::string& serial, const std::string& path, bool is_loading)
{
  if (GPUThread::HasGPUBackend())
  {
    // need to get the undo state, if any
    Host::RunOnCPUThread([serial = serial, is_loading]() {
      std::optional<ExtendedSaveStateInfo> undo_state;
      if (is_loading)
        undo_state = System::GetUndoSaveStateInfo();
      GPUThread::RunOnThread([serial = std::move(serial), undo_state = std::move(undo_state), is_loading]() mutable {
        if (PopulateSaveStateListEntries(serial, std::move(undo_state), is_loading) > 0)
        {
          s_state.save_state_selector_loading = is_loading;
          SwitchToMainWindow(MainWindowType::SaveStateSelector);
        }
        else
        {
          ShowToast({}, FSUI_STR("No save states found."), Host::OSD_INFO_DURATION);
        }
      });
    });
  }
  else
  {
    if (PopulateSaveStateListEntries(serial, std::nullopt, is_loading) > 0)
    {
      s_state.save_state_selector_loading = is_loading;
      SwitchToMainWindow(MainWindowType::SaveStateSelector);
    }
    else
    {
      ShowToast({}, FSUI_STR("No save states found."), Host::OSD_INFO_DURATION);
    }
  }
}

void FullscreenUI::DrawSaveStateSelector()
{
  static constexpr auto do_load_state = [](std::string game_path, std::string state_path) {
    if (GPUThread::HasGPUBackend())
    {
      ClearSaveStateEntryList();
      ReturnToMainWindow(LONG_TRANSITION_TIME);

      Host::RunOnCPUThread([game_path = std::move(game_path), state_path = std::move(state_path)]() mutable {
        if (System::IsValid())
        {
          if (state_path.empty())
          {
            // Loading undo state.
            if (!System::UndoLoadState())
            {
              GPUThread::RunOnThread(
                []() { ShowToast(std::string(), TRANSLATE_STR("System", "Failed to undo load state.")); });
            }
          }
          else
          {
            Error error;
            if (!System::LoadState(state_path.c_str(), &error, true, false))
            {
              GPUThread::RunOnThread([error_desc = error.TakeDescription()]() {
                ShowToast(std::string(), fmt::format(TRANSLATE_FS("System", "Failed to load state: {}"), error_desc));
              });
            }
          }
        }
      });
    }
    else
    {
      DoStartPath(std::move(game_path), std::move(state_path));
    }
  };

  static constexpr auto do_save_state = [](s32 slot, bool global) {
    ClearSaveStateEntryList();
    ReturnToMainWindow(LONG_TRANSITION_TIME);

    Host::RunOnCPUThread([slot, global]() {
      if (!System::IsValid())
        return;

      std::string path(global ? System::GetGlobalSaveStatePath(slot) :
                                System::GetGameSaveStatePath(System::GetGameSerial(), slot));
      Error error;
      if (!System::SaveState(std::move(path), &error, g_settings.create_save_state_backups, false))
      {
        GPUThread::RunOnThread([error_desc = error.TakeDescription()]() {
          ShowToast(std::string(), fmt::format(TRANSLATE_FS("System", "Failed to save state: {}"), error_desc));
        });
      }
    });
  };

  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, UIStyle.LargeFontSize + (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));
  SaveStateListEntry* pressed_entry = nullptr;
  bool closed = false;

  // last state deleted?
  if (s_state.save_state_selector_slots.empty())
    closed = true;

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "##save_state_selector_title",
                            ModAlpha(UIStyle.PrimaryColor, GetBackgroundAlpha())))
  {
    BeginNavBar();
    if (NavButton(ICON_PF_NAVIGATION_BACK, true, true))
      closed = true;

    NavTitle(s_state.save_state_selector_loading ? FSUI_VSTR("Load State") : FSUI_VSTR("Save State"));
    EndNavBar();
  }

  EndFullscreenWindow();

  if (IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  if (BeginFullscreenWindow(
        ImVec2(0.0f, heading_size.y),
        ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y - LayoutScale(LAYOUT_FOOTER_HEIGHT)),
        "##save_state_selector_list", ModAlpha(UIStyle.BackgroundColor, GetBackgroundAlpha()), 0.0f,
        ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING)))
  {
    ResetFocusHere();
    BeginMenuButtons();

    const ImGuiStyle& style = ImGui::GetStyle();

    const float title_spacing = LayoutScale(10.0f);
    const float summary_spacing = LayoutScale(4.0f);
    const float item_spacing = LayoutScale(20.0f);
    const float item_width_with_spacing = std::floor(LayoutScale(LAYOUT_SCREEN_WIDTH / 4.0f));
    const float item_width = item_width_with_spacing - item_spacing;
    const float image_width = item_width - (style.FramePadding.x * 2.0f);
    const float image_height = image_width / 1.33f;
    const ImVec2 image_size(image_width, image_height);
    const float item_height = (style.FramePadding.y * 2.0f) + image_height + title_spacing + UIStyle.LargeFontSize +
                              summary_spacing + UIStyle.MediumFontSize;
    const ImVec2 item_size(item_width, item_height);
    const u32 grid_count_x = static_cast<u32>(std::floor(ImGui::GetContentRegionAvail().x / item_width_with_spacing));
    const float start_x =
      (static_cast<float>(ImGui::GetWindowWidth()) - (item_width_with_spacing * static_cast<float>(grid_count_x))) *
      0.5f;

    u32 grid_x = 0;
    ImGui::SetCursorPosX(start_x);
    for (u32 i = 0;;)
    {
      SaveStateListEntry& entry = s_state.save_state_selector_slots[i];

      ImGuiWindow* window = ImGui::GetCurrentWindow();
      if (window->SkipItems)
      {
        i++;
        continue;
      }

      const ImGuiID id = window->GetID(static_cast<int>(i));
      const ImVec2 pos(window->DC.CursorPos);
      ImRect bb(pos, pos + item_size);
      ImGui::ItemSize(item_size);
      if (ImGui::ItemAdd(bb, id))
      {
        bool held;
        bool hovered;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, 0);
        if (hovered)
        {
          const ImU32 col = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered, 1.0f);

          const float t = std::min(static_cast<float>(std::abs(std::sin(ImGui::GetTime() * 0.75) * 1.1)), 1.0f);
          ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Border, t));

          DrawMenuButtonFrame(bb.Min, bb.Max, col, true);

          ImGui::PopStyleColor();
        }

        bb.Min += style.FramePadding;
        bb.Max -= style.FramePadding;

        GPUTexture* const screenshot =
          entry.preview_texture ? entry.preview_texture.get() : GetCachedTextureAsync("no-save.png");
        const ImRect image_rect(
          CenterImage(ImRect(bb.Min, bb.Min + image_size),
                      ImVec2(static_cast<float>(screenshot->GetWidth()), static_cast<float>(screenshot->GetHeight()))));

        ImGui::GetWindowDrawList()->AddImage(screenshot, image_rect.Min, image_rect.Max, ImVec2(0.0f, 0.0f),
                                             ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));

        const ImVec2 title_pos(bb.Min.x, bb.Min.y + image_height + title_spacing);
        const ImRect title_bb(title_pos, ImVec2(bb.Max.x, title_pos.y + UIStyle.LargeFontSize));
        RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, title_bb.Min,
                                  title_bb.Max, ImGui::GetColorU32(ImGuiCol_Text), entry.title, nullptr,
                                  ImVec2(0.0f, 0.0f), 0.0f, &title_bb);

        if (!entry.summary.empty())
        {
          const ImVec2 summary_pos(bb.Min.x, title_pos.y + UIStyle.LargeFontSize + summary_spacing);
          const ImRect summary_bb(summary_pos, ImVec2(bb.Max.x, summary_pos.y + UIStyle.MediumFontSize));
          RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, summary_bb.Min,
                                    summary_bb.Max, ImGui::GetColorU32(ImGuiCol_Text), entry.summary, nullptr,
                                    ImVec2(0.0f, 0.0f), 0.0f, &summary_bb);
        }

        if (pressed)
        {
          // avoid closing while drawing
          pressed_entry = &entry;
        }
        else if (hovered &&
                 (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                  ImGui::IsKeyPressed(ImGuiKey_NavGamepadInput, false) || ImGui::IsKeyPressed(ImGuiKey_F1, false)))
        {
          CancelPendingMenuClose();

          std::string title;
          if (s_state.save_state_selector_loading)
            title = FSUI_ICONVSTR(ICON_FA_FOLDER_OPEN, "Load State");
          else
            title = FSUI_ICONVSTR(ICON_FA_FOLDER_OPEN, "Save State");

          ChoiceDialogOptions options;
          options.reserve(3);
          options.emplace_back(title, false);
          if (!entry.state_path.empty())
            options.emplace_back(FSUI_ICONVSTR(ICON_FA_FOLDER_MINUS, "Delete Save"), false);
          options.emplace_back(FSUI_ICONVSTR(ICON_FA_SQUARE_XMARK, "Close Menu"), false);

          OpenChoiceDialog(std::move(title), false, std::move(options),
                           [i](s32 index, const std::string& title, bool checked) mutable {
                             if (index < 0 || i >= s_state.save_state_selector_slots.size())
                               return;

                             SaveStateListEntry& entry = s_state.save_state_selector_slots[i];
                             if (index == 0)
                             {
                               // load state
                               if (s_state.save_state_selector_loading)
                                 do_load_state(std::move(entry.game_path), std::move(entry.state_path));
                               else
                                 do_save_state(entry.slot, entry.global);
                             }
                             else if (!entry.state_path.empty() && index == 1)
                             {
                               // delete state
                               if (!FileSystem::FileExists(entry.state_path.c_str()))
                               {
                                 ShowToast({}, fmt::format(FSUI_FSTR("{} does not exist."), RemoveHash(entry.title)));
                               }
                               else if (FileSystem::DeleteFile(entry.state_path.c_str()))
                               {
                                 ShowToast({}, fmt::format(FSUI_FSTR("{} deleted."), RemoveHash(entry.title)));

                                 // need to preserve the texture, since it's going to be drawn this frame
                                 // TODO: do this with a transition for safety
                                 g_gpu_device->RecycleTexture(std::move(entry.preview_texture));

                                 if (s_state.save_state_selector_loading)
                                   s_state.save_state_selector_slots.erase(s_state.save_state_selector_slots.begin() +
                                                                           i);
                                 else
                                   InitializePlaceholderSaveStateListEntry(&entry, entry.slot, entry.global);
                               }
                               else
                               {
                                 ShowToast({}, fmt::format(FSUI_FSTR("Failed to delete {}."), RemoveHash(entry.title)));
                               }
                             }
                           });
        }
      }

      // avoid triggering imgui warning
      i++;
      if (i == s_state.save_state_selector_slots.size())
        break;

      grid_x++;
      if (grid_x == grid_count_x)
      {
        grid_x = 0;
        ImGui::SetCursorPosX(start_x);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + item_spacing);
      }
      else
      {
        ImGui::SameLine(start_x + static_cast<float>(grid_x) * (item_width + item_spacing));
      }
    }

    EndMenuButtons();
    SetWindowNavWrapping(true, true);
  }

  EndFullscreenWindow();

  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_XBOX_DPAD, FSUI_VSTR("Select State")),
                 std::make_pair(ICON_PF_BUTTON_Y, FSUI_VSTR("Delete State")),
                 std::make_pair(ICON_PF_BUTTON_A, s_state.save_state_selector_loading ? FSUI_VSTR("Load State") :
                                                                                        FSUI_VSTR("Save State")),
                 std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Cancel"))});
  }
  else
  {
    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT,
                                FSUI_VSTR("Select State")),
                 std::make_pair(ICON_PF_F1, FSUI_VSTR("Delete State")),
                 std::make_pair(ICON_PF_ENTER, s_state.save_state_selector_loading ? FSUI_VSTR("Load State") :
                                                                                     FSUI_VSTR("Save State")),
                 std::make_pair(ICON_PF_ESC, FSUI_VSTR("Cancel"))});
  }

  if (pressed_entry)
  {
    if (s_state.save_state_selector_loading)
      do_load_state(std::move(pressed_entry->game_path), std::move(pressed_entry->state_path));
    else
      do_save_state(pressed_entry->slot, pressed_entry->global);
  }
  else if ((!AreAnyDialogsOpen() && WantsToCloseMenu()) || closed)
  {
    ClearSaveStateEntryList();
    ReturnToPreviousWindow();
  }
}

bool FullscreenUI::OpenLoadStateSelectorForGameResume(const GameList::Entry* entry)
{
  SaveStateListEntry slentry;
  if (!InitializeSaveStateListEntryFromSerial(&slentry, entry->serial, -1, false))
    return false;

  slentry.game_path = entry->path;
  s_state.save_state_selector_slots.push_back(std::move(slentry));
  OpenFixedPopupDialog(RESUME_STATE_SELECTOR_DIALOG_NAME);
  return true;
}

void FullscreenUI::DrawResumeStateSelector()
{
  if (!BeginFixedPopupDialog(LayoutScale(30.0f), LayoutScale(40.0f), ImVec2(LayoutScale(550.0f), 0.0f)))
  {
    ClearSaveStateEntryList();
    return;
  }

  SaveStateListEntry& entry = s_state.save_state_selector_slots.front();

  SmallString sick;
  sick.format(FSUI_FSTR("Do you want to continue from the automatic save created at {}?"),
              Host::FormatNumber(Host::NumberFormatType::LongDateTime, static_cast<s64>(entry.timestamp)));
  ImGui::PushFont(nullptr, 0.0f, UIStyle.BoldFontWeight);
  TextAlignedMultiLine(0.5f, IMSTR_START_END(sick));
  ImGui::PopFont();

  const GPUTexture* image = entry.preview_texture ? entry.preview_texture.get() : GetPlaceholderTexture().get();
  const float image_height = LayoutScale(280.0f);
  const float image_width =
    image_height * (static_cast<float>(image->GetWidth()) / static_cast<float>(image->GetHeight()));
  const ImVec2 pos(ImGui::GetCursorScreenPos() +
                   ImVec2((ImGui::GetCurrentWindow()->WorkRect.GetWidth() - image_width) * 0.5f, LayoutScale(20.0f)));
  const ImRect image_bb(pos, pos + ImVec2(image_width, image_height));
  ImGui::GetWindowDrawList()->AddImage(
    static_cast<ImTextureID>(entry.preview_texture ? entry.preview_texture.get() : GetPlaceholderTexture().get()),
    image_bb.Min, image_bb.Max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
    ImGui::GetColorU32(IM_COL32(255, 255, 255, 255)));

  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + image_height + LayoutScale(40.0f));

  ResetFocusHere();
  BeginMenuButtons();

  if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_FOLDER_OPEN, "Load State"), true, LAYOUT_CENTER_ALIGN_TEXT))
  {
    std::string game_path = std::move(entry.game_path);
    std::string state_path = std::move(entry.state_path);
    ClearSaveStateEntryList();
    CloseFixedPopupDialogImmediately();
    DoStartPath(std::move(game_path), std::move(state_path));
  }

  if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_PLAY, "Clean Boot"), true, LAYOUT_CENTER_ALIGN_TEXT))
  {
    std::string game_path = std::move(entry.game_path);
    ClearSaveStateEntryList();
    CloseFixedPopupDialogImmediately();
    DoStartPath(std::move(game_path));
  }

  if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_TRASH, "Delete State"), true, LAYOUT_CENTER_ALIGN_TEXT))
  {
    if (FileSystem::DeleteFile(entry.state_path.c_str()))
    {
      std::string game_path = std::move(entry.game_path);
      ClearSaveStateEntryList();
      CloseFixedPopupDialogImmediately();
      DoStartPath(std::move(game_path));
    }
    else
    {
      ShowToast(std::string(), FSUI_STR("Failed to delete save state."));
    }
  }

  if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_SQUARE_XMARK, "Cancel"), true, LAYOUT_CENTER_ALIGN_TEXT))
    CloseFixedPopupDialog();

  EndMenuButtons();

  SetStandardSelectionFooterText(false);

  EndFixedPopupDialog();
}


//////////////////////////////////////////////////////////////////////////
// Overlays
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::ExitFullscreenAndOpenURL(std::string_view url)
{
  Host::RunOnCPUThread([url = std::string(url)]() {
    if (Host::IsFullscreen())
      Host::SetFullscreen(false);

    Host::OpenURL(url);
  });
}

void FullscreenUI::CopyTextToClipboard(std::string title, std::string_view text)
{
  if (Host::CopyTextToClipboard(text))
    ShowToast(std::string(), std::move(title));
  else
    ShowToast(std::string(), FSUI_STR("Failed to copy text to clipboard."));
}

void FullscreenUI::DrawAboutWindow()
{
  if (!BeginFixedPopupDialog(LayoutScale(LAYOUT_LARGE_POPUP_PADDING), LayoutScale(LAYOUT_LARGE_POPUP_ROUNDING),
                             LayoutScale(1100.0f, 0.0f)))
  {
    return;
  }

  const ImVec2 image_size = LayoutScale(64.0f, 64.0f);
  const float indent = image_size.x + LayoutScale(8.0f);
  ImGui::GetWindowDrawList()->AddImage(s_state.app_icon_texture.get(), ImGui::GetCursorScreenPos(),
                                       ImGui::GetCursorScreenPos() + image_size);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
  ImGui::PushFont(nullptr, 0.0f, UIStyle.BoldFontWeight);
  ImGui::TextUnformatted("DuckStation");
  ImGui::PopFont();
  ImGui::PushStyleColor(ImGuiCol_Text, DarkerColor(UIStyle.BackgroundTextColor));
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
  ImGui::TextUnformatted(g_scm_tag_str);
  ImGui::PopStyleColor();

  ImGui::NewLine();
  ImGui::TextWrapped("%s", FSUI_CSTR("DuckStation is a free simulator/emulator of the Sony PlayStation(TM) "
                                     "console, focusing on playability, speed, and long-term maintainability."));
  ImGui::NewLine();
  ImGui::TextWrapped("%s",
                     FSUI_CSTR("Duck icon by icons8 (https://icons8.com/icon/74847/platforms.undefined.short-title)"));
  ImGui::NewLine();
  ImGui::TextWrapped(
    "%s", FSUI_CSTR("\"PlayStation\" and \"PSX\" are registered trademarks of Sony Interactive Entertainment Europe "
                    "Limited. This software is not affiliated in any way with Sony Interactive Entertainment."));

  ImGui::NewLine();

  BeginHorizontalMenuButtons(4);
  if (HorizontalMenuButton(FSUI_ICONVSTR(ICON_FA_GLOBE, "GitHub Repository")))
    ExitFullscreenAndOpenURL("https://github.com/stenzek/duckstation/");
  if (HorizontalMenuButton(FSUI_ICONVSTR(ICON_FA_COMMENT, "Discord Server")))
    ExitFullscreenAndOpenURL("https://www.duckstation.org/discord.html");
  if (HorizontalMenuButton(FSUI_ICONVSTR(ICON_FA_PEOPLE_CARRY_BOX, "Contributor List")))
    ExitFullscreenAndOpenURL("https://github.com/stenzek/duckstation/blob/master/CONTRIBUTORS.md");

  if (HorizontalMenuButton(FSUI_ICONVSTR(ICON_FA_SQUARE_XMARK, "Close")) || WantsToCloseMenu())
    CloseFixedPopupDialog();
  else
    SetStandardSelectionFooterText(true);

  EndHorizontalMenuButtons();

  EndFixedPopupDialog();
}

void FullscreenUI::OpenAchievementsWindow()
{
  // NOTE: Called from CPU thread.
  if (!System::IsValid())
    return;

  const auto lock = Achievements::GetLock();
  if (!Achievements::IsActive() || !Achievements::HasAchievements())
  {
    ShowToast(std::string(), Achievements::IsActive() ? FSUI_STR("This game has no achievements.") :
                                                        FSUI_STR("Achievements are not enabled."));
    return;
  }

  GPUThread::RunOnThread([]() {
    Initialize();

    PauseForMenuOpen(false);
    ForceKeyNavEnabled();

    BeginTransition(SHORT_TRANSITION_TIME, &SwitchToAchievements);
  });
}

void FullscreenUI::SwitchToAchievements()
{
  if (!Achievements::PrepareAchievementsWindow())
  {
    ClosePauseMenuImmediately();
    return;
  }

  SwitchToMainWindow(MainWindowType::Achievements);
}

void FullscreenUI::OpenLeaderboardsWindow()
{
  if (!System::IsValid())
    return;

  const auto lock = Achievements::GetLock();
  if (!Achievements::IsActive() || !Achievements::HasLeaderboards())
  {
    ShowToast(std::string(), Achievements::IsActive() ? FSUI_STR("This game has no leaderboards.") :
                                                        FSUI_STR("Achievements are not enabled."));
    return;
  }

  GPUThread::RunOnThread([]() {
    Initialize();

    PauseForMenuOpen(false);
    ForceKeyNavEnabled();

    BeginTransition(SHORT_TRANSITION_TIME, &SwitchToLeaderboards);
  });
}

void FullscreenUI::SwitchToLeaderboards()
{
  if (!Achievements::PrepareLeaderboardsWindow())
  {
    ClosePauseMenuImmediately();
    return;
  }

  SwitchToMainWindow(MainWindowType::Leaderboards);
}

FullscreenUI::BackgroundProgressCallback::BackgroundProgressCallback(std::string name)
  : ProgressCallback(), m_name(std::move(name))
{
  OpenBackgroundProgressDialog(m_name.c_str(), "", 0, 100, 0);
}

FullscreenUI::BackgroundProgressCallback::~BackgroundProgressCallback()
{
  CloseBackgroundProgressDialog(m_name.c_str());
}

void FullscreenUI::BackgroundProgressCallback::SetStatusText(const std::string_view text)
{
  ProgressCallback::SetStatusText(text);
  Redraw(true);
}

void FullscreenUI::BackgroundProgressCallback::SetProgressRange(u32 range)
{
  const u32 last_range = m_progress_range;

  ProgressCallback::SetProgressRange(range);

  if (m_progress_range != last_range)
    Redraw(false);
}

void FullscreenUI::BackgroundProgressCallback::SetProgressValue(u32 value)
{
  const u32 last_value = m_progress_value;

  ProgressCallback::SetProgressValue(value);

  if (m_progress_value != last_value)
    Redraw(false);
}

void FullscreenUI::BackgroundProgressCallback::Redraw(bool force)
{
  const int percent =
    static_cast<int>((static_cast<float>(m_progress_value) / static_cast<float>(m_progress_range)) * 100.0f);
  if (percent == m_last_progress_percent && !force)
    return;

  m_last_progress_percent = percent;
  UpdateBackgroundProgressDialog(m_name, m_status_text, 0, 100, percent);
}

void FullscreenUI::BackgroundProgressCallback::ModalError(const std::string_view message)
{
  Host::ReportErrorAsync("Error", message);
}

bool FullscreenUI::BackgroundProgressCallback::ModalConfirmation(const std::string_view message)
{
  return Host::ConfirmMessage("Confirm", message);
}

void FullscreenUI::BackgroundProgressCallback::ModalInformation(const std::string_view message)
{
  Host::ReportErrorAsync("Information", message);
}

void FullscreenUI::BackgroundProgressCallback::SetCancelled()
{
  if (m_cancellable)
    m_cancelled = true;
}

#endif // __ANDROID__

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Translation String Area
// To avoid having to type TRANSLATE("FullscreenUI", ...) everywhere, we use the shorter macros at the top
// of the file, then preprocess and generate a bunch of noops here to define the strings. Sadly that means
// the view in Linguist is gonna suck, but you can search the file for the string for more context.
/////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0
// TRANSLATION-STRING-AREA-BEGIN
TRANSLATE_NOOP("FullscreenUI", " (%u MB on disk)");
TRANSLATE_NOOP("FullscreenUI", "${title}: Title of the game.\n${filetitle}: Name component of the game's filename.\n${serial}: Serial of the game.");
TRANSLATE_NOOP("FullscreenUI", "%.1f ms");
TRANSLATE_NOOP("FullscreenUI", "%.2f Seconds");
TRANSLATE_NOOP("FullscreenUI", "%d Frames");
TRANSLATE_NOOP("FullscreenUI", "%d cycles");
TRANSLATE_NOOP("FullscreenUI", "%d ms");
TRANSLATE_NOOP("FullscreenUI", "%d sectors");
TRANSLATE_NOOP("FullscreenUI", "%u MB");
TRANSLATE_NOOP("FullscreenUI", "-");
TRANSLATE_NOOP("FullscreenUI", "1 Frame");
TRANSLATE_NOOP("FullscreenUI", "10 Frames");
TRANSLATE_NOOP("FullscreenUI", "100% [60 FPS (NTSC) / 50 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "1000% [600 FPS (NTSC) / 500 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "10x");
TRANSLATE_NOOP("FullscreenUI", "10x (20x Speed)");
TRANSLATE_NOOP("FullscreenUI", "11x");
TRANSLATE_NOOP("FullscreenUI", "125% [75 FPS (NTSC) / 62 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "12x");
TRANSLATE_NOOP("FullscreenUI", "13x");
TRANSLATE_NOOP("FullscreenUI", "14x");
TRANSLATE_NOOP("FullscreenUI", "150% [90 FPS (NTSC) / 75 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "15x");
TRANSLATE_NOOP("FullscreenUI", "16x");
TRANSLATE_NOOP("FullscreenUI", "175% [105 FPS (NTSC) / 87 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "1x");
TRANSLATE_NOOP("FullscreenUI", "2 Frames");
TRANSLATE_NOOP("FullscreenUI", "20% [12 FPS (NTSC) / 10 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "200% [120 FPS (NTSC) / 100 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "250% [150 FPS (NTSC) / 125 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "2x");
TRANSLATE_NOOP("FullscreenUI", "2x (Quad Speed)");
TRANSLATE_NOOP("FullscreenUI", "3 Frames");
TRANSLATE_NOOP("FullscreenUI", "30% [18 FPS (NTSC) / 15 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "300% [180 FPS (NTSC) / 150 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "350% [210 FPS (NTSC) / 175 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "3x");
TRANSLATE_NOOP("FullscreenUI", "3x (6x Speed)");
TRANSLATE_NOOP("FullscreenUI", "3x (for 720p)");
TRANSLATE_NOOP("FullscreenUI", "4 Frames");
TRANSLATE_NOOP("FullscreenUI", "40% [24 FPS (NTSC) / 20 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "400% [240 FPS (NTSC) / 200 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "450% [270 FPS (NTSC) / 225 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "4x");
TRANSLATE_NOOP("FullscreenUI", "4x (8x Speed)");
TRANSLATE_NOOP("FullscreenUI", "5 Frames");
TRANSLATE_NOOP("FullscreenUI", "50% [30 FPS (NTSC) / 25 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "500% [300 FPS (NTSC) / 250 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "5x");
TRANSLATE_NOOP("FullscreenUI", "5x (10x Speed)");
TRANSLATE_NOOP("FullscreenUI", "5x (for 1080p)");
TRANSLATE_NOOP("FullscreenUI", "6 Frames");
TRANSLATE_NOOP("FullscreenUI", "60% [36 FPS (NTSC) / 30 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "600% [360 FPS (NTSC) / 300 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "6x");
TRANSLATE_NOOP("FullscreenUI", "6x (12x Speed)");
TRANSLATE_NOOP("FullscreenUI", "6x (for 1440p)");
TRANSLATE_NOOP("FullscreenUI", "7 Frames");
TRANSLATE_NOOP("FullscreenUI", "70% [42 FPS (NTSC) / 35 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "700% [420 FPS (NTSC) / 350 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "7x");
TRANSLATE_NOOP("FullscreenUI", "7x (14x Speed)");
TRANSLATE_NOOP("FullscreenUI", "8 Frames");
TRANSLATE_NOOP("FullscreenUI", "80% [48 FPS (NTSC) / 40 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "800% [480 FPS (NTSC) / 400 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "8x");
TRANSLATE_NOOP("FullscreenUI", "8x (16x Speed)");
TRANSLATE_NOOP("FullscreenUI", "9 Frames");
TRANSLATE_NOOP("FullscreenUI", "90% [54 FPS (NTSC) / 45 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "900% [540 FPS (NTSC) / 450 FPS (PAL)]");
TRANSLATE_NOOP("FullscreenUI", "9x");
TRANSLATE_NOOP("FullscreenUI", "9x (18x Speed)");
TRANSLATE_NOOP("FullscreenUI", "9x (for 4K)");
TRANSLATE_NOOP("FullscreenUI", "<Parent Directory>");
TRANSLATE_NOOP("FullscreenUI", "<Use This Directory>");
TRANSLATE_NOOP("FullscreenUI", "A cover already exists for this game. Are you sure that you want to overwrite it?");
TRANSLATE_NOOP("FullscreenUI", "AMOLED");
TRANSLATE_NOOP("FullscreenUI", "About");
TRANSLATE_NOOP("FullscreenUI", "Achievement Notifications");
TRANSLATE_NOOP("FullscreenUI", "Achievement Unlock/Count");
TRANSLATE_NOOP("FullscreenUI", "Achievements");
TRANSLATE_NOOP("FullscreenUI", "Achievements Settings");
TRANSLATE_NOOP("FullscreenUI", "Achievements are not enabled.");
TRANSLATE_NOOP("FullscreenUI", "Achievements: ");
TRANSLATE_NOOP("FullscreenUI", "Activates runahead when analog input changes, which significantly increases system requirements.");
TRANSLATE_NOOP("FullscreenUI", "Add Search Directory");
TRANSLATE_NOOP("FullscreenUI", "Add Shader");
TRANSLATE_NOOP("FullscreenUI", "Adds a new directory to the game search list.");
TRANSLATE_NOOP("FullscreenUI", "Adds a new shader to the chain.");
TRANSLATE_NOOP("FullscreenUI", "Adds additional precision to PGXP data post-projection. May improve visuals in some games.");
TRANSLATE_NOOP("FullscreenUI", "Adjusts the emulation speed so the console's refresh rate matches the host when VSync is enabled.");
TRANSLATE_NOOP("FullscreenUI", "Advanced");
TRANSLATE_NOOP("FullscreenUI", "Advanced Settings");
TRANSLATE_NOOP("FullscreenUI", "All Time: {}");
TRANSLATE_NOOP("FullscreenUI", "Allow Booting Without SBI File");
TRANSLATE_NOOP("FullscreenUI", "Allows booting to continue even without a required SBI file. These games will not run correctly.");
TRANSLATE_NOOP("FullscreenUI", "Alpha Blending");
TRANSLATE_NOOP("FullscreenUI", "Always Track Uploads");
TRANSLATE_NOOP("FullscreenUI", "An error occurred while deleting empty game settings:\n{}");
TRANSLATE_NOOP("FullscreenUI", "An error occurred while saving game settings:\n{}");
TRANSLATE_NOOP("FullscreenUI", "Animates windows opening/closing and changes between views in the Big Picture UI.");
TRANSLATE_NOOP("FullscreenUI", "Appearance");
TRANSLATE_NOOP("FullscreenUI", "Apply Image Patches");
TRANSLATE_NOOP("FullscreenUI", "Are you sure you want to clear all mappings for this controller?\n\nYou cannot undo this action.");
TRANSLATE_NOOP("FullscreenUI", "Are you sure you want to clear the current post-processing chain? All configuration will be lost.");
TRANSLATE_NOOP("FullscreenUI", "Are you sure you want to restore the default controller configuration?\n\nAll bindings and configuration will be lost. You cannot undo this action.");
TRANSLATE_NOOP("FullscreenUI", "Are you sure you want to restore the default settings? Any preferences will be lost.\n\nYou cannot undo this action.");
TRANSLATE_NOOP("FullscreenUI", "Aspect Ratio");
TRANSLATE_NOOP("FullscreenUI", "Attempts to detect one pixel high/wide lines that rely on non-upscaled rasterization behavior, filling in gaps introduced by upscaling.");
TRANSLATE_NOOP("FullscreenUI", "Attempts to map the selected port to a chosen controller.");
TRANSLATE_NOOP("FullscreenUI", "Audio Backend");
TRANSLATE_NOOP("FullscreenUI", "Audio Control");
TRANSLATE_NOOP("FullscreenUI", "Audio Settings");
TRANSLATE_NOOP("FullscreenUI", "Auto-Detect");
TRANSLATE_NOOP("FullscreenUI", "Automatic");
TRANSLATE_NOOP("FullscreenUI", "Automatic Mapping");
TRANSLATE_NOOP("FullscreenUI", "Automatic based on window size");
TRANSLATE_NOOP("FullscreenUI", "Automatic mapping completed for {}.");
TRANSLATE_NOOP("FullscreenUI", "Automatic mapping failed for {}.");
TRANSLATE_NOOP("FullscreenUI", "Automatic mapping failed, no devices are available.");
TRANSLATE_NOOP("FullscreenUI", "Automatically Resize Window");
TRANSLATE_NOOP("FullscreenUI", "Automatically applies patches to disc images when they are present, currently only PPF is supported.");
TRANSLATE_NOOP("FullscreenUI", "Automatically resizes the window to match the internal resolution.");
TRANSLATE_NOOP("FullscreenUI", "Automatically saves the emulator state when powering down or exiting. You can then resume directly from where you left off next time.");
TRANSLATE_NOOP("FullscreenUI", "Automatically switches to fullscreen mode when the program is started.");
TRANSLATE_NOOP("FullscreenUI", "Automatically switches to the next disc in the game when the game stops the CD-ROM motor. Does not work for all games.");
TRANSLATE_NOOP("FullscreenUI", "Avoids calls to C++ code, significantly speeding up the recompiler.");
TRANSLATE_NOOP("FullscreenUI", "BIOS Directory");
TRANSLATE_NOOP("FullscreenUI", "BIOS Selection");
TRANSLATE_NOOP("FullscreenUI", "BIOS Settings");
TRANSLATE_NOOP("FullscreenUI", "BIOS for {}");
TRANSLATE_NOOP("FullscreenUI", "BIOS to use when emulating {} consoles.");
TRANSLATE_NOOP("FullscreenUI", "Back");
TRANSLATE_NOOP("FullscreenUI", "Back To Pause Menu");
TRANSLATE_NOOP("FullscreenUI", "Backend Settings");
TRANSLATE_NOOP("FullscreenUI", "Behavior");
TRANSLATE_NOOP("FullscreenUI", "Border Overlay");
TRANSLATE_NOOP("FullscreenUI", "Borderless Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Bottom: ");
TRANSLATE_NOOP("FullscreenUI", "Buffer Size");
TRANSLATE_NOOP("FullscreenUI", "Buttons");
TRANSLATE_NOOP("FullscreenUI", "CD-ROM Emulation");
TRANSLATE_NOOP("FullscreenUI", "CPU Emulation");
TRANSLATE_NOOP("FullscreenUI", "CPU Mode");
TRANSLATE_NOOP("FullscreenUI", "Cancel");
TRANSLATE_NOOP("FullscreenUI", "Capture");
TRANSLATE_NOOP("FullscreenUI", "Challenge Indicators");
TRANSLATE_NOOP("FullscreenUI", "Change Disc");
TRANSLATE_NOOP("FullscreenUI", "Change Page");
TRANSLATE_NOOP("FullscreenUI", "Change Selection");
TRANSLATE_NOOP("FullscreenUI", "Change View");
TRANSLATE_NOOP("FullscreenUI", "Changes settings for the application.");
TRANSLATE_NOOP("FullscreenUI", "Changes the aspect ratio used to display the console's output to the screen.");
TRANSLATE_NOOP("FullscreenUI", "Cheats");
TRANSLATE_NOOP("FullscreenUI", "Chooses the language used for UI elements.");
TRANSLATE_NOOP("FullscreenUI", "Clean Boot");
TRANSLATE_NOOP("FullscreenUI", "Clear Mappings");
TRANSLATE_NOOP("FullscreenUI", "Clear Settings");
TRANSLATE_NOOP("FullscreenUI", "Clear Shaders");
TRANSLATE_NOOP("FullscreenUI", "Clears a shader from the chain.");
TRANSLATE_NOOP("FullscreenUI", "Clears all settings set for this game.");
TRANSLATE_NOOP("FullscreenUI", "Close");
TRANSLATE_NOOP("FullscreenUI", "Close Game");
TRANSLATE_NOOP("FullscreenUI", "Close Menu");
TRANSLATE_NOOP("FullscreenUI", "Cobalt Sky");
TRANSLATE_NOOP("FullscreenUI", "Compatibility Rating");
TRANSLATE_NOOP("FullscreenUI", "Compatibility: ");
TRANSLATE_NOOP("FullscreenUI", "Completely exits the application, returning you to your desktop.");
TRANSLATE_NOOP("FullscreenUI", "Configuration");
TRANSLATE_NOOP("FullscreenUI", "Confirm Power Off");
TRANSLATE_NOOP("FullscreenUI", "Console Settings");
TRANSLATE_NOOP("FullscreenUI", "Contributor List");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {}");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {} Bindings");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {} Macros");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {} Settings");
TRANSLATE_NOOP("FullscreenUI", "Controller Settings");
TRANSLATE_NOOP("FullscreenUI", "Controller Type");
TRANSLATE_NOOP("FullscreenUI", "Controller mapping cleared.");
TRANSLATE_NOOP("FullscreenUI", "Controller preset '{}' loaded.");
TRANSLATE_NOOP("FullscreenUI", "Controller preset '{}' saved.");
TRANSLATE_NOOP("FullscreenUI", "Controller settings reset to default.");
TRANSLATE_NOOP("FullscreenUI", "Controls");
TRANSLATE_NOOP("FullscreenUI", "Controls how dithering is applied in the emulated GPU. True Color disables dithering and produces the nicest looking gradients.");
TRANSLATE_NOOP("FullscreenUI", "Controls the volume of the audio played on the host when fast forwarding.");
TRANSLATE_NOOP("FullscreenUI", "Controls the volume of the audio played on the host.");
TRANSLATE_NOOP("FullscreenUI", "Copies the current global settings to this game.");
TRANSLATE_NOOP("FullscreenUI", "Copies the global controller configuration to this game.");
TRANSLATE_NOOP("FullscreenUI", "Copy Global Settings");
TRANSLATE_NOOP("FullscreenUI", "Copy Settings");
TRANSLATE_NOOP("FullscreenUI", "Could not find any CD/DVD-ROM devices. Please ensure you have a drive connected and sufficient permissions to access it.");
TRANSLATE_NOOP("FullscreenUI", "Cover Downloader");
TRANSLATE_NOOP("FullscreenUI", "Cover Settings");
TRANSLATE_NOOP("FullscreenUI", "Cover set.");
TRANSLATE_NOOP("FullscreenUI", "Covers Directory");
TRANSLATE_NOOP("FullscreenUI", "Create");
TRANSLATE_NOOP("FullscreenUI", "Create New...");
TRANSLATE_NOOP("FullscreenUI", "Create Save State Backups");
TRANSLATE_NOOP("FullscreenUI", "Crop Mode");
TRANSLATE_NOOP("FullscreenUI", "Culling Correction");
TRANSLATE_NOOP("FullscreenUI", "Custom");
TRANSLATE_NOOP("FullscreenUI", "Dark");
TRANSLATE_NOOP("FullscreenUI", "Dark Ruby");
TRANSLATE_NOOP("FullscreenUI", "Deadzone");
TRANSLATE_NOOP("FullscreenUI", "Debugging Settings");
TRANSLATE_NOOP("FullscreenUI", "Default");
TRANSLATE_NOOP("FullscreenUI", "Default Boot");
TRANSLATE_NOOP("FullscreenUI", "Default Value");
TRANSLATE_NOOP("FullscreenUI", "Default View");
TRANSLATE_NOOP("FullscreenUI", "Default: Disabled");
TRANSLATE_NOOP("FullscreenUI", "Default: Enabled");
TRANSLATE_NOOP("FullscreenUI", "Deinterlacing Mode");
TRANSLATE_NOOP("FullscreenUI", "Delete Save");
TRANSLATE_NOOP("FullscreenUI", "Delete State");
TRANSLATE_NOOP("FullscreenUI", "Depth Clear Threshold");
TRANSLATE_NOOP("FullscreenUI", "Depth Test Transparent Polygons");
TRANSLATE_NOOP("FullscreenUI", "Desktop Mode");
TRANSLATE_NOOP("FullscreenUI", "Destination Alpha Blending");
TRANSLATE_NOOP("FullscreenUI", "Details");
TRANSLATE_NOOP("FullscreenUI", "Details unavailable for game not scanned in game list.");
TRANSLATE_NOOP("FullscreenUI", "Determines how large the on-screen messages and monitor are.");
TRANSLATE_NOOP("FullscreenUI", "Determines how much button pressure is ignored before activating the macro.");
TRANSLATE_NOOP("FullscreenUI", "Determines how much latency there is between the audio being picked up by the host API, and played through speakers.");
TRANSLATE_NOOP("FullscreenUI", "Determines how much of the area typically not visible on a consumer TV set to crop/hide.");
TRANSLATE_NOOP("FullscreenUI", "Determines how much pressure is simulated when macro is active.");
TRANSLATE_NOOP("FullscreenUI", "Determines how the emulated CPU executes instructions.");
TRANSLATE_NOOP("FullscreenUI", "Determines how the emulated console's output is upscaled or downscaled to your monitor's resolution.");
TRANSLATE_NOOP("FullscreenUI", "Determines quality of audio when not running at 100% speed.");
TRANSLATE_NOOP("FullscreenUI", "Determines that field that the game list will be sorted by.");
TRANSLATE_NOOP("FullscreenUI", "Determines the amount of audio buffered before being pulled by the host API.");
TRANSLATE_NOOP("FullscreenUI", "Determines the area of the overlay image that the display will be drawn within.");
TRANSLATE_NOOP("FullscreenUI", "Determines the emulated hardware type.");
TRANSLATE_NOOP("FullscreenUI", "Determines the format that screenshots will be saved/compressed with.");
TRANSLATE_NOOP("FullscreenUI", "Determines the frequency at which the macro will toggle the buttons on and off (aka auto fire).");
TRANSLATE_NOOP("FullscreenUI", "Determines the margin between the edge of the screen and on-screen messages.");
TRANSLATE_NOOP("FullscreenUI", "Determines the position on the screen when black borders must be added.");
TRANSLATE_NOOP("FullscreenUI", "Determines the rotation of the simulated TV screen.");
TRANSLATE_NOOP("FullscreenUI", "Determines the scaling algorithm used when 24-bit content is active, typically FMVs.");
TRANSLATE_NOOP("FullscreenUI", "Determines the size of screenshots created by DuckStation.");
TRANSLATE_NOOP("FullscreenUI", "Determines whether a prompt will be displayed to confirm shutting down the emulator/game when the hotkey is pressed.");
TRANSLATE_NOOP("FullscreenUI", "Determines which algorithm is used to convert interlaced frames to progressive for display on your system.");
TRANSLATE_NOOP("FullscreenUI", "Device Settings");
TRANSLATE_NOOP("FullscreenUI", "Disable Mailbox Presentation");
TRANSLATE_NOOP("FullscreenUI", "Disable Speedup on MDEC");
TRANSLATE_NOOP("FullscreenUI", "Disable Subdirectory Scanning");
TRANSLATE_NOOP("FullscreenUI", "Disable on 2D Polygons");
TRANSLATE_NOOP("FullscreenUI", "Disabled");
TRANSLATE_NOOP("FullscreenUI", "Disc");
TRANSLATE_NOOP("FullscreenUI", "Discord Server");
TRANSLATE_NOOP("FullscreenUI", "Display Area");
TRANSLATE_NOOP("FullscreenUI", "Displays DualShock/DualSense button icons in the footer and input binding, instead of Xbox buttons.");
TRANSLATE_NOOP("FullscreenUI", "Displays only the game title in the list, instead of the title and serial/file name.");
TRANSLATE_NOOP("FullscreenUI", "Displays popup messages on events such as achievement unlocks and leaderboard submissions.");
TRANSLATE_NOOP("FullscreenUI", "Displays popup messages when starting, submitting, or failing a leaderboard challenge.");
TRANSLATE_NOOP("FullscreenUI", "Dithering");
TRANSLATE_NOOP("FullscreenUI", "Do you want to continue from the automatic save created at {}?");
TRANSLATE_NOOP("FullscreenUI", "Double-Click Toggles Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Download Covers");
TRANSLATE_NOOP("FullscreenUI", "Downloads covers from a user-specified URL template.");
TRANSLATE_NOOP("FullscreenUI", "Downsamples the rendered image prior to displaying it. Can improve overall image quality in mixed 2D/3D games.");
TRANSLATE_NOOP("FullscreenUI", "Downsampling");
TRANSLATE_NOOP("FullscreenUI", "Downsampling Display Scale");
TRANSLATE_NOOP("FullscreenUI", "Draws a border around the currently-selected item for readability.");
TRANSLATE_NOOP("FullscreenUI", "Duck icon by icons8 (https://icons8.com/icon/74847/platforms.undefined.short-title)");
TRANSLATE_NOOP("FullscreenUI", "DuckStation can automatically download covers for games which do not currently have a cover set. We do not host any cover images, the user must provide their own source for images.");
TRANSLATE_NOOP("FullscreenUI", "DuckStation is a free simulator/emulator of the Sony PlayStation(TM) console, focusing on playability, speed, and long-term maintainability.");
TRANSLATE_NOOP("FullscreenUI", "Dump Replaced Textures");
TRANSLATE_NOOP("FullscreenUI", "Dumps textures that have replacements already loaded.");
TRANSLATE_NOOP("FullscreenUI", "Emulation Settings");
TRANSLATE_NOOP("FullscreenUI", "Emulation Speed");
TRANSLATE_NOOP("FullscreenUI", "Enable 8MB RAM");
TRANSLATE_NOOP("FullscreenUI", "Enable Achievements");
TRANSLATE_NOOP("FullscreenUI", "Enable Achievements to see your user summary.");
TRANSLATE_NOOP("FullscreenUI", "Enable Cheats");
TRANSLATE_NOOP("FullscreenUI", "Enable Discord Presence");
TRANSLATE_NOOP("FullscreenUI", "Enable Fast Boot");
TRANSLATE_NOOP("FullscreenUI", "Enable GPU-Based Validation");
TRANSLATE_NOOP("FullscreenUI", "Enable GPU-based validation when supported by the host's renderer API. Only for developer use.");
TRANSLATE_NOOP("FullscreenUI", "Enable Overclocking");
TRANSLATE_NOOP("FullscreenUI", "Enable Post Processing");
TRANSLATE_NOOP("FullscreenUI", "Enable Recompiler Block Linking");
TRANSLATE_NOOP("FullscreenUI", "Enable Recompiler ICache");
TRANSLATE_NOOP("FullscreenUI", "Enable Recompiler Memory Exceptions");
TRANSLATE_NOOP("FullscreenUI", "Enable Region Check");
TRANSLATE_NOOP("FullscreenUI", "Enable Rewinding");
TRANSLATE_NOOP("FullscreenUI", "Enable SDL Input Source");
TRANSLATE_NOOP("FullscreenUI", "Enable Subdirectory Scanning");
TRANSLATE_NOOP("FullscreenUI", "Enable TTY Logging");
TRANSLATE_NOOP("FullscreenUI", "Enable Texture Cache");
TRANSLATE_NOOP("FullscreenUI", "Enable Texture Dumping");
TRANSLATE_NOOP("FullscreenUI", "Enable Texture Replacements");
TRANSLATE_NOOP("FullscreenUI", "Enable VRAM Write Dumping");
TRANSLATE_NOOP("FullscreenUI", "Enable VRAM Write Replacement");
TRANSLATE_NOOP("FullscreenUI", "Enable XInput Input Source");
TRANSLATE_NOOP("FullscreenUI", "Enable debugging when supported by the host's renderer API. Only for developer use.");
TRANSLATE_NOOP("FullscreenUI", "Enable/Disable the Player LED on DualSense controllers.");
TRANSLATE_NOOP("FullscreenUI", "Enables alignment and bus exceptions. Not needed for any known games.");
TRANSLATE_NOOP("FullscreenUI", "Enables an additional 6MB of RAM to obtain a total of 2+6 = 8MB, usually present on dev consoles.");
TRANSLATE_NOOP("FullscreenUI", "Enables an additional three controller slots on each port. Not supported in all games.");
TRANSLATE_NOOP("FullscreenUI", "Enables caching of guest textures, required for texture replacement.");
TRANSLATE_NOOP("FullscreenUI", "Enables depth testing for semi-transparent polygons. Usually these include shadows, and tend to clip through the ground when depth testing is enabled.");
TRANSLATE_NOOP("FullscreenUI", "Enables dumping of textures to image files, which can be replaced. Not compatible with all games.");
TRANSLATE_NOOP("FullscreenUI", "Enables loading of cheats for this game from DuckStation's database.");
TRANSLATE_NOOP("FullscreenUI", "Enables loading of replacement textures. Not compatible with all games.");
TRANSLATE_NOOP("FullscreenUI", "Enables smooth scrolling of menus in the Big Picture UI.");
TRANSLATE_NOOP("FullscreenUI", "Enables the cheats that are selected below.");
TRANSLATE_NOOP("FullscreenUI", "Enables the older, less accurate MDEC decoding routines. May be required for old replacement backgrounds to match/load.");
TRANSLATE_NOOP("FullscreenUI", "Enables the replacement of background textures in supported games.");
TRANSLATE_NOOP("FullscreenUI", "Encore Mode");
TRANSLATE_NOOP("FullscreenUI", "Ensures every frame generated is displayed for optimal pacing. Enable for variable refresh displays, such as GSync/FreeSync. Disable if you are having speed or sound issues.");
TRANSLATE_NOOP("FullscreenUI", "Enter Value");
TRANSLATE_NOOP("FullscreenUI", "Enter the name of the controller preset you wish to create.");
TRANSLATE_NOOP("FullscreenUI", "Error");
TRANSLATE_NOOP("FullscreenUI", "Example: https://www.example-not-a-real-domain.com/covers/${serial}.jpg");
TRANSLATE_NOOP("FullscreenUI", "Execution Mode");
TRANSLATE_NOOP("FullscreenUI", "Exit");
TRANSLATE_NOOP("FullscreenUI", "Exit And Save State");
TRANSLATE_NOOP("FullscreenUI", "Exit DuckStation");
TRANSLATE_NOOP("FullscreenUI", "Exit Without Saving");
TRANSLATE_NOOP("FullscreenUI", "Exits Big Picture mode, returning to the desktop interface.");
TRANSLATE_NOOP("FullscreenUI", "FMV Chroma Smoothing");
TRANSLATE_NOOP("FullscreenUI", "FMV Scaling");
TRANSLATE_NOOP("FullscreenUI", "Failed to copy cover: {}");
TRANSLATE_NOOP("FullscreenUI", "Failed to copy text to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Failed to delete existing cover: {}");
TRANSLATE_NOOP("FullscreenUI", "Failed to delete save state.");
TRANSLATE_NOOP("FullscreenUI", "Failed to delete {}.");
TRANSLATE_NOOP("FullscreenUI", "Failed to load '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Failed to load shader {}. It may be invalid.\nError was:");
TRANSLATE_NOOP("FullscreenUI", "Failed to save controller preset '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Failed to update progress database");
TRANSLATE_NOOP("FullscreenUI", "Fast Boot");
TRANSLATE_NOOP("FullscreenUI", "Fast Forward Boot");
TRANSLATE_NOOP("FullscreenUI", "Fast Forward Memory Card Access");
TRANSLATE_NOOP("FullscreenUI", "Fast Forward Speed");
TRANSLATE_NOOP("FullscreenUI", "Fast Forward Volume");
TRANSLATE_NOOP("FullscreenUI", "Fast forwards through memory card access, both loading and saving. Can reduce waiting times in games that frequently access memory cards.");
TRANSLATE_NOOP("FullscreenUI", "Fast forwards through the early loading process when fast booting, saving time. Results may vary between games.");
TRANSLATE_NOOP("FullscreenUI", "File Size");
TRANSLATE_NOOP("FullscreenUI", "File Title");
TRANSLATE_NOOP("FullscreenUI", "Force 4:3 For FMVs");
TRANSLATE_NOOP("FullscreenUI", "Forces a full rescan of all games previously identified.");
TRANSLATE_NOOP("FullscreenUI", "Forces texture upload tracking to be enabled regardless of whether it is needed.");
TRANSLATE_NOOP("FullscreenUI", "Forces the use of FIFO over Mailbox presentation, i.e. double buffering instead of triple buffering. Usually results in worse frame pacing.");
TRANSLATE_NOOP("FullscreenUI", "Forcibly mutes both CD-DA and XA audio from the CD-ROM. Can be used to disable background music in some games.");
TRANSLATE_NOOP("FullscreenUI", "Frame Rate");
TRANSLATE_NOOP("FullscreenUI", "Frame Time Buffer");
TRANSLATE_NOOP("FullscreenUI", "Frequency");
TRANSLATE_NOOP("FullscreenUI", "From File...");
TRANSLATE_NOOP("FullscreenUI", "Fullscreen Resolution");
TRANSLATE_NOOP("FullscreenUI", "GPU Adapter");
TRANSLATE_NOOP("FullscreenUI", "GPU Renderer");
TRANSLATE_NOOP("FullscreenUI", "Game Display");
TRANSLATE_NOOP("FullscreenUI", "Game Grid");
TRANSLATE_NOOP("FullscreenUI", "Game List");
TRANSLATE_NOOP("FullscreenUI", "Game List Settings");
TRANSLATE_NOOP("FullscreenUI", "Game Patches");
TRANSLATE_NOOP("FullscreenUI", "Game Properties");
TRANSLATE_NOOP("FullscreenUI", "Game Quick Save");
TRANSLATE_NOOP("FullscreenUI", "Game Slot {0}##game_slot_{0}");
TRANSLATE_NOOP("FullscreenUI", "Game compatibility rating copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game path copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game region copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game serial copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game settings have been cleared for '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Game settings initialized with global settings for '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Game title copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game type copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Genre: ");
TRANSLATE_NOOP("FullscreenUI", "Geometry Tolerance");
TRANSLATE_NOOP("FullscreenUI", "GitHub Repository");
TRANSLATE_NOOP("FullscreenUI", "Global Slot {0} - {1}##global_slot_{0}");
TRANSLATE_NOOP("FullscreenUI", "Global Slot {0}##global_slot_{0}");
TRANSLATE_NOOP("FullscreenUI", "Graphics Settings");
TRANSLATE_NOOP("FullscreenUI", "Green Giant");
TRANSLATE_NOOP("FullscreenUI", "Grey Matter");
TRANSLATE_NOOP("FullscreenUI", "Hardcore Mode");
TRANSLATE_NOOP("FullscreenUI", "Hardcore mode will not be enabled until the system is reset. Do you want to reset the system now?");
TRANSLATE_NOOP("FullscreenUI", "Hide Cursor In Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Hides the mouse pointer/cursor when the emulator is in fullscreen mode.");
TRANSLATE_NOOP("FullscreenUI", "Hotkey Settings");
TRANSLATE_NOOP("FullscreenUI", "How many saves will be kept for rewinding. Higher values have greater memory requirements.");
TRANSLATE_NOOP("FullscreenUI", "How often a rewind state will be created. Higher frequencies have greater system requirements.");
TRANSLATE_NOOP("FullscreenUI", "Identifies any new files added to the game directories.");
TRANSLATE_NOOP("FullscreenUI", "If enabled, the display will be blended with the transparency of the overlay image.");
TRANSLATE_NOOP("FullscreenUI", "If enabled, the transparency of the overlay image will be applied.");
TRANSLATE_NOOP("FullscreenUI", "If not enabled, the current post processing chain will be ignored.");
TRANSLATE_NOOP("FullscreenUI", "Ignore Drive Subcode");
TRANSLATE_NOOP("FullscreenUI", "Ignores the subchannel provided by the drive when using physical discs, instead always generating subchannel data. Can improve read reliability on some drives.");
TRANSLATE_NOOP("FullscreenUI", "Image Path");
TRANSLATE_NOOP("FullscreenUI", "In the form below, specify the URLs to download covers from, with one template URL per line. The following variables are available:");
TRANSLATE_NOOP("FullscreenUI", "Includes the elapsed time since the application start in file logs.");
TRANSLATE_NOOP("FullscreenUI", "Includes the elapsed time since the application start in window and console logs.");
TRANSLATE_NOOP("FullscreenUI", "Increases the field of view from 4:3 to the chosen display aspect ratio in 3D games.");
TRANSLATE_NOOP("FullscreenUI", "Increases the precision of polygon culling, reducing the number of holes in geometry.");
TRANSLATE_NOOP("FullscreenUI", "Inhibit Screensaver");
TRANSLATE_NOOP("FullscreenUI", "Input Sources");
TRANSLATE_NOOP("FullscreenUI", "Interface Settings");
TRANSLATE_NOOP("FullscreenUI", "Internal Resolution");
TRANSLATE_NOOP("FullscreenUI", "Language");
TRANSLATE_NOOP("FullscreenUI", "Language: ");
TRANSLATE_NOOP("FullscreenUI", "Last Played");
TRANSLATE_NOOP("FullscreenUI", "Last Played: ");
TRANSLATE_NOOP("FullscreenUI", "Latency Control");
TRANSLATE_NOOP("FullscreenUI", "Launch Options");
TRANSLATE_NOOP("FullscreenUI", "Launch a game by selecting a file/disc image.");
TRANSLATE_NOOP("FullscreenUI", "Launch a game from a file, disc, or starts the console without any disc inserted.");
TRANSLATE_NOOP("FullscreenUI", "Launch a game from images scanned from your game directories.");
TRANSLATE_NOOP("FullscreenUI", "Leaderboard Notifications");
TRANSLATE_NOOP("FullscreenUI", "Leaderboard Trackers");
TRANSLATE_NOOP("FullscreenUI", "Leaderboards");
TRANSLATE_NOOP("FullscreenUI", "Left: ");
TRANSLATE_NOOP("FullscreenUI", "Light");
TRANSLATE_NOOP("FullscreenUI", "Line Detection");
TRANSLATE_NOOP("FullscreenUI", "List Compact Mode");
TRANSLATE_NOOP("FullscreenUI", "List Settings");
TRANSLATE_NOOP("FullscreenUI", "Load Database Cheats");
TRANSLATE_NOOP("FullscreenUI", "Load Devices From Save States");
TRANSLATE_NOOP("FullscreenUI", "Load Global State");
TRANSLATE_NOOP("FullscreenUI", "Load Preset");
TRANSLATE_NOOP("FullscreenUI", "Load State");
TRANSLATE_NOOP("FullscreenUI", "Loads all replacement texture to RAM, reducing stuttering at runtime.");
TRANSLATE_NOOP("FullscreenUI", "Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay.");
TRANSLATE_NOOP("FullscreenUI", "Log File Timestamps");
TRANSLATE_NOOP("FullscreenUI", "Log Level");
TRANSLATE_NOOP("FullscreenUI", "Log Timestamps");
TRANSLATE_NOOP("FullscreenUI", "Log To Debug Console");
TRANSLATE_NOOP("FullscreenUI", "Log To File");
TRANSLATE_NOOP("FullscreenUI", "Log To System Console");
TRANSLATE_NOOP("FullscreenUI", "Logging In...");
TRANSLATE_NOOP("FullscreenUI", "Logging Settings");
TRANSLATE_NOOP("FullscreenUI", "Logging in to RetroAchievements...");
TRANSLATE_NOOP("FullscreenUI", "Login");
TRANSLATE_NOOP("FullscreenUI", "Login Failed.\nError: {}\nPlease check your username and password, and try again.");
TRANSLATE_NOOP("FullscreenUI", "Login token generated on {}");
TRANSLATE_NOOP("FullscreenUI", "Logout");
TRANSLATE_NOOP("FullscreenUI", "Logs BIOS calls to printf(). Not all games contain debugging messages.");
TRANSLATE_NOOP("FullscreenUI", "Logs messages to duckstation.log in the user directory.");
TRANSLATE_NOOP("FullscreenUI", "Logs messages to the console window.");
TRANSLATE_NOOP("FullscreenUI", "Logs messages to the debug console where supported.");
TRANSLATE_NOOP("FullscreenUI", "Macro Button {}");
TRANSLATE_NOOP("FullscreenUI", "Makes games run closer to their console framerate, at a small cost to performance.");
TRANSLATE_NOOP("FullscreenUI", "Maximum");
TRANSLATE_NOOP("FullscreenUI", "Maximum Read Speedup Cycles");
TRANSLATE_NOOP("FullscreenUI", "Maximum Seek Speedup Cycles");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Busy");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Directory");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Port {}");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Settings");
TRANSLATE_NOOP("FullscreenUI", "Memory Card {} Type");
TRANSLATE_NOOP("FullscreenUI", "Menu Background");
TRANSLATE_NOOP("FullscreenUI", "Menu Borders");
TRANSLATE_NOOP("FullscreenUI", "Merge Multi-Disc Games");
TRANSLATE_NOOP("FullscreenUI", "Merges multi-disc games into one item in the game list.");
TRANSLATE_NOOP("FullscreenUI", "Minimal Output Latency");
TRANSLATE_NOOP("FullscreenUI", "Move Down");
TRANSLATE_NOOP("FullscreenUI", "Move Up");
TRANSLATE_NOOP("FullscreenUI", "Moves this shader higher in the chain, applying it earlier.");
TRANSLATE_NOOP("FullscreenUI", "Moves this shader lower in the chain, applying it later.");
TRANSLATE_NOOP("FullscreenUI", "Multitap");
TRANSLATE_NOOP("FullscreenUI", "Multitap Mode");
TRANSLATE_NOOP("FullscreenUI", "Mute All Sound");
TRANSLATE_NOOP("FullscreenUI", "Mute CD Audio");
TRANSLATE_NOOP("FullscreenUI", "Navigate");
TRANSLATE_NOOP("FullscreenUI", "No");
TRANSLATE_NOOP("FullscreenUI", "No Game Selected");
TRANSLATE_NOOP("FullscreenUI", "No LED");
TRANSLATE_NOOP("FullscreenUI", "No Vibration");
TRANSLATE_NOOP("FullscreenUI", "No cheats are available for this game.");
TRANSLATE_NOOP("FullscreenUI", "No devices with vibration motors were detected.");
TRANSLATE_NOOP("FullscreenUI", "No input profiles available.");
TRANSLATE_NOOP("FullscreenUI", "No patches are available for this game.");
TRANSLATE_NOOP("FullscreenUI", "No resume save state found.");
TRANSLATE_NOOP("FullscreenUI", "No save present in this slot.");
TRANSLATE_NOOP("FullscreenUI", "No save states found.");
TRANSLATE_NOOP("FullscreenUI", "No, resume the game.");
TRANSLATE_NOOP("FullscreenUI", "None");
TRANSLATE_NOOP("FullscreenUI", "None (Double Speed)");
TRANSLATE_NOOP("FullscreenUI", "None (Normal Speed)");
TRANSLATE_NOOP("FullscreenUI", "Not Logged In");
TRANSLATE_NOOP("FullscreenUI", "Not Scanning Subdirectories");
TRANSLATE_NOOP("FullscreenUI", "OK");
TRANSLATE_NOOP("FullscreenUI", "OSD Scale");
TRANSLATE_NOOP("FullscreenUI", "On-Screen Display");
TRANSLATE_NOOP("FullscreenUI", "Open Containing Directory");
TRANSLATE_NOOP("FullscreenUI", "Open To Game List");
TRANSLATE_NOOP("FullscreenUI", "Open in File Browser");
TRANSLATE_NOOP("FullscreenUI", "Operations");
TRANSLATE_NOOP("FullscreenUI", "Optimal Frame Pacing");
TRANSLATE_NOOP("FullscreenUI", "Options");
TRANSLATE_NOOP("FullscreenUI", "Output Latency");
TRANSLATE_NOOP("FullscreenUI", "Output Volume");
TRANSLATE_NOOP("FullscreenUI", "Overclocking Percentage");
TRANSLATE_NOOP("FullscreenUI", "Overlays or replaces normal triangle drawing with a wireframe/line view.");
TRANSLATE_NOOP("FullscreenUI", "PGXP (Precision Geometry Transform Pipeline)");
TRANSLATE_NOOP("FullscreenUI", "PGXP Depth Buffer");
TRANSLATE_NOOP("FullscreenUI", "PGXP Geometry Correction");
TRANSLATE_NOOP("FullscreenUI", "Parent Directory");
TRANSLATE_NOOP("FullscreenUI", "Password");
TRANSLATE_NOOP("FullscreenUI", "Patches");
TRANSLATE_NOOP("FullscreenUI", "Patches the BIOS to skip the boot animation. Safe to enable.");
TRANSLATE_NOOP("FullscreenUI", "Path");
TRANSLATE_NOOP("FullscreenUI", "Pause On Controller Disconnection");
TRANSLATE_NOOP("FullscreenUI", "Pause On Focus Loss");
TRANSLATE_NOOP("FullscreenUI", "Pause On Start");
TRANSLATE_NOOP("FullscreenUI", "Pauses the emulator when a controller with bindings is disconnected.");
TRANSLATE_NOOP("FullscreenUI", "Pauses the emulator when a game is started.");
TRANSLATE_NOOP("FullscreenUI", "Pauses the emulator when you minimize the window or switch to another application, and unpauses when you switch back.");
TRANSLATE_NOOP("FullscreenUI", "Per-Game Configuration");
TRANSLATE_NOOP("FullscreenUI", "Per-game controller configuration initialized with global settings.");
TRANSLATE_NOOP("FullscreenUI", "Performance enhancement - jumps directly between blocks instead of returning to the dispatcher.");
TRANSLATE_NOOP("FullscreenUI", "Perspective Correct Colors");
TRANSLATE_NOOP("FullscreenUI", "Perspective Correct Textures");
TRANSLATE_NOOP("FullscreenUI", "Pinky Pals");
TRANSLATE_NOOP("FullscreenUI", "Plays sound effects for events such as achievement unlocks and leaderboard submissions.");
TRANSLATE_NOOP("FullscreenUI", "Please enter your user name and password for retroachievements.org below. Your password will not be saved in DuckStation, an access token will be generated and used instead.");
TRANSLATE_NOOP("FullscreenUI", "Port {} Controller Type");
TRANSLATE_NOOP("FullscreenUI", "Post-Processing Settings");
TRANSLATE_NOOP("FullscreenUI", "Post-processing chain cleared.");
TRANSLATE_NOOP("FullscreenUI", "Post-processing shaders reloaded.");
TRANSLATE_NOOP("FullscreenUI", "Prefer OpenGL ES Context");
TRANSLATE_NOOP("FullscreenUI", "Preload Images to RAM");
TRANSLATE_NOOP("FullscreenUI", "Preload Replacement Textures");
TRANSLATE_NOOP("FullscreenUI", "Preserve Projection Precision");
TRANSLATE_NOOP("FullscreenUI", "Press To Toggle");
TRANSLATE_NOOP("FullscreenUI", "Pressure");
TRANSLATE_NOOP("FullscreenUI", "Prevents the emulator from producing any audible sound.");
TRANSLATE_NOOP("FullscreenUI", "Prevents the screen saver from activating and the host from sleeping while emulation is running.");
TRANSLATE_NOOP("FullscreenUI", "Progress Indicators");
TRANSLATE_NOOP("FullscreenUI", "Provides vibration and LED control support over Bluetooth.");
TRANSLATE_NOOP("FullscreenUI", "Purple Rain");
TRANSLATE_NOOP("FullscreenUI", "Push a controller button or axis now.");
TRANSLATE_NOOP("FullscreenUI", "Quick Save");
TRANSLATE_NOOP("FullscreenUI", "Read Speedup");
TRANSLATE_NOOP("FullscreenUI", "Readahead Sectors");
TRANSLATE_NOOP("FullscreenUI", "Recompiler Fast Memory Access");
TRANSLATE_NOOP("FullscreenUI", "Reduce Input Latency");
TRANSLATE_NOOP("FullscreenUI", "Reduces \"wobbly\" polygons by attempting to preserve the fractional component through memory transfers.");
TRANSLATE_NOOP("FullscreenUI", "Reduces hitches in emulation by reading/decompressing CD data asynchronously on a worker thread.");
TRANSLATE_NOOP("FullscreenUI", "Reduces input latency by delaying the start of frame until closer to the presentation time.");
TRANSLATE_NOOP("FullscreenUI", "Reduces polygon Z-fighting through depth testing. Low compatibility with games.");
TRANSLATE_NOOP("FullscreenUI", "Reduces the size of save states by compressing the data before saving.");
TRANSLATE_NOOP("FullscreenUI", "Region");
TRANSLATE_NOOP("FullscreenUI", "Region: ");
TRANSLATE_NOOP("FullscreenUI", "Release Date: ");
TRANSLATE_NOOP("FullscreenUI", "Reload Shaders");
TRANSLATE_NOOP("FullscreenUI", "Reloads the shaders from disk, applying any changes.");
TRANSLATE_NOOP("FullscreenUI", "Remove From Chain");
TRANSLATE_NOOP("FullscreenUI", "Remove From List");
TRANSLATE_NOOP("FullscreenUI", "Removed stage {} ({}).");
TRANSLATE_NOOP("FullscreenUI", "Removes all bindings for this controller port.");
TRANSLATE_NOOP("FullscreenUI", "Removes this shader from the chain.");
TRANSLATE_NOOP("FullscreenUI", "Renames existing save states when saving to a backup file.");
TRANSLATE_NOOP("FullscreenUI", "Rendering");
TRANSLATE_NOOP("FullscreenUI", "Replaces these settings with a previously saved controller preset.");
TRANSLATE_NOOP("FullscreenUI", "Rescan All Games");
TRANSLATE_NOOP("FullscreenUI", "Reset Controller Settings");
TRANSLATE_NOOP("FullscreenUI", "Reset Memory Card Directory");
TRANSLATE_NOOP("FullscreenUI", "Reset Play Time");
TRANSLATE_NOOP("FullscreenUI", "Reset Settings");
TRANSLATE_NOOP("FullscreenUI", "Reset System");
TRANSLATE_NOOP("FullscreenUI", "Resets all configuration to defaults (including bindings).");
TRANSLATE_NOOP("FullscreenUI", "Resets all settings to the defaults.");
TRANSLATE_NOOP("FullscreenUI", "Resets memory card directory to default (user directory).");
TRANSLATE_NOOP("FullscreenUI", "Resolution change will be applied after restarting.");
TRANSLATE_NOOP("FullscreenUI", "Restore Defaults");
TRANSLATE_NOOP("FullscreenUI", "Restores the state of the system prior to the last state loaded.");
TRANSLATE_NOOP("FullscreenUI", "Resume Game");
TRANSLATE_NOOP("FullscreenUI", "Resume Last Session");
TRANSLATE_NOOP("FullscreenUI", "Return To Game");
TRANSLATE_NOOP("FullscreenUI", "Return to desktop mode, or exit the application.");
TRANSLATE_NOOP("FullscreenUI", "Return to the previous menu.");
TRANSLATE_NOOP("FullscreenUI", "Reverses the game list sort order from the default (usually ascending to descending).");
TRANSLATE_NOOP("FullscreenUI", "Rewind Save Frequency");
TRANSLATE_NOOP("FullscreenUI", "Rewind Save Slots");
TRANSLATE_NOOP("FullscreenUI", "Rewind for {0} frames, lasting {1:.2f} seconds will require up to {2} MB of RAM and {3} MB of VRAM.");
TRANSLATE_NOOP("FullscreenUI", "Rewind is disabled because runahead is enabled. Runahead will significantly increase system requirements.");
TRANSLATE_NOOP("FullscreenUI", "Rewind is not enabled. Please note that enabling rewind may significantly increase system requirements.");
TRANSLATE_NOOP("FullscreenUI", "Right: ");
TRANSLATE_NOOP("FullscreenUI", "Round Upscaled Texture Coordinates");
TRANSLATE_NOOP("FullscreenUI", "Rounds texture coordinates instead of flooring when upscaling. Can fix misaligned textures in some games, but break others, and is incompatible with texture filtering.");
TRANSLATE_NOOP("FullscreenUI", "Runahead");
TRANSLATE_NOOP("FullscreenUI", "Runahead for Analog Input");
TRANSLATE_NOOP("FullscreenUI", "Runahead/Rewind");
TRANSLATE_NOOP("FullscreenUI", "Runs the software renderer in parallel for VRAM readbacks. On some systems, this may result in greater performance when using graphical enhancements with the hardware renderer.");
TRANSLATE_NOOP("FullscreenUI", "SDL DualSense Player LED");
TRANSLATE_NOOP("FullscreenUI", "SDL DualShock 4 / DualSense Enhanced Mode");
TRANSLATE_NOOP("FullscreenUI", "Safe Mode");
TRANSLATE_NOOP("FullscreenUI", "Save Controller Preset");
TRANSLATE_NOOP("FullscreenUI", "Save Preset");
TRANSLATE_NOOP("FullscreenUI", "Save Screenshot");
TRANSLATE_NOOP("FullscreenUI", "Save State");
TRANSLATE_NOOP("FullscreenUI", "Save State Compression");
TRANSLATE_NOOP("FullscreenUI", "Save State On Shutdown");
TRANSLATE_NOOP("FullscreenUI", "Save as Serial File Names");
TRANSLATE_NOOP("FullscreenUI", "Saved {}");
TRANSLATE_NOOP("FullscreenUI", "Saves state periodically so you can rewind any mistakes while playing.");
TRANSLATE_NOOP("FullscreenUI", "Scaled Interlacing");
TRANSLATE_NOOP("FullscreenUI", "Scales line skipping in interlaced rendering to the internal resolution, making it less noticeable. Usually safe to enable.");
TRANSLATE_NOOP("FullscreenUI", "Scaling");
TRANSLATE_NOOP("FullscreenUI", "Scan For New Games");
TRANSLATE_NOOP("FullscreenUI", "Scanning Subdirectories");
TRANSLATE_NOOP("FullscreenUI", "Screen Margins");
TRANSLATE_NOOP("FullscreenUI", "Screen Position");
TRANSLATE_NOOP("FullscreenUI", "Screen Rotation");
TRANSLATE_NOOP("FullscreenUI", "Screenshot Format");
TRANSLATE_NOOP("FullscreenUI", "Screenshot Quality");
TRANSLATE_NOOP("FullscreenUI", "Screenshot Size");
TRANSLATE_NOOP("FullscreenUI", "Search Directories");
TRANSLATE_NOOP("FullscreenUI", "Seek Speedup");
TRANSLATE_NOOP("FullscreenUI", "Select");
TRANSLATE_NOOP("FullscreenUI", "Select Device");
TRANSLATE_NOOP("FullscreenUI", "Select Disc");
TRANSLATE_NOOP("FullscreenUI", "Select Disc Drive");
TRANSLATE_NOOP("FullscreenUI", "Select Disc Image");
TRANSLATE_NOOP("FullscreenUI", "Select Disc for {}");
TRANSLATE_NOOP("FullscreenUI", "Select Game");
TRANSLATE_NOOP("FullscreenUI", "Select Macro {} Binds");
TRANSLATE_NOOP("FullscreenUI", "Select State");
TRANSLATE_NOOP("FullscreenUI", "Select from the list of preset borders, or manually specify a custom configuration.");
TRANSLATE_NOOP("FullscreenUI", "Selected Preset");
TRANSLATE_NOOP("FullscreenUI", "Selects the GPU to use for rendering.");
TRANSLATE_NOOP("FullscreenUI", "Selects the backend to use for rendering the console/game visuals.");
TRANSLATE_NOOP("FullscreenUI", "Selects the color style to be used for Big Picture UI.");
TRANSLATE_NOOP("FullscreenUI", "Selects the percentage of the normal clock speed the emulated hardware will run at.");
TRANSLATE_NOOP("FullscreenUI", "Selects the quality at which screenshots will be compressed.");
TRANSLATE_NOOP("FullscreenUI", "Selects the resolution scale that will be applied to the final image. 1x will downsample to the original console resolution.");
TRANSLATE_NOOP("FullscreenUI", "Selects the resolution to use in fullscreen modes.");
TRANSLATE_NOOP("FullscreenUI", "Selects the type of emulated controller for this port.");
TRANSLATE_NOOP("FullscreenUI", "Selects the view that the game list will open to.");
TRANSLATE_NOOP("FullscreenUI", "Serial");
TRANSLATE_NOOP("FullscreenUI", "Session: {}");
TRANSLATE_NOOP("FullscreenUI", "Set Cover Image");
TRANSLATE_NOOP("FullscreenUI", "Set Input Binding");
TRANSLATE_NOOP("FullscreenUI", "Sets a threshold for discarding precise values when exceeded. May help with glitches in some games.");
TRANSLATE_NOOP("FullscreenUI", "Sets a threshold for discarding the emulated depth buffer. May help in some games.");
TRANSLATE_NOOP("FullscreenUI", "Sets the fast forward speed. It is not guaranteed that this speed will be reached on all systems.");
TRANSLATE_NOOP("FullscreenUI", "Sets the minimum delay for the 'Maximum' read speedup level.");
TRANSLATE_NOOP("FullscreenUI", "Sets the minimum delay for the 'Maximum' seek speedup level.");
TRANSLATE_NOOP("FullscreenUI", "Sets the target emulation speed. It is not guaranteed that this speed will be reached on all systems.");
TRANSLATE_NOOP("FullscreenUI", "Sets the turbo speed. It is not guaranteed that this speed will be reached on all systems.");
TRANSLATE_NOOP("FullscreenUI", "Sets the verbosity of messages logged. Higher levels will log more messages.");
TRANSLATE_NOOP("FullscreenUI", "Sets which sort of memory card image will be used for slot {}.");
TRANSLATE_NOOP("FullscreenUI", "Setting {} binding {}.");
TRANSLATE_NOOP("FullscreenUI", "Settings");
TRANSLATE_NOOP("FullscreenUI", "Settings and Operations");
TRANSLATE_NOOP("FullscreenUI", "Settings reset to default.");
TRANSLATE_NOOP("FullscreenUI", "Shader {} added as stage {}.");
TRANSLATE_NOOP("FullscreenUI", "Shared Card Name");
TRANSLATE_NOOP("FullscreenUI", "Show Achievement Trophy Icons");
TRANSLATE_NOOP("FullscreenUI", "Show CPU Usage");
TRANSLATE_NOOP("FullscreenUI", "Show Controller Input");
TRANSLATE_NOOP("FullscreenUI", "Show Enhancement Settings");
TRANSLATE_NOOP("FullscreenUI", "Show FPS");
TRANSLATE_NOOP("FullscreenUI", "Show Frame Times");
TRANSLATE_NOOP("FullscreenUI", "Show GPU Statistics");
TRANSLATE_NOOP("FullscreenUI", "Show GPU Usage");
TRANSLATE_NOOP("FullscreenUI", "Show Grid View Titles");
TRANSLATE_NOOP("FullscreenUI", "Show Latency Statistics");
TRANSLATE_NOOP("FullscreenUI", "Show Localized Titles");
TRANSLATE_NOOP("FullscreenUI", "Show OSD Messages");
TRANSLATE_NOOP("FullscreenUI", "Show Resolution");
TRANSLATE_NOOP("FullscreenUI", "Show Speed");
TRANSLATE_NOOP("FullscreenUI", "Show Status Indicators");
TRANSLATE_NOOP("FullscreenUI", "Shows a background image or shader when a game isn't running. Backgrounds are located in resources/fullscreenui/backgrounds in the data directory.");
TRANSLATE_NOOP("FullscreenUI", "Shows a notification or icons in the lower-right corner of the screen when a challenge/primed achievement is active.");
TRANSLATE_NOOP("FullscreenUI", "Shows a popup in the lower-right corner of the screen when progress towards a measured achievement changes.");
TRANSLATE_NOOP("FullscreenUI", "Shows a timer in the bottom-right corner of the screen when leaderboard challenges are active.");
TRANSLATE_NOOP("FullscreenUI", "Shows a visual history of frame times in the upper-left corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows enhancement settings in the bottom-right corner of the screen.");
TRANSLATE_NOOP("FullscreenUI", "Shows information about input and audio latency in the top-right corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows information about the emulated GPU in the top-right corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows on-screen-display messages when events occur.");
TRANSLATE_NOOP("FullscreenUI", "Shows persistent icons when turbo is active or when paused.");
TRANSLATE_NOOP("FullscreenUI", "Shows the current controller state of the system in the bottom-left corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows the current emulation speed of the system in the top-right corner of the display as a percentage.");
TRANSLATE_NOOP("FullscreenUI", "Shows the current rendering resolution of the system in the top-right corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows the game you are currently playing as part of your profile in Discord.");
TRANSLATE_NOOP("FullscreenUI", "Shows the host's CPU usage of each system thread in the top-right corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows the host's GPU usage in the top-right corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows the number of frames (or v-syncs) displayed per second by the system in the top-right corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows titles underneath the images in the game grid view.");
TRANSLATE_NOOP("FullscreenUI", "Shows trophy icons in game grid when games have achievements or have been mastered.");
TRANSLATE_NOOP("FullscreenUI", "Simulates the region check present in original, unmodified consoles.");
TRANSLATE_NOOP("FullscreenUI", "Simulates the system ahead of time and rolls back/replays to reduce input lag. Very high system requirements.");
TRANSLATE_NOOP("FullscreenUI", "Size: ");
TRANSLATE_NOOP("FullscreenUI", "Skip Duplicate Frame Display");
TRANSLATE_NOOP("FullscreenUI", "Skips the presentation/display of frames that are not unique. Can result in worse frame pacing.");
TRANSLATE_NOOP("FullscreenUI", "Slow Boot");
TRANSLATE_NOOP("FullscreenUI", "Smooth Scrolling");
TRANSLATE_NOOP("FullscreenUI", "Smooths out blockyness between colour transitions in 24-bit content, usually FMVs.");
TRANSLATE_NOOP("FullscreenUI", "Smooths out the blockiness of magnified textures on 2D objects.");
TRANSLATE_NOOP("FullscreenUI", "Smooths out the blockiness of magnified textures on 3D objects.");
TRANSLATE_NOOP("FullscreenUI", "Sort Alphabetically");
TRANSLATE_NOOP("FullscreenUI", "Sort By");
TRANSLATE_NOOP("FullscreenUI", "Sort Reversed");
TRANSLATE_NOOP("FullscreenUI", "Sorts the cheat list alphabetically by the name of the code.");
TRANSLATE_NOOP("FullscreenUI", "Sound Effects");
TRANSLATE_NOOP("FullscreenUI", "Specifies the amount of buffer time added, which reduces the additional sleep time introduced.");
TRANSLATE_NOOP("FullscreenUI", "Spectator Mode");
TRANSLATE_NOOP("FullscreenUI", "Speed Control");
TRANSLATE_NOOP("FullscreenUI", "Speeds up CD-ROM reads by the specified factor. May improve loading speeds in some games, and break others.");
TRANSLATE_NOOP("FullscreenUI", "Speeds up CD-ROM seeks by the specified factor. May improve loading speeds in some games, and break others.");
TRANSLATE_NOOP("FullscreenUI", "Sprite Texture Filtering");
TRANSLATE_NOOP("FullscreenUI", "Stage {}: {}");
TRANSLATE_NOOP("FullscreenUI", "Start BIOS");
TRANSLATE_NOOP("FullscreenUI", "Start Disc");
TRANSLATE_NOOP("FullscreenUI", "Start Download");
TRANSLATE_NOOP("FullscreenUI", "Start File");
TRANSLATE_NOOP("FullscreenUI", "Start Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Start Game");
TRANSLATE_NOOP("FullscreenUI", "Start a game from a disc in your PC's DVD drive.");
TRANSLATE_NOOP("FullscreenUI", "Start the console without any disc inserted.");
TRANSLATE_NOOP("FullscreenUI", "Stores the current settings to a controller preset.");
TRANSLATE_NOOP("FullscreenUI", "Stretch Mode");
TRANSLATE_NOOP("FullscreenUI", "Summary");
TRANSLATE_NOOP("FullscreenUI", "Support for controllers that use the XInput protocol. XInput should only be used if you are using a XInput wrapper library.");
TRANSLATE_NOOP("FullscreenUI", "Switch to Next Disc on Stop");
TRANSLATE_NOOP("FullscreenUI", "Switches back to 4:3 display aspect ratio when displaying 24-bit content, usually FMVs.");
TRANSLATE_NOOP("FullscreenUI", "Switches between full screen and windowed when the window is double-clicked.");
TRANSLATE_NOOP("FullscreenUI", "Sync To Host Refresh Rate");
TRANSLATE_NOOP("FullscreenUI", "Synchronizes presentation of the console's frames to the host. GSync/FreeSync users should enable Optimal Frame Pacing instead.");
TRANSLATE_NOOP("FullscreenUI", "Temporarily disables all enhancements, useful when testing.");
TRANSLATE_NOOP("FullscreenUI", "Test Unofficial Achievements");
TRANSLATE_NOOP("FullscreenUI", "Texture Filtering");
TRANSLATE_NOOP("FullscreenUI", "Texture Replacements");
TRANSLATE_NOOP("FullscreenUI", "Textures Directory");
TRANSLATE_NOOP("FullscreenUI", "The SDL input source supports most controllers.");
TRANSLATE_NOOP("FullscreenUI", "The audio backend determines how frames produced by the emulator are submitted to the host.");
TRANSLATE_NOOP("FullscreenUI", "The selected memory card image will be used in shared mode for this slot.");
TRANSLATE_NOOP("FullscreenUI", "Theme");
TRANSLATE_NOOP("FullscreenUI", "This game has no achievements.");
TRANSLATE_NOOP("FullscreenUI", "This game has no leaderboards.");
TRANSLATE_NOOP("FullscreenUI", "Threaded Rendering");
TRANSLATE_NOOP("FullscreenUI", "Time Played");
TRANSLATE_NOOP("FullscreenUI", "Time Played: ");
TRANSLATE_NOOP("FullscreenUI", "Timing out in {:.0f} seconds...");
TRANSLATE_NOOP("FullscreenUI", "Title");
TRANSLATE_NOOP("FullscreenUI", "To use achievements, please log in with your retroachievements.org account.");
TRANSLATE_NOOP("FullscreenUI", "Toggle Analog");
TRANSLATE_NOOP("FullscreenUI", "Toggle Fast Forward");
TRANSLATE_NOOP("FullscreenUI", "Toggle Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Toggle every %d frames");
TRANSLATE_NOOP("FullscreenUI", "Toggles the macro when the button is pressed, instead of held.");
TRANSLATE_NOOP("FullscreenUI", "Top: ");
TRANSLATE_NOOP("FullscreenUI", "Tries to detect FMVs and disable read speedup during games that don't use XA streaming audio.");
TRANSLATE_NOOP("FullscreenUI", "Trigger");
TRANSLATE_NOOP("FullscreenUI", "Turbo Speed");
TRANSLATE_NOOP("FullscreenUI", "Type");
TRANSLATE_NOOP("FullscreenUI", "UI Language");
TRANSLATE_NOOP("FullscreenUI", "Uncompressed Size");
TRANSLATE_NOOP("FullscreenUI", "Undo Load State");
TRANSLATE_NOOP("FullscreenUI", "Ungrouped");
TRANSLATE_NOOP("FullscreenUI", "Unknown");
TRANSLATE_NOOP("FullscreenUI", "Unknown File Size");
TRANSLATE_NOOP("FullscreenUI", "Unlimited");
TRANSLATE_NOOP("FullscreenUI", "Update Progress");
TRANSLATE_NOOP("FullscreenUI", "Updates the progress database for achievements shown in the game list.");
TRANSLATE_NOOP("FullscreenUI", "Upscales the game's rendering by the specified multiplier.");
TRANSLATE_NOOP("FullscreenUI", "Use Blit Swap Chain");
TRANSLATE_NOOP("FullscreenUI", "Use Debug GPU Device");
TRANSLATE_NOOP("FullscreenUI", "Use DualShock/DualSense Button Icons");
TRANSLATE_NOOP("FullscreenUI", "Use Global Setting");
TRANSLATE_NOOP("FullscreenUI", "Use Old MDEC Routines");
TRANSLATE_NOOP("FullscreenUI", "Use Separate Disc Settings");
TRANSLATE_NOOP("FullscreenUI", "Use Single Card For Multi-Disc Games");
TRANSLATE_NOOP("FullscreenUI", "Use Software Renderer For Readbacks");
TRANSLATE_NOOP("FullscreenUI", "User Name");
TRANSLATE_NOOP("FullscreenUI", "Uses OpenGL ES even when desktop OpenGL is supported. May improve performance on some SBC drivers.");
TRANSLATE_NOOP("FullscreenUI", "Uses PGXP for all instructions, not just memory operations.");
TRANSLATE_NOOP("FullscreenUI", "Uses a blit presentation model instead of flipping. This may be needed on some systems.");
TRANSLATE_NOOP("FullscreenUI", "Uses a second thread for drawing graphics. Provides a significant speed improvement particularly with the software renderer, and is safe to use.");
TRANSLATE_NOOP("FullscreenUI", "Uses game-specific settings for controllers for this game.");
TRANSLATE_NOOP("FullscreenUI", "Uses localized (native language) titles in the game list.");
TRANSLATE_NOOP("FullscreenUI", "Uses native resolution coordinates for 2D polygons, instead of precise coordinates. Can fix misaligned UI in some games, but otherwise should be left disabled.");
TRANSLATE_NOOP("FullscreenUI", "Uses perspective-correct interpolation for colors, which can improve visuals in some games.");
TRANSLATE_NOOP("FullscreenUI", "Uses perspective-correct interpolation for texture coordinates, straightening out warped textures.");
TRANSLATE_NOOP("FullscreenUI", "Uses screen positions to resolve PGXP data. May improve visuals in some games.");
TRANSLATE_NOOP("FullscreenUI", "Uses separate game settings for each disc of multi-disc games. Can only be set on the first/main disc.");
TRANSLATE_NOOP("FullscreenUI", "Utilizes the chosen frame rate regardless of the game's setting.");
TRANSLATE_NOOP("FullscreenUI", "Value Range");
TRANSLATE_NOOP("FullscreenUI", "Value: {} | Default: {} | Minimum: {} | Maximum: {}");
TRANSLATE_NOOP("FullscreenUI", "Vertex Cache");
TRANSLATE_NOOP("FullscreenUI", "Vertical Sync (VSync)");
TRANSLATE_NOOP("FullscreenUI", "WARNING: Activating cheats can cause unpredictable behavior, crashing, soft-locks, or broken saved games.");
TRANSLATE_NOOP("FullscreenUI", "WARNING: Activating game patches can cause unpredictable behavior, crashing, soft-locks, or broken saved games.");
TRANSLATE_NOOP("FullscreenUI", "WARNING: Your game is still saving to the memory card. Continuing to {0} may IRREVERSIBLY DESTROY YOUR MEMORY CARD. We recommend resuming your game and waiting 5 seconds for it to finish saving.\n\nDo you want to {0} anyway?");
TRANSLATE_NOOP("FullscreenUI", "When Big Picture mode is started, the game list will be displayed instead of the main menu.");
TRANSLATE_NOOP("FullscreenUI", "When enabled and logged in, DuckStation will scan for achievements on startup.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, DuckStation will assume all achievements are locked and not send any unlock notifications to the server.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, DuckStation will list achievements from unofficial sets. These achievements are not tracked by RetroAchievements.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, each session will behave as if no achievements have been unlocked.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, memory cards and controllers will be overwritten when save states are loaded.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, the minimum supported output latency will be used for the host API.");
TRANSLATE_NOOP("FullscreenUI", "When playing a multi-disc game and using per-game (title) memory cards, use a single memory card for all discs.");
TRANSLATE_NOOP("FullscreenUI", "When this option is chosen, the clock speed set below will be used.");
TRANSLATE_NOOP("FullscreenUI", "Widescreen Rendering");
TRANSLATE_NOOP("FullscreenUI", "Window Animations");
TRANSLATE_NOOP("FullscreenUI", "Wireframe Rendering");
TRANSLATE_NOOP("FullscreenUI", "Writes backgrounds that can be replaced to the dump directory.");
TRANSLATE_NOOP("FullscreenUI", "Yes");
TRANSLATE_NOOP("FullscreenUI", "Yes, {} now and risk memory card corruption.");
TRANSLATE_NOOP("FullscreenUI", "\"Challenge\" mode for achievements, including leaderboard tracking. Disables save state, cheats, and slowdown functions.");
TRANSLATE_NOOP("FullscreenUI", "\"PlayStation\" and \"PSX\" are registered trademarks of Sony Interactive Entertainment Europe Limited. This software is not affiliated in any way with Sony Interactive Entertainment.");
TRANSLATE_NOOP("FullscreenUI", "change disc");
TRANSLATE_NOOP("FullscreenUI", "reset");
TRANSLATE_NOOP("FullscreenUI", "shut down");
TRANSLATE_NOOP("FullscreenUI", "{} Frames");
TRANSLATE_NOOP("FullscreenUI", "{} deleted.");
TRANSLATE_NOOP("FullscreenUI", "{} does not exist.");
TRANSLATE_NOOP("FullscreenUI", "{} is not a valid disc image.");
TRANSLATE_NOOP("FullscreenUI", "{} of {}");
// TRANSLATION-STRING-AREA-END
#endif
