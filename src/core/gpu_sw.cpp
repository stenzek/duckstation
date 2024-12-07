// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_sw.h"
#include "gpu_hw_texture_cache.h"
#include "settings.h"
#include "system.h"

#include "util/gpu_device.h"
#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/gsvector.h"
#include "common/gsvector_formatter.h"
#include "common/log.h"

#include <algorithm>

LOG_CHANNEL(GPU);

GPU_SW::GPU_SW() = default;

GPU_SW::~GPU_SW()
{
  g_gpu_device->RecycleTexture(std::move(m_upload_texture));
  m_backend.Shutdown();
}

const Threading::Thread* GPU_SW::GetSWThread() const
{
  return m_backend.GetThread();
}

bool GPU_SW::IsHardwareRenderer() const
{
  return false;
}

bool GPU_SW::Initialize(Error* error)
{
  if (!GPU::Initialize(error) || !m_backend.Initialize(g_settings.gpu_use_thread))
    return false;

  static constexpr const std::array formats_for_16bit = {GPUTexture::Format::RGB5A1, GPUTexture::Format::A1BGR5,
                                                         GPUTexture::Format::RGB565, GPUTexture::Format::RGBA8};
  for (const GPUTexture::Format format : formats_for_16bit)
  {
    if (g_gpu_device->SupportsTextureFormat(format))
    {
      m_16bit_display_format = format;
      break;
    }
  }

  // RGBA8 will always be supported, hence we'll find one.
  INFO_LOG("Using {} format for 16-bit display", GPUTexture::GetFormatName(m_16bit_display_format));
  Assert(m_16bit_display_format != GPUTexture::Format::Unknown);
  return true;
}

bool GPU_SW::DoState(StateWrapper& sw, bool update_display)
{
  // need to ensure the worker thread is done
  m_backend.Sync(true);

  // ignore the host texture for software mode, since we want to save vram here
  if (!GPU::DoState(sw, update_display))
    return false;

  // need to still call the TC, to toss any data in the state
  return GPUTextureCache::DoState(sw, true);
}

bool GPU_SW::DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss, bool update_display)
{
  m_backend.Sync(true);
  sw.DoBytes(g_vram, VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16));
  return GPU::DoMemoryState(sw, mss, update_display);
}

void GPU_SW::Reset(bool clear_vram)
{
  GPU::Reset(clear_vram);

  m_backend.Reset();
}

void GPU_SW::UpdateSettings(const Settings& old_settings)
{
  GPU::UpdateSettings(old_settings);
  if (g_settings.gpu_use_thread != old_settings.gpu_use_thread)
    m_backend.SetThreadEnabled(g_settings.gpu_use_thread);
}

GPUTexture* GPU_SW::GetDisplayTexture(u32 width, u32 height, GPUTexture::Format format)
{
  if (!m_upload_texture || m_upload_texture->GetWidth() != width || m_upload_texture->GetHeight() != height ||
      m_upload_texture->GetFormat() != format)
  {
    ClearDisplayTexture();
    g_gpu_device->RecycleTexture(std::move(m_upload_texture));
    m_upload_texture = g_gpu_device->FetchTexture(width, height, 1, 1, 1, GPUTexture::Type::Texture, format,
                                                  GPUTexture::Flags::AllowMap, nullptr, 0);
    if (!m_upload_texture) [[unlikely]]
      ERROR_LOG("Failed to create {}x{} {} texture", width, height, static_cast<u32>(format));
  }

  return m_upload_texture.get();
}

