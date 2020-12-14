#include "gpu_hw.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "cpu_core.h"
#include "pgxp.h"
#include "settings.h"
#include "system.h"
#include <cmath>
#include <sstream>
#include <tuple>
#ifdef WITH_IMGUI
#include "imgui.h"
#endif
Log_SetChannel(GPU_HW);

template<typename T>
ALWAYS_INLINE static constexpr std::tuple<T, T> MinMax(T v1, T v2)
{
  if (v1 > v2)
    return std::tie(v2, v1);
  else
    return std::tie(v1, v2);
}

ALWAYS_INLINE static bool ShouldUseUVLimits()
{
  // We only need UV limits if PGXP is enabled, or texture filtering is enabled.
  return g_settings.gpu_pgxp_enable || g_settings.gpu_texture_filter != GPUTextureFilter::Nearest;
}

GPU_HW::GPU_HW() : GPU()
{
  m_vram_ptr = m_vram_shadow.data();
}

GPU_HW::~GPU_HW() = default;

bool GPU_HW::IsHardwareRenderer() const
{
  return true;
}

bool GPU_HW::Initialize(HostDisplay* host_display)
{
  if (!GPU::Initialize(host_display))
    return false;

  m_resolution_scale = CalculateResolutionScale();
  m_multisamples = std::min(g_settings.gpu_multisamples, m_max_multisamples);
  m_render_api = host_display->GetRenderAPI();
  m_per_sample_shading = g_settings.gpu_per_sample_shading && m_supports_per_sample_shading;
  m_true_color = g_settings.gpu_true_color;
  m_scaled_dithering = g_settings.gpu_scaled_dithering;
  m_texture_filtering = g_settings.gpu_texture_filter;
  m_using_uv_limits = ShouldUseUVLimits();
  m_chroma_smoothing = g_settings.gpu_24bit_chroma_smoothing;

  if (m_multisamples != g_settings.gpu_multisamples)
  {
    g_host_interface->AddFormattedOSDMessage(
      20.0f, g_host_interface->TranslateString("OSDMessage", "%ux MSAA is not supported, using %ux instead."),
      g_settings.gpu_multisamples, m_multisamples);
  }
  if (!m_per_sample_shading && g_settings.gpu_per_sample_shading)
  {
    g_host_interface->AddOSDMessage(
      g_host_interface->TranslateStdString("OSDMessage", "SSAA is not supported, using MSAA instead."), 20.0f);
  }
  if (!m_supports_dual_source_blend && TextureFilterRequiresDualSourceBlend(m_texture_filtering))
  {
    g_host_interface->AddFormattedOSDMessage(
      20.0f, g_host_interface->TranslateString("OSDMessage", "Texture filter '%s' is not supported on your device."),
      Settings::GetTextureFilterDisplayName(m_texture_filtering));
    m_texture_filtering = GPUTextureFilter::Nearest;
  }

  PrintSettingsToLog();
  return true;
}

void GPU_HW::Reset()
{
  GPU::Reset();

  m_batch_current_vertex_ptr = m_batch_start_vertex_ptr;

  m_vram_shadow.fill(0);

  m_batch = {};
  m_batch_ubo_data = {};
  m_batch_ubo_dirty = true;
  m_current_depth = 1;

  SetFullVRAMDirtyRectangle();
}

bool GPU_HW::DoState(StateWrapper& sw, bool update_display)
{
  if (!GPU::DoState(sw, update_display))
    return false;

  // invalidate the whole VRAM read texture when loading state
  if (sw.IsReading())
  {
    m_batch_current_vertex_ptr = m_batch_start_vertex_ptr;
    SetFullVRAMDirtyRectangle();
    ResetBatchVertexDepth();
  }

  return true;
}

void GPU_HW::UpdateHWSettings(bool* framebuffer_changed, bool* shaders_changed)
{
  const u32 resolution_scale = CalculateResolutionScale();
  const u32 multisamples = std::min(m_max_multisamples, g_settings.gpu_multisamples);
  const bool per_sample_shading = g_settings.gpu_per_sample_shading && m_supports_per_sample_shading;
  const bool use_uv_limits = ShouldUseUVLimits();

  *framebuffer_changed = (m_resolution_scale != resolution_scale || m_multisamples != multisamples);
  *shaders_changed =
    (m_resolution_scale != resolution_scale || m_multisamples != multisamples ||
     m_true_color != g_settings.gpu_true_color || m_per_sample_shading != per_sample_shading ||
     m_scaled_dithering != g_settings.gpu_scaled_dithering || m_texture_filtering != g_settings.gpu_texture_filter ||
     m_using_uv_limits != use_uv_limits || m_chroma_smoothing != g_settings.gpu_24bit_chroma_smoothing);

  if (m_resolution_scale != resolution_scale)
  {
    g_host_interface->AddFormattedOSDMessage(
      10.0f, g_host_interface->TranslateString("OSDMessage", "Resolution scale set to %ux (display %ux%u, VRAM %ux%u)"),
      resolution_scale, m_crtc_state.display_vram_width * resolution_scale,
      resolution_scale * m_crtc_state.display_vram_height, VRAM_WIDTH * resolution_scale,
      VRAM_HEIGHT * resolution_scale);
  }

  if (m_multisamples != multisamples || m_per_sample_shading != per_sample_shading)
  {
    if (per_sample_shading)
    {
      g_host_interface->AddFormattedOSDMessage(
        10.0f, g_host_interface->TranslateString("OSDMessage", "Multisample anti-aliasing set to %ux (SSAA)."),
        multisamples);
    }
    else
    {
      g_host_interface->AddFormattedOSDMessage(
        10.0f, g_host_interface->TranslateString("OSDMessage", "Multisample anti-aliasing set to %ux."), multisamples);
    }
  }

  m_resolution_scale = resolution_scale;
  m_multisamples = multisamples;
  m_per_sample_shading = per_sample_shading;
  m_true_color = g_settings.gpu_true_color;
  m_scaled_dithering = g_settings.gpu_scaled_dithering;
  m_texture_filtering = g_settings.gpu_texture_filter;
  m_using_uv_limits = use_uv_limits;
  m_chroma_smoothing = g_settings.gpu_24bit_chroma_smoothing;

  if (!m_supports_dual_source_blend && TextureFilterRequiresDualSourceBlend(m_texture_filtering))
    m_texture_filtering = GPUTextureFilter::Nearest;

  PrintSettingsToLog();
}

