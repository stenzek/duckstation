// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"
#include "core/cpu_pgxp.h"
#include "core/gte.h"
#include "core/settings.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

namespace {

using namespace CPU::PGXP;

enum : u32
{
  R_ZERO = 0,
  R_V0 = 2,
  R_V1 = 3,
  R_A0 = 4,
  R_A1 = 5,
  R_T2 = 10,
};

static constexpr double TOLERANCE = 0.001;

class PGXPTest : public ::testing::Test
{
protected:
  void SetUp() override { CPU::PGXP::Reset(); }

  static CPU::Instruction R(u32 rs, u32 rt, u32 rd, u32 sh = 0)
  {
    CPU::Instruction inst{};
    inst.r.rs = static_cast<CPU::Reg>(rs);
    inst.r.rt = static_cast<CPU::Reg>(rt);
    inst.r.rd = static_cast<CPU::Reg>(rd);
    inst.r.shamt = sh;
    return inst;
  }

  static CPU::Instruction I(u32 rs, u32 rt, u32 imm)
  {
    CPU::Instruction inst{};
    inst.i.rs = static_cast<CPU::Reg>(rs);
    inst.i.rt = static_cast<CPU::Reg>(rt);
    inst.i.imm = imm;
    return inst;
  }

  static void Set(u32 reg, u32 value, float x, float y, u32 flags = VALID_ALL, float z = 160.0f)
  {
    CPU::g_state.pgxp_gpr[reg] = {x, y, z, value, flags};
  }

  static const CPU::PGXPValue& Get(u32 reg) { return CPU::g_state.pgxp_gpr[reg]; }

  // The 32-bit value the precise halves represent, reading the low half as unsigned.
  static double Scalar(const CPU::PGXPValue& v) { return ((v.x < 0.0f) ? (v.x + 65536.0) : v.x) + 65536.0 * v.y; }
};

} // namespace

//////////////////////////////////////////////////////////////////////////
// Right shifts: upper half after shifting
//////////////////////////////////////////////////////////////////////////

TEST_F(PGXPTest, SRAUpperHalfHasNoRemainderFromShiftedBits)
{
  // Rage Racer: the shifted-out integer bits of -4416 were left as a fraction in the upper half (-1.078125).
  Set(R_V0, 0xEEC00FFF, 4095.0f, -4416.0f);
  CPU::PGXP::CPU_SRA(R(0, R_V0, R_V0, 12), 0xEEC00FFF);
  EXPECT_EQ(Get(R_V0).value, 0xFFFEEC00u);
  EXPECT_EQ(Get(R_V0).y, -2.0f);
  EXPECT_NEAR(Scalar(Get(R_V0)), static_cast<double>(0xFFFEEC00u) - 4294967296.0, 1.0);
}

TEST_F(PGXPTest, SRLUpperHalfHasNoRemainderFromShiftedBits)
{
  Set(R_V0, 0x12345678, 22136.0f, 4660.0f);
  CPU::PGXP::CPU_SRL(R(0, R_V0, R_V1, 4), 0x12345678);
  EXPECT_EQ(Get(R_V1).y, 291.0f);
}

TEST_F(PGXPTest, SRAVUpperHalfHasNoRemainderFromShiftedBits)
{
  Set(R_V0, 0xEEC00FFF, 4095.0f, -4416.0f);
  Set(R_A0, 12, 12.0f, 0.0f, VALID_XY);
  CPU::PGXP::CPU_SRAV(R(R_A0, R_V0, R_V1), 12, 0xEEC00FFF);
  EXPECT_EQ(Get(R_V1).y, -2.0f);
}

TEST_F(PGXPTest, RageRacerSkyArithmeticChain)
{
  // ADD with the destination as a source keeps depth, which takes SRA down the precise path.
  Set(R_A0, 0xEEC00000, 0.0f, -4416.0f, VALID_XY, 0.0f);
  Set(R_V0, 0, 0.0f, 0.0f, VALID_ALL, 41459.0f);
  CPU::PGXP::CPU_ADD(R(R_A0, R_V0, R_V0), 0xEEC00000, 0);
  EXPECT_TRUE(Get(R_V0).flags & VALID_Z);
  EXPECT_EQ(Get(R_V0).z, 41459.0f);

  CPU::PGXP::CPU_ADDI(I(R_V0, R_V0, 4095), 0xEEC00000);
  CPU::PGXP::CPU_SRA(R(0, R_V0, R_V0, 12), 0xEEC00FFF);
  EXPECT_EQ(Get(R_V0).y, -2.0f);

  Set(R_A1, 0xA000, -24576.0f, 0.0f, VALID_X, 0.0f);
  CPU::PGXP::CPU_ADD(R(R_V0, R_A1, R_V0), 0xFFFEEC00, 0xA000);
  Set(R_T2, 0x20000, 0.0f, 2.0f, 0, 0.0f);
  CPU::PGXP::CPU_ADD(R(R_V0, R_T2, R_A1), 0xFFFF8C00, 0x20000);
  CPU::PGXP::CPU_SRA(R(0, R_A1, R_V0, 8), 0x18C00);

  // X was 632 instead of 396 when the remainder was carried into it.
  EXPECT_EQ(Get(R_V0).value, 396u);
  EXPECT_EQ(Get(R_V0).x, 396.0f);
  EXPECT_TRUE(Get(R_V0).flags & VALID_Z);
  EXPECT_EQ(Get(R_V0).z, 41459.0f);
}

TEST_F(PGXPTest, ShiftKeepsPackedUpperFraction)
{
  // Native Y is even, so none of its bits reach X, and its fraction must stay in Y.
  for (const bool arithmetic : {false, true})
  {
    Set(R_V0, 0x02000064, 100.25f, 512.5f);
    if (arithmetic)
      CPU::PGXP::CPU_SRA(R(0, R_V0, R_V0, 1), 0x02000064);
    else
      CPU::PGXP::CPU_SRL(R(0, R_V0, R_V0, 1), 0x02000064);
    EXPECT_EQ(Get(R_V0).value, 0x01000032u);
    EXPECT_NEAR(Get(R_V0).x, 50.125, TOLERANCE);
    EXPECT_NEAR(Get(R_V0).y, 256.25, TOLERANCE);

    CPU::PGXP::CPU_SRA(R(0, R_V0, R_V1, 16), 0x01000032);
    EXPECT_NEAR(Get(R_V1).x, 256.25, TOLERANCE);
  }
}

TEST_F(PGXPTest, ShiftKeepsPackedUpperFractionWhenBitsReachX)
{
  // Native Y is odd, so it isn't only sign bits after the shift, and keeps its offset from native. Only its native bit
  // moves into X, otherwise Y's fraction lands in X scaled by 32768 (Driver 2 averages packed vertices this way).
  for (const bool arithmetic : {false, true})
  {
    for (const u32 dest : {R_V0, R_V1})
    {
      Set(R_V0, 0x02010064, 100.25f, 513.5f);
      if (arithmetic)
        CPU::PGXP::CPU_SRA(R(0, R_V0, dest, 1), 0x02010064);
      else
        CPU::PGXP::CPU_SRL(R(0, R_V0, dest, 1), 0x02010064);
      EXPECT_EQ(Get(dest).value, 0x01008032u);
      EXPECT_NEAR(Get(dest).x, 50.125 - 32768.0, TOLERANCE);
      EXPECT_NEAR(Get(dest).y, 256.25, TOLERANCE);

      CPU::PGXP::CPU_SRA(R(0, dest, R_A0, 16), 0x01008032);
      EXPECT_NEAR(Get(R_A0).x, 256.25, TOLERANCE);
    }
  }
}

TEST_F(PGXPTest, ShiftKeepsFractionalLowHalf)
{
  Set(R_V0, 100, 100.25f, 0.0f);
  CPU::PGXP::CPU_SRA(R(0, R_V0, R_V1, 1), 100);
  EXPECT_NEAR(Get(R_V1).x, 50.125, TOLERANCE);
}

TEST_F(PGXPTest, ShiftKeepsWrappedLowHalfSign)
{
  // Spyro: precise 32905.1 is stored wrapped as -32630.9, while native 0x7F9E is positive.
  Set(R_V0, 0x7F9E, -32630.9f, 0.0f);
  CPU::PGXP::CPU_SRA(R(0, R_V0, R_V1, 5), 0x7F9E);
  EXPECT_NEAR(Get(R_V1).x, 32905.1 / 32.0, TOLERANCE);
}

