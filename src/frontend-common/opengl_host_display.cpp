#include "opengl_host_display.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common_host_interface.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "postprocessing_shadergen.h"
#include <array>
#include <tuple>
Log_SetChannel(OpenGLHostDisplay);

namespace FrontendCommon {

class OpenGLHostDisplayTexture : public HostDisplayTexture
{
public:
  OpenGLHostDisplayTexture(GL::Texture texture, HostDisplayPixelFormat format)
    : m_texture(std::move(texture)), m_format(format)
  {
  }
  ~OpenGLHostDisplayTexture() override = default;

  void* GetHandle() const override { return reinterpret_cast<void*>(static_cast<uintptr_t>(m_texture.GetGLId())); }
  u32 GetWidth() const override { return m_texture.GetWidth(); }
  u32 GetHeight() const override { return m_texture.GetHeight(); }
  u32 GetLayers() const override { return 1; }
  u32 GetLevels() const override { return 1; }
  u32 GetSamples() const override { return m_texture.GetSamples(); }
  HostDisplayPixelFormat GetFormat() const override { return m_format; }

  GLuint GetGLID() const { return m_texture.GetGLId(); }

private:
  GL::Texture m_texture;
  HostDisplayPixelFormat m_format;
};

OpenGLHostDisplay::OpenGLHostDisplay() = default;

OpenGLHostDisplay::~OpenGLHostDisplay()
{
  AssertMsg(!m_gl_context, "Context should have been destroyed by now");
}

HostDisplay::RenderAPI OpenGLHostDisplay::GetRenderAPI() const
{
  return m_gl_context->IsGLES() ? RenderAPI::OpenGLES : RenderAPI::OpenGL;
}

void* OpenGLHostDisplay::GetRenderDevice() const
{
  return nullptr;
}

void* OpenGLHostDisplay::GetRenderContext() const
{
  return m_gl_context.get();
}

static const std::tuple<GLenum, GLenum, GLenum>& GetPixelFormatMapping(bool is_gles, HostDisplayPixelFormat format)
{
  static constexpr std::array<std::tuple<GLenum, GLenum, GLenum>, static_cast<u32>(HostDisplayPixelFormat::Count)>
    mapping = {{
      {},                                                  // Unknown
      {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE},               // RGBA8
      {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE},               // BGRA8
      {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5},        // RGB565
      {GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV} // RGBA5551
    }};

  static constexpr std::array<std::tuple<GLenum, GLenum, GLenum>, static_cast<u32>(HostDisplayPixelFormat::Count)>
    mapping_gles2 = {{
      {},                                        // Unknown
      {GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE},      // RGBA8
      {},                                        // BGRA8
      {GL_RGB, GL_RGB, GL_UNSIGNED_SHORT_5_6_5}, // RGB565
      {}                                         // RGBA5551
    }};

  if (is_gles && !GLAD_GL_ES_VERSION_3_0)
    return mapping_gles2[static_cast<u32>(format)];
  else
    return mapping[static_cast<u32>(format)];
}

std::unique_ptr<HostDisplayTexture> OpenGLHostDisplay::CreateTexture(u32 width, u32 height, u32 layers, u32 levels,
                                                                     u32 samples, HostDisplayPixelFormat format,
                                                                     const void* data, u32 data_stride,
                                                                     bool dynamic /* = false */)
{
  if (layers != 1 || levels != 1)
    return {};

  const auto [gl_internal_format, gl_format, gl_type] = GetPixelFormatMapping(m_gl_context->IsGLES(), format);

  // TODO: Set pack width
  Assert(!data || data_stride == (width * sizeof(u32)));

  GL::Texture tex;
  if (!tex.Create(width, height, samples, gl_internal_format, gl_format, gl_type, data, data_stride))
    return {};

  return std::make_unique<OpenGLHostDisplayTexture>(std::move(tex), format);
}

void OpenGLHostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                      const void* texture_data, u32 texture_data_stride)
{
  OpenGLHostDisplayTexture* tex = static_cast<OpenGLHostDisplayTexture*>(texture);
  const auto [gl_internal_format, gl_format, gl_type] =
    GetPixelFormatMapping(m_gl_context->IsGLES(), texture->GetFormat());

  GLint alignment;
  if (texture_data_stride & 1)
    alignment = 1;
  else if (texture_data_stride & 2)
    alignment = 2;
  else
    alignment = 4;

  GLint old_texture_binding = 0, old_alignment = 0, old_row_length = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture_binding);
  glBindTexture(GL_TEXTURE_2D, tex->GetGLID());

  glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_alignment);
  glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);

  if (!m_use_gles2_draw_path)
  {
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &old_row_length);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, texture_data_stride / GetDisplayPixelFormatSize(texture->GetFormat()));
  }

  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, gl_format, gl_type, texture_data);

  if (!m_use_gles2_draw_path)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, old_row_length);

  glPixelStorei(GL_UNPACK_ALIGNMENT, old_alignment);
  glBindTexture(GL_TEXTURE_2D, old_texture_binding);
}

