// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "fullscreenui.h"
#include "achievements_private.h"
#include "controller.h"
#include "core.h"
#include "fullscreenui_private.h"
#include "fullscreenui_widgets.h"
#include "game_list.h"
#include "gpu_thread.h"
#include "host.h"
#include "system.h"

#include "scmversion/scmversion.h"

#include "util/cd_image.h"
#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/shadergen.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsPromptFont.h"

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
static void BeginChangeDiscOnCoreThread(bool needs_pause);
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
// Constants
//////////////////////////////////////////////////////////////////////////

static constexpr std::string_view RESUME_STATE_SELECTOR_DIALOG_NAME = "##resume_state_selector";
static constexpr std::string_view ABOUT_DIALOG_NAME = "##about_duckstation";

//////////////////////////////////////////////////////////////////////////
// State
//////////////////////////////////////////////////////////////////////////

namespace {

struct Locals
{
  // Main
  MainWindowType current_main_window = MainWindowType::None;
  PauseSubMenu current_pause_submenu = PauseSubMenu::None;
  MainWindowType previous_main_window = MainWindowType::None;
  bool initialized = false;
  bool background_loaded = false;
  bool pause_menu_was_open = false;
  bool was_paused_on_quick_menu_open = false;

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

ALIGN_TO_CACHE_LINE static Locals s_locals;

} // namespace FullscreenUI

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::Initialize()
{
  // some achievement callbacks fire early while e.g. there is a load state popup blocking system init
  if (s_locals.initialized || !ImGuiManager::IsInitialized())
    return;

  s_locals.initialized = true;

  // in case we open the pause menu while the game is running
  if (s_locals.current_main_window == MainWindowType::None && !GPUThread::HasGPUBackend() &&
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
  return s_locals.initialized;
}

bool FullscreenUI::HasActiveWindow()
{
  return s_locals.initialized && (s_locals.current_main_window != MainWindowType::None ||
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
      s_locals.current_main_window = MainWindowType::None;
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
    if (s_locals.current_main_window == MainWindowType::PauseMenu)
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

    s_locals.pause_menu_was_open = false;
    s_locals.was_paused_on_quick_menu_open = false;
    s_locals.current_pause_submenu = PauseSubMenu::None;
    ReturnToMainWindow(LONG_TRANSITION_TIME);
  });
}

void FullscreenUI::PauseForMenuOpen(bool set_pause_menu_open)
{
  s_locals.was_paused_on_quick_menu_open = GPUThread::IsSystemPaused();
  if (!s_locals.was_paused_on_quick_menu_open)
    Host::RunOnCoreThread([]() { System::PauseSystem(true); });

  s_locals.pause_menu_was_open |= set_pause_menu_open;
}

void FullscreenUI::OpenPauseMenu()
{
  if (!System::IsValid())
    return;

  GPUThread::RunOnThread([]() {
    Initialize();
    if (s_locals.current_main_window != MainWindowType::None)
      return;

    PauseForMenuOpen(true);
    ForceKeyNavEnabled();

    UpdateAchievementsRecentUnlockAndAlmostThere();
    BeginTransition(SHORT_TRANSITION_TIME, []() {
      s_locals.current_pause_submenu = PauseSubMenu::None;
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
    if (s_locals.current_main_window != MainWindowType::None)
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
  BeginChangeDiscOnCoreThread(true);
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

  if (GPUThread::IsSystemPaused() && !s_locals.was_paused_on_quick_menu_open)
    Host::RunOnCoreThread([]() { System::PauseSystem(false); });

  BeginTransition(SHORT_TRANSITION_TIME, []() {
    s_locals.current_pause_submenu = PauseSubMenu::None;
    s_locals.pause_menu_was_open = false;
    SwitchToMainWindow(MainWindowType::None);
  });
}

void FullscreenUI::ClosePauseMenuImmediately()
{
  if (!GPUThread::HasGPUBackend())
    return;

  CancelTransition();

  if (GPUThread::IsSystemPaused() && !s_locals.was_paused_on_quick_menu_open)
    Host::RunOnCoreThread([]() { System::PauseSystem(false); });

  s_locals.current_pause_submenu = PauseSubMenu::None;
  s_locals.pause_menu_was_open = false;
  SwitchToMainWindow(MainWindowType::None);

  // Present frame with menu closed. We have to defer this for a frame so imgui loses keyboard focus.
  if (GPUThread::IsSystemPaused())
    GPUThread::PresentCurrentFrame();
}

void FullscreenUI::SwitchToMainWindow(MainWindowType type)
{
  if (s_locals.current_main_window == type)
    return;

  s_locals.previous_main_window = (type == MainWindowType::None) ? MainWindowType::None : s_locals.current_main_window;
  s_locals.current_main_window = type;
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
  if (s_locals.previous_main_window == MainWindowType::None)
  {
    ReturnToMainWindow();
  }
  else
  {
    BeginTransition([window = s_locals.previous_main_window]() {
      SwitchToMainWindow(window);

      // return stack is only one deep
      s_locals.previous_main_window = window;
    });
  }
}

void FullscreenUI::ReturnToMainWindow()
{
  ReturnToMainWindow(GPUThread::HasGPUBackend() ? SHORT_TRANSITION_TIME : DEFAULT_TRANSITION_TIME);
}

void FullscreenUI::ReturnToMainWindow(float transition_time)
{
  if (GPUThread::IsSystemPaused() && !s_locals.was_paused_on_quick_menu_open)
    Host::RunOnCoreThread([]() { System::PauseSystem(false); });

  BeginTransition(transition_time, []() {
    s_locals.previous_main_window = MainWindowType::None;
    s_locals.current_pause_submenu = PauseSubMenu::None;
    s_locals.pause_menu_was_open = false;

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
    s_locals.current_main_window = MainWindowType::None;
    s_locals.current_pause_submenu = PauseSubMenu::None;
    s_locals.pause_menu_was_open = false;
    s_locals.was_paused_on_quick_menu_open = false;

    ClearAchievementsState();
    ClearSaveStateEntryList();
    ClearSettingsState();
    ClearGameListState();
    s_locals.current_time_string = {};
    s_locals.current_time = 0;
  }

  DestroyResources();

  s_locals.initialized = false;
  UpdateRunIdleState();
}

void FullscreenUI::Render()
{
  UploadAsyncTextures();

  if (!s_locals.initialized)
  {
    // achievement overlays still need to get drawn
    Achievements::DrawGameOverlays();
    return;
  }

  // draw background before any overlays
  if (!GPUThread::HasGPUBackend() && s_locals.current_main_window != MainWindowType::None)
    DrawBackground();

  BeginLayout();

  // Primed achievements must come first, because we don't want the pause screen to be behind them.
  if (s_locals.current_main_window == MainWindowType::None)
    Achievements::DrawGameOverlays();

  switch (s_locals.current_main_window)
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
      DrawAchievementsWindow();
      break;
    case MainWindowType::Leaderboards:
      DrawLeaderboardsWindow();
      break;
    default:
      break;
  }

  if (IsFixedPopupDialogOpen(ABOUT_DIALOG_NAME))
    DrawAboutWindow();
  else if (IsFixedPopupDialogOpen(RESUME_STATE_SELECTOR_DIALOG_NAME))
    DrawResumeStateSelector();

  EndLayout();

  ResetCloseMenuIfNeeded();
  UpdateTransitionState();
}

void FullscreenUI::DestroyResources()
{
  s_locals.app_background_texture.reset();
  s_locals.app_background_shader.reset();
  s_locals.background_loaded = false;
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

  if (!svg_name.empty())
    return GetCachedTexture(svg_name, svg_width, svg_height);
  else
    return GetCachedTexture(png_name);
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
  if (s_locals.current_time == current_time)
    return;

  s_locals.current_time = current_time;
  s_locals.current_time_string = {};
  s_locals.current_time_string = Host::FormatNumber(Host::NumberFormatType::ShortTime, static_cast<s64>(current_time));
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
  Host::RunOnCoreThread([params = std::move(params)]() mutable {
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
    ShowToast(OSDMessageType::Info, {}, FSUI_STR("No resume save state found."));
    return;
  }

  SaveStateListEntry slentry;
  if (!InitializeSaveStateListEntryFromPath(&slentry, std::move(path), -1, false))
    return;

  ClearSaveStateEntryList();
  s_locals.save_state_selector_slots.push_back(std::move(slentry));
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
    ShowToast(OSDMessageType::Info, {},
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
  Host::RunOnCoreThread([action = std::move(action), callback = std::move(callback)]() mutable {
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
      Host::RunOnCoreThread([save_state]() { Host::RequestSystemShutdown(false, save_state, false); });
    else
      ClosePauseMenuImmediately();
  });
}

void FullscreenUI::RequestReset()
{
  SwitchToMainWindow(MainWindowType::None);

  ConfirmIfSavingMemoryCards(FSUI_STR("reset"), [](bool result) {
    if (result)
      Host::RunOnCoreThread(System::ResetSystem);

    BeginTransition(LONG_TRANSITION_TIME, &ClosePauseMenuImmediately);
  });
}

void FullscreenUI::DoToggleFastForward()
{
  Host::RunOnCoreThread([]() {
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
          ShowToast(OSDMessageType::Error, {},
                    fmt::format(FSUI_FSTR("{} is not a valid disc image."), FileSystem::GetDisplayNameFromPath(path)));
        }
        else
        {
          Host::RunOnCoreThread([path]() { System::InsertMedia(path.c_str()); });
        }
      }

      ReturnToMainWindow();
    });
  };

  OpenFileSelector(FSUI_ICONVSTR(ICON_FA_COMPACT_DISC, "Select Disc Image"), false, std::move(callback),
                   GetDiscImageFilters(), std::string(Path::GetDirectory(GPUThread::GetGamePath())));
}

void FullscreenUI::BeginChangeDiscOnCoreThread(bool needs_pause)
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
                Host::RunOnCoreThread([path = std::move(paths[index - 1])]() { System::InsertMedia(path.c_str()); });

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
  Host::RunOnCoreThread([]() {
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
  Host::RunOnCoreThread([]() { Host::RequestExitApplication(true); });
}

void FullscreenUI::DoDesktopMode()
{
  Host::RunOnCoreThread([]() { Host::RequestExitBigPicture(); });
}

void FullscreenUI::DoToggleFullscreen()
{
  Host::RunOnCoreThread([]() { GPUThread::SetFullscreen(!GPUThread::IsFullscreen()); });
}

//////////////////////////////////////////////////////////////////////////
// Landing Window
//////////////////////////////////////////////////////////////////////////

bool FullscreenUI::HasBackground()
{
  return static_cast<bool>(s_locals.app_background_texture || s_locals.app_background_shader);
}

void FullscreenUI::UpdateBackground()
{
  if (!IsInitialized())
    return;

  g_gpu_device->RecycleTexture(std::move(s_locals.app_background_texture));
  s_locals.app_background_shader.reset();
  s_locals.background_loaded = true;

  const TinyString background_name =
    Core::GetBaseTinyStringSettingValue("Main", "FullscreenUIBackground", DEFAULT_BACKGROUND_NAME);
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
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
  plconfig.SetTargetFormats(g_gpu_device->HasMainSwapChain() ? g_gpu_device->GetMainSwapChain()->GetFormat() :
                                                               GPUTexture::Format::RGBA8);
  plconfig.vertex_shader = vs.get();
  plconfig.fragment_shader = fs.get();
  s_locals.app_background_shader = g_gpu_device->CreatePipeline(plconfig, error);
  if (!s_locals.app_background_shader)
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
    static_cast<float>(Timer::ConvertValueToSeconds(Timer::GetCurrentValue() - s_locals.app_background_load_time));

  g_gpu_device->SetPipeline(s_locals.app_background_shader.get());
  g_gpu_device->DrawWithPushConstants(3, 0, &uniforms, sizeof(uniforms));
}

bool FullscreenUI::LoadBackgroundImage(const std::string& path, Error* error)
{
  Image image;
  if (!image.LoadFromFile(path.c_str(), error))
    return false;

  s_locals.app_background_texture = g_gpu_device->FetchAndUploadTextureImage(image, GPUTexture::Flags::None, error);
  if (!s_locals.app_background_texture)
    return false;

  return true;
}

void FullscreenUI::DrawBackground()
{
  if (!s_locals.background_loaded)
    UpdateBackground();

  if (s_locals.app_background_shader)
  {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddCallback(&FullscreenUI::DrawShaderBackgroundCallback, nullptr);
  }
  else if (s_locals.app_background_texture)
  {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const ImRect uv_rect = FitImage(size, ImVec2(static_cast<float>(s_locals.app_background_texture->GetWidth()),
                                                 static_cast<float>(s_locals.app_background_texture->GetHeight())));
    dl->AddImage(s_locals.app_background_texture.get(), ImVec2(0.0f, 0.0f), size, uv_rect.Min, uv_rect.Max);
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
  return Core::GetBaseBoolSettingValue("Main", "FullscreenUIOpenToGameList", false);
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
      GPUTexture* const logo_texture = GetUserThemeableTexture("images/duck.png", {}, nullptr, logo_size);
      dl->AddImage(logo_texture, logo_pos, logo_pos + logo_size);

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
                                                           IMSTR_START_END(s_locals.current_time_string));
      time_pos = ImVec2(heading_size.x - LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING) - time_size.x,
                        LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING));
      RenderShadowedTextClipped(heading_font, heading_font_size, heading_font_weight, time_pos, time_pos + time_size,
                                text_color, s_locals.current_time_string, &time_size);
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
          "fullscreenui/start-disc.png", "fullscreenui/start-disc.svg", FSUI_VSTR("Start Game"),
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
    if (s_locals.current_main_window == MainWindowType::Settings &&
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
    s_locals.current_pause_submenu = submenu;
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
                                  -1.0f, IMSTR_START_END(s_locals.current_time_string));
    text_pos = ImVec2(display_size.x - scaled_top_bar_padding - text_size.x, scaled_top_bar_padding);
    RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, text_pos, display_size,
                              title_text_color, s_locals.current_time_string);
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

  DrawAchievementsPauseMenuOverlays(scaled_top_bar_height);

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
    BeginMenuButtons(submenu_item_count[static_cast<u32>(s_locals.current_pause_submenu)], 1.0f,
                     LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING, 0.0f, 4.0f);

    switch (s_locals.current_pause_submenu)
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

        if (MenuButtonWithoutSummary(
              FSUI_ICONVSTR(ICON_FA_WRENCH, "Game Properties"),
              has_game && GameList::CanEditGameSettingsForPath(GPUThread::GetGameSerial(), GPUThread::GetGamePath())))
        {
          BeginTransition([]() { SwitchToGameSettings(); });
        }

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
          Host::RunOnCoreThread([]() { System::SaveScreenshot(); });
          ClosePauseMenu();
        }

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_COMPACT_DISC, "Change Disc")))
        {
          BeginTransition(SHORT_TRANSITION_TIME, []() { s_locals.current_main_window = MainWindowType::None; });
          Host::RunOnCoreThread([]() { BeginChangeDiscOnCoreThread(false); });
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

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_ARROWS_ROTATE, "Reset Game")))
          RequestReset();

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_FLOPPY_DISK, "Close and Save State")))
          BeginTransition(LONG_TRANSITION_TIME, []() { RequestShutdown(true); });

        if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_POWER_OFF, "Close Without Saving")))
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
  for (SaveStateListEntry& entry : s_locals.save_state_selector_slots)
  {
    if (entry.preview_texture)
      g_gpu_device->RecycleTexture(std::move(entry.preview_texture));
  }
  s_locals.save_state_selector_slots.clear();
}

