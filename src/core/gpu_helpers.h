// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_types.h"

#include "util/gpu_types.h"

#include "common/bitutils.h"
#include "common/gsvector.h"

ALWAYS_INLINE static constexpr bool TextureModeHasPalette(GPUTextureMode mode)
{
  return (mode < GPUTextureMode::Direct16Bit);
}

ALWAYS_INLINE constexpr u32 VRAMRGBA5551ToRGBA8888(u32 color)
{
  // Helper/format conversion functions - constants from https://stackoverflow.com/a/9069480
#define E5TO8(color) ((((color) * 527u) + 23u) >> 6)

  const u32 r = E5TO8(color & 31u);
  const u32 g = E5TO8((color >> 5) & 31u);
  const u32 b = E5TO8((color >> 10) & 31u);
  const u32 a = ((color >> 15) != 0) ? 255 : 0;
  return ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16) | (ZeroExtend32(a) << 24);

#undef E5TO8
}

ALWAYS_INLINE constexpr u16 VRAMRGBA8888ToRGBA5551(u32 color)
{
  const u32 r = (color & 0xFFu) >> 3;
  const u32 g = ((color >> 8) & 0xFFu) >> 3;
  const u32 b = ((color >> 16) & 0xFFu) >> 3;
  const u32 a = ((color >> 24) & 0x01u);
  return Truncate16(r | (g << 5) | (b << 10) | (a << 15));
}

#ifdef CPU_ARCH_SIMD

ALWAYS_INLINE GSVector4i VRAM5BitTo8Bit(GSVector4i val)
{
  return val.mul32l(GSVector4i::cxpr(527)).add32(GSVector4i::cxpr(23)).srl32<6>();
}

ALWAYS_INLINE GSVector4i VRAMRGB5A1ToRGBA8888(GSVector4i val)
{
  static constexpr GSVector4i cmask = GSVector4i::cxpr(0x1F);

  const GSVector4i r = VRAM5BitTo8Bit(val & cmask);
  const GSVector4i g = VRAM5BitTo8Bit((val.srl32<5>() & cmask));
  const GSVector4i b = VRAM5BitTo8Bit((val.srl32<10>() & cmask));
  const GSVector4i a = val.srl32<15>().sll32<31>().sra32<7>();

  return r | g.sll32<8>() | b.sll32<16>() | a;
}

template<GPUTextureFormat format>
ALWAYS_INLINE void ConvertVRAMPixels(u8*& dest, GSVector4i c16)
{
  if constexpr (format == GPUTextureFormat::RGBA8)
  {
    const GSVector4i low = VRAMRGB5A1ToRGBA8888(c16.upl16());
    const GSVector4i high = VRAMRGB5A1ToRGBA8888(c16.uph16());

    GSVector4i::store<false>(dest, low);
    dest += sizeof(GSVector4i);

    GSVector4i::store<false>(dest, high);
    dest += sizeof(GSVector4i);
  }
  else if constexpr (format == GPUTextureFormat::RGB5A1)
  {
    static constexpr GSVector4i cmask = GSVector4i::cxpr16(0x1F);

    const GSVector4i repacked =
      (c16 & GSVector4i::cxpr16(static_cast<s16>(0x83E0))) | (c16.srl16<10>() & cmask) | (c16 & cmask).sll16<10>();

    GSVector4i::store<false>(dest, repacked);
    dest += sizeof(GSVector4i);
  }
  else if constexpr (format == GPUTextureFormat::A1BGR5)
  {
    const GSVector4i repacked = (c16 & GSVector4i::cxpr16(static_cast<s16>(0x3E0))).sll16<1>() |
                                (c16.srl16<9>() & GSVector4i::cxpr16(0x3E)) |
                                (c16 & GSVector4i::cxpr16(0x1F)).sll16<11>() | c16.srl16<15>();

    GSVector4i::store<false>(dest, repacked);
    dest += sizeof(GSVector4i);
  }
  else if constexpr (format == GPUTextureFormat::RGB565)
  {
    constexpr GSVector4i single_mask = GSVector4i::cxpr16(0x1F);
    const GSVector4i a = (c16 & GSVector4i::cxpr16(0x3E0)).sll16<1>(); // (value & 0x3E0) << 1
    const GSVector4i b = (c16 & GSVector4i::cxpr16(0x20)).sll16<1>();  // (value & 0x20) << 1
    const GSVector4i c = (c16.srl16<10>() & single_mask);              // ((value >> 10) & 0x1F)
    const GSVector4i d = (c16 & single_mask).sll16<11>();              // ((value & 0x1F) << 11)
    GSVector4i::store<false>(dest, (((a | b) | c) | d));
    dest += sizeof(GSVector4i);
  }
}

