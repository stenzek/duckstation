// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "util/ini_settings_interface.h"

#include "common/small_string.h"

#include <gtest/gtest.h>

// ---- Parsing / Loading ----

TEST(INISettingsInterface, LoadEmptyString)
{
  INISettingsInterface si;
  EXPECT_TRUE(si.LoadFromString(""));
  EXPECT_TRUE(si.IsEmpty());
}

TEST(INISettingsInterface, LoadHashComments)
{
  INISettingsInterface si;
  si.LoadFromString("# This is a comment\n"
                    "[Section]\n"
                    "key = value\n"
                    "# Another comment\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, LoadSemicolonComments)
{
  INISettingsInterface si;
  si.LoadFromString("; Semicolon comment\n"
                    "[Section]\n"
                    "key = hello\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "key", &val));
  EXPECT_EQ(val, "hello");
}

TEST(INISettingsInterface, InlineCommentHash)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "key = value # inline comment\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, InlineCommentSemicolon)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "key = value ; inline comment\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, QuotedValuePreservesCommentChars)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "key = \"value ; with # chars\"\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value ; with # chars");
}

TEST(INISettingsInterface, WhitespaceTrimming)
{
  INISettingsInterface si;
  si.LoadFromString("  [  Section  ]  \n"
                    "  key  =  value  \n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, MultipleSections)
{
  INISettingsInterface si;
  si.LoadFromString("[First]\n"
                    "a = 1\n"
                    "[Second]\n"
                    "b = 2\n"
                    "[Third]\n"
                    "c = 3\n");
  EXPECT_EQ(si.GetIntValue("First", "a", 0), 1);
  EXPECT_EQ(si.GetIntValue("Second", "b", 0), 2);
  EXPECT_EQ(si.GetIntValue("Third", "c", 0), 3);
}

TEST(INISettingsInterface, MultiValueSameKey)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "color = red\n"
                    "color = green\n"
                    "color = blue\n");
  auto list = si.GetStringList("Section", "color");
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list[0], "red");
  EXPECT_EQ(list[1], "green");
  EXPECT_EQ(list[2], "blue");
}

TEST(INISettingsInterface, CaseInsensitiveSectionLookup)
{
  INISettingsInterface si;
  si.LoadFromString("[MySection]\n"
                    "key = value\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("mysection", "key", &val));
  EXPECT_EQ(val, "value");
  EXPECT_TRUE(si.GetStringValue("MYSECTION", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, CaseInsensitiveKeyLookup)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "MyKey = value\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "mykey", &val));
  EXPECT_EQ(val, "value");
  EXPECT_TRUE(si.GetStringValue("Section", "MYKEY", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, LineWithoutEquals)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "not_a_key_value\n"
                    "key = value\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
  EXPECT_FALSE(si.ContainsValue("Section", "not_a_key_value"));
}

TEST(INISettingsInterface, EmptyKeysSkipped)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    " = value\n"
                    "key = value\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "key", &val));
  auto kvlist = si.GetKeyValueList("Section");
  EXPECT_EQ(kvlist.size(), 1u);
}

TEST(INISettingsInterface, EmptyValue)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "key =\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "key", &val));
  EXPECT_EQ(val, "");
}

TEST(INISettingsInterface, NoTrailingNewline)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "key = value");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, WindowsLineEndings)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\r\n"
                    "key = value\r\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
}

// ---- Unicode ----

TEST(INISettingsInterface, UnicodeSectionName)
{
  INISettingsInterface si;
  si.LoadFromString("[ゲーム設定]\n"
                    "key = value\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("ゲーム設定", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, UnicodeKeyAndValue)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "名前 = ダックステーション\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Section", "名前", &val));
  EXPECT_EQ(val, "ダックステーション");
}

TEST(INISettingsInterface, UnicodeRoundTrip)
{
  INISettingsInterface si;
  si.LoadFromString("");
  si.SetStringValue("Einstellungen", "Sprache", "Deutsch üöä");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Einstellungen", "Sprache", &val));
  EXPECT_EQ(val, "Deutsch üöä");

  const std::string output = si.SaveToString();
  INISettingsInterface si2;
  si2.LoadFromString(output);
  std::string val2;
  EXPECT_TRUE(si2.GetStringValue("Einstellungen", "Sprache", &val2));
  EXPECT_EQ(val2, "Deutsch üöä");
}

TEST(INISettingsInterface, EmojiValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nicon = 🎮\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("S", "icon", &val));
  EXPECT_EQ(val, "🎮");
}

