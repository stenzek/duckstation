#include "util/shiftjis.h"

#include "common/string_util.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

#define UNICODE_REPLACEMENT_CHARACTER_STR "\xef\xbf\xbd" // "\uFFFD".encode("utf-8")

static std::string Bytes(std::initializer_list<unsigned int> values)
{
  std::string result;
  result.reserve(values.size());
  for (const unsigned int value : values)
  {
    if (value > 0xFFu)
    {
      ADD_FAILURE() << "test byte is outside the range 0..255";
      return {};
    }
    result.push_back(static_cast<char>(value));
  }
  return result;
}

static std::string EncodeUtf8(std::uint32_t cp)
{
  if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
  {
    ADD_FAILURE() << "invalid Unicode scalar in test";
    return {};
  }

  std::string out;
  if (cp <= 0x7Fu)
  {
    out.push_back(static_cast<char>(cp));
  }
  else if (cp <= 0x7FFu)
  {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
  else if (cp <= 0xFFFFu)
  {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
  else
  {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
  return out;
}

static std::optional<std::vector<std::uint32_t>> DecodeUtf8Strict(std::string_view input)
{
  std::vector<std::uint32_t> result;

  std::size_t i = 0;
  while (i < input.size())
  {
    const auto b0 = static_cast<unsigned char>(input[i]);
    std::uint32_t cp = 0;
    std::size_t length = 0;

    if (b0 <= 0x7Fu)
    {
      cp = b0;
      length = 1;
    }
    else if (b0 >= 0xC2u && b0 <= 0xDFu)
    {
      cp = b0 & 0x1Fu;
      length = 2;
    }
    else if (b0 >= 0xE0u && b0 <= 0xEFu)
    {
      cp = b0 & 0x0Fu;
      length = 3;
    }
    else if (b0 >= 0xF0u && b0 <= 0xF4u)
    {
      cp = b0 & 0x07u;
      length = 4;
    }
    else
    {
      return std::nullopt;
    }

    if (i + length > input.size())
    {
      return std::nullopt;
    }

    for (std::size_t j = 1; j < length; ++j)
    {
      const auto continuation = static_cast<unsigned char>(input[i + j]);
      if ((continuation & 0xC0u) != 0x80u)
      {
        return std::nullopt;
      }
      cp = (cp << 6) | (continuation & 0x3Fu);
    }

    if ((length == 2 && cp < 0x80u) || (length == 3 && cp < 0x800u) || (length == 4 && cp < 0x10000u) ||
        cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
    {
      return std::nullopt;
    }

    result.push_back(cp);
    i += length;
  }

  return result;
}

static bool IsLeadByte(unsigned int byte)
{
  return (byte >= 0x81u && byte <= 0x9Fu) || (byte >= 0xE0u && byte <= 0xFCu);
}

static bool IsTrailByte(unsigned int byte)
{
  return (byte >= 0x40u && byte <= 0x7Eu) || (byte >= 0x80u && byte <= 0xFCu);
}

static std::uint32_t ExpectedSingleByteCodePoint(unsigned int byte)
{
  if (byte <= 0x7Fu)
  {
    // ICU ibm-943_P15A-2003 round-trip control mappings.
    if (byte == 0x1Au)
      return 0x001Cu;
    if (byte == 0x1Cu)
      return 0x007Fu;
    if (byte == 0x7Fu)
      return 0x001Au;

    return byte;
  }
  if (byte >= 0xA1u && byte <= 0xDFu)
    return 0xFF61u + (byte - 0xA1u);

  ADD_FAILURE() << "not a valid one-byte Shift-JIS code unit";
  return StringUtil::UNICODE_REPLACEMENT_CHARACTER;
}

TEST(ConvertShiftJISToUTF8Test, EmptyInputProducesEmptyOutput)
{
  EXPECT_TRUE(ShiftJIS::ConvertShiftJISToUTF8({}).empty());
}

TEST(ConvertShiftJISToUTF8Test, ConvertsEveryValidSingleByteCodeUnit)
{
  for (unsigned int byte = 1; byte <= 0x7Fu; ++byte)
  {
    const std::string input = Bytes({byte});
    const std::string expected = EncodeUtf8(ExpectedSingleByteCodePoint(byte));
    SCOPED_TRACE(fmt::format("byte: {:02X}", byte));
    EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(input), expected);
  }
  for (unsigned int byte = 0xA1u; byte <= 0xDFu; ++byte)
  {
    const std::string input = Bytes({byte});
    const std::string expected = EncodeUtf8(ExpectedSingleByteCodePoint(byte));
    SCOPED_TRACE(fmt::format("byte: {:02X}", byte));
    EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(input), expected);
  }
}

TEST(ConvertShiftJISToUTF8Test, NulByteTerminatesInput)
{
  EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(Bytes({0x00, 'A'})), "");
  EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(Bytes({'A', 0x00, 'B', 0x82, 0xA0})), "A");
  EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(Bytes({0x82, 0x00, 'A'})), UNICODE_REPLACEMENT_CHARACTER_STR);
}

