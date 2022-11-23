#include "opengl_host_display.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common_host.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "postprocessing_shadergen.h"
#include <array>
#include <tuple>
Log_SetChannel(OpenGLHostDisplay);

enum : u32
{
  TEXTURE_STREAM_BUFFER_SIZE = 16 * 1024 * 1024,
};

OpenGLHostDisplay::OpenGLHostDisplay() = default;

OpenGLHostDisplay::~OpenGLHostDisplay()
{
  if (!m_gl_context)
    return;

  DestroyResources();

  m_gl_context->DoneCurrent();
  m_gl_context.reset();
}

RenderAPI OpenGLHostDisplay::GetRenderAPI() const
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

std::unique_ptr<GPUTexture> OpenGLHostDisplay::CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                             GPUTexture::Format format, const void* data,
                                                             u32 data_stride, bool dynamic /* = false */)
{
  std::unique_ptr<GL::Texture> tex(std::make_unique<GL::Texture>());
  if (!tex->Create(width, height, layers, levels, samples, format, data, data_stride))
    tex.reset();

  return tex;
}

bool OpenGLHostDisplay::BeginTextureUpdate(GPUTexture* texture, u32 width, u32 height, void** out_buffer,
                                           u32* out_pitch)
{
  const u32 pixel_size = texture->GetPixelSize();
  const u32 stride = Common::AlignUpPow2(width * pixel_size, 4);
  const u32 size_required = stride * height;
  GL::StreamBuffer* buffer = UsePBOForUploads() ? GetTextureStreamBuffer() : nullptr;

  if (buffer && size_required < buffer->GetSize())
  {
    auto map = buffer->Map(4096, size_required);
    m_texture_stream_buffer_offset = map.buffer_offset;
    *out_buffer = map.pointer;
    *out_pitch = stride;
  }
  else
  {
    std::vector<u8>& repack_buffer = GetTextureRepackBuffer();
    if (repack_buffer.size() < size_required)
      repack_buffer.resize(size_required);

    *out_buffer = repack_buffer.data();
    *out_pitch = stride;
  }

  return true;
}

void OpenGLHostDisplay::EndTextureUpdate(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height)
{
  const u32 pixel_size = texture->GetPixelSize();
  const u32 stride = Common::AlignUpPow2(width * pixel_size, 4);
  const u32 size_required = stride * height;
  GL::Texture* gl_texture = static_cast<GL::Texture*>(texture);
  GL::StreamBuffer* buffer = UsePBOForUploads() ? GetTextureStreamBuffer() : nullptr;

  const auto [gl_internal_format, gl_format, gl_type] = GL::Texture::GetPixelFormatMapping(gl_texture->GetFormat());
  const bool whole_texture = (!gl_texture->UseTextureStorage() && x == 0 && y == 0 && width == gl_texture->GetWidth() &&
                              height == gl_texture->GetHeight());

  gl_texture->Bind();
  if (buffer && size_required < buffer->GetSize())
  {
    buffer->Unmap(size_required);
    buffer->Bind();

    if (whole_texture)
    {
      glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type,
                   reinterpret_cast<void*>(static_cast<uintptr_t>(m_texture_stream_buffer_offset)));
    }
    else
    {
      glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, gl_format, gl_type,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(m_texture_stream_buffer_offset)));
    }

    buffer->Unbind();
  }
  else
  {
    std::vector<u8>& repack_buffer = GetTextureRepackBuffer();
    if (whole_texture)
      glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type, repack_buffer.data());
    else
      glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, gl_format, gl_type, repack_buffer.data());
  }
}

bool OpenGLHostDisplay::UpdateTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                                      u32 pitch)
{
  GL::Texture* gl_texture = static_cast<GL::Texture*>(texture);
  const auto [gl_internal_format, gl_format, gl_type] = GL::Texture::GetPixelFormatMapping(gl_texture->GetFormat());
  const u32 pixel_size = gl_texture->GetPixelSize();
  const bool is_packed_tightly = (pitch == (pixel_size * width));

  const bool whole_texture = (!gl_texture->UseTextureStorage() && x == 0 && y == 0 && width == gl_texture->GetWidth() &&
                              height == gl_texture->GetHeight());
  gl_texture->Bind();

  // If we have GLES3, we can set row_length.
  if (UseGLES3DrawPath() || is_packed_tightly)
  {
    if (!is_packed_tightly)
      glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / pixel_size);

    if (whole_texture)
      glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type, data);
    else
      glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, gl_format, gl_type, data);

    if (!is_packed_tightly)
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  }
  else
  {
    // Otherwise, we need to repack the image.
    std::vector<u8>& repack_buffer = GetTextureRepackBuffer();
    const u32 packed_pitch = width * pixel_size;
    const u32 repack_size = packed_pitch * height;
    if (repack_buffer.size() < repack_size)
      repack_buffer.resize(repack_size);

    StringUtil::StrideMemCpy(repack_buffer.data(), packed_pitch, data, pitch, packed_pitch, height);

    if (whole_texture)
      glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type, repack_buffer.data());
    else
      glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, gl_format, gl_type, repack_buffer.data());
  }

  return true;
}

