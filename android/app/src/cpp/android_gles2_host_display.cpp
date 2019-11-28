#include "android_gles2_host_display.h"
#include "YBaseLib/Log.h"
#include <EGL/eglext.h>
#include <array>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <tuple>
Log_SetChannel(AndroidGLES2HostDisplay);

class AndroidGLES2HostDisplayTexture : public HostDisplayTexture
{
public:
  AndroidGLES2HostDisplayTexture(GLuint id, u32 width, u32 height) : m_id(id), m_width(width), m_height(height) {}
  ~AndroidGLES2HostDisplayTexture() override { glDeleteTextures(1, &m_id); }

  void* GetHandle() const override { return reinterpret_cast<void*>(static_cast<uintptr_t>(m_id)); }
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }

  GLuint GetGLID() const { return m_id; }

  static std::unique_ptr<AndroidGLES2HostDisplayTexture> Create(u32 width, u32 height, const void* initial_data,
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
    return std::make_unique<AndroidGLES2HostDisplayTexture>(id, width, height);
  }

private:
  GLuint m_id;
  u32 m_width;
  u32 m_height;
};

AndroidGLES2HostDisplay::AndroidGLES2HostDisplay(ANativeWindow* window)
  : m_window(window), m_window_width(ANativeWindow_getWidth(window)), m_window_height(ANativeWindow_getHeight(window))
{
}

AndroidGLES2HostDisplay::~AndroidGLES2HostDisplay()
{
  if (m_egl_context != EGL_NO_CONTEXT)
  {
    m_display_program.Destroy();
    ImGui_ImplOpenGL3_Shutdown();
    eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(m_egl_display, m_egl_context);
  }

  if (m_egl_surface != EGL_NO_SURFACE)
    eglDestroySurface(m_egl_display, m_egl_surface);
}

HostDisplay::RenderAPI AndroidGLES2HostDisplay::GetRenderAPI() const
{
  return HostDisplay::RenderAPI::OpenGLES;
}

void* AndroidGLES2HostDisplay::GetRenderDevice() const
{
  return nullptr;
}

void* AndroidGLES2HostDisplay::GetRenderContext() const
{
  return m_egl_context;
}

void* AndroidGLES2HostDisplay::GetRenderWindow() const
{
  return m_window;
}

void AndroidGLES2HostDisplay::ChangeRenderWindow(void* new_window)
{
  eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  DestroySurface();

  m_window = static_cast<ANativeWindow*>(new_window);

  if (!CreateSurface())
    Panic("Failed to recreate surface after window change");

  if (!eglMakeCurrent(m_egl_display, m_egl_surface, m_egl_surface, m_egl_context))
    Panic("Failed to make context current after window change");
}

std::unique_ptr<HostDisplayTexture> AndroidGLES2HostDisplay::CreateTexture(u32 width, u32 height, const void* data,
                                                                           u32 data_stride, bool dynamic)
{
  return AndroidGLES2HostDisplayTexture::Create(width, height, data, data_stride);
}

void AndroidGLES2HostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                            const void* data, u32 data_stride)
{
  AndroidGLES2HostDisplayTexture* tex = static_cast<AndroidGLES2HostDisplayTexture*>(texture);
  Assert(data_stride == (width * sizeof(u32)));

  GLint old_texture_binding = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture_binding);

  glBindTexture(GL_TEXTURE_2D, tex->GetGLID());
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);

  glBindTexture(GL_TEXTURE_2D, old_texture_binding);
}

void AndroidGLES2HostDisplay::SetDisplayTexture(void* texture, s32 offset_x, s32 offset_y, s32 width, s32 height,
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

void AndroidGLES2HostDisplay::SetDisplayLinearFiltering(bool enabled)
{
  m_display_linear_filtering = enabled;
}

void AndroidGLES2HostDisplay::SetDisplayTopMargin(int height)
{
  m_display_top_margin = height;
}

void AndroidGLES2HostDisplay::SetVSync(bool enabled)
{
  eglSwapInterval(m_egl_display, enabled ? 1 : 0);
}

std::tuple<u32, u32> AndroidGLES2HostDisplay::GetWindowSize() const
{
  return std::make_tuple(static_cast<u32>(m_window_width), static_cast<u32>(m_window_height));
}

void AndroidGLES2HostDisplay::WindowResized()
{
  m_window_width = ANativeWindow_getWidth(m_window);
  m_window_height = ANativeWindow_getHeight(m_window);
  Log_InfoPrintf("WindowResized %dx%d", m_window_width, m_window_height);
}

const char* AndroidGLES2HostDisplay::GetGLSLVersionString() const
{
  return "#version 100";
}

std::string AndroidGLES2HostDisplay::GetGLSLVersionHeader() const
{
  return R"(
#version 100

precision highp float;
precision highp int;
)";
}

