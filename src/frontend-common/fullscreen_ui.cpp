#define IMGUI_DEFINE_MATH_OPERATORS

#include "fullscreen_ui.h"
#include "IconsFontAwesome5.h"
#include "cheevos.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/lru_cache.h"
#include "common/make_array.h"
#include "common/string.h"
#include "common/string_util.h"
#include "common_host_interface.h"
#include "controller_interface.h"
#include "core/cheats.h"
#include "core/cpu_core.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/host_interface_progress_callback.h"
#include "core/resources.h"
#include "core/settings.h"
#include "core/system.h"
#include "fullscreen_ui_progress_callback.h"
#include "game_list.h"
#include "icon.h"
#include "imgui.h"
#include "imgui_fullscreen.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "imgui_styles.h"
#include "scmversion/scmversion.h"
#include <bitset>
#include <thread>
Log_SetChannel(FullscreenUI);

static constexpr float LAYOUT_MAIN_MENU_BAR_SIZE = 20.0f; // Should be DPI scaled, not layout scaled!

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

using ImGuiFullscreen::ActiveButton;
using ImGuiFullscreen::BeginFullscreenColumns;
using ImGuiFullscreen::BeginFullscreenColumnWindow;
using ImGuiFullscreen::BeginFullscreenWindow;
using ImGuiFullscreen::BeginMenuButtons;
using ImGuiFullscreen::BeginNavBar;
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
using ImGuiFullscreen::LayoutScale;
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
using ImGuiFullscreen::RangeButton;
using ImGuiFullscreen::RightAlignNavButtons;
using ImGuiFullscreen::ToggleButton;

namespace FullscreenUI {

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////
static void LoadSettings();
static void ClearImGuiFocus();
static void ReturnToMainWindow();
static void DrawLandingWindow();
static void DrawQuickMenu(MainWindowType type);
static void DrawAchievementWindow();
static void DrawLeaderboardsWindow();
static void DrawDebugMenu();
static void DrawAboutWindow();
static void OpenAboutWindow();
static void SetDebugMenuEnabled(bool enabled);
static void UpdateDebugMenuVisibility();

static ALWAYS_INLINE bool IsCheevosHardcoreModeActive()
{
#ifdef WITH_CHEEVOS
  return Cheevos::IsChallengeModeActive();
#else
  return false;
#endif
}

static CommonHostInterface* s_host_interface;
static MainWindowType s_current_main_window = MainWindowType::Landing;
static std::bitset<static_cast<u32>(FrontendCommon::ControllerNavigationButton::Count)> s_nav_input_values{};
static bool s_debug_menu_enabled = false;
static bool s_debug_menu_allowed = false;
static bool s_quick_menu_was_open = false;
static bool s_was_paused_on_quick_menu_open = false;
static bool s_about_window_open = false;
static u32 s_close_button_state = 0;
static std::optional<u32> s_open_leaderboard_id;

//////////////////////////////////////////////////////////////////////////
// Resources
//////////////////////////////////////////////////////////////////////////
static bool LoadResources();
static void DestroyResources();

static HostDisplayTexture* GetCachedTexture(const std::string& name);
static ImTextureID ResolveTextureHandle(const std::string& name);

static std::unique_ptr<HostDisplayTexture> s_app_icon_texture;
static std::unique_ptr<HostDisplayTexture> s_placeholder_texture;
static std::array<std::unique_ptr<HostDisplayTexture>, static_cast<u32>(DiscRegion::Count)> s_disc_region_textures;
static std::array<std::unique_ptr<HostDisplayTexture>, static_cast<u32>(GameListCompatibilityRating::Count)>
  s_game_compatibility_textures;
static std::unique_ptr<HostDisplayTexture> s_fallback_disc_texture;
static std::unique_ptr<HostDisplayTexture> s_fallback_exe_texture;
static std::unique_ptr<HostDisplayTexture> s_fallback_psf_texture;
static std::unique_ptr<HostDisplayTexture> s_fallback_playlist_texture;
static LRUCache<std::string, std::unique_ptr<HostDisplayTexture>> s_texture_cache;

//////////////////////////////////////////////////////////////////////////
// Settings
//////////////////////////////////////////////////////////////////////////

enum class InputBindingType
{
  None,
  Button,
  Axis,
  HalfAxis,
  Rumble
};

static constexpr double INPUT_BINDING_TIMEOUT_SECONDS = 5.0;

static void DrawSettingsWindow();
static void BeginInputBinding(InputBindingType type, const std::string_view& section, const std::string_view& key,
                              const std::string_view& display_name);
static void EndInputBinding();
static void ClearInputBinding(const char* section, const char* key);
static void DrawInputBindingWindow();

static SettingsPage s_settings_page = SettingsPage::InterfaceSettings;
static Settings s_settings_copy;
static InputBindingType s_input_binding_type = InputBindingType::None;
static TinyString s_input_binding_section;
static TinyString s_input_binding_key;
static TinyString s_input_binding_display_name;
static bool s_input_binding_keyboard_pressed;
static Common::Timer s_input_binding_timer;

//////////////////////////////////////////////////////////////////////////
// Save State List
//////////////////////////////////////////////////////////////////////////
struct SaveStateListEntry
{
  std::string title;
  std::string summary;
  std::string path;
  std::string media_path;
  std::unique_ptr<HostDisplayTexture> preview_texture;
  s32 slot;
  bool global;
};

static void InitializePlaceholderSaveStateListEntry(SaveStateListEntry* li, s32 slot, bool global);
static void InitializeSaveStateListEntry(SaveStateListEntry* li, CommonHostInterface::ExtendedSaveStateInfo* ssi);
static void PopulateSaveStateListEntries();
static void OpenSaveStateSelector(bool is_loading);
static void CloseSaveStateSelector();
static void DrawSaveStateSelector(bool is_loading, bool fullscreen);

static std::vector<SaveStateListEntry> s_save_state_selector_slots;
static bool s_save_state_selector_open = false;
static bool s_save_state_selector_loading = true;

//////////////////////////////////////////////////////////////////////////
// Game List
//////////////////////////////////////////////////////////////////////////
static void DrawGameListWindow();
static void SwitchToGameList();
static void SortGameList();
static HostDisplayTexture* GetTextureForGameListEntryType(GameListEntryType type);
static HostDisplayTexture* GetGameListCover(const GameListEntry* entry);
static HostDisplayTexture* GetCoverForCurrentGame();

// Lazily populated cover images.
static std::unordered_map<std::string, std::string> s_cover_image_map;
static std::vector<const GameListEntry*> s_game_list_sorted_entries;
static std::thread s_game_list_load_thread;

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////

bool Initialize(CommonHostInterface* host_interface)
{
  s_host_interface = host_interface;
  s_texture_cache.SetMaxCapacity(128);
  if (!LoadResources())
    return false;

  s_settings_copy.Load(*s_host_interface->GetSettingsInterface());
  LoadSettings();
  UpdateDebugMenuVisibility();

  ImGuiFullscreen::UpdateLayoutScale();
  ImGuiFullscreen::UpdateFonts();
  ImGuiFullscreen::SetResolveTextureFunction(ResolveTextureHandle);

  if (System::IsValid())
    SystemCreated();

  return true;
}

bool IsInitialized()
{
  return (s_host_interface != nullptr);
}

bool HasActiveWindow()
{
  return s_current_main_window != MainWindowType::None || s_save_state_selector_open ||
         ImGuiFullscreen::IsChoiceDialogOpen() || ImGuiFullscreen::IsFileSelectorOpen();
}

void LoadSettings() {}

void UpdateSettings()
{
  LoadSettings();
}

void SystemCreated()
{
  s_current_main_window = MainWindowType::None;
  ClearImGuiFocus();
}

void SystemDestroyed()
{
  s_current_main_window = MainWindowType::Landing;
  s_quick_menu_was_open = false;
  ClearImGuiFocus();
}

static void PauseForMenuOpen()
{
  s_was_paused_on_quick_menu_open = System::IsPaused();
  if (s_settings_copy.pause_on_menu && !s_was_paused_on_quick_menu_open)
    s_host_interface->RunLater([]() { s_host_interface->PauseSystem(true); });

  s_quick_menu_was_open = true;
}

void OpenQuickMenu()
{
  if (!System::IsValid() || s_current_main_window != MainWindowType::None)
    return;

  PauseForMenuOpen();

  s_current_main_window = MainWindowType::QuickMenu;
  ClearImGuiFocus();
}

void CloseQuickMenu()
{
  if (!System::IsValid())
    return;

  if (System::IsPaused() && !s_was_paused_on_quick_menu_open)
    s_host_interface->RunLater([]() { s_host_interface->PauseSystem(false); });

  s_current_main_window = MainWindowType::None;
  s_quick_menu_was_open = false;
  ClearImGuiFocus();
}

#ifdef WITH_CHEEVOS

bool OpenAchievementsWindow()
{
  const bool achievements_enabled = Cheevos::HasActiveGame() && (Cheevos::GetAchievementCount() > 0);
  if (!achievements_enabled)
    return false;

  if (!s_quick_menu_was_open)
    PauseForMenuOpen();

  s_current_main_window = MainWindowType::Achievements;
  return true;
}

bool OpenLeaderboardsWindow()
{
  const bool leaderboards_enabled = Cheevos::HasActiveGame() && (Cheevos::GetLeaderboardCount() > 0);
  if (!leaderboards_enabled)
    return false;

  if (!s_quick_menu_was_open)
    PauseForMenuOpen();

  s_current_main_window = MainWindowType::Leaderboards;
  s_open_leaderboard_id.reset();
  return true;
}

#endif

void Shutdown()
{
  if (s_game_list_load_thread.joinable())
    s_game_list_load_thread.join();

  CloseSaveStateSelector();
  s_cover_image_map.clear();
  s_nav_input_values = {};
  DestroyResources();

  s_host_interface = nullptr;
}

void Render()
{
  if (s_debug_menu_enabled)
    DrawDebugMenu();

  if (System::IsValid())
  {
    if (!s_debug_menu_enabled)
      s_host_interface->DrawStatsOverlay();

    if (!IsCheevosHardcoreModeActive())
      s_host_interface->DrawDebugWindows();
  }

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
    case MainWindowType::QuickMenu:
      DrawQuickMenu(s_current_main_window);
      break;
    case MainWindowType::Achievements:
      DrawAchievementWindow();
      break;
    case MainWindowType::Leaderboards:
      DrawLeaderboardsWindow();
      break;
    default:
      break;
  }

  if (s_save_state_selector_open)
    DrawSaveStateSelector(s_save_state_selector_loading, false);

  if (s_about_window_open)
    DrawAboutWindow();

  if (s_input_binding_type != InputBindingType::None)
    DrawInputBindingWindow();

  s_host_interface->DrawOSDMessages();

  ImGuiFullscreen::EndLayout();
}

Settings& GetSettingsCopy()
{
  return s_settings_copy;
}

void SaveAndApplySettings()
{
  s_settings_copy.Save(*s_host_interface->GetSettingsInterface());
  s_host_interface->GetSettingsInterface()->Save();
  s_host_interface->ApplySettings(false);
  UpdateSettings();
}

void ClearImGuiFocus()
{
  ImGui::SetWindowFocus(nullptr);
  s_close_button_state = 0;
}

void ReturnToMainWindow()
{
  if (s_quick_menu_was_open)
    CloseQuickMenu();

  s_current_main_window = System::IsValid() ? MainWindowType::None : MainWindowType::Landing;
}

bool LoadResources()
{
  if (!(s_app_icon_texture = LoadTextureResource("logo.png", false)) &&
      !(s_app_icon_texture = LoadTextureResource("duck.png")))
  {
    return false;
  }

  if (!(s_placeholder_texture = s_host_interface->GetDisplay()->CreateTexture(
          PLACEHOLDER_ICON_WIDTH, PLACEHOLDER_ICON_HEIGHT, 1, 1, 1, HostDisplayPixelFormat::RGBA8,
          PLACEHOLDER_ICON_DATA, sizeof(u32) * PLACEHOLDER_ICON_WIDTH, false)))
  {
    return false;
  }

  if (!(s_disc_region_textures[static_cast<u32>(DiscRegion::NTSC_U)] = LoadTextureResource("flag-uc.png")) ||
      !(s_disc_region_textures[static_cast<u32>(DiscRegion::NTSC_J)] = LoadTextureResource("flag-jp.png")) ||
      !(s_disc_region_textures[static_cast<u32>(DiscRegion::PAL)] = LoadTextureResource("flag-eu.png")) ||
      !(s_disc_region_textures[static_cast<u32>(DiscRegion::Other)] = LoadTextureResource("flag-eu.png")) ||
      !(s_fallback_disc_texture = LoadTextureResource("media-cdrom.png")) ||
      !(s_fallback_exe_texture = LoadTextureResource("applications-system.png")) ||
      !(s_fallback_psf_texture = LoadTextureResource("multimedia-player.png")) ||
      !(s_fallback_playlist_texture = LoadTextureResource("address-book-new.png")))
  {
    return false;
  }

  for (u32 i = 0; i < static_cast<u32>(GameListCompatibilityRating::Count); i++)
  {
    if (!(s_game_compatibility_textures[i] = LoadTextureResource(TinyString::FromFormat("star-%u.png", i))))
      return false;
  }

  return true;
}

void DestroyResources()
{
  s_texture_cache.Clear();
  s_app_icon_texture.reset();
  s_placeholder_texture.reset();
  s_fallback_playlist_texture.reset();
  s_fallback_psf_texture.reset();
  s_fallback_exe_texture.reset();
  s_fallback_disc_texture.reset();
  for (auto& tex : s_game_compatibility_textures)
    tex.reset();
  for (auto& tex : s_disc_region_textures)
    tex.reset();
}

