// SPDX-FileCopyrightText: 2016 iCatButler, 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "cpu_core.h"

namespace CPU::PGXP {

/// PGXPValue::flags
enum : u32
{
  VALID_X = (1u << 0),
  VALID_Y = (1u << 1),
  VALID_Z = (1u << 2),
  BITFIELD = (1u << 3),         // Significant bits were shifted out of the top, lower bits may be another field.
  VALID_LOWZ = (1u << 16),      // Valid Z from the low part of a 32-bit value.
  VALID_HIGHZ = (1u << 17),     // Valid Z from the high part of a 32-bit value.
  VALID_TAINTED_Z = (1u << 31), // X/Y has been changed, Z may not be accurate.

  VALID_XY = (VALID_X | VALID_Y),
  VALID_XYZ = (VALID_X | VALID_Y | VALID_Z),
  VALID_ALL = (VALID_X | VALID_Y | VALID_Z),
};

/// State management.
void Initialize();
void Reset();
void Shutdown();

/// Returns true if PGXP state should be saved to memory save states.
bool ShouldSavePGXPState();

/// Returns memory usage to serialize additional PGXP state.
size_t GetStateSize(CPURAMSize ram_size);

/// Save/load additional PGXP state.
void DoState(StateWrapper& sw);

/// Vertex lookup from GPU side.
bool GetPreciseVertex(u32 addr, u32 value, int x, int y, int xOffs, int yOffs, float* out_x, float* out_y,
                      float* out_w);

// GTE instruction hooks.

void GTE_RTPS(float x, float y, float z, u32 value);
bool GTE_HasPreciseVertices(u32 sxy0, u32 sxy1, u32 sxy2);
float GTE_NCLIP();
void GTE_GPF_GPL(const u32 old_ir[3], const u32* old_mac, u8 shift);

// CPU instruction implementations.

void CPU_MFC2(Instruction instr, u32 rdVal);
void CPU_MTC2(Instruction instr, u32 rtVal);
void CPU_LWC2(Instruction instr, u32 addr, u32 rtVal);
void CPU_SWC2(Instruction instr, u32 addr, u32 rtVal);
void CPU_LW(Instruction instr, u32 addr, u32 rtVal);
void CPU_LH(Instruction instr, u32 addr, u32 rtVal);
void CPU_LHU(Instruction instr, u32 addr, u32 rtVal);
void CPU_LBx(Instruction instr, u32 addr, u32 rtVal);
void CPU_LWx(Instruction instr, u32 addr, u32 rtVal);
void CPU_SB(Instruction instr, u32 addr, u32 rtVal);
void CPU_SH(Instruction instr, u32 addr, u32 rtVal);
void CPU_SW(Instruction instr, u32 addr, u32 rtVal);
void CPU_SWx(Instruction instr, u32 addr, u32 rtVal);
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
void CPU_SLLV(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_SRLV(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_SRAV(Instruction instr, u32 rsVal, u32 rtVal);
void CPU_MFC0(Instruction instr, u32 rdVal);
void CPU_MTC0(Instruction instr, u32 rtVal);

// Utility functions.

ALWAYS_INLINE u32 PackMoveArgs(Reg rd, Reg rs)
{
  return (static_cast<u32>(rd) << 8) | static_cast<u32>(rs);
}

ALWAYS_INLINE void TryMove(Reg rd, Reg rs, Reg rt)
{
  u32 src;
  if (rd == Reg::zero)
    return;
  else if (rs == Reg::zero)
    src = static_cast<u32>(rt);
  else if (rt == Reg::zero)
    src = static_cast<u32>(rs);
  else
    return;

  CPU_MOVE(static_cast<u32>(rd), src, g_state.regs.r[src]);
}

ALWAYS_INLINE void TryMoveImm(Reg rd, Reg rs, u32 imm)
{
  if (imm == 0 && rd != Reg::zero)
  {
    const u32 src = static_cast<u32>(rs);
    CPU_MOVE(static_cast<u32>(rd), src, g_state.regs.r[src]);
  }
}

} // namespace CPU::PGXP