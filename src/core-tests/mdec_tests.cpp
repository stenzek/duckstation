// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"
#include "core/mdec.h"
#include "core/timing_event.h"

#include "common/bitutils.h"
#include "common/types.h"
#include "common/xorshift_prng.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace {

enum : u32
{
  DEPTH_4BIT = 0,
  DEPTH_8BIT = 1,
  DEPTH_24BIT = 2,
  DEPTH_15BIT = 3,

  STATUS_DATA_OUT_FIFO_EMPTY = (1u << 31),
  STATUS_COMMAND_BUSY = (1u << 29),
  STATUS_DATA_IN_REQUEST = (1u << 28),

  CONTROL_ENABLE_DMA_IN = (1u << 30),
};

using Random = XorShift128PlusPlus;
using Block = std::array<s16, 64>;

struct Tables
{
  std::array<u8, 64> iq_y;
  std::array<u8, 64> iq_uv;
  std::array<u16, 64> scale;
};

} // namespace

static constexpr std::array<u8, 64> s_zigzag = {{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                                 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                                 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                                 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63}};

// Quantization table in zigzag order, and IDCT scale table, as used by most games.
static constexpr std::array<u8, 64> s_iq_table = {
  {0x02, 0x10, 0x10, 0x13, 0x10, 0x13, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x1A, 0x18, 0x1A, 0x1B,
   0x1B, 0x1B, 0x1A, 0x1A, 0x1A, 0x1A, 0x1B, 0x1B, 0x1B, 0x1D, 0x1D, 0x1D, 0x22, 0x22, 0x22, 0x1D,
   0x1D, 0x1D, 0x1B, 0x1B, 0x1D, 0x1D, 0x20, 0x20, 0x22, 0x22, 0x25, 0x26, 0x25, 0x23, 0x23, 0x22,
   0x23, 0x26, 0x26, 0x28, 0x28, 0x28, 0x30, 0x30, 0x2E, 0x2E, 0x38, 0x38, 0x3A, 0x45, 0x45, 0x53}};
static constexpr std::array<u16, 64> s_scale_table = {
  {0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x7D8A, 0x6A6D, 0x471C, 0x18F8, 0xE707,
   0xB8E3, 0x9592, 0x8275, 0x7641, 0x30FB, 0xCF04, 0x89BE, 0x89BE, 0xCF04, 0x30FB, 0x7641, 0x6A6D, 0xE707,
   0x8275, 0xB8E3, 0x471C, 0x7D8A, 0x18F8, 0x9592, 0x5A82, 0xA57D, 0xA57D, 0x5A82, 0x5A82, 0xA57D, 0xA57D,
   0x5A82, 0x471C, 0x8275, 0x18F8, 0x6A6D, 0x9592, 0xE707, 0x7D8A, 0xB8E3, 0x30FB, 0x89BE, 0x7641, 0xCF04,
   0xCF04, 0x7641, 0x89BE, 0x30FB, 0x18F8, 0xB8E3, 0x6A6D, 0x8275, 0x7D8A, 0x9592, 0x471C, 0xE707}};
static constexpr Tables s_standard_tables = {s_iq_table, s_iq_table, s_scale_table};

static u32 NextIndex(Random& rng, u32 count)
{
  return static_cast<u32>(rng.NextRange(static_cast<u64>(count)));
}

/// Generates complete blocks. Dense blocks have all 64 coefficients present, and use the full range for the levels and
/// quantizer scale, which saturates most of the coefficients. Otherwise, blocks have a handful of small coefficients,
/// like in typical streams.
static std::vector<u16> GenerateStream(Random& rng, u32 num_blocks, bool dense)
{
  std::vector<u16> stream;
  for (u32 i = 0; i < num_blocks; i++)
  {
    // 0xFE00 is padding when it is in place of the DC coefficient.
    u16 dc;
    do
    {
      const u32 q_scale = NextIndex(rng, dense ? 64 : 16);
      dc = static_cast<u16>((q_scale << 10) | NextIndex(rng, 0x400));
    } while (dc == 0xFE00);
    stream.push_back(dc);

    u32 k = 0;
    while (k < 63 && (dense || NextIndex(rng, 4) != 0))
    {
      const u32 run = (dense || NextIndex(rng, 4) != 0) ? 0 : NextIndex(rng, 8);
      if ((k + run + 1) > 63)
        break;

      const u32 level = dense ? NextIndex(rng, 0x400) : ((NextIndex(rng, 33) - 16) & 0x3FF);
      stream.push_back(static_cast<u16>((run << 10) | level));
      k += run + 1;
    }

    // End of block is implied when the last coefficient is present.
    if (k < 63)
      stream.push_back(0xFE00);
  }

  // Parameters are in words.
  if (stream.size() & 1)
    stream.push_back(0xFE00);

  return stream;
}

