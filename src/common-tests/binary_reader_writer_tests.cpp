// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/binary_reader_writer.h"
#include "common/small_string.h"

#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>

using namespace std::string_view_literals;

//////////////////////////////////////////////////////////////////////////
// Global test data
//////////////////////////////////////////////////////////////////////////

// Buffer with various primitive types
alignas(8) static constexpr std::array<u8, 32> g_primitive_buffer = {{
  0x01,                                           // u8: 1
  0xFE,                                           // s8: -2
  0x34, 0x12,                                     // u16: 0x1234 (little endian)
  0xCD, 0xAB,                                     // s16: -0x5433 (little endian)
  0x78, 0x56, 0x34, 0x12,                         // u32: 0x12345678
  0x88, 0xA9, 0xCB, 0xED,                         // s32: -0x12345678
  0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // u64: 0x0123456789ABCDEF
  0x00, 0x00, 0x80, 0x3F,                         // float: 1.0f
  0x00, 0x00, 0x00, 0x00                          // padding
}};

// Buffer with C-strings (null-terminated)
static constexpr std::array<u8, 16> g_cstring_buffer = {{
  'H', 'e', 'l', 'l', 'o', '\0', // "Hello"
  'W', 'o', 'r', 'l', 'd', '\0', // "World"
  '\0',                          // empty string
  'A', 'B', 'C'                  // "ABC" without null terminator (truncated)
}};

// Buffer with size-prefixed strings (u32 length + data)
static constexpr std::array<u8, 25> g_size_prefixed_buffer = {
  0x05, 0x00, 0x00, 0x00,      // length: 5
  'H',  'e',  'l',  'l',  'o', // "Hello"
  0x00, 0x00, 0x00, 0x00,      // length: 0 (empty string)
  0x04, 0x00, 0x00, 0x00,      // length: 4
  'T',  'e',  's',  't',       // "Test"
  0xFF, 0xFF, 0xFF, 0xFF       // invalid length (overflow)
};

//////////////////////////////////////////////////////////////////////////
// BinarySpanReader Tests
//////////////////////////////////////////////////////////////////////////

TEST(BinarySpanReader, DefaultConstructor)
{
  BinarySpanReader reader;
  EXPECT_FALSE(reader.IsValid());
  EXPECT_EQ(reader.GetBufferRemaining(), 0u);
  EXPECT_EQ(reader.GetBufferConsumed(), 0u);
}

TEST(BinarySpanReader, SpanConstructor)
{
  BinarySpanReader reader(g_primitive_buffer);
  EXPECT_TRUE(reader.IsValid());
  EXPECT_EQ(reader.GetBufferRemaining(), g_primitive_buffer.size());
  EXPECT_EQ(reader.GetBufferConsumed(), 0u);
}

TEST(BinarySpanReader, MoveConstructor)
{
  BinarySpanReader original(g_primitive_buffer);
  original.ReadU8();
  original.ReadU8();

  BinarySpanReader moved(std::move(original));
  EXPECT_TRUE(moved.IsValid());
  EXPECT_EQ(moved.GetBufferConsumed(), 2u);
  EXPECT_EQ(original.GetBufferConsumed(), 0u);
}

TEST(BinarySpanReader, MoveAssignment)
{
  BinarySpanReader original(g_primitive_buffer);
  original.ReadU8();

  BinarySpanReader moved;
  moved = std::move(original);
  EXPECT_TRUE(moved.IsValid());
  EXPECT_EQ(moved.GetBufferConsumed(), 1u);
  EXPECT_EQ(original.GetBufferConsumed(), 0u);
}

TEST(BinarySpanReader, GetSpan)
{
  BinarySpanReader reader(g_primitive_buffer);
  auto span = reader.GetSpan();
  EXPECT_EQ(span.size(), g_primitive_buffer.size());
  EXPECT_EQ(span.data(), g_primitive_buffer.data());
}

