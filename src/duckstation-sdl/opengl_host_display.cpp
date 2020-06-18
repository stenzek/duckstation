#include "opengl_host_display.h"
#include "common/assert.h"
#include "common/log.h"
#include "imgui_impl_sdl.h"
#include "sdl_util.h"
#include <SDL_syswm.h>
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
    m_gl_context.reset();
  }

  if (m_window)
    SDL_DestroyWindow(m_window);
}

HostDisplay::RenderAPI OpenGLHostDisplay::GetRenderAPI() const
{
  return m_gl_context->IsGLES() ? HostDisplay::RenderAPI::OpenGLES : HostDisplay::RenderAPI::OpenGL;
}

void* OpenGLHostDisplay::GetRenderDevice() const
{
  return nullptr;
}

void* OpenGLHostDisplay::GetRenderContext() const
{
  return m_gl_context.get();
}

void OpenGLHostDisplay::WindowResized(s32 new_window_width, s32 new_window_height)
{
  HostDisplay::WindowResized(new_window_width, new_window_height);
  m_gl_context->ResizeSurface(static_cast<u32>(new_window_width), static_cast<u32>(new_window_height));
  m_window_width = static_cast<s32>(m_gl_context->GetSurfaceWidth());
  m_window_height = static_cast<s32>(m_gl_context->GetSurfaceHeight());
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
  Assert((data_stride % sizeof(u32)) == 0);

  GLint old_texture_binding = 0, old_alignment = 0, old_row_length = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture_binding);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_alignment);
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &old_row_length);

  glBindTexture(GL_TEXTURE_2D, tex->GetGLID());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, data_stride / sizeof(u32));

  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);

  glPixelStorei(GL_UNPACK_ALIGNMENT, old_alignment);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, old_row_length);
  glBindTexture(GL_TEXTURE_2D, old_texture_binding);
}

bool OpenGLHostDisplay::DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                        u32 out_data_stride)
{
  GLint old_alignment = 0, old_row_length = 0;
  glGetIntegerv(GL_PACK_ALIGNMENT, &old_alignment);
  glGetIntegerv(GL_PACK_ROW_LENGTH, &old_row_length);
  glPixelStorei(GL_PACK_ALIGNMENT, sizeof(u32));
  glPixelStorei(GL_PACK_ROW_LENGTH, out_data_stride / sizeof(u32));

  const GLuint texture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture_handle));
  GL::Texture::GetTextureSubImage(texture, 0, x, y, 0, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                  height * out_data_stride, out_data);

  glPixelStorei(GL_PACK_ALIGNMENT, old_alignment);
  glPixelStorei(GL_PACK_ROW_LENGTH, old_row_length);
  return true;
}

void OpenGLHostDisplay::SetVSync(bool enabled)
{
  // Window framebuffer has to be bound to call SetSwapInterval.
  GLint current_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  m_gl_context->SetSwapInterval(enabled ? 1 : 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
}

const char* OpenGLHostDisplay::GetGLSLVersionString() const
{
  if (m_gl_context->IsGLES())
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
  if (m_gl_context->IsGLES())
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
  std::optional<WindowInfo> wi = SDLUtil::GetWindowInfoForSDLWindow(m_window);
  if (!wi)
    return false;

  m_gl_context = GL::Context::Create(wi.value());
  if (!m_gl_context)
  {
    Log_ErrorPrintf("Failed to create a GL context of any kind.");
    return false;
  }

  if (debug_device && GLAD_GL_KHR_debug)
  {
    glad_glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  }

  // this can change due to retina scaling on macos?
  m_window_width = static_cast<s32>(m_gl_context->GetSurfaceWidth());
  m_window_height = static_cast<s32>(m_gl_context->GetSurfaceHeight());

  // start with vsync on
  m_gl_context->SetSwapInterval(1);
  return true;
}

bool OpenGLHostDisplay::CreateImGuiContext()
{
  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_window_width);
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_window_height);

  if (!ImGui_ImplSDL2_InitForOpenGL(m_window, nullptr) || !ImGui_ImplOpenGL3_Init(GetGLSLVersionString()))
    return false;

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame(m_window);
  ImGui::NewFrame();
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

  if (!m_display_program.Compile(GetGLSLVersionHeader() + fullscreen_quad_vertex_shader, {},
                                 GetGLSLVersionHeader() + display_fragment_shader))
  {
    Log_ErrorPrintf("Failed to compile display shaders");
    return false;
  }

  if (!m_gl_context->IsGLES())
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

  m_gl_context->SwapBuffers();

  ImGui::NewFrame();
  ImGui_ImplSDL2_NewFrame(m_window);
  ImGui_ImplOpenGL3_NewFrame();

  GL::Program::ResetLastProgram();
}

void OpenGLHostDisplay::RenderDisplay()
{
  if (!m_display_texture_handle)
    return;

  const auto [vp_left, vp_top, vp_width, vp_height] =
    CalculateDrawRect(m_window_width, m_window_height, m_display_top_margin);

  glViewport(vp_left, m_window_height - vp_top - vp_height, vp_width, vp_height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  m_display_program.Bind();
  m_display_program.Uniform4f(
    0, static_cast<float>(m_display_texture_view_x) / static_cast<float>(m_display_texture_width),
    static_cast<float>(m_display_texture_view_y) / static_cast<float>(m_display_texture_height),
    (static_cast<float>(m_display_texture_view_width) - 0.5f) / static_cast<float>(m_display_texture_width),
    (static_cast<float>(m_display_texture_view_height) + 0.5f) / static_cast<float>(m_display_texture_height));
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(m_display_texture_handle)));
  glBindSampler(0, m_display_linear_filtering ? m_display_linear_sampler : m_display_nearest_sampler);
  glBindVertexArray(m_display_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(0, 0);
}
