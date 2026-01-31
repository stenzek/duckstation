// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/small_string.h"

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

using namespace std::string_view_literals;

//////////////////////////////////////////////////////////////////////////
// SmallStringBase Test Suite
//////////////////////////////////////////////////////////////////////////

// Constructor Tests
TEST(SmallStringBase, DefaultConstructor)
{
  SmallString s;
  EXPECT_TRUE(s.empty());
  EXPECT_EQ(s.length(), 0u);
  EXPECT_STREQ(s.c_str(), "");
}

TEST(SmallStringBase, ConstCharConstructor)
{
  SmallString s("Hello");
  EXPECT_FALSE(s.empty());
  EXPECT_EQ(s.length(), 5u);
  EXPECT_STREQ(s.c_str(), "Hello");
}

TEST(SmallStringBase, ConstCharWithLengthConstructor)
{
  SmallString s("Hello World", 5);
  EXPECT_EQ(s.length(), 5u);
  EXPECT_STREQ(s.c_str(), "Hello");
}

TEST(SmallStringBase, CopyConstructor)
{
  SmallString original("Test String");
  SmallString copy(original);
  EXPECT_EQ(copy.length(), original.length());
  EXPECT_STREQ(copy.c_str(), original.c_str());
}

TEST(SmallStringBase, MoveConstructor)
{
  SmallString original("Test String");
  SmallString moved(std::move(original));
  EXPECT_EQ(moved.length(), 11u);
  EXPECT_STREQ(moved.c_str(), "Test String");
}

TEST(SmallStringBase, StdStringConstructor)
{
  std::string stdStr = "Hello from std::string";
  SmallString s(stdStr);
  EXPECT_EQ(s.length(), stdStr.length());
  EXPECT_STREQ(s.c_str(), stdStr.c_str());
}

TEST(SmallStringBase, StringViewConstructor)
{
  std::string_view sv = "Hello from string_view";
  SmallString s(sv);
  EXPECT_EQ(s.length(), sv.length());
  EXPECT_EQ(s.view(), sv);
}

// Assignment Tests
TEST(SmallStringBase, AssignConstChar)
{
  SmallString str;
  str.assign("New Value");
  EXPECT_EQ(str.length(), 9u);
  EXPECT_STREQ(str.c_str(), "New Value");
}

TEST(SmallStringBase, AssignConstCharWithLength)
{
  SmallString str;
  str.assign("New Value Extended", 9);
  EXPECT_EQ(str.length(), 9u);
  EXPECT_STREQ(str.c_str(), "New Value");
}

TEST(SmallStringBase, AssignStdString)
{
  SmallString str;
  std::string stdStr = "std::string value";
  str.assign(stdStr);
  EXPECT_STREQ(str.c_str(), stdStr.c_str());
}

TEST(SmallStringBase, AssignStringView)
{
  SmallString str;
  std::string_view sv = "string_view value"sv;
  str.assign(sv);
  EXPECT_EQ(str.view(), sv);
}

TEST(SmallStringBase, AssignSmallStringBase)
{
  SmallString str;
  SmallString other("Other string");
  str.assign(other);
  EXPECT_STREQ(str.c_str(), other.c_str());
}

TEST(SmallStringBase, AssignMove)
{
  SmallString str;
  SmallString other("Move this");
  str.assign(std::move(other));
  EXPECT_STREQ(str.c_str(), "Move this");
}

// Clear Tests
TEST(SmallStringBase, Clear)
{
  SmallString str("Some content");
  EXPECT_FALSE(str.empty());
  str.clear();
  EXPECT_TRUE(str.empty());
  EXPECT_EQ(str.length(), 0u);
}

// Append Tests
TEST(SmallStringBase, AppendChar)
{
  SmallString str("Hello");
  str.append('!');
  EXPECT_STREQ(str.c_str(), "Hello!");
}

TEST(SmallStringBase, AppendConstChar)
{
  SmallString str("Hello");
  str.append(" World");
  EXPECT_STREQ(str.c_str(), "Hello World");
}

TEST(SmallStringBase, AppendConstCharWithLength)
{
  SmallString str("Hello");
  str.append(" World!!!", 6);
  EXPECT_STREQ(str.c_str(), "Hello World");
}

TEST(SmallStringBase, AppendStdString)
{
  SmallString str("Hello");
  std::string suffix = " World";
  str.append(suffix);
  EXPECT_STREQ(str.c_str(), "Hello World");
}

TEST(SmallStringBase, AppendStringView)
{
  SmallString str("Hello");
  str.append(" World"sv);
  EXPECT_STREQ(str.c_str(), "Hello World");
}

TEST(SmallStringBase, AppendSmallStringBase)
{
  SmallString str("Hello");
  SmallString suffix(" World");
  str.append(suffix);
  EXPECT_STREQ(str.c_str(), "Hello World");
}

TEST(SmallStringBase, AppendSprintf)
{
  SmallString str("Value: ");
  str.append_sprintf("%d", 42);
  EXPECT_STREQ(str.c_str(), "Value: 42");
}

TEST(SmallStringBase, AppendFormat)
{
  SmallString str("Value: ");
  str.append_format("{}", 42);
  EXPECT_STREQ(str.c_str(), "Value: 42");
}

TEST(SmallStringBase, AppendHex)
{
  SmallString str;
  const u8 data[] = {0xDE, 0xAD, 0xBE, 0xEF};
  str.append_hex(data, sizeof(data));
  EXPECT_STREQ(str.c_str(), "deadbeef");
}

