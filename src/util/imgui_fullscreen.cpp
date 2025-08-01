// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "imgui_fullscreen.h"
#include "gpu_device.h"
#include "image.h"
#include "imgui_animated.h"
#include "imgui_manager.h"

#include "common/assert.h"
#include "common/easing.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/lru_cache.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/threading.h"
#include "common/timer.h"

#include "core/fullscreen_ui.h" // For updating run idle state.
#include "core/gpu_thread.h"    // For kicking to GPU thread.
#include "core/host.h"
#include "core/system.h" // For async workers, should be in general host.

#include "fmt/core.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome6.h"
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

LOG_CHANNEL(ImGuiFullscreen);

namespace ImGuiFullscreen {

static constexpr float MENU_BACKGROUND_ANIMATION_TIME = 0.5f;
static constexpr float MENU_ITEM_BORDER_ROUNDING = 10.0f;
static constexpr float SMOOTH_SCROLLING_SPEED = 3.5f;

static std::optional<Image> LoadTextureImage(std::string_view path, u32 svg_width, u32 svg_height);
static std::shared_ptr<GPUTexture> UploadTexture(std::string_view path, const Image& image);

static void DrawBackgroundProgressDialogs(ImVec2& position, float spacing);
static void DrawLoadingScreen(std::string_view image, std::string_view message, s32 progress_min, s32 progress_max,
                              s32 progress_value, bool is_persistent);
static void DrawNotifications(ImVec2& position, float spacing);
static void DrawToast();
static ImGuiID GetBackgroundProgressID(std::string_view str_id);

namespace {

enum class CloseButtonState
{
  None,
  KeyboardPressed,
  MousePressed,
  GamepadPressed,
  AnyReleased,
  Cancelled,
};

struct Notification
{
  std::string key;
  std::string title;
  std::string text;
  std::string badge_path;
  Timer::Value start_time;
  Timer::Value move_time;
  float duration;
  float target_y;
  float last_y;
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

  void Open(std::string_view title, std::string message, CallbackVariant callback, std::string first_button_text,
            std::string second_button_text, std::string third_button_text);
  void ClearState();

  void Draw();

private:
  static void InvokeCallback(const CallbackVariant& cb, std::optional<s32> choice);

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

  std::unique_ptr<ProgressCallback> GetProgressCallback(std::string title, float window_unscaled_width);

  void Draw();

private:
  class ProgressCallbackImpl : public ProgressCallback
  {
  public:
    ProgressCallbackImpl();
    ~ProgressCallbackImpl() override;

    void SetStatusText(std::string_view text) override;
    void SetProgressRange(u32 range) override;
    void SetProgressValue(u32 value) override;
    void SetCancellable(bool cancellable) override;
    bool IsCancelled() const override;
  };

  std::string m_status_text;
  float m_last_frac = 0.0f;
  float m_width = 0.0f;
  u32 m_progress_value = 0;
  u32 m_progress_range = 0;
  std::atomic_bool m_cancelled{false};
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

struct ALIGN_TO_CACHE_LINE UIState
{
  std::recursive_mutex shared_state_mutex;

  CloseButtonState close_button_state = CloseButtonState::None;
  ImGuiDir has_pending_nav_move = ImGuiDir_None;
  FocusResetType focus_reset_queued = FocusResetType::None;
  bool initialized = false;

  u32 menu_button_index = 0;
  ImVec2 horizontal_menu_button_size = {};

  LRUCache<std::string, std::shared_ptr<GPUTexture>> texture_cache{128, true};
  std::shared_ptr<GPUTexture> placeholder_texture;
  std::deque<std::pair<std::string, Image>> texture_upload_queue;

  SmallString fullscreen_footer_text;
  SmallString last_fullscreen_footer_text;
  SmallString left_fullscreen_footer_text;
  SmallString last_left_fullscreen_footer_text;
  std::vector<std::pair<std::string_view, std::string_view>> fullscreen_footer_icon_mapping;
  float fullscreen_text_change_time;
  float fullscreen_text_alpha;

  ImGuiID enum_choice_button_id = 0;
  s32 enum_choice_button_value = 0;
  bool enum_choice_button_set = false;

  MessageDialog message_dialog;
  ChoiceDialog choice_dialog;
  FileSelectorDialog file_selector_dialog;
  InputStringDialog input_string_dialog;
  FixedPopupDialog fixed_popup_dialog;
  ProgressDialog progress_dialog;

  ImAnimatedVec2 menu_button_frame_min_animated;
  ImAnimatedVec2 menu_button_frame_max_animated;
  bool had_hovered_menu_item = false;
  bool has_hovered_menu_item = false;
  bool rendered_menu_item_border = false;

  std::vector<Notification> notifications;

  std::string toast_title;
  std::string toast_message;
  Timer::Value toast_start_time;
  float toast_duration;

  std::vector<BackgroundProgressDialogData> background_progress_dialogs;

  std::string loading_screen_image;
  std::string loading_screen_message;
  s32 loading_screen_min = 0;
  s32 loading_screen_max = 0;
  s32 loading_screen_value = 0;
  bool loading_screen_open = false;
};

} // namespace

UIStyles UIStyle = {};
static UIState s_state;

} // namespace ImGuiFullscreen

void ImGuiFullscreen::SetFont(ImFont* ui_font)
{
  UIStyle.Font = ui_font;
}

bool ImGuiFullscreen::Initialize(const char* placeholder_image_path)
{
  std::unique_lock lock(s_state.shared_state_mutex);

  s_state.focus_reset_queued = FocusResetType::ViewChanged;
  s_state.close_button_state = CloseButtonState::None;

  s_state.placeholder_texture = LoadTexture(placeholder_image_path);
  if (!s_state.placeholder_texture)
  {
    ERROR_LOG("Missing placeholder texture '{}', cannot continue", placeholder_image_path);
    return false;
  }

  s_state.initialized = true;
  ResetMenuButtonFrame();
  return true;
}

void ImGuiFullscreen::Shutdown(bool clear_state)
{
  std::unique_lock lock(s_state.shared_state_mutex);
  s_state.initialized = false;
  s_state.texture_upload_queue.clear();
  s_state.placeholder_texture.reset();
  UIStyle.Font = nullptr;

  s_state.texture_cache.Clear();

  if (clear_state)
  {
    s_state.fullscreen_footer_icon_mapping = {};
    s_state.notifications.clear();
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
}

void ImGuiFullscreen::SetAnimations(bool enabled)
{
  UIStyle.Animations = enabled;
}

void ImGuiFullscreen::SetSmoothScrolling(bool enabled)
{
  UIStyle.SmoothScrolling = enabled;
}

void ImGuiFullscreen::SetMenuBorders(bool enabled)
{
  UIStyle.MenuBorders = enabled;
}

const std::shared_ptr<GPUTexture>& ImGuiFullscreen::GetPlaceholderTexture()
{
  return s_state.placeholder_texture;
}

std::optional<Image> ImGuiFullscreen::LoadTextureImage(std::string_view path, u32 svg_width, u32 svg_height)
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
      if (!image->RasterizeSVG(svg_data->cspan(), svg_width, svg_height))
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

std::shared_ptr<GPUTexture> ImGuiFullscreen::UploadTexture(std::string_view path, const Image& image)
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

std::shared_ptr<GPUTexture> ImGuiFullscreen::LoadTexture(std::string_view path, u32 width_hint, u32 height_hint)
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

GPUTexture* ImGuiFullscreen::FindCachedTexture(std::string_view name)
{
  std::shared_ptr<GPUTexture>* tex_ptr = s_state.texture_cache.Lookup(name);
  return tex_ptr ? tex_ptr->get() : nullptr;
}

GPUTexture* ImGuiFullscreen::FindCachedTexture(std::string_view name, u32 svg_width, u32 svg_height)
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

GPUTexture* ImGuiFullscreen::GetCachedTexture(std::string_view name)
{
  std::shared_ptr<GPUTexture>* tex_ptr = s_state.texture_cache.Lookup(name);
  if (!tex_ptr)
  {
    std::shared_ptr<GPUTexture> tex = LoadTexture(name);
    tex_ptr = s_state.texture_cache.Insert(std::string(name), std::move(tex));
  }

  return tex_ptr->get();
}

GPUTexture* ImGuiFullscreen::GetCachedTexture(std::string_view name, u32 svg_width, u32 svg_height)
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

GPUTexture* ImGuiFullscreen::GetCachedTextureAsync(std::string_view name)
{
  std::shared_ptr<GPUTexture>* tex_ptr = s_state.texture_cache.Lookup(name);
  if (!tex_ptr)
  {
    // insert the placeholder
    tex_ptr = s_state.texture_cache.Insert(std::string(name), s_state.placeholder_texture);

    // queue the actual load
    System::QueueAsyncTask([path = std::string(name)]() mutable {
      std::optional<Image> image(LoadTextureImage(path.c_str(), 0, 0));

      // don't bother queuing back if it doesn't exist
      if (!image.has_value())
        return;

      std::unique_lock lock(s_state.shared_state_mutex);
      if (s_state.initialized)
        s_state.texture_upload_queue.emplace_back(std::move(path), std::move(image.value()));
    });
  }

  return tex_ptr->get();
}

GPUTexture* ImGuiFullscreen::GetCachedTextureAsync(std::string_view name, u32 svg_width, u32 svg_height)
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
    System::QueueAsyncTask([path = std::string(name), wh_name = std::string(wh_name), svg_width, svg_height]() mutable {
      std::optional<Image> image(LoadTextureImage(path.c_str(), svg_width, svg_height));

      // don't bother queuing back if it doesn't exist
      if (!image.has_value())
        return;

      std::unique_lock lock(s_state.shared_state_mutex);
      if (s_state.initialized)
        s_state.texture_upload_queue.emplace_back(std::move(wh_name), std::move(image.value()));
    });
  }

  return tex_ptr->get();
}

bool ImGuiFullscreen::InvalidateCachedTexture(std::string_view path)
{
  // need to do a partial match on this because SVG
  return (s_state.texture_cache.RemoveMatchingItems([&path](const std::string& key) { return key.starts_with(path); }) >
          0);
}

bool ImGuiFullscreen::TextureNeedsSVGDimensions(std::string_view path)
{
  return StringUtil::EndsWithNoCase(Path::GetExtension(path), "svg");
}

void ImGuiFullscreen::UploadAsyncTextures()
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

bool ImGuiFullscreen::UpdateLayoutScale()
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
  return (UIStyle.LayoutScale != old_scale);

#endif
}

