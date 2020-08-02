#include "gpu_sw.h"
#include "common/assert.h"
#include "common/log.h"
#include "host_display.h"
#include "system.h"
#include <algorithm>
Log_SetChannel(GPU_SW);

GPU_SW::GPU_SW()
{
  m_vram.fill(0);
  m_vram_ptr = m_vram.data();
}

GPU_SW::~GPU_SW()
{
  if (m_host_display)
    m_host_display->ClearDisplayTexture();
}

bool GPU_SW::IsHardwareRenderer() const
{
  return false;
}

bool GPU_SW::Initialize(HostDisplay* host_display)
{
  if (!GPU::Initialize(host_display))
    return false;

  m_display_texture = host_display->CreateTexture(VRAM_WIDTH, VRAM_HEIGHT, nullptr, 0, true);
  if (!m_display_texture)
    return false;

  return true;
}

void GPU_SW::Reset()
{
  GPU::Reset();

  m_vram.fill(0);
}

void GPU_SW::CopyOut15Bit(u32 src_x, u32 src_y, u32* dst_ptr, u32 dst_stride, u32 width, u32 height, bool interlaced,
                          bool interleaved)
{
  const u8 interlaced_shift = BoolToUInt8(interlaced);
  const u8 interleaved_shift = BoolToUInt8(interleaved);

  // Fast path when not wrapping around.
  if ((src_x + width) <= VRAM_WIDTH && (src_y + height) <= VRAM_HEIGHT)
  {
    dst_stride <<= interlaced_shift;
    height >>= interlaced_shift;

    const u16* src_ptr = &m_vram[src_y * VRAM_WIDTH + src_x];
    const u32 src_stride = VRAM_WIDTH << interleaved_shift;
    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = src_ptr;
      u32* dst_row_ptr = dst_ptr;
      for (u32 col = 0; col < width; col++)
        *(dst_row_ptr++) = RGBA5551ToRGBA8888(*(src_row_ptr++));

      src_ptr += src_stride;
      dst_ptr += dst_stride;
    }
  }
  else
  {
    dst_stride <<= interlaced_shift;
    height >>= interlaced_shift;

    const u32 end_x = src_x + width;
    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &m_vram[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
      u32* dst_row_ptr = dst_ptr;

      for (u32 col = src_x; col < end_x; col++)
        *(dst_row_ptr++) = RGBA5551ToRGBA8888(src_row_ptr[col % VRAM_WIDTH]);

      src_y += (1 << interleaved_shift);
      dst_ptr += dst_stride;
    }
  }
}

void GPU_SW::CopyOut24Bit(u32 src_x, u32 src_y, u32* dst_ptr, u32 dst_stride, u32 width, u32 height, bool interlaced,
                          bool interleaved)
{
  const u8 interlaced_shift = BoolToUInt8(interlaced);
  const u8 interleaved_shift = BoolToUInt8(interleaved);

  if ((src_x + width) <= VRAM_WIDTH && (src_y + height) <= VRAM_HEIGHT)
  {
    dst_stride <<= interlaced_shift;
    height >>= interlaced_shift;

    const u8* src_ptr = reinterpret_cast<const u8*>(&m_vram[src_y * VRAM_WIDTH + src_x]);
    const u32 src_stride = (VRAM_WIDTH << interleaved_shift) * sizeof(u16);
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
    dst_stride <<= interlaced_shift;
    height >>= interlaced_shift;

    const u32 end_x = src_x + width;
    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &m_vram[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
      u32* dst_row_ptr = dst_ptr;

      for (u32 col = 0; col < width; col++)
      {
        const u32 offset = (src_x + ((col * 3) / 2));
        const u16 s0 = src_row_ptr[offset % VRAM_WIDTH];
        const u16 s1 = src_row_ptr[(offset + 1) % VRAM_WIDTH];
        const u8 shift = static_cast<u8>(col & 1u) * 8;
        *(dst_row_ptr++) = (((ZeroExtend32(s1) << 16) | ZeroExtend32(s0)) >> shift) | 0xFF000000u;
      }

      src_y += (1 << interleaved_shift);
      dst_ptr += dst_stride;
    }
  }
}

void GPU_SW::ClearDisplay()
{
  std::memset(m_display_texture_buffer.data(), 0, sizeof(u32) * m_display_texture_buffer.size());
}