TEST(SmallStringBase, AppendHexCommaSeparated)
{
  SmallString str;
  const u8 data[] = {0xDE, 0xAD};
  str.append_hex(data, sizeof(data), true);
  EXPECT_STREQ(str.c_str(), "0xde, 0xad");
}

// Prepend Tests
TEST(SmallStringBase, PrependChar)
{
  SmallString str("ello");
  str.prepend('H');
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, PrependConstChar)
{
  SmallString str("World");
  str.prepend("Hello ");
  EXPECT_STREQ(str.c_str(), "Hello World");
}

TEST(SmallStringBase, PrependConstCharWithLength)
{
  SmallString str("World");
  str.prepend("Hello XXX", 6);
  EXPECT_STREQ(str.c_str(), "Hello World");
}

TEST(SmallStringBase, PrependStdString)
{
  SmallString str("World");
  std::string prefix = "Hello ";
  str.prepend(prefix);
  EXPECT_STREQ(str.c_str(), "Hello World");
}

TEST(SmallStringBase, PrependStringView)
{
  SmallString str("World");
  str.prepend("Hello "sv);
  EXPECT_STREQ(str.c_str(), "Hello World");
}

TEST(SmallStringBase, PrependSmallStringBase)
{
  SmallString str("World");
  SmallString prefix("Hello ");
  str.prepend(prefix);
  EXPECT_STREQ(str.c_str(), "Hello World");
}

TEST(SmallStringBase, PrependSprintf)
{
  SmallString str(" items");
  str.prepend_sprintf("%d", 5);
  EXPECT_STREQ(str.c_str(), "5 items");
}

TEST(SmallStringBase, PrependFormat)
{
  SmallString str(" items");
  str.prepend_format("{}", 5);
  EXPECT_STREQ(str.c_str(), "5 items");
}

// Insert Tests
TEST(SmallStringBase, InsertConstChar)
{
  SmallString str("Hello World");
  str.insert(5, " Beautiful");
  EXPECT_STREQ(str.c_str(), "Hello Beautiful World");
}

TEST(SmallStringBase, InsertConstCharWithLength)
{
  SmallString str("Hello World");
  str.insert(5, " BeautifulXXX", 10);
  EXPECT_STREQ(str.c_str(), "Hello Beautiful World");
}

TEST(SmallStringBase, InsertStdString)
{
  SmallString str("Hello World");
  std::string insert = " Beautiful";
  str.insert(5, insert);
  EXPECT_STREQ(str.c_str(), "Hello Beautiful World");
}

TEST(SmallStringBase, InsertStringView)
{
  SmallString str("Hello World");
  str.insert(5, " Beautiful"sv);
  EXPECT_STREQ(str.c_str(), "Hello Beautiful World");
}

TEST(SmallStringBase, InsertSmallStringBase)
{
  SmallString str("Hello World");
  SmallString insert(" Beautiful");
  str.insert(5, insert);
  EXPECT_STREQ(str.c_str(), "Hello Beautiful World");
}

TEST(SmallStringBase, InsertNegativeOffset)
{
  SmallString str("Hello World");
  str.insert(-6, "Beautiful ");
  EXPECT_STREQ(str.c_str(), "HelloBeautiful  World");
}

// Format Tests
TEST(SmallStringBase, Sprintf)
{
  SmallString str;
  str.sprintf("Value: %d, String: %s", 42, "test");
  EXPECT_STREQ(str.c_str(), "Value: 42, String: test");
}

TEST(SmallStringBase, Format)
{
  SmallString str;
  str.format("Value: {}, String: {}", 42, "test");
  EXPECT_STREQ(str.c_str(), "Value: 42, String: test");
}

// Comparison Tests - equals
TEST(SmallStringBase, EqualsConstChar)
{
  SmallString str("Hello");
  EXPECT_TRUE(str.equals("Hello"));
  EXPECT_FALSE(str.equals("hello"));
  EXPECT_FALSE(str.equals("Hello World"));
}

TEST(SmallStringBase, EqualsSmallStringBase)
{
  SmallString str("Hello");
  SmallString other("Hello");
  SmallString different("World");
  EXPECT_TRUE(str.equals(other));
  EXPECT_FALSE(str.equals(different));
}

TEST(SmallStringBase, EqualsStringView)
{
  SmallString str("Hello");
  EXPECT_TRUE(str.equals("Hello"sv));
  EXPECT_FALSE(str.equals("hello"sv));
}

TEST(SmallStringBase, EqualsStdString)
{
  SmallString str("Hello");
  std::string same = "Hello";
  std::string different = "World";
  EXPECT_TRUE(str.equals(same));
  EXPECT_FALSE(str.equals(different));
}

TEST(SmallStringBase, EqualsEmptyString)
{
  SmallString str;
  EXPECT_TRUE(str.equals(""));
  EXPECT_FALSE(str.equals("x"));
}

// Comparison Tests - iequals (case insensitive)
TEST(SmallStringBase, IEqualsConstChar)
{
  SmallString str("Hello");
  EXPECT_TRUE(str.iequals("hello"));
  EXPECT_TRUE(str.iequals("HELLO"));
  EXPECT_TRUE(str.iequals("HeLLo"));
  EXPECT_FALSE(str.iequals("World"));
}

TEST(SmallStringBase, IEqualsStringView)
{
  SmallString str("Hello");
  EXPECT_TRUE(str.iequals("hello"sv));
  EXPECT_TRUE(str.iequals("HELLO"sv));
}