u32 FullscreenUI::PopulateSaveStateListEntries(const std::string& serial,
                                               std::optional<ExtendedSaveStateInfo> undo_save_state, bool is_loading)
{
  ClearSaveStateEntryList();

  if (undo_save_state.has_value())
  {
    SaveStateListEntry li;
    li.title = FSUI_STR("Undo Load State");
    li.summary = fmt::format(FSUI_FSTR("Saved {}"), Host::FormatNumber(Host::NumberFormatType::ShortDateTime,
                                                                       static_cast<s64>(undo_save_state->timestamp)));
    if (undo_save_state->screenshot.IsValid())
      li.preview_texture = g_gpu_device->FetchAndUploadTextureImage(undo_save_state->screenshot);
    s_locals.save_state_selector_slots.push_back(std::move(li));
  }

  if (!serial.empty())
  {
    for (s32 i = 1; i <= System::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      SaveStateListEntry li;
      if (InitializeSaveStateListEntryFromSerial(&li, serial, i, false) || !is_loading)
        s_locals.save_state_selector_slots.push_back(std::move(li));
    }
  }

  for (s32 i = 1; i <= System::GLOBAL_SAVE_STATE_SLOTS; i++)
  {
    SaveStateListEntry li;
    if (InitializeSaveStateListEntryFromSerial(&li, serial, i, true) || !is_loading)
      s_locals.save_state_selector_slots.push_back(std::move(li));
  }

  return static_cast<u32>(s_locals.save_state_selector_slots.size());
}

