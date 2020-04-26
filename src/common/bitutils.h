#pragma once
#include "types.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

template<typename T>
ALWAYS_INLINE unsigned CountLeadingZeros(T value)
{
#ifdef _MSC_VER
  if constexpr (sizeof(value) >= sizeof(u64))
  {
    unsigned long index;
    return _BitScanReverse64(&index, ZeroExtend64(value)) ? static_cast<unsigned>(index) : 0;
  }
  else
  {
    unsigned long index;
    return _BitScanReverse(&index, ZeroExtend32(value)) ? static_cast<unsigned>(index) : 0;
  }
#else
  if constexpr (sizeof(value) >= sizeof(u64))
  {
    const unsigned bits = static_cast<unsigned>(__builtin_clzl(ZeroExtend64(value)));
    return (value != 0) ? static_cast<unsigned>(bits) : 0;
  }
  else
  {
    const unsigned bits = static_cast<unsigned>(__builtin_clz(ZeroExtend32(value)));
    return (value != 0) ? static_cast<unsigned>(bits) : 0;
  }
#endif
}

template<typename T>
ALWAYS_INLINE unsigned CountTrailingZeros(T value)
{
#ifdef _MSC_VER
  if constexpr (sizeof(value) >= sizeof(u64))
  {
    unsigned long index;
    return _BitScanForward64(&index, ZeroExtend64(value)) ? static_cast<unsigned>(index) : 0;
  }
  else
  {
    unsigned long index;
    return _BitScanForward(&index, ZeroExtend32(value)) ? static_cast<unsigned>(index) : 0;
  }
#else
  if constexpr (sizeof(value) >= sizeof(u64))
  {
    const unsigned bits = static_cast<unsigned>(__builtin_ctzl(ZeroExtend64(value)));
    return (value != 0) ? static_cast<unsigned>(bits) : 0;
  }
  else
  {
    const unsigned bits = static_cast<unsigned>(__builtin_ctz(ZeroExtend32(value)));
    return (value != 0) ? static_cast<unsigned>(bits) : 0;
  }
#endif
}
