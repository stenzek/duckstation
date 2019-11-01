#include "gpu_hw.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
#include <sstream>
Log_SetChannel(GPU_HW);

GPU_HW::GPU_HW() = default;

GPU_HW::~GPU_HW() = default;

void GPU_HW::Reset()
{
  GPU::Reset();

  m_batch = {};
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
      bool restart_strip = (rc.quad_polygon && !m_batch.vertices.empty());
      if (restart_strip)
        m_batch.vertices.push_back(m_batch.vertices.back());

      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;

      u32 buffer_pos = 1;
      for (u32 i = 0; i < num_vertices; i++)
      {
        HWVertex hw_vert;
        hw_vert.color = (shaded && i > 0) ? (command_ptr[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;

        const VertexPosition vp{command_ptr[buffer_pos++]};
        hw_vert.x = vp.x;
        hw_vert.y = vp.y;
        hw_vert.texpage = texpage;

        if (textured)
        {
          const auto [texcoord_x, texcoord_y] = UnpackTexcoord(Truncate16(command_ptr[buffer_pos++]));
          hw_vert.texcoord = HWVertex::PackTexcoord(texcoord_x, texcoord_y);
        }
        else
        {
          hw_vert.texcoord = 0;
        }

        m_batch.vertices.push_back(hw_vert);
        if (restart_strip)
        {
          m_batch.vertices.push_back(m_batch.vertices.back());
          restart_strip = false;
        }
      }
    }
    break;

    case Primitive::Rectangle:
    {
      // if we're drawing quads, we need to create a degenerate triangle to restart the triangle strip
      const bool restart_strip = !m_batch.vertices.empty();
      if (restart_strip)
        m_batch.vertices.push_back(m_batch.vertices.back());

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

      m_batch.vertices.push_back(
        HWVertex{pos_left, pos_top, color, texpage, HWVertex::PackTexcoord(tex_left, tex_top)});
      if (restart_strip)
        m_batch.vertices.push_back(m_batch.vertices.back());
      m_batch.vertices.push_back(
        HWVertex{pos_right, pos_top, color, texpage, HWVertex::PackTexcoord(tex_right, tex_top)});
      m_batch.vertices.push_back(
        HWVertex{pos_left, pos_bottom, color, texpage, HWVertex::PackTexcoord(tex_left, tex_bottom)});
      m_batch.vertices.push_back(
        HWVertex{pos_right, pos_bottom, color, texpage, HWVertex::PackTexcoord(tex_right, tex_bottom)});
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
        m_batch.vertices.push_back(HWVertex{vp.x.GetValue(), vp.y.GetValue(), color});
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

static void DefineMacro(std::stringstream& ss, const char* name, bool enabled)
{
  if (enabled)
    ss << "#define " << name << " 1\n";
  else
    ss << "/* #define " << name << " 0 */\n";
}

void GPU_HW::GenerateShaderHeader(std::stringstream& ss)
{
  ss << "#version 330 core\n\n";
  ss << "const int RESOLUTION_SCALE = " << m_resolution_scale << ";\n";
  ss << "const ivec2 VRAM_SIZE = ivec2(" << VRAM_WIDTH << ", " << VRAM_HEIGHT << ") * RESOLUTION_SCALE;\n";
  ss << "const vec2 RCP_VRAM_SIZE = vec2(1.0, 1.0) / vec2(VRAM_SIZE);\n";
  ss << R"(

float fixYCoord(float y)
{
  return 1.0 - RCP_VRAM_SIZE.y - y;
}

int fixYCoord(int y)
{
  return VRAM_SIZE.y - y - 1;
}

uint RGBA8ToRGBA5551(vec4 v)
{
  uint r = uint(v.r * 255.0) >> 3;
  uint g = uint(v.g * 255.0) >> 3;
  uint b = uint(v.b * 255.0) >> 3;
  uint a = (v.a != 0.0) ? 1u : 0u;
  return (r) | (g << 5) | (b << 10) | (a << 15);
}

vec4 RGBA5551ToRGBA8(uint v)
{
  uint r = (v & 0x1Fu);
  uint g = ((v >> 5) & 0x1Fu);
  uint b = ((v >> 10) & 0x1Fu);
  uint a = ((v >> 15) & 0x01u);

  return vec4(float(r) * 255.0, float(g) * 255.0, float(b) * 255.0, float(a) * 255.0);
}
)";
}

std::string GPU_HW::GenerateVertexShader(bool textured)
{
  std::stringstream ss;
  GenerateShaderHeader(ss);
  DefineMacro(ss, "TEXTURED", textured);

  ss << R"(
in ivec2 a_pos;
in vec4 a_col0;
in int a_texcoord;
in int a_texpage;

out vec3 v_col0;
#if TEXTURED
  out vec2 v_tex0;
  flat out ivec4 v_texpage;
#endif

uniform ivec2 u_pos_offset;

void main()
{
  // 0..+1023 -> -1..1
  float pos_x = (float(a_pos.x + u_pos_offset.x) / 512.0) - 1.0;
  float pos_y = (float(a_pos.y + u_pos_offset.y) / -256.0) + 1.0;
  gl_Position = vec4(pos_x, pos_y, 0.0, 1.0);

  v_col0 = a_col0.rgb;
  #if TEXTURED
    v_tex0 = vec2(float(a_texcoord & 0xFFFF), float(a_texcoord >> 16)) / vec2(255.0);

    // base_x,base_y,palette_x,palette_y
    v_texpage.x = (a_texpage & 15) * 64 * RESOLUTION_SCALE;
    v_texpage.y = ((a_texpage >> 4) & 1) * 256 * RESOLUTION_SCALE;
    v_texpage.z = ((a_texpage >> 16) & 63) * 16 * RESOLUTION_SCALE;
    v_texpage.w = ((a_texpage >> 22) & 511) * RESOLUTION_SCALE;
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW::GenerateFragmentShader(HWBatchRenderMode transparency, TextureMode texture_mode, bool dithering)
{
  const TextureMode actual_texture_mode = texture_mode & ~TextureMode::RawTextureBit;
  const bool raw_texture = (texture_mode & TextureMode::RawTextureBit) == TextureMode::RawTextureBit;

  std::stringstream ss;
  GenerateShaderHeader(ss);
  DefineMacro(ss, "TRANSPARENCY", transparency != HWBatchRenderMode::TransparencyDisabled);
  DefineMacro(ss, "TRANSPARENCY_ONLY_OPAQUE", transparency == HWBatchRenderMode::OnlyOpaque);
  DefineMacro(ss, "TRANSPARENCY_ONLY_TRANSPARENCY", transparency == HWBatchRenderMode::OnlyTransparent);
  DefineMacro(ss, "TEXTURED", actual_texture_mode != TextureMode::Disabled);
  DefineMacro(ss, "PALETTE",
              actual_texture_mode == GPU::TextureMode::Palette4Bit ||
                actual_texture_mode == GPU::TextureMode::Palette8Bit);
  DefineMacro(ss, "PALETTE_4_BIT", actual_texture_mode == GPU::TextureMode::Palette4Bit);
  DefineMacro(ss, "PALETTE_8_BIT", actual_texture_mode == GPU::TextureMode::Palette8Bit);
  DefineMacro(ss, "RAW_TEXTURE", raw_texture);
  DefineMacro(ss, "DITHERING", dithering);

  ss << "const int[16] s_dither_values = int[16]( ";
  for (u32 i = 0; i < 16; i++)
  {
    if (i > 0)
      ss << ", ";
    ss << DITHER_MATRIX[i / 4][i % 4];
  }
  ss << " );\n";

  ss << R"(
in vec3 v_col0;
uniform vec2 u_transparent_alpha;
#if TEXTURED
  in vec2 v_tex0;
  flat in ivec4 v_texpage;
  uniform sampler2D samp0;
  uniform uvec4 u_texture_window;
#endif

out vec4 o_col0;

vec4 ApplyDithering(vec4 col)
{
  ivec3 icol = ivec3(col.rgb * vec3(255.0, 255.0, 255.0));

  // apply dither
  ivec2 fc = ivec2(gl_FragCoord.xy) & ivec2(3, 3);
  int offset = s_dither_values[fc.y * 4 + fc.x];
  icol += ivec3(offset, offset, offset);

  // saturate
  icol = clamp(icol, ivec3(0, 0, 0), ivec3(255, 255, 255));

  // clip to 5-bit range
  return vec4((icol.rgb >> 3) / vec3(31.0, 31.0, 31.0), col.a);
}

#if TEXTURED
ivec2 ApplyNativeTextureWindow(ivec2 coords)
{
  uint x = (uint(coords.x) & ~(u_texture_window.x * 8u)) | ((u_texture_window.z & u_texture_window.x) * 8u);
  uint y = (uint(coords.y) & ~(u_texture_window.y * 8u)) | ((u_texture_window.w & u_texture_window.y) * 8u);
  return ivec2(int(x), int(y));
}  

ivec2 ApplyTextureWindow(ivec2 coords)
{
  if (RESOLUTION_SCALE == 1)
    return ApplyNativeTextureWindow(coords);

  ivec2 downscaled_coords = coords / ivec2(RESOLUTION_SCALE);
  ivec2 coords_offset = coords % ivec2(RESOLUTION_SCALE);
  return (ApplyNativeTextureWindow(downscaled_coords) * ivec2(RESOLUTION_SCALE)) + coords_offset;
}

vec4 SampleFromVRAM(vec2 coord)
{
  // from 0..1 to 0..255
  ivec2 icoord = ivec2(coord * vec2(255 * RESOLUTION_SCALE));
  icoord = ApplyTextureWindow(icoord);

  // adjust for tightly packed palette formats
  ivec2 index_coord = icoord;
  #if PALETTE_4_BIT
    index_coord.x /= 4;
  #elif PALETTE_8_BIT
    index_coord.x /= 2;
  #endif

  // fixup coords
  ivec2 vicoord = ivec2(v_texpage.x + index_coord.x, fixYCoord(v_texpage.y + index_coord.y));

  // load colour/palette
  vec4 color = texelFetch(samp0, vicoord, 0);

  // apply palette
  #if PALETTE
    #if PALETTE_4_BIT
      int subpixel = int(icoord.x / RESOLUTION_SCALE) & 3;
      uint vram_value = RGBA8ToRGBA5551(color);
      int palette_index = int((vram_value >> (subpixel * 4)) & 0x0Fu);
    #elif PALETTE_8_BIT
      int subpixel = int(icoord.x / RESOLUTION_SCALE) & 1;
      uint vram_value = RGBA8ToRGBA5551(color);
      int palette_index = int((vram_value >> (subpixel * 8)) & 0xFFu);
    #endif
    ivec2 palette_icoord = ivec2(v_texpage.z + (palette_index * RESOLUTION_SCALE), fixYCoord(v_texpage.w));
    color = texelFetch(samp0, palette_icoord, 0);
  #endif

  return color;
}
#endif

void main()
{
  #if TEXTURED
    vec4 texcol = SampleFromVRAM(v_tex0);
    if (texcol == vec4(0.0, 0.0, 0.0, 0.0))
      discard;

    vec3 color;
    #if RAW_TEXTURE
      color = texcol.rgb;
    #else
      color = vec3((ivec3(v_col0 * 255.0) * ivec3(texcol.rgb * 255.0)) >> 7) / 255.0;
    #endif

    #if TRANSPARENCY
      // Apply semitransparency. If not a semitransparent texel, destination alpha is ignored.
      if (texcol.a != 0)
      {
        #if TRANSPARENCY_ONLY_OPAQUE
          discard;
        #endif
        o_col0 = vec4(color * u_transparent_alpha.x, u_transparent_alpha.y);
      }
      else
      {
        #if TRANSPARENCY_ONLY_TRANSPARENCY
          discard;
        #endif
        o_col0 = vec4(color, 0.0);
      }
    #else
      // Mask bit from texture.
      o_col0 = vec4(color, texcol.a);
    #endif
  #else
    #if TRANSPARENCY
      o_col0 = vec4(v_col0 * u_transparent_alpha.x, u_transparent_alpha.y);
    #else
      // Mask bit is cleared for untextured polygons.
      o_col0 = vec4(v_col0, 0.0);
    #endif
  #endif

  #if DITHERING
    o_col0 = ApplyDithering(o_col0);
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW::GenerateScreenQuadVertexShader()
{
  std::stringstream ss;
  GenerateShaderHeader(ss);
  ss << R"(

out vec2 v_tex0;

void main()
{
  v_tex0 = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(v_tex0 * vec2(2.0f, -2.0f) + vec2(-1.0f, 1.0f), 0.0f, 1.0f);
  gl_Position.y = -gl_Position.y;
}
)";

  return ss.str();
}

std::string GPU_HW::GenerateFillFragmentShader()
{
  std::stringstream ss;
  GenerateShaderHeader(ss);

  ss << R"(
uniform vec4 fill_color;
out vec4 o_col0;

void main()
{
  o_col0 = fill_color;
}
)";

  return ss.str();
}

std::string GPU_HW::GenerateDisplayFragmentShader(bool depth_24bit, bool interlaced)
{
  std::stringstream ss;
  GenerateShaderHeader(ss);
  DefineMacro(ss, "DEPTH_24BIT", depth_24bit);
  DefineMacro(ss, "INTERLACED", interlaced);

  ss << R"(
in vec2 v_tex0;
out vec4 o_col0;

uniform sampler2D samp0;
uniform ivec3 u_base_coords;

ivec2 GetCoords(vec2 fragcoord)
{
  ivec2 icoords = ivec2(fragcoord);
  #if INTERLACED
    if ((((icoords.y - u_base_coords.z) / RESOLUTION_SCALE) & 1) != 0)
      discard;
  #endif
  return icoords;
}

void main()
{
  ivec2 icoords = GetCoords(gl_FragCoord.xy);

  #if DEPTH_24BIT
    // compute offset in dwords from the start of the 24-bit values
    ivec2 base = ivec2(u_base_coords.x, u_base_coords.y + icoords.y);
    int xoff = int(icoords.x);
    int dword_index = (xoff / 2) + (xoff / 4);

    // sample two adjacent dwords, or four 16-bit values as the 24-bit value will lie somewhere between these
    uint s0 = RGBA8ToRGBA5551(texelFetch(samp0, ivec2(base.x + dword_index * 2 + 0, base.y), 0));
    uint s1 = RGBA8ToRGBA5551(texelFetch(samp0, ivec2(base.x + dword_index * 2 + 1, base.y), 0));
    uint s2 = RGBA8ToRGBA5551(texelFetch(samp0, ivec2(base.x + (dword_index + 1) * 2 + 0, base.y), 0));
    uint s3 = RGBA8ToRGBA5551(texelFetch(samp0, ivec2(base.x + (dword_index + 1) * 2 + 1, base.y), 0));

    // select the bit for this pixel depending on its offset in the 4-pixel block
    uint r, g, b;
    int block_offset = xoff & 3;
    if (block_offset == 0)
    {
      r = s0 & 0xFFu;
      g = s0 >> 8;
      b = s1 & 0xFFu;
    }
    else if (block_offset == 1)
    {
      r = s1 >> 8;
      g = s2 & 0xFFu;
      b = s2 >> 8;
    }
    else if (block_offset == 2)
    {
      r = s1 & 0xFFu;
      g = s1 >> 8;
      b = s2 & 0xFFu;
    }
    else
    {
      r = s2 >> 8;
      g = s3 & 0xFFu;
      b = s3 >> 8;
    }

    // and normalize
    o_col0 = vec4(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0, 1.0);
  #else
    // load and return
    o_col0 = texelFetch(samp0, u_base_coords.xy + icoords, 0);
  #endif
}
)";

  return ss.str();
}

GPU_HW::HWPrimitive GPU_HW::GetPrimitiveForCommand(RenderCommand rc)
{
  if (rc.primitive == Primitive::Line)
    return rc.polyline ? HWPrimitive::LineStrip : HWPrimitive::Lines;
  else if ((rc.primitive == Primitive::Polygon && rc.quad_polygon) || rc.primitive == Primitive::Rectangle)
    return HWPrimitive::TriangleStrip;
  else
    return HWPrimitive::Triangles;
}

void GPU_HW::InvalidateVRAMReadCache() {}

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

    texture_mode = m_render_state.texture_color_mode;
    if (rc.raw_texture_enable)
      texture_mode |= TextureMode::RawTextureBit;
  }
  else
  {
    m_render_state.SetFromPageAttribute(Truncate16(m_GPUSTAT.bits));
    texture_mode = TextureMode::Disabled;
  }

  // has any state changed which requires a new batch?
  const TransparencyMode transparency_mode =
    rc.transparency_enable ? m_render_state.transparency_mode : TransparencyMode::Disabled;
  const HWPrimitive rc_primitive = GetPrimitiveForCommand(rc);
  const bool dithering_enable = rc.IsDitheringEnabled() ? m_GPUSTAT.dither_enable : false;
  if (!IsFlushed())
  {
    const u32 max_added_vertices = num_vertices + 2;
    const bool buffer_overflow = (m_batch.vertices.size() + max_added_vertices) >= MAX_BATCH_VERTEX_COUNT;
    if (buffer_overflow || rc_primitive == HWPrimitive::LineStrip || m_batch.texture_mode != texture_mode ||
        m_batch.transparency_mode != transparency_mode || m_batch.primitive != rc_primitive ||
        dithering_enable != m_batch.dithering || m_render_state.IsTexturePageChanged() ||
        m_render_state.IsTextureWindowChanged())
    {
      FlushRender();
    }
  }

  // update state
  m_batch.primitive = rc_primitive;
  m_batch.texture_mode = texture_mode;
  m_batch.transparency_mode = transparency_mode;
  m_batch.dithering = dithering_enable;

  if (m_render_state.IsTexturePageChanged())
  {
    // we only need to update the copy texture if the render area intersects with the texture page
    const u32 texture_page_left = m_render_state.texture_page_x;
    const u32 texture_page_right = m_render_state.texture_page_y + TEXTURE_PAGE_WIDTH;
    const u32 texture_page_top = m_render_state.texture_page_y;
    const u32 texture_page_bottom = texture_page_top + TEXTURE_PAGE_HEIGHT;
    const bool texture_page_overlaps =
      (texture_page_left < m_drawing_area.right && texture_page_right > m_drawing_area.left &&
       texture_page_top > m_drawing_area.bottom && texture_page_bottom < m_drawing_area.top);

    // TODO: Check palette too.
    if (texture_page_overlaps)
    {
      Log_DebugPrintf("Invalidating VRAM read cache due to drawing area overlap");
      InvalidateVRAMReadCache();
    }

    m_batch.texture_page_x = m_render_state.texture_page_x;
    m_batch.texture_page_y = m_render_state.texture_page_y;
    m_batch.texture_palette_x = m_render_state.texture_palette_x;
    m_batch.texture_palette_y = m_render_state.texture_palette_y;
    m_render_state.ClearTexturePageChangedFlag();
  }

  if (m_render_state.IsTextureWindowChanged())
  {
    m_batch.texture_window_values[0] = m_render_state.texture_window_mask_x;
    m_batch.texture_window_values[1] = m_render_state.texture_window_mask_y;
    m_batch.texture_window_values[2] = m_render_state.texture_window_offset_x;
    m_batch.texture_window_values[3] = m_render_state.texture_window_offset_y;
    m_render_state.ClearTextureWindowChangedFlag();
  }

  LoadVertices(rc, num_vertices, command_ptr);
}
