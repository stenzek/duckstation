// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "fullscreen_ui.h"
#include "achievements.h"
#include "bios.h"
#include "cheats.h"
#include "controller.h"
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
#include "util/imgui_fullscreen.h"
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

using ImGuiFullscreen::ChoiceDialogOptions;
using ImGuiFullscreen::FocusResetType;

using ImGuiFullscreen::LAYOUT_CENTER_ALIGN_TEXT;
using ImGuiFullscreen::LAYOUT_FOOTER_HEIGHT;
using ImGuiFullscreen::LAYOUT_HORIZONTAL_MENU_ITEM_IMAGE_SIZE;
using ImGuiFullscreen::LAYOUT_LARGE_FONT_SIZE;
using ImGuiFullscreen::LAYOUT_LARGE_POPUP_PADDING;
using ImGuiFullscreen::LAYOUT_LARGE_POPUP_ROUNDING;
using ImGuiFullscreen::LAYOUT_MEDIUM_FONT_SIZE;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_SPACING;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING;
using ImGuiFullscreen::LAYOUT_MENU_WINDOW_X_PADDING;
using ImGuiFullscreen::LAYOUT_MENU_WINDOW_Y_PADDING;
using ImGuiFullscreen::LAYOUT_SCREEN_HEIGHT;
using ImGuiFullscreen::LAYOUT_SCREEN_WIDTH;
using ImGuiFullscreen::LAYOUT_SMALL_POPUP_PADDING;
using ImGuiFullscreen::LAYOUT_WIDGET_FRAME_ROUNDING;
using ImGuiFullscreen::UIStyle;

using ImGuiFullscreen::AddNotification;
using ImGuiFullscreen::BeginFixedPopupDialog;
using ImGuiFullscreen::BeginFullscreenColumns;
using ImGuiFullscreen::BeginFullscreenColumnWindow;
using ImGuiFullscreen::BeginFullscreenWindow;
using ImGuiFullscreen::BeginHorizontalMenu;
using ImGuiFullscreen::BeginHorizontalMenuButtons;
using ImGuiFullscreen::BeginMenuButtons;
using ImGuiFullscreen::BeginNavBar;
using ImGuiFullscreen::CancelPendingMenuClose;
using ImGuiFullscreen::CenterImage;
using ImGuiFullscreen::CloseFixedPopupDialog;
using ImGuiFullscreen::CloseFixedPopupDialogImmediately;
using ImGuiFullscreen::DarkerColor;
using ImGuiFullscreen::EndFixedPopupDialog;
using ImGuiFullscreen::EndFullscreenColumns;
using ImGuiFullscreen::EndFullscreenColumnWindow;
using ImGuiFullscreen::EndFullscreenWindow;
using ImGuiFullscreen::EndHorizontalMenu;
using ImGuiFullscreen::EndHorizontalMenuButtons;
using ImGuiFullscreen::EndMenuButtons;
using ImGuiFullscreen::EndNavBar;
using ImGuiFullscreen::EnumChoiceButton;
using ImGuiFullscreen::FloatingButton;
using ImGuiFullscreen::ForceKeyNavEnabled;
using ImGuiFullscreen::GetCachedTexture;
using ImGuiFullscreen::GetCachedTextureAsync;
using ImGuiFullscreen::GetPlaceholderTexture;
using ImGuiFullscreen::HorizontalMenuButton;
using ImGuiFullscreen::HorizontalMenuItem;
using ImGuiFullscreen::IsAnyFixedPopupDialogOpen;
using ImGuiFullscreen::IsFixedPopupDialogOpen;
using ImGuiFullscreen::IsFocusResetFromWindowChange;
using ImGuiFullscreen::IsFocusResetQueued;
using ImGuiFullscreen::IsGamepadInputSource;
using ImGuiFullscreen::LayoutScale;
using ImGuiFullscreen::LayoutUnscale;
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
using ImGuiFullscreen::OpenFixedPopupDialog;
using ImGuiFullscreen::OpenInfoMessageDialog;
using ImGuiFullscreen::OpenInputStringDialog;
using ImGuiFullscreen::PopPrimaryColor;
using ImGuiFullscreen::PushPrimaryColor;
using ImGuiFullscreen::QueueResetFocus;
using ImGuiFullscreen::RangeButton;
using ImGuiFullscreen::RenderShadowedTextClipped;
using ImGuiFullscreen::ResetFocusHere;
using ImGuiFullscreen::RightAlignNavButtons;
using ImGuiFullscreen::SetFullscreenFooterText;
using ImGuiFullscreen::SetWindowNavWrapping;
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
  Settings,
  PauseMenu,
  SaveStateSelector,
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
static void PauseForMenuOpen(bool set_pause_menu_open);
static bool AreAnyDialogsOpen();
static void ClosePauseMenu();
static void ClosePauseMenuImmediately();
static void SwitchToMainWindow(MainWindowType type);
static void ReturnToMainWindow(float transition_time = GPUThread::HasGPUBackend() ? SHORT_TRANSITION_TIME :
                                                                                    DEFAULT_TRANSITION_TIME);
static void DrawLandingTemplate(ImVec2* menu_pos, ImVec2* menu_size);
static void DrawLandingWindow();
static void DrawStartGameWindow();
static void DrawExitWindow();
static void DrawPauseMenu();
static void ExitFullscreenAndOpenURL(std::string_view url);
static void CopyTextToClipboard(std::string title, std::string_view text);
static void DrawAboutWindow();
static void FixStateIfPaused();
static void GetStandardSelectionFooterText(SmallStringBase& dest, bool back_instead_of_cancel);
static bool CompileTransitionPipelines();

//////////////////////////////////////////////////////////////////////////
// Backgrounds
//////////////////////////////////////////////////////////////////////////

static constexpr const char* DEFAULT_BACKGROUND_NAME = "StaticGray";

static bool HasBackground();
static void LoadBackground();
static bool LoadBackgroundShader(const std::string& path, Error* error);
static bool LoadBackgroundImage(const std::string& path, Error* error);
static void DrawBackground();
static void DrawShaderBackgroundCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd);
static ChoiceDialogOptions GetBackgroundOptions(const TinyString& current_value);
static ImVec4 GetTransparentBackgroundColor(const ImVec4& no_background_color = UIStyle.BackgroundColor);

//////////////////////////////////////////////////////////////////////////
// Resources
//////////////////////////////////////////////////////////////////////////
static bool LoadResources();
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
static bool ShouldOpenToGameList();
static ImGuiFullscreen::FileSelectorFilters GetDiscImageFilters();
static ImGuiFullscreen::FileSelectorFilters GetImageFilters();
static void DoStartPath(std::string path, std::string state = std::string(),
                        std::optional<bool> fast_boot = std::nullopt);
static void DoResume();
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
static void DoSetCoverImage(std::string entry_path);
static void DoSetCoverImage(std::string source_path, std::string existing_path, std::string new_path);

//////////////////////////////////////////////////////////////////////////
// Settings
//////////////////////////////////////////////////////////////////////////

static void InitializeHotkeyList();
static void SwitchToSettings();
static bool SwitchToGameSettings();
static void SwitchToGameSettings(const GameList::Entry* entry);
static bool SwitchToGameSettingsForPath(const std::string& path);
static void SwitchToGameSettingsForSerial(std::string_view serial, GameHash hash);
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
static void DrawAchievementsSettingsPage(std::unique_lock<std::mutex>& settings_lock);
static void DrawAchievementsSettingsHeader(SettingsInterface* bsi, std::unique_lock<std::mutex>& settings_lock);
static void DrawAchievementsLoginWindow();
static void DrawAdvancedSettingsPage();
static void DrawPatchesOrCheatsSettingsPage(bool cheats);
static void DrawCoverDownloaderWindow();

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
static void BeginResetSettings();
static void DoCopyGameSettings();
static void DoClearGameSettings();
static void CopyGlobalControllerSettingsToGame();
static void BeginResetControllerSettings();
static void DoLoadInputProfile();
static void DoSaveInputProfile();
static void DoSaveNewInputProfile();
static void DoSaveInputProfile(const std::string& name);

static bool DrawToggleSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                              const char* section, const char* key, bool default_value, bool enabled = true,
                              bool allow_tristate = true);
static void DrawIntListSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                               const char* section, const char* key, int default_value,
                               std::span<const char* const> options, bool translate_options = true,
                               int option_offset = 0, bool enabled = true,
                               std::string_view tr_context = FSUI_TR_CONTEXT);
static void DrawIntListSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                               const char* section, const char* key, int default_value,
                               std::span<const char* const> options, bool translate_options,
                               std::span<const int> values, bool enabled = true,
                               std::string_view tr_context = FSUI_TR_CONTEXT);
static void DrawIntRangeSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                const char* section, const char* key, int default_value, int min_value, int max_value,
                                const char* format = "%d", bool enabled = true);
static void DrawIntSpinBoxSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                  const char* section, const char* key, int default_value, int min_value, int max_value,
                                  int step_value, const char* format = "%d", bool enabled = true);
static void DrawFloatRangeSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                  const char* section, const char* key, float default_value, float min_value,
                                  float max_value, const char* format = "%f", float multiplier = 1.0f,
                                  bool enabled = true);
static void DrawFloatSpinBoxSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                    const char* section, const char* key, float default_value, float min_value,
                                    float max_value, float step_value, float multiplier, const char* format = "%f",
                                    bool enabled = true);
static bool DrawIntRectSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                               const char* section, const char* left_key, int default_left, const char* top_key,
                               int default_top, const char* right_key, int default_right, const char* bottom_key,
                               int default_bottom, int min_value, int max_value, const char* format = "%d",
                               bool enabled = true);
static void DrawStringListSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                  const char* section, const char* key, const char* default_value,
                                  std::span<const char* const> options, std::span<const char* const> option_values,
                                  bool enabled = true, void (*changed_callback)(std::string_view) = nullptr,
                                  std::string_view tr_context = FSUI_TR_CONTEXT);
template<typename DataType, typename SizeType>
static void DrawEnumSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                            const char* section, const char* key, DataType default_value,
                            std::optional<DataType> (*from_string_function)(const char* str),
                            const char* (*to_string_function)(DataType value),
                            const char* (*to_display_string_function)(DataType value), SizeType option_count,
                            bool enabled = true);
static void DrawFloatListSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                 const char* section, const char* key, float default_value, const char* const* options,
                                 const float* option_values, size_t option_count, bool translate_options,
                                 bool enabled = true);
static void DrawFolderSetting(SettingsInterface* bsi, std::string_view title, const char* section, const char* key,
                              const std::string& runtime_var);

static void PopulateGraphicsAdapterList();
static void PopulateGameListDirectoryCache(SettingsInterface* si);
static void PopulatePatchesAndCheatsList();
static void PopulatePostProcessingChain(SettingsInterface* si, const char* section);
static void BeginVibrationMotorBinding(SettingsInterface* bsi, InputBindingInfo::Type type, const char* section,
                                       const char* key, std::string_view display_name);
static void DrawInputBindingButton(SettingsInterface* bsi, InputBindingInfo::Type type, const char* section,
                                   const char* name, std::string_view display_name, std::string_view icon_name,
                                   bool show_type = true);
static void StartAutomaticBindingForPort(u32 port);
static void StartClearBindingsForPort(u32 port);

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
static void OpenSaveStateSelector(const std::string& serial, const std::string& path, bool is_loading);
static void DrawSaveStateSelector();
static bool OpenLoadStateSelectorForGameResume(const GameList::Entry* entry);
static void DrawResumeStateSelector();

//////////////////////////////////////////////////////////////////////////
// Game List
//////////////////////////////////////////////////////////////////////////
static void DrawGameListWindow();
static void DrawGameList(const ImVec2& heading_size);
static void DrawGameGrid(const ImVec2& heading_size);
static void HandleGameListActivate(const GameList::Entry* entry);
static void HandleGameListOptions(const GameList::Entry* entry);
static void HandleSelectDiscForDiscSet(const GameDatabase::DiscSetEntry* dsentry);
static void DrawGameListSettingsPage();
static void SwitchToGameList();
static void PopulateGameListEntryList();
std::string_view GetKeyForGameListEntry(const GameList::Entry* entry);
static GPUTexture* GetTextureForGameListEntryType(GameList::EntryType type);
static GPUTexture* GetGameListCover(const GameList::Entry* entry, bool fallback_to_achievements_icon,
                                    bool fallback_to_icon);
static GPUTexture* GetGameListCoverTrophy(const GameList::Entry* entry, const ImVec2& image_size);
static GPUTexture* GetCoverForCurrentGame(const std::string& game_path);
static void SwitchToAchievements();
static void SwitchToLeaderboards();

//////////////////////////////////////////////////////////////////////////
// Constants
//////////////////////////////////////////////////////////////////////////

static constexpr const std::array s_ps_button_mapping{
  std::make_pair(ICON_PF_XBOX_DPAD_LEFT, ICON_PF_DPAD_LEFT),
  std::make_pair(ICON_PF_XBOX_DPAD_UP, ICON_PF_DPAD_UP),
  std::make_pair(ICON_PF_XBOX_DPAD_RIGHT, ICON_PF_DPAD_RIGHT),
  std::make_pair(ICON_PF_XBOX_DPAD_DOWN, ICON_PF_DPAD_DOWN),
  std::make_pair(ICON_PF_XBOX_DPAD_LEFT_RIGHT, ICON_PF_DPAD_LEFT_RIGHT),
  std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, ICON_PF_DPAD_UP_DOWN),
  std::make_pair(ICON_PF_BUTTON_A, ICON_PF_BUTTON_CROSS),
  std::make_pair(ICON_PF_BUTTON_B, ICON_PF_BUTTON_CIRCLE),
  std::make_pair(ICON_PF_BUTTON_X, ICON_PF_BUTTON_SQUARE),
  std::make_pair(ICON_PF_BUTTON_Y, ICON_PF_BUTTON_TRIANGLE),
  std::make_pair(ICON_PF_SHARE_CAPTURE, ICON_PF_DUALSHOCK_SHARE),
  std::make_pair(ICON_PF_BURGER_MENU, ICON_PF_DUALSHOCK_OPTIONS),
  std::make_pair(ICON_PF_XBOX, ICON_PF_PLAYSTATION),
  std::make_pair(ICON_PF_LEFT_SHOULDER_LB, ICON_PF_LEFT_SHOULDER_L1),
  std::make_pair(ICON_PF_LEFT_TRIGGER_LT, ICON_PF_LEFT_TRIGGER_L2),
  std::make_pair(ICON_PF_RIGHT_SHOULDER_RB, ICON_PF_RIGHT_SHOULDER_R1),
  std::make_pair(ICON_PF_RIGHT_TRIGGER_RT, ICON_PF_RIGHT_TRIGGER_R2),
};

static constexpr std::array s_theme_names = {
  FSUI_NSTR("Automatic"),  FSUI_NSTR("Dark"),        FSUI_NSTR("Light"),       FSUI_NSTR("AMOLED"),
  FSUI_NSTR("Cobalt Sky"), FSUI_NSTR("Grey Matter"), FSUI_NSTR("Green Giant"), FSUI_NSTR("Pinky Pals"),
  FSUI_NSTR("Dark Ruby"),  FSUI_NSTR("Purple Rain")};

static constexpr std::array s_theme_values = {"",           "Dark",       "Light",     "AMOLED",   "CobaltSky",
                                              "GreyMatter", "GreenGiant", "PinkyPals", "DarkRuby", "PurpleRain"};

static constexpr std::string_view RESUME_STATE_SELECTOR_DIALOG_NAME = "##resume_state_selector";
static constexpr std::string_view ABOUT_DIALOG_NAME = "##about_duckstation";
static constexpr std::string_view ACHIEVEMENTS_LOGIN_DIALOG_NAME = "##achievements_login";
static constexpr std::string_view COVER_DOWNLOADER_DIALOG_NAME = "##cover_downloader";

//////////////////////////////////////////////////////////////////////////
// State
//////////////////////////////////////////////////////////////////////////

namespace {

class InputBindingDialog : public ImGuiFullscreen::PopupDialog
{
public:
  InputBindingDialog();
  ~InputBindingDialog();

  void Draw();
  void ClearState();

  void Start(SettingsInterface* bsi, InputBindingInfo::Type type, std::string_view section, std::string_view key,
             std::string_view display_name);

private:
  static constexpr float INPUT_BINDING_TIMEOUT_SECONDS = 5.0f;

  std::string m_binding_section;
  std::string m_binding_key;
  std::string m_display_name;
  std::vector<InputBindingKey> m_new_bindings;
  std::vector<std::pair<InputBindingKey, std::pair<float, float>>> m_value_ranges;
  float m_time_remaining = 0.0f;
  InputBindingInfo::Type m_binding_type = InputBindingInfo::Type::Unknown;
};

struct ALIGN_TO_CACHE_LINE UIState
{
  // Main
  TransitionState transition_state = TransitionState::Inactive;
  MainWindowType current_main_window = MainWindowType::None;
  PauseSubMenu current_pause_submenu = PauseSubMenu::None;
  MainWindowType previous_main_window = MainWindowType::None;
  bool initialized = false;
  bool tried_to_initialize = false;
  bool pause_menu_was_open = false;
  bool was_paused_on_quick_menu_open = false;
  std::string achievements_user_badge_path;

  // Resources
  std::shared_ptr<GPUTexture> app_icon_texture;
  std::shared_ptr<GPUTexture> fallback_disc_texture;
  std::shared_ptr<GPUTexture> fallback_exe_texture;
  std::shared_ptr<GPUTexture> fallback_psf_texture;
  std::shared_ptr<GPUTexture> fallback_playlist_texture;

  // Background
  std::unique_ptr<GPUTexture> app_background_texture;
  std::unique_ptr<GPUPipeline> app_background_shader;
  Timer::Value app_background_load_time = 0;

  // Transition Resources
  TransitionStartCallback transition_start_callback;
  std::unique_ptr<GPUTexture> transition_prev_texture;
  std::unique_ptr<GPUTexture> transition_current_texture;
  std::unique_ptr<GPUPipeline> transition_blend_pipeline;
  float transition_total_time = 0.0f;
  float transition_remaining_time = 0.0f;

  // Pause Menu
  std::time_t current_time = 0;
  std::string current_time_string;

  // Settings
  float settings_last_bg_alpha = 1.0f;
  SettingsPage settings_page = SettingsPage::Interface;
  std::unique_ptr<INISettingsInterface> game_settings_interface;
  std::string game_settings_serial;
  GameHash game_settings_hash = 0;
  const GameDatabase::Entry* game_settings_db_entry;
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
  bool controller_macro_expanded[NUM_CONTROLLER_AND_CARD_PORTS][InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER] = {};
  InputBindingDialog input_binding_dialog;

  // Save State List
  std::vector<SaveStateListEntry> save_state_selector_slots;
  bool save_state_selector_loading = true;

  // Lazily populated cover images.
  std::unordered_map<std::string, std::string> cover_image_map;
  std::unordered_map<std::string, std::string> icon_image_map;
  std::vector<const GameList::Entry*> game_list_sorted_entries;
  GameListView game_list_view = GameListView::Grid;
  std::string game_list_current_selection_path;
  float game_list_current_selection_timeout = 0.0f;
  bool game_list_show_trophy_icons = true;
  bool game_grid_show_titles = true;
  bool show_localized_titles = true;
};

} // namespace

static UIState s_state;

} // namespace FullscreenUI

//////////////////////////////////////////////////////////////////////////
// Utility
//////////////////////////////////////////////////////////////////////////

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
  ImGuiFullscreen::SetFullscreenFooterText(text, GetBackgroundAlpha());
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

std::vector<std::string_view> FullscreenUI::GetThemeNames()
{
  std::vector<std::string_view> ret;
  ret.reserve(std::size(s_theme_names));
  for (const char* name : s_theme_names)
    ret.push_back(TRANSLATE_SV("FullscreenUI", name));
  return ret;
}

std::span<const char* const> FullscreenUI::GetThemeConfigNames()
{
  return s_theme_values;
}

void FullscreenUI::SetTheme()
{
  TinyString theme =
    Host::GetBaseTinyStringSettingValue("UI", "FullscreenUITheme", Host::GetDefaultFullscreenUITheme());
  if (theme.empty())
    theme = Host::GetDefaultFullscreenUITheme();

  ImGuiFullscreen::SetTheme(theme);
}

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////

bool FullscreenUI::Initialize()
{
  if (s_state.initialized)
    return true;

  // some achievement callbacks fire early while e.g. there is a load state popup blocking system init
  if (s_state.tried_to_initialize || !ImGuiManager::IsInitialized())
    return false;

  s_state.show_localized_titles = Host::GetBaseBoolSettingValue("Main", "FullscreenUIShowLocalizedTitles", true);

  ImGuiFullscreen::SetAnimations(Host::GetBaseBoolSettingValue("Main", "FullscreenUIAnimations", true));
  ImGuiFullscreen::SetSmoothScrolling(Host::GetBaseBoolSettingValue("Main", "FullscreenUISmoothScrolling", true));
  ImGuiFullscreen::SetMenuBorders(Host::GetBaseBoolSettingValue("Main", "FullscreenUIMenuBorders", false));

  if (Host::GetBaseBoolSettingValue("Main", "FullscreenUIDisplayPSIcons", false))
    ImGuiFullscreen::SetFullscreenFooterTextIconMapping(s_ps_button_mapping);

  if (!ImGuiFullscreen::Initialize("images/placeholder.png") || !LoadResources())
  {
    DestroyResources();
    Shutdown(true);
    s_state.tried_to_initialize = true;
    return false;
  }

  s_state.initialized = true;

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

  return true;
}

bool FullscreenUI::IsInitialized()
{
  return s_state.initialized;
}

bool FullscreenUI::HasActiveWindow()
{
  return s_state.initialized && (s_state.current_main_window != MainWindowType::None ||
                                 s_state.transition_state != TransitionState::Inactive || AreAnyDialogsOpen());
}

bool FullscreenUI::AreAnyDialogsOpen()
{
  return (s_state.input_binding_dialog.IsOpen() || ImGuiFullscreen::IsAnyFixedPopupDialogOpen() ||
          ImGuiFullscreen::IsChoiceDialogOpen() || ImGuiFullscreen::IsInputDialogOpen() ||
          ImGuiFullscreen::IsFileSelectorOpen() || ImGuiFullscreen::IsMessageBoxDialogOpen());
}

void FullscreenUI::CheckForConfigChanges(const GPUSettings& old_settings)
{
  // NOTE: Called on GPU thread.
}

void FullscreenUI::UpdateRunIdleState()
{
  const bool new_run_idle =
    (HasActiveWindow() || ImGuiFullscreen::HasToast() || ImGuiFullscreen::HasAnyNotifications());
  GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::FullscreenUIActive, new_run_idle);
}

void FullscreenUI::BeginTransition(TransitionStartCallback func, float time)
{
  if (s_state.transition_state == TransitionState::Inactive)
  {
    const float real_time = UIStyle.Animations ? time : 0.0f;
    s_state.transition_state = TransitionState::Starting;
    s_state.transition_total_time = real_time;
    s_state.transition_remaining_time = real_time;
  }

  // run any callback if we queue another transition in the middle of one already active
  if (s_state.transition_start_callback)
  {
    if (s_state.transition_state == TransitionState::Starting)
      WARNING_LOG("More than one transition started");

    std::move(s_state.transition_start_callback)();
  }

  s_state.transition_start_callback = std::move(func);

  UpdateRunIdleState();
}

void FullscreenUI::CancelTransition()
{
  if (s_state.transition_state != TransitionState::Active)
    return;

  if (s_state.transition_start_callback)
    std::move(s_state.transition_start_callback)();

  s_state.transition_state = TransitionState::Inactive;
  s_state.transition_start_callback = {};
  s_state.transition_remaining_time = 0.0f;
}

void FullscreenUI::BeginTransition(float time, TransitionStartCallback func)
{
  BeginTransition(std::move(func), time);
}

bool FullscreenUI::IsTransitionActive()
{
  return (s_state.transition_state != TransitionState::Inactive);
}

FullscreenUI::TransitionState FullscreenUI::GetTransitionState()
{
  return s_state.transition_state;
}

GPUTexture* FullscreenUI::GetTransitionRenderTexture(GPUSwapChain* swap_chain)
{
  if (!g_gpu_device->ResizeTexture(&s_state.transition_current_texture, swap_chain->GetWidth(), swap_chain->GetHeight(),
                                   GPUTexture::Type::RenderTarget, swap_chain->GetFormat(), GPUTexture::Flags::None,
                                   false))
  {
    ERROR_LOG("Failed to allocate {}x{} texture for transition, cancelling.", swap_chain->GetWidth(),
              swap_chain->GetHeight());
    s_state.transition_state = TransitionState::Inactive;
    return nullptr;
  }

  return s_state.transition_current_texture.get();
}

bool FullscreenUI::CompileTransitionPipelines()
{
  const RenderAPI render_api = g_gpu_device->GetRenderAPI();
  const ShaderGen shadergen(render_api, ShaderGen::GetShaderLanguageForAPI(render_api), false, false);
  GPUSwapChain* const swap_chain = g_gpu_device->GetMainSwapChain();

  Error error;
  std::unique_ptr<GPUShader> vs = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                             shadergen.GeneratePassthroughVertexShader(), &error);
  std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                                             shadergen.GenerateFadeFragmentShader(), &error);
  if (!vs || !fs)
  {
    ERROR_LOG("Failed to compile transition shaders: {}", error.GetDescription());
    return false;
  }
  GL_OBJECT_NAME(vs, "Transition Vertex Shader");
  GL_OBJECT_NAME(fs, "Transition Fragment Shader");

  GPUPipeline::GraphicsConfig plconfig;
  GPUBackend::SetScreenQuadInputLayout(plconfig);
  plconfig.layout = GPUPipeline::Layout::MultiTextureAndPushConstants;
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.SetTargetFormats(swap_chain ? swap_chain->GetFormat() : GPUTexture::Format::RGBA8);
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  plconfig.vertex_shader = vs.get();
  plconfig.geometry_shader = nullptr;
  plconfig.fragment_shader = fs.get();

  s_state.transition_blend_pipeline = g_gpu_device->CreatePipeline(plconfig, &error);
  if (!s_state.transition_blend_pipeline)
  {
    ERROR_LOG("Failed to create transition blend pipeline: {}", error.GetDescription());
    return false;
  }

  return true;
}

void FullscreenUI::RenderTransitionBlend(GPUSwapChain* swap_chain)
{
  GPUTexture* const curr = s_state.transition_current_texture.get();
  DebugAssert(curr);

  if (s_state.transition_state == TransitionState::Starting)
  {
    // copy current frame
    if (!g_gpu_device->ResizeTexture(&s_state.transition_prev_texture, curr->GetWidth(), curr->GetHeight(),
                                     GPUTexture::Type::RenderTarget, curr->GetFormat(), GPUTexture::Flags::None, false))
    {
      ERROR_LOG("Failed to allocate {}x{} texture for transition, cancelling.", curr->GetWidth(), curr->GetHeight());
      s_state.transition_state = TransitionState::Inactive;
      return;
    }

    g_gpu_device->CopyTextureRegion(s_state.transition_prev_texture.get(), 0, 0, 0, 0, curr, 0, 0, 0, 0,
                                    curr->GetWidth(), curr->GetHeight());

    s_state.transition_state = TransitionState::Active;
  }

  const float transition_alpha = s_state.transition_remaining_time / s_state.transition_total_time;
  const float uniforms[2] = {1.0f - transition_alpha, transition_alpha};
  g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));
  g_gpu_device->SetPipeline(s_state.transition_blend_pipeline.get());
  g_gpu_device->SetViewportAndScissor(0, 0, swap_chain->GetPostRotatedWidth(), swap_chain->GetPostRotatedHeight());
  g_gpu_device->SetTextureSampler(0, curr, g_gpu_device->GetNearestSampler());
  g_gpu_device->SetTextureSampler(1, s_state.transition_prev_texture.get(), g_gpu_device->GetNearestSampler());

  const GSVector2i size = swap_chain->GetSizeVec();
  const GSVector2i postrotated_size = swap_chain->GetPostRotatedSizeVec();
  const GSVector4 uv_rect = g_gpu_device->UsesLowerLeftOrigin() ? GSVector4::cxpr(0.0f, 1.0f, 1.0f, 0.0f) :
                                                                  GSVector4::cxpr(0.0f, 0.0f, 1.0f, 1.0f);
  GPUPresenter::DrawScreenQuad(GSVector4i::loadh(size), uv_rect, size, postrotated_size, DisplayRotation::Normal,
                               swap_chain->GetPreRotation());
}