bool AndroidGLES2HostDisplay::CreateGLContext()
{
  m_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!m_egl_display)
  {
    Log_ErrorPrint("eglGetDisplay() failed");
    return false;
  }

  EGLint egl_major_version, egl_minor_version;
  if (!eglInitialize(m_egl_display, &egl_major_version, &egl_minor_version))
  {
    Log_ErrorPrint("eglInitialize() failed");
    return false;
  }

  Log_InfoPrintf("EGL version %d.%d initialized", egl_major_version, egl_minor_version);

  static constexpr std::array<int, 11> egl_surface_attribs = {{EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
                                                               EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_SURFACE_TYPE,
                                                               EGL_WINDOW_BIT, EGL_NONE}};

  int num_m_egl_configs;
  if (!eglChooseConfig(m_egl_display, egl_surface_attribs.data(), &m_egl_config, 1, &num_m_egl_configs))
  {
    Log_ErrorPrint("eglChooseConfig() failed");
    return false;
  }

  eglBindAPI(EGL_OPENGL_ES_API);

  static constexpr std::array<int, 3> egl_context_attribs = {{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE}};
  m_egl_context = eglCreateContext(m_egl_display, m_egl_config, EGL_NO_CONTEXT, egl_context_attribs.data());
  if (!m_egl_context)
  {
    Log_ErrorPrint("eglCreateContext() failed");
    return false;
  }

  if (!CreateSurface())
    return false;

  if (!eglMakeCurrent(m_egl_display, m_egl_surface, m_egl_surface, m_egl_context))
  {
    Log_ErrorPrint("eglMakeCurrent() failed");
    return false;
  }

  // Load GLAD.
  if (!gladLoadGLES2Loader(reinterpret_cast<GLADloadproc>(eglGetProcAddress)))
  {
    Log_ErrorPrintf("Failed to load GL functions");
    return false;
  }

  return true;
}

bool AndroidGLES2HostDisplay::CreateSurface()
{
  EGLint native_visual;
  eglGetConfigAttrib(m_egl_display, m_egl_config, EGL_NATIVE_VISUAL_ID, &native_visual);
  ANativeWindow_setBuffersGeometry(m_window, 0, 0, native_visual);
  m_window_width = ANativeWindow_getWidth(m_window);
  m_window_height = ANativeWindow_getHeight(m_window);

  m_egl_surface = eglCreateWindowSurface(m_egl_display, m_egl_config, m_window, nullptr);
  if (!m_egl_surface)
  {
    Log_ErrorPrint("eglCreateWindowSurface() failed");
    return false;
  }

  WindowResized();
  return true;
}

void AndroidGLES2HostDisplay::DestroySurface()
{
  eglDestroySurface(m_egl_display, m_egl_surface);
  m_egl_surface = EGL_NO_SURFACE;
}

bool AndroidGLES2HostDisplay::CreateImGuiContext()
{
  if (!ImGui_ImplOpenGL3_Init(GetGLSLVersionString()))
    return false;

  ImGui_ImplOpenGL3_NewFrame();
  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_window_width);
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_window_height);
  return true;
}

bool AndroidGLES2HostDisplay::CreateGLResources()
{
  static constexpr char fullscreen_quad_vertex_shader[] = R"(
attribute vec2 a_pos;
attribute vec2 a_tex0;

varying vec2 v_tex0;

void main()
{
  v_tex0 = a_tex0;
  gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

  static constexpr char display_fragment_shader[] = R"(
uniform sampler2D samp0;

varying vec2 v_tex0;

void main()
{
  gl_FragColor = texture2D(samp0, v_tex0);
}
)";

  if (!m_display_program.Compile(GetGLSLVersionHeader() + fullscreen_quad_vertex_shader,
                                 GetGLSLVersionHeader() + display_fragment_shader))
  {
    Log_ErrorPrintf("Failed to compile display shaders");
    return false;
  }

  m_display_program.BindAttribute(0, "a_pos");
  m_display_program.BindAttribute(1, "a_tex0");

  if (!m_display_program.Link())
  {
    Log_ErrorPrintf("Failed to link display program");
    return false;
  }

  m_display_program.Bind();
  m_display_program.RegisterUniform("samp0");
  m_display_program.Uniform1i(0, 0);

  return true;
}

std::unique_ptr<HostDisplay> AndroidGLES2HostDisplay::Create(ANativeWindow* window)
{
  std::unique_ptr<AndroidGLES2HostDisplay> display = std::make_unique<AndroidGLES2HostDisplay>(window);
  if (!display->CreateGLContext() || !display->CreateImGuiContext() || !display->CreateGLResources())
    return nullptr;

  Log_DevPrintf("%dx%d display created", display->m_window_width, display->m_window_height);
  return display;
}

void AndroidGLES2HostDisplay::Render()
{
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  RenderDisplay();

  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  eglSwapBuffers(m_egl_display, m_egl_surface);

  ImGui_ImplOpenGL3_NewFrame();

  GL::Program::ResetLastProgram();
}

void AndroidGLES2HostDisplay::RenderDisplay()
{
  if (!m_display_texture_id)
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

  const float tex_left = static_cast<float>(m_display_offset_x) / static_cast<float>(m_display_texture_width);
  const float tex_right = tex_left + static_cast<float>(m_display_width) / static_cast<float>(m_display_texture_width);
  const float tex_top = static_cast<float>(m_display_offset_y) / static_cast<float>(m_display_texture_height);
  const float tex_bottom =
    tex_top + static_cast<float>(m_display_height) / static_cast<float>(m_display_texture_height);
  const std::array<std::array<float, 4>, 4> vertices = {{
    {{-1.0f, -1.0f, tex_left, tex_bottom}}, // bottom-left
    {{1.0f, -1.0f, tex_right, tex_bottom}}, // bottom-right
    {{-1.0f, 1.0f, tex_left, tex_top}},     // top-left
    {{1.0f, 1.0f, tex_right, tex_top}},     // top-right
  }};

  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(vertices[0]), &vertices[0][0]);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertices[0]), &vertices[0][2]);
  glEnableVertexAttribArray(1);

  glBindTexture(GL_TEXTURE_2D, m_display_texture_id);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  glDisableVertexAttribArray(1);
  glDisableVertexAttribArray(0);
}
