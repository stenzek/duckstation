// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

// BCD helpers
ALWAYS_INLINE constexpr u8 BinaryToBCD(u8 value)
{
  return ((value / 10) << 4) + (value % 10);
}
ALWAYS_INLINE constexpr u8 PackedBCDToBinary(u8 value)
{
  return ((value >> 4) * 10) + (value % 16);
}
ALWAYS_INLINE constexpr u8 IsValidBCDDigit(u8 digit)
{
  return (digit <= 9);
}
ALWAYS_INLINE constexpr u8 IsValidPackedBCD(u8 value)
{
  return IsValidBCDDigit(value & 0x0F) && IsValidBCDDigit(value >> 4);
}
