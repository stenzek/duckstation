// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "cpu_disasm.h"
#include "cpu_core.h"
#include "cpu_types.h"

#include "common/assert.h"
#include "common/small_string.h"

#include <array>

namespace CPU {
namespace {

enum Operand : u8
{
  Operand_None,
  i_rs,
  i_rt,
  i_imm,
  j_target,
  r_rs,
  r_rt,
  r_rd,
  r_shamt,
  r_funct
};

struct TableEntry
{
  const char* format;
};

struct GTEInstructionTable
{
  const char* name;
  bool sf;
  bool lm;
  bool mvmva;
};
} // namespace

static void FormatInstruction(SmallStringBase* dest, const Instruction inst, u32 pc, const char* format);
static void FormatComment(SmallStringBase* dest, const Instruction inst, u32 pc, const char* format);

template<typename T>
static void FormatCopInstruction(SmallStringBase* dest, u32 pc, const Instruction inst,
                                 const std::pair<T, const char*>* table, size_t table_size, T table_key);

template<typename T>
static void FormatCopComment(SmallStringBase* dest, u32 pc, const Instruction inst,
                             const std::pair<T, const char*>* table, size_t table_size, T table_key);

static void FormatGTEInstruction(SmallStringBase* dest, u32 pc, const Instruction inst);

static const std::array<const char*, 64> s_base_table = {{
  "",                       // 0
  "UNKNOWN",                // 1
  "j $jt",                  // 2
  "jal $jt",                // 3
  "beq $rs, $rt, $rel",     // 4
  "bne $rs, $rt, $rel",     // 5
  "blez $rs, $rel",         // 6
  "bgtz $rs, $rel",         // 7
  "addi $rt, $rs, $imm",    // 8
  "addiu $rt, $rs, $imm",   // 9
  "slti $rt, $rs, $imm",    // 10
  "sltiu $rt, $rs, $immu",  // 11
  "andi $rt, $rs, $immu",   // 12
  "ori $rt, $rs, $immu",    // 13
  "xori $rt, $rs, $immu",   // 14
  "lui $rt, $imm",          // 15
  "UNKNOWN",                // 16
  "UNKNOWN",                // 17
  "UNKNOWN",                // 18
  "UNKNOWN",                // 19
  "UNKNOWN",                // 20
  "UNKNOWN",                // 21
  "UNKNOWN",                // 22
  "UNKNOWN",                // 23
  "UNKNOWN",                // 24
  "UNKNOWN",                // 25
  "UNKNOWN",                // 26
  "UNKNOWN",                // 27
  "UNKNOWN",                // 28
  "UNKNOWN",                // 29
  "UNKNOWN",                // 30
  "UNKNOWN",                // 31
  "lb $rt, $offsetrs",      // 32
  "lh $rt, $offsetrs",      // 33
  "lwl $rt, $offsetrs",     // 34
  "lw $rt, $offsetrs",      // 35
  "lbu $rt, $offsetrs",     // 36
  "lhu $rt, $offsetrs",     // 37
  "lwr $rt, $offsetrs",     // 38
  "UNKNOWN",                // 39
  "sb $rt, $offsetrs",      // 40
  "sh $rt, $offsetrs",      // 41
  "swl $rt, $offsetrs",     // 42
  "sw $rt, $offsetrs",      // 43
  "UNKNOWN",                // 44
  "UNKNOWN",                // 45
  "swr $rt, $offsetrs",     // 46
  "UNKNOWN",                // 47
  "lwc0 $coprt, $offsetrs", // 48
  "lwc1 $coprt, $offsetrs", // 49
  "lwc2 $coprt, $offsetrs", // 50
  "lwc3 $coprt, $offsetrs", // 51
  "UNKNOWN",                // 52
  "UNKNOWN",                // 53
  "UNKNOWN",                // 54
  "UNKNOWN",                // 55
  "swc0 $coprt, $offsetrs", // 56
  "swc1 $coprt, $offsetrs", // 57
  "swc2 $coprt, $offsetrs", // 58
  "swc3 $coprt, $offsetrs", // 59
  "UNKNOWN",                // 60
  "UNKNOWN",                // 61
  "UNKNOWN",                // 62
  "UNKNOWN"                 // 63
}};

static const std::array<const char*, 64> s_special_table = {{
  "sll $rd, $rt, $shamt", // 0
  "UNKNOWN",              // 1
  "srl $rd, $rt, $shamt", // 2
  "sra $rd, $rt, $shamt", // 3
  "sllv $rd, $rt, $rs",   // 4
  "UNKNOWN",              // 5
  "srlv $rd, $rt, $rs",   // 6
  "srav $rd, $rt, $rs",   // 7
  "jr $rs",               // 8
  "jalr $rd, $rs",        // 9
  "UNKNOWN",              // 10
  "UNKNOWN",              // 11
  "syscall",              // 12
  "break",                // 13
  "UNKNOWN",              // 14
  "UNKNOWN",              // 15
  "mfhi $rd",             // 16
  "mthi $rs",             // 17
  "mflo $rd",             // 18
  "mtlo $rs",             // 19
  "UNKNOWN",              // 20
  "UNKNOWN",              // 21
  "UNKNOWN",              // 22
  "UNKNOWN",              // 23
  "mult $rs, $rt",        // 24
  "multu $rs, $rt",       // 25
  "div $rs, $rt",         // 26
  "divu $rs, $rt",        // 27
  "UNKNOWN",              // 28
  "UNKNOWN",              // 29
  "UNKNOWN",              // 30
  "UNKNOWN",              // 31
  "add $rd, $rs, $rt",    // 32
  "addu $rd, $rs, $rt",   // 33
  "sub $rd, $rs, $rt",    // 34
  "subu $rd, $rs, $rt",   // 35
  "and $rd, $rs, $rt",    // 36
  "or $rd, $rs, $rt",     // 37
  "xor $rd, $rs, $rt",    // 38
  "nor $rd, $rs, $rt",    // 39
  "UNKNOWN",              // 40
  "UNKNOWN",              // 41
  "slt $rd, $rs, $rt",    // 42
  "sltu $rd, $rs, $rt",   // 43
  "UNKNOWN",              // 44
  "UNKNOWN",              // 45
  "UNKNOWN",              // 46
  "UNKNOWN",              // 47
  "UNKNOWN",              // 48
  "UNKNOWN",              // 49
  "UNKNOWN",              // 50
  "UNKNOWN",              // 51
  "UNKNOWN",              // 52
  "UNKNOWN",              // 53
  "UNKNOWN",              // 54
  "UNKNOWN",              // 55
  "UNKNOWN",              // 56
  "UNKNOWN",              // 57
  "UNKNOWN",              // 58
  "UNKNOWN",              // 59
  "UNKNOWN",              // 60
  "UNKNOWN",              // 61
  "UNKNOWN",              // 62
  "UNKNOWN"               // 63
}};

static const std::array<std::pair<CopCommonInstruction, const char*>, 4> s_cop_common_table = {
  {{CopCommonInstruction::mfcn, "mfc$cop $rt_, $coprd"},
   {CopCommonInstruction::cfcn, "cfc$cop $rt_, $coprdc"},
   {CopCommonInstruction::mtcn, "mtc$cop $rt, $coprd"},
   {CopCommonInstruction::ctcn, "ctc$cop $rt, $coprdc"}}};

static const std::array<std::pair<Cop0Instruction, const char*>, 1> s_cop0_table = {{{Cop0Instruction::rfe, "rfe"}}};

static constexpr const std::array<const char*, 64> s_gte_register_names = {
  {"v0_xy", "v0_z",  "v1_xy", "v1_z",  "v2_xy", "v2_z",  "rgbc", "otz",  "ir0",  "ir1",   "ir2",   "ir3",   "sxy0",
   "sxy1",  "sxy2",  "sxyp",  "sz0",   "sz1",   "sz2",   "sz3",  "rgb0", "rgb1", "rgb2",  "res1",  "mac0",  "mac1",
   "mac2",  "mac3",  "irgb",  "orgb",  "lzcs",  "lzcr",  "rt_0", "rt_1", "rt_2", "rt_3",  "rt_4",  "trx",   "try",
   "trz",   "llm_0", "llm_1", "llm_2", "llm_3", "llm_4", "rbk",  "gbk",  "bbk",  "lcm_0", "lcm_1", "lcm_2", "lcm_3",
   "lcm_4", "rfc",   "gfc",   "bfc",   "ofx",   "ofy",   "h",    "dqa",  "dqb",  "zsf3",  "zsf4",  "flag"}};

static constexpr const std::array<GTEInstructionTable, 64> s_gte_instructions = {{
  {"rtps", true, true, false},      // 0x00
  {"nclip", false, false, false},   // 0x01
  {"UNKNOWN", false, false, false}, // 0x02
  {"UNKNOWN", false, false, false}, // 0x03
  {"UNKNOWN", false, false, false}, // 0x04
  {"UNKNOWN", false, false, false}, // 0x05
  {"UNKNOWN", false, false, false}, // 0x06
  {"UNKNOWN", false, false, false}, // 0x07
  {"UNKNOWN", false, false, false}, // 0x08
  {"UNKNOWN", false, false, false}, // 0x09
  {"UNKNOWN", false, false, false}, // 0x0A
  {"UNKNOWN", false, false, false}, // 0x0B
  {"op", true, true, false},        // 0x0C
  {"UNKNOWN", false, false, false}, // 0x0D
  {"UNKNOWN", false, false, false}, // 0x0E
  {"UNKNOWN", false, false, false}, // 0x0F
  {"dpcs", true, true, false},      // 0x10
  {"intpl", true, true, false},     // 0x11
  {"mvmva", true, true, true},      // 0x12
  {"ncds", true, true, false},      // 0x13
  {"cdp", true, true, false},       // 0x14
  {"UNKNOWN", false, false, false}, // 0x15
  {"ncdt", true, true, false},      // 0x16
  {"UNKNOWN", false, false, false}, // 0x17
  {"UNKNOWN", false, false, false}, // 0x18
  {"UNKNOWN", false, false, false}, // 0x19
  {"UNKNOWN", false, false, false}, // 0x1A
  {"nccs", true, true, false},      // 0x1B
  {"cc", true, true, false},        // 0x1C
  {"UNKNOWN", false, false, false}, // 0x1D
  {"ncs", true, true, false},       // 0x1E
  {"UNKNOWN", false, false, false}, // 0x1F
  {"nct", true, true, false},       // 0x20
  {"UNKNOWN", false, false, false}, // 0x21
  {"UNKNOWN", false, false, false}, // 0x22
  {"UNKNOWN", false, false, false}, // 0x23
  {"UNKNOWN", false, false, false}, // 0x24
  {"UNKNOWN", false, false, false}, // 0x25
  {"UNKNOWN", false, false, false}, // 0x26
  {"UNKNOWN", false, false, false}, // 0x27
  {"sqr", true, true, false},       // 0x28
  {"dcpl", true, true, false},      // 0x29
  {"dpct", true, true, false},      // 0x2A
  {"UNKNOWN", false, false, false}, // 0x2B
  {"UNKNOWN", false, false, false}, // 0x2C
  {"avsz3", false, false, false},   // 0x2D
  {"avsz4", false, false, false},   // 0x2E
  {"UNKNOWN", false, false, false}, // 0x2F
  {"rtpt", true, true, false},      // 0x30
  {"UNKNOWN", false, false, false}, // 0x31
  {"UNKNOWN", false, false, false}, // 0x32
  {"UNKNOWN", false, false, false}, // 0x33
  {"UNKNOWN", false, false, false}, // 0x34
  {"UNKNOWN", false, false, false}, // 0x35
  {"UNKNOWN", false, false, false}, // 0x36
  {"UNKNOWN", false, false, false}, // 0x37
  {"UNKNOWN", false, false, false}, // 0x38
  {"UNKNOWN", false, false, false}, // 0x39
  {"UNKNOWN", false, false, false}, // 0x3A
  {"UNKNOWN", false, false, false}, // 0x3B
  {"UNKNOWN", false, false, false}, // 0x3C
  {"gpf", true, true, false},       // 0x3D
  {"gpl", true, true, false},       // 0x3E
  {"ncct", true, true, false},      // 0x3F
}};

} // namespace CPU

