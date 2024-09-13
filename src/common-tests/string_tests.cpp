// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/string_util.h"

#include <gtest/gtest.h>

TEST(StringUtil, Ellipsise)
{
  ASSERT_EQ(StringUtil::Ellipsise("HelloWorld", 6, "..."), "Hel...");
  ASSERT_EQ(StringUtil::Ellipsise("HelloWorld", 7, ".."), "Hello..");
  ASSERT_EQ(StringUtil::Ellipsise("HelloWorld", 20, ".."), "HelloWorld");
  ASSERT_EQ(StringUtil::Ellipsise("", 20, "..."), "");
  ASSERT_EQ(StringUtil::Ellipsise("Hello", 10, "..."), "Hello");
}

TEST(StringUtil, EllipsiseInPlace)
{
  std::string s;
  s = "HelloWorld";
  StringUtil::EllipsiseInPlace(s, 6, "...");
  ASSERT_EQ(s, "Hel...");
  s = "HelloWorld";
  StringUtil::EllipsiseInPlace(s, 7, "..");
  ASSERT_EQ(s, "Hello..");
  s = "HelloWorld";
  StringUtil::EllipsiseInPlace(s, 20, "..");
  ASSERT_EQ(s, "HelloWorld");
  s = "";
  StringUtil::EllipsiseInPlace(s, 20, "...");
  ASSERT_EQ(s, "");
  s = "Hello";
  StringUtil::EllipsiseInPlace(s, 10, "...");
  ASSERT_EQ(s, "Hello");
}