static void SetTables(const Tables& tables)
{
  // Luminance table followed by chrominance table.
  MDEC::WriteRegister(0, (2u << 29) | 1u);
  for (const std::array<u8, 64>* table : {&tables.iq_y, &tables.iq_uv})
  {
    for (u32 i = 0; i < 64; i += 4)
    {
      u32 word;
      std::memcpy(&word, &(*table)[i], sizeof(word));
      MDEC::WriteRegister(0, word);
    }
  }

  MDEC::WriteRegister(0, (3u << 29));
  for (u32 i = 0; i < 64; i += 2)
    MDEC::WriteRegister(0, ZeroExtend32(tables.scale[i]) | (ZeroExtend32(tables.scale[i + 1]) << 16));
}

static std::vector<u32> Decode(u32 depth, bool output_signed, bool output_bit15, const std::vector<u16>& stream)
{
  std::vector<u32> output;

  MDEC::WriteRegister(0, (1u << 29) | (depth << 27) | (BoolToUInt32(output_signed) << 26) |
                           (BoolToUInt32(output_bit15) << 25) | static_cast<u32>(stream.size() / 2));
  for (size_t i = 0; i < stream.size(); i += 2)
  {
    MDEC::WriteRegister(0, ZeroExtend32(stream[i]) | (ZeroExtend32(stream[i + 1]) << 16));

    // If the command is still busy, but it's not decoding, then it's waiting for the macroblock to be copied out.
    while (!MDEC::IsDecodingMacroblock() && (MDEC::ReadRegister(4) & STATUS_COMMAND_BUSY))
    {
      CPU::AddPendingTicks(static_cast<TickCount>(CPU::g_state.downcount));
      TimingEvents::RunEvents();

      while (!(MDEC::ReadRegister(4) & STATUS_DATA_OUT_FIFO_EMPTY))
        output.push_back(MDEC::ReadRegister(0));
    }
  }

  return output;
}

static void ReferenceIDCT(Block& blk, const std::array<u16, 64>& scale)
{
  Block temp;
  for (u32 y = 0; y < 8; y++)
  {
    for (u32 x = 0; x < 8; x++)
    {
      s64 sum = 0;
      for (u32 u = 0; u < 8; u++)
        sum += static_cast<s64>(blk[u * 8 + x]) * static_cast<s16>(scale[u * 8 + y]);

      temp[y * 8 + x] = static_cast<s16>((sum + 0x20000) >> 18);
    }
  }

  for (u32 y = 0; y < 8; y++)
  {
    for (u32 x = 0; x < 8; x++)
    {
      s64 sum = 0;
      for (u32 u = 0; u < 8; u++)
        sum += static_cast<s64>(temp[y * 8 + u]) * static_cast<s16>(scale[u * 8 + x]);

      blk[y * 8 + x] =
        static_cast<s16>(std::clamp(SignExtendN<9, s32>(static_cast<s32>((sum + 0x20000) >> 18)), -128, 127));
    }
  }
}

/// Returns false if there is not a complete block remaining in the stream.
static bool ReferenceDecodeBlock(Block& blk, const u16*& stream, const u16* stream_end, const std::array<u8, 64>& qt,
                                 const std::array<u16, 64>& scale)
{
  // Blocks have an additional 4 bits of precision.
  const auto saturate = [](s32 val, s32 coeff) {
    return static_cast<s16>(std::clamp(coeff + (val ? ((val < 0) ? 8 : -8) : 0), -0x4000, 0x3FFF));
  };

  u16 n;
  do
  {
    if (stream == stream_end)
      return false;

    n = *(stream++);
  } while (n == 0xFE00);

  // The DC coefficient is not multiplied by the quantizer scale.
  const s32 q_scale = n >> 10;
  const s32 dc = SignExtendN<10, s32>(n);
  blk.fill(0);
  blk[0] = (q_scale == 0) ? static_cast<s16>(dc << 5) : saturate(dc, (dc * qt[0]) << 4);

  for (u32 k = 0; k < 63;)
  {
    if (stream == stream_end)
      return false;

    n = *(stream++);
    k += (n >> 10) + 1;
    if (k < 64)
    {
      const s32 val = SignExtendN<10, s32>(n);
      const s32 scq = q_scale * qt[k];
      blk[s_zigzag[k]] = (scq == 0) ? static_cast<s16>(val << 5) : saturate(val, ((val * scq) >> 3) << 4);
    }
  }

  ReferenceIDCT(blk, scale);
  return true;
}

