#include "gte.h"
#include "YBaseLib/Log.h"
#include <algorithm>
Log_SetChannel(GTE);

static inline constexpr u32 CountLeadingBits(u32 value)
{
  u32 count = 0;
  if ((value & UINT32_C(0x80000000)) != 0)
  {
    for (u32 i = 0; i < 32 && ((value & UINT32_C(0x80000000)) != 0); i++)
    {
      count++;
      value <<= 1;
    }
  }
  else
  {
    for (u32 i = 0; i < 32 && (value & UINT32_C(0x80000000)) == 0; i++)
    {
      count++;
      value <<= 1;
    }
  }

  return count;
}

namespace GTE {

Core::Core() = default;

Core::~Core() = default;

void Core::Initialize() {}

void Core::Reset()
{
  m_regs = {};
}

bool Core::DoState(StateWrapper& sw)
{
  sw.DoPOD(&m_regs);
  return !sw.HasError();
}

u32 Core::ReadDataRegister(u32 index) const
{
  switch (index)
  {
    case 15: // SXY3
    {
      // mirror of SXY2
      return m_regs.dr32[14];
    }

    case 28: // IRGB
    case 29: // ORGB
    {
      // ORGB register, convert 16-bit to 555
      const u8 r = static_cast<u8>(std::clamp(m_regs.IR1 / 0x80, 0x00, 0x1F));
      const u8 g = static_cast<u8>(std::clamp(m_regs.IR2 / 0x80, 0x00, 0x1F));
      const u8 b = static_cast<u8>(std::clamp(m_regs.IR3 / 0x80, 0x00, 0x1F));
      return ZeroExtend32(r) | (ZeroExtend32(g) << 5) | (ZeroExtend32(b) << 10);
    }

    case 0:  // V0-1 [x,y]
    case 1:  // V0[z]
    case 2:  // V1-2 [x,y]
    case 3:  // V1[z]
    case 4:  // V2-3 [x,y]
    case 5:  // V2[z]
    case 6:  // RGBC
    case 7:  // OTZ
    case 8:  // IR0
    case 9:  // IR1
    case 10: // IR2
    case 11: // IR3
    case 12: // SXY0
    case 13: // SXY1
    case 14: // SXY2
    case 16: // SZ0
    case 17: // SZ1
    case 18: // SZ2
    case 19: // SZ3
    case 20: // RGB0
    case 21: // RGB1
    case 22: // RGB2
    case 23: // RES1
    case 24: // MAC0
    case 25: // MAC1
    case 26: // MAC2
    case 27: // MAC3
    case 30: // LZCS
    case 31: // LZCR
      return m_regs.dr32[index];

    default:
      Panic("Unknown register");
      return 0;
  }
}

void Core::WriteDataRegister(u32 index, u32 value)
{
  // Log_DebugPrintf("DataReg(%u) <- 0x%08X", index, value);

  switch (index)
  {
    case 1:  // V0[z]
    case 3:  // V1[z]
    case 5:  // V2[z]
    case 8:  // IR0
    case 9:  // IR1
    case 10: // IR2
    case 11: // IR3
    {
      // sign-extend z component of vector registers
      m_regs.dr32[index] = SignExtend32(Truncate16(value));
    }
    break;

    case 7:  // OTZ
    case 16: // SZ0
    case 17: // SZ1
    case 18: // SZ2
    case 19: // SZ3
    {
      // zero-extend unsigned values
      m_regs.dr32[index] = ZeroExtend32(Truncate16(value));
    }
    break;

    case 15: // SXY3
    {
      // writing to SXYP pushes to the FIFO
      m_regs.dr32[12] = m_regs.dr32[13]; // SXY0 <- SXY1
      m_regs.dr32[13] = m_regs.dr32[14]; // SXY1 <- SXY2
      m_regs.dr32[14] = value;           // SXY2 <- SXYP
    }
    break;

    case 28: // IRGB
    {
      // IRGB register, convert 555 to 16-bit
      m_regs.IRGB = value & UINT32_C(0x7FFF);
      m_regs.dr32[9] = SignExtend32(static_cast<u16>(Truncate16((value & UINT32_C(0x1F)) * UINT32_C(0x80))));
      m_regs.dr32[10] = SignExtend32(static_cast<u16>(Truncate16(((value >> 5) & UINT32_C(0x1F)) * UINT32_C(0x80))));
      m_regs.dr32[11] = SignExtend32(static_cast<u16>(Truncate16(((value >> 10) & UINT32_C(0x1F)) * UINT32_C(0x80))));
    }
    break;

    case 30: // LZCS
    {
      m_regs.LZCS = static_cast<s32>(value);
      m_regs.LZCR = CountLeadingBits(value);
    }
    break;

    case 29: // ORGB
    case 31: // LZCR
    {
      // read-only registers
    }
    break;

    case 0:  // V0-1 [x,y]
    case 2:  // V1-2 [x,y]
    case 4:  // V2-3 [x,y]
    case 6:  // RGBC
    case 12: // SXY0
    case 13: // SXY1
    case 14: // SXY2
    case 20: // RGB0
    case 21: // RGB1
    case 22: // RGB2
    case 23: // RES1
    case 24: // MAC0
    case 25: // MAC1
    case 26: // MAC2
    case 27: // MAC3
      m_regs.dr32[index] = value;
      break;

    default:
      Panic("Unknown register");
      break;
  }
}

u32 Core::ReadControlRegister(u32 index) const
{
  return m_regs.cr32[index];
}

void Core::WriteControlRegister(u32 index, u32 value)
{
  // Log_DebugPrintf("ControlReg(%u,%u) <- 0x%08X", index, index + 32, value);

  switch (index)
  {
    case 36 - 32: // RT33
    case 44 - 32: // L33
    case 52 - 32: // LR33
    case 58 - 32: // H       - sign-extended on read but zext on use
    case 59 - 32: // DQA
    case 61 - 32: // ZSF3
    case 62 - 32: // ZSF4
    {
      // MSB of the last matrix element is the last element sign-extended
      m_regs.cr32[index] = SignExtend32(Truncate16(value));
    }
    break;

    case 63 - 32: // FLAG
    {
      m_regs.FLAG.bits = value & UINT32_C(0x7FFFF000);
      m_regs.FLAG.UpdateError();
    }
    break;

    case 32 - 32: // RT11,RT12
    case 33 - 32: // RT13,RT21
    case 34 - 32: // RT22,RT23
    case 35 - 32: // RT31,RT32
    case 37 - 32: // TRX
    case 38 - 32: // TRY
    case 39 - 32: // TRZ
    case 40 - 32: // L11,L12
    case 41 - 32: // L13,L21
    case 42 - 32: // L22,L23
    case 43 - 32: // L31,L32
    case 45 - 32: // RBK
    case 46 - 32: // GBK
    case 47 - 32: // BBK
    case 48 - 32: // LR11,LR12
    case 49 - 32: // LR13,LR21
    case 50 - 32: // LR22,LR23
    case 51 - 32: // LR31,LR32
    case 53 - 32: // RFC
    case 54 - 32: // GFC
    case 55 - 32: // BFC
    case 56 - 32: // OFX
    case 57 - 32: // OFY
    case 60 - 32: // DQB
    {
      // written as-is, 2x16 or 1x32 bits
      m_regs.cr32[index] = value;
    }
    break;

    default:
      Panic("Unknown register");
      break;
  }
}

void Core::ExecuteInstruction(Instruction inst)
{
  // Panic("GTE instruction");

  switch (inst.command)
  {
    case 0x01:
      Execute_RTPS(inst);
      break;

    case 0x06:
      Execute_NCLIP(inst);
      break;

    case 0x10:
      Execute_DPCS(inst);
      break;

    case 0x11:
      Execute_INTPL(inst);
      break;

    case 0x12:
      Execute_MVMVA(inst);
      break;

    case 0x13:
      Execute_NCDS(inst);
      break;

    case 0x14:
      Execute_CDP(inst);
      break;

    case 0x16:
      Execute_NCCT(inst);
      break;

    case 0x1B:
      Execute_NCCS(inst);
      break;

    case 0x1C:
      Execute_CC(inst);
      break;

    case 0x1E:
      Execute_NCS(inst);
      break;

    case 0x20:
      Execute_NCT(inst);
      break;

    case 0x28:
      Execute_SQR(inst);
      break;

    case 0x29:
      Execute_DCPL(inst);
      break;

    case 0x2A:
      Execute_DPCT(inst);
      break;

    case 0x2D:
      Execute_AVSZ3(inst);
      break;

    case 0x2E:
      Execute_AVSZ4(inst);
      break;

    case 0x30:
      Execute_RTPT(inst);
      break;

    case 0x3D:
      Execute_GPF(inst);
      break;

    case 0x3E:
      Execute_GPL(inst);
      break;

    case 0x3F:
      Execute_NCCT(inst);
      break;

    default:
      Panic("Missing handler");
      break;
  }
}

void Core::SetOTZ(s32 value)
{
  if (value < 0)
  {
    m_regs.FLAG.sz1_otz_saturated = true;
    value = 0;
  }
  else if (value > 0xFFFF)
  {
    m_regs.FLAG.sz1_otz_saturated = true;
    value = 0xFFFF;
  }

  m_regs.dr32[7] = static_cast<u32>(value);
}

void Core::PushSXY(s32 x, s32 y)
{
  if (x < -1024)
  {
    m_regs.FLAG.sx2_saturated = true;
    x = -1024;
  }
  else if (x > 32767)
  {
    m_regs.FLAG.sx2_saturated = true;
    x = 32767;
  }

  if (y < -1024)
  {
    m_regs.FLAG.sy2_saturated = true;
    y = -1024;
  }
  else if (x > 32767)
  {
    m_regs.FLAG.sy2_saturated = true;
    y = 32767;
  }

  m_regs.dr32[12] = m_regs.dr32[13]; // SXY0 <- SXY1
  m_regs.dr32[13] = m_regs.dr32[14]; // SXY1 <- SXY2
  m_regs.SXY2[0] = static_cast<s16>(x);
  m_regs.SXY2[1] = static_cast<s16>(y);
}

void Core::PushSZ(s32 value)
{
  if (value < 0)
  {
    m_regs.FLAG.sz1_otz_saturated = true;
    value = 0;
  }
  else if (value > 0xFFFF)
  {
    m_regs.FLAG.sz1_otz_saturated = true;
    value = 0xFFFF;
  }

  m_regs.dr32[16] = m_regs.dr32[17];         // SZ0 <- SZ1
  m_regs.dr32[17] = m_regs.dr32[18];         // SZ1 <- SZ2
  m_regs.dr32[18] = m_regs.dr32[19];         // SZ2 <- SZ3
  m_regs.dr32[19] = static_cast<u32>(value); // SZ3 <- value
}

void Core::PushRGB(u8 r, u8 g, u8 b, u8 c)
{
  m_regs.dr32[20] = m_regs.dr32[21]; // RGB0 <- RGB1
  m_regs.dr32[21] = m_regs.dr32[22]; // RGB1 <- RGB2
  m_regs.dr32[22] =
    ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16) | (ZeroExtend32(c) << 24); // RGB2 <- Value
}

