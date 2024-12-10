// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/gsvector.h"

#include <gtest/gtest.h>

TEST(Rectangle, AdjacentRectanglesNotIntersecting)
{
  GSVector4i r1(0, 0, 10, 10);
  GSVector4i r2(10, 10, 20, 20);
  ASSERT_FALSE(r1.rintersects(r2));
}

TEST(Rectangle, IntersectingRectanglesIntersecting)
{
  GSVector4i r1(0, 0, 10, 10);
  GSVector4i r2(9, 9, 14, 14);
  ASSERT_TRUE(r1.rintersects(r2));
  ASSERT_TRUE(r2.rintersects(r1));
}

TEST(Rectangle, PointContainedInRectangle)
{
  GSVector4i r1(0, 0, 10, 10);
  GSVector4i r2(5, 5, 6, 6);
  ASSERT_TRUE(r1.rcontains(r2));
}

TEST(Rectangle, PointOutsideRectangleNotContained)
{
  GSVector4i r1(0, 0, 10, 10);
  GSVector4i r2(10, 10, 11, 11);
  ASSERT_FALSE(r1.rcontains(r2));
}

TEST(Rectangle, RectangleSize)
{
  GSVector4i r(0, 0, 10, 10);
  ASSERT_EQ(r.width(), 10);
  ASSERT_EQ(r.height(), 10);
}

TEST(Rectangle, IncludeAfterInvalid)
{
  GSVector4i r(0, 0, 1, 1);
  GSVector4i r2(5, 5, 10, 10);
  GSVector4i ru(0, 0, 10, 10);
  ASSERT_TRUE(r.runion(r2).eq(ru));
}

TEST(Rectangle, EmptyRectangleHasNoExtents)
{
  GSVector4i r(0, 0, 0, 0);
  ASSERT_EQ(r.width(), 0);
  ASSERT_EQ(r.height(), 0);
  ASSERT_TRUE(r.rempty());
}

TEST(Rectangle, NonEmptyRectangleHasExtents)
{
  GSVector4i r(0, 0, 1, 1);
  ASSERT_EQ(r.width(), 1);
  ASSERT_EQ(r.height(), 1);
  ASSERT_FALSE(r.rempty());
}

TEST(Rectangle, RelationalOperators)
{
  GSVector4i r1(0, 0, 1, 1);
  GSVector4i r2(1, 1, 2, 2);

  ASSERT_TRUE(r1.eq(r1));
  ASSERT_TRUE(r1.lt32(r2).alltrue());
  ASSERT_TRUE(r2.eq(r2));
  ASSERT_TRUE(r2.gt32(r1).alltrue());
  ASSERT_FALSE(r2.lt32(r1).alltrue());
  ASSERT_FALSE(r1.eq(r2));
}

TEST(Rectangle, ValidRectangles)
{
  static constexpr GSVector4i cases[] = {
    GSVector4i::cxpr(1, 2, 3, 4),
    GSVector4i::cxpr(-5, -10, -1, -2),
    GSVector4i::cxpr(0, 0, 1, 1),
    GSVector4i::cxpr(100, 200, 300, 400),
    GSVector4i::cxpr(-1000, -2000, 500, 600),
    GSVector4i::cxpr(5, 10, 6, 12),
    GSVector4i::cxpr(-10, -20, -5, -15),
    GSVector4i::cxpr(-5, 0, 5, 10),
    GSVector4i::cxpr(-100, -200, 100, 200),
    GSVector4i::cxpr(-1, -2, 0, 1),
  };

  for (GSVector4i tcase : cases)
  {
    ASSERT_TRUE(tcase.rvalid());
    ASSERT_FALSE(tcase.rempty());
  }
}

TEST(Rectangle, InvalidRectangles)
{
  static constexpr GSVector4i cases[] = {
    // left < right but not top < bottom
    GSVector4i::cxpr(1, 4, 3, 2),
    GSVector4i::cxpr(-5, -2, -1, -10),
    GSVector4i::cxpr(0, 1, 1, 0),
    GSVector4i::cxpr(100, 400, 300, 200),
    GSVector4i::cxpr(-1000, 600, 500, -2000),
    GSVector4i::cxpr(5, 12, 6, 10),
    GSVector4i::cxpr(-10, -15, -5, -20),
    GSVector4i::cxpr(-5, 10, 5, 0),
    GSVector4i::cxpr(-100, 200, 100, -200),
    GSVector4i::cxpr(-1, 1, 0, -2),

    // not left < right but top < bottom
    GSVector4i::cxpr(3, 2, 1, 4),
    GSVector4i::cxpr(-1, -10, -5, -2),
    GSVector4i::cxpr(1, 0, 0, 1),
    GSVector4i::cxpr(300, 200, 100, 400),
    GSVector4i::cxpr(500, -2000, -1000, 600),
    GSVector4i::cxpr(6, 10, 5, 12),
    GSVector4i::cxpr(-5, -20, -10, -15),
    GSVector4i::cxpr(5, 0, -5, 10),
    GSVector4i::cxpr(100, -200, -100, 200),
    GSVector4i::cxpr(0, -2, -1, 1),
  };

  for (GSVector4i tcase : cases)
  {
    ASSERT_FALSE(tcase.rvalid());
    ASSERT_TRUE(tcase.rempty());
  }
}
