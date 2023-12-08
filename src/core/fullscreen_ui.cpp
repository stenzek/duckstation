// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "fullscreen_ui.h"
#include "achievements.h"
#include "bios.h"
#include "cheats.h"
#include "controller.h"
#include "game_list.h"
#include "gpu.h"
#include "gpu_thread.h"
#include "host.h"
#include "imgui_overlays.h"
#include "settings.h"
#include "system.h"
#include "system_private.h"

#include "scmversion/scmversion.h"

#include "util/cd_image.h"
#include "util/gpu_device.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/ini_settings_interface.h"
#include "util/input_manager.h"
#include "util/postprocessing.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome5.h"
#include "IconsPromptFont.h"
#include "fmt/chrono.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <atomic>
#include <bitset>
#include <unordered_map>
#include <utility>
#include <vector>

LOG_CHANNEL(FullscreenUI);

#define TR_CONTEXT "FullscreenUI"

namespace {
template<size_t L>
class IconStackString : public SmallStackString<L>
{
public:
  ALWAYS_INLINE IconStackString(const char* icon, const char* str)
  {
    SmallStackString<L>::format("{} {}", icon, Host::TranslateToStringView(TR_CONTEXT, str));
  }
  ALWAYS_INLINE IconStackString(const char* icon, const char* str, const char* suffix)
  {
    SmallStackString<L>::format("{} {}##{}", icon, Host::TranslateToStringView(TR_CONTEXT, str), suffix);
  }
};
} // namespace

#define FSUI_ICONSTR(icon, str) IconStackString<128>(icon, str).c_str()
#define FSUI_STR(str) Host::TranslateToString(TR_CONTEXT, str)
#define FSUI_CSTR(str) Host::TranslateToCString(TR_CONTEXT, str)
#define FSUI_VSTR(str) Host::TranslateToStringView(TR_CONTEXT, str)
#define FSUI_FSTR(str) fmt::runtime(Host::TranslateToStringView(TR_CONTEXT, str))
#define FSUI_NSTR(str) str

using ImGuiFullscreen::FocusResetType;
using ImGuiFullscreen::LAYOUT_FOOTER_HEIGHT;
using ImGuiFullscreen::LAYOUT_LARGE_FONT_SIZE;
using ImGuiFullscreen::LAYOUT_MEDIUM_FONT_SIZE;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING;
using ImGuiFullscreen::LAYOUT_SCREEN_HEIGHT;
using ImGuiFullscreen::LAYOUT_SCREEN_WIDTH;
using ImGuiFullscreen::UIStyle;

using ImGuiFullscreen::ActiveButton;
using ImGuiFullscreen::AddNotification;
using ImGuiFullscreen::BeginFullscreenColumns;
using ImGuiFullscreen::BeginFullscreenColumnWindow;
using ImGuiFullscreen::BeginFullscreenWindow;
using ImGuiFullscreen::BeginHorizontalMenu;
using ImGuiFullscreen::BeginMenuButtons;
using ImGuiFullscreen::BeginNavBar;
using ImGuiFullscreen::CenterImage;
using ImGuiFullscreen::CloseChoiceDialog;
using ImGuiFullscreen::CloseFileSelector;
using ImGuiFullscreen::DefaultActiveButton;
using ImGuiFullscreen::DrawShadowedText;
using ImGuiFullscreen::EndFullscreenColumns;
using ImGuiFullscreen::EndFullscreenColumnWindow;
using ImGuiFullscreen::EndFullscreenWindow;
using ImGuiFullscreen::EndHorizontalMenu;
using ImGuiFullscreen::EndMenuButtons;
using ImGuiFullscreen::EndNavBar;
using ImGuiFullscreen::EnumChoiceButton;
using ImGuiFullscreen::FloatingButton;
using ImGuiFullscreen::ForceKeyNavEnabled;
using ImGuiFullscreen::GetCachedTexture;
using ImGuiFullscreen::GetCachedTextureAsync;
using ImGuiFullscreen::GetPlaceholderTexture;
using ImGuiFullscreen::HorizontalMenuItem;
using ImGuiFullscreen::IsFocusResetFromWindowChange;
using ImGuiFullscreen::IsFocusResetQueued;
using ImGuiFullscreen::IsGamepadInputSource;
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
using ImGuiFullscreen::NavTab;
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
using ImGuiFullscreen::SetFullscreenFooterText;
using ImGuiFullscreen::ShowToast;
using ImGuiFullscreen::ThreeWayToggleButton;
using ImGuiFullscreen::ToggleButton;
using ImGuiFullscreen::WantsToCloseMenu;

#ifndef __ANDROID__

namespace FullscreenUI {

enum class MainWindowType : u8
{
  None,
  Landing,
  StartGame,
  Exit,
  GameList,
  GameListSettings,
  Settings,
  PauseMenu,
  Achievements,
  Leaderboards,
};

enum class PauseSubMenu : u8
{
  None,
  Exit,
  Achievements,
};

enum class SettingsPage : u8
{
  Summary,
  Interface,
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

enum class GameListView : u8
{
  Grid,
  List,
  Count
};

struct PostProcessingStageInfo
{
  std::string name;
  std::vector<PostProcessing::ShaderOption> options;
};

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////
static void UpdateRunIdleState();
static void PauseForMenuOpen(bool set_pause_menu_open);
static bool AreAnyDialogsOpen();
static void ClosePauseMenu();
static void OpenPauseSubMenu(PauseSubMenu submenu);
static void DrawLandingTemplate(ImVec2* menu_pos, ImVec2* menu_size);
static void DrawLandingWindow();
static void DrawStartGameWindow();
static void DrawExitWindow();
static void DrawPauseMenu();
static void ExitFullscreenAndOpenURL(std::string_view url);
static void CopyTextToClipboard(std::string title, std::string_view text);
static void DrawAboutWindow();
static void OpenAboutWindow();
static void FixStateIfPaused();
static void GetStandardSelectionFooterText(SmallStringBase& dest, bool back_instead_of_cancel);

//////////////////////////////////////////////////////////////////////////
// Resources
//////////////////////////////////////////////////////////////////////////
static bool LoadResources();
static void DestroyResources();

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
static void DoStartDisc(std::string path);
static void DoStartDisc();
static void DoToggleFastForward();
static void ConfirmIfSavingMemoryCards(std::string_view action, std::function<void(bool)> callback);
static void RequestShutdown(bool save_state);
static void RequestReset();
static void DoChangeDiscFromFile();
static void DoChangeDisc();
static void DoRequestExit();
static void DoDesktopMode();
static void DoToggleFullscreen();
static void DoToggleAnalogMode();

//////////////////////////////////////////////////////////////////////////
// Settings
//////////////////////////////////////////////////////////////////////////

static constexpr double INPUT_BINDING_TIMEOUT_SECONDS = 5.0;

static void SwitchToSettings();
static bool SwitchToGameSettings();
static void SwitchToGameSettings(const GameList::Entry* entry);
static bool SwitchToGameSettingsForPath(const std::string& path);
static void SwitchToGameSettingsForSerial(std::string_view serial);
static void DrawSettingsWindow();
static void DrawSummarySettingsPage();
static void DrawInterfaceSettingsPage();
static void DrawBIOSSettingsPage();
static void DrawConsoleSettingsPage();
static void DrawEmulationSettingsPage();
static void DrawGraphicsSettingsPage();
static void DrawPostProcessingSettingsPage();
static void DrawAudioSettingsPage();
static void DrawMemoryCardSettingsPage();
static void DrawControllerSettingsPage();
static void DrawHotkeySettingsPage();
static void DrawAchievementsSettingsPage();
static void DrawAchievementsLoginWindow();
static void DrawAdvancedSettingsPage();
static void DrawPatchesOrCheatsSettingsPage(bool cheats);

static bool ShouldShowAdvancedSettings();
static bool IsEditingGameSettings(SettingsInterface* bsi);
static SettingsInterface* GetEditingSettingsInterface();
static SettingsInterface* GetEditingSettingsInterface(bool game_settings);
static void SetSettingsChanged(SettingsInterface* bsi);
static bool GetEffectiveBoolSetting(SettingsInterface* bsi, const char* section, const char* key, bool default_value);
static s32 GetEffectiveIntSetting(SettingsInterface* bsi, const char* section, const char* key, s32 default_value);
static u32 GetEffectiveUIntSetting(SettingsInterface* bsi, const char* section, const char* key, u32 default_value);
static float GetEffectiveFloatSetting(SettingsInterface* bsi, const char* section, const char* key,
                                      float default_value);
static TinyString GetEffectiveTinyStringSetting(SettingsInterface* bsi, const char* section, const char* key,
                                                const char* default_value);
static void DoCopyGameSettings();
static void DoClearGameSettings();
static void CopyGlobalControllerSettingsToGame();
static void ResetControllerSettings();
static void DoLoadInputProfile();
static void DoSaveInputProfile();
static void DoSaveNewInputProfile();
static void DoSaveInputProfile(const std::string& name);

static bool DrawToggleSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                              const char* key, bool default_value, bool enabled = true, bool allow_tristate = true,
                              float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                              ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont);
static void DrawIntListSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                               const char* key, int default_value, const char* const* options, size_t option_count,
                               bool translate_options, int option_offset = 0, bool enabled = true,
                               float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                               ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont,
                               const char* tr_context = TR_CONTEXT);
static void DrawIntRangeSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                                const char* key, int default_value, int min_value, int max_value,
                                const char* format = "%d", bool enabled = true,
                                float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont);
static void DrawIntSpinBoxSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                                  const char* key, int default_value, int min_value, int max_value, int step_value,
                                  const char* format = "%d", bool enabled = true,
                                  float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                  ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont);
static void DrawFloatRangeSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                                  const char* key, float default_value, float min_value, float max_value,
                                  const char* format = "%f", float multiplier = 1.0f, bool enabled = true,
                                  float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                  ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont);
static void DrawFloatSpinBoxSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                                    const char* key, float default_value, float min_value, float max_value,
                                    float step_value, float multiplier, const char* format = "%f", bool enabled = true,
                                    float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                    ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont);
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
                            ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont);
static void DrawFloatListSetting(SettingsInterface* bsi, const char* title, const char* summary, const char* section,
                                 const char* key, float default_value, const char* const* options,
                                 const float* option_values, size_t option_count, bool translate_options,
                                 bool enabled = true, float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                 ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont);
static void DrawFolderSetting(SettingsInterface* bsi, const char* title, const char* section, const char* key,
                              const std::string& runtime_var, float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                              ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont);

static void PopulateGraphicsAdapterList();
static void PopulateGameListDirectoryCache(SettingsInterface* si);
static void PopulatePatchesAndCheatsList(const std::string_view serial);
static void PopulatePostProcessingChain(SettingsInterface* si, const char* section);
static void BeginInputBinding(SettingsInterface* bsi, InputBindingInfo::Type type, std::string_view section,
                              std::string_view key, std::string_view display_name);
static void DrawInputBindingWindow();
static void DrawInputBindingButton(SettingsInterface* bsi, InputBindingInfo::Type type, const char* section,
                                   const char* name, const char* display_name, const char* icon_name,
                                   bool show_type = true);
static void ClearInputBindingVariables();
static void StartAutomaticBinding(u32 port);

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

static void InitializePlaceholderSaveStateListEntry(SaveStateListEntry* li, const std::string& serial, s32 slot,
                                                    bool global);
static bool InitializeSaveStateListEntryFromSerial(SaveStateListEntry* li, const std::string& serial, s32 slot,
                                                   bool global);
static bool InitializeSaveStateListEntryFromPath(SaveStateListEntry* li, std::string path, s32 slot, bool global);
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

//////////////////////////////////////////////////////////////////////////
// Game List
//////////////////////////////////////////////////////////////////////////
static void DrawGameListWindow();
static void DrawGameList(const ImVec2& heading_size);
static void DrawGameGrid(const ImVec2& heading_size);
static void HandleGameListActivate(const GameList::Entry* entry);
static void HandleGameListOptions(const GameList::Entry* entry);
static void HandleSelectDiscForDiscSet(std::string_view disc_set_name);
static void DrawGameListSettingsWindow();
static void SwitchToGameList();
static void PopulateGameListEntryList();
static GPUTexture* GetTextureForGameListEntryType(GameList::EntryType type);
static GPUTexture* GetGameListCover(const GameList::Entry* entry);
static GPUTexture* GetCoverForCurrentGame();

namespace {

struct ALIGN_TO_CACHE_LINE UIState
{
  // Main
  MainWindowType current_main_window = MainWindowType::None;
  PauseSubMenu current_pause_submenu = PauseSubMenu::None;
  bool initialized = false;
  bool tried_to_initialize = false;
  bool pause_menu_was_open = false;
  bool was_paused_on_quick_menu_open = false;
  bool about_window_open = false;
  bool achievements_login_window_open = false;
  std::string current_game_subtitle;

  // Resources
  std::shared_ptr<GPUTexture> app_icon_texture;
  std::shared_ptr<GPUTexture> fallback_disc_texture;
  std::shared_ptr<GPUTexture> fallback_exe_texture;
  std::shared_ptr<GPUTexture> fallback_psf_texture;
  std::shared_ptr<GPUTexture> fallback_playlist_texture;

  // Settings
  SettingsPage settings_page = SettingsPage::Interface;
  std::unique_ptr<INISettingsInterface> game_settings_interface;
  std::unique_ptr<GameList::Entry> game_settings_entry;
  std::vector<std::pair<std::string, bool>> game_list_directories_cache;
  GPUDevice::AdapterInfoList graphics_adapter_list_cache;
  std::vector<std::string> fullscreen_mode_list_cache;
  Cheats::CodeInfoList game_patch_list;
  std::vector<std::string> enabled_game_patch_cache;
  Cheats::CodeInfoList game_cheats_list;
  std::vector<std::string> enabled_game_cheat_cache;
  std::vector<std::string_view> game_cheat_groups;
  std::vector<PostProcessingStageInfo> postprocessing_stages;
  std::vector<const HotkeyInfo*> hotkey_list_cache;
  std::atomic_bool settings_changed{false};
  std::atomic_bool game_settings_changed{false};
  InputBindingInfo::Type input_binding_type = InputBindingInfo::Type::Unknown;
  std::string input_binding_section;
  std::string input_binding_key;
  std::string input_binding_display_name;
  std::vector<InputBindingKey> input_binding_new_bindings;
  std::vector<std::pair<InputBindingKey, std::pair<float, float>>> input_binding_value_ranges;
  Timer input_binding_timer;
  bool controller_macro_expanded[NUM_CONTROLLER_AND_CARD_PORTS][InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER] = {};

  // Save State List
  std::vector<SaveStateListEntry> save_state_selector_slots;
  std::string save_state_selector_game_path;
  s32 save_state_selector_submenu_index = -1;
  bool save_state_selector_open = false;
  bool save_state_selector_loading = true;
  bool save_state_selector_resuming = false;

  // Lazily populated cover images.
  std::unordered_map<std::string, std::string> cover_image_map;
  std::vector<const GameList::Entry*> game_list_sorted_entries;
  GameListView game_list_view = GameListView::Grid;
};

} // namespace

static UIState s_state;

} // namespace FullscreenUI

//////////////////////////////////////////////////////////////////////////
// Utility
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::TimeToPrintableString(SmallStringBase* str, time_t t)
{
  struct tm lt = {};
#ifdef _MSC_VER
  localtime_s(&lt, &t);
#else
  localtime_r(&t, &lt);
#endif

  char buf[256];
  std::strftime(buf, sizeof(buf), "%c", &lt);
  str->assign(buf);
}

void FullscreenUI::GetStandardSelectionFooterText(SmallStringBase& dest, bool back_instead_of_cancel)
{
  if (IsGamepadInputSource())
  {
    ImGuiFullscreen::CreateFooterTextString(
      dest,
      std::array{std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, FSUI_VSTR("Change Selection")),
                 std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Select")),
                 std::make_pair(ICON_PF_BUTTON_B, back_instead_of_cancel ? FSUI_VSTR("Back") : FSUI_VSTR("Cancel"))});
  }
  else
  {
    ImGuiFullscreen::CreateFooterTextString(
      dest, std::array{std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, FSUI_VSTR("Change Selection")),
                       std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")),
                       std::make_pair(ICON_PF_ESC, back_instead_of_cancel ? FSUI_VSTR("Back") : FSUI_VSTR("Cancel"))});
  }
}

void FullscreenUI::SetStandardSelectionFooterText(bool back_instead_of_cancel)
{
  SmallString text;
  GetStandardSelectionFooterText(text, back_instead_of_cancel);
  ImGuiFullscreen::SetFullscreenFooterText(text);
}

void ImGuiFullscreen::GetChoiceDialogHelpText(SmallStringBase& dest)
{
  FullscreenUI::GetStandardSelectionFooterText(dest, false);
}

void ImGuiFullscreen::GetFileSelectorHelpText(SmallStringBase& dest)
{
  if (IsGamepadInputSource())
  {
    ImGuiFullscreen::CreateFooterTextString(
      dest, std::array{std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, FSUI_VSTR("Change Selection")),
                       std::make_pair(ICON_PF_BUTTON_Y, FSUI_VSTR("Parent Directory")),
                       std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Select")),
                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Cancel"))});
  }
  else
  {
    ImGuiFullscreen::CreateFooterTextString(
      dest,
      std::array{std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, FSUI_VSTR("Change Selection")),
                 std::make_pair(ICON_PF_BACKSPACE, FSUI_VSTR("Parent Directory")),
                 std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")), std::make_pair(ICON_PF_ESC, FSUI_VSTR("Cancel"))});
  }
}

void ImGuiFullscreen::GetInputDialogHelpText(SmallStringBase& dest)
{
  if (IsGamepadInputSource())
  {
    CreateFooterTextString(dest, std::array{std::make_pair(ICON_PF_KEYBOARD, FSUI_VSTR("Enter Value")),
                                            std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Select")),
                                            std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Cancel"))});
  }
  else
  {
    CreateFooterTextString(dest, std::array{std::make_pair(ICON_PF_KEYBOARD, FSUI_VSTR("Enter Value")),
                                            std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")),
                                            std::make_pair(ICON_PF_ESC, FSUI_VSTR("Cancel"))});
  }
}

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////

bool FullscreenUI::Initialize()
{
  if (s_state.initialized)
    return true;

  if (s_state.tried_to_initialize)
    return false;

  ImGuiFullscreen::SetTheme(Host::GetBaseBoolSettingValue("Main", "UseLightFullscreenUITheme", false));
  ImGuiFullscreen::SetSmoothScrolling(Host::GetBaseBoolSettingValue("Main", "FullscreenUISmoothScrolling", true));
  ImGuiFullscreen::UpdateLayoutScale();

  if (!ImGuiManager::AddFullscreenFontsIfMissing() || !ImGuiFullscreen::Initialize("images/placeholder.png") ||
      !LoadResources())
  {
    DestroyResources();
    ImGuiFullscreen::Shutdown();
    s_state.tried_to_initialize = true;
    return false;
  }

  s_state.initialized = true;
  s_state.current_main_window = MainWindowType::None;
  s_state.current_pause_submenu = PauseSubMenu::None;
  s_state.pause_menu_was_open = false;
  s_state.was_paused_on_quick_menu_open = false;
  s_state.about_window_open = false;
  s_state.hotkey_list_cache = InputManager::GetHotkeyList();

  Host::RunOnCPUThread([]() { Host::OnFullscreenUIStartedOrStopped(true); });

  if (!GPUThread::HasGPUBackend())
    SwitchToLanding();

  UpdateRunIdleState();
  ForceKeyNavEnabled();
  return true;
}

bool FullscreenUI::IsInitialized()
{
  return s_state.initialized;
}

bool FullscreenUI::HasActiveWindow()
{
  return s_state.initialized && (s_state.current_main_window != MainWindowType::None || AreAnyDialogsOpen());
}

bool FullscreenUI::AreAnyDialogsOpen()
{
  return (s_state.save_state_selector_open || s_state.about_window_open ||
          s_state.input_binding_type != InputBindingInfo::Type::Unknown || ImGuiFullscreen::IsChoiceDialogOpen() ||
          ImGuiFullscreen::IsFileSelectorOpen());
}

void FullscreenUI::CheckForConfigChanges(const Settings& old_settings)
{
  // NOTE: Called on CPU thread.
  if (!IsInitialized())
    return;

  // If achievements got disabled, we might have the menu open...
  // That means we're going to be reading achievement state.
  if (old_settings.achievements_enabled && !g_settings.achievements_enabled)
  {
    if (!IsInitialized())
      return;

    GPUThread::RunOnThread([]() {
      if (s_state.current_main_window == MainWindowType::Achievements ||
          s_state.current_main_window == MainWindowType::Leaderboards)
      {
        ReturnToPreviousWindow();
      }
    });
  }
}

void FullscreenUI::UpdateRunIdleState()
{
  const bool new_run_idle = HasActiveWindow();
  GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::FullscreenUIActive, new_run_idle);
}

void FullscreenUI::OnSystemStarted()
{
  // NOTE: Called on CPU thread.
  if (!IsInitialized())
    return;

  GPUThread::RunOnThread([]() {
    if (!IsInitialized())
      return;

    s_state.current_main_window = MainWindowType::None;
    QueueResetFocus(FocusResetType::ViewChanged);
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
      ClosePauseMenu();

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
    SwitchToLanding();
    UpdateRunIdleState();
  });
}

void FullscreenUI::OnRunningGameChanged()
{
  // NOTE: Called on CPU thread.
  if (!IsInitialized())
    return;

  const std::string& path = System::GetDiscPath();
  const std::string& serial = System::GetGameSerial();

  std::string subtitle;
  if (!serial.empty())
    subtitle = fmt::format("{0} - {1}", serial, Path::GetFileName(path));
  else
    subtitle = {};

  GPUThread::RunOnThread([subtitle = std::move(subtitle)]() mutable {
    if (!IsInitialized())
      return;

    s_state.current_game_subtitle = std::move(subtitle);
  });
}

void FullscreenUI::PauseForMenuOpen(bool set_pause_menu_open)
{
  s_state.was_paused_on_quick_menu_open = (System::GetState() == System::State::Paused);
  if (!s_state.was_paused_on_quick_menu_open)
    Host::RunOnCPUThread([]() { System::PauseSystem(true); });

  s_state.pause_menu_was_open |= set_pause_menu_open;
}

void FullscreenUI::OpenPauseMenu()
{
  if (!System::IsValid())
    return;

  GPUThread::RunOnThread([]() {
    if (!Initialize() || s_state.current_main_window != MainWindowType::None)
      return;

    PauseForMenuOpen(true);
    s_state.current_main_window = MainWindowType::PauseMenu;
    s_state.current_pause_submenu = PauseSubMenu::None;
    QueueResetFocus(FocusResetType::ViewChanged);
    ForceKeyNavEnabled();
    UpdateRunIdleState();
    FixStateIfPaused();
  });
}

void FullscreenUI::OpenCheatsMenu()
{
  if (!System::IsValid())
    return;

  if (!Initialize() || s_state.current_main_window != MainWindowType::None || !SwitchToGameSettings())
    return;

  s_state.settings_page = SettingsPage::Cheats;
  PauseForMenuOpen(true);
  ForceKeyNavEnabled();
  UpdateRunIdleState();
  FixStateIfPaused();
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
  if (!System::IsValid())
    return;

  const bool paused = System::IsPaused();
  GPUThread::RunOnThread([paused]() {
    if (!IsInitialized())
      return;

    if (paused && !s_state.was_paused_on_quick_menu_open)
      Host::RunOnCPUThread([]() { System::PauseSystem(false); });

    s_state.current_main_window = MainWindowType::None;
    s_state.current_pause_submenu = PauseSubMenu::None;
    s_state.pause_menu_was_open = false;
    QueueResetFocus(FocusResetType::ViewChanged);
    UpdateRunIdleState();
    FixStateIfPaused();
  });
}

void FullscreenUI::OpenPauseSubMenu(PauseSubMenu submenu)
{
  s_state.current_main_window = MainWindowType::PauseMenu;
  s_state.current_pause_submenu = submenu;
  QueueResetFocus(FocusResetType::ViewChanged);
}

void FullscreenUI::Shutdown()
{
  Achievements::ClearUIState();
  ClearInputBindingVariables();
  CloseSaveStateSelector();
  s_state.cover_image_map.clear();
  std::memset(s_state.controller_macro_expanded, 0, sizeof(s_state.controller_macro_expanded));
  s_state.game_list_sorted_entries = {};
  s_state.game_list_directories_cache = {};
  s_state.game_patch_list = {};
  s_state.enabled_game_patch_cache = {};
  s_state.game_cheats_list = {};
  s_state.enabled_game_cheat_cache = {};
  s_state.game_cheat_groups = {};
  s_state.postprocessing_stages = {};
  s_state.fullscreen_mode_list_cache = {};
  s_state.graphics_adapter_list_cache = {};
  s_state.hotkey_list_cache = {};
  s_state.current_game_subtitle = {};
  DestroyResources();
  ImGuiFullscreen::Shutdown();
  if (s_state.initialized)
    Host::RunOnCPUThread([]() { Host::OnFullscreenUIStartedOrStopped(false); });

  s_state.initialized = false;
  s_state.tried_to_initialize = false;
  UpdateRunIdleState();
}

void FullscreenUI::Render()
{
  if (!s_state.initialized)
  {
    ImGuiFullscreen::RenderLoadingScreen();
    return;
  }

  ImGuiFullscreen::UploadAsyncTextures();

  ImGuiFullscreen::BeginLayout();

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
    case MainWindowType::GameListSettings:
      DrawGameListSettingsWindow();
      break;
    case MainWindowType::Settings:
      DrawSettingsWindow();
      break;
    case MainWindowType::PauseMenu:
      DrawPauseMenu();
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

  if (s_state.save_state_selector_open)
  {
    if (s_state.save_state_selector_resuming)
      DrawResumeStateSelector();
    else
      DrawSaveStateSelector(s_state.save_state_selector_loading);
  }

  if (s_state.about_window_open)
    DrawAboutWindow();

  if (s_state.input_binding_type != InputBindingInfo::Type::Unknown)
    DrawInputBindingWindow();

  ImGuiFullscreen::EndLayout();

  ImGuiFullscreen::RenderLoadingScreen();

  if (s_state.settings_changed.load(std::memory_order_relaxed))
  {
    Host::CommitBaseSettingChanges();
    Host::RunOnCPUThread([]() { System::ApplySettings(false); });
    s_state.settings_changed.store(false, std::memory_order_release);
  }
  if (s_state.game_settings_changed.load(std::memory_order_relaxed))
  {
    if (s_state.game_settings_interface)
    {
      Error error;
      s_state.game_settings_interface->RemoveEmptySections();

      if (s_state.game_settings_interface->IsEmpty())
      {
        if (FileSystem::FileExists(s_state.game_settings_interface->GetFileName().c_str()) &&
            !FileSystem::DeleteFile(s_state.game_settings_interface->GetFileName().c_str(), &error))
        {
          ImGuiFullscreen::OpenInfoMessageDialog(
            FSUI_STR("Error"), fmt::format(FSUI_FSTR("An error occurred while deleting empty game settings:\n{}"),
                                           error.GetDescription()));
        }
      }
      else
      {
        if (!s_state.game_settings_interface->Save(&error))
        {
          ImGuiFullscreen::OpenInfoMessageDialog(
            FSUI_STR("Error"),
            fmt::format(FSUI_FSTR("An error occurred while saving game settings:\n{}"), error.GetDescription()));
        }
      }

      if (GPUThread::HasGPUBackend())
        Host::RunOnCPUThread([]() { System::ReloadGameSettings(false); });
    }
    s_state.game_settings_changed.store(false, std::memory_order_release);
  }

  ImGuiFullscreen::ResetCloseMenuIfNeeded();
}

void FullscreenUI::InvalidateCoverCache()
{
  if (!IsInitialized())
    return;

  Host::RunOnCPUThread([]() { s_state.cover_image_map.clear(); });
}

void FullscreenUI::ReturnToPreviousWindow()
{
  if (GPUThread::HasGPUBackend() && s_state.pause_menu_was_open)
  {
    s_state.current_main_window = MainWindowType::PauseMenu;
    QueueResetFocus(FocusResetType::ViewChanged);
  }
  else
  {
    ReturnToMainWindow();
  }
}

void FullscreenUI::ReturnToMainWindow()
{
  ClosePauseMenu();
  s_state.current_main_window = GPUThread::HasGPUBackend() ? MainWindowType::None : MainWindowType::Landing;
  UpdateRunIdleState();
  FixStateIfPaused();
}

bool FullscreenUI::LoadResources()
{
  s_state.app_icon_texture = LoadTexture("images/duck.png");

  s_state.fallback_disc_texture = LoadTexture("fullscreenui/media-cdrom.png");
  s_state.fallback_exe_texture = LoadTexture("fullscreenui/applications-system.png");
  s_state.fallback_psf_texture = LoadTexture("fullscreenui/multimedia-player.png");
  s_state.fallback_playlist_texture = LoadTexture("fullscreenui/address-book-new.png");
  return true;
}

void FullscreenUI::DestroyResources()
{
  s_state.app_icon_texture.reset();
  s_state.fallback_playlist_texture.reset();
  s_state.fallback_psf_texture.reset();
  s_state.fallback_exe_texture.reset();
  s_state.fallback_disc_texture.reset();
}

//////////////////////////////////////////////////////////////////////////
// Utility
//////////////////////////////////////////////////////////////////////////

ImGuiFullscreen::FileSelectorFilters FullscreenUI::GetDiscImageFilters()
{
  return {"*.bin",   "*.cue",    "*.iso", "*.img", "*.chd", "*.ecm",     "*.mds", "*.cpe", "*.elf",
          "*.psexe", "*.ps-exe", "*.exe", "*.psx", "*.psf", "*.minipsf", "*.m3u", "*.pbp"};
}

void FullscreenUI::DoStartPath(std::string path, std::string state, std::optional<bool> fast_boot)
{
  if (GPUThread::HasGPUBackend())
    return;

  // Switch to nothing, we'll get called back via OnSystemDestroyed() if startup fails.
  s_state.current_main_window = MainWindowType::None;
  QueueResetFocus(FocusResetType::ViewChanged);
  UpdateRunIdleState();

  SystemBootParameters params;
  params.filename = std::move(path);
  params.save_state = std::move(state);
  params.override_fast_boot = std::move(fast_boot);
  Host::RunOnCPUThread([params = std::move(params)]() {
    if (System::IsValid())
      return;

    Error error;
    if (!System::BootSystem(std::move(params), &error))
    {
      Host::ReportErrorAsync(TRANSLATE_SV("System", "Error"),
                             fmt::format(TRANSLATE_FS("System", "Failed to boot system: {}"), error.GetDescription()));
    }
  });
}

void FullscreenUI::DoResume()
{
  std::string path = System::GetMostRecentResumeSaveStatePath();
  if (path.empty())
  {
    ShowToast({}, FSUI_CSTR("No resume save state found."));
    return;
  }

  SaveStateListEntry slentry;
  if (!InitializeSaveStateListEntryFromPath(&slentry, std::move(path), -1, false))
    return;

  CloseSaveStateSelector();
  s_state.save_state_selector_slots.push_back(std::move(slentry));
  s_state.save_state_selector_game_path = {};
  s_state.save_state_selector_loading = true;
  s_state.save_state_selector_open = true;
  s_state.save_state_selector_resuming = true;
}

void FullscreenUI::DoStartFile()
{
  auto callback = [](const std::string& path) {
    if (!path.empty())
      DoStartPath(path);

    CloseFileSelector();
  };

  OpenFileSelector(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Select Disc Image"), false, std::move(callback),
                   GetDiscImageFilters());
}

void FullscreenUI::DoStartBIOS()
{
  DoStartDisc(std::string());
}

void FullscreenUI::DoStartDisc(std::string path)
{
  Host::RunOnCPUThread([path = std::move(path)]() mutable {
    if (System::IsValid())
      return;

    Error error;
    SystemBootParameters params;
    params.filename = std::move(path);
    if (!System::BootSystem(std::move(params), &error))
    {
      Host::ReportErrorAsync(TRANSLATE_SV("System", "Error"),
                             fmt::format(TRANSLATE_FS("System", "Failed to boot system: {}"), error.GetDescription()));
    }
  });
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
    DoStartDisc(std::move(devices.front().first));
    return;
  }

  ImGuiFullscreen::ChoiceDialogOptions options;
  std::vector<std::string> paths;
  options.reserve(devices.size());
  paths.reserve(paths.size());
  for (auto& [path, name] : devices)
  {
    options.emplace_back(std::move(name), false);
    paths.push_back(std::move(path));
  }
  OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Select Disc Drive"), false, std::move(options),
                   [paths = std::move(paths)](s32 index, const std::string&, bool) mutable {
                     if (index < 0)
                       return;

                     DoStartDisc(std::move(paths[index]));
                     CloseChoiceDialog();
                   });
}

void FullscreenUI::ConfirmIfSavingMemoryCards(std::string_view action, std::function<void(bool)> callback)
{
  if (!System::IsSavingMemoryCards())
  {
    callback(true);
    return;
  }

  OpenConfirmMessageDialog(
    FSUI_ICONSTR(ICON_PF_MEMORY_CARD, "Memory Card Busy"),
    fmt::format(FSUI_FSTR("WARNING: Your game is still saving to the memory card. Continuing to {0} may IRREVERSIBLY "
                          "DESTROY YOUR MEMORY CARD. We recommend resuming your game and waiting 5 seconds for it to "
                          "finish saving.\n\nDo you want to {0} anyway?"),
                action),
    std::move(callback),
    fmt::format(
      fmt::runtime(FSUI_ICONSTR(ICON_FA_EXCLAMATION_TRIANGLE, "Yes, {} now and risk memory card corruption.")), action),
    FSUI_ICONSTR(ICON_FA_PLAY, "No, resume the game."));
}

void FullscreenUI::RequestShutdown(bool save_state)
{
  ConfirmIfSavingMemoryCards(FSUI_VSTR("shut down"), [save_state](bool result) {
    if (result)
      Host::RunOnCPUThread([save_state]() { Host::RequestSystemShutdown(false, save_state); });
    else
      ClosePauseMenu();
  });
}

void FullscreenUI::RequestReset()
{
  ConfirmIfSavingMemoryCards(FSUI_VSTR("reset"), [](bool result) {
    if (result)
      Host::RunOnCPUThread(System::ResetSystem);
    else
      ClosePauseMenu();
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

void FullscreenUI::DoChangeDiscFromFile()
{
  ConfirmIfSavingMemoryCards(FSUI_VSTR("change disc"), [](bool result) {
    if (!result)
    {
      ClosePauseMenu();
      return;
    }

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

      CloseFileSelector();
      ReturnToPreviousWindow();
    };

    OpenFileSelector(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Select Disc Image"), false, std::move(callback),
                     GetDiscImageFilters(), std::string(Path::GetDirectory(System::GetDiscPath())));
  });
}

void FullscreenUI::DoChangeDisc()
{
  Host::RunOnCPUThread([]() {
    ImGuiFullscreen::ChoiceDialogOptions options;

    if (System::HasMediaSubImages())
    {
      const u32 current_index = System::GetMediaSubImageIndex();
      const u32 count = System::GetMediaSubImageCount();
      options.reserve(count + 1);
      options.emplace_back(FSUI_STR("From File..."), false);

      for (u32 i = 0; i < count; i++)
        options.emplace_back(System::GetMediaSubImageTitle(i), i == current_index);

      GPUThread::RunOnThread([options = std::move(options)]() mutable {
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

          CloseChoiceDialog();
          ReturnToPreviousWindow();
        };

        OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Select Disc Image"), true, std::move(options),
                         std::move(callback));
      });

      return;
    }

    if (const GameDatabase::Entry* entry = System::GetGameDatabaseEntry(); entry && !entry->disc_set_serials.empty())
    {
      const auto lock = GameList::GetLock();
      auto matches = GameList::GetMatchingEntriesForSerial(entry->disc_set_serials);
      if (matches.size() > 1)
      {
        options.reserve(matches.size() + 1);
        options.emplace_back(FSUI_STR("From File..."), false);

        std::vector<std::string> paths;
        paths.reserve(matches.size());

        const std::string& current_path = System::GetDiscPath();
        for (auto& [title, glentry] : matches)
        {
          options.emplace_back(std::move(title), current_path == glentry->path);
          paths.push_back(glentry->path);
        }

        GPUThread::RunOnThread([options = std::move(options), paths = std::move(paths)]() mutable {
          auto callback = [paths = std::move(paths)](s32 index, const std::string& title, bool checked) {
            if (index == 0)
            {
              CloseChoiceDialog();
              DoChangeDiscFromFile();
              return;
            }
            else if (index > 0)
            {
              System::InsertMedia(paths[index - 1].c_str());
            }

            CloseChoiceDialog();
            ReturnToMainWindow();
          };

          OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Select Disc Image"), true, std::move(options),
                           std::move(callback));
        });

        return;
      }
    }

    GPUThread::RunOnThread([]() { DoChangeDiscFromFile(); });
  });
}