void CPU::FormatInstruction(SmallStringBase* dest, const Instruction inst, u32 pc, const char* format)
{
  dest->clear();

  const char* str = format;
  while (*str != '\0')
  {
    const char ch = *(str++);
    if (ch != '$')
    {
      dest->append(ch);
      continue;
    }

    if (std::strncmp(str, "rs", 2) == 0)
    {
      dest->append(GetRegName(inst.r.rs));
      str += 2;
    }
    else if (std::strncmp(str, "rt_", 3) == 0)
    {
      dest->append(GetRegName(inst.r.rt));
      str += 3;
    }
    else if (std::strncmp(str, "rt", 2) == 0)
    {
      dest->append(GetRegName(inst.r.rt));
      str += 2;
    }
    else if (std::strncmp(str, "rd", 2) == 0)
    {
      dest->append(GetRegName(inst.r.rd));
      str += 2;
    }
    else if (std::strncmp(str, "shamt", 5) == 0)
    {
      dest->append_format("{}", ZeroExtend32(inst.r.shamt.GetValue()));
      str += 5;
    }
    else if (std::strncmp(str, "immu", 4) == 0)
    {
      dest->append_format("{}", inst.i.imm_zext32());
      str += 4;
    }
    else if (std::strncmp(str, "imm", 3) == 0)
    {
      // dest->AppendFormattedString("%d", static_cast<int>(inst.i.imm_sext32()));
      dest->append_format("{:04x}", inst.i.imm_zext32());
      str += 3;
    }
    else if (std::strncmp(str, "rel", 3) == 0)
    {
      const u32 target = (pc + UINT32_C(4)) + (inst.i.imm_sext32() << 2);
      dest->append_format("{:08x}", target);
      str += 3;
    }
    else if (std::strncmp(str, "offsetrs", 8) == 0)
    {
      const s32 offset = static_cast<s32>(inst.i.imm_sext32());
      dest->append_format("{}({})", offset, GetRegName(inst.i.rs));
      str += 8;
    }
    else if (std::strncmp(str, "jt", 2) == 0)
    {
      const u32 target = ((pc + UINT32_C(4)) & UINT32_C(0xF0000000)) | (inst.j.target << 2);
      dest->append_format("{:08x}", target);
      str += 2;
    }
    else if (std::strncmp(str, "copcc", 5) == 0)
    {
      dest->append(((inst.bits & (UINT32_C(1) << 24)) != 0) ? 't' : 'f');
      str += 5;
    }
    else if (std::strncmp(str, "coprdc", 6) == 0)
    {
      if (inst.IsCop2Instruction())
        dest->append(GetGTERegisterName(static_cast<u8>(inst.r.rd.GetValue()) + 32));
      else
        dest->append_format("{}", ZeroExtend32(static_cast<u8>(inst.r.rd.GetValue())));
      str += 6;
    }
    else if (std::strncmp(str, "coprd", 5) == 0)
    {
      if (inst.IsCop2Instruction())
        dest->append(GetGTERegisterName(static_cast<u8>(inst.r.rd.GetValue())));
      else
        dest->append_format("{}", ZeroExtend32(static_cast<u8>(inst.r.rd.GetValue())));
      str += 5;
    }
    else if (std::strncmp(str, "coprt", 5) == 0)
    {
      if (inst.IsCop2Instruction())
        dest->append(GetGTERegisterName(static_cast<u8>(inst.r.rt.GetValue())));
      else
        dest->append_format("{}", ZeroExtend32(static_cast<u8>(inst.r.rt.GetValue())));
      str += 5;
    }
    else if (std::strncmp(str, "cop", 3) == 0)
    {
      dest->append_format("{}", static_cast<u8>(inst.op.GetValue()) & INSTRUCTION_COP_N_MASK);
      str += 3;
    }
    else
    {
      Panic("Unknown operand");
    }
  }
}

