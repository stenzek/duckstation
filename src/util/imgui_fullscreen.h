// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include "IconsFontAwesome5.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Image;
class GPUTexture;
class SmallStringBase;

namespace ImGuiFullscreen {
#define HEX_TO_IMVEC4(hex, alpha)                                                                                      \
  ImVec4(static_cast<float>((hex >> 16) & 0xFFu) / 255.0f, static_cast<float>((hex >> 8) & 0xFFu) / 255.0f,            \
         static_cast<float>(hex & 0xFFu) / 255.0f, static_cast<float>(alpha) / 255.0f)

static constexpr float LAYOUT_SCREEN_WIDTH = 1280.0f;
static constexpr float LAYOUT_SCREEN_HEIGHT = 720.0f;
static constexpr float LAYOUT_LARGE_FONT_SIZE = 26.0f;
static constexpr float LAYOUT_MEDIUM_FONT_SIZE = 16.0f;
static constexpr float LAYOUT_SMALL_FONT_SIZE = 10.0f;
static constexpr float LAYOUT_MENU_BUTTON_HEIGHT = 50.0f;
static constexpr float LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY = 26.0f;
static constexpr float LAYOUT_MENU_BUTTON_X_PADDING = 15.0f;
static constexpr float LAYOUT_MENU_BUTTON_Y_PADDING = 10.0f;
static constexpr float LAYOUT_MENU_WINDOW_X_PADDING = 12.0f;
static constexpr float LAYOUT_FOOTER_PADDING = 10.0f;
static constexpr float LAYOUT_FOOTER_HEIGHT = LAYOUT_MEDIUM_FONT_SIZE + LAYOUT_FOOTER_PADDING * 2.0f;
static constexpr float LAYOUT_HORIZONTAL_MENU_HEIGHT = 320.0f;
static constexpr float LAYOUT_HORIZONTAL_MENU_PADDING = 30.0f;
static constexpr float LAYOUT_HORIZONTAL_MENU_ITEM_WIDTH = 250.0f;

struct ALIGN_TO_CACHE_LINE UIStyles
{
  ImVec4 BackgroundColor;
  ImVec4 BackgroundTextColor;
  ImVec4 BackgroundLineColor;
  ImVec4 BackgroundHighlight;
  ImVec4 PopupBackgroundColor;
  ImVec4 DisabledColor;
  ImVec4 PrimaryColor;
  ImVec4 PrimaryLightColor;
  ImVec4 PrimaryDarkColor;
  ImVec4 PrimaryTextColor;
  ImVec4 TextHighlightColor;
  ImVec4 PrimaryLineColor;
  ImVec4 SecondaryColor;
  ImVec4 SecondaryWeakColor; // Not currently used.
  ImVec4 SecondaryStrongColor;
  ImVec4 SecondaryTextColor;

  ImFont* MediumFont;
  ImFont* LargeFont;

