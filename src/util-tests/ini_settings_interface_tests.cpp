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
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, LoadSemicolonComments)
{
  INISettingsInterface si;
  si.LoadFromString("; Semicolon comment\n"
                    "[Section]\n"
                    "key = hello\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "key", &val));
  EXPECT_EQ(val, "hello");
}

TEST(INISettingsInterface, InlineCommentHash)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "key = value # inline comment\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, InlineCommentSemicolon)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "key = value ; inline comment\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, QuotedValuePreservesCommentChars)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "key = \"value ; with # chars\"\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value ; with # chars");
}

TEST(INISettingsInterface, WhitespaceTrimming)
{
  INISettingsInterface si;
  si.LoadFromString("  [  Section  ]  \n"
                    "  key  =  value  \n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "key", &val));
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
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("MySection", "key", &val));
  EXPECT_EQ(val, "value");
  val = {};
  EXPECT_FALSE(si.FindStringValue("mysection", "key", &val));
  EXPECT_TRUE(val.empty());
  val = {};
  EXPECT_FALSE(si.FindStringValue("MYSECTION", "key", &val));
  EXPECT_TRUE(val.empty());
}

TEST(INISettingsInterface, CaseInsensitiveKeyLookup)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "MyKey = value\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "MyKey", &val));
  EXPECT_EQ(val, "value");
  val = {};
  EXPECT_FALSE(si.FindStringValue("Section", "mykey", &val));
  EXPECT_TRUE(val.empty());
  val = {};
  EXPECT_FALSE(si.FindStringValue("Section", "MYKEY", &val));
  EXPECT_TRUE(val.empty());
}

TEST(INISettingsInterface, LineWithoutEquals)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "not_a_key_value\n"
                    "key = value\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
  EXPECT_FALSE(si.ContainsValue("Section", "not_a_key_value"));
}

TEST(INISettingsInterface, EmptyKeysSkipped)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    " = value\n"
                    "key = value\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "key", &val));
  auto kvlist = si.GetKeyValueList("Section");
  EXPECT_EQ(kvlist.size(), 1u);
}

TEST(INISettingsInterface, EmptyValue)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "key =\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "key", &val));
  EXPECT_EQ(val, "");
}

TEST(INISettingsInterface, NoTrailingNewline)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "key = value");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, WindowsLineEndings)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\r\n"
                    "key = value\r\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "key", &val));
  EXPECT_EQ(val, "value");
}

// ---- Unicode ----

TEST(INISettingsInterface, UnicodeSectionName)
{
  INISettingsInterface si;
  si.LoadFromString("[ゲーム設定]\n"
                    "key = value\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("ゲーム設定", "key", &val));
  EXPECT_EQ(val, "value");
}

TEST(INISettingsInterface, UnicodeKeyAndValue)
{
  INISettingsInterface si;
  si.LoadFromString("[Section]\n"
                    "名前 = ダックステーション\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Section", "名前", &val));
  EXPECT_EQ(val, "ダックステーション");
}

TEST(INISettingsInterface, UnicodeRoundTrip)
{
  INISettingsInterface si;
  si.LoadFromString("");
  si.SetStringValue("Einstellungen", "Sprache", "Deutsch üöä");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Einstellungen", "Sprache", &val));
  EXPECT_EQ(val, "Deutsch üöä");

  const std::string output = si.SaveToString();
  INISettingsInterface si2;
  si2.LoadFromString(output);
  std::string_view val2;
  EXPECT_TRUE(si2.FindStringValue("Einstellungen", "Sprache", &val2));
  EXPECT_EQ(val2, "Deutsch üöä");
}

TEST(INISettingsInterface, EmojiValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nicon = 🎮\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("S", "icon", &val));
  EXPECT_EQ(val, "🎮");
}

TEST(INISettingsInterface, MultipleEmojis)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nstatus = 🎮🕹️🏆✅\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("S", "status", &val));
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
  std::string_view val;
  EXPECT_TRUE(si2.FindStringValue("S", "face", &val));
  EXPECT_EQ(val, "😀🤖👾");
}

TEST(INISettingsInterface, ChineseCharacters)
{
  INISettingsInterface si;
  si.LoadFromString("[设置]\n"
                    "语言 = 中文\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("设置", "语言", &val));
  EXPECT_EQ(val, "中文");
}

TEST(INISettingsInterface, MixedAsciiAndUnicode)
{
  INISettingsInterface si;
  si.LoadFromString("[Display]\n"
                    "title = DuckStation — ダックステーション\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("Display", "title", &val));
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
  EXPECT_TRUE(si.FindIntValue("S", "key", &val));
  EXPECT_EQ(val, 42);
}

TEST(INISettingsInterface, GetIntValueNegative)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = -100\n");
  s32 val = 0;
  EXPECT_TRUE(si.FindIntValue("S", "key", &val));
  EXPECT_EQ(val, -100);
}

TEST(INISettingsInterface, GetIntValueInvalid)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = notanumber\n");
  s32 val = 0;
  EXPECT_FALSE(si.FindIntValue("S", "key", &val));
}

