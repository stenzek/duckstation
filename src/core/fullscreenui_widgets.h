// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include "util/translation.h"

#include "common/move_only_function.h"
#include "common/small_string.h"
#include "common/types.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome.h"
#include "imgui.h"
#include "imgui_internal.h"

#include "fmt/format.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Error;
class GSVector2;
class Image;
class ProgressCallbackWithPrompt;

enum class GPUPresentResult : u8;
class GPUPipeline;
class GPUTexture;
class GPUSwapChain;

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
inline constexpr float LAYOUT_SMALL_FONT_SIZE = 10.0f;
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
inline constexpr float LAYOUT_HORIZONTAL_MENU_HEIGHT = 280.0f;
inline constexpr float LAYOUT_SCROLL_FADE_HEIGHT = 40.0f;
inline constexpr float LAYOUT_HORIZONTAL_MENU_PADDING = 24.0f;
inline constexpr float LAYOUT_HORIZONTAL_MENU_ITEM_WIDTH = 250.0f;
inline constexpr float LAYOUT_HORIZONTAL_MENU_ITEM_IMAGE_SIZE = 112.0f;
inline constexpr float LAYOUT_SHADOW_OFFSET = 1.0f;
inline constexpr float LAYOUT_SMALL_POPUP_PADDING = 20.0f;
inline constexpr float LAYOUT_LARGE_POPUP_PADDING = 30.0f;
inline constexpr float LAYOUT_LARGE_POPUP_ROUNDING = 40.0f;
inline constexpr float LAYOUT_WIDGET_FRAME_ROUNDING = 20.0f;
inline constexpr float LAYOUT_FRAME_BORDER_SIZE = 1.0f;
inline constexpr float LAYOUT_PAGE_HEADER_HEIGHT = 52.0f;
inline constexpr float LAYOUT_PAGE_X_PADDING = 10.0f;
inline constexpr ImVec2 LAYOUT_CENTER_ALIGN_TEXT = ImVec2(0.5f, 0.0f);

/// Font size, weight and colour for text in the UI font. The colour goes through ImGui::GetColorU32() when drawn, so it
/// follows the style alpha.
struct TextStyle
{
  float font_size;
  float font_weight;
  ImVec4 color;

  ALWAYS_INLINE TextStyle WithColor(const ImVec4& new_color) const { return {font_size, font_weight, new_color}; }
  ALWAYS_INLINE TextStyle WithSize(float new_size) const { return {new_size, font_weight, color}; }
  ALWAYS_INLINE TextStyle WithWeight(float new_weight) const { return {font_size, new_weight, color}; }
  ALWAYS_INLINE TextStyle WithSizeAndWeight(float new_size, float new_weight) const
  {
    return {new_size, new_weight, color};
  }
};

struct ALIGN_TO_CACHE_LINE UIStyles
{
  ImVec4 BackgroundColor;
  ImVec4 BackgroundTextColor;
  ImVec4 BackgroundLineColor;
  ImVec4 BackgroundHighlight;
  ImVec4 PopupBackgroundColor;
  ImVec4 PopupFrameBackgroundColor;
  ImVec4 PopupHighlight;
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

  // Semantic colours, derived from the theme colours above.
  ImVec4 SurfaceColor;
  ImVec4 SurfaceRaisedColor;
  ImVec4 BorderColor;
  ImVec4 MutedTextColor;
  ImVec4 AccentColor;
  ImVec4 AccentTextColor;

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
  float BlurBackgroundWeight;

  // Text presets, updated with the layout scale and the theme.
  TextStyle HeadingText;    // large bold, page titles
  TextStyle TitleText;      // medium bold, row titles
  TextStyle BodyText;       // medium
  TextStyle MutedText;      // medium, muted colour
  TextStyle SmallBoldText;  // medium-small bold
  TextStyle SmallMutedText; // medium-small, muted colour

  static constexpr float NormalFontWeight = 0.0f;
  static constexpr float BoldFontWeight = 500.0f;
  static constexpr float ExtraBoldFontWeight = 800.0f;

