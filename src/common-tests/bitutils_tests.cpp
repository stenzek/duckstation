// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/align.h"
#include "common/bcdutils.h"
#include "common/bitfield.h"
#include "common/bitutils.h"

#include "gtest/gtest.h"

#include <type_traits>

// Test fixture for BitField tests
namespace {
// Simple test structs using different backing types and field configurations
union TestUnion8
{
  u8 bits;
  BitField<u8, bool, 0, 1> flag;
  BitField<u8, u8, 1, 3> value3bit;
  BitField<u8, u8, 4, 4> value4bit;
  BitField<u8, s8, 4, 4> signed4bit;
};

union TestUnion16
{
  u16 bits;
  BitField<u16, bool, 0, 1> flag;
  BitField<u16, u8, 1, 8> value8bit;
  BitField<u16, u8, 9, 7> value7bit;
  BitField<u16, s8, 8, 8> signed8bit;
};

union TestUnion32
{
  u32 bits;
  BitField<u32, bool, 0, 1> flag;
  BitField<u32, u16, 1, 16> value16bit;
  BitField<u32, u32, 17, 15> value15bit;
  BitField<u32, s16, 16, 16> signed16bit;
};

union TestUnion64
{
  u64 bits;
  BitField<u64, bool, 0, 1> flag;
  BitField<u64, u32, 1, 32> value32bit;
  BitField<u64, u64, 33, 31> value31bit;
  BitField<u64, s32, 32, 32> signed32bit;
};
} // namespace

// Tests for GetMask method
TEST(BitField, GetMask)
{
  TestUnion8 test8{};
  EXPECT_EQ(test8.flag.GetMask(), 0x01u);
  EXPECT_EQ(test8.value3bit.GetMask(), 0x0Eu);
  EXPECT_EQ(test8.value4bit.GetMask(), 0xF0u);

  TestUnion16 test16{};
  EXPECT_EQ(test16.flag.GetMask(), 0x0001u);
  EXPECT_EQ(test16.value8bit.GetMask(), 0x01FEu);
  EXPECT_EQ(test16.value7bit.GetMask(), 0xFE00u);

  TestUnion32 test32{};
  EXPECT_EQ(test32.flag.GetMask(), 0x00000001u);
  EXPECT_EQ(test32.value16bit.GetMask(), 0x0001FFFEu);
  EXPECT_EQ(test32.value15bit.GetMask(), 0xFFFE0000u);

  TestUnion64 test64{};
  EXPECT_EQ(test64.flag.GetMask(), 0x0000000000000001ULL);
  EXPECT_EQ(test64.value32bit.GetMask(), 0x00000001FFFFFFFEULL);
  EXPECT_EQ(test64.value31bit.GetMask(), 0xFFFFFFFE00000000ULL);
}

// Tests for implicit conversion operator (operator DataType())
TEST(BitField, ImplicitConversion)
{
  TestUnion8 test8{};
  test8.bits = 0xFF;

  bool flag_value = test8.flag;
  EXPECT_TRUE(flag_value);

  u8 value3bit_value = test8.value3bit;
  EXPECT_EQ(value3bit_value, 7);

  u8 value4bit_value = test8.value4bit;
  EXPECT_EQ(value4bit_value, 15);

  // Test with zeros
  test8.bits = 0x00;
  flag_value = test8.flag;
  EXPECT_FALSE(flag_value);

  value3bit_value = test8.value3bit;
  EXPECT_EQ(value3bit_value, 0);
}

// Tests for assignment operator (operator=)
TEST(BitField, Assignment)
{
  TestUnion8 test8{};
  test8.bits = 0x00;

  test8.flag = true;
  EXPECT_EQ(test8.bits, 0x01);

  test8.value3bit = 5;
  EXPECT_EQ(test8.bits, 0x0B); // 0001 (flag) + 1010 (5 << 1)

  test8.value4bit = 12;
  EXPECT_EQ(test8.bits, 0xCB); // Previous + 1100 (12 << 4)

  // Test overwriting
  test8.flag = false;
  EXPECT_EQ(test8.bits, 0xCA);
}

