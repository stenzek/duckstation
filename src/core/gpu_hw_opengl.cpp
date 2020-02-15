#include "gpu_hw_opengl.h"
#include "common/assert.h"
#include "common/log.h"
#include "gpu_hw_shadergen.h"
#include "host_display.h"
#include "system.h"
Log_SetChannel(GPU_HW_OpenGL);

GPU_HW_OpenGL::GPU_HW_OpenGL() : GPU_HW() {}

GPU_HW_OpenGL::~GPU_HW_OpenGL()
{
  // Destroy objects which don't have destructors to clean them up
  if (m_vao_id != 0)
    glDeleteVertexArrays(1, &m_vao_id);
  if (m_attributeless_vao_id != 0)
    glDeleteVertexArrays(1, &m_attributeless_vao_id);
  if (m_texture_buffer_r16ui_texture != 0)
    glDeleteTextures(1, &m_texture_buffer_r16ui_texture);

  if (m_host_display)
  {
    m_host_display->SetDisplayTexture(nullptr, 0, 0, 0, 0, 0, 0, 1.0f);
    ResetGraphicsAPIState();
  }
}

bool GPU_HW_OpenGL::Initialize(HostDisplay* host_display, System* system, DMA* dma,
                               InterruptController* interrupt_controller, Timers* timers)
{
  if (host_display->GetRenderAPI() != HostDisplay::RenderAPI::OpenGL &&
      host_display->GetRenderAPI() != HostDisplay::RenderAPI::OpenGLES)
  {
    Log_ErrorPrintf("Host render API type is incompatible");
    return false;
  }

  SetCapabilities(host_display);

  if (!GPU_HW::Initialize(host_display, system, dma, interrupt_controller, timers))
    return false;

  if (!CreateFramebuffer())
  {
    Log_ErrorPrintf("Failed to create framebuffer");
    return false;
  }

  if (!CreateVertexBuffer())
  {
    Log_ErrorPrintf("Failed to create vertex buffer");
    return false;
  }

  if (!CreateUniformBuffer())
  {
    Log_ErrorPrintf("Failed to create uniform buffer");
    return false;
  }

  if (!CreateTextureBuffer())
  {
    Log_ErrorPrintf("Failed to create texture buffer");
    return false;
  }

  if (!CompilePrograms())
  {
    Log_ErrorPrintf("Failed to compile programs");
    return false;
  }

  m_host_display->SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_vram_texture.GetGLId())), 0, 0,
                                    m_display_texture.GetWidth(), m_display_texture.GetHeight(),
                                    m_display_texture.GetWidth(), m_display_texture.GetHeight(), 1.0f);
  RestoreGraphicsAPIState();
  return true;
}

void GPU_HW_OpenGL::Reset()
{
  GPU_HW::Reset();

  ClearFramebuffer();
}

void GPU_HW_OpenGL::ResetGraphicsAPIState()
{
  GPU_HW::ResetGraphicsAPIState();

  glEnable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glLineWidth(1.0f);
  glBindVertexArray(0);
}

void GPU_HW_OpenGL::RestoreGraphicsAPIState()
{
  m_vram_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  glViewport(0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());

  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  glLineWidth(static_cast<float>(m_resolution_scale));
  glBindVertexArray(m_vao_id);

  SetScissorFromDrawingArea();
  m_batch_ubo_dirty = true;
}

void GPU_HW_OpenGL::UpdateSettings()
{
  GPU_HW::UpdateSettings();

  CreateFramebuffer();
  CompilePrograms();
  UpdateDisplay();
}

void GPU_HW_OpenGL::MapBatchVertexPointer(u32 required_vertices)
{
  Assert(!m_batch_start_vertex_ptr);

  const GL::StreamBuffer::MappingResult res =
    m_vertex_stream_buffer->Map(sizeof(BatchVertex), required_vertices * sizeof(BatchVertex));

  m_batch_start_vertex_ptr = static_cast<BatchVertex*>(res.pointer);
  m_batch_current_vertex_ptr = m_batch_start_vertex_ptr;
  m_batch_end_vertex_ptr = m_batch_start_vertex_ptr + res.space_aligned;
  m_batch_base_vertex = res.index_aligned;
}

