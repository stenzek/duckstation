#include "opengldisplaywindow.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
#include <array>
#include <tuple>
Log_SetChannel(OpenGLDisplayWindow);

static thread_local QOpenGLContext* s_thread_gl_context;

static void* GetProcAddressCallback(const char* name)
{
  QOpenGLContext* ctx = s_thread_gl_context;
  if (!ctx)
    return nullptr;

  return (void*)ctx->getProcAddress(name);
}

class OpenGLHostDisplayTexture : public HostDisplayTexture
{
public:
  OpenGLHostDisplayTexture(GLuint id, u32 width, u32 height) : m_id(id), m_width(width), m_height(height) {}
  ~OpenGLHostDisplayTexture() override { glDeleteTextures(1, &m_id); }

  void* GetHandle() const override { return reinterpret_cast<void*>(static_cast<uintptr_t>(m_id)); }
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }

  GLuint GetGLID() const { return m_id; }

  static std::unique_ptr<OpenGLHostDisplayTexture> Create(u32 width, u32 height, const void* initial_data,
                                                          u32 initial_data_stride)
  {
    GLuint id;
    glGenTextures(1, &id);

    GLint old_texture_binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture_binding);

    // TODO: Set pack width
    Assert(!initial_data || initial_data_stride == (width * sizeof(u32)));

    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, initial_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, id);
    return std::make_unique<OpenGLHostDisplayTexture>(id, width, height);
  }

private:
  GLuint m_id;
  u32 m_width;
  u32 m_height;
};

OpenGLDisplayWindow::OpenGLDisplayWindow(QWindow* parent) : QWindow(parent)
{
  setSurfaceType(QWindow::OpenGLSurface);
}

OpenGLDisplayWindow::~OpenGLDisplayWindow() = default;

HostDisplay::RenderAPI OpenGLDisplayWindow::GetRenderAPI() const
{
  return HostDisplay::RenderAPI::OpenGL;
}

void* OpenGLDisplayWindow::GetRenderDevice() const
{
  return nullptr;
}

void* OpenGLDisplayWindow::GetRenderContext() const
{
  return m_gl_context;
}

void* OpenGLDisplayWindow::GetRenderWindow() const
{
  return const_cast<QWindow*>(static_cast<const QWindow*>(this));
}

void OpenGLDisplayWindow::ChangeRenderWindow(void* new_window)
{
  Panic("Not implemented");
}

std::unique_ptr<HostDisplayTexture> OpenGLDisplayWindow::CreateTexture(u32 width, u32 height, const void* data,
                                                                       u32 data_stride, bool dynamic)
{
  return OpenGLHostDisplayTexture::Create(width, height, data, data_stride);
}

void OpenGLDisplayWindow::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                        const void* data, u32 data_stride)
{
  OpenGLHostDisplayTexture* tex = static_cast<OpenGLHostDisplayTexture*>(texture);
  Assert(data_stride == (width * sizeof(u32)));

  GLint old_texture_binding = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture_binding);

  glBindTexture(GL_TEXTURE_2D, tex->GetGLID());
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);

  glBindTexture(GL_TEXTURE_2D, old_texture_binding);
}

void OpenGLDisplayWindow::SetDisplayTexture(void* texture, s32 offset_x, s32 offset_y, s32 width, s32 height,
                                            u32 texture_width, u32 texture_height, float aspect_ratio)
{
  m_display_texture_id = static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture));
  m_display_offset_x = offset_x;
  m_display_offset_y = offset_y;
  m_display_width = width;
  m_display_height = height;
  m_display_texture_width = texture_width;
  m_display_texture_height = texture_height;
  m_display_aspect_ratio = aspect_ratio;
  m_display_texture_changed = true;
}

void OpenGLDisplayWindow::SetDisplayLinearFiltering(bool enabled)
{
  m_display_linear_filtering = enabled;
}

void OpenGLDisplayWindow::SetDisplayTopMargin(int height)
{
  m_display_top_margin = height;
}

void OpenGLDisplayWindow::SetVSync(bool enabled)
{
  // Window framebuffer has to be bound to call SetSwapInterval.
  GLint current_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  // SDL_GL_SetSwapInterval(enabled ? 1 : 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
}