// Tests for prefix increment operator (operator++())
TEST(BitField, PrefixIncrement)
{
  TestUnion8 test8{};
  test8.bits = 0x00;
  test8.value3bit = 3;

  u8 result = ++test8.value3bit;
  EXPECT_EQ(result, 4);
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 4);

  // Test overflow handling
  test8.value3bit = 7; // Maximum for 3 bits
  result = ++test8.value3bit;
  EXPECT_EQ(result, 0); // Should wrap around
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 0);
}

// Tests for postfix increment operator (operator++(int))
TEST(BitField, PostfixIncrement)
{
  TestUnion8 test8{};
  test8.bits = 0x00;
  test8.value3bit = 3;

  u8 result = test8.value3bit++;
  EXPECT_EQ(result, 3);                           // Should return old value
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 4); // Should increment

  // Test overflow handling
  test8.value3bit = 7; // Maximum for 3 bits
  result = test8.value3bit++;
  EXPECT_EQ(result, 7);                           // Should return old value
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 0); // Should wrap around
}

// Tests for prefix decrement operator (operator--())
TEST(BitField, PrefixDecrement)
{
  TestUnion8 test8{};
  test8.bits = 0x00;
  test8.value3bit = 4;

  u8 result = --test8.value3bit;
  EXPECT_EQ(result, 3);
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 3);

  // Test underflow handling
  test8.value3bit = 0;
  result = --test8.value3bit;
  EXPECT_EQ(result, 7); // Should wrap around
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 7);
}

// Tests for postfix decrement operator (operator--(int))
TEST(BitField, PostfixDecrement)
{
  TestUnion8 test8{};
  test8.bits = 0x00;
  test8.value3bit = 4;

  u8 result = test8.value3bit--;
  EXPECT_EQ(result, 4);                           // Should return old value
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 3); // Should decrement

  // Test underflow handling
  test8.value3bit = 0;
  result = test8.value3bit--;
  EXPECT_EQ(result, 0);                           // Should return old value
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 7); // Should wrap around
}

// Tests for compound assignment operators (+=, -=, *=, /=)
TEST(BitField, CompoundArithmeticOperators)
{
  TestUnion8 test8{};
  test8.bits = 0x00;
  test8.value4bit = 5;

  // Test +=
  test8.value4bit += 3;
  EXPECT_EQ(static_cast<u8>(test8.value4bit), 8);

  // Test -=
  test8.value4bit -= 2;
  EXPECT_EQ(static_cast<u8>(test8.value4bit), 6);

  // Test *=
  test8.value4bit *= 2;
  EXPECT_EQ(static_cast<u8>(test8.value4bit), 12);

  // Test / =
  test8.value4bit /= 3;
  EXPECT_EQ(static_cast<u8>(test8.value4bit), 4);

  // Test overflow with +=
  test8.value4bit = 14;
  test8.value4bit += 5;                           // Should overflow and wrap
  EXPECT_EQ(static_cast<u8>(test8.value4bit), 3); // (14 + 5) & 0xF = 19 & 0xF = 3
}

// Tests for compound bitwise operators (&=, |=, ^=, <<=, >>=)
TEST(BitField, CompoundBitwiseOperators)
{
  TestUnion8 test8{};
  test8.bits = 0x00;
  test8.value4bit = 12; // 1100

  // Test & =
  test8.value4bit &= 10; // 1100 & 1010 = 1000
  EXPECT_EQ(static_cast<u8>(test8.value4bit), 8);

  // Test |=
  test8.value4bit |= 3; // 1000 | 0011 = 1011
  EXPECT_EQ(static_cast<u8>(test8.value4bit), 11);

  // Test ^ =
  test8.value4bit ^= 5; // 1011 ^ 0101 = 1110
  EXPECT_EQ(static_cast<u8>(test8.value4bit), 14);

  // Test << =
  test8.value4bit = 3;   // 0011
  test8.value4bit <<= 2; // 0011 << 2 = 1100
  EXPECT_EQ(static_cast<u8>(test8.value4bit), 12);

  // Test >> =
  test8.value4bit >>= 1; // 1100 >> 1 = 0110
  EXPECT_EQ(static_cast<u8>(test8.value4bit), 6);
}

