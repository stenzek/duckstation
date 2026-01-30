// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/string_pool.h"
#include "common/string_util.h"

#include <gtest/gtest.h>
#include <string_view>
#include <tuple>

using namespace std::string_view_literals;

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

TEST(StringUtil, Base64EncodeDecode)
{
  struct TestCase
  {
    const char* hexString;
    const char* base64String;
  };
  static const TestCase testCases[] = {
    {"33326a6f646933326a68663937683732383368", "MzJqb2RpMzJqaGY5N2g3MjgzaA=="},
    {"32753965333268756979386672677537366967723839683432703075693132393065755c5d0931325c335c31323439303438753839333272",
     "MnU5ZTMyaHVpeThmcmd1NzZpZ3I4OWg0MnAwdWkxMjkwZXVcXQkxMlwzXDEyNDkwNDh1ODkzMnI="},
    {"3332726a33323738676838666233326830393233386637683938323139", "MzJyajMyNzhnaDhmYjMyaDA5MjM4ZjdoOTgyMTk="},
    {"9956967BE9C96E10B27FF8897A5B768A2F4B103CE934718D020FE6B5B770", "mVaWe+nJbhCyf/iJelt2ii9LEDzpNHGNAg/mtbdw"},
    {"BC94251814827A5D503D62D5EE6CBAB0FD55D2E2FCEDBB2261D6010084B95DD648766D8983F03AFA3908956D8201E26BB09FE52B515A61A9E"
     "1D3ADC207BD9E622128F22929CDED456B595A410F7168B0BA6370289E6291E38E47C18278561C79A7297C21D23C06BB2F694DC2F65FAAF994"
     "59E3FC14B1FA415A3320AF00ACE54C00BE",
     "vJQlGBSCel1QPWLV7my6sP1V0uL87bsiYdYBAIS5XdZIdm2Jg/A6+jkIlW2CAeJrsJ/"
     "lK1FaYanh063CB72eYiEo8ikpze1Fa1laQQ9xaLC6Y3AonmKR445HwYJ4Vhx5pyl8IdI8BrsvaU3C9l+q+ZRZ4/wUsfpBWjMgrwCs5UwAvg=="},
    {"192B42CB0F66F69BE8A5", "GStCyw9m9pvopQ=="},
    {"38ABD400F3BB6960EB60C056719B5362", "OKvUAPO7aWDrYMBWcZtTYg=="},
    {"776FAB27DC7F8DA86F298D55B69F8C278D53871F8CBCCF", "d2+rJ9x/jahvKY1Vtp+MJ41Thx+MvM8="},
    {"B1ED3EA2E35EE69C7E16707B05042A", "se0+ouNe5px+FnB7BQQq"},
  };

  for (const TestCase& tc : testCases)
  {
    std::optional<std::vector<u8>> bytes = StringUtil::DecodeHex(tc.hexString);
    ASSERT_TRUE(bytes.has_value());

    std::string encoded_b64 = StringUtil::EncodeBase64(bytes.value());
    ASSERT_EQ(encoded_b64, tc.base64String);

    std::optional<std::vector<u8>> dbytes = StringUtil::DecodeBase64(tc.base64String);
    ASSERT_TRUE(dbytes.has_value());
    ASSERT_EQ(dbytes.value(), bytes.value());
  }
}

TEST(StringUtil, CompareNoCase)
{
  // Test identical strings
  ASSERT_EQ(StringUtil::CompareNoCase("hello", "hello"), 0);
  ASSERT_EQ(StringUtil::CompareNoCase("", ""), 0);

  // Test case insensitive comparison - should be equal
  ASSERT_EQ(StringUtil::CompareNoCase("Hello", "hello"), 0);
  ASSERT_EQ(StringUtil::CompareNoCase("HELLO", "hello"), 0);
  ASSERT_EQ(StringUtil::CompareNoCase("hello", "HELLO"), 0);
  ASSERT_EQ(StringUtil::CompareNoCase("HeLLo", "hEllO"), 0);
  ASSERT_EQ(StringUtil::CompareNoCase("WoRlD", "world"), 0);

  // Test different strings - first string lexicographically less than second
  ASSERT_LT(StringUtil::CompareNoCase("apple", "banana"), 0);
  ASSERT_LT(StringUtil::CompareNoCase("Apple", "BANANA"), 0);
  ASSERT_LT(StringUtil::CompareNoCase("APPLE", "banana"), 0);
  ASSERT_LT(StringUtil::CompareNoCase("aaa", "aab"), 0);

  // Test different strings - first string lexicographically greater than second
  ASSERT_GT(StringUtil::CompareNoCase("zebra", "apple"), 0);
  ASSERT_GT(StringUtil::CompareNoCase("ZEBRA", "apple"), 0);
  ASSERT_GT(StringUtil::CompareNoCase("zebra", "APPLE"), 0);
  ASSERT_GT(StringUtil::CompareNoCase("aab", "aaa"), 0);

  // Test different length strings - shorter vs longer
  ASSERT_LT(StringUtil::CompareNoCase("abc", "abcd"), 0);
  ASSERT_GT(StringUtil::CompareNoCase("abcd", "abc"), 0);
  ASSERT_LT(StringUtil::CompareNoCase("ABC", "abcd"), 0);
  ASSERT_GT(StringUtil::CompareNoCase("ABCD", "abc"), 0);

  // Test empty string comparisons
  ASSERT_GT(StringUtil::CompareNoCase("hello", ""), 0);
  ASSERT_LT(StringUtil::CompareNoCase("", "hello"), 0);
  ASSERT_GT(StringUtil::CompareNoCase("A", ""), 0);
  ASSERT_LT(StringUtil::CompareNoCase("", "a"), 0);

  // Test strings with numbers and special characters
  ASSERT_EQ(StringUtil::CompareNoCase("Test123", "test123"), 0);
  ASSERT_EQ(StringUtil::CompareNoCase("Hello_World", "hello_world"), 0);
  ASSERT_LT(StringUtil::CompareNoCase("Test1", "Test2"), 0);
  ASSERT_GT(StringUtil::CompareNoCase("Test2", "Test1"), 0);
  ASSERT_EQ(StringUtil::CompareNoCase("File.txt", "FILE.TXT"), 0);

  // Test prefix scenarios
  ASSERT_LT(StringUtil::CompareNoCase("test", "testing"), 0);
  ASSERT_GT(StringUtil::CompareNoCase("testing", "test"), 0);
  ASSERT_LT(StringUtil::CompareNoCase("TEST", "testing"), 0);

  // Test single character differences
  ASSERT_LT(StringUtil::CompareNoCase("a", "b"), 0);
  ASSERT_GT(StringUtil::CompareNoCase("B", "a"), 0);
  ASSERT_EQ(StringUtil::CompareNoCase("A", "a"), 0);
  ASSERT_EQ(StringUtil::CompareNoCase("z", "Z"), 0);
}

// New tests for methods not already covered

TEST(StringUtil, ToLowerToUpper)
{
  // Test ToLower
  ASSERT_EQ(StringUtil::ToLower('A'), 'a');
  ASSERT_EQ(StringUtil::ToLower('Z'), 'z');
  ASSERT_EQ(StringUtil::ToLower('M'), 'm');
  ASSERT_EQ(StringUtil::ToLower('a'), 'a'); // Already lowercase
  ASSERT_EQ(StringUtil::ToLower('z'), 'z'); // Already lowercase
  ASSERT_EQ(StringUtil::ToLower('1'), '1'); // Non-alphabetic
  ASSERT_EQ(StringUtil::ToLower('!'), '!'); // Non-alphabetic
  ASSERT_EQ(StringUtil::ToLower(' '), ' '); // Space

  // Test ToUpper
  ASSERT_EQ(StringUtil::ToUpper('a'), 'A');
  ASSERT_EQ(StringUtil::ToUpper('z'), 'Z');
  ASSERT_EQ(StringUtil::ToUpper('m'), 'M');
  ASSERT_EQ(StringUtil::ToUpper('A'), 'A'); // Already uppercase
  ASSERT_EQ(StringUtil::ToUpper('Z'), 'Z'); // Already uppercase
  ASSERT_EQ(StringUtil::ToUpper('1'), '1'); // Non-alphabetic
  ASSERT_EQ(StringUtil::ToUpper('!'), '!'); // Non-alphabetic
  ASSERT_EQ(StringUtil::ToUpper(' '), ' '); // Space
}

