#pragma once

#include "common/string.h"
#include "common/types.h"

#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct WindowInfo;
enum class AudioBackend : u8;
class AudioStream;
class CDImage;

/// Marks a core string as being translatable.
#define TRANSLATABLE(context, str) str

/// Generic input bindings. These roughly match a DualShock 4 or XBox One controller.
/// They are used for automatic binding to PS2 controller types, and for big picture mode navigation.
enum class GenericInputBinding : u8
{
  Unknown,

  DPadUp,
  DPadRight,
  DPadLeft,
  DPadDown,

  LeftStickUp,
  LeftStickRight,
  LeftStickDown,
  LeftStickLeft,
  L3,

  RightStickUp,
  RightStickRight,
  RightStickDown,
  RightStickLeft,
  R3,

  Triangle, // Y on XBox pads.
  Circle,   // B on XBox pads.
  Cross,    // A on XBox pads.
  Square,   // X on XBox pads.

  Select, // Share on DS4, View on XBox pads.
  Start,  // Options on DS4, Menu on XBox pads.
  System, // PS button on DS4, Guide button on XBox pads.

  L1, // LB on Xbox pads.
  L2, // Left trigger on XBox pads.
  R1, // RB on XBox pads.
  R2, // Right trigger on Xbox pads.

  SmallMotor, // High frequency vibration.
  LargeMotor, // Low frequency vibration.

  Count,
};

namespace Host {
/// Reads a file from the resources directory of the application.
/// This may be outside of the "normal" filesystem on platforms such as Mac.
std::optional<std::vector<u8>> ReadResourceFile(const char* filename);

/// Reads a resource file file from the resources directory as a string.
std::optional<std::string> ReadResourceFileToString(const char* filename);

/// Returns the modified time of a resource.
std::optional<std::time_t> GetResourceFileTimestamp(const char* filename);

/// Translates a string to the current language.
TinyString TranslateString(const char* context, const char* str, const char* disambiguation = nullptr, int n = -1);
std::string TranslateStdString(const char* context, const char* str, const char* disambiguation = nullptr, int n = -1);

std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend);

/// Returns the scale of OSD elements.
float GetOSDScale();

/// Adds OSD messages, duration is in seconds.
void AddOSDMessage(std::string message, float duration = 2.0f);
void AddKeyedOSDMessage(std::string key, std::string message, float duration = 2.0f);
void AddFormattedOSDMessage(float duration, const char* format, ...);
void AddKeyedFormattedOSDMessage(std::string key, float duration, const char* format, ...);
void RemoveKeyedOSDMessage(std::string key);
void ClearOSDMessages();

/// Displays an asynchronous error on the UI thread, i.e. doesn't block the caller.
void ReportErrorAsync(const std::string_view& title, const std::string_view& message);
void ReportFormattedErrorAsync(const std::string_view& title, const char* format, ...);

/// Displays a synchronous confirmation on the UI thread, i.e. blocks the caller.
bool ConfirmMessage(const std::string_view& title, const std::string_view& message);
bool ConfirmFormattedMessage(const std::string_view& title, const char* format, ...);

/// Debugger feedback.
void ReportDebuggerMessage(const std::string_view& message);
void ReportFormattedDebuggerMessage(const char* format, ...);

/// Displays a loading screen with the logo, rendered with ImGui. Use when executing possibly-time-consuming tasks
/// such as compiling shaders when starting up.
void DisplayLoadingScreen(const char* message, int progress_min = -1, int progress_max = -1, int progress_value = -1);

/// Internal method used by pads to dispatch vibration updates to input sources.
/// Intensity is normalized from 0 to 1.
void SetPadVibrationIntensity(u32 pad_index, float large_or_single_motor_intensity, float small_motor_intensity);

/// Enables "relative" mouse mode, locking the cursor position and returning relative coordinates.
void SetMouseMode(bool relative, bool hide_cursor);

/// Safely executes a function on the VM thread.
void RunOnCPUThread(std::function<void()> function, bool block = false);
} // namespace Host