TEST(SmallStringBase, IEqualsStdString)
{
  SmallString str("Hello");
  std::string same = "hello";
  EXPECT_TRUE(str.iequals(same));
}

// Comparison Tests - compare
TEST(SmallStringBase, CompareConstChar)
{
  SmallString str("banana");
  EXPECT_LT(str.compare("cherry"), 0);
  EXPECT_GT(str.compare("apple"), 0);
  EXPECT_EQ(str.compare("banana"), 0);
}

TEST(SmallStringBase, CompareSmallStringBase)
{
  SmallString str("banana");
  SmallString less("apple");
  SmallString greater("cherry");
  SmallString equal("banana");
  EXPECT_GT(str.compare(less), 0);
  EXPECT_LT(str.compare(greater), 0);
  EXPECT_EQ(str.compare(equal), 0);
}

TEST(SmallStringBase, CompareStringView)
{
  SmallString str("banana");
  EXPECT_LT(str.compare("cherry"sv), 0);
  EXPECT_GT(str.compare("apple"sv), 0);
  EXPECT_EQ(str.compare("banana"sv), 0);
}

TEST(SmallStringBase, CompareStdString)
{
  SmallString str("banana");
  std::string less = "apple";
  std::string greater = "cherry";
  std::string equal = "banana";
  EXPECT_GT(str.compare(less), 0);
  EXPECT_LT(str.compare(greater), 0);
  EXPECT_EQ(str.compare(equal), 0);
}

TEST(SmallStringBase, CompareEmptyStrings)
{
  SmallString str;
  EXPECT_EQ(str.compare(""), 0);
  EXPECT_LT(str.compare("a"), 0);

  str.assign("a");
  SmallString empty;
  EXPECT_GT(str.compare(empty), 0);
}

TEST(SmallStringBase, CompareDifferentLengths)
{
  SmallString str("abc");
  EXPECT_LT(str.compare("abcd"), 0);
  EXPECT_GT(str.compare("ab"), 0);
}

// Comparison Tests - icompare (case insensitive)
TEST(SmallStringBase, ICompareConstChar)
{
  SmallString str("Banana");
  EXPECT_LT(str.icompare("CHERRY"), 0);
  EXPECT_GT(str.icompare("APPLE"), 0);
  EXPECT_EQ(str.icompare("BANANA"), 0);
}

TEST(SmallStringBase, ICompareSmallStringBase)
{
  SmallString str("Banana");
  SmallString equal("BANANA");
  EXPECT_EQ(str.icompare(equal), 0);
}

TEST(SmallStringBase, ICompareStringView)
{
  SmallString str("Banana");
  EXPECT_EQ(str.icompare("BANANA"sv), 0);
}

TEST(SmallStringBase, ICompareStdString)
{
  SmallString str("Banana");
  std::string equal = "BANANA";
  EXPECT_EQ(str.icompare(equal), 0);
}

// StartsWith Tests
TEST(SmallStringBase, StartsWithConstChar)
{
  SmallString str("Hello World");
  EXPECT_TRUE(str.starts_with("Hello"));
  EXPECT_TRUE(str.starts_with("H"));
  EXPECT_TRUE(str.starts_with(""));
  EXPECT_FALSE(str.starts_with("World"));
  EXPECT_FALSE(str.starts_with("hello"));
}

TEST(SmallStringBase, StartsWithCaseInsensitive)
{
  SmallString str("Hello World");
  EXPECT_TRUE(str.starts_with("HELLO", false));
  EXPECT_TRUE(str.starts_with("hello", false));
  EXPECT_FALSE(str.starts_with("hello", true));
}

TEST(SmallStringBase, StartsWithSmallStringBase)
{
  SmallString str("Hello World");
  SmallString prefix("Hello");
  EXPECT_TRUE(str.starts_with(prefix));
}

TEST(SmallStringBase, StartsWithStringView)
{
  SmallString str("Hello World");
  EXPECT_TRUE(str.starts_with("Hello"sv));
}

TEST(SmallStringBase, StartsWithStdString)
{
  SmallString str("Hello World");
  std::string prefix = "Hello";
  EXPECT_TRUE(str.starts_with(prefix));
}

TEST(SmallStringBase, StartsWithLongerPrefix)
{
  SmallString str("Hi");
  EXPECT_FALSE(str.starts_with("Hello"));
}

// EndsWith Tests
TEST(SmallStringBase, EndsWithConstChar)
{
  SmallString str("Hello World");
  EXPECT_TRUE(str.ends_with("World"));
  EXPECT_TRUE(str.ends_with("d"));
  EXPECT_TRUE(str.ends_with(""));
  EXPECT_FALSE(str.ends_with("Hello"));
  EXPECT_FALSE(str.ends_with("world"));
}

TEST(SmallStringBase, EndsWithCaseInsensitive)
{
  SmallString str("Hello World");
  EXPECT_TRUE(str.ends_with("WORLD", false));
  EXPECT_TRUE(str.ends_with("world", false));
  EXPECT_FALSE(str.ends_with("world", true));
}

TEST(SmallStringBase, EndsWithSmallStringBase)
{
  SmallString str("Hello World");
  SmallString suffix("World");
  EXPECT_TRUE(str.ends_with(suffix));
}

TEST(SmallStringBase, EndsWithStringView)
{
  SmallString str("Hello World");
  EXPECT_TRUE(str.ends_with("World"sv));
}

TEST(SmallStringBase, EndsWithStdString)
{
  SmallString str("Hello World");
  std::string suffix = "World";
  EXPECT_TRUE(str.ends_with(suffix));
}