std::tuple<s32, s32> GPU_HW_OpenGL::ConvertToFramebufferCoordinates(s32 x, s32 y)
{
  return std::make_tuple(x, static_cast<s32>(static_cast<s32>(VRAM_HEIGHT) - y));
}

void GPU_HW_OpenGL::SetCapabilities(HostDisplay* host_display)
{
  m_is_gles = (host_display->GetRenderAPI() == HostDisplay::RenderAPI::OpenGLES);

  Log_InfoPrintf("GL_VERSION: %s", glGetString(GL_VERSION));
  Log_InfoPrintf("GL_RENDERER: %s", glGetString(GL_VERSION));

  GLint max_texture_size = VRAM_WIDTH;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
  Log_InfoPrintf("Max texture size: %dx%d", max_texture_size, max_texture_size);
  const int max_texture_scale = max_texture_size / VRAM_WIDTH;

  std::array<int, 2> line_width_range = {{1, 1}};
  glGetIntegerv(GL_ALIASED_LINE_WIDTH_RANGE, line_width_range.data());
  Log_InfoPrintf("Max line width: %d", line_width_range[1]);

  m_max_resolution_scale = std::min(max_texture_scale, line_width_range[1]);
  Log_InfoPrintf("Maximum resolution scale is %u", m_max_resolution_scale);

  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, reinterpret_cast<GLint*>(&m_uniform_buffer_alignment));
  Log_InfoPrintf("Uniform buffer offset alignment: %u", m_uniform_buffer_alignment);

  if (!GLAD_GL_VERSION_4_3 && !GLAD_GL_EXT_copy_image)
    Log_WarningPrintf("GL_EXT_copy_image missing, this may affect performance.");

  m_supports_texture_buffer = (GLAD_GL_VERSION_3_1 || GLAD_GL_ES_VERSION_3_2);
  if (m_supports_texture_buffer)
  {
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, reinterpret_cast<GLint*>(&m_max_texture_buffer_size));
    Log_InfoPrintf("Max texel buffer size: %u", m_max_texture_buffer_size);
    if (m_max_texture_buffer_size < VRAM_WIDTH * VRAM_HEIGHT)
      Log_WarningPrintf("Maximum texture buffer size is less than VRAM size, VRAM writes may be slower.");
  }
  else
  {
    Log_WarningPrintf("Texture buffers are not supported, VRAM writes will be slower.");
  }

  int max_dual_source_draw_buffers = 0;
  glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &max_dual_source_draw_buffers);
  m_supports_dual_source_blend = (max_dual_source_draw_buffers > 0);
  if (!m_supports_dual_source_blend)
    Log_WarningPrintf("Dual-source blending is not supported, this may break some mask effects.");
}

bool GPU_HW_OpenGL::CreateFramebuffer()
{
  // save old vram texture/fbo, in case we're changing scale
  GL::Texture old_vram_texture = std::move(m_vram_texture);

  // scale vram size to internal resolution
  const u32 texture_width = VRAM_WIDTH * m_resolution_scale;
  const u32 texture_height = VRAM_HEIGHT * m_resolution_scale;

  if (!m_vram_texture.Create(texture_width, texture_height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false) ||
      !m_vram_texture.CreateFramebuffer())
  {
    return false;
  }

  // do we need to restore the framebuffer after a size change?
  if (old_vram_texture.IsValid())
  {
    const bool linear_filter = old_vram_texture.GetWidth() > m_vram_texture.GetWidth();
    Log_DevPrintf("Scaling %ux%u VRAM texture to %ux%u using %s filter", old_vram_texture.GetWidth(),
                  old_vram_texture.GetHeight(), m_vram_texture.GetWidth(), m_vram_texture.GetHeight(),
                  linear_filter ? "linear" : "nearest");
    glDisable(GL_SCISSOR_TEST);
    old_vram_texture.BindFramebuffer(GL_READ_FRAMEBUFFER);
    glBlitFramebuffer(0, 0, old_vram_texture.GetWidth(), old_vram_texture.GetHeight(), 0, 0, m_vram_texture.GetWidth(),
                      m_vram_texture.GetHeight(), GL_COLOR_BUFFER_BIT, linear_filter ? GL_LINEAR : GL_NEAREST);

    glEnable(GL_SCISSOR_TEST);
    old_vram_texture.Destroy();
  }

  if (!m_vram_read_texture.Create(texture_width, texture_height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false) ||
      !m_vram_read_texture.CreateFramebuffer() ||
      !m_vram_encoding_texture.Create(VRAM_WIDTH, VRAM_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false) ||
      !m_vram_encoding_texture.CreateFramebuffer() ||
      !m_display_texture.Create(texture_width, texture_height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false) ||
      !m_display_texture.CreateFramebuffer())
  {
    return false;
  }

  m_vram_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  SetFullVRAMDirtyRectangle();
  return true;
}

