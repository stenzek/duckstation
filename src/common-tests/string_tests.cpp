// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/string_util.h"

#include <gtest/gtest.h>

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
