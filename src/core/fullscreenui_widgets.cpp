// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "fullscreenui_widgets.h"
#include "core.h"
#include "fullscreenui.h"
#include "gpu_backend.h"
#include "host.h"
#include "imgui_overlays.h"
#include "sound_effect_manager.h"
#include "system.h"
#include "video_presenter.h"
#include "video_thread.h"

#include "util/gpu_device.h"
#include "util/image.h"
#include "util/imgui_animated.h"
#include "util/imgui_manager.h"
#include "util/shadergen.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/lru_cache.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/threading.h"
#include "common/timer.h"

#include "fmt/core.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome.h"
#include "IconsPromptFont.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>
#include <variant>

using namespace std::string_view_literals;

LOG_CHANNEL(FullscreenUI);

namespace FullscreenUI {

static constexpr float MENU_BACKGROUND_ANIMATION_TIME = 0.25f;
static constexpr float SMOOTH_SCROLLING_SPEED = 3.5f;
static constexpr u32 LOADING_PROGRESS_SAMPLE_COUNT = 30;

static constexpr int MENU_BUTTON_SPLIT_LAYER_BACKGROUND = 0;
static constexpr int MENU_BUTTON_SPLIT_LAYER_HIGHLIGHT = 1;
static constexpr int MENU_BUTTON_SPLIT_LAYER_FOREGROUND = 2;
static constexpr int NUM_MENU_BUTTON_SPLIT_LAYERS = 3;

enum class SplitWindowFocusChange : u8
{
  None,
  FocusSidebar,
  FocusContent,
};

static std::optional<Image> LoadTextureImage(std::string_view path, u32 svg_width, u32 svg_height);
static std::shared_ptr<GPUTexture> UploadTexture(std::string_view path, const Image& image);

static bool CompileTransitionPipelines(Error* error);

static void CreateFooterTextString(SmallStringBase& dest,
                                   std::span<const std::pair<const char*, std::string_view>> items);

static void BeginMenuButtonDrawSplit();
static void EndMenuButtonDrawSplit();
static void SetMenuButtonSplitLayer(int layer);
static void DrawMenuButtonFrameAt(const ImVec2& frame_min, const ImVec2& frame_max, u32 col, bool border);
static void DrawMenuButtonFrameAtOnCurrentLayer(const ImVec2& frame_min, const ImVec2& frame_max, u32 col, bool border);
static void PostDrawMenuButtonFrame();

static void DrawBackgroundProgressDialogs(float& current_y);
static void UpdateLoadingScreenProgress(s32 progress_min, s32 progress_max, s32 progress_value);
static bool GetLoadingScreenTimeEstimate(SmallString& out_str);
static void DrawLoadingScreen(std::string_view image, std::string_view title, std::string_view caption,
                              s32 progress_min, s32 progress_max, s32 progress_value, bool is_persistent);

// Returns true if any overlay windows are active, such as notifications or toasts.
static bool AreAnyNotificationsActive();

/// Updates the run-idle trigger for notifications.
static void UpdateNotificationsRunIdle();
static void UpdateLoadingScreenRunIdle();

static void DrawToast(float& current_y);
static void DrawLoadingScreen();

static ImGuiID GetBackgroundProgressID(std::string_view str_id);

static constexpr std::array s_theme_display_names = {
  FSUI_NSTR("Automatic"),  FSUI_NSTR("Dark"),        FSUI_NSTR("Light"),       FSUI_NSTR("AMOLED"),
  FSUI_NSTR("Cobalt Sky"), FSUI_NSTR("Grey Matter"), FSUI_NSTR("Green Giant"), FSUI_NSTR("Pinky Pals"),
  FSUI_NSTR("Dark Ruby"),  FSUI_NSTR("Purple Rain"),
};

static constexpr std::array s_theme_names = {
  "", "Dark", "Light", "AMOLED", "CobaltSky", "GreyMatter", "GreenGiant", "PinkyPals", "DarkRuby", "PurpleRain",
};

// [0] = Mapping from Xbox button icons to PlayStation button icons.
// [1] = Swapped south/east face buttons.
using ControllerButtonMappingTable = std::array<std::pair<const char*, const char*>, 17>;
static constexpr ControllerButtonMappingTable GetButtonMapping(bool ps_buttons, bool swap_south_east)
{
  return ControllerButtonMappingTable{{
    {ICON_PF_LEFT_TRIGGER_LT, ps_buttons ? ICON_PF_LEFT_TRIGGER_L2 : ICON_PF_LEFT_TRIGGER_LT},
    {ICON_PF_RIGHT_TRIGGER_RT, ps_buttons ? ICON_PF_RIGHT_TRIGGER_R2 : ICON_PF_RIGHT_TRIGGER_RT},
    {ICON_PF_LEFT_SHOULDER_LB, ps_buttons ? ICON_PF_LEFT_SHOULDER_L1 : ICON_PF_LEFT_SHOULDER_LB},
    {ICON_PF_RIGHT_SHOULDER_RB, ps_buttons ? ICON_PF_RIGHT_SHOULDER_R1 : ICON_PF_RIGHT_SHOULDER_RB},
    {ICON_PF_BUTTON_X, ps_buttons ? ICON_PF_BUTTON_SQUARE : ICON_PF_BUTTON_X},
    {ICON_PF_BUTTON_Y, ps_buttons ? ICON_PF_BUTTON_TRIANGLE : ICON_PF_BUTTON_Y},
    {ICON_PF_BUTTON_B, ps_buttons ? (swap_south_east ? ICON_PF_BUTTON_CROSS : ICON_PF_BUTTON_CIRCLE) :
                                    (swap_south_east ? ICON_PF_BUTTON_A : ICON_PF_BUTTON_B)},
    {ICON_PF_BUTTON_A, ps_buttons ? (swap_south_east ? ICON_PF_BUTTON_CIRCLE : ICON_PF_BUTTON_CROSS) :
                                    (swap_south_east ? ICON_PF_BUTTON_B : ICON_PF_BUTTON_A)},
    {ICON_PF_SHARE_CAPTURE, ps_buttons ? ICON_PF_DUALSHOCK_SHARE : ICON_PF_SHARE_CAPTURE},
    {ICON_PF_BURGER_MENU, ps_buttons ? ICON_PF_DUALSHOCK_OPTIONS : ICON_PF_BURGER_MENU},
    {ICON_PF_XBOX_DPAD_LEFT, ps_buttons ? ICON_PF_DPAD_LEFT : ICON_PF_XBOX_DPAD_LEFT},
    {ICON_PF_XBOX_DPAD_UP, ps_buttons ? ICON_PF_DPAD_UP : ICON_PF_XBOX_DPAD_UP},
    {ICON_PF_XBOX_DPAD_RIGHT, ps_buttons ? ICON_PF_DPAD_RIGHT : ICON_PF_XBOX_DPAD_RIGHT},
    {ICON_PF_XBOX_DPAD_DOWN, ps_buttons ? ICON_PF_DPAD_DOWN : ICON_PF_XBOX_DPAD_DOWN},
    {ICON_PF_XBOX_DPAD_LEFT_RIGHT, ps_buttons ? ICON_PF_DPAD_LEFT_RIGHT : ICON_PF_XBOX_DPAD_LEFT_RIGHT},
    {ICON_PF_XBOX_DPAD_UP_DOWN, ps_buttons ? ICON_PF_DPAD_UP_DOWN : ICON_PF_XBOX_DPAD_UP_DOWN},
    {ICON_PF_XBOX, ps_buttons ? ICON_PF_PLAYSTATION : ICON_PF_XBOX},
  }};
}
static constexpr const ControllerButtonMappingTable s_button_mapping[2][2] = {
  {GetButtonMapping(false, false), GetButtonMapping(false, true)},
  {GetButtonMapping(true, false), GetButtonMapping(true, true)}};
static_assert(
  []() {
    for (size_t i = 0; i < std::size(s_button_mapping); i++)
    {
      for (size_t j = 0; j < std::size(s_button_mapping[0]); j++)
      {
        for (size_t k = 1; k < std::size(s_button_mapping[0][0]); k++)
        {
          if (StringUtil::ConstexprCompare(s_button_mapping[i][j][k - 1].first, s_button_mapping[i][j][k].first) >= 0)
            return false;
        }
      }
    }
    return true;
  }(),
  "Button mapping is not sorted");

namespace {

enum class CloseButtonState : u8
{
  None,
  KeyboardPressed,
  MousePressed,
  GamepadPressed,
  AnyReleased,
  Cancelled,
};

struct BackgroundProgressDialogData
{
  std::string message;
  ImGuiID id;
  s32 min;
  s32 max;
  s32 value;
};

class MessageDialog : public PopupDialog
{
public:
  using CallbackVariant = std::variant<InfoMessageDialogCallback, ConfirmMessageDialogCallback>;

  MessageDialog();
  ~MessageDialog();

  void Open(std::string_view icon, std::string_view title, std::string message, CallbackVariant callback,
            std::string first_button_text, std::string second_button_text, std::string third_button_text);
  void ClearState();

  void Draw();

private:
  static void InvokeCallback(const CallbackVariant& cb, std::optional<s32> choice);

  std::string m_icon;
  std::string m_message;
  std::array<std::string, 3> m_buttons;
  CallbackVariant m_callback;
};

class ChoiceDialog : public PopupDialog
{
public:
  ChoiceDialog();
  ~ChoiceDialog();

  void Open(std::string_view title, ChoiceDialogOptions options, ChoiceDialogCallback callback, bool checkable);
  void ClearState();

  void Draw();

private:
  ChoiceDialogOptions m_options;
  ChoiceDialogCallback m_callback;
  bool m_checkable = false;
};

class FileSelectorDialog : public PopupDialog
{
public:
  FileSelectorDialog();
  ~FileSelectorDialog();

  void Open(std::string_view title, FileSelectorCallback callback, FileSelectorFilters filters,
            std::string initial_directory, bool select_directory);
  void ClearState();

  void Draw();

private:
  struct Item
  {
    Item() = default;
    Item(std::string display_name_, std::string full_path_, bool is_file_);
    Item(const Item&) = default;
    Item(Item&&) = default;
    ~Item() = default;

    Item& operator=(const Item&) = default;
    Item& operator=(Item&&) = default;

    std::string display_name;
    std::string full_path;
    bool is_file;
  };

  void PopulateItems();
  void SetDirectory(std::string dir);

  std::string m_current_directory;
  std::vector<Item> m_items;
  std::vector<std::string> m_filters;
  FileSelectorCallback m_callback;

  bool m_is_directory = false;
  bool m_directory_changed = false;
  bool m_first_item_is_parent_directory = false;
};

class InputStringDialog : public PopupDialog
{
public:
  InputStringDialog();
  ~InputStringDialog();

  void Open(std::string_view title, std::string message, std::string caption, std::string ok_button_text,
            InputStringDialogCallback callback);
  void ClearState();

  void Draw();

private:
  std::string m_message;
  std::string m_caption;
  std::string m_text;
  std::string m_ok_text;
  InputStringDialogCallback m_callback;
};

class ProgressDialog : public PopupDialog
{
public:
  ProgressDialog();
  ~ProgressDialog();

  std::unique_ptr<ProgressCallbackWithPrompt> GetProgressCallback(std::string title, float window_unscaled_width);

  void Draw();

private:
  class ProgressCallbackImpl : public ProgressCallbackWithPrompt
  {
  public:
    ProgressCallbackImpl();
    ~ProgressCallbackImpl() override;

    void SetStatusText(std::string_view text) override;
    void SetProgressRange(u32 range) override;
    void SetProgressValue(u32 value) override;
    void SetCancellable(bool cancellable) override;
    bool IsCancelled() const override;

    void AlertPrompt(PromptIcon icon, std::string_view message) override;
    bool ConfirmPrompt(PromptIcon icon, std::string_view message, std::string_view yes_text = {},
                       std::string_view no_text = {}) override;
  };

  std::string m_status_text;
  float m_last_frac = 0.0f;
  float m_width = 0.0f;
  u32 m_progress_value = 0;
  u32 m_progress_range = 0;
  std::atomic_bool m_cancelled{false};
  std::atomic_bool m_prompt_result{false};
  std::atomic_flag m_prompt_waiting = ATOMIC_FLAG_INIT;
};

class FixedPopupDialog : public PopupDialog
{
public:
  FixedPopupDialog();
  ~FixedPopupDialog();

  void Open(std::string title);

  bool Begin(float scaled_window_padding = LayoutScale(20.0f), float scaled_window_rounding = LayoutScale(20.0f),
             const ImVec2& scaled_window_size = ImVec2(0.0f, 0.0f));
  void End();
};

struct WidgetsState
{
  std::recursive_mutex shared_state_mutex;

  bool has_initialized = false; // used to prevent notification queuing without GPU device
  CloseButtonState close_button_state = CloseButtonState::None;
  FocusResetType focus_reset_queued = FocusResetType::None;
  TransitionState transition_state = TransitionState::Inactive;
  ImGuiDir has_pending_nav_move = ImGuiDir_None;

  ImVec2 horizontal_menu_button_size = {};

  LRUCache<std::string, std::shared_ptr<GPUTexture>> texture_cache{128, true};
  std::shared_ptr<GPUTexture> placeholder_texture;
  std::deque<std::pair<std::string, Image>> texture_upload_queue;

  // Transition Resources
  TransitionStartCallback transition_start_callback;
  std::unique_ptr<GPUTexture> transition_prev_texture;
  std::unique_ptr<GPUTexture> transition_current_texture;
  std::unique_ptr<GPUPipeline> transition_blend_pipeline;
  float transition_total_time = 0.0f;
  float transition_remaining_time = 0.0f;

  SmallString fullscreen_footer_text;
  SmallString last_fullscreen_footer_text;
  SmallString left_fullscreen_footer_text;
  SmallString last_left_fullscreen_footer_text;
  std::span<const std::pair<const char*, const char*>> fullscreen_footer_icon_mapping;
  float fullscreen_text_change_time;

  ImGuiID enum_choice_button_id = 0;
  s32 enum_choice_button_value = 0;
  bool enum_choice_button_set = false;

  SplitWindowFocusChange split_window_focus_change = SplitWindowFocusChange::None;
  bool had_hovered_menu_item = false;
  bool has_hovered_menu_item = false;
  bool rendered_menu_item_border = false;
  bool had_focus_reset = false;
  bool sound_effects_enabled = false;
  bool had_sound_effect = false;

  ImAnimatedVec2 menu_button_frame_min_animated;
  ImAnimatedVec2 menu_button_frame_max_animated;

  ChoiceDialog choice_dialog;
  FileSelectorDialog file_selector_dialog;
  InputStringDialog input_string_dialog;
  FixedPopupDialog fixed_popup_dialog;
  ProgressDialog progress_dialog;
  MessageDialog message_dialog;

  std::string toast_title;
  std::string toast_message;
  Timer::Value toast_start_time;
  float toast_duration;

  std::vector<BackgroundProgressDialogData> background_progress_dialogs;

  std::string loading_screen_image;
  std::string loading_screen_title;
  std::string loading_screen_caption;
  s32 loading_screen_min = 0;
  s32 loading_screen_max = 0;
  s32 loading_screen_value = 0;

  u32 loading_screen_sample_index = 0;
  u32 loading_screen_valid_samples = 0;
  bool loading_screen_open = false;

  std::array<std::pair<Timer::Value, s32>, LOADING_PROGRESS_SAMPLE_COUNT> loading_screen_samples;
};

} // namespace

ALIGN_TO_CACHE_LINE UIStyles UIStyle = {};
ALIGN_TO_CACHE_LINE static WidgetsState s_state;

} // namespace FullscreenUI

void FullscreenUI::SetFont(ImFont* ui_font)
{
  UIStyle.Font = ui_font;
}

bool FullscreenUI::InitializeWidgets(Error* error)
{
  if (!CreateWidgetsGPUResources(error))
    return false;

  s_state.focus_reset_queued = FocusResetType::ViewChanged;
  s_state.close_button_state = CloseButtonState::None;
  ResetMenuButtonFrame();

  UpdateWidgetsSettings();

  s_state.has_initialized = true;
  return true;
}

void FullscreenUI::ShutdownWidgets()
{
  DestroyWidgetsGPUResources();

  {
    std::unique_lock lock(s_state.shared_state_mutex);
    s_state.texture_upload_queue.clear();

    s_state.transition_state = TransitionState::Inactive;
    if (s_state.transition_start_callback) [[unlikely]]
      WARNING_LOG("Shutting down FullscreenUI while a transition callback is still set.");
    s_state.transition_start_callback = {};
    s_state.fullscreen_footer_icon_mapping = {};
    s_state.background_progress_dialogs.clear();
    s_state.fullscreen_footer_text.clear();
    s_state.last_fullscreen_footer_text.clear();
    s_state.left_fullscreen_footer_text.clear();
    s_state.last_left_fullscreen_footer_text.clear();
    s_state.fullscreen_text_change_time = 0.0f;
    s_state.input_string_dialog.ClearState();
    s_state.message_dialog.ClearState();
    s_state.choice_dialog.ClearState();
    s_state.file_selector_dialog.ClearState();
  }

  s_state.has_initialized = false;

  UIStyle.Font = nullptr;

  UpdateLoadingScreenRunIdle();
  UpdateNotificationsRunIdle();
}

void FullscreenUI::UpdateWidgetsSettings()
{
  UIStyle.Animations = Core::GetBaseBoolSettingValue("Main", "FullscreenUIAnimations", true);
  UIStyle.SmoothScrolling = Core::GetBaseBoolSettingValue("Main", "FullscreenUISmoothScrolling", true);
  UIStyle.MenuBorders = Core::GetBaseBoolSettingValue("Main", "FullscreenUIMenuBorders", false);
  s_state.sound_effects_enabled = Core::GetBaseBoolSettingValue("Main", "FullscreenUISoundEffects", true);

  const bool display_ps_icons = Core::GetBaseBoolSettingValue("Main", "FullscreenUIDisplayPSIcons", false);
  const bool swap_face_buttons = Core::GetBaseBoolSettingValue("Main", "FullscreenUISwapGamepadFaceButtons", false);

  // Don't bother setting a mapping if there's nothing to map.
  if (display_ps_icons || swap_face_buttons)
  {
    s_state.fullscreen_footer_icon_mapping =
      s_button_mapping[BoolToUInt8(display_ps_icons)][BoolToUInt8(swap_face_buttons)];
  }
  else
  {
    s_state.fullscreen_footer_icon_mapping = {};
  }

  ImGuiManager::SetGamepadFaceButtonsSwapped(swap_face_buttons);
}

bool FullscreenUI::CreateWidgetsGPUResources(Error* error)
{
  if (!(s_state.placeholder_texture = LoadTexture("images/placeholder.png")))
  {
    Error::SetStringView(error, "Failed to load placeholder.png");
    return false;
  }

  if (!CompileTransitionPipelines(error))
    return false;

  return true;
}

void FullscreenUI::DestroyWidgetsGPUResources()
{
  s_state.transition_blend_pipeline.reset();
  g_gpu_device->RecycleTexture(std::move(s_state.transition_prev_texture));
  g_gpu_device->RecycleTexture(std::move(s_state.transition_current_texture));

  s_state.placeholder_texture.reset();

  s_state.texture_cache.Clear();
}

const std::shared_ptr<GPUTexture>& FullscreenUI::GetPlaceholderTexture()
{
  return s_state.placeholder_texture;
}

std::optional<Image> FullscreenUI::LoadTextureImage(std::string_view path, u32 svg_width, u32 svg_height)
{
  std::optional<Image> image;
  Error error;

  if (StringUtil::EqualNoCase(Path::GetExtension(path), "svg"))
  {
    std::optional<DynamicHeapArray<u8>> svg_data;
    if (Path::IsAbsolute(path))
      svg_data = FileSystem::ReadBinaryFile(std::string(path).c_str(), &error);
    else
      svg_data = Host::ReadResourceFile(path, true, &error);

    if (svg_data.has_value())
    {
      image = Image();
      if (!image->RasterizeSVG(svg_data->cspan(), svg_width, svg_height, &error))
      {
        ERROR_LOG("Failed to rasterize SVG texture file '{}': {}", path, error.GetDescription());
        image.reset();
      }
    }
    else
    {
      ERROR_LOG("Failed to read SVG texture file '{}': {}", path, error.GetDescription());
    }
  }
  else if (Path::IsAbsolute(path))
  {
    std::string path_str(path);
    auto fp = FileSystem::OpenManagedCFile(path_str.c_str(), "rb", &error);
    if (fp)
    {
      image = Image();
      if (!image->LoadFromFile(path_str.c_str(), fp.get(), &error))
      {
        ERROR_LOG("Failed to read texture file '{}': {}", path, error.GetDescription());
        image.reset();
      }
    }
    else
    {
      ERROR_LOG("Failed to open texture file '{}': {}", path, error.GetDescription());
    }
  }
  else
  {
    std::optional<DynamicHeapArray<u8>> data = Host::ReadResourceFile(path, true, &error);
    if (data.has_value())
    {
      image = Image();
      if (!image->LoadFromBuffer(path, data->cspan(), &error))
      {
        ERROR_LOG("Failed to read texture resource '{}': {}", path, error.GetDescription());
        image.reset();
      }
    }
    else
    {
      ERROR_LOG("Failed to open texture resource '{}: {}'", path, error.GetDescription());
    }
  }

  return image;
}

std::shared_ptr<GPUTexture> FullscreenUI::UploadTexture(std::string_view path, const Image& image)
{
  Error error;
  std::unique_ptr<GPUTexture> texture =
    g_gpu_device->FetchAndUploadTextureImage(image, GPUTexture::Flags::None, &error);
  if (!texture)
  {
    ERROR_LOG("Failed to upload texture '{}': {}", Path::GetFileTitle(path), error.GetDescription());
    return {};
  }

  DEV_LOG("Uploaded texture resource '{}' ({}x{})", path, image.GetWidth(), image.GetHeight());
  return std::shared_ptr<GPUTexture>(texture.release(), GPUDevice::PooledTextureDeleter());
}

std::shared_ptr<GPUTexture> FullscreenUI::LoadTexture(std::string_view path, u32 width_hint, u32 height_hint)
{
  std::optional<Image> image(LoadTextureImage(path, width_hint, height_hint));
  if (image.has_value())
  {
    std::shared_ptr<GPUTexture> ret(UploadTexture(path, image.value()));
    if (ret)
      return ret;
  }

  return s_state.placeholder_texture;
}

GPUTexture* FullscreenUI::FindCachedTexture(std::string_view name)
{
  std::shared_ptr<GPUTexture>* tex_ptr = s_state.texture_cache.Lookup(name);
  return tex_ptr ? tex_ptr->get() : nullptr;
}

GPUTexture* FullscreenUI::FindCachedTexture(std::string_view name, u32 svg_width, u32 svg_height)
{
  // ignore size hints if it's not needed, don't duplicate
  if (!TextureNeedsSVGDimensions(name))
    return FindCachedTexture(name);

  svg_width = static_cast<u32>(std::ceil(LayoutScale(static_cast<float>(svg_width))));
  svg_height = static_cast<u32>(std::ceil(LayoutScale(static_cast<float>(svg_height))));

  const SmallString wh_name = SmallString::from_format("{}#{}x{}", name, svg_width, svg_height);
  std::shared_ptr<GPUTexture>* tex_ptr = s_state.texture_cache.Lookup(wh_name.view());
  return tex_ptr ? tex_ptr->get() : nullptr;
}

GPUTexture* FullscreenUI::GetCachedTexture(std::string_view name)
{
  std::shared_ptr<GPUTexture>* tex_ptr = s_state.texture_cache.Lookup(name);
  if (!tex_ptr)
  {
    std::shared_ptr<GPUTexture> tex = LoadTexture(name);
    tex_ptr = s_state.texture_cache.Insert(std::string(name), std::move(tex));
  }

  return tex_ptr->get();
}

GPUTexture* FullscreenUI::GetCachedTexture(std::string_view name, u32 svg_width, u32 svg_height)
{
  // ignore size hints if it's not needed, don't duplicate
  if (!TextureNeedsSVGDimensions(name))
    return GetCachedTexture(name);

  svg_width = static_cast<u32>(std::ceil(LayoutScale(static_cast<float>(svg_width))));
  svg_height = static_cast<u32>(std::ceil(LayoutScale(static_cast<float>(svg_height))));

  const SmallString wh_name = SmallString::from_format("{}#{}x{}", name, svg_width, svg_height);
  std::shared_ptr<GPUTexture>* tex_ptr = s_state.texture_cache.Lookup(wh_name.view());
  if (!tex_ptr)
  {
    std::shared_ptr<GPUTexture> tex = LoadTexture(name, svg_width, svg_height);
    tex_ptr = s_state.texture_cache.Insert(std::string(wh_name.view()), std::move(tex));
  }

  return tex_ptr->get();
}

GPUTexture* FullscreenUI::GetCachedTextureAsync(std::string_view name)
{
  std::shared_ptr<GPUTexture>* tex_ptr = s_state.texture_cache.Lookup(name);
  if (!tex_ptr)
  {
    // insert the placeholder
    tex_ptr = s_state.texture_cache.Insert(std::string(name), s_state.placeholder_texture);

    // queue the actual load
    Host::QueueAsyncTask([path = std::string(name)]() mutable {
      std::optional<Image> image(LoadTextureImage(path.c_str(), 0, 0));

      // don't bother queuing back if it doesn't exist
      if (!image.has_value())
        return;

      std::unique_lock lock(s_state.shared_state_mutex);
      s_state.texture_upload_queue.emplace_back(std::move(path), std::move(image.value()));
    });
  }

  return tex_ptr->get();
}

GPUTexture* FullscreenUI::GetCachedTextureAsync(std::string_view name, u32 svg_width, u32 svg_height)
{
  // ignore size hints if it's not needed, don't duplicate
  if (!TextureNeedsSVGDimensions(name))
    return GetCachedTextureAsync(name);

  svg_width = static_cast<u32>(std::ceil(LayoutScale(static_cast<float>(svg_width))));
  svg_height = static_cast<u32>(std::ceil(LayoutScale(static_cast<float>(svg_height))));

  const SmallString wh_name = SmallString::from_format("{}#{}x{}", name, svg_width, svg_height);
  std::shared_ptr<GPUTexture>* tex_ptr = s_state.texture_cache.Lookup(wh_name.view());
  if (!tex_ptr)
  {
    // insert the placeholder
    tex_ptr = s_state.texture_cache.Insert(std::string(wh_name), s_state.placeholder_texture);

    // queue the actual load
    Host::QueueAsyncTask([path = std::string(name), wh_name = std::string(wh_name), svg_width, svg_height]() mutable {
      std::optional<Image> image(LoadTextureImage(path.c_str(), svg_width, svg_height));

      // don't bother queuing back if it doesn't exist
      if (!image.has_value())
        return;

      std::unique_lock lock(s_state.shared_state_mutex);
      s_state.texture_upload_queue.emplace_back(std::move(wh_name), std::move(image.value()));
    });
  }

  return tex_ptr->get();
}

bool FullscreenUI::InvalidateCachedTexture(std::string_view path)
{
  // need to do a partial match on this because SVG
  return (s_state.texture_cache.RemoveMatchingItems([&path](const std::string& key) { return key.starts_with(path); }) >
          0);
}

bool FullscreenUI::TextureNeedsSVGDimensions(std::string_view path)
{
  return StringUtil::EndsWithNoCase(Path::GetExtension(path), "svg");
}