static std::unique_ptr<HostDisplayTexture> LoadTexture(const char* path, bool from_package)
{
  std::unique_ptr<ByteStream> stream;
  if (from_package)
    stream = g_host_interface->OpenPackageFile(path, BYTESTREAM_OPEN_READ);
  else
    stream = FileSystem::OpenFile(path, BYTESTREAM_OPEN_READ);
  if (!stream)
  {
    Log_ErrorPrintf("Failed to open texture resource '%s'", path);
    return {};
  }

  Common::RGBA8Image image;
  if (!Common::LoadImageFromStream(&image, stream.get()) && image.IsValid())
  {
    Log_ErrorPrintf("Failed to read texture resource '%s'", path);
    return {};
  }

  std::unique_ptr<HostDisplayTexture> texture = g_host_interface->GetDisplay()->CreateTexture(
    image.GetWidth(), image.GetHeight(), 1, 1, 1, HostDisplayPixelFormat::RGBA8, image.GetPixels(),
    image.GetByteStride());
  if (!texture)
  {
    Log_ErrorPrintf("failed to create %ux%u texture for resource", image.GetWidth(), image.GetHeight());
    return {};
  }

  Log_DevPrintf("Uploaded texture resource '%s' (%ux%u)", path, image.GetWidth(), image.GetHeight());
  return texture;
}

std::unique_ptr<HostDisplayTexture> LoadTextureResource(const char* name, bool allow_fallback /*= true*/)
{
  const std::string path(StringUtil::StdStringFromFormat("resources" FS_OSPATH_SEPARATOR_STR "%s", name));
  std::unique_ptr<HostDisplayTexture> texture = LoadTexture(path.c_str(), true);
  if (texture)
    return texture;

  if (!allow_fallback)
    return nullptr;

  Log_ErrorPrintf("Missing resource '%s', using fallback", name);

  texture = g_host_interface->GetDisplay()->CreateTexture(PLACEHOLDER_ICON_WIDTH, PLACEHOLDER_ICON_HEIGHT, 1, 1, 1,
                                                          HostDisplayPixelFormat::RGBA8, PLACEHOLDER_ICON_DATA,
                                                          sizeof(u32) * PLACEHOLDER_ICON_WIDTH, false);
  if (!texture)
    Panic("Failed to create placeholder texture");

  return texture;
}

HostDisplayTexture* GetCachedTexture(const std::string& name)
{
  std::unique_ptr<HostDisplayTexture>* tex_ptr = s_texture_cache.Lookup(name);
  if (!tex_ptr)
  {
    std::unique_ptr<HostDisplayTexture> tex = LoadTexture(name.c_str(), false);
    tex_ptr = s_texture_cache.Insert(name, std::move(tex));
  }

  return tex_ptr->get();
}

ImTextureID ResolveTextureHandle(const std::string& name)
{
  HostDisplayTexture* tex = GetCachedTexture(name);
  return tex ? tex->GetHandle() : nullptr;
}

bool InvalidateCachedTexture(const std::string& path)
{
  return s_texture_cache.Remove(path);
}

//////////////////////////////////////////////////////////////////////////
// Utility
//////////////////////////////////////////////////////////////////////////

static ImGuiFullscreen::FileSelectorFilters GetDiscImageFilters()
{
  return {"*.bin",   "*.cue", "*.iso", "*.img",     "*.chd", "*.ecm", "*.mds",
          "*.psexe", "*.exe", "*.psf", "*.minipsf", "*.m3u", "*.pbp"};
}

static void DoStartPath(const std::string& path, bool allow_resume)
{
  // we can never resume from exe/psf, or when challenge mode is active
  if (System::IsExeFileName(path.c_str()) || System::IsPsfFileName(path.c_str()) ||
      s_host_interface->IsCheevosChallengeModeActive())
    allow_resume = false;

  if (allow_resume && g_settings.save_state_on_exit)
  {
    s_host_interface->RunLater([path]() { s_host_interface->ResumeSystemFromState(path.c_str(), true); });
  }
  else
  {
    auto params = std::make_shared<SystemBootParameters>(path);
    s_host_interface->RunLater([params]() { s_host_interface->BootSystem(std::move(params)); });
  }
}

static void DoStartFile()
{
  auto callback = [](const std::string& path) {
    if (!path.empty())
      DoStartPath(path, false);

    ClearImGuiFocus();
    CloseFileSelector();
  };

  OpenFileSelector(ICON_FA_COMPACT_DISC "  Select Disc Image", false, std::move(callback), GetDiscImageFilters());
}

static void DoStartBIOS()
{
  s_host_interface->RunLater([]() { s_host_interface->BootSystem(std::make_shared<SystemBootParameters>()); });
  ClearImGuiFocus();
}

static void DoPowerOff()
{
  s_host_interface->RunLater([]() {
    if (!System::IsValid())
      return;

    s_host_interface->PowerOffSystem(s_host_interface->ShouldSaveResumeState());

    ReturnToMainWindow();
  });
  ClearImGuiFocus();
}

static void DoReset()
{
  s_host_interface->RunLater([]() {
    if (!System::IsValid())
      return;

    s_host_interface->ResetSystem();
  });
}

static void DoPause()
{
  s_host_interface->RunLater([]() {
    if (!System::IsValid())
      return;

    s_host_interface->PauseSystem(!System::IsPaused());
  });
}

static void DoCheatsMenu()
{
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
}

static void DoToggleAnalogMode()
{
  // hacky way to toggle analog mode
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* ctrl = System::GetController(i);
    if (!ctrl)
      continue;

    std::optional<s32> code = Controller::GetButtonCodeByName(ctrl->GetType(), "Analog");
    if (!code.has_value())
      continue;

    ctrl->SetButtonState(code.value(), true);
    ctrl->SetButtonState(code.value(), false);
  }
}

static void DoChangeDiscFromFile()
{
  auto callback = [](const std::string& path) {
    if (!path.empty())
      System::InsertMedia(path.c_str());

    ClearImGuiFocus();
    CloseFileSelector();
    ReturnToMainWindow();
  };

  OpenFileSelector(ICON_FA_COMPACT_DISC "  Select Disc Image", false, std::move(callback), GetDiscImageFilters(),
                   std::string(FileSystem::GetPathDirectory(System::GetMediaFileName())));
}

static void DoChangeDisc()
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

    ClearImGuiFocus();
    CloseChoiceDialog();
    ReturnToMainWindow();
  };

  OpenChoiceDialog(ICON_FA_COMPACT_DISC "  Select Disc Image", true, std::move(options), std::move(callback));
}

//////////////////////////////////////////////////////////////////////////
// Landing Window
//////////////////////////////////////////////////////////////////////////

void DrawLandingWindow()
{
  BeginFullscreenColumns();

  if (BeginFullscreenColumnWindow(0.0f, 570.0f, "logo", ImVec4(0.11f, 0.15f, 0.17f, 1.00f)))
  {
    const float image_size = LayoutScale(380.f);
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() * 0.5f) - (image_size * 0.5f),
                               (ImGui::GetWindowHeight() * 0.5f) - (image_size * 0.5f)));
    ImGui::Image(s_app_icon_texture->GetHandle(), ImVec2(image_size, image_size));
  }
  EndFullscreenColumnWindow();

  if (BeginFullscreenColumnWindow(570.0f, LAYOUT_SCREEN_WIDTH, "menu"))
  {
    BeginMenuButtons(7, 0.5f);

    if (MenuButton(" " ICON_FA_PLAY_CIRCLE "  Resume",
                   "Starts the console from where it was before it was last closed.", !IsCheevosHardcoreModeActive()))
    {
      s_host_interface->RunLater([]() { s_host_interface->ResumeSystemFromMostRecentState(); });
      ClearImGuiFocus();
    }

    if (MenuButton(" " ICON_FA_LIST "  Open Game List",
                   "Launch a game from images scanned from your game directories."))
    {
      s_host_interface->RunLater(SwitchToGameList);
    }

    if (MenuButton(" " ICON_FA_FOLDER_OPEN "  Start File", "Launch a game by selecting a file/disc image."))
      s_host_interface->RunLater(DoStartFile);

    if (MenuButton(" " ICON_FA_TOOLBOX "  Start BIOS", "Start the console without any disc inserted."))
      s_host_interface->RunLater(DoStartBIOS);

    if (MenuButton(" " ICON_FA_UNDO "  Load State", "Loads a global save state.", !IsCheevosHardcoreModeActive()))
    {
      OpenSaveStateSelector(true);
    }

    if (MenuButton(" " ICON_FA_SLIDERS_H "  Settings", "Change settings for the emulator."))
      s_current_main_window = MainWindowType::Settings;

    if (MenuButton(" " ICON_FA_SIGN_OUT_ALT "  Exit", "Exits the program."))
      s_host_interface->RequestExit();

    {
      ImVec2 fullscreen_pos;
      if (FloatingButton(ICON_FA_WINDOW_CLOSE, 0.0f, 0.0f, -1.0f, -1.0f, 1.0f, 0.0f, true, g_large_font,
                         &fullscreen_pos))
      {
        s_host_interface->RequestExit();
      }

      if (FloatingButton(ICON_FA_EXPAND, fullscreen_pos.x, 0.0f, -1.0f, -1.0f, -1.0f, 0.0f, true, g_large_font,
                         &fullscreen_pos))
      {
        s_host_interface->RunLater([]() { s_host_interface->SetFullscreen(!s_host_interface->IsFullscreen()); });
      }

      if (FloatingButton(ICON_FA_QUESTION_CIRCLE, fullscreen_pos.x, 0.0f, -1.0f, -1.0f, -1.0f, 0.0f))
        OpenAboutWindow();
    }

    EndMenuButtons();
  }

  EndFullscreenColumnWindow();

  EndFullscreenColumns();
}

static ImGuiFullscreen::ChoiceDialogOptions GetGameListDirectoryOptions(bool recursive_as_checked)
{
  ImGuiFullscreen::ChoiceDialogOptions options;

  for (std::string& dir : s_host_interface->GetSettingsInterface()->GetStringList("GameList", "Paths"))
    options.emplace_back(std::move(dir), false);

  for (std::string& dir : s_host_interface->GetSettingsInterface()->GetStringList("GameList", "RecursivePaths"))
    options.emplace_back(std::move(dir), recursive_as_checked);

  std::sort(options.begin(), options.end(), [](const auto& lhs, const auto& rhs) {
    return (StringUtil::Strcasecmp(lhs.first.c_str(), rhs.first.c_str()) < 0);
  });

  return options;
}

static void DrawInputBindingButton(InputBindingType type, const char* section, const char* name,
                                   const char* display_name, bool show_type = true)
{
  TinyString title;
  title.Format("%s/%s", section, name);

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
      case InputBindingType::Button:
        title.Format(ICON_FA_CIRCLE "  %s Button", display_name);
        break;
      case InputBindingType::Axis:
        title.Format(ICON_FA_BULLSEYE "  %s Axis", display_name);
        break;
      case InputBindingType::HalfAxis:
        title.Format(ICON_FA_SLIDERS_H "  %s Half-Axis", display_name);
        break;
      case InputBindingType::Rumble:
        title.Format(ICON_FA_BELL "  %s", display_name);
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

  // eek, potential heap allocation :/
  const std::string value = s_host_interface->GetSettingsInterface()->GetStringValue(section, name);
  ImGui::PushFont(g_medium_font);
  ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, value.empty() ? "(No Binding)" : value.c_str(), nullptr,
                           nullptr, ImVec2(0.0f, 0.0f), &summary_bb);
  ImGui::PopFont();

  if (clicked)
    BeginInputBinding(type, section, name, display_name);
  else if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    ClearInputBinding(section, name);
}

static void ClearInputBindingVariables()
{
  s_input_binding_type = InputBindingType::None;
  s_input_binding_section.Clear();
  s_input_binding_key.Clear();
  s_input_binding_display_name.Clear();
}

bool IsBindingInput()
{
  return s_input_binding_type != InputBindingType::None;
}

bool HandleKeyboardBinding(const char* keyName, bool pressed)
{
  if (s_input_binding_type == InputBindingType::None)
    return false;

  if (pressed)
  {
    s_input_binding_keyboard_pressed = true;
    return true;
  }

  if (!s_input_binding_keyboard_pressed)
    return false;

  TinyString value;
  value.Format("Keyboard/%s", keyName);

  {
    auto lock = s_host_interface->GetSettingsLock();
    s_host_interface->GetSettingsInterface()->SetStringValue(s_input_binding_section, s_input_binding_key, value);
  }

  EndInputBinding();
  s_host_interface->RunLater(SaveAndApplySettings);
  return true;
}

void BeginInputBinding(InputBindingType type, const std::string_view& section, const std::string_view& key,
                       const std::string_view& display_name)
{
  s_input_binding_type = type;
  s_input_binding_section = section;
  s_input_binding_key = key;
  s_input_binding_display_name = display_name;
  s_input_binding_timer.Reset();

  ControllerInterface* ci = s_host_interface->GetControllerInterface();
  if (ci)
  {
    auto callback = [](const ControllerInterface::Hook& hook) -> ControllerInterface::Hook::CallbackResult {
      // ignore if axis isn't at least halfway
      if (hook.type == ControllerInterface::Hook::Type::Axis && std::abs(std::get<float>(hook.value)) < 0.5f)
        return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;

      TinyString value;
      switch (s_input_binding_type)
      {
        case InputBindingType::Axis:
        {
          if (hook.type == ControllerInterface::Hook::Type::Axis)
            value.Format("Controller%d/Axis%d", hook.controller_index, hook.button_or_axis_number);
        }
        break;

        case InputBindingType::HalfAxis:
        {
          if (hook.type == ControllerInterface::Hook::Type::Axis)
          {
            value.Format("Controller%d/%cAxis%d", hook.controller_index,
                         (std::get<float>(hook.value) < 0.0f) ? '-' : '+', hook.button_or_axis_number);
          }
        }
        break;

        case InputBindingType::Button:
        {
          if (hook.type == ControllerInterface::Hook::Type::Axis)
            value.Format("Controller%d/+Axis%d", hook.controller_index, hook.button_or_axis_number);
          else if (hook.type == ControllerInterface::Hook::Type::Button && std::get<float>(hook.value) > 0.0f)
            value.Format("Controller%d/Button%d", hook.controller_index, hook.button_or_axis_number);
        }
        break;

        case InputBindingType::Rumble:
        {
          if (hook.type == ControllerInterface::Hook::Type::Button && std::get<float>(hook.value) > 0.0f)
            value.Format("Controller%d", hook.controller_index);
        }
        break;

        default:
          break;
      }

      if (value.IsEmpty())
        return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;

      {
        auto lock = s_host_interface->GetSettingsLock();
        s_host_interface->GetSettingsInterface()->SetStringValue(s_input_binding_section, s_input_binding_key, value);
      }

      ClearInputBindingVariables();
      s_host_interface->RunLater(SaveAndApplySettings);

      return ControllerInterface::Hook::CallbackResult::StopMonitoring;
    };
    ci->SetHook(std::move(callback));
  }
}

