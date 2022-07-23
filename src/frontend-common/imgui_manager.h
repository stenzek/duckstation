#pragma once

#include "common/types.h"
#include <string>

struct ImFont;

union InputBindingKey;
enum class GenericInputBinding : u8;

namespace ImGuiManager {
/// Sets the path to the font to use. Empty string means to use the default.
void SetFontPath(std::string path);

/// Sets the glyph range to use when loading fonts.
void SetFontRange(const u16* range);

/// Initializes ImGui, creates fonts, etc.
bool Initialize();

/// Frees all ImGui resources.
void Shutdown();

/// Updates internal state when the window is size.
void WindowResized();

/// Updates scaling of the on-screen elements.
void UpdateScale();

/// Call at the beginning of the frame to set up ImGui state.
void NewFrame();

/// Renders any on-screen display elements.
void RenderOSD();

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
} // namespace ImGuiManager
