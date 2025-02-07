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

#include "core/host.h"
#include "core/system.h" // For async workers, should be in general host.

#include "fmt/core.h"

#include "IconsFontAwesome5.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>
#include <variant>

LOG_CHANNEL(ImGuiFullscreen);

namespace ImGuiFullscreen {
using MessageDialogCallbackVariant = std::variant<InfoMessageDialogCallback, ConfirmMessageDialogCallback>;

static constexpr float MENU_BACKGROUND_ANIMATION_TIME = 0.5f;
static constexpr float SMOOTH_SCROLLING_SPEED = 3.5f;

static std::optional<Image> LoadTextureImage(std::string_view path, u32 svg_width, u32 svg_height);
static std::shared_ptr<GPUTexture> UploadTexture(std::string_view path, const Image& image);

static void DrawFileSelector();
static void DrawChoiceDialog();
static void DrawInputDialog();
static void DrawMessageDialog();
static void DrawBackgroundProgressDialogs(ImVec2& position, float spacing);
static void DrawLoadingScreen(std::string_view image, std::string_view message, s32 progress_min, s32 progress_max,
                              s32 progress_value, bool is_persistent);
static void DrawNotifications(ImVec2& position, float spacing);
static void DrawToast();
static bool MenuButtonFrame(const char* str_id, bool enabled, float height, bool* visible, bool* hovered, ImRect* bb,
                            ImGuiButtonFlags flags = 0, float hover_alpha = 1.0f);
static void PopulateFileSelectorItems();
static void SetFileSelectorDirectory(std::string dir);
static ImGuiID GetBackgroundProgressID(const char* str_id);

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

struct FileSelectorItem
{
  FileSelectorItem() = default;
  FileSelectorItem(std::string display_name_, std::string full_path_, bool is_file_)
    : display_name(std::move(display_name_)), full_path(std::move(full_path_)), is_file(is_file_)
  {
  }
  FileSelectorItem(const FileSelectorItem&) = default;
  FileSelectorItem(FileSelectorItem&&) = default;
  ~FileSelectorItem() = default;

  FileSelectorItem& operator=(const FileSelectorItem&) = default;
  FileSelectorItem& operator=(FileSelectorItem&&) = default;

  std::string display_name;
  std::string full_path;
  bool is_file;
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

struct ALIGN_TO_CACHE_LINE UIState
{
  std::recursive_mutex shared_state_mutex;

  u32 menu_button_index = 0;
  CloseButtonState close_button_state = CloseButtonState::None;
  ImGuiDir has_pending_nav_move = ImGuiDir_None;
  FocusResetType focus_reset_queued = FocusResetType::None;
  bool initialized = false;
  bool light_theme = false;
  bool smooth_scrolling = false;

  LRUCache<std::string, std::shared_ptr<GPUTexture>> texture_cache{128, true};
  std::shared_ptr<GPUTexture> placeholder_texture;
  std::deque<std::pair<std::string, Image>> texture_upload_queue;

  SmallString fullscreen_footer_text;
  SmallString last_fullscreen_footer_text;
  std::vector<std::pair<std::string_view, std::string_view>> fullscreen_footer_icon_mapping;
  float fullscreen_text_change_time;
  float fullscreen_text_alpha;

  std::string choice_dialog_title;
  ChoiceDialogOptions choice_dialog_options;
  ChoiceDialogCallback choice_dialog_callback;
  ImGuiID enum_choice_button_id = 0;
  s32 enum_choice_button_value = 0;
  bool enum_choice_button_set = false;
  bool choice_dialog_open = false;
  bool choice_dialog_checkable = false;

  bool input_dialog_open = false;
  std::string input_dialog_title;
  std::string input_dialog_message;
  std::string input_dialog_caption;
  std::string input_dialog_text;
  std::string input_dialog_ok_text;
  InputStringDialogCallback input_dialog_callback;

  bool message_dialog_open = false;
  std::string message_dialog_title;
  std::string message_dialog_message;
  std::array<std::string, 3> message_dialog_buttons;
  MessageDialogCallbackVariant message_dialog_callback;

  ImAnimatedVec2 menu_button_frame_min_animated;
  ImAnimatedVec2 menu_button_frame_max_animated;
  bool had_hovered_menu_item = false;
  bool has_hovered_menu_item = false;
  bool rendered_menu_item_border = false;

  bool file_selector_open = false;
  bool file_selector_directory = false;
  bool file_selector_directory_changed = false;
  std::string file_selector_title;
  ImGuiFullscreen::FileSelectorCallback file_selector_callback;
  std::string file_selector_current_directory;
  std::vector<std::string> file_selector_filters;
  std::vector<FileSelectorItem> file_selector_items;

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

void ImGuiFullscreen::SetFonts(ImFont* medium_font, ImFont* large_font)
{
  UIStyle.MediumFont = medium_font;
  UIStyle.LargeFont = large_font;
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
  UIStyle.MediumFont = nullptr;
  UIStyle.LargeFont = nullptr;

  s_state.texture_cache.Clear();

  if (clear_state)
  {
    s_state.fullscreen_footer_icon_mapping = {};
    s_state.notifications.clear();
    s_state.background_progress_dialogs.clear();
    s_state.fullscreen_footer_text.clear();
    s_state.last_fullscreen_footer_text.clear();
    s_state.fullscreen_text_change_time = 0.0f;
    CloseInputDialog();
    CloseMessageDialog();
    s_state.choice_dialog_open = false;
    s_state.choice_dialog_checkable = false;
    s_state.choice_dialog_title = {};
    s_state.choice_dialog_options.clear();
    s_state.choice_dialog_callback = {};
    s_state.enum_choice_button_id = 0;
    s_state.enum_choice_button_value = 0;
    s_state.enum_choice_button_set = false;
    s_state.file_selector_open = false;
    s_state.file_selector_directory = false;
    s_state.file_selector_title = {};
    s_state.file_selector_callback = {};
    s_state.file_selector_current_directory = {};
    s_state.file_selector_filters.clear();
    s_state.file_selector_items.clear();
    s_state.message_dialog_open = false;
    s_state.message_dialog_title = {};
    s_state.message_dialog_message = {};
    s_state.message_dialog_buttons = {};
    s_state.message_dialog_callback = {};
  }
}

void ImGuiFullscreen::SetSmoothScrolling(bool enabled)
{
  s_state.smooth_scrolling = enabled;
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

  return (UIStyle.LayoutScale != old_scale);

#else

  // On Android, treat a rotated display as always being in landscape mode for FSUI scaling.
  // Makes achievement popups readable regardless of the device's orientation, and avoids layout changes.
  const ImGuiIO& io = ImGui::GetIO();
  const float old_scale = UIStyle.LayoutScale;
  UIStyle.LayoutScale = std::max(io.DisplaySize.x, io.DisplaySize.y) / LAYOUT_SCREEN_WIDTH;
  UIStyle.RcpLayoutScale = 1.0f / UIStyle.LayoutScale;
  return (UIStyle.LayoutScale != old_scale);

#endif
}

ImRect ImGuiFullscreen::CenterImage(const ImVec2& fit_size, const ImVec2& image_size)
{
  const float fit_ar = fit_size.x / fit_size.y;
  const float image_ar = image_size.x / image_size.y;

  ImRect ret;
  if (fit_ar > image_ar)
  {
    // center horizontally
    const float width = fit_size.y * image_ar;
    const float offset = (fit_size.x - width) / 2.0f;
    const float height = fit_size.y;
    ret = ImRect(ImVec2(offset, 0.0f), ImVec2(offset + width, height));
  }
  else
  {
    // center vertically
    const float height = fit_size.x / image_ar;
    const float offset = (fit_size.y - height) / 2.0f;
    const float width = fit_size.x;
    ret = ImRect(ImVec2(0.0f, offset), ImVec2(width, offset + height));
  }

  return ret;
}

ImRect ImGuiFullscreen::CenterImage(const ImRect& fit_rect, const ImVec2& image_size)
{
  ImRect ret(CenterImage(fit_rect.Max - fit_rect.Min, image_size));
  ret.Translate(fit_rect.Min);
  return ret;
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
  DrawFileSelector();
  DrawChoiceDialog();
  DrawInputDialog();
  DrawMessageDialog();

  DrawFullscreenFooter();

  PopResetLayout();

  s_state.fullscreen_footer_text.clear();

  s_state.rendered_menu_item_border = false;
  s_state.had_hovered_menu_item = std::exchange(s_state.has_hovered_menu_item, false);
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
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollSmooth, s_state.smooth_scrolling ? SMOOTH_SCROLLING_SPEED : 1.0f);
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
}

void ImGuiFullscreen::PopResetLayout()
{
  ImGui::PopStyleColor(11);
  ImGui::PopStyleVar(13);
}

void ImGuiFullscreen::QueueResetFocus(FocusResetType type)
{
  s_state.focus_reset_queued = type;
  s_state.close_button_state =
    (s_state.close_button_state != CloseButtonState::Cancelled) ? CloseButtonState::None : CloseButtonState::Cancelled;
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
  if (!GImGui->NavDisableHighlight && GImGui->NavDisableMouseHover)
  {
    window->Appearing = true;
    s_state.has_hovered_menu_item = s_state.had_hovered_menu_item;
  }

  ImGui::SetWindowFocus();

  // If this is a popup closing, we don't want to reset the current nav item, since we were presumably opened by one.
  if (s_state.focus_reset_queued != FocusResetType::PopupClosed)
    ImGui::NavInitWindow(window, true);

  s_state.focus_reset_queued = FocusResetType::None;

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
  g.NavDisableHighlight = false;
  g.NavDisableMouseHover = true;
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
    ImGui::PushFont(UIStyle.LargeFont);
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

bool ImGuiFullscreen::BeginFullscreenColumnWindow(float start, float end, const char* name, const ImVec4& background)
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

  ImGui::SetCursorPos(pos);

  return ImGui::BeginChild(name, size, false, ImGuiWindowFlags_NavFlattened);
}

void ImGuiFullscreen::EndFullscreenColumnWindow()
{
  ImGui::EndChild();
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
                        ImGuiWindowFlags_NoBringToFrontOnFocus | flags);
}

void ImGuiFullscreen::EndFullscreenWindow()
{
  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();
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
  if (s_state.fullscreen_footer_text.empty())
  {
    s_state.last_fullscreen_footer_text.clear();
    return;
  }

  const float padding = LayoutScale(LAYOUT_FOOTER_PADDING);
  const float height = LayoutScale(LAYOUT_FOOTER_HEIGHT);

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0.0f, io.DisplaySize.y - height), io.DisplaySize,
                    ImGui::GetColorU32(ModAlpha(UIStyle.PrimaryColor, s_state.fullscreen_text_alpha)), 0.0f);

  ImFont* const font = UIStyle.MediumFont;
  const float max_width = io.DisplaySize.x - padding * 2.0f;

  float prev_opacity = 0.0f;
  if (!s_state.last_fullscreen_footer_text.empty() &&
      s_state.fullscreen_footer_text != s_state.last_fullscreen_footer_text)
  {
    if (s_state.fullscreen_text_change_time == 0.0f)
      s_state.fullscreen_text_change_time = 0.15f;
    else
      s_state.fullscreen_text_change_time = std::max(s_state.fullscreen_text_change_time - io.DeltaTime, 0.0f);

    if (s_state.fullscreen_text_change_time == 0.0f)
      s_state.last_fullscreen_footer_text = s_state.fullscreen_footer_text;

    prev_opacity = s_state.fullscreen_text_change_time * (1.0f / 0.15f);
    if (prev_opacity > 0.0f)
    {
      const ImVec2 text_size =
        font->CalcTextSizeA(font->FontSize, max_width, 0.0f, s_state.last_fullscreen_footer_text.c_str(),
                            s_state.last_fullscreen_footer_text.end_ptr());
      dl->AddText(font, font->FontSize,
                  ImVec2(io.DisplaySize.x - padding * 2.0f - text_size.x, io.DisplaySize.y - font->FontSize - padding),
                  ImGui::GetColorU32(ImVec4(UIStyle.PrimaryTextColor.x, UIStyle.PrimaryTextColor.y,
                                            UIStyle.PrimaryTextColor.z, prev_opacity)),
                  s_state.last_fullscreen_footer_text.c_str(), s_state.last_fullscreen_footer_text.end_ptr());
    }
  }
  else if (s_state.last_fullscreen_footer_text.empty())
  {
    s_state.last_fullscreen_footer_text = s_state.fullscreen_footer_text;
  }

  if (prev_opacity < 1.0f)
  {
    const ImVec2 text_size =
      font->CalcTextSizeA(font->FontSize, max_width, 0.0f, s_state.fullscreen_footer_text.c_str(),
                          s_state.fullscreen_footer_text.end_ptr());
    dl->AddText(font, font->FontSize,
                ImVec2(io.DisplaySize.x - padding * 2.0f - text_size.x, io.DisplaySize.y - font->FontSize - padding),
                ImGui::GetColorU32(ImVec4(UIStyle.PrimaryTextColor.x, UIStyle.PrimaryTextColor.y,
                                          UIStyle.PrimaryTextColor.z, 1.0f - prev_opacity)),
                s_state.fullscreen_footer_text.c_str(), s_state.fullscreen_footer_text.end_ptr());
  }

  // for next frame
  s_state.fullscreen_text_alpha = 1.0f;
}

