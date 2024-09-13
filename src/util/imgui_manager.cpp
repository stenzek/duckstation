// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "imgui_manager.h"
#include "gpu_device.h"
#include "host.h"
#include "image.h"
#include "imgui_fullscreen.h"
#include "imgui_glyph_ranges.inl"
#include "input_manager.h"

#include "common/assert.h"
#include "common/easing.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "IconsFontAwesome5.h"
#include "fmt/format.h"
#include "imgui.h"
#include "imgui_freetype.h"
#include "imgui_internal.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <type_traits>
#include <unordered_map>

Log_SetChannel(ImGuiManager);

namespace ImGuiManager {
namespace {

struct SoftwareCursor
{
  std::string image_path;
  std::unique_ptr<GPUTexture> texture;
  u32 color;
  float scale;
  float extent_x;
  float extent_y;
  std::pair<float, float> pos;
};

struct OSDMessage
{
  std::string key;
  std::string text;
  Common::Timer::Value start_time;
  Common::Timer::Value move_time;
  float duration;
  float target_y;
  float last_y;
  bool is_warning;
};

} // namespace

static_assert(std::is_same_v<WCharType, ImWchar>);

static void UpdateScale();
static void SetStyle();
static void SetKeyMap();
static bool LoadFontData();
static void ReloadFontDataIfActive();
static bool AddImGuiFonts(bool fullscreen_fonts);
static ImFont* AddTextFont(float size, bool full_glyph_range);
static ImFont* AddFixedFont(float size);
static bool AddIconFonts(float size);
static void AddOSDMessage(std::string key, std::string message, float duration, bool is_warning);
static void RemoveKeyedOSDMessage(std::string key, bool is_warning);
static void ClearOSDMessages(bool clear_warnings);
static void AcquirePendingOSDMessages(Common::Timer::Value current_time);
static void DrawOSDMessages(Common::Timer::Value current_time);
static void CreateSoftwareCursorTextures();
static void UpdateSoftwareCursorTexture(u32 index);
static void DestroySoftwareCursorTextures();
static void DrawSoftwareCursor(const SoftwareCursor& sc, const std::pair<float, float>& pos);

static float s_global_prescale = 1.0f; // before window scale
static float s_global_scale = 1.0f;

static constexpr std::array<ImWchar, 4> s_ascii_font_range = {{0x20, 0x7F, 0x00, 0x00}};

static std::string s_font_path;
static std::vector<WCharType> s_font_range;
static std::vector<WCharType> s_emoji_range;

static ImFont* s_standard_font;
static ImFont* s_osd_font;
static ImFont* s_fixed_font;
static ImFont* s_medium_font;
static ImFont* s_large_font;

static DynamicHeapArray<u8> s_standard_font_data;
static DynamicHeapArray<u8> s_fixed_font_data;
static DynamicHeapArray<u8> s_icon_fa_font_data;
static DynamicHeapArray<u8> s_icon_pf_font_data;
static DynamicHeapArray<u8> s_emoji_font_data;

static float s_window_width;
static float s_window_height;
static Common::Timer s_last_render_time;

// cached copies of WantCaptureKeyboard/Mouse, used to know when to dispatch events
static std::atomic_bool s_imgui_wants_keyboard{false};
static std::atomic_bool s_imgui_wants_mouse{false};

// mapping of host key -> imgui key
static std::unordered_map<u32, ImGuiKey> s_imgui_key_map;

static constexpr float OSD_FADE_IN_TIME = 0.1f;
static constexpr float OSD_FADE_OUT_TIME = 0.4f;

static std::deque<OSDMessage> s_osd_active_messages;
static std::deque<OSDMessage> s_osd_posted_messages;
static std::mutex s_osd_messages_lock;
static bool s_show_osd_messages = true;
static bool s_scale_changed = false;

static std::array<ImGuiManager::SoftwareCursor, InputManager::MAX_SOFTWARE_CURSORS> s_software_cursors = {};
} // namespace ImGuiManager

void ImGuiManager::SetFontPathAndRange(std::string path, std::vector<WCharType> range)
{
  if (s_font_path == path && s_font_range == range)
    return;

  s_font_path = std::move(path);
  s_font_range = std::move(range);
  s_standard_font_data = {};
  ReloadFontDataIfActive();
}

void ImGuiManager::SetEmojiFontRange(std::vector<WCharType> range)
{
  static constexpr size_t builtin_size = std::size(EMOJI_ICON_RANGE);
  const size_t runtime_size = range.size();

  if (runtime_size == 0)
  {
    if (s_emoji_range.empty())
      return;

    s_emoji_range = {};
  }
  else
  {
    if (!s_emoji_range.empty() && (s_emoji_range.size() - builtin_size) == range.size() &&
        std::memcmp(s_emoji_range.data(), range.data(), range.size() * sizeof(ImWchar)) == 0)
    {
      // no change
      return;
    }

    s_emoji_range = std::move(range);
    s_emoji_range.resize(s_emoji_range.size() + builtin_size);
    std::memcpy(&s_emoji_range[runtime_size], EMOJI_ICON_RANGE, sizeof(EMOJI_ICON_RANGE));
  }

  ReloadFontDataIfActive();
}

