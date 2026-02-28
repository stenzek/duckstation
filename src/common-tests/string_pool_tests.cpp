// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/string_pool.h"

#include <gtest/gtest.h>

// ---- BumpStringPool ----

TEST(BumpStringPool, InitiallyEmpty)
{
  BumpStringPool pool;
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
}

TEST(BumpStringPool, AddSingleString)
{
  BumpStringPool pool;
  auto offset = pool.AddString("hello");
  EXPECT_NE(offset, BumpStringPool::InvalidOffset);
  EXPECT_FALSE(pool.IsEmpty());
  EXPECT_EQ(pool.GetString(offset), "hello");
}

TEST(BumpStringPool, AddEmptyStringReturnsInvalid)
{
  BumpStringPool pool;
  auto offset = pool.AddString("");
  EXPECT_EQ(offset, BumpStringPool::InvalidOffset);
  EXPECT_TRUE(pool.IsEmpty());
}

TEST(BumpStringPool, AddEmptyStringViewReturnsInvalid)
{
  BumpStringPool pool;
  auto offset = pool.AddString(std::string_view{});
  EXPECT_EQ(offset, BumpStringPool::InvalidOffset);
  EXPECT_TRUE(pool.IsEmpty());
}

TEST(BumpStringPool, AddMultipleStrings)
{
  BumpStringPool pool;
  auto o1 = pool.AddString("alpha");
  auto o2 = pool.AddString("beta");
  auto o3 = pool.AddString("gamma");

  EXPECT_NE(o1, o2);
  EXPECT_NE(o2, o3);
  EXPECT_EQ(pool.GetString(o1), "alpha");
  EXPECT_EQ(pool.GetString(o2), "beta");
  EXPECT_EQ(pool.GetString(o3), "gamma");
}

TEST(BumpStringPool, DuplicateStringsStored)
{
  BumpStringPool pool;
  auto o1 = pool.AddString("same");
  const size_t size1 = pool.GetSize();

  auto o2 = pool.AddString("same");
  EXPECT_NE(o1, o2);
  EXPECT_GT(pool.GetSize(), size1);
  EXPECT_EQ(pool.GetString(o1), "same");
  EXPECT_EQ(pool.GetString(o2), "same");
}

TEST(BumpStringPool, GetStringWithOffset)
{
  BumpStringPool pool;
  auto offset = pool.AddString("world");
  EXPECT_EQ(pool.GetString(offset), "world");
}

TEST(BumpStringPool, GetStringWithOffsetAndLength)
{
  BumpStringPool pool;
  auto offset = pool.AddString("hello world");
  EXPECT_EQ(pool.GetString(offset, 5), "hello");
  EXPECT_EQ(pool.GetString(offset, 11), "hello world");
}

TEST(BumpStringPool, GetStringInvalidOffset)
{
  BumpStringPool pool;
  EXPECT_EQ(pool.GetString(BumpStringPool::InvalidOffset), std::string_view{});
}

TEST(BumpStringPool, GetStringOutOfBounds)
{
  BumpStringPool pool;
  std::ignore = pool.AddString("test");
  EXPECT_EQ(pool.GetString(9999), std::string_view{});
}

TEST(BumpStringPool, GetStringWithLengthOutOfBounds)
{
  BumpStringPool pool;
  auto offset = pool.AddString("short");
  EXPECT_EQ(pool.GetString(offset, 9999), std::string_view{});
}

TEST(BumpStringPool, SizeIncludesNullTerminators)
{
  BumpStringPool pool;
  std::ignore = pool.AddString("abc"); // 3 chars + 1 null = 4
  EXPECT_EQ(pool.GetSize(), 4u);

  std::ignore = pool.AddString("de"); // 2 chars + 1 null = 3
  EXPECT_EQ(pool.GetSize(), 7u);
}

TEST(BumpStringPool, Clear)
{
  BumpStringPool pool;
  std::ignore = pool.AddString("one");
  std::ignore = pool.AddString("two");
  EXPECT_FALSE(pool.IsEmpty());

  pool.Clear();
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
}

TEST(BumpStringPool, ClearThenReuse)
{
  BumpStringPool pool;
  std::ignore = pool.AddString("before");
  pool.Clear();

  auto offset = pool.AddString("after");
  EXPECT_NE(offset, BumpStringPool::InvalidOffset);
  EXPECT_EQ(pool.GetString(offset), "after");
}

TEST(BumpStringPool, ReserveDoesNotChangeState)
{
  BumpStringPool pool;
  pool.Reserve(4096);
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
}