ImGuiFullscreen::IconStackString::IconStackString(std::string_view icon, std::string_view str)
{
  SmallStackString::format("{} {}", icon, Host::TranslateToStringView(FSUI_TR_CONTEXT, str));
}

ImGuiFullscreen::IconStackString::IconStackString(std::string_view icon, std::string_view str, std::string_view suffix)
{
  SmallStackString::format("{} {}##{}", icon, Host::TranslateToStringView(FSUI_TR_CONTEXT, str), suffix);
}

ImRect ImGuiFullscreen::CenterImage(const ImVec2& fit_size, const ImVec2& image_size)
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

ImRect ImGuiFullscreen::CenterImage(const ImVec2& fit_rect, const GPUTexture* texture)
{
  const GSVector2 texture_size = GSVector2(texture->GetSizeVec());
  return CenterImage(fit_rect, ImVec2(texture_size.x, texture_size.y));
}

ImRect ImGuiFullscreen::CenterImage(const ImRect& fit_rect, const ImVec2& image_size)
{
  ImRect ret(CenterImage(fit_rect.Max - fit_rect.Min, image_size));
  ret.Translate(fit_rect.Min);
  return ret;
}

ImRect ImGuiFullscreen::CenterImage(const ImRect& fit_rect, const GPUTexture* texture)
{
  const GSVector2 texture_size = GSVector2(texture->GetSizeVec());
  return CenterImage(fit_rect, ImVec2(texture_size.x, texture_size.y));
}

ImRect ImGuiFullscreen::FitImage(const ImVec2& fit_size, const ImVec2& image_size)
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

void ImGuiFullscreen::BeginLayout()
{
  // we evict from the texture cache at the start of the frame, in case we go over mid-frame,
  // we need to keep all those textures alive until the end of the frame
  s_state.texture_cache.ManualEvict();
  PushResetLayout();
}

void ImGuiFullscreen::EndLayout()
{
  s_state.message_dialog.Draw();
  s_state.choice_dialog.Draw();
  s_state.file_selector_dialog.Draw();
  s_state.input_string_dialog.Draw();
  s_state.progress_dialog.Draw();

  DrawFullscreenFooter();

  PopResetLayout();

  s_state.left_fullscreen_footer_text.clear();
  s_state.fullscreen_footer_text.clear();

  s_state.rendered_menu_item_border = false;
  s_state.had_hovered_menu_item = std::exchange(s_state.has_hovered_menu_item, false);
}

ImGuiFullscreen::FixedPopupDialog::FixedPopupDialog() = default;

ImGuiFullscreen::FixedPopupDialog::~FixedPopupDialog() = default;

void ImGuiFullscreen::FixedPopupDialog::Open(std::string title)
{
  SetTitleAndOpen(std::move(title));
}

bool ImGuiFullscreen::FixedPopupDialog::Begin(float scaled_window_padding /* = LayoutScale(20.0f) */,
                                              float scaled_window_rounding /* = LayoutScale(20.0f) */,
                                              const ImVec2& scaled_window_size /* = ImVec2(0.0f, 0.0f) */)
{
  return BeginRender(scaled_window_padding, scaled_window_rounding, scaled_window_size);
}

void ImGuiFullscreen::FixedPopupDialog::End()
{
  EndRender();
}

bool ImGuiFullscreen::IsAnyFixedPopupDialogOpen()
{
  return s_state.fixed_popup_dialog.IsOpen();
}

bool ImGuiFullscreen::IsFixedPopupDialogOpen(std::string_view name)
{
  return (s_state.fixed_popup_dialog.GetTitle() == name);
}

void ImGuiFullscreen::OpenFixedPopupDialog(std::string_view name)
{
  // TODO: suffix?
  s_state.fixed_popup_dialog.Open(std::string(name));
}

void ImGuiFullscreen::CloseFixedPopupDialog()
{
  s_state.fixed_popup_dialog.StartClose();
}

void ImGuiFullscreen::CloseFixedPopupDialogImmediately()
{
  s_state.fixed_popup_dialog.CloseImmediately();
}

bool ImGuiFullscreen::BeginFixedPopupDialog(float scaled_window_padding /* = LayoutScale(20.0f) */,
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

void ImGuiFullscreen::EndFixedPopupDialog()
{
  s_state.fixed_popup_dialog.End();
}

void ImGuiFullscreen::RenderOverlays()
{
  if (!s_state.initialized)
    return;

  const float margin = std::max(ImGuiManager::GetScreenMargin(), LayoutScale(10.0f));
  const float spacing = LayoutScale(10.0f);
  const float notification_vertical_pos = GetNotificationVerticalPosition();
  ImVec2 position(margin, notification_vertical_pos * ImGui::GetIO().DisplaySize.y +
                            ((notification_vertical_pos >= 0.5f) ? -margin : margin));
  DrawBackgroundProgressDialogs(position, spacing);
  DrawNotifications(position, spacing);
  DrawToast();
}

void ImGuiFullscreen::PushResetLayout()
{
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(8.0f, 8.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(4.0f, 3.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, LayoutScale(8.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, LayoutScale(4.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, LayoutScale(4.0f, 2.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, LayoutScale(21.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, LayoutScale(14.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, LayoutScale(4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollSmooth, UIStyle.SmoothScrolling ? SMOOTH_SCROLLING_SPEED : 1.0f);
  ImGui::PushStyleVar(
    ImGuiStyleVar_ScrollStepSize,
    ImVec2(LayoutScale(LAYOUT_LARGE_FONT_SIZE), MenuButtonBounds::GetSingleLineHeight(LAYOUT_MENU_BUTTON_Y_PADDING)));
  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.SecondaryTextColor);
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, UIStyle.DisabledColor);
  ImGui::PushStyleColor(ImGuiCol_Button, UIStyle.SecondaryColor);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, UIStyle.BackgroundColor);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIStyle.BackgroundHighlight);
  ImGui::PushStyleColor(ImGuiCol_Border, UIStyle.BackgroundLineColor);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, UIStyle.BackgroundColor);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, UIStyle.PrimaryColor);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, UIStyle.PrimaryLightColor);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, UIStyle.PrimaryDarkColor);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, UIStyle.PopupBackgroundColor);
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));
}

void ImGuiFullscreen::PopResetLayout()
{
  ImGui::PopStyleColor(12);
  ImGui::PopStyleVar(14);
}

void ImGuiFullscreen::QueueResetFocus(FocusResetType type)
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

bool ImGuiFullscreen::ResetFocusHere()
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
  if (s_state.focus_reset_queued != FocusResetType::PopupClosed)
    ImGui::NavInitWindow(window, true);
  else
    ImGui::SetNavWindow(window);

  s_state.focus_reset_queued = FocusResetType::None;
  ResetMenuButtonFrame();

  // only do the active selection magic when we're using keyboard/gamepad
  return (GImGui->NavInputSource == ImGuiInputSource_Keyboard || GImGui->NavInputSource == ImGuiInputSource_Gamepad);
}

bool ImGuiFullscreen::IsFocusResetQueued()
{
  return (s_state.focus_reset_queued != FocusResetType::None);
}

bool ImGuiFullscreen::IsFocusResetFromWindowChange()
{
  return (s_state.focus_reset_queued != FocusResetType::None &&
          s_state.focus_reset_queued != FocusResetType::PopupOpened &&
          s_state.focus_reset_queued != FocusResetType::PopupClosed);
}

ImGuiFullscreen::FocusResetType ImGuiFullscreen::GetQueuedFocusResetType()
{
  return s_state.focus_reset_queued;
}

void ImGuiFullscreen::ForceKeyNavEnabled()
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

bool ImGuiFullscreen::WantsToCloseMenu()
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
           (s_state.close_button_state == CloseButtonState::MousePressed &&
            ImGui::IsKeyReleased(ImGuiKey_MouseRight)) ||
           (s_state.close_button_state == CloseButtonState::GamepadPressed &&
            ImGui::IsKeyReleased(ImGuiKey_NavGamepadCancel)))
  {
    s_state.close_button_state = CloseButtonState::AnyReleased;
  }
  return (s_state.close_button_state == CloseButtonState::AnyReleased);
}

void ImGuiFullscreen::ResetCloseMenuIfNeeded()
{
  // If s_close_button_state reached the "Released" state, reset it after the tick
  s_state.close_button_state =
    (s_state.close_button_state >= CloseButtonState::AnyReleased) ? CloseButtonState::None : s_state.close_button_state;
}

void ImGuiFullscreen::CancelPendingMenuClose()
{
  s_state.close_button_state = CloseButtonState::Cancelled;
}

void ImGuiFullscreen::PushPrimaryColor()
{
  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.PrimaryTextColor);
  ImGui::PushStyleColor(ImGuiCol_Button, UIStyle.PrimaryDarkColor);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, UIStyle.PrimaryColor);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIStyle.PrimaryLightColor);
  ImGui::PushStyleColor(ImGuiCol_Border, UIStyle.PrimaryLightColor);
}

void ImGuiFullscreen::PopPrimaryColor()
{
  ImGui::PopStyleColor(5);
}

bool ImGuiFullscreen::BeginFullscreenColumns(const char* title, float pos_y, bool expand_to_screen_width, bool footer)
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

void ImGuiFullscreen::EndFullscreenColumns()
{
  ImGui::End();
  ImGui::PopStyleVar(3);
}

bool ImGuiFullscreen::BeginFullscreenColumnWindow(float start, float end, const char* name, const ImVec4& background,
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
                           (padding.x != 0.0f || padding.y != 0.0f) ? ImGuiChildFlags_AlwaysUseWindowPadding : 0,
                           ImGuiChildFlags_NavFlattened);
}