void FullscreenUI::UpdateTransitionState()
{
  if (s_state.transition_state == TransitionState::Inactive)
    return;

  // this callback will exist after starting if a second transition gets queued
  if (s_state.transition_start_callback)
  {
    std::move(s_state.transition_start_callback)();
    s_state.transition_start_callback = {};
  }

  s_state.transition_remaining_time -= ImGui::GetIO().DeltaTime;
  if (s_state.transition_remaining_time <= 0.0f)
  {
    // At 1080p we're only talking 2MB of VRAM, 16MB at 4K.. saves reallocating it on the next transition.
    // g_gpu_device->RecycleTexture(std::move(s_state.transition_current_texture));
    // g_gpu_device->RecycleTexture(std::move(s_state.transition_prev_texture));
    s_state.transition_state = TransitionState::Inactive;
  }
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
    if (!Initialize() || s_state.current_main_window != MainWindowType::None)
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
    if (!Initialize() || s_state.current_main_window != MainWindowType::None)
      return;

    PauseForMenuOpen(false);
    ForceKeyNavEnabled();

    BeginTransition(SHORT_TRANSITION_TIME, []() {
      if (!SwitchToGameSettings())
      {
        ClosePauseMenuImmediately();
        return;
      }

      SwitchToMainWindow(MainWindowType::Settings);
      s_state.settings_page = SettingsPage::Cheats;
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

  s_state.previous_main_window = s_state.current_main_window;
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

void FullscreenUI::ReturnToMainWindow(
  float transition_time /* = GPUThread::HasGPUBackend() ? SHORT_TRANSITION_TIME : DEFAULT_TRANSITION_TIME */)
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
    s_state.input_binding_dialog.ClearState();
    ClearSaveStateEntryList();
    s_state.icon_image_map.clear();
    s_state.cover_image_map.clear();
    std::memset(s_state.controller_macro_expanded, 0, sizeof(s_state.controller_macro_expanded));
    s_state.game_list_sorted_entries = {};
    s_state.game_list_directories_cache = {};
    s_state.game_settings_db_entry = nullptr;
    s_state.game_settings_entry.reset();
    s_state.game_settings_hash = 0;
    s_state.game_settings_serial = {};
    s_state.game_settings_interface.reset();
    s_state.game_settings_changed = false;
    s_state.game_patch_list = {};
    s_state.enabled_game_patch_cache = {};
    s_state.game_cheats_list = {};
    s_state.enabled_game_cheat_cache = {};
    s_state.game_cheat_groups = {};
    s_state.postprocessing_stages = {};
    s_state.fullscreen_mode_list_cache = {};
    s_state.graphics_adapter_list_cache = {};
    s_state.hotkey_list_cache = {};
    s_state.current_time_string = {};
    s_state.current_time = 0;
  }

  DestroyResources();
  ImGuiFullscreen::Shutdown(clear_state);

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

  // draw background before any overlays
  if (!GPUThread::HasGPUBackend() && s_state.current_main_window != MainWindowType::None)
    DrawBackground();

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
  else if (IsFixedPopupDialogOpen(COVER_DOWNLOADER_DIALOG_NAME))
    DrawCoverDownloaderWindow();

  s_state.input_binding_dialog.Draw();

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
        if (FileSystem::FileExists(s_state.game_settings_interface->GetPath().c_str()))
        {
          INFO_LOG("Removing empty game settings {}", s_state.game_settings_interface->GetPath());
          if (!FileSystem::DeleteFile(s_state.game_settings_interface->GetPath().c_str(), &error))
          {
            OpenInfoMessageDialog(FSUI_STR("Error"),
                                  fmt::format(FSUI_FSTR("An error occurred while deleting empty game settings:\n{}"),
                                              error.GetDescription()));
          }
        }
      }
      else
      {
        if (!s_state.game_settings_interface->Save(&error))
        {
          OpenInfoMessageDialog(
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
  UpdateTransitionState();
}

void FullscreenUI::InvalidateCoverCache()
{
  if (!GPUThread::IsFullscreenUIRequested())
    return;

  GPUThread::RunOnThread([]() {
    if (!IsInitialized())
      return;

    s_state.cover_image_map.clear();
  });
}

bool FullscreenUI::LoadResources()
{
  s_state.app_icon_texture = LoadTexture("images/duck.png");
  s_state.fallback_disc_texture = LoadTexture("fullscreenui/cdrom.png");
  s_state.fallback_exe_texture = LoadTexture("fullscreenui/exe-file.png");
  s_state.fallback_psf_texture = LoadTexture("fullscreenui/psf-file.png");
  s_state.fallback_playlist_texture = LoadTexture("fullscreenui/playlist-file.png");

  if (!CompileTransitionPipelines())
    return false;

  InitializeHotkeyList();

  return true;
}

void FullscreenUI::DestroyResources()
{
  s_state.transition_blend_pipeline.reset();
  g_gpu_device->RecycleTexture(std::move(s_state.transition_prev_texture));
  g_gpu_device->RecycleTexture(std::move(s_state.transition_current_texture));
  s_state.transition_state = TransitionState::Inactive;
  s_state.transition_start_callback = {};
  s_state.fallback_playlist_texture.reset();
  s_state.fallback_psf_texture.reset();
  s_state.fallback_exe_texture.reset();
  s_state.fallback_disc_texture.reset();
  s_state.app_background_texture.reset();
  s_state.app_background_shader.reset();
  s_state.app_icon_texture.reset();
}

GPUTexture* FullscreenUI::GetUserThemeableTexture(const std::string_view png_name, const std::string_view svg_name,
                                                  bool* is_colorable, const ImVec2& svg_size)
{
  GPUTexture* tex = ImGuiFullscreen::FindCachedTexture(png_name);
  if (tex)
  {
    if (is_colorable)
      *is_colorable = false;

    return tex;
  }

  const u32 svg_width = static_cast<u32>(svg_size.x);
  const u32 svg_height = static_cast<u32>(svg_size.y);
  tex = ImGuiFullscreen::FindCachedTexture(svg_name, svg_width, svg_height);
  if (tex)
    return tex;

  // slow path, check filesystem for override
  if (EmuFolders::Resources != EmuFolders::UserResources &&
      FileSystem::FileExists(Path::Combine(EmuFolders::UserResources, png_name).c_str()))
  {
    // use the user's png
    if (is_colorable)
      *is_colorable = false;

    return ImGuiFullscreen::GetCachedTexture(png_name);
  }

  // otherwise use the system/user svg
  if (is_colorable)
    *is_colorable = true;

  return ImGuiFullscreen::GetCachedTexture(svg_name, svg_width, svg_height);
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

ImGuiFullscreen::FileSelectorFilters FullscreenUI::GetDiscImageFilters()
{
  return {"*.bin",   "*.cue",    "*.iso", "*.img", "*.chd", "*.ecm",     "*.mds", "*.cpe", "*.elf",
          "*.psexe", "*.ps-exe", "*.exe", "*.psx", "*.psf", "*.minipsf", "*.m3u", "*.pbp"};
}

ImGuiFullscreen::FileSelectorFilters FullscreenUI::GetImageFilters()
{
  return {"*.png", "*.jpg", "*.jpeg", "*.webp"};
}

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

  ImGuiFullscreen::ChoiceDialogOptions options;
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
  ImGuiFullscreen::ChoiceDialogOptions options;

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
      if (!Initialize())
        return;

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
    auto matches = GameList::GetEntriesInDiscSet(entry->disc_set, s_state.show_localized_titles);
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
        if (!Initialize())
          return;

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
    if (!Initialize())
      return;

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

void FullscreenUI::DoSetCoverImage(std::string entry_path)
{
  OpenFileSelector(
    FSUI_ICONVSTR(ICON_FA_IMAGE, "Set Cover Image"), false,
    [entry_path = std::move(entry_path)](std::string path) {
      if (path.empty())
        return;

      const auto lock = GameList::GetLock();
      const GameList::Entry* entry = GameList::GetEntryForPath(entry_path);
      if (!entry)
        return;

      std::string existing_path = GameList::GetCoverImagePathForEntry(entry);
      std::string new_path = GameList::GetNewCoverImagePathForEntry(entry, path.c_str(), false);
      if (!existing_path.empty())
      {
        OpenConfirmMessageDialog(
          FSUI_ICONVSTR(ICON_FA_IMAGE, "Set Cover Image"),
          FSUI_STR("A cover already exists for this game. Are you sure that you want to overwrite it?"),
          [path = std::move(path), existing_path = std::move(existing_path),
           new_path = std::move(new_path)](bool result) {
            if (!result)
              return;

            DoSetCoverImage(std::move(path), std::move(existing_path), std::move(new_path));
          });
      }
      else
      {
        DoSetCoverImage(std::move(path), std::move(existing_path), std::move(new_path));
      }
    },
    GetImageFilters(), EmuFolders::Covers);
}

void FullscreenUI::DoSetCoverImage(std::string source_path, std::string existing_path, std::string new_path)
{
  Error error;
  if (!existing_path.empty() && existing_path != new_path && FileSystem::FileExists(existing_path.c_str()))
  {
    if (!FileSystem::DeleteFile(existing_path.c_str(), &error))
    {
      ShowToast({}, fmt::format(FSUI_FSTR("Failed to delete existing cover: {}"), error.GetDescription()));
      return;
    }
  }

  if (!FileSystem::CopyFilePath(source_path.c_str(), new_path.c_str(), true, &error))
  {
    ShowToast({}, fmt::format(FSUI_FSTR("Failed to copy cover: {}"), error.GetDescription()));
    return;
  }

  ShowToast({}, FSUI_STR("Cover set."));

  // Ensure the old one wasn't cached.
  if (!existing_path.empty())
    ImGuiFullscreen::InvalidateCachedTexture(existing_path);
  if (existing_path != new_path)
    ImGuiFullscreen::InvalidateCachedTexture(new_path);
  s_state.cover_image_map.clear();
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

ChoiceDialogOptions FullscreenUI::GetBackgroundOptions(const TinyString& current_value)
{
  static constexpr const char* dir = FS_OSPATH_SEPARATOR_STR "fullscreenui" FS_OSPATH_SEPARATOR_STR "backgrounds";

  ChoiceDialogOptions options;
  options.emplace_back(FSUI_STR("None"), current_value == "None");

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
    const ImRect uv_rect =
      ImGuiFullscreen::FitImage(size, ImVec2(static_cast<float>(s_state.app_background_texture->GetWidth()),
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
      ImGuiFullscreen::RenderShadowedTextClipped(heading_font, heading_font_size, heading_font_weight, text_pos,
                                                 text_pos + text_size, text_color, heading_text, &text_size);
    }

    // draw time
    ImVec2 time_pos;
    {
      UpdateCurrentTimeString();

      const ImVec2 time_size = heading_font->CalcTextSizeA(heading_font_size, heading_font_weight, FLT_MAX, 0.0f,
                                                           IMSTR_START_END(s_state.current_time_string));
      time_pos = ImVec2(heading_size.x - LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING) - time_size.x,
                        LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING));
      ImGuiFullscreen::RenderShadowedTextClipped(heading_font, heading_font_size, heading_font_weight, time_pos,
                                                 time_pos + time_size, text_color, s_state.current_time_string,
                                                 &time_size);
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
        ImGuiFullscreen::RenderShadowedTextClipped(heading_font, heading_font_size, heading_font_weight, name_pos,
                                                   name_pos + name_size, text_color, username, &name_size);

        if (s_state.achievements_user_badge_path.empty()) [[unlikely]]
          s_state.achievements_user_badge_path = Achievements::GetLoggedInUserBadgePath();
        if (!s_state.achievements_user_badge_path.empty()) [[likely]]
        {
          const ImVec2 badge_size = ImVec2(UIStyle.LargeFontSize, UIStyle.LargeFontSize);
          const ImVec2 badge_pos =
            ImVec2(name_pos.x - badge_size.x - LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING), time_pos.y);

          dl->AddImage(reinterpret_cast<ImTextureID>(GetCachedTextureAsync(s_state.achievements_user_badge_path)),
                       badge_pos, badge_pos + badge_size);
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
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Exit"))},
                            GetBackgroundAlpha());
  }
  else
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_F1, FSUI_VSTR("About")),
                                       std::make_pair(ICON_PF_F3, FSUI_VSTR("Resume Last Session")),
                                       std::make_pair(ICON_PF_F11, FSUI_VSTR("Toggle Fullscreen")),
                                       std::make_pair(ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT, FSUI_VSTR("Navigate")),
                                       std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_ESC, FSUI_VSTR("Exit"))},
                            GetBackgroundAlpha());
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
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Back"))},
                            GetBackgroundAlpha());
  }
  else
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT, FSUI_VSTR("Navigate")),
                                       std::make_pair(ICON_PF_F1, FSUI_VSTR("Load Global State")),
                                       std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_ESC, FSUI_VSTR("Back"))},
                            GetBackgroundAlpha());
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

bool FullscreenUI::ShouldShowAdvancedSettings()
{
  return Host::GetBaseBoolSettingValue("Main", "ShowDebugMenu", false);
}

float FullscreenUI::GetBackgroundAlpha()
{
  return GPUThread::HasGPUBackend() ? (s_state.settings_page == SettingsPage::PostProcessing ? 0.50f : 0.90f) :
                                      (HasBackground() ? 0.5f : 1.0f);
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
                                          const char* name, std::string_view display_name, std::string_view icon_name,
                                          bool show_type)
{
  if (type == InputBindingInfo::Type::Pointer || type == InputBindingInfo::Type::RelativePointer)
    return;

  SmallString title;
  SmallString value = bsi->GetSmallStringValue(section, name);
  const bool oneline = value.count('&') <= 1;
  if (oneline && type != InputBindingInfo::Type::Pointer && type != InputBindingInfo::Type::Device)
    InputManager::PrettifyInputBinding(value, &ImGuiFullscreen::GetControllerIconMapping);

  if (show_type)
  {
    if (!icon_name.empty())
    {
      title.format("{} {}", icon_name, display_name);
    }
    else
    {
      switch (type)
      {
        case InputBindingInfo::Type::Button:
          title.format(ICON_FA_CIRCLE_DOT " {}", display_name);
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
  else
  {
    title = display_name;
  }

  title.append_format("##{}/{}", section, name);

  bool clicked;
  if (oneline)
  {
    if (value.empty())
      value.assign(FSUI_VSTR("-"));

    clicked = MenuButtonWithValue(title, {}, value);
  }
  else
  {
    clicked = MenuButton(title, value);
  }

  if (clicked)
  {
    if (type == InputBindingInfo::Type::Motor)
      BeginVibrationMotorBinding(bsi, type, section, name, display_name);
    else
      s_state.input_binding_dialog.Start(bsi, type, section, name, display_name);
  }
  else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_NavGamepadMenu, false))
  {
    CancelPendingMenuClose();
    bsi->DeleteValue(section, name);
    SetSettingsChanged(bsi);
  }
}

FullscreenUI::InputBindingDialog::InputBindingDialog() = default;

FullscreenUI::InputBindingDialog::~InputBindingDialog() = default;

void FullscreenUI::InputBindingDialog::Start(SettingsInterface* bsi, InputBindingInfo::Type type,
                                             std::string_view section, std::string_view key,
                                             std::string_view display_name)
{
  if (m_binding_type != InputBindingInfo::Type::Unknown)
    InputManager::RemoveHook();

  m_binding_type = type;
  m_binding_section = section;
  m_binding_key = key;
  m_display_name = display_name;
  m_new_bindings = {};
  m_value_ranges = {};
  m_time_remaining = INPUT_BINDING_TIMEOUT_SECONDS;
  SetTitleAndOpen(FSUI_ICONSTR(ICON_FA_GAMEPAD, "Set Input Binding"));

  const bool game_settings = IsEditingGameSettings(bsi);

  InputManager::SetHook([this, game_settings](InputBindingKey key, float value) -> InputInterceptHook::CallbackResult {
    // holding the settings lock here will protect the input binding list
    auto lock = Host::GetSettingsLock();

    // shouldn't happen, just in case
    if (m_binding_type == InputBindingInfo::Type::Unknown)
      return InputInterceptHook::CallbackResult::RemoveHookAndContinueProcessingEvent;

    float initial_value = value;
    float min_value = value;
    InputInterceptHook::CallbackResult default_action = InputInterceptHook::CallbackResult::StopProcessingEvent;
    const auto it = std::find_if(m_value_ranges.begin(), m_value_ranges.end(),
                                 [key](const auto& it) { return it.first.bits == key.bits; });

    if (it != m_value_ranges.end())
    {
      initial_value = it->second.first;
      min_value = it->second.second = std::min(it->second.second, value);
    }
    else
    {
      m_value_ranges.emplace_back(key, std::make_pair(initial_value, min_value));

      // forward the event to imgui if it's a new key and a release, because this is what triggered the binding to
      // start if we don't do this, imgui thinks the activate button is held down
      default_action = (value == 0.0f) ? InputInterceptHook::CallbackResult::ContinueProcessingEvent :
                                         InputInterceptHook::CallbackResult::StopProcessingEvent;
    }

    const float abs_value = std::abs(value);
    const bool reverse_threshold =
      (key.source_subtype == InputSubclass::ControllerAxis && std::abs(initial_value) > 0.5f);

    for (InputBindingKey& other_key : m_new_bindings)
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
            m_binding_type, m_new_bindings.data(), m_new_bindings.size()));
          bsi->SetStringValue(m_binding_section.c_str(), m_binding_key.c_str(), new_binding.c_str());
          SetSettingsChanged(bsi);

          // don't try to process any more
          m_binding_type = InputBindingInfo::Type::Unknown;
          GPUThread::RunOnThread([this]() { StartClose(); });

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
      key_to_add.modifier = (value < 0.0f) ? InputModifier::Negate : InputModifier::None;
      key_to_add.invert = reverse_threshold;
      m_new_bindings.push_back(key_to_add);
    }

    return default_action;
  });
}

void FullscreenUI::InputBindingDialog::ClearState()
{
  PopupDialog::ClearState();

  if (m_binding_type != InputBindingInfo::Type::Unknown)
    InputManager::RemoveHook();

  m_binding_type = InputBindingInfo::Type::Unknown;
  m_binding_section = {};
  m_binding_key = {};
  m_display_name = {};
  m_new_bindings = {};
  m_value_ranges = {};
}

void FullscreenUI::InputBindingDialog::Draw()
{
  if (!IsOpen())
    return;

  if (m_time_remaining > 0.0f)
  {
    m_time_remaining -= ImGui::GetIO().DeltaTime;
    if (m_time_remaining <= 0.0f)
    {
      // allow the dialog to fade out, but stop receiving any more events
      m_time_remaining = 0.0f;
      m_binding_type = InputBindingInfo::Type::Unknown;
      InputManager::RemoveHook();
      StartClose();
    }
  }

  if (!BeginRender(LayoutScale(LAYOUT_SMALL_POPUP_PADDING), LayoutScale(LAYOUT_SMALL_POPUP_PADDING),
                   LayoutScale(500.0f, 0.0f)))
  {
    ClearState();
    return;
  }

  ImGui::TextWrapped(
    "%s", SmallString::from_format(FSUI_FSTR("Setting {} binding {}."), m_binding_section, m_display_name).c_str());
  ImGui::TextUnformatted(FSUI_CSTR("Push a controller button or axis now."));
  ImGui::NewLine();
  ImGui::TextUnformatted(SmallString::from_format(FSUI_FSTR("Timing out in {:.0f} seconds..."), m_time_remaining));

  EndRender();
}

void FullscreenUI::BeginVibrationMotorBinding(SettingsInterface* bsi, InputBindingInfo::Type type, const char* section,
                                              const char* key, std::string_view display_name)
{
  // vibration motors use a list to select
  const bool game_settings = IsEditingGameSettings(bsi);
  InputManager::VibrationMotorList motors = InputManager::EnumerateVibrationMotors();
  if (motors.empty())
  {
    ShowToast({}, FSUI_STR("No devices with vibration motors were detected."));
    return;
  }

  const TinyString current_binding = bsi->GetTinyStringValue(section, key);
  size_t current_index = motors.size();
  ChoiceDialogOptions options;
  options.reserve(motors.size() + 1);
  for (size_t i = 0; i < motors.size(); i++)
  {
    std::string text = InputManager::ConvertInputBindingKeyToString(InputBindingInfo::Type::Motor, motors[i]);
    const bool this_index = (current_binding.view() == text);
    current_index = this_index ? i : current_index;
    options.emplace_back(std::move(text), this_index);
  }

  // empty/no mapping value
  options.emplace_back(FSUI_STR("No Vibration"), current_binding.empty());

  // add current value to list if it's not currently available
  if (!current_binding.empty() && current_index == motors.size())
    options.emplace_back(std::make_pair(std::string(current_binding.view()), true));

  OpenChoiceDialog(display_name, false, std::move(options),
                   [game_settings, section = std::string(section), key = std::string(key),
                    motors = std::move(motors)](s32 index, const std::string& title, bool checked) {
                     if (index < 0)
                       return;

                     auto lock = Host::GetSettingsLock();
                     SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                     if (static_cast<size_t>(index) == motors.size())
                       bsi->DeleteValue(section.c_str(), key.c_str());
                     else
                       bsi->SetStringValue(section.c_str(), key.c_str(), title.c_str());
                     SetSettingsChanged(bsi);
                   });
}

bool FullscreenUI::DrawToggleSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                     const char* section, const char* key, bool default_value,
                                     bool enabled /* = true */, bool allow_tristate /* = true */)
{
  if (!allow_tristate || !IsEditingGameSettings(bsi))
  {
    bool value = bsi->GetBoolValue(section, key, default_value);
    if (!ToggleButton(title, summary, &value, enabled))
      return false;

    bsi->SetBoolValue(section, key, value);
  }
  else
  {
    std::optional<bool> value(false);
    if (!bsi->GetBoolValue(section, key, &value.value()))
      value.reset();
    if (!ThreeWayToggleButton(title, summary, &value, enabled))
      return false;

    if (value.has_value())
      bsi->SetBoolValue(section, key, value.value());
    else
      bsi->DeleteValue(section, key);
  }

  SetSettingsChanged(bsi);
  return true;
}

void FullscreenUI::DrawIntListSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                      const char* section, const char* key, int default_value,
                                      std::span<const char* const> options, bool translate_options /* = true */,
                                      int option_offset /* = 0 */, bool enabled /* = true */,
                                      std::string_view tr_context /* = TR_CONTEXT */)
{
  const bool game_settings = IsEditingGameSettings(bsi);

  const std::optional<int> value =
    bsi->GetOptionalIntValue(section, key, game_settings ? std::nullopt : std::optional<int>(default_value));
  const int index = value.has_value() ? (value.value() - option_offset) : std::numeric_limits<int>::min();
  const std::string_view value_text =
    (value.has_value()) ?
      ((index < 0 || static_cast<size_t>(index) >= options.size()) ?
         FSUI_VSTR("Unknown") :
         (translate_options ? Host::TranslateToStringView(tr_context, options[index]) : options[index])) :
      FSUI_VSTR("Use Global Setting");

  if (MenuButtonWithValue(title, summary, value_text, enabled))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(options.size() + 1);
    if (game_settings)
      cd_options.emplace_back(FSUI_STR("Use Global Setting"), !value.has_value());
    for (size_t i = 0; i < options.size(); i++)
    {
      cd_options.emplace_back(translate_options ? Host::TranslateToString(tr_context, options[i]) :
                                                  std::string(options[i]),
                              (i == static_cast<size_t>(index)));
    }
    OpenChoiceDialog(title, false, std::move(cd_options),
                     [game_settings, section = TinyString(section), key = TinyString(key),
                      option_offset](s32 index, const std::string& title, bool checked) {
                       if (index < 0)
                         return;

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
                     });
  }
}

void FullscreenUI::DrawIntListSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                      const char* section, const char* key, int default_value,
                                      std::span<const char* const> options, bool translate_options,
                                      std::span<const int> values, bool enabled /* = true */,
                                      std::string_view tr_context /* = TR_CONTEXT */)
{
  static constexpr auto value_to_index = [](s32 value, const std::span<const int> values) {
    for (size_t i = 0; i < values.size(); i++)
    {
      if (values[i] == value)
        return static_cast<int>(i);
    }

    return -1;
  };

  DebugAssert(options.size() == values.size());

  const bool game_settings = IsEditingGameSettings(bsi);

  const std::optional<int> value =
    bsi->GetOptionalIntValue(section, key, game_settings ? std::nullopt : std::optional<int>(default_value));
  const int index = value.has_value() ? value_to_index(value.value(), values) : -1;
  const std::string_view value_text =
    (value.has_value()) ?
      ((index < 0 || static_cast<size_t>(index) >= options.size()) ?
         FSUI_VSTR("Unknown") :
         (translate_options ? Host::TranslateToStringView(tr_context, options[index]) : options[index])) :
      FSUI_VSTR("Use Global Setting");

  if (MenuButtonWithValue(title, summary, value_text, enabled))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(options.size() + 1);
    if (game_settings)
      cd_options.emplace_back(FSUI_STR("Use Global Setting"), !value.has_value());
    for (size_t i = 0; i < options.size(); i++)
    {
      cd_options.emplace_back(translate_options ? Host::TranslateToString(tr_context, options[i]) :
                                                  std::string(options[i]),
                              (i == static_cast<size_t>(index)));
    }
    OpenChoiceDialog(title, false, std::move(cd_options),
                     [game_settings, section = TinyString(section), key = TinyString(key),
                      values](s32 index, const std::string& title, bool checked) {
                       if (index < 0)
                         return;

                       auto lock = Host::GetSettingsLock();
                       SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                       if (game_settings)
                       {
                         if (index == 0)
                           bsi->DeleteValue(section, key);
                         else
                           bsi->SetIntValue(section, key, values[index - 1]);
                       }
                       else
                       {
                         bsi->SetIntValue(section, key, values[index]);
                       }

                       SetSettingsChanged(bsi);
                     });
  }
}

void FullscreenUI::DrawIntRangeSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                       const char* section, const char* key, int default_value, int min_value,
                                       int max_value, const char* format /* = "%d" */, bool enabled /* = true */)
{
  const bool game_settings = IsEditingGameSettings(bsi);
  const std::optional<int> value =
    bsi->GetOptionalIntValue(section, key, game_settings ? std::nullopt : std::optional<int>(default_value));
  const SmallString value_text =
    value.has_value() ? SmallString::from_sprintf(format, value.value()) : SmallString(FSUI_VSTR("Use Global Setting"));

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled))
    OpenFixedPopupDialog(title);

  if (!IsFixedPopupDialogOpen(title) ||
      !BeginFixedPopupDialog(LayoutScale(LAYOUT_SMALL_POPUP_PADDING), LayoutScale(LAYOUT_SMALL_POPUP_PADDING),
                             LayoutScale(600.0f, 0.0f)))
  {
    return;
  }

  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.NormalFontWeight);
  ImGuiFullscreen::TextAlignedMultiLine(0.0f, IMSTR_START_END(summary));
  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight);
  ImGui::Text("%s: ", FSUI_CSTR("Value Range"));
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::Text(format, min_value);
  ImGui::SameLine();
  ImGui::TextUnformatted(" - ");
  ImGui::SameLine();
  ImGui::Text(format, max_value);
  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight);
  ImGui::Text("%s: ", FSUI_CSTR("Default Value"));
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::Text(format, default_value);
  ImGui::PopFont();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));

  BeginMenuButtons();

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, LayoutScale(15.0f));

  ImGui::SetNextItemWidth(ImGui::GetCurrentWindow()->WorkRect.GetWidth());
  s32 dlg_value = static_cast<s32>(value.value_or(default_value));
  if (ImGui::SliderInt("##value", &dlg_value, min_value, max_value, format, ImGuiSliderFlags_NoInput))
  {
    if (IsEditingGameSettings(bsi) && dlg_value == default_value)
      bsi->DeleteValue(section, key);
    else
      bsi->SetIntValue(section, key, dlg_value);

    SetSettingsChanged(bsi);
  }

  ImGui::PopStyleVar(4);

  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
  if (MenuButtonWithoutSummary(FSUI_VSTR("OK"), true, LAYOUT_CENTER_ALIGN_TEXT))
    CloseFixedPopupDialog();
  EndMenuButtons();

  EndFixedPopupDialog();
}

void FullscreenUI::DrawFloatRangeSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                         const char* section, const char* key, float default_value, float min_value,
                                         float max_value, const char* format /* = "%f" */,
                                         float multiplier /* = 1.0f */, bool enabled /* = true */)
{
  const bool game_settings = IsEditingGameSettings(bsi);
  const std::optional<float> value =
    bsi->GetOptionalFloatValue(section, key, game_settings ? std::nullopt : std::optional<float>(default_value));
  const SmallString value_text = value.has_value() ? SmallString::from_sprintf(format, value.value() * multiplier) :
                                                     SmallString(FSUI_VSTR("Use Global Setting"));

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled))
    OpenFixedPopupDialog(title);

  if (!IsFixedPopupDialogOpen(title) ||
      !BeginFixedPopupDialog(LayoutScale(LAYOUT_SMALL_POPUP_PADDING), LayoutScale(LAYOUT_SMALL_POPUP_PADDING),
                             LayoutScale(600.0f, 0.0f)))
  {
    return;
  }

  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.NormalFontWeight);
  ImGuiFullscreen::TextAlignedMultiLine(0.0f, IMSTR_START_END(summary));
  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight);
  ImGui::Text("%s: ", FSUI_CSTR("Value Range"));
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::Text(format, min_value * multiplier);
  ImGui::SameLine();
  ImGui::TextUnformatted(" - ");
  ImGui::SameLine();
  ImGui::Text(format, max_value * multiplier);
  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight);
  ImGui::Text("%s: ", FSUI_CSTR("Default Value"));
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::Text(format, default_value * multiplier);
  ImGui::PopFont();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));

  BeginMenuButtons();

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, LayoutScale(15.0f));

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

  ImGui::PopStyleVar(4);

  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
  if (MenuButtonWithoutSummary(FSUI_VSTR("OK"), true, LAYOUT_CENTER_ALIGN_TEXT))
    CloseFixedPopupDialog();
  EndMenuButtons();

  EndFixedPopupDialog();
}