bool OpenGLHostDisplay::DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                        u32 out_data_stride)
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
    glPixelStorei(GL_PACK_ROW_LENGTH, out_data_stride / texture->GetPixelSize());
  }

  const auto [gl_internal_format, gl_format, gl_type] = GL::Texture::GetPixelFormatMapping(texture->GetFormat());

  GL::Texture::GetTextureSubImage(static_cast<const GL::Texture*>(texture)->GetGLId(), 0, x, y, 0, width, height, 1,
                                  gl_format, gl_type, height * out_data_stride, out_data);

  glPixelStorei(GL_PACK_ALIGNMENT, old_alignment);
  if (!m_use_gles2_draw_path)
    glPixelStorei(GL_PACK_ROW_LENGTH, old_row_length);
  return true;
}

bool OpenGLHostDisplay::SupportsTextureFormat(GPUTexture::Format format) const
{
  const auto [gl_internal_format, gl_format, gl_type] = GL::Texture::GetPixelFormatMapping(format);
  return (gl_internal_format != static_cast<GLenum>(0));
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

bool OpenGLHostDisplay::CreateRenderDevice(const WindowInfo& wi)
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

bool OpenGLHostDisplay::InitializeRenderDevice()
{
  m_use_gles2_draw_path = (GetRenderAPI() == RenderAPI::OpenGLES && !GLAD_GL_ES_VERSION_3_0);
  if (!m_use_gles2_draw_path)
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, reinterpret_cast<GLint*>(&m_uniform_buffer_alignment));

  // Doubt GLES2 drivers will support PBOs efficiently.
  m_use_pbo_for_pixels = !m_use_gles2_draw_path;
  if (GetRenderAPI() == RenderAPI::OpenGLES)
  {
    // Adreno seems to corrupt textures through PBOs... and Mali is slow.
    const char* gl_vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    if (std::strstr(gl_vendor, "Qualcomm") || std::strstr(gl_vendor, "ARM") || std::strstr(gl_vendor, "Broadcom"))
      m_use_pbo_for_pixels = false;
  }

  Log_VerbosePrintf("Using GLES2 draw path: %s", m_use_gles2_draw_path ? "yes" : "no");
  Log_VerbosePrintf("Using PBO for streaming: %s", m_use_pbo_for_pixels ? "yes" : "no");

  if (g_settings.gpu_use_debug_device && GLAD_GL_KHR_debug)
  {
    if (GetRenderAPI() == RenderAPI::OpenGLES)
      glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    else
      glDebugMessageCallback(GLDebugCallback, nullptr);

    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
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
      aml.fullscreen_modes.push_back(GetFullscreenModeString(fmi.width, fmi.height, fmi.refresh_rate));
    }
  }

  return aml;
}