void FullscreenUI::DoToggleAnalogMode()
{
  // hacky way to toggle analog mode
  Host::RunOnCPUThread([]() {
    for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    {
      Controller* ctrl = System::GetController(i);
      if (!ctrl)
        continue;

      const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(ctrl->GetType());
      if (!cinfo)
        continue;

      for (const Controller::ControllerBindingInfo& bi : cinfo->bindings)
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

void FullscreenUI::SwitchToLanding()
{
  s_state.current_main_window = MainWindowType::Landing;
  QueueResetFocus(FocusResetType::ViewChanged);
}

void FullscreenUI::DrawLandingTemplate(ImVec2* menu_pos, ImVec2* menu_size)
{
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 heading_size =
    ImVec2(io.DisplaySize.x, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) +
                               (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));
  *menu_pos = ImVec2(0.0f, heading_size.y);
  *menu_size = ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y - LayoutScale(LAYOUT_FOOTER_HEIGHT));

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "landing_heading", UIStyle.PrimaryColor))
  {
    ImFont* const heading_font = UIStyle.LargeFont;
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    SmallString heading_str;

    ImGui::PushFont(heading_font);
    ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.PrimaryTextColor);

    // draw branding
    {
      const ImVec2 logo_pos = LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING);
      const ImVec2 logo_size = LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
      dl->AddImage(s_state.app_icon_texture.get(), logo_pos, logo_pos + logo_size);
      dl->AddText(heading_font, heading_font->FontSize,
                  ImVec2(logo_pos.x + logo_size.x + LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING), logo_pos.y),
                  ImGui::GetColorU32(ImGuiCol_Text), "DuckStation");
    }

    // draw time
    ImVec2 time_pos;
    {
      heading_str.format(FSUI_FSTR("{:%H:%M}"), fmt::localtime(std::time(nullptr)));

      const ImVec2 time_size = heading_font->CalcTextSizeA(heading_font->FontSize, FLT_MAX, 0.0f, "00:00");
      time_pos = ImVec2(heading_size.x - LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING) - time_size.x,
                        LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING));
      ImGui::RenderTextClipped(time_pos, time_pos + time_size, heading_str.c_str(), heading_str.end_ptr(), &time_size);
    }

    // draw achievements info
    if (Achievements::IsActive())
    {
      const auto lock = Achievements::GetLock();
      const char* username = Achievements::GetLoggedInUserName();
      if (username)
      {
        const ImVec2 name_size = heading_font->CalcTextSizeA(heading_font->FontSize, FLT_MAX, 0.0f, username);
        const ImVec2 name_pos =
          ImVec2(time_pos.x - name_size.x - LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING), time_pos.y);
        ImGui::RenderTextClipped(name_pos, name_pos + name_size, username, nullptr, &name_size);

        // TODO: should we cache this? heap allocations bad...
        std::string badge_path = Achievements::GetLoggedInUserBadgePath();
        if (!badge_path.empty()) [[likely]]
        {
          const ImVec2 badge_size =
            LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
          const ImVec2 badge_pos =
            ImVec2(name_pos.x - badge_size.x - LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING), time_pos.y);

          dl->AddImage(reinterpret_cast<ImTextureID>(GetCachedTextureAsync(badge_path)), badge_pos,
                       badge_pos + badge_size);
        }
      }
    }

    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  EndFullscreenWindow();
}

void FullscreenUI::DrawLandingWindow()
{
  ImVec2 menu_pos, menu_size;
  DrawLandingTemplate(&menu_pos, &menu_size);

  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.BackgroundTextColor);

  if (BeginHorizontalMenu("landing_window", menu_pos, menu_size, 4))
  {
    ResetFocusHere();

    if (HorizontalMenuItem(GetCachedTexture("fullscreenui/address-book-new.png"), FSUI_CSTR("Game List"),
                           FSUI_CSTR("Launch a game from images scanned from your game directories.")))
    {
      SwitchToGameList();
    }

    if (HorizontalMenuItem(
          GetCachedTexture("fullscreenui/media-cdrom.png"), FSUI_CSTR("Start Game"),
          FSUI_CSTR("Launch a game from a file, disc, or starts the console without any disc inserted.")))
    {
      s_state.current_main_window = MainWindowType::StartGame;
      QueueResetFocus(FocusResetType::ViewChanged);
    }

    if (HorizontalMenuItem(GetCachedTexture("fullscreenui/applications-system.png"), FSUI_CSTR("Settings"),
                           FSUI_CSTR("Changes settings for the application.")))
    {
      SwitchToSettings();
    }

    if (HorizontalMenuItem(GetCachedTexture("fullscreenui/exit.png"), FSUI_CSTR("Exit"),
                           FSUI_CSTR("Return to desktop mode, or exit the application.")) ||
        (!AreAnyDialogsOpen() && WantsToCloseMenu()))
    {
      s_state.current_main_window = MainWindowType::Exit;
      QueueResetFocus(FocusResetType::ViewChanged);
    }
  }
  EndHorizontalMenu();

  ImGui::PopStyleColor();

  if (!AreAnyDialogsOpen())
  {
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) || ImGui::IsKeyPressed(ImGuiKey_F1, false))
      OpenAboutWindow();
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

  if (BeginHorizontalMenu("start_game_window", menu_pos, menu_size, 4))
  {
    ResetFocusHere();

    if (HorizontalMenuItem(GetCachedTexture("fullscreenui/start-file.png"), FSUI_CSTR("Start File"),
                           FSUI_CSTR("Launch a game by selecting a file/disc image.")))
    {
      DoStartFile();
    }

    if (HorizontalMenuItem(GetCachedTexture("fullscreenui/drive-cdrom.png"), FSUI_CSTR("Start Disc"),
                           FSUI_CSTR("Start a game from a disc in your PC's DVD drive.")))
    {
      DoStartDisc();
    }

    if (HorizontalMenuItem(GetCachedTexture("fullscreenui/start-bios.png"), FSUI_CSTR("Start BIOS"),
                           FSUI_CSTR("Start the console without any disc inserted.")))
    {
      DoStartBIOS();
    }

    // https://www.iconpacks.net/free-icon/arrow-back-3783.html
    if (HorizontalMenuItem(GetCachedTexture("fullscreenui/back-icon.png"), FSUI_CSTR("Back"),
                           FSUI_CSTR("Return to the previous menu.")) ||
        (!AreAnyDialogsOpen() && WantsToCloseMenu()))
    {
      s_state.current_main_window = MainWindowType::Landing;
      QueueResetFocus(FocusResetType::ViewChanged);
    }
  }
  EndHorizontalMenu();

  ImGui::PopStyleColor();

  if (!AreAnyDialogsOpen())
  {
    if (ImGui::IsKeyPressed(ImGuiKey_NavGamepadMenu, false) || ImGui::IsKeyPressed(ImGuiKey_F1, false))
      OpenSaveStateSelector(true);
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

  if (BeginHorizontalMenu("exit_window", menu_pos, menu_size, 3))
  {
    ResetFocusHere();

    // https://www.iconpacks.net/free-icon/arrow-back-3783.html
    if (HorizontalMenuItem(GetCachedTexture("fullscreenui/back-icon.png"), FSUI_CSTR("Back"),
                           FSUI_CSTR("Return to the previous menu.")) ||
        WantsToCloseMenu())
    {
      s_state.current_main_window = MainWindowType::Landing;
      QueueResetFocus(FocusResetType::ViewChanged);
    }

    if (HorizontalMenuItem(GetCachedTexture("fullscreenui/exit.png"), FSUI_CSTR("Exit DuckStation"),
                           FSUI_CSTR("Completely exits the application, returning you to your desktop.")))
    {
      DoRequestExit();
    }

    if (HorizontalMenuItem(GetCachedTexture("fullscreenui/desktop-mode.png"), FSUI_CSTR("Desktop Mode"),
                           FSUI_CSTR("Exits Big Picture mode, returning to the desktop interface.")))
    {
      DoDesktopMode();
    }
  }
  EndHorizontalMenu();

  ImGui::PopStyleColor();

  SetStandardSelectionFooterText(true);
}

bool FullscreenUI::ShouldShowAdvancedSettings()
{
  return Host::GetBaseBoolSettingValue("Main", "ShowDebugMenu", false);
}

bool FullscreenUI::IsEditingGameSettings(SettingsInterface* bsi)
{
  return (bsi == s_state.game_settings_interface.get());
}

SettingsInterface* FullscreenUI::GetEditingSettingsInterface()
{
  return s_state.game_settings_interface ? s_state.game_settings_interface.get() :
                                           Host::Internal::GetBaseSettingsLayer();
}

SettingsInterface* FullscreenUI::GetEditingSettingsInterface(bool game_settings)
{
  return (game_settings && s_state.game_settings_interface) ? s_state.game_settings_interface.get() :
                                                              Host::Internal::GetBaseSettingsLayer();
}

void FullscreenUI::SetSettingsChanged(SettingsInterface* bsi)
{
  if (bsi && bsi == s_state.game_settings_interface.get())
    s_state.game_settings_changed.store(true, std::memory_order_release);
  else
    s_state.settings_changed.store(true, std::memory_order_release);
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

TinyString FullscreenUI::GetEffectiveTinyStringSetting(SettingsInterface* bsi, const char* section, const char* key,
                                                       const char* default_value)
{
  TinyString ret;
  std::optional<TinyString> value;

  if (IsEditingGameSettings(bsi))
    value = bsi->GetOptionalTinyStringValue(section, key, std::nullopt);

  if (value.has_value())
    ret = std::move(value.value());
  else
    ret = Host::Internal::GetBaseSettingsLayer()->GetTinyStringValue(section, key, default_value);

  return ret;
}

void FullscreenUI::DrawInputBindingButton(SettingsInterface* bsi, InputBindingInfo::Type type, const char* section,
                                          const char* name, const char* display_name, const char* icon_name,
                                          bool show_type)
{
  if (type == InputBindingInfo::Type::Pointer || type == InputBindingInfo::Type::RelativePointer)
    return;

  TinyString title;
  title.format("{}/{}", section, name);

  SmallString value = bsi->GetSmallStringValue(section, name);
  const bool oneline = value.count('&') <= 1;

  ImRect bb;
  bool visible, hovered, clicked;
  clicked = MenuButtonFrame(title, true,
                            oneline ? ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY :
                                      ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                            &visible, &hovered, &bb.Min, &bb.Max);
  if (!visible)
    return;

  if (oneline && type != InputBindingInfo::Type::Pointer && type != InputBindingInfo::Type::Device)
    InputManager::PrettifyInputBinding(value);

  if (show_type)
  {
    if (icon_name)
    {
      title.format("{} {}", icon_name, display_name);
    }
    else
    {
      switch (type)
      {
        case InputBindingInfo::Type::Button:
          title.format(ICON_FA_DOT_CIRCLE " {}", display_name);
          break;
        case InputBindingInfo::Type::Axis:
        case InputBindingInfo::Type::HalfAxis:
          title.format(ICON_FA_BULLSEYE " {}", display_name);
          break;
        case InputBindingInfo::Type::Motor:
          title.format(ICON_FA_BELL " {}", display_name);
          break;
        case InputBindingInfo::Type::Macro:
          title.format(ICON_FA_PIZZA_SLICE " {}", display_name);
          break;
        case InputBindingInfo::Type::Device:
          title.format(ICON_FA_GAMEPAD " {}", display_name);
          break;
        default:
          title = display_name;
          break;
      }
    }
  }

  const float midpoint = bb.Min.y + UIStyle.LargeFont->FontSize + LayoutScale(4.0f);

  if (oneline)
  {
    ImGui::PushFont(UIStyle.LargeFont);

    const ImVec2 value_size(ImGui::CalcTextSize(value.empty() ? FSUI_CSTR("-") : value.c_str(), nullptr));
    const float text_end = bb.Max.x - value_size.x;
    const ImRect title_bb(bb.Min, ImVec2(text_end, midpoint));

    ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, show_type ? title.c_str() : display_name, nullptr, nullptr,
                             ImVec2(0.0f, 0.0f), &title_bb);
    ImGui::RenderTextClipped(bb.Min, bb.Max, value.empty() ? FSUI_CSTR("-") : value.c_str(), nullptr, &value_size,
                             ImVec2(1.0f, 0.5f), &bb);
    ImGui::PopFont();
  }
  else
  {
    const ImRect title_bb(bb.Min, ImVec2(bb.Max.x, midpoint));
    const ImRect summary_bb(ImVec2(bb.Min.x, midpoint), bb.Max);

    ImGui::PushFont(UIStyle.LargeFont);
    ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, show_type ? title.c_str() : display_name, nullptr, nullptr,
                             ImVec2(0.0f, 0.0f), &title_bb);
    ImGui::PopFont();

    ImGui::PushFont(UIStyle.MediumFont);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, value.empty() ? FSUI_CSTR("No Binding") : value.c_str(),
                             nullptr, nullptr, ImVec2(0.0f, 0.0f), &summary_bb);
    ImGui::PopFont();
  }

  if (clicked)
  {
    BeginInputBinding(bsi, type, section, name, display_name);
  }
  else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_NavGamepadMenu, false))
  {
    bsi->DeleteValue(section, name);
    SetSettingsChanged(bsi);
  }
}

void FullscreenUI::ClearInputBindingVariables()
{
  s_state.input_binding_type = InputBindingInfo::Type::Unknown;
  s_state.input_binding_section = {};
  s_state.input_binding_key = {};
  s_state.input_binding_display_name = {};
  s_state.input_binding_new_bindings = {};
  s_state.input_binding_value_ranges = {};
}

void FullscreenUI::BeginInputBinding(SettingsInterface* bsi, InputBindingInfo::Type type, std::string_view section,
                                     std::string_view key, std::string_view display_name)
{
  if (s_state.input_binding_type != InputBindingInfo::Type::Unknown)
  {
    InputManager::RemoveHook();
    ClearInputBindingVariables();
  }

  s_state.input_binding_type = type;
  s_state.input_binding_section = section;
  s_state.input_binding_key = key;
  s_state.input_binding_display_name = display_name;
  s_state.input_binding_new_bindings = {};
  s_state.input_binding_value_ranges = {};
  s_state.input_binding_timer.Reset();

  const bool game_settings = IsEditingGameSettings(bsi);

  InputManager::SetHook([game_settings](InputBindingKey key, float value) -> InputInterceptHook::CallbackResult {
    // shouldn't happen, just in case
    if (s_state.input_binding_type == InputBindingInfo::Type::Unknown)
      return InputInterceptHook::CallbackResult::RemoveHookAndContinueProcessingEvent;

    // holding the settings lock here will protect the input binding list
    auto lock = Host::GetSettingsLock();

    float initial_value = value;
    float min_value = value;
    InputInterceptHook::CallbackResult default_action = InputInterceptHook::CallbackResult::StopProcessingEvent;
    const auto it = std::find_if(s_state.input_binding_value_ranges.begin(), s_state.input_binding_value_ranges.end(),
                                 [key](const auto& it) { return it.first.bits == key.bits; });

    if (it != s_state.input_binding_value_ranges.end())
    {
      initial_value = it->second.first;
      min_value = it->second.second = std::min(it->second.second, value);
    }
    else
    {
      s_state.input_binding_value_ranges.emplace_back(key, std::make_pair(initial_value, min_value));

      // forward the event to imgui if it's a new key and a release, because this is what triggered the binding to start
      // if we don't do this, imgui thinks the activate button is held down
      default_action = (value == 0.0f) ? InputInterceptHook::CallbackResult::ContinueProcessingEvent :
                                         InputInterceptHook::CallbackResult::StopProcessingEvent;
    }

    const float abs_value = std::abs(value);
    const bool reverse_threshold = (key.source_subtype == InputSubclass::ControllerAxis && initial_value > 0.5f);

    for (InputBindingKey& other_key : s_state.input_binding_new_bindings)
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
            s_state.input_binding_type, s_state.input_binding_new_bindings.data(),
            s_state.input_binding_new_bindings.size()));
          bsi->SetStringValue(s_state.input_binding_section.c_str(), s_state.input_binding_key.c_str(),
                              new_binding.c_str());
          SetSettingsChanged(bsi);
          ClearInputBindingVariables();
          QueueResetFocus(FocusResetType::PopupClosed);
          return InputInterceptHook::CallbackResult::RemoveHookAndStopProcessingEvent;
        }

        // otherwise, keep waiting
        return default_action;
      }
    }

    // new binding, add it to the list, but wait for a decent distance first, and then wait for release
    if ((reverse_threshold ? (abs_value < 0.5f) : (abs_value >= 0.5f)))
    {
      InputBindingKey key_to_add = key;
      key_to_add.modifier = (value < 0.0f && !reverse_threshold) ? InputModifier::Negate : InputModifier::None;
      key_to_add.invert = reverse_threshold;
      s_state.input_binding_new_bindings.push_back(key_to_add);
    }

    return default_action;
  });
}

void FullscreenUI::DrawInputBindingWindow()
{
  DebugAssert(s_state.input_binding_type != InputBindingInfo::Type::Unknown);

  const double time_remaining = INPUT_BINDING_TIMEOUT_SECONDS - s_state.input_binding_timer.GetTimeSeconds();
  if (time_remaining <= 0.0)
  {
    InputManager::RemoveHook();
    ClearInputBindingVariables();
    QueueResetFocus(FocusResetType::PopupClosed);
    return;
  }

  const char* title = FSUI_ICONSTR(ICON_FA_GAMEPAD, "Set Input Binding");
  ImGui::SetNextWindowSize(LayoutScale(500.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup(title);

  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  if (ImGui::BeginPopupModal(title, nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse |
                               ImGuiWindowFlags_NoCollapse))
  {
    ImGui::TextWrapped("%s", SmallString::from_format(FSUI_FSTR("Setting {} binding {}."),
                                                      s_state.input_binding_section, s_state.input_binding_display_name)
                               .c_str());
    ImGui::TextUnformatted(FSUI_CSTR("Push a controller button or axis now."));
    ImGui::NewLine();
    ImGui::TextUnformatted(SmallString::from_format(FSUI_FSTR("Timing out in {:.0f} seconds..."), time_remaining));
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
                                      const char* const* options, size_t option_count, bool translate_options,
                                      int option_offset, bool enabled, float height, ImFont* font, ImFont* summary_font,
                                      const char* tr_context)
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
      ((index < 0 || static_cast<size_t>(index) >= option_count) ?
         FSUI_CSTR("Unknown") :
         (translate_options ? Host::TranslateToCString(tr_context, options[index]) : options[index])) :
      FSUI_CSTR("Use Global Setting");

  if (MenuButtonWithValue(title, summary, value_text, enabled, height, font, summary_font))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(option_count + 1);
    if (game_settings)
      cd_options.emplace_back(FSUI_STR("Use Global Setting"), !value.has_value());
    for (size_t i = 0; i < option_count; i++)
    {
      cd_options.emplace_back(translate_options ? Host::TranslateToString(tr_context, options[i]) :
                                                  std::string(options[i]),
                              (i == static_cast<size_t>(index)));
    }
    OpenChoiceDialog(title, false, std::move(cd_options),
                     [game_settings, section = TinyString(section), key = TinyString(key),
                      option_offset](s32 index, const std::string& title, bool checked) {
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
  const SmallString value_text =
    value.has_value() ? SmallString::from_sprintf(format, value.value()) : SmallString(FSUI_VSTR("Use Global Setting"));

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled, height, font, summary_font))
    ImGui::OpenPopup(title);

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 194.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushFont(UIStyle.LargeFont);
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
    if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, UIStyle.LargeFont,
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
  const SmallString value_text = value.has_value() ? SmallString::from_sprintf(format, value.value() * multiplier) :
                                                     SmallString(FSUI_VSTR("Use Global Setting"));

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled, height, font, summary_font))
    ImGui::OpenPopup(title);

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 194.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushFont(UIStyle.LargeFont);
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
    if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, UIStyle.LargeFont,
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
  const SmallString value_text = value.has_value() ? SmallString::from_sprintf(format, value.value() * multiplier) :
                                                     SmallString(FSUI_VSTR("Use Global Setting"));

  static bool manual_input = false;

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled, height, font, summary_font))
  {
    ImGui::OpenPopup(title);
    manual_input = false;
  }

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 194.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushFont(UIStyle.LargeFont);
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
        ((LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + padding.y * 2.0f) - UIStyle.LargeFont->FontSize) * 0.5f);
      ImGui::TextUnformatted(str_value);

      float step = 0;
      if (FloatingButton(ICON_FA_CHEVRON_UP, padding.x, button_pos.y, -1.0f, -1.0f, 1.0f, 0.0f, true, UIStyle.LargeFont,
                         &button_pos, true))
      {
        step = step_value;
      }
      if (FloatingButton(ICON_FA_CHEVRON_DOWN, button_pos.x - padding.x, button_pos.y, -1.0f, -1.0f, -1.0f, 0.0f, true,
                         UIStyle.LargeFont, &button_pos, true))
      {
        step = -step_value;
      }
      if (FloatingButton(ICON_FA_KEYBOARD, button_pos.x - padding.x, button_pos.y, -1.0f, -1.0f, -1.0f, 0.0f, true,
                         UIStyle.LargeFont, &button_pos))
      {
        manual_input = true;
      }
      if (FloatingButton(ICON_FA_TRASH, button_pos.x - padding.x, button_pos.y, -1.0f, -1.0f, -1.0f, 0.0f, true,
                         UIStyle.LargeFont, &button_pos))
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

    if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, UIStyle.LargeFont,
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
  const SmallString value_text = SmallString::from_format(
    "{}/{}/{}/{}",
    left_value.has_value() ? TinyString::from_sprintf(format, left_value.value()) : TinyString(FSUI_VSTR("Default")),
    top_value.has_value() ? TinyString::from_sprintf(format, top_value.value()) : TinyString(FSUI_VSTR("Default")),
    right_value.has_value() ? TinyString::from_sprintf(format, right_value.value()) : TinyString(FSUI_VSTR("Default")),
    bottom_value.has_value() ? TinyString::from_sprintf(format, bottom_value.value()) : TinyString(FSUI_VSTR("Default")));

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
    value_text.sprintf(format, value.value());
  else
    value_text = FSUI_VSTR("Use Global Setting");

  static bool manual_input = false;

  if (MenuButtonWithValue(title, summary, value_text, enabled, height, font, summary_font))
  {
    ImGui::OpenPopup(title);
    manual_input = false;
  }

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 194.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushFont(UIStyle.LargeFont);
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
        ((LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + padding.y * 2.0f) - UIStyle.LargeFont->FontSize) * 0.5f);
      ImGui::TextUnformatted(str_value);

      s32 step = 0;
      if (FloatingButton(ICON_FA_CHEVRON_UP, padding.x, button_pos.y, -1.0f, -1.0f, 1.0f, 0.0f, true, UIStyle.LargeFont,
                         &button_pos, true))
      {
        step = step_value;
      }
      if (FloatingButton(ICON_FA_CHEVRON_DOWN, button_pos.x - padding.x, button_pos.y, -1.0f, -1.0f, -1.0f, 0.0f, true,
                         UIStyle.LargeFont, &button_pos, true))
      {
        step = -step_value;
      }
      if (FloatingButton(ICON_FA_KEYBOARD, button_pos.x - padding.x, button_pos.y, -1.0f, -1.0f, -1.0f, 0.0f, true,
                         UIStyle.LargeFont, &button_pos))
      {
        manual_input = true;
      }
      if (FloatingButton(ICON_FA_TRASH, button_pos.x - padding.x, button_pos.y, -1.0f, -1.0f, -1.0f, 0.0f, true,
                         UIStyle.LargeFont, &button_pos))
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

    if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, UIStyle.LargeFont,
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
  const std::optional<SmallString> value(bsi->GetOptionalSmallStringValue(
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
  const std::optional<SmallString> value(bsi->GetOptionalSmallStringValue(
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
    OpenChoiceDialog(title, false, std::move(cd_options),
                     [section = TinyString(section), key = TinyString(key), to_string_function,
                      game_settings](s32 index, const std::string& title, bool checked) {
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
                                        bool translate_options, bool enabled, float height, ImFont* font,
                                        ImFont* summary_font)
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

  if (MenuButtonWithValue(
        title, summary,
        value.has_value() ?
          ((index < option_count) ?
             (translate_options ? Host::TranslateToCString(TR_CONTEXT, options[index]) : options[index]) :
             FSUI_CSTR("Unknown")) :
          FSUI_CSTR("Use Global Setting"),
        enabled, height, font, summary_font))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(option_count + 1);
    if (game_settings)
      cd_options.emplace_back(FSUI_CSTR("Use Global Setting"), !value.has_value());
    for (size_t i = 0; i < option_count; i++)
    {
      cd_options.emplace_back(translate_options ? Host::TranslateToString(TR_CONTEXT, options[i]) :
                                                  std::string(options[i]),
                              (value.has_value() && i == static_cast<size_t>(index)));
    }
    OpenChoiceDialog(title, false, std::move(cd_options),
                     [game_settings, section = TinyString(section), key = TinyString(key),
                      option_values](s32 index, const std::string& title, bool checked) {
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
                     [game_settings = IsEditingGameSettings(bsi), section = TinyString(section),
                      key = TinyString(key)](const std::string& dir) {
                       if (dir.empty())
                         return;

                       auto lock = Host::GetSettingsLock();
                       SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                       std::string relative_path(Path::MakeRelative(dir, EmuFolders::DataRoot));
                       bsi->SetStringValue(section.c_str(), key.c_str(), relative_path.c_str());
                       SetSettingsChanged(bsi);

                       Host::RunOnCPUThread(&EmuFolders::Update);
                       s_state.cover_image_map.clear();

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
  s_state.game_settings_entry.reset();
  s_state.game_settings_interface.reset();
  s_state.game_patch_list = {};
  s_state.enabled_game_patch_cache = {};
  s_state.game_cheats_list = {};
  s_state.enabled_game_cheat_cache = {};
  s_state.game_cheat_groups = {};

  PopulateGraphicsAdapterList();
  PopulatePostProcessingChain(GetEditingSettingsInterface(), PostProcessing::Config::DISPLAY_CHAIN_SECTION);

  s_state.current_main_window = MainWindowType::Settings;
  s_state.settings_page = SettingsPage::Interface;
}

void FullscreenUI::SwitchToGameSettingsForSerial(std::string_view serial)
{
  s_state.game_settings_entry.reset();
  s_state.game_settings_interface = std::make_unique<INISettingsInterface>(System::GetGameSettingsPath(serial));
  s_state.game_settings_interface->Load();
  PopulatePatchesAndCheatsList(serial);
  s_state.current_main_window = MainWindowType::Settings;
  s_state.settings_page = SettingsPage::Summary;
  QueueResetFocus(FocusResetType::ViewChanged);
}

bool FullscreenUI::SwitchToGameSettings()
{
  if (System::GetGameSerial().empty())
    return false;

  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(System::GetDiscPath());
  if (!entry)
  {
    SwitchToGameSettingsForSerial(System::GetGameSerial());
    return true;
  }
  else
  {
    SwitchToGameSettings(entry);
    return true;
  }
}

bool FullscreenUI::SwitchToGameSettingsForPath(const std::string& path)
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(path);
  if (!entry)
    return false;

  SwitchToGameSettings(entry);
  return true;
}

void FullscreenUI::SwitchToGameSettings(const GameList::Entry* entry)
{
  SwitchToGameSettingsForSerial(entry->serial);
  s_state.game_settings_entry = std::make_unique<GameList::Entry>(*entry);
}

void FullscreenUI::PopulateGraphicsAdapterList()
{
  const GPURenderer renderer =
    Settings::ParseRendererName(GetEffectiveTinyStringSetting(GetEditingSettingsInterface(false), "GPU", "Renderer",
                                                              Settings::GetRendererName(Settings::DEFAULT_GPU_RENDERER))
                                  .c_str())
      .value_or(Settings::DEFAULT_GPU_RENDERER);

  s_state.graphics_adapter_list_cache = GPUDevice::GetAdapterListForAPI(Settings::GetRenderAPIForRenderer(renderer));
}

void FullscreenUI::PopulateGameListDirectoryCache(SettingsInterface* si)
{
  s_state.game_list_directories_cache.clear();
  for (std::string& dir : si->GetStringList("GameList", "Paths"))
    s_state.game_list_directories_cache.emplace_back(std::move(dir), false);
  for (std::string& dir : si->GetStringList("GameList", "RecursivePaths"))
    s_state.game_list_directories_cache.emplace_back(std::move(dir), true);
}

void FullscreenUI::PopulatePatchesAndCheatsList(const std::string_view serial)
{
  s_state.game_patch_list = Cheats::GetCodeInfoList(serial, std::nullopt, false, true, true);
  s_state.game_cheats_list = Cheats::GetCodeInfoList(
    serial, std::nullopt, true, s_state.game_settings_interface->GetBoolValue("Cheats", "LoadCheatsFromDatabase", true),
    true);
  s_state.game_cheat_groups = Cheats::GetCodeListUniquePrefixes(s_state.game_cheats_list, true);
  s_state.enabled_game_patch_cache =
    s_state.game_settings_interface->GetStringList(Cheats::PATCHES_CONFIG_SECTION, Cheats::PATCH_ENABLE_CONFIG_KEY);
  s_state.enabled_game_cheat_cache =
    s_state.game_settings_interface->GetStringList(Cheats::CHEATS_CONFIG_SECTION, Cheats::PATCH_ENABLE_CONFIG_KEY);
}

void FullscreenUI::DoCopyGameSettings()
{
  if (!s_state.game_settings_interface)
    return;

  Settings temp_settings;
  temp_settings.Load(*GetEditingSettingsInterface(false), *GetEditingSettingsInterface(false));
  temp_settings.Save(*s_state.game_settings_interface, true);
  SetSettingsChanged(s_state.game_settings_interface.get());

  ShowToast("Game Settings Copied", fmt::format(FSUI_FSTR("Game settings initialized with global settings for '{}'."),
                                                Path::GetFileTitle(s_state.game_settings_interface->GetFileName())));
}

void FullscreenUI::DoClearGameSettings()
{
  if (!s_state.game_settings_interface)
    return;

  s_state.game_settings_interface->Clear();
  if (!s_state.game_settings_interface->GetFileName().empty())
    FileSystem::DeleteFile(s_state.game_settings_interface->GetFileName().c_str());

  SetSettingsChanged(s_state.game_settings_interface.get());

  ShowToast("Game Settings Cleared", fmt::format(FSUI_FSTR("Game settings have been cleared for '{}'."),
                                                 Path::GetFileTitle(s_state.game_settings_interface->GetFileName())));
}

void FullscreenUI::DrawSettingsWindow()
{
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 heading_size =
    ImVec2(io.DisplaySize.x, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) +
                               (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));

  const float bg_alpha =
    GPUThread::HasGPUBackend() ? (s_state.settings_page == SettingsPage::PostProcessing ? 0.50f : 0.90f) : 1.0f;

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "settings_category",
                            ImVec4(UIStyle.PrimaryColor.x, UIStyle.PrimaryColor.y, UIStyle.PrimaryColor.z, bg_alpha)))
  {
    static constexpr float ITEM_WIDTH = 25.0f;

    static constexpr const SettingsPage global_pages[] = {
      SettingsPage::Interface, SettingsPage::Console,        SettingsPage::Emulation,    SettingsPage::BIOS,
      SettingsPage::Graphics,  SettingsPage::PostProcessing, SettingsPage::Audio,        SettingsPage::Controller,
      SettingsPage::Hotkey,    SettingsPage::MemoryCards,    SettingsPage::Achievements, SettingsPage::Advanced};
    static constexpr const SettingsPage per_game_pages[] = {
      SettingsPage::Summary,     SettingsPage::Console,      SettingsPage::Emulation, SettingsPage::Patches,
      SettingsPage::Cheats,      SettingsPage::Graphics,     SettingsPage::Audio,     SettingsPage::Controller,
      SettingsPage::MemoryCards, SettingsPage::Achievements, SettingsPage::Advanced};
    static constexpr std::array<std::pair<const char*, const char*>, static_cast<u32>(SettingsPage::Count)> titles = {
      {{FSUI_NSTR("Summary"), ICON_FA_FILE_ALT},
       {FSUI_NSTR("Interface Settings"), ICON_FA_TV},
       {FSUI_NSTR("Console Settings"), ICON_FA_DICE_D20},
       {FSUI_NSTR("Emulation Settings"), ICON_FA_COGS},
       {FSUI_NSTR("BIOS Settings"), ICON_PF_MICROCHIP},
       {FSUI_NSTR("Controller Settings"), ICON_PF_GAMEPAD_ALT},
       {FSUI_NSTR("Hotkey Settings"), ICON_PF_KEYBOARD_ALT},
       {FSUI_NSTR("Memory Card Settings"), ICON_PF_MEMORY_CARD},
       {FSUI_NSTR("Graphics Settings"), ICON_PF_PICTURE},
       {FSUI_NSTR("Post-Processing Settings"), ICON_FA_MAGIC},
       {FSUI_NSTR("Audio Settings"), ICON_PF_SOUND},
       {FSUI_NSTR("Achievements Settings"), ICON_FA_TROPHY},
       {FSUI_NSTR("Advanced Settings"), ICON_FA_EXCLAMATION_TRIANGLE},
       {FSUI_NSTR("Patches"), ICON_FA_BAND_AID},
       {FSUI_NSTR("Cheats"), ICON_FA_FLASK}}};

    const bool game_settings = IsEditingGameSettings(GetEditingSettingsInterface());
    const u32 count =
      (game_settings ? static_cast<u32>(std::size(per_game_pages)) : static_cast<u32>(std::size(global_pages))) -
      BoolToUInt32(!ShouldShowAdvancedSettings());
    const SettingsPage* pages = game_settings ? per_game_pages : global_pages;
    u32 index = 0;
    for (u32 i = 0; i < count; i++)
    {
      if (pages[i] == s_state.settings_page)
      {
        index = i;
        break;
      }
    }

    BeginNavBar();

    if (!ImGui::IsPopupOpen(0u, ImGuiPopupFlags_AnyPopup))
    {
      if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) ||
          ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakSlow, true) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
      {
        index = (index == 0) ? (count - 1) : (index - 1);
        s_state.settings_page = pages[index];
        QueueResetFocus(FocusResetType::Other);
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) ||
               ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakFast, true) ||
               ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
      {
        index = (index + 1) % count;
        s_state.settings_page = pages[index];
        QueueResetFocus(FocusResetType::Other);
      }
    }

    if (NavButton(ICON_FA_BACKWARD, true, true))
      ReturnToPreviousWindow();

    if (s_state.game_settings_entry)
      NavTitle(s_state.game_settings_entry->title.c_str());
    else
      NavTitle(Host::TranslateToCString(TR_CONTEXT, titles[static_cast<u32>(pages[index])].first));

    RightAlignNavButtons(count, ITEM_WIDTH, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    for (u32 i = 0; i < count; i++)
    {
      if (NavButton(titles[static_cast<u32>(pages[i])].second, i == index, true, ITEM_WIDTH,
                    LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
      {
        s_state.settings_page = pages[i];
        QueueResetFocus(FocusResetType::Other);
      }
    }

    EndNavBar();
  }

  EndFullscreenWindow();

  // we have to do this here, because otherwise it uses target, and jumps a frame later.
  // don't do it for popups opening/closing, otherwise we lose our position
  if (IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  if (BeginFullscreenWindow(
        ImVec2(0.0f, heading_size.y),
        ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y - LayoutScale(LAYOUT_FOOTER_HEIGHT)),
        TinyString::from_format("settings_page_{}", static_cast<u32>(s_state.settings_page)).c_str(),
        ImVec4(UIStyle.BackgroundColor.x, UIStyle.BackgroundColor.y, UIStyle.BackgroundColor.z, bg_alpha), 0.0f,
        ImVec2(ImGuiFullscreen::LAYOUT_MENU_WINDOW_X_PADDING, 0.0f)))
  {
    ResetFocusHere();

    if (ImGui::IsWindowFocused() && WantsToCloseMenu())
      ReturnToPreviousWindow();

    auto lock = Host::GetSettingsLock();

    switch (s_state.settings_page)
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

      case SettingsPage::Graphics:
        DrawGraphicsSettingsPage();
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

      case SettingsPage::Patches:
        DrawPatchesOrCheatsSettingsPage(false);
        break;

      case SettingsPage::Cheats:
        DrawPatchesOrCheatsSettingsPage(true);
        break;

      default:
        break;
    }
  }

  EndFullscreenWindow();

  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_XBOX_DPAD_LEFT_RIGHT, FSUI_VSTR("Change Page")),
                                       std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, FSUI_VSTR("Navigate")),
                                       std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Back"))});
  }
  else
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT, FSUI_VSTR("Change Page")),
                                       std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, FSUI_VSTR("Navigate")),
                                       std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_ESC, FSUI_VSTR("Back"))});
  }
}

