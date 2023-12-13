// SPDX-FileCopyrightText: 2016 iCatButler, 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: GPL-2.0+

#pragma once
#include "cpu_core.h"

namespace CPU::PGXP {

void Initialize();
void Reset();
void Shutdown();

// -- GTE functions
// Transforms
void GTE_PushSXYZ2f(float x, float y, float z, u32 v);
int GTE_NCLIP_valid(u32 sxy0, u32 sxy1, u32 sxy2);
float GTE_NCLIP();

// Data transfer tracking
void CPU_MFC2(u32 instr, u32 rdVal); // copy GTE data reg to GPR reg (MFC2)
void CPU_MTC2(u32 instr, u32 rtVal); // copy GPR reg to GTE data reg (MTC2)
// Memory Access
void CPU_LWC2(u32 instr, u32 addr, u32 rtVal); // copy memory to GTE reg
void CPU_SWC2(u32 instr, u32 addr, u32 rtVal); // copy GTE reg to memory

bool GetPreciseVertex(u32 addr, u32 value, int x, int y, int xOffs, int yOffs, float* out_x, float* out_y,
                      float* out_w);

// -- CPU functions
void CPU_LW(u32 instr, u32 addr, u32 rtVal);
void CPU_LH(u32 instr, u32 addr, u32 rtVal);
void CPU_LHU(u32 instr, u32 addr, u32 rtVal);
void CPU_LBx(u32 instr, u32 addr, u32 rtVal);
void CPU_SB(u32 instr, u32 addr, u32 rtVal);
void CPU_SH(u32 instr, u32 addr, u32 rtVal);
void CPU_SW(u32 instr, u32 addr, u32 rtVal);
void CPU_MOVE(u32 Rd, u32 Rs, u32 rsVal);

ALWAYS_INLINE static u32 PackMoveArgs(Reg rd, Reg rs)
{
  return (static_cast<u32>(rd) << 8) | static_cast<u32>(rs);
}
void CPU_MOVE_Packed(u32 rd_and_rs, u32 rsVal);

// Arithmetic with immediate value
void CPU_ADDI(u32 instr, u32 rsVal);
void CPU_ANDI(u32 instr, u32 rsVal);
void CPU_ORI(u32 instr, u32 rsVal);
void CPU_XORI(u32 instr, u32 rsVal);
void CPU_SLTI(u32 instr, u32 rsVal);
void CPU_SLTIU(u32 instr, u32 rsVal);

// Load Upper
void CPU_LUI(u32 instr);

// Register Arithmetic
void CPU_ADD(u32 instr, u32 rsVal, u32 rtVal);
void CPU_SUB(u32 instr, u32 rsVal, u32 rtVal);
void CPU_AND_(u32 instr, u32 rsVal, u32 rtVal);
void CPU_OR_(u32 instr, u32 rsVal, u32 rtVal);
void CPU_XOR_(u32 instr, u32 rsVal, u32 rtVal);
void CPU_NOR(u32 instr, u32 rsVal, u32 rtVal);
void CPU_SLT(u32 instr, u32 rsVal, u32 rtVal);
void CPU_SLTU(u32 instr, u32 rsVal, u32 rtVal);

// Register mult/div
void CPU_MULT(u32 instr, u32 rsVal, u32 rtVal);
void CPU_MULTU(u32 instr, u32 rsVal, u32 rtVal);
void CPU_DIV(u32 instr, u32 rsVal, u32 rtVal);
void CPU_DIVU(u32 instr, u32 rsVal, u32 rtVal);

// Shift operations (sa)
void CPU_SLL(u32 instr, u32 rtVal);
void CPU_SRL(u32 instr, u32 rtVal);
void CPU_SRA(u32 instr, u32 rtVal);

// Shift operations variable
void CPU_SLLV(u32 instr, u32 rtVal, u32 rsVal);
void CPU_SRLV(u32 instr, u32 rtVal, u32 rsVal);
void CPU_SRAV(u32 instr, u32 rtVal, u32 rsVal);

// CP0 Data transfer tracking
void CPU_MFC0(u32 instr, u32 rdVal);
void CPU_MTC0(u32 instr, u32 rdVal, u32 rtVal);

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