void Core::PushRGBFromMAC()
{
  // Note: SHR 4 used instead of /16 as the results are different.
  PushRGB(TruncateRGB<0>(m_regs.MAC1 >> 4), TruncateRGB<1>(m_regs.MAC2 >> 4), TruncateRGB<2>(m_regs.MAC3 >> 4),
          m_regs.RGBC[3]);
}

void Core::MulMatVec(const s16 M[3][3], const s16 Vx, const s16 Vy, const s16 Vz, u8 shift, bool lm)
{
#define dot3(i)                                                                                                        \
  TruncateAndSetMACAndIR<i + 1>(SignExtendMACResult<i + 1>((s64(M[i][0]) * s64(Vx)) + (s64(M[i][1]) * s64(Vy))) +      \
                                  (s64(M[i][2]) * s64(Vz)),                                                            \
                                shift, lm)

  dot3(0);
  dot3(1);
  dot3(2);

#undef dot3
}

void Core::MulMatVec(const s16 M[3][3], const s32 T[3], const s16 Vx, const s16 Vy, const s16 Vz, u8 shift, bool lm)
{
#define dot3(i)                                                                                                        \
  TruncateAndSetMACAndIR<i + 1>(                                                                                       \
    SignExtendMACResult<i + 1>(SignExtendMACResult<i + 1>((s64(T[i]) << 12) + (s64(M[i][0]) * s64(Vx))) +              \
                               (s64(M[i][1]) * s64(Vy))) +                                                             \
      (s64(M[i][2]) * s64(Vz)),                                                                                        \
    shift, lm)

  dot3(0);
  dot3(1);
  dot3(2);

#undef dot3
}