void GPU_HW_OpenGL::ClearFramebuffer()
{
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  SetFullVRAMDirtyRectangle();
}

bool GPU_HW_OpenGL::CreateVertexBuffer()
{
  m_vertex_stream_buffer = GL::StreamBuffer::Create(GL_ARRAY_BUFFER, VERTEX_BUFFER_SIZE);
  if (!m_vertex_stream_buffer)
    return false;

  m_vertex_stream_buffer->Bind();

  glGenVertexArrays(1, &m_vao_id);
  glBindVertexArray(m_vao_id);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(0, 2, GL_INT, sizeof(BatchVertex), reinterpret_cast<void*>(offsetof(BatchVertex, x)));
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, true, sizeof(BatchVertex),
                        reinterpret_cast<void*>(offsetof(BatchVertex, color)));
  glVertexAttribIPointer(2, 1, GL_INT, sizeof(BatchVertex), reinterpret_cast<void*>(offsetof(BatchVertex, texcoord)));
  glVertexAttribIPointer(3, 1, GL_INT, sizeof(BatchVertex), reinterpret_cast<void*>(offsetof(BatchVertex, texpage)));
  glBindVertexArray(0);

  glGenVertexArrays(1, &m_attributeless_vao_id);
  return true;
}

bool GPU_HW_OpenGL::CreateUniformBuffer()
{
  m_uniform_stream_buffer = GL::StreamBuffer::Create(GL_UNIFORM_BUFFER, UNIFORM_BUFFER_SIZE);
  if (!m_uniform_stream_buffer)
    return false;

  return true;
}

bool GPU_HW_OpenGL::CreateTextureBuffer()
{
  // We use the pixel unpack buffer here because we share it with CPU-decoded VRAM writes.
  m_texture_stream_buffer = GL::StreamBuffer::Create(GL_PIXEL_UNPACK_BUFFER, VRAM_UPDATE_TEXTURE_BUFFER_SIZE);
  if (!m_texture_stream_buffer)
    return false;

  if (m_max_texture_buffer_size > 0)
  {
    glGenTextures(1, &m_texture_buffer_r16ui_texture);
    glBindTexture(GL_TEXTURE_BUFFER, m_texture_buffer_r16ui_texture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R16UI, m_texture_stream_buffer->GetGLBufferId());
  }

  m_texture_stream_buffer->Unbind();
  return true;
}