std::vector<ImGuiManager::WCharType> ImGuiManager::CompactFontRange(std::span<const WCharType> range)
{
  std::vector<ImWchar> ret;

  for (auto it = range.begin(); it != range.end();)
  {
    auto next_it = it;
    ++next_it;

    // Combine sequential ranges.
    const ImWchar start_codepoint = *it;
    ImWchar end_codepoint = start_codepoint;
    while (next_it != range.end())
    {
      const ImWchar next_codepoint = *next_it;
      if (next_codepoint != (end_codepoint + 1))
        break;

      // Yep, include it.
      end_codepoint = next_codepoint;
      ++next_it;
    }

    ret.push_back(start_codepoint);
    ret.push_back(end_codepoint);

    it = next_it;
  }

  return ret;
}

void ImGuiManager::SetGlobalScale(float global_scale)
{
  if (s_global_prescale == global_scale)
    return;

  s_global_prescale = global_scale;
  s_scale_changed = true;
}

bool ImGuiManager::IsShowingOSDMessages()
{
  return s_show_osd_messages;
}

void ImGuiManager::SetShowOSDMessages(bool enable)
{
  if (s_show_osd_messages == enable)
    return;

  s_show_osd_messages = enable;
  if (!enable)
    Host::ClearOSDMessages(false);
}

bool ImGuiManager::Initialize(float global_scale, Error* error)
{
  if (!LoadFontData())
  {
    Error::SetString(error, "Failed to load font data");
    return false;
  }

  s_global_prescale = global_scale;
  s_global_scale = std::max(g_gpu_device->GetWindowScale() * global_scale, 1.0f);
  s_scale_changed = false;

  ImGui::CreateContext();

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad | ImGuiBackendFlags_RendererHasVtxOffset;
  io.BackendUsingLegacyKeyArrays = 0;
  io.BackendUsingLegacyNavInputArray = 0;
  io.KeyRepeatDelay = 0.5f;
#ifndef __ANDROID__
  // Android has no keyboard, nor are we using ImGui for any actual user-interactable windows.
  io.ConfigFlags |=
    ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NoMouseCursorChange;
#else
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
#endif

  s_window_width = static_cast<float>(g_gpu_device->GetWindowWidth());
  s_window_height = static_cast<float>(g_gpu_device->GetWindowHeight());
  io.DisplayFramebufferScale = ImVec2(1, 1); // We already scale things ourselves, this would double-apply scaling
  io.DisplaySize = ImVec2(s_window_width, s_window_height);

  SetKeyMap();
  SetStyle();

  if (!AddImGuiFonts(false) || !g_gpu_device->UpdateImGuiFontTexture())
  {
    Error::SetString(error, "Failed to create ImGui font text");
    ImGui::DestroyContext();
    return false;
  }

  // don't need the font data anymore, save some memory
  ImGui::GetIO().Fonts->ClearTexData();

  NewFrame();

  CreateSoftwareCursorTextures();
  return true;
}

void ImGuiManager::Shutdown()
{
  DestroySoftwareCursorTextures();

  if (ImGui::GetCurrentContext())
    ImGui::DestroyContext();

  s_standard_font = nullptr;
  s_fixed_font = nullptr;
  s_medium_font = nullptr;
  s_large_font = nullptr;
  ImGuiFullscreen::SetFonts(nullptr, nullptr);
}

float ImGuiManager::GetWindowWidth()
{
  return s_window_width;
}

float ImGuiManager::GetWindowHeight()
{
  return s_window_height;
}

void ImGuiManager::WindowResized(float width, float height)
{
  s_window_width = width;
  s_window_height = height;
  ImGui::GetIO().DisplaySize = ImVec2(width, height);

  // Scale might have changed as a result of window resize.
  RequestScaleUpdate();
}

void ImGuiManager::RequestScaleUpdate()
{
  // Might need to update the scale.
  s_scale_changed = true;
}

void ImGuiManager::UpdateScale()
{
  const float window_scale = g_gpu_device ? g_gpu_device->GetWindowScale() : 1.0f;
  const float scale = std::max(window_scale * s_global_prescale, 1.0f);

  if ((!HasFullscreenFonts() || !ImGuiFullscreen::UpdateLayoutScale()) && scale == s_global_scale)
    return;

  s_global_scale = scale;

  ImGui::GetStyle() = ImGuiStyle();
  ImGui::GetStyle().WindowMinSize = ImVec2(1.0f, 1.0f);
  SetStyle();
  ImGui::GetStyle().ScaleAllSizes(scale);

  if (!AddImGuiFonts(HasFullscreenFonts()))
    Panic("Failed to create ImGui font text");

  if (!g_gpu_device->UpdateImGuiFontTexture())
    Panic("Failed to recreate font texture after scale+resize");
}

void ImGuiManager::NewFrame()
{
  ImGuiIO& io = ImGui::GetIO();
  io.DeltaTime = static_cast<float>(s_last_render_time.GetTimeSecondsAndReset());

  if (s_scale_changed)
  {
    s_scale_changed = false;
    UpdateScale();
  }

  ImGui::NewFrame();

  // Disable nav input on the implicit (Debug##Default) window. Otherwise we end up requesting keyboard
  // focus when there's nothing there. We use GetCurrentWindowRead() because otherwise it'll make it visible.
  ImGui::GetCurrentWindowRead()->Flags |= ImGuiWindowFlags_NoNavInputs;
  s_imgui_wants_keyboard.store(io.WantCaptureKeyboard, std::memory_order_relaxed);
  s_imgui_wants_mouse.store(io.WantCaptureMouse, std::memory_order_release);
}