std::tuple<u32, u32> OpenGLDisplayWindow::GetWindowSize() const
{
  const QSize s = size();
  return std::make_tuple(static_cast<u32>(s.width()), static_cast<u32>(s.height()));
}

void OpenGLDisplayWindow::WindowResized() {}

const char* OpenGLDisplayWindow::GetGLSLVersionString() const
{
  return m_is_gles ? "#version 300 es" : "#version 130\n";
}

std::string OpenGLDisplayWindow::GetGLSLVersionHeader() const
{
  std::string header = GetGLSLVersionString();
  header += "\n\n";
  if (m_is_gles)
  {
    header += "precision highp float;\n";
    header += "precision highp int;\n\n";
  }

  return header;
}

static void APIENTRY GLDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                     const GLchar* message, const void* userParam)
{
  switch (severity)
  {
    case GL_DEBUG_SEVERITY_HIGH_KHR:
      Log_ErrorPrintf(message);
      break;
    case GL_DEBUG_SEVERITY_MEDIUM_KHR:
      Log_WarningPrint(message);
      break;
    case GL_DEBUG_SEVERITY_LOW_KHR:
      Log_InfoPrintf(message);
      break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
      // Log_DebugPrint(message);
      break;
  }
}

bool OpenGLDisplayWindow::createGLContext(QThread* worker_thread)
{
  m_gl_context = new QOpenGLContext();

  // Prefer a desktop OpenGL context where possible. If we can't get this, try OpenGL ES.
  static constexpr std::array<std::tuple<int, int>, 11> desktop_versions_to_try = {
    {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2}, {3, 1}, {3, 0}}};
  static constexpr std::array<std::tuple<int, int>, 4> es_versions_to_try = {{{3, 2}, {3, 1}, {3, 0}}};

  QSurfaceFormat surface_format = requestedFormat();
  surface_format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
  surface_format.setSwapInterval(0);
  surface_format.setRenderableType(QSurfaceFormat::OpenGL);
  surface_format.setProfile(QSurfaceFormat::CoreProfile);

#ifdef _DEBUG
  surface_format.setOption(QSurfaceFormat::DebugContext);
#endif

  for (const auto [major, minor] : desktop_versions_to_try)
  {
    surface_format.setVersion(major, minor);
    m_gl_context->setFormat(surface_format);
    if (m_gl_context->create())
    {
      Log_InfoPrintf("Got a desktop OpenGL %d.%d context", major, minor);
      break;
    }
  }

  if (!m_gl_context)
  {
    // try es
    surface_format.setRenderableType(QSurfaceFormat::OpenGLES);
    surface_format.setProfile(QSurfaceFormat::NoProfile);
#ifdef _DEBUG
    surface_format.setOption(QSurfaceFormat::DebugContext, false);
#endif

    for (const auto [major, minor] : es_versions_to_try)
    {
      surface_format.setVersion(major, minor);
      m_gl_context->setFormat(surface_format);
      if (m_gl_context->create())
      {
        Log_InfoPrintf("Got a OpenGL ES %d.%d context", major, minor);
        m_is_gles = true;
        break;
      }
    }
  }

  if (!m_gl_context->isValid())
  {
    Log_ErrorPrintf("Failed to create any GL context");
    delete m_gl_context;
    m_gl_context = nullptr;
    return false;
  }

  if (!m_gl_context->makeCurrent(this))
  {
    Log_ErrorPrintf("Failed to make GL context current on UI thread");
    delete m_gl_context;
    m_gl_context = nullptr;
    return false;
  }

  m_gl_context->doneCurrent();
  m_gl_context->moveToThread(worker_thread);
  return true;
}

bool OpenGLDisplayWindow::initializeGLContext()
{
  if (!m_gl_context->makeCurrent(this))
    return false;

  s_thread_gl_context = m_gl_context;

  // Load GLAD.
  const auto load_result =
    m_is_gles ? gladLoadGLES2Loader(GetProcAddressCallback) : gladLoadGLLoader(GetProcAddressCallback);
  if (!load_result)
  {
    Log_ErrorPrintf("Failed to load GL functions");
    return false;
  }

#if 1
  if (GLAD_GL_KHR_debug)
  {
    glad_glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  }
#endif

  if (!CreateImGuiContext() || !CreateGLResources())
  {
    s_thread_gl_context = nullptr;
    m_gl_context->doneCurrent();
    return false;
  }

  return true;
}

