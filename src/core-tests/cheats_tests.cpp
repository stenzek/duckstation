// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cheats_private.h"

#include "common/error.h"

#include <array>
#include <gtest/gtest.h>

namespace {

static Cheats::CheatCode::Metadata MakeMetadata(Cheats::CodeType type,
                                                Cheats::CodeActivation activation = Cheats::CodeActivation::EndFrame)
{
  Cheats::CheatCode::Metadata metadata = {};
  metadata.name = "Test Code";
  metadata.type = type;
  metadata.activation = activation;
  return metadata;
}

static void ExpectAssemblyInstructions(const Cheats::AssemblyCheatCode& code,
                                       std::span<const std::pair<u32, u32>> expected)
{
  ASSERT_EQ(code.GetInstructions().size(), expected.size());
  for (size_t i = 0; i < expected.size(); i++)
  {
    EXPECT_EQ(code.GetInstructions()[i].pc, expected[i].first) << i;
    EXPECT_EQ(code.GetInstructions()[i].new_value, expected[i].second) << i;
    EXPECT_EQ(code.GetInstructions()[i].old_value, Cheats::AssemblyCheatCode::UNINITIALIZED_OLD_VALUE) << i;
  }
}

} // namespace

TEST(Cheats, ParseCodeInfo)
{
  static constexpr std::string_view input = "# ignored comment\n"
                                            "[Group\\Code]\n"
                                            "Author = Test Author\n"
                                            "Description = Test Description\n"
                                            "Type = Assembly\n"
                                            "Activation = EndFrame\n"
                                            "Option = Easy:0x12\n"
                                            "Option = Hard:52\n"
                                            "OptionRange = 1:255\n"
                                            "DisallowForAchievements = true\n"
                                            ".org 0x80010000\n"
                                            "addiu t0, zero, 1\n";

  Cheats::CodeInfoList codes;
  Error error;
  ASSERT_TRUE(Cheats::ImportCodesFromString(&codes, input, Cheats::FileFormat::DuckStation, true, &error))
    << error.GetDescription();
  ASSERT_EQ(codes.size(), 1u);

  const Cheats::CodeInfo& code = codes.front();
  EXPECT_EQ(code.name, "Group\\Code");
  EXPECT_EQ(code.GetNameParentPart(), "Group");
  EXPECT_EQ(code.GetNamePart(), "Code");
  EXPECT_EQ(code.author, "Test Author");
  EXPECT_EQ(code.description, "Test Description");
  EXPECT_EQ(code.type, Cheats::CodeType::Assembly);
  EXPECT_EQ(code.activation, Cheats::CodeActivation::EndFrame);
  EXPECT_FALSE(code.from_database);
  EXPECT_TRUE(code.disallow_for_achievements);
  ASSERT_EQ(code.options.size(), 2u);
  EXPECT_EQ(code.options[0], Cheats::CodeOption("Easy", 0x12));
  EXPECT_EQ(code.options[1], Cheats::CodeOption("Hard", 52));
  EXPECT_EQ(code.option_range_start, 1);
  EXPECT_EQ(code.option_range_end, 255);
  EXPECT_EQ(code.body, ".org 0x80010000\naddiu t0, zero, 1");
  EXPECT_EQ(input.substr(code.file_offset_start, code.file_offset_body_start - code.file_offset_start),
            "[Group\\Code]\nAuthor = Test Author\nDescription = Test Description\nType = Assembly\n"
            "Activation = EndFrame\nOption = Easy:0x12\nOption = Hard:52\nOptionRange = 1:255\n"
            "DisallowForAchievements = true\n");
  EXPECT_EQ(input.substr(code.file_offset_body_start, code.file_offset_end - code.file_offset_body_start),
            ".org 0x80010000\naddiu t0, zero, 1\n");
}