TEST(INISettingsInterface, GetIntValueMissing)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\n");
  s32 val = 0;
  EXPECT_FALSE(si.FindIntValue("S", "nokey", &val));
}

TEST(INISettingsInterface, GetIntValueDefault)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\n");
  EXPECT_EQ(si.GetIntValue("S", "nokey", 99), 99);
}

TEST(INISettingsInterface, FindUIntValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = 4294967295\n");
  u32 val = 0;
  EXPECT_TRUE(si.FindUIntValue("S", "key", &val));
  EXPECT_EQ(val, 4294967295u);
}

TEST(INISettingsInterface, FindFloatValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = 3.14\n");
  float val = 0.0f;
  EXPECT_TRUE(si.FindFloatValue("S", "key", &val));
  EXPECT_FLOAT_EQ(val, 3.14f);
}

TEST(INISettingsInterface, FindDoubleValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = 2.718281828\n");
  double val = 0.0;
  EXPECT_TRUE(si.FindDoubleValue("S", "key", &val));
  EXPECT_DOUBLE_EQ(val, 2.718281828);
}

TEST(INISettingsInterface, FindBoolValueTrue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = true\n");
  bool val = false;
  EXPECT_TRUE(si.FindBoolValue("S", "key", &val));
  EXPECT_TRUE(val);
}

TEST(INISettingsInterface, FindBoolValueFalse)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = false\n");
  bool val = true;
  EXPECT_TRUE(si.FindBoolValue("S", "key", &val));
  EXPECT_FALSE(val);
}

TEST(INISettingsInterface, FindBoolValueInvalid)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = maybe\n");
  bool val = false;
  EXPECT_FALSE(si.FindBoolValue("S", "key", &val));
}

TEST(INISettingsInterface, FindStringValue)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = hello world\n");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("S", "key", &val));
  EXPECT_EQ(val, "hello world");
}

TEST(INISettingsInterface, FindStringValueMissingSection)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = value\n");
  std::string_view val;
  EXPECT_FALSE(si.FindStringValue("Missing", "key", &val));
  EXPECT_TRUE(val.empty());
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
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("S", "newkey", &val));
  EXPECT_EQ(val, "newvalue");
  EXPECT_TRUE(si.IsDirty());
}

TEST(INISettingsInterface, SetStringValueUpdate)
{
  INISettingsInterface si;
  si.LoadFromString("[S]\nkey = old\n");
  si.SetStringValue("S", "key", "new");
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("S", "key", &val));
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
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("NewSection", "key", &val));
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

  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("S", "key", &val));
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
  std::string_view val;
  EXPECT_TRUE(si.FindStringValue("S", "key", &val));
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

// ---- Ordered Save ----

TEST(INISettingsInterface, OrderedSaveEmptyOrder)
{
  INISettingsInterface si;
  si.LoadFromString("[B]\nb = 2\n\n[A]\na = 1\n");

  const std::string without_order = si.SaveToString();
  const std::string with_empty_order = si.SaveToString({});
  EXPECT_EQ(without_order, with_empty_order);
}

TEST(INISettingsInterface, OrderedSaveBasic)
{
  INISettingsInterface si;
  si.LoadFromString("[Alpha]\na = 1\n\n[Beta]\nb = 2\n\n[Gamma]\ng = 3\n");

  // Request Gamma first, then Alpha.
  const char* const order[] = {"Gamma", "Alpha"};
  const std::string output = si.SaveToString(order);
  EXPECT_EQ(output,
            "[Gamma]\ng = 3\n\n"
            "[Alpha]\na = 1\n\n"
            "[Beta]\nb = 2\n");
}

TEST(INISettingsInterface, OrderedSaveAllSections)
{
  INISettingsInterface si;
  si.LoadFromString("[A]\na = 1\n\n[B]\nb = 2\n\n[C]\nc = 3\n");

  // Reverse the natural alphabetical order.
  const char* const order[] = {"C", "B", "A"};
  const std::string output = si.SaveToString(order);
  EXPECT_EQ(output,
            "[C]\nc = 3\n\n"
            "[B]\nb = 2\n\n"
            "[A]\na = 1\n");
}

TEST(INISettingsInterface, OrderedSaveNonExistentSections)
{
  INISettingsInterface si;
  si.LoadFromString("[A]\na = 1\n\n[B]\nb = 2\n");

  // "Missing" doesn't exist; should be silently skipped.
  const char* const order[] = {"Missing", "B"};
  const std::string output = si.SaveToString(order);
  EXPECT_EQ(output,
            "[B]\nb = 2\n\n"
            "[A]\na = 1\n");
}

TEST(INISettingsInterface, OrderedSaveRemainingInAlphabeticalOrder)
{
  INISettingsInterface si;
  si.LoadFromString("[Delta]\nd = 4\n\n[Alpha]\na = 1\n\n[Charlie]\nc = 3\n\n[Bravo]\nb = 2\n");

  // Only specify Charlie; rest should come after in alphabetical order.
  const char* const order[] = {"Charlie"};
  const std::string output = si.SaveToString(order);
  EXPECT_EQ(output,
            "[Charlie]\nc = 3\n\n"
            "[Alpha]\na = 1\n\n"
            "[Bravo]\nb = 2\n\n"
            "[Delta]\nd = 4\n");
}