TEST(BinarySpanReader, CheckRemaining)
{
  BinarySpanReader reader(g_primitive_buffer);
  EXPECT_TRUE(reader.CheckRemaining(g_primitive_buffer.size()));
  EXPECT_TRUE(reader.CheckRemaining(1));
  EXPECT_FALSE(reader.CheckRemaining(g_primitive_buffer.size() + 1));
}

TEST(BinarySpanReader, ReadU8)
{
  BinarySpanReader reader(g_primitive_buffer);
  u8 val;
  EXPECT_TRUE(reader.ReadU8(&val));
  EXPECT_EQ(val, 0x01u);
  EXPECT_EQ(reader.GetBufferConsumed(), 1u);
}

TEST(BinarySpanReader, ReadS8)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(1);
  s8 val;
  EXPECT_TRUE(reader.ReadS8(&val));
  EXPECT_EQ(val, static_cast<s8>(0xFE));
  EXPECT_EQ(reader.GetBufferConsumed(), 2u);
}

TEST(BinarySpanReader, ReadU16)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(2);
  u16 val;
  EXPECT_TRUE(reader.ReadU16(&val));
  EXPECT_EQ(val, 0x1234u);
}

TEST(BinarySpanReader, ReadS16)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(4);
  s16 val;
  EXPECT_TRUE(reader.ReadS16(&val));
  EXPECT_EQ(val, static_cast<s16>(0xABCD));
}

TEST(BinarySpanReader, ReadU32)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(6);
  u32 val;
  EXPECT_TRUE(reader.ReadU32(&val));
  EXPECT_EQ(val, 0x12345678u);
}

TEST(BinarySpanReader, ReadS32)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(10);
  s32 val;
  EXPECT_TRUE(reader.ReadS32(&val));
  EXPECT_EQ(val, static_cast<s32>(0xEDCBA988));
}

TEST(BinarySpanReader, ReadU64)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(14);
  u64 val;
  EXPECT_TRUE(reader.ReadU64(&val));
  EXPECT_EQ(val, 0x0123456789ABCDEFull);
}

TEST(BinarySpanReader, ReadFloat)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(22);
  float val;
  EXPECT_TRUE(reader.ReadFloat(&val));
  EXPECT_FLOAT_EQ(val, 1.0f);
}

TEST(BinarySpanReader, ReadBool)
{
  std::array<u8, 2> buf = {0x00, 0x01};
  BinarySpanReader reader(buf);
  bool val;
  EXPECT_TRUE(reader.ReadBool(&val));
  EXPECT_FALSE(val);
  EXPECT_TRUE(reader.ReadBool(&val));
  EXPECT_TRUE(val);
}

TEST(BinarySpanReader, ReadTReturningValue)
{
  BinarySpanReader reader(g_primitive_buffer);
  EXPECT_EQ(reader.ReadU8(), 0x01u);
  EXPECT_EQ(reader.ReadS8(), static_cast<s8>(0xFE));
  EXPECT_EQ(reader.ReadU16(), 0x1234u);
  EXPECT_EQ(reader.ReadS16(), static_cast<s16>(0xABCD));
  EXPECT_EQ(reader.ReadU32(), 0x12345678u);
  EXPECT_EQ(reader.ReadS32(), static_cast<s32>(0xEDCBA988));
  EXPECT_EQ(reader.ReadU64(), 0x0123456789ABCDEFull);
  EXPECT_FLOAT_EQ(reader.ReadFloat(), 1.0f);
}

TEST(BinarySpanReader, ReadBoolReturningValue)
{
  std::array<u8, 3> buf = {0x00, 0x01, 0xFF};
  BinarySpanReader reader(buf);
  EXPECT_FALSE(reader.ReadBool());
  EXPECT_TRUE(reader.ReadBool());
  EXPECT_TRUE(reader.ReadBool()); // any non-zero is true
}

TEST(BinarySpanReader, ReadCStringToString)
{
  BinarySpanReader reader(g_cstring_buffer);
  std::string val;
  EXPECT_TRUE(reader.ReadCString(&val));
  EXPECT_EQ(val, "Hello");
  EXPECT_EQ(reader.GetBufferConsumed(), 6u);
}