// Tests for GetValue method with different data types
TEST(BitField, GetValue)
{
  // Test unsigned values
  TestUnion16 test16{};
  test16.bits = 0xFFFF;
  EXPECT_EQ(test16.flag.GetValue(), true);
  EXPECT_EQ(test16.value8bit.GetValue(), 0xFF);
  EXPECT_EQ(test16.value7bit.GetValue(), 0x7F);

  // Test signed values
  test16.bits = 0xFF00;                        // Set upper 8 bits
  EXPECT_EQ(test16.signed8bit.GetValue(), -1); // Should be sign-extended

  test16.bits = 0x7F00; // Set bit pattern for +127
  EXPECT_EQ(test16.signed8bit.GetValue(), 127);

  test16.bits = 0x8000; // Set bit pattern for -128
  EXPECT_EQ(test16.signed8bit.GetValue(), -128);
}

// Tests for SetValue method
TEST(BitField, SetValue)
{
  TestUnion16 test16{};
  test16.bits = 0x0000;

  test16.flag.SetValue(true);
  EXPECT_EQ(test16.bits, 0x0001);

  test16.value8bit.SetValue(0xAB);
  EXPECT_EQ(test16.bits, 0x0157); // 0xAB << 1 | 0x0001

  test16.value7bit.SetValue(0x55);
  EXPECT_EQ(test16.bits, 0xAB57); // 0x55 << 9 | previous

  // Test that setting doesn't affect other fields
  test16.bits = 0xFFFF;
  test16.flag.SetValue(false);
  EXPECT_EQ(test16.bits, 0xFFFE); // Only bit 0 should be cleared
}

// Tests for boolean BitFields (1-bit fields)
TEST(BitField, BooleanBitFields)
{
  TestUnion8 test8{};
  test8.bits = 0x00;

  // Test setting and getting boolean values
  test8.flag.SetValue(true);
  EXPECT_TRUE(test8.flag.GetValue());
  EXPECT_TRUE(static_cast<bool>(test8.flag));

  test8.flag.SetValue(false);
  EXPECT_FALSE(test8.flag.GetValue());
  EXPECT_FALSE(static_cast<bool>(test8.flag));

  // Test that any non-zero value in the bit position is treated as true
  test8.bits = 0x01;
  EXPECT_TRUE(test8.flag.GetValue());
}

// Tests for signed BitFields with sign extension
TEST(BitField, SignedBitFields)
{
  TestUnion32 test32{};
  test32.bits = 0x00000000;

  // Test positive signed values
  test32.signed16bit.SetValue(12345);
  EXPECT_EQ(test32.signed16bit.GetValue(), 12345);

  // Test negative signed values
  test32.signed16bit.SetValue(-12345);
  EXPECT_EQ(test32.signed16bit.GetValue(), -12345);

  // Test maximum positive value for 16-bit signed
  test32.signed16bit.SetValue(32767);
  EXPECT_EQ(test32.signed16bit.GetValue(), 32767);

  // Test minimum negative value for 16-bit signed
  test32.signed16bit.SetValue(-32768);
  EXPECT_EQ(test32.signed16bit.GetValue(), -32768);

  // Test sign extension with manual bit manipulation
  test32.bits = 0xFFFF0000; // Set all bits in the signed field to 1
  EXPECT_EQ(test32.signed16bit.GetValue(), -1);
}

// Tests for edge cases and boundary conditions
TEST(BitField, EdgeCases)
{
  TestUnion64 test64{};
  test64.bits = 0x0000000000000000ULL;

  // Test maximum values
  test64.value32bit.SetValue(0xFFFFFFFF);
  EXPECT_EQ(test64.value32bit.GetValue(), 0xFFFFFFFFu);

  test64.value31bit.SetValue(0x7FFFFFFF);
  EXPECT_EQ(test64.value31bit.GetValue(), 0x7FFFFFFFu);

  // Test that fields don't interfere with each other
  test64.bits = 0x0000000000000000ULL;
  test64.flag.SetValue(true);
  test64.value32bit.SetValue(0x12345678);
  test64.value31bit.SetValue(0x55555555);

  EXPECT_TRUE(test64.flag.GetValue());
  EXPECT_EQ(test64.value32bit.GetValue(), 0x12345678u);
  EXPECT_EQ(test64.value31bit.GetValue(), 0x55555555u);
}