#endif

template<GPUTextureFormat format>
ALWAYS_INLINE void ConvertVRAMPixel(u8*& dest, u16 c16)
{
  if constexpr (format == GPUTextureFormat::RGBA8)
  {
    const u32 c32 = VRAMRGBA5551ToRGBA8888(c16);
    std::memcpy(std::assume_aligned<sizeof(c32)>(dest), &c32, sizeof(c32));
    dest += sizeof(c32);
  }
  else if constexpr (format == GPUTextureFormat::RGB5A1)
  {
    const u16 repacked = (c16 & 0x83E0) | ((c16 >> 10) & 0x1F) | ((c16 & 0x1F) << 10);
    std::memcpy(std::assume_aligned<sizeof(repacked)>(dest), &repacked, sizeof(repacked));
    dest += sizeof(repacked);
  }
  else if constexpr (format == GPUTextureFormat::A1BGR5)
  {
    const u16 repacked = ((c16 & 0x3E0) << 1) | ((c16 >> 9) & 0x3E) | ((c16 & 0x1F) << 11) | (c16 >> 15);
    std::memcpy(std::assume_aligned<sizeof(repacked)>(dest), &repacked, sizeof(repacked));
    dest += sizeof(repacked);
  }
  else if constexpr (format == GPUTextureFormat::RGB565)
  {
    const u16 repacked = ((c16 & 0x3E0) << 1) | ((c16 & 0x20) << 1) | ((c16 >> 10) & 0x1F) | ((c16 & 0x1F) << 11);
    std::memcpy(std::assume_aligned<sizeof(repacked)>(dest), &repacked, sizeof(repacked));
    dest += sizeof(repacked);
  }
}

// Sprites/rectangles should be clipped to 11 bits before drawing.
inline constexpr s32 TruncateGPUVertexPosition(s32 x)
{
  return SignExtendN<11, s32>(x);
}

ALWAYS_INLINE constexpr u32 VRAMPageIndex(u32 px, u32 py)
{
  return ((py * VRAM_PAGES_WIDE) + px);
}
ALWAYS_INLINE constexpr GSVector4i VRAMPageRect(u32 px, u32 py)
{
  return GSVector4i::cxpr(px * VRAM_PAGE_WIDTH, py * VRAM_PAGE_HEIGHT, (px + 1) * VRAM_PAGE_WIDTH,
                          (py + 1) * VRAM_PAGE_HEIGHT);
}
ALWAYS_INLINE constexpr GSVector4i VRAMPageRect(u32 pn)
{
  // TODO: Put page rects in a LUT instead?
  return VRAMPageRect(pn % VRAM_PAGES_WIDE, pn / VRAM_PAGES_WIDE);
}

ALWAYS_INLINE constexpr u32 VRAMCoordinateToPage(u32 x, u32 y)
{
  return VRAMPageIndex(x / VRAM_PAGE_WIDTH, y / VRAM_PAGE_HEIGHT);
}

ALWAYS_INLINE constexpr u32 VRAMPageStartX(u32 pn)
{
  return (pn % VRAM_PAGES_WIDE) * VRAM_PAGE_WIDTH;
}

ALWAYS_INLINE constexpr u32 VRAMPageStartY(u32 pn)
{
  return (pn / VRAM_PAGES_WIDE) * VRAM_PAGE_HEIGHT;
}