void Core::Execute_MVMVA(Instruction inst)
{
  m_regs.FLAG.Clear();

  // TODO: Remove memcpy..
  s16 M[3][3];
  switch (inst.mvmva_multiply_matrix)
  {
    case 0:
      std::memcpy(M, m_regs.RT, sizeof(s16) * 3 * 3);
      break;
    case 1:
      std::memcpy(M, m_regs.LLM, sizeof(s16) * 3 * 3);
      break;
    case 2:
      std::memcpy(M, m_regs.LCM, sizeof(s16) * 3 * 3);
      break;
    default:
      // buggy
      Panic("Missing implementation");
      return;
  }

  s16 Vx, Vy, Vz;
  switch (inst.mvmva_multiply_vector)
  {
    case 0:
      Vx = m_regs.V0[0];
      Vy = m_regs.V0[1];
      Vz = m_regs.V0[2];
      break;
    case 1:
      Vx = m_regs.V1[0];
      Vy = m_regs.V1[1];
      Vz = m_regs.V1[2];
      break;
    case 2:
      Vx = m_regs.V2[0];
      Vy = m_regs.V2[1];
      Vz = m_regs.V2[2];
      break;
    default:
      Vx = m_regs.IR1;
      Vy = m_regs.IR2;
      Vz = m_regs.IR3;
      break;
  }

  s32 T[3];
  switch (inst.mvmva_translation_vector)
  {
    case 0:
      std::memcpy(T, m_regs.TR, sizeof(T));
      break;
    case 1:
      std::memcpy(T, m_regs.BK, sizeof(T));
      break;
    case 2:
      // buggy
      std::memcpy(T, m_regs.FC, sizeof(T));
      break;
    case 3:
      std::fill_n(T, countof(T), s32(0));
      break;
    default:
      Panic("Missing implementation");
      return;
  }

  MulMatVec(M, T, Vx, Vy, Vz, inst.GetShift(), inst.lm);

  m_regs.FLAG.UpdateError();
}

