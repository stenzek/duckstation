#define IMGUI_DEFINE_MATH_OPERATORS

#include "fullscreen_ui.h"
#include "IconsFontAwesome5.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/make_array.h"
#include "common/path.h"
#include "common/string.h"
#include "common/string_util.h"
#include "common_host.h"
#include "core/cheats.h"
#include "core/controller.h"
#include "core/cpu_core.h"
#include "core/gpu.h"
#include "core/host.h"
#include "core/host_display.h"
#include "core/host_settings.h"
#include "core/resources.h"
#include "core/settings.h"
#include "core/system.h"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "game_list.h"
#include "icon.h"
#include "imgui.h"
#include "imgui_fullscreen.h"
#include "imgui_internal.h"
#include "imgui_manager.h"
#include "imgui_stdlib.h"
#include "input_manager.h"
#include "scmversion/scmversion.h"
#include "util/ini_settings_interface.h"
#include <atomic>
#include <bitset>
#include <thread>
Log_SetChannel(FullscreenUI);

#ifdef WITH_CHEEVOS
#include "achievements.h"
#endif
static constexpr float LAYOUT_MAIN_MENU_BAR_SIZE = 20.0f; // Should be DPI scaled, not layout scaled!
static constexpr s32 MAX_SAVE_STATE_SLOTS = 10;

using ImGuiFullscreen::g_large_font;
using ImGuiFullscreen::g_layout_padding_left;
using ImGuiFullscreen::g_layout_padding_top;
using ImGuiFullscreen::g_medium_font;
using ImGuiFullscreen::LAYOUT_LARGE_FONT_SIZE;
using ImGuiFullscreen::LAYOUT_MEDIUM_FONT_SIZE;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING;
using ImGuiFullscreen::LAYOUT_SCREEN_HEIGHT;
using ImGuiFullscreen::LAYOUT_SCREEN_WIDTH;
using ImGuiFullscreen::UIBackgroundColor;
using ImGuiFullscreen::UIBackgroundHighlightColor;
using ImGuiFullscreen::UIBackgroundLineColor;
using ImGuiFullscreen::UIBackgroundTextColor;
using ImGuiFullscreen::UIDisabledColor;
using ImGuiFullscreen::UIPrimaryColor;
using ImGuiFullscreen::UIPrimaryDarkColor;
using ImGuiFullscreen::UIPrimaryLightColor;
using ImGuiFullscreen::UIPrimaryLineColor;
using ImGuiFullscreen::UIPrimaryTextColor;
using ImGuiFullscreen::UISecondaryColor;
using ImGuiFullscreen::UISecondaryDarkColor;
using ImGuiFullscreen::UISecondaryLightColor;
using ImGuiFullscreen::UISecondaryTextColor;
using ImGuiFullscreen::UITextHighlightColor;

using ImGuiFullscreen::ActiveButton;
using ImGuiFullscreen::AddNotification;
using ImGuiFullscreen::BeginFullscreenColumns;
using ImGuiFullscreen::BeginFullscreenColumnWindow;
using ImGuiFullscreen::BeginFullscreenWindow;
using ImGuiFullscreen::BeginMenuButtons;
using ImGuiFullscreen::BeginNavBar;
using ImGuiFullscreen::CenterImage;
using ImGuiFullscreen::CloseChoiceDialog;
using ImGuiFullscreen::CloseFileSelector;
using ImGuiFullscreen::DPIScale;
using ImGuiFullscreen::EndFullscreenColumns;
using ImGuiFullscreen::EndFullscreenColumnWindow;
using ImGuiFullscreen::EndFullscreenWindow;
using ImGuiFullscreen::EndMenuButtons;
using ImGuiFullscreen::EndNavBar;
using ImGuiFullscreen::EnumChoiceButton;
using ImGuiFullscreen::FloatingButton;
using ImGuiFullscreen::GetCachedTexture;
using ImGuiFullscreen::GetCachedTextureAsync;
using ImGuiFullscreen::GetPlaceholderTexture;
using ImGuiFullscreen::LayoutScale;
using ImGuiFullscreen::LoadTexture;
using ImGuiFullscreen::MenuButton;
using ImGuiFullscreen::MenuButtonFrame;
using ImGuiFullscreen::MenuButtonWithValue;
using ImGuiFullscreen::MenuHeading;
using ImGuiFullscreen::MenuHeadingButton;
using ImGuiFullscreen::MenuImageButton;
using ImGuiFullscreen::NavButton;
using ImGuiFullscreen::NavTitle;
using ImGuiFullscreen::OpenChoiceDialog;
using ImGuiFullscreen::OpenFileSelector;
using ImGuiFullscreen::PopPrimaryColor;
using ImGuiFullscreen::PushPrimaryColor;
using ImGuiFullscreen::QueueResetFocus;
using ImGuiFullscreen::RangeButton;
using ImGuiFullscreen::ResetFocusHere;
using ImGuiFullscreen::RightAlignNavButtons;
using ImGuiFullscreen::ShowToast;
using ImGuiFullscreen::ThreeWayToggleButton;
using ImGuiFullscreen::ToggleButton;
using ImGuiFullscreen::WantsToCloseMenu;

namespace FullscreenUI {
enum class MainWindowType
{
  None,
  Landing,
  GameList,
  Settings,
  PauseMenu,
#ifdef WITH_CHEEVOS
  Achievements,
  Leaderboards,
#endif

};

enum class PauseSubMenu
{
  None,
  Exit,
#ifdef WITH_CHEEVOS
  Achievements,
#endif
};

enum class SettingsPage
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
  Display,
  Enhancements,
  Audio,
  Achievements,
  Advanced,
  Count
};

//////////////////////////////////////////////////////////////////////////
// Utility
//////////////////////////////////////////////////////////////////////////
static std::string TimeToPrintableString(time_t t);

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////
static void UpdateForcedVsync(bool should_force);
static void PauseForMenuOpen();
static void ClosePauseMenu();
static void OpenPauseSubMenu(PauseSubMenu submenu);
static void ReturnToMainWindow();
static void DrawLandingWindow();
static void DrawPauseMenu(MainWindowType type);
static void DrawAboutWindow();
static void OpenAboutWindow();

static MainWindowType s_current_main_window = MainWindowType::None;
static PauseSubMenu s_current_pause_submenu = PauseSubMenu::None;
static std::string s_current_game_subtitle;
static bool s_initialized = false;
static bool s_tried_to_initialize = false;
static bool s_pause_menu_was_open = false;
static bool s_was_paused_on_quick_menu_open = false;
static bool s_about_window_open = false;

//////////////////////////////////////////////////////////////////////////
// Resources
//////////////////////////////////////////////////////////////////////////
static bool LoadResources();
static void DestroyResources();

static std::shared_ptr<HostDisplayTexture> s_app_icon_texture;
static std::array<std::shared_ptr<HostDisplayTexture>, static_cast<u32>(GameDatabase::CompatibilityRating::Count)>
  s_game_compatibility_textures;
static std::shared_ptr<HostDisplayTexture> s_fallback_disc_texture;
static std::shared_ptr<HostDisplayTexture> s_fallback_exe_texture;
static std::shared_ptr<HostDisplayTexture> s_fallback_psf_texture;
static std::shared_ptr<HostDisplayTexture> s_fallback_playlist_texture;
static std::vector<std::unique_ptr<HostDisplayTexture>> s_cleanup_textures;

//////////////////////////////////////////////////////////////////////////
// Landing
//////////////////////////////////////////////////////////////////////////
static void SwitchToLanding();
static ImGuiFullscreen::FileSelectorFilters GetDiscImageFilters();
static void DoStartPath(std::string path, std::string state = std::string(),
                        std::optional<bool> fast_boot = std::nullopt);
static void DoResume();
static void DoStartFile();
static void DoStartBIOS();
static void DoToggleFastForward();
static void DoToggleSoftwareRenderer();
static void DoShutdown(bool save_state);
static void DoReset();
static void DoChangeDiscFromFile();
static void DoChangeDisc();
static void DoRequestExit();
static void DoToggleFullscreen();

//////////////////////////////////////////////////////////////////////////
// Settings
//////////////////////////////////////////////////////////////////////////

static constexpr double INPUT_BINDING_TIMEOUT_SECONDS = 5.0;
static constexpr u32 NUM_MEMORY_CARD_PORTS = 2;

static void SwitchToSettings();
static void SwitchToGameSettings();
static void SwitchToGameSettings(const GameList::Entry* entry);
static void SwitchToGameSettingsForPath(const std::string& path);
static void SwitchToGameSettingsForSerial(const std::string_view& serial);
static void DrawSettingsWindow();
static void DrawSummarySettingsPage();
static void DrawInterfaceSettingsPage();
static void DrawGameListSettingsPage();
static void DrawBIOSSettingsPage();
static void DrawConsoleSettingsPage();
static void DrawEmulationSettingsPage();
static void DrawDisplaySettingsPage();
static void DrawEnhancementSettingsPage();
static void DrawAudioSettingsPage();
static void DrawMemoryCardSettingsPage();
static void DrawControllerSettingsPage();
static void DrawHotkeySettingsPage();
static void DrawAchievementsSettingsPage();
static void DrawAdvancedSettingsPage();

static bool IsEditingGameSettings();
static SettingsInterface* GetEditingSettingsInterface();
static void SetSettingsChanged();
static void DoCopyGameSettings();
static void DoClearGameSettings();

static bool DrawToggleSetting(const char* title, const char* summary, const char* section, const char* key,
                              bool default_value, bool enabled = true,
                              float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = g_large_font,
                              ImFont* summary_font = g_medium_font);
static void DrawIntListSetting(const char* title, const char* summary, const char* section, const char* key,
                               int default_value, const char* const* options, size_t option_count,
                               int option_offset = 0, bool enabled = true,
                               float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = g_large_font,
                               ImFont* summary_font = g_medium_font);
static void DrawIntRangeSetting(const char* title, const char* summary, const char* section, const char* key,
                                int default_value, int min_value, int max_value, const char* format = "%d",
                                bool enabled = true, float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
static void DrawFloatRangeSetting(const char* title, const char* summary, const char* section, const char* key,
                                  float default_value, float min_value, float max_value, const char* format = "%f",
                                  bool enabled = true, float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                  ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
static void DrawIntRectSetting(const char* title, const char* summary, const char* section, const char* left_key,
                               int default_left, const char* top_key, int default_top, const char* right_key,
                               int default_right, const char* bottom_key, int default_bottom, int min_value,
                               int max_value, const char* format = "%d", bool enabled = true,
                               float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = g_large_font,
                               ImFont* summary_font = g_medium_font);
static void DrawStringListSetting(const char* title, const char* summary, const char* section, const char* key,
                                  const char* default_value, const char* const* options,
                                  const char* const* option_values, size_t option_count, bool enabled = true,
                                  float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                  ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
template<typename DataType, typename SizeType>
static void DrawEnumSetting(const char* title, const char* summary, const char* section, const char* key,
                            DataType default_value, std::optional<DataType> (*from_string_function)(const char* str),
                            const char* (*to_string_function)(DataType value),
                            const char* (*to_display_string_function)(DataType value), SizeType option_count,
                            bool enabled = true, float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                            ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
static void DrawFloatListSetting(const char* title, const char* summary, const char* section, const char* key,
                                 float default_value, const char* const* options, const float* option_values,
                                 size_t option_count, bool enabled = true,
                                 float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = g_large_font,
                                 ImFont* summary_font = g_medium_font);
static void DrawFolderSetting(const char* title, const char* section, const char* key, const std::string& runtime_var,
                              float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = g_large_font,
                              ImFont* summary_font = g_medium_font);

static void PopulateGraphicsAdapterList();
static void PopulateGameListDirectoryCache(SettingsInterface* si);
static ImGuiFullscreen::ChoiceDialogOptions GetGameListDirectoryOptions(bool recursive_as_checked);
static void BeginInputBinding(Controller::ControllerBindingType type, const std::string_view& section,
                              const std::string_view& key, const std::string_view& display_name);
static void DrawInputBindingWindow();
static void DrawInputBindingButton(Controller::ControllerBindingType type, const char* section, const char* name,
                                   const char* display_name, bool show_type = true);
static void ClearInputBindingVariables();
static void StartAutomaticBinding(u32 port);

static SettingsPage s_settings_page = SettingsPage::Interface;
static std::unique_ptr<INISettingsInterface> s_game_settings_interface;
static std::unique_ptr<GameList::Entry> s_game_settings_entry;
static std::vector<std::pair<std::string, bool>> s_game_list_directories_cache;
static std::vector<std::string> s_graphics_adapter_list_cache;
static std::vector<std::string> s_fullscreen_mode_list_cache;
static std::vector<const HotkeyInfo*> s_hotkey_list_cache;
static std::atomic_bool s_settings_changed{false};
static Controller::ControllerBindingType s_input_binding_type = Controller::ControllerBindingType::Unknown;
static std::string s_input_binding_section;
static std::string s_input_binding_key;
static std::string s_input_binding_display_name;
static std::vector<InputBindingKey> s_input_binding_new_bindings;
static Common::Timer s_input_binding_timer;

//////////////////////////////////////////////////////////////////////////
// Save State List
//////////////////////////////////////////////////////////////////////////
struct SaveStateListEntry
{
  std::string title;
  std::string summary;
  std::string path;
  std::unique_ptr<HostDisplayTexture> preview_texture;
  s32 slot;
  bool global;
};

static void InitializePlaceholderSaveStateListEntry(SaveStateListEntry* li, const std::string& title,
                                                    const std::string& serial, s32 slot, bool global);
static bool InitializeSaveStateListEntry(SaveStateListEntry* li, const std::string& title, const std::string& serial,
                                         s32 slot, bool global);
static void PopulateSaveStateScreenshot(SaveStateListEntry* li, const ExtendedSaveStateInfo* ssi);
static void ClearSaveStateEntryList();
static u32 PopulateSaveStateListEntries(const std::string& title, const std::string& serial);
static bool OpenLoadStateSelectorForGame(const std::string& game_path);
static bool OpenSaveStateSelector(bool is_loading);
static void CloseSaveStateSelector();
static void DrawSaveStateSelector(bool is_loading, bool fullscreen);
static void DoLoadState(std::string path);
static void DoSaveState(s32 slot, bool global);

static std::vector<SaveStateListEntry> s_save_state_selector_slots;
static std::string s_save_state_selector_game_path;
static bool s_save_state_selector_open = false;
static bool s_save_state_selector_loading = true;

//////////////////////////////////////////////////////////////////////////
// Game List
//////////////////////////////////////////////////////////////////////////
static void DrawGameListWindow();
static void SwitchToGameList();
static void PopulateGameListEntryList();
static HostDisplayTexture* GetTextureForGameListEntryType(GameList::EntryType type);
static HostDisplayTexture* GetGameListCover(const GameList::Entry* entry);
static HostDisplayTexture* GetCoverForCurrentGame();
static std::string GetNotificationImageForGame(const GameList::Entry* entry);
static std::string GetNotificationImageForGame(const std::string& game_path);

// Lazily populated cover images.
static std::unordered_map<std::string, std::string> s_cover_image_map;
static std::vector<const GameList::Entry*> s_game_list_sorted_entries;

#ifdef WITH_CHEEVOS
//////////////////////////////////////////////////////////////////////////
// Achievements
//////////////////////////////////////////////////////////////////////////
static void DrawAchievementsWindow();
static void DrawAchievement(const Achievements::Achievement& cheevo);
static void DrawLeaderboardsWindow();
static void DrawLeaderboardListEntry(const Achievements::Leaderboard& lboard);
static void DrawLeaderboardEntry(const Achievements::LeaderboardEntry& lbEntry, float rank_column_width,
                                 float name_column_width, float column_spacing);

static std::optional<u32> s_open_leaderboard_id;
#endif
} // namespace FullscreenUI

//////////////////////////////////////////////////////////////////////////
// Utility
//////////////////////////////////////////////////////////////////////////

std::string FullscreenUI::TimeToPrintableString(time_t t)
{
  struct tm lt = {};
#ifdef _MSC_VER
  localtime_s(&lt, &t);
#else
  localtime_r(&t, &lt);
#endif

  char buf[256];
  std::strftime(buf, sizeof(buf), "%c", &lt);
  return std::string(buf);
}

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////

bool FullscreenUI::Initialize()
{
  if (s_initialized)
    return true;

  if (s_tried_to_initialize)
    return false;

  ImGuiFullscreen::SetTheme();
  ImGuiFullscreen::UpdateLayoutScale();

  if (!ImGuiManager::AddFullscreenFontsIfMissing() || !ImGuiFullscreen::Initialize("fullscreenui/placeholder.png") ||
      !LoadResources())
  {
    DestroyResources();
    ImGuiFullscreen::Shutdown();
    s_tried_to_initialize = true;
    return false;
  }

  s_initialized = true;
  s_current_main_window = MainWindowType::None;
  s_current_pause_submenu = PauseSubMenu::None;
  s_pause_menu_was_open = false;
  s_was_paused_on_quick_menu_open = false;
  s_about_window_open = false;
  s_hotkey_list_cache = InputManager::GetHotkeyList();
  // GetMTGS().SetRunIdle(true);

  if (!System::IsValid())
    SwitchToLanding();

  // force vsync on so we don't run at thousands of fps
  // Initialize is called on the GS thread, so we can access the display directly.
  UpdateForcedVsync(System::GetState() != System::State::Running);

  return true;
}

bool FullscreenUI::IsInitialized()
{
  return s_initialized;
}

bool FullscreenUI::HasActiveWindow()
{
  return s_initialized && (s_current_main_window != MainWindowType::None || s_save_state_selector_open ||
                           ImGuiFullscreen::IsChoiceDialogOpen() || ImGuiFullscreen::IsFileSelectorOpen());
}

void FullscreenUI::UpdateForcedVsync(bool should_force)
{
  // force vsync on so we don't run at thousands of fps
  // toss it through regardless of the mode, because options can change it
  g_host_display->SetVSync((should_force && !g_settings.video_sync_enabled) ? true : false);
}

void FullscreenUI::OnSystemStarted()
{
  if (!IsInitialized())
    return;

  s_current_main_window = MainWindowType::None;
  QueueResetFocus();
}

void FullscreenUI::OnSystemPaused()
{
  if (!IsInitialized())
    return;

  UpdateForcedVsync(true);
}

void FullscreenUI::OnSystemResumed()
{
  if (!IsInitialized())
    return;

  UpdateForcedVsync(false);
}

void FullscreenUI::OnSystemDestroyed()
{
  if (!IsInitialized())
    return;

  s_pause_menu_was_open = false;
  SwitchToLanding();
  UpdateForcedVsync(true);
}

void FullscreenUI::OnRunningGameChanged()
{
  if (!IsInitialized())
    return;

  const std::string& path = System::GetRunningPath();
  const std::string& serial = System::GetRunningCode();
  if (!serial.empty())
    s_current_game_subtitle = fmt::format("{0} - {1}", serial, Path::GetFileName(path));
  else
    s_current_game_subtitle = {};
}

void FullscreenUI::PauseForMenuOpen()
{
  s_was_paused_on_quick_menu_open = (System::GetState() == System::State::Paused);
  if (g_settings.pause_on_menu && !s_was_paused_on_quick_menu_open)
    Host::RunOnCPUThread([]() { System::PauseSystem(true); });

  s_pause_menu_was_open = true;
}

void FullscreenUI::OpenPauseMenu()
{
  if (!System::IsValid())
    return;

  if (!Initialize() || s_current_main_window != MainWindowType::None)
    return;

  PauseForMenuOpen();
  s_current_main_window = MainWindowType::PauseMenu;
  s_current_pause_submenu = PauseSubMenu::None;
  QueueResetFocus();
}

void FullscreenUI::ClosePauseMenu()
{
  if (!IsInitialized() || !System::IsValid())
    return;

  if (System::GetState() == System::State::Paused && !s_was_paused_on_quick_menu_open)
    Host::RunOnCPUThread([]() { System::PauseSystem(false); });

  s_current_main_window = MainWindowType::None;
  s_current_pause_submenu = PauseSubMenu::None;
  s_pause_menu_was_open = false;
  QueueResetFocus();
}

void FullscreenUI::OpenPauseSubMenu(PauseSubMenu submenu)
{
  s_current_main_window = MainWindowType::PauseMenu;
  s_current_pause_submenu = submenu;
  QueueResetFocus();
}

void FullscreenUI::Shutdown()
{
  CloseSaveStateSelector();
  s_cover_image_map.clear();
  s_game_list_sorted_entries = {};
  s_game_list_directories_cache = {};
  s_fullscreen_mode_list_cache = {};
  s_graphics_adapter_list_cache = {};
  s_hotkey_list_cache = {};
  s_current_game_subtitle = {};
  DestroyResources();
  ImGuiFullscreen::Shutdown();
  s_initialized = false;
  s_tried_to_initialize = false;
}

void FullscreenUI::Render()
{
  if (!s_initialized)
    return;

  for (std::unique_ptr<HostDisplayTexture>& tex : s_cleanup_textures)
    tex.reset();
  s_cleanup_textures.clear();
  ImGuiFullscreen::UploadAsyncTextures();

  ImGuiFullscreen::BeginLayout();

  switch (s_current_main_window)
  {
    case MainWindowType::Landing:
      DrawLandingWindow();
      break;
    case MainWindowType::GameList:
      DrawGameListWindow();
      break;
    case MainWindowType::Settings:
      DrawSettingsWindow();
      break;
    case MainWindowType::PauseMenu:
      DrawPauseMenu(s_current_main_window);
      break;
#ifdef WITH_CHEEVOS
    case MainWindowType::Achievements:
      DrawAchievementsWindow();
      break;
    case MainWindowType::Leaderboards:
      DrawLeaderboardsWindow();
      break;
#endif
    default:
      break;
  }

  if (s_save_state_selector_open)
    DrawSaveStateSelector(s_save_state_selector_loading, false);

  if (s_about_window_open)
    DrawAboutWindow();

  if (s_input_binding_type != Controller::ControllerBindingType::Unknown)
    DrawInputBindingWindow();

  ImGuiFullscreen::EndLayout();

  if (s_settings_changed.exchange(false, std::memory_order_release))
  {
    auto lock = Host::GetSettingsLock();
    GetEditingSettingsInterface()->Save();
    Host::RunOnCPUThread([]() { System::ApplySettings(false); });
  }

  ImGuiFullscreen::ResetCloseMenuIfNeeded();
}

void FullscreenUI::ReturnToMainWindow()
{
  if (s_pause_menu_was_open)
    ClosePauseMenu();

  s_current_main_window = System::IsValid() ? MainWindowType::None : MainWindowType::Landing;
}

bool FullscreenUI::LoadResources()
{
  s_app_icon_texture = LoadTexture("fullscreenui/duck.png");

  s_fallback_disc_texture = LoadTexture("fullscreenui/media-cdrom.png");
  s_fallback_exe_texture = LoadTexture("fullscreenui/applications-system.png");
  s_fallback_exe_texture = LoadTexture("fullscreenui/multimedia-player.png");
  s_fallback_playlist_texture = LoadTexture("fullscreenui/address-book-new.png");

  for (u32 i = 0; i < static_cast<u32>(GameDatabase::CompatibilityRating::Count); i++)
    s_game_compatibility_textures[i] = LoadTexture(fmt::format("fullscreenui/star-{}.png", i).c_str());

  return true;
}

void FullscreenUI::DestroyResources()
{
  s_app_icon_texture.reset();
  s_fallback_playlist_texture.reset();
  s_fallback_psf_texture.reset();
  s_fallback_exe_texture.reset();
  s_fallback_disc_texture.reset();
  for (auto& tex : s_game_compatibility_textures)
    tex.reset();
  for (auto& tex : s_cleanup_textures)
    tex.reset();
}

//////////////////////////////////////////////////////////////////////////
// Utility
//////////////////////////////////////////////////////////////////////////

ImGuiFullscreen::FileSelectorFilters FullscreenUI::GetDiscImageFilters()
{
  return {"*.bin",   "*.cue",    "*.iso", "*.img", "*.chd",     "*.ecm", "*.mds",
          "*.psexe", "*.ps-exe", "*.exe", "*.psf", "*.minipsf", "*.m3u", "*.pbp"};
}

void FullscreenUI::DoStartPath(std::string path, std::string state, std::optional<bool> fast_boot)
{
  if (System::IsValid())
    return;

  SystemBootParameters params;
  params.filename = std::move(path);
  params.save_state = std::move(state);
  params.override_fast_boot = std::move(fast_boot);
  Host::RunOnCPUThread([params = std::move(params)]() {
    if (System::IsValid())
      return;

    System::BootSystem(std::move(params));
  });
}

void FullscreenUI::DoResume()
{
  std::string path(System::GetMostRecentResumeSaveStatePath());
  if (path.empty())
  {
    ShowToast({}, "No resume save state found.");
    return;
  }

  DoStartPath({}, std::move(path));
}

void FullscreenUI::DoStartFile()
{
  auto callback = [](const std::string& path) {
    if (!path.empty())
      DoStartPath(path);

    QueueResetFocus();
    CloseFileSelector();
  };

  OpenFileSelector(ICON_FA_COMPACT_DISC "  Select Disc Image", false, std::move(callback), GetDiscImageFilters());
}

void FullscreenUI::DoStartBIOS()
{
  Host::RunOnCPUThread([]() {
    if (System::IsValid())
      return;

    SystemBootParameters params;
    System::BootSystem(std::move(params));
  });
}

void FullscreenUI::DoShutdown(bool save_state)
{
  Host::RunOnCPUThread([save_state]() { Host::RequestSystemShutdown(false, save_state); });
}

void FullscreenUI::DoReset()
{
  Host::RunOnCPUThread(System::ResetSystem);
}

void FullscreenUI::DoToggleFastForward()
{
  Host::RunOnCPUThread([]() {
    if (!System::IsValid())
      return;

    System::SetFastForwardEnabled(!System::IsFastForwardEnabled());
  });
}

void FullscreenUI::DoToggleSoftwareRenderer()
{
  Host::RunOnCPUThread(System::ToggleSoftwareRendering);
}

void FullscreenUI::DoChangeDiscFromFile()
{
  auto callback = [](const std::string& path) {
    if (!path.empty())
    {
      if (!GameList::IsScannableFilename(path))
      {
        ShowToast({}, fmt::format("{} is not a valid disc image.", FileSystem::GetDisplayNameFromPath(path)));
      }
      else
      {
        Host::RunOnCPUThread([path]() { System::InsertMedia(path.c_str()); });
      }
    }

    QueueResetFocus();
    CloseFileSelector();
    ReturnToMainWindow();
  };

  OpenFileSelector(ICON_FA_COMPACT_DISC "  Select Disc Image", false, std::move(callback), GetDiscImageFilters(),
                   std::string(Path::GetDirectory(System::GetRunningPath())));
}

void FullscreenUI::DoChangeDisc()
{
  if (!System::HasMediaSubImages())
  {
    DoChangeDiscFromFile();
    return;
  }

  const u32 current_index = System::GetMediaSubImageIndex();
  const u32 count = System::GetMediaSubImageCount();
  ImGuiFullscreen::ChoiceDialogOptions options;
  options.reserve(count + 1);
  options.emplace_back("From File...", false);

  for (u32 i = 0; i < count; i++)
    options.emplace_back(System::GetMediaSubImageTitle(i), i == current_index);

  auto callback = [](s32 index, const std::string& title, bool checked) {
    if (index == 0)
    {
      CloseChoiceDialog();
      DoChangeDiscFromFile();
      return;
    }
    else if (index > 0)
    {
      System::SwitchMediaSubImage(static_cast<u32>(index - 1));
    }

    QueueResetFocus();
    CloseChoiceDialog();
    ReturnToMainWindow();
  };

  OpenChoiceDialog(ICON_FA_COMPACT_DISC "  Select Disc Image", true, std::move(options), std::move(callback));
}

static void DoCheatsMenu()
{
#if 0
  CheatList* cl = System::GetCheatList();
  if (!cl)
  {
    if (!s_host_interface->LoadCheatListFromDatabase() || ((cl = System::GetCheatList()) == nullptr))
    {
      s_host_interface->AddFormattedOSDMessage(10.0f, "No cheats found for %s.", System::GetRunningTitle().c_str());
      ReturnToMainWindow();
      return;
    }
  }

  ImGuiFullscreen::ChoiceDialogOptions options;
  options.reserve(cl->GetCodeCount());
  for (u32 i = 0; i < cl->GetCodeCount(); i++)
  {
    const CheatCode& cc = cl->GetCode(i);
    options.emplace_back(cc.description.c_str(), cc.enabled);
  }

  auto callback = [](s32 index, const std::string& title, bool checked) {
    if (index < 0)
    {
      ReturnToMainWindow();
      return;
    }

    CheatList* cl = System::GetCheatList();
    if (!cl)
      return;

    const CheatCode& cc = cl->GetCode(static_cast<u32>(index));
    if (cc.activation == CheatCode::Activation::Manual)
      cl->ApplyCode(static_cast<u32>(index));
    else
      s_host_interface->SetCheatCodeState(static_cast<u32>(index), checked, true);
  };
  OpenChoiceDialog(ICON_FA_FROWN "  Cheat List", true, std::move(options), std::move(callback));
#endif
}

static void DoToggleAnalogMode()
{
  // hacky way to toggle analog mode
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* ctrl = System::GetController(i);
    if (!ctrl)
      continue;

    const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(ctrl->GetType());
    if (!cinfo)
      continue;

    for (u32 j = 0; j < cinfo->num_bindings; j++)
    {
      const Controller::ControllerBindingInfo& bi = cinfo->bindings[j];
      if (std::strcmp(bi.name, "Analog") == 0)
      {
        ctrl->SetBindState(bi.bind_index, 1.0f);
        ctrl->SetBindState(bi.bind_index, 0.0f);
        break;
      }
    }
  }
}

void FullscreenUI::DoRequestExit()
{
  Host::RunOnCPUThread([]() { Host::RequestExit(g_settings.save_state_on_exit); });
}

void FullscreenUI::DoToggleFullscreen()
{
  Host::RunOnCPUThread([]() { Host::SetFullscreen(!Host::IsFullscreen()); });
}

//////////////////////////////////////////////////////////////////////////
// Landing Window
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::SwitchToLanding()
{
  s_current_main_window = MainWindowType::Landing;
  QueueResetFocus();
}

void FullscreenUI::DrawLandingWindow()
{
  BeginFullscreenColumns();

  if (BeginFullscreenColumnWindow(0.0f, 570.0f, "logo", UIPrimaryDarkColor))
  {
    const float image_size = LayoutScale(380.f);
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() * 0.5f) - (image_size * 0.5f),
                               (ImGui::GetWindowHeight() * 0.5f) - (image_size * 0.5f)));
    ImGui::Image(s_app_icon_texture->GetHandle(), ImVec2(image_size, image_size));
  }
  EndFullscreenColumnWindow();

  if (BeginFullscreenColumnWindow(570.0f, LAYOUT_SCREEN_WIDTH, "menu", UIBackgroundColor))
  {
    ResetFocusHere();

    BeginMenuButtons(7, 0.5f);

    if (MenuButton(" " ICON_FA_PLAY_CIRCLE "  Resume",
                   "Starts the console from where it was before it was last closed."))
    {
      System::GetMostRecentResumeSaveStatePath();
      DoResume();
    }

    if (MenuButton(" " ICON_FA_FOLDER_OPEN "  Start File", "Launch a game by selecting a file/disc image."))
    {
      DoStartFile();
    }

    if (MenuButton(" " ICON_FA_TOOLBOX "  Start BIOS", "Start the console without any disc inserted."))
    {
      DoStartBIOS();
    }

    if (MenuButton(" " ICON_FA_UNDO "  Load State", "Loads a global save state."))
    {
      OpenSaveStateSelector(true);
    }

    if (MenuButton(" " ICON_FA_LIST "  Open Game List",
                   "Launch a game from images scanned from your game directories."))
    {
      SwitchToGameList();
    }

    if (MenuButton(" " ICON_FA_SLIDERS_H "  Settings", "Change settings for the emulator."))
      SwitchToSettings();

    if (MenuButton(" " ICON_FA_SIGN_OUT_ALT "  Exit", "Exits the program."))
    {
      DoRequestExit();
    }

    {
      ImVec2 fullscreen_pos;
      if (FloatingButton(ICON_FA_WINDOW_CLOSE, 0.0f, 0.0f, -1.0f, -1.0f, 1.0f, 0.0f, true, g_large_font,
                         &fullscreen_pos))
      {
        DoRequestExit();
      }

      if (FloatingButton(ICON_FA_EXPAND, fullscreen_pos.x, 0.0f, -1.0f, -1.0f, -1.0f, 0.0f, true, g_large_font,
                         &fullscreen_pos))
      {
        DoToggleFullscreen();
      }

      if (FloatingButton(ICON_FA_QUESTION_CIRCLE, fullscreen_pos.x, 0.0f, -1.0f, -1.0f, -1.0f, 0.0f))
        OpenAboutWindow();
    }

    EndMenuButtons();

    const ImVec2 text_size(g_medium_font->CalcTextSizeA(g_medium_font->FontSize, FLT_MAX, 0.0f, g_scm_tag_str));
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - text_size.x - LayoutScale(15.0f),
                               ImGui::GetWindowHeight() - text_size.y - LayoutScale(15.0f)));
    ImGui::PushFont(g_medium_font);
    ImGui::TextUnformatted(g_scm_tag_str);
    ImGui::PopFont();
  }

  EndFullscreenColumnWindow();

  EndFullscreenColumns();
}