void FullscreenUI::UploadAsyncTextures()
{
  std::unique_lock lock(s_state.shared_state_mutex);
  while (!s_state.texture_upload_queue.empty())
  {
    std::pair<std::string, Image> it(std::move(s_state.texture_upload_queue.front()));
    s_state.texture_upload_queue.pop_front();
    lock.unlock();

    std::shared_ptr<GPUTexture> tex = UploadTexture(it.first.c_str(), it.second);
    if (tex)
      s_state.texture_cache.Insert(std::move(it.first), std::move(tex));

    lock.lock();
  }
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

bool FullscreenUI::CompileTransitionPipelines(Error* error)
{
  const RenderAPI render_api = g_gpu_device->GetRenderAPI();
  const ShaderGen shadergen(render_api, ShaderGen::GetShaderLanguageForAPI(render_api), false, false);
  GPUSwapChain* const swap_chain = g_gpu_device->GetMainSwapChain();

  std::unique_ptr<GPUShader> vs = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                             shadergen.GeneratePassthroughVertexShader(), error);
  std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                                             shadergen.GenerateFadeFragmentShader(), error);
  if (!vs || !fs)
  {
    Error::AddPrefix(error, "Failed to compile transition shaders: ");
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
  plconfig.SetTargetFormats(swap_chain ? swap_chain->GetFormat() : GPUTextureFormat::RGBA8);
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  plconfig.vertex_shader = vs.get();
  plconfig.geometry_shader = nullptr;
  plconfig.fragment_shader = fs.get();

  s_state.transition_blend_pipeline = g_gpu_device->CreatePipeline(plconfig, error);
  if (!s_state.transition_blend_pipeline)
  {
    Error::AddPrefix(error, "Failed to create transition blend pipeline: ");
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
  g_gpu_device->SetPipeline(s_state.transition_blend_pipeline.get());
  g_gpu_device->SetViewportAndScissor(0, 0, swap_chain->GetPostRotatedWidth(), swap_chain->GetPostRotatedHeight());
  g_gpu_device->SetTextureSampler(0, curr, g_gpu_device->GetNearestSampler());
  g_gpu_device->SetTextureSampler(1, s_state.transition_prev_texture.get(), g_gpu_device->GetNearestSampler());

  const GSVector2i size = swap_chain->GetSizeVec();
  const GSVector2i postrotated_size = swap_chain->GetPostRotatedSizeVec();
  const GSVector4 uv_rect = g_gpu_device->UsesLowerLeftOrigin() ? GSVector4::cxpr(0.0f, 1.0f, 1.0f, 0.0f) :
                                                                  GSVector4::cxpr(0.0f, 0.0f, 1.0f, 1.0f);
  VideoPresenter::DrawScreenQuad(GSVector4i::loadh(size), uv_rect, size, postrotated_size, DisplayRotation::Normal,
                                 swap_chain->GetPreRotation(), uniforms, sizeof(uniforms));
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
    UpdateRunIdleState();
  }
}

bool FullscreenUI::UpdateLayoutScale()
{
#ifndef __ANDROID__

  static constexpr float LAYOUT_RATIO = LAYOUT_SCREEN_WIDTH / LAYOUT_SCREEN_HEIGHT;
  const ImGuiIO& io = ImGui::GetIO();

  const float screen_width = io.DisplaySize.x;
  const float screen_height = io.DisplaySize.y;
  const float screen_ratio = screen_width / screen_height;
  const float old_scale = UIStyle.LayoutScale;

  if (screen_ratio > LAYOUT_RATIO)
  {
    // screen is wider, use height, pad width
    UIStyle.LayoutScale = std::max(screen_height / LAYOUT_SCREEN_HEIGHT, 0.1f);
    UIStyle.LayoutPaddingTop = 0.0f;
    UIStyle.LayoutPaddingLeft = (screen_width - (LAYOUT_SCREEN_WIDTH * UIStyle.LayoutScale)) / 2.0f;
  }
  else
  {
    // screen is taller, use width, pad height
    UIStyle.LayoutScale = std::max(screen_width / LAYOUT_SCREEN_WIDTH, 0.1f);
    UIStyle.LayoutPaddingTop = (screen_height - (LAYOUT_SCREEN_HEIGHT * UIStyle.LayoutScale)) / 2.0f;
    UIStyle.LayoutPaddingLeft = 0.0f;
  }

  UIStyle.RcpLayoutScale = 1.0f / UIStyle.LayoutScale;
  UIStyle.LargeFontSize = LayoutScale(LAYOUT_LARGE_FONT_SIZE);
  UIStyle.MediumFontSize = LayoutScale(LAYOUT_MEDIUM_FONT_SIZE);
  UIStyle.MediumLargeFontSize = LayoutScale(LAYOUT_MEDIUM_LARGE_FONT_SIZE);
  UIStyle.MediumSmallFontSize = LayoutScale(LAYOUT_MEDIUM_SMALL_FONT_SIZE);

  return (UIStyle.LayoutScale != old_scale);

#else

  // On Android, treat a rotated display as always being in landscape mode for FSUI scaling.
  // Makes achievement popups readable regardless of the device's orientation, and avoids layout changes.
  const ImGuiIO& io = ImGui::GetIO();
  const float old_scale = UIStyle.LayoutScale;
  UIStyle.LayoutScale = std::max(io.DisplaySize.x, io.DisplaySize.y) / LAYOUT_SCREEN_WIDTH;
  UIStyle.RcpLayoutScale = 1.0f / UIStyle.LayoutScale;
  UIStyle.LargeFontSize = LayoutScale(LAYOUT_LARGE_FONT_SIZE);
  UIStyle.MediumFontSize = LayoutScale(LAYOUT_MEDIUM_FONT_SIZE);
  UIStyle.MediumLargeFontSize = LayoutScale(LAYOUT_MEDIUM_LARGE_FONT_SIZE);
  UIStyle.MediumSmallFontSize = LayoutScale(LAYOUT_MEDIUM_SMALL_FONT_SIZE);
  return (UIStyle.LayoutScale != old_scale);

#endif
}

FullscreenUI::IconStackString::IconStackString(std::string_view icon, std::string_view str)
{
  SmallStackString::format("{} {}", icon, Host::TranslateToStringView(FSUI_TR_CONTEXT, str));
}

FullscreenUI::IconStackString::IconStackString(std::string_view icon, std::string_view str, std::string_view suffix)
{
  SmallStackString::format("{} {}##{}", icon, Host::TranslateToStringView(FSUI_TR_CONTEXT, str), suffix);
}

void FullscreenUI::FormatIconString(SmallStringBase& str, std::string_view icon, std::string_view label)
{
  str.format("{} {}", icon, Host::TranslateToStringView(FSUI_TR_CONTEXT, label));
}

ImRect FullscreenUI::CenterImage(const ImVec2& fit_size, const ImVec2& image_size)
{
  const float fit_ar = fit_size.x / fit_size.y;
  const float image_ar = image_size.x / image_size.y;

  ImRect ret;
  if (fit_ar > image_ar)
  {
    // center horizontally
    const float width = ImFloor(fit_size.y * image_ar);
    const float offset = ImFloor((fit_size.x - width) * 0.5f);
    const float height = fit_size.y;
    ret = ImRect(ImVec2(offset, 0.0f), ImVec2(offset + width, height));
  }
  else
  {
    // center vertically
    const float height = ImFloor(fit_size.x / image_ar);
    const float offset = ImFloor((fit_size.y - height) * 0.5f);
    const float width = fit_size.x;
    ret = ImRect(ImVec2(0.0f, offset), ImVec2(width, offset + height));
  }

  return ret;
}

ImRect FullscreenUI::CenterImage(const ImVec2& fit_rect, const GPUTexture* texture)
{
  const GSVector2 texture_size = GSVector2(texture->GetSizeVec());
  return CenterImage(fit_rect, ImVec2(texture_size.x, texture_size.y));
}

ImRect FullscreenUI::CenterImage(const ImRect& fit_rect, const ImVec2& image_size)
{
  ImRect ret(CenterImage(fit_rect.Max - fit_rect.Min, image_size));
  ret.Translate(fit_rect.Min);
  return ret;
}

ImRect FullscreenUI::CenterImage(const ImRect& fit_rect, const GPUTexture* texture)
{
  const GSVector2 texture_size = GSVector2(texture->GetSizeVec());
  return CenterImage(fit_rect, ImVec2(texture_size.x, texture_size.y));
}

ImRect FullscreenUI::FitImage(const ImVec2& fit_size, const ImVec2& image_size)
{
  ImRect rect;

  const float image_aspect = image_size.x / image_size.y;
  const float screen_aspect = fit_size.x / fit_size.y;

  if (screen_aspect < image_aspect)
  {
    // Screen is narrower than image - crop horizontally
    float cropAmount = 1.0f - (screen_aspect / image_aspect);
    float offset = cropAmount * 0.5f;
    rect.Min = ImVec2(offset, 0.0f);
    rect.Max = ImVec2(1.0f - offset, 1.0f);
  }
  else
  {
    // Screen is wider than image - crop vertically
    float cropAmount = 1.0f - (image_aspect / screen_aspect);
    float offset = cropAmount * 0.5f;
    rect.Min = ImVec2(0.0f, offset);
    rect.Max = ImVec2(1.0f, 1.0f - offset);
  }

  return rect;
}

void FullscreenUI::BeginLayout()
{
  // we evict from the texture cache at the start of the frame, in case we go over mid-frame,
  // we need to keep all those textures alive until the end of the frame
  s_state.texture_cache.ManualEvict();
  PushResetLayout();
}

void FullscreenUI::EndLayout()
{
  s_state.choice_dialog.Draw();
  s_state.file_selector_dialog.Draw();
  s_state.input_string_dialog.Draw();
  s_state.progress_dialog.Draw();
  s_state.message_dialog.Draw();

#if 0
  if (HasActiveWindow())
  {
    s_state.left_fullscreen_footer_text.append_format("FPS: {:.2f} ({:.3f} ms)", GImGui->IO.Framerate,
                                                      1000.0f / GImGui->IO.Framerate);
  }
#endif

  DrawFullscreenFooter();

  PopResetLayout();

  s_state.left_fullscreen_footer_text.clear();
  s_state.fullscreen_footer_text.clear();

  s_state.split_window_focus_change = SplitWindowFocusChange::None;

  s_state.rendered_menu_item_border = false;
  s_state.had_hovered_menu_item = std::exchange(s_state.has_hovered_menu_item, false);

  if (!s_state.had_sound_effect)
  {
    if (GImGui->NavActivateId != 0)
      EnqueueSoundEffect(SFX_NAV_ACTIVATE);
    else if (GImGui->NavJustMovedToId != 0)
      EnqueueSoundEffect(SFX_NAV_MOVE);
  }

  // Avoid playing the move sound on focus reset, since it'll also be an active previously.
  s_state.had_sound_effect = s_state.had_focus_reset;
  s_state.had_focus_reset = false;
}

void FullscreenUI::EnqueueSoundEffect(std::string_view sound_effect)
{
  if (s_state.had_sound_effect || !s_state.sound_effects_enabled)
    return;

  SoundEffectManager::EnqueueSoundEffect(sound_effect);
  s_state.had_sound_effect = true;
}

float FullscreenUI::GetScreenBottomMargin()
{
  return std::max(ImGuiManager::GetScreenMargin(),
                  LayoutScale(20.0f + (s_state.last_fullscreen_footer_text.empty() ? 0.0f : LAYOUT_FOOTER_HEIGHT)));
}

FullscreenUI::FixedPopupDialog::FixedPopupDialog() = default;

FullscreenUI::FixedPopupDialog::~FixedPopupDialog() = default;

void FullscreenUI::FixedPopupDialog::Open(std::string title)
{
  SetTitleAndOpen(std::move(title));
}

bool FullscreenUI::FixedPopupDialog::Begin(float scaled_window_padding /* = LayoutScale(20.0f) */,
                                           float scaled_window_rounding /* = LayoutScale(20.0f) */,
                                           const ImVec2& scaled_window_size /* = ImVec2(0.0f, 0.0f) */)
{
  return BeginRender(scaled_window_padding, scaled_window_rounding, scaled_window_size);
}

void FullscreenUI::FixedPopupDialog::End()
{
  EndRender();
}

bool FullscreenUI::IsAnyFixedPopupDialogOpen()
{
  return s_state.fixed_popup_dialog.IsOpen();
}

bool FullscreenUI::IsFixedPopupDialogOpen(std::string_view name)
{
  return (s_state.fixed_popup_dialog.GetTitle() == name);
}

void FullscreenUI::OpenFixedPopupDialog(std::string_view name)
{
  // TODO: suffix?
  s_state.fixed_popup_dialog.Open(std::string(name));
}

void FullscreenUI::CloseFixedPopupDialog()
{
  s_state.fixed_popup_dialog.StartClose();
}

void FullscreenUI::CloseFixedPopupDialogImmediately()
{
  s_state.fixed_popup_dialog.CloseImmediately();
}

bool FullscreenUI::BeginFixedPopupDialog(float scaled_window_padding /* = LayoutScale(20.0f) */,
                                         float scaled_window_rounding /* = LayoutScale(20.0f) */,
                                         const ImVec2& scaled_window_size /* = ImVec2(0.0f, 0.0f) */)
{
  if (!s_state.fixed_popup_dialog.Begin(scaled_window_padding, scaled_window_rounding, scaled_window_size))
  {
    s_state.fixed_popup_dialog.ClearState();
    return false;
  }

  return true;
}

void FullscreenUI::EndFixedPopupDialog()
{
  s_state.fixed_popup_dialog.End();
}

void FullscreenUI::RenderOverlays()
{
  std::unique_lock lock(s_state.shared_state_mutex);
  if (!AreAnyNotificationsActive())
    return;

  DrawLoadingScreen();

  float bottom_center_y = ImGui::GetIO().DisplaySize.y - GetScreenBottomMargin();
  DrawBackgroundProgressDialogs(bottom_center_y);
  DrawToast(bottom_center_y);

  // cleared?
  if (!AreAnyNotificationsActive())
    VideoThread::SetRunIdleReason(VideoThread::RunIdleReason::NotificationsActive, false);
}

void FullscreenUI::PushResetLayout()
{
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(8.0f, 8.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(4.0f, 3.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, LayoutScale(LAYOUT_ITEM_X_SPACING, LAYOUT_ITEM_Y_SPACING));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, LayoutScale(4.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, LayoutScale(4.0f, 2.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, LayoutScale(21.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, LayoutScale(14.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, LayoutScale(4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollSmooth, UIStyle.SmoothScrolling ? SMOOTH_SCROLLING_SPEED : 1.0f);
  ImGui::PushStyleVar(
    ImGuiStyleVar_ScrollStepSize,
    ImVec2(LayoutScale(LAYOUT_LARGE_FONT_SIZE), MenuButtonBounds::GetSingleLineHeight(LAYOUT_MENU_BUTTON_Y_PADDING)));
  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.SecondaryTextColor);
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, UIStyle.DisabledColor);
  ImGui::PushStyleColor(ImGuiCol_Button, UIStyle.SecondaryColor);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, DarkerColor(UIStyle.BackgroundHighlight, 1.2f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIStyle.BackgroundHighlight);
  ImGui::PushStyleColor(ImGuiCol_Border, UIStyle.BackgroundLineColor);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, UIStyle.BackgroundColor);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, UIStyle.PrimaryColor);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, UIStyle.PrimaryLightColor);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, UIStyle.PrimaryDarkColor);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, UIStyle.PopupBackgroundColor);
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));
}

void FullscreenUI::PopResetLayout()
{
  ImGui::PopStyleColor(12);
  ImGui::PopStyleVar(14);
}

void FullscreenUI::QueueResetFocus(FocusResetType type)
{
  s_state.focus_reset_queued = type;
  s_state.close_button_state =
    (s_state.close_button_state != CloseButtonState::Cancelled) ? CloseButtonState::None : CloseButtonState::Cancelled;

  GImGui->NavMoveSubmitted = false;
  GImGui->NavMoveDir = ImGuiDir_None;
  GImGui->NavMoveFlags = ImGuiNavMoveFlags_None;
  GImGui->NavMoveScrollFlags = ImGuiScrollFlags_None;
  GImGui->NavMoveClipDir = GImGui->NavMoveDir;
  GImGui->NavScoringNoClipRect = ImRect(+FLT_MAX, +FLT_MAX, -FLT_MAX, -FLT_MAX);
}

void FullscreenUI::CancelResetFocus()
{
  s_state.focus_reset_queued = FocusResetType::None;
}

bool FullscreenUI::ResetFocusHere()
{
  if (s_state.focus_reset_queued == FocusResetType::None)
    return false;

  // don't take focus from dialogs
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (ImGui::FindBlockingModal(window))
    return false;

  // Set the flag that we drew an active/hovered item active for a frame, because otherwise there's one frame where
  // there'll be no frame drawn, which will cancel the animation. Also set the appearing flag, so that the default
  // focus set does actually go through.
  if (GImGui->NavCursorVisible && GImGui->NavHighlightItemUnderNav)
  {
    window->Appearing = true;
    s_state.has_hovered_menu_item = s_state.had_hovered_menu_item;
  }

  ImGui::SetWindowFocus();

  // If this is a popup closing, we don't want to reset the current nav item, since we were presumably opened by one.
  if (s_state.focus_reset_queued != FocusResetType::PopupClosed &&
      s_state.focus_reset_queued != FocusResetType::SplitWindowChanged)
  {
    ImGui::NavInitWindow(window, true);
  }
  else
  {
    ImGui::SetNavWindow(window);
  }

  // prevent any sound from playing on the nav change
  s_state.had_focus_reset = true;

  s_state.focus_reset_queued = FocusResetType::None;
  ResetMenuButtonFrame();

  // only do the active selection magic when we're using keyboard/gamepad
  return (GImGui->NavInputSource == ImGuiInputSource_Keyboard || GImGui->NavInputSource == ImGuiInputSource_Gamepad);
}

bool FullscreenUI::IsFocusResetQueued()
{
  return (s_state.focus_reset_queued != FocusResetType::None);
}

bool FullscreenUI::IsFocusResetFromWindowChange()
{
  return (s_state.focus_reset_queued == FocusResetType::ViewChanged ||
          s_state.focus_reset_queued == FocusResetType::Other);
}

FullscreenUI::FocusResetType FullscreenUI::GetQueuedFocusResetType()
{
  return s_state.focus_reset_queued;
}

void FullscreenUI::ForceKeyNavEnabled()
{
  ImGuiContext& g = *ImGui::GetCurrentContext();
  g.ActiveIdSource = (g.ActiveIdSource == ImGuiInputSource_Mouse || g.ActiveIdSource == ImGuiInputSource_None) ?
                       ImGuiInputSource_Keyboard :
                       g.ActiveIdSource;
  g.NavInputSource = (g.NavInputSource == ImGuiInputSource_Mouse || g.NavInputSource == ImGuiInputSource_None) ?
                       ImGuiInputSource_Keyboard :
                       g.ActiveIdSource;
  g.NavCursorVisible = true;
  g.NavHighlightItemUnderNav = true;
}

bool FullscreenUI::WantsToCloseMenu()
{
  ImGuiContext& g = *GImGui;

  // Wait for the Close button to be pressed, THEN released
  if (s_state.close_button_state == CloseButtonState::None)
  {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      s_state.close_button_state = CloseButtonState::KeyboardPressed;
    else if (ImGui::IsKeyPressed(ImGuiKey_MouseRight, false))
      s_state.close_button_state = CloseButtonState::MousePressed;
    else if (ImGui::IsKeyPressed(ImGuiKey_NavGamepadCancel, false))
      s_state.close_button_state = CloseButtonState::GamepadPressed;
  }
  else if ((s_state.close_button_state == CloseButtonState::KeyboardPressed && ImGui::IsKeyReleased(ImGuiKey_Escape)) ||
           (s_state.close_button_state == CloseButtonState::GamepadPressed &&
            ImGui::IsKeyReleased(ImGuiKey_NavGamepadCancel)))
  {
    EnqueueSoundEffect(SFX_NAV_BACK);
    s_state.close_button_state = CloseButtonState::AnyReleased;
  }
  else if ((s_state.close_button_state == CloseButtonState::MousePressed && ImGui::IsKeyReleased(ImGuiKey_MouseRight)))
  {
    s_state.close_button_state = CloseButtonState::AnyReleased;
  }
  return (s_state.close_button_state == CloseButtonState::AnyReleased);
}

void FullscreenUI::ResetCloseMenuIfNeeded()
{
  // If s_close_button_state reached the "Released" state, reset it after the tick
  s_state.close_button_state =
    (s_state.close_button_state >= CloseButtonState::AnyReleased) ? CloseButtonState::None : s_state.close_button_state;
}

void FullscreenUI::CancelPendingMenuClose()
{
  s_state.close_button_state = CloseButtonState::Cancelled;
}

void FullscreenUI::PushPrimaryColor()
{
  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.PrimaryTextColor);
  ImGui::PushStyleColor(ImGuiCol_Button, UIStyle.PrimaryDarkColor);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, DarkerColor(UIStyle.PrimaryLightColor, 1.2f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIStyle.PrimaryLightColor);
  ImGui::PushStyleColor(ImGuiCol_Border, UIStyle.PrimaryLightColor);
}

void FullscreenUI::PopPrimaryColor()
{
  ImGui::PopStyleColor(5);
}

void FullscreenUI::DrawRoundedGradientRect(ImDrawList* const dl, const ImVec2& pos_min, const ImVec2& pos_max,
                                           ImU32 col_left, ImU32 col_right, float rounding)
{
  dl->AddRectFilled(pos_min, ImVec2(pos_min.x + rounding, pos_max.y), col_left, rounding, ImDrawFlags_RoundCornersLeft);
  dl->AddRectFilledMultiColor(ImVec2(pos_min.x + rounding, pos_min.y), ImVec2(pos_max.x - rounding, pos_max.y),
                              col_left, col_right, col_right, col_left);
  dl->AddRectFilled(ImVec2(pos_max.x - rounding, pos_min.y), pos_max, col_right, rounding,
                    ImDrawFlags_RoundCornersRight);
}

void FullscreenUI::DrawSpinner(ImDrawList* const dl, const ImVec2& pos, ImU32 color, float size, float thickness)
{
  // based off https://github.com/ocornut/imgui/issues/1901
  static constexpr u32 num_segments = 30;
  const float radius = ImFloor(size * 0.5f);
  const ImVec2 center = ImVec2(pos.x + radius, pos.y + radius);
  const float start = std::abs(ImSin(static_cast<float>(GImGui->Time * 1.8f)) * static_cast<float>(num_segments - 5));
  const float a_min = IM_PI * 2.0f * start / static_cast<float>(num_segments);
  const float a_max = IM_PI * 2.0f * (static_cast<float>(num_segments - 3) / static_cast<float>(num_segments));
  for (u32 i = 0; i < num_segments; i++)
  {
    const float a = a_min + ((float)i / (float)num_segments) * (a_max - a_min);
    dl->PathLineTo(ImVec2(center.x + ImCos(static_cast<float>(a + GImGui->Time * 8.0f)) * radius,
                          center.y + ImSin(static_cast<float>(a + GImGui->Time * 8.0f)) * radius));
  }
  dl->PathStroke(color, false, thickness);
}

bool FullscreenUI::BeginFullscreenColumns(const char* title, float pos_y, bool expand_to_screen_width, bool footer)
{
  ImGui::SetNextWindowPos(ImVec2(expand_to_screen_width ? 0.0f : UIStyle.LayoutPaddingLeft, pos_y));
  ImGui::SetNextWindowSize(
    ImVec2(expand_to_screen_width ? ImGui::GetIO().DisplaySize.x : LayoutScale(LAYOUT_SCREEN_WIDTH),
           ImGui::GetIO().DisplaySize.y - pos_y - (footer ? LayoutScale(LAYOUT_FOOTER_HEIGHT) : 0.0f)));

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

  bool clipped;
  if (title)
  {
    ImGui::PushFont(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight);
    clipped = ImGui::Begin(title, nullptr,
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground);
    ImGui::PopFont();
  }
  else
  {
    clipped = ImGui::Begin("fullscreen_ui_columns_parent", nullptr,
                           ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoBackground);
  }

  return clipped;
}

void FullscreenUI::EndFullscreenColumns()
{
  ImGui::End();
  ImGui::PopStyleVar(3);
}

bool FullscreenUI::BeginFullscreenColumnWindow(float start, float end, const char* name, const ImVec4& background,
                                               const ImVec2& padding)
{
  start = LayoutScale(start);
  end = LayoutScale(end);

  if (start < 0.0f)
    start = ImGui::GetIO().DisplaySize.x + start;
  if (end <= 0.0f)
    end = ImGui::GetIO().DisplaySize.x + end;

  const ImVec2 pos(start, 0.0f);
  const ImVec2 size(end - start, ImGui::GetCurrentWindow()->Size.y);

  ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(padding));

  ImGui::SetCursorPos(pos);

  return ImGui::BeginChild(name, size,
                           ((padding.x != 0.0f || padding.y != 0.0f) ? ImGuiChildFlags_AlwaysUseWindowPadding : 0) |
                             ImGuiChildFlags_NavFlattened | ImGuiChildFlags_NoNavCancel);
}

void FullscreenUI::EndFullscreenColumnWindow()
{
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

bool FullscreenUI::BeginFullscreenWindow(float left, float top, float width, float height, const char* name,
                                         const ImVec4& background /* = HEX_TO_IMVEC4(0x212121, 0xFF) */,
                                         float rounding /*= 0.0f*/, const ImVec2& padding /*= 0.0f*/,
                                         ImGuiWindowFlags flags /*= 0*/)
{
  if (left < 0.0f)
    left = (LAYOUT_SCREEN_WIDTH - width) * -left;
  if (top < 0.0f)
    top = (LAYOUT_SCREEN_HEIGHT - height) * -top;

  const ImVec2 pos(ImVec2(LayoutScale(left) + UIStyle.LayoutPaddingLeft, LayoutScale(top) + UIStyle.LayoutPaddingTop));
  const ImVec2 size(LayoutScale(ImVec2(width, height)));
  return BeginFullscreenWindow(pos, size, name, background, rounding, padding, flags);
}

bool FullscreenUI::BeginFullscreenWindow(const ImVec2& position, const ImVec2& size, const char* name,
                                         const ImVec4& background /* = HEX_TO_IMVEC4(0x212121, 0xFF) */,
                                         float rounding /*= 0.0f*/, const ImVec2& padding /*= 0.0f*/,
                                         ImGuiWindowFlags flags /*= 0*/)
{
  ImGui::SetNextWindowPos(position);
  ImGui::SetNextWindowSize(size);

  ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(padding));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

  return ImGui::Begin(name, nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoBringToFrontOnFocus |
                        ((background.w == 0.0f) ? ImGuiWindowFlags_NoBackground : 0) | flags);
}

void FullscreenUI::EndFullscreenWindow(bool allow_wrap_x, bool allow_wrap_y)
{
  if (allow_wrap_x || allow_wrap_y)
    SetWindowNavWrapping(allow_wrap_x, allow_wrap_y);

  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();
}

void FullscreenUI::SetWindowNavWrapping(bool allow_wrap_x /*= false*/, bool allow_wrap_y /*= true*/)
{
  DebugAssert(allow_wrap_x || allow_wrap_y);
  if (ImGuiWindow* const win = ImGui::GetCurrentWindowRead(); GImGui->NavWindow == win)
  {
    ImGui::NavMoveRequestTryWrapping(win, (allow_wrap_x ? ImGuiNavMoveFlags_LoopX : 0) |
                                            (allow_wrap_y ? ImGuiNavMoveFlags_LoopY : 0));
  }
}

bool FullscreenUI::IsGamepadInputSource()
{
  return (ImGui::GetCurrentContext()->NavInputSource == ImGuiInputSource_Gamepad);
}

std::string_view FullscreenUI::GetControllerIconMapping(std::string_view icon)
{
  const auto iter =
    std::lower_bound(s_state.fullscreen_footer_icon_mapping.begin(), s_state.fullscreen_footer_icon_mapping.end(), icon,
                     [](const auto& it, const auto& value) { return (it.first < value); });
  if (iter != s_state.fullscreen_footer_icon_mapping.end() && iter->first == icon)
    icon = iter->second;
  return icon;
}

void FullscreenUI::CreateFooterTextString(SmallStringBase& dest,
                                          std::span<const std::pair<const char*, std::string_view>> items)
{
  dest.clear();
  for (const auto& [icon, text] : items)
  {
    if (!dest.empty())
      dest.append("     ");

    dest.append(GetControllerIconMapping(icon));
    dest.append("  ");
    dest.append(text);
  }
}

void FullscreenUI::SetFullscreenFooterText(std::string_view text)
{
  s_state.fullscreen_footer_text.assign(text);
}

void FullscreenUI::SetFullscreenFooterText(std::span<const std::pair<const char*, std::string_view>> items)
{
  CreateFooterTextString(s_state.fullscreen_footer_text, items);
}

void FullscreenUI::SetFullscreenStatusText(std::string_view text)
{
  s_state.left_fullscreen_footer_text = text;
}

void FullscreenUI::SetFullscreenStatusText(std::span<const std::pair<const char*, std::string_view>> items)
{
  CreateFooterTextString(s_state.left_fullscreen_footer_text, items);
}

void FullscreenUI::SetStandardSelectionFooterText(bool back_instead_of_cancel)
{
  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, FSUI_VSTR("Change Selection")),
                 std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Select")),
                 std::make_pair(ICON_PF_BUTTON_B, back_instead_of_cancel ? FSUI_VSTR("Back") : FSUI_VSTR("Cancel"))});
  }
  else
  {
    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, FSUI_VSTR("Change Selection")),
                 std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")),
                 std::make_pair(ICON_PF_ESC, back_instead_of_cancel ? FSUI_VSTR("Back") : FSUI_VSTR("Cancel"))});
  }
}