void ImGuiFullscreen::EndFullscreenColumnWindow()
{
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

bool ImGuiFullscreen::BeginFullscreenWindow(float left, float top, float width, float height, const char* name,
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

bool ImGuiFullscreen::BeginFullscreenWindow(const ImVec2& position, const ImVec2& size, const char* name,
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

void ImGuiFullscreen::EndFullscreenWindow(bool allow_wrap_x, bool allow_wrap_y)
{
  if (allow_wrap_x || allow_wrap_y)
    SetWindowNavWrapping(allow_wrap_x, allow_wrap_y);

  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();
}

void ImGuiFullscreen::SetWindowNavWrapping(bool allow_wrap_x /*= false*/, bool allow_wrap_y /*= true*/)
{
  DebugAssert(allow_wrap_x || allow_wrap_y);
  if (ImGuiWindow* const win = ImGui::GetCurrentWindowRead(); GImGui->NavWindow == win)
  {
    ImGui::NavMoveRequestTryWrapping(win, (allow_wrap_x ? ImGuiNavMoveFlags_LoopX : 0) |
                                            (allow_wrap_y ? ImGuiNavMoveFlags_LoopY : 0));
  }
}

bool ImGuiFullscreen::IsGamepadInputSource()
{
  return (ImGui::GetCurrentContext()->NavInputSource == ImGuiInputSource_Gamepad);
}

std::string_view ImGuiFullscreen::GetControllerIconMapping(std::string_view icon)
{
  const auto iter =
    std::lower_bound(s_state.fullscreen_footer_icon_mapping.begin(), s_state.fullscreen_footer_icon_mapping.end(), icon,
                     [](const auto& it, const auto& value) { return (it.first < value); });
  if (iter != s_state.fullscreen_footer_icon_mapping.end() && iter->first == icon)
    icon = iter->second;
  return icon;
}

void ImGuiFullscreen::CreateFooterTextString(SmallStringBase& dest,
                                             std::span<const std::pair<const char*, std::string_view>> items)
{
  dest.clear();
  for (const auto& [icon, text] : items)
  {
    if (!dest.empty())
      dest.append("    ");

    dest.append(GetControllerIconMapping(icon));
    dest.append(' ');
    dest.append(text);
  }
}

void ImGuiFullscreen::SetFullscreenFooterText(std::string_view text, float background_alpha)
{
  s_state.fullscreen_footer_text.assign(text);
  s_state.fullscreen_text_alpha = background_alpha;
}

void ImGuiFullscreen::SetFullscreenFooterText(std::span<const std::pair<const char*, std::string_view>> items,
                                              float background_alpha)
{
  CreateFooterTextString(s_state.fullscreen_footer_text, items);
  s_state.fullscreen_text_alpha = background_alpha;
}

void ImGuiFullscreen::SetFullscreenStatusText(std::string_view text)
{
  s_state.left_fullscreen_footer_text = text;
}

void ImGuiFullscreen::SetFullscreenStatusText(std::span<const std::pair<const char*, std::string_view>> items)
{
  CreateFooterTextString(s_state.left_fullscreen_footer_text, items);
}

void ImGuiFullscreen::SetFullscreenFooterTextIconMapping(std::span<const std::pair<const char*, const char*>> mapping)
{
  if (mapping.empty())
  {
    s_state.fullscreen_footer_icon_mapping = {};
    return;
  }

  s_state.fullscreen_footer_icon_mapping.reserve(mapping.size());
  for (const auto& [icon, mapped_icon] : mapping)
    s_state.fullscreen_footer_icon_mapping.emplace_back(icon, mapped_icon);
  std::sort(s_state.fullscreen_footer_icon_mapping.begin(), s_state.fullscreen_footer_icon_mapping.end(),
            [](const auto& lhs, const auto& rhs) { return (lhs.first < rhs.first); });
}

void ImGuiFullscreen::DrawFullscreenFooter()
{
  const ImGuiIO& io = ImGui::GetIO();
  if (s_state.fullscreen_footer_text.empty() && s_state.left_fullscreen_footer_text.empty())
  {
    s_state.last_fullscreen_footer_text.clear();
    s_state.last_left_fullscreen_footer_text.clear();
    return;
  }

  static constexpr float TRANSITION_TIME = 0.15f;

  const float padding = LayoutScale(LAYOUT_FOOTER_PADDING);
  const float height = LayoutScale(LAYOUT_FOOTER_HEIGHT);
  const ImVec2 shadow_offset = LayoutScale(LAYOUT_SHADOW_OFFSET, LAYOUT_SHADOW_OFFSET);
  const u32 text_color = ImGui::GetColorU32(UIStyle.PrimaryTextColor);

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0.0f, io.DisplaySize.y - height), io.DisplaySize,
                    ImGui::GetColorU32(ModAlpha(UIStyle.PrimaryColor, s_state.fullscreen_text_alpha)), 0.0f);

  ImFont* const font = UIStyle.Font;
  const float font_size = UIStyle.MediumFontSize;
  const float font_weight = UIStyle.BoldFontWeight;
  const float max_width = io.DisplaySize.x - padding * 2.0f;

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
          ImVec2(io.DisplaySize.x - padding * 2.0f - text_size.x, io.DisplaySize.y - font_size - padding);
        dl->AddText(font, font_size, font_weight, text_pos + shadow_offset, MulAlpha(UIStyle.ShadowColor, prev_opacity),
                    IMSTR_START_END(s_state.last_fullscreen_footer_text));
        dl->AddText(font, font_size, font_weight, text_pos, ModAlpha(text_color, prev_opacity),
                    IMSTR_START_END(s_state.last_fullscreen_footer_text));
      }

      if (!s_state.last_left_fullscreen_footer_text.empty())
      {
        const ImVec2 text_pos = ImVec2(padding, io.DisplaySize.y - font_size - padding);
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
    if (!s_state.fullscreen_footer_text.empty())
    {
      const ImVec2 text_size =
        font->CalcTextSizeA(font_size, font_weight, max_width, 0.0f, IMSTR_START_END(s_state.fullscreen_footer_text));
      const ImVec2 text_pos =
        ImVec2(io.DisplaySize.x - padding * 2.0f - text_size.x, io.DisplaySize.y - font_size - padding);
      const float opacity = 1.0f - prev_opacity;
      dl->AddText(font, font_size, font_weight, text_pos + shadow_offset, MulAlpha(UIStyle.ShadowColor, opacity),
                  IMSTR_START_END(s_state.fullscreen_footer_text));
      dl->AddText(font, font_size, font_weight, text_pos, ModAlpha(text_color, opacity),
                  IMSTR_START_END(s_state.fullscreen_footer_text));
    }

    if (!s_state.left_fullscreen_footer_text.empty())
    {
      const ImVec2 text_pos = ImVec2(padding, io.DisplaySize.y - font_size - padding);
      const float opacity = 1.0f - prev_opacity;
      dl->AddText(font, font_size, font_weight, text_pos + shadow_offset, MulAlpha(UIStyle.ShadowColor, opacity),
                  IMSTR_START_END(s_state.left_fullscreen_footer_text));
      dl->AddText(font, font_size, font_weight, text_pos, ModAlpha(text_color, opacity),
                  IMSTR_START_END(s_state.left_fullscreen_footer_text));
    }
  }

  // for next frame
  s_state.fullscreen_text_alpha = 1.0f;
}

void ImGuiFullscreen::PrerenderMenuButtonBorder()
{
  if (!s_state.had_hovered_menu_item || GImGui->CurrentWindow != GImGui->NavWindow)
    return;

  // updating might finish the animation
  const ImVec2& min = s_state.menu_button_frame_min_animated.UpdateAndGetValue();
  const ImVec2& max = s_state.menu_button_frame_max_animated.UpdateAndGetValue();
  const ImU32 col = ImGui::GetColorU32(ImGuiCol_ButtonHovered);

  const float t = static_cast<float>(std::min(std::abs(std::sin(ImGui::GetTime() * 0.75) * 1.1), 1.0));
  ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Border, t));

  ImGui::RenderFrame(min, max, col, true, LayoutScale(MENU_ITEM_BORDER_ROUNDING));

  ImGui::PopStyleColor();

  s_state.rendered_menu_item_border = true;
}

void ImGuiFullscreen::BeginMenuButtons(u32 num_items /* = 0 */, float y_align /* = 0.0f */,
                                       float x_padding /* = LAYOUT_MENU_BUTTON_X_PADDING */,
                                       float y_padding /* = LAYOUT_MENU_BUTTON_Y_PADDING */,
                                       float x_spacing /* = 0.0f */, float y_spacing /* = LAYOUT_MENU_BUTTON_SPACING */,
                                       bool prerender_frame /*= true*/)
{
  s_state.menu_button_index = 0;

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

  if (prerender_frame)
    PrerenderMenuButtonBorder();
}

void ImGuiFullscreen::EndMenuButtons()
{
  ImGui::PopStyleVar(4);
}

float ImGuiFullscreen::GetMenuButtonAvailableWidth()
{
  return MenuButtonBounds::CalcAvailWidth();
}

bool ImGuiFullscreen::MenuButtonFrame(std::string_view str_id, float height, bool enabled, ImRect* item_bb,
                                      bool* visible, bool* hovered, ImGuiButtonFlags flags /*= 0*/,
                                      float alpha /*= 1.0f*/)
{
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float avail_width = MenuButtonBounds::CalcAvailWidth();
  const ImGuiStyle& style = ImGui::GetStyle();

  *item_bb = ImRect(pos + style.FramePadding, pos + style.FramePadding + ImVec2(avail_width, height));
  const ImRect frame_bb = ImRect(pos, pos + style.FramePadding * 2.0f + ImVec2(avail_width, height));
  return MenuButtonFrame(str_id, enabled, frame_bb, visible, hovered, 0, alpha);
}

void ImGuiFullscreen::DrawWindowTitle(std::string_view title)
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

bool ImGuiFullscreen::MenuButtonFrame(std::string_view str_id, bool enabled, const ImRect& bb, bool* visible,
                                      bool* hovered, ImGuiButtonFlags flags, float hover_alpha)
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
    {
      const ImU32 col = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered, hover_alpha);

      const float t = static_cast<float>(std::min(std::abs(std::sin(ImGui::GetTime() * 0.75) * 1.1), 1.0));
      ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Border, t));

      DrawMenuButtonFrame(bb.Min, bb.Max, col, true);

      ImGui::PopStyleColor();
    }
  }
  else
  {
    pressed = false;
    held = false;
  }

  return pressed;
}

void ImGuiFullscreen::DrawMenuButtonFrame(const ImVec2& p_min, const ImVec2& p_max, ImU32 fill_col,
                                          bool border /* = true */)
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
    ImGui::RenderFrame(frame_min, frame_max, fill_col, border, LayoutScale(MENU_ITEM_BORDER_ROUNDING));
  }
}

float ImGuiFullscreen::MenuButtonBounds::CalcAvailWidth()
{
  return ImGui::GetCurrentWindowRead()->WorkRect.GetWidth() - ImGui::GetStyle().FramePadding.x * 2.0f;
}

void ImGuiFullscreen::MenuButtonBounds::CalcValueSize(const std::string_view& value, float font_size)
{
  SetValueSize(value.empty() ? ImVec2() :
                               UIStyle.Font->CalcTextSizeA(font_size, UIStyle.BoldFontWeight, FLT_MAX,
                                                           available_width * 0.5f, IMSTR_START_END(value)));
}