void EndInputBinding()
{
  ClearInputBindingVariables();

  ControllerInterface* ci = s_host_interface->GetControllerInterface();
  if (ci)
    ci->ClearHook();
}

void ClearInputBinding(const char* section, const char* key)
{
  {
    auto lock = s_host_interface->GetSettingsLock();
    s_host_interface->GetSettingsInterface()->DeleteValue(section, key);
  }

  s_host_interface->RunLater(SaveAndApplySettings);
}

void DrawInputBindingWindow()
{
  DebugAssert(s_input_binding_type != InputBindingType::None);

  const double time_remaining = INPUT_BINDING_TIMEOUT_SECONDS - s_input_binding_timer.GetTimeSeconds();
  if (time_remaining <= 0.0)
  {
    EndInputBinding();
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
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(10.0f, 10.0f));

  if (ImGui::BeginPopupModal(title, nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
  {
    ImGui::TextWrapped("Setting %s binding %s.", s_input_binding_section.GetCharArray(),
                       s_input_binding_display_name.GetCharArray());
    ImGui::TextUnformatted("Push a controller button or axis now.");
    ImGui::NewLine();
    ImGui::Text("Timing out in %.0f seconds...", time_remaining);
    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(3);
  ImGui::PopFont();
}

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
                         std::string(FileSystem::GetPathDirectory(std::move(value))));
      }

      return false;
    }

    default:
      return false;
  }
}

static bool ToggleButtonForNonSetting(const char* title, const char* summary, const char* section, const char* key,
                                      bool default_value, bool enabled = true,
                                      float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                      ImFont* font = g_large_font, ImFont* summary_font = g_medium_font)
{
  bool value = s_host_interface->GetSettingsInterface()->GetBoolValue(section, key, default_value);
  if (!ToggleButton(title, summary, &value, enabled, height, font, summary_font))
    return false;

  s_host_interface->GetSettingsInterface()->SetBoolValue(section, key, value);
  return true;
}

#ifdef WITH_CHEEVOS

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

static bool WantsToCloseMenu()
{
  // Wait for the Close button to be released, THEN pressed
  if (s_close_button_state == 0)
  {
    if (!ImGuiFullscreen::IsCancelButtonPressed())
      s_close_button_state = 1;
  }
  else if (s_close_button_state == 1)
  {
    if (ImGuiFullscreen::IsCancelButtonPressed())
    {
      s_close_button_state = 0;
      return true;
    }
  }
  return false;
}

