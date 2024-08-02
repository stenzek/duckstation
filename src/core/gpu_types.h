// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "common/bitfield.h"
#include "common/bitutils.h"
#include "common/gsvector.h"
#include "types.h"
#include <array>

enum : u32
{
  VRAM_WIDTH = 1024,
  VRAM_HEIGHT = 512,
  VRAM_SIZE = VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16),
  VRAM_WIDTH_MASK = VRAM_WIDTH - 1,
  VRAM_HEIGHT_MASK = VRAM_HEIGHT - 1,
  TEXTURE_PAGE_WIDTH = 256,
  TEXTURE_PAGE_HEIGHT = 256,
  GPU_CLUT_SIZE = 256,

  // In interlaced modes, we can exceed the 512 height of VRAM, up to 576 in PAL games.
  GPU_MAX_DISPLAY_WIDTH = 720,
  GPU_MAX_DISPLAY_HEIGHT = 576,

  DITHER_MATRIX_SIZE = 4,

  VRAM_PAGE_WIDTH = 64,
  VRAM_PAGE_HEIGHT = 256,
  VRAM_PAGES_WIDE = VRAM_WIDTH / VRAM_PAGE_WIDTH,
  VRAM_PAGES_HIGH = VRAM_HEIGHT / VRAM_PAGE_HEIGHT,
  VRAM_PAGE_X_MASK = 0xf,  // 16 pages wide
  VRAM_PAGE_Y_MASK = 0x10, // 2 pages high
  NUM_VRAM_PAGES = VRAM_PAGES_WIDE * VRAM_PAGES_HIGH,
};

enum : s32
{
  MAX_PRIMITIVE_WIDTH = 1024,
  MAX_PRIMITIVE_HEIGHT = 512,
};

enum class GPUPrimitive : u8
{
  Reserved = 0,
  Polygon = 1,
  Line = 2,
  Rectangle = 3
};

enum class GPUDrawRectangleSize : u8
{
  Variable = 0,
  R1x1 = 1,
  R8x8 = 2,
  R16x16 = 3
};

enum class GPUTextureMode : u8
{
  Palette4Bit = 0,
  Palette8Bit = 1,
  Direct16Bit = 2,
  Reserved_Direct16Bit = 3, // Not used.
};

IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(GPUTextureMode);

ALWAYS_INLINE static constexpr bool TextureModeHasPalette(GPUTextureMode mode)
{
  return (mode < GPUTextureMode::Direct16Bit);
}

enum class GPUTransparencyMode : u8
{
  HalfBackgroundPlusHalfForeground = 0,
  BackgroundPlusForeground = 1,
  BackgroundMinusForeground = 2,
  BackgroundPlusQuarterForeground = 3,

  Disabled = 4 // Not a register value
};

enum class GPUInterlacedDisplayMode : u8
{
  None,
  InterleavedFields,
  SeparateFields
};

// NOTE: Inclusive, not exclusive on the upper bounds.
struct GPUDrawingArea
{
  u32 left;
  u32 top;
  u32 right;
  u32 bottom;
};

struct GPUDrawingOffset
{
  s32 x;
  s32 y;
};

union GPURenderCommand
{
  u32 bits;

  BitField<u32, u32, 0, 24> color_for_first_vertex;
  BitField<u32, bool, 24, 1> raw_texture_enable; // not valid for lines
  BitField<u32, bool, 25, 1> transparency_enable;
  BitField<u32, bool, 26, 1> texture_enable;
  BitField<u32, GPUDrawRectangleSize, 27, 2> rectangle_size; // only for rectangles
  BitField<u32, bool, 27, 1> quad_polygon;                   // only for polygons
  BitField<u32, bool, 27, 1> polyline;                       // only for lines
  BitField<u32, bool, 28, 1> shading_enable;                 // 0 - flat, 1 = gouraud
  BitField<u32, GPUPrimitive, 29, 21> primitive;

