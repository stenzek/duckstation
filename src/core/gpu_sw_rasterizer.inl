// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#ifdef __INTELLISENSE__

#include "gpu.h"

#include "common/gsvector.h"

#include <algorithm>

#define USE_VECTOR 1
#define GSVECTOR_HAS_SRLV 1
#define GSVECTOR_HAS_256 1

extern GPU_SW_Rasterizer::DitherLUT g_dither_lut;

namespace GPU_SW_Rasterizer {

#endif

// TODO: UpdateVRAM, FillVRAM, etc.

#ifdef USE_VECTOR
// #define CHECK_VECTOR
#ifdef CHECK_VECTOR
static u16 s_vram_backup[VRAM_WIDTH * VRAM_HEIGHT];
static u16 s_new_vram[VRAM_WIDTH * VRAM_HEIGHT];
static u32 s_bad_counter = 0;
#define BACKUP_VRAM()                                                                                                  \
  do                                                                                                                   \
  {                                                                                                                    \
    std::memcpy(s_vram_backup, g_vram, sizeof(g_vram));                                                                \
    s_bad_counter++;                                                                                                   \
  } while (0)
#define CHECK_VRAM(drawer)                                                                                             \
  do                                                                                                                   \
  {                                                                                                                    \
    std::memcpy(s_new_vram, g_vram, sizeof(g_vram));                                                                   \
    std::memcpy(g_vram, s_vram_backup, sizeof(g_vram));                                                                \
                                                                                                                       \
    drawer;                                                                                                            \
    for (u32 vidx = 0; vidx < (VRAM_WIDTH * VRAM_HEIGHT); vidx++)                                                      \
    {                                                                                                                  \
      if (s_new_vram[vidx] != g_vram[vidx])                                                                            \
      {                                                                                                                \
        fprintf(stderr, "[%u] Mismatch at %d,%d, expected %04x got %04x\n", s_bad_counter, (vidx % VRAM_WIDTH),        \
                (vidx / VRAM_WIDTH), g_vram[vidx], s_new_vram[vidx]);                                                  \
        AssertMsg(false, "Mismatch");                                                                                  \
      }                                                                                                                \
    }                                                                                                                  \
    /*Assert(std::memcmp(g_vram, s_new_vram, sizeof(g_vram)) == 0)*/                                                   \
  } while (0)
#endif
#endif

[[maybe_unused]] ALWAYS_INLINE_RELEASE static u16 GetPixel(const u32 x, const u32 y)
{
  return g_vram[VRAM_WIDTH * y + x];
}
[[maybe_unused]] ALWAYS_INLINE_RELEASE static u16* GetPixelPtr(const u32 x, const u32 y)
{
  return &g_vram[VRAM_WIDTH * y + x];
}
[[maybe_unused]] ALWAYS_INLINE_RELEASE static void SetPixel(const u32 x, const u32 y, const u16 value)
{
  g_vram[VRAM_WIDTH * y + x] = value;
}

[[maybe_unused]] ALWAYS_INLINE_RELEASE static constexpr std::tuple<u8, u8> UnpackTexcoord(u16 texcoord)
{
  return std::make_tuple(static_cast<u8>(texcoord), static_cast<u8>(texcoord >> 8));
}

[[maybe_unused]] ALWAYS_INLINE_RELEASE static constexpr std::tuple<u8, u8, u8> UnpackColorRGB24(u32 rgb24)
{
  return std::make_tuple(static_cast<u8>(rgb24), static_cast<u8>(rgb24 >> 8), static_cast<u8>(rgb24 >> 16));
}

template<bool texture_enable, bool raw_texture_enable, bool transparency_enable>
[[maybe_unused]] ALWAYS_INLINE_RELEASE static void ShadePixel(const GPUBackendDrawCommand* cmd, u32 x, u32 y,
                                                              u8 color_r, u8 color_g, u8 color_b, u8 texcoord_x,
                                                              u8 texcoord_y)
{
  u16 color;
  if constexpr (texture_enable)
  {
    // Apply texture window
    texcoord_x = (texcoord_x & cmd->window.and_x) | cmd->window.or_x;
    texcoord_y = (texcoord_y & cmd->window.and_y) | cmd->window.or_y;

    u16 texture_color;
    switch (cmd->draw_mode.texture_mode)
    {
      case GPUTextureMode::Palette4Bit:
      {
        const u16 palette_value =
          GetPixel((cmd->draw_mode.GetTexturePageBaseX() + ZeroExtend32(texcoord_x / 4)) % VRAM_WIDTH,
                   (cmd->draw_mode.GetTexturePageBaseY() + ZeroExtend32(texcoord_y)) % VRAM_HEIGHT);
        const size_t palette_index = (palette_value >> ((texcoord_x % 4) * 4)) & 0x0Fu;
        texture_color = g_gpu_clut[palette_index];
      }
      break;

      case GPUTextureMode::Palette8Bit:
      {
        const u16 palette_value =
          GetPixel((cmd->draw_mode.GetTexturePageBaseX() + ZeroExtend32(texcoord_x / 2)) % VRAM_WIDTH,
                   (cmd->draw_mode.GetTexturePageBaseY() + ZeroExtend32(texcoord_y)) % VRAM_HEIGHT);
        const size_t palette_index = (palette_value >> ((texcoord_x % 2) * 8)) & 0xFFu;
        texture_color = g_gpu_clut[palette_index];
      }
      break;

      default:
      {
        texture_color = GetPixel((cmd->draw_mode.GetTexturePageBaseX() + ZeroExtend32(texcoord_x)) % VRAM_WIDTH,
                                 (cmd->draw_mode.GetTexturePageBaseY() + ZeroExtend32(texcoord_y)) % VRAM_HEIGHT);
      }
      break;
    }

    if (texture_color == 0)
      return;

    if constexpr (raw_texture_enable)
    {
      color = texture_color;
    }
    else
    {
      const bool dithering_enable = cmd->draw_mode.dither_enable;
      const u32 dither_y = (dithering_enable) ? (y & 3u) : 2u;
      const u32 dither_x = (dithering_enable) ? (x & 3u) : 3u;

      color =
        (ZeroExtend16(g_dither_lut[dither_y][dither_x][(u16(texture_color & 0x1Fu) * u16(color_r)) >> 4]) << 0) |
        (ZeroExtend16(g_dither_lut[dither_y][dither_x][(u16((texture_color >> 5) & 0x1Fu) * u16(color_g)) >> 4]) << 5) |
        (ZeroExtend16(g_dither_lut[dither_y][dither_x][(u16((texture_color >> 10) & 0x1Fu) * u16(color_b)) >> 4])
         << 10) |
        (texture_color & 0x8000u);
    }
  }
  else
  {
    const bool dithering_enable = cmd->draw_mode.dither_enable;
    const u32 dither_y = (dithering_enable) ? (y & 3u) : 2u;
    const u32 dither_x = (dithering_enable) ? (x & 3u) : 3u;

    // Non-textured transparent polygons don't set bit 15, but are treated as transparent.
    color = (ZeroExtend16(g_dither_lut[dither_y][dither_x][color_r]) << 0) |
            (ZeroExtend16(g_dither_lut[dither_y][dither_x][color_g]) << 5) |
            (ZeroExtend16(g_dither_lut[dither_y][dither_x][color_b]) << 10) | (transparency_enable ? 0x8000u : 0);
  }

  const u16 bg_color = GetPixel(static_cast<u32>(x), static_cast<u32>(y));
  if constexpr (transparency_enable)
  {
    if (color & 0x8000u || !texture_enable)
    {
      // Based on blargg's efficient 15bpp pixel math.
      u32 bg_bits = ZeroExtend32(bg_color);
      u32 fg_bits = ZeroExtend32(color);
      switch (cmd->draw_mode.transparency_mode)
      {
        case GPUTransparencyMode::HalfBackgroundPlusHalfForeground:
        {
          bg_bits |= 0x8000u;
          color = Truncate16(((fg_bits + bg_bits) - ((fg_bits ^ bg_bits) & 0x0421u)) >> 1);
        }
        break;

        case GPUTransparencyMode::BackgroundPlusForeground:
        {
          bg_bits &= ~0x8000u;

          const u32 sum = fg_bits + bg_bits;
          const u32 carry = (sum - ((fg_bits ^ bg_bits) & 0x8421u)) & 0x8420u;

          color = Truncate16((sum - carry) | (carry - (carry >> 5)));
        }
        break;

        case GPUTransparencyMode::BackgroundMinusForeground:
        {
          bg_bits |= 0x8000u;
          fg_bits &= ~0x8000u;

          const u32 diff = bg_bits - fg_bits + 0x108420u;
          const u32 borrow = (diff - ((bg_bits ^ fg_bits) & 0x108420u)) & 0x108420u;

          color = Truncate16((diff - borrow) & (borrow - (borrow >> 5)));
        }
        break;

        case GPUTransparencyMode::BackgroundPlusQuarterForeground:
        {
          bg_bits &= ~0x8000u;
          fg_bits = ((fg_bits >> 2) & 0x1CE7u) | 0x8000u;

          const u32 sum = fg_bits + bg_bits;
          const u32 carry = (sum - ((fg_bits ^ bg_bits) & 0x8421u)) & 0x8420u;

          color = Truncate16((sum - carry) | (carry - (carry >> 5)));
        }
        break;

        default:
          break;
      }

      // See above.
      if constexpr (!texture_enable)
        color &= ~0x8000u;
    }
  }

  const u16 mask_and = cmd->params.GetMaskAND();
  if ((bg_color & mask_and) != 0)
    return;

  DebugAssert(static_cast<u32>(x) < VRAM_WIDTH && static_cast<u32>(y) < VRAM_HEIGHT);
  SetPixel(static_cast<u32>(x), static_cast<u32>(y), color | cmd->params.GetMaskOR());
}

#ifndef USE_VECTOR

template<bool texture_enable, bool raw_texture_enable, bool transparency_enable>
static void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd)
{
  const s32 origin_x = cmd->x;
  const s32 origin_y = cmd->y;
  const auto [r, g, b] = UnpackColorRGB24(cmd->color);
  const auto [origin_texcoord_x, origin_texcoord_y] = UnpackTexcoord(cmd->texcoord);

  for (u32 offset_y = 0; offset_y < cmd->height; offset_y++)
  {
    const s32 y = origin_y + static_cast<s32>(offset_y);
    if (y < static_cast<s32>(g_drawing_area.top) || y > static_cast<s32>(g_drawing_area.bottom) ||
        (cmd->params.interlaced_rendering && cmd->params.active_line_lsb == (Truncate8(static_cast<u32>(y)) & 1u)))
    {
      continue;
    }

    const u32 draw_y = static_cast<u32>(y) & VRAM_HEIGHT_MASK;
    const u8 texcoord_y = Truncate8(ZeroExtend32(origin_texcoord_y) + offset_y);

    for (u32 offset_x = 0; offset_x < cmd->width; offset_x++)
    {
      const s32 x = origin_x + static_cast<s32>(offset_x);
      if (x < static_cast<s32>(g_drawing_area.left) || x > static_cast<s32>(g_drawing_area.right))
        continue;

      const u8 texcoord_x = Truncate8(ZeroExtend32(origin_texcoord_x) + offset_x);

      ShadePixel<texture_enable, raw_texture_enable, transparency_enable>(cmd, static_cast<u32>(x), draw_y, r, g, b,
                                                                          texcoord_x, texcoord_y);
    }
  }
}

