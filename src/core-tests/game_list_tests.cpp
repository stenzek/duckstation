// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/game_list.h"

#include <gtest/gtest.h>

using namespace GameList::GeneratedArtwork;

TEST(GameList, GeneratedArtworkMonogram)
{
  EXPECT_EQ(GetMonogram("The Legend of Dragoon"), "LD");
  EXPECT_EQ(GetMonogram("Suikoden II"), "S2");
  EXPECT_EQ(GetMonogram("Final Fantasy VII (Disc 1)"), "FF");
  EXPECT_EQ(GetMonogram("Suikoden III [USA]"), "S3");
  EXPECT_EQ(GetMonogram("Ape Escape VII"), "AE");
  EXPECT_EQ(GetMonogram("Crash Bandicoot 2 - Cortex Strikes Back"), "CB");
  EXPECT_EQ(GetMonogram("Tony Hawk's Pro Skater 2"), "TH");
  EXPECT_EQ(GetMonogram("Spyro 2 - Ripto's Rage!"), "S2");
  EXPECT_EQ(GetMonogram("NBA 2000"), "N20");
  EXPECT_EQ(GetMonogram("Xenogears"), "X");
  EXPECT_EQ(GetMonogram("padtest"), "P");
  EXPECT_EQ(GetMonogram("FF8-PAL"), "FP");
  EXPECT_EQ(GetMonogram("CTR - Crash Team Racing"), "CC");
  EXPECT_EQ(GetMonogram("The The"), "TT");
  EXPECT_EQ(GetMonogram("Ape Escape [USA]"), "AE");
  EXPECT_EQ(GetMonogram("Hawk’s Run"), "HR");
  EXPECT_EQ(GetMonogram("Rayman ・X"), "RX");
  EXPECT_EQ(GetMonogram("élan vital"), "ÉV");
  EXPECT_EQ(GetMonogram("ファイナルファンタジーVII (DISC 1)"), "フ");
  EXPECT_EQ(GetMonogram("ＳＩＭＰＬＥ１５００シリーズ　Ｖｏｌ．５７　ＴＨＥ　迷路"), "SV");
  EXPECT_EQ(GetMonogram("ＴＨＥ　ＫＩＮＧ　ＩＩ（ＤＩＳＣ　１）"), "K2");
  EXPECT_EQ(GetMonogram("バウンダリーゲート　—Ｄａｕｇｈｔｅｒ　ｏｆ　Ｋｉｎｇｄｏｍ—"), "バD");
  EXPECT_EQ(GetMonogram("(Disc 1)"), "D1");
  EXPECT_EQ(GetMonogram("!!!"), "");
  EXPECT_EQ(GetMonogram(""), "");
}

TEST(GameList, GeneratedArtworkColors)
{
  // Fixed values, so that colours don't change by accident.
  EXPECT_EQ(GetColors("Xenogears"), std::make_pair(0xff3098a6u, 0xff1d7259u));
  EXPECT_EQ(GetColors("Threads of Fate"), std::make_pair(0xff7130a6u, 0xff281d72u));
  EXPECT_EQ(GetColors("Final Fantasy VIII"), std::make_pair(0xff8ea630u, 0xff72601du));
  EXPECT_EQ(GetColors("Crash Bandicoot 2 - Cortex Strikes Back"), std::make_pair(0xff3054a6u, 0xff1d5a72u));
  EXPECT_EQ(GetColors("Metal Gear Solid (Disc 1)"), std::make_pair(0xffa65730u, 0xff721d24u));
  EXPECT_EQ(GetColors("Spyro 2 - Ripto's Rage!"), std::make_pair(0xffa67730u, 0xff722c1du));
  EXPECT_EQ(GetColors("Pokémon Stadium"), std::make_pair(0xff3ca630u, 0xff49721du));
  EXPECT_EQ(GetColors("ÉCOLE"), std::make_pair(0xff30a0a6u, 0xff1d7253u));
  EXPECT_EQ(GetColors("ファイナルファンタジーVII (DISC 1)"), std::make_pair(0xff5730a6u, 0xff1d2472u));
  EXPECT_EQ(GetColors("(Disc 1)"), std::make_pair(0xff3036a6u, 0xff1d4572u));
  EXPECT_EQ(GetColors(""), std::make_pair(0xff3030a6u, 0xff1d4072u));

  // Qualifiers and case don't affect the colour, so the discs of one game match.
  EXPECT_EQ(GetColors("Final Fantasy VII (Disc 1)"), GetColors("Final Fantasy VII (Disc 2)"));
  EXPECT_EQ(GetColors("Final Fantasy VII (Disc 1)"), GetColors("FINAL FANTASY VII [USA]"));
  EXPECT_EQ(GetColors("Final Fantasy VII (Disc 1)"), GetColors("ＦＩＮＡＬ　ＦＡＮＴＡＳＹ　ＶＩＩ"));
}
