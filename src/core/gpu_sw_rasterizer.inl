// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#ifdef __INTELLISENSE__

#include "common/gsvector.h"
#include "gpu.h"
#include <algorithm>

#define USE_VECTOR 1
#define GSVECTOR_HAS_SRLV 1

extern GPU_SW_Rasterizer::DitherLUT g_dither_lut;

namespace GPU_SW_Rasterizer {

#endif

// TODO: UpdateVRAM, FillVRAM, etc.

#ifdef USE_VECTOR
#if 0
static u16 s_vram_backup[VRAM_WIDTH * VRAM_HEIGHT];
static u16 s_new_vram[VRAM_WIDTH * VRAM_HEIGHT];
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
#else
#define BACKUP_VRAM()
#define CHECK_VRAM(drawer)
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

template<bool texture_enable, bool raw_texture_enable, bool transparency_enable, bool dithering_enable>
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

      ShadePixel<texture_enable, raw_texture_enable, transparency_enable, false>(cmd, static_cast<u32>(x), draw_y, r, g,
                                                                                 b, texcoord_x, texcoord_y);
    }
  }
}

#else // USE_VECTOR

ALWAYS_INLINE_RELEASE static GSVector4i GatherVector(GSVector4i coord_x, GSVector4i coord_y)
{
  GSVector4i offsets = coord_y.sll32<11>();    // y * 2048 (1024 * sizeof(pixel))
  offsets = offsets.add32(coord_x.sll32<1>()); // x * 2 (x * sizeof(pixel))

  const u32 o0 = offsets.extract32<0>();
  const u32 o1 = offsets.extract32<1>();
  const u32 o2 = offsets.extract32<2>();
  const u32 o3 = offsets.extract32<3>();

  // TODO: split in two, merge, maybe could be zx loaded instead..
  u16 p0, p1, p2, p3;
  std::memcpy(&p0, reinterpret_cast<const u8*>(g_vram) + o0, sizeof(p0));
  std::memcpy(&p1, reinterpret_cast<const u8*>(g_vram) + o1, sizeof(p1));
  std::memcpy(&p2, reinterpret_cast<const u8*>(g_vram) + o2, sizeof(p2));
  std::memcpy(&p3, reinterpret_cast<const u8*>(g_vram) + o3, sizeof(p3));
  GSVector4i pixels = GSVector4i::load(p0);
  pixels = pixels.insert16<2>(p1);
  pixels = pixels.insert16<4>(p2);
  pixels = pixels.insert16<6>(p3);

  return pixels;
}

ALWAYS_INLINE_RELEASE static GSVector4i GatherCLUTVector(GSVector4i indices)
{
  const GSVector4i offsets = indices.sll32<1>(); // x * 2 (x * sizeof(pixel))
  const u32 o0 = offsets.extract32<0>();
  const u32 o1 = offsets.extract32<1>();
  const u32 o2 = offsets.extract32<2>();
  const u32 o3 = offsets.extract32<3>();

  // TODO: split in two, merge, maybe could be zx loaded instead..
  u16 p0, p1, p2, p3;
  std::memcpy(&p0, reinterpret_cast<const u8*>(g_gpu_clut) + o0, sizeof(p0));
  std::memcpy(&p1, reinterpret_cast<const u8*>(g_gpu_clut) + o1, sizeof(p1));
  std::memcpy(&p2, reinterpret_cast<const u8*>(g_gpu_clut) + o2, sizeof(p2));
  std::memcpy(&p3, reinterpret_cast<const u8*>(g_gpu_clut) + o3, sizeof(p3));
  GSVector4i pixels = GSVector4i::load(p0);
  pixels = pixels.insert16<2>(p1);
  pixels = pixels.insert16<4>(p2);
  pixels = pixels.insert16<6>(p3);

  return pixels;
}

