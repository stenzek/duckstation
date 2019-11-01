#include "gpu_hw_opengl.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
#include "YBaseLib/String.h"
#include "host_interface.h"
#include "imgui.h"
#include "system.h"
Log_SetChannel(GPU_HW_OpenGL);

GPU_HW_OpenGL::GPU_HW_OpenGL() : GPU_HW() {}

GPU_HW_OpenGL::~GPU_HW_OpenGL()
{
  DestroyFramebuffer();
}

bool GPU_HW_OpenGL::Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, Timers* timers)
{
  if (!GPU_HW::Initialize(system, dma, interrupt_controller, timers))
    return false;

  SetMaxResolutionScale();
  CreateFramebuffer();
  CreateVertexBuffer();
  if (!CompilePrograms())
    return false;

  m_system->GetHostInterface()->SetDisplayTexture(m_display_texture.get(), 0, 0, VRAM_WIDTH, VRAM_HEIGHT, 1.0f);
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
  m_vram_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  glViewport(0, 0, m_vram_texture->GetWidth(), m_vram_texture->GetHeight());

  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  glLineWidth(static_cast<float>(m_resolution_scale));
  UpdateDrawingArea();

  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  glBindVertexArray(m_vao_id);
}

void GPU_HW_OpenGL::UpdateSettings()
{
  GPU_HW::UpdateSettings();

  CreateFramebuffer();
  CompilePrograms();
  UpdateDisplay();
}

void GPU_HW_OpenGL::DrawRendererStatsWindow()
{
  GPU_HW::DrawRendererStatsWindow();

  ImGui::SetNextWindowSize(ImVec2(300.0f, 150.0f), ImGuiCond_FirstUseEver);

  const bool is_null_frame = m_stats.num_batches == 0;
  if (!is_null_frame)
  {
    m_last_stats = m_stats;
    m_stats = {};
  }

  if (ImGui::Begin("GPU Renderer Statistics", &m_show_renderer_statistics))
  {
    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, 200.0f);

    ImGui::TextUnformatted("GPU Active In This Frame: ");
    ImGui::NextColumn();
    ImGui::Text("%s", is_null_frame ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextUnformatted("VRAM Reads: ");
    ImGui::NextColumn();
    ImGui::Text("%u", m_last_stats.num_vram_reads);
    ImGui::NextColumn();

    ImGui::TextUnformatted("VRAM Writes: ");
    ImGui::NextColumn();
    ImGui::Text("%u", m_last_stats.num_vram_writes);
    ImGui::NextColumn();

    ImGui::TextUnformatted("VRAM Read Texture Updates:");
    ImGui::NextColumn();
    ImGui::Text("%u", m_last_stats.num_vram_read_texture_updates);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Batches Drawn:");
    ImGui::NextColumn();
    ImGui::Text("%u", m_last_stats.num_batches);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Vertices Drawn: ");
    ImGui::NextColumn();
    ImGui::Text("%u", m_last_stats.num_vertices);
    ImGui::NextColumn();
  }

  ImGui::End();
}

void GPU_HW_OpenGL::InvalidateVRAMReadCache()
{
  m_vram_read_texture_dirty = true;
}

std::tuple<s32, s32> GPU_HW_OpenGL::ConvertToFramebufferCoordinates(s32 x, s32 y)
{
  return std::make_tuple(x, static_cast<s32>(static_cast<s32>(VRAM_HEIGHT) - y));
}

void GPU_HW_OpenGL::SetMaxResolutionScale()
{
  GLint max_texture_size = VRAM_WIDTH;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
  Log_InfoPrintf("Max texture size: %dx%d", max_texture_size, max_texture_size);
  const int max_texture_scale = max_texture_size / VRAM_WIDTH;

  std::array<int, 2> line_width_range = {{1, 1}};
  glGetIntegerv(GL_ALIASED_LINE_WIDTH_RANGE, line_width_range.data());
  Log_InfoPrintf("Max line width: %d", line_width_range[1]);

  m_max_resolution_scale = std::min(max_texture_scale, line_width_range[1]);
  Log_InfoPrintf("Maximum resolution scale is %u", m_max_resolution_scale);

  m_resolution_scale = std::min(m_resolution_scale, m_max_resolution_scale);
}

