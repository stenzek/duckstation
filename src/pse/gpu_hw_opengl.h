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

  bool Initialize(System* system, Bus* bus, DMA* dma) override;
  void Reset() override;

protected:
  void UpdateDisplay() override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data) override;
  void UpdateTexturePageTexture() override;
  void FlushRender() override;

private:
  std::tuple<s32, s32> ConvertToFramebufferCoordinates(s32 x, s32 y);

  void CreateFramebuffer();
  void ClearFramebuffer();
  void DestroyFramebuffer();

  void CreateVertexBuffer();

  bool CompilePrograms();
  bool CompileProgram(GL::Program& prog, bool textured, bool blending);

  void SetProgram(bool textured, bool blending);
  void SetViewport();
  void SetScissor();

  std::unique_ptr<GL::Texture> m_framebuffer_texture;
  GLuint m_framebuffer_fbo_id = 0;

  std::unique_ptr<GL::Texture> m_texture_page_texture;
  GLuint m_texture_page_fbo_id = 0;

  GLuint m_vertex_buffer = 0;
  GLuint m_vao_id = 0;
  GLuint m_attributeless_vao_id = 0;

  GL::Program m_texture_program;
  GL::Program m_color_program;
  GL::Program m_blended_texture_program;
  std::array<GL::Program, 3> m_texture_page_programs;
};
