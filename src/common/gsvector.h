// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/intrin.h"

#include <cstring>

template<class T>
class GSVector2T
{
public:
  union
  {
    struct
    {
      T x, y;
    };
    struct
    {
      T r, g;
    };
    struct
    {
      T v[2];
    };
  };

  GSVector2T() = default;

  ALWAYS_INLINE constexpr GSVector2T(T x) : x(x), y(x) {}
  ALWAYS_INLINE constexpr GSVector2T(T x, T y) : x(x), y(y) {}
  ALWAYS_INLINE constexpr bool operator==(const GSVector2T& v) const { return std::memcmp(this, &v, sizeof(*this)) == 0; }
  ALWAYS_INLINE constexpr bool operator!=(const GSVector2T& v) const { return std::memcmp(this, &v, sizeof(*this)) != 0; }
  ALWAYS_INLINE constexpr GSVector2T operator*(const GSVector2T& v) const { return {x * v.x, y * v.y}; }
  ALWAYS_INLINE constexpr GSVector2T operator/(const GSVector2T& v) const { return {x / v.x, y / v.y}; }
};

using GSVector2 = GSVector2T<float>;
using GSVector2i = GSVector2T<s32>;

#if defined(CPU_ARCH_SSE)
#include "common/gsvector_sse.h"
#elif defined(CPU_ARCH_NEON)
#include "common/gsvector_neon.h"
#else
#include "common/gsvector_nosimd.h"
#endif
