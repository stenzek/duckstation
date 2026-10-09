// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"
#include "core/gte.h"
#include "core/gte_types.h"
#include "core/types.h"

#include "common/types.h"
#include "common/xorshift_prng.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace {

// Register indices, control registers are offset by 32.
enum : u32
{
  REG_VXY0 = 0,
  REG_VZ0 = 1,
  REG_RGBC = 6,
  REG_IR0 = 8,
  REG_IR1 = 9,
  REG_IR2 = 10,
  REG_IR3 = 11,
  REG_SXY0 = 12,
  REG_SXY1 = 13,
  REG_SXY2 = 14,
  REG_RGB2 = 22,
  REG_MAC0 = 24,
  REG_MAC1 = 25,
  REG_MAC2 = 26,
  REG_MAC3 = 27,
  REG_IRGB = 28,
  REG_ORGB = 29,
  REG_RT11RT12 = 32,
  REG_TRX = 37,
  REG_FLAG = 63,
};

enum : u32
{
  CMD_RTPS = 0x01,
  CMD_NCLIP = 0x06,
  CMD_MVMVA = 0x12,
  CMD_RTPT = 0x30,
  CMD_GPF = 0x3D,

  INST_SF = (1u << 19),
  INST_LM = (1u << 10),
};

using RegisterState = std::array<u32, GTE::NUM_REGS>;

using Random = XorShift128PlusPlus;

} // namespace

static u32 Next32(Random& rng)
{
  return static_cast<u32>(rng.Next() >> 32);
}

static u32 NextIndex(Random& rng, u32 count)
{
  return static_cast<u32>(rng.NextRange(static_cast<u64>(count)));
}

static u16 NextEdge16(Random& rng)
{
  static constexpr std::array<u16, 12> values = {
    {0x0000, 0x0001, 0xFFFF, 0x7FFF, 0x8000, 0x8001, 0x1000, 0xF000, 0x00FF, 0x0100, 0x4000, 0xC000}};
  return values[NextIndex(rng, static_cast<u32>(values.size()))];
}

static u32 NextEdge32(Random& rng)
{
  static constexpr std::array<u32, 10> values = {{0x00000000u, 0x00000001u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u,
                                                  0x80000001u, 0x0000FFFFu, 0x00010000u, 0x7FFF0000u, 0xFFFF8000u}};
  return values[NextIndex(rng, static_cast<u32>(values.size()))];
}

static u16 NextSmall16(Random& rng)
{
  // 4.12 fixed point values around +/-1.0, as used for most matrices/vectors.
  return static_cast<u16>(static_cast<s32>(NextIndex(rng, 0x2001)) - 0x1000);
}

static u32 NextRegisterValue(Random& rng, bool typical_values)
{
  switch (NextIndex(rng, typical_values ? 2 : 5))
  {
    case 0:
      return ZeroExtend32(NextSmall16(rng)) | (ZeroExtend32(NextSmall16(rng)) << 16);

    case 1:
    {
      // sign-extended value of a random width
      const u32 shift = 32 - (1 + NextIndex(rng, 24));
      return static_cast<u32>(static_cast<s32>(Next32(rng) << shift) >> shift);
    }

    case 2:
      return NextEdge32(rng);

    case 3:
      return ZeroExtend32(NextEdge16(rng)) | (ZeroExtend32(NextEdge16(rng)) << 16);

    default:
      return Next32(rng);
  }
}

static RegisterState NextRegisterState(Random& rng)
{
  // Half of the states use values which mostly stay in range, the other half hammer the overflow/saturation paths.
  const bool typical_values = (NextIndex(rng, 2) == 0);

  RegisterState state;
  for (u32& value : state)
    value = NextRegisterValue(rng, typical_values);

  return state;
}

static void LoadRegisterState(const RegisterState& state)
{
  GTE::Reset();
  for (u32 i = 0; i < GTE::NUM_REGS; i++)
    GTE::WriteRegister(i, state[i]);
}

