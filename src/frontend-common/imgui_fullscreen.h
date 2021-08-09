#pragma once
#include "common/types.h"
#include "imgui.h"
#include <functional>
#include <string>
#include <vector>

namespace ImGuiFullscreen {
#define HEX_TO_IMVEC4(hex, alpha)                                                                                      \
  ImVec4(static_cast<float>((hex >> 16) & 0xFFu) / 255.0f, static_cast<float>((hex >> 8) & 0xFFu) / 255.0f,            \
         static_cast<float>(hex & 0xFFu) / 255.0f, static_cast<float>(alpha) / 255.0f)

using ResolveTextureHandleCallback = ImTextureID (*)(const std::string& path);

static constexpr float LAYOUT_SCREEN_WIDTH = 1280.0f;
static constexpr float LAYOUT_SCREEN_HEIGHT = 720.0f;
static constexpr float LAYOUT_LARGE_FONT_SIZE = 26.0f;
static constexpr float LAYOUT_MEDIUM_FONT_SIZE = 16.0f;
static constexpr float LAYOUT_SMALL_FONT_SIZE = 10.0f;
static constexpr float LAYOUT_MENU_BUTTON_HEIGHT = 50.0f;
static constexpr float LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY = 26.0f;
static constexpr float LAYOUT_MENU_BUTTON_X_PADDING = 15.0f;
static constexpr float LAYOUT_MENU_BUTTON_Y_PADDING = 10.0f;

extern ImFont* g_standard_font;
extern ImFont* g_medium_font;
extern ImFont* g_large_font;

extern float g_layout_scale;
extern float g_layout_padding_left;
extern float g_layout_padding_top;
extern float g_menu_bar_size;

static ALWAYS_INLINE float DPIScale(float v)
{
  return ImGui::GetIO().DisplayFramebufferScale.x * v;
}

static ALWAYS_INLINE float DPIScale(int v)
{
  return ImGui::GetIO().DisplayFramebufferScale.x * static_cast<float>(v);
}

static ALWAYS_INLINE ImVec2 DPIScale(const ImVec2& v)
{
  const ImVec2& fbs = ImGui::GetIO().DisplayFramebufferScale;
  return ImVec2(v.x * fbs.x, v.y * fbs.y);
}

static ALWAYS_INLINE float WindowWidthScale(float v)
{
  return ImGui::GetWindowWidth() * v;
}

static ALWAYS_INLINE float WindowHeightScale(float v)
{
  return ImGui::GetWindowHeight() * v;
}

static ALWAYS_INLINE float LayoutScale(float v)
{
  return g_layout_scale * v;
}

static ALWAYS_INLINE ImVec2 LayoutScale(const ImVec2& v)
{
  return ImVec2(v.x * g_layout_scale, v.y * g_layout_scale);
}

static ALWAYS_INLINE ImVec2 LayoutScale(float x, float y)
{
  return ImVec2(x * g_layout_scale, y * g_layout_scale);
}

static ALWAYS_INLINE ImVec2 LayoutScaleAndOffset(float x, float y)
{
  return ImVec2(g_layout_padding_left + x * g_layout_scale, g_layout_padding_top + y * g_layout_scale);
}

static ALWAYS_INLINE ImVec4 UIPrimaryColor()
{
  return HEX_TO_IMVEC4(0x212121, 0xff);
}

static ALWAYS_INLINE ImVec4 UIPrimaryLightColor()
{
  return HEX_TO_IMVEC4(0x484848, 0xff);
}

static ALWAYS_INLINE ImVec4 UIPrimaryDarkColor()
{
  return HEX_TO_IMVEC4(0x484848, 0xff);
}

static ALWAYS_INLINE ImVec4 UIPrimaryTextColor()
{
  return HEX_TO_IMVEC4(0xffffff, 0xff);
}

static ALWAYS_INLINE ImVec4 UIPrimaryDisabledTextColor()
{
  return HEX_TO_IMVEC4(0xaaaaaa, 0xff);
}

static ALWAYS_INLINE ImVec4 UITextHighlightColor()
{
  return HEX_TO_IMVEC4(0x90caf9, 0xff);
}

static ALWAYS_INLINE ImVec4 UIPrimaryLineColor()
{
  return HEX_TO_IMVEC4(0xffffff, 0xff);
}

static ALWAYS_INLINE ImVec4 UISecondaryColor()
{
  return HEX_TO_IMVEC4(0x1565c0, 0xff);
}

static ALWAYS_INLINE ImVec4 UISecondaryLightColor()
{
  return HEX_TO_IMVEC4(0x5e92f3, 0xff);
}

static ALWAYS_INLINE ImVec4 UISecondaryDarkColor()
{
  return HEX_TO_IMVEC4(0x003c8f, 0xff);
}

static ALWAYS_INLINE ImVec4 UISecondaryTextColor()
{
  return HEX_TO_IMVEC4(0xffffff, 0xff);
}

void SetFontFilename(std::string filename);
void SetFontData(std::vector<u8> data);
void SetIconFontFilename(std::string icon_font_filename);
void SetIconFontData(std::vector<u8> data);
void SetFontSize(float size_pixels);
void SetFontGlyphRanges(const ImWchar* glyph_ranges);

/// Changes the menu bar size. Don't forget to call UpdateLayoutScale() and UpdateFonts().
void SetMenuBarSize(float size);

/// Resolves a texture name to a handle.
void SetResolveTextureFunction(ResolveTextureHandleCallback callback);

/// Rebuilds fonts to a new scale if needed. Returns true if fonts have changed and the texture needs updating.
bool UpdateFonts();

/// Removes the fullscreen fonts, leaving only the standard font.
void ResetFonts();

bool UpdateLayoutScale();

void BeginLayout();
void EndLayout();

bool IsCancelButtonPressed();

void DrawWindowTitle(const char* title);

bool BeginFullscreenColumns(const char* title = nullptr);
void EndFullscreenColumns();

bool BeginFullscreenColumnWindow(float start, float end, const char* name,
                                 const ImVec4& background = HEX_TO_IMVEC4(0x212121, 0xFF));
void EndFullscreenColumnWindow();

bool BeginFullscreenWindow(float left, float top, float width, float height, const char* name,
                           const ImVec4& background = HEX_TO_IMVEC4(0x212121, 0xFF), float rounding = 0.0f,
                           float padding = 0.0f, ImGuiWindowFlags flags = 0);
bool BeginFullscreenWindow(const ImVec2& position, const ImVec2& size, const char* name,
                           const ImVec4& background = HEX_TO_IMVEC4(0x212121, 0xFF), float rounding = 0.0f,
                           float padding = 0.0f, ImGuiWindowFlags flags = 0);
void EndFullscreenWindow();

void BeginMenuButtons(u32 num_items = 0, float y_align = 0.0f, float x_padding = LAYOUT_MENU_BUTTON_X_PADDING,
                      float y_padding = LAYOUT_MENU_BUTTON_Y_PADDING, float item_height = LAYOUT_MENU_BUTTON_HEIGHT);
void EndMenuButtons();
bool MenuButtonFrame(const char* str_id, bool enabled, float height, bool* visible, bool* hovered, ImVec2* min,
                     ImVec2* max, ImGuiButtonFlags flags = 0, float hover_alpha = 1.0f);
void MenuHeading(const char* title, bool draw_line = true);
bool MenuHeadingButton(const char* title, const char* value = nullptr, bool enabled = true, bool draw_line = true);
bool ActiveButton(const char* title, bool is_active, bool enabled = true,
                  float height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, ImFont* font = g_large_font);
bool MenuButton(const char* title, const char* summary, bool enabled = true, float height = LAYOUT_MENU_BUTTON_HEIGHT,
                ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
bool MenuButtonWithValue(const char* title, const char* summary, const char* value, bool enabled = true,
                         float height = LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = g_large_font,
                         ImFont* summary_font = g_medium_font);
bool MenuImageButton(const char* title, const char* summary, ImTextureID user_texture_id, const ImVec2& image_size,
                     bool enabled = true, float height = LAYOUT_MENU_BUTTON_HEIGHT,
                     const ImVec2& uv0 = ImVec2(0.0f, 0.0f), const ImVec2& uv1 = ImVec2(1.0f, 1.0f),
                     ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
bool FloatingButton(const char* text, float x, float y, float width = -1.0f,
                    float height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, float anchor_x = 0.0f, float anchor_y = 0.0f,
                    bool enabled = true, ImFont* font = g_large_font, ImVec2* out_position = nullptr);
bool ToggleButton(const char* title, const char* summary, bool* v, bool enabled = true,
                  float height = LAYOUT_MENU_BUTTON_HEIGHT, ImFont* font = g_large_font,
                  ImFont* summary_font = g_medium_font);
bool RangeButton(const char* title, const char* summary, s32* value, s32 min, s32 max, s32 increment,
                 const char* format = "%d", bool enabled = true, float height = LAYOUT_MENU_BUTTON_HEIGHT,
                 ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
bool RangeButton(const char* title, const char* summary, float* value, float min, float max, float increment,
                 const char* format = "%f", bool enabled = true, float height = LAYOUT_MENU_BUTTON_HEIGHT,
                 ImFont* font = g_large_font, ImFont* summary_font = g_medium_font);
bool EnumChoiceButtonImpl(const char* title, const char* summary, s32* value_pointer,
                          const char* (*to_display_name_function)(s32 value, void* opaque), void* opaque, u32 count,
                          bool enabled, float height, ImFont* font, ImFont* summary_font);

template<typename DataType, typename CountType>
static ALWAYS_INLINE bool EnumChoiceButton(const char* title, const char* summary, DataType* value_pointer,
                                           const char* (*to_display_name_function)(DataType value), CountType count,
                                           bool enabled = true, float height = LAYOUT_MENU_BUTTON_HEIGHT,
                                           ImFont* font = g_large_font, ImFont* summary_font = g_medium_font)
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

void BeginNavBar(float x_padding = LAYOUT_MENU_BUTTON_X_PADDING, float y_padding = LAYOUT_MENU_BUTTON_Y_PADDING);
void EndNavBar();
void NavTitle(const char* title, float height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, ImFont* font = g_large_font);
void RightAlignNavButtons(u32 num_items = 0, float item_width = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY,
                          float item_height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
bool NavButton(const char* title, bool is_active, bool enabled = true, float width = -1.0f,
               float height = LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, ImFont* font = g_large_font);

using FileSelectorCallback = std::function<void(const std::string& path)>;
using FileSelectorFilters = std::vector<std::string>;
bool IsFileSelectorOpen();
void OpenFileSelector(const char* title, bool select_directory, FileSelectorCallback callback,
                      FileSelectorFilters filters = FileSelectorFilters(),
                      std::string initial_directory = std::string());
void CloseFileSelector();

using ChoiceDialogCallback = std::function<void(s32 index, const std::string& title, bool checked)>;
using ChoiceDialogOptions = std::vector<std::pair<std::string, bool>>;
bool IsChoiceDialogOpen();
void OpenChoiceDialog(const char* title, bool checkable, ChoiceDialogOptions options, ChoiceDialogCallback callback);
void CloseChoiceDialog();

float GetNotificationVerticalPosition();
float GetNotificationVerticalDirection();
void SetNotificationVerticalPosition(float position, float direction);

void OpenBackgroundProgressDialog(const char* str_id, std::string message, s32 min, s32 max, s32 value);
void UpdateBackgroundProgressDialog(const char* str_id, std::string message, s32 min, s32 max, s32 value);
void CloseBackgroundProgressDialog(const char* str_id);

void AddNotification(float duration, std::string title, std::string text, std::string image_path);
void ClearNotifications();

} // namespace ImGuiFullscreen
