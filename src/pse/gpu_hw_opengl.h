#pragma once
#include "gpu_hw.h"
#include "glad.h"
#include <array>

class GPU_HW_OpenGL : public GPU_HW
{
public:
  GPU_HW_OpenGL();
  ~GPU_HW_OpenGL() override;

  bool Initialize(Bus* bus, DMA* dma) override;
  void Reset() override;

protected:
  virtual void DispatchRenderCommand(RenderCommand rc, u32 num_vertices) override;
  virtual void FlushRender() override;

private:
  void CreateFramebuffer();
  void ClearFramebuffer();
  void DestroyFramebuffer();

  void CreateVertexBuffer();

  GLuint m_framebuffer_texture_id = 0;
  GLuint m_framebuffer_fbo_id = 0;

  GLuint m_vertex_buffer = 0;
  GLuint m_vao_id = 0;

  struct Program
  {
    GLuint program_id = 0;

    bool IsValid() const { return program_id != 0; }
    void Bind();
    bool Compile(const std::string& vertex_shader, const std::string& fragment_shader);
  };

  Program m_texture_program;
  Program m_color_program;
};

