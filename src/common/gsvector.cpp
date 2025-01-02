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

ALWAYS_INLINE static void MatrixMult4x4(GSMatrix4x4& res, const GSMatrix4x4& lhs, const GSMatrix4x4& rhs)
{
  // This isn't speedy by any means, but it's not hot code either.
#define MultRC(rw, cl)                                                                                                 \
  lhs.E[rw][0] * rhs.E[0][cl] + lhs.E[rw][1] * rhs.E[1][cl] + lhs.E[rw][2] * rhs.E[2][cl] + lhs.E[rw][3] * rhs.E[3][cl]

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
}

GSMatrix4x4 GSMatrix4x4::operator*(const GSMatrix4x4& m) const
{
  GSMatrix4x4 res;
  MatrixMult4x4(res, *this, m);
  return res;
}

GSMatrix4x4& GSMatrix4x4::operator*=(const GSMatrix4x4& m)
{
  const GSMatrix4x4 copy(*this);
  MatrixMult4x4(*this, copy, m);
  return *this;
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

GSMatrix4x4 GSMatrix4x4::Translation(float x, float y, float z)
{
  return GSMatrix4x4(1.0f, 0.0f, 0.0f, x, 0.0f, 1.0f, 0.0f, y, 0.0f, 0.0f, 1.0f, z, 0.0f, 0.0f, 0.0f, 1.0f);
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

GSMatrix4x4 GSMatrix4x4::invert() const
{
  GSMatrix4x4 ret;

  float v0 = E[2][0] * E[3][1] - E[2][1] * E[3][0];
  float v1 = E[2][0] * E[3][2] - E[2][2] * E[3][0];
  float v2 = E[2][0] * E[3][3] - E[2][3] * E[3][0];
  float v3 = E[2][1] * E[3][2] - E[2][2] * E[3][1];
  float v4 = E[2][1] * E[3][3] - E[2][3] * E[3][1];
  float v5 = E[2][2] * E[3][3] - E[2][3] * E[3][2];

  const float t00 = +(v5 * E[1][1] - v4 * E[1][2] + v3 * E[1][3]);
  const float t10 = -(v5 * E[1][0] - v2 * E[1][2] + v1 * E[1][3]);
  const float t20 = +(v4 * E[1][0] - v2 * E[1][1] + v0 * E[1][3]);
  const float t30 = -(v3 * E[1][0] - v1 * E[1][1] + v0 * E[1][2]);

  const float inv_det = 1.0f / (t00 * E[0][0] + t10 * E[0][1] + t20 * E[0][2] + t30 * E[0][3]);

  ret.E[0][0] = t00 * inv_det;
  ret.E[1][0] = t10 * inv_det;
  ret.E[2][0] = t20 * inv_det;
  ret.E[3][0] = t30 * inv_det;

  ret.E[0][1] = -(v5 * E[0][1] - v4 * E[0][2] + v3 * E[0][3]) * inv_det;
  ret.E[1][1] = +(v5 * E[0][0] - v2 * E[0][2] + v1 * E[0][3]) * inv_det;
  ret.E[2][1] = -(v4 * E[0][0] - v2 * E[0][1] + v0 * E[0][3]) * inv_det;
  ret.E[3][1] = +(v3 * E[0][0] - v1 * E[0][1] + v0 * E[0][2]) * inv_det;

  v0 = E[1][0] * E[3][1] - E[1][1] * E[3][0];
  v1 = E[1][0] * E[3][2] - E[1][2] * E[3][0];
  v2 = E[1][0] * E[3][3] - E[1][3] * E[3][0];
  v3 = E[1][1] * E[3][2] - E[1][2] * E[3][1];
  v4 = E[1][1] * E[3][3] - E[1][3] * E[3][1];
  v5 = E[1][2] * E[3][3] - E[1][3] * E[3][2];

  ret.E[0][2] = +(v5 * E[0][1] - v4 * E[0][2] + v3 * E[0][3]) * inv_det;
  ret.E[1][2] = -(v5 * E[0][0] - v2 * E[0][2] + v1 * E[0][3]) * inv_det;
  ret.E[2][2] = +(v4 * E[0][0] - v2 * E[0][1] + v0 * E[0][3]) * inv_det;
  ret.E[3][2] = -(v3 * E[0][0] - v1 * E[0][1] + v0 * E[0][2]) * inv_det;

  v0 = E[2][1] * E[1][0] - E[2][0] * E[1][1];
  v1 = E[2][2] * E[1][0] - E[2][0] * E[1][2];
  v2 = E[2][3] * E[1][0] - E[2][0] * E[1][3];
  v3 = E[2][2] * E[1][1] - E[2][1] * E[1][2];
  v4 = E[2][3] * E[1][1] - E[2][1] * E[1][3];
  v5 = E[2][3] * E[1][2] - E[2][2] * E[1][3];

  ret.E[0][3] = -(v5 * E[0][1] - v4 * E[0][2] + v3 * E[0][3]) * inv_det;
  ret.E[1][3] = +(v5 * E[0][0] - v2 * E[0][2] + v1 * E[0][3]) * inv_det;
  ret.E[2][3] = -(v4 * E[0][0] - v2 * E[0][1] + v0 * E[0][3]) * inv_det;
  ret.E[3][3] = +(v3 * E[0][0] - v1 * E[0][1] + v0 * E[0][2]) * inv_det;

  return ret;
}

void GSMatrix4x4::store(void* m)
{
  std::memcpy(m, &E[0][0], sizeof(E));
}