void GPU_HW_OpenGL::CreateFramebuffer()
{
  // save old vram texture/fbo, in case we're changing scale
  auto old_vram_texture = std::move(m_vram_texture);
  DestroyFramebuffer();

  // scale vram size to internal resolution
  const u32 texture_width = VRAM_WIDTH * m_resolution_scale;
  const u32 texture_height = VRAM_HEIGHT * m_resolution_scale;

  m_vram_texture =
    std::make_unique<GL::Texture>(texture_width, texture_height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false, true);

  // do we need to restore the framebuffer after a size change?
  if (old_vram_texture)
  {
    const bool linear_filter = old_vram_texture->GetWidth() > m_vram_texture->GetWidth();
    Log_DevPrintf("Scaling %ux%u VRAM texture to %ux%u using %s filter", old_vram_texture->GetWidth(),
                  old_vram_texture->GetHeight(), m_vram_texture->GetWidth(), m_vram_texture->GetHeight(),
                  linear_filter ? "linear" : "nearest");
    glDisable(GL_SCISSOR_TEST);
    old_vram_texture->BindFramebuffer(GL_READ_FRAMEBUFFER);
    glBlitFramebuffer(0, 0, old_vram_texture->GetWidth(), old_vram_texture->GetHeight(), 0, 0,
                      m_vram_texture->GetWidth(), m_vram_texture->GetHeight(), GL_COLOR_BUFFER_BIT,
                      linear_filter ? GL_LINEAR : GL_NEAREST);

    glEnable(GL_SCISSOR_TEST);
    old_vram_texture.reset();
  }

  m_vram_read_texture =
    std::make_unique<GL::Texture>(texture_width, texture_height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false, true);

  if (m_resolution_scale > 1)
  {
    m_vram_downsample_texture =
      std::make_unique<GL::Texture>(VRAM_WIDTH, VRAM_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false, true);
  }

  m_display_texture =
    std::make_unique<GL::Texture>(texture_width, texture_height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false, true);

  m_vram_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  m_vram_read_texture_dirty = true;
}

void GPU_HW_OpenGL::ClearFramebuffer()
{
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  m_vram_read_texture_dirty = true;
}

void GPU_HW_OpenGL::DestroyFramebuffer()
{
  m_vram_read_texture.reset();
  m_vram_texture.reset();
  m_vram_downsample_texture.reset();
  m_display_texture.reset();
}

void GPU_HW_OpenGL::CreateVertexBuffer()
{
  glGenBuffers(1, &m_vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, VERTEX_BUFFER_SIZE, nullptr, GL_STREAM_DRAW);

  glGenVertexArrays(1, &m_vao_id);
  glBindVertexArray(m_vao_id);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(0, 2, GL_INT, sizeof(HWVertex), reinterpret_cast<void*>(offsetof(HWVertex, x)));
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, true, sizeof(HWVertex),
                        reinterpret_cast<void*>(offsetof(HWVertex, color)));
  glVertexAttribIPointer(2, 2, GL_INT, sizeof(HWVertex), reinterpret_cast<void*>(offsetof(HWVertex, texcoord)));
  glVertexAttribIPointer(3, 1, GL_INT, sizeof(HWVertex), reinterpret_cast<void*>(offsetof(HWVertex, texpage)));
  glBindVertexArray(0);

  glGenVertexArrays(1, &m_attributeless_vao_id);
}