template<GPUTexture::Format display_format>
ALWAYS_INLINE_RELEASE bool GPU_SW::CopyOut15Bit(u32 src_x, u32 src_y, u32 width, u32 height, u32 line_skip)
{
  GPUTexture* texture = GetDisplayTexture(width, height, display_format);
  if (!texture) [[unlikely]]
    return false;

  u32 dst_stride = Common::AlignUpPow2(width * texture->GetPixelSize(), 4);
  u8* dst_ptr = m_upload_buffer.data();
  const bool mapped = texture->Map(reinterpret_cast<void**>(&dst_ptr), &dst_stride, 0, 0, width, height);

  // Fast path when not wrapping around.
  if ((src_x + width) <= VRAM_WIDTH && (src_y + height) <= VRAM_HEIGHT)
  {
    [[maybe_unused]] constexpr u32 pixels_per_vec = 8;
    [[maybe_unused]] const u32 aligned_width = Common::AlignDownPow2(width, pixels_per_vec);

    const u16* src_ptr = &g_vram[src_y * VRAM_WIDTH + src_x];
    const u32 src_step = VRAM_WIDTH << line_skip;

    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = src_ptr;
      u8* dst_row_ptr = dst_ptr;
      u32 x = 0;

#ifdef CPU_ARCH_SIMD
      for (; x < aligned_width; x += pixels_per_vec)
      {
        ConvertVRAMPixels<display_format>(dst_row_ptr, GSVector4i::load<false>(src_row_ptr));
        src_row_ptr += pixels_per_vec;
      }
#endif

      for (; x < width; x++)
        ConvertVRAMPixel<display_format>(dst_row_ptr, *(src_row_ptr++));

      src_ptr += src_step;
      dst_ptr += dst_stride;
    }
  }
  else
  {
    const u32 end_x = src_x + width;
    const u32 y_step = (1 << line_skip);
    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &g_vram[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
      u8* dst_row_ptr = dst_ptr;

      for (u32 col = src_x; col < end_x; col++)
        ConvertVRAMPixel<display_format>(dst_row_ptr, src_row_ptr[col % VRAM_WIDTH]);

      src_y += y_step;
      dst_ptr += dst_stride;
    }
  }

  if (mapped)
    texture->Unmap();
  else
    texture->Update(0, 0, width, height, m_upload_buffer.data(), dst_stride);

  return true;
}

ALWAYS_INLINE_RELEASE bool GPU_SW::CopyOut24Bit(u32 src_x, u32 src_y, u32 skip_x, u32 width, u32 height, u32 line_skip)
{
  GPUTexture* texture = GetDisplayTexture(width, height, FORMAT_FOR_24BIT);
  if (!texture) [[unlikely]]
    return false;

  u32 dst_stride = width * sizeof(u32);
  u8* dst_ptr = m_upload_buffer.data();
  const bool mapped = texture->Map(reinterpret_cast<void**>(&dst_ptr), &dst_stride, 0, 0, width, height);

  if ((src_x + width) <= VRAM_WIDTH && (src_y + (height << line_skip)) <= VRAM_HEIGHT)
  {
    const u8* src_ptr = reinterpret_cast<const u8*>(&g_vram[src_y * VRAM_WIDTH + src_x]) + (skip_x * 3);
    const u32 src_stride = (VRAM_WIDTH << line_skip) * sizeof(u16);
    for (u32 row = 0; row < height; row++)
    {
      const u8* src_row_ptr = src_ptr;
      u8* dst_row_ptr = reinterpret_cast<u8*>(dst_ptr);
      for (u32 col = 0; col < width; col++)
      {
        *(dst_row_ptr++) = *(src_row_ptr++);
        *(dst_row_ptr++) = *(src_row_ptr++);
        *(dst_row_ptr++) = *(src_row_ptr++);
        *(dst_row_ptr++) = 0xFF;
      }

      src_ptr += src_stride;
      dst_ptr += dst_stride;
    }
  }
  else
  {
    const u32 y_step = (1 << line_skip);

    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &g_vram[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
      u32* dst_row_ptr = reinterpret_cast<u32*>(dst_ptr);

      for (u32 col = 0; col < width; col++)
      {
        const u32 offset = (src_x + (((skip_x + col) * 3) / 2));
        const u16 s0 = src_row_ptr[offset % VRAM_WIDTH];
        const u16 s1 = src_row_ptr[(offset + 1) % VRAM_WIDTH];
        const u8 shift = static_cast<u8>(col & 1u) * 8;
        const u32 rgb = (((ZeroExtend32(s1) << 16) | ZeroExtend32(s0)) >> shift);

        *(dst_row_ptr++) = rgb | 0xFF000000u;
      }

      src_y += y_step;
      dst_ptr += dst_stride;
    }
  }

  if (mapped)
    texture->Unmap();
  else
    texture->Update(0, 0, width, height, m_upload_buffer.data(), dst_stride);

  return true;
}