bool OpenGLHostDisplay::DownloadTexture(const void* texture_handle, HostDisplayPixelFormat texture_format, u32 x, u32 y,
                                        u32 width, u32 height, void* out_data, u32 out_data_stride)
{
  GLint alignment;
  if (out_data_stride & 1)
    alignment = 1;
  else if (out_data_stride & 2)
    alignment = 2;
  else
    alignment = 4;

  GLint old_alignment = 0, old_row_length = 0;
  glGetIntegerv(GL_PACK_ALIGNMENT, &old_alignment);
  glPixelStorei(GL_PACK_ALIGNMENT, alignment);
  if (!m_use_gles2_draw_path)
  {
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_row_length);
    glPixelStorei(GL_PACK_ROW_LENGTH, out_data_stride / GetDisplayPixelFormatSize(texture_format));
  }

  const GLuint texture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture_handle));
  const auto [gl_internal_format, gl_format, gl_type] = GetPixelFormatMapping(m_gl_context->IsGLES(), texture_format);

  GL::Texture::GetTextureSubImage(texture, 0, x, y, 0, width, height, 1, gl_format, gl_type, height * out_data_stride,
                                  out_data);

  glPixelStorei(GL_PACK_ALIGNMENT, old_alignment);
  if (!m_use_gles2_draw_path)
    glPixelStorei(GL_PACK_ROW_LENGTH, old_row_length);
  return true;
}

void OpenGLHostDisplay::BindDisplayPixelsTexture()
{
  if (m_display_pixels_texture_id == 0)
  {
    glGenTextures(1, &m_display_pixels_texture_id);
    glBindTexture(GL_TEXTURE_2D, m_display_pixels_texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_display_linear_filtering ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_display_linear_filtering ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    m_display_texture_is_linear_filtered = m_display_linear_filtering;
  }
  else
  {
    glBindTexture(GL_TEXTURE_2D, m_display_pixels_texture_id);
  }
}

void OpenGLHostDisplay::UpdateDisplayPixelsTextureFilter()
{
  if (m_display_linear_filtering == m_display_texture_is_linear_filtered)
    return;

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_display_linear_filtering ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_display_linear_filtering ? GL_LINEAR : GL_NEAREST);
  m_display_texture_is_linear_filtered = m_display_linear_filtering;
}

bool OpenGLHostDisplay::SupportsDisplayPixelFormat(HostDisplayPixelFormat format) const
{
  const auto [gl_internal_format, gl_format, gl_type] = GetPixelFormatMapping(m_gl_context->IsGLES(), format);
  return (gl_internal_format != static_cast<GLenum>(0));
}

bool OpenGLHostDisplay::BeginSetDisplayPixels(HostDisplayPixelFormat format, u32 width, u32 height, void** out_buffer,
                                              u32* out_pitch)
{
  const u32 pixel_size = GetDisplayPixelFormatSize(format);
  const u32 stride = Common::AlignUpPow2(width * pixel_size, 4);
  const u32 size_required = stride * height * pixel_size;

  if (m_use_pbo_for_pixels)
  {
    const u32 buffer_size = Common::AlignUpPow2(size_required * 2, 4 * 1024 * 1024);
    if (!m_display_pixels_texture_pbo || m_display_pixels_texture_pbo->GetSize() < buffer_size)
    {
      m_display_pixels_texture_pbo.reset();
      m_display_pixels_texture_pbo = GL::StreamBuffer::Create(GL_PIXEL_UNPACK_BUFFER, buffer_size);
      if (!m_display_pixels_texture_pbo)
        return false;
    }

    const auto map = m_display_pixels_texture_pbo->Map(GetDisplayPixelFormatSize(format), size_required);
    m_display_texture_format = format;
    m_display_pixels_texture_pbo_map_offset = map.buffer_offset;
    m_display_pixels_texture_pbo_map_size = size_required;
    *out_buffer = map.pointer;
    *out_pitch = stride;
  }
  else
  {
    if (m_gles_pixels_repack_buffer.size() < size_required)
      m_gles_pixels_repack_buffer.resize(size_required);

    *out_buffer = m_gles_pixels_repack_buffer.data();
    *out_pitch = stride;
  }

  BindDisplayPixelsTexture();
  SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_display_pixels_texture_id)), format, width, height,
                    0, 0, width, height);
  return true;
}

