#pragma once
#include "common/bitfield.h"
#include "types.h"

namespace GTE {
static constexpr u32 NUM_REGS = 64;

union Regs
{
  u32 r32[NUM_REGS];

#pragma pack(push, 1)
  struct
  {
    s16 V0[3];    // 0-1
    u16 pad1;     // 1
    s16 V1[3];    // 2-3
    u16 pad2;     // 3
    s16 V2[3];    // 4-5
    u16 pad3;     // 5
    u8 RGBC[4];   // 6
    u16 OTZ;      // 7
    u16 pad4;     // 7
    s16 IR0;      // 8
    u16 pad5;     // 8
    s16 IR1;      // 9
    u16 pad6;     // 9
    s16 IR2;      // 10
    u16 pad7;     // 10
    s16 IR3;      // 11
    u16 pad8;     // 11
    s16 SXY0;     // 12
    u16 pad9;     // 12
    s16 SXY1;     // 13
    u16 pad10;    // 13
    s16 SXY2;     // 14
    u16 pad11;    // 14
    s16 SXYP;     // 15
    u16 pad12;    // 15
    u16 SZ0;      // 16
    u16 pad13;    // 16
    u16 SZ1;      // 17
    u16 pad14;    // 17
    u16 SZ2;      // 18
    u16 pad15;    // 18
    u16 SZ3;      // 19
    u16 pad16;    // 19
    u32 RGB0;     // 20
    u32 RGB1;     // 21
    u32 RGB2;     // 22
    u32 RES1;     // 23
    s32 MAC0;     // 24
    s32 MAC1;     // 25
    s32 MAC2;     // 26
    s32 MAC4;     // 27
    u16 IRGB;     // 28
    u16 ORGB;     // 29
    s32 LZCS;     // 30
    s32 LZCR;     // 31
    u16 RT[3][3]; // 32-36
    u16 pad17;    // 36
    s32 TR[3];    // 37-39
    u16 L[3][3];  // 40-44
    u16 pad18;    // 44
    u32 RBK;      // 45
    u32 GBK;      // 46
    u32 BBK;      // 47
    u16 LR[3][3]; // 48-52
    u16 pad19;    // 52
    u32 RFC;      // 53
    u32 GFC;      // 54
    u32 BFC;      // 55
    u32 OFX;      // 56
    u32 OFY;      // 57
    u16 H;        // 58
    u16 pad20;    // 58
    u16 DQA;      // 59
    u16 pad21;    // 59
    u32 DQB;      // 60
    u16 ZSF3;     // 61
    u16 pad22;    // 61
    u16 ZSF4;     // 62
    u16 pad23;    // 62
    u32 FLAG;     // 63
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
};

} // namespace GTE