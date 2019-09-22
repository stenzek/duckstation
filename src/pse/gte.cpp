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
      // m_regs.IR1 = static_cast<s16>(Truncate16((value & UINT32_C(0x1F)) * UINT32_C(0x80)));
      // m_regs.IR2 = static_cast<s16>(Truncate16(((value >> 5) & UINT32_C(0x1F)) * UINT32_C(0x80)));
      // m_regs.IR3 = static_cast<s16>(Truncate16(((value >> 10) & UINT32_C(0x1F)) * UINT32_C(0x80)));
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

    case 0x28:
      Execute_SQR(inst);
      break;

    case 0x30:
      Execute_RTPT(inst);
      break;

    default:
      Panic("Missing handler");
      break;
  }
}

void Core::SetMAC(u32 index, s64 value)
{
  if (value < INT64_C(-2147483648))
    m_regs.FLAG.SetMACUnderflow(index);
  else if (value > INT64_C(2147483647))
    m_regs.FLAG.SetMACOverflow(index);

  m_regs.dr32[24 + index] = Truncate32(static_cast<u64>(value));
}

void Core::SetIR(u32 index, s32 value, bool lm)
{
  if (lm && value < 0)
  {
    m_regs.FLAG.SetIRSaturated(index);
    m_regs.dr32[8 + index] = 0;
    return;
  }

  // saturate to -32768..32767
  if (!lm && value < -32768)
  {
    m_regs.FLAG.SetIRSaturated(index);
    m_regs.dr32[8 + index] = static_cast<u32>(-1);
    return;
  }

  if (value > 32767)
  {
    m_regs.FLAG.SetIRSaturated(index);
    m_regs.dr32[8 + index] = UINT32_C(0x7FFF);
    return;
  }

  // store the sign extension in the padding bits
  m_regs.dr32[8 + index] = value;
}