void FullscreenUI::DrawSummarySettingsPage()
{
  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Details"));

  if (s_state.game_settings_entry)
  {
    if (MenuButton(FSUI_ICONSTR(ICON_FA_WINDOW_MAXIMIZE, "Title"), s_state.game_settings_entry->title.c_str(), true))
      CopyTextToClipboard(FSUI_STR("Game title copied to clipboard."), s_state.game_settings_entry->title);
    if (MenuButton(FSUI_ICONSTR(ICON_FA_PAGER, "Serial"), s_state.game_settings_entry->serial.c_str(), true))
      CopyTextToClipboard(FSUI_STR("Game serial copied to clipboard."), s_state.game_settings_entry->serial);
    if (MenuButton(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Type"),
                   GameList::GetEntryTypeDisplayName(s_state.game_settings_entry->type), true))
    {
      CopyTextToClipboard(FSUI_STR("Game type copied to clipboard."),
                          GameList::GetEntryTypeDisplayName(s_state.game_settings_entry->type));
    }
    if (MenuButton(FSUI_ICONSTR(ICON_FA_BOX, "Region"),
                   Settings::GetDiscRegionDisplayName(s_state.game_settings_entry->region), true))
    {
      CopyTextToClipboard(FSUI_STR("Game region copied to clipboard."),
                          Settings::GetDiscRegionDisplayName(s_state.game_settings_entry->region));
    }
    if (MenuButton(FSUI_ICONSTR(ICON_FA_STAR, "Compatibility Rating"),
                   GameDatabase::GetCompatibilityRatingDisplayName(
                     s_state.game_settings_entry->dbentry ? s_state.game_settings_entry->dbentry->compatibility :
                                                            GameDatabase::CompatibilityRating::Unknown),
                   true))
    {
      CopyTextToClipboard(FSUI_STR("Game compatibility rating copied to clipboard."),
                          GameDatabase::GetCompatibilityRatingDisplayName(
                            s_state.game_settings_entry->dbentry ? s_state.game_settings_entry->dbentry->compatibility :
                                                                   GameDatabase::CompatibilityRating::Unknown));
    }
    if (MenuButton(FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Path"), s_state.game_settings_entry->path.c_str(), true))
    {
      CopyTextToClipboard(FSUI_STR("Game path copied to clipboard."), s_state.game_settings_entry->path);
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
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_GAMEPAD, "Pause On Controller Disconnection"),
                    FSUI_CSTR("Pauses the emulator when a controller with bindings is disconnected."), "Main",
                    "PauseOnControllerDisconnection", false);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_POWER_OFF, "Confirm Power Off"),
    FSUI_CSTR("Determines whether a prompt will be displayed to confirm shutting down the emulator/game "
              "when the hotkey is pressed."),
    "Main", "ConfirmPowerOff", true);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_SAVE, "Save State On Exit"),
                    FSUI_CSTR("Automatically saves the emulator state when powering down or exiting. You can then "
                              "resume directly from where you left off next time."),
                    "Main", "SaveStateOnExit", true);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_FILE_EXPORT, "Create Save State Backups"),
                    FSUI_CSTR("Renames existing save states when saving to a backup file."), "Main",
                    "CreateSaveStateBackups", false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_WINDOW_MAXIMIZE, "Start Fullscreen"),
                    FSUI_CSTR("Automatically switches to fullscreen mode when the program is started."), "Main",
                    "StartFullscreen", false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_MOUSE, "Double-Click Toggles Fullscreen"),
                    FSUI_CSTR("Switches between full screen and windowed when the window is double-clicked."), "Main",
                    "DoubleClickTogglesFullscreen", true);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_MOUSE_POINTER, "Hide Cursor In Fullscreen"),
                    FSUI_CSTR("Hides the mouse pointer/cursor when the emulator is in fullscreen mode."), "Main",
                    "HideCursorInFullscreen", true);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_MAGIC, "Inhibit Screensaver"),
    FSUI_CSTR("Prevents the screen saver from activating and the host from sleeping while emulation is running."),
    "Main", "InhibitScreensaver", true);

  if (DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_PAINT_BRUSH, "Use Light Theme"),
                        FSUI_CSTR("Uses a light coloured theme instead of the default dark theme."), "Main",
                        "UseLightFullscreenUITheme", false))
  {
    ImGuiFullscreen::SetTheme(bsi->GetBoolValue("Main", "UseLightFullscreenUITheme", false));
  }

  if (DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_LIST, "Smooth Scrolling"),
                        FSUI_CSTR("Enables smooth scrolling of menus in Big Picture UI."), "Main",
                        "FullscreenUISmoothScrolling", true))
  {
    ImGuiFullscreen::SetSmoothScrolling(bsi->GetBoolValue("Main", "FullscreenUISmoothScrolling", false));
  }

  {
    // Have to do this the annoying way, because it's host-derived.
    const auto language_list = Host::GetAvailableLanguageList();
    TinyString current_language = bsi->GetTinyStringValue("Main", "Language", "");
    const char* current_language_name = "Unknown";
    for (const auto& [language, code] : language_list)
    {
      if (current_language == code)
        current_language_name = language;
    }
    if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_LANGUAGE, "UI Language"),
                            FSUI_CSTR("Chooses the language used for UI elements."), current_language_name))
    {
      ImGuiFullscreen::ChoiceDialogOptions options;
      for (const auto& [language, code] : language_list)
        options.emplace_back(fmt::format("{} [{}]", language, code), (current_language == code));
      OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_LANGUAGE, "UI Language"), false, std::move(options),
                       [language_list](s32 index, const std::string& title, bool checked) {
                         if (static_cast<u32>(index) >= language_list.size())
                           return;

                         Host::RunOnCPUThread(
                           [language = language_list[index].second]() { Host::ChangeLanguage(language); });
                         ImGuiFullscreen::CloseChoiceDialog();
                       });
    }
  }

  MenuHeading(FSUI_CSTR("Integration"));
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_CHARGING_STATION, "Enable Discord Presence"),
                    FSUI_CSTR("Shows the game you are currently playing as part of your profile in Discord."), "Main",
                    "EnableDiscordPresence", false);

  MenuHeading(FSUI_CSTR("On-Screen Display"));
  DrawIntSpinBoxSetting(bsi, FSUI_ICONSTR(ICON_FA_SEARCH, "OSD Scale"),
                        FSUI_CSTR("Determines how large the on-screen messages and monitor are."), "Display",
                        "OSDScale", 100, 25, 500, 1, "%d%%");
  DrawFloatSpinBoxSetting(bsi, FSUI_ICONSTR(ICON_FA_RULER, "Screen Margins"),
                          FSUI_CSTR("Determines the margin between the edge of the screen and on-screen messages."),
                          "Display", "OSDMargin", ImGuiManager::DEFAULT_SCREEN_MARGIN, 0.0f, 100.0f, 1.0f, 1.0f,
                          "%.0fpx");
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_LIST, "Show OSD Messages"),
                    FSUI_CSTR("Shows on-screen-display messages when events occur."), "Display", "ShowOSDMessages",
                    true);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_PLAY_CIRCLE, "Show Status Indicators"),
                    FSUI_CSTR("Shows persistent icons when turbo is active or when paused."), "Display",
                    "ShowStatusIndicators", true);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_SIGNAL, "Show Speed"),
    FSUI_CSTR(
      "Shows the current emulation speed of the system in the top-right corner of the display as a percentage."),
    "Display", "ShowSpeed", false);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_STOPWATCH, "Show FPS"),
    FSUI_CSTR("Shows the number of frames (or v-syncs) displayed per second by the system in the top-right "
              "corner of the display."),
    "Display", "ShowFPS", false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_BARS, "Show GPU Statistics"),
                    FSUI_CSTR("Shows information about the emulated GPU in the top-right corner of the display."),
                    "Display", "ShowGPUStatistics", false);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_COGS, "Show Latency Statistics"),
    FSUI_CSTR("Shows information about input and audio latency in the top-right corner of the display."), "Display",
    "ShowLatencyStatistics", false);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_BATTERY_HALF, "Show CPU Usage"),
    FSUI_CSTR("Shows the host's CPU usage of each system thread in the top-right corner of the display."), "Display",
    "ShowCPU", false);
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
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_CHART_LINE, "Show Enhancement Settings"),
                    FSUI_CSTR("Shows enhancement settings in the bottom-right corner of the screen."), "Display",
                    "ShowEnhancements", false);

  EndMenuButtons();
}

void FullscreenUI::DrawBIOSSettingsPage()
{
  static constexpr const std::array config_keys = {"", "PathNTSCJ", "PathNTSCU", "PathPAL"};

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
    title.assign(ICON_FA_MICROCHIP " ");
    title.append_format(FSUI_FSTR("BIOS for {}"), Settings::GetConsoleRegionDisplayName(region));

    const std::optional<SmallString> filename(bsi->GetOptionalSmallStringValue(
      "BIOS", config_keys[i], game_settings ? std::nullopt : std::optional<const char*>("")));

    if (MenuButtonWithValue(title,
                            TinyString::from_format(FSUI_FSTR("BIOS to use when emulating {} consoles."),
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

  DrawFolderSetting(bsi, FSUI_ICONSTR(ICON_FA_FOLDER, "BIOS Directory"), "BIOS", "SearchDirectory", EmuFolders::Bios);

  MenuHeading(FSUI_CSTR("Patches"));

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_BOLT, "Enable Fast Boot"),
                    FSUI_CSTR("Patches the BIOS to skip the boot animation. Safe to enable."), "BIOS", "PatchFastBoot",
                    Settings::DEFAULT_FAST_BOOT_VALUE);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_FAST_FORWARD, "Fast Forward Boot"),
                    FSUI_CSTR("Fast forwards through the early loading process when fast booting, saving time. Results "
                              "may vary between games."),
                    "BIOS", "FastForwardBoot", false,
                    GetEffectiveBoolSetting(bsi, "BIOS", "PatchFastBoot", Settings::DEFAULT_FAST_BOOT_VALUE));
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_SCROLL, "Enable TTY Logging"),
                    FSUI_CSTR("Logs BIOS calls to printf(). Not all games contain debugging messages."), "BIOS",
                    "TTYLogging", false);

  EndMenuButtons();
}

void FullscreenUI::DrawConsoleSettingsPage()
{
  static constexpr const std::array cdrom_read_speeds = {
    FSUI_NSTR("None (Double Speed)"), FSUI_NSTR("2x (Quad Speed)"), FSUI_NSTR("3x (6x Speed)"),
    FSUI_NSTR("4x (8x Speed)"),       FSUI_NSTR("5x (10x Speed)"),  FSUI_NSTR("6x (12x Speed)"),
    FSUI_NSTR("7x (14x Speed)"),      FSUI_NSTR("8x (16x Speed)"),  FSUI_NSTR("9x (18x Speed)"),
    FSUI_NSTR("10x (20x Speed)"),
  };

  static constexpr const std::array cdrom_seek_speeds = {
    FSUI_NSTR("Infinite/Instantaneous"),
    FSUI_NSTR("None (Normal Speed)"),
    FSUI_NSTR("2x"),
    FSUI_NSTR("3x"),
    FSUI_NSTR("4x"),
    FSUI_NSTR("5x"),
    FSUI_NSTR("6x"),
    FSUI_NSTR("7x"),
    FSUI_NSTR("8x"),
    FSUI_NSTR("9x"),
    FSUI_NSTR("10x"),
  };

  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Console Settings"));

  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_GLOBE, "Region"), FSUI_CSTR("Determines the emulated hardware type."),
                  "Console", "Region", Settings::DEFAULT_CONSOLE_REGION, &Settings::ParseConsoleRegionName,
                  &Settings::GetConsoleRegionName, &Settings::GetConsoleRegionDisplayName, ConsoleRegion::Count);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_MAGIC, "Safe Mode"),
                    FSUI_CSTR("Temporarily disables all enhancements, useful when testing."), "Main",
                    "DisableAllEnhancements", false);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_MEMORY, "Enable 8MB RAM"),
    FSUI_CSTR("Enables an additional 6MB of RAM to obtain a total of 2+6 = 8MB, usually present on dev consoles."),
    "Console", "Enable8MBRAM", false);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_FROWN, "Enable Cheats"),
    FSUI_CSTR("Automatically loads and applies cheats on game start. Cheats can break games and saves."), "Console",
    "EnableCheats", false);

  MenuHeading(FSUI_CSTR("CPU Emulation"));

  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_BOLT, "Execution Mode"),
                  FSUI_CSTR("Determines how the emulated CPU executes instructions."), "CPU", "ExecutionMode",
                  Settings::DEFAULT_CPU_EXECUTION_MODE, &Settings::ParseCPUExecutionMode,
                  &Settings::GetCPUExecutionModeName, &Settings::GetCPUExecutionModeDisplayName,
                  CPUExecutionMode::Count);

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_TACHOMETER_ALT, "Enable Overclocking"),
                    FSUI_CSTR("When this option is chosen, the clock speed set below will be used."), "CPU",
                    "OverclockEnable", false);

  const bool oc_enable = GetEffectiveBoolSetting(bsi, "CPU", "OverclockEnable", false);
  if (oc_enable)
  {
    u32 oc_numerator = GetEffectiveUIntSetting(bsi, "CPU", "OverclockNumerator", 1);
    u32 oc_denominator = GetEffectiveUIntSetting(bsi, "CPU", "OverclockDenominator", 1);
    s32 oc_percent = static_cast<s32>(Settings::CPUOverclockFractionToPercent(oc_numerator, oc_denominator));
    if (RangeButton(FSUI_ICONSTR(ICON_FA_TACHOMETER_ALT, "Overclocking Percentage"),
                    FSUI_CSTR("Selects the percentage of the normal clock speed the emulated hardware will run at."),
                    &oc_percent, 10, 1000, 10, "%d%%"))
    {
      Settings::CPUOverclockPercentToFraction(oc_percent, &oc_numerator, &oc_denominator);
      bsi->SetUIntValue("CPU", "OverclockNumerator", oc_numerator);
      bsi->SetUIntValue("CPU", "OverclockDenominator", oc_denominator);
      SetSettingsChanged(bsi);
    }
  }

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_MICROCHIP, "Enable Recompiler ICache"),
                    FSUI_CSTR("Makes games run closer to their console framerate, at a small cost to performance."),
                    "CPU", "RecompilerICache", false);

  MenuHeading(FSUI_CSTR("CD-ROM Emulation"));

  DrawIntListSetting(
    bsi, FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Read Speedup"),
    FSUI_CSTR(
      "Speeds up CD-ROM reads by the specified factor. May improve loading speeds in some games, and break others."),
    "CDROM", "ReadSpeedup", 1, cdrom_read_speeds.data(), cdrom_read_speeds.size(), true, 1);
  DrawIntListSetting(
    bsi, FSUI_ICONSTR(ICON_FA_SEARCH, "Seek Speedup"),
    FSUI_CSTR(
      "Speeds up CD-ROM seeks by the specified factor. May improve loading speeds in some games, and break others."),
    "CDROM", "SeekSpeedup", 1, cdrom_seek_speeds.data(), cdrom_seek_speeds.size(), true);

  DrawIntRangeSetting(
    bsi, FSUI_ICONSTR(ICON_FA_FAST_FORWARD, "Readahead Sectors"),
    FSUI_CSTR("Reduces hitches in emulation by reading/decompressing CD data asynchronously on a worker thread."),
    "CDROM", "ReadaheadSectors", Settings::DEFAULT_CDROM_READAHEAD_SECTORS, 0, 32, FSUI_CSTR("%d sectors"));

  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_DOWNLOAD, "Preload Images to RAM"),
    FSUI_CSTR("Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay."),
    "CDROM", "LoadImageToRAM", false);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_VEST_PATCHES, "Apply Image Patches"),
    FSUI_CSTR("Automatically applies patches to disc images when they are present, currently only PPF is supported."),
    "CDROM", "LoadImagePatches", false);

  EndMenuButtons();
}

void FullscreenUI::DrawEmulationSettingsPage()
{
  static constexpr const std::array emulation_speed_values = {
    0.0f,  0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.25f, 1.5f,
    1.75f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 4.5f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,  10.0f,
  };
  static constexpr const std::array emulation_speed_titles = {
    FSUI_NSTR("Unlimited"),
    "10% [6 FPS (NTSC) / 5 FPS (PAL)]",
    FSUI_NSTR("20% [12 FPS (NTSC) / 10 FPS (PAL)]"),
    FSUI_NSTR("30% [18 FPS (NTSC) / 15 FPS (PAL)]"),
    FSUI_NSTR("40% [24 FPS (NTSC) / 20 FPS (PAL)]"),
    FSUI_NSTR("50% [30 FPS (NTSC) / 25 FPS (PAL)]"),
    FSUI_NSTR("60% [36 FPS (NTSC) / 30 FPS (PAL)]"),
    FSUI_NSTR("70% [42 FPS (NTSC) / 35 FPS (PAL)]"),
    FSUI_NSTR("80% [48 FPS (NTSC) / 40 FPS (PAL)]"),
    FSUI_NSTR("90% [54 FPS (NTSC) / 45 FPS (PAL)]"),
    FSUI_NSTR("100% [60 FPS (NTSC) / 50 FPS (PAL)]"),
    FSUI_NSTR("125% [75 FPS (NTSC) / 62 FPS (PAL)]"),
    FSUI_NSTR("150% [90 FPS (NTSC) / 75 FPS (PAL)]"),
    FSUI_NSTR("175% [105 FPS (NTSC) / 87 FPS (PAL)]"),
    FSUI_NSTR("200% [120 FPS (NTSC) / 100 FPS (PAL)]"),
    FSUI_NSTR("250% [150 FPS (NTSC) / 125 FPS (PAL)]"),
    FSUI_NSTR("300% [180 FPS (NTSC) / 150 FPS (PAL)]"),
    FSUI_NSTR("350% [210 FPS (NTSC) / 175 FPS (PAL)]"),
    FSUI_NSTR("400% [240 FPS (NTSC) / 200 FPS (PAL)]"),
    FSUI_NSTR("450% [270 FPS (NTSC) / 225 FPS (PAL)]"),
    FSUI_NSTR("500% [300 FPS (NTSC) / 250 FPS (PAL)]"),
    FSUI_NSTR("600% [360 FPS (NTSC) / 300 FPS (PAL)]"),
    FSUI_NSTR("700% [420 FPS (NTSC) / 350 FPS (PAL)]"),
    FSUI_NSTR("800% [480 FPS (NTSC) / 400 FPS (PAL)]"),
    FSUI_NSTR("900% [540 FPS (NTSC) / 450 FPS (PAL)]"),
    FSUI_NSTR("1000% [600 FPS (NTSC) / 500 FPS (PAL)]"),
  };

  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Speed Control"));
  DrawFloatListSetting(
    bsi, FSUI_ICONSTR(ICON_FA_STOPWATCH, "Emulation Speed"),
    FSUI_CSTR("Sets the target emulation speed. It is not guaranteed that this speed will be reached on all systems."),
    "Main", "EmulationSpeed", 1.0f, emulation_speed_titles.data(), emulation_speed_values.data(),
    emulation_speed_titles.size(), true);
  DrawFloatListSetting(
    bsi, FSUI_ICONSTR(ICON_FA_BOLT, "Fast Forward Speed"),
    FSUI_CSTR("Sets the fast forward speed. It is not guaranteed that this speed will be reached on all systems."),
    "Main", "FastForwardSpeed", 0.0f, emulation_speed_titles.data(), emulation_speed_values.data(),
    emulation_speed_titles.size(), true);
  DrawFloatListSetting(
    bsi, FSUI_ICONSTR(ICON_FA_BOLT, "Turbo Speed"),
    FSUI_CSTR("Sets the turbo speed. It is not guaranteed that this speed will be reached on all systems."), "Main",
    "TurboSpeed", 2.0f, emulation_speed_titles.data(), emulation_speed_values.data(), emulation_speed_titles.size(),
    true);

  MenuHeading(FSUI_CSTR("Latency Control"));
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_TV, "Vertical Sync (VSync)"),
                    FSUI_CSTR("Synchronizes presentation of the console's frames to the host. GSync/FreeSync users "
                              "should enable Optimal Frame Pacing instead."),
                    "Display", "VSync", false);

  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_LIGHTBULB, "Sync To Host Refresh Rate"),
    FSUI_CSTR("Adjusts the emulation speed so the console's refresh rate matches the host when VSync is enabled."),
    "Main", "SyncToHostRefreshRate", false);

  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_TACHOMETER_ALT, "Optimal Frame Pacing"),
    FSUI_CSTR("Ensures every frame generated is displayed for optimal pacing. Enable for variable refresh displays, "
              "such as GSync/FreeSync. Disable if you are having speed or sound issues."),
    "Display", "OptimalFramePacing", false);

  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_CHARGING_STATION, "Skip Duplicate Frame Display"),
    FSUI_CSTR("Skips the presentation/display of frames that are not unique. Can result in worse frame pacing."),
    "Display", "SkipPresentingDuplicateFrames", false);

  const bool optimal_frame_pacing_active = GetEffectiveBoolSetting(bsi, "Display", "OptimalFramePacing", false);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_STOPWATCH_20, "Reduce Input Latency"),
    FSUI_CSTR("Reduces input latency by delaying the start of frame until closer to the presentation time."), "Display",
    "PreFrameSleep", false, optimal_frame_pacing_active);

  const bool pre_frame_sleep_active =
    (optimal_frame_pacing_active && GetEffectiveBoolSetting(bsi, "Display", "PreFrameSleep", false));
  if (pre_frame_sleep_active)
  {
    DrawFloatRangeSetting(
      bsi, FSUI_ICONSTR(ICON_FA_BATTERY_FULL, "Frame Time Buffer"),
      FSUI_CSTR("Specifies the amount of buffer time added, which reduces the additional sleep time introduced."),
      "Display", "PreFrameSleepBuffer", Settings::DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER, 0.0f, 20.0f,
      FSUI_CSTR("%.1f ms"), 1.0f, pre_frame_sleep_active);
  }

  MenuHeading(FSUI_CSTR("Runahead/Rewind"));

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_BACKWARD, "Enable Rewinding"),
                    FSUI_CSTR("Saves state periodically so you can rewind any mistakes while playing."), "Main",
                    "RewindEnable", false);

  const s32 runahead_frames = GetEffectiveIntSetting(bsi, "Main", "RunaheadFrameCount", 0);
  const bool runahead_enabled = (runahead_frames > 0);
  const bool rewind_enabled = GetEffectiveBoolSetting(bsi, "Main", "RewindEnable", false);

  DrawFloatRangeSetting(
    bsi, FSUI_ICONSTR(ICON_FA_SAVE, "Rewind Save Frequency"),
    FSUI_CSTR("How often a rewind state will be created. Higher frequencies have greater system requirements."), "Main",
    "RewindFrequency", 10.0f, 0.0f, 3600.0f, FSUI_CSTR("%.2f Seconds"), 1.0f, rewind_enabled);
  DrawIntRangeSetting(
    bsi, FSUI_ICONSTR(ICON_FA_GLASS_WHISKEY, "Rewind Save Slots"),
    FSUI_CSTR("How many saves will be kept for rewinding. Higher values have greater memory requirements."), "Main",
    "RewindSaveSlots", 10, 1, 10000, FSUI_CSTR("%d Frames"), rewind_enabled);

  static constexpr const std::array runahead_options = {
    FSUI_NSTR("Disabled"), FSUI_NSTR("1 Frame"),  FSUI_NSTR("2 Frames"), FSUI_NSTR("3 Frames"),
    FSUI_NSTR("4 Frames"), FSUI_NSTR("5 Frames"), FSUI_NSTR("6 Frames"), FSUI_NSTR("7 Frames"),
    FSUI_NSTR("8 Frames"), FSUI_NSTR("9 Frames"), FSUI_NSTR("10 Frames")};

  DrawIntListSetting(
    bsi, FSUI_ICONSTR(ICON_FA_RUNNING, "Runahead"),
    FSUI_CSTR(
      "Simulates the system ahead of time and rolls back/replays to reduce input lag. Very high system requirements."),
    "Main", "RunaheadFrameCount", 0, runahead_options.data(), runahead_options.size(), true);

  TinyString rewind_summary;
  if (runahead_enabled)
  {
    rewind_summary = FSUI_VSTR("Rewind is disabled because runahead is enabled. Runahead will significantly increase "
                               "system requirements.");
  }
  else if (rewind_enabled)
  {
    const u32 resolution_scale = GetEffectiveUIntSetting(bsi, "GPU", "ResolutionScale", 1);
    const float rewind_frequency = GetEffectiveFloatSetting(bsi, "Main", "RewindFrequency", 10.0f);
    const s32 rewind_save_slots = GetEffectiveIntSetting(bsi, "Main", "RewindSaveSlots", 10);
    const float duration =
      ((rewind_frequency <= std::numeric_limits<float>::epsilon()) ? (1.0f / 60.0f) : rewind_frequency) *
      static_cast<float>(rewind_save_slots);

    u64 ram_usage, vram_usage;
    System::CalculateRewindMemoryUsage(rewind_save_slots, resolution_scale, &ram_usage, &vram_usage);
    rewind_summary.format(
      FSUI_FSTR("Rewind for {0} frames, lasting {1:.2f} seconds will require up to {2} MB of RAM and {3} MB of VRAM."),
      rewind_save_slots, duration, ram_usage / 1048576, vram_usage / 1048576);
  }
  else
  {
    rewind_summary = FSUI_VSTR("Rewind is not enabled. Please note that enabling rewind may significantly increase "
                               "system requirements.");
  }

  ActiveButton(rewind_summary, false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, UIStyle.LargeFont);

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
  std::vector<std::string> profiles = InputManager::GetInputProfileNames();
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
                     InputManager::CopyConfiguration(dsi, ssi, true, true, true, IsEditingGameSettings(dsi));
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
  InputManager::CopyConfiguration(&dsi, *ssi, true, true, true, IsEditingGameSettings(ssi));
  if (dsi.Save())
    ShowToast(std::string(), fmt::format(FSUI_FSTR("Input profile '{}' saved."), name));
  else
    ShowToast(std::string(), fmt::format(FSUI_FSTR("Failed to save input profile '{}'."), name));
}

void FullscreenUI::DoSaveNewInputProfile()
{
  OpenInputStringDialog(FSUI_ICONSTR(ICON_FA_SAVE, "Save Profile"),
                        FSUI_STR("Enter the name of the input profile you wish to create."), std::string(),
                        FSUI_ICONSTR(ICON_FA_FOLDER_PLUS, "Create"), [](std::string title) {
                          if (!title.empty())
                            DoSaveInputProfile(title);
                        });
}

void FullscreenUI::DoSaveInputProfile()
{
  std::vector<std::string> profiles = InputManager::GetInputProfileNames();
  if (profiles.empty())
  {
    DoSaveNewInputProfile();
    return;
  }

  ImGuiFullscreen::ChoiceDialogOptions coptions;
  coptions.reserve(profiles.size() + 1);
  coptions.emplace_back(FSUI_STR("Create New..."), false);
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
                     }
                     else
                     {
                       CloseChoiceDialog();
                       DoSaveNewInputProfile();
                     }
                   });
}

void FullscreenUI::ResetControllerSettings()
{
  SettingsInterface* dsi = GetEditingSettingsInterface();

  Settings::SetDefaultControllerConfig(*dsi);
  ShowToast(std::string(), FSUI_STR("Controller settings reset to default."));
}

void FullscreenUI::DrawControllerSettingsPage()
{
  BeginMenuButtons();

  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  MenuHeading(FSUI_CSTR("Configuration"));

  if (IsEditingGameSettings(bsi))
  {
    if (DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_COG, "Per-Game Configuration"),
                          FSUI_CSTR("Uses game-specific settings for controllers for this game."), "ControllerPorts",
                          "UseGameSettingsForController", false, IsEditingGameSettings(bsi), false))
    {
      // did we just enable per-game for the first time?
      if (bsi->GetBoolValue("ControllerPorts", "UseGameSettingsForController", false) &&
          !bsi->GetBoolValue("ControllerPorts", "GameSettingsInitialized", false))
      {
        bsi->SetBoolValue("ControllerPorts", "GameSettingsInitialized", true);
        CopyGlobalControllerSettingsToGame();
      }
    }
  }

  if (IsEditingGameSettings(bsi) && !bsi->GetBoolValue("ControllerPorts", "UseGameSettingsForController", false))
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
    if (MenuButton(FSUI_ICONSTR(ICON_FA_DUMPSTER_FIRE, "Reset Settings"),
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

  MenuHeading(FSUI_CSTR("Input Sources"));

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_COG, "Enable SDL Input Source"),
                    FSUI_CSTR("The SDL input source supports most controllers."), "InputSources", "SDL", true, true,
                    false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_WIFI, "SDL DualShock 4 / DualSense Enhanced Mode"),
                    FSUI_CSTR("Provides vibration and LED control support over Bluetooth."), "InputSources",
                    "SDLControllerEnhancedMode", false, bsi->GetBoolValue("InputSources", "SDL", true), false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_LIGHTBULB, "SDL DualSense Player LED"),
                    FSUI_CSTR("Enable/Disable the Player LED on DualSense controllers."), "InputSources",
                    "SDLPS5PlayerLED", false, bsi->GetBoolValue("InputSources", "SDLControllerEnhancedMode", true),
                    false);
