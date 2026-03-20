// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "window_info.h"
#include "gpu_types.h"

#include <numbers>

WindowInfo::WindowInfo()
  : type(WindowInfoType::Surfaceless), surface_format(GPUTextureFormat::Unknown),
    surface_prerotation(WindowInfoPrerotation::Identity), surface_width(0), surface_height(0),
    surface_refresh_rate(0.0f), surface_scale(1.0f), display_connection(nullptr), window_handle(nullptr)
{
}

float WindowInfo::GetZRotationForPreRotation(WindowInfoPrerotation prerotation)
{
  static constexpr const std::array<float, 4> rotation_radians = {{
    0.0f,                                        // Identity
    static_cast<float>(std::numbers::pi * 1.5f), // Rotate90Clockwise
    static_cast<float>(std::numbers::pi),        // Rotate180Clockwise
    static_cast<float>(std::numbers::pi / 2.0),  // Rotate270Clockwise
  }};

  return rotation_radians[static_cast<size_t>(prerotation)];
}
