// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#define IMGUI_DEFINE_MATH_OPERATORS

#include "fullscreen_ui.h"
#include "achievements.h"
#include "bios.h"
#include "cheats.h"
#include "common_host.h"
#include "controller.h"
#include "core/memory_card_image.h"
#include "cpu_core.h"
#include "game_list.h"
#include "gpu.h"
#include "host.h"
#include "host_settings.h"
#include "resources.h"
#include "settings.h"
#include "system.h"

#include "scmversion/scmversion.h"

#include "util/gpu_device.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/ini_settings_interface.h"
#include "util/input_manager.h"
#include "util/postprocessing_chain.h"

#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/make_array.h"
#include "common/path.h"
#include "common/string.h"
#include "common/string_util.h"
#include "common/threading.h"

#include "IconsFontAwesome5.h"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <atomic>
#include <bitset>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

Log_SetChannel(FullscreenUI);

#ifdef WITH_CHEEVOS
#include "achievements_private.h"
#endif

#define TR_CONTEXT "FullscreenUI"

namespace {
template<size_t L>
class IconStackString : public StackString<L>
{
public:
  ALWAYS_INLINE IconStackString(const char* icon, const char* str)
  {
    StackString<L>::Fmt("{} {}", icon, Host::TranslateToStringView(TR_CONTEXT, str));
  }
};
} // namespace

#define FSUI_ICONSTR(icon, str) IconStackString<128>(icon, str).GetCharArray()
#define FSUI_STR(str) Host::TranslateToString(TR_CONTEXT, str)
#define FSUI_CSTR(str) Host::TranslateToCString(TR_CONTEXT, str)
#define FSUI_VSTR(str) Host::TranslateToStringView(TR_CONTEXT, str)
#define FSUI_FSTR(str) fmt::runtime(Host::TranslateToStringView(TR_CONTEXT, str))
#define FSUI_NSTR(str) str

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
static void DrawFloatSpinBoxSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                                    const char* key, float default_value, float min_value, float max_value,
                                    float step_value, float multiplier, const char* format = "%f", bool enabled = true,
                                    float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                    ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
#if 0
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
#endif
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
static void BeginInputBinding(SettingsInterface* bsi, InputBindingInfo::Type type, const std::string_view& section,
                              const std::string_view& key, const std::string_view& display_name);
static void DrawInputBindingWindow();
static void DrawInputBindingButton(SettingsInterface* bsi, InputBindingInfo::Type type, const char* section,
                                   const char* name, const char* display_name, bool show_type = true);
static void ClearInputBindingVariables();
static void StartAutomaticBinding(u32 port);

static SettingsPage s_settings_page = SettingsPage::Interface;
static std::unique_ptr<INISettingsInterface> s_game_settings_interface;
static std::unique_ptr<GameList::Entry> s_game_settings_entry;
static std::vector<std::pair<std::string, bool>> s_game_list_directories_cache;
static std::vector<std::string> s_graphics_adapter_list_cache;
static std::vector<std::string> s_fullscreen_mode_list_cache;
static PostProcessingChain s_postprocessing_chain;
static std::vector<const HotkeyInfo*> s_hotkey_list_cache;
static std::atomic_bool s_settings_changed{false};
static std::atomic_bool s_game_settings_changed{false};
static InputBindingInfo::Type s_input_binding_type = InputBindingInfo::Type::Unknown;
static std::string s_input_binding_section;
static std::string s_input_binding_key;
static std::string s_input_binding_display_name;
static std::vector<InputBindingKey> s_input_binding_new_bindings;
static std::vector<std::pair<InputBindingKey, std::pair<float, float>>> s_input_binding_value_ranges;
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

  s_pause_menu_was_open = false;
  SwitchToLanding();
}

void FullscreenUI::OnRunningGameChanged()
{
  if (!IsInitialized())
    return;

  const std::string& path = System::GetDiscPath();
  const std::string& serial = System::GetGameSerial();
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

  if (s_input_binding_type != InputBindingInfo::Type::Unknown)
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
    ShowToast({}, FSUI_CSTR("No resume save state found."));
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

  OpenFileSelector(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Select Disc Image"), false, std::move(callback),
                   GetDiscImageFilters());
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
        ShowToast({},
                  fmt::format(FSUI_FSTR("{} is not a valid disc image."), FileSystem::GetDisplayNameFromPath(path)));
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

  OpenFileSelector(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Select Disc Image"), false, std::move(callback),
                   GetDiscImageFilters(), std::string(Path::GetDirectory(System::GetDiscPath())));
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

  OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Select Disc Image"), true, std::move(options),
                   std::move(callback));
}

void FullscreenUI::DoCheatsMenu()
{
  CheatList* cl = System::GetCheatList();
  if (!cl)
  {
    if (!System::LoadCheatListFromDatabase() || ((cl = System::GetCheatList()) == nullptr))
    {
      Host::AddKeyedOSDMessage("load_cheat_list",
                               fmt::format(FSUI_FSTR("No cheats found for {}."), System::GetGameTitle()), 10.0f);
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
  OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_FROWN, "Cheat List"), true, std::move(options), std::move(callback));
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
  Host::RunOnCPUThread([]() { Host::RequestExit(true); });
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

    if (MenuButton(FSUI_ICONSTR(ICON_FA_LIST, "Game List"),
                   FSUI_CSTR("Launch a game from images scanned from your game directories.")))
    {
      SwitchToGameList();
    }

    if (MenuButton(FSUI_ICONSTR(ICON_FA_PLAY_CIRCLE, "Resume"),
                   FSUI_CSTR("Starts the console from where it was before it was last closed.")))
    {
      System::GetMostRecentResumeSaveStatePath();
      DoResume();
    }

    if (MenuButton(FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Start File"),
                   FSUI_CSTR("Launch a game by selecting a file/disc image.")))
    {
      DoStartFile();
    }

    if (MenuButton(FSUI_ICONSTR(ICON_FA_TOOLBOX, "Start BIOS"),
                   FSUI_CSTR("Start the console without any disc inserted.")))
    {
      DoStartBIOS();
    }

    if (MenuButton(FSUI_ICONSTR(ICON_FA_UNDO, "Load State"), FSUI_CSTR("Loads a global save state.")))
    {
      OpenSaveStateSelector(true);
    }

    if (MenuButton(FSUI_ICONSTR(ICON_FA_SLIDERS_H, "Settings"), FSUI_CSTR("Change settings for the emulator.")))
      SwitchToSettings();

    if (MenuButton(FSUI_ICONSTR(ICON_FA_SIGN_OUT_ALT, "Exit"), FSUI_CSTR("Exits the program.")))
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

void FullscreenUI::DrawInputBindingButton(SettingsInterface* bsi, InputBindingInfo::Type type, const char* section,
                                          const char* name, const char* display_name, bool show_type)
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
      case InputBindingInfo::Type::Button:
        title = fmt::format(ICON_FA_DOT_CIRCLE "{}", display_name);
        break;
      case InputBindingInfo::Type::Axis:
      case InputBindingInfo::Type::HalfAxis:
        title = fmt::format(ICON_FA_BULLSEYE "{}", display_name);
        break;
      case InputBindingInfo::Type::Motor:
        title = fmt::format(ICON_FA_BELL "{}", display_name);
        break;
      case InputBindingInfo::Type::Macro:
        title = fmt::format(ICON_FA_PIZZA_SLICE "{}", display_name);
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
  ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, value.empty() ? FSUI_CSTR("No Binding") : value.c_str(),
                           nullptr, nullptr, ImVec2(0.0f, 0.0f), &summary_bb);
  ImGui::PopFont();

  if (clicked)
  {
    BeginInputBinding(bsi, type, section, name, display_name);
  }
  else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
           ImGui::IsNavInputTest(ImGuiNavInput_Input, ImGuiNavReadMode_Pressed))
  {
    bsi->DeleteValue(section, name);
    SetSettingsChanged(bsi);
  }
}

void FullscreenUI::ClearInputBindingVariables()
{
  s_input_binding_type = InputBindingInfo::Type::Unknown;
  s_input_binding_section = {};
  s_input_binding_key = {};
  s_input_binding_display_name = {};
  s_input_binding_new_bindings = {};
  s_input_binding_value_ranges = {};
}

void FullscreenUI::BeginInputBinding(SettingsInterface* bsi, InputBindingInfo::Type type,
                                     const std::string_view& section, const std::string_view& key,
                                     const std::string_view& display_name)
{
  if (s_input_binding_type != InputBindingInfo::Type::Unknown)
  {
    InputManager::RemoveHook();
    ClearInputBindingVariables();
  }

  s_input_binding_type = type;
  s_input_binding_section = section;
  s_input_binding_key = key;
  s_input_binding_display_name = display_name;
  s_input_binding_new_bindings = {};
  s_input_binding_value_ranges = {};
  s_input_binding_timer.Reset();

  const bool game_settings = IsEditingGameSettings(bsi);

  InputManager::SetHook([game_settings](InputBindingKey key, float value) -> InputInterceptHook::CallbackResult {
    if (s_input_binding_type == InputBindingInfo::Type::Unknown)
      return InputInterceptHook::CallbackResult::StopProcessingEvent;

    // holding the settings lock here will protect the input binding list
    auto lock = Host::GetSettingsLock();

    float initial_value = value;
    float min_value = value;
    auto it = std::find_if(s_input_binding_value_ranges.begin(), s_input_binding_value_ranges.end(),
                           [key](const auto& it) { return it.first.bits == key.bits; });
    if (it != s_input_binding_value_ranges.end())
    {
      initial_value = it->second.first;
      min_value = it->second.second = std::min(it->second.second, value);
    }
    else
    {
      s_input_binding_value_ranges.emplace_back(key, std::make_pair(initial_value, min_value));
    }

    const float abs_value = std::abs(value);
    const bool reverse_threshold = (key.source_subtype == InputSubclass::ControllerAxis && initial_value > 0.5f);

    for (InputBindingKey& other_key : s_input_binding_new_bindings)
    {
      // if this key is in our new binding list, it's a "release", and we're done
      if (other_key.MaskDirection() == key.MaskDirection())
      {
        // for pedals, we wait for it to go back to near its starting point to commit the binding
        if ((reverse_threshold ? ((initial_value - value) <= 0.25f) : (abs_value < 0.5f)))
        {
          // did we go the full range?
          if (reverse_threshold && initial_value > 0.5f && min_value <= -0.5f)
            other_key.modifier = InputModifier::FullAxis;

          SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
          const std::string new_binding(InputManager::ConvertInputBindingKeysToString(
            s_input_binding_type, s_input_binding_new_bindings.data(), s_input_binding_new_bindings.size()));
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
    if ((reverse_threshold ? (abs_value < 0.5f) : (abs_value >= 0.5f)))
    {
      InputBindingKey key_to_add = key;
      key_to_add.modifier = (value < 0.0f && !reverse_threshold) ? InputModifier::Negate : InputModifier::None;
      key_to_add.invert = reverse_threshold;
      s_input_binding_new_bindings.push_back(key_to_add);
    }

    return InputInterceptHook::CallbackResult::StopProcessingEvent;
  });
}

void FullscreenUI::DrawInputBindingWindow()
{
  DebugAssert(s_input_binding_type != InputBindingInfo::Type::Unknown);

  const double time_remaining = INPUT_BINDING_TIMEOUT_SECONDS - s_input_binding_timer.GetTimeSeconds();
  if (time_remaining <= 0.0)
  {
    InputManager::RemoveHook();
    ClearInputBindingVariables();
    return;
  }

  const char* title = FSUI_ICONSTR(ICON_FA_GAMEPAD, "Set Input Binding");
  ImGui::SetNextWindowSize(LayoutScale(500.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup(title);

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  if (ImGui::BeginPopupModal(title, nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
  {
    ImGui::TextWrapped(
      SmallString::FromFmt(FSUI_FSTR("Setting {} binding {}."), s_input_binding_section, s_input_binding_display_name));
    ImGui::TextUnformatted(FSUI_CSTR("Push a controller button or axis now."));
    ImGui::NewLine();
    ImGui::Text(FSUI_CSTR("Timing out in %.0f seconds..."), time_remaining);
    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(4);
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
  const char* value_text =
    (value.has_value()) ?
      ((index < 0 || static_cast<size_t>(index) >= option_count) ? FSUI_CSTR("Unknown") : options[index]) :
      FSUI_CSTR("Use Global Setting");

  if (MenuButtonWithValue(title, summary, value_text, enabled, height, font, summary_font))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(option_count + 1);
    if (game_settings)
      cd_options.emplace_back(FSUI_STR("Use Global Setting"), !value.has_value());
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
                                                   FSUI_STR("Use Global Setting"));

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled, height, font, summary_font))
    ImGui::OpenPopup(title);

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
    if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font,
                                 ImVec2(0.5f, 0.0f)))
    {
      ImGui::CloseCurrentPopup();
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(4);
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
                                                   FSUI_STR("Use Global Setting"));

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled, height, font, summary_font))
    ImGui::OpenPopup(title);

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

    const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    ImGui::SetNextItemWidth(end);
    float dlg_value = value.value_or(default_value) * multiplier;
    if (ImGui::SliderFloat("##value", &dlg_value, min_value * multiplier, max_value * multiplier, format,
                           ImGuiSliderFlags_NoInput))
    {
      dlg_value /= multiplier;

      if (IsEditingGameSettings(bsi) && dlg_value == default_value)
        bsi->DeleteValue(section, key);
      else
        bsi->SetFloatValue(section, key, dlg_value);

      SetSettingsChanged(bsi);
    }

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
    if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font,
                                 ImVec2(0.5f, 0.0f)))
    {
      ImGui::CloseCurrentPopup();
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(4);
  ImGui::PopFont();
}

void FullscreenUI::DrawFloatSpinBoxSetting(SettingsInterface* bsi, const char* title, const char* summary,
                                           const char* section, const char* key, float default_value, float min_value,
                                           float max_value, float step_value, float multiplier, const char* format,
                                           bool enabled, float height, ImFont* font, ImFont* summary_font)
{
  const bool game_settings = IsEditingGameSettings(bsi);
  const std::optional<float> value =
    bsi->GetOptionalFloatValue(section, key, game_settings ? std::nullopt : std::optional<float>(default_value));
  const std::string value_text(value.has_value() ? StringUtil::StdStringFromFormat(format, value.value() * multiplier) :
                                                   FSUI_STR("Use Global Setting"));

  static bool manual_input = false;

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled, height, font, summary_font))
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

    float dlg_value = value.value_or(default_value) * multiplier;
    bool dlg_value_changed = false;

    char str_value[32];
    std::snprintf(str_value, std::size(str_value), format, dlg_value);

    if (manual_input)
    {
      const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
      ImGui::SetNextItemWidth(end);

      // round trip to drop any suffixes (e.g. percent)
      if (auto tmp_value = StringUtil::FromChars<float>(str_value); tmp_value.has_value())
      {
        std::snprintf(str_value, std::size(str_value),
                      ((tmp_value.value() - std::floor(tmp_value.value())) < 0.01f) ? "%.0f" : "%f", tmp_value.value());
      }

      if (ImGui::InputText("##value", str_value, std::size(str_value), ImGuiInputTextFlags_CharsDecimal))
      {
        dlg_value = StringUtil::FromChars<float>(str_value).value_or(dlg_value);
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

      float step = 0;
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
        dlg_value = default_value * multiplier;
        dlg_value_changed = true;
      }

      if (step != 0)
      {
        dlg_value += step * multiplier;
        dlg_value_changed = true;
      }

      ImGui::SetCursorPosY(button_pos.y + (padding.y * 2.0f) +
                           LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY + 10.0f));
    }

    if (dlg_value_changed)
    {
      dlg_value = std::clamp(dlg_value / multiplier, min_value, max_value);
      if (IsEditingGameSettings(bsi) && dlg_value == default_value)
        bsi->DeleteValue(section, key);
      else
        bsi->SetFloatValue(section, key, dlg_value);

      SetSettingsChanged(bsi);
    }

    if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font,
                                 ImVec2(0.5f, 0.0f)))
    {
      ImGui::CloseCurrentPopup();
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(4);
  ImGui::PopFont();
}

#if 0
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
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
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

  ImGui::PopStyleVar(4);
  ImGui::PopFont();
}
#endif

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
    value_text = FSUI_VSTR("Use Global Setting");

  static bool manual_input = false;

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

    if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font,
                                 ImVec2(0.5f, 0.0f)))
    {
      ImGui::CloseCurrentPopup();
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(4);
  ImGui::PopFont();
}