static RegisterState SaveRegisterState()
{
  RegisterState state;
  std::memcpy(state.data(), CPU::g_state.gte_regs.r32, sizeof(state));
  return state;
}

static u64 HashValue(u64 hash, u32 value)
{
  // FNV-1a
  for (u32 i = 0; i < 4; i++)
  {
    hash ^= (value >> (i * 8)) & 0xFFu;
    hash *= UINT64_C(0x100000001B3);
  }
  return hash;
}

/// Executes random instructions of the specified command on random register states, through both the interpreter and
/// recompiler entry points, and returns a hash of the resulting registers.
static u64 RunRandomInstructions(u32 command, u32 count, u64 seed)
{
  Random rng(seed);
  u64 hash = UINT64_C(0);

  for (u32 i = 0; i < count; i++)
  {
    const RegisterState input = NextRegisterState(rng);

    // cop2 imm25 with random sf/lm/mvmva fields, including the unused bits.
    const u32 inst = UINT32_C(0x4A000000) | ((Next32(rng) & UINT32_C(0x01FFFFC0)) | command);

    LoadRegisterState(input);
    GTE::ExecuteInstruction(inst);
    const RegisterState interpreter_output = SaveRegisterState();
    const u32 interpreter_orgb = GTE::ReadRegister(REG_ORGB);

    LoadRegisterState(input);
    TickCount ticks = 0;
    const GTE::InstructionImpl impl = GTE::GetInstructionImpl(inst, &ticks);
    impl(GTE::Instruction{inst & GTE::Instruction::REQUIRED_BITS_MASK});
    const RegisterState recompiler_output = SaveRegisterState();

    if (interpreter_output != recompiler_output)
    {
      ADD_FAILURE() << "Interpreter/recompiler mismatch for instruction 0x" << std::hex << inst << " at iteration "
                    << std::dec << i;
      return 0;
    }

    for (const u32 value : interpreter_output)
      hash = HashValue(hash, value);
    hash = HashValue(hash, interpreter_orgb);
  }

  return hash;
}

static u32 PackXY(s16 x, s16 y)
{
  return ZeroExtend32(static_cast<u16>(x)) | (ZeroExtend32(static_cast<u16>(y)) << 16);
}

TEST(GTE, GPFSaturatesIRAndColor)
{
  const auto run = [](bool lm) {
    GTE::Reset();
    GTE::WriteRegister(REG_RGBC, 0xAB000000u);
    GTE::WriteRegister(REG_IR0, 0x2000);
    GTE::WriteRegister(REG_IR1, static_cast<u32>(-5));
    GTE::WriteRegister(REG_IR2, 0x7FFF);
    GTE::WriteRegister(REG_IR3, 100);
    GTE::ExecuteInstruction(UINT32_C(0x4A000000) | INST_SF | (lm ? INST_LM : 0u) | CMD_GPF);
  };

  // MAC = (IR * IR0) >> 12
  run(false);
  EXPECT_EQ(GTE::ReadRegister(REG_MAC1), static_cast<u32>(-10));
  EXPECT_EQ(GTE::ReadRegister(REG_MAC2), 0xFFFEu);
  EXPECT_EQ(GTE::ReadRegister(REG_MAC3), 200u);
  EXPECT_EQ(GTE::ReadRegister(REG_IR1), static_cast<u32>(-10));
  EXPECT_EQ(GTE::ReadRegister(REG_IR2), 0x7FFFu);
  EXPECT_EQ(GTE::ReadRegister(REG_IR3), 200u);

  // R underflows, G overflows, B = 200 >> 4.
  EXPECT_EQ(GTE::ReadRegister(REG_RGB2), 0xAB0CFF00u);

  // IR2 saturated (also sets the error bit), R/G saturated.
  EXPECT_EQ(GTE::ReadRegister(REG_FLAG), 0x80B00000u);

  // With lm set, negative values are clamped to zero.
  run(true);
  EXPECT_EQ(GTE::ReadRegister(REG_MAC1), static_cast<u32>(-10));
  EXPECT_EQ(GTE::ReadRegister(REG_IR1), 0u);
  EXPECT_EQ(GTE::ReadRegister(REG_IR2), 0x7FFFu);
  EXPECT_EQ(GTE::ReadRegister(REG_IR3), 200u);
  EXPECT_EQ(GTE::ReadRegister(REG_RGB2), 0xAB0CFF00u);
  EXPECT_EQ(GTE::ReadRegister(REG_FLAG), 0x81B00000u);
}