TEST(BumpStringPool, UnicodeStrings)
{
  BumpStringPool pool;
  auto jp = pool.AddString("日本語テスト");
  auto emoji = pool.AddString("🎮🕹️");

  EXPECT_EQ(pool.GetString(jp), "日本語テスト");
  EXPECT_EQ(pool.GetString(emoji), "🎮🕹️");
}

TEST(BumpStringPool, SingleCharacterString)
{
  BumpStringPool pool;
  auto offset = pool.AddString("x");
  EXPECT_EQ(pool.GetString(offset), "x");
  EXPECT_EQ(pool.GetSize(), 2u); // 1 char + null
}

TEST(BumpStringPool, VeryLongString)
{
  BumpStringPool pool;
  std::string long_str(10000, 'z');
  auto offset = pool.AddString(long_str);
  EXPECT_EQ(pool.GetString(offset), long_str);
  EXPECT_EQ(pool.GetSize(), 10001u);
}

TEST(BumpStringPool, ManyStrings)
{
  BumpStringPool pool;
  pool.Reserve(2000);

  for (int i = 0; i < 200; i++)
  {
    std::string s = "str_" + std::to_string(i);
    auto offset = pool.AddString(s);
    EXPECT_EQ(pool.GetString(offset), s);
  }
}

TEST(BumpStringPool, SpecialCharacters)
{
  BumpStringPool pool;
  auto tab = pool.AddString("a\tb");
  auto newline = pool.AddString("c\nd");

  EXPECT_EQ(pool.GetString(tab), "a\tb");
  EXPECT_EQ(pool.GetString(newline), "c\nd");
}

// ---- BumpUniqueStringPool ----

TEST(BumpUniqueStringPool, InitiallyEmpty)
{
  BumpUniqueStringPool pool;
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
  EXPECT_EQ(pool.GetCount(), 0u);
}

TEST(BumpUniqueStringPool, AddSingleString)
{
  BumpUniqueStringPool pool;
  auto offset = pool.AddString("hello");
  EXPECT_NE(offset, BumpUniqueStringPool::InvalidOffset);
  EXPECT_EQ(pool.GetCount(), 1u);
  EXPECT_EQ(pool.GetString(offset), "hello");
}

TEST(BumpUniqueStringPool, AddEmptyStringReturnsInvalid)
{
  BumpUniqueStringPool pool;
  auto offset = pool.AddString("");
  EXPECT_EQ(offset, BumpUniqueStringPool::InvalidOffset);
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetCount(), 0u);
}

TEST(BumpUniqueStringPool, AddEmptyStringViewReturnsInvalid)
{
  BumpUniqueStringPool pool;
  auto offset = pool.AddString(std::string_view{});
  EXPECT_EQ(offset, BumpUniqueStringPool::InvalidOffset);
  EXPECT_TRUE(pool.IsEmpty());
}

TEST(BumpUniqueStringPool, DeduplicatesSameString)
{
  BumpUniqueStringPool pool;
  auto offset1 = pool.AddString("duplicate");
  const size_t size_after_first = pool.GetSize();
  const size_t count_after_first = pool.GetCount();

  auto offset2 = pool.AddString("duplicate");
  EXPECT_EQ(offset1, offset2);
  EXPECT_EQ(pool.GetSize(), size_after_first);
  EXPECT_EQ(pool.GetCount(), count_after_first);
}

TEST(BumpUniqueStringPool, DeduplicateMultipleAdds)
{
  BumpUniqueStringPool pool;
  auto offset = pool.AddString("test");
  const size_t size_after = pool.GetSize();

  for (int i = 0; i < 100; i++)
  {
    EXPECT_EQ(pool.AddString("test"), offset);
  }

  EXPECT_EQ(pool.GetSize(), size_after);
  EXPECT_EQ(pool.GetCount(), 1u);
}

TEST(BumpUniqueStringPool, DistinguishesDifferentStrings)
{
  BumpUniqueStringPool pool;
  auto offset1 = pool.AddString("alpha");
  auto offset2 = pool.AddString("beta");
  EXPECT_NE(offset1, offset2);
  EXPECT_EQ(pool.GetCount(), 2u);
  EXPECT_EQ(pool.GetString(offset1), "alpha");
  EXPECT_EQ(pool.GetString(offset2), "beta");
}