bool FullscreenUI::IsEditingGameSettings()
{
  return static_cast<bool>(s_game_settings_interface);
}

SettingsInterface* FullscreenUI::GetEditingSettingsInterface()
{
  return s_game_settings_interface ? s_game_settings_interface.get() : Host::Internal::GetBaseSettingsLayer();
}

void FullscreenUI::SetSettingsChanged()
{
  s_settings_changed.store(true, std::memory_order_release);
}

void FullscreenUI::DrawInputBindingButton(Controller::ControllerBindingType type, const char* section, const char* name,
                                          const char* display_name, bool show_type)
{
  std::string title(fmt::format("{}/{}", section, name));

  ImRect bb;
  bool visible, hovered, clicked;
  clicked = MenuButtonFrame(title.c_str(), true, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, &visible, &hovered,
                            &bb.Min, &bb.Max);
  if (!visible)
    return;

  const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
  const ImRect title_bb(bb.Min, ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(bb.Min.x, midpoint), bb.Max);

  if (show_type)
  {
    switch (type)
    {
      case Controller::ControllerBindingType::Button:
        title = fmt::format(ICON_FA_DOT_CIRCLE "  {}", display_name);
        break;
      case Controller::ControllerBindingType::Axis:
      case Controller::ControllerBindingType::HalfAxis:
        title = fmt::format(ICON_FA_BULLSEYE "  {}", display_name);
        break;
      case Controller::ControllerBindingType::Motor:
        title = fmt::format(ICON_FA_BELL "  {}", display_name);
        break;
      case Controller::ControllerBindingType::Macro:
        title = fmt::format(ICON_FA_PIZZA_SLICE "  {}", display_name);
        break;
      default:
        title = display_name;
        break;
    }
  }

  ImGui::PushFont(g_large_font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, show_type ? title.c_str() : display_name, nullptr, nullptr,
                           ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  const std::optional<std::string> value(GetEditingSettingsInterface()->GetOptionalStringValue(
    section, name, IsEditingGameSettings() ? std::nullopt : std::optional<const char*>("")));
  ImGui::PushFont(g_medium_font);
  ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max,
                           value.has_value() ? (value->empty() ? "No Binding" : value->c_str()) : "Use Global Setting",
                           nullptr, nullptr, ImVec2(0.0f, 0.0f), &summary_bb);
  ImGui::PopFont();

  if (clicked)
  {
    BeginInputBinding(type, section, name, display_name);
  }
  else if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
  {
    GetEditingSettingsInterface()->DeleteValue(section, name);
    SetSettingsChanged();
  }
}

void FullscreenUI::ClearInputBindingVariables()
{
  s_input_binding_type = Controller::ControllerBindingType::Unknown;
  s_input_binding_section = {};
  s_input_binding_key = {};
  s_input_binding_display_name = {};
  s_input_binding_new_bindings = {};
}

void FullscreenUI::BeginInputBinding(Controller::ControllerBindingType type, const std::string_view& section,
                                     const std::string_view& key, const std::string_view& display_name)
{
  if (s_input_binding_type != Controller::ControllerBindingType::Unknown)
  {
    InputManager::RemoveHook();
    ClearInputBindingVariables();
  }

  s_input_binding_type = type;
  s_input_binding_section = section;
  s_input_binding_key = key;
  s_input_binding_display_name = display_name;
  s_input_binding_new_bindings = {};
  s_input_binding_timer.Reset();

  InputManager::SetHook([](InputBindingKey key, float value) -> InputInterceptHook::CallbackResult {
    // holding the settings lock here will protect the input binding list
    auto lock = Host::GetSettingsLock();

    const float abs_value = std::abs(value);

    for (InputBindingKey other_key : s_input_binding_new_bindings)
    {
      if (other_key.MaskDirection() == key.MaskDirection())
      {
        if (abs_value < 0.5f)
        {
          // if this key is in our new binding list, it's a "release", and we're done
          const std::string new_binding(InputManager::ConvertInputBindingKeysToString(
            s_input_binding_new_bindings.data(), s_input_binding_new_bindings.size()));
          GetEditingSettingsInterface()->SetStringValue(s_input_binding_section.c_str(), s_input_binding_key.c_str(),
                                                        new_binding.c_str());
          SetSettingsChanged();
          ClearInputBindingVariables();
          return InputInterceptHook::CallbackResult::RemoveHookAndStopProcessingEvent;
        }

        // otherwise, keep waiting
        return InputInterceptHook::CallbackResult::StopProcessingEvent;
      }
    }

    // new binding, add it to the list, but wait for a decent distance first, and then wait for release
    if (abs_value >= 0.5f)
    {
      InputBindingKey key_to_add = key;
      key_to_add.negative = (value < 0.0f);
      s_input_binding_new_bindings.push_back(key_to_add);
    }

    return InputInterceptHook::CallbackResult::StopProcessingEvent;
  });
}