TEST(SmallStringBase, EndsWithLongerSuffix)
{
  SmallString str("Hi");
  EXPECT_FALSE(str.ends_with("Hello"));
}

// Find Tests
TEST(SmallStringBase, FindChar)
{
  SmallString str("Hello World");
  EXPECT_EQ(str.find('o'), 4);
  EXPECT_EQ(str.find('W'), 6);
  EXPECT_EQ(str.find('z'), -1);
}

TEST(SmallStringBase, FindCharWithOffset)
{
  SmallString str("Hello World");
  EXPECT_EQ(str.find('o', 5), 7);
  EXPECT_EQ(str.find('o', 8), -1);
}

TEST(SmallStringBase, RFindChar)
{
  SmallString str("Hello World");
  EXPECT_EQ(str.rfind('o'), 7);
  EXPECT_EQ(str.rfind('l'), 9);
}

TEST(SmallStringBase, FindCharEmptyString)
{
  SmallString str;
  EXPECT_EQ(str.find('a'), -1);
  EXPECT_EQ(str.rfind('a'), -1);
}

TEST(SmallStringBase, FindString)
{
  SmallString str("Hello World Hello");
  EXPECT_EQ(str.find("Hello"), 0);
  EXPECT_EQ(str.find("World"), 6);
  EXPECT_EQ(str.find("Goodbye"), -1);
}

TEST(SmallStringBase, FindStringWithOffset)
{
  SmallString str("Hello World Hello");
  EXPECT_EQ(str.find("Hello", 1), 12);
  EXPECT_EQ(str.find("Hello", 13), -1);
}

// Count Tests
TEST(SmallStringBase, Count)
{
  SmallString str("Hello World");
  EXPECT_EQ(str.count('l'), 3u);
  EXPECT_EQ(str.count('o'), 2u);
  EXPECT_EQ(str.count('z'), 0u);
}

// Replace Tests
TEST(SmallStringBase, Replace)
{
  SmallString str("Hello World");
  u32 count = str.replace("World", "Universe");
  EXPECT_EQ(count, 1u);
  EXPECT_STREQ(str.c_str(), "Hello Universe");
}

TEST(SmallStringBase, ReplaceMultiple)
{
  SmallString str("Hello Hello Hello");
  u32 count = str.replace("Hello", "Hi");
  EXPECT_EQ(count, 3u);
  EXPECT_STREQ(str.c_str(), "Hi Hi Hi");
}

TEST(SmallStringBase, ReplaceNoMatch)
{
  SmallString str("Hello World");
  u32 count = str.replace("Goodbye", "Hi");
  EXPECT_EQ(count, 0u);
  EXPECT_STREQ(str.c_str(), "Hello World");
}

TEST(SmallStringBase, ReplaceWithLonger)
{
  SmallString str("Hi");
  str.replace("Hi", "Hello World");
  EXPECT_STREQ(str.c_str(), "Hello World");
}

TEST(SmallStringBase, ReplaceWithShorter)
{
  SmallString str("Hello World");
  str.replace("Hello World", "Hi");
  EXPECT_STREQ(str.c_str(), "Hi");
}

// Erase Tests
TEST(SmallStringBase, EraseFromOffset)
{
  SmallString str("Hello World");
  str.erase(5);
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, EraseWithCount)
{
  SmallString str("Hello World");
  str.erase(5, 1);
  EXPECT_STREQ(str.c_str(), "HelloWorld");
}

TEST(SmallStringBase, EraseNegativeOffset)
{
  SmallString str("Hello World");
  str.erase(-5);
  EXPECT_STREQ(str.c_str(), "Hello ");
}

TEST(SmallStringBase, EraseAll)
{
  SmallString str("Hello World");
  str.erase(0);
  EXPECT_TRUE(str.empty());
}

// Reserve/Resize Tests
TEST(SmallStringBase, Reserve)
{
  SmallString str;
  str.reserve(100);
  EXPECT_GE(str.buffer_size(), 100u);
  EXPECT_TRUE(str.empty());
}

TEST(SmallStringBase, ResizeGrow)
{
  SmallString str("Hello");
  str.resize(10, 'X');
  EXPECT_EQ(str.length(), 10u);
  EXPECT_STREQ(str.c_str(), "HelloXXXXX");
}

TEST(SmallStringBase, ResizeShrink)
{
  SmallString str("Hello World");
  str.resize(5);
  EXPECT_EQ(str.length(), 5u);
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, SetSize)
{
  SmallString str("Hello World");
  str.set_size(5);
  EXPECT_EQ(str.length(), 5u);
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, UpdateSize)
{
  SmallString str;
  str.reserve(20);
  std::strcpy(str.data(), "Manual");
  str.update_size();
  EXPECT_EQ(str.length(), 6u);
  EXPECT_STREQ(str.c_str(), "Manual");
}

TEST(SmallStringBase, MakeRoomFor)
{
  SmallString str("Hello");
  str.make_room_for(1000);
  EXPECT_GE(str.buffer_size(), str.length() + 1000u);
}

// Case Conversion Tests
TEST(SmallStringBase, ConvertToLowerCase)
{
  SmallString str("HELLO WORLD");
  str.convert_to_lower_case();
  EXPECT_STREQ(str.c_str(), "hello world");
}

TEST(SmallStringBase, ConvertToUpperCase)
{
  SmallString str("hello world");
  str.convert_to_upper_case();
  EXPECT_STREQ(str.c_str(), "HELLO WORLD");
}