TEST(BinarySpanReader, ReadCStringToStringView)
{
  BinarySpanReader reader(g_cstring_buffer);
  std::string_view val;
  EXPECT_TRUE(reader.ReadCString(&val));
  EXPECT_EQ(val, "Hello"sv);
}

TEST(BinarySpanReader, ReadCStringToSmallString)
{
  BinarySpanReader reader(g_cstring_buffer);
  SmallString val;
  EXPECT_TRUE(reader.ReadCString(&val));
  EXPECT_STREQ(val.c_str(), "Hello");
}

TEST(BinarySpanReader, ReadCStringReturningValue)
{
  BinarySpanReader reader(g_cstring_buffer);
  EXPECT_EQ(reader.ReadCString(), "Hello"sv);
  EXPECT_EQ(reader.ReadCString(), "World"sv);
  EXPECT_EQ(reader.ReadCString(), ""sv);
}

TEST(BinarySpanReader, ReadCStringWithoutNullTerminator)
{
  // Buffer ending without null terminator
  std::array<u8, 3> buf = {'A', 'B', 'C'};
  BinarySpanReader reader(buf);
  std::string val;
  EXPECT_FALSE(reader.ReadCString(&val));
}

TEST(BinarySpanReader, ReadSizePrefixedStringToString)
{
  BinarySpanReader reader(g_size_prefixed_buffer);
  std::string val;
  EXPECT_TRUE(reader.ReadSizePrefixedString(&val));
  EXPECT_EQ(val, "Hello");
  EXPECT_EQ(reader.GetBufferConsumed(), 9u);
}

TEST(BinarySpanReader, ReadSizePrefixedStringToStringView)
{
  BinarySpanReader reader(g_size_prefixed_buffer);
  std::string_view val;
  EXPECT_TRUE(reader.ReadSizePrefixedString(&val));
  EXPECT_EQ(val, "Hello"sv);
}

TEST(BinarySpanReader, ReadSizePrefixedStringToSmallString)
{
  BinarySpanReader reader(g_size_prefixed_buffer);
  SmallString val;
  EXPECT_TRUE(reader.ReadSizePrefixedString(&val));
  EXPECT_STREQ(val.c_str(), "Hello");
}

TEST(BinarySpanReader, ReadSizePrefixedStringReturningValue)
{
  BinarySpanReader reader(g_size_prefixed_buffer);
  EXPECT_EQ(reader.ReadSizePrefixedString(), "Hello"sv);
  EXPECT_EQ(reader.ReadSizePrefixedString(), ""sv);
  EXPECT_EQ(reader.ReadSizePrefixedString(), "Test"sv);
}

TEST(BinarySpanReader, ReadSizePrefixedStringEmpty)
{
  BinarySpanReader reader(g_size_prefixed_buffer);
  reader.IncrementPosition(9);
  std::string val;
  EXPECT_TRUE(reader.ReadSizePrefixedString(&val));
  EXPECT_TRUE(val.empty());
}

TEST(BinarySpanReader, PeekU8)
{
  BinarySpanReader reader(g_primitive_buffer);
  u8 val;
  EXPECT_TRUE(reader.PeekU8(&val));
  EXPECT_EQ(val, 0x01u);
  EXPECT_EQ(reader.GetBufferConsumed(), 0u); // position unchanged
}

TEST(BinarySpanReader, PeekU16)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(2);
  u16 val;
  EXPECT_TRUE(reader.PeekU16(&val));
  EXPECT_EQ(val, 0x1234u);
  EXPECT_EQ(reader.GetBufferConsumed(), 2u); // position unchanged after peek
}

TEST(BinarySpanReader, PeekU32)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(6);
  u32 val;
  EXPECT_TRUE(reader.PeekU32(&val));
  EXPECT_EQ(val, 0x12345678u);
  EXPECT_EQ(reader.GetBufferConsumed(), 6u);
}