// Tests for arithmetic operations that may cause overflow
TEST(BitField, OverflowBehavior)
{
  TestUnion8 test8{};
  test8.bits = 0x00;

  // Test 3-bit field (max value 7)
  test8.value3bit.SetValue(7);
  test8.value3bit += 1; // Should overflow
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 0);

  test8.value3bit.SetValue(7);
  test8.value3bit *= 2; // 7 * 2 = 14, should wrap to 6 (14 & 0x7)
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 6);

  // Test underflow
  test8.value3bit.SetValue(0);
  test8.value3bit -= 1; // Should underflow
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 7);
}

// Tests for chaining operations
TEST(BitField, OperatorChaining)
{
  TestUnion8 test8{};
  test8.bits = 0x00;

  // Test chaining assignment operators
  auto& result = test8.value3bit = 5;
  EXPECT_EQ(&result, &test8.value3bit);
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 5);

  // Test chaining compound operators
  auto& result2 = (test8.value3bit += 2);
  EXPECT_EQ(&result2, &test8.value3bit);
  EXPECT_EQ(static_cast<u8>(test8.value3bit), 7);
}

// Tests for static_assert conditions
TEST(BitField, StaticAssertions)
{
  // These should compile without issues due to the static_assert in BitField

  // Boolean fields should only be 1 bit
  union TestBoolField
  {
    u8 bits;
    BitField<u8, bool, 0, 1> valid_bool; // This should compile
  };

  // This would fail to compile:
  // BitField<u8, bool, 0, 2> invalid_bool; // static_assert should trigger

  TestBoolField test{};
  test.bits = 0;
  test.valid_bool = true;
  EXPECT_TRUE(test.valid_bool);
}

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

// Alignment function tests
TEST(Align, IsAligned)
{
  // Test with various integer types and alignments
  EXPECT_TRUE(Common::IsAligned(0u, 4u));
  EXPECT_TRUE(Common::IsAligned(4u, 4u));
  EXPECT_TRUE(Common::IsAligned(8u, 4u));
  EXPECT_TRUE(Common::IsAligned(16u, 4u));
  EXPECT_FALSE(Common::IsAligned(1u, 4u));
  EXPECT_FALSE(Common::IsAligned(3u, 4u));
  EXPECT_FALSE(Common::IsAligned(5u, 4u));

  // Test with pointer types
  EXPECT_TRUE(Common::IsAligned(reinterpret_cast<uintptr_t>(nullptr), 8u));
  EXPECT_TRUE(Common::IsAligned(0x1000ull, 16u));
  EXPECT_FALSE(Common::IsAligned(0x1001ull, 16u));

  // Test with different alignments
  EXPECT_TRUE(Common::IsAligned(15u, 1u));
  EXPECT_TRUE(Common::IsAligned(16u, 8u));
  EXPECT_FALSE(Common::IsAligned(15u, 8u));
}

TEST(Align, AlignUp)
{
  // Test basic alignment up
  EXPECT_EQ(Common::AlignUp(0u, 4u), 0u);
  EXPECT_EQ(Common::AlignUp(1u, 4u), 4u);
  EXPECT_EQ(Common::AlignUp(3u, 4u), 4u);
  EXPECT_EQ(Common::AlignUp(4u, 4u), 4u);
  EXPECT_EQ(Common::AlignUp(5u, 4u), 8u);
  EXPECT_EQ(Common::AlignUp(7u, 4u), 8u);
  EXPECT_EQ(Common::AlignUp(8u, 4u), 8u);

  // Test with larger values
  EXPECT_EQ(Common::AlignUp(1000u, 64u), 1024u);
  EXPECT_EQ(Common::AlignUp(1024u, 64u), 1024u);
  EXPECT_EQ(Common::AlignUp(1025u, 64u), 1088u);

  // Test with different types
  EXPECT_EQ(Common::AlignUp(13ull, 8u), 16ull);
  EXPECT_EQ(Common::AlignUp(size_t(100), 16u), size_t(112));
}

