// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/gpu.h"
#include "core/gpu_sw_rasterizer.h"
#include "core/gpu_types.h"

#include "common/types.h"
#include "common/xorshift_prng.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

class GPUSWRasterizer : public ::testing::Test
{
protected:
  void SetUp() override
  {
    GPU_SW_Rasterizer::SelectImplementation();
    GPU_SW_Rasterizer::g_drawing_area = {0, 0, VRAM_WIDTH - 1, VRAM_HEIGHT - 1};

    // Fill VRAM with noise, so that the mask bit and transparent pixels show up everywhere.
    XorShift128PlusPlus rng(0x1234);
    for (u16& pixel : g_vram)
      pixel = static_cast<u16>(rng.Next());

    m_initial_vram.assign(std::begin(g_vram), std::end(g_vram));
    m_expected_vram = m_initial_vram;
  }

  static u32 GetVRAMIndex(u32 x, u32 y) { return ((y % VRAM_HEIGHT) * VRAM_WIDTH) + (x % VRAM_WIDTH); }

  /// Compares all of VRAM against the expected contents, reporting the first few pixels which differ.
  void CheckVRAM() const
  {
    u32 num_mismatches = 0;
    for (u32 i = 0; i < (VRAM_WIDTH * VRAM_HEIGHT); i++)
    {
      if (g_vram[i] == m_expected_vram[i])
        continue;

      if ((num_mismatches++) < 8)
      {
        ADD_FAILURE() << "Mismatch at " << (i % VRAM_WIDTH) << "," << (i / VRAM_WIDTH) << ": expected 0x" << std::hex
                      << m_expected_vram[i] << ", got 0x" << g_vram[i];
      }
    }

    EXPECT_EQ(num_mismatches, 0u);
  }

  std::vector<u16> m_initial_vram;
  std::vector<u16> m_expected_vram;
};

} // namespace

TEST_F(GPUSWRasterizer, WriteVRAM)
{
  struct TestCase
  {
    u32 x, y, width, height;
    bool set_mask, check_mask;
  };
  static constexpr TestCase test_cases[] = {
    {16, 16, 64, 8, false, false},   {33, 40, 13, 5, false, false}, {33, 60, 13, 5, true, false},
    {33, 80, 13, 5, false, true},    {33, 100, 13, 5, true, true},  {77, 120, 3, 3, false, true},
    {1017, 140, 21, 4, false, true}, {500, 508, 30, 9, true, true},
  };

  XorShift128PlusPlus rng(0x5678);
  for (const TestCase& tc : test_cases)
  {
    std::vector<u16> data(tc.width * tc.height);
    for (u16& pixel : data)
      pixel = static_cast<u16>(rng.Next());

    // Each source pixel maps to a fixed position, and is dropped when the destination is masked.
    for (u32 row = 0; row < tc.height; row++)
    {
      for (u32 col = 0; col < tc.width; col++)
      {
        u16& dst = m_expected_vram[GetVRAMIndex(tc.x + col, tc.y + row)];
        if (!tc.check_mask || !(dst & 0x8000u))
          dst = data[(row * tc.width) + col] | (tc.set_mask ? 0x8000u : 0u);
      }
    }

    GPU_SW_Rasterizer::WriteVRAM(tc.x, tc.y, tc.width, tc.height, data.data(), tc.set_mask, tc.check_mask);
  }

  CheckVRAM();
}
