// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "achievements.h"
#include "bios.h"
#include "cheats.h"
#include "controller.h"
#include "fullscreenui_private.h"
#include "game_database.h"
#include "game_list.h"
#include "gpu.h"
#include "gpu_presenter.h"
#include "gpu_thread.h"
#include "gte.h"
#include "host.h"
#include "input_types.h"
#include "settings.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/ini_settings_interface.h"
#include "util/input_manager.h"
#include "util/postprocessing.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome6.h"
#include "IconsPromptFont.h"

#include <limits>
#include <mutex>

LOG_CHANNEL(FullscreenUI);

#ifndef __ANDROID__

namespace FullscreenUI {

namespace {

class InputBindingDialog : public PopupDialog
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

struct PostProcessingStageInfo
{
  std::string name;
  std::vector<PostProcessing::ShaderOption> options;
  bool enabled;
};

} // namespace

static void PopulateHotkeyList();

static void DrawSummarySettingsPage(bool show_localized_titles);
static void DrawInterfaceSettingsPage();
static void DrawGameListSettingsPage();
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
static void DrawAdvancedSettingsPage();
static void DrawPatchesOrCheatsSettingsPage(bool cheats);

static void DrawCoverDownloaderWindow();
static void DrawAchievementsLoginWindow();

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
static void PopulateGameListDirectoryCache(const SettingsInterface& si);
static void PopulatePatchesAndCheatsList();
static void PopulatePostProcessingChain(const SettingsInterface& si, const char* section);
static void BeginEffectBinding(SettingsInterface* bsi, InputBindingInfo::Type type, const char* section,
                               const char* key, std::string_view display_name);
static void DrawInputBindingButton(SettingsInterface* bsi, InputBindingInfo::Type type, const char* section,
                                   const char* name, std::string_view display_name, std::string_view icon_name,
                                   bool show_type = true);
static void StartAutomaticBindingForPort(u32 port);
static void StartClearBindingsForPort(u32 port);

static constexpr std::string_view ACHIEVEMENTS_LOGIN_DIALOG_NAME = "##achievements_login";
static constexpr std::string_view COVER_DOWNLOADER_DIALOG_NAME = "##cover_downloader";

namespace {
struct SettingsLocals
{
  float settings_last_bg_alpha = 1.0f;
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
  bool controller_macro_expanded[NUM_CONTROLLER_AND_CARD_PORTS][InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER] = {};
  InputBindingDialog input_binding_dialog;
};

} // namespace

ALIGN_TO_CACHE_LINE static SettingsLocals s_settings_locals;

} // namespace FullscreenUI

bool FullscreenUI::ShouldShowAdvancedSettings()
{
  return Host::GetBaseBoolSettingValue("Main", "ShowDebugMenu", false);
}

bool FullscreenUI::IsEditingGameSettings(SettingsInterface* bsi)
{
  return (bsi == s_settings_locals.game_settings_interface.get());
}

SettingsInterface* FullscreenUI::GetEditingSettingsInterface()
{
  return s_settings_locals.game_settings_interface ? s_settings_locals.game_settings_interface.get() :
                                                     Host::Internal::GetBaseSettingsLayer();
}

SettingsInterface* FullscreenUI::GetEditingSettingsInterface(bool game_settings)
{
  return (game_settings && s_settings_locals.game_settings_interface) ?
           s_settings_locals.game_settings_interface.get() :
           Host::Internal::GetBaseSettingsLayer();
}