bool GPU_HW_OpenGL::CompilePrograms()
{
  GPU_HW_ShaderGen shadergen(m_host_display->GetRenderAPI(), m_resolution_scale, m_true_color, m_texture_filtering,
                             m_supports_dual_source_blend);

  m_system->GetHostInterface()->DisplayLoadingScreen("Compiling Shaders...");

  for (u32 render_mode = 0; render_mode < 4; render_mode++)
  {
    for (u32 texture_mode = 0; texture_mode < 9; texture_mode++)
    {
      for (u8 dithering = 0; dithering < 2; dithering++)
      {
        const bool textured = (static_cast<TextureMode>(texture_mode) != TextureMode::Disabled);
        const std::string vs = shadergen.GenerateBatchVertexShader(textured);
        const std::string fs = shadergen.GenerateBatchFragmentShader(static_cast<BatchRenderMode>(render_mode),
                                                                     static_cast<TextureMode>(texture_mode),
                                                                     ConvertToBoolUnchecked(dithering));

        GL::Program& prog = m_render_programs[render_mode][texture_mode][dithering];
        if (!prog.Compile(vs, fs))
          return false;

        prog.BindAttribute(0, "a_pos");
        prog.BindAttribute(1, "a_col0");
        if (textured)
        {
          prog.BindAttribute(2, "a_texcoord");
          prog.BindAttribute(3, "a_texpage");
        }

        if (!m_is_gles)
          prog.BindFragData(0, "o_col0");

        if (!prog.Link())
          return false;

        prog.BindUniformBlock("UBOBlock", 1);
        if (textured)
        {
          prog.Bind();
          prog.Uniform1i("samp0", 0);
        }
      }
    }
  }

  for (u8 depth_24bit = 0; depth_24bit < 2; depth_24bit++)
  {
    for (u8 interlaced = 0; interlaced < 2; interlaced++)
    {
      GL::Program& prog = m_display_programs[depth_24bit][interlaced];
      const std::string vs = shadergen.GenerateScreenQuadVertexShader();
      const std::string fs = shadergen.GenerateDisplayFragmentShader(ConvertToBoolUnchecked(depth_24bit),
                                                                     ConvertToBoolUnchecked(interlaced));
      if (!prog.Compile(vs, fs))
        return false;

      if (!m_is_gles)
      {
        if (m_supports_dual_source_blend)
        {
          prog.BindFragDataIndexed(0, "o_col0");
          prog.BindFragDataIndexed(1, "o_col1");
        }
        else
        {
          prog.BindFragData(0, "o_col0");
        }
      }

      if (!prog.Link())
        return false;

      prog.BindUniformBlock("UBOBlock", 1);

      prog.Bind();
      prog.Uniform1i("samp0", 0);
    }
  }

  if (!m_vram_read_program.Compile(shadergen.GenerateScreenQuadVertexShader(),
                                   shadergen.GenerateVRAMReadFragmentShader()))
  {
    return false;
  }

  if (!m_is_gles)
    m_vram_read_program.BindFragData(0, "o_col0");

  if (!m_vram_read_program.Link())
    return false;

  m_vram_read_program.BindUniformBlock("UBOBlock", 1);

  m_vram_read_program.Bind();
  m_vram_read_program.Uniform1i("samp0", 0);

  if (m_supports_texture_buffer)
  {
    if (!m_vram_write_program.Compile(shadergen.GenerateScreenQuadVertexShader(),
                                      shadergen.GenerateVRAMWriteFragmentShader()))
    {
      return false;
    }

    if (!m_is_gles)
      m_vram_write_program.BindFragData(0, "o_col0");

    if (!m_vram_write_program.Link())
      return false;

    m_vram_write_program.BindUniformBlock("UBOBlock", 1);

    m_vram_write_program.Bind();
    m_vram_write_program.Uniform1i("samp0", 0);
  }

  return true;
}

void GPU_HW_OpenGL::SetDrawState(BatchRenderMode render_mode)
{
  const GL::Program& prog = m_render_programs[static_cast<u8>(render_mode)][static_cast<u8>(m_batch.texture_mode)]
                                             [BoolToUInt8(m_batch.dithering)];
  prog.Bind();

  if (m_batch.texture_mode != TextureMode::Disabled)
    m_vram_read_texture.Bind();

  if (m_batch.transparency_mode == TransparencyMode::Disabled || render_mode == BatchRenderMode::OnlyOpaque)
  {
    glDisable(GL_BLEND);
  }
  else
  {
    glEnable(GL_BLEND);
    glBlendEquationSeparate(
      m_batch.transparency_mode == TransparencyMode::BackgroundMinusForeground ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD,
      GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, m_supports_dual_source_blend ? GL_SRC1_ALPHA : GL_SRC_ALPHA, GL_ONE, GL_ZERO);
  }

  if (m_drawing_area_changed)
  {
    m_drawing_area_changed = false;
    m_vram_dirty_rect.Include(m_drawing_area);
    SetScissorFromDrawingArea();
  }

  if (m_batch_ubo_dirty)
  {
    UploadUniformBlock(&m_batch_ubo_data, sizeof(m_batch_ubo_data));
    m_batch_ubo_dirty = false;
  }
}

void GPU_HW_OpenGL::SetScissorFromDrawingArea()
{
  int left, top, right, bottom;
  CalcScissorRect(&left, &top, &right, &bottom);

  const int width = right - left;
  const int height = bottom - top;
  const int x = left;
  const int y = m_vram_texture.GetHeight() - bottom;

  Log_DebugPrintf("SetScissor: (%d-%d, %d-%d)", x, x + width, y, y + height);
  glScissor(x, y, width, height);
}

