// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/analog_controller.h"
#include "core/bus.h"
#include "core/cheats_private.h"
#include "core/cpu_code_cache.h"
#include "core/cpu_core_private.h"
#include "core/digital_controller.h"
#include "core/negcon.h"
#include "core/pad.h"
#include "core/settings.h"
#include "core/system.h"

#include "common/error.h"
#include "common/log.h"
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

template<typename F>
static u32 CountLogErrors(F&& run)
{
  u32 errors = 0;
  const Log::CallbackFunctionType count_errors = [](void* param, Log::MessageCategory category, const char*,
                                                    std::string_view) {
    if (Log::UnpackLevel(category) == Log::Level::Error)
      (*static_cast<u32*>(param))++;
  };
  Log::RegisterCallback(count_errors, &errors);
  ScopedGuard unregister([count_errors, &errors]() { Log::UnregisterCallback(count_errors, &errors); });
  run();
  unregister.Run();
  return errors;
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
                                            "OverrideRAMSize = 8MBForced\n"
                                            "OverrideVRAMSize = 2MBForced\n"
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
  EXPECT_EQ(code.override_ram_size, std::make_optional(CPURAMSize::Expanded8MBForced));
  EXPECT_EQ(code.override_vram_size, std::make_optional(GPUVRAMSize::Expanded2MBForced));
  EXPECT_TRUE(code.disallow_for_achievements);

  // writing the code back out should preserve the overrides
  EXPECT_EQ(Cheats::FormatCodeForFile(code), input);
}

TEST(Cheats, ParseAndFormatStartFrameCode)
{
  for (const auto& [type, body] : {std::pair{Cheats::CodeType::Gameshark, "80010000 0001\n"},
                                   std::pair{Cheats::CodeType::Assembly, ".org 0x80010000\naddiu t0, zero, 1\n"}})
  {
    const std::string input =
      fmt::format("[Late Code]\nType = {}\nActivation = StartFrame\n{}", Cheats::GetTypeName(type), body);
    Cheats::CodeInfoList codes;
    Error error;
    ASSERT_TRUE(Cheats::ImportCodesFromString(&codes, input, Cheats::FileFormat::DuckStation, true, &error))
      << error.GetDescription();
    ASSERT_EQ(codes.size(), 1u);
    EXPECT_EQ(codes.front().activation, Cheats::CodeActivation::StartFrame);
    EXPECT_EQ(Cheats::FormatCodeForFile(codes.front()), input);
    EXPECT_NE(Cheats::ParseCode(codes.front(), codes.front().body, &error), nullptr) << error.GetDescription();
  }
}

TEST(Cheats, ParseAndFormatRAMSizeOverrides)
{
  static constexpr std::string_view input = "[Four]\n"
                                            "OverrideRAMSize = 4MB\n"
                                            "80010000 0001\n"
                                            "[Eight]\n"
                                            "Enable8MBRAM = true\n"
                                            "80010000 0001\n"
                                            "[Sixteen]\n"
                                            "Enable16MBRAM = true\n"
                                            "80010000 0001\n"
                                            "[Largest]\n"
                                            "OverrideRAMSize = 16MB\n"
                                            "Enable8MBRAM = true\n"
                                            "80010000 0001\n"
                                            "[Disabled]\n"
                                            "Enable8MBRAM = false\n"
                                            "80010000 0001\n"
                                            "[Invalid]\n"
                                            "OverrideRAMSize = 3MB\n"
                                            "80010000 0001\n"
                                            "[NoSuffix]\n"
                                            "OverrideRAMSize = 4\n"
                                            "80010000 0001\n"
                                            "[ForceMapping]\n"
                                            "OverrideRAMSize = 16MB\n"
                                            "ForceFullRAMMapping = true\n"
                                            "80010000 0001\n";

  Cheats::CodeInfoList codes;
  Error error;
  ASSERT_TRUE(Cheats::ImportCodesFromString(&codes, input, Cheats::FileFormat::DuckStation, true, &error))
    << error.GetDescription();
  ASSERT_EQ(codes.size(), 8u);
  EXPECT_EQ(codes[0].override_ram_size, std::make_optional(CPURAMSize::Expanded4MB));
  EXPECT_EQ(codes[1].override_ram_size, std::make_optional(CPURAMSize::Expanded8MB));
  EXPECT_EQ(codes[2].override_ram_size, std::make_optional(CPURAMSize::Expanded16MB));
  EXPECT_EQ(codes[3].override_ram_size, std::make_optional(CPURAMSize::Expanded16MB));
  EXPECT_FALSE(codes[4].override_ram_size.has_value());
  EXPECT_FALSE(codes[5].override_ram_size.has_value());
  EXPECT_FALSE(codes[6].override_ram_size.has_value());
  EXPECT_EQ(codes[7].override_ram_size, std::make_optional(CPURAMSize::Expanded16MBForced));

  // The aliases are written back out as OverrideRAMSize.
  EXPECT_NE(Cheats::FormatCodeForFile(codes[0]).find("OverrideRAMSize = 4MB\n"), std::string::npos);
  EXPECT_NE(Cheats::FormatCodeForFile(codes[1]).find("OverrideRAMSize = 8MB\n"), std::string::npos);
  EXPECT_NE(Cheats::FormatCodeForFile(codes[2]).find("OverrideRAMSize = 16MB\n"), std::string::npos);
  EXPECT_EQ(Cheats::FormatCodeForFile(codes[1]).find("Enable8MBRAM"), std::string::npos);
  EXPECT_EQ(Cheats::FormatCodeForFile(codes[2]).find("Enable16MBRAM"), std::string::npos);
  EXPECT_NE(Cheats::FormatCodeForFile(codes[7]).find("OverrideRAMSize = 16MBForced\n"), std::string::npos);
  EXPECT_EQ(Cheats::FormatCodeForFile(codes[7]).find("ForceFullRAMMapping"), std::string::npos);
}

TEST(Cheats, RAMSizeOverrideUsesLargestSize)
{
  const Settings old_settings = g_settings;
  const ScopedGuard restore_settings([&old_settings]() { g_settings = old_settings; });

  std::vector<std::unique_ptr<Cheats::CheatCode>> codes;
  for (const CPURAMSize size : {CPURAMSize::Expanded4MB, CPURAMSize::Expanded8MB, CPURAMSize::Expanded16MB})
  {
    Cheats::CodeMetadata metadata = MakeMetadata(Cheats::CodeType::Gameshark);
    metadata.override_ram_size = size;
    Error error;
    codes.push_back(Cheats::ParseCode(std::move(metadata), "80010000 0001\n", &error));
    ASSERT_NE(codes.back(), nullptr) << error.GetDescription();
    EXPECT_TRUE(codes.back()->HasAnySettingOverrides());
  }

  g_settings.cpu_ram_size = Settings::DEFAULT_CPU_RAM_SIZE;
  codes[0]->ApplySettingOverrides();
  EXPECT_EQ(g_settings.cpu_ram_size, CPURAMSize::Expanded4MB);
  codes[1]->ApplySettingOverrides();
  codes[2]->ApplySettingOverrides();
  EXPECT_EQ(g_settings.cpu_ram_size, CPURAMSize::Expanded16MB);

  // The configured size is not reduced.
  g_settings.cpu_ram_size = CPURAMSize::Expanded8MB;
  codes[0]->ApplySettingOverrides();
  EXPECT_EQ(g_settings.cpu_ram_size, CPURAMSize::Expanded8MB);

  // Forcing the full mapping is kept from either side.
  g_settings.cpu_ram_size = CPURAMSize::Expanded8MBForced;
  codes[2]->ApplySettingOverrides();
  EXPECT_EQ(g_settings.cpu_ram_size, CPURAMSize::Expanded16MBForced);
  Cheats::CodeMetadata metadata = MakeMetadata(Cheats::CodeType::Gameshark);
  metadata.override_ram_size = CPURAMSize::Expanded4MBForced;
  Error error;
  std::unique_ptr<Cheats::CheatCode> forced_code = Cheats::ParseCode(std::move(metadata), "80010000 0001\n", &error);
  ASSERT_NE(forced_code, nullptr) << error.GetDescription();
  g_settings.cpu_ram_size = CPURAMSize::Expanded16MB;
  forced_code->ApplySettingOverrides();
  EXPECT_EQ(g_settings.cpu_ram_size, CPURAMSize::Expanded16MBForced);
}

TEST(Cheats, RAMSizeOverrideNamesRoundTrip)
{
  for (const CPURAMSize size :
       {CPURAMSize::Expanded4MB, CPURAMSize::Expanded4MBForced, CPURAMSize::Expanded8MB, CPURAMSize::Expanded8MBForced,
        CPURAMSize::Expanded16MB, CPURAMSize::Expanded16MBForced})
  {
    SCOPED_TRACE(Settings::GetCPURAMSizeName(size));
    const std::string input =
      fmt::format("[Expanded RAM]\nOverrideRAMSize = {}\n80010000 0001\n", Settings::GetCPURAMSizeName(size));
    Cheats::CodeInfoList codes;
    Error error;
    ASSERT_TRUE(Cheats::ImportCodesFromString(&codes, input, Cheats::FileFormat::DuckStation, true, &error))
      << error.GetDescription();
    ASSERT_EQ(codes.size(), 1u);
    EXPECT_EQ(codes.front().override_ram_size, size);

    const std::string formatted = Cheats::FormatCodeForFile(codes.front());
    Cheats::CodeInfoList reparsed;
    ASSERT_TRUE(Cheats::ImportCodesFromString(&reparsed, formatted, Cheats::FileFormat::DuckStation, true, &error))
      << error.GetDescription();
    ASSERT_EQ(reparsed.size(), 1u);
    EXPECT_EQ(reparsed.front().override_ram_size, size);
  }
}

