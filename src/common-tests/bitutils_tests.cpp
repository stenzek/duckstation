// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/bitutils.h"

#include "gtest/gtest.h"

#include <type_traits>

template<typename T>
static inline constexpr unsigned ManualCountLeadingZeros(T value)
{
  constexpr unsigned BITS = sizeof(T) * 8u;
  constexpr T MASK = T(1) << (BITS - 1);

  unsigned count = 0;
  for (unsigned i = 0; i < BITS && (value & MASK) == 0; i++)
  {
    count++;
    value <<= 1;
  }

  return count;
}

template<typename T>
static inline constexpr unsigned ManualCountTrailingZeros(T value)
{
  constexpr unsigned BITS = sizeof(T) * 8u;
  constexpr auto MASK = static_cast<std::make_unsigned_t<T>>(1);

  auto u_value = static_cast<std::make_unsigned_t<T>>(value);

  unsigned count = 0;
  for (unsigned i = 0; i < BITS && (u_value & MASK) != MASK; i++)
  {
    count++;
    u_value >>= 1;
  }

  return count;
}

TEST(BitUtils, Test8Bit)
{
  ASSERT_EQ(CountLeadingZeros(u8(1)), ManualCountLeadingZeros(u8(1)));
  ASSERT_EQ(CountLeadingZeros(u8(2)), ManualCountLeadingZeros(u8(2)));
  ASSERT_EQ(CountLeadingZeros(u8(4)), ManualCountLeadingZeros(u8(4)));
  ASSERT_EQ(CountLeadingZeros(u8(8)), ManualCountLeadingZeros(u8(8)));
  ASSERT_EQ(CountLeadingZeros(u8(16)), ManualCountLeadingZeros(u8(16)));
  ASSERT_EQ(CountLeadingZeros(u8(32)), ManualCountLeadingZeros(u8(32)));
  ASSERT_EQ(CountLeadingZeros(u8(64)), ManualCountLeadingZeros(u8(64)));
  ASSERT_EQ(CountLeadingZeros(u8(128)), ManualCountLeadingZeros(u8(128)));

  ASSERT_EQ(CountTrailingZeros(u8(1)), ManualCountTrailingZeros(u8(1)));
  ASSERT_EQ(CountTrailingZeros(u8(2)), ManualCountTrailingZeros(u8(2)));
  ASSERT_EQ(CountTrailingZeros(u8(4)), ManualCountTrailingZeros(u8(4)));
  ASSERT_EQ(CountTrailingZeros(u8(8)), ManualCountTrailingZeros(u8(8)));
  ASSERT_EQ(CountTrailingZeros(u8(16)), ManualCountTrailingZeros(u8(16)));
  ASSERT_EQ(CountTrailingZeros(u8(32)), ManualCountTrailingZeros(u8(32)));
  ASSERT_EQ(CountTrailingZeros(u8(64)), ManualCountTrailingZeros(u8(64)));
  ASSERT_EQ(CountTrailingZeros(u8(128)), ManualCountTrailingZeros(u8(128)));
}

TEST(BitUtils, Test16Bit)
{
  for (u32 i = 1; i < 0x10000; i++)
  {
    u16 value = Truncate16(i);
    ASSERT_EQ(CountLeadingZeros(value), ManualCountLeadingZeros(value));
    ASSERT_EQ(CountTrailingZeros(value), ManualCountTrailingZeros(value));
  }
}

TEST(BitUtils, ZeroExtendHelpers)
{
  EXPECT_EQ(ZeroExtend16(u8(0xFF)), 0x00FFu);
  EXPECT_EQ(ZeroExtend32(u16(0xFFFF)), 0x0000FFFFu);
  EXPECT_EQ(ZeroExtend64(u32(0xFFFFFFFF)), 0x00000000FFFFFFFFull);
}

TEST(BitUtils, SignExtendHelpers)
{
  EXPECT_EQ(SignExtend16(s8(-1)), 0xFFFFu);
  EXPECT_EQ(SignExtend32(s16(-1)), 0xFFFFFFFFu);
  EXPECT_EQ(SignExtend64(s32(-1)), 0xFFFFFFFFFFFFFFFFull);
}

TEST(BitUtils, TruncateHelpers)
{
  EXPECT_EQ(Truncate8(u16(0x1234)), 0x34u);
  EXPECT_EQ(Truncate16(u32(0x12345678)), 0x5678u);
  EXPECT_EQ(Truncate32(u64(0x123456789ABCDEF0)), 0x9ABCDEF0u);
}

