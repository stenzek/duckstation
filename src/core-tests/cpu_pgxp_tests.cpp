// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"
#include "core/cpu_pgxp.h"

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