// View/Substr Tests
TEST(SmallStringBase, View)
{
  SmallString str("Hello World");
  std::string_view sv = str.view();
  EXPECT_EQ(sv, "Hello World");
}

TEST(SmallStringBase, ViewEmpty)
{
  SmallString str;
  std::string_view sv = str.view();
  EXPECT_TRUE(sv.empty());
}

TEST(SmallStringBase, Substr)
{
  SmallString str("Hello World");
  std::string_view sv = str.substr(0, 5);
  EXPECT_EQ(sv, "Hello");
}

TEST(SmallStringBase, SubstrNegativeOffset)
{
  SmallString str("Hello World");
  std::string_view sv = str.substr(-5, 5);
  EXPECT_EQ(sv, "World");
}

TEST(SmallStringBase, SubstrNegativeCount)
{
  SmallString str("Hello World");
  std::string_view sv = str.substr(0, -6);
  EXPECT_EQ(sv, "Hello");
}

// Span Tests
TEST(SmallStringBase, CSpan)
{
  SmallString str("Hello");
  std::span<const char> sp = str.cspan();
  EXPECT_EQ(sp.size(), 5u);
  EXPECT_EQ(sp[0], 'H');
}

TEST(SmallStringBase, Span)
{
  SmallString str("Hello");
  std::span<char> sp = str.span();
  sp[0] = 'J';
  EXPECT_STREQ(str.c_str(), "Jello");
}

TEST(SmallStringBase, CBSpan)
{
  SmallString str("AB");
  std::span<const u8> sp = str.cbspan();
  EXPECT_EQ(sp.size(), 2u);
  EXPECT_EQ(sp[0], static_cast<u8>('A'));
}

TEST(SmallStringBase, BSpan)
{
  SmallString str("AB");
  std::span<u8> sp = str.bspan();
  sp[0] = static_cast<u8>('X');
  EXPECT_STREQ(str.c_str(), "XB");
}

// STL Adapter Tests
TEST(SmallStringBase, Front)
{
  SmallString str("Hello");
  EXPECT_EQ(str.front(), 'H');
}

TEST(SmallStringBase, Back)
{
  SmallString str("Hello");
  EXPECT_EQ(str.back(), 'o');
}

TEST(SmallStringBase, PushBack)
{
  SmallString str("Hello");
  str.push_back('!');
  EXPECT_STREQ(str.c_str(), "Hello!");
}

TEST(SmallStringBase, PopBack)
{
  SmallString str("Hello!");
  str.pop_back();
  EXPECT_STREQ(str.c_str(), "Hello");
}

// Accessor Tests
TEST(SmallStringBase, Length)
{
  SmallString str("Hello");
  EXPECT_EQ(str.length(), 5u);
}

TEST(SmallStringBase, Empty)
{
  SmallString str;
  EXPECT_TRUE(str.empty());
  str.assign("x");
  EXPECT_FALSE(str.empty());
}

TEST(SmallStringBase, BufferSize)
{
  SmallString str;
  EXPECT_GT(str.buffer_size(), 0u);
}

TEST(SmallStringBase, CStr)
{
  SmallString str("Hello");
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, Data)
{
  SmallString str("Hello");
  str.data()[0] = 'J';
  EXPECT_STREQ(str.c_str(), "Jello");
}

TEST(SmallStringBase, EndPtr)
{
  SmallString str("Hello");
  const char* end = str.end_ptr();
  EXPECT_EQ(end - str.c_str(), 5);
}

// Operator Tests
TEST(SmallStringBase, OperatorConstCharStar)
{
  SmallString str("Hello");
  const char* ptr = str;
  EXPECT_STREQ(ptr, "Hello");
}

TEST(SmallStringBase, OperatorCharStar)
{
  SmallString str("Hello");
  char* ptr = str;
  ptr[0] = 'J';
  EXPECT_STREQ(str.c_str(), "Jello");
}

TEST(SmallStringBase, OperatorStringView)
{
  SmallString str("Hello");
  std::string_view sv = str;
  EXPECT_EQ(sv, "Hello");
}

TEST(SmallStringBase, OperatorEqualityConstChar)
{
  SmallString str("Hello");
  EXPECT_TRUE(str == "Hello");
  EXPECT_FALSE(str == "World");
}

TEST(SmallStringBase, OperatorEqualitySmallStringBase)
{
  SmallString str("Hello");
  SmallString other("Hello");
  SmallString different("World");
  EXPECT_TRUE(str == other);
  EXPECT_FALSE(str == different);
}

TEST(SmallStringBase, OperatorEqualityStringView)
{
  SmallString str("Hello");
  EXPECT_TRUE(str == "Hello"sv);
  EXPECT_FALSE(str == "World"sv);
}

TEST(SmallStringBase, OperatorEqualityStdString)
{
  SmallString str("Hello");
  std::string same = "Hello";
  std::string different = "World";
  EXPECT_TRUE(str == same);
  EXPECT_FALSE(str == different);
}

TEST(SmallStringBase, OperatorInequality)
{
  SmallString str("Hello");
  EXPECT_TRUE(str != "World");
  EXPECT_FALSE(str != "Hello");
}

TEST(SmallStringBase, OperatorLessThan)
{
  SmallString str("apple");
  EXPECT_TRUE(str < "banana");
  EXPECT_FALSE(str < "aardvark");
}

TEST(SmallStringBase, OperatorGreaterThan)
{
  SmallString str("banana");
  EXPECT_TRUE(str > "apple");
  EXPECT_FALSE(str > "cherry");
}