void OpenGLDisplayWindow::destroyGLContext()
{
  Assert(m_gl_context && s_thread_gl_context == m_gl_context);
  s_thread_gl_context = nullptr;

  if (m_display_vao != 0)
    glDeleteVertexArrays(1, &m_display_vao);
  if (m_display_linear_sampler != 0)
    glDeleteSamplers(1, &m_display_linear_sampler);
  if (m_display_nearest_sampler != 0)
    glDeleteSamplers(1, &m_display_nearest_sampler);

  m_display_program.Destroy();

  m_gl_context->doneCurrent();
  delete m_gl_context;
  m_gl_context = nullptr;
}

bool OpenGLDisplayWindow::CreateImGuiContext()
{
  return true;
}

bool OpenGLDisplayWindow::CreateGLResources()
{
  static constexpr char fullscreen_quad_vertex_shader[] = R"(
uniform vec4 u_src_rect;
out vec2 v_tex0;

void main()
{
  vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  v_tex0 = u_src_rect.xy + pos * u_src_rect.zw;
  gl_Position = vec4(pos * vec2(2.0f, -2.0f) + vec2(-1.0f, 1.0f), 0.0f, 1.0f);
}
)";

  static constexpr char display_fragment_shader[] = R"(
uniform sampler2D samp0;

in vec2 v_tex0;
out vec4 o_col0;

void main()
{
  o_col0 = texture(samp0, v_tex0);
}
)";

  if (!m_display_program.Compile(GetGLSLVersionHeader() + fullscreen_quad_vertex_shader,
                                 GetGLSLVersionHeader() + display_fragment_shader))
  {
    Log_ErrorPrintf("Failed to compile display shaders");
    return false;
  }

  if (!m_is_gles)
    m_display_program.BindFragData(0, "o_col0");

  if (!m_display_program.Link())
  {
    Log_ErrorPrintf("Failed to link display program");
    return false;
  }

  m_display_program.Bind();
  m_display_program.RegisterUniform("u_src_rect");
  m_display_program.RegisterUniform("samp0");
  m_display_program.Uniform1i(1, 0);

  glGenVertexArrays(1, &m_display_vao);

  // samplers
  glGenSamplers(1, &m_display_nearest_sampler);
  glSamplerParameteri(m_display_nearest_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glSamplerParameteri(m_display_nearest_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenSamplers(1, &m_display_linear_sampler);
  glSamplerParameteri(m_display_linear_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(m_display_linear_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  return true;
}

void OpenGLDisplayWindow::Render()
{
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  RenderDisplay();

  // ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  m_gl_context->makeCurrent(this);
  m_gl_context->swapBuffers(this);

  // ImGui_ImplSDL2_NewFrame(m_window);
  // ImGui_ImplOpenGL3_NewFrame();

  GL::Program::ResetLastProgram();
}

void OpenGLDisplayWindow::RenderDisplay()
{
  if (!m_display_texture_id)
    return;

  // - 20 for main menu padding
  const QSize window_size = size();
  const auto [vp_left, vp_top, vp_width, vp_height] = CalculateDrawRect(
    window_size.width(), std::max(window_size.height() - m_display_top_margin, 1), m_display_aspect_ratio);

  glViewport(vp_left, window_size.height() - (m_display_top_margin + vp_top) - vp_height, vp_width, vp_height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  m_display_program.Bind();
  m_display_program.Uniform4f(0, static_cast<float>(m_display_offset_x) / static_cast<float>(m_display_texture_width),
                              static_cast<float>(m_display_offset_y) / static_cast<float>(m_display_texture_height),
                              static_cast<float>(m_display_width) / static_cast<float>(m_display_texture_width),
                              static_cast<float>(m_display_height) / static_cast<float>(m_display_texture_height));
  glBindTexture(GL_TEXTURE_2D, m_display_texture_id);
  glBindSampler(0, m_display_linear_filtering ? m_display_linear_sampler : m_display_nearest_sampler);
  glBindVertexArray(m_display_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(0, 0);
}