void GPU_HW_OpenGL::UploadUniformBlock(const void* data, u32 data_size)
{
  const GL::StreamBuffer::MappingResult res = m_uniform_stream_buffer->Map(m_uniform_buffer_alignment, data_size);
  std::memcpy(res.pointer, data, data_size);
  m_uniform_stream_buffer->Unmap(data_size);

  glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_uniform_stream_buffer->GetGLBufferId(), res.buffer_offset, data_size);

  m_renderer_stats.num_uniform_buffer_updates++;
}

void GPU_HW_OpenGL::UpdateDisplay()
{
  GPU_HW::UpdateDisplay();

  if (m_system->GetSettings().debugging.show_vram)
  {
    m_host_display->SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_vram_texture.GetGLId())), 0,
                                      m_vram_texture.GetHeight(), m_vram_texture.GetWidth(),
                                      -static_cast<s32>(m_vram_texture.GetHeight()), m_vram_texture.GetWidth(),
                                      m_vram_texture.GetHeight(), 1.0f);
  }
  else
  {
    const u32 vram_offset_x = m_crtc_state.regs.X;
    const u32 vram_offset_y = m_crtc_state.regs.Y;
    const u32 scaled_vram_offset_x = vram_offset_x * m_resolution_scale;
    const u32 scaled_vram_offset_y = vram_offset_y * m_resolution_scale;
    const u32 display_width = std::min<u32>(m_crtc_state.display_width, VRAM_WIDTH - vram_offset_x);
    const u32 display_height = std::min<u32>(m_crtc_state.display_height, VRAM_HEIGHT - vram_offset_y);
    const u32 scaled_display_width = display_width * m_resolution_scale;
    const u32 scaled_display_height = display_height * m_resolution_scale;
    const bool interlaced = IsDisplayInterlaced();

    if (m_GPUSTAT.display_disable)
    {
      m_host_display->SetDisplayTexture(nullptr, 0, 0, 0, 0, 0, 0, m_crtc_state.display_aspect_ratio);
    }
    else if (!m_GPUSTAT.display_area_color_depth_24 && !interlaced)
    {
      m_host_display->SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_vram_texture.GetGLId())),
                                        scaled_vram_offset_x, m_vram_texture.GetHeight() - scaled_vram_offset_y,
                                        scaled_display_width, -static_cast<s32>(scaled_display_height),
                                        m_vram_texture.GetWidth(), m_vram_texture.GetHeight(),
                                        m_crtc_state.display_aspect_ratio);
    }
    else
    {
      const u32 flipped_vram_offset_y = VRAM_HEIGHT - vram_offset_y - display_height;
      const u32 scaled_flipped_vram_offset_y =
        m_vram_texture.GetHeight() - scaled_vram_offset_y - scaled_display_height;
      const u32 field_offset = BoolToUInt8(interlaced && m_GPUSTAT.interlaced_field);

      glDisable(GL_BLEND);
      glDisable(GL_SCISSOR_TEST);

      const GL::Program& prog =
        m_display_programs[BoolToUInt8(m_GPUSTAT.display_area_color_depth_24)][BoolToUInt8(interlaced)];
      prog.Bind();

      // Because of how the reinterpret shader works, we need to use the downscaled version.
      if (m_GPUSTAT.display_area_color_depth_24 && m_resolution_scale > 1)
      {
        const u32 copy_width = std::min<u32>((display_width * 3) / 2, VRAM_WIDTH - vram_offset_x);
        const u32 scaled_copy_width = copy_width * m_resolution_scale;
        m_vram_encoding_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
        m_vram_texture.BindFramebuffer(GL_READ_FRAMEBUFFER);
        glBlitFramebuffer(scaled_vram_offset_x, scaled_flipped_vram_offset_y, scaled_vram_offset_x + scaled_copy_width,
                          scaled_flipped_vram_offset_y + scaled_display_height, vram_offset_x, flipped_vram_offset_y,
                          vram_offset_x + copy_width, flipped_vram_offset_y + display_height, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);

        m_display_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
        m_vram_encoding_texture.Bind();

        glViewport(0, field_offset, display_width, display_height);

        const u32 uniforms[4] = {vram_offset_x, flipped_vram_offset_y, field_offset};
        UploadUniformBlock(uniforms, sizeof(uniforms));
        m_batch_ubo_dirty = true;

        glDrawArrays(GL_TRIANGLES, 0, 3);

        m_host_display->SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_display_texture.GetGLId())),
                                          0, display_height, display_width, -static_cast<s32>(display_height),
                                          m_display_texture.GetWidth(), m_display_texture.GetHeight(),
                                          m_crtc_state.display_aspect_ratio);
      }
      else
      {
        m_display_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
        m_vram_texture.Bind();

        glViewport(0, field_offset, scaled_display_width, scaled_display_height);

        const u32 uniforms[4] = {scaled_vram_offset_x, scaled_flipped_vram_offset_y, field_offset};
        UploadUniformBlock(uniforms, sizeof(uniforms));
        m_batch_ubo_dirty = true;

        glDrawArrays(GL_TRIANGLES, 0, 3);

        m_host_display->SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_display_texture.GetGLId())),
                                          0, scaled_display_height, scaled_display_width,
                                          -static_cast<s32>(scaled_display_height), m_display_texture.GetWidth(),
                                          m_display_texture.GetHeight(), m_crtc_state.display_aspect_ratio);
      }

      // restore state
      m_vram_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
      glViewport(0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());
      glEnable(GL_SCISSOR_TEST);
    }
  }
}

