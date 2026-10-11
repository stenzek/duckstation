// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/gpu.h"
#include "core/gpu_sw_rasterizer.h"
#include "core/gpu_types.h"
#include "core/video_thread_commands.h"

#include "common/types.h"
#include "common/xorshift_prng.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

using GPU_SW_Rasterizer::TextureModulationMode;
using Vertex = GPUBackendDrawPolygonCommand::Vertex;

// Primitives are drawn to the left half of VRAM, and textures are sampled from the right half, since the result of
// sampling pixels drawn by the same primitive depends on how many pixels the implementation processes at once.
constexpr u32 DRAW_AREA_LEFT = 256;
constexpr u32 DRAW_AREA_RIGHT = 511;
constexpr u32 TEXTURE_AREA_LEFT = 512;
constexpr u8 FIRST_TEXTURE_PAGE = TEXTURE_AREA_LEFT / 64;

constexpr GPUTextureMode TEXTURE_MODES[] = {GPUTextureMode::Palette4Bit, GPUTextureMode::Palette8Bit,
                                            GPUTextureMode::Direct16Bit};
constexpr TextureModulationMode TEXTURED_MODULATION_MODES[] = {
  TextureModulationMode::NoModulation, TextureModulationMode::Modulate8Bit, TextureModulationMode::Modulate5Bit};

class GPUSWRasterizer : public ::testing::Test
{
protected:
  void SetUp() override
  {
    GPU_SW_Rasterizer::SelectImplementation();
    GPU_SW_Rasterizer::g_drawing_area = {DRAW_AREA_LEFT, 0, DRAW_AREA_RIGHT, VRAM_HEIGHT - 1};

    // Fill VRAM with noise, so that the mask bit shows up everywhere, with some transparent texels mixed in.
    XorShift128PlusPlus rng(0x1234);
    for (u32 i = 0; i < (VRAM_WIDTH * VRAM_HEIGHT); i++)
    {
      const u64 value = rng.Next();
      g_vram[i] = ((i % VRAM_WIDTH) >= TEXTURE_AREA_LEFT && (value >> 16) % 8 == 0) ? 0 : static_cast<u16>(value);
    }
    for (u32 i = 0; i < GPU_CLUT_SIZE; i++)
      g_gpu_clut[i] = ((i % 5) == 0) ? 0 : static_cast<u16>(rng.Next());

    m_initial_vram.assign(std::begin(g_vram), std::end(g_vram));
    m_expected_vram = m_initial_vram;
  }

  static u32 GetVRAMIndex(u32 x, u32 y) { return ((y % VRAM_HEIGHT) * VRAM_WIDTH) + (x % VRAM_WIDTH); }

  static bool IsInsideDrawingArea(s32 x, s32 y)
  {
    const GPUDrawingArea& area = GPU_SW_Rasterizer::g_drawing_area;
    return (x >= static_cast<s32>(area.left) && x <= static_cast<s32>(area.right) && y >= static_cast<s32>(area.top) &&
            y <= static_cast<s32>(area.bottom));
  }

  static u16 SampleTexture(const GPUBackendDrawCommand& cmd, u8 u, u8 v)
  {
    u = (u & cmd.window.and_x) | cmd.window.or_x;
    v = (v & cmd.window.and_y) | cmd.window.or_y;

    const u32 base_x = cmd.draw_mode.GetTexturePageBaseX();
    const u32 y = cmd.draw_mode.GetTexturePageBaseY() + v;
    switch (cmd.draw_mode.texture_mode)
    {
      case GPUTextureMode::Palette4Bit:
        return g_gpu_clut[(g_vram[GetVRAMIndex(base_x + (u / 4), y)] >> ((u % 4) * 4)) & 0x0Fu];

      case GPUTextureMode::Palette8Bit:
        return g_gpu_clut[(g_vram[GetVRAMIndex(base_x + (u / 2), y)] >> ((u % 2) * 8)) & 0xFFu];

      default:
        return g_vram[GetVRAMIndex(base_x + u, y)];
    }
  }