void FullscreenUI::DrawFloatSpinBoxSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                           const char* section, const char* key, float default_value, float min_value,
                                           float max_value, float step_value, float multiplier,
                                           const char* format /* = "%f" */, bool enabled /* = true */)
{
  const bool game_settings = IsEditingGameSettings(bsi);
  const std::optional<float> value =
    bsi->GetOptionalFloatValue(section, key, game_settings ? std::nullopt : std::optional<float>(default_value));
  const SmallString value_text = value.has_value() ? SmallString::from_sprintf(format, value.value() * multiplier) :
                                                     SmallString(FSUI_VSTR("Use Global Setting"));

  static bool manual_input = false;

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled))
  {
    OpenFixedPopupDialog(title);
    manual_input = false;
  }

  if (!IsFixedPopupDialogOpen(title) ||
      !BeginFixedPopupDialog(LayoutScale(LAYOUT_SMALL_POPUP_PADDING), LayoutScale(LAYOUT_SMALL_POPUP_PADDING),
                             LayoutScale(650.0f, 0.0f)))
  {
    return;
  }

  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.NormalFontWeight);
  ImGuiFullscreen::TextAlignedMultiLine(0.0f, IMSTR_START_END(summary));
  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight);
  ImGui::Text("%s: ", FSUI_CSTR("Value Range"));
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::Text(format, min_value * multiplier);
  ImGui::SameLine();
  ImGui::TextUnformatted(" - ");
  ImGui::SameLine();
  ImGui::Text(format, max_value * multiplier);
  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight);
  ImGui::Text("%s: ", FSUI_CSTR("Default Value"));
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::Text(format, default_value * multiplier);
  ImGui::PopFont();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));

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

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    if (ImGui::InputText("##value", str_value, std::size(str_value), ImGuiInputTextFlags_CharsDecimal))
    {
      dlg_value = StringUtil::FromChars<float>(str_value).value_or(dlg_value);
      dlg_value_changed = true;
    }

    ImGui::PopStyleVar(2);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
  }
  else
  {
    BeginHorizontalMenuButtons(5);
    HorizontalMenuButton(str_value, false);

    float step = 0;
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    if (HorizontalMenuButton(ICON_FA_CHEVRON_UP))
      step = step_value;
    if (HorizontalMenuButton(ICON_FA_CHEVRON_DOWN))
      step = -step_value;
    ImGui::PopItemFlag();
    if (HorizontalMenuButton(ICON_FA_KEYBOARD))
      manual_input = true;
    if (HorizontalMenuButton(ICON_FA_ARROW_ROTATE_LEFT))
    {
      dlg_value = default_value * multiplier;
      dlg_value_changed = true;
    }

    EndHorizontalMenuButtons(10.0f);

    if (step != 0)
    {
      dlg_value += step * multiplier;
      dlg_value_changed = true;
    }
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

  if (MenuButtonWithoutSummary(FSUI_VSTR("OK"), true, LAYOUT_CENTER_ALIGN_TEXT))
    CloseFixedPopupDialog();
  EndMenuButtons();

  EndFixedPopupDialog();
}

bool FullscreenUI::DrawIntRectSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                      const char* section, const char* left_key, int default_left, const char* top_key,
                                      int default_top, const char* right_key, int default_right, const char* bottom_key,
                                      int default_bottom, int min_value, int max_value, const char* format /* = "%d" */,
                                      bool enabled /* = true */)
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
    bottom_value.has_value() ? TinyString::from_sprintf(format, bottom_value.value()) :
                               TinyString(FSUI_VSTR("Default")));

  if (MenuButtonWithValue(title, summary, value_text.c_str(), enabled))
    OpenFixedPopupDialog(title);

  if (!IsFixedPopupDialogOpen(title) ||
      !BeginFixedPopupDialog(LayoutScale(LAYOUT_SMALL_POPUP_PADDING), LayoutScale(LAYOUT_SMALL_POPUP_PADDING),
                             ImVec2(LayoutScale(500.0f), 0.0f)))
  {
    return false;
  }

  s32 dlg_left_value = static_cast<s32>(left_value.value_or(default_left));
  s32 dlg_top_value = static_cast<s32>(top_value.value_or(default_top));
  s32 dlg_right_value = static_cast<s32>(right_value.value_or(default_right));
  s32 dlg_bottom_value = static_cast<s32>(bottom_value.value_or(default_bottom));
  bool changed = false;

  BeginMenuButtons();

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, LayoutScale(15.0f));

  const float midpoint = LayoutScale(150.0f);
  const float end = (ImGui::GetCurrentWindow()->WorkRect.GetWidth() - midpoint) + ImGui::GetStyle().WindowPadding.x;
  ImGui::TextUnformatted(IMSTR_START_END(FSUI_VSTR("Left: ")));
  ImGui::SameLine(midpoint);
  ImGui::SetNextItemWidth(end);
  const bool left_modified =
    ImGui::SliderInt("##left", &dlg_left_value, min_value, max_value, format, ImGuiSliderFlags_NoInput);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
  ImGui::TextUnformatted(IMSTR_START_END(FSUI_VSTR("Top: ")));
  ImGui::SameLine(midpoint);
  ImGui::SetNextItemWidth(end);
  const bool top_modified =
    ImGui::SliderInt("##top", &dlg_top_value, min_value, max_value, format, ImGuiSliderFlags_NoInput);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
  ImGui::TextUnformatted(IMSTR_START_END(FSUI_VSTR("Right: ")));
  ImGui::SameLine(midpoint);
  ImGui::SetNextItemWidth(end);
  const bool right_modified =
    ImGui::SliderInt("##right", &dlg_right_value, min_value, max_value, format, ImGuiSliderFlags_NoInput);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
  ImGui::TextUnformatted(IMSTR_START_END(FSUI_VSTR("Bottom: ")));
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

  changed = (left_modified || top_modified || right_modified || bottom_modified);
  if (changed)
    SetSettingsChanged(bsi);

  ImGui::PopStyleVar(4);

  if (MenuButtonWithoutSummary(FSUI_VSTR("OK"), true, LAYOUT_CENTER_ALIGN_TEXT))
    CloseFixedPopupDialog();
  EndMenuButtons();

  EndFixedPopupDialog();

  return changed;
}

void FullscreenUI::DrawIntSpinBoxSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                         const char* section, const char* key, int default_value, int min_value,
                                         int max_value, int step_value, const char* format /* = "%d" */,
                                         bool enabled /* = true */)
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

  if (MenuButtonWithValue(title, summary, value_text, enabled))
  {
    OpenFixedPopupDialog(title);
    manual_input = false;
  }

  if (!IsFixedPopupDialogOpen(title) ||
      !BeginFixedPopupDialog(LayoutScale(LAYOUT_SMALL_POPUP_PADDING), LayoutScale(LAYOUT_SMALL_POPUP_PADDING),
                             LayoutScale(650.0f, 0.0f)))
  {
    return;
  }

  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.NormalFontWeight);
  ImGuiFullscreen::TextAlignedMultiLine(0.0f, IMSTR_START_END(summary));
  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight);
  ImGui::Text("%s: ", FSUI_CSTR("Value Range"));
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::Text(format, min_value);
  ImGui::SameLine();
  ImGui::TextUnformatted(" - ");
  ImGui::SameLine();
  ImGui::Text(format, max_value);
  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight);
  ImGui::Text("%s: ", FSUI_CSTR("Default Value"));
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::Text(format, default_value);
  ImGui::PopFont();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));

  BeginMenuButtons();

  s32 dlg_value = static_cast<s32>(value.value_or(default_value));
  bool dlg_value_changed = false;

  char str_value[32];
  std::snprintf(str_value, std::size(str_value), format, dlg_value);

  if (manual_input)
  {
    const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    ImGui::SetNextItemWidth(end);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    std::snprintf(str_value, std::size(str_value), "%d", dlg_value);
    if (ImGui::InputText("##value", str_value, std::size(str_value), ImGuiInputTextFlags_CharsDecimal))
    {
      dlg_value = StringUtil::FromChars<s32>(str_value).value_or(dlg_value);
      dlg_value_changed = true;
    }

    ImGui::PopStyleVar(2);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
  }
  else
  {
    BeginHorizontalMenuButtons(5);
    HorizontalMenuButton(str_value, false);

    s32 step = 0;
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    if (HorizontalMenuButton(ICON_FA_CHEVRON_UP))
      step = step_value;
    if (HorizontalMenuButton(ICON_FA_CHEVRON_DOWN))
      step = -step_value;
    ImGui::PopItemFlag();
    if (HorizontalMenuButton(ICON_FA_KEYBOARD))
      manual_input = true;
    if (HorizontalMenuButton(ICON_FA_ARROW_ROTATE_LEFT))
    {
      dlg_value = default_value;
      dlg_value_changed = true;
    }

    EndHorizontalMenuButtons(10.0f);

    if (step != 0)
    {
      dlg_value += step;
      dlg_value_changed = true;
    }
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

  if (MenuButtonWithoutSummary(FSUI_VSTR("OK"), true, LAYOUT_CENTER_ALIGN_TEXT))
    CloseFixedPopupDialog();
  EndMenuButtons();

  EndFixedPopupDialog();
}

[[maybe_unused]] void FullscreenUI::DrawStringListSetting(
  SettingsInterface* bsi, std::string_view title, std::string_view summary, const char* section, const char* key,
  const char* default_value, std::span<const char* const> options, std::span<const char* const> option_values,
  bool enabled /* = true */, void (*changed_callback)(std::string_view) /* = nullptr */,
  std::string_view tr_context /* = TR_CONTEXT */)
{
  const bool game_settings = IsEditingGameSettings(bsi);
  const std::optional<SmallString> value(bsi->GetOptionalSmallStringValue(
    section, key, game_settings ? std::nullopt : std::optional<const char*>(default_value)));

  DebugAssert(options.size() == option_values.size());

  size_t index = options.size();
  if (value.has_value())
  {
    for (size_t i = 0; i < options.size(); i++)
    {
      if (value == option_values[i])
      {
        index = i;
        break;
      }
    }
  }

  if (MenuButtonWithValue(title, summary,
                          value.has_value() ? ((index < options.size()) ? TRANSLATE_SV(tr_context, options[index]) :
                                                                          FSUI_VSTR("Unknown")) :
                                              FSUI_VSTR("Use Global Setting"),
                          enabled))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(options.size() + 1);
    if (game_settings)
      cd_options.emplace_back(FSUI_STR("Use Global Setting"), !value.has_value());
    for (size_t i = 0; i < options.size(); i++)
    {
      cd_options.emplace_back(TRANSLATE_STR(tr_context, options[i]),
                              (value.has_value() && i == static_cast<size_t>(index)));
    }
    OpenChoiceDialog(title, false, std::move(cd_options),
                     [game_settings, section, key, default_value, option_values,
                      changed_callback](s32 index, const std::string& title, bool checked) {
                       if (index < 0)
                         return;

                       auto lock = Host::GetSettingsLock();
                       SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                       if (game_settings)
                       {
                         if (index == 0)
                           bsi->DeleteValue(section, key);
                         else
                           bsi->SetStringValue(section, key, option_values[index - 1]);

                         if (changed_callback)
                           changed_callback(Host::GetStringSettingValue(section, key, default_value));
                       }
                       else
                       {
                         bsi->SetStringValue(section, key, option_values[index]);

                         if (changed_callback)
                           changed_callback(option_values[index]);
                       }

                       SetSettingsChanged(bsi);
                     });
  }
}

template<typename DataType, typename SizeType>
void FullscreenUI::DrawEnumSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                   const char* section, const char* key, DataType default_value,
                                   std::optional<DataType> (*from_string_function)(const char* str),
                                   const char* (*to_string_function)(DataType value),
                                   const char* (*to_display_string_function)(DataType value), SizeType option_count,
                                   bool enabled /* = true */)
{
  const bool game_settings = IsEditingGameSettings(bsi);
  const std::optional<SmallString> value(bsi->GetOptionalSmallStringValue(
    section, key, game_settings ? std::nullopt : std::optional<const char*>(to_string_function(default_value))));

  const std::optional<DataType> typed_value(value.has_value() ? from_string_function(value->c_str()) : std::nullopt);

  if (MenuButtonWithValue(title, summary,
                          typed_value.has_value() ? to_display_string_function(typed_value.value()) :
                                                    FSUI_CSTR("Use Global Setting"),
                          enabled))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(static_cast<u32>(option_count) + 1);
    if (game_settings)
      cd_options.emplace_back(FSUI_STR("Use Global Setting"), !value.has_value());
    for (u32 i = 0; i < static_cast<u32>(option_count); i++)
      cd_options.emplace_back(to_display_string_function(static_cast<DataType>(i)),
                              (typed_value.has_value() && i == static_cast<u32>(typed_value.value())));
    OpenChoiceDialog(title, false, std::move(cd_options),
                     [section = TinyString(section), key = TinyString(key), to_string_function,
                      game_settings](s32 index, const std::string& title, bool checked) {
                       if (index < 0)
                         return;

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
                     });
  }
}
void FullscreenUI::DrawFloatListSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                        const char* section, const char* key, float default_value,
                                        const char* const* options, const float* option_values, size_t option_count,
                                        bool translate_options, bool enabled /* = true */)
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
             (translate_options ? Host::TranslateToStringView(FSUI_TR_CONTEXT, options[index]) : options[index]) :
             FSUI_VSTR("Unknown")) :
          FSUI_VSTR("Use Global Setting"),
        enabled))
  {
    ImGuiFullscreen::ChoiceDialogOptions cd_options;
    cd_options.reserve(option_count + 1);
    if (game_settings)
      cd_options.emplace_back(FSUI_STR("Use Global Setting"), !value.has_value());
    for (size_t i = 0; i < option_count; i++)
    {
      cd_options.emplace_back(translate_options ? Host::TranslateToString(FSUI_TR_CONTEXT, options[i]) :
                                                  std::string(options[i]),
                              (value.has_value() && i == static_cast<size_t>(index)));
    }
    OpenChoiceDialog(title, false, std::move(cd_options),
                     [game_settings, section = TinyString(section), key = TinyString(key),
                      option_values](s32 index, const std::string& title, bool checked) {
                       if (index < 0)
                         return;

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
                     });
  }
}

void FullscreenUI::DrawFolderSetting(SettingsInterface* bsi, std::string_view title, const char* section,
                                     const char* key, const std::string& runtime_var)
{
  if (MenuButton(title, runtime_var))
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
                     });
  }
}

void FullscreenUI::StartAutomaticBindingForPort(u32 port)
{
  InputManager::DeviceList devices = InputManager::EnumerateDevices();
  if (devices.empty())
  {
    ShowToast({}, FSUI_STR("Automatic mapping failed, no devices are available."));
    return;
  }

  std::vector<std::string> names;
  ImGuiFullscreen::ChoiceDialogOptions options;
  options.reserve(devices.size());
  names.reserve(devices.size());
  for (auto& [key, name, display_name] : devices)
  {
    names.push_back(std::move(name));
    options.emplace_back(std::move(display_name), false);
  }
  OpenChoiceDialog(FSUI_STR("Select Device"), false, std::move(options),
                   [port, names = std::move(names)](s32 index, const std::string& title, bool checked) {
                     if (index < 0)
                       return;

                     const std::string& name = names[index];
                     auto lock = Host::GetSettingsLock();
                     SettingsInterface* bsi = GetEditingSettingsInterface();
                     const bool result =
                       InputManager::MapController(*bsi, port, InputManager::GetGenericBindingMapping(name), true);
                     SetSettingsChanged(bsi);

                     // and the toast needs to happen on the UI thread.
                     ShowToast({}, result ? fmt::format(FSUI_FSTR("Automatic mapping completed for {}."), name) :
                                            fmt::format(FSUI_FSTR("Automatic mapping failed for {}."), name));
                   });
}

void FullscreenUI::StartClearBindingsForPort(u32 port)
{
  ImGuiFullscreen::OpenConfirmMessageDialog(
    FSUI_STR("Clear Mappings"),
    FSUI_STR("Are you sure you want to clear all mappings for this controller?\n\nYou cannot undo this action."),
    [port](bool result) {
      if (!result)
        return;

      auto lock = Host::GetSettingsLock();
      SettingsInterface* bsi = GetEditingSettingsInterface();
      InputManager::ClearPortBindings(*bsi, port);
      ShowToast({}, FSUI_STR("Controller mapping cleared."));
    });
}

void FullscreenUI::InitializeHotkeyList()
{
  // sort hotkeys by category so we don't duplicate the groups
  const auto hotkeys = InputManager::GetHotkeyList();
  s_state.hotkey_list_cache.reserve(hotkeys.size());

  // this mess is needed to preserve the category order
  for (size_t i = 0; i < hotkeys.size(); i++)
  {
    const HotkeyInfo* hk = hotkeys[i];
    size_t j;
    for (j = 0; j < s_state.hotkey_list_cache.size(); j++)
    {
      if (std::strcmp(hk->category, s_state.hotkey_list_cache[j]->category) == 0)
        break;
    }
    if (j != s_state.hotkey_list_cache.size())
    {
      // already done
      continue;
    }

    // add all hotkeys with this category
    for (const HotkeyInfo* other_hk : hotkeys)
    {
      if (std::strcmp(hk->category, other_hk->category) != 0)
        continue;

      s_state.hotkey_list_cache.push_back(other_hk);
    }
  }
}

void FullscreenUI::SwitchToSettings()
{
  s_state.game_settings_entry.reset();
  s_state.game_settings_interface.reset();
  s_state.game_settings_serial = {};
  s_state.game_settings_hash = 0;
  s_state.game_settings_db_entry = nullptr;
  s_state.game_patch_list = {};
  s_state.enabled_game_patch_cache = {};
  s_state.game_cheats_list = {};
  s_state.enabled_game_cheat_cache = {};
  s_state.game_cheat_groups = {};

  PopulateGraphicsAdapterList();
  PopulatePostProcessingChain(GetEditingSettingsInterface(), PostProcessing::Config::DISPLAY_CHAIN_SECTION);

  if (!IsEditingGameSettings(GetEditingSettingsInterface()))
  {
    auto lock = Host::GetSettingsLock();
    PopulateGameListDirectoryCache(Host::Internal::GetBaseSettingsLayer());
  }

  SwitchToMainWindow(MainWindowType::Settings);
  s_state.settings_page = SettingsPage::Interface;
  s_state.settings_last_bg_alpha = GetBackgroundAlpha();
}

void FullscreenUI::SwitchToGameSettingsForSerial(std::string_view serial, GameHash hash)
{
  s_state.game_settings_serial = serial;
  s_state.game_settings_hash = hash;
  s_state.game_settings_entry.reset();
  s_state.game_settings_db_entry = GameDatabase::GetEntryForSerial(serial);
  s_state.game_settings_interface =
    System::GetGameSettingsInterface(s_state.game_settings_db_entry, serial, true, false);
  PopulatePatchesAndCheatsList();
  s_state.settings_page = SettingsPage::Summary;
  SwitchToMainWindow(MainWindowType::Settings);
}

bool FullscreenUI::SwitchToGameSettings()
{
  const std::string& serial = GPUThread::GetGameSerial();
  if (serial.empty())
    return false;

  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(GPUThread::GetGamePath());
  if (!entry)
  {
    SwitchToGameSettingsForSerial(serial, GPUThread::GetGameHash());
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
  SwitchToGameSettingsForSerial(entry->serial, entry->hash);
  s_state.game_settings_entry = std::make_unique<GameList::Entry>(*entry);
}

void FullscreenUI::PopulateGraphicsAdapterList()
{
  auto lock = Host::GetSettingsLock();
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

void FullscreenUI::PopulatePatchesAndCheatsList()
{
  s_state.game_patch_list =
    Cheats::GetCodeInfoList(s_state.game_settings_serial, s_state.game_settings_hash, false, true, true);
  s_state.game_cheats_list =
    Cheats::GetCodeInfoList(s_state.game_settings_serial, s_state.game_settings_hash, true,
                            s_state.game_settings_interface->GetBoolValue("Cheats", "LoadCheatsFromDatabase", true),
                            s_state.game_settings_interface->GetBoolValue("Cheats", "SortList", false));
  s_state.game_cheat_groups = Cheats::GetCodeListUniquePrefixes(s_state.game_cheats_list, true);
  s_state.enabled_game_patch_cache =
    s_state.game_settings_interface->GetStringList(Cheats::PATCHES_CONFIG_SECTION, Cheats::PATCH_ENABLE_CONFIG_KEY);
  s_state.enabled_game_cheat_cache =
    s_state.game_settings_interface->GetStringList(Cheats::CHEATS_CONFIG_SECTION, Cheats::PATCH_ENABLE_CONFIG_KEY);
}

void FullscreenUI::BeginResetSettings()
{
  OpenConfirmMessageDialog(FSUI_STR("Restore Defaults"),
                           FSUI_STR("Are you sure you want to restore the default settings? Any preferences will be "
                                    "lost.\n\nYou cannot undo this action."),
                           [](bool result) {
                             if (!result)
                               return;

                             Host::RequestResetSettings(true, false);
                             ShowToast(std::string(), FSUI_STR("Settings reset to default."));
                           });
}

void FullscreenUI::DoCopyGameSettings()
{
  if (!s_state.game_settings_interface)
    return;

  Settings temp_settings;
  temp_settings.Load(*GetEditingSettingsInterface(false), *GetEditingSettingsInterface(false));
  temp_settings.Save(*s_state.game_settings_interface, true);
  SetSettingsChanged(s_state.game_settings_interface.get());

  ShowToast(std::string(), fmt::format(FSUI_FSTR("Game settings initialized with global settings for '{}'."),
                                       Path::GetFileTitle(s_state.game_settings_interface->GetPath())));
}

void FullscreenUI::DoClearGameSettings()
{
  if (!s_state.game_settings_interface)
    return;

  s_state.game_settings_interface->Clear();
  if (!s_state.game_settings_interface->GetPath().empty())
    FileSystem::DeleteFile(s_state.game_settings_interface->GetPath().c_str());

  SetSettingsChanged(s_state.game_settings_interface.get());

  ShowToast(std::string(), fmt::format(FSUI_FSTR("Game settings have been cleared for '{}'."),
                                       Path::GetFileTitle(s_state.game_settings_interface->GetPath())));
}

void FullscreenUI::DrawSettingsWindow()
{
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, UIStyle.LargeFontSize + (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));

  const float target_bg_alpha = GetBackgroundAlpha();
  s_state.settings_last_bg_alpha = (target_bg_alpha < s_state.settings_last_bg_alpha) ?
                                     std::max(s_state.settings_last_bg_alpha - io.DeltaTime * 2.0f, target_bg_alpha) :
                                     std::min(s_state.settings_last_bg_alpha + io.DeltaTime * 2.0f, target_bg_alpha);

  if (BeginFullscreenWindow(
        ImVec2(0.0f, 0.0f), heading_size, "settings_category",
        ImVec4(UIStyle.PrimaryColor.x, UIStyle.PrimaryColor.y, UIStyle.PrimaryColor.z, s_state.settings_last_bg_alpha)))
  {
    static constexpr const SettingsPage global_pages[] = {
      SettingsPage::Interface,  SettingsPage::GameList, SettingsPage::Console,        SettingsPage::Emulation,
      SettingsPage::BIOS,       SettingsPage::Graphics, SettingsPage::PostProcessing, SettingsPage::Audio,
      SettingsPage::Controller, SettingsPage::Hotkey,   SettingsPage::MemoryCards,    SettingsPage::Achievements,
      SettingsPage::Advanced};
    static constexpr const SettingsPage per_game_pages[] = {
      SettingsPage::Summary,     SettingsPage::Console,      SettingsPage::Emulation, SettingsPage::Patches,
      SettingsPage::Cheats,      SettingsPage::Graphics,     SettingsPage::Audio,     SettingsPage::Controller,
      SettingsPage::MemoryCards, SettingsPage::Achievements, SettingsPage::Advanced};
    static constexpr std::array<std::pair<const char*, const char*>, static_cast<u32>(SettingsPage::Count)> titles = {
      {{FSUI_NSTR("Summary"), ICON_FA_FILE},
       {FSUI_NSTR("Interface Settings"), ICON_FA_TV},
       {FSUI_NSTR("Game List Settings"), ICON_FA_LIST_UL},
       {FSUI_NSTR("Console Settings"), ICON_FA_DICE_D20},
       {FSUI_NSTR("Emulation Settings"), ICON_FA_GEAR},
       {FSUI_NSTR("BIOS Settings"), ICON_PF_MICROCHIP},
       {FSUI_NSTR("Controller Settings"), ICON_PF_GAMEPAD_ALT},
       {FSUI_NSTR("Hotkey Settings"), ICON_PF_KEYBOARD_ALT},
       {FSUI_NSTR("Memory Card Settings"), ICON_PF_MEMORY_CARD},
       {FSUI_NSTR("Graphics Settings"), ICON_PF_PICTURE},
       {FSUI_NSTR("Post-Processing Settings"), ICON_FA_WAND_MAGIC_SPARKLES},
       {FSUI_NSTR("Audio Settings"), ICON_PF_SOUND},
       {FSUI_NSTR("Achievements Settings"), ICON_FA_TROPHY},
       {FSUI_NSTR("Advanced Settings"), ICON_FA_TRIANGLE_EXCLAMATION},
       {FSUI_NSTR("Patches"), ICON_PF_SPARKLING},
       {FSUI_NSTR("Cheats"), ICON_PF_CHEATS}}};

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
        BeginTransition([page = pages[(index == 0) ? (count - 1) : (index - 1)]]() {
          s_state.settings_page = page;
          QueueResetFocus(FocusResetType::Other);
        });
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) ||
               ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakFast, true) ||
               ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
      {
        BeginTransition([page = pages[(index + 1) % count]]() {
          s_state.settings_page = page;
          QueueResetFocus(FocusResetType::Other);
        });
      }
    }

    if (NavButton(ICON_PF_NAVIGATION_BACK, true, true))
      ReturnToPreviousWindow();

    NavTitle(s_state.game_settings_entry ?
               s_state.game_settings_entry->GetDisplayTitle(s_state.show_localized_titles) :
               Host::TranslateToStringView(FSUI_TR_CONTEXT, titles[static_cast<u32>(pages[index])].first));

    RightAlignNavButtons(count);

    for (u32 i = 0; i < count; i++)
    {
      if (NavButton(titles[static_cast<u32>(pages[i])].second, i == index, true))
      {
        BeginTransition([page = pages[i]]() {
          s_state.settings_page = page;
          QueueResetFocus(FocusResetType::Other);
        });
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
        ImVec4(UIStyle.BackgroundColor.x, UIStyle.BackgroundColor.y, UIStyle.BackgroundColor.z,
               s_state.settings_last_bg_alpha),
        0.0f, ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, 0.0f)))
  {
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
        DrawAchievementsSettingsPage(lock);
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
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Back"))},
                            GetBackgroundAlpha());
  }
  else
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT, FSUI_VSTR("Change Page")),
                                       std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, FSUI_VSTR("Navigate")),
                                       std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_ESC, FSUI_VSTR("Back"))},
                            GetBackgroundAlpha());
  }
}

void FullscreenUI::DrawSummarySettingsPage()
{
  BeginMenuButtons();
  ResetFocusHere();

  MenuHeading(FSUI_VSTR("Details"));

  if (s_state.game_settings_entry)
  {
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_WINDOW_MAXIMIZE, "Title"), s_state.game_settings_entry->title.c_str(), true))
      CopyTextToClipboard(FSUI_STR("Game title copied to clipboard."), s_state.game_settings_entry->title);
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_PAGER, "Serial"), s_state.game_settings_entry->serial.c_str(), true))
      CopyTextToClipboard(FSUI_STR("Game serial copied to clipboard."), s_state.game_settings_entry->serial);
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_COMPACT_DISC, "Type"),
                   GameList::GetEntryTypeDisplayName(s_state.game_settings_entry->type), true))
    {
      CopyTextToClipboard(FSUI_STR("Game type copied to clipboard."),
                          GameList::GetEntryTypeDisplayName(s_state.game_settings_entry->type));
    }
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_GLOBE, "Region"),
                   Settings::GetDiscRegionDisplayName(s_state.game_settings_entry->region), true))
    {
      CopyTextToClipboard(FSUI_STR("Game region copied to clipboard."),
                          Settings::GetDiscRegionDisplayName(s_state.game_settings_entry->region));
    }
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_STAR, "Compatibility Rating"),
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
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_FILE, "Path"), s_state.game_settings_entry->path.c_str(), true))
    {
      CopyTextToClipboard(FSUI_STR("Game path copied to clipboard."), s_state.game_settings_entry->path);
    }
  }
  else
  {
    MenuButton(FSUI_ICONVSTR(ICON_FA_BAN, "Details unavailable for game not scanned in game list."), "");
  }

  MenuHeading(FSUI_VSTR("Options"));

  if (s_state.game_settings_db_entry && s_state.game_settings_db_entry->disc_set)
  {
    // only enable for first disc
    const bool is_first_disc =
      (s_state.game_settings_db_entry->serial == s_state.game_settings_db_entry->disc_set->serials.front());
    DrawToggleSetting(
      GetEditingSettingsInterface(), FSUI_ICONVSTR(ICON_FA_COMPACT_DISC, "Use Separate Disc Settings"),
      FSUI_VSTR(
        "Uses separate game settings for each disc of multi-disc games. Can only be set on the first/main disc."),
      "Main", "UseSeparateConfigForDiscSet", !is_first_disc, is_first_disc, false);
  }

  if (MenuButton(FSUI_ICONVSTR(ICON_FA_COPY, "Copy Settings"),
                 FSUI_VSTR("Copies the current global settings to this game.")))
  {
    DoCopyGameSettings();
  }
  if (MenuButton(FSUI_ICONVSTR(ICON_FA_TRASH, "Clear Settings"), FSUI_VSTR("Clears all settings set for this game.")))
  {
    DoClearGameSettings();
  }

  EndMenuButtons();
}