void FullscreenUI::SetSettingsChanged(SettingsInterface* bsi)
{
  if (bsi && bsi == s_settings_locals.game_settings_interface.get())
    s_settings_locals.game_settings_changed.store(true, std::memory_order_release);
  else
    s_settings_locals.settings_changed.store(true, std::memory_order_release);
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
    InputManager::PrettifyInputBinding(value, &GetControllerIconMapping);

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
        case InputBindingInfo::Type::LED:
          title.format(ICON_FA_LIGHTBULB " {}", display_name);
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
    if (type == InputBindingInfo::Type::Motor || type == InputBindingInfo::Type::LED)
      BeginEffectBinding(bsi, type, section, name, display_name);
    else
      s_settings_locals.input_binding_dialog.Start(bsi, type, section, name, display_name);
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
    const bool reverse_threshold = (key.source_subtype == InputSubclass::ControllerAxis &&
                                    std::abs(initial_value) > 0.5f && std::abs(initial_value - min_value) > 0.1f);

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
          const SmallString new_binding =
            InputManager::ConvertInputBindingKeysToString(m_binding_type, m_new_bindings.data(), m_new_bindings.size());
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

void FullscreenUI::BeginEffectBinding(SettingsInterface* bsi, InputBindingInfo::Type type, const char* section,
                                      const char* key, std::string_view display_name)
{
  // vibration motors use a list to select
  const bool game_settings = IsEditingGameSettings(bsi);
  const InputManager::DeviceEffectList effects = InputManager::EnumerateDeviceEffects(type);
  if (effects.empty())
  {
    ShowToast({}, FSUI_STR("No devices with vibration motors were detected."));
    return;
  }

  const TinyString current_binding = bsi->GetTinyStringValue(section, key);
  size_t current_index = effects.size();
  ChoiceDialogOptions options;
  options.reserve(effects.size() + 1);
  for (size_t i = 0; i < effects.size(); i++)
  {
    const TinyString text = InputManager::ConvertInputBindingKeyToString(effects[i].first, effects[i].second);
    const bool this_index = (current_binding.view() == text);
    current_index = this_index ? i : current_index;
    options.emplace_back(text, this_index);
  }

  // empty/no mapping value
  if (type == InputBindingInfo::Type::Motor)
    options.emplace_back(FSUI_STR("No Vibration"), current_binding.empty());
  else if (type == InputBindingInfo::Type::LED)
    options.emplace_back(FSUI_STR("No LED"), current_binding.empty());

  // add current value to list if it's not currently available
  if (!current_binding.empty() && current_index == effects.size())
    options.emplace_back(std::make_pair(std::string(current_binding.view()), true));

  OpenChoiceDialog(display_name, false, std::move(options),
                   [game_settings, section = std::string(section), key = std::string(key),
                    effects = std::move(effects)](s32 index, const std::string& title, bool checked) {
                     if (index < 0)
                       return;

                     auto lock = Host::GetSettingsLock();
                     SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                     if (static_cast<size_t>(index) == effects.size())
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
    ChoiceDialogOptions cd_options;
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
    ChoiceDialogOptions cd_options;
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
  TextAlignedMultiLine(0.0f, IMSTR_START_END(summary));
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
  TextAlignedMultiLine(0.0f, IMSTR_START_END(summary));
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
  TextAlignedMultiLine(0.0f, IMSTR_START_END(summary));
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
  TextAlignedMultiLine(0.0f, IMSTR_START_END(summary));
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

void FullscreenUI::DrawStringListSetting(SettingsInterface* bsi, std::string_view title, std::string_view summary,
                                         const char* section, const char* key, const char* default_value,
                                         std::span<const char* const> options,
                                         std::span<const char* const> option_values, bool enabled /* = true */,
                                         void (*changed_callback)(std::string_view) /* = nullptr */,
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
    ChoiceDialogOptions cd_options;
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
    ChoiceDialogOptions cd_options;
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
    ChoiceDialogOptions cd_options;
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
                       ClearCoverCache();
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
  ChoiceDialogOptions options;
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
  OpenConfirmMessageDialog(
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

void FullscreenUI::PopulateHotkeyList()
{
  if (!s_settings_locals.hotkey_list_cache.empty())
    return;

  // sort hotkeys by category so we don't duplicate the groups
  const auto hotkeys = InputManager::GetHotkeyList();
  s_settings_locals.hotkey_list_cache.reserve(hotkeys.size());

  // this mess is needed to preserve the category order
  for (size_t i = 0; i < hotkeys.size(); i++)
  {
    const HotkeyInfo* hk = hotkeys[i];
    size_t j;
    for (j = 0; j < s_settings_locals.hotkey_list_cache.size(); j++)
    {
      if (std::strcmp(hk->category, s_settings_locals.hotkey_list_cache[j]->category) == 0)
        break;
    }
    if (j != s_settings_locals.hotkey_list_cache.size())
    {
      // already done
      continue;
    }

    // add all hotkeys with this category
    for (const HotkeyInfo* other_hk : hotkeys)
    {
      if (std::strcmp(hk->category, other_hk->category) != 0)
        continue;

      s_settings_locals.hotkey_list_cache.push_back(other_hk);
    }
  }
}

void FullscreenUI::ClearSettingsState()
{
  s_settings_locals.input_binding_dialog.ClearState();
  std::memset(s_settings_locals.controller_macro_expanded, 0, sizeof(s_settings_locals.controller_macro_expanded));
  s_settings_locals.game_list_directories_cache = {};
  s_settings_locals.game_settings_entry.reset();
  s_settings_locals.game_settings_interface.reset();
  s_settings_locals.game_settings_changed = false;
  s_settings_locals.game_patch_list = {};
  s_settings_locals.enabled_game_patch_cache = {};
  s_settings_locals.game_cheats_list = {};
  s_settings_locals.enabled_game_cheat_cache = {};
  s_settings_locals.game_cheat_groups = {};
  s_settings_locals.postprocessing_stages = {};
  s_settings_locals.fullscreen_mode_list_cache = {};
  s_settings_locals.graphics_adapter_list_cache = {};
  s_settings_locals.hotkey_list_cache = {};
}

void FullscreenUI::SwitchToSettings()
{
  s_settings_locals.game_settings_entry.reset();
  s_settings_locals.game_settings_interface.reset();
  s_settings_locals.settings_changed = false;
  s_settings_locals.game_patch_list = {};
  s_settings_locals.enabled_game_patch_cache = {};
  s_settings_locals.game_cheats_list = {};
  s_settings_locals.enabled_game_cheat_cache = {};
  s_settings_locals.game_cheat_groups = {};

  PopulateGraphicsAdapterList();
  PopulateHotkeyList();

  const auto lock = Host::GetSettingsLock();
  const SettingsInterface* const sif = GetEditingSettingsInterface();
  PopulateGameListDirectoryCache(*sif);
  PopulatePostProcessingChain(*sif, PostProcessing::Config::DISPLAY_CHAIN_SECTION);

  SwitchToMainWindow(MainWindowType::Settings);
  s_settings_locals.settings_page = SettingsPage::Interface;
  s_settings_locals.settings_last_bg_alpha = GetBackgroundAlpha();
}

bool FullscreenUI::SwitchToGameSettings(SettingsPage page)
{
  return SwitchToGameSettingsForPath(GPUThread::GetGamePath());
}

bool FullscreenUI::SwitchToGameSettingsForPath(const std::string& path, SettingsPage page)
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = !path.empty() ? GameList::GetEntryForPath(path) : nullptr;
  if (!entry || entry->serial.empty())
  {
    ShowToast({}, FSUI_STR("Game properties is only available for scanned games."));
    return false;
  }

  SwitchToGameSettings(entry, page);
  return true;
}

void FullscreenUI::SwitchToGameSettings(const GameList::Entry* entry, SettingsPage page)
{
  s_settings_locals.game_settings_entry = std::make_unique<GameList::Entry>(*entry);
  s_settings_locals.game_settings_interface = System::GetGameSettingsInterface(
    s_settings_locals.game_settings_entry->dbentry, s_settings_locals.game_settings_entry->serial, true, false);
  PopulatePatchesAndCheatsList();
  s_settings_locals.settings_page = page;
  SwitchToMainWindow(MainWindowType::Settings);
}

void FullscreenUI::PopulateGraphicsAdapterList()
{
  auto lock = Host::GetSettingsLock();
  const GPURenderer renderer =
    Settings::ParseRendererName(GetEffectiveTinyStringSetting(GetEditingSettingsInterface(false), "GPU", "Renderer",
                                                              Settings::GetRendererName(Settings::DEFAULT_GPU_RENDERER))
                                  .c_str())
      .value_or(Settings::DEFAULT_GPU_RENDERER);

  s_settings_locals.graphics_adapter_list_cache =
    GPUDevice::GetAdapterListForAPI(Settings::GetRenderAPIForRenderer(renderer));
}

void FullscreenUI::PopulateGameListDirectoryCache(const SettingsInterface& si)
{
  s_settings_locals.game_list_directories_cache.clear();
  for (std::string& dir : si.GetStringList("GameList", "Paths"))
    s_settings_locals.game_list_directories_cache.emplace_back(std::move(dir), false);
  for (std::string& dir : si.GetStringList("GameList", "RecursivePaths"))
    s_settings_locals.game_list_directories_cache.emplace_back(std::move(dir), true);
}

void FullscreenUI::PopulatePatchesAndCheatsList()
{
  s_settings_locals.game_patch_list = Cheats::GetCodeInfoList(
    s_settings_locals.game_settings_entry->serial, s_settings_locals.game_settings_entry->hash, false, true, true);
  s_settings_locals.game_cheats_list = Cheats::GetCodeInfoList(
    s_settings_locals.game_settings_entry->serial, s_settings_locals.game_settings_entry->hash, true,
    s_settings_locals.game_settings_interface->GetBoolValue("Cheats", "LoadCheatsFromDatabase", true),
    s_settings_locals.game_settings_interface->GetBoolValue("Cheats", "SortList", false));
  s_settings_locals.game_cheat_groups = Cheats::GetCodeListUniquePrefixes(s_settings_locals.game_cheats_list, true);
  s_settings_locals.enabled_game_patch_cache = s_settings_locals.game_settings_interface->GetStringList(
    Cheats::PATCHES_CONFIG_SECTION, Cheats::PATCH_ENABLE_CONFIG_KEY);
  s_settings_locals.enabled_game_cheat_cache = s_settings_locals.game_settings_interface->GetStringList(
    Cheats::CHEATS_CONFIG_SECTION, Cheats::PATCH_ENABLE_CONFIG_KEY);
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
  if (!s_settings_locals.game_settings_interface)
    return;

  Settings temp_settings;
  temp_settings.Load(*GetEditingSettingsInterface(false), *GetEditingSettingsInterface(false));
  temp_settings.Save(*s_settings_locals.game_settings_interface, true);
  SetSettingsChanged(s_settings_locals.game_settings_interface.get());

  ShowToast(std::string(), fmt::format(FSUI_FSTR("Game settings initialized with global settings for '{}'."),
                                       Path::GetFileTitle(s_settings_locals.game_settings_interface->GetPath())));
}

void FullscreenUI::DoClearGameSettings()
{
  if (!s_settings_locals.game_settings_interface)
    return;

  s_settings_locals.game_settings_interface->Clear();
  if (!s_settings_locals.game_settings_interface->GetPath().empty())
    FileSystem::DeleteFile(s_settings_locals.game_settings_interface->GetPath().c_str());

  SetSettingsChanged(s_settings_locals.game_settings_interface.get());

  ShowToast(std::string(), fmt::format(FSUI_FSTR("Game settings have been cleared for '{}'."),
                                       Path::GetFileTitle(s_settings_locals.game_settings_interface->GetPath())));
}

FullscreenUI::SettingsPage FullscreenUI::GetCurrentSettingsPage()
{
  return s_settings_locals.settings_page;
}

bool FullscreenUI::IsInputBindingDialogOpen()
{
  return s_settings_locals.input_binding_dialog.IsOpen();
}

void FullscreenUI::DrawSettingsWindow()
{
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, UIStyle.LargeFontSize + (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));

  const float target_bg_alpha = GetBackgroundAlpha();
  s_settings_locals.settings_last_bg_alpha =
    (target_bg_alpha < s_settings_locals.settings_last_bg_alpha) ?
      std::max(s_settings_locals.settings_last_bg_alpha - io.DeltaTime * 2.0f, target_bg_alpha) :
      std::min(s_settings_locals.settings_last_bg_alpha + io.DeltaTime * 2.0f, target_bg_alpha);

  const bool show_localized_titles = GameList::ShouldShowLocalizedTitles();

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "settings_category",
                            ImVec4(UIStyle.PrimaryColor.x, UIStyle.PrimaryColor.y, UIStyle.PrimaryColor.z,
                                   s_settings_locals.settings_last_bg_alpha)))
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
      if (pages[i] == s_settings_locals.settings_page)
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
          s_settings_locals.settings_page = page;
          QueueResetFocus(FocusResetType::Other);
        });
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) ||
               ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakFast, true) ||
               ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
      {
        BeginTransition([page = pages[(index + 1) % count]]() {
          s_settings_locals.settings_page = page;
          QueueResetFocus(FocusResetType::Other);
        });
      }
    }

    if (NavButton(ICON_PF_NAVIGATION_BACK, true, true))
      ReturnToPreviousWindow();

    NavTitle(s_settings_locals.game_settings_entry ?
               s_settings_locals.game_settings_entry->GetDisplayTitle(show_localized_titles) :
               Host::TranslateToStringView(FSUI_TR_CONTEXT, titles[static_cast<u32>(pages[index])].first));

    RightAlignNavButtons(count);

    for (u32 i = 0; i < count; i++)
    {
      if (NavButton(titles[static_cast<u32>(pages[i])].second, i == index, true))
      {
        BeginTransition([page = pages[i]]() {
          s_settings_locals.settings_page = page;
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
        TinyString::from_format("settings_page_{}", static_cast<u32>(s_settings_locals.settings_page)).c_str(),
        ImVec4(UIStyle.BackgroundColor.x, UIStyle.BackgroundColor.y, UIStyle.BackgroundColor.z,
               s_settings_locals.settings_last_bg_alpha),
        0.0f, ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, 0.0f)))
  {
    if (ImGui::IsWindowFocused() && WantsToCloseMenu())
      ReturnToPreviousWindow();

    auto lock = Host::GetSettingsLock();

    switch (s_settings_locals.settings_page)
    {
      case SettingsPage::Summary:
        DrawSummarySettingsPage(show_localized_titles);
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
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Back"))});
  }
  else
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT, FSUI_VSTR("Change Page")),
                                       std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, FSUI_VSTR("Navigate")),
                                       std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_ESC, FSUI_VSTR("Back"))});
  }

  if (IsFixedPopupDialogOpen(COVER_DOWNLOADER_DIALOG_NAME))
    DrawCoverDownloaderWindow();
  else if (IsFixedPopupDialogOpen(ACHIEVEMENTS_LOGIN_DIALOG_NAME))
    DrawAchievementsLoginWindow();
  else
    s_settings_locals.input_binding_dialog.Draw();

  if (s_settings_locals.settings_changed.load(std::memory_order_relaxed))
  {
    Host::CommitBaseSettingChanges();
    Host::RunOnCPUThread([]() { System::ApplySettings(false); });
    s_settings_locals.settings_changed.store(false, std::memory_order_release);
  }
  if (s_settings_locals.game_settings_changed.load(std::memory_order_relaxed))
  {
    if (s_settings_locals.game_settings_interface)
    {
      Error error;
      s_settings_locals.game_settings_interface->RemoveEmptySections();

      if (s_settings_locals.game_settings_interface->IsEmpty())
      {
        if (FileSystem::FileExists(s_settings_locals.game_settings_interface->GetPath().c_str()))
        {
          INFO_LOG("Removing empty game settings {}", s_settings_locals.game_settings_interface->GetPath());
          if (!FileSystem::DeleteFile(s_settings_locals.game_settings_interface->GetPath().c_str(), &error))
          {
            OpenInfoMessageDialog(FSUI_STR("Error"),
                                  fmt::format(FSUI_FSTR("An error occurred while deleting empty game settings:\n{}"),
                                              error.GetDescription()));
          }
        }
      }
      else
      {
        if (!s_settings_locals.game_settings_interface->Save(&error))
        {
          OpenInfoMessageDialog(
            FSUI_STR("Error"),
            fmt::format(FSUI_FSTR("An error occurred while saving game settings:\n{}"), error.GetDescription()));
        }
      }

      if (GPUThread::HasGPUBackend())
        Host::RunOnCPUThread([]() { System::ReloadGameSettings(false); });
    }
    s_settings_locals.game_settings_changed.store(false, std::memory_order_release);
  }
}

