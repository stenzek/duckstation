// SPDX-FileCopyrightText: 2016 iCatButler, 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "cpu_core.h"

namespace CPU::PGXP {

/// State management.
void Initialize();
void Reset();
void Shutdown();

/// Vertex lookup from GPU side.
bool GetPreciseVertex(u32 addr, u32 value, int x, int y, int xOffs, int yOffs, float* out_x, float* out_y,
                      float* out_w);

// GTE instruction hooks.

void GTE_RTPS(float x, float y, float z, u32 value);
bool GTE_HasPreciseVertices(u32 sxy0, u32 sxy1, u32 sxy2);
float GTE_NCLIP();

// CPU instruction implementations.

void CPU_MFC2(Instruction instr, u32 rdVal);
void CPU_MTC2(Instruction instr, u32 rtVal);
void CPU_LWC2(Instruction instr, u32 addr, u32 rtVal);
void CPU_SWC2(Instruction instr, u32 addr, u32 rtVal);
void CPU_LW(Instruction instr, u32 addr, u32 rtVal);
void CPU_LH(Instruction instr, u32 addr, u32 rtVal);
void CPU_LHU(Instruction instr, u32 addr, u32 rtVal);
void CPU_LBx(Instruction instr, u32 addr, u32 rtVal);
void CPU_SB(Instruction instr, u32 addr, u32 rtVal);
void CPU_SH(Instruction instr, u32 addr, u32 rtVal);
void CPU_SW(Instruction instr, u32 addr, u32 rtVal);
void CPU_MOVE(u32 Rd, u32 Rs, u32 rsVal);
void CPU_MOVE_Packed(u32 rd_and_rs, u32 rsVal);
void CPU_ADDI(Instruction instr, u32 rsVal);
void CPU_ANDI(Instruction instr, u32 rsVal);
void CPU_ORI(Instruction instr, u32 rsVal);
void CPU_XORI(Instruction instr, u32 rsVal);
void CPU_SLTI(Instruction instr, u32 rsVal);
void CPU_SLTIU(Instruction instr, u32 rsVal);
void CPU_LUI(Instruction instr);
void CPU_ADD(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_SUB(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_AND_(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_OR_(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_XOR_(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_NOR(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_SLT(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_SLTU(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_MULT(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_MULTU(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_DIV(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_DIVU(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_SLL(Instruction instr, u32 rtVal);
void CPU_SRL(Instruction instr, u32 rtVal);
void CPU_SRA(Instruction instr, u32 rtVal);
void CPU_SLLV(Instruction instr, u32 rtVal, u32 rsVal);
void CPU_SRLV(Instruction instr, u32 rtVal, u32 rsVal);
void CPU_SRAV(Instruction instr, u32 rtVal, u32 rsVal);
void CPU_MFC0(Instruction instr, u32 rdVal);
void CPU_MTC0(Instruction instr, u32 rdVal, u32 rtVal);

// Utility functions.

ALWAYS_INLINE static u32 PackMoveArgs(Reg rd, Reg rs)
{
  return (static_cast<u32>(rd) << 8) | static_cast<u32>(rs);
}

ALWAYS_INLINE void TryMove(Reg rd, Reg rs, Reg rt)
{
  u32 src;
  if (rs == Reg::zero)
    src = static_cast<u32>(rt);
  else if (rt == Reg::zero)
    src = static_cast<u32>(rs);
  else
    return;

  CPU_MOVE(static_cast<u32>(rd), src, g_state.regs.r[src]);
}

ALWAYS_INLINE void TryMoveImm(Reg rd, Reg rs, u32 imm)
{
  if (imm == 0)
  {
    const u32 src = static_cast<u32>(rs);
    CPU_MOVE(static_cast<u32>(rd), src, g_state.regs.r[src]);
  }
}

} // namespace CPU::PGXP