void ImGuiManager::SetStyle()
{
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();
  style.WindowMinSize = ImVec2(1.0f, 1.0f);

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.36f, 0.42f, 0.47f, 1.00f);
  colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_ChildBg] = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
  colors[ImGuiCol_Border] = ImVec4(0.08f, 0.10f, 0.12f, 1.00f);
  colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.20f, 0.28f, 1.00f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.09f, 0.12f, 0.14f, 1.00f);
  colors[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.12f, 0.14f, 0.65f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.10f, 0.12f, 1.00f);
  colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
  colors[ImGuiCol_MenuBarBg] = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.39f);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
  colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.18f, 0.22f, 0.25f, 1.00f);
  colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.09f, 0.21f, 0.31f, 1.00f);
  colors[ImGuiCol_CheckMark] = ImVec4(0.28f, 0.56f, 1.00f, 1.00f);
  colors[ImGuiCol_SliderGrab] = ImVec4(0.28f, 0.56f, 1.00f, 1.00f);
  colors[ImGuiCol_SliderGrabActive] = ImVec4(0.37f, 0.61f, 1.00f, 1.00f);
  colors[ImGuiCol_Button] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_Header] = ImVec4(0.20f, 0.25f, 0.29f, 0.55f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
  colors[ImGuiCol_SeparatorHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_SeparatorActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.59f, 0.98f, 0.25f);
  colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_ResizeGripActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_Tab] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_TabHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_TabActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_TabUnfocused] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
  colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
  colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
  colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
  colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
  colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
  colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
  colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
  colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

  style.ScaleAllSizes(s_global_scale);
}

void ImGuiManager::SetKeyMap()
{
  struct KeyMapping
  {
    ImGuiKey index;
    const char* name;
    const char* alt_name;
  };

  static constexpr KeyMapping mapping[] = {{ImGuiKey_LeftArrow, "Left", nullptr},
                                           {ImGuiKey_RightArrow, "Right", nullptr},
                                           {ImGuiKey_UpArrow, "Up", nullptr},
                                           {ImGuiKey_DownArrow, "Down", nullptr},
                                           {ImGuiKey_PageUp, "PageUp", nullptr},
                                           {ImGuiKey_PageDown, "PageDown", nullptr},
                                           {ImGuiKey_Home, "Home", nullptr},
                                           {ImGuiKey_End, "End", nullptr},
                                           {ImGuiKey_Insert, "Insert", nullptr},
                                           {ImGuiKey_Delete, "Delete", nullptr},
                                           {ImGuiKey_Backspace, "Backspace", nullptr},
                                           {ImGuiKey_Space, "Space", nullptr},
                                           {ImGuiKey_Enter, "Return", nullptr},
                                           {ImGuiKey_Escape, "Escape", nullptr},
                                           {ImGuiKey_LeftCtrl, "LeftCtrl", "Ctrl"},
                                           {ImGuiKey_LeftShift, "LeftShift", "Shift"},
                                           {ImGuiKey_LeftAlt, "LeftAlt", "Alt"},
                                           {ImGuiKey_LeftSuper, "LeftSuper", "Super"},
                                           {ImGuiKey_RightCtrl, "RightCtrl", nullptr},
                                           {ImGuiKey_RightShift, "RightShift", nullptr},
                                           {ImGuiKey_RightAlt, "RightAlt", nullptr},
                                           {ImGuiKey_RightSuper, "RightSuper", nullptr},
                                           {ImGuiKey_Menu, "Menu", nullptr},
                                           {ImGuiKey_0, "0", nullptr},
                                           {ImGuiKey_1, "1", nullptr},
                                           {ImGuiKey_2, "2", nullptr},
                                           {ImGuiKey_3, "3", nullptr},
                                           {ImGuiKey_4, "4", nullptr},
                                           {ImGuiKey_5, "5", nullptr},
                                           {ImGuiKey_6, "6", nullptr},
                                           {ImGuiKey_7, "7", nullptr},
                                           {ImGuiKey_8, "8", nullptr},
                                           {ImGuiKey_9, "9", nullptr},
                                           {ImGuiKey_A, "A", nullptr},
                                           {ImGuiKey_B, "B", nullptr},
                                           {ImGuiKey_C, "C", nullptr},
                                           {ImGuiKey_D, "D", nullptr},
                                           {ImGuiKey_E, "E", nullptr},
                                           {ImGuiKey_F, "F", nullptr},
                                           {ImGuiKey_G, "G", nullptr},
                                           {ImGuiKey_H, "H", nullptr},
                                           {ImGuiKey_I, "I", nullptr},
                                           {ImGuiKey_J, "J", nullptr},
                                           {ImGuiKey_K, "K", nullptr},
                                           {ImGuiKey_L, "L", nullptr},
                                           {ImGuiKey_M, "M", nullptr},
                                           {ImGuiKey_N, "N", nullptr},
                                           {ImGuiKey_O, "O", nullptr},
                                           {ImGuiKey_P, "P", nullptr},
                                           {ImGuiKey_Q, "Q", nullptr},
                                           {ImGuiKey_R, "R", nullptr},
                                           {ImGuiKey_S, "S", nullptr},
                                           {ImGuiKey_T, "T", nullptr},
                                           {ImGuiKey_U, "U", nullptr},
                                           {ImGuiKey_V, "V", nullptr},
                                           {ImGuiKey_W, "W", nullptr},
                                           {ImGuiKey_X, "X", nullptr},
                                           {ImGuiKey_Y, "Y", nullptr},
                                           {ImGuiKey_Z, "Z", nullptr},
                                           {ImGuiKey_F1, "F1", nullptr},
                                           {ImGuiKey_F2, "F2", nullptr},
                                           {ImGuiKey_F3, "F3", nullptr},
                                           {ImGuiKey_F4, "F4", nullptr},
                                           {ImGuiKey_F5, "F5", nullptr},
                                           {ImGuiKey_F6, "F6", nullptr},
                                           {ImGuiKey_F7, "F7", nullptr},
                                           {ImGuiKey_F8, "F8", nullptr},
                                           {ImGuiKey_F9, "F9", nullptr},
                                           {ImGuiKey_F10, "F10", nullptr},
                                           {ImGuiKey_F11, "F11", nullptr},
                                           {ImGuiKey_F12, "F12", nullptr},
                                           {ImGuiKey_Apostrophe, "Apostrophe", nullptr},
                                           {ImGuiKey_Comma, "Comma", nullptr},
                                           {ImGuiKey_Minus, "Minus", nullptr},
                                           {ImGuiKey_Period, "Period", nullptr},
                                           {ImGuiKey_Slash, "Slash", nullptr},
                                           {ImGuiKey_Semicolon, "Semicolon", nullptr},
                                           {ImGuiKey_Equal, "Equal", nullptr},
                                           {ImGuiKey_LeftBracket, "BracketLeft", nullptr},
                                           {ImGuiKey_Backslash, "Backslash", nullptr},
                                           {ImGuiKey_RightBracket, "BracketRight", nullptr},
                                           {ImGuiKey_GraveAccent, "QuoteLeft", nullptr},
                                           {ImGuiKey_CapsLock, "CapsLock", nullptr},
                                           {ImGuiKey_ScrollLock, "ScrollLock", nullptr},
                                           {ImGuiKey_NumLock, "NumLock", nullptr},
                                           {ImGuiKey_PrintScreen, "PrintScreen", nullptr},
                                           {ImGuiKey_Pause, "Pause", nullptr},
                                           {ImGuiKey_Keypad0, "Keypad0", nullptr},
                                           {ImGuiKey_Keypad1, "Keypad1", nullptr},
                                           {ImGuiKey_Keypad2, "Keypad2", nullptr},
                                           {ImGuiKey_Keypad3, "Keypad3", nullptr},
                                           {ImGuiKey_Keypad4, "Keypad4", nullptr},
                                           {ImGuiKey_Keypad5, "Keypad5", nullptr},
                                           {ImGuiKey_Keypad6, "Keypad6", nullptr},
                                           {ImGuiKey_Keypad7, "Keypad7", nullptr},
                                           {ImGuiKey_Keypad8, "Keypad8", nullptr},
                                           {ImGuiKey_Keypad9, "Keypad9", nullptr},
                                           {ImGuiKey_KeypadDecimal, "KeypadPeriod", nullptr},
                                           {ImGuiKey_KeypadDivide, "KeypadDivide", nullptr},
                                           {ImGuiKey_KeypadMultiply, "KeypadMultiply", nullptr},
                                           {ImGuiKey_KeypadSubtract, "KeypadMinus", nullptr},
                                           {ImGuiKey_KeypadAdd, "KeypadPlus", nullptr},
                                           {ImGuiKey_KeypadEnter, "KeypadReturn", nullptr},
                                           {ImGuiKey_KeypadEqual, "KeypadEqual", nullptr}};

  s_imgui_key_map.clear();
  for (const KeyMapping& km : mapping)
  {
    std::optional<u32> map(InputManager::ConvertHostKeyboardStringToCode(km.name));
    if (!map.has_value() && km.alt_name)
      map = InputManager::ConvertHostKeyboardStringToCode(km.alt_name);
    if (map.has_value())
      s_imgui_key_map[map.value()] = km.index;
  }
}

