#include "gpu_hw.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
#include "settings.h"
#include "system.h"
#include <imgui.h>
#include <sstream>
Log_SetChannel(GPU_HW);

GPU_HW::GPU_HW() = default;

GPU_HW::~GPU_HW() = default;

bool GPU_HW::Initialize(HostDisplay* host_display, System* system, DMA* dma, InterruptController* interrupt_controller,
                        Timers* timers)
{
  if (!GPU::Initialize(host_display, system, dma, interrupt_controller, timers))
    return false;

  m_resolution_scale = std::clamp<u32>(m_system->GetSettings().gpu_resolution_scale, 1, m_max_resolution_scale);
  m_system->GetSettings().max_gpu_resolution_scale = m_max_resolution_scale;
  m_true_color = m_system->GetSettings().gpu_true_color;
  return true;
}

void GPU_HW::Reset()
{
  GPU::Reset();

  m_batch = {};
  m_batch_ubo_data = {};
  m_batch_ubo_dirty = true;

  SetFullVRAMDirtyRectangle();
}

bool GPU_HW::DoState(StateWrapper& sw)
{
  if (!GPU::DoState(sw))
    return false;

  // invalidate the whole VRAM read texture when loading state
  if (sw.IsReading())
    SetFullVRAMDirtyRectangle();

  return true;
}

void GPU_HW::UpdateSettings()
{
  GPU::UpdateSettings();

  m_resolution_scale = std::clamp<u32>(m_system->GetSettings().gpu_resolution_scale, 1, m_max_resolution_scale);
  m_true_color = m_system->GetSettings().gpu_true_color;
}

