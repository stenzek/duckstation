// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gte.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_pgxp.h"
#include "host.h"
#include "settings.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/gsvector.h"
#include "common/timer.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <numbers>
#include <numeric>

LOG_CHANNEL(Host);

// Freecam is disabled on Android because there's no windowed UI for it.
// And because users can't be trusted to not crash games and complain.
#ifndef __ANDROID__
#define ENABLE_FREECAM 1
#endif

namespace GTE {

static constexpr s64 MAC0_MIN_VALUE = -(INT64_C(1) << 31);
static constexpr s64 MAC0_MAX_VALUE = (INT64_C(1) << 31) - 1;
static constexpr s64 MAC123_MIN_VALUE = -(INT64_C(1) << 43);
static constexpr s64 MAC123_MAX_VALUE = (INT64_C(1) << 43) - 1;
static constexpr s32 IR0_MIN_VALUE = 0x0000;
static constexpr s32 IR0_MAX_VALUE = 0x1000;
static constexpr s32 IR123_MIN_VALUE = -(INT64_C(1) << 15);
static constexpr s32 IR123_MAX_VALUE = (INT64_C(1) << 15) - 1;

static constexpr float FREECAM_MIN_TRANSLATION = -40000.0f;
static constexpr float FREECAM_MAX_TRANSLATION = 40000.0f;
static constexpr float FREECAM_MIN_ROTATION = -360.0f;
static constexpr float FREECAM_MAX_ROTATION = 360.0f;
static constexpr float FREECAM_DEFAULT_MOVE_SPEED = 4096.0f;
static constexpr float FREECAM_MAX_MOVE_SPEED = 65536.0f;
static constexpr float FREECAM_DEFAULT_TURN_SPEED = 30.0f;
static constexpr float FREECAM_MAX_TURN_SPEED = 360.0f;

namespace {

struct ALIGN_TO_CACHE_LINE Config
{
  DisplayAspectRatio aspect_ratio = DisplayAspectRatio::R4_3;
  u32 custom_aspect_ratio_numerator;
  u32 custom_aspect_ratio_denominator;
  float custom_aspect_ratio_f;

#ifdef ENABLE_FREECAM

  Timer::Value freecam_update_time = 0;
  std::atomic_bool freecam_transform_changed{false};
  bool freecam_enabled = false;
  bool freecam_active = false;
  bool freecam_reverse_transform_order = false;

  float freecam_move_speed = FREECAM_DEFAULT_MOVE_SPEED;
  float freecam_turn_speed = FREECAM_DEFAULT_TURN_SPEED;
  GSVector4 freecam_move = GSVector4::cxpr(0.0f);
  GSVector4 freecam_turn = GSVector4::cxpr(0.0f);

  GSVector4 freecam_rotation = GSVector4::cxpr(0.0f);
  GSVector4 freecam_translation = GSVector4::cxpr(0.0f);

  ALIGN_TO_CACHE_LINE GSMatrix4x4 freecam_matrix = GSMatrix4x4::Identity();

#endif
};

} // namespace

static Config s_config;

#define REGS CPU::g_state.gte_regs

ALWAYS_INLINE static u32 CountLeadingBits(u32 value)
{
  // if top-most bit is set, we want to count ones not zeros
  if (value & UINT32_C(0x80000000))
    value ^= UINT32_C(0xFFFFFFFF);

  return (value == 0u) ? 32 : CountLeadingZeros(value);
}

template<u32 index>
ALWAYS_INLINE static void CheckMACOverflow(s64 value)
{
  constexpr s64 MIN_VALUE = (index == 0) ? MAC0_MIN_VALUE : MAC123_MIN_VALUE;
  constexpr s64 MAX_VALUE = (index == 0) ? MAC0_MAX_VALUE : MAC123_MAX_VALUE;
  if (value < MIN_VALUE)
  {
    if constexpr (index == 0)
      REGS.FLAG.mac0_underflow = true;
    else if constexpr (index == 1)
      REGS.FLAG.mac1_underflow = true;
    else if constexpr (index == 2)
      REGS.FLAG.mac2_underflow = true;
    else if constexpr (index == 3)
      REGS.FLAG.mac3_underflow = true;
  }
  else if (value > MAX_VALUE)
  {
    if constexpr (index == 0)
      REGS.FLAG.mac0_overflow = true;
    else if constexpr (index == 1)
      REGS.FLAG.mac1_overflow = true;
    else if constexpr (index == 2)
      REGS.FLAG.mac2_overflow = true;
    else if constexpr (index == 3)
      REGS.FLAG.mac3_overflow = true;
  }
}

template<u32 index>
ALWAYS_INLINE static s64 SignExtendMACResult(s64 value)
{
  CheckMACOverflow<index>(value);
  return SignExtendN < index == 0 ? 31 : 44 > (value);
}

template<u32 index>
ALWAYS_INLINE static void TruncateAndSetMAC(s64 value, u8 shift)
{
  CheckMACOverflow<index>(value);

  // shift should be done before storing to avoid losing precision
  value >>= shift;

  REGS.dr32[24 + index] = Truncate32(static_cast<u64>(value));
}

template<u32 index>
ALWAYS_INLINE static void TruncateAndSetIR(s32 value, bool lm)
{
  constexpr s32 MIN_VALUE = (index == 0) ? IR0_MIN_VALUE : IR123_MIN_VALUE;
  constexpr s32 MAX_VALUE = (index == 0) ? IR0_MAX_VALUE : IR123_MAX_VALUE;
  const s32 actual_min_value = lm ? 0 : MIN_VALUE;
  if (value < actual_min_value)
  {
    value = actual_min_value;
    if constexpr (index == 0)
      REGS.FLAG.ir0_saturated = true;
    else if constexpr (index == 1)
      REGS.FLAG.ir1_saturated = true;
    else if constexpr (index == 2)
      REGS.FLAG.ir2_saturated = true;
    else if constexpr (index == 3)
      REGS.FLAG.ir3_saturated = true;
  }
  else if (value > MAX_VALUE)
  {
    value = MAX_VALUE;
    if constexpr (index == 0)
      REGS.FLAG.ir0_saturated = true;
    else if constexpr (index == 1)
      REGS.FLAG.ir1_saturated = true;
    else if constexpr (index == 2)
      REGS.FLAG.ir2_saturated = true;
    else if constexpr (index == 3)
      REGS.FLAG.ir3_saturated = true;
  }

  // store sign-extended 16-bit value as 32-bit
  REGS.dr32[8 + index] = value;
}

template<u32 index>
ALWAYS_INLINE static void TruncateAndSetMACAndIR(s64 value, u8 shift, bool lm)
{
  CheckMACOverflow<index>(value);

  // shift should be done before storing to avoid losing precision
  value >>= shift;

  // set MAC
  const s32 value32 = static_cast<s32>(value);
  REGS.dr32[24 + index] = value32;

  // set IR
  TruncateAndSetIR<index>(value32, lm);
}

template<u32 index>
ALWAYS_INLINE static u32 TruncateRGB(s32 value)
{
  if (value < 0 || value > 0xFF)
  {
    if constexpr (index == 0)
      REGS.FLAG.color_r_saturated = true;
    else if constexpr (index == 1)
      REGS.FLAG.color_g_saturated = true;
    else
      REGS.FLAG.color_b_saturated = true;

    return (value < 0) ? 0 : 0xFF;
  }

  return static_cast<u32>(value);
}

static void SetOTZ(s32 value);
static void PushSXY(s32 x, s32 y);
static void PushSZ(s32 value);
static void PushRGBFromMAC();
static u32 UNRDivide(u32 lhs, u32 rhs);

static void MulMatVec(const s16* M_, const s16 Vx, const s16 Vy, const s16 Vz, u8 shift, bool lm);
static void MulMatVec(const s16* M_, const s32 T[3], const s16 Vx, const s16 Vy, const s16 Vz, u8 shift, bool lm);
static void MulMatVecBuggy(const s16* M_, const s32 T[3], const s16 Vx, const s16 Vy, const s16 Vz, u8 shift, bool lm);

static void InterpolateColor(s64 in_MAC1, s64 in_MAC2, s64 in_MAC3, u8 shift, bool lm);
static void RTPS(const s16 V[3], u8 shift, bool lm, bool last);
static void NCS(const s16 V[3], u8 shift, bool lm);
static void NCCS(const s16 V[3], u8 shift, bool lm);
static void NCDS(const s16 V[3], u8 shift, bool lm);
static void DPCS(const u8 color[3], u8 shift, bool lm);

#ifdef ENABLE_FREECAM
static void ApplyFreecam(s64& x, s64& y, s64& z);
#endif

static void Execute_MVMVA(Instruction inst);
static void Execute_SQR(Instruction inst);
static void Execute_OP(Instruction inst);
static void Execute_RTPS(Instruction inst);
static void Execute_RTPT(Instruction inst);
static void Execute_NCLIP(Instruction inst);
static void Execute_NCLIP_PGXP(Instruction inst);
static void Execute_AVSZ3(Instruction inst);
static void Execute_AVSZ4(Instruction inst);
static void Execute_NCS(Instruction inst);
static void Execute_NCT(Instruction inst);
static void Execute_NCCS(Instruction inst);
static void Execute_NCCT(Instruction inst);
static void Execute_NCDS(Instruction inst);
static void Execute_NCDT(Instruction inst);
static void Execute_CC(Instruction inst);
static void Execute_CDP(Instruction inst);
static void Execute_DPCS(Instruction inst);
static void Execute_DPCT(Instruction inst);
static void Execute_DCPL(Instruction inst);
static void Execute_INTPL(Instruction inst);
static void Execute_GPL(Instruction inst);
static void Execute_GPF(Instruction inst);

} // namespace GTE