#else // USE_VECTOR

#ifdef GSVECTOR_HAS_256
using GSVectorNi = GSVector8i;
static constexpr GSVector8i SPAN_OFFSET_VEC = GSVector8i::cxpr(0, 1, 2, 3, 4, 5, 6, 7);
static constexpr GSVector8i SPAN_WIDTH_VEC = GSVector8i::cxpr(1, 2, 3, 4, 5, 6, 7, 8);
static constexpr GSVector8i PIXELS_PER_VEC_VEC = GSVector8i::cxpr(8);
static constexpr u32 PIXELS_PER_VEC = 8;
#else
using GSVectorNi = GSVector4i;
static constexpr GSVector4i SPAN_OFFSET_VEC = GSVector4i::cxpr(0, 1, 2, 3);
static constexpr GSVector4i SPAN_WIDTH_VEC = GSVector4i::cxpr(1, 2, 3, 4);
static constexpr GSVector4i PIXELS_PER_VEC_VEC = GSVector4i::cxpr(4);
static constexpr u32 PIXELS_PER_VEC = 4;
#endif

#ifdef GSVECTOR_HAS_256

ALWAYS_INLINE_RELEASE static GSVector8i GatherVector(GSVector8i coord_x, GSVector8i coord_y)
{
  const GSVector8i offsets = coord_y.sll32<10>().add32(coord_x); // y * 1024 + x
  GSVector8i pixels = GSVector8i::zext32(g_vram[static_cast<u32>(offsets.extract32<0>())]);
  pixels = pixels.insert16<2>(g_vram[static_cast<u32>(offsets.extract32<1>())]);
  pixels = pixels.insert16<4>(g_vram[static_cast<u32>(offsets.extract32<2>())]);
  pixels = pixels.insert16<6>(g_vram[static_cast<u32>(offsets.extract32<3>())]);
  pixels = pixels.insert16<8>(g_vram[static_cast<u32>(offsets.extract32<4>())]);
  pixels = pixels.insert16<10>(g_vram[static_cast<u32>(offsets.extract32<5>())]);
  pixels = pixels.insert16<12>(g_vram[static_cast<u32>(offsets.extract32<6>())]);
  pixels = pixels.insert16<14>(g_vram[static_cast<u32>(offsets.extract32<7>())]);
  return pixels;
}

template<u32 mask>
ALWAYS_INLINE_RELEASE static GSVector8i GatherCLUTVector(GSVector8i indices, GSVector8i shifts)
{
  const GSVector8i offsets = indices.srlv32(shifts) & GSVector8i::cxpr(mask);
  GSVector8i pixels = GSVector8i::zext32(g_gpu_clut[static_cast<u32>(offsets.extract32<0>())]);
  pixels = pixels.insert16<2>(g_gpu_clut[static_cast<u32>(offsets.extract32<1>())]);
  pixels = pixels.insert16<4>(g_gpu_clut[static_cast<u32>(offsets.extract32<2>())]);
  pixels = pixels.insert16<6>(g_gpu_clut[static_cast<u32>(offsets.extract32<3>())]);
  pixels = pixels.insert16<8>(g_gpu_clut[static_cast<u32>(offsets.extract32<4>())]);
  pixels = pixels.insert16<10>(g_gpu_clut[static_cast<u32>(offsets.extract32<5>())]);
  pixels = pixels.insert16<12>(g_gpu_clut[static_cast<u32>(offsets.extract32<6>())]);
  pixels = pixels.insert16<14>(g_gpu_clut[static_cast<u32>(offsets.extract32<7>())]);
  return pixels;
}

ALWAYS_INLINE_RELEASE static GSVector8i LoadVector(u32 x, u32 y)
{
  // TODO: Split into high/low
  if (x <= (VRAM_WIDTH - 8))
  {
    return GSVector8i::u16to32(GSVector4i::load<false>(&g_vram[y * VRAM_WIDTH + x]));
  }
  else
  {
    // TODO: Avoid loads for masked pixels if a contiguous region is masked
    const u16* line = &g_vram[y * VRAM_WIDTH];
    GSVector8i pixels = GSVector8i::zero();
    pixels = pixels.insert16<0>(line[(x++) & VRAM_WIDTH_MASK]);
    pixels = pixels.insert16<2>(line[(x++) & VRAM_WIDTH_MASK]);
    pixels = pixels.insert16<4>(line[(x++) & VRAM_WIDTH_MASK]);
    pixels = pixels.insert16<6>(line[(x++) & VRAM_WIDTH_MASK]);
    pixels = pixels.insert16<8>(line[(x++) & VRAM_WIDTH_MASK]);
    pixels = pixels.insert16<10>(line[(x++) & VRAM_WIDTH_MASK]);
    pixels = pixels.insert16<12>(line[(x++) & VRAM_WIDTH_MASK]);
    pixels = pixels.insert16<14>(line[(x++) & VRAM_WIDTH_MASK]);
    return pixels;
  }
}

ALWAYS_INLINE_RELEASE static void StoreVector(u32 x, u32 y, GSVector8i color)
{
  // TODO: Split into high/low
  const GSVector4i packed = color.low128().pu32(color.high128());
  if (x <= (VRAM_WIDTH - 8))
  {
    GSVector4i::store<false>(&g_vram[y * VRAM_WIDTH + x], packed);
  }
  else
  {
    // TODO: Avoid stores for masked pixels if a contiguous region is masked
    u16* line = &g_vram[y * VRAM_WIDTH];
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(packed.extract16<0>());
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(packed.extract16<1>());
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(packed.extract16<2>());
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(packed.extract16<3>());
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(packed.extract16<4>());
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(packed.extract16<5>());
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(packed.extract16<6>());
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(packed.extract16<7>());
  }
}

#else

ALWAYS_INLINE_RELEASE static GSVector4i GatherVector(GSVector4i coord_x, GSVector4i coord_y)
{
  const GSVector4i offsets = coord_y.sll32<10>().add32(coord_x); // y * 1024 + x

  // Clang seems to optimize this directly into pextrd+pinsrw, good.
  GSVector4i pixels = GSVector4i::zext32(g_vram[static_cast<u32>(offsets.extract32<0>())]);
  pixels = pixels.insert16<2>(g_vram[static_cast<u32>(offsets.extract32<1>())]);
  pixels = pixels.insert16<4>(g_vram[static_cast<u32>(offsets.extract32<2>())]);
  pixels = pixels.insert16<6>(g_vram[static_cast<u32>(offsets.extract32<3>())]);

  return pixels;
}

template<u32 mask>
ALWAYS_INLINE_RELEASE static GSVector4i GatherCLUTVector(GSVector4i indices, GSVector4i shifts)
{
#ifdef GSVECTOR_HAS_SRLV
  // On everywhere except RISC-V, we can do the shl 1 (* 2) as part of the load instruction.
  const GSVector4i offsets = indices.srlv32(shifts) & GSVector4i::cxpr(mask);
  GSVector4i pixels = GSVector4i::zext32(g_gpu_clut[static_cast<u32>(offsets.extract32<0>())]);
  pixels = pixels.insert16<2>(g_gpu_clut[static_cast<u32>(offsets.extract32<1>())]);
  pixels = pixels.insert16<4>(g_gpu_clut[static_cast<u32>(offsets.extract32<2>())]);
  pixels = pixels.insert16<6>(g_gpu_clut[static_cast<u32>(offsets.extract32<3>())]);
  return pixels;
#else
  // Without variable shifts, it's probably quicker to do it without vectors.
  // Because otherwise we have to do 4 separate vector shifts, as well as broadcasting the shifts...
  // Clang seems to turn this into a bunch of extracts, and skips memory. Nice.
  alignas(VECTOR_ALIGNMENT) s32 indices_array[4], shifts_array[4];
  GSVector4i::store<true>(indices_array, indices);
  GSVector4i::store<true>(shifts_array, shifts);

  GSVector4i pixels = GSVector4i::zext32(g_gpu_clut[((indices_array[0] >> shifts_array[0]) & mask)]);
  pixels = pixels.insert16<2>(g_gpu_clut[((indices_array[1] >> shifts_array[1]) & mask)]);
  pixels = pixels.insert16<4>(g_gpu_clut[((indices_array[2] >> shifts_array[2]) & mask)]);
  pixels = pixels.insert16<6>(g_gpu_clut[((indices_array[3] >> shifts_array[3]) & mask)]);
  return pixels;
#endif
}

ALWAYS_INLINE_RELEASE static GSVector4i LoadVector(u32 x, u32 y)
{
  if (x <= (VRAM_WIDTH - 4))
  {
    return GSVector4i::loadl<false>(&g_vram[y * VRAM_WIDTH + x]).u16to32();
  }
  else
  {
    const u16* line = &g_vram[y * VRAM_WIDTH];
    GSVector4i pixels = GSVector4i(line[(x++) & VRAM_WIDTH_MASK]);
    pixels = pixels.insert16<2>(line[(x++) & VRAM_WIDTH_MASK]);
    pixels = pixels.insert16<4>(line[(x++) & VRAM_WIDTH_MASK]);
    pixels = pixels.insert16<6>(line[x & VRAM_WIDTH_MASK]);
    return pixels;
  }
}