bool GPU_HW_OpenGL::CompilePrograms()
{
  for (u32 render_mode = 0; render_mode < 4; render_mode++)
  {
    for (u32 texture_mode = 0; texture_mode < 9; texture_mode++)
    {
      for (u8 dithering = 0; dithering < 2; dithering++)
      {
        if (!CompileProgram(m_render_programs[render_mode][texture_mode][dithering],
                            static_cast<HWBatchRenderMode>(render_mode), static_cast<TextureMode>(texture_mode),
                            ConvertToBoolUnchecked(dithering)))
        {
          return false;
        }
      }
    }
  }

  // TODO: Use string_view
  for (u8 depth_24bit = 0; depth_24bit < 2; depth_24bit++)
  {
    for (u8 interlaced = 0; interlaced < 2; interlaced++)
    {
      GL::Program& prog = m_display_programs[depth_24bit][interlaced];
      const std::string vs = GenerateScreenQuadVertexShader();
      const std::string fs =
        GenerateDisplayFragmentShader(ConvertToBoolUnchecked(depth_24bit), ConvertToBoolUnchecked(interlaced));
      if (!prog.Compile(vs.c_str(), fs.c_str()))
        return false;

      prog.BindFragData(0, "o_col0");
      if (!prog.Link())
        return false;

      prog.Bind();
      prog.RegisterUniform("u_base_coords");
      prog.RegisterUniform("samp0");
      prog.Uniform1i(1, 0);
    }
  }

  return true;
}

bool GPU_HW_OpenGL::CompileProgram(GL::Program& prog, HWBatchRenderMode render_mode, TextureMode texture_mode,
                                   bool dithering)
{
  const bool textured = texture_mode != TextureMode::Disabled;
  const std::string vs = GenerateVertexShader(textured);
  const std::string fs = GenerateFragmentShader(render_mode, texture_mode, dithering);
  if (!prog.Compile(vs.c_str(), fs.c_str()))
    return false;

  prog.BindAttribute(0, "a_pos");
  prog.BindAttribute(1, "a_col0");
  if (textured)
  {
    prog.BindAttribute(2, "a_texcoord");
    prog.BindAttribute(3, "a_texpage");
  }

  prog.BindFragData(0, "o_col0");

  if (!prog.Link())
    return false;

  prog.Bind();
  prog.RegisterUniform("u_pos_offset");
  prog.RegisterUniform("u_transparent_alpha");
  prog.Uniform2i(0, 0, 0);
  prog.Uniform2f(1, 1.0f, 0.0f);

  if (textured)
  {
    prog.RegisterUniform("u_texture_window");
    prog.RegisterUniform("samp0");
    prog.Uniform1i(3, 0);
  }

  return true;
}

void GPU_HW_OpenGL::SetDrawState(HWBatchRenderMode render_mode)
{
  const GL::Program& prog = m_render_programs[static_cast<u8>(render_mode)][static_cast<u8>(m_batch.texture_mode)]
                                             [BoolToUInt8(m_batch.dithering)];
  prog.Bind();

  prog.Uniform2i(0, m_drawing_offset.x, m_drawing_offset.y);
  if (m_batch.transparency_mode != TransparencyMode::Disabled)
  {
    static constexpr float transparent_alpha[4][2] = {{0.5f, 0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.25f, 1.0f}};
    prog.Uniform2fv(1, transparent_alpha[static_cast<u32>(m_batch.transparency_mode)]);
  }
  else
  {
    static constexpr float disabled_alpha[2] = {1.0f, 0.0f};
    prog.Uniform2fv(1, disabled_alpha);
  }

  if (m_batch.texture_mode != TextureMode::Disabled)
  {
    prog.Uniform4ui(2, m_batch.texture_window_values[0], m_batch.texture_window_values[1],
                    m_batch.texture_window_values[2], m_batch.texture_window_values[3]);
    m_vram_read_texture->Bind();
  }

  if (m_batch.transparency_mode == TransparencyMode::Disabled || render_mode == HWBatchRenderMode::OnlyOpaque)
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

    int left, top, right, bottom;
    CalcScissorRect(&left, &top, &right, &bottom);

    const int width = right - left;
    const int height = bottom - top;
    const int x = left;
    const int y = m_vram_texture->GetHeight() - bottom;

    Log_DebugPrintf("SetScissor: (%d-%d, %d-%d)", x, x + width, y, y + height);
    glScissor(x, y, width, height);
  }
}

void GPU_HW_OpenGL::UpdateDrawingArea()
{
  m_drawing_area_changed = true;
}

