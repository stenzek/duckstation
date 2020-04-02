#include "common/rectangle.h"
#include <gtest/gtest.h>

using Common::Rectangle;
using IntRectangle = Rectangle<int>;

TEST(Rectangle, DefaultConstructorIsInvalid)
{
  IntRectangle r;
  ASSERT_FALSE(r.Valid());
}

TEST(Rectangle, AdjacentRectanglesNotIntersecting)
{
  IntRectangle r1(0, 0, 10, 10);
  IntRectangle r2(10, 10, 20, 20);
  ASSERT_FALSE(r1.Intersects(r2));
}

TEST(Rectangle, IntersectingRectanglesIntersecting)
{
  IntRectangle r1(0, 0, 10, 10);
  IntRectangle r2(9, 9, 4, 4);
  ASSERT_TRUE(r1.Intersects(r2));
  ASSERT_TRUE(r2.Intersects(r1));
}

TEST(Rectangle, PointContainedInRectangle)
{
  IntRectangle r1(0, 0, 10, 10);
  ASSERT_TRUE(r1.Contains(5, 5));
}

TEST(Rectangle, PointOutsideRectangleNotContained)
{
  IntRectangle r1(0, 0, 10, 10);
  ASSERT_FALSE(r1.Contains(10, 10));
}

TEST(Rectangle, RectangleSize)
{
  IntRectangle r(0, 0, 10, 10);
  ASSERT_EQ(r.GetWidth(), 10);
  ASSERT_EQ(r.GetHeight(), 10);
}

TEST(Rectangle, IncludeAfterInvalid)
{
  IntRectangle r;
  IntRectangle r2(0, 0, 10, 10);
  ASSERT_FALSE(r.Valid());
  ASSERT_TRUE(r2.Valid());
  r.Include(r2);
  ASSERT_EQ(r, r2);
}

TEST(Rectangle, EmptyRectangleHasNoExtents)
{
  IntRectangle r(0, 0, 0, 0);
  ASSERT_FALSE(r.HasExtents());
}

TEST(Rectangle, NonEmptyRectangleHasExtents)
{
  IntRectangle r(0, 0, 1, 1);
  ASSERT_TRUE(r.HasExtents());
}

TEST(Rectangle, RelationalOperators)
{
  IntRectangle r1(0, 0, 1, 1);
  IntRectangle r2(1, 1, 2, 2);

  ASSERT_EQ(r1, r1);
  ASSERT_LE(r1, r1);
  ASSERT_LE(r1, r2);
  ASSERT_LT(r1, r2);
  ASSERT_EQ(r2, r2);
  ASSERT_GE(r2, r1);
  ASSERT_GT(r2, r1);
  ASSERT_NE(r1, r2);
}

