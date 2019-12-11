#include "gte.h"
#include "YBaseLib/Log.h"
#include <algorithm>
#include <array>
Log_SetChannel(GTE);

// TODO: Optimize, intrinsics?
static inline constexpr u32 CountLeadingZeros(u16 value)
{
  u32 count = 0;
  for (u32 i = 0; i < 16 && (value & UINT16_C(0x8000)) == 0; i++)
  {
    count++;
    value <<= 1;
  }

  return count;
}

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
  std::memset(&m_regs, 0, sizeof(m_regs));
}

bool Core::DoState(StateWrapper& sw)
{
  sw.DoArray(m_regs.r32, NUM_DATA_REGS + NUM_CONTROL_REGS);
  return !sw.HasError();
}

u32 Core::ReadRegister(u32 index) const
{
  DebugAssert(index < countof(m_regs.r32));

  switch (index)
  {
    case 15: // SXY3
    {
      // mirror of SXY2
      return m_regs.r32[14];
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

    default:
      return m_regs.r32[index];
  }
}

void Core::WriteRegister(u32 index, u32 value)
{
#if 0
  if (index < 32)
  {
    Log_DebugPrintf("DataReg(%u) <- 0x%08X", index, value);
  }
  else
  {
    Log_DebugPrintf("ControlReg(%u) <- 0x%08X", index, value);
  }
#endif

  switch (index)
  {
    case 1:  // V0[z]
    case 3:  // V1[z]
    case 5:  // V2[z]
    case 8:  // IR0
    case 9:  // IR1
    case 10: // IR2
    case 11: // IR3
    case 36: // RT33
    case 44: // L33
    case 52: // LR33
    case 58: // H       - sign-extended on read but zext on use
    case 59: // DQA
    case 61: // ZSF3
    case 62: // ZSF4
    {
      // sign-extend z component of vector registers
      m_regs.r32[index] = SignExtend32(Truncate16(value));
    }
    break;

    case 7:  // OTZ
    case 16: // SZ0
    case 17: // SZ1
    case 18: // SZ2
    case 19: // SZ3
    {
      // zero-extend unsigned values
      m_regs.r32[index] = ZeroExtend32(Truncate16(value));
    }
    break;

    case 15: // SXY3
    {
      // writing to SXYP pushes to the FIFO
      m_regs.r32[12] = m_regs.r32[13]; // SXY0 <- SXY1
      m_regs.r32[13] = m_regs.r32[14]; // SXY1 <- SXY2
      m_regs.r32[14] = value;          // SXY2 <- SXYP
    }
    break;

    case 28: // IRGB
    {
      // IRGB register, convert 555 to 16-bit
      m_regs.IRGB = value & UINT32_C(0x7FFF);
      m_regs.r32[9] = SignExtend32(static_cast<u16>(Truncate16((value & UINT32_C(0x1F)) * UINT32_C(0x80))));
      m_regs.r32[10] = SignExtend32(static_cast<u16>(Truncate16(((value >> 5) & UINT32_C(0x1F)) * UINT32_C(0x80))));
      m_regs.r32[11] = SignExtend32(static_cast<u16>(Truncate16(((value >> 10) & UINT32_C(0x1F)) * UINT32_C(0x80))));
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

    case 63: // FLAG
    {
      m_regs.FLAG.bits = value & UINT32_C(0x7FFFF000);
      m_regs.FLAG.UpdateError();
    }
    break;

    default:
    {
      // written as-is, 2x16 or 1x32 bits
      m_regs.r32[index] = value;
    }
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

    case 0x0C:
      Execute_OP(inst);
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
      Execute_NCDT(inst);
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
  else if (x > 1023)
  {
    m_regs.FLAG.sx2_saturated = true;
    x = 1023;
  }

  if (y < -1024)
  {
    m_regs.FLAG.sy2_saturated = true;
    y = -1024;
  }
  else if (y > 1023)
  {
    m_regs.FLAG.sy2_saturated = true;
    y = 1023;
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

void Core::PushRGBFromMAC()
{
  // Note: SHR 4 used instead of /16 as the results are different.
  const u32 r = TruncateRGB<0>(static_cast<u32>(m_regs.MAC1 >> 4));
  const u32 g = TruncateRGB<1>(static_cast<u32>(m_regs.MAC2 >> 4));
  const u32 b = TruncateRGB<2>(static_cast<u32>(m_regs.MAC3 >> 4));
  const u32 c = ZeroExtend32(m_regs.RGBC[3]);

  m_regs.dr32[20] = m_regs.dr32[21];                      // RGB0 <- RGB1
  m_regs.dr32[21] = m_regs.dr32[22];                      // RGB1 <- RGB2
  m_regs.dr32[22] = r | (g << 8) | (b << 16) | (c << 24); // RGB2 <- Value
}

u32 Core::UNRDivide(u32 lhs, u32 rhs)
{
  if (rhs * 2 <= lhs)
  {
    m_regs.FLAG.divide_overflow = true;
    return 0x1FFFF;
  }

  const u32 shift = CountLeadingZeros(static_cast<u16>(rhs));
  lhs <<= shift;
  rhs <<= shift;

  static constexpr std::array<u8, 257> unr_table = {{
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3, //
    0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5, 0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8, //  00h..3Fh
    0xC6, 0xC5, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0, //
    0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A, //
    0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86, //
    0x85, 0x84, 0x83, 0x82, 0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74, //  40h..7Fh
    0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64, //
    0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55, //
    0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48, //
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3F, 0x3F, 0x3E, 0x3D, 0x3C, 0x3C, 0x3B, //  80h..BFh
    0x3A, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F, //
    0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24, //
    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C, 0x1B, 0x1B, 0x1A, //
    0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11, //  C0h..FFh
    0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x08, 0x08, //
    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, //
    0x00 // <-- one extra table entry (for "(d-7FC0h)/80h"=100h)
  }};

  const u32 divisor = rhs | 0x8000;
  const s32 x = static_cast<s32>(0x101 + ZeroExtend32(unr_table[((divisor & 0x7FFF) + 0x40) >> 7]));
  const s32 d = ((static_cast<s32>(ZeroExtend32(divisor)) * -x) + 0x80) >> 8;
  const u32 recip = static_cast<u32>(((x * (0x20000 + d)) + 0x80) >> 8);

  const u32 result = Truncate32((ZeroExtend64(lhs) * ZeroExtend64(recip) + u64(0x8000)) >> 16);

  // The min(1FFFFh) limit is needed for cases like FE3Fh/7F20h, F015h/780Bh, etc. (these do produce UNR result 20000h,
  // and are saturated to 1FFFFh, but without setting overflow FLAG bits).
  return std::min<u32>(0x1FFFF, result);
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
    {
      // buggy
      M[0][0] = -static_cast<s16>(ZeroExtend16(m_regs.RGBC[0]) << 4);
      M[0][1] = static_cast<s16>(ZeroExtend16(m_regs.RGBC[0]) << 4);
      M[0][2] = m_regs.IR0;
      M[1][0] = m_regs.RT[0][2];
      M[1][1] = m_regs.RT[0][2];
      M[1][2] = m_regs.RT[0][2];
      M[2][0] = m_regs.RT[1][1];
      M[2][1] = m_regs.RT[1][1];
      M[2][2] = m_regs.RT[1][1];
    }
    break;
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
      std::memcpy(T, m_regs.FC, sizeof(T));
      break;
    default:
      std::fill_n(T, countof(T), s32(0));
      break;
  }

  MulMatVec(M, T, Vx, Vy, Vz, inst.GetShift(), inst.lm);

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

void Core::Execute_OP(Instruction inst)
{
  m_regs.FLAG.Clear();

  // Take copies since we overwrite them in each step.
  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;
  const s32 D1 = s32(m_regs.RT[0][0]);
  const s32 D2 = s32(m_regs.RT[1][1]);
  const s32 D3 = s32(m_regs.RT[2][2]);
  const s32 IR1 = s32(m_regs.IR1);
  const s32 IR2 = s32(m_regs.IR2);
  const s32 IR3 = s32(m_regs.IR3);

  // [MAC1,MAC2,MAC3] = [IR3*D2-IR2*D3, IR1*D3-IR3*D1, IR2*D1-IR1*D2] SAR (sf*12)
  // [IR1, IR2, IR3] = [MAC1, MAC2, MAC3]; copy result
  TruncateAndSetMACAndIR<1>(s64(IR3 * D2) - s64(IR2 * D3), shift, lm);
  TruncateAndSetMACAndIR<2>(s64(IR1 * D3) - s64(IR3 * D1), shift, lm);
  TruncateAndSetMACAndIR<3>(s64(IR2 * D1) - s64(IR1 * D2), shift, lm);

  m_regs.FLAG.UpdateError();
}

void Core::RTPS(const s16 V[3], u8 shift, bool lm, bool last)
{
#define dot3(i)                                                                                                        \
  SignExtendMACResult<i + 1>(                                                                                          \
    SignExtendMACResult<i + 1>((s64(m_regs.TR[i]) << 12) + (s64(m_regs.RT[i][0]) * s64(V[0]))) +                       \
    (s64(m_regs.RT[i][1]) * s64(V[1]))) +                                                                              \
    (s64(m_regs.RT[i][2]) * s64(V[2]))

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
  TruncateAndSetIR<3>(s32(z >> 12), false);
  m_regs.dr32[11] = std::clamp(m_regs.MAC3, lm ? 0 : IR123_MIN_VALUE, IR123_MAX_VALUE);
#undef dot3

  // SZ3 = MAC3 SAR ((1-sf)*12)                           ;ScreenZ FIFO 0..+FFFFh
  PushSZ(s32(z >> 12));

  // MAC0=(((H*20000h/SZ3)+1)/2)*IR1+OFX, SX2=MAC0/10000h ;ScrX FIFO -400h..+3FFh
  // MAC0=(((H*20000h/SZ3)+1)/2)*IR2+OFY, SY2=MAC0/10000h ;ScrY FIFO -400h..+3FFh
  const s64 result = static_cast<s64>(ZeroExtend64(UNRDivide(m_regs.H, m_regs.SZ3)));
  const s64 Sx = s64(result) * s64(m_regs.IR1) + s64(m_regs.OFX);
  const s64 Sy = s64(result) * s64(m_regs.IR2) + s64(m_regs.OFY);
  CheckMACOverflow<0>(Sx);
  CheckMACOverflow<0>(Sy);
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
  RTPS(m_regs.V0, inst.GetShift(), inst.lm, true);
  m_regs.FLAG.UpdateError();
}

void Core::Execute_RTPT(Instruction inst)
{
  m_regs.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  RTPS(m_regs.V0, shift, lm, false);
  RTPS(m_regs.V1, shift, lm, false);
  RTPS(m_regs.V2, shift, lm, true);

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