void GPU_HW_OpenGL::ReadVRAM(u32 x, u32 y, u32 width, u32 height)
{
  // Get bounds with wrap-around handled.
  const Common::Rectangle<u32> copy_rect = GetVRAMTransferBounds(x, y, width, height);
  const u32 encoded_width = (copy_rect.GetWidth() + 1) / 2;
  const u32 encoded_height = copy_rect.GetHeight();

  // Encode the 24-bit texture as 16-bit.
  const u32 uniforms[4] = {copy_rect.left, VRAM_HEIGHT - copy_rect.top - copy_rect.GetHeight(), copy_rect.GetWidth(),
                           copy_rect.GetHeight()};
  m_vram_encoding_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  m_vram_texture.Bind();
  m_vram_read_program.Bind();
  UploadUniformBlock(uniforms, sizeof(uniforms));
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glViewport(0, 0, encoded_width, encoded_height);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // Readback encoded texture.
  m_vram_encoding_texture.BindFramebuffer(GL_READ_FRAMEBUFFER);
  glPixelStorei(GL_PACK_ALIGNMENT, 2);
  glPixelStorei(GL_PACK_ROW_LENGTH, VRAM_WIDTH / 2);
  glReadPixels(0, 0, encoded_width, encoded_height, GL_RGBA, GL_UNSIGNED_BYTE,
               &m_vram_shadow[copy_rect.top * VRAM_WIDTH + copy_rect.left]);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  RestoreGraphicsAPIState();
}

void GPU_HW_OpenGL::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  if ((x + width) > VRAM_WIDTH || (y + height) > VRAM_HEIGHT)
  {
    // CPU round trip if oversized for now.
    Log_WarningPrintf("Oversized VRAM fill (%u-%u, %u-%u), CPU round trip", x, x + width, y, y + height);
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    GPU::FillVRAM(x, y, width, height, color);
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, m_vram_shadow.data());
    return;
  }

  GPU_HW::FillVRAM(x, y, width, height, color);

  // scale coordinates
  x *= m_resolution_scale;
  y *= m_resolution_scale;
  width *= m_resolution_scale;
  height *= m_resolution_scale;

  glScissor(x, m_vram_texture.GetHeight() - y - height, width, height);

  // drop precision unless true colour is enabled
  if (!m_true_color)
    color = RGBA5551ToRGBA8888(RGBA8888ToRGBA5551(color));

  const auto [r, g, b, a] = RGBA8ToFloat(color);
  glClearColor(r, g, b, a);
  glClear(GL_COLOR_BUFFER_BIT);

  SetScissorFromDrawingArea();
}