bool ImGuiManager::LoadFontData()
{
  if (s_standard_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data = s_font_path.empty() ?
                                                      Host::ReadResourceFile("fonts/Roboto-Regular.ttf", true) :
                                                      FileSystem::ReadBinaryFile(s_font_path.c_str());
    if (!font_data.has_value())
      return false;

    s_standard_font_data = std::move(font_data.value());
  }

  if (s_fixed_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data = Host::ReadResourceFile("fonts/RobotoMono-Medium.ttf", true);
    if (!font_data.has_value())
      return false;

    s_fixed_font_data = std::move(font_data.value());
  }

  if (s_icon_fa_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data = Host::ReadResourceFile("fonts/fa-solid-900.ttf", true);
    if (!font_data.has_value())
      return false;

    s_icon_fa_font_data = std::move(font_data.value());
  }

  if (s_icon_pf_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data = Host::ReadResourceFile("fonts/promptfont.otf", true);
    if (!font_data.has_value())
      return false;

    s_icon_pf_font_data = std::move(font_data.value());
  }

  if (s_emoji_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data =
      Host::ReadCompressedResourceFile("fonts/TwitterColorEmoji-SVGinOT.ttf.zst", true);
    if (!font_data.has_value())
      return false;

    s_emoji_font_data = std::move(font_data.value());
  }

  return true;
}

ImFont* ImGuiManager::AddTextFont(float size, bool full_glyph_range)
{
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(s_standard_font_data.data(),
                                                    static_cast<int>(s_standard_font_data.size()), size, &cfg,
                                                    full_glyph_range ? s_font_range.data() : s_ascii_font_range.data());
}

ImFont* ImGuiManager::AddFixedFont(float size)
{
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
    s_fixed_font_data.data(), static_cast<int>(s_fixed_font_data.size()), size, &cfg, s_ascii_font_range.data());
}