void ImGuiFullscreen::PrerenderMenuButtonBorder()
{
  if (!s_state.had_hovered_menu_item)
    return;

  // updating might finish the animation
  const ImVec2& min = s_state.menu_button_frame_min_animated.UpdateAndGetValue();
  const ImVec2& max = s_state.menu_button_frame_max_animated.UpdateAndGetValue();
  const ImU32 col = ImGui::GetColorU32(ImGuiCol_ButtonHovered);

  const float t = static_cast<float>(std::min(std::abs(std::sin(ImGui::GetTime() * 0.75) * 1.1), 1.0));
  ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Border, t));

  ImGui::RenderFrame(min, max, col, true, 0.0f);

  ImGui::PopStyleColor();

  s_state.rendered_menu_item_border = true;
}

void ImGuiFullscreen::BeginMenuButtons(u32 num_items, float y_align, float x_padding, float y_padding,
                                       float item_height)
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
          ImGui::SetScrollY(std::max(ImGui::GetScrollY() - item_height, 0.0f));
          break;
        case ImGuiDir_Down:
          ImGui::SetScrollY(std::min(ImGui::GetScrollY() + item_height, ImGui::GetScrollMaxY()));
          break;
        default:
          break;
      }
    }

    s_state.has_pending_nav_move = ImGuiDir_None;
  }

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(x_padding, y_padding));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, LayoutScale(1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

  if (y_align != 0.0f)
  {
    const float real_item_height = LayoutScale(item_height) + (LayoutScale(y_padding) * 2.0f);
    const float total_size = (static_cast<float>(num_items) * real_item_height) + (LayoutScale(y_padding) * 2.0f);
    const float window_height = ImGui::GetWindowHeight();
    if (window_height > total_size)
      ImGui::SetCursorPosY((window_height - total_size) * y_align);
  }

  PrerenderMenuButtonBorder();
}

void ImGuiFullscreen::EndMenuButtons()
{
  ImGui::PopStyleVar(4);
}

void ImGuiFullscreen::DrawWindowTitle(const char* title)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  const ImVec2 pos(window->DC.CursorPos + LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
  const ImVec2 size(window->WorkRect.GetWidth() - (LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING) * 2.0f),
                    UIStyle.LargeFont->FontSize + LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f);
  const ImRect rect(pos, pos + size);

  ImGui::ItemSize(size);
  if (!ImGui::ItemAdd(rect, window->GetID("window_title")))
    return;

  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::RenderTextClipped(rect.Min, rect.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &rect);
  ImGui::PopFont();

  const ImVec2 line_start(pos.x, pos.y + UIStyle.LargeFont->FontSize + LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING));
  const ImVec2 line_end(pos.x + size.x, line_start.y);
  const float line_thickness = LayoutScale(1.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddLine(line_start, line_end, IM_COL32(255, 255, 255, 255), line_thickness);
}

void ImGuiFullscreen::GetMenuButtonFrameBounds(float height, ImVec2* pos, ImVec2* size)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  *pos = window->DC.CursorPos;
  *size = ImVec2(window->WorkRect.GetWidth(), LayoutScale(height) + ImGui::GetStyle().FramePadding.y * 2.0f);
}

bool ImGuiFullscreen::MenuButtonFrame(const char* str_id, bool enabled, float height, bool* visible, bool* hovered,
                                      ImRect* bb, ImGuiButtonFlags flags, float hover_alpha)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
  {
    *visible = false;
    *hovered = false;
    return false;
  }

  ImVec2 pos, size;
  GetMenuButtonFrameBounds(height, &pos, &size);
  *bb = ImRect(pos, pos + size);

  const ImGuiID id = window->GetID(str_id);
  ImGui::ItemSize(size);
  if (enabled)
  {
    if (!ImGui::ItemAdd(*bb, id))
    {
      *visible = false;
      *hovered = false;
      return false;
    }
  }
  else
  {
    if (ImGui::IsClippedEx(*bb, id))
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
    pressed = ImGui::ButtonBehavior(*bb, id, hovered, &held, flags);
    if (*hovered)
    {
      const ImU32 col = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered, hover_alpha);

      const float t = static_cast<float>(std::min(std::abs(std::sin(ImGui::GetTime() * 0.75) * 1.1), 1.0));
      ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Border, t));

      DrawMenuButtonFrame(bb->Min, bb->Max, col, true, 0.0f);

      ImGui::PopStyleColor();
    }
  }
  else
  {
    pressed = false;
    held = false;
  }

  const ImGuiStyle& style = ImGui::GetStyle();
  bb->Min += style.FramePadding;
  bb->Max -= style.FramePadding;

  return pressed;
}

void ImGuiFullscreen::DrawMenuButtonFrame(const ImVec2& p_min, const ImVec2& p_max, ImU32 fill_col,
                                          bool border /* = true */, float rounding /* = 0.0f */)
{
  ImVec2 frame_min = p_min;
  ImVec2 frame_max = p_max;

  const ImGuiIO& io = ImGui::GetIO();
  if (s_state.smooth_scrolling && io.NavVisible)
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
    ImGui::RenderFrame(frame_min, frame_max, fill_col, border, rounding);
  }
}

bool ImGuiFullscreen::MenuButtonFrame(const char* str_id, bool enabled, float height, bool* visible, bool* hovered,
                                      ImVec2* min, ImVec2* max, ImGuiButtonFlags flags /*= 0*/,
                                      float hover_alpha /*= 0*/)
{
  ImRect bb;
  const bool result = MenuButtonFrame(str_id, enabled, height, visible, hovered, &bb, flags, hover_alpha);
  *min = bb.Min;
  *max = bb.Max;
  return result;
}

void ImGuiFullscreen::ResetMenuButtonFrame()
{
  s_state.had_hovered_menu_item = false;
  s_state.has_hovered_menu_item = false;
}

void ImGuiFullscreen::MenuHeading(const char* title, bool draw_line /*= true*/)
{
  const float line_thickness = draw_line ? LayoutScale(1.0f) : 0.0f;
  const float line_padding = draw_line ? LayoutScale(5.0f) : 0.0f;

  bool visible, hovered;
  ImRect bb;
  MenuButtonFrame(title, false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, &visible, &hovered, &bb);
  if (!visible)
    return;

  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::RenderTextClipped(bb.Min, bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &bb);
  ImGui::PopFont();
  ImGui::PopStyleColor();

  if (draw_line)
  {
    const ImVec2 line_start(bb.Min.x, bb.Min.y + UIStyle.LargeFont->FontSize + line_padding);
    const ImVec2 line_end(bb.Max.x, line_start.y);
    ImGui::GetWindowDrawList()->AddLine(line_start, line_end, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                        line_thickness);
  }
}