void Core::RTPS(const s16 V[3], bool sf, bool lm, bool last)
{
  const u8 shift = sf ? 12 : 0;
#define dot3(i)                                                                                                        \
  SignExtendMACResult<i + 1>(                                                                                          \
    (s64(m_regs.TR[i]) << 12) +                                                                                        \
    SignExtendMACResult<i + 1>(                                                                                        \
      SignExtendMACResult<i + 1>(SignExtendMACResult<i + 1>(s64(s32(m_regs.RT[i][0]) * s32(V[0]))) +                   \
                                 s64(s32(m_regs.RT[i][1]) * s32(V[1]))) +                                              \
      s64(s32(m_regs.RT[i][2]) * s32(V[2]))))

  // IR1 = MAC1 = (TRX*1000h + RT11*VX0 + RT12*VY0 + RT13*VZ0) SAR (sf*12)
  // IR2 = MAC2 = (TRY*1000h + RT21*VX0 + RT22*VY0 + RT23*VZ0) SAR (sf*12)
  // IR3 = MAC3 = (TRZ*1000h + RT31*VX0 + RT32*VY0 + RT33*VZ0) SAR (sf*12)
  const s64 x = dot3(0);
  const s64 y = dot3(1);
  const s64 z = dot3(2);
  TruncateAndSetMAC<1>(x, shift);
  TruncateAndSetMAC<2>(y, shift);
  TruncateAndSetMAC<3>(z, shift);
  TruncateAndSetIR<1>(m_regs.MAC1, lm);
  TruncateAndSetIR<2>(m_regs.MAC2, lm);

  // The command does saturate IR1,IR2,IR3 to -8000h..+7FFFh (regardless of lm bit). When using RTP with sf=0, then the
  // IR3 saturation flag (FLAG.22) gets set <only> if "MAC3 SAR 12" exceeds -8000h..+7FFFh (although IR3 is saturated
  // when "MAC3" exceeds -8000h..+7FFFh).
  TruncateAndSetIR<3>(m_regs.MAC3, false);
  m_regs.dr32[11] = std::clamp(m_regs.MAC3, lm ? 0 : IR123_MIN_VALUE, IR123_MAX_VALUE);
#undef dot3

  // SZ3 = MAC3 SAR ((1-sf)*12)                           ;ScreenZ FIFO 0..+FFFFh
  PushSZ(s32(z >> 12));

  s32 result;
  if (m_regs.SZ3 == 0)
  {
    // divide by zero
    result = 0x1FFFF;
  }
  else
  {
    result = s32(((s64(ZeroExtend64(m_regs.H) * 0x20000) / s64(ZeroExtend64(m_regs.SZ3))) + 1) / 2);
    if (result > 0x1FFFF)
    {
      m_regs.FLAG.divide_overflow = true;
      result = 0x1FFFF;
    }
  }

  // MAC0=(((H*20000h/SZ3)+1)/2)*IR1+OFX, SX2=MAC0/10000h ;ScrX FIFO -400h..+3FFh
  // MAC0=(((H*20000h/SZ3)+1)/2)*IR2+OFY, SY2=MAC0/10000h ;ScrY FIFO -400h..+3FFh
  const s64 Sx = s64(result) * s64(m_regs.IR1) + s64(m_regs.OFX);
  const s64 Sy = s64(result) * s64(m_regs.IR2) + s64(m_regs.OFY);
  TruncateAndSetMAC<0>(Sx, 0);
  TruncateAndSetMAC<1>(Sy, 0);
  PushSXY(s32(Sx >> 16), s32(Sy >> 16));

  if (last)
  {
    // MAC0=(((H*20000h/SZ3)+1)/2)*DQA+DQB, IR0=MAC0/1000h  ;Depth cueing 0..+1000h
    const s64 Sz = s64(result) * s64(m_regs.DQA) + s64(m_regs.DQB);
    TruncateAndSetMAC<0>(Sz, 0);
    TruncateAndSetIR<0>(s32(Sz >> 12), true);
  }
}