void OpenGLHostDisplay::EndSetDisplayPixels()
{
  const u32 width = static_cast<u32>(m_display_texture_view_width);
  const u32 height = static_cast<u32>(m_display_texture_view_height);

  const auto [gl_internal_format, gl_format, gl_type] =
    GetPixelFormatMapping(m_gl_context->IsGLES(), m_display_texture_format);

  glBindTexture(GL_TEXTURE_2D, m_display_pixels_texture_id);
  if (m_use_pbo_for_pixels)
  {
    m_display_pixels_texture_pbo->Unmap(m_display_pixels_texture_pbo_map_size);
    m_display_pixels_texture_pbo->Bind();
    glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type,
                 reinterpret_cast<void*>(static_cast<uintptr_t>(m_display_pixels_texture_pbo_map_offset)));
    m_display_pixels_texture_pbo->Unbind();

    m_display_pixels_texture_pbo_map_offset = 0;
    m_display_pixels_texture_pbo_map_size = 0;
  }
  else
  {
    // glTexImage2D should be quicker on Mali...
    glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type,
                 m_gles_pixels_repack_buffer.data());
  }

  glBindTexture(GL_TEXTURE_2D, 0);
}

bool OpenGLHostDisplay::SetDisplayPixels(HostDisplayPixelFormat format, u32 width, u32 height, const void* buffer,
                                         u32 pitch)
{
  BindDisplayPixelsTexture();

  const auto [gl_internal_format, gl_format, gl_type] = GetPixelFormatMapping(m_gl_context->IsGLES(), format);
  const u32 pixel_size = GetDisplayPixelFormatSize(format);
  const bool is_packed_tightly = (pitch == (pixel_size * width));

  // If we have GLES3, we can set row_length.
  if (!m_use_gles2_draw_path || is_packed_tightly)
  {
    if (!is_packed_tightly)
      glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / pixel_size);

    glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type, buffer);

    if (!is_packed_tightly)
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  }
  else
  {
    // Otherwise, we need to repack the image.
    const u32 packed_pitch = width * pixel_size;
    const u32 repack_size = packed_pitch * height;
    if (m_gles_pixels_repack_buffer.size() < repack_size)
      m_gles_pixels_repack_buffer.resize(repack_size);
    StringUtil::StrideMemCpy(m_gles_pixels_repack_buffer.data(), packed_pitch, buffer, pitch, packed_pitch, height);
    glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type,
                 m_gles_pixels_repack_buffer.data());
  }

  glBindTexture(GL_TEXTURE_2D, 0);

  SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_display_pixels_texture_id)), format, width, height,
                    0, 0, width, height);
  return true;
}

void OpenGLHostDisplay::SetVSync(bool enabled)
{
  if (m_gl_context->GetWindowInfo().type == WindowInfo::Type::Surfaceless)
    return;

  // Window framebuffer has to be bound to call SetSwapInterval.
  GLint current_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  m_gl_context->SetSwapInterval(enabled ? 1 : 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
}

const char* OpenGLHostDisplay::GetGLSLVersionString() const
{
  if (GetRenderAPI() == RenderAPI::OpenGLES)
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
  if (GetRenderAPI() == RenderAPI::OpenGLES)
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
      Log_ErrorPrint(message);
      break;
    case GL_DEBUG_SEVERITY_MEDIUM_KHR:
      Log_WarningPrint(message);
      break;
    case GL_DEBUG_SEVERITY_LOW_KHR:
      Log_InfoPrint(message);
      break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
      // Log_DebugPrint(message);
      break;
  }
}

bool OpenGLHostDisplay::HasRenderDevice() const
{
  return static_cast<bool>(m_gl_context);
}

bool OpenGLHostDisplay::HasRenderSurface() const
{
  return m_window_info.type != WindowInfo::Type::Surfaceless;
}

bool OpenGLHostDisplay::CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device,
                                           bool threaded_presentation)
{
  m_gl_context = GL::Context::Create(wi);
  if (!m_gl_context)
  {
    Log_ErrorPrintf("Failed to create any GL context");
    m_gl_context.reset();
    return false;
  }

  m_window_info = m_gl_context->GetWindowInfo();
  return true;
}