bool ImGuiFullscreen::MenuHeadingButton(const char* title, const char* value /*= nullptr*/, bool enabled /*= true*/,
                                        bool draw_line /*= true*/)
{
  const float line_thickness = draw_line ? LayoutScale(1.0f) : 0.0f;
  const float line_padding = draw_line ? LayoutScale(5.0f) : 0.0f;

  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, &visible, &hovered, &bb);
  if (!visible)
    return false;

  if (!enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::RenderTextClipped(bb.Min, bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &bb);

  if (value)
  {
    const ImVec2 value_size(
      UIStyle.LargeFont->CalcTextSizeA(UIStyle.LargeFont->FontSize, std::numeric_limits<float>::max(), 0.0f, value));
    const ImRect value_bb(ImVec2(bb.Max.x - value_size.x, bb.Min.y), ImVec2(bb.Max.x, bb.Max.y));
    ImGui::RenderTextClipped(value_bb.Min, value_bb.Max, value, nullptr, nullptr, ImVec2(0.0f, 0.0f), &value_bb);
  }

  ImGui::PopFont();
  if (!enabled)
    ImGui::PopStyleColor();

  if (draw_line)
  {
    const ImVec2 line_start(bb.Min.x, bb.Min.y + UIStyle.LargeFont->FontSize + line_padding);
    const ImVec2 line_end(bb.Max.x, line_start.y);
    ImGui::GetWindowDrawList()->AddLine(line_start, line_end, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                        line_thickness);
  }

  return pressed;
}

bool ImGuiFullscreen::ActiveButton(const char* title, bool is_active, bool enabled, float height, ImFont* font)
{
  return ActiveButtonWithRightText(title, nullptr, is_active, enabled, height, font);
}

bool ImGuiFullscreen::DefaultActiveButton(const char* title, bool is_active, bool enabled /* = true */,
                                          float height /* = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY */,
                                          ImFont* font /* = g_large_font */)
{
  const bool result = ActiveButtonWithRightText(title, nullptr, is_active, enabled, height, font);
  ImGui::SetItemDefaultFocus();
  return result;
}

bool ImGuiFullscreen::ActiveButtonWithRightText(const char* title, const char* right_title, bool is_active,
                                                bool enabled, float height, ImFont* font)
{
  if (is_active)
  {
    // don't draw over a prerendered border
    const float border_size = ImGui::GetStyle().FrameBorderSize;
    const ImVec2 border_size_v = ImVec2(border_size, border_size);
    ImVec2 pos, size;
    GetMenuButtonFrameBounds(height, &pos, &size);
    ImGui::RenderFrame(pos + border_size_v, pos + size - border_size_v, ImGui::GetColorU32(UIStyle.PrimaryColor),
                       false);
  }

  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, height, &visible, &hovered, &bb);
  if (!visible)
    return false;

  const ImRect title_bb(bb.GetTL(), bb.GetBR());

  if (!enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));

  ImGui::PushFont(font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);

  if (right_title && *right_title)
  {
    const ImVec2 right_text_size = font->CalcTextSizeA(font->FontSize, title_bb.GetWidth(), 0.0f, right_title);
    const ImVec2 right_text_start = ImVec2(title_bb.Max.x - right_text_size.x, title_bb.Min.y);
    ImGui::RenderTextClipped(right_text_start, title_bb.Max, right_title, nullptr, &right_text_size, ImVec2(0.0f, 0.0f),
                             &title_bb);
  }

  ImGui::PopFont();

  if (!enabled)
    ImGui::PopStyleColor();

  s_state.menu_button_index++;
  return pressed;
}

bool ImGuiFullscreen::MenuButton(const char* title, const char* summary, bool enabled, float height, ImFont* font,
                                 ImFont* summary_font)
{
  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, height, &visible, &hovered, &bb);
  if (!visible)
    return false;

  const float midpoint = bb.Min.y + font->FontSize + LayoutScale(4.0f);
  const ImRect title_bb(bb.Min, ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(bb.Min.x, midpoint), bb.Max);

  if (!enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));

  ImGui::PushFont(font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  if (summary)
  {
    ImGui::PushFont(summary_font);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, summary, nullptr, nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (!enabled)
    ImGui::PopStyleColor();

  s_state.menu_button_index++;
  return pressed;
}

bool ImGuiFullscreen::MenuButtonWithoutSummary(const char* title, bool enabled, float height, ImFont* font,
                                               const ImVec2& text_align)
{
  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, height, &visible, &hovered, &bb);
  if (!visible)
    return false;

  const float midpoint = bb.Min.y + font->FontSize + LayoutScale(4.0f);
  const ImRect title_bb(bb.Min, ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(bb.Min.x, midpoint), bb.Max);

  if (!enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));

  ImGui::PushFont(font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, title, nullptr, nullptr, text_align, &title_bb);
  ImGui::PopFont();

  if (!enabled)
    ImGui::PopStyleColor();

  s_state.menu_button_index++;
  return pressed;
}

bool ImGuiFullscreen::MenuImageButton(const char* title, const char* summary, ImTextureID user_texture_id,
                                      const ImVec2& image_size, bool enabled, float height, const ImVec2& uv0,
                                      const ImVec2& uv1, ImFont* title_font, ImFont* summary_font)
{
  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, height, &visible, &hovered, &bb);
  if (!visible)
    return false;

  ImGui::GetWindowDrawList()->AddImage(user_texture_id, bb.Min, bb.Min + image_size, uv0, uv1,
                                       enabled ? IM_COL32(255, 255, 255, 255) :
                                                 ImGui::GetColorU32(ImGuiCol_TextDisabled));

  const float midpoint = bb.Min.y + title_font->FontSize + LayoutScale(4.0f);
  const float text_start_x = bb.Min.x + image_size.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint), bb.Max);

  if (!enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));

  ImGui::PushFont(title_font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  if (summary)
  {
    ImGui::PushFont(summary_font);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, summary, nullptr, nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (!enabled)
    ImGui::PopStyleColor();

  s_state.menu_button_index++;
  return pressed;
}

bool ImGuiFullscreen::FloatingButton(const char* text, float x, float y, float width, float height, float anchor_x,
                                     float anchor_y, bool enabled, ImFont* font, ImVec2* out_position,
                                     bool repeat_button)
{
  const ImVec2 text_size(font->CalcTextSizeA(font->FontSize, std::numeric_limits<float>::max(), 0.0f, text));
  const ImVec2& padding(ImGui::GetStyle().FramePadding);
  if (width < 0.0f)
    width = (padding.x * 2.0f) + text_size.x;
  if (height < 0.0f)
    height = (padding.y * 2.0f) + text_size.y;

  const ImVec2 window_size(ImGui::GetWindowSize());
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

  const ImGuiID id = window->GetID(text);
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
    pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, repeat_button ? ImGuiButtonFlags_Repeat : 0);
    if (hovered)
    {
      const float t = std::min(static_cast<float>(std::abs(std::sin(ImGui::GetTime() * 0.75) * 1.1)), 1.0f);
      const ImU32 col = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered, 1.0f);
      ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_Border, t));
      DrawMenuButtonFrame(bb.Min, bb.Max, col, true, 0.0f);
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

  if (!enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));

  ImGui::PushFont(font);
  ImGui::RenderTextClipped(bb.Min, bb.Max, text, nullptr, nullptr, ImVec2(0.0f, 0.0f), &bb);
  ImGui::PopFont();

  if (!enabled)
    ImGui::PopStyleColor();

  return pressed;
}

bool ImGuiFullscreen::ToggleButton(const char* title, const char* summary, bool* v, bool enabled, float height,
                                   ImFont* font, ImFont* summary_font)
{
  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, height, &visible, &hovered, &bb, ImGuiButtonFlags_PressedOnClick);
  if (!visible)
    return false;

  const float midpoint = bb.Min.y + font->FontSize + LayoutScale(4.0f);
  const ImRect title_bb(bb.Min, ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(bb.Min.x, midpoint), bb.Max);

  if (!enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));

  ImGui::PushFont(font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  if (summary)
  {
    ImGui::PushFont(summary_font);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, summary, nullptr, nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (!enabled)
    ImGui::PopStyleColor();

  const float toggle_width = LayoutScale(50.0f);
  const float toggle_height = LayoutScale(25.0f);
  const float toggle_x = LayoutScale(8.0f);
  const float toggle_y = (LayoutScale(height) - toggle_height) * 0.5f;
  const float toggle_radius = toggle_height * 0.5f;
  const ImVec2 toggle_pos(bb.Max.x - toggle_width - toggle_x, bb.Min.y + toggle_y);

  if (pressed)
    *v = !*v;

  float t = *v ? 1.0f : 0.0f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiContext& g = *GImGui;
  if (g.LastActiveId == g.CurrentWindow->GetID(title)) // && g.LastActiveIdTimer < ANIM_SPEED)
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

  dl->AddRectFilled(toggle_pos, ImVec2(toggle_pos.x + toggle_width, toggle_pos.y + toggle_height), col_bg,
                    toggle_height * 0.5f);
  dl->AddCircleFilled(
    ImVec2(toggle_pos.x + toggle_radius + t * (toggle_width - toggle_radius * 2.0f), toggle_pos.y + toggle_radius),
    toggle_radius - 1.5f, col_knob, 32);

  s_state.menu_button_index++;
  return pressed;
}

bool ImGuiFullscreen::ThreeWayToggleButton(const char* title, const char* summary, std::optional<bool>* v, bool enabled,
                                           float height, ImFont* font, ImFont* summary_font)
{
  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, height, &visible, &hovered, &bb, ImGuiButtonFlags_PressedOnClick);
  if (!visible)
    return false;

  const float midpoint = bb.Min.y + font->FontSize + LayoutScale(4.0f);
  const ImRect title_bb(bb.Min, ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(bb.Min.x, midpoint), bb.Max);

  if (!enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));

  ImGui::PushFont(font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  if (summary)
  {
    ImGui::PushFont(summary_font);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, summary, nullptr, nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (!enabled)
    ImGui::PopStyleColor();

  const float toggle_width = LayoutScale(50.0f);
  const float toggle_height = LayoutScale(25.0f);
  const float toggle_x = LayoutScale(8.0f);
  const float toggle_y = (LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT) - toggle_height) * 0.5f;
  const float toggle_radius = toggle_height * 0.5f;
  const ImVec2 toggle_pos(bb.Max.x - toggle_width - toggle_x, bb.Min.y + toggle_y);

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
  if (g.LastActiveId == g.CurrentWindow->GetID(title)) // && g.LastActiveIdTimer < ANIM_SPEED)
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

  dl->AddRectFilled(toggle_pos, ImVec2(toggle_pos.x + toggle_width, toggle_pos.y + toggle_height), col_bg,
                    toggle_height * 0.5f);
  dl->AddCircleFilled(
    ImVec2(toggle_pos.x + toggle_radius + t * (toggle_width - toggle_radius * 2.0f), toggle_pos.y + toggle_radius),
    toggle_radius - 1.5f, IM_COL32(255, 255, 255, 255), 32);

  s_state.menu_button_index++;
  return pressed;
}