TEST(BinarySpanReader, PeekU64)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(14);
  u64 val;
  EXPECT_TRUE(reader.PeekU64(&val));
  EXPECT_EQ(val, 0x0123456789ABCDEFull);
  EXPECT_EQ(reader.GetBufferConsumed(), 14u);
}

TEST(BinarySpanReader, PeekFloat)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(22);
  float val;
  EXPECT_TRUE(reader.PeekFloat(&val));
  EXPECT_FLOAT_EQ(val, 1.0f);
  EXPECT_EQ(reader.GetBufferConsumed(), 22u);
}

TEST(BinarySpanReader, PeekBool)
{
  std::array<u8, 2> buf = {0x00, 0x01};
  BinarySpanReader reader(buf);
  bool val;
  EXPECT_TRUE(reader.PeekBool(&val));
  EXPECT_FALSE(val);
  EXPECT_EQ(reader.GetBufferConsumed(), 0u);
}

TEST(BinarySpanReader, PeekCString)
{
  BinarySpanReader reader(g_cstring_buffer);
  std::string_view val;
  EXPECT_TRUE(reader.PeekCString(&val));
  EXPECT_EQ(val, "Hello"sv);
  EXPECT_EQ(reader.GetBufferConsumed(), 0u); // position unchanged
}

TEST(BinarySpanReader, PeekSizePrefixedString)
{
  BinarySpanReader reader(g_size_prefixed_buffer);
  std::string_view val;
  EXPECT_TRUE(reader.PeekSizePrefixedString(&val));
  EXPECT_EQ(val, "Hello"sv);
  EXPECT_EQ(reader.GetBufferConsumed(), 0u); // position unchanged
}

TEST(BinarySpanReader, StreamOperators)
{
  BinarySpanReader reader(g_primitive_buffer);
  u8 u8val;
  s8 s8val;
  u16 u16val;
  s16 s16val;
  u32 u32val;
  s32 s32val;
  u64 u64val;
  float fval;

  reader >> u8val >> s8val >> u16val >> s16val >> u32val >> s32val >> u64val >> fval;

  EXPECT_EQ(u8val, 0x01u);
  EXPECT_EQ(s8val, static_cast<s8>(0xFE));
  EXPECT_EQ(u16val, 0x1234u);
  EXPECT_EQ(s16val, static_cast<s16>(0xABCD));
  EXPECT_EQ(u32val, 0x12345678u);
  EXPECT_EQ(s32val, static_cast<s32>(0xEDCBA988));
  EXPECT_EQ(u64val, 0x0123456789ABCDEFull);
  EXPECT_FLOAT_EQ(fval, 1.0f);
}

TEST(BinarySpanReader, StreamOperatorCString)
{
  BinarySpanReader reader(g_cstring_buffer);
  std::string_view val;
  reader >> val;
  EXPECT_EQ(val, "Hello"sv);
}

TEST(BinarySpanReader, ReadOptionalTWithValue)
{
  std::array<u8, 5> buf = {0x01, 0x78, 0x56, 0x34, 0x12}; // has_value=true, value=0x12345678
  BinarySpanReader reader(buf);
  std::optional<u32> val;
  EXPECT_TRUE(reader.ReadOptionalT(&val));
  EXPECT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), 0x12345678u);
}

TEST(BinarySpanReader, ReadOptionalTWithoutValue)
{
  std::array<u8, 5> buf = {0x00, 0x00, 0x00, 0x00, 0x00}; // has_value=false
  BinarySpanReader reader(buf);
  std::optional<u32> val = 123u; // preset to non-empty
  EXPECT_TRUE(reader.ReadOptionalT(&val));
  EXPECT_FALSE(val.has_value());
}

TEST(BinarySpanReader, GetRemainingSpan)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(10);
  auto remaining = reader.GetRemainingSpan();
  EXPECT_EQ(remaining.size(), g_primitive_buffer.size() - 10);
}