TEST(StringUtil, WildcardMatch)
{
  // Basic wildcard tests
  ASSERT_TRUE(StringUtil::WildcardMatch("test", "test"));
  ASSERT_TRUE(StringUtil::WildcardMatch("test", "*"));
  ASSERT_TRUE(StringUtil::WildcardMatch("test", "t*"));
  ASSERT_TRUE(StringUtil::WildcardMatch("test", "*t"));
  ASSERT_TRUE(StringUtil::WildcardMatch("test", "te*"));
  ASSERT_TRUE(StringUtil::WildcardMatch("test", "*st"));
  ASSERT_TRUE(StringUtil::WildcardMatch("test", "t*t"));
  ASSERT_TRUE(StringUtil::WildcardMatch("test", "?est"));
  ASSERT_TRUE(StringUtil::WildcardMatch("test", "t?st"));
  ASSERT_TRUE(StringUtil::WildcardMatch("test", "tes?"));
  ASSERT_TRUE(StringUtil::WildcardMatch("test", "????"));

  // Negative tests
  ASSERT_FALSE(StringUtil::WildcardMatch("test", "best"));
  ASSERT_FALSE(StringUtil::WildcardMatch("test", "tests"));
  ASSERT_FALSE(StringUtil::WildcardMatch("test", "???"));
  ASSERT_FALSE(StringUtil::WildcardMatch("test", "?????"));

  // Case sensitivity tests
  ASSERT_TRUE(StringUtil::WildcardMatch("Test", "test", false));
  ASSERT_FALSE(StringUtil::WildcardMatch("Test", "test", true));
  ASSERT_TRUE(StringUtil::WildcardMatch("TEST", "*est", false));
  ASSERT_FALSE(StringUtil::WildcardMatch("TEST", "*est", true));

  // Empty string tests
  ASSERT_TRUE(StringUtil::WildcardMatch("", ""));
  ASSERT_TRUE(StringUtil::WildcardMatch("", "*"));
  ASSERT_FALSE(StringUtil::WildcardMatch("", "?"));
  ASSERT_FALSE(StringUtil::WildcardMatch("test", ""));
}

TEST(StringUtil, Strlcpy)
{
  char buffer[10];

  // Normal copy
  std::size_t result = StringUtil::Strlcpy(buffer, "hello", sizeof(buffer));
  ASSERT_EQ(result, 5u);
  ASSERT_STREQ(buffer, "hello");

  // Truncation test
  result = StringUtil::Strlcpy(buffer, "hello world", sizeof(buffer));
  ASSERT_EQ(result, 11u);            // Should return original string length
  ASSERT_STREQ(buffer, "hello wor"); // Should be truncated and null-terminated

  // Empty string
  result = StringUtil::Strlcpy(buffer, "", sizeof(buffer));
  ASSERT_EQ(result, 0u);
  ASSERT_STREQ(buffer, "");

  // Buffer size 1 (only null terminator)
  result = StringUtil::Strlcpy(buffer, "test", 1);
  ASSERT_EQ(result, 4u);
  ASSERT_STREQ(buffer, "");

  // Test with string_view
  std::string_view sv = "test string";
  result = StringUtil::Strlcpy(buffer, sv, sizeof(buffer));
  ASSERT_EQ(result, 11u);
  ASSERT_STREQ(buffer, "test stri");
}

TEST(StringUtil, Strnlen)
{
  const char* str = "hello world";
  ASSERT_EQ(StringUtil::Strnlen(str, 100), 11u);
  ASSERT_EQ(StringUtil::Strnlen(str, 5), 5u);
  ASSERT_EQ(StringUtil::Strnlen(str, 0), 0u);
  ASSERT_EQ(StringUtil::Strnlen("", 10), 0u);
}

TEST(StringUtil, Strcasecmp)
{
  ASSERT_EQ(StringUtil::Strcasecmp("hello", "hello"), 0);
  ASSERT_EQ(StringUtil::Strcasecmp("Hello", "hello"), 0);
  ASSERT_EQ(StringUtil::Strcasecmp("HELLO", "hello"), 0);
  ASSERT_LT(StringUtil::Strcasecmp("apple", "banana"), 0);
  ASSERT_GT(StringUtil::Strcasecmp("zebra", "apple"), 0);
}

TEST(StringUtil, Strncasecmp)
{
  ASSERT_EQ(StringUtil::Strncasecmp("hello", "hello", 5), 0);
  ASSERT_EQ(StringUtil::Strncasecmp("Hello", "hello", 5), 0);
  ASSERT_EQ(StringUtil::Strncasecmp("hello world", "hello test", 5), 0);
  ASSERT_NE(StringUtil::Strncasecmp("hello world", "hello test", 10), 0);
}

TEST(StringUtil, EqualNoCase)
{
  ASSERT_TRUE(StringUtil::EqualNoCase("hello", "hello"));
  ASSERT_TRUE(StringUtil::EqualNoCase("Hello", "hello"));
  ASSERT_TRUE(StringUtil::EqualNoCase("HELLO", "hello"));
  ASSERT_TRUE(StringUtil::EqualNoCase("", ""));
  ASSERT_FALSE(StringUtil::EqualNoCase("hello", "world"));
  ASSERT_FALSE(StringUtil::EqualNoCase("hello", "hello world"));
  ASSERT_FALSE(StringUtil::EqualNoCase("hello world", "hello"));
}

TEST(StringUtil, ContainsNoCase)
{
  ASSERT_TRUE(StringUtil::ContainsNoCase("hello world", "world"));
  ASSERT_TRUE(StringUtil::ContainsNoCase("hello world", "WORLD"));
  ASSERT_TRUE(StringUtil::ContainsNoCase("Hello World", "lo wo"));
  ASSERT_TRUE(StringUtil::ContainsNoCase("test", "test"));
  ASSERT_TRUE(StringUtil::ContainsNoCase("test", ""));
  ASSERT_FALSE(StringUtil::ContainsNoCase("hello", "world"));
  ASSERT_FALSE(StringUtil::ContainsNoCase("test", "testing"));
}

TEST(StringUtil, FromCharsIntegral)
{
  // Test integers
  auto result = StringUtil::FromChars<int>("123");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(*result, 123);

  result = StringUtil::FromChars<int>("-456");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(*result, -456);

  // Test hex
  auto hex_result = StringUtil::FromChars<int>("FF", 16);
  ASSERT_TRUE(hex_result.has_value());
  ASSERT_EQ(*hex_result, 255);

  // Test invalid input
  auto invalid = StringUtil::FromChars<int>("abc");
  ASSERT_FALSE(invalid.has_value());

  // Test with endptr
  std::string_view endptr;
  auto endptr_result = StringUtil::FromChars<int>("123abc", 10, &endptr);
  ASSERT_TRUE(endptr_result.has_value());
  ASSERT_EQ(*endptr_result, 123);
  ASSERT_EQ(endptr, "abc");
}

TEST(StringUtil, FromCharsWithOptionalBase)
{
  // Test hex prefix
  auto hex = StringUtil::FromCharsWithOptionalBase<int>("0xFF");
  ASSERT_TRUE(hex.has_value());
  ASSERT_EQ(*hex, 255);

  // Test binary prefix
  auto bin = StringUtil::FromCharsWithOptionalBase<int>("0b1010");
  ASSERT_TRUE(bin.has_value());
  ASSERT_EQ(*bin, 10);

  // Test octal prefix
  auto oct = StringUtil::FromCharsWithOptionalBase<int>("0123");
  ASSERT_TRUE(oct.has_value());
  ASSERT_EQ(*oct, 83); // 123 in octal = 83 in decimal

  // Test decimal (no prefix)
  auto dec = StringUtil::FromCharsWithOptionalBase<int>("123");
  ASSERT_TRUE(dec.has_value());
  ASSERT_EQ(*dec, 123);
}