void FullscreenUI::DrawFullscreenFooter()
{
  const ImGuiIO& io = ImGui::GetIO();
  if (s_state.fullscreen_footer_text.empty() && s_state.left_fullscreen_footer_text.empty())
  {
    s_state.last_fullscreen_footer_text.clear();
    s_state.last_left_fullscreen_footer_text.clear();
    return;
  }

  static constexpr float TRANSITION_TIME = 0.15f;

  const ImVec2 padding = LayoutScale(LAYOUT_FOOTER_X_PADDING, LAYOUT_FOOTER_Y_PADDING);
  const float height = LayoutScale(LAYOUT_FOOTER_HEIGHT);
  const ImVec2 shadow_offset = LayoutScale(LAYOUT_SHADOW_OFFSET, LAYOUT_SHADOW_OFFSET);
  const u32 text_color = ImGui::GetColorU32(UIStyle.PrimaryTextColor);
  const float bg_alpha = GetBackgroundAlpha();

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0.0f, io.DisplaySize.y - height), io.DisplaySize,
                    ImGui::GetColorU32(ModAlpha(UIStyle.PrimaryColor, bg_alpha)), 0.0f);

  ImFont* const font = UIStyle.Font;
  const float font_size = UIStyle.MediumFontSize;
  const float font_weight = UIStyle.BoldFontWeight;
  const float max_width = io.DisplaySize.x - padding.x * 2.0f;

  float prev_opacity = 0.0f;
  if (!s_state.last_fullscreen_footer_text.empty() &&
      s_state.fullscreen_footer_text != s_state.last_fullscreen_footer_text)
  {
    if (s_state.fullscreen_text_change_time == 0.0f)
      s_state.fullscreen_text_change_time = TRANSITION_TIME;
    else
      s_state.fullscreen_text_change_time = std::max(s_state.fullscreen_text_change_time - io.DeltaTime, 0.0f);

    if (s_state.fullscreen_text_change_time == 0.0f)
      s_state.last_fullscreen_footer_text = s_state.fullscreen_footer_text;

    prev_opacity = s_state.fullscreen_text_change_time * (1.0f / TRANSITION_TIME);
    if (prev_opacity > 0.0f)
    {
      if (!s_state.last_fullscreen_footer_text.empty())
      {
        const ImVec2 text_size = font->CalcTextSizeA(font_size, font_weight, max_width, 0.0f,
                                                     IMSTR_START_END(s_state.last_fullscreen_footer_text));
        const ImVec2 text_pos =
          ImVec2(io.DisplaySize.x - padding.x - text_size.x, io.DisplaySize.y - font_size - padding.y);
        dl->AddText(font, font_size, font_weight, text_pos + shadow_offset, MulAlpha(UIStyle.ShadowColor, prev_opacity),
                    IMSTR_START_END(s_state.last_fullscreen_footer_text));
        dl->AddText(font, font_size, font_weight, text_pos, ModAlpha(text_color, prev_opacity),
                    IMSTR_START_END(s_state.last_fullscreen_footer_text));
      }

      if (!s_state.last_left_fullscreen_footer_text.empty())
      {
        const ImVec2 text_pos = ImVec2(padding.x, io.DisplaySize.y - font_size - padding.y);
        dl->AddText(font, font_size, font_weight, text_pos + shadow_offset, MulAlpha(UIStyle.ShadowColor, prev_opacity),
                    IMSTR_START_END(s_state.last_left_fullscreen_footer_text));
        dl->AddText(font, font_size, font_weight, text_pos, ModAlpha(text_color, prev_opacity),
                    IMSTR_START_END(s_state.last_left_fullscreen_footer_text));
      }
    }
  }
  else if (s_state.last_fullscreen_footer_text.empty())
  {
    s_state.last_fullscreen_footer_text = s_state.fullscreen_footer_text;
  }

  if (prev_opacity < 1.0f)
  {
    const float opacity = 1.0f - prev_opacity;
    if (!s_state.fullscreen_footer_text.empty())
    {
      const ImVec2 text_size =
        font->CalcTextSizeA(font_size, font_weight, max_width, 0.0f, IMSTR_START_END(s_state.fullscreen_footer_text));
      const ImVec2 text_pos =
        ImVec2(io.DisplaySize.x - padding.x - text_size.x, io.DisplaySize.y - font_size - padding.y);
      dl->AddText(font, font_size, font_weight, text_pos + shadow_offset, MulAlpha(UIStyle.ShadowColor, opacity),
                  IMSTR_START_END(s_state.fullscreen_footer_text));
      dl->AddText(font, font_size, font_weight, text_pos, ModAlpha(text_color, opacity),
                  IMSTR_START_END(s_state.fullscreen_footer_text));
    }

    if (!s_state.left_fullscreen_footer_text.empty())
    {
      const ImVec2 text_pos = ImVec2(padding.x, io.DisplaySize.y - font_size - padding.y);
      dl->AddText(font, font_size, font_weight, text_pos + shadow_offset, MulAlpha(UIStyle.ShadowColor, opacity),
                  IMSTR_START_END(s_state.left_fullscreen_footer_text));
      dl->AddText(font, font_size, font_weight, text_pos, ModAlpha(text_color, opacity),
                  IMSTR_START_END(s_state.left_fullscreen_footer_text));
    }
  }
}

void FullscreenUI::BeginMenuButtons(u32 num_items /* = 0 */, float y_align /* = 0.0f */,
                                    float x_padding /* = LAYOUT_MENU_BUTTON_X_PADDING */,
                                    float y_padding /* = LAYOUT_MENU_BUTTON_Y_PADDING */, float x_spacing /* = 0.0f */,
                                    float y_spacing /* = LAYOUT_MENU_BUTTON_SPACING */)
{
  // If we're scrolling up and down, it's possible that the first menu item won't be enabled.
  // If so, track when the scroll happens, and if we moved to a new ID. If not, scroll the parent window.
  if (GImGui->NavMoveDir != ImGuiDir_None)
  {
    s_state.has_pending_nav_move = GImGui->NavMoveDir;
  }
  else if (s_state.has_pending_nav_move != ImGuiDir_None)
  {
    if (GImGui->NavJustMovedToId == 0)
    {
      switch (s_state.has_pending_nav_move)
      {
        case ImGuiDir_Up:
          ImGui::SetScrollY(std::max(ImGui::GetScrollY() - MenuButtonBounds::GetSingleLineHeight(y_padding), 0.0f));
          break;
        case ImGuiDir_Down:
          ImGui::SetScrollY(
            std::min(ImGui::GetScrollY() + MenuButtonBounds::GetSingleLineHeight(y_padding), ImGui::GetScrollMaxY()));
          break;
        default:
          break;
      }
    }

    s_state.has_pending_nav_move = ImGuiDir_None;
  }

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(x_padding, y_padding));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, LayoutScale(UIStyle.MenuBorders ? 1.0f : 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, LayoutScale(x_spacing, y_spacing));

  if (y_align != 0.0f)
  {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float real_item_height = MenuButtonBounds::GetSingleLineHeight(y_padding) + style.ItemSpacing.y;
    const float total_size = (static_cast<float>(num_items) * real_item_height - style.ItemSpacing.y);
    const float window_height = ImGui::GetWindowHeight() - (style.WindowPadding.y * 2.0f);
    if (window_height > total_size)
      ImGui::SetCursorPosY((window_height - total_size) * y_align);
  }

  BeginMenuButtonDrawSplit();
}

void FullscreenUI::EndMenuButtons()
{
  PostDrawMenuButtonFrame();
  EndMenuButtonDrawSplit();

  ImGui::PopStyleVar(4);
}

float FullscreenUI::GetMenuButtonAvailableWidth()
{
  return MenuButtonBounds::CalcAvailWidth();
}

bool FullscreenUI::MenuButtonFrame(std::string_view str_id, float height, bool enabled, ImRect* item_bb, bool* visible,
                                   bool* hovered, ImGuiButtonFlags flags /*= 0*/)
{
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float avail_width = MenuButtonBounds::CalcAvailWidth();
  const ImGuiStyle& style = ImGui::GetStyle();

  *item_bb = ImRect(pos + style.FramePadding, pos + style.FramePadding + ImVec2(avail_width, height));
  const ImRect frame_bb = ImRect(pos, pos + style.FramePadding * 2.0f + ImVec2(avail_width, height));
  return MenuButtonFrame(str_id, enabled, frame_bb, visible, hovered, 0);
}

void FullscreenUI::DrawWindowTitle(std::string_view title)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  const ImVec2 pos(window->DC.CursorPos + LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
  const ImVec2 size(window->WorkRect.GetWidth() - (LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING) * 2.0f),
                    UIStyle.LargeFontSize + LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f);
  const ImRect rect(pos, pos + size);

  ImGui::ItemSize(size);
  if (!ImGui::ItemAdd(rect, window->GetID("window_title")))
    return;

  ImGui::PushFont(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight);
  ImGui::RenderTextClipped(rect.Min, rect.Max, IMSTR_START_END(title), nullptr, ImVec2(0.0f, 0.0f), &rect);
  ImGui::PopFont();

  const ImVec2 line_start(pos.x, pos.y + UIStyle.LargeFontSize + LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING));
  const ImVec2 line_end(pos.x + size.x, line_start.y);
  const float line_thickness = LayoutScale(1.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddLine(line_start, line_end, IM_COL32(255, 255, 255, 255), line_thickness);
}

bool FullscreenUI::MenuButtonFrame(std::string_view str_id, bool enabled, const ImRect& bb, bool* visible,
                                   bool* hovered, ImGuiButtonFlags flags)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (ImGui::GetCurrentWindowRead()->SkipItems)
  {
    *visible = false;
    *hovered = false;
    return false;
  }

  const ImGuiID id = window->GetID(IMSTR_START_END(str_id));
  ImGui::ItemSize(bb.GetSize());
  if (enabled)
  {
    if (!ImGui::ItemAdd(bb, id))
    {
      *visible = false;
      *hovered = false;
      return false;
    }
  }
  else
  {
    if (ImGui::IsClippedEx(bb, id))
    {
      *visible = false;
      *hovered = false;
      return false;
    }
  }

  *visible = true;

  bool held;
  bool pressed;
  if (enabled)
  {
    pressed = ImGui::ButtonBehavior(bb, id, hovered, &held, flags);
    if (*hovered)
      DrawMenuButtonFrame(bb.Min, bb.Max, held);
  }
  else
  {
    pressed = false;
    held = false;
  }

  return pressed;
}

void FullscreenUI::DrawMenuButtonFrame(const ImVec2& p_min, const ImVec2& p_max, bool held)
{
  ImVec2 frame_min = p_min;
  ImVec2 frame_max = p_max;

  const ImGuiIO& io = ImGui::GetIO();
  if (UIStyle.Animations && io.NavVisible && GImGui->CurrentWindow == GImGui->NavWindow)
  {
    if (!s_state.had_hovered_menu_item || io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
    {
      s_state.menu_button_frame_min_animated.Reset(frame_min);
      s_state.menu_button_frame_max_animated.Reset(frame_max);
      s_state.has_hovered_menu_item = true;
    }
    else
    {
      if (frame_min.x != s_state.menu_button_frame_min_animated.GetEndValue().x ||
          frame_min.y != s_state.menu_button_frame_min_animated.GetEndValue().y)
      {
        s_state.menu_button_frame_min_animated.Start(s_state.menu_button_frame_min_animated.GetCurrentValue(),
                                                     frame_min, MENU_BACKGROUND_ANIMATION_TIME);
      }
      if (frame_max.x != s_state.menu_button_frame_max_animated.GetEndValue().x ||
          frame_max.y != s_state.menu_button_frame_max_animated.GetEndValue().y)
      {
        s_state.menu_button_frame_max_animated.Start(s_state.menu_button_frame_max_animated.GetCurrentValue(),
                                                     frame_max, MENU_BACKGROUND_ANIMATION_TIME);
      }
      frame_min = s_state.menu_button_frame_min_animated.UpdateAndGetValue();
      frame_max = s_state.menu_button_frame_max_animated.UpdateAndGetValue();
      s_state.has_hovered_menu_item = true;
    }
  }

  if (!s_state.rendered_menu_item_border)
  {
    s_state.rendered_menu_item_border = true;

    const ImU32 col = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
    DrawMenuButtonFrameAt(frame_min, frame_max, col, true);
  }
}

void FullscreenUI::DrawMenuButtonFrameAt(const ImVec2& frame_min, const ImVec2& frame_max, u32 col, bool border)
{
  SetMenuButtonSplitLayer(MENU_BUTTON_SPLIT_LAYER_HIGHLIGHT);

  DrawMenuButtonFrameAtOnCurrentLayer(frame_min, frame_max, col, border);

  SetMenuButtonSplitLayer(MENU_BUTTON_SPLIT_LAYER_FOREGROUND);
}

void FullscreenUI::DrawMenuButtonFrameAtOnCurrentLayer(const ImVec2& frame_min, const ImVec2& frame_max, u32 col,
                                                       bool border)
{
  const float rounding = LayoutScale(LAYOUT_MENU_ITEM_BORDER_ROUNDING);
  if (border && UIStyle.MenuBorders)
  {
    const float t = static_cast<float>(std::min(std::abs(std::sin(ImGui::GetTime() * 0.75) * 1.1), 1.0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Border, t));
    ImGui::RenderFrame(frame_min, frame_max, col, true, rounding);
    ImGui::PopStyleColor();
  }
  else
  {
    ImGui::RenderFrame(frame_min, frame_max, col, false, rounding);
  }
}

void FullscreenUI::PostDrawMenuButtonFrame()
{
  if (s_state.rendered_menu_item_border || !s_state.had_hovered_menu_item || GImGui->CurrentWindow != GImGui->NavWindow)
    return;

  // updating might finish the animation
  const ImVec2& frame_min = s_state.menu_button_frame_min_animated.UpdateAndGetValue();
  const ImVec2& frame_max = s_state.menu_button_frame_max_animated.UpdateAndGetValue();
  const ImU32 col = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
  DrawMenuButtonFrameAt(frame_min, frame_max, col, true);
}

void FullscreenUI::BeginMenuButtonDrawSplit()
{
  // NOTE: I would use my own splitter instance here, but we're currently nesting these calls
  // due to the setting popups that get created inline...
  ImDrawList* const dl = ImGui::GetWindowDrawList();
  dl->_Splitter.Split(dl, NUM_MENU_BUTTON_SPLIT_LAYERS);
  dl->_Splitter.SetCurrentChannel(dl, MENU_BUTTON_SPLIT_LAYER_FOREGROUND);
}

void FullscreenUI::EndMenuButtonDrawSplit()
{
  ImDrawList* const dl = ImGui::GetWindowDrawList();
  DebugAssert(dl->_Splitter._Count == NUM_MENU_BUTTON_SPLIT_LAYERS);
  dl->_Splitter.Merge(ImGui::GetWindowDrawList());
}

void FullscreenUI::SetMenuButtonSplitLayer(int layer)
{
  ImDrawList* const dl = ImGui::GetWindowDrawList();
  DebugAssert(dl->_Splitter._Count == NUM_MENU_BUTTON_SPLIT_LAYERS);
  dl->_Splitter.SetCurrentChannel(dl, layer);
}

float FullscreenUI::MenuButtonBounds::CalcAvailWidth()
{
  return ImGui::GetCurrentWindowRead()->WorkRect.GetWidth() - ImGui::GetStyle().FramePadding.x * 2.0f;
}

void FullscreenUI::MenuButtonBounds::CalcValueSize(const std::string_view& value, float font_size)
{
  SetValueSize(value.empty() ? ImVec2() :
                               UIStyle.Font->CalcTextSizeA(font_size, UIStyle.BoldFontWeight, FLT_MAX,
                                                           available_width * 0.5f, IMSTR_START_END(value)));
}

void FullscreenUI::MenuButtonBounds::SetValueSize(const ImVec2& size)
{
  value_size = size;
  available_non_value_width = available_width - ((size.x > 0.0f) ? (size.x + LayoutScale(16.0f)) : 0.0f);
}

void FullscreenUI::MenuButtonBounds::CalcTitleSize(const std::string_view& title, float font_size)
{
  const std::string_view real_title = FullscreenUI::RemoveHash(title);
  title_size = real_title.empty() ? ImVec2() :
                                    UIStyle.Font->CalcTextSizeA(font_size, UIStyle.BoldFontWeight, FLT_MAX,
                                                                available_non_value_width, IMSTR_START_END(real_title));
}

void FullscreenUI::MenuButtonBounds::CalcSummarySize(const std::string_view& summary, float font_size)
{
  summary_size = summary.empty() ? ImVec2() :
                                   UIStyle.Font->CalcTextSizeA(font_size, UIStyle.NormalFontWeight, FLT_MAX,
                                                               available_non_value_width, IMSTR_START_END(summary));
}

FullscreenUI::MenuButtonBounds::MenuButtonBounds(const std::string_view& title, const std::string_view& value,
                                                 const std::string_view& summary)
{
  CalcValueSize(value, UIStyle.LargeFontSize);
  CalcTitleSize(title, UIStyle.LargeFontSize);
  CalcSummarySize(summary, UIStyle.MediumFontSize);
  CalcBB();
}

FullscreenUI::MenuButtonBounds::MenuButtonBounds(const std::string_view& title, const std::string_view& value,
                                                 const std::string_view& summary, float left_margin,
                                                 float title_value_size, float summary_size)
{
  // ugly, but only used for compact game list, whatever
  const float orig_width = available_width;
  available_width -= left_margin;

  CalcValueSize(value, title_value_size);
  CalcTitleSize(title, title_value_size);
  CalcSummarySize(summary, summary_size);

  available_width = orig_width;

  CalcBB();

  title_bb.Min.x += left_margin;
  title_bb.Max.x += left_margin;
  summary_bb.Min.x += left_margin;
  summary_bb.Max.x += left_margin;
}

FullscreenUI::MenuButtonBounds::MenuButtonBounds(const std::string_view& title, const ImVec2& value_size,
                                                 const std::string_view& summary)
{
  SetValueSize(value_size);
  CalcTitleSize(title, UIStyle.LargeFontSize);
  CalcSummarySize(summary, UIStyle.MediumFontSize);
  CalcBB();
}

FullscreenUI::MenuButtonBounds::MenuButtonBounds(const ImVec2& title_size, const ImVec2& value_size,
                                                 const ImVec2& summary_size)
  : title_size(title_size), value_size(value_size), summary_size(summary_size), available_width(CalcAvailWidth())
{
  CalcBB();
}

void FullscreenUI::MenuButtonBounds::CalcBB()
{
  // give the frame a bit of a chin, because otherwise it's too cramped
  const ImVec2& padding = ImGui::GetStyle().FramePadding;
  const ImVec2 pos = ImGui::GetCurrentWindowRead()->DC.CursorPos + padding;
  const float summary_spacing = LayoutScale(LAYOUT_MENU_ITEM_TITLE_SUMMARY_SPACING);
  const float content_height =
    std::max(title_size.y, value_size.y) +
    ((summary_size.x > 0.0f) ? (summary_size.y + LayoutScale(LAYOUT_MENU_ITEM_EXTRA_HEIGHT) + summary_spacing) : 0.0f);
  const ImVec2 br_pos = pos + ImVec2(available_width, content_height);

  frame_bb = ImRect(pos - padding, br_pos + padding);
  title_bb = ImRect(pos, ImVec2(pos.x + title_size.x, pos.y + title_size.y));

  // give the title the full bounding box if there's no value
  if (value_size.x > 0.0f)
    value_bb = ImRect(ImVec2(br_pos.x - value_size.x, pos.y), br_pos);
  else
    title_bb.Max.x = br_pos.x;

  if (summary_size.x > 0.0f)
  {
    const float summary_start_y = pos.y + std::max(title_size.y, value_size.y) + summary_spacing;
    summary_bb =
      ImRect(ImVec2(pos.x, summary_start_y), ImVec2(pos.x + summary_size.x, summary_start_y + summary_size.y));
  }
}

float FullscreenUI::MenuButtonBounds::GetSingleLineHeight(float padding)
{
  return (LayoutScale(padding) * 2.0f) + UIStyle.LargeFontSize;
}

float FullscreenUI::MenuButtonBounds::GetSummaryLineHeight(float y_padding)
{
  return GetSingleLineHeight(y_padding) + LayoutScale(LAYOUT_MENU_ITEM_TITLE_SUMMARY_SPACING) + UIStyle.MediumFontSize +
         LayoutScale(LAYOUT_MENU_ITEM_EXTRA_HEIGHT);
}

void FullscreenUI::ResetMenuButtonFrame()
{
  s_state.had_hovered_menu_item = false;
  s_state.has_hovered_menu_item = false;
}

void FullscreenUI::RenderShadowedTextClipped(ImDrawList* draw_list, ImFont* font, float font_size, float font_weight,
                                             const ImVec2& pos_min, const ImVec2& pos_max, u32 color,
                                             std::string_view text, const ImVec2* text_size_if_known,
                                             const ImVec2& align, float wrap_width, const ImRect* clip_rect,
                                             float shadow_offset)
{
  if (text.empty())
    return;

  const char* text_display_end = ImGui::FindRenderedTextEnd(IMSTR_START_END(text));
  const size_t text_len = (text_display_end - text.data());
  if (text_len == 0)
    return;

  text = text.substr(0, text_len);

  // Perform CPU side clipping for single clipped element to avoid using scissor state
  ImVec2 pos = pos_min;
  const ImVec2 text_size = text_size_if_known ?
                             *text_size_if_known :
                             font->CalcTextSizeA(font_size, font_weight, FLT_MAX, 0.0f, IMSTR_START_END(text), nullptr);

  const ImVec2* clip_min = clip_rect ? &clip_rect->Min : &pos_min;
  const ImVec2* clip_max = clip_rect ? &clip_rect->Max : &pos_max;
  bool need_clipping = (pos.x + text_size.x > clip_max->x) || (pos.y + text_size.y > clip_max->y);
  if (clip_rect) // If we had no explicit clipping rectangle then pos==clip_min
    need_clipping |= (pos.x < clip_min->x) || (pos.y < clip_min->y);

  // Align whole block. We should defer that to the better rendering function when we'll have support for individual
  // line alignment.
  if (align.x > 0.0f)
    pos.x = ImMax(pos.x, pos.x + (pos_max.x - pos.x - text_size.x) * align.x);
  if (align.y > 0.0f)
    pos.y = ImMax(pos.y, pos.y + (pos_max.y - pos.y - text_size.y) * align.y);

  // Render
  const u32 alpha = (color /*& IM_COL32_A_MASK*/) >> IM_COL32_A_SHIFT;
  if (alpha == 0)
    return;

  const u32 shadow_color = MulAlpha(UIStyle.ShadowColor, alpha);
  if (need_clipping)
  {
    ImVec4 fine_clip_rect(clip_min->x, clip_min->y, clip_max->x, clip_max->y);
    draw_list->AddText(font, font_size, font_weight, ImVec2(pos.x + shadow_offset, pos.y + shadow_offset), shadow_color,
                       IMSTR_START_END(text), wrap_width, &fine_clip_rect);
    draw_list->AddText(font, font_size, font_weight, pos, color, IMSTR_START_END(text), wrap_width, &fine_clip_rect);
  }
  else
  {
    draw_list->AddText(font, font_size, font_weight, ImVec2(pos.x + shadow_offset, pos.y + shadow_offset), shadow_color,
                       IMSTR_START_END(text), wrap_width, nullptr);
    draw_list->AddText(font, font_size, font_weight, pos, color, IMSTR_START_END(text), wrap_width, nullptr);
  }
}

void FullscreenUI::RenderShadowedTextClipped(ImDrawList* draw_list, ImFont* font, float font_size, float font_weight,
                                             const ImVec2& pos_min, const ImVec2& pos_max, u32 color,
                                             std::string_view text, const ImVec2* text_size_if_known /* = nullptr */,
                                             const ImVec2& align /* = ImVec2(0, 0)*/, float wrap_width /* = 0.0f*/,
                                             const ImRect* clip_rect /* = nullptr */)
{
  RenderShadowedTextClipped(draw_list, font, font_size, font_weight, pos_min, pos_max, color, text, text_size_if_known,
                            align, wrap_width, clip_rect, LayoutScale(LAYOUT_SHADOW_OFFSET));
}

void FullscreenUI::RenderShadowedTextClipped(ImFont* font, float font_size, float font_weight, const ImVec2& pos_min,
                                             const ImVec2& pos_max, u32 color, std::string_view text,
                                             const ImVec2* text_size_if_known /* = nullptr */,
                                             const ImVec2& align /* = ImVec2(0, 0)*/, float wrap_width /* = 0.0f*/,
                                             const ImRect* clip_rect /* = nullptr */)
{
  RenderShadowedTextClipped(ImGui::GetWindowDrawList(), font, font_size, font_weight, pos_min, pos_max, color, text,
                            text_size_if_known, align, wrap_width, clip_rect);
}

void FullscreenUI::RenderMultiLineShadowedTextClipped(ImDrawList* draw_list, ImFont* font, float font_size,
                                                      float font_weight, const ImVec2& pos_min, const ImVec2& pos_max,
                                                      u32 color, std::string_view text, const ImVec2& align,
                                                      float wrap_width, const ImRect* clip_rect /* = nullptr */,
                                                      float shadow_offset /* = LayoutScale(LAYOUT_SHADOW_OFFSET) */)
{
  if (text.empty())
    return;

  const char* text_display_end = ImGui::FindRenderedTextEnd(IMSTR_START_END(text));
  const size_t text_len = (text_display_end - text.data());
  if (text_len == 0)
    return;

  text = text.substr(0, text_len);

  const char* text_ptr = text.data();
  const char* text_end = text.data() + text_len;
  ImVec2 current_pos = pos_min;
  while (text_ptr < text_end)
  {
    const char* line_end = font->CalcWordWrapPosition(font_size, font_weight, text_ptr, text_end, wrap_width);
    const ImVec2 line_size = font->CalcTextSizeA(font_size, font_weight, FLT_MAX, 0.0f, text_ptr, line_end);
    RenderShadowedTextClipped(draw_list, font, font_size, font_weight, current_pos, pos_max, color,
                              std::string_view(text_ptr, line_end), &line_size, align, 0.0f, clip_rect, shadow_offset);

    current_pos.y += line_size.y;
    text_ptr = line_end;
  }
}

void FullscreenUI::RenderAutoLabelText(ImDrawList* draw_list, ImFont* font, float font_size, float font_weight,
                                       float label_weight, const ImVec2& pos_min, const ImVec2& pos_max, u32 color,
                                       std::string_view text, char separator, float shadow_offset)
{
  const std::string_view::size_type label_end = text.find(separator);

  ImVec2 text_pos = pos_min;

  std::string_view remaining;
  if (label_end != std::string_view::npos)
  {
    // include label in bold part
    const std::string_view label = text.substr(0, label_end + 1);
    const ImVec2 size = font->CalcTextSizeA(font_size, font_weight, FLT_MAX, 0.0f, IMSTR_START_END(label));
    RenderShadowedTextClipped(draw_list, font, font_size, label_weight, text_pos, pos_max, color, label, &size,
                              ImVec2(0.0f, 0.0f), 0.0f, nullptr, shadow_offset);

    text_pos.x += size.x;
    remaining = text.substr(label_end + 1);
  }
  else
  {
    remaining = text;
  }

  const ImVec2 size = font->CalcTextSizeA(font_size, font_weight, FLT_MAX, 0.0f, IMSTR_START_END(remaining));
  RenderShadowedTextClipped(draw_list, font, font_size, font_weight, text_pos, pos_max, color, remaining, &size,
                            ImVec2(0.0f, 0.0f), 0.0f, nullptr, shadow_offset);
}

void FullscreenUI::TextAlignedMultiLine(float align_x, const char* text, const char* text_end, float wrap_width)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return;

  ImGuiContext& g = *GImGui;
  IM_ASSERT(text != NULL);

  if (text_end == NULL)
    text_end = text + strlen(text);

  const ImVec2 text_pos = window->DC.CursorPos;
  const ImRect clip_rect = window->ClipRect;
  wrap_width = (wrap_width < 0.0f) ? ImGui::GetContentRegionAvail().x : wrap_width;

  ImVec2 pos = text_pos;
  const char* text_remaining = text;

  while (text_remaining < text_end)
  {
    // Find the end of the current wrapped line
    const char* line_end = text_remaining;

    // Process text word by word to find natural line breaks
    while (line_end < text_end)
    {
      const char* word_start = line_end;
      const char* word_end = word_start;

      // Handle explicit newlines
      if (*line_end == '\n')
      {
        line_end++;
        break;
      }

      // Find end of current word (including spaces)
      while (word_end < text_end && *word_end != ' ' && *word_end != '\n')
        word_end++;

      // Include trailing space if present
      if (word_end < text_end && *word_end == ' ')
        word_end++;

      // Calculate width if we add this word
      const ImVec2 word_size = ImGui::CalcTextSize(text_remaining, word_end, false, -1.0f);

      // If adding this word would exceed wrap width, break here
      if (word_size.x > wrap_width && line_end > text_remaining)
        break;

      line_end = word_end;
    }

    // If we didn't advance at all, force at least one character to prevent infinite loop
    if (line_end == text_remaining && line_end < text_end)
      line_end++;

    // Calculate actual line size for the determined line segment
    const ImVec2 line_size = ImGui::CalcTextSize(text_remaining, line_end, false, -1.0f);

    // Calculate aligned position for this line
    ImVec2 line_pos = pos;
    if (align_x > 0.0f)
    {
      float offset_x = (wrap_width - line_size.x) * align_x;
      line_pos.x += offset_x;
    }

    // Render the line
    if (line_size.x > 0.0f)
    {
      ImGui::RenderTextClipped(line_pos, ImVec2(line_pos.x + line_size.x, line_pos.y + line_size.y), text_remaining,
                               line_end, &line_size, ImVec2(0.0f, 0.0f), &clip_rect);
    }

    // Move to next line
    pos.y += g.FontSize + g.Style.ItemSpacing.y;
    text_remaining = line_end;

    // Skip trailing spaces at the beginning of the next line
    while (text_remaining < text_end && *text_remaining == ' ')
      text_remaining++;
  }

  // Update cursor position to account for the rendered text
  const ImVec2 text_size = ImVec2(wrap_width, pos.y - text_pos.y);
  const ImRect bb(text_pos, text_pos + text_size);
  ImGui::ItemSize(text_size);
  ImGui::ItemAdd(bb, 0);
}