#if 0
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
#endif

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
                                                    FSUI_CSTR("Use Global Setting"),
                          enabled, height, font, summary_font))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(static_cast<u32>(option_count) + 1);
    if (game_settings)
      cd_options.emplace_back(FSUI_CSTR("Use Global Setting"), !value.has_value());
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
                          value.has_value() ? ((index < option_count) ? options[index] : FSUI_CSTR("Unknown")) :
                                              FSUI_CSTR("Use Global Setting"),
                          enabled, height, font, summary_font))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(option_count + 1);
    if (game_settings)
      cd_options.emplace_back(FSUI_CSTR("Use Global Setting"), !value.has_value());
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

                       Host::RunOnCPUThread(&EmuFolders::Update);
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
    ShowToast({}, FSUI_STR("Automatic mapping failed, no devices are available."));
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
  OpenChoiceDialog(FSUI_CSTR("Select Device"), false, std::move(options),
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
                     ShowToast({}, result ? fmt::format(FSUI_FSTR("Automatic mapping completed for {}."), name) :
                                            fmt::format(FSUI_FSTR("Automatic mapping failed for {}."), name));
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
  if (System::GetGameSerial().empty())
    return;

  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(System::GetDiscPath().c_str());
  if (!entry)
  {
    SwitchToGameSettingsForSerial(System::GetGameSerial());
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
  GPUDevice::AdapterAndModeList ml(g_gpu_device->GetAdapterAndModeList());
  s_graphics_adapter_list_cache = std::move(ml.adapter_names);
  s_fullscreen_mode_list_cache = std::move(ml.fullscreen_modes);
  s_fullscreen_mode_list_cache.insert(s_fullscreen_mode_list_cache.begin(), FSUI_STR("Borderless Fullscreen"));
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

  ShowToast("Game Settings Copied", fmt::format(FSUI_FSTR("Game settings initialized with global settings for '{}'."),
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

  ShowToast("Game Settings Cleared", fmt::format(FSUI_FSTR("Game settings have been cleared for '{}'."),
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
      {FSUI_NSTR("Summary"), FSUI_NSTR("Interface Settings"), FSUI_NSTR("Console Settings"),
       FSUI_NSTR("Emulation Settings"), FSUI_NSTR("BIOS Settings"), FSUI_NSTR("Controller Settings"),
       FSUI_NSTR("Hotkey Settings"), FSUI_NSTR("Memory Card Settings"), FSUI_NSTR("Display Settings"),
       FSUI_NSTR("Post-Processing Settings"), FSUI_NSTR("Audio Settings"), FSUI_NSTR("Achievements Settings"),
       FSUI_NSTR("Advanced Settings")}};

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
      NavTitle(Host::TranslateToCString(TR_CONTEXT, titles[static_cast<u32>(pages[index])]));

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

  MenuHeading(FSUI_CSTR("Details"));

  if (s_game_settings_entry)
  {
    if (MenuButton(FSUI_ICONSTR(ICON_FA_WINDOW_MAXIMIZE, "Title"), s_game_settings_entry->title.c_str(), true))
      CopyTextToClipboard(FSUI_STR("Game title copied to clipboard."), s_game_settings_entry->title);
    if (MenuButton(FSUI_ICONSTR(ICON_FA_PAGER, "Serial"), s_game_settings_entry->serial.c_str(), true))
      CopyTextToClipboard(FSUI_STR("Game serial copied to clipboard."), s_game_settings_entry->serial);
    if (MenuButton(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Type"),
                   GameList::GetEntryTypeDisplayName(s_game_settings_entry->type), true))
    {
      CopyTextToClipboard(FSUI_STR("Game type copied to clipboard."),
                          GameList::GetEntryTypeDisplayName(s_game_settings_entry->type));
    }
    if (MenuButton(FSUI_ICONSTR(ICON_FA_BOX, "Region"),
                   Settings::GetDiscRegionDisplayName(s_game_settings_entry->region), true))
    {
      CopyTextToClipboard(FSUI_STR("Game region copied to clipboard."),
                          Settings::GetDiscRegionDisplayName(s_game_settings_entry->region));
    }
    if (MenuButton(FSUI_ICONSTR(ICON_FA_STAR, "Compatibility Rating"),
                   GameDatabase::GetCompatibilityRatingDisplayName(s_game_settings_entry->compatibility), true))
    {
      CopyTextToClipboard(FSUI_STR("Game compatibility rating copied to clipboard."),
                          GameDatabase::GetCompatibilityRatingDisplayName(s_game_settings_entry->compatibility));
    }
    if (MenuButton(FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Path"), s_game_settings_entry->path.c_str(), true))
    {
      CopyTextToClipboard(FSUI_STR("Game path copied to clipboard."), s_game_settings_entry->path);
    }
  }
  else
  {
    MenuButton(FSUI_ICONSTR(ICON_FA_BAN, "Details unavailable for game not scanned in game list."), "");
  }

  MenuHeading(FSUI_CSTR("Options"));

  if (MenuButton(FSUI_ICONSTR(ICON_FA_COPY, "Copy Settings"),
                 FSUI_CSTR("Copies the current global settings to this game.")))
  {
    DoCopyGameSettings();
  }
  if (MenuButton(FSUI_ICONSTR(ICON_FA_TRASH, "Clear Settings"), FSUI_CSTR("Clears all settings set for this game.")))
  {
    DoClearGameSettings();
  }

  EndMenuButtons();
}

void FullscreenUI::DrawInterfaceSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Behavior"));

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_PAUSE, "Pause On Start"),
                    FSUI_CSTR("Pauses the emulator when a game is started."), "Main", "StartPaused", false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_VIDEO, "Pause On Focus Loss"),
                    FSUI_CSTR("Pauses the emulator when you minimize the window or switch to another "
                              "application, and unpauses when you switch back."),
                    "Main", "PauseOnFocusLoss", false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_WINDOW_MAXIMIZE, "Pause On Menu"),
                    FSUI_CSTR("Pauses the emulator when you open the quick menu, and unpauses when you close it."),
                    "Main", "PauseOnMenu", true);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_POWER_OFF, "Confirm Power Off"),
    FSUI_CSTR("Determines whether a prompt will be displayed to confirm shutting down the emulator/game "
              "when the hotkey is pressed."),
    "Main", "ConfirmPowerOff", true);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_SAVE, "Save State On Exit"),
                    FSUI_CSTR("Automatically saves the emulator state when powering down or exiting. You can then "
                              "resume directly from where you left off next time."),
                    "Main", "SaveStateOnExit", true);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_TV, "Start Fullscreen"),
                    FSUI_CSTR("Automatically switches to fullscreen mode when the program is started."), "Main",
                    "StartFullscreen", false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_MOUSE, "Double-Click Toggles Fullscreen"),
                    FSUI_CSTR("Switches between full screen and windowed when the window is double-clicked."), "Main",
                    "DoubleClickTogglesFullscreen", true);
  DrawToggleSetting(bsi, ICON_FA_MOUSE_POINTER "Hide Cursor In Fullscreen",
                    FSUI_CSTR("Hides the mouse pointer/cursor when the emulator is in fullscreen mode."), "Main",
                    "HideCursorInFullscreen", true);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_MAGIC, "Inhibit Screensaver"),
    FSUI_CSTR("Prevents the screen saver from activating and the host from sleeping while emulation is running."),
    "Main", "InhibitScreensaver", true);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_GAMEPAD, "Load Devices From Save States"),
    FSUI_CSTR("When enabled, memory cards and controllers will be overwritten when save states are loaded."), "Main",
    "LoadDevicesFromSaveStates", false);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_COGS, "Apply Per-Game Settings"),
    FSUI_CSTR("When enabled, per-game settings will be applied, and incompatible enhancements will be disabled."),
    "Main", "ApplyGameSettings", true);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_FROWN, "Automatically Load Cheats"),
                    FSUI_CSTR("Automatically loads and applies cheats on game start."), "Main", "AutoLoadCheats", true);
  if (DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_PAINT_BRUSH, "Use Light Theme"),
                        FSUI_CSTR("Uses a light coloured theme instead of the default dark theme."), "Main",
                        "UseLightFullscreenUITheme", false))
  {
    ImGuiFullscreen::SetTheme(bsi->GetBoolValue("Main", "UseLightFullscreenUITheme", false));
  }

#ifdef WITH_DISCORD_PRESENCE
  MenuHeading(FSUI_CSTR("Integration"));
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_CHARGING_STATION, "Enable Discord Presence"),
                    "Shows the game you are currently playing as part of your profile on Discord.", "Main",
                    "EnableDiscordPresence", false);
#endif

  MenuHeading(FSUI_CSTR("On-Screen Display"));
  DrawIntSpinBoxSetting(bsi, FSUI_ICONSTR(ICON_FA_SEARCH, "OSD Scale"),
                        FSUI_CSTR("Determines how large the on-screen messages and monitor are."), "Display",
                        "OSDScale", 100, 25, 500, 1, "%d%%");
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_LIST, "Show OSD Messages"),
                    FSUI_CSTR("Shows on-screen-display messages when events occur."), "Display", "ShowOSDMessages",
                    true);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_CLOCK, "Show Speed"),
    FSUI_CSTR(
      "Shows the current emulation speed of the system in the top-right corner of the display as a percentage."),
    "Display", "ShowSpeed", false);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_RULER, "Show FPS"),
    FSUI_CSTR("Shows the number of frames (or v-syncs) displayed per second by the system in the top-right "
              "corner of the display."),
    "Display", "ShowFPS", false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_BATTERY_HALF, "Show CPU Usage"),
                    FSUI_CSTR("Shows the host's CPU usage based on threads in the top-right corner of the display."),
                    "Display", "ShowCPU", false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_SPINNER, "Show GPU Usage"),
                    FSUI_CSTR("Shows the host's GPU usage in the top-right corner of the display."), "Display",
                    "ShowGPU", false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_RULER_HORIZONTAL, "Show Frame Times"),
                    FSUI_CSTR("Shows a visual history of frame times in the upper-left corner of the display."),
                    "Display", "ShowFrameTimes", false);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_RULER_VERTICAL, "Show Resolution"),
    FSUI_CSTR("Shows the current rendering resolution of the system in the top-right corner of the display."),
    "Display", "ShowResolution", false);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_GAMEPAD, "Show Controller Input"),
    FSUI_CSTR("Shows the current controller state of the system in the bottom-left corner of the display."), "Display",
    "ShowInputs", false);

  EndMenuButtons();
}

void FullscreenUI::DrawBIOSSettingsPage()
{
  static constexpr auto config_keys = make_array("", "PathNTSCJ", "PathNTSCU", "PathPAL");

  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("BIOS Selection"));

  for (u32 i = 0; i < static_cast<u32>(ConsoleRegion::Count); i++)
  {
    const ConsoleRegion region = static_cast<ConsoleRegion>(i);
    if (region == ConsoleRegion::Auto)
      continue;

    TinyString title;
    title.Fmt(FSUI_FSTR("BIOS for {}"), Settings::GetConsoleRegionDisplayName(region));

    const std::optional<std::string> filename(bsi->GetOptionalStringValue(
      "BIOS", config_keys[i], game_settings ? std::nullopt : std::optional<const char*>("")));

    if (MenuButtonWithValue(title,
                            SmallString::FromFmt(FSUI_FSTR("BIOS to use when emulating {} consoles."),
                                                 Settings::GetConsoleRegionDisplayName(region)),
                            filename.has_value() ? (filename->empty() ? FSUI_CSTR("Auto-Detect") : filename->c_str()) :
                                                   FSUI_CSTR("Use Global Setting")))
    {
      ImGuiFullscreen::ChoiceDialogOptions options;
      auto images = BIOS::FindBIOSImagesInDirectory(EmuFolders::Bios.c_str());
      options.reserve(images.size() + 2);
      if (IsEditingGameSettings(bsi))
        options.emplace_back(FSUI_STR("Use Global Setting"), !filename.has_value());
      options.emplace_back(FSUI_STR("Auto-Detect"), filename.has_value() && filename->empty());
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

  DrawFolderSetting(bsi, FSUI_CSTR("BIOS Directory"), "BIOS", "SearchDirectory", EmuFolders::Bios);

  MenuHeading(FSUI_CSTR("Patches"));

  DrawToggleSetting(bsi, FSUI_CSTR("Enable Fast Boot"),
                    FSUI_CSTR("Patches the BIOS to skip the boot animation. Safe to enable."), "BIOS", "PatchFastBoot",
                    Settings::DEFAULT_FAST_BOOT_VALUE);
  DrawToggleSetting(bsi, FSUI_CSTR("Enable TTY Output"),
                    FSUI_CSTR("Patches the BIOS to log calls to printf(). Only use when debugging, can break games."),
                    "BIOS", "PatchTTYEnable", false);

  EndMenuButtons();
}

void FullscreenUI::DrawConsoleSettingsPage()
{
  static constexpr auto cdrom_read_speeds = make_array(
    FSUI_NSTR("None (Double Speed)"), FSUI_NSTR("2x (Quad Speed)"), FSUI_NSTR("3x (6x Speed)"),
    FSUI_NSTR("4x (8x Speed)"), FSUI_NSTR("5x (10x Speed)"), FSUI_NSTR("6x (12x Speed)"), FSUI_NSTR("7x (14x Speed)"),
    FSUI_NSTR("8x (16x Speed)"), FSUI_NSTR("9x (18x Speed)"), FSUI_NSTR("10x (20x Speed)"));

  static constexpr auto cdrom_seek_speeds =
    make_array(FSUI_NSTR("Infinite/Instantaneous"), FSUI_NSTR("None (Normal Speed)"), FSUI_NSTR("2x"), FSUI_NSTR("3x"),
               FSUI_NSTR("4x"), FSUI_NSTR("5x"), FSUI_NSTR("6x"), FSUI_NSTR("7x"), FSUI_NSTR("8x"), FSUI_NSTR("9x"),
               FSUI_NSTR("10x"));

  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Console Settings"));

  DrawEnumSetting(bsi, FSUI_CSTR("Region"), FSUI_CSTR("Determines the emulated hardware type."), "Console", "Region",
                  Settings::DEFAULT_CONSOLE_REGION, &Settings::ParseConsoleRegionName, &Settings::GetConsoleRegionName,
                  &Settings::GetConsoleRegionDisplayName, ConsoleRegion::Count);
  DrawToggleSetting(
    bsi, FSUI_CSTR("Enable 8MB RAM"),
    FSUI_CSTR("Enables an additional 6MB of RAM to obtain a total of 2+6 = 8MB, usually present on dev consoles."),
    "Console", "Enable8MBRAM", false);

  MenuHeading(FSUI_CSTR("CPU Emulation"));

  DrawEnumSetting(bsi, FSUI_CSTR("Execution Mode"), FSUI_CSTR("Determines how the emulated CPU executes instructions."),
                  "CPU", "ExecutionMode", Settings::DEFAULT_CPU_EXECUTION_MODE, &Settings::ParseCPUExecutionMode,
                  &Settings::GetCPUExecutionModeName, &Settings::GetCPUExecutionModeDisplayName,
                  CPUExecutionMode::Count);

  DrawToggleSetting(bsi, FSUI_CSTR("Enable Overclocking"),
                    FSUI_CSTR("When this option is chosen, the clock speed set below will be used."), "CPU",
                    "OverclockEnable", false);

  const bool oc_enable = GetEffectiveBoolSetting(bsi, "CPU", "OverclockEnable", false);
  if (oc_enable)
  {
    u32 oc_numerator = GetEffectiveUIntSetting(bsi, "CPU", "OverclockNumerator", 1);
    u32 oc_denominator = GetEffectiveUIntSetting(bsi, "CPU", "OverclockDenominator", 1);
    s32 oc_percent = static_cast<s32>(Settings::CPUOverclockFractionToPercent(oc_numerator, oc_denominator));
    if (RangeButton(FSUI_CSTR("Overclocking Percentage"),
                    FSUI_CSTR("Selects the percentage of the normal clock speed the emulated hardware will run at."),
                    &oc_percent, 10, 1000, 10, "%d%%"))
    {
      Settings::CPUOverclockPercentToFraction(oc_percent, &oc_numerator, &oc_denominator);
      bsi->SetUIntValue("CPU", "OverclockNumerator", oc_numerator);
      bsi->SetUIntValue("CPU", "OverclockDenominator", oc_denominator);
      SetSettingsChanged(bsi);
    }
  }

  DrawToggleSetting(bsi, FSUI_CSTR("Enable Recompiler ICache"),
                    FSUI_CSTR("Makes games run closer to their console framerate, at a small cost to performance."),
                    "CPU", "RecompilerICache", false);

  MenuHeading(FSUI_CSTR("CD-ROM Emulation"));

  DrawIntListSetting(
    bsi, FSUI_CSTR("Read Speedup"),
    FSUI_CSTR(
      "Speeds up CD-ROM reads by the specified factor. May improve loading speeds in some games, and break others."),
    "CDROM", "ReadSpeedup", 1, cdrom_read_speeds.data(), cdrom_read_speeds.size(), 1);
  DrawIntListSetting(
    bsi, FSUI_CSTR("Seek Speedup"),
    FSUI_CSTR(
      "Speeds up CD-ROM seeks by the specified factor. May improve loading speeds in some games, and break others."),
    "CDROM", "SeekSpeedup", 1, cdrom_seek_speeds.data(), cdrom_seek_speeds.size());

  DrawIntRangeSetting(
    bsi, FSUI_CSTR("Readahead Sectors"),
    FSUI_CSTR("Reduces hitches in emulation by reading/decompressing CD data asynchronously on a worker thread."),
    "CDROM", "ReadaheadSectors", Settings::DEFAULT_CDROM_READAHEAD_SECTORS, 0, 32, "%d sectors");

  DrawToggleSetting(bsi, FSUI_CSTR("Enable Region Check"),
                    FSUI_CSTR("Simulates the region check present in original, unmodified consoles."), "CDROM",
                    "RegionCheck", false);
  DrawToggleSetting(
    bsi, FSUI_CSTR("Preload Images to RAM"),
    FSUI_CSTR("Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay."),
    "CDROM", "LoadImageToRAM", false);
  DrawToggleSetting(
    bsi, FSUI_CSTR("Apply Image Patches"),
    FSUI_CSTR("Automatically applies patches to disc images when they are present, currently only PPF is supported."),
    "CDROM", "LoadImagePatches", false);

  EndMenuButtons();
}

void FullscreenUI::DrawEmulationSettingsPage()
{
  static constexpr auto emulation_speed_values =
    make_array(0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f,
               3.5f, 4.0f, 4.5f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f);
  static constexpr auto emulation_speed_titles =
    make_array(FSUI_NSTR("Unlimited"), "10% [6 FPS (NTSC) / 5 FPS (PAL)]",
               FSUI_NSTR("20% [12 FPS (NTSC) / 10 FPS (PAL)]"), FSUI_NSTR("30% [18 FPS (NTSC) / 15 FPS (PAL)]"),
               FSUI_NSTR("40% [24 FPS (NTSC) / 20 FPS (PAL)]"), FSUI_NSTR("50% [30 FPS (NTSC) / 25 FPS (PAL)]"),
               FSUI_NSTR("60% [36 FPS (NTSC) / 30 FPS (PAL)]"), FSUI_NSTR("70% [42 FPS (NTSC) / 35 FPS (PAL)]"),
               FSUI_NSTR("80% [48 FPS (NTSC) / 40 FPS (PAL)]"), FSUI_NSTR("90% [54 FPS (NTSC) / 45 FPS (PAL)]"),
               FSUI_NSTR("100% [60 FPS (NTSC) / 50 FPS (PAL)]"), FSUI_NSTR("125% [75 FPS (NTSC) / 62 FPS (PAL)]"),
               FSUI_NSTR("150% [90 FPS (NTSC) / 75 FPS (PAL)]"), FSUI_NSTR("175% [105 FPS (NTSC) / 87 FPS (PAL)]"),
               FSUI_NSTR("200% [120 FPS (NTSC) / 100 FPS (PAL)]"), FSUI_NSTR("250% [150 FPS (NTSC) / 125 FPS (PAL)]"),
               FSUI_NSTR("300% [180 FPS (NTSC) / 150 FPS (PAL)]"), FSUI_NSTR("350% [210 FPS (NTSC) / 175 FPS (PAL)]"),
               FSUI_NSTR("400% [240 FPS (NTSC) / 200 FPS (PAL)]"), FSUI_NSTR("450% [270 FPS (NTSC) / 225 FPS (PAL)]"),
               FSUI_NSTR("500% [300 FPS (NTSC) / 250 FPS (PAL)]"), FSUI_NSTR("600% [360 FPS (NTSC) / 300 FPS (PAL)]"),
               FSUI_NSTR("700% [420 FPS (NTSC) / 350 FPS (PAL)]"), FSUI_NSTR("800% [480 FPS (NTSC) / 400 FPS (PAL)]"),
               FSUI_NSTR("900% [540 FPS (NTSC) / 450 FPS (PAL)]"), FSUI_NSTR("1000% [600 FPS (NTSC) / 500 FPS (PAL)]"));

  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Speed Control"));
  DrawFloatListSetting(
    bsi, FSUI_CSTR("Emulation Speed"),
    FSUI_CSTR("Sets the target emulation speed. It is not guaranteed that this speed will be reached on all systems."),
    "Main", "EmulationSpeed", 1.0f, emulation_speed_titles.data(), emulation_speed_values.data(),
    emulation_speed_titles.size());
  DrawFloatListSetting(
    bsi, FSUI_CSTR("Fast Forward Speed"),
    FSUI_CSTR("Sets the fast forward speed. It is not guaranteed that this speed will be reached on all systems."),
    "Main", "FastForwardSpeed", 0.0f, emulation_speed_titles.data(), emulation_speed_values.data(),
    emulation_speed_titles.size());
  DrawFloatListSetting(
    bsi, FSUI_CSTR("Turbo Speed"),
    FSUI_CSTR("Sets the turbo speed. It is not guaranteed that this speed will be reached on all systems."), "Main",
    "TurboSpeed", 2.0f, emulation_speed_titles.data(), emulation_speed_values.data(), emulation_speed_titles.size());

  MenuHeading(FSUI_CSTR("Runahead/Rewind"));

  DrawToggleSetting(bsi, FSUI_CSTR("Enable Rewinding"),
                    FSUI_CSTR("Saves state periodically so you can rewind any mistakes while playing."), "Main",
                    "RewindEnable", false);
  DrawFloatRangeSetting(
    bsi, FSUI_CSTR("Rewind Save Frequency"),
    FSUI_CSTR("How often a rewind state will be created. Higher frequencies have greater system requirements."), "Main",
    "RewindFrequency", 10.0f, 0.0f, 3600.0f, "%.2f Seconds");
  DrawIntRangeSetting(
    bsi, FSUI_CSTR("Rewind Save Slots"),
    FSUI_CSTR("How many saves will be kept for rewinding. Higher values have greater memory requirements."), "Main",
    "RewindSaveSlots", 10, 1, 10000, "%d Frames");

  const s32 runahead_frames = GetEffectiveIntSetting(bsi, "Main", "RunaheadFrameCount", 0);
  const bool runahead_enabled = (runahead_frames > 0);
  const bool rewind_enabled = GetEffectiveBoolSetting(bsi, "Main", "RewindEnable", false);

  static constexpr auto runahead_options =
    make_array(FSUI_NSTR("Disabled"), FSUI_NSTR("1 Frame"), FSUI_NSTR("2 Frames"), FSUI_NSTR("3 Frames"),
               FSUI_NSTR("4 Frames"), FSUI_NSTR("5 Frames"), FSUI_NSTR("6 Frames"), FSUI_NSTR("7 Frames"),
               FSUI_NSTR("8 Frames"), FSUI_NSTR("9 Frames"), FSUI_NSTR("10 Frames"));

  DrawIntListSetting(
    bsi, FSUI_CSTR("Runahead"),
    FSUI_CSTR(
      "Simulates the system ahead of time and rolls back/replays to reduce input lag. Very high system requirements."),
    "Main", "RunaheadFrameCount", 0, runahead_options.data(), runahead_options.size());

  TinyString rewind_summary;
  if (runahead_enabled)
  {
    rewind_summary = FSUI_VSTR("Rewind is disabled because runahead is enabled. Runahead will significantly increase "
                               "system requirements.");
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
    rewind_summary.Fmt(
      FSUI_FSTR("Rewind for {0} frames, lasting {1:.2f} seconds will require up to {3} MB of RAM and {4} MB of VRAM."),
      rewind_save_slots, duration, ram_usage / 1048576, vram_usage / 1048576);
  }
  else
  {
    rewind_summary = FSUI_VSTR(
      "Rewind is not enabled. Please note that enabling rewind may significantly increase system requirements.");
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

  ShowToast(std::string(), FSUI_STR("Per-game controller configuration initialized with global settings."));
}

void FullscreenUI::DoLoadInputProfile()
{
  std::vector<std::string> profiles(InputManager::GetInputProfileNames());
  if (profiles.empty())
  {
    ShowToast(std::string(), FSUI_STR("No input profiles available."));
    return;
  }

  ImGuiFullscreen::ChoiceDialogOptions coptions;
  coptions.reserve(profiles.size());
  for (std::string& name : profiles)
    coptions.emplace_back(std::move(name), false);
  OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Load Profile"), false, std::move(coptions),
                   [](s32 index, const std::string& title, bool checked) {
                     if (index < 0)
                       return;

                     INISettingsInterface ssi(System::GetInputProfilePath(title));
                     if (!ssi.Load())
                     {
                       ShowToast(std::string(), fmt::format(FSUI_FSTR("Failed to load '{}'."), title));
                       CloseChoiceDialog();
                       return;
                     }

                     auto lock = Host::GetSettingsLock();
                     SettingsInterface* dsi = GetEditingSettingsInterface();
                     InputManager::CopyConfiguration(dsi, ssi, true, true, IsEditingGameSettings(dsi));
                     SetSettingsChanged(dsi);
                     ShowToast(std::string(), fmt::format(FSUI_FSTR("Input profile '{}' loaded."), title));
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
    ShowToast(std::string(), fmt::format(FSUI_FSTR("Input profile '{}' saved."), name));
  else
    ShowToast(std::string(), fmt::format(FSUI_FSTR("Failed to save input profile '{}'."), name));
}

void FullscreenUI::DoSaveInputProfile()
{
  std::vector<std::string> profiles(InputManager::GetInputProfileNames());
  if (profiles.empty())
  {
    ShowToast(std::string(), FSUI_STR("No input profiles available."));
    return;
  }

  ImGuiFullscreen::ChoiceDialogOptions coptions;
  coptions.reserve(profiles.size() + 1);
  coptions.emplace_back("Create New...", false);
  for (std::string& name : profiles)
    coptions.emplace_back(std::move(name), false);
  OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_SAVE, "Save Profile"), false, std::move(coptions),
                   [](s32 index, const std::string& title, bool checked) {
                     if (index < 0)
                       return;

                     if (index > 0)
                     {
                       DoSaveInputProfile(title);
                       CloseChoiceDialog();
                       return;
                     }

                     CloseChoiceDialog();

                     OpenInputStringDialog(FSUI_ICONSTR(ICON_FA_SAVE, "Save Profile"),
                                           FSUI_STR("Enter the name of the input profile you wish to create."),
                                           std::string(), FSUI_ICONSTR(ICON_FA_FOLDER_PLUS, "Create"),
                                           [](std::string title) {
                                             if (!title.empty())
                                               DoSaveInputProfile(title);
                                           });
                   });
}

void FullscreenUI::ResetControllerSettings()
{
  SettingsInterface* dsi = GetEditingSettingsInterface();

  CommonHost::SetDefaultControllerSettings(*dsi);
  ShowToast(std::string(), FSUI_STR("Controller settings reset to default."));
}

void FullscreenUI::DrawControllerSettingsPage()
{
  BeginMenuButtons();

  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  MenuHeading("Configuration");

  if (IsEditingGameSettings(bsi))
  {
    if (DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_COG, "Per-Game Configuration"),
                          FSUI_CSTR("Uses game-specific settings for controllers for this game."), "Pad",
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
    if (MenuButton(FSUI_ICONSTR(ICON_FA_COPY, "Copy Global Settings"),
                   FSUI_CSTR("Copies the global controller configuration to this game.")))
      CopyGlobalControllerSettingsToGame();
  }
  else
  {
    if (MenuButton(FSUI_ICONSTR(ICON_FA_FOLDER_MINUS, "Reset Settings"),
                   FSUI_CSTR("Resets all configuration to defaults (including bindings).")))
    {
      ResetControllerSettings();
    }
  }

  if (MenuButton(FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Load Profile"),
                 FSUI_CSTR("Replaces these settings with a previously saved input profile.")))
  {
    DoLoadInputProfile();
  }
  if (MenuButton(FSUI_ICONSTR(ICON_FA_SAVE, "Save Profile"),
                 FSUI_CSTR("Stores the current settings to an input profile.")))
  {
    DoSaveInputProfile();
  }

  MenuHeading("Input Sources");

#ifdef WITH_SDL2
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_COG, "Enable SDL Input Source"),
                    FSUI_CSTR("The SDL input source supports most controllers."), "InputSources", "SDL", true, true,
                    false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_WIFI, "SDL DualShock 4 / DualSense Enhanced Mode"),
                    FSUI_CSTR("Provides vibration and LED control support over Bluetooth."), "InputSources",
                    "SDLControllerEnhancedMode", false, bsi->GetBoolValue("InputSources", "SDL", true), false);
