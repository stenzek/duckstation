#include "opengldisplaywindow.h"
#include "common/assert.h"
#include "common/log.h"
#include "imgui.h"
#include "qthostinterface.h"
#include <QtGui/QKeyEvent>
#include <array>
#include <imgui_impl_opengl3.h>
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

#ifdef WIN32
#include "common/windows_headers.h"
#endif

/// Changes the swap interval on a window. Since Qt doesn't expose this functionality, we need to change it manually
/// ourselves it by calling system-specific functions. Assumes the context is current.
static void SetSwapInterval(QWindow* window, QOpenGLContext* context, int interval)
{
  static QOpenGLContext* last_context = nullptr;

#ifdef WIN32
  static void(WINAPI * wgl_swap_interval_ext)(int) = nullptr;

  if (last_context != context)
  {
    wgl_swap_interval_ext = nullptr;
    last_context = context;

    HMODULE gl_module = GetModuleHandleA("opengl32.dll");
    if (!gl_module)
      return;

    const auto wgl_get_proc_address =
      reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(GetProcAddress(gl_module, "wglGetProcAddress"));
    if (!wgl_get_proc_address)
      return;

    wgl_swap_interval_ext =
      reinterpret_cast<decltype(wgl_swap_interval_ext)>(wgl_get_proc_address("wglSwapIntervalEXT"));
  }

  if (wgl_swap_interval_ext)
    wgl_swap_interval_ext(interval);
#endif
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

OpenGLDisplayWindow::OpenGLDisplayWindow(QtHostInterface* host_interface, QWindow* parent)
  : QtDisplayWindow(host_interface, parent)
{
  setSurfaceType(QWindow::OpenGLSurface);
}

OpenGLDisplayWindow::~OpenGLDisplayWindow() = default;

HostDisplay* OpenGLDisplayWindow::getHostDisplayInterface()
{
  return this;
}

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
  return m_gl_context.get();
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

void OpenGLDisplayWindow::SetVSync(bool enabled)
{
  // Window framebuffer has to be bound to call SetSwapInterval.
  GLint current_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  SetSwapInterval(this, m_gl_context.get(), enabled ? 1 : 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
}

std::tuple<u32, u32> OpenGLDisplayWindow::GetWindowSize() const
{
  return std::make_tuple(static_cast<u32>(m_window_width), static_cast<u32>(m_window_height));
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

bool OpenGLDisplayWindow::createDeviceContext(QThread* worker_thread, bool debug_device)
{
  m_gl_context = std::make_unique<QOpenGLContext>();

  // Prefer a desktop OpenGL context where possible. If we can't get this, try OpenGL ES.
  static constexpr std::array<std::tuple<int, int>, 11> desktop_versions_to_try = {
    {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2}, {3, 1}, {3, 0}}};
  static constexpr std::array<std::tuple<int, int>, 4> es_versions_to_try = {{{3, 2}, {3, 1}, {3, 0}}};

  QSurfaceFormat surface_format = requestedFormat();
  surface_format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
  surface_format.setSwapInterval(0);
  surface_format.setRenderableType(QSurfaceFormat::OpenGL);
  surface_format.setProfile(QSurfaceFormat::CoreProfile);
  if (debug_device)
    surface_format.setOption(QSurfaceFormat::DebugContext);

  for (const auto [major, minor] : desktop_versions_to_try)
  {
    surface_format.setVersion(major, minor);
    m_gl_context->setFormat(surface_format);
    if (m_gl_context->create())
    {
      m_is_gles = m_gl_context->isOpenGLES();
      Log_InfoPrintf("Got a %s %d.%d context", major, minor, m_is_gles ? "OpenGL ES" : "desktop OpenGL");
      break;
    }
  }

  if (!m_gl_context)
  {
    // try forcing ES
    surface_format.setRenderableType(QSurfaceFormat::OpenGLES);
    surface_format.setProfile(QSurfaceFormat::NoProfile);
    if (debug_device)
      surface_format.setOption(QSurfaceFormat::DebugContext, false);

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
    m_gl_context.reset();
    return false;
  }

  if (!m_gl_context->makeCurrent(this))
  {
    Log_ErrorPrintf("Failed to make GL context current on UI thread");
    m_gl_context.reset();
    return false;
  }

  if (!QtDisplayWindow::createDeviceContext(worker_thread, debug_device))
  {
    m_gl_context->doneCurrent();
    m_gl_context.reset();
    return false;
  }

  m_gl_context->doneCurrent();
  m_gl_context->moveToThread(worker_thread);
  return true;
}

bool OpenGLDisplayWindow::initializeDeviceContext(bool debug_device)
{
  if (!m_gl_context->makeCurrent(this))
    return false;

  s_thread_gl_context = m_gl_context.get();

  // Load GLAD.
  const auto load_result =
    m_is_gles ? gladLoadGLES2Loader(GetProcAddressCallback) : gladLoadGLLoader(GetProcAddressCallback);
  if (!load_result)
  {
    Log_ErrorPrintf("Failed to load GL functions");
    s_thread_gl_context = nullptr;
    m_gl_context->doneCurrent();
    return false;
  }

  if (debug_device && GLAD_GL_KHR_debug)
  {
    glad_glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  }

  if (!QtDisplayWindow::initializeDeviceContext(debug_device))
  {
    s_thread_gl_context = nullptr;
    m_gl_context->doneCurrent();
    return false;
  }

  return true;
}

void OpenGLDisplayWindow::destroyDeviceContext()
{
  Assert(m_gl_context && s_thread_gl_context == m_gl_context.get());

  QtDisplayWindow::destroyDeviceContext();

  s_thread_gl_context = nullptr;
  m_gl_context->doneCurrent();
  m_gl_context.reset();
}

bool OpenGLDisplayWindow::createImGuiContext()
{
  if (!QtDisplayWindow::createImGuiContext())
    return false;

  if (!ImGui_ImplOpenGL3_Init(GetGLSLVersionString()))
    return false;

  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();
  return true;
}

void OpenGLDisplayWindow::destroyImGuiContext()
{
  ImGui_ImplOpenGL3_Shutdown();

  QtDisplayWindow::destroyImGuiContext();
}

bool OpenGLDisplayWindow::createDeviceResources()
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

void OpenGLDisplayWindow::destroyDeviceResources()
{
  QtDisplayWindow::destroyDeviceResources();

  if (m_display_vao != 0)
    glDeleteVertexArrays(1, &m_display_vao);
  if (m_display_linear_sampler != 0)
    glDeleteSamplers(1, &m_display_linear_sampler);
  if (m_display_nearest_sampler != 0)
    glDeleteSamplers(1, &m_display_nearest_sampler);

  m_display_program.Destroy();
}

void OpenGLDisplayWindow::Render()
{
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  renderDisplay();

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  m_gl_context->makeCurrent(this);
  m_gl_context->swapBuffers(this);

  ImGui::NewFrame();
  ImGui_ImplOpenGL3_NewFrame();

  GL::Program::ResetLastProgram();
}

void OpenGLDisplayWindow::renderDisplay()
{
  if (!m_display_texture_handle)
    return;

  // - 20 for main menu padding
  const auto [vp_left, vp_top, vp_width, vp_height] =
    CalculateDrawRect(m_window_width, std::max(m_window_height - m_display_top_margin, 1), m_display_aspect_ratio);

  glViewport(vp_left, m_window_height - (m_display_top_margin + vp_top) - vp_height, vp_width, vp_height);
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
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(m_display_texture_handle)));
  glBindSampler(0, m_display_linear_filtering ? m_display_linear_sampler : m_display_nearest_sampler);
  glBindVertexArray(m_display_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(0, 0);
}