TEST(GTE, MVMVAOverflowWrapsTo44Bits)
{
  // MAC1 = (TRX * 0x1000 + RT11 * VX0) >> 12, where the intermediate sum overflows 44 bits and wraps around.
  GTE::Reset();
  GTE::WriteRegister(REG_TRX, 0x7FFFFFFFu);
  GTE::WriteRegister(REG_RT11RT12, 0x7FFFu);
  GTE::WriteRegister(REG_VXY0, 0x7FFFu);
  GTE::ExecuteInstruction(UINT32_C(0x4A000000) | INST_SF | CMD_MVMVA);

  const s64 sum = (INT64_C(0x7FFFFFFF) << 12) + (INT64_C(0x7FFF) * INT64_C(0x7FFF));
  ASSERT_GT(sum, (INT64_C(1) << 43) - 1);
  const s64 wrapped = sum - (INT64_C(1) << 44);

  EXPECT_EQ(GTE::ReadRegister(REG_MAC1), static_cast<u32>(wrapped >> 12));
  EXPECT_EQ(GTE::ReadRegister(REG_MAC2), 0u);
  EXPECT_EQ(GTE::ReadRegister(REG_MAC3), 0u);
  EXPECT_EQ(GTE::ReadRegister(REG_IR1), static_cast<u32>(-0x8000));
  EXPECT_EQ(GTE::ReadRegister(REG_IR2), 0u);
  EXPECT_EQ(GTE::ReadRegister(REG_IR3), 0u);

  // MAC1 positive overflow, IR1 saturated, error.
  EXPECT_EQ(GTE::ReadRegister(REG_FLAG), 0xC1000000u);
}

TEST(GTE, NCLIPMatchesReference)
{
  Random rng(0x4E434C4950ull);

  for (u32 i = 0; i < 100000; i++)
  {
    std::array<s16, 6> v;
    for (s16& value : v)
      value = static_cast<s16>((NextIndex(rng, 4) == 0) ? NextEdge16(rng) : static_cast<u16>(Next32(rng)));

    GTE::Reset();
    GTE::WriteRegister(REG_SXY0, PackXY(v[0], v[1]));
    GTE::WriteRegister(REG_SXY1, PackXY(v[2], v[3]));
    GTE::WriteRegister(REG_SXY2, PackXY(v[4], v[5]));
    GTE::ExecuteInstruction(UINT32_C(0x4A000000) | CMD_NCLIP);

    // MAC0 = SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1
    const s64 expected = s64(v[0]) * s64(v[3]) + s64(v[2]) * s64(v[5]) + s64(v[4]) * s64(v[1]) - s64(v[0]) * s64(v[5]) -
                         s64(v[2]) * s64(v[1]) - s64(v[4]) * s64(v[3]);
    const u32 expected_flag =
      (expected > INT64_C(0x7FFFFFFF)) ? 0x80010000u : ((expected < -INT64_C(0x80000000)) ? 0x80008000u : 0u);

    ASSERT_EQ(GTE::ReadRegister(REG_MAC0), static_cast<u32>(expected)) << "iteration " << i;
    ASSERT_EQ(GTE::ReadRegister(REG_FLAG), expected_flag) << "iteration " << i;
  }
}

