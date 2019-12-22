#pragma once
#include "gpu.h"
#include <array>
#include <memory>
#include <vector>

class HostDisplayTexture;

class GPU_SW final : public GPU
{
public:
  GPU_SW();
  ~GPU_SW() override;

  bool Initialize(HostDisplay* host_display, System* system, DMA* dma, InterruptController* interrupt_controller,
                  Timers* timers) override;
  void Reset() override;

  u16 GetPixel(u32 x, u32 y) const { return m_vram[VRAM_WIDTH * y + x]; }
  const u16* GetPixelPtr(u32 x, u32 y) const { return &m_vram[VRAM_WIDTH * y + x]; }
  u16* GetPixelPtr(u32 x, u32 y) { return &m_vram[VRAM_WIDTH * y + x]; }
  void SetPixel(u32 x, u32 y, u16 value) { m_vram[VRAM_WIDTH * y + x] = value; }

protected:
  struct SWVertex
  {
    s32 x, y;
    u8 color_r, color_g, color_b;
    u8 texcoord_x, texcoord_y;

    ALWAYS_INLINE void SetPosition(VertexPosition p)
    {
      x = p.x;
      y = p.y;
    }

    ALWAYS_INLINE void SetColorRGB24(u32 color) { std::tie(color_r, color_g, color_b) = UnpackColorRGB24(color); }
    ALWAYS_INLINE void SetTexcoord(u16 value) { std::tie(texcoord_x, texcoord_y) = UnpackTexcoord(value); }
  };

  void ReadVRAM(u32 x, u32 y, u32 width, u32 height) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) override;

  //////////////////////////////////////////////////////////////////////////
  // Scanout
  //////////////////////////////////////////////////////////////////////////
  static void CopyOut15Bit(const u16* src_ptr, u32 src_stride, u32* dst_ptr, u32 dst_stride, u32 width, u32 height);

  static void CopyOut24Bit(const u16* src_ptr, u32 src_stride, u32* dst_ptr, u32 dst_stride, u32 width, u32 height);

  void UpdateDisplay() override;

  //////////////////////////////////////////////////////////////////////////
  // Rasterization
  //////////////////////////////////////////////////////////////////////////

  void DispatchRenderCommand(RenderCommand rc, u32 num_vertices, const u32* command_ptr) override;

  static bool IsClockwiseWinding(const SWVertex* v0, const SWVertex* v1, const SWVertex* v2);

  template<bool texture_enable, bool raw_texture_enable, bool transparency_enable, bool dithering_enable>
  void ShadePixel(u32 x, u32 y, u8 color_r, u8 color_g, u8 color_b, u8 texcoord_x, u8 texcoord_y);

  template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
           bool dithering_enable>
  void DrawTriangle(const SWVertex* v0, const SWVertex* v1, const SWVertex* v2);

  using DrawTriangleFunction = void (GPU_SW::*)(const SWVertex* v0, const SWVertex* v1, const SWVertex* v2);
  DrawTriangleFunction GetDrawTriangleFunction(bool shading_enable, bool texture_enable, bool raw_texture_enable,
                                               bool transparency_enable, bool dithering_enable);

  template<bool texture_enable, bool raw_texture_enable, bool transparency_enable>
  void DrawRectangle(s32 origin_x, s32 origin_y, u32 width, u32 height, u8 r, u8 g, u8 b, u8 origin_texcoord_x,
                     u8 origin_texcoord_y);

  using DrawRectangleFunction = void (GPU_SW::*)(s32 origin_x, s32 origin_y, u32 width, u32 height, u8 r, u8 g, u8 b,
                                                 u8 origin_texcoord_x, u8 origin_texcoord_y);
  DrawRectangleFunction GetDrawRectangleFunction(bool texture_enable, bool raw_texture_enable,
                                                 bool transparency_enable);

  template<bool shading_enable, bool transparency_enable, bool dithering_enable>
  void DrawLine(const SWVertex* p0, const SWVertex* p1);

  using DrawLineFunction = void (GPU_SW::*)(const SWVertex* p0, const SWVertex* p1);
  DrawLineFunction GetDrawLineFunction(bool shading_enable, bool transparency_enable, bool dithering_enable);

  std::vector<u32> m_display_texture_buffer;
  std::unique_ptr<HostDisplayTexture> m_display_texture;

  std::array<u16, VRAM_WIDTH * VRAM_HEIGHT> m_vram;
};