TEST(ConvertShiftJISToUTF8Test, ConvertsRepresentativeStandardCharacters)
{
  struct Case
  {
    std::string input;
    std::string expected;
  };

  const std::array<Case, 6> cases = {{
    {Bytes({0x81, 0x40}), Bytes({0xE3, 0x80, 0x80})}, // U+3000 IDEOGRAPHIC SPACE
    {Bytes({0x82, 0xA0}), Bytes({0xE3, 0x81, 0x82})}, // U+3042 HIRAGANA LETTER A
    {Bytes({0x83, 0x41}), Bytes({0xE3, 0x82, 0xA2})}, // U+30A2 KATAKANA LETTER A
    {Bytes({0x93, 0xFA}), Bytes({0xE6, 0x97, 0xA5})}, // U+65E5
    {Bytes({0x96, 0x7B}), Bytes({0xE6, 0x9C, 0xAC})}, // U+672C
    {Bytes({0x8C, 0xEA}), Bytes({0xE8, 0xAA, 0x9E})}, // U+8A9E
  }};

  for (const Case& test_case : cases)
  {
    SCOPED_TRACE("Shift-JIS bytes: " + StringUtil::EncodeHex(test_case.input.data(), test_case.input.size()));
    EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(test_case.input), test_case.expected);
  }
}

TEST(ConvertShiftJISToUTF8Test, ConvertsJapaneseText)
{
  const std::string input = Bytes({0x93, 0xFA, 0x96, 0x7B, 0x8C, 0xEA}); // 日本語
  const std::string expected = Bytes({0xE6, 0x97, 0xA5, 0xE6, 0x9C, 0xAC, 0xE8, 0xAA, 0x9E});

  EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(input), expected);
}

TEST(ConvertShiftJISToUTF8Test, ConvertsWindows932Extensions)
{
  struct Case
  {
    std::string input;
    std::string expected;
  };

  const std::array<Case, 4> cases = {{
    {Bytes({0x87, 0x40}), EncodeUtf8(0x2460)}, // CIRCLED DIGIT ONE
    {Bytes({0xED, 0x40}), EncodeUtf8(0x7E8A)}, // CJK compatibility extension
    {Bytes({0xFA, 0x40}), EncodeUtf8(0x2170)}, // SMALL ROMAN NUMERAL ONE
    {Bytes({0xFC, 0x4B}), EncodeUtf8(0x9ED1)}, // Last assigned table entry
  }};

  for (const Case& test_case : cases)
  {
    SCOPED_TRACE("Shift-JIS bytes: " + StringUtil::EncodeHex(test_case.input.data(), test_case.input.size()));
    EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(test_case.input), test_case.expected);
  }
}

TEST(ConvertShiftJISToUTF8Test, ConvertsPrivateUseAreaMapping)
{
  EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(Bytes({0xF0, 0x40})), EncodeUtf8(0xE000));
}

TEST(ConvertShiftJISToUTF8Test, RejectsEveryInvalidOneByteInput)
{
  for (unsigned int byte = 0; byte <= 0xFFu; ++byte)
  {
    if (byte <= 0x7Fu || (byte >= 0xA1u && byte <= 0xDFu))
      continue;

    SCOPED_TRACE(fmt::format("byte: {:02X}", byte));
    ASSERT_EQ(ShiftJIS::ConvertShiftJISToUTF8(Bytes({byte})), UNICODE_REPLACEMENT_CHARACTER_STR);
  }
}

TEST(ConvertShiftJISToUTF8Test, RejectsEveryIllegalTrailByteForEveryLead)
{
  for (unsigned int lead = 0; lead <= 0xFFu; ++lead)
  {
    if (!IsLeadByte(lead))
      continue;

    for (unsigned int trail = 0; trail <= 0xFFu; ++trail)
    {
      if (IsTrailByte(trail))
        continue;

      if (lead == 0 && trail == 0)
        continue;

      const std::string input = Bytes({lead, trail});
      SCOPED_TRACE("bytes: " + StringUtil::EncodeHex(input.data(), input.size()));
      ASSERT_EQ(ShiftJIS::ConvertShiftJISToUTF8(input), UNICODE_REPLACEMENT_CHARACTER_STR);
    }
  }
}

TEST(ConvertShiftJISToUTF8Test, RejectsSyntacticallyValidButUnassignedPair)
{
  EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(Bytes({0x81, 0xAD})), UNICODE_REPLACEMENT_CHARACTER_STR);
}