TEST(INISettingsInterface, MultipleEmojis)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nstatus = 🎮🕹️🏆✅\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("S", "status", &val));
  EXPECT_EQ(val, "🎮🕹️🏆✅");
}

TEST(INISettingsInterface, EmojiRoundTrip)
{
  INISettingsInterface si;
  si.LoadFromString("");
  si.SetStringValue("S", "face", "😀🤖👾");
  const std::string output = si.SaveToString();

  INISettingsInterface si2;
  si2.LoadFromString(output);
  std::string val;
  EXPECT_TRUE(si2.GetStringValue("S", "face", &val));
  EXPECT_EQ(val, "😀🤖👾");
}

TEST(INISettingsInterface, ChineseCharacters)
{
  INISettingsInterface si;
  si.LoadFromString("[设置]\n"
                    "语言 = 中文\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("设置", "语言", &val));
  EXPECT_EQ(val, "中文");
}

TEST(INISettingsInterface, MixedAsciiAndUnicode)
{
  INISettingsInterface si;
  si.LoadFromString("[Display]\n"
                    "title = DuckStation — ダックステーション\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("Display", "title", &val));
  EXPECT_EQ(val, "DuckStation — ダックステーション");
}

// ---- Save / Serialize ----

TEST(INISettingsInterface, SaveToStringBasic)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "key = value\n");
  const std::string output = si.SaveToString();
  EXPECT_EQ(output, "[Section]\nkey = value\n");
}

TEST(INISettingsInterface, SaveToStringQuotedValue)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "key = \"value;with#special\"\n");
  const std::string output = si.SaveToString();
  EXPECT_EQ(output, "[Section]\nkey = \"value;with#special\"\n");
}

TEST(INISettingsInterface, SaveToStringMultipleSections)
{
  INISettingsInterface si;
  si.LoadFromString("[B]\n"
                    "x = 1\n"
                    "[A]\n"
                    "y = 2\n");
  const std::string output = si.SaveToString();
  // Sections sorted alphabetically (case-insensitive).
  EXPECT_EQ(output, "[A]\ny = 2\n\n[B]\nx = 1\n");
}

TEST(INISettingsInterface, RoundTrip)
{
  const std::string input = "[Alpha]\n"
                            "num = 42\n"
                            "str = hello\n"
                            "\n"
                            "[Beta]\n"
                            "flag = true\n";
  INISettingsInterface si;
  si.LoadFromString(input);
  const std::string output = si.SaveToString();
  EXPECT_EQ(output, input);
}

TEST(INISettingsInterface, RoundTripMultiValue)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "item = a\n"
                    "item = b\n"
                    "item = c\n");
  const std::string output = si.SaveToString();
  EXPECT_EQ(output, "[Section]\nitem = a\nitem = b\nitem = c\n");
}

// ---- Data type getters ----

TEST(INISettingsInterface, GetIntValuePositive)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = 42\n");
  s32 val = 0;
  EXPECT_TRUE(si.GetIntValue("S", "key", &val));
  EXPECT_EQ(val, 42);
}

TEST(INISettingsInterface, GetIntValueNegative)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = -100\n");
  s32 val = 0;
  EXPECT_TRUE(si.GetIntValue("S", "key", &val));
  EXPECT_EQ(val, -100);
}

TEST(INISettingsInterface, GetIntValueInvalid)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = notanumber\n");
  s32 val = 0;
  EXPECT_FALSE(si.GetIntValue("S", "key", &val));
}

TEST(INISettingsInterface, GetIntValueMissing)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\n");
  s32 val = 0;
  EXPECT_FALSE(si.GetIntValue("S", "nokey", &val));
}

TEST(INISettingsInterface, GetIntValueDefault)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\n");
  EXPECT_EQ(si.GetIntValue("S", "nokey", 99), 99);
}

TEST(INISettingsInterface, GetUIntValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = 4294967295\n");
  u32 val = 0;
  EXPECT_TRUE(si.GetUIntValue("S", "key", &val));
  EXPECT_EQ(val, 4294967295u);
}

TEST(INISettingsInterface, GetFloatValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = 3.14\n");
  float val = 0.0f;
  EXPECT_TRUE(si.GetFloatValue("S", "key", &val));
  EXPECT_FLOAT_EQ(val, 3.14f);
}

