#pragma once
#include "common/heap_array.h"
#include "gpu.h"
#include "host_display.h"
#include <array>
#include <memory>
#include <vector>

class HostDisplayTexture;

class GPU_SW final : public GPU
{
public:
  GPU_SW();
  ~GPU_SW() override;

  bool IsHardwareRenderer() const override;

  bool Initialize(HostDisplay* host_display) override;
  void Reset() override;

  ALWAYS_INLINE_RELEASE u16 GetPixel(const u32 x, const u32 y) const { return m_vram[VRAM_WIDTH * y + x]; }
  ALWAYS_INLINE_RELEASE const u16* GetPixelPtr(const u32 x, const u32 y) const { return &m_vram[VRAM_WIDTH * y + x]; }
  ALWAYS_INLINE_RELEASE u16* GetPixelPtr(const u32 x, const u32 y) { return &m_vram[VRAM_WIDTH * y + x]; }
  ALWAYS_INLINE_RELEASE void SetPixel(const u32 x, const u32 y, const u16 value) { m_vram[VRAM_WIDTH * y + x] = value; }

  // this is actually (31 * 255) >> 4) == 494, but to simplify addressing we use the next power of two (512)
  static constexpr u32 DITHER_LUT_SIZE = 512;
  using DitherLUT = std::array<std::array<std::array<u8, 512>, DITHER_MATRIX_SIZE>, DITHER_MATRIX_SIZE>;
  static constexpr DitherLUT ComputeDitherLUT();

protected:
  struct SWVertex
  {
    s32 x, y;
    u8 r, g, b;
    u8 u, v;

    ALWAYS_INLINE void SetPosition(GPUVertexPosition p, s32 offset_x, s32 offset_y)
    {
      x = TruncateGPUVertexPosition(offset_x + p.x);
      y = TruncateGPUVertexPosition(offset_y + p.y);
    }

    ALWAYS_INLINE void SetColorRGB24(u32 color) { std::tie(r, g, b) = UnpackColorRGB24(color); }
    ALWAYS_INLINE void SetTexcoord(u16 value) { std::tie(u, v) = UnpackTexcoord(value); }
  };

  //////////////////////////////////////////////////////////////////////////
  // Scanout
  //////////////////////////////////////////////////////////////////////////
  template<HostDisplayPixelFormat display_format>
  void CopyOut15Bit(u32 src_x, u32 src_y, u32 width, u32 height, u32 field, bool interlaced, bool interleaved);
  void CopyOut15Bit(HostDisplayPixelFormat display_format, u32 src_x, u32 src_y, u32 width, u32 height, u32 field,
                    bool interlaced, bool interleaved);

  template<HostDisplayPixelFormat display_format>
  void CopyOut24Bit(u32 src_x, u32 src_y, u32 skip_x, u32 width, u32 height, u32 field, bool interlaced,
                    bool interleaved);
  void CopyOut24Bit(HostDisplayPixelFormat display_format, u32 src_x, u32 src_y, u32 skip_x, u32 width, u32 height,
                    u32 field, bool interlaced, bool interleaved);

  void ClearDisplay() override;
  void UpdateDisplay() override;

  //////////////////////////////////////////////////////////////////////////
  // Rasterization
  //////////////////////////////////////////////////////////////////////////

  void DispatchRenderCommand() override;

  template<bool texture_enable, bool raw_texture_enable, bool transparency_enable, bool dithering_enable>
  void ShadePixel(u32 x, u32 y, u8 color_r, u8 color_g, u8 color_b, u8 texcoord_x, u8 texcoord_y);

  template<bool texture_enable, bool raw_texture_enable, bool transparency_enable>
  void DrawRectangle(s32 origin_x, s32 origin_y, u32 width, u32 height, u8 r, u8 g, u8 b, u8 origin_texcoord_x,
                     u8 origin_texcoord_y);

  using DrawRectangleFunction = void (GPU_SW::*)(s32 origin_x, s32 origin_y, u32 width, u32 height, u8 r, u8 g, u8 b,
                                                 u8 origin_texcoord_x, u8 origin_texcoord_y);
  DrawRectangleFunction GetDrawRectangleFunction(bool texture_enable, bool raw_texture_enable,
                                                 bool transparency_enable);

  //////////////////////////////////////////////////////////////////////////
  // Polygon and line rasterization ported from Mednafen
  //////////////////////////////////////////////////////////////////////////
  struct i_deltas
  {
    u32 du_dx, dv_dx;
    u32 dr_dx, dg_dx, db_dx;

    u32 du_dy, dv_dy;
    u32 dr_dy, dg_dy, db_dy;
  };

  struct i_group
  {
    u32 u, v;
    u32 r, g, b;
  };

  template<bool shading_enable, bool texture_enable>
  bool CalcIDeltas(i_deltas& idl, const SWVertex* A, const SWVertex* B, const SWVertex* C);

  template<bool shading_enable, bool texture_enable>
  void AddIDeltas_DX(i_group& ig, const i_deltas& idl, u32 count = 1);

  template<bool shading_enable, bool texture_enable>
  void AddIDeltas_DY(i_group& ig, const i_deltas& idl, u32 count = 1);

  template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
           bool dithering_enable>
  void DrawSpan(s32 y, s32 x_start, s32 x_bound, i_group ig, const i_deltas& idl);

  template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
           bool dithering_enable>
  void DrawTriangle(const SWVertex* v0, const SWVertex* v1, const SWVertex* v2);

  using DrawTriangleFunction = void (GPU_SW::*)(const SWVertex* v0, const SWVertex* v1, const SWVertex* v2);
  DrawTriangleFunction GetDrawTriangleFunction(bool shading_enable, bool texture_enable, bool raw_texture_enable,
                                               bool transparency_enable, bool dithering_enable);

  template<bool shading_enable, bool transparency_enable, bool dithering_enable>
  void DrawLine(const SWVertex* p0, const SWVertex* p1);

  using DrawLineFunction = void (GPU_SW::*)(const SWVertex* p0, const SWVertex* p1);
  DrawLineFunction GetDrawLineFunction(bool shading_enable, bool transparency_enable, bool dithering_enable);

  std::array<u16, VRAM_WIDTH * VRAM_HEIGHT> m_vram;
  HeapArray<u8, VRAM_WIDTH * VRAM_HEIGHT * sizeof(u32)> m_display_texture_buffer;
  HostDisplayPixelFormat m_16bit_display_format = HostDisplayPixelFormat::RGB565;
  HostDisplayPixelFormat m_24bit_display_format = HostDisplayPixelFormat::RGBA8;
};
