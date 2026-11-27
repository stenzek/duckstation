// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <cmath>
#include <type_traits>

namespace ControllerHelpers {

/// Returns true if the specified coordinates are inside a circular deadzone.
ALWAYS_INLINE bool InCircularDeadzone(float deadzone, float pos_x, float pos_y)
{
  // Calculate the actual distance from center, and compare to deadzone radius.
  const float distance = std::sqrt(pos_x * pos_x + pos_y * pos_y);
  return (distance <= deadzone);
}

/// Converts a 0..255 half-axis value to an unsigned 8-bit value, with 127 indicating center.
ALWAYS_INLINE u8 MergeHalfAxes(u8 neg_value, u8 pos_value, bool invert)
{
  if (invert)
    std::swap(neg_value, pos_value);

  // Merge before rounding, so that equal opposing inputs produce center.
  const s32 diff = static_cast<s32>(pos_value) - static_cast<s32>(neg_value);
  return static_cast<u8>((diff > 0) ? (127 + ((diff + 1) / 2)) : (127 - (-diff / 2)));
}

/// Converts a 0..255 half-axis value to a normalized floating-point value, with 0 indicating center.
ALWAYS_INLINE float MergeHalfAxesToFloat(u8 neg_value, u8 pos_value, bool invert)
{
  const float result = (static_cast<s32>(pos_value) - static_cast<s32>(neg_value)) / 255.0f;
  return (invert ? -result : result);
}

/// Merges the half axes of a stick into both of its axes, centring the stick inside a circular deadzone.
/// Bit 0 of invert inverts the X axis, and bit 1 the Y axis.
ALWAYS_INLINE void UpdateStickAxes(u8 left, u8 right, u8 up, u8 down, u8 invert, float deadzone, u8* x, u8* y)
{
  *x = MergeHalfAxes(left, right, (invert & 1));
  *y = MergeHalfAxes(up, down, (invert & 2));

  if (deadzone > 0.0f && InCircularDeadzone(deadzone, MergeHalfAxesToFloat(left, right, (invert & 1)),
                                            MergeHalfAxesToFloat(up, down, (invert & 2))))
  {
    *x = 127;
    *y = 127;
  }
}

/// Removes opposing directions (left/right and up/down), preventing these buttons from being pressed concurrently.
/// Note: Assumes up is bit 4, right is bit 5, down is bit 6, and left is bit 7, which is the case for all PSX pads.
template<typename T>
  requires(std::is_integral_v<T> || std::is_enum_v<T>)
ALWAYS_INLINE T RemoveOpposingDirections(T state)
{
  using BitsType = std::make_unsigned_t<
    typename std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>, std::type_identity<T>>::type>;

  const BitsType bits = static_cast<BitsType>(state);
  const BitsType conflicts = static_cast<BitsType>(~(bits | (bits >> 2))) & static_cast<BitsType>(0x0030u);

  // Active-low, so setting both conflicting bits releases them.
  // Prefer Up (bit 4) over Down (bit 6), and Right (bit 5) over Left (bit 7).
  return static_cast<T>(bits | (conflicts << 2));
}

} // namespace ControllerHelpers