TEST(StringUtil, FromCharsFloatingPoint)
{
  auto result = StringUtil::FromChars<float>("123.45");
  ASSERT_TRUE(result.has_value());
  ASSERT_FLOAT_EQ(*result, 123.45f);

  auto double_result = StringUtil::FromChars<double>("-456.789");
  ASSERT_TRUE(double_result.has_value());
  ASSERT_DOUBLE_EQ(*double_result, -456.789);

  // Test scientific notation
  auto sci = StringUtil::FromChars<double>("1.23e-4");
  ASSERT_TRUE(sci.has_value());
  ASSERT_DOUBLE_EQ(*sci, 0.000123);

  // Test invalid
  auto invalid = StringUtil::FromChars<float>("abc");
  ASSERT_FALSE(invalid.has_value());
}

TEST(StringUtil, FromCharsBool)
{
  // Test true values
  ASSERT_TRUE(StringUtil::FromChars<bool>("true", 10).value_or(false));
  ASSERT_TRUE(StringUtil::FromChars<bool>("TRUE", 10).value_or(false));
  ASSERT_TRUE(StringUtil::FromChars<bool>("yes", 10).value_or(false));
  ASSERT_TRUE(StringUtil::FromChars<bool>("YES", 10).value_or(false));
  ASSERT_TRUE(StringUtil::FromChars<bool>("on", 10).value_or(false));
  ASSERT_TRUE(StringUtil::FromChars<bool>("ON", 10).value_or(false));
  ASSERT_TRUE(StringUtil::FromChars<bool>("1", 10).value_or(false));
  ASSERT_TRUE(StringUtil::FromChars<bool>("enabled", 10).value_or(false));
  ASSERT_TRUE(StringUtil::FromChars<bool>("ENABLED", 10).value_or(false));

  // Test false values
  ASSERT_FALSE(StringUtil::FromChars<bool>("false", 10).value_or(true));
  ASSERT_FALSE(StringUtil::FromChars<bool>("FALSE", 10).value_or(true));
  ASSERT_FALSE(StringUtil::FromChars<bool>("no", 10).value_or(true));
  ASSERT_FALSE(StringUtil::FromChars<bool>("NO", 10).value_or(true));
  ASSERT_FALSE(StringUtil::FromChars<bool>("off", 10).value_or(true));
  ASSERT_FALSE(StringUtil::FromChars<bool>("OFF", 10).value_or(true));
  ASSERT_FALSE(StringUtil::FromChars<bool>("0", 10).value_or(true));
  ASSERT_FALSE(StringUtil::FromChars<bool>("disabled", 10).value_or(true));
  ASSERT_FALSE(StringUtil::FromChars<bool>("DISABLED", 10).value_or(true));

  // Test invalid
  ASSERT_FALSE(StringUtil::FromChars<bool>("maybe", 10).has_value());
  ASSERT_FALSE(StringUtil::FromChars<bool>("2", 10).has_value());
}

TEST(StringUtil, ToCharsIntegral)
{
  ASSERT_EQ(StringUtil::ToChars(123), "123");
  ASSERT_EQ(StringUtil::ToChars(-456), "-456");
  ASSERT_EQ(StringUtil::ToChars(255, 16), "ff");
  ASSERT_EQ(StringUtil::ToChars(15, 2), "1111");
}

TEST(StringUtil, ToCharsFloatingPoint)
{
  std::string result = StringUtil::ToChars(123.45f);
  ASSERT_FALSE(result.empty());
  // Just check it's a valid representation, exact format may vary
  ASSERT_NE(result.find("123"), std::string::npos);
}

TEST(StringUtil, ToCharsBool)
{
  ASSERT_EQ(StringUtil::ToChars(true), "true");
  ASSERT_EQ(StringUtil::ToChars(false), "false");
}

TEST(StringUtil, IsWhitespace)
{
  ASSERT_TRUE(StringUtil::IsWhitespace(' '));
  ASSERT_TRUE(StringUtil::IsWhitespace('\t'));
  ASSERT_TRUE(StringUtil::IsWhitespace('\n'));
  ASSERT_TRUE(StringUtil::IsWhitespace('\r'));
  ASSERT_TRUE(StringUtil::IsWhitespace('\f'));
  ASSERT_TRUE(StringUtil::IsWhitespace('\v'));

  ASSERT_FALSE(StringUtil::IsWhitespace('a'));
  ASSERT_FALSE(StringUtil::IsWhitespace('1'));
  ASSERT_FALSE(StringUtil::IsWhitespace('!'));
}

TEST(StringUtil, DecodeHexDigit)
{
  ASSERT_EQ(StringUtil::DecodeHexDigit('0'), 0);
  ASSERT_EQ(StringUtil::DecodeHexDigit('9'), 9);
  ASSERT_EQ(StringUtil::DecodeHexDigit('a'), 10);
  ASSERT_EQ(StringUtil::DecodeHexDigit('f'), 15);
  ASSERT_EQ(StringUtil::DecodeHexDigit('A'), 10);
  ASSERT_EQ(StringUtil::DecodeHexDigit('F'), 15);
  ASSERT_EQ(StringUtil::DecodeHexDigit('g'), 0); // Invalid should return 0
}

TEST(StringUtil, EncodeHex)
{
  std::vector<u8> data = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
  std::string hex = StringUtil::EncodeHex(data.data(), data.size());
  ASSERT_EQ(hex, "0123456789abcdef");

  // Test with span
  std::string hex_span = StringUtil::EncodeHex(std::span<const u8>(data));
  ASSERT_EQ(hex_span, "0123456789abcdef");

  // Test empty
  std::string empty_hex = StringUtil::EncodeHex(nullptr, 0);
  ASSERT_EQ(empty_hex, "");
}

TEST(StringUtil, DecodeHex)
{
  // Test buffer version
  std::vector<u8> buffer(8);
  size_t decoded = StringUtil::DecodeHex(std::span<u8>(buffer), "0123456789ABCDEF");
  ASSERT_EQ(decoded, 8u);
  ASSERT_EQ(buffer[0], 0x01u);
  ASSERT_EQ(buffer[1], 0x23u);
  ASSERT_EQ(buffer[7], 0xEFu);

  // Test vector version
  auto result = StringUtil::DecodeHex("0123456789ABCDEF");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), 8u);
  ASSERT_EQ((*result)[0], 0x01u);
  ASSERT_EQ((*result)[7], 0xEFu);

  // Test invalid hex
  auto invalid = StringUtil::DecodeHex("xyz");
  ASSERT_FALSE(invalid.has_value());
}

TEST(StringUtil, IsHexDigit)
{
  ASSERT_TRUE(StringUtil::IsHexDigit('0'));
  ASSERT_TRUE(StringUtil::IsHexDigit('9'));
  ASSERT_TRUE(StringUtil::IsHexDigit('a'));
  ASSERT_TRUE(StringUtil::IsHexDigit('f'));
  ASSERT_TRUE(StringUtil::IsHexDigit('A'));
  ASSERT_TRUE(StringUtil::IsHexDigit('F'));

  ASSERT_FALSE(StringUtil::IsHexDigit('g'));
  ASSERT_FALSE(StringUtil::IsHexDigit('G'));
  ASSERT_FALSE(StringUtil::IsHexDigit('!'));
  ASSERT_FALSE(StringUtil::IsHexDigit(' '));
}

TEST(StringUtil, ParseFixedHexString)
{
  constexpr auto result = StringUtil::ParseFixedHexString<4>("01234567");
  ASSERT_EQ(result[0], 0x01);
  ASSERT_EQ(result[1], 0x23);
  ASSERT_EQ(result[2], 0x45);
  ASSERT_EQ(result[3], 0x67);
}

TEST(StringUtil, Base64Lengths)
{
  ASSERT_EQ(StringUtil::DecodedBase64Length(""), 0u);
  ASSERT_EQ(StringUtil::DecodedBase64Length("SGVsbG8="), 5u);
  ASSERT_EQ(StringUtil::DecodedBase64Length("SGVsbG8h"), 6u);
  ASSERT_EQ(StringUtil::DecodedBase64Length("abc"), 0u); // Invalid length

  std::vector<u8> data = {1, 2, 3, 4, 5};
  ASSERT_EQ(StringUtil::EncodedBase64Length(std::span<const u8>(data)), 8u);
}

