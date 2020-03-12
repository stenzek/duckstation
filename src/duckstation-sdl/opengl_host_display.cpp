#include "opengl_host_display.h"
#include "common/assert.h"
#include "common/log.h"
#include "imgui_impl_sdl.h"
#include <array>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <tuple>
Log_SetChannel(OpenGLHostDisplay);

class OpenGLDisplayWidgetTexture : public HostDisplayTexture
{
public:
  OpenGLDisplayWidgetTexture(GLuint id, u32 width, u32 height) : m_id(id), m_width(width), m_height(height) {}
  ~OpenGLDisplayWidgetTexture() override { glDeleteTextures(1, &m_id); }

  void* GetHandle() const override { return reinterpret_cast<void*>(static_cast<uintptr_t>(m_id)); }
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }

  GLuint GetGLID() const { return m_id; }

  static std::unique_ptr<OpenGLDisplayWidgetTexture> Create(u32 width, u32 height, const void* initial_data,
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
    return std::make_unique<OpenGLDisplayWidgetTexture>(id, width, height);
  }

private:
  GLuint m_id;
  u32 m_width;
  u32 m_height;
};

OpenGLHostDisplay::OpenGLHostDisplay(SDL_Window* window) : m_window(window)
{
  SDL_GetWindowSize(window, &m_window_width, &m_window_height);
}

OpenGLHostDisplay::~OpenGLHostDisplay()
{
  if (m_gl_context)
  {
    if (m_display_vao != 0)
      glDeleteVertexArrays(1, &m_display_vao);
    if (m_display_linear_sampler != 0)
      glDeleteSamplers(1, &m_display_linear_sampler);
    if (m_display_nearest_sampler != 0)
      glDeleteSamplers(1, &m_display_nearest_sampler);

    m_display_program.Destroy();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    SDL_GL_MakeCurrent(nullptr, nullptr);
    SDL_GL_DeleteContext(m_gl_context);
  }

  if (m_window)
    SDL_DestroyWindow(m_window);
}

HostDisplay::RenderAPI OpenGLHostDisplay::GetRenderAPI() const
{
  return m_is_gles ? HostDisplay::RenderAPI::OpenGLES : HostDisplay::RenderAPI::OpenGL;
}

void* OpenGLHostDisplay::GetRenderDevice() const
{
  return nullptr;
}

void* OpenGLHostDisplay::GetRenderContext() const
{
  return m_gl_context;
}

void* OpenGLHostDisplay::GetRenderWindow() const
{
  return m_window;
}

void OpenGLHostDisplay::ChangeRenderWindow(void* new_window)
{
  Panic("Not implemented");
}

void OpenGLHostDisplay::WindowResized(s32 new_window_width, s32 new_window_height)
{
  HostDisplay::WindowResized(new_window_width, new_window_height);
  SDL_GL_GetDrawableSize(m_window, &m_window_width, &m_window_height);
  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_window_width);
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_window_height);
}

std::unique_ptr<HostDisplayTexture> OpenGLHostDisplay::CreateTexture(u32 width, u32 height, const void* data,
                                                                     u32 data_stride, bool dynamic)
{
  return OpenGLDisplayWidgetTexture::Create(width, height, data, data_stride);
}

void OpenGLHostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                      const void* data, u32 data_stride)
{
  OpenGLDisplayWidgetTexture* tex = static_cast<OpenGLDisplayWidgetTexture*>(texture);
  Assert(data_stride == (width * sizeof(u32)));

  GLint old_texture_binding = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture_binding);

  glBindTexture(GL_TEXTURE_2D, tex->GetGLID());
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);

  glBindTexture(GL_TEXTURE_2D, old_texture_binding);
}