TEST(GTE, NCLIPOverflow)
{
  // (-32768 * -32768) * 2 == 0x80000000, one past the largest positive MAC0.
  GTE::Reset();
  GTE::WriteRegister(REG_SXY0, PackXY(-32768, 0));
  GTE::WriteRegister(REG_SXY1, PackXY(0, -32768));
  GTE::WriteRegister(REG_SXY2, PackXY(-32768, 32767));
  GTE::ExecuteInstruction(UINT32_C(0x4A000000) | CMD_NCLIP);

  const s64 expected = s64(-32768) * s64(-32768) + s64(-32768) * s64(0) - s64(-32768) * s64(32767) -
                       s64(-32768) * s64(-32768) + s64(0) * s64(32767) - s64(0) * s64(0);
  EXPECT_EQ(GTE::ReadRegister(REG_MAC0), static_cast<u32>(expected));
  EXPECT_EQ(GTE::ReadRegister(REG_FLAG), 0u);
}

TEST(GTE, ORGBMatchesReference)
{
  GTE::Reset();

  for (s32 ir = -0x8000; ir <= 0x7FFF; ir++)
  {
    GTE::WriteRegister(REG_IR1, static_cast<u32>(ir));
    GTE::WriteRegister(REG_IR2, static_cast<u32>(-ir));
    GTE::WriteRegister(REG_IR3, static_cast<u32>(ir ^ 0x5555));

    const s32 ir1 = static_cast<s16>(ir);
    const s32 ir2 = static_cast<s16>(-ir);
    const s32 ir3 = static_cast<s16>(ir ^ 0x5555);
    const u32 expected = static_cast<u32>(std::clamp(ir1 / 0x80, 0x00, 0x1F)) |
                         (static_cast<u32>(std::clamp(ir2 / 0x80, 0x00, 0x1F)) << 5) |
                         (static_cast<u32>(std::clamp(ir3 / 0x80, 0x00, 0x1F)) << 10);

    ASSERT_EQ(GTE::ReadRegister(REG_ORGB), expected) << "IR " << ir;
    ASSERT_EQ(GTE::ReadRegister(REG_IRGB), expected) << "IR " << ir;
  }
}

TEST(GTE, FlagWriteUpdatesErrorBit)
{
  GTE::Reset();

  // Low 12 bits and the error bit are not writable, error is computed from bits 30..23 and 18..13.
  GTE::WriteRegister(REG_FLAG, 0xFFFFFFFFu);
  EXPECT_EQ(GTE::ReadRegister(REG_FLAG), 0xFFFFF000u);
  GTE::WriteRegister(REG_FLAG, 0x80000000u);
  EXPECT_EQ(GTE::ReadRegister(REG_FLAG), 0u);
  GTE::WriteRegister(REG_FLAG, 0x00781000u);
  EXPECT_EQ(GTE::ReadRegister(REG_FLAG), 0x00781000u);
  GTE::WriteRegister(REG_FLAG, 0x00002000u);
  EXPECT_EQ(GTE::ReadRegister(REG_FLAG), 0x80002000u);
  GTE::WriteRegister(REG_FLAG, 0x00800000u);
  EXPECT_EQ(GTE::ReadRegister(REG_FLAG), 0x80800000u);
}