bool ImGuiFullscreen::RangeButton(const char* title, const char* summary, s32* value, s32 min, s32 max, s32 increment,
                                  const char* format, bool enabled /*= true*/,
                                  float height /*= LAYOUT_MENU_BUTTON_HEIGHT*/, ImFont* font /*= g_large_font*/,
                                  ImFont* summary_font /*= g_medium_font*/, const char* ok_text /*= "OK"*/)
{
  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, height, &visible, &hovered, &bb);
  if (!visible)
    return false;

  const SmallString value_text = SmallString::from_sprintf(format, *value);
  const ImVec2 value_size(ImGui::CalcTextSize(value_text.c_str()));

  const float midpoint = bb.Min.y + font->FontSize + LayoutScale(4.0f);
  const float text_end = bb.Max.x - value_size.x;
  const ImRect title_bb(bb.Min, ImVec2(text_end, midpoint));
  const ImRect summary_bb(ImVec2(bb.Min.x, midpoint), ImVec2(text_end, bb.Max.y));

  if (!enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));

  ImGui::PushFont(font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::RenderTextClipped(bb.Min, bb.Max, value_text.c_str(), nullptr, nullptr, ImVec2(1.0f, 0.5f), &bb);
  ImGui::PopFont();

  if (summary)
  {
    ImGui::PushFont(summary_font);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, summary, nullptr, nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (!enabled)
    ImGui::PopStyleColor();

  if (pressed)
    ImGui::OpenPopup(title);

  bool changed = false;

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 192.0f));
  ImGui::SetNextWindowPos((ImGui::GetIO().DisplaySize - LayoutScale(0.0f, LAYOUT_FOOTER_HEIGHT)) * 0.5f,
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  if (ImGui::BeginPopupModal(title, nullptr,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
  {
    BeginMenuButtons();

    const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    ImGui::SetNextItemWidth(end);

    changed = ImGui::SliderInt("##value", value, min, max, format, ImGuiSliderFlags_NoInput);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));
    if (MenuButtonWithoutSummary(ok_text, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, UIStyle.LargeFont,
                                 ImVec2(0.5f, 0.0f)))
      ImGui::CloseCurrentPopup();
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(4);
  ImGui::PopFont();

  return changed;
}

bool ImGuiFullscreen::RangeButton(const char* title, const char* summary, float* value, float min, float max,
                                  float increment, const char* format, bool enabled /*= true*/,
                                  float height /*= LAYOUT_MENU_BUTTON_HEIGHT*/, ImFont* font /*= g_large_font*/,
                                  ImFont* summary_font /*= g_medium_font*/, const char* ok_text /*= "OK"*/)
{
  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, height, &visible, &hovered, &bb);
  if (!visible)
    return false;

  const SmallString value_text = SmallString::from_sprintf(format, *value);
  const ImVec2 value_size(ImGui::CalcTextSize(value_text.c_str()));

  const float midpoint = bb.Min.y + font->FontSize + LayoutScale(4.0f);
  const float text_end = bb.Max.x - value_size.x;
  const ImRect title_bb(bb.Min, ImVec2(text_end, midpoint));
  const ImRect summary_bb(ImVec2(bb.Min.x, midpoint), ImVec2(text_end, bb.Max.y));

  if (!enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));

  ImGui::PushFont(font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::RenderTextClipped(bb.Min, bb.Max, value_text.c_str(), nullptr, nullptr, ImVec2(1.0f, 0.5f), &bb);
  ImGui::PopFont();

  if (summary)
  {
    ImGui::PushFont(summary_font);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, summary, nullptr, nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (!enabled)
    ImGui::PopStyleColor();

  if (pressed)
    ImGui::OpenPopup(title);

  bool changed = false;

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 192.0f));
  ImGui::SetNextWindowPos((ImGui::GetIO().DisplaySize - LayoutScale(0.0f, LAYOUT_FOOTER_HEIGHT)) * 0.5f,
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));

  if (ImGui::BeginPopupModal(title, nullptr,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
  {
    BeginMenuButtons();

    const float end = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    ImGui::SetNextItemWidth(end);

    changed = ImGui::SliderFloat("##value", value, min, max, format, ImGuiSliderFlags_NoInput);

    if (MenuButtonWithoutSummary(ok_text, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, UIStyle.LargeFont,
                                 ImVec2(0.5f, 0.0f)))
      ImGui::CloseCurrentPopup();
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(4);
  ImGui::PopFont();

  return changed;
}

bool ImGuiFullscreen::MenuButtonWithValue(const char* title, const char* summary, const char* value, bool enabled,
                                          float height, ImFont* font, ImFont* summary_font)
{
  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(title, enabled, height, &visible, &hovered, &bb);
  if (!visible)
    return false;

  const ImVec2 value_size(ImGui::CalcTextSize(value));

  const float midpoint = bb.Min.y + font->FontSize + LayoutScale(4.0f);
  const float text_end = bb.Max.x - value_size.x;
  const ImRect title_bb(bb.Min, ImVec2(text_end, midpoint));
  const ImRect summary_bb(ImVec2(bb.Min.x, midpoint), ImVec2(text_end, bb.Max.y));

  if (!enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));

  ImGui::PushFont(font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::RenderTextClipped(bb.Min, bb.Max, value, nullptr, nullptr, ImVec2(1.0f, 0.5f), &bb);
  ImGui::PopFont();

  if (summary)
  {
    ImGui::PushFont(summary_font);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, summary, nullptr, nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (!enabled)
    ImGui::PopStyleColor();

  return pressed;
}

bool ImGuiFullscreen::EnumChoiceButtonImpl(const char* title, const char* summary, s32* value_pointer,
                                           const char* (*to_display_name_function)(s32 value, void* opaque),
                                           void* opaque, u32 count, bool enabled, float height, ImFont* font,
                                           ImFont* summary_font)
{
  const bool pressed = MenuButtonWithValue(title, summary, to_display_name_function(*value_pointer, opaque), enabled,
                                           height, font, summary_font);

  if (pressed)
  {
    s_state.enum_choice_button_id = ImGui::GetID(title);
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
  if (s_state.enum_choice_button_set && s_state.enum_choice_button_id == ImGui::GetID(title))
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

void ImGuiFullscreen::DrawShadowedText(ImDrawList* dl, ImFont* font, const ImVec2& pos, u32 col, const char* text,
                                       const char* text_end /*= nullptr*/, float wrap_width /*= 0.0f*/)
{
  dl->AddText(font, font->FontSize, pos + LayoutScale(1.0f, 1.0f),
              s_state.light_theme ? IM_COL32(255, 255, 255, 100) : IM_COL32(0, 0, 0, 100), text, text_end, wrap_width);
  dl->AddText(font, font->FontSize, pos, col, text, text_end, wrap_width);
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

void ImGuiFullscreen::NavTitle(const char* title, float height /*= LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY*/,
                               ImFont* font /*= g_large_font*/)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return;

  s_state.menu_button_index++;

  const ImVec2 text_size(font->CalcTextSizeA(font->FontSize, std::numeric_limits<float>::max(), 0.0f, title));
  const ImVec2 pos(window->DC.CursorPos);
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 size = ImVec2(text_size.x, LayoutScale(height) + style.FramePadding.y * 2.0f);

  ImGui::ItemSize(
    ImVec2(size.x + style.FrameBorderSize + style.ItemSpacing.x, size.y + style.FrameBorderSize + style.ItemSpacing.y));
  ImGui::SameLine();

  ImRect bb(pos, pos + size);
  if (ImGui::IsClippedEx(bb, 0))
    return;

  bb.Min.y += style.FramePadding.y;
  bb.Max.y -= style.FramePadding.y;

  ImGui::PushFont(font);
  ImGui::RenderTextClipped(bb.Min, bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &bb);
  ImGui::PopFont();
}

void ImGuiFullscreen::RightAlignNavButtons(u32 num_items /*= 0*/,
                                           float item_width /*= LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY*/,
                                           float item_height /*= LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY*/)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  const ImGuiStyle& style = ImGui::GetStyle();

  const float total_item_width =
    style.FramePadding.x * 2.0f + style.FrameBorderSize + style.ItemSpacing.x + LayoutScale(item_width);
  const float margin = total_item_width * static_cast<float>(num_items);
  ImGui::SetCursorPosX(window->InnerClipRect.Max.x - margin - style.FramePadding.x);
}

bool ImGuiFullscreen::NavButton(const char* title, bool is_active, bool enabled /* = true */, float width /* = -1.0f */,
                                float height /* = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY */,
                                ImFont* font /* = g_large_font */)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;

  s_state.menu_button_index++;

  const ImVec2 text_size(font->CalcTextSizeA(font->FontSize, std::numeric_limits<float>::max(), 0.0f, title));
  const ImVec2 pos(window->DC.CursorPos);
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 size = ImVec2(((width < 0.0f) ? text_size.x : LayoutScale(width)) + style.FramePadding.x * 2.0f,
                             LayoutScale(height) + style.FramePadding.y * 2.0f);

  ImGui::ItemSize(
    ImVec2(size.x + style.FrameBorderSize + style.ItemSpacing.x, size.y + style.FrameBorderSize + style.ItemSpacing.y));
  ImGui::SameLine();

  ImRect bb(pos, pos + size);
  const ImGuiID id = window->GetID(title);
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
      DrawMenuButtonFrame(bb.Min, bb.Max, col, true, 0.0f);
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

  ImGui::PushStyleColor(
    ImGuiCol_Text,
    ImGui::GetColorU32(enabled ? (is_active ? ImGuiCol_Text : ImGuiCol_TextDisabled) : ImGuiCol_ButtonHovered));

  ImGui::PushFont(font);
  ImGui::RenderTextClipped(bb.Min, bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &bb);
  ImGui::PopFont();

  ImGui::PopStyleColor();

  return pressed;
}

bool ImGuiFullscreen::NavTab(const char* title, bool is_active, bool enabled /* = true */, float width, float height,
                             const ImVec4& background, ImFont* font /* = g_large_font */)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;

  s_state.menu_button_index++;

  const ImVec2 text_size(font->CalcTextSizeA(font->FontSize, std::numeric_limits<float>::max(), 0.0f, title));
  const ImVec2 pos(window->DC.CursorPos);
  const ImVec2 size = ImVec2(((width < 0.0f) ? text_size.x : LayoutScale(width)), LayoutScale(height));

  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
  ImGui::ItemSize(ImVec2(size.x, size.y));
  ImGui::SameLine();
  ImGui::PopStyleVar();

  ImRect bb(pos, pos + size);
  const ImGuiID id = window->GetID(title);
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

  const ImU32 col =
    hovered ? ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered, 1.0f) :
              ImGui::GetColorU32(is_active ? background : ImVec4(background.x, background.y, background.z, 0.5f));

  if (hovered)
    DrawMenuButtonFrame(bb.Min, bb.Max, col, true, 0.0f);

  if (is_active)
  {
    const float line_thickness = LayoutScale(2.0f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(bb.Min.x, bb.Max.y - line_thickness),
                                        ImVec2(bb.Max.x, bb.Max.y - line_thickness),
                                        ImGui::GetColorU32(ImGuiCol_TextDisabled), line_thickness);
  }

  const ImVec2 pad(std::max((size.x - text_size.x) * 0.5f, 0.0f), std::max((size.y - text_size.y) * 0.5f, 0.0f));
  bb.Min += pad;
  bb.Max -= pad;

  ImGui::PushStyleColor(
    ImGuiCol_Text,
    ImGui::GetColorU32(enabled ? (is_active ? ImGuiCol_Text : ImGuiCol_TextDisabled) : ImGuiCol_ButtonHovered));

  ImGui::PushFont(font);
  ImGui::RenderTextClipped(bb.Min, bb.Max, title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &bb);
  ImGui::PopFont();

  ImGui::PopStyleColor();

  return pressed;
}

bool ImGuiFullscreen::BeginHorizontalMenu(const char* name, const ImVec2& position, const ImVec2& size,
                                          const ImVec4& bg_color, u32 num_items)
{
  s_state.menu_button_index = 0;

  const float item_padding = LayoutScale(LAYOUT_HORIZONTAL_MENU_PADDING);
  const float item_width = LayoutScale(LAYOUT_HORIZONTAL_MENU_ITEM_WIDTH);
  const float item_spacing = LayoutScale(30.0f);
  const float menu_width = static_cast<float>(num_items) * (item_width + item_spacing) - item_spacing;
  const float menu_height = LayoutScale(LAYOUT_HORIZONTAL_MENU_HEIGHT);

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(item_padding, item_padding));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, LayoutScale(1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(item_spacing, 0.0f));

  if (!BeginFullscreenWindow(position, size, name, bg_color, 0.0f, ImVec2()))
    return false;

  ImGui::SetCursorPos(ImVec2((size.x - menu_width) * 0.5f, (size.y - menu_height) * 0.5f));

  PrerenderMenuButtonBorder();
  return true;
}