void FullscreenUI::DrawInterfaceSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();
  ResetFocusHere();

  MenuHeading(FSUI_VSTR("Appearance"));

  {
    // Have to do this the annoying way, because it's host-derived.
    const auto language_list = Host::GetAvailableLanguageList();
    TinyString current_language = bsi->GetTinyStringValue("Main", "Language", "");
    if (MenuButtonWithValue(FSUI_ICONVSTR(ICON_FA_LANGUAGE, "Language"),
                            FSUI_VSTR("Chooses the language used for UI elements."),
                            Host::GetLanguageName(current_language)))
    {
      ImGuiFullscreen::ChoiceDialogOptions options;
      for (const auto& [language, code] : language_list)
        options.emplace_back(Host::GetLanguageName(code), (current_language == code));
      OpenChoiceDialog(FSUI_ICONVSTR(ICON_FA_LANGUAGE, "UI Language"), false, std::move(options),
                       [language_list](s32 index, const std::string& title, bool checked) {
                         if (static_cast<u32>(index) >= language_list.size())
                           return;

                         Host::RunOnCPUThread(
                           [language = language_list[index].second]() { Host::ChangeLanguage(language); });
                       });
    }
  }

  DrawStringListSetting(bsi, FSUI_ICONVSTR(ICON_FA_PAINTBRUSH, "Theme"),
                        FSUI_VSTR("Selects the color style to be used for Big Picture UI."), "UI", "FullscreenUITheme",
                        "Dark", s_theme_names, s_theme_values, true,
                        [](std::string_view) { BeginTransition(LONG_TRANSITION_TIME, &FullscreenUI::SetTheme); });

  if (const TinyString current_value =
        bsi->GetTinyStringValue("Main", "FullscreenUIBackground", DEFAULT_BACKGROUND_NAME);
      MenuButtonWithValue(FSUI_ICONVSTR(ICON_FA_IMAGE, "Menu Background"),
                          FSUI_VSTR("Shows a background image or shader when a game isn't running. Backgrounds are "
                                    "located in resources/fullscreenui/backgrounds in the data directory."),
                          current_value.c_str()))
  {
    ChoiceDialogOptions options = GetBackgroundOptions(current_value);
    OpenChoiceDialog(FSUI_ICONVSTR(ICON_FA_IMAGE, "Menu Background"), false, std::move(options),
                     [](s32 index, const std::string& title, bool checked) {
                       if (index < 0)
                         return;

                       SettingsInterface* bsi = GetEditingSettingsInterface();
                       bsi->SetStringValue("Main", "FullscreenUIBackground", (index == 0) ? "None" : title.c_str());
                       SetSettingsChanged(bsi);

                       // Have to defer the reload, because we've already drawn the bg for this frame.
                       BeginTransition(LONG_TRANSITION_TIME, {});
                       Host::RunOnCPUThread([]() { GPUThread::RunOnThread(&FullscreenUI::LoadBackground); });
                     });
  }

  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_LIST, "Open To Game List"),
    FSUI_VSTR("When Big Picture mode is started, the game list will be displayed instead of the main menu."), "Main",
    "FullscreenUIOpenToGameList", false);

  if (DrawToggleSetting(
        bsi, FSUI_ICONVSTR(ICON_PF_GAMEPAD, "Use DualShock/DualSense Button Icons"),
        FSUI_VSTR(
          "Displays DualShock/DualSense button icons in the footer and input binding, instead of Xbox buttons."),
        "Main", "FullscreenUIDisplayPSIcons", false))
  {
    if (bsi->GetBoolValue("Main", "FullscreenUIDisplayPSIcons", false))
      ImGuiFullscreen::SetFullscreenFooterTextIconMapping(s_ps_button_mapping);
    else
      ImGuiFullscreen::SetFullscreenFooterTextIconMapping({});
  }

  if (DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_WINDOW_RESTORE, "Window Animations"),
                        FSUI_VSTR("Animates windows opening/closing and changes between views in the Big Picture UI."),
                        "Main", "FullscreenUIAnimations", true))
  {
    ImGuiFullscreen::SetAnimations(bsi->GetBoolValue("Main", "FullscreenUIAnimations", true));
  }

  if (DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_LIST, "Smooth Scrolling"),
                        FSUI_VSTR("Enables smooth scrolling of menus in the Big Picture UI."), "Main",
                        "FullscreenUISmoothScrolling", true))
  {
    ImGuiFullscreen::SetSmoothScrolling(bsi->GetBoolValue("Main", "FullscreenUISmoothScrolling", true));
  }

  if (DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_BORDER_ALL, "Menu Borders"),
                        FSUI_VSTR("Draws a border around the currently-selected item for readability."), "Main",
                        "FullscreenUIMenuBorders", false))
  {
    ImGuiFullscreen::SetMenuBorders(bsi->GetBoolValue("Main", "FullscreenUIMenuBorders", false));
  }

  MenuHeading(FSUI_VSTR("Behavior"));

  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_POWER_OFF, "Confirm Power Off"),
    FSUI_VSTR("Determines whether a prompt will be displayed to confirm shutting down the emulator/game "
              "when the hotkey is pressed."),
    "Main", "ConfirmPowerOff", true);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_FLOPPY_DISK, "Save State On Shutdown"),
                    FSUI_VSTR("Automatically saves the emulator state when powering down or exiting. You can then "
                              "resume directly from where you left off next time."),
                    "Main", "SaveStateOnExit", true);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_WAND_MAGIC_SPARKLES, "Inhibit Screensaver"),
    FSUI_VSTR("Prevents the screen saver from activating and the host from sleeping while emulation is running."),
    "Main", "InhibitScreensaver", true);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_PAUSE, "Pause On Start"),
                    FSUI_VSTR("Pauses the emulator when a game is started."), "Main", "StartPaused", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_EYE_LOW_VISION, "Pause On Focus Loss"),
                    FSUI_VSTR("Pauses the emulator when you minimize the window or switch to another "
                              "application, and unpauses when you switch back."),
                    "Main", "PauseOnFocusLoss", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_GAMEPAD, "Pause On Controller Disconnection"),
                    FSUI_VSTR("Pauses the emulator when a controller with bindings is disconnected."), "Main",
                    "PauseOnControllerDisconnection", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_FILE_EXPORT, "Create Save State Backups"),
                    FSUI_VSTR("Renames existing save states when saving to a backup file."), "Main",
                    "CreateSaveStateBackups", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_CIRCLE_USER, "Enable Discord Presence"),
                    FSUI_VSTR("Shows the game you are currently playing as part of your profile in Discord."), "Main",
                    "EnableDiscordPresence", false);

  MenuHeading(FSUI_VSTR("Game Display"));

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_PF_FULLSCREEN, "Start Fullscreen"),
                    FSUI_VSTR("Automatically switches to fullscreen mode when the program is started."), "Main",
                    "StartFullscreen", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_WINDOW_MAXIMIZE, "Double-Click Toggles Fullscreen"),
                    FSUI_VSTR("Switches between full screen and windowed when the window is double-clicked."), "Main",
                    "DoubleClickTogglesFullscreen", true);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_ARROW_POINTER, "Hide Cursor In Fullscreen"),
                    FSUI_VSTR("Hides the mouse pointer/cursor when the emulator is in fullscreen mode."), "Main",
                    "HideCursorInFullscreen", true);

  MenuHeading(FSUI_VSTR("On-Screen Display"));
  DrawIntSpinBoxSetting(bsi, FSUI_ICONVSTR(ICON_FA_MAGNIFYING_GLASS, "OSD Scale"),
                        FSUI_VSTR("Determines how large the on-screen messages and monitor are."), "Display",
                        "OSDScale", 100, 25, 500, 1, "%d%%");
  DrawFloatSpinBoxSetting(bsi, FSUI_ICONVSTR(ICON_FA_RULER, "Screen Margins"),
                          FSUI_VSTR("Determines the margin between the edge of the screen and on-screen messages."),
                          "Display", "OSDMargin", ImGuiManager::DEFAULT_SCREEN_MARGIN, 0.0f, 100.0f, 1.0f, 1.0f,
                          "%.0fpx");
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_CIRCLE_EXCLAMATION, "Show OSD Messages"),
                    FSUI_VSTR("Shows on-screen-display messages when events occur."), "Display", "ShowOSDMessages",
                    true);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_PLAY, "Show Status Indicators"),
                    FSUI_VSTR("Shows persistent icons when turbo is active or when paused."), "Display",
                    "ShowStatusIndicators", true);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_GAUGE_HIGH, "Show Speed"),
    FSUI_VSTR(
      "Shows the current emulation speed of the system in the top-right corner of the display as a percentage."),
    "Display", "ShowSpeed", false);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_STOPWATCH, "Show FPS"),
    FSUI_VSTR("Shows the number of frames (or v-syncs) displayed per second by the system in the top-right "
              "corner of the display."),
    "Display", "ShowFPS", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_CHART_BAR, "Show GPU Statistics"),
                    FSUI_VSTR("Shows information about the emulated GPU in the top-right corner of the display."),
                    "Display", "ShowGPUStatistics", false);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_USER_CLOCK, "Show Latency Statistics"),
    FSUI_VSTR("Shows information about input and audio latency in the top-right corner of the display."), "Display",
    "ShowLatencyStatistics", false);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_PF_CPU_PROCESSOR, "Show CPU Usage"),
    FSUI_VSTR("Shows the host's CPU usage of each system thread in the top-right corner of the display."), "Display",
    "ShowCPU", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_PF_GPU_GRAPHICS_CARD, "Show GPU Usage"),
                    FSUI_VSTR("Shows the host's GPU usage in the top-right corner of the display."), "Display",
                    "ShowGPU", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_RULER_HORIZONTAL, "Show Frame Times"),
                    FSUI_VSTR("Shows a visual history of frame times in the upper-left corner of the display."),
                    "Display", "ShowFrameTimes", false);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_EXPAND, "Show Resolution"),
    FSUI_VSTR("Shows the current rendering resolution of the system in the top-right corner of the display."),
    "Display", "ShowResolution", false);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_GAMEPAD, "Show Controller Input"),
    FSUI_VSTR("Shows the current controller state of the system in the bottom-left corner of the display."), "Display",
    "ShowInputs", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_CHART_LINE, "Show Enhancement Settings"),
                    FSUI_VSTR("Shows enhancement settings in the bottom-right corner of the screen."), "Display",
                    "ShowEnhancements", false);

  MenuHeading(FSUI_VSTR("Operations"));
  {
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_DUMPSTER_FIRE, "Restore Defaults"),
                   FSUI_VSTR("Resets all settings to the defaults.")))
    {
      BeginResetSettings();
    }
  }

  EndMenuButtons();
}

void FullscreenUI::DrawBIOSSettingsPage()
{
  static constexpr const std::array config_keys = {"", "PathNTSCJ", "PathNTSCU", "PathPAL"};

  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  BeginMenuButtons();
  ResetFocusHere();

  MenuHeading(FSUI_VSTR("BIOS Selection"));

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
                            filename.has_value() ? (filename->empty() ? FSUI_VSTR("Auto-Detect") : filename->view()) :
                                                   FSUI_VSTR("Use Global Setting")))
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

      OpenChoiceDialog(
        title, false, std::move(options), [game_settings, i](s32 index, const std::string& path, bool checked) {
          if (index < 0)
            return;

          auto lock = Host::GetSettingsLock();
          SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
          if (game_settings && index == 0)
          {
            bsi->DeleteValue("BIOS", config_keys[i]);
          }
          else
          {
            bsi->SetStringValue("BIOS", config_keys[i],
                                (index == static_cast<s32>(BoolToUInt32(game_settings))) ? "" : path.c_str());
          }
          SetSettingsChanged(bsi);
        });
    }
  }

  MenuHeading(FSUI_VSTR("Options"));

  DrawFolderSetting(bsi, FSUI_ICONVSTR(ICON_FA_FOLDER, "BIOS Directory"), "BIOS", "SearchDirectory", EmuFolders::Bios);

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_SCROLL, "Enable TTY Logging"),
                    FSUI_VSTR("Logs BIOS calls to printf(). Not all games contain debugging messages."), "BIOS",
                    "TTYLogging", false);

  EndMenuButtons();
}

void FullscreenUI::DrawConsoleSettingsPage()
{
  static constexpr const std::array cdrom_read_speeds = {
    FSUI_NSTR("None (Double Speed)"), FSUI_NSTR("2x (Quad Speed)"), FSUI_NSTR("3x (6x Speed)"),
    FSUI_NSTR("4x (8x Speed)"),       FSUI_NSTR("5x (10x Speed)"),  FSUI_NSTR("6x (12x Speed)"),
    FSUI_NSTR("7x (14x Speed)"),      FSUI_NSTR("8x (16x Speed)"),  FSUI_NSTR("9x (18x Speed)"),
    FSUI_NSTR("10x (20x Speed)"),     FSUI_NSTR("Maximum"),
  };

  static constexpr const std::array cdrom_seek_speeds = {
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
    FSUI_NSTR("Maximum"),
  };

  static constexpr std::array cdrom_read_seek_speed_values = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0};

  SettingsInterface* const bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  BeginMenuButtons();
  ResetFocusHere();

  MenuHeading(FSUI_VSTR("Console Settings"));

  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_GLOBE, "Region"), FSUI_VSTR("Determines the emulated hardware type."),
                  "Console", "Region", Settings::DEFAULT_CONSOLE_REGION, &Settings::ParseConsoleRegionName,
                  &Settings::GetConsoleRegionName, &Settings::GetConsoleRegionDisplayName, ConsoleRegion::Count);
  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_STOPWATCH, "Frame Rate"),
                  FSUI_VSTR("Utilizes the chosen frame rate regardless of the game's setting."), "GPU",
                  "ForceVideoTiming", Settings::DEFAULT_FORCE_VIDEO_TIMING_MODE, &Settings::ParseForceVideoTimingName,
                  &Settings::GetForceVideoTimingName, &Settings::GetForceVideoTimingDisplayName,
                  ForceVideoTimingMode::Count);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_SHIELD_HALVED, "Safe Mode"),
                    FSUI_VSTR("Temporarily disables all enhancements, useful when testing."), "Main",
                    "DisableAllEnhancements", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_BOLT, "Enable Fast Boot"),
                    FSUI_VSTR("Patches the BIOS to skip the boot animation. Safe to enable."), "BIOS", "PatchFastBoot",
                    Settings::DEFAULT_FAST_BOOT_VALUE);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_PF_FAST_FORWARD, "Fast Forward Boot"),
                    FSUI_VSTR("Fast forwards through the early loading process when fast booting, saving time. Results "
                              "may vary between games."),
                    "BIOS", "FastForwardBoot", false,
                    GetEffectiveBoolSetting(bsi, "BIOS", "PatchFastBoot", Settings::DEFAULT_FAST_BOOT_VALUE));
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_PF_MEMORY_CARD, "Fast Forward Memory Card Access"),
                    FSUI_VSTR("Fast forwards through memory card access, both loading and saving. Can reduce waiting "
                              "times in games that frequently access memory cards."),
                    "MemoryCards", "FastForwardAccess", false);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_MEMORY, "Enable 8MB RAM"),
    FSUI_VSTR("Enables an additional 6MB of RAM to obtain a total of 2+6 = 8MB, usually present on dev consoles."),
    "Console", "Enable8MBRAM", false);

  MenuHeading(FSUI_VSTR("CPU Emulation"));

  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_BOLT, "Execution Mode"),
                  FSUI_VSTR("Determines how the emulated CPU executes instructions."), "CPU", "ExecutionMode",
                  Settings::DEFAULT_CPU_EXECUTION_MODE, &Settings::ParseCPUExecutionMode,
                  &Settings::GetCPUExecutionModeName, &Settings::GetCPUExecutionModeDisplayName,
                  CPUExecutionMode::Count);

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_GAUGE_SIMPLE_HIGH, "Enable Overclocking"),
                    FSUI_VSTR("When this option is chosen, the clock speed set below will be used."), "CPU",
                    "OverclockEnable", false);

  const bool oc_enable = GetEffectiveBoolSetting(bsi, "CPU", "OverclockEnable", false);
  if (oc_enable)
  {
    u32 oc_numerator = GetEffectiveUIntSetting(bsi, "CPU", "OverclockNumerator", 1);
    u32 oc_denominator = GetEffectiveUIntSetting(bsi, "CPU", "OverclockDenominator", 1);
    s32 oc_percent = static_cast<s32>(Settings::CPUOverclockFractionToPercent(oc_numerator, oc_denominator));
    if (RangeButton(FSUI_ICONVSTR(ICON_FA_GAUGE_SIMPLE_HIGH, "Overclocking Percentage"),
                    FSUI_VSTR("Selects the percentage of the normal clock speed the emulated hardware will run at."),
                    &oc_percent, 10, 1000, 10, "%d%%"))
    {
      Settings::CPUOverclockPercentToFraction(oc_percent, &oc_numerator, &oc_denominator);
      bsi->SetUIntValue("CPU", "OverclockNumerator", oc_numerator);
      bsi->SetUIntValue("CPU", "OverclockDenominator", oc_denominator);
      SetSettingsChanged(bsi);
    }
  }

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_MICROCHIP, "Enable Recompiler ICache"),
                    FSUI_VSTR("Makes games run closer to their console framerate, at a small cost to performance."),
                    "CPU", "RecompilerICache", false);

  MenuHeading(FSUI_VSTR("CD-ROM Emulation"));

  DrawIntListSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_COMPACT_DISC, "Read Speedup"),
    FSUI_VSTR(
      "Speeds up CD-ROM reads by the specified factor. May improve loading speeds in some games, and break others."),
    "CDROM", "ReadSpeedup", 1, cdrom_read_speeds, true, cdrom_read_seek_speed_values);
  DrawIntListSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_MAGNIFYING_GLASS, "Seek Speedup"),
    FSUI_VSTR(
      "Speeds up CD-ROM seeks by the specified factor. May improve loading speeds in some games, and break others."),
    "CDROM", "SeekSpeedup", 1, cdrom_seek_speeds, true, cdrom_read_seek_speed_values);

  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_DOWNLOAD, "Preload Images to RAM"),
    FSUI_VSTR("Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay."),
    "CDROM", "LoadImageToRAM", false);
  if (!game_settings)
  {
    DrawToggleSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_VEST_PATCHES, "Apply Image Patches"),
      FSUI_VSTR("Automatically applies patches to disc images when they are present, currently only PPF is supported."),
      "CDROM", "LoadImagePatches", false);
    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_BAN, "Ignore Drive Subcode"),
                      FSUI_VSTR("Ignores the subchannel provided by the drive when using physical discs, instead "
                                "always generating subchannel data. Can improve read reliability on some drives."),
                      "CDROM", "IgnoreHostSubcode", false);
  }
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_LIST_OL, "Switch to Next Disc on Stop"),
                    FSUI_VSTR("Automatically switches to the next disc in the game when the game stops the CD-ROM "
                              "motor. Does not work for all games."),
                    "CDROM", "AutoDiscChange", false);

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
  ResetFocusHere();

  MenuHeading(FSUI_VSTR("Speed Control"));
  DrawFloatListSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_STOPWATCH, "Emulation Speed"),
    FSUI_VSTR("Sets the target emulation speed. It is not guaranteed that this speed will be reached on all systems."),
    "Main", "EmulationSpeed", 1.0f, emulation_speed_titles.data(), emulation_speed_values.data(),
    emulation_speed_titles.size(), true);
  DrawFloatListSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_BOLT, "Fast Forward Speed"),
    FSUI_VSTR("Sets the fast forward speed. It is not guaranteed that this speed will be reached on all systems."),
    "Main", "FastForwardSpeed", 0.0f, emulation_speed_titles.data(), emulation_speed_values.data(),
    emulation_speed_titles.size(), true);
  DrawFloatListSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_BOLT, "Turbo Speed"),
    FSUI_VSTR("Sets the turbo speed. It is not guaranteed that this speed will be reached on all systems."), "Main",
    "TurboSpeed", 2.0f, emulation_speed_titles.data(), emulation_speed_values.data(), emulation_speed_titles.size(),
    true);

  MenuHeading(FSUI_VSTR("Latency Control"));
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_TV, "Vertical Sync (VSync)"),
                    FSUI_VSTR("Synchronizes presentation of the console's frames to the host. GSync/FreeSync users "
                              "should enable Optimal Frame Pacing instead."),
                    "Display", "VSync", false);

  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_LIGHTBULB, "Sync To Host Refresh Rate"),
    FSUI_VSTR("Adjusts the emulation speed so the console's refresh rate matches the host when VSync is enabled."),
    "Main", "SyncToHostRefreshRate", false);

  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_GAUGE_SIMPLE_HIGH, "Optimal Frame Pacing"),
    FSUI_VSTR("Ensures every frame generated is displayed for optimal pacing. Enable for variable refresh displays, "
              "such as GSync/FreeSync. Disable if you are having speed or sound issues."),
    "Display", "OptimalFramePacing", false);

  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_CHARGING_STATION, "Skip Duplicate Frame Display"),
    FSUI_VSTR("Skips the presentation/display of frames that are not unique. Can result in worse frame pacing."),
    "Display", "SkipPresentingDuplicateFrames", false);

  const bool optimal_frame_pacing_active = GetEffectiveBoolSetting(bsi, "Display", "OptimalFramePacing", false);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_STOPWATCH_20, "Reduce Input Latency"),
    FSUI_VSTR("Reduces input latency by delaying the start of frame until closer to the presentation time."), "Display",
    "PreFrameSleep", false, optimal_frame_pacing_active);

  const bool pre_frame_sleep_active =
    (optimal_frame_pacing_active && GetEffectiveBoolSetting(bsi, "Display", "PreFrameSleep", false));
  if (pre_frame_sleep_active)
  {
    DrawFloatRangeSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_HOURGLASS, "Frame Time Buffer"),
      FSUI_VSTR("Specifies the amount of buffer time added, which reduces the additional sleep time introduced."),
      "Display", "PreFrameSleepBuffer", Settings::DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER, 0.0f, 20.0f,
      FSUI_CSTR("%.1f ms"), 1.0f, pre_frame_sleep_active);
  }

  MenuHeading(FSUI_VSTR("Runahead/Rewind"));

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_BACKWARD, "Enable Rewinding"),
                    FSUI_VSTR("Saves state periodically so you can rewind any mistakes while playing."), "Main",
                    "RewindEnable", false);

  const s32 runahead_frames = GetEffectiveIntSetting(bsi, "Main", "RunaheadFrameCount", 0);
  const bool runahead_enabled = (runahead_frames > 0);
  const bool rewind_enabled = GetEffectiveBoolSetting(bsi, "Main", "RewindEnable", false);

  DrawFloatRangeSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_FLOPPY_DISK, "Rewind Save Frequency"),
    FSUI_VSTR("How often a rewind state will be created. Higher frequencies have greater system requirements."), "Main",
    "RewindFrequency", 10.0f, 0.0f, 3600.0f, FSUI_CSTR("%.2f Seconds"), 1.0f, rewind_enabled);
  DrawIntRangeSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_WHISKEY_GLASS, "Rewind Save Slots"),
    FSUI_VSTR("How many saves will be kept for rewinding. Higher values have greater memory requirements."), "Main",
    "RewindSaveSlots", 10, 1, 10000, FSUI_CSTR("%d Frames"), rewind_enabled);

  static constexpr const std::array runahead_options = {
    FSUI_NSTR("Disabled"), FSUI_NSTR("1 Frame"),  FSUI_NSTR("2 Frames"), FSUI_NSTR("3 Frames"),
    FSUI_NSTR("4 Frames"), FSUI_NSTR("5 Frames"), FSUI_NSTR("6 Frames"), FSUI_NSTR("7 Frames"),
    FSUI_NSTR("8 Frames"), FSUI_NSTR("9 Frames"), FSUI_NSTR("10 Frames")};

  DrawIntListSetting(bsi, FSUI_ICONVSTR(ICON_FA_PERSON_RUNNING, "Runahead"),
                     FSUI_VSTR("Simulates the system ahead of time and rolls back/replays to reduce input lag. Very "
                               "high system requirements."),
                     "Main", "RunaheadFrameCount", 0, runahead_options);

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

  MenuButtonWithoutSummary(rewind_summary, false);

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
  OpenChoiceDialog(FSUI_ICONVSTR(ICON_FA_FOLDER_OPEN, "Load Preset"), false, std::move(coptions),
                   [](s32 index, const std::string& title, bool checked) {
                     if (index < 0)
                       return;

                     INISettingsInterface ssi(System::GetInputProfilePath(title));
                     if (!ssi.Load())
                     {
                       ShowToast(std::string(), fmt::format(FSUI_FSTR("Failed to load '{}'."), title));
                       return;
                     }

                     auto lock = Host::GetSettingsLock();
                     SettingsInterface* dsi = GetEditingSettingsInterface();
                     InputManager::CopyConfiguration(dsi, ssi, true, true, true, IsEditingGameSettings(dsi));
                     SetSettingsChanged(dsi);
                     ShowToast(std::string(), fmt::format(FSUI_FSTR("Controller preset '{}' loaded."), title));
                   });
}

void FullscreenUI::DoSaveInputProfile(const std::string& name)
{
  INISettingsInterface dsi(System::GetInputProfilePath(name));

  auto lock = Host::GetSettingsLock();
  SettingsInterface* ssi = GetEditingSettingsInterface();
  InputManager::CopyConfiguration(&dsi, *ssi, true, true, true, IsEditingGameSettings(ssi));
  if (dsi.Save())
    ShowToast(std::string(), fmt::format(FSUI_FSTR("Controller preset '{}' saved."), name));
  else
    ShowToast(std::string(), fmt::format(FSUI_FSTR("Failed to save controller preset '{}'."), name));
}

void FullscreenUI::DoSaveNewInputProfile()
{
  OpenInputStringDialog(FSUI_ICONSTR(ICON_FA_FLOPPY_DISK, "Save Controller Preset"),
                        FSUI_STR("Enter the name of the controller preset you wish to create."), std::string(),
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
  OpenChoiceDialog(FSUI_ICONVSTR(ICON_FA_FLOPPY_DISK, "Save Preset"), false, std::move(coptions),
                   [](s32 index, const std::string& title, bool checked) {
                     if (index < 0)
                       return;

                     if (index > 0)
                       DoSaveInputProfile(title);
                     else
                       DoSaveNewInputProfile();
                   });
}

void FullscreenUI::BeginResetControllerSettings()
{
  OpenConfirmMessageDialog(FSUI_STR("Reset Controller Settings"),
                           FSUI_STR("Are you sure you want to restore the default controller configuration?\n\nAll "
                                    "bindings and configuration will be lost. You cannot undo this action."),
                           [](bool result) {
                             if (!result)
                               return;

                             Host::RequestResetSettings(false, true);
                             ShowToast(std::string(), FSUI_STR("Controller settings reset to default."));
                           });
}

void FullscreenUI::DrawControllerSettingsPage()
{
  BeginMenuButtons();

  SettingsInterface* bsi = GetEditingSettingsInterface();
  const bool game_settings = IsEditingGameSettings(bsi);

  MenuHeading(FSUI_VSTR("Configuration"));
  ResetFocusHere();

  if (IsEditingGameSettings(bsi))
  {
    if (DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_GEARS, "Per-Game Configuration"),
                          FSUI_VSTR("Uses game-specific settings for controllers for this game."), "ControllerPorts",
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
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_COPY, "Copy Global Settings"),
                   FSUI_VSTR("Copies the global controller configuration to this game.")))
    {
      CopyGlobalControllerSettingsToGame();
    }
  }
  else
  {
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_DUMPSTER_FIRE, "Reset Settings"),
                   FSUI_VSTR("Resets all configuration to defaults (including bindings).")))
    {
      BeginResetControllerSettings();
    }
  }

  if (MenuButton(FSUI_ICONVSTR(ICON_FA_FOLDER_OPEN, "Load Preset"),
                 FSUI_VSTR("Replaces these settings with a previously saved controller preset.")))
  {
    DoLoadInputProfile();
  }
  if (MenuButton(FSUI_ICONVSTR(ICON_FA_FLOPPY_DISK, "Save Preset"),
                 FSUI_VSTR("Stores the current settings to a controller preset.")))
  {
    DoSaveInputProfile();
  }

  MenuHeading(FSUI_VSTR("Input Sources"));

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_GEAR, "Enable SDL Input Source"),
                    FSUI_VSTR("The SDL input source supports most controllers."), "InputSources", "SDL", true, true,
                    false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_WIFI, "SDL DualShock 4 / DualSense Enhanced Mode"),
                    FSUI_VSTR("Provides vibration and LED control support over Bluetooth."), "InputSources",
                    "SDLControllerEnhancedMode", false, bsi->GetBoolValue("InputSources", "SDL", true), false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_LIGHTBULB, "SDL DualSense Player LED"),
                    FSUI_VSTR("Enable/Disable the Player LED on DualSense controllers."), "InputSources",
                    "SDLPS5PlayerLED", false, bsi->GetBoolValue("InputSources", "SDLControllerEnhancedMode", true),
                    false);
#ifdef _WIN32
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_GEAR, "Enable XInput Input Source"),
                    FSUI_VSTR("Support for controllers that use the XInput protocol. XInput should only be used if you "
                              "are using a XInput wrapper library."),
                    "InputSources", "XInput", false);