void Core::Execute_RTPS(Instruction inst)
{
  m_regs.FLAG.Clear();
  RTPS(m_regs.V0, inst.sf, inst.lm, true);
  m_regs.FLAG.UpdateError();
}

void Core::Execute_RTPT(Instruction inst)
{
  m_regs.FLAG.Clear();

  const bool sf = inst.sf;
  RTPS(m_regs.V0, sf, inst.lm, false);
  RTPS(m_regs.V1, sf, inst.lm, false);
  RTPS(m_regs.V2, sf, inst.lm, true);

  m_regs.FLAG.UpdateError();
}

void Core::Execute_NCLIP(Instruction inst)
{
  // MAC0 =   SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1
  m_regs.FLAG.Clear();

  TruncateAndSetMAC<0>(s64(m_regs.SXY0[0]) * s64(m_regs.SXY1[1]) + s64(m_regs.SXY1[0]) * s64(m_regs.SXY2[1]) +
                         s64(m_regs.SXY2[0]) * s64(m_regs.SXY0[1]) - s64(m_regs.SXY0[0]) * s64(m_regs.SXY2[1]) -
                         s64(m_regs.SXY1[0]) * s64(m_regs.SXY0[1]) - s64(m_regs.SXY2[0]) * s64(m_regs.SXY1[1]),
                       0);

  m_regs.FLAG.UpdateError();
}

