#pragma once
#include "common/gl_program.h"
#include "common/gl_texture.h"
#include "glad.h"
#include "gpu_hw.h"
#include <array>
#include <memory>

class GPU_HW_OpenGL : public GPU_HW
{
public:
  GPU_HW_OpenGL();
  ~GPU_HW_OpenGL() override;

  bool Initialize(System* system, Bus* bus, DMA* dma) override;
  void Reset() override;

protected:
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data) override;
  void DispatchRenderCommand(RenderCommand rc, u32 num_vertices) override;
  void FlushRender() override;

private:
  void CreateFramebuffer();
  void ClearFramebuffer();
  void DestroyFramebuffer();

  void CreateVertexBuffer();

  bool CompilePrograms();

  bool SetProgram(bool texture_enable);
  void SetViewport();
  void SetScissor();

  std::unique_ptr<GL::Texture> m_framebuffer_texture;
  GLuint m_framebuffer_fbo_id = 0;

  GLuint m_vertex_buffer = 0;
  GLuint m_vao_id = 0;

  GL::Program m_texture_program;
  GL::Program m_color_program;
};