  /// Returns true if texturing should be enabled. Depends on the primitive type.
  ALWAYS_INLINE bool IsTexturingEnabled() const { return (primitive != GPUPrimitive::Line) ? texture_enable : false; }

  /// Returns true if dithering should be enabled. Depends on the primitive type.
  ALWAYS_INLINE bool IsDitheringEnabled() const
  {
    switch (primitive)
    {
      case GPUPrimitive::Polygon:
        return shading_enable || (texture_enable && !raw_texture_enable);

      case GPUPrimitive::Line:
        return true;

      case GPUPrimitive::Rectangle:
      default:
        return false;
    }
  }
};

ALWAYS_INLINE static constexpr u32 VRAMRGBA5551ToRGBA8888(u32 color)
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

ALWAYS_INLINE static constexpr u16 VRAMRGBA8888ToRGBA5551(u32 color)
{
  const u32 r = (color & 0xFFu) >> 3;
  const u32 g = ((color >> 8) & 0xFFu) >> 3;
  const u32 b = ((color >> 16) & 0xFFu) >> 3;
  const u32 a = ((color >> 24) & 0x01u);
  return Truncate16(r | (g << 5) | (b << 10) | (a << 15));
}

union GPUVertexPosition
{
  u32 bits;

  BitField<u32, s32, 0, 11> x;
  BitField<u32, s32, 16, 11> y;
};

// Sprites/rectangles should be clipped to 12 bits before drawing.
static constexpr s32 TruncateGPUVertexPosition(s32 x)
{
  return SignExtendN<11, s32>(x);
}

// bits in GP0(E1h) or texpage part of polygon
union GPUDrawModeReg
{
  static constexpr u16 MASK = 0b1111111111111;
  static constexpr u16 TEXTURE_MODE_AND_PAGE_MASK = UINT16_C(0b0000000110011111);

  // Polygon texpage commands only affect bits 0-8, 11
  static constexpr u16 POLYGON_TEXPAGE_MASK = 0b0000100111111111;

  // Bits 0..5 are returned in the GPU status register, latched at E1h/polygon draw time.
  static constexpr u32 GPUSTAT_MASK = 0b11111111111;

  u16 bits;

  BitField<u16, u8, 0, 5> texture_page;
  BitField<u16, u8, 0, 4> texture_page_x_base;
  BitField<u16, u8, 4, 1> texture_page_y_base;
  BitField<u16, GPUTransparencyMode, 5, 2> transparency_mode;
  BitField<u16, GPUTextureMode, 7, 2> texture_mode;
  BitField<u16, bool, 9, 1> dither_enable;
  BitField<u16, bool, 10, 1> draw_to_displayed_field;
  BitField<u16, bool, 11, 1> texture_disable;
  BitField<u16, bool, 12, 1> texture_x_flip;
  BitField<u16, bool, 13, 1> texture_y_flip;

  ALWAYS_INLINE u32 GetTexturePageBaseX() const { return ZeroExtend32(texture_page_x_base.GetValue()) * 64; }
  ALWAYS_INLINE u32 GetTexturePageBaseY() const { return ZeroExtend32(texture_page_y_base.GetValue()) * 256; }

  /// Returns true if the texture mode requires a palette.
  ALWAYS_INLINE bool IsUsingPalette() const { return (bits & (2 << 7)) == 0; }
};

union GPUTexturePaletteReg
{
  static constexpr u16 MASK = UINT16_C(0b0111111111111111);

  u16 bits;

  BitField<u16, u16, 0, 6> x;
  BitField<u16, u16, 6, 9> y;

  ALWAYS_INLINE constexpr u32 GetXBase() const { return static_cast<u32>(x) * 16u; }
  ALWAYS_INLINE constexpr u32 GetYBase() const { return static_cast<u32>(y); }
};

struct GPUTextureWindow
{
  u8 and_x;
  u8 and_y;
  u8 or_x;
  u8 or_y;
};