TEST(ConvertShiftJISToUTF8Test, ReplacesInvalidSequencesAtAnyOffset)
{
  // invalid or unassigned Shift-JIS byte sequence at offset 0
  ASSERT_EQ(ShiftJIS::ConvertShiftJISToUTF8(Bytes({0x80})), UNICODE_REPLACEMENT_CHARACTER_STR);

  // invalid or unassigned Shift-JIS byte sequence at offset 1
  ASSERT_EQ(ShiftJIS::ConvertShiftJISToUTF8(Bytes({'A', 0x82})), "A" UNICODE_REPLACEMENT_CHARACTER_STR);

  // invalid or unassigned Shift-JIS byte sequence at offset 2
  ASSERT_EQ(ShiftJIS::ConvertShiftJISToUTF8(Bytes({'A', 0x82, 0x20})), "A" UNICODE_REPLACEMENT_CHARACTER_STR);

  // invalid or unassigned Shift-JIS byte sequence at offset 3
  ASSERT_EQ(ShiftJIS::ConvertShiftJISToUTF8(Bytes({0x82, 0xA0, 'B', 0x81, 0xAD})),
            "\xe3\x81\x82"
            "B" UNICODE_REPLACEMENT_CHARACTER_STR);
}

TEST(ConvertShiftJISToUTF8Test, InvalidLeadDoesNotSkipFollowingCodeUnit)
{
  EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(Bytes({0x80, 'A'})), UNICODE_REPLACEMENT_CHARACTER_STR "A");
  EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(Bytes({0x80, 0x00, 'A'})), UNICODE_REPLACEMENT_CHARACTER_STR);
}

TEST(ConvertShiftJISToUTF8Test, ExhaustivelyClassifiesEveryPossibleDoubleByteCodeUnit)
{
  std::size_t assigned_count = 0;
  std::size_t unassigned_count = 0;

  for (unsigned int lead = 0; lead <= 0xFFu; ++lead)
  {
    if (!IsLeadByte(lead))
      continue;

    for (unsigned int trail = 0; trail <= 0xFFu; ++trail)
    {
      if (!IsTrailByte(trail))
        continue;

      const std::string input = Bytes({lead, trail});
      SCOPED_TRACE("bytes: " + StringUtil::EncodeHex(input.data(), input.size()));

      const std::string output = ShiftJIS::ConvertShiftJISToUTF8(input);
      const auto code_points = DecodeUtf8Strict(output);
      ASSERT_TRUE(code_points.has_value())
        << "converter returned malformed UTF-8: " << StringUtil::EncodeHex(output.data(), output.size());
      ASSERT_EQ(code_points->size(), 1u) << "one Shift-JIS code unit must produce one Unicode scalar";
      if (code_points->front() == StringUtil::UNICODE_REPLACEMENT_CHARACTER)
      {
        unassigned_count++;
        continue;
      }

      EXPECT_LE(code_points->front(), 0xFFFFu) << "the selected ICU Shift-JIS table contains only BMP values";
      assigned_count++;
    }
  }

  // Exact counts for ICU ibm-943_P15A-2003 / windows-932.
  EXPECT_EQ(assigned_count, 9604u);
  EXPECT_EQ(unassigned_count, 1676u);
  EXPECT_EQ(assigned_count + unassigned_count, 11280u);
}

TEST(ConvertShiftJISToUTF8Test, ConvertingConcatenatedCodeUnitsMatchesIndividualConversions)
{
  std::string combined_input;
  std::string combined_expected;

  for (unsigned int byte = 1; byte <= 0x7Fu; ++byte)
  {
    const std::string code_unit = Bytes({byte});
    combined_input += code_unit;
    combined_expected += ShiftJIS::ConvertShiftJISToUTF8(code_unit);
  }
  for (unsigned int byte = 0xA1u; byte <= 0xDFu; ++byte)
  {
    const std::string code_unit = Bytes({byte});
    combined_input += code_unit;
    combined_expected += ShiftJIS::ConvertShiftJISToUTF8(code_unit);
  }
  for (unsigned int lead = 0; lead <= 0xFFu; ++lead)
  {
    if (!IsLeadByte(lead))
      continue;

    for (unsigned int trail = 0; trail <= 0xFFu; ++trail)
    {
      if (!IsTrailByte(trail))
        continue;

      const std::string code_unit = Bytes({lead, trail});
      const std::string converted = ShiftJIS::ConvertShiftJISToUTF8(code_unit);
      combined_input += code_unit;
      combined_expected += converted;
    }
  }

  combined_input.push_back('\0');
  EXPECT_EQ(ShiftJIS::ConvertShiftJISToUTF8(combined_input), combined_expected);
}

TEST(ConvertShiftJISToUTF8Test, ConvertFullWidthToASCII)
{
  EXPECT_EQ(ShiftJIS::ConvertFullWidthToASCII("ＡＢＣ　１２３！"), "ABC 123!");
  EXPECT_EQ(ShiftJIS::ConvertFullWidthToASCII("ａｂｃ＠ｘｙｚ．ｃｏｍ"), "abc@xyz.com");

  // Non-full-width Japanese is preserved.
  EXPECT_EQ(ShiftJIS::ConvertFullWidthToASCII("日本語テスト"), "日本語テスト");

  // Mixed text.
  EXPECT_EQ(ShiftJIS::ConvertFullWidthToASCII("ＳＡＶＥ　データ　０１"), "SAVE データ 01");
}