ALWAYS_INLINE constexpr u8 GetTextureModeShift(GPUTextureMode mode)
{
  return ((mode < GPUTextureMode::Direct16Bit) ? (2 - static_cast<u8>(mode)) : 0);
}

ALWAYS_INLINE constexpr u32 ApplyTextureModeShift(GPUTextureMode mode, u32 vram_width)
{
  return vram_width << GetTextureModeShift(mode);
}

ALWAYS_INLINE GSVector4i ApplyTextureModeShift(GPUTextureMode mode, const GSVector4i rect)
{
  return rect.sll32(GetTextureModeShift(mode));
}

ALWAYS_INLINE constexpr u32 TexturePageCountForMode(GPUTextureMode mode)
{
  return ((mode < GPUTextureMode::Direct16Bit) ? (1 + static_cast<u8>(mode)) : 4);
}

ALWAYS_INLINE constexpr u32 TexturePageWidthForMode(GPUTextureMode mode)
{
  return TEXTURE_PAGE_WIDTH >> GetTextureModeShift(mode);
}

ALWAYS_INLINE constexpr bool TexturePageIsWrapping(GPUTextureMode mode, u32 pn)
{
  return ((VRAMPageStartX(pn) + TexturePageWidthForMode(mode)) > VRAM_WIDTH);
}

ALWAYS_INLINE constexpr u32 PalettePageCountForMode(GPUTextureMode mode)
{
  return (mode == GPUTextureMode::Palette4Bit) ? 1 : 4;
}

ALWAYS_INLINE constexpr u32 PalettePageNumber(GPUTexturePaletteReg reg)
{
  return VRAMCoordinateToPage(reg.GetXBase(), reg.GetYBase());
}

ALWAYS_INLINE constexpr GSVector4i GetTextureRect(u32 pn, GPUTextureMode mode)
{
  u32 left = VRAMPageStartX(pn);
  u32 top = VRAMPageStartY(pn);
  u32 right = left + TexturePageWidthForMode(mode);
  u32 bottom = top + VRAM_PAGE_HEIGHT;
  if (right > VRAM_WIDTH) [[unlikely]]
  {
    left = 0;
    right = VRAM_WIDTH;
  }
  if (bottom > VRAM_HEIGHT) [[unlikely]]
  {
    top = 0;
    bottom = VRAM_HEIGHT;
  }

  return GSVector4i::cxpr(left, top, right, bottom);
}

ALWAYS_INLINE constexpr GSVector4i GetTextureRectWithoutWrap(u32 pn, GPUTextureMode mode)
{
  const u32 left = VRAMPageStartX(pn);
  const u32 top = VRAMPageStartY(pn);
  const u32 right = std::min<u32>(left + TexturePageWidthForMode(mode), VRAM_WIDTH);
  const u32 bottom = top + VRAM_PAGE_HEIGHT;
  return GSVector4i::cxpr(left, top, right, bottom);
}

/// Returns the maximum index for a paletted texture.
ALWAYS_INLINE constexpr u32 GetPaletteWidth(GPUTextureMode mode)
{
  return (mode == GPUTextureMode::Palette4Bit ? 16 : ((mode == GPUTextureMode::Palette8Bit) ? 256 : 0));
}

/// Returns a rectangle comprising the texture palette area.
ALWAYS_INLINE constexpr GSVector4i GetPaletteRect(GPUTexturePaletteReg palette, GPUTextureMode mode,
                                                  bool clamp_instead_of_wrapping = false)
{
  const u32 width = GetPaletteWidth(mode);
  u32 left = palette.GetXBase();
  u32 top = palette.GetYBase();
  u32 right = left + width;
  u32 bottom = top + 1;
  if (right > VRAM_WIDTH) [[unlikely]]
  {
    right = VRAM_WIDTH;
    left = clamp_instead_of_wrapping ? left : 0;
  }
  return GSVector4i::cxpr(left, top, right, bottom);
}
