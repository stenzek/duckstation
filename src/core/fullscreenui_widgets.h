// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include "util/translation.h"

#include "common/small_string.h"
#include "common/types.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome.h"
#include "imgui.h"
#include "imgui_internal.h"

#include "fmt/format.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Error;
class Image;
class GPUTexture;
class GPUSwapChain;
class ProgressCallbackWithPrompt;

enum class OSDMessageType : u8;

namespace FullscreenUI {

#define HEX_TO_IMVEC4(hex, alpha)                                                                                      \
  ImVec4(static_cast<float>((hex >> 16) & 0xFFu) / 255.0f, static_cast<float>((hex >> 8) & 0xFFu) / 255.0f,            \
         static_cast<float>(hex & 0xFFu) / 255.0f, static_cast<float>(alpha) / 255.0f)

// end_ptr() for string_view
#define IMSTR_START_END(sv) (sv).data(), (sv).data() + (sv).length()

inline constexpr float LAYOUT_SCREEN_WIDTH = 1280.0f;
inline constexpr float LAYOUT_SCREEN_HEIGHT = 720.0f;
inline constexpr float LAYOUT_LARGE_FONT_SIZE = 26.0f;
inline constexpr float LAYOUT_MEDIUM_FONT_SIZE = 16.0f;
inline constexpr float LAYOUT_MEDIUM_LARGE_FONT_SIZE = 21.0f;
inline constexpr float LAYOUT_MEDIUM_SMALL_FONT_SIZE = 14.0f;
inline constexpr float LAYOUT_SMALL_FONT_SIZE = 14.0f;
inline constexpr float LAYOUT_MENU_BUTTON_X_PADDING = 15.0f;
inline constexpr float LAYOUT_MENU_BUTTON_Y_PADDING = 10.0f;
inline constexpr float LAYOUT_MENU_BUTTON_SPACING = 6.0f;
inline constexpr float LAYOUT_MENU_WINDOW_X_PADDING = 12.0f;
inline constexpr float LAYOUT_MENU_WINDOW_Y_PADDING = 12.0f;
inline constexpr float LAYOUT_MENU_ITEM_TITLE_SUMMARY_SPACING = 6.0f;
inline constexpr float LAYOUT_MENU_ITEM_EXTRA_HEIGHT = 2.0f;
inline constexpr float LAYOUT_MENU_ITEM_BORDER_ROUNDING = 10.0f;
inline constexpr float LAYOUT_ITEM_X_SPACING = 8.0f;
inline constexpr float LAYOUT_ITEM_Y_SPACING = 4.0f;
inline constexpr float LAYOUT_FOOTER_X_PADDING = 15.0f;
inline constexpr float LAYOUT_FOOTER_Y_PADDING = 10.0f;
inline constexpr float LAYOUT_FOOTER_HEIGHT = LAYOUT_MEDIUM_FONT_SIZE + LAYOUT_FOOTER_Y_PADDING * 2.0f;
inline constexpr float LAYOUT_HORIZONTAL_MENU_HEIGHT = 320.0f;
inline constexpr float LAYOUT_HORIZONTAL_MENU_PADDING = 30.0f;
inline constexpr float LAYOUT_HORIZONTAL_MENU_ITEM_WIDTH = 250.0f;
inline constexpr float LAYOUT_HORIZONTAL_MENU_ITEM_IMAGE_SIZE = 150.0f;
inline constexpr float LAYOUT_SHADOW_OFFSET = 1.0f;
inline constexpr float LAYOUT_SMALL_POPUP_PADDING = 20.0f;
inline constexpr float LAYOUT_LARGE_POPUP_PADDING = 30.0f;
inline constexpr float LAYOUT_LARGE_POPUP_ROUNDING = 40.0f;
inline constexpr float LAYOUT_WIDGET_FRAME_ROUNDING = 20.0f;
inline constexpr ImVec2 LAYOUT_CENTER_ALIGN_TEXT = ImVec2(0.5f, 0.0f);

struct ALIGN_TO_CACHE_LINE UIStyles
{
  ImVec4 BackgroundColor;
  ImVec4 BackgroundTextColor;
  ImVec4 BackgroundLineColor;
  ImVec4 BackgroundHighlight;
  ImVec4 PopupBackgroundColor;
  ImVec4 PopupFrameBackgroundColor;
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
  ImVec4 ToastBackgroundColor;
  ImVec4 ToastTextColor;

