#pragma once

#include "YBaseLib/Common.h"
#include <cstdint>
#include <cstring>
#include <type_traits>

// Force inline helper
#ifndef ALWAYS_INLINE
#if defined(_MSC_VER)
#define ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define ALWAYS_INLINE inline
#endif
#endif

using s8 = int8_t;
using u8 = uint8_t;
using s16 = int16_t;
using u16 = uint16_t;
using s32 = int32_t;
using u32 = uint32_t;
using s64 = int64_t;
using u64 = uint64_t;

// Enable use of static_assert in constexpr if
template<class T>
struct dependent_false : std::false_type
{
};
template<int T>
struct dependent_int_false : std::false_type
{
};

// Use a signed number for cycle counting
using CycleCount = int64_t;

// Use int64 for time tracking
using SimulationTime = int64_t;

// Helpers for simulation time.
ALWAYS_INLINE constexpr SimulationTime SecondsToSimulationTime(SimulationTime s)
{
  return s * INT64_C(1000000000);
}
ALWAYS_INLINE constexpr SimulationTime MillisecondsToSimulationTime(SimulationTime ms)
{
  return ms * INT64_C(1000000);
}
ALWAYS_INLINE constexpr SimulationTime MicrosecondsToSimulationTime(SimulationTime us)
{
  return us * INT64_C(1000);
}
ALWAYS_INLINE constexpr SimulationTime SimulationTimeToSeconds(SimulationTime s)
{
  return s / INT64_C(1000000000);
}
ALWAYS_INLINE constexpr SimulationTime SimulationTimeToMilliseconds(SimulationTime ms)
{
  return ms / INT64_C(1000000);
}
ALWAYS_INLINE constexpr SimulationTime SimulationTimeToMicroseconds(SimulationTime us)
{
  return us / INT64_C(1000);
}

// Calculates the difference between the specified timestamps, accounting for signed overflow.
ALWAYS_INLINE constexpr SimulationTime GetSimulationTimeDifference(SimulationTime prev, SimulationTime now)
{
  if (prev <= now)
    return now - prev;
  else
    return (std::numeric_limits<SimulationTime>::max() - prev) + now;
}

// Zero-extending helper
template<typename TReturn, typename TValue>
ALWAYS_INLINE constexpr TReturn ZeroExtend(TValue value)
{
  return static_cast<TReturn>(static_cast<typename std::make_unsigned<TReturn>::type>(
    static_cast<typename std::make_unsigned<TValue>::type>(value)));
}
// Sign-extending helper
template<typename TReturn, typename TValue>
ALWAYS_INLINE constexpr TReturn SignExtend(TValue value)
{
  return static_cast<TReturn>(
    static_cast<typename std::make_signed<TReturn>::type>(static_cast<typename std::make_signed<TValue>::type>(value)));
}

// Type-specific helpers
template<typename TValue>
ALWAYS_INLINE constexpr u16 ZeroExtend16(TValue value)
{
  return ZeroExtend<u16, TValue>(value);
}
template<typename TValue>
ALWAYS_INLINE constexpr u32 ZeroExtend32(TValue value)
{
  return ZeroExtend<u32, TValue>(value);
}
template<typename TValue>
ALWAYS_INLINE constexpr u64 ZeroExtend64(TValue value)
{
  return ZeroExtend<u64, TValue>(value);
}
template<typename TValue>
ALWAYS_INLINE constexpr u16 SignExtend16(TValue value)
{
  return SignExtend<u16, TValue>(value);
}
template<typename TValue>
ALWAYS_INLINE constexpr u32 SignExtend32(TValue value)
{
  return SignExtend<u32, TValue>(value);
}
template<typename TValue>
ALWAYS_INLINE constexpr u64 SignExtend64(TValue value)
{
  return SignExtend<u64, TValue>(value);
}
template<typename TValue>
ALWAYS_INLINE constexpr u8 Truncate8(TValue value)
{
  return static_cast<u8>(static_cast<typename std::make_unsigned<decltype(value)>::type>(value));
}
template<typename TValue>
ALWAYS_INLINE constexpr u16 Truncate16(TValue value)
{
  return static_cast<u16>(static_cast<typename std::make_unsigned<decltype(value)>::type>(value));
}
template<typename TValue>
ALWAYS_INLINE constexpr u32 Truncate32(TValue value)
{
  return static_cast<u32>(static_cast<typename std::make_unsigned<decltype(value)>::type>(value));
}

