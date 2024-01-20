// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/types.h"
#include <string>

class Error;

struct ImFont;

union InputBindingKey;
enum class GenericInputBinding : u8;

namespace ImGuiManager {
/// Sets the path to the font to use. Empty string means to use the default.
void SetFontPathAndRange(std::string path, std::vector<u16> range);

/// Changes the global scale.
void SetGlobalScale(float global_scale);

/// Changes whether OSD messages are silently dropped.
void SetShowOSDMessages(bool enable);

/// Initializes ImGui, creates fonts, etc.
bool Initialize(float global_scale, bool show_osd_messages, Error* error);

/// Frees all ImGui resources.
void Shutdown();

/// Returns the size of the display window. Can be safely called from any thread.
float GetWindowWidth();
float GetWindowHeight();

/// Updates internal state when the window is size.
void WindowResized();

/// Updates scaling of the on-screen elements.
void UpdateScale();

/// Call at the beginning of the frame to set up ImGui state.
void NewFrame();

/// Renders any on-screen display elements.
void RenderOSDMessages();

/// Returns the scale of all on-screen elements.
float GetGlobalScale();

/// Returns true if fullscreen fonts are present.
bool HasFullscreenFonts();

/// Allocates/adds fullscreen fonts if they're not loaded.
bool AddFullscreenFontsIfMissing();

/// Returns the standard font for external drawing.
ImFont* GetStandardFont();

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
} // namespace ImGuiManager

namespace Host {
/// Typical durations for OSD messages.
static constexpr float OSD_CRITICAL_ERROR_DURATION = 20.0f;
static constexpr float OSD_ERROR_DURATION = 15.0f;
static constexpr float OSD_WARNING_DURATION = 10.0f;
static constexpr float OSD_INFO_DURATION = 5.0f;
static constexpr float OSD_QUICK_DURATION = 2.5f;

/// Returns the scale of OSD elements.
float GetOSDScale();

/// Adds OSD messages, duration is in seconds.
void AddOSDMessage(std::string message, float duration = 2.0f);
void AddKeyedOSDMessage(std::string key, std::string message, float duration = 2.0f);
void AddIconOSDMessage(std::string key, const char* icon, std::string message, float duration = 2.0f);
void AddFormattedOSDMessage(float duration, const char* format, ...);
void AddKeyedFormattedOSDMessage(std::string key, float duration, const char* format, ...);
void RemoveKeyedOSDMessage(std::string key);
void ClearOSDMessages();
} // namespace Host