void GPU_HW_OpenGL::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data)
{
  if ((x + width) > VRAM_WIDTH || (y + height) > VRAM_HEIGHT)
  {
    // CPU round trip if oversized for now.
    Log_WarningPrintf("Oversized VRAM update (%u-%u, %u-%u), CPU round trip", x, x + width, y, y + height);
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    GPU::UpdateVRAM(x, y, width, height, data);
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, m_vram_shadow.data());
    return;
  }

  GPU_HW::UpdateVRAM(x, y, width, height, data);

  const u32 num_pixels = width * height;
  if (num_pixels < m_max_texture_buffer_size)
  {
    const auto map_result = m_texture_stream_buffer->Map(sizeof(u16), num_pixels * sizeof(u16));
    std::memcpy(map_result.pointer, data, num_pixels * sizeof(u16));
    m_texture_stream_buffer->Unmap(num_pixels * sizeof(u16));
    m_texture_stream_buffer->Unbind();

    // viewport should be set to the whole VRAM size, so we can just set the scissor
    const u32 flipped_y = VRAM_HEIGHT - y - height;
    const u32 scaled_width = width * m_resolution_scale;
    const u32 scaled_height = height * m_resolution_scale;
    const u32 scaled_x = x * m_resolution_scale;
    const u32 scaled_y = y * m_resolution_scale;
    const u32 scaled_flipped_y = m_vram_texture.GetHeight() - scaled_y - scaled_height;
    glViewport(scaled_x, scaled_flipped_y, scaled_width, scaled_height);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    m_vram_write_program.Bind();
    glBindTexture(GL_TEXTURE_BUFFER, m_texture_buffer_r16ui_texture);

    const u32 uniforms[5] = {x, flipped_y, width, height, map_result.index_aligned};
    UploadUniformBlock(uniforms, sizeof(uniforms));

    glDrawArrays(GL_TRIANGLES, 0, 3);

    RestoreGraphicsAPIState();
  }
  else
  {
    const auto map_result = m_texture_stream_buffer->Map(sizeof(u32), num_pixels * sizeof(u32));

    // reverse copy the rows so it matches opengl's lower-left origin
    const u32 source_stride = width * sizeof(u16);
    const u8* source_ptr = static_cast<const u8*>(data) + (source_stride * (height - 1));
    u32* dest_ptr = static_cast<u32*>(map_result.pointer);
    for (u32 row = 0; row < height; row++)
    {
      const u8* source_row_ptr = source_ptr;

      for (u32 col = 0; col < width; col++)
      {
        u16 src_col;
        std::memcpy(&src_col, source_row_ptr, sizeof(src_col));
        source_row_ptr += sizeof(src_col);

        *(dest_ptr++) = RGBA5551ToRGBA8888(src_col);
      }

      source_ptr -= source_stride;
    }

    m_texture_stream_buffer->Unmap(num_pixels * sizeof(u32));
    m_texture_stream_buffer->Bind();

    // have to write to the 1x texture first
    if (m_resolution_scale > 1)
      m_vram_encoding_texture.Bind();
    else
      m_vram_texture.Bind();

    // lower-left origin flip happens here
    const u32 flipped_y = VRAM_HEIGHT - y - height;

    // update texture data
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, flipped_y, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                    reinterpret_cast<void*>(map_result.index_aligned * sizeof(u32)));
    m_texture_stream_buffer->Unbind();

    if (m_resolution_scale > 1)
    {
      // scale to internal resolution
      const u32 scaled_width = width * m_resolution_scale;
      const u32 scaled_height = height * m_resolution_scale;
      const u32 scaled_x = x * m_resolution_scale;
      const u32 scaled_y = y * m_resolution_scale;
      const u32 scaled_flipped_y = m_vram_texture.GetHeight() - scaled_y - scaled_height;
      glDisable(GL_SCISSOR_TEST);
      m_vram_encoding_texture.BindFramebuffer(GL_READ_FRAMEBUFFER);
      glBlitFramebuffer(x, flipped_y, x + width, flipped_y + height, scaled_x, scaled_flipped_y,
                        scaled_x + scaled_width, scaled_flipped_y + scaled_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glEnable(GL_SCISSOR_TEST);
    }
  }
}