TEST(BitUtils, BinaryToBCD)
{
  EXPECT_EQ(BinaryToBCD(0), 0x00u);
  EXPECT_EQ(BinaryToBCD(9), 0x09u);
  EXPECT_EQ(BinaryToBCD(12), 0x12u);
  EXPECT_EQ(BinaryToBCD(99), 0x99u);
}

TEST(BitUtils, PackedBCDToBinary)
{
  EXPECT_EQ(PackedBCDToBinary(0x00), 0);
  EXPECT_EQ(PackedBCDToBinary(0x09), 9);
  EXPECT_EQ(PackedBCDToBinary(0x12), 12);
  EXPECT_EQ(PackedBCDToBinary(0x99), 99);
}

TEST(BitUtils, IsValidBCDDigit)
{
  for (u8 i = 0; i < 10; ++i)
    EXPECT_TRUE(IsValidBCDDigit(i));
  for (u8 i = 10; i < 16; ++i)
    EXPECT_FALSE(IsValidBCDDigit(i));
}

TEST(BitUtils, IsValidPackedBCD)
{
  EXPECT_TRUE(IsValidPackedBCD(0x12));
  EXPECT_TRUE(IsValidPackedBCD(0x99));
  EXPECT_FALSE(IsValidPackedBCD(0x1A));
  EXPECT_FALSE(IsValidPackedBCD(0xA1));
}

TEST(BitUtils, BoolToUInt)
{
  EXPECT_EQ(BoolToUInt8(true), 1u);
  EXPECT_EQ(BoolToUInt8(false), 0u);
  EXPECT_EQ(BoolToUInt16(true), 1u);
  EXPECT_EQ(BoolToUInt32(false), 0u);
  EXPECT_EQ(BoolToUInt64(true), 1ull);
  EXPECT_FLOAT_EQ(BoolToFloat(true), 1.0f);
  EXPECT_FLOAT_EQ(BoolToFloat(false), 0.0f);
}

TEST(BitUtils, ConvertToBool)
{
  EXPECT_TRUE(ConvertToBool(1));
  EXPECT_FALSE(ConvertToBool(0));
  EXPECT_TRUE(ConvertToBool(-1));
  EXPECT_TRUE(ConvertToBool(123));
}

TEST(BitUtils, ConvertToBoolUnchecked)
{
  u8 one = 1, zero = 0;
  EXPECT_TRUE(ConvertToBoolUnchecked(one));
  EXPECT_FALSE(ConvertToBoolUnchecked(zero));
}

TEST(BitUtils, SignExtendN)
{
  EXPECT_EQ(SignExtendN<4>(0b0111), 7);
  EXPECT_EQ(SignExtendN<4>(0b1000), -8);
  EXPECT_EQ(SignExtendN<8>(0xFF), -1);
  EXPECT_EQ(SignExtendN<12>(0x800), -2048);
}

TEST(BitUtils, CountLeadingZeros)
{
  EXPECT_EQ(CountLeadingZeros<u8>(0b00010000), 3u);
  EXPECT_EQ(CountLeadingZeros<u16>(0x0001), 15u);
  EXPECT_EQ(CountLeadingZeros<u32>(0x80000000), 0u);
  EXPECT_EQ(CountLeadingZeros<u64>(0x0000000100000000ull), 31u);
}

TEST(BitUtils, CountTrailingZeros)
{
  EXPECT_EQ(CountTrailingZeros<u8>(0b10000000), 7u);
  EXPECT_EQ(CountTrailingZeros<u16>(0x8000), 15u);
  EXPECT_EQ(CountTrailingZeros<u32>(0x00010000), 16u);
  EXPECT_EQ(CountTrailingZeros<u64>(0x0000000100000000ull), 32u);
}

TEST(BitUtils, ByteSwap)
{
  EXPECT_EQ(ByteSwap<u16>(0x1234), 0x3412u);
  EXPECT_EQ(ByteSwap<u32>(0x12345678), 0x78563412u);
  EXPECT_EQ(ByteSwap<u64>(0x0123456789ABCDEFull), 0xEFCDAB8967452301ull);
  EXPECT_EQ(ByteSwap<s16>(s16(0x1234)), s16(0x3412));
  EXPECT_EQ(ByteSwap<s32>(s32(0x12345678)), s32(0x78563412));
  EXPECT_EQ(ByteSwap<s64>(s64(0x0123456789ABCDEFll)), s64(0xEFCDAB8967452301ll));
}