void GTE::Initialize()
{
  s_config.aspect_ratio = DisplayAspectRatio::R4_3;
  Reset();
}

void GTE::Reset()
{
  std::memset(&REGS, 0, sizeof(REGS));
  SetFreecamEnabled(false);
  ResetFreecam();
}

bool GTE::DoState(StateWrapper& sw)
{
  sw.DoArray(REGS.r32, NUM_DATA_REGS + NUM_CONTROL_REGS);
  return !sw.HasError();
}

void GTE::SetAspectRatio(DisplayAspectRatio aspect, u32 custom_num, u32 custom_denom)
{
  s_config.aspect_ratio = aspect;
  if (aspect != DisplayAspectRatio::Custom)
    return;

  // (4 / 3) / (num / denom) => gcd((4 * denom) / (3 * num))
  const u32 x = 4u * custom_denom;
  const u32 y = 3u * custom_num;
  const u32 gcd = std::gcd(x, y);

  s_config.custom_aspect_ratio_numerator = x / gcd;
  s_config.custom_aspect_ratio_denominator = y / gcd;

  s_config.custom_aspect_ratio_f =
    static_cast<float>((4.0 / 3.0) / (static_cast<double>(custom_num) / static_cast<double>(custom_denom)));
}

u32 GTE::ReadRegister(u32 index)
{
  DebugAssert(index < countof(REGS.r32));

  switch (index)
  {
    case 15: // SXY3
    {
      // mirror of SXY2
      return REGS.r32[14];
    }

    case 28: // IRGB
    case 29: // ORGB
    {
      // ORGB register, convert 16-bit to 555
      const u8 r = static_cast<u8>(std::clamp(REGS.IR1 / 0x80, 0x00, 0x1F));
      const u8 g = static_cast<u8>(std::clamp(REGS.IR2 / 0x80, 0x00, 0x1F));
      const u8 b = static_cast<u8>(std::clamp(REGS.IR3 / 0x80, 0x00, 0x1F));
      return ZeroExtend32(r) | (ZeroExtend32(g) << 5) | (ZeroExtend32(b) << 10);
    }

    default:
      return REGS.r32[index];
  }
}

void GTE::WriteRegister(u32 index, u32 value)
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
      REGS.r32[index] = SignExtend32(Truncate16(value));
    }
    break;

    case 7:  // OTZ
    case 16: // SZ0
    case 17: // SZ1
    case 18: // SZ2
    case 19: // SZ3
    {
      // zero-extend unsigned values
      REGS.r32[index] = ZeroExtend32(Truncate16(value));
    }
    break;

    case 15: // SXY3
    {
      // writing to SXYP pushes to the FIFO
      REGS.r32[12] = REGS.r32[13]; // SXY0 <- SXY1
      REGS.r32[13] = REGS.r32[14]; // SXY1 <- SXY2
      REGS.r32[14] = value;        // SXY2 <- SXYP
    }
    break;

    case 28: // IRGB
    {
      // IRGB register, convert 555 to 16-bit
      REGS.IRGB = value & UINT32_C(0x7FFF);
      REGS.r32[9] = SignExtend32(static_cast<u16>(Truncate16((value & UINT32_C(0x1F)) * UINT32_C(0x80))));
      REGS.r32[10] = SignExtend32(static_cast<u16>(Truncate16(((value >> 5) & UINT32_C(0x1F)) * UINT32_C(0x80))));
      REGS.r32[11] = SignExtend32(static_cast<u16>(Truncate16(((value >> 10) & UINT32_C(0x1F)) * UINT32_C(0x80))));
    }
    break;

    case 30: // LZCS
    {
      REGS.LZCS = static_cast<s32>(value);
      REGS.LZCR = CountLeadingBits(value);
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
      REGS.FLAG.bits = value & UINT32_C(0x7FFFF000);
      REGS.FLAG.UpdateError();
    }
    break;

    default:
    {
      // written as-is, 2x16 or 1x32 bits
      REGS.r32[index] = value;
    }
    break;
  }
}

u32* GTE::GetRegisterPtr(u32 index)
{
  return &REGS.r32[index];
}

ALWAYS_INLINE void GTE::SetOTZ(s32 value)
{
  if (value < 0)
  {
    REGS.FLAG.sz1_otz_saturated = true;
    value = 0;
  }
  else if (value > 0xFFFF)
  {
    REGS.FLAG.sz1_otz_saturated = true;
    value = 0xFFFF;
  }

  REGS.dr32[7] = static_cast<u32>(value);
}

ALWAYS_INLINE void GTE::PushSXY(s32 x, s32 y)
{
  if (x < -1024)
  {
    REGS.FLAG.sx2_saturated = true;
    x = -1024;
  }
  else if (x > 1023)
  {
    REGS.FLAG.sx2_saturated = true;
    x = 1023;
  }

  if (y < -1024)
  {
    REGS.FLAG.sy2_saturated = true;
    y = -1024;
  }
  else if (y > 1023)
  {
    REGS.FLAG.sy2_saturated = true;
    y = 1023;
  }

  REGS.dr32[12] = REGS.dr32[13]; // SXY0 <- SXY1
  REGS.dr32[13] = REGS.dr32[14]; // SXY1 <- SXY2
  REGS.dr32[14] = (static_cast<u32>(x) & 0xFFFFu) | (static_cast<u32>(y) << 16);
}