TEST(INISettingsInterface, GetDoubleValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = 2.718281828\n");
  double val = 0.0;
  EXPECT_TRUE(si.GetDoubleValue("S", "key", &val));
  EXPECT_DOUBLE_EQ(val, 2.718281828);
}

TEST(INISettingsInterface, GetBoolValueTrue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = true\n");
  bool val = false;
  EXPECT_TRUE(si.GetBoolValue("S", "key", &val));
  EXPECT_TRUE(val);
}

TEST(INISettingsInterface, GetBoolValueFalse)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = false\n");
  bool val = true;
  EXPECT_TRUE(si.GetBoolValue("S", "key", &val));
  EXPECT_FALSE(val);
}

TEST(INISettingsInterface, GetBoolValueInvalid)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = maybe\n");
  bool val = false;
  EXPECT_FALSE(si.GetBoolValue("S", "key", &val));
}

TEST(INISettingsInterface, GetStringValueStdString)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = hello world\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("S", "key", &val));
  EXPECT_EQ(val, "hello world");
}

TEST(INISettingsInterface, GetStringValueSmallString)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = hello\n");
  SmallString val;
  EXPECT_TRUE(si.GetStringValue("S", "key", &val));
  EXPECT_EQ(val.view(), "hello");
}

TEST(INISettingsInterface, GetStringValueMissingSection)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = value\n");
  std::string val;
  EXPECT_FALSE(si.GetStringValue("Missing", "key", &val));
}

// ---- Data type setters ----

TEST(INISettingsInterface, SetIntValue)
{
  INISettingsInterface si;
  si.LoadFromString("");
  si.SetIntValue("S", "key", 42);
  EXPECT_EQ(si.GetIntValue("S", "key", 0), 42);
  EXPECT_TRUE(si.IsDirty());
}

TEST(INISettingsInterface, SetUIntValue)
{
  INISettingsInterface si;
  si.LoadFromString("");
  si.SetUIntValue("S", "key", 100u);
  EXPECT_EQ(si.GetUIntValue("S", "key", 0u), 100u);
}

TEST(INISettingsInterface, SetFloatValue)
{
  INISettingsInterface si;
  si.LoadFromString("");
  si.SetFloatValue("S", "key", 1.5f);
  EXPECT_FLOAT_EQ(si.GetFloatValue("S", "key", 0.0f), 1.5f);
}

TEST(INISettingsInterface, SetDoubleValue)
{
  INISettingsInterface si;
  si.LoadFromString("");
  si.SetDoubleValue("S", "key", 2.5);
  EXPECT_DOUBLE_EQ(si.GetDoubleValue("S", "key", 0.0), 2.5);
}

TEST(INISettingsInterface, SetBoolValue)
{
  INISettingsInterface si;
  si.LoadFromString("");
  si.SetBoolValue("S", "key", true);
  EXPECT_TRUE(si.GetBoolValue("S", "key", false));
  si.SetBoolValue("S", "key", false);
  EXPECT_FALSE(si.GetBoolValue("S", "key", true));
}

TEST(INISettingsInterface, SetStringValueNewKey)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\n");
  si.SetStringValue("S", "newkey", "newvalue");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("S", "newkey", &val));
  EXPECT_EQ(val, "newvalue");
  EXPECT_TRUE(si.IsDirty());
}

TEST(INISettingsInterface, SetStringValueUpdate)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = old\n");
  si.SetStringValue("S", "key", "new");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("S", "key", &val));
  EXPECT_EQ(val, "new");
}

TEST(INISettingsInterface, SetStringValueNoOpSameValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = same\n");
  EXPECT_FALSE(si.IsDirty());
  si.SetStringValue("S", "key", "same");
  EXPECT_FALSE(si.IsDirty());
}

TEST(INISettingsInterface, SetStringValueCreatesSection)
{
  INISettingsInterface si;
  si.LoadFromString("");
  si.SetStringValue("NewSection", "key", "value");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("NewSection", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, SetStringValueCollapsesMultiValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\n"
                    "key = a\n"
                    "key = b\n"
                    "key = c\n");
  si.SetStringValue("S", "key", "single");
  auto list = si.GetStringList("S", "key");
  ASSERT_EQ(list.size(), 1u);
  EXPECT_EQ(list[0], "single");
}

// ---- Contains / Delete / Clear / Remove ----

TEST(INISettingsInterface, ContainsValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = value\n");
  EXPECT_TRUE(si.ContainsValue("S", "key"));
  EXPECT_FALSE(si.ContainsValue("S", "missing"));
  EXPECT_FALSE(si.ContainsValue("Missing", "key"));
}

