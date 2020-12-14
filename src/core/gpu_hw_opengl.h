#pragma once
#include "common/gl/program.h"
#include "common/gl/shader_cache.h"
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

  bool Initialize(HostDisplay* host_display) override;
  void Reset() override;

  void ResetGraphicsAPIState() override;
  void RestoreGraphicsAPIState() override;
  void UpdateSettings() override;

protected:
  void ClearDisplay() override;
  void UpdateDisplay() override;
  void ReadVRAM(u32 x, u32 y, u32 width, u32 height) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) override;
  void UpdateVRAMReadTexture() override;
  void UpdateDepthBufferFromMaskBit() override;
  void SetScissorFromDrawingArea() override;
  void MapBatchVertexPointer(u32 required_vertices) override;
  void UnmapBatchVertexPointer(u32 used_vertices) override;
  void UploadUniformBuffer(const void* data, u32 data_size) override;
  void DrawBatchVertices(BatchRenderMode render_mode, u32 base_vertex, u32 num_vertices) override;

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

  ALWAYS_INLINE bool IsGLES() const { return (m_render_api == HostDisplay::RenderAPI::OpenGLES); }

  std::tuple<s32, s32> ConvertToFramebufferCoordinates(s32 x, s32 y);

  void SetCapabilities(HostDisplay* host_display);
  bool CreateFramebuffer();
  void ClearFramebuffer();

  bool CreateVertexBuffer();
  bool CreateUniformBuffer();
  bool CreateTextureBuffer();

  bool CompilePrograms();

  void SetDepthFunc();
  void SetBlendMode();

  // downsample texture - used for readbacks at >1xIR.
  GL::Texture m_vram_texture;
  GL::Texture m_vram_depth_texture;
  GL::Texture m_vram_read_texture;
  GL::Texture m_vram_encoding_texture;
  GL::Texture m_display_texture;

  std::unique_ptr<GL::StreamBuffer> m_vertex_stream_buffer;
  GLuint m_vram_fbo_id = 0;
  GLuint m_vao_id = 0;
  GLuint m_attributeless_vao_id = 0;

  std::unique_ptr<GL::StreamBuffer> m_uniform_stream_buffer;

  std::unique_ptr<GL::StreamBuffer> m_texture_stream_buffer;
  GLuint m_texture_buffer_r16ui_texture = 0;

  std::array<std::array<std::array<std::array<GL::Program, 2>, 2>, 9>, 4>
    m_render_programs;                                          // [render_mode][texture_mode][dithering][interlacing]
  std::array<std::array<GL::Program, 3>, 2> m_display_programs; // [depth_24][interlaced]
  GL::Program m_vram_interlaced_fill_program;
  GL::Program m_vram_read_program;
  GL::Program m_vram_write_program;
  GL::Program m_vram_copy_program;
  GL::Program m_vram_update_depth_program;

  u32 m_uniform_buffer_alignment = 1;
  u32 m_max_texture_buffer_size = 0;

  bool m_supports_texture_buffer = false;
  bool m_supports_geometry_shaders = false;
  bool m_use_ssbo_for_vram_writes = false;

  bool m_current_check_mask_before_draw = false;
  GPUTransparencyMode m_current_transparency_mode = GPUTransparencyMode::Disabled;
  BatchRenderMode m_current_render_mode = BatchRenderMode::TransparencyDisabled;
};