void CPU::FormatComment(SmallStringBase* dest, const Instruction inst, u32 pc, const char* format)
{
  const CPU::Registers* regs = &CPU::g_state.regs;

  const char* str = format;
  while (*str != '\0')
  {
    const char ch = *(str++);
    if (ch != '$')
      continue;

    if (std::strncmp(str, "rs", 2) == 0)
    {
      dest->append_format("{}{}=0x{:08X}", dest->empty() ? "" : ", ", GetRegName(inst.r.rs),
                          regs->r[static_cast<u8>(inst.r.rs.GetValue())]);

      str += 2;
    }
    else if (std::strncmp(str, "rt_", 3) == 0)
    {
      str += 3;
    }
    else if (std::strncmp(str, "rt", 2) == 0)
    {
      dest->append_format("{}{}=0x{:08X}", dest->empty() ? "" : ", ", GetRegName(inst.r.rt),
                          regs->r[static_cast<u8>(inst.r.rt.GetValue())]);
      str += 2;
    }
    else if (std::strncmp(str, "rd", 2) == 0)
    {
      dest->append_format("{}{}=0x{:08X}", dest->empty() ? "" : ", ", GetRegName(inst.r.rd),
                          regs->r[static_cast<u8>(inst.r.rd.GetValue())]);
      str += 2;
    }
    else if (std::strncmp(str, "shamt", 5) == 0)
    {
      str += 5;
    }
    else if (std::strncmp(str, "immu", 4) == 0)
    {
      str += 4;
    }
    else if (std::strncmp(str, "imm", 3) == 0)
    {
      str += 3;
    }
    else if (std::strncmp(str, "rel", 3) == 0)
    {
      str += 3;
    }
    else if (std::strncmp(str, "offsetrs", 8) == 0)
    {
      const s32 offset = static_cast<s32>(inst.i.imm_sext32());
      dest->append_format("{}addr={:08X}", dest->empty() ? "" : ", ",
                          regs->r[static_cast<u8>(inst.i.rs.GetValue())] + offset);
      str += 8;
    }
    else if (std::strncmp(str, "jt", 2) == 0)
    {
      str += 2;
    }
    else if (std::strncmp(str, "copcc", 5) == 0)
    {
      str += 5;
    }
    else if (std::strncmp(str, "coprdc", 6) == 0)
    {
      if (inst.IsCop2Instruction())
      {
        dest->append_format("{}{}=0x{:08X}", dest->empty() ? "" : ", ",
                            GetGTERegisterName(static_cast<u8>(inst.r.rd.GetValue()) + 32),
                            g_state.gte_regs.cr32[static_cast<u8>(inst.r.rd.GetValue())]);
      }
      str += 6;
    }
    else if (std::strncmp(str, "coprd", 5) == 0)
    {
      if (inst.IsCop2Instruction())
      {
        dest->append_format("{}{}=0x{:08X}", dest->empty() ? "" : ", ",
                            GetGTERegisterName(static_cast<u8>(inst.r.rd.GetValue())),
                            g_state.gte_regs.dr32[static_cast<u8>(inst.r.rd.GetValue())]);
      }

      str += 5;
    }
    else if (std::strncmp(str, "coprt", 5) == 0)
    {
      if (inst.IsCop2Instruction())
      {
        dest->append_format("{}{}=0x{:08X}", dest->empty() ? "" : ", ",
                            GetGTERegisterName(static_cast<u8>(inst.r.rt.GetValue())),
                            g_state.gte_regs.dr32[static_cast<u8>(inst.r.rt.GetValue())]);
      }

      str += 5;
    }
    else if (std::strncmp(str, "cop", 3) == 0)
    {
      str += 3;
    }
    else
    {
      Panic("Unknown operand");
    }
  }
}