static std::vector<u32> ReferenceDecode(u32 depth, bool output_signed, bool output_bit15,
                                        const std::vector<u16>& stream, const Tables& tables)
{
  const auto to_u8 = [output_signed](s32 value) {
    return static_cast<u8>(std::clamp(SignExtendN<9, s32>(value), -128, 127) + (output_signed ? 0 : 0x80));
  };

  std::vector<u32> output;
  const u16* stream_ptr = stream.data();
  const u16* const stream_end = stream_ptr + stream.size();

  if (depth == DEPTH_4BIT || depth == DEPTH_8BIT)
  {
    Block blk;
    while (ReferenceDecodeBlock(blk, stream_ptr, stream_end, tables.iq_y, tables.scale))
    {
      const u32 pixels_per_word = (depth == DEPTH_4BIT) ? 8 : 4;
      const u32 bits_per_pixel = 32 / pixels_per_word;
      for (u32 i = 0; i < 64; i += pixels_per_word)
      {
        u32 word = 0;
        for (u32 j = 0; j < pixels_per_word; j++)
          word |= ZeroExtend32(to_u8(blk[i + j]) >> (8 - bits_per_pixel)) << (j * bits_per_pixel);

        output.push_back(word);
      }
    }

    return output;
  }

  for (;;)
  {
    // Cr, Cb, Y1-4
    std::array<Block, 6> blocks;
    for (u32 i = 0; i < 6; i++)
    {
      if (!ReferenceDecodeBlock(blocks[i], stream_ptr, stream_end, (i >= 2) ? tables.iq_y : tables.iq_uv, tables.scale))
      {
        return output;
      }
    }

    std::array<u8, 16 * 16 * 3> rgb;
    for (u32 y = 0; y < 16; y++)
    {
      for (u32 x = 0; x < 16; x++)
      {
        const s32 Cr = blocks[0][(y / 2) * 8 + (x / 2)];
        const s32 Cb = blocks[1][(y / 2) * 8 + (x / 2)];
        const s32 Y = blocks[2 + (y / 8) * 2 + (x / 8)][(y % 8) * 8 + (x % 8)];
        u8* const pixel = &rgb[(y * 16 + x) * 3];
        pixel[0] = to_u8(Y + (((359 * Cr) + 0x80) >> 8));
        pixel[1] = to_u8(Y + ((((-88 * Cb) & ~0x1F) + ((-183 * Cr) & ~0x07) + 0x80) >> 8));
        pixel[2] = to_u8(Y + (((454 * Cb) + 0x80) >> 8));
      }
    }

    if (depth == DEPTH_24BIT)
    {
      for (size_t i = 0; i < rgb.size(); i += sizeof(u32))
      {
        u32 word;
        std::memcpy(&word, &rgb[i], sizeof(word));
        output.push_back(word);
      }
    }
    else
    {
      const auto to_rgb555 = [output_bit15](const u8* pixel) {
        const auto to_5bit = [](u8 value) { return std::min<u32>((ZeroExtend32(value) + 4) >> 3, 0x1F); };
        return to_5bit(pixel[0]) | (to_5bit(pixel[1]) << 5) | (to_5bit(pixel[2]) << 10) |
               (BoolToUInt32(output_bit15) << 15);
      };

      for (size_t i = 0; i < rgb.size(); i += 6)
        output.push_back(to_rgb555(&rgb[i]) | (to_rgb555(&rgb[i + 3]) << 16));
    }
  }
}

static void TestDecode(u32 depth, bool output_signed, bool output_bit15, const std::vector<u16>& stream,
                       const Tables& tables)
{
  SCOPED_TRACE(testing::Message() << "depth=" << depth << " signed=" << output_signed << " bit15=" << output_bit15);

  const std::vector<u32> expected = ReferenceDecode(depth, output_signed, output_bit15, stream, tables);
  const std::vector<u32> actual = Decode(depth, output_signed, output_bit15, stream);
  ASSERT_FALSE(expected.empty());
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < expected.size(); i++)
    ASSERT_EQ(actual[i], expected[i]) << "at word " << i;
}

static void TestDecode(const Tables& tables, const std::vector<u16>& stream)
{
  // There is always something scheduled in the actual system, the event list cannot be empty.
  TimingEvent idle_event("MDEC Test Idle", 1, 1, [](void*, TickCount) {}, nullptr);
  CPU::g_state.pending_ticks = 0;
  TimingEvents::Reset();
  idle_event.Schedule(std::numeric_limits<TickCount>::max());

  MDEC::Initialize();
  SetTables(tables);

  for (const u32 depth : {DEPTH_4BIT, DEPTH_8BIT, DEPTH_24BIT, DEPTH_15BIT})
  {
    for (const bool output_signed : {false, true})
    {
      // Bit 15 only applies to 15-bit output.
      for (const bool output_bit15 : {false, true})
      {
        if ((!output_bit15 || depth == DEPTH_15BIT) && !testing::Test::HasFatalFailure())
          TestDecode(depth, output_signed, output_bit15, stream, tables);
      }
    }
  }

  MDEC::Shutdown();
  idle_event.Deactivate();
}