void FullscreenUI::OpenSaveStateSelector(const std::string& serial, const std::string& path, bool is_loading)
{
  if (GPUThread::HasGPUBackend())
  {
    // need to get the undo state, if any
    Host::RunOnCoreThread([serial = serial, is_loading]() {
      std::optional<ExtendedSaveStateInfo> undo_state;
      if (is_loading)
        undo_state = System::GetUndoSaveStateInfo();
      GPUThread::RunOnThread([serial = std::move(serial), undo_state = std::move(undo_state), is_loading]() mutable {
        if (PopulateSaveStateListEntries(serial, std::move(undo_state), is_loading) > 0)
        {
          s_locals.save_state_selector_loading = is_loading;
          SwitchToMainWindow(MainWindowType::SaveStateSelector);
        }
        else
        {
          ShowToast(OSDMessageType::Info, {}, FSUI_STR("No save states found."));
        }
      });
    });
  }
  else
  {
    if (PopulateSaveStateListEntries(serial, std::nullopt, is_loading) > 0)
    {
      s_locals.save_state_selector_loading = is_loading;
      SwitchToMainWindow(MainWindowType::SaveStateSelector);
    }
    else
    {
      ShowToast(OSDMessageType::Info, {}, FSUI_STR("No save states found."));
    }
  }
}