void ImGuiFullscreen::MenuButtonBounds::SetValueSize(const ImVec2& size)
{
  value_size = size;
  available_non_value_width = available_width - ((size.x > 0.0f) ? (size.x + LayoutScale(16.0f)) : 0.0f);
}

void ImGuiFullscreen::MenuButtonBounds::CalcTitleSize(const std::string_view& title, float font_size)
{
  const std::string_view real_title = ImGuiFullscreen::RemoveHash(title);
  title_size = real_title.empty() ? ImVec2() :
                                    UIStyle.Font->CalcTextSizeA(font_size, UIStyle.BoldFontWeight, FLT_MAX,
                                                                available_non_value_width, IMSTR_START_END(real_title));
}

void ImGuiFullscreen::MenuButtonBounds::CalcSummarySize(const std::string_view& summary, float font_size)
{
  summary_size = summary.empty() ? ImVec2() :
                                   UIStyle.Font->CalcTextSizeA(font_size, UIStyle.NormalFontWeight, FLT_MAX,
                                                               available_non_value_width, IMSTR_START_END(summary));
}

ImGuiFullscreen::MenuButtonBounds::MenuButtonBounds(const std::string_view& title, const std::string_view& value,
                                                    const std::string_view& summary)
{
  CalcValueSize(value, UIStyle.LargeFontSize);
  CalcTitleSize(title, UIStyle.LargeFontSize);
  CalcSummarySize(summary, UIStyle.MediumFontSize);
  CalcBB();
}

ImGuiFullscreen::MenuButtonBounds::MenuButtonBounds(const std::string_view& title, const std::string_view& value,
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

ImGuiFullscreen::MenuButtonBounds::MenuButtonBounds(const std::string_view& title, const ImVec2& value_size,
                                                    const std::string_view& summary)
{
  SetValueSize(value_size);
  CalcTitleSize(title, UIStyle.LargeFontSize);
  CalcSummarySize(summary, UIStyle.MediumFontSize);
  CalcBB();
}

ImGuiFullscreen::MenuButtonBounds::MenuButtonBounds(const ImVec2& title_size, const ImVec2& value_size,
                                                    const ImVec2& summary_size)
  : title_size(title_size), value_size(value_size), summary_size(summary_size), available_width(CalcAvailWidth())
{
  CalcBB();
}

void ImGuiFullscreen::MenuButtonBounds::CalcBB()
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

float ImGuiFullscreen::MenuButtonBounds::GetSingleLineHeight(float padding)
{
  return (LayoutScale(padding) * 2.0f) + UIStyle.LargeFontSize;
}

float ImGuiFullscreen::MenuButtonBounds::GetSummaryLineHeight(float y_padding)
{
  return GetSingleLineHeight(y_padding) + LayoutScale(LAYOUT_MENU_ITEM_TITLE_SUMMARY_SPACING) + UIStyle.MediumFontSize +
         LayoutScale(LAYOUT_MENU_ITEM_EXTRA_HEIGHT);
}

void ImGuiFullscreen::ResetMenuButtonFrame()
{
  s_state.had_hovered_menu_item = false;
  s_state.has_hovered_menu_item = false;
}

void ImGuiFullscreen::RenderShadowedTextClipped(ImDrawList* draw_list, ImFont* font, float font_size, float font_weight,
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

void ImGuiFullscreen::RenderShadowedTextClipped(ImDrawList* draw_list, ImFont* font, float font_size, float font_weight,
                                                const ImVec2& pos_min, const ImVec2& pos_max, u32 color,
                                                std::string_view text, const ImVec2* text_size_if_known /* = nullptr */,
                                                const ImVec2& align /* = ImVec2(0, 0)*/, float wrap_width /* = 0.0f*/,
                                                const ImRect* clip_rect /* = nullptr */)
{
  RenderShadowedTextClipped(draw_list, font, font_size, font_weight, pos_min, pos_max, color, text, text_size_if_known,
                            align, wrap_width, clip_rect, LayoutScale(LAYOUT_SHADOW_OFFSET));
}

void ImGuiFullscreen::RenderShadowedTextClipped(ImFont* font, float font_size, float font_weight, const ImVec2& pos_min,
                                                const ImVec2& pos_max, u32 color, std::string_view text,
                                                const ImVec2* text_size_if_known /* = nullptr */,
                                                const ImVec2& align /* = ImVec2(0, 0)*/, float wrap_width /* = 0.0f*/,
                                                const ImRect* clip_rect /* = nullptr */)
{
  RenderShadowedTextClipped(ImGui::GetWindowDrawList(), font, font_size, font_weight, pos_min, pos_max, color, text,
                            text_size_if_known, align, wrap_width, clip_rect);
}

void ImGuiFullscreen::RenderMultiLineShadowedTextClipped(ImDrawList* draw_list, ImFont* font, float font_size,
                                                         float font_weight, const ImVec2& pos_min,
                                                         const ImVec2& pos_max, u32 color, std::string_view text,
                                                         const ImVec2& align, float wrap_width,
                                                         const ImRect* clip_rect /* = nullptr */,
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

void ImGuiFullscreen::RenderAutoLabelText(ImDrawList* draw_list, ImFont* font, float font_size, float font_weight,
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

void ImGuiFullscreen::TextAlignedMultiLine(float align_x, const char* text, const char* text_end, float wrap_width)
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

void ImGuiFullscreen::TextUnformatted(std::string_view text)
{
  ImGui::TextUnformatted(IMSTR_START_END(text));
}

void ImGuiFullscreen::MenuHeading(std::string_view title, bool draw_line /*= true*/)
{
  const float line_thickness = draw_line ? LayoutScale(1.0f) : 0.0f;
  const float line_padding = draw_line ? LayoutScale(5.0f) : 0.0f;

  const MenuButtonBounds bb(title, ImVec2(), {});
  bool visible, hovered;
  MenuButtonFrame(title, false, bb.frame_bb, &visible, &hovered);
  if (!visible)
    return;

  const u32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.title_bb.Min,
                            bb.title_bb.Max, color, title, &bb.title_size, ImVec2(0.0f, 0.0f), bb.title_size.x,
                            &bb.title_bb);

  if (draw_line)
  {
    const ImVec2 line_start(bb.title_bb.Min.x, bb.title_bb.Max.y + line_padding);
    const ImVec2 line_end(bb.title_bb.Min.x + bb.available_width, line_start.y);
    const ImVec2 shadow_offset = LayoutScale(LAYOUT_SHADOW_OFFSET, LAYOUT_SHADOW_OFFSET);
    ImGui::GetWindowDrawList()->AddLine(line_start + shadow_offset, line_end + shadow_offset, UIStyle.ShadowColor,
                                        line_thickness);
    ImGui::GetWindowDrawList()->AddLine(line_start, line_end, color, line_thickness);
  }
}

bool ImGuiFullscreen::MenuHeadingButton(std::string_view title, std::string_view value /*= {}*/,
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

bool ImGuiFullscreen::MenuButton(std::string_view title, std::string_view summary, bool enabled /* = true */,
                                 const ImVec2& text_align /* = ImVec2(0.0f, 0.0f) */)
{
  bool visible;
  return MenuButtonWithVisibilityQuery(title, title, summary, {}, &visible, enabled, text_align);
}

bool ImGuiFullscreen::MenuButtonWithoutSummary(std::string_view title, bool enabled /* = true */,
                                               const ImVec2& text_align /* = ImVec2(0.0f, 0.0f) */)
{
  bool visible;
  return MenuButtonWithVisibilityQuery(title, title, {}, {}, &visible, enabled, text_align);
}

bool ImGuiFullscreen::MenuButtonWithValue(std::string_view title, std::string_view summary, std::string_view value,
                                          bool enabled /* = true*/, const ImVec2& text_align /* = ImVec2(0.0f, 0.0f)*/)
{
  bool visible;
  return MenuButtonWithVisibilityQuery(title, title, summary, value, &visible, enabled, text_align);
}

bool ImGuiFullscreen::MenuButtonWithVisibilityQuery(std::string_view str_id, std::string_view title,
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

  s_state.menu_button_index++;
  return pressed;
}

bool ImGuiFullscreen::MenuImageButton(std::string_view title, std::string_view summary, ImTextureID user_texture_id,
                                      const ImVec2& image_size, bool enabled /*= true*/,
                                      const ImVec2& uv0 /*= ImVec2(0.0f, 0.0f)*/,
                                      const ImVec2& uv1 /*= ImVec2(1.0f, 1.0f)*/)
{
  const float left_margin = image_size.x + LayoutScale(15.0f);
  const MenuButtonBounds mbb(title, {}, summary, left_margin);

  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, mbb.frame_bb, &visible, &hovered);
  if (!visible)
    return false;

  const ImRect image_rect(CenterImage(
    ImRect(ImVec2(mbb.title_bb.Min.x - left_margin, mbb.title_bb.Min.y),
           ImVec2(mbb.title_bb.Min.x - left_margin, mbb.title_bb.Min.y + image_size.x)),
    ImVec2(static_cast<float>(user_texture_id->GetWidth()), static_cast<float>(user_texture_id->GetHeight()))));

  ImGui::GetWindowDrawList()->AddImage(user_texture_id, image_rect.Min, image_rect.Max, uv0, uv1,
                                       enabled ? IM_COL32(255, 255, 255, 255) :
                                                 ImGui::GetColorU32(ImGuiCol_TextDisabled));

  const ImVec4& color = ImGui::GetStyle().Colors[enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled];
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, mbb.title_bb.Min,
                            mbb.title_bb.Max, ImGui::GetColorU32(color), title, &mbb.title_size, ImVec2(0.0f, 0.0f),
                            mbb.title_size.x, &mbb.title_bb);

  if (!summary.empty())
  {
    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, mbb.summary_bb.Min,
                              mbb.summary_bb.Max, ImGui::GetColorU32(DarkerColor(color)), summary, &mbb.summary_size,
                              ImVec2(0.0f, 0.0f), mbb.summary_size.x, &mbb.summary_bb);
  }

  s_state.menu_button_index++;
  return pressed;
}

bool ImGuiFullscreen::FloatingButton(std::string_view text, float x, float y, float anchor_x /* = 0.0f */,
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
    x = (window_size.x * 0.5f) - (width * 0.5f) - x;
  else if (anchor_x == 1.0f)
    x = window_size.x - width - x;
  if (anchor_y == -1.0f)
    y -= height;
  else if (anchor_y == -0.5f)
    y -= (height * 0.5f);
  else if (anchor_y == 0.5f)
    y = (window_size.y * 0.5f) - (height * 0.5f) - y;
  else if (anchor_y == 1.0f)
    y = window_size.y - height - y;

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
      const float t = std::min(static_cast<float>(std::abs(std::sin(ImGui::GetTime() * 0.75) * 1.1)), 1.0f);
      const ImU32 col = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered, 1.0f);
      ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Border, t));
      DrawMenuButtonFrame(bb.Min, bb.Max, col, true);
      ImGui::PopStyleColor();
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

