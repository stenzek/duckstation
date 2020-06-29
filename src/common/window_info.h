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
    Libretro,
  };

  enum class SurfaceFormat
  {
    None,
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
  SurfaceFormat surface_format = SurfaceFormat::RGB8;

  // Needed for macOS.
#ifdef __APPLE__
  void* surface_handle = nullptr;
#endif
};
