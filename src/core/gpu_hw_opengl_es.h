#pragma once
#include "common/gl/program.h"
#include "common/gl/stream_buffer.h"
#include "common/gl/texture.h"
#include "glad.h"
#include "gpu_hw.h"
#include <array>
#include <memory>
#include <tuple>

class GPU_HW_OpenGL_ES : public GPU_HW
{
public:
  GPU_HW_OpenGL_ES();
  ~GPU_HW_OpenGL_ES() override;

  bool Initialize(HostDisplay* host_display, System* system, DMA* dma, InterruptController* interrupt_controller,
                  Timers* timers) override;
  void Reset() override;

  void ResetGraphicsAPIState() override;
  void RestoreGraphicsAPIState() override;
  void UpdateSettings() override;

protected:
  void UpdateDisplay() override;
  void ReadVRAM(u32 x, u32 y, u32 width, u32 height) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) override;
  void FlushRender() override;
  void MapBatchVertexPointer(u32 required_vertices) override;
  void UpdateVRAMReadTexture() override;

private:
  struct GLStats
  {
    u32 num_batches;
    u32 num_vertices;
    u32 num_vram_reads;
    u32 num_vram_writes;
    u32 num_vram_read_texture_updates;
    u32 num_uniform_buffer_updates;
  };

  std::tuple<s32, s32> ConvertToFramebufferCoordinates(s32 x, s32 y);

  void SetCapabilities(HostDisplay* host_display);
  void CreateFramebuffer();
  void ClearFramebuffer();
  void DestroyFramebuffer();

  bool CompilePrograms();
  void SetVertexPointers();
  void SetDrawState(BatchRenderMode render_mode);
  void SetScissorFromDrawingArea();

  // downsample texture - used for readbacks at >1xIR.
  std::unique_ptr<GL::Texture> m_vram_texture;
  std::unique_ptr<GL::Texture> m_vram_read_texture;
  std::unique_ptr<GL::Texture> m_vram_encoding_texture;
  std::unique_ptr<GL::Texture> m_display_texture;

  std::vector<BatchVertex> m_vertex_buffer;

  std::array<std::array<std::array<GL::Program, 2>, 9>, 4> m_render_programs; // [render_mode][texture_mode][dithering]
  std::array<std::array<GL::Program, 2>, 2> m_display_programs;               // [depth_24][interlaced]
  GL::Program m_vram_read_program;
};