ALWAYS_INLINE_RELEASE static GSVector4i LoadVector(u32 x, u32 y)
{
  if (x <= (VRAM_WIDTH - 4))
  {
    return GSVector4i::loadl(&g_vram[y * VRAM_WIDTH + x]).u16to32();
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
  if (x <= (VRAM_WIDTH - 4))
  {
    GSVector4i::storel(&g_vram[y * VRAM_WIDTH + x], color);
  }
  else
  {
    u16* line = &g_vram[y * VRAM_WIDTH];
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(color.extract16<0>());
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(color.extract16<1>());
    line[(x++) & VRAM_WIDTH_MASK] = Truncate16(color.extract16<2>());
    line[x & VRAM_WIDTH_MASK] = Truncate16(color.extract16<3>());
  }
}

ALWAYS_INLINE_RELEASE static void RGB5A1ToRG_BA(GSVector4i rgb5a1, GSVector4i& rg, GSVector4i& ba)
{
  rg = rgb5a1 & GSVector4i::cxpr(0x1F);                     // R | R | R | R
  rg = rg | (rgb5a1 & GSVector4i::cxpr(0x3E0)).sll32<11>(); // R0G0 | R0G0 | R0G0 | R0G0
  ba = rgb5a1.srl32<10>() & GSVector4i::cxpr(0x1F);         // B | B | B | B
  ba = ba | (rgb5a1 & GSVector4i::cxpr(0x8000)).sll32<1>(); // B0A0 | B0A0 | B0A0 | B0A0
}

ALWAYS_INLINE_RELEASE static GSVector4i RG_BAToRGB5A1(GSVector4i rg, GSVector4i ba)
{
  GSVector4i res;

  res = rg & GSVector4i::cxpr(0x1F);                       // R | R | R | R
  res = res | (rg.srl32<11>() & GSVector4i::cxpr(0x3E0));  // RG | RG | RG | RG
  res = res | ((ba & GSVector4i::cxpr(0x1F)).sll32<10>()); // RGB | RGB | RGB | RGB
  res = res | ba.srl32<16>().sll32<15>();                  // RGBA | RGBA | RGBA | RGBA

  return res;
}

// Color repeated twice for RG packing, then duplicated to we can load based on the X offset.
static constexpr s16 VECTOR_DITHER_MATRIX[4][16] = {
#define P(m, n) static_cast<s16>(DITHER_MATRIX[m][n]), static_cast<s16>(DITHER_MATRIX[m][n])
#define R(m) P(m, 0), P(m, 1), P(m, 2), P(m, 3), P(m, 0), P(m, 1), P(m, 2), P(m, 3)

  {R(0)}, {R(1)}, {R(2)}, {R(3)}

#undef R
#undef P
};

template<bool texture_enable, bool raw_texture_enable, bool transparency_enable, bool dithering_enable>
ALWAYS_INLINE_RELEASE static void
ShadePixel(const GPUBackendDrawCommand* cmd, u32 start_x, u32 y, GSVector4i vertex_color_rg, GSVector4i vertex_color_ba,
           GSVector4i texcoord_x, GSVector4i texcoord_y, GSVector4i preserve_mask, GSVector4i dither)
{
  static constinit GSVector4i coord_mask_x = GSVector4i::cxpr(VRAM_WIDTH_MASK);
  static constinit GSVector4i coord_mask_y = GSVector4i::cxpr(VRAM_HEIGHT_MASK);

  GSVector4i color;

  if constexpr (texture_enable)
  {
    // Apply texture window
    texcoord_x = (texcoord_x & GSVector4i(cmd->window.and_x)) | GSVector4i(cmd->window.or_x);
    texcoord_y = (texcoord_y & GSVector4i(cmd->window.and_y)) | GSVector4i(cmd->window.or_y);

    const GSVector4i base_x = GSVector4i(cmd->draw_mode.GetTexturePageBaseX());
    const GSVector4i base_y = GSVector4i(cmd->draw_mode.GetTexturePageBaseY());

    texcoord_y = base_y.add32(texcoord_y) & coord_mask_y;

    GSVector4i texture_color;
    switch (cmd->draw_mode.texture_mode)
    {
      case GPUTextureMode::Palette4Bit:
      {
        GSVector4i load_texcoord_x = texcoord_x.srl32<2>();
        load_texcoord_x = base_x.add32(load_texcoord_x);
        load_texcoord_x = load_texcoord_x & coord_mask_x;

        // todo: sse4 path
        GSVector4i palette_shift = (texcoord_x & GSVector4i::cxpr(3)).sll32<2>();
        GSVector4i palette_indices = GatherVector(load_texcoord_x, texcoord_y);
#ifdef GSVECTOR_HAS_SRLV
        palette_indices = palette_indices.srlv32(palette_shift) & GSVector4i::cxpr(0x0F);
#else
        Assert(false && "Fixme");
#endif

        texture_color = GatherCLUTVector(palette_indices);
      }
      break;

      case GPUTextureMode::Palette8Bit:
      {
        GSVector4i load_texcoord_x = texcoord_x.srl32<1>();
        load_texcoord_x = base_x.add32(load_texcoord_x);
        load_texcoord_x = load_texcoord_x & coord_mask_x;

        GSVector4i palette_shift = (texcoord_x & GSVector4i::cxpr(1)).sll32<3>();
        GSVector4i palette_indices = GatherVector(load_texcoord_x, texcoord_y);
#ifdef GSVECTOR_HAS_SRLV
        palette_indices = palette_indices.srlv32(palette_shift) & GSVector4i::cxpr(0xFF);
#else
        Assert(false && "Fixme");
#endif

        texture_color = GatherCLUTVector(palette_indices);
      }
      break;

      default:
      {
        texcoord_x = base_x.add32(texcoord_x);
        texcoord_x = texcoord_x & coord_mask_x;
        texture_color = GatherVector(texcoord_x, texcoord_y);
      }
      break;
    }

    // check for zero texture colour across the 4 pixels, early out if so
    const GSVector4i texture_transparent_mask = texture_color.eq32(GSVector4i::zero());
    if (texture_transparent_mask.alltrue())
      return;

    preserve_mask = preserve_mask | texture_transparent_mask;

    if constexpr (raw_texture_enable)
    {
      color = texture_color;
    }
    else
    {
      GSVector4i trg, tba;
      RGB5A1ToRG_BA(texture_color, trg, tba);

      // now we have both the texture and vertex color in RG/GA pairs, for 4 pixels, which we can multiply
      GSVector4i rg = trg.mul16l(vertex_color_rg);
      GSVector4i ba = tba.mul16l(vertex_color_ba);

      // TODO: Dither
      // Convert to 5bit.
      if constexpr (dithering_enable)
      {
        rg = rg.sra16<4>().add16(dither).max_i16(GSVector4i::zero()).sra16<3>();
        ba = ba.sra16<4>().add16(dither).max_i16(GSVector4i::zero()).sra16<3>();
      }
      else
      {
        rg = rg.sra16<7>();
        ba = ba.sra16<7>();
      }

      // Bit15 gets passed through as-is.
      ba = ba.blend16<0xaa>(tba);

      // Clamp to 5bit.
      static constexpr GSVector4i colclamp = GSVector4i::cxpr16(0x1F);
      rg = rg.min_u16(colclamp);
      ba = ba.min_u16(colclamp);

      // And interleave back to 16bpp.
      color = RG_BAToRGB5A1(rg, ba);
    }
  }
  else
  {
    // Non-textured transparent polygons don't set bit 15, but are treated as transparent.
    if constexpr (dithering_enable)
    {
      GSVector4i rg = vertex_color_rg.add16(dither).max_i16(GSVector4i::zero()).sra16<3>();
      GSVector4i ba = vertex_color_ba.add16(dither).max_i16(GSVector4i::zero()).sra16<3>();

      // Clamp to 5bit. We use 32bit for BA to set a to zero.
      rg = rg.min_u16(GSVector4i::cxpr16(0x1F));
      ba = ba.min_u16(GSVector4i::cxpr(0x1F));

      // And interleave back to 16bpp.
      color = RG_BAToRGB5A1(rg, ba);
    }
    else
    {
      // Note that bit15 is set to 0 here, which the shift will do.
      const GSVector4i rg = vertex_color_rg.srl16<3>();
      const GSVector4i ba = vertex_color_ba.srl16<3>();
      color = RG_BAToRGB5A1(rg, ba);
    }
  }

  GSVector4i bg_color = LoadVector(start_x, y);

  if constexpr (transparency_enable)
  {
    [[maybe_unused]] GSVector4i transparent_mask;
    if constexpr (texture_enable)
    {
      // Compute transparent_mask, ffff per lane if transparent otherwise 0000
      transparent_mask = color.sra16<15>();
    }

    // TODO: We don't need to OR color here with 0x8000 for textures.
    // 0x8000 is added to match serial path.

    GSVector4i blended_color;
    switch (cmd->draw_mode.transparency_mode)
    {
      case GPUTransparencyMode::HalfBackgroundPlusHalfForeground:
      {
        const GSVector4i fg_bits = color | GSVector4i::cxpr(0x8000u);
        const GSVector4i bg_bits = bg_color | GSVector4i::cxpr(0x8000u);
        const GSVector4i res = fg_bits.add32(bg_bits).sub32((fg_bits ^ bg_bits) & GSVector4i::cxpr(0x0421u)).srl32<1>();
        blended_color = res & GSVector4i::cxpr(0xffff);
      }
      break;

      case GPUTransparencyMode::BackgroundPlusForeground:
      {
        const GSVector4i fg_bits = color | GSVector4i::cxpr(0x8000u);
        const GSVector4i bg_bits = bg_color & GSVector4i::cxpr(0x7FFFu);
        const GSVector4i sum = fg_bits.add32(bg_bits);
        const GSVector4i carry =
          (sum.sub32((fg_bits ^ bg_bits) & GSVector4i::cxpr(0x8421u))) & GSVector4i::cxpr(0x8420u);
        const GSVector4i res = sum.sub32(carry) | carry.sub32(carry.srl32<5>());
        blended_color = res & GSVector4i::cxpr(0xffff);
      }
      break;

      case GPUTransparencyMode::BackgroundMinusForeground:
      {
        const GSVector4i bg_bits = bg_color | GSVector4i::cxpr(0x8000u);
        const GSVector4i fg_bits = color & GSVector4i::cxpr(0x7FFFu);
        const GSVector4i diff = bg_bits.sub32(fg_bits).add32(GSVector4i::cxpr(0x108420u));
        const GSVector4i borrow =
          diff.sub32((bg_bits ^ fg_bits) & GSVector4i::cxpr(0x108420u)) & GSVector4i::cxpr(0x108420u);
        const GSVector4i res = diff.sub32(borrow) & borrow.sub32(borrow.srl32<5>());
        blended_color = res & GSVector4i::cxpr(0xffff);
      }
      break;

      case GPUTransparencyMode::BackgroundPlusQuarterForeground:
      default:
      {
        const GSVector4i bg_bits = bg_color & GSVector4i::cxpr(0x7FFFu);
        const GSVector4i fg_bits =
          ((color | GSVector4i::cxpr(0x8000)).srl32<2>() & GSVector4i::cxpr(0x1CE7u)) | GSVector4i::cxpr(0x8000u);
        const GSVector4i sum = fg_bits.add32(bg_bits);
        const GSVector4i carry = sum.sub32((fg_bits ^ bg_bits) & GSVector4i::cxpr(0x8421u)) & GSVector4i::cxpr(0x8420u);
        const GSVector4i res = sum.sub32(carry) | carry.sub32(carry.srl32<5>());
        blended_color = res & GSVector4i::cxpr(0xffff);
      }
      break;
    }

    // select blended pixels for transparent pixels, otherwise consider opaque
    // TODO: SSE2
    if constexpr (texture_enable)
      color = color.blend8(blended_color, transparent_mask);
    else
      color = blended_color & GSVector4i::cxpr(0x7fff);
  }

  // TODO: lift out to parent?
  const GSVector4i mask_and = GSVector4i(cmd->params.GetMaskAND());
  const GSVector4i mask_or = GSVector4i(cmd->params.GetMaskOR());

  GSVector4i mask_bits_set = bg_color & mask_and; // 8000 if masked else 0000
  mask_bits_set = mask_bits_set.sra16<15>();      // ffff if masked else 0000
  preserve_mask = preserve_mask | mask_bits_set;  // ffff if preserved else 0000

  bg_color = bg_color & preserve_mask;
  color = (color | mask_or).andnot(preserve_mask);
  color = color | bg_color;

  const GSVector4i packed_color = color.pu32();
  StoreVector(start_x, y, packed_color);
}

template<bool texture_enable, bool raw_texture_enable, bool transparency_enable>
static void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd)
{
  const s32 origin_x = cmd->x;
  const s32 origin_y = cmd->y;

  const GSVector4i rgba = GSVector4i(cmd->color); // RGBA | RGBA | RGBA | RGBA
  GSVector4i rg = rgba.xxxxl();                   // RGRG | RGRG | RGRG | RGRG
  GSVector4i ba = rgba.yyyyl();                   // BABA | BABA | BABA | BABA
  rg = rg.u8to16();                               // R0G0 | R0G0 | R0G0 | R0G0
  ba = ba.u8to16();                               // B0A0 | B0A0 | B0A0 | B0A0

  const GSVector4i texcoord_x = GSVector4i(cmd->texcoord & 0xFF).add32(GSVector4i::cxpr(0, 1, 2, 3));
  GSVector4i texcoord_y = GSVector4i(cmd->texcoord >> 8);

  const GSVector4i clip_left = GSVector4i(g_drawing_area.left);
  const GSVector4i clip_right = GSVector4i(g_drawing_area.right);
  const u32 width = cmd->width;

  BACKUP_VRAM();

  for (u32 offset_y = 0; offset_y < cmd->height; offset_y++)
  {
    const s32 y = origin_y + static_cast<s32>(offset_y);
    if (y < static_cast<s32>(g_drawing_area.top) || y > static_cast<s32>(g_drawing_area.bottom) ||
        (cmd->params.interlaced_rendering && cmd->params.active_line_lsb == (Truncate8(static_cast<u32>(y)) & 1u)))
    {
      continue;
    }

    GSVector4i row_texcoord_x = texcoord_x;
    GSVector4i xvec = GSVector4i(origin_x).add32(GSVector4i::cxpr(0, 1, 2, 3));
    GSVector4i wvec = GSVector4i(width).sub32(GSVector4i::cxpr(1, 2, 3, 4));

    for (u32 offset_x = 0; offset_x < width; offset_x += 4)
    {
      const s32 x = origin_x + static_cast<s32>(offset_x);

      // width test
      GSVector4i preserve_mask = wvec.lt32(GSVector4i::zero());

      // clip test, if all pixels are outside, skip
      preserve_mask = preserve_mask | xvec.lt32(clip_left);
      preserve_mask = preserve_mask | xvec.gt32(clip_right);
      if (!preserve_mask.alltrue())
      {
        ShadePixel<texture_enable, raw_texture_enable, transparency_enable, false>(
          cmd, x, y, rg, ba, row_texcoord_x, texcoord_y, preserve_mask, GSVector4i::zero());
      }

      xvec = xvec.add32(GSVector4i::cxpr(4));
      wvec = wvec.sub32(GSVector4i::cxpr(4));

      if constexpr (texture_enable)
        row_texcoord_x = row_texcoord_x.add32(GSVector4i::cxpr(4)) & GSVector4i::cxpr(0xFF);
    }

    if constexpr (texture_enable)
      texcoord_y = texcoord_y.add32(GSVector4i::cxpr(1)) & GSVector4i::cxpr(0xFF);
  }

  CHECK_VRAM(GPU_SW_Rasterizer::DrawRectangleFunctions[texture_enable][raw_texture_enable][transparency_enable](cmd));
}