void FullscreenUI::DrawSaveStateSelector()
{
  static constexpr auto do_load_state = [](const SaveStateListEntry& entry) {
    if (GPUThread::HasGPUBackend())
    {
      const s32 slot = entry.slot;
      const bool global = entry.global;
      const bool is_undo = entry.state_path.empty();
      ClearSaveStateEntryList(); // entry no longer valid
      ReturnToMainWindow(LONG_TRANSITION_TIME);

      // Loading undo state?
      if (is_undo)
        Host::RunOnCoreThread(&System::UndoLoadState);
      else
        Host::RunOnCoreThread([global, slot]() { System::LoadStateFromSlot(global, slot); });
    }
    else
    {
      DoStartPath(entry.game_path, entry.state_path);
    }
  };

  static constexpr auto do_save_state = [](const SaveStateListEntry& entry) {
    const s32 slot = entry.slot;
    const bool global = entry.global;
    ClearSaveStateEntryList(); // entry no longer valid
    ReturnToMainWindow(LONG_TRANSITION_TIME);

    Host::RunOnCoreThread([slot, global]() { System::SaveStateToSlot(global, slot); });
  };

  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, UIStyle.LargeFontSize + (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));
  SaveStateListEntry* pressed_entry = nullptr;
  bool closed = false;

  // last state deleted?
  if (s_locals.save_state_selector_slots.empty())
    closed = true;

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "##save_state_selector_title",
                            ModAlpha(UIStyle.PrimaryColor, GetBackgroundAlpha())))
  {
    BeginNavBar();
    if (NavButton(ICON_PF_NAVIGATION_BACK, true, true))
      closed = true;

    NavTitle(s_locals.save_state_selector_loading ? FSUI_VSTR("Load State") : FSUI_VSTR("Save State"));
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
      SaveStateListEntry& entry = s_locals.save_state_selector_slots[i];

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
          if (s_locals.save_state_selector_loading)
            title = FSUI_ICONVSTR(ICON_FA_FOLDER_OPEN, "Load State");
          else
            title = FSUI_ICONVSTR(ICON_FA_FOLDER_OPEN, "Save State");

          ChoiceDialogOptions options;
          options.reserve(3);
          options.emplace_back(title, false);
          if (!entry.state_path.empty())
            options.emplace_back(FSUI_ICONVSTR(ICON_FA_FOLDER_MINUS, "Delete Save"), false);
          options.emplace_back(FSUI_ICONVSTR(ICON_FA_SQUARE_XMARK, "Close Menu"), false);

          OpenChoiceDialog(
            std::move(title), false, std::move(options),
            [i](s32 index, const std::string& title, bool checked) mutable {
              if (index < 0 || i >= s_locals.save_state_selector_slots.size())
                return;

              SaveStateListEntry& entry = s_locals.save_state_selector_slots[i];
              if (index == 0)
              {
                // load state
                if (s_locals.save_state_selector_loading)
                  do_load_state(entry);
                else
                  do_save_state(entry);
              }
              else if (!entry.state_path.empty() && index == 1)
              {
                // delete state
                if (!FileSystem::FileExists(entry.state_path.c_str()))
                {
                  ShowToast(OSDMessageType::Error, {},
                            fmt::format(FSUI_FSTR("{} does not exist."), RemoveHash(entry.title)));
                }
                else if (FileSystem::DeleteFile(entry.state_path.c_str()))
                {
                  ShowToast(OSDMessageType::Quick, {}, fmt::format(FSUI_FSTR("{} deleted."), RemoveHash(entry.title)));

                  // need to preserve the texture, since it's going to be drawn this frame
                  // TODO: do this with a transition for safety
                  g_gpu_device->RecycleTexture(std::move(entry.preview_texture));

                  if (s_locals.save_state_selector_loading)
                    s_locals.save_state_selector_slots.erase(s_locals.save_state_selector_slots.begin() + i);
                  else
                    InitializePlaceholderSaveStateListEntry(&entry, entry.slot, entry.global);
                }
                else
                {
                  ShowToast(OSDMessageType::Quick, {},
                            fmt::format(FSUI_FSTR("Failed to delete {}."), RemoveHash(entry.title)));
                }
              }
            });
        }
      }

      // avoid triggering imgui warning
      i++;
      if (i == s_locals.save_state_selector_slots.size())
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
                 std::make_pair(ICON_PF_BUTTON_A, s_locals.save_state_selector_loading ? FSUI_VSTR("Load State") :
                                                                                         FSUI_VSTR("Save State")),
                 std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Cancel"))});
  }
  else
  {
    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT,
                                FSUI_VSTR("Select State")),
                 std::make_pair(ICON_PF_F1, FSUI_VSTR("Delete State")),
                 std::make_pair(ICON_PF_ENTER, s_locals.save_state_selector_loading ? FSUI_VSTR("Load State") :
                                                                                      FSUI_VSTR("Save State")),
                 std::make_pair(ICON_PF_ESC, FSUI_VSTR("Cancel"))});
  }

  if (pressed_entry)
  {
    if (s_locals.save_state_selector_loading)
      do_load_state(*pressed_entry);
    else
      do_save_state(*pressed_entry);
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
  s_locals.save_state_selector_slots.push_back(std::move(slentry));
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

  SaveStateListEntry& entry = s_locals.save_state_selector_slots.front();

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
    Error error;
    if (FileSystem::DeleteFile(entry.state_path.c_str(), &error))
    {
      std::string game_path = std::move(entry.game_path);
      ClearSaveStateEntryList();
      CloseFixedPopupDialogImmediately();
      DoStartPath(std::move(game_path));
    }
    else
    {
      ShowToast(OSDMessageType::Error, FSUI_STR("Failed to delete save state."), error.TakeDescription());
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
  Host::RunOnCoreThread([url = std::string(url)]() {
    GPUThread::SetFullscreen(false);
    Host::OpenURL(url);
  });
}

void FullscreenUI::CopyTextToClipboard(std::string title, std::string_view text)
{
  if (Host::CopyTextToClipboard(text))
    ShowToast(OSDMessageType::Quick, {}, std::move(title));
  else
    ShowToast(OSDMessageType::Error, {}, FSUI_STR("Failed to copy text to clipboard."));
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
  GPUTexture* const logo_texture = GetUserThemeableTexture("images/duck.png", "images/duck.svg", nullptr, image_size);
  ImGui::GetWindowDrawList()->AddImage(logo_texture, ImGui::GetCursorScreenPos(),
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

#endif // __ANDROID__