void FullscreenUI::DrawInputBindingWindow()
{
  DebugAssert(s_input_binding_type != Controller::ControllerBindingType::Unknown);

  const double time_remaining = INPUT_BINDING_TIMEOUT_SECONDS - s_input_binding_timer.GetTimeSeconds();
  if (time_remaining <= 0.0)
  {
    InputManager::RemoveHook();
    ClearInputBindingVariables();
    return;
  }

  const char* title = ICON_FA_GAMEPAD "  Set Input Binding";
  ImGui::SetNextWindowSize(LayoutScale(500.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup(title);

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  if (ImGui::BeginPopupModal(title, nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
  {
    ImGui::TextWrapped("Setting %s binding %s.", s_input_binding_section.c_str(), s_input_binding_display_name.c_str());
    ImGui::TextUnformatted("Push a controller button or axis now.");
    ImGui::NewLine();
    ImGui::Text("Timing out in %.0f seconds...", time_remaining);
    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(3);
  ImGui::PopFont();
}

bool FullscreenUI::DrawToggleSetting(const char* title, const char* summary, const char* section, const char* key,
                                     bool default_value, bool enabled, float height, ImFont* font, ImFont* summary_font)
{
  SettingsInterface* bsi = GetEditingSettingsInterface();
  if (!IsEditingGameSettings())
  {
    bool value = bsi->GetBoolValue(section, key, default_value);
    if (!ToggleButton(title, summary, &value, enabled, height, font, summary_font))
      return false;

    bsi->SetBoolValue(section, key, value);
  }
  else
  {
    std::optional<bool> value(false);
    if (!bsi->GetBoolValue(section, key, &value.value()))
      value.reset();
    if (!ThreeWayToggleButton(title, summary, &value, enabled, height, font, summary_font))
      return false;

    if (value.has_value())
      bsi->SetBoolValue(section, key, value.value());
    else
      bsi->DeleteValue(section, key);
  }

  SetSettingsChanged();
  return true;
}

void FullscreenUI::DrawIntListSetting(const char* title, const char* summary, const char* section, const char* key,
                                      int default_value, const char* const* options, size_t option_count,
                                      int option_offset, bool enabled, float height, ImFont* font, ImFont* summary_font)
{
  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings();
  const std::optional<int> value =
    bsi->GetOptionalIntValue(section, key, game_settings ? std::nullopt : std::optional<int>(default_value));
  const int index = value.has_value() ? (value.value() - option_offset) : std::numeric_limits<int>::min();
  const char* value_text = (value.has_value()) ?
                             ((index < 0 || static_cast<size_t>(index) >= option_count) ? "Unknown" : options[index]) :
                             "Use Global Setting";

  if (MenuButtonWithValue(title, summary, value_text, enabled, height, font, summary_font))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(option_count + 1);
    if (game_settings)
      cd_options.emplace_back("Use Global Setting", !value.has_value());
    for (size_t i = 0; i < option_count; i++)
      cd_options.emplace_back(options[i], (i == static_cast<size_t>(index)));
    OpenChoiceDialog(title, false, std::move(cd_options),
                     [section, key, option_offset](s32 index, const std::string& title, bool checked) {
                       if (index >= 0)
                       {
                         auto lock = Host::GetSettingsLock();
                         SettingsInterface* bsi = GetEditingSettingsInterface();
                         if (IsEditingGameSettings())
                         {
                           if (index == 0)
                             bsi->DeleteValue(section, key);
                           else
                             bsi->SetIntValue(section, key, index - 1 + option_offset);
                         }
                         else
                         {
                           bsi->SetIntValue(section, key, index + option_offset);
                         }

                         SetSettingsChanged();
                       }

                       CloseChoiceDialog();
                     });
  }
}

void FullscreenUI::DrawIntRangeSetting(const char* title, const char* summary, const char* section, const char* key,
                                       int default_value, int min_value, int max_value, const char* format,
                                       bool enabled, float height, ImFont* font, ImFont* summary_font)
{
  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings();
  const std::optional<int> value =
    bsi->GetOptionalIntValue(section, key, game_settings ? std::nullopt : std::optional<int>(default_value));
  const std::string value_text(value.has_value() ? StringUtil::StdStringFromFormat(format, value.value()) :
                                                   std::string("Use Global Setting"));

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled, height, font, summary_font))
    ImGui::OpenPopup(title);

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 180.0f));

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  if (ImGui::BeginPopupModal(title, nullptr,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
  {
    ImGui::SetNextItemWidth(LayoutScale(450.0f));
    s32 dlg_value = static_cast<s32>(value.value_or(default_value));
    if (ImGui::SliderInt("##value", &dlg_value, min_value, max_value, format, ImGuiSliderFlags_NoInput))
    {
      if (IsEditingGameSettings() && dlg_value == default_value)
        bsi->DeleteValue(section, key);
      else
        bsi->SetIntValue(section, key, dlg_value);

      SetSettingsChanged();
    }

    BeginMenuButtons();
    if (MenuButton("OK", nullptr, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
      ImGui::CloseCurrentPopup();
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(3);
  ImGui::PopFont();
}

void FullscreenUI::DrawFloatRangeSetting(const char* title, const char* summary, const char* section, const char* key,
                                         float default_value, float min_value, float max_value, const char* format,
                                         bool enabled, float height, ImFont* font, ImFont* summary_font)
{
  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings();
  const std::optional<float> value =
    bsi->GetOptionalFloatValue(section, key, game_settings ? std::nullopt : std::optional<float>(default_value));
  const std::string value_text(value.has_value() ? StringUtil::StdStringFromFormat(format, value.value()) :
                                                   std::string("Use Global Setting"));

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled, height, font, summary_font))
    ImGui::OpenPopup(title);

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 180.0f));

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  if (ImGui::BeginPopupModal(title, nullptr,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
  {
    ImGui::SetNextItemWidth(LayoutScale(450.0f));
    float dlg_value = value.value_or(default_value);
    if (ImGui::SliderFloat("##value", &dlg_value, min_value, max_value, format, ImGuiSliderFlags_NoInput))
    {
      if (IsEditingGameSettings() && dlg_value == default_value)
        bsi->DeleteValue(section, key);
      else
        bsi->SetFloatValue(section, key, dlg_value);

      SetSettingsChanged();
    }

    BeginMenuButtons();
    if (MenuButton("OK", nullptr, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
      ImGui::CloseCurrentPopup();
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(3);
  ImGui::PopFont();
}

void FullscreenUI::DrawIntRectSetting(const char* title, const char* summary, const char* section, const char* left_key,
                                      int default_left, const char* top_key, int default_top, const char* right_key,
                                      int default_right, const char* bottom_key, int default_bottom, int min_value,
                                      int max_value, const char* format, bool enabled, float height, ImFont* font,
                                      ImFont* summary_font)
{
  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings();
  const std::optional<int> left_value =
    bsi->GetOptionalIntValue(section, left_key, game_settings ? std::nullopt : std::optional<int>(default_left));
  const std::optional<int> top_value =
    bsi->GetOptionalIntValue(section, top_key, game_settings ? std::nullopt : std::optional<int>(default_top));
  const std::optional<int> right_value =
    bsi->GetOptionalIntValue(section, right_key, game_settings ? std::nullopt : std::optional<int>(default_right));
  const std::optional<int> bottom_value =
    bsi->GetOptionalIntValue(section, bottom_key, game_settings ? std::nullopt : std::optional<int>(default_bottom));
  const std::string value_text(fmt::format(
    "{}/{}/{}/{}",
    left_value.has_value() ? StringUtil::StdStringFromFormat(format, left_value.value()) : std::string("Default"),
    top_value.has_value() ? StringUtil::StdStringFromFormat(format, top_value.value()) : std::string("Default"),
    right_value.has_value() ? StringUtil::StdStringFromFormat(format, right_value.value()) : std::string("Default"),
    bottom_value.has_value() ? StringUtil::StdStringFromFormat(format, bottom_value.value()) : std::string("Default")));

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled, height, font, summary_font))
    ImGui::OpenPopup(title);

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 320.0f));

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  if (ImGui::BeginPopupModal(title, nullptr,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
  {
    s32 dlg_left_value = static_cast<s32>(left_value.value_or(default_left));
    s32 dlg_top_value = static_cast<s32>(top_value.value_or(default_top));
    s32 dlg_right_value = static_cast<s32>(right_value.value_or(default_right));
    s32 dlg_bottom_value = static_cast<s32>(bottom_value.value_or(default_bottom));

    const bool left_modified =
      ImGui::SliderInt("Left", &dlg_left_value, min_value, max_value, format, ImGuiSliderFlags_NoInput);
    const bool top_modified =
      ImGui::SliderInt("Top", &dlg_top_value, min_value, max_value, format, ImGuiSliderFlags_NoInput);
    const bool right_modified =
      ImGui::SliderInt("Right", &dlg_right_value, min_value, max_value, format, ImGuiSliderFlags_NoInput);
    const bool bottom_modified =
      ImGui::SliderInt("Bottom", &dlg_bottom_value, min_value, max_value, format, ImGuiSliderFlags_NoInput);

    if (left_modified)
    {
      if (IsEditingGameSettings() && dlg_left_value == default_left)
        bsi->DeleteValue(section, left_key);
      else
        bsi->SetIntValue(section, left_key, dlg_left_value);
    }
    if (top_modified)
    {
      if (IsEditingGameSettings() && dlg_top_value == default_top)
        bsi->DeleteValue(section, top_key);
      else
        bsi->SetIntValue(section, top_key, dlg_top_value);
    }
    if (right_modified)
    {
      if (IsEditingGameSettings() && dlg_right_value == default_right)
        bsi->DeleteValue(section, right_key);
      else
        bsi->SetIntValue(section, right_key, dlg_right_value);
    }
    if (bottom_modified)
    {
      if (IsEditingGameSettings() && dlg_bottom_value == default_bottom)
        bsi->DeleteValue(section, bottom_key);
      else
        bsi->SetIntValue(section, bottom_key, dlg_bottom_value);
    }

    if (left_modified || top_modified || right_modified || bottom_modified)
      SetSettingsChanged();

    BeginMenuButtons();
    if (MenuButton("OK", nullptr, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
      ImGui::CloseCurrentPopup();
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(3);
  ImGui::PopFont();
}

void FullscreenUI::DrawStringListSetting(const char* title, const char* summary, const char* section, const char* key,
                                         const char* default_value, const char* const* options,
                                         const char* const* option_values, size_t option_count, bool enabled,
                                         float height, ImFont* font, ImFont* summary_font)
{
  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings();
  const std::optional<std::string> value(bsi->GetOptionalStringValue(
    section, key, game_settings ? std::nullopt : std::optional<const char*>(default_value)));

  if (option_count == 0)
  {
    // select from null entry
    while (options && options[option_count] != nullptr)
      option_count++;
  }

  size_t index = option_count;
  if (value.has_value())
  {
    for (size_t i = 0; i < option_count; i++)
    {
      if (value == option_values[i])
      {
        index = i;
        break;
      }
    }
  }

  if (MenuButtonWithValue(title, summary,
                          value.has_value() ? ((index < option_count) ? options[index] : "Unknown") :
                                              "Use Global Setting",
                          enabled, height, font, summary_font))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(option_count + 1);
    if (game_settings)
      cd_options.emplace_back("Use Global Setting", !value.has_value());
    for (size_t i = 0; i < option_count; i++)
      cd_options.emplace_back(options[i], (value.has_value() && i == static_cast<size_t>(index)));
    OpenChoiceDialog(title, false, std::move(cd_options),
                     [section, key, option_values](s32 index, const std::string& title, bool checked) {
                       if (index >= 0)
                       {
                         auto lock = Host::GetSettingsLock();
                         SettingsInterface* bsi = GetEditingSettingsInterface();
                         if (IsEditingGameSettings())
                         {
                           if (index == 0)
                             bsi->DeleteValue(section, key);
                           else
                             bsi->SetStringValue(section, key, option_values[index - 1]);
                         }
                         else
                         {
                           bsi->SetStringValue(section, key, option_values[index]);
                         }

                         SetSettingsChanged();
                       }

                       CloseChoiceDialog();
                     });
  }
}

template<typename DataType, typename SizeType>
void FullscreenUI::DrawEnumSetting(const char* title, const char* summary, const char* section, const char* key,
                                   DataType default_value,
                                   std::optional<DataType> (*from_string_function)(const char* str),
                                   const char* (*to_string_function)(DataType value),
                                   const char* (*to_display_string_function)(DataType value), SizeType option_count,
                                   bool enabled /*= true*/,
                                   float height /*= ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT*/,
                                   ImFont* font /*= g_large_font*/, ImFont* summary_font /*= g_medium_font*/)
{
  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings();
  const std::optional<std::string> value(bsi->GetOptionalStringValue(
    section, key, game_settings ? std::nullopt : std::optional<const char*>(to_string_function(default_value))));

  const std::optional<DataType> typed_value(value.has_value() ? from_string_function(value->c_str()) : std::nullopt);

  if (MenuButtonWithValue(title, summary,
                          typed_value.has_value() ? to_display_string_function(typed_value.value()) :
                                                    "Use Global Setting",
                          enabled, height, font, summary_font))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(static_cast<u32>(option_count) + 1);
    if (game_settings)
      cd_options.emplace_back("Use Global Setting", !value.has_value());
    for (u32 i = 0; i < static_cast<u32>(option_count); i++)
      cd_options.emplace_back(to_display_string_function(static_cast<DataType>(i)),
                              (typed_value.has_value() && i == static_cast<u32>(typed_value.value())));
    OpenChoiceDialog(title, false, std::move(cd_options),
                     [section, key, to_string_function](s32 index, const std::string& title, bool checked) {
                       if (index >= 0)
                       {
                         auto lock = Host::GetSettingsLock();
                         SettingsInterface* bsi = GetEditingSettingsInterface();
                         if (IsEditingGameSettings())
                         {
                           if (index == 0)
                             bsi->DeleteValue(section, key);
                           else
                             bsi->SetStringValue(section, key, to_string_function(static_cast<DataType>(index - 1)));
                         }
                         else
                         {
                           bsi->SetStringValue(section, key, to_string_function(static_cast<DataType>(index)));
                         }

                         SetSettingsChanged();
                       }

                       CloseChoiceDialog();
                     });
  }
}

void FullscreenUI::DrawFloatListSetting(const char* title, const char* summary, const char* section, const char* key,
                                        float default_value, const char* const* options, const float* option_values,
                                        size_t option_count, bool enabled, float height, ImFont* font,
                                        ImFont* summary_font)
{
  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings();
  const std::optional<float> value(
    bsi->GetOptionalFloatValue(section, key, game_settings ? std::nullopt : std::optional<float>(default_value)));

  if (option_count == 0)
  {
    // select from null entry
    while (options && options[option_count] != nullptr)
      option_count++;
  }

  size_t index = option_count;
  if (value.has_value())
  {
    for (size_t i = 0; i < option_count; i++)
    {
      if (value == option_values[i])
      {
        index = i;
        break;
      }
    }
  }

  if (MenuButtonWithValue(title, summary,
                          value.has_value() ? ((index < option_count) ? options[index] : "Unknown") :
                                              "Use Global Setting",
                          enabled, height, font, summary_font))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(option_count + 1);
    if (game_settings)
      cd_options.emplace_back("Use Global Setting", !value.has_value());
    for (size_t i = 0; i < option_count; i++)
      cd_options.emplace_back(options[i], (value.has_value() && i == static_cast<size_t>(index)));
    OpenChoiceDialog(title, false, std::move(cd_options),
                     [section, key, option_values](s32 index, const std::string& title, bool checked) {
                       if (index >= 0)
                       {
                         auto lock = Host::GetSettingsLock();
                         SettingsInterface* bsi = GetEditingSettingsInterface();
                         if (IsEditingGameSettings())
                         {
                           if (index == 0)
                             bsi->DeleteValue(section, key);
                           else
                             bsi->SetFloatValue(section, key, option_values[index - 1]);
                         }
                         else
                         {
                           bsi->SetFloatValue(section, key, option_values[index]);
                         }

                         SetSettingsChanged();
                       }

                       CloseChoiceDialog();
                     });
  }
}

void FullscreenUI::DrawFolderSetting(const char* title, const char* section, const char* key,
                                     const std::string& runtime_var,
                                     float height /* = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT */,
                                     ImFont* font /* = g_large_font */, ImFont* summary_font /* = g_medium_font */)
{
  if (MenuButton(title, runtime_var.c_str()))
  {
    OpenFileSelector(title, true, [section = std::string(section), key = std::string(key)](const std::string& dir) {
      if (dir.empty())
        return;

      auto lock = Host::GetSettingsLock();
      std::string relative_path(Path::MakeRelative(dir, EmuFolders::DataRoot));
      GetEditingSettingsInterface()->SetStringValue(section.c_str(), key.c_str(), relative_path.c_str());
      SetSettingsChanged();

      Host::RunOnCPUThread(EmuFolders::Update);

      CloseFileSelector();
    });
  }
}

void FullscreenUI::StartAutomaticBinding(u32 port)
{
  std::vector<std::pair<std::string, std::string>> devices(InputManager::EnumerateDevices());
  if (devices.empty())
  {
    ShowToast({}, "Automatic mapping failed, no devices are available.");
    return;
  }

  std::vector<std::string> names;
  ImGuiFullscreen::ChoiceDialogOptions options;
  options.reserve(devices.size());
  names.reserve(devices.size());
  for (auto& [name, display_name] : devices)
  {
    names.push_back(std::move(name));
    options.emplace_back(std::move(display_name), false);
  }
  OpenChoiceDialog("Select Device", false, std::move(options),
                   [port, names = std::move(names)](s32 index, const std::string& title, bool checked) {
                     if (index < 0)
                       return;

                     const std::string& name = names[index];
                     auto lock = Host::GetSettingsLock();
                     const bool result = InputManager::MapController(*GetEditingSettingsInterface(), port,
                                                                     InputManager::GetGenericBindingMapping(name));

                     // and the toast needs to happen on the UI thread.
                     ShowToast({}, result ? fmt::format("Automatic mapping completed for {}.", name) :
                                            fmt::format("Automatic mapping failed for {}.", name));
                     CloseChoiceDialog();
                   });
}

void FullscreenUI::SwitchToSettings()
{
  s_game_settings_entry.reset();
  s_game_settings_interface.reset();

  // populate the cache with all settings from ini
  auto lock = Host::GetSettingsLock();
  SettingsInterface* bsi = Host::Internal::GetBaseSettingsLayer();

  PopulateGameListDirectoryCache(bsi);
  PopulateGraphicsAdapterList();

  s_current_main_window = MainWindowType::Settings;
  s_settings_page = SettingsPage::Interface;
}

void FullscreenUI::SwitchToGameSettingsForSerial(const std::string_view& serial)
{
  s_game_settings_entry.reset();
  s_game_settings_interface = std::make_unique<INISettingsInterface>(System::GetGameSettingsPath(serial));
  s_game_settings_interface->Load();
  s_current_main_window = MainWindowType::Settings;
  s_settings_page = SettingsPage::Summary;
  QueueResetFocus();
}

void FullscreenUI::SwitchToGameSettings()
{
  if (System::GetRunningCode().empty())
    return;

  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(System::GetRunningPath().c_str());
  if (!entry)
  {
    SwitchToGameSettingsForSerial(System::GetRunningCode());
    return;
  }

  SwitchToGameSettings(entry);
}

void FullscreenUI::SwitchToGameSettingsForPath(const std::string& path)
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(path.c_str());
  if (entry)
    SwitchToGameSettings(entry);
}

void FullscreenUI::SwitchToGameSettings(const GameList::Entry* entry)
{
  SwitchToGameSettingsForSerial(entry->serial);
  s_game_settings_entry = std::make_unique<GameList::Entry>(*entry);
}

void FullscreenUI::PopulateGraphicsAdapterList()
{
  HostDisplay::AdapterAndModeList ml(g_host_display->GetAdapterAndModeList());
  s_graphics_adapter_list_cache = std::move(ml.adapter_names);
  s_fullscreen_mode_list_cache = std::move(ml.fullscreen_modes);
  s_fullscreen_mode_list_cache.insert(s_fullscreen_mode_list_cache.begin(), "Borderless Fullscreen");
}

void FullscreenUI::PopulateGameListDirectoryCache(SettingsInterface* si)
{
  s_game_list_directories_cache.clear();
  for (std::string& dir : si->GetStringList("GameList", "Paths"))
    s_game_list_directories_cache.emplace_back(std::move(dir), false);
  for (std::string& dir : si->GetStringList("GameList", "RecursivePaths"))
    s_game_list_directories_cache.emplace_back(std::move(dir), true);
}

ImGuiFullscreen::ChoiceDialogOptions FullscreenUI::GetGameListDirectoryOptions(bool recursive_as_checked)
{
  ImGuiFullscreen::ChoiceDialogOptions options;
  for (const auto& it : s_game_list_directories_cache)
    options.emplace_back(it.first, it.second && recursive_as_checked);
  return options;
}

void FullscreenUI::DoCopyGameSettings()
{
  //
}

void FullscreenUI::DoClearGameSettings()
{
  if (!s_game_settings_interface)
    return;

  s_game_settings_interface->Clear();
  if (!s_game_settings_interface->GetFileName().empty())
    FileSystem::DeleteFile(s_game_settings_interface->GetFileName().c_str());

  s_settings_changed.store(false, std::memory_order_release);
  Host::RunOnCPUThread([]() { System::ReloadGameSettings(false); });

  ShowToast("Game Settings Cleared", fmt::format("Game settings have been cleared for {}.",
                                                 Path::GetFileTitle(s_game_settings_interface->GetFileName())));
}

#if 0

static bool SettingInfoButton(const SettingInfo& si, const char* section)
{
  // this.. isn't pretty :(
  TinyString title;
  title.Format("%s##%s/%s", si.visible_name, section, si.key);
  switch (si.type)
  {
    case SettingInfo::Type::Boolean:
    {
      bool value = s_host_interface->GetSettingsInterface()->GetBoolValue(
        section, si.key, StringUtil::FromChars<bool>(si.default_value).value_or(false));
      if (ToggleButton(title, si.description, &value))
      {
        s_host_interface->GetSettingsInterface()->SetBoolValue(section, si.key, value);
        return true;
      }

      return false;
    }

    case SettingInfo::Type::Integer:
    {
      int value = s_host_interface->GetSettingsInterface()->GetIntValue(
        section, si.key, StringUtil::FromChars<int>(si.default_value).value_or(0));
      const int min = StringUtil::FromChars<int>(si.min_value).value_or(0);
      const int max = StringUtil::FromChars<int>(si.max_value).value_or(0);
      const int step = StringUtil::FromChars<int>(si.step_value).value_or(0);
      if (RangeButton(title, si.description, &value, min, max, step))
      {
        s_host_interface->GetSettingsInterface()->SetIntValue(section, si.key, value);
        return true;
      }

      return false;
    }

    case SettingInfo::Type::Float:
    {
      float value = s_host_interface->GetSettingsInterface()->GetFloatValue(
        section, si.key, StringUtil::FromChars<float>(si.default_value).value_or(0));
      const float min = StringUtil::FromChars<float>(si.min_value).value_or(0);
      const float max = StringUtil::FromChars<float>(si.max_value).value_or(0);
      const float step = StringUtil::FromChars<float>(si.step_value).value_or(0);
      if (RangeButton(title, si.description, &value, min, max, step))
      {
        s_host_interface->GetSettingsInterface()->SetFloatValue(section, si.key, value);
        return true;
      }

      return false;
    }

    case SettingInfo::Type::Path:
    {
      std::string value = s_host_interface->GetSettingsInterface()->GetStringValue(section, si.key);
      if (MenuButtonWithValue(title, si.description, value.c_str()))
      {
        std::string section_copy(section);
        std::string key_copy(si.key);
        auto callback = [section_copy, key_copy](const std::string& path) {
          if (!path.empty())
          {
            s_host_interface->GetSettingsInterface()->SetStringValue(section_copy.c_str(), key_copy.c_str(),
                                                                     path.c_str());
            s_host_interface->RunLater(SaveAndApplySettings);
          }

          ClearImGuiFocus();
          CloseFileSelector();
        };
        OpenFileSelector(si.visible_name, false, std::move(callback), ImGuiFullscreen::FileSelectorFilters(),
                         std::string(Path::GetDirectory(std::move(value))));
      }

      return false;
    }

    default:
      return false;
  }
}

#endif

#ifdef WITH_CHEEVOS

#if 0
static void DrawAchievementsLoginWindow()
{
  ImGui::SetNextWindowSize(LayoutScale(700.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));
  ImGui::PushFont(g_large_font);

  bool is_open = true;
  if (ImGui::BeginPopupModal("Achievements Login", &is_open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {

    ImGui::TextWrapped("Please enter user name and password for retroachievements.org.");
    ImGui::NewLine();
    ImGui::TextWrapped(
      "Your password will not be saved in DuckStation, an access token will be generated and used instead.");

    ImGui::NewLine();

    static char username[256] = {};
    static char password[256] = {};

    ImGui::Text("User Name: ");
    ImGui::SameLine(LayoutScale(200.0f));
    ImGui::InputText("##username", username, sizeof(username));

    ImGui::Text("Password: ");
    ImGui::SameLine(LayoutScale(200.0f));
    ImGui::InputText("##password", password, sizeof(password), ImGuiInputTextFlags_Password);

    ImGui::NewLine();

    BeginMenuButtons();

    const bool login_enabled = (std::strlen(username) > 0 && std::strlen(password) > 0);

    if (ActiveButton(ICON_FA_KEY "  Login", false, login_enabled))
    {
      Cheevos::LoginAsync(username, password);
      std::memset(username, 0, sizeof(username));
      std::memset(password, 0, sizeof(password));
      ImGui::CloseCurrentPopup();
    }

    if (ActiveButton(ICON_FA_TIMES "  Cancel", false))
      ImGui::CloseCurrentPopup();

    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopFont();
  ImGui::PopStyleVar(2);
}

static bool ConfirmChallengeModeEnable()
{
  if (!System::IsValid())
    return true;

  const bool cheevos_enabled = s_host_interface->GetBoolSettingValue("Cheevos", "Enabled", false);
  const bool cheevos_hardcore = s_host_interface->GetBoolSettingValue("Cheevos", "ChallengeMode", false);
  if (!cheevos_enabled || !cheevos_hardcore)
    return true;

  SmallString message;
  message.AppendString("Enabling hardcore mode will shut down your current game.\n\n");

  if (s_host_interface->ShouldSaveResumeState())
  {
    message.AppendString(
      "The current state will be saved, but you will be unable to load it until you disable hardcore mode.\n\n");
  }

  message.AppendString("Do you want to continue?");

  if (!s_host_interface->ConfirmMessage(message))
    return false;

  SaveAndApplySettings();
  s_host_interface->PowerOffSystem(s_host_interface->ShouldSaveResumeState());
  return true;
}
#endif

#endif

void FullscreenUI::DrawSettingsWindow()
{
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY + LAYOUT_MENU_BUTTON_Y_PADDING * 2.0f + 2.0f));

  const float bg_alpha = System::IsValid() ? 0.90f : 1.0f;

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "settings_category", UIPrimaryColor))
  {
    static constexpr float ITEM_WIDTH = 22.0f;

    static constexpr const char* global_icons[] = {ICON_FA_WINDOW_MAXIMIZE,
                                                   ICON_FA_LIST,
                                                   ICON_FA_HDD,
                                                   ICON_FA_SLIDERS_H,
                                                   ICON_FA_MICROCHIP,
                                                   ICON_FA_GAMEPAD,
                                                   ICON_FA_KEYBOARD,
                                                   ICON_FA_SD_CARD,
                                                   ICON_FA_TV,
                                                   ICON_FA_MAGIC,
                                                   ICON_FA_HEADPHONES,
                                                   ICON_FA_TROPHY,
                                                   ICON_FA_EXCLAMATION_TRIANGLE};
    static constexpr const char* per_game_icons[] = {ICON_FA_PARAGRAPH, ICON_FA_HDD, ICON_FA_SLIDERS_H,
                                                     ICON_FA_SD_CARD,   ICON_FA_TV,  ICON_FA_MAGIC,
                                                     ICON_FA_HEADPHONES};
    static constexpr SettingsPage global_pages[] = {
      SettingsPage::Interface, SettingsPage::GameList,     SettingsPage::Console, SettingsPage::Emulation,
      SettingsPage::BIOS,      SettingsPage::Controller,   SettingsPage::Hotkey,  SettingsPage::MemoryCards,
      SettingsPage::Display,   SettingsPage::Enhancements, SettingsPage::Audio,   SettingsPage::Achievements,
      SettingsPage::Advanced};
    static constexpr SettingsPage per_game_pages[] = {
      SettingsPage::Summary, SettingsPage::Console,      SettingsPage::Emulation, SettingsPage::MemoryCards,
      SettingsPage::Display, SettingsPage::Enhancements, SettingsPage::Audio};
    static constexpr std::array<const char*, static_cast<u32>(SettingsPage::Count)> titles = {
      {"Summary", "Interface Settings", "Game List Settings", "Console Settings", "Emulation Settings", "BIOS Settings",
       "Controller Settings", "Hotkey Settings", "Memory Card Settings", "Display Settings", "Enhancement Settings",
       "Audio Settings", "Achievements Settings", "Advanced Settings"}};

    const bool game_settings = IsEditingGameSettings();
    const u32 count =
      game_settings ? static_cast<u32>(std::size(per_game_pages)) : static_cast<u32>(std::size(global_pages));
    const char* const* icons = game_settings ? per_game_icons : global_icons;
    const SettingsPage* pages = game_settings ? per_game_pages : global_pages;
    u32 index = 0;
    for (u32 i = 0; i < count; i++)
    {
      if (pages[i] == s_settings_page)
      {
        index = i;
        break;
      }
    }

    BeginNavBar();

    if (ImGui::IsNavInputTest(ImGuiNavInput_FocusPrev, ImGuiNavReadMode_Pressed))
    {
      index = (index == 0) ? (count - 1) : (index - 1);
      s_settings_page = pages[index];
    }
    else if (ImGui::IsNavInputTest(ImGuiNavInput_FocusNext, ImGuiNavReadMode_Pressed))
    {
      index = (index + 1) % count;
      s_settings_page = pages[index];
    }

    if (NavButton(ICON_FA_BACKWARD, true, true))
      ReturnToMainWindow();

    if (s_game_settings_entry)
      NavTitle(fmt::format("{} ({})", titles[static_cast<u32>(pages[index])], s_game_settings_entry->title).c_str());
    else
      NavTitle(titles[static_cast<u32>(pages[index])]);

    RightAlignNavButtons(count, ITEM_WIDTH, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    for (u32 i = 0; i < count; i++)
    {
      if (NavButton(icons[i], i == index, true, ITEM_WIDTH, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
      {
        s_settings_page = pages[i];
      }
    }

    EndNavBar();
  }

  EndFullscreenWindow();

  if (BeginFullscreenWindow(ImVec2(0.0f, heading_size.y), ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y),
                            "settings_parent",
                            ImVec4(UIBackgroundColor.x, UIBackgroundColor.y, UIBackgroundColor.z, bg_alpha)))
  {
    ResetFocusHere();

    if (WantsToCloseMenu())
    {
      if (ImGui::IsWindowFocused())
        ReturnToMainWindow();
    }

    auto lock = Host::GetSettingsLock();

    switch (s_settings_page)
    {
      case SettingsPage::Summary:
        DrawSummarySettingsPage();
        break;

      case SettingsPage::Interface:
        DrawInterfaceSettingsPage();
        break;

      case SettingsPage::GameList:
        DrawGameListSettingsPage();
        break;

      case SettingsPage::BIOS:
        DrawBIOSSettingsPage();
        break;

      case SettingsPage::Emulation:
        DrawEmulationSettingsPage();
        break;

      case SettingsPage::Console:
        DrawConsoleSettingsPage();
        break;

      case SettingsPage::Display:
        DrawDisplaySettingsPage();
        break;

      case SettingsPage::Enhancements:
        DrawEnhancementSettingsPage();
        break;

      case SettingsPage::Audio:
        DrawAudioSettingsPage();
        break;

      case SettingsPage::MemoryCards:
        DrawMemoryCardSettingsPage();
        break;

      case SettingsPage::Controller:
        DrawControllerSettingsPage();
        break;

      case SettingsPage::Hotkey:
        DrawHotkeySettingsPage();
        break;

      case SettingsPage::Achievements:
        DrawAchievementsSettingsPage();
        break;

      case SettingsPage::Advanced:
        DrawAdvancedSettingsPage();
        break;

      default:
        break;
    }
  }

  EndFullscreenWindow();
}

void FullscreenUI::DrawSummarySettingsPage()
{
  BeginMenuButtons();

  MenuHeading("Details");

  if (s_game_settings_entry)
  {
    // TODO: Make clicking these copy to clipboard.
    MenuButton(ICON_FA_WINDOW_MAXIMIZE "  Title", s_game_settings_entry->title.c_str(), true);
    MenuButton(ICON_FA_PAGER "  Serial", s_game_settings_entry->serial.c_str(), true);
    // MenuButton(ICON_FA_CODE "  CRC", fmt::format("{:08X}", s_game_settings_entry->crc).c_str(), true);
    MenuButton(ICON_FA_COMPACT_DISC "  Type", GameList::GetEntryTypeDisplayName(s_game_settings_entry->type), true);
    MenuButton(ICON_FA_BOX "  Region", Settings::GetDiscRegionDisplayName(s_game_settings_entry->region), true);
    MenuButton(ICON_FA_STAR "  Compatibility Rating",
               GameDatabase::GetCompatibilityRatingDisplayName(s_game_settings_entry->compatibility), true);
    MenuButton(ICON_FA_FOLDER_OPEN "  Path", s_game_settings_entry->path.c_str(), true);
  }
  else
  {
    MenuButton(ICON_FA_BAN "  Details unavailable for game not scanned in game list.", "");
  }

  MenuHeading("Options");

  // TODO: Implement this.
  if (MenuButton(ICON_FA_COPY "  Copy Settings", "Copies the current global settings to this game.", false))
    DoCopyGameSettings();
  if (MenuButton(ICON_FA_TRASH "  Clear Settings", "Clears all settings set for this game."))
    DoClearGameSettings();

  EndMenuButtons();
}

void FullscreenUI::DrawInterfaceSettingsPage()
{
  BeginMenuButtons();

  MenuHeading("Behavior");

  DrawToggleSetting("Pause On Start", "Pauses the emulator when a game is started.", "Main", "StartPaused", false);
  DrawToggleSetting("Pause On Focus Loss",
                    "Pauses the emulator when you minimize the window or switch to another "
                    "application, and unpauses when you switch back.",
                    "Main", "PauseOnFocusLoss", false);
  DrawToggleSetting("Pause On Menu",
                    "Pauses the emulator when you open the quick menu, and unpauses when you close it.", "Main",
                    "PauseOnMenu", true);
  DrawToggleSetting("Confirm Power Off",
                    "Determines whether a prompt will be displayed to confirm shutting down the emulator/game "
                    "when the hotkey is pressed.",
                    "Main", "ConfirmPowerOff", true);
  DrawToggleSetting("Save State On Exit",
                    "Automatically saves the emulator state when powering down or exiting. You can then "
                    "resume directly from where you left off next time.",
                    "Main", "SaveStateOnExit", true);
  DrawToggleSetting("Start Fullscreen", "Automatically switches to fullscreen mode when the program is started.",
                    "Main", "StartFullscreen", false);
  DrawToggleSetting("Hide Cursor In Fullscreen",
                    "Hides the mouse pointer/cursor when the emulator is in fullscreen mode.", "Main",
                    "HideCursorInFullscreen", true);
  DrawToggleSetting("Inhibit Screensaver",
                    "Prevents the screen saver from activating and the host from sleeping while emulation is running.",
                    "Main", "InhibitScreensaver", true);
  DrawToggleSetting("Load Devices From Save States",
                    "When enabled, memory cards and controllers will be overwritten when save states are loaded.",
                    "Main", "LoadDevicesFromSaveStates", false);
  DrawToggleSetting("Apply Per-Game Settings",
                    "When enabled, per-game settings will be applied, and incompatible enhancements will be disabled.",
                    "Main", "ApplyGameSettings", true);
  DrawToggleSetting("Automatically Load Cheats", "Automatically loads and applies cheats on game start.", "Main",
                    "AutoLoadCheats", true);

#ifdef WITH_DISCORD_PRESENCE
  MenuHeading("Integration");
  DrawToggleSetting("Enable Discord Presence",
                    "Shows the game you are currently playing as part of your profile on Discord.", "Main",
                    "EnableDiscordPresence", false);
#endif

  EndMenuButtons();
}

void FullscreenUI::DrawGameListSettingsPage()
{
  BeginMenuButtons();

  MenuHeading("Game List");

  if (MenuButton(ICON_FA_FOLDER_PLUS "  Add Search Directory", "Adds a new directory to the game search list."))
  {
    OpenFileSelector(ICON_FA_FOLDER_PLUS "  Add Search Directory", true, [](const std::string& dir) {
      if (!dir.empty())
      {
        auto lock = Host::GetSettingsLock();
        SettingsInterface* bsi = Host::Internal::GetBaseSettingsLayer();

        bsi->AddToStringList("GameList", "RecursivePaths", dir.c_str());
        bsi->RemoveFromStringList("GameList", "Paths", dir.c_str());
        bsi->Save();
        PopulateGameListDirectoryCache(bsi);
        Host::RefreshGameListAsync(false);
      }

      CloseFileSelector();
    });
  }

  if (MenuButton(ICON_FA_FOLDER_OPEN "  Change Recursive Directories",
                 "Sets whether subdirectories are searched for each game directory"))
  {
    OpenChoiceDialog(ICON_FA_FOLDER_OPEN "  Change Recursive Directories", true, GetGameListDirectoryOptions(true),
                     [](s32 index, const std::string& title, bool checked) {
                       if (index < 0)
                         return;

                       auto lock = Host::GetSettingsLock();
                       SettingsInterface* bsi = Host::Internal::GetBaseSettingsLayer();
                       if (checked)
                       {
                         bsi->RemoveFromStringList("GameList", "Paths", title.c_str());
                         bsi->AddToStringList("GameList", "RecursivePaths", title.c_str());
                       }
                       else
                       {
                         bsi->RemoveFromStringList("GameList", "RecursivePaths", title.c_str());
                         bsi->AddToStringList("GameList", "Paths", title.c_str());
                       }

                       bsi->Save();
                       PopulateGameListDirectoryCache(bsi);
                       Host::RefreshGameListAsync(false);
                     });
  }

  if (MenuButton(ICON_FA_FOLDER_MINUS "  Remove Search Directory", "Removes a directory from the game search list."))
  {
    OpenChoiceDialog(ICON_FA_FOLDER_MINUS "  Remove Search Directory", false, GetGameListDirectoryOptions(false),
                     [](s32 index, const std::string& title, bool checked) {
                       if (index < 0)
                         return;

                       auto lock = Host::GetSettingsLock();
                       SettingsInterface* bsi = Host::Internal::GetBaseSettingsLayer();
                       bsi->RemoveFromStringList("GameList", "Paths", title.c_str());
                       bsi->RemoveFromStringList("GameList", "RecursivePaths", title.c_str());
                       bsi->Save();
                       PopulateGameListDirectoryCache(bsi);
                       Host::RefreshGameListAsync(false);
                       CloseChoiceDialog();
                     });
  }

  if (MenuButton(ICON_FA_SEARCH "  Scan For New Games", "Identifies any new files added to the game directories."))
    Host::RefreshGameListAsync(false);
  if (MenuButton(ICON_FA_SEARCH_PLUS "  Rescan All Games", "Forces a full rescan of all games previously identified."))
    Host::RefreshGameListAsync(true);

  MenuHeading("Search Directories");
  for (const auto& it : s_game_list_directories_cache)
    MenuButton(it.first.c_str(), it.second ? "Scanning Subdirectories" : "Not Scanning Subdirectories", false);

  EndMenuButtons();
}

void FullscreenUI::DrawBIOSSettingsPage()
{
#if 0
  static constexpr auto config_keys = make_array("", "PathNTSCJ", "PathNTSCU", "PathPAL");
  static std::string bios_region_filenames[static_cast<u32>(ConsoleRegion::Count)];
  static std::string bios_directory;
  static bool bios_filenames_loaded = false;

  if (!bios_filenames_loaded)
  {
    for (u32 i = 0; i < static_cast<u32>(ConsoleRegion::Count); i++)
    {
      if (i == static_cast<u32>(ConsoleRegion::Auto))
        continue;
      bios_region_filenames[i] = s_host_interface->GetSettingsInterface()->GetStringValue("BIOS", config_keys[i]);
    }
    bios_directory = s_host_interface->GetBIOSDirectory();
    bios_filenames_loaded = true;
  }

  BeginMenuButtons();

  MenuHeading("BIOS Selection");

  for (u32 i = 0; i < static_cast<u32>(ConsoleRegion::Count); i++)
  {
    const ConsoleRegion region = static_cast<ConsoleRegion>(i);
    if (region == ConsoleRegion::Auto)
      continue;

    TinyString title;
    title.Format("BIOS for %s", Settings::GetConsoleRegionName(region));

    if (MenuButtonWithValue(title,
      SmallString::FromFormat("BIOS to use when emulating %s consoles.",
        Settings::GetConsoleRegionDisplayName(region)),
      bios_region_filenames[i].c_str()))
    {
      ImGuiFullscreen::ChoiceDialogOptions options;
      auto images = s_host_interface->FindBIOSImagesInDirectory(s_host_interface->GetBIOSDirectory().c_str());
      options.reserve(images.size() + 1);
      options.emplace_back("Auto-Detect", bios_region_filenames[i].empty());
      for (auto& [path, info] : images)
      {
        const bool selected = bios_region_filenames[i] == path;
        options.emplace_back(std::move(path), selected);
      }

      OpenChoiceDialog(title, false, std::move(options), [i](s32 index, const std::string& path, bool checked) {
        if (index >= 0)
        {
          bios_region_filenames[i] = path;
          s_host_interface->GetSettingsInterface()->SetStringValue("BIOS", config_keys[i], path.c_str());
          s_host_interface->GetSettingsInterface()->Save();
        }
        CloseChoiceDialog();
      });
    }
  }

  if (MenuButton("BIOS Directory", bios_directory.c_str()))
  {
    OpenFileSelector("BIOS Directory", true, [](const std::string& path) {
      if (!path.empty())
      {
        bios_directory = path;
        s_host_interface->GetSettingsInterface()->SetStringValue("BIOS", "SearchDirectory", path.c_str());
        s_host_interface->GetSettingsInterface()->Save();
      }
      CloseFileSelector();
    });
  }

  MenuHeading("Patches");

  settings_changed |= ToggleButton("Enable Fast Boot", "Patches the BIOS to skip the boot animation. Safe to enable.",
    &s_settings_copy.bios_patch_fast_boot);
  settings_changed |= ToggleButton(
    "Enable TTY Output", "Patches the BIOS to log calls to printf(). Only use when debugging, can break games.",
    &s_settings_copy.bios_patch_tty_enable);

  EndMenuButtons();
#endif
}

void FullscreenUI::DrawConsoleSettingsPage()
{
  static constexpr auto cdrom_read_speeds =
    make_array("None (Double Speed)", "2x (Quad Speed)", "3x (6x Speed)", "4x (8x Speed)", "5x (10x Speed)",
               "6x (12x Speed)", "7x (14x Speed)", "8x (16x Speed)", "9x (18x Speed)", "10x (20x Speed)");

  static constexpr auto cdrom_seek_speeds =
    make_array("Infinite/Instantaneous", "None (Normal Speed)", "2x", "3x", "4x", "5x", "6x", "7x", "8x", "9x", "10x");

  BeginMenuButtons();

  MenuHeading("Console Settings");

  DrawEnumSetting("Region", "Determines the emulated hardware type.", "Console", "Region",
                  Settings::DEFAULT_CONSOLE_REGION, &Settings::ParseConsoleRegionName, &Settings::GetConsoleRegionName,
                  &Settings::GetConsoleRegionDisplayName, ConsoleRegion::Count);
  DrawToggleSetting("Enable 8MB RAM",
                    "Enables an additional 6MB of RAM to obtain a total of 2+6 = 8MB, usually present on dev consoles.",
                    "Console", "Enable8MBRAM", false);

  MenuHeading("CPU Emulation (MIPS R3000A Derivative)");

  DrawEnumSetting("Execution Mode", "Determines how the emulated CPU executes instructions. Recompiler is recommended.",
                  "CPU", "ExecutionMode", Settings::DEFAULT_CPU_EXECUTION_MODE, &Settings::ParseCPUExecutionMode,
                  &Settings::GetCPUExecutionModeName, &Settings::GetCPUExecutionModeDisplayName,
                  CPUExecutionMode::Count);

  DrawToggleSetting("Enable Overclocking", "When this option is chosen, the clock speed set below will be used.", "CPU",
                    "OverclockEnable", false);

#if 0
  s32 overclock_percent =
    s_settings_copy.cpu_overclock_enable ? static_cast<s32>(s_settings_copy.GetCPUOverclockPercent()) : 100;
  if (RangeButton("Overclocking Percentage",
    "Selects the percentage of the normal clock speed the emulated hardware will run at.",
    &overclock_percent, 10, 1000, 10, "%d%%", s_settings_copy.cpu_overclock_enable))
  {
    s_settings_copy.SetCPUOverclockPercent(static_cast<u32>(overclock_percent));
    settings_changed = true;
  }
#endif

  MenuHeading("CD-ROM Emulation");

  DrawIntListSetting(
    "Read Speedup",
    "Speeds up CD-ROM reads by the specified factor. May improve loading speeds in some games, and break others.",
    "CDROM", "ReadSpeedup", 1, cdrom_read_speeds.data(), cdrom_read_speeds.size(), 1);
  DrawIntListSetting(
    "Read Speedup",
    "Speeds up CD-ROM seeks by the specified factor. May improve loading speeds in some games, and break others.",
    "CDROM", "SeekSpeedup", 1, cdrom_seek_speeds.data(), cdrom_seek_speeds.size());

  DrawIntRangeSetting(
    "Readahead Sectors",
    "Reduces hitches in emulation by reading/decompressing CD data asynchronously on a worker thread.", "CDROM",
    "ReadaheadSectors", Settings::DEFAULT_CDROM_READAHEAD_SECTORS, 0, 32, "%d sectors");

  DrawToggleSetting("Enable Region Check", "Simulates the region check present in original, unmodified consoles.",
                    "CDROM", "RegionCheck", false);
  DrawToggleSetting(
    "Preload Images to RAM",
    "Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay.", "CDROM",
    "LoadImageToRAM", false);
  DrawToggleSetting(
    "Apply Image Patches",
    "Automatically applies patches to disc images when they are present, currently only PPF is supported.", "CDROM",
    "LoadImagePatches", false);

  EndMenuButtons();
}

void FullscreenUI::DrawEmulationSettingsPage()
{
  static constexpr auto emulation_speed_values =
    make_array(0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f,
               3.5f, 4.0f, 4.5f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f);
  static constexpr auto emulation_speed_titles = make_array(
    "Unlimited", "10% [6 FPS (NTSC) / 5 FPS (PAL)]", "20% [12 FPS (NTSC) / 10 FPS (PAL)]",
    "30% [18 FPS (NTSC) / 15 FPS (PAL)]", "40% [24 FPS (NTSC) / 20 FPS (PAL)]", "50% [30 FPS (NTSC) / 25 FPS (PAL)]",
    "60% [36 FPS (NTSC) / 30 FPS (PAL)]", "70% [42 FPS (NTSC) / 35 FPS (PAL)]", "80% [48 FPS (NTSC) / 40 FPS (PAL)]",
    "90% [54 FPS (NTSC) / 45 FPS (PAL)]", "100% [60 FPS (NTSC) / 50 FPS (PAL)]", "125% [75 FPS (NTSC) / 62 FPS (PAL)]",
    "150% [90 FPS (NTSC) / 75 FPS (PAL)]", "175% [105 FPS (NTSC) / 87 FPS (PAL)]",
    "200% [120 FPS (NTSC) / 100 FPS (PAL)]", "250% [150 FPS (NTSC) / 125 FPS (PAL)]",
    "300% [180 FPS (NTSC) / 150 FPS (PAL)]", "350% [210 FPS (NTSC) / 175 FPS (PAL)]",
    "400% [240 FPS (NTSC) / 200 FPS (PAL)]", "450% [270 FPS (NTSC) / 225 FPS (PAL)]",
    "500% [300 FPS (NTSC) / 250 FPS (PAL)]", "600% [360 FPS (NTSC) / 300 FPS (PAL)]",
    "700% [420 FPS (NTSC) / 350 FPS (PAL)]", "800% [480 FPS (NTSC) / 400 FPS (PAL)]",
    "900% [540 FPS (NTSC) / 450 FPS (PAL)]", "1000% [600 FPS (NTSC) / 500 FPS (PAL)]");

  BeginMenuButtons();

  MenuHeading("Speed Control");
  DrawFloatListSetting(
    "Emulation Speed",
    "Sets the target emulation speed. It is not guaranteed that this speed will be reached on all systems.", "Main",
    "EmulationSpeed", 1.0f, emulation_speed_titles.data(), emulation_speed_values.data(),
    emulation_speed_titles.size());
  DrawFloatListSetting(
    "Fast Forward Speed",
    "Sets the fast forward speed. It is not guaranteed that this speed will be reached on all systems.", "Main",
    "FastForwardSpeed", 0.0f, emulation_speed_titles.data(), emulation_speed_values.data(),
    emulation_speed_titles.size());
  DrawFloatListSetting(
    "Turbo Speed", "Sets the turbo speed. It is not guaranteed that this speed will be reached on all systems.", "Main",
    "TurboSpeed", 2.0f, emulation_speed_titles.data(), emulation_speed_values.data(), emulation_speed_titles.size());

  MenuHeading("Runahead/Rewind");

  DrawToggleSetting("Enable Rewinding", "Saves state periodically so you can rewind any mistakes while playing.",
                    "Main", "RewindEnable", false);
  DrawFloatRangeSetting(
    "Rewind Save Frequency",
    "How often a rewind state will be created. Higher frequencies have greater system requirements.", "Main",
    "RewindFrequency", 10.0f, 0.0f, 3600.0f, "%.2f Seconds");
  DrawIntRangeSetting("Rewind Save Slots",
                      "How many saves will be kept for rewinding. Higher values have greater memory requirements.",
                      "Main", "RewindSaveSlots", 10, 1, 10000, "%d Frames");

  const s32 runahead_frames =
    IsEditingGameSettings() ?
      GetEditingSettingsInterface()->GetIntValue("Main", "RunaheadFrameCount", g_settings.runahead_frames) :
      g_settings.runahead_frames;
  const bool runahead_enabled = (runahead_frames > 0);
  const bool rewind_enabled =
    IsEditingGameSettings() ?
      GetEditingSettingsInterface()->GetBoolValue("Main", "RewindEnable", g_settings.rewind_enable) :
      g_settings.rewind_enable;

  static constexpr auto runahead_options =
    make_array("Disabled", "1 Frame", "2 Frames", "3 Frames", "4 Frames", "5 Frames", "6 Frames", "7 Frames",
               "8 Frames", "9 Frames", "10 Frames");

  DrawIntListSetting(
    "Runahead",
    "Simulates the system ahead of time and rolls back/replays to reduce input lag. Very high system requirements.",
    "Main", "RunaheadFrameCount", 0, runahead_options.data(), runahead_options.size());

  TinyString rewind_summary;
  if (runahead_enabled)
  {
    rewind_summary = "Rewind is disabled because runahead is enabled. Runahead will significantly increase "
                     "system requirements.";
  }
  else if (rewind_enabled)
  {
    const float rewind_frequency =
      IsEditingGameSettings() ?
        GetEditingSettingsInterface()->GetFloatValue("Main", "RewindFrequency", g_settings.rewind_save_frequency) :
        g_settings.rewind_save_frequency;
    const s32 rewind_save_slots =
      IsEditingGameSettings() ?
        GetEditingSettingsInterface()->GetIntValue("Main", "RunaheadFrameCount", g_settings.rewind_save_slots) :
        g_settings.rewind_save_slots;
    const float duration =
      ((rewind_frequency <= std::numeric_limits<float>::epsilon()) ? (1.0f / 60.0f) : rewind_frequency) *
      static_cast<float>(rewind_save_slots);

    u64 ram_usage, vram_usage;
    System::CalculateRewindMemoryUsage(rewind_save_slots, &ram_usage, &vram_usage);
    rewind_summary.Format("Rewind for %u frames, lasting %.2f seconds will require up to %" PRIu64
                          "MB of RAM and %" PRIu64 "MB of VRAM.",
                          rewind_save_slots, duration, ram_usage / 1048576, vram_usage / 1048576);
  }
  else
  {
    rewind_summary =
      "Rewind is not enabled. Please note that enabling rewind may significantly increase system requirements.";
  }

  ActiveButton(rewind_summary, false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font);

  EndMenuButtons();
}

void FullscreenUI::DrawControllerSettingsPage()
{
  BeginMenuButtons();

  SettingsInterface* bsi = GetEditingSettingsInterface();

  MenuHeading("Input Sources");

#ifdef WITH_SDL2
  DrawToggleSetting(ICON_FA_COG "  Enable SDL Input Source", "The SDL input source supports most controllers.",
                    "InputSources", "SDL", true);
  DrawToggleSetting(ICON_FA_WIFI "  SDL DualShock 4 / DualSense Enhanced Mode",
                    "Provides vibration and LED control support over Bluetooth.", "InputSources",
                    "SDLControllerEnhancedMode", false, bsi->GetBoolValue("InputSources", "SDL"));
#endif
#ifdef _WIN32
  DrawToggleSetting(ICON_FA_COG "  Enable XInput Input Source",
                    "The XInput source provides support for XBox 360/XBox One/XBox Series controllers.", "InputSources",
                    "XInput", false);
#endif

  MenuHeading("Multitap");
  DrawEnumSetting(ICON_FA_PLUS_SQUARE "  Multitap Mode",
                  "Enables an additional three controller slots on each port. Not supported in all games.",
                  "ControllerPorts", "MultitapMode", Settings::DEFAULT_MULTITAP_MODE, &Settings::ParseMultitapModeName,
                  &Settings::GetMultitapModeName, &Settings::GetMultitapModeDisplayName, MultitapMode::Count);

  // load mtap settings
  MultitapMode mtap_mode = g_settings.multitap_mode;
  if (IsEditingGameSettings())
  {
    mtap_mode = Settings::ParseMultitapModeName(bsi->GetStringValue("ControllerPorts", "MultitapMode", "").c_str())
                  .value_or(g_settings.multitap_mode);
  }
  const std::array<bool, 2> mtap_enabled = {
    {(mtap_mode == MultitapMode::Port1Only || mtap_mode == MultitapMode::BothPorts),
     (mtap_mode == MultitapMode::Port2Only || mtap_mode == MultitapMode::BothPorts)}};

  // we reorder things a little to make it look less silly for mtap
  static constexpr const std::array<char, 4> mtap_slot_names = {{'A', 'B', 'C', 'D'}};
  static constexpr const std::array<u32, NUM_CONTROLLER_AND_CARD_PORTS> mtap_port_order = {{0, 2, 3, 4, 1, 5, 6, 7}};

  // create the ports
  for (u32 global_slot : mtap_port_order)
  {
    const auto [mtap_port, mtap_slot] = Controller::ConvertPadToPortAndSlot(global_slot);
    const bool is_mtap_port = Controller::PortAndSlotIsMultitap(mtap_port, mtap_slot);
    if (is_mtap_port && !mtap_enabled[mtap_port])
      continue;

    MenuHeading((mtap_enabled[mtap_port] ?
                   fmt::format(ICON_FA_PLUG "  Controller Port {}{}", mtap_port + 1, mtap_slot_names[mtap_slot]) :
                   fmt::format(ICON_FA_PLUG "  Controller Port {}", mtap_port + 1))
                  .c_str());

    const std::string section(fmt::format("Pad{}", global_slot + 1));
    const std::string type(bsi->GetStringValue(section.c_str(), "Type", Controller::GetDefaultPadType(global_slot)));
    const Controller::ControllerInfo* ci = Controller::GetControllerInfo(type);
    if (MenuButton(fmt::format(ICON_FA_GAMEPAD "  Controller Type##type{}", global_slot).c_str(),
                   ci ? ci->display_name : "Unknown"))
    {
      std::vector<std::pair<std::string, std::string>> raw_options(Controller::GetControllerTypeNames());
      ImGuiFullscreen::ChoiceDialogOptions options;
      options.reserve(raw_options.size());
      for (auto& it : raw_options)
      {
        options.emplace_back(std::move(it.second), type == it.first);
      }
      OpenChoiceDialog(
        fmt::format("Port {} Controller Type", global_slot + 1).c_str(), false, std::move(options),
        [section, raw_options = std::move(raw_options)](s32 index, const std::string& title, bool checked) {
          if (index < 0)
            return;

          auto lock = Host::GetSettingsLock();
          SettingsInterface* bsi = GetEditingSettingsInterface();
          bsi->SetStringValue(section.c_str(), "Type", raw_options[index].first.c_str());
          SetSettingsChanged();
          CloseChoiceDialog();
        });
    }

    if (!ci || ci->num_bindings == 0)
      continue;

    if (MenuButton(ICON_FA_MAGIC "  Automatic Mapping", "Attempts to map the selected port to a chosen controller."))
      StartAutomaticBinding(global_slot);

    for (u32 i = 0; i < ci->num_bindings; i++)
    {
      const Controller::ControllerBindingInfo& bi = ci->bindings[i];
      DrawInputBindingButton(bi.type, section.c_str(), bi.name, bi.display_name, true);
    }

    MenuHeading((mtap_enabled[mtap_port] ? fmt::format(ICON_FA_MICROCHIP "  Controller Port {}{} Macros", mtap_port + 1,
                                                       mtap_slot_names[mtap_slot]) :
                                           fmt::format(ICON_FA_MICROCHIP "  Controller Port {} Macros", mtap_port + 1))
                  .c_str());

    for (u32 macro_index = 0; macro_index < InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER; macro_index++)
    {
      DrawInputBindingButton(Controller::ControllerBindingType::Macro, section.c_str(),
                             fmt::format("Macro{}", macro_index + 1).c_str(),
                             fmt::format("Macro {} Trigger", macro_index + 1).c_str());

      std::string binds_string(
        bsi->GetStringValue(section.c_str(), fmt::format("Macro{}Binds", macro_index + 1).c_str()));
      if (MenuButton(fmt::format(ICON_FA_KEYBOARD "  Macro {} Buttons", macro_index + 1).c_str(),
                     binds_string.empty() ? "No Buttons Selected" : binds_string.c_str()))
      {
        std::vector<std::string_view> buttons_split(StringUtil::SplitString(binds_string, '&', true));
        ImGuiFullscreen::ChoiceDialogOptions options;
        for (u32 i = 0; i < ci->num_bindings; i++)
        {
          const Controller::ControllerBindingInfo& bi = ci->bindings[i];
          if (bi.type != Controller::ControllerBindingType::Button &&
              bi.type != Controller::ControllerBindingType::Axis &&
              bi.type != Controller::ControllerBindingType::HalfAxis)
          {
            continue;
          }
          options.emplace_back(bi.display_name,
                               std::any_of(buttons_split.begin(), buttons_split.end(),
                                           [bi](const std::string_view& it) { return (it == bi.name); }));
        }

        OpenChoiceDialog(fmt::format("Select Macro {} Binds", macro_index + 1).c_str(), true, std::move(options),
                         [section, macro_index, ci](s32 index, const std::string& title, bool checked) {
                           // convert display name back to bind name
                           std::string_view to_modify;
                           for (u32 j = 0; j < ci->num_bindings; j++)
                           {
                             const Controller::ControllerBindingInfo& bi = ci->bindings[j];
                             if (bi.display_name == title)
                             {
                               to_modify = bi.name;
                               break;
                             }
                           }
                           if (to_modify.empty())
                           {
                             // wtf?
                             return;
                           }

                           auto lock = Host::GetSettingsLock();
                           SettingsInterface* bsi = GetEditingSettingsInterface();
                           const std::string key(fmt::format("Macro{}Binds", macro_index + 1));

                           std::string binds_string(bsi->GetStringValue(section.c_str(), key.c_str()));
                           std::vector<std::string_view> buttons_split(
                             StringUtil::SplitString(binds_string, '&', true));
                           auto it = std::find(buttons_split.begin(), buttons_split.end(), to_modify);
                           if (checked)
                           {
                             if (it == buttons_split.end())
                               buttons_split.push_back(to_modify);
                           }
                           else
                           {
                             if (it != buttons_split.end())
                               buttons_split.erase(it);
                           }

                           binds_string = StringUtil::JoinString(buttons_split.begin(), buttons_split.end(), " & ");
                           if (binds_string.empty())
                             bsi->DeleteValue(section.c_str(), key.c_str());
                           else
                             bsi->SetStringValue(section.c_str(), key.c_str(), binds_string.c_str());
                         });
      }

      const std::string freq_key(fmt::format("Macro{}Frequency", macro_index + 1));
      const std::string freq_title(fmt::format(ICON_FA_LIGHTBULB "  Macro {} Frequency", macro_index + 1));
      s32 frequency = bsi->GetIntValue(section.c_str(), freq_key.c_str(), 0);
      const std::string freq_summary((frequency == 0) ? std::string("Macro will not auto-toggle.") :
                                                        fmt::format("Macro will toggle every {} frames.", frequency));
      if (MenuButton(freq_title.c_str(), freq_summary.c_str()))
        ImGui::OpenPopup(freq_title.c_str());

      ImGui::SetNextWindowSize(LayoutScale(500.0f, 180.0f));

      ImGui::PushFont(g_large_font);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                                  ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

      if (ImGui::BeginPopupModal(freq_title.c_str(), nullptr,
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
      {
        ImGui::SetNextItemWidth(LayoutScale(450.0f));
        if (ImGui::SliderInt("##value", &frequency, 0, 60, "Toggle every %d frames", ImGuiSliderFlags_NoInput))
        {
          if (frequency == 0)
            bsi->DeleteValue(section.c_str(), freq_key.c_str());
          else
            bsi->SetIntValue(section.c_str(), freq_key.c_str(), frequency);
        }

        BeginMenuButtons();
        if (MenuButton("OK", nullptr, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
          ImGui::CloseCurrentPopup();
        EndMenuButtons();

        ImGui::EndPopup();
      }

      ImGui::PopStyleVar(3);
      ImGui::PopFont();
    }

    MenuHeading(
      (mtap_enabled[mtap_port] ?
         fmt::format(ICON_FA_SLIDERS_H "  Controller Port {}{} Settings", mtap_port + 1, mtap_slot_names[mtap_slot]) :
         fmt::format(ICON_FA_SLIDERS_H "  Controller Port {} Settings", mtap_port + 1))
        .c_str());

    DrawFloatRangeSetting(ICON_FA_BULLSEYE "  Analog Deadzone",
                          "Defines the percentage of the stick which is ignored for the physical controller.",
                          section.c_str(), "AnalogDeadzone", Controller::DEFAULT_STICK_DEADZONE, 0.0f, 1.0f, "%.2f");
    DrawFloatRangeSetting(ICON_FA_BULLSEYE "  Analog Sensitivity",
                          "Scales input values when the stick range differs to the virtual controller.",
                          section.c_str(), "AnalogSensitivity", Controller::DEFAULT_STICK_SENSITIVITY, 0.1f, 3.0f,
                          "%.2f");

    if (ci->vibration_caps == Controller::VibrationCapabilities::LargeSmallMotors)
    {
      DrawFloatRangeSetting(ICON_FA_BELL "  Large Motor Scale", "Adjusts intensity of low-frequency vibration.",
                            section.c_str(), "LargeMotorScale", 1.0f, 0.0f, 3.0f, "%.2f");
      DrawFloatRangeSetting(ICON_FA_BELL "  Small Motor Scale", "Adjusts intensity of high-frequency vibration.",
                            section.c_str(), "SmallMotorScale", 1.0f, 0.0f, 3.0f, "%.2f");
    }
  }

  EndMenuButtons();
}

void FullscreenUI::DrawHotkeySettingsPage()
{
  BeginMenuButtons();

  InputManager::GetHotkeyList();

  const HotkeyInfo* last_category = nullptr;
  for (const HotkeyInfo* hotkey : s_hotkey_list_cache)
  {
    if (!last_category || hotkey->category != last_category->category)
    {
      MenuHeading(hotkey->category);
      last_category = hotkey;
    }

    DrawInputBindingButton(Controller::ControllerBindingType::Button, "Hotkeys", hotkey->name, hotkey->display_name,
                           false);
  }

  EndMenuButtons();
}

void FullscreenUI::DrawMemoryCardSettingsPage()
{
  BeginMenuButtons();

#if 0

  for (u32 i = 0; i < 2; i++)
  {
    MenuHeading(TinyString::FromFormat("Memory Card Port %u", i + 1));

    settings_changed |= EnumChoiceButton(
      TinyString::FromFormat("Memory Card %u Type", i + 1),
      SmallString::FromFormat("Sets which sort of memory card image will be used for slot %u.", i + 1),
      &s_settings_copy.memory_card_types[i], &Settings::GetMemoryCardTypeDisplayName, MemoryCardType::Count);

    settings_changed |= MenuButton(TinyString::FromFormat("Shared Memory Card %u Path", i + 1),
                                   s_settings_copy.memory_card_paths[i].c_str(),
                                   s_settings_copy.memory_card_types[i] == MemoryCardType::Shared);
  }

  MenuHeading("Shared Settings");

  settings_changed |= ToggleButton("Use Single Card For Sub-Images",
                                   "When using a multi-disc image (m3u/pbp) and per-game (title) memory cards, "
                                   "use a single memory card for all discs.",
                                   &s_settings_copy.memory_card_use_playlist_title);

  static std::string memory_card_directory;
  static bool memory_card_directory_set = false;
  if (!memory_card_directory_set)
  {
    memory_card_directory = s_host_interface->GetMemoryCardDirectory();
    memory_card_directory_set = true;
  }

  if (MenuButton("Memory Card Directory", memory_card_directory.c_str()))
  {
    OpenFileSelector("Memory Card Directory", true, [](const std::string& path) {
      if (!path.empty())
      {
        memory_card_directory = path;
        s_settings_copy.memory_card_directory = path;
        s_host_interface->RunLater(SaveAndApplySettings);
      }
      CloseFileSelector();
    });
  }

  if (MenuButton("Reset Memory Card Directory", "Resets memory card directory to default (user directory)."))
  {
    s_settings_copy.memory_card_directory.clear();
    s_host_interface->RunLater(SaveAndApplySettings);
    memory_card_directory_set = false;
  }
#endif

  EndMenuButtons();
}

void FullscreenUI::DrawDisplaySettingsPage()
{
  BeginMenuButtons();

  MenuHeading("Device Settings");

  DrawEnumSetting("GPU Renderer", "Chooses the backend to use for rendering the console/game visuals.", "GPU",
                  "Renderer", Settings::DEFAULT_GPU_RENDERER, &Settings::ParseRendererName, &Settings::GetRendererName,
                  &Settings::GetRendererDisplayName, GPURenderer::Count);

#if 0
  static std::string fullscreen_mode;
  static bool fullscreen_mode_set;
  if (!fullscreen_mode_set)
  {
    fullscreen_mode = s_host_interface->GetSettingsInterface()->GetStringValue("GPU", "FullscreenMode", "");
    fullscreen_mode_set = true;
  }

#ifndef _UWP
  if (MenuButtonWithValue("Fullscreen Resolution", "Selects the resolution to use in fullscreen modes.",
                          fullscreen_mode.empty() ? "Borderless Fullscreen" : fullscreen_mode.c_str()))
  {
    HostDisplay::AdapterAndModeList aml(s_host_interface->GetDisplay()->GetAdapterAndModeList());

    ImGuiFullscreen::ChoiceDialogOptions options;
    options.reserve(aml.fullscreen_modes.size() + 1);
    options.emplace_back("Borderless Fullscreen", fullscreen_mode.empty());
    for (std::string& mode : aml.fullscreen_modes)
      options.emplace_back(std::move(mode), mode == fullscreen_mode);

    auto callback = [](s32 index, const std::string& title, bool checked) {
      if (index < 0)
        return;
      else if (index == 0)
        std::string().swap(fullscreen_mode);
      else
        fullscreen_mode = title;

      s_host_interface->GetSettingsInterface()->SetStringValue("GPU", "FullscreenMode", fullscreen_mode.c_str());
      s_host_interface->GetSettingsInterface()->Save();
      s_host_interface->AddOSDMessage("Resolution change will be applied after restarting.", 10.0f);
      CloseChoiceDialog();
    };
    OpenChoiceDialog(ICON_FA_TV "  Fullscreen Resolution", false, std::move(options), std::move(callback));
  }
#endif
#endif

  const GPURenderer renderer =
    IsEditingGameSettings() ?
      Settings::ParseRendererName(
        GetEditingSettingsInterface()
          ->GetStringValue("GPU", "Renderer", Settings::GetRendererName(g_settings.gpu_renderer))
          .c_str())
        .value_or(g_settings.gpu_renderer) :
      g_settings.gpu_renderer;

  switch (renderer)
  {
#ifdef _WIN32
    case GPURenderer::HardwareD3D11:
    {
      DrawToggleSetting("Use Blit Swap Chain",
                        "Uses a blit presentation model instead of flipping. This may be needed on some systems.",
                        "Display", "UseBlitSwapChain", false);
    }
    break;
#endif

    case GPURenderer::HardwareVulkan:
    {
      DrawToggleSetting("Threaded Presentation",
                        "Presents frames on a background thread when fast forwarding or vsync is disabled.", "GPU",
                        "ThreadedPresentation", true);
    }
    break;

    case GPURenderer::Software:
    {
      DrawToggleSetting("Threaded Rendering",
                        "Uses a second thread for drawing graphics. Speed boost, and safe to use.", "GPU", "UseThread",
                        true);
    }
    break;

    default:
      break;
  }

  if (renderer != GPURenderer::Software)
  {
    DrawToggleSetting("Use Software Renderer For Readbacks",
                      "Runs the software renderer in parallel for VRAM readbacks. On some systems, this may result "
                      "in greater performance.",
                      "GPU", "UseSoftwareRendererForReadbacks", false);
  }

  DrawToggleSetting("Enable VSync",
                    "Synchronizes presentation of the console's frames to the host. Enable for smoother animations.",
                    "Display", "VSync", Settings::DEFAULT_VSYNC_VALUE);

  DrawToggleSetting("Sync To Host Refresh Rate",
                    "Adjusts the emulation speed so the console's refresh rate matches the host when VSync and Audio "
                    "Resampling are enabled.",
                    "Main", "SyncToHostRefreshRate", false);

  DrawToggleSetting(
    "Optimal Frame Pacing",
    "Ensures every frame generated is displayed for optimal pacing. Disable if you are having speed or sound issues.",
    "Display", "DisplayAllFrames", false);

  MenuHeading("Screen Display");

  DrawEnumSetting("Aspect Ratio", "Changes the aspect ratio used to display the console's output to the screen.",
                  "Display", "AspectRatio", Settings::DEFAULT_DISPLAY_ASPECT_RATIO, &Settings::ParseDisplayAspectRatio,
                  &Settings::GetDisplayAspectRatioName, &Settings::GetDisplayAspectRatioName,
                  DisplayAspectRatio::Count);

  DrawEnumSetting("Crop Mode",
                  "Determines how much of the area typically not visible on a consumer TV set to crop/hide.", "Display",
                  "CropMode", Settings::DEFAULT_DISPLAY_CROP_MODE, &Settings::ParseDisplayCropMode,
                  &Settings::GetDisplayCropModeName, &Settings::GetDisplayCropModeDisplayName, DisplayCropMode::Count);

  DrawEnumSetting("Downsampling",
                  "Downsamples the rendered image prior to displaying it. Can improve "
                  "overall image quality in mixed 2D/3D games.",
                  "GPU", "DownsampleMode", Settings::DEFAULT_GPU_DOWNSAMPLE_MODE, &Settings::ParseDownsampleModeName,
                  &Settings::GetDownsampleModeName, &Settings::GetDownsampleModeDisplayName, GPUDownsampleMode::Count,
                  (renderer != GPURenderer::Software));

  DrawToggleSetting("Linear Upscaling", "Uses a bilinear filter when upscaling to display, smoothing out the image.",
                    "Display", "LinearFiltering", true);

  DrawToggleSetting("Integer Upscaling", "Adds padding to ensure pixels are a whole number in size.", "Display",
                    "IntegerScaling", false);

  DrawToggleSetting("Stretch To Fit", "Fills the window with the active display area, regardless of the aspect ratio.",
                    "Display", "Stretch", false);

  DrawToggleSetting("Internal Resolution Screenshots",
                    "Saves screenshots at internal render resolution and without postprocessing.", "Display",
                    "InternalResolutionScreenshots", false);

  MenuHeading("On-Screen Display");

  DrawToggleSetting("Show OSD Messages", "Shows on-screen-display messages when events occur.", "Display",
                    "ShowOSDMessages", true);
  DrawToggleSetting("Show Game Frame Rate",
                    "Shows the internal frame rate of the game in the top-right corner of the display.", "Display",
                    "ShowFPS", false);
  DrawToggleSetting("Show Display FPS",
                    "Shows the number of frames (or v-syncs) displayed per second by the system "
                    "in the top-right corner of the display.",
                    "Display", "ShowVPS", false);
  DrawToggleSetting(
    "Show Speed",
    "Shows the current emulation speed of the system in the top-right corner of the display as a percentage.",
    "Display", "ShowSpeed", false);
  DrawToggleSetting("Show Resolution",
                    "Shows the current rendering resolution of the system in the top-right corner of the display.",
                    "Display", "ShowResolution", false);
  DrawToggleSetting("Show Controller Input",
                    "Shows the current controller state of the system in the bottom-left corner of the display.",
                    "Display", "ShowInputs", false);

  EndMenuButtons();
}

void FullscreenUI::DrawEnhancementSettingsPage()
{
  static constexpr auto resolution_scales =
    make_array("Automatic based on window size", "1x", "2x", "3x (for 720p)", "4x", "5x (for 1080p)", "6x (for 1440p)",
               "7x", "8x", "9x (for 4K)", "10x", "11x", "12x", "13x", "14x", "15x", "16x");

  BeginMenuButtons();

  MenuHeading("Rendering Enhancements");

  DrawIntListSetting(
    "Internal Resolution Scale",
    "Scales internal VRAM resolution by the specified multiplier. Some games require 1x VRAM resolution.", "GPU",
    "ResolutionScale", 1, resolution_scales.data(), resolution_scales.size());

  DrawEnumSetting("Texture Filtering",
                  "Smooths out the blockyness of magnified textures on 3D objects. Will have a greater effect "
                  "on higher resolution scales.",
                  "GPU", "TextureFilter", Settings::DEFAULT_GPU_TEXTURE_FILTER, &Settings::ParseTextureFilterName,
                  &Settings::GetTextureFilterName, &Settings::GetTextureFilterDisplayName, GPUTextureFilter::Count);

  DrawToggleSetting("True Color Rendering",
                    "Disables dithering and uses the full 8 bits per channel of color information. May break "
                    "rendering in some games.",
                    "GPU", "TrueColor", true);

  DrawToggleSetting("Scaled Dithering",
                    "Scales the dithering pattern with the internal rendering resolution, making it less noticeable. "
                    "Usually safe to enable.",
                    "GPU", "ScaledDithering", true);
  DrawToggleSetting("Widescreen Hack",
                    "Increases the field of view from 4:3 to the chosen display aspect ratio in 3D games.", "GPU",
                    "WidescreenHack", false);

  MenuHeading("Display Enhancements");

  DrawToggleSetting("Disable Interlacing",
                    "Disables interlaced rendering and display in the GPU. Some games can render in 480p this way, "
                    "but others will break.",
                    "GPU", "DisableInterlacing", true);
  DrawToggleSetting("Force NTSC Timings",
                    "Forces PAL games to run at NTSC timings, i.e. 60hz. Some PAL games will run at their \"normal\" "
                    "speeds, while others will break.",
                    "GPU", "ForceNTSCTimings", false);
  DrawToggleSetting("Force 4:3 For 24-Bit Display",
                    "Switches back to 4:3 display aspect ratio when displaying 24-bit content, usually FMVs.",
                    "Display", "Force4_3For24Bit", false);
  DrawToggleSetting("Chroma Smoothing For 24-Bit Display",
                    "Smooths out blockyness between colour transitions in 24-bit content, usually FMVs. Only applies "
                    "to the hardware renderers.",
                    "GPU", "ChromaSmoothing24Bit", false);

  MenuHeading("PGXP (Precision Geometry Transform Pipeline");

  const bool pgxp_enabled =
    IsEditingGameSettings() ?
      GetEditingSettingsInterface()->GetBoolValue("GPU", "PGXPEnable", g_settings.gpu_pgxp_enable) :
      g_settings.gpu_pgxp_enable;
  const bool texture_correction_enabled =
    IsEditingGameSettings() ? GetEditingSettingsInterface()->GetBoolValue("GPU", "PGXPTextureCorrection",
                                                                          g_settings.gpu_pgxp_texture_correction) :
                              g_settings.gpu_pgxp_texture_correction;
  DrawToggleSetting("PGXP Geometry Correction",
                    "Reduces \"wobbly\" polygons by attempting to preserve the fractional component through memory "
                    "transfers.",
                    "GPU", "PGXPEnable", false);
  DrawToggleSetting("PGXP Texture Correction",
                    "Uses perspective-correct interpolation for texture coordinates and colors, straightening out "
                    "warped textures.",
                    "GPU", "PGXPTextureCorrection", true, pgxp_enabled);
  DrawToggleSetting("PGXP Culling Correction",
                    "Increases the precision of polygon culling, reducing the number of holes in geometry.", "GPU",
                    "PGXPCulling", true, pgxp_enabled);
  DrawToggleSetting("PGXP Preserve Projection Precision",
                    "Adds additional precision to PGXP data post-projection. May improve visuals in some games.", "GPU",
                    "PGXPPreserveProjFP", false, pgxp_enabled);
  DrawToggleSetting("PGXP Depth Buffer",
                    "Reduces polygon Z-fighting through depth testing. Low compatibility with games.", "GPU",
                    "PGXPDepthBuffer", false, pgxp_enabled && texture_correction_enabled);
  DrawToggleSetting("PGXP CPU Mode", "Uses PGXP for all instructions, not just memory operations.", "GPU", "PGXPCPU",
                    false, pgxp_enabled);

  EndMenuButtons();
}

void FullscreenUI::DrawAudioSettingsPage()
{
  BeginMenuButtons();

  MenuHeading("Audio Control");

  DrawIntRangeSetting("Output Volume", "Controls the volume of the audio played on the host.", "Audio", "OutputVolume",
                      100, 0, 100, "%d%%");
  DrawIntRangeSetting("Fast Forward Volume",
                      "Controls the volume of the audio played on the host when fast forwarding.", "Audio",
                      "FastForwardVolume", 100, 0, 100, "%d%%");
  DrawToggleSetting("Mute All Sound", "Prevents the emulator from producing any audible sound.", "Audio", "OutputMuted",
                    false);
  DrawToggleSetting("Mute CD Audio",
                    "Forcibly mutes both CD-DA and XA audio from the CD-ROM. Can be used to "
                    "disable background music in some games.",
                    "CDROM", "MuteCDAudio", false);

  MenuHeading("Backend Settings");

  DrawEnumSetting("Audio Backend",
                  "The audio backend determines how frames produced by the emulator are submitted to the host.",
                  "Audio", "Backend", Settings::DEFAULT_AUDIO_BACKEND, &Settings::ParseAudioBackend,
                  &Settings::GetAudioBackendName, &Settings::GetAudioBackendDisplayName, AudioBackend::Count);
  DrawIntRangeSetting("Buffer Size",
                      "The buffer size determines the size of the chunks of audio which will be pulled by the host.",
                      "Audio", "BufferSize", Settings::DEFAULT_AUDIO_BUFFER_SIZE, 1024, 8192, "%d Frames");

  DrawToggleSetting("Sync To Output",
                    "Throttles the emulation speed based on the audio backend pulling audio "
                    "frames. Enable to reduce the chances of crackling.",
                    "Audio", "Sync", true);
  DrawToggleSetting(
    "Resampling",
    "When running outside of 100% speed, resamples audio from the target speed instead of dropping frames.", "Audio",
    "Resampling", true);

  EndMenuButtons();
}

void FullscreenUI::DrawAchievementsSettingsPage()
{
#if 0
#ifdef WITH_RAINTEGRATION
  if (Cheevos::IsUsingRAIntegration())
  {
    BeginMenuButtons();
    ActiveButton(ICON_FA_BAN "  RAIntegration is being used instead of the built-in cheevos implementation.", false,
                 false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    EndMenuButtons();
    break;
  }
#endif

#ifdef WITH_CHEEVOS
  BeginMenuButtons();

  MenuHeading("Settings");
  if (ToggleButtonForNonSetting(ICON_FA_TROPHY "  Enable RetroAchievements",
                                "When enabled and logged in, DuckStation will scan for achievements on startup.",
                                "Cheevos", "Enabled", false))
  {
    settings_changed = true;
    s_host_interface->RunLater([]() {
      if (!ConfirmChallengeModeEnable())
        s_host_interface->GetSettingsInterface()->SetBoolValue("Cheevos", "Enabled", false);
    });
  }

  settings_changed |= ToggleButtonForNonSetting(
    ICON_FA_USER_FRIENDS "  Rich Presence",
    "When enabled, rich presence information will be collected and sent to the server where supported.", "Cheevos",
    "RichPresence", true);
  settings_changed |=
    ToggleButtonForNonSetting(ICON_FA_STETHOSCOPE "  Test Mode",
                              "When enabled, DuckStation will assume all achievements are locked and not "
                              "send any unlock notifications to the server.",
                              "Cheevos", "TestMode", false);
  settings_changed |=
    ToggleButtonForNonSetting(ICON_FA_MEDAL "  Test Unofficial Achievements",
                              "When enabled, DuckStation will list achievements from unofficial sets. These "
                              "achievements are not tracked by RetroAchievements.",
                              "Cheevos", "UnofficialTestMode", false);
  settings_changed |= ToggleButtonForNonSetting(ICON_FA_COMPACT_DISC "  Use First Disc From Playlist",
                                                "When enabled, the first disc in a playlist will be used for "
                                                "achievements, regardless of which disc is active.",
                                                "Cheevos", "UseFirstDiscFromPlaylist", true);

  if (ToggleButtonForNonSetting(ICON_FA_HARD_HAT "  Hardcore Mode",
                                "\"Challenge\" mode for achievements. Disables save state, cheats, and slowdown "
                                "functions, but you receive double the achievement points.",
                                "Cheevos", "ChallengeMode", false))
  {
    s_host_interface->RunLater([]() {
      if (!ConfirmChallengeModeEnable())
        s_host_interface->GetSettingsInterface()->SetBoolValue("Cheevos", "ChallengeMode", false);
    });
  }

  MenuHeading("Account");
  if (Cheevos::IsLoggedIn())
  {
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::GetStyle().Colors[ImGuiCol_Text]);
    ActiveButton(SmallString::FromFormat(ICON_FA_USER "  Username: %s", Cheevos::GetUsername().c_str()), false, false,
                 ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    TinyString ts_string;
    ts_string.AppendFmtString(
      "{:%Y-%m-%d %H:%M:%S}",
      fmt::localtime(StringUtil::FromChars<u64>(
                       s_host_interface->GetSettingsInterface()->GetStringValue("Cheevos", "LoginTimestamp", "0"))
                       .value_or(0)));
    ActiveButton(SmallString::FromFormat(ICON_FA_CLOCK "  Login token generated on %s", ts_string.GetCharArray()),
                 false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    ImGui::PopStyleColor();

    if (MenuButton(ICON_FA_KEY "  Logout", "Logs out of RetroAchievements."))
      Cheevos::Logout();
  }
  else if (Cheevos::IsActive())
  {
    ActiveButton(ICON_FA_USER "  Not Logged In", false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    if (MenuButton(ICON_FA_KEY "  Login", "Logs in to RetroAchievements."))
      ImGui::OpenPopup("Achievements Login");

    DrawAchievementsLoginWindow();
  }
  else
  {
    ActiveButton(ICON_FA_USER "  Achievements are disabled.", false, false,
                 ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
  }

  MenuHeading("Current Game");
  if (Cheevos::HasActiveGame())
  {
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::GetStyle().Colors[ImGuiCol_Text]);
    ActiveButton(TinyString::FromFormat(ICON_FA_BOOKMARK "  Game ID: %u", Cheevos::GetGameID()), false, false,
                 ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    ActiveButton(TinyString::FromFormat(ICON_FA_BOOK "  Game Title: %s", Cheevos::GetGameTitle().c_str()), false, false,
                 ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    ActiveButton(TinyString::FromFormat(ICON_FA_DESKTOP "  Game Developer: %s", Cheevos::GetGameDeveloper().c_str()),
                 false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    ActiveButton(TinyString::FromFormat(ICON_FA_DESKTOP "  Game Publisher: %s", Cheevos::GetGamePublisher().c_str()),
                 false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    ActiveButton(TinyString::FromFormat(ICON_FA_TROPHY "  Achievements: %u (%u points)", Cheevos::GetAchievementCount(),
                                        Cheevos::GetMaximumPointsForGame()),
                 false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    const std::string& rich_presence_string = Cheevos::GetRichPresenceString();
    if (!rich_presence_string.empty())
    {
      ActiveButton(SmallString::FromFormat(ICON_FA_MAP "  %s", rich_presence_string.c_str()), false, false,
                   ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    }
    else
    {
      ActiveButton(ICON_FA_MAP "  Rich presence inactive or unsupported.", false, false,
                   ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    }

    ImGui::PopStyleColor();
  }
  else
  {
    ActiveButton(ICON_FA_BAN "  Game not loaded or no RetroAchievements available.", false, false,
                 ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
  }

  EndMenuButtons();
#else
  BeginMenuButtons();
  ActiveButton(ICON_FA_BAN "  This build was not compiled with RetroAchivements support.", false, false,
               ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
  EndMenuButtons();
#endif
  // ImGuiFullscreen::moda
  // if (ImGui::BeginPopup("))
#endif
}

void FullscreenUI::DrawAdvancedSettingsPage()
{
  BeginMenuButtons();

  MenuHeading("Logging Settings");
  DrawEnumSetting("Log Level", "Sets the verbosity of messages logged. Higher levels will log more messages.",
                  "Logging", "LogLevel", Settings::DEFAULT_LOG_LEVEL, &Settings::ParseLogLevelName,
                  &Settings::GetLogLevelName, &Settings::GetLogLevelDisplayName, LOGLEVEL_COUNT);
  DrawToggleSetting("Log To System Console", "Logs messages to the console window.", "Logging", "LogToConsole",
                    Settings::DEFAULT_LOG_TO_CONSOLE);
  DrawToggleSetting("Log To Debug Console", "Logs messages to the debug console where supported.", "Logging",
                    "LogToDebug", false);
  DrawToggleSetting("Log To File", "Logs messages to duckstation.log in the user directory.", "Logging", "LogToFile",
                    false);

  MenuHeading("Debugging Settings");

#if 0
  bool debug_menu = s_debug_menu_enabled;
  if (ToggleButton("Enable Debug Menu", "Shows a debug menu bar with additional statistics and quick settings.",
                   &debug_menu))
  {
    s_host_interface->RunLater([debug_menu]() { SetDebugMenuEnabled(debug_menu); });
  }
#endif

  DrawToggleSetting("Disable All Enhancements", "Temporarily disables all enhancements, useful when testing.", "Main",
                    "DisableAllEnhancements", false);

  DrawToggleSetting("Use Debug GPU Device",
                    "Enable debugging when supported by the host's renderer API. Only for developer use.", "GPU",
                    "UseDebugDevice", false);

#ifdef _WIN32
  DrawToggleSetting("Increase Timer Resolution", "Enables more precise frame pacing at the cost of battery life.",
                    "Main", "IncreaseTimerResolution", true);
#endif

  DrawToggleSetting("Allow Booting Without SBI File", "Allows loading protected games without subchannel information.",
                    "CDROM", "AllowBootingWithoutSBIFile", false);

  DrawToggleSetting("Create Save State Backups", "Renames existing save states when saving to a backup file.", "Main",
                    "CreateSaveStateBackups", false);

  MenuHeading("Display Settings");
  DrawToggleSetting("Show Status Indicators", "Shows persistent icons when turbo is active or when paused.", "Display",
                    "ShowStatusIndicators", true);
  DrawToggleSetting("Show Enhancement Settings", "Shows enhancement settings in the bottom-right corner of the screen.",
                    "Display", "ShowEnhancements", false);
  DrawFloatRangeSetting("Display FPS Limit",
                        "Limits how many frames are displayed to the screen. These frames are still rendered.",
                        "Display", "MaxFPS", Settings::DEFAULT_DISPLAY_MAX_FPS, 0.0f, 500.0f, "%.2f FPS");

  MenuHeading("PGXP Settings");

  const bool pgxp_enabled =
    IsEditingGameSettings() ?
      GetEditingSettingsInterface()->GetBoolValue("GPU", "PGXPEnable", g_settings.gpu_pgxp_enable) :
      g_settings.gpu_pgxp_enable;

  DrawToggleSetting("Enable PGXP Vertex Cache",
                    "Uses screen positions to resolve PGXP data. May improve visuals in some games.", "GPU",
                    "PGXPVertexCache", pgxp_enabled);
  DrawFloatRangeSetting(
    "PGXP Geometry Tolerance",
    "Sets a threshold for discarding precise values when exceeded. May help with glitches in some games.", "GPU",
    "PGXPTolerance", -1.0f, -1.0f, 10.0f, "%.1f", pgxp_enabled);
  DrawFloatRangeSetting(
    "PGXP Depth Clear Threshold", "Sets a threshold for discarding the emulated depth buffer. May help in some games.",
    "GPU", "PGXPDepthBuffer", Settings::DEFAULT_GPU_PGXP_DEPTH_THRESHOLD, 0.0f, 4096.0f, "%.1f", pgxp_enabled);

  MenuHeading("Texture Dumping/Replacements");

  DrawToggleSetting("Enable VRAM Write Texture Replacement",
                    "Enables the replacement of background textures in supported games.", "TextureReplacements",
                    "EnableVRAMWriteReplacements", false);
  DrawToggleSetting("Preload Replacement Textures",
                    "Loads all replacement texture to RAM, reducing stuttering at runtime.", "TextureReplacements",
                    "PreloadTextures", false);
  DrawToggleSetting("Dump Replaceable VRAM Writes", "Writes textures which can be replaced to the dump directory.",
                    "TextureReplacements", "DumpVRAMWrites", false);
  DrawToggleSetting("Set VRAM Write Dump Alpha Channel", "Clears the mask/transparency bit in VRAM write dumps.",
                    "TextureReplacements", "DumpVRAMWriteForceAlphaChannel", true);

  MenuHeading("CPU Emulation");

  DrawToggleSetting("Enable Recompiler ICache",
                    "Simulates the CPU's instruction cache in the recompiler. Can help with games running too fast.",
                    "CPU", "RecompilerICache", false);
  DrawToggleSetting("Enable Recompiler Memory Exceptions",
                    "Enables alignment and bus exceptions. Not needed for any known games.", "CPU",
                    "RecompilerMemoryExceptions", false);
  DrawToggleSetting("Enable Recompiler Block Linking",
                    "Performance enhancement - jumps directly between blocks instead of returning to the dispatcher.",
                    "CPU", "RecompilerBlockLinking", true);
  DrawEnumSetting("Recompiler Fast Memory Access",
                  "Avoids calls to C++ code, significantly speeding up the recompiler.", "CPU", "FastmemMode",
                  Settings::DEFAULT_CPU_FASTMEM_MODE, &Settings::ParseCPUFastmemMode, &Settings::GetCPUFastmemModeName,
                  &Settings::GetCPUFastmemModeDisplayName, CPUFastmemMode::Count);

  EndMenuButtons();
}

void FullscreenUI::DrawPauseMenu(MainWindowType type)
{
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImVec2 display_size(ImGui::GetIO().DisplaySize);
  dl->AddRectFilled(ImVec2(0.0f, 0.0f), display_size, IM_COL32(0x21, 0x21, 0x21, 200));

#if 0
  // title info
  {
    const ImVec2 title_size(g_large_font->CalcTextSizeA(g_large_font->FontSize, std::numeric_limits<float>::max(),
                                                        -1.0f, s_current_game_title.c_str()));
    const ImVec2 subtitle_size(g_medium_font->CalcTextSizeA(g_medium_font->FontSize, std::numeric_limits<float>::max(),
                                                            -1.0f, s_current_game_subtitle.c_str()));

    ImVec2 title_pos(display_size.x - LayoutScale(20.0f + 50.0f + 20.0f) - title_size.x,
                     display_size.y - LayoutScale(20.0f + 50.0f));
    ImVec2 subtitle_pos(display_size.x - LayoutScale(20.0f + 50.0f + 20.0f) - subtitle_size.x,
                        title_pos.y + g_large_font->FontSize + LayoutScale(4.0f));
    float rp_height = 0.0f;

    dl->AddText(g_large_font, g_large_font->FontSize, title_pos, IM_COL32(255, 255, 255, 255),
                s_current_game_title.c_str());
    dl->AddText(g_medium_font, g_medium_font->FontSize, subtitle_pos, IM_COL32(255, 255, 255, 255),
                s_current_game_subtitle.c_str());

    const ImVec2 image_min(display_size.x - LayoutScale(20.0f + 50.0f) - rp_height,
                           display_size.y - LayoutScale(20.0f + 50.0f) - rp_height);
    const ImVec2 image_max(image_min.x + LayoutScale(50.0f) + rp_height, image_min.y + LayoutScale(50.0f) + rp_height);
    dl->AddImage(GetCoverForCurrentGame()->GetHandle(), image_min, image_max);
  }
#endif

  // title info
  {
    const std::string& title = System::GetRunningTitle();
    const std::string& code = System::GetRunningCode();

    SmallString subtitle;
    if (!code.empty())
      subtitle.Format("%s - ", code.c_str());
    subtitle.AppendString(Path::GetFileTitle(System::GetRunningPath()));

    const ImVec2 title_size(
      g_large_font->CalcTextSizeA(g_large_font->FontSize, std::numeric_limits<float>::max(), -1.0f, title.c_str()));
    const ImVec2 subtitle_size(
      g_medium_font->CalcTextSizeA(g_medium_font->FontSize, std::numeric_limits<float>::max(), -1.0f, subtitle));

    ImVec2 title_pos(display_size.x - LayoutScale(20.0f + 50.0f + 20.0f) - title_size.x,
                     display_size.y - LayoutScale(20.0f + 50.0f));
    ImVec2 subtitle_pos(display_size.x - LayoutScale(20.0f + 50.0f + 20.0f) - subtitle_size.x,
                        title_pos.y + g_large_font->FontSize + LayoutScale(4.0f));
    float rp_height = 0.0f;

#ifdef WITH_CHEEVOS
    if (Achievements::IsActive())
    {
      const std::string& rp = Achievements::GetRichPresenceString();
      if (!rp.empty())
      {
        const float wrap_width = LayoutScale(350.0f);
        const ImVec2 rp_size = g_medium_font->CalcTextSizeA(g_medium_font->FontSize, std::numeric_limits<float>::max(),
                                                            wrap_width, rp.data(), rp.data() + rp.size());
        rp_height = rp_size.y + LayoutScale(4.0f);

        const ImVec2 rp_pos(display_size.x - LayoutScale(20.0f + 50.0f + 20.0f) - rp_size.x - rp_height,
                            subtitle_pos.y + LayoutScale(4.0f));

        title_pos.x -= rp_height;
        title_pos.y -= rp_height;
        subtitle_pos.x -= rp_height;
        subtitle_pos.y -= rp_height;

        dl->AddText(g_medium_font, g_medium_font->FontSize, rp_pos, IM_COL32(255, 255, 255, 255), rp.data(),
                    rp.data() + rp.size(), wrap_width);
      }
    }
#endif

    dl->AddText(g_large_font, g_large_font->FontSize, title_pos, IM_COL32(255, 255, 255, 255), title.c_str());
    dl->AddText(g_medium_font, g_medium_font->FontSize, subtitle_pos, IM_COL32(255, 255, 255, 255), subtitle);

    const ImVec2 image_min(display_size.x - LayoutScale(20.0f + 50.0f) - rp_height,
                           display_size.y - LayoutScale(20.0f + 50.0f) - rp_height);
    const ImVec2 image_max(image_min.x + LayoutScale(50.0f) + rp_height, image_min.y + LayoutScale(50.0f) + rp_height);
    dl->AddImage(GetCoverForCurrentGame()->GetHandle(), image_min, image_max);
  }

  const ImVec2 window_size(LayoutScale(500.0f, LAYOUT_SCREEN_HEIGHT));
  const ImVec2 window_pos(0.0f, display_size.y - window_size.y);

  if (BeginFullscreenWindow(window_pos, window_size, "pause_menu", ImVec4(0.0f, 0.0f, 0.0f, 0.0f), 0.0f, 10.0f,
                            ImGuiWindowFlags_NoBackground))
  {
    static constexpr u32 submenu_item_count[] = {
      11, // None
      4,  // Exit
#ifdef WITH_CHEEVOS
      3, // Achievements
#endif
    };

    const bool just_focused = ResetFocusHere();
    BeginMenuButtons(submenu_item_count[static_cast<u32>(s_current_pause_submenu)], 1.0f,
                     ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING, ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING,
                     ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    switch (s_current_pause_submenu)
    {
      case PauseSubMenu::None:
      {
        // NOTE: Menu close must come first, because otherwise VM destruction options will race.
        const bool can_load_or_save_state = System::IsValid();

        if (ActiveButton(ICON_FA_PLAY "  Resume Game", false) || WantsToCloseMenu())
          ClosePauseMenu();

        if (ActiveButton(ICON_FA_FAST_FORWARD "  Toggle Fast Forward", false))
        {
          ClosePauseMenu();
          DoToggleFastForward();
        }

        if (ActiveButton(ICON_FA_UNDO "  Load State", false, can_load_or_save_state))
        {
          if (OpenSaveStateSelector(true))
            s_current_main_window = MainWindowType::None;
        }

        if (ActiveButton(ICON_FA_DOWNLOAD "  Save State", false, can_load_or_save_state))
        {
          if (OpenSaveStateSelector(false))
            s_current_main_window = MainWindowType::None;
        }

#if 0
        if (ActiveButton(ICON_FA_FROWN_OPEN "  Cheat List", false, !IsCheevosHardcoreModeActive()))
        {
          s_current_main_window = MainWindowType::None;
          DoCheatsMenu();
        }

        if (ActiveButton(ICON_FA_GAMEPAD "  Toggle Analog", false))
        {
          CloseQuickMenu();
          DoToggleAnalogMode();
        }
#endif

        if (ActiveButton(ICON_FA_WRENCH "  Game Properties", false, !System::GetRunningCode().empty()))
        {
          SwitchToGameSettings();
        }

#ifdef WITH_CHEEVOS
        if (ActiveButton(ICON_FA_TROPHY "  Achievements", false,
                         Achievements::HasActiveGame() && Achievements::SafeHasAchievementsOrLeaderboards()))
        {
          const auto lock = Achievements::GetLock();

          // skip second menu and go straight to cheevos if there's no lbs
          if (Achievements::GetLeaderboardCount() == 0)
            OpenAchievementsWindow();
          else
            OpenPauseSubMenu(PauseSubMenu::Achievements);
        }
#else
        ActiveButton(ICON_FA_TROPHY "  Achievements", false, false);
#endif

        if (ActiveButton(ICON_FA_CAMERA "  Save Screenshot", false))
        {
          System::SaveScreenshot();
          ClosePauseMenu();
        }

        if (ActiveButton(g_gpu->IsHardwareRenderer() ? (ICON_FA_PAINT_BRUSH "  Switch To Software Renderer") :
                                                       (ICON_FA_PAINT_BRUSH "  Switch To Hardware Renderer"),
                         false))
        {
          ClosePauseMenu();
          DoToggleSoftwareRenderer();
        }

        if (ActiveButton(ICON_FA_COMPACT_DISC "  Change Disc", false))
        {
          s_current_main_window = MainWindowType::None;
          DoChangeDisc();
        }

        if (ActiveButton(ICON_FA_SLIDERS_H "  Settings", false))
          SwitchToSettings();

        if (ActiveButton(ICON_FA_POWER_OFF "  Close Game", false))
        {
          // skip submenu when we can't save anyway
          if (!can_load_or_save_state)
            DoShutdown(false);
          else
            OpenPauseSubMenu(PauseSubMenu::Exit);
        }
      }
      break;

      case PauseSubMenu::Exit:
      {
        if (just_focused)
          ImGui::SetFocusID(ImGui::GetID(ICON_FA_POWER_OFF "  Exit Without Saving"), ImGui::GetCurrentWindow());

        if (ActiveButton(ICON_FA_BACKWARD "  Back To Pause Menu", false))
        {
          OpenPauseSubMenu(PauseSubMenu::None);
        }

        if (ActiveButton(ICON_FA_SYNC "  Reset System", false))
        {
          ClosePauseMenu();
          DoReset();
        }

        if (ActiveButton(ICON_FA_SAVE "  Exit And Save State", false))
          DoShutdown(true);

        if (ActiveButton(ICON_FA_POWER_OFF "  Exit Without Saving", false))
          DoShutdown(false);
      }
      break;

#ifdef WITH_CHEEVOS
      case PauseSubMenu::Achievements:
      {
        if (ActiveButton(ICON_FA_BACKWARD "  Back To Pause Menu", false))
          OpenPauseSubMenu(PauseSubMenu::None);

        if (ActiveButton(ICON_FA_TROPHY "  Achievements", false))
          OpenAchievementsWindow();

        if (ActiveButton(ICON_FA_STOPWATCH "  Leaderboards", false))
          OpenLeaderboardsWindow();
      }
      break;
#endif
    }

    EndMenuButtons();

    EndFullscreenWindow();
  }
}

void FullscreenUI::InitializePlaceholderSaveStateListEntry(SaveStateListEntry* li, const std::string& title,
                                                           const std::string& serial, s32 slot, bool global)
{
  li->title = fmt::format("{0} {1} Slot {2}##{1}_slot_{2}", title, global ? "Global" : "Game", slot);
  li->summary = "No Save State";
  li->path = {};
  li->slot = slot;
  li->preview_texture = {};
}

bool FullscreenUI::InitializeSaveStateListEntry(SaveStateListEntry* li, const std::string& title,
                                                const std::string& serial, s32 slot, bool global)
{
  std::string filename(global ? System::GetGlobalSaveStateFileName(slot) :
                                System::GetGameSaveStateFileName(serial, slot));
  std::optional<ExtendedSaveStateInfo> ssi(System::GetExtendedSaveStateInfo(filename.c_str()));
  if (!ssi.has_value())
  {
    InitializePlaceholderSaveStateListEntry(li, title, serial, slot, global);
    return false;
  }

  if (global)
  {
    li->title = StringUtil::StdStringFromFormat("Global Save %d - %s##global_slot_%d", slot, ssi->title.c_str(), slot);
  }
  else
  {
    li->title = StringUtil::StdStringFromFormat("%s Slot %d##game_slot_%d", ssi->title.c_str(), slot, slot);
  }

  li->summary = fmt::format("{} - Saved {:%c}", ssi->game_code.c_str(), fmt::localtime(ssi->timestamp));
  li->slot = slot;
  li->path = std::move(filename);

  PopulateSaveStateScreenshot(li, &ssi.value());
  return true;
}

void FullscreenUI::PopulateSaveStateScreenshot(SaveStateListEntry* li, const ExtendedSaveStateInfo* ssi)
{
  li->preview_texture.reset();
  if (ssi && !ssi->screenshot_data.empty())
  {
    li->preview_texture = g_host_display->CreateTexture(ssi->screenshot_width, ssi->screenshot_height, 1, 1, 1,
                                                        HostDisplayPixelFormat::RGBA8, ssi->screenshot_data.data(),
                                                        sizeof(u32) * ssi->screenshot_width, false);
  }
  else
  {
    li->preview_texture = g_host_display->CreateTexture(PLACEHOLDER_ICON_WIDTH, PLACEHOLDER_ICON_HEIGHT, 1, 1, 1,
                                                        HostDisplayPixelFormat::RGBA8, PLACEHOLDER_ICON_DATA,
                                                        sizeof(u32) * PLACEHOLDER_ICON_WIDTH, false);
  }

  if (!li->preview_texture)
    Log_ErrorPrintf("Failed to upload save state image to GPU");
}

void FullscreenUI::ClearSaveStateEntryList()
{
  for (SaveStateListEntry& entry : s_save_state_selector_slots)
  {
    if (entry.preview_texture)
      s_cleanup_textures.push_back(std::move(entry.preview_texture));
  }
  s_save_state_selector_slots.clear();
}

u32 FullscreenUI::PopulateSaveStateListEntries(const std::string& title, const std::string& serial)
{
  ClearSaveStateEntryList();

  if (s_save_state_selector_loading)
  {
    std::optional<ExtendedSaveStateInfo> ssi = System::GetUndoSaveStateInfo();
    if (ssi)
    {
      SaveStateListEntry li;
      PopulateSaveStateScreenshot(&li, &ssi.value());
      li.title = "Undo Load State";
      li.summary = "Restores the state of the system prior to the last state loaded.";
      s_save_state_selector_slots.push_back(std::move(li));
    }
  }

  if (!serial.empty())
  {
    for (s32 i = 1; i <= System::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      SaveStateListEntry li;
      if (InitializeSaveStateListEntry(&li, title, serial, i, false) || !s_save_state_selector_loading)
        s_save_state_selector_slots.push_back(std::move(li));
    }
  }

  for (s32 i = 1; i <= System::GLOBAL_SAVE_STATE_SLOTS; i++)
  {
    SaveStateListEntry li;
    if (InitializeSaveStateListEntry(&li, title, serial, i, true) || !s_save_state_selector_loading)
      s_save_state_selector_slots.push_back(std::move(li));
  }

  return static_cast<u32>(s_save_state_selector_slots.size());
}

bool FullscreenUI::OpenLoadStateSelectorForGame(const std::string& game_path)
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(game_path.c_str());
  if (entry)
  {
    s_save_state_selector_loading = true;
    if (PopulateSaveStateListEntries(entry->title.c_str(), entry->serial.c_str()) > 0)
    {
      s_save_state_selector_open = true;
      s_save_state_selector_game_path = game_path;
      return true;
    }
  }

  ShowToast({}, "No save states found.", 5.0f);
  return false;
}

bool FullscreenUI::OpenSaveStateSelector(bool is_loading)
{
  s_save_state_selector_game_path = {};
  s_save_state_selector_loading = is_loading;
  if (PopulateSaveStateListEntries(System::GetRunningTitle().c_str(), System::GetRunningCode().c_str()) > 0)
  {
    s_save_state_selector_open = true;
    return true;
  }

  ShowToast({}, "No save states found.", 5.0f);
  return false;
}

void FullscreenUI::CloseSaveStateSelector()
{
  ClearSaveStateEntryList();
  s_save_state_selector_open = false;
  s_save_state_selector_loading = false;
  s_save_state_selector_game_path = {};
  ReturnToMainWindow();
}

void FullscreenUI::DrawSaveStateSelector(bool is_loading, bool fullscreen)
{
  if (fullscreen)
  {
    if (!BeginFullscreenColumns())
    {
      EndFullscreenColumns();
      return;
    }

    if (!BeginFullscreenColumnWindow(0.0f, LAYOUT_SCREEN_WIDTH, "save_state_selector_slots"))
    {
      EndFullscreenColumnWindow();
      EndFullscreenColumns();
      return;
    }
  }
  else
  {
    const char* window_title = is_loading ? "Load State" : "Save State";

    ImGui::PushFont(g_large_font);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                                ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));

    ImGui::SetNextWindowSize(LayoutScale(1000.0f, 680.0f));
    ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::OpenPopup(window_title);
    bool is_open = !WantsToCloseMenu();
    if (!ImGui::BeginPopupModal(window_title, &is_open,
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove) ||
        !is_open)
    {
      ImGui::PopStyleVar(2);
      ImGui::PopFont();
      CloseSaveStateSelector();
      return;
    }
  }

  BeginMenuButtons();

  constexpr float padding = 10.0f;
  constexpr float button_height = 96.0f;
  constexpr float max_image_width = 96.0f;
  constexpr float max_image_height = 96.0f;

  for (const SaveStateListEntry& entry : s_save_state_selector_slots)
  {
    ImRect bb;
    bool visible, hovered;
    bool pressed = MenuButtonFrame(entry.title.c_str(), true, button_height, &visible, &hovered, &bb.Min, &bb.Max);
    if (!visible)
      continue;

    ImVec2 pos(bb.Min);

    // use aspect ratio of screenshot to determine height
    const HostDisplayTexture* image =
      entry.preview_texture ? entry.preview_texture.get() : GetPlaceholderTexture().get();
    const float image_height =
      max_image_width / (static_cast<float>(image->GetWidth()) / static_cast<float>(image->GetHeight()));
    const float image_margin = (max_image_height - image_height) / 2.0f;
    const ImRect image_bb(ImVec2(pos.x, pos.y + LayoutScale(image_margin)),
                          pos + LayoutScale(max_image_width, image_margin + image_height));
    pos.x += LayoutScale(max_image_width + padding);

    ImRect text_bb(pos, ImVec2(bb.Max.x, pos.y + g_large_font->FontSize));
    ImGui::PushFont(g_large_font);
    ImGui::RenderTextClipped(text_bb.Min, text_bb.Max, entry.title.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f),
                             &text_bb);
    ImGui::PopFont();

    ImGui::PushFont(g_medium_font);

    if (!entry.summary.empty())
    {
      text_bb.Min.y = text_bb.Max.y + LayoutScale(4.0f);
      text_bb.Max.y = text_bb.Min.y + g_medium_font->FontSize;
      ImGui::RenderTextClipped(text_bb.Min, text_bb.Max, entry.summary.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f),
                               &text_bb);
    }

    if (!entry.path.empty())
    {
      text_bb.Min.y = text_bb.Max.y + LayoutScale(4.0f);
      text_bb.Max.y = text_bb.Min.y + g_medium_font->FontSize;
      ImGui::RenderTextClipped(text_bb.Min, text_bb.Max, entry.path.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f),
                               &text_bb);
    }

    ImGui::PopFont();

    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(entry.preview_texture ?
                                                                    entry.preview_texture->GetHandle() :
                                                                    GetPlaceholderTexture()->GetHandle()),
                                         image_bb.Min, image_bb.Max);

    if (pressed)
    {
      if (is_loading)
        DoLoadState(entry.path);
      else
        DoSaveState(entry.slot, entry.global);
    }
  }

  EndMenuButtons();

  if (fullscreen)
  {
    EndFullscreenColumnWindow();
    EndFullscreenColumns();
  }
  else
  {
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
  }
}

void FullscreenUI::DoLoadState(std::string path)
{
  Host::RunOnCPUThread([boot_path = s_save_state_selector_game_path, path = std::move(path)]() {
    CloseSaveStateSelector();

    if (System::IsValid())
    {
      System::LoadState(path.c_str());
    }
    else
    {
      SystemBootParameters params;
      params.filename = std::move(boot_path);
      params.save_state = std::move(path);
      System::BootSystem(std::move(params));
    }
  });
}

void FullscreenUI::DoSaveState(s32 slot, bool global)
{
  Host::RunOnCPUThread([slot, global]() {
    CloseSaveStateSelector();
    if (!System::IsValid())
      return;

    std::string filename(global ? System::GetGlobalSaveStateFileName(slot) :
                                  System::GetGameSaveStateFileName(System::GetRunningCode(), slot));
    System::SaveState(filename.c_str(), g_settings.create_save_state_backups);
  });
}

void FullscreenUI::PopulateGameListEntryList()
{
  const u32 count = GameList::GetEntryCount();
  s_game_list_sorted_entries.resize(count);
  for (u32 i = 0; i < count; i++)
    s_game_list_sorted_entries[i] = GameList::GetEntryByIndex(i);

  // TODO: Custom sort types
  std::sort(s_game_list_sorted_entries.begin(), s_game_list_sorted_entries.end(),
            [](const GameList::Entry* lhs, const GameList::Entry* rhs) { return lhs->title < rhs->title; });
}

void FullscreenUI::DrawGameListWindow()
{
  if (!BeginFullscreenColumns())
  {
    EndFullscreenColumns();
    return;
  }

  auto game_list_lock = GameList::GetLock();
  const GameList::Entry* selected_entry = nullptr;
  PopulateGameListEntryList();

  if (BeginFullscreenColumnWindow(0.0f, 750.0f, "game_list_entries"))
  {
    const ImVec2 image_size(LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT * 0.68f, LAYOUT_MENU_BUTTON_HEIGHT));

    ResetFocusHere();

    BeginMenuButtons();

    // TODO: replace with something not heap alllocating
    SmallString summary;

    for (const GameList::Entry* entry : s_game_list_sorted_entries)
    {
      ImRect bb;
      bool visible, hovered;
      bool pressed =
        MenuButtonFrame(entry->path.c_str(), true, LAYOUT_MENU_BUTTON_HEIGHT, &visible, &hovered, &bb.Min, &bb.Max);
      if (!visible)
        continue;

      HostDisplayTexture* cover_texture = GetGameListCover(entry);

      if (entry->serial.empty())
      {
        summary.Fmt("{} - ", Settings::GetDiscRegionDisplayName(entry->region));
      }
      else
      {
        summary.Fmt("{} - {} - ", entry->serial, Settings::GetDiscRegionDisplayName(entry->region));
      }

      const std::string_view filename(Path::GetFileName(entry->path));
      summary.AppendString(filename);

      const ImRect image_rect(
        CenterImage(ImRect(bb.Min, bb.Min + image_size), ImVec2(static_cast<float>(cover_texture->GetWidth()),
                                                                static_cast<float>(cover_texture->GetHeight()))));

      ImGui::GetWindowDrawList()->AddImage(cover_texture->GetHandle(), image_rect.Min, image_rect.Max,
                                           ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));

      const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
      const float text_start_x = bb.Min.x + image_size.x + LayoutScale(15.0f);
      const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
      const ImRect summary_bb(ImVec2(text_start_x, midpoint), bb.Max);

      ImGui::PushFont(g_large_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, entry->title.c_str(),
                               entry->title.c_str() + entry->title.size(), nullptr, ImVec2(0.0f, 0.0f), &title_bb);
      ImGui::PopFont();

      if (!summary.IsEmpty())
      {
        ImGui::PushFont(g_medium_font);
        ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, summary.GetCharArray(),
                                 summary.GetCharArray() + summary.GetLength(), nullptr, ImVec2(0.0f, 0.0f),
                                 &summary_bb);
        ImGui::PopFont();
      }

      if (pressed)
      {
        // launch game
        DoStartPath(entry->path);
      }

      if (hovered)
        selected_entry = entry;

      if (selected_entry && (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                             ImGui::IsNavInputTest(ImGuiNavInput_Input, ImGuiNavReadMode_Pressed)))
      {
        ImGuiFullscreen::ChoiceDialogOptions options = {
          {"Open Game Properties", false}, {"Resume Game", false}, {"Load State", false},
          {"Default Boot", false},         {"Fast Boot", false},   {"Slow Boot", false},
          {"Close Menu", false},
        };

        OpenChoiceDialog(selected_entry->title.c_str(), false, std::move(options),
                         [entry_path = selected_entry->path,
                          entry_serial = selected_entry->serial](s32 index, const std::string& title, bool checked) {
                           switch (index)
                           {
                             case 0: // Open Game Properties
                               SwitchToGameSettingsForPath(entry_path);
                               break;
                             case 1: // Resume Game
                               DoStartPath(entry_path, System::GetGameSaveStateFileName(entry_serial, -1));
                               break;
                             case 2: // Load State
                               OpenLoadStateSelectorForGame(entry_path);
                               break;
                             case 3: // Default Boot
                               DoStartPath(entry_path);
                               break;
                             case 4: // Fast Boot
                               DoStartPath(entry_path, {}, true);
                               break;
                             case 5: // Slow Boot
                               DoStartPath(entry_path, {}, false);
                               break;
                             default:
                               break;
                           }

                           CloseChoiceDialog();
                         });
      }
    }

    EndMenuButtons();
  }
  EndFullscreenColumnWindow();

  if (BeginFullscreenColumnWindow(750.0f, LAYOUT_SCREEN_WIDTH, "game_list_info", UIPrimaryDarkColor))
  {
    const HostDisplayTexture* cover_texture =
      selected_entry ? GetGameListCover(selected_entry) : GetTextureForGameListEntryType(GameList::EntryType::Count);
    if (cover_texture)
    {
      const ImRect image_rect(
        CenterImage(LayoutScale(ImVec2(240.0f, 350.0f)), ImVec2(static_cast<float>(cover_texture->GetWidth()),
                                                                static_cast<float>(cover_texture->GetHeight()))));

      ImGui::SetCursorPos(LayoutScale(ImVec2(145.0f, 50.0f)) + image_rect.Min);
      ImGui::Image(selected_entry ? GetGameListCover(selected_entry)->GetHandle() :
                                    GetTextureForGameListEntryType(GameList::EntryType::Count)->GetHandle(),
                   image_rect.GetSize());
    }

    const float work_width = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    constexpr float field_margin_y = 10.0f;
    constexpr float start_x = 50.0f;
    float text_y = 425.0f;
    float text_width;

    ImGui::SetCursorPos(LayoutScale(start_x, text_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, field_margin_y));
    ImGui::BeginGroup();

    if (selected_entry)
    {
      // title
      ImGui::PushFont(g_large_font);
      text_width = ImGui::CalcTextSize(selected_entry->title.c_str(), nullptr, false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::TextWrapped("%s", selected_entry->title.c_str());
      ImGui::PopFont();

      ImGui::PushFont(g_medium_font);

      // developer
      const char* developer = "Unknown Developer";
      if (!selected_entry->developer.empty())
      {
        text_width =
          ImGui::CalcTextSize(selected_entry->developer.c_str(),
                              selected_entry->developer.c_str() + selected_entry->developer.length(), false, work_width)
            .x;
        ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
        ImGui::TextWrapped("%s", developer);
      }

      // code
      text_width = ImGui::CalcTextSize(selected_entry->serial.c_str(), nullptr, false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::TextWrapped("%s", selected_entry->serial.c_str());
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 15.0f);

      // region
      {
        std::string flag_texture(
          fmt::format("fullscreenui/{}.png", Settings::GetDiscRegionName(selected_entry->region)));
        ImGui::TextUnformatted("Region: ");
        ImGui::SameLine();
        ImGui::Image(GetCachedTextureAsync(flag_texture.c_str())->GetHandle(), LayoutScale(23.0f, 16.0f));
        ImGui::SameLine();
        ImGui::Text(" (%s)", Settings::GetDiscRegionDisplayName(selected_entry->region));
      }

      // genre
      ImGui::Text("Genre: %s", selected_entry->genre.c_str());

      // release date
      char release_date_str[64];
      selected_entry->GetReleaseDateString(release_date_str, sizeof(release_date_str));
      ImGui::Text("Release Date: %s", release_date_str);

      // compatibility
      ImGui::TextUnformatted("Compatibility: ");
      ImGui::SameLine();
      if (selected_entry->compatibility != GameDatabase::CompatibilityRating::Unknown)
      {
        ImGui::Image(s_game_compatibility_textures[static_cast<u32>(selected_entry->compatibility)]->GetHandle(),
                     LayoutScale(64.0f, 16.0f));
        ImGui::SameLine();
      }
      ImGui::Text(" (%s)", GameDatabase::GetCompatibilityRatingDisplayName(selected_entry->compatibility));

      // size
      ImGui::Text("Size: %.2f MB", static_cast<float>(selected_entry->total_size) / 1048576.0f);

      // game settings
      const u32 user_setting_count = 0; // FIXME
      if (user_setting_count > 0)
        ImGui::Text("%u Per-Game Settings Set", user_setting_count);
      else
        ImGui::TextUnformatted("No Per-Game Settings Set");

      ImGui::PopFont();
    }
    else
    {
      // title
      const char* title = "No Game Selected";
      ImGui::PushFont(g_large_font);
      text_width = ImGui::CalcTextSize(title, nullptr, false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::TextWrapped("%s", title);
      ImGui::PopFont();
    }

    ImGui::EndGroup();
    ImGui::PopStyleVar();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - LayoutScale(50.0f));
    BeginMenuButtons();
    if (ActiveButton(ICON_FA_BACKWARD "  Back", false))
      ReturnToMainWindow();
    EndMenuButtons();
  }
  EndFullscreenColumnWindow();

  EndFullscreenColumns();
}

void FullscreenUI::SwitchToGameList()
{
  s_current_main_window = MainWindowType::GameList;
  QueueResetFocus();
}

HostDisplayTexture* FullscreenUI::GetGameListCover(const GameList::Entry* entry)
{
  // lookup and grab cover image
  auto cover_it = s_cover_image_map.find(entry->path);
  if (cover_it == s_cover_image_map.end())
  {
    std::string cover_path(GameList::GetCoverImagePathForEntry(entry));
    cover_it = s_cover_image_map.emplace(entry->path, std::move(cover_path)).first;
  }

  HostDisplayTexture* tex = (!cover_it->second.empty()) ? GetCachedTextureAsync(cover_it->second.c_str()) : nullptr;
  return tex ? tex : GetTextureForGameListEntryType(entry->type);
}

HostDisplayTexture* FullscreenUI::GetTextureForGameListEntryType(GameList::EntryType type)
{
  switch (type)
  {
    case GameList::EntryType::PSExe:
      return s_fallback_exe_texture.get();

    case GameList::EntryType::Playlist:
      return s_fallback_playlist_texture.get();

    case GameList::EntryType::PSF:
      return s_fallback_psf_texture.get();

    case GameList::EntryType::Disc:
    default:
      return s_fallback_disc_texture.get();
  }
}

HostDisplayTexture* FullscreenUI::GetCoverForCurrentGame()
{
  auto lock = GameList::GetLock();

  const GameList::Entry* entry = GameList::GetEntryForPath(System::GetRunningPath().c_str());
  if (!entry)
    return s_fallback_disc_texture.get();

  return GetGameListCover(entry);
}

std::string FullscreenUI::GetNotificationImageForGame(const GameList::Entry* entry)
{
  std::string ret;

  if (entry)
    ret = GameList::GetCoverImagePathForEntry(entry);

  return ret;
}

std::string FullscreenUI::GetNotificationImageForGame(const std::string& game_path)
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(game_path.c_str());
  return entry ? GetNotificationImageForGame(entry) : std::string();
}

//////////////////////////////////////////////////////////////////////////
// Overlays
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::OpenAboutWindow()
{
  s_about_window_open = true;
}

void FullscreenUI::DrawAboutWindow()
{
  ImGui::SetNextWindowSize(LayoutScale(1000.0f, 500.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup("About DuckStation");

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(10.0f, 10.0f));

  if (ImGui::BeginPopupModal("About DuckStation", &s_about_window_open,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    ImGui::TextWrapped("DuckStation is a free and open-source simulator/emulator of the Sony PlayStation(TM) console, "
                       "focusing on playability, speed, and long-term maintainability.");
    ImGui::NewLine();
    ImGui::TextWrapped("Contributor List: https://github.com/stenzek/duckstation/blob/master/CONTRIBUTORS.md");
    ImGui::NewLine();
    ImGui::TextWrapped("Duck icon by icons8 (https://icons8.com/icon/74847/platforms.undefined.short-title)");
    ImGui::NewLine();
    ImGui::TextWrapped("\"PlayStation\" and \"PSX\" are registered trademarks of Sony Interactive Entertainment Europe "
                       "Limited. This software is not affiliated in any way with Sony Interactive Entertainment.");

    ImGui::NewLine();

    BeginMenuButtons();
    if (ActiveButton(ICON_FA_GLOBE "  GitHub Repository", false))
    {
      Host::ReportErrorAsync("Notice", "Go to https://github.com/stenzek/duckstation/");
    }
    if (ActiveButton(ICON_FA_BUG "  Issue Tracker", false))
    {
      Host::ReportErrorAsync("Notice", "Go to https://github.com/stenzek/duckstation/issues");
    }
    if (ActiveButton(ICON_FA_COMMENT "  Discord Server", false))
    {
      Host::ReportErrorAsync("Notice", "Go to https://discord.gg/Buktv3t");
    }

    if (ActiveButton(ICON_FA_WINDOW_CLOSE "  Close", false))
    {
      ImGui::CloseCurrentPopup();
      s_about_window_open = false;
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopFont();
}

bool FullscreenUI::DrawErrorWindow(const char* message)
{
  bool is_open = true;

  ImGuiFullscreen::BeginLayout();

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup("ReportError");

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(10.0f, 10.0f));

  if (ImGui::BeginPopupModal("ReportError", &is_open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    ImGui::SetCursorPos(LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
    ImGui::TextWrapped("%s", message);
    ImGui::GetCurrentWindow()->DC.CursorPos.y += LayoutScale(5.0f);

    BeginMenuButtons();

    if (ActiveButton(ICON_FA_WINDOW_CLOSE "  Close", false))
    {
      ImGui::CloseCurrentPopup();
      is_open = false;
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopFont();

  ImGuiFullscreen::EndLayout();
  return !is_open;
}

bool FullscreenUI::DrawConfirmWindow(const char* message, bool* result)
{
  bool is_open = true;

  ImGuiFullscreen::BeginLayout();

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup("ConfirmMessage");

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(10.0f, 10.0f));

  if (ImGui::BeginPopupModal("ConfirmMessage", &is_open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    ImGui::SetCursorPos(LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
    ImGui::TextWrapped("%s", message);
    ImGui::GetCurrentWindow()->DC.CursorPos.y += LayoutScale(5.0f);

    BeginMenuButtons();

    bool done = false;

    if (ActiveButton(ICON_FA_CHECK "  Yes", false))
    {
      *result = true;
      done = true;
    }

    if (ActiveButton(ICON_FA_TIMES "  No", false))
    {
      *result = false;
      done = true;
    }
    if (done)
    {
      ImGui::CloseCurrentPopup();
      is_open = false;
    }

    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopFont();

  ImGuiFullscreen::EndLayout();
  return !is_open;
}

#ifdef WITH_CHEEVOS

bool FullscreenUI::OpenAchievementsWindow()
{
  if (!System::IsValid() || !Achievements::HasActiveGame() || Achievements::GetAchievementCount() == 0 || !Initialize())
    return false;

  if (s_current_main_window != MainWindowType::PauseMenu)
    PauseForMenuOpen();

  s_current_main_window = MainWindowType::Achievements;
  QueueResetFocus();
  return true;
}

void FullscreenUI::DrawAchievement(const Achievements::Achievement& cheevo)
{
  static constexpr float alpha = 0.8f;
  static constexpr float progress_height_unscaled = 20.0f;
  static constexpr float progress_spacing_unscaled = 5.0f;

  std::string id_str(fmt::format("chv_{}", cheevo.id));

  const auto progress = Achievements::GetAchievementProgress(cheevo);
  const bool is_measured = progress.second != 0;

  ImRect bb;
  bool visible, hovered;
  bool pressed =
    MenuButtonFrame(id_str.c_str(), true,
                    !is_measured ? LAYOUT_MENU_BUTTON_HEIGHT :
                                   LAYOUT_MENU_BUTTON_HEIGHT + progress_height_unscaled + progress_spacing_unscaled,
                    &visible, &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const ImVec2 image_size(LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT, LAYOUT_MENU_BUTTON_HEIGHT));
  const std::string& badge_path = Achievements::GetAchievementBadgePath(cheevo);
  if (!badge_path.empty())
  {
    HostDisplayTexture* badge = GetCachedTextureAsync(badge_path.c_str());
    if (badge)
    {
      ImGui::GetWindowDrawList()->AddImage(badge->GetHandle(), bb.Min, bb.Min + image_size, ImVec2(0.0f, 0.0f),
                                           ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
    }
  }

  const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
  const float text_start_x = bb.Min.x + image_size.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint), bb.Max);

  ImGui::PushFont(g_large_font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, cheevo.title.c_str(), cheevo.title.c_str() + cheevo.title.size(),
                           nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  if (!cheevo.description.empty())
  {
    ImGui::PushFont(g_medium_font);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, cheevo.description.c_str(),
                             cheevo.description.c_str() + cheevo.description.size(), nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (is_measured)
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float progress_height = LayoutScale(progress_height_unscaled);
    const float progress_spacing = LayoutScale(progress_spacing_unscaled);
    const float top = midpoint + g_medium_font->FontSize + progress_spacing;
    const ImRect progress_bb(ImVec2(text_start_x, top), ImVec2(bb.Max.x, top + progress_height));
    const float fraction = static_cast<float>(progress.first) / static_cast<float>(progress.second);
    dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryDarkColor));
    dl->AddRectFilled(progress_bb.Min, ImVec2(progress_bb.Min.x + fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                      ImGui::GetColorU32(ImGuiFullscreen::UISecondaryColor));

    const std::string text(Achievements::GetAchievementProgressText(cheevo));
    const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                          progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f));
    dl->AddText(g_medium_font, g_medium_font->FontSize, text_pos,
                ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryTextColor), text.c_str(), text.c_str() + text.size());
  }

#if 0
  // The API doesn't seem to send us this :(
  if (!cheevo.locked)
  {
    ImGui::PushFont(g_medium_font);

    const ImRect time_bb(ImVec2(text_start_x, bb.Min.y),
      ImVec2(bb.Max.x, bb.Min.y + g_medium_font->FontSize + LayoutScale(4.0f)));
    text.Format("Unlocked 21 Feb, 2019 @ 3:14am");
    ImGui::RenderTextClipped(time_bb.Min, time_bb.Max, text.GetCharArray(), text.GetCharArray() + text.GetLength(),
      nullptr, ImVec2(1.0f, 0.0f), &time_bb);
    ImGui::PopFont();
  }
#endif

  if (pressed)
  {
    // TODO: What should we do here?
    // Display information or something..
  }
}

void FullscreenUI::DrawAchievementsWindow()
{
  // ensure image downloads still happen while we're paused
  Achievements::ProcessPendingHTTPRequests();

  static constexpr float alpha = 0.8f;
  static constexpr float heading_height_unscaled = 110.0f;

  ImGui::SetNextWindowBgAlpha(alpha);

  const ImVec4 background(0.13f, 0.13f, 0.13f, alpha);
  const ImVec2 display_size(ImGui::GetIO().DisplaySize);
  const float heading_height = LayoutScale(heading_height_unscaled);

  if (BeginFullscreenWindow(
        ImVec2(0.0f, 0.0f), ImVec2(display_size.x, heading_height), "achievements_heading", background, 0.0f, 0.0f,
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse))
  {
    auto lock = Achievements::GetLock();

    ImRect bb;
    bool visible, hovered;
    /*bool pressed = */ MenuButtonFrame("achievements_heading", false, heading_height_unscaled, &visible, &hovered,
                                        &bb.Min, &bb.Max, 0, alpha);

    if (visible)
    {
      const float padding = LayoutScale(10.0f);
      const float spacing = LayoutScale(10.0f);
      const float image_height = LayoutScale(85.0f);

      const ImVec2 icon_min(bb.Min + ImVec2(padding, padding));
      const ImVec2 icon_max(icon_min + ImVec2(image_height, image_height));

      const std::string& icon_path = Achievements::GetGameIcon();
      if (!icon_path.empty())
      {
        HostDisplayTexture* badge = GetCachedTexture(icon_path.c_str());
        if (badge)
        {
          ImGui::GetWindowDrawList()->AddImage(badge->GetHandle(), icon_min, icon_max, ImVec2(0.0f, 0.0f),
                                               ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
        }
      }

      float left = bb.Min.x + padding + image_height + spacing;
      float right = bb.Max.x - padding;
      float top = bb.Min.y + padding;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      std::string text;
      ImVec2 text_size;

      const u32 unlocked_count = Achievements::GetUnlockedAchiementCount();
      const u32 achievement_count = Achievements::GetAchievementCount();
      const u32 current_points = Achievements::GetCurrentPointsForGame();
      const u32 total_points = Achievements::GetMaximumPointsForGame();

      if (FloatingButton(ICON_FA_WINDOW_CLOSE, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true, g_large_font) ||
          WantsToCloseMenu())
      {
        ReturnToMainWindow();
      }

      const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + g_large_font->FontSize));
      text = Achievements::GetGameTitle();

      if (Achievements::ChallengeModeActive())
        text += " (Hardcore Mode)";

      top += g_large_font->FontSize + spacing;

      ImGui::PushFont(g_large_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, text.c_str(), text.c_str() + text.length(), nullptr,
                               ImVec2(0.0f, 0.0f), &title_bb);
      ImGui::PopFont();

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
      if (unlocked_count == achievement_count)
      {
        text = fmt::format("You have unlocked all achievements and earned {} points!", total_points);
      }
      else
      {
        text = fmt::format("You have unlocked {} of {} achievements, earning {} of {} possible points.", unlocked_count,
                           achievement_count, current_points, total_points);
      }

      top += g_medium_font->FontSize + spacing;

      ImGui::PushFont(g_medium_font);
      ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, text.c_str(), text.c_str() + text.length(), nullptr,
                               ImVec2(0.0f, 0.0f), &summary_bb);
      ImGui::PopFont();

      const float progress_height = LayoutScale(20.0f);
      const ImRect progress_bb(ImVec2(left, top), ImVec2(right, top + progress_height));
      const float fraction = static_cast<float>(unlocked_count) / static_cast<float>(achievement_count);
      dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryDarkColor));
      dl->AddRectFilled(progress_bb.Min,
                        ImVec2(progress_bb.Min.x + fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                        ImGui::GetColorU32(ImGuiFullscreen::UISecondaryColor));

      text = fmt::format("{}%", static_cast<int>(std::round(fraction * 100.0f)));
      text_size = ImGui::CalcTextSize(text.c_str());
      const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                            progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) -
                              (text_size.y / 2.0f));
      dl->AddText(g_medium_font, g_medium_font->FontSize, text_pos,
                  ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryTextColor), text.c_str(), text.c_str() + text.length());
      top += progress_height + spacing;
    }
  }
  EndFullscreenWindow();

  ImGui::SetNextWindowBgAlpha(alpha);

  if (BeginFullscreenWindow(ImVec2(0.0f, heading_height), ImVec2(display_size.x, display_size.y - heading_height),
                            "achievements", background, 0.0f, 0.0f, 0))
  {
    BeginMenuButtons();

    static bool unlocked_achievements_collapsed = false;

    unlocked_achievements_collapsed ^= MenuHeadingButton(
      "Unlocked Achievements", unlocked_achievements_collapsed ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_UP);
    if (!unlocked_achievements_collapsed)
    {
      Achievements::EnumerateAchievements([](const Achievements::Achievement& cheevo) -> bool {
        if (!cheevo.locked)
          DrawAchievement(cheevo);

        return true;
      });
    }

    if (Achievements::GetUnlockedAchiementCount() != Achievements::GetAchievementCount())
    {
      static bool locked_achievements_collapsed = false;
      locked_achievements_collapsed ^= MenuHeadingButton(
        "Locked Achievements", locked_achievements_collapsed ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_UP);
      if (!locked_achievements_collapsed)
      {
        Achievements::EnumerateAchievements([](const Achievements::Achievement& cheevo) -> bool {
          if (cheevo.locked)
            DrawAchievement(cheevo);

          return true;
        });
      }
    }

    EndMenuButtons();
  }
  EndFullscreenWindow();
}

bool FullscreenUI::OpenLeaderboardsWindow()
{
#ifdef WITH_CHEEVOS
  if (!System::IsValid() || !Achievements::HasActiveGame() || Achievements::GetLeaderboardCount() == 0 || !Initialize())
    return false;

  if (s_current_main_window != MainWindowType::PauseMenu)
    PauseForMenuOpen();

  s_current_main_window = MainWindowType::Leaderboards;
  s_open_leaderboard_id.reset();
  QueueResetFocus();
  return true;
#else
  return false;
#endif
}

void FullscreenUI::DrawLeaderboardListEntry(const Achievements::Leaderboard& lboard)
{
  static constexpr float alpha = 0.8f;

  TinyString id_str;
  id_str.Format("%u", lboard.id);

  ImRect bb;
  bool visible, hovered;
  bool pressed =
    MenuButtonFrame(id_str, true, LAYOUT_MENU_BUTTON_HEIGHT, &visible, &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
  const float text_start_x = bb.Min.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint), bb.Max);

  ImGui::PushFont(g_large_font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, lboard.title.c_str(), lboard.title.c_str() + lboard.title.size(),
                           nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  if (!lboard.description.empty())
  {
    ImGui::PushFont(g_medium_font);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, lboard.description.c_str(),
                             lboard.description.c_str() + lboard.description.size(), nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (pressed)
  {
    s_open_leaderboard_id = lboard.id;
  }
}

void FullscreenUI::DrawLeaderboardEntry(const Achievements::LeaderboardEntry& lbEntry, float rank_column_width,
                                        float name_column_width, float column_spacing)
{
  static constexpr float alpha = 0.8f;

  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(lbEntry.user.c_str(), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, &visible, &hovered,
                                 &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
  float text_start_x = bb.Min.x + LayoutScale(15.0f);
  SmallString text;

  text.Format("%u", lbEntry.rank);

  ImGui::PushFont(g_large_font);
  if (lbEntry.is_self)
  {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(255, 242, 0, 255));
  }

  const ImRect rank_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  ImGui::RenderTextClipped(rank_bb.Min, rank_bb.Max, text.GetCharArray(), text.GetCharArray() + text.GetLength(),
                           nullptr, ImVec2(0.0f, 0.0f), &rank_bb);
  text_start_x += rank_column_width + column_spacing;

  const ImRect user_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  ImGui::RenderTextClipped(user_bb.Min, user_bb.Max, lbEntry.user.c_str(), lbEntry.user.c_str() + lbEntry.user.size(),
                           nullptr, ImVec2(0.0f, 0.0f), &user_bb);
  text_start_x += name_column_width + column_spacing;

  const ImRect score_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  ImGui::RenderTextClipped(score_bb.Min, score_bb.Max, lbEntry.formatted_score.c_str(),
                           lbEntry.formatted_score.c_str() + lbEntry.formatted_score.size(), nullptr,
                           ImVec2(0.0f, 0.0f), &score_bb);

  if (lbEntry.is_self)
  {
    ImGui::PopStyleColor();
  }

  ImGui::PopFont();

  // This API DOES list the submission date/time, but is it relevant?
#if 0
  if (!cheevo.locked)
  {
    ImGui::PushFont(g_medium_font);

    const ImRect time_bb(ImVec2(text_start_x, bb.Min.y),
      ImVec2(bb.Max.x, bb.Min.y + g_medium_font->FontSize + LayoutScale(4.0f)));
    text.Format("Unlocked 21 Feb, 2019 @ 3:14am");
    ImGui::RenderTextClipped(time_bb.Min, time_bb.Max, text.GetCharArray(), text.GetCharArray() + text.GetLength(),
      nullptr, ImVec2(1.0f, 0.0f), &time_bb);
    ImGui::PopFont();
  }
#endif

  if (pressed)
  {
    // Anything?
  }
}

void FullscreenUI::DrawLeaderboardsWindow()
{
  static constexpr float alpha = 0.8f;
  static constexpr float heading_height_unscaled = 110.0f;

  // ensure image downloads still happen while we're paused
  Achievements::ProcessPendingHTTPRequests();

  ImGui::SetNextWindowBgAlpha(alpha);

  const bool is_leaderboard_open = s_open_leaderboard_id.has_value();
  bool close_leaderboard_on_exit = false;

  const ImVec4 background(0.13f, 0.13f, 0.13f, alpha);
  const ImVec2 display_size(ImGui::GetIO().DisplaySize);
  const float padding = LayoutScale(10.0f);
  const float spacing = LayoutScale(10.0f);
  const float spacing_small = spacing / 2.0f;
  float heading_height = LayoutScale(heading_height_unscaled);
  if (is_leaderboard_open)
  {
    // Add space for a legend - spacing + 1 line of text + spacing + line
    heading_height += spacing + LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + spacing;
  }

  const float rank_column_width =
    g_large_font->CalcTextSizeA(g_large_font->FontSize, std::numeric_limits<float>::max(), -1.0f, "99999").x;
  const float name_column_width =
    g_large_font
      ->CalcTextSizeA(g_large_font->FontSize, std::numeric_limits<float>::max(), -1.0f, "WWWWWWWWWWWWWWWWWWWW")
      .x;
  const float column_spacing = spacing * 2.0f;

  if (BeginFullscreenWindow(
        ImVec2(0.0f, 0.0f), ImVec2(display_size.x, heading_height), "leaderboards_heading", background, 0.0f, 0.0f,
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse))
  {
    ImRect bb;
    bool visible, hovered;
    bool pressed = MenuButtonFrame("leaderboards_heading", false, heading_height_unscaled, &visible, &hovered, &bb.Min,
                                   &bb.Max, 0, alpha);
    UNREFERENCED_VARIABLE(pressed);

    if (visible)
    {
      const float image_height = LayoutScale(85.0f);

      const ImVec2 icon_min(bb.Min + ImVec2(padding, padding));
      const ImVec2 icon_max(icon_min + ImVec2(image_height, image_height));

      const std::string& icon_path = Achievements::GetGameIcon();
      if (!icon_path.empty())
      {
        HostDisplayTexture* badge = GetCachedTexture(icon_path.c_str());
        if (badge)
        {
          ImGui::GetWindowDrawList()->AddImage(badge->GetHandle(), icon_min, icon_max, ImVec2(0.0f, 0.0f),
                                               ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
        }
      }

      float left = bb.Min.x + padding + image_height + spacing;
      float right = bb.Max.x - padding;
      float top = bb.Min.y + padding;
      SmallString text;
      ImVec2 text_size;

      const u32 leaderboard_count = Achievements::GetLeaderboardCount();

      if (!is_leaderboard_open)
      {
        if (FloatingButton(ICON_FA_WINDOW_CLOSE, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true, g_large_font) ||
            WantsToCloseMenu())
        {
          ReturnToMainWindow();
        }
      }
      else
      {
        if (FloatingButton(ICON_FA_CARET_SQUARE_LEFT, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true, g_large_font) ||
            WantsToCloseMenu())
        {
          close_leaderboard_on_exit = true;
        }
      }

      const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + g_large_font->FontSize));
      text.Assign(Achievements::GetGameTitle());

      top += g_large_font->FontSize + spacing;

      ImGui::PushFont(g_large_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, text.GetCharArray(), text.GetCharArray() + text.GetLength(),
                               nullptr, ImVec2(0.0f, 0.0f), &title_bb);
      ImGui::PopFont();

      if (s_open_leaderboard_id.has_value())
      {
        const Achievements::Leaderboard* lboard = Achievements::GetLeaderboardByID(s_open_leaderboard_id.value());
        if (lboard != nullptr)
        {
          const ImRect subtitle_bb(ImVec2(left, top), ImVec2(right, top + g_large_font->FontSize));
          text.Assign(lboard->title);

          top += g_large_font->FontSize + spacing_small;

          ImGui::PushFont(g_large_font);
          ImGui::RenderTextClipped(subtitle_bb.Min, subtitle_bb.Max, text.GetCharArray(),
                                   text.GetCharArray() + text.GetLength(), nullptr, ImVec2(0.0f, 0.0f), &subtitle_bb);
          ImGui::PopFont();

          text.Assign(lboard->description);
        }
        else
        {
          text.Clear();
        }
      }
      else
      {
        text.Fmt(Host::TranslateString("Achievements", "This game has {} leaderboards.").GetCharArray(),
                 leaderboard_count);
      }

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
      top += g_medium_font->FontSize + spacing_small;

      ImGui::PushFont(g_medium_font);
      ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, text.GetCharArray(),
                               text.GetCharArray() + text.GetLength(), nullptr, ImVec2(0.0f, 0.0f), &summary_bb);

      if (!Achievements::ChallengeModeActive())
      {
        const ImRect hardcore_warning_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
        top += g_medium_font->FontSize + spacing_small;

        ImGui::RenderTextClipped(
          hardcore_warning_bb.Min, hardcore_warning_bb.Max,
          Host::TranslateString(
            "Achievements", "Submitting scores is disabled because hardcore mode is off. Leaderboards are read-only."),
          nullptr, nullptr, ImVec2(0.0f, 0.0f), &hardcore_warning_bb);
      }

      ImGui::PopFont();
    }

    if (is_leaderboard_open)
    {
      pressed = MenuButtonFrame("legend", false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, &visible, &hovered, &bb.Min,
                                &bb.Max, 0, alpha);

      UNREFERENCED_VARIABLE(pressed);

      if (visible)
      {
        const Achievements::Leaderboard* lboard = Achievements::GetLeaderboardByID(s_open_leaderboard_id.value());

        const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
        float text_start_x = bb.Min.x + LayoutScale(15.0f) + padding;

        ImGui::PushFont(g_large_font);

        const ImRect rank_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(rank_bb.Min, rank_bb.Max, "Rank", nullptr, nullptr, ImVec2(0.0f, 0.0f), &rank_bb);
        text_start_x += rank_column_width + column_spacing;

        const ImRect user_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(user_bb.Min, user_bb.Max, "Name", nullptr, nullptr, ImVec2(0.0f, 0.0f), &user_bb);
        text_start_x += name_column_width + column_spacing;

        const ImRect score_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(score_bb.Min, score_bb.Max,
                                 lboard != nullptr && Achievements::IsLeaderboardTimeType(*lboard) ?
                                   Host::TranslateString("Achievements", "Time") :
                                   Host::TranslateString("Achievements", "Score"),
                                 nullptr, nullptr, ImVec2(0.0f, 0.0f), &score_bb);

        ImGui::PopFont();

        const float line_thickness = LayoutScale(1.0f);
        const float line_padding = LayoutScale(5.0f);
        const ImVec2 line_start(bb.Min.x, bb.Min.y + g_large_font->FontSize + line_padding);
        const ImVec2 line_end(bb.Max.x, line_start.y);
        ImGui::GetWindowDrawList()->AddLine(line_start, line_end, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            line_thickness);
      }
    }
  }
  EndFullscreenWindow();

  ImGui::SetNextWindowBgAlpha(alpha);

  if (!is_leaderboard_open)
  {
    if (BeginFullscreenWindow(ImVec2(0.0f, heading_height), ImVec2(display_size.x, display_size.y - heading_height),
                              "leaderboards", background, 0.0f, 0.0f, 0))
    {
      BeginMenuButtons();

      Achievements::EnumerateLeaderboards([](const Achievements::Leaderboard& lboard) -> bool {
        DrawLeaderboardListEntry(lboard);

        return true;
      });

      EndMenuButtons();
    }
    EndFullscreenWindow();
  }
  else
  {
    if (BeginFullscreenWindow(ImVec2(0.0f, heading_height), ImVec2(display_size.x, display_size.y - heading_height),
                              "leaderboard", background, 0.0f, 0.0f, 0))
    {
      BeginMenuButtons();

      const auto result = Achievements::TryEnumerateLeaderboardEntries(
        s_open_leaderboard_id.value(),
        [rank_column_width, name_column_width, column_spacing](const Achievements::LeaderboardEntry& lbEntry) -> bool {
          DrawLeaderboardEntry(lbEntry, rank_column_width, name_column_width, column_spacing);
          return true;
        });

      if (!result.has_value())
      {
        ImGui::PushFont(g_large_font);

        const ImVec2 pos_min(0.0f, heading_height);
        const ImVec2 pos_max(display_size.x, display_size.y);
        ImGui::RenderTextClipped(pos_min, pos_max,
                                 Host::TranslateString("Achievements", "Downloading leaderboard data, please wait..."),
                                 nullptr, nullptr, ImVec2(0.5f, 0.5f));

        ImGui::PopFont();
      }

      EndMenuButtons();
    }
    EndFullscreenWindow();
  }

  if (close_leaderboard_on_exit)
    s_open_leaderboard_id.reset();
}

#else

bool FullscreenUI::OpenAchievementsWindow()
{
  return false;
}

bool FullscreenUI::OpenLeaderboardsWindow()
{
  return false;
}

#endif

FullscreenUI::ProgressCallback::ProgressCallback(std::string name) : BaseProgressCallback(), m_name(std::move(name))
{
  ImGuiFullscreen::OpenBackgroundProgressDialog(m_name.c_str(), "", 0, 100, 0);
}

FullscreenUI::ProgressCallback::~ProgressCallback()
{
  ImGuiFullscreen::CloseBackgroundProgressDialog(m_name.c_str());
}

void FullscreenUI::ProgressCallback::PushState()
{
  BaseProgressCallback::PushState();
}

void FullscreenUI::ProgressCallback::PopState()
{
  BaseProgressCallback::PopState();
  Redraw(true);
}

void FullscreenUI::ProgressCallback::SetCancellable(bool cancellable)
{
  BaseProgressCallback::SetCancellable(cancellable);
  Redraw(true);
}

void FullscreenUI::ProgressCallback::SetTitle(const char* title)
{
  // todo?
}

void FullscreenUI::ProgressCallback::SetStatusText(const char* text)
{
  BaseProgressCallback::SetStatusText(text);
  Redraw(true);
}

void FullscreenUI::ProgressCallback::SetProgressRange(u32 range)
{
  u32 last_range = m_progress_range;

  BaseProgressCallback::SetProgressRange(range);

  if (m_progress_range != last_range)
    Redraw(false);
}

void FullscreenUI::ProgressCallback::SetProgressValue(u32 value)
{
  u32 lastValue = m_progress_value;

  BaseProgressCallback::SetProgressValue(value);

  if (m_progress_value != lastValue)
    Redraw(false);
}

void FullscreenUI::ProgressCallback::Redraw(bool force)
{
  const int percent =
    static_cast<int>((static_cast<float>(m_progress_value) / static_cast<float>(m_progress_range)) * 100.0f);
  if (percent == m_last_progress_percent && !force)
    return;

  m_last_progress_percent = percent;
  ImGuiFullscreen::UpdateBackgroundProgressDialog(
    m_name.c_str(), std::string(m_status_text.GetCharArray(), m_status_text.GetLength()), 0, 100, percent);
}

void FullscreenUI::ProgressCallback::DisplayError(const char* message)
{
  Log_ErrorPrint(message);
  Host::ReportErrorAsync("Error", message);
}

void FullscreenUI::ProgressCallback::DisplayWarning(const char* message)
{
  Log_WarningPrint(message);
}

void FullscreenUI::ProgressCallback::DisplayInformation(const char* message)
{
  Log_InfoPrint(message);
}

void FullscreenUI::ProgressCallback::DisplayDebugMessage(const char* message)
{
  Log_DebugPrint(message);
}

void FullscreenUI::ProgressCallback::ModalError(const char* message)
{
  Log_ErrorPrint(message);
  Host::ReportErrorAsync("Error", message);
}

bool FullscreenUI::ProgressCallback::ModalConfirmation(const char* message)
{
  return false;
}

void FullscreenUI::ProgressCallback::ModalInformation(const char* message)
{
  Log_InfoPrint(message);
}

void FullscreenUI::ProgressCallback::SetCancelled()
{
  if (m_cancellable)
    m_cancelled = true;
}

//////////////////////////////////////////////////////////////////////////
// Debug Menu
//////////////////////////////////////////////////////////////////////////

#if 0
void SetDebugMenuAllowed(bool allowed)
{
  s_debug_menu_allowed = allowed;
  UpdateDebugMenuVisibility();
}

void SetDebugMenuEnabled(bool enabled)
{
  s_host_interface->GetSettingsInterface()->SetBoolValue("Main", "ShowDebugMenu", enabled);
  s_host_interface->GetSettingsInterface()->Save();
  UpdateDebugMenuVisibility();
}

void UpdateDebugMenuVisibility()
{
  const bool enabled =
    s_debug_menu_allowed && s_host_interface->GetSettingsInterface()->GetBoolValue("Main", "ShowDebugMenu", false);
  if (s_debug_menu_enabled == enabled)
    return;

  const float size = enabled ? DPIScale(LAYOUT_MAIN_MENU_BAR_SIZE) : 0.0f;
  s_host_interface->GetDisplay()->SetDisplayTopMargin(static_cast<s32>(size));
  ImGuiFullscreen::SetMenuBarSize(size);
  ImGuiFullscreen::UpdateLayoutScale();
  if (ImGuiFullscreen::UpdateFonts())
    s_host_interface->GetDisplay()->UpdateImGuiFontTexture();
  s_debug_menu_enabled = enabled;
}

static void DrawDebugStats();
static void DrawDebugSystemMenu();
static void DrawDebugSettingsMenu();
static void DrawDebugDebugMenu();

void DrawDebugMenu()
{
  if (!ImGui::BeginMainMenuBar())
    return;

  if (ImGui::BeginMenu("System"))
  {
    DrawDebugSystemMenu();
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Settings"))
  {
    DrawDebugSettingsMenu();
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Debug"))
  {
    DrawDebugDebugMenu();
    ImGui::EndMenu();
  }

  DrawDebugStats();

  ImGui::EndMainMenuBar();
}

void DrawDebugStats()
{
  if (!System::IsShutdown())
  {
    const float framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale.x;
    const float framebuffer_width = ImGui::GetIO().DisplaySize.x;

    if (System::IsPaused())
    {
      ImGui::SetCursorPosX(framebuffer_width - (50.0f * framebuffer_scale));
      ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Paused");
    }
    else
    {
      const auto [display_width, display_height] = g_gpu->GetEffectiveDisplayResolution();
      ImGui::SetCursorPosX(framebuffer_width - (580.0f * framebuffer_scale));
      ImGui::Text("%ux%u (%s)", display_width, display_height,
                  g_gpu->IsInterlacedDisplayEnabled() ? "interlaced" : "progressive");

      ImGui::SetCursorPosX(framebuffer_width - (420.0f * framebuffer_scale));
      ImGui::Text("Average: %.2fms", System::GetAverageFrameTime());

      ImGui::SetCursorPosX(framebuffer_width - (310.0f * framebuffer_scale));
      ImGui::Text("Worst: %.2fms", System::GetWorstFrameTime());

      ImGui::SetCursorPosX(framebuffer_width - (210.0f * framebuffer_scale));

      const float speed = System::GetEmulationSpeed();
      const u32 rounded_speed = static_cast<u32>(std::round(speed));
      if (speed < 90.0f)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%u%%", rounded_speed);
      else if (speed < 110.0f)
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%u%%", rounded_speed);
      else
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%u%%", rounded_speed);

      ImGui::SetCursorPosX(framebuffer_width - (165.0f * framebuffer_scale));
      ImGui::Text("FPS: %.2f", System::GetFPS());

      ImGui::SetCursorPosX(framebuffer_width - (80.0f * framebuffer_scale));
      ImGui::Text("VPS: %.2f", System::GetVPS());
    }
  }
}

void DrawDebugSystemMenu()
{
  const bool system_enabled = static_cast<bool>(!System::IsShutdown());

  if (ImGui::MenuItem("Start Disc", nullptr, false, !system_enabled))
  {
    DoStartFile();
    ClearImGuiFocus();
  }

  if (ImGui::MenuItem("Start BIOS", nullptr, false, !system_enabled))
  {
    DoStartBIOS();
    ClearImGuiFocus();
  }

  ImGui::Separator();

  if (ImGui::MenuItem("Power Off", nullptr, false, system_enabled))
  {
    DoPowerOff();
    ClearImGuiFocus();
  }

  if (ImGui::MenuItem("Reset", nullptr, false, system_enabled))
  {
    DoReset();
    ClearImGuiFocus();
  }

  if (ImGui::MenuItem("Pause", nullptr, System::IsPaused(), system_enabled))
  {
    DoPause();
    ClearImGuiFocus();
  }

  ImGui::Separator();

  if (ImGui::MenuItem("Change Disc", nullptr, false, system_enabled))
  {
    DoChangeDisc();
    ClearImGuiFocus();
  }

  if (ImGui::MenuItem("Remove Disc", nullptr, false, system_enabled))
  {
    s_host_interface->RunLater([]() { System::RemoveMedia(); });
    ClearImGuiFocus();
  }

  ImGui::Separator();

  if (ImGui::BeginMenu("Load State", !IsCheevosHardcoreModeActive()))
  {
    for (u32 i = 1; i <= CommonHostInterface::GLOBAL_SAVE_STATE_SLOTS; i++)
    {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "State %u", i);
      if (ImGui::MenuItem(buf))
      {
        s_host_interface->RunLater([i]() { s_host_interface->LoadState(true, i); });
        ClearImGuiFocus();
      }
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Save State", system_enabled))
  {
    for (u32 i = 1; i <= CommonHostInterface::GLOBAL_SAVE_STATE_SLOTS; i++)
    {
      TinyString buf;
      buf.Format("State %u", i);
      if (ImGui::MenuItem(buf))
      {
        s_host_interface->RunLater([i]() { s_host_interface->SaveState(true, i); });
        ClearImGuiFocus();
      }
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();

  if (ImGui::BeginMenu("Cheats", system_enabled && !IsCheevosHardcoreModeActive()))
  {
    const bool has_cheat_file = System::HasCheatList();
    if (ImGui::BeginMenu("Enabled Cheats", has_cheat_file))
    {
      CheatList* cl = System::GetCheatList();
      for (u32 i = 0; i < cl->GetCodeCount(); i++)
      {
        const CheatCode& cc = cl->GetCode(i);
        if (ImGui::MenuItem(cc.description.c_str(), nullptr, cc.enabled, true))
          s_host_interface->SetCheatCodeState(i, !cc.enabled, g_settings.auto_load_cheats);
      }

      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Apply Cheat", has_cheat_file))
    {
      CheatList* cl = System::GetCheatList();
      for (u32 i = 0; i < cl->GetCodeCount(); i++)
      {
        const CheatCode& cc = cl->GetCode(i);
        if (ImGui::MenuItem(cc.description.c_str()))
          s_host_interface->ApplyCheatCode(i);
      }

      ImGui::EndMenu();
    }

    ImGui::EndMenu();
  }

  ImGui::Separator();

  if (ImGui::MenuItem("Exit"))
    s_host_interface->RequestExit();
}

void DrawDebugSettingsMenu()
{
  bool settings_changed = false;

  if (ImGui::BeginMenu("CPU Execution Mode"))
  {
    const CPUExecutionMode current = s_settings_copy.cpu_execution_mode;
    for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetCPUExecutionModeDisplayName(static_cast<CPUExecutionMode>(i)), nullptr,
                          i == static_cast<u32>(current)))
      {
        s_settings_copy.cpu_execution_mode = static_cast<CPUExecutionMode>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  if (ImGui::MenuItem("CPU Clock Control", nullptr, &s_settings_copy.cpu_overclock_enable))
  {
    settings_changed = true;
    s_settings_copy.UpdateOverclockActive();
  }

  if (ImGui::BeginMenu("CPU Clock Speed"))
  {
    static constexpr auto values = make_array(10u, 25u, 50u, 75u, 100u, 125u, 150u, 175u, 200u, 225u, 250u, 275u,
                                              300u, 350u, 400u, 450u, 500u, 600u, 700u, 800u);
    const u32 percent = s_settings_copy.GetCPUOverclockPercent();
    for (u32 value : values)
    {
      if (ImGui::MenuItem(TinyString::FromFormat("%u%%", value), nullptr, percent == value))
      {
        s_settings_copy.SetCPUOverclockPercent(value);
        s_settings_copy.UpdateOverclockActive();
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  settings_changed |=
    ImGui::MenuItem("Recompiler Memory Exceptions", nullptr, &s_settings_copy.cpu_recompiler_memory_exceptions);
  settings_changed |=
    ImGui::MenuItem("Recompiler Block Linking", nullptr, &s_settings_copy.cpu_recompiler_block_linking);
  if (ImGui::BeginMenu("Recompiler Fastmem"))
  {
    for (u32 i = 0; i < static_cast<u32>(CPUFastmemMode::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetCPUFastmemModeDisplayName(static_cast<CPUFastmemMode>(i)), nullptr,
                          s_settings_copy.cpu_fastmem_mode == static_cast<CPUFastmemMode>(i)))
      {
        s_settings_copy.cpu_fastmem_mode = static_cast<CPUFastmemMode>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  settings_changed |= ImGui::MenuItem("Recompiler ICache", nullptr, &s_settings_copy.cpu_recompiler_icache);

  ImGui::Separator();

  if (ImGui::BeginMenu("Renderer"))
  {
    const GPURenderer current = s_settings_copy.gpu_renderer;
    for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetRendererDisplayName(static_cast<GPURenderer>(i)), nullptr,
                          i == static_cast<u32>(current)))
      {
        s_settings_copy.gpu_renderer = static_cast<GPURenderer>(i);
        settings_changed = true;
      }
    }

    settings_changed |= ImGui::MenuItem("GPU on Thread", nullptr, &s_settings_copy.gpu_use_thread);

    ImGui::EndMenu();
  }

  if (ImGui::MenuItem("Toggle Fullscreen"))
    s_host_interface->RunLater([] { s_host_interface->SetFullscreen(!s_host_interface->IsFullscreen()); });

  if (ImGui::BeginMenu("Resize to Game", System::IsValid()))
  {
    static constexpr auto scales = make_array(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    for (const u32 scale : scales)
    {
      if (ImGui::MenuItem(TinyString::FromFormat("%ux Scale", scale)))
        s_host_interface->RunLater(
          [scale]() { s_host_interface->RequestRenderWindowScale(static_cast<float>(scale)); });
    }

    ImGui::EndMenu();
  }

  settings_changed |= ImGui::MenuItem("VSync", nullptr, &s_settings_copy.video_sync_enabled);

  ImGui::Separator();

  if (ImGui::BeginMenu("Resolution Scale"))
  {
    const u32 current_internal_resolution = s_settings_copy.gpu_resolution_scale;
    for (u32 scale = 1; scale <= GPU::MAX_RESOLUTION_SCALE; scale++)
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%ux (%ux%u)", scale, scale * VRAM_WIDTH, scale * VRAM_HEIGHT);

      if (ImGui::MenuItem(buf, nullptr, current_internal_resolution == scale))
      {
        s_settings_copy.gpu_resolution_scale = scale;
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Multisampling"))
  {
    const u32 current_multisamples = s_settings_copy.gpu_multisamples;
    const bool current_ssaa = s_settings_copy.gpu_per_sample_shading;

    if (ImGui::MenuItem("None", nullptr, (current_multisamples == 1)))
    {
      s_settings_copy.gpu_multisamples = 1;
      s_settings_copy.gpu_per_sample_shading = false;
      settings_changed = true;
    }

    for (u32 i = 2; i <= 32; i *= 2)
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%ux MSAA", i);

      if (ImGui::MenuItem(buf, nullptr, (current_multisamples == i && !current_ssaa)))
      {
        s_settings_copy.gpu_multisamples = i;
        s_settings_copy.gpu_per_sample_shading = false;
        settings_changed = true;
      }
    }

    for (u32 i = 2; i <= 32; i *= 2)
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%ux SSAA", i);

      if (ImGui::MenuItem(buf, nullptr, (current_multisamples == i && current_ssaa)))
      {
        s_settings_copy.gpu_multisamples = i;
        s_settings_copy.gpu_per_sample_shading = true;
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("PGXP"))
  {
    settings_changed |= ImGui::MenuItem("PGXP Enabled", nullptr, &s_settings_copy.gpu_pgxp_enable);
    settings_changed |=
      ImGui::MenuItem("PGXP Culling", nullptr, &s_settings_copy.gpu_pgxp_culling, s_settings_copy.gpu_pgxp_enable);
    settings_changed |=
      ImGui::MenuItem("PGXP Texture Correction", nullptr, &s_settings_copy.gpu_pgxp_texture_correction,
                      s_settings_copy.gpu_pgxp_enable);
    settings_changed |= ImGui::MenuItem("PGXP Vertex Cache", nullptr, &s_settings_copy.gpu_pgxp_vertex_cache,
                                        s_settings_copy.gpu_pgxp_enable);
    settings_changed |= ImGui::MenuItem("PGXP CPU Instructions", nullptr, &s_settings_copy.gpu_pgxp_cpu,
                                        s_settings_copy.gpu_pgxp_enable);
    settings_changed |= ImGui::MenuItem("PGXP Preserve Projection Precision", nullptr,
                                        &s_settings_copy.gpu_pgxp_preserve_proj_fp, s_settings_copy.gpu_pgxp_enable);
    settings_changed |= ImGui::MenuItem("PGXP Depth Buffer", nullptr, &s_settings_copy.gpu_pgxp_depth_buffer,
                                        s_settings_copy.gpu_pgxp_enable);
    ImGui::EndMenu();
  }

  settings_changed |= ImGui::MenuItem("True (24-Bit) Color", nullptr, &s_settings_copy.gpu_true_color);
  settings_changed |= ImGui::MenuItem("Scaled Dithering", nullptr, &s_settings_copy.gpu_scaled_dithering);

  if (ImGui::BeginMenu("Texture Filtering"))
  {
    const GPUTextureFilter current = s_settings_copy.gpu_texture_filter;
    for (u32 i = 0; i < static_cast<u32>(GPUTextureFilter::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetTextureFilterDisplayName(static_cast<GPUTextureFilter>(i)), nullptr,
                          i == static_cast<u32>(current)))
      {
        s_settings_copy.gpu_texture_filter = static_cast<GPUTextureFilter>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("Disable Interlacing", nullptr, &s_settings_copy.gpu_disable_interlacing);
  settings_changed |= ImGui::MenuItem("Widescreen Hack", nullptr, &s_settings_copy.gpu_widescreen_hack);
  settings_changed |= ImGui::MenuItem("Force NTSC Timings", nullptr, &s_settings_copy.gpu_force_ntsc_timings);
  settings_changed |=
    ImGui::MenuItem("24-Bit Chroma Smoothing", nullptr, &s_settings_copy.gpu_24bit_chroma_smoothing);

  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("Display Linear Filtering", nullptr, &s_settings_copy.display_linear_filtering);
  settings_changed |= ImGui::MenuItem("Display Integer Scaling", nullptr, &s_settings_copy.display_integer_scaling);

  if (ImGui::BeginMenu("Aspect Ratio"))
  {
    for (u32 i = 0; i < static_cast<u32>(DisplayAspectRatio::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetDisplayAspectRatioName(static_cast<DisplayAspectRatio>(i)), nullptr,
                          s_settings_copy.display_aspect_ratio == static_cast<DisplayAspectRatio>(i)))
      {
        s_settings_copy.display_aspect_ratio = static_cast<DisplayAspectRatio>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Crop Mode"))
  {
    for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetDisplayCropModeDisplayName(static_cast<DisplayCropMode>(i)), nullptr,
                          s_settings_copy.display_crop_mode == static_cast<DisplayCropMode>(i)))
      {
        s_settings_copy.display_crop_mode = static_cast<DisplayCropMode>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Downsample Mode"))
  {
    for (u32 i = 0; i < static_cast<u32>(GPUDownsampleMode::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetDownsampleModeDisplayName(static_cast<GPUDownsampleMode>(i)), nullptr,
                          s_settings_copy.gpu_downsample_mode == static_cast<GPUDownsampleMode>(i)))
      {
        s_settings_copy.gpu_downsample_mode = static_cast<GPUDownsampleMode>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  settings_changed |= ImGui::MenuItem("Force 4:3 For 24-bit", nullptr, &s_settings_copy.display_force_4_3_for_24bit);

  ImGui::Separator();

  if (ImGui::MenuItem("Dump Audio", nullptr, s_host_interface->IsDumpingAudio(), System::IsValid()))
  {
    if (!s_host_interface->IsDumpingAudio())
      s_host_interface->StartDumpingAudio();
    else
      s_host_interface->StopDumpingAudio();
  }

  if (ImGui::MenuItem("Save Screenshot"))
    s_host_interface->RunLater([]() { s_host_interface->SaveScreenshot(); });

  if (settings_changed)
    s_host_interface->RunLater(SaveAndApplySettings);
}

void DrawDebugDebugMenu()
{
  const bool system_valid = System::IsValid();
  Settings::DebugSettings& debug_settings = g_settings.debugging;
  bool settings_changed = false;

  if (ImGui::BeginMenu("Log Level"))
  {
    for (u32 i = LOGLEVEL_NONE; i < LOGLEVEL_COUNT; i++)
    {
      if (ImGui::MenuItem(Settings::GetLogLevelDisplayName(static_cast<LOGLEVEL>(i)), nullptr,
                          g_settings.log_level == static_cast<LOGLEVEL>(i)))
      {
        s_settings_copy.log_level = static_cast<LOGLEVEL>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  settings_changed |= ImGui::MenuItem("Log To Console", nullptr, &s_settings_copy.log_to_console);
  settings_changed |= ImGui::MenuItem("Log To Debug", nullptr, &s_settings_copy.log_to_debug);
  settings_changed |= ImGui::MenuItem("Log To File", nullptr, &s_settings_copy.log_to_file);

  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("Disable All Enhancements", nullptr, &s_settings_copy.disable_all_enhancements);
  settings_changed |= ImGui::MenuItem("Dump CPU to VRAM Copies", nullptr, &debug_settings.dump_cpu_to_vram_copies);
  settings_changed |= ImGui::MenuItem("Dump VRAM to CPU Copies", nullptr, &debug_settings.dump_vram_to_cpu_copies);

  if (ImGui::MenuItem("CPU Trace Logging", nullptr, CPU::IsTraceEnabled(), system_valid))
  {
    if (!CPU::IsTraceEnabled())
      CPU::StartTrace();
    else
      CPU::StopTrace();
  }

  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("Show VRAM", nullptr, &debug_settings.show_vram);
  settings_changed |= ImGui::MenuItem("Show GPU State", nullptr, &debug_settings.show_gpu_state);
  settings_changed |= ImGui::MenuItem("Show CDROM State", nullptr, &debug_settings.show_cdrom_state);
  settings_changed |= ImGui::MenuItem("Show SPU State", nullptr, &debug_settings.show_spu_state);
  settings_changed |= ImGui::MenuItem("Show Timers State", nullptr, &debug_settings.show_timers_state);
  settings_changed |= ImGui::MenuItem("Show MDEC State", nullptr, &debug_settings.show_mdec_state);
  settings_changed |= ImGui::MenuItem("Show DMA State", nullptr, &debug_settings.show_dma_state);

  if (settings_changed)
  {
    // have to apply it to the copy too, otherwise it won't save
    Settings::DebugSettings& debug_settings_copy = s_settings_copy.debugging;
    debug_settings_copy.show_gpu_state = debug_settings.show_gpu_state;
    debug_settings_copy.show_vram = debug_settings.show_vram;
    debug_settings_copy.dump_cpu_to_vram_copies = debug_settings.dump_cpu_to_vram_copies;
    debug_settings_copy.dump_vram_to_cpu_copies = debug_settings.dump_vram_to_cpu_copies;
    debug_settings_copy.show_cdrom_state = debug_settings.show_cdrom_state;
    debug_settings_copy.show_spu_state = debug_settings.show_spu_state;
    debug_settings_copy.show_timers_state = debug_settings.show_timers_state;
    debug_settings_copy.show_mdec_state = debug_settings.show_mdec_state;
    debug_settings_copy.show_dma_state = debug_settings.show_dma_state;
    s_host_interface->RunLater(SaveAndApplySettings);
  }
}
#endif