u32 GPU_HW::CalculateResolutionScale() const
{
  if (g_settings.gpu_resolution_scale != 0)
    return std::clamp<u32>(g_settings.gpu_resolution_scale, 1, m_max_resolution_scale);

  // auto scaling
  const s32 height = (m_crtc_state.display_height != 0) ? static_cast<s32>(m_crtc_state.display_height) : 480;
  const s32 preferred_scale =
    static_cast<s32>(std::ceil(static_cast<float>(m_host_display->GetWindowHeight()) / height));
  Log_InfoPrintf("Height = %d, preferred scale = %d", height, preferred_scale);

  return static_cast<u32>(std::clamp<s32>(preferred_scale, 1, m_max_resolution_scale));
}

void GPU_HW::UpdateResolutionScale()
{
  GPU::UpdateResolutionScale();

  if (CalculateResolutionScale() != m_resolution_scale)
    UpdateSettings();
}

std::tuple<u32, u32> GPU_HW::GetEffectiveDisplayResolution()
{
  return std::make_tuple(m_crtc_state.display_vram_width * m_resolution_scale,
                         m_resolution_scale * m_crtc_state.display_vram_height);
}

void GPU_HW::PrintSettingsToLog()
{
  Log_InfoPrintf("Resolution Scale: %u (%ux%u), maximum %u", m_resolution_scale, VRAM_WIDTH * m_resolution_scale,
                 VRAM_HEIGHT * m_resolution_scale, m_max_resolution_scale);
  Log_InfoPrintf("Multisampling: %ux%s", m_multisamples, m_per_sample_shading ? " (per sample shading)" : "");
  Log_InfoPrintf("Dithering: %s%s", m_true_color ? "Disabled" : "Enabled",
                 (!m_true_color && m_scaled_dithering) ? " (Scaled)" : "");
  Log_InfoPrintf("Texture Filtering: %s", Settings::GetTextureFilterDisplayName(m_texture_filtering));
  Log_InfoPrintf("Dual-source blending: %s", m_supports_dual_source_blend ? "Supported" : "Not supported");
  Log_InfoPrintf("Using UV limits: %s", m_using_uv_limits ? "YES" : "NO");
}

void GPU_HW::UpdateVRAMReadTexture()
{
  m_renderer_stats.num_vram_read_texture_updates++;
  ClearVRAMDirtyRectangle();
}

void GPU_HW::HandleFlippedQuadTextureCoordinates(BatchVertex* vertices)
{
  // Taken from beetle-psx gpu_polygon.cpp
  // For X/Y flipped 2D sprites, PSX games rely on a very specific rasterization behavior. If U or V is decreasing in X
  // or Y, and we use the provided U/V as is, we will sample the wrong texel as interpolation covers an entire pixel,
  // while PSX samples its interpolation essentially in the top-left corner and splats that interpolant across the
  // entire pixel. While we could emulate this reasonably well in native resolution by shifting our vertex coords by
  // 0.5, this breaks in upscaling scenarios, because we have several samples per native sample and we need NN rules to
  // hit the same UV every time. One approach here is to use interpolate at offset or similar tricks to generalize the
  // PSX interpolation patterns, but the problem is that vertices sharing an edge will no longer see the same UV (due to
  // different plane derivatives), we end up sampling outside the intended boundary and artifacts are inevitable, so the
  // only case where we can apply this fixup is for "sprites" or similar which should not share edges, which leads to
  // this unfortunate code below.

  // It might be faster to do more direct checking here, but the code below handles primitives in any order and
  // orientation, and is far more SIMD-friendly if needed.
  const float abx = vertices[1].x - vertices[0].x;
  const float aby = vertices[1].y - vertices[0].y;
  const float bcx = vertices[2].x - vertices[1].x;
  const float bcy = vertices[2].y - vertices[1].y;
  const float cax = vertices[0].x - vertices[2].x;
  const float cay = vertices[0].y - vertices[2].y;

  // Compute static derivatives, just assume W is uniform across the primitive and that the plane equation remains the
  // same across the quad. (which it is, there is no Z.. yet).
  const float dudx = -aby * static_cast<float>(vertices[2].u) - bcy * static_cast<float>(vertices[0].u) -
                     cay * static_cast<float>(vertices[1].u);
  const float dvdx = -aby * static_cast<float>(vertices[2].v) - bcy * static_cast<float>(vertices[0].v) -
                     cay * static_cast<float>(vertices[1].v);
  const float dudy = +abx * static_cast<float>(vertices[2].u) + bcx * static_cast<float>(vertices[0].u) +
                     cax * static_cast<float>(vertices[1].u);
  const float dvdy = +abx * static_cast<float>(vertices[2].v) + bcx * static_cast<float>(vertices[0].v) +
                     cax * static_cast<float>(vertices[1].v);
  const float area = bcx * cay - bcy * cax;

  // Detect and reject any triangles with 0 size texture area
  const s32 texArea = (vertices[1].u - vertices[0].u) * (vertices[2].v - vertices[0].v) -
                      (vertices[2].u - vertices[0].u) * (vertices[1].v - vertices[0].v);

  // Leverage PGXP to further avoid 3D polygons that just happen to align this way after projection
  const bool is_3d = (vertices[0].w != vertices[1].w || vertices[0].w != vertices[2].w);

  // Shouldn't matter as degenerate primitives will be culled anyways.
  if (area == 0.0f || texArea == 0 || is_3d)
    return;

  // Use floats here as it'll be faster than integer divides.
  const float rcp_area = 1.0f / area;
  const float dudx_area = dudx * rcp_area;
  const float dudy_area = dudy * rcp_area;
  const float dvdx_area = dvdx * rcp_area;
  const float dvdy_area = dvdy * rcp_area;
  const bool neg_dudx = dudx_area < 0.0f;
  const bool neg_dudy = dudy_area < 0.0f;
  const bool neg_dvdx = dvdx_area < 0.0f;
  const bool neg_dvdy = dvdy_area < 0.0f;
  const bool zero_dudx = dudx_area == 0.0f;
  const bool zero_dudy = dudy_area == 0.0f;
  const bool zero_dvdx = dvdx_area == 0.0f;
  const bool zero_dvdy = dvdy_area == 0.0f;

  // If we have negative dU or dV in any direction, increment the U or V to work properly with nearest-neighbor in
  // this impl. If we don't have 1:1 pixel correspondence, this creates a slight "shift" in the sprite, but we
  // guarantee that we don't sample garbage at least. Overall, this is kinda hacky because there can be legitimate,
  // rare cases where 3D meshes hit this scenario, and a single texel offset can pop in, but this is way better than
  // having borked 2D overall.
  //
  // TODO: If perf becomes an issue, we can probably SIMD the 8 comparisons above,
  // create an 8-bit code, and use a LUT to get the offsets.
  // Case 1: U is decreasing in X, but no change in Y.
  // Case 2: U is decreasing in Y, but no change in X.
  // Case 3: V is decreasing in X, but no change in Y.
  // Case 4: V is decreasing in Y, but no change in X.
  if ((neg_dudx && zero_dudy) || (neg_dudy && zero_dudx))
  {
    vertices[0].u++;
    vertices[1].u++;
    vertices[2].u++;
    vertices[3].u++;
  }

  if ((neg_dvdx && zero_dvdy) || (neg_dvdy && zero_dvdx))
  {
    vertices[0].v++;
    vertices[1].v++;
    vertices[2].v++;
    vertices[3].v++;
  }
}