  /// Straightforward version of the pixel pipeline, working on one colour component at a time.
  static void ShadePixel(const GPUBackendDrawCommand& cmd, TextureModulationMode modulation_mode, u32 color, u8 u, u8 v,
                         s32 dither, u16& dst)
  {
    const auto dither_and_truncate = [dither](u32 value) {
      return static_cast<u32>(std::clamp((static_cast<s32>(value) + dither) >> 3, 0, 31));
    };

    // Only texels with bit 15 set are blended, the bit is otherwise passed through.
    u16 texel = 0;
    bool blend = cmd.transparency_enable;
    if (modulation_mode != TextureModulationMode::Disabled)
    {
      texel = SampleTexture(cmd, u, v);
      if (texel == 0)
        return;

      blend = (blend && (texel & 0x8000u) != 0);
    }

    u16 result = (texel & 0x8000u);
    for (u32 i = 0; i < 3; i++)
    {
      const u32 vertex_component = (color >> (i * 8)) & 0xFFu;
      const u32 texel_component = (texel >> (i * 5)) & 0x1Fu;
      u32 component;
      switch (modulation_mode)
      {
        case TextureModulationMode::NoModulation:
          component = texel_component;
          break;
        case TextureModulationMode::Modulate8Bit:
          component = dither_and_truncate((texel_component * vertex_component) >> 4);
          break;
        case TextureModulationMode::Modulate5Bit:
          component = dither_and_truncate((texel_component * (vertex_component >> 3)) >> 1);
          break;
        default:
          component = dither_and_truncate(vertex_component);
          break;
      }

      if (blend)
      {
        const u32 bg_component = (dst >> (i * 5)) & 0x1Fu;
        switch (cmd.draw_mode.transparency_mode)
        {
          case GPUTransparencyMode::HalfBackgroundPlusHalfForeground:
            component = (bg_component + component) / 2;
            break;
          case GPUTransparencyMode::BackgroundPlusForeground:
            component = std::min<u32>(bg_component + component, 31);
            break;
          case GPUTransparencyMode::BackgroundMinusForeground:
            component = (bg_component > component) ? (bg_component - component) : 0;
            break;
          default:
            component = std::min<u32>(bg_component + (component / 4), 31);
            break;
        }
      }

      result |= static_cast<u16>(component << (i * 5));
    }

    if (cmd.check_mask_before_draw && (dst & 0x8000u))
      return;

    dst = result | (cmd.set_mask_while_drawing ? 0x8000u : 0u);
  }

  void DrawRectangle(const GPUBackendDrawRectangleCommand& cmd, TextureModulationMode modulation_mode)
  {
    for (u32 row = 0; row < cmd.height; row++)
    {
      for (u32 col = 0; col < cmd.width; col++)
      {
        const s32 x = cmd.x + static_cast<s32>(col);
        const s32 y = cmd.y + static_cast<s32>(row);
        if (IsInsideDrawingArea(x, y))
        {
          ShadePixel(cmd, modulation_mode, cmd.color, static_cast<u8>(cmd.texcoord + col),
                     static_cast<u8>((cmd.texcoord >> 8) + row), 0, m_expected_vram[GetVRAMIndex(x, y)]);
        }
      }
    }

    GPU_SW_Rasterizer::GetDrawRectangleFunction(modulation_mode, cmd.transparency_enable)(&cmd);
  }