void GPU_SW::UpdateDisplay()
{
  // fill display texture
  m_display_texture_buffer.resize(VRAM_WIDTH * VRAM_HEIGHT);

  if (!g_settings.debugging.show_vram)
  {
    if (IsDisplayDisabled())
    {
      m_host_display->ClearDisplayTexture();
      return;
    }

    const u32 vram_offset_x = m_crtc_state.display_vram_left;
    const u32 vram_offset_y = m_crtc_state.display_vram_top;
    const u32 display_width = m_crtc_state.display_vram_width;
    const u32 display_height = m_crtc_state.display_vram_height;
    const u32 texture_offset_x = m_crtc_state.display_vram_left - m_crtc_state.regs.X;
    if (IsInterlacedDisplayEnabled())
    {
      const u32 field = GetInterlacedDisplayField();
      if (m_GPUSTAT.display_area_color_depth_24)
      {
        CopyOut24Bit(m_crtc_state.regs.X, vram_offset_y + field, m_display_texture_buffer.data() + field * VRAM_WIDTH,
                     VRAM_WIDTH, display_width + texture_offset_x, display_height, true, m_GPUSTAT.vertical_resolution);
      }
      else
      {
        CopyOut15Bit(m_crtc_state.regs.X, vram_offset_y + field, m_display_texture_buffer.data() + field * VRAM_WIDTH,
                     VRAM_WIDTH, display_width + texture_offset_x, display_height, true, m_GPUSTAT.vertical_resolution);
      }
    }
    else
    {
      if (m_GPUSTAT.display_area_color_depth_24)
      {
        CopyOut24Bit(m_crtc_state.regs.X, vram_offset_y, m_display_texture_buffer.data(), VRAM_WIDTH,
                     display_width + texture_offset_x, display_height, false, false);
      }
      else
      {
        CopyOut15Bit(m_crtc_state.regs.X, vram_offset_y, m_display_texture_buffer.data(), VRAM_WIDTH,
                     display_width + texture_offset_x, display_height, false, false);
      }
    }

    m_host_display->UpdateTexture(m_display_texture.get(), 0, 0, display_width, display_height,
                                  m_display_texture_buffer.data(), VRAM_WIDTH * sizeof(u32));
    m_host_display->SetDisplayTexture(m_display_texture->GetHandle(), VRAM_WIDTH, VRAM_HEIGHT, texture_offset_x, 0,
                                      display_width, display_height);
    m_host_display->SetDisplayParameters(m_crtc_state.display_width, m_crtc_state.display_height,
                                         m_crtc_state.display_origin_left, m_crtc_state.display_origin_top,
                                         m_crtc_state.display_vram_width, m_crtc_state.display_vram_height,
                                         m_crtc_state.display_aspect_ratio);
  }
  else
  {
    CopyOut15Bit(0, 0, m_display_texture_buffer.data(), VRAM_WIDTH, VRAM_WIDTH, VRAM_HEIGHT, false, false);
    m_host_display->UpdateTexture(m_display_texture.get(), 0, 0, VRAM_WIDTH, VRAM_HEIGHT,
                                  m_display_texture_buffer.data(), VRAM_WIDTH * sizeof(u32));
    m_host_display->SetDisplayTexture(m_display_texture->GetHandle(), VRAM_WIDTH, VRAM_HEIGHT, 0, 0, VRAM_WIDTH,
                                      VRAM_HEIGHT);
    m_host_display->SetDisplayParameters(VRAM_WIDTH, VRAM_HEIGHT, 0, 0, VRAM_WIDTH, VRAM_HEIGHT,
                                         static_cast<float>(VRAM_WIDTH) / static_cast<float>(VRAM_HEIGHT));
  }
}