TEST(INISettingsInterface, DeleteValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = value\nother = keep\n");
  si.DeleteValue("S", "key");
  EXPECT_FALSE(si.ContainsValue("S", "key"));
  EXPECT_TRUE(si.ContainsValue("S", "other"));
  EXPECT_TRUE(si.IsDirty());
}

TEST(INISettingsInterface, DeleteValueMultiKey)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = a\nkey = b\nkey = c\n");
  si.DeleteValue("S", "key");
  EXPECT_FALSE(si.ContainsValue("S", "key"));
  auto list = si.GetStringList("S", "key");
  EXPECT_TRUE(list.empty());
}

TEST(INISettingsInterface, DeleteValueNonExistent)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = value\n");
  si.DeleteValue("S", "missing");
  EXPECT_FALSE(si.IsDirty());
}

TEST(INISettingsInterface, ClearSection)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\na = 1\nb = 2\n");
  si.ClearSection("S");
  EXPECT_FALSE(si.ContainsValue("S", "a"));
  EXPECT_FALSE(si.ContainsValue("S", "b"));
  auto kvlist = si.GetKeyValueList("S");
  EXPECT_TRUE(kvlist.empty());
}

TEST(INISettingsInterface, ClearSectionCreatesIfMissing)
{
  INISettingsInterface si;
  si.LoadFromString("");
  si.ClearSection("NewSection");
  EXPECT_TRUE(si.IsDirty());
  auto kvlist = si.GetKeyValueList("NewSection");
  EXPECT_TRUE(kvlist.empty());
}

TEST(INISettingsInterface, RemoveSection)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\na = 1\n[T]\nb = 2\n");
  si.RemoveSection("S");
  EXPECT_FALSE(si.ContainsValue("S", "a"));
  EXPECT_TRUE(si.ContainsValue("T", "b"));
  EXPECT_TRUE(si.IsDirty());
}

TEST(INISettingsInterface, RemoveSectionNonExistent)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\na = 1\n");
  si.RemoveSection("Missing");
  EXPECT_FALSE(si.IsDirty());
}

TEST(INISettingsInterface, RemoveEmptySections)
{
  INISettingsInterface si;
  si.LoadFromString("[Empty]\n[HasKeys]\nkey = value\n");
  si.RemoveEmptySections();
  EXPECT_TRUE(si.ContainsValue("HasKeys", "key"));
  auto kvlist = si.GetKeyValueList("Empty");
  EXPECT_TRUE(kvlist.empty());
}

TEST(INISettingsInterface, IsEmptyAndClear)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = value\n");
  EXPECT_FALSE(si.IsEmpty());
  si.Clear();
  EXPECT_TRUE(si.IsEmpty());
}

// ---- String list operations ----

TEST(INISettingsInterface, GetStringListEmpty)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\n");
  auto list = si.GetStringList("S", "key");
  EXPECT_TRUE(list.empty());
}

TEST(INISettingsInterface, SetStringList)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\n");
  si.SetStringList("S", "key", {"x", "y", "z"});
  auto list = si.GetStringList("S", "key");
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list[0], "x");
  EXPECT_EQ(list[1], "y");
  EXPECT_EQ(list[2], "z");
}

TEST(INISettingsInterface, SetStringListReplacesExisting)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = old1\nkey = old2\n");
  si.SetStringList("S", "key", {"new1"});
  auto list = si.GetStringList("S", "key");
  ASSERT_EQ(list.size(), 1u);
  EXPECT_EQ(list[0], "new1");
}

TEST(INISettingsInterface, AddToStringList)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = a\n");
  EXPECT_TRUE(si.AddToStringList("S", "key", "b"));
  auto list = si.GetStringList("S", "key");
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list[0], "a");
  EXPECT_EQ(list[1], "b");
}

TEST(INISettingsInterface, AddToStringListDuplicate)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = a\n");
  EXPECT_FALSE(si.AddToStringList("S", "key", "a"));
  auto list = si.GetStringList("S", "key");
  EXPECT_EQ(list.size(), 1u);
}

TEST(INISettingsInterface, RemoveFromStringList)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = a\nkey = b\nkey = c\n");
  EXPECT_TRUE(si.RemoveFromStringList("S", "key", "b"));
  auto list = si.GetStringList("S", "key");
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list[0], "a");
  EXPECT_EQ(list[1], "c");
}