  /// Draws a quad with one texel per pixel, which covers the same pixels as the equivalent rectangle.
  void DrawQuad(const GPUBackendDrawCommand& cmd, TextureModulationMode modulation_mode, s32 x, s32 y, u32 width,
                u32 height, u32 color, u8 u, u8 v)
  {
    for (u32 row = 0; row < height; row++)
    {
      for (u32 col = 0; col < width; col++)
      {
        const s32 px = x + static_cast<s32>(col);
        const s32 py = y + static_cast<s32>(row);
        if (IsInsideDrawingArea(px, py))
        {
          ShadePixel(cmd, modulation_mode, color, static_cast<u8>(u + col), static_cast<u8>(v + row),
                     cmd.dither_enable ? DITHER_MATRIX[py & 3][px & 3] : 0, m_expected_vram[GetVRAMIndex(px, py)]);
        }
      }
    }

    Vertex vertices[4];
    for (u32 i = 0; i < 4; i++)
    {
      const u32 right = (i & 1u), bottom = (i >> 1);
      vertices[i].x = x + static_cast<s32>(right * width);
      vertices[i].y = y + static_cast<s32>(bottom * height);
      vertices[i].color = color;
      vertices[i].texcoord = static_cast<u16>((u + (right * width)) | ((v + (bottom * height)) << 8));
    }

    DrawTriangle(cmd, modulation_mode, vertices[0], vertices[1], vertices[2]);
    DrawTriangle(cmd, modulation_mode, vertices[2], vertices[1], vertices[3]);
  }