TEST_F(PGXPTest, ShiftDropsFractionOfBitsShiftedOut)
{
  // 007 Racing packs vertices as y:10 x:10 z:12, here 173.25, 142.125 and 20. The fraction of y sits in the bits of x,
  // so it must not stay behind when y is shifted out.
  Set(R_V0, 0x2B48E014, -7660.0f, 11096.0f);
  CPU::PGXP::CPU_SLL(R(0, R_V0, R_V1, 10), 0x2B48E014);
  EXPECT_EQ(Get(R_V1).value, 0x23805000u);
  EXPECT_NEAR(Get(R_V1).y, 9096.0, TOLERANCE);
  CPU::PGXP::CPU_SRA(R(0, R_V1, R_A0, 22), 0x23805000);
  EXPECT_NEAR(Get(R_A0).x, 142.125, TOLERANCE);
  CPU::PGXP::CPU_SRA(R(0, R_V0, R_A1, 22), 0x2B48E014);
  EXPECT_NEAR(Get(R_A1).x, 11096.0 / 64.0, TOLERANCE); // the top bits of x become a fraction

  // Without bits shifted out, the fraction of y is kept.
  Set(R_V0, 0x015A011C, 284.25f, 346.5f);
  CPU::PGXP::CPU_SLL(R(0, R_V0, R_V1, 5), 0x015A011C);
  EXPECT_NEAR(Get(R_V1).y, 346.5 * 32.0, TOLERANCE);
}

TEST_F(PGXPTest, BitfieldExtractDoesNotTakeFractionFromFieldBelow)
{
  // Syphon Filter 3 packs vertices as z:10 y:11 x:11, here x = -50 and y = -19, and extracts y with sll 10; sra 21.
  // The top bits of x are below y, and gave it a fraction of 31/32.
  Set(R_V0, 0x13FF6FCE, 28622.0f, 5119.0f);
  CPU::PGXP::CPU_SLL(R(0, R_V0, R_V1, 10), 0x13FF6FCE);
  EXPECT_EQ(Get(R_V1).value, 0xFDBF3800u);
  CPU::PGXP::CPU_SRA(R(0, R_V1, R_A0, 21), 0xFDBF3800);
  EXPECT_EQ(Get(R_A0).value, 0xFFFFFFEDu);
  EXPECT_NEAR(Get(R_A0).x, -19.0, TOLERANCE);
  EXPECT_NEAR(Get(R_A0).y, -1.0, TOLERANCE);
  EXPECT_FALSE(Get(R_A0).flags & BITFIELD);

  // Without bits shifted out by a left shift, the bits shifted out are a fraction.
  Set(R_V1, 0xFDBF3800, 14336.0f, -577.0f);
  CPU::PGXP::CPU_SRA(R(0, R_V1, R_A0, 21), 0xFDBF3800);
  EXPECT_NEAR(Get(R_A0).x, -577.0 / 32.0, TOLERANCE);
}

//////////////////////////////////////////////////////////////////////////
// Right shifts: sign of an upper half that is only sign bits
//////////////////////////////////////////////////////////////////////////

TEST_F(PGXPTest, ShiftSignFillFollowsNegativePreciseValue)
{
  // Rage Racer: native 45, precise -57.9. A zero upper half made the halves represent 65478.
  Set(R_V1, 0x000B42FC, -31276.53515625f, -15.0f);
  CPU::PGXP::CPU_SRA(R(0, R_V1, R_V1, 14), 0x000B42FC);
  EXPECT_EQ(Get(R_V1).value, 45u);
  EXPECT_NEAR(Get(R_V1).x, -57.9089661, TOLERANCE);
  EXPECT_EQ(Get(R_V1).y, -1.0f);
  EXPECT_NEAR(Scalar(Get(R_V1)), Get(R_V1).x, TOLERANCE);

  // The multiplication then doesn't gain 65536 times the multiplier.
  Set(R_A0, 2795, 2795.0f, 0.0f, VALID_XY, 0.0f);
  CPU::PGXP::CPU_MULT(R(R_V1, R_A0, 0), 45, 2795);
  const CPU::PGXPValue& lo = CPU::g_state.pgxp_gpr[static_cast<u8>(CPU::Reg::lo)];
  EXPECT_NEAR(Scalar(lo), static_cast<double>(Get(R_V1).x) * 2795.0, 0.1);
}

TEST_F(PGXPTest, ShiftSignFillFollowsPositivePreciseValue)
{
  Set(R_V0, 0xFFF40000, 31276.5f, 15.0f);
  CPU::PGXP::CPU_SRA(R(0, R_V0, R_V1, 14), 0xFFF40000);
  EXPECT_EQ(Get(R_V1).y, 0.0f);
  EXPECT_GT(Scalar(Get(R_V1)), 0.0);
}

TEST_F(PGXPTest, ShiftSignFillSplitsQuantizedValue)
{
  // The shifted value is a tiny negative that truncates to 0 in the low half, so the upper half must be 0 too.
  Set(R_V0, 0x00010000, 0.0f, -2.0e-10f);
  CPU::PGXP::CPU_SRA(R(0, R_V0, R_V1, 1), 0x00010000);
  EXPECT_EQ(Get(R_V1).value, 0x8000u);
  EXPECT_NEAR(Scalar(Get(R_V1)), 0.0, TOLERANCE);
}

TEST_F(PGXPTest, ShiftUpperHalfMatchesNativeForIntegerValues)
{
  static constexpr u32 inputs[] = {0x00000000, 0x00000001, 0xFFFFFFFF, 0xFFFF0000, 0x00010000, 0x12345678,
                                   0x80000000, 0x7FFFFFFF, 0x80008000, 0x00008000, 0xFFFF8000, 0x0000FFFF,
                                   0x80007FFF, 0xFFFEEC00, 0xFFF9D1F8, 0x68DB8BAD};
  for (const u32 input : inputs)
  {
    for (u32 sh = 1; sh < 32; sh++)
    {
      for (const bool arithmetic : {false, true})
      {
        for (const u32 dest : {R_V0, R_V1})
        {
          Set(R_V0, input, static_cast<float>(static_cast<s16>(input)),
              static_cast<float>(static_cast<s16>(input >> 16)));
          if (arithmetic)
            CPU::PGXP::CPU_SRA(R(0, R_V0, dest, sh), input);
          else
            CPU::PGXP::CPU_SRL(R(0, R_V0, dest, sh), input);

          const u32 expected = arithmetic ? static_cast<u32>(static_cast<s32>(input) >> sh) : (input >> sh);
          EXPECT_EQ(Get(dest).value, expected);
          EXPECT_EQ(Get(dest).y, static_cast<float>(static_cast<s16>(expected >> 16)))
            << (arithmetic ? "SRA " : "SRL ") << std::hex << input << " by " << std::dec << sh;
        }
      }
    }
  }
}

//////////////////////////////////////////////////////////////////////////
// Shifts: invalid components
//////////////////////////////////////////////////////////////////////////

TEST_F(PGXPTest, SLLIgnoresInvalidLowHalf)
{
  // The carry comes from native 0x8000, not the stale 777.25.
  Set(R_V0, 0x00648000, 777.25f, 100.25f, VALID_Y | VALID_Z);
  CPU::PGXP::CPU_SLL(R(0, R_V0, R_V1, 1), 0x00648000);
  EXPECT_NEAR(Get(R_V1).y, 201.5, TOLERANCE);

  Set(R_V0, 0x00640007, 777.25f, 100.25f, VALID_Y | VALID_Z);
  CPU::PGXP::CPU_SLL(R(0, R_V0, R_V1, 16), 0x00640007);
  EXPECT_EQ(Get(R_V1).y, 7.0f);
}

TEST_F(PGXPTest, SRLIgnoresInvalidUpperHalf)
{
  Set(R_V0, 0x12340064, 100.25f, 777.25f, VALID_X | VALID_Z);
  CPU::PGXP::CPU_SRL(R(0, R_V0, R_V1, 4), 0x12340064);
  EXPECT_NEAR(Get(R_V1).x, 16390.265625, TOLERANCE);

  Set(R_V0, 0x00640007, 7.25f, 777.25f, VALID_X | VALID_Z);
  CPU::PGXP::CPU_SRL(R(0, R_V0, R_V1, 16), 0x00640007);
  EXPECT_EQ(Get(R_V1).x, 100.0f);
  EXPECT_FALSE(Get(R_V1).flags & VALID_X);
}