void GPU_HW::ComputePolygonUVLimits(BatchVertex* vertices, u32 num_vertices)
{
  u16 min_u = vertices[0].u, max_u = vertices[0].u, min_v = vertices[0].v, max_v = vertices[0].v;
  for (u32 i = 1; i < num_vertices; i++)
  {
    min_u = std::min<u16>(min_u, vertices[i].u);
    max_u = std::max<u16>(max_u, vertices[i].u);
    min_v = std::min<u16>(min_v, vertices[i].v);
    max_v = std::max<u16>(max_v, vertices[i].v);
  }

  if (min_u != max_u)
    max_u--;
  if (min_v != max_v)
    max_v--;

  for (u32 i = 0; i < num_vertices; i++)
    vertices[i].SetUVLimits(min_u, max_u, min_v, max_v);
}

void GPU_HW::DrawLine(float x0, float y0, u32 col0, float x1, float y1, u32 col1, float depth)
{
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  std::array<BatchVertex, 4> output;
  if (dx == 0.0f && dy == 0.0f)
  {
    // Degenerate, render a point.
    output[0].Set(x0, y0, depth, 1.0f, col0, 0, 0, 0);
    output[1].Set(x0 + 1.0f, y0, depth, 1.0f, col0, 0, 0, 0);
    output[2].Set(x1, y1 + 1.0f, depth, 1.0f, col0, 0, 0, 0);
    output[3].Set(x1 + 1.0f, y1 + 1.0f, depth, 1.0f, col0, 0, 0, 0);
  }
  else
  {
    const float abs_dx = std::fabs(dx);
    const float abs_dy = std::fabs(dy);
    float fill_dx, fill_dy;
    float dxdk, dydk;
    float pad_x0 = 0.0f;
    float pad_x1 = 0.0f;
    float pad_y0 = 0.0f;
    float pad_y1 = 0.0f;

    // Check for vertical or horizontal major lines.
    // When expanding to a rect, do so in the appropriate direction.
    // FIXME: This scheme seems to kinda work, but it seems very hard to find a method
    // that looks perfect on every game.
    // Vagrant Story speech bubbles are a very good test case here!
    if (abs_dx > abs_dy)
    {
      fill_dx = 0.0f;
      fill_dy = 1.0f;
      dxdk = 1.0f;
      dydk = dy / abs_dx;

      if (dx > 0.0f)
      {
        // Right
        pad_x1 = 1.0f;
        pad_y1 = dydk;
      }
      else
      {
        // Left
        pad_x0 = 1.0f;
        pad_y0 = -dydk;
      }
    }
    else
    {
      fill_dx = 1.0f;
      fill_dy = 0.0f;
      dydk = 1.0f;
      dxdk = dx / abs_dy;

      if (dy > 0.0f)
      {
        // Down
        pad_y1 = 1.0f;
        pad_x1 = dxdk;
      }
      else
      {
        // Up
        pad_y0 = 1.0f;
        pad_x0 = -dxdk;
      }
    }

    const float ox0 = x0 + pad_x0;
    const float oy0 = y0 + pad_y0;
    const float ox1 = x1 + pad_x1;
    const float oy1 = y1 + pad_y1;

    output[0].Set(ox0, oy0, depth, 1.0f, col0, 0, 0, 0);
    output[1].Set(ox0 + fill_dx, oy0 + fill_dy, depth, 1.0f, col0, 0, 0, 0);
    output[2].Set(ox1, oy1, depth, 1.0f, col1, 0, 0, 0);
    output[3].Set(ox1 + fill_dx, oy1 + fill_dy, depth, 1.0f, col1, 0, 0, 0);
  }

  AddVertex(output[0]);
  AddVertex(output[1]);
  AddVertex(output[2]);
  AddVertex(output[3]);
  AddVertex(output[2]);
  AddVertex(output[1]);
}

