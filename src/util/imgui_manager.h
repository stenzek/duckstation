// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/gpu_texture.h"

#include "common/types.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

class Error;
struct WindowInfo;

class GPUSwapChain;
class GPUTexture;

struct ImGuiContext;
struct ImFont;

union InputBindingKey;
enum class GenericInputBinding : u8;

namespace Host {

/// Handle representing an auxiliary render window in the host.
using AuxiliaryRenderWindowHandle = void*;
using AuxiliaryRenderWindowUserData = void*;

enum class AuxiliaryRenderWindowEvent : u8
{
  CloseRequest,
  Resized,
  KeyPressed,
  KeyReleased,
  TextEntered,
  MouseMoved,
  MousePressed,
  MouseReleased,
  MouseWheel,
  MaxCount,
};

union AuxiliaryRenderWindowEventParam
{
  s32 int_param;
  u32 uint_param;
  float float_param;
};

} // namespace Host

namespace ImGuiManager {

using WCharType = u32;

/// Default size for screen margins.
#ifndef __ANDROID__
static constexpr float DEFAULT_SCREEN_MARGIN = 10.0f;
#else
static constexpr float DEFAULT_SCREEN_MARGIN = 16.0f;
#endif

/// Sets the path to the font to use. Empty string means to use the default.
void SetFontPathAndRange(std::string path, std::vector<WCharType> range);

/// Sets the normal/emoji font range to use. Empty means no glyphs will be rasterized.
/// Should NOT be terminated with zeros, unlike the font range above.
void SetDynamicFontRange(std::vector<WCharType> font_range, std::vector<WCharType> emoji_range);

/// Returns a compacted font range, with adjacent glyphs merged into one pair.
std::vector<WCharType> CompactFontRange(std::span<const WCharType> range);

/// Changes the global scale.
void SetGlobalScale(float global_scale);

/// Initializes ImGui, creates fonts, etc.
bool Initialize(float global_scale, float screen_margin, Error* error);

/// Frees all ImGui resources.
void Shutdown();

/// Returns main ImGui context.
ImGuiContext* GetMainContext();

/// Returns true if there is currently a context created.
bool IsInitialized();

/// Sets the size of the screen margins, or "safe zone".
void SetScreenMargin(float margin);

/// Returns the size of the display window. Can be safely called from any thread.
float GetWindowWidth();
float GetWindowHeight();

/// Updates internal state when the window is size.
void WindowResized(GPUTexture::Format format, float width, float height);

/// Updates scaling of the on-screen elements.
void RequestScaleUpdate();

/// Call at the beginning of the frame to set up ImGui state.
void NewFrame();

/// Creates the draw list for the frame, akin to ImGui::Render().
void CreateDrawLists();

/// Renders ImGui screen elements. Call before EndPresent().
void RenderDrawLists(GPUSwapChain* swap_chain);
void RenderDrawLists(GPUTexture* texture);

/// Renders any on-screen display elements.
void RenderOSDMessages();

/// Returns the scale of all on-screen elements.
float GetGlobalScale();

/// Returns the screen margins, or "safe zone".
float GetScreenMargin();

/// Returns true if fullscreen fonts are present.
bool HasFullscreenFonts();

/// Allocates/adds fullscreen fonts if they're not loaded.
bool AddFullscreenFontsIfMissing();

/// Returns true if there is a separate debug font.
bool HasDebugFont();

/// Changes whether a debug font is generated. Otherwise, the OSD font will be used for GetStandardFont().
bool AddDebugFontIfMissing();

/// Returns the standard font for external drawing.
ImFont* GetDebugFont();

/// Returns the standard font for on-screen display drawing.
ImFont* GetOSDFont();

/// Returns the fixed-width font for external drawing.
ImFont* GetFixedFont();

/// Returns the medium font for external drawing, scaled by ImGuiFullscreen.
/// This font is allocated on demand.
ImFont* GetMediumFont();

/// Returns the large font for external drawing, scaled by ImGuiFullscreen.
/// This font is allocated on demand.
ImFont* GetLargeFont();

/// Returns true if imgui wants to intercept text input.
bool WantsTextInput();

/// Returns true if imgui wants to intercept mouse input.
bool WantsMouseInput();

/// Called on the UI or CPU thread in response to a key press. String is UTF-8.
void AddTextInput(std::string str);

/// Called on the UI or CPU thread in response to mouse movement.
void UpdateMousePosition(float x, float y);

/// Called on the CPU thread in response to a mouse button press.
/// Returns true if ImGui intercepted the event, and regular handlers should not execute.
bool ProcessPointerButtonEvent(InputBindingKey key, float value);

/// Called on the CPU thread in response to a mouse wheel movement.
/// Returns true if ImGui intercepted the event, and regular handlers should not execute.
bool ProcessPointerAxisEvent(InputBindingKey key, float value);

/// Called on the CPU thread in response to a key press.
/// Returns true if ImGui intercepted the event, and regular handlers should not execute.
bool ProcessHostKeyEvent(InputBindingKey key, float value);

/// Called on the CPU thread when any input event fires. Allows imgui to take over controller navigation.
bool ProcessGenericInputEvent(GenericInputBinding key, float value);

/// Sets an image and scale for a software cursor. Software cursors can be used for things like crosshairs.
void SetSoftwareCursor(u32 index, std::string image_path, float image_scale, u32 multiply_color = 0xFFFFFF);
bool HasSoftwareCursor(u32 index);
void ClearSoftwareCursor(u32 index);

/// Sets the position of a software cursor, used when we have relative coordinates such as controllers.
void SetSoftwareCursorPosition(u32 index, float pos_x, float pos_y);

/// Adds software cursors to ImGui render list.
void RenderSoftwareCursors();

/// Strips icon characters from a string.
std::string StripIconCharacters(std::string_view str);

#ifndef __ANDROID__

/// Auxiliary imgui windows.
struct AuxiliaryRenderWindowState
{
  Host::AuxiliaryRenderWindowHandle window_handle = nullptr;
  std::unique_ptr<GPUSwapChain> swap_chain;
  ImGuiContext* imgui_context = nullptr;
  bool close_request = false;
};

/// Create a new aux render window. This creates the window itself, swap chain, and imgui context.
/// Window position and dimensions are restored from the configuration file, under the specified section/key.
bool CreateAuxiliaryRenderWindow(AuxiliaryRenderWindowState* state, std::string_view title, std::string_view icon_name,
                                 const char* config_section, const char* config_prefix, u32 default_width,
                                 u32 default_height, Error* error);

/// Destroys a previously-created aux render window, optionally saving its position information.
void DestroyAuxiliaryRenderWindow(AuxiliaryRenderWindowState* state, const char* config_section = nullptr,
                                  const char* config_prefix = nullptr);

/// Renders the specified aux render window. draw_callback will be invoked if the window is not hidden.
/// Returns false if the user has closed the window.
bool RenderAuxiliaryRenderWindow(AuxiliaryRenderWindowState* state, void (*draw_callback)(float scale));

/// Processes input events from the host.
void ProcessAuxiliaryRenderWindowInputEvent(Host::AuxiliaryRenderWindowUserData userdata,
                                            Host::AuxiliaryRenderWindowEvent event,
                                            Host::AuxiliaryRenderWindowEventParam param1,
                                            Host::AuxiliaryRenderWindowEventParam param2,
                                            Host::AuxiliaryRenderWindowEventParam param3);

#endif

} // namespace ImGuiManager

