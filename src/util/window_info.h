// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <optional>

class Error;

enum class GPUTextureFormat : u8;

enum class WindowInfoType : u8
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

enum class WindowInfoPrerotation : u8
{
  Identity,
  Rotate90Clockwise,
  Rotate180Clockwise,
  Rotate270Clockwise,
};

// Contains the information required to create a graphics context in a window.
struct WindowInfo
{
  WindowInfo();

  WindowInfoType type;
  GPUTextureFormat surface_format;
  WindowInfoPrerotation surface_prerotation;
  u16 surface_width;
  u16 surface_height;
  float surface_refresh_rate;
  float surface_scale;
  void* display_connection;
  void* window_handle;

  ALWAYS_INLINE bool IsSurfaceless() const { return type == WindowInfoType::Surfaceless; }

  ALWAYS_INLINE u32 GetPostRotatedWidth() const
  {
    return ShouldSwapDimensionsForPreRotation(surface_prerotation) ? surface_height : surface_width;
  }
  ALWAYS_INLINE u32 GetPostRotatedHeight() const
  {
    return ShouldSwapDimensionsForPreRotation(surface_prerotation) ? surface_width : surface_height;
  }

  ALWAYS_INLINE static bool ShouldSwapDimensionsForPreRotation(WindowInfoPrerotation prerotation)
  {
    return (prerotation == WindowInfoPrerotation::Rotate90Clockwise ||
            prerotation == WindowInfoPrerotation::Rotate270Clockwise);
  }

  /// Sets a new pre-rotation, adjusting the virtual width/height to suit.
  void SetPreRotated(WindowInfoPrerotation prerotation);

  static float GetZRotationForPreRotation(WindowInfoPrerotation prerotation);

  static std::optional<float> QueryRefreshRateForWindow(const WindowInfo& wi, Error* error = nullptr);
};
