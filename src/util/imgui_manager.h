// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/gpu_texture.h"

#include "common/types.h"

#include <array>
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

enum class OSDMessageType : u8
{
  Error,
  Warning,
  Info,
  Quick,
  Persistent,

  MaxCount
};

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

enum class TextFont : u8
{
  Default,
  Chinese,
  Japanese,
  Korean,
  MaxCount
};

using TextFontOrder = std::array<TextFont, static_cast<size_t>(TextFont::MaxCount)>;

/// Default size for screen margins.
#ifndef __ANDROID__
inline constexpr float DEFAULT_SCREEN_MARGIN = 10.0f;
#else
inline constexpr float DEFAULT_SCREEN_MARGIN = 16.0f;
#endif

/// Sets the order for text fonts.
TextFontOrder GetDefaultTextFontOrder();
void SetTextFontOrder(const TextFontOrder& order);

/// Initializes ImGui, creates fonts, etc.
bool Initialize(Error* error);

/// Frees all ImGui resources.
void Shutdown(bool clear_fsui_state);

/// Returns main ImGui context.
ImGuiContext* GetMainContext();

/// Returns true if there is currently a context created.
bool IsInitialized();

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

/// Returns the standard font for on-screen display drawing.
ImFont* GetTextFont();

/// Returns the fixed-width font for external drawing.
float GetFixedFontSize();
ImFont* GetFixedFont();

/// Returns the standard font for external drawing.
float GetDebugFontSize(float window_scale);

/// Returns the font size for rendering the OSD.
float GetOSDFontSize();

/// Multiplies an arbitrary size by the OSD scale.
float OSDScale(float size);

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

/// Returns the duration for the specified OSD message type.
float GetOSDMessageDuration(OSDMessageType type);

/// Returns the ending position of OSD messages from the last frame.
float GetOSDMessageEndPosition();

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

/// Adds OSD messages.
void AddOSDMessage(OSDMessageType type, std::string message);
void AddKeyedOSDMessage(OSDMessageType type, std::string key, std::string message);
void AddIconOSDMessage(OSDMessageType type, std::string key, const char* icon, std::string message);
void AddIconOSDMessage(OSDMessageType type, std::string key, const char* icon, std::string title, std::string message);
void RemoveKeyedOSDMessage(std::string key);
void ClearOSDMessages();

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