void GPU_SW::DispatchRenderCommand()
{
  const RenderCommand rc{m_render_command.bits};
  const bool dithering_enable = rc.IsDitheringEnabled() && m_GPUSTAT.dither_enable;

  switch (rc.primitive)
  {
    case Primitive::Polygon:
    {
      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;

      const u32 num_vertices = rc.quad_polygon ? 4 : 3;
      std::array<SWVertex, 4> vertices;
      for (u32 i = 0; i < num_vertices; i++)
      {
        SWVertex& vert = vertices[i];
        const u32 color_rgb = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
        vert.color_r = Truncate8(color_rgb);
        vert.color_g = Truncate8(color_rgb >> 8);
        vert.color_b = Truncate8(color_rgb >> 16);

        const VertexPosition vp{FifoPop()};
        vert.x = vp.x;
        vert.y = vp.y;

        if (textured)
        {
          std::tie(vert.texcoord_x, vert.texcoord_y) = UnpackTexcoord(Truncate16(FifoPop()));
        }
        else
        {
          vert.texcoord_x = 0;
          vert.texcoord_y = 0;
        }
      }

      if (!IsDrawingAreaIsValid())
        return;

      const DrawTriangleFunction DrawFunction = GetDrawTriangleFunction(
        rc.shading_enable, rc.texture_enable, rc.raw_texture_enable, rc.transparency_enable, dithering_enable);

      (this->*DrawFunction)(&vertices[0], &vertices[1], &vertices[2]);
      if (num_vertices > 3)
        (this->*DrawFunction)(&vertices[2], &vertices[1], &vertices[3]);
    }
    break;

    case Primitive::Rectangle:
    {
      const auto [r, g, b] = UnpackColorRGB24(rc.color_for_first_vertex);
      const VertexPosition vp{FifoPop()};
      const u32 texcoord_and_palette = rc.texture_enable ? FifoPop() : 0;
      const auto [texcoord_x, texcoord_y] = UnpackTexcoord(Truncate16(texcoord_and_palette));

      s32 width;
      s32 height;
      switch (rc.rectangle_size)
      {
        case DrawRectangleSize::R1x1:
          width = 1;
          height = 1;
          break;
        case DrawRectangleSize::R8x8:
          width = 8;
          height = 8;
          break;
        case DrawRectangleSize::R16x16:
          width = 16;
          height = 16;
          break;
        default:
        {
          const u32 width_and_height = FifoPop();
          width = static_cast<s32>(width_and_height & VRAM_WIDTH_MASK);
          height = static_cast<s32>((width_and_height >> 16) & VRAM_HEIGHT_MASK);

          if (width >= MAX_PRIMITIVE_WIDTH || height >= MAX_PRIMITIVE_HEIGHT)
          {
            Log_DebugPrintf("Culling too-large rectangle: %d,%d %dx%d", vp.x.GetValue(), vp.y.GetValue(), width,
                            height);
            return;
          }
        }
        break;
      }

      if (!IsDrawingAreaIsValid())
        return;

      const DrawRectangleFunction DrawFunction =
        GetDrawRectangleFunction(rc.texture_enable, rc.raw_texture_enable, rc.transparency_enable);

      (this->*DrawFunction)(vp.x, vp.y, width, height, r, g, b, texcoord_x, texcoord_y);
    }
    break;

    case Primitive::Line:
    {
      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;

      const DrawLineFunction DrawFunction = GetDrawLineFunction(shaded, rc.transparency_enable, dithering_enable);

      std::array<SWVertex, 2> vertices = {};
      u32 buffer_pos = 0;

      // first vertex
      SWVertex* p0 = &vertices[0];
      SWVertex* p1 = &vertices[1];
      p0->SetPosition(VertexPosition{rc.polyline ? m_blit_buffer[buffer_pos++] : Truncate32(FifoPop())});
      p0->SetColorRGB24(first_color);

      // remaining vertices in line strip
      const u32 num_vertices = rc.polyline ? GetPolyLineVertexCount() : 2;
      for (u32 i = 1; i < num_vertices; i++)
      {
        if (rc.polyline)
        {
          p1->SetColorRGB24(shaded ? (m_blit_buffer[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color);
          p1->SetPosition(VertexPosition{m_blit_buffer[buffer_pos++]});
        }
        else
        {
          p1->SetColorRGB24(shaded ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color);
          p1->SetPosition(VertexPosition{Truncate32(FifoPop())});
        }

        // down here because of the FIFO pops
        if (IsDrawingAreaIsValid())
          (this->*DrawFunction)(p0, p1);

        // swap p0/p1 so that the last vertex is used as the first for the next line
        std::swap(p0, p1);
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

enum : u32
{
  COORD_FRAC_BITS = 32,
  COLOR_FRAC_BITS = 12
};

using FixedPointCoord = u64;

constexpr FixedPointCoord IntToFixedCoord(s32 x)
{
  return (ZeroExtend64(static_cast<u32>(x)) << COORD_FRAC_BITS) | (ZeroExtend64(1u) << (COORD_FRAC_BITS - 1));
}

using FixedPointColor = u32;

constexpr FixedPointColor IntToFixedColor(u8 r)
{
  return ZeroExtend32(r) << COLOR_FRAC_BITS | (1u << (COLOR_FRAC_BITS - 1));
}

constexpr u8 FixedColorToInt(FixedPointColor r)
{
  return Truncate8(r >> 12);
}

bool GPU_SW::IsClockwiseWinding(const SWVertex* v0, const SWVertex* v1, const SWVertex* v2)
{
  const s32 abx = v1->x - v0->x;
  const s32 aby = v1->y - v0->y;
  const s32 acx = v2->x - v0->x;
  const s32 acy = v2->y - v0->y;
  return ((abx * acy) - (aby * acx) < 0);
}

static constexpr bool IsTopLeftEdge(s32 ex, s32 ey)
{
  return (ey < 0 || (ey == 0 && ex < 0));
}

static constexpr u8 Interpolate(u8 v0, u8 v1, u8 v2, s32 w0, s32 w1, s32 w2, s32 ws, s32 half_ws)
{
  const s32 v = w0 * static_cast<s32>(static_cast<u32>(v0)) + w1 * static_cast<s32>(static_cast<u32>(v1)) +
                w2 * static_cast<s32>(static_cast<u32>(v2));
  const s32 vd = (v + half_ws) / ws;
  return (vd < 0) ? 0 : ((vd > 0xFF) ? 0xFF : static_cast<u8>(vd));
}

template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
         bool dithering_enable>
void GPU_SW::DrawTriangle(const SWVertex* v0, const SWVertex* v1, const SWVertex* v2)
{
#define orient2d(ax, ay, bx, by, cx, cy) ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax))

  // ensure the vertices follow a counter-clockwise order
  if (IsClockwiseWinding(v0, v1, v2))
    std::swap(v1, v2);

  const s32 px0 = v0->x + m_drawing_offset.x;
  const s32 py0 = v0->y + m_drawing_offset.y;
  const s32 px1 = v1->x + m_drawing_offset.x;
  const s32 py1 = v1->y + m_drawing_offset.y;
  const s32 px2 = v2->x + m_drawing_offset.x;
  const s32 py2 = v2->y + m_drawing_offset.y;

  // Barycentric coordinates at minX/minY corner
  const s32 ws = orient2d(px0, py0, px1, py1, px2, py2);
  const s32 half_ws = std::max<s32>((ws / 2) - 1, 0);
  if (ws == 0)
    return;

  // compute bounding box of triangle
  s32 min_x = std::min(px0, std::min(px1, px2));
  s32 max_x = std::max(px0, std::max(px1, px2));
  s32 min_y = std::min(py0, std::min(py1, py2));
  s32 max_y = std::max(py0, std::max(py1, py2));

  // reject triangles which cover the whole vram area
  if (static_cast<u32>(max_x - min_x) > MAX_PRIMITIVE_WIDTH || static_cast<u32>(max_y - min_y) > MAX_PRIMITIVE_HEIGHT)
    return;

  // clip to drawing area
  min_x = std::clamp(min_x, static_cast<s32>(m_drawing_area.left), static_cast<s32>(m_drawing_area.right));
  max_x = std::clamp(max_x, static_cast<s32>(m_drawing_area.left), static_cast<s32>(m_drawing_area.right));
  min_y = std::clamp(min_y, static_cast<s32>(m_drawing_area.top), static_cast<s32>(m_drawing_area.bottom));
  max_y = std::clamp(max_y, static_cast<s32>(m_drawing_area.top), static_cast<s32>(m_drawing_area.bottom));
  AddDrawTriangleTicks(max_x - min_x + 1, max_y - min_y + 1, shading_enable, texture_enable, transparency_enable);

  // compute per-pixel increments
  const s32 a01 = py0 - py1, b01 = px1 - px0;
  const s32 a12 = py1 - py2, b12 = px2 - px1;
  const s32 a20 = py2 - py0, b20 = px0 - px2;

  // top-left edge rule
  const s32 w0_bias = 0 - s32(IsTopLeftEdge(b12, a12));
  const s32 w1_bias = 0 - s32(IsTopLeftEdge(b20, a20));
  const s32 w2_bias = 0 - s32(IsTopLeftEdge(b01, a01));

  // compute base barycentric coordinates
  s32 w0 = orient2d(px1, py1, px2, py2, min_x, min_y);
  s32 w1 = orient2d(px2, py2, px0, py0, min_x, min_y);
  s32 w2 = orient2d(px0, py0, px1, py1, min_x, min_y);

  // *exclusive* of max coordinate in PSX
  for (s32 y = min_y; y <= max_y; y++)
  {
    s32 row_w0 = w0;
    s32 row_w1 = w1;
    s32 row_w2 = w2;

    for (s32 x = min_x; x <= max_x; x++)
    {
      if (((row_w0 + w0_bias) | (row_w1 + w1_bias) | (row_w2 + w2_bias)) >= 0)
      {
        const s32 b0 = row_w0;
        const s32 b1 = row_w1;
        const s32 b2 = row_w2;

        const u8 r =
          shading_enable ? Interpolate(v0->color_r, v1->color_r, v2->color_r, b0, b1, b2, ws, half_ws) : v0->color_r;
        const u8 g =
          shading_enable ? Interpolate(v0->color_g, v1->color_g, v2->color_g, b0, b1, b2, ws, half_ws) : v0->color_g;
        const u8 b =
          shading_enable ? Interpolate(v0->color_b, v1->color_b, v2->color_b, b0, b1, b2, ws, half_ws) : v0->color_b;

        const u8 texcoord_x = Interpolate(v0->texcoord_x, v1->texcoord_x, v2->texcoord_x, b0, b1, b2, ws, half_ws);
        const u8 texcoord_y = Interpolate(v0->texcoord_y, v1->texcoord_y, v2->texcoord_y, b0, b1, b2, ws, half_ws);

        ShadePixel<texture_enable, raw_texture_enable, transparency_enable, dithering_enable>(
          static_cast<u32>(x), static_cast<u32>(y), r, g, b, texcoord_x, texcoord_y);
      }

      row_w0 += a12;
      row_w1 += a20;
      row_w2 += a01;
    }

    w0 += b12;
    w1 += b20;
    w2 += b01;
  }

#undef orient2d
}

GPU_SW::DrawTriangleFunction GPU_SW::GetDrawTriangleFunction(bool shading_enable, bool texture_enable,
                                                             bool raw_texture_enable, bool transparency_enable,
                                                             bool dithering_enable)
{
#define F(SHADING, TEXTURE, RAW_TEXTURE, TRANSPARENCY, DITHERING)                                                      \
  &GPU_SW::DrawTriangle<SHADING, TEXTURE, RAW_TEXTURE, TRANSPARENCY, DITHERING>

  static constexpr DrawTriangleFunction funcs[2][2][2][2][2] = {
    {{{{F(false, false, false, false, false), F(false, false, false, false, true)},
       {F(false, false, false, true, false), F(false, false, false, true, true)}},
      {{F(false, false, true, false, false), F(false, false, true, false, true)},
       {F(false, false, true, true, false), F(false, false, true, true, true)}}},
     {{{F(false, true, false, false, false), F(false, true, false, false, true)},
       {F(false, true, false, true, false), F(false, true, false, true, true)}},
      {{F(false, true, true, false, false), F(false, true, true, false, true)},
       {F(false, true, true, true, false), F(false, true, true, true, true)}}}},
    {{{{F(true, false, false, false, false), F(true, false, false, false, true)},
       {F(true, false, false, true, false), F(true, false, false, true, true)}},
      {{F(true, false, true, false, false), F(true, false, true, false, true)},
       {F(true, false, true, true, false), F(true, false, true, true, true)}}},
     {{{F(true, true, false, false, false), F(true, true, false, false, true)},
       {F(true, true, false, true, false), F(true, true, false, true, true)}},
      {{F(true, true, true, false, false), F(true, true, true, false, true)},
       {F(true, true, true, true, false), F(true, true, true, true, true)}}}}};

#undef F

  return funcs[u8(shading_enable)][u8(texture_enable)][u8(raw_texture_enable)][u8(transparency_enable)]
              [u8(dithering_enable)];
}

template<bool texture_enable, bool raw_texture_enable, bool transparency_enable>
void GPU_SW::DrawRectangle(s32 origin_x, s32 origin_y, u32 width, u32 height, u8 r, u8 g, u8 b, u8 origin_texcoord_x,
                           u8 origin_texcoord_y)
{
  const s32 start_x = TruncateVertexPosition(m_drawing_offset.x + origin_x);
  const s32 start_y = TruncateVertexPosition(m_drawing_offset.y + origin_y);

  {
    const u32 clip_left = static_cast<u32>(std::clamp<s32>(start_x, m_drawing_area.left, m_drawing_area.right));
    const u32 clip_right =
      static_cast<u32>(std::clamp<s32>(start_x + static_cast<s32>(width), m_drawing_area.left, m_drawing_area.right)) +
      1u;
    const u32 clip_top = static_cast<u32>(std::clamp<s32>(start_y, m_drawing_area.top, m_drawing_area.bottom));
    const u32 clip_bottom =
      static_cast<u32>(std::clamp<s32>(start_y + static_cast<s32>(height), m_drawing_area.top, m_drawing_area.bottom)) +
      1u;
    AddDrawRectangleTicks(clip_right - clip_left, clip_bottom - clip_top, texture_enable, transparency_enable);
  }

  for (u32 offset_y = 0; offset_y < height; offset_y++)
  {
    const s32 y = start_y + static_cast<s32>(offset_y);
    if (y < static_cast<s32>(m_drawing_area.top) || y > static_cast<s32>(m_drawing_area.bottom))
      continue;

    const u8 texcoord_y = Truncate8(ZeroExtend32(origin_texcoord_y) + offset_y);

    for (u32 offset_x = 0; offset_x < width; offset_x++)
    {
      const s32 x = start_x + static_cast<s32>(offset_x);
      if (x < static_cast<s32>(m_drawing_area.left) || x > static_cast<s32>(m_drawing_area.right))
        continue;

      const u8 texcoord_x = Truncate8(ZeroExtend32(origin_texcoord_x) + offset_x);

      ShadePixel<texture_enable, raw_texture_enable, transparency_enable, false>(
        static_cast<u32>(x), static_cast<u32>(y), r, g, b, texcoord_x, texcoord_y);
    }
  }
}

constexpr GPU_SW::DitherLUT GPU_SW::ComputeDitherLUT()
{
  DitherLUT lut = {};
  for (u32 i = 0; i < DITHER_MATRIX_SIZE; i++)
  {
    for (u32 j = 0; j < DITHER_MATRIX_SIZE; j++)
    {
      for (s32 value = 0; value < DITHER_LUT_SIZE; value++)
      {
        const s32 dithered_value = (value + DITHER_MATRIX[i][j]) >> 3;
        lut[i][j][value] = static_cast<u8>((dithered_value < 0) ? 0 : ((dithered_value > 31) ? 31 : dithered_value));
      }
    }
  }
  return lut;
}

static constexpr GPU_SW::DitherLUT s_dither_lut = GPU_SW::ComputeDitherLUT();

template<bool texture_enable, bool raw_texture_enable, bool transparency_enable, bool dithering_enable>
void GPU_SW::ShadePixel(u32 x, u32 y, u8 color_r, u8 color_g, u8 color_b, u8 texcoord_x, u8 texcoord_y)
{
  VRAMPixel color;
  bool transparent;
  if constexpr (texture_enable)
  {
    // Apply texture window
    // TODO: Precompute the second half
    texcoord_x = (texcoord_x & ~(m_draw_mode.texture_window_mask_x * 8u)) |
                 ((m_draw_mode.texture_window_offset_x & m_draw_mode.texture_window_mask_x) * 8u);
    texcoord_y = (texcoord_y & ~(m_draw_mode.texture_window_mask_y * 8u)) |
                 ((m_draw_mode.texture_window_offset_y & m_draw_mode.texture_window_mask_y) * 8u);

    VRAMPixel texture_color;
    switch (m_draw_mode.GetTextureMode())
    {
      case GPU::TextureMode::Palette4Bit:
      {
        const u16 palette_value =
          GetPixel(std::min<u32>(m_draw_mode.texture_page_x + ZeroExtend32(texcoord_x / 4), VRAM_WIDTH - 1),
                   std::min<u32>(m_draw_mode.texture_page_y + ZeroExtend32(texcoord_y), VRAM_HEIGHT - 1));
        const u16 palette_index = (palette_value >> ((texcoord_x % 4) * 4)) & 0x0Fu;
        texture_color.bits =
          GetPixel(std::min<u32>(m_draw_mode.texture_palette_x + ZeroExtend32(palette_index), VRAM_WIDTH - 1),
                   m_draw_mode.texture_palette_y);
      }
      break;

      case GPU::TextureMode::Palette8Bit:
      {
        const u16 palette_value =
          GetPixel(std::min<u32>(m_draw_mode.texture_page_x + ZeroExtend32(texcoord_x / 2), VRAM_WIDTH - 1),
                   std::min<u32>(m_draw_mode.texture_page_y + ZeroExtend32(texcoord_y), VRAM_HEIGHT - 1));
        const u16 palette_index = (palette_value >> ((texcoord_x % 2) * 8)) & 0xFFu;
        texture_color.bits =
          GetPixel(std::min<u32>(m_draw_mode.texture_palette_x + ZeroExtend32(palette_index), VRAM_WIDTH - 1),
                   m_draw_mode.texture_palette_y);
      }
      break;

      default:
      {
        texture_color.bits =
          GetPixel(std::min<u32>(m_draw_mode.texture_page_x + ZeroExtend32(texcoord_x), VRAM_WIDTH - 1),
                   std::min<u32>(m_draw_mode.texture_page_y + ZeroExtend32(texcoord_y), VRAM_HEIGHT - 1));
      }
      break;
    }

    if (texture_color.bits == 0)
      return;

    transparent = texture_color.c;

    if constexpr (raw_texture_enable)
    {
      color.bits = texture_color.bits;
    }
    else
    {
      const u32 dither_y = (dithering_enable) ? (y & 3u) : 2u;
      const u32 dither_x = (dithering_enable) ? (x & 3u) : 3u;

      color.bits = (ZeroExtend16(s_dither_lut[dither_y][dither_x][(u16(texture_color.r) * u16(color_r)) >> 4]) << 0) |
                   (ZeroExtend16(s_dither_lut[dither_y][dither_x][(u16(texture_color.g) * u16(color_g)) >> 4]) << 5) |
                   (ZeroExtend16(s_dither_lut[dither_y][dither_x][(u16(texture_color.b) * u16(color_b)) >> 4]) << 10) |
                   (texture_color.bits & 0x8000u);
    }
  }
  else
  {
    transparent = true;

    const u32 dither_y = (dithering_enable) ? (y & 3u) : 2u;
    const u32 dither_x = (dithering_enable) ? (x & 3u) : 3u;

    color.bits = (ZeroExtend16(s_dither_lut[dither_y][dither_x][color_r]) << 0) |
                 (ZeroExtend16(s_dither_lut[dither_y][dither_x][color_g]) << 5) |
                 (ZeroExtend16(s_dither_lut[dither_y][dither_x][color_b]) << 10);
  }

  const VRAMPixel bg_color{GetPixel(static_cast<u32>(x), static_cast<u32>(y))};
  if constexpr (transparency_enable)
  {
    if (transparent)
    {
#define BLEND_AVERAGE(bg, fg) Truncate8(std::min<u32>((ZeroExtend32(bg) / 2) + (ZeroExtend32(fg) / 2), 0x1F))
#define BLEND_ADD(bg, fg) Truncate8(std::min<u32>(ZeroExtend32(bg) + ZeroExtend32(fg), 0x1F))
#define BLEND_SUBTRACT(bg, fg) Truncate8((bg > fg) ? ((bg) - (fg)) : 0)
#define BLEND_QUARTER(bg, fg) Truncate8(std::min<u32>(ZeroExtend32(bg) + ZeroExtend32(fg / 4), 0x1F))

#define BLEND_RGB(func)                                                                                                \
  color.Set(func(bg_color.r.GetValue(), color.r.GetValue()), func(bg_color.g.GetValue(), color.g.GetValue()),          \
            func(bg_color.b.GetValue(), color.b.GetValue()), color.c.GetValue())

      switch (m_draw_mode.GetTransparencyMode())
      {
        case GPU::TransparencyMode::HalfBackgroundPlusHalfForeground:
          BLEND_RGB(BLEND_AVERAGE);
          break;
        case GPU::TransparencyMode::BackgroundPlusForeground:
          BLEND_RGB(BLEND_ADD);
          break;
        case GPU::TransparencyMode::BackgroundMinusForeground:
          BLEND_RGB(BLEND_SUBTRACT);
          break;
        case GPU::TransparencyMode::BackgroundPlusQuarterForeground:
          BLEND_RGB(BLEND_QUARTER);
          break;
        default:
          break;
      }

#undef BLEND_RGB

#undef BLEND_QUARTER
#undef BLEND_SUBTRACT
#undef BLEND_ADD
#undef BLEND_AVERAGE
    }
  }
  else
  {
    UNREFERENCED_VARIABLE(transparent);
  }

  const u16 mask_and = m_GPUSTAT.GetMaskAND();
  if ((bg_color.bits & mask_and) != 0)
    return;

  if (IsInterlacedRenderingEnabled() && GetActiveLineLSB() == (static_cast<u32>(y) & 1u))
    return;

  SetPixel(static_cast<u32>(x), static_cast<u32>(y), color.bits | m_GPUSTAT.GetMaskOR());
}

constexpr FixedPointCoord GetLineCoordStep(s32 delta, s32 k)
{
  s64 delta_fp = static_cast<s64>(ZeroExtend64(static_cast<u32>(delta)) << 32);
  if (delta_fp < 0)
    delta_fp -= s64(k - 1);
  if (delta_fp > 0)
    delta_fp += s64(k - 1);

  return static_cast<FixedPointCoord>(delta_fp / k);
}

constexpr s32 FixedToIntCoord(FixedPointCoord x)
{
  return static_cast<s32>(Truncate32(x >> COORD_FRAC_BITS));
}

constexpr FixedPointColor GetLineColorStep(s32 delta, s32 k)
{
  return static_cast<s32>(static_cast<u32>(delta) << COLOR_FRAC_BITS) / k;
}

template<bool shading_enable, bool transparency_enable, bool dithering_enable>
void GPU_SW::DrawLine(const SWVertex* p0, const SWVertex* p1)
{
  // Algorithm based on Mednafen.
  if (p0->x > p1->x)
    std::swap(p0, p1);

  const s32 dx = p1->x - p0->x;
  const s32 dy = p1->y - p0->y;
  const s32 k = std::max(std::abs(dx), std::abs(dy));

  {
    // TODO: Move to base class
    const s32 min_x = std::min(p0->x, p1->x);
    const s32 max_x = std::max(p0->x, p1->x);
    const s32 min_y = std::min(p0->y, p1->y);
    const s32 max_y = std::max(p0->y, p1->y);

    const u32 clip_left = static_cast<u32>(std::clamp<s32>(min_x, m_drawing_area.left, m_drawing_area.left));
    const u32 clip_right = static_cast<u32>(std::clamp<s32>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
    const u32 clip_top = static_cast<u32>(std::clamp<s32>(min_y, m_drawing_area.top, m_drawing_area.bottom));
    const u32 clip_bottom = static_cast<u32>(std::clamp<s32>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

    AddDrawLineTicks(clip_right - clip_left, clip_bottom - clip_top, shading_enable);
  }

  FixedPointCoord step_x, step_y;
  FixedPointColor step_r, step_g, step_b;
  if (k > 0)
  {
    step_x = GetLineCoordStep(dx, k);
    step_y = GetLineCoordStep(dy, k);

    if constexpr (shading_enable)
    {
      step_r = GetLineColorStep(s32(ZeroExtend32(p1->color_r)) - s32(ZeroExtend32(p0->color_r)), k);
      step_g = GetLineColorStep(s32(ZeroExtend32(p1->color_g)) - s32(ZeroExtend32(p0->color_g)), k);
      step_b = GetLineColorStep(s32(ZeroExtend32(p1->color_b)) - s32(ZeroExtend32(p0->color_b)), k);
    }
    else
    {
      step_r = 0;
      step_g = 0;
      step_b = 0;
    }
  }
  else
  {
    step_x = 0;
    step_y = 0;
    step_r = 0;
    step_g = 0;
    step_b = 0;
  }

  FixedPointCoord current_x = IntToFixedCoord(p0->x);
  FixedPointCoord current_y = IntToFixedCoord(p0->y);
  FixedPointColor current_r = IntToFixedColor(p0->color_r);
  FixedPointColor current_g = IntToFixedColor(p0->color_g);
  FixedPointColor current_b = IntToFixedColor(p0->color_b);

  for (s32 i = 0; i <= k; i++)
  {
    const s32 x = m_drawing_offset.x + FixedToIntCoord(current_x);
    const s32 y = m_drawing_offset.y + FixedToIntCoord(current_y);

    const u8 r = shading_enable ? FixedColorToInt(current_r) : p0->color_r;
    const u8 g = shading_enable ? FixedColorToInt(current_g) : p0->color_g;
    const u8 b = shading_enable ? FixedColorToInt(current_b) : p0->color_b;

    if (x >= static_cast<s32>(m_drawing_area.left) && x <= static_cast<s32>(m_drawing_area.right) &&
        y >= static_cast<s32>(m_drawing_area.top) && y <= static_cast<s32>(m_drawing_area.bottom))
    {
      ShadePixel<false, false, transparency_enable, dithering_enable>(static_cast<u32>(x), static_cast<u32>(y), r, g, b,
                                                                      0, 0);
    }

    current_x += step_x;
    current_y += step_y;

    if constexpr (shading_enable)
    {
      current_r += step_r;
      current_g += step_g;
      current_b += step_b;
    }
  }
}

GPU_SW::DrawLineFunction GPU_SW::GetDrawLineFunction(bool shading_enable, bool transparency_enable,
                                                     bool dithering_enable)
{
#define F(SHADING, TRANSPARENCY, DITHERING) &GPU_SW::DrawLine<SHADING, TRANSPARENCY, DITHERING>

  static constexpr DrawLineFunction funcs[2][2][2] = {
    {{F(false, false, false), F(false, false, true)}, {F(false, true, false), F(false, true, true)}},
    {{F(true, false, false), F(true, false, true)}, {F(true, true, false), F(true, true, true)}}};

#undef F

  return funcs[u8(shading_enable)][u8(transparency_enable)][u8(dithering_enable)];
}

GPU_SW::DrawRectangleFunction GPU_SW::GetDrawRectangleFunction(bool texture_enable, bool raw_texture_enable,
                                                               bool transparency_enable)
{
#define F(TEXTURE, RAW_TEXTURE, TRANSPARENCY) &GPU_SW::DrawRectangle<TEXTURE, RAW_TEXTURE, TRANSPARENCY>

  static constexpr DrawRectangleFunction funcs[2][2][2] = {
    {{F(false, false, false), F(false, false, true)}, {F(false, true, false), F(false, true, true)}},
    {{F(true, false, false), F(true, false, true)}, {F(true, true, false), F(true, true, true)}}};

#undef F

  return funcs[u8(texture_enable)][u8(raw_texture_enable)][u8(transparency_enable)];
}

std::unique_ptr<GPU> GPU::CreateSoftwareRenderer()
{
  return std::make_unique<GPU_SW>();
}
