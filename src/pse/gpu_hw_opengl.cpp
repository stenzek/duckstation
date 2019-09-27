#include "gpu_hw_opengl.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
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

  CreateFramebuffer();
  CreateVertexBuffer();
  if (!CompilePrograms())
    return false;

  return true;
}

void GPU_HW_OpenGL::Reset()
{
  GPU_HW::Reset();

  ClearFramebuffer();
}

void GPU_HW_OpenGL::RenderUI()
{
  GPU_HW::RenderUI();

  ImGui::SetNextWindowSize(ImVec2(300.0f, 130.0f), ImGuiCond_Once);

  const bool is_null_frame = m_stats.num_batches == 0;
  if (!is_null_frame)
  {
    m_last_stats = m_stats;
    m_stats = {};
  }

  if (ImGui::Begin("GL Render Statistics"))
  {
    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, 200.0f);

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

    ImGui::TextUnformatted("GPU Active In This Frame: ");
    ImGui::NextColumn();
    ImGui::Text("%s", is_null_frame ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::Columns(1);

    ImGui::Checkbox("Show VRAM##gpu_gl_show_vram", &m_show_vram);
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

void GPU_HW_OpenGL::CreateFramebuffer()
{
  m_framebuffer_texture =
    std::make_unique<GL::Texture>(VRAM_WIDTH, VRAM_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false);

  glGenFramebuffers(1, &m_framebuffer_fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_framebuffer_texture->GetGLId(), 0);
  Assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

  m_vram_read_texture =
    std::make_unique<GL::Texture>(VRAM_WIDTH, VRAM_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false);
  glGenFramebuffers(1, &m_vram_read_fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, m_vram_read_fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_vram_read_texture->GetGLId(), 0);
  Assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

  m_display_texture = std::make_unique<GL::Texture>(VRAM_WIDTH, VRAM_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false);
  m_display_texture->Bind();
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glGenFramebuffers(1, &m_display_fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, m_display_fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_display_texture->GetGLId(), 0);
  Assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
}

void GPU_HW_OpenGL::ClearFramebuffer()
{
  // TODO: get rid of the FBO switches
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  m_system->GetHostInterface()->SetDisplayTexture(m_framebuffer_texture.get(), 0, 0, VRAM_WIDTH, VRAM_HEIGHT);
}

void GPU_HW_OpenGL::DestroyFramebuffer()
{
  glDeleteFramebuffers(1, &m_vram_read_fbo_id);
  m_vram_read_fbo_id = 0;
  m_vram_read_texture.reset();

  glDeleteFramebuffers(1, &m_framebuffer_fbo_id);
  m_framebuffer_fbo_id = 0;
  m_framebuffer_texture.reset();
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
  glVertexAttribPointer(2, 2, GL_UNSIGNED_BYTE, true, sizeof(HWVertex),
                        reinterpret_cast<void*>(offsetof(HWVertex, texcoord)));
  glVertexAttribIPointer(3, 1, GL_INT, sizeof(HWVertex), reinterpret_cast<void*>(offsetof(HWVertex, texpage)));
  glBindVertexArray(0);

  glGenVertexArrays(1, &m_attributeless_vao_id);
}

bool GPU_HW_OpenGL::CompilePrograms()
{
  for (u32 textured = 0; textured < 2; textured++)
  {
    for (u32 blending = 0; blending < 2; blending++)
    {
      for (u32 transparent = 0; transparent < 2; transparent++)
      {
        for (u32 format = 0; format < 3; format++)
        {
          // TODO: eliminate duplicate shaders here
          if (!CompileProgram(m_render_programs[textured][blending][transparent][format],
                              ConvertToBoolUnchecked(textured), ConvertToBoolUnchecked(blending),
                              ConvertToBoolUnchecked(transparent), static_cast<TextureColorMode>(format)))
          {
            return false;
          }
        }
      }
    }
  }

  return true;
}

bool GPU_HW_OpenGL::CompileProgram(GL::Program& prog, bool textured, bool blending, bool transparent,
                                   TextureColorMode texture_color_mode)
{
  const std::string vs = GenerateVertexShader(textured);
  const std::string fs = GenerateFragmentShader(textured, blending, transparent, texture_color_mode);
  if (!prog.Compile(vs.c_str(), fs.c_str()))
    return false;

  prog.BindAttribute(0, "a_pos");
  prog.BindAttribute(1, "a_col0");
  if (textured)
  {
    prog.BindAttribute(2, "a_tex0");
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
    prog.RegisterUniform("samp0");
    prog.Uniform1i(2, 0);
  }

  return true;
}

void GPU_HW_OpenGL::SetProgram()
{
  const GL::Program& prog =
    m_render_programs[BoolToUInt32(m_batch.texture_enable)][BoolToUInt32(m_batch.texture_blending_enable)]
                     [BoolToUInt32(m_batch.transparency_enable)][static_cast<u32>(m_batch.texture_color_mode)];
  prog.Bind();

  prog.Uniform2i(0, m_drawing_offset.x, m_drawing_offset.y);
  if (m_batch.transparency_enable)
  {
    static constexpr float transparent_alpha[4][2] = {{0.5f, 0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.25f, 1.0f}};
    prog.Uniform2fv(1, transparent_alpha[static_cast<u32>(m_batch.transparency_mode)]);
  }
  else
  {
    static constexpr float disabled_alpha[2] = {1.0f, 0.0f};
    prog.Uniform2fv(1, disabled_alpha);
  }

  if (m_batch.texture_enable)
    m_vram_read_texture->Bind();
}

void GPU_HW_OpenGL::SetViewport()
{
  glViewport(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
}

void GPU_HW_OpenGL::SetScissor()
{
  int left, top, right, bottom;
  CalcScissorRect(&left, &top, &right, &bottom);

  const int width = right - left;
  const int height = bottom - top;
  const int x = left;
  const int y = VRAM_HEIGHT - bottom;

  Log_DebugPrintf("SetScissor: (%d-%d, %d-%d)", x, x + width, y, y + height);
  glScissor(x, y, width, height);
}

void GPU_HW_OpenGL::SetBlendState()
{
  if (!m_batch.transparency_enable)
  {
    glDisable(GL_BLEND);
    return;
  }

  glEnable(GL_BLEND);
  glBlendEquationSeparate(m_batch.transparency_mode == GPU::TransparencyMode::BackgroundMinusForeground ?
                            GL_FUNC_REVERSE_SUBTRACT :
                            GL_FUNC_ADD,
                          GL_FUNC_ADD);
  glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ONE, GL_ZERO);
}

void GPU_HW_OpenGL::UpdateDisplay()
{
  GPU_HW::UpdateDisplay();

  // TODO: 24-bit support.
  if (m_show_vram)
  {
    m_system->GetHostInterface()->SetDisplayTexture(m_framebuffer_texture.get(), 0, 0, VRAM_WIDTH, VRAM_HEIGHT);
  }
  else
  {
    const u32 display_width = m_crtc_state.horizontal_resolution;
    const u32 display_height = m_crtc_state.vertical_resolution;
    const u32 vram_offset_x = m_crtc_state.regs.X;
    const u32 vram_offset_y = m_crtc_state.regs.Y;
    const u32 copy_width =
      ((vram_offset_x + display_width) > VRAM_WIDTH) ? (VRAM_WIDTH - vram_offset_x) : display_width;
    const u32 copy_height =
      ((vram_offset_y + display_height) > VRAM_HEIGHT) ? (VRAM_HEIGHT - vram_offset_y) : display_height;
    glCopyImageSubData(m_framebuffer_texture->GetGLId(), GL_TEXTURE_2D, 0, vram_offset_x,
                       VRAM_HEIGHT - vram_offset_y - copy_height, 0, m_display_texture->GetGLId(), GL_TEXTURE_2D, 0, 0,
                       0, 0, copy_width, copy_height, 1);

    m_system->GetHostInterface()->SetDisplayTexture(m_display_texture.get(), 0, 0, copy_width, copy_height);
  }
}

void GPU_HW_OpenGL::ReadVRAM(u32 x, u32 y, u32 width, u32 height, void* buffer)
{
  // we need to convert RGBA8 -> RGBA5551
  std::vector<u32> temp_buffer(width * height);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_framebuffer_fbo_id);
  glReadPixels(x, VRAM_HEIGHT - y - height, width, height, GL_RGBA, GL_UNSIGNED_BYTE, temp_buffer.data());

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
}

void GPU_HW_OpenGL::FillVRAM(u32 x, u32 y, u32 width, u32 height, u16 color)
{
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);

  glEnable(GL_SCISSOR_TEST);
  glScissor(x, VRAM_HEIGHT - y - height, width, height);

  const auto [r, g, b, a] = RGBA8ToFloat(RGBA5551ToRGBA8888(color));
  glClearColor(r, g, b, a);
  glClear(GL_COLOR_BUFFER_BIT);

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

  m_framebuffer_texture->Bind();

  // lower-left origin flip happens here
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, VRAM_HEIGHT - y - height, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                  rgba_data.data());

  InvalidateVRAMReadCache();
}