#endif

  MenuHeading(FSUI_VSTR("Multitap"));
  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_SQUARE_PLUS, "Multitap Mode"),
                  FSUI_VSTR("Enables an additional three controller slots on each port. Not supported in all games."),
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

    MenuHeading(TinyString::from_format(fmt::runtime(FSUI_ICONVSTR(ICON_FA_PLUG, "Controller Port {}")),
                                        Controller::GetPortDisplayName(mtap_port, mtap_slot, mtap_enabled[mtap_port])));

    ImGui::PushID(TinyString::from_format("port_{}", global_slot));

    const TinyString section = TinyString::from_format("Pad{}", global_slot + 1);
    const TinyString type = bsi->GetTinyStringValue(
      section.c_str(), "Type", Controller::GetControllerInfo(Settings::GetDefaultControllerType(global_slot)).name);
    const Controller::ControllerInfo* ci = Controller::GetControllerInfo(type);
    TinyString value;
    if (ci && ci->icon_name)
      value.format("{} {}", ci->icon_name, ci->GetDisplayName());
    else if (ci)
      value = ci->GetDisplayName();
    else
      value = FSUI_VSTR("Unknown");

    if (MenuButtonWithValue(
          TinyString::from_format("{}##type{}", FSUI_ICONVSTR(ICON_FA_GAMEPAD, "Controller Type"), global_slot),
          FSUI_VSTR("Selects the type of emulated controller for this port."), value))
    {
      const auto& infos = Controller::GetControllerInfoList();
      ImGuiFullscreen::ChoiceDialogOptions options;
      options.reserve(infos.size());
      for (const Controller::ControllerInfo* it : infos)
      {
        if (it->icon_name)
          options.emplace_back(fmt::format("{} {}", it->icon_name, it->GetDisplayName()), type == it->name);
        else
          options.emplace_back(it->GetDisplayName(), type == it->name);
      }

      OpenChoiceDialog(TinyString::from_format(FSUI_FSTR("Port {} Controller Type"), global_slot + 1), false,
                       std::move(options),
                       [game_settings, section, infos](s32 index, const std::string& title, bool checked) {
                         if (index < 0)
                           return;

                         auto lock = Host::GetSettingsLock();
                         SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                         bsi->SetStringValue(section.c_str(), "Type", infos[index]->name);
                         SetSettingsChanged(bsi);
                       });
    }

    if (!ci || ci->bindings.empty())
    {
      ImGui::PopID();
      continue;
    }

    if (MenuButton(FSUI_ICONVSTR(ICON_FA_WAND_MAGIC_SPARKLES, "Automatic Mapping"),
                   FSUI_VSTR("Attempts to map the selected port to a chosen controller.")))
    {
      StartAutomaticBindingForPort(global_slot);
    }

    if (MenuButton(FSUI_ICONVSTR(ICON_FA_TRASH, "Clear Mappings"),
                   FSUI_VSTR("Removes all bindings for this controller port.")))
    {
      StartClearBindingsForPort(global_slot);
    }

    MenuHeading(
      SmallString::from_format(fmt::runtime(FSUI_ICONVSTR(ICON_FA_MICROCHIP, "Controller Port {} Bindings")),
                               Controller::GetPortDisplayName(mtap_port, mtap_slot, mtap_enabled[mtap_port])));

    for (const Controller::ControllerBindingInfo& bi : ci->bindings)
    {
      DrawInputBindingButton(bsi, bi.type, section.c_str(), bi.name, ci->GetBindingDisplayName(bi),
                             bi.icon_name ? std::string_view(bi.icon_name) : std::string_view(), true);
    }

    MenuHeading(
      SmallString::from_format(fmt::runtime(FSUI_ICONVSTR(ICON_FA_MICROCHIP, "Controller Port {} Macros")),
                               Controller::GetPortDisplayName(mtap_port, mtap_slot, mtap_enabled[mtap_port])));

    for (u32 macro_index = 0; macro_index < InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER; macro_index++)
    {
      ImGui::PushID(TinyString::from_format("macro_{}", macro_index));

      bool& expanded = s_state.controller_macro_expanded[global_slot][macro_index];
      expanded ^= MenuHeadingButton(
        SmallString::from_format(fmt::runtime(FSUI_ICONVSTR(ICON_PF_EMPTY_KEYCAP, "Macro Button {}")), macro_index + 1),
        s_state.controller_macro_expanded[global_slot][macro_index] ? ICON_FA_CHEVRON_UP : ICON_FA_CHEVRON_DOWN);
      if (!expanded)
      {
        ImGui::PopID();
        continue;
      }

      DrawInputBindingButton(bsi, InputBindingInfo::Type::Macro, section.c_str(),
                             TinyString::from_format("Macro{}", macro_index + 1), FSUI_CSTR("Trigger"),
                             std::string_view(), true);

      SmallString binds_string =
        bsi->GetSmallStringValue(section.c_str(), TinyString::from_format("Macro{}Binds", macro_index + 1).c_str());
      TinyString pretty_binds_string;
      if (!binds_string.empty())
      {
        for (const std::string_view& bind : StringUtil::SplitString(binds_string, '&', true))
        {
          std::string_view dispname;
          for (const Controller::ControllerBindingInfo& bi : ci->bindings)
          {
            if (bind == bi.name)
            {
              dispname = bi.icon_name ? std::string_view(bi.icon_name) : ci->GetBindingDisplayName(bi);
              break;
            }
          }
          pretty_binds_string.append_format("{}{}", pretty_binds_string.empty() ? "" : " ", dispname);
        }
      }
      if (MenuButtonWithValue(FSUI_ICONVSTR(ICON_FA_KEYBOARD, "Buttons"), std::string_view(),
                              pretty_binds_string.empty() ? FSUI_VSTR("-") : pretty_binds_string.view()))
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

      DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_GAMEPAD, "Press To Toggle"),
                        FSUI_VSTR("Toggles the macro when the button is pressed, instead of held."), section.c_str(),
                        TinyString::from_format("Macro{}Toggle", macro_index + 1), false, true, false);

      const TinyString freq_key = TinyString::from_format("Macro{}Frequency", macro_index + 1);
      const TinyString freq_label =
        TinyString::from_format(ICON_FA_CLOCK " {}##macro_{}_frequency", FSUI_VSTR("Frequency"), macro_index + 1);
      s32 frequency = bsi->GetIntValue(section.c_str(), freq_key.c_str(), 0);
      const TinyString freq_summary = ((frequency == 0) ? TinyString(FSUI_VSTR("Disabled")) :
                                                          TinyString::from_format(FSUI_FSTR("{} Frames"), frequency));
      if (MenuButtonWithValue(
            freq_label,
            FSUI_VSTR(
              "Determines the frequency at which the macro will toggle the buttons on and off (aka auto fire)."),
            freq_summary, true))
      {
        OpenFixedPopupDialog(freq_label);
      }

      DrawFloatSpinBoxSetting(bsi, FSUI_ICONVSTR(ICON_FA_ARROW_DOWN, "Pressure"),
                              FSUI_VSTR("Determines how much pressure is simulated when macro is active."), section,
                              TinyString::from_format("Macro{}Pressure", macro_index + 1), 1.0f, 0.01f, 1.0f, 0.01f,
                              100.0f, "%.0f%%");

      DrawFloatSpinBoxSetting(bsi, FSUI_ICONVSTR(ICON_FA_SKULL, "Deadzone"),
                              FSUI_VSTR("Determines how much button pressure is ignored before activating the macro."),
                              section, TinyString::from_format("Macro{}Deadzone", macro_index + 1).c_str(), 0.0f, 0.00f,
                              1.0f, 0.01f, 100.0f, "%.0f%%");

      if (IsFixedPopupDialogOpen(freq_label) &&
          BeginFixedPopupDialog(LayoutScale(LAYOUT_SMALL_POPUP_PADDING), LayoutScale(LAYOUT_SMALL_POPUP_PADDING),
                                LayoutScale(500.0f, 200.0f)))
      {
        BeginMenuButtons();

        ImGui::SetNextItemWidth(ImGui::GetCurrentWindow()->WorkRect.GetWidth());
        if (ImGui::SliderInt("##value", &frequency, 0, 60,
                             (frequency == 0) ? FSUI_CSTR("Disabled") : FSUI_CSTR("Toggle every %d frames"),
                             ImGuiSliderFlags_NoInput))
        {
          if (frequency == 0)
            bsi->DeleteValue(section.c_str(), freq_key.c_str());
          else
            bsi->SetIntValue(section.c_str(), freq_key.c_str(), frequency);
        }

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
        if (MenuButtonWithoutSummary(FSUI_VSTR("OK"), true, LAYOUT_CENTER_ALIGN_TEXT))
          CloseFixedPopupDialog();

        EndMenuButtons();

        EndFixedPopupDialog();
      }

      ImGui::PopID();
    }

    if (!ci->settings.empty())
    {
      MenuHeading(
        SmallString::from_format(fmt::runtime(FSUI_ICONVSTR(ICON_FA_SLIDERS, "Controller Port {} Settings")),
                                 Controller::GetPortDisplayName(mtap_port, mtap_slot, mtap_enabled[mtap_port])));

      for (const SettingInfo& si : ci->settings)
      {
        TinyString title;
        title.format(ICON_FA_GEAR "{}", Host::TranslateToStringView(ci->name, si.display_name));
        std::string_view description = Host::TranslateToStringView(ci->name, si.description);
        switch (si.type)
        {
          case SettingInfo::Type::Boolean:
          {
            DrawToggleSetting(bsi, title, description, section.c_str(), si.name, si.BooleanDefaultValue(), true, false);
          }
          break;

          case SettingInfo::Type::Integer:
          {
            DrawIntRangeSetting(bsi, title, description, section.c_str(), si.name, si.IntegerDefaultValue(),
                                si.IntegerMinValue(), si.IntegerMaxValue(), si.format, true);
          }
          break;

          case SettingInfo::Type::IntegerList:
          {
            size_t option_count = 0;
            if (si.options)
            {
              while (si.options[option_count])
                option_count++;
            }

            DrawIntListSetting(bsi, title, description, section.c_str(), si.name, si.IntegerDefaultValue(),
                               std::span<const char* const>(si.options, option_count), true, si.IntegerMinValue(), true,
                               ci->name);
          }
          break;

          case SettingInfo::Type::Float:
          {
            DrawFloatSpinBoxSetting(bsi, title, description, section.c_str(), si.name, si.FloatDefaultValue(),
                                    si.FloatMinValue(), si.FloatMaxValue(), si.FloatStepValue(), si.multiplier,
                                    si.format, true);
          }
          break;

          default:
            break;
        }
      }
    }

    ImGui::PopID();
  }

  EndMenuButtons();
}

void FullscreenUI::DrawHotkeySettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();
  ResetFocusHere();

  const HotkeyInfo* last_category = nullptr;
  for (const HotkeyInfo* hotkey : s_state.hotkey_list_cache)
  {
    if (!last_category || std::strcmp(hotkey->category, last_category->category) != 0)
    {
      MenuHeading(Host::TranslateToStringView("Hotkeys", hotkey->category));
      last_category = hotkey;
    }

    DrawInputBindingButton(bsi, InputBindingInfo::Type::Button, "Hotkeys", hotkey->name,
                           Host::TranslateToStringView("Hotkeys", hotkey->display_name), std::string_view(), false);
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

  MenuHeading(FSUI_VSTR("Settings and Operations"));
  ResetFocusHere();

  DrawFolderSetting(bsi, FSUI_ICONVSTR(ICON_FA_FOLDER_OPEN, "Memory Card Directory"), "MemoryCards", "Directory",
                    EmuFolders::MemoryCards);

  if (!game_settings && MenuButton(FSUI_ICONVSTR(ICON_FA_ARROW_ROTATE_LEFT, "Reset Memory Card Directory"),
                                   FSUI_VSTR("Resets memory card directory to default (user directory).")))
  {
    bsi->SetStringValue("MemoryCards", "Directory", "memcards");
    SetSettingsChanged(bsi);
  }

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_SHARE_NODES, "Use Single Card For Multi-Disc Games"),
                    FSUI_VSTR("When playing a multi-disc game and using per-game (title) memory cards, "
                              "use a single memory card for all discs."),
                    "MemoryCards", "UsePlaylistTitle", true);

  for (u32 i = 0; i < 2; i++)
  {
    MenuHeading(TinyString::from_format(FSUI_FSTR("Memory Card Port {}"), i + 1));

    const MemoryCardType default_type =
      (i == 0) ? Settings::DEFAULT_MEMORY_CARD_1_TYPE : Settings::DEFAULT_MEMORY_CARD_2_TYPE;
    DrawEnumSetting(
      bsi, TinyString::from_format(fmt::runtime(FSUI_ICONVSTR(ICON_PF_MEMORY_CARD, "Memory Card {} Type")), i + 1),
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
    title.format("{}##card_name_{}", FSUI_ICONVSTR(ICON_FA_FILE, "Shared Card Name"), i);
    if (MenuButtonWithValue(title,
                            FSUI_VSTR("The selected memory card image will be used in shared mode for this slot."),
                            path_value.has_value() ? path_value->view() : FSUI_VSTR("Use Global Setting"), is_shared))
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

  MenuHeading(FSUI_VSTR("Device Settings"));

  ResetFocusHere();
  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_PF_PICTURE, "GPU Renderer"),
                  FSUI_VSTR("Selects the backend to use for rendering the console/game visuals."), "GPU", "Renderer",
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
        FSUI_ICONVSTR(ICON_PF_GPU_GRAPHICS_CARD, "GPU Adapter"), FSUI_VSTR("Selects the GPU to use for rendering."),
        current_adapter.has_value() ? (current_adapter->empty() ? FSUI_VSTR("Default") : current_adapter->view()) :
                                      FSUI_VSTR("Use Global Setting")))
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
    };
    OpenChoiceDialog(FSUI_ICONVSTR(ICON_PF_GPU_GRAPHICS_CARD, "GPU Adapter"), false, std::move(options),
                     std::move(callback));
  }

  const bool pgxp_enabled = (is_hardware && GetEffectiveBoolSetting(bsi, "GPU", "PGXPEnable", false));
  const bool texture_correction_enabled =
    (pgxp_enabled && GetEffectiveBoolSetting(bsi, "GPU", "PGXPTextureCorrection", true));

  MenuHeading(FSUI_VSTR("Rendering"));

  if (is_hardware)
  {
    DrawIntListSetting(bsi, FSUI_ICONVSTR(ICON_FA_EXPAND, "Internal Resolution"),
                       FSUI_VSTR("Upscales the game's rendering by the specified multiplier."), "GPU",
                       "ResolutionScale", 1, resolution_scales);

    DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_COMPRESS, "Downsampling"),
                    FSUI_VSTR("Downsamples the rendered image prior to displaying it. Can improve "
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
      DrawIntRangeSetting(bsi, FSUI_ICONVSTR(ICON_FA_COMPRESS, "Downsampling Display Scale"),
                          FSUI_VSTR("Selects the resolution scale that will be applied to the final image. 1x will "
                                    "downsample to the original console resolution."),
                          "GPU", "DownsampleScale", 1, 1, GPU::MAX_RESOLUTION_SCALE, "%dx");
    }

    DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_TABLE_CELLS, "Texture Filtering"),
                    FSUI_VSTR("Smooths out the blockiness of magnified textures on 3D objects."), "GPU",
                    "TextureFilter", Settings::DEFAULT_GPU_TEXTURE_FILTER, &Settings::ParseTextureFilterName,
                    &Settings::GetTextureFilterName, &Settings::GetTextureFilterDisplayName, GPUTextureFilter::Count);

    DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_SQUARE_ARROW_UP_RIGHT, "Sprite Texture Filtering"),
                    FSUI_VSTR("Smooths out the blockiness of magnified textures on 2D objects."), "GPU",
                    "SpriteTextureFilter", Settings::DEFAULT_GPU_TEXTURE_FILTER, &Settings::ParseTextureFilterName,
                    &Settings::GetTextureFilterName, &Settings::GetTextureFilterDisplayName, GPUTextureFilter::Count);

    DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_DROPLET_SLASH, "Dithering"),
                    FSUI_VSTR("Controls how dithering is applied in the emulated GPU. True Color disables dithering "
                              "and produces the nicest looking gradients."),
                    "GPU", "DitheringMode", Settings::DEFAULT_GPU_DITHERING_MODE, &Settings::ParseGPUDitheringModeName,
                    &Settings::GetGPUDitheringModeName, &Settings::GetGPUDitheringModeDisplayName,
                    GPUDitheringMode::MaxCount);
  }

  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_SHAPES, "Aspect Ratio"),
                  FSUI_VSTR("Changes the aspect ratio used to display the console's output to the screen."), "Display",
                  "AspectRatio", Settings::DEFAULT_DISPLAY_ASPECT_RATIO, &Settings::ParseDisplayAspectRatio,
                  &Settings::GetDisplayAspectRatioName, &Settings::GetDisplayAspectRatioDisplayName,
                  DisplayAspectRatio::Count);

  DrawEnumSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_GRIP_LINES, "Deinterlacing Mode"),
    FSUI_VSTR(
      "Determines which algorithm is used to convert interlaced frames to progressive for display on your system."),
    "GPU", "DeinterlacingMode", Settings::DEFAULT_DISPLAY_DEINTERLACING_MODE, &Settings::ParseDisplayDeinterlacingMode,
    &Settings::GetDisplayDeinterlacingModeName, &Settings::GetDisplayDeinterlacingModeDisplayName,
    DisplayDeinterlacingMode::Count);

  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_CROP, "Crop Mode"),
                  FSUI_VSTR("Determines how much of the area typically not visible on a consumer TV set to crop/hide."),
                  "Display", "CropMode", Settings::DEFAULT_DISPLAY_CROP_MODE, &Settings::ParseDisplayCropMode,
                  &Settings::GetDisplayCropModeName, &Settings::GetDisplayCropModeDisplayName,
                  DisplayCropMode::MaxCount);

  DrawEnumSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_EXPAND, "Scaling"),
    FSUI_VSTR("Determines how the emulated console's output is upscaled or downscaled to your monitor's resolution."),
    "Display", "Scaling", Settings::DEFAULT_DISPLAY_SCALING, &Settings::ParseDisplayScaling,
    &Settings::GetDisplayScalingName, &Settings::GetDisplayScalingDisplayName, DisplayScalingMode::Count);

  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_VIDEO, "FMV Scaling"),
                  FSUI_VSTR("Determines the scaling algorithm used when 24-bit content is active, typically FMVs."),
                  "Display", "Scaling24Bit", Settings::DEFAULT_DISPLAY_SCALING, &Settings::ParseDisplayScaling,
                  &Settings::GetDisplayScalingName, &Settings::GetDisplayScalingDisplayName, DisplayScalingMode::Count);

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_ARROWS_LEFT_RIGHT_TO_LINE, "Widescreen Rendering"),
                    FSUI_VSTR("Increases the field of view from 4:3 to the chosen display aspect ratio in 3D games."),
                    "GPU", "WidescreenHack", false);

  if (is_hardware)
  {
    DrawToggleSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_BEZIER_CURVE, "PGXP Geometry Correction"),
      FSUI_VSTR("Reduces \"wobbly\" polygons by attempting to preserve the fractional component through memory "
                "transfers."),
      "GPU", "PGXPEnable", false);

    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_SITEMAP, "PGXP Depth Buffer"),
                      FSUI_VSTR("Reduces polygon Z-fighting through depth testing. Low compatibility with games."),
                      "GPU", "PGXPDepthBuffer", false, pgxp_enabled && texture_correction_enabled);
  }

  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_COMPRESS, "Force 4:3 For FMVs"),
    FSUI_VSTR("Switches back to 4:3 display aspect ratio when displaying 24-bit content, usually FMVs."), "Display",
    "Force4_3For24Bit", false);

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_BRUSH, "FMV Chroma Smoothing"),
                    FSUI_VSTR("Smooths out blockyness between colour transitions in 24-bit content, usually FMVs."),
                    "GPU", "ChromaSmoothing24Bit", false);

  MenuHeading(FSUI_VSTR("Advanced"));

  std::optional<SmallString> strvalue = bsi->GetOptionalSmallStringValue(
    "GPU", "FullscreenMode", game_settings ? std::nullopt : std::optional<const char*>(""));

  if (MenuButtonWithValue(FSUI_ICONVSTR(ICON_FA_TV, "Fullscreen Resolution"),
                          FSUI_VSTR("Selects the resolution to use in fullscreen modes."),
                          strvalue.has_value() ?
                            (strvalue->empty() ? FSUI_VSTR("Borderless Fullscreen") : strvalue->view()) :
                            FSUI_VSTR("Use Global Setting")))
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
    };
    OpenChoiceDialog(FSUI_ICONVSTR(ICON_FA_TV, "Fullscreen Resolution"), false, std::move(options),
                     std::move(callback));
  }

  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "Screen Position"),
                  FSUI_VSTR("Determines the position on the screen when black borders must be added."), "Display",
                  "Alignment", Settings::DEFAULT_DISPLAY_ALIGNMENT, &Settings::ParseDisplayAlignment,
                  &Settings::GetDisplayAlignmentName, &Settings::GetDisplayAlignmentDisplayName,
                  DisplayAlignment::Count);

  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_ARROWS_SPIN, "Screen Rotation"),
                  FSUI_VSTR("Determines the rotation of the simulated TV screen."), "Display", "Rotation",
                  Settings::DEFAULT_DISPLAY_ROTATION, &Settings::ParseDisplayRotation,
                  &Settings::GetDisplayRotationName, &Settings::GetDisplayRotationDisplayName, DisplayRotation::Count);

  if (is_hardware)
  {
    DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_GRIP_LINES_VERTICAL, "Line Detection"),
                    FSUI_VSTR("Attempts to detect one pixel high/wide lines that rely on non-upscaled rasterization "
                              "behavior, filling in gaps introduced by upscaling."),
                    "GPU", "LineDetectMode", Settings::DEFAULT_GPU_LINE_DETECT_MODE, &Settings::ParseLineDetectModeName,
                    &Settings::GetLineDetectModeName, &Settings::GetLineDetectModeDisplayName, GPULineDetectMode::Count,
                    resolution_scale > 1);

    DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_BOX, "Wireframe Rendering"),
                    FSUI_VSTR("Overlays or replaces normal triangle drawing with a wireframe/line view."), "GPU",
                    "WireframeMode", GPUWireframeMode::Disabled, &Settings::ParseGPUWireframeMode,
                    &Settings::GetGPUWireframeModeName, &Settings::GetGPUWireframeModeDisplayName,
                    GPUWireframeMode::Count);

    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_DROPLET_SLASH, "Scaled Interlacing"),
                      FSUI_VSTR("Scales line skipping in interlaced rendering to the internal resolution, making it "
                                "less noticeable. Usually safe to enable."),
                      "GPU", "ScaledInterlacing", true, resolution_scale > 1);

    const GPUTextureFilter texture_filtering =
      Settings::ParseTextureFilterName(
        GetEffectiveTinyStringSetting(bsi, "GPU", "TextureFilter",
                                      Settings::GetTextureFilterName(Settings::DEFAULT_GPU_TEXTURE_FILTER)))
        .value_or(Settings::DEFAULT_GPU_TEXTURE_FILTER);

    DrawToggleSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_EYE_DROPPER, "Round Upscaled Texture Coordinates"),
      FSUI_VSTR("Rounds texture coordinates instead of flooring when upscaling. Can fix misaligned "
                "textures in some games, but break others, and is incompatible with texture filtering."),
      "GPU", "ForceRoundTextureCoordinates", false,
      resolution_scale > 1 && texture_filtering == GPUTextureFilter::Nearest);

    DrawToggleSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_DOWNLOAD, "Use Software Renderer For Readbacks"),
      FSUI_VSTR("Runs the software renderer in parallel for VRAM readbacks. On some systems, this may result in "
                "greater performance when using graphical enhancements with the hardware renderer."),
      "GPU", "UseSoftwareRendererForReadbacks", false);
  }

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_BOLT, "Threaded Rendering"),
                    FSUI_VSTR("Uses a second thread for drawing graphics. Provides a significant speed improvement "
                              "particularly with the software renderer, and is safe to use."),
                    "GPU", "UseThread", true);

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "Automatically Resize Window"),
                    FSUI_VSTR("Automatically resizes the window to match the internal resolution."), "Display",
                    "AutoResizeWindow", false);

  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_ENVELOPE, "Disable Mailbox Presentation"),
    FSUI_VSTR("Forces the use of FIFO over Mailbox presentation, i.e. double buffering instead of triple buffering. "
              "Usually results in worse frame pacing."),
    "Display", "DisableMailboxPresentation", false);

#ifdef _WIN32
  if (renderer == GPURenderer::HardwareD3D11 || renderer == GPURenderer::Software)
  {
    DrawToggleSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_PAINTBRUSH, "Use Blit Swap Chain"),
      FSUI_VSTR("Uses a blit presentation model instead of flipping. This may be needed on some systems."), "Display",
      "UseBlitSwapChain", false);
  }
