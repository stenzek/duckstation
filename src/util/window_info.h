// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"
#include "gpu_texture.h"

#include <optional>

class Error;

// Contains the information required to create a graphics context in a window.
struct WindowInfo
{
  enum class Type : u8
  {
    Surfaceless,
    Win32,
    Xlib,
    XCB,
    Wayland,
    MacOS,
    Android,
    SDL,
  };

  enum class PreRotation : u8
  {
    Identity,
    Rotate90Clockwise,
    Rotate180Clockwise,
    Rotate270Clockwise,
  };

  Type type = Type::Surfaceless;
  GPUTexture::Format surface_format = GPUTexture::Format::Unknown;
  PreRotation surface_prerotation = PreRotation::Identity;
  u16 surface_width = 0;
  u16 surface_height = 0;
  float surface_refresh_rate = 0.0f;
  float surface_scale = 1.0f;
  void* display_connection = nullptr;
  void* window_handle = nullptr;

  ALWAYS_INLINE bool IsSurfaceless() const { return type == Type::Surfaceless; }

  ALWAYS_INLINE u32 GetPostRotatedWidth() const
  {
    return ShouldSwapDimensionsForPreRotation(surface_prerotation) ? surface_height : surface_width;
  }
  ALWAYS_INLINE u32 GetPostRotatedHeight() const
  {
    return ShouldSwapDimensionsForPreRotation(surface_prerotation) ? surface_width : surface_height;
  }

  ALWAYS_INLINE static bool ShouldSwapDimensionsForPreRotation(PreRotation prerotation)
  {
    return (prerotation == PreRotation::Rotate90Clockwise || prerotation == PreRotation::Rotate270Clockwise);
  }

  /// Sets a new pre-rotation, adjusting the virtual width/height to suit.
  void SetPreRotated(PreRotation prerotation);

  static float GetZRotationForPreRotation(PreRotation prerotation);

  static std::optional<float> QueryRefreshRateForWindow(const WindowInfo& wi, Error* error = nullptr);
};