namespace Host {

/// Typical durations for OSD messages.
static constexpr float OSD_CRITICAL_ERROR_DURATION = 20.0f;
static constexpr float OSD_ERROR_DURATION = 15.0f;
static constexpr float OSD_WARNING_DURATION = 10.0f;
static constexpr float OSD_INFO_DURATION = 5.0f;
static constexpr float OSD_QUICK_DURATION = 2.5f;

/// Adds OSD messages, duration is in seconds.
void AddOSDMessage(std::string message, float duration = 2.0f);
void AddKeyedOSDMessage(std::string key, std::string message, float duration = 2.0f);
void AddIconOSDMessage(std::string key, const char* icon, std::string message, float duration = 2.0f);
void AddKeyedOSDWarning(std::string key, std::string message, float duration = 2.0f);
void AddIconOSDWarning(std::string key, const char* icon, std::string message, float duration = 2.0f);
void RemoveKeyedOSDMessage(std::string key);
void RemoveKeyedOSDWarning(std::string key);
void ClearOSDMessages(bool clear_warnings);

/// Called by ImGuiManager when the cursor enters a text field. The host may choose to open an on-screen
/// keyboard for devices without a physical keyboard.
void BeginTextInput();

/// Called by ImGuiManager when the cursor leaves a text field.
void EndTextInput();

#ifndef __ANDROID__

/// Auxiliary window management.
bool CreateAuxiliaryRenderWindow(s32 x, s32 y, u32 width, u32 height, std::string_view title,
                                 std::string_view icon_name, AuxiliaryRenderWindowUserData userdata,
                                 AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error);
void DestroyAuxiliaryRenderWindow(AuxiliaryRenderWindowHandle handle, s32* pos_x = nullptr, s32* pos_y = nullptr,
                                  u32* width = nullptr, u32* height = nullptr);

#endif

} // namespace Host
