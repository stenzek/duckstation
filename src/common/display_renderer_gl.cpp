#include "display_renderer_gl.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Memory.h"
#include "YBaseLib/String.h"
#include <algorithm>
#include <array>
#include <glad.h>
#include <sstream>

namespace {
class DisplayGL : public Display
{
public:
  DisplayGL(DisplayRenderer* display_manager, const String& name, Type type, u8 priority);
  ~DisplayGL();

  void Render();

private:
  void UpdateFramebufferTexture();

  GLuint m_framebuffer_texture_id = 0;

  u32 m_framebuffer_texture_width = 0;
  u32 m_framebuffer_texture_height = 0;

  std::vector<byte> m_framebuffer_texture_upload_buffer;
};

DisplayGL::DisplayGL(DisplayRenderer* display_manager, const String& name, Type type, u8 priority)
  : Display(display_manager, name, type, priority)
{
  // TODO: Customizable sampler states
}

DisplayGL::~DisplayGL()
{
  if (m_framebuffer_texture_id != 0)
    glDeleteTextures(1, &m_framebuffer_texture_id);
}

void DisplayGL::Render()
{
  if (UpdateFrontbuffer())
    UpdateFramebufferTexture();

  if (m_framebuffer_texture_id == 0)
    return;

  // Assumes that everything is already setup/bound.
  glBindTexture(GL_TEXTURE_2D, m_framebuffer_texture_id);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void DisplayGL::UpdateFramebufferTexture()
{
  if (m_framebuffer_texture_width != m_front_buffer.width || m_framebuffer_texture_height != m_front_buffer.height)
  {
    m_framebuffer_texture_width = m_front_buffer.width;
    m_framebuffer_texture_height = m_front_buffer.height;

    if (m_framebuffer_texture_width > 0 && m_framebuffer_texture_height > 0)
    {
      if (m_framebuffer_texture_id == 0)
        glGenTextures(1, &m_framebuffer_texture_id);

      glBindTexture(GL_TEXTURE_2D, m_framebuffer_texture_id);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_framebuffer_texture_width, m_framebuffer_texture_height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
      // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    else
    {
      if (m_framebuffer_texture_id != 0)
      {
        glDeleteTextures(1, &m_framebuffer_texture_id);
        m_framebuffer_texture_id = 0;
      }
    }
  }

  if (m_framebuffer_texture_id == 0)
    return;

  const u32 upload_stride = m_framebuffer_texture_width * sizeof(u32);
  const size_t required_bytes = size_t(upload_stride) * m_framebuffer_texture_height;
  if (m_framebuffer_texture_upload_buffer.size() != required_bytes)
    m_framebuffer_texture_upload_buffer.resize(required_bytes);

  CopyFramebufferToRGBA8Buffer(&m_front_buffer, m_framebuffer_texture_upload_buffer.data(), upload_stride);

  glBindTexture(GL_TEXTURE_2D, m_framebuffer_texture_id);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_framebuffer_texture_width, m_framebuffer_texture_height, GL_RGBA,
                  GL_UNSIGNED_BYTE, m_framebuffer_texture_upload_buffer.data());
}

} // namespace

DisplayRendererGL::DisplayRendererGL(WindowHandleType window_handle, u32 window_width, u32 window_height)
  : DisplayRenderer(window_handle, window_width, window_height)
{
}

DisplayRendererGL::~DisplayRendererGL() = default;

std::unique_ptr<Display> DisplayRendererGL::CreateDisplay(const char* name, Display::Type type,
                                                          u8 priority /*= Display::DEFAULT_PRIORITY*/)
{
  std::unique_ptr<DisplayGL> display = std::make_unique<DisplayGL>(this, name, type, priority);
  AddDisplay(display.get());
  return display;
}

bool DisplayRendererGL::Initialize()
{
  if (!DisplayRenderer::Initialize())
    return false;

  if (!GLAD_GL_VERSION_2_0)
  {
    Panic("GL version 2.0 not loaded.");
    return false;
  }

  if (!CreateQuadVAO())
  {
    Panic("Failed to create quad VAO");
    return false;
  }

  if (!CreateQuadProgram())
  {
    Panic("Failed to create quad program");
    return false;
  }

  return true;
}

bool DisplayRendererGL::BeginFrame()
{
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  return true;
}

void DisplayRendererGL::RenderDisplays()
{
  std::lock_guard<std::mutex> guard(m_display_lock);

  // Setup GL state.
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glUseProgram(m_quad_program_id);
  BindQuadVAO();

  // How many pixels do we need to render?
  u32 total_width = 0;
  for (const Display* display : m_active_displays)
  {
    auto dim = GetDisplayRenderSize(display);
    total_width += dim.first;
  }

  // Compute the viewport bounds.
  const int window_width = int(m_window_width);
  const int window_height = std::max(1, int(m_window_height) - int(m_top_padding));

  int viewport_x = (window_width - total_width) / 2;
  for (Display* display : m_active_displays)
  {
    auto dim = GetDisplayRenderSize(display);
    const int viewport_width = int(dim.first);
    const int viewport_height = int(dim.second);
    const int viewport_y = ((window_height - viewport_height) / 2) + m_top_padding;
    glViewport(viewport_x, m_window_height - viewport_height - viewport_y, viewport_width, viewport_height);
    static_cast<DisplayGL*>(display)->Render();
    viewport_x += dim.first;
  }
}

void DisplayRendererGL::EndFrame() {}

void DisplayRendererGL::WindowResized(u32 window_width, u32 window_height)
{
  DisplayRenderer::WindowResized(window_width, window_height);
}

DisplayRenderer::BackendType DisplayRendererGL::GetBackendType()
{
  return DisplayRenderer::BackendType::OpenGL;
}

struct QuadVertex
{
  float position[4];
  float texcoord[2];
};

bool DisplayRendererGL::CreateQuadVAO()
{
  static const QuadVertex vertices[4] = {{{1.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
                                         {{-1.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
                                         {{1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
                                         {{-1.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 1.0f}}};

  if (GLAD_GL_VERSION_3_0 || GLAD_GL_ES_VERSION_3_0)
  {
    glGenVertexArrays(1, &m_quad_vao_id);
    glBindVertexArray(m_quad_vao_id);
    glGenBuffers(1, &m_quad_vbo_id);
    glBindBuffer(GL_ARRAY_BUFFER, m_quad_vbo_id);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                          reinterpret_cast<void*>(offsetof(QuadVertex, position[0])));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                          reinterpret_cast<void*>(offsetof(QuadVertex, texcoord[0])));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
  }
  else
  {
    glGenBuffers(1, &m_quad_vbo_id);
    glBindBuffer(GL_ARRAY_BUFFER, m_quad_vbo_id);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }

  return true;
}

void DisplayRendererGL::BindQuadVAO()
{
  if (GLAD_GL_VERSION_3_0 || GLAD_GL_ES_VERSION_3_0)
  {
    glBindVertexArray(m_quad_vao_id);
    return;
  }

  // old-style
  glBindBuffer(GL_ARRAY_BUFFER, m_quad_vbo_id);
  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                        reinterpret_cast<void*>(offsetof(QuadVertex, position[0])));
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                        reinterpret_cast<void*>(offsetof(QuadVertex, texcoord[0])));
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
}

static std::string GenerateQuadVertexShader(const bool old_glsl)
{
  std::stringstream ss;
  if (old_glsl)
  {
    ss << "#version 110\n";
    ss << "attribute vec4 a_position;\n";
    ss << "attribute vec2 a_texcoord;\n";
    ss << "varying vec2 v_texcoord;\n";
  }
  else
  {
    ss << "#version 130\n";
    ss << "in vec4 a_position;\n";
    ss << "in vec2 a_texcoord;\n";
    ss << "out vec2 v_texcoord;\n";
  }

  ss << "void main() {\n";
  ss << "  gl_Position = a_position;\n";
  ss << "  v_texcoord = a_texcoord;\n";
  ss << "}\n";

  return ss.str();
}

static std::string GenerateQuadFragmentShader(const bool old_glsl)
{
  std::stringstream ss;
  if (old_glsl)
  {
    ss << "#version 110\n";
    ss << "varying vec2 v_texcoord;\n";
    ss << "uniform sampler2D samp0;\n";
  }
  else
  {
    ss << "#version 130\n";
    ss << "in vec2 v_texcoord;\n";
    ss << "uniform sampler2D samp0;\n";
    ss << "out vec4 ocol0;\n";
  }

  ss << "void main() {\n";
  if (old_glsl)
    ss << "  gl_FragColor = texture2D(samp0, v_texcoord);\n";
  else
    ss << "  ocol0 = texture(samp0, v_texcoord);\n";
  ss << "}\n";

  return ss.str();
}

bool DisplayRendererGL::CreateQuadProgram()
{
  const bool old_glsl = !GLAD_GL_VERSION_3_2 && !GLAD_GL_ES_VERSION_3_0;
  const std::string vs_str = GenerateQuadVertexShader(old_glsl);
  const std::string fs_str = GenerateQuadFragmentShader(old_glsl);
  const char* vs_str_ptr = vs_str.c_str();
  const GLint vs_length = static_cast<GLint>(vs_str.length());
  const char* fs_str_ptr = fs_str.c_str();
  const GLint fs_length = static_cast<GLint>(fs_str.length());
  GLint param;

  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_str_ptr, &vs_length);
  glCompileShader(vs);
  glGetShaderiv(vs, GL_COMPILE_STATUS, &param);
  if (param != GL_TRUE)
  {
    Panic("Failed to compile vertex shader.");
    return false;
  }

  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &fs_str_ptr, &fs_length);
  glCompileShader(fs);
  glGetShaderiv(fs, GL_COMPILE_STATUS, &param);
  if (param != GL_TRUE)
  {
    Panic("Failed to compile fragment shader.");
    return false;
  }

  m_quad_program_id = glCreateProgram();
  glAttachShader(m_quad_program_id, vs);
  glAttachShader(m_quad_program_id, fs);
  glBindAttribLocation(m_quad_program_id, 0, "a_position");
  glBindAttribLocation(m_quad_program_id, 1, "a_texcoord");
  if (!old_glsl)
    glBindFragDataLocation(m_quad_program_id, 0, "ocol0");
  glLinkProgram(m_quad_program_id);
  glGetProgramiv(m_quad_program_id, GL_LINK_STATUS, &param);
  if (param != GL_TRUE)
  {
    Panic("Failed to link program.");
    return false;
  }

  // Bind texture unit zero to the shader.
  glUseProgram(m_quad_program_id);
  GLint pos = glGetUniformLocation(m_quad_program_id, "samp0");
  if (pos >= 0)
    glUniform1i(pos, 0);

  // Shaders are no longer needed after linking.
  glDeleteShader(vs);
  glDeleteShader(fs);

  glUseProgram(0);
  return true;
}