void FullscreenUI::TextUnformatted(std::string_view text)
{
  ImGui::TextUnformatted(IMSTR_START_END(text));
}

void FullscreenUI::MenuHeading(std::string_view title, bool draw_line /*= true*/)
{
  const float line_thickness = LayoutScale(1.0f);
  const float line_padding = LayoutScale(5.0f);
  const float avail_width = MenuButtonBounds::CalcAvailWidth();

  const ImGuiWindow* const window = ImGui::GetCurrentWindowRead();
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 pos = ImVec2(window->DC.CursorPos + style.FramePadding);

  static constexpr const float& font_size = UIStyle.LargeFontSize;
  static constexpr const float& font_weight = UIStyle.BoldFontWeight;
  const ImVec2 title_size =
    UIStyle.Font->CalcTextSizeA(font_size, font_weight, FLT_MAX, avail_width, IMSTR_START_END(title));
  const u32 title_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  RenderShadowedTextClipped(UIStyle.Font, font_size, font_weight, pos, pos + title_size, title_color, title,
                            &title_size, ImVec2(0.0f, 0.0f), avail_width);

  float total_height = UIStyle.LargeFontSize + style.FramePadding.y;

  if (draw_line)
  {
    const ImVec2 line_start = ImVec2(pos.x, pos.y + title_size.y + line_padding);
    const ImVec2 line_end = ImVec2(line_start.x + avail_width, line_start.y);

    window->DrawList->AddLine(line_start, line_end, title_color, line_thickness);
    total_height += line_thickness + line_padding;
  }

  ImGui::Dummy(ImVec2(0.0f, total_height));
}

bool FullscreenUI::MenuHeadingButton(std::string_view title, std::string_view value /*= {}*/,
                                     float font_size /*= UIStyle.LargeFontSize */, bool enabled /*= true*/,
                                     bool draw_line /*= true*/)
{
  const MenuButtonBounds bb(title, value, {}, 0.0f, font_size);
  bool visible, hovered;
  const bool pressed = MenuButtonFrame(title, enabled, bb.frame_bb, &visible, &hovered);
  if (!visible)
    return false;

  const u32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  RenderShadowedTextClipped(UIStyle.Font, font_size, UIStyle.BoldFontWeight, bb.title_bb.Min, bb.title_bb.Max, color,
                            title, &bb.title_size, ImVec2(0.0f, 0.0f), bb.title_size.x, &bb.title_bb);

  if (!value.empty())
  {
    RenderShadowedTextClipped(UIStyle.Font, font_size, UIStyle.BoldFontWeight, bb.value_bb.Min, bb.value_bb.Max, color,
                              value, &bb.value_size, ImVec2(0.0f, 0.0f), bb.value_size.x, &bb.value_bb);
  }

  if (draw_line)
  {
    const float line_thickness = draw_line ? LayoutScale(1.0f) : 0.0f;
    const float line_padding = draw_line ? LayoutScale(5.0f) : 0.0f;
    const ImVec2 line_start(bb.title_bb.Min.x, bb.title_bb.Max.y + line_padding);
    const ImVec2 line_end(bb.title_bb.Min.x + bb.available_width, line_start.y);
    const ImVec2 shadow_offset = LayoutScale(LAYOUT_SHADOW_OFFSET, LAYOUT_SHADOW_OFFSET);
    ImGui::GetWindowDrawList()->AddLine(line_start + shadow_offset, line_end + shadow_offset, UIStyle.ShadowColor,
                                        line_thickness);
    ImGui::GetWindowDrawList()->AddLine(line_start, line_end, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                        line_thickness);
  }

  return pressed;
}

bool FullscreenUI::MenuButton(std::string_view title, std::string_view summary, bool enabled /* = true */,
                              const ImVec2& text_align /* = ImVec2(0.0f, 0.0f) */)
{
  bool visible;
  return MenuButtonWithVisibilityQuery(title, title, summary, {}, &visible, enabled, text_align);
}

bool FullscreenUI::MenuButtonWithoutSummary(std::string_view title, bool enabled /* = true */,
                                            const ImVec2& text_align /* = ImVec2(0.0f, 0.0f) */)
{
  bool visible;
  return MenuButtonWithVisibilityQuery(title, title, {}, {}, &visible, enabled, text_align);
}

bool FullscreenUI::MenuButtonWithValue(std::string_view title, std::string_view summary, std::string_view value,
                                       bool enabled /* = true*/, const ImVec2& text_align /* = ImVec2(0.0f, 0.0f)*/)
{
  bool visible;
  return MenuButtonWithVisibilityQuery(title, title, summary, value, &visible, enabled, text_align);
}

bool FullscreenUI::MenuButtonWithVisibilityQuery(std::string_view str_id, std::string_view title,
                                                 std::string_view summary, std::string_view value, bool* visible,
                                                 bool enabled /* = true */,
                                                 const ImVec2& text_align /* = ImVec2(0.0f, 0.0f) */)
{
  const MenuButtonBounds bb(title, value, summary);

  bool hovered;
  bool pressed = MenuButtonFrame(str_id, enabled, bb.frame_bb, visible, &hovered);
  if (!*visible)
    return false;

  const ImVec4& color = ImGui::GetStyle().Colors[enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled];
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.title_bb.Min,
                            bb.title_bb.Max, ImGui::GetColorU32(color), title, &bb.title_size, text_align,
                            bb.title_size.x, &bb.title_bb);

  if (!value.empty())
  {
    RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.value_bb.Min,
                              bb.value_bb.Max, ImGui::GetColorU32(color), value, &bb.value_size, ImVec2(1.0f, 0.5f),
                              bb.value_size.x, &bb.value_bb);
  }

  if (!summary.empty())
  {
    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, bb.summary_bb.Min,
                              bb.summary_bb.Max, ImGui::GetColorU32(DarkerColor(color)), summary, &bb.summary_size,
                              text_align, bb.summary_size.x, &bb.summary_bb);
  }

  return pressed;
}

bool FullscreenUI::MenuImageButton(std::string_view title, std::string_view summary, std::string_view value,
                                   ImTextureID image, const ImVec2& image_size, bool enabled /*= true*/,
                                   const ImVec2& uv0 /*= ImVec2(0.0f, 0.0f)*/,
                                   const ImVec2& uv1 /*= ImVec2(1.0f, 1.0f)*/)
{
  ImVec2 real_image_size = image_size;
  if (real_image_size.x <= 0.0f || real_image_size.y <= 0.0f)
  {
    const float size =
      summary.empty() ? MenuButtonBounds::GetSingleLineHeight(0.0f) : MenuButtonBounds::GetSummaryLineHeight(0.0f);
    real_image_size = ImVec2(size, size);
  }

  const float image_margin = LayoutScale(15.0f);
  const float left_margin = real_image_size.x + image_margin;
  const MenuButtonBounds bb(title, value, summary, left_margin);

  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, bb.frame_bb, &visible, &hovered);
  if (!visible)
    return false;

  const ImRect image_rect(
    CenterImage(ImRect(ImVec2(bb.title_bb.Min.x - left_margin, bb.title_bb.Min.y),
                       ImVec2(bb.title_bb.Min.x - image_margin, bb.title_bb.Min.y + real_image_size.x)),
                ImVec2(static_cast<float>(image->GetWidth()), static_cast<float>(image->GetHeight()))));

  ImGui::GetWindowDrawList()->AddImage(image, image_rect.Min, image_rect.Max, uv0, uv1,
                                       enabled ? IM_COL32(255, 255, 255, 255) :
                                                 ImGui::GetColorU32(ImGuiCol_TextDisabled));

  const ImVec4& color = ImGui::GetStyle().Colors[enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled];
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.title_bb.Min,
                            bb.title_bb.Max, ImGui::GetColorU32(color), title, &bb.title_size, ImVec2(0.0f, 0.0f),
                            bb.title_size.x, &bb.title_bb);

  if (!value.empty())
  {
    RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.value_bb.Min,
                              bb.value_bb.Max, ImGui::GetColorU32(color), value, &bb.value_size, ImVec2(1.0f, 0.5f),
                              bb.value_size.x, &bb.value_bb);
  }

  if (!summary.empty())
  {
    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, bb.summary_bb.Min,
                              bb.summary_bb.Max, ImGui::GetColorU32(DarkerColor(color)), summary, &bb.summary_size,
                              ImVec2(0.0f, 0.0f), bb.summary_size.x, &bb.summary_bb);
  }

  return pressed;
}

bool FullscreenUI::FloatingButton(std::string_view text, float x, float y, float anchor_x /* = 0.0f */,
                                  float anchor_y /* = 0.0f */, bool enabled /* = true */,
                                  ImVec2* out_position /* = nullptr */, bool repeat_button /* = false */)
{
  const ImVec2 text_size = UIStyle.Font->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight,
                                                       std::numeric_limits<float>::max(), 0.0f, IMSTR_START_END(text));
  const ImVec2& padding = ImGui::GetStyle().FramePadding;
  const float width = (padding.x * 2.0f) + text_size.x;
  const float height = (padding.y * 2.0f) + text_size.y;

  const ImVec2 window_size = ImGui::GetWindowSize();
  if (anchor_x == -1.0f)
    x -= width;
  else if (anchor_x == -0.5f)
    x -= (width * 0.5f);
  else if (anchor_x == 0.5f)
    x = (window_size.x * 0.5f) - (width * 0.5f) - LayoutScale(x);
  else if (anchor_x == 1.0f)
    x = window_size.x - width - LayoutScale(x);
  else
    x = LayoutScale(x);
  if (anchor_y == -1.0f)
    y -= height;
  else if (anchor_y == -0.5f)
    y -= (height * 0.5f);
  else if (anchor_y == 0.5f)
    y = (window_size.y * 0.5f) - (height * 0.5f) - LayoutScale(y);
  else if (anchor_y == 1.0f)
    y = window_size.y - height - LayoutScale(y);
  else
    y = LayoutScale(y);

  if (out_position)
    *out_position = ImVec2(x, y);

  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;

  const ImVec2 base(ImGui::GetWindowPos() + ImVec2(x, y));
  ImRect bb(base, base + ImVec2(width, height));

  const ImGuiID id = window->GetID(IMSTR_START_END(text));
  if (enabled)
  {
    if (!ImGui::ItemAdd(bb, id, nullptr, repeat_button ? ImGuiItemFlags_ButtonRepeat : 0))
      return false;
  }
  else
  {
    if (ImGui::IsClippedEx(bb, id))
      return false;
  }

  bool hovered;
  bool held;
  bool pressed;
  if (enabled)
  {
    pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (hovered)
    {
      const ImU32 col = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
      DrawMenuButtonFrameAtOnCurrentLayer(bb.Min, bb.Max, col, true);
    }
  }
  else
  {
    hovered = false;
    pressed = false;
    held = false;
  }

  bb.Min += padding;
  bb.Max -= padding;

  const u32 color = enabled ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.Min, bb.Max, color, text,
                            nullptr, ImVec2(0.0f, 0.0f), 0.0f, &bb);
  return pressed;
}

bool FullscreenUI::ToggleButton(std::string_view title, std::string_view summary, bool* v, bool enabled /* = true */)
{
  const ImVec2 toggle_size = LayoutScale(50.0f, 25.0f);
  const MenuButtonBounds bb(title, ImVec2(toggle_size.x, toggle_size.y), summary);

  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, bb.frame_bb, &visible, &hovered, ImGuiButtonFlags_PressedOnClick);
  if (!visible)
    return false;

  const ImVec4& color = ImGui::GetStyle().Colors[enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled];

  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.title_bb.Min,
                            bb.title_bb.Max, ImGui::GetColorU32(color), title, &bb.title_size, ImVec2(0.0f, 0.0f),
                            bb.title_size.x, &bb.title_bb);

  if (!summary.empty())
  {
    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, bb.summary_bb.Min,
                              bb.summary_bb.Max, ImGui::GetColorU32(DarkerColor(color)), summary, &bb.summary_size,
                              ImVec2(0.0f, 0.0f), bb.summary_size.x, &bb.summary_bb);
  }

  if (pressed)
    *v = !*v;

  float t = *v ? 1.0f : 0.0f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiContext& g = *GImGui;
  if (UIStyle.Animations &&
      g.LastActiveId == g.CurrentWindow->GetID(IMSTR_START_END(title))) // && g.LastActiveIdTimer < ANIM_SPEED)
  {
    static constexpr const float ANIM_SPEED = 0.08f;
    float t_anim = ImSaturate(g.LastActiveIdTimer / ANIM_SPEED);
    t = *v ? (t_anim) : (1.0f - t_anim);
  }

  ImU32 col_bg;
  ImU32 col_knob;
  if (!enabled)
  {
    col_bg = ImGui::GetColorU32(UIStyle.DisabledColor);
    col_knob = IM_COL32(200, 200, 200, 200);
  }
  else
  {
    col_bg = ImGui::GetColorU32(ImLerp(HEX_TO_IMVEC4(0x8C8C8C, 0xff), UIStyle.SecondaryStrongColor, t));
    col_knob = IM_COL32(255, 255, 255, 255);
  }

  const float toggle_radius = toggle_size.y * 0.5f;
  const float toggle_y_offset = ImFloor((bb.value_bb.GetHeight() - toggle_size.y) * 0.5f);
  const ImVec2 toggle_pos = ImVec2(bb.value_bb.Min.x, bb.value_bb.Min.y + toggle_y_offset);
  dl->AddRectFilled(toggle_pos, ImVec2(toggle_pos.x + toggle_size.x, toggle_pos.y + toggle_size.y), col_bg,
                    toggle_size.y * 0.5f);
  dl->AddCircleFilled(
    ImVec2(toggle_pos.x + toggle_radius + t * (toggle_size.x - toggle_radius * 2.0f), toggle_pos.y + toggle_radius),
    toggle_radius - 1.5f, col_knob, 32);

  return pressed;
}

bool FullscreenUI::ThreeWayToggleButton(std::string_view title, std::string_view summary, std::optional<bool>* v,
                                        bool enabled /* = true */)
{
  const ImVec2 toggle_size = LayoutScale(50.0f, 25.0f);
  const MenuButtonBounds bb(title, ImVec2(toggle_size.x, toggle_size.y), summary);

  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, bb.frame_bb, &visible, &hovered, ImGuiButtonFlags_PressedOnClick);
  if (!visible)
    return false;

  const ImVec4& color = ImGui::GetStyle().Colors[enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled];

  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.title_bb.Min,
                            bb.title_bb.Max, ImGui::GetColorU32(color), title, &bb.title_size, ImVec2(0.0f, 0.0f),
                            bb.title_size.x, &bb.title_bb);

  if (!summary.empty())
  {
    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, bb.summary_bb.Min,
                              bb.summary_bb.Max, ImGui::GetColorU32(DarkerColor(color)), summary, &bb.summary_size,
                              ImVec2(0.0f, 0.0f), bb.summary_size.x, &bb.summary_bb);
  }

  if (pressed)
  {
    if (v->has_value() && v->value())
      *v = false;
    else if (v->has_value() && !v->value())
      v->reset();
    else
      *v = true;
  }

  float t = v->has_value() ? (v->value() ? 1.0f : 0.0f) : 0.5f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiContext& g = *GImGui;
  float ANIM_SPEED = 0.08f;
  if (g.LastActiveId == g.CurrentWindow->GetID(IMSTR_START_END(title))) // && g.LastActiveIdTimer < ANIM_SPEED)
  {
    float t_anim = ImSaturate(g.LastActiveIdTimer / ANIM_SPEED);
    t = (v->has_value() ? (v->value() ? std::min(t_anim + 0.5f, 1.0f) : (1.0f - t_anim)) : (t_anim * 0.5f));
  }

  const float color_t = v->has_value() ? t : 0.0f;

  ImU32 col_bg;
  if (!enabled)
    col_bg = IM_COL32(0x75, 0x75, 0x75, 0xff);
  else if (hovered)
    col_bg = ImGui::GetColorU32(ImLerp(v->has_value() ? HEX_TO_IMVEC4(0xf05100, 0xff) : HEX_TO_IMVEC4(0x9e9e9e, 0xff),
                                       UIStyle.SecondaryStrongColor, color_t));
  else
    col_bg = ImGui::GetColorU32(ImLerp(v->has_value() ? HEX_TO_IMVEC4(0xc45100, 0xff) : HEX_TO_IMVEC4(0x757575, 0xff),
                                       UIStyle.SecondaryStrongColor, color_t));

  const float toggle_radius = toggle_size.y * 0.5f;
  const float toggle_y_offset = ImFloor((bb.value_bb.GetHeight() - toggle_size.y) * 0.5f);
  const ImVec2 toggle_pos = ImVec2(bb.value_bb.Min.x, bb.value_bb.Min.y + toggle_y_offset);
  dl->AddRectFilled(toggle_pos, ImVec2(toggle_pos.x + toggle_size.x, toggle_pos.y + toggle_size.y), col_bg,
                    toggle_size.y * 0.5f);
  dl->AddCircleFilled(
    ImVec2(toggle_pos.x + toggle_radius + t * (toggle_size.x - toggle_radius * 2.0f), toggle_pos.y + toggle_radius),
    toggle_radius - 1.5f, IM_COL32(255, 255, 255, 255), 32);

  return pressed;
}

bool FullscreenUI::RangeButton(std::string_view title, std::string_view summary, s32* value, s32 min, s32 max,
                               s32 increment, const char* format /* = "%d" */, bool enabled /* = true */,
                               std::string_view ok_text /* = "OK" */)
{
  const SmallString value_text = SmallString::from_sprintf(format, *value);
  if (MenuButtonWithValue(title, summary, value_text, enabled))
    OpenFixedPopupDialog(title);

  bool changed = false;

  if (IsFixedPopupDialogOpen(title) &&
      BeginFixedPopupDialog(LayoutScale(LAYOUT_SMALL_POPUP_PADDING), LayoutScale(LAYOUT_SMALL_POPUP_PADDING),
                            LayoutScale(500.0f, 200.0f)))
  {
    BeginMenuButtons();

    const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    ImGui::SetNextItemWidth(end);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, LayoutScale(15.0f));

    changed = ImGui::SliderInt("##value", value, min, max, format, ImGuiSliderFlags_NoInput);

    ImGui::PopStyleVar(4);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
    if (MenuButtonWithoutSummary(ok_text, true, LAYOUT_CENTER_ALIGN_TEXT))
      CloseFixedPopupDialog();

    EndMenuButtons();

    EndFixedPopupDialog();
  }

  return changed;
}

bool FullscreenUI::RangeButton(std::string_view title, std::string_view summary, float* value, float min, float max,
                               float increment, const char* format /* = "%f" */, bool enabled /* = true */,
                               std::string_view ok_text /* = "OK" */)
{
  const SmallString value_text = SmallString::from_sprintf(format, *value);
  if (MenuButtonWithValue(title, summary, value_text, enabled))
    OpenFixedPopupDialog(title);

  bool changed = false;

  if (IsFixedPopupDialogOpen(title) &&
      BeginFixedPopupDialog(LayoutScale(LAYOUT_SMALL_POPUP_PADDING), LayoutScale(LAYOUT_SMALL_POPUP_PADDING),
                            LayoutScale(500.0f, 200.0f)))
  {
    BeginMenuButtons();

    const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    ImGui::SetNextItemWidth(end);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, LayoutScale(LAYOUT_WIDGET_FRAME_ROUNDING));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, LayoutScale(15.0f));

    changed = ImGui::SliderFloat("##value", value, min, max, format, ImGuiSliderFlags_NoInput);

    ImGui::PopStyleVar(4);

    if (MenuButtonWithoutSummary(ok_text, true, LAYOUT_CENTER_ALIGN_TEXT))
      CloseFixedPopupDialog();

    EndMenuButtons();

    EndFixedPopupDialog();
  }

  return changed;
}

bool FullscreenUI::EnumChoiceButtonImpl(std::string_view title, std::string_view summary, s32* value_pointer,
                                        const char* (*to_display_name_function)(s32 value, void* opaque), void* opaque,
                                        u32 count, bool enabled)
{
  const bool pressed = MenuButtonWithValue(title, summary, to_display_name_function(*value_pointer, opaque), enabled);

  if (pressed)
  {
    s_state.enum_choice_button_id = ImGui::GetID(IMSTR_START_END(title));
    s_state.enum_choice_button_value = *value_pointer;
    s_state.enum_choice_button_set = false;

    ChoiceDialogOptions options;
    options.reserve(count);
    for (u32 i = 0; i < count; i++)
      options.emplace_back(to_display_name_function(static_cast<s32>(i), opaque),
                           static_cast<u32>(*value_pointer) == i);
    OpenChoiceDialog(title, false, std::move(options), [](s32 index, const std::string& title, bool checked) {
      if (index >= 0)
        s_state.enum_choice_button_value = index;

      s_state.enum_choice_button_set = true;
      CloseChoiceDialog();
    });
  }

  bool changed = false;
  if (s_state.enum_choice_button_set && s_state.enum_choice_button_id == ImGui::GetID(IMSTR_START_END(title)))
  {
    changed = s_state.enum_choice_button_value != *value_pointer;
    if (changed)
      *value_pointer = s_state.enum_choice_button_value;

    s_state.enum_choice_button_id = 0;
    s_state.enum_choice_button_value = 0;
    s_state.enum_choice_button_set = false;
  }

  return changed;
}

void FullscreenUI::BeginHorizontalMenuButtons(u32 num_items, float max_item_width /* = 0.0f */,
                                              float x_padding /* = LAYOUT_MENU_BUTTON_Y_PADDING */,
                                              float y_padding /* = LAYOUT_MENU_BUTTON_Y_PADDING */,
                                              float x_spacing /* = LAYOUT_MENU_BUTTON_X_PADDING */,
                                              float x_margin /* = LAYOUT_MENU_WINDOW_X_PADDING */)
{
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(x_padding, y_padding));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, LayoutScale(UIStyle.MenuBorders ? 1.0f : 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, LayoutScale(x_spacing, 0.0f));

  ImGuiWindow* const window = ImGui::GetCurrentWindow();
  const ImGuiStyle& style = ImGui::GetStyle();
  window->DC.LayoutType = ImGuiLayoutType_Horizontal;

  const float available_width = ImGui::GetContentRegionAvail().x;
  const float space_per_item = ImFloor(
    std::max((available_width - LayoutScale(x_margin) - (style.ItemSpacing.x * static_cast<float>(num_items - 1))) /
               static_cast<float>(num_items),
             0.0f));
  s_state.horizontal_menu_button_size = ImVec2(space_per_item, MenuButtonBounds::GetSingleLineHeight(y_padding));
  s_state.horizontal_menu_button_size.x =
    (max_item_width > 0.0f) ?
      std::min(s_state.horizontal_menu_button_size.x, LayoutScale(max_item_width) + (style.FramePadding.x * 2.0f)) :
      s_state.horizontal_menu_button_size.x;

  const float left_padding =
    ImFloor((available_width -
             (((s_state.horizontal_menu_button_size.x + style.ItemSpacing.x) * static_cast<float>(num_items)) -
              style.ItemSpacing.x)) *
            0.5f);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left_padding);

  BeginMenuButtonDrawSplit();
}

void FullscreenUI::EndHorizontalMenuButtons(float add_vertical_spacing /*= -1.0f*/)
{
  ImGui::PopStyleVar(4);
  ImGui::GetCurrentWindow()->DC.LayoutType = ImGuiLayoutType_Vertical;

  const float dummy_height = ImGui::GetCurrentWindowRead()->DC.CurrLineSize.y +
                             ((add_vertical_spacing > 0.0f) ? LayoutScale(add_vertical_spacing) : 0.0f);
  ImGui::ItemSize(ImVec2(0.0f, (dummy_height > 0.0f) ? dummy_height : ImGui::GetFontSize()));

  PostDrawMenuButtonFrame();
  EndMenuButtonDrawSplit();
}

bool FullscreenUI::HorizontalMenuButton(std::string_view title, bool enabled /* = true */,
                                        const ImVec2& text_align /* = LAYOUT_CENTER_ALIGN_TEXT */,
                                        ImGuiButtonFlags flags /*= 0 */)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;

  const ImVec2 pos = window->DC.CursorPos;
  const ImVec2& size = s_state.horizontal_menu_button_size;

  ImRect bb(pos, pos + size);

  const ImGuiID id = window->GetID(IMSTR_START_END(title));
  ImGui::ItemSize(size);
  if (enabled)
  {
    if (!ImGui::ItemAdd(bb, id))
      return false;
  }
  else
  {
    if (ImGui::IsClippedEx(bb, id))
      return false;
  }

  // horizontal menu buttons always have frames
  const ImU32 frame_background = ImGui::GetColorU32(
    DarkerColor((window->Flags & ImGuiWindowFlags_Popup) ? UIStyle.PopupBackgroundColor : UIStyle.BackgroundColor));
  SetMenuButtonSplitLayer(MENU_BUTTON_SPLIT_LAYER_BACKGROUND);
  ImGui::RenderFrame(bb.Min, bb.Max, frame_background, false, LayoutScale(LAYOUT_MENU_ITEM_BORDER_ROUNDING));
  SetMenuButtonSplitLayer(MENU_BUTTON_SPLIT_LAYER_FOREGROUND);

  bool hovered;
  bool held;
  bool pressed;
  if (enabled)
  {
    pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, flags);
    if (hovered)
      DrawMenuButtonFrame(bb.Min, bb.Max, held);
  }
  else
  {
    pressed = false;
    held = false;
  }

  const ImGuiStyle& style = ImGui::GetStyle();
  bb.Min += style.FramePadding;
  bb.Max -= style.FramePadding;

  const ImVec4& color = ImGui::GetStyle().Colors[enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled];
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.Min, bb.Max,
                            ImGui::GetColorU32(color), title, nullptr, text_align, 0.0f, &bb);

  return pressed;
}

void FullscreenUI::BeginNavBar(float x_padding /*= LAYOUT_MENU_BUTTON_X_PADDING*/,
                               float y_padding /*= LAYOUT_MENU_BUTTON_Y_PADDING*/)
{
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(x_padding, y_padding));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, LayoutScale(1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, LayoutScale(1.0f, 0.0f));
  PushPrimaryColor();
  BeginMenuButtonDrawSplit();
}

void FullscreenUI::EndNavBar()
{
  EndMenuButtonDrawSplit();
  PopPrimaryColor();
  ImGui::PopStyleVar(4);
}

void FullscreenUI::NavTitle(std::string_view title)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return;

  const ImVec2 text_size(UIStyle.Font->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight,
                                                     std::numeric_limits<float>::max(), 0.0f, IMSTR_START_END(title)));
  const ImVec2 pos(window->DC.CursorPos);
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 size = ImVec2(text_size.x, text_size.y + style.FramePadding.y * 2.0f);

  ImGui::ItemSize(
    ImVec2(size.x + style.FrameBorderSize + style.ItemSpacing.x, size.y + style.FrameBorderSize + style.ItemSpacing.y));
  ImGui::SameLine();

  ImRect bb(pos, pos + size);
  if (ImGui::IsClippedEx(bb, 0))
    return;

  bb.Min.y += style.FramePadding.y;
  bb.Max.y -= style.FramePadding.y;

  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.Min, bb.Max,
                            ImGui::GetColorU32(ImGuiCol_Text), title, &text_size, ImVec2(0.0f, 0.0f), 0.0f, &bb);
}