void OpenGLHostDisplay::DestroyRenderSurface()
{
  if (!m_gl_context)
    return;

  m_window_info.SetSurfaceless();
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
    glGenSamplers(1, &m_display_border_sampler);
    glSamplerParameteri(m_display_border_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(m_display_border_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // If we don't have border clamp.. too bad, just hope for the best.
    if (!m_gl_context->IsGLES() || GLAD_GL_ES_VERSION_3_2 || GLAD_GL_NV_texture_border_clamp ||
        GLAD_GL_EXT_texture_border_clamp || GLAD_GL_OES_texture_border_clamp)
    {
      static constexpr const float border_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};

      glSamplerParameteri(m_display_border_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
      glSamplerParameteri(m_display_border_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
      glTexParameterfv(m_display_border_sampler, GL_TEXTURE_BORDER_COLOR, border_color);
    }
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

  if (m_display_vao != 0)
  {
    glDeleteVertexArrays(1, &m_display_vao);
    m_display_vao = 0;
  }
  if (m_display_border_sampler != 0)
  {
    glDeleteSamplers(1, &m_display_border_sampler);
    m_display_border_sampler = 0;
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

bool OpenGLHostDisplay::Render(bool skip_present)
{
  if (skip_present || m_window_info.type == WindowInfo::Type::Surfaceless)
  {
    if (ImGui::GetCurrentContext())
      ImGui::Render();

    return false;
  }

  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

  RenderDisplay();

  if (ImGui::GetCurrentContext())
    RenderImGui();

  RenderSoftwareCursor();

  if (m_gpu_timing_enabled)
    PopTimestampQuery();

  m_gl_context->SwapBuffers();

  if (m_gpu_timing_enabled)
    KickTimestampQuery();

  return true;
}

bool OpenGLHostDisplay::RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                                         GPUTexture::Format* out_format)
{
  GL::Texture texture;
  if (!texture.Create(width, height, 1, 1, 1, GPUTexture::Format::RGBA8, nullptr, 0) || !texture.CreateFramebuffer())
  {
    return false;
  }

  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

  const auto [left, top, draw_width, draw_height] = CalculateDrawRect(width, height);

  if (HasDisplayTexture() && !m_post_processing_chain.IsEmpty())
  {
    ApplyPostProcessingChain(texture.GetGLFramebufferID(), left, height - top - draw_height, draw_width, draw_height,
                             static_cast<GL::Texture*>(m_display_texture), m_display_texture_view_x,
                             m_display_texture_view_y, m_display_texture_view_width, m_display_texture_view_height,
                             width, height);
  }
  else
  {
    texture.BindFramebuffer(GL_FRAMEBUFFER);
    glClear(GL_COLOR_BUFFER_BIT);

    if (HasDisplayTexture())
    {
      RenderDisplay(left, height - top - draw_height, draw_width, draw_height,
                    static_cast<GL::Texture*>(m_display_texture), m_display_texture_view_x, m_display_texture_view_y,
                    m_display_texture_view_width, m_display_texture_view_height, IsUsingLinearFiltering());
    }
  }

  out_pixels->resize(width * height);
  *out_stride = sizeof(u32) * width;
  *out_format = GPUTexture::Format::RGBA8;
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
  const auto [left, top, width, height] = CalculateDrawRect(GetWindowWidth(), GetWindowHeight());

  if (HasDisplayTexture() && !m_post_processing_chain.IsEmpty())
  {
    ApplyPostProcessingChain(0, left, GetWindowHeight() - top - height, width, height,
                             static_cast<GL::Texture*>(m_display_texture), m_display_texture_view_x,
                             m_display_texture_view_y, m_display_texture_view_width, m_display_texture_view_height,
                             GetWindowWidth(), GetWindowHeight());
    return;
  }

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  if (!HasDisplayTexture())
    return;

  RenderDisplay(left, GetWindowHeight() - top - height, width, height, static_cast<GL::Texture*>(m_display_texture),
                m_display_texture_view_x, m_display_texture_view_y, m_display_texture_view_width,
                m_display_texture_view_height, IsUsingLinearFiltering());
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

void OpenGLHostDisplay::RenderDisplay(s32 left, s32 bottom, s32 width, s32 height, GL::Texture* texture,
                                      s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                                      s32 texture_view_height, bool linear_filter)
{
  glViewport(left, bottom, width, height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  m_display_program.Bind();
  texture->Bind();

  const bool linear = IsUsingLinearFiltering();

  if (!m_use_gles2_draw_path)
  {
    const float position_adjust = linear ? 0.5f : 0.0f;
    const float size_adjust = linear ? 1.0f : 0.0f;
    const float flip_adjust = (texture_view_height < 0) ? -1.0f : 1.0f;
    m_display_program.Uniform4f(
      0, (static_cast<float>(texture_view_x) + position_adjust) / static_cast<float>(texture->GetWidth()),
      (static_cast<float>(texture_view_y) + (position_adjust * flip_adjust)) / static_cast<float>(texture->GetHeight()),
      (static_cast<float>(texture_view_width) - size_adjust) / static_cast<float>(texture->GetWidth()),
      (static_cast<float>(texture_view_height) - (size_adjust * flip_adjust)) /
        static_cast<float>(texture->GetHeight()));
    glBindSampler(0, linear_filter ? m_display_linear_sampler : m_display_nearest_sampler);
    glBindVertexArray(m_display_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindSampler(0, 0);
  }
  else
  {
    texture->SetLinearFilter(linear_filter);

    DrawFullscreenQuadES2(m_display_texture_view_x, m_display_texture_view_y, m_display_texture_view_width,
                          m_display_texture_view_height, texture->GetWidth(), texture->GetHeight());
  }
}

void OpenGLHostDisplay::RenderSoftwareCursor()
{
  if (!HasSoftwareCursor())
    return;

  const auto [left, top, width, height] = CalculateSoftwareCursorDrawRect();
  RenderSoftwareCursor(left, GetWindowHeight() - top - height, width, height, m_cursor_texture.get());
}

void OpenGLHostDisplay::RenderSoftwareCursor(s32 left, s32 bottom, s32 width, s32 height, GPUTexture* texture_handle)
{
  glViewport(left, bottom, width, height);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
  glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  m_cursor_program.Bind();
  static_cast<GL::Texture*>(texture_handle)->Bind();

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
    const s32 tex_width = static_cast<s32>(texture_handle->GetWidth());
    const s32 tex_height = static_cast<s32>(texture_handle->GetHeight());
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

  FrontendCommon::PostProcessingShaderGen shadergen(GetRenderAPI(), false);

  for (u32 i = 0; i < m_post_processing_chain.GetStageCount(); i++)
  {
    const FrontendCommon::PostProcessingShader& shader = m_post_processing_chain.GetShaderStage(i);
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

  m_post_processing_timer.Reset();
  return true;
}

bool OpenGLHostDisplay::CheckPostProcessingRenderTargets(u32 target_width, u32 target_height)
{
  DebugAssert(!m_post_processing_stages.empty());

  if (m_post_processing_input_texture.GetWidth() != target_width ||
      m_post_processing_input_texture.GetHeight() != target_height)
  {
    if (!m_post_processing_input_texture.Create(target_width, target_height, 1, 1, 1, GPUTexture::Format::RGBA8) ||
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
      if (!pps.output_texture.Create(target_width, target_height, 1, 1, 1, GPUTexture::Format::RGBA8) ||
          !pps.output_texture.CreateFramebuffer())
      {
        return false;
      }
    }
  }

  return true;
}

void OpenGLHostDisplay::ApplyPostProcessingChain(GLuint final_target, s32 final_left, s32 final_top, s32 final_width,
                                                 s32 final_height, GL::Texture* texture, s32 texture_view_x,
                                                 s32 texture_view_y, s32 texture_view_width, s32 texture_view_height,
                                                 u32 target_width, u32 target_height)
{
  if (!CheckPostProcessingRenderTargets(target_width, target_height))
  {
    RenderDisplay(final_left, target_height - final_top - final_height, final_width, final_height, texture,
                  texture_view_x, texture_view_y, texture_view_width, texture_view_height, IsUsingLinearFiltering());
    return;
  }

  // downsample/upsample - use same viewport for remainder
  m_post_processing_input_texture.BindFramebuffer(GL_FRAMEBUFFER);
  glClear(GL_COLOR_BUFFER_BIT);
  RenderDisplay(final_left, target_height - final_top - final_height, final_width, final_height, texture,
                texture_view_x, texture_view_y, texture_view_width, texture_view_height, IsUsingLinearFiltering());

  const s32 orig_texture_width = texture_view_width;
  const s32 orig_texture_height = texture_view_height;
  texture = &m_post_processing_input_texture;
  texture_view_x = final_left;
  texture_view_y = final_top;
  texture_view_width = final_width;
  texture_view_height = final_height;

  m_post_processing_ubo->Bind();

  const u32 final_stage = static_cast<u32>(m_post_processing_stages.size()) - 1u;
  for (u32 i = 0; i < static_cast<u32>(m_post_processing_stages.size()); i++)
  {
    PostProcessingStage& pps = m_post_processing_stages[i];
    glBindFramebuffer(GL_FRAMEBUFFER, (i == final_stage) ? final_target : pps.output_texture.GetGLFramebufferID());
    glClear(GL_COLOR_BUFFER_BIT);

    pps.program.Bind();

    static_cast<const GL::Texture*>(texture)->Bind();
    glBindSampler(0, m_display_border_sampler);

    const auto map_result = m_post_processing_ubo->Map(m_uniform_buffer_alignment, pps.uniforms_size);
    m_post_processing_chain.GetShaderStage(i).FillUniformBuffer(
      map_result.pointer, texture->GetWidth(), texture->GetHeight(), texture_view_x, texture_view_y, texture_view_width,
      texture_view_height, GetWindowWidth(), GetWindowHeight(), orig_texture_width, orig_texture_height,
      static_cast<float>(m_post_processing_timer.GetTimeSeconds()));
    m_post_processing_ubo->Unmap(pps.uniforms_size);
    glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_post_processing_ubo->GetGLBufferId(), map_result.buffer_offset,
                      pps.uniforms_size);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (i != final_stage)
      texture = &pps.output_texture;
  }

  glBindSampler(0, 0);
  m_post_processing_ubo->Unbind();
}

void OpenGLHostDisplay::CreateTimestampQueries()
{
  const bool gles = m_gl_context->IsGLES();
  const auto GenQueries = gles ? glGenQueriesEXT : glGenQueries;

  GenQueries(static_cast<u32>(m_timestamp_queries.size()), m_timestamp_queries.data());
  KickTimestampQuery();
}

void OpenGLHostDisplay::DestroyTimestampQueries()
{
  if (m_timestamp_queries[0] == 0)
    return;

  const bool gles = m_gl_context->IsGLES();
  const auto DeleteQueries = gles ? glDeleteQueriesEXT : glDeleteQueries;

  if (m_timestamp_query_started)
  {
    const auto EndQuery = gles ? glEndQueryEXT : glEndQuery;
    EndQuery(GL_TIME_ELAPSED);
  }

  DeleteQueries(static_cast<u32>(m_timestamp_queries.size()), m_timestamp_queries.data());
  m_timestamp_queries.fill(0);
  m_read_timestamp_query = 0;
  m_write_timestamp_query = 0;
  m_waiting_timestamp_queries = 0;
  m_timestamp_query_started = false;
}

void OpenGLHostDisplay::PopTimestampQuery()
{
  const bool gles = m_gl_context->IsGLES();

  if (gles)
  {
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    if (disjoint)
    {
      Log_VerbosePrintf("GPU timing disjoint, resetting.");
      if (m_timestamp_query_started)
        glEndQueryEXT(GL_TIME_ELAPSED);

      m_read_timestamp_query = 0;
      m_write_timestamp_query = 0;
      m_waiting_timestamp_queries = 0;
      m_timestamp_query_started = false;
    }
  }

  while (m_waiting_timestamp_queries > 0)
  {
    const auto GetQueryObjectiv = gles ? glGetQueryObjectivEXT : glGetQueryObjectiv;
    const auto GetQueryObjectui64v = gles ? glGetQueryObjectui64vEXT : glGetQueryObjectui64v;

    GLint available = 0;
    GetQueryObjectiv(m_timestamp_queries[m_read_timestamp_query], GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available)
      break;

    u64 result = 0;
    GetQueryObjectui64v(m_timestamp_queries[m_read_timestamp_query], GL_QUERY_RESULT, &result);
    m_accumulated_gpu_time += static_cast<float>(static_cast<double>(result) / 1000000.0);
    m_read_timestamp_query = (m_read_timestamp_query + 1) % NUM_TIMESTAMP_QUERIES;
    m_waiting_timestamp_queries--;
  }

  if (m_timestamp_query_started)
  {
    const auto EndQuery = gles ? glEndQueryEXT : glEndQuery;
    EndQuery(GL_TIME_ELAPSED);

    m_write_timestamp_query = (m_write_timestamp_query + 1) % NUM_TIMESTAMP_QUERIES;
    m_timestamp_query_started = false;
    m_waiting_timestamp_queries++;
  }
}

void OpenGLHostDisplay::KickTimestampQuery()
{
  if (m_timestamp_query_started || m_waiting_timestamp_queries == NUM_TIMESTAMP_QUERIES)
    return;

  const bool gles = m_gl_context->IsGLES();
  const auto BeginQuery = gles ? glBeginQueryEXT : glBeginQuery;

  BeginQuery(GL_TIME_ELAPSED, m_timestamp_queries[m_write_timestamp_query]);
  m_timestamp_query_started = true;
}

bool OpenGLHostDisplay::SetGPUTimingEnabled(bool enabled)
{
  if (m_gpu_timing_enabled == enabled)
    return true;

  if (enabled && m_gl_context->IsGLES() &&
      (!GLAD_GL_EXT_disjoint_timer_query || !glGetQueryObjectivEXT || !glGetQueryObjectui64vEXT))
  {
    return false;
  }

  m_gpu_timing_enabled = enabled;
  if (m_gpu_timing_enabled)
    CreateTimestampQueries();
  else
    DestroyTimestampQueries();

  return true;
}

float OpenGLHostDisplay::GetAndResetAccumulatedGPUTime()
{
  const float value = m_accumulated_gpu_time;
  m_accumulated_gpu_time = 0.0f;
  return value;
}

GL::StreamBuffer* OpenGLHostDisplay::GetTextureStreamBuffer()
{
  if (m_use_gles2_draw_path || m_texture_stream_buffer)
    return m_texture_stream_buffer.get();

  m_texture_stream_buffer = GL::StreamBuffer::Create(GL_PIXEL_UNPACK_BUFFER, TEXTURE_STREAM_BUFFER_SIZE);
  return m_texture_stream_buffer.get();
}