//////////////////////////////////////////////////////////////////////////
// Right shifts of 16 or more
//////////////////////////////////////////////////////////////////////////

TEST_F(PGXPTest, WholeHalfShiftMovesValidity)
{
  Set(R_V0, 0x00640007, 777.25f, 100.25f, VALID_Y | VALID_Z);
  CPU::PGXP::CPU_SRL(R(0, R_V0, R_V1, 16), 0x00640007);
  EXPECT_NEAR(Get(R_V1).x, 100.25, TOLERANCE);
  EXPECT_TRUE(Get(R_V1).flags & VALID_X);
}

TEST_F(PGXPTest, WholeHalfShiftKeepsFractionOfZeroOrMinusOne)
{
  Set(R_V0, 0x00000064, 100.5f, 0.25f);
  CPU::PGXP::CPU_SRA(R(0, R_V0, R_V1, 16), 0x00000064);
  EXPECT_NEAR(Get(R_V1).x, 0.25, TOLERANCE);

  Set(R_V0, 0xFFFFFF9C, -99.5f, -0.75f);
  CPU::PGXP::CPU_SRA(R(0, R_V0, R_V1, 16), 0xFFFFFF9C);
  EXPECT_NEAR(Get(R_V1).x, -0.75, TOLERANCE);

  // The sign of the low half doesn't matter to the extracted upper half.
  Set(R_V0, 0x0000FF9C, -99.5f, 0.25f);
  CPU::PGXP::CPU_SRA(R(0, R_V0, R_V1, 16), 0x0000FF9C);
  EXPECT_NEAR(Get(R_V1).x, 0.25, TOLERANCE);

  // Logical shift uses the representation closest to native 0, not 65535.75.
  Set(R_V0, 0x00000000, 0.0f, -0.25f);
  CPU::PGXP::CPU_SRL(R(0, R_V0, R_V1, 16), 0x00000000);
  EXPECT_NEAR(Get(R_V1).x, -0.25, TOLERANCE);
}

TEST_F(PGXPTest, WholeHalfShiftExtractsFractionalUpperHalf)
{
  Set(R_V0, 0x00010000, 0.0f, 1.25f);
  CPU::PGXP::CPU_SRA(R(0, R_V0, R_V1, 16), 0x00010000);
  EXPECT_NEAR(Get(R_V1).x, 1.25, TOLERANCE);
}

TEST_F(PGXPTest, WholeHalfShiftWithNegativeLowHalf)
{
  // Was 49, the sign-filled low half was added to the shifted upper half.
  Set(R_V0, 0x0064FF9C, -100.0f, 100.0f);
  CPU::PGXP::CPU_SRL(R(0, R_V0, R_V1, 17), 0x0064FF9C);
  EXPECT_EQ(Get(R_V1).value, 50u);
  EXPECT_EQ(Get(R_V1).x, 50.0f);

  Set(R_V0, 0x0064FF9C, -100.0f, 100.5f);
  CPU::PGXP::CPU_SRA(R(0, R_V0, R_V1, 17), 0x0064FF9C);
  EXPECT_NEAR(Get(R_V1).x, 50.25, TOLERANCE);
}

TEST_F(PGXPTest, WholeHalfShiftOfExtremes)
{
  Set(R_V0, 0xFFFFFFFF, -1.0f, -1.0f);
  CPU::PGXP::CPU_SRL(R(0, R_V0, R_V1, 16), 0xFFFFFFFF);
  EXPECT_EQ(Get(R_V1).x, -1.0f);
  EXPECT_EQ(Get(R_V1).y, 0.0f);

  Set(R_V0, 0x80000000, 0.0f, -32768.0f);
  CPU::PGXP::CPU_SRL(R(0, R_V0, R_V1, 31), 0x80000000);
  EXPECT_EQ(Get(R_V1).x, 1.0f);
  EXPECT_EQ(Get(R_V1).y, 0.0f);

  CPU::PGXP::CPU_SRA(R(0, R_V0, R_V1, 31), 0x80000000);
  EXPECT_EQ(Get(R_V1).x, -1.0f);
  EXPECT_EQ(Get(R_V1).y, -1.0f);
}

TEST_F(PGXPTest, WholeHalfShiftSplitsQuantizedValue)
{
  // -0.25 >> 15 truncates to 0 in the low half, so the upper half must be 0 too, not -1 (i.e. -65536).
  for (const bool arithmetic : {false, true})
  {
    for (const u32 dest : {R_V0, R_V1})
    {
      Set(R_V0, 0x00000064, 100.0f, -0.25f);
      if (arithmetic)
        CPU::PGXP::CPU_SRA(R(0, R_V0, dest, 31), 0x00000064);
      else
        CPU::PGXP::CPU_SRL(R(0, R_V0, dest, 31), 0x00000064);
      EXPECT_EQ(Get(dest).value, 0u);
      EXPECT_NEAR(Scalar(Get(dest)), 0.0, TOLERANCE);
    }
  }
}

//////////////////////////////////////////////////////////////////////////
// $zero
//////////////////////////////////////////////////////////////////////////

TEST_F(PGXPTest, ZeroIsPrecise)
{
  // Constants built from $zero are precise, but have no depth.
  CPU::PGXP::CPU_ORI(I(R_ZERO, R_V0, 0x1234), 0);
  EXPECT_EQ(Get(R_V0).flags & VALID_ALL, VALID_XY);
  CPU::PGXP::CPU_XORI(I(R_ZERO, R_V0, 0x1234), 0);
  EXPECT_EQ(Get(R_V0).flags & VALID_ALL, VALID_XY);
  CPU::PGXP::CPU_ADDI(I(R_ZERO, R_V0, 0), 0);
  EXPECT_EQ(Get(R_V0).flags & VALID_ALL, VALID_XY);
  CPU::PGXP::CPU_OR_(R(R_ZERO, R_ZERO, R_V0), 0, 0);
  EXPECT_EQ(Get(R_V0).flags & VALID_ALL, VALID_XY);
}

TEST_F(PGXPTest, ZeroDoesNotMakeNegationPrecise)
{
  // negu/not of an untracked value isn't precise, so bitwise ops still prefer the precise source.
  Set(R_A0, 0xFFFFFF9Cu, -99.75f, -0.75f);

  Set(R_V1, 100, 0.0f, 0.0f, 0);
  CPU::PGXP::CPU_SUB(R(R_ZERO, R_V1, R_V0), 0, 100);
  CPU::PGXP::CPU_AND_(R(R_V0, R_A0, R_A1), 0xFFFFFF9Cu, 0xFFFFFF9Cu);
  EXPECT_NEAR(Get(R_A1).x, -99.75, TOLERANCE);
  EXPECT_NEAR(Get(R_A1).y, -0.75, TOLERANCE);

  Set(R_V1, 0x63, 0.0f, 0.0f, 0);
  CPU::PGXP::CPU_NOR(R(R_V1, R_ZERO, R_V0), 0x63, 0);
  CPU::PGXP::CPU_AND_(R(R_V0, R_A0, R_A1), 0xFFFFFF9Cu, 0xFFFFFF9Cu);
  EXPECT_NEAR(Get(R_A1).x, -99.75, TOLERANCE);
  EXPECT_NEAR(Get(R_A1).y, -0.75, TOLERANCE);
}

TEST_F(PGXPTest, CopiedZeroDoesNotMakeNegationPrecise)
{
  Set(R_A0, 0xFFFFFF9Cu, -99.75f, -0.75f);
  CPU::PGXP::CPU_ORI(I(R_ZERO, R_T2, 0), 0);

  Set(R_V1, 100, 0.0f, 0.0f, 0);
  CPU::PGXP::CPU_SUB(R(R_T2, R_V1, R_V0), 0, 100);
  CPU::PGXP::CPU_AND_(R(R_V0, R_A0, R_A1), 0xFFFFFF9Cu, 0xFFFFFF9Cu);
  EXPECT_NEAR(Get(R_A1).x, -99.75, TOLERANCE);
  EXPECT_NEAR(Get(R_A1).y, -0.75, TOLERANCE);

  Set(R_V1, 0x63, 0.0f, 0.0f, 0);
  CPU::PGXP::CPU_NOR(R(R_V1, R_T2, R_V0), 0x63, 0);
  CPU::PGXP::CPU_AND_(R(R_V0, R_A0, R_A1), 0xFFFFFF9Cu, 0xFFFFFF9Cu);
  EXPECT_NEAR(Get(R_A1).x, -99.75, TOLERANCE);
  EXPECT_NEAR(Get(R_A1).y, -0.75, TOLERANCE);
}