bool OpenGLHostDisplay::InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device,
                                               bool threaded_presentation)
{
  m_use_gles2_draw_path = (GetRenderAPI() == RenderAPI::OpenGLES && !GLAD_GL_ES_VERSION_3_0);
  if (!m_use_gles2_draw_path)
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, reinterpret_cast<GLint*>(&m_uniform_buffer_alignment));

  // Doubt GLES2 drivers will support PBOs efficiently.
  m_use_pbo_for_pixels = !m_use_gles2_draw_path;
  if (GetRenderAPI() == RenderAPI::OpenGLES)
  {
    // Adreno seems to corrupt textures through PBOs...
    const char* gl_vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    if (std::strstr(gl_vendor, "Qualcomm") || std::strstr(gl_vendor, "Broadcom"))
      m_use_pbo_for_pixels = false;
  }

  Log_VerbosePrintf("Using GLES2 draw path: %s", m_use_gles2_draw_path ? "yes" : "no");
  Log_VerbosePrintf("Using PBO for streaming: %s", m_use_pbo_for_pixels ? "yes" : "no");

  if (debug_device && GLAD_GL_KHR_debug)
  {
    if (GetRenderAPI() == RenderAPI::OpenGLES)
      glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    else
      glDebugMessageCallback(GLDebugCallback, nullptr);

    glEnable(GL_DEBUG_OUTPUT);
    // glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  }

  if (!CreateResources())
    return false;

  // Start with vsync on.
  SetVSync(true);

  return true;
}

bool OpenGLHostDisplay::MakeRenderContextCurrent()
{
  if (!m_gl_context->MakeCurrent())
  {
    Log_ErrorPrintf("Failed to make GL context current");
    return false;
  }

  return true;
}

bool OpenGLHostDisplay::DoneRenderContextCurrent()
{
  return m_gl_context->DoneCurrent();
}

void OpenGLHostDisplay::DestroyRenderDevice()
{
  if (!m_gl_context)
    return;

  DestroyResources();

  m_gl_context->DoneCurrent();
  m_gl_context.reset();
}

bool OpenGLHostDisplay::ChangeRenderWindow(const WindowInfo& new_wi)
{
  Assert(m_gl_context);

  if (!m_gl_context->ChangeSurface(new_wi))
  {
    Log_ErrorPrintf("Failed to change surface");
    return false;
  }

  m_window_info = m_gl_context->GetWindowInfo();
  return true;
}

void OpenGLHostDisplay::ResizeRenderWindow(s32 new_window_width, s32 new_window_height)
{
  if (!m_gl_context)
    return;

  m_gl_context->ResizeSurface(static_cast<u32>(new_window_width), static_cast<u32>(new_window_height));
  m_window_info = m_gl_context->GetWindowInfo();
}

bool OpenGLHostDisplay::SupportsFullscreen() const
{
  return false;
}

bool OpenGLHostDisplay::IsFullscreen()
{
  return false;
}

bool OpenGLHostDisplay::SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate)
{
  return false;
}

HostDisplay::AdapterAndModeList OpenGLHostDisplay::GetAdapterAndModeList()
{
  AdapterAndModeList aml;

  if (m_gl_context)
  {
    for (const GL::Context::FullscreenModeInfo& fmi : m_gl_context->EnumerateFullscreenModes())
    {
      aml.fullscreen_modes.push_back(
        CommonHostInterface::GetFullscreenModeString(fmi.width, fmi.height, fmi.refresh_rate));
    }
  }

  return aml;
}

void OpenGLHostDisplay::DestroyRenderSurface()
{
  if (!m_gl_context)
    return;

  m_window_info = {};
  if (!m_gl_context->ChangeSurface(m_window_info))
    Log_ErrorPrintf("Failed to switch to surfaceless");
}

bool OpenGLHostDisplay::CreateImGuiContext()
{
  return ImGui_ImplOpenGL3_Init(GetGLSLVersionString());
}

void OpenGLHostDisplay::DestroyImGuiContext()
{
  ImGui_ImplOpenGL3_Shutdown();
}

bool OpenGLHostDisplay::UpdateImGuiFontTexture()
{
  ImGui_ImplOpenGL3_DestroyFontsTexture();
  return ImGui_ImplOpenGL3_CreateFontsTexture();
}

bool OpenGLHostDisplay::CreateResources()
{
  if (!m_use_gles2_draw_path)
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
  o_col0 = vec4(texture(samp0, v_tex0).rgb, 1.0);
}
)";

    static constexpr char cursor_fragment_shader[] = R"(