#endif
#ifdef _WIN32
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_COG, "Enable XInput Input Source"),
                    FSUI_CSTR("The XInput source provides support for XBox 360/XBox One/XBox Series controllers."),
                    "InputSources", "XInput", false);
#endif

  MenuHeading("Multitap");
  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_PLUS_SQUARE, "Multitap Mode"),
                  FSUI_CSTR("Enables an additional three controller slots on each port. Not supported in all games."),
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

    if (mtap_enabled[mtap_port])
    {
      MenuHeading(TinyString::FromFmt(fmt::runtime(FSUI_ICONSTR(ICON_FA_PLUG, "Controller Port {}{}")), mtap_port + 1,
                                      mtap_slot_names[mtap_slot]));
    }
    else
    {
      MenuHeading(TinyString::FromFmt(fmt::runtime(FSUI_ICONSTR(ICON_FA_PLUG, "Controller Port {}")), mtap_port + 1));
    }

    const std::string section(fmt::format("Pad{}", global_slot + 1));
    const std::string type(bsi->GetStringValue(section.c_str(), "Type", Controller::GetDefaultPadType(global_slot)));
    const Controller::ControllerInfo* ci = Controller::GetControllerInfo(type);
    if (MenuButton(TinyString::FromFmt("{}##type{}", FSUI_ICONSTR(ICON_FA_GAMEPAD, "Controller Type"), global_slot),
                   ci ? ci->display_name : FSUI_CSTR("Unknown")))
    {
      std::vector<std::pair<std::string, std::string>> raw_options(Controller::GetControllerTypeNames());
      ImGuiFullscreen::ChoiceDialogOptions options;
      options.reserve(raw_options.size());
      for (auto& it : raw_options)
      {
        options.emplace_back(std::move(it.second), type == it.first);
      }
      OpenChoiceDialog(TinyString::FromFmt(FSUI_FSTR("Port {} Controller Type"), global_slot + 1), false,
                       std::move(options),
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

    if (MenuButton(FSUI_ICONSTR(ICON_FA_MAGIC, "Automatic Mapping"),
                   FSUI_CSTR("Attempts to map the selected port to a chosen controller.")))
    {
      StartAutomaticBinding(global_slot);
    }

    for (u32 i = 0; i < ci->num_bindings; i++)
    {
      const Controller::ControllerBindingInfo& bi = ci->bindings[i];
      DrawInputBindingButton(bsi, bi.type, section.c_str(), bi.name, bi.display_name, true);
    }

    if (mtap_enabled[mtap_port])
    {
      MenuHeading(SmallString::FromFmt(fmt::runtime(FSUI_ICONSTR(ICON_FA_MICROCHIP, "Controller Port {}{} Macros")),
                                       mtap_port + 1, mtap_slot_names[mtap_slot]));
    }
    else
    {
      MenuHeading(SmallString::FromFmt(fmt::runtime(FSUI_ICONSTR(ICON_FA_MICROCHIP, "Controller Port {} Macros")),
                                       mtap_port + 1));
    }

    for (u32 macro_index = 0; macro_index < InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER; macro_index++)
    {
      DrawInputBindingButton(bsi, InputBindingInfo::Type::Macro, section.c_str(),
                             TinyString::FromFmt("Macro{}", macro_index + 1),
                             TinyString::FromFmt(FSUI_FSTR("Macro {} Trigger"), macro_index + 1));

      std::string binds_string(
        bsi->GetStringValue(section.c_str(), fmt::format("Macro{}Binds", macro_index + 1).c_str()));
      if (MenuButton(
            TinyString::FromFmt(fmt::runtime(FSUI_ICONSTR(ICON_FA_KEYBOARD, "Macro {} Buttons")), macro_index + 1),
            binds_string.empty() ? FSUI_CSTR("No Buttons Selected") : binds_string.c_str()))
      {
        std::vector<std::string_view> buttons_split(StringUtil::SplitString(binds_string, '&', true));
        ImGuiFullscreen::ChoiceDialogOptions options;
        for (u32 i = 0; i < ci->num_bindings; i++)
        {
          const Controller::ControllerBindingInfo& bi = ci->bindings[i];
          if (bi.type != InputBindingInfo::Type::Button && bi.type != InputBindingInfo::Type::Axis &&
              bi.type != InputBindingInfo::Type::HalfAxis)
          {
            continue;
          }
          options.emplace_back(bi.display_name,
                               std::any_of(buttons_split.begin(), buttons_split.end(),
                                           [bi](const std::string_view& it) { return (it == bi.name); }));
        }

        OpenChoiceDialog(
          TinyString::FromFmt(FSUI_FSTR("Select Macro {} Binds"), macro_index + 1), true, std::move(options),
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
            std::vector<std::string_view> buttons_split(StringUtil::SplitString(binds_string, '&', true));
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
      const SmallString freq_title =
        SmallString::FromFmt(fmt::runtime(FSUI_ICONSTR(ICON_FA_LIGHTBULB, "Macro {} Frequency")), macro_index + 1);
      s32 frequency = bsi->GetIntValue(section.c_str(), freq_key.c_str(), 0);
      SmallString freq_summary;
      if (frequency == 0)
        freq_summary = FSUI_VSTR("Macro will not auto-toggle.");
      else
        freq_summary.Fmt(FSUI_FSTR("Macro will toggle every {} frames."), frequency);
      if (MenuButton(freq_title, freq_summary))
        ImGui::OpenPopup(freq_title);

      ImGui::SetNextWindowSize(LayoutScale(500.0f, 180.0f));

      ImGui::PushFont(g_large_font);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                                  ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

      if (ImGui::BeginPopupModal(freq_title, nullptr,
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
      {
        ImGui::SetNextItemWidth(LayoutScale(450.0f));
        if (ImGui::SliderInt("##value", &frequency, 0, 60, FSUI_CSTR("Toggle every %d frames"),
                             ImGuiSliderFlags_NoInput))
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
      if (mtap_enabled[mtap_port])
      {
        MenuHeading(SmallString::FromFmt(fmt::runtime(FSUI_ICONSTR(ICON_FA_SLIDERS_H, "Controller Port {}{} Settings")),
                                         mtap_port + 1, mtap_slot_names[mtap_slot]));
      }
      else
      {
        MenuHeading(SmallString::FromFmt(fmt::runtime(FSUI_ICONSTR(ICON_FA_SLIDERS_H, "Controller Port {} Settings")),
                                         mtap_port + 1));
      }

      for (u32 i = 0; i < ci->num_settings; i++)
      {
        const SettingInfo& si = ci->settings[i];
        TinyString title;
        title.Fmt(ICON_FA_COG "{}", si.display_name);
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
            DrawFloatSpinBoxSetting(bsi, title, si.description, section.c_str(), si.name, si.FloatDefaultValue(),
                                    si.FloatMinValue(), si.FloatMaxValue(), si.FloatStepValue(), si.multiplier,
                                    si.format, true);
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
    if (!last_category || std::strcmp(hotkey->category, last_category->category) != 0)
    {
      MenuHeading(hotkey->category);
      last_category = hotkey;
    }

    DrawInputBindingButton(bsi, InputBindingInfo::Type::Button, "Hotkeys", hotkey->name, hotkey->display_name, false);
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
  if (MenuButton(FSUI_ICONSTR(ICON_FA_PLUS, "Create Memory Card"),
                 FSUI_CSTR("Creates a new memory card file or folder.")))
  {
    OpenInputStringDialog(
      FSUI_ICONSTR(ICON_FA_PLUS, "Create Memory Card"),
      FSUI_CSTR("Enter the name of the memory card you wish to create."),
      "Card Name: ", FSUI_ICONSTR(ICON_FA_FOLDER_PLUS, "Create"), [](std::string memcard_name) {
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
            ShowToast(std::string(), fmt::format(FSUI_FSTR("Failed to create memory card '{}'."), memcard_name));
          }
          else
          {
            ShowToast(std::string(), fmt::format(FSUI_FSTR("Memory card '{}' created."), memcard_name));
          }
        }
        else
        {
          ShowToast(std::string(),
                    fmt::format(FSUI_FSTR("A memory card with the name '{}' already exists."), memcard_name));
        }
      });
  }

  DrawFolderSetting(bsi, FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Memory Card Directory"), "MemoryCards", "Directory",
                    EmuFolders::MemoryCards);

  if (!game_settings && MenuButton(FSUI_ICONSTR(ICON_FA_MAGIC, "Reset Memory Card Directory"),
                                   FSUI_CSTR("Resets memory card directory to default (user directory).")))
  {
    bsi->SetStringValue("MemoryCards", "Directory", "memcards");
    SetSettingsChanged(bsi);
  }

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_SEARCH, "Use Single Card For Sub-Images"),
                    FSUI_CSTR("When using a multi-disc image (m3u/pbp) and per-game (title) memory cards, "
                              "use a single memory card for all discs."),
                    "MemoryCards", "UsePlaylistTitle", true);

  for (u32 i = 0; i < 2; i++)
  {
    MenuHeading(TinyString::FromFmt(FSUI_FSTR("Memory Card Port {}"), i + 1));

    const MemoryCardType default_type =
      (i == 0) ? Settings::DEFAULT_MEMORY_CARD_1_TYPE : Settings::DEFAULT_MEMORY_CARD_2_TYPE;
    DrawEnumSetting(
      bsi, TinyString::FromFmt(fmt::runtime(FSUI_ICONSTR(ICON_FA_SD_CARD, "Memory Card {} Type")), i + 1),
      SmallString::FromFmt(FSUI_FSTR("Sets which sort of memory card image will be used for slot {}."), i + 1),
      "MemoryCards", type_keys[i], default_type, &Settings::ParseMemoryCardTypeName, &Settings::GetMemoryCardTypeName,
      &Settings::GetMemoryCardTypeDisplayName, MemoryCardType::Count);

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
    title.Fmt("{}##card_name_{}", FSUI_ICONSTR(ICON_FA_FILE, "Shared Card Name"), i);
    if (MenuButtonWithValue(title,
                            FSUI_CSTR("The selected memory card image will be used in shared mode for this slot."),
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
  // TODO: Translation context
  static constexpr auto resolution_scales =
    make_array(FSUI_NSTR("Automatic based on window size"), FSUI_NSTR("1x"), FSUI_NSTR("2x"),
               FSUI_NSTR("3x (for 720p)"), FSUI_NSTR("4x"), FSUI_NSTR("5x (for 1080p)"), FSUI_NSTR("6x (for 1440p)"),
               FSUI_NSTR("7x"), FSUI_NSTR("8x"), FSUI_NSTR("9x (for 4K)"), FSUI_NSTR("10x"), FSUI_NSTR("11x"),
               FSUI_NSTR("12x"), FSUI_NSTR("13x"), FSUI_NSTR("14x"), FSUI_NSTR("15x"), FSUI_NSTR("16x"));

  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Device Settings"));

  DrawEnumSetting(bsi, FSUI_CSTR("GPU Renderer"),
                  FSUI_CSTR("Chooses the backend to use for rendering the console/game visuals."), "GPU", "Renderer",
                  Settings::DEFAULT_GPU_RENDERER, &Settings::ParseRendererName, &Settings::GetRendererName,
                  &Settings::GetRendererDisplayName, GPURenderer::Count);

  const GPURenderer renderer =
    Settings::ParseRendererName(
      GetEffectiveStringSetting(bsi, "GPU", "Renderer", Settings::GetRendererName(Settings::DEFAULT_GPU_RENDERER))
        .c_str())
      .value_or(Settings::DEFAULT_GPU_RENDERER);
  const bool is_hardware = (renderer != GPURenderer::Software);

  std::optional<std::string> adapter(
    bsi->GetOptionalStringValue("GPU", "Adapter", game_settings ? std::nullopt : std::optional<const char*>("")));

  if (MenuButtonWithValue(FSUI_CSTR("GPU Adapter"), FSUI_CSTR("Selects the GPU to use for rendering."),
                          adapter.has_value() ? (adapter->empty() ? FSUI_CSTR("Default") : adapter->c_str()) :
                                                FSUI_CSTR("Use Global Setting")))
  {
    GPUDevice::AdapterAndModeList aml(g_gpu_device->GetAdapterAndModeList());

    ImGuiFullscreen::ChoiceDialogOptions options;
    options.reserve(aml.adapter_names.size() + 2);
    if (game_settings)
      options.emplace_back(FSUI_STR("Use Global Setting"), !adapter.has_value());
    options.emplace_back(FSUI_STR("Default"), adapter.has_value() && adapter->empty());
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
      ShowToast(std::string(), FSUI_STR("GPU adapter will be applied after restarting."), 10.0f);
      CloseChoiceDialog();
    };
    OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_TV, "GPU Adapter"), false, std::move(options), std::move(callback));
  }

  std::optional<std::string> fsmode(bsi->GetOptionalStringValue(
    "GPU", "FullscreenMode", game_settings ? std::nullopt : std::optional<const char*>("")));

  if (MenuButtonWithValue(
        FSUI_CSTR("Fullscreen Resolution"), FSUI_CSTR("Selects the resolution to use in fullscreen modes."),
        fsmode.has_value() ? (fsmode->empty() ? FSUI_CSTR("Borderless Fullscreen") : fsmode->c_str()) :
                             FSUI_CSTR("Use Global Setting")))
  {
    GPUDevice::AdapterAndModeList aml(g_gpu_device->GetAdapterAndModeList());

    ImGuiFullscreen::ChoiceDialogOptions options;
    options.reserve(aml.fullscreen_modes.size() + 2);
    if (game_settings)
      options.emplace_back(FSUI_STR("Use Global Setting"), !fsmode.has_value());
    options.emplace_back(FSUI_STR("Borderless Fullscreen"), fsmode.has_value() && fsmode->empty());
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
      ShowToast(std::string(), FSUI_STR("Resolution change will be applied after restarting."), 10.0f);
      CloseChoiceDialog();
    };
    OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_TV, "Fullscreen Resolution"), false, std::move(options), std::move(callback));
  }

  switch (renderer)
  {
#ifdef _WIN32
    case GPURenderer::HardwareD3D11:
    {
      DrawToggleSetting(
        bsi, FSUI_CSTR("Use Blit Swap Chain"),
        FSUI_CSTR("Uses a blit presentation model instead of flipping. This may be needed on some systems."), "Display",
        "UseBlitSwapChain", false);
    }
    break;
#endif

#ifdef WITH_VULKAN
    case GPURenderer::HardwareVulkan:
    {
      DrawToggleSetting(bsi, FSUI_CSTR("Threaded Presentation"),
                        FSUI_CSTR("Presents frames on a background thread when fast forwarding or vsync is disabled."),
                        "GPU", "ThreadedPresentation", true);
    }
    break;
#endif

    case GPURenderer::Software:
    {
      DrawToggleSetting(bsi, FSUI_CSTR("Threaded Rendering"),
                        FSUI_CSTR("Uses a second thread for drawing graphics. Speed boost, and safe to use."), "GPU",
                        "UseThread", true);
    }
    break;

    default:
      break;
  }

  if (renderer != GPURenderer::Software)
  {
    DrawToggleSetting(
      bsi, FSUI_CSTR("Use Software Renderer For Readbacks"),
      FSUI_CSTR("Runs the software renderer in parallel for VRAM readbacks. On some systems, this may result "
                "in greater performance."),
      "GPU", "UseSoftwareRendererForReadbacks", false);
  }

  DrawToggleSetting(
    bsi, FSUI_CSTR("Enable VSync"),
    FSUI_CSTR("Synchronizes presentation of the console's frames to the host. Enable for smoother animations."),
    "Display", "VSync", Settings::DEFAULT_VSYNC_VALUE);

  DrawToggleSetting(
    bsi, FSUI_CSTR("Sync To Host Refresh Rate"),
    FSUI_CSTR("Adjusts the emulation speed so the console's refresh rate matches the host when VSync and Audio "
              "Resampling are enabled."),
    "Main", "SyncToHostRefreshRate", false);

  DrawToggleSetting(bsi, FSUI_CSTR("Optimal Frame Pacing"),
                    FSUI_CSTR("Ensures every frame generated is displayed for optimal pacing. Disable if you are "
                              "having speed or sound issues."),
                    "Display", "DisplayAllFrames", false);

  MenuHeading("Rendering");

  DrawIntListSetting(
    bsi, FSUI_CSTR("Internal Resolution Scale"),
    FSUI_CSTR("Scales internal VRAM resolution by the specified multiplier. Some games require 1x VRAM resolution."),
    "GPU", "ResolutionScale", 1, resolution_scales.data(), resolution_scales.size(), 0, is_hardware);

  DrawEnumSetting(
    bsi, FSUI_CSTR("Texture Filtering"), FSUI_CSTR("Smooths out the blockiness of magnified textures on 3D objects."),
    "GPU", "TextureFilter", Settings::DEFAULT_GPU_TEXTURE_FILTER, &Settings::ParseTextureFilterName,
    &Settings::GetTextureFilterName, &Settings::GetTextureFilterDisplayName, GPUTextureFilter::Count, is_hardware);

  DrawToggleSetting(bsi, FSUI_CSTR("True Color Rendering"),
                    FSUI_CSTR("Disables dithering and uses the full 8 bits per channel of color information."), "GPU",
                    "TrueColor", true, is_hardware);

  DrawToggleSetting(bsi, "Widescreen Hack",
                    FSUI_CSTR("Increases the field of view from 4:3 to the chosen display aspect ratio in 3D games."),
                    "GPU", "WidescreenHack", false, is_hardware);

  DrawToggleSetting(
    bsi, FSUI_CSTR("PGXP Geometry Correction"),
    FSUI_CSTR("Reduces \"wobbly\" polygons by attempting to preserve the fractional component through memory "
              "transfers."),
    "GPU", "PGXPEnable", false);

  MenuHeading(FSUI_CSTR("Screen Display"));

  DrawEnumSetting(bsi, FSUI_CSTR("Aspect Ratio"),
                  FSUI_CSTR("Changes the aspect ratio used to display the console's output to the screen."), "Display",
                  "AspectRatio", Settings::DEFAULT_DISPLAY_ASPECT_RATIO, &Settings::ParseDisplayAspectRatio,
                  &Settings::GetDisplayAspectRatioName, &Settings::GetDisplayAspectRatioDisplayName,
                  DisplayAspectRatio::Count);

  DrawEnumSetting(bsi, FSUI_CSTR("Crop Mode"),
                  FSUI_CSTR("Determines how much of the area typically not visible on a consumer TV set to crop/hide."),
                  "Display", "CropMode", Settings::DEFAULT_DISPLAY_CROP_MODE, &Settings::ParseDisplayCropMode,
                  &Settings::GetDisplayCropModeName, &Settings::GetDisplayCropModeDisplayName, DisplayCropMode::Count);

  DrawEnumSetting(
    bsi, FSUI_CSTR("Position"), FSUI_CSTR("Determines the position on the screen when black borders must be added."),
    "Display", "Alignment", Settings::DEFAULT_DISPLAY_ALIGNMENT, &Settings::ParseDisplayAlignment,
    &Settings::GetDisplayAlignmentDisplayName, &Settings::GetDisplayAlignmentDisplayName, DisplayAlignment::Count);

  DrawEnumSetting(bsi, FSUI_CSTR("Downsampling"),
                  FSUI_CSTR("Downsamples the rendered image prior to displaying it. Can improve "
                            "overall image quality in mixed 2D/3D games."),
                  "GPU", "DownsampleMode", Settings::DEFAULT_GPU_DOWNSAMPLE_MODE, &Settings::ParseDownsampleModeName,
                  &Settings::GetDownsampleModeName, &Settings::GetDownsampleModeDisplayName, GPUDownsampleMode::Count,
                  (renderer != GPURenderer::Software));

  DrawToggleSetting(bsi, FSUI_CSTR("Linear Upscaling"),
                    FSUI_CSTR("Uses a bilinear filter when upscaling to display, smoothing out the image."), "Display",
                    "LinearFiltering", true);

  DrawToggleSetting(bsi, FSUI_CSTR("Integer Upscaling"),
                    FSUI_CSTR("Adds padding to ensure pixels are a whole number in size."), "Display", "IntegerScaling",
                    false);

  DrawToggleSetting(bsi, FSUI_CSTR("Stretch To Fit"),
                    FSUI_CSTR("Fills the window with the active display area, regardless of the aspect ratio."),
                    "Display", "Stretch", false);

  DrawToggleSetting(bsi, FSUI_CSTR("Internal Resolution Screenshots"),
                    FSUI_CSTR("Saves screenshots at internal render resolution and without postprocessing."), "Display",
                    "InternalResolutionScreenshots", false);

  MenuHeading(FSUI_CSTR("Enhancements"));
  DrawToggleSetting(
    bsi, FSUI_CSTR("Scaled Dithering"),
    FSUI_CSTR("Scales the dithering pattern with the internal rendering resolution, making it less noticeable. "
              "Usually safe to enable."),
    "GPU", "ScaledDithering", true, is_hardware);

  DrawToggleSetting(
    bsi, FSUI_CSTR("Disable Interlacing"),
    FSUI_CSTR("Disables interlaced rendering and display in the GPU. Some games can render in 480p this way, "
              "but others will break."),
    "GPU", "DisableInterlacing", true);
  DrawToggleSetting(
    bsi, FSUI_CSTR("Force NTSC Timings"),
    FSUI_CSTR("Forces PAL games to run at NTSC timings, i.e. 60hz. Some PAL games will run at their \"normal\" "
              "speeds, while others will break."),
    "GPU", "ForceNTSCTimings", false);
  DrawToggleSetting(
    bsi, FSUI_CSTR("Force 4:3 For 24-Bit Display"),
    FSUI_CSTR("Switches back to 4:3 display aspect ratio when displaying 24-bit content, usually FMVs."), "Display",
    "Force4_3For24Bit", false);
  DrawToggleSetting(bsi, FSUI_CSTR("Chroma Smoothing For 24-Bit Display"),
                    FSUI_CSTR("Smooths out blockyness between colour transitions in 24-bit content, usually FMVs. Only "
                              "applies to the hardware renderers."),
                    "GPU", "ChromaSmoothing24Bit", false);

  MenuHeading("PGXP (Precision Geometry Transform Pipeline)");

  const bool pgxp_enabled = GetEffectiveBoolSetting(bsi, "GPU", "PGXPEnable", false);
  const bool texture_correction_enabled = GetEffectiveBoolSetting(bsi, "GPU", "PGXPTextureCorrection", true);

  DrawToggleSetting(
    bsi, FSUI_CSTR("Perspective Correct Textures"),
    FSUI_CSTR("Uses perspective-correct interpolation for texture coordinates, straightening out warped textures."),
    "GPU", "PGXPTextureCorrection", true, pgxp_enabled);
  DrawToggleSetting(
    bsi, FSUI_CSTR("Perspective Correct Colors"),
    FSUI_CSTR("Uses perspective-correct interpolation for colors, which can improve visuals in some games."), "GPU",
    "PGXPColorCorrection", false, pgxp_enabled);
  DrawToggleSetting(bsi, FSUI_CSTR("Culling Correction"),
                    FSUI_CSTR("Increases the precision of polygon culling, reducing the number of holes in geometry."),
                    "GPU", "PGXPCulling", true, pgxp_enabled);
  DrawToggleSetting(
    bsi, FSUI_CSTR("Preserve Projection Precision"),
    FSUI_CSTR("Adds additional precision to PGXP data post-projection. May improve visuals in some games."), "GPU",
    "PGXPPreserveProjFP", false, pgxp_enabled);
  DrawToggleSetting(bsi, FSUI_CSTR("Depth Buffer"),
                    FSUI_CSTR("Reduces polygon Z-fighting through depth testing. Low compatibility with games."), "GPU",
                    "PGXPDepthBuffer", false, pgxp_enabled && texture_correction_enabled);
  DrawToggleSetting(bsi, FSUI_CSTR("CPU Mode"),
                    FSUI_CSTR("Uses PGXP for all instructions, not just memory operations."), "GPU", "PGXPCPU", false,
                    pgxp_enabled);

  MenuHeading(FSUI_CSTR("Texture Replacements"));

  DrawToggleSetting(bsi, FSUI_CSTR("Enable VRAM Write Texture Replacement"),
                    FSUI_CSTR("Enables the replacement of background textures in supported games."),
                    "TextureReplacements", "EnableVRAMWriteReplacements", false);
  DrawToggleSetting(bsi, FSUI_CSTR("Preload Replacement Textures"),
                    FSUI_CSTR("Loads all replacement texture to RAM, reducing stuttering at runtime."),
                    "TextureReplacements", "PreloadTextures", false);

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
    g_gpu_device->SetPostProcessingChain(config);
  if (IsEditingGameSettings(bsi))
  {
    s_game_settings_interface->Save();
  }
  else
  {
    s_settings_changed.store(true, std::memory_order_release);
  }
}

enum
{
  POSTPROCESSING_ACTION_NONE = 0,
  POSTPROCESSING_ACTION_REMOVE,
  POSTPROCESSING_ACTION_MOVE_UP,
  POSTPROCESSING_ACTION_MOVE_DOWN,
};

void FullscreenUI::DrawPostProcessingSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Controls"));

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_MAGIC, "Enable Post Processing"),
                    FSUI_CSTR("If not enabled, the current post processing chain will be ignored."), "Display",
                    "PostProcessing", false);

  if (MenuButton(FSUI_ICONSTR(ICON_FA_SEARCH, "Reload Shaders"),
                 FSUI_CSTR("Reloads the shaders from disk, applying any changes."),
                 bsi->GetBoolValue("Display", "PostProcessing", false)))
  {
    const std::string chain(bsi->GetStringValue("Display", "PostProcessChain", ""));
    g_gpu_device->SetPostProcessingChain(chain);
    if (chain.empty())
      ShowToast(std::string(), FSUI_STR("Post-processing chain is empty."));
    else
      ShowToast(std::string(), FSUI_STR("Post-processing shaders reloaded."));
  }

  MenuHeading(FSUI_CSTR("Operations"));

  if (MenuButton(FSUI_ICONSTR(ICON_FA_PLUS, "Add Shader"), FSUI_CSTR("Adds a new shader to the chain.")))
  {
    ImGuiFullscreen::ChoiceDialogOptions options;
    for (std::string& name : PostProcessingChain::GetAvailableShaderNames())
      options.emplace_back(std::move(name), false);

    OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_PLUS, "Add Shader"), false, std::move(options),
                     [](s32 index, const std::string& title, bool checked) {
                       if (index < 0)
                         return;

                       if (s_postprocessing_chain.AddStage(title))
                       {
                         ShowToast(std::string(), fmt::format(FSUI_FSTR("Shader {} added as stage {}."), title,
                                                              s_postprocessing_chain.GetStageCount()));
                         SavePostProcessingChain();
                       }
                       else
                       {
                         ShowToast(std::string(),
                                   fmt::format(FSUI_FSTR("Failed to load shader {}. It may be invalid."), title));
                       }

                       CloseChoiceDialog();
                     });
  }

  if (MenuButton(FSUI_ICONSTR(ICON_FA_TIMES, "Clear Shaders"), FSUI_CSTR("Clears a shader from the chain.")))
  {
    OpenConfirmMessageDialog(
      FSUI_ICONSTR(ICON_FA_TIMES, "Clear Shaders"),
      FSUI_CSTR("Are you sure you want to clear the current post-processing chain? All configuration will be lost."),
      [](bool confirmed) {
        if (!confirmed)
          return;

        s_postprocessing_chain.ClearStages();
        ShowToast(std::string(), FSUI_STR("Post-processing chain cleared."));
        SavePostProcessingChain();
      });
  }

  u32 postprocessing_action = POSTPROCESSING_ACTION_NONE;
  u32 postprocessing_action_index = 0;

  SmallString str;
  SmallString tstr;
  for (u32 stage_index = 0; stage_index < s_postprocessing_chain.GetStageCount(); stage_index++)
  {
    ImGui::PushID(stage_index);
    PostProcessingShader* stage = s_postprocessing_chain.GetShaderStage(stage_index);
    str.Fmt(FSUI_FSTR("Stage {}: {}"), stage_index + 1, stage->GetName());
    MenuHeading(str);

    if (MenuButton(FSUI_ICONSTR(ICON_FA_TIMES, "Remove From Chain"), FSUI_CSTR("Removes this shader from the chain.")))
    {
      postprocessing_action = POSTPROCESSING_ACTION_REMOVE;
      postprocessing_action_index = stage_index;
    }

    if (MenuButton(FSUI_ICONSTR(ICON_FA_ARROW_UP, "Move Up"),
                   FSUI_CSTR("Moves this shader higher in the chain, applying it earlier."), (stage_index > 0)))
    {
      postprocessing_action = POSTPROCESSING_ACTION_MOVE_UP;
      postprocessing_action_index = stage_index;
    }

    if (MenuButton(FSUI_ICONSTR(ICON_FA_ARROW_DOWN, "Move Down"),
                   FSUI_CSTR("Moves this shader lower in the chain, applying it later."),
                   (stage_index != (s_postprocessing_chain.GetStageCount() - 1))))
    {
      postprocessing_action = POSTPROCESSING_ACTION_MOVE_DOWN;
      postprocessing_action_index = stage_index;
    }

    for (PostProcessingShader::Option& opt : stage->GetOptions())
    {
      switch (opt.type)
      {
        case PostProcessingShader::Option::Type::Bool:
        {
          bool value = (opt.value[0].int_value != 0);
          tstr.Fmt(ICON_FA_COGS "{}", opt.ui_name);
          if (ToggleButton(tstr,
                           (opt.default_value[0].int_value != 0) ? FSUI_CSTR("Default: Enabled") :
                                                                   FSUI_CSTR("Default: Disabled"),
                           &value))
          {
            opt.value[0].int_value = (value != 0);
            SavePostProcessingChain();
          }
        }
        break;

        case PostProcessingShader::Option::Type::Float:
        {
          tstr.Fmt(ICON_FA_RULER_VERTICAL "{}##{}", opt.ui_name, opt.name);
          str.Fmt(FSUI_FSTR("Value: {} | Default: {} | Minimum: {} | Maximum: {}"), opt.value[0].float_value,
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
            if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font,
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

        case PostProcessingShader::Option::Type::Int:
        {
          tstr.Fmt(ICON_FA_RULER_VERTICAL "{}##{}", opt.ui_name, opt.name);
          str.Fmt(FSUI_FSTR("Value: {} | Default: {} | Minimum: {} | Maximum: {}"), opt.value[0].int_value,
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
            if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, g_large_font,
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

    ImGui::PopID();
  }

  switch (postprocessing_action)
  {
    case POSTPROCESSING_ACTION_REMOVE:
    {
      PostProcessingShader* stage = s_postprocessing_chain.GetShaderStage(postprocessing_action_index);
      ShowToast(std::string(),
                fmt::format(FSUI_FSTR("Removed stage {} ({})."), postprocessing_action_index + 1, stage->GetName()));
      s_postprocessing_chain.RemoveStage(postprocessing_action_index);
      SavePostProcessingChain();
    }
    break;
    case POSTPROCESSING_ACTION_MOVE_UP:
    {
      s_postprocessing_chain.MoveStageUp(postprocessing_action_index);
      SavePostProcessingChain();
    }
    break;
    case POSTPROCESSING_ACTION_MOVE_DOWN:
    {
      s_postprocessing_chain.MoveStageDown(postprocessing_action_index);
      SavePostProcessingChain();
    }
    break;
    default:
      break;
  }

  EndMenuButtons();
}

void FullscreenUI::DrawAudioSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading("Audio Control");

  DrawIntRangeSetting(bsi, FSUI_CSTR("Output Volume"),
                      FSUI_CSTR("Controls the volume of the audio played on the host."), "Audio", "OutputVolume", 100,
                      0, 100, "%d%%");
  DrawIntRangeSetting(bsi, FSUI_CSTR("Fast Forward Volume"),
                      FSUI_CSTR("Controls the volume of the audio played on the host when fast forwarding."), "Audio",
                      "FastForwardVolume", 100, 0, 100, "%d%%");
  DrawToggleSetting(bsi, FSUI_CSTR("Mute All Sound"),
                    FSUI_CSTR("Prevents the emulator from producing any audible sound."), "Audio", "OutputMuted",
                    false);
  DrawToggleSetting(bsi, FSUI_CSTR("Mute CD Audio"),
                    FSUI_CSTR("Forcibly mutes both CD-DA and XA audio from the CD-ROM. Can be used to "
                              "disable background music in some games."),
                    "CDROM", "MuteCDAudio", false);

  MenuHeading(FSUI_CSTR("Backend Settings"));

  DrawEnumSetting(
    bsi, FSUI_CSTR("Audio Backend"),
    FSUI_CSTR("The audio backend determines how frames produced by the emulator are submitted to the host."), "Audio",
    "Backend", Settings::DEFAULT_AUDIO_BACKEND, &Settings::ParseAudioBackend, &Settings::GetAudioBackendName,
    &Settings::GetAudioBackendDisplayName, AudioBackend::Count);
  DrawEnumSetting(bsi, FSUI_CSTR("Stretch Mode"),
                  FSUI_CSTR("Determines quality of audio when not running at 100% speed."), "Audio", "StretchMode",
                  Settings::DEFAULT_AUDIO_STRETCH_MODE, &AudioStream::ParseStretchMode,
                  &AudioStream::GetStretchModeName, &AudioStream::GetStretchModeDisplayName, AudioStretchMode::Count);
  DrawIntRangeSetting(bsi, FSUI_CSTR("Buffer Size"),
                      FSUI_CSTR("Determines the amount of audio buffered before being pulled by the host API."),
                      "Audio", "BufferMS", Settings::DEFAULT_AUDIO_BUFFER_MS, 10, 500, "%d ms");

  const u32 output_latency =
    GetEffectiveUIntSetting(bsi, "Audio", "OutputLatencyMS", Settings::DEFAULT_AUDIO_OUTPUT_LATENCY_MS);
  bool output_latency_minimal = (output_latency == 0);
  if (ToggleButton(FSUI_CSTR("Minimal Output Latency"),
                   FSUI_CSTR("When enabled, the minimum supported output latency will be used for the host API."),
                   &output_latency_minimal))
  {
    bsi->SetUIntValue("Audio", "OutputLatencyMS",
                      output_latency_minimal ? 0 : Settings::DEFAULT_AUDIO_OUTPUT_LATENCY_MS);
    SetSettingsChanged(bsi);
  }
  if (!output_latency_minimal)
  {
    DrawIntRangeSetting(
      bsi, FSUI_CSTR("Output Latency"),
      FSUI_CSTR("Determines how much latency there is between the audio being picked up by the host API, and "
                "played through speakers."),
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
    ActiveButton(
      FSUI_ICONSTR(ICON_FA_BAN,
                   FSUI_CSTR("RAIntegration is being used instead of the built-in achievements implementation.")),
      false, false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
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
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_TROPHY, "Enable Achievements"),
                    FSUI_CSTR("When enabled and logged in, DuckStation will scan for achievements on startup."),
                    "Cheevos", "Enabled", false);

  const bool enabled = bsi->GetBoolValue("Cheevos", "Enabled", false);
  const bool challenge = bsi->GetBoolValue("Cheevos", "ChallengeMode", false);

  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_USER_FRIENDS, "Rich Presence"),
    FSUI_CSTR("When enabled, rich presence information will be collected and sent to the server where supported."),
    "Cheevos", "RichPresence", true, enabled);
  if (DrawToggleSetting(
        bsi, FSUI_ICONSTR(ICON_FA_HARD_HAT, "Hardcore Mode"),
        FSUI_CSTR("\"Challenge\" mode for achievements, including leaderboard tracking. Disables save state, "
                  "cheats, and slowdown functions."),
        "Cheevos", "ChallengeMode", false, enabled))
  {
    if (System::IsValid() && bsi->GetBoolValue("Cheevos", "ChallengeMode", false))
      ShowToast(std::string(), "Hardcore mode will be enabled on next game restart.");
  }
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_LIST_OL, "Leaderboards"),
                    FSUI_CSTR("Enables tracking and submission of leaderboards in supported games."), "Cheevos",
                    "Leaderboards", true, enabled && challenge);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_INBOX, "Show Notifications"),
    FSUI_CSTR("Displays popup messages on events such as achievement unlocks and leaderboard submissions."), "Cheevos",
    "Notifications", true, enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_HEADPHONES, "Enable Sound Effects"),
    FSUI_CSTR("Plays sound effects for events such as achievement unlocks and leaderboard submissions."), "Cheevos",
    "SoundEffects", true, enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_MAGIC, "Show Challenge Indicators"),
    FSUI_CSTR("Shows icons in the lower-right corner of the screen when a challenge/primed achievement is active."),
    "Cheevos", "PrimedIndicators", true, enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_MEDAL, "Test Unofficial Achievements"),
    FSUI_CSTR("When enabled, DuckStation will list achievements from unofficial sets. These achievements are not "
              "tracked by RetroAchievements."),
    "Cheevos", "UnofficialTestMode", false, enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_STETHOSCOPE, "Test Mode"),
    FSUI_CSTR("When enabled, DuckStation will assume all achievements are locked and not send any unlock "
              "notifications to the server."),
    "Cheevos", "TestMode", false, enabled);

  MenuHeading("Account");
  if (Achievements::IsLoggedIn())
  {
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::GetStyle().Colors[ImGuiCol_Text]);
    ActiveButton(
      SmallString::FromFmt(fmt::runtime(FSUI_ICONSTR(ICON_FA_USER, "Username: {}")), Achievements::GetUsername()),
      false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    TinyString ts_string;
    ts_string.AppendFmtString(
      "{:%Y-%m-%d %H:%M:%S}",
      fmt::localtime(StringUtil::FromChars<u64>(bsi->GetStringValue("Cheevos", "LoginTimestamp", "0")).value_or(0)));
    ActiveButton(SmallString::FromFmt(fmt::runtime(FSUI_ICONSTR(ICON_FA_CLOCK, "Login token generated on {}")),
                                      ts_string.GetCharArray()),
                 false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    ImGui::PopStyleColor();

    if (MenuButton(FSUI_ICONSTR(ICON_FA_KEY, "Logout"), FSUI_CSTR("Logs out of RetroAchievements.")))
    {
      Host::RunOnCPUThread([]() { Achievements::Logout(); });
    }
  }
  else if (Achievements::IsActive())
  {
    ActiveButton(FSUI_ICONSTR(ICON_FA_USER, "Not Logged In"), false, false,
                 ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    if (MenuButton(FSUI_ICONSTR(ICON_FA_KEY, "Login"), FSUI_CSTR("Logs in to RetroAchievements.")))
      ImGui::OpenPopup("Achievements Login");

    DrawAchievementsLoginWindow();
  }
  else
  {
    ActiveButton(FSUI_ICONSTR(ICON_FA_USER, "Achievements are disabled."), false, false,
                 ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
  }

  MenuHeading("Current Game");
  if (Achievements::HasActiveGame())
  {
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::GetStyle().Colors[ImGuiCol_Text]);
    ActiveButton(
      fmt::format(fmt::runtime(FSUI_ICONSTR(ICON_FA_BOOKMARK, "Game ID: {}")), Achievements::GetGameID()).c_str(),
      false, false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    ActiveButton(
      fmt::format(fmt::runtime(FSUI_ICONSTR(ICON_FA_BOOK, "Game Title: {}")), Achievements::GetGameTitle()).c_str(),
      false, false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    ActiveButton(fmt::format(fmt::runtime(FSUI_ICONSTR(ICON_FA_TROPHY, "Achievements: {} ({} points)")),
                             Achievements::GetAchievementCount(), Achievements::GetMaximumPointsForGame())
                   .c_str(),
                 false, false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    const std::string& rich_presence_string = Achievements::GetRichPresenceString();
    if (!rich_presence_string.empty())
    {
      ActiveButton(SmallString::FromFmt(ICON_FA_MAP "{}", rich_presence_string), false, false,
                   LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    }
    else
    {
      ActiveButton(FSUI_ICONSTR(ICON_FA_MAP, "Rich presence inactive or unsupported."), false, false,
                   LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    }

    ImGui::PopStyleColor();
  }
  else
  {
    ActiveButton(FSUI_ICONSTR(ICON_FA_BAN, "Game not loaded or no RetroAchievements available."), false, false,
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

    ImGui::TextWrapped(FSUI_CSTR("Please enter your user name and password for retroachievements.org."));
    ImGui::NewLine();
    ImGui::TextWrapped(
      FSUI_CSTR("Your password will not be saved in DuckStation, an access token will be generated and used instead."));

    ImGui::NewLine();

    static char username[256] = {};
    static char password[256] = {};

    ImGui::Text(FSUI_CSTR("User Name: "));
    ImGui::SameLine(LayoutScale(200.0f));
    ImGui::InputText("##username", username, sizeof(username));

    ImGui::Text(FSUI_CSTR("Password: "));
    ImGui::SameLine(LayoutScale(200.0f));
    ImGui::InputText("##password", password, sizeof(password), ImGuiInputTextFlags_Password);

    ImGui::NewLine();

    BeginMenuButtons();

    const bool login_enabled = (std::strlen(username) > 0 && std::strlen(password) > 0);

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_KEY, "Login"), false, login_enabled))
    {
      Achievements::LoginAsync(username, password);
      std::memset(username, 0, sizeof(username));
      std::memset(password, 0, sizeof(password));
      ImGui::CloseCurrentPopup();
    }

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_TIMES, "Cancel"), false))
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
  ActiveButton(FSUI_ICONSTR(ICON_FA_BAN, "This build was not compiled with RetroAchivements support."), false, false,
               ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
  EndMenuButtons();
}

void FullscreenUI::DrawAchievementsLoginWindow()
{
}

#endif

void FullscreenUI::DrawAdvancedSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Logging Settings"));
  DrawEnumSetting(bsi, FSUI_CSTR("Log Level"),
                  FSUI_CSTR("Sets the verbosity of messages logged. Higher levels will log more messages."), "Logging",
                  "LogLevel", Settings::DEFAULT_LOG_LEVEL, &Settings::ParseLogLevelName, &Settings::GetLogLevelName,
                  &Settings::GetLogLevelDisplayName, LOGLEVEL_COUNT);
  DrawToggleSetting(bsi, FSUI_CSTR("Log To System Console"), FSUI_CSTR("Logs messages to the console window."),
                    FSUI_CSTR("Logging"), "LogToConsole", Settings::DEFAULT_LOG_TO_CONSOLE);
  DrawToggleSetting(bsi, FSUI_CSTR("Log To Debug Console"),
                    FSUI_CSTR("Logs messages to the debug console where supported."), "Logging", "LogToDebug", false);
  DrawToggleSetting(bsi, FSUI_CSTR("Log To File"), FSUI_CSTR("Logs messages to duckstation.log in the user directory."),
                    "Logging", "LogToFile", false);

  MenuHeading(FSUI_CSTR("Debugging Settings"));

  DrawToggleSetting(bsi, FSUI_CSTR("Disable All Enhancements"),
                    FSUI_CSTR("Temporarily disables all enhancements, useful when testing."), "Main",
                    "DisableAllEnhancements", false);

  DrawToggleSetting(bsi, FSUI_CSTR("Use Debug GPU Device"),
                    FSUI_CSTR("Enable debugging when supported by the host's renderer API. Only for developer use."),
                    "GPU", "UseDebugDevice", false);

#ifdef _WIN32
  DrawToggleSetting(bsi, FSUI_CSTR("Increase Timer Resolution"),
                    FSUI_CSTR("Enables more precise frame pacing at the cost of battery life."), "Main",
                    "IncreaseTimerResolution", true);
#endif

  DrawToggleSetting(bsi, FSUI_CSTR("Allow Booting Without SBI File"),
                    FSUI_CSTR("Allows loading protected games without subchannel information."), "CDROM",
                    "AllowBootingWithoutSBIFile", false);

  DrawToggleSetting(bsi, FSUI_CSTR("Create Save State Backups"),
                    FSUI_CSTR("Renames existing save states when saving to a backup file."), "Main",
                    "CreateSaveStateBackups", false);

  MenuHeading(FSUI_CSTR("Display Settings"));
  DrawToggleSetting(bsi, FSUI_CSTR("Show Status Indicators"),
                    FSUI_CSTR("Shows persistent icons when turbo is active or when paused."), "Display",
                    "ShowStatusIndicators", true);
  DrawToggleSetting(bsi, FSUI_CSTR("Show Enhancement Settings"),
                    FSUI_CSTR("Shows enhancement settings in the bottom-right corner of the screen."), "Display",
                    "ShowEnhancements", false);
  DrawFloatRangeSetting(
    bsi, FSUI_CSTR("Display FPS Limit"),
    FSUI_CSTR("Limits how many frames are displayed to the screen. These frames are still rendered."), "Display",
    "MaxFPS", Settings::DEFAULT_DISPLAY_MAX_FPS, 0.0f, 500.0f, "%.2f FPS");
  DrawToggleSetting(
    bsi, FSUI_CSTR("Stretch Display Vertically"),
    FSUI_CSTR("Stretches the display to match the aspect ratio by multiplying vertically instead of horizontally."),
    "Display", "StretchVertically", false);

  MenuHeading(FSUI_CSTR("PGXP Settings"));

  const bool pgxp_enabled = GetEffectiveBoolSetting(bsi, "GPU", "PGXPEnable", false);

  DrawToggleSetting(bsi, FSUI_CSTR("Enable PGXP Vertex Cache"),
                    FSUI_CSTR("Uses screen positions to resolve PGXP data. May improve visuals in some games."), "GPU",
                    "PGXPVertexCache", pgxp_enabled);
  DrawFloatRangeSetting(
    bsi, FSUI_CSTR("PGXP Geometry Tolerance"),
    FSUI_CSTR("Sets a threshold for discarding precise values when exceeded. May help with glitches in some games."),
    "GPU", "PGXPTolerance", -1.0f, -1.0f, 10.0f, "%.1f", pgxp_enabled);
  DrawFloatRangeSetting(bsi, FSUI_CSTR("PGXP Depth Clear Threshold"),
                        FSUI_CSTR("Sets a threshold for discarding the emulated depth buffer. May help in some games."),
                        "GPU", "PGXPDepthBuffer", Settings::DEFAULT_GPU_PGXP_DEPTH_THRESHOLD, 0.0f, 4096.0f, "%.1f",
                        pgxp_enabled);

  MenuHeading(FSUI_CSTR("Texture Dumping"));

  DrawToggleSetting(bsi, FSUI_CSTR("Dump Replaceable VRAM Writes"),
                    FSUI_CSTR("Writes textures which can be replaced to the dump directory."), "TextureReplacements",
                    "DumpVRAMWrites", false);
  DrawToggleSetting(bsi, FSUI_CSTR("Set VRAM Write Dump Alpha Channel"),
                    FSUI_CSTR("Clears the mask/transparency bit in VRAM write dumps."), "TextureReplacements",
                    "DumpVRAMWriteForceAlphaChannel", true);

  MenuHeading(FSUI_CSTR("CPU Emulation"));

  DrawToggleSetting(
    bsi, FSUI_CSTR("Enable Recompiler ICache"),
    FSUI_CSTR("Simulates the CPU's instruction cache in the recompiler. Can help with games running too fast."), "CPU",
    "RecompilerICache", false);
  DrawToggleSetting(bsi, FSUI_CSTR("Enable Recompiler Memory Exceptions"),
                    FSUI_CSTR("Enables alignment and bus exceptions. Not needed for any known games."), "CPU",
                    "RecompilerMemoryExceptions", false);
  DrawToggleSetting(
    bsi, FSUI_CSTR("Enable Recompiler Block Linking"),
    FSUI_CSTR("Performance enhancement - jumps directly between blocks instead of returning to the dispatcher."), "CPU",
    "RecompilerBlockLinking", true);
  DrawEnumSetting(bsi, FSUI_CSTR("Recompiler Fast Memory Access"),
                  FSUI_CSTR("Avoids calls to C++ code, significantly speeding up the recompiler."), "CPU",
                  "FastmemMode", Settings::DEFAULT_CPU_FASTMEM_MODE, &Settings::ParseCPUFastmemMode,
                  &Settings::GetCPUFastmemModeName, &Settings::GetCPUFastmemModeDisplayName, CPUFastmemMode::Count);

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
    const std::string& title = System::GetGameTitle();
    const std::string& serial = System::GetGameSerial();

    if (!serial.empty())
      buffer.Format("%s - ", serial.c_str());
    buffer.AppendString(Path::GetFileName(System::GetDiscPath()));

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

    const std::string& serial = System::GetGameSerial();
    if (!serial.empty())
    {
      const std::time_t cached_played_time = GameList::GetCachedPlayedTimeForSerial(serial);
      const std::time_t session_time = static_cast<std::time_t>(CommonHost::GetSessionPlayedTime());

      buffer.Fmt(FSUI_FSTR("Session: {}"), GameList::FormatTimespan(session_time, true).GetStringView());
      const ImVec2 session_size(g_medium_font->CalcTextSizeA(g_medium_font->FontSize, std::numeric_limits<float>::max(),
                                                             -1.0f, buffer.GetCharArray(),
                                                             buffer.GetCharArray() + buffer.GetLength()));
      const ImVec2 session_pos(display_size.x - LayoutScale(10.0f) - session_size.x,
                               time_pos.y + g_large_font->FontSize + LayoutScale(4.0f));
      DrawShadowedText(dl, g_medium_font, session_pos, IM_COL32(255, 255, 255, 255), buffer.GetCharArray(),
                       buffer.GetCharArray() + buffer.GetLength());

      buffer.Fmt(FSUI_FSTR("All Time: {}"),
                 GameList::FormatTimespan(cached_played_time + session_time, true).GetStringView());
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
        const bool has_game = System::IsValid() && !System::GetGameSerial().empty();

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_PLAY, "Resume Game"), false) || WantsToCloseMenu())
          ClosePauseMenu();

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_FAST_FORWARD, "Toggle Fast Forward"), false))
        {
          ClosePauseMenu();
          DoToggleFastForward();
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_UNDO, "Load State"), false, has_game))
        {
          if (OpenSaveStateSelector(true))
            s_current_main_window = MainWindowType::None;
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_DOWNLOAD, "Save State"), false, has_game))
        {
          if (OpenSaveStateSelector(false))
            s_current_main_window = MainWindowType::None;
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_FROWN_OPEN, "Cheat List"), false,
                         !System::GetGameSerial().empty() && !Achievements::ChallengeModeActive()))
        {
          s_current_main_window = MainWindowType::None;
          DoCheatsMenu();
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_GAMEPAD, "Toggle Analog"), false))
        {
          ClosePauseMenu();
          DoToggleAnalogMode();
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_WRENCH, "Game Properties"), false, has_game))
        {
          SwitchToGameSettings();
        }

