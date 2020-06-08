#include "opengl_host_display.h"
#include "common/assert.h"
#include "common/log.h"
#include "libretro.h"
#include "libretro_host_interface.h"
#include <array>
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

OpenGLHostDisplay::OpenGLHostDisplay(bool is_gles) : m_is_gles(is_gles) {}

OpenGLHostDisplay::~OpenGLHostDisplay()
{
  if (m_display_vao != 0)
    glDeleteVertexArrays(1, &m_display_vao);
  if (m_display_linear_sampler != 0)
    glDeleteSamplers(1, &m_display_linear_sampler);
  if (m_display_nearest_sampler != 0)
    glDeleteSamplers(1, &m_display_nearest_sampler);

  m_display_program.Destroy();
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
  return nullptr;
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
  // TODO
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

bool OpenGLHostDisplay::RequestHardwareRendererContext(retro_hw_render_callback* cb)
{
  // Prefer a desktop OpenGL context where possible. If we can't get this, try OpenGL ES.
  static constexpr std::array<std::tuple<u32, u32>, 11> desktop_versions_to_try = {
    {/*{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2}, */ {3, 1}, {3, 0}}};
  static constexpr std::array<std::tuple<u32, u32>, 4> es_versions_to_try = {{{3, 2}, {3, 1}, {3, 0}}};

  cb->cache_context = true;
  cb->bottom_left_origin = true;

  for (const auto& [major, minor] : desktop_versions_to_try)
  {
    if (major > 3 || (major == 3 && minor >= 2))
    {
      cb->context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
      cb->version_major = major;
      cb->version_minor = minor;
    }
    else
    {
      cb->context_type = RETRO_HW_CONTEXT_OPENGL;
      cb->version_major = 0;
      cb->version_minor = 0;
    }

    if (g_retro_environment_callback(RETRO_ENVIRONMENT_SET_HW_RENDER, cb))
      return true;
  }

  for (const auto& [major, minor] : es_versions_to_try)
  {
    if (major >= 3 && minor > 0)
    {
      cb->context_type = RETRO_HW_CONTEXT_OPENGLES_VERSION;
      cb->version_major = major;
      cb->version_minor = minor;
    }
    else
    {
      cb->context_type = RETRO_HW_CONTEXT_OPENGLES3;
      cb->version_major = 0;
      cb->version_minor = 0;
    }

    if (g_retro_environment_callback(RETRO_ENVIRONMENT_SET_HW_RENDER, cb))
      return true;
  }

  Log_ErrorPrint("Failed to set any GL HW renderer");
  return false;
}

std::unique_ptr<HostDisplay> OpenGLHostDisplay::Create(bool debug_device)
{
  const retro_hw_context_type context_type = g_libretro_host_interface.GetHWRenderCallback().context_type;
  const GLADloadproc get_proc_address = [](const char* sym) -> void* {
    return reinterpret_cast<void*>(g_libretro_host_interface.GetHWRenderCallback().get_proc_address(sym));
  };
  const bool is_gles =
    (context_type == RETRO_HW_CONTEXT_OPENGLES3 || context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION);

  // Load GLAD.
  const auto load_result = is_gles ? gladLoadGLES2Loader(get_proc_address) : gladLoadGLLoader(get_proc_address);
  if (!load_result)
  {
    Log_ErrorPrintf("Failed to load GL functions");
    return nullptr;
  }

#if 0
  // Disabled until we can turn it off as well
  if (debug_device && GLAD_GL_KHR_debug)
  {
    glad_glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  }
#endif

  std::unique_ptr<OpenGLHostDisplay> display = std::make_unique<OpenGLHostDisplay>(is_gles);
  if (!display->CreateGLResources())
  {
    Log_ErrorPrint("Failed to create GL resources");
    return nullptr;
  }

  return display;
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

void OpenGLHostDisplay::Render()
{
  if (m_display_width != m_last_display_width || m_display_height != m_last_display_height)
  {
    retro_game_geometry geom = {};
    geom.base_width = m_display_width;
    geom.base_height = m_display_height;
    geom.aspect_ratio = m_display_pixel_aspect_ratio;

    if (!g_retro_environment_callback(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom))
      Log_WarningPrint("RETRO_ENVIRONMENT_SET_GEOMETRY failed");

    m_last_display_width = m_display_width;
    m_last_display_height = m_display_height;
  }

  const GLuint fbo = static_cast<GLuint>(g_libretro_host_interface.GetHWRenderCallback().get_current_framebuffer());

  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  RenderDisplay();

  g_retro_video_refresh_callback(RETRO_HW_FRAME_BUFFER_VALID, m_display_width, m_display_height, 0);

  GL::Program::ResetLastProgram();
}

void OpenGLHostDisplay::RenderDisplay()
{
  if (!m_display_texture_handle)
    return;

  const auto [vp_left, vp_top, vp_width, vp_height] =
    CalculateDrawRect(m_display_width, m_display_height, m_display_top_margin);

  glViewport(vp_left, m_display_height - vp_top - vp_height, vp_width, vp_height);
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