TEST(BumpUniqueStringPool, SizeDoesNotGrowOnDuplicate)
{
  BumpUniqueStringPool pool;
  std::ignore = pool.AddString("aaa");
  std::ignore = pool.AddString("bbb");
  std::ignore = pool.AddString("ccc");
  const size_t size_before = pool.GetSize();
  const size_t count_before = pool.GetCount();

  std::ignore = pool.AddString("aaa");
  std::ignore = std::ignore = pool.AddString("bbb");
  std::ignore = pool.AddString("ccc");
  EXPECT_EQ(pool.GetSize(), size_before);
  EXPECT_EQ(pool.GetCount(), count_before);
}

TEST(BumpUniqueStringPool, SizeGrowsOnlyForNewStrings)
{
  BumpUniqueStringPool pool;
  std::ignore = pool.AddString("aaa");
  const size_t size1 = pool.GetSize();

  std::ignore = pool.AddString("aaa"); // duplicate
  EXPECT_EQ(pool.GetSize(), size1);

  std::ignore = pool.AddString("bbb"); // new
  EXPECT_GT(pool.GetSize(), size1);
  const size_t size2 = pool.GetSize();

  std::ignore = pool.AddString("bbb"); // duplicate
  EXPECT_EQ(pool.GetSize(), size2);
}

TEST(BumpUniqueStringPool, GetStringWithOffset)
{
  BumpUniqueStringPool pool;
  auto offset = pool.AddString("world");
  EXPECT_EQ(pool.GetString(offset), "world");
}

TEST(BumpUniqueStringPool, GetStringWithOffsetAndLength)
{
  BumpUniqueStringPool pool;
  auto offset = pool.AddString("hello world");
  EXPECT_EQ(pool.GetString(offset, 5), "hello");
  EXPECT_EQ(pool.GetString(offset, 11), "hello world");
}

TEST(BumpUniqueStringPool, GetStringInvalidOffset)
{
  BumpUniqueStringPool pool;
  EXPECT_EQ(pool.GetString(BumpUniqueStringPool::InvalidOffset), std::string_view{});
}

TEST(BumpUniqueStringPool, GetStringOutOfBounds)
{
  BumpUniqueStringPool pool;
  std::ignore = pool.AddString("test");
  EXPECT_EQ(pool.GetString(9999), std::string_view{});
}

TEST(BumpUniqueStringPool, GetStringWithLengthOutOfBounds)
{
  BumpUniqueStringPool pool;
  auto offset = pool.AddString("short");
  EXPECT_EQ(pool.GetString(offset, 9999), std::string_view{});
}

TEST(BumpUniqueStringPool, Clear)
{
  BumpUniqueStringPool pool;
  std::ignore = pool.AddString("one");
  std::ignore = pool.AddString("two");
  std::ignore = pool.AddString("three");
  EXPECT_FALSE(pool.IsEmpty());
  EXPECT_EQ(pool.GetCount(), 3u);

  pool.Clear();
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
  EXPECT_EQ(pool.GetCount(), 0u);
}

TEST(BumpUniqueStringPool, ClearThenReuse)
{
  BumpUniqueStringPool pool;
  std::ignore = pool.AddString("before");
  pool.Clear();

  auto offset = pool.AddString("after");
  EXPECT_NE(offset, BumpUniqueStringPool::InvalidOffset);
  EXPECT_EQ(pool.GetString(offset), "after");
  EXPECT_EQ(pool.GetCount(), 1u);
}

TEST(BumpUniqueStringPool, ReserveDoesNotChangeState)
{
  BumpUniqueStringPool pool;
  pool.Reserve(100, 4096);
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
  EXPECT_EQ(pool.GetCount(), 0u);
}

TEST(BumpUniqueStringPool, ManyUniqueStrings)
{
  BumpUniqueStringPool pool;
  pool.Reserve(200, 2000);

  for (int i = 0; i < 200; i++)
  {
    std::string s = "string_" + std::to_string(i);
    std::ignore = pool.AddString(s);
  }

  EXPECT_EQ(pool.GetCount(), 200u);

  // Verify all are retrievable and deduplicated.
  for (int i = 0; i < 200; i++)
  {
    std::string s = "string_" + std::to_string(i);
    auto offset = pool.AddString(s);
    EXPECT_EQ(pool.GetString(offset), s);
  }

  // Count should not have grown.
  EXPECT_EQ(pool.GetCount(), 200u);
}

TEST(BumpUniqueStringPool, CaseSensitive)
{
  BumpUniqueStringPool pool;
  auto lower = pool.AddString("hello");
  auto upper = pool.AddString("Hello");
  auto allupper = pool.AddString("HELLO");

  EXPECT_NE(lower, upper);
  EXPECT_NE(upper, allupper);
  EXPECT_NE(lower, allupper);
  EXPECT_EQ(pool.GetCount(), 3u);
}