  ImFont* Font;

  u32 ShadowColor;

  float LayoutScale;
  float RcpLayoutScale;
  float LayoutPaddingLeft;
  float LayoutPaddingTop;
  float LargeFontSize;
  float MediumFontSize;
  float MediumLargeFontSize;
  float MediumSmallFontSize;

  static constexpr float NormalFontWeight = 0.0f;
  static constexpr float BoldFontWeight = 500.0f;

  bool Animations;
  bool SmoothScrolling;
  bool MenuBorders;
  bool IsDarkTheme;
};

extern UIStyles UIStyle;

ALWAYS_INLINE float LayoutScale(float v)
{
  return ImCeil(UIStyle.LayoutScale * v);
}

ALWAYS_INLINE ImVec2 LayoutScale(const ImVec2& v)
{
  return ImVec2(ImCeil(v.x * UIStyle.LayoutScale), ImCeil(v.y * UIStyle.LayoutScale));
}

ALWAYS_INLINE ImVec2 LayoutScale(float x, float y)
{
  return ImVec2(ImCeil(x * UIStyle.LayoutScale), ImCeil(y * UIStyle.LayoutScale));
}

ALWAYS_INLINE float LayoutUnscale(float v)
{
  return ImCeil(UIStyle.RcpLayoutScale * v);
}
ALWAYS_INLINE ImVec2 LayoutUnscale(const ImVec2& v)
{
  return ImVec2(ImCeil(v.x * UIStyle.RcpLayoutScale), ImCeil(v.y * UIStyle.RcpLayoutScale));
}
ALWAYS_INLINE ImVec2 LayoutUnscale(float x, float y)
{
  return ImVec2(ImCeil(x * UIStyle.RcpLayoutScale), ImCeil(y * UIStyle.RcpLayoutScale));
}

ALWAYS_INLINE ImVec4 ModAlpha(const ImVec4& v, float a)
{
  return ImVec4(v.x, v.y, v.z, a);
}
ALWAYS_INLINE u32 ModAlpha(u32 col32, float a)
{
  return (col32 & ~IM_COL32_A_MASK) | (static_cast<u32>(a * 255.0f) << IM_COL32_A_SHIFT);
}

// lighter in light themes
ALWAYS_INLINE ImVec4 DarkerColor(const ImVec4& v, float f = 0.8f)
{
  f = UIStyle.IsDarkTheme ? f : (1.0f / f);
  return ImVec4(std::max(v.x, 1.0f / 255.0f) * f, std::max(v.y, 1.0f / 255.0f) * f, std::max(v.z, 1.0f / 255.0f) * f,
                v.w);
}

ALWAYS_INLINE ImVec4 MulAlpha(const ImVec4& v, float a)
{
  return ImVec4(v.x, v.y, v.z, v.w * a);
}

ALWAYS_INLINE u32 MulAlpha(u32 col32, float a)
{
  return (col32 & ~IM_COL32_A_MASK) |
         (static_cast<u32>(static_cast<float>((col32 /*& IM_COL32_A_MASK*/) >> IM_COL32_A_SHIFT) * a)
          << IM_COL32_A_SHIFT);
}

ALWAYS_INLINE u32 MulAlpha(u32 col32, u32 a)
{
  return (col32 & ~IM_COL32_A_MASK) |
         (((((col32 /*& IM_COL32_A_MASK*/) >> IM_COL32_A_SHIFT) * a) / 255u) << IM_COL32_A_SHIFT);
}

ALWAYS_INLINE std::string_view RemoveHash(std::string_view s)
{
  const std::string_view::size_type pos = s.find("##");
  return (pos != std::string_view::npos) ? s.substr(0, pos) : s;
}

/// Localization support.
#define FSUI_TR_CONTEXT std::string_view("FullscreenUI")

class IconStackString : public SmallStackString<128>
{
public:
  IconStackString(std::string_view icon, std::string_view str);
  IconStackString(std::string_view icon, std::string_view str, std::string_view suffix);
};
void FormatIconString(SmallStringBase& str, std::string_view icon, std::string_view label);

#define FSUI_ICONSTR(icon, str) fmt::format("{} {}", icon, Host::TranslateToStringView(FSUI_TR_CONTEXT, str))
#define FSUI_ICONVSTR(icon, str) ::FullscreenUI::IconStackString(icon, str).view()
#define FSUI_ICONCSTR(icon, str) ::FullscreenUI::IconStackString(icon, str).c_str()
#define FSUI_STR(str) Host::TranslateToString(FSUI_TR_CONTEXT, std::string_view(str))
#define FSUI_CSTR(str) Host::TranslateToCString(FSUI_TR_CONTEXT, std::string_view(str))
#define FSUI_VSTR(str) Host::TranslateToStringView(FSUI_TR_CONTEXT, std::string_view(str))
#define FSUI_FSTR(str) fmt::runtime(Host::TranslateToStringView(FSUI_TR_CONTEXT, std::string_view(str)))
#define FSUI_NSTR(str) str

/// Centers an image within the specified bounds, scaling up or down as needed.
ImRect CenterImage(const ImVec2& fit_size, const ImVec2& image_size);
ImRect CenterImage(const ImVec2& fit_rect, const GPUTexture* texture);
ImRect CenterImage(const ImRect& fit_rect, const ImVec2& image_size);
ImRect CenterImage(const ImRect& fit_rect, const GPUTexture* texture);

/// Fits an image to the specified bounds, cropping if needed. Returns UV coordinates.
ImRect FitImage(const ImVec2& fit_size, const ImVec2& image_size);

/// Initializes, setting up any state.
bool InitializeWidgets(Error* error);

/// Shuts down, clearing all state.
void ShutdownWidgets();

/// Loads settings from the settings interface.
void UpdateWidgetsSettings();

bool CreateWidgetsGPUResources(Error* error);
void DestroyWidgetsGPUResources();

std::span<const char* const> GetThemeNames();
std::span<const char* const> GetThemeDisplayNames();
std::vector<std::string_view> GetLocalizedThemeDisplayNames();
void UpdateTheme();

void SetFont(ImFont* ui_font);
bool UpdateLayoutScale();

/// Texture cache.
const std::shared_ptr<GPUTexture>& GetPlaceholderTexture();
std::shared_ptr<GPUTexture> LoadTexture(std::string_view path, u32 svg_width = 0, u32 svg_height = 0);
GPUTexture* FindCachedTexture(std::string_view name);
GPUTexture* FindCachedTexture(std::string_view name, u32 svg_width, u32 svg_height);
GPUTexture* GetCachedTexture(std::string_view name);
GPUTexture* GetCachedTexture(std::string_view name, u32 svg_width, u32 svg_height);
GPUTexture* GetCachedTextureAsync(std::string_view name);
GPUTexture* GetCachedTextureAsync(std::string_view name, u32 svg_width, u32 svg_height);
bool InvalidateCachedTexture(std::string_view path);
bool TextureNeedsSVGDimensions(std::string_view path);
void UploadAsyncTextures();

/// Screen transitions.
inline constexpr float SHORT_TRANSITION_TIME = 0.08f;
inline constexpr float DEFAULT_TRANSITION_TIME = 0.15f;
inline constexpr float LONG_TRANSITION_TIME = 0.3f;

enum class TransitionState : u8
{
  Inactive,
  Starting,
  Active,
};

using TransitionStartCallback = std::function<void()>;
void BeginTransition(TransitionStartCallback func, float time = DEFAULT_TRANSITION_TIME);
void BeginTransition(float time, TransitionStartCallback func);
void CancelTransition();
bool IsTransitionActive();
TransitionState GetTransitionState();
GPUTexture* GetTransitionRenderTexture(GPUSwapChain* swap_chain);
void RenderTransitionBlend(GPUSwapChain* swap_chain);
void UpdateTransitionState();

/// Layout helpers.
void BeginLayout();
void EndLayout();

/// Enqueues a sound effect, and prevents any other sound effects from being started this frame.
void EnqueueSoundEffect(std::string_view sound_effect);

float GetScreenBottomMargin();

bool IsAnyFixedPopupDialogOpen();
bool IsFixedPopupDialogOpen(std::string_view name);
void OpenFixedPopupDialog(std::string_view name);
void CloseFixedPopupDialog();
void CloseFixedPopupDialogImmediately();
bool BeginFixedPopupDialog(float scaled_window_padding = LayoutScale(20.0f),
                           float scaled_window_rounding = LayoutScale(20.0f),
                           const ImVec2& scaled_window_size = ImVec2(0.0f, 0.0f));
void EndFixedPopupDialog();

void RenderOverlays();

void PushResetLayout();
void PopResetLayout();

enum class FocusResetType : u8
{
  None,
  PopupOpened,
  PopupClosed,
  ViewChanged,
  SplitWindowChanged,
  SplitContentChanged,
  Other,
};
void QueueResetFocus(FocusResetType type);
void CancelResetFocus();
bool ResetFocusHere();
bool IsFocusResetQueued();
bool IsFocusResetFromWindowChange();
FocusResetType GetQueuedFocusResetType();
void ForceKeyNavEnabled();

bool WantsToCloseMenu();
void ResetCloseMenuIfNeeded();
void CancelPendingMenuClose();

void PushPrimaryColor();
void PopPrimaryColor();

void DrawRoundedGradientRect(ImDrawList* const dl, const ImVec2& pos_min, const ImVec2& pos_max, ImU32 col_left,
                             ImU32 col_right, float rounding);
void DrawSpinner(ImDrawList* const dl, const ImVec2& pos, ImU32 color, float size, float thickness);

void DrawWindowTitle(std::string_view title);

bool BeginFullscreenColumns(const char* title = nullptr, float pos_y = 0.0f, bool expand_to_screen_width = false,
                            bool footer = false);
void EndFullscreenColumns();

bool BeginFullscreenColumnWindow(float start, float end, const char* name,
                                 const ImVec4& background = UIStyle.BackgroundColor, const ImVec2& padding = ImVec2());
void EndFullscreenColumnWindow();

bool BeginFullscreenWindow(float left, float top, float width, float height, const char* name,
                           const ImVec4& background = HEX_TO_IMVEC4(0x212121, 0xFF), float rounding = 0.0f,
                           const ImVec2& padding = ImVec2(), ImGuiWindowFlags flags = 0);
bool BeginFullscreenWindow(const ImVec2& position, const ImVec2& size, const char* name,
                           const ImVec4& background = HEX_TO_IMVEC4(0x212121, 0xFF), float rounding = 0.0f,
                           const ImVec2& padding = ImVec2(), ImGuiWindowFlags flags = 0);
void EndFullscreenWindow(bool allow_wrap_x = false, bool allow_wrap_y = true);
void SetWindowNavWrapping(bool allow_wrap_x = false, bool allow_wrap_y = true);

bool IsGamepadInputSource();
std::string_view GetControllerIconMapping(std::string_view icon);
void SetFullscreenFooterText(std::string_view text);
void SetFullscreenFooterText(std::span<const std::pair<const char*, std::string_view>> items);
void SetStandardSelectionFooterText(bool back_instead_of_cancel);
void SetFullscreenStatusText(std::string_view text);
void SetFullscreenStatusText(std::span<const std::pair<const char*, std::string_view>> items);
void DrawFullscreenFooter();

void BeginMenuButtons(u32 num_items = 0, float y_align = 0.0f, float x_padding = LAYOUT_MENU_BUTTON_X_PADDING,
                      float y_padding = LAYOUT_MENU_BUTTON_Y_PADDING, float x_spacing = 0.0f,
                      float y_spacing = LAYOUT_MENU_BUTTON_SPACING);
void EndMenuButtons();
float GetMenuButtonAvailableWidth();
bool MenuButtonFrame(std::string_view str_id, float height, bool enabled, ImRect* item_bb, bool* visible, bool* hovered,
                     ImGuiButtonFlags flags = 0);
bool MenuButtonFrame(std::string_view str_id, bool enabled, const ImRect& bb, bool* visible, bool* hovered,
                     ImGuiButtonFlags flags = 0);
void DrawMenuButtonFrame(const ImVec2& p_min, const ImVec2& p_max, bool held);
void ResetMenuButtonFrame();
void RenderShadowedTextClipped(ImFont* font, float font_size, float font_weight, const ImVec2& pos_min,
                               const ImVec2& pos_max, u32 color, std::string_view text,
                               const ImVec2* text_size_if_known = nullptr, const ImVec2& align = ImVec2(0, 0),
                               float wrap_width = 0.0f, const ImRect* clip_rect = nullptr);
void RenderShadowedTextClipped(ImDrawList* draw_list, ImFont* font, float font_size, float font_weight,
                               const ImVec2& pos_min, const ImVec2& pos_max, u32 color, std::string_view text,
                               const ImVec2* text_size_if_known = nullptr, const ImVec2& align = ImVec2(0, 0),
                               float wrap_width = 0.0f, const ImRect* clip_rect = nullptr);
void RenderShadowedTextClipped(ImDrawList* draw_list, ImFont* font, float font_size, float font_weight,
                               const ImVec2& pos_min, const ImVec2& pos_max, u32 color, std::string_view text,
                               const ImVec2* text_size_if_known, const ImVec2& align, float wrap_width,
                               const ImRect* clip_rect, float shadow_offset);
void RenderMultiLineShadowedTextClipped(ImDrawList* draw_list, ImFont* font, float font_size, float font_weight,
                                        const ImVec2& pos_min, const ImVec2& pos_max, u32 color, std::string_view text,
                                        const ImVec2& align, float wrap_width, const ImRect* clip_rect = nullptr,
                                        float shadow_offset = LayoutScale(LAYOUT_SHADOW_OFFSET));
void RenderAutoLabelText(ImDrawList* draw_list, ImFont* font, float font_size, float font_weight, float label_weight,
                         const ImVec2& pos_min, const ImVec2& pos_max, u32 color, std::string_view text,
                         char separator = ':', float shadow_offset = LayoutScale(LAYOUT_SHADOW_OFFSET));
void TextAlignedMultiLine(float align_x, const char* text, const char* text_end = nullptr, float wrap_width = -1.0f);
void TextUnformatted(std::string_view text);
void MenuHeading(std::string_view title, bool draw_line = true);
bool MenuHeadingButton(std::string_view title, std::string_view value = {}, float font_size = UIStyle.LargeFontSize,
                       bool enabled = true, bool draw_line = true);
bool MenuButton(std::string_view title, std::string_view summary, bool enabled = true,
                const ImVec2& text_align = ImVec2(0.0f, 0.0f));
bool MenuButtonWithoutSummary(std::string_view title, bool enabled = true,
                              const ImVec2& text_align = ImVec2(0.0f, 0.0f));
bool MenuButtonWithValue(std::string_view title, std::string_view summary, std::string_view value, bool enabled = true,
                         const ImVec2& text_align = ImVec2(0.0f, 0.0f));
bool MenuButtonWithVisibilityQuery(std::string_view str_id, std::string_view title, std::string_view summary,
                                   std::string_view value, bool* visible, bool enabled = true,
                                   const ImVec2& text_align = ImVec2(0.0f, 0.0f));
bool MenuImageButton(std::string_view title, std::string_view summary, std::string_view value, ImTextureID image,
                     const ImVec2& image_size = ImVec2(0.0f, 0.0f), bool enabled = true,
                     const ImVec2& uv0 = ImVec2(0.0f, 0.0f), const ImVec2& uv1 = ImVec2(1.0f, 1.0f));
bool FloatingButton(std::string_view text, float x, float y, float anchor_x = 0.0f, float anchor_y = 0.0f,
                    bool enabled = true, ImVec2* out_position = nullptr, bool repeat_button = false);
bool ToggleButton(std::string_view title, std::string_view summary, bool* v, bool enabled = true);
bool ThreeWayToggleButton(std::string_view title, std::string_view summary, std::optional<bool>* v,
                          bool enabled = true);
bool RangeButton(std::string_view title, std::string_view summary, s32* value, s32 min, s32 max, s32 increment,
                 const char* format = "%d", bool enabled = true, std::string_view ok_text = FSUI_VSTR("OK"));
bool RangeButton(std::string_view title, std::string_view summary, float* value, float min, float max, float increment,
                 const char* format = "%f", bool enabled = true, std::string_view ok_text = FSUI_VSTR("OK"));
bool EnumChoiceButtonImpl(std::string_view title, std::string_view summary, s32* value_pointer,
                          const char* (*to_display_name_function)(s32 value, void* opaque), void* opaque, u32 count,
                          bool enabled);

template<typename DataType, typename CountType>
ALWAYS_INLINE static bool EnumChoiceButton(std::string_view title, std::string_view summary, DataType* value_pointer,
                                           const char* (*to_display_name_function)(DataType value), CountType count,
                                           bool enabled = true)
{
  s32 value = static_cast<s32>(*value_pointer);
  auto to_display_name_wrapper = [](s32 value, void* opaque) -> const char* {
    return (*static_cast<decltype(to_display_name_function)*>(opaque))(static_cast<DataType>(value));
  };

  if (EnumChoiceButtonImpl(title, summary, &value, to_display_name_wrapper, &to_display_name_function,
                           static_cast<u32>(count), enabled))
  {
    *value_pointer = static_cast<DataType>(value);
    return true;
  }
  else
  {
    return false;
  }
}

void BeginHorizontalMenuButtons(u32 num_items, float max_item_width = 0.0f,
                                float x_padding = LAYOUT_MENU_BUTTON_Y_PADDING,
                                float y_padding = LAYOUT_MENU_BUTTON_Y_PADDING,
                                float x_spacing = LAYOUT_MENU_BUTTON_X_PADDING,
                                float x_margin = LAYOUT_MENU_WINDOW_X_PADDING);
void EndHorizontalMenuButtons(float add_vertical_spacing = -1.0f);
bool HorizontalMenuButton(std::string_view title, bool enabled = true,
                          const ImVec2& text_align = LAYOUT_CENTER_ALIGN_TEXT, ImGuiButtonFlags flags = 0);

void BeginNavBar(float x_padding = LAYOUT_MENU_BUTTON_X_PADDING, float y_padding = LAYOUT_MENU_BUTTON_Y_PADDING);
void EndNavBar();
void NavTitle(std::string_view title);
void RightAlignNavButtons(u32 num_items);
void RightAlignNavButtons(float total_width);
bool NavButton(std::string_view title, bool is_active, bool enabled = true);
bool NavTab(std::string_view title, bool is_active, bool enabled, float width);

bool BeginFloatingNavBar(float x, float y, float items_width, float font_size = UIStyle.LargeFontSize,
                         float anchor_x = 0.0f, float anchor_y = 0.0f, float x_padding = LAYOUT_MENU_BUTTON_X_PADDING,
                         float y_padding = LAYOUT_MENU_BUTTON_Y_PADDING);
void EndFloatingNavBar();
float CalcFloatingNavBarButtonWidth(std::string_view title, float icon_size = UIStyle.LargeFontSize,
                                    float font_size = UIStyle.LargeFontSize, float font_weight = UIStyle.BoldFontWeight,
                                    float x_padding = LAYOUT_MENU_BUTTON_X_PADDING);
bool FloatingNavBarIcon(std::string_view title, ImTextureID image, bool is_active,
                        float image_size = UIStyle.LargeFontSize, float font_size = UIStyle.LargeFontSize,
                        float font_weight = UIStyle.BoldFontWeight, bool enabled = true,
                        const ImVec2& uv0 = ImVec2(0.0f, 0.0f), const ImVec2& uv1 = ImVec2(1.0f, 1.0f));

bool BeginHorizontalMenu(const char* name, const ImVec2& position, const ImVec2& size, const ImVec4& bg_color,
                         u32 num_items);
void EndHorizontalMenu();
bool HorizontalMenuItem(GPUTexture* icon, std::string_view title, std::string_view description,
                        u32 color = IM_COL32(255, 255, 255, 255));

// Sidebars
bool BeginSplitWindow(const ImVec2& position, const ImVec2& size, const char* name,
                      const ImVec4& background = UIStyle.BackgroundColor, float rounding = 0.0f,
                      const ImVec2& padding = ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING),
                      ImGuiWindowFlags flags = 0);
