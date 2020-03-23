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

  const Settings& settings = m_system->GetSettings();
  m_resolution_scale = settings.gpu_resolution_scale;
  m_true_color = settings.gpu_true_color;
  m_scaled_dithering = settings.gpu_scaled_dithering;
  m_texture_filtering = settings.gpu_texture_filtering;
  if (m_resolution_scale < 1 || m_resolution_scale > m_max_resolution_scale)
  {
    m_system->GetHostInterface()->AddFormattedOSDMessage(5.0f, "Invalid resolution scale %ux specified. Maximum is %u.",
                                                         m_resolution_scale, m_max_resolution_scale);
    m_resolution_scale = std::clamp<u32>(m_resolution_scale, 1u, m_max_resolution_scale);
  }

  PrintSettingsToLog();
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

  const Settings& settings = m_system->GetSettings();
  m_resolution_scale = std::clamp<u32>(settings.gpu_resolution_scale, 1, m_max_resolution_scale);
  m_true_color = settings.gpu_true_color;
  m_scaled_dithering = settings.gpu_scaled_dithering;
  m_texture_filtering = settings.gpu_texture_filtering;
  PrintSettingsToLog();
}

void GPU_HW::PrintSettingsToLog()
{
  Log_InfoPrintf("Resolution Scale: %u (%ux%u), maximum %u", m_resolution_scale, VRAM_WIDTH * m_resolution_scale,
                 VRAM_HEIGHT * m_resolution_scale, m_max_resolution_scale);
  Log_InfoPrintf("Dithering: %s%s", m_true_color ? "Disabled" : "Enabled",
                 (!m_true_color && m_scaled_dithering) ? " (Scaled)" : "");
  Log_InfoPrintf("Texture Filtering: %s", m_texture_filtering ? "Enabled" : "Disabled");
  Log_InfoPrintf("Dual-source blending: %s", m_supports_dual_source_blend ? "Supported" : "Not supported");
}

