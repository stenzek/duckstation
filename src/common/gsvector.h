// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

//
// Lightweight wrapper over native SIMD types for cross-platform vector code.
//

#pragma once

#include "common/intrin.h"

#if defined(CPU_ARCH_SSE)
#include "common/gsvector_sse.h"
#elif defined(CPU_ARCH_NEON)
#include "common/gsvector_neon.h"
#else
#include "common/gsvector_nosimd.h"
#endif

class GSMatrix2x2
{
public:
  GSMatrix2x2() = default;
  GSMatrix2x2(float e00, float e01, float e10, float e11);

  GSMatrix2x2 operator*(const GSMatrix2x2& m) const;

  GSVector2 operator*(const GSVector2& v) const;

  static GSMatrix2x2 Identity();
  static GSMatrix2x2 Rotation(float angle_in_radians);

  GSVector2 row(size_t i) const;
  GSVector2 col(size_t i) const;

  void store(void* m);

  float E[2][2];
};