TEST_F(PGXPTest, BitwiseZeroResultIsValid)
{
  // Exactly zero, so stores of it keep the other half precise.
  CPU::PGXP::CPU_XOR_(R(R_ZERO, R_ZERO, R_V0), 0, 0);
  EXPECT_EQ(Get(R_V0).flags & VALID_XY, VALID_XY);

  Set(R_V1, 100, 0.0f, 0.0f, 0);
  CPU::PGXP::CPU_AND_(R(R_ZERO, R_V1, R_V0), 0, 100);
  EXPECT_EQ(Get(R_V0).flags & VALID_XY, VALID_XY);

  // A zero from untracked values isn't known to be exact, so the vertex cache can still look it up.
  Set(R_V1, 0, 0.0f, 0.0f, 0);
  Set(R_A0, 0xFFFFFFFFu, 0.0f, 0.0f, 0);
  CPU::PGXP::CPU_AND_(R(R_V1, R_A0, R_V0), 0, 0xFFFFFFFFu);
  EXPECT_EQ(Get(R_V0).flags & VALID_XY, 0u);

  // NOR with zero is a not, so it's only zero because of the untracked value.
  CPU::Instruction nor = R(R_A0, R_ZERO, R_V0);
  nor.r.funct = CPU::InstructionFunct::nor;
  CPU::PGXP::CPU_NOR(nor, 0xFFFFFFFFu, 0);
  EXPECT_EQ(Get(R_V0).flags & VALID_XY, 0u);
}

TEST_F(PGXPTest, ZeroOperandKeepsNativelyZeroPreciseValue)
{
  // The other operand is natively zero too, but has fractions.
  Set(R_V1, 0, 0.25f, 0.5f);
  CPU::PGXP::CPU_ADD(R(R_ZERO, R_V1, R_V0), 0, 0);
  EXPECT_NEAR(Get(R_V0).x, 0.25, TOLERANCE);
  EXPECT_NEAR(Get(R_V0).y, 0.5, TOLERANCE);

  Set(R_V0, 0, 0.0f, 0.0f, 0);
  CPU::PGXP::CPU_OR_(R(R_ZERO, R_V1, R_V0), 0, 0);
  EXPECT_NEAR(Get(R_V0).x, 0.25, TOLERANCE);
  EXPECT_NEAR(Get(R_V0).y, 0.5, TOLERANCE);

  Set(R_V0, 0, 0.0f, 0.0f, 0);
  CPU::PGXP::CPU_XOR_(R(R_ZERO, R_V1, R_V0), 0, 0);
  EXPECT_NEAR(Get(R_V0).x, 0.25, TOLERANCE);
  EXPECT_NEAR(Get(R_V0).y, 0.5, TOLERANCE);
}

TEST_F(PGXPTest, ZeroOperandKeepsTrackedVertexAtZero)
{
  // Exactly zero, but it has depth, so it isn't a constant.
  Set(R_V1, 0, 0.0f, 0.0f, VALID_ALL, 200.0f);
  CPU::PGXP::CPU_OR_(R(R_V1, R_ZERO, R_V0), 0, 0);
  EXPECT_EQ(Get(R_V0).flags & VALID_Z, VALID_Z);
  EXPECT_EQ(Get(R_V0).z, 200.0f);

  Set(R_V0, 0, 0.0f, 0.0f, 0);
  CPU::PGXP::CPU_XOR_(R(R_V1, R_ZERO, R_V0), 0, 0);
  EXPECT_EQ(Get(R_V0).flags & VALID_Z, VALID_Z);
  EXPECT_EQ(Get(R_V0).z, 200.0f);

  Set(R_A1, 100, 0.0f, 0.0f, 0);
  CPU::PGXP::CPU_SUB(R(R_V1, R_A1, R_V0), 0, 100);
  EXPECT_EQ(Get(R_V0).flags & VALID_ALL, VALID_ALL);
}

TEST_F(PGXPTest, ZeroDivisorDoesNotMakeResultPrecise)
{
  Set(R_V1, 100, 0.0f, 0.0f, 0);
  CPU::PGXP::CPU_DIV(R(R_V1, R_ZERO, 0), 100, 0);
  EXPECT_EQ(CPU::g_state.pgxp_gpr[static_cast<u32>(CPU::Reg::lo)].flags & VALID_XY, 0u);
  EXPECT_EQ(CPU::g_state.pgxp_gpr[static_cast<u32>(CPU::Reg::hi)].flags & VALID_XY, 0u);

  CPU::PGXP::CPU_DIVU(R(R_V1, R_ZERO, 0), 100, 0);
  EXPECT_EQ(CPU::g_state.pgxp_gpr[static_cast<u32>(CPU::Reg::lo)].flags & VALID_XY, 0u);
  EXPECT_EQ(CPU::g_state.pgxp_gpr[static_cast<u32>(CPU::Reg::hi)].flags & VALID_XY, 0u);
}

TEST_F(PGXPTest, ZeroNegationOfNativeZeroIsOnlyPreciseForExactZero)
{
  // Negating a natively zero value, the fractions aren't negated, so only an exact zero gives a precise result.
  Set(R_V1, 0, 0.0f, 0.0f, 0);
  CPU::PGXP::CPU_SUB(R(R_ZERO, R_V1, R_V0), 0, 0);
  EXPECT_EQ(Get(R_V0).flags & VALID_XY, 0u);

  Set(R_V1, 0, 0.0f, 0.25f);
  CPU::PGXP::CPU_SUB(R(R_ZERO, R_V1, R_V0), 0, 0);
  EXPECT_EQ(Get(R_V0).flags & VALID_XY, 0u);

  CPU::PGXP::CPU_SUB(R(R_ZERO, R_ZERO, R_V0), 0, 0);
  EXPECT_EQ(Get(R_V0).flags & VALID_XY, VALID_XY);
}

TEST_F(PGXPTest, ZeroStoreKeepsOtherHalfPrecise)
{
  // A vertex with a native zero Y, written as halves, or cleared and then written.
  static constexpr u32 SCRATCHPAD_ADDR = 0x1F800000;
  CPU::PGXP::Initialize();
  Set(R_V0, 100, 100.25f, 0.0f);

  float x, y, w;
  CPU::PGXP::CPU_SH(I(R_A0, R_V0, 0), SCRATCHPAD_ADDR, 100);
  CPU::PGXP::CPU_SH(I(R_A0, R_ZERO, 0), SCRATCHPAD_ADDR + 2, 0);
  CPU::PGXP::GetPreciseVertex(SCRATCHPAD_ADDR, 100, 100, 0, 0, 0, &x, &y, &w);
  EXPECT_EQ(x, 100.25f);
  EXPECT_EQ(y, 0.0f);

  CPU::PGXP::CPU_SW(I(R_A0, R_ZERO, 0), SCRATCHPAD_ADDR + 4, 0);
  CPU::PGXP::CPU_SH(I(R_A0, R_V0, 0), SCRATCHPAD_ADDR + 4, 100);
  CPU::PGXP::GetPreciseVertex(SCRATCHPAD_ADDR + 4, 100, 100, 0, 0, 0, &x, &y, &w);
  EXPECT_EQ(x, 100.25f);
  EXPECT_EQ(y, 0.0f);

  CPU::PGXP::Shutdown();
}

//////////////////////////////////////////////////////////////////////////
// Bitwise
//////////////////////////////////////////////////////////////////////////

TEST_F(PGXPTest, OrXorWithZeroIsMove)
{
  Set(R_V0, 0x00640000, 0.25f, 100.5f);
  CPU::PGXP::CPU_OR_(R(R_V0, R_ZERO, R_V1), 0x00640000, 0);
  EXPECT_NEAR(Get(R_V1).x, 0.25, TOLERANCE);
  EXPECT_NEAR(Get(R_V1).y, 100.5, TOLERANCE);

  Set(R_V1, 0, 0.0f, 0.0f);
  CPU::PGXP::CPU_XOR_(R(R_ZERO, R_V0, R_V1), 0, 0x00640000);
  EXPECT_NEAR(Get(R_V1).x, 0.25, TOLERANCE);
  EXPECT_NEAR(Get(R_V1).y, 100.5, TOLERANCE);
}