uniform sampler2D samp0;

in vec2 v_tex0;
out vec4 o_col0;

void main()
{
  o_col0 = texture(samp0, v_tex0);
}
)";

    if (!m_display_program.Compile(GetGLSLVersionHeader() + fullscreen_quad_vertex_shader, {},
                                   GetGLSLVersionHeader() + display_fragment_shader) ||
        !m_cursor_program.Compile(GetGLSLVersionHeader() + fullscreen_quad_vertex_shader, {},
                                  GetGLSLVersionHeader() + cursor_fragment_shader))
    {
      Log_ErrorPrintf("Failed to compile display shaders");
      return false;
    }

    if (GetRenderAPI() != RenderAPI::OpenGLES)
    {
      m_display_program.BindFragData(0, "o_col0");
      m_cursor_program.BindFragData(0, "o_col0");
    }

    if (!m_display_program.Link() || !m_cursor_program.Link())
    {
      Log_ErrorPrintf("Failed to link display programs");
      return false;
    }

    m_display_program.Bind();
    m_display_program.RegisterUniform("u_src_rect");
    m_display_program.RegisterUniform("samp0");
    m_display_program.Uniform1i(1, 0);
    m_cursor_program.Bind();
    m_cursor_program.RegisterUniform("u_src_rect");
    m_cursor_program.RegisterUniform("samp0");
    m_cursor_program.Uniform1i(1, 0);

    glGenVertexArrays(1, &m_display_vao);

    // samplers
    glGenSamplers(1, &m_display_nearest_sampler);
    glSamplerParameteri(m_display_nearest_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(m_display_nearest_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenSamplers(1, &m_display_linear_sampler);
    glSamplerParameteri(m_display_linear_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(m_display_linear_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  }
  else
  {
    static constexpr char fullscreen_quad_vertex_shader[] = R"(
#version 100

attribute highp vec2 a_pos;
attribute highp vec2 a_tex0;
varying highp vec2 v_tex0;

void main()
{
  gl_Position = vec4(a_pos, 0.0, 1.0);
  v_tex0 = a_tex0;
}
)";

    static constexpr char display_fragment_shader[] = R"(
#version 100

uniform highp sampler2D samp0;

varying highp vec2 v_tex0;

void main()
{
  gl_FragColor = vec4(texture2D(samp0, v_tex0).rgb, 1.0);
}
)";

    static constexpr char cursor_fragment_shader[] = R"(
#version 100

uniform highp sampler2D samp0;

varying highp vec2 v_tex0;

void main()
{
  gl_FragColor = texture2D(samp0, v_tex0);
}
)";

    if (!m_display_program.Compile(fullscreen_quad_vertex_shader, {}, display_fragment_shader) ||
        !m_cursor_program.Compile(fullscreen_quad_vertex_shader, {}, cursor_fragment_shader))
    {
      Log_ErrorPrintf("Failed to compile display shaders");
      return false;
    }

    m_display_program.BindAttribute(0, "a_pos");
    m_display_program.BindAttribute(1, "a_tex0");
    m_cursor_program.BindAttribute(0, "a_pos");
    m_cursor_program.BindAttribute(1, "a_tex0");

    if (!m_display_program.Link() || !m_cursor_program.Link())
    {
      Log_ErrorPrintf("Failed to link display programs");
      return false;
    }

    m_display_program.Bind();
    m_display_program.RegisterUniform("samp0");
    m_display_program.Uniform1i(0, 0);
    m_cursor_program.Bind();
    m_cursor_program.RegisterUniform("samp0");
    m_cursor_program.Uniform1i(0, 0);
  }

  return true;
}

void OpenGLHostDisplay::DestroyResources()
{
  m_post_processing_chain.ClearStages();
  m_post_processing_input_texture.Destroy();
  m_post_processing_ubo.reset();
  m_post_processing_stages.clear();

  if (m_display_pixels_texture_id != 0)
  {
    glDeleteTextures(1, &m_display_pixels_texture_id);
    m_display_pixels_texture_id = 0;
  }

  if (m_display_vao != 0)
  {
    glDeleteVertexArrays(1, &m_display_vao);
    m_display_vao = 0;
  }
  if (m_display_linear_sampler != 0)
  {
    glDeleteSamplers(1, &m_display_linear_sampler);
    m_display_linear_sampler = 0;
  }
  if (m_display_nearest_sampler != 0)
  {
    glDeleteSamplers(1, &m_display_nearest_sampler);
    m_display_nearest_sampler = 0;
  }

  m_cursor_program.Destroy();
  m_display_program.Destroy();
}