bool ImGuiFullscreen::ToggleButton(std::string_view title, std::string_view summary, bool* v, bool enabled /* = true */)
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

  s_state.menu_button_index++;
  return pressed;
}

bool ImGuiFullscreen::ThreeWayToggleButton(std::string_view title, std::string_view summary, std::optional<bool>* v,
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

  s_state.menu_button_index++;
  return pressed;
}

bool ImGuiFullscreen::RangeButton(std::string_view title, std::string_view summary, s32* value, s32 min, s32 max,
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

bool ImGuiFullscreen::RangeButton(std::string_view title, std::string_view summary, float* value, float min, float max,
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

bool ImGuiFullscreen::EnumChoiceButtonImpl(std::string_view title, std::string_view summary, s32* value_pointer,
                                           const char* (*to_display_name_function)(s32 value, void* opaque),
                                           void* opaque, u32 count, bool enabled)
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

void ImGuiFullscreen::BeginHorizontalMenuButtons(u32 num_items, float max_item_width /* = 0.0f */,
                                                 float x_padding /* = LAYOUT_MENU_BUTTON_Y_PADDING */,
                                                 float y_padding /* = LAYOUT_MENU_BUTTON_Y_PADDING */,
                                                 float x_spacing /* = LAYOUT_MENU_BUTTON_X_PADDING */,
                                                 float x_margin /* = LAYOUT_MENU_WINDOW_X_PADDING */)
{
  s_state.menu_button_index = 0;

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

  // need to prerender the backgrounds for all inactive items, otherwise the animation overlaps it
  const ImU32 frame_background = ImGui::GetColorU32(
    DarkerColor((window->Flags & ImGuiWindowFlags_Popup) ? UIStyle.PopupBackgroundColor : UIStyle.BackgroundColor));
  ImVec2 current_pos = ImGui::GetCursorScreenPos();
  for (u32 i = 0; i < num_items; i++)
  {
    ImGui::RenderFrame(current_pos, current_pos + s_state.horizontal_menu_button_size, frame_background, false,
                       LayoutScale(MENU_ITEM_BORDER_ROUNDING));

    current_pos.x += s_state.horizontal_menu_button_size.x + style.ItemSpacing.x;
  }

  PrerenderMenuButtonBorder();
}

void ImGuiFullscreen::EndHorizontalMenuButtons(float add_vertical_spacing /*= -1.0f*/)
{
  ImGui::PopStyleVar(4);
  ImGui::GetCurrentWindow()->DC.LayoutType = ImGuiLayoutType_Vertical;

  const float dummy_height = ImGui::GetCurrentWindowRead()->DC.CurrLineSize.y +
                             ((add_vertical_spacing > 0.0f) ? LayoutScale(add_vertical_spacing) : 0.0f);
  ImGui::ItemSize(ImVec2(0.0f, (dummy_height > 0.0f) ? dummy_height : ImGui::GetFontSize()));
}

bool ImGuiFullscreen::HorizontalMenuButton(std::string_view title, bool enabled /* = true */,
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

  bool hovered;
  bool held;
  bool pressed;
  if (enabled)
  {
    pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, flags);
    if (hovered)
    {
      const ImU32 col = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);

      const float t = static_cast<float>(std::min(std::abs(std::sin(ImGui::GetTime() * 0.75) * 1.1), 1.0));
      ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Border, t));

      DrawMenuButtonFrame(bb.Min, bb.Max, col, true);

      ImGui::PopStyleColor();
    }
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

  s_state.menu_button_index++;
  return pressed;
}

void ImGuiFullscreen::BeginNavBar(float x_padding /*= LAYOUT_MENU_BUTTON_X_PADDING*/,
                                  float y_padding /*= LAYOUT_MENU_BUTTON_Y_PADDING*/)
{
  s_state.menu_button_index = 0;

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(x_padding, y_padding));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, LayoutScale(1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, LayoutScale(1.0f, 0.0f));
  PushPrimaryColor();
}

void ImGuiFullscreen::EndNavBar()
{
  PopPrimaryColor();
  ImGui::PopStyleVar(4);
}

void ImGuiFullscreen::NavTitle(std::string_view title)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return;

  s_state.menu_button_index++;

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

void ImGuiFullscreen::RightAlignNavButtons(u32 num_items /*= 0*/)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  const ImGuiStyle& style = ImGui::GetStyle();

  const float total_item_width = style.FramePadding.x * 2.0f + style.FrameBorderSize + style.ItemSpacing.x +
                                 LayoutScale(LAYOUT_LARGE_FONT_SIZE - 1.0f);
  const float margin = total_item_width * static_cast<float>(num_items);
  ImGui::SetCursorPosX(window->InnerClipRect.Max.x - margin - style.FramePadding.x);
}

bool ImGuiFullscreen::NavButton(std::string_view title, bool is_active, bool enabled /* = true */)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;

  s_state.menu_button_index++;

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
    {
      const ImU32 col = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered, 1.0f);
      DrawMenuButtonFrame(bb.Min, bb.Max, col, true);
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

  RenderShadowedTextClipped(
    UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, bb.Min, bb.Max,
    ImGui::GetColorU32(enabled ? (is_active ? ImGuiCol_Text : ImGuiCol_TextDisabled) : ImGuiCol_ButtonHovered), title,
    &text_size, ImVec2(0.0f, 0.0f), 0.0f, &bb);

  return pressed;
}

bool ImGuiFullscreen::NavTab(std::string_view title, bool is_active, bool enabled, float width)
{
  ImGuiWindow* const window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;

  s_state.menu_button_index++;

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
    ImGui::RenderFrame(bb.Min, bb.Max, col, true, LayoutScale(MENU_ITEM_BORDER_ROUNDING));
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

bool ImGuiFullscreen::BeginHorizontalMenu(const char* name, const ImVec2& position, const ImVec2& size,
                                          const ImVec4& bg_color, u32 num_items)
{
  s_state.menu_button_index = 0;

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

  PrerenderMenuButtonBorder();
  return true;
}

void ImGuiFullscreen::EndHorizontalMenu()
{
  ImGui::PopStyleVar(4);
  EndFullscreenWindow(true, true);
}

bool ImGuiFullscreen::HorizontalMenuItem(GPUTexture* icon, std::string_view title, std::string_view description,
                                         u32 color)
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
  {
    const ImU32 col = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered, 1.0f);

    const float t = static_cast<float>(std::min(std::abs(std::sin(ImGui::GetTime() * 0.75) * 1.1), 1.0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Border, t));

    DrawMenuButtonFrame(bb.Min, bb.Max, col, true);

    ImGui::PopStyleColor();
  }

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

  s_state.menu_button_index++;
  return pressed;
}

ImGuiFullscreen::PopupDialog::PopupDialog() = default;

ImGuiFullscreen::PopupDialog::~PopupDialog() = default;

void ImGuiFullscreen::PopupDialog::StartClose()
{
  if (!IsOpen() || m_state == State::Closing || m_state == State::ClosingTrigger)
    return;

  m_state = State::Closing;
  m_animation_time_remaining = UIStyle.Animations ? CLOSE_TIME : 0.0f;
}

void ImGuiFullscreen::PopupDialog::ClearState()
{
  m_state = State::Inactive;
  m_title = {};
}

void ImGuiFullscreen::PopupDialog::SetTitleAndOpen(std::string title)
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

void ImGuiFullscreen::PopupDialog::CloseImmediately()
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

bool ImGuiFullscreen::PopupDialog::BeginRender(float scaled_window_padding /* = LayoutScale(20.0f) */,
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
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ModAlpha(DarkerColor(UIStyle.PopupBackgroundColor), 1.0f));
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

    ImGui::PopStyleColor(6);
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

void ImGuiFullscreen::PopupDialog::EndRender()
{
  ImGui::EndPopup();
  ImGui::PopStyleColor(6);
  ImGui::PopStyleVar(6);
  ImGui::PopFont();
}

ImGuiFullscreen::FileSelectorDialog::FileSelectorDialog() = default;

ImGuiFullscreen::FileSelectorDialog::~FileSelectorDialog() = default;

void ImGuiFullscreen::FileSelectorDialog::Open(std::string_view title, FileSelectorCallback callback,
                                               FileSelectorFilters filters, std::string initial_directory,
                                               bool select_directory)
{
  if (initial_directory.empty() || !FileSystem::DirectoryExists(initial_directory.c_str()))
    initial_directory = FileSystem::GetWorkingDirectory();

  if (Host::ShouldPreferHostFileSelector())
  {
    Host::OpenHostFileSelectorAsync(ImGuiManager::StripIconCharacters(title), select_directory, std::move(callback),
                                    std::move(filters), initial_directory);
    return;
  }

  SetTitleAndOpen(fmt::format("{}##file_selector_dialog", title));
  m_callback = std::move(callback);
  m_filters = std::move(filters);
  m_is_directory = select_directory;
  SetDirectory(std::move(initial_directory));
}

void ImGuiFullscreen::FileSelectorDialog::ClearState()
{
  PopupDialog::ClearState();
  m_callback = {};
  m_filters = {};
  m_is_directory = false;
  m_directory_changed = false;
}

ImGuiFullscreen::FileSelectorDialog::Item::Item(std::string display_name_, std::string full_path_, bool is_file_)
  : display_name(std::move(display_name_)), full_path(std::move(full_path_)), is_file(is_file_)
{
}