void EndSplitWindow();
void BeginInnerSplitWindow();
void EndInnerSplitWindow();
bool BeginSplitWindowSidebar(float sidebar_width = 0.2f);
void EndSplitWindowSidebar();
bool SplitWindowSidebarItem(std::string_view title, bool active = false, bool enabled = true);
bool SplitWindowSidebarItem(std::string_view title, std::string_view summary, bool active = false, bool enabled = true);
void SplitWindowSidebarSeparator();
bool BeginSplitWindowContent(bool background);
void ResetSplitWindowContentFocusHere();
void EndSplitWindowContent();
bool WasSplitWindowChanged();
void FocusSplitWindowContent();
bool SplitWindowIsNavWindow();

using FileSelectorCallback = std::function<void(std::string path)>;
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
void OpenInputStringDialog(std::string_view title, std::string message, std::string caption, std::string ok_button_text,
                           InputStringDialogCallback callback);
void CloseInputDialog();

using ConfirmMessageDialogCallback = std::function<void(bool)>;
using InfoMessageDialogCallback = std::function<void()>;
using MessageDialogCallback = std::function<void(s32)>;
bool IsMessageBoxDialogOpen();
void OpenConfirmMessageDialog(std::string_view icon, std::string_view title, std::string message,
                              ConfirmMessageDialogCallback callback,
                              std::string yes_button_text = FSUI_ICONSTR(ICON_FA_CHECK, "Yes"),
                              std::string no_button_text = FSUI_ICONSTR(ICON_FA_XMARK, "No"));