void FullscreenUI::RightAlignNavButtons(u32 num_items)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float total_item_width = style.FramePadding.x * 2.0f + style.FrameBorderSize + style.ItemSpacing.x +
                                 LayoutScale(LAYOUT_LARGE_FONT_SIZE - 1.0f);
  const float margin = total_item_width * static_cast<float>(num_items);
  ImGui::SetCursorPosX(window->InnerClipRect.Max.x - margin - style.FramePadding.x);
}

void FullscreenUI::RightAlignNavButtons(float total_width)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  const ImGuiStyle& style = ImGui::GetStyle();

  ImGui::SetCursorPosX(window->InnerClipRect.Max.x - total_width - style.FramePadding.x);
}

bool FullscreenUI::NavButton(std::string_view title, bool is_active, bool enabled /* = true */)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;

  const ImVec2 text_size(UIStyle.Font->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight,
                                                     std::numeric_limits<float>::max(), 0.0f, IMSTR_START_END(title)));
  const ImVec2 pos(window->DC.CursorPos);
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 size = ImVec2(LayoutScale(LAYOUT_LARGE_FONT_SIZE - 1.0f) + style.FramePadding.x * 2.0f,
                             text_size.y + style.FramePadding.y * 2.0f);

  ImGui::ItemSize(
    ImVec2(size.x + style.FrameBorderSize + style.ItemSpacing.x, size.y + style.FrameBorderSize + style.ItemSpacing.y));
  ImGui::SameLine();

  ImRect bb(pos, pos + size);
  const ImGuiID id = window->GetID(IMSTR_START_END(title));
  if (enabled)
  {
    // bit contradictory - we don't want this button to be used for *gamepad* navigation, since they're usually
    // activated with the bumpers and/or the back button.
    if (!ImGui::ItemAdd(bb, id, nullptr, ImGuiItemFlags_NoNav | ImGuiItemFlags_NoNavDefaultFocus))
      return false;
  }
  else
  {
    if (ImGui::IsClippedEx(bb, id))
      return false;
  }

  bool held;
  bool pressed;
  bool hovered;
  if (enabled)
  {
    pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_NoNavFocus);
    if (hovered)
      DrawMenuButtonFrame(bb.Min, bb.Max, held);
  }
  else
  {
    pressed = false;
    held = false;
    hovered = false;
  }

  bb.Min += style.FramePadding;
  bb.Max -= style.FramePadding;

  RenderShadowedTextClipped(
    UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.Min, bb.Max,
    ImGui::GetColorU32(enabled ? (is_active ? ImGuiCol_Text : ImGuiCol_TextDisabled) : ImGuiCol_ButtonHovered), title,
    &text_size, ImVec2(0.0f, 0.0f), 0.0f, &bb);

  return pressed;
}

bool FullscreenUI::NavTab(std::string_view title, bool is_active, bool enabled, float width)
{
  ImGuiWindow* const window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;

  const ImVec2 text_size = UIStyle.Font->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight,
                                                       std::numeric_limits<float>::max(), 0.0f, IMSTR_START_END(title));

  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 pos = window->DC.CursorPos;
  const ImVec2 size = ImVec2(((width < 0.0f) ? text_size.x : width), text_size.y) + (style.FramePadding * 2.0f);

  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
  ImGui::ItemSize(size);
  ImGui::SameLine();
  ImGui::PopStyleVar();

  ImRect bb(pos, pos + size);
  const ImGuiID id = window->GetID(IMSTR_START_END(title));
  if (enabled)
  {
    // bit contradictory - we don't want this button to be used for *gamepad* navigation, since they're usually
    // activated with the bumpers and/or the back button.
    if (!ImGui::ItemAdd(bb, id, nullptr, ImGuiItemFlags_NoNav | ImGuiItemFlags_NoNavDefaultFocus))
      return false;
  }
  else
  {
    if (ImGui::IsClippedEx(bb, id))
      return false;
  }

  bool held;
  bool pressed;
  bool hovered;
  if (enabled)
  {
    pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_NoNavFocus);
  }
  else
  {
    pressed = false;
    held = false;
    hovered = false;
  }

  if (is_active || hovered)
  {
    const ImU32 col = hovered ? ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered, 1.0f) :
                                ImGui::GetColorU32(DarkerColor(style.Colors[ImGuiCol_ButtonHovered]));
    ImGui::RenderFrame(bb.Min, bb.Max, col, true, LayoutScale(LAYOUT_MENU_ITEM_BORDER_ROUNDING));
  }

  const ImVec2 pad(std::max((size.x - text_size.x) * 0.5f, 0.0f), std::max((size.y - text_size.y) * 0.5f, 0.0f));
  bb.Min += pad;
  bb.Max -= pad;

  RenderShadowedTextClipped(
    UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.Min, bb.Max,
    ImGui::GetColorU32(enabled ? (is_active ? ImGuiCol_Text : ImGuiCol_TextDisabled) : ImGuiCol_ButtonHovered), title,
    nullptr, ImVec2(0.0f, 0.0f), 0.0f, &bb);

  return pressed;
}

bool FullscreenUI::BeginFloatingNavBar(float x, float y, float items_width,
                                       float font_size /* = UIStyle.LargeFontSize */, float anchor_x /* = 0.0f */,
                                       float anchor_y /* = 0.0f */,
                                       float x_padding /* = LAYOUT_MENU_BUTTON_X_PADDING */,
                                       float y_padding /* = LAYOUT_MENU_BUTTON_Y_PADDING */)
{
  if (ImGui::GetCurrentWindowRead()->SkipItems)
    return false;

  const ImVec2 padding = LayoutScale(x_padding, y_padding);
  const float width = (padding.x * 2.0f) + items_width;
  const float height = (padding.y * 2.0f) + font_size;

  const ImVec2 window_size = ImGui::GetWindowSize();
  ImVec2 local_pos = LayoutScale(x, y);
  if (anchor_x == -1.0f)
    local_pos.x -= width;
  else if (anchor_x == -0.5f)
    local_pos.x -= (width * 0.5f);
  else if (anchor_x == 0.5f)
    local_pos.x = (window_size.x * 0.5f) - (width * 0.5f) - local_pos.x;
  else if (anchor_x == 1.0f)
    local_pos.x = window_size.x - width - local_pos.x;
  if (anchor_y == -1.0f)
    local_pos.y -= height;
  else if (anchor_y == -0.5f)
    local_pos.y -= (height * 0.5f);
  else if (anchor_y == 0.5f)
    local_pos.y = (window_size.y * 0.5f) - (height * 0.5f) - local_pos.y;
  else if (anchor_y == 1.0f)
    local_pos.y = window_size.y - height - local_pos.y;

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(x_padding, y_padding));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, LayoutScale(1.0f, 0.0f));
  PushPrimaryColor();

  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 nav_pos = ImGui::GetWindowPos() + local_pos;
  const ImVec2 nav_size = ImVec2(items_width, font_size + style.FramePadding.y * 2.0f);
  ImGui::GetWindowDrawList()->AddRectFilled(nav_pos, nav_pos + nav_size,
                                            ImGui::GetColorU32(ModAlpha(UIStyle.BackgroundHighlight, 0.4f)),
                                            LayoutScale(LAYOUT_MENU_ITEM_BORDER_ROUNDING));
  ImGui::SetCursorPos(local_pos);

  return true;
}

void FullscreenUI::EndFloatingNavBar()
{
  PopPrimaryColor();
  ImGui::PopStyleVar(4);
}

float FullscreenUI::CalcFloatingNavBarButtonWidth(std::string_view title, float icon_size /* = UIStyle.LargeFontSize */,
                                                  float font_size /* = UIStyle.LargeFontSize */,
                                                  float font_weight /* = UIStyle.BoldFontWeight */,
                                                  float x_padding /* = LAYOUT_MENU_BUTTON_X_PADDING */)
{
  float text_width;
  if (!title.empty())
  {
    const ImVec2 text_size = UIStyle.Font->CalcTextSizeA(font_size, font_weight, std::numeric_limits<float>::max(),
                                                         0.0f, IMSTR_START_END(title));
    text_width = text_size.x;

    if (icon_size > 0.0f)
    {
      text_width +=
        (UIStyle.Font->CalcTextSizeA(font_size, font_weight, std::numeric_limits<float>::max(), 0.0f, " ").x * 2.0f);
    }
  }
  else
  {
    text_width = 0.0f;
  }

  return icon_size + text_width + (LayoutScale(x_padding) * 2.0f) + /* spacing */ LayoutScale(1.0f);
}

bool FullscreenUI::FloatingNavBarIcon(std::string_view title, ImTextureID image, bool is_active,
                                      float image_size /* = UIStyle.LargeFontSize */,
                                      float font_size /* = UIStyle.LargeFontSize */,
                                      float font_weight /* = UIStyle.BoldFontWeight */, bool enabled /* = true */,
                                      const ImVec2& uv0 /* = ImVec2(0.0f, 0.0f) */,
                                      const ImVec2& uv1 /* = ImVec2(1.0f, 1.0f) */)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;

  const ImVec2 text_size = !title.empty() ?
                             UIStyle.Font->CalcTextSizeA(font_size, font_weight, std::numeric_limits<float>::max(),
                                                         0.0f, IMSTR_START_END(title)) :
                             ImVec2();
  const float text_space_size =
    (!title.empty() && image) ?
      (UIStyle.Font->CalcTextSizeA(font_size, font_weight, std::numeric_limits<float>::max(), 0.0f, " ").x * 2.0f) :
      0.0f;
  const ImVec2 pos(window->DC.CursorPos);
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 size = ImVec2(image_size + text_space_size + text_size.x + style.FramePadding.x * 2.0f,
                             text_size.y + style.FramePadding.y * 2.0f);

  ImGui::ItemSize(
    ImVec2(size.x + style.FrameBorderSize + style.ItemSpacing.x, size.y + style.FrameBorderSize + style.ItemSpacing.y));
  ImGui::SameLine();

  ImRect bb(pos, pos + size);
  const ImGuiID id = window->GetID(IMSTR_START_END(title));
  if (enabled)
  {
    // bit contradictory - we don't want this button to be used for *gamepad* navigation, since they're usually
    // activated with the bumpers and/or the back button.
    if (!ImGui::ItemAdd(bb, id, nullptr, ImGuiItemFlags_NoNav | ImGuiItemFlags_NoNavDefaultFocus))
      return false;
  }
  else
  {
    if (ImGui::IsClippedEx(bb, id))
      return false;
  }

  bool held;
  bool pressed;
  bool hovered;
  if (enabled)
  {
    pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_NoNavFocus);
    if (hovered || is_active)
    {
      // Intentionally no animation here.
      ImU32 col;
      if (is_active)
      {
        if (hovered && !held)
          col = ImGui::GetColorU32(DarkerColor(UIStyle.SecondaryColor, 1.2f));
        else
          col = ImGui::GetColorU32(UIStyle.SecondaryColor);
      }
      else
      {
        col = ImGui::GetColorU32((hovered && !held) ? ImGuiCol_ButtonHovered : ImGuiCol_ButtonActive, 1.0f);
      }

      ImGui::RenderFrame(bb.Min, bb.Max, col, false, LayoutScale(LAYOUT_MENU_ITEM_BORDER_ROUNDING));
    }
  }
  else
  {
    pressed = false;
    held = false;
    hovered = false;
  }

  bb.Min += style.FramePadding;
  bb.Max -= style.FramePadding;

  if (image)
  {
    const ImVec2& image_min = bb.Min;
    const ImVec2 image_max = image_min + ImVec2(image_size, image_size);
    ImGui::GetWindowDrawList()->AddImage(image, image_min, image_max, uv0, uv1,
                                         enabled ? IM_COL32_WHITE : ImGui::GetColorU32(ImGuiCol_TextDisabled));
    bb.Min.x = image_max.x + text_space_size;
  }

  RenderShadowedTextClipped(UIStyle.Font, font_size, font_weight, bb.Min, bb.Max,
                            enabled ?
                              ImGui::GetColorU32(is_active ? UIStyle.SecondaryTextColor : UIStyle.PrimaryTextColor) :
                              ImGui::GetColorU32(ImGuiCol_TextDisabled),
                            title, &text_size, ImVec2(0.0f, 0.0f), 0.0f, &bb);

  return pressed;
}

bool FullscreenUI::BeginHorizontalMenu(const char* name, const ImVec2& position, const ImVec2& size,
                                       const ImVec4& bg_color, u32 num_items)
{
  const float item_padding = LayoutScale(LAYOUT_HORIZONTAL_MENU_PADDING);
  const float item_width = LayoutScale(LAYOUT_HORIZONTAL_MENU_ITEM_WIDTH);
  const float item_spacing = LayoutScale(40.0f);
  const float menu_width = static_cast<float>(num_items) * (item_width + item_spacing) - item_spacing;
  const float menu_height = LayoutScale(LAYOUT_HORIZONTAL_MENU_HEIGHT);

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(item_padding, item_padding));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, LayoutScale(UIStyle.MenuBorders ? 1.0f : 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(item_spacing, 0.0f));

  if (!BeginFullscreenWindow(position, size, name, bg_color, 0.0f, ImVec2()))
    return false;

  ImGui::SetCursorPos(ImFloor(ImVec2((size.x - menu_width) * 0.5f, (size.y - menu_height) * 0.5f)));

  BeginMenuButtonDrawSplit();
  return true;
}

void FullscreenUI::EndHorizontalMenu()
{
  PostDrawMenuButtonFrame();
  EndMenuButtonDrawSplit();

  ImGui::PopStyleVar(4);
  EndFullscreenWindow(true, true);
}

bool FullscreenUI::HorizontalMenuItem(GPUTexture* icon, std::string_view title, std::string_view description, u32 color)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;

  const ImVec2 pos = window->DC.CursorPos;
  const ImVec2 size = LayoutScale(LAYOUT_HORIZONTAL_MENU_ITEM_WIDTH, LAYOUT_HORIZONTAL_MENU_HEIGHT);
  ImRect bb = ImRect(pos, pos + size);

  const ImGuiID id = window->GetID(IMSTR_START_END(title));
  ImGui::ItemSize(size);
  if (!ImGui::ItemAdd(bb, id))
    return false;

  bool held;
  bool hovered;
  const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, 0);
  if (hovered)
    DrawMenuButtonFrame(bb.Min, bb.Max, held);

  const ImGuiStyle& style = ImGui::GetStyle();
  bb.Min += style.FramePadding;
  bb.Max -= style.FramePadding;

  const float avail_width = bb.Max.x - bb.Min.x;
  const float icon_size = LayoutScale(LAYOUT_HORIZONTAL_MENU_ITEM_IMAGE_SIZE);
  const ImVec2 icon_pos = bb.Min + ImVec2((avail_width - icon_size) * 0.5f, 0.0f);
  const ImRect icon_box = CenterImage(ImRect(icon_pos, icon_pos + ImVec2(icon_size, icon_size)), icon);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddImage(reinterpret_cast<ImTextureID>(icon), icon_box.Min, icon_box.Max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
               color);

  ImFont* const title_font = UIStyle.Font;
  const float title_font_size = UIStyle.LargeFontSize;
  const float title_font_weight = UIStyle.BoldFontWeight;
  const ImVec2 title_size = title_font->CalcTextSizeA(
    title_font_size, title_font_weight, std::numeric_limits<float>::max(), avail_width, IMSTR_START_END(title));
  const ImVec2 title_pos =
    ImVec2(bb.Min.x + (avail_width - title_size.x) * 0.5f, icon_pos.y + icon_size + LayoutScale(10.0f));
  const ImRect title_bb = ImRect(title_pos, title_pos + title_size);

  RenderShadowedTextClipped(title_font, title_font_size, title_font_weight, title_bb.Min, title_bb.Max,
                            ImGui::GetColorU32(ImGui::GetStyle().Colors[ImGuiCol_Text]), title, &title_size,
                            ImVec2(0.0f, 0.0f), avail_width, &title_bb);

  if (!description.empty())
  {
    ImFont* const desc_font = UIStyle.Font;
    const float desc_font_size = UIStyle.MediumFontSize;
    const float desc_font_weight = UIStyle.NormalFontWeight;
    const ImVec2 desc_size = desc_font->CalcTextSizeA(
      desc_font_size, desc_font_weight, std::numeric_limits<float>::max(), avail_width, IMSTR_START_END(description));
    const ImVec2 desc_pos = ImVec2(bb.Min.x + (avail_width - desc_size.x) * 0.5f, title_bb.Max.y + LayoutScale(10.0f));
    const ImRect desc_bb = ImRect(desc_pos, desc_pos + desc_size);

    RenderShadowedTextClipped(desc_font, desc_font_size, desc_font_weight, desc_bb.Min, desc_bb.Max,
                              ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text])), description,
                              nullptr, ImVec2(0.0f, 0.0f), avail_width, &desc_bb);
  }

  ImGui::SameLine();

  return pressed;
}

bool FullscreenUI::BeginSplitWindow(const ImVec2& position, const ImVec2& size, const char* name,
                                    const ImVec4& background /*= UIStyle.BackgroundColor*/, float rounding /*= 0.0f*/,
                                    const ImVec2& padding /*= ImVec2()*/, ImGuiWindowFlags flags /*= 0*/)
{
  const bool ret = BeginFullscreenWindow(position, size, name, background, rounding, padding, flags);
  BeginInnerSplitWindow();
  return ret;
}

void FullscreenUI::EndSplitWindow()
{
  EndInnerSplitWindow();
  EndFullscreenWindow();
}

void FullscreenUI::BeginInnerSplitWindow()
{
  ImGuiWindow* const window = ImGui::GetCurrentWindow();
  window->DC.LayoutType = ImGuiLayoutType_Horizontal;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);

  if (!ImGui::IsPopupOpen(0u, ImGuiPopupFlags_AnyPopup))
  {
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
    {
      s_state.split_window_focus_change = SplitWindowFocusChange::FocusSidebar;
      QueueResetFocus(FocusResetType::SplitWindowChanged);
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
    {
      s_state.split_window_focus_change = SplitWindowFocusChange::FocusContent;
      QueueResetFocus(FocusResetType::SplitWindowChanged);
    }
  }
}

void FullscreenUI::EndInnerSplitWindow()
{
  ImGui::PopStyleVar(4);
}

bool FullscreenUI::BeginSplitWindowSidebar(float sidebar_width /*= 0.2f*/)
{
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      LayoutScale(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, LayoutScale(LAYOUT_ITEM_X_SPACING, LAYOUT_ITEM_Y_SPACING));
  ImGui::PushStyleColor(ImGuiCol_ChildBg,
                        ModAlpha(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_WindowBg], 1.75f), 0.25f));

  if (IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  const float sidebar_width_px = ImFloor(ImGui::GetCurrentWindowRead()->WorkRect.GetWidth() * sidebar_width);
  if (!ImGui::BeginChild("sidebar", ImVec2(sidebar_width_px, 0.0f),
                         ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NoNavCancel))
    return false;

  if (ImGui::IsWindowFocused())
  {
    // cancel any pending focus change
    if (s_state.split_window_focus_change == SplitWindowFocusChange::FocusSidebar)
    {
      // don't eat the right keypress
      s_state.split_window_focus_change = SplitWindowFocusChange::None;
      CancelResetFocus();
    }
  }

  BeginMenuButtons();

  // todo: do we need to move this down if our first item is a heading?
  if (s_state.split_window_focus_change == SplitWindowFocusChange::FocusSidebar)
    ResetFocusHere();
  if (ImGui::IsWindowFocused() && GImGui->NavId == 0)
    ImGui::NavInitWindow(ImGui::GetCurrentWindow(), true);

  return true;
}

void FullscreenUI::EndSplitWindowSidebar()
{
  EndMenuButtons();

  ImGui::GetWindowDrawList()->ChannelsMerge();
  ImGui::PopStyleColor(1);
  ImGui::PopStyleVar(2);

  SetWindowNavWrapping(false, true);

  ImGui::EndChild();
}

bool FullscreenUI::SplitWindowSidebarItem(std::string_view title, bool active /*= false*/, bool enabled /*= true*/)
{
  if (active)
  {
    SetMenuButtonSplitLayer(MENU_BUTTON_SPLIT_LAYER_BACKGROUND);

    const MenuButtonBounds bb(title, std::string_view(), std::string_view());
    ImGui::GetWindowDrawList()->AddRectFilled(bb.frame_bb.Min, bb.frame_bb.Max,
                                              ImGui::GetColorU32(DarkerColor(UIStyle.BackgroundColor, 0.6f)),
                                              LayoutScale(LAYOUT_MENU_ITEM_BORDER_ROUNDING));

    SetMenuButtonSplitLayer(MENU_BUTTON_SPLIT_LAYER_FOREGROUND);
  }

  return MenuButtonWithoutSummary(title, enabled);
}

void FullscreenUI::SplitWindowSidebarSeparator()
{
  const float line_width = MenuButtonBounds::CalcAvailWidth();

  const float line_thickness = LayoutScale(1.0f);
  const float line_padding = LayoutScale(2.0f);

  const ImVec2 line_start =
    ImGui::GetCurrentWindowRead()->DC.CursorPos + ImVec2(ImGui::GetStyle().FramePadding.x, line_padding);
  const ImVec2 line_end = ImVec2(line_start.x + line_width, line_start.y);
  const u32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);

  ImGui::GetWindowDrawList()->AddLine(line_start, line_end, color, line_thickness);

  ImGui::Dummy(ImVec2(0.0f, line_padding * 2.0f + line_thickness));
}

bool FullscreenUI::SplitWindowSidebarItem(std::string_view title, std::string_view summary, bool active /*= false*/,
                                          bool enabled /*= true*/)
{
  if (active)
  {
    SetMenuButtonSplitLayer(MENU_BUTTON_SPLIT_LAYER_BACKGROUND);

    const MenuButtonBounds bb(title, std::string_view(), summary);
    ImGui::GetWindowDrawList()->AddRectFilled(
      bb.frame_bb.Min, bb.frame_bb.Max,
      ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_WindowBg], 0.6f)),
      LayoutScale(LAYOUT_MENU_ITEM_BORDER_ROUNDING));

    SetMenuButtonSplitLayer(MENU_BUTTON_SPLIT_LAYER_FOREGROUND);
  }

  return MenuButton(title, summary, enabled);
}

bool FullscreenUI::BeginSplitWindowContent(bool background)
{
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(LAYOUT_MENU_WINDOW_X_PADDING, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, LayoutScale(LAYOUT_ITEM_X_SPACING, LAYOUT_ITEM_Y_SPACING));
  ImGui::PushStyleColor(ImGuiCol_ChildBg,
                        ModAlpha(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_WindowBg], 1.5f), 0.25f));

  if (IsFocusResetFromWindowChange() || s_state.split_window_focus_change == SplitWindowFocusChange::FocusContent)
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  ImGuiWindow* const window = ImGui::GetCurrentWindow();
  const float content_width = window->WorkRect.Max.x - window->DC.CursorPos.x;

  const bool ret = ImGui::BeginChild("content", ImVec2(content_width, 0.0f),
                                     ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NoNavCancel,
                                     background ? 0 : ImGuiWindowFlags_NoBackground);

  if (ImGui::IsWindowFocused())
  {
    // cancel any pending focus change
    if (s_state.split_window_focus_change == SplitWindowFocusChange::FocusContent)
    {
      // don't eat the right keypress
      s_state.split_window_focus_change = SplitWindowFocusChange::None;
      CancelResetFocus();
    }
  }
  else
  {
    if (s_state.split_window_focus_change == SplitWindowFocusChange::FocusContent)
      ResetFocusHere();
  }

  return ret;
}

void FullscreenUI::ResetSplitWindowContentFocusHere()
{
  if (s_state.split_window_focus_change == SplitWindowFocusChange::FocusSidebar ||
      (s_state.split_window_focus_change == SplitWindowFocusChange::None && IsFocusResetQueued()))
  {
    ResetFocusHere();
  }

  if (ImGui::IsWindowFocused() && GImGui->NavId == 0)
    ImGui::NavInitWindow(ImGui::GetCurrentWindow(), true);
}

void FullscreenUI::EndSplitWindowContent()
{
  ImGui::PopStyleColor(1);
  ImGui::PopStyleVar(2);

  SetWindowNavWrapping(false, true);

  ImGui::EndChild();
}

bool FullscreenUI::WasSplitWindowChanged()
{
  return (s_state.split_window_focus_change != SplitWindowFocusChange::None);
}

void FullscreenUI::FocusSplitWindowContent()
{
  s_state.split_window_focus_change = SplitWindowFocusChange::FocusContent;
  QueueResetFocus(FocusResetType::SplitContentChanged);
}

bool FullscreenUI::SplitWindowIsNavWindow()
{
  const ImGuiWindow* const nav_window = GImGui->NavWindow;
  const ImGuiWindow* const current_window = ImGui::GetCurrentWindowRead();
  return (nav_window && (nav_window == current_window || nav_window->ParentWindow == current_window));
}

FullscreenUI::PopupDialog::PopupDialog() = default;

FullscreenUI::PopupDialog::~PopupDialog() = default;

void FullscreenUI::PopupDialog::StartClose()
{
  if (!IsOpen() || m_state == State::Closing || m_state == State::ClosingTrigger)
    return;

  m_state = State::Closing;
  m_animation_time_remaining = UIStyle.Animations ? CLOSE_TIME : 0.0f;
}

void FullscreenUI::PopupDialog::ClearState()
{
  m_state = State::Inactive;
  m_title = {};
}

void FullscreenUI::PopupDialog::SetTitleAndOpen(std::string title)
{
  DebugAssert(!title.empty());
  m_title = std::move(title);
  m_animation_time_remaining = UIStyle.Animations ? OPEN_TIME : 0.0f;

  if (m_state == State::Inactive)
  {
    // inactive -> active
    m_state = State::OpeningTrigger;
  }
  else
  {
    // we need to close under the old name, and reopen under the new
    m_state = State::Reopening;
  }
}

void FullscreenUI::PopupDialog::CloseImmediately()
{
  if (!IsOpen())
    return;

  if (!GImGui->BeginPopupStack.empty() && GImGui->BeginPopupStack.front().Window == ImGui::GetCurrentWindowRead())
  {
    ImGui::CloseCurrentPopup();
    ClearState();
    QueueResetFocus(FocusResetType::PopupClosed);
  }
  else
  {
    // have  to defer it
    m_state = State::ClosingTrigger;
  }
}