#ifdef WITH_CHEEVOS
        if (ActiveButton(FSUI_ICONSTR(ICON_FA_TROPHY, "Achievements"), false,
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
        ActiveButton(FSUI_ICONSTR(ICON_FA_TROPHY, "Achievements"), false, false);
#endif

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_CAMERA, "Save Screenshot"), false))
        {
          System::SaveScreenshot();
          ClosePauseMenu();
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Change Disc"), false))
        {
          s_current_main_window = MainWindowType::None;
          DoChangeDisc();
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_SLIDERS_H, "Settings"), false))
          SwitchToSettings();

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_POWER_OFF, "Close Game"), false))
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
          ImGui::SetFocusID(ImGui::GetID(FSUI_ICONSTR(ICON_FA_POWER_OFF, "Exit Without Saving")),
                            ImGui::GetCurrentWindow());

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_BACKWARD, "Back To Pause Menu"), false))
        {
          OpenPauseSubMenu(PauseSubMenu::None);
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_SYNC, "Reset System"), false))
        {
          ClosePauseMenu();
          DoReset();
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_SAVE, "Exit And Save State"), false))
          DoShutdown(true);

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_POWER_OFF, "Exit Without Saving"), false))
          DoShutdown(false);
      }
      break;

#ifdef WITH_CHEEVOS
      case PauseSubMenu::Achievements:
      {
        if (ActiveButton(FSUI_ICONSTR(ICON_FA_BACKWARD, "Back To Pause Menu"), false))
          OpenPauseSubMenu(PauseSubMenu::None);

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_TROPHY, "Achievements"), false))
          OpenAchievementsWindow();

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_STOPWATCH, "Leaderboards"), false))
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
  li->title = (global || slot > 0) ? fmt::format(global ? FSUI_FSTR("Global Slot {0}##global_slot_{0}") :
                                                          FSUI_FSTR("Game Slot {0}##game_slot_{0}"),
                                                 slot) :
                                     FSUI_STR("Quick Save");
  li->summary = FSUI_STR("No save present in this slot.");
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
    li->title = fmt::format(FSUI_FSTR("Global Slot {0} - {1}##global_slot_{0}"), slot, ssi->serial);
  }
  else
  {
    li->title = (slot > 0) ? fmt::format(FSUI_FSTR("Game Slot {0}##game_slot_{0}"), slot) : FSUI_STR("Game Quick Save");
  }

  li->summary = fmt::format(FSUI_FSTR("Saved {:%c}"), fmt::localtime(ssi->timestamp));
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
    li->preview_texture = g_gpu_device->CreateTexture(
      ssi->screenshot_width, ssi->screenshot_height, 1, 1, 1, GPUTexture::Type::Texture, GPUTexture::Format::RGBA8,
      ssi->screenshot_data.data(), sizeof(u32) * ssi->screenshot_width, false);
  }
  else
  {
    li->preview_texture = g_gpu_device->CreateTexture(
      Resources::PLACEHOLDER_ICON_WIDTH, Resources::PLACEHOLDER_ICON_HEIGHT, 1, 1, 1, GPUTexture::Type::Texture,
      GPUTexture::Format::RGBA8, Resources::PLACEHOLDER_ICON_DATA, sizeof(u32) * Resources::PLACEHOLDER_ICON_WIDTH,
      false);
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
      li.title = FSUI_STR("Undo Load State");
      li.summary = FSUI_STR("Restores the state of the system prior to the last state loaded.");
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

  ShowToast({}, FSUI_STR("No save states found."), 5.0f);
  return false;
}