TEST_F(PGXPTest, BitwiseZeroResultIsExact)
{
  Set(R_V0, 0x00640000, 0.25f, 100.5f);
  CPU::PGXP::CPU_XOR_(R(R_V0, R_V0, R_V1), 0x00640000, 0x00640000);
  EXPECT_EQ(Get(R_V1).x, 0.0f);
  EXPECT_EQ(Get(R_V1).y, 0.0f);

  // Natively zero, so XOR with itself isn't treated as XOR with zero.
  Set(R_V0, 0, 0.25f, 0.5f);
  CPU::PGXP::CPU_XOR_(R(R_V0, R_V0, R_V1), 0, 0);
  EXPECT_EQ(Get(R_V1).x, 0.0f);
  EXPECT_EQ(Get(R_V1).y, 0.0f);

  Set(R_V0, 0x00640000, 0.25f, 100.5f);
  Set(R_A0, 0, 0.0f, 0.0f, VALID_XY);
  CPU::PGXP::CPU_AND_(R(R_V0, R_A0, R_V1), 0x00640000, 0);
  EXPECT_EQ(Get(R_V1).x, 0.0f);
  EXPECT_EQ(Get(R_V1).y, 0.0f);
}

TEST_F(PGXPTest, BitwisePrefersPreciseSource)
{
  Set(R_V0, 0x00640064, 0.0f, 0.0f, 0);
  Set(R_A0, 0x00640064, 100.25f, 100.5f);
  CPU::PGXP::CPU_OR_(R(R_V0, R_A0, R_V1), 0x00640064, 0x00640064);
  EXPECT_NEAR(Get(R_V1).x, 100.25, TOLERANCE);
  EXPECT_NEAR(Get(R_V1).y, 100.5, TOLERANCE);
}

TEST_F(PGXPTest, ORIKeepsUnchangedLowHalf)
{
  Set(R_V0, 0x00640064, 100.25f, 100.5f);
  CPU::PGXP::CPU_ORI(I(R_V0, R_V1, 4), 0x00640064);
  EXPECT_NEAR(Get(R_V1).x, 100.25, TOLERANCE);

  CPU::PGXP::CPU_ORI(I(R_V0, R_V1, 1), 0x00640064);
  EXPECT_EQ(Get(R_V1).x, 101.0f);

  CPU::PGXP::CPU_ORI(I(R_V0, R_V1, 0), 0x00640064);
  EXPECT_NEAR(Get(R_V1).x, 100.25, TOLERANCE);
  EXPECT_NEAR(Get(R_V1).y, 100.5, TOLERANCE);
}

TEST_F(PGXPTest, XORIWithZeroKeepsValue)
{
  Set(R_V0, 0x00640064, 100.25f, 100.5f);
  CPU::PGXP::CPU_XORI(I(R_V0, R_V1, 0), 0x00640064);
  EXPECT_NEAR(Get(R_V1).x, 100.25, TOLERANCE);
  EXPECT_NEAR(Get(R_V1).y, 100.5, TOLERANCE);
}

TEST_F(PGXPTest, OrWithNonZeroHalvesUsesUnchangedHalf)
{
  // Upper half comes from the precise source, lower half is changed and low precision.
  Set(R_V0, 0x00640064, 100.25f, 100.5f);
  Set(R_A0, 0x00000001, 1.0f, 0.0f, VALID_XY);
  CPU::PGXP::CPU_OR_(R(R_V0, R_A0, R_V1), 0x00640064, 0x00000001);
  EXPECT_EQ(Get(R_V1).x, 101.0f);
  EXPECT_NEAR(Get(R_V1).y, 100.5, TOLERANCE);
}

TEST_F(PGXPTest, ANDIKeepsUnchangedLowHalf)
{
  Set(R_V0, 0x00640064, 100.25f, 100.5f);
  CPU::PGXP::CPU_ANDI(I(R_V0, R_V1, 0x7FF), 0x00640064);
  EXPECT_NEAR(Get(R_V1).x, 100.25, TOLERANCE);
  EXPECT_EQ(Get(R_V1).y, 0.0f);

  CPU::PGXP::CPU_ANDI(I(R_V0, R_V1, 0xFFFF), 0x00640064);
  EXPECT_NEAR(Get(R_V1).x, 100.25, TOLERANCE);
  EXPECT_EQ(Get(R_V1).y, 0.0f);

  CPU::PGXP::CPU_ANDI(I(R_V0, R_V1, 0), 0x00640064);
  EXPECT_EQ(Get(R_V1).x, 0.0f);
  EXPECT_EQ(Get(R_V1).y, 0.0f);

  Set(R_V0, 0x0064FF9C, -100.25f, 100.5f);
  CPU::PGXP::CPU_ANDI(I(R_V0, R_V1, 0x7FF), 0x0064FF9C);
  EXPECT_EQ(Get(R_V1).x, static_cast<float>(0x79C));
}

//////////////////////////////////////////////////////////////////////////
// ADDI
//////////////////////////////////////////////////////////////////////////

TEST_F(PGXPTest, ADDIUsesDoublePrecision)
{
  // In float, -0.3 became 65535.7 with only 1/256 precision, giving 15.7031.
  Set(R_V0, 0x00640000, -0.3f, 100.2f);
  CPU::PGXP::CPU_ADDI(I(R_V0, R_V1, 16), 0x00640000);
  EXPECT_NEAR(Get(R_V1).x, 15.7, 0.0001);
}

TEST_F(PGXPTest, ADDAveragesDepthOfMidpoint)
{
  // Syphon Filter 3 subdivides polygons in screen space with a + ((b - a) >> 1).
  Set(R_V0, 0xFFFFFFCE, -49.5f, -1.0f, VALID_ALL, 635.0f);
  Set(R_V1, 0xFFFFFFD9, -38.5f, -1.0f, VALID_ALL, 843.0f);
  CPU::PGXP::CPU_SUB(R(R_V1, R_V0, R_A0), 0xFFFFFFD9, 0xFFFFFFCE);
  CPU::PGXP::CPU_SRA(R(0, R_A0, R_A0, 1), 0x0000000B);
  CPU::PGXP::CPU_ADD(R(R_V0, R_A0, R_A1), 0xFFFFFFCE, 0x00000005);
  EXPECT_EQ(Get(R_A1).value, 0xFFFFFFD3u);
  EXPECT_NEAR(Get(R_A1).x, -44.0, TOLERANCE);
  EXPECT_TRUE(Get(R_A1).flags & VALID_Z);
  EXPECT_NEAR(Get(R_A1).z, 739.0, TOLERANCE);

  // A side without depth doesn't change it.
  Set(R_V1, 0x00000004, 4.0f, 0.0f, VALID_XY, 0.0f);
  CPU::PGXP::CPU_ADD(R(R_V0, R_V1, R_A1), 0xFFFFFFCE, 0x00000004);
  EXPECT_EQ(Get(R_A1).z, 635.0f);
}

TEST_F(PGXPTest, ADDIIgnoresInvalidLowHalf)
{
  Set(R_V0, 0x0064FFF0, 777.25f, 100.25f, VALID_Y | VALID_Z);
  CPU::PGXP::CPU_ADDI(I(R_V0, R_V1, 32), 0x0064FFF0);
  EXPECT_EQ(Get(R_V1).value, 0x00650010u);
  EXPECT_NEAR(Get(R_V1).y, 101.25, TOLERANCE);
}

//////////////////////////////////////////////////////////////////////////
// Shift matrices
//////////////////////////////////////////////////////////////////////////

namespace {

enum class ShiftOp
{
  SLL,
  SRL,
  SRA,
};

static constexpr u32 s_shift_matrix_inputs[] = {0x00000000, 0x00000001, 0xFFFFFFFF, 0xFFFF0000, 0x00010000, 0x12345678,
                                                0x80000000, 0x7FFFFFFF, 0x80008000, 0x00008000, 0xFFFF8000, 0x0000FFFF,
                                                0x80007FFF, 0xFFFEEC00, 0xFFF9D1F8, 0x68DB8BAD};

static void DoShift(ShiftOp op, u32 rt, u32 rd, u32 sh, u32 value)
{
  CPU::Instruction i{};
  i.r.rt = static_cast<CPU::Reg>(rt);
  i.r.rd = static_cast<CPU::Reg>(rd);
  i.r.shamt = sh;
  switch (op)
  {
    case ShiftOp::SLL:
      CPU::PGXP::CPU_SLL(i, value);
      break;
    case ShiftOp::SRL:
      CPU::PGXP::CPU_SRL(i, value);
      break;
    case ShiftOp::SRA:
      CPU::PGXP::CPU_SRA(i, value);
      break;
  }
}

} // namespace

