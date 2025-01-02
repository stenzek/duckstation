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
  constexpr GSMatrix4x4() = default;
  constexpr GSMatrix4x4(float e00, float e01, float e02, float e03, float e10, float e11, float e12, float e13,
                        float e20, float e21, float e22, float e23, float e30, float e31, float e32, float e33)
  {
    E[0][0] = e00;
    E[0][1] = e01;
    E[0][2] = e02;
    E[0][3] = e03;
    E[1][0] = e10;
    E[1][1] = e11;
    E[1][2] = e12;
    E[1][3] = e13;
    E[2][0] = e20;
    E[2][1] = e21;
    E[2][2] = e22;
    E[2][3] = e23;
    E[3][0] = e30;
    E[3][1] = e31;
    E[3][2] = e32;
    E[3][3] = e33;
  }

  constexpr GSMatrix4x4(const GSMatrix2x2& m)
  {
    E[0][0] = m.E[0][0];
    E[0][1] = m.E[0][1];
    E[0][2] = 0.0f;
    E[0][3] = 0.0f;
    E[1][0] = m.E[1][0];
    E[1][1] = m.E[1][1];
    E[1][2] = 0.0f;
    E[1][3] = 0.0f;
    E[2][0] = 0.0f;
    E[2][1] = 0.0f;
    E[2][2] = 1.0f;
    E[2][3] = 0.0f;
    E[3][0] = 0.0f;
    E[3][1] = 0.0f;
    E[3][2] = 0.0f;
    E[3][3] = 1.0f;
  }

  GSMatrix4x4 operator*(const GSMatrix4x4& m) const;
  GSMatrix4x4& operator*=(const GSMatrix4x4& m);

  GSVector4 operator*(const GSVector4& v) const
  {
    return GSVector4(row(0).dot(v), row(1).dot(v), row(2).dot(v), row(3).dot(v));
  }

  static constexpr GSMatrix4x4 Identity()
  {
    return GSMatrix4x4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
  }

  static GSMatrix4x4 RotationX(float angle_in_radians);
  static GSMatrix4x4 RotationY(float angle_in_radians);
  static GSMatrix4x4 RotationZ(float angle_in_radians);
  static GSMatrix4x4 Translation(float x, float y, float z);

  static GSMatrix4x4 OffCenterOrthographicProjection(float left, float top, float right, float bottom, float zNear,
                                                     float zFar);
  static GSMatrix4x4 OffCenterOrthographicProjection(float width, float height, float zNear, float zFar);

  GSVector4 row(size_t i) const { return GSVector4::load<true>(&E[i][0]); }
  GSVector4 col(size_t i) const { return GSVector4(E[0][i], E[1][i], E[2][i], E[3][i]); }

  void set_row(size_t i, GSVector4 row) { GSVector4::store<true>(&E[i][0], row); }
  void set_col(size_t i, GSVector4 col)
  {
    E[0][i] = col.x;
    E[1][i] = col.y;
    E[2][i] = col.z;
    E[3][i] = col.w;
  }

  GSMatrix4x4 invert() const;

  void store(void* m);

  float E[4][4];
};
