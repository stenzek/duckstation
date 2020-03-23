#include "gpu_hw_opengl_es.h"
#include "common/assert.h"
#include "common/log.h"
#include "gpu_hw_shadergen.h"
#include "host_display.h"
#include "system.h"
Log_SetChannel(GPU_HW_OpenGL_ES);

GPU_HW_OpenGL_ES::GPU_HW_OpenGL_ES() : GPU_HW(), m_vertex_buffer(VERTEX_BUFFER_SIZE / sizeof(BatchVertex)) {}

GPU_HW_OpenGL_ES::~GPU_HW_OpenGL_ES()
{
  // TODO: Destroy objects...
  if (m_host_display)
  {
    m_host_display->ClearDisplayTexture();
    ResetGraphicsAPIState();
  }
}

bool GPU_HW_OpenGL_ES::Initialize(HostDisplay* host_display, System* system, DMA* dma,
                                  InterruptController* interrupt_controller, Timers* timers)
{
  if (host_display->GetRenderAPI() != HostDisplay::RenderAPI::OpenGLES)
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

  if (!CompilePrograms())
  {
    Log_ErrorPrintf("Failed to compile programs");
    return false;
  }

  RestoreGraphicsAPIState();
  return true;
}

void GPU_HW_OpenGL_ES::Reset()
{
  GPU_HW::Reset();

  ClearFramebuffer();
}

void GPU_HW_OpenGL_ES::ResetGraphicsAPIState()
{
  GPU_HW::ResetGraphicsAPIState();

  glEnable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glLineWidth(1.0f);

  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  glDisableVertexAttribArray(2);
  glDisableVertexAttribArray(3);
}

void GPU_HW_OpenGL_ES::RestoreGraphicsAPIState()
{
  m_vram_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  glViewport(0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());

  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  glLineWidth(static_cast<float>(m_resolution_scale));
  glBindVertexArray(0);

  SetScissorFromDrawingArea();
  SetVertexPointers();
}

void GPU_HW_OpenGL_ES::UpdateSettings()
{
  GPU_HW::UpdateSettings();

  CreateFramebuffer();
  CompilePrograms();
  UpdateDisplay();
}

void GPU_HW_OpenGL_ES::MapBatchVertexPointer(u32 required_vertices)
{
  Assert(!m_batch_start_vertex_ptr);

  m_batch_start_vertex_ptr = m_vertex_buffer.data();
  m_batch_current_vertex_ptr = m_batch_start_vertex_ptr;
  m_batch_end_vertex_ptr = m_vertex_buffer.data() + m_vertex_buffer.size();
  m_batch_base_vertex = 0;
}

std::tuple<s32, s32> GPU_HW_OpenGL_ES::ConvertToFramebufferCoordinates(s32 x, s32 y)
{
  return std::make_tuple(x, static_cast<s32>(static_cast<s32>(VRAM_HEIGHT) - y));
}

void GPU_HW_OpenGL_ES::SetCapabilities(HostDisplay* host_display)
{
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
  m_supports_dual_source_blend = false;
}

bool GPU_HW_OpenGL_ES::CreateFramebuffer()
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

void GPU_HW_OpenGL_ES::ClearFramebuffer()
{
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  SetFullVRAMDirtyRectangle();
}

bool GPU_HW_OpenGL_ES::CompilePrograms()
{
  GPU_HW_ShaderGen shadergen(m_host_display->GetRenderAPI(), m_resolution_scale, m_true_color, m_scaled_dithering,
                             m_texture_filtering, m_supports_dual_source_blend);

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

        if (!prog.Link())
          return false;

        prog.Bind();

        prog.RegisterUniform("u_texture_window_mask");
        prog.RegisterUniform("u_texture_window_offset");
        prog.RegisterUniform("u_src_alpha_factor");
        prog.RegisterUniform("u_dst_alpha_factor");
        prog.RegisterUniform("u_set_mask_while_drawing");

        if (textured)
          prog.Uniform1i("samp0", 0);
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

      if (!prog.Link())
        return false;

      prog.Bind();
      prog.RegisterUniform("u_base_coords");
      prog.Uniform1i("samp0", 0);
    }
  }

  if (!m_vram_read_program.Compile(shadergen.GenerateScreenQuadVertexShader(),
                                   shadergen.GenerateVRAMReadFragmentShader()))
  {
    return false;
  }

  if (!m_vram_read_program.Link())
    return false;

  m_vram_read_program.Bind();
  m_vram_read_program.RegisterUniform("u_base_coords");
  m_vram_read_program.RegisterUniform("u_size");
  m_vram_read_program.Uniform1i("samp0", 0);
  return true;
}