  bool Animations : 1;
  bool SmoothScrolling : 1;
  bool MenuBorders : 1;
  bool BlurMenuBackground : 1;
  bool SoundEffects : 1;
  bool IsDarkTheme : 1;
  bool UsingPSIcons : 1;
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

ALWAYS_INLINE ImRect ImRectFromExtent(const ImVec2& pos, const ImVec2& size)
{
  return ImRect(pos, pos + size);
}
ALWAYS_INLINE ImRect ImRectFromExtent(const ImVec2& pos, const float width, const float height)
{
  return ImRectFromExtent(pos, ImVec2(width, height));
}
ALWAYS_INLINE ImRect ImRectFromLayoutExtent(const ImVec2& pos, const ImVec2& size)
{
  return ImRectFromExtent(pos, LayoutScale(size));
}
ALWAYS_INLINE ImRect ImRectFromLayoutExtent(const ImVec2& pos, const float width, const float height)
{
  return ImRectFromExtent(pos, LayoutScale(width, height));
}

/// Size of a single unwrapped line of text in the UI font.
ALWAYS_INLINE ImVec2 MeasureText(float font_size, float font_weight, std::string_view text)
{
  return text.empty() ?
           ImVec2() :
           UIStyle.Font->CalcTextSizeA(font_size, font_weight, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
}
ALWAYS_INLINE ImVec2 MeasureText(const TextStyle& style, std::string_view text)
{
  return MeasureText(style.font_size, style.font_weight, text);
}

/// Size of text word-wrapped to wrap_width.
ALWAYS_INLINE ImVec2 MeasureText(const TextStyle& style, std::string_view text, float wrap_width)
{
  return text.empty() ? ImVec2() :
                        UIStyle.Font->CalcTextSizeA(style.font_size, style.font_weight, FLT_MAX, wrap_width,
                                                    text.data(), text.data() + text.size());
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

#if 0
ALWAYS_INLINE ImVec2 ApplyPivot(const ImVec2& pos, const ImVec2& size, const ImVec2& pivot)
{
  return ImVec2(pos.x - size.x * pivot.x, pos.y - size.y * pivot.y);
}
#endif

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

/// Placeholder for an empty save slot: dimmed fill, dashed border and a centred plus.
void DrawEmptySlotTile(ImDrawList* dl, const ImRect& bb, float rounding, float alpha = 1.0f);

/// Fits an image to the specified bounds, cropping if needed. Returns UV coordinates.
ImRect FitImage(const ImVec2& fit_size, const ImVec2& image_size);

/// Initializes, setting up any state.
bool InitializeWidgets(Error* error);

/// Shuts down, clearing all state.
void ShutdownWidgets();

bool CreateWidgetsGPUResources(Error* error);
void DestroyWidgetsGPUResources();
GPUPipeline* GetPresentCopyPipeline();
GPUPipeline* GetCarouselPipeline();

std::span<const char* const> GetThemeNames();
std::span<const char* const> GetThemeDisplayNames();
std::vector<std::string_view> GetLocalizedThemeDisplayNames();
void UpdateTheme();

void SetFont(ImFont* ui_font);
bool UpdateLayoutScale();

/// Texture cache.
inline constexpr std::string_view GENERATED_ARTWORK_KEY_PREFIX = "generated:";
inline constexpr u32 DEFAULT_TEXTURE_CACHE_SIZE_MB = 512;
GPUTexture* GetPlaceholderTexture();
std::unique_ptr<GPUTexture> LoadTexture(std::string_view path);
std::unique_ptr<GPUTexture> LoadTexture(std::string_view path, std::string_view name);
std::unique_ptr<GPUTexture> LoadTexture(std::string_view path, u32 svg_width, u32 svg_height);
std::unique_ptr<GPUTexture> LoadTexture(std::string_view path, const ImVec2& size);
GPUTexture* FindCachedTexture(std::string_view name);
GPUTexture* FindCachedTexture(std::string_view name, u32 svg_width, u32 svg_height);
GPUTexture* FindCachedTexture(std::string_view name, const ImVec2& size);
GPUTexture* GetCachedTexture(std::string_view name);
GPUTexture* GetCachedTexture(std::string_view path, std::string_view name);
GPUTexture* GetCachedTexture(std::string_view name, u32 svg_width, u32 svg_height);
GPUTexture* GetCachedTexture(std::string_view name, const ImVec2& size);
GPUTexture* GetCachedTextureAsync(std::string_view name);
GPUTexture* GetCachedTextureAsync(std::string_view path, std::string_view name);
GPUTexture* GetCachedTextureAsync(std::string_view name, u32 svg_width, u32 svg_height);
GPUTexture* GetCachedTextureAsync(std::string_view name, const ImVec2& size);
GPUTexture* InsertCachedTexture(std::string name, std::unique_ptr<GPUTexture> texture);
bool InvalidateCachedTexture(std::string_view path);
bool TextureNeedsSVGDimensions(std::string_view path);
void UploadAsyncTextures();
void QueueTextureRecycle(std::unique_ptr<GPUTexture> texture);
void RecycleQueuedTextures();

/// Screen transitions.
inline constexpr float SHORT_TRANSITION_TIME = 0.15f;
inline constexpr float DEFAULT_TRANSITION_TIME = 0.22f;
inline constexpr float LONG_TRANSITION_TIME = 0.3f;

enum class TransitionState : u8
{
  Inactive,
  Starting,
  Active,
};

enum class TransitionEffect : u8
{
  None,
  Fade,
  ZoomIn,
  ZoomOut,
  SlideLeft,
  SlideRight,
};

using TransitionStartCallback = MoveOnlyFunction<void()>;
void BeginTransition(TransitionStartCallback func, float time = DEFAULT_TRANSITION_TIME);
void BeginTransition(float time, TransitionStartCallback func);
void BeginTransition(TransitionEffect effect, float time, TransitionStartCallback func);
void CancelTransition();
bool IsTransitionActive();
TransitionState GetTransitionState();
GPUTexture* GetTransitionRenderTexture(GPUSwapChain* const swap_chain);
void RenderTransitionBlend(GPUSwapChain* const swap_chain, GPUTexture* const transition_texture);
void UpdateTransitionState();

/// Screen blurring.
bool CanBlurBackground();
void InvalidateBlurBackground();
GPUTexture* GetBlurRenderTexture(GPUSwapChain* const swap_chain);
const GSVector2& GetBlurTextureScale();
void RenderBlur(GPUSwapChain* const swap_chain, GPUTexture* const blur_render_texture);
bool BeginBlurBackground(ImDrawList* const dl, const ImVec2& bb_min, const ImVec2& bb_max);
void EndBlurBackground(ImDrawList* const dl);

/// Blurred image backdrop for a header.
GPUTexture* GetBlurredHeaderBackground(GPUTexture* image, std::string_view image_key, const ImVec2& size);

/// Layout helpers.
void BeginLayout();
void EndLayout();

/// Enqueues a sound effect, and prevents any other sound effects from being started this frame.
void EnqueueSoundEffect(std::string_view sound_effect);

float GetScreenBottomMargin();

bool IsFixedPopupDialogOpen(std::string_view name);
void OpenFixedPopupDialog(std::string_view name);
void CloseFixedPopupDialog();
bool BeginFixedPopupDialog(float scaled_window_padding = LayoutScale(20.0f),
                           float scaled_window_rounding = LayoutScale(20.0f),
                           const ImVec2& scaled_window_size = ImVec2(), const ImVec2& min_window_size = ImVec2());
void EndFixedPopupDialog();

/// Stacked dialog whose contents are drawn by a callback. Sizes are unscaled. The close callback runs once the
/// dialog has been removed, however it was closed.
using CustomDialogDrawCallback = MoveOnlyFunction<void()>;
using CustomDialogCloseCallback = MoveOnlyFunction<void()>;
void OpenCustomDialog(std::string title, float window_padding, float window_rounding, const ImVec2& window_size,
                      const ImVec2& min_window_size, CustomDialogDrawCallback draw_callback,
                      CustomDialogCloseCallback close_callback = {});

/// Closes the dialog currently being drawn, i.e. call from within a custom dialog's draw callback.
void CloseCurrentDialog();
void CloseCurrentDialogImmediately();
u32 GetCurrentDialogSerial();

/// Closes a dialog later, e.g. from an async callback. Does nothing if the dialog has already gone.
void CloseDialog(u32 serial);

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
                           const ImVec2& padding = ImVec2(), ImGuiWindowFlags flags = 0, bool blur = false);
void EndFullscreenWindow(bool allow_wrap_x = false, bool allow_wrap_y = true);
void SetWindowNavWrapping(bool allow_wrap_x = false, bool allow_wrap_y = true);
bool BeginBlurWindow(const char* name, bool* p_open = nullptr, ImGuiWindowFlags flags = 0, bool blur = true);

enum class FooterBackground : u8
{
  None,
  Solid,
  Blurred,
};

bool IsGamepadInputSource();
std::string_view GetControllerIconMapping(std::string_view icon);

/// The gamepad icon (mapped for the controller in use) or the keyboard icon, depending on the last input source.
std::string_view GetInputHint(std::string_view gamepad_icon, std::string_view keyboard_icon);

/// Input hints are drawn in the muted text style, vertically centred in [pos.y, pos.y + height].
/// DrawInputHint() returns the width used.
float MeasureInputHint(std::string_view hint);
float DrawInputHint(ImDrawList* dl, const ImVec2& pos, float height, std::string_view hint);

void SetFullscreenFooterText(std::span<const std::pair<const char*, std::string_view>> items);
void SetFullscreenFooterText(std::span<const std::pair<const char*, std::string_view>> left_items,
                             std::span<const std::pair<const char*, std::string_view>> right_items);
void SetStandardSelectionFooterText(bool back_instead_of_cancel);
void SetFullscreenFooterBackground(FooterBackground background);
void DrawFullscreenFooter();

void BeginMenuButtons(u32 num_items = 0, float y_align = 0.0f, float x_padding = LAYOUT_MENU_BUTTON_X_PADDING,
                      float y_padding = LAYOUT_MENU_BUTTON_Y_PADDING, float x_spacing = 0.0f,
                      float y_spacing = LAYOUT_MENU_BUTTON_SPACING);
void EndMenuButtons();

/// Fades out the content drawn between Begin/End towards the top/bottom edges of the current window when there is
/// more to scroll in that direction. Works per-vertex, so large quads fade across their whole height.
void BeginScrollFade();
void EndScrollFade();
float GetMenuButtonAvailableWidth();
bool MenuButtonFrame(std::string_view str_id, float height, bool enabled, ImRect* item_bb, bool* visible, bool* hovered,
                     ImGuiButtonFlags flags = 0);
bool MenuButtonFrame(std::string_view str_id, bool enabled, const ImRect& bb, bool* visible, bool* hovered,
                     ImGuiButtonFlags flags = 0);

/// For windows that handle pointer drags themselves instead of scrolling, e.g. a horizontal carousel. Drags are
/// detected like drag scrolling, which cancels the press underneath. Returns true while the current window is being
/// dragged, and on the frame it's released, when fling_distance is how much further a fling would carry the content.
bool GetWindowDrag(ImVec2* fling_distance);

/// A single line of text, pills and spaces, laid out when drawn. Each item is vertically centred in the rect passed to
/// Draw(), or put on its bottom edge. Shrink items are ellipsised to make the line fit, each getting an equal share of
/// what's left, Fill items share the space left over, and without any Fill items the line is aligned by align_x. Text
/// is copied, so the caller's buffers can be reused straight away.
class InlineRun
{
public:
  enum Flags : u8
  {
    Shadow = (1 << 0),
    Shrink = (1 << 1), // also drawn with a shadow
    Bottom = (1 << 2), // e.g. smaller text after a title
  };

  explicit InlineRun(float spacing);

  u32 Text(const TextStyle& style, std::string_view text, u8 flags = 0);
  template<typename... T>
  u32 TextFmt(const TextStyle& style, fmt::format_string<T...> fmt, T&&... args)
  {
    const u32 offset = m_buffer.length();
    m_buffer.append_format(fmt, std::forward<T>(args)...);
    return AddText(style, offset, 0);
  }

  /// Input hint, see DrawInputHint().
  u32 Hint(std::string_view hint);
  u32 Pill(std::string_view text, const TextStyle& style, u32 bg_color, float height, float rounding = -1.0f);
  template<typename... T>
  u32 PillFmt(const TextStyle& style, u32 bg_color, float height, float rounding, fmt::format_string<T...> fmt,
              T&&... args)
  {
    const u32 offset = m_buffer.length();
    m_buffer.append_format(fmt, std::forward<T>(args)...);
    return AddPill(style, offset, bg_color, height, rounding);
  }

  /// Empty slot, e.g. for a widget drawn afterwards at GetItemRect(). Height 0 is the full height of the line.
  u32 Space(float width, float height = 0.0f, u8 flags = 0);

  /// Takes no spacing of its own, only the item after it is spaced.
  u32 Fill();

  /// Overrides the spacing before the next item.
  void Spacing(float spacing);

  /// Width of the line before any shrinking or filling.
  float GetWidth() const;
  void Draw(ImDrawList* dl, const ImRect& bb, float align_x = 0.0f);

  /// Only valid after Draw().
  const ImRect& GetItemRect(u32 index) const { return m_items[index].rect; }

private:
  static constexpr u32 MAX_ITEMS = 8;

  enum class ItemType : u8
  {
    Text,
    Pill,
    Space,
    Fill,
  };

  struct Item
  {
    ItemType type;
    u8 flags;
    u16 text_offset;
    u16 text_length;
    float spacing;
    float width;
    float height;
    float rounding;
    u32 bg_color;
    TextStyle style;
    ImRect rect;
  };

  Item& AddItem(ItemType type, float width, float height);
  u32 AddText(const TextStyle& style, u32 offset, u8 flags);
  u32 AddPill(const TextStyle& style, u32 offset, u32 bg_color, float height, float rounding);

  Item m_items[MAX_ITEMS];
  u32 m_num_items = 0;
  float m_spacing;
  float m_next_spacing;
  SmallString m_buffer;
};

/// Rounded label. Rounding < 0 rounds the ends fully. DrawPill() returns the width used.
ImVec2 MeasurePill(std::string_view text, float height, float font_size, float font_weight);
ALWAYS_INLINE ImVec2 MeasurePill(std::string_view text, float height, const TextStyle& style)
{
  return MeasurePill(text, height, style.font_size, style.font_weight);
}
float DrawPill(ImDrawList* dl, const ImVec2& pos, float height, std::string_view text, const TextStyle& style,
               u32 bg_color, float rounding = -1.0f);

/// Mouse-clickable pill, not gamepad navigable. A transparent bg_color is filled when hovered.
bool PillButton(ImGuiID id, std::string_view label, float height, u32 bg_color, u32 text_color, u32 border_color,
                bool* hovered, GPUTexture* icon = nullptr);

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
void RenderShadowedTextClipped(ImDrawList* draw_list, const ImVec2& pos_min, const ImVec2& pos_max,
                               const TextStyle& style, std::string_view text, const ImVec2& align = ImVec2(0.0f, 0.0f),
                               const ImRect* clip_rect = nullptr);

/// Single line, truncated with an ellipsis if it does not fit between pos_min.x and pos_max.x.
void RenderShadowedTextEllipsis(ImDrawList* draw_list, const ImVec2& pos_min, const ImVec2& pos_max,
                                const TextStyle& style, std::string_view text,
                                const ImVec2& align = ImVec2(0.0f, 0.0f));

/// Unshadowed single line at pos. Returns the width used.
float RenderText(ImDrawList* draw_list, const ImVec2& pos, const TextStyle& style, std::string_view text);

ImVec2 RenderOutlinedText(ImDrawList* draw_list, ImFont* font, float size, float weight, const ImVec2& pos, ImU32 col,
                          std::string_view text);
void RenderAutoLabelText(ImDrawList* draw_list, ImFont* font, float font_size, float font_weight, float label_weight,
                         const ImVec2& pos_min, const ImVec2& pos_max, u32 color, std::string_view text,
                         char separator = ':', float shadow_offset = LayoutScale(LAYOUT_SHADOW_OFFSET));
void TextAlignedMultiLine(float align_x, const char* text, const char* text_end = nullptr, float wrap_width = -1.0f);
void TextUnformatted(std::string_view text);
void MenuHeading(std::string_view title, bool draw_line = true);
void MenuSeparator();
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
bool MenuButtonWithInlineValue(std::string_view title, std::string_view value, std::string_view right_value,
                               bool enabled = true, const float min_title_width = 0.0f,
                               const ImVec2& text_align = ImVec2(0.0f, 0.0f));
bool MenuImageButton(std::string_view title, std::string_view summary, std::string_view value, ImTextureID image,
                     const ImVec2& image_size = ImVec2(0.0f, 0.0f), bool enabled = true,
                     const ImVec2& uv0 = ImVec2(0.0f, 0.0f), const ImVec2& uv1 = ImVec2(1.0f, 1.0f));
bool FloatingButton(std::string_view text, float x, float y, float anchor_x = 0.0f, float anchor_y = 0.0f,
                    bool enabled = true, ImVec2* out_position = nullptr, bool repeat_button = false);
bool ToggleButton(std::string_view title, std::string_view summary, bool* v, bool enabled = true);
bool ThreeWayToggleButton(std::string_view title, std::string_view summary, std::optional<bool>* v,
                          bool enabled = true);
bool EnumChoiceButtonImpl(std::string_view title, std::string_view summary, s32* value_pointer,
                          const char* (*to_display_name_function)(s32 value, void* opaque), void* opaque, u32 count,
                          bool enabled);
bool MenuActionButton(std::string_view title, std::string_view summary, std::string_view value,
                      bool dropdown_icon = false, bool enabled = true);

template<typename DataType, typename CountType>
ALWAYS_INLINE bool EnumChoiceButton(std::string_view title, std::string_view summary, DataType* value_pointer,
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
bool NavButton(std::string_view title, bool is_active, bool enabled = true, bool show_active_indicator = false);
bool NavTab(std::string_view title, bool is_active, bool enabled, float width);

/// Mouse-clickable segmented control, not gamepad navigable. Call Segment() for each item between
/// BeginSegmentedControl() and EndSegmentedControl(), it returns true when an inactive segment is clicked.
/// MeasureSegment() and GetSegmentedControlFrameSize() give the size beforehand, e.g. for right alignment.
inline constexpr float SEGMENTED_CONTROL_HEIGHT = 30.0f;
inline constexpr float SEGMENTED_CONTROL_PADDING = 4.0f;
float MeasureSegment(std::string_view label, bool has_icon = false);
ImVec2 GetSegmentedControlFrameSize(float segments_width);
void BeginSegmentedControl(std::string_view str_id);
bool Segment(std::string_view label, bool active, GPUTexture* icon = nullptr);
void EndSegmentedControl();

/// Page header: a non-navigable window across the top of the screen, with a border along the bottom.
bool BeginPageHeader(const char* name, const ImVec4& background, bool blur, float height = LAYOUT_PAGE_HEADER_HEIGHT);

/// Round back button, returns true when clicked.
bool PageHeaderBackButton();

/// Heading-sized title, followed by a muted " / breadcrumb" if one is given. Items after it continue on the same line.
void PageHeaderTitle(std::string_view title, std::string_view breadcrumb = {});
void EndPageHeader();

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

enum class OnScreenKeyboardMode : u8
{
  Alphanumeric,
  Numeric,
};

/// Registers the previously submitted InputText item for gamepad keyboard handling.
/// Returns true when the keyboard was closed with its Confirm action.
bool HandleOnScreenKeyboard(OnScreenKeyboardMode mode = OnScreenKeyboardMode::Alphanumeric, bool allow_decimal = false,
                            bool allow_negative = false, bool allow_newline = false);
bool IsOnScreenKeyboardOpen();

bool InputTextWithIcon(const char* str_id, std::string_view icon, const char* hint, char* buf, size_t buf_size,
                       float width, float font_size, float font_weight, ImGuiInputTextFlags flags = 0,
                       ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr);
bool InputTextWithSuffix(const char* str_id, std::string_view suffix, char* buf, size_t buf_size,
                         ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr,
                         void* user_data = nullptr);

bool AreAnyWidgetsDialogOpen();
bool AreAnyWidgetsDialogInteractable();

/// Dialogs are drawn as a stack, each nested inside the one below it. Only the top dialog receives input.
class PopupDialog;
PopupDialog* PushDialog(std::unique_ptr<PopupDialog> dialog);
PopupDialog* FindDialog(u32 serial);

/// Creates a dialog of type T with the given constructor arguments, and pushes it.
template<typename T, typename... Args>
T* PushDialog(Args&&... args)
{
  return static_cast<T*>(PushDialog(std::make_unique<T>(std::forward<Args>(args)...)));
}

/// Returns the open dialog with the given serial, or null. Serials are unique, so T must be the type that was pushed.
template<typename T = PopupDialog>
T* FindDialog(u32 serial)
{
  return static_cast<T*>(FindDialog(serial));
}

using FileSelectorCallback = MoveOnlyFunction<void(std::string path)>;
using FileSelectorFilters = std::vector<std::string>;
void OpenFileSelector(std::string_view title, FileSelectorFilters filters, std::string initial_directory,
                      FileSelectorCallback callback);

using DirectorySelectorCallback = MoveOnlyFunction<void(std::string path)>;
void OpenDirectorySelector(std::string_view title, std::string initial_directory, std::string default_directory,
                           DirectorySelectorCallback callback);

using ChoiceDialogCallback = MoveOnlyFunction<void(s32 index, const std::string& title, bool checked)>;
using ChoiceDialogOptions = std::vector<std::pair<std::string, bool>>;
void OpenChoiceDialog(std::string_view title, bool checkable, ChoiceDialogOptions options,
                      ChoiceDialogCallback callback);

using DropdownDialogCallback = MoveOnlyFunction<void(s32 index, const std::string& title)>;
using DropdownDialogOptions = std::vector<std::pair<std::string, bool>>;
void OpenDropdownDialog(std::string_view hidden_title, DropdownDialogOptions options, DropdownDialogCallback callback,
                        float min_width = 0.0f);

using InputStringDialogCallback = MoveOnlyFunction<void(std::string text)>;
void OpenInputStringDialog(std::string_view title, std::string message, std::string caption, std::string ok_button_text,
                           std::string initial_value, InputStringDialogCallback callback);

using ConfirmMessageDialogCallback = MoveOnlyFunction<void(bool)>;
using InfoMessageDialogCallback = MoveOnlyFunction<void()>;
using MessageDialogCallback = MoveOnlyFunction<void(s32)>;
void OpenConfirmMessageDialog(std::string icon, std::string_view title, std::string message,
                              ConfirmMessageDialogCallback callback,
                              std::string yes_button_text = FSUI_ICONSTR(ICON_FA_CHECK, "Yes"),
                              std::string no_button_text = FSUI_ICONSTR(ICON_FA_XMARK, "No"));
void OpenInfoMessageDialog(std::string icon, std::string_view title, std::string message,
                           InfoMessageDialogCallback callback = {},
                           std::string button_text = FSUI_ICONSTR(ICON_FA_SQUARE_XMARK, "Close"));
void OpenMessageDialog(std::string icon, std::string_view title, std::string message, MessageDialogCallback callback,
                       std::string first_button_text, std::string second_button_text, std::string third_button_text);

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
  virtual ~PopupDialog();

  ALWAYS_INLINE u32 GetSerial() const { return m_serial; }
  ALWAYS_INLINE const std::string& GetTitle() const { return m_title; }
  ALWAYS_INLINE bool IsOpen() const { return (m_state != State::Inactive); }
  ALWAYS_INLINE bool IsInteractable() const { return (m_state >= State::Open && m_state <= State::Opening); }

  virtual void Draw() = 0;

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

  // Titles starting with "##" have no title bar, and therefore no close button.
  void SetTitleAndOpen(std::string title, bool user_closeable = true);

  bool BeginRender(float scaled_window_padding = LayoutScale(20.0f), float scaled_window_rounding = LayoutScale(20.0f),
                   const ImVec2& scaled_window_size = ImVec2(0.0f, 0.0f), const ImVec2* position = nullptr,
                   const ImVec2* pivot = nullptr);
  void EndRender();

  std::string m_title;
  float m_animation_time_remaining = 0.0f;
  u32 m_serial;
  State m_state = State::Inactive;
  bool m_user_closeable = true;
  bool m_reverse_animation = false;
  bool m_close_when_top = false; // StartClose() was called with dialogs above this one, close once they have gone.
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
  MenuButtonBounds(const std::string_view& title, const std::string_view& value, float value_x_padding,
                   const std::string_view& summary);
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
