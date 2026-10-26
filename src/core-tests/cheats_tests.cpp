// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/bus.h"
#include "core/cheats_private.h"
#include "core/cpu_code_cache.h"
#include "core/cpu_core_private.h"
#include "core/settings.h"

#include "common/error.h"
#include "common/scoped_guard.h"

#include "fmt/format.h"

#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <random>

namespace {

static Cheats::CodeMetadata MakeMetadata(Cheats::CodeType type,
                                         Cheats::CodeActivation activation = Cheats::CodeActivation::EndFrame)
{
  Cheats::CodeMetadata metadata = {};
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

static std::unique_ptr<Cheats::GamesharkCheatCode> ParseGamesharkCode(std::string_view input, Error* error)
{
  std::unique_ptr<Cheats::CheatCode> base = Cheats::ParseCode(MakeMetadata(Cheats::CodeType::Gameshark), input, error);
  return std::unique_ptr<Cheats::GamesharkCheatCode>(static_cast<Cheats::GamesharkCheatCode*>(base.release()));
}

template<size_t N>
static std::array<u32, N> GetBlockPayloadWords(const Cheats::GamesharkCheatCode& code)
{
  std::array<u32, N> words;
  std::memcpy(words.data(), &code.GetInstructions()[1], sizeof(words));
  return words;
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
  EXPECT_FALSE(code.force_pgxp_preserve_projection_precision);
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

TEST(Cheats, ParseAndFormatCodeInfoOverrides)
{
  static constexpr std::string_view input = "[Widescreen]\n"
                                            "Type = Gameshark\n"
                                            "Activation = EndFrame\n"
                                            "OverrideAspectRatio = 16:9\n"
                                            "OverrideCPUOverclock = 200\n"
                                            "DisableWidescreenRendering = true\n"
                                            "ForcePGXPPreserveProjectionPrecision = true\n"
                                            "Enable8MBRAM = true\n"
                                            "DisallowForAchievements = true\n"
                                            "80010000 0001\n";

  Cheats::CodeInfoList codes;
  Error error;
  ASSERT_TRUE(Cheats::ImportCodesFromString(&codes, input, Cheats::FileFormat::DuckStation, true, &error))
    << error.GetDescription();
  ASSERT_EQ(codes.size(), 1u);

  const Cheats::CodeInfo& code = codes.front();
  ASSERT_TRUE(code.override_aspect_ratio.has_value());
  EXPECT_EQ(code.override_aspect_ratio.value(), (DisplayAspectRatio{16, 9}));
  EXPECT_EQ(code.override_cpu_overclock, 200u);
  EXPECT_TRUE(code.disable_widescreen_rendering);
  EXPECT_TRUE(code.force_pgxp_preserve_projection_precision);
  EXPECT_TRUE(code.enable_8mb_ram);
  EXPECT_TRUE(code.disallow_for_achievements);

  // writing the code back out should preserve the overrides
  EXPECT_EQ(Cheats::FormatCodeForFile(code), input);
}

TEST(Cheats, PGXPPreserveProjectionPrecisionOverride)
{
  const Settings old_settings = g_settings;
  const ScopedGuard restore_settings([&old_settings]() { g_settings = old_settings; });

  for (const bool force_precision : {false, true})
  {
    Cheats::CodeMetadata metadata = MakeMetadata(Cheats::CodeType::Gameshark);
    metadata.force_pgxp_preserve_projection_precision = force_precision;
    Error error;
    std::unique_ptr<Cheats::CheatCode> code = Cheats::ParseCode(std::move(metadata), "80010000 0001\n", &error);
    ASSERT_NE(code, nullptr) << error.GetDescription();
    EXPECT_EQ(code->HasAnySettingOverrides(), force_precision);

    for (const bool pgxp_enabled : {false, true})
    {
      for (const bool preserve_precision : {false, true})
      {
        g_settings.gpu_pgxp_enable = pgxp_enabled;
        g_settings.gpu_pgxp_preserve_proj_fp = preserve_precision;
        code->ApplySettingOverrides();
        EXPECT_EQ(g_settings.gpu_pgxp_preserve_proj_fp, force_precision || preserve_precision);
        EXPECT_EQ(g_settings.gpu_pgxp_enable, pgxp_enabled);
      }
    }
  }
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
  Cheats::CodeMetadata metadata = MakeMetadata(Cheats::CodeType::Gameshark);
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
  Cheats::CodeMetadata metadata = MakeMetadata(Cheats::CodeType::Assembly);
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

TEST(Cheats, ParseGamesharkBlockWrite)
{
  Error error;
  std::unique_ptr<Cheats::GamesharkCheatCode> code = ParseGamesharkCode("A9000100 00000003\n"
                                                                        "11111111 22222222\n"
                                                                        "33333333 44444444\n",
                                                                        &error);
  ASSERT_NE(code, nullptr) << error.GetDescription();
  ASSERT_EQ(code->GetInstructions().size(), 3u);
  EXPECT_EQ(code->GetInstructions()[0].first, 0xA9000100u);
  EXPECT_EQ((GetBlockPayloadWords<4>(*code)), (std::array<u32, 4>{0x11111111u, 0x22222222u, 0x33333333u, 0u}));
}

TEST(Cheats, ParseGamesharkBlockWriteIfMatch)
{
  for (const std::string_view input : {"AB000100 00000002\nC0C0C0C0 A0A0A0A0\nC1C1C1C1 A1A1A1A1\n",
                                       "AA000100 00000002\nC0C0C0C0 A0A0A0A0\nC1C1C1C1 A1A1A1A1\n"})
  {
    Error error;
    std::unique_ptr<Cheats::GamesharkCheatCode> code = ParseGamesharkCode(input, &error);
    ASSERT_NE(code, nullptr) << error.GetDescription();
    ASSERT_EQ(code->GetInstructions().size(), 3u);
    EXPECT_EQ((GetBlockPayloadWords<4>(*code)),
              (std::array<u32, 4>{0xC0C0C0C0u, 0xC1C1C1C1u, 0xA0A0A0A0u, 0xA1A1A1A1u}))
      << input;
  }
}

TEST(Cheats, ParseGamesharkBlockWriteAfterMultiLineCode)
{
  // F4 data line looks like a block write header, which would be incomplete.
  Error error;
  std::unique_ptr<Cheats::GamesharkCheatCode> code = ParseGamesharkCode("F4000180 00000190\n"
                                                                        "A9000000 00000010\n"
                                                                        "00000000 00000000\n"
                                                                        "00000000 00000000\n"
                                                                        "00000000 00000000\n"
                                                                        "A9000100 00000001\n"
                                                                        "12345678 00000000\n",
                                                                        &error);
  ASSERT_NE(code, nullptr) << error.GetDescription();
  ASSERT_EQ(code->GetInstructions().size(), 7u);
  EXPECT_EQ(code->GetInstructions()[1].first, 0xA9000000u);
  EXPECT_EQ(code->GetInstructions()[6].bits, UINT64_C(0x12345678));
}

TEST(Cheats, ParseGamesharkBlockWriteErrors)
{
  static constexpr std::array<std::string_view, 9> cases = {
    "FB000100 00000004\n",
    "FC000100 00000004\n",
    "FD000100 00000004\n",
    "FE000100 00000004\n",
    "A9000100 00000000\n",
    "A9000100 00000003\n11111111 22222222\n",
    "AB000100 00000002\nC0C0C0C0 A0A0A0A0\n",
    "AB000100 00000002\nC0C0C0C0 0000000?\n11111111 22222222\n",
    "A9000100 0000000?\n11111111 22222222\n",
  };
  for (const std::string_view input : cases)
  {
    Error error;
    EXPECT_EQ(ParseGamesharkCode(input, &error), nullptr) << input;
    EXPECT_TRUE(error.IsValid()) << input;
  }
}

namespace {

class CheatsApply : public testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    g_settings.cpu_execution_mode = CPUExecutionMode::Interpreter;

    Error error;
    ASSERT_TRUE(CPU::CodeCache::ProcessStartup(&error)) << error.GetDescription();
    ASSERT_TRUE(Bus::AllocateMemory(false, &error)) << error.GetDescription();
    Bus::Initialize();
    CPU::Initialize();
    Bus::Reset();
    CPU::Reset();
  }

  static void TearDownTestSuite()
  {
    CPU::CodeCache::Shutdown();
    CPU::Shutdown();
    Bus::Shutdown();
    Bus::ReleaseMemory();
    CPU::CodeCache::ProcessShutdown();
  }

  void SetUp() override { std::memset(g_bus.unprotected_ram, 0, g_bus.ram_size); }

  static std::unique_ptr<Cheats::GamesharkCheatCode> Parse(std::string_view input)
  {
    Error error;
    std::unique_ptr<Cheats::GamesharkCheatCode> code = ParseGamesharkCode(input, &error);
    EXPECT_NE(code, nullptr) << error.GetDescription();
    return code;
  }

  static u32 ReadWord(u32 address)
  {
    u32 value;
    std::memcpy(&value, &g_bus.unprotected_ram[address], sizeof(value));
    return value;
  }

  static void WriteWord(u32 address, u32 value) { std::memcpy(&g_bus.unprotected_ram[address], &value, sizeof(value)); }
};

} // namespace

TEST_F(CheatsApply, BlockWrite)
{
  WriteWord(0x100C, 0xDEADBEEF);
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("A9001000 00000003\n"
                                                           "3C028001 8C420010\n"
                                                           "24420001 00000000\n");
  ASSERT_NE(code, nullptr);
  code->Apply();
  EXPECT_EQ(ReadWord(0x1000), 0x3C028001u);
  EXPECT_EQ(ReadWord(0x1004), 0x8C420010u);
  EXPECT_EQ(ReadWord(0x1008), 0x24420001u);
  EXPECT_EQ(ReadWord(0x100C), 0xDEADBEEFu);
}

TEST_F(CheatsApply, BlockWriteOutsideRAM)
{
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("A91FFFFC 00000002\n"
                                                           "11111111 22222222\n");
  ASSERT_NE(code, nullptr);
  code->Apply();
  EXPECT_EQ(ReadWord(0x1FFFFC), 0u);
  EXPECT_EQ(ReadWord(0), 0u);
}

TEST_F(CheatsApply, BlockWriteIfMatch)
{
  WriteWord(0x2000, 0x0C02A4B6);
  WriteWord(0x2004, 0x24020001);
  WriteWord(0x2008, 0xAE020011);
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("AB002000 00000003\n"
                                                           "0C02A4B6 00000000\n"
                                                           "24020001 24020000\n"
                                                           "AE020010 AE000010\n");
  ASSERT_NE(code, nullptr);
  EXPECT_FALSE(code->HasRestorableOnDisableEffects());

  // partial match, nothing should be written
  code->Apply();
  EXPECT_EQ(ReadWord(0x2000), 0x0C02A4B6u);
  EXPECT_EQ(ReadWord(0x2004), 0x24020001u);
  EXPECT_EQ(ReadWord(0x2008), 0xAE020011u);

  WriteWord(0x2008, 0xAE020010);
  code->Apply();
  EXPECT_EQ(ReadWord(0x2000), 0x00000000u);
  EXPECT_EQ(ReadWord(0x2004), 0x24020000u);
  EXPECT_EQ(ReadWord(0x2008), 0xAE000010u);

  // disabling shouldn't restore
  code->ApplyOnDisable(nullptr);
  EXPECT_EQ(ReadWord(0x2000), 0x00000000u);
}

TEST_F(CheatsApply, BlockWriteIfMatchInvalidatesICache)
{
  const ScopedGuard clear_icache([] { CPU::ClearICache(); });
  for (const std::string_view input :
       {"A900200C 00000002\n24020000 AE000010\n", "AA00200C 00000002\n24020001 24020000\nAE020010 AE000010\n",
        "AB00200C 00000002\n24020001 24020000\nAE020010 AE000010\n",
        "8000200C 0000\n8000200E 2402\n80002010 0010\n80002012 AE00\n",
        "A700200C 00010000\nA700200E 24022402\nA7002010 00100010\nA7002012 AE02AE00\n"})
  {
    SCOPED_TRACE(input);
    for (const u32 cached_alias : {0u, 0x80000000u, 0x00200000u, 0x80200000u})
    {
      SCOPED_TRACE(cached_alias);
      CPU::ClearICache();
      WriteWord(0x200C, 0x24020001);
      WriteWord(0x2010, 0xAE020010);
      std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse(input);
      ASSERT_NE(code, nullptr);

      // Fetch both cache lines before applying the patch.
      CPU::SetPC(cached_alias | 0x200C);
      ASSERT_EQ(CPU::g_state.current_instruction.bits, 0x24020001u);
      CPU::SetPC(cached_alias | 0x2010);
      ASSERT_EQ(CPU::g_state.current_instruction.bits, 0xAE020010u);

      code->Apply();
      EXPECT_EQ(ReadWord(0x200C), 0x24020000u);
      EXPECT_EQ(ReadWord(0x2010), 0xAE000010u);

      CPU::SetPC(cached_alias | 0x200C);
      EXPECT_EQ(CPU::g_state.current_instruction.bits, 0x24020000u);
      CPU::SetPC(cached_alias | 0x2010);
      EXPECT_EQ(CPU::g_state.current_instruction.bits, 0xAE000010u);
    }
  }
}

TEST_F(CheatsApply, BlockWriteIfMatchWithRestore)
{
  WriteWord(0x2000, 0x11111111);
  WriteWord(0x2004, 0x22222222);
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("AA002000 00000002\n"
                                                           "11111111 AAAAAAAA\n"
                                                           "22222222 BBBBBBBB\n");
  ASSERT_NE(code, nullptr);
  EXPECT_TRUE(code->HasRestorableOnDisableEffects());

  code->Apply();
  EXPECT_EQ(ReadWord(0x2000), 0xAAAAAAAAu);
  EXPECT_EQ(ReadWord(0x2004), 0xBBBBBBBBu);

  Cheats::RollbackLog rollback;
  code->ApplyOnDisable(&rollback);
  EXPECT_EQ(ReadWord(0x2000), 0x11111111u);
  EXPECT_EQ(ReadWord(0x2004), 0x22222222u);
  ASSERT_EQ(rollback.size(), 2u);
  EXPECT_EQ(rollback[0].size, MemoryAccessSize::Word);
  EXPECT_EQ(rollback[0].address, 0x2000u);
  EXPECT_EQ(rollback[0].value, 0xAAAAAAAAu);
  EXPECT_EQ(rollback[1].address, 0x2004u);
  EXPECT_EQ(rollback[1].value, 0xBBBBBBBBu);

  Cheats::ReapplyOnDisableCodes(rollback);
  EXPECT_EQ(ReadWord(0x2000), 0xAAAAAAAAu);
  EXPECT_EQ(ReadWord(0x2004), 0xBBBBBBBBu);
}

TEST_F(CheatsApply, BlockWriteSkippedByConditional)
{
  // Payload packs to 30000000 00000055, which would write to 0 if executed as an instruction.
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("D0003000 1234\n"
                                                           "A9001000 00000002\n"
                                                           "00000055 30000000\n"
                                                           "90001010 00000099\n");
  ASSERT_NE(code, nullptr);
  code->Apply();
  EXPECT_EQ(ReadWord(0x1000), 0u);
  EXPECT_EQ(ReadWord(0), 0u);
  EXPECT_EQ(ReadWord(0x1010), 0x99u);

  g_bus.unprotected_ram[0x3000] = 0x34;
  g_bus.unprotected_ram[0x3001] = 0x12;
  code->Apply();
  EXPECT_EQ(ReadWord(0x1000), 0x55u);
  EXPECT_EQ(ReadWord(0x1004), 0x30000000u);
}

TEST_F(CheatsApply, BlockWriteSkippedByBlockConditional)
{
  // Payload packs to the 00000000 FFFF separator.
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("C0003000 1234\n"
                                                           "A9001000 00000002\n"
                                                           "0000FFFF 00000000\n"
                                                           "90001020 00000077\n"
                                                           "00000000 FFFF\n"
                                                           "90001030 00000088\n");
  ASSERT_NE(code, nullptr);
  code->Apply();
  EXPECT_EQ(ReadWord(0x1000), 0u);
  EXPECT_EQ(ReadWord(0x1020), 0u);
  EXPECT_EQ(ReadWord(0x1030), 0x88u);
}

TEST_F(CheatsApply, FindAndReplacePayloadSkippedByBlockConditional)
{
  // An F4 data word that resembles a block header must not skip the separator.
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("C0003000 1234\n"
                                                           "F4000180 00000190\n"
                                                           "A9000000 00000010\n"
                                                           "00000000 00000000\n"
                                                           "00000000 00000000\n"
                                                           "00000000 00000000\n"
                                                           "00000000 FFFF\n"
                                                           "90001030 00000088\n");
  ASSERT_NE(code, nullptr);
  code->Apply();
  EXPECT_EQ(ReadWord(0x1030), 0x88u);
}

TEST_F(CheatsApply, FindAndReplacePayloadSkippedByConditional)
{
  // A false D0 must skip the entire F4, including data that resembles an A9 header.
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("D0003000 1234\n"
                                                           "F4000180 00000190\n"
                                                           "A9001000 00000002\n"
                                                           "00000055 00000066\n"
                                                           "00000000 00000000\n"
                                                           "00000000 00000000\n"
                                                           "90001030 00000088\n");
  ASSERT_NE(code, nullptr);
  code->Apply();
  EXPECT_EQ(ReadWord(0x1000), 0u);
  EXPECT_EQ(ReadWord(0x1004), 0u);
  EXPECT_EQ(ReadWord(0x1030), 0x88u);
}

TEST(Cheats, BlockWritePayloadDoesNotHaveRestorableEffects)
{
  for (const std::string_view input :
       {"A9000100 00000002\n00000000 AA000000\n", "AB000100 00000002\n00000000 00000000\nAA000000 00000000\n"})
  {
    SCOPED_TRACE(input);
    Error error;
    std::unique_ptr<Cheats::GamesharkCheatCode> code = ParseGamesharkCode(input, &error);
    ASSERT_NE(code, nullptr) << error.GetDescription();
    EXPECT_FALSE(code->HasRestorableOnDisableEffects());
  }
}

TEST(Cheats, ParseGamesharkMergedWrites)
{
  // Constant writes merge with gaps and overlap, in line order.
  Error error;
  std::unique_ptr<Cheats::GamesharkCheatCode> code = ParseGamesharkCode("30001000 00000011\n"
                                                                        "80001002 00002233\n"
                                                                        "90001001 44556677\n"
                                                                        "30001010 00000088\n",
                                                                        &error);
  ASSERT_NE(code, nullptr) << error.GetDescription();
  ASSERT_EQ(code->GetInstructions().size(), 1u + 5u);
  EXPECT_EQ(code->GetInstructions()[0].first, 0xFB001000u);
  EXPECT_EQ(code->GetInstructions()[0].second, 17u);
  std::array<u8, 34> payload;
  std::memcpy(payload.data(), &code->GetInstructions()[1], sizeof(payload));
  EXPECT_EQ(payload, (std::array<u8, 34>{0x11, 0x77, 0x66, 0x55, 0x44, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x88,
                                         0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF}));

  // Too short, overlapping match writes, and data lines don't merge.
  for (const std::string_view input : {"30001000 00000011\n30001001 00000011\n30001002 00000011\n",
                                       "A7001000 00010002\nA7001001 00010002\nA7001004 00010002\nA7001006 00010002\n",
                                       "F4000180 00000190\n30001000 00000011\n30001001 00000011\n"
                                       "30001002 00000011\n30001003 00000011\n"})
  {
    SCOPED_TRACE(input);
    code = ParseGamesharkCode(input, &error);
    ASSERT_NE(code, nullptr) << error.GetDescription();
    EXPECT_EQ(code->GetInstructions().size(), static_cast<size_t>(std::count(input.begin(), input.end(), '\n')));
  }
}

TEST_F(CheatsApply, MergedConstantWrites)
{
  WriteWord(0x1004, 0xDEADBEEF);
  WriteWord(0x1014, 0xDEADBEEF);
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("90001000 11111111\n"
                                                           "80001008 2222\n"
                                                           "3000100A 33\n"
                                                           "90001010 44444444\n"
                                                           "90001018 55555555\n");
  ASSERT_NE(code, nullptr);
  ASSERT_EQ(code->GetInstructions()[0].code, Cheats::GamesharkCheatCode::InstructionCode::InternalMaskedWrite);
  code->Apply();
  EXPECT_EQ(ReadWord(0x1000), 0x11111111u);
  EXPECT_EQ(ReadWord(0x1004), 0xDEADBEEFu);
  EXPECT_EQ(ReadWord(0x1008), 0x00332222u);
  EXPECT_EQ(ReadWord(0x100C), 0u);
  EXPECT_EQ(ReadWord(0x1010), 0x44444444u);
  EXPECT_EQ(ReadWord(0x1014), 0xDEADBEEFu);
  EXPECT_EQ(ReadWord(0x1018), 0x55555555u);
  EXPECT_EQ(ReadWord(0x101C), 0u);
}

TEST_F(CheatsApply, MergedUnchangedMatchWritesInvalidateICache)
{
  const ScopedGuard clear_icache([] { CPU::ClearICache(); });
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("A7002000 00010001\n"
                                                           "A7002002 24022402\n"
                                                           "A7002010 00010001\n"
                                                           "A7002012 24022402\n");
  ASSERT_NE(code, nullptr);
  ASSERT_EQ(code->GetInstructions()[0].code,
            Cheats::GamesharkCheatCode::InstructionCode::InternalMatchWriteWithRestore16);

  for (const bool restore : {false, true})
  {
    SCOPED_TRACE(restore);
    CPU::ClearICache();
    for (const u32 address : {0x2000u, 0x2010u})
    {
      WriteWord(address, 0x24020000);
      CPU::SetPC(0x80000000u | address);
      ASSERT_EQ(CPU::g_state.current_instruction.bits, 0x24020000u);
      // Update RAM while leaving the old instruction cached.
      WriteWord(address, 0x24020001);
    }

    if (restore)
      code->ApplyOnDisable(nullptr);
    else
      code->Apply();

    // Covers both the vector loop and the scalar tail.
    for (const u32 address : {0x2000u, 0x2010u})
    {
      EXPECT_EQ(ReadWord(address), 0x24020001u);
      CPU::SetPC(0x80000000u | address);
      EXPECT_EQ(CPU::g_state.current_instruction.bits, 0x24020001u);
    }
  }
}

TEST_F(CheatsApply, MergedWritesOnlyInvalidateWrittenICacheLines)
{
  const ScopedGuard clear_icache([] { CPU::ClearICache(); });

  // Caches old_value at address, then changes RAM underneath it.
  const auto cache_stale = [](u32 address, u32 old_value, u32 new_value) {
    WriteWord(address, old_value);
    CPU::SetPC(0x80000000u | address);
    ASSERT_EQ(CPU::g_state.current_instruction.bits, old_value);
    WriteWord(address, new_value);
  };
  const auto fetch = [](u32 address) {
    CPU::SetPC(0x80000000u | address);
    return CPU::g_state.current_instruction.bits;
  };

  // Compares fail at 0x1FF8 and 0x2010, and match at 0x2000 in the same vector chunk as 0x1FF8.
  for (const bool changed : {false, true})
  {
    SCOPED_TRACE(changed);
    std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse(changed ? "A7001FF8 11112222\n"
                                                                       "A7001FFA 11112222\n"
                                                                       "A7002000 00010002\n"
                                                                       "A7002002 24022402\n"
                                                                       "A7002010 11112222\n"
                                                                       "A7002012 11112222\n" :
                                                                       "A7001FF8 11112222\n"
                                                                       "A7001FFA 11112222\n"
                                                                       "A7002000 00010001\n"
                                                                       "A7002002 24022402\n"
                                                                       "A7002010 11112222\n"
                                                                       "A7002012 11112222\n");
    ASSERT_NE(code, nullptr);
    ASSERT_EQ(code->GetInstructions().front().code,
              Cheats::GamesharkCheatCode::InstructionCode::InternalMatchWriteWithRestore16);

    CPU::ClearICache();
    cache_stale(0x1FF8, 0x24020000, 0x77776666);
    cache_stale(0x2000, 0x24020000, 0x24020001);
    cache_stale(0x2010, 0x24020000, 0x77776666);
    code->Apply();
    EXPECT_EQ(fetch(0x1FF8), 0x24020000u);
    EXPECT_EQ(fetch(0x2000), changed ? 0x24020002u : 0x24020001u);
    EXPECT_EQ(fetch(0x2010), 0x24020000u);
  }

  // The gap between 0x1FFC and 0x2010 covers the whole of line 0x2000.
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("80001FF8 0001\n"
                                                           "80001FFA 2402\n"
                                                           "80002010 0001\n"
                                                           "80002012 2402\n");
  ASSERT_NE(code, nullptr);
  ASSERT_EQ(code->GetInstructions().front().code, Cheats::GamesharkCheatCode::InstructionCode::InternalMaskedWrite);

  CPU::ClearICache();
  cache_stale(0x1FF8, 0x24020000, 0x77776666);
  cache_stale(0x2000, 0x24020000, 0x77776666);
  cache_stale(0x2010, 0x24020000, 0x77776666);
  code->Apply();
  EXPECT_EQ(fetch(0x1FF8), 0x24020001u);
  EXPECT_EQ(fetch(0x2000), 0x24020000u);
  EXPECT_EQ(fetch(0x2010), 0x24020001u);
}

TEST_F(CheatsApply, MergedWritesSkipICacheWhenUnused)
{
  const ScopedGuard restore([old_settings = g_settings]() {
    g_settings = old_settings;
    CPU::g_state.using_interpreter = true;
    CPU::ClearICache();
  });

  // Blocks only use the tags with the recompiler icache, the interpreter always does.
  CPU::g_state.using_interpreter = false;
  for (const std::string_view input : {"80002000 0002\n80002002 2402\n80002004 0000\n80002006 0000\n",
                                       "A7002000 00010002\nA7002002 24022402\nA7002004 00000000\nA7002006 00000000\n"})
  {
    SCOPED_TRACE(input);
    std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse(input);
    ASSERT_NE(code, nullptr);
    ASSERT_GE(static_cast<u32>(code->GetInstructions().front().code.GetValue()), 0xFBu);
    for (const bool recompiler_icache : {false, true})
    {
      SCOPED_TRACE(recompiler_icache);
      g_settings.cpu_recompiler_icache = recompiler_icache;
      WriteWord(0x2000, 0x24020001);
      CPU::g_state.icache_tags[CPU::GetICacheLine(0x2000)] = 0x80002000u;
      code->Apply();
      EXPECT_EQ(ReadWord(0x2000), 0x24020002u);
      EXPECT_EQ(CPU::g_state.icache_tags[CPU::GetICacheLine(0x2000)],
                recompiler_icache ? static_cast<u32>(CPU::ICACHE_INVALID_BITS) : 0x80002000u);
    }
  }
}

TEST_F(CheatsApply, MergedMatchWritesWithRestore)
{
  // Second line doesn't match, so it's not written or restored.
  WriteWord(0x2000, 0x22221111);
  WriteWord(0x2010, 0x44443333);
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("A7002000 1111AAAA\n"
                                                           "A7002002 9999BBBB\n"
                                                           "A7002010 3333CCCC\n"
                                                           "A7002012 4444DDDD\n");
  ASSERT_NE(code, nullptr);
  ASSERT_EQ(code->GetInstructions()[0].code,
            Cheats::GamesharkCheatCode::InstructionCode::InternalMatchWriteWithRestore16);
  EXPECT_TRUE(code->HasRestorableOnDisableEffects());

  code->Apply();
  EXPECT_EQ(ReadWord(0x2000), 0x2222AAAAu);
  EXPECT_EQ(ReadWord(0x2010), 0xDDDDCCCCu);

  Cheats::RollbackLog rollback;
  code->ApplyOnDisable(&rollback);
  EXPECT_EQ(ReadWord(0x2000), 0x22221111u);
  EXPECT_EQ(ReadWord(0x2010), 0x44443333u);
  ASSERT_EQ(rollback.size(), 3u);
  EXPECT_EQ(rollback[0].size, MemoryAccessSize::HalfWord);
  EXPECT_EQ(rollback[0].address, 0x2000u);
  EXPECT_EQ(rollback[0].value, 0xAAAAu);
  EXPECT_EQ(rollback[1].address, 0x2010u);
  EXPECT_EQ(rollback[1].value, 0xCCCCu);
  EXPECT_EQ(rollback[2].address, 0x2012u);
  EXPECT_EQ(rollback[2].value, 0xDDDDu);

  Cheats::ReapplyOnDisableCodes(rollback);
  EXPECT_EQ(ReadWord(0x2000), 0x2222AAAAu);
  EXPECT_EQ(ReadWord(0x2010), 0xDDDDCCCCu);
}

TEST_F(CheatsApply, MergeSkipsConditionalsAndOptions)
{
  Cheats::CodeMetadata metadata = MakeMetadata(Cheats::CodeType::Gameshark);
  metadata.has_options = true;
  Error error;
  std::unique_ptr<Cheats::CheatCode> base = Cheats::ParseCode(std::move(metadata),
                                                              "D0003000 1234\n"
                                                              "30001000 00000011\n"
                                                              "30001001 00000022\n"
                                                              "30001002 00000033\n"
                                                              "30001003 00000044\n"
                                                              "30001004 00000055\n"
                                                              "30001005 000000??\n"
                                                              "30001006 00000077\n",
                                                              &error);
  ASSERT_NE(base, nullptr) << error.GetDescription();
  Cheats::GamesharkCheatCode* code = static_cast<Cheats::GamesharkCheatCode*>(base.get());

  // D0, 30, FB + 1 payload, option line, 30
  ASSERT_EQ(code->GetInstructions().size(), 6u);
  EXPECT_EQ(code->GetInstructions()[2].code, Cheats::GamesharkCheatCode::InstructionCode::InternalMaskedWrite);
  code->SetOptionValue(0x66);
  EXPECT_EQ(code->GetInstructions()[4].first, 0x30001005u);
  EXPECT_EQ(code->GetInstructions()[4].second, 0x66u);

  code->Apply();
  EXPECT_EQ(ReadWord(0x1000), 0x44332200u);
  EXPECT_EQ(ReadWord(0x1004), 0x00776655u);
}

TEST_F(CheatsApply, MergeSplitsAt2MBBoundary)
{
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("801FFFF8 1111\n"
                                                           "801FFFFA 2222\n"
                                                           "801FFFFC 3333\n"
                                                           "801FFFFE 4444\n"
                                                           "90200000 55555555\n"
                                                           "90200004 66666666\n"
                                                           "90200008 77777777\n"
                                                           "9020000C 88888888\n");
  ASSERT_NE(code, nullptr);
  ASSERT_EQ(code->GetInstructions()[0].second, 8u);
  code->Apply();
  EXPECT_EQ(ReadWord(0x1FFFF8), 0x22221111u);
  EXPECT_EQ(ReadWord(0x1FFFFC), 0x44443333u);
  EXPECT_EQ(ReadWord(0x0), 0x55555555u);
  EXPECT_EQ(ReadWord(0xC), 0x88888888u);
}

TEST_F(CheatsApply, MergedWritesMatchSequential)
{
  // Compare against the same lines separated by nops, which prevents merging.
  std::mt19937 rng(0x5EED);
  const auto random = [&rng](u32 n) { return static_cast<u32>(rng() % n); };
  static constexpr std::array<std::pair<u32, u32>, 2> regions = {{{0, 0xA000}, {0x1FF000, 0x200000}}};
  u32 merged_count = 0;
  const ScopedGuard restore_interpreter([]() { CPU::g_state.using_interpreter = true; });
  for (u32 iteration = 0; iteration < 500; iteration++)
  {
    SCOPED_TRACE(iteration);

    // Odd iterations use the no-icache paths, as the recompilers would.
    CPU::g_state.using_interpreter = ((iteration & 1) == 0);
    std::string input, reference_input;
    std::vector<std::pair<u32, u16>> presets;
    const auto add_line = [&](u32 first, u32 second, bool guard = false) {
      const std::string line = fmt::format("{:08X} {:08X}\n", first, second);
      input += line;
      reference_input += line;
      if (!guard)
        reference_input += "00000000 00000000\n";
    };

    for (u32 run = 0, num_runs = 1 + random(4); run < num_runs; run++)
    {
      const u32 group = random(4);
      u32 address = (random(4) == 0) ? (0x1FFF80 + random(0x40)) : (0x1000 + random(0x7000));
      for (u32 line = 0, num_lines = 1 + random(40); line < num_lines; line++)
      {
        if (random(16) == 0)
        {
          const u32 guard_address = 0x8000 + random(0x100) * 2;
          const u16 guard_value = Truncate16(rng());
          if (random(2))
            presets.emplace_back(guard_address, guard_value);
          add_line(0xD0000000u | guard_address, guard_value, true);
        }

        const u32 compare = rng(), write = rng();
        u32 width;
        switch (group)
        {
          case 0:
          {
            static constexpr std::array<u32, 3> codes = {0x30, 0x80, 0x90};
            const u32 code = codes[random(3)];
            width = (code == 0x30) ? 1 : ((code == 0x80) ? 2 : 4);
            add_line((code << 24) | address, write & ((width == 4) ? 0xFFFFFFFFu : ((1u << (width * 8)) - 1)));
          }
          break;

          case 1:
          case 2:
            width = 2;
            if (random(2))
              presets.emplace_back(address, Truncate16(compare));
            add_line(((group == 1) ? 0xA6000000u : 0xA7000000u) | address, (compare << 16) | (write & 0xFFFF));
            break;

          default:
            width = 1;
            if (random(2))
              presets.emplace_back(address | 0x80000000u, Truncate8(compare));
            add_line(0xA8000000u | address, ((compare & 0xFF) << 8) | (write & 0xFF));
            break;
        }

        // Mostly small forward steps, with some overlap, misalignment and large gaps.
        const u32 step = random(16);
        if (step == 0)
          address += random(100);
        else if (step == 1)
          address -= std::min(address, random(4));
        else if (step == 2)
          address += 1;
        else
          address += width + random(3) * width;
      }
    }

    std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse(input);
    std::unique_ptr<Cheats::GamesharkCheatCode> reference = Parse(reference_input);
    ASSERT_NE(code, nullptr);
    ASSERT_NE(reference, nullptr);
    merged_count +=
      (code->GetInstructions().size() != static_cast<size_t>(std::count(input.begin(), input.end(), '\n')));

    const auto run = [&](const Cheats::GamesharkCheatCode& c, std::vector<u8>* ram, Cheats::RollbackLog* rollback) {
      std::mt19937 ram_rng(iteration);
      for (const auto& [start, end] : regions)
      {
        for (u32 i = start; i < end; i++)
          g_bus.unprotected_ram[i] = Truncate8(ram_rng());
      }
      for (const auto& [address, value] : presets)
      {
        if (address & 0x80000000u)
          g_bus.unprotected_ram[address & g_bus.ram_mask] = Truncate8(value);
        else
          CPU::SafeWriteMemoryHalfWord(address, value);
      }

      for (u32 pass = 0; pass < 3; pass++)
      {
        if (pass == 1)
          c.ApplyOnDisable(rollback);
        else
          c.Apply();

        for (const auto& [start, end] : regions)
          ram->insert(ram->end(), &g_bus.unprotected_ram[start], &g_bus.unprotected_ram[end]);
      }
    };

    std::vector<u8> ram, reference_ram;
    Cheats::RollbackLog rollback, reference_rollback;
    run(*code, &ram, &rollback);
    run(*reference, &reference_ram, &reference_rollback);
    ASSERT_TRUE(ram == reference_ram) << input;
    ASSERT_EQ(rollback.size(), reference_rollback.size()) << input;
    for (size_t i = 0; i < rollback.size(); i++)
    {
      EXPECT_EQ(rollback[i].size, reference_rollback[i].size) << i;
      EXPECT_EQ(rollback[i].address, reference_rollback[i].address) << i;
      EXPECT_EQ(rollback[i].value, reference_rollback[i].value) << i;
    }
  }

  EXPECT_GT(merged_count, 250u);
}