void Core::SetIR0(s32 value)
{
  if (value < 0)
  {
    m_regs.FLAG.SetIRSaturated(0);
    m_regs.dr32[8] = 0;
    return;
  }

  if (value > 0x1000)
  {
    m_regs.FLAG.SetIRSaturated(0);
    m_regs.dr32[8] = UINT32_C(0x1000);
    return;
  }

  // store the sign extension in the padding bits
  m_regs.dr32[8] = value;
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

s32 Core::Divide(s32 dividend, s32 divisor)
{
  DebugAssert(divisor != 0);

  const s32 res = dividend / divisor;
  if (res > 0x1FFFF)
  {
    m_regs.FLAG.divide_overflow = true;
    return 0x1FFFF;
  }

  return res;
}

s32 Core::SaturateDivide(s32 result)
{
  if (result > 0x1FFFF)
  {
    m_regs.FLAG.divide_overflow = true;
    return 0x1FFFF;
  }

  return result;
}

void Core::RTPS(const s16 V[3], bool sf)
{
  const u8 shift = sf ? 12 : 0;

  // IR1 = MAC1 = (TRX*1000h + RT11*VX0 + RT12*VY0 + RT13*VZ0) SAR (sf*12)
  // IR2 = MAC2 = (TRY*1000h + RT21*VX0 + RT22*VY0 + RT23*VZ0) SAR (sf*12)
  // IR3 = MAC3 = (TRZ*1000h + RT31*VX0 + RT32*VY0 + RT33*VZ0) SAR (sf*12)
#define T(i)                                                                                                           \
  (((s64(m_regs.TR[i]) * 0x1000) + (s64(m_regs.RT[i][0]) * V[0]) + (s64(m_regs.RT[i][1]) * V[1]) +                     \
    (s64(m_regs.RT[i][2]) * V[2])) >>                                                                                  \
   shift)

  const s64 Rx = T(0);
  const s64 Ry = T(1);
  const s64 Rz = T(2);

#undef T

  SetMAC(1, Rx);
  SetMAC(2, Ry);
  SetMAC(3, Rz);

  SetIR(1, m_regs.MAC1, false);
  SetIR(2, m_regs.MAC2, false);
  SetIR(3, m_regs.MAC3, false);

  // SZ3 = MAC3 SAR ((1-sf)*12)                           ;ScreenZ FIFO 0..+FFFFh
  const s32 SZ3 = sf ? m_regs.MAC3 : (m_regs.MAC3 >> 12);
  PushSZ(SZ3);

  // MAC0=(((H*20000h/SZ3)+1)/2)*IR1+OFX, SX2=MAC0/10000h ;ScrX FIFO -400h..+3FFh
  // MAC0=(((H*20000h/SZ3)+1)/2)*IR2+OFY, SY2=MAC0/10000h ;ScrY FIFO -400h..+3FFh
  // MAC0=(((H*20000h/SZ3)+1)/2)*DQA+DQB, IR0=MAC0/1000h  ;Depth cueing 0..+1000h
  s32 result;
  if (m_regs.SZ3 == 0)
  {
    // divide by zero
    result = 0x1FFFF;
  }
  else
  {
    result = SaturateDivide(Truncate32(((ZeroExtend64(m_regs.H) * 0x20000) / SZ3) + 1) / 2);
  }

  // MAC0=(((H*20000h/SZ3)+1)/2)*IR1+OFX, SX2=MAC0/10000h ;ScrX FIFO -400h..+3FFh
  const s32 MAC0_x = result * m_regs.IR1 + m_regs.OFX;
  const s32 MAC0_y = result * m_regs.IR2 + m_regs.OFY;
  const s32 MAC0_z = result * m_regs.DQA + m_regs.DQB;
  PushSXY(MAC0_x / 0x10000, MAC0_y / 0x10000);
  SetIR0(MAC0_z / 0x1000);
}

void Core::Execute_RTPS(Instruction inst)
{
  m_regs.FLAG.Clear();
  RTPS(m_regs.V0, inst.sf);
  m_regs.FLAG.UpdateError();
}

void Core::Execute_RTPT(Instruction inst)
{
  m_regs.FLAG.Clear();

  const bool sf = inst.sf;
  RTPS(m_regs.V0, sf);
  RTPS(m_regs.V1, sf);
  RTPS(m_regs.V2, sf);

  m_regs.FLAG.UpdateError();
}

void Core::Execute_NCLIP(Instruction inst)
{
  // MAC0 =   SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1
  m_regs.FLAG.Clear();

  const s64 MAC0x = s64(m_regs.SXY0[0]) * s64(m_regs.SXY1[1]) + s64(m_regs.SXY1[0]) * s64(m_regs.SXY2[1]) +
                    s64(m_regs.SXY2[0]) * s64(m_regs.SXY0[1]) - s64(m_regs.SXY0[0]) * s64(m_regs.SXY2[1]) -
                    s64(m_regs.SXY1[0]) * s64(m_regs.SXY0[1]) - s64(m_regs.SXY2[0]) * s64(m_regs.SXY1[1]);

  const s64 MAC0 = s64(m_regs.SXY0[0]) * m_regs.SXY1[1] + m_regs.SXY1[0] * m_regs.SXY2[1] +
                   m_regs.SXY2[0] * m_regs.SXY0[1] - m_regs.SXY0[0] * m_regs.SXY2[1] - m_regs.SXY1[0] * m_regs.SXY0[1] -
                   m_regs.SXY2[0] * m_regs.SXY1[1];

  SetMAC(0, MAC0x);

  m_regs.FLAG.UpdateError();
}

void Core::Execute_SQR(Instruction inst)
{
  m_regs.FLAG.Clear();

  const u8 shift = inst.sf ? 12 : 0;
  SetMAC(1, (s32(m_regs.IR1) * s32(m_regs.IR1)) >> shift);
  SetMAC(2, (s32(m_regs.IR2) * s32(m_regs.IR2)) >> shift);
  SetMAC(3, (s32(m_regs.IR3) * s32(m_regs.IR3)) >> shift);

  const bool lm = inst.lm;
  SetIR(1, m_regs.MAC1, lm);
  SetIR(2, m_regs.MAC2, lm);
  SetIR(3, m_regs.MAC3, lm);

  m_regs.FLAG.UpdateError();
}

} // namespace GTE