#pragma once
#include "types.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

/// Returns the number of zero bits before the first set bit, going MSB->LSB.
template<typename T>
ALWAYS_INLINE unsigned CountLeadingZeros(T value)
{
#ifdef _MSC_VER
  if constexpr (sizeof(value) >= sizeof(u64))
  {
    unsigned long index;
    _BitScanReverse64(&index, ZeroExtend64(value));
    return static_cast<unsigned>(index) ^ static_cast<unsigned>((sizeof(value) * 8u) - 1u);
  }
  else
  {
    unsigned long index;
    _BitScanReverse(&index, ZeroExtend32(value));
    return static_cast<unsigned>(index) ^ static_cast<unsigned>((sizeof(value) * 8u) - 1u);
  }
#else
  if constexpr (sizeof(value) >= sizeof(u64))
    return static_cast<unsigned>(__builtin_clzl(ZeroExtend64(value)));
  else if constexpr (sizeof(value) == sizeof(u32))
    return static_cast<unsigned>(__builtin_clz(ZeroExtend32(value)));
  else
    return static_cast<unsigned>(__builtin_clz(ZeroExtend32(value))) & static_cast<unsigned>((sizeof(value) * 8u) - 1u);
#endif
}

/// Returns the number of zero bits before the first set bit, going LSB->MSB.
template<typename T>
ALWAYS_INLINE unsigned CountTrailingZeros(T value)
{
#ifdef _MSC_VER
  if constexpr (sizeof(value) >= sizeof(u64))
  {
    unsigned long index;
    _BitScanForward64(&index, ZeroExtend64(value));
    return index;
  }
  else
  {
    unsigned long index;
    _BitScanForward(&index, ZeroExtend32(value));
    return index;
  }
#else
  if constexpr (sizeof(value) >= sizeof(u64))
    return static_cast<unsigned>(__builtin_ctzl(ZeroExtend64(value)));
  else
    return static_cast<unsigned>(__builtin_ctz(ZeroExtend32(value)));
#endif
}
