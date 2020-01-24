#include "gpu_hw.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "settings.h"
#include "system.h"
#include <imgui.h>
#include <sstream>
Log_SetChannel(GPU_HW);

GPU_HW::GPU_HW() : GPU()
{
  m_vram_ptr = m_vram_shadow.data();
}

GPU_HW::~GPU_HW() = default;

bool GPU_HW::IsHardwareRenderer() const
{
  return true;
}

bool GPU_HW::Initialize(HostDisplay* host_display, System* system, DMA* dma, InterruptController* interrupt_controller,
                        Timers* timers)
{
  if (!GPU::Initialize(host_display, system, dma, interrupt_controller, timers))
    return false;

  m_resolution_scale = std::clamp<u32>(m_system->GetSettings().gpu_resolution_scale, 1, m_max_resolution_scale);
  m_system->GetSettings().max_gpu_resolution_scale = m_max_resolution_scale;
  m_true_color = m_system->GetSettings().gpu_true_color;
  m_texture_filtering = m_system->GetSettings().gpu_texture_filtering;
  return true;
}

void GPU_HW::Reset()
{
  GPU::Reset();

  m_vram_shadow.fill(0);

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
  m_texture_filtering = m_system->GetSettings().gpu_texture_filtering;
}

void GPU_HW::LoadVertices(RenderCommand rc, u32 num_vertices, const u32* command_ptr)
{
  const u32 texpage = ZeroExtend32(m_draw_mode.mode_reg.bits) | (ZeroExtend32(m_draw_mode.palette_reg) << 16);

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

      s32 min_x = std::numeric_limits<s32>::max();
      s32 max_x = std::numeric_limits<s32>::min();
      s32 min_y = std::numeric_limits<s32>::max();
      s32 max_y = std::numeric_limits<s32>::min();

      u32 buffer_pos = 1;
      for (u32 i = 0; i < num_vertices; i++)
      {
        const u32 color = (shaded && i > 0) ? (command_ptr[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;
        const VertexPosition vp{command_ptr[buffer_pos++]};
        const u16 packed_texcoord = textured ? Truncate16(command_ptr[buffer_pos++]) : 0;
        const s32 x = vp.x;
        const s32 y = vp.y;

        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);

        AddVertex(x, y, color, texpage, packed_texcoord);

        if (restart_strip)
        {
          AddDuplicateVertex();
          restart_strip = false;
        }
      }

      // Cull polygons which are too large.
      if (static_cast<u32>(max_x - min_x) > MAX_PRIMITIVE_WIDTH ||
          static_cast<u32>(max_y - min_y) > MAX_PRIMITIVE_HEIGHT)
      {
        m_batch_current_vertex_ptr -= 2;
        AddDuplicateVertex();
        AddDuplicateVertex();
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

      if (rectangle_width >= MAX_PRIMITIVE_WIDTH || rectangle_height >= MAX_PRIMITIVE_HEIGHT)
        return;

      // TODO: This should repeat the texcoords instead of stretching
      const s32 pos_right = pos_left + static_cast<s32>(rectangle_width);
      const s32 pos_bottom = pos_top + static_cast<s32>(rectangle_height);
      const u16 tex_right = tex_left + static_cast<u16>(rectangle_width);
      const u16 tex_bottom = tex_top + static_cast<u16>(rectangle_height);

      AddVertex(pos_left, pos_top, color, texpage, tex_left, tex_top);
      if (restart_strip)
        AddDuplicateVertex();

      AddVertex(pos_right, pos_top, color, texpage, tex_right, tex_top);
      AddVertex(pos_left, pos_bottom, color, texpage, tex_left, tex_bottom);
      AddVertex(pos_right, pos_bottom, color, texpage, tex_right, tex_bottom);
    }
    break;

    case Primitive::Line:
    {
      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;

      s32 min_x = std::numeric_limits<s32>::max();
      s32 max_x = std::numeric_limits<s32>::min();
      s32 min_y = std::numeric_limits<s32>::max();
      s32 max_y = std::numeric_limits<s32>::min();

      u32 buffer_pos = 1;
      for (u32 i = 0; i < num_vertices; i++)
      {
        const u32 color = (shaded && i > 0) ? (command_ptr[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;
        const VertexPosition vp{command_ptr[buffer_pos++]};
        const s32 x = vp.x;
        const s32 y = vp.y;

        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);

        (m_batch_current_vertex_ptr++)->Set(x, y, color, 0, 0);
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
  std::memcpy(m_batch_current_vertex_ptr, &m_batch_last_vertex, sizeof(BatchVertex));
  m_batch_current_vertex_ptr++;
}

void GPU_HW::CalcScissorRect(int* left, int* top, int* right, int* bottom)
{
  *left = m_drawing_area.left * m_resolution_scale;
  *right = std::max<u32>((m_drawing_area.right + 1) * m_resolution_scale, *left + 1);
  *top = m_drawing_area.top * m_resolution_scale;
  *bottom = std::max<u32>((m_drawing_area.bottom + 1) * m_resolution_scale, *top + 1);
}

Common::Rectangle<u32> GPU_HW::GetVRAMTransferBounds(u32 x, u32 y, u32 width, u32 height)
{
  Common::Rectangle<u32> out_rc = Common::Rectangle<u32>::FromExtents(x, y, width, height);
  if (out_rc.right > VRAM_WIDTH)
  {
    out_rc.left = 0;
    out_rc.right = VRAM_WIDTH;
  }
  if (out_rc.bottom > VRAM_HEIGHT)
  {
    out_rc.top = 0;
    out_rc.bottom = VRAM_HEIGHT;
  }
  return out_rc;
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
  m_vram_dirty_rect.Include(Common::Rectangle<u32>::FromExtents(x, y, width, height).Clamped(0, 0, VRAM_WIDTH, VRAM_HEIGHT));
}

void GPU_HW::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data)
{
  DebugAssert((x + width) <= VRAM_WIDTH && (y + height) <= VRAM_HEIGHT);
  m_vram_dirty_rect.Include(Common::Rectangle<u32>::FromExtents(x, y, width, height));
}

void GPU_HW::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  m_vram_dirty_rect.Include(Common::Rectangle<u32>::FromExtents(dst_x, dst_y, width, height).Clamped(0, 0, VRAM_WIDTH, VRAM_HEIGHT));
}

void GPU_HW::DispatchRenderCommand(RenderCommand rc, u32 num_vertices, const u32* command_ptr)
{
  TextureMode texture_mode;
  if (rc.texture_enable)
  {
    // texture page changed - check that the new page doesn't intersect the drawing area
    if (m_draw_mode.IsTexturePageChanged())
    {
      m_draw_mode.ClearTexturePageChangedFlag();
      if (m_vram_dirty_rect.Valid() && (m_draw_mode.GetTexturePageRectangle().Intersects(m_vram_dirty_rect) ||
                                        m_draw_mode.GetTexturePaletteRectangle().Intersects(m_vram_dirty_rect)))
      {
        Log_DevPrintf("Invalidating VRAM read cache due to drawing area overlap");
        if (!IsFlushed())
          FlushRender();

        UpdateVRAMReadTexture();
      }
    }

    texture_mode = m_draw_mode.GetTextureMode();
    if (rc.raw_texture_enable)
    {
      texture_mode =
        static_cast<TextureMode>(static_cast<u8>(texture_mode) | static_cast<u8>(TextureMode::RawTextureBit));
    }
  }
  else
  {
    texture_mode = TextureMode::Disabled;
  }

  // has any state changed which requires a new batch?
  const TransparencyMode transparency_mode =
    rc.transparency_enable ? m_draw_mode.GetTransparencyMode() : TransparencyMode::Disabled;
  const BatchPrimitive rc_primitive = GetPrimitiveForCommand(rc);
  const bool dithering_enable = (!m_true_color && rc.IsDitheringEnabled()) ? m_GPUSTAT.dither_enable : false;
  const u32 max_added_vertices = num_vertices + 5;
  if (!IsFlushed())
  {
    const bool buffer_overflow = GetBatchVertexSpace() < max_added_vertices;
    if (buffer_overflow || rc_primitive == BatchPrimitive::LineStrip || m_batch.texture_mode != texture_mode ||
        m_batch.transparency_mode != transparency_mode || m_batch.primitive != rc_primitive ||
        dithering_enable != m_batch.dithering || m_drawing_area_changed || m_drawing_offset_changed ||
        m_draw_mode.IsTextureWindowChanged())
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

  if (m_batch.check_mask_before_draw != m_GPUSTAT.check_mask_before_draw ||
      m_batch.set_mask_while_drawing != m_GPUSTAT.set_mask_while_drawing)
  {
    m_batch.check_mask_before_draw = m_GPUSTAT.check_mask_before_draw;
    m_batch.set_mask_while_drawing = m_GPUSTAT.set_mask_while_drawing;
    m_batch_ubo_data.u_set_mask_while_drawing = BoolToUInt32(m_GPUSTAT.set_mask_while_drawing);
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

  if (m_draw_mode.IsTextureWindowChanged())
  {
    m_draw_mode.ClearTextureWindowChangedFlag();

    m_batch_ubo_data.u_texture_window_mask[0] = ZeroExtend32(m_draw_mode.texture_window_mask_x);
    m_batch_ubo_data.u_texture_window_mask[1] = ZeroExtend32(m_draw_mode.texture_window_mask_y);
    m_batch_ubo_data.u_texture_window_offset[0] = ZeroExtend32(m_draw_mode.texture_window_offset_x);
    m_batch_ubo_data.u_texture_window_offset[1] = ZeroExtend32(m_draw_mode.texture_window_offset_y);
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