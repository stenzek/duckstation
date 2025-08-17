// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "types.h"
#include <cmath>

// From https://github.com/nicolausYes/easing-functions/blob/master/src/easing.cpp

namespace Easing {
inline constexpr float pi = 3.1415926545f;

template<typename T>
ALWAYS_INLINE_RELEASE T InSine(T t)
{
  return std::sin(1.5707963f * t);
}

template<typename T>
ALWAYS_INLINE_RELEASE T OutSine(T t)
{
  return 1 + std::sin(1.5707963f * (--t));
}

template<typename T>
ALWAYS_INLINE_RELEASE T InOutSine(T t)
{
  return 0.5f * (1 + std::sin(3.1415926f * (t - 0.5f)));
}

template<typename T>
ALWAYS_INLINE_RELEASE T InQuad(T t)
{
  return t * t;
}

template<typename T>
ALWAYS_INLINE_RELEASE T OutQuad(T t)
{
  return t * (2 - t);
}

template<typename T>
ALWAYS_INLINE_RELEASE T InOutQuad(T t)
{
  return t < 0.5f ? 2 * t * t : t * (4 - 2 * t) - 1;
}

template<typename T>
ALWAYS_INLINE_RELEASE T InCubic(T t)
{
  return t * t * t;
}

template<typename T>
ALWAYS_INLINE_RELEASE T OutCubic(T t)
{
  return 1 + (--t) * t * t;
}

template<typename T>
ALWAYS_INLINE_RELEASE T InOutCubic(T t)
{
  return t < 0.5f ? 4 * t * t * t : 1 + (--t) * (2 * (--t)) * (2 * t);
}

template<typename T>
ALWAYS_INLINE_RELEASE T InQuart(T t)
{
  t *= t;
  return t * t;
}

template<typename T>
ALWAYS_INLINE_RELEASE T OutQuart(T t)
{
  t = (--t) * t;
  return 1 - t * t;
}

template<typename T>
ALWAYS_INLINE_RELEASE T InOutQuart(T t)
{
  if (t < 0.5)
  {
    t *= t;
    return 8 * t * t;
  }
  else
  {
    t = (--t) * t;
    return 1 - 8 * t * t;
  }
}

template<typename T>
ALWAYS_INLINE_RELEASE T InQuint(T t)
{
  T t2 = t * t;
  return t * t2 * t2;
}

template<typename T>
ALWAYS_INLINE_RELEASE T OutQuint(T t)
{
  T t2 = (--t) * t;
  return 1 + t * t2 * t2;
}

template<typename T>
ALWAYS_INLINE_RELEASE T InOutQuint(T t)
{
  T t2;
  if (t < 0.5)
  {
    t2 = t * t;
    return 16 * t * t2 * t2;
  }
  else
  {
    t2 = (--t) * t;
    return 1 + 16 * t * t2 * t2;
  }
}

template<typename T>
ALWAYS_INLINE_RELEASE T InExpo(T t)
{
  return (std::pow(static_cast<T>(2), static_cast<T>(8) * t) - static_cast<T>(1)) / static_cast<T>(255);
}

template<typename T>
ALWAYS_INLINE_RELEASE T OutExpo(T t)
{
  return static_cast<T>(1) - std::pow(static_cast<T>(2), static_cast<T>(-8) * t);
}

template<typename T>
ALWAYS_INLINE_RELEASE T InOutExpo(T t)
{
  if (t < 0.5f)
  {
    return (std::pow(2, 16 * t) - 1) / 510;
  }
  else
  {
    return 1 - 0.5f * std::pow(2, -16 * (t - 0.5f));
  }
}

template<typename T>
ALWAYS_INLINE_RELEASE T InCirc(T t)
{
  return 1 - std::sqrt(1 - t);
}

template<typename T>
ALWAYS_INLINE_RELEASE T OutCirc(T t)
{
  return std::sqrt(t);
}

template<typename T>
ALWAYS_INLINE_RELEASE T InOutCirc(T t)
{
  if (t < 0.5f)
  {
    return (1 - std::sqrt(1 - 2 * t)) * 0.5f;
  }
  else
  {
    return (1 + std::sqrt(2 * t - 1)) * 0.5f;
  }
}

template<typename T>
ALWAYS_INLINE_RELEASE T InBack(T t)
{
  return t * t * (2.70158f * t - 1.70158f);
}

template<typename T>
ALWAYS_INLINE_RELEASE T OutBack(T t)
{
  t -= 1;
  return 1 + t * t * (2.70158f * t + 1.70158f);
}

template<typename T>
ALWAYS_INLINE_RELEASE T InOutBack(T t)
{
  if (t < 0.5f)
  {
    return t * t * (7 * t - 2.5f) * 2;
  }
  else
  {
    return 1 + (--t) * t * 2 * (7 * t + 2.5f);
  }
}

template<typename T>
ALWAYS_INLINE_RELEASE T InElastic(T t)
{
  T t2 = t * t;
  return t2 * t2 * std::sin(t * pi * 4.5f);
}

template<typename T>
ALWAYS_INLINE_RELEASE T OutElastic(T t)
{
  T t2 = (t - 1) * (t - 1);
  return 1 - t2 * t2 * std::cos(t * pi * 4.5f);
}

template<typename T>
ALWAYS_INLINE_RELEASE T InOutElastic(T t)
{
  T t2;
  if (t < 0.45f)
  {
    t2 = t * t;
    return 8 * t2 * t2 * std::sin(t * pi * 9);
  }
  else if (t < 0.55f)
  {
    return 0.5f + 0.75f * std::sin(t * pi * 4);
  }
  else
  {
    t2 = (t - 1) * (t - 1);
    return 1 - 8 * t2 * t2 * std::sin(t * pi * 9);
  }
}

template<typename T>
ALWAYS_INLINE_RELEASE T InBounce(T t)
{
  return std::pow(2, 6 * (t - 1)) * std::abs(sin(t * pi * 3.5f));
}

template<typename T>
ALWAYS_INLINE_RELEASE T OutBounce(T t)
{
  return 1 - std::pow(2, -6 * t) * std::abs(std::cos(t * pi * 3.5f));
}

template<typename T>
ALWAYS_INLINE_RELEASE T InOutBounce(T t)
{
  if (t < 0.5f)
  {
    return 8 * std::pow(2, 8 * (t - 1)) * std::abs(std::sin(t * pi * 7));
  }
  else
  {
    return 1 - 8 * std::pow(2, -8 * t) * std::abs(std::sin(t * pi * 7));
  }
}

} // namespace Easing