TEST(BumpUniqueStringPool, SubstringNotDeduplicated)
{
  BumpUniqueStringPool pool;
  auto full = pool.AddString("hello world");
  auto sub = pool.AddString("hello");
  EXPECT_NE(full, sub);
  EXPECT_EQ(pool.GetCount(), 2u);
  EXPECT_EQ(pool.GetString(full), "hello world");
  EXPECT_EQ(pool.GetString(sub), "hello");
}

TEST(BumpUniqueStringPool, PrefixSuffixDistinct)
{
  BumpUniqueStringPool pool;
  auto a = pool.AddString("abc");
  auto b = pool.AddString("abcd");
  auto c = pool.AddString("ab");
  EXPECT_NE(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(b, c);
  EXPECT_EQ(pool.GetCount(), 3u);
}

TEST(BumpUniqueStringPool, UnicodeStrings)
{
  BumpUniqueStringPool pool;
  auto jp = pool.AddString("日本語テスト");
  auto cn = pool.AddString("中文测试");
  auto emoji = pool.AddString("🎮🕹️");

  EXPECT_EQ(pool.GetString(jp), "日本語テスト");
  EXPECT_EQ(pool.GetString(cn), "中文测试");
  EXPECT_EQ(pool.GetString(emoji), "🎮🕹️");
  EXPECT_EQ(pool.GetCount(), 3u);
}

TEST(BumpUniqueStringPool, UnicodeDeduplicated)
{
  BumpUniqueStringPool pool;
  auto offset1 = pool.AddString("日本語");
  const size_t size_after = pool.GetSize();

  auto offset2 = pool.AddString("日本語");
  EXPECT_EQ(offset1, offset2);
  EXPECT_EQ(pool.GetSize(), size_after);
  EXPECT_EQ(pool.GetCount(), 1u);
}

TEST(BumpUniqueStringPool, SpecialCharacters)
{
  BumpUniqueStringPool pool;
  auto tab = pool.AddString("hello\tworld");
  auto newline = pool.AddString("hello\nworld");
  auto null_inside = pool.AddString(std::string_view("a\0b", 3));

  EXPECT_EQ(pool.GetString(tab), "hello\tworld");
  EXPECT_EQ(pool.GetString(newline), "hello\nworld");
  // Embedded null: GetString(offset) stops at null, but GetString(offset, length) returns full view.
  EXPECT_EQ(pool.GetString(null_inside, 3), std::string_view("a\0b", 3));
  EXPECT_EQ(pool.GetCount(), 3u);
}

TEST(BumpUniqueStringPool, SingleCharacterStrings)
{
  BumpUniqueStringPool pool;
  auto a = pool.AddString("a");
  auto b = pool.AddString("b");
  auto a2 = pool.AddString("a");

  EXPECT_NE(a, b);
  EXPECT_EQ(a, a2);
  EXPECT_EQ(pool.GetCount(), 2u);
}

TEST(BumpUniqueStringPool, VeryLongString)
{
  BumpUniqueStringPool pool;
  std::string long_str(10000, 'x');
  auto offset1 = pool.AddString(long_str);
  const size_t size_after = pool.GetSize();

  auto offset2 = pool.AddString(long_str);
  EXPECT_EQ(offset1, offset2);
  EXPECT_EQ(pool.GetSize(), size_after);
  EXPECT_EQ(pool.GetString(offset1), long_str);
}

TEST(BumpUniqueStringPool, InterleaveAddAndLookup)
{
  BumpUniqueStringPool pool;
  auto a = pool.AddString("zebra");
  auto b = pool.AddString("apple");
  auto c = pool.AddString("mango");

  // All original offsets still valid after sorted insertions.
  EXPECT_EQ(pool.GetString(a), "zebra");
  EXPECT_EQ(pool.GetString(b), "apple");
  EXPECT_EQ(pool.GetString(c), "mango");

  // Duplicates return same offsets.
  EXPECT_EQ(pool.AddString("mango"), c);
  EXPECT_EQ(pool.AddString("apple"), b);
  EXPECT_EQ(pool.AddString("zebra"), a);
  EXPECT_EQ(pool.GetCount(), 3u);
}

TEST(BumpUniqueStringPool, CountMatchesUniqueStrings)
{
  BumpUniqueStringPool pool;
  std::ignore = pool.AddString("a");
  std::ignore = pool.AddString("b");
  std::ignore = pool.AddString("c");
  std::ignore = pool.AddString("a");
  std::ignore = pool.AddString("b");
  std::ignore = pool.AddString("d");
  std::ignore = pool.AddString("c");

  EXPECT_EQ(pool.GetCount(), 4u); // a, b, c, d
}

// ---- StringPool ----

TEST(StringPool, InitiallyEmpty)
{
  StringPool pool;
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
  EXPECT_EQ(pool.GetCount(), 0u);
}

TEST(StringPool, AddSingleString)
{
  StringPool pool;
  auto offset = pool.AddString("hello");
  EXPECT_NE(offset, StringPool::InvalidOffset);
  EXPECT_EQ(pool.GetCount(), 1u);
  EXPECT_EQ(pool.GetString(offset), "hello");
}

TEST(StringPool, AddEmptyStringReturnsInvalid)
{
  StringPool pool;
  auto offset = pool.AddString("");
  EXPECT_EQ(offset, StringPool::InvalidOffset);
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetCount(), 0u);
}