void ImGuiFullscreen::EndHorizontalMenu()
{
  ImGui::PopStyleVar(4);
  EndFullscreenWindow();
}

bool ImGuiFullscreen::HorizontalMenuItem(GPUTexture* icon, const char* title, const char* description)
{
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;

  const ImVec2 pos = window->DC.CursorPos;
  const ImVec2 size = LayoutScale(LAYOUT_HORIZONTAL_MENU_ITEM_WIDTH, LAYOUT_HORIZONTAL_MENU_HEIGHT);
  ImRect bb = ImRect(pos, pos + size);

  const ImGuiID id = window->GetID(title);
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

    DrawMenuButtonFrame(bb.Min, bb.Max, col, true, 0.0f);

    ImGui::PopStyleColor();
  }

  const ImGuiStyle& style = ImGui::GetStyle();
  bb.Min += style.FramePadding;
  bb.Max -= style.FramePadding;

  const float avail_width = bb.Max.x - bb.Min.x;
  const float icon_size = LayoutScale(150.0f);
  const ImVec2 icon_pos = bb.Min + ImVec2((avail_width - icon_size) * 0.5f, 0.0f);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddImage(reinterpret_cast<ImTextureID>(icon), icon_pos, icon_pos + ImVec2(icon_size, icon_size));

  ImFont* title_font = UIStyle.LargeFont;
  const ImVec2 title_size =
    title_font->CalcTextSizeA(title_font->FontSize, std::numeric_limits<float>::max(), avail_width, title);
  const ImVec2 title_pos =
    ImVec2(bb.Min.x + (avail_width - title_size.x) * 0.5f, icon_pos.y + icon_size + LayoutScale(10.0f));
  const ImVec4 title_bb = ImVec4(title_pos.x, title_pos.y, title_pos.x + title_size.x, title_pos.y + title_size.y);

  dl->AddText(title_font, title_font->FontSize, title_pos, ImGui::GetColorU32(ImGuiCol_Text), title, nullptr, 0.0f,
              &title_bb);

  ImFont* desc_font = UIStyle.MediumFont;
  const ImVec2 desc_size =
    desc_font->CalcTextSizeA(desc_font->FontSize, std::numeric_limits<float>::max(), avail_width, description);
  const ImVec2 desc_pos = ImVec2(bb.Min.x + (avail_width - desc_size.x) * 0.5f, title_bb.w + LayoutScale(10.0f));
  const ImVec4 desc_bb = ImVec4(desc_pos.x, desc_pos.y, desc_pos.x + desc_size.x, desc_pos.y + desc_size.y);

  dl->AddText(desc_font, desc_font->FontSize, desc_pos, ImGui::GetColorU32(ImGuiCol_Text), description, nullptr,
              avail_width, &desc_bb);

  ImGui::SameLine();

  s_state.menu_button_index++;
  return pressed;
}

