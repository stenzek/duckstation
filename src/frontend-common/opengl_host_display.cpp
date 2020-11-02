#include "opengl_host_display.h"
#include "common/assert.h"
#include "common/log.h"
#include <array>
#include <tuple>
#ifdef WITH_IMGUI
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#endif
#ifndef LIBRETRO
#include "postprocessing_shadergen.h"
#endif
Log_SetChannel(LibretroOpenGLHostDisplay);

namespace FrontendCommon {

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

std::unique_ptr<HostDisplayTexture> OpenGLHostDisplay::CreateTexture(u32 width, u32 height, const void* initial_data,
                                                                     u32 initial_data_stride, bool dynamic)
{
  return OpenGLHostDisplayTexture::Create(width, height, initial_data, initial_data_stride);
}

void OpenGLHostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                      const void* texture_data, u32 texture_data_stride)
{
  OpenGLHostDisplayTexture* tex = static_cast<OpenGLHostDisplayTexture*>(texture);
  Assert((texture_data_stride % sizeof(u32)) == 0);

  GLint old_texture_binding = 0, old_alignment = 0, old_row_length = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture_binding);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_alignment);
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &old_row_length);

  glBindTexture(GL_TEXTURE_2D, tex->GetGLID());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, texture_data_stride / sizeof(u32));

  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, texture_data);

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

bool OpenGLHostDisplay::HasRenderDevice() const
{
  return static_cast<bool>(m_gl_context);
}

bool OpenGLHostDisplay::HasRenderSurface() const
{
  return m_window_info.type != WindowInfo::Type::Surfaceless;
}

bool OpenGLHostDisplay::CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device)
{
  m_gl_context = GL::Context::Create(wi);
  if (!m_gl_context)
  {
    Log_ErrorPrintf("Failed to create any GL context");
    return false;
  }

  m_window_info = wi;
  m_window_info.surface_width = m_gl_context->GetSurfaceWidth();
  m_window_info.surface_height = m_gl_context->GetSurfaceHeight();
  return true;
}

bool OpenGLHostDisplay::InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device)
{
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, reinterpret_cast<GLint*>(&m_uniform_buffer_alignment));

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

#ifdef WITH_IMGUI
  if (ImGui::GetCurrentContext() && !CreateImGuiContext())
    return false;
#endif

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

#ifdef WITH_IMGUI
  if (ImGui::GetCurrentContext())
    DestroyImGuiContext();
#endif

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

  m_window_info = new_wi;
  m_window_info.surface_width = m_gl_context->GetSurfaceWidth();
  m_window_info.surface_height = m_gl_context->GetSurfaceHeight();

#ifdef WITH_IMGUI
  if (ImGui::GetCurrentContext())
  {
    ImGui::GetIO().DisplaySize.x = static_cast<float>(m_window_info.surface_width);
    ImGui::GetIO().DisplaySize.y = static_cast<float>(m_window_info.surface_height);
  }
#endif

  return true;
}

void OpenGLHostDisplay::ResizeRenderWindow(s32 new_window_width, s32 new_window_height)
{
  if (!m_gl_context)
    return;

  m_gl_context->ResizeSurface(static_cast<u32>(new_window_width), static_cast<u32>(new_window_height));
  m_window_info.surface_width = m_gl_context->GetSurfaceWidth();
  m_window_info.surface_height = m_gl_context->GetSurfaceHeight();

#ifdef WITH_IMGUI
  if (ImGui::GetCurrentContext())
  {
    ImGui::GetIO().DisplaySize.x = static_cast<float>(m_window_info.surface_width);
    ImGui::GetIO().DisplaySize.y = static_cast<float>(m_window_info.surface_height);
  }
#endif
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
#ifdef WITH_IMGUI
  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_window_info.surface_width);
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_window_info.surface_height);

  if (!ImGui_ImplOpenGL3_Init(GetGLSLVersionString()))
    return false;

  ImGui_ImplOpenGL3_NewFrame();
#endif
  return true;
}

void OpenGLHostDisplay::DestroyImGuiContext()
{
#ifdef WITH_IMGUI
  ImGui_ImplOpenGL3_Shutdown();
#endif
}

bool OpenGLHostDisplay::CreateResources()
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

  return true;
}

