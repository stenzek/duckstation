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
  static constexpr s64 MAC0_MIN_VALUE = -(INT64_C(1) << 43);
  static constexpr s64 MAC0_MAX_VALUE = (INT64_C(1) << 43) - 1;
  static constexpr s64 MAC123_MIN_VALUE = -(INT64_C(1) << 43);
  static constexpr s64 MAC123_MAX_VALUE = (INT64_C(1) << 43) - 1;
  static constexpr s32 IR0_MIN_VALUE = 0x0000;
  static constexpr s32 IR0_MAX_VALUE = 0x1000;
  static constexpr s32 IR123_MIN_VALUE = -(INT64_C(1) << 15);
  static constexpr s32 IR123_MAX_VALUE = (INT64_C(1) << 15) - 1;

  template<u32 index>
  s64 TruncateMAC(s64 value);

  template<u32 index>
  s32 TruncateAndSetMAC(s64 value, u8 shift);

  template<u32 index>
  u8 TruncateRGB(s32 value);

  template<u32 index>
  s16 TruncateAndSetIR(s32 value, bool lm);

  void SetMAC(u32 index, s64 value);
  void SetIR(u32 index, s32 value, bool lm);
  void SetOTZ(s32 value);
  void PushSXY(s32 x, s32 y);
  void PushSZ(s32 value);
  void PushRGB(u8 r, u8 g, u8 b, u8 c);

  s64 VecDot(const s16 A[3], const s16 B[3]);
  s64 VecDot(const s16 A[3], s16 B_x, s16 B_y, s16 B_z);

  // 3x3 matrix * 3x1 vector, updates MAC[1-3] and IR[1-3]
  void MulMatVec(const s16 M[3][3], const s16 Vx, const s16 Vy, const s16 Vz, u8 shift, bool lm);
  
  // 3x3 matrix * 3x1 vector with translation, updates MAC[1-3] and IR[1-3]
  void MulMatVec(const s16 M[3][3], const s32 T[3], const s16 Vx, const s16 Vy, const s16 Vz, u8 shift, bool lm);

  void RTPS(const s16 V[3], bool sf, bool lm);
  void NCCS(const s16 V[3], bool sf, bool lm);
  void NCDS(const s16 V[3], bool sf, bool lm);

  void Execute_RTPS(Instruction inst);
  void Execute_RTPT(Instruction inst);
  void Execute_NCLIP(Instruction inst);
  void Execute_SQR(Instruction inst);
  void Execute_AVSZ3(Instruction inst);
  void Execute_AVSZ4(Instruction inst);
  void Execute_NCCS(Instruction inst);
  void Execute_NCCT(Instruction inst);
  void Execute_NCDS(Instruction inst);
  void Execute_NCDT(Instruction inst);
  void Execute_MVMVA(Instruction inst);
  void Execute_DPCS(Instruction inst);

  Regs m_regs = {};
};

#include "gte.inl"

} // namespace GTE