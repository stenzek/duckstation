// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <array>
#include <optional>
#include <type_traits>

/// PackedOptional<T, Bit> is an optional value whose has-value flag lives in an external bitmask, shared with other
/// PackedOptionals in the same struct. This avoids the per-member bool and alignment padding of std::optional<T>.
///
/// The bitmask is passed to every accessor that needs it, and can be any unsigned integer type wide enough for Bit:
///   u32 mask = 0;
///   PackedOptional<u16, 0> a;
///   PackedOptional<float, 1> b;
///   a.set(mask, 123);
///   if (a.has_value(mask)) ...
///   const std::optional<float> b_opt = b.get(mask);
template<typename T, u32 Bit>
class PackedOptional
{
public:
  using value_type = T;

  /// Returns true if the value is present in \p mask.
  template<typename M>
  ALWAYS_INLINE constexpr bool has_value(M mask) const
  {
    return ((mask & bit<M>()) != 0);
  }

  /// Returns the stored value. Only meaningful if has_value() is true.
  ALWAYS_INLINE constexpr const T& value() const { return m_value; }

  /// Returns the stored value if present, otherwise \p default_value.
  template<typename M>
  ALWAYS_INLINE constexpr T value_or(M mask, const T& default_value) const
  {
    return has_value(mask) ? m_value : default_value;
  }

  /// Returns the value as a std::optional.
  template<typename M>
  ALWAYS_INLINE constexpr std::optional<T> get(M mask) const
  {
    return has_value(mask) ? std::optional<T>(m_value) : std::nullopt;
  }

  /// Stores a value and sets the bit in \p mask.
  template<typename M>
  ALWAYS_INLINE constexpr void set(M& mask, const T& value)
  {
    m_value = value;
    mask |= bit<M>();
  }

  /// Stores the value if present, otherwise clears it.
  template<typename M>
  ALWAYS_INLINE constexpr void set(M& mask, const std::optional<T>& value)
  {
    if (value.has_value())
      set(mask, value.value());
    else
      reset(mask);
  }

  /// Clears the bit in \p mask, and resets the stored value.
  template<typename M>
  ALWAYS_INLINE constexpr void reset(M& mask)
  {
    m_value = T{};
    mask &= static_cast<M>(~bit<M>());
  }

private:
  template<typename M>
  ALWAYS_INLINE static constexpr M bit()
  {
    static_assert(std::is_unsigned_v<M>, "Mask must be an unsigned integer type");
    static_assert(Bit < sizeof(M) * 8, "Bit index does not fit in mask type");
    return static_cast<M>(static_cast<M>(1) << Bit);
  }

  T m_value{};
};

/// PackedOptionalArray<T, N> is a fixed-size array of optional values, indexed at runtime, whose has-value flags are
/// packed into one bitmask stored with the array. This avoids the per-element bool and alignment padding of
/// std::array<std::optional<T>, N>.
///   PackedOptionalArray<u16, 4> arr;
///   arr.set(2, 123);
///   if (arr.has_value(2)) ...
///   const std::optional<u16> v = arr.get(3);
template<typename T, u32 N>
class PackedOptionalArray
{
  static_assert(N > 0 && N <= 64, "PackedOptionalArray holds between 1 and 64 values");

public:
  using value_type = T;
  using mask_type =
    std::conditional_t<(N <= 8), u8, std::conditional_t<(N <= 16), u16, std::conditional_t<(N <= 32), u32, u64>>>;

  /// Returns the number of elements, present or not.
  ALWAYS_INLINE static constexpr u32 size() { return N; }

  /// Returns true if the value at \p index is present.
  ALWAYS_INLINE constexpr bool has_value(u32 index) const { return ((m_mask & bit(index)) != 0); }

  /// Returns true if any value is present.
  ALWAYS_INLINE constexpr bool any() const { return (m_mask != 0); }

  /// Returns the value at \p index. Only meaningful if has_value() is true.
  ALWAYS_INLINE constexpr const T& value(u32 index) const { return m_values[index]; }

  /// Returns the value at \p index if present, otherwise \p default_value.
  ALWAYS_INLINE constexpr T value_or(u32 index, const T& default_value) const
  {
    return has_value(index) ? m_values[index] : default_value;
  }

  /// Returns the value at \p index as a std::optional.
  ALWAYS_INLINE constexpr std::optional<T> get(u32 index) const
  {
    return has_value(index) ? std::optional<T>(m_values[index]) : std::nullopt;
  }

  /// Stores a value at \p index.
  ALWAYS_INLINE constexpr void set(u32 index, const T& value)
  {
    m_values[index] = value;
    m_mask |= bit(index);
  }

  /// Stores the value at \p index if present, otherwise clears it.
  ALWAYS_INLINE constexpr void set(u32 index, const std::optional<T>& value)
  {
    if (value.has_value())
      set(index, value.value());
    else
      reset(index);
  }

  /// Clears the value at \p index.
  ALWAYS_INLINE constexpr void reset(u32 index)
  {
    m_values[index] = T{};
    m_mask &= static_cast<mask_type>(~bit(index));
  }

  /// Clears every value.
  ALWAYS_INLINE constexpr void clear()
  {
    m_values = {};
    m_mask = 0;
  }

private:
  ALWAYS_INLINE static constexpr mask_type bit(u32 index)
  {
    return static_cast<mask_type>(static_cast<mask_type>(1) << index);
  }

  std::array<T, N> m_values{};
  mask_type m_mask = 0;
};