bool OpenGLHostDisplay::Render()
{
  if (ShouldSkipDisplayingFrame())
  {
    if (ImGui::GetCurrentContext())
      ImGui::Render();

    return false;
  }

  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  RenderDisplay();

  if (ImGui::GetCurrentContext())
    RenderImGui();

  RenderSoftwareCursor();

  m_gl_context->SwapBuffers();
  return true;
}

bool OpenGLHostDisplay::RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                                         HostDisplayPixelFormat* out_format)
{
  GL::Texture texture;
  if (!texture.Create(width, height, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nullptr) || !texture.CreateFramebuffer())
    return false;

  glDisable(GL_SCISSOR_TEST);
  texture.BindFramebuffer(GL_FRAMEBUFFER);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (HasDisplayTexture())
  {
    const auto [left, top, draw_width, draw_height] = CalculateDrawRect(width, height, 0);

    if (!m_post_processing_chain.IsEmpty())
    {
      ApplyPostProcessingChain(texture.GetGLFramebufferID(), left, height - top - draw_height, draw_width, draw_height,
                               m_display_texture_handle, m_display_texture_width, m_display_texture_height,
                               m_display_texture_view_x, m_display_texture_view_y, m_display_texture_view_width,
                               m_display_texture_view_height, width, height);
    }
    else
    {
      RenderDisplay(left, height - top - draw_height, draw_width, draw_height, m_display_texture_handle,
                    m_display_texture_width, m_display_texture_height, m_display_texture_view_x,
                    m_display_texture_view_y, m_display_texture_view_width, m_display_texture_view_height,
                    m_display_linear_filtering);
    }
  }

  out_pixels->resize(width * height);
  *out_stride = sizeof(u32) * width;
  *out_format = HostDisplayPixelFormat::RGBA8;
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out_pixels->data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  return true;
}

void OpenGLHostDisplay::RenderImGui()
{
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  GL::Program::ResetLastProgram();
}

void OpenGLHostDisplay::RenderDisplay()
{
  if (!HasDisplayTexture())
    return;

  const auto [left, top, width, height] = CalculateDrawRect(GetWindowWidth(), GetWindowHeight(), m_display_top_margin);

  if (!m_post_processing_chain.IsEmpty())
  {
    ApplyPostProcessingChain(0, left, GetWindowHeight() - top - height, width, height, m_display_texture_handle,
                             m_display_texture_width, m_display_texture_height, m_display_texture_view_x,
                             m_display_texture_view_y, m_display_texture_view_width, m_display_texture_view_height,
                             GetWindowWidth(), GetWindowHeight());
    return;
  }

  RenderDisplay(left, GetWindowHeight() - top - height, width, height, m_display_texture_handle,
                m_display_texture_width, m_display_texture_height, m_display_texture_view_x, m_display_texture_view_y,
                m_display_texture_view_width, m_display_texture_view_height, m_display_linear_filtering);
}

static void DrawFullscreenQuadES2(s32 tex_view_x, s32 tex_view_y, s32 tex_view_width, s32 tex_view_height,
                                  s32 tex_width, s32 tex_height)
{
  const float tex_left = static_cast<float>(tex_view_x) / static_cast<float>(tex_width);
  const float tex_right = tex_left + static_cast<float>(tex_view_width) / static_cast<float>(tex_width);
  const float tex_top = static_cast<float>(tex_view_y) / static_cast<float>(tex_height);
  const float tex_bottom = tex_top + static_cast<float>(tex_view_height) / static_cast<float>(tex_height);
  const std::array<std::array<float, 4>, 4> vertices = {{
    {{-1.0f, -1.0f, tex_left, tex_bottom}}, // bottom-left
    {{1.0f, -1.0f, tex_right, tex_bottom}}, // bottom-right
    {{-1.0f, 1.0f, tex_left, tex_top}},     // top-left
    {{1.0f, 1.0f, tex_right, tex_top}},     // top-right
  }};

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(vertices[0]), &vertices[0][0]);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertices[0]), &vertices[0][2]);
  glEnableVertexAttribArray(1);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(1);
  glDisableVertexAttribArray(0);
}