#ifdef _WIN32
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_COG, "Enable XInput Input Source"),
                    FSUI_CSTR("The XInput source provides support for XBox 360/XBox One/XBox Series controllers."),
                    "InputSources", "XInput", false);
#endif

  MenuHeading(FSUI_CSTR("Multitap"));
  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_PLUS_SQUARE, "Multitap Mode"),
                  FSUI_CSTR("Enables an additional three controller slots on each port. Not supported in all games."),
                  "ControllerPorts", "MultitapMode", Settings::DEFAULT_MULTITAP_MODE, &Settings::ParseMultitapModeName,
                  &Settings::GetMultitapModeName, &Settings::GetMultitapModeDisplayName, MultitapMode::Count);

  // load mtap settings
  const MultitapMode mtap_mode =
    Settings::ParseMultitapModeName(bsi->GetTinyStringValue("ControllerPorts", "MultitapMode", "").c_str())
      .value_or(Settings::DEFAULT_MULTITAP_MODE);
  const std::array<bool, 2> mtap_enabled = {
    {(mtap_mode == MultitapMode::Port1Only || mtap_mode == MultitapMode::BothPorts),
     (mtap_mode == MultitapMode::Port2Only || mtap_mode == MultitapMode::BothPorts)}};

  // create the ports
  for (const u32 global_slot : Controller::PortDisplayOrder)
  {
    const auto [mtap_port, mtap_slot] = Controller::ConvertPadToPortAndSlot(global_slot);
    const bool is_mtap_port = Controller::PortAndSlotIsMultitap(mtap_port, mtap_slot);
    if (is_mtap_port && !mtap_enabled[mtap_port])
      continue;

    MenuHeading(TinyString::from_format(fmt::runtime(FSUI_ICONSTR(ICON_FA_PLUG, "Controller Port {}")),
                                        Controller::GetPortDisplayName(mtap_port, mtap_slot, mtap_enabled[mtap_port])));

    const TinyString section = TinyString::from_format("Pad{}", global_slot + 1);
    const TinyString type =
      bsi->GetTinyStringValue(section.c_str(), "Type", Controller::GetDefaultPadType(global_slot));
    const Controller::ControllerInfo* ci = Controller::GetControllerInfo(type);
    if (MenuButton(TinyString::from_format("{}##type{}", FSUI_ICONSTR(ICON_FA_GAMEPAD, "Controller Type"), global_slot),
                   ci ? Host::TranslateToCString("ControllerType", ci->display_name) : FSUI_CSTR("Unknown")))
    {
      std::vector<std::pair<std::string, std::string>> raw_options(Controller::GetControllerTypeNames());
      ImGuiFullscreen::ChoiceDialogOptions options;
      options.reserve(raw_options.size());
      for (auto& it : raw_options)
      {
        options.emplace_back(std::move(it.second), type == it.first);
      }
      OpenChoiceDialog(TinyString::from_format(FSUI_FSTR("Port {} Controller Type"), global_slot + 1), false,
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

    if (!ci || ci->bindings.empty())
      continue;

    if (MenuButton(FSUI_ICONSTR(ICON_FA_MAGIC, "Automatic Mapping"),
                   FSUI_CSTR("Attempts to map the selected port to a chosen controller.")))
    {
      StartAutomaticBinding(global_slot);
    }

    for (const Controller::ControllerBindingInfo& bi : ci->bindings)
      DrawInputBindingButton(bsi, bi.type, section.c_str(), bi.name, ci->GetBindingDisplayName(bi), bi.icon_name, true);

    MenuHeading(
      SmallString::from_format(fmt::runtime(FSUI_ICONSTR(ICON_FA_MICROCHIP, "Controller Port {} Macros")),
                               Controller::GetPortDisplayName(mtap_port, mtap_slot, mtap_enabled[mtap_port])));

    for (u32 macro_index = 0; macro_index < InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER; macro_index++)
    {
      bool& expanded = s_state.controller_macro_expanded[global_slot][macro_index];
      expanded ^= MenuHeadingButton(
        SmallString::from_format(fmt::runtime(FSUI_ICONSTR(ICON_PF_EMPTY_KEYCAP, "Macro Button {}")), macro_index + 1),
        s_state.controller_macro_expanded[global_slot][macro_index] ? ICON_FA_CHEVRON_UP : ICON_FA_CHEVRON_DOWN);
      if (!expanded)
        continue;

      DrawInputBindingButton(bsi, InputBindingInfo::Type::Macro, section.c_str(),
                             TinyString::from_format("Macro{}", macro_index + 1), FSUI_CSTR("Trigger"), nullptr, true);

      SmallString binds_string =
        bsi->GetSmallStringValue(section.c_str(), TinyString::from_format("Macro{}Binds", macro_index + 1).c_str());
      TinyString pretty_binds_string;
      if (!binds_string.empty())
      {
        for (const std::string_view& bind : StringUtil::SplitString(binds_string, '&', true))
        {
          const char* dispname = nullptr;
          for (const Controller::ControllerBindingInfo& bi : ci->bindings)
          {
            if (bind == bi.name)
            {
              dispname = bi.icon_name ? bi.icon_name : ci->GetBindingDisplayName(bi);
              break;
            }
          }
          pretty_binds_string.append_format("{}{}", pretty_binds_string.empty() ? "" : " ", dispname);
        }
      }
      if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_KEYBOARD, "Buttons"), nullptr,
                              pretty_binds_string.empty() ? FSUI_CSTR("-") : pretty_binds_string.c_str(), true,
                              LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
      {
        std::vector<std::string_view> buttons_split(StringUtil::SplitString(binds_string, '&', true));
        ImGuiFullscreen::ChoiceDialogOptions options;
        for (const Controller::ControllerBindingInfo& bi : ci->bindings)
        {
          if (bi.type != InputBindingInfo::Type::Button && bi.type != InputBindingInfo::Type::Axis &&
              bi.type != InputBindingInfo::Type::HalfAxis)
          {
            continue;
          }
          options.emplace_back(ci->GetBindingDisplayName(bi),
                               std::any_of(buttons_split.begin(), buttons_split.end(),
                                           [bi](const std::string_view& it) { return (it == bi.name); }));
        }

        OpenChoiceDialog(
          TinyString::from_format(FSUI_FSTR("Select Macro {} Binds"), macro_index + 1), true, std::move(options),
          [game_settings, section, macro_index, ci](s32 index, const std::string& title, bool checked) {
            // convert display name back to bind name
            std::string_view to_modify;
            for (const Controller::ControllerBindingInfo& bi : ci->bindings)
            {
              if (title == ci->GetBindingDisplayName(bi))
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
            const TinyString key = TinyString::from_format("Macro{}Binds", macro_index + 1);

            std::string binds_string = bsi->GetStringValue(section.c_str(), key.c_str());
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

      DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_GAMEPAD, "Press To Toggle"),
                        FSUI_CSTR("Toggles the macro when the button is pressed, instead of held."), section.c_str(),
                        TinyString::from_format("Macro{}Toggle", macro_index + 1), false, true, false);

      const TinyString freq_key = TinyString::from_format("Macro{}Frequency", macro_index + 1);
      const TinyString freq_label =
        TinyString::from_format(ICON_FA_CLOCK " {}##macro_{}_frequency", FSUI_VSTR("Frequency"), macro_index + 1);
      s32 frequency = bsi->GetIntValue(section.c_str(), freq_key.c_str(), 0);
      const TinyString freq_summary = ((frequency == 0) ? TinyString(FSUI_VSTR("Disabled")) :
                                                          TinyString::from_format(FSUI_FSTR("{} Frames"), frequency));
      if (MenuButtonWithValue(
            freq_label,
            FSUI_CSTR(
              "Determines the frequency at which the macro will toggle the buttons on and off (aka auto fire)."),
            freq_summary, true))
      {
        ImGui::OpenPopup(freq_label.c_str());
      }

      DrawFloatSpinBoxSetting(bsi, FSUI_ICONSTR(ICON_FA_ARROW_DOWN, "Pressure"),
                              FSUI_CSTR("Determines how much pressure is simulated when macro is active."), section,
                              TinyString::from_format("Macro{}Pressure", macro_index + 1), 1.0f, 0.01f, 1.0f, 0.01f,
                              100.0f, "%.0f%%");

      DrawFloatSpinBoxSetting(bsi, FSUI_ICONSTR(ICON_FA_SKULL, "Deadzone"),
                              FSUI_CSTR("Determines how much button pressure is ignored before activating the macro."),
                              section, TinyString::from_format("Macro{}Deadzone", macro_index + 1).c_str(), 0.0f, 0.00f,
                              1.0f, 0.01f, 100.0f, "%.0f%%");

      ImGui::SetNextWindowSize(LayoutScale(500.0f, 180.0f));
      ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

      ImGui::PushFont(UIStyle.LargeFont);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                                  ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

      if (ImGui::BeginPopupModal(freq_label, nullptr,
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
        if (MenuButton(FSUI_CSTR("OK"), nullptr, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
          ImGui::CloseCurrentPopup();
        EndMenuButtons();

        ImGui::EndPopup();
      }

      ImGui::PopStyleVar(3);
      ImGui::PopFont();
    }

    if (!ci->settings.empty())
    {
      MenuHeading(
        SmallString::from_format(fmt::runtime(FSUI_ICONSTR(ICON_FA_SLIDERS_H, "Controller Port {} Settings")),
                                 Controller::GetPortDisplayName(mtap_port, mtap_slot, mtap_enabled[mtap_port])));

      for (const SettingInfo& si : ci->settings)
      {
        TinyString title;
        title.format(ICON_FA_COG "{}", Host::TranslateToStringView(ci->name, si.display_name));
        const char* description = Host::TranslateToCString(ci->name, si.description);
        switch (si.type)
        {
          case SettingInfo::Type::Boolean:
            DrawToggleSetting(bsi, title, description, section.c_str(), si.name, si.BooleanDefaultValue(), true, false);
            break;
          case SettingInfo::Type::Integer:
            DrawIntRangeSetting(bsi, title, description, section.c_str(), si.name, si.IntegerDefaultValue(),
                                si.IntegerMinValue(), si.IntegerMaxValue(), si.format, true);
            break;
          case SettingInfo::Type::IntegerList:
            DrawIntListSetting(bsi, title, description, section.c_str(), si.name, si.IntegerDefaultValue(), si.options,
                               0, true, si.IntegerMinValue(), true, LAYOUT_MENU_BUTTON_HEIGHT, UIStyle.LargeFont,
                               UIStyle.MediumFont, ci->name);
            break;
          case SettingInfo::Type::Float:
            DrawFloatSpinBoxSetting(bsi, title, description, section.c_str(), si.name, si.FloatDefaultValue(),
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

  const HotkeyInfo* last_category = nullptr;
  for (const HotkeyInfo* hotkey : s_state.hotkey_list_cache)
  {
    if (!last_category || std::strcmp(hotkey->category, last_category->category) != 0)
    {
      MenuHeading(Host::TranslateToCString("Hotkeys", hotkey->category));
      last_category = hotkey;
    }

    DrawInputBindingButton(bsi, InputBindingInfo::Type::Button, "Hotkeys", hotkey->name,
                           Host::TranslateToCString("Hotkeys", hotkey->display_name), nullptr, false);
  }

  EndMenuButtons();
}

void FullscreenUI::DrawMemoryCardSettingsPage()
{
  static constexpr const std::array type_keys = {"Card1Type", "Card2Type"};
  static constexpr const std::array path_keys = {"Card1Path", "Card2Path"};

  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Settings and Operations"));
  DrawFolderSetting(bsi, FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Memory Card Directory"), "MemoryCards", "Directory",
                    EmuFolders::MemoryCards);

  if (!game_settings && MenuButton(FSUI_ICONSTR(ICON_FA_MAGIC, "Reset Memory Card Directory"),
                                   FSUI_CSTR("Resets memory card directory to default (user directory).")))
  {
    bsi->SetStringValue("MemoryCards", "Directory", "memcards");
    SetSettingsChanged(bsi);
  }

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_SEARCH, "Use Single Card For Multi-Disc Games"),
                    FSUI_CSTR("When playing a multi-disc game and using per-game (title) memory cards, "
                              "use a single memory card for all discs."),
                    "MemoryCards", "UsePlaylistTitle", true);

  for (u32 i = 0; i < 2; i++)
  {
    MenuHeading(TinyString::from_format(FSUI_FSTR("Memory Card Port {}"), i + 1));

    const MemoryCardType default_type =
      (i == 0) ? Settings::DEFAULT_MEMORY_CARD_1_TYPE : Settings::DEFAULT_MEMORY_CARD_2_TYPE;
    DrawEnumSetting(
      bsi, TinyString::from_format(fmt::runtime(FSUI_ICONSTR(ICON_FA_SD_CARD, "Memory Card {} Type")), i + 1),
      SmallString::from_format(FSUI_FSTR("Sets which sort of memory card image will be used for slot {}."), i + 1),
      "MemoryCards", type_keys[i], default_type, &Settings::ParseMemoryCardTypeName, &Settings::GetMemoryCardTypeName,
      &Settings::GetMemoryCardTypeDisplayName, MemoryCardType::Count);

    const MemoryCardType effective_type =
      Settings::ParseMemoryCardTypeName(
        GetEffectiveTinyStringSetting(bsi, "MemoryCards", type_keys[i], Settings::GetMemoryCardTypeName(default_type))
          .c_str())
        .value_or(default_type);
    const bool is_shared = (effective_type == MemoryCardType::Shared);
    std::optional<SmallString> path_value(bsi->GetOptionalSmallStringValue(
      "MemoryCards", path_keys[i],
      IsEditingGameSettings(bsi) ? std::nullopt :
                                   std::optional<const char*>((i == 0) ? "shared_card_1.mcd" : "shared_card_2.mcd")));

    TinyString title;
    title.format("{}##card_name_{}", FSUI_ICONSTR(ICON_FA_FILE, "Shared Card Name"), i);
    if (MenuButtonWithValue(title,
                            FSUI_CSTR("The selected memory card image will be used in shared mode for this slot."),
                            path_value.has_value() ? path_value->c_str() : FSUI_CSTR("Use Global Setting"), is_shared))
    {
      ImGuiFullscreen::ChoiceDialogOptions options;
      std::vector<std::string> names;
      if (IsEditingGameSettings(bsi))
        options.emplace_back("Use Global Setting", !path_value.has_value());
      if (path_value.has_value() && !path_value->empty())
      {
        options.emplace_back(fmt::format("{} (Current)", path_value.value()), true);
        names.emplace_back(path_value.value().view());
      }

      FileSystem::FindResultsArray results;
      FileSystem::FindFiles(EmuFolders::MemoryCards.c_str(), "*.mcd",
                            FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RELATIVE_PATHS,
                            &results);
      for (FILESYSTEM_FIND_DATA& ffd : results)
      {
        const bool selected = (path_value.has_value() && path_value.value() == ffd.FileName);
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

void FullscreenUI::DrawGraphicsSettingsPage()
{
  static constexpr const std::array resolution_scales = {
    FSUI_NSTR("Automatic based on window size"),
    FSUI_NSTR("1x"),
    FSUI_NSTR("2x"),
    FSUI_NSTR("3x (for 720p)"),
    FSUI_NSTR("4x"),
    FSUI_NSTR("5x (for 1080p)"),
    FSUI_NSTR("6x (for 1440p)"),
    FSUI_NSTR("7x"),
    FSUI_NSTR("8x"),
    FSUI_NSTR("9x (for 4K)"),
    FSUI_NSTR("10x"),
    FSUI_NSTR("11x"),
    FSUI_NSTR("12x"),
    FSUI_NSTR("13x"),
    FSUI_NSTR("14x"),
    FSUI_NSTR("15x"),
    FSUI_NSTR("16x"),
  };

  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);
  const u32 resolution_scale = GetEffectiveUIntSetting(bsi, "GPU", "ResolutionScale", 1);

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Device Settings"));

  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_PF_PICTURE, "GPU Renderer"),
                  FSUI_CSTR("Chooses the backend to use for rendering the console/game visuals."), "GPU", "Renderer",
                  Settings::DEFAULT_GPU_RENDERER, &Settings::ParseRendererName, &Settings::GetRendererName,
                  &Settings::GetRendererDisplayName, GPURenderer::Count);

  const GPURenderer renderer =
    Settings::ParseRendererName(
      GetEffectiveTinyStringSetting(bsi, "GPU", "Renderer", Settings::GetRendererName(Settings::DEFAULT_GPU_RENDERER))
        .c_str())
      .value_or(Settings::DEFAULT_GPU_RENDERER);
  const bool is_hardware = (renderer != GPURenderer::Software);

  std::optional<SmallString> current_adapter =
    bsi->GetOptionalSmallStringValue("GPU", "Adapter", game_settings ? std::nullopt : std::optional<const char*>(""));

  if (MenuButtonWithValue(
        FSUI_ICONSTR(ICON_FA_MICROCHIP, "GPU Adapter"), FSUI_CSTR("Selects the GPU to use for rendering."),
        current_adapter.has_value() ? (current_adapter->empty() ? FSUI_CSTR("Default") : current_adapter->c_str()) :
                                      FSUI_CSTR("Use Global Setting")))
  {
    ImGuiFullscreen::ChoiceDialogOptions options;
    options.reserve(s_state.graphics_adapter_list_cache.size() + 2);
    if (game_settings)
      options.emplace_back(FSUI_STR("Use Global Setting"), !current_adapter.has_value());
    options.emplace_back(FSUI_STR("Default"), current_adapter.has_value() && current_adapter->empty());
    for (const GPUDevice::AdapterInfo& adapter : s_state.graphics_adapter_list_cache)
    {
      const bool checked = (current_adapter.has_value() && current_adapter.value() == adapter.name);
      options.emplace_back(adapter.name, checked);
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
    OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_MICROCHIP, "GPU Adapter"), false, std::move(options), std::move(callback));
  }

  const bool true_color_enabled = (is_hardware && GetEffectiveBoolSetting(bsi, "GPU", "TrueColor", false));
  const bool pgxp_enabled = (is_hardware && GetEffectiveBoolSetting(bsi, "GPU", "PGXPEnable", false));
  const bool texture_correction_enabled =
    (pgxp_enabled && GetEffectiveBoolSetting(bsi, "GPU", "PGXPTextureCorrection", true));

  MenuHeading(FSUI_CSTR("Rendering"));

  if (is_hardware)
  {
    DrawIntListSetting(
      bsi, FSUI_ICONSTR(ICON_FA_EXPAND_ALT, "Internal Resolution"),
      FSUI_CSTR("Scales internal VRAM resolution by the specified multiplier. Some games require 1x VRAM resolution."),
      "GPU", "ResolutionScale", 1, resolution_scales.data(), resolution_scales.size(), true, 0);

    DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_COMPRESS_ALT, "Downsampling"),
                    FSUI_CSTR("Downsamples the rendered image prior to displaying it. Can improve "
                              "overall image quality in mixed 2D/3D games."),
                    "GPU", "DownsampleMode", Settings::DEFAULT_GPU_DOWNSAMPLE_MODE, &Settings::ParseDownsampleModeName,
                    &Settings::GetDownsampleModeName, &Settings::GetDownsampleModeDisplayName, GPUDownsampleMode::Count,
                    (renderer != GPURenderer::Software));
    if (Settings::ParseDownsampleModeName(
          GetEffectiveTinyStringSetting(bsi, "GPU", "DownsampleMode",
                                        Settings::GetDownsampleModeName(Settings::DEFAULT_GPU_DOWNSAMPLE_MODE))
            .c_str())
          .value_or(Settings::DEFAULT_GPU_DOWNSAMPLE_MODE) == GPUDownsampleMode::Box)
    {
      DrawIntRangeSetting(bsi, FSUI_ICONSTR(ICON_FA_COMPRESS_ARROWS_ALT, "Downsampling Display Scale"),
                          FSUI_CSTR("Selects the resolution scale that will be applied to the final image. 1x will "
                                    "downsample to the original console resolution."),
                          "GPU", "DownsampleScale", 1, 1, GPU::MAX_RESOLUTION_SCALE, "%dx");
    }

    DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_EXTERNAL_LINK_ALT, "Texture Filtering"),
                    FSUI_CSTR("Smooths out the blockiness of magnified textures on 3D objects."), "GPU",
                    "TextureFilter", Settings::DEFAULT_GPU_TEXTURE_FILTER, &Settings::ParseTextureFilterName,
                    &Settings::GetTextureFilterName, &Settings::GetTextureFilterDisplayName, GPUTextureFilter::Count);

    DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_EXTERNAL_LINK_SQUARE_ALT, "Sprite Texture Filtering"),
                    FSUI_CSTR("Smooths out the blockiness of magnified textures on 2D objects."), "GPU",
                    "SpriteTextureFilter", Settings::DEFAULT_GPU_TEXTURE_FILTER, &Settings::ParseTextureFilterName,
                    &Settings::GetTextureFilterName, &Settings::GetTextureFilterDisplayName, GPUTextureFilter::Count);
  }

  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_SHAPES, "Aspect Ratio"),
                  FSUI_CSTR("Changes the aspect ratio used to display the console's output to the screen."), "Display",
                  "AspectRatio", Settings::DEFAULT_DISPLAY_ASPECT_RATIO, &Settings::ParseDisplayAspectRatio,
                  &Settings::GetDisplayAspectRatioName, &Settings::GetDisplayAspectRatioDisplayName,
                  DisplayAspectRatio::Count);

  DrawEnumSetting(
    bsi, FSUI_ICONSTR(ICON_FA_GRIP_LINES, "Deinterlacing Mode"),
    FSUI_CSTR(
      "Determines which algorithm is used to convert interlaced frames to progressive for display on your system."),
    "Display", "DeinterlacingMode", Settings::DEFAULT_DISPLAY_DEINTERLACING_MODE,
    &Settings::ParseDisplayDeinterlacingMode, &Settings::GetDisplayDeinterlacingModeName,
    &Settings::GetDisplayDeinterlacingModeDisplayName, DisplayDeinterlacingMode::Count);

  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_CROP_ALT, "Crop Mode"),
                  FSUI_CSTR("Determines how much of the area typically not visible on a consumer TV set to crop/hide."),
                  "Display", "CropMode", Settings::DEFAULT_DISPLAY_CROP_MODE, &Settings::ParseDisplayCropMode,
                  &Settings::GetDisplayCropModeName, &Settings::GetDisplayCropModeDisplayName,
                  DisplayCropMode::MaxCount);

  DrawEnumSetting(
    bsi, FSUI_ICONSTR(ICON_FA_EXPAND, "Scaling"),
    FSUI_CSTR("Determines how the emulated console's output is upscaled or downscaled to your monitor's resolution."),
    "Display", "Scaling", Settings::DEFAULT_DISPLAY_SCALING, &Settings::ParseDisplayScaling,
    &Settings::GetDisplayScalingName, &Settings::GetDisplayScalingDisplayName, DisplayScalingMode::Count);

  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_STOPWATCH, "Force Video Timing"),
                  FSUI_CSTR("Utilizes the chosen video timing regardless of the game's setting."), "GPU",
                  "ForceVideoTiming", Settings::DEFAULT_FORCE_VIDEO_TIMING_MODE, &Settings::ParseForceVideoTimingName,
                  &Settings::GetForceVideoTimingName, &Settings::GetForceVideoTimingDisplayName,
                  ForceVideoTimingMode::Count);

  if (is_hardware)
  {
    DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_PALETTE, "True Color Rendering"),
                      FSUI_CSTR("Disables dithering and uses the full 8 bits per channel of color information."), "GPU",
                      "TrueColor", true);
  }

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_EXCHANGE_ALT, "Widescreen Rendering"),
                    FSUI_CSTR("Increases the field of view from 4:3 to the chosen display aspect ratio in 3D games."),
                    "GPU", "WidescreenHack", false);

  if (is_hardware)
  {
    DrawToggleSetting(
      bsi, FSUI_ICONSTR(ICON_FA_BEZIER_CURVE, "PGXP Geometry Correction"),
      FSUI_CSTR("Reduces \"wobbly\" polygons by attempting to preserve the fractional component through memory "
                "transfers."),
      "GPU", "PGXPEnable", false);

    DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_SITEMAP, "PGXP Depth Buffer"),
                      FSUI_CSTR("Reduces polygon Z-fighting through depth testing. Low compatibility with games."),
                      "GPU", "PGXPDepthBuffer", false, pgxp_enabled && texture_correction_enabled);
  }

  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_COMPRESS, "Force 4:3 For FMVs"),
    FSUI_CSTR("Switches back to 4:3 display aspect ratio when displaying 24-bit content, usually FMVs."), "Display",
    "Force4_3For24Bit", false);

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_BRUSH, "FMV Chroma Smoothing"),
                    FSUI_CSTR("Smooths out blockyness between colour transitions in 24-bit content, usually FMVs."),
                    "GPU", "ChromaSmoothing24Bit", false);

  MenuHeading(FSUI_CSTR("Advanced"));

  std::optional<SmallString> strvalue = bsi->GetOptionalSmallStringValue(
    "GPU", "FullscreenMode", game_settings ? std::nullopt : std::optional<const char*>(""));

  if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_TV, "Fullscreen Resolution"),
                          FSUI_CSTR("Selects the resolution to use in fullscreen modes."),
                          strvalue.has_value() ?
                            (strvalue->empty() ? FSUI_CSTR("Borderless Fullscreen") : strvalue->c_str()) :
                            FSUI_CSTR("Use Global Setting")))
  {
    const GPUDevice::AdapterInfo* selected_adapter = nullptr;
    if (current_adapter.has_value())
    {
      for (const GPUDevice::AdapterInfo& ai : s_state.graphics_adapter_list_cache)
      {
        if (ai.name == current_adapter->view())
        {
          selected_adapter = &ai;
          break;
        }
      }
    }
    else
    {
      if (!s_state.graphics_adapter_list_cache.empty())
        selected_adapter = &s_state.graphics_adapter_list_cache.front();
    }

    ImGuiFullscreen::ChoiceDialogOptions options;
    options.reserve((selected_adapter ? selected_adapter->fullscreen_modes.size() : 0) + 2);
    if (game_settings)
      options.emplace_back(FSUI_STR("Use Global Setting"), !strvalue.has_value());
    options.emplace_back(FSUI_STR("Borderless Fullscreen"), strvalue.has_value() && strvalue->empty());
    if (selected_adapter)
    {
      for (const GPUDevice::ExclusiveFullscreenMode& mode : selected_adapter->fullscreen_modes)
      {
        const TinyString mode_str = mode.ToString();
        const bool checked = (strvalue.has_value() && strvalue.value() == mode_str);
        options.emplace_back(std::string(mode_str.view()), checked);
      }
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

  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_ARROWS_ALT, "Screen Position"),
                  FSUI_CSTR("Determines the position on the screen when black borders must be added."), "Display",
                  "Alignment", Settings::DEFAULT_DISPLAY_ALIGNMENT, &Settings::ParseDisplayAlignment,
                  &Settings::GetDisplayAlignmentName, &Settings::GetDisplayAlignmentDisplayName,
                  DisplayAlignment::Count);

  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_SYNC_ALT, "Screen Rotation"),
                  FSUI_CSTR("Determines the rotation of the simulated TV screen."), "Display", "Rotation",
                  Settings::DEFAULT_DISPLAY_ROTATION, &Settings::ParseDisplayRotation,
                  &Settings::GetDisplayRotationName, &Settings::GetDisplayRotationDisplayName, DisplayRotation::Count);

  if (is_hardware)
  {
    DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_GRIP_LINES_VERTICAL, "Line Detection"),
                    FSUI_CSTR("Attempts to detect one pixel high/wide lines that rely on non-upscaled rasterization "
                              "behavior, filling in gaps introduced by upscaling."),
                    "GPU", "LineDetectMode", Settings::DEFAULT_GPU_LINE_DETECT_MODE, &Settings::ParseLineDetectModeName,
                    &Settings::GetLineDetectModeName, &Settings::GetLineDetectModeDisplayName, GPULineDetectMode::Count,
                    resolution_scale > 1);

    DrawToggleSetting(
      bsi, FSUI_ICONSTR(ICON_FA_TINT_SLASH, "Scaled Dithering"),
      FSUI_CSTR("Scales the dithering pattern with the internal rendering resolution, making it less noticeable. "
                "Usually safe to enable."),
      "GPU", "ScaledDithering", true, !true_color_enabled);

    DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_FILL, "Accurate Blending"),
                      FSUI_CSTR("Forces blending to be done in the shader at 16-bit precision, when not using true "
                                "color. Non-trivial performance impact, and unnecessary for most games."),
                      "GPU", "AccurateBlending", false, !true_color_enabled);

    const GPUTextureFilter texture_filtering =
      Settings::ParseTextureFilterName(
        GetEffectiveTinyStringSetting(bsi, "GPU", "TextureFilter",
                                      Settings::GetTextureFilterName(Settings::DEFAULT_GPU_TEXTURE_FILTER)))
        .value_or(Settings::DEFAULT_GPU_TEXTURE_FILTER);

    DrawToggleSetting(
      bsi, FSUI_ICONSTR(ICON_FA_EYE_DROPPER, "Round Upscaled Texture Coordinates"),
      FSUI_CSTR("Rounds texture coordinates instead of flooring when upscaling. Can fix misaligned "
                "textures in some games, but break others, and is incompatible with texture filtering."),
      "GPU", "ForceRoundTextureCoordinates", false,
      resolution_scale > 1 && texture_filtering == GPUTextureFilter::Nearest);

    DrawToggleSetting(
      bsi, FSUI_ICONSTR(ICON_FA_DOWNLOAD, "Use Software Renderer For Readbacks"),
      FSUI_CSTR("Runs the software renderer in parallel for VRAM readbacks. On some systems, this may result "
                "in greater performance."),
      "GPU", "UseSoftwareRendererForReadbacks", false);
  }

  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_ARROWS_ALT_V, "Stretch Display Vertically"),
    FSUI_CSTR("Stretches the display to match the aspect ratio by multiplying vertically instead of horizontally."),
    "Display", "StretchVertically", false);

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_EXPAND_ARROWS_ALT, "Automatically Resize Window"),
                    FSUI_CSTR("Automatically resizes the window to match the internal resolution."), "Display",
                    "AutoResizeWindow", false);

  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_ENVELOPE, "Disable Mailbox Presentation"),
    FSUI_CSTR("Forces the use of FIFO over Mailbox presentation, i.e. double buffering instead of triple buffering. "
              "Usually results in worse frame pacing."),
    "Display", "DisableMailboxPresentation", false);

#ifdef _WIN32
  if (renderer == GPURenderer::HardwareD3D11 || renderer == GPURenderer::Software)
  {
    DrawToggleSetting(
      bsi, FSUI_ICONSTR(ICON_FA_PAINT_BRUSH, "Use Blit Swap Chain"),
      FSUI_CSTR("Uses a blit presentation model instead of flipping. This may be needed on some systems."), "Display",
      "UseBlitSwapChain", false);
  }