TEST_F(PGXPTest, ShiftValidOutputsIgnoreInvalidComponents)
{
  // Whatever is left in an invalid half must not change a valid output.
  for (const ShiftOp op : {ShiftOp::SLL, ShiftOp::SRL, ShiftOp::SRA})
  {
    for (const u32 input : s_shift_matrix_inputs)
    {
      for (u32 sh = 0; sh < 32; sh++)
      {
        for (const u32 valid : {VALID_X | VALID_Z, VALID_Y | VALID_Z})
        {
          const float x = static_cast<float>(static_cast<s16>(input)) + 0.25f;
          const float y = static_cast<float>(static_cast<s16>(input >> 16)) + 0.25f;

          Set(R_V0, input, (valid & VALID_X) ? x : 777.25f, (valid & VALID_Y) ? y : 777.25f, valid);
          DoShift(op, R_V0, R_V1, sh, input);
          const CPU::PGXPValue first = Get(R_V1);

          Set(R_V0, input, (valid & VALID_X) ? x : -12345.5f, (valid & VALID_Y) ? y : -12345.5f, valid);
          DoShift(op, R_V0, R_V1, sh, input);
          const CPU::PGXPValue& second = Get(R_V1);

          SCOPED_TRACE(testing::Message() << static_cast<int>(op) << " " << std::hex << input << std::dec << " by "
                                          << sh << " valid " << valid);
          EXPECT_EQ(first.flags, second.flags);
          if (second.flags & VALID_X)
            EXPECT_EQ(first.x, second.x);
          if (second.flags & VALID_Y)
            EXPECT_EQ(first.y, second.y);
        }
      }
    }
  }
}

TEST_F(PGXPTest, ShiftResultDoesNotDependOnAliasing)
{
  for (const ShiftOp op : {ShiftOp::SLL, ShiftOp::SRL, ShiftOp::SRA})
  {
    for (const u32 input : s_shift_matrix_inputs)
    {
      for (u32 sh = 0; sh < 32; sh++)
      {
        for (const u32 valid : {u32{VALID_ALL}, VALID_X | VALID_Z, VALID_Y | VALID_Z, u32{VALID_XY}})
        {
          const float x = static_cast<float>(static_cast<s16>(input)) + 0.25f;
          const float y = static_cast<float>(static_cast<s16>(input >> 16)) - 0.75f;

          Set(R_V0, input, x, y, valid);
          DoShift(op, R_V0, R_V1, sh, input);
          const CPU::PGXPValue separate = Get(R_V1);

          Set(R_V0, input, x, y, valid);
          DoShift(op, R_V0, R_V0, sh, input);
          const CPU::PGXPValue& aliased = Get(R_V0);

          SCOPED_TRACE(testing::Message() << static_cast<int>(op) << " " << std::hex << input << std::dec << " by "
                                          << sh << " valid " << valid);
          EXPECT_EQ(std::memcmp(&separate, &aliased, sizeof(separate)), 0);
        }
      }
    }
  }
}

//////////////////////////////////////////////////////////////////////////
// MVMVA: matrix/vector precision and native execution
//////////////////////////////////////////////////////////////////////////

class PGXPMVMVATest : public PGXPTest
{
protected:
  void SetUp() override
  {
    PGXPTest::SetUp();
    CPU::g_state.gte_regs = {};
    m_old_enable = g_settings.gpu_pgxp_enable;
    m_old_cpu = g_settings.gpu_pgxp_cpu;
    g_settings.gpu_pgxp_enable = true;
    g_settings.gpu_pgxp_cpu = true;
  }

  void TearDown() override
  {
    g_settings.gpu_pgxp_enable = m_old_enable;
    g_settings.gpu_pgxp_cpu = m_old_cpu;
  }

  static u32 Command(u32 matrix, u32 vector, u32 translation, bool shift = false, bool lm = false)
  {
    return 0x12u | (matrix << 17) | (vector << 15) | (translation << 13) | (u32(shift) << 19) | (u32(lm) << 10);
  }

  static void Shadow(u32 index, float x, float y, u32 flags = VALID_XY)
  {
    CPU::g_state.pgxp_gte[index] = {x, y, 500.0f, CPU::g_state.gte_regs.r32[index], flags};
  }

  bool m_old_enable;
  bool m_old_cpu;
};

TEST_F(PGXPMVMVATest, ApeEscapeSubdivisionRetainsFractionsThroughCPUDivide)
{
  auto& regs = CPU::g_state.gte_regs;
  regs.dr32[9] = 676;
  regs.dr32[10] = 484;

  // The game packs the X and Y of both projected endpoints with OR, and loads them into the light matrix with CTC2.
  const auto pack = [](u32 llm_index, s16 c0, float p0, s16 c1, float p1) {
    const u32 lo = static_cast<u16>(c0);
    const u32 hi = static_cast<u32>(static_cast<u16>(c1)) << 16;
    Set(R_A0, lo, p0, 0.0f, VALID_ALL, 676.0f);
    Set(R_A1, hi, 0.0f, p1, VALID_ALL, 484.0f);
    CPU_OR_(R(R_A0, R_A1, R_V0), lo, hi);
    if constexpr (CPU::PGXP::BITWISE_AVERAGE_PACKED_DEPTH)
      EXPECT_FLOAT_EQ(Get(R_V0).z, 580.0f);

    const u32 control_reg = llm_index - 32;
    CPU::Instruction ctc{0x48C00000u | (R_V0 << 16) | (control_reg << 11)};
    GTE::WriteRegister(llm_index, lo | hi);
    CPU_MTC2(ctc, lo | hi);
  };
  pack(40, 101, 101.75f, 387, 387.85f);
  pack(43, 231, 231.6f, 290, 290.1f);

  GTE::ExecuteInstruction(Command(1, 3, 3, false, true));
  EXPECT_EQ(regs.MAC1, 255584);
  EXPECT_EQ(regs.MAC3, 296516);
  const auto& mac = CPU::g_state.pgxp_gte[25];
  EXPECT_NEAR(Scalar(mac), 101.75 * 676 + 387.85 * 484, 0.02);
  EXPECT_EQ(mac.flags & VALID_XYZ, VALID_XYZ);

  // Midpoint depth of the camera space edge.
  if constexpr (CPU::PGXP::BITWISE_AVERAGE_PACKED_DEPTH)
    EXPECT_FLOAT_EQ(mac.z, 580.0f);

  // MFC2 followed by ADD, DIV, MFLO preserves precision as used by the game.
  CPU::Instruction mfc{};
  mfc.r.rt = static_cast<CPU::Reg>(R_V0);
  mfc.r.rd = static_cast<CPU::Reg>(25);
  CPU_MFC2(mfc, regs.dr32[25]);
  CPU_ADD(R(R_V0, R_A0, R_V0), regs.dr32[25], 1160);
  CPU_DIV(R(R_V0, R_A1, R_V1), regs.dr32[25] + 1160, 1160);
  const auto& lo = CPU::g_state.pgxp_gpr[static_cast<u32>(CPU::Reg::lo)];
  EXPECT_NEAR(lo.x, (101.75 * 676 + 387.85 * 484) / 1160 + 1, 0.001);
  EXPECT_TRUE(lo.flags & VALID_Z);
  EXPECT_FLOAT_EQ(lo.z, mac.z);
}

TEST_F(PGXPMVMVATest, MatrixComponentsWithoutDepthGiveNoDepth)
{
  auto& regs = CPU::g_state.gte_regs;
  regs.LLM[0][0] = 10;
  regs.LLM[0][1] = 20;
  regs.dr32[9] = 1;
  regs.dr32[10] = 1;
  Shadow(40, 10.5f, 20.5f, VALID_XY);
  GTE::ExecuteInstruction(Command(1, 3, 3));
  EXPECT_EQ(CPU::g_state.pgxp_gte[25].flags & VALID_XYZ, VALID_XY);
}