ALWAYS_INLINE_RELEASE static void StoreVector(u32 x, u32 y, GSVector4i color)
{
  const GSVector4i packed_color = color.pu32();
  if (x <= (VRAM_WIDTH - 4))
  {
    GSVector4i::storel<false>(&g_vram[y * VRAM_WIDTH + x], packed_color);
  }
  else
  {
    u16* line = &g_vram[y * VRAM_WIDTH];
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(packed_color.extract16<0>());
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(packed_color.extract16<1>());
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(packed_color.extract16<2>());
    line[x & VRAM_WIDTH_MASK] = Truncate16(packed_color.extract16<3>());
  }
}

#endif

ALWAYS_INLINE_RELEASE static void RGB5A1ToRG_BA(GSVectorNi rgb5a1, GSVectorNi& rg, GSVectorNi& ba)
{
  rg = rgb5a1 & GSVectorNi::cxpr(0x1F);                     // R | R | R | R
  rg = rg | (rgb5a1 & GSVectorNi::cxpr(0x3E0)).sll32<11>(); // R0G0 | R0G0 | R0G0 | R0G0
  ba = rgb5a1.srl32<10>() & GSVectorNi::cxpr(0x1F);         // B | B | B | B
  ba = ba | (rgb5a1 & GSVectorNi::cxpr(0x8000)).sll32<1>(); // B0A0 | B0A0 | B0A0 | B0A0
}

ALWAYS_INLINE_RELEASE static GSVectorNi RG_BAToRGB5A1(GSVectorNi rg, GSVectorNi ba)
{
  GSVectorNi res;

  res = rg & GSVectorNi::cxpr(0x1F);                       // R | R | R | R
  res = res | (rg.srl32<11>() & GSVectorNi::cxpr(0x3E0));  // RG | RG | RG | RG
  res = res | ((ba & GSVectorNi::cxpr(0x1F)).sll32<10>()); // RGB | RGB | RGB | RGB
  res = res | ba.srl32<16>().sll32<15>();                  // RGBA | RGBA | RGBA | RGBA

  return res;
}

// Color repeated twice for RG packing, then duplicated to we can load based on the X offset.
alignas(VECTOR_ALIGNMENT) static constexpr s16 VECTOR_DITHER_MATRIX[4][16] = {
#define P(m, n) static_cast<s16>(DITHER_MATRIX[m][n]), static_cast<s16>(DITHER_MATRIX[m][n])
#define R(m) P(m, 0), P(m, 1), P(m, 2), P(m, 3), P(m, 0), P(m, 1), P(m, 2), P(m, 3)

  {R(0)}, {R(1)}, {R(2)}, {R(3)}

#undef R
#undef P
};

namespace {
template<bool texture_enable>
struct PixelVectors
{
  struct UnusedField
  {
  };

  GSVectorNi clip_left;
  GSVectorNi clip_right;

  GSVectorNi mask_and;
  GSVectorNi mask_or;

  typename std::conditional_t<texture_enable, GSVectorNi, UnusedField> texture_window_and_x;
  typename std::conditional_t<texture_enable, GSVectorNi, UnusedField> texture_window_or_x;
  typename std::conditional_t<texture_enable, GSVectorNi, UnusedField> texture_window_and_y;
  typename std::conditional_t<texture_enable, GSVectorNi, UnusedField> texture_window_or_y;
  typename std::conditional_t<texture_enable, GSVectorNi, UnusedField> texture_base_x;
  typename std::conditional_t<texture_enable, GSVectorNi, UnusedField> texture_base_y;

  PixelVectors(const GPUBackendDrawCommand* cmd)
  {
    clip_left = GSVectorNi(g_drawing_area.left);
    clip_right = GSVectorNi(g_drawing_area.right);

    mask_and = GSVectorNi(cmd->params.GetMaskAND());
    mask_or = GSVectorNi(cmd->params.GetMaskOR());

    if constexpr (texture_enable)
    {
      texture_window_and_x = GSVectorNi(cmd->window.and_x);
      texture_window_or_x = GSVectorNi(cmd->window.or_x);
      texture_window_and_y = GSVectorNi(cmd->window.and_y);
      texture_window_or_y = GSVectorNi(cmd->window.or_y);
      texture_base_x = GSVectorNi(cmd->draw_mode.GetTexturePageBaseX());
      texture_base_y = GSVectorNi(cmd->draw_mode.GetTexturePageBaseY());
    }
  }
};
} // namespace

template<bool texture_enable, bool raw_texture_enable, bool transparency_enable>
ALWAYS_INLINE_RELEASE static void
ShadePixel(const PixelVectors<texture_enable>& pv, GPUTextureMode texture_mode, GPUTransparencyMode transparency_mode,
           u32 start_x, u32 y, GSVectorNi vertex_color_rg, GSVectorNi vertex_color_ba, GSVectorNi texcoord_x,
           GSVectorNi texcoord_y, GSVectorNi preserve_mask, GSVectorNi dither)
{
  static constexpr GSVectorNi coord_mask_x = GSVectorNi::cxpr(VRAM_WIDTH_MASK);
  static constexpr GSVectorNi coord_mask_y = GSVectorNi::cxpr(VRAM_HEIGHT_MASK);

  GSVectorNi color;

  if constexpr (texture_enable)
  {
    // Apply texture window
    texcoord_x = (texcoord_x & pv.texture_window_and_x) | pv.texture_window_or_x;
    texcoord_y = (texcoord_y & pv.texture_window_and_y) | pv.texture_window_or_y;

    texcoord_y = pv.texture_base_y.add32(texcoord_y) & coord_mask_y;

    GSVectorNi texture_color;
    switch (texture_mode)
    {
      case GPUTextureMode::Palette4Bit:
      {
        GSVectorNi load_texcoord_x = texcoord_x.srl32<2>();
        load_texcoord_x = pv.texture_base_x.add32(load_texcoord_x);
        load_texcoord_x = load_texcoord_x & coord_mask_x;

        const GSVectorNi palette_shift = (texcoord_x & GSVectorNi::cxpr(3)).sll32<2>();
        const GSVectorNi palette_indices = GatherVector(load_texcoord_x, texcoord_y);
        texture_color = GatherCLUTVector<0x0F>(palette_indices, palette_shift);
      }
      break;

      case GPUTextureMode::Palette8Bit:
      {
        GSVectorNi load_texcoord_x = texcoord_x.srl32<1>();
        load_texcoord_x = pv.texture_base_x.add32(load_texcoord_x);
        load_texcoord_x = load_texcoord_x & coord_mask_x;

        const GSVectorNi palette_shift = (texcoord_x & GSVectorNi::cxpr(1)).sll32<3>();
        const GSVectorNi palette_indices = GatherVector(load_texcoord_x, texcoord_y);
        texture_color = GatherCLUTVector<0xFF>(palette_indices, palette_shift);
      }
      break;

      default:
      {
        texcoord_x = pv.texture_base_x.add32(texcoord_x);
        texcoord_x = texcoord_x & coord_mask_x;
        texture_color = GatherVector(texcoord_x, texcoord_y);
      }
      break;
    }

    // check for zero texture colour across the 4 pixels, early out if so
    const GSVectorNi texture_transparent_mask = texture_color.eq32(GSVectorNi::zero());
    if (texture_transparent_mask.alltrue())
      return;

    preserve_mask = preserve_mask | texture_transparent_mask;

    if constexpr (raw_texture_enable)
    {
      color = texture_color;
    }
    else
    {
      GSVectorNi trg, tba;
      RGB5A1ToRG_BA(texture_color, trg, tba);

      // now we have both the texture and vertex color in RG/GA pairs, for 4 pixels, which we can multiply
      GSVectorNi rg = trg.mul16l(vertex_color_rg);
      GSVectorNi ba = tba.mul16l(vertex_color_ba);

      // Convert to 5bit.
      rg = rg.sra16<4>().add16(dither).max_s16(GSVectorNi::zero()).sra16<3>();
      ba = ba.sra16<4>().add16(dither).max_s16(GSVectorNi::zero()).sra16<3>();

      // Bit15 gets passed through as-is.
      ba = ba.blend16<0xaa>(tba);

      // Clamp to 5bit.
      static constexpr GSVectorNi colclamp = GSVectorNi::cxpr16(0x1F);
      rg = rg.min_u16(colclamp);
      ba = ba.min_u16(colclamp);

      // And interleave back to 16bpp.
      color = RG_BAToRGB5A1(rg, ba);
    }
  }
  else
  {
    // Non-textured transparent polygons don't set bit 15, but are treated as transparent.
    GSVectorNi rg = vertex_color_rg.add16(dither).max_s16(GSVectorNi::zero()).sra16<3>();
    GSVectorNi ba = vertex_color_ba.add16(dither).max_s16(GSVectorNi::zero()).sra16<3>();

    // Clamp to 5bit. We use 32bit for BA to set a to zero.
    rg = rg.min_u16(GSVectorNi::cxpr16(0x1F));
    ba = ba.min_u16(GSVectorNi::cxpr(0x1F));

    // And interleave back to 16bpp.
    color = RG_BAToRGB5A1(rg, ba);
  }

  GSVectorNi bg_color = LoadVector(start_x, y);

  if constexpr (transparency_enable)
  {
    [[maybe_unused]] GSVectorNi transparent_mask;
    if constexpr (texture_enable)
    {
      // Compute transparent_mask, ffff per lane if transparent otherwise 0000
      transparent_mask = color.sra16<15>();
    }

    // TODO: We don't need to OR color here with 0x8000 for textures.
    // 0x8000 is added to match serial path.

    GSVectorNi blended_color;
    switch (transparency_mode)
    {
      case GPUTransparencyMode::HalfBackgroundPlusHalfForeground:
      {
        const GSVectorNi fg_bits = color | GSVectorNi::cxpr(0x8000u);
        const GSVectorNi bg_bits = bg_color | GSVectorNi::cxpr(0x8000u);
        const GSVectorNi res = fg_bits.add32(bg_bits).sub32((fg_bits ^ bg_bits) & GSVectorNi::cxpr(0x0421u)).srl32<1>();
        blended_color = res & GSVectorNi::cxpr(0xffff);
      }
      break;

      case GPUTransparencyMode::BackgroundPlusForeground:
      {
        const GSVectorNi fg_bits = color | GSVectorNi::cxpr(0x8000u);
        const GSVectorNi bg_bits = bg_color & GSVectorNi::cxpr(0x7FFFu);
        const GSVectorNi sum = fg_bits.add32(bg_bits);
        const GSVectorNi carry =
          (sum.sub32((fg_bits ^ bg_bits) & GSVectorNi::cxpr(0x8421u))) & GSVectorNi::cxpr(0x8420u);
        const GSVectorNi res = sum.sub32(carry) | carry.sub32(carry.srl32<5>());
        blended_color = res & GSVectorNi::cxpr(0xffff);
      }
      break;

      case GPUTransparencyMode::BackgroundMinusForeground:
      {
        const GSVectorNi bg_bits = bg_color | GSVectorNi::cxpr(0x8000u);
        const GSVectorNi fg_bits = color & GSVectorNi::cxpr(0x7FFFu);
        const GSVectorNi diff = bg_bits.sub32(fg_bits).add32(GSVectorNi::cxpr(0x108420u));
        const GSVectorNi borrow =
          diff.sub32((bg_bits ^ fg_bits) & GSVectorNi::cxpr(0x108420u)) & GSVectorNi::cxpr(0x108420u);
        const GSVectorNi res = diff.sub32(borrow) & borrow.sub32(borrow.srl32<5>());
        blended_color = res & GSVectorNi::cxpr(0xffff);
      }
      break;

      case GPUTransparencyMode::BackgroundPlusQuarterForeground:
      default:
      {
        const GSVectorNi bg_bits = bg_color & GSVectorNi::cxpr(0x7FFFu);
        const GSVectorNi fg_bits =
          ((color | GSVectorNi::cxpr(0x8000)).srl32<2>() & GSVectorNi::cxpr(0x1CE7u)) | GSVectorNi::cxpr(0x8000u);
        const GSVectorNi sum = fg_bits.add32(bg_bits);
        const GSVectorNi carry = sum.sub32((fg_bits ^ bg_bits) & GSVectorNi::cxpr(0x8421u)) & GSVectorNi::cxpr(0x8420u);
        const GSVectorNi res = sum.sub32(carry) | carry.sub32(carry.srl32<5>());
        blended_color = res & GSVectorNi::cxpr(0xffff);
      }
      break;
    }

    // select blended pixels for transparent pixels, otherwise consider opaque
    if constexpr (texture_enable)
      color = color.blend8(blended_color, transparent_mask);
    else
      color = blended_color & GSVectorNi::cxpr(0x7fff);
  }

  GSVectorNi mask_bits_set = bg_color & pv.mask_and; // 8000 if masked else 0000
  mask_bits_set = mask_bits_set.sra16<15>();         // ffff if masked else 0000
  preserve_mask = preserve_mask | mask_bits_set;     // ffff if preserved else 0000

  bg_color = bg_color & preserve_mask;
  color = (color | pv.mask_or).andnot(preserve_mask);
  color = color | bg_color;

  StoreVector(start_x, y, color);
}