TEST(MDEC, TypicalBlocksMatchReference)
{
  Random rng(0x4D444543u);
  TestDecode(s_standard_tables, GenerateStream(rng, 6 * 200, false));
}

TEST(MDEC, SaturatedBlocksMatchReference)
{
  // Exceeds 32 bits in the IDCT, and both the 9-bit wrap and clamp in the colour conversion.
  Random rng(0x53415455u);
  TestDecode(s_standard_tables, GenerateStream(rng, 6 * 40, true));
}

TEST(MDEC, MixedBlocksMatchReference)
{
  Random rng(0x4D495845u);
  std::vector<u16> stream;
  for (u32 i = 0; i < 100; i++)
  {
    const std::vector<u16> blocks = GenerateStream(rng, 1 + NextIndex(rng, 4), NextIndex(rng, 4) == 0);
    stream.insert(stream.end(), blocks.begin(), blocks.end());
  }

  TestDecode(s_standard_tables, stream);
}

TEST(MDEC, CustomTablesMatchReference)
{
  Random rng(0x5441424Cu);
  for (u32 i = 0; i < 8; i++)
  {
    // Zero in the quantization table uses the unscaled coefficient path.
    Tables tables;
    for (u32 j = 0; j < 64; j++)
    {
      tables.iq_y[j] = static_cast<u8>(NextIndex(rng, 256));
      tables.iq_uv[j] = (NextIndex(rng, 8) == 0) ? u8(0) : static_cast<u8>(NextIndex(rng, 256));
      tables.scale[j] = static_cast<u16>(NextIndex(rng, 0x10000));
    }

    TestDecode(tables, GenerateStream(rng, 6 * 20, (i % 2) != 0));
  }
}

TEST(MDEC, DataInRequestOnlyWhileCommandNeedsParameters)
{
  // Games such as GameShark 4 write the command word with the CPU, and start an input DMA with a block count of zero.
  // The DMA must stall once the command has its parameters, instead of feeding the rest of RAM in as commands.
  TimingEvent idle_event("MDEC Test Idle", 1, 1, [](void*, TickCount) {}, nullptr);
  CPU::g_state.pending_ticks = 0;
  TimingEvents::Reset();
  idle_event.Schedule(std::numeric_limits<TickCount>::max());

  MDEC::Initialize();
  MDEC::WriteRegister(4, CONTROL_ENABLE_DMA_IN);
  EXPECT_FALSE(MDEC::ReadRegister(4) & STATUS_DATA_IN_REQUEST);

  // Set scale table, 32 parameter words.
  MDEC::WriteRegister(0, (3u << 29));
  EXPECT_TRUE(MDEC::ReadRegister(4) & STATUS_DATA_IN_REQUEST);

  std::array<u32, 32> words;
  for (u32 i = 0; i < 32; i++)
    words[i] = ZeroExtend32(s_scale_table[i * 2]) | (ZeroExtend32(s_scale_table[i * 2 + 1]) << 16);
  MDEC::DMAWrite(words.data(), static_cast<u32>(words.size()));
  EXPECT_FALSE(MDEC::ReadRegister(4) & STATUS_COMMAND_BUSY);
  EXPECT_FALSE(MDEC::ReadRegister(4) & STATUS_DATA_IN_REQUEST);

  // Decode macroblock with only padding, still wants data until all parameters have been received.
  MDEC::WriteRegister(0, (1u << 29) | (DEPTH_24BIT << 27) | 64);
  EXPECT_TRUE(MDEC::ReadRegister(4) & STATUS_DATA_IN_REQUEST);

  words.fill(0xFE00FE00u);
  MDEC::DMAWrite(words.data(), static_cast<u32>(words.size()));
  EXPECT_TRUE(MDEC::ReadRegister(4) & STATUS_DATA_IN_REQUEST);
  MDEC::DMAWrite(words.data(), static_cast<u32>(words.size()));
  EXPECT_FALSE(MDEC::ReadRegister(4) & STATUS_COMMAND_BUSY);
  EXPECT_FALSE(MDEC::ReadRegister(4) & STATUS_DATA_IN_REQUEST);

  MDEC::Shutdown();
  idle_event.Deactivate();
}
