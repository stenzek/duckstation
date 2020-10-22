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
  VRAM_COORD_MASK = 0x3FF,
  TEXTURE_PAGE_WIDTH = 256,
  TEXTURE_PAGE_HEIGHT = 256,
  MAX_PRIMITIVE_WIDTH = 1024,
  MAX_PRIMITIVE_HEIGHT = 512,
  DITHER_MATRIX_SIZE = 4
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

// Helper/format conversion functions.
static constexpr u32 RGBA5551ToRGBA8888(u16 color)
{
  u8 r = Truncate8(color & 31);
  u8 g = Truncate8((color >> 5) & 31);
  u8 b = Truncate8((color >> 10) & 31);
  u8 a = Truncate8((color >> 15) & 1);

  // 00012345 -> 1234545
  b = (b << 3) | (b & 0b111);
  g = (g << 3) | (g & 0b111);
  r = (r << 3) | (r & 0b111);
  a = a ? 255 : 0;

  return ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16) | (ZeroExtend32(a) << 24);
}

static constexpr u16 RGBA8888ToRGBA5551(u32 color)
{
  const u16 r = Truncate16((color >> 3) & 0x1Fu);
  const u16 g = Truncate16((color >> 11) & 0x1Fu);
  const u16 b = Truncate16((color >> 19) & 0x1Fu);
  const u16 a = Truncate16((color >> 31) & 0x01u);

  return r | (g << 5) | (b << 10) | (a << 15);
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
  bool IsUsingPalette() const { return (bits & (2 << 7)) == 0; }

  /// Returns a rectangle comprising the texture page area.
  Common::Rectangle<u32> GetTexturePageRectangle() const
  {
    static constexpr std::array<u32, 4> texture_page_widths = {
      {TEXTURE_PAGE_WIDTH / 4, TEXTURE_PAGE_WIDTH / 2, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_WIDTH} };
    return Common::Rectangle<u32>::FromExtents(GetTexturePageBaseX(), GetTexturePageBaseY(),
      texture_page_widths[static_cast<u8>(texture_mode.GetValue())],
      TEXTURE_PAGE_HEIGHT);
  }

  /// Returns a rectangle comprising the texture palette area.
  Common::Rectangle<u32> GetTexturePaletteRectangle() const
  {
    static constexpr std::array<u32, 4> palette_widths = { {16, 256, 0, 0} };
    return Common::Rectangle<u32>::FromExtents(GetTexturePageBaseX(), GetTexturePageBaseY(),
      palette_widths[static_cast<u8>(texture_mode.GetValue())], 1);
  }
};

union GPUTexturePaletteReg
{
  static constexpr u16 MASK = UINT16_C(0b0111111111111111);

  u16 bits;

  BitField<u16, u16, 0, 6> x;
  BitField<u16, u16, 6, 10> y;

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
static constexpr s32 DITHER_MATRIX[DITHER_MATRIX_SIZE][DITHER_MATRIX_SIZE] = { {-4, +0, -3, +1},  // row 0
                                                                              {+2, -2, +3, -1},  // row 1
                                                                              {-3, +1, -4, +0},  // row 2
                                                                              {+4, -1, +2, -2} }; // row 3