template<bool texture_enable, bool raw_texture_enable, bool transparency_enable>
static void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd)
{
  const s32 origin_x = cmd->x;
  const s32 origin_y = cmd->y;

  const GSVector4i rgba = GSVector4i(cmd->color);               // RGBA | RGBA | RGBA | RGBA
  const GSVector4i rgp = rgba.xxxxl();                          // RGRG | RGRG | RGRG | RGRG
  const GSVector4i bap = rgba.yyyyl();                          // BABA | BABA | BABA | BABA
  const GSVectorNi rg = GSVectorNi::broadcast128(rgp.u8to16()); // R0G0 | R0G0 | R0G0 | R0G0
  const GSVectorNi ba = GSVectorNi::broadcast128(bap.u8to16()); // B0A0 | B0A0 | B0A0 | B0A0

  const GSVectorNi texcoord_x = GSVectorNi(cmd->texcoord & 0xFF).add32(SPAN_OFFSET_VEC);
  GSVectorNi texcoord_y = GSVectorNi(cmd->texcoord >> 8);

  const PixelVectors<texture_enable> pv(cmd);
  const u32 width = cmd->width;

#ifdef CHECK_VECTOR
  BACKUP_VRAM();
#endif

  for (u32 offset_y = 0; offset_y < cmd->height; offset_y++)
  {
    const s32 y = origin_y + static_cast<s32>(offset_y);
    if (y >= static_cast<s32>(g_drawing_area.top) && y <= static_cast<s32>(g_drawing_area.bottom) &&
        (!cmd->params.interlaced_rendering || cmd->params.active_line_lsb != (Truncate8(static_cast<u32>(y)) & 1u)))
    {
      const s32 draw_y = (y & VRAM_HEIGHT_MASK);

      GSVectorNi row_texcoord_x = texcoord_x;
      GSVectorNi xvec = GSVectorNi(origin_x).add32(SPAN_OFFSET_VEC);
      GSVectorNi wvec = GSVectorNi(width).sub32(SPAN_WIDTH_VEC);

      for (u32 offset_x = 0; offset_x < width; offset_x += PIXELS_PER_VEC)
      {
        const s32 x = origin_x + static_cast<s32>(offset_x);

        // width test
        GSVectorNi preserve_mask = wvec.lt32(GSVectorNi::zero());

        // clip test, if all pixels are outside, skip
        preserve_mask = preserve_mask | xvec.lt32(pv.clip_left);
        preserve_mask = preserve_mask | xvec.gt32(pv.clip_right);
        if (!preserve_mask.alltrue())
        {
          ShadePixel<texture_enable, raw_texture_enable, transparency_enable>(
            pv, cmd->draw_mode.texture_mode, cmd->draw_mode.transparency_mode, x, draw_y, rg, ba, row_texcoord_x,
            texcoord_y, preserve_mask, GSVectorNi::zero());
        }

        xvec = xvec.add32(PIXELS_PER_VEC_VEC);
        wvec = wvec.sub32(PIXELS_PER_VEC_VEC);

        if constexpr (texture_enable)
          row_texcoord_x = row_texcoord_x.add32(PIXELS_PER_VEC_VEC) & GSVectorNi::cxpr(0xFF);
      }
    }

    if constexpr (texture_enable)
      texcoord_y = texcoord_y.add32(GSVectorNi::cxpr(1)) & GSVectorNi::cxpr(0xFF);
  }

#ifdef CHECK_VECTOR
  CHECK_VRAM(GPU_SW_Rasterizer::DrawRectangleFunctions[texture_enable][raw_texture_enable][transparency_enable](cmd));
#endif
}

#endif // USE_VECTOR

// TODO: Vectorize line draw.
template<bool shading_enable, bool transparency_enable>
static void DrawLine(const GPUBackendDrawLineCommand* cmd, const GPUBackendDrawLineCommand::Vertex* p0,
                     const GPUBackendDrawLineCommand::Vertex* p1)
{
  static constexpr u32 XY_SHIFT = 32;
  static constexpr u32 RGB_SHIFT = 12;
  static constexpr auto makefp_xy = [](s32 x) { return (static_cast<s64>(x) << XY_SHIFT) | (1LL << (XY_SHIFT - 1)); };
  static constexpr auto unfp_xy = [](s64 x) { return static_cast<s32>(x >> XY_SHIFT) & 2047; };
  static constexpr auto div_xy = [](s64 delta, s32 dk) {
    return ((delta << XY_SHIFT) - ((delta < 0) ? (dk - 1) : 0) + ((delta > 0) ? (dk - 1) : 0)) / dk;
  };
  static constexpr auto makefp_rgb = [](u32 c) { return (static_cast<s32>(c) << RGB_SHIFT) | (1 << (RGB_SHIFT - 1)); };
  static constexpr auto unfp_rgb = [](s32 c) { return static_cast<u8>(c >> RGB_SHIFT); };
  static constexpr auto div_rgb = [](u32 c1, u32 c0, s32 dk) {
    return ((static_cast<s32>(c1) - static_cast<s32>(c0)) << RGB_SHIFT) / dk;
  };

  const s32 i_dx = std::abs(p1->x - p0->x);
  const s32 i_dy = std::abs(p1->y - p0->y);
  const s32 k = (i_dx > i_dy) ? i_dx : i_dy;
  if (i_dx >= MAX_PRIMITIVE_WIDTH || i_dy >= MAX_PRIMITIVE_HEIGHT) [[unlikely]]
    return;

  if (p0->x >= p1->x && k > 0)
    std::swap(p0, p1);

  s64 dxdk = 0, dydk = 0;
  [[maybe_unused]] s32 drdk = 0, dgdk = 0, dbdk = 0;
  if (k != 0) [[likely]]
  {
    dxdk = div_xy(p1->x - p0->x, k);
    dydk = div_xy(p1->y - p0->y, k);
    if constexpr (shading_enable)
    {
      drdk = div_rgb(p1->r, p0->r, k);
      dgdk = div_rgb(p1->g, p0->g, k);
      dbdk = div_rgb(p1->b, p0->b, k);
    }
  }

  s64 curx = makefp_xy(p0->x) - 1024;
  s64 cury = makefp_xy(p0->y) - ((dydk < 0) ? 1024 : 0);
  [[maybe_unused]] s32 curr, curg, curb;
  if constexpr (shading_enable)
  {
    curr = makefp_rgb(p0->r);
    curg = makefp_rgb(p0->g);
    curb = makefp_rgb(p0->b);
  }

  for (s32 i = 0; i <= k; i++)
  {
    const s32 x = unfp_xy(curx);
    const s32 y = unfp_xy(cury);

    if ((!cmd->params.interlaced_rendering || cmd->params.active_line_lsb != (Truncate8(static_cast<u32>(y)) & 1u)) &&
        x >= static_cast<s32>(g_drawing_area.left) && x <= static_cast<s32>(g_drawing_area.right) &&
        y >= static_cast<s32>(g_drawing_area.top) && y <= static_cast<s32>(g_drawing_area.bottom))
    {
      const u8 r = shading_enable ? unfp_rgb(curr) : p0->r;
      const u8 g = shading_enable ? unfp_rgb(curg) : p0->g;
      const u8 b = shading_enable ? unfp_rgb(curb) : p0->b;

      ShadePixel<false, false, transparency_enable>(cmd, static_cast<u32>(x), static_cast<u32>(y) & VRAM_HEIGHT_MASK, r,
                                                    g, b, 0, 0);
    }

    curx += dxdk;
    cury += dydk;

    if constexpr (shading_enable)
    {
      curr += drdk;
      curg += dgdk;
      curb += dbdk;
    }
  }
}

