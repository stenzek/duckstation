#pragma once

namespace Common {
template<typename T>
constexpr bool IsAligned(T value, unsigned int alignment)
{
  return (value % static_cast<T>(alignment)) == 0;
}
template<typename T>
constexpr T AlignUp(T value, unsigned int alignment)
{
  return (value + static_cast<T>(alignment - 1)) / static_cast<T>(alignment) * static_cast<T>(alignment);
}
template<typename T>
constexpr T AlignDown(T value, unsigned int alignment)
{
  return value / static_cast<T>(alignment) * static_cast<T>(alignment);
}
template<typename T>
constexpr bool IsAlignedPow2(T value, unsigned int alignment)
{
  return (value & static_cast<T>(alignment - 1)) == 0;
}
template<typename T>
constexpr T AlignUpPow2(T value, unsigned int alignment)
{
  return (value + static_cast<T>(alignment - 1)) & static_cast<T>(~static_cast<T>(alignment - 1));
}
template<typename T>
constexpr T AlignDownPow2(T value, unsigned int alignment)
{
  return value & static_cast<T>(~static_cast<T>(alignment - 1));
}
template<typename T>
constexpr bool IsPow2(T value)
{
  return (value & (value - 1)) == 0;
}
template<typename T>
constexpr T PreviousPow2(T value)
{
  if (value == static_cast<T>(0))
    return 0;

  value |= (value >> 1);
  value |= (value >> 2);
  value |= (value >> 4);
  value |= (value >> 8);
  value |= (value >> 16);
  return value - (value >> 1);
}
} // namespace Common
