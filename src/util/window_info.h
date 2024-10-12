// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"
#include "gpu_texture.h"

#include <optional>

// Contains the information required to create a graphics context in a window.
struct WindowInfo
{
  enum class Type : u8
  {
    Surfaceless,
    Win32,
    X11,
    Wayland,
    MacOS,
    Android,
  };

  Type type = Type::Surfaceless;
  GPUTexture::Format surface_format = GPUTexture::Format::Unknown;
  u16 surface_width = 0;
  u16 surface_height = 0;
  float surface_refresh_rate = 0.0f;
  float surface_scale = 1.0f;
  void* display_connection = nullptr;
  void* window_handle = nullptr;

  ALWAYS_INLINE bool IsSurfaceless() const { return type == Type::Surfaceless; }

  static std::optional<float> QueryRefreshRateForWindow(const WindowInfo& wi);
};