  float LayoutScale;
  float RcpLayoutScale;
  float LayoutPaddingLeft;
  float LayoutPaddingTop;
};

extern UIStyles UIStyle;

ALWAYS_INLINE static float LayoutScale(float v)
{
  return ImCeil(UIStyle.LayoutScale * v);
}

ALWAYS_INLINE static ImVec2 LayoutScale(const ImVec2& v)
{
  return ImVec2(ImCeil(v.x * UIStyle.LayoutScale), ImCeil(v.y * UIStyle.LayoutScale));
}

ALWAYS_INLINE static ImVec2 LayoutScale(float x, float y)
{
  return ImVec2(ImCeil(x * UIStyle.LayoutScale), ImCeil(y * UIStyle.LayoutScale));
}

ALWAYS_INLINE static float LayoutUnscale(float v)
{
  return ImCeil(UIStyle.RcpLayoutScale * v);
}
ALWAYS_INLINE static ImVec2 LayoutUnscale(const ImVec2& v)
{
  return ImVec2(ImCeil(v.x * UIStyle.RcpLayoutScale), ImCeil(v.y * UIStyle.RcpLayoutScale));
}
ALWAYS_INLINE static ImVec2 LayoutUnscale(float x, float y)
{
  return ImVec2(ImCeil(x * UIStyle.RcpLayoutScale), ImCeil(y * UIStyle.RcpLayoutScale));
}

ALWAYS_INLINE static ImVec4 ModAlpha(const ImVec4& v, float a)
{
  return ImVec4(v.x, v.y, v.z, a);
}

ALWAYS_INLINE static ImVec4 MulAlpha(const ImVec4& v, float a)
{
  return ImVec4(v.x, v.y, v.z, v.w * a);
}

ALWAYS_INLINE static std::string_view RemoveHash(std::string_view s)
{
  const std::string_view::size_type pos = s.find('#');
  return (pos != std::string_view::npos) ? s.substr(0, pos) : s;
}

/// Centers an image within the specified bounds, scaling up or down as needed.
ImRect CenterImage(const ImVec2& fit_size, const ImVec2& image_size);
ImRect CenterImage(const ImRect& fit_rect, const ImVec2& image_size);

/// Initializes, setting up any state.
bool Initialize(const char* placeholder_image_path);

void SetTheme(bool light);
void SetSmoothScrolling(bool enabled);
void SetFonts(ImFont* medium_font, ImFont* large_font);
bool UpdateLayoutScale();

/// Shuts down, clearing all state.
void Shutdown();

/// Texture cache.
const std::shared_ptr<GPUTexture>& GetPlaceholderTexture();
std::shared_ptr<GPUTexture> LoadTexture(std::string_view path, u32 svg_width = 0, u32 svg_height = 0);
GPUTexture* GetCachedTexture(std::string_view name);
GPUTexture* GetCachedTexture(std::string_view name, u32 svg_width, u32 svg_height);
GPUTexture* GetCachedTextureAsync(std::string_view name);
bool InvalidateCachedTexture(const std::string& path);
void UploadAsyncTextures();

void BeginLayout();
void EndLayout();

void PushResetLayout();
void PopResetLayout();

enum class FocusResetType : u8
{
  None,
  PopupOpened,
  PopupClosed,
  ViewChanged,
  Other,
};
void QueueResetFocus(FocusResetType type);
bool ResetFocusHere();
bool IsFocusResetQueued();
bool IsFocusResetFromWindowChange();
FocusResetType GetQueuedFocusResetType();
void ForceKeyNavEnabled();

bool WantsToCloseMenu();
void ResetCloseMenuIfNeeded();

void PushPrimaryColor();
void PopPrimaryColor();

void DrawWindowTitle(const char* title);

bool BeginFullscreenColumns(const char* title = nullptr, float pos_y = 0.0f, bool expand_to_screen_width = false,
                            bool footer = false);
void EndFullscreenColumns();

bool BeginFullscreenColumnWindow(float start, float end, const char* name,
                                 const ImVec4& background = UIStyle.BackgroundColor);
void EndFullscreenColumnWindow();

bool BeginFullscreenWindow(float left, float top, float width, float height, const char* name,
                           const ImVec4& background = HEX_TO_IMVEC4(0x212121, 0xFF), float rounding = 0.0f,
                           const ImVec2& padding = ImVec2(), ImGuiWindowFlags flags = 0);
bool BeginFullscreenWindow(const ImVec2& position, const ImVec2& size, const char* name,
                           const ImVec4& background = HEX_TO_IMVEC4(0x212121, 0xFF), float rounding = 0.0f,
                           const ImVec2& padding = ImVec2(), ImGuiWindowFlags flags = 0);
void EndFullscreenWindow();

bool IsGamepadInputSource();
void CreateFooterTextString(SmallStringBase& dest, std::span<const std::pair<const char*, std::string_view>> items);
void SetFullscreenFooterText(std::string_view text);
void SetFullscreenFooterText(std::span<const std::pair<const char*, std::string_view>> items);
void DrawFullscreenFooter();

void PrerenderMenuButtonBorder();
void BeginMenuButtons(u32 num_items = 0, float y_align = 0.0f, float x_padding = LAYOUT_MENU_BUTTON_X_PADDING,
                      float y_padding = LAYOUT_MENU_BUTTON_Y_PADDING, float item_height = LAYOUT_MENU_BUTTON_HEIGHT);
void EndMenuButtons();
void GetMenuButtonFrameBounds(float height, ImVec2* pos, ImVec2* size);
bool MenuButtonFrame(const char* str_id, bool enabled, float height, bool* visible, bool* hovered, ImVec2* min,
                     ImVec2* max, ImGuiButtonFlags flags = 0, float hover_alpha = 1.0f);
void DrawMenuButtonFrame(const ImVec2& p_min, const ImVec2& p_max, ImU32 fill_col, bool border = true,
                         float rounding = 0.0f);
void ResetMenuButtonFrame();
void MenuHeading(const char* title, bool draw_line = true);
bool MenuHeadingButton(const char* title, const char* value = nullptr, bool enabled = true, bool draw_line = true);
bool ActiveButton(const char* title, bool is_active, bool enabled = true,
                  float height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, ImFont* font = UIStyle.LargeFont);
bool DefaultActiveButton(const char* title, bool is_active, bool enabled = true,
                         float height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, ImFont* font = UIStyle.LargeFont);
bool ActiveButtonWithRightText(const char* title, const char* right_title, bool is_active, bool enabled = true,
                               float height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, ImFont* font = UIStyle.LargeFont);
bool MenuButton(const char* title, const char* summary, bool enabled = true, float height = LAYOUT_MENU_BUTTON_HEIGHT,
                ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont);
bool MenuButtonWithoutSummary(const char* title, bool enabled = true,
                              float height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, ImFont* font = UIStyle.LargeFont,
                              const ImVec2& text_align = ImVec2(0.0f, 0.0f));
bool MenuButtonWithValue(const char* title, const char* summary, const char* value, bool enabled = true,
                         float height = LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = UIStyle.LargeFont,
                         ImFont* summary_font = UIStyle.MediumFont);
bool MenuImageButton(const char* title, const char* summary, ImTextureID user_texture_id, const ImVec2& image_size,
                     bool enabled = true, float height = LAYOUT_MENU_BUTTON_HEIGHT,
                     const ImVec2& uv0 = ImVec2(0.0f, 0.0f), const ImVec2& uv1 = ImVec2(1.0f, 1.0f),
                     ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont);
bool FloatingButton(const char* text, float x, float y, float width = -1.0f,
                    float height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, float anchor_x = 0.0f, float anchor_y = 0.0f,
                    bool enabled = true, ImFont* font = UIStyle.LargeFont, ImVec2* out_position = nullptr,
                    bool repeat_button = false);
bool ToggleButton(const char* title, const char* summary, bool* v, bool enabled = true,
                  float height = LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = UIStyle.LargeFont,
                  ImFont* summary_font = UIStyle.MediumFont);
bool ThreeWayToggleButton(const char* title, const char* summary, std::optional<bool>* v, bool enabled = true,
                          float height = LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = UIStyle.LargeFont,
                          ImFont* summary_font = UIStyle.MediumFont);
bool RangeButton(const char* title, const char* summary, s32* value, s32 min, s32 max, s32 increment,
                 const char* format = "%d", bool enabled = true, float height = LAYOUT_MENU_BUTTON_HEIGHT,
                 ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont,
                 const char* ok_text = "OK");
bool RangeButton(const char* title, const char* summary, float* value, float min, float max, float increment,
                 const char* format = "%f", bool enabled = true, float height = LAYOUT_MENU_BUTTON_HEIGHT,
                 ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont,
                 const char* ok_text = "OK");
bool EnumChoiceButtonImpl(const char* title, const char* summary, s32* value_pointer,
                          const char* (*to_display_name_function)(s32 value, void* opaque), void* opaque, u32 count,
                          bool enabled, float height, ImFont* font, ImFont* summary_font);

template<typename DataType, typename CountType>
ALWAYS_INLINE static bool EnumChoiceButton(const char* title, const char* summary, DataType* value_pointer,
                                           const char* (*to_display_name_function)(DataType value), CountType count,
                                           bool enabled = true, float height = LAYOUT_MENU_BUTTON_HEIGHT,
                                           ImFont* font = UIStyle.LargeFont, ImFont* summary_font = UIStyle.MediumFont)
{
  s32 value = static_cast<s32>(*value_pointer);
  auto to_display_name_wrapper = [](s32 value, void* opaque) -> const char* {
    return (*static_cast<decltype(to_display_name_function)*>(opaque))(static_cast<DataType>(value));
  };

  if (EnumChoiceButtonImpl(title, summary, &value, to_display_name_wrapper, &to_display_name_function,
                           static_cast<u32>(count), enabled, height, font, summary_font))
  {
    *value_pointer = static_cast<DataType>(value);
    return true;
  }
  else
  {
    return false;
  }
}

void DrawShadowedText(ImDrawList* dl, ImFont* font, const ImVec2& pos, u32 col, const char* text,
                      const char* text_end = nullptr, float wrap_width = 0.0f);

void BeginNavBar(float x_padding = LAYOUT_MENU_BUTTON_X_PADDING, float y_padding = LAYOUT_MENU_BUTTON_Y_PADDING);
void EndNavBar();
void NavTitle(const char* title, float height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, ImFont* font = UIStyle.LargeFont);
void RightAlignNavButtons(u32 num_items = 0, float item_width = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY,
                          float item_height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
bool NavButton(const char* title, bool is_active, bool enabled = true, float width = -1.0f,
               float height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, ImFont* font = UIStyle.LargeFont);
bool NavTab(const char* title, bool is_active, bool enabled, float width, float height, const ImVec4& background,
            ImFont* font = UIStyle.LargeFont);

bool BeginHorizontalMenu(const char* name, const ImVec2& position, const ImVec2& size, u32 num_items);
void EndHorizontalMenu();
bool HorizontalMenuItem(GPUTexture* icon, const char* title, const char* description);

using FileSelectorCallback = std::function<void(const std::string& path)>;
using FileSelectorFilters = std::vector<std::string>;
bool IsFileSelectorOpen();
void OpenFileSelector(std::string_view title, bool select_directory, FileSelectorCallback callback,
                      FileSelectorFilters filters = FileSelectorFilters(),
                      std::string initial_directory = std::string());
void CloseFileSelector();

using ChoiceDialogCallback = std::function<void(s32 index, const std::string& title, bool checked)>;
using ChoiceDialogOptions = std::vector<std::pair<std::string, bool>>;
bool IsChoiceDialogOpen();
void OpenChoiceDialog(std::string_view title, bool checkable, ChoiceDialogOptions options,
                      ChoiceDialogCallback callback);
void CloseChoiceDialog();

using InputStringDialogCallback = std::function<void(std::string text)>;
bool IsInputDialogOpen();
void OpenInputStringDialog(std::string title, std::string message, std::string caption, std::string ok_button_text,
                           InputStringDialogCallback callback);
void CloseInputDialog();

using ConfirmMessageDialogCallback = std::function<void(bool)>;
using InfoMessageDialogCallback = std::function<void()>;
using MessageDialogCallback = std::function<void(s32)>;
bool IsMessageBoxDialogOpen();
void OpenConfirmMessageDialog(std::string title, std::string message, ConfirmMessageDialogCallback callback,
                              std::string yes_button_text = ICON_FA_CHECK " Yes",
                              std::string no_button_text = ICON_FA_TIMES " No");
void OpenInfoMessageDialog(std::string title, std::string message, InfoMessageDialogCallback callback = {},
                           std::string button_text = ICON_FA_WINDOW_CLOSE " Close");
void OpenMessageDialog(std::string title, std::string message, MessageDialogCallback callback,
                       std::string first_button_text, std::string second_button_text, std::string third_button_text);
void CloseMessageDialog();

float GetNotificationVerticalPosition();
float GetNotificationVerticalDirection();
void SetNotificationVerticalPosition(float position, float direction);

void OpenBackgroundProgressDialog(const char* str_id, std::string message, s32 min, s32 max, s32 value);
void UpdateBackgroundProgressDialog(const char* str_id, std::string message, s32 min, s32 max, s32 value);
void CloseBackgroundProgressDialog(const char* str_id);
bool IsBackgroundProgressDialogOpen(const char* str_id);

/// Displays a loading screen with the logo, rendered with ImGui. Use when executing possibly-time-consuming tasks
/// such as compiling shaders when starting up.
void RenderLoadingScreen(std::string_view image, std::string_view message, s32 progress_min = -1, s32 progress_max = -1,
                         s32 progress_value = -1);
void OpenOrUpdateLoadingScreen(std::string_view image, std::string_view message, s32 progress_min = -1,
                               s32 progress_max = -1, s32 progress_value = -1);
bool IsLoadingScreenOpen();
void CloseLoadingScreen();

/// Renders a previously-configured loading screen.
void RenderLoadingScreen();

void AddNotification(std::string key, float duration, std::string title, std::string text, std::string image_path);
void ClearNotifications();

void ShowToast(std::string title, std::string message, float duration = 10.0f);
void ClearToast();

// Message callbacks.
void GetChoiceDialogHelpText(SmallStringBase& dest);
void GetFileSelectorHelpText(SmallStringBase& dest);
void GetInputDialogHelpText(SmallStringBase& dest);
} // namespace ImGuiFullscreen

// Host UI triggers from Big Picture mode.
namespace Host {
/// Returns true if native file dialogs should be preferred over Big Picture.
bool ShouldPreferHostFileSelector();

/// Opens a file selector dialog.
using FileSelectorCallback = std::function<void(const std::string& path)>;
using FileSelectorFilters = std::vector<std::string>;
void OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
                               FileSelectorFilters filters = FileSelectorFilters(),
                               std::string_view initial_directory = std::string_view());
} // namespace Host