void GPU_HW::LoadVertices()
{
  if (m_GPUSTAT.check_mask_before_draw)
    m_current_depth++;

  const GPURenderCommand rc{m_render_command.bits};
  const u32 texpage = ZeroExtend32(m_draw_mode.mode_reg.bits) | (ZeroExtend32(m_draw_mode.palette_reg) << 16);
  const float depth = GetCurrentNormalizedVertexDepth();

  switch (rc.primitive)
  {
    case GPUPrimitive::Polygon:
    {
      DebugAssert(GetBatchVertexSpace() >= (rc.quad_polygon ? 6u : 3u));

      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;
      const bool pgxp = g_settings.gpu_pgxp_enable;

      const u32 num_vertices = rc.quad_polygon ? 4 : 3;
      std::array<BatchVertex, 4> vertices;
      std::array<std::array<s32, 2>, 4> native_vertex_positions;
      bool valid_w = g_settings.gpu_pgxp_texture_correction;
      for (u32 i = 0; i < num_vertices; i++)
      {
        const u32 color = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
        const u64 maddr_and_pos = m_fifo.Pop();
        const GPUVertexPosition vp{Truncate32(maddr_and_pos)};
        const u16 texcoord = textured ? Truncate16(FifoPop()) : 0;
        const s32 native_x = m_drawing_offset.x + vp.x;
        const s32 native_y = m_drawing_offset.y + vp.y;
        native_vertex_positions[i][0] = native_x;
        native_vertex_positions[i][1] = native_y;
        vertices[i].Set(static_cast<float>(native_x), static_cast<float>(native_y), depth, 1.0f, color, texpage,
                        texcoord, 0xFFFF0000u);

        if (pgxp)
        {
          valid_w &=
            PGXP::GetPreciseVertex(Truncate32(maddr_and_pos >> 32), vp.bits, native_x, native_y, m_drawing_offset.x,
                                   m_drawing_offset.y, &vertices[i].x, &vertices[i].y, &vertices[i].w);
        }
      }
      if (!valid_w)
      {
        for (BatchVertex& v : vertices)
          v.w = 1.0f;
      }

      if (rc.quad_polygon && m_resolution_scale > 1)
        HandleFlippedQuadTextureCoordinates(vertices.data());

      if (m_using_uv_limits && textured)
        ComputePolygonUVLimits(vertices.data(), num_vertices);

      if (!IsDrawingAreaIsValid())
        return;

      // Cull polygons which are too large.
      const auto [min_x_12, max_x_12] = MinMax(native_vertex_positions[1][0], native_vertex_positions[2][0]);
      const auto [min_y_12, max_y_12] = MinMax(native_vertex_positions[1][1], native_vertex_positions[2][1]);
      const s32 min_x = std::min(min_x_12, native_vertex_positions[0][0]);
      const s32 max_x = std::max(max_x_12, native_vertex_positions[0][0]);
      const s32 min_y = std::min(min_y_12, native_vertex_positions[0][1]);
      const s32 max_y = std::max(max_y_12, native_vertex_positions[0][1]);

      if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
      {
        Log_DebugPrintf("Culling too-large polygon: %d,%d %d,%d %d,%d", native_vertex_positions[0][0],
                        native_vertex_positions[0][1], native_vertex_positions[1][0], native_vertex_positions[1][1],
                        native_vertex_positions[2][0], native_vertex_positions[2][1]);
      }
      else
      {
        const u32 clip_left = static_cast<u32>(std::clamp<s32>(min_x, m_drawing_area.left, m_drawing_area.right));
        const u32 clip_right = static_cast<u32>(std::clamp<s32>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
        const u32 clip_top = static_cast<u32>(std::clamp<s32>(min_y, m_drawing_area.top, m_drawing_area.bottom));
        const u32 clip_bottom =
          static_cast<u32>(std::clamp<s32>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

        m_vram_dirty_rect.Include(clip_left, clip_right, clip_top, clip_bottom);
        AddDrawTriangleTicks(native_vertex_positions[0][0], native_vertex_positions[0][1],
                             native_vertex_positions[1][0], native_vertex_positions[1][1],
                             native_vertex_positions[2][0], native_vertex_positions[2][1], rc.shading_enable,
                             rc.texture_enable, rc.transparency_enable);

        std::memcpy(m_batch_current_vertex_ptr, vertices.data(), sizeof(BatchVertex) * 3);
        m_batch_current_vertex_ptr += 3;
      }

      // quads
      if (rc.quad_polygon)
      {
        const s32 min_x_123 = std::min(min_x_12, native_vertex_positions[3][0]);
        const s32 max_x_123 = std::max(max_x_12, native_vertex_positions[3][0]);
        const s32 min_y_123 = std::min(min_y_12, native_vertex_positions[3][1]);
        const s32 max_y_123 = std::max(max_y_12, native_vertex_positions[3][1]);

        // Cull polygons which are too large.
        if ((max_x_123 - min_x_123) >= MAX_PRIMITIVE_WIDTH || (max_y_123 - min_y_123) >= MAX_PRIMITIVE_HEIGHT)
        {
          Log_DebugPrintf("Culling too-large polygon (quad second half): %d,%d %d,%d %d,%d",
                          native_vertex_positions[2][0], native_vertex_positions[2][1], native_vertex_positions[1][0],
                          native_vertex_positions[1][1], native_vertex_positions[0][0], native_vertex_positions[0][1]);
        }
        else
        {
          const u32 clip_left = static_cast<u32>(std::clamp<s32>(min_x_123, m_drawing_area.left, m_drawing_area.right));
          const u32 clip_right =
            static_cast<u32>(std::clamp<s32>(max_x_123, m_drawing_area.left, m_drawing_area.right)) + 1u;
          const u32 clip_top = static_cast<u32>(std::clamp<s32>(min_y_123, m_drawing_area.top, m_drawing_area.bottom));
          const u32 clip_bottom =
            static_cast<u32>(std::clamp<s32>(max_y_123, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

          m_vram_dirty_rect.Include(clip_left, clip_right, clip_top, clip_bottom);
          AddDrawTriangleTicks(native_vertex_positions[2][0], native_vertex_positions[2][1],
                               native_vertex_positions[1][0], native_vertex_positions[1][1],
                               native_vertex_positions[3][0], native_vertex_positions[3][1], rc.shading_enable,
                               rc.texture_enable, rc.transparency_enable);

          AddVertex(vertices[2]);
          AddVertex(vertices[1]);
          AddVertex(vertices[3]);
        }
      }
    }
    break;

    case GPUPrimitive::Rectangle:
    {
      const u32 color = rc.color_for_first_vertex;
      const GPUVertexPosition vp{FifoPop()};
      const s32 pos_x = TruncateGPUVertexPosition(m_drawing_offset.x + vp.x);
      const s32 pos_y = TruncateGPUVertexPosition(m_drawing_offset.y + vp.y);

      const auto [texcoord_x, texcoord_y] = UnpackTexcoord(rc.texture_enable ? Truncate16(FifoPop()) : 0);
      u16 orig_tex_left = ZeroExtend16(texcoord_x);
      u16 orig_tex_top = ZeroExtend16(texcoord_y);
      s32 rectangle_width;
      s32 rectangle_height;
      switch (rc.rectangle_size)
      {
        case GPUDrawRectangleSize::R1x1:
          rectangle_width = 1;
          rectangle_height = 1;
          break;
        case GPUDrawRectangleSize::R8x8:
          rectangle_width = 8;
          rectangle_height = 8;
          break;
        case GPUDrawRectangleSize::R16x16:
          rectangle_width = 16;
          rectangle_height = 16;
          break;
        default:
        {
          const u32 width_and_height = FifoPop();
          rectangle_width = static_cast<s32>(width_and_height & VRAM_WIDTH_MASK);
          rectangle_height = static_cast<s32>((width_and_height >> 16) & VRAM_HEIGHT_MASK);

          if (rectangle_width >= MAX_PRIMITIVE_WIDTH || rectangle_height >= MAX_PRIMITIVE_HEIGHT)
          {
            Log_DebugPrintf("Culling too-large rectangle: %d,%d %dx%d", pos_x, pos_y, rectangle_width,
                            rectangle_height);
            return;
          }
        }
        break;
      }

      // we can split the rectangle up into potentially 8 quads
      DebugAssert(GetBatchVertexSpace() >= MAX_VERTICES_FOR_RECTANGLE);

      if (!IsDrawingAreaIsValid())
        return;

      // Split the rectangle into multiple quads if it's greater than 256x256, as the texture page should repeat.
      u16 tex_top = orig_tex_top;
      for (s32 y_offset = 0; y_offset < rectangle_height;)
      {
        const s32 quad_height = std::min<s32>(rectangle_height - y_offset, TEXTURE_PAGE_WIDTH - tex_top);
        const float quad_start_y = static_cast<float>(pos_y + y_offset);
        const float quad_end_y = quad_start_y + static_cast<float>(quad_height);
        const u16 tex_bottom = tex_top + static_cast<u16>(quad_height);

        u16 tex_left = orig_tex_left;
        for (s32 x_offset = 0; x_offset < rectangle_width;)
        {
          const s32 quad_width = std::min<s32>(rectangle_width - x_offset, TEXTURE_PAGE_HEIGHT - tex_left);
          const float quad_start_x = static_cast<float>(pos_x + x_offset);
          const float quad_end_x = quad_start_x + static_cast<float>(quad_width);
          const u16 tex_right = tex_left + static_cast<u16>(quad_width);
          const u32 uv_limits = BatchVertex::PackUVLimits(tex_left, tex_right - 1, tex_top, tex_bottom - 1);

          AddNewVertex(quad_start_x, quad_start_y, depth, 1.0f, color, texpage, tex_left, tex_top, uv_limits);
          AddNewVertex(quad_end_x, quad_start_y, depth, 1.0f, color, texpage, tex_right, tex_top, uv_limits);
          AddNewVertex(quad_start_x, quad_end_y, depth, 1.0f, color, texpage, tex_left, tex_bottom, uv_limits);

          AddNewVertex(quad_start_x, quad_end_y, depth, 1.0f, color, texpage, tex_left, tex_bottom, uv_limits);
          AddNewVertex(quad_end_x, quad_start_y, depth, 1.0f, color, texpage, tex_right, tex_top, uv_limits);
          AddNewVertex(quad_end_x, quad_end_y, depth, 1.0f, color, texpage, tex_right, tex_bottom, uv_limits);

          x_offset += quad_width;
          tex_left = 0;
        }

        y_offset += quad_height;
        tex_top = 0;
      }

      const u32 clip_left = static_cast<u32>(std::clamp<s32>(pos_x, m_drawing_area.left, m_drawing_area.right));
      const u32 clip_right =
        static_cast<u32>(std::clamp<s32>(pos_x + rectangle_width, m_drawing_area.left, m_drawing_area.right)) + 1u;
      const u32 clip_top = static_cast<u32>(std::clamp<s32>(pos_y, m_drawing_area.top, m_drawing_area.bottom));
      const u32 clip_bottom =
        static_cast<u32>(std::clamp<s32>(pos_y + rectangle_height, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

      m_vram_dirty_rect.Include(clip_left, clip_right, clip_top, clip_bottom);
      AddDrawRectangleTicks(clip_right - clip_left, clip_bottom - clip_top, rc.texture_enable, rc.transparency_enable);
    }
    break;

    case GPUPrimitive::Line:
    {
      if (!rc.polyline)
      {
        DebugAssert(GetBatchVertexSpace() >= 2);

        u32 start_color, end_color;
        GPUVertexPosition start_pos, end_pos;
        if (rc.shading_enable)
        {
          start_color = rc.color_for_first_vertex;
          start_pos.bits = FifoPop();
          end_color = FifoPop() & UINT32_C(0x00FFFFFF);
          end_pos.bits = FifoPop();
        }
        else
        {
          start_color = end_color = rc.color_for_first_vertex;
          start_pos.bits = FifoPop();
          end_pos.bits = FifoPop();
        }

        if (!IsDrawingAreaIsValid())
          return;

        s32 start_x = start_pos.x + m_drawing_offset.x;
        s32 start_y = start_pos.y + m_drawing_offset.y;
        s32 end_x = end_pos.x + m_drawing_offset.x;
        s32 end_y = end_pos.y + m_drawing_offset.y;
        const auto [min_x, max_x] = MinMax(start_x, end_x);
        const auto [min_y, max_y] = MinMax(start_y, end_y);
        if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
        {
          Log_DebugPrintf("Culling too-large line: %d,%d - %d,%d", start_x, start_y, end_x, end_y);
          return;
        }

        const u32 clip_left = static_cast<u32>(std::clamp<s32>(min_x, m_drawing_area.left, m_drawing_area.right));
        const u32 clip_right = static_cast<u32>(std::clamp<s32>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
        const u32 clip_top = static_cast<u32>(std::clamp<s32>(min_y, m_drawing_area.top, m_drawing_area.bottom));
        const u32 clip_bottom =
          static_cast<u32>(std::clamp<s32>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

        m_vram_dirty_rect.Include(clip_left, clip_right, clip_top, clip_bottom);
        AddDrawLineTicks(clip_right - clip_left, clip_bottom - clip_top, rc.shading_enable);

        // TODO: Should we do a PGXP lookup here? Most lines are 2D.
        DrawLine(static_cast<float>(start_x), static_cast<float>(start_y), start_color, static_cast<float>(end_x),
                 static_cast<float>(end_y), end_color, depth);
      }
      else
      {
        // Multiply by two because we don't use line strips.
        const u32 num_vertices = GetPolyLineVertexCount();
        DebugAssert(GetBatchVertexSpace() >= (num_vertices * 2));

        if (!IsDrawingAreaIsValid())
          return;

        const bool shaded = rc.shading_enable;

        u32 buffer_pos = 0;
        const GPUVertexPosition start_vp{m_blit_buffer[buffer_pos++]};
        s32 start_x = start_vp.x + m_drawing_offset.x;
        s32 start_y = start_vp.y + m_drawing_offset.y;
        u32 start_color = rc.color_for_first_vertex;

        for (u32 i = 1; i < num_vertices; i++)
        {
          const u32 end_color = shaded ? (m_blit_buffer[buffer_pos++] & UINT32_C(0x00FFFFFF)) : start_color;
          const GPUVertexPosition vp{m_blit_buffer[buffer_pos++]};
          const s32 end_x = m_drawing_offset.x + vp.x;
          const s32 end_y = m_drawing_offset.y + vp.y;

          const auto [min_x, max_x] = MinMax(start_x, end_x);
          const auto [min_y, max_y] = MinMax(start_y, end_y);
          if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
          {
            Log_DebugPrintf("Culling too-large line: %d,%d - %d,%d", start_x, start_y, end_x, end_y);
          }
          else
          {
            const u32 clip_left = static_cast<u32>(std::clamp<s32>(min_x, m_drawing_area.left, m_drawing_area.right));
            const u32 clip_right =
              static_cast<u32>(std::clamp<s32>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
            const u32 clip_top = static_cast<u32>(std::clamp<s32>(min_y, m_drawing_area.top, m_drawing_area.bottom));
            const u32 clip_bottom =
              static_cast<u32>(std::clamp<s32>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

            m_vram_dirty_rect.Include(clip_left, clip_right, clip_top, clip_bottom);
            AddDrawLineTicks(clip_right - clip_left, clip_bottom - clip_top, rc.shading_enable);

            // TODO: Should we do a PGXP lookup here? Most lines are 2D.
            DrawLine(static_cast<float>(start_x), static_cast<float>(start_y), start_color, static_cast<float>(end_x),
                     static_cast<float>(end_y), end_color, depth);
          }

          start_x = end_x;
          start_y = end_y;
          start_color = end_color;
        }
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

void GPU_HW::CalcScissorRect(int* left, int* top, int* right, int* bottom)
{
  *left = m_drawing_area.left * m_resolution_scale;
  *right = std::max<u32>((m_drawing_area.right + 1) * m_resolution_scale, *left + 1);
  *top = m_drawing_area.top * m_resolution_scale;
  *bottom = std::max<u32>((m_drawing_area.bottom + 1) * m_resolution_scale, *top + 1);
}

GPU_HW::VRAMFillUBOData GPU_HW::GetVRAMFillUBOData(u32 x, u32 y, u32 width, u32 height, u32 color) const
{
  // drop precision unless true colour is enabled
  if (!m_true_color)
    color = RGBA5551ToRGBA8888(RGBA8888ToRGBA5551(color));

  VRAMFillUBOData uniforms;
  std::tie(uniforms.u_fill_color[0], uniforms.u_fill_color[1], uniforms.u_fill_color[2], uniforms.u_fill_color[3]) =
    RGBA8ToFloat(color);
  uniforms.u_interlaced_displayed_field = GetActiveLineLSB();
  return uniforms;
}

Common::Rectangle<u32> GPU_HW::GetVRAMTransferBounds(u32 x, u32 y, u32 width, u32 height) const
{
  Common::Rectangle<u32> out_rc = Common::Rectangle<u32>::FromExtents(x % VRAM_WIDTH, y % VRAM_HEIGHT, width, height);
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

GPU_HW::VRAMWriteUBOData GPU_HW::GetVRAMWriteUBOData(u32 x, u32 y, u32 width, u32 height, u32 buffer_offset,
                                                     bool set_mask, bool check_mask) const
{
  const VRAMWriteUBOData uniforms = {
    (x % VRAM_WIDTH), (y % VRAM_HEIGHT), ((x + width) % VRAM_WIDTH),  ((y + height) % VRAM_HEIGHT),     width,
    height,           buffer_offset,     (set_mask) ? 0x8000u : 0x00, GetCurrentNormalizedVertexDepth()};
  return uniforms;
}

bool GPU_HW::UseVRAMCopyShader(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) const
{
  // masking enabled, oversized, or overlapping
  return (m_GPUSTAT.IsMaskingEnabled() || ((src_x % VRAM_WIDTH) + width) > VRAM_WIDTH ||
          ((src_y % VRAM_HEIGHT) + height) > VRAM_HEIGHT || ((dst_x % VRAM_WIDTH) + width) > VRAM_WIDTH ||
          ((dst_y % VRAM_HEIGHT) + height) > VRAM_HEIGHT ||
          Common::Rectangle<u32>::FromExtents(src_x, src_y, width, height)
            .Intersects(Common::Rectangle<u32>::FromExtents(dst_x, dst_y, width, height)));
}

GPU_HW::VRAMCopyUBOData GPU_HW::GetVRAMCopyUBOData(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width,
                                                   u32 height) const
{
  const VRAMCopyUBOData uniforms = {(src_x % VRAM_WIDTH) * m_resolution_scale,
                                    (src_y % VRAM_HEIGHT) * m_resolution_scale,
                                    (dst_x % VRAM_WIDTH) * m_resolution_scale,
                                    (dst_y % VRAM_HEIGHT) * m_resolution_scale,
                                    ((dst_x + width) % VRAM_WIDTH) * m_resolution_scale,
                                    ((dst_y + height) % VRAM_HEIGHT) * m_resolution_scale,
                                    width * m_resolution_scale,
                                    height * m_resolution_scale,
                                    m_GPUSTAT.set_mask_while_drawing ? 1u : 0u,
                                    GetCurrentNormalizedVertexDepth()};

  return uniforms;
}

void GPU_HW::IncludeVRAMDityRectangle(const Common::Rectangle<u32>& rect)
{
  m_vram_dirty_rect.Include(rect);

  // the vram area can include the texture page, but the game can leave it as-is. in this case, set it as dirty so the
  // shadow texture is updated
  if (!m_draw_mode.IsTexturePageChanged() &&
      (m_draw_mode.mode_reg.GetTexturePageRectangle().Intersects(rect) ||
       (m_draw_mode.mode_reg.IsUsingPalette() && m_draw_mode.GetTexturePaletteRectangle().Intersects(rect))))
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

void GPU_HW::EnsureVertexBufferSpaceForCurrentCommand()
{
  u32 required_vertices;
  switch (m_render_command.primitive)
  {
    case GPUPrimitive::Polygon:
      required_vertices = m_render_command.quad_polygon ? 6 : 3;
      break;
    case GPUPrimitive::Rectangle:
      required_vertices = MAX_VERTICES_FOR_RECTANGLE;
      break;
    case GPUPrimitive::Line:
    default:
      required_vertices = m_render_command.polyline ? (GetPolyLineVertexCount() * 6u) : 6u;
      break;
  }

  // can we fit these vertices in the current depth buffer range?
  if ((m_current_depth + required_vertices) > MAX_BATCH_VERTEX_COUNTER_IDS)
  {
    // implies FlushRender()
    ResetBatchVertexDepth();
  }
  else if (m_batch_current_vertex_ptr)
  {
    if (GetBatchVertexSpace() >= required_vertices)
      return;

    FlushRender();
  }

  MapBatchVertexPointer(required_vertices);
}

void GPU_HW::ResetBatchVertexDepth()
{
  Log_PerfPrint("Resetting batch vertex depth");
  FlushRender();
  UpdateDepthBufferFromMaskBit();

  m_current_depth = 1;
}

void GPU_HW::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  IncludeVRAMDityRectangle(
    Common::Rectangle<u32>::FromExtents(x, y, width, height).Clamped(0, 0, VRAM_WIDTH, VRAM_HEIGHT));
}

void GPU_HW::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask)
{
  DebugAssert((x + width) <= VRAM_WIDTH && (y + height) <= VRAM_HEIGHT);
  IncludeVRAMDityRectangle(Common::Rectangle<u32>::FromExtents(x, y, width, height));

  if (check_mask)
  {
    // set new vertex counter since we want this to take into consideration previous masked pixels
    m_current_depth++;
  }
}

void GPU_HW::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  IncludeVRAMDityRectangle(
    Common::Rectangle<u32>::FromExtents(dst_x, dst_y, width, height).Clamped(0, 0, VRAM_WIDTH, VRAM_HEIGHT));

  if (m_GPUSTAT.check_mask_before_draw)
  {
    // set new vertex counter since we want this to take into consideration previous masked pixels
    m_current_depth++;
  }
}

void GPU_HW::DispatchRenderCommand()
{
  const GPURenderCommand rc{m_render_command.bits};

  GPUTextureMode texture_mode;
  if (rc.IsTexturingEnabled())
  {
    // texture page changed - check that the new page doesn't intersect the drawing area
    if (m_draw_mode.IsTexturePageChanged())
    {
      m_draw_mode.ClearTexturePageChangedFlag();
      if (m_vram_dirty_rect.Valid() && (m_draw_mode.mode_reg.GetTexturePageRectangle().Intersects(m_vram_dirty_rect) ||
                                        (m_draw_mode.mode_reg.IsUsingPalette() &&
                                         m_draw_mode.GetTexturePaletteRectangle().Intersects(m_vram_dirty_rect))))
      {
        // Log_DevPrintf("Invalidating VRAM read cache due to drawing area overlap");
        if (!IsFlushed())
          FlushRender();

        UpdateVRAMReadTexture();
      }
    }

    texture_mode = m_draw_mode.mode_reg.texture_mode;
    if (rc.raw_texture_enable)
    {
      texture_mode =
        static_cast<GPUTextureMode>(static_cast<u8>(texture_mode) | static_cast<u8>(GPUTextureMode::RawTextureBit));
    }
  }
  else
  {
    texture_mode = GPUTextureMode::Disabled;
  }

  // has any state changed which requires a new batch?
  const GPUTransparencyMode transparency_mode =
    rc.transparency_enable ? m_draw_mode.mode_reg.transparency_mode : GPUTransparencyMode::Disabled;
  const bool dithering_enable = (!m_true_color && rc.IsDitheringEnabled()) ? m_GPUSTAT.dither_enable : false;
  if (m_batch.texture_mode != texture_mode || m_batch.transparency_mode != transparency_mode ||
      dithering_enable != m_batch.dithering)
  {
    FlushRender();
  }

  EnsureVertexBufferSpaceForCurrentCommand();

  // transparency mode change
  if (m_batch.transparency_mode != transparency_mode && transparency_mode != GPUTransparencyMode::Disabled)
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
    m_batch_ubo_data.u_set_mask_while_drawing = BoolToUInt32(m_batch.set_mask_while_drawing);
    m_batch_ubo_dirty = true;
  }

  m_batch.interlacing = IsInterlacedRenderingEnabled();
  if (m_batch.interlacing)
  {
    const u32 displayed_field = GetActiveLineLSB();
    m_batch_ubo_dirty |= (m_batch_ubo_data.u_interlaced_displayed_field != displayed_field);
    m_batch_ubo_data.u_interlaced_displayed_field = displayed_field;
  }

  // update state
  m_batch.texture_mode = texture_mode;
  m_batch.transparency_mode = transparency_mode;
  m_batch.dithering = dithering_enable;

  if (m_draw_mode.IsTextureWindowChanged())
  {
    m_draw_mode.ClearTextureWindowChangedFlag();

    m_batch_ubo_data.u_texture_window_and[0] = ZeroExtend32(m_draw_mode.texture_window.and_x);
    m_batch_ubo_data.u_texture_window_and[1] = ZeroExtend32(m_draw_mode.texture_window.and_y);
    m_batch_ubo_data.u_texture_window_or[0] = ZeroExtend32(m_draw_mode.texture_window.or_x);
    m_batch_ubo_data.u_texture_window_or[1] = ZeroExtend32(m_draw_mode.texture_window.or_y);
    m_batch_ubo_dirty = true;
  }

  LoadVertices();
}

void GPU_HW::FlushRender()
{
  if (!m_batch_current_vertex_ptr)
    return;

  const u32 vertex_count = GetBatchVertexCount();
  UnmapBatchVertexPointer(vertex_count);

  if (vertex_count == 0)
    return;

  if (m_drawing_area_changed)
  {
    m_drawing_area_changed = false;
    SetScissorFromDrawingArea();
  }

  if (m_batch_ubo_dirty)
  {
    UploadUniformBuffer(&m_batch_ubo_data, sizeof(m_batch_ubo_data));
    m_batch_ubo_dirty = false;
  }

  if (m_batch.NeedsTwoPassRendering())
  {
    m_renderer_stats.num_batches += 2;
    DrawBatchVertices(BatchRenderMode::OnlyOpaque, m_batch_base_vertex, vertex_count);
    DrawBatchVertices(BatchRenderMode::OnlyTransparent, m_batch_base_vertex, vertex_count);
  }
  else
  {
    m_renderer_stats.num_batches++;
    DrawBatchVertices(m_batch.GetRenderMode(), m_batch_base_vertex, vertex_count);
  }
}

void GPU_HW::DrawRendererStats(bool is_idle_frame)
{
  if (!is_idle_frame)
  {
    m_last_renderer_stats = m_renderer_stats;
    m_renderer_stats = {};
  }

#ifdef WITH_IMGUI
  if (ImGui::CollapsingHeader("Renderer Statistics", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static const ImVec4 active_color{1.0f, 1.0f, 1.0f, 1.0f};
    static const ImVec4 inactive_color{0.4f, 0.4f, 0.4f, 1.0f};
    const auto& stats = m_last_renderer_stats;

    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, 200.0f * ImGui::GetIO().DisplayFramebufferScale.x);

    ImGui::TextUnformatted("Resolution Scale:");
    ImGui::NextColumn();
    ImGui::Text("%u (VRAM %ux%u)", m_resolution_scale, VRAM_WIDTH * m_resolution_scale,
                VRAM_HEIGHT * m_resolution_scale);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Effective Display Resolution:");
    ImGui::NextColumn();
    ImGui::Text("%ux%u", m_crtc_state.display_vram_width * m_resolution_scale,
                m_crtc_state.display_vram_height * m_resolution_scale);
    ImGui::NextColumn();

    ImGui::TextUnformatted("True Color:");
    ImGui::NextColumn();
    ImGui::TextColored(m_true_color ? active_color : inactive_color, m_true_color ? "Enabled" : "Disabled");
    ImGui::NextColumn();

    ImGui::TextUnformatted("Scaled Dithering:");
    ImGui::NextColumn();
    ImGui::TextColored(m_scaled_dithering ? active_color : inactive_color, m_scaled_dithering ? "Enabled" : "Disabled");
    ImGui::NextColumn();

    ImGui::TextUnformatted("Texture Filtering:");
    ImGui::NextColumn();
    ImGui::TextColored((m_texture_filtering != GPUTextureFilter::Nearest) ? active_color : inactive_color, "%s",
                       Settings::GetTextureFilterDisplayName(m_texture_filtering));
    ImGui::NextColumn();

    ImGui::TextUnformatted("PGXP:");
    ImGui::NextColumn();
    ImGui::TextColored(g_settings.gpu_pgxp_enable ? active_color : inactive_color, "Geom");
    ImGui::SameLine();
    ImGui::TextColored((g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_culling) ? active_color : inactive_color,
                       "Cull");
    ImGui::SameLine();
    ImGui::TextColored(
      (g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_texture_correction) ? active_color : inactive_color, "Tex");
    ImGui::SameLine();
    ImGui::TextColored((g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_vertex_cache) ? active_color : inactive_color,
                       "Cache");
    ImGui::NextColumn();

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
#endif
}