TEST(Align, AlignDown)
{
  // Test basic alignment down
  EXPECT_EQ(Common::AlignDown(0u, 4u), 0u);
  EXPECT_EQ(Common::AlignDown(1u, 4u), 0u);
  EXPECT_EQ(Common::AlignDown(3u, 4u), 0u);
  EXPECT_EQ(Common::AlignDown(4u, 4u), 4u);
  EXPECT_EQ(Common::AlignDown(5u, 4u), 4u);
  EXPECT_EQ(Common::AlignDown(7u, 4u), 4u);
  EXPECT_EQ(Common::AlignDown(8u, 4u), 8u);

  // Test with larger values
  EXPECT_EQ(Common::AlignDown(1000u, 64u), 960u);
  EXPECT_EQ(Common::AlignDown(1024u, 64u), 1024u);
  EXPECT_EQ(Common::AlignDown(1087u, 64u), 1024u);

  // Test with different types
  EXPECT_EQ(Common::AlignDown(13ull, 8u), 8ull);
  EXPECT_EQ(Common::AlignDown(size_t(100), 16u), size_t(96));
}

TEST(Align, IsAlignedPow2)
{
  // Test power-of-2 alignment checks
  EXPECT_TRUE(Common::IsAlignedPow2(0u, 4u));
  EXPECT_TRUE(Common::IsAlignedPow2(4u, 4u));
  EXPECT_TRUE(Common::IsAlignedPow2(8u, 4u));
  EXPECT_TRUE(Common::IsAlignedPow2(16u, 4u));
  EXPECT_FALSE(Common::IsAlignedPow2(1u, 4u));
  EXPECT_FALSE(Common::IsAlignedPow2(3u, 4u));
  EXPECT_FALSE(Common::IsAlignedPow2(5u, 4u));

  // Test with different power-of-2 alignments
  EXPECT_TRUE(Common::IsAlignedPow2(32u, 16u));
  EXPECT_TRUE(Common::IsAlignedPow2(64u, 16u));
  EXPECT_TRUE(Common::IsAlignedPow2(48u, 16u));
  EXPECT_FALSE(Common::IsAlignedPow2(56u, 16u));
  EXPECT_FALSE(Common::IsAlignedPow2(63u, 16u));

  // Test with larger values
  EXPECT_TRUE(Common::IsAlignedPow2(1024u, 256u));
  EXPECT_TRUE(Common::IsAlignedPow2(1280u, 256u));
  EXPECT_FALSE(Common::IsAlignedPow2(1100u, 256u));
}

TEST(Align, AlignUpPow2)
{
  // Test power-of-2 alignment up
  EXPECT_EQ(Common::AlignUpPow2(0u, 4u), 0u);
  EXPECT_EQ(Common::AlignUpPow2(1u, 4u), 4u);
  EXPECT_EQ(Common::AlignUpPow2(3u, 4u), 4u);
  EXPECT_EQ(Common::AlignUpPow2(4u, 4u), 4u);
  EXPECT_EQ(Common::AlignUpPow2(5u, 4u), 8u);
  EXPECT_EQ(Common::AlignUpPow2(7u, 4u), 8u);
  EXPECT_EQ(Common::AlignUpPow2(8u, 4u), 8u);

  // Test with larger power-of-2 alignments
  EXPECT_EQ(Common::AlignUpPow2(1000u, 64u), 1024u);
  EXPECT_EQ(Common::AlignUpPow2(1024u, 64u), 1024u);
  EXPECT_EQ(Common::AlignUpPow2(1025u, 64u), 1088u);

  // Test with different types
  EXPECT_EQ(Common::AlignUpPow2(13ull, 8u), 16ull);
  EXPECT_EQ(Common::AlignUpPow2(size_t(100), 16u), size_t(112));
}