TEST(INISettingsInterface, RemoveFromStringListNotFound)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = a\n");
  EXPECT_FALSE(si.RemoveFromStringList("S", "key", "b"));
}

// ---- Key-value list operations ----

TEST(INISettingsInterface, GetKeyValueList)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nalpha = 1\nbeta = 2\n");
  auto kvlist = si.GetKeyValueList("S");
  ASSERT_EQ(kvlist.size(), 2u);
  EXPECT_EQ(kvlist[0].first, "alpha");
  EXPECT_EQ(kvlist[0].second, "1");
  EXPECT_EQ(kvlist[1].first, "beta");
  EXPECT_EQ(kvlist[1].second, "2");
}

TEST(INISettingsInterface, GetKeyValueListMissingSection)
{
  INISettingsInterface si;
  si.LoadFromString("");
  auto kvlist = si.GetKeyValueList("Missing");
  EXPECT_TRUE(kvlist.empty());
}

TEST(INISettingsInterface, SetKeyValueList)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nold = data\n");
  si.SetKeyValueList("S", {{"newA", "1"}, {"newB", "2"}});
  auto kvlist = si.GetKeyValueList("S");
  ASSERT_EQ(kvlist.size(), 2u);
  EXPECT_EQ(kvlist[0].first, "newA");
  EXPECT_EQ(kvlist[0].second, "1");
  EXPECT_EQ(kvlist[1].first, "newB");
  EXPECT_EQ(kvlist[1].second, "2");
  EXPECT_FALSE(si.ContainsValue("S", "old"));
}

// ---- CompactStrings ----

TEST(INISettingsInterface, CompactStrings)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = original\n");

  si.SetStringValue("S", "key", "updated1");
  si.SetStringValue("S", "key", "updated2");
  si.SetStringValue("S", "key", "final");

  si.CompactStrings();

  std::string val;
  EXPECT_TRUE(si.GetStringValue("S", "key", &val));
  EXPECT_EQ(val, "final");

  const std::string output = si.SaveToString();
  EXPECT_EQ(output, "[S]\nkey = final\n");
}

TEST(INISettingsInterface, CompactStringsMultipleSections)
{
  INISettingsInterface si;
  si.LoadFromString("[A]\na = 1\n\n[B]\nb = 2\n");
  si.SetStringValue("A", "a", "10");
  si.SetStringValue("B", "b", "20");
  si.CompactStrings();

  EXPECT_EQ(si.GetIntValue("A", "a", 0), 10);
  EXPECT_EQ(si.GetIntValue("B", "b", 0), 20);
}

// ---- Sorted output order ----

TEST(INISettingsInterface, SortedSectionOutput)
{
  INISettingsInterface si;
  si.LoadFromString("");
  si.SetStringValue("Zebra", "key", "z");
  si.SetStringValue("Alpha", "key", "a");
  si.SetStringValue("Middle", "key", "m");

  const std::string output = si.SaveToString();
  EXPECT_EQ(output,
            "[Alpha]\nkey = a\n\n"
            "[Middle]\nkey = m\n\n"
            "[Zebra]\nkey = z\n");
}

TEST(INISettingsInterface, SortedKeyOutput)
{
  INISettingsInterface si;
  si.LoadFromString("");
  si.SetStringValue("S", "z_key", "3");
  si.SetStringValue("S", "a_key", "1");
  si.SetStringValue("S", "m_key", "2");

  const std::string output = si.SaveToString();
  EXPECT_EQ(output,
            "[S]\na_key = 1\nm_key = 2\nz_key = 3\n");
}

// ---- Edge cases ----

TEST(INISettingsInterface, ValueWithEqualsSign)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = a=b=c\n");
  std::string val;
  EXPECT_TRUE(si.GetStringValue("S", "key", &val));
  EXPECT_EQ(val, "a=b=c");
}

TEST(INISettingsInterface, SectionWithDuplicateNames)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\na = 1\n[S]\nb = 2\n");
  EXPECT_EQ(si.GetIntValue("S", "a", 0), 1);
  EXPECT_EQ(si.GetIntValue("S", "b", 0), 2);
}

TEST(INISettingsInterface, SetPathDirtyFlag)
{
  INISettingsInterface si;
  si.LoadFromString("");
  EXPECT_FALSE(si.IsDirty());
  si.SetPath("/tmp/new_path.ini");
  EXPECT_TRUE(si.IsDirty());
}