TEST(BinarySpanReader, GetRemainingSpanWithSize)
{
  BinarySpanReader reader(g_primitive_buffer);
  reader.IncrementPosition(10);
  auto remaining = reader.GetRemainingSpan(5);
  EXPECT_EQ(remaining.size(), 5u);
}

TEST(BinarySpanReader, ReadBeyondBuffer)
{
  std::array<u8, 2> buf = {0x01, 0x02};
  BinarySpanReader reader(buf);
  u32 val;
  EXPECT_FALSE(reader.ReadU32(&val));
}

TEST(BinarySpanReader, PeekBeyondBuffer)
{
  std::array<u8, 2> buf = {0x01, 0x02};
  BinarySpanReader reader(buf);
  u32 val;
  EXPECT_FALSE(reader.PeekU32(&val));
}

//////////////////////////////////////////////////////////////////////////
// BinarySpanWriter Tests
//////////////////////////////////////////////////////////////////////////

TEST(BinarySpanWriter, DefaultConstructor)
{
  BinarySpanWriter writer;
  EXPECT_FALSE(writer.IsValid());
  EXPECT_EQ(writer.GetBufferRemaining(), 0u);
  EXPECT_EQ(writer.GetBufferWritten(), 0u);
}

TEST(BinarySpanWriter, SpanConstructor)
{
  std::array<u8, 32> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.IsValid());
  EXPECT_EQ(writer.GetBufferRemaining(), buf.size());
  EXPECT_EQ(writer.GetBufferWritten(), 0u);
}

TEST(BinarySpanWriter, MoveConstructor)
{
  std::array<u8, 32> buf = {};
  BinarySpanWriter original(buf);
  original.WriteU8(0x01);
  original.WriteU8(0x02);

  BinarySpanWriter moved(std::move(original));
  EXPECT_TRUE(moved.IsValid());
  EXPECT_EQ(moved.GetBufferWritten(), 2u);
  EXPECT_EQ(original.GetBufferWritten(), 0u);
}

TEST(BinarySpanWriter, MoveAssignment)
{
  std::array<u8, 32> buf = {};
  BinarySpanWriter original(buf);
  original.WriteU8(0x01);

  BinarySpanWriter moved;
  moved = std::move(original);
  EXPECT_TRUE(moved.IsValid());
  EXPECT_EQ(moved.GetBufferWritten(), 1u);
  EXPECT_EQ(original.GetBufferWritten(), 0u);
}

TEST(BinarySpanWriter, GetSpan)
{
  std::array<u8, 32> buf = {};
  BinarySpanWriter writer(buf);
  auto span = writer.GetSpan();
  EXPECT_EQ(span.size(), buf.size());
  EXPECT_EQ(span.data(), buf.data());
}

TEST(BinarySpanWriter, WriteU8)
{
  std::array<u8, 4> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteU8(0xAB));
  EXPECT_EQ(buf[0], 0xABu);
  EXPECT_EQ(writer.GetBufferWritten(), 1u);
}

TEST(BinarySpanWriter, WriteS8)
{
  std::array<u8, 4> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteS8(-5));
  EXPECT_EQ(static_cast<s8>(buf[0]), -5);
}

TEST(BinarySpanWriter, WriteU16)
{
  std::array<u8, 4> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteU16(0x1234));
  EXPECT_EQ(buf[0], 0x34u);
  EXPECT_EQ(buf[1], 0x12u);
}

TEST(BinarySpanWriter, WriteS16)
{
  std::array<u8, 4> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteS16(-1000));
  s16 val;
  std::memcpy(&val, buf.data(), sizeof(val));
  EXPECT_EQ(val, -1000);
}

TEST(BinarySpanWriter, WriteU32)
{
  std::array<u8, 4> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteU32(0x12345678));
  EXPECT_EQ(buf[0], 0x78u);
  EXPECT_EQ(buf[1], 0x56u);
  EXPECT_EQ(buf[2], 0x34u);
  EXPECT_EQ(buf[3], 0x12u);
}

