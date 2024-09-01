// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_types.h"

#include "common/assert.h"

#include <array>

static const std::array<const char*, 36> s_reg_names = {
  {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "s0", "s1",
   "s2",   "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra", "hi", "lo", "pc", "npc"}};

const char* CPU::GetRegName(Reg reg)
{
  DebugAssert(reg < Reg::count);
  return s_reg_names[static_cast<u8>(reg)];
}

bool CPU::IsNopInstruction(const Instruction instruction)
{
  // TODO: Handle other types of nop.
  return (instruction.bits == 0);
}

bool CPU::IsBranchInstruction(const Instruction instruction)
{
  switch (instruction.op)
  {
    case InstructionOp::j:
    case InstructionOp::jal:
    case InstructionOp::b:
    case InstructionOp::beq:
    case InstructionOp::bgtz:
    case InstructionOp::blez:
    case InstructionOp::bne:
      return true;

    case InstructionOp::funct:
    {
      switch (instruction.r.funct)
      {
        case InstructionFunct::jr:
        case InstructionFunct::jalr:
          return true;

        default:
          return false;
      }
    }

    default:
      return false;
  }
}

bool CPU::IsUnconditionalBranchInstruction(const Instruction instruction)
{
  switch (instruction.op)
  {
    case InstructionOp::j:
    case InstructionOp::jal:
    case InstructionOp::b:
      return true;

    case InstructionOp::beq:
    {
      if (instruction.i.rs == Reg::zero && instruction.i.rt == Reg::zero)
        return true;
      else
        return false;
    }
    break;

    case InstructionOp::funct:
    {
      switch (instruction.r.funct)
      {
        case InstructionFunct::jr:
        case InstructionFunct::jalr:
          return true;

        default:
          return false;
      }
    }

    default:
      return false;
  }
}

bool CPU::IsDirectBranchInstruction(const Instruction instruction)
{
  switch (instruction.op)
  {
    case InstructionOp::j:
    case InstructionOp::jal:
    case InstructionOp::b:
    case InstructionOp::beq:
    case InstructionOp::bgtz:
    case InstructionOp::blez:
    case InstructionOp::bne:
      return true;

    default:
      return false;
  }
}

VirtualMemoryAddress CPU::GetDirectBranchTarget(const Instruction instruction, VirtualMemoryAddress instruction_pc)
{
  const VirtualMemoryAddress pc = instruction_pc + 4;

  switch (instruction.op)
  {
    case InstructionOp::j:
    case InstructionOp::jal:
      return (pc & UINT32_C(0xF0000000)) | (instruction.j.target << 2);

    case InstructionOp::b:
    case InstructionOp::beq:
    case InstructionOp::bgtz:
    case InstructionOp::blez:
    case InstructionOp::bne:
      return (pc + (instruction.i.imm_sext32() << 2));

    default:
      return pc;
  }
}

bool CPU::IsCallInstruction(const Instruction instruction)
{
  return (instruction.op == InstructionOp::funct && instruction.r.funct == InstructionFunct::jalr) ||
         (instruction.op == InstructionOp::jal);
}

bool CPU::IsReturnInstruction(const Instruction instruction)
{
  if (instruction.op != InstructionOp::funct)
    return false;

  // j(al)r ra
  if (instruction.r.funct == InstructionFunct::jr || instruction.r.funct == InstructionFunct::jalr)
    return (instruction.r.rs == Reg::ra);

  return false;
}

bool CPU::IsMemoryLoadInstruction(const Instruction instruction)
{
  switch (instruction.op)
  {
    case InstructionOp::lb:
    case InstructionOp::lh:
    case InstructionOp::lw:
    case InstructionOp::lbu:
    case InstructionOp::lhu:
    case InstructionOp::lwl:
    case InstructionOp::lwr:
      return true;

    case InstructionOp::lwc2:
      return true;

    default:
      return false;
  }
}

bool CPU::IsMemoryStoreInstruction(const Instruction instruction)
{
  switch (instruction.op)
  {
    case InstructionOp::sb:
    case InstructionOp::sh:
    case InstructionOp::sw:
    case InstructionOp::swl:
    case InstructionOp::swr:
      return true;

    case InstructionOp::swc2:
      return true;

    default:
      return false;
  }
}

std::optional<VirtualMemoryAddress> CPU::GetLoadStoreEffectiveAddress(const Instruction instruction,
                                                                      const Registers* regs)
{
  switch (instruction.op)
  {
    case InstructionOp::lb:
    case InstructionOp::lh:
    case InstructionOp::lw:
    case InstructionOp::lbu:
    case InstructionOp::lhu:
    case InstructionOp::lwc2:
    case InstructionOp::sb:
    case InstructionOp::sh:
    case InstructionOp::sw:
    case InstructionOp::swc2:
      return (regs->r[static_cast<u32>(instruction.i.rs.GetValue())] + instruction.i.imm_sext32());

    case InstructionOp::lwl:
    case InstructionOp::lwr:
    case InstructionOp::swl:
    case InstructionOp::swr:
      return (regs->r[static_cast<u32>(instruction.i.rs.GetValue())] + instruction.i.imm_sext32()) & ~UINT32_C(3);

    default:
      return std::nullopt;
  }
}