void GPU_HW_OpenGL::UpdateDisplay()
{
  GPU_HW::UpdateDisplay();

  if (m_system->GetSettings().debugging.show_vram)
  {
    m_system->GetHostInterface()->SetDisplayTexture(m_vram_texture.get(), 0, 0, m_vram_texture->GetWidth(),
                                                    m_vram_texture->GetHeight(), 1.0f);
  }
  else
  {
    const u32 vram_offset_x = m_crtc_state.regs.X;
    const u32 vram_offset_y = m_crtc_state.regs.Y;
    const u32 scaled_vram_offset_x = vram_offset_x * m_resolution_scale;
    const u32 scaled_vram_offset_y = vram_offset_y * m_resolution_scale;
    const u32 display_width = std::min<u32>(m_crtc_state.display_width, VRAM_WIDTH - vram_offset_x);
    const u32 display_height = std::min<u32>(m_crtc_state.display_height << BoolToUInt8(m_GPUSTAT.vertical_interlace),
                                             VRAM_HEIGHT - vram_offset_y);
    const u32 scaled_display_width = display_width * m_resolution_scale;
    const u32 scaled_display_height = display_height * m_resolution_scale;
    const u32 flipped_vram_offset_y = VRAM_HEIGHT - vram_offset_y - display_height;
    const u32 scaled_flipped_vram_offset_y = m_vram_texture->GetHeight() - scaled_vram_offset_y - scaled_display_height;

    if (m_GPUSTAT.display_disable)
    {
      m_system->GetHostInterface()->SetDisplayTexture(nullptr, 0, 0, 0, 0, m_crtc_state.display_aspect_ratio);
    }
    else if (!m_GPUSTAT.display_area_color_depth_24 && !m_GPUSTAT.vertical_interlace)
    {
      // fast path when both interlacing and 24-bit depth is off
      glCopyImageSubData(m_vram_texture->GetGLId(), GL_TEXTURE_2D, 0, scaled_vram_offset_x,
                         scaled_flipped_vram_offset_y, 0, m_display_texture->GetGLId(), GL_TEXTURE_2D, 0, 0, 0, 0,
                         scaled_display_width, scaled_display_height, 1);

      m_system->GetHostInterface()->SetDisplayTexture(m_display_texture.get(), 0, 0, scaled_display_width,
                                                      scaled_display_height, m_crtc_state.display_aspect_ratio);
    }
    else
    {
      const u32 field_offset = BoolToUInt8(m_GPUSTAT.vertical_interlace && !m_GPUSTAT.drawing_even_line);
      const u32 scaled_field_offset = field_offset * m_resolution_scale;

      glDisable(GL_BLEND);
      glDisable(GL_SCISSOR_TEST);

      const GL::Program& prog = m_display_programs[BoolToUInt8(m_GPUSTAT.display_area_color_depth_24)]
                                                  [BoolToUInt8(m_GPUSTAT.vertical_interlace)];
      prog.Bind();

      // Because of how the reinterpret shader works, we need to use the downscaled version.
      if (m_GPUSTAT.display_area_color_depth_24 && m_resolution_scale > 1)
      {
        m_vram_downsample_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
        m_vram_texture->BindFramebuffer(GL_READ_FRAMEBUFFER);
        glBlitFramebuffer(
          scaled_vram_offset_x, scaled_flipped_vram_offset_y, scaled_vram_offset_x + scaled_display_width,
          scaled_flipped_vram_offset_y + scaled_display_height, vram_offset_x, flipped_vram_offset_y,
          vram_offset_x + display_width, flipped_vram_offset_y + display_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        m_display_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
        m_vram_downsample_texture->Bind();

        glViewport(0, field_offset, display_width, display_height);
        prog.Uniform3i(0, vram_offset_x, flipped_vram_offset_y, field_offset);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        m_system->GetHostInterface()->SetDisplayTexture(m_display_texture.get(), 0, 0, display_width, display_height,
                                                        m_crtc_state.display_aspect_ratio);
      }
      else
      {
        m_display_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
        m_vram_texture->Bind();

        glViewport(0, scaled_field_offset, scaled_display_width, scaled_display_height);
        prog.Uniform3i(0, scaled_vram_offset_x, scaled_flipped_vram_offset_y, scaled_field_offset);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        m_system->GetHostInterface()->SetDisplayTexture(m_display_texture.get(), 0, 0, scaled_display_width,
                                                        scaled_display_height, m_crtc_state.display_aspect_ratio);
      }

      // restore state
      m_vram_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
      glViewport(0, 0, m_vram_texture->GetWidth(), m_vram_texture->GetHeight());
      glEnable(GL_SCISSOR_TEST);
    }
  }
}

void GPU_HW_OpenGL::ReadVRAM(u32 x, u32 y, u32 width, u32 height, void* buffer)
{
  // we need to convert RGBA8 -> RGBA5551
  std::vector<u32> temp_buffer(width * height);
  const u32 flipped_y = VRAM_HEIGHT - y - height;

  // downscaling to 1xIR.
  if (m_resolution_scale > 1)
  {
    const u32 texture_height = m_vram_texture->GetHeight();
    const u32 scaled_x = x * m_resolution_scale;
    const u32 scaled_y = y * m_resolution_scale;
    const u32 scaled_width = width * m_resolution_scale;
    const u32 scaled_height = height * m_resolution_scale;
    const u32 scaled_flipped_y = texture_height - scaled_y - scaled_height;

    m_vram_texture->BindFramebuffer(GL_READ_FRAMEBUFFER);
    m_vram_downsample_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(scaled_x, scaled_flipped_y, scaled_x + scaled_width, scaled_flipped_y + scaled_height, 0, 0,
                      width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glEnable(GL_SCISSOR_TEST);
    m_vram_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
    m_vram_downsample_texture->BindFramebuffer(GL_READ_FRAMEBUFFER);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, temp_buffer.data());
  }
  else
  {
    m_vram_texture->BindFramebuffer(GL_READ_FRAMEBUFFER);
    glReadPixels(x, flipped_y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, temp_buffer.data());
  }

  // reverse copy because of lower-left origin
  const u32 source_stride = width * sizeof(u32);
  const u8* source_ptr = reinterpret_cast<const u8*>(temp_buffer.data()) + (source_stride * (height - 1));
  const u32 dst_stride = width * sizeof(u16);
  u8* dst_ptr = static_cast<u8*>(buffer);
  for (u32 row = 0; row < height; row++)
  {
    const u8* source_row_ptr = source_ptr;
    u8* dst_row_ptr = dst_ptr;

    for (u32 col = 0; col < width; col++)
    {
      u32 src_col;
      std::memcpy(&src_col, source_row_ptr, sizeof(src_col));
      source_row_ptr += sizeof(src_col);

      const u16 dst_col = RGBA8888ToRGBA5551(src_col);
      std::memcpy(dst_row_ptr, &dst_col, sizeof(dst_col));
      dst_row_ptr += sizeof(dst_col);
    }

    source_ptr -= source_stride;
    dst_ptr += dst_stride;
  }

  m_stats.num_vram_reads++;
}

void GPU_HW_OpenGL::FillVRAM(u32 x, u32 y, u32 width, u32 height, u16 color)
{
  // scale coordinates
  x *= m_resolution_scale;
  y *= m_resolution_scale;
  width *= m_resolution_scale;
  height *= m_resolution_scale;

  glEnable(GL_SCISSOR_TEST);
  glScissor(x, m_vram_texture->GetHeight() - y - height, width, height);

  const auto [r, g, b, a] = RGBA8ToFloat(RGBA5551ToRGBA8888(color));
  glClearColor(r, g, b, a);
  glClear(GL_COLOR_BUFFER_BIT);

  UpdateDrawingArea();
  InvalidateVRAMReadCache();
}

void GPU_HW_OpenGL::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data)
{
  std::vector<u32> rgba_data;
  rgba_data.reserve(width * height);

  // reverse copy the rows so it matches opengl's lower-left origin
  const u32 source_stride = width * sizeof(u16);
  const u8* source_ptr = static_cast<const u8*>(data) + (source_stride * (height - 1));
  for (u32 row = 0; row < height; row++)
  {
    const u8* source_row_ptr = source_ptr;

    for (u32 col = 0; col < width; col++)
    {
      u16 src_col;
      std::memcpy(&src_col, source_row_ptr, sizeof(src_col));
      source_row_ptr += sizeof(src_col);

      const u32 dst_col = RGBA5551ToRGBA8888(src_col);
      rgba_data.push_back(dst_col);
    }

    source_ptr -= source_stride;
  }

  // have to write to the 1x texture first
  if (m_resolution_scale > 1)
    m_vram_downsample_texture->Bind();
  else
    m_vram_texture->Bind();

  // lower-left origin flip happens here
  const u32 flipped_y = VRAM_HEIGHT - y - height;

  // update texture data
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, flipped_y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba_data.data());
  InvalidateVRAMReadCache();

  if (m_resolution_scale > 1)
  {
    // scale to internal resolution
    const u32 scaled_width = width * m_resolution_scale;
    const u32 scaled_height = height * m_resolution_scale;
    const u32 scaled_x = x * m_resolution_scale;
    const u32 scaled_y = y * m_resolution_scale;
    const u32 scaled_flipped_y = m_vram_texture->GetHeight() - scaled_y - scaled_height;
    glDisable(GL_SCISSOR_TEST);
    m_vram_downsample_texture->BindFramebuffer(GL_READ_FRAMEBUFFER);
    glBlitFramebuffer(x, flipped_y, x + width, flipped_y + height, scaled_x, scaled_flipped_y, scaled_x + scaled_width,
                      scaled_flipped_y + scaled_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);
  }

  m_stats.num_vram_writes++;
}

