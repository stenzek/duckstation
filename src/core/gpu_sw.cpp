#include "gpu_sw.h"
#include "YBaseLib/Log.h"
#include "YBaseLib/Timer.h"
#include "common/gl_texture.h"
#include "host_interface.h"
#include "system.h"
#include <algorithm>
Log_SetChannel(GPU_SW);

GPU_SW::GPU_SW()
{
  m_vram.fill(0);
}

GPU_SW::~GPU_SW() = default;

bool GPU_SW::Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, Timers* timers)
{
  if (!GPU::Initialize(system, dma, interrupt_controller, timers))
    return false;

  m_display_texture = std::make_unique<GL::Texture>(VRAM_WIDTH, VRAM_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE);
  return true;
}

void GPU_SW::Reset()
{
  GPU::Reset();

  m_vram.fill(0);
}

void GPU_SW::ReadVRAM(u32 x, u32 y, u32 width, u32 height, void* buffer)
{
  u16* buffer_ptr = static_cast<u16*>(buffer);
  for (u32 yoffs = 0; yoffs < height; yoffs++)
  {
    u16* src_ptr = GetPixelPtr(x, y + yoffs);
    std::copy_n(src_ptr, width, buffer_ptr);
    buffer_ptr += width;
  }
}

void GPU_SW::FillVRAM(u32 x, u32 y, u32 width, u32 height, u16 color)
{
  for (u32 yoffs = 0; yoffs < height; yoffs++)
    std::fill_n(GetPixelPtr(x, y + yoffs), width, color);
}

void GPU_SW::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data)
{
  const u16* src_ptr = static_cast<const u16*>(data);
  for (u32 yoffs = 0; yoffs < height; yoffs++)
  {
    u16* dst_ptr = GetPixelPtr(x, y + yoffs);
    std::copy_n(src_ptr, width, dst_ptr);
    src_ptr += width;
  }
}

void GPU_SW::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  for (u32 yoffs = 0; yoffs < height; yoffs++)
  {
    const u16* src_ptr = GetPixelPtr(src_x, src_y + yoffs);
    u16* dst_ptr = GetPixelPtr(dst_x, dst_y + yoffs);
    std::copy_n(src_ptr, width, dst_ptr);
  }
}

void GPU_SW::CopyOut15Bit(const u16* src_ptr, u32 src_stride, u32* dst_ptr, u32 dst_stride, u32 width, u32 height)
{
  // OpenGL is beeg silly for lower-left origin
  dst_ptr = (dst_ptr + ((height - 1) * dst_stride));

  for (u32 row = 0; row < height; row++)
  {
    const u16* src_row_ptr = src_ptr;
    u32* dst_row_ptr = dst_ptr;
    for (u32 col = 0; col < width; col++)
      *(dst_row_ptr++) = RGBA5551ToRGBA8888(*(src_row_ptr++));

    src_ptr += src_stride;
    dst_ptr -= dst_stride;
  }
}

void GPU_SW::CopyOut24Bit(const u16* src_ptr, u32 src_stride, u32* dst_ptr, u32 dst_stride, u32 width, u32 height)
{
  // OpenGL is beeg silly for lower-left origin
  dst_ptr = (dst_ptr + ((height - 1) * dst_stride));

  for (u32 row = 0; row < height; row++)
  {
    const u8* src_row_ptr = reinterpret_cast<const u8*>(src_ptr);
    u32* dst_row_ptr = dst_ptr;

    // Beware unaligned accesses.
    for (u32 col = 0; col < width; col++)
    {
      // This will fill the alpha channel with junk, but that's okay since we don't use it
      std::memcpy(dst_row_ptr, src_row_ptr, sizeof(u32));
      src_row_ptr += 3;
      dst_row_ptr++;
    }

    src_ptr += src_stride;
    dst_ptr -= dst_stride;
  }
}