bool CPU::InstructionHasLoadDelay(const Instruction instruction)
{
  switch (instruction.op)
  {
    case InstructionOp::lb:
    case InstructionOp::lh:
    case InstructionOp::lw:
    case InstructionOp::lbu:
    case InstructionOp::lhu:
    case InstructionOp::lwl:
    case InstructionOp::lwr:
      return true;

    case InstructionOp::cop0:
    case InstructionOp::cop2:
    {
      if (instruction.cop.IsCommonInstruction())
      {
        const CopCommonInstruction common_op = instruction.cop.CommonOp();
        return (common_op == CopCommonInstruction::cfcn || common_op == CopCommonInstruction::mfcn);
      }

      return false;
    }

    default:
      return false;
  }
}

bool CPU::IsExitBlockInstruction(const Instruction instruction)
{
  switch (instruction.op)
  {
    case InstructionOp::funct:
    {
      switch (instruction.r.funct)
      {
        case InstructionFunct::syscall:
        case InstructionFunct::break_:
          return true;

        default:
          return false;
      }
    }

    default:
      return false;
  }
}

bool CPU::IsValidInstruction(const Instruction instruction)
{
  // No constexpr std::bitset until C++23 :(
  static constexpr const std::array<u32, 64 / 32> valid_op_map = []() constexpr {
    std::array<u32, 64 / 32> ret = {};

#define SET(op) ret[static_cast<size_t>(op) / 32] |= (1u << (static_cast<size_t>(op) % 32));

    SET(InstructionOp::b);
    SET(InstructionOp::j);
    SET(InstructionOp::jal);
    SET(InstructionOp::beq);
    SET(InstructionOp::bne);
    SET(InstructionOp::blez);
    SET(InstructionOp::bgtz);
    SET(InstructionOp::addi);
    SET(InstructionOp::addiu);
    SET(InstructionOp::slti);
    SET(InstructionOp::sltiu);
    SET(InstructionOp::andi);
    SET(InstructionOp::ori);
    SET(InstructionOp::xori);
    SET(InstructionOp::lui);

    // Invalid COP0-3 ops don't raise #RI?
    SET(InstructionOp::cop0);
    SET(InstructionOp::cop1);
    SET(InstructionOp::cop2);
    SET(InstructionOp::cop3);

    SET(InstructionOp::lb);
    SET(InstructionOp::lh);
    SET(InstructionOp::lwl);
    SET(InstructionOp::lw);
    SET(InstructionOp::lbu);
    SET(InstructionOp::lhu);
    SET(InstructionOp::lwr);
    SET(InstructionOp::sb);
    SET(InstructionOp::sh);
    SET(InstructionOp::swl);
    SET(InstructionOp::sw);
    SET(InstructionOp::swr);
    SET(InstructionOp::lwc0);
    SET(InstructionOp::lwc1);
    SET(InstructionOp::lwc2);
    SET(InstructionOp::lwc3);
    SET(InstructionOp::swc0);
    SET(InstructionOp::swc1);
    SET(InstructionOp::swc2);
    SET(InstructionOp::swc3);

#undef SET

    return ret;
  }();

  static constexpr const std::array<u32, 64 / 32> valid_func_map = []() constexpr {
    std::array<u32, 64 / 32> ret = {};

#define SET(op) ret[static_cast<size_t>(op) / 32] |= (1u << (static_cast<size_t>(op) % 32));

    SET(InstructionFunct::sll);
    SET(InstructionFunct::srl);
    SET(InstructionFunct::sra);
    SET(InstructionFunct::sllv);
    SET(InstructionFunct::srlv);
    SET(InstructionFunct::srav);
    SET(InstructionFunct::jr);
    SET(InstructionFunct::jalr);
    SET(InstructionFunct::syscall);
    SET(InstructionFunct::break_);
    SET(InstructionFunct::mfhi);
    SET(InstructionFunct::mthi);
    SET(InstructionFunct::mflo);
    SET(InstructionFunct::mtlo);
    SET(InstructionFunct::mult);
    SET(InstructionFunct::multu);
    SET(InstructionFunct::div);
    SET(InstructionFunct::divu);
    SET(InstructionFunct::add);
    SET(InstructionFunct::addu);
    SET(InstructionFunct::sub);
    SET(InstructionFunct::subu);
    SET(InstructionFunct::and_);
    SET(InstructionFunct::or_);
    SET(InstructionFunct::xor_);
    SET(InstructionFunct::nor);
    SET(InstructionFunct::slt);
    SET(InstructionFunct::sltu);

#undef SET

    return ret;
  }();

#define CHECK(arr, val) ((arr[static_cast<size_t>(val) / 32] & (1u << (static_cast<size_t>(val) % 32))) != 0u)

  if (instruction.op == InstructionOp::funct)
    return CHECK(valid_func_map, instruction.r.funct.GetValue());
  else
    return CHECK(valid_op_map, instruction.op.GetValue());

#undef CHECK
}