bool FullscreenUI::OpenSaveStateSelector(bool is_loading)
{
  s_save_state_selector_game_path = {};
  s_save_state_selector_loading = is_loading;
  s_save_state_selector_resuming = false;
  if (PopulateSaveStateListEntries(System::GetGameTitle().c_str(), System::GetGameSerial().c_str()) > 0)
  {
    s_save_state_selector_open = true;
    return true;
  }

  ShowToast({}, FSUI_STR("No save states found."), 5.0f);
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

  const char* window_title = is_loading ? FSUI_CSTR("Load State") : FSUI_CSTR("Save State");
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

  if (ImGui::BeginChild("state_titlebar", heading_size, false, ImGuiWindowFlags_NavFlattened))
  {
    BeginNavBar();
    if (NavButton(ICON_FA_BACKWARD, true, true))
      CloseSaveStateSelector();

    NavTitle(is_loading ? FSUI_CSTR("Load State") : FSUI_CSTR("Save State"));
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

            if (ActiveButton(is_loading ? FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Load State") :
                                          FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Save State"),
                             false, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
            {
              if (is_loading)
                DoLoadState(std::move(entry.path));
              else
                DoSaveState(entry.slot, entry.global);

              CloseSaveStateSelector();
              closed = true;
            }

            if (ActiveButton(FSUI_ICONSTR(ICON_FA_FOLDER_MINUS, "Delete Save"), false, true,
                             LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
            {
              if (!FileSystem::FileExists(entry.path.c_str()))
              {
                ShowToast({}, fmt::format(FSUI_FSTR("{} does not exist."), ImGuiFullscreen::RemoveHash(entry.title)));
                is_open = true;
              }
              else if (FileSystem::DeleteFile(entry.path.c_str()))
              {
                ShowToast({}, fmt::format(FSUI_FSTR("{} deleted."), ImGuiFullscreen::RemoveHash(entry.title)));
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
                ShowToast({}, fmt::format(FSUI_FSTR("Failed to delete {}."), ImGuiFullscreen::RemoveHash(entry.title)));
                is_open = false;
              }
            }

            if (ActiveButton(FSUI_ICONSTR(ICON_FA_WINDOW_CLOSE, "Close Menu"), false, true,
                             LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
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
  ImGui::OpenPopup(FSUI_CSTR("Load Resume State"));

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  bool is_open = true;
  if (ImGui::BeginPopupModal(FSUI_CSTR("Load Resume State"), &is_open,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    const SaveStateListEntry& entry = s_save_state_selector_slots.front();
    ImGui::TextWrapped(
      FSUI_CSTR("A resume save state created at %s was found.\n\nDo you want to load this save and continue?"),
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

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_PLAY, "Load State"), false))
    {
      DoStartPath(s_save_state_selector_game_path, std::move(entry.path));
      is_open = false;
    }

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_LIGHTBULB, "Clean Boot"), false))
    {
      DoStartPath(s_save_state_selector_game_path);
      is_open = false;
    }

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_FOLDER_MINUS, "Delete State"), false))
    {
      if (FileSystem::DeleteFile(entry.path.c_str()))
      {
        DoStartPath(s_save_state_selector_game_path);
        is_open = false;
      }
      else
      {
        ShowToast(std::string(), FSUI_STR("Failed to delete save state."));
      }
    }

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_WINDOW_CLOSE, "Cancel"), false))
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
                                  System::GetGameSaveStateFileName(System::GetGameSerial(), slot));
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
    static constexpr const char* titles[] = {FSUI_NSTR("Game Grid"), FSUI_NSTR("Game List"),
                                             FSUI_NSTR("Game List Settings")};
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

    NavTitle(Host::TranslateToCString(TR_CONTEXT, titles[static_cast<u32>(s_game_list_page)]));
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
        ImGui::TextUnformatted(FSUI_CSTR("Region: "));
        ImGui::SameLine();
        ImGui::Image(GetCachedTextureAsync(flag_texture.c_str()), LayoutScale(23.0f, 16.0f));
        ImGui::SameLine();
        ImGui::Text(" (%s)", Settings::GetDiscRegionDisplayName(selected_entry->region));
      }

      // genre
      ImGui::Text(FSUI_CSTR("Genre: %s"), selected_entry->genre.c_str());

      // release date
      char release_date_str[64];
      selected_entry->GetReleaseDateString(release_date_str, sizeof(release_date_str));
      ImGui::Text(FSUI_CSTR("Release Date: %s"), release_date_str);

      // compatibility
      ImGui::TextUnformatted(FSUI_CSTR("Compatibility: "));
      ImGui::SameLine();
      if (selected_entry->compatibility != GameDatabase::CompatibilityRating::Unknown)
      {
        ImGui::Image(s_game_compatibility_textures[static_cast<u32>(selected_entry->compatibility)].get(),
                     LayoutScale(64.0f, 16.0f));
        ImGui::SameLine();
      }
      ImGui::Text(" (%s)", GameDatabase::GetCompatibilityRatingDisplayName(selected_entry->compatibility));

      // play time
      ImGui::Text(FSUI_CSTR("Time Played: %s"),
                  GameList::FormatTimespan(selected_entry->total_played_time).GetCharArray());
      ImGui::Text(FSUI_CSTR("Last Played: %s"),
                  GameList::FormatTimestamp(selected_entry->last_played_time).GetCharArray());

      // size
      ImGui::Text(FSUI_CSTR("Size: %.2f MB"), static_cast<float>(selected_entry->total_size) / 1048576.0f);

      ImGui::PopFont();
    }
    else
    {
      // title
      const char* title = FSUI_CSTR("No Game Selected");
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
    {FSUI_ICONSTR(ICON_FA_WRENCH, "Game Properties"), false},
    {FSUI_ICONSTR(ICON_FA_PLAY, "Resume Game"), false},
    {FSUI_ICONSTR(ICON_FA_UNDO, "Load State"), false},
    {FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Default Boot"), false},
    {FSUI_ICONSTR(ICON_FA_LIGHTBULB, "Fast Boot"), false},
    {FSUI_ICONSTR(ICON_FA_MAGIC, "Slow Boot"), false},
    {FSUI_ICONSTR(ICON_FA_FOLDER_MINUS, "Reset Play Time"), false},
    {FSUI_ICONSTR(ICON_FA_WINDOW_CLOSE, "Close Menu"), false},
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
        case 6: // Reset Play Time
          GameList::ClearPlayedTimeForSerial(entry_serial);
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

  MenuHeading(FSUI_CSTR("Search Directories"));
  if (MenuButton(FSUI_ICONSTR(ICON_FA_FOLDER_PLUS, "Add Search Directory"),
                 FSUI_CSTR("Adds a new directory to the game search list.")))
  {
    OpenFileSelector(FSUI_ICONSTR(ICON_FA_FOLDER_PLUS, "Add Search Directory"), true, [](const std::string& dir) {
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
    if (MenuButton(SmallString::FromFmt(ICON_FA_FOLDER "{}", it.first),
                   it.second ? FSUI_CSTR("Scanning Subdirectories") : FSUI_CSTR("Not Scanning Subdirectories")))
    {
      ImGuiFullscreen::ChoiceDialogOptions options = {
        {FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Open in File Browser"), false},
        {it.second ? (FSUI_ICONSTR(ICON_FA_FOLDER_MINUS, "Disable Subdirectory Scanning")) :
                     (FSUI_ICONSTR(ICON_FA_FOLDER_PLUS, "Enable Subdirectory Scanning")),
         false},
        {FSUI_ICONSTR(ICON_FA_TIMES, "Remove From List"), false},
        {FSUI_ICONSTR(ICON_FA_WINDOW_CLOSE, "Close Menu"), false},
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
    static constexpr const char* view_types[] = {FSUI_NSTR("Game Grid"), FSUI_NSTR("Game List")};
    static constexpr const char* sort_types[] = {
      FSUI_NSTR("Type"),        FSUI_NSTR("Serial"),      FSUI_NSTR("Title"), FSUI_NSTR("File Title"),
      FSUI_NSTR("Time Played"), FSUI_NSTR("Last Played"), FSUI_NSTR("Size")};

    DrawIntListSetting(bsi, FSUI_ICONSTR(ICON_FA_BORDER_ALL, "Default View"),
                       "Sets which view the game list will open to.", "Main", "DefaultFullscreenUIGameView", 0,
                       view_types, std::size(view_types));
    DrawIntListSetting(bsi, FSUI_ICONSTR(ICON_FA_SORT, "Sort By"),
                       "Determines which field the game list will be sorted by.", "Main", "FullscreenUIGameSort", 0,
                       sort_types, std::size(sort_types));
    DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_SORT_ALPHA_DOWN, "Sort Reversed"),
                      "Reverses the game list sort order from the default (usually ascending to descending).", "Main",
                      "FullscreenUIGameSortReverse", false);
  }

  MenuHeading(FSUI_CSTR("Cover Settings"));
  {
    DrawFolderSetting(bsi, FSUI_ICONSTR(ICON_FA_FOLDER, "Covers Directory"), "Folders", "Covers", EmuFolders::Covers);
    if (MenuButton(FSUI_ICONSTR(ICON_FA_DOWNLOAD, "Download Covers"),
                   FSUI_CSTR("Downloads covers from a user-specified URL template.")))
      ImGui::OpenPopup("Download Covers");
  }

  MenuHeading("Operations");
  {
    if (MenuButton(FSUI_ICONSTR(ICON_FA_SEARCH, "Scan For New Games"),
                   FSUI_CSTR("Identifies any new files added to the game directories.")))
      Host::RefreshGameListAsync(false);
    if (MenuButton(FSUI_ICONSTR(ICON_FA_SEARCH_PLUS, "Rescan All Games"),
                   FSUI_CSTR("Forces a full rescan of all games previously identified.")))
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
    ImGui::TextWrapped(
      FSUI_CSTR("DuckStation can automatically download covers for games which do not currently have a cover set. We "
                "do not host any cover images, the user must provide their own source for images."));
    ImGui::NewLine();
    ImGui::TextWrapped(FSUI_CSTR("In the form below, specify the URLs to download covers from, with one template URL "
                                 "per line. The following variables are available:"));
    ImGui::NewLine();
    ImGui::TextWrapped(FSUI_CSTR("${title}: Title of the game.\n${filetitle}: Name component of the game's "
                                 "filename.\n${serial}: Serial of the game."));
    ImGui::NewLine();
    ImGui::TextWrapped(FSUI_CSTR("Example: https://www.example-not-a-real-domain.com/covers/${serial}.jpg"));
    ImGui::NewLine();

    BeginMenuButtons();

    static char template_urls[512];
    ImGui::InputTextMultiline("##templates", template_urls, sizeof(template_urls),
                              ImVec2(ImGui::GetCurrentWindow()->WorkRect.GetWidth(), LayoutScale(175.0f)));

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(5.0f));

    static bool use_serial_names;
    ImGui::PushFont(g_medium_font);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(2.0f, 2.0f));
    ImGui::Checkbox(FSUI_CSTR("Use Serial File Names"), &use_serial_names);
    ImGui::PopStyleVar(1);
    ImGui::PopFont();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));

    const bool download_enabled = (std::strlen(template_urls) > 0);

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_DOWNLOAD, "Start Download"), false, download_enabled))
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

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_TIMES, "Cancel"), false))
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

  const GameList::Entry* entry = GameList::GetEntryForPath(System::GetDiscPath().c_str());
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
    ShowToast(std::string(), FSUI_STR("Failed to copy text to clipboard."));
}