void OpenGLHostDisplay::RenderDisplay(s32 left, s32 bottom, s32 width, s32 height, void* texture_handle,
                                      u32 texture_width, s32 texture_height, s32 texture_view_x, s32 texture_view_y,
                                      s32 texture_view_width, s32 texture_view_height, bool linear_filter)
{
  glViewport(left, bottom, width, height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture_handle)));
  m_display_program.Bind();

  if (!m_use_gles2_draw_path)
  {
    const float position_adjust = m_display_linear_filtering ? 0.5f : 0.0f;
    const float size_adjust = m_display_linear_filtering ? 1.0f : 0.0f;
    const float flip_adjust = (texture_view_height < 0) ? -1.0f : 1.0f;
    m_display_program.Uniform4f(
      0, (static_cast<float>(texture_view_x) + position_adjust) / static_cast<float>(texture_width),
      (static_cast<float>(texture_view_y) + (position_adjust * flip_adjust)) / static_cast<float>(texture_height),
      (static_cast<float>(texture_view_width) - size_adjust) / static_cast<float>(texture_width),
      (static_cast<float>(texture_view_height) - (size_adjust * flip_adjust)) / static_cast<float>(texture_height));
    glBindSampler(0, linear_filter ? m_display_linear_sampler : m_display_nearest_sampler);
    glBindVertexArray(m_display_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindSampler(0, 0);
  }
  else
  {
    if (static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture_handle)) == m_display_pixels_texture_id)
      UpdateDisplayPixelsTextureFilter();

    DrawFullscreenQuadES2(m_display_texture_view_x, m_display_texture_view_y, m_display_texture_view_width,
                          m_display_texture_view_height, m_display_texture_width, m_display_texture_height);
  }
}

void OpenGLHostDisplay::RenderSoftwareCursor()
{
  if (!HasSoftwareCursor())
    return;

  const auto [left, top, width, height] = CalculateSoftwareCursorDrawRect();
  RenderSoftwareCursor(left, GetWindowHeight() - top - height, width, height, m_cursor_texture.get());
}

