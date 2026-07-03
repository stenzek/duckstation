// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_disasm.h"

#include "common/error.h"
#include "common/small_string.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <array>
#include <limits>

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

struct LabelAssemblyCase
{
  std::string_view label_text;
  std::string_view forward_text;
  std::string_view backward_text;
};

struct LabelAssemblyInstructions
{
  u32* instructions;
  size_t count;
  u32 base_pc;
};

static u32* GetLabelAssemblyInstruction(const u32 pc, void* userdata)
{
  const LabelAssemblyInstructions* const code = static_cast<LabelAssemblyInstructions*>(userdata);
  if (pc < code->base_pc || ((pc - code->base_pc) % CPU::INSTRUCTION_SIZE) != 0)
    return nullptr;

  const size_t index = (pc - code->base_pc) / CPU::INSTRUCTION_SIZE;
  return (index < code->count) ? &code->instructions[index] : nullptr;
}

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

TEST(CPUDisasm, JumpTargets)
{
  struct JumpType
  {
    std::string_view mnemonic;
    u32 opcode;
  };

  static constexpr std::array jump_types = {
    JumpType{"j", 0x08000000},
    JumpType{"jal", 0x0C000000},
  };
  static constexpr std::array<u32, 5> regions = {0x00000000, 0x10000000, 0x80000000, 0xA0000000, 0xF0000000};
  static constexpr std::array<u32, 4> target_fields = {0x0000000, 0x0000001, 0x0123456, 0x3FFFFFF};

  for (const JumpType& jump : jump_types)
  {
    for (const u32 region : regions)
    {
      const u32 pc = region | 0x1000;
      for (const u32 target_field : target_fields)
      {
        const u32 target = region | (target_field << 2);
        const u32 bits = jump.opcode | target_field;
        const std::string text = fmt::format("{} 0x{:08x}", jump.mnemonic, target);
        SCOPED_TRACE(fmt::format("pc=0x{:08x}, instruction={}", pc, text));
        EXPECT_EQ(Assemble(pc, text), bits);
        EXPECT_EQ(Disassemble(pc, bits), text);
      }
    }
  }

  static constexpr std::array boundary_cases = {
    AssemblyCase{"j 0x10000000", 0x08000000},
    AssemblyCase{"jal 0x10000000", 0x0C000000},
  };
  for (const AssemblyCase& test : boundary_cases)
  {
    EXPECT_EQ(Assemble(0x0FFFFFFC, test.text), test.bits);
    EXPECT_EQ(Disassemble(0x0FFFFFFC, test.bits), test.text);
  }

  EXPECT_EQ(Assemble(0xFFFFFFFC, "j 0x00000000"), 0x08000000u);
  EXPECT_EQ(Disassemble(0xFFFFFFFC, 0x08000000), "j 0x00000000");
}

TEST(CPUDisasm, BranchTargetBoundaries)
{
  static constexpr std::array<s32, 5> word_offsets = {
    std::numeric_limits<s16>::min(), -1, 0, 1, std::numeric_limits<s16>::max(),
  };
  static constexpr u32 pc = 0x80010000;
  for (const s32 word_offset : word_offsets)
  {
    const u32 target = (pc + 4) + static_cast<u32>(word_offset * 4);
    const u32 bits = 0x11090000 | static_cast<u16>(word_offset);
    const std::string text = fmt::format("beq t0, t1, 0x{:08x}", target);
    SCOPED_TRACE(text);
    EXPECT_EQ(Assemble(pc, text), bits);
    EXPECT_EQ(Disassemble(pc, bits), text);
  }

  EXPECT_EQ(Assemble(0xFFFFFFFC, "beq t0, t1, 0x00000000"), 0x11090000u);
  EXPECT_EQ(Disassemble(0xFFFFFFFC, 0x11090000), "beq t0, t1, 0x00000000");
}