void OpenInfoMessageDialog(std::string_view icon, std::string_view title, std::string message,
                           InfoMessageDialogCallback callback = {},
                           std::string button_text = FSUI_ICONSTR(ICON_FA_SQUARE_XMARK, "Close"));
void OpenMessageDialog(std::string_view icon, std::string_view title, std::string message,
                       MessageDialogCallback callback, std::string first_button_text, std::string second_button_text,
                       std::string third_button_text);
void CloseMessageDialog();

std::unique_ptr<ProgressCallbackWithPrompt> OpenModalProgressDialog(std::string title,
                                                                    float window_unscaled_width = 500.0f);

void OpenBackgroundProgressDialog(std::string_view str_id, std::string message, s32 min, s32 max, s32 value);
void UpdateBackgroundProgressDialog(std::string_view str_id, std::string message, s32 min, s32 max, s32 value);
void CloseBackgroundProgressDialog(std::string_view str_id);
bool IsBackgroundProgressDialogOpen(std::string_view str_id);

/// Displays a loading screen with the logo, rendered with ImGui. Use when executing possibly-time-consuming tasks
/// such as compiling shaders when starting up.
void RenderLoadingScreen(std::string_view image, std::string_view title, std::string_view caption,
                         s32 progress_min = -1, s32 progress_max = -1, s32 progress_value = -1);