void ImGuiFullscreen::FileSelectorDialog::PopulateItems()
{
  m_items.clear();
  m_first_item_is_parent_directory = false;

  if (m_current_directory.empty())
  {
    for (std::string& root_path : FileSystem::GetRootDirectoryList())
    {
#ifdef _WIN32A
      // Remove trailing backslash on Windows.
      while (!root_path.empty() && root_path.back() == FS_OSPATH_SEPARATOR_CHARACTER)
        root_path.pop_back();
#endif

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

void ImGuiFullscreen::FileSelectorDialog::SetDirectory(std::string dir)
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

void ImGuiFullscreen::FileSelectorDialog::Draw()
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

  GetFileSelectorHelpText(s_state.fullscreen_footer_text);

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

bool ImGuiFullscreen::IsFileSelectorOpen()
{
  return s_state.file_selector_dialog.IsOpen();
}

void ImGuiFullscreen::OpenFileSelector(std::string_view title, bool select_directory, FileSelectorCallback callback,
                                       FileSelectorFilters filters, std::string initial_directory)
{
  s_state.file_selector_dialog.Open(title, std::move(callback), std::move(filters), std::move(initial_directory),
                                    select_directory);
}

void ImGuiFullscreen::CloseFileSelector()
{
  s_state.file_selector_dialog.StartClose();
}

ImGuiFullscreen::ChoiceDialog::ChoiceDialog() = default;

ImGuiFullscreen::ChoiceDialog::~ChoiceDialog() = default;

void ImGuiFullscreen::ChoiceDialog::Open(std::string_view title, ChoiceDialogOptions options,
                                         ChoiceDialogCallback callback, bool checkable)
{
  SetTitleAndOpen(fmt::format("{}##choice_dialog", title));
  m_options = std::move(options);
  m_callback = std::move(callback);
  m_checkable = checkable;
}

void ImGuiFullscreen::ChoiceDialog::ClearState()
{
  PopupDialog::ClearState();
  m_options = {};
  m_callback = {};
  m_checkable = false;
}

void ImGuiFullscreen::ChoiceDialog::Draw()
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
                     LAYOUT_MENU_BUTTON_SPACING, false);

    if (std::any_of(m_options.begin(), m_options.end(), [](const auto& it) { return it.second; }))
    {
      // draw background first, because otherwise it'll obscure the frame border
      ImVec2 pos = ImGui::GetCurrentWindowRead()->DC.CursorPos;
      for (s32 i = 0; i < static_cast<s32>(m_options.size()); i++)
      {
        const auto& option = m_options[i];
        const MenuButtonBounds bb(option.first, ImVec2(), {});
        if (!option.second)
        {
          pos.y += bb.frame_bb.GetHeight() + ImGui::GetStyle().ItemSpacing.y;
          continue;
        }

        ImGui::RenderFrame(pos, pos + bb.frame_bb.GetSize(), ImGui::GetColorU32(UIStyle.PrimaryColor), false,
                           LayoutScale(MENU_ITEM_BORDER_ROUNDING));
        break;
      }
    }

    PrerenderMenuButtonBorder();
    ResetFocusHere();

    const bool appearing = ImGui::IsWindowAppearing();

    for (s32 i = 0; i < static_cast<s32>(m_options.size()); i++)
    {
      auto& option = m_options[i];

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

  GetChoiceDialogHelpText(s_state.fullscreen_footer_text);

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

bool ImGuiFullscreen::IsChoiceDialogOpen()
{
  return s_state.choice_dialog.IsOpen();
}

void ImGuiFullscreen::OpenChoiceDialog(std::string_view title, bool checkable, ChoiceDialogOptions options,
                                       ChoiceDialogCallback callback)
{
  s_state.choice_dialog.Open(title, std::move(options), std::move(callback), checkable);
}

void ImGuiFullscreen::CloseChoiceDialog()
{
  s_state.choice_dialog.StartClose();
}

bool ImGuiFullscreen::IsInputDialogOpen()
{
  return s_state.input_string_dialog.IsOpen();
}

void ImGuiFullscreen::OpenInputStringDialog(std::string_view title, std::string message, std::string caption,
                                            std::string ok_button_text, InputStringDialogCallback callback)
{
  s_state.input_string_dialog.Open(title, std::move(message), std::move(caption), std::move(ok_button_text),
                                   std::move(callback));
  QueueResetFocus(FocusResetType::PopupOpened);
}

ImGuiFullscreen::InputStringDialog::InputStringDialog() = default;

ImGuiFullscreen::InputStringDialog::~InputStringDialog() = default;

void ImGuiFullscreen::InputStringDialog::Open(std::string_view title, std::string message, std::string caption,
                                              std::string ok_button_text, InputStringDialogCallback callback)
{
  SetTitleAndOpen(fmt::format("{}##input_string_dialog", title));
  m_message = std::move(message);
  m_caption = std::move(caption);
  m_ok_text = std::move(ok_button_text);
  m_callback = std::move(callback);
}

void ImGuiFullscreen::InputStringDialog::ClearState()
{
  PopupDialog::ClearState();
  m_message = {};
  m_caption = {};
  m_ok_text = {};
  m_callback = {};
}

void ImGuiFullscreen::InputStringDialog::Draw()
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

  GetInputDialogHelpText(s_state.fullscreen_footer_text);

  EndRender();
}

void ImGuiFullscreen::CloseInputDialog()
{
  s_state.input_string_dialog.StartClose();
}

ImGuiFullscreen::MessageDialog::MessageDialog() = default;

ImGuiFullscreen::MessageDialog::~MessageDialog() = default;

void ImGuiFullscreen::MessageDialog::Open(std::string_view title, std::string message, CallbackVariant callback,
                                          std::string first_button_text, std::string second_button_text,
                                          std::string third_button_text)
{
  SetTitleAndOpen(fmt::format("{}##message_dialog", title));
  m_message = std::move(message);
  m_callback = std::move(callback);
  m_buttons[0] = std::move(first_button_text);
  m_buttons[1] = std::move(second_button_text);
  m_buttons[2] = std::move(third_button_text);
}

void ImGuiFullscreen::MessageDialog::ClearState()
{
  PopupDialog::ClearState();
  m_message = {};
  m_buttons = {};
  m_callback = {};
}

void ImGuiFullscreen::MessageDialog::Draw()
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

  ResetFocusHere();
  BeginMenuButtons();

  ImGui::TextWrapped("%s", m_message.c_str());
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(20.0f));

  for (s32 button_index = 0; button_index < static_cast<s32>(m_buttons.size()); button_index++)
  {
    if (!m_buttons[button_index].empty() &&
        MenuButtonWithoutSummary(m_buttons[button_index], true, LAYOUT_CENTER_ALIGN_TEXT))
    {
      result = button_index;
    }
  }

  EndMenuButtons();

  GetChoiceDialogHelpText(s_state.fullscreen_footer_text);

  EndRender();

  if (result.has_value())
  {
    // have to move out in case they open another dialog in the callback
    StartClose();
    InvokeCallback(std::exchange(m_callback, CallbackVariant()), result);
  }
}

void ImGuiFullscreen::MessageDialog::InvokeCallback(const CallbackVariant& cb, std::optional<s32> choice)
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

bool ImGuiFullscreen::IsMessageBoxDialogOpen()
{
  return s_state.message_dialog.IsOpen();
}

void ImGuiFullscreen::OpenConfirmMessageDialog(std::string_view title, std::string message,
                                               ConfirmMessageDialogCallback callback, std::string yes_button_text,
                                               std::string no_button_text)
{
  s_state.message_dialog.Open(std::move(title), std::move(message), std::move(callback), std::move(yes_button_text),
                              std::move(no_button_text), std::string());
}

void ImGuiFullscreen::OpenInfoMessageDialog(std::string_view title, std::string message,
                                            InfoMessageDialogCallback callback, std::string button_text)
{
  s_state.message_dialog.Open(std::move(title), std::move(message), std::move(callback), std::move(button_text),
                              std::string(), std::string());
}

void ImGuiFullscreen::OpenMessageDialog(std::string_view title, std::string message, MessageDialogCallback callback,
                                        std::string first_button_text, std::string second_button_text,
                                        std::string third_button_text)
{
  s_state.message_dialog.Open(std::move(title), std::move(message), std::move(callback), std::move(first_button_text),
                              std::move(second_button_text), std::move(third_button_text));
}

void ImGuiFullscreen::CloseMessageDialog()
{
  s_state.message_dialog.StartClose();
}

ImGuiFullscreen::ProgressDialog::ProgressDialog() = default;
ImGuiFullscreen::ProgressDialog::~ProgressDialog() = default;

void ImGuiFullscreen::ProgressDialog::Draw()
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
    ImGuiFullscreen::TextAlignedMultiLine(0.0f, IMSTR_START_END(m_status_text), wrap_width);

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

std::unique_ptr<ProgressCallback> ImGuiFullscreen::ProgressDialog::GetProgressCallback(std::string title,
                                                                                       float window_unscaled_width)
{
  if (m_state == PopupDialog::State::Open)
  {
    // return dummy callback so the op can still go through
    ERROR_LOG("Progress dialog is already open, cannot create dialog for '{}'.", std::move(title));
    return std::make_unique<ProgressCallback>();
  }

  SetTitleAndOpen(std::move(title));
  m_width = LayoutScale(window_unscaled_width);
  m_last_frac = 0.0f;
  m_cancelled.store(false, std::memory_order_release);
  return std::make_unique<ProgressCallbackImpl>();
}

ImGuiFullscreen::ProgressDialog::ProgressCallbackImpl::ProgressCallbackImpl() = default;

ImGuiFullscreen::ProgressDialog::ProgressCallbackImpl::~ProgressCallbackImpl()
{
  Host::RunOnCPUThread([]() mutable {
    GPUThread::RunOnThread([]() mutable {
      if (!s_state.progress_dialog.IsOpen())
        return;
      s_state.progress_dialog.StartClose();
    });
  });
}

void ImGuiFullscreen::ProgressDialog::ProgressCallbackImpl::SetStatusText(std::string_view text)
{
  Host::RunOnCPUThread([text = std::string(text)]() mutable {
    GPUThread::RunOnThread([text = std::move(text)]() mutable {
      if (!s_state.progress_dialog.IsOpen())
        return;

      s_state.progress_dialog.m_status_text = std::move(text);
    });
  });
}

void ImGuiFullscreen::ProgressDialog::ProgressCallbackImpl::SetProgressRange(u32 range)
{
  ProgressCallback::SetProgressRange(range);

  Host::RunOnCPUThread([range]() {
    GPUThread::RunOnThread([range]() {
      if (!s_state.progress_dialog.IsOpen())
        return;

      s_state.progress_dialog.m_progress_range = range;
    });
  });
}

void ImGuiFullscreen::ProgressDialog::ProgressCallbackImpl::SetProgressValue(u32 value)
{
  ProgressCallback::SetProgressValue(value);

  Host::RunOnCPUThread([value]() {
    GPUThread::RunOnThread([value]() {
      if (!s_state.progress_dialog.IsOpen())
        return;

      s_state.progress_dialog.m_progress_value = value;
    });
  });
}