void GPU_SW::UpdateDisplay()
{
  // fill display texture
  m_display_texture_buffer.resize(VRAM_WIDTH * VRAM_HEIGHT);

  u32 display_width;
  u32 display_height;
  float display_aspect_ratio;
  if (!m_system->GetSettings().debugging.show_vram)
  {
    // TODO: Handle interlacing
    const u32 vram_offset_x = m_crtc_state.regs.X;
    const u32 vram_offset_y = m_crtc_state.regs.Y;
    display_width = std::min<u32>(m_crtc_state.display_width, VRAM_WIDTH - vram_offset_x);
    display_height = std::min<u32>(m_crtc_state.display_height << BoolToUInt8(m_GPUSTAT.vertical_interlace),
                                   VRAM_HEIGHT - vram_offset_y);
    display_aspect_ratio = m_crtc_state.display_aspect_ratio;

    if (m_GPUSTAT.display_disable)
    {
      m_system->GetHostInterface()->SetDisplayTexture(nullptr, 0, 0, 0, 0, display_aspect_ratio);
      return;
    }
    else if (m_GPUSTAT.display_area_color_depth_24)
    {
      CopyOut24Bit(m_vram.data() + vram_offset_y * VRAM_WIDTH + vram_offset_x, VRAM_WIDTH,
                   m_display_texture_buffer.data(), display_width, display_width, display_height);
    }
    else
    {
      CopyOut15Bit(m_vram.data() + vram_offset_y * VRAM_WIDTH + vram_offset_x, VRAM_WIDTH,
                   m_display_texture_buffer.data(), display_width, display_width, display_height);
    }
  }
  else
  {
    display_width = VRAM_WIDTH;
    display_height = VRAM_HEIGHT;
    display_aspect_ratio = 1.0f;
    CopyOut15Bit(m_vram.data(), VRAM_WIDTH, m_display_texture_buffer.data(), display_width, display_width,
                 display_height);
  }

  m_display_texture->Bind();
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, display_width, display_height, GL_RGBA, GL_UNSIGNED_BYTE,
                  m_display_texture_buffer.data());
  m_system->GetHostInterface()->SetDisplayTexture(m_display_texture.get(), 0, 0, display_width, display_height,
                                                  display_aspect_ratio);
}