ALWAYS_INLINE void GTE::PushSZ(s32 value)
{
  if (value < 0)
  {
    REGS.FLAG.sz1_otz_saturated = true;
    value = 0;
  }
  else if (value > 0xFFFF)
  {
    REGS.FLAG.sz1_otz_saturated = true;
    value = 0xFFFF;
  }

  REGS.dr32[16] = REGS.dr32[17];           // SZ0 <- SZ1
  REGS.dr32[17] = REGS.dr32[18];           // SZ1 <- SZ2
  REGS.dr32[18] = REGS.dr32[19];           // SZ2 <- SZ3
  REGS.dr32[19] = static_cast<u32>(value); // SZ3 <- value
}

ALWAYS_INLINE void GTE::PushRGBFromMAC()
{
  // Note: SHR 4 used instead of /16 as the results are different.
  const u32 r = TruncateRGB<0>(static_cast<u32>(REGS.MAC1 >> 4));
  const u32 g = TruncateRGB<1>(static_cast<u32>(REGS.MAC2 >> 4));
  const u32 b = TruncateRGB<2>(static_cast<u32>(REGS.MAC3 >> 4));
  const u32 c = ZeroExtend32(REGS.RGBC[3]);

  REGS.dr32[20] = REGS.dr32[21];                        // RGB0 <- RGB1
  REGS.dr32[21] = REGS.dr32[22];                        // RGB1 <- RGB2
  REGS.dr32[22] = r | (g << 8) | (b << 16) | (c << 24); // RGB2 <- Value
}

ALWAYS_INLINE u32 GTE::UNRDivide(u32 lhs, u32 rhs)
{
  if (rhs * 2 <= lhs)
  {
    REGS.FLAG.divide_overflow = true;
    return 0x1FFFF;
  }

  const u32 shift = (rhs == 0) ? 16 : CountLeadingZeros(static_cast<u16>(rhs));
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

void GTE::MulMatVec(const s16* M_, const s16 Vx, const s16 Vy, const s16 Vz, u8 shift, bool lm)
{
#define M(i, j) M_[((i) * 3) + (j)]
#define dot3(i)                                                                                                        \
  TruncateAndSetMACAndIR<i + 1>(SignExtendMACResult<i + 1>((s64(M(i, 0)) * s64(Vx)) + (s64(M(i, 1)) * s64(Vy))) +      \
                                  (s64(M(i, 2)) * s64(Vz)),                                                            \
                                shift, lm)

  dot3(0);
  dot3(1);
  dot3(2);

#undef dot3
#undef M
}

void GTE::MulMatVec(const s16* M_, const s32 T[3], const s16 Vx, const s16 Vy, const s16 Vz, u8 shift, bool lm)
{
#define M(i, j) M_[((i) * 3) + (j)]
#define dot3(i)                                                                                                        \
  TruncateAndSetMACAndIR<i + 1>(                                                                                       \
    SignExtendMACResult<i + 1>(SignExtendMACResult<i + 1>((s64(T[i]) << 12) + (s64(M(i, 0)) * s64(Vx))) +              \
                               (s64(M(i, 1)) * s64(Vy))) +                                                             \
      (s64(M(i, 2)) * s64(Vz)),                                                                                        \
    shift, lm)

  dot3(0);
  dot3(1);
  dot3(2);

#undef dot3
#undef M
}

void GTE::MulMatVecBuggy(const s16* M_, const s32 T[3], const s16 Vx, const s16 Vy, const s16 Vz, u8 shift, bool lm)
{
#define M(i, j) M_[((i) * 3) + (j)]
#define dot3(i)                                                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    TruncateAndSetIR<i + 1>(static_cast<s32>(SignExtendMACResult<i + 1>(SignExtendMACResult<i + 1>(                    \
                                               (s64(T[i]) << 12) + (s64(M(i, 0)) * s64(Vx)))) >>                       \
                                             shift),                                                                   \
                            false);                                                                                    \
    TruncateAndSetMACAndIR<i + 1>(SignExtendMACResult<i + 1>((s64(M(i, 1)) * s64(Vy))) + (s64(M(i, 2)) * s64(Vz)),     \
                                  shift, lm);                                                                          \
  } while (0)

  dot3(0);
  dot3(1);
  dot3(2);

#undef dot3
#undef M
}

void GTE::Execute_MVMVA(Instruction inst)
{
  REGS.FLAG.Clear();

  static constexpr const s16* M_lookup[4] = {&REGS.RT[0][0], &REGS.LLM[0][0], &REGS.LCM[0][0], nullptr};
  static constexpr const s16* V_lookup[4][3] = {
    {&REGS.V0[0], &REGS.V0[1], &REGS.V0[2]},
    {&REGS.V1[0], &REGS.V1[1], &REGS.V1[2]},
    {&REGS.V2[0], &REGS.V2[1], &REGS.V2[2]},
    {&REGS.IR1, &REGS.IR2, &REGS.IR3},
  };
  static constexpr const s32 zero_T[3] = {};
  static constexpr const s32* T_lookup[4] = {REGS.TR, REGS.BK, REGS.FC, zero_T};

  const s16* M = M_lookup[inst.mvmva_multiply_matrix];
  const s16* const* const V = V_lookup[inst.mvmva_multiply_vector];
  const s32* const T = T_lookup[inst.mvmva_translation_vector];
  s16 buggy_M[3][3];

  if (!M)
  {
    // buggy
    buggy_M[0][0] = -static_cast<s16>(ZeroExtend16(REGS.RGBC[0]) << 4);
    buggy_M[0][1] = static_cast<s16>(ZeroExtend16(REGS.RGBC[0]) << 4);
    buggy_M[0][2] = REGS.IR0;
    buggy_M[1][0] = REGS.RT[0][2];
    buggy_M[1][1] = REGS.RT[0][2];
    buggy_M[1][2] = REGS.RT[0][2];
    buggy_M[2][0] = REGS.RT[1][1];
    buggy_M[2][1] = REGS.RT[1][1];
    buggy_M[2][2] = REGS.RT[1][1];
    M = &buggy_M[0][0];
  }

  const s16 Vx = *V[0];
  const s16 Vy = *V[1];
  const s16 Vz = *V[2];
  if (inst.mvmva_translation_vector != 2)
    MulMatVec(M, T, Vx, Vy, Vz, inst.GetShift(), inst.lm);
  else
    MulMatVecBuggy(M, T, Vx, Vy, Vz, inst.GetShift(), inst.lm);

  REGS.FLAG.UpdateError();
}

void GTE::Execute_SQR(Instruction inst)
{
  REGS.FLAG.Clear();

  // 32-bit multiply for speed - 16x16 isn't >32bit, and we know it won't overflow/underflow.
  const u8 shift = inst.GetShift();
  REGS.MAC1 = (s32(REGS.IR1) * s32(REGS.IR1)) >> shift;
  REGS.MAC2 = (s32(REGS.IR2) * s32(REGS.IR2)) >> shift;
  REGS.MAC3 = (s32(REGS.IR3) * s32(REGS.IR3)) >> shift;

  const bool lm = inst.lm;
  TruncateAndSetIR<1>(REGS.MAC1, lm);
  TruncateAndSetIR<2>(REGS.MAC2, lm);
  TruncateAndSetIR<3>(REGS.MAC3, lm);

  REGS.FLAG.UpdateError();
}