/// Opens or updates a loading screen, rendering is managed by FullscreenUI. Safe to call from CPU thread.
void OpenOrUpdateLoadingScreen(std::string_view image, std::string_view title, std::string_view caption = {},
                               s32 progress_min = -1, s32 progress_max = -1, s32 progress_value = -1);
bool IsLoadingScreenOpen();
void CloseLoadingScreen();

/// Notification and toast support.
class NotificationLayout
{
public:
  NotificationLayout(NotificationLocation location);
  NotificationLayout(NotificationLocation location, float spacing, float screen_margin);

  ALWAYS_INLINE NotificationLocation GetLocation() const { return m_location; }
  ALWAYS_INLINE const ImVec2& GetCurrentPosition() const { return m_current_position; }
  bool IsVerticalAnimation() const;

  ImVec2 GetFixedPosition(float width, float height);

  // [position, opacity]
  std::pair<ImVec2, float> GetNextPosition(float width, float height, bool active, float anim_coeff, float width_coeff);
  std::pair<ImVec2, float> GetNextPosition(float width, float height, bool active, float time, float in_duration,
                                           float out_duration, float width_coeff);
  std::pair<ImVec2, float> GetNextPosition(float width, float height, float time_passed, float total_duration,
                                           float in_duration, float out_duration, float width_coeff);

private:
  void CalcStartPosition(float screen_margin, float top_margin);