void OpenGLHostDisplay::RenderSoftwareCursor(s32 left, s32 bottom, s32 width, s32 height,
                                             HostDisplayTexture* texture_handle)
{
  glViewport(left, bottom, width, height);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
  glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  m_cursor_program.Bind();
  glBindTexture(GL_TEXTURE_2D, static_cast<OpenGLHostDisplayTexture*>(texture_handle)->GetGLID());

  if (!m_use_gles2_draw_path)
  {
    m_cursor_program.Uniform4f(0, 0.0f, 0.0f, 1.0f, 1.0f);
    glBindSampler(0, m_display_linear_sampler);
    glBindVertexArray(m_display_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindSampler(0, 0);
  }
  else
  {
    const s32 tex_width = static_cast<s32>(static_cast<OpenGLHostDisplayTexture*>(texture_handle)->GetWidth());
    const s32 tex_height = static_cast<s32>(static_cast<OpenGLHostDisplayTexture*>(texture_handle)->GetHeight());
    DrawFullscreenQuadES2(0, 0, tex_width, tex_height, tex_width, tex_height);
  }
}

bool OpenGLHostDisplay::SetPostProcessingChain(const std::string_view& config)
{
  if (config.empty())
  {
    m_post_processing_input_texture.Destroy();
    m_post_processing_stages.clear();
    m_post_processing_chain.ClearStages();
    return true;
  }

  if (!m_post_processing_chain.CreateFromString(config))
    return false;

  m_post_processing_stages.clear();

  FrontendCommon::PostProcessingShaderGen shadergen(HostDisplay::RenderAPI::OpenGL, false);

  for (u32 i = 0; i < m_post_processing_chain.GetStageCount(); i++)
  {
    const PostProcessingShader& shader = m_post_processing_chain.GetShaderStage(i);
    const std::string vs = shadergen.GeneratePostProcessingVertexShader(shader);
    const std::string ps = shadergen.GeneratePostProcessingFragmentShader(shader);

    PostProcessingStage stage;
    stage.uniforms_size = shader.GetUniformsSize();
    if (!stage.program.Compile(vs, {}, ps))
    {
      Log_InfoPrintf("Failed to compile post-processing program, disabling.");
      m_post_processing_stages.clear();
      m_post_processing_chain.ClearStages();
      return false;
    }

    if (!shadergen.UseGLSLBindingLayout())
    {
      stage.program.BindUniformBlock("UBOBlock", 1);
      stage.program.Bind();
      stage.program.Uniform1i("samp0", 0);
    }

    if (!stage.program.Link())
    {
      Log_InfoPrintf("Failed to link post-processing program, disabling.");
      m_post_processing_stages.clear();
      m_post_processing_chain.ClearStages();
      return false;
    }

    m_post_processing_stages.push_back(std::move(stage));
  }

  if (!m_post_processing_ubo)
  {
    m_post_processing_ubo = GL::StreamBuffer::Create(GL_UNIFORM_BUFFER, 1 * 1024 * 1024);
    if (!m_post_processing_ubo)
    {
      Log_InfoPrintf("Failed to allocate uniform buffer for postprocessing");
      m_post_processing_stages.clear();
      m_post_processing_chain.ClearStages();
      return false;
    }

    m_post_processing_ubo->Unbind();
  }

  return true;
}

bool OpenGLHostDisplay::CheckPostProcessingRenderTargets(u32 target_width, u32 target_height)
{
  DebugAssert(!m_post_processing_stages.empty());

  if (m_post_processing_input_texture.GetWidth() != target_width ||
      m_post_processing_input_texture.GetHeight() != target_height)
  {
    if (!m_post_processing_input_texture.Create(target_width, target_height, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE) ||
        !m_post_processing_input_texture.CreateFramebuffer())
    {
      return false;
    }
  }

  const u32 target_count = (static_cast<u32>(m_post_processing_stages.size()) - 1);
  for (u32 i = 0; i < target_count; i++)
  {
    PostProcessingStage& pps = m_post_processing_stages[i];
    if (pps.output_texture.GetWidth() != target_width || pps.output_texture.GetHeight() != target_height)
    {
      if (!pps.output_texture.Create(target_width, target_height, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE) ||
          !pps.output_texture.CreateFramebuffer())
      {
        return false;
      }
    }
  }

  return true;
}

void OpenGLHostDisplay::ApplyPostProcessingChain(GLuint final_target, s32 final_left, s32 final_top, s32 final_width,
                                                 s32 final_height, void* texture_handle, u32 texture_width,
                                                 s32 texture_height, s32 texture_view_x, s32 texture_view_y,
                                                 s32 texture_view_width, s32 texture_view_height, u32 target_width,
                                                 u32 target_height)
{
  if (!CheckPostProcessingRenderTargets(target_width, target_height))
  {
    RenderDisplay(final_left, target_height - final_top - final_height, final_width, final_height, texture_handle,
                  texture_width, texture_height, texture_view_x, texture_view_y, texture_view_width,
                  texture_view_height, m_display_linear_filtering);
    return;
  }

  // downsample/upsample - use same viewport for remainder
  m_post_processing_input_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  glClear(GL_COLOR_BUFFER_BIT);
  RenderDisplay(final_left, target_height - final_top - final_height, final_width, final_height, texture_handle,
                texture_width, texture_height, texture_view_x, texture_view_y, texture_view_width, texture_view_height,
                m_display_linear_filtering);

  texture_handle = reinterpret_cast<void*>(static_cast<uintptr_t>(m_post_processing_input_texture.GetGLId()));
  texture_width = m_post_processing_input_texture.GetWidth();
  texture_height = m_post_processing_input_texture.GetHeight();
  texture_view_x = final_left;
  texture_view_y = final_top;
  texture_view_width = final_width;
  texture_view_height = final_height;

  m_post_processing_ubo->Bind();

  const u32 final_stage = static_cast<u32>(m_post_processing_stages.size()) - 1u;
  for (u32 i = 0; i < static_cast<u32>(m_post_processing_stages.size()); i++)
  {
    PostProcessingStage& pps = m_post_processing_stages[i];
    if (i == final_stage)
    {
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, final_target);
    }
    else
    {
      pps.output_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
      glClear(GL_COLOR_BUFFER_BIT);
    }

    pps.program.Bind();
    glBindSampler(0, m_display_linear_sampler);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture_handle)));
    glBindSampler(0, m_display_nearest_sampler);

    const auto map_result = m_post_processing_ubo->Map(m_uniform_buffer_alignment, pps.uniforms_size);
    m_post_processing_chain.GetShaderStage(i).FillUniformBuffer(
      map_result.pointer, texture_width, texture_height, texture_view_x, texture_view_y, texture_view_width,
      texture_view_height, GetWindowWidth(), GetWindowHeight(), 0.0f);
    m_post_processing_ubo->Unmap(pps.uniforms_size);
    glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_post_processing_ubo->GetGLBufferId(), map_result.buffer_offset,
                      pps.uniforms_size);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (i != final_stage)
      texture_handle = reinterpret_cast<void*>(static_cast<uintptr_t>(pps.output_texture.GetGLId()));
  }

  glBindSampler(0, 0);
  m_post_processing_ubo->Unbind();
}

} // namespace FrontendCommon
