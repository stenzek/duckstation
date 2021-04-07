#pragma once
#include "types.h"

// Contains the information required to create a graphics context in a window.
struct WindowInfo
{
  enum class Type
  {
    Surfaceless,
    Win32,
    X11,
    Wayland,
    MacOS,
    Android,
    Display,
  };

  enum class SurfaceFormat
  {
    None,
    Auto,
    RGB8,
    RGBA8,
    RGB565,
    Count
  };

  Type type = Type::Surfaceless;
  void* display_connection = nullptr;
  void* window_handle = nullptr;
  u32 surface_width = 0;
  u32 surface_height = 0;
  float surface_refresh_rate = 0.0f;
  float surface_scale = 1.0f;
  SurfaceFormat surface_format = SurfaceFormat::RGB8;

  // Needed for macOS.
#ifdef __APPLE__
  void* surface_handle = nullptr;
#endif

  static bool QueryRefreshRateForWindow(const WindowInfo& wi, float* refresh_rate);
};
