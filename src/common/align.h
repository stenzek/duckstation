// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <cstdlib>
#include <memory>
#include <type_traits>

#ifdef _MSC_VER
#include <malloc.h>
#endif

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
  if constexpr (sizeof(T) >= 16)
    value |= (value >> 8);
  if constexpr (sizeof(T) >= 32)
    value |= (value >> 16);
  if constexpr (sizeof(T) >= 64)
    value |= (value >> 32);
  return value - (value >> 1);
}
template<typename T>
constexpr T NextPow2(T value)
{
  // https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
  if (value == static_cast<T>(0))
    return 0;

  value--;
  value |= (value >> 1);
  value |= (value >> 2);
  value |= (value >> 4);
  if constexpr (sizeof(T) >= 16)
    value |= (value >> 8);
  if constexpr (sizeof(T) >= 32)
    value |= (value >> 16);
  if constexpr (sizeof(T) >= 64)
    value |= (value >> 32);
  value++;
  return value;
}

ALWAYS_INLINE static void* AlignedMalloc(size_t size, size_t alignment)
{
#ifdef _MSC_VER
  return _aligned_malloc(size, alignment);
#else
  // Unaligned sizes are slow on macOS.
#ifdef __APPLE__
  if (IsPow2(alignment))
    size = (size + alignment - 1) & ~(alignment - 1);
#endif
  void* ret = nullptr;
  return (posix_memalign(&ret, alignment, size) == 0) ? ret : nullptr;
#endif
}

ALWAYS_INLINE static void AlignedFree(void* ptr)
{
#ifdef _MSC_VER
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

namespace detail {
template<class T>
struct unique_aligned_ptr_deleter
{
  ALWAYS_INLINE void operator()(T* ptr) { Common::AlignedFree(ptr); }
};

template<class>
constexpr bool is_unbounded_array_v = false;
template<class T>
constexpr bool is_unbounded_array_v<T[]> = true;

template<class>
constexpr bool is_bounded_array_v = false;
template<class T, std::size_t N>
constexpr bool is_bounded_array_v<T[N]> = true;
} // namespace detail

template<class T>
using unique_aligned_ptr = std::unique_ptr<T, detail::unique_aligned_ptr_deleter<std::remove_extent_t<T>>>;

template<class T, class... Args>
  requires(std::is_unbounded_array_v<T>, std::is_trivially_default_constructible_v<std::remove_extent_t<T>>,
           std::is_trivially_destructible_v<std::remove_extent_t<T>>)
unique_aligned_ptr<T> make_unique_aligned(size_t alignment, size_t n)
{
  unique_aligned_ptr<T> ptr(
    static_cast<std::remove_extent_t<T>*>(AlignedMalloc(sizeof(std::remove_extent_t<T>) * n, alignment)));
  if (ptr)
    new (ptr.get()) std::remove_extent_t<T>[ n ]();
  return ptr;
}

template<class T, class... Args>
  requires(std::is_unbounded_array_v<T>, std::is_trivially_default_constructible_v<std::remove_extent_t<T>>,
           std::is_trivially_destructible_v<std::remove_extent_t<T>>)
unique_aligned_ptr<T> make_unique_aligned_for_overwrite(size_t alignment, size_t n)
{
  return unique_aligned_ptr<T>(
    static_cast<std::remove_extent_t<T>*>(AlignedMalloc(sizeof(std::remove_extent_t<T>) * n, alignment)));
}

} // namespace Common