TEST(StringUtil, StartsWithNoCase)
{
  ASSERT_TRUE(StringUtil::StartsWithNoCase("Hello World", "hello"));
  ASSERT_TRUE(StringUtil::StartsWithNoCase("Hello World", "HELLO"));
  ASSERT_TRUE(StringUtil::StartsWithNoCase("test", "test"));
  ASSERT_TRUE(StringUtil::StartsWithNoCase("test", ""));
  ASSERT_FALSE(StringUtil::StartsWithNoCase("Hello", "world"));
  ASSERT_FALSE(StringUtil::StartsWithNoCase("Hi", "Hello"));
  ASSERT_FALSE(StringUtil::StartsWithNoCase("", "test"));
}

TEST(StringUtil, EndsWithNoCase)
{
  ASSERT_TRUE(StringUtil::EndsWithNoCase("Hello World", "world"));
  ASSERT_TRUE(StringUtil::EndsWithNoCase("Hello World", "WORLD"));
  ASSERT_TRUE(StringUtil::EndsWithNoCase("test", "test"));
  ASSERT_TRUE(StringUtil::EndsWithNoCase("test", ""));
  ASSERT_FALSE(StringUtil::EndsWithNoCase("Hello", "world"));
  ASSERT_FALSE(StringUtil::EndsWithNoCase("Hi", "Hello"));
  ASSERT_FALSE(StringUtil::EndsWithNoCase("", "test"));
}

TEST(StringUtil, StripWhitespace)
{
  // Test string_view version
  ASSERT_EQ(StringUtil::StripWhitespace("  hello  "), "hello");
  ASSERT_EQ(StringUtil::StripWhitespace("\t\n hello world \r\f"), "hello world");
  ASSERT_EQ(StringUtil::StripWhitespace("   "), "");
  ASSERT_EQ(StringUtil::StripWhitespace(""), "");
  ASSERT_EQ(StringUtil::StripWhitespace("hello"), "hello");
  ASSERT_EQ(StringUtil::StripWhitespace("  hello"), "hello");
  ASSERT_EQ(StringUtil::StripWhitespace("hello  "), "hello");

  // Test in-place version
  std::string s = "  hello world  ";
  StringUtil::StripWhitespace(&s);
  ASSERT_EQ(s, "hello world");

  s = "\t\n test \r\f";
  StringUtil::StripWhitespace(&s);
  ASSERT_EQ(s, "test");

  s = "   ";
  StringUtil::StripWhitespace(&s);
  ASSERT_EQ(s, "");
}

TEST(StringUtil, SplitString)
{
  auto result = StringUtil::SplitString("a,b,c", ',');
  ASSERT_EQ(result.size(), 3u);
  ASSERT_EQ(result[0], "a");
  ASSERT_EQ(result[1], "b");
  ASSERT_EQ(result[2], "c");

  // Test with empty parts
  result = StringUtil::SplitString("a,,c", ',', false);
  ASSERT_EQ(result.size(), 3u);
  ASSERT_EQ(result[1], "");

  // Test skip empty
  result = StringUtil::SplitString("a,,c", ',', true);
  ASSERT_EQ(result.size(), 2u);
  ASSERT_EQ(result[0], "a");
  ASSERT_EQ(result[1], "c");

  // Test empty string
  result = StringUtil::SplitString("", ',');
  ASSERT_EQ(result.size(), 0u);

  // Test no delimiter
  result = StringUtil::SplitString("hello", ',');
  ASSERT_EQ(result.size(), 1u);
  ASSERT_EQ(result[0], "hello");
}

TEST(StringUtil, SplitNewString)
{
  auto result = StringUtil::SplitNewString("a,b,c", ',');
  ASSERT_EQ(result.size(), 3u);
  ASSERT_EQ(result[0], "a");
  ASSERT_EQ(result[1], "b");
  ASSERT_EQ(result[2], "c");

  // Test empty string
  result = StringUtil::SplitNewString("", ',');
  ASSERT_EQ(result.size(), 0u);
}

TEST(StringUtil, IsInStringList)
{
  std::vector<std::string> list = {"apple", "banana", "cherry"};
  ASSERT_TRUE(StringUtil::IsInStringList(list, "apple"));
  ASSERT_TRUE(StringUtil::IsInStringList(list, "banana"));
  ASSERT_FALSE(StringUtil::IsInStringList(list, "grape"));
  ASSERT_FALSE(StringUtil::IsInStringList(list, ""));

  std::vector<std::string> empty_list;
  ASSERT_FALSE(StringUtil::IsInStringList(empty_list, "apple"));
}

TEST(StringUtil, AddToStringList)
{
  std::vector<std::string> list = {"apple", "banana"};

  // Add new item
  ASSERT_TRUE(StringUtil::AddToStringList(list, "cherry"));
  ASSERT_EQ(list.size(), 3u);
  ASSERT_EQ(list[2], "cherry");

  // Try to add existing item
  ASSERT_FALSE(StringUtil::AddToStringList(list, "apple"));
  ASSERT_EQ(list.size(), 3u);
}

TEST(StringUtil, RemoveFromStringList)
{
  std::vector<std::string> list = {"apple", "banana", "apple", "cherry"};

  // Remove existing item (should remove all occurrences)
  ASSERT_TRUE(StringUtil::RemoveFromStringList(list, "apple"));
  ASSERT_EQ(list.size(), 2u);
  ASSERT_EQ(list[0], "banana");
  ASSERT_EQ(list[1], "cherry");

  // Try to remove non-existing item
  ASSERT_FALSE(StringUtil::RemoveFromStringList(list, "grape"));
  ASSERT_EQ(list.size(), 2u);
}

TEST(StringUtil, JoinString)
{
  std::vector<std::string> list = {"apple", "banana", "cherry"};

  // Test with char delimiter
  ASSERT_EQ(StringUtil::JoinString(list, ','), "apple,banana,cherry");
  ASSERT_EQ(StringUtil::JoinString(list, ' '), "apple banana cherry");

  // Test with string delimiter
  ASSERT_EQ(StringUtil::JoinString(list, ", "), "apple, banana, cherry");
  ASSERT_EQ(StringUtil::JoinString(list, " and "), "apple and banana and cherry");

  // Test with iterator range
  ASSERT_EQ(StringUtil::JoinString(list.begin(), list.end(), ','), "apple,banana,cherry");

  // Test empty list
  std::vector<std::string> empty_list;
  ASSERT_EQ(StringUtil::JoinString(empty_list, ','), "");

  // Test single item
  std::vector<std::string> single = {"apple"};
  ASSERT_EQ(StringUtil::JoinString(single, ','), "apple");
}

TEST(StringUtil, ReplaceAll)
{
  // Test string return version
  ASSERT_EQ(StringUtil::ReplaceAll("hello world", "world", "universe"), "hello universe");
  ASSERT_EQ(StringUtil::ReplaceAll("test test test", "test", "exam"), "exam exam exam");
  ASSERT_EQ(StringUtil::ReplaceAll("abcdef", "xyz", "123"), "abcdef"); // No match
  ASSERT_EQ(StringUtil::ReplaceAll("", "test", "exam"), "");
  ASSERT_EQ(StringUtil::ReplaceAll("test", "", "exam"), "test"); // Empty search

  // Test in-place version
  std::string s = "hello world";
  StringUtil::ReplaceAll(&s, "world", "universe");
  ASSERT_EQ(s, "hello universe");

  // Test char versions
  ASSERT_EQ(StringUtil::ReplaceAll("a,b,c", ',', ';'), "a;b;c");

  s = "a,b,c";
  StringUtil::ReplaceAll(&s, ',', ';');
  ASSERT_EQ(s, "a;b;c");
}

TEST(StringUtil, ParseAssignmentString)
{
  std::string_view key, value;

  // Test normal assignment
  ASSERT_TRUE(StringUtil::ParseAssignmentString("key=value", &key, &value));
  ASSERT_EQ(key, "key");
  ASSERT_EQ(value, "value");

  // Test with spaces
  ASSERT_TRUE(StringUtil::ParseAssignmentString("  key  =  value  ", &key, &value));
  ASSERT_EQ(key, "key");
  ASSERT_EQ(value, "value");

  // Test empty value
  ASSERT_TRUE(StringUtil::ParseAssignmentString("key=", &key, &value));
  ASSERT_EQ(key, "key");
  ASSERT_EQ(value, "");

  // Test no equals sign
  ASSERT_FALSE(StringUtil::ParseAssignmentString("keyvalue", &key, &value));

  // Test empty string
  ASSERT_FALSE(StringUtil::ParseAssignmentString("", &key, &value));

  // Test only equals
  ASSERT_TRUE(StringUtil::ParseAssignmentString("=", &key, &value));
  ASSERT_EQ(key, "");
  ASSERT_EQ(value, "");
}

