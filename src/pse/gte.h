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
  template<u32 index>
  s32 TruncateMAC(s64 value);

  template<u32 index>
  u8 TruncateRGB(s32 value);

  template<u32 index>
  void SetIR(s32 value, bool lm);

  void SetMAC(u32 index, s64 value);
  void SetIR(u32 index, s32 value, bool lm);
  void SetIR0(s32 value);
  void SetOTZ(s32 value);
  void PushSXY(s32 x, s32 y);
  void PushSZ(s32 value);
  void PushRGB(u8 r, u8 g, u8 b, u8 c);
  s32 Divide(s32 dividend, s32 divisor);
  s32 SaturateDivide(s32 result);

  static s64 VecDot(const s16 A[3], const s16 B[3]);
  static s64 VecDot(const s16 A[3], s16 B_x, s16 B_y, s16 B_z);

  void RTPS(const s16 V[3], bool sf);
  void NCDS(const s16 V[3], bool sf, bool lm);

  void Execute_RTPS(Instruction inst);
  void Execute_RTPT(Instruction inst);
  void Execute_NCLIP(Instruction inst);
  void Execute_SQR(Instruction inst);
  void Execute_AVSZ3(Instruction inst);
  void Execute_AVSZ4(Instruction inst);
  void Execute_NCDS(Instruction inst);

  Regs m_regs = {};
};

#include "gte.inl"

} // namespace GTE