TEST(BinarySpanWriter, WriteS32)
{
  std::array<u8, 4> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteS32(-123456789));
  s32 val;
  std::memcpy(&val, buf.data(), sizeof(val));
  EXPECT_EQ(val, -123456789);
}

TEST(BinarySpanWriter, WriteU64)
{
  std::array<u8, 8> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteU64(0x0123456789ABCDEFull));
  EXPECT_EQ(buf[0], 0xEFu);
  EXPECT_EQ(buf[7], 0x01u);
}

TEST(BinarySpanWriter, WriteS64)
{
  std::array<u8, 8> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteS64(-1234567890123456789ll));
  s64 val;
  std::memcpy(&val, buf.data(), sizeof(val));
  EXPECT_EQ(val, -1234567890123456789ll);
}

TEST(BinarySpanWriter, WriteFloat)
{
  std::array<u8, 4> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteFloat(1.0f));
  float val;
  std::memcpy(&val, buf.data(), sizeof(val));
  EXPECT_FLOAT_EQ(val, 1.0f);
}

TEST(BinarySpanWriter, WriteBool)
{
  std::array<u8, 2> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteBool(false));
  EXPECT_TRUE(writer.WriteBool(true));
  EXPECT_EQ(buf[0], 0x00u);
  EXPECT_EQ(buf[1], 0x01u);
}

TEST(BinarySpanWriter, WriteCString)
{
  std::array<u8, 16> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteCString("Hello"));
  EXPECT_EQ(buf[0], 'H');
  EXPECT_EQ(buf[4], 'o');
  EXPECT_EQ(buf[5], '\0');
  EXPECT_EQ(writer.GetBufferWritten(), 6u);
}

TEST(BinarySpanWriter, WriteCStringEmpty)
{
  std::array<u8, 16> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteCString(""));
  EXPECT_EQ(buf[0], '\0');
  EXPECT_EQ(writer.GetBufferWritten(), 1u);
}

TEST(BinarySpanWriter, WriteSizePrefixedString)
{
  std::array<u8, 16> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteSizePrefixedString("Test"));

  u32 length;
  std::memcpy(&length, buf.data(), sizeof(length));
  EXPECT_EQ(length, 4u);
  EXPECT_EQ(buf[4], 'T');
  EXPECT_EQ(buf[7], 't');
  EXPECT_EQ(writer.GetBufferWritten(), 8u);
}

TEST(BinarySpanWriter, WriteSizePrefixedStringEmpty)
{
  std::array<u8, 16> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_TRUE(writer.WriteSizePrefixedString(""));

  u32 length;
  std::memcpy(&length, buf.data(), sizeof(length));
  EXPECT_EQ(length, 0u);
  EXPECT_EQ(writer.GetBufferWritten(), 4u);
}

TEST(BinarySpanWriter, StreamOperators)
{
  std::array<u8, 32> buf = {};
  BinarySpanWriter writer(buf);

  writer << static_cast<u8>(0x01) << static_cast<s8>(0xFE) << static_cast<u16>(0x1234) << static_cast<s16>(0xABCD)
         << static_cast<u32>(0x12345678) << static_cast<s32>(0xEDCBA988) << static_cast<u64>(0x0123456789ABCDEFull)
         << 1.0f;

  EXPECT_EQ(writer.GetBufferWritten(), 26u);

  // Verify by reading back
  BinarySpanReader reader(std::span<const u8>(buf.data(), writer.GetBufferWritten()));
  EXPECT_EQ(reader.ReadU8(), 0x01u);
  EXPECT_EQ(reader.ReadS8(), static_cast<s8>(0xFE));
  EXPECT_EQ(reader.ReadU16(), 0x1234u);
  EXPECT_EQ(reader.ReadS16(), static_cast<s16>(0xABCD));
  EXPECT_EQ(reader.ReadU32(), 0x12345678u);
  EXPECT_EQ(reader.ReadS32(), static_cast<s32>(0xEDCBA988));
  EXPECT_EQ(reader.ReadU64(), 0x0123456789ABCDEFull);
  EXPECT_FLOAT_EQ(reader.ReadFloat(), 1.0f);
}