bool ImGuiManager::AddIconFonts(float size)
{
  {
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = size;
    cfg.GlyphMaxAdvanceX = size;
    cfg.FontDataOwnedByAtlas = false;

    if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
          s_icon_fa_font_data.data(), static_cast<int>(s_icon_fa_font_data.size()), size * 0.75f, &cfg, FA_ICON_RANGE))
      [[unlikely]]
    {
      return false;
    }
  }

  {
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = size;
    cfg.GlyphMaxAdvanceX = size;
    cfg.FontDataOwnedByAtlas = false;

    if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
          s_icon_pf_font_data.data(), static_cast<int>(s_icon_pf_font_data.size()), size * 1.2f, &cfg, PF_ICON_RANGE))
      [[unlikely]]
    {
      return false;
    }
  }

  {
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = size;
    cfg.GlyphMaxAdvanceX = size;
    cfg.FontDataOwnedByAtlas = false;
    cfg.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LoadColor | ImGuiFreeTypeBuilderFlags_Bitmap;

    if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
          s_emoji_font_data.data(), static_cast<int>(s_emoji_font_data.size()), size * 0.9f, &cfg,
          s_emoji_range.empty() ? EMOJI_ICON_RANGE : s_emoji_range.data())) [[unlikely]]
    {
      return false;
    }
  }

  return true;
}

bool ImGuiManager::AddImGuiFonts(bool fullscreen_fonts)
{
  const float standard_font_size = std::ceil(15.0f * s_global_scale);
  const float osd_font_size = std::ceil(17.0f * s_global_scale);

  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();

  s_standard_font = AddTextFont(standard_font_size, false);
  if (!s_standard_font)
    return false;

  s_fixed_font = AddFixedFont(standard_font_size);
  if (!s_fixed_font)
    return false;

  s_osd_font = AddTextFont(osd_font_size, true);
  if (!s_osd_font || !AddIconFonts(osd_font_size))
    return false;

  if (fullscreen_fonts)
  {
    const float medium_font_size = ImGuiFullscreen::LayoutScale(ImGuiFullscreen::LAYOUT_MEDIUM_FONT_SIZE);
    s_medium_font = AddTextFont(medium_font_size, true);
    if (!s_medium_font || !AddIconFonts(medium_font_size))
      return false;

    const float large_font_size = ImGuiFullscreen::LayoutScale(ImGuiFullscreen::LAYOUT_LARGE_FONT_SIZE);
    s_large_font = AddTextFont(large_font_size, true);
    if (!s_large_font || !AddIconFonts(large_font_size))
      return false;
  }
  else
  {
    s_medium_font = nullptr;
    s_large_font = nullptr;
  }

  ImGuiFullscreen::SetFonts(s_medium_font, s_large_font);

  return io.Fonts->Build();
}

void ImGuiManager::ReloadFontDataIfActive()
{
  if (!ImGui::GetCurrentContext())
    return;

  ImGui::EndFrame();

  if (!LoadFontData())
    Panic("Failed to load font data");

  if (!AddImGuiFonts(HasFullscreenFonts()))
    Panic("Failed to create ImGui font text");

  if (!g_gpu_device->UpdateImGuiFontTexture())
    Panic("Failed to recreate font texture after scale+resize");

  NewFrame();
}

bool ImGuiManager::AddFullscreenFontsIfMissing()
{
  if (HasFullscreenFonts())
    return true;

  // can't do this in the middle of a frame
  ImGui::EndFrame();

  if (!AddImGuiFonts(true))
  {
    ERROR_LOG("Failed to lazily allocate fullscreen fonts.");
    AddImGuiFonts(false);
  }

  g_gpu_device->UpdateImGuiFontTexture();
  NewFrame();

  return HasFullscreenFonts();
}

bool ImGuiManager::HasFullscreenFonts()
{
  return (s_medium_font && s_large_font);
}

void ImGuiManager::AddOSDMessage(std::string key, std::string message, float duration, bool is_warning)
{
  if (!key.empty())
    INFO_LOG("OSD [{}]: {}", key, message);
  else
    INFO_LOG("OSD: {}", message);

  if (!s_show_osd_messages && !is_warning)
    return;

  const Common::Timer::Value current_time = Common::Timer::GetCurrentValue();

  OSDMessage msg;
  msg.key = std::move(key);
  msg.text = std::move(message);
  msg.duration = duration;
  msg.start_time = current_time;
  msg.move_time = current_time;
  msg.target_y = -1.0f;
  msg.last_y = -1.0f;
  msg.is_warning = is_warning;

  std::unique_lock<std::mutex> lock(s_osd_messages_lock);
  s_osd_posted_messages.push_back(std::move(msg));
}

void ImGuiManager::RemoveKeyedOSDMessage(std::string key, bool is_warning)
{
  if (!s_show_osd_messages && !is_warning)
    return;

  ImGuiManager::OSDMessage msg = {};
  msg.key = std::move(key);
  msg.duration = 0.0f;
  msg.is_warning = is_warning;

  std::unique_lock<std::mutex> lock(s_osd_messages_lock);
  s_osd_posted_messages.push_back(std::move(msg));
}

void ImGuiManager::ClearOSDMessages(bool clear_warnings)
{
  {
    std::unique_lock<std::mutex> lock(s_osd_messages_lock);
    if (clear_warnings)
    {
      s_osd_posted_messages.clear();
    }
    else
    {
      for (auto iter = s_osd_posted_messages.begin(); iter != s_osd_posted_messages.end();)
      {
        if (!iter->is_warning)
          iter = s_osd_posted_messages.erase(iter);
        else
          ++iter;
      }
    }
  }

  if (clear_warnings)
  {
    s_osd_active_messages.clear();
  }
  else
  {
    for (auto iter = s_osd_active_messages.begin(); iter != s_osd_active_messages.end();)
    {
      if (!iter->is_warning)
        s_osd_active_messages.erase(iter);
      else
        ++iter;
    }
  }
}