// BCD helpers
ALWAYS_INLINE constexpr u8 DecimalToBCD(u8 value)
{
  return ((value / 10) << 4) + (value % 10);
}

ALWAYS_INLINE constexpr u8 BCDToDecimal(u8 value)
{
  return ((value >> 4) * 10) + (value % 16);
}

// Boolean to integer
ALWAYS_INLINE constexpr u8 BoolToUInt8(bool value)
{
  return static_cast<u8>(value);
}
ALWAYS_INLINE constexpr u16 BoolToUInt16(bool value)
{
  return static_cast<u16>(value);
}
ALWAYS_INLINE constexpr u32 BoolToUInt32(bool value)
{
  return static_cast<u32>(value);
}
ALWAYS_INLINE constexpr u64 BoolToUInt64(bool value)
{
  return static_cast<u64>(value);
}

// Integer to boolean
template<typename TValue>
ALWAYS_INLINE constexpr bool ConvertToBool(TValue value)
{
  return static_cast<bool>(value);
}

// Unsafe integer to boolean
template<typename TValue>
ALWAYS_INLINE constexpr bool ConvertToBoolUnchecked(TValue value)
{
  // static_assert(sizeof(uint8) == sizeof(bool));
  bool ret;
  std::memcpy(&ret, &value, sizeof(bool));
  return ret;
}

// Generic sign extension
template<int NBITS, typename T>
ALWAYS_INLINE constexpr T SignExtendN(T value)
{
  // http://graphics.stanford.edu/~seander/bithacks.html#VariableSignExtend
  constexpr int shift = 8 * sizeof(T) - NBITS;
  return static_cast<T>((static_cast<std::make_signed_t<T>>(value) << shift) >> shift);
}

// Enum class bitwise operators
#define IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(type_)                                                                  \
  ALWAYS_INLINE constexpr type_ operator&(type_ lhs, type_ rhs)                                                               \
  {                                                                                                                    \
    return static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) &                                    \
                              static_cast<std::underlying_type<type_>::type>(rhs));                                    \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_ operator|(type_ lhs, type_ rhs)                                                               \
  {                                                                                                                    \
    return static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) |                                    \
                              static_cast<std::underlying_type<type_>::type>(rhs));                                    \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_ operator^(type_ lhs, type_ rhs)                                                               \
  {                                                                                                                    \
    return static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) ^                                    \
                              static_cast<std::underlying_type<type_>::type>(rhs));                                    \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_ operator~(type_ val)                                                                          \
  {                                                                                                                    \
    return static_cast<type_>(~static_cast<std::underlying_type<type_>::type>(val));                                   \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_& operator&=(type_& lhs, type_ rhs)                                                            \
  {                                                                                                                    \
    lhs = static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) &                                     \
                             static_cast<std::underlying_type<type_>::type>(rhs));                                     \
    return lhs;                                                                                                        \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_& operator|=(type_& lhs, type_ rhs)                                                            \
  {                                                                                                                    \
    lhs = static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) |                                     \
                             static_cast<std::underlying_type<type_>::type>(rhs));                                     \
    return lhs;                                                                                                        \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_& operator^=(type_& lhs, type_ rhs)                                                            \
  {                                                                                                                    \
    lhs = static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) ^                                     \
                             static_cast<std::underlying_type<type_>::type>(rhs));                                     \
    return lhs;                                                                                                        \
  }