TEST(BinarySpanWriter, StreamOperatorCString)
{
  std::array<u8, 16> buf = {};
  BinarySpanWriter writer(buf);
  writer << "Hello"sv;

  BinarySpanReader reader(std::span<const u8>(buf.data(), writer.GetBufferWritten()));
  EXPECT_EQ(reader.ReadCString(), "Hello"sv);
}

TEST(BinarySpanWriter, WriteOptionalTWithValue)
{
  std::array<u8, 8> buf = {};
  BinarySpanWriter writer(buf);
  std::optional<u32> val = 0x12345678u;
  EXPECT_TRUE(writer.WriteOptionalT(val));
  EXPECT_EQ(writer.GetBufferWritten(), 5u);

  BinarySpanReader reader(std::span<const u8>(buf.data(), writer.GetBufferWritten()));
  EXPECT_TRUE(reader.ReadBool());
  EXPECT_EQ(reader.ReadU32(), 0x12345678u);
}

TEST(BinarySpanWriter, WriteOptionalTWithoutValue)
{
  std::array<u8, 8> buf = {};
  BinarySpanWriter writer(buf);
  std::optional<u32> val;
  EXPECT_TRUE(writer.WriteOptionalT(val));
  EXPECT_EQ(writer.GetBufferWritten(), 1u);

  BinarySpanReader reader(std::span<const u8>(buf.data(), writer.GetBufferWritten()));
  EXPECT_FALSE(reader.ReadBool());
}

TEST(BinarySpanWriter, GetRemainingSpan)
{
  std::array<u8, 32> buf = {};
  BinarySpanWriter writer(buf);
  writer.IncrementPosition(10);
  auto remaining = writer.GetRemainingSpan();
  EXPECT_EQ(remaining.size(), buf.size() - 10);
}

TEST(BinarySpanWriter, GetRemainingSpanWithSize)
{
  std::array<u8, 32> buf = {};
  BinarySpanWriter writer(buf);
  writer.IncrementPosition(10);
  auto remaining = writer.GetRemainingSpan(5);
  EXPECT_EQ(remaining.size(), 5u);
}

TEST(BinarySpanWriter, WriteBeyondBuffer)
{
  std::array<u8, 2> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_FALSE(writer.WriteU32(0x12345678));
}

TEST(BinarySpanWriter, WriteCStringBeyondBuffer)
{
  std::array<u8, 4> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_FALSE(writer.WriteCString("Hello")); // needs 6 bytes
}

TEST(BinarySpanWriter, WriteSizePrefixedStringBeyondBuffer)
{
  std::array<u8, 4> buf = {};
  BinarySpanWriter writer(buf);
  EXPECT_FALSE(writer.WriteSizePrefixedString("Test")); // needs 8 bytes
}

//////////////////////////////////////////////////////////////////////////
// Round-trip Tests (Writer -> Reader)
//////////////////////////////////////////////////////////////////////////

TEST(BinarySpanRoundTrip, AllPrimitiveTypes)
{
  std::array<u8, 64> buf = {};
  BinarySpanWriter writer(buf);

  writer.WriteU8(0xAB);
  writer.WriteS8(-42);
  writer.WriteU16(0x1234);
  writer.WriteS16(-1000);
  writer.WriteU32(0xDEADBEEF);
  writer.WriteS32(-123456789);
  writer.WriteU64(0xFEDCBA9876543210ull);
  writer.WriteS64(-876543210123456789ll);
  writer.WriteFloat(3.14159f);
  writer.WriteBool(true);

  BinarySpanReader reader(std::span<const u8>(buf.data(), writer.GetBufferWritten()));
  EXPECT_EQ(reader.ReadU8(), 0xABu);
  EXPECT_EQ(reader.ReadS8(), -42);
  EXPECT_EQ(reader.ReadU16(), 0x1234u);
  EXPECT_EQ(reader.ReadS16(), -1000);
  EXPECT_EQ(reader.ReadU32(), 0xDEADBEEFu);
  EXPECT_EQ(reader.ReadS32(), -123456789);
  EXPECT_EQ(reader.ReadU64(), 0xFEDCBA9876543210ull);
  EXPECT_EQ(reader.ReadS64(), -876543210123456789ll);
  EXPECT_FLOAT_EQ(reader.ReadFloat(), 3.14159f);
  EXPECT_TRUE(reader.ReadBool());
}