void GPU_HW::LoadVertices(RenderCommand rc, u32 num_vertices, const u32* command_ptr)
{
  const u32 texpage =
    ZeroExtend32(m_render_state.texpage_attribute) | (ZeroExtend32(m_render_state.texlut_attribute) << 16);

  // TODO: Move this to the GPU..
  switch (rc.primitive)
  {
    case Primitive::Polygon:
    {
      // if we're drawing quads, we need to create a degenerate triangle to restart the triangle strip
      bool restart_strip = (rc.quad_polygon && !IsFlushed());
      if (restart_strip)
        AddDuplicateVertex();

      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;

      u32 buffer_pos = 1;
      for (u32 i = 0; i < num_vertices; i++)
      {
        const u32 color = (shaded && i > 0) ? (command_ptr[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;
        const VertexPosition vp{command_ptr[buffer_pos++]};
        const u16 packed_texcoord = textured ? Truncate16(command_ptr[buffer_pos++]) : 0;

        (m_batch_current_vertex_ptr++)->Set(vp.x, vp.y, color, texpage, packed_texcoord);

        if (restart_strip)
        {
          AddDuplicateVertex();
          restart_strip = false;
        }
      }
    }
    break;

    case Primitive::Rectangle:
    {
      // if we're drawing quads, we need to create a degenerate triangle to restart the triangle strip
      const bool restart_strip = !IsFlushed();
      if (restart_strip)
        AddDuplicateVertex();

      u32 buffer_pos = 1;
      const u32 color = rc.color_for_first_vertex;
      const VertexPosition vp{command_ptr[buffer_pos++]};
      const s32 pos_left = vp.x;
      const s32 pos_top = vp.y;
      const auto [texcoord_x, texcoord_y] =
        UnpackTexcoord(rc.texture_enable ? Truncate16(command_ptr[buffer_pos++]) : 0);
      const u16 tex_left = ZeroExtend16(texcoord_x);
      const u16 tex_top = ZeroExtend16(texcoord_y);
      u32 rectangle_width;
      u32 rectangle_height;
      switch (rc.rectangle_size)
      {
        case DrawRectangleSize::R1x1:
          rectangle_width = 1;
          rectangle_height = 1;
          break;
        case DrawRectangleSize::R8x8:
          rectangle_width = 8;
          rectangle_height = 8;
          break;
        case DrawRectangleSize::R16x16:
          rectangle_width = 16;
          rectangle_height = 16;
          break;
        default:
          rectangle_width = command_ptr[buffer_pos] & 0xFFFF;
          rectangle_height = command_ptr[buffer_pos] >> 16;
          break;
      }

      // TODO: This should repeat the texcoords instead of stretching
      const s32 pos_right = pos_left + static_cast<s32>(rectangle_width);
      const s32 pos_bottom = pos_top + static_cast<s32>(rectangle_height);
      const u16 tex_right = tex_left + static_cast<u16>(rectangle_width);
      const u16 tex_bottom = tex_top + static_cast<u16>(rectangle_height);

      (m_batch_current_vertex_ptr++)->Set(pos_left, pos_top, color, texpage, tex_left, tex_top);
      if (restart_strip)
        AddDuplicateVertex();

      (m_batch_current_vertex_ptr++)->Set(pos_right, pos_top, color, texpage, tex_right, tex_top);
      (m_batch_current_vertex_ptr++)->Set(pos_left, pos_bottom, color, texpage, tex_left, tex_bottom);
      (m_batch_current_vertex_ptr++)->Set(pos_right, pos_bottom, color, texpage, tex_right, tex_bottom);
    }
    break;

    case Primitive::Line:
    {
      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;

      u32 buffer_pos = 1;
      for (u32 i = 0; i < num_vertices; i++)
      {
        const u32 color = (shaded && i > 0) ? (command_ptr[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;
        const VertexPosition vp{command_ptr[buffer_pos++]};
        (m_batch_current_vertex_ptr++)->Set(vp.x, vp.y, color, 0, 0);
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

void GPU_HW::AddDuplicateVertex()
{
  std::memcpy(m_batch_current_vertex_ptr, m_batch_current_vertex_ptr - 1, sizeof(BatchVertex));
  m_batch_current_vertex_ptr++;
}

void GPU_HW::CalcScissorRect(int* left, int* top, int* right, int* bottom)
{
  *left = m_drawing_area.left * m_resolution_scale;
  *right = std::max<u32>((m_drawing_area.right + 1) * m_resolution_scale, *left + 1);
  *top = m_drawing_area.top * m_resolution_scale;
  *bottom = std::max<u32>((m_drawing_area.bottom + 1) * m_resolution_scale, *top + 1);
}

GPU_HW::BatchPrimitive GPU_HW::GetPrimitiveForCommand(RenderCommand rc)
{
  if (rc.primitive == Primitive::Line)
    return rc.polyline ? BatchPrimitive::LineStrip : BatchPrimitive::Lines;
  else if ((rc.primitive == Primitive::Polygon && rc.quad_polygon) || rc.primitive == Primitive::Rectangle)
    return BatchPrimitive::TriangleStrip;
  else
    return BatchPrimitive::Triangles;
}

void GPU_HW::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  m_vram_dirty_rect.Include(Common::Rectangle<u32>::FromExtents(x, y, width, height));
}

void GPU_HW::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data)
{
  m_vram_dirty_rect.Include(Common::Rectangle<u32>::FromExtents(x, y, width, height));
}

void GPU_HW::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  m_vram_dirty_rect.Include(Common::Rectangle<u32>::FromExtents(dst_x, dst_y, width, height));
}

void GPU_HW::DispatchRenderCommand(RenderCommand rc, u32 num_vertices, const u32* command_ptr)
{
  TextureMode texture_mode;
  if (rc.texture_enable)
  {
    // extract texture lut/page
    switch (rc.primitive)
    {
      case Primitive::Polygon:
      {
        if (rc.shading_enable)
          m_render_state.SetFromPolygonTexcoord(command_ptr[2], command_ptr[5]);
        else
          m_render_state.SetFromPolygonTexcoord(command_ptr[2], command_ptr[4]);
      }
      break;

      case Primitive::Rectangle:
      {
        m_render_state.SetFromRectangleTexcoord(command_ptr[2]);
        m_render_state.SetFromPageAttribute(Truncate16(m_GPUSTAT.bits));
      }
      break;

      default:
        break;
    }

    // texture page changed - check that the new page doesn't intersect the drawing area
    if (m_render_state.IsTexturePageChanged())
    {
      m_render_state.ClearTexturePageChangedFlag();
      if (m_vram_dirty_rect.Valid() && (m_render_state.GetTexturePageRectangle().Intersects(m_vram_dirty_rect) ||
                                        m_render_state.GetTexturePaletteRectangle().Intersects(m_vram_dirty_rect)))
      {
        Log_DevPrintf("Invalidating VRAM read cache due to drawing area overlap");
        if (!IsFlushed())
          FlushRender();

        UpdateVRAMReadTexture();
      }
    }

    texture_mode = m_render_state.texture_mode;
    if (rc.raw_texture_enable)
    {
      texture_mode =
        static_cast<TextureMode>(static_cast<u8>(texture_mode) | static_cast<u8>(TextureMode::RawTextureBit));
    }
  }
  else
  {
    m_render_state.SetFromPageAttribute(Truncate16(m_GPUSTAT.bits));
    texture_mode = TextureMode::Disabled;
  }

  // has any state changed which requires a new batch?
  const TransparencyMode transparency_mode =
    rc.transparency_enable ? m_render_state.transparency_mode : TransparencyMode::Disabled;
  const BatchPrimitive rc_primitive = GetPrimitiveForCommand(rc);
  const bool dithering_enable = (!m_true_color && rc.IsDitheringEnabled()) ? m_GPUSTAT.dither_enable : false;
  const u32 max_added_vertices = num_vertices + 5;
  if (!IsFlushed())
  {
    const bool buffer_overflow = GetBatchVertexSpace() < max_added_vertices;
    if (buffer_overflow || rc_primitive == BatchPrimitive::LineStrip || m_batch.texture_mode != texture_mode ||
        m_batch.transparency_mode != transparency_mode || m_batch.primitive != rc_primitive ||
        dithering_enable != m_batch.dithering || m_drawing_area_changed || m_drawing_offset_changed ||
        m_render_state.IsTextureWindowChanged())
    {
      FlushRender();
    }
  }

  // transparency mode change
  if (m_batch.transparency_mode != transparency_mode && transparency_mode != TransparencyMode::Disabled)
  {
    static constexpr float transparent_alpha[4][2] = {{0.5f, 0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.25f, 1.0f}};
    m_batch_ubo_data.u_src_alpha_factor = transparent_alpha[static_cast<u32>(transparency_mode)][0];
    m_batch_ubo_data.u_dst_alpha_factor = transparent_alpha[static_cast<u32>(transparency_mode)][1];
    m_batch_ubo_dirty = true;
  }

  if (m_drawing_offset_changed)
  {
    m_drawing_offset_changed = false;
    m_batch_ubo_data.u_pos_offset[0] = m_drawing_offset.x;
    m_batch_ubo_data.u_pos_offset[1] = m_drawing_offset.y;
    m_batch_ubo_dirty = true;
  }

  // map buffer if it's not already done
  if (!m_batch_current_vertex_ptr)
    MapBatchVertexPointer(max_added_vertices);

  // update state
  m_batch.primitive = rc_primitive;
  m_batch.texture_mode = texture_mode;
  m_batch.transparency_mode = transparency_mode;
  m_batch.dithering = dithering_enable;

  if (m_render_state.IsTextureWindowChanged())
  {
    m_render_state.ClearTextureWindowChangedFlag();

    m_batch_ubo_data.u_texture_window_mask[0] = ZeroExtend32(m_render_state.texture_window_mask_x);
    m_batch_ubo_data.u_texture_window_mask[1] = ZeroExtend32(m_render_state.texture_window_mask_y);
    m_batch_ubo_data.u_texture_window_offset[0] = ZeroExtend32(m_render_state.texture_window_offset_x);
    m_batch_ubo_data.u_texture_window_offset[1] = ZeroExtend32(m_render_state.texture_window_offset_y);
    m_batch_ubo_dirty = true;
  }

  LoadVertices(rc, num_vertices, command_ptr);
}

void GPU_HW::DrawRendererStats(bool is_idle_frame)
{
  if (!is_idle_frame)
  {
    m_last_renderer_stats = m_renderer_stats;
    m_renderer_stats = {};
  }

  if (ImGui::CollapsingHeader("Renderer Statistics", ImGuiTreeNodeFlags_DefaultOpen))
  {
    const auto& stats = m_last_renderer_stats;

    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, 200.0f);

    ImGui::TextUnformatted("Batches Drawn:");
    ImGui::NextColumn();
    ImGui::Text("%u", stats.num_batches);
    ImGui::NextColumn();

    ImGui::TextUnformatted("VRAM Read Texture Updates:");
    ImGui::NextColumn();
    ImGui::Text("%u", stats.num_vram_read_texture_updates);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Uniform Buffer Updates: ");
    ImGui::NextColumn();
    ImGui::Text("%u", stats.num_uniform_buffer_updates);
    ImGui::NextColumn();

    ImGui::Columns(1);
  }
}