#endif

  if (is_hardware && pgxp_enabled)
  {
    MenuHeading(FSUI_VSTR("PGXP (Precision Geometry Transform Pipeline)"));

    DrawToggleSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_IMAGES, "Perspective Correct Textures"),
      FSUI_VSTR("Uses perspective-correct interpolation for texture coordinates, straightening out warped textures."),
      "GPU", "PGXPTextureCorrection", true, pgxp_enabled);
    DrawToggleSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_PAINT_ROLLER, "Perspective Correct Colors"),
      FSUI_VSTR("Uses perspective-correct interpolation for colors, which can improve visuals in some games."), "GPU",
      "PGXPColorCorrection", false, pgxp_enabled);
    DrawToggleSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_TEXT_SLASH, "Culling Correction"),
      FSUI_VSTR("Increases the precision of polygon culling, reducing the number of holes in geometry."), "GPU",
      "PGXPCulling", true, pgxp_enabled);
    DrawToggleSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_DRAW_POLYGON, "Preserve Projection Precision"),
      FSUI_VSTR("Adds additional precision to PGXP data post-projection. May improve visuals in some games."), "GPU",
      "PGXPPreserveProjFP", false, pgxp_enabled);

    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_MICROCHIP, "CPU Mode"),
                      FSUI_VSTR("Uses PGXP for all instructions, not just memory operations."), "GPU", "PGXPCPU", false,
                      pgxp_enabled);

    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_VECTOR_SQUARE, "Vertex Cache"),
                      FSUI_VSTR("Uses screen positions to resolve PGXP data. May improve visuals in some games."),
                      "GPU", "PGXPVertexCache", pgxp_enabled);

    DrawToggleSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_SQUARE_MINUS, "Disable on 2D Polygons"),
      FSUI_VSTR("Uses native resolution coordinates for 2D polygons, instead of precise coordinates. Can "
                "fix misaligned UI in some games, but otherwise should be left disabled."),
      "GPU", "PGXPDisableOn2DPolygons", false, pgxp_enabled);

    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_RULER, "Depth Test Transparent Polygons"),
                      FSUI_VSTR("Enables depth testing for semi-transparent polygons. Usually these include shadows, "
                                "and tend to clip through the ground when depth testing is enabled."),
                      "GPU", "PGXPTransparentDepthTest", false, pgxp_enabled);

    DrawFloatRangeSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_STAR, "Geometry Tolerance"),
      FSUI_VSTR("Sets a threshold for discarding precise values when exceeded. May help with glitches in some games."),
      "GPU", "PGXPTolerance", -1.0f, -1.0f, 10.0f, "%.1f", pgxp_enabled);

    DrawFloatRangeSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_CIRCLE_MINUS, "Depth Clear Threshold"),
      FSUI_VSTR("Sets a threshold for discarding the emulated depth buffer. May help in some games."), "GPU",
      "PGXPDepthThreshold", Settings::DEFAULT_GPU_PGXP_DEPTH_THRESHOLD, 0.0f, static_cast<float>(GTE::MAX_Z), "%.1f",
      pgxp_enabled);
  }

  MenuHeading(FSUI_VSTR("Capture"));

  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_CAMERA, "Screenshot Size"),
                  FSUI_VSTR("Determines the size of screenshots created by DuckStation."), "Display", "ScreenshotMode",
                  Settings::DEFAULT_DISPLAY_SCREENSHOT_MODE, &Settings::ParseDisplayScreenshotMode,
                  &Settings::GetDisplayScreenshotModeName, &Settings::GetDisplayScreenshotModeDisplayName,
                  DisplayScreenshotMode::Count);
  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_FA_FILE_IMAGE, "Screenshot Format"),
                  FSUI_VSTR("Determines the format that screenshots will be saved/compressed with."), "Display",
                  "ScreenshotFormat", Settings::DEFAULT_DISPLAY_SCREENSHOT_FORMAT,
                  &Settings::ParseDisplayScreenshotFormat, &Settings::GetDisplayScreenshotFormatName,
                  &Settings::GetDisplayScreenshotFormatDisplayName, DisplayScreenshotFormat::Count);
  DrawIntRangeSetting(bsi, FSUI_ICONVSTR(ICON_FA_CAMERA_RETRO, "Screenshot Quality"),
                      FSUI_VSTR("Selects the quality at which screenshots will be compressed."), "Display",
                      "ScreenshotQuality", Settings::DEFAULT_DISPLAY_SCREENSHOT_QUALITY, 1, 100, "%d%%");

  MenuHeading(FSUI_VSTR("Texture Replacements"));

  const bool texture_cache_enabled = GetEffectiveBoolSetting(bsi, "GPU", "EnableTextureCache", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_ID_BADGE, "Enable Texture Cache"),
                    FSUI_VSTR("Enables caching of guest textures, required for texture replacement."), "GPU",
                    "EnableTextureCache", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_DATABASE, "Preload Replacement Textures"),
                    FSUI_VSTR("Loads all replacement texture to RAM, reducing stuttering at runtime."),
                    "TextureReplacements", "PreloadTextures", false,
                    ((texture_cache_enabled &&
                      GetEffectiveBoolSetting(bsi, "TextureReplacements", "EnableTextureReplacements", false)) ||
                     GetEffectiveBoolSetting(bsi, "TextureReplacements", "EnableVRAMWriteReplacements", false)));

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_FILE_IMPORT, "Enable Texture Replacements"),
                    FSUI_VSTR("Enables loading of replacement textures. Not compatible with all games."),
                    "TextureReplacements", "EnableTextureReplacements", false, texture_cache_enabled);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_LIST_CHECK, "Always Track Uploads"),
                    FSUI_VSTR("Forces texture upload tracking to be enabled regardless of whether it is needed."),
                    "TextureReplacements", "AlwaysTrackUploads", false, texture_cache_enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_FILE_EXPORT, "Enable Texture Dumping"),
    FSUI_VSTR("Enables dumping of textures to image files, which can be replaced. Not compatible with all games."),
    "TextureReplacements", "DumpTextures", false, texture_cache_enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_FILE, "Dump Replaced Textures"),
    FSUI_VSTR("Dumps textures that have replacements already loaded."), "TextureReplacements", "DumpReplacedTextures",
    false,
    (texture_cache_enabled && GetEffectiveBoolSetting(bsi, "TextureReplacements", "DumpTextures", false)) ||
      GetEffectiveBoolSetting(bsi, "TextureReplacements", "DumpVRAMWrites", false));

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_FILE_PEN, "Enable VRAM Write Replacement"),
                    FSUI_VSTR("Enables the replacement of background textures in supported games."),
                    "TextureReplacements", "EnableVRAMWriteReplacements", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_FILE_INVOICE, "Enable VRAM Write Dumping"),
                    FSUI_VSTR("Writes backgrounds that can be replaced to the dump directory."), "TextureReplacements",
                    "DumpVRAMWrites", false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_VIDEO, "Use Old MDEC Routines"),
                    FSUI_VSTR("Enables the older, less accurate MDEC decoding routines. May be required for old "
                              "replacement backgrounds to match/load."),
                    "Hacks", "UseOldMDECRoutines", false);

  DrawFolderSetting(bsi, FSUI_ICONVSTR(ICON_FA_FOLDER, "Textures Directory"), "Folders", "Textures",
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
  static constexpr const char* section = PostProcessing::Config::DISPLAY_CHAIN_SECTION;

  static constexpr auto queue_reload = []() {
    if (GPUThread::HasGPUBackend())
    {
      Host::RunOnCPUThread([]() {
        if (System::IsValid())
          GPUPresenter::ReloadPostProcessingSettings(true, false, false);
      });
    }
  };

  SettingsInterface* bsi = GetEditingSettingsInterface();
  bool reload_pending = false;

  BeginMenuButtons();

  MenuHeading(FSUI_VSTR("Controls"));
  ResetFocusHere();

  reload_pending |= DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_WAND_MAGIC_SPARKLES, "Enable Post Processing"),
                                      FSUI_VSTR("If not enabled, the current post processing chain will be ignored."),
                                      "PostProcessing", "Enabled", false);

  if (MenuButton(FSUI_ICONVSTR(ICON_FA_ARROWS_ROTATE, "Reload Shaders"),
                 FSUI_VSTR("Reloads the shaders from disk, applying any changes."),
                 bsi->GetBoolValue("PostProcessing", "Enabled", false)))
  {
    // Have to defer because of the settings lock.
    if (GPUThread::HasGPUBackend())
    {
      Host::RunOnCPUThread([]() { GPUPresenter::ReloadPostProcessingSettings(true, true, true); });
      ShowToast(std::string(), FSUI_STR("Post-processing shaders reloaded."));
    }
  }

  MenuHeading(FSUI_VSTR("Operations"));

  if (MenuButton(FSUI_ICONVSTR(ICON_PF_ADD, "Add Shader"), FSUI_VSTR("Adds a new shader to the chain.")))
  {
    std::vector<std::pair<std::string, std::string>> shaders = PostProcessing::GetAvailableShaderNames();
    ImGuiFullscreen::ChoiceDialogOptions options;
    options.reserve(shaders.size());
    for (auto& [display_name, name] : shaders)
      options.emplace_back(std::move(display_name), false);

    OpenChoiceDialog(FSUI_ICONVSTR(ICON_PF_ADD, "Add Shader"), false, std::move(options),
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
                         queue_reload();
                       }
                       else
                       {
                         ShowToast(std::string(),
                                   fmt::format(FSUI_FSTR("Failed to load shader {}. It may be invalid.\nError was:"),
                                               title, error.GetDescription()));
                       }
                     });
  }

  if (MenuButton(FSUI_ICONVSTR(ICON_PF_TRASH, "Clear Shaders"), FSUI_VSTR("Clears a shader from the chain.")))
  {
    OpenConfirmMessageDialog(
      FSUI_ICONVSTR(ICON_PF_TRASH, "Clear Shaders"),
      FSUI_STR("Are you sure you want to clear the current post-processing chain? All configuration will be lost."),
      [](bool confirmed) {
        if (!confirmed)
          return;

        SettingsInterface* bsi = GetEditingSettingsInterface();
        PostProcessing::Config::ClearStages(*bsi, section);
        PopulatePostProcessingChain(bsi, section);
        SetSettingsChanged(bsi);
        ShowToast(std::string(), FSUI_STR("Post-processing chain cleared."));
        queue_reload();
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

    if (MenuButton(FSUI_ICONVSTR(ICON_PF_REMOVE, "Remove From Chain"),
                   FSUI_VSTR("Removes this shader from the chain.")))
    {
      postprocessing_action = POSTPROCESSING_ACTION_REMOVE;
      postprocessing_action_index = stage_index;
    }

    if (MenuButton(FSUI_ICONVSTR(ICON_FA_ARROW_UP, "Move Up"),
                   FSUI_VSTR("Moves this shader higher in the chain, applying it earlier."), (stage_index > 0)))
    {
      postprocessing_action = POSTPROCESSING_ACTION_MOVE_UP;
      postprocessing_action_index = stage_index;
    }

    if (MenuButton(FSUI_ICONVSTR(ICON_FA_ARROW_DOWN, "Move Down"),
                   FSUI_VSTR("Moves this shader lower in the chain, applying it later."),
                   (stage_index != (s_state.postprocessing_stages.size() - 1))))
    {
      postprocessing_action = POSTPROCESSING_ACTION_MOVE_DOWN;
      postprocessing_action_index = stage_index;
    }

    for (PostProcessing::ShaderOption& opt : si.options)
    {
      if (!opt.help_text.empty())
      {
        str.format("##help_{}{}", stage_index, opt.name);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        MenuButton(str, opt.help_text, false);
        ImGui::PopStyleColor();
      }

      if (opt.ShouldHide())
        continue;

      switch (opt.type)
      {
        case PostProcessing::ShaderOption::Type::Bool:
        {
          bool value = (opt.value[0].int_value != 0);
          tstr.format(ICON_FA_GEAR " {}", opt.ui_name);
          if (ToggleButton(tstr,
                           (opt.default_value[0].int_value != 0) ? FSUI_VSTR("Default: Enabled") :
                                                                   FSUI_VSTR("Default: Disabled"),
                           &value))
          {
            opt.value[0].int_value = (value != 0);
            PostProcessing::Config::SetStageOption(*bsi, section, stage_index, opt);
            SetSettingsChanged(bsi);
            queue_reload();
          }
        }
        break;

        case PostProcessing::ShaderOption::Type::Float:
        {
          tstr.format(ICON_FA_RULER_VERTICAL " {}###{}", opt.ui_name, opt.name);
          str.format(FSUI_FSTR("Value: {} | Default: {} | Minimum: {} | Maximum: {}"), opt.value[0].float_value,
                     opt.default_value[0].float_value, opt.min_value[0].float_value, opt.max_value[0].float_value);
          if (MenuButton(tstr, str))
            OpenFixedPopupDialog(tstr);

          if (IsFixedPopupDialogOpen(tstr) &&
              BeginFixedPopupDialog(LayoutScale(LAYOUT_SMALL_POPUP_PADDING), LayoutScale(LAYOUT_SMALL_POPUP_PADDING),
                                    LayoutScale(500.0f, 200.0f)))
          {
            BeginMenuButtons();

            const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();

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
              reload_pending = true;
            }

            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
            if (MenuButtonWithoutSummary(FSUI_VSTR("OK"), true, LAYOUT_CENTER_ALIGN_TEXT))
              CloseFixedPopupDialog();

            EndMenuButtons();

            EndFixedPopupDialog();
          }
        }
        break;

        case PostProcessing::ShaderOption::Type::Int:
        {
          tstr.format(ICON_FA_RULER_VERTICAL " {}##{}", opt.ui_name, opt.name);
          str.format(FSUI_FSTR("Value: {} | Default: {} | Minimum: {} | Maximum: {}"), opt.value[0].int_value,
                     opt.default_value[0].int_value, opt.min_value[0].int_value, opt.max_value[0].int_value);
          if (MenuButton(tstr, str))
            OpenFixedPopupDialog(tstr);

          if (IsFixedPopupDialogOpen(tstr) &&
              BeginFixedPopupDialog(LayoutScale(LAYOUT_SMALL_POPUP_PADDING), LayoutScale(LAYOUT_SMALL_POPUP_PADDING),
                                    LayoutScale(500.0f, 200.0f)))
          {
            BeginMenuButtons();

            const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
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
              reload_pending = true;
            }

            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
            if (MenuButtonWithoutSummary(FSUI_VSTR("OK"), true, LAYOUT_CENTER_ALIGN_TEXT))
              CloseFixedPopupDialog();
            EndMenuButtons();

            EndFixedPopupDialog();
          }
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
      reload_pending = true;
    }
    break;
    case POSTPROCESSING_ACTION_MOVE_UP:
    {
      PostProcessing::Config::MoveStageUp(*bsi, section, postprocessing_action_index);
      PopulatePostProcessingChain(bsi, section);
      SetSettingsChanged(bsi);
      reload_pending = true;
    }
    break;
    case POSTPROCESSING_ACTION_MOVE_DOWN:
    {
      PostProcessing::Config::MoveStageDown(*bsi, section, postprocessing_action_index);
      PopulatePostProcessingChain(bsi, section);
      SetSettingsChanged(bsi);
      reload_pending = true;
    }
    break;
    default:
      break;
  }

  MenuHeading(FSUI_VSTR("Border Overlay"));

  {
    const std::optional<TinyString> preset_name = bsi->GetOptionalTinyStringValue("BorderOverlay", "PresetName");
    const bool is_null = !preset_name.has_value();
    const bool is_none = (!is_null && preset_name->empty());
    const bool is_custom = (!is_null && preset_name.value() == "Custom");
    const std::string_view visible_value =
      is_null ? FSUI_VSTR("Use Global Setting") :
                (is_none ? FSUI_VSTR("None") : (is_custom ? FSUI_VSTR("Custom") : preset_name->view()));
    if (MenuButtonWithValue(
          FSUI_ICONVSTR(ICON_FA_BORDER_ALL, "Selected Preset"),
          FSUI_VSTR("Select from the list of preset borders, or manually specify a custom configuration."),
          visible_value))
    {
      std::vector<std::string> preset_names = GPUPresenter::EnumerateBorderOverlayPresets();
      ChoiceDialogOptions options;
      options.reserve(preset_names.size() + 2 + BoolToUInt32(IsEditingGameSettings(bsi)));
      if (IsEditingGameSettings(bsi))
        options.emplace_back(FSUI_STR("Use Global Setting"), is_null);
      options.emplace_back(FSUI_STR("None"), is_none);
      options.emplace_back(FSUI_STR("Custom"), is_custom);
      for (std::string& name : preset_names)
      {
        const bool is_selected = (preset_name.has_value() && preset_name.value() == name);
        options.emplace_back(std::move(name), is_selected);
      }

      OpenChoiceDialog(FSUI_ICONVSTR(ICON_FA_BORDER_ALL, "Border Overlay"), false, std::move(options),
                       [game_settings = IsEditingGameSettings(bsi)](s32 index, const std::string& title, bool) mutable {
                         if (index < 0)
                           return;

                         auto lock = Host::GetSettingsLock();
                         SettingsInterface* const bsi = GetEditingSettingsInterface(game_settings);
                         const s32 offset = static_cast<s32>(BoolToUInt32(game_settings));
                         if (game_settings && index == 0)
                         {
                           bsi->DeleteValue("BorderOverlay", "PresetName");
                         }
                         else
                         {
                           const char* new_value =
                             (index == (offset + 0)) ? "" : ((index == (offset + 1)) ? "Custom" : title.c_str());
                           bsi->SetStringValue("BorderOverlay", "PresetName", new_value);
                         }
                         SetSettingsChanged(bsi);
                         queue_reload();
                       });
    }

    if (is_custom)
    {
      if (MenuButton(FSUI_ICONVSTR(ICON_FA_IMAGE, "Image Path"),
                     GetEffectiveTinyStringSetting(bsi, "BorderOverlay", "ImagePath", "")))
      {
        OpenFileSelector(
          FSUI_ICONVSTR(ICON_FA_IMAGE, "Image Path"), false,
          [game_settings = IsEditingGameSettings(bsi)](const std::string& path) {
            if (path.empty())
              return;

            SettingsInterface* const bsi = GetEditingSettingsInterface(game_settings);
            bsi->SetStringValue("BorderOverlay", "ImagePath", path.c_str());
            SetSettingsChanged(bsi);
            queue_reload();
          },
          GetImageFilters());
      }

      reload_pending |= DrawIntRectSetting(
        bsi, FSUI_ICONVSTR(ICON_FA_BORDER_ALL, "Display Area"),
        FSUI_VSTR("Determines the area of the overlay image that the display will be drawn within."), "BorderOverlay",
        "DisplayStartX", 0, "DisplayStartY", 0, "DisplayEndX", 0, "DisplayEndY", 0, 0, 16384, "%dpx");

      reload_pending |=
        DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_BLENDER, "Alpha Blending"),
                          FSUI_VSTR("If enabled, the transparency of the overlay image will be applied."),
                          "BorderOverlay", "AlphaBlend", false);

      reload_pending |= DrawToggleSetting(
        bsi, FSUI_ICONVSTR(ICON_FA_BLENDER, "Destination Alpha Blending"),
        FSUI_VSTR("If enabled, the display will be blended with the transparency of the overlay image."),
        "BorderOverlay", "DestinationAlphaBlend", false);
    }
  }

  EndMenuButtons();

  if (reload_pending)
    queue_reload();
}

void FullscreenUI::DrawAudioSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading(FSUI_VSTR("Audio Control"));
  ResetFocusHere();

  DrawIntRangeSetting(bsi, FSUI_ICONVSTR(ICON_FA_VOLUME_HIGH, "Output Volume"),
                      FSUI_VSTR("Controls the volume of the audio played on the host."), "Audio", "OutputVolume", 100,
                      0, 200, "%d%%");
  DrawIntRangeSetting(bsi, FSUI_ICONVSTR(ICON_PF_FAST_FORWARD, "Fast Forward Volume"),
                      FSUI_VSTR("Controls the volume of the audio played on the host when fast forwarding."), "Audio",
                      "FastForwardVolume", 100, 0, 200, "%d%%");
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_VOLUME_XMARK, "Mute All Sound"),
                    FSUI_VSTR("Prevents the emulator from producing any audible sound."), "Audio", "OutputMuted",
                    false);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_COMPACT_DISC, "Mute CD Audio"),
                    FSUI_VSTR("Forcibly mutes both CD-DA and XA audio from the CD-ROM. Can be used to "
                              "disable background music in some games."),
                    "CDROM", "MuteCDAudio", false);

  MenuHeading(FSUI_VSTR("Backend Settings"));

  DrawEnumSetting(
    bsi, FSUI_ICONVSTR(ICON_PF_SPEAKER, "Audio Backend"),
    FSUI_VSTR("The audio backend determines how frames produced by the emulator are submitted to the host."), "Audio",
    "Backend", AudioStream::DEFAULT_BACKEND, &AudioStream::ParseBackendName, &AudioStream::GetBackendName,
    &AudioStream::GetBackendDisplayName, AudioBackend::Count);
  DrawEnumSetting(bsi, FSUI_ICONVSTR(ICON_PF_SFX_SOUND_EFFECT_NOISE, "Stretch Mode"),
                  FSUI_CSTR("Determines quality of audio when not running at 100% speed."), "Audio", "StretchMode",
                  AudioStreamParameters::DEFAULT_STRETCH_MODE, &AudioStream::ParseStretchMode,
                  &AudioStream::GetStretchModeName, &AudioStream::GetStretchModeDisplayName, AudioStretchMode::Count);
  DrawIntRangeSetting(bsi, FSUI_ICONVSTR(ICON_FA_BUCKET, "Buffer Size"),
                      FSUI_VSTR("Determines the amount of audio buffered before being pulled by the host API."),
                      "Audio", "BufferMS", AudioStreamParameters::DEFAULT_BUFFER_MS, 10, 500, FSUI_CSTR("%d ms"));
  if (!GetEffectiveBoolSetting(bsi, "Audio", "OutputLatencyMinimal",
                               AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MINIMAL))
  {
    DrawIntRangeSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_STOPWATCH_20, "Output Latency"),
      FSUI_VSTR("Determines how much latency there is between the audio being picked up by the host API, and "
                "played through speakers."),
      "Audio", "OutputLatencyMS", AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MS, 1, 500, FSUI_CSTR("%d ms"));
  }
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_STOPWATCH, "Minimal Output Latency"),
                    FSUI_VSTR("When enabled, the minimum supported output latency will be used for the host API."),
                    "Audio", "OutputLatencyMinimal", AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MINIMAL);

  EndMenuButtons();
}

void FullscreenUI::DrawAchievementsSettingsHeader(SettingsInterface* bsi, std::unique_lock<std::mutex>& settings_lock)
{
  ImDrawList* const dl = ImGui::GetWindowDrawList();

  const float panel_height = LayoutScale(100.0f);
  const float panel_rounding = LayoutScale(20.0f);
  const float spacing = LayoutScale(10.0f);
  const float line_spacing = LayoutScale(5.0f);
  const float badge_size = LayoutScale(60.0f);
  const ImGuiStyle& style = ImGui::GetStyle();

  const ImVec2 bg_pos = ImGui::GetCursorScreenPos() + ImVec2(0.0f, spacing * 2.0f);
  const ImVec2 bg_size = ImVec2(ImGui::GetContentRegionAvail().x, LayoutScale(100.0f));
  dl->AddRectFilled(bg_pos, bg_pos + bg_size,
                    ImGui::GetColorU32(ModAlpha(DarkerColor(UIStyle.BackgroundColor), GetBackgroundAlpha())),
                    panel_rounding);

  // must be after background rect
  BeginMenuButtons();

  ImVec2 pos = bg_pos + ImVec2(panel_rounding, panel_rounding);
  const float max_content_width = bg_size.x - panel_rounding;

  const ImVec2 pos_backup = ImGui::GetCursorPos();

  TinyString username;
  std::string_view badge_path;
  SmallString score_summary;
  const bool logged_in = (bsi->ContainsValue("Cheevos", "Token"));
  {
    // avoid locking order issues
    settings_lock.unlock();
    {
      const auto lock = Achievements::GetLock();
      if (s_state.achievements_user_badge_path.empty()) [[unlikely]]
        s_state.achievements_user_badge_path = Achievements::GetLoggedInUserBadgePath();

      badge_path = s_state.achievements_user_badge_path;
      if (badge_path.empty())
        badge_path = "images/ra-generic-user.png";

      if (Achievements::IsLoggedIn())
      {
        const char* username_ptr = Achievements::GetLoggedInUserName();
        if (username_ptr)
          username = username_ptr;
      }
      else if (Achievements::IsLoggedInOrLoggingIn())
      {
        username = FSUI_VSTR("Logging In...");
      }
      else if (!logged_in)
      {
        username = FSUI_VSTR("Not Logged In");
      }
      else
      {
        // client not active
        username = bsi->GetSmallStringValue("Cheevos", "Username");
      }

      score_summary = Achievements::GetLoggedInUserPointsSummary();
      if (score_summary.empty())
      {
        if (logged_in)
          score_summary = FSUI_VSTR("Enable Achievements to see your user summary.");
        else
          score_summary = FSUI_VSTR("To use achievements, please log in with your retroachievements.org account.");
      }
    }
    settings_lock.lock();
  }

  if (GPUTexture* badge_tex = GetCachedTextureAsync(badge_path))
  {
    const ImRect badge_rect =
      ImGuiFullscreen::CenterImage(ImRect(pos, pos + ImVec2(badge_size, badge_size)), badge_tex);
    dl->AddImage(reinterpret_cast<ImTextureID>(GetCachedTextureAsync(badge_path)), badge_rect.Min, badge_rect.Max);
  }

  pos.x += badge_size + LayoutScale(15.0f);

  RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, pos,
                            pos + ImVec2(max_content_width - pos.x, UIStyle.LargeFontSize),
                            ImGui::GetColorU32(ImGuiCol_Text), username);

  pos.y += UIStyle.LargeFontSize + line_spacing;

  ImGuiFullscreen::RenderAutoLabelText(dl, UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.NormalFontWeight,
                                       UIStyle.BoldFontWeight, pos,
                                       pos + ImVec2(max_content_width - pos.x, UIStyle.LargeFontSize),
                                       ImGui::GetColorU32(ImGuiCol_Text), score_summary, ':');

  if (!IsEditingGameSettings(bsi))
  {
    const auto login_logout_text =
      logged_in ? FSUI_ICONVSTR(ICON_FA_KEY, "Logout") : FSUI_ICONVSTR(ICON_FA_KEY, "Login");
    const ImVec2 login_logout_button_size =
      UIStyle.Font->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight, FLT_MAX, 0.0f,
                                  IMSTR_START_END(login_logout_text)) +
      style.FramePadding * 2.0f;
    const ImVec2 login_logout_button_pos =
      ImVec2(bg_pos.x + bg_size.x - panel_rounding - login_logout_button_size.x - LayoutScale(10.0f),
             bg_pos.y + ((panel_height - UIStyle.LargeFontSize - (style.FramePadding.y * 2.0f)) * 0.5f));

    ImGui::SetCursorPos(login_logout_button_pos);

    bool visible, hovered;
    const bool clicked = MenuButtonFrame(
      "login_logout", true, ImRect(login_logout_button_pos, login_logout_button_pos + login_logout_button_size),
      &visible, &hovered);
    if (visible)
    {
      const ImRect text_bb = ImRect(login_logout_button_pos + style.FramePadding,
                                    login_logout_button_pos + login_logout_button_size - style.FramePadding);
      RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, text_bb.Min,
                                text_bb.Max, ImGui::GetColorU32(ImGuiCol_Text), login_logout_text, nullptr,
                                ImVec2(0.0f, 0.0f), 0.0f, &text_bb);
    }

    if (clicked)
    {
      if (logged_in)
        Host::RunOnCPUThread(&Achievements::Logout);
      else
        OpenFixedPopupDialog(ACHIEVEMENTS_LOGIN_DIALOG_NAME);
    }
  }

  ImGui::SetCursorPos(ImVec2(pos_backup.x, pos_backup.y + panel_height + (spacing * 3.0f)));
}

void FullscreenUI::DrawAchievementsSettingsPage(std::unique_lock<std::mutex>& settings_lock)
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  DrawAchievementsSettingsHeader(bsi, settings_lock);

  ResetFocusHere();
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_TROPHY, "Enable Achievements"),
                    FSUI_VSTR("When enabled and logged in, DuckStation will scan for achievements on startup."),
                    "Cheevos", "Enabled", false);

  const bool enabled = GetEffectiveBoolSetting(bsi, "Cheevos", "Enabled", false);

  if (DrawToggleSetting(
        bsi, FSUI_ICONVSTR(ICON_FA_HAT_COWBOY, "Hardcore Mode"),
        FSUI_VSTR("\"Challenge\" mode for achievements, including leaderboard tracking. Disables save state, "
                  "cheats, and slowdown functions."),
        "Cheevos", "ChallengeMode", false, enabled))
  {
    if (GPUThread::HasGPUBackend() && bsi->GetBoolValue("Cheevos", "ChallengeMode", false))
    {
      // prevent locking order deadlock
      settings_lock.unlock();
      const auto lock = Achievements::GetLock();
      if (Achievements::HasActiveGame())
      {
        OpenConfirmMessageDialog(
          FSUI_ICONVSTR(ICON_FA_HAT_COWBOY, "Hardcore Mode"),
          FSUI_STR("Hardcore mode will not be enabled until the system is reset. Do you want to reset the system now?"),
          [](bool result) {
            if (result)
              Host::RunOnCPUThread(&System::ResetSystem);
          });
      }
      settings_lock.lock();
    }
  }

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_ARROW_ROTATE_RIGHT, "Encore Mode"),
                    FSUI_VSTR("When enabled, each session will behave as if no achievements have been unlocked."),
                    "Cheevos", "EncoreMode", false, enabled);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_USER_LOCK, "Spectator Mode"),
                    FSUI_VSTR("When enabled, DuckStation will assume all achievements are locked and not send any "
                              "unlock notifications to the server."),
                    "Cheevos", "SpectatorMode", false, enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_FLASK_VIAL, "Test Unofficial Achievements"),
    FSUI_VSTR("When enabled, DuckStation will list achievements from unofficial sets. These achievements are not "
              "tracked by RetroAchievements."),
    "Cheevos", "UnofficialTestMode", false, enabled);

  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_BELL, "Achievement Notifications"),
    FSUI_VSTR("Displays popup messages on events such as achievement unlocks and leaderboard submissions."), "Cheevos",
    "Notifications", true, enabled);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_LIST_OL, "Leaderboard Notifications"),
                    FSUI_VSTR("Displays popup messages when starting, submitting, or failing a leaderboard challenge."),
                    "Cheevos", "LeaderboardNotifications", true, enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_CLOCK, "Leaderboard Trackers"),
    FSUI_VSTR("Shows a timer in the bottom-right corner of the screen when leaderboard challenges are active."),
    "Cheevos", "LeaderboardTrackers", true, enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_MUSIC, "Sound Effects"),
    FSUI_VSTR("Plays sound effects for events such as achievement unlocks and leaderboard submissions."), "Cheevos",
    "SoundEffects", true, enabled);
  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_BARS_PROGRESS, "Progress Indicators"),
    FSUI_VSTR(
      "Shows a popup in the lower-right corner of the screen when progress towards a measured achievement changes."),
    "Cheevos", "ProgressIndicators", true, enabled);
  DrawEnumSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_TEMPERATURE_ARROW_UP, "Challenge Indicators"),
    FSUI_VSTR("Shows a notification or icons in the lower-right corner of the screen when a challenge/primed "
              "achievement is active."),
    "Cheevos", "ChallengeIndicatorMode", Settings::DEFAULT_ACHIEVEMENT_CHALLENGE_INDICATOR_MODE,
    &Settings::ParseAchievementChallengeIndicatorMode, &Settings::GetAchievementChallengeIndicatorModeName,
    &Settings::GetAchievementChallengeIndicatorModeDisplayName, AchievementChallengeIndicatorMode::MaxCount, enabled);

  if (!IsEditingGameSettings(bsi))
  {
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_ARROWS_ROTATE, "Update Progress"),
                   FSUI_VSTR("Updates the progress database for achievements shown in the game list.")))
    {
      Host::RunOnCPUThread([]() {
        Error error;
        if (!Achievements::RefreshAllProgressDatabase(&error))
          ImGuiFullscreen::ShowToast(FSUI_STR("Failed to update progress database"), error.TakeDescription(),
                                     Host::OSD_ERROR_DURATION);
      });
    }

    if (bsi->ContainsValue("Cheevos", "Token"))
    {
      const std::string ts_string = Host::FormatNumber(
        Host::NumberFormatType::LongDateTime,
        StringUtil::FromChars<s64>(bsi->GetTinyStringValue("Cheevos", "LoginTimestamp", "0")).value_or(0));
      MenuButtonWithoutSummary(
        SmallString::from_format(fmt::runtime(FSUI_ICONVSTR(ICON_FA_USER_CLOCK, "Login token generated on {}")),
                                 ts_string),
        false);
    }
  }

  EndMenuButtons();

  if (IsFixedPopupDialogOpen(ACHIEVEMENTS_LOGIN_DIALOG_NAME))
    DrawAchievementsLoginWindow();
}

void FullscreenUI::DrawAchievementsLoginWindow()
{
  static constexpr const char* LOGIN_PROGRESS_NAME = "AchievementsLogin";

  static char username[256] = {};
  static char password[256] = {};
  static std::unique_ptr<std::string> login_error;

  if (!BeginFixedPopupDialog(LayoutScale(LAYOUT_LARGE_POPUP_PADDING), LayoutScale(LAYOUT_LARGE_POPUP_ROUNDING),
                             LayoutScale(600.0f, 0.0f)))
  {
    std::memset(username, 0, sizeof(username));
    std::memset(password, 0, sizeof(password));
    login_error.reset();
    return;
  }

  const std::string_view ra_title = "RetroAchivements";
  const ImVec2 ra_title_size = UIStyle.Font->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight, FLT_MAX, 0.0f,
                                                           IMSTR_START_END(ra_title));
  const float ra_title_spacing = LayoutScale(10.0f);
  GPUTexture* ra_logo = GetCachedTexture("images/ra-icon.webp");
  const ImVec2 ra_logo_size = ImVec2(UIStyle.LargeFontSize * 1.85f, UIStyle.LargeFontSize);
  const ImVec2 ra_logo_imgsize = CenterImage(ra_logo_size, ra_logo).GetSize();
  const ImRect work_rect = ImGui::GetCurrentWindow()->WorkRect;
  const float indent = (work_rect.GetWidth() - (ra_logo_size.x + ra_title_spacing + ra_title_size.x)) * 0.5f;
  ImDrawList* const dl = ImGui::GetWindowDrawList();
  const ImVec2 ra_logo_pos = work_rect.Min + ImVec2(indent, 0.0f);
  dl->AddImage(ra_logo, ra_logo_pos, ra_logo_pos + ra_logo_imgsize);
  dl->AddText(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight,
              ra_logo_pos + ImVec2(ra_logo_size.x + ra_title_spacing, 0.0f), ImGui::GetColorU32(ImGuiCol_Text),
              IMSTR_START_END(ra_title));

  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ra_logo_size.y + LayoutScale(15.0f));

  ImGui::PushStyleColor(ImGuiCol_Text, DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]));

  if (!login_error)
  {
    ImGui::TextWrapped(
      FSUI_CSTR("Please enter your user name and password for retroachievements.org below. Your password will "
                "not be saved in DuckStation, an access token will be generated and used instead."));
  }
  else
  {
    ImGui::TextWrapped("%s", login_error->c_str());
  }

  ImGui::PopStyleColor();

  ImGui::ItemSize(ImVec2(0.0f, LayoutScale(LAYOUT_MENU_BUTTON_SPACING * 2.0f)));

  const bool is_logging_in = ImGuiFullscreen::IsBackgroundProgressDialogOpen(LOGIN_PROGRESS_NAME);
  BeginMenuButtons();
  ResetFocusHere();

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

  const float item_width = LayoutScale(550.0f);

  ImGui::SetCursorPosX((ImGui::GetWindowWidth() - item_width) * 0.5f);
  ImGui::SetNextItemWidth(item_width);
  ImGui::InputTextWithHint("##username", FSUI_CSTR("User Name"), username, sizeof(username),
                           is_logging_in ? ImGuiInputTextFlags_ReadOnly : 0);
  ImGui::NextColumn();

  ImGui::SetCursorPosX((ImGui::GetWindowWidth() - item_width) * 0.5f);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
  ImGui::SetNextItemWidth(item_width);
  ImGui::InputTextWithHint("##password", FSUI_CSTR("Password"), password, sizeof(password),
                           is_logging_in ? (ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_Password) :
                                           ImGuiInputTextFlags_Password);

  ImGui::PopStyleVar(2);

  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(15.0f));

  const bool login_enabled = (std::strlen(username) > 0 && std::strlen(password) > 0 && !is_logging_in);

  if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_KEY, "Login"), login_enabled, LAYOUT_CENTER_ALIGN_TEXT))
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
          CloseFixedPopupDialog();
          return;
        }

        login_error = std::make_unique<std::string>(
          fmt::format(FSUI_FSTR("Login Failed.\nError: {}\nPlease check your username and password, and try again."),
                      error.GetDescription()));
      });
    });
  }

  if (MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_XMARK, "Cancel"), !is_logging_in, LAYOUT_CENTER_ALIGN_TEXT))
    CloseFixedPopupDialog();

  EndMenuButtons();

  EndFixedPopupDialog();
}