TEST(SmallStringBase, OperatorAssignConstChar)
{
  SmallString str;
  str = "Hello";
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, OperatorAssignStdString)
{
  SmallString str;
  std::string s = "Hello";
  str = s;
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, OperatorAssignStringView)
{
  SmallString str;
  str = "Hello"sv;
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, OperatorAssignSmallStringBase)
{
  SmallString str;
  SmallString other("Hello");
  str = other;
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, OperatorAssignMove)
{
  SmallString str;
  SmallString other("Hello");
  str = std::move(other);
  EXPECT_STREQ(str.c_str(), "Hello");
}

// ShrinkToFit Tests
TEST(SmallStringBase, ShrinkToFit)
{
  SmallString str;
  str.reserve(1000);
  str.assign("Hi");
  u32 size_before = str.buffer_size();
  str.shrink_to_fit();
  EXPECT_LE(str.buffer_size(), size_before);
  EXPECT_STREQ(str.c_str(), "Hi");
}

TEST(SmallStringBase, ShrinkToFitEmpty)
{
  SmallString str;
  str.reserve(1000);
  str.clear();
  str.shrink_to_fit();
  EXPECT_TRUE(str.empty());
}

// Edge Cases
TEST(SmallStringBase, AppendEmptyString)
{
  SmallString str("Hello");
  str.append("");
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, PrependEmptyString)
{
  SmallString str("Hello");
  str.prepend("");
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, InsertEmptyString)
{
  SmallString str("Hello");
  str.insert(2, "");
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, LargeStringAppend)
{
  SmallString str;
  for (int i = 0; i < 1000; i++)
  {
    str.append('X');
  }
  EXPECT_EQ(str.length(), 1000u);
}

TEST(SmallStringBase, EraseNegativeOffsetMiddle)
{
  SmallString str("Hello World");
  // Erase 3 characters starting from position -6 (i.e., position 5 = ' ')
  // Should result in "Hellorld" (erase " Wo")
  str.erase(-6, 3);
  EXPECT_STREQ(str.c_str(), "Hellorld");
  EXPECT_EQ(str.length(), 8u);
}

TEST(SmallStringBase, MoveAssignmentActuallyMoves)
{
  // Create a string large enough to force heap allocation
  SmallString source;
  source.reserve(1000);
  source.assign("This is a long string that should be on the heap");

  const char* original_buffer = source.c_str();

  SmallString dest;
  dest = std::move(source);

  EXPECT_STREQ(dest.c_str(), "This is a long string that should be on the heap");
  EXPECT_TRUE(source.empty() || source.c_str() != original_buffer);
}

TEST(SmallStringBase, SmallStringBaseCaseInsensitive)
{
  SmallString str("Hello");
  SmallString other("HELLO");
  SmallString otherLower("hello");
  SmallString otherMixed("hElLo");

  EXPECT_TRUE(str.iequals(other));
  EXPECT_TRUE(str.iequals(otherLower));
  EXPECT_TRUE(str.iequals(otherMixed));

  // Check const char* overloads
  EXPECT_TRUE(str.iequals("Hello"));
  EXPECT_TRUE(str.iequals("HELLO"));
  EXPECT_TRUE(str.iequals("hello"));
  EXPECT_TRUE(str.iequals("hElLo"));
}

#ifdef _WIN32

TEST(SmallStringBase, WStringViewAssignNullTermination)
{
  SmallString str;
  // First assign something to ensure the buffer has garbage after
  str.assign("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");

  // Now assign a shorter wstring
  std::wstring_view wstr = L"Hello";
  str.assign(wstr);

  EXPECT_EQ(str.length(), 5u);
  EXPECT_STREQ(str.c_str(), "Hello");

  // Verify the string is properly null-terminated by checking length matches strlen
  EXPECT_EQ(std::strlen(str.c_str()), str.length());
}

TEST(SmallStringBase, WStringViewAssignEmptyFollowedByContent)
{
  SmallString str;
  str.reserve(100);

  // Assign via wstring_view
  std::wstring_view wstr = L"Test";
  str.assign(wstr);

  // The string should be exactly "Test" with no trailing garbage
  EXPECT_EQ(str.length(), 4u);
  EXPECT_EQ(str.view(), "Test");

  // Append should work correctly if null-terminated properly
  str.append("123");
  EXPECT_STREQ(str.c_str(), "Test123");
}
#endif

TEST(SmallStringBase, EraseNegativeOffsetPreservesRemainder)
{
  SmallString str("ABCDEFGHIJ");
  // Erase 2 characters starting from -5 (position 5 = 'F')
  // Should result in "ABCDEHIJ" (erase "FG")
  str.erase(-5, 2);
  EXPECT_STREQ(str.c_str(), "ABCDEHIJ");
  EXPECT_EQ(str.length(), 8u);
}

TEST(SmallStringBase, AppendHexNullTermination)
{
  SmallString str;
  // Pre-fill with garbage to detect missing null terminator
  str.assign("XXXXXXXXXXXXXXXXXXXX");
  str.clear();

  const u8 data[] = {0xAB, 0xCD};
  str.append_hex(data, sizeof(data));

  EXPECT_EQ(str.length(), 4u);
  EXPECT_STREQ(str.c_str(), "abcd");
  // Verify null termination by checking strlen matches length
  EXPECT_EQ(std::strlen(str.c_str()), str.length());
}