void ImGuiFullscreen::ProgressDialog::ProgressCallbackImpl::SetCancellable(bool cancellable)
{
  ProgressCallback::SetCancellable(cancellable);

  Host::RunOnCPUThread([cancellable]() {
    GPUThread::RunOnThread([cancellable]() {
      if (!s_state.progress_dialog.IsOpen())
        return;

      s_state.progress_dialog.m_user_closeable = cancellable;
    });
  });
}

bool ImGuiFullscreen::ProgressDialog::ProgressCallbackImpl::IsCancelled() const
{
  return s_state.progress_dialog.m_cancelled.load(std::memory_order_acquire);
}

std::unique_ptr<ProgressCallback> ImGuiFullscreen::OpenModalProgressDialog(std::string title,
                                                                           float window_unscaled_width)
{
  return s_state.progress_dialog.GetProgressCallback(std::move(title), window_unscaled_width);
}

static float s_notification_vertical_position = 0.15f;
static float s_notification_vertical_direction = 1.0f;

float ImGuiFullscreen::GetNotificationVerticalPosition()
{
  return s_notification_vertical_position;
}

float ImGuiFullscreen::GetNotificationVerticalDirection()
{
  return s_notification_vertical_direction;
}

void ImGuiFullscreen::SetNotificationVerticalPosition(float position, float direction)
{
  s_notification_vertical_position = position;
  s_notification_vertical_direction = direction;
}

ImGuiID ImGuiFullscreen::GetBackgroundProgressID(std::string_view str_id)
{
  return ImHashStr(str_id.data(), str_id.length());
}

void ImGuiFullscreen::OpenBackgroundProgressDialog(std::string_view str_id, std::string message, s32 min, s32 max,
                                                   s32 value)
{
  const ImGuiID id = GetBackgroundProgressID(str_id);

  std::unique_lock lock(s_state.shared_state_mutex);

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
}

void ImGuiFullscreen::UpdateBackgroundProgressDialog(std::string_view str_id, std::string message, s32 min, s32 max,
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

void ImGuiFullscreen::CloseBackgroundProgressDialog(std::string_view str_id)
{
  const ImGuiID id = GetBackgroundProgressID(str_id);

  std::unique_lock lock(s_state.shared_state_mutex);

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

bool ImGuiFullscreen::IsBackgroundProgressDialogOpen(std::string_view str_id)
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

void ImGuiFullscreen::DrawBackgroundProgressDialogs(ImVec2& position, float spacing)
{
  std::unique_lock lock(s_state.shared_state_mutex);
  if (s_state.background_progress_dialogs.empty())
    return;

  const float window_width = LayoutScale(500.0f);
  const float window_height = LayoutScale(75.0f);

  ImDrawList* dl = ImGui::GetForegroundDrawList();

  for (const BackgroundProgressDialogData& data : s_state.background_progress_dialogs)
  {
    const float window_pos_x = position.x;
    const float window_pos_y = position.y - ((s_notification_vertical_direction < 0.0f) ? window_height : 0.0f);

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

    position.y += s_notification_vertical_direction * (window_height + spacing);
  }
}

void ImGuiFullscreen::RenderLoadingScreen(std::string_view image, std::string_view message, s32 progress_min /*= -1*/,
                                          s32 progress_max /*= -1*/, s32 progress_value /*= -1*/)
{
  if (progress_min < progress_max)
    INFO_LOG("{}: {}/{}", message, progress_value, progress_max);

  if (!g_gpu_device || !g_gpu_device->HasMainSwapChain())
    return;

  // eat the last imgui frame, it might've been partially rendered by the caller.
  ImGui::EndFrame();
  ImGui::NewFrame();

  DrawLoadingScreen(image, message, progress_min, progress_max, progress_value, false);

  ImGuiManager::CreateDrawLists();

  GPUSwapChain* swap_chain = g_gpu_device->GetMainSwapChain();
  if (g_gpu_device->BeginPresent(swap_chain) == GPUDevice::PresentResult::OK)
  {
    ImGuiManager::RenderDrawLists(swap_chain);
    g_gpu_device->EndPresent(swap_chain, false);
  }

  ImGuiManager::NewFrame();
}

void ImGuiFullscreen::OpenOrUpdateLoadingScreen(std::string_view image, std::string_view message,
                                                s32 progress_min /*= -1*/, s32 progress_max /*= -1*/,
                                                s32 progress_value /*= -1*/)
{
  if (!image.empty() && s_state.loading_screen_image != image)
    s_state.loading_screen_image = image;
  if (s_state.loading_screen_message != message)
    s_state.loading_screen_message = message;
  s_state.loading_screen_min = progress_min;
  s_state.loading_screen_max = progress_max;
  s_state.loading_screen_value = progress_value;
  s_state.loading_screen_open = true;
}

bool ImGuiFullscreen::IsLoadingScreenOpen()
{
  return s_state.loading_screen_open;
}

void ImGuiFullscreen::RenderLoadingScreen()
{
  if (!s_state.loading_screen_open)
    return;

  DrawLoadingScreen(s_state.loading_screen_image, s_state.loading_screen_message, s_state.loading_screen_min,
                    s_state.loading_screen_max, s_state.loading_screen_value, true);
}

void ImGuiFullscreen::CloseLoadingScreen()
{
  s_state.loading_screen_image = {};
  s_state.loading_screen_message = {};
  s_state.loading_screen_min = 0;
  s_state.loading_screen_max = 0;
  s_state.loading_screen_value = 0;
  s_state.loading_screen_open = false;
}

void ImGuiFullscreen::DrawLoadingScreen(std::string_view image, std::string_view message, s32 progress_min,
                                        s32 progress_max, s32 progress_value, bool is_persistent)
{
  const auto& io = ImGui::GetIO();
  const bool has_progress = (progress_min < progress_max);

  const float font_size = UIStyle.MediumFontSize;
  const float font_weight = UIStyle.BoldFontWeight;
  const float padding_and_rounding = LayoutScale(18.0f);
  const float item_spacing = LayoutScale(10.0f);
  const float frame_rounding = LayoutScale(6.0f);
  const float bar_height = (has_progress || is_persistent) ? LayoutScale(18.0f) : 0.0f;
  const float window_width = LayoutScale(450.0f);
  const float window_height =
    ((padding_and_rounding * 2.0f) + font_size + item_spacing + bar_height + LayoutScale(5.0f));
  const float window_spacing = LayoutScale(20.0f);

  const float image_width = LayoutScale(260.0f);
  const float image_height = LayoutScale(260.0f);
  const ImVec2 image_pos = ImVec2(ImCeil((io.DisplaySize.x - image_width) * 0.5f),
                                  ImCeil(((io.DisplaySize.y - image_height - window_height - window_spacing) * 0.5f)));
  ImDrawList* const dl = ImGui::GetBackgroundDrawList();
  GPUTexture* tex = GetCachedTexture(image);
  if (tex)
  {
    const ImRect image_rect = CenterImage(ImRect(image_pos, image_pos + ImVec2(image_width, image_height)), tex);
    dl->AddImage(tex, image_rect.Min, image_rect.Max);
  }

  const ImVec2 window_pos =
    ImVec2(ImCeil((io.DisplaySize.x - window_width) * 0.5f), image_pos.y + image_height + window_spacing);
  dl->AddRectFilled(window_pos, window_pos + ImVec2(window_width, window_height),
                    ImGui::GetColorU32(UIStyle.PopupBackgroundColor), padding_and_rounding);

  TinyString prog_text;
  ImVec2 prog_text_size;
  if (has_progress)
  {
    prog_text.format("{}/{}", progress_value, progress_max);
    prog_text_size = UIStyle.Font->CalcTextSizeA(font_size, font_weight, FLT_MAX, 0.0f, IMSTR_START_END(prog_text));
  }

  const float avail_width = window_width - (padding_and_rounding * 2.0f);
  const ImVec2 text_pos = window_pos + ImVec2(padding_and_rounding, padding_and_rounding);
  const float text_wrap_width = avail_width - prog_text_size.x;
  const ImVec2 text_size =
    UIStyle.Font->CalcTextSizeA(font_size, font_weight, FLT_MAX, text_wrap_width, IMSTR_START_END(message));
  dl->AddText(UIStyle.Font, font_size, font_weight, text_pos, ImGui::GetColorU32(UIStyle.PrimaryTextColor),
              IMSTR_START_END(message), text_wrap_width);

  if (has_progress)
  {
    const ImVec2 prog_text_pos = ImVec2(text_pos.x + avail_width - prog_text_size.x, text_pos.y);
    dl->AddText(UIStyle.Font, font_size, font_weight, prog_text_pos, ImGui::GetColorU32(UIStyle.PrimaryTextColor),
                IMSTR_START_END(prog_text));
  }

  if (bar_height > 0.0f)
  {
    const ImVec2 box_start = ImVec2(text_pos.x, text_pos.y + text_size.y + item_spacing);
    const ImVec2 box_end = box_start + ImVec2(avail_width, bar_height);
    dl->AddRectFilled(box_start, box_end, ImGui::GetColorU32(UIStyle.PopupFrameBackgroundColor), frame_rounding);

    if (has_progress)
    {
      const float fraction = static_cast<float>(progress_value) / static_cast<float>(progress_max - progress_min);
      ImGui::RenderRectFilledRangeH(dl, ImRect(box_start, box_end), ImGui::GetColorU32(UIStyle.SecondaryColor), 0.0f,
                                    fraction, frame_rounding);

#if 0
      prog_text.format("{}%", static_cast<int>(std::round(fraction * 100.0f)));
      const ImVec2 pct_text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight,
                                                               FLT_MAX, 0.0f, IMSTR_START_END(prog_text));
      const ImVec2 pct_text_pos = ImVec2(box_start.x + ((box_end.x - box_start.x) / 2.0f) - (pct_text_size.x / 2.0f),
                                         box_start.y + ((box_end.y - box_start.y) / 2.0f) - (pct_text_size.y / 2.0f));
      dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, pct_text_pos,
                  ImGui::GetColorU32(UIStyle.PrimaryTextColor), IMSTR_START_END(prog_text));
#endif
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
  }
}

//////////////////////////////////////////////////////////////////////////
// Notifications
//////////////////////////////////////////////////////////////////////////

static constexpr float NOTIFICATION_APPEAR_ANIMATION_TIME = 0.2f;
static constexpr float NOTIFICATION_DISAPPEAR_ANIMATION_TIME = 0.5f;

void ImGuiFullscreen::AddNotification(std::string key, float duration, std::string title, std::string text,
                                      std::string image_path)
{
  const Timer::Value current_time = Timer::GetCurrentValue();

  if (!key.empty())
  {
    for (auto it = s_state.notifications.begin(); it != s_state.notifications.end(); ++it)
    {
      if (it->key == key)
      {
        it->duration = duration;
        it->title = std::move(title);
        it->text = std::move(text);
        it->badge_path = std::move(image_path);

        // Don't fade it in again
        const float time_passed = static_cast<float>(Timer::ConvertValueToSeconds(current_time - it->start_time));
        it->start_time =
          current_time - Timer::ConvertSecondsToValue(std::min(time_passed, NOTIFICATION_APPEAR_ANIMATION_TIME));
        return;
      }
    }
  }

  Notification notif;
  notif.key = std::move(key);
  notif.duration = duration;
  notif.title = std::move(title);
  notif.text = std::move(text);
  notif.badge_path = std::move(image_path);
  notif.start_time = current_time;
  notif.move_time = current_time;
  notif.target_y = -1.0f;
  notif.last_y = -1.0f;
  s_state.notifications.push_back(std::move(notif));
  FullscreenUI::UpdateRunIdleState();
}

bool ImGuiFullscreen::HasAnyNotifications()
{
  return !s_state.notifications.empty();
}

void ImGuiFullscreen::ClearNotifications()
{
  s_state.notifications.clear();
}

void ImGuiFullscreen::DrawNotifications(ImVec2& position, float spacing)
{
  if (s_state.notifications.empty())
    return;

  static constexpr float MOVE_DURATION = 0.5f;
  const Timer::Value current_time = Timer::GetCurrentValue();

  const float horizontal_padding = ImGuiFullscreen::LayoutScale(20.0f);
  const float vertical_padding = ImGuiFullscreen::LayoutScale(15.0f);
  const float horizontal_spacing = ImGuiFullscreen::LayoutScale(10.0f);
  const float vertical_spacing = ImGuiFullscreen::LayoutScale(4.0f);
  const float badge_size = ImGuiFullscreen::LayoutScale(48.0f);
  const float min_width = ImGuiFullscreen::LayoutScale(200.0f);
  const float max_width = ImGuiFullscreen::LayoutScale(800.0f);
  const float max_text_width = max_width - badge_size - (horizontal_padding * 2.0f) - horizontal_spacing;
  const float min_height = (vertical_padding * 2.0f) + badge_size;
  const float shadow_size = ImGuiFullscreen::LayoutScale(2.0f);
  const float rounding = ImGuiFullscreen::LayoutScale(20.0f);

  ImFont* const title_font = UIStyle.Font;
  const float title_font_size = UIStyle.LargeFontSize;
  const float title_font_weight = UIStyle.BoldFontWeight;
  ImFont* const text_font = UIStyle.Font;
  const float text_font_size = UIStyle.MediumFontSize;
  const float text_font_weight = UIStyle.NormalFontWeight;

  for (u32 index = 0; index < static_cast<u32>(s_state.notifications.size());)
  {
    Notification& notif = s_state.notifications[index];
    const float time_passed = static_cast<float>(Timer::ConvertValueToSeconds(current_time - notif.start_time));
    if (time_passed >= notif.duration)
    {
      s_state.notifications.erase(s_state.notifications.begin() + index);
      continue;
    }

    const ImVec2 title_size = title_font->CalcTextSizeA(title_font_size, title_font_weight, max_text_width,
                                                        max_text_width, IMSTR_START_END(notif.title));
    const ImVec2 text_size = text_font->CalcTextSizeA(text_font_size, text_font_weight, max_text_width, max_text_width,
                                                      IMSTR_START_END(notif.text));

    float box_width = std::max((horizontal_padding * 2.0f) + badge_size + horizontal_spacing +
                                 ImCeil(std::max(title_size.x, text_size.x)),
                               min_width);
    const float box_height =
      std::max((vertical_padding * 2.0f) + ImCeil(title_size.y) + vertical_spacing + ImCeil(text_size.y), min_height);

    float opacity = 1.0f;
    bool clip_box = false;
    if (time_passed < NOTIFICATION_APPEAR_ANIMATION_TIME)
    {
      const float pct = time_passed / NOTIFICATION_APPEAR_ANIMATION_TIME;
      const float eased_pct = Easing::OutExpo(pct);
      box_width = box_width * eased_pct;
      opacity = pct;
      clip_box = true;
    }
    else if (time_passed >= (notif.duration - NOTIFICATION_DISAPPEAR_ANIMATION_TIME))
    {
      const float pct = (notif.duration - time_passed) / NOTIFICATION_DISAPPEAR_ANIMATION_TIME;
      const float eased_pct = Easing::InExpo(pct);
      box_width = box_width * eased_pct;
      opacity = eased_pct;
      clip_box = true;
    }

    const float expected_y = position.y - ((s_notification_vertical_direction < 0.0f) ? box_height : 0.0f);
    float actual_y = notif.last_y;
    if (notif.target_y != expected_y)
    {
      notif.move_time = current_time;
      notif.target_y = expected_y;
      notif.last_y = (notif.last_y < 0.0f) ? expected_y : notif.last_y;
      actual_y = notif.last_y;
    }
    else if (actual_y != expected_y)
    {
      const float time_since_move = static_cast<float>(Timer::ConvertValueToSeconds(current_time - notif.move_time));
      if (time_since_move >= MOVE_DURATION)
      {
        notif.move_time = current_time;
        notif.last_y = notif.target_y;
        actual_y = notif.last_y;
      }
      else
      {
        const float frac = Easing::OutExpo(time_since_move / MOVE_DURATION);
        actual_y = notif.last_y - ((notif.last_y - notif.target_y) * frac);
      }
    }

    const ImVec2 box_min(position.x, actual_y);
    const ImVec2 box_max(box_min.x + box_width, box_min.y + box_height);
    const u32 background_color = ImGui::GetColorU32(ModAlpha(UIStyle.ToastBackgroundColor, opacity * 0.95f));

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(box_min, box_max, background_color, rounding, ImDrawFlags_RoundCornersAll);

    if (clip_box)
      dl->PushClipRect(box_min, box_max);

    const ImVec2 badge_min(box_min.x + horizontal_padding, box_min.y + vertical_padding);
    const ImVec2 badge_max(badge_min.x + badge_size, badge_min.y + badge_size);
    if (!notif.badge_path.empty())
    {
      GPUTexture* tex = GetCachedTexture(notif.badge_path, static_cast<u32>(badge_size), static_cast<u32>(badge_size));
      if (tex)
      {
        dl->AddImage(tex, badge_min, badge_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                     ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacity)));
      }
    }

    const u32 title_col = ImGui::GetColorU32(ModAlpha(UIStyle.ToastTextColor, opacity));
    const u32 text_col = ImGui::GetColorU32(ModAlpha(DarkerColor(UIStyle.ToastTextColor), opacity));

    const ImVec2 title_pos = ImVec2(badge_max.x + horizontal_spacing, box_min.y + vertical_padding);
    const ImRect title_bb = ImRect(title_pos, title_pos + title_size);
    RenderShadowedTextClipped(dl, title_font, title_font_size, title_font_weight, title_bb.Min, title_bb.Max, title_col,
                              notif.title, &title_size, ImVec2(0.0f, 0.0f), max_text_width, &title_bb);

    const ImVec2 text_pos = ImVec2(badge_max.x + horizontal_spacing, title_bb.Max.y + vertical_spacing);
    const ImRect text_bb = ImRect(text_pos, text_pos + text_size);
    RenderShadowedTextClipped(dl, text_font, text_font_size, text_font_weight, text_bb.Min, text_bb.Max, text_col,
                              notif.text, &text_size, ImVec2(0.0f, 0.0f), max_text_width, &text_bb);

    if (clip_box)
      dl->PopClipRect();

    position.y += s_notification_vertical_direction * (box_height + shadow_size + spacing);
    index++;
  }

  // all gone?
  if (s_state.notifications.empty())
    FullscreenUI::UpdateRunIdleState();
}

