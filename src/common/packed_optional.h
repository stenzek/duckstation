// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

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