TEST(SmallStringBase, AppendHexCommaSeparatedNullTermination)
{
  SmallString str;
  str.assign("XXXXXXXXXXXXXXXXXXXX");
  str.clear();

  const u8 data[] = {0xAB, 0xCD};
  str.append_hex(data, sizeof(data), true);

  EXPECT_EQ(str.length(), 10u);
  EXPECT_STREQ(str.c_str(), "0xab, 0xcd");
  EXPECT_EQ(std::strlen(str.c_str()), str.length());
}

TEST(SmallStringBase, AppendHexThenAppendMore)
{
  SmallString str("Prefix: ");
  const u8 data[] = {0xFF};
  str.append_hex(data, sizeof(data));
  str.append(" Suffix");

  // If null terminator is missing, append will start from wrong position
  EXPECT_STREQ(str.c_str(), "Prefix: ff Suffix");
}

TEST(SmallStringBase, PrependSprintfLargeString)
{
  SmallString str(" end");

  std::string largeFormat(1500, 'X');
  str.prepend_sprintf("%s", largeFormat.c_str());

  std::string expected = largeFormat + " end";
  EXPECT_STREQ(str.c_str(), expected.c_str());
}

TEST(SmallStringBase, ShrinkToFitEmptyHeapStringResetsState)
{
  SmallString str;
  str.reserve(1000);   // Force heap allocation
  str.clear();         // Empty the string
  str.shrink_to_fit(); // Should free heap and restore to valid state

  // After shrink_to_fit on empty heap string, string should still be usable
  EXPECT_TRUE(str.empty());
  EXPECT_EQ(str.length(), 0u);

  // These operations should not crash
  str.append("Hello");
  EXPECT_STREQ(str.c_str(), "Hello");
}

TEST(SmallStringBase, ShrinkToFitEmptyThenClear)
{
  SmallString str;
  str.reserve(1000); // Force heap allocation
  str.clear();
  str.shrink_to_fit();

  // clear() should not crash after shrink_to_fit on empty heap string
  str.clear();
  EXPECT_TRUE(str.empty());
}

TEST(SmallStringBase, ShrinkToFitEmptyThenReserve)
{
  SmallString str;
  str.reserve(1000); // Force heap allocation
  str.clear();
  str.shrink_to_fit();

  // reserve() should work correctly after shrink_to_fit on empty heap string
  str.reserve(100);
  EXPECT_GE(str.buffer_size(), 100u);

  str.assign("Test");
  EXPECT_STREQ(str.c_str(), "Test");
}

TEST(SmallStringBase, ReplaceMiddleNullTermination)
{
  SmallString str("Hello World End");
  str.replace("World", "X");

  EXPECT_EQ(str.length(), 11u); // "Hello X End" = 11 chars
  EXPECT_STREQ(str.c_str(), "Hello X End");

  // Verify null termination by checking strlen matches length
  EXPECT_EQ(std::strlen(str.c_str()), str.length());
}

TEST(SmallStringBase, ReplaceMiddleShorterReplacement)
{
  SmallString str("AAABBBCCC");
  str.replace("BBB", "X");

  EXPECT_EQ(str.length(), 7u); // "AAAXCCC" = 7 chars
  EXPECT_STREQ(str.c_str(), "AAAXCCC");
  EXPECT_EQ(std::strlen(str.c_str()), str.length());
}

TEST(SmallStringBase, ReplaceMiddleThenAppend)
{
  SmallString str("Hello World End");
  str.replace("World", "X");

  // If null terminator is missing, append will start from wrong position
  str.append("!");
  EXPECT_STREQ(str.c_str(), "Hello X End!");
  EXPECT_EQ(str.length(), 12u);
}

TEST(SmallStringBase, ReplaceMultipleInMiddle)
{
  SmallString str("aXXXbXXXc");
  u32 count = str.replace("XXX", "Y");

  EXPECT_EQ(count, 2u);
  EXPECT_STREQ(str.c_str(), "aYbYc");
  EXPECT_EQ(str.length(), 5u);
  EXPECT_EQ(std::strlen(str.c_str()), str.length());
}

TEST(SmallStringBase, ReplaceEmptySearchString)
{
  SmallString str("Hello");

  // This should either be a no-op or handle gracefully, not infinite loop
  // Set a timeout expectation or just verify it completes
  u32 count = str.replace("", "X");

  // Expected: either 0 replacements, or reasonable behavior
  // Actual with bug: infinite loop (test hangs)
  EXPECT_EQ(count, 0u);
  EXPECT_STREQ(str.c_str(), "Hello");
}

//////////////////////////////////////////////////////////////////////////
// SmallStackString Test Suite
//////////////////////////////////////////////////////////////////////////

TEST(SmallStackString, DefaultConstructor)
{
  SmallString s;
  EXPECT_TRUE(s.empty());
  EXPECT_EQ(s.length(), 0u);
  EXPECT_EQ(s.buffer_size(), 256u);
}

TEST(SmallStackString, ConstCharConstructor)
{
  SmallString s("Hello");
  EXPECT_EQ(s.length(), 5u);
  EXPECT_STREQ(s.c_str(), "Hello");
}

TEST(SmallStackString, ConstCharWithLengthConstructor)
{
  SmallString s("Hello World", 5);
  EXPECT_EQ(s.length(), 5u);
  EXPECT_STREQ(s.c_str(), "Hello");
}

TEST(SmallStackString, CopyConstructorFromBase)
{
  SmallString original("Test");
  SmallStringBase& base = original;
  SmallString copy(base);
  EXPECT_STREQ(copy.c_str(), "Test");
}

