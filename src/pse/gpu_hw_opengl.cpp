#include "gpu_hw_opengl.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
#include "host_interface.h"
#include "system.h"
Log_SetChannel(GPU_HW_OpenGL);

GPU_HW_OpenGL::GPU_HW_OpenGL() : GPU_HW() {}

GPU_HW_OpenGL::~GPU_HW_OpenGL()
{
  DestroyFramebuffer();
}

bool GPU_HW_OpenGL::Initialize(System* system, Bus* bus, DMA* dma)
{
  if (!GPU_HW::Initialize(system, bus, dma))
    return false;

  CreateFramebuffer();
  CreateVertexBuffer();
  if (!CompilePrograms())
    return false;

  return true;
}

void GPU_HW_OpenGL::Reset()
{
  GPU_HW::Reset();

  ClearFramebuffer();
}

void GPU_HW_OpenGL::CreateFramebuffer()
{
  m_framebuffer_texture =
    std::make_unique<GL::Texture>(VRAM_WIDTH, VRAM_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false);

  glGenFramebuffers(1, &m_framebuffer_fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_framebuffer_texture->GetGLId(), 0);
  Assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
}

void GPU_HW_OpenGL::ClearFramebuffer()
{
  // TODO: get rid of the FBO switches
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  //m_system->GetHostInterface()->SetDisplayTexture(m_framebuffer_texture.get(), 0, 0, VRAM_WIDTH, VRAM_HEIGHT);
}

void GPU_HW_OpenGL::DestroyFramebuffer()
{
  glDeleteFramebuffers(1, &m_framebuffer_fbo_id);
  m_framebuffer_fbo_id = 0;

  m_framebuffer_texture.reset();
}

void GPU_HW_OpenGL::CreateVertexBuffer()
{
  glGenBuffers(1, &m_vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, 128, nullptr, GL_STREAM_DRAW);

  glGenVertexArrays(1, &m_vao_id);
  glBindVertexArray(m_vao_id);
  glVertexAttribIPointer(0, 2, GL_INT, sizeof(HWVertex), reinterpret_cast<void*>(offsetof(HWVertex, x)));
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, true, sizeof(HWVertex),
                        reinterpret_cast<void*>(offsetof(HWVertex, color)));
  glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(HWVertex), reinterpret_cast<void*>(offsetof(HWVertex, color)));
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glBindVertexArray(0);
}

bool GPU_HW_OpenGL::CompilePrograms()
{
  for (u32 texture_enable_i = 0; texture_enable_i < 2; texture_enable_i++)
  {
    const bool texture_enable = ConvertToBool(texture_enable_i);
    const std::string vs = GenerateVertexShader(texture_enable);
    const std::string fs = GenerateFragmentShader(texture_enable);

    GL::Program& prog = texture_enable ? m_texture_program : m_color_program;
    if (!prog.Compile(vs.c_str(), fs.c_str()))
      return false;

    prog.BindAttribute(0, "a_position");
    prog.BindAttribute(1, "a_color");
    if (texture_enable)
      prog.BindAttribute(2, "a_texcoord");

    prog.BindFragData(0, "ocol0");

    if (!prog.Link())
      return false;
  }

  return true;
}

bool GPU_HW_OpenGL::SetProgram(bool texture_enable)
{
  GL::Program& prog = texture_enable ? m_texture_program : m_color_program;
  if (!prog.IsVaild())
    return false;

  prog.Bind();
  return true;
}

void GPU_HW_OpenGL::SetViewport()
{
  int x, y, width, height;
  CalcViewport(&x, &y, &width, &height);

  y = VRAM_HEIGHT - y - height;
  Log_DebugPrintf("SetViewport: Offset (%d,%d) Size (%d, %d)", x, y, width, height);
  glViewport(x, y, width, height);
}

void GPU_HW_OpenGL::SetScissor() {}

inline u32 ConvertRGBA5551ToRGBA8888(u16 color)
{
  u8 r = Truncate8(color & 31);
  u8 g = Truncate8((color >> 5) & 31);
  u8 b = Truncate8((color >> 10) & 31);
  u8 a = Truncate8((color >> 15) & 1);

  // 00012345 -> 1234545
  b = (b << 3) | (b >> 3);
  g = (g << 3) | (g >> 3);
  r = (r << 3) | (r >> 3);
  a = a ? 255 : 0;

  return ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16) | (ZeroExtend32(a) << 24);
}

void GPU_HW_OpenGL::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data)
{
  const u32 pixel_count = width * height;
  std::vector<u32> rgba_data;
  rgba_data.reserve(pixel_count);

  const u8* source_ptr = static_cast<const u8*>(data);
  for (u32 i = 0; i < pixel_count; i++)
  {
    u16 src_col;
    std::memcpy(&src_col, source_ptr, sizeof(src_col));
    source_ptr += sizeof(src_col);

    const u32 dst_col = ConvertRGBA5551ToRGBA8888(src_col);
    rgba_data.push_back(dst_col);
  }

  m_framebuffer_texture->Bind();
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                  rgba_data.data());
  m_system->GetHostInterface()->SetDisplayTexture(m_framebuffer_texture.get(), 0, 0, VRAM_WIDTH, VRAM_HEIGHT);
}

void GPU_HW_OpenGL::DispatchRenderCommand(RenderCommand rc, u32 num_vertices)
{
  LoadVertices(rc, num_vertices);
  if (m_vertex_staging.empty())
    return;

  if (!SetProgram(rc.texture_enable))
  {
    Log_ErrorPrintf("Failed to set GL program");
    m_vertex_staging.clear();
    return;
  }

  SetViewport();

  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glBindVertexArray(m_vao_id);

  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(sizeof(HWVertex) * m_vertex_staging.size()),
               m_vertex_staging.data(), GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribIPointer(0, 2, GL_INT, sizeof(HWVertex), reinterpret_cast<void*>(offsetof(HWVertex, x)));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, true, sizeof(HWVertex),
                        reinterpret_cast<void*>(offsetof(HWVertex, color)));
  glEnableVertexAttribArray(2);
  glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(HWVertex), reinterpret_cast<void*>(offsetof(HWVertex, color)));

  glDrawArrays(rc.quad_polygon ? GL_TRIANGLE_STRIP : GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertex_staging.size()));

  m_system->GetHostInterface()->SetDisplayTexture(m_framebuffer_texture.get(), 0, 0, VRAM_WIDTH, VRAM_HEIGHT);
  m_vertex_staging.clear();
}

void GPU_HW_OpenGL::FlushRender()
{
  if (m_vertex_staging.empty())
    return;

  m_vertex_staging.clear();
}

std::unique_ptr<GPU> GPU::CreateHardwareOpenGLRenderer()
{
  return std::make_unique<GPU_HW_OpenGL>();
}