void OpenGLHostDisplay::DestroyResources()
{
#ifndef LIBRETRO
  m_post_processing_chain.ClearStages();
  m_post_processing_input_texture.Destroy();
  m_post_processing_ubo.reset();
  m_post_processing_stages.clear();
#endif

  if (m_display_vao != 0)
    glDeleteVertexArrays(1, &m_display_vao);
  if (m_display_linear_sampler != 0)
    glDeleteSamplers(1, &m_display_linear_sampler);
  if (m_display_nearest_sampler != 0)
    glDeleteSamplers(1, &m_display_nearest_sampler);

  m_cursor_program.Destroy();
  m_display_program.Destroy();
}

bool OpenGLHostDisplay::Render()
{
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  RenderDisplay();

#ifdef WITH_IMGUI
  if (ImGui::GetCurrentContext())
    RenderImGui();
#endif

  RenderSoftwareCursor();

  m_gl_context->SwapBuffers();

#ifdef WITH_IMGUI
  if (ImGui::GetCurrentContext())
    ImGui_ImplOpenGL3_NewFrame();
#endif

  return true;
}

void OpenGLHostDisplay::RenderImGui()
{
#ifdef WITH_IMGUI
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  GL::Program::ResetLastProgram();
#endif
}

void OpenGLHostDisplay::RenderDisplay()
{
  if (!HasDisplayTexture())
    return;

  const auto [left, top, width, height] = CalculateDrawRect(GetWindowWidth(), GetWindowHeight(), m_display_top_margin);

#ifndef LIBRETRO
  if (!m_post_processing_chain.IsEmpty())
  {
    ApplyPostProcessingChain(0, left, GetWindowHeight() - top - height, width, height, m_display_texture_handle,
                             m_display_texture_width, m_display_texture_height, m_display_texture_view_x,
                             m_display_texture_view_y, m_display_texture_view_width, m_display_texture_view_height);
    return;
  }
#endif

  RenderDisplay(left, GetWindowHeight() - top - height, width, height, m_display_texture_handle,
                m_display_texture_width, m_display_texture_height, m_display_texture_view_x, m_display_texture_view_y,
                m_display_texture_view_width, m_display_texture_view_height, m_display_linear_filtering);
}

void OpenGLHostDisplay::RenderDisplay(s32 left, s32 bottom, s32 width, s32 height, void* texture_handle,
                                      u32 texture_width, s32 texture_height, s32 texture_view_x, s32 texture_view_y,
                                      s32 texture_view_width, s32 texture_view_height, bool linear_filter)
{
  glViewport(left, bottom, width, height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  m_display_program.Bind();
  m_display_program.Uniform4f(0, static_cast<float>(texture_view_x) / static_cast<float>(texture_width),
                              static_cast<float>(texture_view_y) / static_cast<float>(texture_height),
                              (static_cast<float>(texture_view_width) - 0.5f) / static_cast<float>(texture_width),
                              (static_cast<float>(texture_view_height) + 0.5f) / static_cast<float>(texture_height));
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture_handle)));
  glBindSampler(0, linear_filter ? m_display_linear_sampler : m_display_nearest_sampler);
  glBindVertexArray(m_display_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(0, 0);
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
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  m_cursor_program.Bind();
  m_cursor_program.Uniform4f(0, 0.0f, 0.0f, 1.0f, 1.0f);
  glBindTexture(GL_TEXTURE_2D, static_cast<OpenGLHostDisplayTexture*>(texture_handle)->GetGLID());
  glBindSampler(0, m_display_linear_sampler);
  glBindVertexArray(m_display_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(0, 0);
}

#ifndef LIBRETRO

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
                                                 s32 texture_view_width, s32 texture_view_height)
{
  static constexpr std::array<float, 4> clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

  if (!CheckPostProcessingRenderTargets(GetWindowWidth(), GetWindowHeight()))
  {
    RenderDisplay(final_left, GetWindowHeight() - final_top - final_height, final_width, final_height, texture_handle,
                  texture_width, texture_height, texture_view_x, texture_view_y, texture_view_width,
                  texture_view_height, m_display_linear_filtering);
    return;
  }

  // downsample/upsample - use same viewport for remainder
  m_post_processing_input_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  glClear(GL_COLOR_BUFFER_BIT);
  RenderDisplay(final_left, GetWindowHeight() - final_top - final_height, final_width, final_height, texture_handle,
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

#else

bool OpenGLHostDisplay::SetPostProcessingChain(const std::string_view& config)
{
  return false;
}

#endif

} // namespace FrontendCommon
