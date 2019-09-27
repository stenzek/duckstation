#pragma once
#include "common/gl_program.h"
#include "common/gl_texture.h"
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

  bool Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, Timers* timers) override;
  void Reset() override;
  void RenderUI() override;

protected:
  void UpdateDisplay() override;
  void ReadVRAM(u32 x, u32 y, u32 width, u32 height, void* buffer) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u16 color) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) override;
  void FlushRender() override;
  void InvalidateVRAMReadCache() override;

private:
  struct GLStats
  {
    u32 num_vram_read_texture_updates;
    u32 num_batches;
    u32 num_vertices;
  };

  std::tuple<s32, s32> ConvertToFramebufferCoordinates(s32 x, s32 y);

  void CreateFramebuffer();
  void ClearFramebuffer();
  void DestroyFramebuffer();
  void UpdateVRAMReadTexture();

  void CreateVertexBuffer();

  bool CompilePrograms();
  bool CompileProgram(GL::Program& prog, bool textured, bool blending, bool transparent, TextureColorMode texture_color_mode);

  void SetProgram();
  void SetViewport();
  void SetScissor();
  void SetBlendState();

  std::unique_ptr<GL::Texture> m_framebuffer_texture;
  GLuint m_framebuffer_fbo_id = 0;

  std::unique_ptr<GL::Texture> m_vram_read_texture;
  GLuint m_vram_read_fbo_id = 0;
  bool m_vram_read_texture_dirty = true;

  std::unique_ptr<GL::Texture> m_display_texture;
  GLuint m_display_fbo_id = 0;

  GLuint m_vertex_buffer = 0;
  GLuint m_vao_id = 0;
  GLuint m_attributeless_vao_id = 0;

  std::array<std::array<std::array<std::array<GL::Program, 3>, 2>, 2>, 2> m_render_programs;
  std::array<GL::Program, 3> m_texture_page_programs;

  GLStats m_stats = {};
  GLStats m_last_stats = {};
  bool m_show_vram = false;
};
