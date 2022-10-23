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
#include "common/threading.h"
#include "common_host.h"
#include "core/bios.h"
#include "core/cheats.h"
#include "core/controller.h"
#include "core/cpu_core.h"
#include "core/gpu.h"
#include "core/host.h"
#include "core/host_display.h"
#include "core/host_settings.h"
#include "core/memory_card_image.h"
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
#include "postprocessing_chain.h"
#include "scmversion/scmversion.h"
#include "util/ini_settings_interface.h"
#include <atomic>
#include <bitset>
#include <thread>
#include <unordered_map>
Log_SetChannel(FullscreenUI);

#ifdef WITH_CHEEVOS
#include "achievements.h"
#endif

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
using ImGuiFullscreen::DrawShadowedText;
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
using ImGuiFullscreen::MenuButtonWithoutSummary;
using ImGuiFullscreen::MenuButtonWithValue;
using ImGuiFullscreen::MenuHeading;
using ImGuiFullscreen::MenuHeadingButton;
using ImGuiFullscreen::MenuImageButton;
using ImGuiFullscreen::ModAlpha;
using ImGuiFullscreen::MulAlpha;
using ImGuiFullscreen::NavButton;
using ImGuiFullscreen::NavTitle;
using ImGuiFullscreen::OpenChoiceDialog;
using ImGuiFullscreen::OpenConfirmMessageDialog;
using ImGuiFullscreen::OpenFileSelector;
using ImGuiFullscreen::OpenInputStringDialog;
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

#ifndef __ANDROID__
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
  Console,
  Emulation,
  BIOS,
  Controller,
  Hotkey,
  MemoryCards,
  Display,
  PostProcessing,
  Audio,
  Achievements,
  Advanced,
  Count
};

enum class GameListPage
{
  Grid,
  List,
  Settings,
  Count
};

//////////////////////////////////////////////////////////////////////////
// Utility
//////////////////////////////////////////////////////////////////////////
static std::string TimeToPrintableString(time_t t);
static void StartAsyncOp(std::function<void(::ProgressCallback*)> callback, std::string name);
static void AsyncOpThreadEntryPoint(std::function<void(::ProgressCallback*)> callback,
                                    FullscreenUI::ProgressCallback* progress);
static void CancelAsyncOpWithName(const std::string_view& name);
static void CancelAsyncOps();

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////
static void ToggleTheme();
static void PauseForMenuOpen();
static void ClosePauseMenu();
static void OpenPauseSubMenu(PauseSubMenu submenu);
static void ReturnToMainWindow();
static void DrawLandingWindow();
static void DrawPauseMenu(MainWindowType type);
static void ExitFullscreenAndOpenURL(const std::string_view& url);
static void CopyTextToClipboard(std::string title, const std::string_view& text);
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

// async operations (e.g. cover downloads)
using AsyncOpEntry = std::pair<std::thread, std::unique_ptr<FullscreenUI::ProgressCallback>>;
static std::mutex s_async_op_mutex;
static std::deque<AsyncOpEntry> s_async_ops;

//////////////////////////////////////////////////////////////////////////
// Resources
//////////////////////////////////////////////////////////////////////////
static bool LoadResources();
static void DestroyResources();

static std::shared_ptr<GPUTexture> s_app_icon_texture;
static std::array<std::shared_ptr<GPUTexture>, static_cast<u32>(GameDatabase::CompatibilityRating::Count)>
  s_game_compatibility_textures;
static std::shared_ptr<GPUTexture> s_fallback_disc_texture;
static std::shared_ptr<GPUTexture> s_fallback_exe_texture;
static std::shared_ptr<GPUTexture> s_fallback_psf_texture;
static std::shared_ptr<GPUTexture> s_fallback_playlist_texture;
static std::vector<std::unique_ptr<GPUTexture>> s_cleanup_textures;

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
static void DoShutdown(bool save_state);
static void DoReset();
static void DoChangeDiscFromFile();
static void DoChangeDisc();
static void DoRequestExit();
static void DoToggleFullscreen();
static void DoCheatsMenu();
static void DoToggleAnalogMode();

//////////////////////////////////////////////////////////////////////////
// Settings
//////////////////////////////////////////////////////////////////////////

static constexpr double INPUT_BINDING_TIMEOUT_SECONDS = 5.0;

static void SwitchToSettings();
static void SwitchToGameSettings();
static void SwitchToGameSettings(const GameList::Entry* entry);
static void SwitchToGameSettingsForPath(const std::string& path);
static void SwitchToGameSettingsForSerial(const std::string_view& serial);
static void DrawSettingsWindow();
static void DrawSummarySettingsPage();
static void DrawInterfaceSettingsPage();
static void DrawBIOSSettingsPage();
static void DrawConsoleSettingsPage();
static void DrawEmulationSettingsPage();
static void DrawDisplaySettingsPage();
static void DrawPostProcessingSettingsPage();
static void DrawAudioSettingsPage();
static void DrawMemoryCardSettingsPage();
static void DrawControllerSettingsPage();
static void DrawHotkeySettingsPage();
static void DrawAchievementsSettingsPage();
static void DrawAchievementsLoginWindow();
static void DrawAdvancedSettingsPage();

static bool IsEditingGameSettings(SettingsInterface* bsi);
static SettingsInterface* GetEditingSettingsInterface();
static SettingsInterface* GetEditingSettingsInterface(bool game_settings);
static void SetSettingsChanged(SettingsInterface* bsi);
static bool GetEffectiveBoolSetting(SettingsInterface* bsi, const char* section, const char* key, bool default_value);
static s32 GetEffectiveIntSetting(SettingsInterface* bsi, const char* section, const char* key, s32 default_value);
static u32 GetEffectiveUIntSetting(SettingsInterface* bsi, const char* section, const char* key, u32 default_value);
static float GetEffectiveFloatSetting(SettingsInterface* bsi, const char* section, const char* key,
                                      float default_value);
static std::string GetEffectiveStringSetting(SettingsInterface* bsi, const char* section, const char* key,
                                             const char* default_value);
static void DoCopyGameSettings();
static void DoClearGameSettings();
static void CopyGlobalControllerSettingsToGame();
static void ResetControllerSettings();
static void DoLoadInputProfile();
static void DoSaveInputProfile();
static void DoSaveInputProfile(const std::string& name);

static bool DrawToggleSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                              const char* key, bool default_value, bool enabled = true, bool allow_tristate = true,
                              float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = g_large_font,
                              ImFont* summary_font = g_medium_font);
static void DrawIntListSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                               const char* key, int default_value, const char* const* options, size_t option_count,
                               int option_offset = 0, bool enabled = true,
                               float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = g_large_font,
                               ImFont* summary_font = g_medium_font);
static void DrawIntRangeSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                                const char* key, int default_value, int min_value, int max_value,
                                const char* format = "%d", bool enabled = true,
                                float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = g_large_font,
                                ImFont* summary_font = g_medium_font);
static void DrawIntSpinBoxSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                                  const char* key, int default_value, int min_value, int max_value, int step_value,
                                  const char* format = "%d", bool enabled = true,
                                  float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                  ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
static void DrawFloatRangeSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                                  const char* key, float default_value, float min_value, float max_value,
                                  const char* format = "%f", float multiplier = 1.0f, bool enabled = true,
                                  float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                  ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
static void DrawIntRectSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                               const char* left_key, int default_left, const char* top_key, int default_top,
                               const char* right_key, int default_right, const char* bottom_key, int default_bottom,
                               int min_value, int max_value, const char* format = "%d", bool enabled = true,
                               float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = g_large_font,
                               ImFont* summary_font = g_medium_font);
static void DrawStringListSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                                  const char* key, const char* default_value, const char* const* options,
                                  const char* const* option_values, size_t option_count, bool enabled = true,
                                  float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                  ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
template<typename DataType, typename SizeType>
static void DrawEnumSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                            const char* key, DataType default_value,
                            std::optional<DataType> (*from_string_function)(const char* str),
                            const char* (*to_string_function)(DataType value),
                            const char* (*to_display_string_function)(DataType value), SizeType option_count,
                            bool enabled = true, float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                            ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
static void DrawFloatListSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                                 const char* key, float default_value, const char* const* options,
                                 const float* option_values, size_t option_count, bool enabled = true,
                                 float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = g_large_font,
                                 ImFont* summary_font = g_medium_font);
static void DrawFolderSetting(SettingsInterface* bsi, const char* title, const char* section, const char* key,
                              const std::string& runtime_var, float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                              ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);

static void PopulateGraphicsAdapterList();
static void PopulateGameListDirectoryCache(SettingsInterface* si);
static void PopulatePostProcessingChain();
static void SavePostProcessingChain();
static void BeginInputBinding(SettingsInterface* bsi, Controller::ControllerBindingType type,
                              const std::string_view& section, const std::string_view& key,
                              const std::string_view& display_name);
static void DrawInputBindingWindow();
static void DrawInputBindingButton(SettingsInterface* bsi, Controller::ControllerBindingType type, const char* section,
                                   const char* name, const char* display_name, bool show_type = true);
static void ClearInputBindingVariables();
static void StartAutomaticBinding(u32 port);

static SettingsPage s_settings_page = SettingsPage::Interface;
static std::unique_ptr<INISettingsInterface> s_game_settings_interface;
static std::unique_ptr<GameList::Entry> s_game_settings_entry;
static std::vector<std::pair<std::string, bool>> s_game_list_directories_cache;
static std::vector<std::string> s_graphics_adapter_list_cache;
static std::vector<std::string> s_fullscreen_mode_list_cache;
static FrontendCommon::PostProcessingChain s_postprocessing_chain;
static std::vector<const HotkeyInfo*> s_hotkey_list_cache;
static std::atomic_bool s_settings_changed{false};
static std::atomic_bool s_game_settings_changed{false};
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
  std::unique_ptr<GPUTexture> preview_texture;
  time_t timestamp;
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
static void DrawSaveStateSelector(bool is_loading);
static bool OpenLoadStateSelectorForGameResume(const GameList::Entry* entry);
static void DrawResumeStateSelector();
static void DoLoadState(std::string path);
static void DoSaveState(s32 slot, bool global);

static std::vector<SaveStateListEntry> s_save_state_selector_slots;
static std::string s_save_state_selector_game_path;
static s32 s_save_state_selector_submenu_index = -1;
static bool s_save_state_selector_open = false;
static bool s_save_state_selector_loading = true;
static bool s_save_state_selector_resuming = false;

//////////////////////////////////////////////////////////////////////////
// Game List
//////////////////////////////////////////////////////////////////////////
static void DrawGameListWindow();
static void DrawCoverDownloaderWindow();
static void DrawGameList(const ImVec2& heading_size);
static void DrawGameGrid(const ImVec2& heading_size);
static void HandleGameListActivate(const GameList::Entry* entry);
static void HandleGameListOptions(const GameList::Entry* entry);
static void DrawGameListSettingsPage(const ImVec2& heading_size);
static void SwitchToGameList();
static void PopulateGameListEntryList();
static GPUTexture* GetTextureForGameListEntryType(GameList::EntryType type);
static GPUTexture* GetGameListCover(const GameList::Entry* entry);
static GPUTexture* GetCoverForCurrentGame();

// Lazily populated cover images.
static std::unordered_map<std::string, std::string> s_cover_image_map;
static std::vector<const GameList::Entry*> s_game_list_sorted_entries;
static GameListPage s_game_list_page = GameListPage::Grid;

#ifdef WITH_CHEEVOS
//////////////////////////////////////////////////////////////////////////
// Achievements
//////////////////////////////////////////////////////////////////////////
static void DrawAchievementsWindow();
static void DrawAchievement(const Achievements::Achievement& cheevo);
static void DrawPrimedAchievementsIcons();
static void DrawPrimedAchievementsList();
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

void FullscreenUI::StartAsyncOp(std::function<void(::ProgressCallback*)> callback, std::string name)
{
  CancelAsyncOpWithName(name);

  std::unique_lock lock(s_async_op_mutex);
  std::unique_ptr<FullscreenUI::ProgressCallback> progress(
    std::make_unique<FullscreenUI::ProgressCallback>(std::move(name)));
  std::thread thread(AsyncOpThreadEntryPoint, std::move(callback), progress.get());
  s_async_ops.emplace_back(std::move(thread), std::move(progress));
}

void FullscreenUI::CancelAsyncOpWithName(const std::string_view& name)
{
  std::unique_lock lock(s_async_op_mutex);
  for (auto iter = s_async_ops.begin(); iter != s_async_ops.end(); ++iter)
  {
    if (name != iter->second->GetName())
      continue;

    // move the thread out so it doesn't detach itself, then join
    std::unique_ptr<FullscreenUI::ProgressCallback> progress(std::move(iter->second));
    std::thread thread(std::move(iter->first));
    progress->SetCancelled();
    s_async_ops.erase(iter);
    lock.unlock();
    if (thread.joinable())
      thread.join();
    lock.lock();
    break;
  }
}

void FullscreenUI::CancelAsyncOps()
{
  std::unique_lock lock(s_async_op_mutex);
  while (!s_async_ops.empty())
  {
    auto iter = s_async_ops.begin();

    // move the thread out so it doesn't detach itself, then join
    std::unique_ptr<FullscreenUI::ProgressCallback> progress(std::move(iter->second));
    std::thread thread(std::move(iter->first));
    progress->SetCancelled();
    s_async_ops.erase(iter);
    lock.unlock();
    if (thread.joinable())
      thread.join();
    lock.lock();
  }
}

void FullscreenUI::AsyncOpThreadEntryPoint(std::function<void(::ProgressCallback*)> callback,
                                           FullscreenUI::ProgressCallback* progress)
{
  Threading::SetNameOfCurrentThread(fmt::format("{} Async Op", progress->GetName()).c_str());

  callback(progress);

  // if we were removed from the list, it means we got cancelled, and the main thread is blocking
  std::unique_lock lock(s_async_op_mutex);
  for (auto iter = s_async_ops.begin(); iter != s_async_ops.end(); ++iter)
  {
    if (iter->second.get() == progress)
    {
      iter->first.detach();
      s_async_ops.erase(iter);
      break;
    }
  }
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

  ImGuiFullscreen::SetTheme(Host::GetBaseBoolSettingValue("Main", "UseLightFullscreenUITheme", false));
  ImGuiFullscreen::UpdateLayoutScale();

  if (!ImGuiManager::AddFullscreenFontsIfMissing() || !ImGuiFullscreen::Initialize("images/placeholder.png") ||
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

  if (!System::IsValid())
    SwitchToLanding();

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

void FullscreenUI::CheckForConfigChanges(const Settings& old_settings)
{
  if (!IsInitialized())
    return;

#ifdef WITH_CHEEVOS
  // If achievements got disabled, we might have the menu open...
  // That means we're going to be reading achievement state.
  if (old_settings.achievements_enabled && !g_settings.achievements_enabled)
  {
    if (s_current_main_window == MainWindowType::Achievements || s_current_main_window == MainWindowType::Leaderboards)
      ReturnToMainWindow();
  }
#endif
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
  // noop
}

void FullscreenUI::OnSystemResumed()
{
  // get rid of pause menu if we unpaused another way
  if (s_current_main_window == MainWindowType::PauseMenu)
    ClosePauseMenu();
}

void FullscreenUI::OnSystemDestroyed()
{
  if (!IsInitialized())
    return;

  g_host_display->SetVSync(true);
  s_pause_menu_was_open = false;
  SwitchToLanding();
}

void FullscreenUI::OnRunningGameChanged()
{
  if (!IsInitialized())
    return;

  const std::string& path = System::GetRunningPath();
  const std::string& serial = System::GetRunningSerial();
  if (!serial.empty())
    s_current_game_subtitle = fmt::format("{0} - {1}", serial, Path::GetFileName(path));
  else
    s_current_game_subtitle = {};
}

void FullscreenUI::ToggleTheme()
{
  const bool new_light = !Host::GetBaseBoolSettingValue("Main", "UseLightFullscreenUITheme", false);
  Host::SetBaseBoolSettingValue("Main", "UseLightFullscreenUITheme", new_light);
  Host::CommitBaseSettingChanges();
  ImGuiFullscreen::SetTheme(new_light);
}

void FullscreenUI::PauseForMenuOpen()
{
  s_was_paused_on_quick_menu_open = (System::GetState() == System::State::Paused);
  if (g_settings.pause_on_menu && !s_was_paused_on_quick_menu_open)
  {
    Host::RunOnCPUThread([]() {
      System::PauseSystem(true);

      // force vsync on when pausing
      if (g_host_display)
        g_host_display->SetVSync(true);
    });
  }

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
  CancelAsyncOps();
  CloseSaveStateSelector();
  s_cover_image_map.clear();
  s_game_list_sorted_entries = {};
  s_game_list_directories_cache = {};
  s_postprocessing_chain.ClearStages();
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

  for (std::unique_ptr<GPUTexture>& tex : s_cleanup_textures)
    tex.reset();
  s_cleanup_textures.clear();
  ImGuiFullscreen::UploadAsyncTextures();

  ImGuiFullscreen::BeginLayout();

#ifdef WITH_CHEEVOS
  // Primed achievements must come first, because we don't want the pause screen to be behind them.
  if (g_settings.achievements_primed_indicators && s_current_main_window == MainWindowType::None &&
      Achievements::GetPrimedAchievementCount() > 0)
  {
    DrawPrimedAchievementsIcons();
  }
#endif

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
  {
    if (s_save_state_selector_resuming)
      DrawResumeStateSelector();
    else
      DrawSaveStateSelector(s_save_state_selector_loading);
  }

  if (s_about_window_open)
    DrawAboutWindow();

  if (s_input_binding_type != Controller::ControllerBindingType::Unknown)
    DrawInputBindingWindow();

  ImGuiFullscreen::EndLayout();

  if (s_settings_changed.load(std::memory_order_relaxed))
  {
    Host::CommitBaseSettingChanges();
    Host::RunOnCPUThread([]() { System::ApplySettings(false); });
    s_settings_changed.store(false, std::memory_order_release);
  }
  if (s_game_settings_changed.load(std::memory_order_relaxed))
  {
    if (s_game_settings_interface)
    {
      s_game_settings_interface->Save();
      if (System::IsValid())
        Host::RunOnCPUThread([]() { System::ReloadGameSettings(false); });
    }
    s_game_settings_changed.store(false, std::memory_order_release);
  }

  ImGuiFullscreen::ResetCloseMenuIfNeeded();
}

void FullscreenUI::InvalidateCoverCache()
{
  if (!IsInitialized())
    return;

  Host::RunOnCPUThread([]() { s_cover_image_map.clear(); });
}

void FullscreenUI::ReturnToMainWindow()
{
  if (s_pause_menu_was_open)
    ClosePauseMenu();

  s_current_main_window = System::IsValid() ? MainWindowType::None : MainWindowType::Landing;
}

bool FullscreenUI::LoadResources()
{
  s_app_icon_texture = LoadTexture("images/duck.png");

  s_fallback_disc_texture = LoadTexture("fullscreenui/media-cdrom.png");
  s_fallback_exe_texture = LoadTexture("fullscreenui/applications-system.png");
  s_fallback_psf_texture = LoadTexture("fullscreenui/multimedia-player.png");
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
  return {"*.bin",    "*.cue", "*.iso", "*.img",     "*.chd", "*.ecm", "*.mds", "*.psexe",
          "*.ps-exe", "*.exe", "*.psf", "*.minipsf", "*.m3u", "*.pbp", "*.PBP"};
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

  OpenFileSelector(ICON_FA_COMPACT_DISC " Select Disc Image", false, std::move(callback), GetDiscImageFilters());
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

  OpenFileSelector(ICON_FA_COMPACT_DISC " Select Disc Image", false, std::move(callback), GetDiscImageFilters(),
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

  OpenChoiceDialog(ICON_FA_COMPACT_DISC " Select Disc Image", true, std::move(options), std::move(callback));
}

void FullscreenUI::DoCheatsMenu()
{
  CheatList* cl = System::GetCheatList();
  if (!cl)
  {
    if (!System::LoadCheatListFromDatabase() || ((cl = System::GetCheatList()) == nullptr))
    {
      Host::AddKeyedOSDMessage("load_cheat_list", fmt::format("No cheats found for {}.", System::GetRunningTitle()),
                               10.0f);
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
      System::SetCheatCodeState(static_cast<u32>(index), checked, true);
  };
  OpenChoiceDialog(ICON_FA_FROWN " Cheat List", true, std::move(options), std::move(callback));
}

void FullscreenUI::DoToggleAnalogMode()
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
  BeginFullscreenColumns(nullptr, 0.0f, true);

  if (BeginFullscreenColumnWindow(0.0f, -710.0f, "logo", UIPrimaryDarkColor))
  {
    const float image_size = LayoutScale(380.f);
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() * 0.5f) - (image_size * 0.5f),
                               (ImGui::GetWindowHeight() * 0.5f) - (image_size * 0.5f)));
    ImGui::Image(s_app_icon_texture.get(), ImVec2(image_size, image_size));
  }
  EndFullscreenColumnWindow();

  if (BeginFullscreenColumnWindow(-710.0f, 0.0f, "menu", UIBackgroundColor))
  {
    ResetFocusHere();

    BeginMenuButtons(7, 0.5f);

    if (MenuButton(ICON_FA_LIST " Game List", "Launch a game from images scanned from your game directories."))
    {
      SwitchToGameList();
    }

    if (MenuButton(ICON_FA_PLAY_CIRCLE " Resume", "Starts the console from where it was before it was last closed."))
    {
      System::GetMostRecentResumeSaveStatePath();
      DoResume();
    }

    if (MenuButton(ICON_FA_FOLDER_OPEN " Start File", "Launch a game by selecting a file/disc image."))
    {
      DoStartFile();
    }

    if (MenuButton(ICON_FA_TOOLBOX " Start BIOS", "Start the console without any disc inserted."))
    {
      DoStartBIOS();
    }

    if (MenuButton(ICON_FA_UNDO " Load State", "Loads a global save state."))
    {
      OpenSaveStateSelector(true);
    }

    if (MenuButton(ICON_FA_SLIDERS_H " Settings", "Change settings for the emulator."))
      SwitchToSettings();

    if (MenuButton(ICON_FA_SIGN_OUT_ALT " Exit", "Exits the program."))
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

      if (FloatingButton(ICON_FA_QUESTION_CIRCLE, fullscreen_pos.x, 0.0f, -1.0f, -1.0f, -1.0f, 0.0f, true, g_large_font,
                         &fullscreen_pos))
      {
        OpenAboutWindow();
      }

      if (FloatingButton(ICON_FA_LIGHTBULB, fullscreen_pos.x, 0.0f, -1.0f, -1.0f, -1.0f, 0.0f, true, g_large_font,
                         &fullscreen_pos))
      {
        ToggleTheme();
      }
    }

    EndMenuButtons();

    const ImVec2 rev_size(g_medium_font->CalcTextSizeA(g_medium_font->FontSize, FLT_MAX, 0.0f, g_scm_tag_str));
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - rev_size.x - LayoutScale(20.0f),
                               ImGui::GetWindowHeight() - rev_size.y - LayoutScale(20.0f)));
    ImGui::PushFont(g_medium_font);
    ImGui::TextUnformatted(g_scm_tag_str);
    ImGui::PopFont();
  }

  EndFullscreenColumnWindow();

  EndFullscreenColumns();
}