void ImGuiFullscreen::ShowToast(std::string title, std::string message, float duration)
{
  s_state.toast_title = std::move(title);
  s_state.toast_message = std::move(message);
  s_state.toast_start_time = Timer::GetCurrentValue();
  s_state.toast_duration = duration;
  FullscreenUI::UpdateRunIdleState();
}

bool ImGuiFullscreen::HasToast()
{
  return (!s_state.toast_title.empty() || !s_state.toast_message.empty());
}

void ImGuiFullscreen::ClearToast()
{
  s_state.toast_message = {};
  s_state.toast_title = {};
  s_state.toast_start_time = 0;
  s_state.toast_duration = 0.0f;
  FullscreenUI::UpdateRunIdleState();
}

void ImGuiFullscreen::DrawToast()
{
  if (s_state.toast_title.empty() && s_state.toast_message.empty())
    return;

  const float elapsed =
    static_cast<float>(Timer::ConvertValueToSeconds(Timer::GetCurrentValue() - s_state.toast_start_time));
  if (elapsed >= s_state.toast_duration)
  {
    ClearToast();
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
  const float margin = LayoutScale(20.0f + (s_state.fullscreen_footer_text.empty() ? 0.0f : LAYOUT_FOOTER_HEIGHT));
  const float spacing = s_state.toast_title.empty() ? 0.0f : LayoutScale(10.0f);
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
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
  const ImVec2 box_pos((display_size.x - box_size.x) * 0.5f, (display_size.y - margin - box_size.y));

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

void ImGuiFullscreen::SetTheme(std::string_view theme)
{
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
    UIStyle.DisabledColor = HEX_TO_IMVEC4(0x999999, 0xff);
    UIStyle.TextHighlightColor = HEX_TO_IMVEC4(0x8e8e8e, 0xff);
    UIStyle.PrimaryLineColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.SecondaryColor = HEX_TO_IMVEC4(0x2a3e78, 0xff);
    UIStyle.SecondaryStrongColor = HEX_TO_IMVEC4(0x464db1, 0xff);
    UIStyle.SecondaryWeakColor = HEX_TO_IMVEC4(0xc0cfff, 0xff);
    UIStyle.SecondaryTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.ToastBackgroundColor = HEX_TO_IMVEC4(0xf1f1f1, 0xff);
    UIStyle.ToastTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.ShadowColor = IM_COL32(100, 100, 100, 50);
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
  }
}