void GPU_HW_OpenGL::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  if ((src_x + width) > VRAM_WIDTH || (src_y + height) > VRAM_HEIGHT || (dst_x + width) > VRAM_WIDTH ||
      (dst_y + height) > VRAM_HEIGHT)
  {
    Log_WarningPrintf("Oversized VRAM copy (%u,%u, %u,%u, %u,%u), CPU round trip", src_x, src_y, dst_x, dst_y, width,
                      height);
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    GPU::CopyVRAM(src_x, src_y, dst_x, dst_y, width, height);
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, m_vram_shadow.data());
    return;
  }

  GPU_HW::CopyVRAM(src_x, src_y, dst_x, dst_y, width, height);

  src_x *= m_resolution_scale;
  src_y *= m_resolution_scale;
  dst_x *= m_resolution_scale;
  dst_y *= m_resolution_scale;
  width *= m_resolution_scale;
  height *= m_resolution_scale;

  // lower-left origin flip
  src_y = m_vram_texture.GetHeight() - src_y - height;
  dst_y = m_vram_texture.GetHeight() - dst_y - height;

  if (GLAD_GL_VERSION_4_3)
  {
    glCopyImageSubData(m_vram_texture.GetGLId(), GL_TEXTURE_2D, 0, src_x, src_y, 0, m_vram_texture.GetGLId(),
                       GL_TEXTURE_2D, 0, dst_x, dst_y, 0, width, height, 1);
  }
  else if (GLAD_GL_EXT_copy_image)
  {
    glCopyImageSubDataEXT(m_vram_texture.GetGLId(), GL_TEXTURE_2D, 0, src_x, src_y, 0, m_vram_texture.GetGLId(),
                          GL_TEXTURE_2D, 0, dst_x, dst_y, 0, width, height, 1);
  }
  else
  {
    glDisable(GL_SCISSOR_TEST);
    m_vram_texture.BindFramebuffer(GL_READ_FRAMEBUFFER);
    glBlitFramebuffer(src_x, src_y, src_x + width, src_y + height, dst_x, dst_y, dst_x + width, dst_y + height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);
  }
}

void GPU_HW_OpenGL::UpdateVRAMReadTexture()
{
  const auto scaled_rect = m_vram_dirty_rect * m_resolution_scale;
  const u32 width = scaled_rect.GetWidth();
  const u32 height = scaled_rect.GetHeight();
  const u32 x = scaled_rect.left;
  const u32 y = m_vram_texture.GetHeight() - scaled_rect.top - height;

  if (GLAD_GL_VERSION_4_3)
  {
    glCopyImageSubData(m_vram_texture.GetGLId(), GL_TEXTURE_2D, 0, x, y, 0, m_vram_read_texture.GetGLId(),
                       GL_TEXTURE_2D, 0, x, y, 0, width, height, 1);
  }
  else if (GLAD_GL_EXT_copy_image)
  {
    glCopyImageSubDataEXT(m_vram_texture.GetGLId(), GL_TEXTURE_2D, 0, x, y, 0, m_vram_read_texture.GetGLId(),
                          GL_TEXTURE_2D, 0, x, y, 0, width, height, 1);
  }
  else
  {
    m_vram_read_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
    m_vram_texture.BindFramebuffer(GL_READ_FRAMEBUFFER);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(x, y, x + width, y + height, x, y, x + width, y + height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);
    m_vram_texture.BindFramebuffer(GL_FRAMEBUFFER);
  }
}

void GPU_HW_OpenGL::FlushRender()
{
  const u32 vertex_count = GetBatchVertexCount();
  if (vertex_count == 0)
    return;

  m_renderer_stats.num_batches++;

  m_vertex_stream_buffer->Unmap(vertex_count * sizeof(BatchVertex));
  m_vertex_stream_buffer->Bind();
  m_batch_start_vertex_ptr = nullptr;
  m_batch_end_vertex_ptr = nullptr;
  m_batch_current_vertex_ptr = nullptr;

  static constexpr std::array<GLenum, 4> gl_primitives = {{GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP}};

  if (m_batch.NeedsTwoPassRendering())
  {
    SetDrawState(BatchRenderMode::OnlyTransparent);
    glDrawArrays(gl_primitives[static_cast<u8>(m_batch.primitive)], m_batch_base_vertex, vertex_count);
    SetDrawState(BatchRenderMode::OnlyOpaque);
    glDrawArrays(gl_primitives[static_cast<u8>(m_batch.primitive)], m_batch_base_vertex, vertex_count);
  }
  else
  {
    SetDrawState(m_batch.GetRenderMode());
    glDrawArrays(gl_primitives[static_cast<u8>(m_batch.primitive)], m_batch_base_vertex, vertex_count);
  }
}

std::unique_ptr<GPU> GPU::CreateHardwareOpenGLRenderer()
{
  return std::make_unique<GPU_HW_OpenGL>();
}