void DrawSettingsWindow()
{
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY + LAYOUT_MENU_BUTTON_Y_PADDING * 2.0f + 2.0f));

  if (BeginFullscreenWindow(ImVec2(0.0f, ImGuiFullscreen::g_menu_bar_size), heading_size, "settings_category",
                            ImVec4(0.18f, 0.18f, 0.18f, 1.00f)))
  {
    static constexpr float ITEM_WIDTH = 22.0f;

    static constexpr std::array<const char*, static_cast<u32>(SettingsPage::Count)> icons = {
      {ICON_FA_WINDOW_MAXIMIZE, ICON_FA_LIST, ICON_FA_HDD, ICON_FA_SLIDERS_H, ICON_FA_MICROCHIP, ICON_FA_GAMEPAD,
       ICON_FA_KEYBOARD, ICON_FA_SD_CARD, ICON_FA_TV, ICON_FA_MAGIC, ICON_FA_HEADPHONES, ICON_FA_TROPHY,
       ICON_FA_EXCLAMATION_TRIANGLE}};

    static constexpr std::array<const char*, static_cast<u32>(SettingsPage::Count)> titles = {
      {"Interface Settings", "Game List Settings", "Console Settings", "Emulation Settings", "BIOS Settings",
       "Controller Settings", "Hotkey Settings", "Memory Card Settings", "Display Settings", "Enhancement Settings",
       "Audio Settings", "Achievements Settings", "Advanced Settings"}};

    BeginNavBar();

    if (ImGui::IsNavInputTest(ImGuiNavInput_FocusPrev, ImGuiInputReadMode_Pressed))
    {
      s_settings_page = static_cast<SettingsPage>((s_settings_page == static_cast<SettingsPage>(0)) ?
                                                    (static_cast<u32>(SettingsPage::Count) - 1) :
                                                    (static_cast<u32>(s_settings_page) - 1));
    }
    else if (ImGui::IsNavInputTest(ImGuiNavInput_FocusNext, ImGuiInputReadMode_Pressed))
    {
      s_settings_page =
        static_cast<SettingsPage>((static_cast<u32>(s_settings_page) + 1) % static_cast<u32>(SettingsPage::Count));
    }

    if (NavButton(ICON_FA_BACKWARD, false, true))
      ReturnToMainWindow();

    NavTitle(titles[static_cast<u32>(s_settings_page)]);

    RightAlignNavButtons(static_cast<u32>(titles.size()), ITEM_WIDTH, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    for (u32 i = 0; i < static_cast<u32>(titles.size()); i++)
    {
      if (NavButton(icons[i], s_settings_page == static_cast<SettingsPage>(i), true, ITEM_WIDTH,
                    LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
      {
        s_settings_page = static_cast<SettingsPage>(i);
      }
    }

    EndNavBar();
  }

  EndFullscreenWindow();

  if (BeginFullscreenWindow(
        ImVec2(0.0f, ImGuiFullscreen::g_menu_bar_size + heading_size.y),
        ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y - ImGuiFullscreen::g_menu_bar_size),
        "settings_parent"))
  {
    if (ImGui::IsNavInputTest(ImGuiNavInput_Cancel, ImGuiInputReadMode_Pressed))
    {
      if (ImGui::IsWindowFocused())
        ReturnToMainWindow();
    }

    bool settings_changed = false;

    switch (s_settings_page)
    {
      case SettingsPage::InterfaceSettings:
      {
        BeginMenuButtons();

        MenuHeading("Behavior");

        settings_changed |=
          ToggleButton("Pause On Start", "Pauses the emulator when a game is started.", &s_settings_copy.start_paused);
        settings_changed |= ToggleButton("Pause On Focus Loss",
                                         "Pauses the emulator when you minimize the window or switch to another "
                                         "application, and unpauses when you switch back.",
                                         &s_settings_copy.pause_on_focus_loss);
        settings_changed |= ToggleButton(
          "Pause On Menu", "Pauses the emulator when you open the quick menu, and unpauses when you close it.",
          &s_settings_copy.pause_on_menu);
        settings_changed |=
          ToggleButton("Confirm Power Off",
                       "Determines whether a prompt will be displayed to confirm shutting down the emulator/game "
                       "when the hotkey is pressed.",
                       &s_settings_copy.confim_power_off);
        settings_changed |=
          ToggleButton("Save State On Exit",
                       "Automatically saves the emulator state when powering down or exiting. You can then "
                       "resume directly from where you left off next time.",
                       &s_settings_copy.save_state_on_exit);
        settings_changed |=
          ToggleButton("Start Fullscreen", "Automatically switches to fullscreen mode when the program is started.",
                       &s_settings_copy.start_fullscreen);
        settings_changed |= ToggleButtonForNonSetting(
          "Hide Cursor In Fullscreen", "Hides the mouse pointer/cursor when the emulator is in fullscreen mode.",
          "Main", "HideCursorInFullscreen", true);
        settings_changed |= ToggleButton(
          "Inhibit Screensaver",
          "Prevents the screen saver from activating and the host from sleeping while emulation is running.",
          &s_settings_copy.inhibit_screensaver);
        settings_changed |=
          ToggleButton("Load Devices From Save States",
                       "When enabled, memory cards and controllers will be overwritten when save states are loaded.",
                       &s_settings_copy.load_devices_from_save_states);
        settings_changed |= ToggleButton(
          "Apply Per-Game Settings",
          "When enabled, per-game settings will be applied, and incompatible enhancements will be disabled.",
          &s_settings_copy.apply_game_settings);
        settings_changed |=
          ToggleButton("Automatically Load Cheats", "Automatically loads and applies cheats on game start.",
                       &s_settings_copy.auto_load_cheats);

#ifdef WITH_DISCORD_PRESENCE
        MenuHeading("Integration");
        settings_changed |= ToggleButtonForNonSetting(
          "Enable Discord Presence", "Shows the game you are currently playing as part of your profile on Discord.",
          "Main", "EnableDiscordPresence", false);
#endif

        MenuHeading("Miscellaneous");

        static ControllerInterface::Backend cbtype = ControllerInterface::Backend::None;
        static bool cbtype_set = false;
        if (!cbtype_set)
        {
          cbtype = ControllerInterface::ParseBackendName(
                     s_host_interface->GetSettingsInterface()->GetStringValue("Main", "ControllerBackend").c_str())
                     .value_or(ControllerInterface::GetDefaultBackend());
          cbtype_set = true;
        }

        if (EnumChoiceButton("Controller Backend", "Sets the API which is used to receive controller input.", &cbtype,
                             ControllerInterface::GetBackendName, ControllerInterface::Backend::Count))
        {
          s_host_interface->GetSettingsInterface()->SetStringValue("Main", "ControllerBackend",
                                                                   ControllerInterface::GetBackendName(cbtype));
          settings_changed = true;
        }

        EndMenuButtons();
      }
      break;

      case SettingsPage::GameListSettings:
      {
        BeginMenuButtons();

        MenuHeading("Game List");

        if (MenuButton(ICON_FA_FOLDER_PLUS "  Add Search Directory", "Adds a new directory to the game search list."))
        {
          OpenFileSelector(ICON_FA_FOLDER_PLUS "  Add Search Directory", true, [](const std::string& dir) {
            if (!dir.empty())
            {
              s_host_interface->GetSettingsInterface()->AddToStringList("GameList", "RecursivePaths", dir.c_str());
              s_host_interface->GetSettingsInterface()->RemoveFromStringList("GameList", "Paths", dir.c_str());
              s_host_interface->GetSettingsInterface()->Save();
              QueueGameListRefresh();
            }

            CloseFileSelector();
          });
        }

        if (MenuButton(ICON_FA_FOLDER_OPEN "  Change Recursive Directories",
                       "Sets whether subdirectories are searched for each game directory"))
        {
          OpenChoiceDialog(
            ICON_FA_FOLDER_OPEN "  Change Recursive Directories", true, GetGameListDirectoryOptions(true),
            [](s32 index, const std::string& title, bool checked) {
              if (index < 0)
                return;

              if (checked)
              {
                s_host_interface->GetSettingsInterface()->RemoveFromStringList("GameList", "Paths", title.c_str());
                s_host_interface->GetSettingsInterface()->AddToStringList("GameList", "RecursivePaths", title.c_str());
              }
              else
              {
                s_host_interface->GetSettingsInterface()->RemoveFromStringList("GameList", "RecursivePaths",
                                                                               title.c_str());
                s_host_interface->GetSettingsInterface()->AddToStringList("GameList", "Paths", title.c_str());
              }

              s_host_interface->GetSettingsInterface()->Save();
              QueueGameListRefresh();
            });
        }

        if (MenuButton(ICON_FA_FOLDER_MINUS "  Remove Search Directory",
                       "Removes a directory from the game search list."))
        {
          OpenChoiceDialog(ICON_FA_FOLDER_MINUS "  Remove Search Directory", false, GetGameListDirectoryOptions(false),
                           [](s32 index, const std::string& title, bool checked) {
                             if (index < 0)
                               return;

                             s_host_interface->GetSettingsInterface()->RemoveFromStringList("GameList", "Paths",
                                                                                            title.c_str());
                             s_host_interface->GetSettingsInterface()->RemoveFromStringList(
                               "GameList", "RecursivePaths", title.c_str());
                             s_host_interface->GetSettingsInterface()->Save();
                             QueueGameListRefresh();
                             CloseChoiceDialog();
                           });
        }

        MenuHeading("Search Directories");
        for (const GameList::DirectoryEntry& entry : s_host_interface->GetGameList()->GetSearchDirectories())
        {
          MenuButton(entry.path.c_str(), entry.recursive ? "Scanning Subdirectories" : "Not Scanning Subdirectories",
                     false);
        }

        EndMenuButtons();
      }
      break;

      case SettingsPage::ConsoleSettings:
      {
        static constexpr auto cdrom_read_speeds =
          make_array("None (Double Speed)", "2x (Quad Speed)", "3x (6x Speed)", "4x (8x Speed)", "5x (10x Speed)",
                     "6x (12x Speed)", "7x (14x Speed)", "8x (16x Speed)", "9x (18x Speed)", "10x (20x Speed)");

        static constexpr auto cdrom_seek_speeds = make_array("Infinite/Instantaneous", "None (Normal Speed)", "2x",
                                                             "3x", "4x", "5x", "6x", "7x", "8x", "9x", "10x");

        BeginMenuButtons();

        MenuHeading("Console Settings");

        settings_changed |=
          EnumChoiceButton("Region", "Determines the emulated hardware type.", &s_settings_copy.region,
                           &Settings::GetConsoleRegionDisplayName, ConsoleRegion::Count);

        MenuHeading("CPU Emulation (MIPS R3000A Derivative)");

        settings_changed |= EnumChoiceButton(
          "Execution Mode", "Determines how the emulated CPU executes instructions. Recompiler is recommended.",
          &s_settings_copy.cpu_execution_mode, &Settings::GetCPUExecutionModeDisplayName, CPUExecutionMode::Count);

        settings_changed |=
          ToggleButton("Enable Overclocking", "When this option is chosen, the clock speed set below will be used.",
                       &s_settings_copy.cpu_overclock_enable);

        s32 overclock_percent =
          s_settings_copy.cpu_overclock_enable ? static_cast<s32>(s_settings_copy.GetCPUOverclockPercent()) : 100;
        if (RangeButton("Overclocking Percentage",
                        "Selects the percentage of the normal clock speed the emulated hardware will run at.",
                        &overclock_percent, 10, 1000, 10, "%d%%", s_settings_copy.cpu_overclock_enable))
        {
          s_settings_copy.SetCPUOverclockPercent(static_cast<u32>(overclock_percent));
          settings_changed = true;
        }

        MenuHeading("CD-ROM Emulation");

        const u32 read_speed_index =
          std::clamp<u32>(s_settings_copy.cdrom_read_speedup, 1u, static_cast<u32>(cdrom_read_speeds.size())) - 1u;
        if (MenuButtonWithValue("Read Speedup",
                                "Speeds up CD-ROM reads by the specified factor. May improve loading speeds in some "
                                "games, and break others.",
                                cdrom_read_speeds[read_speed_index]))
        {
          ImGuiFullscreen::ChoiceDialogOptions options;
          options.reserve(cdrom_read_speeds.size());
          for (u32 i = 0; i < static_cast<u32>(cdrom_read_speeds.size()); i++)
            options.emplace_back(cdrom_read_speeds[i], i == read_speed_index);
          OpenChoiceDialog("CD-ROM Read Speedup", false, std::move(options),
                           [](s32 index, const std::string& title, bool checked) {
                             if (index >= 0)
                               s_settings_copy.cdrom_read_speedup = static_cast<u32>(index) + 1;
                             CloseChoiceDialog();
                           });
        }

        const u32 seek_speed_index =
          std::min(s_settings_copy.cdrom_seek_speedup, static_cast<u32>(cdrom_seek_speeds.size()));
        if (MenuButtonWithValue("Seek Speedup",
                                "Speeds up CD-ROM seeks by the specified factor. May improve loading speeds in some "
                                "games, and break others.",
                                cdrom_seek_speeds[seek_speed_index]))
        {
          ImGuiFullscreen::ChoiceDialogOptions options;
          options.reserve(cdrom_seek_speeds.size());
          for (u32 i = 0; i < static_cast<u32>(cdrom_seek_speeds.size()); i++)
            options.emplace_back(cdrom_seek_speeds[i], i == seek_speed_index);
          OpenChoiceDialog("CD-ROM Seek Speedup", false, std::move(options),
                           [](s32 index, const std::string& title, bool checked) {
                             if (index >= 0)
                               s_settings_copy.cdrom_seek_speedup = static_cast<u32>(index);
                             CloseChoiceDialog();
                           });
        }

        s32 readahead_sectors = s_settings_copy.cdrom_readahead_sectors;
        if (RangeButton(
              "Readahead Sectors",
              "Reduces hitches in emulation by reading/decompressing CD data asynchronously on a worker thread.",
              &readahead_sectors, 0, 32, 1))
        {
          s_settings_copy.cdrom_readahead_sectors = static_cast<u8>(readahead_sectors);
          settings_changed = true;
        }

        settings_changed |=
          ToggleButton("Enable Region Check", "Simulates the region check present in original, unmodified consoles.",
                       &s_settings_copy.cdrom_region_check);
        settings_changed |= ToggleButton(
          "Preload Images to RAM",
          "Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay.",
          &s_settings_copy.cdrom_load_image_to_ram);
        settings_changed |= ToggleButtonForNonSetting(
          "Apply Image Patches",
          "Automatically applies patches to disc images when they are present, currently only PPF is supported.",
          "CDROM", "LoadImagePatches", false);

        MenuHeading("Controller Ports");

        settings_changed |= EnumChoiceButton("Multitap", nullptr, &s_settings_copy.multitap_mode,
                                             &Settings::GetMultitapModeDisplayName, MultitapMode::Count);

        EndMenuButtons();
      }
      break;

      case SettingsPage::EmulationSettings:
      {
        static constexpr auto emulation_speeds =
          make_array(0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f,
                     3.0f, 3.5f, 4.0f, 4.5f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f);
        static constexpr auto get_emulation_speed_options = [](float current_speed) {
          ImGuiFullscreen::ChoiceDialogOptions options;
          options.reserve(emulation_speeds.size());
          for (const float speed : emulation_speeds)
          {
            options.emplace_back(
              (speed != 0.0f) ?
                StringUtil::StdStringFromFormat("%d%% [%d FPS (NTSC) / %d FPS (PAL)]", static_cast<int>(speed * 100.0f),
                                                static_cast<int>(60.0f * speed), static_cast<int>(50.0f * speed)) :
                "Unlimited",
              speed == current_speed);
          }
          return options;
        };

        BeginMenuButtons();

        MenuHeading("Speed Control");

#define MAKE_EMULATION_SPEED(setting_title, setting_var)                                                               \
  if (MenuButtonWithValue(                                                                                             \
        setting_title,                                                                                                 \
        "Sets the target emulation speed. It is not guaranteed that this speed will be reached on all systems.",       \
        (setting_var != 0.0f) ? TinyString::FromFormat("%.0f%%", setting_var * 100.0f) : TinyString("Unlimited")))     \
  {                                                                                                                    \
    OpenChoiceDialog(setting_title, false, get_emulation_speed_options(setting_var),                                   \
                     [](s32 index, const std::string& title, bool checked) {                                           \
                       if (index >= 0)                                                                                 \
                       {                                                                                               \
                         setting_var = emulation_speeds[index];                                                        \
                         s_host_interface->RunLater(SaveAndApplySettings);                                             \
                       }                                                                                               \
                       CloseChoiceDialog();                                                                            \
                     });                                                                                               \
  }

        MAKE_EMULATION_SPEED("Emulation Speed", s_settings_copy.emulation_speed);
        MAKE_EMULATION_SPEED("Fast Forward Speed", s_settings_copy.fast_forward_speed);
        MAKE_EMULATION_SPEED("Turbo Speed", s_settings_copy.turbo_speed);

#undef MAKE_EMULATION_SPEED

        MenuHeading("Runahead/Rewind");

        settings_changed |=
          ToggleButton("Enable Rewinding", "Saves state periodically so you can rewind any mistakes while playing.",
                       &s_settings_copy.rewind_enable);
        settings_changed |= RangeButton(
          "Rewind Save Frequency",
          "How often a rewind state will be created. Higher frequencies have greater system requirements.",
          &s_settings_copy.rewind_save_frequency, 0.0f, 3600.0f, 0.1f, "%.2f Seconds", s_settings_copy.rewind_enable);
        settings_changed |=
          RangeButton("Rewind Save Frequency",
                      "How many saves will be kept for rewinding. Higher values have greater memory requirements.",
                      reinterpret_cast<s32*>(&s_settings_copy.rewind_save_slots), 1, 10000, 1, "%d Frames",
                      s_settings_copy.rewind_enable);

        TinyString summary;
        if (!s_settings_copy.IsRunaheadEnabled())
          summary = "Disabled";
        else
          summary.Format("%u Frames", s_settings_copy.runahead_frames);

        if (MenuButtonWithValue("Runahead",
                                "Simulates the system ahead of time and rolls back/replays to reduce input lag. Very "
                                "high system requirements.",
                                summary))
        {
          ImGuiFullscreen::ChoiceDialogOptions options;
          for (u32 i = 0; i <= 10; i++)
          {
            if (i == 0)
              options.emplace_back("Disabled", s_settings_copy.runahead_frames == i);
            else
              options.emplace_back(StringUtil::StdStringFromFormat("%u Frames", i),
                                   s_settings_copy.runahead_frames == i);
          }
          OpenChoiceDialog("Runahead", false, std::move(options),
                           [](s32 index, const std::string& title, bool checked) {
                             s_settings_copy.runahead_frames = index;
                             s_host_interface->RunLater(SaveAndApplySettings);
                             CloseChoiceDialog();
                           });
          settings_changed = true;
        }

        TinyString rewind_summary;
        if (s_settings_copy.IsRunaheadEnabled())
        {
          rewind_summary = "Rewind is disabled because runahead is enabled. Runahead will significantly increase "
                           "system requirements.";
        }
        else if (s_settings_copy.rewind_enable)
        {
          const float duration = ((s_settings_copy.rewind_save_frequency <= std::numeric_limits<float>::epsilon()) ?
                                    (1.0f / 60.0f) :
                                    s_settings_copy.rewind_save_frequency) *
                                 static_cast<float>(s_settings_copy.rewind_save_slots);

          u64 ram_usage, vram_usage;
          System::CalculateRewindMemoryUsage(s_settings_copy.rewind_save_slots, &ram_usage, &vram_usage);
          rewind_summary.Format("Rewind for %u frames, lasting %.2f seconds will require up to %" PRIu64
                                "MB of RAM and %" PRIu64 "MB of VRAM.",
                                s_settings_copy.rewind_save_slots, duration, ram_usage / 1048576, vram_usage / 1048576);
        }
        else
        {
          rewind_summary =
            "Rewind is not enabled. Please note that enabling rewind may significantly increase system requirements.";
        }

        ActiveButton(rewind_summary, false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY,
                     g_medium_font);

        EndMenuButtons();
      }
      break;

      case SettingsPage::BIOSSettings:
      {
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

        settings_changed |=
          ToggleButton("Enable Fast Boot", "Patches the BIOS to skip the boot animation. Safe to enable.",
                       &s_settings_copy.bios_patch_fast_boot);
        settings_changed |= ToggleButton(
          "Enable TTY Output", "Patches the BIOS to log calls to printf(). Only use when debugging, can break games.",
          &s_settings_copy.bios_patch_tty_enable);

        EndMenuButtons();
      }
      break;

      case SettingsPage::ControllerSettings:
      {
        BeginMenuButtons();

        MenuHeading("Input Profiles");
        if (MenuButton(ICON_FA_FOLDER_OPEN "  Load Input Profile",
                       "Applies a saved configuration of controller types and bindings."))
        {
          CommonHostInterface::InputProfileList profiles(s_host_interface->GetInputProfileList());
          ImGuiFullscreen::ChoiceDialogOptions options;
          options.reserve(profiles.size());
          for (CommonHostInterface::InputProfileEntry& entry : profiles)
            options.emplace_back(std::move(entry.name), false);

          auto callback = [profiles](s32 index, const std::string& title, bool checked) {
            if (index < 0)
              return;

            // needs a reload...
            s_host_interface->ApplyInputProfile(profiles[index].path.c_str());
            s_settings_copy.Load(*s_host_interface->GetSettingsInterface());
            s_host_interface->RunLater(SaveAndApplySettings);
            CloseChoiceDialog();
          };
          OpenChoiceDialog(ICON_FA_FOLDER_OPEN "  Load Input Profile", false, std::move(options), std::move(callback));
        }

        static std::array<ControllerType, NUM_CONTROLLER_AND_CARD_PORTS> type_cache = {};
        static std::array<Controller::ButtonList, NUM_CONTROLLER_AND_CARD_PORTS> button_cache;
        static std::array<Controller::AxisList, NUM_CONTROLLER_AND_CARD_PORTS> axis_cache;
        static std::array<Controller::SettingList, NUM_CONTROLLER_AND_CARD_PORTS> setting_cache;
        static std::array<std::string,
                          NUM_CONTROLLER_AND_CARD_PORTS * CommonHostInterface::NUM_CONTROLLER_AUTOFIRE_BUTTONS>
          autofire_buttons_cache;
        TinyString section;
        TinyString key;

        std::array<TinyString, NUM_CONTROLLER_AND_CARD_PORTS> port_labels = s_settings_copy.GeneratePortLabels();

        for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
        {
          if (port_labels[port].IsEmpty())
            continue;
          else
            MenuHeading(port_labels[port]);

          settings_changed |= EnumChoiceButton(
            TinyString::FromFormat(ICON_FA_GAMEPAD "  Controller Type##type%u", port),
            "Determines the simulated controller plugged into this port.", &s_settings_copy.controller_types[port],
            &Settings::GetControllerTypeDisplayName, ControllerType::Count);

          section.Format("Controller%u", port + 1);

          const ControllerType ctype = s_settings_copy.controller_types[port];
          if (ctype != type_cache[port])
          {
            type_cache[port] = ctype;
            button_cache[port] = Controller::GetButtonNames(ctype);
            axis_cache[port] = Controller::GetAxisNames(ctype);
            setting_cache[port] = Controller::GetSettings(ctype);

            for (u32 i = 0; i < CommonHostInterface::NUM_CONTROLLER_AUTOFIRE_BUTTONS; i++)
            {
              autofire_buttons_cache[port * CommonHostInterface::NUM_CONTROLLER_AUTOFIRE_BUTTONS + i] =
                s_host_interface->GetStringSettingValue(section, TinyString::FromFormat("AutoFire%uButton", i + 1));
            }
          }

          for (const auto& it : button_cache[port])
          {
            key.Format("Button%s", it.first.c_str());
            DrawInputBindingButton(InputBindingType::Button, section, key, it.first.c_str());
          }

          for (const auto& it : axis_cache[port])
          {
            key.Format("Axis%s", std::get<0>(it).c_str());
            DrawInputBindingButton(std::get<2>(it) == Controller::AxisType::Half ? InputBindingType::HalfAxis :
                                                                                   InputBindingType::Axis,
                                   section, key, std::get<0>(it).c_str());
          }

          if (Controller::GetVibrationMotorCount(ctype) > 0)
            DrawInputBindingButton(InputBindingType::Rumble, section, "Rumble", "Rumble/Vibration");

          for (const SettingInfo& it : setting_cache[port])
            settings_changed |= SettingInfoButton(it, section);

          for (u32 autofire_index = 0; autofire_index < CommonHostInterface::NUM_CONTROLLER_AUTOFIRE_BUTTONS;
               autofire_index++)
          {
            const u32 cache_index = port * CommonHostInterface::NUM_CONTROLLER_AUTOFIRE_BUTTONS + autofire_index;

            if (MenuButtonWithValue(
                  TinyString::FromFormat("Auto Fire %u##autofire_%u_%u", autofire_index + 1, port, autofire_index),
                  "Selects the button to toggle with this auto fire binding.",
                  autofire_buttons_cache[cache_index].c_str()))

            {
              auto callback = [port, autofire_index, cache_index](s32 index, const std::string& title, bool checked) {
                if (index < 0)
                  return;

                auto lock = s_host_interface->GetSettingsLock();
                if (index == 0)
                {
                  s_host_interface->GetSettingsInterface()->DeleteValue(
                    TinyString::FromFormat("Controller%u", port + 1),
                    TinyString::FromFormat("AutoFire%uButton", autofire_index + 1));
                  std::string().swap(autofire_buttons_cache[cache_index]);
                }
                else
                {
                  s_host_interface->GetSettingsInterface()->SetStringValue(
                    TinyString::FromFormat("Controller%u", port + 1),
                    TinyString::FromFormat("AutoFire%uButton", autofire_index + 1),
                    button_cache[port][index - 1].first.c_str());
                  autofire_buttons_cache[cache_index] = button_cache[port][index - 1].first;
                }

                // needs a reload...
                s_host_interface->RunLater(SaveAndApplySettings);
                CloseChoiceDialog();
              };

              ImGuiFullscreen::ChoiceDialogOptions options;
              options.reserve(button_cache[port].size() + 1);
              options.emplace_back("(None)", autofire_buttons_cache[cache_index].empty());
              for (const auto& it : button_cache[port])
                options.emplace_back(it.first, autofire_buttons_cache[cache_index] == it.first);

              OpenChoiceDialog(ICON_FA_GAMEPAD "  Select Auto Fire Button", false, std::move(options),
                               std::move(callback));
            }

            if (autofire_buttons_cache[cache_index].empty())
              continue;

            key.Format("AutoFire%u", autofire_index + 1);
            DrawInputBindingButton(InputBindingType::Button, section, key,
                                   TinyString::FromFormat("Auto Fire %u Binding##autofire_binding_%u_%u",
                                                          autofire_index + 1, port, autofire_index),
                                   false);

            key.Format("AutoFire%uFrequency", autofire_index + 1);
            int frequency = s_host_interface->GetSettingsInterface()->GetIntValue(
              section, key, CommonHostInterface::DEFAULT_AUTOFIRE_FREQUENCY);
            settings_changed |= RangeButton(TinyString::FromFormat("Auto Fire %u Frequency##autofire_frequency_%u_%u",
                                                                   autofire_index + 1, port, autofire_index),
                                            "Sets the rate at which the auto fire will trigger on and off.", &frequency,
                                            1, 255, 1, "%d Frames");
          }
        }

        EndMenuButtons();
      }
      break;

      case SettingsPage::HotkeySettings:
      {
        BeginMenuButtons();

        TinyString last_category;
        for (const CommonHostInterface::HotkeyInfo& hotkey : s_host_interface->GetHotkeyInfoList())
        {
          if (hotkey.category != last_category)
          {
            MenuHeading(hotkey.category);
            last_category = hotkey.category;
          }

          DrawInputBindingButton(InputBindingType::Button, "Hotkeys", hotkey.name, hotkey.display_name);
        }

        EndMenuButtons();
      }
      break;

      case SettingsPage::MemoryCardSettings:
      {
        BeginMenuButtons();

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

        EndMenuButtons();
      }
      break;

      case SettingsPage::DisplaySettings:
      {
        BeginMenuButtons();

        MenuHeading("Device Settings");

        settings_changed |=
          EnumChoiceButton("GPU Renderer", "Chooses the backend to use for rendering the console/game visuals.",
                           &s_settings_copy.gpu_renderer, &Settings::GetRendererDisplayName, GPURenderer::Count);

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

        switch (s_settings_copy.gpu_renderer)
        {
#ifdef _WIN32
          case GPURenderer::HardwareD3D11:
          {
            settings_changed |= ToggleButtonForNonSetting(
              "Use Blit Swap Chain",
              "Uses a blit presentation model instead of flipping. This may be needed on some systems.", "Display",
              "UseBlitSwapChain", false);
          }
          break;
#endif

          case GPURenderer::HardwareVulkan:
          {
            settings_changed |=
              ToggleButton("Threaded Presentation",
                           "Presents frames on a background thread when fast forwarding or vsync is disabled.",
                           &s_settings_copy.gpu_threaded_presentation);
          }
          break;

          case GPURenderer::Software:
          {
            settings_changed |= ToggleButton("Threaded Rendering",
                                             "Uses a second thread for drawing graphics. Speed boost, and safe to use.",
                                             &s_settings_copy.gpu_use_thread);
          }
          break;

          default:
            break;
        }

        if (!s_settings_copy.IsUsingSoftwareRenderer())
        {
          settings_changed |=
            ToggleButton("Use Software Renderer For Readbacks",
                         "Runs the software renderer in parallel for VRAM readbacks. On some systems, this may result "
                         "in greater performance.",
                         &s_settings_copy.gpu_use_software_renderer_for_readbacks);
        }

        settings_changed |=
          ToggleButton("Enable VSync",
                       "Synchronizes presentation of the console's frames to the host. Enable for smoother animations.",
                       &s_settings_copy.video_sync_enabled);

        settings_changed |= ToggleButton("Sync To Host Refresh Rate",
                                         "Adjusts the emulation speed so the console's refresh rate matches the host "
                                         "when VSync and Audio Resampling are enabled.",
                                         &s_settings_copy.sync_to_host_refresh_rate, s_settings_copy.audio_resampling);

        settings_changed |= ToggleButton("Optimal Frame Pacing",
                                         "Ensures every frame generated is displayed for optimal pacing. Disable if "
                                         "you are having speed or sound issues.",
                                         &s_settings_copy.display_all_frames);

        MenuHeading("Screen Display");

        settings_changed |= EnumChoiceButton(
          "Aspect Ratio", "Changes the aspect ratio used to display the console's output to the screen.",
          &s_settings_copy.display_aspect_ratio, &Settings::GetDisplayAspectRatioName, DisplayAspectRatio::Count);

        settings_changed |= EnumChoiceButton(
          "Crop Mode", "Determines how much of the area typically not visible on a consumer TV set to crop/hide.",
          &s_settings_copy.display_crop_mode, &Settings::GetDisplayCropModeDisplayName, DisplayCropMode::Count);

        settings_changed |=
          EnumChoiceButton("Downsampling",
                           "Downsamples the rendered image prior to displaying it. Can improve "
                           "overall image quality in mixed 2D/3D games.",
                           &s_settings_copy.gpu_downsample_mode, &Settings::GetDownsampleModeDisplayName,
                           GPUDownsampleMode::Count, !s_settings_copy.IsUsingSoftwareRenderer());

        settings_changed |=
          ToggleButton("Linear Upscaling", "Uses a bilinear filter when upscaling to display, smoothing out the image.",
                       &s_settings_copy.display_linear_filtering, !s_settings_copy.display_integer_scaling);

        settings_changed |=
          ToggleButton("Integer Upscaling", "Adds padding to ensure pixels are a whole number in size.",
                       &s_settings_copy.display_integer_scaling);

        settings_changed |= ToggleButton(
          "Stretch To Fit", "Fills the window with the active display area, regardless of the aspect ratio.",
          &s_settings_copy.display_stretch);

        settings_changed |=
          ToggleButtonForNonSetting("Internal Resolution Screenshots",
                                    "Saves screenshots at internal render resolution and without postprocessing.",
                                    "Display", "InternalResolutionScreenshots", false);

        MenuHeading("On-Screen Display");

        settings_changed |= ToggleButton("Show OSD Messages", "Shows on-screen-display messages when events occur.",
                                         &s_settings_copy.display_show_osd_messages);
        settings_changed |= ToggleButton(
          "Show Game Frame Rate", "Shows the internal frame rate of the game in the top-right corner of the display.",
          &s_settings_copy.display_show_fps);
        settings_changed |= ToggleButton("Show Display FPS",
                                         "Shows the number of frames (or v-syncs) displayed per second by the system "
                                         "in the top-right corner of the display.",
                                         &s_settings_copy.display_show_vps);
        settings_changed |= ToggleButton(
          "Show Speed",
          "Shows the current emulation speed of the system in the top-right corner of the display as a percentage.",
          &s_settings_copy.display_show_speed);
        settings_changed |=
          ToggleButton("Show Resolution",
                       "Shows the current rendering resolution of the system in the top-right corner of the display.",
                       &s_settings_copy.display_show_resolution);
        settings_changed |= ToggleButtonForNonSetting(
          "Show Controller Input",
          "Shows the current controller state of the system in the bottom-left corner of the display.", "Display",
          "ShowInputs", false);

        EndMenuButtons();
      }
      break;

      case SettingsPage::EnhancementSettings:
      {
        static const auto resolution_scale_text_callback = [](u32 value) -> const char* {
          static constexpr std::array<const char*, 17> texts = {
            {"Automatic based on window size", "1x", "2x", "3x (for 720p)", "4x", "5x (for 1080p)", "6x (for 1440p)",
             "7x", "8x", "9x (for 4K)", "10x", "11x", "12x", "13x", "14x", "15x", "16x"

            }};
          return (value >= texts.size()) ? "" : texts[value];
        };

        BeginMenuButtons();

        MenuHeading("Rendering Enhancements");

        settings_changed |= EnumChoiceButton<u32, u32>(
          "Internal Resolution Scale",
          "Scales internal VRAM resolution by the specified multiplier. Some games require 1x VRAM resolution.",
          &s_settings_copy.gpu_resolution_scale, resolution_scale_text_callback, 17);
        settings_changed |= EnumChoiceButton(
          "Texture Filtering",
          "Smooths out the blockyness of magnified textures on 3D objects. Will have a greater effect "
          "on higher resolution scales.",
          &s_settings_copy.gpu_texture_filter, &Settings::GetTextureFilterDisplayName, GPUTextureFilter::Count);
        settings_changed |=
          ToggleButton("True Color Rendering",
                       "Disables dithering and uses the full 8 bits per channel of color information. May break "
                       "rendering in some games.",
                       &s_settings_copy.gpu_true_color);
        settings_changed |= ToggleButton(
          "Scaled Dithering",
          "Scales the dithering pattern with the internal rendering resolution, making it less noticeable. "
          "Usually safe to enable.",
          &s_settings_copy.gpu_scaled_dithering, s_settings_copy.gpu_resolution_scale > 1);
        settings_changed |= ToggleButton(
          "Widescreen Hack", "Increases the field of view from 4:3 to the chosen display aspect ratio in 3D games.",
          &s_settings_copy.gpu_widescreen_hack);

        MenuHeading("Display Enhancements");

        settings_changed |=
          ToggleButton("Disable Interlacing",
                       "Disables interlaced rendering and display in the GPU. Some games can render in 480p this way, "
                       "but others will break.",
                       &s_settings_copy.gpu_disable_interlacing);
        settings_changed |= ToggleButton(
          "Force NTSC Timings",
          "Forces PAL games to run at NTSC timings, i.e. 60hz. Some PAL games will run at their \"normal\" "
          "speeds, while others will break.",
          &s_settings_copy.gpu_force_ntsc_timings);
        settings_changed |=
          ToggleButton("Force 4:3 For 24-Bit Display",
                       "Switches back to 4:3 display aspect ratio when displaying 24-bit content, usually FMVs.",
                       &s_settings_copy.display_force_4_3_for_24bit);
        settings_changed |= ToggleButton(
          "Chroma Smoothing For 24-Bit Display",
          "Smooths out blockyness between colour transitions in 24-bit content, usually FMVs. Only applies "
          "to the hardware renderers.",
          &s_settings_copy.gpu_24bit_chroma_smoothing);

        MenuHeading("PGXP (Precision Geometry Transform Pipeline");

        settings_changed |=
          ToggleButton("PGXP Geometry Correction",
                       "Reduces \"wobbly\" polygons by attempting to preserve the fractional component through memory "
                       "transfers.",
                       &s_settings_copy.gpu_pgxp_enable);
        settings_changed |=
          ToggleButton("PGXP Texture Correction",
                       "Uses perspective-correct interpolation for texture coordinates and colors, straightening out "
                       "warped textures.",
                       &s_settings_copy.gpu_pgxp_texture_correction, s_settings_copy.gpu_pgxp_enable);
        settings_changed |=
          ToggleButton("PGXP Culling Correction",
                       "Increases the precision of polygon culling, reducing the number of holes in geometry.",
                       &s_settings_copy.gpu_pgxp_culling, s_settings_copy.gpu_pgxp_enable);
        settings_changed |=
          ToggleButton("PGXP Preserve Projection Precision",
                       "Adds additional precision to PGXP data post-projection. May improve visuals in some games.",
                       &s_settings_copy.gpu_pgxp_preserve_proj_fp, s_settings_copy.gpu_pgxp_enable);
        settings_changed |= ToggleButton(
          "PGXP Depth Buffer", "Reduces polygon Z-fighting through depth testing. Low compatibility with games.",
          &s_settings_copy.gpu_pgxp_depth_buffer,
          s_settings_copy.gpu_pgxp_enable && s_settings_copy.gpu_pgxp_texture_correction);
        settings_changed |= ToggleButton("PGXP CPU Mode", "Uses PGXP for all instructions, not just memory operations.",
                                         &s_settings_copy.gpu_pgxp_cpu, s_settings_copy.gpu_pgxp_enable);

        EndMenuButtons();
      }
      break;

      case SettingsPage::AudioSettings:
      {
        BeginMenuButtons();

        MenuHeading("Audio Control");

        settings_changed |= RangeButton("Output Volume", "Controls the volume of the audio played on the host.",
                                        &s_settings_copy.audio_output_volume, 0, 100, 1, "%d%%");
        settings_changed |= RangeButton("Fast Forward Volume",
                                        "Controls the volume of the audio played on the host when fast forwarding.",
                                        &s_settings_copy.audio_fast_forward_volume, 0, 100, 1, "%d%%");
        settings_changed |= ToggleButton("Mute All Sound", "Prevents the emulator from producing any audible sound.",
                                         &s_settings_copy.audio_output_muted);
        settings_changed |= ToggleButton("Mute CD Audio",
                                         "Forcibly mutes both CD-DA and XA audio from the CD-ROM. Can be used to "
                                         "disable background music in some games.",
                                         &s_settings_copy.cdrom_mute_cd_audio);

        MenuHeading("Backend Settings");

        settings_changed |= EnumChoiceButton(
          "Audio Backend",
          "The audio backend determines how frames produced by the emulator are submitted to the host.",
          &s_settings_copy.audio_backend, &Settings::GetAudioBackendDisplayName, AudioBackend::Count);
        settings_changed |= RangeButton(
          "Buffer Size", "The buffer size determines the size of the chunks of audio which will be pulled by the host.",
          reinterpret_cast<s32*>(&s_settings_copy.audio_buffer_size), 1024, 8192, 128, "%d Frames");

        settings_changed |= ToggleButton("Sync To Output",
                                         "Throttles the emulation speed based on the audio backend pulling audio "
                                         "frames. Enable to reduce the chances of crackling.",
                                         &s_settings_copy.audio_sync_enabled);
        settings_changed |= ToggleButton(
          "Resampling",
          "When running outside of 100% speed, resamples audio from the target speed instead of dropping frames.",
          &s_settings_copy.audio_resampling);

        EndMenuButtons();
      }
      break;

      case SettingsPage::AchievementsSetings:
      {
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
          "When enabled, rich presence information will be collected and sent to the server where supported.",
          "Cheevos", "RichPresence", true);
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
          ActiveButton(SmallString::FromFormat(ICON_FA_USER "  Username: %s", Cheevos::GetUsername().c_str()), false,
                       false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

          Timestamp ts;
          TinyString ts_string;
          ts.SetUnixTimestamp(StringUtil::FromChars<u64>(s_host_interface->GetSettingsInterface()->GetStringValue(
                                                           "Cheevos", "LoginTimestamp", "0"))
                                .value_or(0));
          ts.ToString(ts_string, "%Y-%m-%d %H:%M:%S");
          ActiveButton(SmallString::FromFormat(ICON_FA_CLOCK "  Login token generated on %s", ts_string.GetCharArray()),
                       false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
          ImGui::PopStyleColor();

          if (MenuButton(ICON_FA_KEY "  Logout", "Logs out of RetroAchievements."))
            Cheevos::Logout();
        }
        else if (Cheevos::IsActive())
        {
          ActiveButton(ICON_FA_USER "  Not Logged In", false, false,
                       ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

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
          ActiveButton(TinyString::FromFormat(ICON_FA_BOOK "  Game Title: %s", Cheevos::GetGameTitle().c_str()), false,
                       false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
          ActiveButton(
            TinyString::FromFormat(ICON_FA_DESKTOP "  Game Developer: %s", Cheevos::GetGameDeveloper().c_str()), false,
            false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
          ActiveButton(
            TinyString::FromFormat(ICON_FA_DESKTOP "  Game Publisher: %s", Cheevos::GetGamePublisher().c_str()), false,
            false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
          ActiveButton(TinyString::FromFormat(ICON_FA_TROPHY "  Achievements: %u (%u points)",
                                              Cheevos::GetAchievementCount(), Cheevos::GetMaximumPointsForGame()),
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
      }
      break;

      case SettingsPage::AdvancedSettings:
      {
        BeginMenuButtons();

        MenuHeading("Logging Settings");
        settings_changed |=
          EnumChoiceButton("Log Level", "Sets the verbosity of messages logged. Higher levels will log more messages.",
                           &s_settings_copy.log_level, &Settings::GetLogLevelDisplayName, LOGLEVEL_COUNT);
        settings_changed |= ToggleButton("Log To System Console", "Logs messages to the console window.",
                                         &s_settings_copy.log_to_console);
        settings_changed |= ToggleButton("Log To Debug Console", "Logs messages to the debug console where supported.",
                                         &s_settings_copy.log_to_debug);
        settings_changed |= ToggleButton("Log To File", "Logs messages to duckstation.log in the user directory.",
                                         &s_settings_copy.log_to_file);

        MenuHeading("Debugging Settings");

        bool debug_menu = s_debug_menu_enabled;
        if (ToggleButton("Enable Debug Menu", "Shows a debug menu bar with additional statistics and quick settings.",
                         &debug_menu))
        {
          s_host_interface->RunLater([debug_menu]() { SetDebugMenuEnabled(debug_menu); });
        }

        settings_changed |=
          ToggleButton("Disable All Enhancements", "Temporarily disables all enhancements, useful when testing.",
                       &s_settings_copy.disable_all_enhancements);

        settings_changed |= ToggleButton(
          "Use Debug GPU Device", "Enable debugging when supported by the host's renderer API. Only for developer use.",
          &s_settings_copy.gpu_use_debug_device);

#ifdef _WIN32
        settings_changed |=
          ToggleButton("Increase Timer Resolution", "Enables more precise frame pacing at the cost of battery life.",
                       &s_settings_copy.increase_timer_resolution);
#endif

        settings_changed |= ToggleButtonForNonSetting("Allow Booting Without SBI File",
                                                      "Allows loading protected games without subchannel information.",
                                                      "CDROM", "AllowBootingWithoutSBIFile", false);

        settings_changed |= ToggleButtonForNonSetting("Create Save State Backups",
                                                      "Renames existing save states when saving to a backup file.",
                                                      "General", "CreateSaveStateBackups", false);

        MenuHeading("Display Settings");
        settings_changed |=
          ToggleButton("Show Status Indicators", "Shows persistent icons when turbo is active or when paused.",
                       &g_settings.display_show_status_indicators);
        settings_changed |= ToggleButton("Show Enhancement Settings",
                                         "Shows enhancement settings in the bottom-right corner of the screen.",
                                         &g_settings.display_show_enhancements);
        settings_changed |= RangeButton(
          "Display FPS Limit", "Limits how many frames are displayed to the screen. These frames are still rendered.",
          &s_settings_copy.display_max_fps, 0.0f, 500.0f, 1.0f, "%.2f FPS");

        MenuHeading("PGXP Settings");

        settings_changed |= ToggleButton(
          "Enable PGXP Vertex Cache", "Uses screen positions to resolve PGXP data. May improve visuals in some games.",
          &s_settings_copy.gpu_pgxp_vertex_cache, s_settings_copy.gpu_pgxp_enable);
        settings_changed |= RangeButton(
          "PGXP Geometry Tolerance",
          "Sets a threshold for discarding precise values when exceeded. May help with glitches in some games.",
          &s_settings_copy.gpu_pgxp_tolerance, -1.0f, 10.0f, 0.1f, "%.1f Pixels", s_settings_copy.gpu_pgxp_enable);
        settings_changed |= RangeButton(
          "PGXP Depth Clear Threshold",
          "Sets a threshold for discarding the emulated depth buffer. May help in some games.",
          &s_settings_copy.gpu_pgxp_tolerance, 0.0f, 4096.0f, 1.0f, "%.1f", s_settings_copy.gpu_pgxp_enable);

        MenuHeading("Texture Dumping/Replacements");

        settings_changed |= ToggleButton("Enable VRAM Write Texture Replacement",
                                         "Enables the replacement of background textures in supported games.",
                                         &s_settings_copy.texture_replacements.enable_vram_write_replacements);
        settings_changed |= ToggleButton("Preload Replacement Textures",
                                         "Loads all replacement texture to RAM, reducing stuttering at runtime.",
                                         &s_settings_copy.texture_replacements.preload_textures,
                                         s_settings_copy.texture_replacements.AnyReplacementsEnabled());
        settings_changed |=
          ToggleButton("Dump Replacable VRAM Writes", "Writes textures which can be replaced to the dump directory.",
                       &s_settings_copy.texture_replacements.dump_vram_writes);
        settings_changed |=
          ToggleButton("Set VRAM Write Dump Alpha Channel", "Clears the mask/transparency bit in VRAM write dumps.",
                       &s_settings_copy.texture_replacements.dump_vram_write_force_alpha_channel,
                       s_settings_copy.texture_replacements.dump_vram_writes);

        MenuHeading("CPU Emulation");

        settings_changed |=
          ToggleButton("Enable Recompiler ICache",
                       "Simulates the CPU's instruction cache in the recompiler. Can help with games running too fast.",
                       &s_settings_copy.cpu_recompiler_icache);
        settings_changed |= ToggleButton("Enable Recompiler Memory Exceptions",
                                         "Enables alignment and bus exceptions. Not needed for any known games.",
                                         &s_settings_copy.cpu_recompiler_memory_exceptions);
        settings_changed |= ToggleButton(
          "Enable Recompiler Block Linking",
          "Performance enhancement - jumps directly between blocks instead of returning to the dispatcher.",
          &s_settings_copy.cpu_recompiler_block_linking);
        settings_changed |= EnumChoiceButton("Recompiler Fast Memory Access",
                                             "Avoids calls to C++ code, significantly speeding up the recompiler.",
                                             &s_settings_copy.cpu_fastmem_mode, &Settings::GetCPUFastmemModeDisplayName,
                                             CPUFastmemMode::Count, !s_settings_copy.cpu_recompiler_memory_exceptions);

        EndMenuButtons();
      }
      break;
    }

    if (settings_changed)
      s_host_interface->RunLater(SaveAndApplySettings);
  }

  EndFullscreenWindow();
}

void DrawQuickMenu(MainWindowType type)
{
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImVec2 display_size(ImGui::GetIO().DisplaySize);
  dl->AddRectFilled(ImVec2(0.0f, 0.0f), display_size, IM_COL32(0x21, 0x21, 0x21, 200));

  // title info
  {
    const std::string& title = System::GetRunningTitle();
    const std::string& code = System::GetRunningCode();

    SmallString subtitle;
    if (!code.empty())
      subtitle.Format("%s - ", code.c_str());
    subtitle.AppendString(FileSystem::GetFileNameFromPath(System::GetRunningPath()));

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
    if (Cheevos::IsActive())
    {
      const std::string& rp = Cheevos::GetRichPresenceString();
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
    BeginMenuButtons(13, 1.0f, ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                     ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING,
                     ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    if (ActiveButton(ICON_FA_PLAY "  Resume Game", false) || WantsToCloseMenu())
      CloseQuickMenu();

    if (ActiveButton(ICON_FA_FAST_FORWARD "  Fast Forward", false))
    {
      s_host_interface->RunLater(
        []() { s_host_interface->SetFastForwardEnabled(!s_host_interface->IsFastForwardEnabled()); });
      CloseQuickMenu();
    }

#ifdef WITH_CHEEVOS
    const bool achievements_enabled = Cheevos::HasActiveGame() && (Cheevos::GetAchievementCount() > 0);
    if (ActiveButton(ICON_FA_TROPHY "  Achievements", false, achievements_enabled))
      OpenAchievementsWindow();

    const bool leaderboards_enabled = Cheevos::HasActiveGame() && (Cheevos::GetLeaderboardCount() > 0);
    if (ActiveButton(ICON_FA_STOPWATCH "  Leaderboards", false, leaderboards_enabled))
      OpenLeaderboardsWindow();

#else
    ActiveButton(ICON_FA_TROPHY "  Achievements", false, false);
    ActiveButton(ICON_FA_STOPWATCH "  Leaderboards", false, false);
#endif

    if (ActiveButton(ICON_FA_CAMERA "  Save Screenshot", false))
    {
      CloseQuickMenu();
      s_host_interface->RunLater([]() { s_host_interface->SaveScreenshot(); });
    }

    if (ActiveButton(ICON_FA_UNDO "  Load State", false, !IsCheevosHardcoreModeActive()))
    {
      s_current_main_window = MainWindowType::None;
      OpenSaveStateSelector(true);
    }

    if (ActiveButton(ICON_FA_SAVE "  Save State", false))
    {
      s_current_main_window = MainWindowType::None;
      OpenSaveStateSelector(false);
    }

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

    if (ActiveButton(ICON_FA_COMPACT_DISC "  Change Disc", false))
    {
      s_current_main_window = MainWindowType::None;
      DoChangeDisc();
    }

    if (ActiveButton(ICON_FA_SLIDERS_H "  Settings", false))
      s_current_main_window = MainWindowType::Settings;

    if (ActiveButton(ICON_FA_SYNC "  Reset System", false))
    {
      CloseQuickMenu();
      s_host_interface->RunLater(DoReset);
    }

    if (ActiveButton(ICON_FA_POWER_OFF "  Exit Game", false))
    {
      CloseQuickMenu();
      s_host_interface->RunLater(DoPowerOff);
    }

    EndMenuButtons();

    EndFullscreenWindow();
  }
}

void InitializePlaceholderSaveStateListEntry(SaveStateListEntry* li, s32 slot, bool global)
{
  if (global)
    li->title = StringUtil::StdStringFromFormat("Global Slot %d##global_slot_%d", slot, slot);
  else
    li->title =
      StringUtil::StdStringFromFormat("%s Slot %d##game_slot_%d", System::GetRunningTitle().c_str(), slot, slot);

  li->summary = "No Save State";

  std::string().swap(li->path);
  std::string().swap(li->media_path);
  li->slot = slot;
  li->global = global;
}

void InitializeSaveStateListEntry(SaveStateListEntry* li, CommonHostInterface::ExtendedSaveStateInfo* ssi)
{
  if (ssi->global)
  {
    li->title =
      StringUtil::StdStringFromFormat("Global Save %d - %s##global_slot_%d", ssi->slot, ssi->title.c_str(), ssi->slot);
  }
  else
  {
    li->title = StringUtil::StdStringFromFormat("%s Slot %d##game_slot_%d", ssi->title.c_str(), ssi->slot, ssi->slot);
  }

  li->summary =
    StringUtil::StdStringFromFormat("%s - Saved %s", ssi->game_code.c_str(),
                                    Timestamp::FromUnixTimestamp(ssi->timestamp).ToString("%c").GetCharArray());

  li->slot = ssi->slot;
  li->global = ssi->global;
  li->path = std::move(ssi->path);
  li->media_path = std::move(ssi->media_path);

  li->preview_texture.reset();
  if (ssi && !ssi->screenshot_data.empty())
  {
    li->preview_texture = s_host_interface->GetDisplay()->CreateTexture(
      ssi->screenshot_width, ssi->screenshot_height, 1, 1, 1, HostDisplayPixelFormat::RGBA8,
      ssi->screenshot_data.data(), sizeof(u32) * ssi->screenshot_width, false);
  }
  else
  {
    li->preview_texture = s_host_interface->GetDisplay()->CreateTexture(
      PLACEHOLDER_ICON_WIDTH, PLACEHOLDER_ICON_HEIGHT, 1, 1, 1, HostDisplayPixelFormat::RGBA8, PLACEHOLDER_ICON_DATA,
      sizeof(u32) * PLACEHOLDER_ICON_WIDTH, false);
  }

  if (!li->preview_texture)
    Log_ErrorPrintf("Failed to upload save state image to GPU");
}

void PopulateSaveStateListEntries()
{
  s_save_state_selector_slots.clear();

  if (s_save_state_selector_loading)
  {
    std::optional<CommonHostInterface::ExtendedSaveStateInfo> ssi = s_host_interface->GetUndoSaveStateInfo();
    if (ssi)
    {
      SaveStateListEntry li;
      InitializeSaveStateListEntry(&li, &ssi.value());
      li.title = "Undo Load State";
      li.summary = "Restores the state of the system prior to the last state loaded.";
      s_save_state_selector_slots.push_back(std::move(li));
    }
  }

  if (!System::GetRunningCode().empty())
  {
    for (s32 i = 1; i <= CommonHostInterface::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      std::optional<CommonHostInterface::ExtendedSaveStateInfo> ssi =
        s_host_interface->GetExtendedSaveStateInfo(System::GetRunningCode().c_str(), i);

      SaveStateListEntry li;
      if (ssi)
        InitializeSaveStateListEntry(&li, &ssi.value());
      else
        InitializePlaceholderSaveStateListEntry(&li, i, false);

      s_save_state_selector_slots.push_back(std::move(li));
    }
  }

  for (s32 i = 1; i <= CommonHostInterface::GLOBAL_SAVE_STATE_SLOTS; i++)
  {
    std::optional<CommonHostInterface::ExtendedSaveStateInfo> ssi =
      s_host_interface->GetExtendedSaveStateInfo(nullptr, i);

    SaveStateListEntry li;
    if (ssi)
      InitializeSaveStateListEntry(&li, &ssi.value());
    else
      InitializePlaceholderSaveStateListEntry(&li, i, true);

    s_save_state_selector_slots.push_back(std::move(li));
  }
}

#if 0
void DrawSaveStateSelector(bool is_loading, bool fullscreen)
{
  const HostDisplayTexture* selected_texture = s_placeholder_texture.get();
  if (!BeginFullscreenColumns())
  {
    EndFullscreenColumns();
    return;
  }

  // drawn back the front so the hover changes the image
  if (BeginFullscreenColumnWindow(570.0f, LAYOUT_SCREEN_WIDTH, "save_state_selector_slots"))
  {
    BeginMenuButtons(static_cast<u32>(s_save_state_selector_slots.size()), true);

    for (const SaveStateListEntry& entry : s_save_state_selector_slots)
    {
      if (MenuButton(entry.title.c_str(), entry.summary.c_str()))
      {
        const std::string& path = entry.path;
        s_host_interface->RunLater([path]() { s_host_interface->LoadState(path.c_str()); });
      }

      if (ImGui::IsItemHovered())
        selected_texture = entry.preview_texture.get();
    }

    EndMenuButtons();
  }
  EndFullscreenColumnWindow();

  if (BeginFullscreenColumnWindow(0.0f, 570.0f, "save_state_selector_preview", ImVec4(0.11f, 0.15f, 0.17f, 1.00f)))
  {
    ImGui::SetCursorPos(LayoutScale(20.0f, 20.0f));
    ImGui::PushFont(g_large_font);
    ImGui::TextUnformatted(is_loading ? ICON_FA_FOLDER_OPEN "  Load State" : ICON_FA_SAVE "  Save State");
    ImGui::PopFont();

    ImGui::SetCursorPos(LayoutScale(ImVec2(85.0f, 160.0f)));
    ImGui::Image(selected_texture ? selected_texture->GetHandle() : s_placeholder_texture->GetHandle(),
      LayoutScale(ImVec2(400.0f, 400.0f)));

    ImGui::SetCursorPosY(LayoutScale(670.0f));
    BeginMenuButtons(1, false);
    if (ActiveButton(ICON_FA_BACKWARD "  Back", false))
      ReturnToMainWindow();
    EndMenuButtons();
  }
  EndFullscreenColumnWindow();

  EndFullscreenColumns();
}
#endif

void OpenSaveStateSelector(bool is_loading)
{
  s_save_state_selector_loading = is_loading;
  s_save_state_selector_open = true;
  s_save_state_selector_slots.clear();
  PopulateSaveStateListEntries();
}

void CloseSaveStateSelector()
{
  s_save_state_selector_slots.clear();
  s_save_state_selector_open = false;
  ReturnToMainWindow();
}

void DrawSaveStateSelector(bool is_loading, bool fullscreen)
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

  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (const SaveStateListEntry& entry : s_save_state_selector_slots)
  {
    ImRect bb;
    bool visible, hovered;
    bool pressed = MenuButtonFrame(entry.title.c_str(), true, button_height, &visible, &hovered, &bb.Min, &bb.Max);
    if (!visible)
      continue;

    ImVec2 pos(bb.Min);

    // use aspect ratio of screenshot to determine height
    const HostDisplayTexture* image = entry.preview_texture ? entry.preview_texture.get() : s_placeholder_texture.get();
    const float image_height =
      max_image_width / (static_cast<float>(image->GetWidth()) / static_cast<float>(image->GetHeight()));
    const float image_margin = (max_image_height - image_height) / 2.0f;
    const ImRect image_bb(ImVec2(pos.x, pos.y + LayoutScale(image_margin)),
                          pos + LayoutScale(max_image_width, image_margin + image_height));
    pos.x += LayoutScale(max_image_width + padding);

    dl->AddImage(static_cast<ImTextureID>(entry.preview_texture ? entry.preview_texture->GetHandle() :
                                                                  s_placeholder_texture->GetHandle()),
                 image_bb.Min, image_bb.Max);

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

    if (!entry.media_path.empty())
    {
      text_bb.Min.y = text_bb.Max.y + LayoutScale(4.0f);
      text_bb.Max.y = text_bb.Min.y + g_medium_font->FontSize;
      ImGui::RenderTextClipped(text_bb.Min, text_bb.Max, entry.media_path.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f),
                               &text_bb);
    }

    ImGui::PopFont();

    if (pressed)
    {
      if (is_loading)
      {
        const std::string& path = entry.path;
        s_host_interface->RunLater([path]() {
          if (path.empty())
            s_host_interface->UndoLoadState();
          else
            s_host_interface->LoadState(path.c_str());

          CloseSaveStateSelector();
        });
      }
      else
      {
        const s32 slot = entry.slot;
        const bool global = entry.global;
        s_host_interface->RunLater([slot, global]() {
          s_host_interface->SaveState(global, slot);
          CloseSaveStateSelector();
        });
      }
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

void DrawGameListWindow()
{
  const GameListEntry* selected_entry = nullptr;

  if (!BeginFullscreenColumns())
  {
    EndFullscreenColumns();
    return;
  }

  if (BeginFullscreenColumnWindow(450.0f, LAYOUT_SCREEN_WIDTH, "game_list_entries"))
  {
    const ImVec2 image_size(LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT, LAYOUT_MENU_BUTTON_HEIGHT));

    BeginMenuButtons();

    SmallString summary;

    for (const GameListEntry* entry : s_game_list_sorted_entries)
    {
      ImRect bb;
      bool visible, hovered;
      bool pressed =
        MenuButtonFrame(entry->path.c_str(), true, LAYOUT_MENU_BUTTON_HEIGHT, &visible, &hovered, &bb.Min, &bb.Max);
      if (!visible)
        continue;

      HostDisplayTexture* cover_texture = GetGameListCover(entry);
      if (entry->code.empty())
        summary.Format("%s - ", Settings::GetDiscRegionName(entry->region));
      else
        summary.Format("%s - %s - ", entry->code.c_str(), Settings::GetDiscRegionName(entry->region));

      summary.AppendString(FileSystem::GetFileNameFromPath(entry->path));

      ImGui::GetWindowDrawList()->AddImage(cover_texture->GetHandle(), bb.Min, bb.Min + image_size, ImVec2(0.0f, 0.0f),
                                           ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));

      const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
      const float text_start_x = bb.Min.x + image_size.x + LayoutScale(15.0f);
      const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
      const ImRect summary_bb(ImVec2(text_start_x, midpoint), bb.Max);

      ImGui::PushFont(g_large_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, entry->title.c_str(),
                               entry->title.c_str() + entry->title.size(), nullptr, ImVec2(0.0f, 0.0f), &title_bb);
      ImGui::PopFont();

      if (summary)
      {
        ImGui::PushFont(g_medium_font);
        ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, summary, summary.GetCharArray() + summary.GetLength(),
                                 nullptr, ImVec2(0.0f, 0.0f), &summary_bb);
        ImGui::PopFont();
      }

      if (pressed)
      {
        // launch game
        const std::string& path_to_launch(entry->path);
        s_host_interface->RunLater([path_to_launch]() { DoStartPath(path_to_launch, true); });
      }

      if (hovered)
        selected_entry = entry;
    }

    EndMenuButtons();
  }
  EndFullscreenColumnWindow();

  if (BeginFullscreenColumnWindow(0.0f, 450.0f, "game_list_info", ImVec4(0.11f, 0.15f, 0.17f, 1.00f)))
  {
    ImGui::SetCursorPos(LayoutScale(ImVec2(50.0f, 50.0f)));
    ImGui::Image(selected_entry ? GetGameListCover(selected_entry)->GetHandle() :
                                  GetTextureForGameListEntryType(GameListEntryType::Count)->GetHandle(),
                 LayoutScale(ImVec2(350.0f, 350.0f)));

    const float work_width = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    constexpr float field_margin_y = 10.0f;
    constexpr float start_x = 50.0f;
    float text_y = 425.0f;
    float text_width;
    SmallString text;

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
        text_width = ImGui::CalcTextSize(selected_entry->developer.c_str(), nullptr, false, work_width).x;
        ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
        ImGui::TextWrapped("%s", selected_entry->developer.c_str());
      }

      // code
      text_width = ImGui::CalcTextSize(selected_entry->code.c_str(), nullptr, false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::TextWrapped("%s", selected_entry->code.c_str());
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 15.0f);

      // region
      ImGui::TextUnformatted("Region: ");
      ImGui::SameLine();
      ImGui::Image(s_disc_region_textures[static_cast<u32>(selected_entry->region)]->GetHandle(),
                   LayoutScale(23.0f, 16.0f));
      ImGui::SameLine();
      ImGui::Text(" (%s)", Settings::GetDiscRegionDisplayName(selected_entry->region));

      // genre
      ImGui::Text("Genre: %s", selected_entry->genre.c_str());

      // release date
      char release_date_str[64];
      selected_entry->GetReleaseDateString(release_date_str, sizeof(release_date_str));
      ImGui::Text("Release Date: %s", release_date_str);

      // compatibility
      ImGui::TextUnformatted("Compatibility: ");
      ImGui::SameLine();
      ImGui::Image(s_game_compatibility_textures[static_cast<u32>(selected_entry->compatibility_rating)]->GetHandle(),
                   LayoutScale(64.0f, 16.0f));
      ImGui::SameLine();
      ImGui::Text(" (%s)", GameList::GetGameListCompatibilityRatingString(selected_entry->compatibility_rating));

      // size
      ImGui::Text("Size: %.2f MB", static_cast<float>(selected_entry->total_size) / 1048576.0f);

      // game settings
      const u32 user_setting_count = selected_entry->settings.GetUserSettingsCount();
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

void EnsureGameListLoaded()
{
  // not worth using a condvar here
  if (s_game_list_load_thread.joinable())
    s_game_list_load_thread.join();

  if (s_game_list_sorted_entries.empty())
    SortGameList();
}

static void GameListRefreshThread()
{
  ProgressCallback cb("game_list_refresh");
  s_host_interface->GetGameList()->Refresh(false, false, &cb);
}

void QueueGameListRefresh()
{
  if (s_game_list_load_thread.joinable())
    s_game_list_load_thread.join();

  s_game_list_sorted_entries.clear();
  s_host_interface->GetGameList()->SetSearchDirectoriesFromSettings(*s_host_interface->GetSettingsInterface());
  s_game_list_load_thread = std::thread(GameListRefreshThread);
}

void SwitchToGameList()
{
  EnsureGameListLoaded();
  s_current_main_window = MainWindowType::GameList;
}

void SortGameList()
{
  s_game_list_sorted_entries.clear();

  for (const GameListEntry& entry : s_host_interface->GetGameList()->GetEntries())
    s_game_list_sorted_entries.push_back(&entry);

  // TODO: Custom sort types
  std::sort(s_game_list_sorted_entries.begin(), s_game_list_sorted_entries.end(),
            [](const GameListEntry* lhs, const GameListEntry* rhs) { return lhs->title < rhs->title; });
}

HostDisplayTexture* GetGameListCover(const GameListEntry* entry)
{
  // lookup and grab cover image
  auto cover_it = s_cover_image_map.find(entry->path);
  if (cover_it == s_cover_image_map.end())
  {
    std::string cover_path(s_host_interface->GetGameList()->GetCoverImagePathForEntry(entry));
    cover_it = s_cover_image_map.emplace(entry->path, std::move(cover_path)).first;
  }

  if (!cover_it->second.empty())
    return GetCachedTexture(cover_it->second);

  return GetTextureForGameListEntryType(entry->type);
}

HostDisplayTexture* GetTextureForGameListEntryType(GameListEntryType type)
{
  switch (type)
  {
    case GameListEntryType::PSExe:
      return s_fallback_exe_texture.get();

    case GameListEntryType::Playlist:
      return s_fallback_playlist_texture.get();

    case GameListEntryType::PSF:
      return s_fallback_psf_texture.get();
      break;

    case GameListEntryType::Disc:
    default:
      return s_fallback_disc_texture.get();
  }
}

HostDisplayTexture* GetCoverForCurrentGame()
{
  EnsureGameListLoaded();

  const GameListEntry* entry = s_host_interface->GetGameList()->GetEntryForPath(System::GetRunningPath().c_str());
  if (!entry)
    return s_fallback_disc_texture.get();

  return GetGameListCover(entry);
}

//////////////////////////////////////////////////////////////////////////
// Overlays
//////////////////////////////////////////////////////////////////////////

void OpenAboutWindow()
{
  s_about_window_open = true;
}

void DrawAboutWindow()
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
      s_host_interface->RunLater(
        []() { s_host_interface->ReportError("Go to https://github.com/stenzek/duckstation/"); });
    }
    if (ActiveButton(ICON_FA_BUG "  Issue Tracker", false))
    {
      s_host_interface->RunLater(
        []() { s_host_interface->ReportError("Go to https://github.com/stenzek/duckstation/issues"); });
    }
    if (ActiveButton(ICON_FA_COMMENT "  Discord Server", false))
    {
      s_host_interface->RunLater([]() { s_host_interface->ReportError("Go to https://discord.gg/Buktv3t"); });
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

bool DrawErrorWindow(const char* message)
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

bool DrawConfirmWindow(const char* message, bool* result)
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

//////////////////////////////////////////////////////////////////////////
// Debug Menu
//////////////////////////////////////////////////////////////////////////

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
    static constexpr auto values = make_array(10u, 25u, 50u, 75u, 100u, 125u, 150u, 175u, 200u, 225u, 250u, 275u, 300u,
                                              350u, 400u, 450u, 500u, 600u, 700u, 800u);
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
    settings_changed |= ImGui::MenuItem("PGXP Texture Correction", nullptr,
                                        &s_settings_copy.gpu_pgxp_texture_correction, s_settings_copy.gpu_pgxp_enable);
    settings_changed |= ImGui::MenuItem("PGXP Vertex Cache", nullptr, &s_settings_copy.gpu_pgxp_vertex_cache,
                                        s_settings_copy.gpu_pgxp_enable);
    settings_changed |=
      ImGui::MenuItem("PGXP CPU Instructions", nullptr, &s_settings_copy.gpu_pgxp_cpu, s_settings_copy.gpu_pgxp_enable);
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
  settings_changed |= ImGui::MenuItem("24-Bit Chroma Smoothing", nullptr, &s_settings_copy.gpu_24bit_chroma_smoothing);

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

#ifdef WITH_CHEEVOS

static void DrawAchievement(const Cheevos::Achievement& cheevo)
{
  static constexpr float alpha = 0.8f;
  static constexpr float progress_height_unscaled = 20.0f;
  static constexpr float progress_spacing_unscaled = 5.0f;

  TinyString id_str;
  id_str.Format("%u", cheevo.id);

  const auto progress = Cheevos::GetAchievementProgress(cheevo);
  const bool is_measured = progress.second != 0;

  ImRect bb;
  bool visible, hovered;
  bool pressed =
    MenuButtonFrame(id_str, true,
                    !is_measured ? LAYOUT_MENU_BUTTON_HEIGHT :
                                   LAYOUT_MENU_BUTTON_HEIGHT + progress_height_unscaled + progress_spacing_unscaled,
                    &visible, &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const ImVec2 image_size(LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT, LAYOUT_MENU_BUTTON_HEIGHT));
  const std::string& badge_path = cheevo.locked ? cheevo.locked_badge_path : cheevo.unlocked_badge_path;
  if (!badge_path.empty())
  {
    HostDisplayTexture* badge = GetCachedTexture(badge_path);
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
    dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryDarkColor()));
    dl->AddRectFilled(progress_bb.Min, ImVec2(progress_bb.Min.x + fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                      ImGui::GetColorU32(ImGuiFullscreen::UISecondaryColor()));

    const TinyString text(GetAchievementProgressText(cheevo));
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                          progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f));
    dl->AddText(g_medium_font, g_medium_font->FontSize, text_pos,
                ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryTextColor()), text.GetCharArray(),
                text.GetCharArray() + text.GetLength());
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

void DrawAchievementWindow()
{
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
    ImRect bb;
    bool visible, hovered;
    bool pressed = MenuButtonFrame("achievements_heading", false, heading_height_unscaled, &visible, &hovered, &bb.Min,
                                   &bb.Max, 0, alpha);
    UNREFERENCED_VARIABLE(pressed);

    if (visible)
    {
      const float padding = LayoutScale(10.0f);
      const float spacing = LayoutScale(10.0f);
      const float image_height = LayoutScale(85.0f);

      const ImVec2 icon_min(bb.Min + ImVec2(padding, padding));
      const ImVec2 icon_max(icon_min + ImVec2(image_height, image_height));

      const std::string& icon_path = Cheevos::GetGameIcon();
      if (!icon_path.empty())
      {
        HostDisplayTexture* badge = GetCachedTexture(icon_path);
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
      SmallString text;
      ImVec2 text_size;

      const u32 unlocked_count = Cheevos::GetUnlockedAchiementCount();
      const u32 achievement_count = Cheevos::GetAchievementCount();
      const u32 current_points = Cheevos::GetCurrentPointsForGame();
      const u32 total_points = Cheevos::GetMaximumPointsForGame();

      if (FloatingButton(ICON_FA_WINDOW_CLOSE, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true, g_large_font) ||
          WantsToCloseMenu())
      {
        ReturnToMainWindow();
      }

      const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + g_large_font->FontSize));
      text.Assign(Cheevos::GetGameTitle());

      if (Cheevos::IsChallengeModeActive())
        text.AppendString(" (Hardcore Mode)");

      top += g_large_font->FontSize + spacing;

      ImGui::PushFont(g_large_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, text.GetCharArray(), text.GetCharArray() + text.GetLength(),
                               nullptr, ImVec2(0.0f, 0.0f), &title_bb);
      ImGui::PopFont();

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
      if (unlocked_count == achievement_count)
      {
        text.Format("You have unlocked all achievements and earned %u points!", total_points);
      }
      else
      {
        text.Format("You have unlocked %u of %u achievements, earning %u of %u possible points.", unlocked_count,
                    achievement_count, current_points, total_points);
      }

      top += g_medium_font->FontSize + spacing;

      ImGui::PushFont(g_medium_font);
      ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, text.GetCharArray(),
                               text.GetCharArray() + text.GetLength(), nullptr, ImVec2(0.0f, 0.0f), &summary_bb);
      ImGui::PopFont();

      const float progress_height = LayoutScale(20.0f);
      const ImRect progress_bb(ImVec2(left, top), ImVec2(right, top + progress_height));
      const float fraction = static_cast<float>(unlocked_count) / static_cast<float>(achievement_count);
      dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryDarkColor()));
      dl->AddRectFilled(progress_bb.Min,
                        ImVec2(progress_bb.Min.x + fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                        ImGui::GetColorU32(ImGuiFullscreen::UISecondaryColor()));

      text.Format("%d%%", static_cast<int>(std::round(fraction * 100.0f)));
      text_size = ImGui::CalcTextSize(text);
      const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                            progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) -
                              (text_size.y / 2.0f));
      dl->AddText(g_medium_font, g_medium_font->FontSize, text_pos,
                  ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryTextColor()), text.GetCharArray(),
                  text.GetCharArray() + text.GetLength());
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
      Cheevos::EnumerateAchievements([](const Cheevos::Achievement& cheevo) -> bool {
        if (!cheevo.locked)
          DrawAchievement(cheevo);

        return true;
      });
    }

    if (Cheevos::GetUnlockedAchiementCount() != Cheevos::GetAchievementCount())
    {
      static bool locked_achievements_collapsed = false;
      locked_achievements_collapsed ^= MenuHeadingButton(
        "Locked Achievements", locked_achievements_collapsed ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_UP);
      if (!locked_achievements_collapsed)
      {
        Cheevos::EnumerateAchievements([](const Cheevos::Achievement& cheevo) -> bool {
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

static void DrawLeaderboardListEntry(const Cheevos::Leaderboard& lboard)
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

static void DrawLeaderboardEntry(const Cheevos::LeaderboardEntry& lbEntry, float rank_column_width,
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

void DrawLeaderboardsWindow()
{
  static constexpr float alpha = 0.8f;
  static constexpr float heading_height_unscaled = 110.0f;

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

      const std::string& icon_path = Cheevos::GetGameIcon();
      if (!icon_path.empty())
      {
        HostDisplayTexture* badge = GetCachedTexture(icon_path);
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

      const u32 leaderboard_count = Cheevos::GetLeaderboardCount();

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
      text.Assign(Cheevos::GetGameTitle());

      top += g_large_font->FontSize + spacing;

      ImGui::PushFont(g_large_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, text.GetCharArray(), text.GetCharArray() + text.GetLength(),
                               nullptr, ImVec2(0.0f, 0.0f), &title_bb);
      ImGui::PopFont();

      if (s_open_leaderboard_id)
      {
        const Cheevos::Leaderboard* lboard = Cheevos::GetLeaderboardByID(*s_open_leaderboard_id);
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
        text.Format("This game has %u leaderboards.", leaderboard_count);
      }

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
      top += g_medium_font->FontSize + spacing_small;

      ImGui::PushFont(g_medium_font);
      ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, text.GetCharArray(),
                               text.GetCharArray() + text.GetLength(), nullptr, ImVec2(0.0f, 0.0f), &summary_bb);

      if (!IsCheevosHardcoreModeActive())
      {
        const ImRect hardcore_warning_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
        top += g_medium_font->FontSize + spacing_small;

        ImGui::RenderTextClipped(
          hardcore_warning_bb.Min, hardcore_warning_bb.Max,
          "Submitting scores is DISABLED because Hardcore Mode is off. Leaderboards are read-only.", nullptr, nullptr,
          ImVec2(0.0f, 0.0f), &hardcore_warning_bb);
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
        const Cheevos::Leaderboard* lboard = Cheevos::GetLeaderboardByID(*s_open_leaderboard_id);

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
                                 lboard != nullptr && Cheevos::IsLeaderboardTimeType(*lboard) ? "Time" : "Score",
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

      Cheevos::EnumerateLeaderboards([](const Cheevos::Leaderboard& lboard) -> bool {
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

      const auto result = Cheevos::TryEnumerateLeaderboardEntries(
        *s_open_leaderboard_id,
        [rank_column_width, name_column_width, column_spacing](const Cheevos::LeaderboardEntry& lbEntry) -> bool {
          DrawLeaderboardEntry(lbEntry, rank_column_width, name_column_width, column_spacing);
          return true;
        });

      if (!result.has_value())
      {
        ImGui::PushFont(g_large_font);

        const ImVec2 pos_min(0.0f, heading_height);
        const ImVec2 pos_max(display_size.x, display_size.y);
        ImGui::RenderTextClipped(pos_min, pos_max, "Downloading leaderboard data, please wait...", nullptr, nullptr,
                                 ImVec2(0.5f, 0.5f));

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

void DrawAchievementWindow() {}
void DrawLeaderboardsWindow() {}

#endif

bool SetControllerNavInput(FrontendCommon::ControllerNavigationButton button, bool value)
{
  s_nav_input_values[static_cast<u32>(button)] = value;
  if (!HasActiveWindow())
    return false;

#if 0
  // This is a bit hacky..
  ImGuiIO& io = ImGui::GetIO();

#define MAP_KEY(nbutton, imkey)                                                                                        \
  if (button == nbutton)                                                                                               \
  {                                                                                                                    \
    io.KeysDown[io.KeyMap[imkey]] = value;                                                                             \
  }

  MAP_KEY(FrontendCommon::ControllerNavigationButton::LeftTrigger, ImGuiKey_PageUp);
  MAP_KEY(FrontendCommon::ControllerNavigationButton::RightTrigger, ImGuiKey_PageDown);

#undef MAP_KEY
#endif

  return true;
}

void SetImGuiNavInputs()
{
  if (!HasActiveWindow())
    return;

  ImGuiIO& io = ImGui::GetIO();

#define MAP_BUTTON(button, imbutton) io.NavInputs[imbutton] = s_nav_input_values[static_cast<u32>(button)] ? 1.0f : 0.0f

  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::Activate, ImGuiNavInput_Activate);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::Cancel, ImGuiNavInput_Cancel);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::DPadLeft, ImGuiNavInput_DpadLeft);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::DPadRight, ImGuiNavInput_DpadRight);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::DPadUp, ImGuiNavInput_DpadUp);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::DPadDown, ImGuiNavInput_DpadDown);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::LeftShoulder, ImGuiNavInput_FocusPrev);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::RightShoulder, ImGuiNavInput_FocusNext);

#undef MAP_BUTTON
}

} // namespace FullscreenUI