void Core::Execute_SQR(Instruction inst)
{
  m_regs.FLAG.Clear();

  // 32-bit multiply for speed - 16x16 isn't >32bit, and we know it won't overflow/underflow.
  const u8 shift = inst.GetShift();
  m_regs.MAC1 = (s32(m_regs.IR1) * s32(m_regs.IR1)) >> shift;
  m_regs.MAC2 = (s32(m_regs.IR2) * s32(m_regs.IR2)) >> shift;
  m_regs.MAC3 = (s32(m_regs.IR3) * s32(m_regs.IR3)) >> shift;

  const bool lm = inst.lm;
  TruncateAndSetIR<1>(m_regs.MAC1, lm);
  TruncateAndSetIR<2>(m_regs.MAC2, lm);
  TruncateAndSetIR<3>(m_regs.MAC3, lm);

  m_regs.FLAG.UpdateError();
}

void Core::Execute_AVSZ3(Instruction inst)
{
  m_regs.FLAG.Clear();

  const s64 result = s64(m_regs.ZSF3) * s32(u32(m_regs.SZ1) + u32(m_regs.SZ2) + u32(m_regs.SZ3));
  TruncateAndSetMAC<0>(result, 0);
  SetOTZ(s32(result >> 12));

  m_regs.FLAG.UpdateError();
}

void Core::Execute_AVSZ4(Instruction inst)
{
  m_regs.FLAG.Clear();

  const s64 result = s64(m_regs.ZSF4) * s32(u32(m_regs.SZ0) + u32(m_regs.SZ1) + u32(m_regs.SZ2) + u32(m_regs.SZ3));
  TruncateAndSetMAC<0>(result, 0);
  SetOTZ(s32(result >> 12));

  m_regs.FLAG.UpdateError();
}

void Core::InterpolateColor(s64 in_MAC1, s64 in_MAC2, s64 in_MAC3, u8 shift, bool lm)
{
  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
  //   [IR1,IR2,IR3] = (([RFC,GFC,BFC] SHL 12) - [MAC1,MAC2,MAC3]) SAR (sf*12)
  TruncateAndSetMACAndIR<1>((s64(m_regs.FC[0]) << 12) - in_MAC1, shift, false);
  TruncateAndSetMACAndIR<2>((s64(m_regs.FC[1]) << 12) - in_MAC2, shift, false);
  TruncateAndSetMACAndIR<3>((s64(m_regs.FC[2]) << 12) - in_MAC3, shift, false);

  //   [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3])
  // [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)
  TruncateAndSetMACAndIR<1>(s64(s32(m_regs.IR1) * s32(m_regs.IR0)) + in_MAC1, shift, lm);
  TruncateAndSetMACAndIR<2>(s64(s32(m_regs.IR2) * s32(m_regs.IR0)) + in_MAC2, shift, lm);
  TruncateAndSetMACAndIR<3>(s64(s32(m_regs.IR3) * s32(m_regs.IR0)) + in_MAC3, shift, lm);
}