template<typename T>
void CPU::FormatCopInstruction(SmallStringBase* dest, u32 pc, const Instruction inst,
                               const std::pair<T, const char*>* table, size_t table_size, T table_key)
{
  for (size_t i = 0; i < table_size; i++)
  {
    if (table[i].first == table_key)
    {
      FormatInstruction(dest, inst, pc, table[i].second);
      return;
    }
  }

  dest->format("<cop{} 0x{:08X}>", ZeroExtend32(inst.cop.cop_n.GetValue()), inst.cop.imm25.GetValue());
}

template<typename T>
void CPU::FormatCopComment(SmallStringBase* dest, u32 pc, const Instruction inst,
                           const std::pair<T, const char*>* table, size_t table_size, T table_key)
{
  for (size_t i = 0; i < table_size; i++)
  {
    if (table[i].first == table_key)
    {
      FormatComment(dest, inst, pc, table[i].second);
      return;
    }
  }
}

void CPU::FormatGTEInstruction(SmallStringBase* dest, u32 pc, const Instruction inst)
{
  const GTE::Instruction gi{inst.bits};
  const GTEInstructionTable& t = s_gte_instructions[gi.command];
  dest->append(t.name);

  if (t.sf && gi.sf)
    dest->append(" sf");

  if (t.lm && gi.lm)
    dest->append(" lm");

  if (t.mvmva)
  {
    dest->append_format(" m={} v={} t={}", static_cast<u8>(gi.mvmva_multiply_matrix),
                        static_cast<u8>(gi.mvmva_multiply_vector), static_cast<u8>(gi.mvmva_translation_vector));
  }
}