TEST(Cheats, ParseCodeInfoLegacyIgnoreAndReplacement)
{
  static constexpr std::string_view input = "#group=Legacy\n"
                                            "#type=Gameshark\n"
                                            "#activation=Manual\n"
                                            "[Ignored]\n"
                                            "Ignore = true\n"
                                            "80010000 0001\n"
                                            "[Duplicate]\n"
                                            "80010000 0002\n"
                                            "[Duplicate]\n"
                                            "80010000 0003\n";

  Cheats::CodeInfoList codes;
  Error error;
  ASSERT_TRUE(Cheats::ImportCodesFromString(&codes, input, Cheats::FileFormat::DuckStation, true, &error))
    << error.GetDescription();
  ASSERT_EQ(codes.size(), 1u);
  EXPECT_EQ(codes[0].name, "Legacy\\Duplicate");
  EXPECT_EQ(codes[0].type, Cheats::CodeType::Gameshark);
  EXPECT_EQ(codes[0].activation, Cheats::CodeActivation::Manual);
  EXPECT_EQ(codes[0].body, "80010000 0003");
}

TEST(Cheats, ParseCodeInfoErrors)
{
  Cheats::CodeInfoList codes;
  Error error;
  EXPECT_FALSE(Cheats::ImportCodesFromString(&codes, "[Code]\nOptionRange = 10:1\n80010000 0001\n",
                                             Cheats::FileFormat::DuckStation, true, &error));
  EXPECT_TRUE(error.IsValid());
}

TEST(Cheats, ParseGamesharkCode)
{
  static constexpr std::string_view input = "30000010 00000012\n"  // ConstantWrite8
                                            "80000020 00003456\n"  // ConstantWrite16
                                            "90000030 89ABCDEF\n"  // ExtConstantWrite32
                                            "10000040 00000001\n"  // Increment16
                                            "11000050 00000002\n"  // Decrement16
                                            "20000060 00000003\n"  // Increment8
                                            "21000070 00000004\n"  // Decrement8
                                            "60000080 00000005\n"  // ExtIncrement32
                                            "61000090 00000006\n"  // ExtDecrement32
                                            "D00000A0 00001111\n"  // CompareEqual16
                                            "D10000B0 00002222\n"  // CompareNotEqual16
                                            "D20000C0 00003333\n"  // CompareLess16
                                            "D30000D0 00004444\n"  // CompareGreater16
                                            "E00000E0 00000055\n"  // CompareEqual8
                                            "E10000F0 00000066\n"  // CompareNotEqual8
                                            "A0000100 12345678\n"  // ExtCompareEqual32
                                            "A1010110 87654321\n"  // ExtCompareNotEqual32
                                            "50000203 00040005\n"  // Slide
                                            "C2000120 00000130\n"  // MemoryCopy
                                            "53000140 01020304\n"  // ExtImprovedSlide
                                            "31000150 00000080\n"  // ExtConstantBitSet8
                                            "82000160 00004000\n"  // ExtConstantBitClear16
                                            "91000170 DEADBEEF\n"  // ExtConstantBitSet32
                                            "F4000180 00000190\n"  // ExtFindAndReplace
                                            "51000001 00000002\n"  // ExtCheatRegisters
                                            "52000003 00000004\n"; // ExtCheatRegistersCompare
  static constexpr std::array expected = {
    std::pair{0x30000010u, 0x00000012u}, std::pair{0x80000020u, 0x00003456u}, std::pair{0x90000030u, 0x89ABCDEFu},
    std::pair{0x10000040u, 0x00000001u}, std::pair{0x11000050u, 0x00000002u}, std::pair{0x20000060u, 0x00000003u},
    std::pair{0x21000070u, 0x00000004u}, std::pair{0x60000080u, 0x00000005u}, std::pair{0x61000090u, 0x00000006u},
    std::pair{0xD00000A0u, 0x00001111u}, std::pair{0xD10000B0u, 0x00002222u}, std::pair{0xD20000C0u, 0x00003333u},
    std::pair{0xD30000D0u, 0x00004444u}, std::pair{0xE00000E0u, 0x00000055u}, std::pair{0xE10000F0u, 0x00000066u},
    std::pair{0xA0000100u, 0x12345678u}, std::pair{0xA1010110u, 0x87654321u}, std::pair{0x50000203u, 0x00040005u},
    std::pair{0xC2000120u, 0x00000130u}, std::pair{0x53000140u, 0x01020304u}, std::pair{0x31000150u, 0x00000080u},
    std::pair{0x82000160u, 0x00004000u}, std::pair{0x91000170u, 0xDEADBEEFu}, std::pair{0xF4000180u, 0x00000190u},
    std::pair{0x51000001u, 0x00000002u}, std::pair{0x52000003u, 0x00000004u},
  };

  Error error;
  std::unique_ptr<Cheats::CheatCode> base = Cheats::ParseCode(MakeMetadata(Cheats::CodeType::Gameshark), input, &error);
  ASSERT_NE(base, nullptr) << error.GetDescription();
  Cheats::GamesharkCheatCode* code = static_cast<Cheats::GamesharkCheatCode*>(base.get());
  ASSERT_EQ(code->GetInstructions().size(), expected.size());
  for (size_t i = 0; i < expected.size(); i++)
  {
    EXPECT_EQ(code->GetInstructions()[i].first, expected[i].first) << i;
    EXPECT_EQ(code->GetInstructions()[i].second, expected[i].second) << i;
  }
}