bool FullscreenUI::PopupDialog::BeginRender(float scaled_window_padding /* = LayoutScale(20.0f) */,
                                            float scaled_window_rounding /* = LayoutScale(20.0f) */,
                                            const ImVec2& scaled_window_size /* = ImVec2(0.0f, 0.0f) */)
{
  DebugAssert(IsOpen());

  // reopening is messy...
  if (m_state == State::Reopening) [[unlikely]]
  {
    // close it under the old name
    if (ImGui::IsPopupOpen(ImGui::GetCurrentWindowRead()->GetID(IMSTR_START_END(m_title)), ImGuiPopupFlags_None))
      ImGui::ClosePopupToLevel(GImGui->OpenPopupStack.Size, true);

    // and open under the new name
    m_state = State::OpeningTrigger;
  }

  if (m_state == State::OpeningTrigger)
  {
    // need to have the openpopup at the correct level
    m_state = State::Opening;
    ImGui::OpenPopup(m_title.c_str());
    QueueResetFocus(FocusResetType::PopupOpened);

    // hackity hack to disable imgui's background fade animation
    GImGui->DimBgRatio = UIStyle.Animations ? GImGui->DimBgRatio : 1.0f;
  }

  // check for animation completion
  ImVec2 pos_offset = ImVec2(0.0f, 0.0f);
  float alpha = 1.0f;
  if (m_state >= State::OpeningTrigger)
  {
    m_animation_time_remaining -= ImGui::GetIO().DeltaTime;
    if (m_animation_time_remaining <= 0.0f)
    {
      m_animation_time_remaining = 0.0f;
      if (m_state == State::Opening)
      {
        m_state = State::Open;
      }
      else
      {
        m_state = State::ClosingTrigger;
        alpha = 0.0f;
      }
    }
    else
    {
      // inhibit menu animation while opening, otherwise it jitters
      ResetMenuButtonFrame();

      if (m_state == State::Opening)
      {
        const float fract = m_animation_time_remaining / OPEN_TIME;
        alpha = 1.0f - fract;
        pos_offset.y = LayoutScale(50.0f) * Easing::InExpo(fract);
      }
      else
      {
        const float fract = m_animation_time_remaining / CLOSE_TIME;
        alpha = fract;
        pos_offset.y = LayoutScale(20.0f) * (1.0f - fract);
      }
    }
  }

  ImGui::PushFont(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, scaled_window_rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(scaled_window_padding, scaled_window_padding));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ModAlpha(UIStyle.PopupBackgroundColor, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ModAlpha(DarkerColor(UIStyle.PopupBackgroundColor, 1.8f), 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ModAlpha(DarkerColor(UIStyle.PopupBackgroundColor, 1.3f), 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, UIStyle.PopupFrameBackgroundColor);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, UIStyle.PrimaryDarkColor);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, UIStyle.PrimaryColor);
  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.PrimaryTextColor);

  ImGui::SetNextWindowPos((ImGui::GetIO().DisplaySize - LayoutScale(0.0f, LAYOUT_FOOTER_HEIGHT)) * 0.5f + pos_offset,
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(scaled_window_size);

  // Based on BeginPopupModal(), because we need to control is_open smooth closing.
  const bool popup_open =
    ImGui::IsPopupOpen(ImGui::GetCurrentWindowRead()->GetID(IMSTR_START_END(m_title)), ImGuiPopupFlags_None);
  const ImGuiWindowFlags window_flags = ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        (m_title.starts_with("##") ? ImGuiWindowFlags_NoTitleBar : 0);
  bool is_open = true;
  if (popup_open && !ImGui::Begin(m_title.c_str(), m_user_closeable ? &is_open : nullptr, window_flags))
    is_open = false;

  if (popup_open && !is_open && m_state != State::ClosingTrigger)
  {
    StartClose();
  }
  else if (!popup_open || m_state == State::ClosingTrigger)
  {
    if (popup_open)
    {
      // hackity hack to disable imgui's background fade animation
      GImGui->DimBgRatio = UIStyle.Animations ? GImGui->DimBgRatio : 0.0f;
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(6);
    ImGui::PopFont();
    QueueResetFocus(FocusResetType::PopupClosed);
    return false;
  }

  // don't draw unreadable text
  ImGui::PopStyleColor(1);
  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.BackgroundTextColor);
  ImGui::PopFont();
  ImGui::PushFont(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.NormalFontWeight);

  if (WantsToCloseMenu())
    StartClose();

  return true;
}

void FullscreenUI::PopupDialog::EndRender()
{
  ImGui::EndPopup();
  ImGui::PopStyleColor(7);
  ImGui::PopStyleVar(6);
  ImGui::PopFont();
}

FullscreenUI::FileSelectorDialog::FileSelectorDialog() = default;

FullscreenUI::FileSelectorDialog::~FileSelectorDialog() = default;

void FullscreenUI::FileSelectorDialog::Open(std::string_view title, FileSelectorCallback callback,
                                            FileSelectorFilters filters, std::string initial_directory,
                                            bool select_directory)
{
  if (initial_directory.empty() || !FileSystem::DirectoryExists(initial_directory.c_str()))
    initial_directory = FileSystem::GetWorkingDirectory();

  SetTitleAndOpen(fmt::format("{}##file_selector_dialog", title));
  m_callback = std::move(callback);
  m_filters = std::move(filters);
  m_is_directory = select_directory;
  SetDirectory(std::move(initial_directory));
}

void FullscreenUI::FileSelectorDialog::ClearState()
{
  PopupDialog::ClearState();
  m_callback = {};
  m_filters = {};
  m_is_directory = false;
  m_directory_changed = false;
}

FullscreenUI::FileSelectorDialog::Item::Item(std::string display_name_, std::string full_path_, bool is_file_)
  : display_name(std::move(display_name_)), full_path(std::move(full_path_)), is_file(is_file_)
{
}

void FullscreenUI::FileSelectorDialog::PopulateItems()
{
  m_items.clear();
  m_first_item_is_parent_directory = false;

  if (m_current_directory.empty())
  {
    for (std::string& root_path : FileSystem::GetRootDirectoryList())
    {
      std::string label = fmt::format(ICON_EMOJI_FILE_FOLDER " {}", root_path);
      m_items.emplace_back(std::move(label), std::move(root_path), false);
    }
  }
  else
  {
    FileSystem::FindResultsArray results;
    FileSystem::FindFiles(m_current_directory.c_str(), "*",
                          FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES |
                            FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_SORT_BY_NAME,
                          &results);

    // Ensure we only go back to the root list once we've gone up from the root of that drive.
    std::string parent_path;
    std::string::size_type sep_pos = m_current_directory.rfind(FS_OSPATH_SEPARATOR_CHARACTER);
    if (sep_pos != std::string::npos && sep_pos != (m_current_directory.size() - 1))
    {
      parent_path = Path::Canonicalize(m_current_directory.substr(0, sep_pos));

      // Ensure that the root directory has a trailing backslash.
      if (parent_path.find(FS_OSPATH_SEPARATOR_CHARACTER) == std::string::npos)
        parent_path.push_back(FS_OSPATH_SEPARATOR_CHARACTER);
    }

    m_items.emplace_back(fmt::format(ICON_EMOJI_FILE_FOLDER_OPEN " {}", FSUI_VSTR("<Parent Directory>")),
                         std::move(parent_path), false);
    m_first_item_is_parent_directory = true;

    for (const FILESYSTEM_FIND_DATA& fd : results)
    {
      std::string full_path = Path::Combine(m_current_directory, fd.FileName);

      if (fd.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
      {
        std::string title = fmt::format(ICON_EMOJI_FILE_FOLDER " {}", fd.FileName);
        m_items.emplace_back(std::move(title), std::move(full_path), false);
      }
      else
      {
        if (m_filters.empty() || std::none_of(m_filters.begin(), m_filters.end(), [&fd](const std::string& filter) {
              return StringUtil::WildcardMatch(fd.FileName.c_str(), filter.c_str(), false);
            }))
        {
          continue;
        }

        std::string title = fmt::format(ICON_EMOJI_PAGE_FACING_UP " {}", fd.FileName);
        m_items.emplace_back(std::move(title), std::move(full_path), true);
      }
    }
  }
}

void FullscreenUI::FileSelectorDialog::SetDirectory(std::string dir)
{
  // Ensure at least one slash always exists.
  while (!dir.empty() && dir.back() == FS_OSPATH_SEPARATOR_CHARACTER &&
         dir.find(FS_OSPATH_SEPARATOR_CHARACTER) != (dir.size() - 1))
  {
    dir.pop_back();
  }

  m_current_directory = std::move(dir);
  m_directory_changed = true;
  PopulateItems();
}

void FullscreenUI::FileSelectorDialog::Draw()
{
  if (!IsOpen())
    return;

  if (!BeginRender(LayoutScale(10.0f), LayoutScale(20.0f), LayoutScale(1000.0f, 650.0f)))
  {
    const FileSelectorCallback callback = std::move(m_callback);
    ClearState();
    if (callback)
      callback(std::string());
    return;
  }

  if (m_directory_changed)
  {
    m_directory_changed = false;
    ImGui::SetScrollY(0.0f);
    QueueResetFocus(FocusResetType::Other);
  }

  ResetFocusHere();
  BeginMenuButtons();

  Item* selected = nullptr;
  bool directory_selected = false;

  if (!m_current_directory.empty())
    MenuButtonWithoutSummary(SmallString::from_format(ICON_FA_FOLDER_OPEN " {}", m_current_directory), false);

  if (m_is_directory && !m_current_directory.empty())
  {
    if (MenuButtonWithoutSummary(
          SmallString::from_format(ICON_EMOJI_FILE_FOLDER_OPEN " {}", FSUI_VSTR("<Use This Directory>"))))
    {
      directory_selected = true;
    }
  }

  for (Item& item : m_items)
  {
    if (MenuButtonWithoutSummary(item.display_name))
      selected = &item;
  }

  EndMenuButtons();

  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, FSUI_VSTR("Change Selection")),
                                       std::make_pair(ICON_PF_BUTTON_Y, FSUI_VSTR("Parent Directory")),
                                       std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Cancel"))});
  }
  else
  {
    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, FSUI_VSTR("Change Selection")),
                 std::make_pair(ICON_PF_BACKSPACE, FSUI_VSTR("Parent Directory")),
                 std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")), std::make_pair(ICON_PF_ESC, FSUI_VSTR("Cancel"))});
  }

  EndRender();

  if (selected)
  {
    if (selected->is_file)
    {
      std::string path = std::exchange(selected->full_path, std::string());
      const FileSelectorCallback callback = std::exchange(m_callback, FileSelectorCallback());
      StartClose();
      callback(std::move(path));
    }
    else
    {
      SetDirectory(std::move(selected->full_path));
    }
  }
  else if (directory_selected)
  {
    std::string path = std::exchange(m_current_directory, std::string());
    const FileSelectorCallback callback = std::exchange(m_callback, FileSelectorCallback());
    StartClose();
    callback(std::move(path));
  }
  else
  {
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ImGui::IsKeyPressed(ImGuiKey_NavGamepadInput, false))
    {
      if (!m_items.empty() && m_first_item_is_parent_directory)
        SetDirectory(std::move(m_items.front().full_path));
    }
  }
}

bool FullscreenUI::IsFileSelectorOpen()
{
  return s_state.file_selector_dialog.IsOpen();
}

void FullscreenUI::OpenFileSelector(std::string_view title, bool select_directory, FileSelectorCallback callback,
                                    FileSelectorFilters filters, std::string initial_directory)
{
  s_state.file_selector_dialog.Open(title, std::move(callback), std::move(filters), std::move(initial_directory),
                                    select_directory);
}

void FullscreenUI::CloseFileSelector()
{
  s_state.file_selector_dialog.StartClose();
}

FullscreenUI::ChoiceDialog::ChoiceDialog() = default;

FullscreenUI::ChoiceDialog::~ChoiceDialog() = default;

void FullscreenUI::ChoiceDialog::Open(std::string_view title, ChoiceDialogOptions options,
                                      ChoiceDialogCallback callback, bool checkable)
{
  SetTitleAndOpen(fmt::format("{}##choice_dialog", title));
  m_options = std::move(options);
  m_callback = std::move(callback);
  m_checkable = checkable;
}

void FullscreenUI::ChoiceDialog::ClearState()
{
  PopupDialog::ClearState();
  m_options = {};
  m_callback = {};
  m_checkable = false;
}

void FullscreenUI::ChoiceDialog::Draw()
{
  if (!IsOpen())
    return;

  const float width = LayoutScale(600.0f);
  const float window_y_padding = LayoutScale(10.0f);
  const float title_height = UIStyle.LargeFontSize + (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f);

  // assume single item line height for precalculation
  const float item_spacing = LayoutScale(LAYOUT_MENU_BUTTON_SPACING);
  const float item_height = MenuButtonBounds::GetSingleLineHeight() + item_spacing;
  const float height = title_height +
                       ((item_height * static_cast<float>(std::min<size_t>(9, m_options.size()))) - item_spacing) +
                       (window_y_padding * 2.0f);

  if (!BeginRender(window_y_padding, LayoutScale(20.0f), ImVec2(width, height)))
  {
    const ChoiceDialogCallback callback = std::move(m_callback);
    ClearState();
    if (callback)
      callback(-1, std::string(), false);
    return;
  }

  s32 choice = -1;

  if (m_checkable)
  {
    BeginMenuButtons();
    ResetFocusHere();

    for (s32 i = 0; i < static_cast<s32>(m_options.size()); i++)
    {
      auto& option = m_options[i];

      const SmallString title =
        SmallString::from_format("{0} {1}", option.second ? ICON_FA_SQUARE_CHECK : ICON_FA_SQUARE, option.first);
      bool visible;
      if (MenuButtonWithVisibilityQuery(TinyString::from_format("item{}", i), title, {}, {}, &visible))
      {
        choice = i;
        option.second = !option.second;
      }
    }

    EndMenuButtons();
  }
  else
  {
    // frame padding is needed for MenuButtonBounds()
    BeginMenuButtons(0, 0.0f, LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING, 0.0f,
                     LAYOUT_MENU_BUTTON_SPACING);

    ResetFocusHere();

    const bool appearing = ImGui::IsWindowAppearing();

    for (s32 i = 0; i < static_cast<s32>(m_options.size()); i++)
    {
      auto& option = m_options[i];

      if (option.second)
      {
        SetMenuButtonSplitLayer(MENU_BUTTON_SPLIT_LAYER_BACKGROUND);

        const MenuButtonBounds bb(option.first, ImVec2(), {});
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::RenderFrame(pos, pos + bb.frame_bb.GetSize(),
                           ImGui::GetColorU32(DarkerColor(UIStyle.PopupBackgroundColor, 0.6f)), false,
                           LayoutScale(LAYOUT_MENU_ITEM_BORDER_ROUNDING));

        SetMenuButtonSplitLayer(MENU_BUTTON_SPLIT_LAYER_FOREGROUND);
      }

      bool visible;
      if (MenuButtonWithVisibilityQuery(TinyString::from_format("item{}", i), option.first, {},
                                        option.second ? ICON_FA_CHECK ""sv : std::string_view(), &visible))
      {
        choice = i;
        for (s32 j = 0; j < static_cast<s32>(m_options.size()); j++)
          m_options[j].second = (j == i);
      }

      if (option.second && appearing)
      {
        ImGui::SetItemDefaultFocus();
        ImGui::SetScrollHereY(0.5f);
      }
    }

    EndMenuButtons();
  }

  SetStandardSelectionFooterText(false);

  EndRender();

  if (choice >= 0)
  {
    // immediately close dialog when selecting, save the callback doing it. have to take a copy in this instance,
    // because the callback may open another dialog, and we don't want to close that one.
    if (!m_checkable)
    {
      const auto selected = m_options[choice];
      const ChoiceDialogCallback callback = std::exchange(m_callback, ChoiceDialogCallback());
      StartClose();
      callback(choice, selected.first, selected.second);
    }
    else
    {
      const auto& option = m_options[choice];
      m_callback(choice, option.first, option.second);
    }
  }
}

bool FullscreenUI::IsChoiceDialogOpen()
{
  return s_state.choice_dialog.IsOpen();
}

void FullscreenUI::OpenChoiceDialog(std::string_view title, bool checkable, ChoiceDialogOptions options,
                                    ChoiceDialogCallback callback)
{
  s_state.choice_dialog.Open(title, std::move(options), std::move(callback), checkable);
}

void FullscreenUI::CloseChoiceDialog()
{
  s_state.choice_dialog.StartClose();
}

bool FullscreenUI::IsInputDialogOpen()
{
  return s_state.input_string_dialog.IsOpen();
}

void FullscreenUI::OpenInputStringDialog(std::string_view title, std::string message, std::string caption,
                                         std::string ok_button_text, InputStringDialogCallback callback)
{
  s_state.input_string_dialog.Open(title, std::move(message), std::move(caption), std::move(ok_button_text),
                                   std::move(callback));
  QueueResetFocus(FocusResetType::PopupOpened);
}

FullscreenUI::InputStringDialog::InputStringDialog() = default;

FullscreenUI::InputStringDialog::~InputStringDialog() = default;

void FullscreenUI::InputStringDialog::Open(std::string_view title, std::string message, std::string caption,
                                           std::string ok_button_text, InputStringDialogCallback callback)
{
  SetTitleAndOpen(fmt::format("{}##input_string_dialog", title));
  m_message = std::move(message);
  m_caption = std::move(caption);
  m_ok_text = std::move(ok_button_text);
  m_callback = std::move(callback);
}

void FullscreenUI::InputStringDialog::ClearState()
{
  PopupDialog::ClearState();
  m_message = {};
  m_caption = {};
  m_ok_text = {};
  m_callback = {};
}

void FullscreenUI::InputStringDialog::Draw()
{
  if (!IsOpen())
    return;

  if (!BeginRender(LayoutScale(20.0f), LayoutScale(20.0f), LayoutScale(700.0f, 0.0f)))
  {
    InputStringDialogCallback cb = std::move(m_callback);
    ClearState();
    if (cb)
      cb(std::string());
    return;
  }

  ResetFocusHere();
  ImGui::TextWrapped("%s", m_message.c_str());

  BeginMenuButtons();

  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));

  if (!m_caption.empty())
  {
    const float prev = ImGui::GetCursorPosX();
    ImGui::TextUnformatted(IMSTR_START_END(m_caption));
    ImGui::SetNextItemWidth(ImGui::GetCursorPosX() - prev);
  }
  else
  {
    ImGui::SetNextItemWidth(ImGui::GetCurrentWindow()->WorkRect.GetWidth());
  }
  ImGui::InputText("##input", &m_text);

  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));

  const bool ok_enabled = !m_text.empty();

  if (MenuButtonWithoutSummary(m_ok_text, ok_enabled) && ok_enabled)
  {
    // have to move out in case they open another dialog in the callback
    const InputStringDialogCallback cb = std::exchange(m_callback, InputStringDialogCallback());
    std::string text = std::exchange(m_text, std::string());
    StartClose();
    cb(std::move(text));
  }

  if (MenuButtonWithoutSummary(ICON_FA_XMARK " Cancel"))
    StartClose();

  EndMenuButtons();

  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_KEYBOARD, FSUI_VSTR("Enter Value")),
                                       std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Cancel"))});
  }
  else
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_KEYBOARD, FSUI_VSTR("Enter Value")),
                                       std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Select")),
                                       std::make_pair(ICON_PF_ESC, FSUI_VSTR("Cancel"))});
  }

  EndRender();
}

void FullscreenUI::CloseInputDialog()
{
  s_state.input_string_dialog.StartClose();
}

FullscreenUI::MessageDialog::MessageDialog() = default;

FullscreenUI::MessageDialog::~MessageDialog() = default;

void FullscreenUI::MessageDialog::Open(std::string_view icon, std::string_view title, std::string message,
                                       CallbackVariant callback, std::string first_button_text,
                                       std::string second_button_text, std::string third_button_text)
{
  SetTitleAndOpen(fmt::format("{}##message_dialog", title));
  m_icon = icon;
  m_message = std::move(message);
  m_callback = std::move(callback);
  m_buttons[0] = std::move(first_button_text);
  m_buttons[1] = std::move(second_button_text);
  m_buttons[2] = std::move(third_button_text);
}

void FullscreenUI::MessageDialog::ClearState()
{
  PopupDialog::ClearState();
  m_message = {};
  m_buttons = {};
  m_callback = {};
}

void FullscreenUI::MessageDialog::Draw()
{
  if (!IsOpen())
    return;

  if (!BeginRender(LayoutScale(20.0f), LayoutScale(20.0f), LayoutScale(700.0f, 0.0f)))
  {
    CallbackVariant cb = std::move(m_callback);
    ClearState();
    InvokeCallback(cb, std::nullopt);
    return;
  }

  std::optional<s32> result;

  if (!m_icon.empty())
  {
    ImGui::PushFont(nullptr, LayoutScale(50.0f), 0.0f);
    ImGui::TextUnformatted(IMSTR_START_END(m_icon));
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + LayoutScale(10.0f));
  }

  ImGui::TextWrapped("%s", m_message.c_str());
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(25.0f));

  ResetFocusHere();

  // use vertical buttons for long text
  if (!m_buttons[0].empty() && StringUtil::GetUTF8CharacterCount(m_buttons[0]) >= 15)
  {
    BeginMenuButtons();

    for (s32 button_index = 0; button_index < static_cast<s32>(m_buttons.size()); button_index++)
    {
      if (!m_buttons[button_index].empty() &&
          MenuButtonWithoutSummary(m_buttons[button_index], true, LAYOUT_CENTER_ALIGN_TEXT))
      {
        result = button_index;
      }
    }
    EndMenuButtons();
  }
  else
  {
    BeginHorizontalMenuButtons(
      static_cast<u32>(std::ranges::count_if(m_buttons, [](const std::string& str) { return !str.empty(); })), 200.0f);

    for (s32 button_index = 0; button_index < static_cast<s32>(m_buttons.size()); button_index++)
    {
      if (!m_buttons[button_index].empty() &&
          HorizontalMenuButton(m_buttons[button_index], true, LAYOUT_CENTER_ALIGN_TEXT))
      {
        result = button_index;
      }
    }

    EndHorizontalMenuButtons();
  }

  SetStandardSelectionFooterText(false);

  EndRender();

  if (result.has_value())
  {
    // have to move out in case they open another dialog in the callback
    StartClose();
    InvokeCallback(std::exchange(m_callback, CallbackVariant()), result);
  }
}

void FullscreenUI::MessageDialog::InvokeCallback(const CallbackVariant& cb, std::optional<s32> choice)
{
  if (std::holds_alternative<InfoMessageDialogCallback>(cb))
  {
    const InfoMessageDialogCallback& func = std::get<InfoMessageDialogCallback>(cb);
    if (func)
      func();
  }
  else if (std::holds_alternative<ConfirmMessageDialogCallback>(cb))
  {
    const ConfirmMessageDialogCallback& func = std::get<ConfirmMessageDialogCallback>(cb);
    if (func)
      func(choice.value_or(1) == 0);
  }
}

bool FullscreenUI::IsMessageBoxDialogOpen()
{
  return s_state.message_dialog.IsOpen();
}

void FullscreenUI::OpenConfirmMessageDialog(std::string_view icon, std::string_view title, std::string message,
                                            ConfirmMessageDialogCallback callback, std::string yes_button_text,
                                            std::string no_button_text)
{
  s_state.message_dialog.Open(icon, std::move(title), std::move(message), std::move(callback),
                              std::move(yes_button_text), std::move(no_button_text), std::string());
}

void FullscreenUI::OpenInfoMessageDialog(std::string_view icon, std::string_view title, std::string message,
                                         InfoMessageDialogCallback callback, std::string button_text)
{
  s_state.message_dialog.Open(icon, std::move(title), std::move(message), std::move(callback), std::move(button_text),
                              std::string(), std::string());
}

void FullscreenUI::OpenMessageDialog(std::string_view icon, std::string_view title, std::string message,
                                     MessageDialogCallback callback, std::string first_button_text,
                                     std::string second_button_text, std::string third_button_text)
{
  s_state.message_dialog.Open(icon, std::move(title), std::move(message), std::move(callback),
                              std::move(first_button_text), std::move(second_button_text),
                              std::move(third_button_text));
}

void FullscreenUI::CloseMessageDialog()
{
  s_state.message_dialog.StartClose();
}

FullscreenUI::ProgressDialog::ProgressDialog() = default;
FullscreenUI::ProgressDialog::~ProgressDialog() = default;

void FullscreenUI::ProgressDialog::Draw()
{
  if (!IsOpen())
    return;

  const float window_padding = LayoutScale(20.0f);

  if (!BeginRender(window_padding, window_padding, ImVec2(m_width, 0.0f)))
  {
    if (m_user_closeable)
      m_cancelled.store(true, std::memory_order_release);

    m_status_text = {};
    m_last_frac = 0.0f;
    ClearState();
    return;
  }

  const float spacing = LayoutScale(5.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, LayoutScale(6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, spacing));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, DarkerColor(UIStyle.PopupBackgroundColor));
  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.BackgroundTextColor);
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, UIStyle.SecondaryColor);
  ImGui::PushFont(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.NormalFontWeight);

  const bool has_progress = (m_progress_range > 0);
  float wrap_width = ImGui::GetContentRegionAvail().x;
  if (has_progress)
  {
    // reserve space for text
    TinyString text;
    text.format("{}/{}", m_progress_value, m_progress_range);

    const ImVec2 text_width = ImGui::CalcTextSize(IMSTR_START_END(text));
    const ImVec2 screen_pos = ImGui::GetCursorScreenPos();
    const ImVec2 text_pos = ImVec2(screen_pos.x + wrap_width - text_width.x, screen_pos.y);
    ImGui::GetWindowDrawList()->AddText(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, text_pos,
                                        ImGui::GetColorU32(ImGuiCol_Text), IMSTR_START_END(text));
    wrap_width -= text_width.x + spacing;
  }

  if (!m_status_text.empty())
    FullscreenUI::TextAlignedMultiLine(0.0f, IMSTR_START_END(m_status_text), wrap_width);

  const float bar_height = LayoutScale(20.0f);

  float frac;
  if (has_progress)
  {
    const float max_frac = (static_cast<float>(m_progress_value) / static_cast<float>(m_progress_range));
    const float dt = ImGui::GetIO().DeltaTime;
    frac = std::min(m_last_frac + dt, max_frac);
    m_last_frac = frac;
  }
  else
  {
    frac = static_cast<float>(-ImGui::GetTime());
  }
  ImGui::ProgressBar(frac, ImVec2(-1.0f, bar_height), "");

  ImGui::Dummy(ImVec2(0.0f, LayoutScale(5.0f)));

  ImGui::PopFont();
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);

  if (m_user_closeable)
  {
    BeginHorizontalMenuButtons(1, 150.0f);
    if (HorizontalMenuButton(FSUI_ICONSTR(ICON_FA_SQUARE_XMARK, "Cancel")))
      StartClose();
    EndHorizontalMenuButtons();
  }

  EndRender();
}

std::unique_ptr<ProgressCallbackWithPrompt>
FullscreenUI::ProgressDialog::GetProgressCallback(std::string title, float window_unscaled_width)
{
  if (m_state == PopupDialog::State::Open)
  {
    // return dummy callback so the op can still go through
    ERROR_LOG("Progress dialog is already open, cannot create dialog for '{}'.", std::move(title));
    return std::make_unique<ProgressCallbackWithPrompt>();
  }

  SetTitleAndOpen(std::move(title));
  m_width = LayoutScale(window_unscaled_width);
  m_last_frac = 0.0f;
  m_cancelled.store(false, std::memory_order_release);
  return std::make_unique<ProgressCallbackImpl>();
}

FullscreenUI::ProgressDialog::ProgressCallbackImpl::ProgressCallbackImpl() = default;

FullscreenUI::ProgressDialog::ProgressCallbackImpl::~ProgressCallbackImpl()
{
  static constexpr auto close_cb = []() {
    if (!s_state.progress_dialog.IsOpen())
      return;
    s_state.progress_dialog.StartClose();
  };
  if (VideoThread::IsOnThread())
  {
    close_cb();
    return;
  }

  Host::RunOnCoreThread([]() { VideoThread::RunOnThread(close_cb); });
}

void FullscreenUI::ProgressDialog::ProgressCallbackImpl::SetStatusText(std::string_view text)
{
  Host::RunOnCoreThread([text = std::string(text)]() mutable {
    VideoThread::RunOnThread([text = std::move(text)]() mutable {
      if (!s_state.progress_dialog.IsOpen())
        return;

      s_state.progress_dialog.m_status_text = std::move(text);
    });
  });
}

void FullscreenUI::ProgressDialog::ProgressCallbackImpl::SetProgressRange(u32 range)
{
  ProgressCallback::SetProgressRange(range);

  Host::RunOnCoreThread([range]() {
    VideoThread::RunOnThread([range]() {
      if (!s_state.progress_dialog.IsOpen())
        return;

      s_state.progress_dialog.m_progress_range = range;
    });
  });
}

void FullscreenUI::ProgressDialog::ProgressCallbackImpl::SetProgressValue(u32 value)
{
  ProgressCallback::SetProgressValue(value);

  Host::RunOnCoreThread([value]() {
    VideoThread::RunOnThread([value]() {
      if (!s_state.progress_dialog.IsOpen())
        return;

      s_state.progress_dialog.m_progress_value = value;
    });
  });
}

void FullscreenUI::ProgressDialog::ProgressCallbackImpl::SetCancellable(bool cancellable)
{
  ProgressCallback::SetCancellable(cancellable);

  Host::RunOnCoreThread([cancellable]() {
    VideoThread::RunOnThread([cancellable]() {
      if (!s_state.progress_dialog.IsOpen())
        return;

      s_state.progress_dialog.m_user_closeable = cancellable;
    });
  });
}

bool FullscreenUI::ProgressDialog::ProgressCallbackImpl::IsCancelled() const
{
  return s_state.progress_dialog.m_cancelled.load(std::memory_order_acquire);
}