TEST(StringPool, AddEmptyStringViewReturnsInvalid)
{
  StringPool pool;
  auto offset = pool.AddString(std::string_view{});
  EXPECT_EQ(offset, StringPool::InvalidOffset);
  EXPECT_TRUE(pool.IsEmpty());
}

TEST(StringPool, DeduplicatesSameString)
{
  StringPool pool;
  auto offset1 = pool.AddString("duplicate");
  const size_t size_after_first = pool.GetSize();
  const size_t count_after_first = pool.GetCount();

  auto offset2 = pool.AddString("duplicate");
  EXPECT_EQ(offset1, offset2);
  EXPECT_EQ(pool.GetSize(), size_after_first);
  EXPECT_EQ(pool.GetCount(), count_after_first);
}

TEST(StringPool, DeduplicateMultipleAdds)
{
  StringPool pool;
  auto offset = pool.AddString("test");
  const size_t size_after = pool.GetSize();

  for (int i = 0; i < 100; i++)
  {
    EXPECT_EQ(pool.AddString("test"), offset);
  }

  EXPECT_EQ(pool.GetSize(), size_after);
  EXPECT_EQ(pool.GetCount(), 1u);
}

TEST(StringPool, DistinguishesDifferentStrings)
{
  StringPool pool;
  auto offset1 = pool.AddString("alpha");
  auto offset2 = pool.AddString("beta");
  EXPECT_NE(offset1, offset2);
  EXPECT_EQ(pool.GetCount(), 2u);
  EXPECT_EQ(pool.GetString(offset1), "alpha");
  EXPECT_EQ(pool.GetString(offset2), "beta");
}

TEST(StringPool, SizeDoesNotGrowOnDuplicate)
{
  StringPool pool;
  std::ignore = pool.AddString("aaa");
  std::ignore = pool.AddString("bbb");
  std::ignore = pool.AddString("ccc");
  const size_t size_before = pool.GetSize();
  const size_t count_before = pool.GetCount();

  std::ignore = pool.AddString("aaa");
  std::ignore = pool.AddString("bbb");
  std::ignore = pool.AddString("ccc");
  EXPECT_EQ(pool.GetSize(), size_before);
  EXPECT_EQ(pool.GetCount(), count_before);
}

TEST(StringPool, SizeGrowsOnlyForNewStrings)
{
  StringPool pool;
  std::ignore = pool.AddString("aaa");
  const size_t size1 = pool.GetSize();

  std::ignore = pool.AddString("aaa"); // duplicate
  EXPECT_EQ(pool.GetSize(), size1);

  std::ignore = pool.AddString("bbb"); // new
  EXPECT_GT(pool.GetSize(), size1);
  const size_t size2 = pool.GetSize();

  std::ignore = pool.AddString("bbb"); // duplicate
  EXPECT_EQ(pool.GetSize(), size2);
}

TEST(StringPool, GetStringWithOffset)
{
  StringPool pool;
  auto offset = pool.AddString("world");
  EXPECT_EQ(pool.GetString(offset), "world");
}

TEST(StringPool, GetStringWithOffsetAndLength)
{
  StringPool pool;
  auto offset = pool.AddString("hello world");
  EXPECT_EQ(pool.GetString(offset, 5), "hello");
  EXPECT_EQ(pool.GetString(offset, 11), "hello world");
}

TEST(StringPool, GetStringOutOfBounds)
{
  StringPool pool;
  std::ignore = pool.AddString("test");
  EXPECT_EQ(pool.GetString(9999), std::string_view{});
}