TEST(StringUtil, GetNextToken)
{
  std::string_view caret = "a,b,c,d";

  auto token = StringUtil::GetNextToken(caret, ',');
  ASSERT_TRUE(token.has_value());
  ASSERT_EQ(*token, "a");
  ASSERT_EQ(caret, "b,c,d");

  token = StringUtil::GetNextToken(caret, ',');
  ASSERT_TRUE(token.has_value());
  ASSERT_EQ(*token, "b");
  ASSERT_EQ(caret, "c,d");

  token = StringUtil::GetNextToken(caret, ',');
  ASSERT_TRUE(token.has_value());
  ASSERT_EQ(*token, "c");
  ASSERT_EQ(caret, "d");

  token = StringUtil::GetNextToken(caret, ',');
  ASSERT_FALSE(token.has_value());
  ASSERT_EQ(caret, "d");
}

TEST(StringUtil, GetUTF8CharacterCount)
{
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount(""sv), 0u);
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("Hello, world!"sv), 13u);

  // COPYRIGHT SIGN U+00A9 -> 0xC2 0xA9
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("\xC2\xA9"sv), 1u);

  // Truncated 2-byte sequence (only leading byte present)
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("\xC2"sv), 1u);

  // EURO SIGN U+20AC -> 0xE2 0x82 0xAC
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("\xE2\x82\xAC"sv), 1u);

  // Truncated 3-byte sequence
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("\xE2\x82"sv), 1u);

  // GRINNING FACE U+1F600 -> 0xF0 0x9F 0x98 0x80
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("\xF0\x9F\x98\x80"sv), 1u);

  // Truncated 4-byte sequence
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("\xF0\x9F\x98"sv), 1u);

  // "A" + EURO + GRINNING + "B"
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("A"
                                              "\xE2\x82\xAC"
                                              "\xF0\x9F\x98\x80"
                                              "B"sv),
            4u);

  // Three grinning faces in a row (3 * 4 bytes)
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("\xF0\x9F\x98\x80"
                                              "\xF0\x9F\x98\x80"
                                              "\xF0\x9F\x98\x80"sv),
            3u);

  // Continuation bytes (0x80 - 0xBF) appearing alone are invalid and should each count as one.
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("\x80\x81\x82"sv), 3u);

  // Leading bytes that are outside allowed ranges (e.g., 0xF5..0xFF)
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("\xF5\xF6\xFF"sv), 3u);

  // 0xF4 allowed as 4-byte lead (e.g., U+10FFFF -> F4 8F BF BF)
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("\xF4\x8F\xBF\xBF"sv), 1u);

  // Mix: ASCII, valid 2-byte, invalid continuation, truncated 3-byte, valid 3-byte, valid 4-byte
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("X"
                                              "\xC3\xA9"
                                              "\x80"
                                              "\xE2"
                                              "\xE2\x82\xAC"
                                              "\xF0\x9F\x8D\x95"sv),
            6u);

  // Inline characters (not hex escapes): 'a' (ASCII), 'é' (U+00E9), '€' (U+20AC), '😀' (U+1F600), 'z'
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("aé€😀z"sv), 5u);

  // Emoji-only example (two emoji characters inline)
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("😀😀"sv), 2u);

  // "Hello ⣿ World 😀" but using standard euro sign U+20AC
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("Hello € World 😀"sv), 15u);

  // 'A' 'é' 'B' '€' '😀' 'C' -> total 6 codepoints
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("AéB€😀C"sv), 6u);

  // Inline 'é' then hex euro then inline emoji
  EXPECT_EQ(StringUtil::GetUTF8CharacterCount("é"
                                              "\xE2\x82\xAC"
                                              "😀"sv),
            3u);
}

TEST(StringUtil, EncodeAndAppendUTF8)
{
  std::string s;

  // Test ASCII character
  StringUtil::EncodeAndAppendUTF8(s, U'A');
  ASSERT_EQ(s, "A");

  // Test 2-byte UTF-8
  s.clear();
  StringUtil::EncodeAndAppendUTF8(s, U'ñ'); // U+00F1
  ASSERT_EQ(s.size(), 2u);

  // Test 3-byte UTF-8
  s.clear();
  StringUtil::EncodeAndAppendUTF8(s, U'€'); // U+20AC
  ASSERT_EQ(s.size(), 3u);

  // Test 4-byte UTF-8
  s.clear();
  StringUtil::EncodeAndAppendUTF8(s, U'💖'); // U+1F496
  ASSERT_EQ(s.size(), 4u);

  // Test invalid character (should encode replacement character)
  s.clear();
  StringUtil::EncodeAndAppendUTF8(s, 0x110000); // Invalid
  ASSERT_EQ(s.size(), 3u);                      // Replacement character is 3 bytes

  // Test buffer version
  u8 buffer[10] = {0};
  size_t written = StringUtil::EncodeAndAppendUTF8(buffer, 0, sizeof(buffer), U'A');
  ASSERT_EQ(written, 1u);
  ASSERT_EQ(buffer[0], 'A');

  written = StringUtil::EncodeAndAppendUTF8(buffer, 1, sizeof(buffer), U'€');
  ASSERT_EQ(written, 3u);

  // Test buffer overflow
  written = StringUtil::EncodeAndAppendUTF8(buffer, 9, sizeof(buffer), U'💖');
  ASSERT_EQ(written, 0u); // Should fail due to insufficient space
}

TEST(StringUtil, GetEncodedUTF8Length)
{
  ASSERT_EQ(StringUtil::GetEncodedUTF8Length(U'A'), 1u);     // ASCII
  ASSERT_EQ(StringUtil::GetEncodedUTF8Length(U'ñ'), 2u);     // 2-byte
  ASSERT_EQ(StringUtil::GetEncodedUTF8Length(U'€'), 3u);     // 3-byte
  ASSERT_EQ(StringUtil::GetEncodedUTF8Length(U'💖'), 4u);    // 4-byte
  ASSERT_EQ(StringUtil::GetEncodedUTF8Length(0x110000), 3u); // Invalid -> replacement
}

TEST(StringUtil, DecodeUTF8)
{
  // Test ASCII
  char32_t ch;
  size_t len = StringUtil::DecodeUTF8("A", 0, &ch);
  ASSERT_EQ(len, 1u);
  ASSERT_EQ(ch, U'A');

  // Test 2-byte UTF-8 (ñ = C3 B1)
  std::string utf8_2byte = "\xC3\xB1";
  len = StringUtil::DecodeUTF8(utf8_2byte, 0, &ch);
  ASSERT_EQ(len, 2u);
  ASSERT_EQ(ch, U'ñ');

  // Test 3-byte UTF-8 (€ = E2 82 AC)
  std::string utf8_3byte = "\xE2\x82\xAC";
  len = StringUtil::DecodeUTF8(utf8_3byte, 0, &ch);
  ASSERT_EQ(len, 3u);
  ASSERT_EQ(ch, U'€');

  // Test void* version
  len = StringUtil::DecodeUTF8(utf8_3byte.data(), utf8_3byte.size(), &ch);
  ASSERT_EQ(len, 3u);
  ASSERT_EQ(ch, U'€');

  // Test invalid UTF-8 sequence
  std::string invalid_utf8 = "\xFF\xFE";
  len = StringUtil::DecodeUTF8(invalid_utf8.data(), invalid_utf8.size(), &ch);
  ASSERT_EQ(len, 1u);
  ASSERT_EQ(ch, StringUtil::UNICODE_REPLACEMENT_CHARACTER);
}

