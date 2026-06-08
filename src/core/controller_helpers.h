// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <cmath>

namespace ControllerHelpers {

/// Returns true if the specified coordinates are inside a circular deadzone.
ALWAYS_INLINE bool InCircularDeadzone(float deadzone, float pos_x, float pos_y)
{
  // Calculate the actual distance from center, and compare to deadzone radius.
  const float distance = std::sqrt(pos_x * pos_x + pos_y * pos_y);
  return (distance <= deadzone);
}

/// Converts a 0..255 half-axis value to an unsigned 8-bit value, with 128 indicating center.
ALWAYS_INLINE u8 MergeHalfAxes(u8 neg_value, u8 pos_value, bool invert)
{
  if (invert)
    std::swap(neg_value, pos_value);

  return static_cast<u8>(128 + (static_cast<u32>(pos_value) / 2) - ((static_cast<u32>(neg_value) + 1) / 2));
}

/// Converts a 0..255 half-axis value to a normalized floating-point value, with 0 indicating center.
ALWAYS_INLINE float MergeHalfAxesToFloat(u8 neg_value, u8 pos_value, bool invert)
{
  const float result = (static_cast<s32>(pos_value) - static_cast<s32>(neg_value)) / 255.0f;
  return (invert ? -result : result);
}

} // namespace ControllerHelpers