void OpenGLHostDisplay::SetVSync(bool enabled)
{
  // Window framebuffer has to be bound to call SetSwapInterval.
  GLint current_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  SDL_GL_SetSwapInterval(enabled ? 1 : 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
}

const char* OpenGLHostDisplay::GetGLSLVersionString() const
{
  if (m_is_gles)
  {
    if (GLAD_GL_ES_VERSION_3_0)
      return "#version 300 es";
    else
      return "#version 100";
  }
  else
  {
    if (GLAD_GL_VERSION_3_3)
      return "#version 330";
    else
      return "#version 130";
  }
}

std::string OpenGLHostDisplay::GetGLSLVersionHeader() const
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

bool OpenGLHostDisplay::CreateGLContext(bool debug_device)
{
  // Prefer a desktop OpenGL context where possible. If we can't get this, try OpenGL ES.
  static constexpr std::array<std::tuple<int, int>, 11> desktop_versions_to_try = {
    {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2}, {3, 1}, {3, 0}}};
  static constexpr std::array<std::tuple<int, int>, 4> es_versions_to_try = {{{3, 2}, {3, 1}, {3, 0}}};

  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  if (debug_device)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);

  for (const auto [major, minor] : desktop_versions_to_try)
  {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);

    Log_InfoPrintf("Trying a Desktop OpenGL %d.%d context", major, minor);
    m_gl_context = SDL_GL_CreateContext(m_window);
    if (m_gl_context)
    {
      Log_InfoPrintf("Got a desktop OpenGL %d.%d context", major, minor);
      break;
    }
  }

  if (!m_gl_context)
  {
    // try es
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);

    for (const auto [major, minor] : es_versions_to_try)
    {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);

      Log_InfoPrintf("Trying a OpenGL ES %d.%d context", major, minor);
      m_gl_context = SDL_GL_CreateContext(m_window);
      if (m_gl_context)
      {
        Log_InfoPrintf("Got a OpenGL ES %d.%d context", major, minor);
        m_is_gles = true;
        break;
      }
    }
  }

  if (!m_gl_context || SDL_GL_MakeCurrent(m_window, m_gl_context) != 0)
  {
    Log_ErrorPrintf("Failed to create any GL context");
    return false;
  }

  // Load GLAD.
  const auto load_result =
    m_is_gles ? gladLoadGLES2Loader(SDL_GL_GetProcAddress) : gladLoadGLLoader(SDL_GL_GetProcAddress);
  if (!load_result)
  {
    Log_ErrorPrintf("Failed to load GL functions");
    return false;
  }

  if (debug_device && GLAD_GL_KHR_debug)
  {
    glad_glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  }

  // this can change due to retina scaling on macos?
  SDL_GL_GetDrawableSize(m_window, &m_window_width, &m_window_height);

  // start with vsync on
  SDL_GL_SetSwapInterval(1);
  return true;
}

bool OpenGLHostDisplay::CreateImGuiContext()
{
  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_window_width);
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_window_height);

  if (!ImGui_ImplSDL2_InitForOpenGL(m_window, m_gl_context) || !ImGui_ImplOpenGL3_Init(GetGLSLVersionString()))
    return false;

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame(m_window);
  return true;
}

bool OpenGLHostDisplay::CreateGLResources()
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

std::unique_ptr<HostDisplay> OpenGLHostDisplay::Create(SDL_Window* window, bool debug_device)
{
  std::unique_ptr<OpenGLHostDisplay> display = std::make_unique<OpenGLHostDisplay>(window);
  if (!display->CreateGLContext(debug_device) || !display->CreateImGuiContext() || !display->CreateGLResources())
    return nullptr;

  return display;
}

void OpenGLHostDisplay::Render()
{
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  RenderDisplay();

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  SDL_GL_SwapWindow(m_window);

  ImGui::NewFrame();
  ImGui_ImplSDL2_NewFrame(m_window);
  ImGui_ImplOpenGL3_NewFrame();

  GL::Program::ResetLastProgram();
}

void OpenGLHostDisplay::RenderDisplay()
{
  if (!m_display_texture_handle)
    return;

  const auto [vp_left, vp_top, vp_width, vp_height] = CalculateDrawRect();

  glViewport(vp_left, m_window_height - vp_top - vp_height, vp_width, vp_height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  m_display_program.Bind();
  m_display_program.Uniform4f(
    0, (static_cast<float>(m_display_texture_view_x) + 0.25f) / static_cast<float>(m_display_texture_width),
    (static_cast<float>(m_display_texture_view_y) - 0.25f) / static_cast<float>(m_display_texture_height),
    (static_cast<float>(m_display_texture_view_width) - 0.5f) / static_cast<float>(m_display_texture_width),
    (static_cast<float>(m_display_texture_view_height) + 0.5f) / static_cast<float>(m_display_texture_height));
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(m_display_texture_handle)));
  glBindSampler(0, m_display_linear_filtering ? m_display_linear_sampler : m_display_nearest_sampler);
  glBindVertexArray(m_display_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(0, 0);
}