#endif

  if (is_hardware && pgxp_enabled)
  {
    MenuHeading(FSUI_CSTR("PGXP (Precision Geometry Transform Pipeline)"));

    DrawToggleSetting(
      bsi, FSUI_ICONSTR(ICON_FA_IMAGES, "Perspective Correct Textures"),
      FSUI_CSTR("Uses perspective-correct interpolation for texture coordinates, straightening out warped textures."),
      "GPU", "PGXPTextureCorrection", true, pgxp_enabled);
    DrawToggleSetting(
      bsi, FSUI_ICONSTR(ICON_FA_PAINT_ROLLER, "Perspective Correct Colors"),
      FSUI_CSTR("Uses perspective-correct interpolation for colors, which can improve visuals in some games."), "GPU",
      "PGXPColorCorrection", false, pgxp_enabled);
    DrawToggleSetting(
      bsi, FSUI_ICONSTR(ICON_FA_REMOVE_FORMAT, "Culling Correction"),
      FSUI_CSTR("Increases the precision of polygon culling, reducing the number of holes in geometry."), "GPU",
      "PGXPCulling", true, pgxp_enabled);
    DrawToggleSetting(
      bsi, FSUI_ICONSTR(ICON_FA_DRAW_POLYGON, "Preserve Projection Precision"),
      FSUI_CSTR("Adds additional precision to PGXP data post-projection. May improve visuals in some games."), "GPU",
      "PGXPPreserveProjFP", false, pgxp_enabled);

    DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_MICROCHIP, "CPU Mode"),
                      FSUI_CSTR("Uses PGXP for all instructions, not just memory operations."), "GPU", "PGXPCPU", false,
                      pgxp_enabled);

    DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_VECTOR_SQUARE, "Vertex Cache"),
                      FSUI_CSTR("Uses screen positions to resolve PGXP data. May improve visuals in some games."),
                      "GPU", "PGXPVertexCache", pgxp_enabled);

    DrawToggleSetting(
      bsi, FSUI_ICONSTR(ICON_FA_MINUS_SQUARE, "Disable on 2D Polygons"),
      FSUI_CSTR("Uses native resolution coordinates for 2D polygons, instead of precise coordinates. Can "
                "fix misaligned UI in some games, but otherwise should be left disabled."),
      "GPU", "PGXPDisableOn2DPolygons", false, pgxp_enabled);

    DrawFloatRangeSetting(
      bsi, FSUI_ICONSTR(ICON_FA_STAR, "Geometry Tolerance"),
      FSUI_CSTR("Sets a threshold for discarding precise values when exceeded. May help with glitches in some games."),
      "GPU", "PGXPTolerance", -1.0f, -1.0f, 10.0f, "%.1f", pgxp_enabled);

    DrawFloatRangeSetting(
      bsi, FSUI_ICONSTR(ICON_FA_MINUS_CIRCLE, "Depth Clear Threshold"),
      FSUI_CSTR("Sets a threshold for discarding the emulated depth buffer. May help in some games."), "GPU",
      "PGXPDepthBuffer", Settings::DEFAULT_GPU_PGXP_DEPTH_THRESHOLD, 0.0f, 4096.0f, "%.1f", pgxp_enabled);
  }

  MenuHeading(FSUI_CSTR("Capture"));

  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_CAMERA, "Screenshot Size"),
                  FSUI_CSTR("Determines the size of screenshots created by DuckStation."), "Display", "ScreenshotMode",
                  Settings::DEFAULT_DISPLAY_SCREENSHOT_MODE, &Settings::ParseDisplayScreenshotMode,
                  &Settings::GetDisplayScreenshotModeName, &Settings::GetDisplayScreenshotModeDisplayName,
                  DisplayScreenshotMode::Count);
  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_FILE_IMAGE, "Screenshot Format"),
                  FSUI_CSTR("Determines the format that screenshots will be saved/compressed with."), "Display",
                  "ScreenshotFormat", Settings::DEFAULT_DISPLAY_SCREENSHOT_FORMAT,
                  &Settings::ParseDisplayScreenshotFormat, &Settings::GetDisplayScreenshotFormatName,
                  &Settings::GetDisplayScreenshotFormatDisplayName, DisplayScreenshotFormat::Count);
  DrawIntRangeSetting(bsi, FSUI_ICONSTR(ICON_FA_CAMERA_RETRO, "Screenshot Quality"),
                      FSUI_CSTR("Selects the quality at which screenshots will be compressed."), "Display",
                      "ScreenshotQuality", Settings::DEFAULT_DISPLAY_SCREENSHOT_QUALITY, 1, 100, "%d%%");

  MenuHeading(FSUI_CSTR("Texture Replacements"));

  ActiveButton(FSUI_CSTR("The texture cache is currently experimental, and may cause rendering errors in some games."),
               false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, UIStyle.LargeFont);

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_ID_BADGE, "Enable Texture Cache"),
                    FSUI_CSTR("Enables caching of guest textures, required for texture replacement."), "GPU",
                    "EnableTextureCache", false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_VIDEO, "Use Old MDEC Routines"),
                    FSUI_CSTR("Enables the older, less accurate MDEC decoding routines. May be required for old "
                              "replacement backgrounds to match/load."),
                    "Hacks", "UseOldMDECRoutines", false);

  const bool texture_cache_enabled = GetEffectiveBoolSetting(bsi, "GPU", "EnableTextureCache", false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_FILE_IMPORT, "Enable Texture Replacements"),
                    FSUI_CSTR("Enables loading of replacement textures. Not compatible with all games."),
                    "TextureReplacements", "EnableTextureReplacements", false, texture_cache_enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_FILE_EXPORT, "Enable Texture Dumping"),
    FSUI_CSTR("Enables dumping of textures to image files, which can be replaced. Not compatible with all games."),
    "TextureReplacements", "DumpTextures", false, texture_cache_enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_FILE, "Dump Replaced Textures"),
    FSUI_CSTR("Dumps textures that have replacements already loaded."), "TextureReplacements", "DumpReplacedTextures",
    false, texture_cache_enabled && GetEffectiveBoolSetting(bsi, "TextureReplacements", "DumpTextures", false));

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_FILE_ALT, "Enable VRAM Write Replacement"),
                    FSUI_CSTR("Enables the replacement of background textures in supported games."),
                    "TextureReplacements", "EnableVRAMWriteReplacements", false);

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_FILE_INVOICE, "Enable VRAM Write Dumping"),
                    FSUI_CSTR("Writes backgrounds that can be replaced to the dump directory."), "TextureReplacements",
                    "DumpVRAMWrites", false);

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_TASKS, "Preload Replacement Textures"),
                    FSUI_CSTR("Loads all replacement texture to RAM, reducing stuttering at runtime."),
                    "TextureReplacements", "PreloadTextures", false,
                    ((texture_cache_enabled &&
                      GetEffectiveBoolSetting(bsi, "TextureReplacements", "EnableTextureReplacements", false)) ||
                     GetEffectiveBoolSetting(bsi, "TextureReplacements", "EnableVRAMWriteReplacements", false)));

  DrawFolderSetting(bsi, FSUI_ICONSTR(ICON_FA_FOLDER, "Textures Directory"), "Folders", "Textures",
                    EmuFolders::Textures);

  EndMenuButtons();
}

void FullscreenUI::PopulatePostProcessingChain(SettingsInterface* si, const char* section)
{
  const u32 stages = PostProcessing::Config::GetStageCount(*si, section);
  s_state.postprocessing_stages.clear();
  s_state.postprocessing_stages.reserve(stages);
  for (u32 i = 0; i < stages; i++)
  {
    PostProcessingStageInfo psi;
    psi.name = PostProcessing::Config::GetStageShaderName(*si, section, i);
    psi.options = PostProcessing::Config::GetStageOptions(*si, section, i);
    s_state.postprocessing_stages.push_back(std::move(psi));
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
  static constexpr const char* section = PostProcessing::Config::DISPLAY_CHAIN_SECTION;

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Controls"));

  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_MAGIC, "Enable Post Processing"),
                    FSUI_CSTR("If not enabled, the current post processing chain will be ignored."), "PostProcessing",
                    "Enabled", false);

  if (MenuButton(FSUI_ICONSTR(ICON_FA_SEARCH, "Reload Shaders"),
                 FSUI_CSTR("Reloads the shaders from disk, applying any changes."),
                 bsi->GetBoolValue("PostProcessing", "Enabled", false)))
  {
    if (GPUThread::HasGPUBackend() && PostProcessing::ReloadShaders())
      ShowToast(std::string(), FSUI_STR("Post-processing shaders reloaded."));
  }

  MenuHeading(FSUI_CSTR("Operations"));

  if (MenuButton(FSUI_ICONSTR(ICON_FA_PLUS, "Add Shader"), FSUI_CSTR("Adds a new shader to the chain.")))
  {
    std::vector<std::pair<std::string, std::string>> shaders = PostProcessing::GetAvailableShaderNames();
    ImGuiFullscreen::ChoiceDialogOptions options;
    options.reserve(shaders.size());
    for (auto& [display_name, name] : shaders)
      options.emplace_back(std::move(display_name), false);

    OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_PLUS, "Add Shader"), false, std::move(options),
                     [shaders = std::move(shaders)](s32 index, const std::string& title, bool checked) {
                       if (index < 0 || static_cast<u32>(index) >= shaders.size())
                         return;

                       const std::string& shader_name = shaders[index].second;
                       SettingsInterface* bsi = GetEditingSettingsInterface();
                       Error error;
                       if (PostProcessing::Config::AddStage(*bsi, section, shader_name, &error))
                       {
                         ShowToast(std::string(), fmt::format(FSUI_FSTR("Shader {} added as stage {}."), title,
                                                              PostProcessing::Config::GetStageCount(*bsi, section)));
                         PopulatePostProcessingChain(bsi, section);
                         SetSettingsChanged(bsi);
                       }
                       else
                       {
                         ShowToast(std::string(),
                                   fmt::format(FSUI_FSTR("Failed to load shader {}. It may be invalid.\nError was:"),
                                               title, error.GetDescription()));
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

        SettingsInterface* bsi = GetEditingSettingsInterface();
        PostProcessing::Config::ClearStages(*bsi, section);
        PopulatePostProcessingChain(bsi, section);
        SetSettingsChanged(bsi);
        ShowToast(std::string(), FSUI_STR("Post-processing chain cleared."));
      });
  }

  u32 postprocessing_action = POSTPROCESSING_ACTION_NONE;
  u32 postprocessing_action_index = 0;

  SmallString str;
  SmallString tstr;
  for (u32 stage_index = 0; stage_index < static_cast<u32>(s_state.postprocessing_stages.size()); stage_index++)
  {
    PostProcessingStageInfo& si = s_state.postprocessing_stages[stage_index];

    ImGui::PushID(stage_index);
    str.format(FSUI_FSTR("Stage {}: {}"), stage_index + 1, si.name);
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
                   (stage_index != (s_state.postprocessing_stages.size() - 1))))
    {
      postprocessing_action = POSTPROCESSING_ACTION_MOVE_DOWN;
      postprocessing_action_index = stage_index;
    }

    for (PostProcessing::ShaderOption& opt : si.options)
    {
      if (opt.ui_name.empty())
        continue;

      switch (opt.type)
      {
        case PostProcessing::ShaderOption::Type::Bool:
        {
          bool value = (opt.value[0].int_value != 0);
          tstr.format(ICON_FA_COGS "{}", opt.ui_name);
          if (ToggleButton(tstr,
                           (opt.default_value[0].int_value != 0) ? FSUI_CSTR("Default: Enabled") :
                                                                   FSUI_CSTR("Default: Disabled"),
                           &value))
          {
            opt.value[0].int_value = (value != 0);
            PostProcessing::Config::SetStageOption(*bsi, section, stage_index, opt);
            SetSettingsChanged(bsi);
          }
        }
        break;

        case PostProcessing::ShaderOption::Type::Float:
        {
          tstr.format(ICON_FA_RULER_VERTICAL "{}##{}", opt.ui_name, opt.name);
          str.format(FSUI_FSTR("Value: {} | Default: {} | Minimum: {} | Maximum: {}"), opt.value[0].float_value,
                     opt.default_value[0].float_value, opt.min_value[0].float_value, opt.max_value[0].float_value);
          if (MenuButton(tstr, str))
            ImGui::OpenPopup(tstr);

          ImGui::SetNextWindowSize(LayoutScale(500.0f, 190.0f));
          ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

          ImGui::PushFont(UIStyle.LargeFont);
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

            bool changed = false;
            switch (opt.vector_size)
            {
              case 1:
              {
                changed = ImGui::SliderFloat("##value", &opt.value[0].float_value, opt.min_value[0].float_value,
                                             opt.max_value[0].float_value);
              }
              break;

              case 2:
              {
                changed = ImGui::SliderFloat2("##value", &opt.value[0].float_value, opt.min_value[0].float_value,
                                              opt.max_value[0].float_value);
              }
              break;

              case 3:
              {
                changed = ImGui::SliderFloat3("##value", &opt.value[0].float_value, opt.min_value[0].float_value,
                                              opt.max_value[0].float_value);
              }
              break;

              case 4:
              {
                changed = ImGui::SliderFloat4("##value", &opt.value[0].float_value, opt.min_value[0].float_value,
                                              opt.max_value[0].float_value);
              }
              break;
            }

            if (changed)
            {
              PostProcessing::Config::SetStageOption(*bsi, section, stage_index, opt);
              SetSettingsChanged(bsi);
            }
#endif

            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
            if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, UIStyle.LargeFont,
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

        case PostProcessing::ShaderOption::Type::Int:
        {
          tstr.format(ICON_FA_RULER_VERTICAL "{}##{}", opt.ui_name, opt.name);
          str.format(FSUI_FSTR("Value: {} | Default: {} | Minimum: {} | Maximum: {}"), opt.value[0].int_value,
                     opt.default_value[0].int_value, opt.min_value[0].int_value, opt.max_value[0].int_value);
          if (MenuButton(tstr, str))
            ImGui::OpenPopup(tstr);

          ImGui::SetNextWindowSize(LayoutScale(500.0f, 190.0f));
          ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

          ImGui::PushFont(UIStyle.LargeFont);
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
            bool changed = false;
            ImGui::SetNextItemWidth(end);
            switch (opt.vector_size)
            {
              case 1:
              {
                changed = ImGui::SliderInt("##value", &opt.value[0].int_value, opt.min_value[0].int_value,
                                           opt.max_value[0].int_value);
              }
              break;

              case 2:
              {
                changed = ImGui::SliderInt2("##value", &opt.value[0].int_value, opt.min_value[0].int_value,
                                            opt.max_value[0].int_value);
              }
              break;

              case 3:
              {
                changed = ImGui::SliderInt3("##value", &opt.value[0].int_value, opt.min_value[0].int_value,
                                            opt.max_value[0].int_value);
              }
              break;

              case 4:
              {
                changed = ImGui::SliderInt4("##value", &opt.value[0].int_value, opt.min_value[0].int_value,
                                            opt.max_value[0].int_value);
              }
              break;
            }

            if (changed)
            {
              PostProcessing::Config::SetStageOption(*bsi, section, stage_index, opt);
              SetSettingsChanged(bsi);
            }
#endif

            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
            if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, UIStyle.LargeFont,
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

        default:
          break;
      }
    }

    ImGui::PopID();
  }

  switch (postprocessing_action)
  {
    case POSTPROCESSING_ACTION_REMOVE:
    {
      const PostProcessingStageInfo& si = s_state.postprocessing_stages[postprocessing_action_index];
      ShowToast(std::string(),
                fmt::format(FSUI_FSTR("Removed stage {} ({})."), postprocessing_action_index + 1, si.name));
      PostProcessing::Config::RemoveStage(*bsi, section, postprocessing_action_index);
      PopulatePostProcessingChain(bsi, section);
      SetSettingsChanged(bsi);
    }
    break;
    case POSTPROCESSING_ACTION_MOVE_UP:
    {
      PostProcessing::Config::MoveStageUp(*bsi, section, postprocessing_action_index);
      PopulatePostProcessingChain(bsi, section);
      SetSettingsChanged(bsi);
    }
    break;
    case POSTPROCESSING_ACTION_MOVE_DOWN:
    {
      PostProcessing::Config::MoveStageDown(*bsi, section, postprocessing_action_index);
      PopulatePostProcessingChain(bsi, section);
      SetSettingsChanged(bsi);
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

  MenuHeading(FSUI_CSTR("Audio Control"));

  DrawIntRangeSetting(bsi, FSUI_ICONSTR(ICON_FA_VOLUME_UP, "Output Volume"),
                      FSUI_CSTR("Controls the volume of the audio played on the host."), "Audio", "OutputVolume", 100,
                      0, 200, "%d%%");
  DrawIntRangeSetting(bsi, FSUI_ICONSTR(ICON_FA_FAST_FORWARD, "Fast Forward Volume"),
                      FSUI_CSTR("Controls the volume of the audio played on the host when fast forwarding."), "Audio",
                      "FastForwardVolume", 200, 0, 100, "%d%%");
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_VOLUME_MUTE, "Mute All Sound"),
                    FSUI_CSTR("Prevents the emulator from producing any audible sound."), "Audio", "OutputMuted",
                    false);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Mute CD Audio"),
                    FSUI_CSTR("Forcibly mutes both CD-DA and XA audio from the CD-ROM. Can be used to "
                              "disable background music in some games."),
                    "CDROM", "MuteCDAudio", false);

  MenuHeading(FSUI_CSTR("Backend Settings"));

  DrawEnumSetting(
    bsi, FSUI_ICONSTR(ICON_FA_VOLUME_OFF, "Audio Backend"),
    FSUI_CSTR("The audio backend determines how frames produced by the emulator are submitted to the host."), "Audio",
    "Backend", AudioStream::DEFAULT_BACKEND, &AudioStream::ParseBackendName, &AudioStream::GetBackendName,
    &AudioStream::GetBackendDisplayName, AudioBackend::Count);
  DrawEnumSetting(bsi, FSUI_ICONSTR(ICON_FA_SYNC, "Stretch Mode"),
                  FSUI_CSTR("Determines quality of audio when not running at 100% speed."), "Audio", "StretchMode",
                  AudioStreamParameters::DEFAULT_STRETCH_MODE, &AudioStream::ParseStretchMode,
                  &AudioStream::GetStretchModeName, &AudioStream::GetStretchModeDisplayName, AudioStretchMode::Count);
  DrawIntRangeSetting(bsi, FSUI_ICONSTR(ICON_FA_RULER, "Buffer Size"),
                      FSUI_CSTR("Determines the amount of audio buffered before being pulled by the host API."),
                      "Audio", "BufferMS", AudioStreamParameters::DEFAULT_BUFFER_MS, 10, 500, FSUI_CSTR("%d ms"));
  if (!GetEffectiveBoolSetting(bsi, "Audio", "OutputLatencyMinimal",
                               AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MINIMAL))
  {
    DrawIntRangeSetting(
      bsi, FSUI_ICONSTR(ICON_FA_STOPWATCH_20, "Output Latency"),
      FSUI_CSTR("Determines how much latency there is between the audio being picked up by the host API, and "
                "played through speakers."),
      "Audio", "OutputLatencyMS", AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MS, 1, 500, FSUI_CSTR("%d ms"));
  }
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_STOPWATCH, "Minimal Output Latency"),
                    FSUI_CSTR("When enabled, the minimum supported output latency will be used for the host API."),
                    "Audio", "OutputLatencyMinimal", AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MINIMAL);

  EndMenuButtons();
}

void FullscreenUI::DrawAchievementsSettingsPage()
{
#ifdef ENABLE_RAINTEGRATION
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

  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Settings"));
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_TROPHY, "Enable Achievements"),
                    FSUI_CSTR("When enabled and logged in, DuckStation will scan for achievements on startup."),
                    "Cheevos", "Enabled", false);

  const bool enabled = bsi->GetBoolValue("Cheevos", "Enabled", false);

  if (DrawToggleSetting(
        bsi, FSUI_ICONSTR(ICON_FA_HARD_HAT, "Hardcore Mode"),
        FSUI_CSTR("\"Challenge\" mode for achievements, including leaderboard tracking. Disables save state, "
                  "cheats, and slowdown functions."),
        "Cheevos", "ChallengeMode", false, enabled))
  {
    if (GPUThread::HasGPUBackend() && bsi->GetBoolValue("Cheevos", "ChallengeMode", false))
      ShowToast(std::string(), FSUI_STR("Hardcore mode will be enabled on next game restart."));
  }
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_INBOX, "Achievement Notifications"),
    FSUI_CSTR("Displays popup messages on events such as achievement unlocks and leaderboard submissions."), "Cheevos",
    "Notifications", true, enabled);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_LIST_OL, "Leaderboard Notifications"),
                    FSUI_CSTR("Displays popup messages when starting, submitting, or failing a leaderboard challenge."),
                    "Cheevos", "LeaderboardNotifications", true, enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_HEADPHONES, "Sound Effects"),
    FSUI_CSTR("Plays sound effects for events such as achievement unlocks and leaderboard submissions."), "Cheevos",
    "SoundEffects", true, enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_MAGIC, "Enable In-Game Overlays"),
    FSUI_CSTR("Shows icons in the lower-right corner of the screen when a challenge/primed achievement is active."),
    "Cheevos", "Overlays", true, enabled);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_USER_FRIENDS, "Encore Mode"),
                    FSUI_CSTR("When enabled, each session will behave as if no achievements have been unlocked."),
                    "Cheevos", "EncoreMode", false, enabled);
  DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_STETHOSCOPE, "Spectator Mode"),
                    FSUI_CSTR("When enabled, DuckStation will assume all achievements are locked and not send any "
                              "unlock notifications to the server."),
                    "Cheevos", "SpectatorMode", false, enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONSTR(ICON_FA_MEDAL, "Test Unofficial Achievements"),
    FSUI_CSTR("When enabled, DuckStation will list achievements from unofficial sets. These achievements are not "
              "tracked by RetroAchievements."),
    "Cheevos", "UnofficialTestMode", false, enabled);

  if (!IsEditingGameSettings(bsi))
  {
    MenuHeading(FSUI_CSTR("Account"));
    if (bsi->ContainsValue("Cheevos", "Token"))
    {
      ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::GetStyle().Colors[ImGuiCol_Text]);
      ActiveButton(SmallString::from_format(fmt::runtime(FSUI_ICONSTR(ICON_FA_USER, "Username: {}")),
                                            bsi->GetTinyStringValue("Cheevos", "Username")),
                   false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

      TinyString ts_string;
      ts_string.format(
        FSUI_FSTR("{:%Y-%m-%d %H:%M:%S}"),
        fmt::localtime(
          StringUtil::FromChars<u64>(bsi->GetTinyStringValue("Cheevos", "LoginTimestamp", "0")).value_or(0)));
      ActiveButton(
        SmallString::from_format(fmt::runtime(FSUI_ICONSTR(ICON_FA_CLOCK, "Login token generated on {}")), ts_string),
        false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
      ImGui::PopStyleColor();

      if (MenuButton(FSUI_ICONSTR(ICON_FA_KEY, "Logout"), FSUI_CSTR("Logs out of RetroAchievements.")))
      {
        Host::RunOnCPUThread(&Achievements::Logout);
      }
    }
    else
    {
      ActiveButton(FSUI_ICONSTR(ICON_FA_USER, "Not Logged In"), false, false,
                   ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

      if (MenuButton(FSUI_ICONSTR(ICON_FA_KEY, "Login"), FSUI_CSTR("Logs in to RetroAchievements.")))
      {
        s_state.achievements_login_window_open = true;
        QueueResetFocus(FocusResetType::PopupOpened);
      }

      if (s_state.achievements_login_window_open)
        DrawAchievementsLoginWindow();
    }

    MenuHeading(FSUI_CSTR("Current Game"));
    if (Achievements::HasActiveGame())
    {
      const auto lock = Achievements::GetLock();

      ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::GetStyle().Colors[ImGuiCol_Text]);
      ActiveButton(SmallString::from_format(fmt::runtime(FSUI_ICONSTR(ICON_FA_BOOKMARK, "Game: {} ({})")),
                                            Achievements::GetGameID(), Achievements::GetGameTitle()),
                   false, false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

      const std::string& rich_presence_string = Achievements::GetRichPresenceString();
      if (!rich_presence_string.empty())
      {
        ActiveButton(SmallString::from_format(ICON_FA_MAP "{}", rich_presence_string), false, false,
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
  }

  EndMenuButtons();
}

void FullscreenUI::DrawAchievementsLoginWindow()
{
  static constexpr const char* LOGIN_PROGRESS_NAME = "AchievementsLogin";

  static char username[256] = {};
  static char password[256] = {};

  static constexpr auto actually_close_popup = []() {
    std::memset(username, 0, sizeof(username));
    std::memset(password, 0, sizeof(password));
    s_state.achievements_login_window_open = false;
    QueueResetFocus(FocusResetType::PopupClosed);
  };

  ImGui::SetNextWindowSize(LayoutScale(700.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));
  ImGui::PushFont(UIStyle.LargeFont);

  const char* popup_title = FSUI_CSTR("RetroAchievements Login");
  bool popup_closed = false;
  ImGui::OpenPopup(popup_title);
  if (ImGui::BeginPopupModal(popup_title, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    ImGui::TextWrapped(
      FSUI_CSTR("Please enter your user name and password for retroachievements.org below. Your password will "
                "not be saved in DuckStation, an access token will be generated and used instead."));

    ImGui::NewLine();

    const bool is_logging_in = ImGuiFullscreen::IsBackgroundProgressDialogOpen(LOGIN_PROGRESS_NAME);
    ResetFocusHere();

    ImGui::Text(FSUI_CSTR("User Name: "));
    ImGui::SameLine(LayoutScale(200.0f));
    ImGui::InputText("##username", username, sizeof(username), is_logging_in ? ImGuiInputTextFlags_ReadOnly : 0);

    ImGui::Text(FSUI_CSTR("Password: "));
    ImGui::SameLine(LayoutScale(200.0f));
    ImGui::InputText("##password", password, sizeof(password),
                     is_logging_in ? (ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_Password) :
                                     ImGuiInputTextFlags_Password);

    ImGui::NewLine();

    BeginMenuButtons();

    const bool login_enabled = (std::strlen(username) > 0 && std::strlen(password) > 0 && !is_logging_in);

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_KEY, "Login"), false, login_enabled))
    {
      ImGuiFullscreen::OpenBackgroundProgressDialog(LOGIN_PROGRESS_NAME, FSUI_STR("Logging in to RetroAchievements..."),
                                                    0, 0, 0);

      Host::RunOnCPUThread([username = std::string(username), password = std::string(password)]() {
        Error error;
        const bool result = Achievements::Login(username.c_str(), password.c_str(), &error);
        GPUThread::RunOnThread([result, error = std::move(error)]() {
          ImGuiFullscreen::CloseBackgroundProgressDialog(LOGIN_PROGRESS_NAME);

          if (result)
          {
            actually_close_popup();
            return;
          }

          // keep popup open on failure
          // because of the whole popup stack thing, we need to hide the dialog while this popup is visible
          s_state.achievements_login_window_open = false;
          ImGuiFullscreen::OpenInfoMessageDialog(
            FSUI_STR("Login Error"),
            fmt::format(FSUI_FSTR("Login Failed.\nError: {}\nPlease check your username and password, and try again."),
                        error.GetDescription()),
            []() {
              s_state.achievements_login_window_open = true;
              QueueResetFocus(FocusResetType::PopupOpened);
            },
            FSUI_ICONSTR(ICON_FA_TIMES, "Close"));
        });
      });
    }

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_TIMES, "Cancel"), false, !is_logging_in))
      popup_closed = true;

    popup_closed = popup_closed || (!is_logging_in && WantsToCloseMenu());
    if (popup_closed)
      ImGui::CloseCurrentPopup();

    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopFont();
  ImGui::PopStyleVar(2);

  if (popup_closed)
    actually_close_popup();
}

void FullscreenUI::DrawAdvancedSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading(FSUI_CSTR("Logging Settings"));
  DrawEnumSetting(bsi, FSUI_CSTR("Log Level"),
                  FSUI_CSTR("Sets the verbosity of messages logged. Higher levels will log more messages."), "Logging",
                  "LogLevel", Settings::DEFAULT_LOG_LEVEL, &Settings::ParseLogLevelName, &Settings::GetLogLevelName,
                  &Settings::GetLogLevelDisplayName, Log::Level::MaxCount);
  DrawToggleSetting(bsi, FSUI_CSTR("Log To System Console"), FSUI_CSTR("Logs messages to the console window."),
                    FSUI_CSTR("Logging"), "LogToConsole", false);
  DrawToggleSetting(bsi, FSUI_CSTR("Log To Debug Console"),
                    FSUI_CSTR("Logs messages to the debug console where supported."), "Logging", "LogToDebug", false);
  DrawToggleSetting(bsi, FSUI_CSTR("Log To File"), FSUI_CSTR("Logs messages to duckstation.log in the user directory."),
                    "Logging", "LogToFile", false);

  MenuHeading(FSUI_CSTR("Debugging Settings"));

  DrawToggleSetting(bsi, FSUI_CSTR("Use Debug GPU Device"),
                    FSUI_CSTR("Enable debugging when supported by the host's renderer API. Only for developer use."),
                    "GPU", "UseDebugDevice", false);

  DrawToggleSetting(bsi, FSUI_CSTR("Allow Booting Without SBI File"),
                    FSUI_CSTR("Allows loading protected games without subchannel information."), "CDROM",
                    "AllowBootingWithoutSBIFile", false);

  DrawToggleSetting(
    bsi, FSUI_CSTR("Load Devices From Save States"),
    FSUI_CSTR("When enabled, memory cards and controllers will be overwritten when save states are loaded."), "Main",
    "LoadDevicesFromSaveStates", false);
  DrawEnumSetting(bsi, FSUI_CSTR("Save State Compression"),
                  FSUI_CSTR("Reduces the size of save states by compressing the data before saving."), "Main",
                  "SaveStateCompression", Settings::DEFAULT_SAVE_STATE_COMPRESSION_MODE,
                  &Settings::ParseSaveStateCompressionModeName, &Settings::GetSaveStateCompressionModeName,
                  &Settings::GetSaveStateCompressionModeDisplayName, SaveStateCompressionMode::Count);

  MenuHeading(FSUI_CSTR("Display Settings"));
  DrawToggleSetting(bsi, FSUI_CSTR("Threaded Rendering"),
                    FSUI_CSTR("Uses a second thread for drawing graphics. Speed boost, and safe to use."), "GPU",
                    "UseThread", true);
  DrawEnumSetting(bsi, FSUI_CSTR("Wireframe Rendering"),
                  FSUI_CSTR("Overlays or replaces normal triangle drawing with a wireframe/line view."), "GPU",
                  "WireframeMode", GPUWireframeMode::Disabled, &Settings::ParseGPUWireframeMode,
                  &Settings::GetGPUWireframeModeName, &Settings::GetGPUWireframeModeDisplayName,
                  GPUWireframeMode::Count);

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

  MenuHeading(FSUI_CSTR("CD-ROM Emulation"));

  DrawToggleSetting(bsi, FSUI_CSTR("Enable Region Check"),
                    FSUI_CSTR("Simulates the region check present in original, unmodified consoles."), "CDROM",
                    "RegionCheck", false);

  EndMenuButtons();
}

