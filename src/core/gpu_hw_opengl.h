#pragma once
#include "common/gl/program.h"
#include "common/gl/stream_buffer.h"
#include "common/gl/texture.h"
#include "glad.h"
#include "gpu_hw.h"
#include <array>
#include <memory>
#include <tuple>

class GPU_HW_OpenGL : public GPU_HW
{
public:
  GPU_HW_OpenGL();
  ~GPU_HW_OpenGL() override;

  bool Initialize(HostDisplay* host_display, System* system, DMA* dma, InterruptController* interrupt_controller,
                  Timers* timers) override;
  void Reset() override;

  void ResetGraphicsAPIState() override;
  void RestoreGraphicsAPIState() override;
  void UpdateSettings() override;

  void DrawRendererStatsWindow() override;

protected:
  void UpdateDisplay() override;
  void UpdateDrawingArea() override;
  void ReadVRAM(u32 x, u32 y, u32 width, u32 height, void* buffer) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) override;
  void FlushRender() override;
  void InvalidateVRAMReadCache() override;
  void MapBatchVertexPointer(u32 required_vertices) override;

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

  void SetCapabilities();
  void CreateFramebuffer();
  void ClearFramebuffer();
  void DestroyFramebuffer();
  void UpdateVRAMReadTexture();

  void CreateVertexBuffer();
  void CreateUniformBuffer();
  void CreateTextureBuffer();

  bool CompilePrograms();
  void SetDrawState(BatchRenderMode render_mode);
  void UploadUniformBlock(const void* data, u32 data_size);

  // downsample texture - used for readbacks at >1xIR.
  std::unique_ptr<GL::Texture> m_vram_texture;
  std::unique_ptr<GL::Texture> m_vram_read_texture;
  std::unique_ptr<GL::Texture> m_vram_downsample_texture;
  std::unique_ptr<GL::Texture> m_display_texture;

  std::unique_ptr<GL::StreamBuffer> m_vertex_stream_buffer;
  GLuint m_vao_id = 0;
  GLuint m_attributeless_vao_id = 0;

  std::unique_ptr<GL::StreamBuffer> m_uniform_stream_buffer;

  std::unique_ptr<GL::StreamBuffer> m_texture_stream_buffer;
  GLuint m_texture_buffer_r16ui_texture = 0;

  u32 m_uniform_buffer_alignment = 1;

  std::array<std::array<std::array<GL::Program, 2>, 9>, 4> m_render_programs; // [render_mode][texture_mode][dithering]
  std::array<std::array<GL::Program, 2>, 2> m_display_programs;               // [depth_24][interlaced]
  GL::Program m_vram_write_program;

  GLStats m_stats = {};
  GLStats m_last_stats = {};

  bool m_vram_read_texture_dirty = true;
  bool m_drawing_area_changed = true;
  bool m_show_renderer_statistics = false;
};