void ImGuiManager::AcquirePendingOSDMessages(Common::Timer::Value current_time)
{
  std::atomic_thread_fence(std::memory_order_consume);
  if (s_osd_posted_messages.empty())
    return;

  std::unique_lock lock(s_osd_messages_lock);
  for (;;)
  {
    if (s_osd_posted_messages.empty())
      break;

    OSDMessage& new_msg = s_osd_posted_messages.front();
    std::deque<OSDMessage>::iterator iter;
    if (!new_msg.key.empty() && (iter = std::find_if(s_osd_active_messages.begin(), s_osd_active_messages.end(),
                                                     [&new_msg](const OSDMessage& other) {
                                                       return new_msg.key == other.key;
                                                     })) != s_osd_active_messages.end())
    {
      iter->text = std::move(new_msg.text);
      iter->duration = new_msg.duration;

      // Don't fade it in again
      const float time_passed =
        static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - iter->start_time));
      iter->start_time = current_time - Common::Timer::ConvertSecondsToValue(std::min(time_passed, OSD_FADE_IN_TIME));
    }
    else
    {
      s_osd_active_messages.push_back(std::move(new_msg));
    }

    s_osd_posted_messages.pop_front();

    static constexpr size_t MAX_ACTIVE_OSD_MESSAGES = 512;
    if (s_osd_active_messages.size() > MAX_ACTIVE_OSD_MESSAGES)
      s_osd_active_messages.pop_front();
  }
}

void ImGuiManager::DrawOSDMessages(Common::Timer::Value current_time)
{
  static constexpr float MOVE_DURATION = 0.5f;

  ImFont* const font = s_osd_font;
  const float scale = s_global_scale;
  const float spacing = std::ceil(6.0f * scale);
  const float margin = std::ceil(11.0f * scale);
  const float padding = std::ceil(9.0f * scale);
  const float rounding = std::ceil(6.0f * scale);
  const float max_width = s_window_width - (margin + padding) * 2.0f;
  float position_x = margin;
  float position_y = margin;

  auto iter = s_osd_active_messages.begin();
  while (iter != s_osd_active_messages.end())
  {
    OSDMessage& msg = *iter;
    const float time_passed = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - msg.start_time));
    if (time_passed >= msg.duration)
    {
      iter = s_osd_active_messages.erase(iter);
      continue;
    }

    ++iter;

    u8 opacity;
    if (time_passed < OSD_FADE_IN_TIME)
      opacity = static_cast<u8>((time_passed / OSD_FADE_IN_TIME) * 255.0f);
    else if (time_passed > (msg.duration - OSD_FADE_OUT_TIME))
      opacity = static_cast<u8>(std::min((msg.duration - time_passed) / OSD_FADE_OUT_TIME, 1.0f) * 255.0f);
    else
      opacity = 255;

    const float expected_y = position_y;
    float actual_y = msg.last_y;
    if (msg.target_y != expected_y)
    {
      if (msg.last_y < 0.0f)
      {
        // First showing.
        msg.last_y = expected_y;
      }
      else
      {
        // We got repositioned, probably due to another message above getting removed.
        const float time_since_move =
          static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - msg.move_time));
        const float frac = Easing::OutExpo(time_since_move / MOVE_DURATION);
        msg.last_y = std::floor(msg.last_y - ((msg.last_y - msg.target_y) * frac));
      }

      msg.move_time = current_time;
      msg.target_y = expected_y;
      actual_y = msg.last_y;
    }
    else if (actual_y != expected_y)
    {
      const float time_since_move =
        static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - msg.move_time));
      if (time_since_move >= MOVE_DURATION)
      {
        msg.move_time = current_time;
        msg.last_y = msg.target_y;
        actual_y = msg.last_y;
      }
      else
      {
        const float frac = Easing::OutExpo(time_since_move / MOVE_DURATION);
        actual_y = std::floor(msg.last_y - ((msg.last_y - msg.target_y) * frac));
      }
    }

    if (actual_y >= ImGui::GetIO().DisplaySize.y)
      break;

    const ImVec2 pos(position_x, actual_y);
    const ImVec2 text_size(font->CalcTextSizeA(font->FontSize, max_width, max_width, msg.text.c_str(),
                                               msg.text.c_str() + msg.text.length()));
    const ImVec2 size(text_size.x + padding * 2.0f, text_size.y + padding * 2.0f);
    const ImVec4 text_rect(pos.x + padding, pos.y + padding, pos.x + size.x - padding, pos.y + size.y - padding);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0x21, 0x21, 0x21, opacity), rounding);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0x48, 0x48, 0x48, opacity), rounding);
    dl->AddText(font, font->FontSize, ImVec2(text_rect.x, text_rect.y), IM_COL32(0xff, 0xff, 0xff, opacity),
                msg.text.c_str(), msg.text.c_str() + msg.text.length(), max_width, &text_rect);
    position_y += size.y + spacing;
  }
}

void ImGuiManager::RenderOSDMessages()
{
  const Common::Timer::Value current_time = Common::Timer::GetCurrentValue();
  AcquirePendingOSDMessages(current_time);
  DrawOSDMessages(current_time);
}

void Host::AddOSDMessage(std::string message, float duration /*= 2.0f*/)
{
  ImGuiManager::AddOSDMessage(std::string(), std::move(message), duration, false);
}