void GPU_HW_OpenGL_ES::SetVertexPointers()
{
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(0, 2, GL_INT, sizeof(BatchVertex), &m_vertex_buffer[0].x);
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, true, sizeof(BatchVertex), &m_vertex_buffer[0].color);
  glVertexAttribIPointer(2, 1, GL_INT, sizeof(BatchVertex), &m_vertex_buffer[0].texcoord);
  glVertexAttribIPointer(3, 1, GL_INT, sizeof(BatchVertex), &m_vertex_buffer[0].texpage);
}

void GPU_HW_OpenGL_ES::SetDrawState(BatchRenderMode render_mode)
{
  const GL::Program& prog = m_render_programs[static_cast<u8>(render_mode)][static_cast<u8>(m_batch.texture_mode)]
                                             [BoolToUInt8(m_batch.dithering)];
  m_batch_ubo_dirty |= !prog.IsBound();
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
    glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ONE, GL_ZERO);
  }

  if (m_drawing_area_changed)
  {
    m_drawing_area_changed = false;
    m_vram_dirty_rect.Include(m_drawing_area);
    SetScissorFromDrawingArea();
  }

  if (m_batch_ubo_dirty)
  {
    prog.Uniform2uiv(0, m_batch_ubo_data.u_texture_window_mask);
    prog.Uniform2uiv(1, m_batch_ubo_data.u_texture_window_offset);
    prog.Uniform1f(2, m_batch_ubo_data.u_src_alpha_factor);
    prog.Uniform1f(3, m_batch_ubo_data.u_dst_alpha_factor);
    prog.Uniform1i(4, static_cast<s32>(m_batch_ubo_data.u_set_mask_while_drawing));
    m_batch_ubo_dirty = false;
  }
}

void GPU_HW_OpenGL_ES::SetScissorFromDrawingArea()
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

void GPU_HW_OpenGL_ES::UpdateDisplay()
{
  GPU_HW::UpdateDisplay();

  if (m_system->GetSettings().debugging.show_vram)
  {
    m_host_display->SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_vram_texture.GetGLId())),
                                      m_vram_texture.GetWidth(), static_cast<s32>(m_vram_texture.GetHeight()), 0,
                                      m_vram_texture.GetHeight(), m_vram_texture.GetWidth(),
                                      -static_cast<s32>(m_vram_texture.GetHeight()));
    m_host_display->SetDisplayParameters(VRAM_WIDTH, VRAM_HEIGHT, Common::Rectangle<s32>(0, 0, VRAM_WIDTH, VRAM_HEIGHT),
                                         1.0f);
  }
  else
  {
    const u32 vram_offset_x = m_crtc_state.regs.X;
    const u32 vram_offset_y = m_crtc_state.regs.Y;
    const u32 scaled_vram_offset_x = vram_offset_x * m_resolution_scale;
    const u32 scaled_vram_offset_y = vram_offset_y * m_resolution_scale;
    const u32 display_width = std::min<u32>(m_crtc_state.active_display_width, VRAM_WIDTH - vram_offset_x);
    const u32 display_height = std::min<u32>(m_crtc_state.active_display_height << BoolToUInt8(m_GPUSTAT.In480iMode()),
                                             VRAM_HEIGHT - vram_offset_y);
    const u32 scaled_display_width = display_width * m_resolution_scale;
    const u32 scaled_display_height = display_height * m_resolution_scale;
    const bool interlaced = IsDisplayInterlaced();

    if (m_GPUSTAT.display_disable)
    {
      m_host_display->ClearDisplayTexture();
    }
    else if (!m_GPUSTAT.display_area_color_depth_24 && !interlaced)
    {
      m_host_display->SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_vram_texture.GetGLId())),
                                        m_vram_texture.GetWidth(), m_vram_texture.GetHeight(), scaled_vram_offset_x,
                                        m_vram_texture.GetHeight() - scaled_vram_offset_y, scaled_display_width,
                                        -static_cast<s32>(scaled_display_height));
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

        prog.Uniform3i(0, static_cast<s32>(vram_offset_x), static_cast<s32>(flipped_vram_offset_y),
                       static_cast<s32>(field_offset));
        m_batch_ubo_dirty = true;

        glDrawArrays(GL_TRIANGLES, 0, 3);

        m_host_display->SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_display_texture.GetGLId())),
                                          m_display_texture.GetWidth(), m_display_texture.GetHeight(), 0,
                                          display_height, display_width, -static_cast<s32>(display_height));
      }
      else
      {
        m_display_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
        m_vram_texture.Bind();

        glViewport(0, field_offset, scaled_display_width, scaled_display_height);

        prog.Uniform3i(0, static_cast<s32>(scaled_vram_offset_x), static_cast<s32>(scaled_flipped_vram_offset_y),
                       static_cast<s32>(field_offset));
        m_batch_ubo_dirty = true;

        glDrawArrays(GL_TRIANGLES, 0, 3);

        m_host_display->SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_display_texture.GetGLId())),
                                          m_display_texture.GetWidth(), m_display_texture.GetHeight(), 0,
                                          scaled_display_height, scaled_display_width,
                                          -static_cast<s32>(scaled_display_height));
      }

      // restore state
      m_vram_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
      glViewport(0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());
      glEnable(GL_SCISSOR_TEST);
    }

    m_host_display->SetDisplayParameters(m_crtc_state.visible_display_width, m_crtc_state.visible_display_height,
                                         m_crtc_state.GetActiveDisplayRectangle(), m_crtc_state.display_aspect_ratio);
  }
}