void GTE::Execute_OP(Instruction inst)
{
  REGS.FLAG.Clear();

  // Take copies since we overwrite them in each step.
  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;
  const s32 D1 = s32(REGS.RT[0][0]);
  const s32 D2 = s32(REGS.RT[1][1]);
  const s32 D3 = s32(REGS.RT[2][2]);
  const s32 IR1 = s32(REGS.IR1);
  const s32 IR2 = s32(REGS.IR2);
  const s32 IR3 = s32(REGS.IR3);

  // [MAC1,MAC2,MAC3] = [IR3*D2-IR2*D3, IR1*D3-IR3*D1, IR2*D1-IR1*D2] SAR (sf*12)
  // [IR1, IR2, IR3] = [MAC1, MAC2, MAC3]; copy result
  TruncateAndSetMACAndIR<1>(s64(IR3 * D2) - s64(IR2 * D3), shift, lm);
  TruncateAndSetMACAndIR<2>(s64(IR1 * D3) - s64(IR3 * D1), shift, lm);
  TruncateAndSetMACAndIR<3>(s64(IR2 * D1) - s64(IR1 * D2), shift, lm);

  REGS.FLAG.UpdateError();
}

void GTE::RTPS(const s16 V[3], u8 shift, bool lm, bool last)
{
#define dot3(i)                                                                                                        \
  SignExtendMACResult<i + 1>(SignExtendMACResult<i + 1>((s64(REGS.TR[i]) << 12) + (s64(REGS.RT[i][0]) * s64(V[0]))) +  \
                             (s64(REGS.RT[i][1]) * s64(V[1]))) +                                                       \
    (s64(REGS.RT[i][2]) * s64(V[2]))

  // IR1 = MAC1 = (TRX*1000h + RT11*VX0 + RT12*VY0 + RT13*VZ0) SAR (sf*12)
  // IR2 = MAC2 = (TRY*1000h + RT21*VX0 + RT22*VY0 + RT23*VZ0) SAR (sf*12)
  // IR3 = MAC3 = (TRZ*1000h + RT31*VX0 + RT32*VY0 + RT33*VZ0) SAR (sf*12)
  s64 x = dot3(0);
  s64 y = dot3(1);
  s64 z = dot3(2);

#ifdef ENABLE_FREECAM
  if (s_config.freecam_active)
    ApplyFreecam(x, y, z);
#endif

  TruncateAndSetMAC<1>(x, shift);
  TruncateAndSetMAC<2>(y, shift);
  TruncateAndSetMAC<3>(z, shift);
  TruncateAndSetIR<1>(REGS.MAC1, lm);
  TruncateAndSetIR<2>(REGS.MAC2, lm);

  // The command does saturate IR1,IR2,IR3 to -8000h..+7FFFh (regardless of lm bit). When using RTP with sf=0, then the
  // IR3 saturation flag (FLAG.22) gets set <only> if "MAC3 SAR 12" exceeds -8000h..+7FFFh (although IR3 is saturated
  // when "MAC3" exceeds -8000h..+7FFFh).
  TruncateAndSetIR<3>(s32(z >> 12), false);
  REGS.dr32[11] = std::clamp(REGS.MAC3, lm ? 0 : IR123_MIN_VALUE, IR123_MAX_VALUE);
#undef dot3

  // SZ3 = MAC3 SAR ((1-sf)*12)                           ;ScreenZ FIFO 0..+FFFFh
  PushSZ(s32(z >> 12));

  // MAC0=(((H*20000h/SZ3)+1)/2)*IR1+OFX, SX2=MAC0/10000h ;ScrX FIFO -400h..+3FFh
  // MAC0=(((H*20000h/SZ3)+1)/2)*IR2+OFY, SY2=MAC0/10000h ;ScrY FIFO -400h..+3FFh
  const s64 result = static_cast<s64>(ZeroExtend64(UNRDivide(REGS.H, REGS.SZ3)));

  s64 Sx;
  switch (s_config.aspect_ratio)
  {
    case DisplayAspectRatio::R16_9:
      Sx = ((((s64(result) * s64(REGS.IR1)) * s64(3)) / s64(4)) + s64(REGS.OFX));
      break;

    case DisplayAspectRatio::R19_9:
      Sx = ((((s64(result) * s64(REGS.IR1)) * s64(12)) / s64(19)) + s64(REGS.OFX));
      break;

    case DisplayAspectRatio::R20_9:
      Sx = ((((s64(result) * s64(REGS.IR1)) * s64(3)) / s64(5)) + s64(REGS.OFX));
      break;

    case DisplayAspectRatio::Custom:
      Sx = ((((s64(result) * s64(REGS.IR1)) * s64(s_config.custom_aspect_ratio_numerator)) /
             s64(s_config.custom_aspect_ratio_denominator)) +
            s64(REGS.OFX));
      break;

    case DisplayAspectRatio::Auto:
    case DisplayAspectRatio::R4_3:
    case DisplayAspectRatio::PAR1_1:
    default:
      Sx = (s64(result) * s64(REGS.IR1) + s64(REGS.OFX));
      break;
  }

  const s64 Sy = s64(result) * s64(REGS.IR2) + s64(REGS.OFY);
  CheckMACOverflow<0>(Sx);
  CheckMACOverflow<0>(Sy);
  PushSXY(s32(Sx >> 16), s32(Sy >> 16));

  if (g_settings.gpu_pgxp_enable)
  {
    float precise_sz3, precise_ir1, precise_ir2;

    if (g_settings.gpu_pgxp_preserve_proj_fp)
    {
      precise_sz3 = float(z) / 4096.0f;
      precise_ir1 = float(x) / (static_cast<float>(1 << shift));
      precise_ir2 = float(y) / (static_cast<float>(1 << shift));
      if (lm)
      {
        precise_ir1 = std::clamp(precise_ir1, float(IR123_MIN_VALUE), float(IR123_MAX_VALUE));
        precise_ir2 = std::clamp(precise_ir2, float(IR123_MIN_VALUE), float(IR123_MAX_VALUE));
      }
      else
      {
        precise_ir1 = std::min(precise_ir1, float(IR123_MAX_VALUE));
        precise_ir2 = std::min(precise_ir2, float(IR123_MAX_VALUE));
      }
    }
    else
    {
      precise_sz3 = float(REGS.SZ3);
      precise_ir1 = float(REGS.IR1);
      precise_ir2 = float(REGS.IR2);
    }

    // this can potentially use increased precision on Z
    const float precise_z = std::max<float>(float(REGS.H) / 2.0f, precise_sz3);
    const float precise_h_div_sz = float(REGS.H) / precise_z;
    const float fofx = float(REGS.OFX) / float(1 << 16);
    const float fofy = float(REGS.OFY) / float(1 << 16);
    float precise_x = precise_ir1 * precise_h_div_sz;

    switch (s_config.aspect_ratio)
    {
      case DisplayAspectRatio::Custom:
        precise_x = precise_x * s_config.custom_aspect_ratio_f;
        break;

      case DisplayAspectRatio::R16_9:
        precise_x = (precise_x * 3.0f) / 4.0f;
        break;

      case DisplayAspectRatio::R19_9:
        precise_x = (precise_x * 12.0f) / 19.0f;
        break;

      case DisplayAspectRatio::R20_9:
        precise_x = (precise_x * 3.0f) / 5.0f;
        break;

      case DisplayAspectRatio::Auto:
      case DisplayAspectRatio::R4_3:
      case DisplayAspectRatio::PAR1_1:
      default:
        break;
    }

    precise_x += fofx;

    float precise_y = fofy + (precise_ir2 * precise_h_div_sz);

    precise_x = std::clamp<float>(precise_x, -1024.0f, 1023.0f);
    precise_y = std::clamp<float>(precise_y, -1024.0f, 1023.0f);
    CPU::PGXP::GTE_RTPS(precise_x, precise_y, precise_z, REGS.dr32[14]);
  }

  if (last)
  {
    // MAC0=(((H*20000h/SZ3)+1)/2)*DQA+DQB, IR0=MAC0/1000h  ;Depth cueing 0..+1000h
    const s64 Sz = s64(result) * s64(REGS.DQA) + s64(REGS.DQB);
    TruncateAndSetMAC<0>(Sz, 0);
    TruncateAndSetIR<0>(s32(Sz >> 12), true);
  }
}