void FullscreenUI::DrawAdvancedSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  BeginMenuButtons();

  MenuHeading(FSUI_VSTR("Logging Settings"));
  ResetFocusHere();

  DrawEnumSetting(bsi, FSUI_VSTR("Log Level"),
                  FSUI_VSTR("Sets the verbosity of messages logged. Higher levels will log more messages."), "Logging",
                  "LogLevel", Settings::DEFAULT_LOG_LEVEL, &Settings::ParseLogLevelName, &Settings::GetLogLevelName,
                  &Settings::GetLogLevelDisplayName, Log::Level::MaxCount);
  DrawToggleSetting(bsi, FSUI_VSTR("Log To System Console"), FSUI_VSTR("Logs messages to the console window."),
                    "Logging", "LogToConsole", false);
  DrawToggleSetting(bsi, FSUI_VSTR("Log To Debug Console"),
                    FSUI_VSTR("Logs messages to the debug console where supported."), "Logging", "LogToDebug", false);
  DrawToggleSetting(bsi, FSUI_VSTR("Log To File"), FSUI_VSTR("Logs messages to duckstation.log in the user directory."),
                    "Logging", "LogToFile", false);
  DrawToggleSetting(bsi, FSUI_VSTR("Log Timestamps"),
                    FSUI_VSTR("Includes the elapsed time since the application start in window and console logs."),
                    "Logging", "LogTimestamps", true);
  DrawToggleSetting(bsi, FSUI_VSTR("Log File Timestamps"),
                    FSUI_VSTR("Includes the elapsed time since the application start in file logs."), "Logging",
                    "LogFileTimestamps", false);

  MenuHeading(FSUI_VSTR("Debugging Settings"));

  DrawToggleSetting(bsi, FSUI_VSTR("Use Debug GPU Device"),
                    FSUI_VSTR("Enable debugging when supported by the host's renderer API. Only for developer use."),
                    "GPU", "UseDebugDevice", false);

  DrawToggleSetting(
    bsi, FSUI_VSTR("Enable GPU-Based Validation"),
    FSUI_VSTR("Enable GPU-based validation when supported by the host's renderer API. Only for developer use."), "GPU",
    "UseGPUBasedValidation", false);

  DrawToggleSetting(
    bsi, FSUI_VSTR("Prefer OpenGL ES Context"),
    FSUI_VSTR("Uses OpenGL ES even when desktop OpenGL is supported. May improve performance on some SBC drivers."),
    "GPU", "PreferGLESContext", Settings::DEFAULT_GPU_PREFER_GLES_CONTEXT);

  DrawToggleSetting(
    bsi, FSUI_VSTR("Load Devices From Save States"),
    FSUI_VSTR("When enabled, memory cards and controllers will be overwritten when save states are loaded."), "Main",
    "LoadDevicesFromSaveStates", false);
  DrawEnumSetting(bsi, FSUI_VSTR("Save State Compression"),
                  FSUI_VSTR("Reduces the size of save states by compressing the data before saving."), "Main",
                  "SaveStateCompression", Settings::DEFAULT_SAVE_STATE_COMPRESSION_MODE,
                  &Settings::ParseSaveStateCompressionModeName, &Settings::GetSaveStateCompressionModeName,
                  &Settings::GetSaveStateCompressionModeDisplayName, SaveStateCompressionMode::Count);

  MenuHeading(FSUI_VSTR("CPU Emulation"));

  DrawToggleSetting(bsi, FSUI_VSTR("Enable Recompiler Memory Exceptions"),
                    FSUI_VSTR("Enables alignment and bus exceptions. Not needed for any known games."), "CPU",
                    "RecompilerMemoryExceptions", false);
  DrawToggleSetting(
    bsi, FSUI_VSTR("Enable Recompiler Block Linking"),
    FSUI_VSTR("Performance enhancement - jumps directly between blocks instead of returning to the dispatcher."), "CPU",
    "RecompilerBlockLinking", true);
  DrawEnumSetting(bsi, FSUI_VSTR("Recompiler Fast Memory Access"),
                  FSUI_VSTR("Avoids calls to C++ code, significantly speeding up the recompiler."), "CPU",
                  "FastmemMode", Settings::DEFAULT_CPU_FASTMEM_MODE, &Settings::ParseCPUFastmemMode,
                  &Settings::GetCPUFastmemModeName, &Settings::GetCPUFastmemModeDisplayName, CPUFastmemMode::Count);

  MenuHeading(FSUI_VSTR("CD-ROM Emulation"));

  DrawIntRangeSetting(
    bsi, FSUI_VSTR("Readahead Sectors"),
    FSUI_VSTR("Reduces hitches in emulation by reading/decompressing CD data asynchronously on a worker thread."),
    "CDROM", "ReadaheadSectors", Settings::DEFAULT_CDROM_READAHEAD_SECTORS, 0, 32, FSUI_CSTR("%d sectors"));

  DrawIntRangeSetting(bsi, FSUI_VSTR("Maximum Seek Speedup Cycles"),
                      FSUI_VSTR("Sets the minimum delay for the 'Maximum' seek speedup level."), "CDROM",
                      "MaxSeekSpeedupCycles", Settings::DEFAULT_CDROM_MAX_SEEK_SPEEDUP_CYCLES, 1, 1000000,
                      FSUI_CSTR("%d cycles"));

  DrawIntRangeSetting(bsi, FSUI_VSTR("Maximum Read Speedup Cycles"),
                      FSUI_VSTR("Sets the minimum delay for the 'Maximum' read speedup level."), "CDROM",
                      "MaxReadSpeedupCycles", Settings::DEFAULT_CDROM_MAX_READ_SPEEDUP_CYCLES, 1, 1000000,
                      FSUI_CSTR("%d cycles"));

  DrawToggleSetting(
    bsi, FSUI_VSTR("Disable Speedup on MDEC"),
    FSUI_VSTR("Tries to detect FMVs and disable read speedup during games that don't use XA streaming audio."), "CDROM",
    "DisableSpeedupOnMDEC", false);

  DrawToggleSetting(bsi, FSUI_VSTR("Enable Region Check"),
                    FSUI_VSTR("Simulates the region check present in original, unmodified consoles."), "CDROM",
                    "RegionCheck", false);

  DrawToggleSetting(
    bsi, FSUI_VSTR("Allow Booting Without SBI File"),
    FSUI_VSTR("Allows booting to continue even without a required SBI file. These games will not run correctly."),
    "CDROM", "AllowBootingWithoutSBIFile", false);

  EndMenuButtons();
}

void FullscreenUI::DrawPatchesOrCheatsSettingsPage(bool cheats)
{
  SettingsInterface* bsi = GetEditingSettingsInterface();

  const Cheats::CodeInfoList& code_list = cheats ? s_state.game_cheats_list : s_state.game_patch_list;
  std::vector<std::string>& enable_list = cheats ? s_state.enabled_game_cheat_cache : s_state.enabled_game_patch_cache;
  const char* section = cheats ? Cheats::CHEATS_CONFIG_SECTION : Cheats::PATCHES_CONFIG_SECTION;

  BeginMenuButtons();
  ResetFocusHere();

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
        options.emplace_back(FSUI_STR("Disabled"), !has_option);

        for (const Cheats::CodeOption& opt : ci.options)
          options.emplace_back(opt.first, has_option && (visible_value.view() == opt.first));

        OpenChoiceDialog(ci.name, false, std::move(options),
                         [cheat_name = ci.name, cheats, section](s32 index, const std::string& title, bool checked) {
                           if (index < 0)
                             return;

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
        OpenFixedPopupDialog(title);

      if (IsFixedPopupDialogOpen(title) &&
          BeginFixedPopupDialog(LayoutScale(LAYOUT_SMALL_POPUP_PADDING), LayoutScale(LAYOUT_SMALL_POPUP_PADDING),
                                LayoutScale(600.0f, 0.0f)))
      {
        BeginMenuButtons();

        bool range_value_changed = false;

        BeginHorizontalMenuButtons(4);
        HorizontalMenuButton(visible_value, false);

        s32 step = 0;
        ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
        if (HorizontalMenuButton(ICON_FA_CHEVRON_UP))
          step = step_value;
        if (HorizontalMenuButton(ICON_FA_CHEVRON_DOWN))
          step = -step_value;
        ImGui::PopItemFlag();
        if (HorizontalMenuButton(ICON_FA_ARROW_ROTATE_LEFT))
        {
          range_value = ci.option_range_start - 1;
          range_value_changed = true;
        }

        EndHorizontalMenuButtons(10.0f);

        if (step != 0)
        {
          range_value += step;
          range_value_changed = true;
        }

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

        if (MenuButtonWithoutSummary(FSUI_VSTR("OK"), true, LAYOUT_CENTER_ALIGN_TEXT))
          CloseFixedPopupDialog();

        EndMenuButtons();

        EndFixedPopupDialog();
      }
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
    MenuButtonWithoutSummary(
      FSUI_ICONVSTR(
        ICON_EMOJI_WARNING,
        "WARNING: Activating cheats can cause unpredictable behavior, crashing, soft-locks, or broken saved games."),
      false);

    MenuHeading(FSUI_VSTR("Settings"));

    bool enable_cheats = bsi->GetBoolValue("Cheats", "EnableCheats", false);
    if (ToggleButton(FSUI_ICONVSTR(ICON_PF_CHEATS, "Enable Cheats"),
                     FSUI_VSTR("Enables the cheats that are selected below."), &enable_cheats))
    {
      if (enable_cheats)
        bsi->SetBoolValue("Cheats", "EnableCheats", true);
      else
        bsi->DeleteValue("Cheats", "EnableCheats");
      SetSettingsChanged(bsi);
    }

    bool load_database_cheats = bsi->GetBoolValue("Cheats", "LoadCheatsFromDatabase", true);
    if (ToggleButton(FSUI_ICONVSTR(ICON_FA_DATABASE, "Load Database Cheats"),
                     FSUI_VSTR("Enables loading of cheats for this game from DuckStation's database."),
                     &load_database_cheats))
    {
      if (load_database_cheats)
        bsi->DeleteValue("Cheats", "LoadCheatsFromDatabase");
      else
        bsi->SetBoolValue("Cheats", "LoadCheatsFromDatabase", false);
      SetSettingsChanged(bsi);
      PopulatePatchesAndCheatsList();
    }

    bool sort_list = bsi->GetBoolValue("Cheats", "SortList", false);
    if (ToggleButton(FSUI_ICONVSTR(ICON_FA_ARROW_DOWN_A_Z, "Sort Alphabetically"),
                     FSUI_VSTR("Sorts the cheat list alphabetically by the name of the code."), &sort_list))
    {
      if (!sort_list)
        bsi->DeleteValue("Cheats", "SortList");
      else
        bsi->SetBoolValue("Cheats", "SortList", true);
      SetSettingsChanged(bsi);
      s_state.game_cheats_list = Cheats::GetCodeInfoList(s_state.game_settings_serial, s_state.game_settings_hash, true,
                                                         load_database_cheats, sort_list);
    }

    if (code_list.empty())
    {
      MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_STORE_SLASH, "No cheats are available for this game."), false);
    }
    else
    {
      for (const std::string_view& group : s_state.game_cheat_groups)
      {
        MenuHeading(group.empty() ? FSUI_VSTR("Ungrouped") : group);
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
    MenuButtonWithoutSummary(
      FSUI_ICONVSTR(
        ICON_EMOJI_WARNING,
        "WARNING: Activating game patches can cause unpredictable behavior, crashing, soft-locks, or broken "
        "saved games."),
      false);

    if (code_list.empty())
    {
      MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_STORE_SLASH, "No patches are available for this game."), false);
    }
    else
    {
      MenuHeading(FSUI_VSTR("Game Patches"));

      for (const Cheats::CodeInfo& ci : code_list)
        draw_code(bsi, section, ci, enable_list, false);
    }
  }

  EndMenuButtons();
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
  const ImU32 text_color = ImGui::GetColorU32(DarkerColor(UIStyle.BackgroundTextColor));

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
                                  display_size, title_text_color, rp);
        text_pos.y += UIStyle.MediumFontSize + scaled_text_spacing;
      }
    }

    RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, text_pos,
                              display_size, text_color, buffer);

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
                    ImGui::GetColorU32(ModAlpha(UIStyle.BackgroundColor, 0.85f)));

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
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Return To Game"))},
                            GetBackgroundAlpha());
  }
  else
  {
    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, FSUI_VSTR("Change Selection")),
                 std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")),
                 std::make_pair(ICON_PF_ESC, FSUI_VSTR("Return To Game"))},
      GetBackgroundAlpha());
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
    ClearSaveStateEntryList();

    if (GPUThread::HasGPUBackend())
    {
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

          ImGuiFullscreen::DrawMenuButtonFrame(bb.Min, bb.Max, col, true);

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

          OpenChoiceDialog(
            std::move(title), false, std::move(options),
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
                  ShowToast({}, fmt::format(FSUI_FSTR("{} does not exist."), ImGuiFullscreen::RemoveHash(entry.title)));
                }
                else if (FileSystem::DeleteFile(entry.state_path.c_str()))
                {
                  ShowToast({}, fmt::format(FSUI_FSTR("{} deleted."), ImGuiFullscreen::RemoveHash(entry.title)));

                  // need to preserve the texture, since it's going to be drawn this frame
                  // TODO: do this with a transition for safety
                  g_gpu_device->RecycleTexture(std::move(entry.preview_texture));

                  if (s_state.save_state_selector_loading)
                    s_state.save_state_selector_slots.erase(s_state.save_state_selector_slots.begin() + i);
                  else
                    InitializePlaceholderSaveStateListEntry(&entry, entry.slot, entry.global);
                }
                else
                {
                  ShowToast({},
                            fmt::format(FSUI_FSTR("Failed to delete {}."), ImGuiFullscreen::RemoveHash(entry.title)));
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
                 std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Cancel"))},
      GetBackgroundAlpha());
  }
  else
  {
    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT,
                                FSUI_VSTR("Select State")),
                 std::make_pair(ICON_PF_F1, FSUI_VSTR("Delete State")),
                 std::make_pair(ICON_PF_ENTER, s_state.save_state_selector_loading ? FSUI_VSTR("Load State") :
                                                                                     FSUI_VSTR("Save State")),
                 std::make_pair(ICON_PF_ESC, FSUI_VSTR("Cancel"))},
      GetBackgroundAlpha());
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
  ImGuiFullscreen::TextAlignedMultiLine(0.5f, IMSTR_START_END(sick));
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

                case 8: // Achievements
                {
                  // sort by unlock percentage
                  const float unlock_lhs =
                    (lhs->num_achievements > 0) ?
                      (static_cast<float>(std::max(lhs->unlocked_achievements, lhs->unlocked_achievements_hc)) /
                       static_cast<float>(lhs->num_achievements)) :
                      0;
                  const float unlock_rhs =
                    (rhs->num_achievements > 0) ?
                      (static_cast<float>(std::max(rhs->unlocked_achievements, rhs->unlocked_achievements_hc)) /
                       static_cast<float>(rhs->num_achievements)) :
                      0;
                  if (std::abs(unlock_lhs - unlock_rhs) >= 0.0001f)
                    return reverse ? (unlock_lhs >= unlock_rhs) : (unlock_lhs < unlock_rhs);

                  // order by achievement count
                  if (lhs->num_achievements != rhs->num_achievements)
                    return reverse ? (rhs->num_achievements < lhs->num_achievements) :
                                     (lhs->num_achievements < rhs->num_achievements);
                }
              }

              // fallback to title when all else is equal
              const int res = StringUtil::CompareNoCase(lhs->GetSortTitle(), rhs->GetSortTitle());
              if (res != 0)
                return reverse ? (res > 0) : (res < 0);

              // fallback to path when all else is equal
              return reverse ? (lhs->path > rhs->path) : (lhs->path < rhs->path);
            });
}

void FullscreenUI::DrawGameListWindow()
{
  auto game_list_lock = GameList::GetLock();
  PopulateGameListEntryList();

  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, UIStyle.LargeFontSize + (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "gamelist_view",
                            MulAlpha(UIStyle.PrimaryColor, GetBackgroundAlpha())))
  {
    static constexpr const char* icons[] = {ICON_FA_TABLE_CELLS_LARGE, ICON_FA_LIST};
    static constexpr const char* titles[] = {FSUI_NSTR("Game Grid"), FSUI_NSTR("Game List")};
    static constexpr u32 count = static_cast<u32>(std::size(titles));

    BeginNavBar();

    if (NavButton(ICON_PF_NAVIGATION_BACK, true, true))
      BeginTransition([]() { SwitchToMainWindow(MainWindowType::Landing); });

    NavTitle(Host::TranslateToStringView(FSUI_TR_CONTEXT, titles[static_cast<u32>(s_state.game_list_view)]));
    RightAlignNavButtons(count);

    for (u32 i = 0; i < count; i++)
    {
      if (NavButton(icons[i], static_cast<GameListView>(i) == s_state.game_list_view, true))
      {
        BeginTransition([]() {
          s_state.game_list_view =
            (s_state.game_list_view == GameListView::Grid) ? GameListView::List : GameListView::Grid;
          QueueResetFocus(FocusResetType::ViewChanged);
        });
      }
    }

    EndNavBar();
  }

  EndFullscreenWindow();

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

  // note: has to come afterwards
  if (!AreAnyDialogsOpen())
  {
    if (ImGui::IsKeyPressed(ImGuiKey_NavGamepadMenu, false) || ImGui::IsKeyPressed(ImGuiKey_F4, false))
    {
      BeginTransition([]() {
        s_state.game_list_view =
          (s_state.game_list_view == GameListView::Grid) ? GameListView::List : GameListView::Grid;
        QueueResetFocus(FocusResetType::ViewChanged);
      });
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) || ImGui::IsKeyPressed(ImGuiKey_F2, false))
    {
      BeginTransition(&SwitchToSettings);
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) || ImGui::IsKeyPressed(ImGuiKey_F3, false))
    {
      DoResume();
    }
  }

  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_XBOX_DPAD, FSUI_VSTR("Select Game")),
                                       std::make_pair(ICON_PF_BURGER_MENU, FSUI_VSTR("Resume Last Session")),
                                       std::make_pair(ICON_PF_SHARE_CAPTURE, FSUI_VSTR("Settings")),
                                       std::make_pair(ICON_PF_BUTTON_X, FSUI_VSTR("Change View")),
                                       std::make_pair(ICON_PF_BUTTON_Y, FSUI_VSTR("Launch Options")),
                                       std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Start Game")),
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Back"))},
                            GetBackgroundAlpha());
  }
  else
  {
    SetFullscreenFooterText(
      std::array{
        std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT,
                       FSUI_VSTR("Select Game")),
        std::make_pair(ICON_PF_F3, FSUI_VSTR("Resume Last Session")),
        std::make_pair(ICON_PF_F2, FSUI_VSTR("Settings")),
        std::make_pair(ICON_PF_F4, FSUI_VSTR("Change View")),
        std::make_pair(ICON_PF_F1, FSUI_VSTR("Launch Options")),
        std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Start Game")),
        std::make_pair(ICON_PF_ESC, FSUI_VSTR("Back")),
      },
      GetBackgroundAlpha());
  }
}