void GPU_HW_OpenGL::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  glDisable(GL_SCISSOR_TEST);

  // lower-left origin flip
  src_y = VRAM_HEIGHT - src_y - height;
  dst_y = VRAM_HEIGHT - dst_y - height;

  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glBlitFramebuffer(src_x, src_y, src_x + width, src_y + height, dst_x, dst_y, dst_x + width, dst_y + height,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);

  InvalidateVRAMReadCache();
}

void GPU_HW_OpenGL::UpdateVRAMReadTexture()
{
  m_stats.num_vram_read_texture_updates++;
  m_vram_read_texture_dirty = false;

  // TODO: Fallback blit path, and partial updates.
  glCopyImageSubData(m_framebuffer_texture->GetGLId(), GL_TEXTURE_2D, 0, 0, 0, 0, m_vram_read_texture->GetGLId(),
                     GL_TEXTURE_2D, 0, 0, 0, 0, VRAM_WIDTH, VRAM_HEIGHT, 1);
}

void GPU_HW_OpenGL::FlushRender()
{
  if (m_batch.vertices.empty())
    return;

  if (m_vram_read_texture_dirty)
    UpdateVRAMReadTexture();

  m_stats.num_batches++;
  m_stats.num_vertices += static_cast<u32>(m_batch.vertices.size());

  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  SetProgram();
  SetViewport();
  SetScissor();
  SetBlendState();

  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glBindVertexArray(m_vao_id);

  Assert((m_batch.vertices.size() * sizeof(HWVertex)) <= VERTEX_BUFFER_SIZE);
  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizei>(sizeof(HWVertex) * m_batch.vertices.size()),
                  m_batch.vertices.data());

  static constexpr std::array<GLenum, 4> gl_primitives = {{GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP}};
  glDrawArrays(gl_primitives[static_cast<u8>(m_batch.primitive)], 0, static_cast<GLsizei>(m_batch.vertices.size()));

  m_batch.vertices.clear();
}

std::unique_ptr<GPU> GPU::CreateHardwareOpenGLRenderer()
{
  return std::make_unique<GPU_HW_OpenGL>();
}