void GTE::Execute_RTPS(Instruction inst)
{
  REGS.FLAG.Clear();
  RTPS(REGS.V0, inst.GetShift(), inst.lm, true);
  REGS.FLAG.UpdateError();
}

void GTE::Execute_RTPT(Instruction inst)
{
  REGS.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  RTPS(REGS.V0, shift, lm, false);
  RTPS(REGS.V1, shift, lm, false);
  RTPS(REGS.V2, shift, lm, true);

  REGS.FLAG.UpdateError();
}

void GTE::Execute_NCLIP(Instruction inst)
{
  // MAC0 =   SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1
  REGS.FLAG.Clear();

  TruncateAndSetMAC<0>(s64(REGS.SXY0[0]) * s64(REGS.SXY1[1]) + s64(REGS.SXY1[0]) * s64(REGS.SXY2[1]) +
                         s64(REGS.SXY2[0]) * s64(REGS.SXY0[1]) - s64(REGS.SXY0[0]) * s64(REGS.SXY2[1]) -
                         s64(REGS.SXY1[0]) * s64(REGS.SXY0[1]) - s64(REGS.SXY2[0]) * s64(REGS.SXY1[1]),
                       0);

  REGS.FLAG.UpdateError();
}

void GTE::Execute_NCLIP_PGXP(Instruction inst)
{
  if (CPU::PGXP::GTE_HasPreciseVertices(REGS.dr32[12], REGS.dr32[13], REGS.dr32[14]))
  {
    REGS.FLAG.Clear();
    REGS.MAC0 = static_cast<s32>(CPU::PGXP::GTE_NCLIP());
  }
  else
  {
    Execute_NCLIP(inst);
  }
}

void GTE::Execute_AVSZ3(Instruction inst)
{
  REGS.FLAG.Clear();

  const s64 result = s64(REGS.ZSF3) * s32(u32(REGS.SZ1) + u32(REGS.SZ2) + u32(REGS.SZ3));
  TruncateAndSetMAC<0>(result, 0);
  SetOTZ(s32(result >> 12));

  REGS.FLAG.UpdateError();
}

void GTE::Execute_AVSZ4(Instruction inst)
{
  REGS.FLAG.Clear();

  const s64 result = s64(REGS.ZSF4) * s32(u32(REGS.SZ0) + u32(REGS.SZ1) + u32(REGS.SZ2) + u32(REGS.SZ3));
  TruncateAndSetMAC<0>(result, 0);
  SetOTZ(s32(result >> 12));

  REGS.FLAG.UpdateError();
}

ALWAYS_INLINE void GTE::InterpolateColor(s64 in_MAC1, s64 in_MAC2, s64 in_MAC3, u8 shift, bool lm)
{
  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
  //   [IR1,IR2,IR3] = (([RFC,GFC,BFC] SHL 12) - [MAC1,MAC2,MAC3]) SAR (sf*12)
  TruncateAndSetMACAndIR<1>((s64(REGS.FC[0]) << 12) - in_MAC1, shift, false);
  TruncateAndSetMACAndIR<2>((s64(REGS.FC[1]) << 12) - in_MAC2, shift, false);
  TruncateAndSetMACAndIR<3>((s64(REGS.FC[2]) << 12) - in_MAC3, shift, false);

  //   [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3])
  // [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)
  TruncateAndSetMACAndIR<1>(s64(s32(REGS.IR1) * s32(REGS.IR0)) + in_MAC1, shift, lm);
  TruncateAndSetMACAndIR<2>(s64(s32(REGS.IR2) * s32(REGS.IR0)) + in_MAC2, shift, lm);
  TruncateAndSetMACAndIR<3>(s64(s32(REGS.IR3) * s32(REGS.IR0)) + in_MAC3, shift, lm);
}

