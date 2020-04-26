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
