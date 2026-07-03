// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_disasm.h"
#include "cpu_core.h"
#include "cpu_types.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include <array>
#include <limits>

namespace CPU {
namespace {

enum class AssembleResult
{
  NoMatch,
  Success,
  Error,
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

static std::string_view GetMnemonic(std::string_view text);
static std::string_view GetOperands(std::string_view text);
static std::string GetFormatMnemonic(const char* format, u32 cop_n);
static bool SplitOperands(std::string_view text, std::array<std::string_view, 4>* operands, size_t* count);
static bool ParseSignedValue(std::string_view text, s64* value);
static bool ParseUnsignedValue(std::string_view text, u64* value);
static bool ParseGPR(std::string_view text, u32* index);
static bool ParseNumericRegister(std::string_view text, u32* index);
static bool ParseCopRegister(std::string_view text, u32 cop_n, bool control, u32* index);
static bool SetField(u32* bits, u32 shift, u32 width, u32 value);
static bool EncodeAssemblyTarget(u32* bits, u32 pc, u32 target, bool jump, Error* error);
static std::optional<u32> ResolveAssemblyTarget(AssemblyLabelList* labels, std::string_view operand, u32 placeholder,
                                                u32* instruction, u32 pc);
static bool AssembleFormat(u32* bits, u32* instruction, u32 pc, const char* format, std::string_view text,
                           AssemblyLabelList* labels, Error* error);
static AssembleResult TryTableEntry(u32* dest, u32 pc, std::string_view text, const char* format, u32 base_bits,
                                    AssemblyLabelList* labels, Error* error);
static AssembleResult AssembleGTEInstruction(u32* dest, std::string_view text, Error* error);

static const std::array<const char*, 64> s_base_table = {{
  "",                       // 0
  nullptr,                  // 1
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
  "andi $rt, $rs, $immx",   // 12
  "ori $rt, $rs, $immx",    // 13
  "xori $rt, $rs, $immx",   // 14
  "lui $rt, $immx",         // 15
  nullptr,                  // 16
  nullptr,                  // 17
  nullptr,                  // 18
  nullptr,                  // 19
  nullptr,                  // 20
  nullptr,                  // 21
  nullptr,                  // 22
  nullptr,                  // 23
  nullptr,                  // 24
  nullptr,                  // 25
  nullptr,                  // 26
  nullptr,                  // 27
  nullptr,                  // 28
  nullptr,                  // 29
  nullptr,                  // 30
  nullptr,                  // 31
  "lb $rt, $offsetrs",      // 32
  "lh $rt, $offsetrs",      // 33
  "lwl $rt, $offsetrs",     // 34
  "lw $rt, $offsetrs",      // 35
  "lbu $rt, $offsetrs",     // 36
  "lhu $rt, $offsetrs",     // 37
  "lwr $rt, $offsetrs",     // 38
  nullptr,                  // 39
  "sb $rt, $offsetrs",      // 40
  "sh $rt, $offsetrs",      // 41
  "swl $rt, $offsetrs",     // 42
  "sw $rt, $offsetrs",      // 43
  nullptr,                  // 44
  nullptr,                  // 45
  "swr $rt, $offsetrs",     // 46
  nullptr,                  // 47
  "lwc0 $coprt, $offsetrs", // 48
  "lwc1 $coprt, $offsetrs", // 49
  "lwc2 $coprt, $offsetrs", // 50
  "lwc3 $coprt, $offsetrs", // 51
  nullptr,                  // 52
  nullptr,                  // 53
  nullptr,                  // 54
  nullptr,                  // 55
  "swc0 $coprt, $offsetrs", // 56
  "swc1 $coprt, $offsetrs", // 57
  "swc2 $coprt, $offsetrs", // 58
  "swc3 $coprt, $offsetrs", // 59
  nullptr,                  // 60
  nullptr,                  // 61
  nullptr,                  // 62
  nullptr                   // 63
}};

static const std::array<const char*, 64> s_special_table = {{
  "sll $rd, $rt, $shamt", // 0
  nullptr,                // 1
  "srl $rd, $rt, $shamt", // 2
  "sra $rd, $rt, $shamt", // 3
  "sllv $rd, $rt, $rs",   // 4
  nullptr,                // 5
  "srlv $rd, $rt, $rs",   // 6
  "srav $rd, $rt, $rs",   // 7
  "jr $rs",               // 8
  "jalr $rd, $rs",        // 9
  nullptr,                // 10
  nullptr,                // 11
  "syscall",              // 12
  "break",                // 13
  nullptr,                // 14
  nullptr,                // 15
  "mfhi $rd",             // 16
  "mthi $rs",             // 17
  "mflo $rd",             // 18
  "mtlo $rs",             // 19
  nullptr,                // 20
  nullptr,                // 21
  nullptr,                // 22
  nullptr,                // 23
  "mult $rs, $rt",        // 24
  "multu $rs, $rt",       // 25
  "div $rs, $rt",         // 26
  "divu $rs, $rt",        // 27
  nullptr,                // 28
  nullptr,                // 29
  nullptr,                // 30
  nullptr,                // 31
  "add $rd, $rs, $rt",    // 32
  "addu $rd, $rs, $rt",   // 33
  "sub $rd, $rs, $rt",    // 34
  "subu $rd, $rs, $rt",   // 35
  "and $rd, $rs, $rt",    // 36
  "or $rd, $rs, $rt",     // 37
  "xor $rd, $rs, $rt",    // 38
  "nor $rd, $rs, $rt",    // 39
  nullptr,                // 40
  nullptr,                // 41
  "slt $rd, $rs, $rt",    // 42
  "sltu $rd, $rs, $rt",   // 43
  nullptr,                // 44
  nullptr,                // 45
  nullptr,                // 46
  nullptr,                // 47
  nullptr,                // 48
  nullptr,                // 49
  nullptr,                // 50
  nullptr,                // 51
  nullptr,                // 52
  nullptr,                // 53
  nullptr,                // 54
  nullptr,                // 55
  nullptr,                // 56
  nullptr,                // 57
  nullptr,                // 58
  nullptr,                // 59
  nullptr,                // 60
  nullptr,                // 61
  nullptr,                // 62
  nullptr                 // 63
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

static constexpr const std::array<const char*, 32> s_cop0_register_names = {
  {"$0",   "$1",  "$2",    "BPC", "$4",   "BDA", "TAR", "DCIC", "BadA", "BDAM", "$10",
   "BPCM", "SR",  "CAUSE", "EPC", "PRID", "$16", "$17", "$18",  "$19",  "$20",  "$21",
   "$22",  "$23", "$24",   "$25", "$26",  "$27", "$28", "$29",  "$30",  "$31"}};

static constexpr const std::array<GTEInstructionTable, 64> s_gte_instructions = {{
  {nullptr, false, false, false}, // 0x00
  {"rtps", true, true, false},    // 0x01
  {nullptr, false, false, false}, // 0x02
  {nullptr, false, false, false}, // 0x03
  {nullptr, false, false, false}, // 0x04
  {nullptr, false, false, false}, // 0x05
  {"nclip", false, false, false}, // 0x06
  {nullptr, false, false, false}, // 0x07
  {nullptr, false, false, false}, // 0x08
  {nullptr, false, false, false}, // 0x09
  {nullptr, false, false, false}, // 0x0A
  {nullptr, false, false, false}, // 0x0B
  {"op", true, true, false},      // 0x0C
  {nullptr, false, false, false}, // 0x0D
  {nullptr, false, false, false}, // 0x0E
  {nullptr, false, false, false}, // 0x0F
  {"dpcs", true, true, false},    // 0x10
  {"intpl", true, true, false},   // 0x11
  {"mvmva", true, true, true},    // 0x12
  {"ncds", true, true, false},    // 0x13
  {"cdp", true, true, false},     // 0x14
  {nullptr, false, false, false}, // 0x15
  {"ncdt", true, true, false},    // 0x16
  {nullptr, false, false, false}, // 0x17
  {nullptr, false, false, false}, // 0x18
  {nullptr, false, false, false}, // 0x19
  {nullptr, false, false, false}, // 0x1A
  {"nccs", true, true, false},    // 0x1B
  {"cc", true, true, false},      // 0x1C
  {nullptr, false, false, false}, // 0x1D
  {"ncs", true, true, false},     // 0x1E
  {nullptr, false, false, false}, // 0x1F
  {"nct", true, true, false},     // 0x20
  {nullptr, false, false, false}, // 0x21
  {nullptr, false, false, false}, // 0x22
  {nullptr, false, false, false}, // 0x23
  {nullptr, false, false, false}, // 0x24
  {nullptr, false, false, false}, // 0x25
  {nullptr, false, false, false}, // 0x26
  {nullptr, false, false, false}, // 0x27
  {"sqr", true, true, false},     // 0x28
  {"dcpl", true, true, false},    // 0x29
  {"dpct", true, true, false},    // 0x2A
  {nullptr, false, false, false}, // 0x2B
  {nullptr, false, false, false}, // 0x2C
  {"avsz3", false, false, false}, // 0x2D
  {"avsz4", false, false, false}, // 0x2E
  {nullptr, false, false, false}, // 0x2F
  {"rtpt", true, true, false},    // 0x30
  {nullptr, false, false, false}, // 0x31
  {nullptr, false, false, false}, // 0x32
  {nullptr, false, false, false}, // 0x33
  {nullptr, false, false, false}, // 0x34
  {nullptr, false, false, false}, // 0x35
  {nullptr, false, false, false}, // 0x36
  {nullptr, false, false, false}, // 0x37
  {nullptr, false, false, false}, // 0x38
  {nullptr, false, false, false}, // 0x39
  {nullptr, false, false, false}, // 0x3A
  {nullptr, false, false, false}, // 0x3B
  {nullptr, false, false, false}, // 0x3C
  {"gpf", true, true, false},     // 0x3D
  {"gpl", true, true, false},     // 0x3E
  {"ncct", true, true, false},    // 0x3F
}};

} // namespace CPU

void CPU::FormatInstruction(SmallStringBase* dest, const Instruction inst, u32 pc, const char* format)
{
  dest->clear();

  if (!format)
  {
    dest->assign("UNKNOWN");
    return;
  }

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
    else if (std::strncmp(str, "immx", 4) == 0)
    {
      dest->append_format("0x{:04x}", inst.i.imm_zext32());
      str += 4;
    }
    else if (std::strncmp(str, "imm", 3) == 0)
    {
      dest->append_format("{}", static_cast<s32>(inst.i.imm_sext32()));
      str += 3;
    }
    else if (std::strncmp(str, "rel", 3) == 0)
    {
      const u32 target = (pc + UINT32_C(4)) + (inst.i.imm_sext32() << 2);
      dest->append_format("0x{:08x}", target);
      str += 3;
    }
    else if (std::strncmp(str, "offsetrs", 8) == 0)
    {
      const s32 offset = static_cast<s32>(inst.i.imm_sext32());
      dest->append_format("{}0x{:x}({})", (offset < 0) ? "-" : "", (offset < 0) ? -offset : offset,
                          GetRegName(inst.i.rs));
      str += 8;
    }
    else if (std::strncmp(str, "jt", 2) == 0)
    {
      const u32 target = ((pc + UINT32_C(4)) & UINT32_C(0xF0000000)) | (inst.j.target << 2);
      dest->append_format("0x{:08x}", target);
      str += 2;
    }
    else if (std::strncmp(str, "copcc", 5) == 0)
    {
      dest->append(((inst.bits & (UINT32_C(1) << 24)) != 0) ? 't' : 'f');
      str += 5;
    }
    else if (std::strncmp(str, "coprdc", 6) == 0)
    {
      if (inst.cop.cop_n == 2)
        dest->append(GetGTERegisterName(static_cast<u8>(inst.r.rd.GetValue()) + 32));
      else
        dest->append_format("{}", ZeroExtend32(static_cast<u8>(inst.r.rd.GetValue())));
      str += 6;
    }
    else if (std::strncmp(str, "coprd", 5) == 0)
    {
      if (inst.cop.cop_n == 2)
        dest->append(GetGTERegisterName(static_cast<u8>(inst.r.rd.GetValue())));
      else if (inst.cop.cop_n == 0)
        dest->append(GetCop0RegisterName(static_cast<u8>(inst.r.rd.GetValue())));
      else
        dest->append_format("{}", ZeroExtend32(static_cast<u8>(inst.r.rd.GetValue())));
      str += 5;
    }
    else if (std::strncmp(str, "coprt", 5) == 0)
    {
      if (inst.cop.cop_n == 2)
        dest->append(GetGTERegisterName(static_cast<u8>(inst.r.rt.GetValue())));
      else if (inst.cop.cop_n == 0)
        dest->append(GetCop0RegisterName(static_cast<u8>(inst.r.rt.GetValue())));
      else
        dest->append_format("{}", ZeroExtend32(static_cast<u8>(inst.r.rt.GetValue())));
      str += 5;
    }
    else if (std::strncmp(str, "cop", 3) == 0)
    {
      dest->append_format("{}", inst.cop.cop_n.GetValue());
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
  if (!format)
    return;

  const CPU::Registers* regs = &CPU::g_state.regs;

  const char* str = format;
  while (*str != '\0')
  {
    const char ch = *(str++);
    if (ch != '$')
      continue;

    if (std::strncmp(str, "rs", 2) == 0)
    {
      dest->append_format("{}{}=0x{:08x}", dest->empty() ? "" : ", ", GetRegName(inst.r.rs),
                          regs->r[static_cast<u8>(inst.r.rs.GetValue())]);

      str += 2;
    }
    else if (std::strncmp(str, "rt_", 3) == 0)
    {
      str += 3;
    }
    else if (std::strncmp(str, "rt", 2) == 0)
    {
      dest->append_format("{}{}=0x{:08x}", dest->empty() ? "" : ", ", GetRegName(inst.r.rt),
                          regs->r[static_cast<u8>(inst.r.rt.GetValue())]);
      str += 2;
    }
    else if (std::strncmp(str, "rd", 2) == 0)
    {
      dest->append_format("{}{}=0x{:08x}", dest->empty() ? "" : ", ", GetRegName(inst.r.rd),
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
      const VirtualMemoryAddress address = (regs->r[static_cast<u8>(inst.i.rs.GetValue())] + offset);

      if (!dest->empty())
        dest->append_format(", ");

      if (inst.op == InstructionOp::lb || inst.op == InstructionOp::lbu)
      {
        u8 data = 0;
        CPU::SafeReadMemoryByte(address, &data);
        dest->append_format("addr=0x{:08x}[0x{:02x}]", address, data);
      }
      else if (inst.op == InstructionOp::lh || inst.op == InstructionOp::lhu)
      {
        u16 data = 0;
        CPU::SafeReadMemoryHalfWord(address, &data);
        dest->append_format("addr=0x{:08x}[0x{:04x}]", address, data);
      }
      else if (inst.op == InstructionOp::lw || (inst.op >= InstructionOp::lwc0 && inst.op <= InstructionOp::lwc3) ||
               inst.op == InstructionOp::lwl || inst.op == InstructionOp::lwr)
      {
        u32 data = 0;
        CPU::SafeReadMemoryWord(address, &data);
        dest->append_format("addr=0x{:08x}[0x{:08x}]", address, data);
      }
      else
      {
        dest->append_format("addr=0x{:08x}", address);
      }

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
      if (inst.cop.cop_n == 2)
      {
        dest->append_format("{}{}=0x{:08x}", dest->empty() ? "" : ", ",
                            GetGTERegisterName(static_cast<u8>(inst.r.rd.GetValue()) + 32),
                            g_state.gte_regs.cr32[static_cast<u8>(inst.r.rd.GetValue())]);
      }

      str += 6;
    }
    else if (std::strncmp(str, "coprd", 5) == 0)
    {
      if (inst.cop.cop_n == 2)
      {
        dest->append_format("{}{}=0x{:08x}", dest->empty() ? "" : ", ",
                            GetGTERegisterName(static_cast<u8>(inst.r.rd.GetValue())),
                            g_state.gte_regs.dr32[static_cast<u8>(inst.r.rd.GetValue())]);
      }
      else if (inst.cop.cop_n == 0)
      {
        dest->append_format("{}{}", dest->empty() ? "" : ", ",
                            GetCop0RegisterName(static_cast<u8>(inst.r.rd.GetValue())));

        u32 value = 0;
        switch (static_cast<Cop0Reg>(inst.r.rd.GetValue()))
        {
          case Cop0Reg::BPC:
            value = g_state.cop0_regs.BPC;
            break;

          case Cop0Reg::BPCM:
            value = g_state.cop0_regs.BPCM;
            break;

          case Cop0Reg::BDA:
            value = g_state.cop0_regs.BDA;
            break;

          case Cop0Reg::BDAM:
            value = g_state.cop0_regs.BDAM;
            break;

          case Cop0Reg::DCIC:
            value = g_state.cop0_regs.dcic.bits;
            break;

          case Cop0Reg::JUMPDEST:
            value = g_state.cop0_regs.TAR;
            break;

          case Cop0Reg::BadVaddr:
            value = g_state.cop0_regs.BadVaddr;
            break;

          case Cop0Reg::SR:
            value = g_state.cop0_regs.sr.bits;
            break;

          case Cop0Reg::CAUSE:
            value = g_state.cop0_regs.cause.bits;
            break;

          case Cop0Reg::EPC:
            value = g_state.cop0_regs.EPC;
            break;

          case Cop0Reg::PRID:
            value = g_state.cop0_regs.PRID;
            break;
        }

        dest->append_format("=0x{:08x}", value);
      }

      str += 5;
    }
    else if (std::strncmp(str, "coprt", 5) == 0)
    {
      if (inst.cop.cop_n == 2)
      {
        dest->append_format("{}{}=0x{:08x}", dest->empty() ? "" : ", ",
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

  dest->format("cop{} 0x{:08x}", ZeroExtend32(inst.cop.cop_n.GetValue()), inst.cop.imm25.GetValue());
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
  if (!t.name)
  {
    dest->assign("UNKNOWN");
    return;
  }

  dest->assign(t.name);

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
            dest->format("cop{} 0x{:08x}", ZeroExtend32(inst.cop.cop_n.GetValue()), inst.cop.imm25.GetValue());
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
      const bool link = (rt & u8(0x1E)) == u8(0x10);
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
            dest->format("cop{} 0x{:08x}", ZeroExtend32(inst.cop.cop_n.GetValue()), inst.cop.imm25.GetValue());
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

const char* CPU::GetCop0RegisterName(u32 index)
{
  return (index < s_cop0_register_names.size()) ? s_cop0_register_names[index] : "";
}

std::string_view CPU::GetMnemonic(std::string_view text)
{
  const size_t end = text.find_first_of(" \t\r\n");
  return text.substr(0, end);
}

std::string_view CPU::GetOperands(std::string_view text)
{
  const size_t end = text.find_first_of(" \t\r\n");
  return (end == std::string_view::npos) ? std::string_view() : StringUtil::StripWhitespace(text.substr(end));
}

std::string CPU::GetFormatMnemonic(const char* format, u32 cop_n)
{
  std::string mnemonic;
  for (const char* ptr = format; *ptr && !StringUtil::IsWhitespace(*ptr);)
  {
    if (std::strncmp(ptr, "$cop", 4) == 0)
    {
      mnemonic.push_back(static_cast<char>('0' + cop_n));
      ptr += 4;
    }
    else
    {
      mnemonic.push_back(*ptr++);
    }
  }
  return mnemonic;
}

bool CPU::SplitOperands(std::string_view text, std::array<std::string_view, 4>* operands, size_t* count)
{
  *count = 0;
  text = StringUtil::StripWhitespace(text);
  if (text.empty())
    return true;

  size_t start = 0;
  u32 parentheses = 0;
  for (size_t i = 0; i <= text.size(); i++)
  {
    const char ch = (i < text.size()) ? text[i] : ',';
    if (ch == '(')
      parentheses++;
    else if (ch == ')')
    {
      if (parentheses == 0)
        return false;
      parentheses--;
    }
    else if (ch == ',' && parentheses == 0)
    {
      if (*count == operands->size())
        return false;

      const std::string_view operand = StringUtil::StripWhitespace(text.substr(start, i - start));
      if (operand.empty())
        return false;
      (*operands)[(*count)++] = operand;
      start = i + 1;
    }
  }

  return (parentheses == 0);
}

bool CPU::ParseSignedValue(std::string_view text, s64* value)
{
  text = StringUtil::StripWhitespace(text);
  const bool negative = (!text.empty() && text.front() == '-');
  if (negative || (!text.empty() && text.front() == '+'))
    text.remove_prefix(1);
  if (text.empty())
    return false;

  std::string_view end;
  const std::optional<u64> parsed = StringUtil::FromCharsWithOptionalBase<u64>(text, &end);
  static constexpr u64 negative_limit = static_cast<u64>(std::numeric_limits<s64>::max()) + 1;
  if (!parsed.has_value() || !end.empty() ||
      parsed.value() > (negative ? negative_limit : static_cast<u64>(std::numeric_limits<s64>::max())))
  {
    return false;
  }

  if (negative)
  {
    *value = (parsed.value() == negative_limit) ? std::numeric_limits<s64>::min() : -static_cast<s64>(parsed.value());
  }
  else
  {
    *value = static_cast<s64>(parsed.value());
  }
  return true;
}

bool CPU::ParseUnsignedValue(std::string_view text, u64* value)
{
  text = StringUtil::StripWhitespace(text);
  std::string_view end;
  const std::optional<u64> parsed = StringUtil::FromCharsWithOptionalBase<u64>(text, &end);
  if (!parsed.has_value() || !end.empty())
    return false;
  *value = parsed.value();
  return true;
}

bool CPU::ParseGPR(std::string_view text, u32* index)
{
  text = StringUtil::StripWhitespace(text);
  if (!text.empty() && text.front() == '$')
    text.remove_prefix(1);

  for (u32 i = 0; i < static_cast<u32>(Reg::count); i++)
  {
    if (StringUtil::EqualNoCase(text, GetRegName(static_cast<Reg>(i))))
    {
      *index = i;
      return true;
    }
  }
  return false;
}

bool CPU::ParseNumericRegister(std::string_view text, u32* index)
{
  text = StringUtil::StripWhitespace(text);
  if (!text.empty() && text.front() == '$')
    text.remove_prefix(1);

  u64 value;
  if (!ParseUnsignedValue(text, &value) || value >= 32)
    return false;
  *index = static_cast<u32>(value);
  return true;
}

bool CPU::ParseCopRegister(std::string_view text, u32 cop_n, bool control, u32* index)
{
  if (cop_n == 2)
  {
    const u32 start = control ? 32 : 0;
    for (u32 i = 0; i < 32; i++)
    {
      if (StringUtil::EqualNoCase(text, s_gte_register_names[start + i]))
      {
        *index = i;
        return true;
      }
    }
  }
  else if (cop_n == 0 && !control)
  {
    for (u32 i = 0; i < s_cop0_register_names.size(); i++)
    {
      if (StringUtil::EqualNoCase(text, s_cop0_register_names[i]))
      {
        *index = i;
        return true;
      }
    }
  }

  return ParseNumericRegister(text, index);
}

bool CPU::SetField(u32* bits, u32 shift, u32 width, u32 value)
{
  const u32 mask = ((UINT32_C(1) << width) - 1) << shift;
  *bits = (*bits & ~mask) | (value << shift);
  return true;
}

bool CPU::EncodeAssemblyTarget(u32* bits, const u32 pc, const u32 target, const bool jump, Error* error)
{
  if (jump)
  {
    if ((target & 3) != 0 || (target & 0xF0000000u) != ((pc + 4) & 0xF0000000u))
    {
      Error::SetStringFmt(error, "Jump target 0x{:08X} is outside the current 256 MB region.", target);
      return false;
    }

    SetField(bits, 0, 26, (target & ~0xF0000000u) >> 2);
    return true;
  }

  const u32 delta = target - (pc + 4);
  const s64 word_delta = static_cast<s64>(static_cast<s32>(delta)) / 4;
  if ((delta & 3) != 0 || word_delta < std::numeric_limits<s16>::min() || word_delta > std::numeric_limits<s16>::max())
  {
    Error::SetStringFmt(error, "Branch target 0x{:08X} is out of range.", target);
    return false;
  }

  SetField(bits, 0, 16, static_cast<u16>(word_delta));
  return true;
}

std::optional<u32> CPU::ResolveAssemblyTarget(AssemblyLabelList* labels, const std::string_view operand,
                                              const u32 placeholder, u32* instruction, const u32 pc)
{
  if (!labels || operand.empty() || operand.front() == '+' || operand.front() == '-' ||
      (operand.front() >= '0' && operand.front() <= '9'))
  {
    return std::nullopt;
  }

  const auto iter = std::ranges::find(*labels, operand, &AssemblyLabel::name);
  if (iter == labels->end())
  {
    labels->emplace_back(std::string(operand), std::nullopt, std::vector<u32>());
    labels->back().backreferences.push_back(pc);
    return placeholder;
  }

  if (!iter->address.has_value())
  {
    if (std::ranges::find(iter->backreferences, pc) == iter->backreferences.end())
      iter->backreferences.push_back(pc);
  }

  return iter->address.value_or(placeholder);
}

bool CPU::AssembleFormat(u32* bits, u32* instruction, u32 pc, const char* format, std::string_view text,
                         AssemblyLabelList* labels, Error* error)
{
  const std::string_view format_view(format);
  const size_t format_space = format_view.find_first_of(" \t\r\n");
  const std::string_view format_operands = (format_space == std::string_view::npos) ?
                                             std::string_view() :
                                             StringUtil::StripWhitespace(format_view.substr(format_space));

  std::array<std::string_view, 4> expected;
  std::array<std::string_view, 4> actual;
  size_t expected_count;
  size_t actual_count;
  if (!SplitOperands(format_operands, &expected, &expected_count) ||
      !SplitOperands(GetOperands(text), &actual, &actual_count) || expected_count != actual_count)
  {
    Error::SetStringFmt(error, "Expected operands matching '{}'.", format);
    return false;
  }

  const u32 cop_n = (*bits >> 26) & 3;
  for (size_t i = 0; i < expected_count; i++)
  {
    const std::string_view type = expected[i];
    const std::string_view operand = actual[i];
    u32 reg;

    if (type == "$rs" || type == "$rt" || type == "$rt_" || type == "$rd")
    {
      if (!ParseGPR(operand, &reg))
      {
        Error::SetStringFmt(error, "Invalid general-purpose register '{}'.", operand);
        return false;
      }

      const u32 shift = (type == "$rs") ? 21 : ((type == "$rd") ? 11 : 16);
      SetField(bits, shift, 5, reg);
    }
    else if (type == "$coprd" || type == "$coprdc" || type == "$coprt")
    {
      const bool control = (type == "$coprdc");
      if (!ParseCopRegister(operand, cop_n, control, &reg))
      {
        Error::SetStringFmt(error, "Invalid coprocessor register '{}'.", operand);
        return false;
      }
      SetField(bits, (type == "$coprt") ? 16 : 11, 5, reg);
    }
    else if (type == "$shamt")
    {
      u64 value;
      if (!ParseUnsignedValue(operand, &value) || value >= 32)
      {
        Error::SetStringFmt(error, "Shift amount '{}' is outside the range 0..31.", operand);
        return false;
      }
      SetField(bits, 6, 5, static_cast<u32>(value));
    }
    else if (type == "$imm")
    {
      s64 value;
      if (!ParseSignedValue(operand, &value) || value < std::numeric_limits<s16>::min() ||
          value > std::numeric_limits<s16>::max())
      {
        Error::SetStringFmt(error, "Immediate '{}' does not fit in a signed 16-bit field.", operand);
        return false;
      }
      SetField(bits, 0, 16, static_cast<u16>(value));
    }
    else if (type == "$immu" || type == "$immx")
    {
      u64 value;
      if (!ParseUnsignedValue(operand, &value) || value > std::numeric_limits<u16>::max())
      {
        Error::SetStringFmt(error, "Immediate '{}' does not fit in an unsigned 16-bit field.", operand);
        return false;
      }
      SetField(bits, 0, 16, static_cast<u32>(value));
    }
    else if (type == "$rel")
    {
      u64 target_value;
      if (!ParseUnsignedValue(operand, &target_value))
      {
        const std::optional<u32> label_target = ResolveAssemblyTarget(labels, operand, pc + 4, instruction, pc);
        if (!label_target.has_value())
        {
          Error::SetStringFmt(error, "Branch target '{}' is not an aligned 32-bit address.", operand);
          return false;
        }
        target_value = label_target.value();
      }
      if (target_value > std::numeric_limits<u32>::max() || (target_value & 3) != 0)
      {
        Error::SetStringFmt(error, "Branch target '{}' is not an aligned 32-bit address.", operand);
        return false;
      }

      if (!EncodeAssemblyTarget(bits, pc, static_cast<u32>(target_value), false, error))
        return false;
    }
    else if (type == "$jt")
    {
      u64 target_value;
      if (!ParseUnsignedValue(operand, &target_value))
      {
        const std::optional<u32> label_target = ResolveAssemblyTarget(labels, operand, pc + 4, instruction, pc);
        if (!label_target.has_value())
        {
          Error::SetStringFmt(error, "Jump target '{}' is not an aligned 32-bit address.", operand);
          return false;
        }
        target_value = label_target.value();
      }
      if (target_value > std::numeric_limits<u32>::max() || (target_value & 3) != 0)
      {
        Error::SetStringFmt(error, "Jump target '{}' is not an aligned 32-bit address.", operand);
        return false;
      }

      if (!EncodeAssemblyTarget(bits, pc, static_cast<u32>(target_value), true, error))
        return false;
    }
    else if (type == "$offsetrs")
    {
      const size_t left_paren = operand.find('(');
      const size_t right_paren = operand.rfind(')');
      if (left_paren == std::string_view::npos || right_paren != operand.size() - 1 || left_paren >= right_paren)
      {
        Error::SetStringFmt(error, "Invalid memory operand '{}'.", operand);
        return false;
      }

      s64 offset;
      if (!ParseSignedValue(operand.substr(0, left_paren), &offset) || offset < std::numeric_limits<s16>::min() ||
          offset > std::numeric_limits<s16>::max())
      {
        Error::SetStringFmt(error, "Memory offset in '{}' does not fit in a signed 16-bit field.", operand);
        return false;
      }
      if (!ParseGPR(operand.substr(left_paren + 1, right_paren - left_paren - 1), &reg))
      {
        Error::SetStringFmt(error, "Invalid base register in '{}'.", operand);
        return false;
      }
      SetField(bits, 21, 5, reg);
      SetField(bits, 0, 16, static_cast<u16>(offset));
    }
    else
    {
      Error::SetStringFmt(error, "Unsupported assembler operand type '{}'.", type);
      return false;
    }
  }

  return true;
}

CPU::AssembleResult CPU::TryTableEntry(u32* dest, u32 pc, std::string_view text, const char* format, u32 base_bits,
                                       AssemblyLabelList* labels, Error* error)
{
  if (!format) // Unknown
    return AssembleResult::NoMatch;

  const u32 cop_n = (base_bits >> 26) & 3;
  if (!StringUtil::EqualNoCase(GetMnemonic(text), GetFormatMnemonic(format, cop_n)))
    return AssembleResult::NoMatch;

  u32 bits = base_bits;
  if (!AssembleFormat(&bits, dest, pc, format, text, labels, error))
    return AssembleResult::Error;

  *dest = bits;
  return AssembleResult::Success;
}

CPU::AssembleResult CPU::AssembleGTEInstruction(u32* dest, std::string_view text, Error* error)
{
  const std::string_view mnemonic = GetMnemonic(text);
  const GTEInstructionTable* entry = nullptr;
  u32 command = 0;
  for (u32 i = 0; i < s_gte_instructions.size(); i++)
  {
    if (s_gte_instructions[i].name && StringUtil::EqualNoCase(mnemonic, s_gte_instructions[i].name))
    {
      entry = &s_gte_instructions[i];
      command = i;
      break;
    }
  }
  if (!entry)
    return AssembleResult::NoMatch;

  u32 bits = (static_cast<u32>(InstructionOp::cop2) << 26) | (UINT32_C(1) << 25) | command;
  std::string_view options = GetOperands(text);
  bool seen_sf = false;
  bool seen_lm = false;
  bool seen_m = false;
  bool seen_v = false;
  bool seen_t = false;
  while (!options.empty())
  {
    const size_t end = options.find_first_of(" \t\r\n");
    const std::string_view option = options.substr(0, end);
    options = (end == std::string_view::npos) ? std::string_view() : StringUtil::StripWhitespace(options.substr(end));

    if (StringUtil::EqualNoCase(option, "sf"))
    {
      if (!entry->sf || seen_sf)
      {
        Error::SetStringFmt(error, "Invalid or duplicate GTE option '{}'.", option);
        return AssembleResult::Error;
      }
      seen_sf = true;
      bits |= UINT32_C(1) << 19;
    }
    else if (StringUtil::EqualNoCase(option, "lm"))
    {
      if (!entry->lm || seen_lm)
      {
        Error::SetStringFmt(error, "Invalid or duplicate GTE option '{}'.", option);
        return AssembleResult::Error;
      }
      seen_lm = true;
      bits |= UINT32_C(1) << 10;
    }
    else
    {
      const size_t equals = option.find('=');
      if (!entry->mvmva || equals == std::string_view::npos || equals != 1)
      {
        Error::SetStringFmt(error, "Invalid GTE option '{}'.", option);
        return AssembleResult::Error;
      }

      u64 value;
      if (!ParseUnsignedValue(option.substr(2), &value) || value >= 4)
      {
        Error::SetStringFmt(error, "GTE option '{}' must have a value from 0 to 3.", option);
        return AssembleResult::Error;
      }

      const char name = StringUtil::ToLower(option[0]);
      bool* seen = (name == 'm') ? &seen_m : ((name == 'v') ? &seen_v : ((name == 't') ? &seen_t : nullptr));
      const u32 shift = (name == 'm') ? 17 : ((name == 'v') ? 15 : 13);
      if (!seen || *seen)
      {
        Error::SetStringFmt(error, "Invalid or duplicate GTE option '{}'.", option);
        return AssembleResult::Error;
      }
      *seen = true;
      bits |= static_cast<u32>(value) << shift;
    }
  }

  if (entry->mvmva && (!seen_m || !seen_v || !seen_t))
  {
    Error::SetStringView(error, "mvmva requires m=, v=, and t= options.");
    return AssembleResult::Error;
  }

  *dest = bits;
  return AssembleResult::Success;
}

bool CPU::AssembleInstruction(u32* dest, u32 pc, std::string_view text, Error* error)
{
  return AssembleInstruction(dest, pc, text, nullptr, error);
}

bool CPU::AssembleInstruction(u32* dest, u32 pc, std::string_view text, AssemblyLabelList* labels, Error* error)
{
  if (!dest)
  {
    Error::SetStringView(error, "Destination pointer is null.");
    return false;
  }

  text = StringUtil::StripWhitespace(text);
  if (text.empty())
  {
    Error::SetStringView(error, "Instruction is empty.");
    return false;
  }

  const std::string_view mnemonic = GetMnemonic(text);
  if (StringUtil::EqualNoCase(mnemonic, "nop"))
  {
    const AssembleResult result = TryTableEntry(dest, pc, text, "nop", 0, labels, error);
    return (result == AssembleResult::Success);
  }

  for (u32 op = 0; op < s_base_table.size(); op++)
  {
    if (op == static_cast<u32>(InstructionOp::funct) || op == static_cast<u32>(InstructionOp::b) ||
        (op >= static_cast<u32>(InstructionOp::cop0) && op <= static_cast<u32>(InstructionOp::cop3)))
    {
      continue;
    }

    const AssembleResult result = TryTableEntry(dest, pc, text, s_base_table[op], op << 26, labels, error);
    if (result != AssembleResult::NoMatch)
      return (result == AssembleResult::Success);
  }

  for (u32 funct = 0; funct < s_special_table.size(); funct++)
  {
    const AssembleResult result = TryTableEntry(dest, pc, text, s_special_table[funct], funct, labels, error);
    if (result != AssembleResult::NoMatch)
      return (result == AssembleResult::Success);
  }

  static constexpr std::array<std::pair<u32, const char*>, 4> branch_table = {{
    {0, "bltz $rs, $rel"},
    {1, "bgez $rs, $rel"},
    {16, "bltzal $rs, $rel"},
    {17, "bgezal $rs, $rel"},
  }};
  for (const auto& [rt, format] : branch_table)
  {
    const u32 base_bits = (static_cast<u32>(InstructionOp::b) << 26) | (rt << 16);
    const AssembleResult result = TryTableEntry(dest, pc, text, format, base_bits, labels, error);
    if (result != AssembleResult::NoMatch)
      return (result == AssembleResult::Success);
  }

  for (u32 cop_n = 0; cop_n < 4; cop_n++)
  {
    for (const auto& [common_op, format] : s_cop_common_table)
    {
      const u32 base_bits =
        ((static_cast<u32>(InstructionOp::cop0) + cop_n) << 26) | (static_cast<u32>(common_op) << 21);
      const AssembleResult result = TryTableEntry(dest, pc, text, format, base_bits, labels, error);
      if (result != AssembleResult::NoMatch)
        return (result == AssembleResult::Success);
    }
  }

  for (const auto& [cop0_op, format] : s_cop0_table)
  {
    const u32 base_bits =
      (static_cast<u32>(InstructionOp::cop0) << 26) | (UINT32_C(1) << 25) | static_cast<u32>(cop0_op);
    const AssembleResult result = TryTableEntry(dest, pc, text, format, base_bits, labels, error);
    if (result != AssembleResult::NoMatch)
      return (result == AssembleResult::Success);
  }

  const AssembleResult gte_result = AssembleGTEInstruction(dest, text, error);
  if (gte_result != AssembleResult::NoMatch)
    return (gte_result == AssembleResult::Success);

  if (mnemonic.size() == 4 && StringUtil::StartsWithNoCase(mnemonic, "cop") && mnemonic[3] >= '0' && mnemonic[3] <= '3')
  {
    u64 immediate;
    if (!ParseUnsignedValue(GetOperands(text), &immediate) || immediate > 0x1FFFFFF)
    {
      Error::SetStringView(error, "Raw coprocessor instruction requires a 25-bit immediate.");
      return false;
    }

    const u32 cop_n = static_cast<u32>(mnemonic[3] - '0');
    *dest = ((static_cast<u32>(InstructionOp::cop0) + cop_n) << 26) | (UINT32_C(1) << 25) | static_cast<u32>(immediate);
    return true;
  }

  Error::SetStringFmt(error, "Unknown instruction mnemonic '{}'.", mnemonic);
  return false;
}

bool CPU::ContainsUnresolvedLabels(const AssemblyLabelList& labels)
{
  return std::ranges::any_of(labels, [](const AssemblyLabel& label) { return !label.address.has_value(); });
}

CPU::AssemblyLabel* CPU::DefineAssemblyLabel(AssemblyLabelList* labels, const std::string_view name, const u32 address,
                                             Error* error)
{
  const auto iter = std::ranges::find(*labels, name, &AssemblyLabel::name);
  if (iter == labels->end())
  {
    labels->emplace_back(std::string(name), std::make_optional(address), std::vector<u32>());
    return &labels->back();
  }

  if (iter->address.has_value())
  {
    Error::SetStringFmt(error, "Label '{}' is already defined.", name);
    return nullptr;
  }

  iter->address = address;
  return &(*iter);
}

bool CPU::FixupAssemblyLabelBackreferences(AssemblyLabel* label, void* userdata, Error* error,
                                           u32* (*instruction_reader)(u32 pc, void* userdata))
{
  for (const u32 fixup_pc : label->backreferences)
  {
    u32* instruction = instruction_reader(fixup_pc, userdata);
    if (!instruction)
    {
      Error::SetStringFmt(error, "Failed to read instruction at 0x{:08X} for label backreference.", fixup_pc);
      return false;
    }

    const InstructionOp op = Instruction{*instruction}.op;
    if (!EncodeAssemblyTarget(instruction, fixup_pc, label->address.value(),
                              (op == InstructionOp::j || op == InstructionOp::jal), error))
    {
      return false;
    }
  }

  label->backreferences.clear();
  return true;
}