TEST_F(PGXPMVMVATest, AllStandardSelectorsKeepNativeRegistersAndFlags)
{
  for (u32 matrix = 0; matrix < 4; matrix++)
  {
    for (u32 vector = 0; vector < 4; vector++)
    {
      for (u32 translation = 0; translation < 4; translation++)
      {
        for (bool shift : {false, true})
        {
          for (bool lm : {false, true})
          {
            for (bool compiled : {false, true})
            {
              SCOPED_TRACE(testing::Message() << matrix << ',' << vector << ',' << translation << ',' << shift << ','
                                              << lm << ',' << compiled);
              auto& regs = CPU::g_state.gte_regs;
              regs = {};
              for (u32 i = 0; i < 63; i++)
              {
                const s16 lo = static_cast<s16>(int(i) * 13 - 300);
                const s16 hi = static_cast<s16>(int(i) * 7 - 200);
                regs.r32[i] = u32(u16(lo)) | (u32(u16(hi)) << 16);
                Shadow(i, lo + 0.25f, hi - 0.5f);
              }
              for (u32 i = 9; i < 12; i++)
              {
                regs.dr32[i] = static_cast<u32>(static_cast<s32>(int(i) - 10));
                Shadow(i, int(i) - 9.75f, int(i) < 10 ? -1.0f : 0.0f);
              }
              const auto initial = regs;
              const u32 command = Command(matrix, vector, translation, shift, lm);
              g_settings.gpu_pgxp_cpu = false;
              GTE::ExecuteInstruction(command);
              const auto expected = regs;
              regs = initial;
              g_settings.gpu_pgxp_cpu = true;
              if (compiled)
              {
                TickCount ticks;
                GTE::GetInstructionImpl(command, &ticks)(GTE::Instruction{command});
              }
              else
              {
                GTE::ExecuteInstruction(command);
              }
              EXPECT_EQ(std::memcmp(&regs, &expected, sizeof(regs)), 0);
            }
          }
        }
      }
    }
  }
}

TEST_F(PGXPMVMVATest, IRInputsAreSnapshottedBeforeOutputsOverwriteThem)
{
  auto& regs = CPU::g_state.gte_regs;
  for (u32 row = 0; row < 3; row++)
    regs.LLM[row][0] = 1;
  regs.dr32[9] = 4;
  Shadow(9, 4.5f, 0.0f);
  GTE::ExecuteInstruction(Command(1, 3, 3));
  for (u32 row = 0; row < 3; row++)
    EXPECT_FLOAT_EQ(CPU::g_state.pgxp_gte[25 + row].x, 4.5f);
}

TEST_F(PGXPMVMVATest, PackedVectorAndPartialMatrixValidity)
{
  auto& regs = CPU::g_state.gte_regs;
  regs.V1[1] = 3;
  regs.LCM[0][1] = 2;
  Shadow(2, 900.0f, 3.5f, VALID_Y);
  Shadow(48, 900.0f, 2.25f, VALID_Y);
  GTE::ExecuteInstruction(Command(2, 1, 3));
  EXPECT_EQ(regs.MAC1, 6);
  EXPECT_FLOAT_EQ(CPU::g_state.pgxp_gte[25].x, 7.875f);
}

TEST_F(PGXPMVMVATest, TranslationShiftAndSignedResult)
{
  auto& regs = CPU::g_state.gte_regs;
  regs.BK[0] = -2;
  Shadow(45, -1.5f, -1.0f);
  GTE::ExecuteInstruction(Command(1, 0, 1, true));
  EXPECT_EQ(regs.MAC1, -2);
  EXPECT_DOUBLE_EQ(Scalar(CPU::g_state.pgxp_gte[25]), -1.5);
}

TEST_F(PGXPMVMVATest, FarColorBugDiscardsTranslationAndFirstProduct)
{
  auto& regs = CPU::g_state.gte_regs;
  regs.FC[0] = 100;
  regs.LLM[0][0] = 10;
  regs.LLM[0][1] = 2;
  regs.LLM[0][2] = 3;
  regs.V0[0] = 5;
  regs.V0[1] = 6;
  regs.V0[2] = 7;
  Shadow(0, 5.5f, 6.25f);
  Shadow(1, 7.5f, 0.0f);
  GTE::ExecuteInstruction(Command(1, 0, 2));
  EXPECT_EQ(regs.MAC1, 33);
  EXPECT_FLOAT_EQ(CPU::g_state.pgxp_gte[25].x, 35.0f);
}

TEST_F(PGXPMVMVATest, SaturatedIRDoesNotClampPreciseMAC)
{
  auto& regs = CPU::g_state.gte_regs;
  regs.LLM[0][0] = 100;
  regs.V0[0] = -400;
  Shadow(0, -400.25f, 0.0f);
  GTE::ExecuteInstruction(Command(1, 0, 3, false, true));
  EXPECT_EQ(regs.MAC1, -40000);
  EXPECT_EQ(regs.IR1, 0);
  EXPECT_DOUBLE_EQ(Scalar(CPU::g_state.pgxp_gte[25]), -40025.0);
  EXPECT_EQ(CPU::g_state.pgxp_gte[9].x, 0.0f);
  EXPECT_EQ(CPU::g_state.pgxp_gte[9].y, 0.0f);
}

TEST_F(PGXPMVMVATest, StaleInputsAndUnsupportedOrOverflowingResultsInvalidateOldPrecision)
{
  auto& regs = CPU::g_state.gte_regs;
  for (u32 kind = 0; kind < 5; kind++)
  {
    regs = {};
    CPU::PGXP::Reset();
    regs.RT[0][0] = 1;
    regs.V0[0] = 1;
    Shadow(0, 1.25f, 0.0f);
    if (kind == 0)
      CPU::g_state.pgxp_gte[0].value = 99;
    else if (kind == 2)
    {
      regs.TR[0] = 0x7FFFFFFF;
      regs.RT[0][0] = 32767;
      regs.V0[0] = 32767;
      Shadow(0, 32767.25f, 0.0f);
    }
    else if (kind == 3)
    {
      regs.TR[0] = 0x100000;
      Shadow(37, 0.0f, 16.0f);
    }
    else if (kind == 4)
    {
      // Native MAC wraps at 32 bits even though the shadow result is just inside the signed range.
      regs.TR[0] = 262160;
      regs.RT[0][0] = 32767;
      regs.V0[0] = 32767;
      Shadow(0, 32765.5f, 0.0f);
    }
    // For the stale-input case, the old shadow's native value already equals the new result.
    regs.dr32[25] = 1;
    regs.dr32[9] = 1;
    Shadow(25, 999.5f, 0.0f, VALID_ALL);
    Shadow(9, 999.5f, 0.0f, VALID_ALL);
    GTE::ExecuteInstruction(Command(kind == 1 ? 3 : 0, 0, kind >= 2 ? 0 : 3));
    EXPECT_EQ(CPU::g_state.pgxp_gte[25].flags, 0u);
    EXPECT_EQ(CPU::g_state.pgxp_gte[9].flags, 0u);
  }
}

TEST_F(PGXPMVMVATest, StandardSelectorsPropagateVectorFractions)
{
  for (u32 matrix = 1; matrix < 3; matrix++)
  {
    for (u32 vector = 0; vector < 4; vector++)
    {
      for (u32 translation = 0; translation < 4; translation++)
      {
        for (bool shift : {false, true})
        {
          CPU::PGXP::Reset();
          auto& regs = CPU::g_state.gte_regs;
          regs = {};
          const u32 matrix_base = 32 + matrix * 8;
          for (u32 row = 0; row < 3; row++)
          {
            for (u32 col = 0; col < 3; col++)
            {
              const u32 offset = row * 3 + col;
              regs.r32[matrix_base + offset / 2] |= (row + col + 1) << ((offset & 1) * 16);
            }
            regs.TR[row] = 2;
            regs.BK[row] = 3;
          }
          if (vector == 3)
          {
            for (u32 i = 0; i < 3; i++)
            {
              regs.dr32[9 + i] = 2 * (i + 1);
              Shadow(9 + i, 2 * (i + 1) + 0.5f, 0.0f);
            }
          }
          else
          {
            regs.dr32[vector * 2] = 2 | (4 << 16);
            regs.dr32[vector * 2 + 1] = 6;
            Shadow(vector * 2, 2.5f, 4.5f);
            Shadow(vector * 2 + 1, 6.5f, 0.0f);
          }
          GTE::ExecuteInstruction(Command(matrix, vector, translation, shift));
          for (u32 row = 0; row < 3; row++)
          {
            double expected = translation < 2 ? (translation + 2) * 4096.0 : 0.0;
            for (u32 col = translation == 2 ? 1 : 0; col < 3; col++)
              expected += (row + col + 1) * (2 * (col + 1) + 0.5);
            if (shift)
              expected /= 4096.0;
            const auto& mac = CPU::g_state.pgxp_gte[25 + row];
            EXPECT_EQ(mac.flags & VALID_XY, VALID_XY);
            EXPECT_NEAR(Scalar(mac), expected, 0.001);
          }
        }
      }
    }
  }
}

