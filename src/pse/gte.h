#pragma once
#include "common/state_wrapper.h"
#include "gte_types.h"

namespace GTE {

class Core
{
public:
  Core();
  ~Core();

  void Initialize();
  void Reset();
  bool DoState(StateWrapper& sw);

  u32 ReadRegister(u32 index) const { return m_regs.dr32[index]; }
  void WriteRegister(u32 index, u32 value) { m_regs.dr32[index] = value; }

  u32 ReadDataRegister(u32 index) const;
  void WriteDataRegister(u32 index, u32 value);

  u32 ReadControlRegister(u32 index) const;
  void WriteControlRegister(u32 index, u32 value);

  void ExecuteInstruction(Instruction inst);

private:
  void SetMAC(u32 index, s64 value);
  void SetIR(u32 index, s32 value, bool lm);
  void SetIR0(s32 value);
  void PushSXY(s32 x, s32 y);
  void PushSZ(s32 value);
  s32 Divide(s32 dividend, s32 divisor);
  s32 SaturateDivide(s32 result);

  void RTPS(const s16 V[3], bool sf);

  void Execute_RTPS(Instruction inst);
  void Execute_RTPT(Instruction inst);
  void Execute_NCLIP(Instruction inst);
  void Execute_SQR(Instruction inst);

  Regs m_regs = {};
};

} // namespace GTE