void ImGuiFullscreen::PopulateFileSelectorItems()
{
  s_state.file_selector_items.clear();

  if (s_state.file_selector_current_directory.empty())
  {
    for (std::string& root_path : FileSystem::GetRootDirectoryList())
      s_state.file_selector_items.emplace_back(fmt::format(ICON_FA_FOLDER " {}", root_path), std::move(root_path),
                                               false);
  }
  else
  {
    FileSystem::FindResultsArray results;
    FileSystem::FindFiles(s_state.file_selector_current_directory.c_str(), "*",
                          FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES |
                            FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_SORT_BY_NAME,
                          &results);

    std::string parent_path;
    std::string::size_type sep_pos = s_state.file_selector_current_directory.rfind(FS_OSPATH_SEPARATOR_CHARACTER);
    if (sep_pos != std::string::npos)
      parent_path = Path::Canonicalize(s_state.file_selector_current_directory.substr(0, sep_pos));

    s_state.file_selector_items.emplace_back(ICON_FA_FOLDER_OPEN "  <Parent Directory>", std::move(parent_path), false);

    for (const FILESYSTEM_FIND_DATA& fd : results)
    {
      std::string full_path =
        fmt::format("{}" FS_OSPATH_SEPARATOR_STR "{}", s_state.file_selector_current_directory, fd.FileName);

      if (fd.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
      {
        std::string title = fmt::format(ICON_FA_FOLDER " {}", fd.FileName);
        s_state.file_selector_items.emplace_back(std::move(title), std::move(full_path), false);
      }
      else
      {
        if (s_state.file_selector_filters.empty() ||
            std::none_of(s_state.file_selector_filters.begin(), s_state.file_selector_filters.end(),
                         [&fd](const std::string& filter) {
                           return StringUtil::WildcardMatch(fd.FileName.c_str(), filter.c_str(), false);
                         }))
        {
          continue;
        }

        std::string title = fmt::format(ICON_FA_FILE " {}", fd.FileName);
        s_state.file_selector_items.emplace_back(std::move(title), std::move(full_path), true);
      }
    }
  }
}

void ImGuiFullscreen::SetFileSelectorDirectory(std::string dir)
{
  while (!dir.empty() && dir.back() == FS_OSPATH_SEPARATOR_CHARACTER)
    dir.erase(dir.size() - 1);

  s_state.file_selector_current_directory = std::move(dir);
  PopulateFileSelectorItems();
}

bool ImGuiFullscreen::IsFileSelectorOpen()
{
  return s_state.file_selector_open;
}

void ImGuiFullscreen::OpenFileSelector(std::string_view title, bool select_directory, FileSelectorCallback callback,
                                       FileSelectorFilters filters, std::string initial_directory)
{
  if (initial_directory.empty() || !FileSystem::DirectoryExists(initial_directory.c_str()))
    initial_directory = FileSystem::GetWorkingDirectory();

  if (Host::ShouldPreferHostFileSelector())
  {
    Host::OpenHostFileSelectorAsync(ImGuiManager::StripIconCharacters(title), select_directory, std::move(callback),
                                    std::move(filters), initial_directory);
    return;
  }

  if (s_state.file_selector_open)
    CloseFileSelector();

  s_state.file_selector_open = true;
  s_state.file_selector_directory = select_directory;
  s_state.file_selector_directory_changed = true;
  s_state.file_selector_title = fmt::format("{}##file_selector", title);
  s_state.file_selector_callback = std::move(callback);
  s_state.file_selector_filters = std::move(filters);

  SetFileSelectorDirectory(std::move(initial_directory));
}

void ImGuiFullscreen::CloseFileSelector()
{
  if (!s_state.file_selector_open)
    return;

  if (ImGui::IsPopupOpen(s_state.file_selector_title.c_str(), 0))
    ImGui::ClosePopupToLevel(GImGui->OpenPopupStack.Size - 1, true);

  s_state.file_selector_open = false;
  s_state.file_selector_directory = false;
  s_state.file_selector_directory_changed = false;
  std::string().swap(s_state.file_selector_title);
  FileSelectorCallback().swap(s_state.file_selector_callback);
  FileSelectorFilters().swap(s_state.file_selector_filters);
  std::string().swap(s_state.file_selector_current_directory);
  s_state.file_selector_items.clear();
  ImGui::CloseCurrentPopup();
  QueueResetFocus(FocusResetType::PopupClosed);
}

void ImGuiFullscreen::DrawFileSelector()
{
  if (!s_state.file_selector_open)
    return;

  ImGui::SetNextWindowSize(LayoutScale(1000.0f, 650.0f));
  ImGui::SetNextWindowPos((ImGui::GetIO().DisplaySize - LayoutScale(0.0f, LAYOUT_FOOTER_HEIGHT)) * 0.5f,
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup(s_state.file_selector_title.c_str());

  FileSelectorItem* selected = nullptr;

  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.PrimaryTextColor);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, UIStyle.PrimaryDarkColor);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, UIStyle.PrimaryColor);

  bool is_open = !WantsToCloseMenu();
  bool directory_selected = false;
  if (ImGui::BeginPopupModal(s_state.file_selector_title.c_str(), &is_open,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
  {
    ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.BackgroundTextColor);

    if (s_state.file_selector_directory_changed)
    {
      s_state.file_selector_directory_changed = false;
      QueueResetFocus(FocusResetType::Other);
    }

    ResetFocusHere();
    BeginMenuButtons();

    if (!s_state.file_selector_current_directory.empty())
    {
      MenuButton(SmallString::from_format(ICON_FA_FOLDER_OPEN " {}", s_state.file_selector_current_directory).c_str(),
                 nullptr, false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
    }

    if (s_state.file_selector_directory && !s_state.file_selector_current_directory.empty())
    {
      if (MenuButton(ICON_FA_FOLDER_PLUS " <Use This Directory>", nullptr, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
        directory_selected = true;
    }

    for (FileSelectorItem& item : s_state.file_selector_items)
    {
      if (MenuButton(item.display_name.c_str(), nullptr, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
        selected = &item;
    }

    EndMenuButtons();

    ImGui::PopStyleColor(1);

    ImGui::EndPopup();
  }
  else
  {
    is_open = false;
  }

  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(3);
  ImGui::PopFont();

  if (is_open)
    GetFileSelectorHelpText(s_state.fullscreen_footer_text);

  if (selected)
  {
    if (selected->is_file)
    {
      std::string path = std::move(selected->full_path);
      const FileSelectorCallback callback = std::move(s_state.file_selector_callback);
      CloseFileSelector();
      callback(std::move(path));
    }
    else
    {
      SetFileSelectorDirectory(std::move(selected->full_path));
      s_state.file_selector_directory_changed = true;
    }
  }
  else if (directory_selected)
  {
    std::string path = std::move(s_state.file_selector_current_directory);
    const FileSelectorCallback callback = std::move(s_state.file_selector_callback);
    CloseFileSelector();
    callback(std::move(path));
  }
  else if (!is_open)
  {
    const FileSelectorCallback callback = std::move(s_state.file_selector_callback);
    CloseFileSelector();
    callback(std::string());
  }
  else
  {
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ImGui::IsKeyPressed(ImGuiKey_NavGamepadMenu, false))
    {
      if (!s_state.file_selector_items.empty() &&
          s_state.file_selector_items.front().display_name == ICON_FA_FOLDER_OPEN "  <Parent Directory>")
      {
        SetFileSelectorDirectory(std::move(s_state.file_selector_items.front().full_path));
        s_state.file_selector_directory_changed = true;
      }
    }
  }
}

bool ImGuiFullscreen::IsChoiceDialogOpen()
{
  return s_state.choice_dialog_open;
}

void ImGuiFullscreen::OpenChoiceDialog(std::string_view title, bool checkable, ChoiceDialogOptions options,
                                       ChoiceDialogCallback callback)
{
  if (s_state.choice_dialog_open)
    CloseChoiceDialog();

  s_state.choice_dialog_open = true;
  s_state.choice_dialog_checkable = checkable;
  s_state.choice_dialog_title = fmt::format("{}##choice_dialog", title);
  s_state.choice_dialog_options = std::move(options);
  s_state.choice_dialog_callback = std::move(callback);
  QueueResetFocus(FocusResetType::PopupOpened);
}

void ImGuiFullscreen::CloseChoiceDialog()
{
  if (!s_state.choice_dialog_open)
    return;

  if (ImGui::IsPopupOpen(s_state.choice_dialog_title.c_str(), 0))
    ImGui::ClosePopupToLevel(GImGui->OpenPopupStack.Size - 1, true);

  s_state.choice_dialog_open = false;
  s_state.choice_dialog_checkable = false;
  std::string().swap(s_state.choice_dialog_title);
  ChoiceDialogOptions().swap(s_state.choice_dialog_options);
  ChoiceDialogCallback().swap(s_state.choice_dialog_callback);
  QueueResetFocus(FocusResetType::PopupClosed);
}

void ImGuiFullscreen::DrawChoiceDialog()
{
  if (!s_state.choice_dialog_open)
    return;

  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.PrimaryTextColor);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, UIStyle.PrimaryDarkColor);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, UIStyle.PrimaryColor);

  const float width = LayoutScale(600.0f);
  const float title_height =
    UIStyle.LargeFont->FontSize + ImGui::GetStyle().FramePadding.y * 2.0f + ImGui::GetStyle().WindowPadding.y * 2.0f;
  const float item_height =
    (LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f);
  const float height =
    title_height + (item_height * static_cast<float>(std::min<size_t>(9, s_state.choice_dialog_options.size())));
  ImGui::SetNextWindowSize(ImVec2(width, height));
  ImGui::SetNextWindowPos((ImGui::GetIO().DisplaySize - LayoutScale(0.0f, LAYOUT_FOOTER_HEIGHT)) * 0.5f,
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup(s_state.choice_dialog_title.c_str());

  bool is_open = true;
  s32 choice = -1;

  if (ImGui::BeginPopupModal(s_state.choice_dialog_title.c_str(), &is_open,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
  {
    ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.BackgroundTextColor);

    ResetFocusHere();
    BeginMenuButtons();

    if (s_state.choice_dialog_checkable)
    {
      for (s32 i = 0; i < static_cast<s32>(s_state.choice_dialog_options.size()); i++)
      {
        auto& option = s_state.choice_dialog_options[i];

        const SmallString title =
          SmallString::from_format("{0} {1}", option.second ? ICON_FA_CHECK_SQUARE : ICON_FA_SQUARE, option.first);
        if (MenuButton(title.c_str(), nullptr, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
        {
          choice = i;
          option.second = !option.second;
        }
      }
    }
    else
    {
      for (s32 i = 0; i < static_cast<s32>(s_state.choice_dialog_options.size()); i++)
      {
        auto& option = s_state.choice_dialog_options[i];
        if (ActiveButtonWithRightText(option.first.c_str(), option.second ? ICON_FA_CHECK : nullptr, option.second,
                                      true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
        {
          choice = i;
          for (s32 j = 0; j < static_cast<s32>(s_state.choice_dialog_options.size()); j++)
            s_state.choice_dialog_options[j].second = (j == i);
        }
      }
    }

    EndMenuButtons();

    ImGui::PopStyleColor(1);

    ImGui::EndPopup();
  }

  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(4);
  ImGui::PopFont();

  is_open &= !WantsToCloseMenu();

  if (choice >= 0)
  {
    // immediately close dialog when selecting, save the callback doing it. have to take a copy in this instance,
    // because the callback may open another dialog, and we don't want to close that one.
    if (!s_state.choice_dialog_checkable)
    {
      auto option = std::move(s_state.choice_dialog_options[choice]);
      const ChoiceDialogCallback callback = std::move(s_state.choice_dialog_callback);
      CloseChoiceDialog();
      callback(choice, option.first, option.second);
    }
    else
    {
      const auto& option = s_state.choice_dialog_options[choice];
      s_state.choice_dialog_callback(choice, option.first, option.second);
    }
  }
  else if (!is_open)
  {
    const ChoiceDialogCallback callback = std::move(s_state.choice_dialog_callback);
    CloseChoiceDialog();
    callback(-1, std::string(), false);
  }
  else
  {
    GetChoiceDialogHelpText(s_state.fullscreen_footer_text);
  }
}

bool ImGuiFullscreen::IsInputDialogOpen()
{
  return s_state.input_dialog_open;
}

void ImGuiFullscreen::OpenInputStringDialog(std::string title, std::string message, std::string caption,
                                            std::string ok_button_text, InputStringDialogCallback callback)
{
  s_state.input_dialog_open = true;
  s_state.input_dialog_title = std::move(title);
  s_state.input_dialog_message = std::move(message);
  s_state.input_dialog_caption = std::move(caption);
  s_state.input_dialog_ok_text = std::move(ok_button_text);
  s_state.input_dialog_callback = std::move(callback);
  QueueResetFocus(FocusResetType::PopupOpened);
}

void ImGuiFullscreen::DrawInputDialog()
{
  if (!s_state.input_dialog_open)
    return;

  ImGui::SetNextWindowSize(LayoutScale(700.0f, 0.0f));
  ImGui::SetNextWindowPos((ImGui::GetIO().DisplaySize - LayoutScale(0.0f, LAYOUT_FOOTER_HEIGHT)) * 0.5f,
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup(s_state.input_dialog_title.c_str());

  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.PrimaryTextColor);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, UIStyle.PrimaryDarkColor);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, UIStyle.PrimaryColor);

  bool is_open = true;
  if (ImGui::BeginPopupModal(s_state.input_dialog_title.c_str(), &is_open,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove))
  {
    ResetFocusHere();
    ImGui::TextWrapped("%s", s_state.input_dialog_message.c_str());

    BeginMenuButtons();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));

    if (!s_state.input_dialog_caption.empty())
    {
      const float prev = ImGui::GetCursorPosX();
      ImGui::TextUnformatted(s_state.input_dialog_caption.c_str());
      ImGui::SetNextItemWidth(ImGui::GetCursorPosX() - prev);
    }
    else
    {
      ImGui::SetNextItemWidth(ImGui::GetCurrentWindow()->WorkRect.GetWidth());
    }
    ImGui::InputText("##input", &s_state.input_dialog_text);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(10.0f));

    const bool ok_enabled = !s_state.input_dialog_text.empty();

    if (ActiveButton(s_state.input_dialog_ok_text.c_str(), false, ok_enabled) && ok_enabled)
    {
      // have to move out in case they open another dialog in the callback
      InputStringDialogCallback cb(std::move(s_state.input_dialog_callback));
      std::string text(std::move(s_state.input_dialog_text));
      CloseInputDialog();
      ImGui::CloseCurrentPopup();
      cb(std::move(text));
    }

    if (ActiveButton(ICON_FA_TIMES " Cancel", false))
    {
      CloseInputDialog();

      ImGui::CloseCurrentPopup();
    }

    EndMenuButtons();

    ImGui::EndPopup();
  }
  if (!is_open)
    CloseInputDialog();
  else
    GetInputDialogHelpText(s_state.fullscreen_footer_text);

  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(4);
  ImGui::PopFont();
}

void ImGuiFullscreen::CloseInputDialog()
{
  if (!s_state.input_dialog_open)
    return;

  if (ImGui::IsPopupOpen(s_state.input_dialog_title.c_str(), 0))
    ImGui::ClosePopupToLevel(GImGui->OpenPopupStack.Size - 1, true);

  s_state.input_dialog_open = false;
  s_state.input_dialog_title = {};
  s_state.input_dialog_message = {};
  s_state.input_dialog_caption = {};
  s_state.input_dialog_ok_text = {};
  s_state.input_dialog_text = {};
  s_state.input_dialog_callback = {};
}

bool ImGuiFullscreen::IsMessageBoxDialogOpen()
{
  return s_state.message_dialog_open;
}

void ImGuiFullscreen::OpenConfirmMessageDialog(std::string title, std::string message,
                                               ConfirmMessageDialogCallback callback, std::string yes_button_text,
                                               std::string no_button_text)
{
  CloseMessageDialog();

  s_state.message_dialog_open = true;
  s_state.message_dialog_title = std::move(title);
  s_state.message_dialog_message = std::move(message);
  s_state.message_dialog_callback = std::move(callback);
  s_state.message_dialog_buttons[0] = std::move(yes_button_text);
  s_state.message_dialog_buttons[1] = std::move(no_button_text);
  QueueResetFocus(FocusResetType::PopupOpened);
}

void ImGuiFullscreen::OpenInfoMessageDialog(std::string title, std::string message, InfoMessageDialogCallback callback,
                                            std::string button_text)
{
  CloseMessageDialog();

  s_state.message_dialog_open = true;
  s_state.message_dialog_title = std::move(title);
  s_state.message_dialog_message = std::move(message);
  s_state.message_dialog_callback = std::move(callback);
  s_state.message_dialog_buttons[0] = std::move(button_text);
  QueueResetFocus(FocusResetType::PopupOpened);
}

void ImGuiFullscreen::OpenMessageDialog(std::string title, std::string message, MessageDialogCallback callback,
                                        std::string first_button_text, std::string second_button_text,
                                        std::string third_button_text)
{
  CloseMessageDialog();

  s_state.message_dialog_open = true;
  s_state.message_dialog_title = std::move(title);
  s_state.message_dialog_message = std::move(message);
  s_state.message_dialog_callback = std::move(callback);
  s_state.message_dialog_buttons[0] = std::move(first_button_text);
  s_state.message_dialog_buttons[1] = std::move(second_button_text);
  s_state.message_dialog_buttons[2] = std::move(third_button_text);
  QueueResetFocus(FocusResetType::PopupOpened);
}

void ImGuiFullscreen::CloseMessageDialog()
{
  if (!s_state.message_dialog_open)
    return;

  if (ImGui::IsPopupOpen(s_state.message_dialog_title.c_str(), 0))
    ImGui::ClosePopupToLevel(GImGui->OpenPopupStack.Size - 1, true);

  s_state.message_dialog_open = false;
  s_state.message_dialog_title = {};
  s_state.message_dialog_message = {};
  s_state.message_dialog_buttons = {};
  s_state.message_dialog_callback = {};
  QueueResetFocus(FocusResetType::PopupClosed);
}

void ImGuiFullscreen::DrawMessageDialog()
{
  if (!s_state.message_dialog_open)
    return;

  const char* win_id = s_state.message_dialog_title.empty() ? "##messagedialog" : s_state.message_dialog_title.c_str();

  ImGui::SetNextWindowSize(LayoutScale(700.0f, 0.0f));
  ImGui::SetNextWindowPos((ImGui::GetIO().DisplaySize - LayoutScale(0.0f, LAYOUT_FOOTER_HEIGHT)) * 0.5f,
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup(win_id);

  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, UIStyle.PrimaryTextColor);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, UIStyle.PrimaryDarkColor);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, UIStyle.PrimaryColor);

  bool is_open = true;
  const u32 flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    (s_state.message_dialog_title.empty() ? ImGuiWindowFlags_NoTitleBar : 0);
  std::optional<s32> result;

  if (ImGui::BeginPopupModal(win_id, &is_open, flags))
  {
    ResetFocusHere();
    BeginMenuButtons();

    ImGui::TextWrapped("%s", s_state.message_dialog_message.c_str());
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(20.0f));

    for (s32 button_index = 0; button_index < static_cast<s32>(s_state.message_dialog_buttons.size()); button_index++)
    {
      if (!s_state.message_dialog_buttons[button_index].empty() &&
          ActiveButton(s_state.message_dialog_buttons[button_index].c_str(), false))
      {
        result = button_index;
        ImGui::CloseCurrentPopup();
      }
    }

    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(4);
  ImGui::PopFont();

  if (!is_open || result.has_value())
  {
    // have to move out in case they open another dialog in the callback
    auto cb = (std::move(s_state.message_dialog_callback));
    CloseMessageDialog();

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
        func(result.value_or(1) == 0);
    }
  }
  else
  {
    GetChoiceDialogHelpText(s_state.fullscreen_footer_text);
  }
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