void Host::AddKeyedOSDMessage(std::string key, std::string message, float duration /* = 2.0f */)
{
  ImGuiManager::AddOSDMessage(std::move(key), std::move(message), duration, false);
}

void Host::AddIconOSDMessage(std::string key, const char* icon, std::string message, float duration /* = 2.0f */)
{
  ImGuiManager::AddOSDMessage(std::move(key), fmt::format("{}  {}", icon, message), duration, false);
}

void Host::AddKeyedOSDWarning(std::string key, std::string message, float duration /* = 2.0f */)
{
  ImGuiManager::AddOSDMessage(std::move(key), std::move(message), duration, true);
}

void Host::AddIconOSDWarning(std::string key, const char* icon, std::string message, float duration /* = 2.0f */)
{
  ImGuiManager::AddOSDMessage(std::move(key), fmt::format("{}  {}", icon, message), duration, true);
}

void Host::RemoveKeyedOSDMessage(std::string key)
{
  ImGuiManager::RemoveKeyedOSDMessage(std::move(key), false);
}

void Host::RemoveKeyedOSDWarning(std::string key)
{
  ImGuiManager::RemoveKeyedOSDMessage(std::move(key), true);
}

void Host::ClearOSDMessages(bool clear_warnings)
{
  ImGuiManager::ClearOSDMessages(clear_warnings);
}

float ImGuiManager::GetGlobalScale()
{
  return s_global_scale;
}

ImFont* ImGuiManager::GetStandardFont()
{
  return s_standard_font;
}

ImFont* ImGuiManager::GetOSDFont()
{
  return s_osd_font;
}

ImFont* ImGuiManager::GetFixedFont()
{
  return s_fixed_font;
}

ImFont* ImGuiManager::GetMediumFont()
{
  AddFullscreenFontsIfMissing();
  return s_medium_font;
}

ImFont* ImGuiManager::GetLargeFont()
{
  AddFullscreenFontsIfMissing();
  return s_large_font;
}

bool ImGuiManager::WantsTextInput()
{
  return s_imgui_wants_keyboard.load(std::memory_order_acquire);
}

bool ImGuiManager::WantsMouseInput()
{
  return s_imgui_wants_mouse.load(std::memory_order_acquire);
}

void ImGuiManager::AddTextInput(std::string str)
{
  if (!ImGui::GetCurrentContext())
    return;

  if (!s_imgui_wants_keyboard.load(std::memory_order_acquire))
    return;

  ImGui::GetIO().AddInputCharactersUTF8(str.c_str());
}

void ImGuiManager::UpdateMousePosition(float x, float y)
{
  if (!ImGui::GetCurrentContext())
    return;

  ImGui::GetIO().MousePos = ImVec2(x, y);
  std::atomic_thread_fence(std::memory_order_release);
}

bool ImGuiManager::ProcessPointerButtonEvent(InputBindingKey key, float value)
{
  if (!ImGui::GetCurrentContext() || key.data >= std::size(ImGui::GetIO().MouseDown))
    return false;

  // still update state anyway
  ImGui::GetIO().AddMouseButtonEvent(key.data, value != 0.0f);

  return s_imgui_wants_mouse.load(std::memory_order_acquire);
}

bool ImGuiManager::ProcessPointerAxisEvent(InputBindingKey key, float value)
{
  if (!ImGui::GetCurrentContext() || key.data < static_cast<u32>(InputPointerAxis::WheelX))
    return false;

  // still update state anyway
  const bool horizontal = (key.data == static_cast<u32>(InputPointerAxis::WheelX));
  ImGui::GetIO().AddMouseWheelEvent(horizontal ? value : 0.0f, horizontal ? 0.0f : value);

  return s_imgui_wants_mouse.load(std::memory_order_acquire);
}

bool ImGuiManager::ProcessHostKeyEvent(InputBindingKey key, float value)
{
  decltype(s_imgui_key_map)::iterator iter;
  if (!ImGui::GetCurrentContext() || (iter = s_imgui_key_map.find(key.data)) == s_imgui_key_map.end())
    return false;

  // still update state anyway
  ImGui::GetIO().AddKeyEvent(iter->second, value != 0.0);

  return s_imgui_wants_keyboard.load(std::memory_order_acquire);
}

bool ImGuiManager::ProcessGenericInputEvent(GenericInputBinding key, float value)
{
  static constexpr ImGuiKey key_map[] = {
    ImGuiKey_None,             // Unknown,
    ImGuiKey_GamepadDpadUp,    // DPadUp
    ImGuiKey_GamepadDpadRight, // DPadRight
    ImGuiKey_GamepadDpadLeft,  // DPadLeft
    ImGuiKey_GamepadDpadDown,  // DPadDown
    ImGuiKey_None,             // LeftStickUp
    ImGuiKey_None,             // LeftStickRight
    ImGuiKey_None,             // LeftStickDown
    ImGuiKey_None,             // LeftStickLeft
    ImGuiKey_GamepadL3,        // L3
    ImGuiKey_None,             // RightStickUp
    ImGuiKey_None,             // RightStickRight
    ImGuiKey_None,             // RightStickDown
    ImGuiKey_None,             // RightStickLeft
    ImGuiKey_GamepadR3,        // R3
    ImGuiKey_GamepadFaceUp,    // Triangle
    ImGuiKey_GamepadFaceRight, // Circle
    ImGuiKey_GamepadFaceDown,  // Cross
    ImGuiKey_GamepadFaceLeft,  // Square
    ImGuiKey_GamepadBack,      // Select
    ImGuiKey_GamepadStart,     // Start
    ImGuiKey_None,             // System
    ImGuiKey_GamepadL1,        // L1
    ImGuiKey_GamepadL2,        // L2
    ImGuiKey_GamepadR1,        // R1
    ImGuiKey_GamepadL2,        // R2
  };

  if (!ImGui::GetCurrentContext())
    return false;

  if (static_cast<u32>(key) >= std::size(key_map) || key_map[static_cast<u32>(key)] == ImGuiKey_None)
    return false;

  ImGui::GetIO().AddKeyAnalogEvent(key_map[static_cast<u32>(key)], (value > 0.0f), value);
  return s_imgui_wants_keyboard.load(std::memory_order_acquire);
}

