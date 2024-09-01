// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_texture.h"
#include "common/types.h"

#include <optional>

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

  Type type = Type::Surfaceless;
  void* display_connection = nullptr;
  void* window_handle = nullptr;
  u32 surface_width = 0;
  u32 surface_height = 0;
  float surface_refresh_rate = 0.0f;
  float surface_scale = 1.0f;
  GPUTexture::Format surface_format = GPUTexture::Format::Unknown;

  // Needed for macOS.
#ifdef __APPLE__
  void* surface_handle = nullptr;
#endif

  ALWAYS_INLINE bool IsSurfaceless() const { return type == Type::Surfaceless; }

  // Changes the window to be surfaceless (i.e. no handle/size/etc).
  void SetSurfaceless();

  static std::optional<float> QueryRefreshRateForWindow(const WindowInfo& wi);
};
