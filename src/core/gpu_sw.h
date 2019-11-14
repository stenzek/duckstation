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
  enum : u32
  {
    MAX_VERTICES_PER_POLYGON = 4
  };

  struct SWVertex
  {
    s32 x, y;
    u8 color_r, color_g, color_b;
    u8 texcoord_x, texcoord_y;
  };

  void ReadVRAM(u32 x, u32 y, u32 width, u32 height) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data) override;
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

  void ShadePixel(RenderCommand rc, u32 x, u32 y, u8 color_r, u8 color_g, u8 color_b, u8 texcoord_x, u8 texcoord_y,
                  bool dithering);
  void DrawTriangle(RenderCommand rc, const SWVertex* v0, const SWVertex* v1, const SWVertex* v2);
  void DrawRectangle(RenderCommand rc, s32 origin_x, s32 origin_y, u32 width, u32 height, u8 r, u8 g, u8 b,
                     u8 origin_texcoord_x, u8 origin_texcoord_y);

  std::vector<u32> m_display_texture_buffer;
  std::unique_ptr<HostDisplayTexture> m_display_texture;

  std::array<SWVertex, MAX_VERTICES_PER_POLYGON> m_vertex_buffer;
  std::array<u16, VRAM_WIDTH * VRAM_HEIGHT> m_vram;
};