void GPU_HW::LoadVertices(RenderCommand rc, u32 num_vertices, const u32* command_ptr)
{
  const u32 texpage = ZeroExtend32(m_draw_mode.mode_reg.bits) | (ZeroExtend32(m_draw_mode.palette_reg) << 16);

  s32 min_x = std::numeric_limits<s32>::max();
  s32 max_x = std::numeric_limits<s32>::min();
  s32 min_y = std::numeric_limits<s32>::max();
  s32 max_y = std::numeric_limits<s32>::min();

  // TODO: Move this to the GPU..
  switch (rc.primitive)
  {
    case Primitive::Polygon:
    {
      DebugAssert(num_vertices == 3 || num_vertices == 4);
      EnsureVertexBufferSpace(rc.quad_polygon ? 6 : 3);

      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;

      u32 buffer_pos = 1;
      std::array<BatchVertex, 4> vertices;
      for (u32 i = 0; i < 3; i++)
      {
        const u32 color = (shaded && i > 0) ? (command_ptr[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;
        const VertexPosition vp{command_ptr[buffer_pos++]};
        const u16 packed_texcoord = textured ? Truncate16(command_ptr[buffer_pos++]) : 0;

        vertices[i].Set(m_drawing_offset.x + vp.x, m_drawing_offset.y + vp.y, color, texpage, packed_texcoord);
      }

      // Cull polygons which are too large.
      if (std::abs(vertices[2].x - vertices[0].x) >= MAX_PRIMITIVE_WIDTH ||
          std::abs(vertices[2].x - vertices[1].x) >= MAX_PRIMITIVE_WIDTH ||
          std::abs(vertices[1].x - vertices[0].x) >= MAX_PRIMITIVE_WIDTH ||
          std::abs(vertices[2].y - vertices[0].y) >= MAX_PRIMITIVE_HEIGHT ||
          std::abs(vertices[2].y - vertices[1].y) >= MAX_PRIMITIVE_HEIGHT ||
          std::abs(vertices[1].y - vertices[0].y) >= MAX_PRIMITIVE_HEIGHT)
      {
        Log_DebugPrintf("Culling too-large polygon: %d,%d %d,%d %d,%d", vertices[0].x, vertices[0].y, vertices[1].x,
                        vertices[1].y, vertices[2].x, vertices[2].y);
      }
      else
      {
        min_x = std::min(std::min(vertices[0].x, vertices[1].x), vertices[2].x);
        max_x = std::max(std::max(vertices[0].x, vertices[1].x), vertices[2].x);
        min_y = std::min(std::min(vertices[0].y, vertices[1].y), vertices[2].y);
        max_y = std::max(std::max(vertices[0].y, vertices[1].y), vertices[2].y);

        std::memcpy(m_batch_current_vertex_ptr, vertices.data(), sizeof(BatchVertex) * 3);
        m_batch_current_vertex_ptr += 3;
      }

      // quads
      for (u32 i = 3; i < num_vertices; i++)
      {
        const u32 color = (shaded && i > 0) ? (command_ptr[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;
        const VertexPosition vp{command_ptr[buffer_pos++]};
        const u16 packed_texcoord = textured ? Truncate16(command_ptr[buffer_pos++]) : 0;

        vertices[3].Set(m_drawing_offset.x + vp.x, m_drawing_offset.y + vp.y, color, texpage, packed_texcoord);

        // Cull polygons which are too large.
        if (std::abs(vertices[3].x - vertices[2].x) >= MAX_PRIMITIVE_WIDTH ||
            std::abs(vertices[3].x - vertices[1].x) >= MAX_PRIMITIVE_WIDTH ||
            std::abs(vertices[1].x - vertices[2].x) >= MAX_PRIMITIVE_WIDTH ||
            std::abs(vertices[3].y - vertices[2].y) >= MAX_PRIMITIVE_HEIGHT ||
            std::abs(vertices[3].y - vertices[1].y) >= MAX_PRIMITIVE_HEIGHT ||
            std::abs(vertices[1].y - vertices[2].y) >= MAX_PRIMITIVE_HEIGHT)
        {
          Log_DebugPrintf("Culling too-large polygon (quad second half): %d,%d %d,%d %d,%d", vertices[2].x,
                          vertices[2].y, vertices[1].x, vertices[1].y, vertices[0].x, vertices[0].y);
        }
        else
        {
          min_x = std::min(min_x, vertices[3].x);
          max_x = std::max(max_x, vertices[3].x);
          min_y = std::min(min_y, vertices[3].y);
          max_y = std::max(max_y, vertices[3].y);

          AddVertex(vertices[2]);
          AddVertex(vertices[1]);
          AddVertex(vertices[3]);
        }
      }
    }
    break;

    case Primitive::Rectangle:
    {
      u32 buffer_pos = 1;
      const u32 color = rc.color_for_first_vertex;
      const VertexPosition vp{command_ptr[buffer_pos++]};
      const s32 pos_x = m_drawing_offset.x + vp.x;
      const s32 pos_y = m_drawing_offset.y + vp.y;

      const auto [texcoord_x, texcoord_y] =
        UnpackTexcoord(rc.texture_enable ? Truncate16(command_ptr[buffer_pos++]) : 0);
      u16 orig_tex_left = ZeroExtend16(texcoord_x);
      u16 orig_tex_top = ZeroExtend16(texcoord_y);
      s32 rectangle_width;
      s32 rectangle_height;
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
          rectangle_width = static_cast<s32>(command_ptr[buffer_pos] & 0xFFFF);
          rectangle_height = static_cast<s32>(command_ptr[buffer_pos] >> 16);
          break;
      }

      if (rectangle_width >= MAX_PRIMITIVE_WIDTH || rectangle_height >= MAX_PRIMITIVE_HEIGHT)
      {
        Log_DebugPrintf("Culling too-large rectangle: %d,%d %dx%d", pos_x, pos_y, rectangle_width, rectangle_height);
        return;
      }

      // we can split the rectangle up into potentially 8 quads
      const u32 required_vertices = 6 * ((rectangle_width + (TEXTURE_PAGE_WIDTH - 1)) / TEXTURE_PAGE_WIDTH) *
                                    ((rectangle_height + (TEXTURE_PAGE_HEIGHT - 1)) / TEXTURE_PAGE_HEIGHT);
      EnsureVertexBufferSpace(required_vertices);

      min_x = pos_x;
      min_y = pos_y;
      max_x = pos_x + rectangle_width;
      max_y = pos_y + rectangle_height;

      // Split the rectangle into multiple quads if it's greater than 256x256, as the texture page should repeat.
      u16 tex_top = orig_tex_top;
      for (s32 y_offset = 0; y_offset < rectangle_height;)
      {
        const s32 quad_height = std::min<s32>(rectangle_height - y_offset, TEXTURE_PAGE_WIDTH - tex_top);
        const s32 quad_start_y = pos_y + y_offset;
        const s32 quad_end_y = quad_start_y + quad_height;
        const u16 tex_bottom = tex_top + static_cast<u16>(quad_height);

        u16 tex_left = orig_tex_left;
        for (s32 x_offset = 0; x_offset < rectangle_width;)
        {
          const s32 quad_width = std::min<s32>(rectangle_width - x_offset, TEXTURE_PAGE_HEIGHT - tex_left);
          const s32 quad_start_x = pos_x + x_offset;
          const s32 quad_end_x = quad_start_x + quad_width;
          const u16 tex_right = tex_left + static_cast<u16>(quad_width);

          AddNewVertex(quad_start_x, quad_start_y, color, texpage, tex_left, tex_top);
          AddNewVertex(quad_end_x, quad_start_y, color, texpage, tex_right, tex_top);
          AddNewVertex(quad_start_x, quad_end_y, color, texpage, tex_left, tex_bottom);

          AddNewVertex(quad_start_x, quad_end_y, color, texpage, tex_left, tex_bottom);
          AddNewVertex(quad_end_x, quad_start_y, color, texpage, tex_right, tex_top);
          AddNewVertex(quad_end_x, quad_end_y, color, texpage, tex_right, tex_bottom);

          x_offset += quad_width;
          tex_left = 0;
        }

        y_offset += quad_height;
        tex_top = 0;
      }
    }
    break;

    case Primitive::Line:
    {
      EnsureVertexBufferSpace(num_vertices * 2);

      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;

      u32 buffer_pos = 1;
      BatchVertex last_vertex;
      for (u32 i = 0; i < num_vertices; i++)
      {
        const u32 color = (shaded && i > 0) ? (command_ptr[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;
        const VertexPosition vp{command_ptr[buffer_pos++]};

        BatchVertex vertex;
        vertex.Set(m_drawing_offset.x + vp.x, m_drawing_offset.y + vp.y, color, 0, 0);

        if (i > 0)
        {
          if (std::abs(last_vertex.x - vertex.x) >= MAX_PRIMITIVE_WIDTH ||
              std::abs(last_vertex.y - vertex.y) >= MAX_PRIMITIVE_HEIGHT)
          {
            Log_DebugPrintf("Culling too-large line: %d,%d - %d,%d", last_vertex.x, last_vertex.y, vertex.x, vertex.y);
          }
          else
          {
            AddVertex(last_vertex);
            AddVertex(vertex);

            min_x = std::min(min_x, std::min(last_vertex.x, vertex.x));
            max_x = std::max(max_x, std::max(last_vertex.x, vertex.x));
            min_y = std::min(min_y, std::min(last_vertex.y, vertex.y));
            max_y = std::max(max_y, std::max(last_vertex.y, vertex.y));
          }
        }

        std::memcpy(&last_vertex, &vertex, sizeof(BatchVertex));
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }

  if (min_x <= max_x)
  {
    const Common::Rectangle<u32> area_covered(
      std::clamp(min_x, static_cast<s32>(m_drawing_area.left), static_cast<s32>(m_drawing_area.right)),
      std::clamp(min_y, static_cast<s32>(m_drawing_area.top), static_cast<s32>(m_drawing_area.bottom)),
      std::clamp(max_x, static_cast<s32>(m_drawing_area.left), static_cast<s32>(m_drawing_area.right)) + 1,
      std::clamp(max_y, static_cast<s32>(m_drawing_area.top), static_cast<s32>(m_drawing_area.bottom)) + 1);
    m_vram_dirty_rect.Include(area_covered);
  }
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
    return BatchPrimitive::Lines;
  else
    return BatchPrimitive::Triangles;
}

void GPU_HW::IncludeVRAMDityRectangle(const Common::Rectangle<u32>& rect)
{
  m_vram_dirty_rect.Include(rect);

  // the vram area can include the texture page, but the game can leave it as-is. in this case, set it as dirty so the
  // shadow texture is updated
  if (!m_draw_mode.IsTexturePageChanged() &&
      (m_draw_mode.GetTexturePageRectangle().Intersects(rect) ||
       (m_draw_mode.IsUsingPalette() && m_draw_mode.GetTexturePaletteRectangle().Intersects(rect))))
  {
    m_draw_mode.SetTexturePageChanged();
  }
}

void GPU_HW::EnsureVertexBufferSpace(u32 required_vertices)
{
  if (m_batch_current_vertex_ptr)
  {
    if (GetBatchVertexSpace() >= required_vertices)
      return;

    FlushRender();
  }

  MapBatchVertexPointer(required_vertices);
}

void GPU_HW::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  IncludeVRAMDityRectangle(
    Common::Rectangle<u32>::FromExtents(x, y, width, height).Clamped(0, 0, VRAM_WIDTH, VRAM_HEIGHT));
}

void GPU_HW::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data)
{
  DebugAssert((x + width) <= VRAM_WIDTH && (y + height) <= VRAM_HEIGHT);
  IncludeVRAMDityRectangle(Common::Rectangle<u32>::FromExtents(x, y, width, height));
}

void GPU_HW::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  IncludeVRAMDityRectangle(
    Common::Rectangle<u32>::FromExtents(dst_x, dst_y, width, height).Clamped(0, 0, VRAM_WIDTH, VRAM_HEIGHT));
}

void GPU_HW::DispatchRenderCommand(RenderCommand rc, u32 num_vertices, const u32* command_ptr)
{
  TextureMode texture_mode;
  if (rc.IsTexturingEnabled())
  {
    // texture page changed - check that the new page doesn't intersect the drawing area
    if (m_draw_mode.IsTexturePageChanged())
    {
      m_draw_mode.ClearTexturePageChangedFlag();
      if (m_vram_dirty_rect.Valid() &&
          (m_draw_mode.GetTexturePageRectangle().Intersects(m_vram_dirty_rect) ||
           (m_draw_mode.IsUsingPalette() && m_draw_mode.GetTexturePaletteRectangle().Intersects(m_vram_dirty_rect))))
      {
        Log_DevPrintf("Invalidating VRAM read cache due to drawing area overlap");
        if (!IsFlushed())
          FlushRender();

        UpdateVRAMReadTexture();
        m_renderer_stats.num_vram_read_texture_updates++;
        ClearVRAMDirtyRectangle();
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
  if (!IsFlushed())
  {
    if (m_batch.texture_mode != texture_mode || m_batch.transparency_mode != transparency_mode ||
        m_batch.primitive != rc_primitive || dithering_enable != m_batch.dithering || m_drawing_area_changed ||
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
    ImGui::SetColumnWidth(0, 200.0f * ImGui::GetIO().DisplayFramebufferScale.x);

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
