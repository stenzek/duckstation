#pragma once
#include "common/bitfield.h"
#include "types.h"

namespace GTE {

enum : u32
{
  NUM_DATA_REGS = 32,
  NUM_CONTROL_REGS = 32,
  NUM_REGS = NUM_DATA_REGS + NUM_CONTROL_REGS
};

union FLAGS
{
  u32 bits;

  BitField<u32, bool, 31, 1> error;
  BitField<u32, bool, 30, 1> mac1_overflow;
  BitField<u32, bool, 29, 1> mac2_overflow;
  BitField<u32, bool, 28, 1> mac3_overflow;
  BitField<u32, bool, 27, 1> mac1_underflow;
  BitField<u32, bool, 26, 1> mac2_underflow;
  BitField<u32, bool, 25, 1> mac3_underflow;
  BitField<u32, bool, 24, 1> ir1_saturated;
  BitField<u32, bool, 23, 1> ir2_saturated;
  BitField<u32, bool, 22, 1> ir3_saturated;
  BitField<u32, bool, 21, 1> color_r_saturated;
  BitField<u32, bool, 20, 1> color_g_saturated;
  BitField<u32, bool, 19, 1> color_b_saturated;
  BitField<u32, bool, 18, 1> sz1_otz_saturated;
  BitField<u32, bool, 17, 1> divide_overflow;
  BitField<u32, bool, 16, 1> mac0_overflow;
  BitField<u32, bool, 15, 1> mac0_underflow;
  BitField<u32, bool, 14, 1> sx2_saturated;
  BitField<u32, bool, 13, 1> sy2_saturated;
  BitField<u32, bool, 12, 1> ir0_saturated;

  static constexpr u32 WRITE_MASK = UINT32_C(0xFFFFF000);

  ALWAYS_INLINE void Clear() { bits = 0; }

  // Bits 30..23, 18..13 OR'ed
  ALWAYS_INLINE void UpdateError() { error = (bits & UINT32_C(0x7F87E000)) != UINT32_C(0); }
};

union Regs
{
  struct
  {
    u32 dr32[NUM_DATA_REGS];
    u32 cr32[NUM_CONTROL_REGS];
  };

  u32 r32[NUM_DATA_REGS + NUM_CONTROL_REGS];

#pragma pack(push, 1)
  struct
  {
    s16 V0[3];     // 0-1
    u16 pad1;      // 1
    s16 V1[3];     // 2-3
    u16 pad2;      // 3
    s16 V2[3];     // 4-5
    u16 pad3;      // 5
    u8 RGBC[4];    // 6
    u16 OTZ;       // 7
    u16 pad4;      // 7
    s16 IR0;       // 8
    u16 pad5;      // 8
    s16 IR1;       // 9
    u16 pad6;      // 9
    s16 IR2;       // 10
    u16 pad7;      // 10
    s16 IR3;       // 11
    u16 pad8;      // 11
    s16 SXY0[2];   // 12
    s16 SXY1[2];   // 13
    s16 SXY2[2];   // 14
    s16 SXYP[2];   // 15
    u16 SZ0;       // 16
    u16 pad13;     // 16
    u16 SZ1;       // 17
    u16 pad14;     // 17
    u16 SZ2;       // 18
    u16 pad15;     // 18
    u16 SZ3;       // 19
    u16 pad16;     // 19
    u8 RGB0[4];    // 20
    u8 RGB1[4];    // 21
    u8 RGB2[4];    // 22
    u32 RES1;      // 23
    s32 MAC0;      // 24
    s32 MAC1;      // 25
    s32 MAC2;      // 26
    s32 MAC3;      // 27
    u32 IRGB;      // 28
    u32 ORGB;      // 29
    s32 LZCS;      // 30
    u32 LZCR;      // 31
    s16 RT[3][3];  // 32-36
    u16 pad17;     // 36
    s32 TR[3];     // 37-39
    s16 LLM[3][3]; // 40-44
    u16 pad18;     // 44
    s32 BK[3];     // 45-47
    s16 LCM[3][3]; // 48-52
    u16 pad19;     // 52
    s32 FC[3];     // 53-55
    s32 OFX;       // 56
    s32 OFY;       // 57
    u16 H;         // 58
    u16 pad20;     // 58
    s16 DQA;       // 59
    u16 pad21;     // 59
    s32 DQB;       // 60
    s16 ZSF3;      // 61
    u16 pad22;     // 61
    s16 ZSF4;      // 62
    u16 pad23;     // 62
    FLAGS FLAG;    // 63
  };
#pragma pack(pop)
};
static_assert(sizeof(Regs) == (sizeof(u32) * NUM_REGS));

union Instruction
{
  u32 bits;

  BitField<u32, u8, 20, 5> fake_command;
  BitField<u32, u8, 19, 1> sf; // shift fraction in IR registers, 0=no fraction, 1=12bit fraction
  BitField<u32, u8, 17, 2> mvmva_multiply_matrix;
  BitField<u32, u8, 15, 2> mvmva_multiply_vector;
  BitField<u32, u8, 13, 2> mvmva_translation_vector;
  BitField<u32, bool, 10, 1> lm; // saturate IR1, IR2, IR3 result
  BitField<u32, u8, 0, 6> command;

  ALWAYS_INLINE u8 GetShift() const { return sf ? 12 : 0; }

  // only the first 20 bits are needed to execute
  static constexpr u32 REQUIRED_BITS_MASK = ((1 << 20) - 1);
};

} // namespace GTE