// The hashes below were generated from the reference implementation, and cover every data/control register, including
// FLAG. Any change to the GTE which alters them is visible to the guest.
TEST(GTE, RandomInstructionsMatchReference)
{
  struct Expected
  {
    u32 command;
    const char* name;
    u64 hash;
  };
  static constexpr Expected expected[] = {
    {0x01, "RTPS", UINT64_C(0x67DB10006F92A79A)},  {0x06, "NCLIP", UINT64_C(0x6FF888C2E9723670)},
    {0x0C, "OP", UINT64_C(0x0198396CA9B93119)},    {0x10, "DPCS", UINT64_C(0x06F2D283E74AE1CD)},
    {0x11, "INTPL", UINT64_C(0x6EF2EC0137B1B261)}, {0x12, "MVMVA", UINT64_C(0x52EE5E2C045F3B93)},
    {0x13, "NCDS", UINT64_C(0x4EE2E9B367B7A687)},  {0x14, "CDP", UINT64_C(0x5FCCBD6B3CAD4E02)},
    {0x16, "NCDT", UINT64_C(0xB348FE7BB72B5E61)},  {0x1B, "NCCS", UINT64_C(0xD7C544FE788657E0)},
    {0x1C, "CC", UINT64_C(0x9D5E7A94079B7A1B)},    {0x1E, "NCS", UINT64_C(0xC4CD369E7EE8C71B)},
    {0x20, "NCT", UINT64_C(0x6397E7FFC162420C)},   {0x28, "SQR", UINT64_C(0xECAF13972B0704E2)},
    {0x29, "DCPL", UINT64_C(0x37FCCC7EFFC1F34F)},  {0x2A, "DPCT", UINT64_C(0x8CAD4B6FEA89BD86)},
    {0x2D, "AVSZ3", UINT64_C(0x2BB924F2315C3943)}, {0x2E, "AVSZ4", UINT64_C(0xBCB5C07569392954)},
    {0x30, "RTPT", UINT64_C(0x7B5F2BE827D9D2F4)},  {0x3D, "GPF", UINT64_C(0xF2962F44DE3C7551)},
    {0x3E, "GPL", UINT64_C(0x4B553A1C4CD9DE0A)},   {0x3F, "NCCT", UINT64_C(0xD9C9884142F6EF5F)},
  };

  for (const Expected& e : expected)
  {
    const u64 hash = RunRandomInstructions(e.command, 20000, UINT64_C(0x475445) + e.command);
    EXPECT_EQ(hash, e.hash) << e.name << " hash is 0x" << std::hex << hash;
  }
}

// Widescreen rendering changes the screen coordinates produced by RTPS/RTPT, but should do so consistently.
TEST(GTE, RandomWidescreenProjectionMatchesReference)
{
  struct Expected
  {
    DisplayAspectRatio aspect_ratio;
    u64 rtps_hash;
    u64 rtpt_hash;
  };
  static constexpr Expected expected[] = {
    {DisplayAspectRatio{16, 9}, UINT64_C(0xE1872C09241C0916), UINT64_C(0xC2B954F4B811C16F)},
    {DisplayAspectRatio{19, 9}, UINT64_C(0x9D6959FBF368F868), UINT64_C(0x8FDFCA41229E8E17)},
    {DisplayAspectRatio{20, 9}, UINT64_C(0x5D3D669C26417415), UINT64_C(0x8E0DBEF4343A23F4)},
    {DisplayAspectRatio{21, 9}, UINT64_C(0x5F6A3CE96DF5F276), UINT64_C(0x8C89A270E1782513)},
  };

  for (const Expected& e : expected)
  {
    GTE::SetAspectRatio(e.aspect_ratio);

    const u64 rtps_hash = RunRandomInstructions(CMD_RTPS, 10000, UINT64_C(0x57494445) + e.aspect_ratio.numerator);
    const u64 rtpt_hash = RunRandomInstructions(CMD_RTPT, 10000, UINT64_C(0x57494445) + e.aspect_ratio.numerator);
    EXPECT_EQ(rtps_hash, e.rtps_hash) << e.aspect_ratio.numerator << ":" << e.aspect_ratio.denominator
                                      << " RTPS hash is 0x" << std::hex << rtps_hash;
    EXPECT_EQ(rtpt_hash, e.rtpt_hash) << e.aspect_ratio.numerator << ":" << e.aspect_ratio.denominator
                                      << " RTPT hash is 0x" << std::hex << rtpt_hash;
  }

  GTE::SetAspectRatio(DisplayAspectRatio::Auto());
}