ALWAYS_INLINE static constexpr u32 VRAMPageIndex(u32 px, u32 py)
{
  return ((py * VRAM_PAGES_WIDE) + px);
}
ALWAYS_INLINE static constexpr GSVector4i VRAMPageRect(u32 px, u32 py)
{
  return GSVector4i::cxpr(px * VRAM_PAGE_WIDTH, py * VRAM_PAGE_HEIGHT, (px + 1) * VRAM_PAGE_WIDTH,
                          (py + 1) * VRAM_PAGE_HEIGHT);
}
ALWAYS_INLINE static constexpr GSVector4i VRAMPageRect(u32 pn)
{
  // TODO: Put page rects in a LUT instead?
  return VRAMPageRect(pn % VRAM_PAGES_WIDE, pn / VRAM_PAGES_WIDE);
}

ALWAYS_INLINE static constexpr u32 VRAMCoordinateToPage(u32 x, u32 y)
{
  return VRAMPageIndex(x / VRAM_PAGE_WIDTH, y / VRAM_PAGE_HEIGHT);
}

ALWAYS_INLINE static constexpr u32 VRAMPageStartX(u32 pn)
{
  return (pn % VRAM_PAGES_WIDE) * VRAM_PAGE_WIDTH;
}

ALWAYS_INLINE static constexpr u32 VRAMPageStartY(u32 pn)
{
  return (pn / VRAM_PAGES_WIDE) * VRAM_PAGE_HEIGHT;
}

ALWAYS_INLINE static constexpr u32 ApplyTextureModeShift(GPUTextureMode mode, u32 vram_width)
{
  return vram_width << ((mode < GPUTextureMode::Direct16Bit) ? (2 - static_cast<u8>(mode)) : 0);
}

ALWAYS_INLINE static GSVector4i ApplyTextureModeShift(GPUTextureMode mode, const GSVector4i rect)
{
  return rect.sll32((mode < GPUTextureMode::Direct16Bit) ? (2 - static_cast<u8>(mode)) : 0);
}

ALWAYS_INLINE static constexpr u32 TexturePageCountForMode(GPUTextureMode mode)
{
  return ((mode < GPUTextureMode::Direct16Bit) ? (1 + static_cast<u8>(mode)) : 4);
}

ALWAYS_INLINE static constexpr u32 TexturePageWidthForMode(GPUTextureMode mode)
{
  return TEXTURE_PAGE_WIDTH >> ((mode < GPUTextureMode::Direct16Bit) ? (2 - static_cast<u8>(mode)) : 0);
}

ALWAYS_INLINE static constexpr u32 PalettePageCountForMode(GPUTextureMode mode)
{
  return (mode == GPUTextureMode::Palette4Bit) ? 1 : 4;
}

ALWAYS_INLINE static constexpr u32 PalettePageNumber(GPUTexturePaletteReg reg)
{
  return VRAMCoordinateToPage(reg.GetXBase(), reg.GetYBase());
}

ALWAYS_INLINE static constexpr GSVector4i GetTextureRect(u32 pn, GPUTextureMode mode)
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

/// Returns the maximum index for a paletted texture.
ALWAYS_INLINE static constexpr u32 GetPaletteWidth(GPUTextureMode mode)
{
  return (mode == GPUTextureMode::Palette4Bit ? 16 : ((mode == GPUTextureMode::Palette8Bit) ? 256 : 0));;
}

/// Returns a rectangle comprising the texture palette area.
ALWAYS_INLINE static constexpr GSVector4i GetPaletteRect(GPUTexturePaletteReg palette, GPUTextureMode mode,
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

// 4x4 dither matrix.
static constexpr s32 DITHER_MATRIX[DITHER_MATRIX_SIZE][DITHER_MATRIX_SIZE] = {{-4, +0, -3, +1},  // row 0
                                                                              {+2, -2, +3, -1},  // row 1
                                                                              {-3, +1, -4, +0},  // row 2
                                                                              {+3, -1, +2, -2}}; // row 3

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4200) // warning C4200: nonstandard extension used: zero-sized array in struct/union
#endif

