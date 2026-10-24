// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/packed_optional.h"

#include <gtest/gtest.h>

namespace {
enum class TestEnum : u8
{
  A,
  B,
  C,
};

struct TestStruct
{
  u32 mask = 0;
  PackedOptional<u32, 0> a;
  PackedOptional<float, 1> b;
  PackedOptional<s16, 2> c;
  PackedOptional<TestEnum, 3> d;
  PackedOptional<bool, 4> e;
};
} // namespace

static_assert(sizeof(PackedOptional<u32, 0>) == sizeof(u32));
static_assert(sizeof(PackedOptional<u8, 0>) == sizeof(u8));
static_assert(std::is_trivially_copyable_v<PackedOptional<u32, 0>>);
static_assert(sizeof(TestStruct) == 16);

// Compile-time checks.
static_assert([] {
  TestStruct s;
  return !s.a.has_value(s.mask) && !s.a.get(s.mask).has_value() && s.a.value_or(s.mask, 7u) == 7u;
}());
static_assert([] {
  TestStruct s;
  s.c.set(s.mask, static_cast<s16>(-5));
  s.d.set(s.mask, TestEnum::C);
  return s.mask == ((1u << 2) | (1u << 3)) && s.c.value() == -5 && s.d.get(s.mask) == TestEnum::C &&
         !s.a.has_value(s.mask);
}());
static_assert([] {
  TestStruct s;
  s.a.set(s.mask, 100u);
  s.b.set(s.mask, 1.5f);
  s.a.reset(s.mask);
  return s.mask == (1u << 1) && s.a.value() == 0u && s.b.value() == 1.5f;
}());
static_assert([] {
  TestStruct s;
  s.e.set(s.mask, std::optional<bool>(false));
  const bool present = s.e.has_value(s.mask) && !s.e.value();
  s.e.set(s.mask, std::optional<bool>());
  return present && s.mask == 0u;
}());
static_assert([] {
  u8 mask8 = 0;
  PackedOptional<u16, 7> top8;
  top8.set(mask8, static_cast<u16>(0xBEEF));
  return mask8 == 0x80u && top8.value() == 0xBEEF;
}());

TEST(PackedOptional, DefaultEmpty)
{
  TestStruct s;
  EXPECT_FALSE(s.a.has_value(s.mask));
  EXPECT_FALSE(s.b.has_value(s.mask));
  EXPECT_FALSE(s.c.has_value(s.mask));
  EXPECT_FALSE(s.d.has_value(s.mask));
  EXPECT_FALSE(s.e.has_value(s.mask));
  EXPECT_EQ(s.a.value(), 0u);
  EXPECT_EQ(s.a.get(s.mask), std::nullopt);
}

TEST(PackedOptional, SetSetsOnlyOwnBit)
{
  TestStruct s;
  s.c.set(s.mask, static_cast<s16>(-5));
  EXPECT_EQ(s.mask, 1u << 2);
  EXPECT_TRUE(s.c.has_value(s.mask));
  EXPECT_EQ(s.c.value(), -5);
  EXPECT_FALSE(s.a.has_value(s.mask));
  EXPECT_FALSE(s.b.has_value(s.mask));
  EXPECT_FALSE(s.d.has_value(s.mask));

  s.d.set(s.mask, TestEnum::C);
  EXPECT_EQ(s.mask, (1u << 2) | (1u << 3));
  EXPECT_EQ(s.d.value(), TestEnum::C);
  EXPECT_EQ(s.c.value(), -5);
}

TEST(PackedOptional, Reset)
{
  TestStruct s;
  s.a.set(s.mask, 100u);
  s.b.set(s.mask, 1.5f);
  s.a.reset(s.mask);
  EXPECT_FALSE(s.a.has_value(s.mask));
  EXPECT_EQ(s.a.value(), 0u);
  EXPECT_TRUE(s.b.has_value(s.mask));
  EXPECT_EQ(s.b.value(), 1.5f);
  EXPECT_EQ(s.mask, 1u << 1);
}

TEST(PackedOptional, Get)
{
  TestStruct s;
  s.b.set(s.mask, 2.25f);
  const std::optional<float> b = s.b.get(s.mask);
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b.value(), 2.25f);
  EXPECT_FALSE(s.a.get(s.mask).has_value());
}

TEST(PackedOptional, ValueOr)
{
  TestStruct s;
  EXPECT_EQ(s.a.value_or(s.mask, 7u), 7u);
  s.a.set(s.mask, 0u);
  EXPECT_EQ(s.a.value_or(s.mask, 7u), 0u);
}

TEST(PackedOptional, FalseBoolIsPresent)
{
  TestStruct s;
  s.e.set(s.mask, false);
  EXPECT_TRUE(s.e.has_value(s.mask));
  EXPECT_FALSE(s.e.value());
  EXPECT_EQ(s.e.get(s.mask), std::optional<bool>(false));
}

TEST(PackedOptional, SetFromStdOptional)
{
  TestStruct s;
  s.a.set(s.mask, std::optional<u32>(42u));
  EXPECT_TRUE(s.a.has_value(s.mask));
  EXPECT_EQ(s.a.value(), 42u);

  s.a.set(s.mask, std::optional<u32>());
  EXPECT_FALSE(s.a.has_value(s.mask));
  EXPECT_EQ(s.mask, 0u);
}

TEST(PackedOptional, OtherMaskTypes)
{
  u8 mask8 = 0;
  PackedOptional<u16, 7> top8;
  top8.set(mask8, static_cast<u16>(0xBEEF));
  EXPECT_EQ(mask8, 0x80u);
  EXPECT_TRUE(top8.has_value(mask8));
  top8.reset(mask8);
  EXPECT_EQ(mask8, 0u);

  u16 mask16 = 0xFFFFu;
  PackedOptional<u8, 15> top16;
  top16.reset(mask16);
  EXPECT_EQ(mask16, 0x7FFFu);
  EXPECT_FALSE(top16.has_value(mask16));

  u64 mask64 = 0;
  PackedOptional<u32, 63> top64;
  top64.set(mask64, 1u);
  EXPECT_EQ(mask64, u64(1) << 63);
  EXPECT_TRUE(top64.has_value(mask64));
  EXPECT_FALSE(top64.has_value(~(u64(1) << 63)));
}

TEST(PackedOptional, CopyKeepsValueAndMaskSeparately)
{
  TestStruct s;
  s.a.set(s.mask, 9u);
  const TestStruct copy = s;
  EXPECT_TRUE(copy.a.has_value(copy.mask));
  EXPECT_EQ(copy.a.value(), 9u);
}