TEST(StringUtil, EncodeAndAppendUTF16)
{
  // Test ASCII character
  u16 buffer[10] = {0};
  size_t written = StringUtil::EncodeAndAppendUTF16(buffer, 0, 10, U'A');
  ASSERT_EQ(written, 1u);
  ASSERT_EQ(buffer[0], u16('A'));

  // Test basic multi-byte character
  written = StringUtil::EncodeAndAppendUTF16(buffer, 1, 10, U'€'); // U+20AC
  ASSERT_EQ(written, 1u);
  ASSERT_EQ(buffer[1], u16(0x20AC));

  // Test surrogate pair (4-byte UTF-8 character)
  written = StringUtil::EncodeAndAppendUTF16(buffer, 2, 10, U'💖'); // U+1F496
  ASSERT_EQ(written, 2u);
  // Should encode as surrogate pair: High surrogate D83D, Low surrogate DC96
  ASSERT_EQ(buffer[2], u16(0xD83D));
  ASSERT_EQ(buffer[3], u16(0xDC96));

  // Test invalid surrogate range (should become replacement character)
  written = StringUtil::EncodeAndAppendUTF16(buffer, 4, 10, 0xD800); // In surrogate range
  ASSERT_EQ(written, 1u);
  ASSERT_EQ(buffer[4], u16(StringUtil::UNICODE_REPLACEMENT_CHARACTER));

  // Test invalid codepoint (should become replacement character)
  written = StringUtil::EncodeAndAppendUTF16(buffer, 5, 10, 0x110000); // Invalid codepoint
  ASSERT_EQ(written, 1u);
  ASSERT_EQ(buffer[5], u16(StringUtil::UNICODE_REPLACEMENT_CHARACTER));

  // Test buffer overflow
  written = StringUtil::EncodeAndAppendUTF16(buffer, 9, 10, U'💖'); // Needs 2 units but only 1 available
  ASSERT_EQ(written, 0u);
}

TEST(StringUtil, DecodeUTF16)
{
  // Test ASCII character
  u16 ascii_data[] = {u16('A')};
  char32_t ch;
  size_t len = StringUtil::DecodeUTF16(ascii_data, 0, 1, &ch);
  ASSERT_EQ(len, 1u);
  ASSERT_EQ(ch, U'A');

  // Test basic multi-byte character
  u16 euro_data[] = {u16(0x20AC)}; // €
  len = StringUtil::DecodeUTF16(euro_data, 0, 1, &ch);
  ASSERT_EQ(len, 1u);
  ASSERT_EQ(ch, U'€');

  // Test surrogate pair
  u16 emoji_data[] = {u16(0xD83D), u16(0xDC96)}; // 💖
  len = StringUtil::DecodeUTF16(emoji_data, 0, 2, &ch);
  ASSERT_EQ(len, 2u);
  ASSERT_EQ(ch, U'💖');

  // Test invalid high surrogate (missing low surrogate)
  u16 invalid_high[] = {u16(0xD83D)};
  len = StringUtil::DecodeUTF16(invalid_high, 0, 1, &ch);
  ASSERT_EQ(len, 1u);
  ASSERT_EQ(ch, StringUtil::UNICODE_REPLACEMENT_CHARACTER);

  // Test invalid high surrogate followed by invalid low surrogate
  u16 invalid_surrogates[] = {u16(0xD83D), u16(0x0041)}; // High surrogate followed by 'A'
  len = StringUtil::DecodeUTF16(invalid_surrogates, 0, 2, &ch);
  ASSERT_EQ(len, 2u);
  ASSERT_EQ(ch, StringUtil::UNICODE_REPLACEMENT_CHARACTER);
}

TEST(StringUtil, DecodeUTF16BE)
{
  // Test with byte-swapped data (big-endian)
  alignas(alignof(u16)) static constexpr const u8 be_data[] = {0x20, 0xAC}; // 0x20AC (€) byte-swapped
  char32_t ch;
  size_t len = StringUtil::DecodeUTF16BE(be_data, 0, sizeof(be_data), &ch);
  ASSERT_EQ(len, 1u);
  ASSERT_EQ(ch, U'€');

  // Test surrogate pair with byte swapping
  alignas(alignof(u16)) static constexpr const u8 be_emoji_data[] = {0xD8, 0x3D, 0xDC, 0x96}; // D83D DC96 byte-swapped
  len = StringUtil::DecodeUTF16BE(be_emoji_data, 0, 2, &ch);
  ASSERT_EQ(len, 2u);
  ASSERT_EQ(ch, U'💖');
}

TEST(StringUtil, DecodeUTF16String)
{
  // Test simple ASCII string
  u16 ascii_utf16[] = {u16('H'), u16('e'), u16('l'), u16('l'), u16('o')};
  std::string result = StringUtil::DecodeUTF16String(ascii_utf16, sizeof(ascii_utf16));
  ASSERT_EQ(result, "Hello");

  // Test string with multi-byte characters
  u16 mixed_utf16[] = {u16('H'), u16('e'), u16('l'), u16('l'), u16('o'), u16(0x20AC)}; // Hello€
  result = StringUtil::DecodeUTF16String(mixed_utf16, sizeof(mixed_utf16));
  ASSERT_EQ(result.size(), 8u); // 5 ASCII + 3 bytes for €
  ASSERT_TRUE(result.starts_with("Hello"));

  // Test with surrogate pairs
  u16 emoji_utf16[] = {u16('H'), u16('i'), u16(0xD83D), u16(0xDC96)}; // Hi💖
  result = StringUtil::DecodeUTF16String(emoji_utf16, sizeof(emoji_utf16));
  ASSERT_EQ(result.size(), 6u); // 2 ASCII + 4 bytes for 💖
  ASSERT_TRUE(result.starts_with("Hi"));
}

TEST(StringUtil, DecodeUTF16BEString)
{
  // Test with byte-swapped data
  u16 be_utf16[] = {0x4800, 0x6500}; // "He" in big-endian
  std::string result = StringUtil::DecodeUTF16BEString(be_utf16, sizeof(be_utf16));
  ASSERT_EQ(result, "He");

  // Test with multi-byte character
  u16 be_euro[] = {0x3D20}; // € in big-endian
  result = StringUtil::DecodeUTF16BEString(be_euro, sizeof(be_euro));
  ASSERT_EQ(result.size(), 3u); // € is 3 bytes in UTF-8
}

TEST(StringUtil, BytePatternSearch)
{
  std::vector<u8> data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

  // Test exact match
  auto result = StringUtil::BytePatternSearch(std::span<const u8>(data), "01 02 03");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value(), 0u);

  // Test match in middle
  result = StringUtil::BytePatternSearch(std::span<const u8>(data), "03 04 05");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value(), 2u);

  // Test with wildcards
  result = StringUtil::BytePatternSearch(std::span<const u8>(data), "01 ?? 03");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value(), 0u);

  // Test no match
  result = StringUtil::BytePatternSearch(std::span<const u8>(data), "FF FF FF");
  ASSERT_FALSE(result.has_value());

  // Test empty pattern
  result = StringUtil::BytePatternSearch(std::span<const u8>(data), "");
  ASSERT_FALSE(result.has_value());

  // Test lowercase hex
  result = StringUtil::BytePatternSearch(std::span<const u8>(data), "01 02 03");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value(), 0u);

  // Test mixed case
  result = StringUtil::BytePatternSearch(std::span<const u8>(data), "01 ?? 03");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value(), 0u);
}

TEST(StringUtil, StrideMemCpy)
{
  static constexpr const u8 src[] = {1, 2, 3, 4, 5, 6, 7, 8};
  u8 dst[8] = {0};

  // Test normal memcpy (same stride and copy size)
  StringUtil::StrideMemCpy(dst, 2, src, 2, 2, 4);
  ASSERT_EQ(dst[0], 1);
  ASSERT_EQ(dst[1], 2);
  ASSERT_EQ(dst[2], 3);
  ASSERT_EQ(dst[3], 4);

  // Reset and test different strides
  memset(dst, 0, sizeof(dst));
  StringUtil::StrideMemCpy(dst, 3, src, 2, 1, 3);
  ASSERT_EQ(dst[0], 1);
  ASSERT_EQ(dst[3], 3);
  ASSERT_EQ(dst[6], 5);
}