void GPU_HW_OpenGL::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  src_x *= m_resolution_scale;
  src_y *= m_resolution_scale;
  dst_x *= m_resolution_scale;
  dst_y *= m_resolution_scale;
  width *= m_resolution_scale;
  height *= m_resolution_scale;

  // lower-left origin flip
  src_y = m_vram_texture->GetHeight() - src_y - height;
  dst_y = m_vram_texture->GetHeight() - dst_y - height;

  glDisable(GL_SCISSOR_TEST);
  m_vram_texture->BindFramebuffer(GL_READ_FRAMEBUFFER);
  glBlitFramebuffer(src_x, src_y, src_x + width, src_y + height, dst_x, dst_y, dst_x + width, dst_y + height,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glEnable(GL_SCISSOR_TEST);

  InvalidateVRAMReadCache();
}

void GPU_HW_OpenGL::UpdateVRAMReadTexture()
{
  m_stats.num_vram_read_texture_updates++;
  m_vram_read_texture_dirty = false;

  // TODO: Fallback blit path, and partial updates.
  glCopyImageSubData(m_vram_texture->GetGLId(), GL_TEXTURE_2D, 0, 0, 0, 0, m_vram_read_texture->GetGLId(),
                     GL_TEXTURE_2D, 0, 0, 0, 0, m_vram_texture->GetWidth(), m_vram_texture->GetHeight(), 1);
}