ImGuiID ImGuiFullscreen::GetBackgroundProgressID(const char* str_id)
{
  return ImHashStr(str_id);
}

void ImGuiFullscreen::OpenBackgroundProgressDialog(const char* str_id, std::string message, s32 min, s32 max, s32 value)
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

void ImGuiFullscreen::UpdateBackgroundProgressDialog(const char* str_id, std::string message, s32 min, s32 max,
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

void ImGuiFullscreen::CloseBackgroundProgressDialog(const char* str_id)
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

bool ImGuiFullscreen::IsBackgroundProgressDialogOpen(const char* str_id)
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
    dl->AddText(UIStyle.MediumFont, UIStyle.MediumFont->FontSize, pos, IM_COL32(255, 255, 255, 255),
                data.message.c_str(), nullptr, 0.0f);
    pos.y += UIStyle.MediumFont->FontSize + LayoutScale(10.0f);

    const ImVec2 box_end(pos.x + window_width - LayoutScale(10.0f * 2.0f), pos.y + LayoutScale(25.0f));
    dl->AddRectFilled(pos, box_end, ImGui::GetColorU32(UIStyle.PrimaryDarkColor));

    if (data.min != data.max)
    {
      const float fraction = static_cast<float>(data.value - data.min) / static_cast<float>(data.max - data.min);
      dl->AddRectFilled(pos, ImVec2(pos.x + fraction * (box_end.x - pos.x), box_end.y),
                        ImGui::GetColorU32(UIStyle.SecondaryColor));

      const auto text = TinyString::from_format("{}%", static_cast<int>(std::round(fraction * 100.0f)));
      const ImVec2 text_size(ImGui::CalcTextSize(text));
      const ImVec2 text_pos(pos.x + ((box_end.x - pos.x) / 2.0f) - (text_size.x / 2.0f),
                            pos.y + ((box_end.y - pos.y) / 2.0f) - (text_size.y / 2.0f));
      dl->AddText(UIStyle.MediumFont, UIStyle.MediumFont->FontSize, text_pos,
                  ImGui::GetColorU32(UIStyle.PrimaryTextColor), text.c_str(), text.end_ptr());
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

  ImGui::EndFrame();

  GPUSwapChain* swap_chain = g_gpu_device->GetMainSwapChain();
  if (g_gpu_device->BeginPresent(swap_chain) == GPUDevice::PresentResult::OK)
  {
    g_gpu_device->RenderImGui(swap_chain);
    g_gpu_device->EndPresent(swap_chain, false);
  }

  ImGui::NewFrame();
}

void ImGuiFullscreen::OpenOrUpdateLoadingScreen(std::string_view image, std::string_view message,
                                                s32 progress_min /*= -1*/, s32 progress_max /*= -1*/,
                                                s32 progress_value /*= -1*/)
{
  if (s_state.loading_screen_image != image)
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
  const float scale = ImGuiManager::GetGlobalScale();
  const float width = (400.0f * scale);
  const bool has_progress = (progress_min < progress_max);

  const float logo_width = 260.0f * scale;
  const float logo_height = 260.0f * scale;

  ImGui::SetNextWindowSize(ImVec2(logo_width, logo_height), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, (io.DisplaySize.y * 0.5f) - (50.0f * scale)),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::Begin("LoadingScreenLogo", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBackground))
  {
    GPUTexture* tex = GetCachedTexture(image);
    if (tex)
      ImGui::Image(tex, ImVec2(logo_width, logo_height));
  }
  ImGui::End();

  const float padding_and_rounding = 18.0f * scale;
  const float frame_rounding = 6.0f * scale;
  const float bar_height = ImCeil(ImGuiManager::GetOSDFont()->FontSize * 1.1f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, padding_and_rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding_and_rounding, padding_and_rounding));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frame_rounding);
  ImGui::PushFont(ImGuiManager::GetOSDFont());
  ImGui::SetNextWindowSize(ImVec2(width, ((has_progress || is_persistent) ? 85.0f : 55.0f) * scale), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, (io.DisplaySize.y * 0.5f) + (100.0f * scale)),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  if (ImGui::Begin("LoadingScreen", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing))
  {
    if (has_progress || is_persistent)
    {
      if (!message.empty())
        ImGui::TextUnformatted(message.data(), message.data() + message.size());

      if (has_progress)
      {
        TinyString buf;
        buf.format("{}/{}", progress_value, progress_max);

        const ImVec2 prog_size = ImGui::CalcTextSize(buf.c_str(), buf.end_ptr());
        ImGui::SameLine();
        ImGui::SetCursorPosX(width - padding_and_rounding - prog_size.x);
        ImGui::TextUnformatted(buf.c_str(), buf.end_ptr());
      }

      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f * scale);

      ImGui::ProgressBar(has_progress ?
                           (static_cast<float>(progress_value) / static_cast<float>(progress_max - progress_min)) :
                           static_cast<float>(-ImGui::GetTime()),
                         ImVec2(-1.0f, bar_height), "");
    }
    else
    {
      if (!message.empty())
      {
        const ImVec2 text_size(ImGui::CalcTextSize(message.data(), message.data() + message.size()));
        ImGui::SetCursorPosX((width - text_size.x) / 2.0f);
        ImGui::TextUnformatted(message.data(), message.data() + message.size());
      }
    }
  }
  ImGui::End();
  ImGui::PopFont();
  ImGui::PopStyleVar(3);
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
  const float rounding = ImGuiFullscreen::LayoutScale(12.0f);

  ImFont* const title_font = ImGuiFullscreen::UIStyle.LargeFont;
  ImFont* const text_font = ImGuiFullscreen::UIStyle.MediumFont;

  const u32 toast_background_color =
    s_state.light_theme ? IM_COL32(241, 241, 241, 255) : IM_COL32(0x28, 0x28, 0x28, 255);
  const u32 toast_title_color = s_state.light_theme ? IM_COL32(1, 1, 1, 255) : IM_COL32(0xff, 0xff, 0xff, 255);
  const u32 toast_text_color = s_state.light_theme ? IM_COL32(0, 0, 0, 255) : IM_COL32(0xff, 0xff, 0xff, 255);

  for (u32 index = 0; index < static_cast<u32>(s_state.notifications.size());)
  {
    Notification& notif = s_state.notifications[index];
    const float time_passed = static_cast<float>(Timer::ConvertValueToSeconds(current_time - notif.start_time));
    if (time_passed >= notif.duration)
    {
      s_state.notifications.erase(s_state.notifications.begin() + index);
      continue;
    }

    const ImVec2 title_size(title_font->CalcTextSizeA(title_font->FontSize, max_text_width, max_text_width,
                                                      notif.title.c_str(), notif.title.c_str() + notif.title.size()));

    const ImVec2 text_size(text_font->CalcTextSizeA(text_font->FontSize, max_text_width, max_text_width,
                                                    notif.text.c_str(), notif.text.c_str() + notif.text.size()));

    float box_width = std::max((horizontal_padding * 2.0f) + badge_size + horizontal_spacing +
                                 ImCeil(std::max(title_size.x, text_size.x)),
                               min_width);
    const float box_height =
      std::max((vertical_padding * 2.0f) + ImCeil(title_size.y) + vertical_spacing + ImCeil(text_size.y), min_height);

    u8 opacity = 255;
    bool clip_box = false;
    if (time_passed < NOTIFICATION_APPEAR_ANIMATION_TIME)
    {
      const float pct = time_passed / NOTIFICATION_APPEAR_ANIMATION_TIME;
      const float eased_pct = Easing::OutExpo(pct);
      box_width = box_width * eased_pct;
      opacity = static_cast<u8>(pct * 255.0f);
      clip_box = true;
    }
    else if (time_passed >= (notif.duration - NOTIFICATION_DISAPPEAR_ANIMATION_TIME))
    {
      const float pct = (notif.duration - time_passed) / NOTIFICATION_DISAPPEAR_ANIMATION_TIME;
      const float eased_pct = Easing::InExpo(pct);
      box_width = box_width * eased_pct;
      opacity = static_cast<u8>(eased_pct * 255.0f);
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
    const u32 background_color = (toast_background_color & ~IM_COL32_A_MASK) | (opacity << IM_COL32_A_SHIFT);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(box_min, box_max, background_color, rounding, ImDrawFlags_RoundCornersAll);

    if (clip_box)
      dl->PushClipRect(box_min, box_max);

    const ImVec2 badge_min(box_min.x + horizontal_padding, box_min.y + vertical_padding);
    const ImVec2 badge_max(badge_min.x + badge_size, badge_min.y + badge_size);
    if (!notif.badge_path.empty())
    {
      GPUTexture* tex =
        GetCachedTexture(notif.badge_path.c_str(), static_cast<u32>(badge_size), static_cast<u32>(badge_size));
      if (tex)
      {
        dl->AddImage(tex, badge_min, badge_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                     IM_COL32(255, 255, 255, opacity));
      }
    }

    const ImVec2 title_min(badge_max.x + horizontal_spacing, box_min.y + vertical_padding);
    const ImVec2 title_max(title_min.x + title_size.x, title_min.y + title_size.y);
    const u32 title_col = (toast_title_color & ~IM_COL32_A_MASK) | (opacity << IM_COL32_A_SHIFT);
    dl->AddText(title_font, title_font->FontSize, title_min, title_col, notif.title.c_str(),
                notif.title.c_str() + notif.title.size(), max_text_width);

    const ImVec2 text_min(badge_max.x + horizontal_spacing, title_max.y + vertical_spacing);
    const ImVec2 text_max(text_min.x + text_size.x, text_min.y + text_size.y);
    const u32 text_col = (toast_text_color & ~IM_COL32_A_MASK) | (opacity << IM_COL32_A_SHIFT);
    dl->AddText(text_font, text_font->FontSize, text_min, text_col, notif.text.c_str(),
                notif.text.c_str() + notif.text.size(), max_text_width);

    if (clip_box)
      dl->PopClipRect();

    position.y += s_notification_vertical_direction * (box_height + shadow_size + spacing);
    index++;
  }
}

