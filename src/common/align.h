#pragma once

namespace Common {
template<typename T>
bool IsAligned(T value, unsigned int alignment)
{
  return (value % static_cast<T>(alignment)) == 0;
}
template<typename T>
T AlignUp(T value, unsigned int alignment)
{
  return (value + static_cast<T>(alignment - 1)) / static_cast<T>(alignment) * static_cast<T>(alignment);
}
template<typename T>
T AlignDown(T value, unsigned int alignment)
{
  return value / static_cast<T>(alignment) * static_cast<T>(alignment);
}
template<typename T>
bool IsAlignedPow2(T value, unsigned int alignment)
{
  return (value & static_cast<T>(alignment - 1)) == 0;
}
template<typename T>
T AlignUpPow2(T value, unsigned int alignment)
{
  return (value + static_cast<T>(alignment - 1)) & static_cast<T>(~static_cast<T>(alignment - 1));
}
template<typename T>
T AlignDownPow2(T value, unsigned int alignment)
{
  return value & static_cast<T>(~static_cast<T>(alignment - 1));
}
template<typename T>
bool IsPow2(T value)
{
  return (value & (value - 1)) == 0;
}
} // namespace Common
