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
  return GSVector2::load(&E[i][0]);
}

GSVector2 GSMatrix2x2::col(size_t i) const
{
  return GSVector2(E[0][i], E[1][i]);
}

void GSMatrix2x2::store(void* m)
{
  std::memcpy(m, E, sizeof(E));
}