void Core::NCS(const s16 V[3], u8 shift, bool lm)
{
  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
  MulMatVec(m_regs.LLM, V[0], V[1], V[2], shift, lm);

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(m_regs.LCM, m_regs.BK, m_regs.IR1, m_regs.IR2, m_regs.IR3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();
}

void Core::Execute_NCS(Instruction inst)
{
  m_regs.FLAG.Clear();

  NCS(m_regs.V0, inst.GetShift(), inst.lm);

  m_regs.FLAG.UpdateError();
}

void Core::Execute_NCT(Instruction inst)
{
  m_regs.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  NCS(m_regs.V0, shift, lm);
  NCS(m_regs.V1, shift, lm);
  NCS(m_regs.V2, shift, lm);

  m_regs.FLAG.UpdateError();
}

void Core::NCCS(const s16 V[3], u8 shift, bool lm)
{
  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
  MulMatVec(m_regs.LLM, V[0], V[1], V[2], shift, lm);

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(m_regs.LCM, m_regs.BK, m_regs.IR1, m_regs.IR2, m_regs.IR3, shift, lm);

  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4          ;<--- for NCDx/NCCx
  // [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)       ;<--- for NCDx/NCCx
  TruncateAndSetMACAndIR<1>(s64(s32(ZeroExtend32(m_regs.RGBC[0])) * s32(m_regs.IR1)) << 4, shift, lm);
  TruncateAndSetMACAndIR<2>(s64(s32(ZeroExtend32(m_regs.RGBC[1])) * s32(m_regs.IR2)) << 4, shift, lm);
  TruncateAndSetMACAndIR<3>(s64(s32(ZeroExtend32(m_regs.RGBC[2])) * s32(m_regs.IR3)) << 4, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();
}

void Core::Execute_NCCS(Instruction inst)
{
  m_regs.FLAG.Clear();

  NCCS(m_regs.V0, inst.GetShift(), inst.lm);

  m_regs.FLAG.UpdateError();
}

void Core::Execute_NCCT(Instruction inst)
{
  m_regs.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  NCCS(m_regs.V0, shift, lm);
  NCCS(m_regs.V1, shift, lm);
  NCCS(m_regs.V2, shift, lm);

  m_regs.FLAG.UpdateError();
}

void Core::NCDS(const s16 V[3], u8 shift, bool lm)
{
  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
  MulMatVec(m_regs.LLM, V[0], V[1], V[2], shift, lm);

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(m_regs.LCM, m_regs.BK, m_regs.IR1, m_regs.IR2, m_regs.IR3, shift, lm);

  // No need to assign these to MAC[1-3], as it'll never overflow.
  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4          ;<--- for NCDx/NCCx
  const s32 in_MAC1 = (s32(ZeroExtend32(m_regs.RGBC[0])) * s32(m_regs.IR1)) << 4;
  const s32 in_MAC2 = (s32(ZeroExtend32(m_regs.RGBC[1])) * s32(m_regs.IR2)) << 4;
  const s32 in_MAC3 = (s32(ZeroExtend32(m_regs.RGBC[2])) * s32(m_regs.IR3)) << 4;

  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0                   ;<--- for NCDx only
  InterpolateColor(in_MAC1, in_MAC2, in_MAC3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();
}

void Core::Execute_NCDS(Instruction inst)
{
  m_regs.FLAG.Clear();

  NCDS(m_regs.V0, inst.GetShift(), inst.lm);

  m_regs.FLAG.UpdateError();
}

void Core::Execute_NCDT(Instruction inst)
{
  m_regs.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  NCDS(m_regs.V0, shift, lm);
  NCDS(m_regs.V1, shift, lm);
  NCDS(m_regs.V2, shift, lm);

  m_regs.FLAG.UpdateError();
}

void Core::Execute_CC(Instruction inst)
{
  m_regs.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(m_regs.LCM, m_regs.BK, m_regs.IR1, m_regs.IR2, m_regs.IR3, shift, lm);

  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4
  // [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)
  TruncateAndSetMACAndIR<1>(s64(s32(ZeroExtend32(m_regs.RGBC[0])) * s32(m_regs.IR1)) << 4, shift, lm);
  TruncateAndSetMACAndIR<2>(s64(s32(ZeroExtend32(m_regs.RGBC[1])) * s32(m_regs.IR2)) << 4, shift, lm);
  TruncateAndSetMACAndIR<3>(s64(s32(ZeroExtend32(m_regs.RGBC[2])) * s32(m_regs.IR3)) << 4, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  m_regs.FLAG.UpdateError();
}

void Core::Execute_CDP(Instruction inst)
{
  m_regs.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(m_regs.LCM, m_regs.BK, m_regs.IR1, m_regs.IR2, m_regs.IR3, shift, lm);

  // No need to assign these to MAC[1-3], as it'll never overflow.
  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4
  const s32 in_MAC1 = (s32(ZeroExtend32(m_regs.RGBC[0])) * s32(m_regs.IR1)) << 4;
  const s32 in_MAC2 = (s32(ZeroExtend32(m_regs.RGBC[1])) * s32(m_regs.IR2)) << 4;
  const s32 in_MAC3 = (s32(ZeroExtend32(m_regs.RGBC[2])) * s32(m_regs.IR3)) << 4;

  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0                   ;<--- for CDP only
  // [MAC1, MAC2, MAC3] = [MAC1, MAC2, MAC3] SAR(sf * 12)
  InterpolateColor(in_MAC1, in_MAC2, in_MAC3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  m_regs.FLAG.UpdateError();
}

void Core::DPCS(const u8 color[3], u8 shift, bool lm)
{
  // In: [IR1,IR2,IR3]=Vector, FC=Far Color, IR0=Interpolation value, CODE=MSB of RGBC
  // [MAC1,MAC2,MAC3] = [R,G,B] SHL 16                     ;<--- for DPCS/DPCT
  TruncateAndSetMAC<1>((s64(ZeroExtend64(color[0])) << 16), 0);
  TruncateAndSetMAC<2>((s64(ZeroExtend64(color[1])) << 16), 0);
  TruncateAndSetMAC<3>((s64(ZeroExtend64(color[2])) << 16), 0);

  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
  InterpolateColor(m_regs.MAC1, m_regs.MAC2, m_regs.MAC3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();
}

void Core::Execute_DPCS(Instruction inst)
{
  m_regs.FLAG.Clear();

  DPCS(m_regs.RGBC, inst.GetShift(), inst.lm);

  m_regs.FLAG.UpdateError();
}

void Core::Execute_DPCT(Instruction inst)
{
  m_regs.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  for (u32 i = 0; i < 3; i++)
    DPCS(m_regs.RGB0, shift, lm);

  m_regs.FLAG.UpdateError();
}

void Core::Execute_DCPL(Instruction inst)
{
  m_regs.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  // No need to assign these to MAC[1-3], as it'll never overflow.
  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4          ;<--- for DCPL only
  const s32 in_MAC1 = (s32(ZeroExtend32(m_regs.RGBC[0])) * s32(m_regs.IR1)) << 4;
  const s32 in_MAC2 = (s32(ZeroExtend32(m_regs.RGBC[1])) * s32(m_regs.IR2)) << 4;
  const s32 in_MAC3 = (s32(ZeroExtend32(m_regs.RGBC[2])) * s32(m_regs.IR3)) << 4;

  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
  InterpolateColor(in_MAC1, in_MAC2, in_MAC3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  m_regs.FLAG.UpdateError();
}

void Core::Execute_INTPL(Instruction inst)
{
  m_regs.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  // No need to assign these to MAC[1-3], as it'll never overflow.
  // [MAC1,MAC2,MAC3] = [IR1,IR2,IR3] SHL 12               ;<--- for INTPL only
  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
  InterpolateColor(s32(m_regs.IR1) << 12, s32(m_regs.IR2) << 12, s32(m_regs.IR3) << 12, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  m_regs.FLAG.UpdateError();
}

void Core::Execute_GPL(Instruction inst)
{
  m_regs.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  // [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SHL (sf*12)       ;<--- for GPL only
  // [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3]) SAR (sf*12)
  TruncateAndSetMACAndIR<1>((s64(s32(m_regs.IR1) * s32(m_regs.IR0)) + (s64(m_regs.MAC1) << shift)), shift, lm);
  TruncateAndSetMACAndIR<2>((s64(s32(m_regs.IR2) * s32(m_regs.IR0)) + (s64(m_regs.MAC2) << shift)), shift, lm);
  TruncateAndSetMACAndIR<3>((s64(s32(m_regs.IR3) * s32(m_regs.IR0)) + (s64(m_regs.MAC3) << shift)), shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  m_regs.FLAG.UpdateError();
}

void Core::Execute_GPF(Instruction inst)
{
  m_regs.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  // [MAC1,MAC2,MAC3] = [0,0,0]                            ;<--- for GPF only
  // [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3]) SAR (sf*12)
  TruncateAndSetMACAndIR<1>(s64(s32(m_regs.IR1) * s32(m_regs.IR0)), shift, lm);
  TruncateAndSetMACAndIR<2>(s64(s32(m_regs.IR2) * s32(m_regs.IR0)), shift, lm);
  TruncateAndSetMACAndIR<3>(s64(s32(m_regs.IR3) * s32(m_regs.IR0)), shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  m_regs.FLAG.UpdateError();
}

} // namespace GTE