enum class GPUBackendCommandType : u8
{
  Wraparound,
  Sync,
  FillVRAM,
  UpdateVRAM,
  CopyVRAM,
  SetDrawingArea,
  UpdateCLUT,
  DrawPolygon,
  DrawRectangle,
  DrawLine,
};

union GPUBackendCommandParameters
{
  u8 bits;

  BitField<u8, bool, 0, 1> interlaced_rendering;

  /// Returns 0 if the currently-displayed field is on an even line in VRAM, otherwise 1.
  BitField<u8, u8, 1, 1> active_line_lsb;

  BitField<u8, bool, 2, 1> set_mask_while_drawing;
  BitField<u8, bool, 3, 1> check_mask_before_draw;

  ALWAYS_INLINE bool IsMaskingEnabled() const { return (bits & 12u) != 0u; }

  // During transfer/render operations, if ((dst_pixel & mask_and) == 0) { pixel = src_pixel | mask_or }
  u16 GetMaskAND() const
  {
    // return check_mask_before_draw ? 0x8000 : 0x0000;
    return Truncate16((bits << 12) & 0x8000);
  }
  u16 GetMaskOR() const
  {
    // return set_mask_while_drawing ? 0x8000 : 0x0000;
    return Truncate16((bits << 13) & 0x8000);
  }
};

struct GPUBackendCommand
{
  u32 size;
  GPUBackendCommandType type;
  GPUBackendCommandParameters params;
};

struct GPUBackendSyncCommand : public GPUBackendCommand
{
  bool allow_sleep;
};

struct GPUBackendFillVRAMCommand : public GPUBackendCommand
{
  u16 x;
  u16 y;
  u16 width;
  u16 height;
  u32 color;
};

struct GPUBackendUpdateVRAMCommand : public GPUBackendCommand
{
  u16 x;
  u16 y;
  u16 width;
  u16 height;
  u16 data[0];
};

struct GPUBackendCopyVRAMCommand : public GPUBackendCommand
{
  u16 src_x;
  u16 src_y;
  u16 dst_x;
  u16 dst_y;
  u16 width;
  u16 height;
};

struct GPUBackendSetDrawingAreaCommand : public GPUBackendCommand
{
  GPUDrawingArea new_area;
};

struct GPUBackendUpdateCLUTCommand : public GPUBackendCommand
{
  GPUTexturePaletteReg reg;
  bool clut_is_8bit;
};

struct GPUBackendDrawCommand : public GPUBackendCommand
{
  GPUDrawModeReg draw_mode;
  GPURenderCommand rc;
  GPUTexturePaletteReg palette;
  GPUTextureWindow window;

  ALWAYS_INLINE bool IsDitheringEnabled() const { return rc.IsDitheringEnabled() && draw_mode.dither_enable; }
};

struct GPUBackendDrawPolygonCommand : public GPUBackendDrawCommand
{
  u16 num_vertices;

  struct Vertex
  {
    s32 x, y;
    union
    {
      struct
      {
        u8 r, g, b, a;
      };
      u32 color;
    };
    union
    {
      struct
      {
        u8 u, v;
      };
      u16 texcoord;
    };

    ALWAYS_INLINE void Set(s32 x_, s32 y_, u32 color_, u16 texcoord_)
    {
      x = x_;
      y = y_;
      color = color_;
      texcoord = texcoord_;
    }
  };

  Vertex vertices[0];
};

struct GPUBackendDrawRectangleCommand : public GPUBackendDrawCommand
{
  s32 x, y;
  u16 width, height;
  u16 texcoord;
  u32 color;
};

struct GPUBackendDrawLineCommand : public GPUBackendDrawCommand
{
  u16 num_vertices;

  struct Vertex
  {
    s32 x, y;
    union
    {
      struct
      {
        u8 r, g, b, a;
      };
      u32 color;
    };

    ALWAYS_INLINE void Set(s32 x_, s32 y_, u32 color_)
    {
      x = x_;
      y = y_;
      color = color_;
    }
  };

  Vertex vertices[0];
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