TEST(Cheats, ParseGamesharkOptions)
{
  Cheats::CheatCode::Metadata metadata = MakeMetadata(Cheats::CodeType::Gameshark);
  metadata.has_options = true;
  Error error;
  std::unique_ptr<Cheats::CheatCode> base =
    Cheats::ParseCode(std::move(metadata), "30000010 000000??\n80000020 00??0000\n90000030 ????????\n", &error);
  ASSERT_NE(base, nullptr) << error.GetDescription();
  Cheats::GamesharkCheatCode* code = static_cast<Cheats::GamesharkCheatCode*>(base.get());

  code->SetOptionValue(0x89ABCDEF);
  ASSERT_EQ(code->GetInstructions().size(), 3u);
  EXPECT_EQ(code->GetInstructions()[0].second, 0x000000EFu);
  EXPECT_EQ(code->GetInstructions()[1].second, 0x00EF0000u);
  EXPECT_EQ(code->GetInstructions()[2].second, 0x89ABCDEFu);
}

TEST(Cheats, ParseGamesharkCodeErrors)
{
  static constexpr std::array<std::string_view, 4> cases = {
    "",
    "80010000",
    "80010000 zzzzzzzz",
    "80010000 00??00??",
  };
  for (const std::string_view input : cases)
  {
    Error error;
    EXPECT_EQ(Cheats::ParseCode(MakeMetadata(Cheats::CodeType::Gameshark), input, &error), nullptr) << input;
    EXPECT_TRUE(error.IsValid()) << input;
  }
}

TEST(Cheats, ParseAssemblyCode)
{
  Cheats::CheatCode::Metadata metadata = MakeMetadata(Cheats::CodeType::Assembly);
  metadata.has_options = true;
  static constexpr std::string_view input = ".org 0x80010000\n"
                                            "start: addiu t0, zero, 1\n"
                                            "beq t0, zero, target\n"
                                            "j start\n"
                                            "nop\n"
                                            "target: ori t1, zero, 0x????\n";

  Error error;
  std::unique_ptr<Cheats::CheatCode> base = Cheats::ParseCode(std::move(metadata), input, &error);
  ASSERT_NE(base, nullptr) << error.GetDescription();
  Cheats::AssemblyCheatCode* code = static_cast<Cheats::AssemblyCheatCode*>(base.get());
  static constexpr std::array expected = {
    std::pair{0x80010000u, 0x24080001u}, std::pair{0x80010004u, 0x11000002u}, std::pair{0x80010008u, 0x08004000u},
    std::pair{0x8001000Cu, 0x00000000u}, std::pair{0x80010010u, 0x34090000u},
  };
  ExpectAssemblyInstructions(*code, expected);

  code->SetOptionValue(0x1234);
  EXPECT_EQ(code->GetInstructions().back().new_value, 0x34091234u);
}