TEST(Align, AlignDownPow2)
{
  // Test power-of-2 alignment down
  EXPECT_EQ(Common::AlignDownPow2(0u, 4u), 0u);
  EXPECT_EQ(Common::AlignDownPow2(1u, 4u), 0u);
  EXPECT_EQ(Common::AlignDownPow2(3u, 4u), 0u);
  EXPECT_EQ(Common::AlignDownPow2(4u, 4u), 4u);
  EXPECT_EQ(Common::AlignDownPow2(5u, 4u), 4u);
  EXPECT_EQ(Common::AlignDownPow2(7u, 4u), 4u);
  EXPECT_EQ(Common::AlignDownPow2(8u, 4u), 8u);

  // Test with larger power-of-2 alignments
  EXPECT_EQ(Common::AlignDownPow2(1000u, 64u), 960u);
  EXPECT_EQ(Common::AlignDownPow2(1024u, 64u), 1024u);
  EXPECT_EQ(Common::AlignDownPow2(1087u, 64u), 1024u);

  // Test with different types
  EXPECT_EQ(Common::AlignDownPow2(13ull, 8u), 8ull);
  EXPECT_EQ(Common::AlignDownPow2(size_t(100), 16u), size_t(96));
}

TEST(Align, IsPow2)
{
  // Test power-of-2 detection
  EXPECT_TRUE(Common::IsPow2(0u));
  EXPECT_TRUE(Common::IsPow2(1u));
  EXPECT_TRUE(Common::IsPow2(2u));
  EXPECT_TRUE(Common::IsPow2(4u));
  EXPECT_TRUE(Common::IsPow2(8u));
  EXPECT_TRUE(Common::IsPow2(16u));
  EXPECT_TRUE(Common::IsPow2(32u));
  EXPECT_TRUE(Common::IsPow2(64u));
  EXPECT_TRUE(Common::IsPow2(128u));
  EXPECT_TRUE(Common::IsPow2(256u));
  EXPECT_TRUE(Common::IsPow2(1024u));

  // Test non-power-of-2 values
  EXPECT_FALSE(Common::IsPow2(3u));
  EXPECT_FALSE(Common::IsPow2(5u));
  EXPECT_FALSE(Common::IsPow2(6u));
  EXPECT_FALSE(Common::IsPow2(7u));
  EXPECT_FALSE(Common::IsPow2(9u));
  EXPECT_FALSE(Common::IsPow2(15u));
  EXPECT_FALSE(Common::IsPow2(31u));
  EXPECT_FALSE(Common::IsPow2(1000u));

  // Test with different types
  EXPECT_TRUE(Common::IsPow2(512ull));
  EXPECT_FALSE(Common::IsPow2(513ull));
}

TEST(Align, PreviousPow2)
{
  // Test finding previous power-of-2
  EXPECT_EQ(Common::PreviousPow2(1u), 1u);
  EXPECT_EQ(Common::PreviousPow2(2u), 2u);
  EXPECT_EQ(Common::PreviousPow2(3u), 2u);
  EXPECT_EQ(Common::PreviousPow2(4u), 4u);
  EXPECT_EQ(Common::PreviousPow2(5u), 4u);
  EXPECT_EQ(Common::PreviousPow2(6u), 4u);
  EXPECT_EQ(Common::PreviousPow2(7u), 4u);
  EXPECT_EQ(Common::PreviousPow2(8u), 8u);
  EXPECT_EQ(Common::PreviousPow2(15u), 8u);
  EXPECT_EQ(Common::PreviousPow2(16u), 16u);
  EXPECT_EQ(Common::PreviousPow2(31u), 16u);
  EXPECT_EQ(Common::PreviousPow2(32u), 32u);
  EXPECT_EQ(Common::PreviousPow2(1000u), 512u);
  EXPECT_EQ(Common::PreviousPow2(1024u), 1024u);

  // Test with different types
  EXPECT_EQ(Common::PreviousPow2(100ull), 64ull);
  EXPECT_EQ(Common::PreviousPow2(static_cast<size_t>(2047)), static_cast<size_t>(1024));
  EXPECT_EQ(Common::PreviousPow2(static_cast<size_t>(2048)), static_cast<size_t>(2048));
}

