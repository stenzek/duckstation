// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_disasm.h"

#include "common/error.h"
#include "common/small_string.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <array>

namespace {

static std::string Disassemble(u32 pc, u32 bits)
{
  SmallString result;
  CPU::DisassembleInstruction(&result, pc, bits);
  return std::string(result.view());
}

static u32 Assemble(u32 pc, std::string_view text)
{
  u32 result = 0xDEADBEEF;
  Error error;
  EXPECT_TRUE(CPU::AssembleInstruction(&result, pc, text, &error)) << error.GetDescription();
  return result;
}

struct AssemblyCase
{
  std::string_view text;
  u32 bits;
};

TEST(CPUDisasm, Disassemble)
{
  EXPECT_EQ(Disassemble(0x1000, 0x00000000), "sll zero, zero, 0");
  EXPECT_EQ(Disassemble(0x1000, 0x012A4020), "add t0, t1, t2");
  EXPECT_EQ(Disassemble(0x1000, 0x01494007), "srav t0, t1, t2");
  EXPECT_EQ(Disassemble(0x1000, 0x0109001B), "divu t0, t1");
  EXPECT_EQ(Disassemble(0x1000, 0x2128FFFC), "addi t0, t1, -4");
  EXPECT_EQ(Disassemble(0x1000, 0x11000003), "beq t0, zero, 0x00001010");
  EXPECT_EQ(Disassemble(0x1000, 0x05110003), "bgezal t0, 0x00001010");
  EXPECT_EQ(Disassemble(0x1000, 0x8FA8FFF0), "lw t0, -0x10(sp)");
  EXPECT_EQ(Disassemble(0x1000, 0xCBA60010), "lwc2 rgbc, 0x10(sp)");
  EXPECT_EQ(Disassemble(0x1000, 0x40086000), "mfc0 t0, SR");
  EXPECT_EQ(Disassemble(0x1000, 0x48481800), "cfc2 t0, rt_3");
  EXPECT_EQ(Disassemble(0x1000, 0x42000010), "rfe");
  EXPECT_EQ(Disassemble(0x1000, 0x4A080401), "rtps sf lm");
  EXPECT_EQ(Disassemble(0x1000, 0x4A000006), "nclip");
  EXPECT_EQ(Disassemble(0x1000, 0x46123456), "cop1 0x00123456");
}

TEST(CPUDisasm, UnknownInstructions)
{
  EXPECT_EQ(Disassemble(0x1000, 0x60000000), "UNKNOWN");
  EXPECT_EQ(Disassemble(0x1000, 0x00000001), "UNKNOWN");
  EXPECT_EQ(Disassemble(0x1000, 0x4A000002), "UNKNOWN");
}

TEST(CPUDisasm, Assemble)
{
  static constexpr std::array cases = {
    AssemblyCase{"nop", 0x00000000},
    AssemblyCase{"NOP", 0x00000000},
    AssemblyCase{"SLL $ZERO, $zero, 0", 0x00000000},
    AssemblyCase{"srl t0, t1, 4", 0x00094102},
    AssemblyCase{"sra t0, t1, 4", 0x00094103},
    AssemblyCase{"sllv t0, t1, t2", 0x01494004},
    AssemblyCase{"srlv t0, t1, t2", 0x01494006},
    AssemblyCase{"srav t0, t1, t2", 0x01494007},
    AssemblyCase{"jr t0", 0x01000008},
    AssemblyCase{"jalr ra, t0", 0x0100F809},
    AssemblyCase{"syscall", 0x0000000C},
    AssemblyCase{"break", 0x0000000D},
    AssemblyCase{"mfhi t0", 0x00004010},
    AssemblyCase{"mthi t0", 0x01000011},
    AssemblyCase{"mflo t0", 0x00004012},
    AssemblyCase{"mtlo t0", 0x01000013},
    AssemblyCase{"mult t0, t1", 0x01090018},
    AssemblyCase{"multu t0, t1", 0x01090019},
    AssemblyCase{"div t0, t1", 0x0109001A},
    AssemblyCase{"divu t0, t1", 0x0109001B},
    AssemblyCase{"add t0, $t1, T2", 0x012A4020},
    AssemblyCase{"addu t0, t1, t2", 0x012A4021},
    AssemblyCase{"sub t0, t1, t2", 0x012A4022},
    AssemblyCase{"subu t0, t1, t2", 0x012A4023},
    AssemblyCase{"and t0, t1, t2", 0x012A4024},
    AssemblyCase{"or t0, t1, t2", 0x012A4025},
    AssemblyCase{"xor t0, t1, t2", 0x012A4026},
    AssemblyCase{"nor t0, t1, t2", 0x012A4027},
    AssemblyCase{"slt t0, t1, t2", 0x012A402A},
    AssemblyCase{"sltu t0, t1, t2", 0x012A402B},
    AssemblyCase{"j 0x1000", 0x08000400},
    AssemblyCase{"jal 0x1000", 0x0C000400},
    AssemblyCase{"beq t1, t2, 0x1010", 0x112A0003},
    AssemblyCase{"bne t1, t2, 0x1010", 0x152A0003},
    AssemblyCase{"blez t1, 0x1010", 0x19200003},
    AssemblyCase{"bgtz t1, 0x1010", 0x1D200003},
    AssemblyCase{"bltz t0, 0x1010", 0x05000003},
    AssemblyCase{"bgez t0, 0x1010", 0x05010003},
    AssemblyCase{"bltzal t0, 0x1010", 0x05100003},
    AssemblyCase{"bgezal t0, 0x1010", 0x05110003},
    AssemblyCase{"addi t0,t1,-4", 0x2128FFFC},
    AssemblyCase{"addiu t0, t1, -4", 0x2528FFFC},
    AssemblyCase{"slti t0, t1, -4", 0x2928FFFC},
    AssemblyCase{"sltiu t0, t1, 291", 0x2D280123},
    AssemblyCase{"andi t0, t1, 0xabcd", 0x3128ABCD},
    AssemblyCase{"ori t0, t1, 0xabcd", 0x3528ABCD},
    AssemblyCase{"xori t0, t1, 0xabcd", 0x3928ABCD},
    AssemblyCase{"lui t0, 0xabcd", 0x3C08ABCD},
    AssemblyCase{"lb t0, -0x10(sp)", 0x83A8FFF0},
    AssemblyCase{"lh t0, -0x10(sp)", 0x87A8FFF0},
    AssemblyCase{"lwl t0, -0x10(sp)", 0x8BA8FFF0},
    AssemblyCase{"lw t0, -0x10($sp)", 0x8FA8FFF0},
    AssemblyCase{"lbu t0, -0x10(sp)", 0x93A8FFF0},
    AssemblyCase{"lhu t0, -0x10(sp)", 0x97A8FFF0},
    AssemblyCase{"lwr t0, -0x10(sp)", 0x9BA8FFF0},
    AssemblyCase{"sb t0, -0x10(sp)", 0xA3A8FFF0},
    AssemblyCase{"sh t0, -0x10(sp)", 0xA7A8FFF0},
    AssemblyCase{"swl t0, -0x10(sp)", 0xABA8FFF0},
    AssemblyCase{"sw t0, -0x10(sp)", 0xAFA8FFF0},
    AssemblyCase{"swr t0, -0x10(sp)", 0xBBA8FFF0},
    AssemblyCase{"lwc2 rgbc, 0x10(sp)", 0xCBA60010},
    AssemblyCase{"swc2 rgbc, 0x10(sp)", 0xEBA60010},
    AssemblyCase{"mfc0 t0, sr", 0x40086000},
    AssemblyCase{"cfc2 t0, rt_3", 0x48481800},
    AssemblyCase{"mtc1 t0, 7", 0x44883800},
    AssemblyCase{"ctc3 t0, 5", 0x4CC82800},
    AssemblyCase{"rfe", 0x42000010},
    AssemblyCase{"rtps SF lm", 0x4A080401},
    AssemblyCase{"nclip", 0x4A000006},
    AssemblyCase{"mvmva sf lm m=2 v=1 t=3", 0x4A0CE412},
    AssemblyCase{"cop1 0x123456", 0x46123456},
  };

  for (const AssemblyCase& test : cases)
    EXPECT_EQ(Assemble(0x1000, test.text), test.bits) << test.text;
}

TEST(CPUDisasm, AssembleErrorsDoNotModifyDestination)
{
  static constexpr std::array<std::string_view, 14> cases = {
    "",                   "unknown t0",          "nop t0",             "sll t0, t1, 32",
    "addi t0, t1, 32768", "andi t0, t1, -1",    "lw t0, 0",           "lw bad, 0(sp)",
    "beq t0, t1, 3",      "beq t0, t1, 0x40000", "j 0x10000000",      "mfc0 t0, nope",
    "mvmva m=0 v=0",      "rtps sf sf",
  };

  for (const std::string_view text : cases)
  {
    u32 result = 0xDEADBEEF;
    Error error;
    EXPECT_FALSE(CPU::AssembleInstruction(&result, 0x1000, text, &error)) << text;
    EXPECT_EQ(result, 0xDEADBEEFu) << text;
    EXPECT_TRUE(error.IsValid()) << text;
    EXPECT_FALSE(error.GetDescription().empty()) << text;
  }
}

TEST(CPUDisasm, RoundTrip)
{
  static constexpr std::array<u32, 54> instructions = {
    0x00000000, 0x00094102, 0x00094103, 0x01494004, 0x01494006, 0x01494007,
    0x01000008, 0x0100F809, 0x0000000C, 0x0000000D, 0x00004010, 0x01000011,
    0x00004012, 0x01000013, 0x01090018, 0x01090019, 0x0109001A, 0x0109001B,
    0x012A4020, 0x012A4021, 0x012A4022, 0x012A4023, 0x012A4024, 0x012A4025,
    0x012A4026, 0x012A4027, 0x012A402A, 0x012A402B, 0x08000400, 0x0C000400,
    0x112A0003, 0x152A0003, 0x19200003, 0x1D200003, 0x05000003, 0x05010003,
    0x05100003, 0x05110003, 0x2128FFFC, 0x2528FFFC, 0x2928FFFC, 0x2D280123,
    0x3128ABCD, 0x3528ABCD, 0x3928ABCD, 0x3C08ABCD, 0x83A8FFF0, 0x8FA8FFF0,
    0xBBA8FFF0, 0xCBA60010, 0xEBA60010, 0x40086000, 0x4A0CE412, 0x46123456,
  };

  for (const u32 bits : instructions)
  {
    const std::string text = Disassemble(0x1000, bits);
    u32 assembled = 0;
    Error error;
    ASSERT_TRUE(CPU::AssembleInstruction(&assembled, 0x1000, text, &error))
      << fmt::format("0x{:08x}: {}: {}", bits, text, error.GetDescription());
    EXPECT_EQ(assembled, bits) << text;
  }
}

} // namespace