TEST(Cheats, ParseAssemblyInstructionFormatsAndOrigins)
{
  static constexpr std::string_view input = "; first section\n"
                                            ".org 0x80020000\n"
                                            "lui t0, 0x1234\n"
                                            "ori t0, t0, 0x5678 # construct constant\n"
                                            "sw t0, 16(sp)\n"
                                            "lw v0, -4(sp)\n"
                                            "addu v1, v0, t0\n"
                                            ".org 80030000\n"
                                            "jal 0x80031000\n"
                                            "jr ra\n"
                                            "0x80040000:\n"
                                            "nop\n";
  static constexpr std::array expected = {
    std::pair{0x80020000u, 0x3C081234u}, std::pair{0x80020004u, 0x35085678u}, std::pair{0x80020008u, 0xAFA80010u},
    std::pair{0x8002000Cu, 0x8FA2FFFCu}, std::pair{0x80020010u, 0x00481821u}, std::pair{0x80030000u, 0x0C00C400u},
    std::pair{0x80030004u, 0x03E00008u}, std::pair{0x80040000u, 0x00000000u},
  };

  Error error;
  std::unique_ptr<Cheats::CheatCode> base = Cheats::ParseCode(MakeMetadata(Cheats::CodeType::Assembly), input, &error);
  ASSERT_NE(base, nullptr) << error.GetDescription();
  ExpectAssemblyInstructions(*static_cast<Cheats::AssemblyCheatCode*>(base.get()), expected);
}

TEST(Cheats, ParseAssemblyBranchTypes)
{
  static constexpr std::string_view input = ".org 0x80040000\n"
                                            "target: nop\n"
                                            "beq t0, t1, target\n"
                                            "bne t0, t1, target\n"
                                            "blez t0, target\n"
                                            "bgtz t0, target\n"
                                            "bltz t0, target\n"
                                            "bgez t0, target\n"
                                            "bltzal t0, target\n"
                                            "bgezal t0, target\n";
  static constexpr std::array expected = {
    std::pair{0x80040000u, 0x00000000u}, std::pair{0x80040004u, 0x1109FFFEu}, std::pair{0x80040008u, 0x1509FFFDu},
    std::pair{0x8004000Cu, 0x1900FFFCu}, std::pair{0x80040010u, 0x1D00FFFBu}, std::pair{0x80040014u, 0x0500FFFAu},
    std::pair{0x80040018u, 0x0501FFF9u}, std::pair{0x8004001Cu, 0x0510FFF8u}, std::pair{0x80040020u, 0x0511FFF7u},
  };

  Error error;
  std::unique_ptr<Cheats::CheatCode> base = Cheats::ParseCode(MakeMetadata(Cheats::CodeType::Assembly), input, &error);
  ASSERT_NE(base, nullptr) << error.GetDescription();
  ExpectAssemblyInstructions(*static_cast<Cheats::AssemblyCheatCode*>(base.get()), expected);
}

TEST(Cheats, ParseAssemblyCodeErrors)
{
  static constexpr std::array<std::string_view, 8> cases = {
    "nop\n",
    ".org 0x80010002\nnop\n",
    ".org nope\nnop\n",
    ".org 0x80010000\nbeq zero, zero, missing\n",
    ".org 0x80010000\nlabel: nop\nlabel: nop\n",
    ".org 0x80010000\nnop\n.org 0x80010000\nnop\n",
    ".org 0x80010000\nori t0, zero, 0x??0?\n",
    ".org 0x80010000\n",
  };

  for (const std::string_view input : cases)
  {
    Error error;
    EXPECT_EQ(Cheats::ParseCode(MakeMetadata(Cheats::CodeType::Assembly), input, &error), nullptr) << input;
    EXPECT_TRUE(error.IsValid()) << input;
  }
}