void GPU_HW_OpenGL::FlushRender()
{
  if (m_batch.vertices.empty())
    return;

  if (m_vram_read_texture_dirty)
    UpdateVRAMReadTexture();

  m_stats.num_batches++;
  m_stats.num_vertices += static_cast<u32>(m_batch.vertices.size());

  Assert((m_batch.vertices.size() * sizeof(HWVertex)) <= VERTEX_BUFFER_SIZE);
  glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizei>(sizeof(HWVertex) * m_batch.vertices.size()),
                  m_batch.vertices.data());

  static constexpr std::array<GLenum, 4> gl_primitives = {{GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP}};

  if (m_batch.NeedsTwoPassRendering())
  {
    SetDrawState(HWBatchRenderMode::OnlyTransparent);
    glDrawArrays(gl_primitives[static_cast<u8>(m_batch.primitive)], 0, static_cast<GLsizei>(m_batch.vertices.size()));
    SetDrawState(HWBatchRenderMode::OnlyOpaque);
    glDrawArrays(gl_primitives[static_cast<u8>(m_batch.primitive)], 0, static_cast<GLsizei>(m_batch.vertices.size()));
  }
  else
  {
    SetDrawState(m_batch.GetRenderMode());
    glDrawArrays(gl_primitives[static_cast<u8>(m_batch.primitive)], 0, static_cast<GLsizei>(m_batch.vertices.size()));
  }

  m_batch.vertices.clear();
}

std::unique_ptr<GPU> GPU::CreateHardwareOpenGLRenderer()
{
  return std::make_unique<GPU_HW_OpenGL>();
}