void CPU::DisassembleInstruction(SmallStringBase* dest, u32 pc, u32 bits)
{
  const Instruction inst{bits};
  switch (inst.op)
  {
    case InstructionOp::funct:
      FormatInstruction(dest, inst, pc, s_special_table[static_cast<u8>(inst.r.funct.GetValue())]);
      return;

    case InstructionOp::cop0:
    case InstructionOp::cop1:
    case InstructionOp::cop2:
    case InstructionOp::cop3:
    {
      if (inst.cop.IsCommonInstruction())
      {
        FormatCopInstruction(dest, pc, inst, s_cop_common_table.data(), s_cop_common_table.size(), inst.cop.CommonOp());
      }
      else
      {
        switch (inst.op)
        {
          case InstructionOp::cop0:
          {
            FormatCopInstruction(dest, pc, inst, s_cop0_table.data(), s_cop0_table.size(), inst.cop.Cop0Op());
          }
          break;

          case InstructionOp::cop2:
          {
            FormatGTEInstruction(dest, pc, inst);
          }
          break;

          case InstructionOp::cop1:
          case InstructionOp::cop3:
          default:
          {
            dest->format("<cop{} 0x{:08X}>", ZeroExtend32(inst.cop.cop_n.GetValue()), inst.cop.imm25.GetValue());
          }
          break;
        }
      }
    }
    break;

    // special case for bltz/bgez{al}
    case InstructionOp::b:
    {
      const u8 rt = static_cast<u8>(inst.i.rt.GetValue());
      const bool bgez = ConvertToBoolUnchecked(rt & u8(1));
      const bool link = ConvertToBoolUnchecked((rt >> 4) & u8(1));
      if (link)
        FormatInstruction(dest, inst, pc, bgez ? "bgezal $rs, $rel" : "bltzal $rs, $rel");
      else
        FormatInstruction(dest, inst, pc, bgez ? "bgez $rs, $rel" : "bltz $rs, $rel");
    }
    break;

    default:
      FormatInstruction(dest, inst, pc, s_base_table[static_cast<u8>(inst.op.GetValue())]);
      break;
  }
}