TEST(INISettingsInterface, OrderedSaveSingleSection)
{
  INISettingsInterface si;
  si.LoadFromString("[Only]\nkey = val\n");

  const char* const order[] = {"Only"};
  const std::string output = si.SaveToString(order);
  EXPECT_EQ(output, "[Only]\nkey = val\n");
}

TEST(INISettingsInterface, OrderedSaveEmptyINI)
{
  INISettingsInterface si;
  si.LoadFromString("");

  const char* const order[] = {"A", "B"};
  const std::string output = si.SaveToString(order);
  EXPECT_TRUE(output.empty());
}

TEST(INISettingsInterface, OrderedSaveDuplicateOrderEntries)
{
  INISettingsInterface si;
  si.LoadFromString("[A]\na = 1\n\n[B]\nb = 2\n");

  // "A" appears twice; section A should only be written once.
  const char* const order[] = {"A", "B", "A"};
  const std::string output = si.SaveToString(order);
  EXPECT_EQ(output,
            "[A]\na = 1\n\n"
            "[B]\nb = 2\n");
}

TEST(INISettingsInterface, OrderedSaveContentPreserved)
{
  INISettingsInterface si;
  si.LoadFromString("[Z]\n"
                    "plain = hello\n"
                    "quoted = \"value ; with # chars\"\n\n"
                    "[A]\n"
                    "num = 42\n");

  const char* const order[] = {"Z"};
  const std::string output = si.SaveToString(order);

  // Z comes first (as ordered), A follows. Quoting is preserved on save.
  EXPECT_EQ(output,
            "[Z]\nplain = hello\nquoted = \"value ; with # chars\"\n\n"
            "[A]\nnum = 42\n");
}

TEST(INISettingsInterface, OrderedSavePrefixMatching)
{
  INISettingsInterface si;
  si.LoadFromString("[Other]\no = 0\n\n"
                    "[Pad]\np = 1\n\n"
                    "[Pad/1]\np1 = 2\n\n"
                    "[Pad/2]\np2 = 3\n");

  // "Pad" should match "Pad" (exact) and "Pad/1", "Pad/2" (prefix with /).
  const char* const order[] = {"Pad"};
  const std::string output = si.SaveToString(order);
  EXPECT_EQ(output,
            "[Pad]\np = 1\n\n"
            "[Pad/1]\np1 = 2\n\n"
            "[Pad/2]\np2 = 3\n\n"
            "[Other]\no = 0\n");
}

TEST(INISettingsInterface, OrderedSavePrefixBoundary)
{
  INISettingsInterface si;
  si.LoadFromString("[Pad]\np = 1\n\n"
                    "[Pad/1]\np1 = 2\n\n"
                    "[Pad2]\np2 = 3\n\n"
                    "[Padding]\npd = 4\n");

  // "Pad" should match "Pad" and "Pad/1", but NOT "Pad2" or "Padding".
  const char* const order[] = {"Pad"};
  const std::string output = si.SaveToString(order);
  EXPECT_EQ(output,
            "[Pad]\np = 1\n\n"
            "[Pad/1]\np1 = 2\n\n"
            "[Pad2]\np2 = 3\n\n"
            "[Padding]\npd = 4\n");
}

TEST(INISettingsInterface, OrderedSavePrefixGroupsPreserveOrder)
{
  INISettingsInterface si;
  si.LoadFromString("[Pad/3]\np3 = 3\n\n"
                    "[Pad/1]\np1 = 1\n\n"
                    "[Pad/2]\np2 = 2\n\n"
                    "[Other]\no = 0\n");

  // Sub-sections should appear in their natural (alphabetical) order within the prefix group.
  const char* const order[] = {"Pad"};
  const std::string output = si.SaveToString(order);
  EXPECT_EQ(output,
            "[Pad/1]\np1 = 1\n\n"
            "[Pad/2]\np2 = 2\n\n"
            "[Pad/3]\np3 = 3\n\n"
            "[Other]\no = 0\n");
}

TEST(INISettingsInterface, OrderedSaveMultiplePrefixes)
{
  INISettingsInterface si;
  si.LoadFromString("[Hotkey]\nh = 0\n\n"
                    "[Hotkey/1]\nh1 = 1\n\n"
                    "[Other]\no = 0\n\n"
                    "[Pad]\np = 0\n\n"
                    "[Pad/1]\np1 = 1\n\n"
                    "[Pad/2]\np2 = 2\n");

  // Pad group first, then Hotkey group, then remaining.
  const char* const order[] = {"Pad", "Hotkey"};
  const std::string output = si.SaveToString(order);
  EXPECT_EQ(output,
            "[Pad]\np = 0\n\n"
            "[Pad/1]\np1 = 1\n\n"
            "[Pad/2]\np2 = 2\n\n"
            "[Hotkey]\nh = 0\n\n"
            "[Hotkey/1]\nh1 = 1\n\n"
            "[Other]\no = 0\n");
}
