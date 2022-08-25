#pragma once
#include "common/types.h"
#include "core/system.h"
#include <functional>
#include <string>

namespace NoGUIHost {
/// Sets batch mode (exit after game shutdown).
bool InBatchMode();
void SetBatchMode(bool enabled);

/// Starts the virtual machine.
void StartSystem(SystemBootParameters params);

/// Returns the application name and version, optionally including debug/devel config indicator.
std::string GetAppNameAndVersion();

/// Returns the debug/devel config indicator.
std::string GetAppConfigSuffix();

/// Thread-safe settings access.
void SaveSettings();

/// Called on the UI thread in response to various events.
void ProcessPlatformWindowResize(s32 width, s32 height, float scale);
void ProcessPlatformMouseMoveEvent(float x, float y);
void ProcessPlatformMouseButtonEvent(s32 button, bool pressed);
void ProcessPlatformMouseWheelEvent(float x, float y);
void ProcessPlatformKeyEvent(s32 key, bool pressed);
void ProcessPlatformTextEvent(const char* text);
void PlatformWindowFocusGained();
void PlatformWindowFocusLost();
bool GetSavedPlatformWindowGeometry(s32* x, s32* y, s32* width, s32* height);
void SavePlatformWindowGeometry(s32 x, s32 y, s32 width, s32 height);
} // namespace NoGUIHost