  ImVec2 m_current_position;
  float m_spacing;
  NotificationLocation m_location;
};

void ShowToast(OSDMessageType type, std::string title, std::string message);

// Wrapper for an animated popup dialog.
class PopupDialog
{
public:
  PopupDialog();
  ~PopupDialog();

  ALWAYS_INLINE const std::string& GetTitle() const { return m_title; }
  ALWAYS_INLINE bool IsOpen() const { return (m_state != State::Inactive); }

  void StartClose();
  void CloseImmediately();
  void ClearState();

protected:
  enum class State : u8
  {
    Inactive,
    ClosingTrigger,
    Open,
    OpeningTrigger,
    Opening,
    Closing,
    Reopening,
  };

  static constexpr float OPEN_TIME = 0.2f;
  static constexpr float CLOSE_TIME = 0.1f;

  void SetTitleAndOpen(std::string title);

  bool BeginRender(float scaled_window_padding = LayoutScale(20.0f), float scaled_window_rounding = LayoutScale(20.0f),
                   const ImVec2& scaled_window_size = ImVec2(0.0f, 0.0f));
  void EndRender();

  std::string m_title;
  float m_animation_time_remaining = 0.0f;
  State m_state = State::Inactive;
  bool m_user_closeable = true;
};

// Wrapper for computing menu button bounds.
struct MenuButtonBounds
{
  ImVec2 title_size;
  ImVec2 value_size;
  ImVec2 summary_size;