void GPU_SW::DispatchRenderCommand(RenderCommand rc, u32 num_vertices, const u32* command_ptr)
{
  switch (rc.primitive)
  {
    case Primitive::Polygon:
    {
      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;

      if (textured)
      {
        if (shaded)
          m_render_state.SetFromPolygonTexcoord(command_ptr[2], command_ptr[5]);
        else
          m_render_state.SetFromPolygonTexcoord(command_ptr[2], command_ptr[4]);
      }
      else
      {
        m_render_state.SetFromPageAttribute(Truncate16(m_GPUSTAT.bits));
      }

      u32 buffer_pos = 1;
      for (u32 i = 0; i < num_vertices; i++)
      {
        SWVertex& vert = m_vertex_buffer[i];
        const u32 color_rgb = (shaded && i > 0) ? (command_ptr[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;
        vert.color_r = Truncate8(color_rgb);
        vert.color_g = Truncate8(color_rgb >> 8);
        vert.color_b = Truncate8(color_rgb >> 16);

        const VertexPosition vp{command_ptr[buffer_pos++]};
        vert.x = vp.x;
        vert.y = vp.y;

        if (textured)
        {
          std::tie(vert.texcoord_x, vert.texcoord_y) = UnpackTexcoord(Truncate16(command_ptr[buffer_pos++]));
        }
        else
        {
          vert.texcoord_x = 0;
          vert.texcoord_y = 0;
        }
      }

      DrawTriangle(rc, &m_vertex_buffer[0], &m_vertex_buffer[1], &m_vertex_buffer[2]);
      if (num_vertices > 3)
        DrawTriangle(rc, &m_vertex_buffer[2], &m_vertex_buffer[1], &m_vertex_buffer[3]);
    }
    break;

    case Primitive::Rectangle:
    {
      u32 buffer_pos = 1;
      const auto [r, g, b] = UnpackColorRGB24(rc.color_for_first_vertex);
      const VertexPosition vp{command_ptr[buffer_pos++]};
      const u32 texcoord_and_palette = rc.texture_enable ? command_ptr[buffer_pos++] : 0;
      const auto [texcoord_x, texcoord_y] = UnpackTexcoord(Truncate16(texcoord_and_palette));

      m_render_state.SetFromPageAttribute(Truncate16(m_GPUSTAT.bits));
      m_render_state.SetFromPaletteAttribute(Truncate16(texcoord_and_palette >> 16));

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
          width = static_cast<s32>(command_ptr[buffer_pos] & UINT32_C(0xFFFF));
          height = static_cast<s32>(command_ptr[buffer_pos] >> 16);
          break;
      }

      DrawRectangle(rc, vp.x, vp.y, width, height, r, g, b, texcoord_x, texcoord_y);
    }
    break;

    case Primitive::Line:
    {
    }
    break;

    default:
      UnreachableCode();
      break;
  }
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

static constexpr u8 Interpolate(u8 v0, u8 v1, u8 v2, s32 w0, s32 w1, s32 w2, s32 ws)
{
  const s32 v = w0 * static_cast<s32>(static_cast<u32>(v0)) + w1 * static_cast<s32>(static_cast<u32>(v1)) +
                w2 * static_cast<s32>(static_cast<u32>(v2));
  const s32 vd = v / ws;
  return (vd < 0) ? 0 : ((vd > 0xFF) ? 0xFF : static_cast<u8>(vd));
}

void GPU_SW::DrawTriangle(RenderCommand rc, const SWVertex* v0, const SWVertex* v1, const SWVertex* v2)
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
  if (ws == 0)
    return;

  // compute bounding box of triangle
  s32 min_x = std::min(px0, std::min(px1, px2));
  s32 max_x = std::max(px0, std::max(px1, px2));
  s32 min_y = std::min(py0, std::min(py1, py2));
  s32 max_y = std::max(py0, std::max(py1, py2));

  // reject triangles which cover the whole vram area
  if ((max_x - min_x) >= VRAM_WIDTH || (max_y - min_y) >= VRAM_HEIGHT)
    return;

  // clip to drawing area
  min_x = std::clamp(min_x, static_cast<s32>(m_drawing_area.left), static_cast<s32>(m_drawing_area.right));
  max_x = std::clamp(max_x, static_cast<s32>(m_drawing_area.left), static_cast<s32>(m_drawing_area.right));
  min_y = std::clamp(min_y, static_cast<s32>(m_drawing_area.top), static_cast<s32>(m_drawing_area.bottom));
  max_y = std::clamp(max_y, static_cast<s32>(m_drawing_area.top), static_cast<s32>(m_drawing_area.bottom));

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
          rc.shading_enable ? Interpolate(v0->color_r, v1->color_r, v2->color_r, b0, b1, b2, ws) : v0->color_r;
        const u8 g =
          rc.shading_enable ? Interpolate(v0->color_g, v1->color_g, v2->color_g, b0, b1, b2, ws) : v0->color_g;
        const u8 b =
          rc.shading_enable ? Interpolate(v0->color_b, v1->color_b, v2->color_b, b0, b1, b2, ws) : v0->color_b;

        const u8 texcoord_x = Interpolate(v0->texcoord_x, v1->texcoord_x, v2->texcoord_x, b0, b1, b2, ws);
        const u8 texcoord_y = Interpolate(v0->texcoord_y, v1->texcoord_y, v2->texcoord_y, b0, b1, b2, ws);

        ShadePixel(rc, static_cast<u32>(x), static_cast<u32>(y), r, g, b, texcoord_x, texcoord_y,
                   rc.IsDitheringEnabled() && m_GPUSTAT.dither_enable);
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

void GPU_SW::DrawRectangle(RenderCommand rc, s32 origin_x, s32 origin_y, u32 width, u32 height, u8 r, u8 g, u8 b,
                           u8 origin_texcoord_x, u8 origin_texcoord_y)
{
  origin_x += m_drawing_offset.x;
  origin_y += m_drawing_offset.y;

  for (u32 offset_y = 0; offset_y < height; offset_y++)
  {
    const s32 y = origin_y + static_cast<s32>(offset_y);
    if (y < static_cast<s32>(m_drawing_area.top) || y > static_cast<s32>(m_drawing_area.bottom))
      continue;

    const u8 texcoord_y = Truncate8(ZeroExtend32(origin_texcoord_y) + offset_y);

    for (u32 offset_x = 0; offset_x < width; offset_x++)
    {
      const s32 x = origin_x + static_cast<s32>(offset_x);
      if (x < static_cast<s32>(m_drawing_area.left) || x > static_cast<s32>(m_drawing_area.right))
        continue;

      const u8 texcoord_x = Truncate8(ZeroExtend32(origin_texcoord_x) + offset_x);

      ShadePixel(rc, static_cast<u32>(x), static_cast<u32>(y), r, g, b, texcoord_x, texcoord_y, false);
    }
  }
}

void GPU_SW::ShadePixel(RenderCommand rc, u32 x, u32 y, u8 color_r, u8 color_g, u8 color_b, u8 texcoord_x,
                        u8 texcoord_y, bool dithering)
{
  VRAMPixel color;
  bool transparent = true;
  if (rc.texture_enable)
  {
    // Apply texture window
    // TODO: Precompute the second half
    texcoord_x = (texcoord_x & ~(m_render_state.texture_window_mask_x * 8u)) |
                 ((m_render_state.texture_window_offset_x & m_render_state.texture_window_mask_x) * 8u);
    texcoord_y = (texcoord_y & ~(m_render_state.texture_window_mask_y * 8u)) |
                 ((m_render_state.texture_window_offset_y & m_render_state.texture_window_mask_y) * 8u);

    VRAMPixel texture_color;
    switch (m_render_state.texture_mode)
    {
      case GPU::TextureMode::Palette4Bit:
      {
        const u16 palette_value =
          GetPixel(std::min<u32>(m_render_state.texture_page_x + ZeroExtend32(texcoord_x / 4), VRAM_WIDTH - 1),
                   std::min<u32>(m_render_state.texture_page_y + ZeroExtend32(texcoord_y), VRAM_HEIGHT - 1));
        const u16 palette_index = (palette_value >> ((texcoord_x % 4) * 4)) & 0x0Fu;
        texture_color.bits =
          GetPixel(std::min<u32>(m_render_state.texture_palette_x + ZeroExtend32(palette_index), VRAM_WIDTH - 1),
                   m_render_state.texture_palette_y);
      }
      break;

      case GPU::TextureMode::Palette8Bit:
      {
        const u16 palette_value =
          GetPixel(std::min<u32>(m_render_state.texture_page_x + ZeroExtend32(texcoord_x / 2), VRAM_WIDTH - 1),
                   std::min<u32>(m_render_state.texture_page_y + ZeroExtend32(texcoord_y), VRAM_HEIGHT - 1));
        const u16 palette_index = (palette_value >> ((texcoord_x % 2) * 8)) & 0xFFu;
        texture_color.bits =
          GetPixel(std::min<u32>(m_render_state.texture_palette_x + ZeroExtend32(palette_index), VRAM_WIDTH - 1),
                   m_render_state.texture_palette_y);
      }
      break;

      default:
      {
        texture_color.bits =
          GetPixel(std::min<u32>(m_render_state.texture_page_x + ZeroExtend32(texcoord_x), VRAM_WIDTH - 1),
                   std::min<u32>(m_render_state.texture_page_y + ZeroExtend32(texcoord_y), VRAM_HEIGHT - 1));
      }
      break;
    }

    if (texture_color.bits == 0)
      return;

    transparent = texture_color.c;

    if (rc.raw_texture_enable)
    {
      color.bits = texture_color.bits;
    }
    else
    {
      const u8 r = Truncate8(std::min<u16>((ZeroExtend16(texture_color.GetR8()) * ZeroExtend16(color_r)) >> 7, 0xFF));
      const u8 g = Truncate8(std::min<u16>((ZeroExtend16(texture_color.GetG8()) * ZeroExtend16(color_g)) >> 7, 0xFF));
      const u8 b = Truncate8(std::min<u16>((ZeroExtend16(texture_color.GetB8()) * ZeroExtend16(color_b)) >> 7, 0xFF));
      if (dithering)
        color.SetRGB24Dithered(x, y, r, g, b);
      else
        color.SetRGB24(r, g, b);
    }
  }
  else
  {
    if (dithering)
      color.SetRGB24Dithered(x, y, color_r, color_g, color_b);
    else
      color.SetRGB24(color_r, color_g, color_b);
  }

  if (rc.transparency_enable && transparent)
  {
    const VRAMPixel bg_color{GetPixel(static_cast<u32>(x), static_cast<u32>(y))};

#define BLEND_AVERAGE(bg, fg) Truncate8(std::min<u32>((ZeroExtend32(bg) / 2) + (ZeroExtend32(fg) / 2), 0x1F))
#define BLEND_ADD(bg, fg) Truncate8(std::min<u32>(ZeroExtend32(bg) + ZeroExtend32(fg), 0x1F))
#define BLEND_SUBTRACT(bg, fg) Truncate8((bg > fg) ? ((bg) - (fg)) : 0)
#define BLEND_QUARTER(bg, fg) Truncate8(std::min<u32>(ZeroExtend32(bg) + ZeroExtend32(fg / 4), 0x1F))

#define BLEND_RGB(func)                                                                                                \
  color.Set(func(bg_color.r.GetValue(), color.r.GetValue()), func(bg_color.g.GetValue(), color.g.GetValue()),          \
            func(bg_color.b.GetValue(), color.b.GetValue()), color.c.GetValue())

    switch (m_render_state.transparency_mode)
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

  SetPixel(static_cast<u32>(x), static_cast<u32>(y), color.bits);
}

std::unique_ptr<GPU> GPU::CreateSoftwareRenderer()
{
  return std::make_unique<GPU_SW>();
}