bool GPU_SW::CopyOut(u32 src_x, u32 src_y, u32 skip_x, u32 width, u32 height, u32 line_skip, bool is_24bit)
{
  if (!is_24bit)
  {
    DebugAssert(skip_x == 0);

    switch (m_16bit_display_format)
    {
      case GPUTexture::Format::RGB5A1:
        return CopyOut15Bit<GPUTexture::Format::RGB5A1>(src_x, src_y, width, height, line_skip);

      case GPUTexture::Format::A1BGR5:
        return CopyOut15Bit<GPUTexture::Format::A1BGR5>(src_x, src_y, width, height, line_skip);

      case GPUTexture::Format::RGB565:
        return CopyOut15Bit<GPUTexture::Format::RGB565>(src_x, src_y, width, height, line_skip);

      case GPUTexture::Format::RGBA8:
        return CopyOut15Bit<GPUTexture::Format::RGBA8>(src_x, src_y, width, height, line_skip);

      case GPUTexture::Format::BGRA8:
        return CopyOut15Bit<GPUTexture::Format::BGRA8>(src_x, src_y, width, height, line_skip);

      default:
        UnreachableCode();
    }
  }
  else
  {
    return CopyOut24Bit(src_x, src_y, skip_x, width, height, line_skip);
  }
}

void GPU_SW::UpdateDisplay()
{
  // fill display texture
  m_backend.Sync(true);

  if (!g_settings.debugging.show_vram)
  {
    if (IsDisplayDisabled())
    {
      ClearDisplayTexture();
      return;
    }

    const bool is_24bit = m_GPUSTAT.display_area_color_depth_24;
    const bool interlaced = IsInterlacedDisplayEnabled();
    const u32 field = GetInterlacedDisplayField();
    const u32 vram_offset_x = is_24bit ? m_crtc_state.regs.X : m_crtc_state.display_vram_left;
    const u32 vram_offset_y =
      m_crtc_state.display_vram_top + ((interlaced && m_GPUSTAT.vertical_resolution) ? field : 0);
    const u32 skip_x = is_24bit ? (m_crtc_state.display_vram_left - m_crtc_state.regs.X) : 0;
    const u32 read_width = m_crtc_state.display_vram_width;
    const u32 read_height = interlaced ? (m_crtc_state.display_vram_height / 2) : m_crtc_state.display_vram_height;

    if (IsInterlacedDisplayEnabled())
    {
      const u32 line_skip = m_GPUSTAT.vertical_resolution;
      if (CopyOut(vram_offset_x, vram_offset_y, skip_x, read_width, read_height, line_skip, is_24bit))
      {
        SetDisplayTexture(m_upload_texture.get(), nullptr, 0, 0, read_width, read_height);
        if (is_24bit && g_settings.display_24bit_chroma_smoothing)
        {
          if (ApplyChromaSmoothing())
            Deinterlace(field, 0);
        }
        else
        {
          Deinterlace(field, 0);
        }
      }
    }
    else
    {
      if (CopyOut(vram_offset_x, vram_offset_y, skip_x, read_width, read_height, 0, is_24bit))
      {
        SetDisplayTexture(m_upload_texture.get(), nullptr, 0, 0, read_width, read_height);
        if (is_24bit && g_settings.display_24bit_chroma_smoothing)
          ApplyChromaSmoothing();
      }
    }
  }
  else
  {
    if (CopyOut(0, 0, 0, VRAM_WIDTH, VRAM_HEIGHT, 0, false))
      SetDisplayTexture(m_upload_texture.get(), nullptr, 0, 0, VRAM_WIDTH, VRAM_HEIGHT);
  }
}

void GPU_SW::FillBackendCommandParameters(GPUBackendCommand* cmd) const
{
  cmd->params.bits = 0;
  cmd->params.check_mask_before_draw = m_GPUSTAT.check_mask_before_draw;
  cmd->params.set_mask_while_drawing = m_GPUSTAT.set_mask_while_drawing;
  cmd->params.active_line_lsb = m_crtc_state.active_line_lsb;
  cmd->params.interlaced_rendering = IsInterlacedRenderingEnabled();
}

void GPU_SW::FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const
{
  FillBackendCommandParameters(cmd);
  cmd->rc.bits = rc.bits;
  cmd->draw_mode.bits = m_draw_mode.mode_reg.bits;
  cmd->draw_mode.dither_enable = rc.IsDitheringEnabled() && cmd->draw_mode.dither_enable;
  cmd->palette.bits = m_draw_mode.palette_reg.bits;
  cmd->window = m_draw_mode.texture_window;
}