void CPU::DisassembleInstructionComment(SmallStringBase* dest, u32 pc, u32 bits)
{
  const Instruction inst{bits};
  switch (inst.op)
  {
    case InstructionOp::funct:
      FormatComment(dest, inst, pc, s_special_table[static_cast<u8>(inst.r.funct.GetValue())]);
      return;

    case InstructionOp::cop0:
    case InstructionOp::cop1:
    case InstructionOp::cop2:
    case InstructionOp::cop3:
    {
      if (inst.cop.IsCommonInstruction())
      {
        FormatCopComment(dest, pc, inst, s_cop_common_table.data(), s_cop_common_table.size(), inst.cop.CommonOp());
      }
      else
      {
        switch (inst.op)
        {
          case InstructionOp::cop0:
          {
            FormatCopComment(dest, pc, inst, s_cop0_table.data(), s_cop0_table.size(), inst.cop.Cop0Op());
          }
          break;

          case InstructionOp::cop2:
            // TODO: Show GTE regs?
            break;

          case InstructionOp::cop1:
          case InstructionOp::cop3:
          default:
          {
            dest->format("<cop{} 0x{:08X}>", ZeroExtend32(inst.cop.cop_n.GetValue()), inst.cop.imm25.GetValue());
          }
          break;
        }
      }
    }
    break;

      // special case for bltz/bgez{al}
    case InstructionOp::b:
    {
      const u8 rt = static_cast<u8>(inst.i.rt.GetValue());
      const bool bgez = ConvertToBoolUnchecked(rt & u8(1));
      const bool link = ConvertToBoolUnchecked((rt >> 4) & u8(1));
      if (link)
        FormatComment(dest, inst, pc, bgez ? "bgezal $rs, $rel" : "bltzal $rs, $rel");
      else
        FormatComment(dest, inst, pc, bgez ? "bgez $rs, $rel" : "bltz $rs, $rel");
    }
    break;

    default:
      FormatComment(dest, inst, pc, s_base_table[static_cast<u8>(inst.op.GetValue())]);
      break;
  }
}

const char* CPU::GetGTERegisterName(u32 index)
{
  return (index < s_gte_register_names.size()) ? s_gte_register_names[index] : "";
}