void GPU_HW_OpenGL_ES::ReadVRAM(u32 x, u32 y, u32 width, u32 height)
{
  // Get bounds with wrap-around handled.
  const Common::Rectangle<u32> copy_rect = GetVRAMTransferBounds(x, y, width, height);
  const u32 encoded_width = (copy_rect.GetWidth() + 1) / 2;
  const u32 encoded_height = copy_rect.GetHeight();

  // Encode the 24-bit texture as 16-bit.
  m_vram_encoding_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  m_vram_texture.Bind();
  m_vram_read_program.Bind();
  m_vram_read_program.Uniform2i(0, copy_rect.left, VRAM_HEIGHT - copy_rect.top - copy_rect.GetHeight());
  m_vram_read_program.Uniform2i(1, copy_rect.GetWidth(), copy_rect.GetHeight());
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

void GPU_HW_OpenGL_ES::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
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

void GPU_HW_OpenGL_ES::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data)
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
  std::vector<u32> staging_buffer(num_pixels);

  // reverse copy the rows so it matches opengl's lower-left origin
  const u32 source_stride = width * sizeof(u16);
  const u8* source_ptr = static_cast<const u8*>(data) + (source_stride * (height - 1));
  u32* dest_ptr = static_cast<u32*>(staging_buffer.data());
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

  // have to write to the 1x texture first
  if (m_resolution_scale > 1)
    m_vram_encoding_texture.Bind();
  else
    m_vram_texture.Bind();

  // lower-left origin flip happens here
  const u32 flipped_y = VRAM_HEIGHT - y - height;

  // update texture data
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, flipped_y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, staging_buffer.data());

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
    glBlitFramebuffer(x, flipped_y, x + width, flipped_y + height, scaled_x, scaled_flipped_y, scaled_x + scaled_width,
                      scaled_flipped_y + scaled_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);
  }
}

void GPU_HW_OpenGL_ES::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
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

  if (GLAD_GL_EXT_copy_image)
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

void GPU_HW_OpenGL_ES::UpdateVRAMReadTexture()
{
  const auto scaled_rect = m_vram_dirty_rect * m_resolution_scale;
  const u32 width = scaled_rect.GetWidth();
  const u32 height = scaled_rect.GetHeight();
  const u32 x = scaled_rect.left;
  const u32 y = m_vram_texture.GetHeight() - scaled_rect.top - height;

  if (GLAD_GL_EXT_copy_image)
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

void GPU_HW_OpenGL_ES::FlushRender()
{
  static constexpr std::array<GLenum, 4> gl_primitives = {{GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP}};

  if (!m_batch_current_vertex_ptr)
    return;

  const u32 vertex_count = GetBatchVertexCount();
  m_batch_start_vertex_ptr = nullptr;
  m_batch_end_vertex_ptr = nullptr;
  m_batch_current_vertex_ptr = nullptr;
  if (vertex_count == 0)
    return;

  m_renderer_stats.num_batches++;

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

std::unique_ptr<GPU> GPU::CreateHardwareOpenGLESRenderer()
{
  return std::make_unique<GPU_HW_OpenGL_ES>();
}