TEST(StringUtil, StrideMemCmp)
{
  static constexpr const u8 data1[] = {1, 0, 2, 0, 3, 0};
  u8 data2[] = {1, 2, 3};

  // Test equal comparison with different strides
  int result = StringUtil::StrideMemCmp(data1, 2, data2, 1, 1, 3);
  ASSERT_EQ(result, 0);

  // Test unequal comparison
  data2[1] = 4;
  result = StringUtil::StrideMemCmp(data1, 2, data2, 1, 1, 3);
  ASSERT_NE(result, 0);
}

#ifdef _WIN32
TEST(StringUtil, UTF8StringToWideString)
{
  std::wstring result = StringUtil::UTF8StringToWideString("Hello");
  ASSERT_EQ(result, L"Hello");

  // Test with UTF-8 characters
  std::wstring utf8_result = StringUtil::UTF8StringToWideString("Héllo");
  ASSERT_FALSE(utf8_result.empty());

  // Test bool version
  std::wstring dest;
  bool success = StringUtil::UTF8StringToWideString(dest, "Hello");
  ASSERT_TRUE(success);
  ASSERT_EQ(dest, L"Hello");
}

TEST(StringUtil, WideStringToUTF8String)
{
  std::string result = StringUtil::WideStringToUTF8String(L"Hello");
  ASSERT_EQ(result, "Hello");

  // Test bool version
  std::string dest;
  bool success = StringUtil::WideStringToUTF8String(dest, L"Hello");
  ASSERT_TRUE(success);
  ASSERT_EQ(dest, "Hello");
}
#endif

// ============================================================================
// BumpStringPool Tests
// ============================================================================

class BumpStringPoolTest : public ::testing::Test
{
protected:
  BumpStringPool pool;
};

TEST_F(BumpStringPoolTest, InitialState)
{
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
}

TEST_F(BumpStringPoolTest, AddString_ValidString)
{
  const std::string_view test_str = "test";
  const auto offset = pool.AddString(test_str);

  EXPECT_NE(offset, BumpStringPool::InvalidOffset);
  EXPECT_FALSE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), test_str.size() + 1); // +1 for null terminator
}

TEST_F(BumpStringPoolTest, AddString_EmptyString)
{
  const auto offset = pool.AddString("");

  EXPECT_EQ(offset, BumpStringPool::InvalidOffset);
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
}

TEST_F(BumpStringPoolTest, AddString_MultipleStrings)
{
  const std::string_view str1 = "first";
  const std::string_view str2 = "second";
  const std::string_view str3 = "third";

  const auto offset1 = pool.AddString(str1);
  const auto offset2 = pool.AddString(str2);
  const auto offset3 = pool.AddString(str3);

  EXPECT_NE(offset1, BumpStringPool::InvalidOffset);
  EXPECT_NE(offset2, BumpStringPool::InvalidOffset);
  EXPECT_NE(offset3, BumpStringPool::InvalidOffset);

  EXPECT_EQ(offset1, 0u);
  EXPECT_EQ(offset2, str1.size() + 1);
  EXPECT_EQ(offset3, str1.size() + 1 + str2.size() + 1);

  const size_t expected_size = str1.size() + str2.size() + str3.size() + 3; // +3 for null terminators
  EXPECT_EQ(pool.GetSize(), expected_size);
}

TEST_F(BumpStringPoolTest, AddString_DuplicateStrings)
{
  const std::string_view test_str = "duplicate";

  const auto offset1 = pool.AddString(test_str);
  const auto offset2 = pool.AddString(test_str);

  // BumpStringPool does NOT deduplicate
  EXPECT_NE(offset1, offset2);
  EXPECT_EQ(pool.GetSize(), (test_str.size() + 1) * 2);
}

TEST_F(BumpStringPoolTest, GetString_ValidOffset)
{
  const std::string_view test_str = "hello world";
  const auto offset = pool.AddString(test_str);

  const auto retrieved = pool.GetString(offset);

  EXPECT_EQ(retrieved, test_str);
}

TEST_F(BumpStringPoolTest, GetString_InvalidOffset)
{
  const auto retrieved = pool.GetString(BumpStringPool::InvalidOffset);

  EXPECT_TRUE(retrieved.empty());
}

TEST_F(BumpStringPoolTest, GetString_OutOfBoundsOffset)
{
  std::ignore = pool.AddString("test");
  const auto retrieved = pool.GetString(9999);

  EXPECT_TRUE(retrieved.empty());
}

TEST_F(BumpStringPoolTest, GetString_MultipleStrings)
{
  const std::string_view str1 = "alpha";
  const std::string_view str2 = "beta";
  const std::string_view str3 = "gamma";

  const auto offset1 = pool.AddString(str1);
  const auto offset2 = pool.AddString(str2);
  const auto offset3 = pool.AddString(str3);

  EXPECT_EQ(pool.GetString(offset1), str1);
  EXPECT_EQ(pool.GetString(offset2), str2);
  EXPECT_EQ(pool.GetString(offset3), str3);
}

TEST_F(BumpStringPoolTest, Clear)
{
  std::ignore = pool.AddString("test1");
  std::ignore = pool.AddString("test2");
  std::ignore = pool.AddString("test3");

  EXPECT_FALSE(pool.IsEmpty());
  EXPECT_GT(pool.GetSize(), 0u);

  pool.Clear();

  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
}

TEST_F(BumpStringPoolTest, Reserve)
{
  pool.Reserve(1024);

  // Reserve doesn't change the logical size or empty state
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);

  // After reservation, adding strings should still work
  const auto offset = pool.AddString("test");
  EXPECT_NE(offset, BumpStringPool::InvalidOffset);
}

TEST_F(BumpStringPoolTest, AddString_SpecialCharacters)
{
  const std::string_view special_str = "Hello\nWorld\t!@#$%^&*()";
  const auto offset = pool.AddString(special_str);

  EXPECT_NE(offset, BumpStringPool::InvalidOffset);
  EXPECT_EQ(pool.GetString(offset), special_str);
}

TEST_F(BumpStringPoolTest, AddString_UnicodeCharacters)
{
  const std::string_view unicode_str = "Hello 世界 🌍";
  const auto offset = pool.AddString(unicode_str);

  EXPECT_NE(offset, BumpStringPool::InvalidOffset);
  EXPECT_EQ(pool.GetString(offset), unicode_str);
}

TEST_F(BumpStringPoolTest, AddString_LongString)
{
  std::string long_str(10000, 'x');
  const auto offset = pool.AddString(long_str);

  EXPECT_NE(offset, BumpStringPool::InvalidOffset);
  EXPECT_EQ(pool.GetString(offset), long_str);
  EXPECT_EQ(pool.GetSize(), long_str.size() + 1);
}

// ============================================================================
// StringPool Tests
// ============================================================================

class StringPoolTest : public ::testing::Test
{
protected:
  StringPool pool;
};

TEST_F(StringPoolTest, InitialState)
{
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
  EXPECT_EQ(pool.GetCount(), 0u);
}

TEST_F(StringPoolTest, AddString_ValidString)
{
  const std::string_view test_str = "test";
  const auto offset = pool.AddString(test_str);

  EXPECT_NE(offset, StringPool::InvalidOffset);
  EXPECT_FALSE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), test_str.size() + 1);
  EXPECT_EQ(pool.GetCount(), 1u);
}

TEST_F(StringPoolTest, AddString_EmptyString)
{
  const auto offset = pool.AddString("");

  EXPECT_EQ(offset, StringPool::InvalidOffset);
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
  EXPECT_EQ(pool.GetCount(), 0u);
}

TEST_F(StringPoolTest, AddString_MultipleStrings)
{
  const std::string_view str1 = "first";
  const std::string_view str2 = "second";
  const std::string_view str3 = "third";

  const auto offset1 = pool.AddString(str1);
  const auto offset2 = pool.AddString(str2);
  const auto offset3 = pool.AddString(str3);

  EXPECT_NE(offset1, StringPool::InvalidOffset);
  EXPECT_NE(offset2, StringPool::InvalidOffset);
  EXPECT_NE(offset3, StringPool::InvalidOffset);

  EXPECT_EQ(pool.GetCount(), 3u);

  const size_t expected_size = str1.size() + str2.size() + str3.size() + 3;
  EXPECT_EQ(pool.GetSize(), expected_size);
}