bool FullscreenUI::IsEditingGameSettings(SettingsInterface* bsi)
{
  return (bsi == s_game_settings_interface.get());
}

SettingsInterface* FullscreenUI::GetEditingSettingsInterface()
{
  return s_game_settings_interface ? s_game_settings_interface.get() : Host::Internal::GetBaseSettingsLayer();
}

SettingsInterface* FullscreenUI::GetEditingSettingsInterface(bool game_settings)
{
  return (game_settings && s_game_settings_interface) ? s_game_settings_interface.get() :
                                                        Host::Internal::GetBaseSettingsLayer();
}

void FullscreenUI::SetSettingsChanged(SettingsInterface* bsi)
{
  if (bsi && bsi == s_game_settings_interface.get())
    s_game_settings_changed.store(true, std::memory_order_release);
  else
    s_settings_changed.store(true, std::memory_order_release);
}

bool FullscreenUI::GetEffectiveBoolSetting(SettingsInterface* bsi, const char* section, const char* key,
                                           bool default_value)
{
  if (IsEditingGameSettings(bsi))
  {
    std::optional<bool> value = bsi->GetOptionalBoolValue(section, key, std::nullopt);
    if (value.has_value())
      return value.value();
  }

  return Host::Internal::GetBaseSettingsLayer()->GetBoolValue(section, key, default_value);
}

s32 FullscreenUI::GetEffectiveIntSetting(SettingsInterface* bsi, const char* section, const char* key,
                                         s32 default_value)
{
  if (IsEditingGameSettings(bsi))
  {
    std::optional<s32> value = bsi->GetOptionalIntValue(section, key, std::nullopt);
    if (value.has_value())
      return value.value();
  }

  return Host::Internal::GetBaseSettingsLayer()->GetIntValue(section, key, default_value);
}

u32 FullscreenUI::GetEffectiveUIntSetting(SettingsInterface* bsi, const char* section, const char* key,
                                          u32 default_value)
{
  if (IsEditingGameSettings(bsi))
  {
    std::optional<u32> value = bsi->GetOptionalUIntValue(section, key, std::nullopt);
    if (value.has_value())
      return value.value();
  }

  return Host::Internal::GetBaseSettingsLayer()->GetUIntValue(section, key, default_value);
}

float FullscreenUI::GetEffectiveFloatSetting(SettingsInterface* bsi, const char* section, const char* key,
                                             float default_value)
{
  if (IsEditingGameSettings(bsi))
  {
    std::optional<float> value = bsi->GetOptionalFloatValue(section, key, std::nullopt);
    if (value.has_value())
      return value.value();
  }

  return Host::Internal::GetBaseSettingsLayer()->GetFloatValue(section, key, default_value);
}

std::string FullscreenUI::GetEffectiveStringSetting(SettingsInterface* bsi, const char* section, const char* key,
                                                    const char* default_value)
{
  std::string ret;
  std::optional<std::string> value;

  if (IsEditingGameSettings(bsi))
    value = bsi->GetOptionalStringValue(section, key, std::nullopt);

  if (value.has_value())
    ret = std::move(value.value());
  else
    ret = Host::Internal::GetBaseSettingsLayer()->GetStringValue(section, key, default_value);

  return ret;
}