#endif // USE_VECTOR

// TODO: Vectorize line draw.
template<bool shading_enable, bool transparency_enable, bool dithering_enable>
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

      ShadePixel<false, false, transparency_enable, dithering_enable>(
        cmd, static_cast<u32>(x), static_cast<u32>(y) & VRAM_HEIGHT_MASK, r, g, b, 0, 0);
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

  ALWAYS_INLINE void SetStart(u32 ustart, u32 vstart)
  {
    u = (((ustart << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT);
    v = (((vstart << ATTRIB_SHIFT) + (1u << (ATTRIB_SHIFT - 1))) << ATTRIB_POST_SHIFT);
  }

  ALWAYS_INLINE void StepX(const UVSteps& steps)
  {
    u = u + steps.dudx;
    v = v + steps.dvdx;
  }
  ALWAYS_INLINE void StepXY(const UVSteps& steps, s32 x_count, s32 y_count)
  {
    u = u + (steps.dudx * static_cast<u32>(x_count)) + (steps.dudy * static_cast<u32>(y_count));
    v = v + (steps.dvdx * static_cast<u32>(x_count)) + (steps.dvdy * static_cast<u32>(y_count));
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

  ALWAYS_INLINE void SetStart(u32 rstart, u32 gstart, u32 bstart)
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
  ALWAYS_INLINE void StepXY(const RGBSteps& steps, s32 x_count, s32 y_count)
  {
    r = r + (steps.drdx * static_cast<u32>(x_count)) + (steps.drdy * static_cast<u32>(y_count));
    g = g + (steps.dgdx * static_cast<u32>(x_count)) + (steps.dgdy * static_cast<u32>(y_count));
    b = b + (steps.dbdx * static_cast<u32>(x_count)) + (steps.dbdy * static_cast<u32>(y_count));
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

template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
         bool dithering_enable>
static void DrawSpan(const GPUBackendDrawPolygonCommand* cmd, s32 y, s32 x_start, s32 x_bound, UVStepper uv,
                     const UVSteps& uvstep, RGBStepper rgb, const RGBSteps& rgbstep)
{
  if (cmd->params.interlaced_rendering && cmd->params.active_line_lsb == (Truncate8(static_cast<u32>(y)) & 1u))
    return;

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
    uv.StepXY(uvstep, x_start, y);
  if constexpr (shading_enable)
    rgb.StepXY(rgbstep, x_start, y);

  do
  {
    ShadePixel<texture_enable, raw_texture_enable, transparency_enable, dithering_enable>(
      cmd, static_cast<u32>(current_x), static_cast<u32>(y), rgb.GetR(), rgb.GetG(), rgb.GetB(), uv.GetU(), uv.GetV());

    current_x++;
    if constexpr (texture_enable)
      uv.StepX(uvstep);
    if constexpr (shading_enable)
      rgb.StepX(rgbstep);
  } while (--width > 0);
}

#else // USE_VECTOR

template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
         bool dithering_enable>
static void DrawSpan(const GPUBackendDrawPolygonCommand* cmd, s32 y, s32 x_start, s32 x_bound, UVStepper uv,
                     const UVSteps& uvstep, RGBStepper rgb, const RGBSteps& rgbstep)
{
  if (cmd->params.interlaced_rendering && cmd->params.active_line_lsb == (Truncate8(static_cast<u32>(y)) & 1u))
    return;

  s32 w = x_bound - x_start;
  s32 x = TruncateGPUVertexPosition(x_start);

  if (x < static_cast<s32>(g_drawing_area.left))
  {
    const s32 delta = static_cast<s32>(g_drawing_area.left) - x;
    x_start += delta;
    x += delta;
    w -= delta;
  }

  if ((x + w) > (static_cast<s32>(g_drawing_area.right) + 1))
    w = static_cast<s32>(g_drawing_area.right) + 1 - x;

  if (w <= 0)
    return;

  // TODO: Precompute.

  const auto clip_left = GSVector4i(g_drawing_area.left);
  const auto clip_right = GSVector4i(g_drawing_area.right);

  const GSVector4i dr_dx = GSVector4i(rgbstep.drdx * 4);
  const GSVector4i dg_dx = GSVector4i(rgbstep.dgdx * 4);
  const GSVector4i db_dx = GSVector4i(rgbstep.dbdx * 4);
  const GSVector4i du_dx = GSVector4i(uvstep.dudx * 4);
  const GSVector4i dv_dx = GSVector4i(uvstep.dvdx * 4);

  // TODO: vectorize
  const GSVector4i dr_dx_offset = GSVector4i(0, rgbstep.drdx, rgbstep.drdx * 2, rgbstep.drdx * 3);
  const GSVector4i dg_dx_offset = GSVector4i(0, rgbstep.dgdx, rgbstep.dgdx * 2, rgbstep.dgdx * 3);
  const GSVector4i db_dx_offset = GSVector4i(0, rgbstep.dbdx, rgbstep.dbdx * 2, rgbstep.dbdx * 3);
  const GSVector4i du_dx_offset = GSVector4i(0, uvstep.dudx, uvstep.dudx * 2, uvstep.dudx * 3);
  const GSVector4i dv_dx_offset = GSVector4i(0, uvstep.dvdx, uvstep.dvdx * 2, uvstep.dvdx * 3);

  GSVector4i dr, dg, db;
  if constexpr (shading_enable)
  {
    dr = GSVector4i(rgb.r + rgbstep.drdx * x_start).add32(dr_dx_offset);
    dg = GSVector4i(rgb.g + rgbstep.dgdx * x_start).add32(dg_dx_offset);
    db = GSVector4i(rgb.b + rgbstep.dbdx * x_start).add32(db_dx_offset);
  }
  else
  {
    // precompute for flat shading
    dr = GSVector4i(rgb.r >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT));
    dg = GSVector4i((rgb.g >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT)) << 16);
    db = GSVector4i(rgb.b >> (ATTRIB_SHIFT + ATTRIB_POST_SHIFT));
  }

  GSVector4i du = GSVector4i(uv.u + uvstep.dudx * x_start).add32(du_dx_offset);
  GSVector4i dv = GSVector4i(uv.v + uvstep.dvdx * x_start).add32(dv_dx_offset);

  // TODO: Move to caller.
  if constexpr (shading_enable)
  {
    // TODO: vectorize multiply?
    dr = dr.add32(GSVector4i(rgbstep.drdy * y));
    dg = dg.add32(GSVector4i(rgbstep.dgdy * y));
    db = db.add32(GSVector4i(rgbstep.dbdy * y));
  }

  if constexpr (texture_enable)
  {
    du = du.add32(GSVector4i(uvstep.dudy * y));
    dv = dv.add32(GSVector4i(uvstep.dvdy * y));
  }

  const GSVector4i dither =
    GSVector4i::load<false>(&VECTOR_DITHER_MATRIX[static_cast<u32>(y) & 3][(static_cast<u32>(x) & 3) * 2]);

  GSVector4i xvec = GSVector4i(x).add32(GSVector4i::cxpr(0, 1, 2, 3));
  GSVector4i wvec = GSVector4i(w).sub32(GSVector4i::cxpr(1, 2, 3, 4));

  for (s32 count = (w + 3) / 4; count > 0; --count)
  {
    // R000 | R000 | R000 | R000
    // R0G0 | R0G0 | R0G0 | R0G0
    const GSVector4i r = shading_enable ? dr.srl32<ATTRIB_SHIFT + ATTRIB_POST_SHIFT>() : dr;
    const GSVector4i g =
      shading_enable ? dg.srl32<ATTRIB_SHIFT + ATTRIB_POST_SHIFT>().sll32<16>() : dg; // get G into the correct position
    const GSVector4i b = shading_enable ? db.srl32<ATTRIB_SHIFT + ATTRIB_POST_SHIFT>() : db;
    const GSVector4i u = du.srl32<ATTRIB_SHIFT + ATTRIB_POST_SHIFT>();
    const GSVector4i v = dv.srl32<ATTRIB_SHIFT + ATTRIB_POST_SHIFT>();

    const GSVector4i rg = r.blend16<0xAA>(g);

    // mask based on what's outside the span
    auto preserve_mask = wvec.lt32(GSVector4i::zero());

    // clip test, if all pixels are outside, skip
    preserve_mask = preserve_mask | xvec.lt32(clip_left);
    preserve_mask = preserve_mask | xvec.gt32(clip_right);
    if (!preserve_mask.alltrue())
    {
      ShadePixel<texture_enable, raw_texture_enable, transparency_enable, dithering_enable>(
        cmd, static_cast<u32>(x), static_cast<u32>(y), rg, b, u, v, preserve_mask, dither);
    }

    x += 4;

    xvec = xvec.add32(GSVector4i::cxpr(4));
    wvec = wvec.sub32(GSVector4i::cxpr(4));

    if constexpr (shading_enable)
    {
      dr = dr.add32(dr_dx);
      dg = dg.add32(dg_dx);
      db = db.add32(db_dx);
    }

    if constexpr (texture_enable)
    {
      du = du.add32(du_dx);
      dv = dv.add32(dv_dx);
    }
  }
}

#endif // USE_VECTOR

template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
         bool dithering_enable>
ALWAYS_INLINE_RELEASE static void DrawTrianglePart(const GPUBackendDrawPolygonCommand* cmd, const TrianglePart& tp,
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
    while (current_y > end_y)
    {
      current_y--;
      left_x -= left_x_step;
      right_x -= right_x_step;

      const s32 y = TruncateGPUVertexPosition(current_y);
      if (y < static_cast<s32>(g_drawing_area.top))
        break;
      else if (y > static_cast<s32>(g_drawing_area.bottom))
        continue;

      DrawSpan<shading_enable, texture_enable, raw_texture_enable, transparency_enable, dithering_enable>(
        cmd, y & VRAM_HEIGHT_MASK, unfp_xy(left_x), unfp_xy(right_x), uv, uvstep, rgb, rgbstep);
    }
  }
  else
  {
    while (current_y < end_y)
    {
      const s32 y = TruncateGPUVertexPosition(current_y);

      if (y > static_cast<s32>(g_drawing_area.bottom))
      {
        break;
      }
      else if (y >= static_cast<s32>(g_drawing_area.top))
      {
        DrawSpan<shading_enable, texture_enable, raw_texture_enable, transparency_enable, dithering_enable>(
          cmd, y & VRAM_HEIGHT_MASK, unfp_xy(left_x), unfp_xy(right_x), uv, uvstep, rgb, rgbstep);
      }

      current_y++;
      left_x += left_x_step;
      right_x += right_x_step;
    }
  }
}

template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
         bool dithering_enable>
static void DrawTriangle(const GPUBackendDrawPolygonCommand* cmd, const GPUBackendDrawPolygonCommand::Vertex* v0,
                         const GPUBackendDrawPolygonCommand::Vertex* v1, const GPUBackendDrawPolygonCommand::Vertex* v2)
{
#if 0
  const GPUBackendDrawPolygonCommand::Vertex* orig_v0 = v0;
  const GPUBackendDrawPolygonCommand::Vertex* orig_v1 = v1;
  const GPUBackendDrawPolygonCommand::Vertex* orig_v2 = v2;
#endif

  // Sort vertices so that v0 is the top vertex, v1 is the bottom vertex, and v2 is the side vertex.
  u32 vc = 0;
  if (v1->x <= v0->x)
    vc = (v2->x <= v1->x) ? 4 : 2;
  else if (v2->x < v0->x)
    vc = 4;
  else
    vc = 1;
  if (v2->y < v1->y)
  {
    std::swap(v2, v1);
    vc = ((vc >> 1) & 0x2) | ((vc << 1) & 0x4) | (vc & 0x1);
  }
  if (v1->y < v0->y)
  {
    std::swap(v1, v0);
    vc = ((vc >> 1) & 0x1) | ((vc << 1) & 0x2) | (vc & 0x4);
  }
  if (v2->y < v1->y)
  {
    std::swap(v2, v1);
    vc = ((vc >> 1) & 0x2) | ((vc << 1) & 0x4) | (vc & 0x1);
  }

  const GPUBackendDrawPolygonCommand::Vertex* vertices[3] = {v0, v1, v2};
  vc = vc >> 1;

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
  const u32 vo = (vc != 0) ? 1 : 0;
  const u32 vp = (vc == 2) ? 3 : 0;
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
  const GPUBackendDrawPolygonCommand::Vertex* core_vertex = vertices[vc];
  if constexpr (texture_enable)
  {
    uv.SetStart(core_vertex->u, core_vertex->v);
    uv.StepXY(uvstep, -core_vertex->x, -core_vertex->y);
  }
  else
  {
    // Not actually used, but shut up the compiler. Should get optimized out.
    uv = {};
  }

  rgb.SetStart(core_vertex->r, core_vertex->g, core_vertex->b);
  if constexpr (shading_enable)
    rgb.StepXY(rgbstep, -core_vertex->x, -core_vertex->y);

#ifdef USE_VECTOR
  BACKUP_VRAM();
#endif

  for (u32 i = 0; i < 2; i++)
  {
    DrawTrianglePart<shading_enable, texture_enable, raw_texture_enable, transparency_enable, dithering_enable>(
      cmd, triparts[i], uv, uvstep, rgb, rgbstep);
  }

#ifdef USE_VECTOR
  CHECK_VRAM(
    GPU_SW_Rasterizer::DrawTriangleFunctions[shading_enable][texture_enable][raw_texture_enable][transparency_enable]
                                            [dithering_enable](cmd, orig_v0, orig_v1, orig_v2));
#endif
}

constinit const DrawRectangleFunctionTable DrawRectangleFunctions = {
  {{&DrawRectangle<false, false, false>, &DrawRectangle<false, false, true>},
   {&DrawRectangle<false, false, false>, &DrawRectangle<false, false, true>}},
  {{&DrawRectangle<true, false, false>, &DrawRectangle<true, false, true>},
   {&DrawRectangle<true, true, false>, &DrawRectangle<true, true, true>}}};

constinit const DrawLineFunctionTable DrawLineFunctions = {
  {{&DrawLine<false, false, false>, &DrawLine<false, false, true>},
   {&DrawLine<false, true, false>, &DrawLine<false, true, true>}},
  {{&DrawLine<true, false, false>, &DrawLine<true, false, true>},
   {&DrawLine<true, true, false>, &DrawLine<true, true, true>}}};

constinit const DrawTriangleFunctionTable DrawTriangleFunctions = {
  {{{{&DrawTriangle<false, false, false, false, false>, &DrawTriangle<false, false, false, false, true>},
     {&DrawTriangle<false, false, false, true, false>, &DrawTriangle<false, false, false, true, true>}},
    {{&DrawTriangle<false, false, false, false, false>, &DrawTriangle<false, false, false, false, false>},
     {&DrawTriangle<false, false, false, true, false>, &DrawTriangle<false, false, false, true, false>}}},
   {{{&DrawTriangle<false, true, false, false, false>, &DrawTriangle<false, true, false, false, true>},
     {&DrawTriangle<false, true, false, true, false>, &DrawTriangle<false, true, false, true, true>}},
    {{&DrawTriangle<false, true, true, false, false>, &DrawTriangle<false, true, true, false, false>},
     {&DrawTriangle<false, true, true, true, false>, &DrawTriangle<false, true, true, true, false>}}}},
  {{{{&DrawTriangle<true, false, false, false, false>, &DrawTriangle<true, false, false, false, true>},
     {&DrawTriangle<true, false, false, true, false>, &DrawTriangle<true, false, false, true, true>}},
    {{&DrawTriangle<true, false, false, false, false>, &DrawTriangle<true, false, false, false, false>},
     {&DrawTriangle<true, false, false, true, false>, &DrawTriangle<true, false, false, true, false>}}},
   {{{&DrawTriangle<true, true, false, false, false>, &DrawTriangle<true, true, false, false, true>},
     {&DrawTriangle<true, true, false, true, false>, &DrawTriangle<true, true, false, true, true>}},
    {{&DrawTriangle<true, true, true, false, false>, &DrawTriangle<true, true, true, false, false>},
     {&DrawTriangle<true, true, true, true, false>, &DrawTriangle<true, true, true, true, false>}}}}};

#ifdef __INTELLISENSE__
}
#endif