// DDA triangle rasterization algorithm originally from Mednafen, rewritten and vectorized for DuckStation.
namespace {
static constexpr u32 ATTRIB_SHIFT = 12;
static constexpr u32 ATTRIB_POST_SHIFT = 12;

struct UVSteps
{
  u32 dudx;
  u32 dvdx;
  u32 dudy;
  u32 dvdy;
};

struct UVStepper
{
  u32 u;
  u32 v;

  ALWAYS_INLINE u8 GetU() const { return Truncate8(u >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)); }
  ALWAYS_INLINE u8 GetV() const { return Truncate8(v >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)); }

  ALWAYS_INLINE void Init(u32 ustart, u32 vstart)
  {
    u = (((ustart << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT);
    v = (((vstart << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT);
  }

  ALWAYS_INLINE void StepX(const UVSteps& steps)
  {
    u = u + steps.dudx;
    v = v + steps.dvdx;
  }

  ALWAYS_INLINE void StepX(const UVSteps& steps, s32 count)
  {
    u = u + static_cast<u32>(static_cast<s32>(steps.dudx) * count);
    v = v + static_cast<u32>(static_cast<s32>(steps.dvdx) * count);
  }

  template<bool upside_down>
  ALWAYS_INLINE void StepY(const UVSteps& steps)
  {
    u = upside_down ? (u - steps.dudy) : (u + steps.dudy);
    v = upside_down ? (v - steps.dvdy) : (v + steps.dvdy);
  }

  ALWAYS_INLINE void StepY(const UVSteps& steps, s32 count)
  {
    u = u + static_cast<u32>(static_cast<s32>(steps.dudy) * count);
    v = v + static_cast<u32>(static_cast<s32>(steps.dvdy) * count);
  }
};

struct RGBSteps
{
  u32 drdx;
  u32 dgdx;
  u32 dbdx;

  u32 drdy;
  u32 dgdy;
  u32 dbdy;
};

struct RGBStepper
{
  u32 r;
  u32 g;
  u32 b;

  ALWAYS_INLINE u8 GetR() const { return Truncate8(r >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)); }
  ALWAYS_INLINE u8 GetG() const { return Truncate8(g >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)); }
  ALWAYS_INLINE u8 GetB() const { return Truncate8(b >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)); }

  ALWAYS_INLINE void Init(u32 rstart, u32 gstart, u32 bstart)
  {
    r = (((rstart << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT);
    g = (((gstart << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT);
    b = (((bstart << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT);
  }

  ALWAYS_INLINE void StepX(const RGBSteps& steps)
  {
    r = r + steps.drdx;
    g = g + steps.dgdx;
    b = b + steps.dbdx;
  }

  ALWAYS_INLINE void StepX(const RGBSteps& steps, s32 count)
  {
    r = r + static_cast<u32>(static_cast<s32>(steps.drdx) * count);
    g = g + static_cast<u32>(static_cast<s32>(steps.dgdx) * count);
    b = b + static_cast<u32>(static_cast<s32>(steps.dbdx) * count);
  }

  template<bool upside_down>
  ALWAYS_INLINE void StepY(const RGBSteps& steps)
  {
    r = upside_down ? (r - steps.drdy) : (r + steps.drdy);
    g = upside_down ? (g - steps.dgdy) : (g + steps.dgdy);
    b = upside_down ? (b - steps.dbdy) : (b + steps.dbdy);
  }

  ALWAYS_INLINE void StepY(const RGBSteps& steps, s32 count)
  {
    r = r + static_cast<u32>(static_cast<s32>(steps.drdy) * count);
    g = g + static_cast<u32>(static_cast<s32>(steps.dgdy) * count);
    b = b + static_cast<u32>(static_cast<s32>(steps.dbdy) * count);
  }
};

struct TrianglePart
{
  // left/right edges
  u64 start_x[2];
  u64 step_x[2];

  s32 start_y;
  s32 end_y;

  bool fill_upside_down;
};
} // namespace

#ifndef USE_VECTOR

template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable>
static void DrawSpan(const GPUBackendDrawCommand* cmd, s32 y, s32 x_start, s32 x_bound, UVStepper uv,
                     const UVSteps& uvstep, RGBStepper rgb, const RGBSteps& rgbstep)
{
  s32 width = x_bound - x_start;
  s32 current_x = TruncateGPUVertexPosition(x_start);

  // Skip pixels outside of the scissor rectangle.
  if (current_x < static_cast<s32>(g_drawing_area.left))
  {
    const s32 delta = static_cast<s32>(g_drawing_area.left) - current_x;
    x_start += delta;
    current_x += delta;
    width -= delta;
  }

  if ((current_x + width) > (static_cast<s32>(g_drawing_area.right) + 1))
    width = static_cast<s32>(g_drawing_area.right) + 1 - current_x;

  if (width <= 0)
    return;

  if constexpr (texture_enable)
    uv.StepX(uvstep, x_start);
  if constexpr (shading_enable)
    rgb.StepX(rgbstep, x_start);

  do
  {
    ShadePixel<texture_enable, raw_texture_enable, transparency_enable>(
      cmd, static_cast<u32>(current_x), static_cast<u32>(y), rgb.GetR(), rgb.GetG(), rgb.GetB(), uv.GetU(), uv.GetV());

    current_x++;
    if constexpr (texture_enable)
      uv.StepX(uvstep);
    if constexpr (shading_enable)
      rgb.StepX(rgbstep);
  } while (--width > 0);
}

template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable>
ALWAYS_INLINE_RELEASE static void DrawTrianglePart(const GPUBackendDrawCommand* cmd, const TrianglePart& tp,
                                                   const UVStepper& uv, const UVSteps& uvstep, const RGBStepper& rgb,
                                                   const RGBSteps& rgbstep)
{
  static constexpr auto unfp_xy = [](s64 xfp) -> s32 { return static_cast<s32>(static_cast<u64>(xfp) >> 32); };

  const u64 left_x_step = tp.step_x[0];
  const u64 right_x_step = tp.step_x[1];
  const s32 end_y = tp.end_y;
  u64 left_x = tp.start_x[0];
  u64 right_x = tp.start_x[1];
  s32 current_y = tp.start_y;

  if (tp.fill_upside_down)
  {
    if (current_y <= end_y)
      return;

    UVStepper luv = uv;
    if constexpr (texture_enable)
      luv.StepY(uvstep, current_y);

    RGBStepper lrgb = rgb;
    if constexpr (shading_enable)
      lrgb.StepY(rgbstep, current_y);

    do
    {
      current_y--;
      left_x -= left_x_step;
      right_x -= right_x_step;

      const s32 y = TruncateGPUVertexPosition(current_y);
      if (y < static_cast<s32>(g_drawing_area.top))
        break;

      // Opposite direction means we need to subtract when stepping instead of adding.
      if constexpr (texture_enable)
        luv.StepY<true>(uvstep);
      if constexpr (shading_enable)
        lrgb.StepY<true>(rgbstep);

      if (y > static_cast<s32>(g_drawing_area.bottom) ||
          (cmd->params.interlaced_rendering && cmd->params.active_line_lsb == (static_cast<u32>(current_y) & 1u)))
      {
        continue;
      }

      DrawSpan<shading_enable, texture_enable, raw_texture_enable, transparency_enable>(
        cmd, y & VRAM_HEIGHT_MASK, unfp_xy(left_x), unfp_xy(right_x), luv, uvstep, lrgb, rgbstep);
    } while (current_y > end_y);
  }
  else
  {
    if (current_y >= end_y)
      return;

    UVStepper luv = uv;
    if constexpr (texture_enable)
      luv.StepY(uvstep, current_y);

    RGBStepper lrgb = rgb;
    if constexpr (shading_enable)
      lrgb.StepY(rgbstep, current_y);

    do
    {
      const s32 y = TruncateGPUVertexPosition(current_y);

      if (y > static_cast<s32>(g_drawing_area.bottom))
      {
        break;
      }
      if (y >= static_cast<s32>(g_drawing_area.top) &&
          (!cmd->params.interlaced_rendering || cmd->params.active_line_lsb != (static_cast<u32>(current_y) & 1u)))
      {
        DrawSpan<shading_enable, texture_enable, raw_texture_enable, transparency_enable>(
          cmd, y & VRAM_HEIGHT_MASK, unfp_xy(left_x), unfp_xy(right_x), luv, uvstep, lrgb, rgbstep);
      }

      current_y++;
      left_x += left_x_step;
      right_x += right_x_step;

      if constexpr (texture_enable)
        luv.StepY<false>(uvstep);
      if constexpr (shading_enable)
        lrgb.StepY<false>(rgbstep);
    } while (current_y < end_y);
  }
}

#else // USE_VECTOR

namespace {
template<bool shading_enable, bool texture_enable>
struct TriangleVectors : PixelVectors<texture_enable>
{
  using UnusedField = PixelVectors<texture_enable>::UnusedField;

  typename std::conditional_t<shading_enable, GSVectorNi, UnusedField> drdx;
  typename std::conditional_t<shading_enable, GSVectorNi, UnusedField> dgdx;
  typename std::conditional_t<shading_enable, GSVectorNi, UnusedField> dbdx;
  typename std::conditional_t<texture_enable, GSVectorNi, UnusedField> dudx;
  typename std::conditional_t<texture_enable, GSVectorNi, UnusedField> dvdx;
  typename std::conditional_t<shading_enable, GSVectorNi, UnusedField> drdx_0123;
  typename std::conditional_t<shading_enable, GSVectorNi, UnusedField> dgdx_0123;
  typename std::conditional_t<shading_enable, GSVectorNi, UnusedField> dbdx_0123;
  typename std::conditional_t<texture_enable, GSVectorNi, UnusedField> dudx_0123;
  typename std::conditional_t<texture_enable, GSVectorNi, UnusedField> dvdx_0123;

  TriangleVectors(const GPUBackendDrawCommand* cmd, const UVSteps& uvstep, const RGBSteps& rgbstep)
    : PixelVectors<texture_enable>(cmd)
  {
    if constexpr (shading_enable)
    {
      drdx = GSVectorNi(rgbstep.drdx * PIXELS_PER_VEC);
      dgdx = GSVectorNi(rgbstep.dgdx * PIXELS_PER_VEC);
      dbdx = GSVectorNi(rgbstep.dbdx * PIXELS_PER_VEC);

      drdx_0123 = GSVectorNi(rgbstep.drdx).mul32l(SPAN_OFFSET_VEC);
      dgdx_0123 = GSVectorNi(rgbstep.dgdx).mul32l(SPAN_OFFSET_VEC);
      dbdx_0123 = GSVectorNi(rgbstep.dbdx).mul32l(SPAN_OFFSET_VEC);
    }

    if constexpr (texture_enable)
    {
      dudx = GSVectorNi(uvstep.dudx * PIXELS_PER_VEC);
      dvdx = GSVectorNi(uvstep.dvdx * PIXELS_PER_VEC);
      dudx_0123 = GSVectorNi(uvstep.dudx).mul32l(SPAN_OFFSET_VEC);
      dvdx_0123 = GSVectorNi(uvstep.dvdx).mul32l(SPAN_OFFSET_VEC);
    }
  }
};
} // namespace

template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable>
ALWAYS_INLINE_RELEASE static void DrawSpan(const GPUBackendDrawCommand *cmd, s32 y, s32 x_start, s32 x_bound,
                                           UVStepper uv, const UVSteps& uvstep, RGBStepper rgb, const RGBSteps& rgbstep,
                                           const TriangleVectors<shading_enable, texture_enable>& tv)
{
  s32 width = x_bound - x_start;
  s32 current_x = TruncateGPUVertexPosition(x_start);

  // Skip pixels outside of the scissor rectangle.
  if (current_x < static_cast<s32>(g_drawing_area.left))
  {
    const s32 delta = static_cast<s32>(g_drawing_area.left) - current_x;
    x_start += delta;
    current_x += delta;
    width -= delta;
  }

  if ((current_x + width) > (static_cast<s32>(g_drawing_area.right) + 1))
    width = static_cast<s32>(g_drawing_area.right) + 1 - current_x;

  if (width <= 0)
    return;

  GSVectorNi dr, dg, db;
  if constexpr (shading_enable)
  {
    dr = GSVectorNi(rgb.r + rgbstep.drdx * x_start).add32(tv.drdx_0123);
    dg = GSVectorNi(rgb.g + rgbstep.dgdx * x_start).add32(tv.dgdx_0123);
    db = GSVectorNi(rgb.b + rgbstep.dbdx * x_start).add32(tv.dbdx_0123);
  }
  else
  {
    // precompute for flat shading
    dr = GSVectorNi(rgb.r >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT));
    dg = GSVectorNi((rgb.g >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)) << 16);
    db = GSVectorNi(rgb.b >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT));
  }

  GSVectorNi du, dv;
  if constexpr (texture_enable)
  {
    du = GSVectorNi(uv.u + uvstep.dudx * x_start).add32(tv.dudx_0123);
    dv = GSVectorNi(uv.v + uvstep.dvdx * x_start).add32(tv.dvdx_0123);
  }
  else
  {
    // Hopefully optimized out...
    du = GSVectorNi::zero();
    dv = GSVectorNi::zero();
  }

  const GSVectorNi dither = cmd->draw_mode.dither_enable ?
                              GSVectorNi::broadcast128<false>(
                                &VECTOR_DITHER_MATRIX[static_cast<u32>(y) & 3][(static_cast<u32>(current_x) & 3) * 2]) :
                              GSVectorNi::zero();

  GSVectorNi xvec = GSVectorNi(current_x).add32(SPAN_OFFSET_VEC);
  GSVectorNi wvec = GSVectorNi(width).sub32(SPAN_WIDTH_VEC);

  for (s32 count = (width + (PIXELS_PER_VEC - 1)) / PIXELS_PER_VEC; count > 0; --count)
  {
    // R000 | R000 | R000 | R000
    // R0G0 | R0G0 | R0G0 | R0G0
    const GSVectorNi r = shading_enable ? dr.srl32<ATTRIB_SHIFT + ATTRIB_POST_SHIFT>() : dr;
    const GSVectorNi g =
      shading_enable ? dg.srl32<ATTRIB_SHIFT + ATTRIB_POST_SHIFT>().sll32<16>() : dg; // get G into the correct position
    const GSVectorNi b = shading_enable ? db.srl32<ATTRIB_SHIFT + ATTRIB_POST_SHIFT>() : db;
    const GSVectorNi u = du.srl32<ATTRIB_SHIFT + ATTRIB_POST_SHIFT>();
    const GSVectorNi v = dv.srl32<ATTRIB_SHIFT + ATTRIB_POST_SHIFT>();

    const GSVectorNi rg = r.blend16<0xAA>(g);

    // mask based on what's outside the span
    GSVectorNi preserve_mask = wvec.lt32(GSVectorNi::zero());

    // clip test, if all pixels are outside, skip
    preserve_mask = preserve_mask | xvec.lt32(tv.clip_left);
    preserve_mask = preserve_mask | xvec.gt32(tv.clip_right);
    if (!preserve_mask.alltrue())
    {
      ShadePixel<texture_enable, raw_texture_enable, transparency_enable>(
        tv, cmd->draw_mode.texture_mode, cmd->draw_mode.transparency_mode, static_cast<u32>(current_x),
        static_cast<u32>(y), rg, b, u, v, preserve_mask, dither);
    }

    current_x += PIXELS_PER_VEC;

    xvec = xvec.add32(PIXELS_PER_VEC_VEC);
    wvec = wvec.sub32(PIXELS_PER_VEC_VEC);

    if constexpr (shading_enable)
    {
      dr = dr.add32(tv.drdx);
      dg = dg.add32(tv.dgdx);
      db = db.add32(tv.dbdx);
    }

    if constexpr (texture_enable)
    {
      du = du.add32(tv.dudx);
      dv = dv.add32(tv.dvdx);
    }
  }
}

template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable>
ALWAYS_INLINE_RELEASE static void DrawTrianglePart(const GPUBackendDrawCommand* cmd, const TrianglePart& tp,
                                                   const UVStepper& uv, const UVSteps& uvstep, const RGBStepper& rgb,
                                                   const RGBSteps& rgbstep)
{
  static constexpr auto unfp_xy = [](s64 xfp) -> s32 { return static_cast<s32>(static_cast<u64>(xfp) >> 32); };

  const u64 left_x_step = tp.step_x[0];
  const u64 right_x_step = tp.step_x[1];
  const s32 end_y = tp.end_y;
  u64 left_x = tp.start_x[0];
  u64 right_x = tp.start_x[1];
  s32 current_y = tp.start_y;

  if (tp.fill_upside_down)
  {
    if (current_y <= end_y)
      return;

    UVStepper luv = uv;
    if constexpr (texture_enable)
      luv.StepY(uvstep, current_y);

    RGBStepper lrgb = rgb;
    if constexpr (shading_enable)
      lrgb.StepY(rgbstep, current_y);

    const TriangleVectors<shading_enable, texture_enable> tv(cmd, uvstep, rgbstep);

    do
    {
      current_y--;
      left_x -= left_x_step;
      right_x -= right_x_step;

      const s32 y = TruncateGPUVertexPosition(current_y);
      if (y < static_cast<s32>(g_drawing_area.top))
        break;

      // Opposite direction means we need to subtract when stepping instead of adding.
      if constexpr (texture_enable)
        luv.StepY<true>(uvstep);
      if constexpr (shading_enable)
        lrgb.StepY<true>(rgbstep);

      if (y > static_cast<s32>(g_drawing_area.bottom) ||
          (cmd->params.interlaced_rendering && cmd->params.active_line_lsb == (static_cast<u32>(current_y) & 1u)))
      {
        continue;
      }

      DrawSpan<shading_enable, texture_enable, raw_texture_enable, transparency_enable>(
        cmd, y & VRAM_HEIGHT_MASK, unfp_xy(left_x), unfp_xy(right_x), luv, uvstep, lrgb, rgbstep, tv);
    } while (current_y > end_y);
  }
  else
  {
    if (current_y >= end_y)
      return;

    UVStepper luv = uv;
    if constexpr (texture_enable)
      luv.StepY(uvstep, current_y);

    RGBStepper lrgb = rgb;
    if constexpr (shading_enable)
      lrgb.StepY(rgbstep, current_y);

    const TriangleVectors<shading_enable, texture_enable> tv(cmd, uvstep, rgbstep);

    do
    {
      const s32 y = TruncateGPUVertexPosition(current_y);

      if (y > static_cast<s32>(g_drawing_area.bottom))
      {
        break;
      }
      if (y >= static_cast<s32>(g_drawing_area.top) &&
          (!cmd->params.interlaced_rendering || cmd->params.active_line_lsb != (static_cast<u32>(current_y) & 1u)))
      {
        DrawSpan<shading_enable, texture_enable, raw_texture_enable, transparency_enable>(
          cmd, y & VRAM_HEIGHT_MASK, unfp_xy(left_x), unfp_xy(right_x), luv, uvstep, lrgb, rgbstep, tv);
      }

      current_y++;
      left_x += left_x_step;
      right_x += right_x_step;

      if constexpr (texture_enable)
        luv.StepY<false>(uvstep);
      if constexpr (shading_enable)
        lrgb.StepY<false>(rgbstep);
    } while (current_y < end_y);
  }
}

#endif // USE_VECTOR

template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable>
static void DrawTriangle(const GPUBackendDrawCommand* cmd, const GPUBackendDrawPolygonCommand::Vertex* v0,
                         const GPUBackendDrawPolygonCommand::Vertex* v1, const GPUBackendDrawPolygonCommand::Vertex* v2)
{
#ifdef CHECK_VECTOR
  const GPUBackendDrawPolygonCommand::Vertex* orig_v0 = v0;
  const GPUBackendDrawPolygonCommand::Vertex* orig_v1 = v1;
  const GPUBackendDrawPolygonCommand::Vertex* orig_v2 = v2;
#endif

  // Sort vertices so that v0 is the top vertex, v1 is the bottom vertex, and v2 is the side vertex.
  u32 tl = 0;
  if (v1->x <= v0->x)
    tl = (v2->x <= v1->x) ? 4 : 2;
  else if (v2->x < v0->x)
    tl = 4;
  else
    tl = 1;
  if (v2->y < v1->y)
  {
    std::swap(v2, v1);
    tl = ((tl >> 1) & 0x2) | ((tl << 1) & 0x4) | (tl & 0x1);
  }
  if (v1->y < v0->y)
  {
    std::swap(v1, v0);
    tl = ((tl >> 1) & 0x1) | ((tl << 1) & 0x2) | (tl & 0x4);
  }
  if (v2->y < v1->y)
  {
    std::swap(v2, v1);
    tl = ((tl >> 1) & 0x2) | ((tl << 1) & 0x4) | (tl & 0x1);
  }

  const GPUBackendDrawPolygonCommand::Vertex* vertices[3] = {v0, v1, v2};
  tl = tl >> 1;

  // Invalid size early culling.
  if (static_cast<u32>(std::abs(v2->x - v0->x)) >= MAX_PRIMITIVE_WIDTH ||
      static_cast<u32>(std::abs(v2->x - v1->x)) >= MAX_PRIMITIVE_WIDTH ||
      static_cast<u32>(std::abs(v1->x - v0->x)) >= MAX_PRIMITIVE_WIDTH ||
      static_cast<u32>(v2->y - v0->y) >= MAX_PRIMITIVE_HEIGHT || v0->y == v2->y)
  {
    return;
  }

  // Same as line rasterization, use higher precision for position.
  static constexpr auto makefp_xy = [](s32 x) { return (static_cast<s64>(x) << 32) + ((1LL << 32) - (1 << 11)); };
  static constexpr auto makestep_xy = [](s32 dx, s32 dy) -> s64 {
    return (((static_cast<s64>(dx) << 32) + ((dx < 0) ? -(dy - 1) : ((dx > 0) ? (dy - 1) : 0))) / dy);
  };
  const s64 base_coord = makefp_xy(v0->x);
  const s64 base_step = makestep_xy(v2->x - v0->x, v2->y - v0->y);
  const s64 bound_coord_us = (v1->y == v0->y) ? 0 : makestep_xy(v1->x - v0->x, v1->y - v0->y);
  const s64 bound_coord_ls = (v2->y == v1->y) ? 0 : makestep_xy(v2->x - v1->x, v2->y - v1->y);
  const u32 vo = (tl != 0) ? 1 : 0;
  const u32 vp = (tl == 2) ? 3 : 0;
  const bool right_facing = (v1->y == v0->y) ? (v1->x > v0->x) : (bound_coord_us > base_step);
  const u32 rfi = BoolToUInt32(right_facing);
  const u32 ofi = BoolToUInt32(!right_facing);

  TrianglePart triparts[2];
  TrianglePart& tpo = triparts[vo];
  TrianglePart& tpp = triparts[vo ^ 1];
  tpo.start_y = vertices[0 ^ vo]->y;
  tpo.end_y = vertices[1 ^ vo]->y;
  tpp.start_y = vertices[1 ^ vp]->y;
  tpp.end_y = vertices[2 ^ vp]->y;
  tpo.start_x[rfi] = makefp_xy(vertices[0 ^ vo]->x);
  tpo.step_x[rfi] = bound_coord_us;
  tpo.start_x[ofi] = base_coord + ((vertices[vo]->y - vertices[0]->y) * base_step);
  tpo.step_x[ofi] = base_step;
  tpo.fill_upside_down = ConvertToBoolUnchecked(vo);
  tpp.start_x[rfi] = makefp_xy(vertices[1 ^ vp]->x);
  tpp.step_x[rfi] = bound_coord_ls;
  tpp.start_x[ofi] = base_coord + ((vertices[1 ^ vp]->y - vertices[0]->y) * base_step);
  tpp.step_x[ofi] = base_step;
  tpp.fill_upside_down = (vp != 0);

#define ATTRIB_DETERMINANT(x, y) (((v1->x - v0->x) * (v2->y - v1->y)) - ((v2->x - v1->x) * (v1->y - v0->y)))
#define ATTRIB_STEP(x, y) (static_cast<u32>(ATTRIB_DETERMINANT(x, y) * (1 << ATTRIB_SHIFT) / det) << ATTRIB_POST_SHIFT)

  // Check edges.
  const s32 det = ATTRIB_DETERMINANT(x, y);
  if (det == 0) [[unlikely]]
    return;

  // Compute step values.
  UVSteps uvstep;
  RGBSteps rgbstep;
  if constexpr (texture_enable)
  {
    uvstep.dudx = ATTRIB_STEP(u, y);
    uvstep.dvdx = ATTRIB_STEP(v, y);
    uvstep.dudy = ATTRIB_STEP(x, u);
    uvstep.dvdy = ATTRIB_STEP(x, v);
  }

  if constexpr (shading_enable)
  {
    rgbstep.drdx = ATTRIB_STEP(r, y);
    rgbstep.dgdx = ATTRIB_STEP(g, y);
    rgbstep.dbdx = ATTRIB_STEP(b, y);
    rgbstep.drdy = ATTRIB_STEP(x, r);
    rgbstep.dgdy = ATTRIB_STEP(x, g);
    rgbstep.dbdy = ATTRIB_STEP(x, b);
  }

#undef ATTRIB_STEP
#undef ATTRIB_DETERMINANT

  // Undo the start of the vertex, so that when we add the offset for each line, it starts at the beginning value.
  UVStepper uv;
  RGBStepper rgb;
  const GPUBackendDrawPolygonCommand::Vertex* top_left_vertex = vertices[tl];
  if constexpr (texture_enable)
  {
    uv.Init(top_left_vertex->u, top_left_vertex->v);
    uv.StepX(uvstep, -top_left_vertex->x);
    uv.StepY(uvstep, -top_left_vertex->y);
  }
  else
  {
    uv = {};
  }

  if constexpr (shading_enable)
  {
    rgb.Init(top_left_vertex->r, top_left_vertex->g, top_left_vertex->b);
    rgb.StepX(rgbstep, -top_left_vertex->x);
    rgb.StepY(rgbstep, -top_left_vertex->y);
  }
  else
  {
    rgb.Init(top_left_vertex->r, top_left_vertex->g, top_left_vertex->b);
  }

#ifdef CHECK_VECTOR
  BACKUP_VRAM();
#endif

  for (u32 i = 0; i < 2; i++)
  {
    DrawTrianglePart<shading_enable, texture_enable, raw_texture_enable, transparency_enable>(cmd, triparts[i], uv,
                                                                                              uvstep, rgb, rgbstep);
  }

#ifdef CHECK_VECTOR
  CHECK_VRAM(
    GPU_SW_Rasterizer::DrawTriangleFunctions[shading_enable][texture_enable][raw_texture_enable][transparency_enable](
      cmd, orig_v0, orig_v1, orig_v2));
#endif
}

constinit const DrawRectangleFunctionTable DrawRectangleFunctions = {
  {{&DrawRectangle<false, false, false>, &DrawRectangle<false, false, true>},
   {&DrawRectangle<false, false, false>, &DrawRectangle<false, false, true>}},
  {{&DrawRectangle<true, false, false>, &DrawRectangle<true, false, true>},
   {&DrawRectangle<true, true, false>, &DrawRectangle<true, true, true>}}};

constinit const DrawLineFunctionTable DrawLineFunctions = {{&DrawLine<false, false>, &DrawLine<false, true>},
                                                           {&DrawLine<true, false>, &DrawLine<true, true>}};

constinit const DrawTriangleFunctionTable DrawTriangleFunctions = {
  {{{&DrawTriangle<false, false, false, false>, &DrawTriangle<false, false, false, true>},
    {&DrawTriangle<false, false, false, false>, &DrawTriangle<false, false, false, true>}},
   {{&DrawTriangle<false, true, false, false>, &DrawTriangle<false, true, false, true>},
    {&DrawTriangle<false, true, true, false>, &DrawTriangle<false, true, true, true>}}},
  {{{&DrawTriangle<true, false, false, false>, &DrawTriangle<true, false, false, true>},
    {&DrawTriangle<true, false, false, false>, &DrawTriangle<true, false, false, true>}},
   {{&DrawTriangle<true, true, false, false>, &DrawTriangle<true, true, false, true>},
    {&DrawTriangle<true, true, true, false>, &DrawTriangle<true, true, true, true>}}}};

static void FillVRAMImpl(u32 x, u32 y, u32 width, u32 height, u32 color, bool interlaced, u8 active_line_lsb)
{
#ifdef USE_VECTOR
  const u16 color16 = VRAMRGBA8888ToRGBA5551(color);
  const GSVector4i fill = GSVector4i(color16, color16, color16, color16, color16, color16, color16, color16);
  constexpr u32 vector_width = 8;
  const u32 aligned_width = Common::AlignDownPow2(width, vector_width);

  if ((x + width) <= VRAM_WIDTH && !interlaced)
  {
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;

      u16* row_ptr = &g_vram[row * VRAM_WIDTH + x];
      u32 xoffs = 0;
      for (; xoffs < aligned_width; xoffs += vector_width, row_ptr += vector_width)
        GSVector4i::store<false>(row_ptr, fill);
      for (; xoffs < width; xoffs++)
        *(row_ptr++) = color16;
    }
  }
  else if (interlaced)
  {
    // Hardware tests show that fills seem to break on the first two lines when the offset matches the displayed field.
    const u32 active_field = active_line_lsb;

    if ((x + width) <= VRAM_WIDTH)
    {
      for (u32 yoffs = 0; yoffs < height; yoffs++)
      {
        const u32 row = (y + yoffs) % VRAM_HEIGHT;
        if ((row & u32(1)) == active_field)
          continue;

        u16* row_ptr = &g_vram[row * VRAM_WIDTH + x];
        u32 xoffs = 0;
        for (; xoffs < aligned_width; xoffs += vector_width, row_ptr += vector_width)
          GSVector4i::store<false>(row_ptr, fill);
        for (; xoffs < width; xoffs++)
          *(row_ptr++) = color16;
      }
    }
    else
    {
      for (u32 yoffs = 0; yoffs < height; yoffs++)
      {
        const u32 row = (y + yoffs) % VRAM_HEIGHT;
        if ((row & u32(1)) == active_field)
          continue;

        u16* row_ptr = &g_vram[row * VRAM_WIDTH];
        for (u32 xoffs = 0; xoffs < width; xoffs++)
        {
          const u32 col = (x + xoffs) % VRAM_WIDTH;
          row_ptr[col] = color16;
        }
      }
    }
  }
  else
  {
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      u16* row_ptr = &g_vram[row * VRAM_WIDTH];
      for (u32 xoffs = 0; xoffs < width; xoffs++)
      {
        const u32 col = (x + xoffs) % VRAM_WIDTH;
        row_ptr[col] = color16;
      }
    }
  }
#else
  const u16 color16 = VRAMRGBA8888ToRGBA5551(color);
  if ((x + width) <= VRAM_WIDTH && !interlaced)
  {
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      std::fill_n(&g_vram[row * VRAM_WIDTH + x], width, color16);
    }
  }
  else if (interlaced)
  {
    // Hardware tests show that fills seem to break on the first two lines when the offset matches the displayed field.
    const u32 active_field = active_line_lsb;

    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      if ((row & u32(1)) == active_field)
        continue;

      u16* row_ptr = &g_vram[row * VRAM_WIDTH];
      for (u32 xoffs = 0; xoffs < width; xoffs++)
      {
        const u32 col = (x + xoffs) % VRAM_WIDTH;
        row_ptr[col] = color16;
      }
    }
  }
  else
  {
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      u16* row_ptr = &g_vram[row * VRAM_WIDTH];
      for (u32 xoffs = 0; xoffs < width; xoffs++)
      {
        const u32 col = (x + xoffs) % VRAM_WIDTH;
        row_ptr[col] = color16;
      }
    }
  }