TEST(Cheats, RAMSizeOverrideAliasesKeepLargestSize)
{
  // Use valid named sizes so a rejected numeric value cannot hide alias precedence problems.
  for (const std::string_view overrides :
       {"OverrideRAMSize = 16MB\nEnable8MBRAM = true\n", "Enable8MBRAM = true\nOverrideRAMSize = 16MB\n",
        "Enable16MBRAM = true\nEnable8MBRAM = true\n", "Enable16MBRAM = true\nOverrideRAMSize = 4MB\n"})
  {
    SCOPED_TRACE(overrides);
    const std::string input = fmt::format("[Expanded RAM]\n{}80010000 0001\n", overrides);
    Cheats::CodeInfoList codes;
    Error error;
    ASSERT_TRUE(Cheats::ImportCodesFromString(&codes, input, Cheats::FileFormat::DuckStation, true, &error))
      << error.GetDescription();
    ASSERT_EQ(codes.size(), 1u);
    EXPECT_EQ(codes.front().override_ram_size, CPURAMSize::Expanded16MB);
  }
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

TEST(Cheats, ForceFullRAMMappingAlias)
{
  // Old files have the mapping as a separate key after the size. Without a size there is nothing to force.
  static constexpr std::string_view input = "[Sixteen]\n"
                                            "OverrideRAMSize = 16MB\n"
                                            "ForceFullRAMMapping = true\n"
                                            "80010000 0001\n"
                                            "[Alias]\n"
                                            "Enable8MBRAM = true\n"
                                            "ForceFullRAMMapping = true\n"
                                            "80010000 0001\n"
                                            "[Larger]\n"
                                            "OverrideRAMSize = 4MB\n"
                                            "ForceFullRAMMapping = true\n"
                                            "Enable8MBRAM = true\n"
                                            "80010000 0001\n"
                                            "[Disabled]\n"
                                            "OverrideRAMSize = 8MB\n"
                                            "ForceFullRAMMapping = false\n"
                                            "80010000 0001\n"
                                            "[NoSize]\n"
                                            "ForceFullRAMMapping = true\n"
                                            "80010000 0001\n";

  Cheats::CodeInfoList codes;
  Error error;
  ASSERT_TRUE(Cheats::ImportCodesFromString(&codes, input, Cheats::FileFormat::DuckStation, true, &error))
    << error.GetDescription();
  ASSERT_EQ(codes.size(), 5u);
  EXPECT_EQ(codes[0].override_ram_size, std::make_optional(CPURAMSize::Expanded16MBForced));
  EXPECT_EQ(codes[1].override_ram_size, std::make_optional(CPURAMSize::Expanded8MBForced));
  EXPECT_EQ(codes[2].override_ram_size, std::make_optional(CPURAMSize::Expanded8MBForced));
  EXPECT_EQ(codes[3].override_ram_size, std::make_optional(CPURAMSize::Expanded8MB));
  EXPECT_FALSE(codes[4].override_ram_size.has_value());
}

TEST(Cheats, VRAMSizeOverride)
{
  const Settings old_settings = g_settings;
  const ScopedGuard restore_settings([&old_settings]() { g_settings = old_settings; });

  for (const GPUVRAMSize size : {GPUVRAMSize::Expanded2MB, GPUVRAMSize::Expanded2MBForced})
  {
    SCOPED_TRACE(Settings::GetGPUVRAMSizeName(size));
    Cheats::CodeMetadata metadata = MakeMetadata(Cheats::CodeType::Gameshark);
    metadata.override_vram_size = size;
    Error error;
    std::unique_ptr<Cheats::CheatCode> code = Cheats::ParseCode(std::move(metadata), "80010000 0001\n", &error);
    ASSERT_NE(code, nullptr) << error.GetDescription();
    EXPECT_TRUE(code->HasAnySettingOverrides());

    // Only ever increases the size.
    for (const GPUVRAMSize setting : {GPUVRAMSize::Default, GPUVRAMSize::Expanded2MB, GPUVRAMSize::Expanded2MBForced})
    {
      g_settings.gpu_vram_size = setting;
      code->ApplySettingOverrides();
      EXPECT_EQ(g_settings.gpu_vram_size, std::max(size, setting));
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
    CPU::CodeCache::Initialize();
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

  void TearDown() override
  {
    for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
      Pad::SetController(i, nullptr);
  }

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

  static u16 ReadHalf(u32 address)
  {
    u16 value;
    std::memcpy(&value, &g_bus.unprotected_ram[address], sizeof(value));
    return value;
  }

  static void WriteHalf(u32 address, u16 value) { std::memcpy(&g_bus.unprotected_ram[address], &value, sizeof(value)); }
  static u8 ReadByte(u32 address) { return g_bus.unprotected_ram[address]; }
  static void WriteByte(u32 address, u8 value) { g_bus.unprotected_ram[address] = value; }

  static void Run(std::string_view input)
  {
    std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse(input);
    ASSERT_NE(code, nullptr) << input;
    code->Apply();
  }

  // The cheat registers are global and only reset on shutdown, so tests set every register they read.
  static void SetCheatRegister(u8 reg, u32 value) { Run(fmt::format("518500{:02X} {:08X}\n", reg, value)); }
  static u32 GetCheatRegister(u8 reg)
  {
    Run(fmt::format("518000{:02X} {:08X}\n", reg, REGISTER_READBACK_ADDRESS));
    return ReadWord(REGISTER_READBACK_ADDRESS);
  }

  static constexpr u32 REGISTER_READBACK_ADDRESS = 0x7F00;
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

TEST_F(CheatsApply, BlockWriteThroughRAMMirrors)
{
  for (const u32 mirror : {0u, 0x200000u, 0x400000u, 0x600000u})
  {
    SCOPED_TRACE(mirror);
    WriteWord(0x2000, 0);
    WriteWord(0x2004, 0);
    std::unique_ptr<Cheats::GamesharkCheatCode> code =
      Parse(fmt::format("{:08X} 00000002\n11223344 55667788\n", 0xA9002000u | mirror));
    ASSERT_NE(code, nullptr);
    code->Apply();
    EXPECT_EQ(ReadWord(0x2000), 0x11223344u);
    EXPECT_EQ(ReadWord(0x2004), 0x55667788u);
  }
}

TEST_F(CheatsApply, ConditionalBlockWritesThroughRAMMirrors)
{
  for (const u32 opcode : {0xAA000000u, 0xAB000000u})
  {
    for (const u32 mirror : {0u, 0x200000u, 0x400000u, 0x600000u})
    {
      SCOPED_TRACE(opcode | mirror);
      WriteWord(0x2000, 0x11111111);
      WriteWord(0x2004, 0x22222223);
      std::unique_ptr<Cheats::GamesharkCheatCode> code =
        Parse(fmt::format("{:08X} 00000002\n11111111 AAAAAAAA\n22222222 BBBBBBBB\n", opcode | mirror | 0x2000));
      ASSERT_NE(code, nullptr);
      code->Apply();
      EXPECT_EQ(ReadWord(0x2000), 0x11111111u);
      EXPECT_EQ(ReadWord(0x2004), 0x22222223u);

      WriteWord(0x2004, 0x22222222);
      code->Apply();
      EXPECT_EQ(ReadWord(0x2000), 0xAAAAAAAAu);
      EXPECT_EQ(ReadWord(0x2004), 0xBBBBBBBBu);
      code->ApplyOnDisable(nullptr);
      const bool restores = (opcode == 0xAA000000u);
      EXPECT_EQ(ReadWord(0x2000), restores ? 0x11111111u : 0xAAAAAAAAu);
      EXPECT_EQ(ReadWord(0x2004), restores ? 0x22222222u : 0xBBBBBBBBu);
    }
  }
}

TEST_F(CheatsApply, MergedWritesRespectShrunkRAMWindow)
{
  const auto set_window = [](u32 value) {
    CPU::GetMemoryWriteHandler(0xBF801060, MemoryAccessSize::Word)(0xBF801060, value);
  };
  const ScopedGuard restore_window([&set_window]() { set_window(0xB88); });
  std::unique_ptr<Cheats::GamesharkCheatCode> code =
    Parse("80180000 1111\n80180002 2222\n80180004 3333\n80180006 4444\n");
  ASSERT_NE(code, nullptr);
  ASSERT_EQ(code->GetInstructions().front().code, Cheats::GamesharkCheatCode::InstructionCode::InternalMaskedWrite);

  set_window(0x088);
  ASSERT_EQ(g_bus.ram_mapped_size, 1048576u);
  code->Apply();
  EXPECT_EQ(ReadWord(0x180000), 0u);
  EXPECT_EQ(ReadWord(0x180004), 0u);
}

TEST_F(CheatsApply, MergedWritesSplitAtOneMBWindowBoundary)
{
  const auto set_window = [](u32 value) {
    CPU::GetMemoryWriteHandler(0xBF801060, MemoryAccessSize::Word)(0xBF801060, value);
  };
  const ScopedGuard restore_window([&set_window]() { set_window(0xB88); });
  std::unique_ptr<Cheats::GamesharkCheatCode> code =
    Parse("800FFFFC 1111\n800FFFFE 2222\n80100000 3333\n80100002 4444\n");
  ASSERT_NE(code, nullptr);

  // Writes below the end of a 1MB window must still apply.
  set_window(0x088);
  ASSERT_EQ(g_bus.ram_mapped_size, 1048576u);
  code->Apply();
  EXPECT_EQ(ReadWord(0xFFFFC), 0x22221111u);
  EXPECT_EQ(ReadWord(0x100000), 0u);
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

TEST_F(CheatsApply, BlockConditionalSkipsMergedRestorePayload)
{
  // Four A8 lines merge into an FE whose first payload line has the same bits as a separator.
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("C0003000 00001234\n"
                                                           "A8002000 0000FF00\n"
                                                           "A8002001 0000FF00\n"
                                                           "A8002002 00000000\n"
                                                           "A8002003 00000000\n"
                                                           "00000001 0000FFFF\n"
                                                           "90001000 00000001\n"
                                                           "00000000 0000FFFF\n"
                                                           "90001010 00000002\n");
  ASSERT_NE(code, nullptr);
  const auto instructions = code->GetInstructions();
  ASSERT_GE(instructions.size(), 3u);
  ASSERT_EQ(instructions[1].code, Cheats::GamesharkCheatCode::InstructionCode::InternalMatchWriteWithRestore8);
  ASSERT_EQ(instructions[2].bits, 0xFFFFu);
  WriteWord(0x2000, 0x0000FFFF);

  code->Apply();
  EXPECT_EQ(ReadWord(0x1000), 0u);
  EXPECT_EQ(ReadWord(0x1010), 2u);
  EXPECT_EQ(ReadWord(0x2000), 0xFFFFu);

  WriteHalf(0x3000, 0x1234);
  code->Apply();
  EXPECT_EQ(ReadWord(0x1000), 1u);
  EXPECT_EQ(ReadWord(0x2000), 0u);

  WriteHalf(0x3000, 0);
  code->ApplyOnDisable(nullptr);
  EXPECT_EQ(ReadWord(0x2000), 0xFFFFu);
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
  // Normal boot parses codes before Bus::Initialize(), so merging must also work without an active RAM allocation.
  ASSERT_EQ(g_bus.ram_size, 0u);
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

// Interpreter coverage, one or more tests per opcode. Fewer than four adjacent writes are used where merging into
// FB-FE would otherwise change the instruction under test.

TEST_F(CheatsApply, ConstantWrites)
{
  WriteWord(0x1000, 0xAAAAAAAA);
  WriteWord(0x1010, 0xAAAAAAAA);
  WriteWord(0x1020, 0xAAAAAAAA);

  // Value bits above the write width are ignored.
  Run("30001001 FFFFFF12\n"
      "80001012 FFFF3456\n"
      "90001020 89ABCDEF\n");
  EXPECT_EQ(ReadWord(0x1000), 0xAAAA12AAu);
  EXPECT_EQ(ReadWord(0x1010), 0x3456AAAAu);
  EXPECT_EQ(ReadWord(0x1020), 0x89ABCDEFu);
}

TEST_F(CheatsApply, ScratchpadWrites)
{
  const auto read_scratchpad = [](u32 offset) {
    u32 value = 0;
    EXPECT_TRUE(CPU::SafeReadMemoryWord(CPU::SCRATCHPAD_ADDR | offset, &value));
    return value;
  };
  for (const u32 offset : {0x10u, 0x20u, 0x3F0u})
    CPU::SafeWriteMemoryWord(CPU::SCRATCHPAD_ADDR | offset, 0xAAAAAAAAu);

  // The address is masked to the scratchpad size.
  Run("1F000010 FFFF1234\n"
      "A5000020 89ABCDEF\n"
      "1F123FF0 00005678\n");
  EXPECT_EQ(read_scratchpad(0x10), 0xAAAA1234u);
  EXPECT_EQ(read_scratchpad(0x20), 0x89ABCDEFu);
  EXPECT_EQ(read_scratchpad(0x3F0), 0xAAAA5678u);
  EXPECT_EQ(ReadWord(0x10), 0u);
}

TEST_F(CheatsApply, BitSetAndClear)
{
  for (const u32 address : {0x1000u, 0x1010u, 0x1020u, 0x1024u})
    WriteWord(address, 0x0F0F0F0F);

  Run("31001000 FFFFFFF0\n"
      "32001001 0000000F\n"
      "81001010 0000F000\n");
  Run("82001012 0000000F\n"
      "91001020 F0000000\n"
      "92001024 0000000F\n");
  EXPECT_EQ(ReadWord(0x1000), 0x0F0F00FFu);
  EXPECT_EQ(ReadWord(0x1010), 0x0F00FF0Fu);
  EXPECT_EQ(ReadWord(0x1020), 0xFF0F0F0Fu);
  EXPECT_EQ(ReadWord(0x1024), 0x0F0F0F00u);
}

TEST_F(CheatsApply, IncrementDecrement)
{
  WriteWord(0x1000, 0x1234FFFF);
  WriteWord(0x1010, 0x12340001);
  WriteWord(0x1020, 0x123455FF);
  WriteWord(0x1030, 0x12345500);
  WriteWord(0x1040, 0xFFFFFFFF);
  WriteWord(0x1050, 0x00000001);

  // All wrap, and don't touch the neighbouring bytes. Value bits above the width are ignored.
  Run("10001000 00010002\n"
      "11001010 00000002\n"
      "20001020 00000101\n");
  Run("21001030 00000001\n"
      "60001040 00000002\n"
      "61001050 00000002\n");
  EXPECT_EQ(ReadWord(0x1000), 0x12340001u);
  EXPECT_EQ(ReadWord(0x1010), 0x1234FFFFu);
  EXPECT_EQ(ReadWord(0x1020), 0x12345500u);
  EXPECT_EQ(ReadWord(0x1030), 0x123455FFu);
  EXPECT_EQ(ReadWord(0x1040), 0x00000001u);
  EXPECT_EQ(ReadWord(0x1050), 0xFFFFFFFFu);
}

TEST_F(CheatsApply, LineConditionals)
{
  struct Case
  {
    u8 opcode;
    u32 width;
    u32 memory;
    u32 compare;
    bool expected;
  };
  static constexpr std::array cases = {
    Case{0xD0, 2, 0x1234, 0x1234, true},
    Case{0xD0, 2, 0x1234, 0x1235, false},
    Case{0xD0, 2, 0x1234, 0xFFFF1234, true},
    Case{0xD1, 2, 0x1234, 0x1235, true},
    Case{0xD1, 2, 0x1234, 0x1234, false},
    Case{0xD2, 2, 0x1234, 0x1235, true},
    Case{0xD2, 2, 0x1234, 0x1234, false},
    Case{0xD2, 2, 0x8000, 0x7FFF, false},
    Case{0xD3, 2, 0x1235, 0x1234, true},
    Case{0xD3, 2, 0x1234, 0x1234, false},
    Case{0xD3, 2, 0x8000, 0x7FFF, true},
    Case{0xE0, 1, 0x34, 0x34, true},
    Case{0xE0, 1, 0x34, 0x35, false},
    Case{0xE0, 1, 0x34, 0xFFFFFF34, true},
    Case{0xE1, 1, 0x34, 0x35, true},
    Case{0xE1, 1, 0x34, 0x34, false},
    Case{0xE2, 1, 0x34, 0x35, true},
    Case{0xE2, 1, 0x34, 0x34, false},
    Case{0xE2, 1, 0x80, 0x7F, false},
    Case{0xE3, 1, 0x35, 0x34, true},
    Case{0xE3, 1, 0x34, 0x34, false},
    Case{0xE3, 1, 0x80, 0x7F, true},
    Case{0xA0, 4, 0x12345678, 0x12345678, true},
    Case{0xA0, 4, 0x12345678, 0x12345679, false},
    Case{0xA1, 4, 0x12345678, 0x12345679, true},
    Case{0xA1, 4, 0x12345678, 0x12345678, false},
    Case{0xA2, 4, 0x12345678, 0x12345679, true},
    Case{0xA2, 4, 0x12345678, 0x12345678, false},
    Case{0xA2, 4, 0x80000000, 0x7FFFFFFF, false},
    Case{0xA3, 4, 0x12345679, 0x12345678, true},
    Case{0xA3, 4, 0x12345678, 0x12345678, false},
    Case{0xA3, 4, 0x80000000, 0x7FFFFFFF, true},
  };

  for (const Case& c : cases)
  {
    SCOPED_TRACE(fmt::format("{:02X} {:X} {:X}", c.opcode, c.memory, c.compare));
    // Bytes above the width are set, so a wider read would fail the compare.
    const u32 width_mask = (c.width == 4) ? 0xFFFFFFFFu : ((1u << (c.width * 8)) - 1);
    WriteWord(0x3000, c.memory | ~width_mask);
    WriteWord(0x1000, 0);
    WriteWord(0x1010, 0);
    Run(fmt::format("{:02X}003000 {:08X}\n90001000 00000001\n90001010 00000002\n", c.opcode, c.compare));
    EXPECT_EQ(ReadWord(0x1000), c.expected ? 1u : 0u);
    EXPECT_EQ(ReadWord(0x1010), 2u);
  }
}

TEST_F(CheatsApply, LineConditionalSkipsPayload)
{
  // A false compare skips the whole of the next instruction. If only its first line was skipped, the payload line
  // would run as a 30 write to 0x1000.
  for (const std::string_view instruction : {
         "50000201 00000001\n30001000 00000011\n", // Slide
         "53000002 00010001\n30001000 00000011\n", // ExtImprovedSlide
         "C2001200 00000004\n30001000 00000011\n", // MemoryCopy
         "F3001200 00800010\n30001000 00000011\n", // ExtConstantForceRange16
         "F4001200 00EE0000\n30001000 00000011\n30001000 00000011\n30001000 00000011\n"
         "30001000 00000011\n",                    // ExtFindAndReplace
         "A9001200 00000002\n30001000 00000011\n", // ExtBlockWrite
         "AB001200 00000001\n30001000 00000011\n", // ExtBlockWriteIfMatch
       })
  {
    SCOPED_TRACE(instruction);
    std::memset(g_bus.unprotected_ram, 0, 0x2000);
    Run(fmt::format("D0003000 00001234\n{}90001100 00000099\n", instruction));
    EXPECT_EQ(ReadWord(0x1000), 0u);
    EXPECT_EQ(ReadWord(0x1200), 0u);
    EXPECT_EQ(ReadWord(0x1100), 0x99u);
  }
}

TEST_F(CheatsApply, ChainedLineConditionals)
{
  static constexpr std::string_view code = "D0003000 00001234\n"
                                           "D0003002 00005678\n"
                                           "90001000 00000001\n"
                                           "90001010 00000002\n";
  WriteHalf(0x3000, 0x1234);
  WriteHalf(0x3002, 0x5678);
  Run(code);
  EXPECT_EQ(ReadWord(0x1000), 1u);
  EXPECT_EQ(ReadWord(0x1010), 2u);

  // Either failing skips the write, but nothing after it.
  for (const u32 address : {0x3000u, 0x3002u})
  {
    SCOPED_TRACE(address);
    WriteWord(0x1000, 0);
    WriteWord(0x1010, 0);
    WriteHalf(0x3000, 0x1234);
    WriteHalf(0x3002, 0x5678);
    WriteHalf(address, 0);
    Run(code);
    EXPECT_EQ(ReadWord(0x1000), 0u);
    EXPECT_EQ(ReadWord(0x1010), 2u);
  }
}

TEST_F(CheatsApply, LineConditionalAtEnd)
{
  for (const u16 value : {u16(0), u16(0x1234)})
  {
    WriteHalf(0x3000, value);
    WriteWord(0x1000, 0);
    Run("90001000 00000001\nD0003000 00001234\n");
    EXPECT_EQ(ReadWord(0x1000), 1u);
  }
}

TEST_F(CheatsApply, LineConditionalBeforeBlockConditional)
{
  // A false line compare only skips the C0 line, so the block body runs.
  WriteHalf(0x3010, 0);
  Run("D0003000 00001234\n"
      "C0003010 00005555\n"
      "90001000 00000001\n"
      "00000000 0000FFFF\n"
      "90001010 00000002\n");
  EXPECT_EQ(ReadWord(0x1000), 1u);
  EXPECT_EQ(ReadWord(0x1010), 2u);
}

TEST_F(CheatsApply, BlockConditionals)
{
  struct Case
  {
    u8 opcode;
    u32 width;
    u32 memory;
    u32 compare;
    bool expected;
  };
  static constexpr std::array cases = {
    Case{0xC0, 2, 0x1234, 0x1234, true},
    Case{0xC0, 2, 0x1234, 0x1235, false},
    Case{0xC0, 2, 0x1234, 0xFFFF1234, true},
    Case{0xA4, 4, 0x12345678, 0x12345678, true},
    Case{0xA4, 4, 0x12345678, 0x12345679, false},
    Case{0xA4, 4, 0x12345678, 0x00005678, false},
    Case{0xC3, 1, 0x05, 0x06, true},
    Case{0xC3, 1, 0x06, 0x06, false},
    Case{0xC3, 1, 0x80, 0x7F, false},
    Case{0xC4, 1, 0x07, 0x06, true},
    Case{0xC4, 1, 0x06, 0x06, false},
    Case{0xC4, 1, 0x80, 0x7F, true},
    Case{0xC5, 2, 0x1233, 0x1234, true},
    Case{0xC5, 2, 0x1234, 0x1234, false},
    Case{0xC5, 2, 0x8000, 0x7FFF, false},
    Case{0xC6, 2, 0x1235, 0x1234, true},
    Case{0xC6, 2, 0x1234, 0x1234, false},
    Case{0xC6, 2, 0x8000, 0x7FFF, true},
  };

  for (const Case& c : cases)
  {
    SCOPED_TRACE(fmt::format("{:02X} {:X} {:X}", c.opcode, c.memory, c.compare));
    const u32 width_mask = (c.width == 4) ? 0xFFFFFFFFu : ((1u << (c.width * 8)) - 1);
    WriteWord(0x3000, c.memory | ~width_mask);
    std::memset(&g_bus.unprotected_ram[0x1000], 0, 0x100);
    Run(fmt::format("{:02X}003000 {:08X}\n"
                    "90001000 00000001\n"
                    "90001004 00000002\n"
                    "00000000 0000FFFF\n"
                    "90001010 00000003\n",
                    c.opcode, c.compare));
    EXPECT_EQ(ReadWord(0x1000), c.expected ? 1u : 0u);
    EXPECT_EQ(ReadWord(0x1004), c.expected ? 2u : 0u);
    EXPECT_EQ(ReadWord(0x1010), 3u);
  }
}

TEST_F(CheatsApply, BlockConditionalWithoutSeparator)
{
  Run("C0003000 00001234\n"
      "90001000 00000001\n"
      "90001010 00000002\n");
  EXPECT_EQ(ReadWord(0x1000), 0u);
  EXPECT_EQ(ReadWord(0x1010), 0u);
}

TEST_F(CheatsApply, BlockConditionalsDoNotNest)
{
  // The first separator ends both blocks.
  static constexpr std::string_view code = "C0003000 00001111\n"
                                           "C0003002 00002222\n"
                                           "90001000 00000001\n"
                                           "00000000 0000FFFF\n"
                                           "90001010 00000002\n"
                                           "00000000 0000FFFF\n"
                                           "90001020 00000003\n";
  for (const auto& [outer, inner, expected] :
       {std::tuple{0x1111, 0x2222, 1u}, std::tuple{0x1111, 0, 0u}, std::tuple{0, 0x2222, 0u}, std::tuple{0, 0, 0u}})
  {
    SCOPED_TRACE(fmt::format("{:X} {:X}", outer, inner));
    std::memset(&g_bus.unprotected_ram[0x1000], 0, 0x100);
    WriteHalf(0x3000, static_cast<u16>(outer));
    WriteHalf(0x3002, static_cast<u16>(inner));
    Run(code);
    EXPECT_EQ(ReadWord(0x1000), expected);
    EXPECT_EQ(ReadWord(0x1010), 2u);
    EXPECT_EQ(ReadWord(0x1020), 3u);
  }
}

TEST_F(CheatsApply, BlockConditionalSkipsSlidePayload)
{
  // The slide's second line looks like a separator, but is skipped as part of the slide.
  Run("C0003000 00001234\n"
      "50000201 00000001\n"
      "00000000 0000FFFF\n"
      "90001004 00000001\n"
      "00000000 0000FFFF\n"
      "90001010 00000002\n");
  EXPECT_EQ(ReadWord(0x1004), 0u);
  EXPECT_EQ(ReadWord(0x1010), 2u);
}

TEST_F(CheatsApply, ButtonConditionals)
{
  using Button = DigitalController::Button;

  // PSX button bit order to GameShark button bits.
  static constexpr std::array<u16, 16> mapping = {0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000,
                                                  0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080};

  const auto check = [](u16 value) {
    std::memset(&g_bus.unprotected_ram[0x1000], 0, 0x100);
    Run(fmt::format("D4000000 {0:08X}\n"
                    "90001000 00000001\n"
                    "D5000000 {0:08X}\n"
                    "90001010 00000001\n"
                    "00000000 0000FFFF\n"
                    "D6000000 {0:08X}\n"
                    "90001020 00000001\n"
                    "00000000 0000FFFF\n",
                    value));
    const bool d4 = (ReadWord(0x1000) != 0), d5 = (ReadWord(0x1010) != 0), d6 = (ReadWord(0x1020) != 0);
    EXPECT_EQ(d4, d5);
    EXPECT_NE(d5, d6);
    return d4;
  };

  // No controllers, no buttons.
  EXPECT_TRUE(check(0x0000));
  EXPECT_FALSE(check(0x0100));

  Pad::SetController(0, DigitalController::Create(0, ControllerType::DigitalController));
  Controller* const pad = Pad::GetController(0);
  EXPECT_TRUE(check(0x0000));
  for (u32 i = 0; i < static_cast<u32>(Button::Count); i++)
  {
    SCOPED_TRACE(i);
    pad->SetBindState(i, 1.0f);
    EXPECT_TRUE(check(mapping[i]));
    EXPECT_FALSE(check(0x0000));
    pad->SetBindState(i, 0.0f);
  }

  // Must match exactly, and buttons from all controllers are combined.
  pad->SetBindState(static_cast<u32>(Button::Select), 1.0f);
  pad->SetBindState(static_cast<u32>(Button::Cross), 1.0f);
  EXPECT_TRUE(check(0x0140));
  EXPECT_FALSE(check(0x0100));
  EXPECT_FALSE(check(0x0141));
  Pad::SetController(1, DigitalController::Create(1, ControllerType::DigitalController));
  Pad::GetController(1)->SetBindState(static_cast<u32>(Button::L2), 1.0f);
  EXPECT_TRUE(check(0x0141));
}

TEST_F(CheatsApply, BitCompareButtons)
{
  using Button = DigitalController::Button;
  static constexpr u8 REG = 0xE0;

  Pad::SetController(0, DigitalController::Create(0, ControllerType::DigitalController));
  Controller* const pad = Pad::GetController(0);
  const auto set_select = [pad](bool pressed) {
    pad->SetBindState(static_cast<u32>(Button::Select), pressed ? 1.0f : 0.0f);
  };

  // Returns whether the block ran.
  const auto run = [](u32 mode, u32 check_value) {
    WriteWord(0x1000, 0);
    WriteWord(0x1010, 0);
    Run(fmt::format("{:08X} {:02X}{:06X}\n"
                    "90001000 00000001\n"
                    "00000000 0000FFFF\n"
                    "90001010 00000002\n",
                    0xD7000000u | mode, REG, check_value));
    EXPECT_EQ(ReadWord(0x1010), 2u);
    return (ReadWord(0x1000) != 0);
  };

  // Mode 0, bits set: runs every frame the buttons are held. Without a frame comparison, the register is untouched.
  SetCheatRegister(REG, 7);
  set_select(true);
  EXPECT_TRUE(run(0x000000, 0x000100));
  EXPECT_TRUE(run(0x000000, 0x000100));
  EXPECT_EQ(GetCheatRegister(REG), 7u);
  EXPECT_FALSE(run(0x000000, 0x000140));
  EXPECT_EQ(GetCheatRegister(REG), 7u);
  set_select(false);

  // Bits clear.
  EXPECT_TRUE(run(0x100000, 0x000100));
  EXPECT_EQ(GetCheatRegister(REG), 7u);
  set_select(true);
  EXPECT_FALSE(run(0x100000, 0x000100));
  EXPECT_EQ(GetCheatRegister(REG), 7u);

  // So a mode 0 line for other buttons doesn't disturb a frame count sharing its register.
  SetCheatRegister(REG, 0);
  for (u32 frame = 0; frame < 3; frame++)
  {
    WriteWord(0x1000, 0);
    Run(fmt::format("D7000000 {0:02X}000040\n00000000 0000FFFF\nD7010001 {0:02X}000100\n90001000 00000001\n", REG));
    EXPECT_EQ(ReadWord(0x1000), (frame == 0) ? 1u : 0u) << frame;
  }
  EXPECT_EQ(GetCheatRegister(REG), 3u);

  // Frame count comparisons against the low 16 bits, on frames 1-5.
  struct Case
  {
    u32 mode;
    std::array<bool, 5> expected;
  };
  static constexpr std::array cases = {
    Case{0x010003, {false, false, true, false, false}},  // ==
    Case{0x020003, {true, true, false, false, false}},   // <
    Case{0x030003, {false, false, false, true, true}},   // >
    Case{0x040003, {true, true, false, true, true}},     // !=
    Case{0x050003, {false, false, false, false, false}}, // invalid
    Case{0x110003, {false, false, true, false, false}},  // == with bits clear
  };
  for (const Case& c : cases)
  {
    SCOPED_TRACE(fmt::format("{:06X}", c.mode));
    SetCheatRegister(REG, 0);
    const bool bits_clear = (c.mode & 0x100000u) != 0;
    set_select(!bits_clear);
    for (u32 frame = 0; frame < c.expected.size(); frame++)
      EXPECT_EQ(run(c.mode, 0x000100), c.expected[frame]) << frame;
    EXPECT_EQ(GetCheatRegister(REG), 5u);

    // Releasing resets the count.
    set_select(bits_clear);
    EXPECT_FALSE(run(c.mode, 0x000100));
    EXPECT_EQ(GetCheatRegister(REG), 0u);
  }
}

TEST_F(CheatsApply, BitCompareButtonsAnalogSticks)
{
  using HalfAxis = AnalogController::HalfAxis;
  static constexpr u8 REG = 0xE1;

  Pad::SetController(0, AnalogController::Create(0));
  Controller* const pad = Pad::GetController(0);
  const auto run = [](u32 check_value) {
    WriteWord(0x1000, 0);
    Run(fmt::format("D7000000 {:02X}{:06X}\n90001000 00000001\n00000000 0000FFFF\n", REG, check_value));
    return (ReadWord(0x1000) != 0);
  };

  static constexpr std::array<std::pair<HalfAxis, u32>, 8> directions = {{
    {HalfAxis::RUp, 0x010000},
    {HalfAxis::RRight, 0x020000},
    {HalfAxis::RDown, 0x040000},
    {HalfAxis::RLeft, 0x080000},
    {HalfAxis::LUp, 0x100000},
    {HalfAxis::LRight, 0x200000},
    {HalfAxis::LDown, 0x400000},
    {HalfAxis::LLeft, 0x800000},
  }};

  // Centred sticks set no bits.
  for (const auto& [axis, bit] : directions)
    EXPECT_FALSE(run(bit)) << bit;

  for (const auto& [axis, bit] : directions)
  {
    SCOPED_TRACE(bit);
    const u32 bind = static_cast<u32>(AnalogController::Button::Count) + static_cast<u32>(axis);
    pad->SetBindState(bind, 1.0f);
    EXPECT_TRUE(run(bit));
    for (const auto& [other_axis, other_bit] : directions)
    {
      if (other_bit != bit)
        EXPECT_FALSE(run(other_bit)) << other_bit;
    }
    pad->SetBindState(bind, 0.0f);
  }

  // Combined with buttons.
  pad->SetBindState(static_cast<u32>(AnalogController::Button::Cross), 1.0f);
  pad->SetBindState(static_cast<u32>(AnalogController::Button::Count) + static_cast<u32>(HalfAxis::LUp), 1.0f);
  EXPECT_TRUE(run(0x100040));
  EXPECT_FALSE(run(0x200040));

  // A neGcon at rest sets no bits, since only its steering is reported.
  Pad::SetController(0, NeGcon::Create(0));
  for (const auto& [axis, bit] : directions)
    EXPECT_FALSE(run(bit)) << bit;
}

TEST_F(CheatsApply, MultiConditionals)
{
  WriteHalf(0x3000, 0x1234);
  WriteByte(0x3004, 0x5A);
  WriteWord(0x3008, 0x80000000);

  // Each condition with a passing and a failing value.
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 15> conditions = {{
    {"D0003000 00001234", "D0003000 00001235"},
    {"D1003000 00001235", "D1003000 00001234"},
    {"D2003000 00001235", "D2003000 00001234"},
    {"D3003000 00001233", "D3003000 00001234"},
    {"E0003004 0000005A", "E0003004 0000005B"},
    {"E1003004 0000005B", "E1003004 0000005A"},
    {"E2003004 0000005B", "E2003004 0000005A"},
    {"E3003004 00000059", "E3003004 0000005A"},
    {"A0003008 80000000", "A0003008 7FFFFFFF"},
    {"A1003008 7FFFFFFF", "A1003008 80000000"},
    {"A2003008 80000001", "A2003008 80000000"},
    {"A3003008 7FFFFFFF", "A3003008 80000000"},
    {"E4003004 0000000A", "E4003004 00000005"}, // bits set
    {"E5003004 00000005", "E5003004 0000000A"}, // bits clear
    {"D7000000 E2000000", "D7100000 E2000000"}, // no buttons, check value 0
  }};

  // AND alongside a passing condition, OR alongside a failing one. Either way, the code continues after the separator.
  for (const auto& [type, other] : {std::pair{0u, "D0003100 00000000"}, std::pair{1u, "D0003100 00000001"}})
  {
    for (const auto& [pass, fail] : conditions)
    {
      for (const bool expected : {true, false})
      {
        const std::string_view condition = expected ? pass : fail;
        SCOPED_TRACE(fmt::format("{} {}", type, condition));
        WriteWord(0x1000, 0);
        WriteWord(0x1010, 0);
        Run(fmt::format("F60000{:02X} 1F000002\n{}\n{}\n90001000 00000001\n00000000 0000FFFF\n90001010 00000002\n",
                        type, other, condition));
        EXPECT_EQ(ReadWord(0x1000), expected ? 1u : 0u);
        EXPECT_EQ(ReadWord(0x1010), 2u);

        // Swapping the order makes no difference.
        WriteWord(0x1000, 0);
        WriteWord(0x1010, 0);
        Run(fmt::format("F60000{:02X} 1F000002\n{}\n{}\n90001000 00000001\n00000000 0000FFFF\n90001010 00000002\n",
                        type, condition, other));
        EXPECT_EQ(ReadWord(0x1000), expected ? 1u : 0u);
        EXPECT_EQ(ReadWord(0x1010), 2u);
      }
    }
  }

  // A passing F6 continues after the separator.
  WriteWord(0x1000, 0);
  Run("F6000000 1F000001\nD0003000 00001234\n90001000 00000001\n00000000 0000FFFF\n90001010 00000002\n");
  EXPECT_EQ(ReadWord(0x1000), 1u);
  EXPECT_EQ(ReadWord(0x1010), 2u);

  // D7 conditions only count frames in the register when comparing them, with AND and OR.
  static constexpr u8 REG = 0xE3;
  for (const u32 type : {0u, 1u})
  {
    SCOPED_TRACE(type);
    SetCheatRegister(REG, 7);
    Run(fmt::format("F60000{0:02X} 1F000001\nD7100000 {1:02X}000100\n00000000 0000FFFF\n"
                    "F60000{0:02X} 1F000001\nD7000000 {1:02X}000100\n00000000 0000FFFF\n",
                    type, REG));
    EXPECT_EQ(GetCheatRegister(REG), 7u);
    Run(fmt::format("F60000{:02X} 1F000001\nD7110001 {:02X}000100\n00000000 0000FFFF\n", type, REG));
    EXPECT_EQ(GetCheatRegister(REG), 8u);
    Run(fmt::format("F60000{:02X} 1F000001\nD7010001 {:02X}000100\n00000000 0000FFFF\n", type, REG));
    EXPECT_EQ(GetCheatRegister(REG), 0u);
  }
}

TEST_F(CheatsApply, MultiConditionalEvaluatesAllFrameCounters)
{
  static constexpr u8 INCREMENT_REG = 0xE4;
  static constexpr u8 RESET_REG = 0xE5;
  for (const bool is_or : {false, true})
  {
    SCOPED_TRACE(is_or);
    SetCheatRegister(INCREMENT_REG, 7);
    SetCheatRegister(RESET_REG, 7);
    WriteWord(0x1000, 0);

    // The first condition decides the result. Both D7 conditions still update their registers.
    Run(fmt::format("F60000{:02X} 1F000003\n"
                    "D0003000 {:08X}\n"
                    "D7110008 {:02X}000100\n"
                    "D7010008 {:02X}000100\n"
                    "90001000 00000001\n"
                    "00000000 0000FFFF\n",
                    is_or ? 1u : 0u, is_or ? 0u : 1u, INCREMENT_REG, RESET_REG));
    EXPECT_EQ(ReadWord(0x1000), is_or ? 1u : 0u);
    EXPECT_EQ(GetCheatRegister(INCREMENT_REG), 8u);
    EXPECT_EQ(GetCheatRegister(RESET_REG), 0u);
  }
}

TEST_F(CheatsApply, MultiConditionalElse)
{
  // The else line directly follows the if body, without a separator.
  static constexpr std::string_view code = "F6000000 1F000001\n"
                                           "D0003000 00001234\n"
                                           "90001000 00000001\n"
                                           "F6000000 E15E0000\n"
                                           "90001010 00000002\n"
                                           "00000000 0000FFFF\n"
                                           "90001020 00000003\n";
  for (const auto& [value, expected] :
       {std::pair{u16(0x1234), std::array{1u, 0u, 3u}}, std::pair{u16(0), std::array{0u, 2u, 3u}}})
  {
    SCOPED_TRACE(value);
    std::memset(&g_bus.unprotected_ram[0x1000], 0, 0x100);
    WriteHalf(0x3000, value);
    Run(code);
    EXPECT_EQ(ReadWord(0x1000), expected[0]);
    EXPECT_EQ(ReadWord(0x1010), expected[1]);
    EXPECT_EQ(ReadWord(0x1020), expected[2]);
  }
}

TEST_F(CheatsApply, MultiConditionalElseIf)
{
  static constexpr std::string_view code = "F6000000 1F000001\n"
                                           "D0003000 00001234\n"
                                           "90001000 00000001\n"
                                           "F6000000 E15E1F01\n"
                                           "D0003000 00005678\n"
                                           "90001010 00000002\n"
                                           "F6000000 E15E0000\n"
                                           "90001020 00000003\n"
                                           "00000000 0000FFFF\n"
                                           "90001030 00000004\n";
  for (const auto& [value, expected] :
       {std::pair{u16(0x1234), std::array{1u, 0u, 0u, 4u}}, std::pair{u16(0x5678), std::array{0u, 2u, 0u, 4u}},
        std::pair{u16(0), std::array{0u, 0u, 3u, 4u}}})
  {
    SCOPED_TRACE(value);
    std::memset(&g_bus.unprotected_ram[0x1000], 0, 0x100);
    WriteHalf(0x3000, value);
    Run(code);
    EXPECT_EQ(ReadWord(0x1000), expected[0]);
    EXPECT_EQ(ReadWord(0x1010), expected[1]);
    EXPECT_EQ(ReadWord(0x1020), expected[2]);
    EXPECT_EQ(ReadWord(0x1030), expected[3]);
  }
}

TEST_F(CheatsApply, MultiConditionalSkipsBranchMarkersInPayload)
{
  // These slide operands look like else and separator lines. Only the actual else body runs.
  Run("F6000000 1F000001\n"
      "D0003000 00001234\n"
      "50000201 00000001\n"
      "F6000000 E15E0000\n"
      "50000201 00000001\n"
      "00000000 0000FFFF\n"
      "90001000 00000001\n"
      "00000000 0000FFFF\n"
      "F6000000 E15E0000\n"
      "90001010 00000002\n"
      "00000000 0000FFFF\n"
      "90001020 00000003\n");
  EXPECT_EQ(ReadWord(0x1000), 0u);
  EXPECT_EQ(ReadWord(0x1010), 2u);
  EXPECT_EQ(ReadWord(0x1020), 3u);
}

TEST_F(CheatsApply, MultiConditionalElseIfChainFrameCounters)
{
  // Both else-if conditions share one counter. A branch that has already matched skips the later D7s.
  static constexpr std::string_view code = "F6000000 1F000001\n"
                                           "D0003000 00000001\n"
                                           "30001000 00000001\n"
                                           "00000000 0000FFFF\n"
                                           "F6000001 E15E1F02\n"
                                           "D0003000 00000002\n"
                                           "D7010002 E6000000\n"
                                           "30001001 00000002\n"
                                           "00000000 0000FFFF\n"
                                           "F6000000 E15E1F02\n"
                                           "D0003000 00000003\n"
                                           "D7010002 E6000000\n"
                                           "30001002 00000003\n"
                                           "00000000 0000FFFF\n"
                                           "F6000000 E15E0000\n"
                                           "30001003 00000004\n"
                                           "00000000 0000FFFF\n";
  static constexpr u8 COUNTER_REG = 0xE6;
  static constexpr std::array cases = {
    std::tuple{u16(1), 0x00000001u, 0u},
    std::tuple{u16(2), 0x00000200u, 1u},
    std::tuple{u16(3), 0x00030000u, 2u},
    std::tuple{u16(4), 0x04000000u, 2u},
  };
  for (const auto& [value, expected, counter] : cases)
  {
    SCOPED_TRACE(value);
    SetCheatRegister(COUNTER_REG, 0);
    WriteHalf(0x3000, value);
    WriteWord(0x1000, 0);
    Run(code);
    EXPECT_EQ(ReadWord(0x1000), expected);
    EXPECT_EQ(GetCheatRegister(COUNTER_REG), counter);
  }
}

TEST_F(CheatsApply, MultiConditionalWritesLikeElse)
{
  // Only F6 lines are else/else-if, so writes of the same values are skipped in a false body, and run after it.
  for (const u32 marker : {0xE15E0000u, 0xE15E1F01u})
  {
    SCOPED_TRACE(marker);
    std::memset(&g_bus.unprotected_ram[0x1000], 0, 0x100);
    Run(fmt::format("F6000000 1F000001\n"
                    "D0003000 00001234\n"
                    "90001000 {0:08X}\n"
                    "90001010 00000002\n"
                    "00000000 0000FFFF\n"
                    "90001020 00000003\n",
                    marker));
    EXPECT_EQ(ReadWord(0x1000), 0u);
    EXPECT_EQ(ReadWord(0x1010), 0u);
    EXPECT_EQ(ReadWord(0x1020), 3u);

    std::memset(&g_bus.unprotected_ram[0x1000], 0, 0x100);
    Run(fmt::format("F6000000 1F000001\n"
                    "D0003000 00001234\n"
                    "90001000 00000001\n"
                    "00000000 0000FFFF\n"
                    "90001010 {0:08X}\n"
                    "90001020 00000003\n",
                    marker));
    EXPECT_EQ(ReadWord(0x1000), 0u);
    EXPECT_EQ(ReadWord(0x1010), marker);
    EXPECT_EQ(ReadWord(0x1020), 3u);
  }
}

TEST_F(CheatsApply, MultiConditionalElseOutsideIf)
{
  // Else/else-if lines outside an if are skipped to the separator.
  for (const std::string_view line : {"F6000000 E15E0000", "F6000000 E15E1F01", "F6000000 12345678"})
  {
    SCOPED_TRACE(line);
    std::memset(&g_bus.unprotected_ram[0x1000], 0, 0x100);
    Run(fmt::format("{}\nD0003000 00000000\n90001000 00000001\n00000000 0000FFFF\n90001010 00000002\n", line));
    EXPECT_EQ(ReadWord(0x1000), 0u);
    EXPECT_EQ(ReadWord(0x1010), 2u);
  }
}

TEST_F(CheatsApply, MultiConditionalErrorsStopCode)
{
  // No conditions, an unknown type, or an unsupported condition stop the rest of the code.
  for (const std::string_view header :
       {"F6000000 1F000000\n", "F6000002 1F000001\nD0003000 00000000\n", "F6000000 1F000001\n80003000 00000000\n",
        "F6000001 1F000001\nC0003000 00000000\n"})
  {
    SCOPED_TRACE(header);
    std::memset(&g_bus.unprotected_ram[0x1000], 0, 0x100);
    Run(fmt::format("90001020 00000003\n{}90001000 00000001\n00000000 0000FFFF\n90001010 00000002\n", header));
    EXPECT_EQ(ReadWord(0x1000), 0u);
    EXPECT_EQ(ReadWord(0x1010), 0u);
    EXPECT_EQ(ReadWord(0x1020), 3u);
  }
}

TEST_F(CheatsApply, ForceRange)
{
  struct Case
  {
    std::string_view code;
    u32 width;
    u32 before;
    u32 after;
  };
  static constexpr std::array cases = {
    // F0: min 10, max 80, below 11, above EE.
    Case{"F0001000 EE118010", 1, 0x05, 0x11},
    Case{"F0001000 EE118010", 1, 0x90, 0xEE},
    Case{"F0001000 EE118010", 1, 0x50, 0x50},
    Case{"F0001000 EE118010", 1, 0x10, 0x10},
    Case{"F0001000 EE118010", 1, 0x80, 0x80},
    Case{"F0001000 EE118000", 1, 0x00, 0x00},
    // F1: clamp.
    Case{"F1001000 00800010", 2, 0x0005, 0x0010},
    Case{"F1001000 00800010", 2, 0x0090, 0x0080},
    Case{"F1001000 00800010", 2, 0x0050, 0x0050},
    Case{"F1001000 00800010", 2, 0x0010, 0x0010},
    Case{"F1001000 00800010", 2, 0x0080, 0x0080},
    Case{"F1001000 00800000", 2, 0x0000, 0x0000},
    Case{"F1001000 80000010", 2, 0x8001, 0x8000},
    // F2: roll round.
    Case{"F2001000 00800010", 2, 0x0005, 0x0080},
    Case{"F2001000 00800010", 2, 0x0090, 0x0010},
    Case{"F2001000 00800010", 2, 0x0050, 0x0050},
    Case{"F2001000 00800010", 2, 0x0010, 0x0010},
    Case{"F2001000 00800010", 2, 0x0080, 0x0080},
    // F3: second line holds the below/above values.
    Case{"F3001000 00800010\n00000000 EEEE1111", 2, 0x0005, 0x1111},
    Case{"F3001000 00800010\n00000000 EEEE1111", 2, 0x0090, 0xEEEE},
    Case{"F3001000 00800010\n00000000 EEEE1111", 2, 0x0050, 0x0050},
    Case{"F3001000 00800010\n00000000 EEEE1111", 2, 0x0010, 0x0010},
    Case{"F3001000 00800010\n00000000 EEEE1111", 2, 0x0080, 0x0080},
    // F5: swap between two values.
    Case{"F5001000 22221111", 2, 0x1111, 0x2222},
    Case{"F5001000 22221111", 2, 0x2222, 0x1111},
    Case{"F5001000 22221111", 2, 0x3333, 0x3333},
  };

  for (const Case& c : cases)
  {
    SCOPED_TRACE(fmt::format("{} {:X}", c.code, c.before));
    const u32 width_mask = (1u << (c.width * 8)) - 1;
    WriteWord(0x1000, (0xA5A5A5A5u & ~width_mask) | c.before);
    WriteWord(0x1100, 0);
    Run(fmt::format("{}\n90001100 00000099\n", c.code));
    EXPECT_EQ(ReadWord(0x1000), (0xA5A5A5A5u & ~width_mask) | c.after);
    EXPECT_EQ(ReadWord(0x1100), 0x99u);
  }
}

TEST_F(CheatsApply, FindAndReplace)
{
  static constexpr std::array<u8, 16> pattern = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                                 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
  static constexpr std::array<u8, 16> replacement = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
                                                     0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};

  // Searches 0x3FE0-0x4020 in steps of 2, with EE as the wildcard.
  static constexpr std::string_view code = "F4004000 00EE0010\n"
                                           "10111213 14151617\n"
                                           "18191A1B 1C1D1E1F\n"
                                           "A0A1A2A3 A4A5A6A7\n"
                                           "A8A9AAAB ACADAEAF\n"
                                           "90001100 00000099\n";

  const auto setup = [](u32 address) {
    std::memset(&g_bus.unprotected_ram[0x3F00], 0, 0x200);
    std::memcpy(&g_bus.unprotected_ram[address], pattern.data(), pattern.size());
    WriteWord(0x1100, 0);
  };
  const auto matches = [](u32 address, const std::array<u8, 16>& expected) {
    return std::memcmp(&g_bus.unprotected_ram[address], expected.data(), expected.size()) == 0;
  };

  for (const u32 address : {0x3FE0u, 0x3FF2u, 0x4000u, 0x4010u, 0x4020u})
  {
    SCOPED_TRACE(address);
    setup(address);
    Run(code);
    EXPECT_TRUE(matches(address, replacement));
    EXPECT_EQ(ReadWord(0x1100), 0x99u);
  }

  // Outside the window, or not 2-byte aligned with it.
  for (const u32 address : {0x3FDEu, 0x4022u, 0x4011u})
  {
    SCOPED_TRACE(address);
    setup(address);
    Run(code);
    EXPECT_TRUE(matches(address, pattern));
    EXPECT_EQ(ReadWord(0x1100), 0x99u);
  }

  // One byte different.
  setup(0x4000);
  WriteByte(0x400F, 0);
  Run(code);
  EXPECT_EQ(ReadByte(0x4000), 0x10u);

  // Wildcards in the search match anything, and in the replacement leave the byte alone.
  setup(0x4000);
  WriteByte(0x4002, 0x77);
  Run("F4004000 00EE0000\n"
      "1011EE13 14151617\n"
      "18191A1B 1C1D1EEE\n"
      "A0EEA2A3 A4A5A6A7\n"
      "A8A9AAAB ACADAEEE\n");
  EXPECT_EQ(ReadByte(0x4000), 0xA0u);
  EXPECT_EQ(ReadByte(0x4001), 0x11u);
  EXPECT_EQ(ReadByte(0x4002), 0xA2u);
  EXPECT_EQ(ReadByte(0x400E), 0xAEu);
  EXPECT_EQ(ReadByte(0x400F), 0x1Fu);

  // A window reaching below address 0 (0x20 - 0x22) starts at 0.
  std::memcpy(&g_bus.unprotected_ram[0x10], pattern.data(), pattern.size());
  Run("F4000020 00EE0011\n"
      "10111213 14151617\n"
      "18191A1B 1C1D1E1F\n"
      "A0A1A2A3 A4A5A6A7\n"
      "A8A9AAAB ACADAEAF\n");
  EXPECT_TRUE(matches(0x10, replacement));
}

TEST_F(CheatsApply, Slide)
{
  // 8-bit, wrapping the value at 0x100.
  Run("50000401 00000010\n30001000 000000F0\n");
  EXPECT_EQ(ReadWord(0x1000), 0x201000F0u);
  EXPECT_EQ(ReadWord(0x1004), 0u);

  // 16-bit, wrapping the value at 0x10000.
  Run("50000302 00000001\n80001010 0000FFFE\n");
  EXPECT_EQ(ReadWord(0x1010), 0xFFFFFFFEu);
  EXPECT_EQ(ReadWord(0x1014), 0x00000000u);
  EXPECT_EQ(ReadWord(0x1018), 0u);

  // 32-bit.
  Run("50000204 00000003\n90001020 00005678\n");
  EXPECT_EQ(ReadWord(0x1020), 0x00005678u);
  EXPECT_EQ(ReadWord(0x1024), 0x0000567Bu);
  EXPECT_EQ(ReadWord(0x1028), 0u);

  // A count of zero writes nothing, and other write types are skipped.
  WriteWord(0x1100, 0);
  Run("50000004 00000001\n90001030 00001234\n50000201 00000001\n10001040 00000001\n90001100 00000099\n");
  EXPECT_EQ(ReadWord(0x1030), 0u);
  EXPECT_EQ(ReadWord(0x1040), 0u);
  EXPECT_EQ(ReadWord(0x1100), 0x99u);
}

TEST_F(CheatsApply, ImprovedSlide)
{
  struct Case
  {
    u8 opcode;
    u32 value;
    std::array<u32, 3> expected;
  };
  static constexpr std::array cases = {
    Case{0x30, 0x12345610, {0xAAAAAA10, 0xAAAAAA11, 0xAAAAAA12}},
    Case{0x80, 0x12341234, {0xAAAA1234, 0xAAAA1235, 0xAAAA1236}},
    Case{0x90, 0x12345678, {0x12345678, 0x12345679, 0x1234567A}},
    Case{0x31, 0x00000001, {0xAAAAAAAB, 0xAAAAAAAA, 0xAAAAAAAB}},
    Case{0x32, 0x00000002, {0xAAAAAAA8, 0xAAAAAAA8, 0xAAAAAAAA}},
    Case{0x81, 0x00000101, {0xAAAAABAB, 0xAAAAABAA, 0xAAAAABAB}},
    Case{0x82, 0x00000002, {0xAAAAAAA8, 0xAAAAAAA8, 0xAAAAAAAA}},
    Case{0x91, 0x01000001, {0xABAAAAAB, 0xABAAAAAA, 0xABAAAAAB}},
    Case{0x92, 0x80000002, {0x2AAAAAA8, 0x2AAAAAA8, 0x2AAAAAAA}},
  };

  for (const Case& c : cases)
  {
    SCOPED_TRACE(fmt::format("{:02X}", c.opcode));
    for (const bool negative : {false, true})
    {
      SCOPED_TRACE(negative);
      for (u32 i = 0; i < 4; i++)
        WriteWord(0x1000 + i * 4, 0xAAAAAAAA);

      // Increasing addresses and values, or the same writes in reverse.
      if (!negative)
      {
        Run(fmt::format("53000003 00040001\n{:02X}001000 {:08X}\n", c.opcode, c.value));
        for (u32 i = 0; i < 3; i++)
          EXPECT_EQ(ReadWord(0x1000 + i * 4), c.expected[i]) << i;
      }
      else
      {
        Run(fmt::format("53110003 00040001\n{:02X}001008 {:08X}\n", c.opcode, c.value + 2));
        for (u32 i = 0; i < 3; i++)
        {
          // Bit set/clear values aren't symmetric, so only check the plain writes in reverse.
          if (c.opcode == 0x30 || c.opcode == 0x80 || c.opcode == 0x90)
            EXPECT_EQ(ReadWord(0x1000 + i * 4), c.expected[i]) << i;
        }
      }
      EXPECT_EQ(ReadWord(0x100C), 0xAAAAAAAAu);
    }
  }

  // Negative address with positive value, and the reverse.
  std::memset(&g_bus.unprotected_ram[0x1000], 0, 0x100);
  Run("53100003 00020010\n80001004 00001000\n");
  EXPECT_EQ(ReadWord(0x1000), 0x10101020u);
  EXPECT_EQ(ReadWord(0x1004), 0x00001000u);
  Run("53010003 00020010\n80001010 00001000\n");
  EXPECT_EQ(ReadWord(0x1010), 0x0FF01000u);
  EXPECT_EQ(ReadWord(0x1014), 0x00000FE0u);

  // Unsupported write types are skipped with their line.
  WriteWord(0x1100, 0);
  Run("53000002 00040001\n10001020 00000001\n90001100 00000099\n");
  EXPECT_EQ(ReadWord(0x1020), 0u);
  EXPECT_EQ(ReadWord(0x1100), 0x99u);
}

TEST_F(CheatsApply, ImprovedSlideReverseBitSetAndClear)
{
  struct Case
  {
    u8 opcode;
    u32 value;
    std::array<u32, 3> expected;
  };
  static constexpr std::array cases = {
    Case{0x31, 0x00000003, {0xAAAAAAAB, 0xAAAAAAAA, 0xAAAAAAAB}},
    Case{0x32, 0x00000004, {0xAAAAAAA8, 0xAAAAAAA8, 0xAAAAAAAA}},
    Case{0x81, 0x00000103, {0xAAAAABAB, 0xAAAAABAA, 0xAAAAABAB}},
    Case{0x82, 0x00000204, {0xAAAAA8A8, 0xAAAAA8A8, 0xAAAAA8AA}},
    Case{0x91, 0x01000003, {0xABAAAAAB, 0xABAAAAAA, 0xABAAAAAB}},
    Case{0x92, 0x80000004, {0x2AAAAAA8, 0x2AAAAAA8, 0x2AAAAAAA}},
  };
  for (const Case& c : cases)
  {
    SCOPED_TRACE(fmt::format("{:02X}", c.opcode));
    for (u32 i = 0; i < 4; i++)
      WriteWord(0x1000 + i * 4, 0xAAAAAAAA);

    Run(fmt::format("53110003 00040001\n{:02X}001008 {:08X}\n", c.opcode, c.value));
    for (u32 i = 0; i < c.expected.size(); i++)
      EXPECT_EQ(ReadWord(0x1000 + i * 4), c.expected[i]) << i;
    EXPECT_EQ(ReadWord(0x100C), 0xAAAAAAAAu);
  }
}

TEST_F(CheatsApply, ImprovedSlideFullCountAndSteps)
{
  // The count and both steps use 16 bits. The running write value wraps at 32 bits.
  Run("53000102 01040101\n90001000 FFFFFFFE\n");
  EXPECT_EQ(ReadWord(0x1000), 0xFFFFFFFEu);
  EXPECT_EQ(ReadWord(0x1004), 0u);
  EXPECT_EQ(ReadWord(0x1104), 0xFFu);
  EXPECT_EQ(ReadWord(0x11504), 0x101FFu);
  EXPECT_EQ(ReadWord(0x11608), 0u);
}

TEST_F(CheatsApply, MemoryCopy)
{
  WriteWord(0x1000, 0x44332211);
  WriteWord(0x1004, 0x88776655);

  // Only the low 16 bits are the count.
  Run("C2001000 FFFF0005\n00002000 00000000\n");
  EXPECT_EQ(ReadWord(0x2000), 0x44332211u);
  EXPECT_EQ(ReadWord(0x2004), 0x00000055u);

  // Copies forwards a byte at a time, so an overlapping copy repeats the first byte.
  Run("C2001000 00000004\n00001001 00000000\n");
  EXPECT_EQ(ReadWord(0x1000), 0x11111111u);
  EXPECT_EQ(ReadWord(0x1004), 0x88776611u);
}

TEST_F(CheatsApply, DelayActivation)
{
  // The frame number is zero, so any non-zero delay stops the code at the C1 line.
  ASSERT_EQ(System::GetFrameNumber(), 0u);
  Run("90001000 00000001\nC1000000 00000001\n90001010 00000002\n");
  EXPECT_EQ(ReadWord(0x1000), 1u);
  EXPECT_EQ(ReadWord(0x1010), 0u);
  Run("C1000000 00000000\n90001010 00000002\n");
  EXPECT_EQ(ReadWord(0x1010), 2u);
}

TEST_F(CheatsApply, UnknownInstructionsAreSkipped)
{
  // Unknown codes, and E4/E5 outside F6, are skipped.
  Run("40001000 00000001\n"
      "E4001000 00000001\n"
      "E5001000 00000001\n"
      "90001010 00000002\n");
  EXPECT_EQ(ReadWord(0x1000), 0u);
  EXPECT_EQ(ReadWord(0x1010), 2u);
}

TEST_F(CheatsApply, IncompleteInstructionsAreIgnored)
{
  for (const std::string_view line :
       {"50000201 00000001", "53000002 00040001", "C2001000 00000004", "F3001000 00800010"})
  {
    SCOPED_TRACE(line);
    WriteWord(0x1000, 0);
    Run(fmt::format("90001000 00000001\n{}\n", line));
    EXPECT_EQ(ReadWord(0x1000), 1u);
  }

  // F4 needs four lines after it.
  WriteWord(0x4000, 0x13121110);
  Run("F4004000 00EE0000\n10111213 EEEEEEEE\nEEEEEEEE EEEEEEEE\nA0A1A2A3 EEEEEEEE\n");
  EXPECT_EQ(ReadWord(0x4000), 0x13121110u);
}

TEST_F(CheatsApply, CheatRegisters)
{
  // 51 TT R2 R1 VVVVVVVV. Registers 0x10-0x1F are used here.
  const auto setup = [](std::initializer_list<std::pair<u8, u32>> registers) {
    std::memset(&g_bus.unprotected_ram[0x5000], 0, 0x200);
    for (u8 reg = 0x10; reg < 0x20; reg++)
      SetCheatRegister(reg, 0xCCCCCCCC);
    for (const auto& [reg, value] : registers)
      SetCheatRegister(reg, value);
  };

  // 8-bit.
  setup({{0x10, 0x1234ABCD}});
  Run("51000010 00005001\n");
  EXPECT_EQ(ReadWord(0x5000), 0x0000CD00u);

  setup({});
  WriteWord(0x5000, 0x123456EF);
  Run("51010010 00005000\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0xEFu);

  setup({{0x10, 0x5004}});
  Run("51020010 123456A5\n");
  EXPECT_EQ(ReadWord(0x5004), 0x000000A5u);

  setup({{0x11, 0x123401F0}});
  Run("51031110 00000005\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0xF5u);
  Run("51031110 00000020\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0x10u);

  setup({{0x10, 0x5008}, {0x11, 0x12345610}});
  Run("51041110 00000005\n");
  EXPECT_EQ(ReadWord(0x5008), 0x00000015u);

  setup({});
  Run("51050010 12345678\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0x78u);

  setup({{0x11, 0x5000}});
  WriteWord(0x5010, 0x1234569A);
  Run("51061110 00000010\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0x9Au);

  // Array write: base + size * (index - 1) + offset, guarded by size >= offset, index > 0 and a D0D0 marker.
  const auto array_write = [&setup](u32 size, u32 index, u32 marker, u32 offset, u8 reg = 0x10) {
    setup({});
    SetCheatRegister(reg, 0x5100);
    SetCheatRegister(reg + 1, size);
    SetCheatRegister(reg + 2, index);
    SetCheatRegister(reg + 3, marker);
    Run(fmt::format("510700{:02X} {:04X}12AB\n", reg, offset));
    Run(fmt::format("514700{:02X} {:04X}CDEF\n", reg, offset + 1));
  };
  array_write(0x10, 2, 0xD0D0, 3);
  EXPECT_EQ(ReadByte(0x5113), 0xABu);
  EXPECT_EQ(ReadHalf(0x5114), 0xCDEFu);
  EXPECT_EQ(ReadByte(0x5112), 0u);
  array_write(0x10, 2, 0xD0D0, 0x10);
  EXPECT_EQ(ReadByte(0x5120), 0xABu);
  array_write(0x10, 2, 0xD0D1, 3);
  EXPECT_EQ(ReadByte(0x5113), 0u);
  EXPECT_EQ(ReadHalf(0x5114), 0u);
  array_write(0x10, 0, 0xD0D0, 3);
  EXPECT_EQ(ReadByte(0x50F3), 0u);
  array_write(0x10, 2, 0xD0D0, 0x11);
  EXPECT_EQ(ReadByte(0x5121), 0u);
  EXPECT_EQ(ReadHalf(0x5122), 0u);

  // 16-bit.
  setup({{0x10, 0x1234ABCD}});
  Run("51400010 00005002\n");
  EXPECT_EQ(ReadWord(0x5000), 0xABCD0000u);

  setup({});
  WriteWord(0x5000, 0x123456EF);
  Run("51410010 00005000\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0x56EFu);

  setup({{0x10, 0x5004}});
  Run("51420010 123456A5\n");
  EXPECT_EQ(ReadWord(0x5004), 0x000056A5u);

  setup({{0x11, 0x1234F000}});
  Run("51431110 00000005\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0xF005u);
  Run("51431110 00001020\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0x0020u);

  setup({{0x10, 0x5008}, {0x11, 0x12345610}});
  Run("51441110 00000005\n");
  EXPECT_EQ(ReadWord(0x5008), 0x00005615u);

  setup({});
  Run("51450010 12345678\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0x5678u);

  setup({{0x11, 0x5000}});
  WriteWord(0x5010, 0x1234569A);
  Run("51461110 00000010\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0x569Au);

  // 32-bit.
  setup({{0x10, 0x1234ABCD}});
  Run("51800010 00005004\n");
  EXPECT_EQ(ReadWord(0x5004), 0x1234ABCDu);

  setup({});
  WriteWord(0x5000, 0x123456EF);
  Run("51810010 00005000\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0x123456EFu);

  setup({{0x10, 0x5004}});
  Run("51820010 123456A5\n");
  EXPECT_EQ(ReadWord(0x5004), 0x123456A5u);

  setup({{0x11, 0xFFFFFFFF}});
  Run("51831110 00000005\n");
  EXPECT_EQ(GetCheatRegister(0x10), 4u);

  setup({{0x10, 0x5008}, {0x11, 0x12345610}});
  Run("51841110 00000005\n");
  EXPECT_EQ(ReadWord(0x5008), 0x12345615u);

  setup({{0x11, 0x5000}});
  WriteWord(0x5010, 0x1234569A);
  Run("51861110 00000010\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0x1234569Au);

  // Arithmetic: R3 (value low byte) = R2 op R1, or R1 op R2-field for shifts.
  struct ArithmeticCase
  {
    u8 sub_type;
    u32 r1;
    u32 r2;
    u32 expected;
  };
  static constexpr std::array arithmetic = {
    ArithmeticCase{0xC0, 0x00000003, 0xFFFFFFFF, 0x00000002}, ArithmeticCase{0xC1, 0x00000003, 0x00000001, 0xFFFFFFFE},
    ArithmeticCase{0xC2, 0x00010001, 0x00010003, 0x00040003}, ArithmeticCase{0xC3, 0x00000003, 0x00000010, 0x00000005},
    ArithmeticCase{0xC3, 0x00000000, 0x00000010, 0x00000000}, ArithmeticCase{0xC4, 0x00000003, 0x00000010, 0x00000001},
    ArithmeticCase{0xC4, 0x00000000, 0x00000010, 0x00000010}, ArithmeticCase{0xC5, 0x0000FF0F, 0x00F0F0F0, 0x0000F000},
    ArithmeticCase{0xC6, 0x0000FF0F, 0x00F0F0F0, 0x00F0FFFF}, ArithmeticCase{0xC7, 0x0000FF0F, 0x00F0F0F0, 0x00F00FFF},
    ArithmeticCase{0xC8, 0x0000FF0F, 0x00F0F0F0, 0xFFFF00F0},
  };
  for (const ArithmeticCase& c : arithmetic)
  {
    SCOPED_TRACE(fmt::format("{:02X}", c.sub_type));
    setup({{0x10, c.r1}, {0x11, c.r2}});
    Run(fmt::format("51{:02X}1110 00000012\n", c.sub_type));
    EXPECT_EQ(GetCheatRegister(0x12), c.expected);
    EXPECT_EQ(GetCheatRegister(0x10), c.r1);
    EXPECT_EQ(GetCheatRegister(0x11), c.r2);
  }

  // Shifts use the R2 field as the amount.
  setup({{0x10, 0x80000001}});
  Run("51C90410 00000013\n51CA0410 00000014\n");
  EXPECT_EQ(GetCheatRegister(0x13), 0x00000010u);
  EXPECT_EQ(GetCheatRegister(0x14), 0x08000000u);
  for (const u32 amount : {31u, 32u, 33u, 255u})
  {
    SCOPED_TRACE(amount);
    Run(fmt::format("51C9{0:02X}10 00000013\n51CA{0:02X}10 00000014\n", amount));
    EXPECT_EQ(GetCheatRegister(0x13), (amount == 31) ? 0x80000000u : 0u);
    EXPECT_EQ(GetCheatRegister(0x14), (amount == 31) ? 1u : 0u);
  }

  // Immediate AND/OR/XOR into R1.
  setup({{0x11, 0x12345678}});
  Run("51D01110 0F0F00FF\n51D01112 FFFFFFFF\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0x02040078u);
  EXPECT_EQ(GetCheatRegister(0x12), 0x12345678u);
  EXPECT_EQ(GetCheatRegister(0x11), 0x12345678u);
  Run("51D11110 0F0F00FF\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0x1F3F56FFu);
  Run("51D21110 0F0F00FF\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0x1D3B5687u);
  Run("51D21010 FFFFFFFF\n");
  EXPECT_EQ(GetCheatRegister(0x10), 0xE2C4A978u);

  // Unknown sub-types do nothing.
  setup({{0x10, 0x5000}, {0x11, 0x5000}});
  Run("51081110 00005000\n513F1110 00005000\n51871110 00005000\n51CB1110 00000012\n51D31110 00005000\n"
      "51FF1110 00005000\n");
  for (u8 reg = 0x12; reg < 0x20; reg++)
    EXPECT_EQ(GetCheatRegister(reg), 0xCCCCCCCCu) << static_cast<u32>(reg);
  EXPECT_EQ(GetCheatRegister(0x10), 0x5000u);
  EXPECT_EQ(ReadWord(0x5000), 0u);
}

TEST_F(CheatsApply, CheatRegistersResetOnUnload)
{
  SetCheatRegister(0x40, 0x12345678);
  SetCheatRegister(0xFF, 0x12345678);
  Cheats::UnloadAll();
  EXPECT_EQ(GetCheatRegister(0x40), 0u);
  EXPECT_EQ(GetCheatRegister(0xFF), 0u);
}

TEST_F(CheatsApply, CheatRegisterCompare)
{
  // 52 TT R2 R1 VVVVVVVV, with R1 = 0x20 and R2 = 0x21. Sub-types repeat for 8/16/32-bit at +0x40.
  // Groups: 0x reg vs reg, 1x reg vs immediate, 2x memory at reg vs memory at reg, 3x reg vs memory at immediate.
  static constexpr u32 MEM_A = 0x6000, MEM_B = 0x6010, MEM_V = 0x6020;
  struct Case
  {
    u8 sub_type;
    u32 x; // 0x: R2, 1x/3x: R1, 2x: memory at R2
    u32 y; // 0x: R1, 1x: R2, 2x: memory at R1, 3x: memory at the immediate
    u32 imm;
    u32 hi; // upper bound for the range sub-types
    bool expected;
  };
  static constexpr std::array cases = {
    // Register vs register.
    Case{0x00, 5, 5, 0, 0, true},
    Case{0x00, 5, 6, 0, 0, false},
    Case{0x01, 5, 6, 0, 0, true},
    Case{0x01, 5, 5, 0, 0, false},
    Case{0x02, 6, 5, 0, 0, true},
    Case{0x02, 5, 5, 0, 0, false},
    Case{0x03, 5, 5, 0, 0, true},
    Case{0x03, 4, 5, 0, 0, false},
    Case{0x04, 4, 5, 0, 0, true},
    Case{0x04, 5, 5, 0, 0, false},
    Case{0x05, 5, 5, 0, 0, true},
    Case{0x05, 6, 5, 0, 0, false},
    Case{0x06, 0x0F, 0x05, 0, 0, true},
    Case{0x06, 0x0A, 0x05, 0, 0, false},
    Case{0x07, 0x0A, 0x05, 0, 0, true},
    Case{0x07, 0x0F, 0x05, 0, 0, false},
    Case{0x0A, 0x05, 0x0F, 0, 0, true},
    Case{0x0A, 0x0F, 0x05, 0, 0, false},
    Case{0x0B, 0x0F, 0x05, 0, 0, true},
    Case{0x0B, 0x05, 0x0F, 0, 0, false},

    // Register vs immediate.
    Case{0x10, 5, 0, 5, 0, true},
    Case{0x10, 5, 0, 6, 0, false},
    Case{0x11, 5, 0, 6, 0, true},
    Case{0x11, 5, 0, 5, 0, false},
    Case{0x12, 6, 0, 5, 0, true},
    Case{0x12, 5, 0, 5, 0, false},
    Case{0x13, 5, 0, 5, 0, true},
    Case{0x13, 4, 0, 5, 0, false},
    Case{0x14, 4, 0, 5, 0, true},
    Case{0x14, 5, 0, 5, 0, false},
    Case{0x15, 5, 0, 5, 0, true},
    Case{0x15, 6, 0, 5, 0, false},
    Case{0x16, 0x0F, 0, 0x05, 0, true},
    Case{0x16, 0x0A, 0, 0x05, 0, false},
    Case{0x17, 0x0A, 0, 0x05, 0, true},
    Case{0x17, 0x0F, 0, 0x05, 0, false},
    Case{0x18, 5, 0, 4, 6, true},
    Case{0x18, 6, 0, 4, 6, false},
    Case{0x18, 4, 0, 4, 6, false},
    Case{0x19, 6, 0, 4, 6, true},
    Case{0x19, 4, 0, 4, 6, true},
    Case{0x19, 7, 0, 4, 6, false},
    Case{0x19, 3, 0, 4, 6, false},
    Case{0x1A, 0x05, 0, 0x0F, 0, true},
    Case{0x1A, 0x0F, 0, 0x05, 0, false},
    Case{0x1B, 0x0F, 0, 0x05, 0, true},
    Case{0x1B, 0x05, 0, 0x0F, 0, false},

    // Memory vs memory.
    Case{0x20, 5, 5, 0, 0, true},
    Case{0x20, 5, 6, 0, 0, false},
    Case{0x21, 5, 6, 0, 0, true},
    Case{0x21, 5, 5, 0, 0, false},
    Case{0x22, 6, 5, 0, 0, true},
    Case{0x22, 5, 5, 0, 0, false},
    Case{0x23, 5, 5, 0, 0, true},
    Case{0x23, 4, 5, 0, 0, false},
    Case{0x24, 4, 5, 0, 0, true},
    Case{0x24, 5, 5, 0, 0, false},
    Case{0x25, 5, 5, 0, 0, true},
    Case{0x25, 6, 5, 0, 0, false},
    Case{0x26, 0, 0x0F, 0x05, 0, true},
    Case{0x26, 0, 0x0A, 0x05, 0, false},
    Case{0x27, 0, 0x0A, 0x05, 0, true},
    Case{0x27, 0, 0x0F, 0x05, 0, false},
    Case{0x28, 0, 5, 4, 6, true},
    Case{0x28, 0, 6, 4, 6, false},
    Case{0x29, 0, 6, 4, 6, true},
    Case{0x29, 0, 7, 4, 6, false},
    Case{0x2A, 0, 0x05, 0x0F, 0, true},
    Case{0x2A, 0, 0x0F, 0x05, 0, false},
    Case{0x2B, 0, 0x0F, 0x05, 0, true},
    Case{0x2B, 0, 0x05, 0x0F, 0, false},

    // Register vs memory.
    Case{0x30, 5, 5, 0, 0, true},
    Case{0x30, 5, 6, 0, 0, false},
    Case{0x31, 5, 6, 0, 0, true},
    Case{0x31, 5, 5, 0, 0, false},
    Case{0x32, 6, 5, 0, 0, true},
    Case{0x32, 5, 5, 0, 0, false},
    Case{0x33, 5, 5, 0, 0, true},
    Case{0x33, 4, 5, 0, 0, false},
    Case{0x34, 4, 5, 0, 0, true},
    Case{0x34, 5, 5, 0, 0, false},
    Case{0x35, 5, 5, 0, 0, true},
    Case{0x35, 4, 5, 0, 0, true},
    Case{0x35, 6, 5, 0, 0, false},
    Case{0x36, 0x0F, 0x05, 0, 0, true},
    Case{0x36, 0x0A, 0x05, 0, 0, false},
    Case{0x37, 0x0A, 0x05, 0, 0, true},
    Case{0x37, 0x0F, 0x05, 0, 0, false},
    Case{0x3A, 0x05, 0x0F, 0, 0, true},
    Case{0x3A, 0x0F, 0x05, 0, 0, false},
    Case{0x3B, 0x0F, 0x05, 0, 0, true},
    Case{0x3B, 0x05, 0x0F, 0, 0, false},
  };

  // Returns whether the block ran.
  const auto run = [](u8 sub_type, u32 r1, u32 r2, u32 value) {
    SetCheatRegister(0x20, r1);
    SetCheatRegister(0x21, r2);
    WriteWord(0x7000, 0);
    WriteWord(0x7010, 0);
    Run(fmt::format("52{:02X}2120 {:08X}\n90007000 00000001\n00000000 0000FFFF\n90007010 00000002\n", sub_type, value));
    EXPECT_EQ(ReadWord(0x7010), 2u);
    return (ReadWord(0x7000) != 0);
  };

  for (const u32 width : {1u, 2u, 4u})
  {
    const u8 offset = (width == 1) ? 0x00 : ((width == 2) ? 0x40 : 0x80);
    const u32 mask = (width == 4) ? 0xFFFFFFFFu : ((1u << (width * 8)) - 1);
    for (const Case& c : cases)
    {
      const u8 group = c.sub_type & 0xF0;
      const u8 op = c.sub_type & 0x0F;

      // No range compares for 32-bit.
      if (width == 4 && (op == 0x08 || op == 0x09))
        continue;

      const u8 sub_type = c.sub_type + offset;
      SCOPED_TRACE(fmt::format("{:02X} x={:X} y={:X} imm={:X} hi={:X}", sub_type, c.x, c.y, c.imm, c.hi));

      // Other bits are set in register values and around memory values, so a wider compare would fail.
      const u32 x_reg = c.x | (0x12345678u & ~mask);
      const u32 y_reg = c.y | (0x9ABCDEF0u & ~mask);
      const u32 value = (width == 4) ? c.imm : (c.imm | (c.hi << 16) | ((width == 1) ? 0x5A00u : 0u));
      std::memset(&g_bus.unprotected_ram[MEM_A - 0x10], 0xEE, 0x40);
      const auto write_memory = [width](u32 address, u32 v) {
        std::memcpy(&g_bus.unprotected_ram[address], &v, width);
      };

      bool result;
      switch (group)
      {
        case 0x00:
          result = run(sub_type, y_reg, x_reg, value);
          break;
        case 0x10:
          result = run(sub_type, x_reg, y_reg, value);
          break;
        case 0x20:
          write_memory(MEM_B, c.x);
          write_memory(MEM_A, c.y);
          result = run(sub_type, MEM_A, MEM_B, value);
          break;
        default:
          write_memory(MEM_V, c.y);
          result = run(sub_type, x_reg, 0, MEM_V);
          break;
      }
      EXPECT_EQ(result, c.expected);
    }
  }

  // Sub-types that don't exist never run the block.
  for (const u8 sub_type : {0x08, 0x0C, 0x1C, 0x2C, 0x3C, 0x4C, 0x88, 0x98, 0xA8, 0xBC, 0xC0, 0xFF})
  {
    SCOPED_TRACE(static_cast<u32>(sub_type));
    EXPECT_FALSE(run(sub_type, 0, 0, 0));
  }
}

TEST_F(CheatsApply, CheatRegisterCompareRangeValueWidth)
{
  WriteByte(0x6000, 0x06);
  WriteHalf(0x6010, 0x0180);
  SetCheatRegister(0x20, 0x12340006);
  SetCheatRegister(0x21, 0x12340180);
  SetCheatRegister(0x22, 0x6000);
  SetCheatRegister(0x23, 0x6010);

  // Byte bounds ignore bits 8-15 and 24-31. Halfword bounds use all 16 bits on each side.
  static constexpr std::array<std::pair<std::string_view, bool>, 8> cases = {{
    {"52180020 A5065A04", false},
    {"52190020 A5065A04", true},
    {"52280022 A5065A04", false},
    {"52290022 A5065A04", true},
    {"52580021 02000100", true},
    {"52590021 02000100", true},
    {"52680023 02000100", true},
    {"52690023 02000100", true},
  }};
  for (const auto& [condition, expected] : cases)
  {
    SCOPED_TRACE(condition);
    WriteByte(0x1000, 0);
    Run(fmt::format("{}\n30001000 00000001\n", condition));
    EXPECT_EQ(ReadByte(0x1000), expected ? 1u : 0u);
  }
}

TEST_F(CheatsApply, WriteIfMatch)
{
  WriteWord(0x1000, 0x55551111);
  WriteWord(0x1010, 0x55555511);
  WriteWord(0x1020, 0x55551111);
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("A6001000 1111AAAA\n"
                                                           "A8001010 000011AA\n"
                                                           "A7001020 1111AAAA\n");
  ASSERT_NE(code, nullptr);
  EXPECT_TRUE(code->HasRestorableOnDisableEffects());
  code->Apply();
  EXPECT_EQ(ReadWord(0x1000), 0x5555AAAAu);
  EXPECT_EQ(ReadWord(0x1010), 0x555555AAu);
  EXPECT_EQ(ReadWord(0x1020), 0x5555AAAAu);

  // No match, no write.
  WriteWord(0x1000, 0x55552222);
  WriteWord(0x1010, 0x55555522);
  WriteWord(0x1020, 0x55552222);
  code->Apply();
  EXPECT_EQ(ReadWord(0x1000), 0x55552222u);
  EXPECT_EQ(ReadWord(0x1010), 0x55555522u);
  EXPECT_EQ(ReadWord(0x1020), 0x55552222u);

  Error error;
  EXPECT_FALSE(ParseGamesharkCode("A6001000 1111AAAA\n", &error)->HasRestorableOnDisableEffects());
  EXPECT_TRUE(ParseGamesharkCode("A7001000 1111AAAA\n", &error)->HasRestorableOnDisableEffects());
  EXPECT_TRUE(ParseGamesharkCode("A8001000 000011AA\n", &error)->HasRestorableOnDisableEffects());
}

TEST_F(CheatsApply, WriteIfMatchRestore)
{
  // A6 isn't restored. A7/A8 are, only if the written value is still there, regardless of conditionals.
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("A6001000 1111AAAA\n"
                                                           "D0003000 00001234\n"
                                                           "A8001010 000011AA\n"
                                                           "C0003000 00001234\n"
                                                           "A7001020 1111AAAA\n"
                                                           "A7001030 1111AAAA\n"
                                                           "00000000 0000FFFF\n");
  ASSERT_NE(code, nullptr);
  WriteWord(0x1000, 0x5555AAAA);
  WriteWord(0x1010, 0x555555AA);
  WriteWord(0x1020, 0x5555AAAA);
  WriteWord(0x1030, 0x5555BBBB);

  Cheats::RollbackLog rollback;
  code->ApplyOnDisable(&rollback);
  EXPECT_EQ(ReadWord(0x1000), 0x5555AAAAu);
  EXPECT_EQ(ReadWord(0x1010), 0x55555511u);
  EXPECT_EQ(ReadWord(0x1020), 0x55551111u);
  EXPECT_EQ(ReadWord(0x1030), 0x5555BBBBu);
  ASSERT_EQ(rollback.size(), 2u);
  EXPECT_EQ(rollback[0].size, MemoryAccessSize::Byte);
  EXPECT_EQ(rollback[0].address, 0x1010u);
  EXPECT_EQ(rollback[0].value, 0xAAu);
  EXPECT_EQ(rollback[1].size, MemoryAccessSize::HalfWord);
  EXPECT_EQ(rollback[1].address, 0x1020u);
  EXPECT_EQ(rollback[1].value, 0xAAAAu);

  Cheats::ReapplyOnDisableCodes(rollback);
  EXPECT_EQ(ReadWord(0x1010), 0x555555AAu);
  EXPECT_EQ(ReadWord(0x1020), 0x5555AAAAu);

  // Also works without a rollback list.
  code->ApplyOnDisable(nullptr);
  EXPECT_EQ(ReadWord(0x1010), 0x55555511u);
  EXPECT_EQ(ReadWord(0x1020), 0x55551111u);
}

TEST_F(CheatsApply, WriteIfMatchRestoreByteIgnoresUpperHalfword)
{
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("A8001010 987611AA\n");
  ASSERT_NE(code, nullptr);
  WriteWord(0x1010, 0x55555511);

  code->Apply();
  EXPECT_EQ(ReadWord(0x1010), 0x555555AAu);
  code->ApplyOnDisable(nullptr);
  EXPECT_EQ(ReadWord(0x1010), 0x55555511u);
}

TEST_F(CheatsApply, ApplyOnDisableSkipsOtherInstructions)
{
  // Payload lines that look like A7 restores mustn't be treated as instructions.
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("30001100 00000011\n"
                                                           "80001100 00001111\n"
                                                           "90001100 11111111\n"
                                                           "31001100 00000011\n"
                                                           "32001100 00000011\n"
                                                           "81001100 00000011\n"
                                                           "82001100 00000011\n"
                                                           "91001100 00000011\n"
                                                           "92001100 00000011\n"
                                                           "1F001100 00000011\n"
                                                           "A5001100 00000011\n"
                                                           "10001100 00000011\n"
                                                           "11001100 00000011\n"
                                                           "20001100 00000011\n"
                                                           "21001100 00000011\n"
                                                           "60001100 00000011\n"
                                                           "61001100 00000011\n"
                                                           "F0001100 00000011\n"
                                                           "F1001100 00000011\n"
                                                           "F2001100 00000011\n"
                                                           "F5001100 00000011\n"
                                                           "C1000000 00000000\n"
                                                           "A6001100 00000011\n"
                                                           "51850030 00001100\n"
                                                           "F3001100 00110000\n"
                                                           "A7001010 1111AAAA\n"
                                                           "50000201 00000001\n"
                                                           "A7001010 1111AAAA\n"
                                                           "53000002 00010001\n"
                                                           "A7001010 1111AAAA\n"
                                                           "C2001100 00000004\n"
                                                           "A7001010 1111AAAA\n"
                                                           "F4001100 00EE0000\n"
                                                           "A7001010 1111AAAA\n"
                                                           "A7001010 1111AAAA\n"
                                                           "A7001010 1111AAAA\n"
                                                           "A7001010 1111AAAA\n"
                                                           "A9001100 00000002\n"
                                                           "A7001010 1111AAAA\n"
                                                           "AB001100 00000001\n"
                                                           "A7001010 1111AAAA\n"
                                                           "D0001100 00000000\n"
                                                           "D4000000 00000000\n"
                                                           "C0001100 00000000\n"
                                                           "D7000000 E3000000\n"
                                                           "F6000000 1F000001\n"
                                                           "D0001100 00000000\n"
                                                           "F6000001 1F000002\n"
                                                           "E4001100 00000000\n"
                                                           "E5001100 00000000\n"
                                                           "52002120 00000000\n"
                                                           "00000000 0000FFFF\n"
                                                           "A7001020 1111AAAA\n");
  ASSERT_NE(code, nullptr);
  WriteWord(0x1010, 0x5555AAAA);
  WriteWord(0x1020, 0x5555AAAA);
  WriteWord(0x1100, 0x12345678);
  SetCheatRegister(0x30, 0);

  // Every instruction is known, so nothing is logged as an error.
  Cheats::RollbackLog rollback;
  EXPECT_EQ(CountLogErrors([&]() { code->ApplyOnDisable(&rollback); }), 0u);
  EXPECT_EQ(ReadWord(0x1010), 0x5555AAAAu);
  EXPECT_EQ(ReadWord(0x1020), 0x55551111u);
  EXPECT_EQ(ReadWord(0x1100), 0x12345678u);
  EXPECT_EQ(GetCheatRegister(0x30), 0u);
  ASSERT_EQ(rollback.size(), 1u);
  EXPECT_EQ(rollback[0].address, 0x1020u);
}

TEST_F(CheatsApply, ApplyOnDisableIgnoresUnknownOpcode)
{
  std::unique_ptr<Cheats::GamesharkCheatCode> code = Parse("40001000 00000001\nA8001010 000011AA\n");
  ASSERT_NE(code, nullptr);
  WriteByte(0x1010, 0xAA);

  EXPECT_EQ(CountLogErrors([&]() { code->ApplyOnDisable(nullptr); }), 0u);
  EXPECT_EQ(ReadByte(0x1010), 0x11u);
}

TEST_F(CheatsApply, MultiConditionalFalseContinuesAfterSeparator)
{
  Run("F6000000 1F000001\n"
      "D0003000 00001234\n"
      "90001000 00000001\n"
      "00000000 0000FFFF\n"
      "90001010 00000002\n");
  EXPECT_EQ(ReadWord(0x1000), 0u);
  EXPECT_EQ(ReadWord(0x1010), 2u);
}

TEST_F(CheatsApply, MultiConditionalFalseWithoutSeparator)
{
  // The last line looks like an OR header, which used to be re-evaluated forever.
  Run("F6000001 1F000001\n"
      "D0003000 00001234\n"
      "90001001 00000005\n");
  EXPECT_EQ(ReadWord(0x1000), 0u);
}

TEST_F(CheatsApply, MultiConditionalElseAfterSeparator)
{
  static constexpr std::string_view code = "F6000000 1F000001\n"
                                           "D0003000 00001234\n"
                                           "90001000 00000001\n"
                                           "00000000 0000FFFF\n"
                                           "F6000000 E15E0000\n"
                                           "90001010 00000002\n"
                                           "00000000 0000FFFF\n"
                                           "90001020 00000003\n";
  for (const auto& [value, expected] :
       {std::pair{u16(0x1234), std::array{1u, 0u, 3u}}, std::pair{u16(0), std::array{0u, 2u, 3u}}})
  {
    SCOPED_TRACE(value);
    std::memset(&g_bus.unprotected_ram[0x1000], 0, 0x100);
    WriteHalf(0x3000, value);
    Run(code);
    EXPECT_EQ(ReadWord(0x1000), expected[0]);
    EXPECT_EQ(ReadWord(0x1010), expected[1]);
    EXPECT_EQ(ReadWord(0x1020), expected[2]);
  }
}

TEST_F(CheatsApply, MultiConditionalElseIfAfterSeparator)
{
  static constexpr std::string_view code = "F6000000 1F000001\n"
                                           "D0003000 00001234\n"
                                           "90001000 00000001\n"
                                           "00000000 0000FFFF\n"
                                           "F6000000 E15E1F01\n"
                                           "D0003000 00005678\n"
                                           "90001010 00000002\n"
                                           "00000000 0000FFFF\n"
                                           "F6000000 E15E0000\n"
                                           "90001020 00000003\n"
                                           "00000000 0000FFFF\n"
                                           "90001030 00000004\n";
  for (const auto& [value, expected] :
       {std::pair{u16(0x1234), std::array{1u, 0u, 0u, 4u}}, std::pair{u16(0x5678), std::array{0u, 2u, 0u, 4u}},
        std::pair{u16(0), std::array{0u, 0u, 3u, 4u}}})
  {
    SCOPED_TRACE(value);
    std::memset(&g_bus.unprotected_ram[0x1000], 0, 0x100);
    WriteHalf(0x3000, value);
    Run(code);
    EXPECT_EQ(ReadWord(0x1000), expected[0]);
    EXPECT_EQ(ReadWord(0x1010), expected[1]);
    EXPECT_EQ(ReadWord(0x1020), expected[2]);
    EXPECT_EQ(ReadWord(0x1030), expected[3]);
  }
}

TEST_F(CheatsApply, FindAndReplaceAdjacentMatch)
{
  static constexpr std::array<u8, 16> pattern = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                                 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
  std::memcpy(&g_bus.unprotected_ram[0x4000], pattern.data(), pattern.size());
  std::memcpy(&g_bus.unprotected_ram[0x4010], pattern.data(), pattern.size());
  Run("F4004010 00EE0008\n"
      "10111213 14151617\n"
      "18191A1B 1C1D1E1F\n"
      "A0A1A2A3 A4A5A6A7\n"
      "A8A9AAAB ACADAEAF\n");
  EXPECT_EQ(ReadByte(0x4000), 0xA0u);
  EXPECT_EQ(ReadByte(0x4010), 0xA0u);
}

TEST_F(CheatsApply, FindAndReplaceSkipsOverlappingMatches)
{
  // An all-wildcard search matches every candidate. The replacements must be at least 16 bytes apart.
  Run("F4004008 00EE0004\n"
      "EEEEEEEE EEEEEEEE\n"
      "EEEEEEEE EEEEEEEE\n"
      "A0EEEEEE EEEEEEEE\n"
      "EEEEEEEE EEEEEEEE\n");
  EXPECT_EQ(ReadWord(0x4000), 0xA0u);
  EXPECT_EQ(ReadWord(0x4004), 0u);
  EXPECT_EQ(ReadWord(0x4008), 0u);
  EXPECT_EQ(ReadWord(0x400C), 0u);
  EXPECT_EQ(ReadWord(0x4010), 0xA0u);
}

TEST_F(CheatsApply, Slide32BitValue)
{
  Run("50000204 00000001\n90001000 1234FFFF\n");
  EXPECT_EQ(ReadWord(0x1000), 0x1234FFFFu);
  EXPECT_EQ(ReadWord(0x1004), 0x12350000u);
}