void FullscreenUI::ProgressDialog::ProgressCallbackImpl::AlertPrompt(PromptIcon icon, std::string_view message)
{
  s_state.progress_dialog.m_prompt_waiting.test_and_set(std::memory_order_release);

  Host::RunOnCoreThread([message = std::string(message), icon]() mutable {
    VideoThread::RunOnThread([message = std::move(message), icon]() mutable {
      if (!s_state.progress_dialog.IsOpen())
      {
        s_state.progress_dialog.m_prompt_waiting.clear(std::memory_order_release);
        s_state.progress_dialog.m_prompt_waiting.notify_one();
        return;
      }

      // need to save state, since opening the next dialog will close this one
      std::string existing_title = s_state.progress_dialog.GetTitle();
      u32 progress_range = s_state.progress_dialog.m_progress_range;
      u32 progress_value = s_state.progress_dialog.m_progress_value;
      float last_frac = s_state.progress_dialog.m_last_frac;
      float width = s_state.progress_dialog.m_width;
      s_state.progress_dialog.CloseImmediately();

      std::string_view icon_str;
      switch (icon)
      {
        case PromptIcon::Error:
          icon_str = ICON_EMOJI_NO_ENTRY_SIGN;
          break;
        case PromptIcon::Warning:
          icon_str = ICON_EMOJI_WARNING;
          break;
        case PromptIcon::Question:
          icon_str = ICON_EMOJI_QUESTION_MARK;
          break;
        case PromptIcon::Information:
        default:
          icon_str = ICON_EMOJI_INFORMATION;
          break;
      }

      OpenInfoMessageDialog(
        icon_str, s_state.progress_dialog.GetTitle(), std::move(message),
        [existing_title = std::move(existing_title), progress_range, progress_value, last_frac, width]() mutable {
          s_state.progress_dialog.SetTitleAndOpen(std::move(existing_title));
          s_state.progress_dialog.m_progress_range = progress_range;
          s_state.progress_dialog.m_progress_value = progress_value;
          s_state.progress_dialog.m_last_frac = last_frac;
          s_state.progress_dialog.m_width = width;
          s_state.progress_dialog.m_prompt_waiting.clear(std::memory_order_release);
          s_state.progress_dialog.m_prompt_waiting.notify_one();
        });
    });
  });

  s_state.progress_dialog.m_prompt_waiting.wait(true, std::memory_order_acquire);
}

bool FullscreenUI::ProgressDialog::ProgressCallbackImpl::ConfirmPrompt(PromptIcon icon, std::string_view message,
                                                                       std::string_view yes_text /* =  */,
                                                                       std::string_view no_text /* = */)
{
  s_state.progress_dialog.m_prompt_result.store(false, std::memory_order_relaxed);
  s_state.progress_dialog.m_prompt_waiting.test_and_set(std::memory_order_release);

  Host::RunOnCoreThread(
    [message = std::string(message), yes_text = std::string(yes_text), no_text = std::string(no_text)]() mutable {
      VideoThread::RunOnThread(
        [message = std::move(message), yes_text = std::move(yes_text), no_text = std::move(no_text)]() mutable {
          if (!s_state.progress_dialog.IsOpen())
          {
            s_state.progress_dialog.m_prompt_waiting.clear(std::memory_order_release);
            s_state.progress_dialog.m_prompt_waiting.notify_one();
            return;
          }

          // need to save state, since opening the next dialog will close this one
          std::string existing_title = s_state.progress_dialog.GetTitle();
          u32 progress_range = s_state.progress_dialog.m_progress_range;
          u32 progress_value = s_state.progress_dialog.m_progress_value;
          float last_frac = s_state.progress_dialog.m_last_frac;
          float width = s_state.progress_dialog.m_width;
          s_state.progress_dialog.CloseImmediately();

          if (yes_text.empty())
            yes_text = FSUI_ICONSTR(ICON_FA_CHECK, "Yes");
          if (no_text.empty())
            no_text = FSUI_ICONSTR(ICON_FA_XMARK, "No");

          OpenConfirmMessageDialog(ICON_EMOJI_QUESTION_MARK, s_state.progress_dialog.GetTitle(), std::move(message),
                                   [existing_title = std::move(existing_title), progress_range, progress_value,
                                    last_frac, width](bool result) mutable {
                                     s_state.progress_dialog.SetTitleAndOpen(std::move(existing_title));
                                     s_state.progress_dialog.m_progress_range = progress_range;
                                     s_state.progress_dialog.m_progress_value = progress_value;
                                     s_state.progress_dialog.m_last_frac = last_frac;
                                     s_state.progress_dialog.m_width = width;
                                     s_state.progress_dialog.m_prompt_result.store(result, std::memory_order_relaxed);
                                     s_state.progress_dialog.m_prompt_waiting.clear(std::memory_order_release);
                                     s_state.progress_dialog.m_prompt_waiting.notify_one();
                                   });
        });
    });

  s_state.progress_dialog.m_prompt_waiting.wait(true, std::memory_order_acquire);
  return s_state.progress_dialog.m_prompt_result.load(std::memory_order_relaxed);
}

std::unique_ptr<ProgressCallbackWithPrompt> FullscreenUI::OpenModalProgressDialog(std::string title,
                                                                                  float window_unscaled_width)
{
  return s_state.progress_dialog.GetProgressCallback(std::move(title), window_unscaled_width);
}

ImGuiID FullscreenUI::GetBackgroundProgressID(std::string_view str_id)
{
  return ImHashStr(str_id.data(), str_id.length());
}

void FullscreenUI::OpenBackgroundProgressDialog(std::string_view str_id, std::string message, s32 min, s32 max,
                                                s32 value)
{
  const ImGuiID id = GetBackgroundProgressID(str_id);

  std::unique_lock lock(s_state.shared_state_mutex);
  const bool was_active = AreAnyNotificationsActive();

#if defined(_DEBUG) || defined(_DEVEL)
  for (const BackgroundProgressDialogData& data : s_state.background_progress_dialogs)
  {
    DebugAssert(data.id != id);
  }
#endif

  BackgroundProgressDialogData data;
  data.id = id;
  data.message = std::move(message);
  data.min = min;
  data.max = max;
  data.value = value;
  s_state.background_progress_dialogs.push_back(std::move(data));

  if (!was_active)
    UpdateNotificationsRunIdle();
}

void FullscreenUI::UpdateBackgroundProgressDialog(std::string_view str_id, std::string message, s32 min, s32 max,
                                                  s32 value)
{
  const ImGuiID id = GetBackgroundProgressID(str_id);

  std::unique_lock lock(s_state.shared_state_mutex);

  for (BackgroundProgressDialogData& data : s_state.background_progress_dialogs)
  {
    if (data.id == id)
    {
      data.message = std::move(message);
      data.min = min;
      data.max = max;
      data.value = value;
      return;
    }
  }

  Panic("Updating unknown progress entry.");
}

void FullscreenUI::CloseBackgroundProgressDialog(std::string_view str_id)
{
  const ImGuiID id = GetBackgroundProgressID(str_id);

  const std::unique_lock lock(s_state.shared_state_mutex);

  for (auto it = s_state.background_progress_dialogs.begin(); it != s_state.background_progress_dialogs.end(); ++it)
  {
    if (it->id == id)
    {
      s_state.background_progress_dialogs.erase(it);
      return;
    }
  }

  Panic("Closing unknown progress entry.");
}

bool FullscreenUI::IsBackgroundProgressDialogOpen(std::string_view str_id)
{
  const ImGuiID id = GetBackgroundProgressID(str_id);

  std::unique_lock lock(s_state.shared_state_mutex);

  for (auto it = s_state.background_progress_dialogs.begin(); it != s_state.background_progress_dialogs.end(); ++it)
  {
    if (it->id == id)
      return true;
  }

  return false;
}

void FullscreenUI::DrawBackgroundProgressDialogs(float& current_y)
{
  if (s_state.background_progress_dialogs.empty())
    return;

  const float window_width = LayoutScale(500.0f);
  const float window_height = LayoutScale(75.0f);
  const float window_pos_x = (ImGui::GetIO().DisplaySize.x - window_width) * 0.5f;
  const float window_spacing = LayoutScale(10.0f);

  ImDrawList* dl = ImGui::GetForegroundDrawList();

  for (const BackgroundProgressDialogData& data : s_state.background_progress_dialogs)
  {
    const float window_pos_y = current_y - window_height;

    dl->AddRectFilled(ImVec2(window_pos_x, window_pos_y),
                      ImVec2(window_pos_x + window_width, window_pos_y + window_height),
                      IM_COL32(0x11, 0x11, 0x11, 200), LayoutScale(10.0f));

    ImVec2 pos(window_pos_x + LayoutScale(10.0f), window_pos_y + LayoutScale(10.0f));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, pos, IM_COL32(255, 255, 255, 255),
                IMSTR_START_END(data.message), 0.0f);
    pos.y += UIStyle.MediumFontSize + LayoutScale(10.0f);

    const ImVec2 box_end(pos.x + window_width - LayoutScale(10.0f * 2.0f), pos.y + LayoutScale(25.0f));
    dl->AddRectFilled(pos, box_end, ImGui::GetColorU32(UIStyle.PrimaryDarkColor));

    if (data.min != data.max)
    {
      const float fraction = static_cast<float>(data.value - data.min) / static_cast<float>(data.max - data.min);
      dl->AddRectFilled(pos, ImVec2(pos.x + fraction * (box_end.x - pos.x), box_end.y),
                        ImGui::GetColorU32(UIStyle.SecondaryColor));

      TinyString text = TinyString::from_format("{}%", static_cast<int>(std::round(fraction * 100.0f)));
      const ImVec2 text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                           0.0f, IMSTR_START_END(text));
      const ImVec2 text_pos(pos.x + ((box_end.x - pos.x) / 2.0f) - (text_size.x / 2.0f),
                            pos.y + ((box_end.y - pos.y) / 2.0f) - (text_size.y / 2.0f));
      dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, text_pos,
                  ImGui::GetColorU32(UIStyle.PrimaryTextColor), IMSTR_START_END(text));
    }
    else
    {
      // indeterminate, so draw a scrolling bar
      const float bar_width = LayoutScale(30.0f);
      const float fraction = static_cast<float>(std::fmod(ImGui::GetTime(), 2.0) * 0.5);
      const ImVec2 bar_start(pos.x + ImLerp(0.0f, box_end.x, fraction) - bar_width, pos.y);
      const ImVec2 bar_end(std::min(bar_start.x + bar_width, box_end.x), pos.y + LayoutScale(25.0f));
      dl->AddRectFilled(ImClamp(bar_start, pos, box_end), ImClamp(bar_end, pos, box_end),
                        ImGui::GetColorU32(UIStyle.SecondaryColor));
    }

    current_y = current_y - window_height - window_spacing;
  }
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

void FullscreenUI::BackgroundProgressCallback::SetCancelled()
{
  if (m_cancellable)
    m_cancelled = true;
}

void FullscreenUI::RenderLoadingScreen(std::string_view image, std::string_view title, std::string_view caption,
                                       s32 progress_min /*= -1*/, s32 progress_max /*= -1*/,
                                       s32 progress_value /*= -1*/)
{
  if (progress_min < progress_max)
  {
    UpdateLoadingScreenProgress(progress_min, progress_max, progress_value);
    INFO_LOG("{}: {}/{}", title, progress_value, progress_max);
  }

  if (!g_gpu_device || !g_gpu_device->HasMainSwapChain())
    return;

  // eat the last imgui frame, it might've been partially rendered by the caller.
  ImGui::EndFrame();
  ImGui::NewFrame();

  DrawLoadingScreen(image, title, caption, progress_min, progress_max, progress_value, false);

  ImGuiManager::CreateDrawLists();

  GPUSwapChain* swap_chain = g_gpu_device->GetMainSwapChain();
  if (g_gpu_device->BeginPresent(swap_chain) == GPUDevice::PresentResult::OK)
  {
    ImGuiManager::RenderDrawLists(swap_chain);
    g_gpu_device->EndPresent(swap_chain, false);
  }

  ImGuiManager::NewFrame(Timer::GetCurrentValue());
}

void FullscreenUI::UpdateLoadingScreenRunIdle()
{
  // early out if we're already on the GPU thread
  if (VideoThread::IsOnThread())
  {
    VideoThread::SetRunIdleReason(VideoThread::RunIdleReason::LoadingScreenActive, s_state.loading_screen_open);
    VideoThread::SetRunIdleReason(VideoThread::RunIdleReason::NotificationsActive, AreAnyNotificationsActive());
    return;
  }

  // need to check it again once we're executing on the gpu thread, it could've changed since
  VideoThread::RunOnThread([]() {
    VideoThread::SetRunIdleReason(VideoThread::RunIdleReason::LoadingScreenActive, s_state.loading_screen_open);
    VideoThread::SetRunIdleReason(VideoThread::RunIdleReason::NotificationsActive, AreAnyNotificationsActive());
  });
}

void FullscreenUI::OpenOrUpdateLoadingScreen(std::string_view image, std::string_view title,
                                             std::string_view caption /* =  */, s32 progress_min /* = -1 */,
                                             s32 progress_max /* = -1 */, s32 progress_value /* = -1 */)
{
  const std::unique_lock lock(s_state.shared_state_mutex);
  const bool was_loading_screen_open = std::exchange(s_state.loading_screen_open, true);

  if (!image.empty() && s_state.loading_screen_image != image)
    s_state.loading_screen_image = image;
  if (s_state.loading_screen_title != title)
    s_state.loading_screen_title = title;
  if (s_state.loading_screen_caption != caption)
    s_state.loading_screen_caption = caption;

  if (progress_min < progress_max)
  {
    UpdateLoadingScreenProgress(progress_min, progress_max, progress_value);

    if (s_state.loading_screen_caption.empty())
    {
      s_state.loading_screen_caption.clear();
      fmt::format_to(std::back_inserter(s_state.loading_screen_caption), FSUI_FSTR("{} of {}"), progress_value,
                     progress_max);
    }
  }

  if (!was_loading_screen_open)
    UpdateLoadingScreenRunIdle();
}

bool FullscreenUI::IsLoadingScreenOpen()
{
  return s_state.loading_screen_open;
}

void FullscreenUI::DrawLoadingScreen()
{
  if (!s_state.loading_screen_open)
    return;

  DrawLoadingScreen(s_state.loading_screen_image, s_state.loading_screen_title, s_state.loading_screen_caption,
                    s_state.loading_screen_min, s_state.loading_screen_max, s_state.loading_screen_value, true);
}

void FullscreenUI::CloseLoadingScreen()
{
  const std::unique_lock lock(s_state.shared_state_mutex);
  if (!s_state.loading_screen_open)
    return;

  s_state.loading_screen_samples = {};
  s_state.loading_screen_valid_samples = 0;
  s_state.loading_screen_sample_index = 0;
  s_state.loading_screen_caption = {};
  s_state.loading_screen_title = {};
  s_state.loading_screen_image = {};
  s_state.loading_screen_min = 0;
  s_state.loading_screen_max = 0;
  s_state.loading_screen_value = 0;
  s_state.loading_screen_open = false;

  UpdateLoadingScreenRunIdle();
}

void FullscreenUI::UpdateLoadingScreenProgress(s32 progress_min, s32 progress_max, s32 progress_value)
{
  if (progress_min != s_state.loading_screen_min || progress_max != s_state.loading_screen_max ||
      progress_value < s_state.loading_screen_value)
  {
    // new range, toss time calculations
    s_state.loading_screen_min = progress_min;
    s_state.loading_screen_max = progress_max;
    s_state.loading_screen_value = progress_value;

    // reset estimation data
    s_state.loading_screen_samples[0] = std::make_pair(Timer::GetCurrentValue(), progress_value);
    s_state.loading_screen_sample_index = 0;
    s_state.loading_screen_valid_samples = 1;
    return;
  }

  // Only update if the value has changed
  if (s_state.loading_screen_value != progress_value)
  {
    s_state.loading_screen_value = progress_value;

    // Add new sample
    s_state.loading_screen_sample_index = (s_state.loading_screen_sample_index + 1) % LOADING_PROGRESS_SAMPLE_COUNT;
    s_state.loading_screen_samples[s_state.loading_screen_sample_index] =
      std::make_pair(Timer::GetCurrentValue(), progress_value);
    s_state.loading_screen_valid_samples =
      std::min(s_state.loading_screen_valid_samples + 1, static_cast<u32>(LOADING_PROGRESS_SAMPLE_COUNT));
  }
}

bool FullscreenUI::GetLoadingScreenTimeEstimate(SmallString& out_str)
{
  // Calculate average progress rate using all samples
  Timer::Value total_time_diff = 0;
  s32 total_progress_diff = 0;
  for (u32 i = 0; i < (s_state.loading_screen_valid_samples - 1); i++)
  {
    const u32 idx =
      (s_state.loading_screen_sample_index + LOADING_PROGRESS_SAMPLE_COUNT - i) % LOADING_PROGRESS_SAMPLE_COUNT;
    const u32 prev_idx = (idx == 0) ? (LOADING_PROGRESS_SAMPLE_COUNT - 1) : (idx - 1);
    total_time_diff += s_state.loading_screen_samples[idx].first - s_state.loading_screen_samples[prev_idx].first;
    total_progress_diff += s_state.loading_screen_samples[idx].second - s_state.loading_screen_samples[prev_idx].second;
  }

  if (total_progress_diff == 0)
    return false;

  // Calculate average progress rate per second
  const double progress_per_second =
    static_cast<double>(total_progress_diff) / Timer::ConvertValueToSeconds(total_time_diff);
  const s32 remaining_progress = s_state.loading_screen_max - s_state.loading_screen_value;
  const double remaining_seconds = std::max(remaining_progress / progress_per_second, 0.0);

  // Format the time string
  if (remaining_seconds < 60.0)
  {
    // lupdate isn't smart enough to find the next line...
    const int secs = static_cast<int>(remaining_seconds);
    out_str = TRANSLATE_PLURAL_SSTR("FullscreenUI", "%n seconds remaining", "Loading time", secs);
  }
  else
  {
    const int mins = static_cast<int>(remaining_seconds / 60.0);
    out_str = TRANSLATE_PLURAL_SSTR("FullscreenUI", "%n minutes remaining", "Loading time", mins);
  }

  return true;
}

void FullscreenUI::DrawLoadingScreen(std::string_view image, std::string_view title, std::string_view caption,
                                     s32 progress_min, s32 progress_max, s32 progress_value, bool is_persistent)
{
  const auto& io = ImGui::GetIO();
  const bool has_progress = (progress_min < progress_max);

  const float item_spacing = LayoutScale(10.0f);
  const float frame_rounding = LayoutScale(6.0f);
  const float bar_height = (has_progress || is_persistent) ? LayoutScale(10.0f) : 0.0f;
  const float content_width = LayoutScale(450.0f);
  const float image_width = LayoutScale(260.0f);
  const float image_height = LayoutScale(260.0f);
  const float image_spacing = LayoutScale(20.0f);

  const float title_font_size = UIStyle.LargeFontSize;
  const float title_font_weight = UIStyle.BoldFontWeight;
  const ImVec2 title_size =
    UIStyle.Font->CalcTextSizeA(title_font_size, title_font_weight, FLT_MAX, content_width, IMSTR_START_END(title));
  const u32 title_color = ImGui::GetColorU32(UIStyle.PrimaryTextColor);

  const float caption_font_size = UIStyle.MediumLargeFontSize;
  const float caption_font_weight = UIStyle.NormalFontWeight;
  const ImVec2 caption_size = caption.empty() ?
                                ImVec2() :
                                UIStyle.Font->CalcTextSizeA(caption_font_size, caption_font_weight, FLT_MAX,
                                                            content_width, IMSTR_START_END(caption));
  const u32 caption_color = ImGui::GetColorU32(DarkerColor(UIStyle.PrimaryTextColor));

  const float estimate_font_size = UIStyle.MediumFontSize;
  const float estimate_font_weight = UIStyle.NormalFontWeight;
  const u32 estimate_color = ImGui::GetColorU32(DarkerColor(DarkerColor(UIStyle.PrimaryTextColor)));
  SmallString estimate;
  ImVec2 estimate_size;
  if (has_progress)
  {
    if (GetLoadingScreenTimeEstimate(estimate))
    {
      estimate_size = UIStyle.Font->CalcTextSizeA(estimate_font_size, estimate_font_weight, FLT_MAX, content_width,
                                                  IMSTR_START_END(estimate));
    }
  }

  const float total_height = image_height + image_spacing + title_size.y +
                             (bar_height > 0.0f ? (item_spacing + bar_height) : 0.0f) +
                             (!caption.empty() ? (item_spacing + caption_size.y) : 0.0f) +
                             (has_progress ? (item_spacing + estimate_font_size) : 0.0f);
  const ImVec2 image_pos =
    ImVec2(ImCeil((io.DisplaySize.x - image_width) * 0.5f), ImCeil(((io.DisplaySize.y - total_height) * 0.5f)));
  ImDrawList* const dl = ImGui::GetBackgroundDrawList();
  GPUTexture* tex = GetCachedTexture(image);
  if (tex)
  {
    const ImRect image_rect = CenterImage(ImRect(image_pos, image_pos + ImVec2(image_width, image_height)), tex);
    dl->AddImage(tex, image_rect.Min, image_rect.Max);
  }

  ImVec2 current_pos =
    ImVec2(ImCeil((io.DisplaySize.x - content_width) * 0.5f), image_pos.y + image_height + image_spacing);

  RenderMultiLineShadowedTextClipped(dl, UIStyle.Font, title_font_size, title_font_weight, current_pos,
                                     current_pos + ImVec2(content_width, title_size.y), title_color, title,
                                     ImVec2(0.5f, 0.0f), content_width);
  current_pos.y += title_size.y + item_spacing;

  if (bar_height > 0.0f)
  {
    const ImVec2& box_start = current_pos;
    const ImVec2 box_end = box_start + ImVec2(content_width, bar_height);
    dl->AddRectFilled(box_start, box_end, ImGui::GetColorU32(UIStyle.PopupFrameBackgroundColor), frame_rounding);

    if (has_progress)
    {
      const float fraction = static_cast<float>(progress_value) / static_cast<float>(progress_max - progress_min);
      ImGui::RenderRectFilledInRangeH(dl, ImRect(box_start, box_end), ImGui::GetColorU32(UIStyle.SecondaryColor),
                                      box_start.x, box_start.x + (fraction * content_width), frame_rounding);
    }
    else
    {
      // indeterminate, so draw a scrolling bar
      const float bar_width = LayoutScale(30.0f);
      const float fraction = static_cast<float>(std::fmod(ImGui::GetTime(), 2.0) * 0.5);
      const ImVec2 bar_start = ImVec2(box_start.x + ImLerp(0.0f, box_end.x, fraction) - bar_width, box_start.y);
      const ImVec2 bar_end = ImVec2(std::min(bar_start.x + bar_width, box_end.x), box_end.y);
      if ((bar_end.x - bar_start.x) > LayoutScale(1.0f))
      {
        dl->AddRectFilled(ImClamp(bar_start, box_start, box_end), ImClamp(bar_end, box_start, box_end),
                          ImGui::GetColorU32(UIStyle.SecondaryColor), frame_rounding);
      }
    }

    current_pos.y += bar_height + item_spacing;
  }

  if (!caption.empty())
  {
    RenderMultiLineShadowedTextClipped(dl, UIStyle.Font, caption_font_size, caption_font_weight, current_pos,
                                       current_pos + ImVec2(content_width, caption_size.y), caption_color, caption,
                                       ImVec2(0.5f, 0.0f), content_width);
    current_pos.y += caption_size.y + item_spacing;
  }

  if (!estimate.empty())
  {
    RenderMultiLineShadowedTextClipped(dl, UIStyle.Font, estimate_font_size, estimate_font_weight, current_pos,
                                       current_pos + ImVec2(content_width, estimate_size.y), estimate_color, estimate,
                                       ImVec2(0.5f, 0.0f), content_width);
  }
}

FullscreenUI::LoadingScreenProgressCallback::LoadingScreenProgressCallback()
  : ProgressCallback(), m_open_time(Timer::GetCurrentValue()), m_on_video_thread(VideoThread::IsOnThread())
{
  m_image = System::GetImageForLoadingScreen(m_on_video_thread ? VideoThread::GetGamePath() : System::GetGamePath());
}

FullscreenUI::LoadingScreenProgressCallback::~LoadingScreenProgressCallback()
{
  Close();
}

void FullscreenUI::LoadingScreenProgressCallback::Close()
{
  // Did we activate?
  if (m_last_progress_percent < 0)
    return;

  if (!m_on_video_thread)
  {
    VideoThread::RunOnThread(&FullscreenUI::CloseLoadingScreen);
  }
  else
  {
    // since this was pushing frames, we need to restore the context. do that by pushing a frame ourselves
    VideoThread::Internal::PresentFrameAndRestoreContext();
  }

  m_last_progress_percent = -1;
}

void FullscreenUI::LoadingScreenProgressCallback::PushState()
{
  ProgressCallback::PushState();
}

void FullscreenUI::LoadingScreenProgressCallback::PopState()
{
  ProgressCallback::PopState();
  Redraw(true);
}

void FullscreenUI::LoadingScreenProgressCallback::SetCancellable(bool cancellable)
{
  ProgressCallback::SetCancellable(cancellable);
  Redraw(true);
}

void FullscreenUI::LoadingScreenProgressCallback::SetTitle(const std::string_view title)
{
  ProgressCallback::SetTitle(title);
  m_title = title;
  Redraw(true);
}

void FullscreenUI::LoadingScreenProgressCallback::SetStatusText(const std::string_view text)
{
  ProgressCallback::SetStatusText(text);
  Redraw(true);
}

void FullscreenUI::LoadingScreenProgressCallback::SetProgressRange(u32 range)
{
  u32 last_range = m_progress_range;

  ProgressCallback::SetProgressRange(range);

  if (m_progress_range != last_range)
    Redraw(false);
}

void FullscreenUI::LoadingScreenProgressCallback::SetProgressValue(u32 value)
{
  u32 lastValue = m_progress_value;

  ProgressCallback::SetProgressValue(value);

  if (m_progress_value != lastValue)
    Redraw(false);
}

void FullscreenUI::LoadingScreenProgressCallback::Redraw(bool force)
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

  if (m_on_video_thread)
  {
    FullscreenUI::RenderLoadingScreen(m_image, m_title, m_status_text, 0, static_cast<s32>(m_progress_range),
                                      static_cast<s32>(m_progress_value));
  }
  else
  {
    // activation? use default image if unspecified
    const std::string_view image = (m_image.empty() && m_last_progress_percent < 0 && !m_on_video_thread) ?
                                     std::string_view(ImGuiManager::LOGO_IMAGE_NAME) :
                                     std::string_view(m_image);
    FullscreenUI::OpenOrUpdateLoadingScreen(image, m_title, m_status_text, 0, static_cast<s32>(m_progress_range),
                                            static_cast<s32>(m_progress_value));
    m_image = {};
  }

  m_last_progress_percent = percent;
}

//////////////////////////////////////////////////////////////////////////
// Notifications
//////////////////////////////////////////////////////////////////////////

bool FullscreenUI::AreAnyNotificationsActive()
{
  return (!s_state.toast_title.empty() || !s_state.toast_message.empty() ||
          !s_state.background_progress_dialogs.empty() || s_state.loading_screen_open);
}

void FullscreenUI::UpdateNotificationsRunIdle()
{
  // early out if we're already on the GPU thread
  if (VideoThread::IsOnThread())
  {
    VideoThread::SetRunIdleReason(VideoThread::RunIdleReason::NotificationsActive, AreAnyNotificationsActive());
    return;
  }

  // need to check it again once we're executing on the gpu thread, it could've changed since
  VideoThread::RunOnThread([]() {
    VideoThread::SetRunIdleReason(VideoThread::RunIdleReason::NotificationsActive, AreAnyNotificationsActive());
  });
}

FullscreenUI::NotificationLayout::NotificationLayout(NotificationLocation location)
  : m_spacing(LayoutScale(10.0f)), m_location(location)
{
  const float screen_margin = ImGuiManager::GetScreenMargin();

  // android goes a little lower due to on-screen buttons
#ifndef __ANDROID__
  static constexpr float top_start_pct = 0.1f;
#else
  static constexpr float top_start_pct = 0.15f;
#endif

  const float top_margin = ImFloor(top_start_pct * ImGui::GetIO().DisplaySize.y);
  CalcStartPosition(screen_margin, top_margin);
}

FullscreenUI::NotificationLayout::NotificationLayout(NotificationLocation location, float spacing, float screen_margin)
  : m_spacing(spacing), m_location(location)
{
  CalcStartPosition(screen_margin, screen_margin);
}