void FullscreenUI::DrawPatchesOrCheatsSettingsPage(bool cheats)
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  const Cheats::CodeInfoList& code_list = cheats ? s_state.game_cheats_list : s_state.game_patch_list;
  std::vector<std::string>& enable_list = cheats ? s_state.enabled_game_cheat_cache : s_state.enabled_game_patch_cache;
  const char* section = cheats ? Cheats::CHEATS_CONFIG_SECTION : Cheats::PATCHES_CONFIG_SECTION;

  BeginMenuButtons();

  static constexpr auto draw_code = [](SettingsInterface* bsi, const char* section, const Cheats::CodeInfo& ci,
                                       std::vector<std::string>& enable_list, bool cheats) {
    const auto enable_it = std::find(enable_list.begin(), enable_list.end(), ci.name);

    SmallString title;
    if (!cheats)
      title = std::string_view(ci.name);
    else
      title.format("{}##{}", ci.GetNamePart(), ci.name);

    bool state = (enable_it != enable_list.end());

    if (ci.HasOptionChoices())
    {
      TinyString visible_value(FSUI_VSTR("Disabled"));
      bool has_option = false;
      if (state)
      {
        // Need to map the value to an option.
        visible_value = ci.MapOptionValueToName(bsi->GetTinyStringValue(section, ci.name.c_str()));
        has_option = true;
      }

      if (MenuButtonWithValue(title.c_str(), ci.description.c_str(), visible_value))
      {
        ImGuiFullscreen::ChoiceDialogOptions options;
        options.reserve(ci.options.size() + 1);
        options.emplace_back(FSUI_VSTR("Disabled"), !has_option);

        for (const Cheats::CodeOption& opt : ci.options)
          options.emplace_back(opt.first, has_option && (visible_value.view() == opt.first));

        OpenChoiceDialog(ci.name, false, std::move(options),
                         [cheat_name = ci.name, cheats, section](s32 index, const std::string& title, bool checked) {
                           if (index >= 0)
                           {
                             const Cheats::CodeInfo* ci = Cheats::FindCodeInInfoList(
                               cheats ? s_state.game_cheats_list : s_state.game_patch_list, cheat_name);
                             if (ci)
                             {
                               SettingsInterface* bsi = GetEditingSettingsInterface();
                               std::vector<std::string>& enable_list =
                                 cheats ? s_state.enabled_game_cheat_cache : s_state.enabled_game_patch_cache;
                               const auto it = std::find(enable_list.begin(), enable_list.end(), ci->name);
                               if (index == 0)
                               {
                                 bsi->RemoveFromStringList(section, Cheats::PATCH_ENABLE_CONFIG_KEY, ci->name.c_str());
                                 if (it != enable_list.end())
                                   enable_list.erase(it);
                               }
                               else
                               {
                                 bsi->AddToStringList(section, Cheats::PATCH_ENABLE_CONFIG_KEY, ci->name.c_str());
                                 bsi->SetUIntValue(section, ci->name.c_str(), ci->MapOptionNameToValue(title));
                                 if (it == enable_list.end())
                                   enable_list.push_back(std::move(cheat_name));
                               }

                               SetSettingsChanged(bsi);
                             }
                           }

                           CloseChoiceDialog();
                         });
      }
    }
    else if (ci.HasOptionRange())
    {
      TinyString visible_value(FSUI_VSTR("Disabled"));
      s32 range_value = static_cast<s32>(ci.option_range_start) - 1;
      if (state)
      {
        const std::optional<s32> value =
          bsi->GetOptionalIntValue(section, ci.name.c_str(), std::nullopt).value_or(ci.option_range_start);
        if (value.has_value())
        {
          range_value = value.value();
          visible_value.format("{}", value.value());
        }
      }

      constexpr s32 step_value = 1;

      if (MenuButtonWithValue(title.c_str(), ci.description.c_str(), visible_value.c_str()))
        ImGui::OpenPopup(title);

      ImGui::SetNextWindowSize(LayoutScale(500.0f, 194.0f));
      ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

      ImGui::PushFont(UIStyle.LargeFont);
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

        bool range_value_changed = false;

        const ImVec2& padding(ImGui::GetStyle().FramePadding);
        ImVec2 button_pos(ImGui::GetCursorPos());

        // Align value text in middle.
        ImGui::SetCursorPosY(button_pos.y + ((LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + padding.y * 2.0f) -
                                             UIStyle.LargeFont->FontSize) *
                                              0.5f);
        ImGui::TextUnformatted(visible_value.c_str(), visible_value.end_ptr());

        s32 step = 0;
        if (FloatingButton(ICON_FA_CHEVRON_UP, padding.x, button_pos.y, -1.0f, -1.0f, 1.0f, 0.0f, true,
                           UIStyle.LargeFont, &button_pos, true))
        {
          step = step_value;
        }
        if (FloatingButton(ICON_FA_CHEVRON_DOWN, button_pos.x - padding.x, button_pos.y, -1.0f, -1.0f, -1.0f, 0.0f,
                           true, UIStyle.LargeFont, &button_pos, true))
        {
          step = -step_value;
        }
        if (FloatingButton(ICON_FA_TRASH, button_pos.x - padding.x, button_pos.y, -1.0f, -1.0f, -1.0f, 0.0f, true,
                           UIStyle.LargeFont, &button_pos))
        {
          range_value = ci.option_range_start - 1;
          range_value_changed = true;
        }

        if (step != 0)
        {
          range_value += step;
          range_value_changed = true;
        }

        ImGui::SetCursorPosY(button_pos.y + (padding.y * 2.0f) +
                             LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY + 10.0f));

        if (range_value_changed)
        {
          const auto it = std::find(enable_list.begin(), enable_list.end(), ci.name);
          range_value =
            std::clamp(range_value, static_cast<s32>(ci.option_range_start) - 1, static_cast<s32>(ci.option_range_end));
          if (range_value < static_cast<s32>(ci.option_range_start))
          {
            bsi->RemoveFromStringList(section, Cheats::PATCH_ENABLE_CONFIG_KEY, ci.name.c_str());
            if (it != enable_list.end())
              enable_list.erase(it);
          }
          else
          {
            bsi->AddToStringList(section, Cheats::PATCH_ENABLE_CONFIG_KEY, ci.name.c_str());
            bsi->SetIntValue(section, ci.name.c_str(), range_value);
            if (it == enable_list.end())
              enable_list.push_back(ci.name);
          }

          SetSettingsChanged(bsi);
        }

        if (MenuButtonWithoutSummary(FSUI_CSTR("OK"), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, UIStyle.LargeFont,
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
    else
    {
      const bool changed = ToggleButton(title.c_str(), ci.description.c_str(), &state);
      if (changed)
      {
        if (state)
        {
          bsi->AddToStringList(section, Cheats::PATCH_ENABLE_CONFIG_KEY, ci.name.c_str());
          enable_list.push_back(ci.name);
        }
        else
        {
          bsi->RemoveFromStringList(section, Cheats::PATCH_ENABLE_CONFIG_KEY, ci.name.c_str());
          enable_list.erase(enable_it);
        }

        SetSettingsChanged(bsi);
      }
    }
  };

  if (cheats)
  {
    ActiveButton(
      FSUI_ICONSTR(
        ICON_EMOJI_WARNING,
        "WARNING: Activating cheats can cause unpredictable behavior, crashing, soft-locks, or broken saved games."),
      false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    MenuHeading(FSUI_CSTR("Settings"));

    bool enable_cheats = bsi->GetBoolValue("Cheats", "EnableCheats", false);
    if (ToggleButton(FSUI_ICONSTR(ICON_FA_FLASK, "Enable Cheats"),
                     FSUI_CSTR("Enables the cheats that are selected below."), &enable_cheats))
    {
      if (enable_cheats)
        bsi->SetBoolValue("Cheats", "EnableCheats", true);
      else
        bsi->DeleteValue("Cheats", "EnableCheats");
      SetSettingsChanged(bsi);
    }

    bool load_database_cheats = bsi->GetBoolValue("Cheats", "LoadCheatsFromDatabase", true);
    if (ToggleButton(FSUI_ICONSTR(ICON_FA_DATABASE, "Load Database Cheats"),
                     FSUI_CSTR("Enables loading of cheats for this game from DuckStation's database."),
                     &load_database_cheats))
    {
      if (load_database_cheats)
        bsi->DeleteValue("Cheats", "LoadCheatsFromDatabase");
      else
        bsi->SetBoolValue("Cheats", "LoadCheatsFromDatabase", false);
      SetSettingsChanged(bsi);
      if (s_state.game_settings_entry)
        PopulatePatchesAndCheatsList(s_state.game_settings_entry->serial);
    }

    if (code_list.empty())
    {
      ActiveButton(FSUI_ICONSTR(ICON_FA_STORE_ALT_SLASH, "No cheats are available for this game."), false, false,
                   ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    }
    else
    {
      for (const std::string_view& group : s_state.game_cheat_groups)
      {
        if (group.empty())
          MenuHeading(FSUI_CSTR("Ungrouped"));
        else
          MenuHeading(SmallString(group).c_str());

        for (const Cheats::CodeInfo& ci : code_list)
        {
          if (ci.GetNameParentPart() != group)
            continue;

          draw_code(bsi, section, ci, enable_list, true);
        }
      }
    }
  }
  else
  {
    ActiveButton(
      FSUI_ICONSTR(ICON_EMOJI_WARNING,
                   "WARNING: Activating game patches can cause unpredictable behavior, crashing, soft-locks, or broken "
                   "saved games."),
      false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    if (code_list.empty())
    {
      ActiveButton(FSUI_ICONSTR(ICON_FA_STORE_ALT_SLASH, "No patches are available for this game."), false, false,
                   ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    }
    else
    {
      MenuHeading(FSUI_CSTR("Game Patches"));

      for (const Cheats::CodeInfo& ci : code_list)
        draw_code(bsi, section, ci, enable_list, false);
    }
  }

  EndMenuButtons();
}

void FullscreenUI::DrawPauseMenu()
{
  SmallString buffer;

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImVec2 display_size(ImGui::GetIO().DisplaySize);
  const ImU32 text_color = ImGui::GetColorU32(UIStyle.BackgroundTextColor) | IM_COL32_A_MASK;
  dl->AddRectFilled(ImVec2(0.0f, 0.0f), display_size,
                    (ImGui::GetColorU32(UIStyle.BackgroundColor) & ~IM_COL32_A_MASK) | (200 << IM_COL32_A_SHIFT));

  // title info
  {
    const std::string& title = System::GetGameTitle();
    const std::string& serial = System::GetGameSerial();

    if (!serial.empty())
      buffer.format("{} - ", serial);
    buffer.append(Path::GetFileName(System::GetDiscPath()));

    const float image_width = 60.0f;
    const float image_height = 60.0f;

    const ImVec2 title_size(UIStyle.LargeFont->CalcTextSizeA(UIStyle.LargeFont->FontSize,
                                                             std::numeric_limits<float>::max(), -1.0f, title.c_str()));
    const ImVec2 subtitle_size(UIStyle.MediumFont->CalcTextSizeA(
      UIStyle.MediumFont->FontSize, std::numeric_limits<float>::max(), -1.0f, buffer.c_str()));

    ImVec2 title_pos(display_size.x - LayoutScale(10.0f + image_width + 20.0f) - title_size.x,
                     display_size.y - LayoutScale(LAYOUT_FOOTER_HEIGHT) - LayoutScale(10.0f + image_height));
    ImVec2 subtitle_pos(display_size.x - LayoutScale(10.0f + image_width + 20.0f) - subtitle_size.x,
                        title_pos.y + UIStyle.LargeFont->FontSize + LayoutScale(4.0f));

    float rp_height = 0.0f;
    {
      const auto lock = Achievements::GetLock();
      const std::string& rp = Achievements::IsActive() ? Achievements::GetRichPresenceString() : std::string();

      if (!rp.empty())
      {
        const float wrap_width = LayoutScale(350.0f);
        const ImVec2 rp_size =
          UIStyle.MediumFont->CalcTextSizeA(UIStyle.MediumFont->FontSize, std::numeric_limits<float>::max(), wrap_width,
                                            rp.data(), rp.data() + rp.length());

        // Add a small extra gap if any Rich Presence is displayed
        rp_height = rp_size.y - UIStyle.MediumFont->FontSize + LayoutScale(2.0f);

        const ImVec2 rp_pos(display_size.x - LayoutScale(20.0f + 50.0f + 20.0f) - rp_size.x,
                            subtitle_pos.y + UIStyle.MediumFont->FontSize + LayoutScale(4.0f) - rp_height);

        title_pos.y -= rp_height;
        subtitle_pos.y -= rp_height;

        DrawShadowedText(dl, UIStyle.MediumFont, rp_pos, text_color, rp.data(), rp.data() + rp.length(), wrap_width);
      }
    }

    DrawShadowedText(dl, UIStyle.LargeFont, title_pos, text_color, title.c_str());
    DrawShadowedText(dl, UIStyle.MediumFont, subtitle_pos, text_color, buffer.c_str());

    GPUTexture* const cover = GetCoverForCurrentGame();
    const ImVec2 image_min(display_size.x - LayoutScale(10.0f + image_width),
                           display_size.y - LayoutScale(LAYOUT_FOOTER_HEIGHT) - LayoutScale(10.0f + image_height) -
                             rp_height);
    const ImVec2 image_max(image_min.x + LayoutScale(image_width), image_min.y + LayoutScale(image_height) + rp_height);
    const ImRect image_rect(CenterImage(ImRect(image_min, image_max), ImVec2(static_cast<float>(cover->GetWidth()),
                                                                             static_cast<float>(cover->GetHeight()))));
    dl->AddImage(cover, image_rect.Min, image_rect.Max);
  }

  // current time / play time
  {
    buffer.format("{:%X}", fmt::localtime(std::time(nullptr)));

    const ImVec2 time_size(UIStyle.LargeFont->CalcTextSizeA(
      UIStyle.LargeFont->FontSize, std::numeric_limits<float>::max(), -1.0f, buffer.c_str(), buffer.end_ptr()));
    const ImVec2 time_pos(display_size.x - LayoutScale(10.0f) - time_size.x, LayoutScale(10.0f));
    DrawShadowedText(dl, UIStyle.LargeFont, time_pos, text_color, buffer.c_str(), buffer.end_ptr());

    const std::string& serial = System::GetGameSerial();
    if (!serial.empty())
    {
      const std::time_t cached_played_time = GameList::GetCachedPlayedTimeForSerial(serial);
      const std::time_t session_time = static_cast<std::time_t>(System::GetSessionPlayedTime());

      buffer.format(FSUI_FSTR("Session: {}"), GameList::FormatTimespan(session_time, true));
      const ImVec2 session_size(UIStyle.MediumFont->CalcTextSizeA(
        UIStyle.MediumFont->FontSize, std::numeric_limits<float>::max(), -1.0f, buffer.c_str(), buffer.end_ptr()));
      const ImVec2 session_pos(display_size.x - LayoutScale(10.0f) - session_size.x,
                               time_pos.y + UIStyle.LargeFont->FontSize + LayoutScale(4.0f));
      DrawShadowedText(dl, UIStyle.MediumFont, session_pos, text_color, buffer.c_str(), buffer.end_ptr());

      buffer.format(FSUI_FSTR("All Time: {}"), GameList::FormatTimespan(cached_played_time + session_time, true));
      const ImVec2 total_size(UIStyle.MediumFont->CalcTextSizeA(
        UIStyle.MediumFont->FontSize, std::numeric_limits<float>::max(), -1.0f, buffer.c_str(), buffer.end_ptr()));
      const ImVec2 total_pos(display_size.x - LayoutScale(10.0f) - total_size.x,
                             session_pos.y + UIStyle.MediumFont->FontSize + LayoutScale(4.0f));
      DrawShadowedText(dl, UIStyle.MediumFont, total_pos, text_color, buffer.c_str(), buffer.end_ptr());
    }
  }

  const ImVec2 window_size(LayoutScale(500.0f, LAYOUT_SCREEN_HEIGHT));
  const ImVec2 window_pos(0.0f, display_size.y - LayoutScale(LAYOUT_FOOTER_HEIGHT) - window_size.y);

  if (BeginFullscreenWindow(window_pos, window_size, "pause_menu", ImVec4(0.0f, 0.0f, 0.0f, 0.0f), 0.0f,
                            ImVec2(10.0f, 10.0f), ImGuiWindowFlags_NoBackground))
  {
    static constexpr u32 submenu_item_count[] = {
      11, // None
      4,  // Exit
      3,  // Achievements
    };

    ResetFocusHere();
    BeginMenuButtons(submenu_item_count[static_cast<u32>(s_state.current_pause_submenu)], 1.0f,
                     ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING, ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING,
                     ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    switch (s_state.current_pause_submenu)
    {
      case PauseSubMenu::None:
      {
        // NOTE: Menu close must come first, because otherwise VM destruction options will race.
        const bool has_game = GPUThread::HasGPUBackend() && !System::GetGameSerial().empty();

        if (DefaultActiveButton(FSUI_ICONSTR(ICON_FA_PLAY, "Resume Game"), false) || WantsToCloseMenu())
          ClosePauseMenu();

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_FAST_FORWARD, "Toggle Fast Forward"), false))
        {
          ClosePauseMenu();
          DoToggleFastForward();
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_UNDO, "Load State"), false, has_game))
        {
          if (OpenSaveStateSelector(true))
            s_state.current_main_window = MainWindowType::None;
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_DOWNLOAD, "Save State"), false, has_game))
        {
          if (OpenSaveStateSelector(false))
            s_state.current_main_window = MainWindowType::None;
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

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_TROPHY, "Achievements"), false,
                         Achievements::HasAchievementsOrLeaderboards()))
        {
          // skip second menu and go straight to cheevos if there's no lbs
          if (!Achievements::HasLeaderboards())
            OpenAchievementsWindow();
          else
            OpenPauseSubMenu(PauseSubMenu::Achievements);
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_CAMERA, "Save Screenshot"), false))
        {
          System::SaveScreenshot();
          ClosePauseMenu();
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Change Disc"), false))
        {
          s_state.current_main_window = MainWindowType::None;
          DoChangeDisc();
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_SLIDERS_H, "Settings"), false))
          SwitchToSettings();

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_POWER_OFF, "Close Game"), false))
        {
          // skip submenu when we can't save anyway
          if (!has_game)
            RequestShutdown(false);
          else
            OpenPauseSubMenu(PauseSubMenu::Exit);
        }
      }
      break;

      case PauseSubMenu::Exit:
      {
        if (ActiveButton(FSUI_ICONSTR(ICON_FA_BACKWARD, "Back To Pause Menu"), false) || WantsToCloseMenu())
          OpenPauseSubMenu(PauseSubMenu::None);

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_SYNC, "Reset System"), false))
        {
          ClosePauseMenu();
          RequestReset();
        }

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_SAVE, "Exit And Save State"), false))
          RequestShutdown(true);

        if (DefaultActiveButton(FSUI_ICONSTR(ICON_FA_POWER_OFF, "Exit Without Saving"), false))
          RequestShutdown(false);
      }
      break;

      case PauseSubMenu::Achievements:
      {
        if (ActiveButton(FSUI_ICONSTR(ICON_FA_BACKWARD, "Back To Pause Menu"), false) || WantsToCloseMenu())
          OpenPauseSubMenu(PauseSubMenu::None);

        if (DefaultActiveButton(FSUI_ICONSTR(ICON_FA_TROPHY, "Achievements"), false))
          OpenAchievementsWindow();

        if (ActiveButton(FSUI_ICONSTR(ICON_FA_STOPWATCH, "Leaderboards"), false))
          OpenLeaderboardsWindow();
      }
      break;
    }

    EndMenuButtons();

    EndFullscreenWindow();
  }

  Achievements::DrawPauseMenuOverlays();

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

void FullscreenUI::InitializePlaceholderSaveStateListEntry(SaveStateListEntry* li, const std::string& serial, s32 slot,
                                                           bool global)
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

bool FullscreenUI::InitializeSaveStateListEntryFromSerial(SaveStateListEntry* li, const std::string& serial, s32 slot,
                                                          bool global)
{
  const std::string path =
    (global ? System::GetGlobalSaveStateFileName(slot) : System::GetGameSaveStateFileName(serial, slot));
  if (!InitializeSaveStateListEntryFromPath(li, path.c_str(), slot, global))
  {
    InitializePlaceholderSaveStateListEntry(li, serial, slot, global);
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

  li->summary = fmt::format(FSUI_FSTR("Saved {:%c}"), fmt::localtime(ssi->timestamp));
  li->timestamp = ssi->timestamp;
  li->slot = slot;
  li->path = std::move(path);
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

u32 FullscreenUI::PopulateSaveStateListEntries(const std::string& title, const std::string& serial)
{
  ClearSaveStateEntryList();

  if (s_state.save_state_selector_loading)
  {
    std::optional<ExtendedSaveStateInfo> ssi = System::GetUndoSaveStateInfo();
    if (ssi)
    {
      SaveStateListEntry li;
      li.title = FSUI_STR("Undo Load State");
      li.summary = FSUI_STR("Restores the state of the system prior to the last state loaded.");
      if (ssi->screenshot.IsValid())
        li.preview_texture = g_gpu_device->FetchAndUploadTextureImage(ssi->screenshot);
      s_state.save_state_selector_slots.push_back(std::move(li));
    }
  }

  if (!serial.empty())
  {
    for (s32 i = 1; i <= System::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      SaveStateListEntry li;
      if (InitializeSaveStateListEntryFromSerial(&li, serial, i, false) || !s_state.save_state_selector_loading)
        s_state.save_state_selector_slots.push_back(std::move(li));
    }
  }

  for (s32 i = 1; i <= System::GLOBAL_SAVE_STATE_SLOTS; i++)
  {
    SaveStateListEntry li;
    if (InitializeSaveStateListEntryFromSerial(&li, serial, i, true) || !s_state.save_state_selector_loading)
      s_state.save_state_selector_slots.push_back(std::move(li));
  }

  return static_cast<u32>(s_state.save_state_selector_slots.size());
}

bool FullscreenUI::OpenLoadStateSelectorForGame(const std::string& game_path)
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(game_path);
  if (entry)
  {
    s_state.save_state_selector_loading = true;
    if (PopulateSaveStateListEntries(entry->title, entry->serial) > 0)
    {
      s_state.save_state_selector_open = true;
      s_state.save_state_selector_resuming = false;
      s_state.save_state_selector_game_path = game_path;
      return true;
    }
  }

  ShowToast({}, FSUI_STR("No save states found."), 5.0f);
  return false;
}

bool FullscreenUI::OpenSaveStateSelector(bool is_loading)
{
  s_state.save_state_selector_game_path = {};
  s_state.save_state_selector_loading = is_loading;
  s_state.save_state_selector_resuming = false;
  if (PopulateSaveStateListEntries(System::GetGameTitle(), System::GetGameSerial()) > 0)
  {
    s_state.save_state_selector_open = true;
    QueueResetFocus(FocusResetType::PopupOpened);
    return true;
  }

  ShowToast({}, FSUI_STR("No save states found."), 5.0f);
  return false;
}

void FullscreenUI::CloseSaveStateSelector()
{
  if (s_state.save_state_selector_open)
    QueueResetFocus(FocusResetType::PopupClosed);

  ClearSaveStateEntryList();
  s_state.save_state_selector_open = false;
  s_state.save_state_selector_loading = false;
  s_state.save_state_selector_resuming = false;
  s_state.save_state_selector_game_path = {};
}

void FullscreenUI::DrawSaveStateSelector(bool is_loading)
{
  ImGuiIO& io = ImGui::GetIO();

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize - LayoutScale(0.0f, LAYOUT_FOOTER_HEIGHT));

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
    {
      CloseSaveStateSelector();
      ReturnToPreviousWindow();
    }
    return;
  }

  const ImVec2 heading_size =
    ImVec2(io.DisplaySize.x, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) +
                               (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ModAlpha(UIStyle.PrimaryColor, 0.9f));

  bool closed = false;
  bool was_close_not_back = false;
  bool ignore_close_request = false;
  if (ImGui::BeginChild("state_titlebar", heading_size, false, ImGuiWindowFlags_NavFlattened))
  {
    BeginNavBar();
    if (NavButton(ICON_FA_BACKWARD, true, true))
      closed = true;

    NavTitle(is_loading ? FSUI_CSTR("Load State") : FSUI_CSTR("Save State"));
    EndNavBar();
    ImGui::EndChild();
  }

  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ModAlpha(UIStyle.BackgroundColor, 0.9f));
  ImGui::SetCursorPos(ImVec2(0.0f, heading_size.y));

  if (IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  if (ImGui::BeginChild("state_list",
                        ImVec2(io.DisplaySize.x, io.DisplaySize.y - LayoutScale(LAYOUT_FOOTER_HEIGHT) - heading_size.y),
                        false, ImGuiWindowFlags_NavFlattened))
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
    const float item_height = (style.FramePadding.y * 2.0f) + image_height + title_spacing +
                              UIStyle.LargeFont->FontSize + summary_spacing + UIStyle.MediumFont->FontSize;
    const ImVec2 item_size(item_width, item_height);
    const u32 grid_count_x = static_cast<u32>(std::floor(ImGui::GetWindowWidth() / item_width_with_spacing));
    const float start_x =
      (static_cast<float>(ImGui::GetWindowWidth()) - (item_width_with_spacing * static_cast<float>(grid_count_x))) *
      0.5f;

    u32 grid_x = 0;
    ImGui::SetCursorPos(ImVec2(start_x, 0.0f));
    for (u32 i = 0; i < s_state.save_state_selector_slots.size();)
    {
      SaveStateListEntry& entry = s_state.save_state_selector_slots[i];
      if (static_cast<s32>(i) == s_state.save_state_selector_submenu_index)
      {
        // can't use a choice dialog here, because we're already in a modal...
        ImGuiFullscreen::PushResetLayout();
        ImGui::PushFont(UIStyle.LargeFont);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.PrimaryTextColor);
        ImGui::PushStyleColor(ImGuiCol_TitleBg, UIStyle.PrimaryDarkColor);
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, UIStyle.PrimaryColor);

        const float width = LayoutScale(600.0f);
        const float title_height = UIStyle.LargeFont->FontSize + ImGui::GetStyle().FramePadding.y * 2.0f +
                                   ImGui::GetStyle().WindowPadding.y * 2.0f;
        const float height =
          title_height +
          ((LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f)) *
           3.0f);
        ImGui::SetNextWindowSize(ImVec2(width, height));
        ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup(entry.title.c_str());

        bool removed = false;
        if (ImGui::BeginPopupModal(entry.title.c_str(), &is_open,
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        {
          ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.BackgroundTextColor);

          BeginMenuButtons();

          if (ActiveButton(is_loading ? FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Load State") :
                                        FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Save State"),
                           false, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
          {
            if (is_loading)
              DoLoadState(std::move(entry.path));
            else
              DoSaveState(entry.slot, entry.global);

            closed = true;
            was_close_not_back = true;
          }

          if (!entry.path.empty() && ActiveButton(FSUI_ICONSTR(ICON_FA_FOLDER_MINUS, "Delete Save"), false, true,
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
              s_state.save_state_selector_slots.erase(s_state.save_state_selector_slots.begin() + i);
              removed = true;

              if (s_state.save_state_selector_slots.empty())
              {
                closed = true;
                was_close_not_back = true;
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
                           LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) ||
              WantsToCloseMenu())
          {
            is_open = false;
            ignore_close_request = true;
          }

          EndMenuButtons();

          ImGui::PopStyleColor();
          ImGui::EndPopup();
        }

        if (!is_open)
          s_state.save_state_selector_submenu_index = -1;

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(3);
        ImGui::PopFont();
        ImGuiFullscreen::PopResetLayout();

        if (removed)
          continue;
      }

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

          ImGuiFullscreen::DrawMenuButtonFrame(bb.Min, bb.Max, col, true, 0.0f);

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
        const ImRect title_bb(title_pos, ImVec2(bb.Max.x, title_pos.y + UIStyle.LargeFont->FontSize));
        ImGui::PushFont(UIStyle.LargeFont);
        ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, entry.title.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f),
                                 &title_bb);
        ImGui::PopFont();

        if (!entry.summary.empty())
        {
          const ImVec2 summary_pos(bb.Min.x, title_pos.y + UIStyle.LargeFont->FontSize + summary_spacing);
          const ImRect summary_bb(summary_pos, ImVec2(bb.Max.x, summary_pos.y + UIStyle.MediumFont->FontSize));
          ImGui::PushFont(UIStyle.MediumFont);
          ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, entry.summary.c_str(), nullptr, nullptr,
                                   ImVec2(0.0f, 0.0f), &summary_bb);
          ImGui::PopFont();
        }

        if (pressed)
        {
          if (is_loading)
            DoLoadState(entry.path);
          else
            DoSaveState(entry.slot, entry.global);

          closed = true;
          was_close_not_back = true;
        }
        else if (hovered &&
                 (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                  ImGui::IsKeyPressed(ImGuiKey_NavGamepadInput, false) || ImGui::IsKeyPressed(ImGuiKey_F1, false)))
        {
          s_state.save_state_selector_submenu_index = static_cast<s32>(i);
        }
      }

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

      i++;
    }

    EndMenuButtons();
    ImGui::EndChild();
  }

  ImGui::PopStyleColor();

  ImGui::EndPopup();
  ImGui::PopStyleVar(5);

  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_XBOX_DPAD, FSUI_VSTR("Select State")),
                 std::make_pair(ICON_PF_BUTTON_Y, FSUI_VSTR("Delete State")),
                 std::make_pair(ICON_PF_BUTTON_A, is_loading ? FSUI_VSTR("Load State") : FSUI_VSTR("Save State")),
                 std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Cancel"))});
  }
  else
  {
    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT,
                                FSUI_VSTR("Select State")),
                 std::make_pair(ICON_PF_F1, FSUI_VSTR("Delete State")),
                 std::make_pair(ICON_PF_ENTER, is_loading ? FSUI_VSTR("Load State") : FSUI_VSTR("Save State")),
                 std::make_pair(ICON_PF_ESC, FSUI_VSTR("Cancel"))});
  }

  if ((!ignore_close_request && WantsToCloseMenu()) || closed)
  {
    CloseSaveStateSelector();
    if (was_close_not_back)
      ReturnToMainWindow();
    else if (s_state.current_main_window != MainWindowType::GameList)
      ReturnToPreviousWindow();
  }
}

bool FullscreenUI::OpenLoadStateSelectorForGameResume(const GameList::Entry* entry)
{
  SaveStateListEntry slentry;
  if (!InitializeSaveStateListEntryFromSerial(&slentry, entry->serial, -1, false))
    return false;

  CloseSaveStateSelector();
  s_state.save_state_selector_slots.push_back(std::move(slentry));
  s_state.save_state_selector_game_path = entry->path;
  s_state.save_state_selector_loading = true;
  s_state.save_state_selector_open = true;
  s_state.save_state_selector_resuming = true;
  QueueResetFocus(FocusResetType::PopupOpened);
  return true;
}

void FullscreenUI::DrawResumeStateSelector()
{
  ImGui::SetNextWindowSize(LayoutScale(800.0f, 602.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup(FSUI_CSTR("Load Resume State"));

  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  bool is_open = true;
  if (ImGui::BeginPopupModal(FSUI_CSTR("Load Resume State"), &is_open,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    SaveStateListEntry& entry = s_state.save_state_selector_slots.front();
    SmallString time;
    TimeToPrintableString(&time, entry.timestamp);
    ImGui::TextWrapped(
      FSUI_CSTR("A resume save state created at %s was found.\n\nDo you want to load this save and continue?"),
      time.c_str());

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
      DoStartPath(s_state.save_state_selector_game_path, std::move(entry.path));
      is_open = false;
    }

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_LIGHTBULB, "Clean Boot"), false))
    {
      DoStartPath(s_state.save_state_selector_game_path);
      is_open = false;
    }

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_FOLDER_MINUS, "Delete State"), false))
    {
      if (FileSystem::DeleteFile(entry.path.c_str()))
      {
        DoStartPath(s_state.save_state_selector_game_path);
        is_open = false;
      }
      else
      {
        ShowToast(std::string(), FSUI_STR("Failed to delete save state."));
      }
    }

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_WINDOW_CLOSE, "Cancel"), false) || WantsToCloseMenu())
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
    s_state.save_state_selector_open = false;
    s_state.save_state_selector_loading = false;
    s_state.save_state_selector_resuming = false;
    s_state.save_state_selector_game_path = {};
  }
  else
  {
    SetStandardSelectionFooterText(false);
  }
}

void FullscreenUI::DoLoadState(std::string path)
{
  Host::RunOnCPUThread([boot_path = s_state.save_state_selector_game_path, path = std::move(path)]() {
    CloseSaveStateSelector();

    if (System::IsValid())
    {
      if (path.empty())
      {
        // Loading undo state.
        if (!System::UndoLoadState())
          ShowToast(std::string(), TRANSLATE_STR("System", "Failed to undo load state."));
      }
      else
      {
        Error error;
        if (!System::LoadState(path.c_str(), &error, true))
        {
          ShowToast(std::string(),
                    fmt::format(TRANSLATE_FS("System", "Failed to load state: {}"), error.GetDescription()));
        }
      }
    }
    else
    {
      DoStartPath(std::move(boot_path), std::move(path));
    }
  });
}

void FullscreenUI::DoSaveState(s32 slot, bool global)
{
  Host::RunOnCPUThread([slot, global]() {
    CloseSaveStateSelector();
    if (!System::IsValid())
      return;

    std::string path(global ? System::GetGlobalSaveStateFileName(slot) :
                              System::GetGameSaveStateFileName(System::GetGameSerial(), slot));
    Error error;
    if (!System::SaveState(std::move(path), &error, g_settings.create_save_state_backups, false))
    {
      ShowToast(std::string(), fmt::format(TRANSLATE_FS("System", "Failed to save state: {}"), error.GetDescription()));
    }
  });
}

