#include "gpu_hw_opengl.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
Log_SetChannel(GPU_HW_OpenGL);

GPU_HW_OpenGL::GPU_HW_OpenGL() : GPU_HW() {}

GPU_HW_OpenGL::~GPU_HW_OpenGL()
{
  DestroyFramebuffer();
}

bool GPU_HW_OpenGL::Initialize(Bus* bus, DMA* dma)
{
  if (!GPU_HW::Initialize(bus, dma))
    return false;

  CreateFramebuffer();
  return true;
}

void GPU_HW_OpenGL::Reset()
{
  GPU_HW::Reset();

  ClearFramebuffer();
}

void GPU_HW_OpenGL::CreateFramebuffer()
{
  glGenTextures(1, &m_framebuffer_texture_id);
  glBindTexture(GL_TEXTURE_2D, m_framebuffer_texture_id);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);

  glGenFramebuffers(1, &m_framebuffer_fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_framebuffer_texture_id, 0);
  Assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
}

void GPU_HW_OpenGL::ClearFramebuffer()
{
  // TODO: get rid of the FBO switches
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GPU_HW_OpenGL::DestroyFramebuffer()
{
  glDeleteFramebuffers(1, &m_framebuffer_fbo_id);
  m_framebuffer_fbo_id = 0;

  glDeleteTextures(1, &m_framebuffer_texture_id);
  m_framebuffer_texture_id = 0;
}

void GPU_HW_OpenGL::DispatchRenderCommand(RenderCommand rc, u32 num_vertices)
{
  LoadVertices(rc, num_vertices);
  if (m_vertex_staging.empty())
    return;

  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);

  if (rc.texture_enable)
    m_texture_program.Bind();
  else
    m_color_program.Bind();

  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(sizeof(HWVertex) * m_vertex_staging.size()),
               m_vertex_staging.data(), GL_STREAM_DRAW);
  glDrawArrays(rc.quad_polygon ? GL_TRIANGLE_STRIP : GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertex_staging.size()));
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

void GPU_HW_OpenGL::Program::Bind()
{
  glUseProgram(program_id);
}

static GLuint CompileShader(GLenum type, const std::string& source)
{
  GLuint id = glCreateShader(type);

  std::array<const GLchar*, 1> sources = {{source.c_str()}};
  std::array<GLint, 1> source_lengths = {{static_cast<GLint>(source.size())}};
  glShaderSource(id, static_cast<GLsizei>(source.size()), sources.data(), source_lengths.data());

  GLint status = GL_FALSE;
  glGetShaderiv(id, GL_COMPILE_STATUS, &status);

  GLint info_log_length = 0;
  glGetShaderiv(id, GL_INFO_LOG_LENGTH, &info_log_length);

  if (status == GL_FALSE || info_log_length > 0)
  {
    std::string info_log;
    info_log.resize(info_log_length + 1);
    glGetShaderInfoLog(id, info_log_length, &info_log_length, info_log.data());

    if (status == GL_TRUE)
    {
      Log_ErrorPrintf("Shader compiled with warnings:\n%s", info_log.c_str());
    }
    else
    {
      Log_ErrorPrintf("Shader failed to compile:\n%s", info_log.c_str());
      glDeleteShader(id);
      return 0;
    }
  }

  return id;
}

bool GPU_HW_OpenGL::Program::Compile(const std::string& vertex_shader, const std::string& fragment_shader)
{
  GLuint vertex_shader_id = CompileShader(GL_VERTEX_SHADER, vertex_shader);
  if (vertex_shader_id == 0)
    return false;

  GLuint fragment_shader_id = CompileShader(GL_FRAGMENT_SHADER, fragment_shader);
  if (fragment_shader_id == 0)
  {
    glDeleteShader(vertex_shader_id);
    return false;
  }

  program_id = glCreateProgram();
  glAttachShader(program_id, vertex_shader_id);
  glAttachShader(program_id, fragment_shader_id);

  glBindAttribLocation(program_id, 0, "a_position");
  glBindAttribLocation(program_id, 1, "a_texcoord");
  glBindAttribLocation(program_id, 2, "a_color");
  glBindFragDataLocation(program_id, 0, "ocol0");

  glLinkProgram(program_id);

  glDeleteShader(vertex_shader_id);
  glDeleteShader(fragment_shader_id);

  GLint status = GL_FALSE;
  glGetProgramiv(program_id, GL_LINK_STATUS, &status);

  GLint info_log_length = 0;
  glGetProgramiv(program_id, GL_INFO_LOG_LENGTH, &info_log_length);

  if (status == GL_FALSE || info_log_length > 0)
  {
    std::string info_log;
    info_log.resize(info_log_length + 1);
    glGetProgramInfoLog(program_id, info_log_length, &info_log_length, info_log.data());

    if (status == GL_TRUE)
    {
      Log_ErrorPrintf("Program linked with warnings:\n%s", info_log.c_str());
    }
    else
    {
      Log_ErrorPrintf("Program failed to link:\n%s", info_log.c_str());
      glDeleteProgram(program_id);
      program_id = 0;
      return false;
    }
  }

  return true;
}