void FullscreenUI::NotificationLayout::CalcStartPosition(float screen_margin, float top_margin)
{
  const ImGuiIO& io = ImGui::GetIO();
  switch (m_location)
  {
    case NotificationLocation::TopLeft:
    {
      m_current_position.x = screen_margin;
      m_current_position.y = std::max(screen_margin, top_margin);
    }
    break;

    case NotificationLocation::TopCenter:
    {
      m_current_position.x = io.DisplaySize.x * 0.5f;
      m_current_position.y = screen_margin;
    }
    break;

    case NotificationLocation::TopRight:
    {
      m_current_position.x = io.DisplaySize.x - screen_margin;
      m_current_position.y = std::max(screen_margin, top_margin);
    }
    break;

    case NotificationLocation::BottomLeft:
    {
      m_current_position.x = screen_margin;
      m_current_position.y = io.DisplaySize.y - GetScreenBottomMargin();
    }
    break;

    case NotificationLocation::BottomCenter:
    {
      m_current_position.x = io.DisplaySize.x * 0.5f;
      m_current_position.y = io.DisplaySize.y - GetScreenBottomMargin();
    }
    break;

    case NotificationLocation::BottomRight:
    {
      m_current_position.x = io.DisplaySize.x - screen_margin;
      m_current_position.y = io.DisplaySize.y - GetScreenBottomMargin();
    }
    break;

      DefaultCaseIsUnreachable();
  }

  // don't draw over osd messages
  if (m_location == g_gpu_settings.display_osd_message_location)
  {
    const float osd_end = ImGuiManager::GetOSDMessageEndPosition();
    if (m_location >= NotificationLocation::BottomLeft)
      m_current_position.y = std::min(m_current_position.y, osd_end);
    else
      m_current_position.y = std::max(m_current_position.y, osd_end);
  }
}

bool FullscreenUI::NotificationLayout::IsVerticalAnimation() const
{
  return (m_location == NotificationLocation::TopCenter || m_location == NotificationLocation::BottomCenter);
}

ImVec2 FullscreenUI::NotificationLayout::GetFixedPosition(float width, float height)
{
  switch (m_location)
  {
    case NotificationLocation::TopLeft:
    {
      const ImVec2 pos = ImVec2(m_current_position.x, m_current_position.y);
      m_current_position.y += height + m_spacing;
      return pos;
    }

    case NotificationLocation::TopCenter:
    {
      const ImVec2 pos = ImVec2(ImFloor((ImGui::GetIO().DisplaySize.x - width) * 0.5f), m_current_position.y);
      m_current_position.y += height + m_spacing;
      return pos;
    }

    case NotificationLocation::TopRight:
    {
      const ImVec2 pos = ImVec2(m_current_position.x - width, m_current_position.y);
      m_current_position.y += height + m_spacing;
      return pos;
    }

    case NotificationLocation::BottomLeft:
    {
      const ImVec2 pos = ImVec2(m_current_position.x, m_current_position.y - height);
      m_current_position.y -= height + m_spacing;
      return pos;
    }

    case NotificationLocation::BottomCenter:
    {
      const ImVec2 pos = ImVec2(ImFloor((ImGui::GetIO().DisplaySize.x - width) * 0.5f), m_current_position.y - height);
      m_current_position.y -= height + m_spacing;
      return pos;
    }

    case NotificationLocation::BottomRight:
    {
      const ImVec2 pos = ImVec2(m_current_position.x - width, m_current_position.y - height);
      m_current_position.y -= height + m_spacing;
      return pos;
    }

      DefaultCaseIsUnreachable();
  }
}

std::pair<ImVec2, float> FullscreenUI::NotificationLayout::GetNextPosition(float width, float height, bool active,
                                                                           float anim_coeff, float width_coeff)
{
  switch (m_location)
  {
    case NotificationLocation::TopLeft:
    case NotificationLocation::BottomLeft:
    case NotificationLocation::TopRight:
    case NotificationLocation::BottomRight:
    {
      float opacity;
      ImVec2 pos;
      if (m_location == NotificationLocation::TopLeft || m_location == NotificationLocation::BottomLeft)
      {
        if (anim_coeff != 1.0f)
        {
          if (active)
          {
            const float eased_pct = Easing::OutExpo(anim_coeff);
            pos.x = ImFloor(m_current_position.x - (width * width_coeff * (1.0f - eased_pct)));
            opacity = anim_coeff;
          }
          else
          {
            const float eased_pct = Easing::OutExpo(anim_coeff);
            pos.x = ImFloor(m_current_position.x - (width * width_coeff * (1.0f - eased_pct)));
            opacity = eased_pct;
          }
        }
        else
        {
          pos.x = m_current_position.x;
          opacity = 1.0f;
        }
      }
      else
      {
        pos.x = m_current_position.x - width;
        if (anim_coeff != 1.0f)
        {
          if (active)
          {
            const float eased_pct = std::clamp(Easing::OutExpo(anim_coeff), 0.0f, 1.0f);
            pos.x = ImFloor(pos.x + (width * width_coeff * (1.0f - eased_pct)));
            opacity = anim_coeff;
          }
          else
          {
            const float eased_pct = std::clamp(Easing::InExpo(anim_coeff), 0.0f, 1.0f);
            pos.x = ImFloor(pos.x + (width * width_coeff * (1.0f - eased_pct)));
            opacity = eased_pct;
          }
        }
        else
        {
          opacity = 1.0f;
        }
      }

      if (m_location == NotificationLocation::TopLeft || m_location == NotificationLocation::TopRight)
      {
        pos.y = m_current_position.y;
        m_current_position.y = m_current_position.y + height + m_spacing;
      }
      else
      {
        pos.y = m_current_position.y - height;
        m_current_position.y = m_current_position.y - height - m_spacing;
      }

      return std::make_pair(pos, opacity);
    }

    case NotificationLocation::TopCenter:
    {
      float opacity;
      ImVec2 pos;

      pos.x = ImFloor((ImGui::GetIO().DisplaySize.x - width) * 0.5f);
      pos.y = m_current_position.y;
      if (anim_coeff != 1.0f)
      {
        if (active)
        {
          const float eased_pct = std::clamp(Easing::OutExpo(anim_coeff), 0.0f, 1.0f);
          // pos.x = ImFloor(pos.x - (width * width_coeff * (1.0f - eased_pct)));
          pos.y = ImFloor(pos.y - (height * width_coeff * (1.0f - eased_pct)));
          opacity = anim_coeff;
        }
        else
        {
          const float eased_pct = std::clamp(Easing::InExpo(anim_coeff), 0.0f, 1.0f);
          // pos.x = ImFloor(pos.x + (width * width_coeff * (1.0f - eased_pct)));
          pos.y = ImFloor(pos.y - (height * width_coeff * (1.0f - eased_pct)));
          opacity = eased_pct;
        }
      }
      else
      {
        opacity = 1.0f;
      }

      m_current_position.y = m_current_position.y + height + m_spacing;

      return std::make_pair(pos, opacity);
    }

    case NotificationLocation::BottomCenter:
    {
      float opacity;
      ImVec2 pos;

      pos.x = ImFloor((ImGui::GetIO().DisplaySize.x - width) * 0.5f);
      pos.y = m_current_position.y - height;
      if (anim_coeff != 1.0f)
      {
        if (active)
        {
          const float eased_pct = std::clamp(Easing::OutExpo(anim_coeff), 0.0f, 1.0f);
          // pos.x = ImFloor(pos.x - (width * width_coeff * (1.0f - eased_pct)));
          pos.y = ImFloor(pos.y + (height * width_coeff * (1.0f - eased_pct)));
          opacity = anim_coeff;
        }
        else
        {
          const float eased_pct = std::clamp(Easing::InExpo(anim_coeff), 0.0f, 1.0f);
          // pos.x = ImFloor(pos.x + (width * width_coeff * (1.0f - eased_pct)));
          pos.y = ImFloor(pos.y + (height * width_coeff * (1.0f - eased_pct)));
          opacity = eased_pct;
        }
      }
      else
      {
        opacity = 1.0f;
      }

      m_current_position.y = m_current_position.y - height - m_spacing;

      return std::make_pair(pos, opacity);
    }

      DefaultCaseIsUnreachable();
  }
}

std::pair<ImVec2, float> FullscreenUI::NotificationLayout::GetNextPosition(float width, float height, bool active,
                                                                           float time, float in_duration,
                                                                           float out_duration, float width_coeff)
{
  const float anim_coeff = active ? std::min(time / in_duration, 1.0f) : std::min(time / out_duration, 1.0f);
  return GetNextPosition(width, height, active, anim_coeff, width_coeff);
}

std::pair<ImVec2, float> FullscreenUI::NotificationLayout::GetNextPosition(float width, float height, float time_passed,
                                                                           float total_duration, float in_duration,
                                                                           float out_duration, float width_coeff)
{
  const bool active = (time_passed < (total_duration - out_duration));
  const float anim_coeff =
    active ? std::min(time_passed / in_duration, 1.0f) : std::max((total_duration - time_passed) / out_duration, 0.0f);
  return GetNextPosition(width, height, active, anim_coeff, width_coeff);
}

void FullscreenUI::ShowToast(OSDMessageType type, std::string title, std::string message)
{
  const std::unique_lock lock(s_state.shared_state_mutex);
  if (!s_state.has_initialized)
    return;

  const bool prev_had_notifications = AreAnyNotificationsActive();
  s_state.toast_title = std::move(title);
  s_state.toast_message = std::move(message);
  s_state.toast_start_time = Timer::GetCurrentValue();
  s_state.toast_duration = ImGuiManager::GetOSDMessageDuration(type);

  if (!prev_had_notifications)
    UpdateNotificationsRunIdle();
}

void FullscreenUI::DrawToast(float& current_y)
{
  if (s_state.toast_title.empty() && s_state.toast_message.empty())
    return;

  const float elapsed =
    static_cast<float>(Timer::ConvertValueToSeconds(Timer::GetCurrentValue() - s_state.toast_start_time));
  if (elapsed >= s_state.toast_duration)
  {
    s_state.toast_message = {};
    s_state.toast_title = {};
    s_state.toast_start_time = 0;
    s_state.toast_duration = 0.0f;
    return;
  }

  // fade out the last second
  const float alpha = std::min(std::min(elapsed * 4.0f, s_state.toast_duration - elapsed), 1.0f);

  const float max_width = LayoutScale(600.0f);

  ImFont* const title_font = UIStyle.Font;
  const float title_font_size = UIStyle.LargeFontSize;
  const float title_font_weight = UIStyle.BoldFontWeight;
  ImFont* message_font = UIStyle.Font;
  const float message_font_size = UIStyle.MediumFontSize;
  const float message_font_weight = UIStyle.NormalFontWeight;
  const float padding = LayoutScale(20.0f);
  const float total_padding = padding * 2.0f;
  const float spacing = s_state.toast_title.empty() ? 0.0f : LayoutScale(10.0f);
  const ImVec2 title_size = s_state.toast_title.empty() ?
                              ImVec2(0.0f, 0.0f) :
                              title_font->CalcTextSizeA(title_font_size, title_font_weight, FLT_MAX, max_width,
                                                        IMSTR_START_END(s_state.toast_title));
  const ImVec2 message_size = s_state.toast_message.empty() ?
                                ImVec2(0.0f, 0.0f) :
                                message_font->CalcTextSizeA(message_font_size, message_font_weight, FLT_MAX, max_width,
                                                            IMSTR_START_END(s_state.toast_message));
  const ImVec2 comb_size(std::max(title_size.x, message_size.x), title_size.y + spacing + message_size.y);

  const ImVec2 box_size(comb_size.x + total_padding, comb_size.y + total_padding);
  const ImVec2 box_pos((ImGui::GetIO().DisplaySize.x - box_size.x) * 0.5f, current_y - box_size.y);

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(box_pos, box_pos + box_size,
                    ImGui::GetColorU32(ModAlpha(UIStyle.ToastBackgroundColor, alpha * 0.95f)), padding);

  if (!s_state.toast_title.empty())
  {
    const u32 text_col = ImGui::GetColorU32(ModAlpha(UIStyle.ToastTextColor, alpha));
    const float offset = (comb_size.x - title_size.x) * 0.5f;
    const ImVec2 title_pos = box_pos + ImVec2(offset + padding, padding);
    const ImRect title_bb = ImRect(title_pos, title_pos + title_size);
    RenderShadowedTextClipped(dl, title_font, title_font_size, title_font_weight, title_bb.Min, title_bb.Max, text_col,
                              s_state.toast_title, &title_size, ImVec2(0.0f, 0.0f), max_width, &title_bb);
  }
  if (!s_state.toast_message.empty())
  {
    const u32 text_col = ImGui::GetColorU32(
      ModAlpha(s_state.toast_title.empty() ? UIStyle.ToastTextColor : DarkerColor(UIStyle.ToastTextColor), alpha));
    const float offset = (comb_size.x - message_size.x) * 0.5f;
    const ImVec2 message_pos = box_pos + ImVec2(offset + padding, padding + spacing + title_size.y);
    const ImRect message_bb = ImRect(message_pos, message_pos + message_size);
    RenderShadowedTextClipped(dl, message_font, message_font_size, message_font_weight, message_bb.Min, message_bb.Max,
                              text_col, s_state.toast_message, &message_size, ImVec2(0.0f, 0.0f), max_width,
                              &message_bb);
  }
}

std::span<const char* const> FullscreenUI::GetThemeNames()
{
  return s_theme_names;
}

std::span<const char* const> FullscreenUI::GetThemeDisplayNames()
{
  return s_theme_display_names;
}

std::vector<std::string_view> FullscreenUI::GetLocalizedThemeDisplayNames()
{
  std::vector<std::string_view> ret;
  ret.reserve(std::size(s_theme_display_names));
  for (const char* name : s_theme_display_names)
    ret.push_back(TRANSLATE_SV("FullscreenUI", name));
  return ret;
}

void FullscreenUI::UpdateTheme()
{
  TinyString theme =
    Core::GetBaseTinyStringSettingValue("UI", "FullscreenUITheme", Host::GetDefaultFullscreenUITheme());
  if (theme.empty())
    theme = Host::GetDefaultFullscreenUITheme();

  if (theme == "AMOLED")
  {
    UIStyle.BackgroundColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.BackgroundTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.BackgroundLineColor = HEX_TO_IMVEC4(0xf0f0f0, 0xff);
    UIStyle.BackgroundHighlight = HEX_TO_IMVEC4(0x0c0c0c, 0xff);
    UIStyle.PopupBackgroundColor = HEX_TO_IMVEC4(0x212121, 0xf2);
    UIStyle.PopupFrameBackgroundColor = HEX_TO_IMVEC4(0x313131, 0xf2);
    UIStyle.PrimaryColor = HEX_TO_IMVEC4(0x0a0a0a, 0xff);
    UIStyle.PrimaryLightColor = HEX_TO_IMVEC4(0xb5b5b5, 0xff);
    UIStyle.PrimaryDarkColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.PrimaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.DisabledColor = HEX_TO_IMVEC4(0x8d8d8d, 0xff);
    UIStyle.TextHighlightColor = HEX_TO_IMVEC4(0x676767, 0xff);
    UIStyle.PrimaryLineColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.SecondaryColor = HEX_TO_IMVEC4(0x969696, 0xff);
    UIStyle.SecondaryStrongColor = HEX_TO_IMVEC4(0x191919, 0xff);
    UIStyle.SecondaryWeakColor = HEX_TO_IMVEC4(0x474747, 0xff);
    UIStyle.SecondaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ToastBackgroundColor = HEX_TO_IMVEC4(0x282828, 0xff);
    UIStyle.ToastTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ShadowColor = IM_COL32(0, 0, 0, 100);
    UIStyle.IsDarkTheme = true;
  }
  else if (theme == "CobaltSky")
  {
    UIStyle.BackgroundColor = HEX_TO_IMVEC4(0x2b3760, 0xff);
    UIStyle.BackgroundTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.BackgroundLineColor = HEX_TO_IMVEC4(0xf0f0f0, 0xff);
    UIStyle.BackgroundHighlight = HEX_TO_IMVEC4(0x3b54ac, 0xff);
    UIStyle.PopupBackgroundColor = HEX_TO_IMVEC4(0x2b3760, 0xf2);
    UIStyle.PopupFrameBackgroundColor = HEX_TO_IMVEC4(0x3b54ac, 0xf2);
    UIStyle.PrimaryColor = HEX_TO_IMVEC4(0x202e5a, 0xff);
    UIStyle.PrimaryLightColor = HEX_TO_IMVEC4(0xb5b5b5, 0xff);
    UIStyle.PrimaryDarkColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.PrimaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.DisabledColor = HEX_TO_IMVEC4(0x8d8d8d, 0xff);
    UIStyle.TextHighlightColor = HEX_TO_IMVEC4(0x676767, 0xff);
    UIStyle.PrimaryLineColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.SecondaryColor = HEX_TO_IMVEC4(0x969696, 0xff);
    UIStyle.SecondaryStrongColor = HEX_TO_IMVEC4(0x245dda, 0xff);
    UIStyle.SecondaryWeakColor = HEX_TO_IMVEC4(0x3a3d7b, 0xff);
    UIStyle.SecondaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ToastBackgroundColor = HEX_TO_IMVEC4(0x2d4183, 0xff);
    UIStyle.ToastTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ShadowColor = IM_COL32(0, 0, 0, 100);
    UIStyle.IsDarkTheme = true;
  }
  else if (theme == "GreyMatter")
  {
    UIStyle.BackgroundColor = HEX_TO_IMVEC4(0x353944, 0xff);
    UIStyle.BackgroundTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.BackgroundLineColor = HEX_TO_IMVEC4(0xf0f0f0, 0xff);
    UIStyle.BackgroundHighlight = HEX_TO_IMVEC4(0x484d57, 0xff);
    UIStyle.PopupFrameBackgroundColor = HEX_TO_IMVEC4(0x313131, 0xf2);
    UIStyle.PopupBackgroundColor = HEX_TO_IMVEC4(0x212121, 0xf2);
    UIStyle.PrimaryColor = HEX_TO_IMVEC4(0x292d35, 0xff);
    UIStyle.PrimaryLightColor = HEX_TO_IMVEC4(0xb5b5b5, 0xff);
    UIStyle.PrimaryDarkColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.PrimaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.DisabledColor = HEX_TO_IMVEC4(0x8d8d8d, 0xff);
    UIStyle.TextHighlightColor = HEX_TO_IMVEC4(0x676767, 0xff);
    UIStyle.PrimaryLineColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.SecondaryColor = HEX_TO_IMVEC4(0x969696, 0xff);
    UIStyle.SecondaryStrongColor = HEX_TO_IMVEC4(0x191919, 0xff);
    UIStyle.SecondaryWeakColor = HEX_TO_IMVEC4(0x2a2e36, 0xff);
    UIStyle.SecondaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ToastBackgroundColor = HEX_TO_IMVEC4(0x282828, 0xff);
    UIStyle.ToastTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ShadowColor = IM_COL32(0, 0, 0, 100);
    UIStyle.IsDarkTheme = true;
  }
  else if (theme == "PinkyPals")
  {
    UIStyle.BackgroundColor = HEX_TO_IMVEC4(0xd692a9, 0xff);
    UIStyle.BackgroundTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.BackgroundLineColor = HEX_TO_IMVEC4(0xe05885, 0xff);
    UIStyle.BackgroundHighlight = HEX_TO_IMVEC4(0xdc6c68, 0xff);
    UIStyle.PopupFrameBackgroundColor = HEX_TO_IMVEC4(0xe05885, 0xf2);
    UIStyle.PopupBackgroundColor = HEX_TO_IMVEC4(0xeba0b9, 0xf2);
    UIStyle.PrimaryColor = HEX_TO_IMVEC4(0xffaec9, 0xff);
    UIStyle.PrimaryLightColor = HEX_TO_IMVEC4(0xe05885, 0xff);
    UIStyle.PrimaryDarkColor = HEX_TO_IMVEC4(0xeba0b9, 0xff);
    UIStyle.PrimaryTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.DisabledColor = HEX_TO_IMVEC4(0x4b4b4b, 0xff);
    UIStyle.TextHighlightColor = HEX_TO_IMVEC4(0xeba0b9, 0xff);
    UIStyle.PrimaryLineColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.SecondaryColor = HEX_TO_IMVEC4(0xe05885, 0xff);
    UIStyle.SecondaryStrongColor = HEX_TO_IMVEC4(0xe05885, 0xff);
    UIStyle.SecondaryWeakColor = HEX_TO_IMVEC4(0xab5451, 0xff);
    UIStyle.SecondaryTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.ToastBackgroundColor = HEX_TO_IMVEC4(0xd86a66, 0xff);
    UIStyle.ToastTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ShadowColor = IM_COL32(100, 100, 100, 50);
    UIStyle.IsDarkTheme = false;
  }
  else if (theme == "GreenGiant")
  {
    UIStyle.BackgroundColor = HEX_TO_IMVEC4(0xB0C400, 0xff);
    UIStyle.BackgroundTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.BackgroundLineColor = HEX_TO_IMVEC4(0xf0f0f0, 0xff);
    UIStyle.BackgroundHighlight = HEX_TO_IMVEC4(0x876433, 0xff);
    UIStyle.PopupBackgroundColor = HEX_TO_IMVEC4(0xB0C400, 0xf2);
    UIStyle.PrimaryColor = HEX_TO_IMVEC4(0xD5DE2E, 0xff);
    UIStyle.PrimaryLightColor = HEX_TO_IMVEC4(0x795A2D, 0xff);
    UIStyle.PrimaryDarkColor = HEX_TO_IMVEC4(0x523213, 0xff);
    UIStyle.PrimaryTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.DisabledColor = HEX_TO_IMVEC4(0x878269, 0xff);
    UIStyle.TextHighlightColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.PrimaryLineColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.SecondaryColor = HEX_TO_IMVEC4(0x523213, 0xff);
    UIStyle.SecondaryStrongColor = HEX_TO_IMVEC4(0x523213, 0xff);
    UIStyle.SecondaryWeakColor = HEX_TO_IMVEC4(0x523213, 0xff);
    UIStyle.SecondaryTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.ToastBackgroundColor = HEX_TO_IMVEC4(0xD5DE2E, 0xff);
    UIStyle.ToastTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.ShadowColor = IM_COL32(100, 100, 100, 50);
    UIStyle.IsDarkTheme = false;
  }
  else if (theme == "DarkRuby")
  {
    UIStyle.BackgroundColor = HEX_TO_IMVEC4(0x1b1b1b, 0xff);
    UIStyle.BackgroundTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.BackgroundLineColor = HEX_TO_IMVEC4(0xf0f0f0, 0xff);
    UIStyle.BackgroundHighlight = HEX_TO_IMVEC4(0xab2720, 0xff);
    UIStyle.PopupFrameBackgroundColor = HEX_TO_IMVEC4(0x313131, 0xf2);
    UIStyle.PopupBackgroundColor = HEX_TO_IMVEC4(0x212121, 0xf2);
    UIStyle.PrimaryColor = HEX_TO_IMVEC4(0x121212, 0xff);
    UIStyle.PrimaryLightColor = HEX_TO_IMVEC4(0xb5b5b5, 0xff);
    UIStyle.PrimaryDarkColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.PrimaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.DisabledColor = HEX_TO_IMVEC4(0x8d8d8d, 0xff);
    UIStyle.TextHighlightColor = HEX_TO_IMVEC4(0x676767, 0xff);
    UIStyle.PrimaryLineColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.SecondaryColor = HEX_TO_IMVEC4(0x969696, 0xff);
    UIStyle.SecondaryStrongColor = HEX_TO_IMVEC4(0xdc143c, 0xff);
    UIStyle.SecondaryWeakColor = HEX_TO_IMVEC4(0x2a2e36, 0xff);
    UIStyle.SecondaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ToastBackgroundColor = HEX_TO_IMVEC4(0x282828, 0xff);
    UIStyle.ToastTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ShadowColor = IM_COL32(0, 0, 0, 100);
    UIStyle.IsDarkTheme = true;
  }
  else if (theme == "PurpleRain")
  {
    UIStyle.BackgroundColor = HEX_TO_IMVEC4(0x341d56, 0xff);
    UIStyle.BackgroundTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.BackgroundLineColor = HEX_TO_IMVEC4(0xa78936, 0xff);
    UIStyle.BackgroundHighlight = HEX_TO_IMVEC4(0xa78936, 0xff);
    UIStyle.PopupFrameBackgroundColor = HEX_TO_IMVEC4(0x341d56, 0xf2);
    UIStyle.PopupBackgroundColor = HEX_TO_IMVEC4(0x532f8a, 0xf2);
    UIStyle.PrimaryColor = HEX_TO_IMVEC4(0x49297a, 0xff);
    UIStyle.PrimaryLightColor = HEX_TO_IMVEC4(0x653aab, 0xff);
    UIStyle.PrimaryDarkColor = HEX_TO_IMVEC4(0x462876, 0xff);
    UIStyle.PrimaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.DisabledColor = HEX_TO_IMVEC4(0x8d8d8d, 0xff);
    UIStyle.TextHighlightColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.PrimaryLineColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.SecondaryColor = HEX_TO_IMVEC4(0x523a74, 0xff);
    UIStyle.SecondaryStrongColor = HEX_TO_IMVEC4(0x8d65ca, 0xff);
    UIStyle.SecondaryWeakColor = HEX_TO_IMVEC4(0xab5451, 0xff);
    UIStyle.SecondaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ToastBackgroundColor = HEX_TO_IMVEC4(0x8e65cb, 0xff);
    UIStyle.ToastTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ShadowColor = IM_COL32(100, 100, 100, 50);
    UIStyle.IsDarkTheme = true;
  }
  else if (theme == "Light")
  {
    // light
    UIStyle.BackgroundColor = HEX_TO_IMVEC4(0xc8c8c8, 0xff);
    UIStyle.BackgroundTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.BackgroundLineColor = HEX_TO_IMVEC4(0xe1e2e1, 0xff);
    UIStyle.BackgroundHighlight = HEX_TO_IMVEC4(0xe1e2e1, 0xc0);
    UIStyle.PopupBackgroundColor = HEX_TO_IMVEC4(0xd8d8d8, 0xf2);
    UIStyle.PopupFrameBackgroundColor = HEX_TO_IMVEC4(0xc8c8c8, 0xf2);
    UIStyle.PrimaryColor = HEX_TO_IMVEC4(0x2a3e78, 0xff);
    UIStyle.PrimaryLightColor = HEX_TO_IMVEC4(0x235cd9, 0xff);
    UIStyle.PrimaryDarkColor = HEX_TO_IMVEC4(0x1d2953, 0xff);
    UIStyle.PrimaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.DisabledColor = HEX_TO_IMVEC4(0x444444, 0xff);
    UIStyle.TextHighlightColor = HEX_TO_IMVEC4(0x8e8e8e, 0xff);
    UIStyle.PrimaryLineColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.SecondaryColor = HEX_TO_IMVEC4(0x2a3e78, 0xff);
    UIStyle.SecondaryStrongColor = HEX_TO_IMVEC4(0x464db1, 0xff);
    UIStyle.SecondaryWeakColor = HEX_TO_IMVEC4(0xc0cfff, 0xff);
    UIStyle.SecondaryTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.ToastBackgroundColor = HEX_TO_IMVEC4(0xf1f1f1, 0xff);
    UIStyle.ToastTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.ShadowColor = IM_COL32(100, 100, 100, 50);
    UIStyle.IsDarkTheme = false;
  }
  else
  {
    // dark
    UIStyle.BackgroundColor = HEX_TO_IMVEC4(0x212121, 0xff);
    UIStyle.BackgroundTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.BackgroundLineColor = HEX_TO_IMVEC4(0xaaaaaa, 0xff);
    UIStyle.BackgroundHighlight = HEX_TO_IMVEC4(0x4b4b4b, 0xc0);
    UIStyle.PopupBackgroundColor = HEX_TO_IMVEC4(0x212121, 0xf2);
    UIStyle.PopupFrameBackgroundColor = HEX_TO_IMVEC4(0x313131, 0xf2);
    UIStyle.PrimaryColor = HEX_TO_IMVEC4(0x2e2e2e, 0xff);
    UIStyle.PrimaryLightColor = HEX_TO_IMVEC4(0x484848, 0xff);
    UIStyle.PrimaryDarkColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.PrimaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.DisabledColor = HEX_TO_IMVEC4(0xaaaaaa, 0xff);
    UIStyle.TextHighlightColor = HEX_TO_IMVEC4(0x90caf9, 0xff);
    UIStyle.PrimaryLineColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.SecondaryColor = HEX_TO_IMVEC4(0x0d47a1, 0xff);
    UIStyle.SecondaryStrongColor = HEX_TO_IMVEC4(0x63a4ff, 0xff);
    UIStyle.SecondaryWeakColor = HEX_TO_IMVEC4(0x002171, 0xff);
    UIStyle.SecondaryTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ToastBackgroundColor = HEX_TO_IMVEC4(0x282828, 0xff);
    UIStyle.ToastTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.ShadowColor = IM_COL32(0, 0, 0, 100);
    UIStyle.IsDarkTheme = true;
  }
}