TEST_F(PGXPMVMVATest, RotationMatrixResultsStayNative)
{
  // Games depend on the truncated results of libgte's ApplyMatrix(), RotTrans(), etc.
  auto& regs = CPU::g_state.gte_regs;
  regs.RT[0][0] = 2;
  regs.TR[0] = 5;
  regs.V0[0] = 3;
  Shadow(32, 2.5f, 0.0f);
  Shadow(37, 5.5f, 0.0f);
  Shadow(0, 3.25f, 0.0f);
  Shadow(9, 1.5f, 0.0f);
  Shadow(25, 1.5f, 0.0f);
  GTE::ExecuteInstruction(Command(0, 0, 0, true));
  EXPECT_EQ(regs.MAC1, 5);
  EXPECT_EQ(CPU::g_state.pgxp_gte[25].flags, 0u);
  EXPECT_EQ(CPU::g_state.pgxp_gte[9].flags, 0u);
}

TEST_F(PGXPMVMVATest, TranslationUsesNativeValueLikeRTPS)
{
  auto& regs = CPU::g_state.gte_regs;
  regs.LLM[0][0] = 2;
  regs.TR[0] = 5;
  regs.V0[0] = 3;
  Shadow(40, 2.5f, 0.0f);
  Shadow(37, 5.5f, 0.0f);
  Shadow(0, 3.25f, 0.0f);
  GTE::ExecuteInstruction(Command(1, 0, 0, true));
  EXPECT_EQ(regs.MAC1, 5);
  EXPECT_NEAR(Scalar(CPU::g_state.pgxp_gte[25]), 5.0 + 8.125 / 4096.0, 1e-6);
}

TEST_F(PGXPMVMVATest, AccumulatedPrecisionIsKept)
{
  // 10.75 scaled by four is 43 against a native 40, which an identity transform must keep.
  auto& regs = CPU::g_state.gte_regs;
  regs.LLM[0][0] = 4;
  regs.V0[0] = 10;
  Shadow(40, 4.0f, 0.0f);
  Shadow(0, 10.75f, 0.0f);
  GTE::ExecuteInstruction(Command(1, 0, 3));
  EXPECT_EQ(regs.MAC1, 40);
  EXPECT_NEAR(Scalar(CPU::g_state.pgxp_gte[25]), 43.0, 0.001);

  regs.LLM[0][0] = 1;
  Shadow(40, 1.0f, 0.0f);
  GTE::ExecuteInstruction(Command(1, 3, 3));
  EXPECT_EQ(regs.MAC1, 40);
  EXPECT_NEAR(Scalar(CPU::g_state.pgxp_gte[25]), 43.0, 0.001);
}

TEST_F(PGXPMVMVATest, SelectingOneHalfOfPackedWordGivesNoDepth)
{
  // The word's depth is the average of both halves, which doesn't belong to either one alone.
  auto& regs = CPU::g_state.gte_regs;
  regs.LLM[0][0] = 101;
  regs.LLM[0][1] = 387;
  regs.IR1 = 1;
  CPU::g_state.pgxp_gte[40] = {101.75f, 387.85f, 200.0f, regs.r32[40], VALID_ALL};
  GTE::ExecuteInstruction(Command(1, 3, 3));
  EXPECT_EQ(regs.MAC1, 101);
  const auto& mac = CPU::g_state.pgxp_gte[25];
  EXPECT_NEAR(Scalar(mac), 101.75, 0.001);
  EXPECT_FALSE(mac.flags & VALID_Z);

  regs.IR1 = 1;
  regs.IR2 = 1;
  GTE::ExecuteInstruction(Command(1, 3, 3));
  EXPECT_NEAR(Scalar(mac), 101.75 + 387.85, 0.01);
  EXPECT_TRUE(mac.flags & VALID_Z);
  EXPECT_FLOAT_EQ(mac.z, 200.0f);
}

TEST_F(PGXPMVMVATest, PreciseIRIsClampedToSaturationRange)
{
  auto& regs = CPU::g_state.gte_regs;
  regs.LLM[0][0] = 1;
  Shadow(40, 1.0f, 0.0f);
  Shadow(0, -0.5f, 0.0f);
  GTE::ExecuteInstruction(Command(1, 0, 3, false, true));
  EXPECT_EQ(regs.IR1, 0);
  EXPECT_NEAR(Scalar(CPU::g_state.pgxp_gte[25]), -0.5, 0.001);
  EXPECT_EQ(CPU::g_state.pgxp_gte[9].x, 0.0f);
  EXPECT_EQ(CPU::g_state.pgxp_gte[9].y, 0.0f);
}

TEST_F(PGXPMVMVATest, OtherInstructionsInvalidatePreciseMACAndIR)
{
  auto& regs = CPU::g_state.gte_regs;
  for (bool compiled : {false, true})
  {
    for (u32 command : {0x28u, 0x2Du})
    {
      SCOPED_TRACE(testing::Message() << compiled << ',' << command);
      regs = {};
      // SQR of IR=1 produces the same native MAC and IR as before.
      for (u32 i = 0; i < 3; i++)
      {
        regs.dr32[9 + i] = 1;
        regs.dr32[25 + i] = 1;
        Shadow(9 + i, 1.25f, 0.0f);
        Shadow(25 + i, 1.25f, 0.0f);
      }
      if (compiled)
      {
        TickCount ticks;
        GTE::GetInstructionImpl(command, &ticks)(GTE::Instruction{command});
      }
      else
      {
        GTE::ExecuteInstruction(command);
      }

      // AVSZ3 only writes MAC0/OTZ, so it keeps them.
      const u32 expected = (command == 0x2Du) ? VALID_XY : 0u;
      for (u32 i = 0; i < 3; i++)
      {
        EXPECT_EQ(CPU::g_state.pgxp_gte[9 + i].flags, expected);
        EXPECT_EQ(CPU::g_state.pgxp_gte[25 + i].flags, expected);
      }
    }
  }
}

TEST_F(PGXPTest, ORPackingHalvesOfTwoValuesAveragesDepth)
{
  Set(R_A0, 0x00000065, 101.75f, 0.0f, VALID_ALL, 600.0f);
  Set(R_A1, 0x01830000, 0.0f, 387.85f, VALID_ALL, 400.0f);
  CPU_OR_(R(R_A0, R_A1, R_V0), 0x00000065, 0x01830000);
  EXPECT_FLOAT_EQ(Get(R_V0).x, 101.75f);
  EXPECT_FLOAT_EQ(Get(R_V0).y, 387.85f);
  if constexpr (CPU::PGXP::BITWISE_AVERAGE_PACKED_DEPTH)
    EXPECT_FLOAT_EQ(Get(R_V0).z, 500.0f);

  // Both halves from one side keep that side's depth.
  Set(R_A0, 0x01830065, 101.75f, 387.85f, VALID_ALL, 600.0f);
  Set(R_A1, 0x00010001, 1.0f, 1.0f, VALID_ALL, 400.0f);
  CPU_OR_(R(R_A0, R_A1, R_V0), 0x01830065, 0x00010001);
  EXPECT_FLOAT_EQ(Get(R_V0).z, 600.0f);

  // One side without depth keeps the depth of the other.
  Set(R_A0, 0x00000065, 101.75f, 0.0f, VALID_XY, 0.0f);
  Set(R_A1, 0x01830000, 0.0f, 387.85f, VALID_ALL, 400.0f);
  CPU_OR_(R(R_A0, R_A1, R_V0), 0x00000065, 0x01830000);
  EXPECT_FLOAT_EQ(Get(R_V0).z, 400.0f);
}