void ImGuiFullscreen::ShowToast(std::string title, std::string message, float duration)
{
  s_state.toast_title = std::move(title);
  s_state.toast_message = std::move(message);
  s_state.toast_start_time = Timer::GetCurrentValue();
  s_state.toast_duration = duration;
}

void ImGuiFullscreen::ClearToast()
{
  s_state.toast_message = {};
  s_state.toast_title = {};
  s_state.toast_start_time = 0;
  s_state.toast_duration = 0.0f;
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

  ImFont* title_font = UIStyle.LargeFont;
  ImFont* message_font = UIStyle.MediumFont;
  const float padding = LayoutScale(20.0f);
  const float total_padding = padding * 2.0f;
  const float margin = LayoutScale(20.0f + (s_state.fullscreen_footer_text.empty() ? 0.0f : LAYOUT_FOOTER_HEIGHT));
  const float spacing = s_state.toast_title.empty() ? 0.0f : LayoutScale(10.0f);
  const ImVec2 display_size(ImGui::GetIO().DisplaySize);
  const ImVec2 title_size(s_state.toast_title.empty() ?
                            ImVec2(0.0f, 0.0f) :
                            title_font->CalcTextSizeA(title_font->FontSize, FLT_MAX, max_width,
                                                      s_state.toast_title.c_str(),
                                                      s_state.toast_title.c_str() + s_state.toast_title.length()));
  const ImVec2 message_size(
    s_state.toast_message.empty() ?
      ImVec2(0.0f, 0.0f) :
      message_font->CalcTextSizeA(message_font->FontSize, FLT_MAX, max_width, s_state.toast_message.c_str(),
                                  s_state.toast_message.c_str() + s_state.toast_message.length()));
  const ImVec2 comb_size(std::max(title_size.x, message_size.x), title_size.y + spacing + message_size.y);

  const ImVec2 box_size(comb_size.x + total_padding, comb_size.y + total_padding);
  const ImVec2 box_pos((display_size.x - box_size.x) * 0.5f, (display_size.y - margin - box_size.y));

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(box_pos, box_pos + box_size, ImGui::GetColorU32(ModAlpha(UIStyle.PrimaryColor, alpha)), padding);
  if (!s_state.toast_title.empty())
  {
    const float offset = (comb_size.x - title_size.x) * 0.5f;
    dl->AddText(title_font, title_font->FontSize, box_pos + ImVec2(offset + padding, padding),
                ImGui::GetColorU32(ModAlpha(UIStyle.PrimaryTextColor, alpha)), s_state.toast_title.c_str(),
                s_state.toast_title.c_str() + s_state.toast_title.length(), max_width);
  }
  if (!s_state.toast_message.empty())
  {
    const float offset = (comb_size.x - message_size.x) * 0.5f;
    dl->AddText(message_font, message_font->FontSize,
                box_pos + ImVec2(offset + padding, padding + spacing + title_size.y),
                ImGui::GetColorU32(ModAlpha(UIStyle.PrimaryTextColor, alpha)), s_state.toast_message.c_str(),
                s_state.toast_message.c_str() + s_state.toast_message.length(), max_width);
  }
}

void ImGuiFullscreen::SetTheme(bool light)
{
  s_state.light_theme = light;

  if (!light)
  {
    // dark
    UIStyle.BackgroundColor = HEX_TO_IMVEC4(0x212121, 0xff);
    UIStyle.BackgroundTextColor = HEX_TO_IMVEC4(0xffffff, 0xff);
    UIStyle.BackgroundLineColor = HEX_TO_IMVEC4(0xf0f0f0, 0xff);
    UIStyle.BackgroundHighlight = HEX_TO_IMVEC4(0x4b4b4b, 0xc0);
    UIStyle.PopupBackgroundColor = HEX_TO_IMVEC4(0x212121, 0xf2);
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
  }
  else
  {
    // light
    UIStyle.BackgroundColor = HEX_TO_IMVEC4(0xc8c8c8, 0xff);
    UIStyle.BackgroundTextColor = HEX_TO_IMVEC4(0x000000, 0xff);
    UIStyle.BackgroundLineColor = HEX_TO_IMVEC4(0xe1e2e1, 0xff);
    UIStyle.BackgroundHighlight = HEX_TO_IMVEC4(0xe1e2e1, 0xc0);
    UIStyle.PopupBackgroundColor = HEX_TO_IMVEC4(0xd8d8d8, 0xf2);
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
  }
}