void FullscreenUI::DrawAboutWindow()
{
  ImGui::SetNextWindowSize(LayoutScale(1000.0f, 510.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup(FSUI_CSTR("About DuckStation"));

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(10.0f, 10.0f));

  if (ImGui::BeginPopupModal(FSUI_CSTR("About DuckStation"), &s_about_window_open,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    ImGui::TextWrapped(FSUI_CSTR("DuckStation is a free and open-source simulator/emulator of the Sony PlayStation(TM) "
                                 "console, focusing on playability, speed, and long-term maintainability."));
    ImGui::NewLine();
    ImGui::TextWrapped(
      FSUI_CSTR("Contributor List: https://github.com/stenzek/duckstation/blob/master/CONTRIBUTORS.md"));
    ImGui::NewLine();
    ImGui::TextWrapped(
      FSUI_CSTR("Duck icon by icons8 (https://icons8.com/icon/74847/platforms.undefined.short-title)"));
    ImGui::NewLine();
    ImGui::TextWrapped(
      FSUI_CSTR("\"PlayStation\" and \"PSX\" are registered trademarks of Sony Interactive Entertainment Europe "
                "Limited. This software is not affiliated in any way with Sony Interactive Entertainment."));

    ImGui::NewLine();

    BeginMenuButtons();
    if (ActiveButton(FSUI_ICONSTR(ICON_FA_GLOBE, "GitHub Repository"), false))
      ExitFullscreenAndOpenURL("https://github.com/stenzek/duckstation/");
    if (ActiveButton(FSUI_ICONSTR(ICON_FA_BUG, "Issue Tracker"), false))
      ExitFullscreenAndOpenURL("https://github.com/stenzek/duckstation/issues");
    if (ActiveButton(FSUI_ICONSTR(ICON_FA_COMMENT, "Discord Server"), false))
      ExitFullscreenAndOpenURL("https://discord.gg/Buktv3t");

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_WINDOW_CLOSE, "Close"), false))
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

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_WINDOW_CLOSE, "Close"), false))
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

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_CHECK, "Yes"), false))
    {
      *result = true;
      done = true;
    }

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_TIMES, "No"), false))
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
  const auto points_text =
    TinyString::FromFmt((cheevo.points != 1) ? FSUI_FSTR("{} point") : FSUI_FSTR("{} points"), cheevo.points);
  const ImVec2 points_template_size(
    g_medium_font->CalcTextSizeA(g_medium_font->FontSize, FLT_MAX, 0.0f, FSUI_CSTR("XXX points")));
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
        text += FSUI_VSTR(" (Hardcore Mode)");

      top += g_large_font->FontSize + spacing;

      ImGui::PushFont(g_large_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, text.c_str(), text.c_str() + text.length(), nullptr,
                               ImVec2(0.0f, 0.0f), &title_bb);
      ImGui::PopFont();

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
      if (unlocked_count == achievement_count)
      {
        text = fmt::format(FSUI_FSTR("You have unlocked all achievements and earned {} points!"), total_points);
      }
      else
      {
        text = fmt::format(FSUI_FSTR("You have unlocked {} of {} achievements, earning {} of {} possible points."),
                           unlocked_count, achievement_count, current_points, total_points);
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
      FSUI_CSTR("Unlocked Achievements"), unlocked_achievements_collapsed ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_UP);
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
        FSUI_CSTR("Locked Achievements"), locked_achievements_collapsed ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_UP);
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
              FSUI_CSTR("Active Challenge Achievements"));

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
        text.Fmt(TRANSLATE_FS("Achievements", "This game has {} leaderboards."), leaderboard_count);
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
          TRANSLATE("Achievements",
                    "Submitting scores is disabled because hardcore mode is off. Leaderboards are read-only."),
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
                                   TRANSLATE("Achievements", "Time") :
                                   TRANSLATE("Achievements", "Score"),
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
                                 TRANSLATE("Achievements", "Downloading leaderboard data, please wait..."), nullptr,
                                 nullptr, ImVec2(0.5f, 0.5f));

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

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Translation String Area
// To avoid having to type T_RANSLATE("FullscreenUI", ...) everywhere, we use the shorter macros at the top
// of the file, then preprocess and generate a bunch of noops here to define the strings. Sadly that means
// the view in Linguist is gonna suck, but you can search the file for the string for more context.
/////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0
// TRANSLATION-STRING-AREA-BEGIN
TRANSLATE_NOOP("FullscreenUI", "${title}: Title of the game.\n${filetitle}: Name component of the game's filename.\n${serial}: Serial of the game.");
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
TRANSLATE_NOOP("FullscreenUI", "A memory card with the name '{}' already exists.");
TRANSLATE_NOOP("FullscreenUI", "A resume save state created at %s was found.\n\nDo you want to load this save and continue?");
TRANSLATE_NOOP("FullscreenUI", "About DuckStation");
TRANSLATE_NOOP("FullscreenUI", "Achievements");
TRANSLATE_NOOP("FullscreenUI", "Achievements Settings");
TRANSLATE_NOOP("FullscreenUI", "Achievements are disabled.");
TRANSLATE_NOOP("FullscreenUI", "Achievements: {} ({} points)");
TRANSLATE_NOOP("FullscreenUI", "Active Challenge Achievements");
TRANSLATE_NOOP("FullscreenUI", "Add Search Directory");
TRANSLATE_NOOP("FullscreenUI", "Add Shader");
TRANSLATE_NOOP("FullscreenUI", "Adds a new directory to the game search list.");
TRANSLATE_NOOP("FullscreenUI", "Adds a new shader to the chain.");
TRANSLATE_NOOP("FullscreenUI", "Adds additional precision to PGXP data post-projection. May improve visuals in some games.");
TRANSLATE_NOOP("FullscreenUI", "Adds padding to ensure pixels are a whole number in size.");
TRANSLATE_NOOP("FullscreenUI", "Adjusts the emulation speed so the console's refresh rate matches the host when VSync and Audio Resampling are enabled.");
TRANSLATE_NOOP("FullscreenUI", "Advanced Settings");
TRANSLATE_NOOP("FullscreenUI", "All Time: {}");
TRANSLATE_NOOP("FullscreenUI", "Allow Booting Without SBI File");
TRANSLATE_NOOP("FullscreenUI", "Allows loading protected games without subchannel information.");
TRANSLATE_NOOP("FullscreenUI", "Apply Image Patches");
TRANSLATE_NOOP("FullscreenUI", "Apply Per-Game Settings");
TRANSLATE_NOOP("FullscreenUI", "Are you sure you want to clear the current post-processing chain? All configuration will be lost.");
TRANSLATE_NOOP("FullscreenUI", "Aspect Ratio");
TRANSLATE_NOOP("FullscreenUI", "Attempts to map the selected port to a chosen controller.");
TRANSLATE_NOOP("FullscreenUI", "Audio Backend");
TRANSLATE_NOOP("FullscreenUI", "Audio Settings");
TRANSLATE_NOOP("FullscreenUI", "Auto-Detect");
TRANSLATE_NOOP("FullscreenUI", "Automatic Mapping");
TRANSLATE_NOOP("FullscreenUI", "Automatic based on window size");
TRANSLATE_NOOP("FullscreenUI", "Automatic mapping completed for {}.");
TRANSLATE_NOOP("FullscreenUI", "Automatic mapping failed for {}.");
TRANSLATE_NOOP("FullscreenUI", "Automatic mapping failed, no devices are available.");
TRANSLATE_NOOP("FullscreenUI", "Automatically Load Cheats");
TRANSLATE_NOOP("FullscreenUI", "Automatically applies patches to disc images when they are present, currently only PPF is supported.");
TRANSLATE_NOOP("FullscreenUI", "Automatically loads and applies cheats on game start.");
TRANSLATE_NOOP("FullscreenUI", "Automatically saves the emulator state when powering down or exiting. You can then resume directly from where you left off next time.");
TRANSLATE_NOOP("FullscreenUI", "Automatically switches to fullscreen mode when the program is started.");
TRANSLATE_NOOP("FullscreenUI", "Avoids calls to C++ code, significantly speeding up the recompiler.");
TRANSLATE_NOOP("FullscreenUI", "BIOS Directory");
TRANSLATE_NOOP("FullscreenUI", "BIOS Selection");
TRANSLATE_NOOP("FullscreenUI", "BIOS Settings");
TRANSLATE_NOOP("FullscreenUI", "BIOS for {}");
TRANSLATE_NOOP("FullscreenUI", "BIOS to use when emulating {} consoles.");
TRANSLATE_NOOP("FullscreenUI", "Back To Pause Menu");
TRANSLATE_NOOP("FullscreenUI", "Backend Settings");
TRANSLATE_NOOP("FullscreenUI", "Behavior");
TRANSLATE_NOOP("FullscreenUI", "Borderless Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Buffer Size");
TRANSLATE_NOOP("FullscreenUI", "CD-ROM Emulation");
TRANSLATE_NOOP("FullscreenUI", "CPU Emulation");
TRANSLATE_NOOP("FullscreenUI", "CPU Mode");
TRANSLATE_NOOP("FullscreenUI", "Cancel");
TRANSLATE_NOOP("FullscreenUI", "Change Disc");
TRANSLATE_NOOP("FullscreenUI", "Change settings for the emulator.");
TRANSLATE_NOOP("FullscreenUI", "Changes the aspect ratio used to display the console's output to the screen.");
TRANSLATE_NOOP("FullscreenUI", "Cheat List");
TRANSLATE_NOOP("FullscreenUI", "Chooses the backend to use for rendering the console/game visuals.");
TRANSLATE_NOOP("FullscreenUI", "Chroma Smoothing For 24-Bit Display");
TRANSLATE_NOOP("FullscreenUI", "Clean Boot");
TRANSLATE_NOOP("FullscreenUI", "Clear Settings");
TRANSLATE_NOOP("FullscreenUI", "Clear Shaders");
TRANSLATE_NOOP("FullscreenUI", "Clears a shader from the chain.");
TRANSLATE_NOOP("FullscreenUI", "Clears all settings set for this game.");
TRANSLATE_NOOP("FullscreenUI", "Clears the mask/transparency bit in VRAM write dumps.");
TRANSLATE_NOOP("FullscreenUI", "Close");
TRANSLATE_NOOP("FullscreenUI", "Close Game");
TRANSLATE_NOOP("FullscreenUI", "Close Menu");
TRANSLATE_NOOP("FullscreenUI", "Compatibility Rating");
TRANSLATE_NOOP("FullscreenUI", "Compatibility: ");
TRANSLATE_NOOP("FullscreenUI", "Confirm Power Off");
TRANSLATE_NOOP("FullscreenUI", "Console Settings");
TRANSLATE_NOOP("FullscreenUI", "Contributor List: https://github.com/stenzek/duckstation/blob/master/CONTRIBUTORS.md");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {}");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {} Macros");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {} Settings");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {}{}");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {}{} Macros");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {}{} Settings");
TRANSLATE_NOOP("FullscreenUI", "Controller Settings");
TRANSLATE_NOOP("FullscreenUI", "Controller Type");
TRANSLATE_NOOP("FullscreenUI", "Controller settings reset to default.");
TRANSLATE_NOOP("FullscreenUI", "Controls");
TRANSLATE_NOOP("FullscreenUI", "Controls the volume of the audio played on the host when fast forwarding.");
TRANSLATE_NOOP("FullscreenUI", "Controls the volume of the audio played on the host.");
TRANSLATE_NOOP("FullscreenUI", "Copies the current global settings to this game.");
TRANSLATE_NOOP("FullscreenUI", "Copies the global controller configuration to this game.");
TRANSLATE_NOOP("FullscreenUI", "Copy Global Settings");
TRANSLATE_NOOP("FullscreenUI", "Copy Settings");
TRANSLATE_NOOP("FullscreenUI", "Cover Settings");
TRANSLATE_NOOP("FullscreenUI", "Covers Directory");
TRANSLATE_NOOP("FullscreenUI", "Create");
TRANSLATE_NOOP("FullscreenUI", "Create Memory Card");
TRANSLATE_NOOP("FullscreenUI", "Create Save State Backups");
TRANSLATE_NOOP("FullscreenUI", "Creates a new memory card file or folder.");
TRANSLATE_NOOP("FullscreenUI", "Crop Mode");
TRANSLATE_NOOP("FullscreenUI", "Culling Correction");
TRANSLATE_NOOP("FullscreenUI", "Debugging Settings");
TRANSLATE_NOOP("FullscreenUI", "Default");
TRANSLATE_NOOP("FullscreenUI", "Default Boot");
TRANSLATE_NOOP("FullscreenUI", "Default View");
TRANSLATE_NOOP("FullscreenUI", "Default: Disabled");
TRANSLATE_NOOP("FullscreenUI", "Default: Enabled");
TRANSLATE_NOOP("FullscreenUI", "Delete Save");
TRANSLATE_NOOP("FullscreenUI", "Delete State");
TRANSLATE_NOOP("FullscreenUI", "Depth Buffer");
TRANSLATE_NOOP("FullscreenUI", "Details");
TRANSLATE_NOOP("FullscreenUI", "Details unavailable for game not scanned in game list.");
TRANSLATE_NOOP("FullscreenUI", "Determines how large the on-screen messages and monitor are.");
TRANSLATE_NOOP("FullscreenUI", "Determines how much latency there is between the audio being picked up by the host API, and played through speakers.");
TRANSLATE_NOOP("FullscreenUI", "Determines how much of the area typically not visible on a consumer TV set to crop/hide.");
TRANSLATE_NOOP("FullscreenUI", "Determines how the emulated CPU executes instructions.");
TRANSLATE_NOOP("FullscreenUI", "Determines quality of audio when not running at 100% speed.");
TRANSLATE_NOOP("FullscreenUI", "Determines the amount of audio buffered before being pulled by the host API.");
TRANSLATE_NOOP("FullscreenUI", "Determines the emulated hardware type.");
TRANSLATE_NOOP("FullscreenUI", "Determines the position on the screen when black borders must be added.");
TRANSLATE_NOOP("FullscreenUI", "Determines whether a prompt will be displayed to confirm shutting down the emulator/game when the hotkey is pressed.");
TRANSLATE_NOOP("FullscreenUI", "Device Settings");
TRANSLATE_NOOP("FullscreenUI", "Disable All Enhancements");
TRANSLATE_NOOP("FullscreenUI", "Disable Interlacing");
TRANSLATE_NOOP("FullscreenUI", "Disable Subdirectory Scanning");
TRANSLATE_NOOP("FullscreenUI", "Disabled");
TRANSLATE_NOOP("FullscreenUI", "Disables dithering and uses the full 8 bits per channel of color information.");
TRANSLATE_NOOP("FullscreenUI", "Disables interlaced rendering and display in the GPU. Some games can render in 480p this way, but others will break.");
TRANSLATE_NOOP("FullscreenUI", "Discord Server");
TRANSLATE_NOOP("FullscreenUI", "Display FPS Limit");
TRANSLATE_NOOP("FullscreenUI", "Display Settings");
TRANSLATE_NOOP("FullscreenUI", "Displays popup messages on events such as achievement unlocks and leaderboard submissions.");
TRANSLATE_NOOP("FullscreenUI", "Double-Click Toggles Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Download Covers");
TRANSLATE_NOOP("FullscreenUI", "Downloads covers from a user-specified URL template.");
TRANSLATE_NOOP("FullscreenUI", "Downsamples the rendered image prior to displaying it. Can improve overall image quality in mixed 2D/3D games.");
TRANSLATE_NOOP("FullscreenUI", "Downsampling");
TRANSLATE_NOOP("FullscreenUI", "Duck icon by icons8 (https://icons8.com/icon/74847/platforms.undefined.short-title)");
TRANSLATE_NOOP("FullscreenUI", "DuckStation can automatically download covers for games which do not currently have a cover set. We do not host any cover images, the user must provide their own source for images.");
TRANSLATE_NOOP("FullscreenUI", "DuckStation is a free and open-source simulator/emulator of the Sony PlayStation(TM) console, focusing on playability, speed, and long-term maintainability.");
TRANSLATE_NOOP("FullscreenUI", "Dump Replaceable VRAM Writes");
TRANSLATE_NOOP("FullscreenUI", "Emulation Settings");
TRANSLATE_NOOP("FullscreenUI", "Emulation Speed");
TRANSLATE_NOOP("FullscreenUI", "Enable 8MB RAM");
TRANSLATE_NOOP("FullscreenUI", "Enable Achievements");
TRANSLATE_NOOP("FullscreenUI", "Enable Discord Presence");
TRANSLATE_NOOP("FullscreenUI", "Enable Fast Boot");
TRANSLATE_NOOP("FullscreenUI", "Enable Overclocking");
TRANSLATE_NOOP("FullscreenUI", "Enable PGXP Vertex Cache");
TRANSLATE_NOOP("FullscreenUI", "Enable Post Processing");
TRANSLATE_NOOP("FullscreenUI", "Enable Recompiler Block Linking");
TRANSLATE_NOOP("FullscreenUI", "Enable Recompiler ICache");
TRANSLATE_NOOP("FullscreenUI", "Enable Recompiler Memory Exceptions");
TRANSLATE_NOOP("FullscreenUI", "Enable Region Check");
TRANSLATE_NOOP("FullscreenUI", "Enable Rewinding");
TRANSLATE_NOOP("FullscreenUI", "Enable SDL Input Source");
TRANSLATE_NOOP("FullscreenUI", "Enable Sound Effects");
TRANSLATE_NOOP("FullscreenUI", "Enable Subdirectory Scanning");
TRANSLATE_NOOP("FullscreenUI", "Enable TTY Output");
TRANSLATE_NOOP("FullscreenUI", "Enable VRAM Write Texture Replacement");
TRANSLATE_NOOP("FullscreenUI", "Enable VSync");
TRANSLATE_NOOP("FullscreenUI", "Enable XInput Input Source");
TRANSLATE_NOOP("FullscreenUI", "Enable debugging when supported by the host's renderer API. Only for developer use.");
TRANSLATE_NOOP("FullscreenUI", "Enables alignment and bus exceptions. Not needed for any known games.");
TRANSLATE_NOOP("FullscreenUI", "Enables an additional 6MB of RAM to obtain a total of 2+6 = 8MB, usually present on dev consoles.");
TRANSLATE_NOOP("FullscreenUI", "Enables an additional three controller slots on each port. Not supported in all games.");
TRANSLATE_NOOP("FullscreenUI", "Enables more precise frame pacing at the cost of battery life.");
TRANSLATE_NOOP("FullscreenUI", "Enables the replacement of background textures in supported games.");
TRANSLATE_NOOP("FullscreenUI", "Enables tracking and submission of leaderboards in supported games.");
TRANSLATE_NOOP("FullscreenUI", "Enhancements");
TRANSLATE_NOOP("FullscreenUI", "Ensures every frame generated is displayed for optimal pacing. Disable if you are having speed or sound issues.");
TRANSLATE_NOOP("FullscreenUI", "Enter the name of the input profile you wish to create.");
TRANSLATE_NOOP("FullscreenUI", "Enter the name of the memory card you wish to create.");
TRANSLATE_NOOP("FullscreenUI", "Example: https://www.example-not-a-real-domain.com/covers/${serial}.jpg");
TRANSLATE_NOOP("FullscreenUI", "Execution Mode");
TRANSLATE_NOOP("FullscreenUI", "Exit");
TRANSLATE_NOOP("FullscreenUI", "Exit And Save State");
TRANSLATE_NOOP("FullscreenUI", "Exit Without Saving");
TRANSLATE_NOOP("FullscreenUI", "Exits the program.");
TRANSLATE_NOOP("FullscreenUI", "Failed to copy text to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Failed to create memory card '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Failed to delete save state.");
TRANSLATE_NOOP("FullscreenUI", "Failed to delete {}.");
TRANSLATE_NOOP("FullscreenUI", "Failed to load '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Failed to load shader {}. It may be invalid.");
TRANSLATE_NOOP("FullscreenUI", "Failed to save input profile '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Fast Boot");
TRANSLATE_NOOP("FullscreenUI", "Fast Forward Speed");
TRANSLATE_NOOP("FullscreenUI", "Fast Forward Volume");
TRANSLATE_NOOP("FullscreenUI", "File Title");
TRANSLATE_NOOP("FullscreenUI", "Fills the window with the active display area, regardless of the aspect ratio.");
TRANSLATE_NOOP("FullscreenUI", "Force 4:3 For 24-Bit Display");
TRANSLATE_NOOP("FullscreenUI", "Force NTSC Timings");
TRANSLATE_NOOP("FullscreenUI", "Forces PAL games to run at NTSC timings, i.e. 60hz. Some PAL games will run at their \"normal\" speeds, while others will break.");
TRANSLATE_NOOP("FullscreenUI", "Forces a full rescan of all games previously identified.");
TRANSLATE_NOOP("FullscreenUI", "Forcibly mutes both CD-DA and XA audio from the CD-ROM. Can be used to disable background music in some games.");
TRANSLATE_NOOP("FullscreenUI", "Fullscreen Resolution");
TRANSLATE_NOOP("FullscreenUI", "GPU Adapter");
TRANSLATE_NOOP("FullscreenUI", "GPU Renderer");
TRANSLATE_NOOP("FullscreenUI", "GPU adapter will be applied after restarting.");
TRANSLATE_NOOP("FullscreenUI", "Game Grid");
TRANSLATE_NOOP("FullscreenUI", "Game ID: {}");
TRANSLATE_NOOP("FullscreenUI", "Game List");
TRANSLATE_NOOP("FullscreenUI", "Game List Settings");
TRANSLATE_NOOP("FullscreenUI", "Game Properties");
TRANSLATE_NOOP("FullscreenUI", "Game Quick Save");
TRANSLATE_NOOP("FullscreenUI", "Game Slot {0}##game_slot_{0}");
TRANSLATE_NOOP("FullscreenUI", "Game Title: {}");
TRANSLATE_NOOP("FullscreenUI", "Game compatibility rating copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game not loaded or no RetroAchievements available.");
TRANSLATE_NOOP("FullscreenUI", "Game path copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game region copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game serial copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game settings have been cleared for '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Game settings initialized with global settings for '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Game title copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game type copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Genre: %s");
TRANSLATE_NOOP("FullscreenUI", "GitHub Repository");
TRANSLATE_NOOP("FullscreenUI", "Global Slot {0} - {1}##global_slot_{0}");
TRANSLATE_NOOP("FullscreenUI", "Global Slot {0}##global_slot_{0}");
TRANSLATE_NOOP("FullscreenUI", "Hardcore Mode");
TRANSLATE_NOOP("FullscreenUI", "Hides the mouse pointer/cursor when the emulator is in fullscreen mode.");
TRANSLATE_NOOP("FullscreenUI", "Hotkey Settings");
TRANSLATE_NOOP("FullscreenUI", "How many saves will be kept for rewinding. Higher values have greater memory requirements.");
TRANSLATE_NOOP("FullscreenUI", "How often a rewind state will be created. Higher frequencies have greater system requirements.");
TRANSLATE_NOOP("FullscreenUI", "Identifies any new files added to the game directories.");
TRANSLATE_NOOP("FullscreenUI", "If not enabled, the current post processing chain will be ignored.");
TRANSLATE_NOOP("FullscreenUI", "In the form below, specify the URLs to download covers from, with one template URL per line. The following variables are available:");
TRANSLATE_NOOP("FullscreenUI", "Increase Timer Resolution");
TRANSLATE_NOOP("FullscreenUI", "Increases the field of view from 4:3 to the chosen display aspect ratio in 3D games.");
TRANSLATE_NOOP("FullscreenUI", "Increases the precision of polygon culling, reducing the number of holes in geometry.");
TRANSLATE_NOOP("FullscreenUI", "Infinite/Instantaneous");
TRANSLATE_NOOP("FullscreenUI", "Inhibit Screensaver");
TRANSLATE_NOOP("FullscreenUI", "Input profile '{}' loaded.");
TRANSLATE_NOOP("FullscreenUI", "Input profile '{}' saved.");
TRANSLATE_NOOP("FullscreenUI", "Integer Upscaling");
TRANSLATE_NOOP("FullscreenUI", "Integration");
TRANSLATE_NOOP("FullscreenUI", "Interface Settings");
TRANSLATE_NOOP("FullscreenUI", "Internal Resolution Scale");
TRANSLATE_NOOP("FullscreenUI", "Internal Resolution Screenshots");
TRANSLATE_NOOP("FullscreenUI", "Issue Tracker");
TRANSLATE_NOOP("FullscreenUI", "Last Played");
TRANSLATE_NOOP("FullscreenUI", "Last Played: %s");
TRANSLATE_NOOP("FullscreenUI", "Launch a game by selecting a file/disc image.");
TRANSLATE_NOOP("FullscreenUI", "Launch a game from images scanned from your game directories.");
TRANSLATE_NOOP("FullscreenUI", "Leaderboards");
TRANSLATE_NOOP("FullscreenUI", "Limits how many frames are displayed to the screen. These frames are still rendered.");
TRANSLATE_NOOP("FullscreenUI", "Linear Upscaling");
TRANSLATE_NOOP("FullscreenUI", "Load Devices From Save States");
TRANSLATE_NOOP("FullscreenUI", "Load Profile");
TRANSLATE_NOOP("FullscreenUI", "Load Resume State");
TRANSLATE_NOOP("FullscreenUI", "Load State");
TRANSLATE_NOOP("FullscreenUI", "Loads a global save state.");
TRANSLATE_NOOP("FullscreenUI", "Loads all replacement texture to RAM, reducing stuttering at runtime.");
TRANSLATE_NOOP("FullscreenUI", "Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay.");
TRANSLATE_NOOP("FullscreenUI", "Locked Achievements");
TRANSLATE_NOOP("FullscreenUI", "Log Level");
TRANSLATE_NOOP("FullscreenUI", "Log To Debug Console");
TRANSLATE_NOOP("FullscreenUI", "Log To File");
TRANSLATE_NOOP("FullscreenUI", "Log To System Console");
TRANSLATE_NOOP("FullscreenUI", "Logging");
TRANSLATE_NOOP("FullscreenUI", "Logging Settings");
TRANSLATE_NOOP("FullscreenUI", "Login");
TRANSLATE_NOOP("FullscreenUI", "Login token generated on {}");
TRANSLATE_NOOP("FullscreenUI", "Logout");
TRANSLATE_NOOP("FullscreenUI", "Logs in to RetroAchievements.");
TRANSLATE_NOOP("FullscreenUI", "Logs messages to duckstation.log in the user directory.");
TRANSLATE_NOOP("FullscreenUI", "Logs messages to the console window.");
TRANSLATE_NOOP("FullscreenUI", "Logs messages to the debug console where supported.");
TRANSLATE_NOOP("FullscreenUI", "Logs out of RetroAchievements.");
TRANSLATE_NOOP("FullscreenUI", "Macro will toggle every {} frames.");
TRANSLATE_NOOP("FullscreenUI", "Macro {} Buttons");
TRANSLATE_NOOP("FullscreenUI", "Macro {} Frequency");
TRANSLATE_NOOP("FullscreenUI", "Macro {} Trigger");
TRANSLATE_NOOP("FullscreenUI", "Makes games run closer to their console framerate, at a small cost to performance.");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Directory");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Port {}");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Settings");
TRANSLATE_NOOP("FullscreenUI", "Memory Card {} Type");
TRANSLATE_NOOP("FullscreenUI", "Memory card '{}' created.");
TRANSLATE_NOOP("FullscreenUI", "Minimal Output Latency");
TRANSLATE_NOOP("FullscreenUI", "Move Down");
TRANSLATE_NOOP("FullscreenUI", "Move Up");
TRANSLATE_NOOP("FullscreenUI", "Moves this shader higher in the chain, applying it earlier.");
TRANSLATE_NOOP("FullscreenUI", "Moves this shader lower in the chain, applying it later.");
TRANSLATE_NOOP("FullscreenUI", "Multitap Mode");
TRANSLATE_NOOP("FullscreenUI", "Mute All Sound");
TRANSLATE_NOOP("FullscreenUI", "Mute CD Audio");
TRANSLATE_NOOP("FullscreenUI", "No");
TRANSLATE_NOOP("FullscreenUI", "No Binding");
TRANSLATE_NOOP("FullscreenUI", "No Buttons Selected");
TRANSLATE_NOOP("FullscreenUI", "No Game Selected");
TRANSLATE_NOOP("FullscreenUI", "No cheats found for {}.");
TRANSLATE_NOOP("FullscreenUI", "No input profiles available.");
TRANSLATE_NOOP("FullscreenUI", "No resume save state found.");
TRANSLATE_NOOP("FullscreenUI", "No save present in this slot.");
TRANSLATE_NOOP("FullscreenUI", "No save states found.");
TRANSLATE_NOOP("FullscreenUI", "None (Double Speed)");
TRANSLATE_NOOP("FullscreenUI", "None (Normal Speed)");
TRANSLATE_NOOP("FullscreenUI", "Not Logged In");
TRANSLATE_NOOP("FullscreenUI", "Not Scanning Subdirectories");
TRANSLATE_NOOP("FullscreenUI", "OK");
TRANSLATE_NOOP("FullscreenUI", "OSD Scale");
TRANSLATE_NOOP("FullscreenUI", "On-Screen Display");
TRANSLATE_NOOP("FullscreenUI", "Open in File Browser");
TRANSLATE_NOOP("FullscreenUI", "Operations");
TRANSLATE_NOOP("FullscreenUI", "Optimal Frame Pacing");
TRANSLATE_NOOP("FullscreenUI", "Options");
TRANSLATE_NOOP("FullscreenUI", "Output Latency");
TRANSLATE_NOOP("FullscreenUI", "Output Volume");
TRANSLATE_NOOP("FullscreenUI", "Overclocking Percentage");
TRANSLATE_NOOP("FullscreenUI", "PGXP Depth Clear Threshold");
TRANSLATE_NOOP("FullscreenUI", "PGXP Geometry Correction");
TRANSLATE_NOOP("FullscreenUI", "PGXP Geometry Tolerance");
TRANSLATE_NOOP("FullscreenUI", "PGXP Settings");
TRANSLATE_NOOP("FullscreenUI", "Password: ");
TRANSLATE_NOOP("FullscreenUI", "Patches");
TRANSLATE_NOOP("FullscreenUI", "Patches the BIOS to log calls to printf(). Only use when debugging, can break games.");
TRANSLATE_NOOP("FullscreenUI", "Patches the BIOS to skip the boot animation. Safe to enable.");
TRANSLATE_NOOP("FullscreenUI", "Path");
TRANSLATE_NOOP("FullscreenUI", "Pause On Focus Loss");
TRANSLATE_NOOP("FullscreenUI", "Pause On Menu");
TRANSLATE_NOOP("FullscreenUI", "Pause On Start");
TRANSLATE_NOOP("FullscreenUI", "Pauses the emulator when a game is started.");
TRANSLATE_NOOP("FullscreenUI", "Pauses the emulator when you minimize the window or switch to another application, and unpauses when you switch back.");
TRANSLATE_NOOP("FullscreenUI", "Pauses the emulator when you open the quick menu, and unpauses when you close it.");
TRANSLATE_NOOP("FullscreenUI", "Per-Game Configuration");
TRANSLATE_NOOP("FullscreenUI", "Per-game controller configuration initialized with global settings.");
TRANSLATE_NOOP("FullscreenUI", "Performance enhancement - jumps directly between blocks instead of returning to the dispatcher.");
TRANSLATE_NOOP("FullscreenUI", "Perspective Correct Colors");
TRANSLATE_NOOP("FullscreenUI", "Perspective Correct Textures");
TRANSLATE_NOOP("FullscreenUI", "Plays sound effects for events such as achievement unlocks and leaderboard submissions.");
TRANSLATE_NOOP("FullscreenUI", "Please enter your user name and password for retroachievements.org.");
TRANSLATE_NOOP("FullscreenUI", "Port {} Controller Type");
TRANSLATE_NOOP("FullscreenUI", "Position");
TRANSLATE_NOOP("FullscreenUI", "Post-Processing Settings");
TRANSLATE_NOOP("FullscreenUI", "Post-processing chain cleared.");
TRANSLATE_NOOP("FullscreenUI", "Post-processing chain is empty.");
TRANSLATE_NOOP("FullscreenUI", "Post-processing shaders reloaded.");
TRANSLATE_NOOP("FullscreenUI", "Preload Images to RAM");
TRANSLATE_NOOP("FullscreenUI", "Preload Replacement Textures");
TRANSLATE_NOOP("FullscreenUI", "Presents frames on a background thread when fast forwarding or vsync is disabled.");
TRANSLATE_NOOP("FullscreenUI", "Preserve Projection Precision");
TRANSLATE_NOOP("FullscreenUI", "Prevents the emulator from producing any audible sound.");
TRANSLATE_NOOP("FullscreenUI", "Prevents the screen saver from activating and the host from sleeping while emulation is running.");
TRANSLATE_NOOP("FullscreenUI", "Provides vibration and LED control support over Bluetooth.");
TRANSLATE_NOOP("FullscreenUI", "Push a controller button or axis now.");
TRANSLATE_NOOP("FullscreenUI", "Quick Save");
TRANSLATE_NOOP("FullscreenUI", "RAIntegration is being used instead of the built-in achievements implementation.");
TRANSLATE_NOOP("FullscreenUI", "Read Speedup");
TRANSLATE_NOOP("FullscreenUI", "Readahead Sectors");
TRANSLATE_NOOP("FullscreenUI", "Recompiler Fast Memory Access");
TRANSLATE_NOOP("FullscreenUI", "Reduces \"wobbly\" polygons by attempting to preserve the fractional component through memory transfers.");
TRANSLATE_NOOP("FullscreenUI", "Reduces hitches in emulation by reading/decompressing CD data asynchronously on a worker thread.");
TRANSLATE_NOOP("FullscreenUI", "Reduces polygon Z-fighting through depth testing. Low compatibility with games.");
TRANSLATE_NOOP("FullscreenUI", "Region");
TRANSLATE_NOOP("FullscreenUI", "Region: ");
TRANSLATE_NOOP("FullscreenUI", "Release Date: %s");
TRANSLATE_NOOP("FullscreenUI", "Reload Shaders");
TRANSLATE_NOOP("FullscreenUI", "Reloads the shaders from disk, applying any changes.");
TRANSLATE_NOOP("FullscreenUI", "Remove From Chain");
TRANSLATE_NOOP("FullscreenUI", "Remove From List");
TRANSLATE_NOOP("FullscreenUI", "Removed stage {} ({}).");
TRANSLATE_NOOP("FullscreenUI", "Removes this shader from the chain.");
TRANSLATE_NOOP("FullscreenUI", "Renames existing save states when saving to a backup file.");
TRANSLATE_NOOP("FullscreenUI", "Replaces these settings with a previously saved input profile.");
TRANSLATE_NOOP("FullscreenUI", "Rescan All Games");
TRANSLATE_NOOP("FullscreenUI", "Reset Memory Card Directory");
TRANSLATE_NOOP("FullscreenUI", "Reset Play Time");
TRANSLATE_NOOP("FullscreenUI", "Reset Settings");
TRANSLATE_NOOP("FullscreenUI", "Reset System");
TRANSLATE_NOOP("FullscreenUI", "Resets all configuration to defaults (including bindings).");
TRANSLATE_NOOP("FullscreenUI", "Resets memory card directory to default (user directory).");
TRANSLATE_NOOP("FullscreenUI", "Resolution change will be applied after restarting.");
TRANSLATE_NOOP("FullscreenUI", "Restores the state of the system prior to the last state loaded.");
TRANSLATE_NOOP("FullscreenUI", "Resume");
TRANSLATE_NOOP("FullscreenUI", "Resume Game");
TRANSLATE_NOOP("FullscreenUI", "Rewind Save Frequency");
TRANSLATE_NOOP("FullscreenUI", "Rewind Save Slots");
TRANSLATE_NOOP("FullscreenUI", "Rewind for {0} frames, lasting {1:.2f} seconds will require up to {3} MB of RAM and {4} MB of VRAM.");
TRANSLATE_NOOP("FullscreenUI", "Rich Presence");
TRANSLATE_NOOP("FullscreenUI", "Rich presence inactive or unsupported.");
TRANSLATE_NOOP("FullscreenUI", "Runahead");
TRANSLATE_NOOP("FullscreenUI", "Runahead/Rewind");
TRANSLATE_NOOP("FullscreenUI", "Runs the software renderer in parallel for VRAM readbacks. On some systems, this may result in greater performance.");
TRANSLATE_NOOP("FullscreenUI", "SDL DualShock 4 / DualSense Enhanced Mode");
TRANSLATE_NOOP("FullscreenUI", "Save Profile");
TRANSLATE_NOOP("FullscreenUI", "Save Screenshot");
TRANSLATE_NOOP("FullscreenUI", "Save State");
TRANSLATE_NOOP("FullscreenUI", "Save State On Exit");
TRANSLATE_NOOP("FullscreenUI", "Saved {:%c}");
TRANSLATE_NOOP("FullscreenUI", "Saves screenshots at internal render resolution and without postprocessing.");
TRANSLATE_NOOP("FullscreenUI", "Saves state periodically so you can rewind any mistakes while playing.");
TRANSLATE_NOOP("FullscreenUI", "Scaled Dithering");
TRANSLATE_NOOP("FullscreenUI", "Scales internal VRAM resolution by the specified multiplier. Some games require 1x VRAM resolution.");
TRANSLATE_NOOP("FullscreenUI", "Scales the dithering pattern with the internal rendering resolution, making it less noticeable. Usually safe to enable.");
TRANSLATE_NOOP("FullscreenUI", "Scan For New Games");
TRANSLATE_NOOP("FullscreenUI", "Scanning Subdirectories");
TRANSLATE_NOOP("FullscreenUI", "Screen Display");
TRANSLATE_NOOP("FullscreenUI", "Search Directories");
TRANSLATE_NOOP("FullscreenUI", "Seek Speedup");
TRANSLATE_NOOP("FullscreenUI", "Select Device");
TRANSLATE_NOOP("FullscreenUI", "Select Disc Image");
TRANSLATE_NOOP("FullscreenUI", "Select Macro {} Binds");
TRANSLATE_NOOP("FullscreenUI", "Selects the GPU to use for rendering.");
TRANSLATE_NOOP("FullscreenUI", "Selects the percentage of the normal clock speed the emulated hardware will run at.");
TRANSLATE_NOOP("FullscreenUI", "Selects the resolution to use in fullscreen modes.");
TRANSLATE_NOOP("FullscreenUI", "Serial");
TRANSLATE_NOOP("FullscreenUI", "Session: {}");
TRANSLATE_NOOP("FullscreenUI", "Set Input Binding");
TRANSLATE_NOOP("FullscreenUI", "Set VRAM Write Dump Alpha Channel");
TRANSLATE_NOOP("FullscreenUI", "Sets a threshold for discarding precise values when exceeded. May help with glitches in some games.");
TRANSLATE_NOOP("FullscreenUI", "Sets a threshold for discarding the emulated depth buffer. May help in some games.");
TRANSLATE_NOOP("FullscreenUI", "Sets the fast forward speed. It is not guaranteed that this speed will be reached on all systems.");
TRANSLATE_NOOP("FullscreenUI", "Sets the target emulation speed. It is not guaranteed that this speed will be reached on all systems.");
TRANSLATE_NOOP("FullscreenUI", "Sets the turbo speed. It is not guaranteed that this speed will be reached on all systems.");
TRANSLATE_NOOP("FullscreenUI", "Sets the verbosity of messages logged. Higher levels will log more messages.");
TRANSLATE_NOOP("FullscreenUI", "Sets which sort of memory card image will be used for slot {}.");
TRANSLATE_NOOP("FullscreenUI", "Setting {} binding {}.");
TRANSLATE_NOOP("FullscreenUI", "Settings");
TRANSLATE_NOOP("FullscreenUI", "Shader {} added as stage {}.");
TRANSLATE_NOOP("FullscreenUI", "Shared Card Name");
TRANSLATE_NOOP("FullscreenUI", "Show CPU Usage");
TRANSLATE_NOOP("FullscreenUI", "Show Challenge Indicators");
TRANSLATE_NOOP("FullscreenUI", "Show Controller Input");
TRANSLATE_NOOP("FullscreenUI", "Show Enhancement Settings");
TRANSLATE_NOOP("FullscreenUI", "Show FPS");
TRANSLATE_NOOP("FullscreenUI", "Show Frame Times");
TRANSLATE_NOOP("FullscreenUI", "Show GPU Usage");
TRANSLATE_NOOP("FullscreenUI", "Show Notifications");
TRANSLATE_NOOP("FullscreenUI", "Show OSD Messages");
TRANSLATE_NOOP("FullscreenUI", "Show Resolution");
TRANSLATE_NOOP("FullscreenUI", "Show Speed");
TRANSLATE_NOOP("FullscreenUI", "Show Status Indicators");
TRANSLATE_NOOP("FullscreenUI", "Shows a visual history of frame times in the upper-left corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows enhancement settings in the bottom-right corner of the screen.");
TRANSLATE_NOOP("FullscreenUI", "Shows icons in the lower-right corner of the screen when a challenge/primed achievement is active.");
TRANSLATE_NOOP("FullscreenUI", "Shows on-screen-display messages when events occur.");
TRANSLATE_NOOP("FullscreenUI", "Shows persistent icons when turbo is active or when paused.");
TRANSLATE_NOOP("FullscreenUI", "Shows the current controller state of the system in the bottom-left corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows the current emulation speed of the system in the top-right corner of the display as a percentage.");
TRANSLATE_NOOP("FullscreenUI", "Shows the current rendering resolution of the system in the top-right corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows the host's CPU usage based on threads in the top-right corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows the host's GPU usage in the top-right corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows the number of frames (or v-syncs) displayed per second by the system in the top-right corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Simulates the CPU's instruction cache in the recompiler. Can help with games running too fast.");
TRANSLATE_NOOP("FullscreenUI", "Simulates the region check present in original, unmodified consoles.");
TRANSLATE_NOOP("FullscreenUI", "Simulates the system ahead of time and rolls back/replays to reduce input lag. Very high system requirements.");
TRANSLATE_NOOP("FullscreenUI", "Size");
TRANSLATE_NOOP("FullscreenUI", "Size: %.2f MB");
TRANSLATE_NOOP("FullscreenUI", "Slow Boot");
TRANSLATE_NOOP("FullscreenUI", "Smooths out blockyness between colour transitions in 24-bit content, usually FMVs. Only applies to the hardware renderers.");
TRANSLATE_NOOP("FullscreenUI", "Smooths out the blockiness of magnified textures on 3D objects.");
TRANSLATE_NOOP("FullscreenUI", "Sort By");
TRANSLATE_NOOP("FullscreenUI", "Sort Reversed");
TRANSLATE_NOOP("FullscreenUI", "Speed Control");
TRANSLATE_NOOP("FullscreenUI", "Speeds up CD-ROM reads by the specified factor. May improve loading speeds in some games, and break others.");
TRANSLATE_NOOP("FullscreenUI", "Speeds up CD-ROM seeks by the specified factor. May improve loading speeds in some games, and break others.");
TRANSLATE_NOOP("FullscreenUI", "Stage {}: {}");
TRANSLATE_NOOP("FullscreenUI", "Start BIOS");
TRANSLATE_NOOP("FullscreenUI", "Start Download");
TRANSLATE_NOOP("FullscreenUI", "Start File");
TRANSLATE_NOOP("FullscreenUI", "Start Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Start the console without any disc inserted.");
TRANSLATE_NOOP("FullscreenUI", "Starts the console from where it was before it was last closed.");
TRANSLATE_NOOP("FullscreenUI", "Stores the current settings to an input profile.");
TRANSLATE_NOOP("FullscreenUI", "Stretch Display Vertically");
TRANSLATE_NOOP("FullscreenUI", "Stretch Mode");
TRANSLATE_NOOP("FullscreenUI", "Stretch To Fit");
TRANSLATE_NOOP("FullscreenUI", "Stretches the display to match the aspect ratio by multiplying vertically instead of horizontally.");
TRANSLATE_NOOP("FullscreenUI", "Summary");
TRANSLATE_NOOP("FullscreenUI", "Switches back to 4:3 display aspect ratio when displaying 24-bit content, usually FMVs.");
TRANSLATE_NOOP("FullscreenUI", "Switches between full screen and windowed when the window is double-clicked.");
TRANSLATE_NOOP("FullscreenUI", "Sync To Host Refresh Rate");
TRANSLATE_NOOP("FullscreenUI", "Synchronizes presentation of the console's frames to the host. Enable for smoother animations.");
TRANSLATE_NOOP("FullscreenUI", "Temporarily disables all enhancements, useful when testing.");
TRANSLATE_NOOP("FullscreenUI", "Test Mode");
TRANSLATE_NOOP("FullscreenUI", "Test Unofficial Achievements");
TRANSLATE_NOOP("FullscreenUI", "Texture Dumping");
TRANSLATE_NOOP("FullscreenUI", "Texture Filtering");
TRANSLATE_NOOP("FullscreenUI", "Texture Replacements");
TRANSLATE_NOOP("FullscreenUI", "The SDL input source supports most controllers.");
TRANSLATE_NOOP("FullscreenUI", "The XInput source provides support for XBox 360/XBox One/XBox Series controllers.");
TRANSLATE_NOOP("FullscreenUI", "The audio backend determines how frames produced by the emulator are submitted to the host.");
TRANSLATE_NOOP("FullscreenUI", "The selected memory card image will be used in shared mode for this slot.");
TRANSLATE_NOOP("FullscreenUI", "This build was not compiled with RetroAchivements support.");
TRANSLATE_NOOP("FullscreenUI", "Threaded Presentation");
TRANSLATE_NOOP("FullscreenUI", "Threaded Rendering");
TRANSLATE_NOOP("FullscreenUI", "Time Played");
TRANSLATE_NOOP("FullscreenUI", "Time Played: %s");
TRANSLATE_NOOP("FullscreenUI", "Timing out in %.0f seconds...");
TRANSLATE_NOOP("FullscreenUI", "Title");
TRANSLATE_NOOP("FullscreenUI", "Toggle Analog");
TRANSLATE_NOOP("FullscreenUI", "Toggle Fast Forward");
TRANSLATE_NOOP("FullscreenUI", "Toggle every %d frames");
TRANSLATE_NOOP("FullscreenUI", "True Color Rendering");
TRANSLATE_NOOP("FullscreenUI", "Turbo Speed");
TRANSLATE_NOOP("FullscreenUI", "Type");
TRANSLATE_NOOP("FullscreenUI", "Undo Load State");
TRANSLATE_NOOP("FullscreenUI", "Unknown");
TRANSLATE_NOOP("FullscreenUI", "Unlimited");
TRANSLATE_NOOP("FullscreenUI", "Unlocked Achievements");
TRANSLATE_NOOP("FullscreenUI", "Use Blit Swap Chain");
TRANSLATE_NOOP("FullscreenUI", "Use Debug GPU Device");
TRANSLATE_NOOP("FullscreenUI", "Use Global Setting");
TRANSLATE_NOOP("FullscreenUI", "Use Light Theme");
TRANSLATE_NOOP("FullscreenUI", "Use Serial File Names");
TRANSLATE_NOOP("FullscreenUI", "Use Single Card For Sub-Images");
TRANSLATE_NOOP("FullscreenUI", "Use Software Renderer For Readbacks");
TRANSLATE_NOOP("FullscreenUI", "User Name: ");
TRANSLATE_NOOP("FullscreenUI", "Username: {}");
TRANSLATE_NOOP("FullscreenUI", "Uses PGXP for all instructions, not just memory operations.");
TRANSLATE_NOOP("FullscreenUI", "Uses a bilinear filter when upscaling to display, smoothing out the image.");
TRANSLATE_NOOP("FullscreenUI", "Uses a blit presentation model instead of flipping. This may be needed on some systems.");
TRANSLATE_NOOP("FullscreenUI", "Uses a light coloured theme instead of the default dark theme.");
TRANSLATE_NOOP("FullscreenUI", "Uses a second thread for drawing graphics. Speed boost, and safe to use.");
TRANSLATE_NOOP("FullscreenUI", "Uses game-specific settings for controllers for this game.");
TRANSLATE_NOOP("FullscreenUI", "Uses perspective-correct interpolation for colors, which can improve visuals in some games.");
TRANSLATE_NOOP("FullscreenUI", "Uses perspective-correct interpolation for texture coordinates, straightening out warped textures.");
TRANSLATE_NOOP("FullscreenUI", "Uses screen positions to resolve PGXP data. May improve visuals in some games.");
TRANSLATE_NOOP("FullscreenUI", "Value: {} | Default: {} | Minimum: {} | Maximum: {}");
TRANSLATE_NOOP("FullscreenUI", "When enabled and logged in, DuckStation will scan for achievements on startup.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, DuckStation will assume all achievements are locked and not send any unlock notifications to the server.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, DuckStation will list achievements from unofficial sets. These achievements are not tracked by RetroAchievements.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, memory cards and controllers will be overwritten when save states are loaded.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, per-game settings will be applied, and incompatible enhancements will be disabled.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, rich presence information will be collected and sent to the server where supported.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, the minimum supported output latency will be used for the host API.");
TRANSLATE_NOOP("FullscreenUI", "When this option is chosen, the clock speed set below will be used.");
TRANSLATE_NOOP("FullscreenUI", "When using a multi-disc image (m3u/pbp) and per-game (title) memory cards, use a single memory card for all discs.");
TRANSLATE_NOOP("FullscreenUI", "Writes textures which can be replaced to the dump directory.");
TRANSLATE_NOOP("FullscreenUI", "XXX points");
TRANSLATE_NOOP("FullscreenUI", "Yes");
TRANSLATE_NOOP("FullscreenUI", "You have unlocked all achievements and earned {} points!");
TRANSLATE_NOOP("FullscreenUI", "You have unlocked {} of {} achievements, earning {} of {} possible points.");
TRANSLATE_NOOP("FullscreenUI", "Your password will not be saved in DuckStation, an access token will be generated and used instead.");
TRANSLATE_NOOP("FullscreenUI", "\"Challenge\" mode for achievements, including leaderboard tracking. Disables save state, cheats, and slowdown functions.");
TRANSLATE_NOOP("FullscreenUI", "\"PlayStation\" and \"PSX\" are registered trademarks of Sony Interactive Entertainment Europe Limited. This software is not affiliated in any way with Sony Interactive Entertainment.");
TRANSLATE_NOOP("FullscreenUI", "{} deleted.");
TRANSLATE_NOOP("FullscreenUI", "{} does not exist.");
TRANSLATE_NOOP("FullscreenUI", "{} is not a valid disc image.");
TRANSLATE_NOOP("FullscreenUI", "{} point");
TRANSLATE_NOOP("FullscreenUI", "{} points");
// TRANSLATION-STRING-AREA-END
#endif
