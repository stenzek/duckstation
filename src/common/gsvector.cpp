// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gsvector.h"

#include <cmath>

GSMatrix2x2::GSMatrix2x2(float e00, float e01, float e10, float e11)
{
  E[0][0] = e00;
  E[0][1] = e01;
  E[1][0] = e10;
  E[1][1] = e11;
}

GSMatrix2x2 GSMatrix2x2::operator*(const GSMatrix2x2& m) const
{
  GSMatrix2x2 ret;
  ret.E[0][0] = E[0][0] * m.E[0][0] + E[0][1] * m.E[1][0];
  ret.E[0][1] = E[0][0] * m.E[0][1] + E[0][1] * m.E[1][1];
  ret.E[1][0] = E[1][0] * m.E[0][0] + E[1][1] * m.E[1][0];
  ret.E[1][1] = E[1][0] * m.E[0][1] + E[1][1] * m.E[1][1];
  return ret;
}

GSVector2 GSMatrix2x2::operator*(const GSVector2& v) const
{
  return GSVector2(row(0).dot(v), row(1).dot(v));
}

GSMatrix2x2 GSMatrix2x2::Identity()
{
  GSMatrix2x2 ret;
  ret.E[0][0] = 1.0f;
  ret.E[0][1] = 0.0f;
  ret.E[1][0] = 0.0f;
  ret.E[1][1] = 1.0f;
  return ret;
}

GSMatrix2x2 GSMatrix2x2::Rotation(float angle_in_radians)
{
  const float sin_angle = std::sin(angle_in_radians);
  const float cos_angle = std::cos(angle_in_radians);

  GSMatrix2x2 ret;
  ret.E[0][0] = cos_angle;
  ret.E[0][1] = -sin_angle;
  ret.E[1][0] = sin_angle;
  ret.E[1][1] = cos_angle;
  return ret;
}

GSVector2 GSMatrix2x2::row(size_t i) const
{
  return GSVector2::load<true>(&E[i][0]);
}

GSVector2 GSMatrix2x2::col(size_t i) const
{
  return GSVector2(E[0][i], E[1][i]);
}

void GSMatrix2x2::store(void* m)
{
  std::memcpy(m, E, sizeof(E));
}

GSMatrix4x4::GSMatrix4x4(float e00, float e01, float e02, float e03, float e10, float e11, float e12, float e13,
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

 GSMatrix4x4::GSMatrix4x4(const GSMatrix2x2& m)
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

GSMatrix4x4 GSMatrix4x4::operator*(const GSMatrix4x4& m) const
{
  // This isn't speedy by any means, but it's not hot code either.
  GSMatrix4x4 res;

#define MultRC(rw, cl) E[rw][0] * m.E[0][cl] + E[rw][1] * m.E[1][cl] + E[rw][2] * m.E[2][cl] + E[rw][3] * m.E[3][cl]

  res.E[0][0] = MultRC(0, 0);
  res.E[0][1] = MultRC(0, 1);
  res.E[0][2] = MultRC(0, 2);
  res.E[0][3] = MultRC(0, 3);
  res.E[1][0] = MultRC(1, 0);
  res.E[1][1] = MultRC(1, 1);
  res.E[1][2] = MultRC(1, 2);
  res.E[1][3] = MultRC(1, 3);
  res.E[2][0] = MultRC(2, 0);
  res.E[2][1] = MultRC(2, 1);
  res.E[2][2] = MultRC(2, 2);
  res.E[2][3] = MultRC(2, 3);
  res.E[3][0] = MultRC(3, 0);
  res.E[3][1] = MultRC(3, 1);
  res.E[3][2] = MultRC(3, 2);
  res.E[3][3] = MultRC(3, 3);

#undef MultRC

  return res;
}

GSVector4 GSMatrix4x4::operator*(const GSVector4& v) const
{
  const GSVector4 r0 = row(0);
  const GSVector4 r1 = row(1);
  const GSVector4 r2 = row(2);
  const GSVector4 r3 = row(4);

  return GSVector4(r0.dot(v), r1.dot(v), r2.dot(v), r3.dot(v));
}

GSMatrix4x4 GSMatrix4x4::Identity()
{
  GSMatrix4x4 res;

#define MultRC(rw, cl) E[rw][0] * m.E[0][cl] + E[rw][1] * m.E[1][cl] + E[rw][2] * m.E[2][cl] + E[rw][3] * m.E[3][cl]

  res.E[0][0] = 1.0f;
  res.E[0][1] = 0.0f;
  res.E[0][2] = 0.0f;
  res.E[0][3] = 0.0f;
  res.E[1][0] = 0.0f;
  res.E[1][1] = 1.0f;
  res.E[1][2] = 0.0f;
  res.E[1][3] = 0.0f;
  res.E[2][0] = 0.0f;
  res.E[2][1] = 0.0f;
  res.E[2][2] = 1.0f;
  res.E[2][3] = 0.0f;
  res.E[3][0] = 0.0f;
  res.E[3][1] = 0.0f;
  res.E[3][2] = 0.0f;
  res.E[3][3] = 1.0f;

  return res;
}

GSMatrix4x4 GSMatrix4x4::RotationX(float angle_in_radians)
{
  const float sin_angle = std::sin(angle_in_radians);
  const float cos_angle = std::cos(angle_in_radians);

  return GSMatrix4x4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, cos_angle, -sin_angle, 0.0f, 0.0f, sin_angle, cos_angle, 0.0f, 0.0f,
                     0.0f, 0.0f, 1.0f);
}

GSMatrix4x4 GSMatrix4x4::RotationY(float angle_in_radians)
{
  const float sin_angle = std::sin(angle_in_radians);
  const float cos_angle = std::cos(angle_in_radians);

  return GSMatrix4x4(cos_angle, 0.0f, sin_angle, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -sin_angle, 0.0f, cos_angle, 0.0f, 0.0f,
                     0.0f, 0.0f, 1.0f);
}

GSMatrix4x4 GSMatrix4x4::RotationZ(float angle_in_radians)
{
  const float sin_angle = std::sin(angle_in_radians);
  const float cos_angle = std::cos(angle_in_radians);

  return GSMatrix4x4(cos_angle, -sin_angle, 0.0f, 0.0f, sin_angle, cos_angle, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, 1.0f);
}

GSMatrix4x4 GSMatrix4x4::OffCenterOrthographicProjection(float left, float top, float right, float bottom, float zNear,
                                                         float zFar)
{
  return GSMatrix4x4(2.0f / (right - left), 0.0f, 0.0f, (left + right) / (left - right), 0.0f, 2.0f / (top - bottom),
                     0.0f, (top + bottom) / (bottom - top), 0.0f, 0.0f, 1.0f / (zNear - zFar), zNear / (zNear - zFar),
                     0.0f, 0.0f, 0.0f, 1.0f);
}

GSMatrix4x4 GSMatrix4x4::OffCenterOrthographicProjection(float width, float height, float zNear, float zFar)
{
  return OffCenterOrthographicProjection(0.0f, 0.0f, width, height, zNear, zFar);
}

GSVector4 GSMatrix4x4::row(size_t i) const
{
  return GSVector4::load<true>(&E[i][0]);
}

GSVector4 GSMatrix4x4::col(size_t i) const
{
  return GSVector4(E[0][i], E[1][i], E[2][i], E[3][i]);
}

void GSMatrix4x4::store(void* m)
{
  std::memcpy(m, &E[0][0], sizeof(E));
}