void ImGuiManager::CreateSoftwareCursorTextures()
{
  for (u32 i = 0; i < static_cast<u32>(s_software_cursors.size()); i++)
  {
    if (!s_software_cursors[i].image_path.empty())
      UpdateSoftwareCursorTexture(i);
  }
}

void ImGuiManager::DestroySoftwareCursorTextures()
{
  for (SoftwareCursor& sc : s_software_cursors)
    sc.texture.reset();
}

void ImGuiManager::UpdateSoftwareCursorTexture(u32 index)
{
  SoftwareCursor& sc = s_software_cursors[index];
  if (sc.image_path.empty())
  {
    sc.texture.reset();
    return;
  }

  RGBA8Image image;
  if (!image.LoadFromFile(sc.image_path.c_str()))
  {
    ERROR_LOG("Failed to load software cursor {} image '{}'", index, sc.image_path);
    return;
  }
  g_gpu_device->RecycleTexture(std::move(sc.texture));
  sc.texture = g_gpu_device->FetchTexture(image.GetWidth(), image.GetHeight(), 1, 1, 1, GPUTexture::Type::Texture,
                                          GPUTexture::Format::RGBA8, image.GetPixels(), image.GetPitch());
  if (!sc.texture)
  {
    ERROR_LOG("Failed to upload {}x{} software cursor {} image '{}'", image.GetWidth(), image.GetHeight(), index,
              sc.image_path);
    return;
  }

  sc.extent_x = std::ceil(static_cast<float>(image.GetWidth()) * sc.scale * s_global_scale) / 2.0f;
  sc.extent_y = std::ceil(static_cast<float>(image.GetHeight()) * sc.scale * s_global_scale) / 2.0f;
}

void ImGuiManager::DrawSoftwareCursor(const SoftwareCursor& sc, const std::pair<float, float>& pos)
{
  if (!sc.texture)
    return;

  const ImVec2 min(pos.first - sc.extent_x, pos.second - sc.extent_y);
  const ImVec2 max(pos.first + sc.extent_x, pos.second + sc.extent_y);

  ImDrawList* dl = ImGui::GetForegroundDrawList();

  dl->AddImage(reinterpret_cast<ImTextureID>(sc.texture.get()), min, max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
               sc.color);
}

void ImGuiManager::RenderSoftwareCursors()
{
  // This one's okay to race, worst that happens is we render the wrong number of cursors for a frame.
  const u32 pointer_count = InputManager::GetPointerCount();
  for (u32 i = 0; i < pointer_count; i++)
    DrawSoftwareCursor(s_software_cursors[i], InputManager::GetPointerAbsolutePosition(i));

  for (u32 i = InputManager::MAX_POINTER_DEVICES; i < InputManager::MAX_SOFTWARE_CURSORS; i++)
    DrawSoftwareCursor(s_software_cursors[i], s_software_cursors[i].pos);
}

void ImGuiManager::SetSoftwareCursor(u32 index, std::string image_path, float image_scale, u32 multiply_color)
{
  DebugAssert(index < std::size(s_software_cursors));
  SoftwareCursor& sc = s_software_cursors[index];
  sc.color = multiply_color | 0xFF000000;
  if (sc.image_path == image_path && sc.scale == image_scale)
    return;

  const bool is_hiding_or_showing = (image_path.empty() != sc.image_path.empty());
  sc.image_path = std::move(image_path);
  sc.scale = image_scale;
  if (g_gpu_device)
    UpdateSoftwareCursorTexture(index);

  // Hide the system cursor when we activate a software cursor.
  if (is_hiding_or_showing && index <= InputManager::MAX_POINTER_DEVICES)
    InputManager::UpdateRelativeMouseMode();
}

bool ImGuiManager::HasSoftwareCursor(u32 index)
{
  return (index < s_software_cursors.size() && !s_software_cursors[index].image_path.empty());
}

void ImGuiManager::ClearSoftwareCursor(u32 index)
{
  SetSoftwareCursor(index, std::string(), 0.0f, 0);
}

void ImGuiManager::SetSoftwareCursorPosition(u32 index, float pos_x, float pos_y)
{
  DebugAssert(index >= InputManager::MAX_POINTER_DEVICES);
  SoftwareCursor& sc = s_software_cursors[index];
  sc.pos.first = pos_x;
  sc.pos.second = pos_y;
}

std::string ImGuiManager::StripIconCharacters(std::string_view str)
{
  std::string result;
  result.reserve(str.length());

  for (size_t offset = 0; offset < str.length();)
  {
    char32_t utf;
    offset += StringUtil::DecodeUTF8(str, offset, &utf);

    // icon if outside BMP/SMP/TIP, or inside private use area
    if (utf > 0x32FFF || (utf >= 0xE000 && utf <= 0xF8FF))
      continue;

    StringUtil::EncodeAndAppendUTF8(result, utf);
  }

  StringUtil::StripWhitespace(&result);

  return result;
}