  static void DrawTriangle(const GPUBackendDrawCommand& cmd, TextureModulationMode modulation_mode, const Vertex& v0,
                           const Vertex& v1, const Vertex& v2)
  {
    GPU_SW_Rasterizer::GetDrawTriangleFunction(cmd.shading_enable, modulation_mode, cmd.transparency_enable)(&cmd, &v0,
                                                                                                             &v1, &v2);
  }

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

TEST_F(GPUSWRasterizer, CopyVRAM)
{
  struct TestCase
  {
    u32 src_x, src_y, dst_x, dst_y, width, height;
    bool set_mask, check_mask;
  };
  static constexpr TestCase test_cases[] = {
    {10, 10, 300, 200, 67, 9, false, false},  // to the right, separate
    {300, 200, 10, 30, 67, 9, false, false},  // to the left, separate
    {20, 50, 31, 50, 67, 5, false, false},    // to the right, overlapping in the same rows
    {31, 60, 20, 60, 67, 5, false, false},    // to the left, overlapping in the same rows
    {20, 70, 31, 72, 67, 9, false, false},    // to the right, overlapping in different rows
    {31, 90, 20, 88, 67, 9, false, false},    // to the left, overlapping in different rows
    {40, 110, 40, 110, 35, 4, true, false},   // onto itself
    {50, 120, 61, 120, 67, 5, true, true},    // overlapping with masking
    {61, 130, 50, 132, 67, 9, false, true},   //
    {100, 140, 400, 150, 19, 3, true, true},  //
    {500, 505, 700, 509, 33, 12, false, true} // wraps around vertically
  };

  for (const TestCase& tc : test_cases)
  {
    // Rows are copied from top to bottom, pixels within a row are never read after being written.
    std::vector<u16> src_row(tc.width);
    for (u32 row = 0; row < tc.height; row++)
    {
      for (u32 col = 0; col < tc.width; col++)
        src_row[col] = m_expected_vram[GetVRAMIndex(tc.src_x + col, tc.src_y + row)];

      for (u32 col = 0; col < tc.width; col++)
      {
        u16& dst = m_expected_vram[GetVRAMIndex(tc.dst_x + col, tc.dst_y + row)];
        if (!tc.check_mask || !(dst & 0x8000u))
          dst = src_row[col] | (tc.set_mask ? 0x8000u : 0u);
      }
    }

    GPU_SW_Rasterizer::CopyVRAM(tc.src_x, tc.src_y, tc.dst_x, tc.dst_y, tc.width, tc.height, tc.set_mask,
                                tc.check_mask);
  }

  CheckVRAM();
}

TEST_F(GPUSWRasterizer, DrawRectangle)
{
  struct TestCase
  {
    s32 x;
    u16 width, height;
    u8 u, v, texture_page;
    bool texture_window, transparency, check_mask, set_mask, clip;
  };
  static constexpr TestCase test_cases[] = {
    {300, 37, 5, 0, 0, FIRST_TEXTURE_PAGE + 1, false, false, false, false, false},    // aligned
    {301, 37, 5, 13, 250, FIRST_TEXTURE_PAGE + 2, false, false, false, false, false}, // unaligned, V wraps around
    {298, 40, 5, 238, 3, FIRST_TEXTURE_PAGE + 3, false, false, false, false, false},  // U wraps around
    {310, 90, 4, 170, 30, 15, false, false, false, false, false},                     // page wraps around VRAM
    {305, 33, 5, 7, 7, FIRST_TEXTURE_PAGE + 4, true, false, false, false, false},     // texture window
    {303, 50, 5, 100, 100, FIRST_TEXTURE_PAGE + 5, false, true, false, false, false}, // semi-transparent
    {299, 21, 5, 31, 200, FIRST_TEXTURE_PAGE + 6, false, false, true, true, false},   // mask test and set
    {302, 19, 5, 3, 66, FIRST_TEXTURE_PAGE, false, true, true, false, false},         // semi-transparent mask test
    {250, 300, 9, 90, 20, FIRST_TEXTURE_PAGE + 2, false, false, false, false, true},  // clipped on all sides
    {1, 1, 1, 77, 88, FIRST_TEXTURE_PAGE + 3, false, false, false, false, false},     // outside drawing area
    {400, 1, 1, 77, 88, FIRST_TEXTURE_PAGE + 3, false, true, false, false, false},    // single pixel
  };

  s32 y = 3;
  u8 transparency_mode = 0;
  const auto draw = [&](GPUTextureMode texture_mode, TextureModulationMode modulation_mode) {
    for (const TestCase& tc : test_cases)
    {
      GPUBackendDrawRectangleCommand cmd = {};
      cmd.transparency_enable = tc.transparency;
      cmd.check_mask_before_draw = tc.check_mask;
      cmd.set_mask_while_drawing = tc.set_mask;
      cmd.draw_mode.texture_page = static_cast<u8>(tc.texture_page | ((y & 0x100) ? 0x00 : 0x10));
      cmd.draw_mode.texture_mode = texture_mode;
      if (tc.transparency)
        cmd.draw_mode.transparency_mode = static_cast<GPUTransparencyMode>((transparency_mode++) & 3);
      cmd.window.and_x = tc.texture_window ? 0xE7 : 0xFF;
      cmd.window.and_y = tc.texture_window ? 0xF8 : 0xFF;
      cmd.window.or_x = tc.texture_window ? 0x08 : 0x00;
      cmd.window.or_y = tc.texture_window ? 0x10 : 0x00;
      cmd.x = tc.x;
      cmd.y = y;
      cmd.width = tc.width;
      cmd.height = tc.height;
      cmd.texcoord = static_cast<u16>(tc.u | (tc.v << 8));
      cmd.color = (0x80C040u + (static_cast<u32>(y) * 0x030507u)) & 0xFFFFFFu;

      GPU_SW_Rasterizer::g_drawing_area =
        tc.clip ? GPUDrawingArea{258, static_cast<u32>(y) + 2, 505, static_cast<u32>(y) + tc.height - 3} :
                  GPUDrawingArea{DRAW_AREA_LEFT, 0, DRAW_AREA_RIGHT, VRAM_HEIGHT - 1};
      DrawRectangle(cmd, modulation_mode);
      y = (y + tc.height + 1) % 500;
    }
  };

  for (const GPUTextureMode texture_mode : TEXTURE_MODES)
  {
    for (const TextureModulationMode modulation_mode : TEXTURED_MODULATION_MODES)
      draw(texture_mode, modulation_mode);
  }
  for (u32 i = 0; i < 4; i++)
    draw(GPUTextureMode::Direct16Bit, TextureModulationMode::Disabled);

  CheckVRAM();
}

TEST_F(GPUSWRasterizer, DrawQuad)
{
  struct TestCase
  {
    s32 x;
    u16 width, height;
    u8 u, v, texture_page;
    bool texture_window, transparency, check_mask, set_mask, clip;
  };
  static constexpr TestCase test_cases[] = {
    {300, 37, 6, 0, 0, FIRST_TEXTURE_PAGE + 1, false, false, false, false, false},    // aligned
    {301, 38, 7, 13, 200, FIRST_TEXTURE_PAGE + 2, false, false, false, false, false}, // unaligned
    {311, 70, 5, 180, 30, 15, false, false, false, false, false},                     // page wraps around VRAM
    {305, 33, 5, 7, 7, FIRST_TEXTURE_PAGE + 4, true, false, false, false, false},     // texture window
    {303, 50, 6, 100, 100, FIRST_TEXTURE_PAGE + 5, false, true, false, false, false}, // semi-transparent
    {299, 21, 5, 31, 200, FIRST_TEXTURE_PAGE + 6, false, true, true, true, false},    // mask test and set
    {297, 61, 9, 90, 20, FIRST_TEXTURE_PAGE + 2, false, false, false, false, true},   // clipped on all sides
    {250, 20, 5, 90, 20, FIRST_TEXTURE_PAGE + 3, false, false, false, false, false},  // clipped on the left
    {500, 20, 5, 90, 20, FIRST_TEXTURE_PAGE + 3, false, false, false, false, false},  // clipped on the right
    {400, 1, 1, 77, 88, FIRST_TEXTURE_PAGE + 3, false, true, false, false, false},    // single pixel
  };

  s32 y = 3;
  u8 transparency_mode = 0;
  const auto draw = [&](GPUTextureMode texture_mode, TextureModulationMode modulation_mode, bool shading_enable,
                        bool dither_enable) {
    for (const TestCase& tc : test_cases)
    {
      GPUBackendDrawCommand cmd = {};
      cmd.shading_enable = shading_enable;
      cmd.dither_enable = dither_enable;
      cmd.transparency_enable = tc.transparency;
      cmd.check_mask_before_draw = tc.check_mask;
      cmd.set_mask_while_drawing = tc.set_mask;
      cmd.draw_mode.texture_page = static_cast<u8>(tc.texture_page | ((y & 0x100) ? 0x00 : 0x10));
      cmd.draw_mode.texture_mode = texture_mode;
      if (tc.transparency)
        cmd.draw_mode.transparency_mode = static_cast<GPUTransparencyMode>((transparency_mode++) & 3);
      cmd.window.and_x = tc.texture_window ? 0xE7 : 0xFF;
      cmd.window.and_y = tc.texture_window ? 0xF8 : 0xFF;
      cmd.window.or_x = tc.texture_window ? 0x08 : 0x00;
      cmd.window.or_y = tc.texture_window ? 0x10 : 0x00;

      GPU_SW_Rasterizer::g_drawing_area =
        tc.clip ? GPUDrawingArea{static_cast<u32>(tc.x) + 5, static_cast<u32>(y) + 2,
                                 static_cast<u32>(tc.x) + tc.width - 7, static_cast<u32>(y) + tc.height - 3} :
                  GPUDrawingArea{DRAW_AREA_LEFT, 0, DRAW_AREA_RIGHT, VRAM_HEIGHT - 1};
      DrawQuad(cmd, modulation_mode, tc.x, y, tc.width, tc.height,
               (0x80C040u + (static_cast<u32>(y) * 0x030507u)) & 0xFFFFFFu, tc.u, tc.v);
      y = (y + tc.height + 1) % 500;
    }
  };

  // The colour is the same at every vertex, so shading only changes how it gets there.
  for (const bool shading_enable : {false, true})
  {
    for (const bool dither_enable : {false, true})
    {
      for (const GPUTextureMode texture_mode : TEXTURE_MODES)
      {
        for (const TextureModulationMode modulation_mode : TEXTURED_MODULATION_MODES)
          draw(texture_mode, modulation_mode, shading_enable, dither_enable);
      }

      draw(GPUTextureMode::Direct16Bit, TextureModulationMode::Disabled, shading_enable, dither_enable);
    }
  }

  CheckVRAM();
}

TEST_F(GPUSWRasterizer, DrawTriangleClipping)
{
  // Clipping a triangle should result in the same pixels as drawing all of it, for those inside the drawing area.
  XorShift128PlusPlus rng(0x9ABC);
  const auto random = [&rng](u32 min, u32 max) { return min + static_cast<u32>(rng.Next() % (max - min + 1)); };

  std::vector<u16> unclipped_vram(VRAM_WIDTH * VRAM_HEIGHT);
  for (u32 i = 0; i < 96; i++)
  {
    GPUBackendDrawCommand cmd = {};
    cmd.shading_enable = ((i & 1) != 0);
    cmd.dither_enable = ((i & 2) != 0);
    cmd.transparency_enable = ((i & 4) != 0);
    cmd.check_mask_before_draw = ((i & 8) != 0);
    cmd.draw_mode.texture_page = static_cast<u8>(random(FIRST_TEXTURE_PAGE, 15) | (random(0, 1) << 4));
    cmd.draw_mode.texture_mode = TEXTURE_MODES[i % 3];
    cmd.draw_mode.transparency_mode = static_cast<GPUTransparencyMode>(random(0, 3));
    cmd.window.and_x = 0xFF;
    cmd.window.and_y = 0xFF;

    const TextureModulationMode modulation_mode = static_cast<TextureModulationMode>(random(0, 3));
    const s32 size = (i < 48) ? 24 : 120;
    const s32 origin_x = static_cast<s32>(random(DRAW_AREA_LEFT - 20, DRAW_AREA_RIGHT - 60));
    const s32 origin_y = static_cast<s32>(random(0, 300));
    Vertex vertices[3];
    for (Vertex& v : vertices)
    {
      v.x = origin_x + static_cast<s32>(random(0, size));
      v.y = origin_y + static_cast<s32>(random(0, size));
      v.color = cmd.shading_enable ? static_cast<u32>(rng.Next() & 0xFFFFFFu) : 0x4080C0u;
      v.texcoord = static_cast<u16>(rng.Next());
    }

    // Some of these end up empty, or do not intersect the triangle at all.
    const GPUDrawingArea clipped_area = {
      std::max(static_cast<u32>(origin_x) + random(0, size), DRAW_AREA_LEFT), origin_y + random(0, size),
      std::min(static_cast<u32>(origin_x) + random(0, size), DRAW_AREA_RIGHT), origin_y + random(0, size)};

    std::memcpy(g_vram, m_initial_vram.data(), sizeof(g_vram));
    GPU_SW_Rasterizer::g_drawing_area = {DRAW_AREA_LEFT, 0, DRAW_AREA_RIGHT, VRAM_HEIGHT - 1};
    DrawTriangle(cmd, modulation_mode, vertices[0], vertices[1], vertices[2]);
    std::memcpy(unclipped_vram.data(), g_vram, sizeof(g_vram));

    std::memcpy(g_vram, m_initial_vram.data(), sizeof(g_vram));
    GPU_SW_Rasterizer::g_drawing_area = clipped_area;
    DrawTriangle(cmd, modulation_mode, vertices[0], vertices[1], vertices[2]);

    for (u32 py = 0; py < VRAM_HEIGHT; py++)
    {
      for (u32 px = 0; px < VRAM_WIDTH; px++)
      {
        const u32 index = GetVRAMIndex(px, py);
        m_expected_vram[index] = IsInsideDrawingArea(static_cast<s32>(px), static_cast<s32>(py)) ?
                                   unclipped_vram[index] :
                                   m_initial_vram[index];
      }
    }

    CheckVRAM();
    if (HasFailure())
    {
      ADD_FAILURE() << "In triangle " << i;
      break;
    }
  }
}