TEST_F(StringPoolTest, AddString_DuplicateStrings)
{
  const std::string_view test_str = "duplicate";

  const auto offset1 = pool.AddString(test_str);
  const auto offset2 = pool.AddString(test_str);

  // StringPool DOES deduplicate
  EXPECT_EQ(offset1, offset2);
  EXPECT_EQ(pool.GetSize(), test_str.size() + 1);
  EXPECT_EQ(pool.GetCount(), 1u);
}

TEST_F(StringPoolTest, AddString_MultipleDuplicates)
{
  const std::string_view str1 = "test";
  const std::string_view str2 = "hello";

  const auto offset1_1 = pool.AddString(str1);
  const auto offset2_1 = pool.AddString(str2);
  const auto offset1_2 = pool.AddString(str1);
  const auto offset2_2 = pool.AddString(str2);
  const auto offset1_3 = pool.AddString(str1);

  EXPECT_EQ(offset1_1, offset1_2);
  EXPECT_EQ(offset1_1, offset1_3);
  EXPECT_EQ(offset2_1, offset2_2);
  EXPECT_NE(offset1_1, offset2_1);

  EXPECT_EQ(pool.GetCount(), 2u);
  EXPECT_EQ(pool.GetSize(), str1.size() + str2.size() + 2);
}

TEST_F(StringPoolTest, GetString_ValidOffset)
{
  const std::string_view test_str = "hello world";
  const auto offset = pool.AddString(test_str);

  const auto retrieved = pool.GetString(offset);

  EXPECT_EQ(retrieved, test_str);
}

TEST_F(StringPoolTest, GetString_InvalidOffset)
{
  const auto retrieved = pool.GetString(StringPool::InvalidOffset);

  EXPECT_TRUE(retrieved.empty());
}

TEST_F(StringPoolTest, GetString_OutOfBoundsOffset)
{
  std::ignore = pool.AddString("test");
  const auto retrieved = pool.GetString(9999);

  EXPECT_TRUE(retrieved.empty());
}

TEST_F(StringPoolTest, GetString_MultipleStrings)
{
  const std::string_view str1 = "alpha";
  const std::string_view str2 = "beta";
  const std::string_view str3 = "gamma";

  const auto offset1 = pool.AddString(str1);
  const auto offset2 = pool.AddString(str2);
  const auto offset3 = pool.AddString(str3);

  EXPECT_EQ(pool.GetString(offset1), str1);
  EXPECT_EQ(pool.GetString(offset2), str2);
  EXPECT_EQ(pool.GetString(offset3), str3);
}

TEST_F(StringPoolTest, Clear)
{
  std::ignore = pool.AddString("test1");
  std::ignore = pool.AddString("test2");
  std::ignore = pool.AddString("test3");

  EXPECT_FALSE(pool.IsEmpty());
  EXPECT_GT(pool.GetSize(), 0u);
  EXPECT_EQ(pool.GetCount(), 3u);

  pool.Clear();

  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
  EXPECT_EQ(pool.GetCount(), 0u);
}

TEST_F(StringPoolTest, Clear_WithDuplicates)
{
  std::ignore = pool.AddString("test");
  std::ignore = pool.AddString("test");
  std::ignore = pool.AddString("hello");

  EXPECT_EQ(pool.GetCount(), 2u);

  pool.Clear();

  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetCount(), 0u);
}

TEST_F(StringPoolTest, Reserve)
{
  pool.Reserve(1024);

  // Reserve doesn't change the logical state
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
  EXPECT_EQ(pool.GetCount(), 0u);

  // After reservation, adding strings should still work
  const auto offset = pool.AddString("test");
  EXPECT_NE(offset, StringPool::InvalidOffset);
}

TEST_F(StringPoolTest, AddString_SpecialCharacters)
{
  const std::string_view special_str = "Hello\nWorld\t!@#$%^&*()";
  const auto offset = pool.AddString(special_str);

  EXPECT_NE(offset, StringPool::InvalidOffset);
  EXPECT_EQ(pool.GetString(offset), special_str);
}

TEST_F(StringPoolTest, AddString_UnicodeCharacters)
{
  const std::string_view unicode_str = "Hello 世界 🌍";
  const auto offset = pool.AddString(unicode_str);

  EXPECT_NE(offset, StringPool::InvalidOffset);
  EXPECT_EQ(pool.GetString(offset), unicode_str);
}

TEST_F(StringPoolTest, AddString_LongString)
{
  std::string long_str(10000, 'x');
  const auto offset = pool.AddString(long_str);

  EXPECT_NE(offset, StringPool::InvalidOffset);
  EXPECT_EQ(pool.GetString(offset), long_str);
  EXPECT_EQ(pool.GetSize(), long_str.size() + 1);
  EXPECT_EQ(pool.GetCount(), 1u);
}

TEST_F(StringPoolTest, AddString_SimilarStrings)
{
  const std::string_view str1 = "test";
  const std::string_view str2 = "test1";
  const std::string_view str3 = "testing";

  const auto offset1 = pool.AddString(str1);
  const auto offset2 = pool.AddString(str2);
  const auto offset3 = pool.AddString(str3);

  EXPECT_NE(offset1, offset2);
  EXPECT_NE(offset1, offset3);
  EXPECT_NE(offset2, offset3);

  EXPECT_EQ(pool.GetCount(), 3u);

  EXPECT_EQ(pool.GetString(offset1), str1);
  EXPECT_EQ(pool.GetString(offset2), str2);
  EXPECT_EQ(pool.GetString(offset3), str3);
}

TEST_F(StringPoolTest, GetCount_TracksUniqueStrings)
{
  EXPECT_EQ(pool.GetCount(), 0u);

  std::ignore = pool.AddString("unique1");
  EXPECT_EQ(pool.GetCount(), 1u);

  std::ignore = pool.AddString("unique2");
  EXPECT_EQ(pool.GetCount(), 2u);

  std::ignore = pool.AddString("unique1"); // Duplicate
  EXPECT_EQ(pool.GetCount(), 2u);

  std::ignore = pool.AddString("unique3");
  EXPECT_EQ(pool.GetCount(), 3u);
}

TEST_F(StringPoolTest, ReuseAfterClear)
{
  const std::string_view test_str = "reuse";

  const auto offset1 = pool.AddString(test_str);
  EXPECT_EQ(offset1, 0u);
  EXPECT_EQ(pool.GetCount(), 1u);

  pool.Clear();

  const auto offset2 = pool.AddString(test_str);
  EXPECT_EQ(pool.GetCount(), 1u);

  // After clear, new strings start at offset 0 again
  EXPECT_EQ(offset2, 0u);
  EXPECT_EQ(pool.GetString(offset2), test_str);
}

// ============================================================================
// Comparison Tests: BumpStringPool vs StringPool
// ============================================================================

TEST(StringPoolComparison, DuplicationBehavior)
{
  BumpStringPool bump_pool;
  StringPool string_pool;

  const std::string_view test_str = "duplicate";

  const auto bump_offset1 = bump_pool.AddString(test_str);
  const auto bump_offset2 = bump_pool.AddString(test_str);

  const auto string_offset1 = string_pool.AddString(test_str);
  const auto string_offset2 = string_pool.AddString(test_str);

  // BumpStringPool creates duplicates
  EXPECT_NE(bump_offset1, bump_offset2);
  EXPECT_EQ(bump_pool.GetSize(), (test_str.size() + 1) * 2);

  // StringPool deduplicates
  EXPECT_EQ(string_offset1, string_offset2);
  EXPECT_EQ(string_pool.GetSize(), test_str.size() + 1);
}

TEST(StringPoolComparison, MemoryEfficiency)
{
  BumpStringPool bump_pool;
  StringPool string_pool;

  const std::string_view str = "test";

  // Add same string 100 times
  for (int i = 0; i < 100; ++i)
  {
    std::ignore = bump_pool.AddString(str);
    std::ignore = string_pool.AddString(str);
  }

  // BumpStringPool stores 100 copies
  EXPECT_EQ(bump_pool.GetSize(), (str.size() + 1) * 100);

  // StringPool stores only 1 copy
  EXPECT_EQ(string_pool.GetSize(), str.size() + 1);
  EXPECT_EQ(string_pool.GetCount(), 1u);
}