void FullscreenUI::PopulateGameListEntryList()
{
  const s32 sort = Host::GetBaseIntSettingValue("Main", "FullscreenUIGameSort", 0);
  const bool reverse = Host::GetBaseBoolSettingValue("Main", "FullscreenUIGameSortReverse", false);
  const bool merge_disc_sets = Host::GetBaseBoolSettingValue("Main", "FullscreenUIMergeDiscSets", true);

  const u32 count = GameList::GetEntryCount();
  s_state.game_list_sorted_entries.clear();
  s_state.game_list_sorted_entries.reserve(count);
  for (u32 i = 0; i < count; i++)
  {
    const GameList::Entry* entry = GameList::GetEntryByIndex(i);
    if (merge_disc_sets)
    {
      if (entry->disc_set_member)
        continue;
    }
    else
    {
      if (entry->IsDiscSet())
        continue;
    }

    s_state.game_list_sorted_entries.push_back(entry);
  }

  std::sort(s_state.game_list_sorted_entries.begin(), s_state.game_list_sorted_entries.end(),
            [sort, reverse](const GameList::Entry* lhs, const GameList::Entry* rhs) {
              switch (sort)
              {
                case 0: // Type
                {
                  const GameList::EntryType lst = lhs->GetSortType();
                  const GameList::EntryType rst = rhs->GetSortType();
                  if (lst != rst)
                    return reverse ? (lst > rst) : (lst < rst);
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

                case 6: // File Size
                {
                  if (lhs->file_size != rhs->file_size)
                  {
                    return reverse ? (lhs->file_size > rhs->file_size) : (lhs->file_size < rhs->file_size);
                  }
                }
                break;

                case 7: // Uncompressed Size
                {
                  if (lhs->uncompressed_size != rhs->uncompressed_size)
                  {
                    return reverse ? (lhs->uncompressed_size > rhs->uncompressed_size) :
                                     (lhs->uncompressed_size < rhs->uncompressed_size);
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
  const ImVec2 heading_size =
    ImVec2(io.DisplaySize.x, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) +
                               (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));

  const float bg_alpha = GPUThread::HasGPUBackend() ? 0.90f : 1.0f;

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "gamelist_view",
                            MulAlpha(UIStyle.PrimaryColor, bg_alpha)))
  {
    static constexpr float ITEM_WIDTH = 25.0f;
    static constexpr const char* icons[] = {ICON_FA_BORDER_ALL, ICON_FA_LIST};
    static constexpr const char* titles[] = {FSUI_NSTR("Game Grid"), FSUI_NSTR("Game List")};
    static constexpr u32 count = static_cast<u32>(std::size(titles));

    BeginNavBar();

    if (NavButton(ICON_FA_BACKWARD, true, true))
      ReturnToPreviousWindow();

    NavTitle(Host::TranslateToCString(TR_CONTEXT, titles[static_cast<u32>(s_state.game_list_view)]));
    RightAlignNavButtons(count, ITEM_WIDTH, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    for (u32 i = 0; i < count; i++)
    {
      if (NavButton(icons[i], static_cast<GameListView>(i) == s_state.game_list_view, true, ITEM_WIDTH,
                    LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
      {
        s_state.game_list_view = static_cast<GameListView>(i);
      }
    }

    EndNavBar();
  }

  EndFullscreenWindow();

  if (ImGui::IsKeyPressed(ImGuiKey_NavGamepadMenu, false) || ImGui::IsKeyPressed(ImGuiKey_F1, false))
  {
    s_state.game_list_view = (s_state.game_list_view == GameListView::Grid) ? GameListView::List : GameListView::Grid;
  }
  else if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) || ImGui::IsKeyPressed(ImGuiKey_F2))
  {
    s_state.current_main_window = MainWindowType::GameListSettings;
    QueueResetFocus(FocusResetType::ViewChanged);
  }

  switch (s_state.game_list_view)
  {
    case GameListView::Grid:
      DrawGameGrid(heading_size);
      break;
    case GameListView::List:
      DrawGameList(heading_size);
      break;
    default:
      break;
  }

  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_XBOX_DPAD, FSUI_VSTR("Select Game")),
                                       std::make_pair(ICON_PF_BUTTON_X, FSUI_VSTR("Change View")),
                                       std::make_pair(ICON_PF_BURGER_MENU, FSUI_VSTR("Settings")),
                                       std::make_pair(ICON_PF_BUTTON_Y, FSUI_VSTR("Launch Options")),
                                       std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Start Game")),
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Back"))});
  }
  else
  {
    SetFullscreenFooterText(std::array{
      std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT,
                     FSUI_VSTR("Select Game")),
      std::make_pair(ICON_PF_F1, FSUI_VSTR("Change View")), std::make_pair(ICON_PF_F2, FSUI_VSTR("Settings")),
      std::make_pair(ICON_PF_F3, FSUI_VSTR("Launch Options")), std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Start Game")),
      std::make_pair(ICON_PF_ESC, FSUI_VSTR("Back"))});
  }
}

void FullscreenUI::DrawGameList(const ImVec2& heading_size)
{
  if (!BeginFullscreenColumns(nullptr, heading_size.y, true, true))
  {
    EndFullscreenColumns();
    return;
  }

  if (!AreAnyDialogsOpen() && WantsToCloseMenu())
    ReturnToPreviousWindow();

  auto game_list_lock = GameList::GetLock();
  const GameList::Entry* selected_entry = nullptr;
  PopulateGameListEntryList();

  if (IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  if (BeginFullscreenColumnWindow(0.0f, -530.0f, "game_list_entries"))
  {
    const ImVec2 image_size(LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT, LAYOUT_MENU_BUTTON_HEIGHT));

    ResetFocusHere();

    BeginMenuButtons();

    SmallString summary;

    for (const GameList::Entry* entry : s_state.game_list_sorted_entries)
    {
      ImRect bb;
      bool visible, hovered;
      bool pressed =
        MenuButtonFrame(entry->path.c_str(), true, LAYOUT_MENU_BUTTON_HEIGHT, &visible, &hovered, &bb.Min, &bb.Max);
      if (!visible)
        continue;

      GPUTexture* cover_texture = GetGameListCover(entry);

      if (entry->serial.empty())
        summary.format("{} - ", Settings::GetDiscRegionDisplayName(entry->region));
      else
        summary.format("{} - {} - ", entry->serial, Settings::GetDiscRegionDisplayName(entry->region));

      summary.append(Path::GetFileName(entry->path));

      const ImRect image_rect(
        CenterImage(ImRect(bb.Min, bb.Min + image_size), ImVec2(static_cast<float>(cover_texture->GetWidth()),
                                                                static_cast<float>(cover_texture->GetHeight()))));

      ImGui::GetWindowDrawList()->AddImage(cover_texture, image_rect.Min, image_rect.Max, ImVec2(0.0f, 0.0f),
                                           ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));

      const float midpoint = bb.Min.y + UIStyle.LargeFont->FontSize + LayoutScale(4.0f);
      const float text_start_x = bb.Min.x + image_size.x + LayoutScale(15.0f);
      const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
      const ImRect summary_bb(ImVec2(text_start_x, midpoint), bb.Max);

      ImGui::PushFont(UIStyle.LargeFont);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, entry->title.c_str(),
                               entry->title.c_str() + entry->title.size(), nullptr, ImVec2(0.0f, 0.0f), &title_bb);
      ImGui::PopFont();

      if (!summary.empty())
      {
        ImGui::PushFont(UIStyle.MediumFont);
        ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, summary.c_str(), summary.end_ptr(), nullptr,
                                 ImVec2(0.0f, 0.0f), &summary_bb);
        ImGui::PopFont();
      }

      if (pressed)
      {
        HandleGameListActivate(entry);
      }
      else
      {
        if (hovered)
          selected_entry = entry;

        if (selected_entry &&
            (ImGui::IsItemClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_NavGamepadInput, false) ||
             ImGui::IsKeyPressed(ImGuiKey_F3, false)))
        {
          HandleGameListOptions(selected_entry);
        }
      }
    }

    EndMenuButtons();
  }
  EndFullscreenColumnWindow();

  if (BeginFullscreenColumnWindow(-530.0f, 0.0f, "game_list_info", UIStyle.PrimaryDarkColor))
  {
    const GPUTexture* cover_texture =
      selected_entry ? GetGameListCover(selected_entry) : GetTextureForGameListEntryType(GameList::EntryType::Count);
    if (cover_texture)
    {
      const ImRect image_rect(
        CenterImage(LayoutScale(ImVec2(350.0f, 350.0f)), ImVec2(static_cast<float>(cover_texture->GetWidth()),
                                                                static_cast<float>(cover_texture->GetHeight()))));

      ImGui::SetCursorPos(LayoutScale(ImVec2(90.0f, 0.0f)) + image_rect.Min);
      ImGui::Image(selected_entry ? GetGameListCover(selected_entry) :
                                    GetTextureForGameListEntryType(GameList::EntryType::Count),
                   image_rect.GetSize());
    }

    const float work_width = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    constexpr float field_margin_y = 10.0f;
    constexpr float start_x = 50.0f;
    float text_y = 400.0f;
    float text_width;

    PushPrimaryColor();
    ImGui::SetCursorPos(LayoutScale(start_x, text_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, field_margin_y));
    ImGui::BeginGroup();

    if (selected_entry)
    {
      // title
      ImGui::PushFont(UIStyle.LargeFont);
      text_width = ImGui::CalcTextSize(selected_entry->title.c_str(), nullptr, false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::TextWrapped("%s", selected_entry->title.c_str());
      ImGui::PopFont();

      ImGui::PushFont(UIStyle.MediumFont);

      // developer
      if (selected_entry->dbentry && !selected_entry->dbentry->developer.empty())
      {
        text_width =
          ImGui::CalcTextSize(selected_entry->dbentry->developer.data(),
                              selected_entry->dbentry->developer.data() + selected_entry->dbentry->developer.length(),
                              false, work_width)
            .x;
        ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
        ImGui::TextWrapped("%.*s", static_cast<int>(selected_entry->dbentry->developer.size()),
                           selected_entry->dbentry->developer.data());
      }

      // code
      text_width = ImGui::CalcTextSize(selected_entry->serial.c_str(), nullptr, false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::TextWrapped("%s", selected_entry->serial.c_str());
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 15.0f);

      // region
      {
        const bool display_as_language = (selected_entry->dbentry && selected_entry->dbentry->HasAnyLanguage());
        ImGui::TextUnformatted(display_as_language ? FSUI_CSTR("Language: ") : FSUI_CSTR("Region: "));
        ImGui::SameLine();
        ImGui::Image(GetCachedTexture(selected_entry->GetLanguageIconName(), 23, 16), LayoutScale(23.0f, 16.0f));
        ImGui::SameLine();
        if (display_as_language)
        {
          ImGui::TextWrapped(" (%s, %s)", selected_entry->dbentry->GetLanguagesString().c_str(),
                             Settings::GetDiscRegionName(selected_entry->region));
        }
        else
        {
          ImGui::TextWrapped(" (%s)", Settings::GetDiscRegionName(selected_entry->region));
        }
      }

      // genre
      if (selected_entry->dbentry && !selected_entry->dbentry->genre.empty())
      {
        ImGui::Text(FSUI_CSTR("Genre: %.*s"), static_cast<int>(selected_entry->dbentry->genre.size()),
                    selected_entry->dbentry->genre.data());
      }

      // release date
      ImGui::Text(FSUI_CSTR("Release Date: %s"), selected_entry->GetReleaseDateString().c_str());

      // compatibility
      ImGui::TextUnformatted(FSUI_CSTR("Compatibility: "));
      ImGui::SameLine();
      ImGui::Image(GetCachedTexture(selected_entry->GetCompatibilityIconFileName(), 88, 16), LayoutScale(88.0f, 16.0f));
      ImGui::SameLine();
      ImGui::Text(" (%s)", GameDatabase::GetCompatibilityRatingDisplayName(
                             (selected_entry && selected_entry->dbentry) ? selected_entry->dbentry->compatibility :
                                                                           GameDatabase::CompatibilityRating::Unknown));

      // play time
      ImGui::Text(FSUI_CSTR("Time Played: %s"), GameList::FormatTimespan(selected_entry->total_played_time).c_str());
      ImGui::Text(FSUI_CSTR("Last Played: %s"), GameList::FormatTimestamp(selected_entry->last_played_time).c_str());

      // size
      if (selected_entry->file_size >= 0)
        ImGui::Text(FSUI_CSTR("File Size: %.2f MB"), static_cast<float>(selected_entry->file_size) / 1048576.0f);
      else
        ImGui::TextUnformatted(FSUI_CSTR("Unknown File Size"));
      ImGui::Text(FSUI_CSTR("Uncompressed Size: %.2f MB"),
                  static_cast<float>(selected_entry->uncompressed_size) / 1048576.0f);

      ImGui::PopFont();
    }
    else
    {
      // title
      const char* title = FSUI_CSTR("No Game Selected");
      ImGui::PushFont(UIStyle.LargeFont);
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
  if (IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  ImGuiIO& io = ImGui::GetIO();
  if (!BeginFullscreenWindow(
        ImVec2(0.0f, heading_size.y),
        ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y - LayoutScale(LAYOUT_FOOTER_HEIGHT)), "game_grid",
        UIStyle.BackgroundColor))
  {
    EndFullscreenWindow();
    return;
  }

  if (ImGui::IsWindowFocused() && WantsToCloseMenu())
    ReturnToPreviousWindow();

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
  const float item_height = (style.FramePadding.y * 2.0f) + image_height + title_spacing + UIStyle.MediumFont->FontSize;
  const ImVec2 item_size(item_width, item_height);
  const u32 grid_count_x = static_cast<u32>(std::floor(ImGui::GetWindowWidth() / item_width_with_spacing));
  const float start_x =
    (static_cast<float>(ImGui::GetWindowWidth()) - (item_width_with_spacing * static_cast<float>(grid_count_x))) * 0.5f;

  SmallString draw_title;

  u32 grid_x = 0;
  ImGui::SetCursorPos(ImVec2(start_x, 0.0f));
  for (const GameList::Entry* entry : s_state.game_list_sorted_entries)
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

        ImGuiFullscreen::DrawMenuButtonFrame(bb.Min, bb.Max, col, true, 0.0f);

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
      draw_title.format("{}{}", title, (title.length() == entry->title.length()) ? "" : "...");
      ImGui::PushFont(UIStyle.MediumFont);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, draw_title.c_str(), draw_title.end_ptr(), nullptr,
                               ImVec2(0.5f, 0.0f), &title_bb);
      ImGui::PopFont();

      if (pressed)
      {
        HandleGameListActivate(entry);
      }
      else if (hovered &&
               (ImGui::IsItemClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_NavGamepadInput, false) ||
                ImGui::IsKeyPressed(ImGuiKey_F3, false)))
      {
        HandleGameListOptions(entry);
      }
    }

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
  EndFullscreenWindow();
}

void FullscreenUI::HandleGameListActivate(const GameList::Entry* entry)
{
  if (entry->IsDiscSet())
  {
    HandleSelectDiscForDiscSet(entry->path);
    return;
  }

  // launch game
  if (!OpenLoadStateSelectorForGameResume(entry))
    DoStartPath(entry->path);
}

void FullscreenUI::HandleGameListOptions(const GameList::Entry* entry)
{
  if (!entry->IsDiscSet())
  {
    ImGuiFullscreen::ChoiceDialogOptions options = {
      {FSUI_ICONSTR(ICON_FA_WRENCH, "Game Properties"), false},
      {FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Open Containing Directory"), false},
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
          case 1: // Open Containing Directory
            ExitFullscreenAndOpenURL(Path::CreateFileURL(Path::GetDirectory(entry_path)));
            break;
          case 2: // Resume Game
            DoStartPath(entry_path, System::GetGameSaveStateFileName(entry_serial, -1));
            break;
          case 3: // Load State
            OpenLoadStateSelectorForGame(entry_path);
            break;
          case 4: // Default Boot
            DoStartPath(entry_path);
            break;
          case 5: // Fast Boot
            DoStartPath(entry_path, {}, true);
            break;
          case 6: // Slow Boot
            DoStartPath(entry_path, {}, false);
            break;
          case 7: // Reset Play Time
            GameList::ClearPlayedTimeForSerial(entry_serial);
            break;
          default:
            break;
        }

        CloseChoiceDialog();
      });
  }
  else
  {
    // shouldn't fail
    const GameList::Entry* first_disc_entry = GameList::GetFirstDiscSetMember(entry->path);
    if (!first_disc_entry)
      return;

    ImGuiFullscreen::ChoiceDialogOptions options = {
      {FSUI_ICONSTR(ICON_FA_WRENCH, "Game Properties"), false},
      {FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Select Disc"), false},
      {FSUI_ICONSTR(ICON_FA_WINDOW_CLOSE, "Close Menu"), false},
    };

    OpenChoiceDialog(entry->title.c_str(), false, std::move(options),
                     [entry_path = first_disc_entry->path,
                      disc_set_name = entry->path](s32 index, const std::string& title, bool checked) {
                       switch (index)
                       {
                         case 0: // Open Game Properties
                           SwitchToGameSettingsForPath(entry_path);
                           break;
                         case 1: // Select Disc
                           HandleSelectDiscForDiscSet(disc_set_name);
                           break;
                         default:
                           break;
                       }

                       CloseChoiceDialog();
                     });
  }
}

void FullscreenUI::HandleSelectDiscForDiscSet(std::string_view disc_set_name)
{
  auto lock = GameList::GetLock();
  const std::vector<const GameList::Entry*> entries = GameList::GetDiscSetMembers(disc_set_name, true);
  if (entries.empty())
    return;

  ImGuiFullscreen::ChoiceDialogOptions options;
  std::vector<std::string> paths;
  paths.reserve(entries.size());

  for (const GameList::Entry* entry : entries)
  {
    std::string title = fmt::format(fmt::runtime(FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Disc {} | {}")),
                                    entry->disc_set_index + 1, Path::GetFileName(entry->path));
    options.emplace_back(std::move(title), false);
    paths.push_back(entry->path);
  }
  options.emplace_back(FSUI_ICONSTR(ICON_FA_WINDOW_CLOSE, "Close Menu"), false);

  OpenChoiceDialog(SmallString::from_format("Select Disc for {}", disc_set_name), false, std::move(options),
                   [paths = std::move(paths)](s32 index, const std::string& title, bool checked) {
                     if (static_cast<u32>(index) < paths.size())
                     {
                       auto lock = GameList::GetLock();
                       const GameList::Entry* entry = GameList::GetEntryForPath(paths[index]);
                       if (entry)
                         HandleGameListActivate(entry);
                     }

                     CloseChoiceDialog();
                   });
}

void FullscreenUI::DrawGameListSettingsWindow()
{
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 heading_size =
    ImVec2(io.DisplaySize.x, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) +
                               (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));

  const float bg_alpha = GPUThread::HasGPUBackend() ? 0.90f : 1.0f;

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "gamelist_view",
                            MulAlpha(UIStyle.PrimaryColor, bg_alpha)))
  {
    BeginNavBar();

    if (NavButton(ICON_FA_BACKWARD, true, true))
    {
      s_state.current_main_window = MainWindowType::GameList;
      QueueResetFocus(FocusResetType::Other);
    }

    NavTitle(FSUI_CSTR("Game List Settings"));
    EndNavBar();
  }

  EndFullscreenWindow();

  if (!BeginFullscreenWindow(
        ImVec2(0.0f, heading_size.y),
        ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y - LayoutScale(LAYOUT_FOOTER_HEIGHT)),
        "settings_parent", UIStyle.BackgroundColor, 0.0f, ImVec2(ImGuiFullscreen::LAYOUT_MENU_WINDOW_X_PADDING, 0.0f)))
  {
    EndFullscreenWindow();
    return;
  }

  if (ImGui::IsWindowFocused() && WantsToCloseMenu())
  {
    s_state.current_main_window = MainWindowType::GameList;
    QueueResetFocus(FocusResetType::ViewChanged);
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

  for (const auto& it : s_state.game_list_directories_cache)
  {
    if (MenuButton(SmallString::from_format(ICON_FA_FOLDER " {}", it.first),
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
                           // Open in file browser
                           ExitFullscreenAndOpenURL(Path::CreateFileURL(dir));
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

  MenuHeading(FSUI_CSTR("List Settings"));
  {
    static constexpr const char* view_types[] = {FSUI_NSTR("Game Grid"), FSUI_NSTR("Game List")};
    static constexpr const char* sort_types[] = {
      FSUI_NSTR("Type"),        FSUI_NSTR("Serial"),      FSUI_NSTR("Title"),     FSUI_NSTR("File Title"),
      FSUI_NSTR("Time Played"), FSUI_NSTR("Last Played"), FSUI_NSTR("File Size"), FSUI_NSTR("Uncompressed Size")};

    DrawIntListSetting(bsi, FSUI_ICONSTR(ICON_FA_BORDER_ALL, "Default View"),
                       FSUI_CSTR("Selects the view that the game list will open to."), "Main",
                       "DefaultFullscreenUIGameView", 0, view_types, std::size(view_types), true);
    DrawIntListSetting(bsi, FSUI_ICONSTR(ICON_FA_SORT, "Sort By"),
                       FSUI_CSTR("Determines that field that the game list will be sorted by."), "Main",
                       "FullscreenUIGameSort", 0, sort_types, std::size(sort_types), true);
    DrawToggleSetting(
      bsi, FSUI_ICONSTR(ICON_FA_SORT_ALPHA_DOWN, "Sort Reversed"),
      FSUI_CSTR("Reverses the game list sort order from the default (usually ascending to descending)."), "Main",
      "FullscreenUIGameSortReverse", false);
    DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_LIST, "Merge Multi-Disc Games"),
                      FSUI_CSTR("Merges multi-disc games into one item in the game list."), "Main",
                      "FullscreenUIMergeDiscSets", true);
  }

  MenuHeading(FSUI_CSTR("Cover Settings"));
  {
    DrawFolderSetting(bsi, FSUI_ICONSTR(ICON_FA_FOLDER, "Covers Directory"), "Folders", "Covers", EmuFolders::Covers);
    if (MenuButton(FSUI_ICONSTR(ICON_FA_DOWNLOAD, "Download Covers"),
                   FSUI_CSTR("Downloads covers from a user-specified URL template.")))
    {
      Host::OnCoverDownloaderOpenRequested();
    }
  }

  MenuHeading(FSUI_CSTR("Operations"));
  {
    if (MenuButton(FSUI_ICONSTR(ICON_FA_SEARCH, "Scan For New Games"),
                   FSUI_CSTR("Identifies any new files added to the game directories.")))
    {
      Host::RefreshGameListAsync(false);
    }
    if (MenuButton(FSUI_ICONSTR(ICON_FA_SEARCH_PLUS, "Rescan All Games"),
                   FSUI_CSTR("Forces a full rescan of all games previously identified.")))
    {
      Host::RefreshGameListAsync(true);
    }
  }

  EndMenuButtons();

  EndFullscreenWindow();

  SetStandardSelectionFooterText(true);
}

void FullscreenUI::SwitchToGameList()
{
  s_state.current_main_window = MainWindowType::GameList;
  s_state.game_list_view =
    static_cast<GameListView>(Host::GetBaseIntSettingValue("Main", "DefaultFullscreenUIGameView", 0));
  {
    auto lock = Host::GetSettingsLock();
    PopulateGameListDirectoryCache(Host::Internal::GetBaseSettingsLayer());
  }
  QueueResetFocus(FocusResetType::ViewChanged);
}

GPUTexture* FullscreenUI::GetGameListCover(const GameList::Entry* entry)
{
  // lookup and grab cover image
  auto cover_it = s_state.cover_image_map.find(entry->path);
  if (cover_it == s_state.cover_image_map.end())
  {
    std::string cover_path(GameList::GetCoverImagePathForEntry(entry));
    cover_it = s_state.cover_image_map.emplace(entry->path, std::move(cover_path)).first;
  }

  GPUTexture* tex = (!cover_it->second.empty()) ? GetCachedTextureAsync(cover_it->second.c_str()) : nullptr;
  return tex ? tex : GetTextureForGameListEntryType(entry->type);
}

GPUTexture* FullscreenUI::GetTextureForGameListEntryType(GameList::EntryType type)
{
  switch (type)
  {
    case GameList::EntryType::PSExe:
      return s_state.fallback_exe_texture.get();

    case GameList::EntryType::Playlist:
      return s_state.fallback_playlist_texture.get();

    case GameList::EntryType::PSF:
      return s_state.fallback_psf_texture.get();

    case GameList::EntryType::Disc:
    default:
      return s_state.fallback_disc_texture.get();
  }
}

GPUTexture* FullscreenUI::GetCoverForCurrentGame()
{
  auto lock = GameList::GetLock();

  const GameList::Entry* entry = GameList::GetEntryForPath(System::GetDiscPath());
  if (!entry)
    return s_state.fallback_disc_texture.get();

  return GetGameListCover(entry);
}

//////////////////////////////////////////////////////////////////////////
// Overlays
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::OpenAboutWindow()
{
  s_state.about_window_open = true;
}

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
  ImGui::SetNextWindowSize(LayoutScale(1000.0f, 540.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup(FSUI_CSTR("About DuckStation"));

  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(30.0f, 30.0f));

  if (ImGui::BeginPopupModal(FSUI_CSTR("About DuckStation"), &s_state.about_window_open,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    ImGui::TextWrapped("%s", FSUI_CSTR("DuckStation is a free simulator/emulator of the Sony PlayStation(TM) "
                                       "console, focusing on playability, speed, and long-term maintainability."));
    ImGui::NewLine();
    ImGui::TextWrapped(FSUI_CSTR("Version: %s"), g_scm_tag_str);
    ImGui::NewLine();
    ImGui::TextWrapped(
      "%s", FSUI_CSTR("Duck icon by icons8 (https://icons8.com/icon/74847/platforms.undefined.short-title)"));
    ImGui::NewLine();
    ImGui::TextWrapped(
      "%s", FSUI_CSTR("\"PlayStation\" and \"PSX\" are registered trademarks of Sony Interactive Entertainment Europe "
                      "Limited. This software is not affiliated in any way with Sony Interactive Entertainment."));

    ImGui::NewLine();

    BeginMenuButtons();
    if (ActiveButton(FSUI_ICONSTR(ICON_FA_GLOBE, "GitHub Repository"), false))
      ExitFullscreenAndOpenURL("https://github.com/stenzek/duckstation/");
    if (ActiveButton(FSUI_ICONSTR(ICON_FA_COMMENT, "Discord Server"), false))
      ExitFullscreenAndOpenURL("https://www.duckstation.org/discord.html");
    if (ActiveButton(FSUI_ICONSTR(ICON_FA_PEOPLE_CARRY, "Contributor List"), false))
      ExitFullscreenAndOpenURL("https://github.com/stenzek/duckstation/blob/master/CONTRIBUTORS.md");

    if (ActiveButton(FSUI_ICONSTR(ICON_FA_WINDOW_CLOSE, "Close"), false) || WantsToCloseMenu())
    {
      ImGui::CloseCurrentPopup();
      s_state.about_window_open = false;
    }
    else
    {
      SetStandardSelectionFooterText(true);
    }

    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopFont();
}

void FullscreenUI::OpenAchievementsWindow()
{
  if (!System::IsValid())
    return;

  if (!Achievements::IsActive())
  {
    Host::AddKeyedOSDMessage("achievements_disabled", FSUI_STR("Achievements are not enabled."),
                             Host::OSD_INFO_DURATION);
    return;
  }
  else if (!Achievements::HasAchievements())
  {
    ShowToast(std::string(), FSUI_STR("This game has no achievements."));
    return;
  }

  GPUThread::RunOnThread([]() {
    if (!Initialize() || !Achievements::PrepareAchievementsWindow())
      return;

    if (s_state.current_main_window != MainWindowType::PauseMenu)
    {
      PauseForMenuOpen(false);
      ForceKeyNavEnabled();
    }

    s_state.current_main_window = MainWindowType::Achievements;
    QueueResetFocus(FocusResetType::ViewChanged);
    UpdateRunIdleState();
    FixStateIfPaused();
  });
}

bool FullscreenUI::IsAchievementsWindowOpen()
{
  return (s_state.current_main_window == MainWindowType::Achievements);
}

void FullscreenUI::OpenLeaderboardsWindow()
{
  if (!System::IsValid())
    return;

  if (!Achievements::IsActive())
  {
    Host::AddKeyedOSDMessage("achievements_disabled", FSUI_STR("Leaderboards are not enabled."),
                             Host::OSD_INFO_DURATION);
    return;
  }
  else if (!Achievements::HasLeaderboards())
  {
    ShowToast(std::string(), FSUI_STR("This game has no leaderboards."));
    return;
  }

  GPUThread::RunOnThread([]() {
    if (!Initialize() || !Achievements::PrepareLeaderboardsWindow())
      return;

    if (s_state.current_main_window != MainWindowType::PauseMenu)
    {
      PauseForMenuOpen(false);
      ForceKeyNavEnabled();
    }

    s_state.current_main_window = MainWindowType::Leaderboards;
    QueueResetFocus(FocusResetType::ViewChanged);
    UpdateRunIdleState();
    FixStateIfPaused();
  });
}

bool FullscreenUI::IsLeaderboardsWindowOpen()
{
  return (s_state.current_main_window == MainWindowType::Leaderboards);
}

#endif // __ANDROID__

LoadingScreenProgressCallback::LoadingScreenProgressCallback()
  : ProgressCallback(), m_open_time(Timer::GetCurrentValue()), m_on_gpu_thread(GPUThread::IsOnThread())
{
}

LoadingScreenProgressCallback::~LoadingScreenProgressCallback()
{
  // Did we activate?
  if (m_last_progress_percent < 0)
    return;

  if (!m_on_gpu_thread)
  {
    GPUThread::RunOnThread([]() {
      ImGuiFullscreen::CloseLoadingScreen();
      Assert(GPUThread::GetRunIdleReason(GPUThread::RunIdleReason::LoadingScreenActive));
      GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::LoadingScreenActive, false);
    });
  }
  else
  {
    // since this was pushing frames, we need to restore the context
    GPUThread::Internal::RestoreContextAfterPresent();
  }
}

void LoadingScreenProgressCallback::PushState()
{
  ProgressCallback::PushState();
}

void LoadingScreenProgressCallback::PopState()
{
  ProgressCallback::PopState();
  Redraw(true);
}

void LoadingScreenProgressCallback::SetCancellable(bool cancellable)
{
  ProgressCallback::SetCancellable(cancellable);
  Redraw(true);
}

void LoadingScreenProgressCallback::SetTitle(const std::string_view title)
{
  // todo?
}

void LoadingScreenProgressCallback::SetStatusText(const std::string_view text)
{
  ProgressCallback::SetStatusText(text);
  Redraw(true);
}

void LoadingScreenProgressCallback::SetProgressRange(u32 range)
{
  u32 last_range = m_progress_range;

  ProgressCallback::SetProgressRange(range);

  if (m_progress_range != last_range)
    Redraw(false);
}

void LoadingScreenProgressCallback::SetProgressValue(u32 value)
{
  u32 lastValue = m_progress_value;

  ProgressCallback::SetProgressValue(value);

  if (m_progress_value != lastValue)
    Redraw(false);
}

void LoadingScreenProgressCallback::Redraw(bool force)
{
  if (m_last_progress_percent < 0 &&
      Timer::ConvertValueToSeconds(Timer::GetCurrentValue() - m_open_time) < m_open_delay)
  {
    return;
  }

  const int percent =
    static_cast<int>((static_cast<float>(m_progress_value) / static_cast<float>(m_progress_range)) * 100.0f);
  DebugAssert(percent >= 0);
  if (percent == m_last_progress_percent && !force)
    return;

  // activation?
  if (m_last_progress_percent < 0 && !m_on_gpu_thread)
  {
    GPUThread::RunOnThread([]() {
      Assert(!GPUThread::GetRunIdleReason(GPUThread::RunIdleReason::LoadingScreenActive));
      GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::LoadingScreenActive, true);
    });
  }

  m_last_progress_percent = percent;
  if (m_on_gpu_thread)
  {
    ImGuiFullscreen::RenderLoadingScreen(ImGuiManager::LOGO_IMAGE_NAME, m_status_text, 0,
                                         static_cast<s32>(m_progress_range), static_cast<s32>(m_progress_value));
  }
  else
  {
    GPUThread::RunOnThread([status_text = SmallString(std::string_view(m_status_text)),
                            range = static_cast<s32>(m_progress_range), value = static_cast<s32>(m_progress_value)]() {
      ImGuiFullscreen::OpenOrUpdateLoadingScreen(ImGuiManager::LOGO_IMAGE_NAME, status_text, 0, range, value);
    });
  }
}

void LoadingScreenProgressCallback::ModalError(const std::string_view message)
{
  ERROR_LOG(message);
  Host::ReportErrorAsync("Error", message);
}

bool LoadingScreenProgressCallback::ModalConfirmation(const std::string_view message)
{
  INFO_LOG(message);
  return Host::ConfirmMessage("Confirm", message);
}

void FullscreenUI::OpenLoadingScreen(std::string_view image, std::string_view message, s32 progress_min /*= -1*/,
                                     s32 progress_max /*= -1*/, s32 progress_value /*= -1*/)
{
  Assert(GPUThread::IsOnThread());
  Assert(!GPUThread::GetRunIdleReason(GPUThread::RunIdleReason::LoadingScreenActive));
  GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::LoadingScreenActive, true);
  ImGuiFullscreen::OpenOrUpdateLoadingScreen(image, message, progress_min, progress_max, progress_value);
}

void FullscreenUI::UpdateLoadingScreen(std::string_view image, std::string_view message, s32 progress_min /*= -1*/,
                                       s32 progress_max /*= -1*/, s32 progress_value /*= -1*/)
{
  Assert(GPUThread::IsOnThread());
  Assert(GPUThread::GetRunIdleReason(GPUThread::RunIdleReason::LoadingScreenActive));
  ImGuiFullscreen::OpenOrUpdateLoadingScreen(image, message, progress_min, progress_max, progress_value);
}

void FullscreenUI::CloseLoadingScreen()
{
  Assert(GPUThread::IsOnThread());
  Assert(GPUThread::GetRunIdleReason(GPUThread::RunIdleReason::LoadingScreenActive));
  ImGuiFullscreen::CloseLoadingScreen();
  GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::LoadingScreenActive, false);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Translation String Area