void GTE::NCS(const s16 V[3], u8 shift, bool lm)
{
  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
  MulMatVec(&REGS.LLM[0][0], V[0], V[1], V[2], shift, lm);

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(&REGS.LCM[0][0], REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();
}

void GTE::Execute_NCS(Instruction inst)
{
  REGS.FLAG.Clear();

  NCS(REGS.V0, inst.GetShift(), inst.lm);

  REGS.FLAG.UpdateError();
}

void GTE::Execute_NCT(Instruction inst)
{
  REGS.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  NCS(REGS.V0, shift, lm);
  NCS(REGS.V1, shift, lm);
  NCS(REGS.V2, shift, lm);

  REGS.FLAG.UpdateError();
}

void GTE::NCCS(const s16 V[3], u8 shift, bool lm)
{
  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
  MulMatVec(&REGS.LLM[0][0], V[0], V[1], V[2], shift, lm);

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(&REGS.LCM[0][0], REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4          ;<--- for NCDx/NCCx
  // [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)       ;<--- for NCDx/NCCx
  TruncateAndSetMACAndIR<1>(s64(s32(ZeroExtend32(REGS.RGBC[0])) * s32(REGS.IR1)) << 4, shift, lm);
  TruncateAndSetMACAndIR<2>(s64(s32(ZeroExtend32(REGS.RGBC[1])) * s32(REGS.IR2)) << 4, shift, lm);
  TruncateAndSetMACAndIR<3>(s64(s32(ZeroExtend32(REGS.RGBC[2])) * s32(REGS.IR3)) << 4, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();
}

void GTE::Execute_NCCS(Instruction inst)
{
  REGS.FLAG.Clear();

  NCCS(REGS.V0, inst.GetShift(), inst.lm);

  REGS.FLAG.UpdateError();
}

void GTE::Execute_NCCT(Instruction inst)
{
  REGS.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  NCCS(REGS.V0, shift, lm);
  NCCS(REGS.V1, shift, lm);
  NCCS(REGS.V2, shift, lm);

  REGS.FLAG.UpdateError();
}

void GTE::NCDS(const s16 V[3], u8 shift, bool lm)
{
  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
  MulMatVec(&REGS.LLM[0][0], V[0], V[1], V[2], shift, lm);

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(&REGS.LCM[0][0], REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  // No need to assign these to MAC[1-3], as it'll never overflow.
  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4          ;<--- for NCDx/NCCx
  const s32 in_MAC1 = (s32(ZeroExtend32(REGS.RGBC[0])) * s32(REGS.IR1)) << 4;
  const s32 in_MAC2 = (s32(ZeroExtend32(REGS.RGBC[1])) * s32(REGS.IR2)) << 4;
  const s32 in_MAC3 = (s32(ZeroExtend32(REGS.RGBC[2])) * s32(REGS.IR3)) << 4;

  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0                   ;<--- for NCDx only
  InterpolateColor(in_MAC1, in_MAC2, in_MAC3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();
}

void GTE::Execute_NCDS(Instruction inst)
{
  REGS.FLAG.Clear();

  NCDS(REGS.V0, inst.GetShift(), inst.lm);

  REGS.FLAG.UpdateError();
}

void GTE::Execute_NCDT(Instruction inst)
{
  REGS.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  NCDS(REGS.V0, shift, lm);
  NCDS(REGS.V1, shift, lm);
  NCDS(REGS.V2, shift, lm);

  REGS.FLAG.UpdateError();
}

void GTE::Execute_CC(Instruction inst)
{
  REGS.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(&REGS.LCM[0][0], REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4
  // [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)
  TruncateAndSetMACAndIR<1>(s64(s32(ZeroExtend32(REGS.RGBC[0])) * s32(REGS.IR1)) << 4, shift, lm);
  TruncateAndSetMACAndIR<2>(s64(s32(ZeroExtend32(REGS.RGBC[1])) * s32(REGS.IR2)) << 4, shift, lm);
  TruncateAndSetMACAndIR<3>(s64(s32(ZeroExtend32(REGS.RGBC[2])) * s32(REGS.IR3)) << 4, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  REGS.FLAG.UpdateError();
}

void GTE::Execute_CDP(Instruction inst)
{
  REGS.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(&REGS.LCM[0][0], REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  // No need to assign these to MAC[1-3], as it'll never overflow.
  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4
  const s32 in_MAC1 = (s32(ZeroExtend32(REGS.RGBC[0])) * s32(REGS.IR1)) << 4;
  const s32 in_MAC2 = (s32(ZeroExtend32(REGS.RGBC[1])) * s32(REGS.IR2)) << 4;
  const s32 in_MAC3 = (s32(ZeroExtend32(REGS.RGBC[2])) * s32(REGS.IR3)) << 4;

  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0                   ;<--- for CDP only
  // [MAC1, MAC2, MAC3] = [MAC1, MAC2, MAC3] SAR(sf * 12)
  InterpolateColor(in_MAC1, in_MAC2, in_MAC3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  REGS.FLAG.UpdateError();
}

void GTE::DPCS(const u8 color[3], u8 shift, bool lm)
{
  // In: [IR1,IR2,IR3]=Vector, FC=Far Color, IR0=Interpolation value, CODE=MSB of RGBC
  // [MAC1,MAC2,MAC3] = [R,G,B] SHL 16                     ;<--- for DPCS/DPCT
  TruncateAndSetMAC<1>((s64(ZeroExtend64(color[0])) << 16), 0);
  TruncateAndSetMAC<2>((s64(ZeroExtend64(color[1])) << 16), 0);
  TruncateAndSetMAC<3>((s64(ZeroExtend64(color[2])) << 16), 0);

  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
  InterpolateColor(REGS.MAC1, REGS.MAC2, REGS.MAC3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();
}

void GTE::Execute_DPCS(Instruction inst)
{
  REGS.FLAG.Clear();

  DPCS(REGS.RGBC, inst.GetShift(), inst.lm);

  REGS.FLAG.UpdateError();
}

void GTE::Execute_DPCT(Instruction inst)
{
  REGS.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  for (u32 i = 0; i < 3; i++)
    DPCS(REGS.RGB0, shift, lm);

  REGS.FLAG.UpdateError();
}

void GTE::Execute_DCPL(Instruction inst)
{
  REGS.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  // No need to assign these to MAC[1-3], as it'll never overflow.
  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4          ;<--- for DCPL only
  const s32 in_MAC1 = (s32(ZeroExtend32(REGS.RGBC[0])) * s32(REGS.IR1)) << 4;
  const s32 in_MAC2 = (s32(ZeroExtend32(REGS.RGBC[1])) * s32(REGS.IR2)) << 4;
  const s32 in_MAC3 = (s32(ZeroExtend32(REGS.RGBC[2])) * s32(REGS.IR3)) << 4;

  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
  InterpolateColor(in_MAC1, in_MAC2, in_MAC3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  REGS.FLAG.UpdateError();
}

void GTE::Execute_INTPL(Instruction inst)
{
  REGS.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  // No need to assign these to MAC[1-3], as it'll never overflow.
  // [MAC1,MAC2,MAC3] = [IR1,IR2,IR3] SHL 12               ;<--- for INTPL only
  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
  InterpolateColor(s32(REGS.IR1) << 12, s32(REGS.IR2) << 12, s32(REGS.IR3) << 12, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  REGS.FLAG.UpdateError();
}

void GTE::Execute_GPL(Instruction inst)
{
  REGS.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  // [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SHL (sf*12)       ;<--- for GPL only
  // [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3]) SAR (sf*12)
  TruncateAndSetMACAndIR<1>((s64(s32(REGS.IR1) * s32(REGS.IR0)) + (s64(REGS.MAC1) << shift)), shift, lm);
  TruncateAndSetMACAndIR<2>((s64(s32(REGS.IR2) * s32(REGS.IR0)) + (s64(REGS.MAC2) << shift)), shift, lm);
  TruncateAndSetMACAndIR<3>((s64(s32(REGS.IR3) * s32(REGS.IR0)) + (s64(REGS.MAC3) << shift)), shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  REGS.FLAG.UpdateError();
}

void GTE::Execute_GPF(Instruction inst)
{
  REGS.FLAG.Clear();

  const u8 shift = inst.GetShift();
  const bool lm = inst.lm;

  // [MAC1,MAC2,MAC3] = [0,0,0]                            ;<--- for GPF only
  // [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3]) SAR (sf*12)
  TruncateAndSetMACAndIR<1>(s64(s32(REGS.IR1) * s32(REGS.IR0)), shift, lm);
  TruncateAndSetMACAndIR<2>(s64(s32(REGS.IR2) * s32(REGS.IR0)), shift, lm);
  TruncateAndSetMACAndIR<3>(s64(s32(REGS.IR3) * s32(REGS.IR0)), shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  REGS.FLAG.UpdateError();
}

void GTE::ExecuteInstruction(u32 inst_bits)
{
  const Instruction inst{inst_bits};
  switch (inst.command)
  {
    case 0x01:
      CPU::AddGTETicks(15);
      Execute_RTPS(inst);
      break;

    case 0x06:
    {
      CPU::AddGTETicks(8);
      if (g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_culling)
        Execute_NCLIP_PGXP(inst);
      else
        Execute_NCLIP(inst);
    }
    break;

    case 0x0C:
      CPU::AddGTETicks(6);
      Execute_OP(inst);
      break;

    case 0x10:
      CPU::AddGTETicks(8);
      Execute_DPCS(inst);
      break;

    case 0x11:
      CPU::AddGTETicks(7);
      Execute_INTPL(inst);
      break;

    case 0x12:
      CPU::AddGTETicks(8);
      Execute_MVMVA(inst);
      break;

    case 0x13:
      CPU::AddGTETicks(19);
      Execute_NCDS(inst);
      break;

    case 0x14:
      CPU::AddGTETicks(13);
      Execute_CDP(inst);
      break;

    case 0x16:
      CPU::AddGTETicks(44);
      Execute_NCDT(inst);
      break;

    case 0x1B:
      CPU::AddGTETicks(17);
      Execute_NCCS(inst);
      break;

    case 0x1C:
      CPU::AddGTETicks(11);
      Execute_CC(inst);
      break;

    case 0x1E:
      CPU::AddGTETicks(14);
      Execute_NCS(inst);
      break;

    case 0x20:
      CPU::AddGTETicks(30);
      Execute_NCT(inst);
      break;

    case 0x28:
      CPU::AddGTETicks(5);
      Execute_SQR(inst);
      break;

    case 0x29:
      CPU::AddGTETicks(8);
      Execute_DCPL(inst);
      break;

    case 0x2A:
      CPU::AddGTETicks(17);
      Execute_DPCT(inst);
      break;

    case 0x2D:
      CPU::AddGTETicks(5);
      Execute_AVSZ3(inst);
      break;

    case 0x2E:
      CPU::AddGTETicks(6);
      Execute_AVSZ4(inst);
      break;

    case 0x30:
      CPU::AddGTETicks(23);
      Execute_RTPT(inst);
      break;

    case 0x3D:
      CPU::AddGTETicks(5);
      Execute_GPF(inst);
      break;

    case 0x3E:
      CPU::AddGTETicks(5);
      Execute_GPL(inst);
      break;

    case 0x3F:
      CPU::AddGTETicks(39);
      Execute_NCCT(inst);
      break;

    default:
      Panic("Missing handler");
      break;
  }
}

GTE::InstructionImpl GTE::GetInstructionImpl(u32 inst_bits, TickCount* ticks)
{
  const Instruction inst{inst_bits};
  switch (inst.command)
  {
    case 0x01:
      *ticks = 15;
      return &Execute_RTPS;

    case 0x06:
    {
      *ticks = 8;
      if (g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_culling)
        return &Execute_NCLIP_PGXP;
      else
        return &Execute_NCLIP;
    }

    case 0x0C:
      *ticks = 6;
      return &Execute_OP;

    case 0x10:
      *ticks = 8;
      return &Execute_DPCS;

    case 0x11:
      *ticks = 7;
      return &Execute_INTPL;

    case 0x12:
      *ticks = 8;
      return &Execute_MVMVA;

    case 0x13:
      *ticks = 19;
      return &Execute_NCDS;

    case 0x14:
      *ticks = 13;
      return &Execute_CDP;

    case 0x16:
      *ticks = 44;
      return &Execute_NCDT;

    case 0x1B:
      *ticks = 17;
      return &Execute_NCCS;

    case 0x1C:
      *ticks = 11;
      return &Execute_CC;

    case 0x1E:
      *ticks = 14;
      return &Execute_NCS;

    case 0x20:
      *ticks = 30;
      return &Execute_NCT;

    case 0x28:
      *ticks = 5;
      return &Execute_SQR;

    case 0x29:
      *ticks = 8;
      return &Execute_DCPL;

    case 0x2A:
      *ticks = 17;
      return &Execute_DPCT;

    case 0x2D:
      *ticks = 5;
      return &Execute_AVSZ3;

    case 0x2E:
      *ticks = 6;
      return &Execute_AVSZ4;

    case 0x30:
      *ticks = 23;
      return &Execute_RTPT;

    case 0x3D:
      *ticks = 5;
      return &Execute_GPF;

    case 0x3E:
      *ticks = 5;
      return &Execute_GPL;

    case 0x3F:
      *ticks = 39;
      return &Execute_NCCT;

    default:
      Panic("Missing handler");
  }
}

#ifdef ENABLE_FREECAM

bool GTE::IsFreecamEnabled()
{
  return s_config.freecam_enabled;
}

void GTE::SetFreecamEnabled(bool enabled)
{
  if (s_config.freecam_enabled == enabled)
    return;

  s_config.freecam_enabled = enabled;
  if (enabled)
  {
    s_config.freecam_transform_changed.store(true, std::memory_order_release);
    s_config.freecam_update_time = Timer::GetCurrentValue();
  }
}

void GTE::SetFreecamMoveAxis(u32 axis, float x)
{
  DebugAssert(axis < 3);
  s_config.freecam_move.F32[axis] = x;
  SetFreecamEnabled(true);
}

void GTE::SetFreecamRotateAxis(u32 axis, float x)
{
  DebugAssert(axis < 3);
  s_config.freecam_turn.F32[axis] = x;
  SetFreecamEnabled(true);
}

void GTE::UpdateFreecam(u64 current_time)
{
  if (!s_config.freecam_enabled)
  {
    s_config.freecam_active = false;
    return;
  }

  const float dt = std::clamp(
    static_cast<float>(Timer::ConvertValueToSeconds(current_time - s_config.freecam_update_time)), 0.0f, 1.0f);
  s_config.freecam_update_time = current_time;

  bool changed = true;
  s_config.freecam_transform_changed.compare_exchange_strong(changed, false, std::memory_order_acq_rel);

  if (!(s_config.freecam_move == GSVector4::zero()).alltrue())
  {
    s_config.freecam_translation += s_config.freecam_move * GSVector4(s_config.freecam_move_speed * dt);
    changed = true;
  }

  if (!(s_config.freecam_turn == GSVector4::zero()).alltrue())
  {
    s_config.freecam_rotation += s_config.freecam_turn * GSVector4(s_config.freecam_turn_speed *
                                                                   static_cast<float>(std::numbers::pi / 180.0) * dt);

    // wrap around -360 degrees/360 degrees
    constexpr GSVector4 min_rot = GSVector4::cxpr(static_cast<float>(std::numbers::pi * -2.0));
    constexpr GSVector4 max_rot = GSVector4::cxpr(static_cast<float>(std::numbers::pi * 2.0));
    s_config.freecam_rotation =
      s_config.freecam_rotation.blend32(s_config.freecam_rotation + max_rot, (s_config.freecam_rotation < min_rot));
    s_config.freecam_rotation =
      s_config.freecam_rotation.blend32(s_config.freecam_rotation + min_rot, (s_config.freecam_rotation > max_rot));

    changed = true;
  }

  if (!changed)
    return;

  bool any_xform = false;
  s_config.freecam_matrix = GSMatrix4x4::Identity();

  // translate than rotate, since the camera is rotating around a point
  // remember, matrix transformation happens in the opposite of the multiplication order

  if (!s_config.freecam_reverse_transform_order)
  {
    if (s_config.freecam_translation.x != 0.0f || s_config.freecam_translation.y != 0.0f ||
        s_config.freecam_translation.z != 0.0f)
    {
      s_config.freecam_matrix = GSMatrix4x4::Translation(s_config.freecam_translation.x, s_config.freecam_translation.y,
                                                         s_config.freecam_translation.z);
      any_xform = true;
    }

    if (s_config.freecam_rotation.z != 0.0f)
    {
      s_config.freecam_matrix *= GSMatrix4x4::RotationZ(s_config.freecam_rotation.z);
      any_xform = true;
    }

    if (s_config.freecam_rotation.y != 0.0f)
    {
      s_config.freecam_matrix *= GSMatrix4x4::RotationY(s_config.freecam_rotation.y);
      any_xform = true;
    }

    if (s_config.freecam_rotation.x != 0.0f)
    {
      s_config.freecam_matrix *= GSMatrix4x4::RotationX(s_config.freecam_rotation.x);
      any_xform = true;
    }
  }
  else
  {
    if (s_config.freecam_rotation.x != 0.0f)
    {
      s_config.freecam_matrix *= GSMatrix4x4::RotationX(s_config.freecam_rotation.x);
      any_xform = true;
    }

    if (s_config.freecam_rotation.y != 0.0f)
    {
      s_config.freecam_matrix *= GSMatrix4x4::RotationY(s_config.freecam_rotation.y);
      any_xform = true;
    }

    if (s_config.freecam_rotation.z != 0.0f)
    {
      s_config.freecam_matrix *= GSMatrix4x4::RotationZ(s_config.freecam_rotation.z);
      any_xform = true;
    }

    if (s_config.freecam_translation.x != 0.0f || s_config.freecam_translation.y != 0.0f ||
        s_config.freecam_translation.z != 0.0f)
    {
      s_config.freecam_matrix *= GSMatrix4x4::Translation(
        s_config.freecam_translation.x, s_config.freecam_translation.y, s_config.freecam_translation.z);
      any_xform = true;
    }
  }

  s_config.freecam_active = any_xform;
}

void GTE::ResetFreecam()
{
  s_config.freecam_active = false;
  s_config.freecam_rotation = GSVector4::zero();
  s_config.freecam_translation = GSVector4::zero();
  s_config.freecam_transform_changed.store(false, std::memory_order_release);
}

void GTE::ApplyFreecam(s64& x, s64& y, s64& z)
{
  constexpr double scale = 1 << 12;

  GSVector4 xyz(static_cast<float>(static_cast<double>(x) / scale), static_cast<float>(static_cast<double>(y) / scale),
                static_cast<float>(static_cast<double>(z) / scale), 1.0f);

  xyz = s_config.freecam_matrix * xyz;

  x = static_cast<s64>(static_cast<double>(xyz.x) * scale);
  y = static_cast<s64>(static_cast<double>(xyz.y) * scale);
  z = static_cast<s64>(static_cast<double>(xyz.z) * scale);
}

void GTE::DrawFreecamWindow(float scale)
{
  const ImGuiStyle& style = ImGui::GetStyle();
  const float label_width = 140.0f * scale;
  const float item_width = 350.0f * scale;
  const float padding_height = 5.0f * scale;

  bool freecam_enabled = s_config.freecam_enabled;
  bool enabled_changed = false;
  bool changed = false;

  if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen))
  {
    const float third_width = 50.0f * scale;
    const float second_width = item_width - third_width;

    enabled_changed = ImGui::Checkbox("Enable Freecam", &freecam_enabled);

    changed |= ImGui::Checkbox("Reverse Transform Order", &s_config.freecam_reverse_transform_order);
    ImGui::SetItemTooltip("Swaps the order that the camera rotation/offset is applied.\nCan work better in some games "
                          "that use different modelview matrices.");

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + padding_height);

    ImGui::Columns(3, "Settings", false);
    ImGui::SetColumnWidth(0, label_width);
    ImGui::SetColumnWidth(1, second_width);
    ImGui::SetColumnWidth(2, third_width);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.ItemInnerSpacing.y);
    ImGui::TextUnformatted("Movement Speed:");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(second_width);
    ImGui::DragFloat("##MovementSpeed", &s_config.freecam_move_speed, 1.0f, 0.0f, FREECAM_MAX_MOVE_SPEED);
    ImGui::NextColumn();
    if (ImGui::Button("Reset##ResetMovementSpeed"))
      s_config.freecam_move_speed = FREECAM_DEFAULT_MOVE_SPEED;
    ImGui::NextColumn();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.ItemInnerSpacing.y);
    ImGui::TextUnformatted("Turning Speed:");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(second_width);
    ImGui::DragFloat("##TurnSpeed", &s_config.freecam_turn_speed, 1.0f, 0.0f, FREECAM_MAX_TURN_SPEED);
    ImGui::NextColumn();
    if (ImGui::Button("Reset##ResetTurnSpeed"))
      s_config.freecam_turn_speed = FREECAM_DEFAULT_TURN_SPEED;
    ImGui::NextColumn();

    ImGui::Columns(1);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + padding_height);
  }

  if (ImGui::CollapsingHeader("Rotation", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Columns(2, "Rotation", false);
    ImGui::SetColumnWidth(0, label_width);
    ImGui::SetColumnWidth(1, item_width);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.ItemInnerSpacing.y);
    ImGui::TextUnformatted("X Rotation (Pitch):");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(item_width);
    changed |= ImGui::SliderAngle("##XRot", &s_config.freecam_rotation.x, FREECAM_MIN_ROTATION, FREECAM_MAX_ROTATION);
    ImGui::NextColumn();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.ItemInnerSpacing.y);
    ImGui::TextUnformatted("Y Rotation (Yaw):");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(item_width);
    changed |= ImGui::SliderAngle("##YRot", &s_config.freecam_rotation.y, FREECAM_MIN_ROTATION, FREECAM_MAX_ROTATION);
    ImGui::NextColumn();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.ItemInnerSpacing.y);
    ImGui::TextUnformatted("Z Rotation (Roll):");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(item_width);
    changed |= ImGui::SliderAngle("##ZRot", &s_config.freecam_rotation.z, FREECAM_MIN_ROTATION, FREECAM_MAX_ROTATION);
    ImGui::NextColumn();

    ImGui::Columns(1);

    if (ImGui::Button("Reset##ResetRotation"))
    {
      s_config.freecam_rotation = GSVector4::zero();
      changed = true;
    }

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + padding_height);
  }

  if (ImGui::CollapsingHeader("Translation", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Columns(2, "Translation", false);
    ImGui::SetColumnWidth(0, label_width);
    ImGui::SetColumnWidth(1, item_width);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.ItemInnerSpacing.y);
    ImGui::TextUnformatted("X Offset:");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(item_width);
    changed |= ImGui::DragFloat("##XOffset", &s_config.freecam_translation.x, 1.0f, FREECAM_MIN_TRANSLATION,
                                FREECAM_MAX_TRANSLATION, "%.1f", ImGuiSliderFlags_None);
    ImGui::NextColumn();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.ItemInnerSpacing.y);
    ImGui::TextUnformatted("Y Offset:");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(item_width);
    changed |= ImGui::DragFloat("##YOffset", &s_config.freecam_translation.y, 1.0f, FREECAM_MIN_TRANSLATION,
                                FREECAM_MAX_TRANSLATION, "%.1f", ImGuiSliderFlags_None);
    ImGui::NextColumn();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.ItemInnerSpacing.y);
    ImGui::TextUnformatted("Z Offset:");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(item_width);
    changed |= ImGui::DragFloat("##ZOffset", &s_config.freecam_translation.z, 1.0f, FREECAM_MIN_TRANSLATION,
                                FREECAM_MAX_TRANSLATION, "%.1f", ImGuiSliderFlags_None);
    ImGui::NextColumn();

    ImGui::Columns(1);

    if (ImGui::Button("Reset##ResetTranslation"))
    {
      s_config.freecam_translation = GSVector4::zero();
      changed = true;
    }

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + padding_height);
  }

  if (enabled_changed || (!freecam_enabled && changed))
    Host::RunOnCPUThread([enabled = freecam_enabled || changed]() { SetFreecamEnabled(enabled); });

  if (changed)
    s_config.freecam_transform_changed.store(true, std::memory_order_release);
}

#else // ENABLE_FREECAM

bool GTE::IsFreecamEnabled()
{
  return false;
}

void GTE::SetFreecamEnabled(bool enabled)
{
}

void GTE::SetFreecamMoveAxis(u32 axis, float x)
{
}

void GTE::SetFreecamRotateAxis(u32 axis, float x)
{
}

void GTE::UpdateFreecam(u64 current_time)
{
}

void GTE::ResetFreecam()
{
}

void GTE::DrawFreecamWindow(float scale)
{
}

#endif // ENABLE_FREECAM
