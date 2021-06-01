#pragma once
#include "common/bitfield.h"
#include "common/rectangle.h"
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

  // In interlaced modes, we can exceed the 512 height of VRAM, up to 576 in PAL games.
  GPU_MAX_DISPLAY_WIDTH = 720,
  GPU_MAX_DISPLAY_HEIGHT = 576,

  DITHER_MATRIX_SIZE = 4
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
  Reserved_Direct16Bit = 3,

  // Not register values.
  RawTextureBit = 4,
  RawPalette4Bit = RawTextureBit | Palette4Bit,
  RawPalette8Bit = RawTextureBit | Palette8Bit,
  RawDirect16Bit = RawTextureBit | Direct16Bit,
  Reserved_RawDirect16Bit = RawTextureBit | Reserved_Direct16Bit,

  Disabled = 8 // Not a register value
};

IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(GPUTextureMode);

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
  BitField<u32, bool, 28, 1> shading_enable;                 // 0 - flat, 1 = gouroud
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

// Helper/format conversion functions - constants from https://stackoverflow.com/a/9069480
ALWAYS_INLINE static constexpr u32 VRAMConvert5To8(u32 color)
{
  return (((color * 527u) + 23u) >> 6);
}
ALWAYS_INLINE static constexpr u32 VRAMConvert8To5(u32 color)
{
  return (((color * 249u) + 1014u) >> 11);
}
ALWAYS_INLINE static constexpr u32 VRAMRGBA5551ToRGBA8888(u32 color)
{
  const u32 r = VRAMConvert5To8(color & 31u);
  const u32 g = VRAMConvert5To8((color >> 5) & 31u);
  const u32 b = VRAMConvert5To8((color >> 10) & 31u);
  const u32 a = ((color >> 15) != 0) ? 255 : 0;
  return ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16) | (ZeroExtend32(a) << 24);
}

ALWAYS_INLINE static constexpr u16 VRAMRGBA8888ToRGBA5551(u32 color)
{
  const u32 r = VRAMConvert8To5(color & 0xFFu);
  const u32 g = VRAMConvert8To5((color >> 8) & 0xFFu);
  const u32 b = VRAMConvert8To5((color >> 16) & 0xFFu);
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
  static constexpr u16 TEXTURE_PAGE_MASK = UINT16_C(0b0000000000011111);

  // Polygon texpage commands only affect bits 0-8, 11
  static constexpr u16 POLYGON_TEXPAGE_MASK = 0b0000100111111111;

  // Bits 0..5 are returned in the GPU status register, latched at E1h/polygon draw time.
  static constexpr u32 GPUSTAT_MASK = 0b11111111111;

  u16 bits;

  BitField<u16, u8, 0, 4> texture_page_x_base;
  BitField<u16, u8, 4, 1> texture_page_y_base;
  BitField<u16, GPUTransparencyMode, 5, 2> transparency_mode;
  BitField<u16, GPUTextureMode, 7, 2> texture_mode;
  BitField<u16, bool, 9, 1> dither_enable;
  BitField<u16, bool, 10, 1> draw_to_displayed_field;
  BitField<u16, bool, 11, 1> texture_disable;
  BitField<u16, bool, 12, 1> texture_x_flip;
  BitField<u16, bool, 13, 1> texture_y_flip;

  ALWAYS_INLINE u16 GetTexturePageBaseX() const { return ZeroExtend16(texture_page_x_base.GetValue()) * 64; }
  ALWAYS_INLINE u16 GetTexturePageBaseY() const { return ZeroExtend16(texture_page_y_base.GetValue()) * 256; }

  /// Returns true if the texture mode requires a palette.
  ALWAYS_INLINE bool IsUsingPalette() const { return (bits & (2 << 7)) == 0; }

  /// Returns a rectangle comprising the texture page area.
  ALWAYS_INLINE_RELEASE Common::Rectangle<u32> GetTexturePageRectangle() const
  {
    static constexpr std::array<u32, 4> texture_page_widths = {
      {TEXTURE_PAGE_WIDTH / 4, TEXTURE_PAGE_WIDTH / 2, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_WIDTH}};
    return Common::Rectangle<u32>::FromExtents(GetTexturePageBaseX(), GetTexturePageBaseY(),
                                               texture_page_widths[static_cast<u8>(texture_mode.GetValue())],
                                               TEXTURE_PAGE_HEIGHT);
  }
};

union GPUTexturePaletteReg
{
  static constexpr u16 MASK = UINT16_C(0b0111111111111111);

  u16 bits;

  BitField<u16, u16, 0, 6> x;
  BitField<u16, u16, 6, 9> y;

  ALWAYS_INLINE u32 GetXBase() const { return static_cast<u32>(x) * 16u; }
  ALWAYS_INLINE u32 GetYBase() const { return static_cast<u32>(y); }
};

struct GPUTextureWindow
{
  u8 and_x;
  u8 and_y;
  u8 or_x;
  u8 or_y;
};

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
  DrawPolygon,
  DrawRectangle,
  DrawLine
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
  Common::Rectangle<u32> new_area;
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