// To avoid having to type T_RANSLATE("FullscreenUI", ...) everywhere, we use the shorter macros at the top
// of the file, then preprocess and generate a bunch of noops here to define the strings. Sadly that means
// the view in Linguist is gonna suck, but you can search the file for the string for more context.
/////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0
// TRANSLATION-STRING-AREA-BEGIN
TRANSLATE_NOOP("FullscreenUI", "%.1f ms");
TRANSLATE_NOOP("FullscreenUI", "%.2f Seconds");
TRANSLATE_NOOP("FullscreenUI", "%d Frames");
TRANSLATE_NOOP("FullscreenUI", "%d ms");
TRANSLATE_NOOP("FullscreenUI", "%d sectors");
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
TRANSLATE_NOOP("FullscreenUI", "A resume save state created at %s was found.\n\nDo you want to load this save and continue?");
TRANSLATE_NOOP("FullscreenUI", "About");
TRANSLATE_NOOP("FullscreenUI", "About DuckStation");
TRANSLATE_NOOP("FullscreenUI", "Account");
TRANSLATE_NOOP("FullscreenUI", "Accurate Blending");
TRANSLATE_NOOP("FullscreenUI", "Achievement Notifications");
TRANSLATE_NOOP("FullscreenUI", "Achievements");
TRANSLATE_NOOP("FullscreenUI", "Achievements Settings");
TRANSLATE_NOOP("FullscreenUI", "Achievements are not enabled.");
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
TRANSLATE_NOOP("FullscreenUI", "Allows loading protected games without subchannel information.");
TRANSLATE_NOOP("FullscreenUI", "An error occurred while deleting empty game settings:\n{}");
TRANSLATE_NOOP("FullscreenUI", "An error occurred while saving game settings:\n{}");
TRANSLATE_NOOP("FullscreenUI", "Apply Image Patches");
TRANSLATE_NOOP("FullscreenUI", "Are you sure you want to clear the current post-processing chain? All configuration will be lost.");
TRANSLATE_NOOP("FullscreenUI", "Aspect Ratio");
TRANSLATE_NOOP("FullscreenUI", "Attempts to detect one pixel high/wide lines that rely on non-upscaled rasterization behavior, filling in gaps introduced by upscaling.");
TRANSLATE_NOOP("FullscreenUI", "Attempts to map the selected port to a chosen controller.");
TRANSLATE_NOOP("FullscreenUI", "Audio Backend");
TRANSLATE_NOOP("FullscreenUI", "Audio Control");
TRANSLATE_NOOP("FullscreenUI", "Audio Settings");
TRANSLATE_NOOP("FullscreenUI", "Auto-Detect");
TRANSLATE_NOOP("FullscreenUI", "Automatic Mapping");
TRANSLATE_NOOP("FullscreenUI", "Automatic based on window size");
TRANSLATE_NOOP("FullscreenUI", "Automatic mapping completed for {}.");
TRANSLATE_NOOP("FullscreenUI", "Automatic mapping failed for {}.");
TRANSLATE_NOOP("FullscreenUI", "Automatic mapping failed, no devices are available.");
TRANSLATE_NOOP("FullscreenUI", "Automatically Resize Window");
TRANSLATE_NOOP("FullscreenUI", "Automatically applies patches to disc images when they are present, currently only PPF is supported.");
TRANSLATE_NOOP("FullscreenUI", "Automatically loads and applies cheats on game start. Cheats can break games and saves.");
TRANSLATE_NOOP("FullscreenUI", "Automatically resizes the window to match the internal resolution.");
TRANSLATE_NOOP("FullscreenUI", "Automatically saves the emulator state when powering down or exiting. You can then resume directly from where you left off next time.");
TRANSLATE_NOOP("FullscreenUI", "Automatically switches to fullscreen mode when the program is started.");
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
TRANSLATE_NOOP("FullscreenUI", "Borderless Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Buffer Size");
TRANSLATE_NOOP("FullscreenUI", "Buttons");
TRANSLATE_NOOP("FullscreenUI", "CD-ROM Emulation");
TRANSLATE_NOOP("FullscreenUI", "CPU Emulation");
TRANSLATE_NOOP("FullscreenUI", "CPU Mode");
TRANSLATE_NOOP("FullscreenUI", "Cancel");
TRANSLATE_NOOP("FullscreenUI", "Capture");
TRANSLATE_NOOP("FullscreenUI", "Change Disc");
TRANSLATE_NOOP("FullscreenUI", "Change Page");
TRANSLATE_NOOP("FullscreenUI", "Change Selection");
TRANSLATE_NOOP("FullscreenUI", "Change View");
TRANSLATE_NOOP("FullscreenUI", "Changes settings for the application.");
TRANSLATE_NOOP("FullscreenUI", "Changes the aspect ratio used to display the console's output to the screen.");
TRANSLATE_NOOP("FullscreenUI", "Cheats");
TRANSLATE_NOOP("FullscreenUI", "Chooses the backend to use for rendering the console/game visuals.");
TRANSLATE_NOOP("FullscreenUI", "Chooses the language used for UI elements.");
TRANSLATE_NOOP("FullscreenUI", "Clean Boot");
TRANSLATE_NOOP("FullscreenUI", "Clear Settings");
TRANSLATE_NOOP("FullscreenUI", "Clear Shaders");
TRANSLATE_NOOP("FullscreenUI", "Clears a shader from the chain.");
TRANSLATE_NOOP("FullscreenUI", "Clears all settings set for this game.");
TRANSLATE_NOOP("FullscreenUI", "Close");
TRANSLATE_NOOP("FullscreenUI", "Close Game");
TRANSLATE_NOOP("FullscreenUI", "Close Menu");
TRANSLATE_NOOP("FullscreenUI", "Compatibility Rating");
TRANSLATE_NOOP("FullscreenUI", "Compatibility: ");
TRANSLATE_NOOP("FullscreenUI", "Completely exits the application, returning you to your desktop.");
TRANSLATE_NOOP("FullscreenUI", "Configuration");
TRANSLATE_NOOP("FullscreenUI", "Confirm Power Off");
TRANSLATE_NOOP("FullscreenUI", "Console Settings");
TRANSLATE_NOOP("FullscreenUI", "Contributor List");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {}");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {} Macros");
TRANSLATE_NOOP("FullscreenUI", "Controller Port {} Settings");
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
TRANSLATE_NOOP("FullscreenUI", "Could not find any CD/DVD-ROM devices. Please ensure you have a drive connected and sufficient permissions to access it.");
TRANSLATE_NOOP("FullscreenUI", "Cover Settings");
TRANSLATE_NOOP("FullscreenUI", "Covers Directory");
TRANSLATE_NOOP("FullscreenUI", "Create");
TRANSLATE_NOOP("FullscreenUI", "Create New...");
TRANSLATE_NOOP("FullscreenUI", "Create Save State Backups");
TRANSLATE_NOOP("FullscreenUI", "Crop Mode");
TRANSLATE_NOOP("FullscreenUI", "Culling Correction");
TRANSLATE_NOOP("FullscreenUI", "Current Game");
TRANSLATE_NOOP("FullscreenUI", "Deadzone");
TRANSLATE_NOOP("FullscreenUI", "Debugging Settings");
TRANSLATE_NOOP("FullscreenUI", "Default");
TRANSLATE_NOOP("FullscreenUI", "Default Boot");
TRANSLATE_NOOP("FullscreenUI", "Default View");
TRANSLATE_NOOP("FullscreenUI", "Default: Disabled");
TRANSLATE_NOOP("FullscreenUI", "Default: Enabled");
TRANSLATE_NOOP("FullscreenUI", "Deinterlacing Mode");
TRANSLATE_NOOP("FullscreenUI", "Delete Save");
TRANSLATE_NOOP("FullscreenUI", "Delete State");
TRANSLATE_NOOP("FullscreenUI", "Depth Clear Threshold");
TRANSLATE_NOOP("FullscreenUI", "Desktop Mode");
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
TRANSLATE_NOOP("FullscreenUI", "Determines the emulated hardware type.");
TRANSLATE_NOOP("FullscreenUI", "Determines the format that screenshots will be saved/compressed with.");
TRANSLATE_NOOP("FullscreenUI", "Determines the frequency at which the macro will toggle the buttons on and off (aka auto fire).");
TRANSLATE_NOOP("FullscreenUI", "Determines the margin between the edge of the screen and on-screen messages.");
TRANSLATE_NOOP("FullscreenUI", "Determines the position on the screen when black borders must be added.");
TRANSLATE_NOOP("FullscreenUI", "Determines the rotation of the simulated TV screen.");
TRANSLATE_NOOP("FullscreenUI", "Determines the size of screenshots created by DuckStation.");
TRANSLATE_NOOP("FullscreenUI", "Determines whether a prompt will be displayed to confirm shutting down the emulator/game when the hotkey is pressed.");
TRANSLATE_NOOP("FullscreenUI", "Determines which algorithm is used to convert interlaced frames to progressive for display on your system.");
TRANSLATE_NOOP("FullscreenUI", "Device Settings");
TRANSLATE_NOOP("FullscreenUI", "Disable Mailbox Presentation");
TRANSLATE_NOOP("FullscreenUI", "Disable Subdirectory Scanning");
TRANSLATE_NOOP("FullscreenUI", "Disable on 2D Polygons");
TRANSLATE_NOOP("FullscreenUI", "Disabled");
TRANSLATE_NOOP("FullscreenUI", "Disables dithering and uses the full 8 bits per channel of color information.");
TRANSLATE_NOOP("FullscreenUI", "Disc {} | {}");
TRANSLATE_NOOP("FullscreenUI", "Discord Server");
TRANSLATE_NOOP("FullscreenUI", "Display Settings");
TRANSLATE_NOOP("FullscreenUI", "Displays popup messages on events such as achievement unlocks and leaderboard submissions.");
TRANSLATE_NOOP("FullscreenUI", "Displays popup messages when starting, submitting, or failing a leaderboard challenge.");
TRANSLATE_NOOP("FullscreenUI", "Double-Click Toggles Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Download Covers");
TRANSLATE_NOOP("FullscreenUI", "Downloads covers from a user-specified URL template.");
TRANSLATE_NOOP("FullscreenUI", "Downsamples the rendered image prior to displaying it. Can improve overall image quality in mixed 2D/3D games.");
TRANSLATE_NOOP("FullscreenUI", "Downsampling");
TRANSLATE_NOOP("FullscreenUI", "Downsampling Display Scale");
TRANSLATE_NOOP("FullscreenUI", "Duck icon by icons8 (https://icons8.com/icon/74847/platforms.undefined.short-title)");
TRANSLATE_NOOP("FullscreenUI", "DuckStation is a free simulator/emulator of the Sony PlayStation(TM) console, focusing on playability, speed, and long-term maintainability.");
TRANSLATE_NOOP("FullscreenUI", "Dump Replaced Textures");
TRANSLATE_NOOP("FullscreenUI", "Dumps textures that have replacements already loaded.");
TRANSLATE_NOOP("FullscreenUI", "Emulation Settings");
TRANSLATE_NOOP("FullscreenUI", "Emulation Speed");
TRANSLATE_NOOP("FullscreenUI", "Enable 8MB RAM");
TRANSLATE_NOOP("FullscreenUI", "Enable Achievements");
TRANSLATE_NOOP("FullscreenUI", "Enable Cheats");
TRANSLATE_NOOP("FullscreenUI", "Enable Discord Presence");
TRANSLATE_NOOP("FullscreenUI", "Enable Fast Boot");
TRANSLATE_NOOP("FullscreenUI", "Enable In-Game Overlays");
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
TRANSLATE_NOOP("FullscreenUI", "Enable VRAM Write Texture Replacement");
TRANSLATE_NOOP("FullscreenUI", "Enable XInput Input Source");
TRANSLATE_NOOP("FullscreenUI", "Enable debugging when supported by the host's renderer API. Only for developer use.");
TRANSLATE_NOOP("FullscreenUI", "Enable/Disable the Player LED on DualSense controllers.");
TRANSLATE_NOOP("FullscreenUI", "Enables alignment and bus exceptions. Not needed for any known games.");
TRANSLATE_NOOP("FullscreenUI", "Enables an additional 6MB of RAM to obtain a total of 2+6 = 8MB, usually present on dev consoles.");
TRANSLATE_NOOP("FullscreenUI", "Enables an additional three controller slots on each port. Not supported in all games.");
TRANSLATE_NOOP("FullscreenUI", "Enables caching of guest textures, required for texture replacement.");
TRANSLATE_NOOP("FullscreenUI", "Enables dumping of textures to image files, which can be replaced. Not compatible with all games.");
TRANSLATE_NOOP("FullscreenUI", "Enables loading of cheats for this game from DuckStation's database.");
TRANSLATE_NOOP("FullscreenUI", "Enables loading of replacement textures. Not compatible with all games.");
TRANSLATE_NOOP("FullscreenUI", "Enables smooth scrolling of menus in Big Picture UI.");
TRANSLATE_NOOP("FullscreenUI", "Enables the cheats that are selected below.");
TRANSLATE_NOOP("FullscreenUI", "Enables the older, less accurate MDEC decoding routines. May be required for old replacement backgrounds to match/load.");
TRANSLATE_NOOP("FullscreenUI", "Enables the replacement of background textures in supported games.");
TRANSLATE_NOOP("FullscreenUI", "Encore Mode");
TRANSLATE_NOOP("FullscreenUI", "Ensures every frame generated is displayed for optimal pacing. Enable for variable refresh displays, such as GSync/FreeSync. Disable if you are having speed or sound issues.");
TRANSLATE_NOOP("FullscreenUI", "Enter Value");
TRANSLATE_NOOP("FullscreenUI", "Enter the name of the input profile you wish to create.");
TRANSLATE_NOOP("FullscreenUI", "Error");
TRANSLATE_NOOP("FullscreenUI", "Execution Mode");
TRANSLATE_NOOP("FullscreenUI", "Exit");
TRANSLATE_NOOP("FullscreenUI", "Exit And Save State");
TRANSLATE_NOOP("FullscreenUI", "Exit DuckStation");
TRANSLATE_NOOP("FullscreenUI", "Exit Without Saving");
TRANSLATE_NOOP("FullscreenUI", "Exits Big Picture mode, returning to the desktop interface.");
TRANSLATE_NOOP("FullscreenUI", "FMV Chroma Smoothing");
TRANSLATE_NOOP("FullscreenUI", "Failed to copy text to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Failed to delete save state.");
TRANSLATE_NOOP("FullscreenUI", "Failed to delete {}.");
TRANSLATE_NOOP("FullscreenUI", "Failed to load '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Failed to load shader {}. It may be invalid.\nError was:");
TRANSLATE_NOOP("FullscreenUI", "Failed to save input profile '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Fast Boot");
TRANSLATE_NOOP("FullscreenUI", "Fast Forward Boot");
TRANSLATE_NOOP("FullscreenUI", "Fast Forward Speed");
TRANSLATE_NOOP("FullscreenUI", "Fast Forward Volume");
TRANSLATE_NOOP("FullscreenUI", "Fast forwards through the early loading process when fast booting, saving time. Results may vary between games.");
TRANSLATE_NOOP("FullscreenUI", "File Size");
TRANSLATE_NOOP("FullscreenUI", "File Size: %.2f MB");
TRANSLATE_NOOP("FullscreenUI", "File Title");
TRANSLATE_NOOP("FullscreenUI", "Force 4:3 For FMVs");
TRANSLATE_NOOP("FullscreenUI", "Force Video Timing");
TRANSLATE_NOOP("FullscreenUI", "Forces a full rescan of all games previously identified.");
TRANSLATE_NOOP("FullscreenUI", "Forces blending to be done in the shader at 16-bit precision, when not using true color. Non-trivial performance impact, and unnecessary for most games.");
TRANSLATE_NOOP("FullscreenUI", "Forces the use of FIFO over Mailbox presentation, i.e. double buffering instead of triple buffering. Usually results in worse frame pacing.");
TRANSLATE_NOOP("FullscreenUI", "Forcibly mutes both CD-DA and XA audio from the CD-ROM. Can be used to disable background music in some games.");
TRANSLATE_NOOP("FullscreenUI", "Frame Time Buffer");
TRANSLATE_NOOP("FullscreenUI", "Frequency");
TRANSLATE_NOOP("FullscreenUI", "From File...");
TRANSLATE_NOOP("FullscreenUI", "Fullscreen Resolution");
TRANSLATE_NOOP("FullscreenUI", "GPU Adapter");
TRANSLATE_NOOP("FullscreenUI", "GPU Renderer");
TRANSLATE_NOOP("FullscreenUI", "GPU adapter will be applied after restarting.");
TRANSLATE_NOOP("FullscreenUI", "Game Grid");
TRANSLATE_NOOP("FullscreenUI", "Game List");
TRANSLATE_NOOP("FullscreenUI", "Game List Settings");
TRANSLATE_NOOP("FullscreenUI", "Game Patches");
TRANSLATE_NOOP("FullscreenUI", "Game Properties");
TRANSLATE_NOOP("FullscreenUI", "Game Quick Save");
TRANSLATE_NOOP("FullscreenUI", "Game Slot {0}##game_slot_{0}");
TRANSLATE_NOOP("FullscreenUI", "Game compatibility rating copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game not loaded or no RetroAchievements available.");
TRANSLATE_NOOP("FullscreenUI", "Game path copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game region copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game serial copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game settings have been cleared for '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Game settings initialized with global settings for '{}'.");
TRANSLATE_NOOP("FullscreenUI", "Game title copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game type copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Game: {} ({})");
TRANSLATE_NOOP("FullscreenUI", "Genre: %.*s");
TRANSLATE_NOOP("FullscreenUI", "Geometry Tolerance");
TRANSLATE_NOOP("FullscreenUI", "GitHub Repository");
TRANSLATE_NOOP("FullscreenUI", "Global Slot {0} - {1}##global_slot_{0}");
TRANSLATE_NOOP("FullscreenUI", "Global Slot {0}##global_slot_{0}");
TRANSLATE_NOOP("FullscreenUI", "Graphics Settings");
TRANSLATE_NOOP("FullscreenUI", "Hardcore Mode");
TRANSLATE_NOOP("FullscreenUI", "Hardcore mode will be enabled on next game restart.");
TRANSLATE_NOOP("FullscreenUI", "Hide Cursor In Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Hides the mouse pointer/cursor when the emulator is in fullscreen mode.");
TRANSLATE_NOOP("FullscreenUI", "Hotkey Settings");
TRANSLATE_NOOP("FullscreenUI", "How many saves will be kept for rewinding. Higher values have greater memory requirements.");
TRANSLATE_NOOP("FullscreenUI", "How often a rewind state will be created. Higher frequencies have greater system requirements.");
TRANSLATE_NOOP("FullscreenUI", "Identifies any new files added to the game directories.");
TRANSLATE_NOOP("FullscreenUI", "If not enabled, the current post processing chain will be ignored.");
TRANSLATE_NOOP("FullscreenUI", "Increases the field of view from 4:3 to the chosen display aspect ratio in 3D games.");
TRANSLATE_NOOP("FullscreenUI", "Increases the precision of polygon culling, reducing the number of holes in geometry.");
TRANSLATE_NOOP("FullscreenUI", "Infinite/Instantaneous");
TRANSLATE_NOOP("FullscreenUI", "Inhibit Screensaver");
TRANSLATE_NOOP("FullscreenUI", "Input Sources");
TRANSLATE_NOOP("FullscreenUI", "Input profile '{}' loaded.");
TRANSLATE_NOOP("FullscreenUI", "Input profile '{}' saved.");
TRANSLATE_NOOP("FullscreenUI", "Integration");
TRANSLATE_NOOP("FullscreenUI", "Interface Settings");
TRANSLATE_NOOP("FullscreenUI", "Internal Resolution");
TRANSLATE_NOOP("FullscreenUI", "Language: ");
TRANSLATE_NOOP("FullscreenUI", "Last Played");
TRANSLATE_NOOP("FullscreenUI", "Last Played: %s");
TRANSLATE_NOOP("FullscreenUI", "Latency Control");
TRANSLATE_NOOP("FullscreenUI", "Launch Options");
TRANSLATE_NOOP("FullscreenUI", "Launch a game by selecting a file/disc image.");
TRANSLATE_NOOP("FullscreenUI", "Launch a game from a file, disc, or starts the console without any disc inserted.");
TRANSLATE_NOOP("FullscreenUI", "Launch a game from images scanned from your game directories.");
TRANSLATE_NOOP("FullscreenUI", "Leaderboard Notifications");
TRANSLATE_NOOP("FullscreenUI", "Leaderboards");
TRANSLATE_NOOP("FullscreenUI", "Leaderboards are not enabled.");
TRANSLATE_NOOP("FullscreenUI", "Line Detection");
TRANSLATE_NOOP("FullscreenUI", "List Settings");
TRANSLATE_NOOP("FullscreenUI", "Load Database Cheats");
TRANSLATE_NOOP("FullscreenUI", "Load Devices From Save States");
TRANSLATE_NOOP("FullscreenUI", "Load Global State");
TRANSLATE_NOOP("FullscreenUI", "Load Profile");
TRANSLATE_NOOP("FullscreenUI", "Load Resume State");
TRANSLATE_NOOP("FullscreenUI", "Load State");
TRANSLATE_NOOP("FullscreenUI", "Loads all replacement texture to RAM, reducing stuttering at runtime.");
TRANSLATE_NOOP("FullscreenUI", "Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay.");
TRANSLATE_NOOP("FullscreenUI", "Log Level");
TRANSLATE_NOOP("FullscreenUI", "Log To Debug Console");
TRANSLATE_NOOP("FullscreenUI", "Log To File");
TRANSLATE_NOOP("FullscreenUI", "Log To System Console");
TRANSLATE_NOOP("FullscreenUI", "Logging");
TRANSLATE_NOOP("FullscreenUI", "Logging Settings");
TRANSLATE_NOOP("FullscreenUI", "Login");
TRANSLATE_NOOP("FullscreenUI", "Login token generated on {}");
TRANSLATE_NOOP("FullscreenUI", "Logout");
TRANSLATE_NOOP("FullscreenUI", "Logs BIOS calls to printf(). Not all games contain debugging messages.");
TRANSLATE_NOOP("FullscreenUI", "Logs in to RetroAchievements.");
TRANSLATE_NOOP("FullscreenUI", "Logs messages to duckstation.log in the user directory.");
TRANSLATE_NOOP("FullscreenUI", "Logs messages to the console window.");
TRANSLATE_NOOP("FullscreenUI", "Logs messages to the debug console where supported.");
TRANSLATE_NOOP("FullscreenUI", "Logs out of RetroAchievements.");
TRANSLATE_NOOP("FullscreenUI", "Macro Button {}");
TRANSLATE_NOOP("FullscreenUI", "Makes games run closer to their console framerate, at a small cost to performance.");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Busy");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Directory");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Port {}");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Settings");
TRANSLATE_NOOP("FullscreenUI", "Memory Card {} Type");
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
TRANSLATE_NOOP("FullscreenUI", "No Binding");
TRANSLATE_NOOP("FullscreenUI", "No Game Selected");
TRANSLATE_NOOP("FullscreenUI", "No cheats are available for this game.");
TRANSLATE_NOOP("FullscreenUI", "No input profiles available.");
TRANSLATE_NOOP("FullscreenUI", "No patches are available for this game.");
TRANSLATE_NOOP("FullscreenUI", "No resume save state found.");
TRANSLATE_NOOP("FullscreenUI", "No save present in this slot.");
TRANSLATE_NOOP("FullscreenUI", "No save states found.");
TRANSLATE_NOOP("FullscreenUI", "No, resume the game.");
TRANSLATE_NOOP("FullscreenUI", "None (Double Speed)");
TRANSLATE_NOOP("FullscreenUI", "None (Normal Speed)");
TRANSLATE_NOOP("FullscreenUI", "Not Logged In");
TRANSLATE_NOOP("FullscreenUI", "Not Scanning Subdirectories");
TRANSLATE_NOOP("FullscreenUI", "OK");
TRANSLATE_NOOP("FullscreenUI", "OSD Scale");
TRANSLATE_NOOP("FullscreenUI", "On-Screen Display");
TRANSLATE_NOOP("FullscreenUI", "Open Containing Directory");
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
TRANSLATE_NOOP("FullscreenUI", "Plays sound effects for events such as achievement unlocks and leaderboard submissions.");
TRANSLATE_NOOP("FullscreenUI", "Port {} Controller Type");
TRANSLATE_NOOP("FullscreenUI", "Post-Processing Settings");
TRANSLATE_NOOP("FullscreenUI", "Post-processing chain cleared.");
TRANSLATE_NOOP("FullscreenUI", "Post-processing shaders reloaded.");
TRANSLATE_NOOP("FullscreenUI", "Preload Images to RAM");
TRANSLATE_NOOP("FullscreenUI", "Preload Replacement Textures");
TRANSLATE_NOOP("FullscreenUI", "Preserve Projection Precision");
TRANSLATE_NOOP("FullscreenUI", "Press To Toggle");
TRANSLATE_NOOP("FullscreenUI", "Pressure");
TRANSLATE_NOOP("FullscreenUI", "Prevents the emulator from producing any audible sound.");
TRANSLATE_NOOP("FullscreenUI", "Prevents the screen saver from activating and the host from sleeping while emulation is running.");
TRANSLATE_NOOP("FullscreenUI", "Provides vibration and LED control support over Bluetooth.");
TRANSLATE_NOOP("FullscreenUI", "Push a controller button or axis now.");
TRANSLATE_NOOP("FullscreenUI", "Quick Save");
TRANSLATE_NOOP("FullscreenUI", "RAIntegration is being used instead of the built-in achievements implementation.");
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
TRANSLATE_NOOP("FullscreenUI", "Release Date: %s");
TRANSLATE_NOOP("FullscreenUI", "Reload Shaders");
TRANSLATE_NOOP("FullscreenUI", "Reloads the shaders from disk, applying any changes.");
TRANSLATE_NOOP("FullscreenUI", "Remove From Chain");
TRANSLATE_NOOP("FullscreenUI", "Remove From List");
TRANSLATE_NOOP("FullscreenUI", "Removed stage {} ({}).");
TRANSLATE_NOOP("FullscreenUI", "Removes this shader from the chain.");
TRANSLATE_NOOP("FullscreenUI", "Renames existing save states when saving to a backup file.");
TRANSLATE_NOOP("FullscreenUI", "Rendering");
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
TRANSLATE_NOOP("FullscreenUI", "Rich presence inactive or unsupported.");
TRANSLATE_NOOP("FullscreenUI", "Round Upscaled Texture Coordinates");
TRANSLATE_NOOP("FullscreenUI", "Rounds texture coordinates instead of flooring when upscaling. Can fix misaligned textures in some games, but break others, and is incompatible with texture filtering.");
TRANSLATE_NOOP("FullscreenUI", "Runahead");
TRANSLATE_NOOP("FullscreenUI", "Runahead/Rewind");
TRANSLATE_NOOP("FullscreenUI", "Runs the software renderer in parallel for VRAM readbacks. On some systems, this may result in greater performance.");
TRANSLATE_NOOP("FullscreenUI", "SDL DualSense Player LED");
TRANSLATE_NOOP("FullscreenUI", "SDL DualShock 4 / DualSense Enhanced Mode");
TRANSLATE_NOOP("FullscreenUI", "Safe Mode");
TRANSLATE_NOOP("FullscreenUI", "Save Profile");
TRANSLATE_NOOP("FullscreenUI", "Save Screenshot");
TRANSLATE_NOOP("FullscreenUI", "Save State");
TRANSLATE_NOOP("FullscreenUI", "Save State Compression");
TRANSLATE_NOOP("FullscreenUI", "Save State On Exit");
TRANSLATE_NOOP("FullscreenUI", "Saved {:%c}");
TRANSLATE_NOOP("FullscreenUI", "Saves state periodically so you can rewind any mistakes while playing.");
TRANSLATE_NOOP("FullscreenUI", "Scaled Dithering");
TRANSLATE_NOOP("FullscreenUI", "Scales internal VRAM resolution by the specified multiplier. Some games require 1x VRAM resolution.");
TRANSLATE_NOOP("FullscreenUI", "Scales the dithering pattern with the internal rendering resolution, making it less noticeable. Usually safe to enable.");
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
TRANSLATE_NOOP("FullscreenUI", "Select Game");
TRANSLATE_NOOP("FullscreenUI", "Select Macro {} Binds");
TRANSLATE_NOOP("FullscreenUI", "Select State");
TRANSLATE_NOOP("FullscreenUI", "Selects the GPU to use for rendering.");
TRANSLATE_NOOP("FullscreenUI", "Selects the percentage of the normal clock speed the emulated hardware will run at.");
TRANSLATE_NOOP("FullscreenUI", "Selects the quality at which screenshots will be compressed.");
TRANSLATE_NOOP("FullscreenUI", "Selects the resolution scale that will be applied to the final image. 1x will downsample to the original console resolution.");
TRANSLATE_NOOP("FullscreenUI", "Selects the resolution to use in fullscreen modes.");
TRANSLATE_NOOP("FullscreenUI", "Selects the view that the game list will open to.");
TRANSLATE_NOOP("FullscreenUI", "Serial");
TRANSLATE_NOOP("FullscreenUI", "Session: {}");
TRANSLATE_NOOP("FullscreenUI", "Set Input Binding");
TRANSLATE_NOOP("FullscreenUI", "Sets a threshold for discarding precise values when exceeded. May help with glitches in some games.");
TRANSLATE_NOOP("FullscreenUI", "Sets a threshold for discarding the emulated depth buffer. May help in some games.");
TRANSLATE_NOOP("FullscreenUI", "Sets the fast forward speed. It is not guaranteed that this speed will be reached on all systems.");
TRANSLATE_NOOP("FullscreenUI", "Sets the target emulation speed. It is not guaranteed that this speed will be reached on all systems.");
TRANSLATE_NOOP("FullscreenUI", "Sets the turbo speed. It is not guaranteed that this speed will be reached on all systems.");
TRANSLATE_NOOP("FullscreenUI", "Sets the verbosity of messages logged. Higher levels will log more messages.");
TRANSLATE_NOOP("FullscreenUI", "Sets which sort of memory card image will be used for slot {}.");
TRANSLATE_NOOP("FullscreenUI", "Setting {} binding {}.");
TRANSLATE_NOOP("FullscreenUI", "Settings");
TRANSLATE_NOOP("FullscreenUI", "Settings and Operations");
TRANSLATE_NOOP("FullscreenUI", "Shader {} added as stage {}.");
TRANSLATE_NOOP("FullscreenUI", "Shared Card Name");
TRANSLATE_NOOP("FullscreenUI", "Show CPU Usage");
TRANSLATE_NOOP("FullscreenUI", "Show Controller Input");
TRANSLATE_NOOP("FullscreenUI", "Show Enhancement Settings");
TRANSLATE_NOOP("FullscreenUI", "Show FPS");
TRANSLATE_NOOP("FullscreenUI", "Show Frame Times");
TRANSLATE_NOOP("FullscreenUI", "Show GPU Statistics");
TRANSLATE_NOOP("FullscreenUI", "Show GPU Usage");
TRANSLATE_NOOP("FullscreenUI", "Show Latency Statistics");
TRANSLATE_NOOP("FullscreenUI", "Show OSD Messages");
TRANSLATE_NOOP("FullscreenUI", "Show Resolution");
TRANSLATE_NOOP("FullscreenUI", "Show Speed");
TRANSLATE_NOOP("FullscreenUI", "Show Status Indicators");
TRANSLATE_NOOP("FullscreenUI", "Shows a visual history of frame times in the upper-left corner of the display.");
TRANSLATE_NOOP("FullscreenUI", "Shows enhancement settings in the bottom-right corner of the screen.");
TRANSLATE_NOOP("FullscreenUI", "Shows icons in the lower-right corner of the screen when a challenge/primed achievement is active.");
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
TRANSLATE_NOOP("FullscreenUI", "Simulates the CPU's instruction cache in the recompiler. Can help with games running too fast.");
TRANSLATE_NOOP("FullscreenUI", "Simulates the region check present in original, unmodified consoles.");
TRANSLATE_NOOP("FullscreenUI", "Simulates the system ahead of time and rolls back/replays to reduce input lag. Very high system requirements.");
TRANSLATE_NOOP("FullscreenUI", "Skip Duplicate Frame Display");
TRANSLATE_NOOP("FullscreenUI", "Skips the presentation/display of frames that are not unique. Can result in worse frame pacing.");
TRANSLATE_NOOP("FullscreenUI", "Slow Boot");
TRANSLATE_NOOP("FullscreenUI", "Smooth Scrolling");
TRANSLATE_NOOP("FullscreenUI", "Smooths out blockyness between colour transitions in 24-bit content, usually FMVs.");
TRANSLATE_NOOP("FullscreenUI", "Smooths out the blockiness of magnified textures on 2D objects.");
TRANSLATE_NOOP("FullscreenUI", "Smooths out the blockiness of magnified textures on 3D objects.");
TRANSLATE_NOOP("FullscreenUI", "Sort By");
TRANSLATE_NOOP("FullscreenUI", "Sort Reversed");
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
TRANSLATE_NOOP("FullscreenUI", "Start File");
TRANSLATE_NOOP("FullscreenUI", "Start Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Start Game");
TRANSLATE_NOOP("FullscreenUI", "Start a game from a disc in your PC's DVD drive.");
TRANSLATE_NOOP("FullscreenUI", "Start the console without any disc inserted.");
TRANSLATE_NOOP("FullscreenUI", "Stores the current settings to an input profile.");
TRANSLATE_NOOP("FullscreenUI", "Stretch Display Vertically");
TRANSLATE_NOOP("FullscreenUI", "Stretch Mode");
TRANSLATE_NOOP("FullscreenUI", "Stretches the display to match the aspect ratio by multiplying vertically instead of horizontally.");
TRANSLATE_NOOP("FullscreenUI", "Summary");
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
TRANSLATE_NOOP("FullscreenUI", "The XInput source provides support for XBox 360/XBox One/XBox Series controllers.");
TRANSLATE_NOOP("FullscreenUI", "The audio backend determines how frames produced by the emulator are submitted to the host.");
TRANSLATE_NOOP("FullscreenUI", "The selected memory card image will be used in shared mode for this slot.");
TRANSLATE_NOOP("FullscreenUI", "The texture cache is currently experimental, and may cause rendering errors in some games.");
TRANSLATE_NOOP("FullscreenUI", "This game has no achievements.");
TRANSLATE_NOOP("FullscreenUI", "This game has no leaderboards.");
TRANSLATE_NOOP("FullscreenUI", "Threaded Rendering");
TRANSLATE_NOOP("FullscreenUI", "Time Played");
TRANSLATE_NOOP("FullscreenUI", "Time Played: %s");
TRANSLATE_NOOP("FullscreenUI", "Timing out in {:.0f} seconds...");
TRANSLATE_NOOP("FullscreenUI", "Title");
TRANSLATE_NOOP("FullscreenUI", "Toggle Analog");
TRANSLATE_NOOP("FullscreenUI", "Toggle Fast Forward");
TRANSLATE_NOOP("FullscreenUI", "Toggle Fullscreen");
TRANSLATE_NOOP("FullscreenUI", "Toggle every %d frames");
TRANSLATE_NOOP("FullscreenUI", "Toggles the macro when the button is pressed, instead of held.");
TRANSLATE_NOOP("FullscreenUI", "Trigger");
TRANSLATE_NOOP("FullscreenUI", "True Color Rendering");
TRANSLATE_NOOP("FullscreenUI", "Turbo Speed");
TRANSLATE_NOOP("FullscreenUI", "Type");
TRANSLATE_NOOP("FullscreenUI", "UI Language");
TRANSLATE_NOOP("FullscreenUI", "Uncompressed Size");
TRANSLATE_NOOP("FullscreenUI", "Uncompressed Size: %.2f MB");
TRANSLATE_NOOP("FullscreenUI", "Undo Load State");
TRANSLATE_NOOP("FullscreenUI", "Ungrouped");
TRANSLATE_NOOP("FullscreenUI", "Unknown");
TRANSLATE_NOOP("FullscreenUI", "Unknown File Size");
TRANSLATE_NOOP("FullscreenUI", "Unlimited");
TRANSLATE_NOOP("FullscreenUI", "Use Blit Swap Chain");
TRANSLATE_NOOP("FullscreenUI", "Use Debug GPU Device");
TRANSLATE_NOOP("FullscreenUI", "Use Global Setting");
TRANSLATE_NOOP("FullscreenUI", "Use Light Theme");
TRANSLATE_NOOP("FullscreenUI", "Use Old MDEC Routines");
TRANSLATE_NOOP("FullscreenUI", "Use Single Card For Multi-Disc Games");
TRANSLATE_NOOP("FullscreenUI", "Use Software Renderer For Readbacks");
TRANSLATE_NOOP("FullscreenUI", "Username: {}");
TRANSLATE_NOOP("FullscreenUI", "Uses PGXP for all instructions, not just memory operations.");
TRANSLATE_NOOP("FullscreenUI", "Uses a blit presentation model instead of flipping. This may be needed on some systems.");
TRANSLATE_NOOP("FullscreenUI", "Uses a light coloured theme instead of the default dark theme.");
TRANSLATE_NOOP("FullscreenUI", "Uses a second thread for drawing graphics. Speed boost, and safe to use.");
TRANSLATE_NOOP("FullscreenUI", "Uses game-specific settings for controllers for this game.");
TRANSLATE_NOOP("FullscreenUI", "Uses native resolution coordinates for 2D polygons, instead of precise coordinates. Can fix misaligned UI in some games, but otherwise should be left disabled.");
TRANSLATE_NOOP("FullscreenUI", "Uses perspective-correct interpolation for colors, which can improve visuals in some games.");
TRANSLATE_NOOP("FullscreenUI", "Uses perspective-correct interpolation for texture coordinates, straightening out warped textures.");
TRANSLATE_NOOP("FullscreenUI", "Uses screen positions to resolve PGXP data. May improve visuals in some games.");
TRANSLATE_NOOP("FullscreenUI", "Utilizes the chosen video timing regardless of the game's setting.");
TRANSLATE_NOOP("FullscreenUI", "Value: {} | Default: {} | Minimum: {} | Maximum: {}");
TRANSLATE_NOOP("FullscreenUI", "Version: %s");
TRANSLATE_NOOP("FullscreenUI", "Vertex Cache");
TRANSLATE_NOOP("FullscreenUI", "Vertical Sync (VSync)");
TRANSLATE_NOOP("FullscreenUI", "WARNING: Activating cheats can cause unpredictable behavior, crashing, soft-locks, or broken saved games.");
TRANSLATE_NOOP("FullscreenUI", "WARNING: Activating game patches can cause unpredictable behavior, crashing, soft-locks, or broken saved games.");
TRANSLATE_NOOP("FullscreenUI", "WARNING: Your game is still saving to the memory card. Continuing to {0} may IRREVERSIBLY DESTROY YOUR MEMORY CARD. We recommend resuming your game and waiting 5 seconds for it to finish saving.\n\nDo you want to {0} anyway?");
TRANSLATE_NOOP("FullscreenUI", "When enabled and logged in, DuckStation will scan for achievements on startup.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, DuckStation will assume all achievements are locked and not send any unlock notifications to the server.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, DuckStation will list achievements from unofficial sets. These achievements are not tracked by RetroAchievements.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, each session will behave as if no achievements have been unlocked.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, memory cards and controllers will be overwritten when save states are loaded.");
TRANSLATE_NOOP("FullscreenUI", "When enabled, the minimum supported output latency will be used for the host API.");
TRANSLATE_NOOP("FullscreenUI", "When playing a multi-disc game and using per-game (title) memory cards, use a single memory card for all discs.");
TRANSLATE_NOOP("FullscreenUI", "When this option is chosen, the clock speed set below will be used.");
TRANSLATE_NOOP("FullscreenUI", "Widescreen Rendering");
TRANSLATE_NOOP("FullscreenUI", "Wireframe Rendering");
TRANSLATE_NOOP("FullscreenUI", "Writes backgrounds that can be replaced to the dump directory.");
TRANSLATE_NOOP("FullscreenUI", "Yes, {} now and risk memory card corruption.");
TRANSLATE_NOOP("FullscreenUI", "\"Challenge\" mode for achievements, including leaderboard tracking. Disables save state, cheats, and slowdown functions.");
TRANSLATE_NOOP("FullscreenUI", "\"PlayStation\" and \"PSX\" are registered trademarks of Sony Interactive Entertainment Europe Limited. This software is not affiliated in any way with Sony Interactive Entertainment.");
TRANSLATE_NOOP("FullscreenUI", "change disc");
TRANSLATE_NOOP("FullscreenUI", "reset");
TRANSLATE_NOOP("FullscreenUI", "shut down");
TRANSLATE_NOOP("FullscreenUI", "{:%H:%M}");
TRANSLATE_NOOP("FullscreenUI", "{:%Y-%m-%d %H:%M:%S}");
TRANSLATE_NOOP("FullscreenUI", "{} Frames");
TRANSLATE_NOOP("FullscreenUI", "{} deleted.");
TRANSLATE_NOOP("FullscreenUI", "{} does not exist.");
TRANSLATE_NOOP("FullscreenUI", "{} is not a valid disc image.");
// TRANSLATION-STRING-AREA-END
#endif