TEST(CPUDisasm, ControlFlowTargetErrorsDoNotModifyDestination)
{
  struct ErrorCase
  {
    u32 pc;
    std::string_view text;
  };
  static constexpr std::array cases = {
    ErrorCase{0x80001000, "j 0x90001000"},           ErrorCase{0x80001000, "jal 0x80001002"},
    ErrorCase{0x0FFFFFF8, "j 0x10000000"},           ErrorCase{0x80010000, "beq t0, t1, 0x80030004"},
    ErrorCase{0x80010000, "beq t0, t1, 0x7fff0000"},
  };

  for (const ErrorCase& test : cases)
  {
    u32 result = 0xDEADBEEF;
    Error error;
    EXPECT_FALSE(CPU::AssembleInstruction(&result, test.pc, test.text, &error)) << test.text;
    EXPECT_EQ(result, 0xDEADBEEFu) << test.text;
    EXPECT_TRUE(error.IsValid()) << test.text;
  }
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

TEST(CPUDisasm, AssembleBranchLabels)
{
  static constexpr std::array cases = {
    LabelAssemblyCase{"j @target", "j 0x1040", "j 0x1000"},
    LabelAssemblyCase{"jal @target", "jal 0x1040", "jal 0x1000"},
    LabelAssemblyCase{"beq t1, t2, @target", "beq t1, t2, 0x1040", "beq t1, t2, 0x1000"},
    LabelAssemblyCase{"bne t1, t2, @target", "bne t1, t2, 0x1040", "bne t1, t2, 0x1000"},
    LabelAssemblyCase{"blez t1, @target", "blez t1, 0x1040", "blez t1, 0x1000"},
    LabelAssemblyCase{"bgtz t1, @target", "bgtz t1, 0x1040", "bgtz t1, 0x1000"},
    LabelAssemblyCase{"bltz t0, @target", "bltz t0, 0x1040", "bltz t0, 0x1000"},
    LabelAssemblyCase{"bgez t0, @target", "bgez t0, 0x1040", "bgez t0, 0x1000"},
    LabelAssemblyCase{"bltzal t0, @target", "bltzal t0, 0x1040", "bltzal t0, 0x1000"},
    LabelAssemblyCase{"bgezal t0, @target", "bgezal t0, 0x1040", "bgezal t0, 0x1000"},
  };

  CPU::AssemblyLabelList forward_labels;
  std::array<u32, cases.size()> forward_results = {};
  for (size_t i = 0; i < cases.size(); i++)
  {
    Error error;
    ASSERT_TRUE(CPU::AssembleInstruction(&forward_results[i], 0x1000 + static_cast<u32>(i * 4), cases[i].label_text,
                                         &forward_labels, &error))
      << error.GetDescription();
  }
  EXPECT_TRUE(CPU::ContainsUnresolvedLabels(forward_labels));

  Error error;
  CPU::AssemblyLabel* label;
  ASSERT_TRUE((label = CPU::DefineAssemblyLabel(&forward_labels, "@target", 0x1040, &error))) << error.GetDescription();
  EXPECT_FALSE(CPU::ContainsUnresolvedLabels(forward_labels));
  LabelAssemblyInstructions forward_code = {forward_results.data(), forward_results.size(), 0x1000};
  ASSERT_TRUE(CPU::FixupAssemblyLabelBackreferences(label, &forward_code, &error, GetLabelAssemblyInstruction))
    << error.GetDescription();
  for (size_t i = 0; i < cases.size(); i++)
    EXPECT_EQ(forward_results[i], Assemble(0x1000 + static_cast<u32>(i * 4), cases[i].forward_text));

  CPU::AssemblyLabelList backward_labels;
  ASSERT_TRUE(CPU::DefineAssemblyLabel(&backward_labels, "@target", 0x1000, &error)) << error.GetDescription();
  for (size_t i = 0; i < cases.size(); i++)
  {
    const u32 pc = 0x1040 + static_cast<u32>(i * 4);
    u32 result;
    ASSERT_TRUE(CPU::AssembleInstruction(&result, pc, cases[i].label_text, &backward_labels, &error))
      << error.GetDescription();
    EXPECT_EQ(result, Assemble(pc, cases[i].backward_text));
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