void FullscreenUI::DrawInputBindingButton(SettingsInterface* bsi, Controller::ControllerBindingType type,
                                          const char* section, const char* name, const char* display_name,
                                          bool show_type)
{
  TinyString title;
  title.Fmt("{}/{}", section, name);

  ImRect bb;
  bool visible, hovered, clicked;
  clicked =
    MenuButtonFrame(title, true, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, &visible, &hovered, &bb.Min, &bb.Max);
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
        title = fmt::format(ICON_FA_DOT_CIRCLE " {}", display_name);
        break;
      case Controller::ControllerBindingType::Axis:
      case Controller::ControllerBindingType::HalfAxis:
        title = fmt::format(ICON_FA_BULLSEYE " {}", display_name);
        break;
      case Controller::ControllerBindingType::Motor:
        title = fmt::format(ICON_FA_BELL " {}", display_name);
        break;
      case Controller::ControllerBindingType::Macro:
        title = fmt::format(ICON_FA_PIZZA_SLICE " {}", display_name);
        break;
      default:
        title = display_name;
        break;
    }
  }

  ImGui::PushFont(g_large_font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, show_type ? title.GetCharArray() : display_name, nullptr,
                           nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  const std::string value(bsi->GetStringValue(section, name));
  ImGui::PushFont(g_medium_font);
  ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, value.empty() ? "No Binding" : value.c_str(), nullptr,
                           nullptr, ImVec2(0.0f, 0.0f), &summary_bb);
  ImGui::PopFont();

  if (clicked)
  {
    BeginInputBinding(bsi, type, section, name, display_name);
  }
  else if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
  {
    bsi->DeleteValue(section, name);
    SetSettingsChanged(bsi);
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

void FullscreenUI::BeginInputBinding(SettingsInterface* bsi, Controller::ControllerBindingType type,
                                     const std::string_view& section, const std::string_view& key,
                                     const std::string_view& display_name)
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

  InputManager::SetHook([game_settings = IsEditingGameSettings(bsi)](
                          InputBindingKey key, float value) -> InputInterceptHook::CallbackResult {
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
          SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
          const std::string new_binding(InputManager::ConvertInputBindingKeysToString(
            s_input_binding_new_bindings.data(), s_input_binding_new_bindings.size()));
          bsi->SetStringValue(s_input_binding_section.c_str(), s_input_binding_key.c_str(), new_binding.c_str());
          SetSettingsChanged(bsi);
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

  const char* title = ICON_FA_GAMEPAD " Set Input Binding";
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

bool FullscreenUI::DrawToggleSetting(SettingsInterface* bsi, const char* title, const char* summary,
                                     const char* section, const char* key, bool default_value, bool enabled,
                                     bool allow_tristate, float height, ImFont* font, ImFont* summary_font)
{
  if (!allow_tristate || !IsEditingGameSettings(bsi))
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

  SetSettingsChanged(bsi);
  return true;
}

void FullscreenUI::DrawIntListSetting(SettingsInterface* bsi, const char* title, const char* summary,
                                      const char* section, const char* key, int default_value,
                                      const char* const* options, size_t option_count, int option_offset, bool enabled,
                                      float height, ImFont* font, ImFont* summary_font)
{
  const bool game_settings = IsEditingGameSettings(bsi);

  if (options && option_count == 0)
  {
    while (options[option_count] != nullptr)
      option_count++;
  }

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
                     [game_settings, section, key, option_offset](s32 index, const std::string& title, bool checked) {
                       if (index >= 0)
                       {
                         auto lock = Host::GetSettingsLock();
                         SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                         if (game_settings)
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

                         SetSettingsChanged(bsi);
                       }

                       CloseChoiceDialog();
                     });
  }
}

void FullscreenUI::DrawIntRangeSetting(SettingsInterface* bsi, const char* title, const char* summary,
                                       const char* section, const char* key, int default_value, int min_value,
                                       int max_value, const char* format, bool enabled, float height, ImFont* font,
                                       ImFont* summary_font)
{
  const bool game_settings = IsEditingGameSettings(bsi);
  const std::optional<int> value =
    bsi->GetOptionalIntValue(section, key, game_settings ? std::nullopt : std::optional<int>(default_value));
  const std::string value_text(value.has_value() ? StringUtil::StdStringFromFormat(format, value.value()) :
                                                   std::string("Use Global Setting"));

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled, height, font, summary_font))
    ImGui::OpenPopup(title);

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 190.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  bool is_open = true;
  if (ImGui::BeginPopupModal(title, &is_open,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
  {
    BeginMenuButtons();

    const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    ImGui::SetNextItemWidth(end);
    s32 dlg_value = static_cast<s32>(value.value_or(default_value));
    if (ImGui::SliderInt("##value", &dlg_value, min_value, max_value, format, ImGuiSliderFlags_NoInput))
    {
      if (IsEditingGameSettings(bsi) && dlg_value == default_value)
        bsi->DeleteValue(section, key);
      else
        bsi->SetIntValue(section, key, dlg_value);

      SetSettingsChanged(bsi);
    }

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
    if (MenuButtonWithoutSummary("OK", true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font, ImVec2(0.5f, 0.0f)))
    {
      ImGui::CloseCurrentPopup();
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(3);
  ImGui::PopFont();
}

void FullscreenUI::DrawFloatRangeSetting(SettingsInterface* bsi, const char* title, const char* summary,
                                         const char* section, const char* key, float default_value, float min_value,
                                         float max_value, const char* format, float multiplier, bool enabled,
                                         float height, ImFont* font, ImFont* summary_font)
{
  const bool game_settings = IsEditingGameSettings(bsi);
  const std::optional<float> value =
    bsi->GetOptionalFloatValue(section, key, game_settings ? std::nullopt : std::optional<float>(default_value));
  const std::string value_text(value.has_value() ? StringUtil::StdStringFromFormat(format, value.value() * multiplier) :
                                                   std::string("Use Global Setting"));

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled, height, font, summary_font))
    ImGui::OpenPopup(title);

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 190.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  bool is_open = true;
  if (ImGui::BeginPopupModal(title, &is_open,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
  {
    BeginMenuButtons();

    const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    ImGui::SetNextItemWidth(end);
    float dlg_value = value.value_or(default_value) * multiplier;
    if (ImGui::SliderFloat("##value", &dlg_value, min_value, max_value, format, ImGuiSliderFlags_NoInput))
    {
      dlg_value /= multiplier;

      if (IsEditingGameSettings(bsi) && dlg_value == default_value)
        bsi->DeleteValue(section, key);
      else
        bsi->SetFloatValue(section, key, dlg_value);

      SetSettingsChanged(bsi);
    }

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
    if (MenuButtonWithoutSummary("OK", true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font, ImVec2(0.5f, 0.0f)))
    {
      ImGui::CloseCurrentPopup();
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(3);
  ImGui::PopFont();
}

void FullscreenUI::DrawIntRectSetting(SettingsInterface* bsi, const char* title, const char* summary,
                                      const char* section, const char* left_key, int default_left, const char* top_key,
                                      int default_top, const char* right_key, int default_right, const char* bottom_key,
                                      int default_bottom, int min_value, int max_value, const char* format,
                                      bool enabled, float height, ImFont* font, ImFont* summary_font)
{
  const bool game_settings = IsEditingGameSettings(bsi);
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

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 370.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  bool is_open = true;
  if (ImGui::BeginPopupModal(title, &is_open,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
  {
    s32 dlg_left_value = static_cast<s32>(left_value.value_or(default_left));
    s32 dlg_top_value = static_cast<s32>(top_value.value_or(default_top));
    s32 dlg_right_value = static_cast<s32>(right_value.value_or(default_right));
    s32 dlg_bottom_value = static_cast<s32>(bottom_value.value_or(default_bottom));

    BeginMenuButtons();

    const float midpoint = LayoutScale(150.0f);
    const float end = (ImGui::GetCurrentWindow()->WorkRect.GetWidth() - midpoint) + ImGui::GetStyle().WindowPadding.x;
    ImGui::TextUnformatted("Left: ");
    ImGui::SameLine(midpoint);
    ImGui::SetNextItemWidth(end);
    const bool left_modified =
      ImGui::SliderInt("##left", &dlg_left_value, min_value, max_value, format, ImGuiSliderFlags_NoInput);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
    ImGui::TextUnformatted("Top: ");
    ImGui::SameLine(midpoint);
    ImGui::SetNextItemWidth(end);
    const bool top_modified =
      ImGui::SliderInt("##top", &dlg_top_value, min_value, max_value, format, ImGuiSliderFlags_NoInput);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
    ImGui::TextUnformatted("Right: ");
    ImGui::SameLine(midpoint);
    ImGui::SetNextItemWidth(end);
    const bool right_modified =
      ImGui::SliderInt("##right", &dlg_right_value, min_value, max_value, format, ImGuiSliderFlags_NoInput);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
    ImGui::TextUnformatted("Bottom: ");
    ImGui::SameLine(midpoint);
    ImGui::SetNextItemWidth(end);
    const bool bottom_modified =
      ImGui::SliderInt("##bottom", &dlg_bottom_value, min_value, max_value, format, ImGuiSliderFlags_NoInput);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
    if (left_modified)
    {
      if (IsEditingGameSettings(bsi) && dlg_left_value == default_left)
        bsi->DeleteValue(section, left_key);
      else
        bsi->SetIntValue(section, left_key, dlg_left_value);
    }
    if (top_modified)
    {
      if (IsEditingGameSettings(bsi) && dlg_top_value == default_top)
        bsi->DeleteValue(section, top_key);
      else
        bsi->SetIntValue(section, top_key, dlg_top_value);
    }
    if (right_modified)
    {
      if (IsEditingGameSettings(bsi) && dlg_right_value == default_right)
        bsi->DeleteValue(section, right_key);
      else
        bsi->SetIntValue(section, right_key, dlg_right_value);
    }
    if (bottom_modified)
    {
      if (IsEditingGameSettings(bsi) && dlg_bottom_value == default_bottom)
        bsi->DeleteValue(section, bottom_key);
      else
        bsi->SetIntValue(section, bottom_key, dlg_bottom_value);
    }

    if (left_modified || top_modified || right_modified || bottom_modified)
      SetSettingsChanged(bsi);

    if (MenuButtonWithoutSummary("OK", true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font, ImVec2(0.5f, 0.0f)))
    {
      ImGui::CloseCurrentPopup();
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(3);
  ImGui::PopFont();
}

void FullscreenUI::DrawIntSpinBoxSetting(SettingsInterface* bsi, const char* title, const char* summary,
                                         const char* section, const char* key, int default_value, int min_value,
                                         int max_value, int step_value, const char* format, bool enabled, float height,
                                         ImFont* font, ImFont* summary_font)
{
  const bool game_settings = IsEditingGameSettings(bsi);
  const std::optional<int> value =
    bsi->GetOptionalIntValue(section, key, game_settings ? std::nullopt : std::optional<int>(default_value));
  TinyString value_text;
  if (value.has_value())
    value_text.Format(format, value.value());
  else
    value_text = "Use Global Setting";

  static bool manual_input = false;
  static u32 repeat_count = 0;

  if (MenuButtonWithValue(title, summary, value_text, enabled, height, font, summary_font))
  {
    ImGui::OpenPopup(title);
    manual_input = false;
  }

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 190.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  bool is_open = true;
  if (ImGui::BeginPopupModal(title, &is_open,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
  {
    BeginMenuButtons();

    s32 dlg_value = static_cast<s32>(value.value_or(default_value));
    bool dlg_value_changed = false;

    char str_value[32];
    std::snprintf(str_value, std::size(str_value), format, dlg_value);

    if (manual_input)
    {
      const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
      ImGui::SetNextItemWidth(end);

      std::snprintf(str_value, std::size(str_value), "%d", dlg_value);
      if (ImGui::InputText("##value", str_value, std::size(str_value), ImGuiInputTextFlags_CharsDecimal))
      {
        dlg_value = StringUtil::FromChars<s32>(str_value).value_or(dlg_value);
        dlg_value_changed = true;
      }

      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
    }
    else
    {
      const ImVec2& padding(ImGui::GetStyle().FramePadding);
      ImVec2 button_pos(ImGui::GetCursorPos());

      // Align value text in middle.
      ImGui::SetCursorPosY(
        button_pos.y +
        ((LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + padding.y * 2.0f) - g_large_font->FontSize) * 0.5f);
      ImGui::TextUnformatted(str_value);

      s32 step = 0;
      if (FloatingButton(ICON_FA_CHEVRON_UP, padding.x, button_pos.y, -1.0f, -1.0f, 1.0f, 0.0f, true, g_large_font,
                         &button_pos, true))
      {
        step = step_value;
      }
      if (FloatingButton(ICON_FA_CHEVRON_DOWN, button_pos.x - padding.x, button_pos.y, -1.0f, -1.0f, -1.0f, 0.0f, true,
                         g_large_font, &button_pos, true))
      {
        step = -step_value;
      }
      if (FloatingButton(ICON_FA_KEYBOARD, button_pos.x - padding.x, button_pos.y, -1.0f, -1.0f, -1.0f, 0.0f, true,
                         g_large_font, &button_pos))
      {
        manual_input = true;
      }
      if (FloatingButton(ICON_FA_TRASH, button_pos.x - padding.x, button_pos.y, -1.0f, -1.0f, -1.0f, 0.0f, true,
                         g_large_font, &button_pos))
      {
        dlg_value = default_value;
        dlg_value_changed = true;
      }

      if (step != 0)
      {
        dlg_value += step;
        dlg_value_changed = true;
      }

      ImGui::SetCursorPosY(button_pos.y + (padding.y * 2.0f) +
                           LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY + 10.0f));
    }

    if (dlg_value_changed)
    {
      dlg_value = std::clamp(dlg_value, min_value, max_value);
      if (IsEditingGameSettings(bsi) && dlg_value == default_value)
        bsi->DeleteValue(section, key);
      else
        bsi->SetIntValue(section, key, dlg_value);

      SetSettingsChanged(bsi);
    }

    if (MenuButtonWithoutSummary("OK", true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font, ImVec2(0.5f, 0.0f)))
    {
      ImGui::CloseCurrentPopup();
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(4);
  ImGui::PopFont();
}

void FullscreenUI::DrawStringListSetting(SettingsInterface* bsi, const char* title, const char* summary,
                                         const char* section, const char* key, const char* default_value,
                                         const char* const* options, const char* const* option_values,
                                         size_t option_count, bool enabled, float height, ImFont* font,
                                         ImFont* summary_font)
{
  const bool game_settings = IsEditingGameSettings(bsi);
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
                     [game_settings, section, key, option_values](s32 index, const std::string& title, bool checked) {
                       if (index >= 0)
                       {
                         auto lock = Host::GetSettingsLock();
                         SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                         if (game_settings)
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

                         SetSettingsChanged(bsi);
                       }

                       CloseChoiceDialog();
                     });
  }
}

template<typename DataType, typename SizeType>
void FullscreenUI::DrawEnumSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                                   const char* key, DataType default_value,
                                   std::optional<DataType> (*from_string_function)(const char* str),
                                   const char* (*to_string_function)(DataType value),
                                   const char* (*to_display_string_function)(DataType value), SizeType option_count,
                                   bool enabled /*= true*/,
                                   float height /*= ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT*/,
                                   ImFont* font /*= g_large_font*/, ImFont* summary_font /*= g_medium_font*/)
{
  const bool game_settings = IsEditingGameSettings(bsi);
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
    OpenChoiceDialog(
      title, false, std::move(cd_options),
      [section, key, to_string_function, game_settings](s32 index, const std::string& title, bool checked) {
        if (index >= 0)
        {
          auto lock = Host::GetSettingsLock();
          SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
          if (game_settings)
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

          SetSettingsChanged(bsi);
        }

        CloseChoiceDialog();
      });
  }
}
void FullscreenUI::DrawFloatListSetting(SettingsInterface* bsi, const char* title, const char* summary,
                                        const char* section, const char* key, float default_value,
                                        const char* const* options, const float* option_values, size_t option_count,
                                        bool enabled, float height, ImFont* font, ImFont* summary_font)
{
  const bool game_settings = IsEditingGameSettings(bsi);
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
                     [game_settings, section, key, option_values](s32 index, const std::string& title, bool checked) {
                       if (index >= 0)
                       {
                         auto lock = Host::GetSettingsLock();
                         SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                         if (game_settings)
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

                         SetSettingsChanged(bsi);
                       }

                       CloseChoiceDialog();
                     });
  }
}

void FullscreenUI::DrawFolderSetting(SettingsInterface* bsi, const char* title, const char* section, const char* key,
                                     const std::string& runtime_var,
                                     float height /* = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT */,
                                     ImFont* font /* = g_large_font */, ImFont* summary_font /* = g_medium_font */)
{
  if (MenuButton(title, runtime_var.c_str()))
  {
    OpenFileSelector(title, true,
                     [game_settings = IsEditingGameSettings(bsi), section = std::string(section),
                      key = std::string(key)](const std::string& dir) {
                       if (dir.empty())
                         return;

                       auto lock = Host::GetSettingsLock();
                       SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                       std::string relative_path(Path::MakeRelative(dir, EmuFolders::DataRoot));
                       bsi->SetStringValue(section.c_str(), key.c_str(), relative_path.c_str());
                       SetSettingsChanged(bsi);

                       // Host::RunOnCPUThread(&Host::Internal::UpdateEmuFolders);
                       s_cover_image_map.clear();

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
                     SettingsInterface* bsi = GetEditingSettingsInterface();
                     const bool result =
                       InputManager::MapController(*bsi, port, InputManager::GetGenericBindingMapping(name));
                     SetSettingsChanged(bsi);

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

  PopulateGraphicsAdapterList();
  PopulatePostProcessingChain();

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
  if (System::GetRunningSerial().empty())
    return;

  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(System::GetRunningPath().c_str());
  if (!entry)
  {
    SwitchToGameSettingsForSerial(System::GetRunningSerial());
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

void FullscreenUI::DoCopyGameSettings()
{
  if (!s_game_settings_interface)
    return;

  Settings temp_settings;
  temp_settings.Load(*GetEditingSettingsInterface(false));
  temp_settings.Save(*s_game_settings_interface);
  SetSettingsChanged(s_game_settings_interface.get());

  ShowToast("Game Settings Copied", fmt::format("Game settings initialized with global settings for '{}'.",
                                                Path::GetFileTitle(s_game_settings_interface->GetFileName())));
}

void FullscreenUI::DoClearGameSettings()
{
  if (!s_game_settings_interface)
    return;

  s_game_settings_interface->Clear();
  if (!s_game_settings_interface->GetFileName().empty())
    FileSystem::DeleteFile(s_game_settings_interface->GetFileName().c_str());

  SetSettingsChanged(s_game_settings_interface.get());

  ShowToast("Game Settings Cleared", fmt::format("Game settings have been cleared for '{}'.",
                                                 Path::GetFileTitle(s_game_settings_interface->GetFileName())));
}

void FullscreenUI::DrawSettingsWindow()
{
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY + LAYOUT_MENU_BUTTON_Y_PADDING * 2.0f + 2.0f));

  const float bg_alpha = System::IsValid() ? (s_settings_page == SettingsPage::PostProcessing ? 0.50f : 0.90f) : 1.0f;

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "settings_category",
                            ImVec4(UIPrimaryColor.x, UIPrimaryColor.y, UIPrimaryColor.z, bg_alpha)))
  {
    static constexpr float ITEM_WIDTH = 25.0f;

    static constexpr const char* global_icons[] = {
      ICON_FA_WINDOW_MAXIMIZE, ICON_FA_HDD,          ICON_FA_SLIDERS_H,  ICON_FA_MICROCHIP,
      ICON_FA_MAGIC,           ICON_FA_PAINT_ROLLER, ICON_FA_HEADPHONES, ICON_FA_GAMEPAD,
      ICON_FA_KEYBOARD,        ICON_FA_SD_CARD,      ICON_FA_TROPHY,     ICON_FA_EXCLAMATION_TRIANGLE};
    static constexpr const char* per_game_icons[] = {
      ICON_FA_PARAGRAPH, ICON_FA_HDD,        ICON_FA_SLIDERS_H,
      ICON_FA_MAGIC,     ICON_FA_HEADPHONES, ICON_FA_GAMEPAD,
      ICON_FA_SD_CARD,   ICON_FA_TROPHY,     ICON_FA_EXCLAMATION_TRIANGLE};
    static constexpr SettingsPage global_pages[] = {
      SettingsPage::Interface, SettingsPage::Console,        SettingsPage::Emulation,    SettingsPage::BIOS,
      SettingsPage::Display,   SettingsPage::PostProcessing, SettingsPage::Audio,        SettingsPage::Controller,
      SettingsPage::Hotkey,    SettingsPage::MemoryCards,    SettingsPage::Achievements, SettingsPage::Advanced};
    static constexpr SettingsPage per_game_pages[] = {
      SettingsPage::Summary,     SettingsPage::Console,      SettingsPage::Emulation,
      SettingsPage::Display,     SettingsPage::Audio,        SettingsPage::Controller,
      SettingsPage::MemoryCards, SettingsPage::Achievements, SettingsPage::Advanced};
    static constexpr std::array<const char*, static_cast<u32>(SettingsPage::Count)> titles = {
      {"Summary", "Interface Settings", "Console Settings", "Emulation Settings", "BIOS Settings",
       "Controller Settings", "Hotkey Settings", "Memory Card Settings", "Display Settings", "Post-Processing Settings",
       "Audio Settings", "Achievements Settings", "Advanced Settings"}};

    const bool game_settings = IsEditingGameSettings(GetEditingSettingsInterface());
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

    if (!ImGui::IsPopupOpen(0u, ImGuiPopupFlags_AnyPopup))
    {
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
    }

    if (NavButton(ICON_FA_BACKWARD, true, true))
      ReturnToMainWindow();

    if (s_game_settings_entry)
      NavTitle(s_game_settings_entry->title.c_str());
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

      case SettingsPage::PostProcessing:
        DrawPostProcessingSettingsPage();
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
    if (MenuButton(ICON_FA_WINDOW_MAXIMIZE " Title", s_game_settings_entry->title.c_str(), true))
      CopyTextToClipboard("Game title copied to clipboard.", s_game_settings_entry->title);
    if (MenuButton(ICON_FA_PAGER " Serial", s_game_settings_entry->serial.c_str(), true))
      CopyTextToClipboard("Game serial copied to clipboard.", s_game_settings_entry->serial);
    if (MenuButton(ICON_FA_COMPACT_DISC " Type", GameList::GetEntryTypeDisplayName(s_game_settings_entry->type), true))
    {
      CopyTextToClipboard("Game type copied to clipboard.",
                          GameList::GetEntryTypeDisplayName(s_game_settings_entry->type));
    }
    if (MenuButton(ICON_FA_BOX " Region", Settings::GetDiscRegionDisplayName(s_game_settings_entry->region), true))
    {
      CopyTextToClipboard("Game region copied to clipboard.",
                          Settings::GetDiscRegionDisplayName(s_game_settings_entry->region));
    }
    if (MenuButton(ICON_FA_STAR " Compatibility Rating",
                   GameDatabase::GetCompatibilityRatingDisplayName(s_game_settings_entry->compatibility), true))
    {
      CopyTextToClipboard("Game compatibility rating copied to clipboard.",
                          GameDatabase::GetCompatibilityRatingDisplayName(s_game_settings_entry->compatibility));
    }
    if (MenuButton(ICON_FA_FOLDER_OPEN " Path", s_game_settings_entry->path.c_str(), true))
    {
      CopyTextToClipboard("Game path copied to clipboard.", s_game_settings_entry->path);
    }
  }
  else
  {
    MenuButton(ICON_FA_BAN " Details unavailable for game not scanned in game list.", "");
  }

  MenuHeading("Options");

  if (MenuButton(ICON_FA_COPY " Copy Settings", "Copies the current global settings to this game."))
    DoCopyGameSettings();
  if (MenuButton(ICON_FA_TRASH " Clear Settings", "Clears all settings set for this game."))
    DoClearGameSettings();

  EndMenuButtons();
}

void FullscreenUI::DrawInterfaceSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading("Behavior");

  DrawToggleSetting(bsi, ICON_FA_PAUSE " Pause On Start", "Pauses the emulator when a game is started.", "Main",
                    "StartPaused", false);
  DrawToggleSetting(bsi, ICON_FA_VIDEO " Pause On Focus Loss",
                    "Pauses the emulator when you minimize the window or switch to another "
                    "application, and unpauses when you switch back.",
                    "Main", "PauseOnFocusLoss", false);
  DrawToggleSetting(bsi, ICON_FA_WINDOW_MAXIMIZE " Pause On Menu",
                    "Pauses the emulator when you open the quick menu, and unpauses when you close it.", "Main",
                    "PauseOnMenu", true);
  DrawToggleSetting(bsi, ICON_FA_POWER_OFF " Confirm Power Off",
                    "Determines whether a prompt will be displayed to confirm shutting down the emulator/game "
                    "when the hotkey is pressed.",
                    "Main", "ConfirmPowerOff", true);
  DrawToggleSetting(bsi, ICON_FA_SAVE " Save State On Exit",
                    "Automatically saves the emulator state when powering down or exiting. You can then "
                    "resume directly from where you left off next time.",
                    "Main", "SaveStateOnExit", true);
  DrawToggleSetting(bsi, ICON_FA_TV " Start Fullscreen",
                    "Automatically switches to fullscreen mode when the program is started.", "Main", "StartFullscreen",
                    false);
  DrawToggleSetting(bsi, ICON_FA_MOUSE "  Double-Click Toggles Fullscreen",
                    "Switches between full screen and windowed when the window is double-clicked.", "Main",
                    "DoubleClickTogglesFullscreen", true);
  DrawToggleSetting(bsi, ICON_FA_MOUSE_POINTER "Hide Cursor In Fullscreen",
                    "Hides the mouse pointer/cursor when the emulator is in fullscreen mode.", "Main",
                    "HideCursorInFullscreen", true);
  DrawToggleSetting(bsi, ICON_FA_MAGIC " Inhibit Screensaver",
                    "Prevents the screen saver from activating and the host from sleeping while emulation is running.",
                    "Main", "InhibitScreensaver", true);
  DrawToggleSetting(bsi, ICON_FA_GAMEPAD " Load Devices From Save States",
                    "When enabled, memory cards and controllers will be overwritten when save states are loaded.",
                    "Main", "LoadDevicesFromSaveStates", false);
  DrawToggleSetting(bsi, ICON_FA_COGS " Apply Per-Game Settings",
                    "When enabled, per-game settings will be applied, and incompatible enhancements will be disabled.",
                    "Main", "ApplyGameSettings", true);
  DrawToggleSetting(bsi, ICON_FA_FROWN " Automatically Load Cheats",
                    "Automatically loads and applies cheats on game start.", "Main", "AutoLoadCheats", true);
  if (DrawToggleSetting(bsi, ICON_FA_PAINT_BRUSH " Use Light Theme",
                        "Uses a light coloured theme instead of the default dark theme.", "Main",
                        "UseLightFullscreenUITheme", false))
  {
    ImGuiFullscreen::SetTheme(bsi->GetBoolValue("Main", "UseLightFullscreenUITheme", false));
  }

#ifdef WITH_DISCORD_PRESENCE
  MenuHeading("Integration");
  DrawToggleSetting(bsi, ICON_FA_CHARGING_STATION " Enable Discord Presence",
                    "Shows the game you are currently playing as part of your profile on Discord.", "Main",
                    "EnableDiscordPresence", false);
#endif

  MenuHeading("On-Screen Display");
  DrawIntSpinBoxSetting(bsi, ICON_FA_SEARCH " OSD Scale",
                        "Determines how large the on-screen messages and monitor are.", "Display", "OSDScale", 100, 25,
                        500, 1, "%d%%");
  DrawToggleSetting(bsi, ICON_FA_LIST " Show OSD Messages", "Shows on-screen-display messages when events occur.",
                    "Display", "ShowOSDMessages", true);
  DrawToggleSetting(
    bsi, ICON_FA_CLOCK " Show Speed",
    "Shows the current emulation speed of the system in the top-right corner of the display as a percentage.",
    "Display", "ShowSpeed", false);
  DrawToggleSetting(bsi, ICON_FA_RULER " Show FPS",
                    "Shows the number of frames (or v-syncs) displayed per second by the system in the top-right "
                    "corner of the display.",
                    "Display", "ShowFPS", false);
  DrawToggleSetting(bsi, ICON_FA_BATTERY_HALF " Show CPU Usage",
                    "Shows the host's CPU usage based on threads in the top-right corner of the display.", "Display",
                    "ShowCPU", false);
  DrawToggleSetting(bsi, ICON_FA_SPINNER " Show GPU Usage",
                    "Shows the host's GPU usage in the top-right corner of the display.", "Display", "ShowGPU", false);
  DrawToggleSetting(bsi, ICON_FA_RULER_VERTICAL " Show Resolution",
                    "Shows the current rendering resolution of the system in the top-right corner of the display.",
                    "Display", "ShowResolution", false);
  DrawToggleSetting(bsi, ICON_FA_GAMEPAD " Show Controller Input",
                    "Shows the current controller state of the system in the bottom-left corner of the display.",
                    "Display", "ShowInputs", false);

  EndMenuButtons();
}

void FullscreenUI::DrawBIOSSettingsPage()
{
  static constexpr auto config_keys = make_array("", "PathNTSCJ", "PathNTSCU", "PathPAL");

  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  BeginMenuButtons();

  MenuHeading("BIOS Selection");

  for (u32 i = 0; i < static_cast<u32>(ConsoleRegion::Count); i++)
  {
    const ConsoleRegion region = static_cast<ConsoleRegion>(i);
    if (region == ConsoleRegion::Auto)
      continue;

    TinyString title;
    title.Format("BIOS for %s", Settings::GetConsoleRegionName(region));

    const std::optional<std::string> filename(bsi->GetOptionalStringValue(
      "BIOS", config_keys[i], game_settings ? std::nullopt : std::optional<const char*>("")));

    if (MenuButtonWithValue(title,
                            SmallString::FromFormat("BIOS to use when emulating %s consoles.",
                                                    Settings::GetConsoleRegionDisplayName(region)),
                            filename.has_value() ? (filename->empty() ? "Auto-Detect" : filename->c_str()) :
                                                   "Use Global Setting"))
    {
      ImGuiFullscreen::ChoiceDialogOptions options;
      auto images = BIOS::FindBIOSImagesInDirectory(EmuFolders::Bios.c_str());
      options.reserve(images.size() + 2);
      if (IsEditingGameSettings(bsi))
        options.emplace_back("Use Global Setting", !filename.has_value());
      options.emplace_back("Auto-Detect", filename.has_value() && filename->empty());
      for (auto& [path, info] : images)
      {
        const bool selected = (filename.has_value() && filename.value() == path);
        options.emplace_back(std::move(path), selected);
      }

      OpenChoiceDialog(title, false, std::move(options),
                       [game_settings, i](s32 index, const std::string& path, bool checked) {
                         if (index >= 0)
                         {
                           auto lock = Host::GetSettingsLock();
                           SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                           if (game_settings && index == 0)
                             bsi->DeleteValue("BIOS", config_keys[i]);
                           else
                             bsi->SetStringValue("BIOS", config_keys[i], path.c_str());
                           SetSettingsChanged(bsi);
                         }
                         CloseChoiceDialog();
                       });
    }
  }

  DrawFolderSetting(bsi, "BIOS Directory", "BIOS", "SearchDirectory", EmuFolders::Bios);

  MenuHeading("Patches");

  DrawToggleSetting(bsi, "Enable Fast Boot", "Patches the BIOS to skip the boot animation. Safe to enable.", "BIOS",
                    "PatchFastBoot", Settings::DEFAULT_FAST_BOOT_VALUE);
  DrawToggleSetting(bsi, "Enable TTY Output",
                    "Patches the BIOS to log calls to printf(). Only use when debugging, can break games.", "BIOS",
                    "PatchTTYEnable", false);

  EndMenuButtons();
}

void FullscreenUI::DrawConsoleSettingsPage()
{
  static constexpr auto cdrom_read_speeds =
    make_array("None (Double Speed)", "2x (Quad Speed)", "3x (6x Speed)", "4x (8x Speed)", "5x (10x Speed)",
               "6x (12x Speed)", "7x (14x Speed)", "8x (16x Speed)", "9x (18x Speed)", "10x (20x Speed)");

  static constexpr auto cdrom_seek_speeds =
    make_array("Infinite/Instantaneous", "None (Normal Speed)", "2x", "3x", "4x", "5x", "6x", "7x", "8x", "9x", "10x");

  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading("Console Settings");

  DrawEnumSetting(bsi, "Region", "Determines the emulated hardware type.", "Console", "Region",
                  Settings::DEFAULT_CONSOLE_REGION, &Settings::ParseConsoleRegionName, &Settings::GetConsoleRegionName,
                  &Settings::GetConsoleRegionDisplayName, ConsoleRegion::Count);
  DrawToggleSetting(bsi, "Enable 8MB RAM",
                    "Enables an additional 6MB of RAM to obtain a total of 2+6 = 8MB, usually present on dev consoles.",
                    "Console", "Enable8MBRAM", false);

  MenuHeading("CPU Emulation");

  DrawEnumSetting(
    bsi, "Execution Mode", "Determines how the emulated CPU executes instructions. Recompiler is recommended.", "CPU",
    "ExecutionMode", Settings::DEFAULT_CPU_EXECUTION_MODE, &Settings::ParseCPUExecutionMode,
    &Settings::GetCPUExecutionModeName, &Settings::GetCPUExecutionModeDisplayName, CPUExecutionMode::Count);

  DrawToggleSetting(bsi, "Enable Overclocking", "When this option is chosen, the clock speed set below will be used.",
                    "CPU", "OverclockEnable", false);

  const bool oc_enable = GetEffectiveBoolSetting(bsi, "CPU", "OverclockEnable", false);
  if (oc_enable)
  {
    u32 oc_numerator = GetEffectiveUIntSetting(bsi, "CPU", "OverclockNumerator", 1);
    u32 oc_denominator = GetEffectiveUIntSetting(bsi, "CPU", "OverclockDenominator", 1);
    s32 oc_percent = static_cast<s32>(Settings::CPUOverclockFractionToPercent(oc_numerator, oc_denominator));
    if (RangeButton("Overclocking Percentage",
                    "Selects the percentage of the normal clock speed the emulated hardware will run at.", &oc_percent,
                    10, 1000, 10, "%d%%"))
    {
      Settings::CPUOverclockPercentToFraction(oc_percent, &oc_numerator, &oc_denominator);
      bsi->SetUIntValue("CPU", "OverclockNumerator", oc_numerator);
      bsi->SetUIntValue("CPU", "OverclockDenominator", oc_denominator);
      SetSettingsChanged(bsi);
    }
  }

  DrawToggleSetting(bsi, "Enable Recompiler ICache",
                    "Makes games run closer to their console framerate, at a small cost to performance.", "CPU",
                    "RecompilerICache", false);

  MenuHeading("CD-ROM Emulation");

  DrawIntListSetting(
    bsi, "Read Speedup",
    "Speeds up CD-ROM reads by the specified factor. May improve loading speeds in some games, and break others.",
    "CDROM", "ReadSpeedup", 1, cdrom_read_speeds.data(), cdrom_read_speeds.size(), 1);
  DrawIntListSetting(
    bsi, "Read Speedup",
    "Speeds up CD-ROM seeks by the specified factor. May improve loading speeds in some games, and break others.",
    "CDROM", "SeekSpeedup", 1, cdrom_seek_speeds.data(), cdrom_seek_speeds.size());

  DrawIntRangeSetting(
    bsi, "Readahead Sectors",
    "Reduces hitches in emulation by reading/decompressing CD data asynchronously on a worker thread.", "CDROM",
    "ReadaheadSectors", Settings::DEFAULT_CDROM_READAHEAD_SECTORS, 0, 32, "%d sectors");

  DrawToggleSetting(bsi, "Enable Region Check", "Simulates the region check present in original, unmodified consoles.",
                    "CDROM", "RegionCheck", false);
  DrawToggleSetting(
    bsi, "Preload Images to RAM",
    "Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay.", "CDROM",
    "LoadImageToRAM", false);
  DrawToggleSetting(
    bsi, "Apply Image Patches",
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

  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading("Speed Control");
  DrawFloatListSetting(
    bsi, "Emulation Speed",
    "Sets the target emulation speed. It is not guaranteed that this speed will be reached on all systems.", "Main",
    "EmulationSpeed", 1.0f, emulation_speed_titles.data(), emulation_speed_values.data(),
    emulation_speed_titles.size());
  DrawFloatListSetting(
    bsi, "Fast Forward Speed",
    "Sets the fast forward speed. It is not guaranteed that this speed will be reached on all systems.", "Main",
    "FastForwardSpeed", 0.0f, emulation_speed_titles.data(), emulation_speed_values.data(),
    emulation_speed_titles.size());
  DrawFloatListSetting(bsi, "Turbo Speed",
                       "Sets the turbo speed. It is not guaranteed that this speed will be reached on all systems.",
                       "Main", "TurboSpeed", 2.0f, emulation_speed_titles.data(), emulation_speed_values.data(),
                       emulation_speed_titles.size());

  MenuHeading("Runahead/Rewind");

  DrawToggleSetting(bsi, "Enable Rewinding", "Saves state periodically so you can rewind any mistakes while playing.",
                    "Main", "RewindEnable", false);
  DrawFloatRangeSetting(
    bsi, "Rewind Save Frequency",
    "How often a rewind state will be created. Higher frequencies have greater system requirements.", "Main",
    "RewindFrequency", 10.0f, 0.0f, 3600.0f, "%.2f Seconds");
  DrawIntRangeSetting(bsi, "Rewind Save Slots",
                      "How many saves will be kept for rewinding. Higher values have greater memory requirements.",
                      "Main", "RewindSaveSlots", 10, 1, 10000, "%d Frames");

  const s32 runahead_frames = GetEffectiveIntSetting(bsi, "Main", "RunaheadFrameCount", 0);
  const bool runahead_enabled = (runahead_frames > 0);
  const bool rewind_enabled = GetEffectiveBoolSetting(bsi, "Main", "RewindEnable", false);

  static constexpr auto runahead_options =
    make_array("Disabled", "1 Frame", "2 Frames", "3 Frames", "4 Frames", "5 Frames", "6 Frames", "7 Frames",
               "8 Frames", "9 Frames", "10 Frames");

  DrawIntListSetting(
    bsi, "Runahead",
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
    const float rewind_frequency = GetEffectiveFloatSetting(bsi, "Main", "RewindFrequency", 10.0f);
    const s32 rewind_save_slots = GetEffectiveIntSetting(bsi, "Main", "RewindSaveSlots", 10);
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

void FullscreenUI::CopyGlobalControllerSettingsToGame()
{
  SettingsInterface* dsi = GetEditingSettingsInterface(true);
  SettingsInterface* ssi = GetEditingSettingsInterface(false);

  InputManager::CopyConfiguration(dsi, *ssi, true, true, false);
  SetSettingsChanged(dsi);

  ShowToast(std::string(), "Per-game controller configuration initialized with global settings.");
}

void FullscreenUI::DoLoadInputProfile()
{
  std::vector<std::string> profiles(InputManager::GetInputProfileNames());
  if (profiles.empty())
  {
    ShowToast(std::string(), "No input profiles available.");
    return;
  }

  ImGuiFullscreen::ChoiceDialogOptions coptions;
  coptions.reserve(profiles.size());
  for (std::string& name : profiles)
    coptions.emplace_back(std::move(name), false);
  OpenChoiceDialog(ICON_FA_FOLDER_OPEN " Load Profile", false, std::move(coptions),
                   [](s32 index, const std::string& title, bool checked) {
                     if (index < 0)
                       return;

                     INISettingsInterface ssi(System::GetInputProfilePath(title));
                     if (!ssi.Load())
                     {
                       ShowToast(std::string(), fmt::format("Failed to load '{}'.", title));
                       CloseChoiceDialog();
                       return;
                     }

                     auto lock = Host::GetSettingsLock();
                     SettingsInterface* dsi = GetEditingSettingsInterface();
                     InputManager::CopyConfiguration(dsi, ssi, true, true, IsEditingGameSettings(dsi));
                     SetSettingsChanged(dsi);
                     ShowToast(std::string(), fmt::format("Input profile '{}' loaded.", title));
                     CloseChoiceDialog();
                   });
}

void FullscreenUI::DoSaveInputProfile(const std::string& name)
{
  INISettingsInterface dsi(System::GetInputProfilePath(name));

  auto lock = Host::GetSettingsLock();
  SettingsInterface* ssi = GetEditingSettingsInterface();
  InputManager::CopyConfiguration(&dsi, *ssi, true, true, IsEditingGameSettings(ssi));
  if (dsi.Save())
    ShowToast(std::string(), fmt::format("Input profile '{}' saved.", name));
  else
    ShowToast(std::string(), fmt::format("Failed to save input profile '{}'.", name));
}

void FullscreenUI::DoSaveInputProfile()
{
  std::vector<std::string> profiles(InputManager::GetInputProfileNames());
  if (profiles.empty())
  {
    ShowToast(std::string(), "No input profiles available.");
    return;
  }

  ImGuiFullscreen::ChoiceDialogOptions coptions;
  coptions.reserve(profiles.size() + 1);
  coptions.emplace_back("Create New...", false);
  for (std::string& name : profiles)
    coptions.emplace_back(std::move(name), false);
  OpenChoiceDialog(
    ICON_FA_SAVE " Save Profile", false, std::move(coptions), [](s32 index, const std::string& title, bool checked) {
      if (index < 0)
        return;

      if (index > 0)
      {
        DoSaveInputProfile(title);
        CloseChoiceDialog();
        return;
      }

      CloseChoiceDialog();

      OpenInputStringDialog(ICON_FA_SAVE " Save Profile", "Enter the name of the input profile you wish to create.",
                            std::string(), ICON_FA_FOLDER_PLUS " Create", [](std::string title) {
                              if (!title.empty())
                                DoSaveInputProfile(title);
                            });
    });
}

void FullscreenUI::ResetControllerSettings()
{
  SettingsInterface* dsi = GetEditingSettingsInterface();

  CommonHost::SetDefaultControllerSettings(*dsi);
  ShowToast(std::string(), "Controller settings reset to default.");
}

void FullscreenUI::DrawControllerSettingsPage()
{
  BeginMenuButtons();

  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  MenuHeading("Configuration");

  if (IsEditingGameSettings(bsi))
  {
    if (DrawToggleSetting(bsi, ICON_FA_COG " Per-Game Configuration",
                          "Uses game-specific settings for controllers for this game.", "Pad",
                          "UseGameSettingsForController", false, IsEditingGameSettings(bsi), false))
    {
      // did we just enable per-game for the first time?
      if (bsi->GetBoolValue("Pad", "UseGameSettingsForController", false) &&
          !bsi->GetBoolValue("Pad", "GameSettingsInitialized", false))
      {
        bsi->SetBoolValue("Pad", "GameSettingsInitialized", true);
        CopyGlobalControllerSettingsToGame();
      }
    }
  }

  if (IsEditingGameSettings(bsi) && !bsi->GetBoolValue("Pad", "UseGameSettingsForController", false))
  {
    // nothing to edit..
    EndMenuButtons();
    return;
  }

  if (IsEditingGameSettings(bsi))
  {
    if (MenuButton(ICON_FA_COPY " Copy Global Settings", "Copies the global controller configuration to this game."))
      CopyGlobalControllerSettingsToGame();
  }
  else
  {
    if (MenuButton(ICON_FA_FOLDER_MINUS " Reset Settings",
                   "Resets all configuration to defaults (including bindings)."))
    {
      ResetControllerSettings();
    }
  }

  if (MenuButton(ICON_FA_FOLDER_OPEN " Load Profile", "Replaces these settings with a previously saved input profile."))
  {
    DoLoadInputProfile();
  }
  if (MenuButton(ICON_FA_SAVE " Save Profile", "Stores the current settings to an input profile."))
  {
    DoSaveInputProfile();
  }

  MenuHeading("Input Sources");

#ifdef WITH_SDL2
  DrawToggleSetting(bsi, ICON_FA_COG " Enable SDL Input Source", "The SDL input source supports most controllers.",
                    "InputSources", "SDL", true, true, false);
  DrawToggleSetting(bsi, ICON_FA_WIFI " SDL DualShock 4 / DualSense Enhanced Mode",
                    "Provides vibration and LED control support over Bluetooth.", "InputSources",
                    "SDLControllerEnhancedMode", false, bsi->GetBoolValue("InputSources", "SDL", true), false);
#endif
#ifdef WITH_EVDEV
  DrawToggleSetting(bsi, ICON_FA_COG " Enable Evdev Input Source",
                    "You can use evdev as a fallback if SDL doesn't work with your device.", "InputSources", "Evdev",
                    false);
#endif
#ifdef _WIN32
  DrawToggleSetting(bsi, ICON_FA_COG " Enable XInput Input Source",
                    "The XInput source provides support for XBox 360/XBox One/XBox Series controllers.", "InputSources",
                    "XInput", false);
#endif

  MenuHeading("Multitap");
  DrawEnumSetting(bsi, ICON_FA_PLUS_SQUARE " Multitap Mode",
                  "Enables an additional three controller slots on each port. Not supported in all games.",
                  "ControllerPorts", "MultitapMode", Settings::DEFAULT_MULTITAP_MODE, &Settings::ParseMultitapModeName,
                  &Settings::GetMultitapModeName, &Settings::GetMultitapModeDisplayName, MultitapMode::Count);

  // load mtap settings
  MultitapMode mtap_mode = g_settings.multitap_mode;
  if (IsEditingGameSettings(bsi))
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
                   fmt::format(ICON_FA_PLUG " Controller Port {}{}", mtap_port + 1, mtap_slot_names[mtap_slot]) :
                   fmt::format(ICON_FA_PLUG " Controller Port {}", mtap_port + 1))
                  .c_str());

    const std::string section(fmt::format("Pad{}", global_slot + 1));
    const std::string type(bsi->GetStringValue(section.c_str(), "Type", Controller::GetDefaultPadType(global_slot)));
    const Controller::ControllerInfo* ci = Controller::GetControllerInfo(type);
    if (MenuButton(fmt::format(ICON_FA_GAMEPAD " Controller Type##type{}", global_slot).c_str(),
                   ci ? ci->display_name : "Unknown"))
    {
      std::vector<std::pair<std::string, std::string>> raw_options(Controller::GetControllerTypeNames());
      ImGuiFullscreen::ChoiceDialogOptions options;
      options.reserve(raw_options.size());
      for (auto& it : raw_options)
      {
        options.emplace_back(std::move(it.second), type == it.first);
      }
      OpenChoiceDialog(fmt::format("Port {} Controller Type", global_slot + 1).c_str(), false, std::move(options),
                       [game_settings, section,
                        raw_options = std::move(raw_options)](s32 index, const std::string& title, bool checked) {
                         if (index < 0)
                           return;

                         auto lock = Host::GetSettingsLock();
                         SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                         bsi->SetStringValue(section.c_str(), "Type", raw_options[index].first.c_str());
                         SetSettingsChanged(bsi);
                         CloseChoiceDialog();
                       });
    }

    if (!ci || ci->num_bindings == 0)
      continue;

    if (MenuButton(ICON_FA_MAGIC " Automatic Mapping", "Attempts to map the selected port to a chosen controller."))
      StartAutomaticBinding(global_slot);

    for (u32 i = 0; i < ci->num_bindings; i++)
    {
      const Controller::ControllerBindingInfo& bi = ci->bindings[i];
      DrawInputBindingButton(bsi, bi.type, section.c_str(), bi.name, bi.display_name, true);
    }

    MenuHeading((mtap_enabled[mtap_port] ? fmt::format(ICON_FA_MICROCHIP " Controller Port {}{} Macros", mtap_port + 1,
                                                       mtap_slot_names[mtap_slot]) :
                                           fmt::format(ICON_FA_MICROCHIP " Controller Port {} Macros", mtap_port + 1))
                  .c_str());

    for (u32 macro_index = 0; macro_index < InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER; macro_index++)
    {
      DrawInputBindingButton(bsi, Controller::ControllerBindingType::Macro, section.c_str(),
                             fmt::format("Macro{}", macro_index + 1).c_str(),
                             fmt::format("Macro {} Trigger", macro_index + 1).c_str());

      std::string binds_string(
        bsi->GetStringValue(section.c_str(), fmt::format("Macro{}Binds", macro_index + 1).c_str()));
      if (MenuButton(fmt::format(ICON_FA_KEYBOARD " Macro {} Buttons", macro_index + 1).c_str(),
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
                         [game_settings, section, macro_index, ci](s32 index, const std::string& title, bool checked) {
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
                           SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
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
      const std::string freq_title(fmt::format(ICON_FA_LIGHTBULB " Macro {} Frequency", macro_index + 1));
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

    if (ci->num_settings > 0)
    {
      MenuHeading(
        (mtap_enabled[mtap_port] ?
           fmt::format(ICON_FA_SLIDERS_H " Controller Port {}{} Settings", mtap_port + 1, mtap_slot_names[mtap_slot]) :
           fmt::format(ICON_FA_SLIDERS_H " Controller Port {} Settings", mtap_port + 1))
          .c_str());

      for (u32 i = 0; i < ci->num_settings; i++)
      {
        const SettingInfo& si = ci->settings[i];
        TinyString title;
        title.Fmt(ICON_FA_COG " {}", si.display_name);
        switch (si.type)
        {
          case SettingInfo::Type::Boolean:
            DrawToggleSetting(bsi, title, si.description, section.c_str(), si.name, si.BooleanDefaultValue(), true,
                              false);
            break;
          case SettingInfo::Type::Integer:
            DrawIntRangeSetting(bsi, title, si.description, section.c_str(), si.name, si.IntegerDefaultValue(),
                                si.IntegerMinValue(), si.IntegerMaxValue(), si.format, true);
            break;
          case SettingInfo::Type::IntegerList:
            DrawIntListSetting(bsi, title, si.description, section.c_str(), si.name, si.IntegerDefaultValue(),
                               si.options, 0, si.IntegerMinValue(), true);
            break;
          case SettingInfo::Type::Float:
            DrawFloatRangeSetting(bsi, title, si.description, section.c_str(), si.name, si.FloatDefaultValue(),
                                  si.FloatMinValue(), si.FloatMaxValue(), si.format, si.multiplier, true);
            break;
          default:
            break;
        }
      }
    }
  }

  EndMenuButtons();
}

void FullscreenUI::DrawHotkeySettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

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

    DrawInputBindingButton(bsi, Controller::ControllerBindingType::Button, "Hotkeys", hotkey->name,
                           hotkey->display_name, false);
  }

  EndMenuButtons();
}

void FullscreenUI::DrawMemoryCardSettingsPage()
{
  static constexpr const auto type_keys = make_array("Card1Type", "Card2Type");
  static constexpr const auto path_keys = make_array("Card1Path", "Card2Path");

  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  BeginMenuButtons();

  MenuHeading("Settings and Operations");
  if (MenuButton(ICON_FA_PLUS " Create Memory Card", "Creates a new memory card file or folder."))
  {
    OpenInputStringDialog(
      ICON_FA_PLUS " Create Memory Card", "Enter the name of the memory card you wish to create.",
      "Card Name: ", ICON_FA_FOLDER_PLUS " Create", [](std::string memcard_name) {
        if (memcard_name.empty())
          return;

        const std::string filename(Path::Combine(EmuFolders::MemoryCards, fmt::format("{}.mcd", memcard_name)));
        if (!FileSystem::FileExists(filename.c_str()))
        {
          MemoryCardImage::DataArray data;
          MemoryCardImage::Format(&data);
          if (!FileSystem::WriteBinaryFile(filename.c_str(), data.data(), data.size()))
          {
            FileSystem::DeleteFile(filename.c_str());
            ShowToast(std::string(), fmt::format("Failed to create memory card '{}'.", memcard_name));
          }
          else
          {
            ShowToast(std::string(), fmt::format("Memory card '{}' created.", memcard_name));
          }
        }
        else
        {
          ShowToast(std::string(), fmt::format("A memory card with the name '{}' already exists.", memcard_name));
        }
      });
  }

  DrawFolderSetting(bsi, ICON_FA_FOLDER_OPEN " Memory Card Directory", "MemoryCards", "Directory",
                    EmuFolders::MemoryCards);

  if (!game_settings && MenuButton(ICON_FA_MAGIC " Reset Memory Card Directory",
                                   "Resets memory card directory to default (user directory)."))
  {
    bsi->SetStringValue("MemoryCards", "Directory", "memcards");
    SetSettingsChanged(bsi);
  }

  DrawToggleSetting(bsi, ICON_FA_SEARCH " Use Single Card For Sub-Images",
                    "When using a multi-disc image (m3u/pbp) and per-game (title) memory cards, "
                    "use a single memory card for all discs.",
                    "MemoryCards", "UsePlaylistTitle", true);

  for (u32 i = 0; i < 2; i++)
  {
    MenuHeading(TinyString::FromFormat("Memory Card Port %u", i + 1));

    const MemoryCardType default_type =
      (i == 0) ? Settings::DEFAULT_MEMORY_CARD_1_TYPE : Settings::DEFAULT_MEMORY_CARD_2_TYPE;
    DrawEnumSetting(bsi, TinyString::FromFmt(ICON_FA_SD_CARD " Memory Card {} Type", i + 1),
                    SmallString::FromFmt("Sets which sort of memory card image will be used for slot {}.", i + 1),
                    "MemoryCards", type_keys[i], default_type, &Settings::ParseMemoryCardTypeName,
                    &Settings::GetMemoryCardTypeName, &Settings::GetMemoryCardTypeDisplayName, MemoryCardType::Count);

    const MemoryCardType effective_type =
      Settings::ParseMemoryCardTypeName(
        GetEffectiveStringSetting(bsi, "MemoryCards", type_keys[i], Settings::GetMemoryCardTypeName(default_type))
          .c_str())
        .value_or(default_type);
    const bool is_shared = (effective_type == MemoryCardType::Shared);
    std::optional<std::string> path_value(bsi->GetOptionalStringValue(
      "MemoryCards", path_keys[i],
      IsEditingGameSettings(bsi) ? std::nullopt :
                                   std::optional<const char*>((i == 0) ? "shared_card_1.mcd" : "shared_card_2.mcd")));

    TinyString title;
    title.Fmt(ICON_FA_FILE " Shared Card Name##card_name_{}", i);
    if (MenuButtonWithValue(title, "The selected memory card image will be used in shared mode for this slot.",
                            path_value.has_value() ? path_value->c_str() : "Use Global Setting", is_shared))
    {
      ImGuiFullscreen::ChoiceDialogOptions options;
      std::vector<std::string> names;
      if (IsEditingGameSettings(bsi))
        options.emplace_back("Use Global Setting", !path_value.has_value());
      if (path_value.has_value() && !path_value->empty())
      {
        options.emplace_back(fmt::format("{} (Current)", path_value.value()), true);
        names.push_back(std::move(path_value.value()));
      }

      FileSystem::FindResultsArray results;
      FileSystem::FindFiles(EmuFolders::MemoryCards.c_str(), "*.mcd",
                            FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RELATIVE_PATHS,
                            &results);
      for (FILESYSTEM_FIND_DATA& ffd : results)
      {
        const bool selected = (path_value.has_value() && ffd.FileName == path_value.value());
        options.emplace_back(std::move(ffd.FileName), selected);
      }

      OpenChoiceDialog(
        title, false, std::move(options),
        [game_settings = IsEditingGameSettings(bsi), i](s32 index, const std::string& title, bool checked) {
          if (index < 0)
            return;

          auto lock = Host::GetSettingsLock();
          SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
          if (game_settings && index == 0)
          {
            bsi->DeleteValue("MemoryCards", path_keys[i]);
          }
          else
          {
            if (game_settings)
              index--;
            bsi->SetStringValue("MemoryCards", path_keys[i], title.c_str());
          }
          SetSettingsChanged(bsi);
          CloseChoiceDialog();
        });
    }
  }

  EndMenuButtons();
}

void FullscreenUI::DrawDisplaySettingsPage()
{
  static constexpr auto resolution_scales =
    make_array("Automatic based on window size", "1x", "2x", "3x (for 720p)", "4x", "5x (for 1080p)", "6x (for 1440p)",
               "7x", "8x", "9x (for 4K)", "10x", "11x", "12x", "13x", "14x", "15x", "16x");

  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  BeginMenuButtons();

  MenuHeading("Device Settings");

  DrawEnumSetting(bsi, "GPU Renderer", "Chooses the backend to use for rendering the console/game visuals.", "GPU",
                  "Renderer", Settings::DEFAULT_GPU_RENDERER, &Settings::ParseRendererName, &Settings::GetRendererName,
                  &Settings::GetRendererDisplayName, GPURenderer::Count);

  const GPURenderer renderer =
    Settings::ParseRendererName(
      GetEffectiveStringSetting(bsi, "GPU", "Renderer", Settings::GetRendererName(Settings::DEFAULT_GPU_RENDERER))
        .c_str())
      .value_or(Settings::DEFAULT_GPU_RENDERER);
  const bool is_hardware = (renderer != GPURenderer::Software);

  std::optional<std::string> adapter(
    bsi->GetOptionalStringValue("GPU", "Adapter", game_settings ? std::nullopt : std::optional<const char*>("")));

  if (MenuButtonWithValue("GPU Adapter", "Selects the GPU to use for rendering.",
                          adapter.has_value() ? (adapter->empty() ? "Default" : adapter->c_str()) :
                                                "Use Global Setting"))
  {
    HostDisplay::AdapterAndModeList aml(g_host_display->GetAdapterAndModeList());

    ImGuiFullscreen::ChoiceDialogOptions options;
    options.reserve(aml.adapter_names.size() + 2);
    if (game_settings)
      options.emplace_back("Use Global Setting", !adapter.has_value());
    options.emplace_back("Default", adapter.has_value() && adapter->empty());
    for (std::string& mode : aml.adapter_names)
    {
      const bool checked = (adapter.has_value() && mode == adapter.value());
      options.emplace_back(std::move(mode), checked);
    }

    auto callback = [game_settings](s32 index, const std::string& title, bool checked) {
      if (index < 0)
        return;

      const char* value;
      if (game_settings && index == 0)
        value = nullptr;
      else if ((!game_settings && index == 0) || (game_settings && index == 1))
        value = "";
      else
        value = title.c_str();

      SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
      if (!value)
        bsi->DeleteValue("GPU", "Adapter");
      else
        bsi->SetStringValue("GPU", "Adapter", value);
      SetSettingsChanged(bsi);
      ShowToast(std::string(), "GPU adapter will be applied after restarting.", 10.0f);
      CloseChoiceDialog();
    };
    OpenChoiceDialog(ICON_FA_TV " GPU Adapter", false, std::move(options), std::move(callback));
  }

  std::optional<std::string> fsmode(bsi->GetOptionalStringValue(
    "GPU", "FullscreenMode", game_settings ? std::nullopt : std::optional<const char*>("")));

  if (MenuButtonWithValue("Fullscreen Resolution", "Selects the resolution to use in fullscreen modes.",
                          fsmode.has_value() ? (fsmode->empty() ? "Borderless Fullscreen" : fsmode->c_str()) :
                                               "Use Global Setting"))
  {
    HostDisplay::AdapterAndModeList aml(g_host_display->GetAdapterAndModeList());

    ImGuiFullscreen::ChoiceDialogOptions options;
    options.reserve(aml.fullscreen_modes.size() + 2);
    if (game_settings)
      options.emplace_back("Use Global Setting", !fsmode.has_value());
    options.emplace_back("Borderless Fullscreen", fsmode.has_value() && fsmode->empty());
    for (std::string& mode : aml.fullscreen_modes)
    {
      const bool checked = (fsmode.has_value() && mode == fsmode.value());
      options.emplace_back(std::move(mode), checked);
    }

    auto callback = [game_settings](s32 index, const std::string& title, bool checked) {
      if (index < 0)
        return;

      const char* value;
      if (game_settings && index == 0)
        value = nullptr;
      else if ((!game_settings && index == 0) || (game_settings && index == 1))
        value = "";
      else
        value = title.c_str();

      SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
      if (!value)
        bsi->DeleteValue("GPU", "FullscreenMode");
      else
        bsi->SetStringValue("GPU", "FullscreenMode", value);
      SetSettingsChanged(bsi);
      ShowToast(std::string(), "Resolution change will be applied after restarting.", 10.0f);
      CloseChoiceDialog();
    };
    OpenChoiceDialog(ICON_FA_TV " Fullscreen Resolution", false, std::move(options), std::move(callback));
  }

  switch (renderer)
  {
#ifdef _WIN32
    case GPURenderer::HardwareD3D11:
    {
      DrawToggleSetting(bsi, "Use Blit Swap Chain",
                        "Uses a blit presentation model instead of flipping. This may be needed on some systems.",
                        "Display", "UseBlitSwapChain", false);
    }
    break;
#endif

#ifdef WITH_VULKAN
    case GPURenderer::HardwareVulkan:
    {
      DrawToggleSetting(bsi, "Threaded Presentation",
                        "Presents frames on a background thread when fast forwarding or vsync is disabled.", "GPU",
                        "ThreadedPresentation", true);
    }
    break;
#endif

    case GPURenderer::Software:
    {
      DrawToggleSetting(bsi, "Threaded Rendering",
                        "Uses a second thread for drawing graphics. Speed boost, and safe to use.", "GPU", "UseThread",
                        true);
    }
    break;

    default:
      break;
  }

  if (renderer != GPURenderer::Software)
  {
    DrawToggleSetting(bsi, "Use Software Renderer For Readbacks",
                      "Runs the software renderer in parallel for VRAM readbacks. On some systems, this may result "
                      "in greater performance.",
                      "GPU", "UseSoftwareRendererForReadbacks", false);
  }

  DrawToggleSetting(bsi, "Enable VSync",
                    "Synchronizes presentation of the console's frames to the host. Enable for smoother animations.",
                    "Display", "VSync", Settings::DEFAULT_VSYNC_VALUE);

  DrawToggleSetting(bsi, "Sync To Host Refresh Rate",
                    "Adjusts the emulation speed so the console's refresh rate matches the host when VSync and Audio "
                    "Resampling are enabled.",
                    "Main", "SyncToHostRefreshRate", false);

  DrawToggleSetting(
    bsi, "Optimal Frame Pacing",
    "Ensures every frame generated is displayed for optimal pacing. Disable if you are having speed or sound issues.",
    "Display", "DisplayAllFrames", false);

  MenuHeading("Rendering");

  DrawIntListSetting(
    bsi, "Internal Resolution Scale",
    "Scales internal VRAM resolution by the specified multiplier. Some games require 1x VRAM resolution.", "GPU",
    "ResolutionScale", 1, resolution_scales.data(), resolution_scales.size(), 0, is_hardware);

  DrawEnumSetting(bsi, "Texture Filtering",
                  "Smooths out the blockiness of magnified textures on 3D objects. Will have a greater effect "
                  "on higher resolution scales. The JINC2 and especially xBR filtering modes are very demanding,"
                  "and may not be worth the speed penalty.",
                  "GPU", "TextureFilter", Settings::DEFAULT_GPU_TEXTURE_FILTER, &Settings::ParseTextureFilterName,
                  &Settings::GetTextureFilterName, &Settings::GetTextureFilterDisplayName, GPUTextureFilter::Count,
                  is_hardware);

  DrawToggleSetting(bsi, "True Color Rendering",
                    "Disables dithering and uses the full 8 bits per channel of color information. May break "
                    "rendering in some games.",
                    "GPU", "TrueColor", true, is_hardware);

  DrawToggleSetting(bsi, "Widescreen Hack",
                    "Increases the field of view from 4:3 to the chosen display aspect ratio in 3D games.", "GPU",
                    "WidescreenHack", false, is_hardware);

  DrawToggleSetting(bsi, "PGXP Geometry Correction",
                    "Reduces \"wobbly\" polygons by attempting to preserve the fractional component through memory "
                    "transfers.",
                    "GPU", "PGXPEnable", false);

  MenuHeading("Screen Display");

  DrawEnumSetting(bsi, "Aspect Ratio", "Changes the aspect ratio used to display the console's output to the screen.",
                  "Display", "AspectRatio", Settings::DEFAULT_DISPLAY_ASPECT_RATIO, &Settings::ParseDisplayAspectRatio,
                  &Settings::GetDisplayAspectRatioName, &Settings::GetDisplayAspectRatioName,
                  DisplayAspectRatio::Count);

  DrawEnumSetting(bsi, "Crop Mode",
                  "Determines how much of the area typically not visible on a consumer TV set to crop/hide.", "Display",
                  "CropMode", Settings::DEFAULT_DISPLAY_CROP_MODE, &Settings::ParseDisplayCropMode,
                  &Settings::GetDisplayCropModeName, &Settings::GetDisplayCropModeDisplayName, DisplayCropMode::Count);

  DrawEnumSetting(bsi, "Position", "Determines the position on the screen when black borders must be added.", "Display",
                  "Alignment", Settings::DEFAULT_DISPLAY_ALIGNMENT, &Settings::ParseDisplayAlignment,
                  &Settings::GetDisplayAlignmentDisplayName, &Settings::GetDisplayAlignmentDisplayName,
                  DisplayAlignment::Count);

  DrawEnumSetting(bsi, "Downsampling",
                  "Downsamples the rendered image prior to displaying it. Can improve "
                  "overall image quality in mixed 2D/3D games.",
                  "GPU", "DownsampleMode", Settings::DEFAULT_GPU_DOWNSAMPLE_MODE, &Settings::ParseDownsampleModeName,
                  &Settings::GetDownsampleModeName, &Settings::GetDownsampleModeDisplayName, GPUDownsampleMode::Count,
                  (renderer != GPURenderer::Software));

  DrawToggleSetting(bsi, "Linear Upscaling",
                    "Uses a bilinear filter when upscaling to display, smoothing out the image.", "Display",
                    "LinearFiltering", true);

  DrawToggleSetting(bsi, "Integer Upscaling", "Adds padding to ensure pixels are a whole number in size.", "Display",
                    "IntegerScaling", false);

  DrawToggleSetting(bsi, "Stretch To Fit",
                    "Fills the window with the active display area, regardless of the aspect ratio.", "Display",
                    "Stretch", false);

  DrawToggleSetting(bsi, "Internal Resolution Screenshots",
                    "Saves screenshots at internal render resolution and without postprocessing.", "Display",
                    "InternalResolutionScreenshots", false);

  MenuHeading("Enhancements");
  DrawToggleSetting(bsi, "Scaled Dithering",
                    "Scales the dithering pattern with the internal rendering resolution, making it less noticeable. "
                    "Usually safe to enable.",
                    "GPU", "ScaledDithering", true, is_hardware);

  DrawToggleSetting(bsi, "Disable Interlacing",
                    "Disables interlaced rendering and display in the GPU. Some games can render in 480p this way, "
                    "but others will break.",
                    "GPU", "DisableInterlacing", true);
  DrawToggleSetting(bsi, "Force NTSC Timings",
                    "Forces PAL games to run at NTSC timings, i.e. 60hz. Some PAL games will run at their \"normal\" "
                    "speeds, while others will break.",
                    "GPU", "ForceNTSCTimings", false);
  DrawToggleSetting(bsi, "Force 4:3 For 24-Bit Display",
                    "Switches back to 4:3 display aspect ratio when displaying 24-bit content, usually FMVs.",
                    "Display", "Force4_3For24Bit", false);
  DrawToggleSetting(bsi, "Chroma Smoothing For 24-Bit Display",
                    "Smooths out blockyness between colour transitions in 24-bit content, usually FMVs. Only applies "
                    "to the hardware renderers.",
                    "GPU", "ChromaSmoothing24Bit", false);

  MenuHeading("PGXP (Precision Geometry Transform Pipeline)");

  const bool pgxp_enabled = GetEffectiveBoolSetting(bsi, "GPU", "PGXPEnable", false);
  const bool texture_correction_enabled = GetEffectiveBoolSetting(bsi, "GPU", "PGXPTextureCorrection", true);

  DrawToggleSetting(
    bsi, "Perspective Correct Textures",
    "Uses perspective-correct interpolation for texture coordinates, straightening out warped textures.", "GPU",
    "PGXPTextureCorrection", true, pgxp_enabled);
  DrawToggleSetting(bsi, "Perspective Correct Colors",
                    "Uses perspective-correct interpolation for colors, which can improve visuals in some games.",
                    "GPU", "PGXPColorCorrection", false, pgxp_enabled);
  DrawToggleSetting(bsi, "Culling Correction",
                    "Increases the precision of polygon culling, reducing the number of holes in geometry.", "GPU",
                    "PGXPCulling", true, pgxp_enabled);
  DrawToggleSetting(bsi, "Preserve Projection Precision",
                    "Adds additional precision to PGXP data post-projection. May improve visuals in some games.", "GPU",
                    "PGXPPreserveProjFP", false, pgxp_enabled);
  DrawToggleSetting(bsi, "Depth Buffer",
                    "Reduces polygon Z-fighting through depth testing. Low compatibility with games.", "GPU",
                    "PGXPDepthBuffer", false, pgxp_enabled && texture_correction_enabled);
  DrawToggleSetting(bsi, "CPU Mode", "Uses PGXP for all instructions, not just memory operations.", "GPU", "PGXPCPU",
                    false, pgxp_enabled);

  MenuHeading("Texture Replacements");

  DrawToggleSetting(bsi, "Enable VRAM Write Texture Replacement",
                    "Enables the replacement of background textures in supported games.", "TextureReplacements",
                    "EnableVRAMWriteReplacements", false);
  DrawToggleSetting(bsi, "Preload Replacement Textures",
                    "Loads all replacement texture to RAM, reducing stuttering at runtime.", "TextureReplacements",
                    "PreloadTextures", false);

  EndMenuButtons();
}

void FullscreenUI::PopulatePostProcessingChain()
{
  std::string chain_value(GetEditingSettingsInterface()->GetStringValue("Display", "PostProcessChain", ""));
  s_postprocessing_chain.CreateFromString(chain_value);
}

void FullscreenUI::SavePostProcessingChain()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();
  const std::string config(s_postprocessing_chain.GetConfigString());
  bsi->SetStringValue("Display", "PostProcessChain", config.c_str());
  if (bsi->GetBoolValue("Display", "PostProcessing", false))
    g_host_display->SetPostProcessingChain(config);
  if (IsEditingGameSettings(bsi))
    s_game_settings_interface->Save();
  else
    Host::CommitBaseSettingChanges();
}

void FullscreenUI::DrawPostProcessingSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  BeginMenuButtons();

  MenuHeading("Controls");

  DrawToggleSetting(bsi, ICON_FA_MAGIC " Enable Post Processing",
                    "If not enabled, the current post processing chain will be ignored.", "Display", "PostProcessing",
                    false);

  if (MenuButton(ICON_FA_SEARCH " Reload Shaders", "Reloads the shaders from disk, applying any changes.",
                 bsi->GetBoolValue("Display", "PostProcessing", false)))
  {
    const std::string chain(bsi->GetStringValue("Display", "PostProcessChain", ""));
    g_host_display->SetPostProcessingChain(chain);
    if (chain.empty())
      ShowToast(std::string(), "Post-processing chain is empty.");
    else
      ShowToast(std::string(), "Post-processing shaders reloaded.");
  }

  MenuHeading("Operations");

  if (MenuButton(ICON_FA_PLUS " Add Shader", "Adds a new shader to the chain."))
  {
    ImGuiFullscreen::ChoiceDialogOptions options;
    for (std::string& name : FrontendCommon::PostProcessingChain::GetAvailableShaderNames())
      options.emplace_back(std::move(name), false);

    OpenChoiceDialog(
      ICON_FA_PLUS " Add Shader", false, std::move(options), [](s32 index, const std::string& title, bool checked) {
        if (index < 0)
          return;

        if (s_postprocessing_chain.AddStage(title))
        {
          ShowToast(std::string(),
                    fmt::format("Shader {} added as stage {}.", title, s_postprocessing_chain.GetStageCount()));
          SavePostProcessingChain();
        }
        else
        {
          ShowToast(std::string(), fmt::format("Failed to load shader {}. It may be invalid.", title));
        }

        CloseChoiceDialog();
      });
  }

  if (MenuButton(ICON_FA_TIMES " Clear Shaders", "Clears a shader from the chain."))
  {
    OpenConfirmMessageDialog(
      ICON_FA_TIMES " Clear Shaders",
      "Are you sure you want to clear the current post-processing chain? All configuration will be lost.",
      [](bool confirmed) {
        if (!confirmed)
          return;

        s_postprocessing_chain.ClearStages();
        ShowToast(std::string(), "Post-processing chain cleared.");
      });
  }

  SmallString str;
  SmallString tstr;
  for (u32 stage_index = 0; stage_index < s_postprocessing_chain.GetStageCount();)
  {
    FrontendCommon::PostProcessingShader& stage = s_postprocessing_chain.GetShaderStage(stage_index);
    str.Fmt("Stage {}: {}", stage_index + 1, stage.GetName());
    MenuHeading(str);

    if (MenuButton(ICON_FA_TIMES " Remove From Chain", "Removes this shader from the chain."))
    {
      ShowToast(std::string(), fmt::format("Removed stage {} ({}).", stage_index + 1, stage.GetName()));
      s_postprocessing_chain.RemoveStage(stage_index);
      continue;
    }

    if (MenuButton(ICON_FA_ARROW_UP " Move Up", "Moves this shader higher in the chain, applying it earlier.",
                   (stage_index > 0)))
    {
      s_postprocessing_chain.MoveStageUp(stage_index);
      continue;
    }

    if (MenuButton(ICON_FA_ARROW_DOWN " Move Down", "Moves this shader lower in the chain, applying it later.",
                   (stage_index != (s_postprocessing_chain.GetStageCount() - 1))))
    {
      s_postprocessing_chain.MoveStageDown(stage_index);
      continue;
    }

    for (FrontendCommon::PostProcessingShader::Option& opt : stage.GetOptions())
    {
      switch (opt.type)
      {
        case FrontendCommon::PostProcessingShader::Option::Type::Bool:
        {
          bool value = (opt.value[0].int_value != 0);
          tstr.Fmt(ICON_FA_COGS " {}", opt.ui_name);
          str.Fmt("Default: {}", (opt.default_value[0].int_value != 0) ? "Enabled" : "Disabled");
          if (ToggleButton(tstr, str, &value))
          {
            opt.value[0].int_value = (value != 0);
            SavePostProcessingChain();
          }
        }
        break;

        case FrontendCommon::PostProcessingShader::Option::Type::Float:
        {
          tstr.Fmt(ICON_FA_RULER_VERTICAL " {}##{}", opt.ui_name, opt.name);
          str.Fmt("Value: {} | Default: {} | Minimum: {} | Maximum: {}", opt.value[0].float_value,
                  opt.default_value[0].float_value, opt.min_value[0].float_value, opt.max_value[0].float_value);
          if (MenuButton(tstr, str))
            ImGui::OpenPopup(tstr);

          ImGui::SetNextWindowSize(LayoutScale(500.0f, 190.0f));
          ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

          ImGui::PushFont(g_large_font);
          ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
          ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                                      ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
          ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

          bool is_open = true;
          if (ImGui::BeginPopupModal(tstr, &is_open,
                                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
          {
            BeginMenuButtons();

            const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();

#if 0
            for (u32 i = 0; i < opt.vector_size; i++)
            {
              static constexpr const char* components[] = { "X", "Y", "Z", "W" };
              if (opt.vector_size == 1)
                tstr.Assign("##value");
              else
                tstr.Fmt("{}##value{}", components[i], i);

              ImGui::SetNextItemWidth(end);
              if (ImGui::SliderFloat(tstr, &opt.value[i].float_value, opt.min_value[i].float_value,
                opt.max_value[i].float_value, "%f", ImGuiSliderFlags_NoInput))
              {
                SavePostProcessingChain();
              }
            }
#else
            ImGui::SetNextItemWidth(end);
            switch (opt.vector_size)
            {
              case 1:
              {
                if (ImGui::SliderFloat("##value", &opt.value[0].float_value, opt.min_value[0].float_value,
                                       opt.max_value[0].float_value))
                {
                  SavePostProcessingChain();
                }
              }
              break;

              case 2:
              {
                if (ImGui::SliderFloat2("##value", &opt.value[0].float_value, opt.min_value[0].float_value,
                                        opt.max_value[0].float_value))
                {
                  SavePostProcessingChain();
                }
              }
              break;

              case 3:
              {
                if (ImGui::SliderFloat3("##value", &opt.value[0].float_value, opt.min_value[0].float_value,
                                        opt.max_value[0].float_value))
                {
                  SavePostProcessingChain();
                }
              }
              break;

              case 4:
              {
                if (ImGui::SliderFloat4("##value", &opt.value[0].float_value, opt.min_value[0].float_value,
                                        opt.max_value[0].float_value))
                {
                  SavePostProcessingChain();
                }
              }
              break;
            }
#endif

            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
            if (MenuButtonWithoutSummary("OK", true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font,
                                         ImVec2(0.5f, 0.0f)))
            {
              ImGui::CloseCurrentPopup();
            }
            EndMenuButtons();

            ImGui::EndPopup();
          }

          ImGui::PopStyleVar(3);
          ImGui::PopFont();
        }
        break;

        case FrontendCommon::PostProcessingShader::Option::Type::Int:
        {
          tstr.Fmt(ICON_FA_RULER_VERTICAL " {}##{}", opt.ui_name, opt.name);
          str.Fmt("Value: {} | Default: {} | Minimum: {} | Maximum: {}", opt.value[0].int_value,
                  opt.default_value[0].int_value, opt.min_value[0].int_value, opt.max_value[0].int_value);
          if (MenuButton(tstr, str))
            ImGui::OpenPopup(tstr);

          ImGui::SetNextWindowSize(LayoutScale(500.0f, 190.0f));
          ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

          ImGui::PushFont(g_large_font);
          ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
          ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                                      ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
          ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

          bool is_open = true;
          if (ImGui::BeginPopupModal(tstr, &is_open,
                                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
          {
            BeginMenuButtons();

            const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();

#if 0
            for (u32 i = 0; i < opt.vector_size; i++)
            {
              static constexpr const char* components[] = { "X", "Y", "Z", "W" };
              if (opt.vector_size == 1)
                tstr.Assign("##value");
              else
                tstr.Fmt("{}##value{}", components[i], i);

              ImGui::SetNextItemWidth(end);
              if (ImGui::SliderInt(tstr, &opt.value[i].int_value, opt.min_value[i].int_value,
                opt.max_value[i].int_value, "%d", ImGuiSliderFlags_NoInput))
              {
                SavePostProcessingChain();
              }
            }
#else
            ImGui::SetNextItemWidth(end);
            switch (opt.vector_size)
            {
              case 1:
              {
                if (ImGui::SliderInt("##value", &opt.value[0].int_value, opt.min_value[0].int_value,
                                     opt.max_value[0].int_value))
                {
                  SavePostProcessingChain();
                }
              }
              break;

              case 2:
              {
                if (ImGui::SliderInt2("##value", &opt.value[0].int_value, opt.min_value[0].int_value,
                                      opt.max_value[0].int_value))
                {
                  SavePostProcessingChain();
                }
              }
              break;

              case 3:
              {
                if (ImGui::SliderInt2("##value", &opt.value[0].int_value, opt.min_value[0].int_value,
                                      opt.max_value[0].int_value))
                {
                  SavePostProcessingChain();
                }
              }
              break;

              case 4:
              {
                if (ImGui::SliderInt4("##value", &opt.value[0].int_value, opt.min_value[0].int_value,
                                      opt.max_value[0].int_value))
                {
                  SavePostProcessingChain();
                }
              }
              break;
            }
#endif

            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
            if (MenuButtonWithoutSummary("OK", true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font,
                                         ImVec2(0.5f, 0.0f)))
            {
              ImGui::CloseCurrentPopup();
            }
            EndMenuButtons();

            ImGui::EndPopup();
          }

          ImGui::PopStyleVar(3);
          ImGui::PopFont();
        }
        break;
      }
    }

    stage_index++;
  }

  EndMenuButtons();
}

void FullscreenUI::DrawAudioSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading("Audio Control");

  DrawIntRangeSetting(bsi, "Output Volume", "Controls the volume of the audio played on the host.", "Audio",
                      "OutputVolume", 100, 0, 100, "%d%%");
  DrawIntRangeSetting(bsi, "Fast Forward Volume",
                      "Controls the volume of the audio played on the host when fast forwarding.", "Audio",
                      "FastForwardVolume", 100, 0, 100, "%d%%");
  DrawToggleSetting(bsi, "Mute All Sound", "Prevents the emulator from producing any audible sound.", "Audio",
                    "OutputMuted", false);
  DrawToggleSetting(bsi, "Mute CD Audio",
                    "Forcibly mutes both CD-DA and XA audio from the CD-ROM. Can be used to "
                    "disable background music in some games.",
                    "CDROM", "MuteCDAudio", false);

  MenuHeading("Backend Settings");

  DrawEnumSetting(bsi, "Audio Backend",
                  "The audio backend determines how frames produced by the emulator are submitted to the host.",
                  "Audio", "Backend", Settings::DEFAULT_AUDIO_BACKEND, &Settings::ParseAudioBackend,
                  &Settings::GetAudioBackendName, &Settings::GetAudioBackendDisplayName, AudioBackend::Count);
  DrawEnumSetting(bsi, "Stretch Mode", "Determines quality of audio when not running at 100% speed.", "Audio",
                  "StretchMode", Settings::DEFAULT_AUDIO_STRETCH_MODE, &AudioStream::ParseStretchMode,
                  &AudioStream::GetStretchModeName, &AudioStream::GetStretchModeDisplayName, AudioStretchMode::Count);
  DrawIntRangeSetting(bsi, "Buffer Size",
                      "Determines the amount of audio buffered before being pulled by the host API.", "Audio",
                      "BufferMS", Settings::DEFAULT_AUDIO_BUFFER_MS, 10, 500, "%d ms");

  const u32 output_latency =
    GetEffectiveUIntSetting(bsi, "Audio", "OutputLatencyMS", Settings::DEFAULT_AUDIO_OUTPUT_LATENCY_MS);
  bool output_latency_minimal = (output_latency == 0);
  if (ToggleButton("Minimal Output Latency",
                   "When enabled, the minimum supported output latency will be used for the host API.",
                   &output_latency_minimal))
  {
    bsi->SetUIntValue("Audio", "OutputLatencyMS",
                      output_latency_minimal ? 0 : Settings::DEFAULT_AUDIO_OUTPUT_LATENCY_MS);
    SetSettingsChanged(bsi);
  }
  if (!output_latency_minimal)
  {
    DrawIntRangeSetting(bsi, "Output Latency",
                        "Determines how much latency there is between the audio being picked up by the host API, and "
                        "played through speakers.",
                        "Audio", "OutputLatencyMS", Settings::DEFAULT_AUDIO_OUTPUT_LATENCY_MS, 1, 500, "%d ms");
  }

  EndMenuButtons();
}

#ifdef WITH_CHEEVOS

void FullscreenUI::DrawAchievementsSettingsPage()
{
#ifdef WITH_RAINTEGRATION
  if (Achievements::IsUsingRAIntegration())
  {
    BeginMenuButtons();
    ActiveButton(ICON_FA_BAN " RAIntegration is being used instead of the built-in achievements implementation.", false,
                 false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    EndMenuButtons();
    return;
  }
#endif

  const auto lock = Achievements::GetLock();
  if (Achievements::IsActive() && !System::IsRunning())
    Achievements::ProcessPendingHTTPRequests();

  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading("Settings");
  DrawToggleSetting(bsi, ICON_FA_TROPHY " Enable Achievements",
                    "When enabled and logged in, DuckStation will scan for achievements on startup.", "Cheevos",
                    "Enabled", false);

  const bool enabled = bsi->GetBoolValue("Cheevos", "Enabled", false);
  const bool challenge = bsi->GetBoolValue("Cheevos", "ChallengeMode", false);

  DrawToggleSetting(bsi, ICON_FA_USER_FRIENDS " Rich Presence",
                    "When enabled, rich presence information will be collected and sent to the server where supported.",
                    "Cheevos", "RichPresence", true, enabled);
  if (DrawToggleSetting(bsi, ICON_FA_HARD_HAT " Hardcore Mode",
                        "\"Challenge\" mode for achievements, including leaderboard tracking. Disables save state, "
                        "cheats, and slowdown functions.",
                        "Cheevos", "ChallengeMode", false, enabled))
  {
    if (System::IsValid() && bsi->GetBoolValue("Cheevos", "ChallengeMode", false))
      ShowToast(std::string(), "Hardcore mode will be enabled on next game restart.");
  }
  DrawToggleSetting(bsi, ICON_FA_LIST_OL " Leaderboards",
                    "Enables tracking and submission of leaderboards in supported games.", "Cheevos", "Leaderboards",
                    true, enabled && challenge);
  DrawToggleSetting(bsi, ICON_FA_HEADPHONES " Sound Effects",
                    "Plays sound effects for events such as achievement unlocks and leaderboard submissions.",
                    "Cheevos", "SoundEffects", true, enabled);
  DrawToggleSetting(
    bsi, ICON_FA_MAGIC " Show Challenge Indicators",
    "Shows icons in the lower-right corner of the screen when a challenge/primed achievement is active.", "Cheevos",
    "PrimedIndicators", true, enabled);
  DrawToggleSetting(bsi, ICON_FA_MEDAL " Test Unofficial Achievements",
                    "When enabled, DuckStation will list achievements from unofficial sets. These achievements are not "
                    "tracked by RetroAchievements.",
                    "Cheevos", "UnofficialTestMode", false, enabled);
  DrawToggleSetting(bsi, ICON_FA_STETHOSCOPE " Test Mode",
                    "When enabled, DuckStation will assume all achievements are locked and not send any unlock "
                    "notifications to the server.",
                    "Cheevos", "TestMode", false, enabled);

  MenuHeading("Account");
  if (Achievements::IsLoggedIn())
  {
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::GetStyle().Colors[ImGuiCol_Text]);
    ActiveButton(SmallString::FromFormat(ICON_FA_USER " Username: %s", Achievements::GetUsername().c_str()), false,
                 false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    TinyString ts_string;
    ts_string.AppendFmtString(
      "{:%Y-%m-%d %H:%M:%S}",
      fmt::localtime(StringUtil::FromChars<u64>(bsi->GetStringValue("Cheevos", "LoginTimestamp", "0")).value_or(0)));
    ActiveButton(SmallString::FromFormat(ICON_FA_CLOCK " Login token generated on %s", ts_string.GetCharArray()), false,
                 false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    ImGui::PopStyleColor();

    if (MenuButton(ICON_FA_KEY " Logout", "Logs out of RetroAchievements."))
    {
      Host::RunOnCPUThread([]() { Achievements::Logout(); });
    }
  }
  else if (Achievements::IsActive())
  {
    ActiveButton(ICON_FA_USER " Not Logged In", false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    if (MenuButton(ICON_FA_KEY " Login", "Logs in to RetroAchievements."))
      ImGui::OpenPopup("Achievements Login");

    DrawAchievementsLoginWindow();
  }
  else
  {
    ActiveButton(ICON_FA_USER " Achievements are disabled.", false, false,
                 ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
  }

  MenuHeading("Current Game");
  if (Achievements::HasActiveGame())
  {
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::GetStyle().Colors[ImGuiCol_Text]);
    ActiveButton(fmt::format(ICON_FA_BOOKMARK " Game ID: {}", Achievements::GetGameID()).c_str(), false, false,
                 LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    ActiveButton(fmt::format(ICON_FA_BOOK " Game Title: {}", Achievements::GetGameTitle()).c_str(), false, false,
                 LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    ActiveButton(fmt::format(ICON_FA_TROPHY " Achievements: {} ({} points)", Achievements::GetAchievementCount(),
                             Achievements::GetMaximumPointsForGame())
                   .c_str(),
                 false, false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    const std::string& rich_presence_string = Achievements::GetRichPresenceString();
    if (!rich_presence_string.empty())
    {
      ActiveButton(fmt::format(ICON_FA_MAP " {}", rich_presence_string).c_str(), false, false,
                   LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    }
    else
    {
      ActiveButton(ICON_FA_MAP " Rich presence inactive or unsupported.", false, false,
                   LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    }

    ImGui::PopStyleColor();
  }
  else
  {
    ActiveButton(ICON_FA_BAN " Game not loaded or no RetroAchievements available.", false, false,
                 LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
  }

  EndMenuButtons();
}

void FullscreenUI::DrawAchievementsLoginWindow()
{
  ImGui::SetNextWindowSize(LayoutScale(700.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));
  ImGui::PushFont(g_large_font);

  bool is_open = true;
  if (ImGui::BeginPopupModal("Achievements Login", &is_open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {

    ImGui::TextWrapped("Please enter your user name and password for retroachievements.org.");
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

    if (ActiveButton(ICON_FA_KEY " Login", false, login_enabled))
    {
      Achievements::LoginAsync(username, password);
      std::memset(username, 0, sizeof(username));
      std::memset(password, 0, sizeof(password));
      ImGui::CloseCurrentPopup();
    }

    if (ActiveButton(ICON_FA_TIMES " Cancel", false))
    {
      std::memset(username, 0, sizeof(username));
      std::memset(password, 0, sizeof(password));
      ImGui::CloseCurrentPopup();
    }

    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopFont();
  ImGui::PopStyleVar(2);
}

#else

void FullscreenUI::DrawAchievementsSettingsPage()
{
  BeginMenuButtons();
  ActiveButton(ICON_FA_BAN " This build was not compiled with RetroAchivements support.", false, false,
               ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
  EndMenuButtons();
}

void FullscreenUI::DrawAchievementsLoginWindow() {}

#endif

void FullscreenUI::DrawAdvancedSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading("Logging Settings");
  DrawEnumSetting(bsi, "Log Level", "Sets the verbosity of messages logged. Higher levels will log more messages.",
                  "Logging", "LogLevel", Settings::DEFAULT_LOG_LEVEL, &Settings::ParseLogLevelName,
                  &Settings::GetLogLevelName, &Settings::GetLogLevelDisplayName, LOGLEVEL_COUNT);
  DrawToggleSetting(bsi, "Log To System Console", "Logs messages to the console window.", "Logging", "LogToConsole",
                    Settings::DEFAULT_LOG_TO_CONSOLE);
  DrawToggleSetting(bsi, "Log To Debug Console", "Logs messages to the debug console where supported.", "Logging",
                    "LogToDebug", false);
  DrawToggleSetting(bsi, "Log To File", "Logs messages to duckstation.log in the user directory.", "Logging",
                    "LogToFile", false);

  MenuHeading("Debugging Settings");

  DrawToggleSetting(bsi, "Disable All Enhancements", "Temporarily disables all enhancements, useful when testing.",
                    "Main", "DisableAllEnhancements", false);

  DrawToggleSetting(bsi, "Use Debug GPU Device",
                    "Enable debugging when supported by the host's renderer API. Only for developer use.", "GPU",
                    "UseDebugDevice", false);

#ifdef _WIN32
  DrawToggleSetting(bsi, "Increase Timer Resolution", "Enables more precise frame pacing at the cost of battery life.",
                    "Main", "IncreaseTimerResolution", true);
#endif

  DrawToggleSetting(bsi, "Allow Booting Without SBI File",
                    "Allows loading protected games without subchannel information.", "CDROM",
                    "AllowBootingWithoutSBIFile", false);

  DrawToggleSetting(bsi, "Create Save State Backups", "Renames existing save states when saving to a backup file.",
                    "Main", "CreateSaveStateBackups", false);

  MenuHeading("Display Settings");
  DrawToggleSetting(bsi, "Show Status Indicators", "Shows persistent icons when turbo is active or when paused.",
                    "Display", "ShowStatusIndicators", true);
  DrawToggleSetting(bsi, "Show Enhancement Settings",
                    "Shows enhancement settings in the bottom-right corner of the screen.", "Display",
                    "ShowEnhancements", false);
  DrawFloatRangeSetting(bsi, "Display FPS Limit",
                        "Limits how many frames are displayed to the screen. These frames are still rendered.",
                        "Display", "MaxFPS", Settings::DEFAULT_DISPLAY_MAX_FPS, 0.0f, 500.0f, "%.2f FPS");

  MenuHeading("PGXP Settings");

  const bool pgxp_enabled = GetEffectiveBoolSetting(bsi, "GPU", "PGXPEnable", false);

  DrawToggleSetting(bsi, "Enable PGXP Vertex Cache",
                    "Uses screen positions to resolve PGXP data. May improve visuals in some games.", "GPU",
                    "PGXPVertexCache", pgxp_enabled);
  DrawFloatRangeSetting(
    bsi, "PGXP Geometry Tolerance",
    "Sets a threshold for discarding precise values when exceeded. May help with glitches in some games.", "GPU",
    "PGXPTolerance", -1.0f, -1.0f, 10.0f, "%.1f", pgxp_enabled);
  DrawFloatRangeSetting(bsi, "PGXP Depth Clear Threshold",
                        "Sets a threshold for discarding the emulated depth buffer. May help in some games.", "GPU",
                        "PGXPDepthBuffer", Settings::DEFAULT_GPU_PGXP_DEPTH_THRESHOLD, 0.0f, 4096.0f, "%.1f",
                        pgxp_enabled);

  MenuHeading("Texture Dumping");

  DrawToggleSetting(bsi, "Dump Replaceable VRAM Writes", "Writes textures which can be replaced to the dump directory.",
                    "TextureReplacements", "DumpVRAMWrites", false);
  DrawToggleSetting(bsi, "Set VRAM Write Dump Alpha Channel", "Clears the mask/transparency bit in VRAM write dumps.",
                    "TextureReplacements", "DumpVRAMWriteForceAlphaChannel", true);

  MenuHeading("CPU Emulation");

  DrawToggleSetting(bsi, "Enable Recompiler ICache",
                    "Simulates the CPU's instruction cache in the recompiler. Can help with games running too fast.",
                    "CPU", "RecompilerICache", false);
  DrawToggleSetting(bsi, "Enable Recompiler Memory Exceptions",
                    "Enables alignment and bus exceptions. Not needed for any known games.", "CPU",
                    "RecompilerMemoryExceptions", false);
  DrawToggleSetting(bsi, "Enable Recompiler Block Linking",
                    "Performance enhancement - jumps directly between blocks instead of returning to the dispatcher.",
                    "CPU", "RecompilerBlockLinking", true);
  DrawEnumSetting(bsi, "Recompiler Fast Memory Access",
                  "Avoids calls to C++ code, significantly speeding up the recompiler.", "CPU", "FastmemMode",
                  Settings::DEFAULT_CPU_FASTMEM_MODE, &Settings::ParseCPUFastmemMode, &Settings::GetCPUFastmemModeName,
                  &Settings::GetCPUFastmemModeDisplayName, CPUFastmemMode::Count);

  EndMenuButtons();
}

void FullscreenUI::DrawPauseMenu(MainWindowType type)
{
  SmallString buffer;

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImVec2 display_size(ImGui::GetIO().DisplaySize);
  dl->AddRectFilled(ImVec2(0.0f, 0.0f), display_size, IM_COL32(0x21, 0x21, 0x21, 200));

  // title info
  {
    const std::string& title = System::GetRunningTitle();
    const std::string& serial = System::GetRunningSerial();

    if (!serial.empty())
      buffer.Format("%s - ", serial.c_str());
    buffer.AppendString(Path::GetFileName(System::GetRunningPath()));

    const ImVec2 title_size(
      g_large_font->CalcTextSizeA(g_large_font->FontSize, std::numeric_limits<float>::max(), -1.0f, title.c_str()));
    const ImVec2 subtitle_size(
      g_medium_font->CalcTextSizeA(g_medium_font->FontSize, std::numeric_limits<float>::max(), -1.0f, buffer));

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

        DrawShadowedText(dl, g_medium_font, rp_pos, IM_COL32(255, 255, 255, 255), rp.data(), rp.data() + rp.size(),
                         wrap_width);
      }
    }
#endif

    DrawShadowedText(dl, g_large_font, title_pos, IM_COL32(255, 255, 255, 255), title.c_str());
    DrawShadowedText(dl, g_medium_font, subtitle_pos, IM_COL32(255, 255, 255, 255), buffer);

    const ImVec2 image_min(display_size.x - LayoutScale(20.0f + 50.0f) - rp_height,
                           display_size.y - LayoutScale(20.0f + 50.0f) - rp_height);
    const ImVec2 image_max(image_min.x + LayoutScale(50.0f) + rp_height, image_min.y + LayoutScale(50.0f) + rp_height);
    dl->AddImage(GetCoverForCurrentGame(), image_min, image_max);
  }

  // current time / play time
  {
    buffer.Fmt("{:%X}", fmt::localtime(std::time(nullptr)));

    const ImVec2 time_size(g_large_font->CalcTextSizeA(g_large_font->FontSize, std::numeric_limits<float>::max(), -1.0f,
                                                       buffer.GetCharArray(),
                                                       buffer.GetCharArray() + buffer.GetLength()));
    const ImVec2 time_pos(display_size.x - LayoutScale(10.0f) - time_size.x, LayoutScale(10.0f));
    DrawShadowedText(dl, g_large_font, time_pos, IM_COL32(255, 255, 255, 255), buffer.GetCharArray(),
                     buffer.GetCharArray() + buffer.GetLength());

    const std::string& serial = System::GetRunningSerial();
    if (!serial.empty())
    {
      const std::time_t cached_played_time = GameList::GetCachedPlayedTimeForSerial(serial);
      const std::time_t session_time = static_cast<std::time_t>(CommonHost::GetSessionPlayedTime());

      buffer.Fmt("Session: {}", GameList::FormatTimespan(session_time, true).GetStringView());
      const ImVec2 session_size(g_medium_font->CalcTextSizeA(g_medium_font->FontSize, std::numeric_limits<float>::max(),
                                                             -1.0f, buffer.GetCharArray(),
                                                             buffer.GetCharArray() + buffer.GetLength()));
      const ImVec2 session_pos(display_size.x - LayoutScale(10.0f) - session_size.x,
                               time_pos.y + g_large_font->FontSize + LayoutScale(4.0f));
      DrawShadowedText(dl, g_medium_font, session_pos, IM_COL32(255, 255, 255, 255), buffer.GetCharArray(),
                       buffer.GetCharArray() + buffer.GetLength());

      buffer.Fmt("All Time: {}", GameList::FormatTimespan(cached_played_time + session_time, true).GetStringView());
      const ImVec2 total_size(g_medium_font->CalcTextSizeA(g_medium_font->FontSize, std::numeric_limits<float>::max(),
                                                           -1.0f, buffer.GetCharArray(),
                                                           buffer.GetCharArray() + buffer.GetLength()));
      const ImVec2 total_pos(display_size.x - LayoutScale(10.0f) - total_size.x,
                             session_pos.y + g_medium_font->FontSize + LayoutScale(4.0f));
      DrawShadowedText(dl, g_medium_font, total_pos, IM_COL32(255, 255, 255, 255), buffer.GetCharArray(),
                       buffer.GetCharArray() + buffer.GetLength());
    }
  }

  const ImVec2 window_size(LayoutScale(500.0f, LAYOUT_SCREEN_HEIGHT));
  const ImVec2 window_pos(0.0f, display_size.y - window_size.y);

  if (BeginFullscreenWindow(window_pos, window_size, "pause_menu", ImVec4(0.0f, 0.0f, 0.0f, 0.0f), 0.0f, 10.0f,
                            ImGuiWindowFlags_NoBackground))
  {
    static constexpr u32 submenu_item_count[] = {
      12, // None
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
        const bool has_game = System::IsValid() && !System::GetRunningSerial().empty();

        if (ActiveButton(ICON_FA_PLAY " Resume Game", false) || WantsToCloseMenu())
          ClosePauseMenu();

        if (ActiveButton(ICON_FA_FAST_FORWARD " Toggle Fast Forward", false))
        {
          ClosePauseMenu();
          DoToggleFastForward();
        }

        if (ActiveButton(ICON_FA_UNDO " Load State", false, has_game))
        {
          if (OpenSaveStateSelector(true))
            s_current_main_window = MainWindowType::None;
        }

        if (ActiveButton(ICON_FA_DOWNLOAD " Save State", false, has_game))
        {
          if (OpenSaveStateSelector(false))
            s_current_main_window = MainWindowType::None;
        }

        if (ActiveButton(ICON_FA_FROWN_OPEN " Cheat List", false,
                         !System::GetRunningSerial().empty() && !Achievements::ChallengeModeActive()))
        {
          s_current_main_window = MainWindowType::None;
          DoCheatsMenu();
        }

        if (ActiveButton(ICON_FA_GAMEPAD " Toggle Analog", false))
        {
          ClosePauseMenu();
          DoToggleAnalogMode();
        }

        if (ActiveButton(ICON_FA_WRENCH " Game Properties", false, has_game))
        {
          SwitchToGameSettings();
        }

#ifdef WITH_CHEEVOS
        if (ActiveButton(ICON_FA_TROPHY " Achievements", false,
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
        ActiveButton(ICON_FA_TROPHY " Achievements", false, false);
#endif

        if (ActiveButton(ICON_FA_CAMERA " Save Screenshot", false))
        {
          System::SaveScreenshot();
          ClosePauseMenu();
        }

        if (ActiveButton(ICON_FA_COMPACT_DISC " Change Disc", false))
        {
          s_current_main_window = MainWindowType::None;
          DoChangeDisc();
        }

        if (ActiveButton(ICON_FA_SLIDERS_H " Settings", false))
          SwitchToSettings();

        if (ActiveButton(ICON_FA_POWER_OFF " Close Game", false))
        {
          // skip submenu when we can't save anyway
          if (!has_game)
            DoShutdown(false);
          else
            OpenPauseSubMenu(PauseSubMenu::Exit);
        }
      }
      break;

      case PauseSubMenu::Exit:
      {
        if (just_focused)
          ImGui::SetFocusID(ImGui::GetID(ICON_FA_POWER_OFF " Exit Without Saving"), ImGui::GetCurrentWindow());

        if (ActiveButton(ICON_FA_BACKWARD " Back To Pause Menu", false))
        {
          OpenPauseSubMenu(PauseSubMenu::None);
        }

        if (ActiveButton(ICON_FA_SYNC " Reset System", false))
        {
          ClosePauseMenu();
          DoReset();
        }

        if (ActiveButton(ICON_FA_SAVE " Exit And Save State", false))
          DoShutdown(true);

        if (ActiveButton(ICON_FA_POWER_OFF " Exit Without Saving", false))
          DoShutdown(false);
      }
      break;

#ifdef WITH_CHEEVOS
      case PauseSubMenu::Achievements:
      {
        if (ActiveButton(ICON_FA_BACKWARD " Back To Pause Menu", false))
          OpenPauseSubMenu(PauseSubMenu::None);

        if (ActiveButton(ICON_FA_TROPHY " Achievements", false))
          OpenAchievementsWindow();

        if (ActiveButton(ICON_FA_STOPWATCH " Leaderboards", false))
          OpenLeaderboardsWindow();
      }
      break;
#endif
    }

    EndMenuButtons();

    EndFullscreenWindow();
  }

#ifdef WITH_CHEEVOS
  if (Achievements::GetPrimedAchievementCount() > 0)
    DrawPrimedAchievementsList();
#endif
}

void FullscreenUI::InitializePlaceholderSaveStateListEntry(SaveStateListEntry* li, const std::string& title,
                                                           const std::string& serial, s32 slot, bool global)
{
  li->title = (global || slot > 0) ? fmt::format("{0} Slot {1}##{0}_slot_{1}", global ? "Global" : "Game", slot) :
                                     std::string("Quick Save");
  li->summary = "No save present in this slot.";
  li->path = {};
  li->timestamp = 0;
  li->slot = slot;
  li->preview_texture = {};
  li->global = global;
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
    li->title = fmt::format("Global Slot {0} - {1}##global_slot_{0}", slot, ssi->serial);
  }
  else
  {
    li->title = (slot > 0) ? fmt::format("Game Slot {0}##game_slot_{0}", slot) : std::string("Game Quick Save");
  }

  li->summary = fmt::format("Saved {:%c}", fmt::localtime(ssi->timestamp));
  li->timestamp = ssi->timestamp;
  li->slot = slot;
  li->path = std::move(filename);
  li->global = global;

  PopulateSaveStateScreenshot(li, &ssi.value());
  return true;
}

void FullscreenUI::PopulateSaveStateScreenshot(SaveStateListEntry* li, const ExtendedSaveStateInfo* ssi)
{
  li->preview_texture.reset();
  if (ssi && !ssi->screenshot_data.empty())
  {
    li->preview_texture =
      g_host_display->CreateTexture(ssi->screenshot_width, ssi->screenshot_height, 1, 1, 1, GPUTexture::Format::RGBA8,
                                    ssi->screenshot_data.data(), sizeof(u32) * ssi->screenshot_width, false);
  }
  else
  {
    li->preview_texture =
      g_host_display->CreateTexture(PLACEHOLDER_ICON_WIDTH, PLACEHOLDER_ICON_HEIGHT, 1, 1, 1, GPUTexture::Format::RGBA8,
                                    PLACEHOLDER_ICON_DATA, sizeof(u32) * PLACEHOLDER_ICON_WIDTH, false);
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
      s_save_state_selector_resuming = false;
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
  s_save_state_selector_resuming = false;
  if (PopulateSaveStateListEntries(System::GetRunningTitle().c_str(), System::GetRunningSerial().c_str()) > 0)
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
  s_save_state_selector_resuming = false;
  s_save_state_selector_game_path = {};
  if (s_current_main_window != MainWindowType::GameList)
    ReturnToMainWindow();
}

void FullscreenUI::DrawSaveStateSelector(bool is_loading)
{
  ImGuiIO& io = ImGui::GetIO();

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);

  const char* window_title = is_loading ? "Load State" : "Save State";
  ImGui::OpenPopup(window_title);

  bool is_open = true;
  const bool valid =
    ImGui::BeginPopupModal(window_title, &is_open,
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground);
  if (!valid || !is_open)
  {
    if (valid)
      ImGui::EndPopup();

    ImGui::PopStyleVar(5);
    if (!is_open)
      CloseSaveStateSelector();
    return;
  }

  ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY + LAYOUT_MENU_BUTTON_Y_PADDING * 2.0f + 2.0f));

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ModAlpha(UIPrimaryColor, 0.9f));

  if (ImGui::BeginChild("state_titlebar", heading_size, false, ImGuiWindowFlags_NoNav))
  {
    BeginNavBar();
    if (NavButton(ICON_FA_BACKWARD, true, true))
      CloseSaveStateSelector();

    NavTitle(is_loading ? "Load State" : "Save State");
    EndNavBar();
    ImGui::EndChild();
  }

  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ModAlpha(UIBackgroundColor, 0.9f));
  ImGui::SetCursorPos(ImVec2(0.0f, heading_size.y));

  bool close_handled = false;
  if (s_save_state_selector_open &&
      ImGui::BeginChild("state_list", ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y), false,
                        ImGuiWindowFlags_NavFlattened))
  {
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
    const float item_height = (style.FramePadding.y * 2.0f) + image_height + title_spacing + g_large_font->FontSize +
                              summary_spacing + g_medium_font->FontSize;
    const ImVec2 item_size(item_width, item_height);
    const u32 grid_count_x = static_cast<u32>(std::floor(ImGui::GetWindowWidth() / item_width_with_spacing));
    const float start_x =
      (static_cast<float>(ImGui::GetWindowWidth()) - (item_width_with_spacing * static_cast<float>(grid_count_x))) *
      0.5f;

    u32 grid_x = 0;
    u32 grid_y = 0;
    ImGui::SetCursorPos(ImVec2(start_x, 0.0f));
    for (u32 i = 0; i < s_save_state_selector_slots.size(); i++)
    {
      if (i == 0)
        ResetFocusHere();

      const SaveStateListEntry& entry = s_save_state_selector_slots[i];
      ImGuiWindow* window = ImGui::GetCurrentWindow();
      if (window->SkipItems)
        continue;

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

          ImGui::RenderFrame(bb.Min, bb.Max, col, true, 0.0f);

          ImGui::PopStyleColor();
        }

        bb.Min += style.FramePadding;
        bb.Max -= style.FramePadding;

        GPUTexture* const screenshot =
          entry.preview_texture ? entry.preview_texture.get() : GetPlaceholderTexture().get();
        const ImRect image_rect(
          CenterImage(ImRect(bb.Min, bb.Min + image_size),
                      ImVec2(static_cast<float>(screenshot->GetWidth()), static_cast<float>(screenshot->GetHeight()))));

        ImGui::GetWindowDrawList()->AddImage(screenshot, image_rect.Min, image_rect.Max, ImVec2(0.0f, 0.0f),
                                             ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));

        const ImVec2 title_pos(bb.Min.x, bb.Min.y + image_height + title_spacing);
        const ImRect title_bb(title_pos, ImVec2(bb.Max.x, title_pos.y + g_large_font->FontSize));
        ImGui::PushFont(g_large_font);
        ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, entry.title.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f),
                                 &title_bb);
        ImGui::PopFont();

        if (!entry.summary.empty())
        {
          const ImVec2 summary_pos(bb.Min.x, title_pos.y + g_large_font->FontSize + summary_spacing);
          const ImRect summary_bb(summary_pos, ImVec2(bb.Max.x, summary_pos.y + g_medium_font->FontSize));
          ImGui::PushFont(g_medium_font);
          ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, entry.summary.c_str(), nullptr, nullptr,
                                   ImVec2(0.0f, 0.0f), &summary_bb);
          ImGui::PopFont();
        }

        if (pressed)
        {
          if (is_loading)
          {
            DoLoadState(entry.path);
            CloseSaveStateSelector();
            break;
          }
          else
          {
            DoSaveState(entry.slot, entry.global);
            CloseSaveStateSelector();
            break;
          }
        }

        if (hovered && (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                        ImGui::IsNavInputTest(ImGuiNavInput_Input, ImGuiNavReadMode_Pressed)))
        {
          s_save_state_selector_submenu_index = static_cast<s32>(i);
        }

        if (static_cast<s32>(i) == s_save_state_selector_submenu_index)
        {
          // can't use a choice dialog here, because we're already in a modal...
          ImGuiFullscreen::PushResetLayout();
          ImGui::PushFont(g_large_font);
          ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
          ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                              LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
          ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
          ImGui::PushStyleColor(ImGuiCol_Text, UIPrimaryTextColor);
          ImGui::PushStyleColor(ImGuiCol_TitleBg, UIPrimaryDarkColor);
          ImGui::PushStyleColor(ImGuiCol_TitleBgActive, UIPrimaryColor);
          ImGui::PushStyleColor(ImGuiCol_PopupBg, MulAlpha(UIBackgroundColor, 0.95f));

          const float width = LayoutScale(600.0f);
          const float title_height =
            g_large_font->FontSize + ImGui::GetStyle().FramePadding.y * 2.0f + ImGui::GetStyle().WindowPadding.y * 2.0f;
          const float height =
            title_height +
            LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY + (LAYOUT_MENU_BUTTON_Y_PADDING * 2.0f)) * 3.0f;
          ImGui::SetNextWindowSize(ImVec2(width, height));
          ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
          ImGui::OpenPopup(entry.title.c_str());

          // don't let the back button flow through to the main window
          bool submenu_open = !WantsToCloseMenu();
          close_handled ^= submenu_open;

          bool closed = false;
          if (ImGui::BeginPopupModal(entry.title.c_str(), &is_open,
                                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
          {
            ImGui::PushStyleColor(ImGuiCol_Text, UIBackgroundTextColor);

            BeginMenuButtons();

            if (ActiveButton(is_loading ? ICON_FA_FOLDER_OPEN " Load State" : ICON_FA_FOLDER_OPEN " Save State", false,
                             true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
            {
              if (is_loading)
                DoLoadState(std::move(entry.path));
              else
                DoSaveState(entry.slot, entry.global);

              CloseSaveStateSelector();
              closed = true;
            }

            if (ActiveButton(ICON_FA_FOLDER_MINUS " Delete Save", false, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
            {
              if (!FileSystem::FileExists(entry.path.c_str()))
              {
                ShowToast({}, fmt::format("{} does not exist.", ImGuiFullscreen::RemoveHash(entry.title)));
                is_open = true;
              }
              else if (FileSystem::DeleteFile(entry.path.c_str()))
              {
                ShowToast({}, fmt::format("{} deleted.", ImGuiFullscreen::RemoveHash(entry.title)));
                s_save_state_selector_slots.erase(s_save_state_selector_slots.begin() + i);

                if (s_save_state_selector_slots.empty())
                {
                  CloseSaveStateSelector();
                  closed = true;
                }
                else
                {
                  is_open = false;
                }
              }
              else
              {
                ShowToast({}, fmt::format("Failed to delete {}.", ImGuiFullscreen::RemoveHash(entry.title)));
                is_open = false;
              }
            }

            if (ActiveButton(ICON_FA_WINDOW_CLOSE " Close Menu", false, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
            {
              is_open = false;
            }

            EndMenuButtons();

            ImGui::PopStyleColor();
            ImGui::EndPopup();
          }
          if (!is_open)
          {
            s_save_state_selector_submenu_index = -1;
            if (!closed)
              QueueResetFocus();
          }

          ImGui::PopStyleColor(4);
          ImGui::PopStyleVar(3);
          ImGui::PopFont();
          ImGuiFullscreen::PopResetLayout();

          if (closed)
            break;
        }
      }

      grid_x++;
      if (grid_x == grid_count_x)
      {
        grid_x = 0;
        grid_y++;
        ImGui::SetCursorPosX(start_x);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + item_spacing);
      }
      else
      {
        ImGui::SameLine(start_x + static_cast<float>(grid_x) * (item_width + item_spacing));
      }
    }

    EndMenuButtons();
    ImGui::EndChild();
  }

  ImGui::PopStyleColor();

  ImGui::EndPopup();
  ImGui::PopStyleVar(5);

  if (!close_handled && WantsToCloseMenu())
    CloseSaveStateSelector();
}

bool FullscreenUI::OpenLoadStateSelectorForGameResume(const GameList::Entry* entry)
{
  SaveStateListEntry slentry;
  if (!InitializeSaveStateListEntry(&slentry, entry->title, entry->serial, -1, false))
    return false;

  CloseSaveStateSelector();
  s_save_state_selector_slots.push_back(std::move(slentry));
  s_save_state_selector_game_path = entry->path;
  s_save_state_selector_loading = true;
  s_save_state_selector_open = true;
  s_save_state_selector_resuming = true;
  return true;
}

void FullscreenUI::DrawResumeStateSelector()
{
  ImGui::SetNextWindowSize(LayoutScale(800.0f, 600.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup("Load Resume State");

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  bool is_open = true;
  if (ImGui::BeginPopupModal("Load Resume State", &is_open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    const SaveStateListEntry& entry = s_save_state_selector_slots.front();
    ImGui::TextWrapped("A resume save state created at %s was found.\n\nDo you want to load this save and continue?",
                       TimeToPrintableString(entry.timestamp).c_str());

    const GPUTexture* image = entry.preview_texture ? entry.preview_texture.get() : GetPlaceholderTexture().get();
    const float image_height = LayoutScale(250.0f);
    const float image_width =
      image_height * (static_cast<float>(image->GetWidth()) / static_cast<float>(image->GetHeight()));
    const ImVec2 pos(ImGui::GetCursorScreenPos() +
                     ImVec2((ImGui::GetCurrentWindow()->WorkRect.GetWidth() - image_width) * 0.5f, LayoutScale(20.0f)));
    const ImRect image_bb(pos, pos + ImVec2(image_width, image_height));
    ImGui::GetWindowDrawList()->AddImage(
      static_cast<ImTextureID>(entry.preview_texture ? entry.preview_texture.get() : GetPlaceholderTexture().get()),
      image_bb.Min, image_bb.Max);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + image_height + LayoutScale(40.0f));

    BeginMenuButtons();

    if (ActiveButton(ICON_FA_PLAY " Load State", false))
    {
      DoStartPath(s_save_state_selector_game_path, std::move(entry.path));
      is_open = false;
    }

    if (ActiveButton(ICON_FA_LIGHTBULB " Clean Boot", false))
    {
      DoStartPath(s_save_state_selector_game_path);
      is_open = false;
    }

    if (ActiveButton(ICON_FA_FOLDER_MINUS " Delete State", false))
    {
      if (FileSystem::DeleteFile(entry.path.c_str()))
      {
        DoStartPath(s_save_state_selector_game_path);
        is_open = false;
      }
      else
      {
        ShowToast(std::string(), "Failed to delete save state.");
      }
    }

    if (ActiveButton(ICON_FA_WINDOW_CLOSE " Cancel", false))
    {
      ImGui::CloseCurrentPopup();
      is_open = false;
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopFont();

  if (!is_open)
  {
    ClearSaveStateEntryList();
    s_save_state_selector_open = false;
    s_save_state_selector_loading = false;
    s_save_state_selector_resuming = false;
    s_save_state_selector_game_path = {};
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
                                  System::GetGameSaveStateFileName(System::GetRunningSerial(), slot));
    System::SaveState(filename.c_str(), g_settings.create_save_state_backups);
  });
}

void FullscreenUI::PopulateGameListEntryList()
{
  const s32 sort = Host::GetBaseIntSettingValue("Main", "FullscreenUIGameSort", 0);
  const bool reverse = Host::GetBaseBoolSettingValue("Main", "FullscreenUIGameSortReverse", false);

  const u32 count = GameList::GetEntryCount();
  s_game_list_sorted_entries.resize(count);
  for (u32 i = 0; i < count; i++)
    s_game_list_sorted_entries[i] = GameList::GetEntryByIndex(i);

  std::sort(s_game_list_sorted_entries.begin(), s_game_list_sorted_entries.end(),
            [sort, reverse](const GameList::Entry* lhs, const GameList::Entry* rhs) {
              switch (sort)
              {
                case 0: // Type
                {
                  if (lhs->type != rhs->type)
                    return reverse ? (lhs->type > rhs->type) : (lhs->type < rhs->type);
                }
                break;

                case 1: // Serial
                {
                  if (lhs->serial != rhs->serial)
                    return reverse ? (lhs->serial > rhs->serial) : (lhs->serial < rhs->serial);
                }
                break;

                case 2: // Title
                  break;

                case 3: // File Title
                {
                  const std::string_view lhs_title(Path::GetFileTitle(lhs->path));
                  const std::string_view rhs_title(Path::GetFileTitle(rhs->path));
                  const int res = StringUtil::Strncasecmp(lhs_title.data(), rhs_title.data(),
                                                          std::min(lhs_title.size(), rhs_title.size()));
                  if (res != 0)
                    return reverse ? (res > 0) : (res < 0);
                }
                break;

                case 4: // Time Played
                {
                  if (lhs->total_played_time != rhs->total_played_time)
                  {
                    return reverse ? (lhs->total_played_time > rhs->total_played_time) :
                                     (lhs->total_played_time < rhs->total_played_time);
                  }
                }
                break;

                case 5: // Last Played (reversed by default)
                {
                  if (lhs->last_played_time != rhs->last_played_time)
                  {
                    return reverse ? (lhs->last_played_time < rhs->last_played_time) :
                                     (lhs->last_played_time > rhs->last_played_time);
                  }
                }
                break;

                case 6: // Size
                {
                  if (lhs->total_size != rhs->total_size)
                  {
                    return reverse ? (lhs->total_size > rhs->total_size) : (lhs->total_size < rhs->total_size);
                  }
                }
                break;
              }

              // fallback to title when all else is equal
              const int res = StringUtil::Strcasecmp(lhs->title.c_str(), rhs->title.c_str());
              return reverse ? (res > 0) : (res < 0);
            });
}

void FullscreenUI::DrawGameListWindow()
{
  auto game_list_lock = GameList::GetLock();
  PopulateGameListEntryList();

  ImGuiIO& io = ImGui::GetIO();
  ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY + LAYOUT_MENU_BUTTON_Y_PADDING * 2.0f + 2.0f));

  const float bg_alpha = System::IsValid() ? 0.90f : 1.0f;

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "gamelist_view", MulAlpha(UIPrimaryColor, bg_alpha)))
  {
    static constexpr float ITEM_WIDTH = 25.0f;
    static constexpr const char* icons[] = {ICON_FA_BORDER_ALL, ICON_FA_LIST, ICON_FA_COG};
    static constexpr const char* titles[] = {"Game Grid", "Game List", "Game List Settings"};
    static constexpr u32 count = static_cast<u32>(std::size(titles));

    BeginNavBar();

    if (!ImGui::IsPopupOpen(0u, ImGuiPopupFlags_AnyPopup))
    {
      if (ImGui::IsNavInputTest(ImGuiNavInput_FocusPrev, ImGuiNavReadMode_Pressed))
      {
        s_game_list_page = static_cast<GameListPage>(
          (s_game_list_page == static_cast<GameListPage>(0)) ? (count - 1) : (static_cast<u32>(s_game_list_page) - 1));
      }
      else if (ImGui::IsNavInputTest(ImGuiNavInput_FocusNext, ImGuiNavReadMode_Pressed))
      {
        s_game_list_page = static_cast<GameListPage>((static_cast<u32>(s_game_list_page) + 1) % count);
      }
    }

    if (NavButton(ICON_FA_BACKWARD, true, true))
      ReturnToMainWindow();

    NavTitle(titles[static_cast<u32>(s_game_list_page)]);
    RightAlignNavButtons(count, ITEM_WIDTH, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    for (u32 i = 0; i < count; i++)
    {
      if (NavButton(icons[i], static_cast<GameListPage>(i) == s_game_list_page, true, ITEM_WIDTH,
                    LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
      {
        s_game_list_page = static_cast<GameListPage>(i);
      }
    }

    EndNavBar();
  }

  EndFullscreenWindow();

  switch (s_game_list_page)
  {
    case GameListPage::Grid:
      DrawGameGrid(heading_size);
      break;
    case GameListPage::List:
      DrawGameList(heading_size);
      break;
    case GameListPage::Settings:
      DrawGameListSettingsPage(heading_size);
      break;
    default:
      break;
  }
}

void FullscreenUI::DrawGameList(const ImVec2& heading_size)
{
  if (!BeginFullscreenColumns(nullptr, heading_size.y, true))
  {
    EndFullscreenColumns();
    return;
  }

  auto game_list_lock = GameList::GetLock();
  const GameList::Entry* selected_entry = nullptr;
  PopulateGameListEntryList();

  if (BeginFullscreenColumnWindow(0.0f, -530.0f, "game_list_entries"))
  {
    const ImVec2 image_size(LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT, LAYOUT_MENU_BUTTON_HEIGHT));

    ResetFocusHere();

    BeginMenuButtons();

    SmallString summary;

    for (const GameList::Entry* entry : s_game_list_sorted_entries)
    {
      ImRect bb;
      bool visible, hovered;
      bool pressed =
        MenuButtonFrame(entry->path.c_str(), true, LAYOUT_MENU_BUTTON_HEIGHT, &visible, &hovered, &bb.Min, &bb.Max);
      if (!visible)
        continue;

      GPUTexture* cover_texture = GetGameListCover(entry);

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

      ImGui::GetWindowDrawList()->AddImage(cover_texture, image_rect.Min, image_rect.Max, ImVec2(0.0f, 0.0f),
                                           ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));

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
        HandleGameListActivate(entry);

      if (hovered)
        selected_entry = entry;

      if (selected_entry && (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                             ImGui::IsNavInputTest(ImGuiNavInput_Input, ImGuiNavReadMode_Pressed)))
      {
        HandleGameListOptions(selected_entry);
      }
    }

    EndMenuButtons();
  }
  EndFullscreenColumnWindow();

  if (BeginFullscreenColumnWindow(-530.0f, 0.0f, "game_list_info", UIPrimaryDarkColor))
  {
    const GPUTexture* cover_texture =
      selected_entry ? GetGameListCover(selected_entry) : GetTextureForGameListEntryType(GameList::EntryType::Count);
    if (cover_texture)
    {
      const ImRect image_rect(
        CenterImage(LayoutScale(ImVec2(350.0f, 350.0f)), ImVec2(static_cast<float>(cover_texture->GetWidth()),
                                                                static_cast<float>(cover_texture->GetHeight()))));

      ImGui::SetCursorPos(LayoutScale(ImVec2(90.0f, 50.0f)) + image_rect.Min);
      ImGui::Image(selected_entry ? GetGameListCover(selected_entry) :
                                    GetTextureForGameListEntryType(GameList::EntryType::Count),
                   image_rect.GetSize());
    }

    const float work_width = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    constexpr float field_margin_y = 10.0f;
    constexpr float start_x = 50.0f;
    float text_y = 425.0f;
    float text_width;

    PushPrimaryColor();
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
      if (!selected_entry->developer.empty())
      {
        text_width =
          ImGui::CalcTextSize(selected_entry->developer.c_str(),
                              selected_entry->developer.c_str() + selected_entry->developer.length(), false, work_width)
            .x;
        ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
        ImGui::TextWrapped("%s", selected_entry->developer.c_str());
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
        ImGui::Image(GetCachedTextureAsync(flag_texture.c_str()), LayoutScale(23.0f, 16.0f));
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
        ImGui::Image(s_game_compatibility_textures[static_cast<u32>(selected_entry->compatibility)].get(),
                     LayoutScale(64.0f, 16.0f));
        ImGui::SameLine();
      }
      ImGui::Text(" (%s)", GameDatabase::GetCompatibilityRatingDisplayName(selected_entry->compatibility));

      // play time
      ImGui::Text("Time Played: %s", GameList::FormatTimespan(selected_entry->total_played_time).GetCharArray());
      ImGui::Text("Last Played: %s", GameList::FormatTimestamp(selected_entry->last_played_time).GetCharArray());

      // size
      ImGui::Text("Size: %.2f MB", static_cast<float>(selected_entry->total_size) / 1048576.0f);

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
    PopPrimaryColor();
  }
  EndFullscreenColumnWindow();

  EndFullscreenColumns();
}

void FullscreenUI::DrawGameGrid(const ImVec2& heading_size)
{
  ImGuiIO& io = ImGui::GetIO();
  if (!BeginFullscreenWindow(ImVec2(0.0f, heading_size.y), ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y),
                             "game_grid", UIBackgroundColor))
  {
    EndFullscreenWindow();
    return;
  }

  if (WantsToCloseMenu())
  {
    if (ImGui::IsWindowFocused())
      ReturnToMainWindow();
  }

  ResetFocusHere();
  BeginMenuButtons();

  const ImGuiStyle& style = ImGui::GetStyle();

  const float title_spacing = LayoutScale(10.0f);
  const float item_spacing = LayoutScale(20.0f);
  const float item_width_with_spacing = std::floor(LayoutScale(LAYOUT_SCREEN_WIDTH / 5.0f));
  const float item_width = item_width_with_spacing - item_spacing;
  const float image_width = item_width - (style.FramePadding.x * 2.0f);
  const float image_height = image_width;
  const ImVec2 image_size(image_width, image_height);
  const float item_height = (style.FramePadding.y * 2.0f) + image_height + title_spacing + g_medium_font->FontSize;
  const ImVec2 item_size(item_width, item_height);
  const u32 grid_count_x = static_cast<u32>(std::floor(ImGui::GetWindowWidth() / item_width_with_spacing));
  const float start_x =
    (static_cast<float>(ImGui::GetWindowWidth()) - (item_width_with_spacing * static_cast<float>(grid_count_x))) * 0.5f;

  SmallString draw_title;

  u32 grid_x = 0;
  u32 grid_y = 0;
  ImGui::SetCursorPos(ImVec2(start_x, 0.0f));
  for (const GameList::Entry* entry : s_game_list_sorted_entries)
  {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
      continue;

    const ImGuiID id = window->GetID(entry->path.c_str(), entry->path.c_str() + entry->path.length());
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

        const float t = static_cast<float>(std::min(std::abs(std::sin(ImGui::GetTime() * 0.75) * 1.1), 1.0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Border, t));

        ImGui::RenderFrame(bb.Min, bb.Max, col, true, 0.0f);

        ImGui::PopStyleColor();
      }

      bb.Min += style.FramePadding;
      bb.Max -= style.FramePadding;

      GPUTexture* const cover_texture = GetGameListCover(entry);
      const ImRect image_rect(
        CenterImage(ImRect(bb.Min, bb.Min + image_size), ImVec2(static_cast<float>(cover_texture->GetWidth()),
                                                                static_cast<float>(cover_texture->GetHeight()))));

      ImGui::GetWindowDrawList()->AddImage(cover_texture, image_rect.Min, image_rect.Max, ImVec2(0.0f, 0.0f),
                                           ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));

      const ImRect title_bb(ImVec2(bb.Min.x, bb.Min.y + image_height + title_spacing), bb.Max);
      const std::string_view title(
        std::string_view(entry->title).substr(0, (entry->title.length() > 31) ? 31 : std::string_view::npos));
      draw_title.Fmt("{}{}", title, (title.length() == entry->title.length()) ? "" : "...");
      ImGui::PushFont(g_medium_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, draw_title.GetCharArray(),
                               draw_title.GetCharArray() + draw_title.GetLength(), nullptr, ImVec2(0.5f, 0.0f),
                               &title_bb);
      ImGui::PopFont();

      if (pressed)
        HandleGameListActivate(entry);

      if (hovered && (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                      ImGui::IsNavInputTest(ImGuiNavInput_Input, ImGuiNavReadMode_Pressed)))
      {
        HandleGameListOptions(entry);
      }
    }

    grid_x++;
    if (grid_x == grid_count_x)
    {
      grid_x = 0;
      grid_y++;
      ImGui::SetCursorPosX(start_x);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + item_spacing);
    }
    else
    {
      ImGui::SameLine(start_x + static_cast<float>(grid_x) * (item_width + item_spacing));
    }
  }

  EndMenuButtons();
  EndFullscreenWindow();
}

void FullscreenUI::HandleGameListActivate(const GameList::Entry* entry)
{
  // launch game
  if (!OpenLoadStateSelectorForGameResume(entry))
    DoStartPath(entry->path);
}

void FullscreenUI::HandleGameListOptions(const GameList::Entry* entry)
{
  ImGuiFullscreen::ChoiceDialogOptions options = {
    {ICON_FA_WRENCH " Game Properties", false},  {ICON_FA_PLAY " Resume Game", false},
    {ICON_FA_UNDO " Load State", false},         {ICON_FA_COMPACT_DISC " Default Boot", false},
    {ICON_FA_LIGHTBULB " Fast Boot", false},     {ICON_FA_MAGIC " Slow Boot", false},
    {ICON_FA_WINDOW_CLOSE " Close Menu", false},
  };

  OpenChoiceDialog(
    entry->title.c_str(), false, std::move(options),
    [entry_path = entry->path, entry_serial = entry->serial](s32 index, const std::string& title, bool checked) {
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

void FullscreenUI::DrawGameListSettingsPage(const ImVec2& heading_size)
{
  const ImGuiIO& io = ImGui::GetIO();
  if (!BeginFullscreenWindow(ImVec2(0.0f, heading_size.y), ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y),
                             "settings_parent", UIBackgroundColor))
  {
    EndFullscreenWindow();
    return;
  }

  if (WantsToCloseMenu())
  {
    if (ImGui::IsWindowFocused())
      ReturnToMainWindow();
  }

  auto lock = Host::GetSettingsLock();
  SettingsInterface* bsi = GetEditingSettingsInterface(false);

  BeginMenuButtons();

  MenuHeading("Search Directories");
  if (MenuButton(ICON_FA_FOLDER_PLUS " Add Search Directory", "Adds a new directory to the game search list."))
  {
    OpenFileSelector(ICON_FA_FOLDER_PLUS " Add Search Directory", true, [](const std::string& dir) {
      if (!dir.empty())
      {
        auto lock = Host::GetSettingsLock();
        SettingsInterface* bsi = Host::Internal::GetBaseSettingsLayer();

        bsi->AddToStringList("GameList", "RecursivePaths", dir.c_str());
        bsi->RemoveFromStringList("GameList", "Paths", dir.c_str());
        SetSettingsChanged(bsi);
        PopulateGameListDirectoryCache(bsi);
        Host::RefreshGameListAsync(false);
      }

      CloseFileSelector();
    });
  }

  for (const auto& it : s_game_list_directories_cache)
  {
    if (MenuButton(SmallString::FromFmt(ICON_FA_FOLDER " {}", it.first),
                   it.second ? "Scanning Subdirectories" : "Not Scanning Subdirectories"))
    {
      ImGuiFullscreen::ChoiceDialogOptions options = {
        {ICON_FA_FOLDER_OPEN " Open in File Browser", false},
        {it.second ? (ICON_FA_FOLDER_MINUS " Disable Subdirectory Scanning") :
                     (ICON_FA_FOLDER_PLUS " Enable Subdirectory Scanning"),
         false},
        {ICON_FA_TIMES " Remove From List", false},
        {ICON_FA_WINDOW_CLOSE " Close Menu", false},
      };

      OpenChoiceDialog(it.first.c_str(), false, std::move(options),
                       [dir = it.first, recursive = it.second](s32 index, const std::string& title, bool checked) {
                         if (index < 0)
                           return;

                         if (index == 0)
                         {
                           // Open in file browser... todo
                           Host::ReportErrorAsync("Error", "Not implemented");
                         }
                         else if (index == 1)
                         {
                           // toggle subdirectory scanning
                           {
                             auto lock = Host::GetSettingsLock();
                             SettingsInterface* bsi = Host::Internal::GetBaseSettingsLayer();
                             if (!recursive)
                             {
                               bsi->RemoveFromStringList("GameList", "Paths", dir.c_str());
                               bsi->AddToStringList("GameList", "RecursivePaths", dir.c_str());
                             }
                             else
                             {
                               bsi->RemoveFromStringList("GameList", "RecursivePaths", dir.c_str());
                               bsi->AddToStringList("GameList", "Paths", dir.c_str());
                             }

                             SetSettingsChanged(bsi);
                             PopulateGameListDirectoryCache(bsi);
                           }

                           Host::RefreshGameListAsync(false);
                         }
                         else if (index == 2)
                         {
                           // remove from list
                           auto lock = Host::GetSettingsLock();
                           SettingsInterface* bsi = Host::Internal::GetBaseSettingsLayer();
                           bsi->RemoveFromStringList("GameList", "Paths", dir.c_str());
                           bsi->RemoveFromStringList("GameList", "RecursivePaths", dir.c_str());
                           SetSettingsChanged(bsi);
                           PopulateGameListDirectoryCache(bsi);
                           Host::RefreshGameListAsync(false);
                         }

                         CloseChoiceDialog();
                       });
    }
  }

  MenuHeading("List Settings");
  {
    static constexpr const char* view_types[] = {"Game Grid", "Game List"};
    static constexpr const char* sort_types[] = {"Type",        "Serial",      "Title", "File Title",
                                                 "Time Played", "Last Played", "Size"};

    DrawIntListSetting(bsi, ICON_FA_BORDER_ALL " Default View", "Sets which view the game list will open to.", "Main",
                       "DefaultFullscreenUIGameView", 0, view_types, std::size(view_types));
    DrawIntListSetting(bsi, ICON_FA_SORT " Sort By", "Determines which field the game list will be sorted by.", "Main",
                       "FullscreenUIGameSort", 0, sort_types, std::size(sort_types));
    DrawToggleSetting(bsi, ICON_FA_SORT_ALPHA_DOWN " Sort Reversed",
                      "Reverses the game list sort order from the default (usually ascending to descending).", "Main",
                      "FullscreenUIGameSortReverse", false);
  }

  MenuHeading("Cover Settings");
  {
    DrawFolderSetting(bsi, ICON_FA_FOLDER " Covers Directory", "Folders", "Covers", EmuFolders::Covers);
    if (MenuButton(ICON_FA_DOWNLOAD " Download Covers", "Downloads covers from a user-specified URL template."))
      ImGui::OpenPopup("Download Covers");
  }

  MenuHeading("Operations");
  {
    if (MenuButton(ICON_FA_SEARCH " Scan For New Games", "Identifies any new files added to the game directories."))
      Host::RefreshGameListAsync(false);
    if (MenuButton(ICON_FA_SEARCH_PLUS " Rescan All Games", "Forces a full rescan of all games previously identified."))
      Host::RefreshGameListAsync(true);
  }

  EndMenuButtons();

  DrawCoverDownloaderWindow();
  EndFullscreenWindow();
}

void FullscreenUI::DrawCoverDownloaderWindow()
{
  ImGui::SetNextWindowSize(LayoutScale(1000.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));
  ImGui::PushFont(g_large_font);

  bool is_open = true;
  if (ImGui::BeginPopupModal("Download Covers", &is_open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    ImGui::TextWrapped("DuckStation can automatically download covers for games which do not currently have a cover "
                       "set. We do not host any "
                       "cover images, the user must provide their own source for images.");
    ImGui::NewLine();
    ImGui::TextWrapped(
      "In the form below, specify the URLs to download covers from, with one template URL per line. The following "
      "variables are available:");
    ImGui::NewLine();
    ImGui::TextWrapped("${title}: Title of the game.\n${filetitle}: Name component of the game's filename.\n${serial}: "
                       "Serial of the game.");
    ImGui::NewLine();
    ImGui::TextWrapped("Example: https://www.example-not-a-real-domain.com/covers/${serial}.jpg");
    ImGui::NewLine();

    BeginMenuButtons();

    static char template_urls[512];
    ImGui::InputTextMultiline("##templates", template_urls, sizeof(template_urls),
                              ImVec2(ImGui::GetCurrentWindow()->WorkRect.GetWidth(), LayoutScale(175.0f)));

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(5.0f));

    static bool use_serial_names;
    ImGui::PushFont(g_medium_font);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(2.0f, 2.0f));
    ImGui::Checkbox("Use Serial File Names", &use_serial_names);
    ImGui::PopStyleVar(1);
    ImGui::PopFont();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));

    const bool download_enabled = (std::strlen(template_urls) > 0);

    if (ActiveButton(ICON_FA_DOWNLOAD " Start Download", false, download_enabled))
    {
      StartAsyncOp(
        [urls = StringUtil::SplitNewString(template_urls, '\n'),
         use_serial_names = use_serial_names](::ProgressCallback* progress) {
          GameList::DownloadCovers(urls, use_serial_names, progress,
                                   [](const GameList::Entry* entry, std::string save_path) {
                                     // cache the cover path on our side once it's saved
                                     Host::RunOnCPUThread([path = entry->path, save_path = std::move(save_path)]() {
                                       s_cover_image_map[std::move(path)] = std::move(save_path);
                                     });
                                   });
        },
        "Download Covers");
      std::memset(template_urls, 0, sizeof(template_urls));
      use_serial_names = false;
      ImGui::CloseCurrentPopup();
    }

    if (ActiveButton(ICON_FA_TIMES " Cancel", false))
    {
      std::memset(template_urls, 0, sizeof(template_urls));
      use_serial_names = false;
      ImGui::CloseCurrentPopup();
    }

    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopFont();
  ImGui::PopStyleVar(2);
}

void FullscreenUI::SwitchToGameList()
{
  s_current_main_window = MainWindowType::GameList;
  s_game_list_page = static_cast<GameListPage>(Host::GetBaseIntSettingValue("Main", "DefaultFullscreenUIGameView", 0));
  {
    auto lock = Host::GetSettingsLock();
    PopulateGameListDirectoryCache(Host::Internal::GetBaseSettingsLayer());
  }
  QueueResetFocus();
}

GPUTexture* FullscreenUI::GetGameListCover(const GameList::Entry* entry)
{
  // lookup and grab cover image
  auto cover_it = s_cover_image_map.find(entry->path);
  if (cover_it == s_cover_image_map.end())
  {
    std::string cover_path(GameList::GetCoverImagePathForEntry(entry));
    cover_it = s_cover_image_map.emplace(entry->path, std::move(cover_path)).first;
  }

  GPUTexture* tex = (!cover_it->second.empty()) ? GetCachedTextureAsync(cover_it->second.c_str()) : nullptr;
  return tex ? tex : GetTextureForGameListEntryType(entry->type);
}

GPUTexture* FullscreenUI::GetTextureForGameListEntryType(GameList::EntryType type)
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

GPUTexture* FullscreenUI::GetCoverForCurrentGame()
{
  auto lock = GameList::GetLock();

  const GameList::Entry* entry = GameList::GetEntryForPath(System::GetRunningPath().c_str());
  if (!entry)
    return s_fallback_disc_texture.get();

  return GetGameListCover(entry);
}

//////////////////////////////////////////////////////////////////////////
// Overlays
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::OpenAboutWindow()
{
  s_about_window_open = true;
}

void FullscreenUI::ExitFullscreenAndOpenURL(const std::string_view& url)
{
  Host::RunOnCPUThread([url = std::string(url)]() {
    if (Host::IsFullscreen())
      Host::SetFullscreen(false);

    Host::OpenURL(url);
  });
}

void FullscreenUI::CopyTextToClipboard(std::string title, const std::string_view& text)
{
  if (Host::CopyTextToClipboard(text))
    ShowToast(std::string(), std::move(title));
  else
    ShowToast(std::string(), "Failed to copy text to clipboard.");
}

void FullscreenUI::DrawAboutWindow()
{
  ImGui::SetNextWindowSize(LayoutScale(1000.0f, 510.0f));
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
    if (ActiveButton(ICON_FA_GLOBE " GitHub Repository", false))
      ExitFullscreenAndOpenURL("https://github.com/stenzek/duckstation/");
    if (ActiveButton(ICON_FA_BUG " Issue Tracker", false))
      ExitFullscreenAndOpenURL("https://github.com/stenzek/duckstation/issues");
    if (ActiveButton(ICON_FA_COMMENT " Discord Server", false))
      ExitFullscreenAndOpenURL("https://discord.gg/Buktv3t");

    if (ActiveButton(ICON_FA_WINDOW_CLOSE " Close", false))
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

    if (ActiveButton(ICON_FA_WINDOW_CLOSE " Close", false))
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

    if (ActiveButton(ICON_FA_CHECK " Yes", false))
    {
      *result = true;
      done = true;
    }

    if (ActiveButton(ICON_FA_TIMES " No", false))
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
    GPUTexture* badge = GetCachedTextureAsync(badge_path.c_str());
    if (badge)
    {
      ImGui::GetWindowDrawList()->AddImage(badge, bb.Min, bb.Min + image_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                           IM_COL32(255, 255, 255, 255));
    }
  }

  const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
  const auto points_text = TinyString::FromFmt("{} point{}", cheevo.points, cheevo.points != 1 ? "s" : "");
  const ImVec2 points_template_size(g_medium_font->CalcTextSizeA(g_medium_font->FontSize, FLT_MAX, 0.0f, "XXX points"));
  const ImVec2 points_size(g_medium_font->CalcTextSizeA(g_medium_font->FontSize, FLT_MAX, 0.0f,
                                                        points_text.GetCharArray(),
                                                        points_text.GetCharArray() + points_text.GetLength()));
  const float points_template_start = bb.Max.x - points_template_size.x;
  const float points_start = points_template_start + ((points_template_size.x - points_size.x) * 0.5f);
  const char* lock_text = cheevo.locked ? ICON_FA_LOCK : ICON_FA_LOCK_OPEN;
  const ImVec2 lock_size(g_large_font->CalcTextSizeA(g_large_font->FontSize, FLT_MAX, 0.0f, lock_text));

  const float text_start_x = bb.Min.x + image_size.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(points_start, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint), ImVec2(points_start, bb.Max.y));
  const ImRect points_bb(ImVec2(points_start, midpoint), bb.Max);
  const ImRect lock_bb(ImVec2(points_template_start + ((points_template_size.x - lock_size.x) * 0.5f), bb.Min.y),
                       ImVec2(bb.Max.x, midpoint));

  ImGui::PushFont(g_large_font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, cheevo.title.c_str(), cheevo.title.c_str() + cheevo.title.size(),
                           nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::RenderTextClipped(lock_bb.Min, lock_bb.Max, lock_text, nullptr, &lock_size, ImVec2(0.0f, 0.0f), &lock_bb);
  ImGui::PopFont();

  ImGui::PushFont(g_medium_font);
  if (!cheevo.description.empty())
  {
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, cheevo.description.c_str(),
                             cheevo.description.c_str() + cheevo.description.size(), nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
  }
  ImGui::RenderTextClipped(points_bb.Min, points_bb.Max, points_text.GetCharArray(),
                           points_text.GetCharArray() + points_text.GetLength(), &points_size, ImVec2(0.0f, 0.0f),
                           &points_bb);
  ImGui::PopFont();

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

    const auto text = Achievements::GetAchievementProgressText(cheevo);
    const ImVec2 text_size = ImGui::CalcTextSize(text.GetCharArray(), text.GetCharArray() + text.GetLength());
    const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                          progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f));
    dl->AddText(g_medium_font, g_medium_font->FontSize, text_pos,
                ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryTextColor), text.GetCharArray(),
                text.GetCharArray() + text.GetLength());
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
        GPUTexture* badge = GetCachedTexture(icon_path.c_str());
        if (badge)
        {
          ImGui::GetWindowDrawList()->AddImage(badge, icon_min, icon_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                               IM_COL32(255, 255, 255, 255));
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

void FullscreenUI::DrawPrimedAchievementsIcons()
{
  const ImVec2 image_size(LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT, LAYOUT_MENU_BUTTON_HEIGHT));
  const float spacing = LayoutScale(10.0f);
  const float padding = LayoutScale(10.0f);

  const ImGuiIO& io = ImGui::GetIO();
  const float x_advance = image_size.x + spacing;
  ImVec2 position(io.DisplaySize.x - padding - image_size.x, io.DisplaySize.y - padding - image_size.y);

  auto lock = Achievements::GetLock();
  Achievements::EnumerateAchievements(
    [&image_size, &x_advance, &position](const Achievements::Achievement& achievement) {
      if (!achievement.primed)
        return true;

      const std::string& badge_path = Achievements::GetAchievementBadgePath(achievement, true, true);
      if (badge_path.empty())
        return true;

      GPUTexture* badge = GetCachedTextureAsync(badge_path.c_str());
      if (!badge)
        return true;

      ImDrawList* dl = ImGui::GetBackgroundDrawList();
      dl->AddImage(badge, position, position + image_size);
      position.x -= x_advance;
      return true;
    });
}

void FullscreenUI::DrawPrimedAchievementsList()
{
  auto lock = Achievements::GetLock();
  const u32 primed_count = Achievements::GetPrimedAchievementCount();

  const ImGuiIO& io = ImGui::GetIO();
  ImFont* font = g_medium_font;

  const ImVec2 image_size(LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY));
  const float margin = LayoutScale(10.0f);
  const float spacing = LayoutScale(10.0f);
  const float padding = LayoutScale(10.0f);

  const float max_text_width = LayoutScale(300.0f);
  const float row_width = max_text_width + padding + padding + image_size.x + spacing;
  const float title_height = padding + font->FontSize + padding;
  const ImVec2 box_min(io.DisplaySize.x - row_width - margin, margin);
  const ImVec2 box_max(box_min.x + row_width,
                       box_min.y + title_height + (static_cast<float>(primed_count) * (image_size.y + padding)));

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  dl->AddRectFilled(box_min, box_max, IM_COL32(0x21, 0x21, 0x21, 200), LayoutScale(10.0f));
  dl->AddText(font, font->FontSize, ImVec2(box_min.x + padding, box_min.y + padding), IM_COL32(255, 255, 255, 255),
              "Active Challenge Achievements");

  const float y_advance = image_size.y + spacing;
  const float acheivement_name_offset = (image_size.y - font->FontSize) / 2.0f;
  const float max_non_ellipised_text_width = max_text_width - LayoutScale(10.0f);
  ImVec2 position(box_min.x + padding, box_min.y + title_height);

  Achievements::EnumerateAchievements([font, &image_size, max_text_width, spacing, y_advance, acheivement_name_offset,
                                       max_non_ellipised_text_width,
                                       &position](const Achievements::Achievement& achievement) {
    if (!achievement.primed)
      return true;

    const std::string& badge_path = Achievements::GetAchievementBadgePath(achievement, true, true);
    if (badge_path.empty())
      return true;

    GPUTexture* badge = GetCachedTextureAsync(badge_path.c_str());
    if (!badge)
      return true;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddImage(badge, position, position + image_size);

    const char* achievement_title = achievement.title.c_str();
    const char* achievement_tile_end = achievement_title + achievement.title.length();
    const char* remaining_text = nullptr;
    const ImVec2 text_width(font->CalcTextSizeA(font->FontSize, max_non_ellipised_text_width, 0.0f, achievement_title,
                                                achievement_tile_end, &remaining_text));
    const ImVec2 text_position(position.x + image_size.x + spacing, position.y + acheivement_name_offset);
    const ImVec4 text_bbox(text_position.x, text_position.y, text_position.x + max_text_width,
                           text_position.y + image_size.y);
    const u32 text_color = IM_COL32(255, 255, 255, 255);

    if (remaining_text < achievement_tile_end)
    {
      dl->AddText(font, font->FontSize, text_position, text_color, achievement_title, remaining_text, 0.0f, &text_bbox);
      dl->AddText(font, font->FontSize, ImVec2(text_position.x + text_width.x, text_position.y), text_color, "...",
                  nullptr, 0.0f, &text_bbox);
    }
    else
    {
      dl->AddText(font, font->FontSize, text_position, text_color, achievement_title,
                  achievement_title + achievement.title.length(), 0.0f, &text_bbox);
    }

    position.y += y_advance;
    return true;
  });
}

bool FullscreenUI::OpenLeaderboardsWindow()
{
  if (!System::IsValid() || !Achievements::HasActiveGame() || Achievements::GetLeaderboardCount() == 0 || !Initialize())
    return false;

  if (s_current_main_window != MainWindowType::PauseMenu)
    PauseForMenuOpen();

  s_current_main_window = MainWindowType::Leaderboards;
  s_open_leaderboard_id.reset();
  QueueResetFocus();
  return true;
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
        GPUTexture* badge = GetCachedTexture(icon_path.c_str());
        if (badge)
        {
          ImGui::GetWindowDrawList()->AddImage(badge, icon_min, icon_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                               IM_COL32(255, 255, 255, 255));
        }
      }

      float left = bb.Min.x + padding + image_height + spacing;
      float right = bb.Max.x - padding;
      float top = bb.Min.y + padding;
      SmallString text;

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

#else

// "Lightweight" version with only notifications for Android.
namespace FullscreenUI {
static bool s_initialized = false;
static bool s_tried_to_initialize = false;
} // namespace FullscreenUI

bool FullscreenUI::Initialize()
{
  if (s_initialized)
    return true;

  if (s_tried_to_initialize)
    return false;

  ImGuiFullscreen::SetTheme(false);
  ImGuiFullscreen::UpdateLayoutScale();

  if (!ImGuiManager::AddFullscreenFontsIfMissing() || !ImGuiFullscreen::Initialize("images/placeholder.png"))
  {
    ImGuiFullscreen::Shutdown();
    s_tried_to_initialize = true;
    return false;
  }

  s_initialized = true;
  return true;
}

bool FullscreenUI::IsInitialized()
{
  return s_initialized;
}

bool FullscreenUI::HasActiveWindow()
{
  return false;
}

void FullscreenUI::CheckForConfigChanges(const Settings& old_settings)
{
  // noop
}

void FullscreenUI::OnSystemStarted()
{
  // noop
}

void FullscreenUI::OnSystemPaused()
{
  // noop
}

void FullscreenUI::OnSystemResumed()
{
  // noop
}

void FullscreenUI::OnSystemDestroyed()
{
  // noop
}

void FullscreenUI::OnRunningGameChanged()
{
  // noop
}

void FullscreenUI::OpenPauseMenu()
{
  // noop
}

bool FullscreenUI::OpenAchievementsWindow()
{
  return false;
}

bool FullscreenUI::OpenLeaderboardsWindow()
{
  return false;
}

void FullscreenUI::Shutdown()
{
  ImGuiFullscreen::Shutdown();
  s_initialized = false;
  s_tried_to_initialize = false;
}

void FullscreenUI::Render()
{
  if (!s_initialized)
    return;

  ImGuiFullscreen::UploadAsyncTextures();

  ImGuiFullscreen::BeginLayout();
  ImGuiFullscreen::EndLayout();
  ImGuiFullscreen::ResetCloseMenuIfNeeded();
}

#endif // __ANDROID__
