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

  alignas(8) float E[2][2];
};

class alignas(VECTOR_ALIGNMENT) GSMatrix4x4
{
public:
  GSMatrix4x4() = default;
  GSMatrix4x4(float e00, float e01, float e02, float e03, float e10, float e11, float e12, float e13, float e20,
              float e21, float e22, float e23, float e30, float e31, float e32, float e33);
  GSMatrix4x4(const GSMatrix2x2& m);

  GSMatrix4x4 operator*(const GSMatrix4x4& m) const;
  GSMatrix4x4& operator*=(const GSMatrix4x4& m);

  GSVector4 operator*(const GSVector4& v) const;

  static GSMatrix4x4 Identity();

  static GSMatrix4x4 RotationX(float angle_in_radians);
  static GSMatrix4x4 RotationY(float angle_in_radians);
  static GSMatrix4x4 RotationZ(float angle_in_radians);
  static GSMatrix4x4 Translation(float x, float y, float z);

  static GSMatrix4x4 OffCenterOrthographicProjection(float left, float top, float right, float bottom, float zNear,
                                                     float zFar);
  static GSMatrix4x4 OffCenterOrthographicProjection(float width, float height, float zNear, float zFar);

  GSVector4 row(size_t i) const;
  GSVector4 col(size_t i) const;

  GSMatrix4x4 Invert() const;

  void store(void* m);

  float E[4][4];
};