TEST(BinarySpanRoundTrip, CStrings)
{
  std::array<u8, 64> buf = {};
  BinarySpanWriter writer(buf);

  writer.WriteCString("Hello");
  writer.WriteCString("");
  writer.WriteCString("World!");

  BinarySpanReader reader(std::span<const u8>(buf.data(), writer.GetBufferWritten()));
  EXPECT_EQ(reader.ReadCString(), "Hello"sv);
  EXPECT_EQ(reader.ReadCString(), ""sv);
  EXPECT_EQ(reader.ReadCString(), "World!"sv);
}

TEST(BinarySpanRoundTrip, SizePrefixedStrings)
{
  std::array<u8, 64> buf = {};
  BinarySpanWriter writer(buf);

  writer.WriteSizePrefixedString("Testing");
  writer.WriteSizePrefixedString("");
  writer.WriteSizePrefixedString("123");

  BinarySpanReader reader(std::span<const u8>(buf.data(), writer.GetBufferWritten()));
  EXPECT_EQ(reader.ReadSizePrefixedString(), "Testing"sv);
  EXPECT_EQ(reader.ReadSizePrefixedString(), ""sv);
  EXPECT_EQ(reader.ReadSizePrefixedString(), "123"sv);
}

TEST(BinarySpanRoundTrip, OptionalValues)
{
  std::array<u8, 32> buf = {};
  BinarySpanWriter writer(buf);

  std::optional<u32> val1 = 12345u;
  std::optional<u32> val2;
  std::optional<u16> val3 = u16(9999);

  writer.WriteOptionalT(val1);
  writer.WriteOptionalT(val2);
  writer.WriteOptionalT(val3);

  BinarySpanReader reader(std::span<const u8>(buf.data(), writer.GetBufferWritten()));

  std::optional<u32> read1, read2;
  std::optional<u16> read3;

  EXPECT_TRUE(reader.ReadOptionalT(&read1));
  EXPECT_TRUE(reader.ReadOptionalT(&read2));
  EXPECT_TRUE(reader.ReadOptionalT(&read3));

  EXPECT_TRUE(read1.has_value());
  EXPECT_EQ(read1.value(), 12345u);
  EXPECT_FALSE(read2.has_value());
  EXPECT_TRUE(read3.has_value());
  EXPECT_EQ(read3.value(), 9999u);
}

TEST(BinarySpanRoundTrip, MixedContent)
{
  std::array<u8, 128> buf = {};
  BinarySpanWriter writer(buf);

  writer.WriteU32(0x12345678);
  writer.WriteCString("Header");
  writer.WriteU16(100);
  writer.WriteSizePrefixedString("Payload");
  writer.WriteFloat(2.5f);
  writer.WriteBool(false);

  BinarySpanReader reader(std::span<const u8>(buf.data(), writer.GetBufferWritten()));

  EXPECT_EQ(reader.ReadU32(), 0x12345678u);
  EXPECT_EQ(reader.ReadCString(), "Header"sv);
  EXPECT_EQ(reader.ReadU16(), 100u);
  EXPECT_EQ(reader.ReadSizePrefixedString(), "Payload"sv);
  EXPECT_FLOAT_EQ(reader.ReadFloat(), 2.5f);
  EXPECT_FALSE(reader.ReadBool());
}