TEST(Align, NextPow2)
{
  // Test finding next power-of-2
  EXPECT_EQ(Common::NextPow2(0u), 0u);
  EXPECT_EQ(Common::NextPow2(1u), 1u);
  EXPECT_EQ(Common::NextPow2(2u), 2u);
  EXPECT_EQ(Common::NextPow2(3u), 4u);
  EXPECT_EQ(Common::NextPow2(4u), 4u);
  EXPECT_EQ(Common::NextPow2(5u), 8u);
  EXPECT_EQ(Common::NextPow2(7u), 8u);
  EXPECT_EQ(Common::NextPow2(8u), 8u);
  EXPECT_EQ(Common::NextPow2(9u), 16u);
  EXPECT_EQ(Common::NextPow2(15u), 16u);
  EXPECT_EQ(Common::NextPow2(16u), 16u);
  EXPECT_EQ(Common::NextPow2(17u), 32u);
  EXPECT_EQ(Common::NextPow2(1000u), 1024u);
  EXPECT_EQ(Common::NextPow2(1024u), 1024u);

  // Test with different types
  EXPECT_EQ(Common::NextPow2(100ull), 128ull);
  EXPECT_EQ(Common::NextPow2(size_t(1025)), size_t(2048));
}

TEST(Align, AlignedMallocAndFree)
{
  // Test aligned memory allocation and deallocation
  void* ptr1 = Common::AlignedMalloc(128, 16);
  ASSERT_NE(ptr1, nullptr);
  EXPECT_TRUE(Common::IsAlignedPow2(reinterpret_cast<uintptr_t>(ptr1), 16u));
  Common::AlignedFree(ptr1);

  void* ptr2 = Common::AlignedMalloc(256, 32);
  ASSERT_NE(ptr2, nullptr);
  EXPECT_TRUE(Common::IsAlignedPow2(reinterpret_cast<uintptr_t>(ptr2), 32u));
  Common::AlignedFree(ptr2);

  void* ptr3 = Common::AlignedMalloc(1024, 64);
  ASSERT_NE(ptr3, nullptr);
  EXPECT_TRUE(Common::IsAlignedPow2(reinterpret_cast<uintptr_t>(ptr3), 64u));
  Common::AlignedFree(ptr3);

  // Test with zero size (implementation-defined behavior)
  void* ptr4 = Common::AlignedMalloc(0, 16);
  Common::AlignedFree(ptr4); // Should not crash even if ptr4 is nullptr

  // Test AlignedFree with nullptr (should not crash)
  Common::AlignedFree(nullptr);
}

TEST(Align, UniqueAlignedPtr)
{
  // Test make_unique_aligned for arrays
  auto ptr1 = Common::make_unique_aligned<int[]>(16, 10);
  ASSERT_NE(ptr1.get(), nullptr);
  EXPECT_TRUE(Common::IsAlignedPow2(reinterpret_cast<uintptr_t>(ptr1.get()), 16u));

  // Test that we can write to the memory
  for (int i = 0; i < 10; ++i)
  {
    ptr1[i] = i * 2;
  }
  for (int i = 0; i < 10; ++i)
  {
    EXPECT_EQ(ptr1[i], i * 2);
  }

  auto ptr2 = Common::make_unique_aligned<u8[]>(32, 100);
  ASSERT_NE(ptr2.get(), nullptr);
  EXPECT_TRUE(Common::IsAlignedPow2(reinterpret_cast<uintptr_t>(ptr2.get()), 32u));

  // Test make_unique_aligned_for_overwrite
  auto ptr3 = Common::make_unique_aligned_for_overwrite<u32[]>(64, 50);
  ASSERT_NE(ptr3.get(), nullptr);
  EXPECT_TRUE(Common::IsAlignedPow2(reinterpret_cast<uintptr_t>(ptr3.get()), 64u));

  // Test that we can write to the memory
  for (u32 i = 0; i < 50; ++i)
  {
    ptr3[i] = i + 1000;
  }
  for (u32 i = 0; i < 50; ++i)
  {
    EXPECT_EQ(ptr3[i], i + 1000);
  }
}