void FullscreenUI::DrawGameList(const ImVec2& heading_size)
{
  static constexpr auto to_mb = [](s64 size) { return static_cast<u32>((size + 1048575) / 1048576); };

  if (!BeginFullscreenColumns(nullptr, heading_size.y, true, true))
  {
    EndFullscreenColumns();
    return;
  }

  if (!AreAnyDialogsOpen() && WantsToCloseMenu())
    BeginTransition([]() { SwitchToMainWindow(MainWindowType::Landing); });

  auto game_list_lock = GameList::GetLock();
  const GameList::Entry* selected_entry = nullptr;
  PopulateGameListEntryList();

  if (IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  if (BeginFullscreenColumnWindow(0.0f, -530.0f, "game_list_entries", GetTransparentBackgroundColor(),
                                  ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING)))
  {
    const bool compact_mode = Host::GetBaseBoolSettingValue("Main", "FullscreenUIGameListCompactMode", true);
    const float image_size = compact_mode ? UIStyle.LargeFontSize : LayoutScale(50.0f);
    const float row_image_padding = LayoutScale(compact_mode ? 15.0f : 15.0f);
    const float row_left_margin = image_size + row_image_padding;

    ResetFocusHere();

    BeginMenuButtons();

    SmallString summary;
    const u32 text_color = ImGui::GetColorU32(ImGui::GetStyle().Colors[ImGuiCol_Text]);
    const u32 subtitle_text_color = ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]));

    for (const GameList::Entry* entry : s_state.game_list_sorted_entries)
    {
      if (!compact_mode)
      {
        if (entry->serial.empty())
          summary.format("{} | {} MB", Path::GetFileName(entry->path), to_mb(entry->file_size));
        else
          summary.format("{} | {} | {} MB", entry->serial, Path::GetFileName(entry->path), to_mb(entry->file_size));
      }

      const ImGuiFullscreen::MenuButtonBounds mbb(entry->GetDisplayTitle(s_state.show_localized_titles), {}, summary,
                                                  row_left_margin);

      bool visible, hovered;
      bool pressed = MenuButtonFrame(GetKeyForGameListEntry(entry), true, mbb.frame_bb, &visible, &hovered);
      if (!visible)
        continue;

      GPUTexture* cover_texture = GetGameListCover(entry, false, true);
      const ImRect image_rect(CenterImage(
        ImRect(ImVec2(mbb.title_bb.Min.x - row_left_margin, mbb.title_bb.Min.y),
               ImVec2(mbb.title_bb.Min.x - row_image_padding, mbb.title_bb.Min.y + image_size)),
        ImVec2(static_cast<float>(cover_texture->GetWidth()), static_cast<float>(cover_texture->GetHeight()))));

      ImGui::GetWindowDrawList()->AddImage(cover_texture, image_rect.Min, image_rect.Max, ImVec2(0.0f, 0.0f),
                                           ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
      RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, mbb.title_bb.Min,
                                mbb.title_bb.Max, text_color, entry->GetDisplayTitle(s_state.show_localized_titles),
                                &mbb.title_size, ImVec2(0.0f, 0.0f), mbb.title_size.x, &mbb.title_bb);

      if (!summary.empty())
      {
        RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, mbb.summary_bb.Min,
                                  mbb.summary_bb.Max, subtitle_text_color, summary, &mbb.summary_size,
                                  ImVec2(0.0f, 0.0f), mbb.summary_size.x, &mbb.summary_bb);
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
             ImGui::IsKeyPressed(ImGuiKey_F1, false)))
        {
          CancelPendingMenuClose();
          HandleGameListOptions(selected_entry);
        }
      }

      if (entry == s_state.game_list_sorted_entries.front())
        ImGui::SetItemDefaultFocus();
    }

    EndMenuButtons();
  }
  SetWindowNavWrapping(false, true);
  EndFullscreenColumnWindow();

  // avoid clearing the selection for a couple of seconds when the mouse goes inbetween items
  static constexpr float ITEM_TIMEOUT = 1.0f;
  if (!selected_entry)
  {
    if (!s_state.game_list_current_selection_path.empty())
    {
      // reset countdown if a dialog was open
      if (AreAnyDialogsOpen())
      {
        s_state.game_list_current_selection_timeout = ITEM_TIMEOUT;
      }
      else
      {
        s_state.game_list_current_selection_timeout -= ImGui::GetIO().DeltaTime;
        if (s_state.game_list_current_selection_timeout <= 0.0f)
        {
          s_state.game_list_current_selection_timeout = 0.0f;
          s_state.game_list_current_selection_path.clear();
        }
      }
    }

    if (!s_state.game_list_current_selection_path.empty())
      selected_entry = GameList::GetEntryForPath(s_state.game_list_current_selection_path);
  }
  else
  {
    // reset countdown on new or current item
    if (s_state.game_list_current_selection_path != selected_entry->path)
      s_state.game_list_current_selection_path = selected_entry->path;
    s_state.game_list_current_selection_timeout = ITEM_TIMEOUT;
  }

  static constexpr float info_window_width = 530.0f;
  if (BeginFullscreenColumnWindow(-info_window_width, 0.0f, "game_list_info",
                                  ModAlpha(UIStyle.PrimaryDarkColor, GetBackgroundAlpha())))
  {
    static constexpr float info_top_margin = 20.0f;
    static constexpr float cover_size = 320.0f;
    GPUTexture* cover_texture = selected_entry ? GetGameListCover(selected_entry, false, false) :
                                                 GetTextureForGameListEntryType(GameList::EntryType::MaxCount);
    if (cover_texture)
    {
      const ImRect image_rect(CenterImage(
        LayoutScale(ImVec2(cover_size, cover_size)),
        ImVec2(static_cast<float>(cover_texture->GetWidth()), static_cast<float>(cover_texture->GetHeight()))));

      ImGui::SetCursorPos(LayoutScale((info_window_width - cover_size) / 2.0f, info_top_margin) + image_rect.Min);
      ImGui::Image(cover_texture, image_rect.GetSize());
    }

    const float work_width = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    static constexpr float field_margin_y = 4.0f;
    static constexpr float start_x = 50.0f;
    float text_y = info_top_margin + cover_size + info_top_margin;

    float text_width;

    PushPrimaryColor();
    ImGui::SetCursorPos(LayoutScale(start_x, text_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, LayoutScale(0.0f, field_margin_y));
    ImGui::BeginGroup();

    if (selected_entry)
    {
      const ImVec4 subtitle_text_color = DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]);
      const std::string_view title = selected_entry->GetDisplayTitle(s_state.show_localized_titles);

      // title
      ImGui::PushFont(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight);
      text_width = ImGui::CalcTextSize(IMSTR_START_END(title), false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::TextWrapped("%.*s", static_cast<int>(title.size()), title.data());
      ImGui::PopFont();

      ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);

      // developer
      if (selected_entry->dbentry && !selected_entry->dbentry->developer.empty())
      {
        text_width =
          ImGui::CalcTextSize(selected_entry->dbentry->developer.data(),
                              selected_entry->dbentry->developer.data() + selected_entry->dbentry->developer.length(),
                              false, work_width)
            .x;
        ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
        ImGui::TextWrapped("%.*s", static_cast<int>(selected_entry->dbentry->developer.size()),
                           selected_entry->dbentry->developer.data());
        ImGui::PopStyleColor();
      }

      // code
      text_width = ImGui::CalcTextSize(selected_entry->serial.c_str(), nullptr, false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::PushStyleColor(ImGuiCol_Text, DarkerColor(subtitle_text_color));
      ImGui::TextWrapped("%s", selected_entry->serial.c_str());
      ImGui::PopStyleColor();
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(15.0f));

      ImGui::PopFont();
      ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight);

      // region
      {
        const bool display_as_language = (selected_entry->dbentry && selected_entry->dbentry->HasAnyLanguage());
        ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
        ImGuiFullscreen::TextUnformatted(
          FSUI_ICONVSTR(ICON_EMOJI_GLOBE, display_as_language ? FSUI_CSTR("Language: ") : FSUI_CSTR("Region: ")));
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::Image(GetCachedTexture(selected_entry->GetLanguageIconName(), 23, 16), LayoutScale(23.0f, 16.0f));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
        if (display_as_language)
        {
          ImGui::TextWrapped(" (%s, %s)", selected_entry->dbentry->GetLanguagesString().c_str(),
                             Settings::GetDiscRegionName(selected_entry->region));
        }
        else
        {
          ImGui::TextWrapped(" (%s)", Settings::GetDiscRegionName(selected_entry->region));
        }
        ImGui::PopStyleColor();
      }

      // genre
      if (selected_entry->dbentry)
      {
        if (!selected_entry->dbentry->genre.empty())
        {
          ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
          ImGuiFullscreen::TextUnformatted(FSUI_ICONVSTR(ICON_EMOJI_BOOKS, FSUI_VSTR("Genre: ")));
          ImGui::PopFont();
          ImGui::SameLine();
          ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
          ImGui::TextUnformatted(selected_entry->dbentry->genre.data(),
                                 selected_entry->dbentry->genre.data() + selected_entry->dbentry->genre.length());
          ImGui::PopStyleColor();
        }

        if (selected_entry->dbentry->release_date != 0)
        {
          ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
          ImGuiFullscreen::TextUnformatted(FSUI_ICONVSTR(ICON_EMOJI_CALENDAR, FSUI_VSTR("Release Date: ")));
          ImGui::PopFont();
          ImGui::SameLine();
          ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
          ImGui::TextUnformatted(selected_entry->GetReleaseDateString().c_str());
          ImGui::PopStyleColor();
        }
      }

      // achievements
      if (selected_entry->num_achievements > 0)
      {
        ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
        ImGuiFullscreen::TextUnformatted(FSUI_ICONVSTR(ICON_EMOJI_TROPHY, "Achievements: "));
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
        if (selected_entry->unlocked_achievements_hc > 0)
        {
          ImGui::Text("%u (%u) / %u", selected_entry->unlocked_achievements, selected_entry->unlocked_achievements_hc,
                      selected_entry->num_achievements);
        }
        else
        {
          ImGui::Text("%u / %u", selected_entry->unlocked_achievements, selected_entry->num_achievements);
        }
        ImGui::PopStyleColor();
      }

      // compatibility
      ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
      ImGuiFullscreen::TextUnformatted(FSUI_ICONSTR(ICON_EMOJI_STAR, FSUI_VSTR("Compatibility: ")));
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::Image(GetCachedTexture(selected_entry->GetCompatibilityIconFileName(), 88, 16), LayoutScale(88.0f, 16.0f));
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
      ImGui::Text(" (%s)", GameDatabase::GetCompatibilityRatingDisplayName(
                             (selected_entry && selected_entry->dbentry) ? selected_entry->dbentry->compatibility :
                                                                           GameDatabase::CompatibilityRating::Unknown));
      ImGui::PopStyleColor();

      // play time
      ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
      ImGuiFullscreen::TextUnformatted(FSUI_ICONSTR(ICON_EMOJI_HOURGLASS, FSUI_VSTR("Time Played: ")));
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
      ImGui::TextUnformatted(GameList::FormatTimespan(selected_entry->total_played_time).c_str());
      ImGui::PopStyleColor();
      ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
      ImGuiFullscreen::TextUnformatted(FSUI_ICONSTR(ICON_EMOJI_CLOCK_FIVE_OCLOCK, FSUI_CSTR("Last Played: ")));
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
      ImGui::TextUnformatted(GameList::FormatTimestamp(selected_entry->last_played_time).c_str());
      ImGui::PopStyleColor();

      // size
      if (selected_entry->file_size >= 0)
      {
        ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
        ImGuiFullscreen::TextUnformatted(FSUI_ICONSTR(ICON_EMOJI_FILE_FOLDER_OPEN, FSUI_VSTR("Size: ")));
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
        ImGui::Text(FSUI_CSTR("%u MB"), to_mb(selected_entry->uncompressed_size));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, DarkerColor(subtitle_text_color));
        ImGui::Text(FSUI_CSTR(" (%u MB on disk)"), to_mb(selected_entry->file_size));
        ImGui::PopStyleColor();
      }
      else
      {
        ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
        ImGui::TextUnformatted(FSUI_CSTR("Unknown File Size"));
        ImGui::PopFont();
      }

      ImGui::PopFont();
    }
    else
    {
      // title
      const char* title = FSUI_CSTR("No Game Selected");
      ImGui::PushFont(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight);
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
        GetTransparentBackgroundColor(), 0.0f, ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING)))
  {
    EndFullscreenWindow();
    return;
  }

  if (ImGui::IsWindowFocused() && WantsToCloseMenu())
    BeginTransition([]() { SwitchToMainWindow(MainWindowType::Landing); });

  ResetFocusHere();
  BeginMenuButtons(0, 0.0f, 15.0f, 15.0f, 20.0f, 20.0f);

  const ImGuiStyle& style = ImGui::GetStyle();

  const float title_font_size = UIStyle.MediumFontSize;
  const float title_font_weight = UIStyle.BoldFontWeight;
  const float avail_width = ImGui::GetContentRegionAvail().x;
  const float title_spacing = LayoutScale(10.0f);
  const float item_width_with_spacing = std::floor(avail_width / 5.0f);
  const float item_width = item_width_with_spacing - style.ItemSpacing.x;
  const float image_width = item_width - (style.FramePadding.x * 2.0f);
  const float image_height = image_width;
  const ImVec2 image_size(image_width, image_height);
  const float base_item_height = (style.FramePadding.y * 2.0f) + image_height;
  const u32 grid_count_x = static_cast<u32>(std::floor(avail_width / item_width_with_spacing));

  // calculate padding to center it, the last item in the row doesn't need spacing
  const float x_padding = std::floor(
    (avail_width - ((item_width_with_spacing * static_cast<float>(grid_count_x)) - style.ItemSpacing.x)) * 0.5f);

  ImGuiWindow* const window = ImGui::GetCurrentWindow();
  ImDrawList* const dl = ImGui::GetWindowDrawList();
  SmallString draw_title;
  const u32 text_color = ImGui::GetColorU32(ImGuiCol_Text);

  u32 grid_x = 0;
  float row_item_height = base_item_height;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x_padding);
  for (size_t entry_index = 0; entry_index < s_state.game_list_sorted_entries.size(); entry_index++)
  {
    if (window->SkipItems)
      continue;

    // This is pretty annoying. If we don't use an equal sized item for each grid item, keyboard/gamepad navigation
    // tends to break when scrolling vertically - it goes left/right. Precompute the maximum item height for the row
    // first, and make all items the same size to work around this.
    const GameList::Entry* entry = s_state.game_list_sorted_entries[entry_index];
    if (grid_x == 0 && s_state.game_grid_show_titles)
    {
      row_item_height = 0.0f;

      const size_t row_entry_index_end = std::min(entry_index + grid_count_x, s_state.game_list_sorted_entries.size());
      for (size_t row_entry_index = entry_index; row_entry_index < row_entry_index_end; row_entry_index++)
      {
        const GameList::Entry* row_entry = s_state.game_list_sorted_entries[row_entry_index];
        const std::string_view row_title = row_entry->GetDisplayTitle(s_state.show_localized_titles);
        const ImVec2 this_title_size = UIStyle.Font->CalcTextSizeA(title_font_size, title_font_weight, image_width,
                                                                   image_width, IMSTR_START_END(row_title));
        row_item_height = std::max(row_item_height, this_title_size.y);
      }

      row_item_height += title_spacing + base_item_height;
    }

    ImVec2 title_size;
    if (s_state.game_grid_show_titles)
    {
      const std::string_view title = entry->GetDisplayTitle(s_state.show_localized_titles);
      title_size = UIStyle.Font->CalcTextSizeA(title_font_size, title_font_weight, image_width, image_width,
                                               IMSTR_START_END(title));
    }

    const std::string_view item_key = GetKeyForGameListEntry(entry);
    const ImGuiID id = window->GetID(IMSTR_START_END(item_key));
    const ImVec2 pos(window->DC.CursorPos);
    const ImVec2 item_size(item_width, row_item_height);
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

        ImGuiFullscreen::DrawMenuButtonFrame(bb.Min, bb.Max, col, true);

        ImGui::PopStyleColor();
      }

      bb.Min += style.FramePadding;
      bb.Max -= style.FramePadding;

      GPUTexture* const cover_texture = GetGameListCover(entry, false, false);
      const ImRect image_rect(
        CenterImage(ImRect(bb.Min, bb.Min + image_size), ImVec2(static_cast<float>(cover_texture->GetWidth()),
                                                                static_cast<float>(cover_texture->GetHeight()))));

      dl->AddImage(cover_texture, image_rect.Min, image_rect.Max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                   IM_COL32(255, 255, 255, 255));

      GPUTexture* const cover_trophy = GetGameListCoverTrophy(entry, image_size);
      if (cover_trophy)
      {
        const ImVec2 trophy_size =
          ImVec2(static_cast<float>(cover_trophy->GetWidth()), static_cast<float>(cover_trophy->GetHeight()));
        dl->AddImage(cover_trophy, bb.Min + image_size - trophy_size, bb.Min + image_size, ImVec2(0.0f, 0.0f),
                     ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
      }

      if (draw_title)
      {
        const ImRect title_bb(ImVec2(bb.Min.x, bb.Min.y + image_height + title_spacing), bb.Max);
        ImGuiFullscreen::RenderMultiLineShadowedTextClipped(
          dl, UIStyle.Font, title_font_size, title_font_weight, title_bb.Min, title_bb.Max, text_color,
          entry->GetDisplayTitle(s_state.show_localized_titles), LAYOUT_CENTER_ALIGN_TEXT, image_width, &title_bb);
      }

      if (pressed)
      {
        HandleGameListActivate(entry);
      }
      else if (hovered &&
               (ImGui::IsItemClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_NavGamepadInput, false) ||
                ImGui::IsKeyPressed(ImGuiKey_F1, false)))
      {
        CancelPendingMenuClose();
        HandleGameListOptions(entry);
      }
    }

    if (entry == s_state.game_list_sorted_entries.front())
      ImGui::SetItemDefaultFocus();
    else if (entry == s_state.game_list_sorted_entries.back())
      break;

    grid_x++;
    if (grid_x == grid_count_x)
    {
      grid_x = 0;
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x_padding);
    }
    else
    {
      ImGui::SameLine();
    }
  }

  EndMenuButtons();
  EndFullscreenWindow();
}

void FullscreenUI::HandleGameListActivate(const GameList::Entry* entry)
{
  if (entry->IsDiscSet())
  {
    HandleSelectDiscForDiscSet(entry->dbentry->disc_set);
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
      {FSUI_ICONSTR(ICON_FA_IMAGE, "Set Cover Image"), false},
      {FSUI_ICONSTR(ICON_FA_PLAY, "Resume Game"), false},
      {FSUI_ICONSTR(ICON_FA_ARROW_ROTATE_LEFT, "Load State"), false},
      {FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Default Boot"), false},
      {FSUI_ICONSTR(ICON_FA_BOLT, "Fast Boot"), false},
      {FSUI_ICONSTR(ICON_FA_HOURGLASS, "Slow Boot"), false},
      {FSUI_ICONSTR(ICON_FA_DELETE_LEFT, "Reset Play Time"), false},
    };

    OpenChoiceDialog(
      entry->GetDisplayTitle(s_state.show_localized_titles), false, std::move(options),
      [entry_path = entry->path, entry_serial = entry->serial](s32 index, const std::string& title,
                                                               bool checked) mutable {
        switch (index)
        {
          case 0: // Open Game Properties
            BeginTransition([entry_path = std::move(entry_path)]() { SwitchToGameSettingsForPath(entry_path); });
            break;
          case 1: // Open Containing Directory
            ExitFullscreenAndOpenURL(Path::CreateFileURL(Path::GetDirectory(entry_path)));
            break;
          case 2: // Set Cover Image
            DoSetCoverImage(std::move(entry_path));
            break;
          case 3: // Resume Game
            DoStartPath(entry_path, System::GetGameSaveStatePath(entry_serial, -1));
            break;
          case 4: // Load State
            BeginTransition([entry_serial = std::move(entry_serial), entry_path = std::move(entry_path)]() {
              OpenSaveStateSelector(entry_serial, entry_path, true);
            });
            break;
          case 5: // Default Boot
            DoStartPath(entry_path);
            break;
          case 6: // Fast Boot
            DoStartPath(entry_path, {}, true);
            break;
          case 7: // Slow Boot
            DoStartPath(entry_path, {}, false);
            break;
          case 8: // Reset Play Time
            GameList::ClearPlayedTimeForSerial(entry_serial);
            break;
          default:
            break;
        }
      });
  }
  else
  {
    ImGuiFullscreen::ChoiceDialogOptions options = {
      {FSUI_ICONSTR(ICON_FA_WRENCH, "Game Properties"), false},
      {FSUI_ICONSTR(ICON_FA_IMAGE, "Set Cover Image"), false},
      {FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Select Disc"), false},
    };

    const GameDatabase::DiscSetEntry* dsentry = entry->dbentry->disc_set;
    OpenChoiceDialog(dsentry->GetDisplayTitle(s_state.show_localized_titles), false, std::move(options),
                     [dsentry](s32 index, const std::string& title, bool checked) mutable {
                       switch (index)
                       {
                         case 0: // Open Game Properties
                           BeginTransition([dsentry]() {
                             // shouldn't fail
                             const GameList::Entry* first_disc_entry = GameList::GetFirstDiscSetMember(dsentry);
                             if (!first_disc_entry)
                               return;

                             SwitchToGameSettingsForPath(first_disc_entry->path);
                           });
                           break;
                         case 1: // Set Cover Image
                           DoSetCoverImage(std::string(dsentry->GetSaveTitle()));
                           break;
                         case 2: // Select Disc
                           HandleSelectDiscForDiscSet(dsentry);
                           break;
                         default:
                           break;
                       }
                     });
  }
}

void FullscreenUI::HandleSelectDiscForDiscSet(const GameDatabase::DiscSetEntry* dsentry)
{
  auto lock = GameList::GetLock();
  const std::vector<const GameList::Entry*> entries = GameList::GetDiscSetMembers(dsentry, true);
  if (entries.empty())
    return;

  ImGuiFullscreen::ChoiceDialogOptions options;
  std::vector<std::string> paths;
  paths.reserve(entries.size());

  for (u32 i = 0; i < static_cast<u32>(entries.size()); i++)
  {
    const GameList::Entry* const entry = entries[i];
    std::string title = fmt::format(ICON_FA_COMPACT_DISC " {} {} | {}##{}", FSUI_VSTR("Disc"),
                                    entry->disc_set_index + 1, Path::GetFileName(entry->path), i);
    options.emplace_back(std::move(title), false);
    paths.push_back(entry->path);
  }
  options.emplace_back(FSUI_ICONVSTR(ICON_FA_SQUARE_XMARK, "Close Menu"), false);

  OpenChoiceDialog(
    fmt::format(FSUI_FSTR("Select Disc for {}"), dsentry->GetDisplayTitle(s_state.show_localized_titles)), false,
    std::move(options), [paths = std::move(paths)](s32 index, const std::string& title, bool checked) {
      if (static_cast<u32>(index) >= paths.size())
        return;

      auto lock = GameList::GetLock();
      const GameList::Entry* entry = GameList::GetEntryForPath(paths[index]);
      if (entry)
        HandleGameListActivate(entry);
    });
}

void FullscreenUI::DrawGameListSettingsPage()
{
  SettingsInterface* bsi = GetEditingSettingsInterface(false);

  BeginMenuButtons();
  ResetFocusHere();

  MenuHeading(FSUI_VSTR("List Settings"));
  {
    static constexpr const char* view_types[] = {FSUI_NSTR("Game Grid"), FSUI_NSTR("Game List")};
    static constexpr const char* sort_types[] = {
      FSUI_NSTR("Type"),
      FSUI_NSTR("Serial"),
      FSUI_NSTR("Title"),
      FSUI_NSTR("File Title"),
      FSUI_NSTR("Time Played"),
      FSUI_NSTR("Last Played"),
      FSUI_NSTR("File Size"),
      FSUI_NSTR("Uncompressed Size"),
      FSUI_NSTR("Achievement Unlock/Count"),
    };

    DrawIntListSetting(bsi, FSUI_ICONVSTR(ICON_FA_TABLE_CELLS_LARGE, "Default View"),
                       FSUI_VSTR("Selects the view that the game list will open to."), "Main",
                       "DefaultFullscreenUIGameView", 0, view_types);
    DrawIntListSetting(bsi, FSUI_ICONVSTR(ICON_FA_SORT, "Sort By"),
                       FSUI_VSTR("Determines that field that the game list will be sorted by."), "Main",
                       "FullscreenUIGameSort", 0, sort_types);
    DrawToggleSetting(
      bsi, FSUI_ICONVSTR(ICON_FA_ARROW_DOWN_Z_A, "Sort Reversed"),
      FSUI_VSTR("Reverses the game list sort order from the default (usually ascending to descending)."), "Main",
      "FullscreenUIGameSortReverse", false);
    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_RECTANGLE_LIST, "Merge Multi-Disc Games"),
                      FSUI_VSTR("Merges multi-disc games into one item in the game list."), "Main",
                      "FullscreenUIMergeDiscSets", true);
    if (DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_LANGUAGE, "Show Localized Titles"),
                          FSUI_VSTR("Uses localized (native language) titles in the game list."), "Main",
                          "FullscreenUIShowLocalizedTitles", true))
    {
      s_state.show_localized_titles = bsi->GetBoolValue("Main", "FullscreenUIShowLocalizedTitles", true);
    }
    if (DrawToggleSetting(
          bsi, FSUI_ICONVSTR(ICON_FA_TROPHY, "Show Achievement Trophy Icons"),
          FSUI_VSTR("Shows trophy icons in game grid when games have achievements or have been mastered."), "Main",
          "FullscreenUIShowTrophyIcons", true))
    {
      s_state.game_list_show_trophy_icons = bsi->GetBoolValue("Main", "FullscreenUIShowTrophyIcons", true);
    }
    if (DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_TAGS, "Show Grid View Titles"),
                          FSUI_VSTR("Shows titles underneath the images in the game grid view."), "Main",
                          "FullscreenUIShowGridTitles", true))
    {
      s_state.game_grid_show_titles = bsi->GetBoolValue("Main", "FullscreenUIShowGridTitles", true);
    }

    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_TEXT_SLASH, "List Compact Mode"),
                      FSUI_VSTR("Displays only the game title in the list, instead of the title and serial/file name."),
                      "Main", "FullscreenUIGameListCompactMode", true);
  }

  MenuHeading(FSUI_VSTR("Search Directories"));
  if (MenuButton(FSUI_ICONVSTR(ICON_FA_FOLDER_PLUS, "Add Search Directory"),
                 FSUI_VSTR("Adds a new directory to the game search list.")))
  {
    OpenFileSelector(FSUI_ICONVSTR(ICON_FA_FOLDER_PLUS, "Add Search Directory"), true, [](const std::string& dir) {
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
    });
  }

  for (const auto& it : s_state.game_list_directories_cache)
  {
    if (MenuButton(SmallString::from_format(ICON_FA_FOLDER " {}", it.first),
                   it.second ? FSUI_VSTR("Scanning Subdirectories") : FSUI_VSTR("Not Scanning Subdirectories")))
    {
      ImGuiFullscreen::ChoiceDialogOptions options = {
        {FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Open in File Browser"), false},
        {it.second ? (FSUI_ICONSTR(ICON_FA_FOLDER_MINUS, "Disable Subdirectory Scanning")) :
                     (FSUI_ICONSTR(ICON_FA_FOLDER_PLUS, "Enable Subdirectory Scanning")),
         false},
        {FSUI_ICONSTR(ICON_FA_XMARK, "Remove From List"), false},
        {FSUI_ICONSTR(ICON_FA_SQUARE_XMARK, "Close Menu"), false},
      };

      OpenChoiceDialog(it.first.c_str(), false, std::move(options),
                       [dir = it.first, recursive = it.second](s32 index, const std::string& title, bool checked) {
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
                       });
    }
  }

  MenuHeading(FSUI_VSTR("Cover Settings"));
  {
    DrawFolderSetting(bsi, FSUI_ICONVSTR(ICON_FA_FOLDER, "Covers Directory"), "Folders", "Covers", EmuFolders::Covers);
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_DOWNLOAD, "Download Covers"),
                   FSUI_VSTR("Downloads covers from a user-specified URL template.")))
    {
      OpenFixedPopupDialog(COVER_DOWNLOADER_DIALOG_NAME);
    }
  }

  MenuHeading(FSUI_VSTR("Operations"));
  {
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_MAGNIFYING_GLASS, "Scan For New Games"),
                   FSUI_VSTR("Identifies any new files added to the game directories.")))
    {
      Host::RefreshGameListAsync(false);
    }
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_ARROWS_ROTATE, "Rescan All Games"),
                   FSUI_VSTR("Forces a full rescan of all games previously identified.")))
    {
      Host::RefreshGameListAsync(true);
    }
  }

  EndMenuButtons();
}

void FullscreenUI::DrawCoverDownloaderWindow()
{
  static char template_urls[512];
  static bool use_serial_names;

  if (!BeginFixedPopupDialog(LayoutScale(LAYOUT_LARGE_POPUP_PADDING), LayoutScale(LAYOUT_LARGE_POPUP_ROUNDING),
                             LayoutScale(1000.0f, 0.0f)))
  {
    return;
  }

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));
  ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.NormalFontWeight);

  ImGui::TextWrapped(
    "%s",
    FSUI_CSTR("DuckStation can automatically download covers for games which do not currently have a cover set. We "
              "do not host any cover images, the user must provide their own source for images."));
  ImGui::NewLine();
  ImGui::TextWrapped("%s",
                     FSUI_CSTR("In the form below, specify the URLs to download covers from, with one template URL "
                               "per line. The following variables are available:"));
  ImGui::NewLine();
  ImGui::TextWrapped("%s", FSUI_CSTR("${title}: Title of the game.\n${filetitle}: Name component of the game's "
                                     "filename.\n${serial}: Serial of the game."));
  ImGui::NewLine();
  ImGui::TextWrapped("%s", FSUI_CSTR("Example: https://www.example-not-a-real-domain.com/covers/${serial}.jpg"));
  ImGui::NewLine();

  ImGui::InputTextMultiline("##templates", template_urls, sizeof(template_urls),
                            ImVec2(ImGui::GetCurrentWindow()->WorkRect.GetWidth(), LayoutScale(175.0f)));

  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(5.0f));

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(2.0f, 2.0f));
  ImGui::Checkbox(FSUI_CSTR("Save as Serial File Names"), &use_serial_names);
  ImGui::PopStyleVar(1);

  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));

  const bool download_enabled = (std::strlen(template_urls) > 0);

  BeginHorizontalMenuButtons(2, 200.0f);

  if (HorizontalMenuButton(FSUI_ICONSTR(ICON_FA_DOWNLOAD, "Start Download"), download_enabled))
  {
    // TODO: Remove release once using move_only_function
    std::unique_ptr<ProgressCallback> progress =
      ImGuiFullscreen::OpenModalProgressDialog(FSUI_STR("Cover Downloader"), 1000.0f);
    System::QueueAsyncTask([progress = progress.release(), urls = StringUtil::SplitNewString(template_urls, '\n'),
                            use_serial_names = use_serial_names]() {
      GameList::DownloadCovers(
        urls, use_serial_names, progress, [](const GameList::Entry* entry, std::string save_path) {
          // cache the cover path on our side once it's saved
          Host::RunOnCPUThread([path = entry->path, save_path = std::move(save_path)]() mutable {
            GPUThread::RunOnThread([path = std::move(path), save_path = std::move(save_path)]() mutable {
              s_state.cover_image_map[std::move(path)] = std::move(save_path);
            });
          });
        });

      // close the parent window if we weren't cancelled
      if (!progress->IsCancelled())
      {
        Host::RunOnCPUThread([]() {
          GPUThread::RunOnThread([]() {
            if (IsFixedPopupDialogOpen(COVER_DOWNLOADER_DIALOG_NAME))
              CloseFixedPopupDialog();
          });
        });
      }

      delete progress;
    });
  }

  if (HorizontalMenuButton(FSUI_ICONSTR(ICON_FA_XMARK, "Close")))
    CloseFixedPopupDialog();

  EndHorizontalMenuButtons();

  ImGui::PopFont();
  ImGui::PopStyleVar(2);

  EndFixedPopupDialog();
}

void FullscreenUI::SwitchToGameList()
{
  s_state.game_list_view =
    static_cast<GameListView>(Host::GetBaseIntSettingValue("Main", "DefaultFullscreenUIGameView", 0));
  s_state.game_list_show_trophy_icons = Host::GetBaseBoolSettingValue("Main", "FullscreenUIShowTrophyIcons", true);
  s_state.game_grid_show_titles = Host::GetBaseBoolSettingValue("Main", "FullscreenUIShowGridTitles", true);
  s_state.game_list_current_selection_path = {};
  s_state.game_list_current_selection_timeout = 0.0f;

  // Wipe icon map, because a new save might give us an icon.
  for (const auto& it : s_state.icon_image_map)
  {
    if (!it.second.empty())
      ImGuiFullscreen::InvalidateCachedTexture(it.second);
  }
  s_state.icon_image_map.clear();

  SwitchToMainWindow(MainWindowType::GameList);
}

GPUTexture* FullscreenUI::GetGameListCover(const GameList::Entry* entry, bool fallback_to_achievements_icon,
                                           bool fallback_to_icon)
{
  // lookup and grab cover image
  auto cover_it = s_state.cover_image_map.find(entry->path);
  if (cover_it == s_state.cover_image_map.end())
  {
    std::string cover_path = GameList::GetCoverImagePathForEntry(entry);
    cover_it = s_state.cover_image_map.emplace(entry->path, std::move(cover_path)).first;

    // try achievements image before memcard icon
    if (fallback_to_achievements_icon && cover_it->second.empty() && Achievements::IsActive())
    {
      const auto lock = Achievements::GetLock();
      if (Achievements::GetGamePath() == entry->path)
        cover_it->second = Achievements::GetGameIconPath();
    }

    // because memcard icons are crap res
    if (fallback_to_icon && cover_it->second.empty())
      cover_it->second = GameList::GetGameIconPath(entry->serial, entry->path);
  }

  GPUTexture* tex = (!cover_it->second.empty()) ? GetCachedTextureAsync(cover_it->second.c_str()) : nullptr;
  return tex ? tex : GetTextureForGameListEntryType(entry->type);
}

GPUTexture* FullscreenUI::GetGameListCoverTrophy(const GameList::Entry* entry, const ImVec2& image_size)
{
  if (!s_state.game_list_show_trophy_icons || entry->num_achievements == 0)
    return nullptr;

  // this'll get re-scaled up, so undo layout scale
  const ImVec2 trophy_size = LayoutUnscale(image_size / 6.0f);

  GPUTexture* texture =
    GetCachedTextureAsync(entry->AreAchievementsMastered() ? "images/trophy-icon-star.svg" : "images/trophy-icon.svg",
                          static_cast<u32>(trophy_size.x), static_cast<u32>(trophy_size.y));

  // don't draw the placeholder, it's way too large
  return (texture == GetPlaceholderTexture().get()) ? nullptr : texture;
}

std::string_view FullscreenUI::GetKeyForGameListEntry(const GameList::Entry* entry)
{
  return entry->IsDiscSet() ? entry->GetDiscSetEntry()->GetSaveTitle() : std::string_view(entry->path);
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

GPUTexture* FullscreenUI::GetCoverForCurrentGame(const std::string& game_path)
{
  auto lock = GameList::GetLock();

  const GameList::Entry* entry = GameList::GetEntryForPath(game_path);
  if (!entry)
    return s_state.fallback_disc_texture.get();

  return GetGameListCover(entry, true, true);
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
    GPUThread::RunOnThread([]() {
      if (!Initialize())
        return;

      ShowToast(std::string(), Achievements::IsActive() ? FSUI_STR("This game has no achievements.") :
                                                          FSUI_STR("Achievements are not enabled."));
    });
    return;
  }

  GPUThread::RunOnThread([]() {
    if (!Initialize())
      return;

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
    GPUThread::RunOnThread([]() {
      if (!Initialize())
        return;

      ShowToast(std::string(), Achievements::IsActive() ? FSUI_STR("This game has no leaderboards.") :
                                                          FSUI_STR("Achievements are not enabled."));
    });
    return;
  }

  GPUThread::RunOnThread([]() {
    if (!Initialize())
      return;

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
  ImGuiFullscreen::OpenBackgroundProgressDialog(m_name.c_str(), "", 0, 100, 0);
}

FullscreenUI::BackgroundProgressCallback::~BackgroundProgressCallback()
{
  ImGuiFullscreen::CloseBackgroundProgressDialog(m_name.c_str());
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
  ImGuiFullscreen::UpdateBackgroundProgressDialog(m_name.c_str(), m_status_text, 0, 100, percent);
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

LoadingScreenProgressCallback::LoadingScreenProgressCallback()
  : ProgressCallback(), m_open_time(Timer::GetCurrentValue()), m_on_gpu_thread(GPUThread::IsOnThread())
{
  m_image = System::GetImageForLoadingScreen(m_on_gpu_thread ? GPUThread::GetGamePath() : System::GetGamePath());
}

LoadingScreenProgressCallback::~LoadingScreenProgressCallback()
{
  Close();
}

void LoadingScreenProgressCallback::Close()
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
    // since this was pushing frames, we need to restore the context. do that by pushing a frame ourselves
    GPUThread::Internal::PresentFrameAndRestoreContext();
  }

  m_last_progress_percent = -1;
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
    ImGuiFullscreen::RenderLoadingScreen(m_image, m_status_text, 0, static_cast<s32>(m_progress_range),
                                         static_cast<s32>(m_progress_value));
  }
  else
  {
    GPUThread::RunOnThread([image = std::move(m_image), status_text = SmallString(std::string_view(m_status_text)),
                            range = static_cast<s32>(m_progress_range), value = static_cast<s32>(m_progress_value)]() {
      ImGuiFullscreen::OpenOrUpdateLoadingScreen(ImGuiManager::LOGO_IMAGE_NAME, status_text, 0, range, value);
    });
    m_image = {};
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
// TRANSLATION-STRING-AREA-END
#endif