  ImRect frame_bb;
  ImRect title_bb;
  ImRect value_bb;
  ImRect summary_bb;

  float available_width = CalcAvailWidth();
  float available_non_value_width;

  MenuButtonBounds(const std::string_view& title, const std::string_view& value, const std::string_view& summary);
  MenuButtonBounds(const std::string_view& title, const std::string_view& value, const std::string_view& summary,
                   float left_margin, float title_value_size = UIStyle.LargeFontSize,
                   float summary_size = UIStyle.MediumFontSize);
  MenuButtonBounds(const std::string_view& title, const ImVec2& value_size, const std::string_view& summary);
  MenuButtonBounds(const ImVec2& title_size, const ImVec2& value_size, const ImVec2& summary_size);

  static float CalcAvailWidth();

  static float GetSingleLineHeight(float y_padding = LAYOUT_MENU_BUTTON_Y_PADDING);
  static float GetSummaryLineHeight(float y_padding = LAYOUT_MENU_BUTTON_Y_PADDING);

  void CalcBB();
  void CalcTitleSize(const std::string_view& title, float font_size);
  void SetValueSize(const ImVec2& value_size);
  void CalcValueSize(const std::string_view& value, float font_size);
  void CalcSummarySize(const std::string_view& summary, float font_size);
};

} // namespace FullscreenUI

// Host UI triggers from Big Picture mode.
namespace Host {

/// Returns the name of the default Big Picture theme to use based on the host theme.
const char* GetDefaultFullscreenUITheme();

} // namespace Host