TEST(StringPool, GetStringWithLengthOutOfBounds)
{
  StringPool pool;
  auto offset = pool.AddString("short");
  EXPECT_EQ(pool.GetString(offset, 9999), std::string_view{});
}

TEST(StringPool, Clear)
{
  StringPool pool;
  std::ignore = pool.AddString("one");
  std::ignore = pool.AddString("two");
  std::ignore = pool.AddString("three");
  EXPECT_FALSE(pool.IsEmpty());
  EXPECT_EQ(pool.GetCount(), 3u);

  pool.Clear();
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
  EXPECT_EQ(pool.GetCount(), 0u);
}

TEST(StringPool, ReserveDoesNotChangeState)
{
  StringPool pool;
  pool.Reserve(4096);
  EXPECT_TRUE(pool.IsEmpty());
  EXPECT_EQ(pool.GetSize(), 0u);
  EXPECT_EQ(pool.GetCount(), 0u);
}

TEST(StringPool, CaseSensitive)
{
  StringPool pool;
  auto lower = pool.AddString("hello");
  auto upper = pool.AddString("Hello");
  auto allupper = pool.AddString("HELLO");

  EXPECT_NE(lower, upper);
  EXPECT_NE(upper, allupper);
  EXPECT_NE(lower, allupper);
  EXPECT_EQ(pool.GetCount(), 3u);
}

TEST(StringPool, SubstringNotDeduplicated)
{
  StringPool pool;
  auto full = pool.AddString("hello world");
  auto sub = pool.AddString("hello");
  EXPECT_NE(full, sub);
  EXPECT_EQ(pool.GetCount(), 2u);
}

TEST(StringPool, UnicodeStrings)
{
  StringPool pool;
  auto jp = pool.AddString("日本語テスト");
  auto cn = pool.AddString("中文测试");
  auto emoji = pool.AddString("🎮🕹️");

  EXPECT_EQ(pool.GetString(jp), "日本語テスト");
  EXPECT_EQ(pool.GetString(cn), "中文测试");
  EXPECT_EQ(pool.GetString(emoji), "🎮🕹️");
  EXPECT_EQ(pool.GetCount(), 3u);
}

TEST(StringPool, UnicodeDeduplicated)
{
  StringPool pool;
  auto offset1 = pool.AddString("日本語");
  const size_t size_after = pool.GetSize();

  auto offset2 = pool.AddString("日本語");
  EXPECT_EQ(offset1, offset2);
  EXPECT_EQ(pool.GetSize(), size_after);
  EXPECT_EQ(pool.GetCount(), 1u);
}

TEST(StringPool, ManyUniqueStrings)
{
  StringPool pool;
  pool.Reserve(2000);

  for (int i = 0; i < 200; i++)
  {
    std::string s = "string_" + std::to_string(i);
    std::ignore = pool.AddString(s);
  }

  EXPECT_EQ(pool.GetCount(), 200u);

  // Verify all are retrievable and deduplicated.
  for (int i = 0; i < 200; i++)
  {
    std::string s = "string_" + std::to_string(i);
    auto offset = pool.AddString(s);
    EXPECT_EQ(pool.GetString(offset), s);
  }

  EXPECT_EQ(pool.GetCount(), 200u);
}

TEST(StringPool, VeryLongString)
{
  StringPool pool;
  std::string long_str(10000, 'x');
  auto offset1 = pool.AddString(long_str);
  const size_t size_after = pool.GetSize();

  auto offset2 = pool.AddString(long_str);
  EXPECT_EQ(offset1, offset2);
  EXPECT_EQ(pool.GetSize(), size_after);
  EXPECT_EQ(pool.GetString(offset1), long_str);
}

TEST(StringPool, GetStringInvalidOffset)
{
  StringPool pool;
  const auto retrieved = pool.GetString(StringPool::InvalidOffset);

  EXPECT_TRUE(retrieved.empty());
}

TEST(StringPool, SpecialCharacters)
{
  StringPool pool;
  const std::string_view special_str = "Hello\nWorld\t!@#$%^&*()";
  const auto offset = pool.AddString(special_str);

  EXPECT_NE(offset, StringPool::InvalidOffset);
  EXPECT_EQ(pool.GetString(offset), special_str);
}

TEST(StringPool, GetCountTracksUniqueStrings)
{
  StringPool pool;
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

TEST(StringPool, ReuseAfterClear)
{
  StringPool pool;
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