void GPU_SW::DispatchRenderCommand()
{
  if (m_drawing_area_changed)
  {
    GPUBackendSetDrawingAreaCommand* cmd = m_backend.NewSetDrawingAreaCommand();
    cmd->new_area = m_drawing_area;
    GSVector4i::store<false>(cmd->new_clamped_area, m_clamped_drawing_area);
    m_backend.PushCommand(cmd);
    m_drawing_area_changed = false;
  }

  const GPURenderCommand rc{m_render_command.bits};

  switch (rc.primitive)
  {
    case GPUPrimitive::Polygon:
    {
      const u32 num_vertices = rc.quad_polygon ? 4 : 3;
      GPUBackendDrawPolygonCommand* cmd = m_backend.NewDrawPolygonCommand(num_vertices);
      FillDrawCommand(cmd, rc);

      std::array<GSVector2i, 4> positions;
      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;
      for (u32 i = 0; i < num_vertices; i++)
      {
        GPUBackendDrawPolygonCommand::Vertex* vert = &cmd->vertices[i];
        vert->color = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
        const u64 maddr_and_pos = m_fifo.Pop();
        const GPUVertexPosition vp{Truncate32(maddr_and_pos)};
        vert->x = m_drawing_offset.x + vp.x;
        vert->y = m_drawing_offset.y + vp.y;
        vert->texcoord = textured ? Truncate16(FifoPop()) : 0;
        positions[i] = GSVector2i::load<false>(&vert->x);
      }

      // Cull polygons which are too large.
      const GSVector2i min_pos_12 = positions[1].min_s32(positions[2]);
      const GSVector2i max_pos_12 = positions[1].max_s32(positions[2]);
      const GSVector4i draw_rect_012 = GSVector4i(min_pos_12.min_s32(positions[0]))
                                         .upl64(GSVector4i(max_pos_12.max_s32(positions[0])))
                                         .add32(GSVector4i::cxpr(0, 0, 1, 1));
      const bool first_tri_culled =
        (draw_rect_012.width() > MAX_PRIMITIVE_WIDTH || draw_rect_012.height() > MAX_PRIMITIVE_HEIGHT ||
         !m_clamped_drawing_area.rintersects(draw_rect_012));
      if (first_tri_culled)
      {
        DEBUG_LOG("Culling off-screen/too-large polygon: {},{} {},{} {},{}", cmd->vertices[0].x, cmd->vertices[0].y,
                  cmd->vertices[1].x, cmd->vertices[1].y, cmd->vertices[2].x, cmd->vertices[2].y);

        if (!rc.quad_polygon)
          return;
      }
      else
      {
        AddDrawTriangleTicks(positions[0], positions[1], positions[2], rc.shading_enable, rc.texture_enable,
                             rc.transparency_enable);
      }

      // quads
      if (rc.quad_polygon)
      {
        const GSVector4i draw_rect_123 = GSVector4i(min_pos_12.min_s32(positions[3]))
                                           .upl64(GSVector4i(max_pos_12.max_s32(positions[3])))
                                           .add32(GSVector4i::cxpr(0, 0, 1, 1));

        // Cull polygons which are too large.
        const bool second_tri_culled =
          (draw_rect_123.width() > MAX_PRIMITIVE_WIDTH || draw_rect_123.height() > MAX_PRIMITIVE_HEIGHT ||
           !m_clamped_drawing_area.rintersects(draw_rect_123));
        if (second_tri_culled)
        {
          DEBUG_LOG("Culling too-large polygon (quad second half): {},{} {},{} {},{}", cmd->vertices[2].x,
                    cmd->vertices[2].y, cmd->vertices[1].x, cmd->vertices[1].y, cmd->vertices[0].x, cmd->vertices[0].y);

          if (first_tri_culled)
            return;
        }
        else
        {
          AddDrawTriangleTicks(positions[2], positions[1], positions[3], rc.shading_enable, rc.texture_enable,
                               rc.transparency_enable);
        }
      }

      m_backend.PushCommand(cmd);
    }
    break;

    case GPUPrimitive::Rectangle:
    {
      GPUBackendDrawRectangleCommand* cmd = m_backend.NewDrawRectangleCommand();
      FillDrawCommand(cmd, rc);
      cmd->color = rc.color_for_first_vertex;

      const GPUVertexPosition vp{FifoPop()};
      cmd->x = TruncateGPUVertexPosition(m_drawing_offset.x + vp.x);
      cmd->y = TruncateGPUVertexPosition(m_drawing_offset.y + vp.y);

      if (rc.texture_enable)
      {
        const u32 texcoord_and_palette = FifoPop();
        cmd->palette.bits = Truncate16(texcoord_and_palette >> 16);
        cmd->texcoord = Truncate16(texcoord_and_palette);
      }
      else
      {
        cmd->palette.bits = 0;
        cmd->texcoord = 0;
      }

      switch (rc.rectangle_size)
      {
        case GPUDrawRectangleSize::R1x1:
          cmd->width = 1;
          cmd->height = 1;
          break;
        case GPUDrawRectangleSize::R8x8:
          cmd->width = 8;
          cmd->height = 8;
          break;
        case GPUDrawRectangleSize::R16x16:
          cmd->width = 16;
          cmd->height = 16;
          break;
        default:
        {
          const u32 width_and_height = FifoPop();
          cmd->width = static_cast<u16>(width_and_height & VRAM_WIDTH_MASK);
          cmd->height = static_cast<u16>((width_and_height >> 16) & VRAM_HEIGHT_MASK);
        }
        break;
      }

      const GSVector4i rect = GSVector4i(cmd->x, cmd->y, cmd->x + cmd->width, cmd->y + cmd->height);
      const GSVector4i clamped_rect = m_clamped_drawing_area.rintersect(rect);
      if (clamped_rect.rempty()) [[unlikely]]
      {
        DEBUG_LOG("Culling off-screen rectangle {}", rect);
        return;
      }

      AddDrawRectangleTicks(clamped_rect, rc.texture_enable, rc.transparency_enable);

      m_backend.PushCommand(cmd);
    }
    break;

    case GPUPrimitive::Line:
    {
      if (!rc.polyline)
      {
        GPUBackendDrawLineCommand* cmd = m_backend.NewDrawLineCommand(2);
        FillDrawCommand(cmd, rc);
        cmd->palette.bits = 0;

        if (rc.shading_enable)
        {
          cmd->vertices[0].color = rc.color_for_first_vertex;
          const GPUVertexPosition start_pos{FifoPop()};
          cmd->vertices[0].x = m_drawing_offset.x + start_pos.x;
          cmd->vertices[0].y = m_drawing_offset.y + start_pos.y;

          cmd->vertices[1].color = FifoPop() & UINT32_C(0x00FFFFFF);
          const GPUVertexPosition end_pos{FifoPop()};
          cmd->vertices[1].x = m_drawing_offset.x + end_pos.x;
          cmd->vertices[1].y = m_drawing_offset.y + end_pos.y;
        }
        else
        {
          cmd->vertices[0].color = rc.color_for_first_vertex;
          cmd->vertices[1].color = rc.color_for_first_vertex;

          const GPUVertexPosition start_pos{FifoPop()};
          cmd->vertices[0].x = m_drawing_offset.x + start_pos.x;
          cmd->vertices[0].y = m_drawing_offset.y + start_pos.y;

          const GPUVertexPosition end_pos{FifoPop()};
          cmd->vertices[1].x = m_drawing_offset.x + end_pos.x;
          cmd->vertices[1].y = m_drawing_offset.y + end_pos.y;
        }

        const GSVector4i v0 = GSVector4i::loadl<false>(&cmd->vertices[0].x);
        const GSVector4i v1 = GSVector4i::loadl<false>(&cmd->vertices[1].x);
        const GSVector4i rect = v0.min_s32(v1).xyxy(v0.max_s32(v1)).add32(GSVector4i::cxpr(0, 0, 1, 1));
        const GSVector4i clamped_rect = rect.rintersect(m_clamped_drawing_area);

        if (rect.width() > MAX_PRIMITIVE_WIDTH || rect.height() > MAX_PRIMITIVE_HEIGHT || clamped_rect.rempty())
        {
          DEBUG_LOG("Culling too-large/off-screen line: {},{} - {},{}", cmd->vertices[0].y, cmd->vertices[0].y,
                    cmd->vertices[1].x, cmd->vertices[1].y);
          return;
        }

        AddDrawLineTicks(clamped_rect, rc.shading_enable);

        m_backend.PushCommand(cmd);
      }
      else
      {
        const u32 num_vertices = GetPolyLineVertexCount();

        GPUBackendDrawLineCommand* cmd = m_backend.NewDrawLineCommand((num_vertices - 1) * 2);
        FillDrawCommand(cmd, m_render_command);

        u32 buffer_pos = 0;
        const GPUVertexPosition start_vp{m_blit_buffer[buffer_pos++]};
        const GSVector2i draw_offset = GSVector2i::load<false>(&m_drawing_offset.x);
        GSVector2i start_pos = GSVector2i(start_vp.x, start_vp.y).add32(draw_offset);
        u32 start_color = m_render_command.color_for_first_vertex;

        const bool shaded = m_render_command.shading_enable;
        u32 out_vertex_count = 0;
        for (u32 i = 1; i < num_vertices; i++)
        {
          const u32 end_color =
            shaded ? (m_blit_buffer[buffer_pos++] & UINT32_C(0x00FFFFFF)) : m_render_command.color_for_first_vertex;
          const GPUVertexPosition vp{m_blit_buffer[buffer_pos++]};
          const GSVector2i end_pos = GSVector2i(vp.x, vp.y).add32(draw_offset);

          const GSVector4i rect = GSVector4i::xyxy(start_pos.min_s32(end_pos), start_pos.max_s32(end_pos))
                                    .add32(GSVector4i::cxpr(0, 0, 1, 1));
          const GSVector4i clamped_rect = rect.rintersect(m_clamped_drawing_area);

          if (rect.width() > MAX_PRIMITIVE_WIDTH || rect.height() > MAX_PRIMITIVE_HEIGHT || clamped_rect.rempty())
          {
            DEBUG_LOG("Culling too-large/off-screen line: {},{} - {},{}", cmd->vertices[i - 1].x,
                      cmd->vertices[i - 1].y, cmd->vertices[i].x, cmd->vertices[i].y);
          }
          else
          {
            AddDrawLineTicks(clamped_rect, rc.shading_enable);

            GPUBackendDrawLineCommand::Vertex* out_vertex = &cmd->vertices[out_vertex_count];
            out_vertex_count += 2;

            GSVector2i::store<false>(&out_vertex[0].x, start_pos);
            out_vertex[0].color = start_color;
            GSVector2i::store<false>(&out_vertex[1].x, end_pos);
            out_vertex[1].color = end_color;
          }

          start_pos = end_pos;
          start_color = end_color;
        }

        if (out_vertex_count > 0)
        {
          DebugAssert(out_vertex_count <= cmd->num_vertices);
          cmd->num_vertices = Truncate16(out_vertex_count);
          m_backend.PushCommand(cmd);
        }
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

void GPU_SW::ReadVRAM(u32 x, u32 y, u32 width, u32 height)
{
  m_backend.Sync(false);
}

void GPU_SW::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  GPUBackendFillVRAMCommand* cmd = m_backend.NewFillVRAMCommand();
  FillBackendCommandParameters(cmd);
  cmd->x = static_cast<u16>(x);
  cmd->y = static_cast<u16>(y);
  cmd->width = static_cast<u16>(width);
  cmd->height = static_cast<u16>(height);
  cmd->color = color;
  m_backend.PushCommand(cmd);
}

void GPU_SW::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask)
{
  const u32 num_words = width * height;
  GPUBackendUpdateVRAMCommand* cmd = m_backend.NewUpdateVRAMCommand(num_words);
  FillBackendCommandParameters(cmd);
  cmd->params.set_mask_while_drawing = set_mask;
  cmd->params.check_mask_before_draw = check_mask;
  cmd->x = static_cast<u16>(x);
  cmd->y = static_cast<u16>(y);
  cmd->width = static_cast<u16>(width);
  cmd->height = static_cast<u16>(height);
  std::memcpy(cmd->data, data, sizeof(u16) * num_words);
  m_backend.PushCommand(cmd);
}

void GPU_SW::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  GPUBackendCopyVRAMCommand* cmd = m_backend.NewCopyVRAMCommand();
  FillBackendCommandParameters(cmd);
  cmd->src_x = static_cast<u16>(src_x);
  cmd->src_y = static_cast<u16>(src_y);
  cmd->dst_x = static_cast<u16>(dst_x);
  cmd->dst_y = static_cast<u16>(dst_y);
  cmd->width = static_cast<u16>(width);
  cmd->height = static_cast<u16>(height);
  m_backend.PushCommand(cmd);
}

void GPU_SW::FlushRender()
{
}

void GPU_SW::UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit)
{
  GPUBackendUpdateCLUTCommand* cmd = m_backend.NewUpdateCLUTCommand();
  FillBackendCommandParameters(cmd);
  cmd->reg.bits = reg.bits;
  cmd->clut_is_8bit = clut_is_8bit;
  m_backend.PushCommand(cmd);
}

std::unique_ptr<GPU> GPU::CreateSoftwareRenderer()
{
  return std::make_unique<GPU_SW>();
}