void FullscreenUI::DrawSummarySettingsPage(bool show_localized_titles)
{
  BeginMenuButtons();
  ResetFocusHere();

  MenuHeading(FSUI_VSTR("Details"));

  if (s_settings_locals.game_settings_entry)
  {
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_WINDOW_MAXIMIZE, "Title"),
                   s_settings_locals.game_settings_entry->GetDisplayTitle(show_localized_titles), true))
    {
      CopyTextToClipboard(FSUI_STR("Game title copied to clipboard."),
                          s_settings_locals.game_settings_entry->GetDisplayTitle(show_localized_titles));
    }
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_PAGER, "Serial"), s_settings_locals.game_settings_entry->serial, true))
      CopyTextToClipboard(FSUI_STR("Game serial copied to clipboard."), s_settings_locals.game_settings_entry->serial);
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_COMPACT_DISC, "Type"),
                   GameList::GetEntryTypeDisplayName(s_settings_locals.game_settings_entry->type), true))
    {
      CopyTextToClipboard(FSUI_STR("Game type copied to clipboard."),
                          GameList::GetEntryTypeDisplayName(s_settings_locals.game_settings_entry->type));
    }
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_GLOBE, "Region"),
                   Settings::GetDiscRegionDisplayName(s_settings_locals.game_settings_entry->region), true))
    {
      CopyTextToClipboard(FSUI_STR("Game region copied to clipboard."),
                          Settings::GetDiscRegionDisplayName(s_settings_locals.game_settings_entry->region));
    }
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_STAR, "Compatibility Rating"),
                   GameDatabase::GetCompatibilityRatingDisplayName(
                     s_settings_locals.game_settings_entry->dbentry ?
                       s_settings_locals.game_settings_entry->dbentry->compatibility :
                       GameDatabase::CompatibilityRating::Unknown),
                   true))
    {
      CopyTextToClipboard(FSUI_STR("Game compatibility rating copied to clipboard."),
                          GameDatabase::GetCompatibilityRatingDisplayName(
                            s_settings_locals.game_settings_entry->dbentry ?
                              s_settings_locals.game_settings_entry->dbentry->compatibility :
                              GameDatabase::CompatibilityRating::Unknown));
    }
    if (MenuButton(FSUI_ICONVSTR(ICON_FA_FILE, "Path"), s_settings_locals.game_settings_entry->path, true))
    {
      CopyTextToClipboard(FSUI_STR("Game path copied to clipboard."), s_settings_locals.game_settings_entry->path);
    }
  }
  else
  {
    MenuButton(FSUI_ICONVSTR(ICON_FA_BAN, "Details unavailable for game not scanned in game list."), "");
  }

  MenuHeading(FSUI_VSTR("Options"));

  DebugAssert(s_settings_locals.game_settings_entry);
  if (s_settings_locals.game_settings_entry->dbentry && s_settings_locals.game_settings_entry->dbentry->disc_set)
  {
    // only enable for first disc
    const bool is_first_disc = (s_settings_locals.game_settings_entry->dbentry->serial ==
                                s_settings_locals.game_settings_entry->dbentry->disc_set->serials.front());
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
      ChoiceDialogOptions options;
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
                        "", FullscreenUI::GetThemeDisplayNames(), FullscreenUI::GetThemeNames(), true,
                        [](std::string_view) { BeginTransition(LONG_TRANSITION_TIME, &FullscreenUI::UpdateTheme); });

  if (const TinyString current_value =
        bsi->GetTinyStringValue("Main", "FullscreenUIBackground", DEFAULT_BACKGROUND_NAME);
      MenuButtonWithValue(FSUI_ICONVSTR(ICON_FA_IMAGE, "Menu Background"),
                          FSUI_VSTR("Shows a background image or shader when a game isn't running. Backgrounds are "
                                    "located in resources/fullscreenui/backgrounds in the data directory."),
                          (current_value == NONE_BACKGROUND_NAME) ? FSUI_VSTR("None") : current_value.view()))
  {
    ChoiceDialogOptions options = GetBackgroundOptions(current_value);
    OpenChoiceDialog(FSUI_ICONVSTR(ICON_FA_IMAGE, "Menu Background"), false, std::move(options),
                     [](s32 index, const std::string& title, bool checked) {
                       if (index < 0)
                         return;

                       SettingsInterface* bsi = GetEditingSettingsInterface();
                       bsi->SetStringValue("Main", "FullscreenUIBackground",
                                           (index == 0) ? NONE_BACKGROUND_NAME : title.c_str());
                       SetSettingsChanged(bsi);

                       // Have to defer the reload, because we've already drawn the bg for this frame.
                       BeginTransition(LONG_TRANSITION_TIME, {});
                       Host::RunOnCPUThread([]() { GPUThread::RunOnThread(&FullscreenUI::UpdateBackground); });
                     });
  }

  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_LIST, "Open To Game List"),
    FSUI_VSTR("When Big Picture mode is started, the game list will be displayed instead of the main menu."), "Main",
    "FullscreenUIOpenToGameList", false);

  bool widgets_settings_changed = DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_PF_GAMEPAD, "Use DualShock/DualSense Button Icons"),
    FSUI_VSTR("Displays DualShock/DualSense button icons in the footer and input binding, instead of Xbox buttons."),
    "Main", "FullscreenUIDisplayPSIcons", false);

  widgets_settings_changed |=
    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_WINDOW_RESTORE, "Window Animations"),
                      FSUI_VSTR("Animates windows opening/closing and changes between views in the Big Picture UI."),
                      "Main", "FullscreenUIAnimations", true);

  widgets_settings_changed |= DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_LIST, "Smooth Scrolling"),
                                                FSUI_VSTR("Enables smooth scrolling of menus in the Big Picture UI."),
                                                "Main", "FullscreenUISmoothScrolling", true);
  widgets_settings_changed |=
    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_BORDER_ALL, "Menu Borders"),
                      FSUI_VSTR("Draws a border around the currently-selected item for readability."), "Main",
                      "FullscreenUIMenuBorders", false);

  // use transition to work around double lock
  if (widgets_settings_changed)
    BeginTransition(0.0f, &FullscreenUI::UpdateWidgetsSettings);

  MenuHeading(FSUI_VSTR("Behavior"));

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_POWER_OFF, "Confirm Game Close"),
                    FSUI_VSTR("Determines whether a prompt will be displayed to confirm closing the game."), "Main",
                    "ConfirmPowerOff", true);
  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_FLOPPY_DISK, "Save State On Game Close"),
                    FSUI_VSTR("Automatically saves the system state when closing the game or exiting. You can then "
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
    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_LANGUAGE, "Show Localized Titles"),
                      FSUI_VSTR("Uses localized (native language) titles in the game list."), "UI",
                      "GameListShowLocalizedTitles", true);
    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_TROPHY, "Show Achievement Trophy Icons"),
                      FSUI_VSTR("Shows trophy icons in game grid when games have achievements or have been mastered."),
                      "Main", "FullscreenUIShowTrophyIcons", true);
    DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_TAGS, "Show Grid View Titles"),
                      FSUI_VSTR("Shows titles underneath the images in the game grid view."), "Main",
                      "FullscreenUIShowGridTitles", true);
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
        PopulateGameListDirectoryCache(*bsi);
        Host::RefreshGameListAsync(false);
      }
    });
  }

  for (const auto& it : s_settings_locals.game_list_directories_cache)
  {
    if (MenuButton(SmallString::from_format(ICON_FA_FOLDER " {}", it.first),
                   it.second ? FSUI_VSTR("Scanning Subdirectories") : FSUI_VSTR("Not Scanning Subdirectories")))
    {
      ChoiceDialogOptions options = {
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
                             PopulateGameListDirectoryCache(*bsi);
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
                           PopulateGameListDirectoryCache(*bsi);
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
    std::unique_ptr<ProgressCallback> progress = OpenModalProgressDialog(FSUI_STR("Cover Downloader"), 1000.0f);
    System::QueueAsyncTask([progress = progress.release(), urls = StringUtil::SplitNewString(template_urls, '\n'),
                            use_serial_names = use_serial_names]() {
      GameList::DownloadCovers(
        urls, use_serial_names, progress, [](const GameList::Entry* entry, std::string save_path) {
          // cache the cover path on our side once it's saved
          Host::RunOnCPUThread([path = entry->path, save_path = std::move(save_path)]() mutable {
            GPUThread::RunOnThread([path = std::move(path), save_path = std::move(save_path)]() mutable {
              FullscreenUI::SetCoverCacheEntry(std::move(path), std::move(save_path));
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
      ChoiceDialogOptions options;
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
    FSUI_NSTR("Maximum (Safer)"),
  };

  static constexpr const std::array cdrom_seek_speeds = {
    FSUI_NSTR("None (Normal Speed)"),
    FSUI_NSTR("2x"),
    FSUI_NSTR("3x"),
    FSUI_NSTR("4x"),
    FSUI_NSTR("5x"),
    FSUI_NSTR("6x"),
    FSUI_NSTR("Maximum (Safer)"),
  };

  static constexpr std::array cdrom_read_seek_speed_values = {1, 2, 3, 4, 5, 6, 0};

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
    FSUI_NSTR("10% [6 FPS (NTSC) / 5 FPS (PAL)]"),
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

  const s32 runahead_frames = GetEffectiveIntSetting(bsi, "Main", "RunaheadFrameCount", 0);
  const bool runahead_enabled = (runahead_frames > 0);
  const bool rewind_enabled = GetEffectiveBoolSetting(bsi, "Main", "RewindEnable", false);

  DrawToggleSetting(bsi, FSUI_ICONVSTR(ICON_FA_BACKWARD, "Enable Rewinding"),
                    FSUI_VSTR("Saves state periodically so you can rewind any mistakes while playing."), "Main",
                    "RewindEnable", false, !runahead_enabled);

  DrawFloatRangeSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_FLOPPY_DISK, "Rewind Save Frequency"),
    FSUI_VSTR("How often a rewind state will be created. Higher frequencies have greater system requirements."), "Main",
    "RewindFrequency", 10.0f, 0.0f, 3600.0f, FSUI_CSTR("%.2f Seconds"), 1.0f, rewind_enabled && !runahead_enabled);
  DrawIntRangeSetting(
    bsi, FSUI_ICONVSTR(ICON_FA_WHISKEY_GLASS, "Rewind Save Slots"),
    FSUI_VSTR("How many saves will be kept for rewinding. Higher values have greater memory requirements."), "Main",
    "RewindSaveSlots", 10, 1, 10000, FSUI_CSTR("%d Frames"), rewind_enabled && !runahead_enabled);

  static constexpr const std::array runahead_options = {
    FSUI_NSTR("Disabled"), FSUI_NSTR("1 Frame"),  FSUI_NSTR("2 Frames"), FSUI_NSTR("3 Frames"),
    FSUI_NSTR("4 Frames"), FSUI_NSTR("5 Frames"), FSUI_NSTR("6 Frames"), FSUI_NSTR("7 Frames"),
    FSUI_NSTR("8 Frames"), FSUI_NSTR("9 Frames"), FSUI_NSTR("10 Frames")};

  DrawIntListSetting(bsi, FSUI_ICONVSTR(ICON_FA_PERSON_RUNNING, "Runahead"),
                     FSUI_VSTR("Simulates the system ahead of time and rolls back/replays to reduce input lag. Very "
                               "high system requirements."),
                     "Main", "RunaheadFrameCount", 0, runahead_options);

  DrawToggleSetting(
    bsi, FSUI_ICONVSTR(ICON_PF_ANALOG_ANY, "Runahead for Analog Input"),
    FSUI_VSTR("Activates runahead when analog input changes, which significantly increases system requirements."),
    "Main", "RunaheadForAnalogInput", false, runahead_enabled);

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

  ChoiceDialogOptions coptions;
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

  ChoiceDialogOptions coptions;
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
                    "SDLPS5PlayerLED", false, bsi->GetBoolValue("InputSources", "SDL", true), false);
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
      ChoiceDialogOptions options;
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

      bool& expanded = s_settings_locals.controller_macro_expanded[global_slot][macro_index];
      expanded ^= MenuHeadingButton(
        SmallString::from_format(fmt::runtime(FSUI_ICONVSTR(ICON_PF_EMPTY_KEYCAP, "Macro Button {}")), macro_index + 1),
        s_settings_locals.controller_macro_expanded[global_slot][macro_index] ? ICON_FA_CHEVRON_UP :
                                                                                ICON_FA_CHEVRON_DOWN);
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
        ChoiceDialogOptions options;
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
  for (const HotkeyInfo* hotkey : s_settings_locals.hotkey_list_cache)
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
      ChoiceDialogOptions options;
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
    ChoiceDialogOptions options;
    options.reserve(s_settings_locals.graphics_adapter_list_cache.size() + 2);
    if (game_settings)
      options.emplace_back(FSUI_STR("Use Global Setting"), !current_adapter.has_value());
    options.emplace_back(FSUI_STR("Default"), current_adapter.has_value() && current_adapter->empty());
    for (const GPUDevice::AdapterInfo& adapter : s_settings_locals.graphics_adapter_list_cache)
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

  static constexpr const char* ASPECT_RATIO_SECTION = "Display";
  static constexpr const char* ASPECT_RATIO_KEY = "AspectRatio";
  if (MenuButtonWithValue(FSUI_ICONVSTR(ICON_FA_SHAPES, "Aspect Ratio"),
                          FSUI_VSTR("Changes the aspect ratio used to display the console's output to the screen."),
                          (game_settings && !bsi->ContainsValue(ASPECT_RATIO_SECTION, ASPECT_RATIO_KEY)) ?
                            TinyString(FSUI_VSTR("Use Global Setting")) :
                            Settings::GetDisplayAspectRatioDisplayName(
                              Settings::ParseDisplayAspectRatio(
                                GetEffectiveTinyStringSetting(bsi, ASPECT_RATIO_SECTION, ASPECT_RATIO_KEY, ""))
                                .value_or(Settings::DEFAULT_DISPLAY_ASPECT_RATIO))))
  {
    static constexpr const DisplayAspectRatio INHERIT_ASPECT_RATIO = {0, -1};
    ChoiceDialogOptions options;
    const DisplayAspectRatio current_ar =
      (bsi && !bsi->ContainsValue(ASPECT_RATIO_SECTION, ASPECT_RATIO_KEY)) ?
        INHERIT_ASPECT_RATIO :
        Settings::ParseDisplayAspectRatio(
          GetEffectiveTinyStringSetting(bsi, ASPECT_RATIO_SECTION, ASPECT_RATIO_KEY, "").c_str())
          .value_or(Settings::DEFAULT_DISPLAY_ASPECT_RATIO);
    if (game_settings)
    {
      options.emplace_back(FSUI_STR("Use Global Setting"), current_ar == INHERIT_ASPECT_RATIO);
    }
    for (const DisplayAspectRatio& ratio : Settings::GetPredefinedDisplayAspectRatios())
      options.emplace_back(Settings::GetDisplayAspectRatioDisplayName(ratio), current_ar == ratio);
    OpenChoiceDialog(FSUI_ICONVSTR(ICON_FA_SHAPES, "Aspect Ratio"), false, std::move(options),
                     [game_settings](s32 index, const std::string& title, bool checked) {
                       if (index < 0)
                         return;

                       auto lock = Host::GetSettingsLock();
                       SettingsInterface* bsi = GetEditingSettingsInterface(game_settings);
                       if (game_settings && index == 0)
                       {
                         bsi->DeleteValue(ASPECT_RATIO_SECTION, ASPECT_RATIO_KEY);
                       }
                       else
                       {
                         bsi->SetStringValue(
                           ASPECT_RATIO_SECTION, ASPECT_RATIO_KEY,
                           Settings::GetDisplayAspectRatioName(
                             Settings::GetPredefinedDisplayAspectRatios()[game_settings ? (index - 1) : index]));
                       }

                       SetSettingsChanged(bsi);
                     });
  }

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
      for (const GPUDevice::AdapterInfo& ai : s_settings_locals.graphics_adapter_list_cache)
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
      if (!s_settings_locals.graphics_adapter_list_cache.empty())
        selected_adapter = &s_settings_locals.graphics_adapter_list_cache.front();
    }

    ChoiceDialogOptions options;
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

void FullscreenUI::PopulatePostProcessingChain(const SettingsInterface& si, const char* section)
{
  const u32 stages = PostProcessing::Config::GetStageCount(si, section);
  s_settings_locals.postprocessing_stages.clear();
  s_settings_locals.postprocessing_stages.reserve(stages);
  for (u32 i = 0; i < stages; i++)
  {
    PostProcessingStageInfo psi;
    psi.name = PostProcessing::Config::GetStageShaderName(si, section, i);
    psi.options = PostProcessing::Config::GetStageOptions(si, section, i);
    psi.enabled = PostProcessing::Config::IsStageEnabled(si, section, i);
    s_settings_locals.postprocessing_stages.push_back(std::move(psi));
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
    std::vector<std::pair<std::string, PostProcessing::ShaderType>> shaders = PostProcessing::GetAvailableShaderNames();
    ChoiceDialogOptions options;
    options.reserve(shaders.size());
    for (auto& [name, type] : shaders)
    {
      std::string display_name = fmt::format("{} [{}]", name, PostProcessing::GetShaderTypeDisplayName(type));
      options.emplace_back(std::move(display_name), false);
    }

    OpenChoiceDialog(FSUI_ICONVSTR(ICON_PF_ADD, "Add Shader"), false, std::move(options),
                     [shaders = std::move(shaders)](s32 index, const std::string& title, bool checked) {
                       if (index < 0 || static_cast<u32>(index) >= shaders.size())
                         return;

                       const std::string& shader_name = shaders[index].first;
                       SettingsInterface* bsi = GetEditingSettingsInterface();
                       Error error;
                       if (PostProcessing::Config::AddStage(*bsi, section, shader_name, &error))
                       {
                         ShowToast(std::string(), fmt::format(FSUI_FSTR("Shader {} added as stage {}."), title,
                                                              PostProcessing::Config::GetStageCount(*bsi, section)));
                         PopulatePostProcessingChain(*bsi, section);
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
        PopulatePostProcessingChain(*bsi, section);
        SetSettingsChanged(bsi);
        ShowToast(std::string(), FSUI_STR("Post-processing chain cleared."));
        queue_reload();
      });
  }

  u32 postprocessing_action = POSTPROCESSING_ACTION_NONE;
  u32 postprocessing_action_index = 0;

  SmallString str;
  SmallString tstr;
  for (u32 stage_index = 0; stage_index < static_cast<u32>(s_settings_locals.postprocessing_stages.size());
       stage_index++)
  {
    PostProcessingStageInfo& si = s_settings_locals.postprocessing_stages[stage_index];

    ImGui::PushID(stage_index);
    str.format(FSUI_FSTR("Stage {}: {}"), stage_index + 1, si.name);
    MenuHeading(str);

    tstr.format("PostProcessing/Stage{}", stage_index + 1);

    if (ToggleButton(FSUI_ICONVSTR(ICON_FA_WAND_MAGIC_SPARKLES, "Enable Stage"),
                     FSUI_VSTR("If disabled, the shader in this stage will not be applied."), &si.enabled))
    {
      PostProcessing::Config::SetStageEnabled(*bsi, section, stage_index, si.enabled);
      SetSettingsChanged(bsi);
      reload_pending = true;
    }

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
                   (stage_index != (s_settings_locals.postprocessing_stages.size() - 1))))
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
            reload_pending = true;
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
      const PostProcessingStageInfo& si = s_settings_locals.postprocessing_stages[postprocessing_action_index];
      ShowToast(std::string(),
                fmt::format(FSUI_FSTR("Removed stage {} ({})."), postprocessing_action_index + 1, si.name));
      PostProcessing::Config::RemoveStage(*bsi, section, postprocessing_action_index);
      PopulatePostProcessingChain(*bsi, section);
      SetSettingsChanged(bsi);
      reload_pending = true;
    }
    break;
    case POSTPROCESSING_ACTION_MOVE_UP:
    {
      PostProcessing::Config::MoveStageUp(*bsi, section, postprocessing_action_index);
      PopulatePostProcessingChain(*bsi, section);
      SetSettingsChanged(bsi);
      reload_pending = true;
    }
    break;
    case POSTPROCESSING_ACTION_MOVE_DOWN:
    {
      PostProcessing::Config::MoveStageDown(*bsi, section, postprocessing_action_index);
      PopulatePostProcessingChain(*bsi, section);
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
  const bool logged_in = (bsi->ContainsValue("Cheevos", "Token"));
  {
    // avoid locking order issues
    settings_lock.unlock();
    {
      const auto lock = Achievements::GetLock();
      std::string_view badge_path = Achievements::GetLoggedInUserBadgePath();
      if (badge_path.empty())
        badge_path = "images/ra-generic-user.png";

      TinyString username;
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
        username = bsi->GetTinyStringValue("Cheevos", "Username");
      }

      SmallString score_summary = Achievements::GetLoggedInUserPointsSummary();
      if (score_summary.empty())
      {
        if (logged_in)
          score_summary = FSUI_VSTR("Enable Achievements to see your user summary.");
        else
          score_summary = FSUI_VSTR("To use achievements, please log in with your retroachievements.org account.");
      }

      if (GPUTexture* badge_tex = GetCachedTextureAsync(badge_path))
      {
        const ImRect badge_rect = CenterImage(ImRect(pos, pos + ImVec2(badge_size, badge_size)), badge_tex);
        dl->AddImage(reinterpret_cast<ImTextureID>(GetCachedTextureAsync(badge_path)), badge_rect.Min, badge_rect.Max);
      }

      pos.x += badge_size + LayoutScale(15.0f);

      RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, pos,
                                pos + ImVec2(max_content_width - pos.x, UIStyle.LargeFontSize),
                                ImGui::GetColorU32(ImGuiCol_Text), username);

      pos.y += UIStyle.LargeFontSize + line_spacing;

      RenderAutoLabelText(dl, UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.NormalFontWeight,
                          UIStyle.BoldFontWeight, pos, pos + ImVec2(max_content_width - pos.x, UIStyle.LargeFontSize),
                          ImGui::GetColorU32(ImGuiCol_Text), score_summary, ':');
    }
    settings_lock.lock();
  }

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
          ShowToast(FSUI_STR("Failed to update progress database"), error.TakeDescription(), Host::OSD_ERROR_DURATION);
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

  const bool is_logging_in = IsBackgroundProgressDialogOpen(LOGIN_PROGRESS_NAME);
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
    OpenBackgroundProgressDialog(LOGIN_PROGRESS_NAME, FSUI_STR("Logging in to RetroAchievements..."), 0, 0, 0);

    Host::RunOnCPUThread([username = std::string(username), password = std::string(password)]() {
      Error error;
      const bool result = Achievements::Login(username.c_str(), password.c_str(), &error);
      GPUThread::RunOnThread([result, error = std::move(error)]() {
        CloseBackgroundProgressDialog(LOGIN_PROGRESS_NAME);

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
                  "LogLevel", Log::DEFAULT_LOG_LEVEL, &Settings::ParseLogLevelName, &Settings::GetLogLevelName,
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

  const Cheats::CodeInfoList& code_list =
    cheats ? s_settings_locals.game_cheats_list : s_settings_locals.game_patch_list;
  std::vector<std::string>& enable_list =
    cheats ? s_settings_locals.enabled_game_cheat_cache : s_settings_locals.enabled_game_patch_cache;
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
        ChoiceDialogOptions options;
        options.reserve(ci.options.size() + 1);
        options.emplace_back(FSUI_STR("Disabled"), !has_option);

        for (const Cheats::CodeOption& opt : ci.options)
          options.emplace_back(opt.first, has_option && (visible_value.view() == opt.first));

        OpenChoiceDialog(
          ci.name, false, std::move(options),
          [cheat_name = ci.name, cheats, section](s32 index, const std::string& title, bool checked) {
            if (index < 0)
              return;

            const Cheats::CodeInfo* ci = Cheats::FindCodeInInfoList(
              cheats ? s_settings_locals.game_cheats_list : s_settings_locals.game_patch_list, cheat_name);
            if (ci)
            {
              SettingsInterface* bsi = GetEditingSettingsInterface();
              std::vector<std::string>& enable_list =
                cheats ? s_settings_locals.enabled_game_cheat_cache : s_settings_locals.enabled_game_patch_cache;
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
      s_settings_locals.game_cheats_list =
        Cheats::GetCodeInfoList(s_settings_locals.game_settings_entry->serial,
                                s_settings_locals.game_settings_entry->hash, true, load_database_cheats, sort_list);
    }

    if (code_list.empty())
    {
      MenuButtonWithoutSummary(FSUI_ICONVSTR(ICON_FA_STORE_SLASH, "No cheats are available for this game."), false);
    }
    else
    {
      for (const std::string_view& group : s_settings_locals.game_cheat_groups)
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

#endif // __ANDROID__