TEST(SmallStackString, MoveConstructorFromBase)
{
  SmallString original("Test");
  SmallStringBase& base = original;
  SmallString moved(std::move(base));
  EXPECT_STREQ(moved.c_str(), "Test");
}

TEST(SmallStackString, CopyConstructorFromSame)
{
  SmallString original("Test");
  SmallString copy(original);
  EXPECT_STREQ(copy.c_str(), "Test");
}

TEST(SmallStackString, MoveConstructorFromSame)
{
  SmallString original("Test");
  SmallString moved(std::move(original));
  EXPECT_STREQ(moved.c_str(), "Test");
}

TEST(SmallStackString, StdStringConstructor)
{
  std::string s = "Hello";
  SmallString ss(s);
  EXPECT_STREQ(ss.c_str(), "Hello");
}

TEST(SmallStackString, StringViewConstructor)
{
  SmallString ss("Hello"sv);
  EXPECT_STREQ(ss.c_str(), "Hello");
}

TEST(SmallStackString, AssignmentFromBase)
{
  SmallString str;
  SmallString other("Source");
  SmallStringBase& base = other;
  str = base;
  EXPECT_STREQ(str.c_str(), "Source");
}

TEST(SmallStackString, AssignmentMoveFromBase)
{
  SmallString str;
  SmallString other("Source");
  SmallStringBase& base = other;
  str = std::move(base);
  EXPECT_STREQ(str.c_str(), "Source");
}

TEST(SmallStackString, AssignmentFromSame)
{
  SmallString str;
  SmallString other("Source");
  str = other;
  EXPECT_STREQ(str.c_str(), "Source");
}

TEST(SmallStackString, AssignmentMoveFromSame)
{
  SmallString str;
  SmallString other("Source");
  str = std::move(other);
  EXPECT_STREQ(str.c_str(), "Source");
}

TEST(SmallStackString, AssignmentFromStdString)
{
  SmallString str;
  std::string s = "Source";
  str = s;
  EXPECT_STREQ(str.c_str(), "Source");
}

TEST(SmallStackString, AssignmentFromStringView)
{
  SmallString str;
  str = "Source"sv;
  EXPECT_STREQ(str.c_str(), "Source");
}

TEST(SmallStackString, AssignmentFromConstChar)
{
  SmallString str;
  str = "Source";
  EXPECT_STREQ(str.c_str(), "Source");
}

TEST(SmallStackString, FromSprintf)
{
  SmallString s = SmallString::from_sprintf("Value: %d", 42);
  EXPECT_STREQ(s.c_str(), "Value: 42");
}

TEST(SmallStackString, FromFormat)
{
  SmallString s = SmallString::from_format("Value: {}", 42);
  EXPECT_STREQ(s.c_str(), "Value: 42");
}

TEST(SmallStackString, FromVFormat)
{
  constexpr int i = 42;
  auto args = fmt::make_format_args(i);
  SmallString s = SmallString::from_vformat("Value: {}", args);
  EXPECT_STREQ(s.c_str(), "Value: 42");
}

TEST(SmallStackString, TinyStringSize)
{
  TinyString tiny;
  EXPECT_EQ(tiny.buffer_size(), 64u);
}

TEST(SmallStackString, SmallStringSize)
{
  SmallString small;
  EXPECT_EQ(small.buffer_size(), 256u);
}

TEST(SmallStackString, LargeStringSize)
{
  LargeString large;
  EXPECT_EQ(large.buffer_size(), 512u);
}

TEST(SmallStackString, StackBufferOverflow)
{
  TinyString tiny;
  // TinyString has 64 bytes, so appending more should trigger heap allocation
  for (int i = 0; i < 100; i++)
  {
    tiny.append('X');
  }
  EXPECT_EQ(tiny.length(), 100u);
  EXPECT_GE(tiny.buffer_size(), 100u);
}

TEST(SmallStackString, FmtFormatter)
{
  SmallString s("test");
  std::string formatted = fmt::format("Value: {}", s);
  EXPECT_EQ(formatted, "Value: test");
}

TEST(SmallStackString, TinyStringFmtFormatter)
{
  TinyString s("tiny");
  std::string formatted = fmt::format("Value: {}", s);
  EXPECT_EQ(formatted, "Value: tiny");
}

TEST(SmallStackString, LargeStringFmtFormatter)
{
  LargeString s("large");
  std::string formatted = fmt::format("Value: {}", s);
  EXPECT_EQ(formatted, "Value: large");
}


TEST(SmallStackString, MoveConstructorFromSameType)
{
  SmallString source;
  source.reserve(1000); // Force heap allocation
  source.assign("Heap allocated string");

  const char* original_buffer = source.c_str();

  SmallString dest(std::move(source));

  EXPECT_STREQ(dest.c_str(), "Heap allocated string");
  // After proper move, source should be empty or have different buffer
  EXPECT_TRUE(source.empty() || source.c_str() != original_buffer);
}

TEST(SmallStackString, MoveConstructorFromBaseHeap)
{
  SmallString source;
  source.reserve(1000); // Force heap allocation
  source.assign("Heap allocated string");

  const char* original_buffer = source.c_str();
  SmallStringBase& baseRef = source;

  SmallString dest(std::move(baseRef));

  EXPECT_STREQ(dest.c_str(), "Heap allocated string");
  // After proper move, source should be empty or have different buffer
  EXPECT_TRUE(source.empty() || source.c_str() != original_buffer);
}