#endif
}

static void WriteVRAMImpl(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask)
{
  // Fast path when the copy is not oversized.
  if ((x + width) <= VRAM_WIDTH && (y + height) <= VRAM_HEIGHT && !set_mask && !check_mask)
  {
    const u16* src_ptr = static_cast<const u16*>(data);
    u16* dst_ptr = &g_vram[y * VRAM_WIDTH + x];
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      std::copy_n(src_ptr, width, dst_ptr);
      src_ptr += width;
      dst_ptr += VRAM_WIDTH;
    }
  }
  else
  {
    // Slow path when we need to handle wrap-around.
    // During transfer/render operations, if ((dst_pixel & mask_and) == 0) { pixel = src_pixel | mask_or }
    const u16* src_ptr = static_cast<const u16*>(data);
    const u16 mask_and = check_mask ? 0x8000u : 0x0000u;
    const u16 mask_or = set_mask ? 0x8000u : 0x0000u;

#ifdef USE_VECTOR
    constexpr u32 write_pixels_per_vec = sizeof(GSVectorNi) / sizeof(u16);
    const u32 aligned_width = Common::AlignDownPow2(std::min(width, VRAM_WIDTH - x), write_pixels_per_vec);
    const GSVectorNi mask_or_vec = GSVectorNi::cxpr16(mask_or);
    const GSVectorNi mask_and_vec = GSVectorNi::cxpr16(mask_and);
#endif

    for (u32 row = 0; row < height;)
    {
      u16* dst_row_ptr = &g_vram[((y + row++) % VRAM_HEIGHT) * VRAM_WIDTH];

      u32 col = 0;

#ifdef USE_VECTOR
      // This doesn't do wraparound.
      if (mask_and != 0)
      {
        for (; col < aligned_width; col += write_pixels_per_vec)
        {
          const GSVectorNi src = GSVectorNi::load<false>(src_ptr);
          src_ptr += write_pixels_per_vec;

          GSVectorNi dst = GSVectorNi::load<false>(&dst_row_ptr[x + col]);

          const GSVectorNi mask = (dst & mask_and_vec).sra16<15>();
          dst = (dst & mask) | src.andnot(mask) | mask_or_vec;

          GSVectorNi::store<false>(&dst_row_ptr[x + col], dst);
        }
      }
      else
      {
        for (; col < aligned_width; col += write_pixels_per_vec)
        {
          const GSVectorNi src = GSVectorNi::load<false>(src_ptr);
          src_ptr += write_pixels_per_vec;

          GSVectorNi::store<false>(&dst_row_ptr[x + col], src | mask_or_vec);
        }
      }
#endif

      for (; col < width;)
      {
        // TODO: Handle unaligned reads...
        u16* pixel_ptr = &dst_row_ptr[(x + col++) % VRAM_WIDTH];
        if (((*pixel_ptr) & mask_and) == 0)
          *pixel_ptr = *(src_ptr++) | mask_or;
      }
    }
  }
}

static void CopyVRAMImpl(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height, bool set_mask,
                         bool check_mask)
{
  // Break up oversized copies. This behavior has not been verified on console.
  if ((src_x + width) > VRAM_WIDTH || (dst_x + width) > VRAM_WIDTH)
  {
    u32 remaining_rows = height;
    u32 current_src_y = src_y;
    u32 current_dst_y = dst_y;
    while (remaining_rows > 0)
    {
      const u32 rows_to_copy =
        std::min<u32>(remaining_rows, std::min<u32>(VRAM_HEIGHT - current_src_y, VRAM_HEIGHT - current_dst_y));

      u32 remaining_columns = width;
      u32 current_src_x = src_x;
      u32 current_dst_x = dst_x;
      while (remaining_columns > 0)
      {
        const u32 columns_to_copy =
          std::min<u32>(remaining_columns, std::min<u32>(VRAM_WIDTH - current_src_x, VRAM_WIDTH - current_dst_x));
        CopyVRAMImpl(current_src_x, current_src_y, current_dst_x, current_dst_y, columns_to_copy, rows_to_copy,
                     set_mask, check_mask);
        current_src_x = (current_src_x + columns_to_copy) % VRAM_WIDTH;
        current_dst_x = (current_dst_x + columns_to_copy) % VRAM_WIDTH;
        remaining_columns -= columns_to_copy;
      }

      current_src_y = (current_src_y + rows_to_copy) % VRAM_HEIGHT;
      current_dst_y = (current_dst_y + rows_to_copy) % VRAM_HEIGHT;
      remaining_rows -= rows_to_copy;
    }

    return;
  }

  // This doesn't have a fast path, but do we really need one? It's not common.
  const u16 mask_and = check_mask ? 0x8000u : 0x0000u;
  const u16 mask_or = set_mask ? 0x8000u : 0x0000u;

  // Copy in reverse when src_x < dst_x, this is verified on console.
  if (src_x < dst_x || ((src_x + width - 1) % VRAM_WIDTH) < ((dst_x + width - 1) % VRAM_WIDTH))
  {
    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &g_vram[((src_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
      u16* dst_row_ptr = &g_vram[((dst_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];

      for (s32 col = static_cast<s32>(width - 1); col >= 0; col--)
      {
        const u16 src_pixel = src_row_ptr[(src_x + static_cast<u32>(col)) % VRAM_WIDTH];
        u16* dst_pixel_ptr = &dst_row_ptr[(dst_x + static_cast<u32>(col)) % VRAM_WIDTH];
        *dst_pixel_ptr = ((*dst_pixel_ptr & mask_and) == 0) ? (src_pixel | mask_or) : *dst_pixel_ptr;
      }
    }
  }
  else
  {
#ifdef USE_VECTOR
    constexpr u32 copy_pixels_per_vec = sizeof(GSVectorNi) / sizeof(u16);
    const u32 aligned_width = Common::AlignDownPow2(
      std::min(width, std::min<u32>(VRAM_WIDTH - src_x, VRAM_WIDTH - dst_x)), copy_pixels_per_vec);
    const GSVectorNi mask_or_vec = GSVectorNi::cxpr16(mask_or);
    const GSVectorNi mask_and_vec = GSVectorNi::cxpr16(mask_and);
#endif

    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &g_vram[((src_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
      u16* dst_row_ptr = &g_vram[((dst_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];

      u32 col = 0;

#ifdef USE_VECTOR
      // This doesn't do wraparound.
      if (mask_and != 0)
      {
        for (; col < aligned_width; col += copy_pixels_per_vec)
        {
          const GSVectorNi src = GSVectorNi::load<false>(&src_row_ptr[src_x + col]);
          GSVectorNi dst = GSVectorNi::load<false>(&dst_row_ptr[dst_x + col]);

          const GSVectorNi mask = (dst & mask_and_vec).sra16<15>();
          dst = (dst & mask) | src.andnot(mask) | mask_or_vec;

          GSVectorNi::store<false>(&dst_row_ptr[dst_x + col], dst);
        }
      }
      else
      {
        for (; col < aligned_width; col += copy_pixels_per_vec)
        {
          const GSVectorNi src = GSVectorNi::load<false>(&src_row_ptr[src_x + col]);
          GSVectorNi::store<false>(&dst_row_ptr[dst_x + col], src | mask_or_vec);
        }
      }
#endif

      for (; col < width; col++)
      {
        const u16 src_pixel = src_row_ptr[(src_x + col) % VRAM_WIDTH];
        u16* dst_pixel_ptr = &dst_row_ptr[(dst_x + col) % VRAM_WIDTH];
        *dst_pixel_ptr = ((*dst_pixel_ptr & mask_and) == 0) ? (src_pixel | mask_or) : *dst_pixel_ptr;
      }
    }
  }
}

#ifdef __INTELLISENSE__
}
#endif
