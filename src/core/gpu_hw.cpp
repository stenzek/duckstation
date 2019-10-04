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

void GPU_HW::LoadVertices(RenderCommand rc, u32 num_vertices)
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
        hw_vert.color = (shaded && i > 0) ? (m_GP0_command[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;

        const VertexPosition vp{m_GP0_command[buffer_pos++]};
        hw_vert.x = vp.x;
        hw_vert.y = vp.y;
        hw_vert.texpage = texpage;

        if (textured)
          hw_vert.texcoord = Truncate16(m_GP0_command[buffer_pos++]);
        else
          hw_vert.texcoord = 0;

        hw_vert.padding = 0;

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
      const bool textured = rc.texture_enable;
      const u32 color = rc.color_for_first_vertex;
      const VertexPosition vp{m_GP0_command[buffer_pos++]};
      const s32 pos_left = vp.x;
      const s32 pos_top = vp.y;
      const auto [tex_left, tex_top] =
        HWVertex::DecodeTexcoord(rc.texture_enable ? Truncate16(m_GP0_command[buffer_pos++]) : 0);
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
          rectangle_width = static_cast<s32>(m_GP0_command[buffer_pos] & UINT32_C(0xFFFF));
          rectangle_height = static_cast<s32>(m_GP0_command[buffer_pos] >> 16);
          break;
      }

      // TODO: This should repeat the texcoords instead of stretching
      const s32 pos_right = pos_left + rectangle_width;
      const s32 pos_bottom = pos_top + rectangle_height;
      const u8 tex_right = static_cast<u8>(tex_left + (rectangle_width - 1));
      const u8 tex_bottom = static_cast<u8>(tex_top + (rectangle_height - 1));

      m_batch.vertices.push_back(
        HWVertex{pos_left, pos_top, color, texpage, HWVertex::EncodeTexcoord(tex_left, tex_top)});
      if (restart_strip)
        m_batch.vertices.push_back(m_batch.vertices.back());
      m_batch.vertices.push_back(
        HWVertex{pos_right, pos_top, color, texpage, HWVertex::EncodeTexcoord(tex_right, tex_top)});
      m_batch.vertices.push_back(
        HWVertex{pos_left, pos_bottom, color, texpage, HWVertex::EncodeTexcoord(tex_left, tex_bottom)});
      m_batch.vertices.push_back(
        HWVertex{pos_right, pos_bottom, color, texpage, HWVertex::EncodeTexcoord(tex_right, tex_bottom)});
    }
    break;

    case Primitive::Line:
    {
      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;

      u32 buffer_pos = 1;
      for (u32 i = 0; i < num_vertices; i++)
      {
        const u32 color = (shaded && i > 0) ? (m_GP0_command[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;
        const VertexPosition vp{m_GP0_command[buffer_pos++]};
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
  *left = m_drawing_area.left * s32(m_resolution_scale);
  *right = (m_drawing_area.right + 1) * s32(m_resolution_scale);
  *top = m_drawing_area.top * s32(m_resolution_scale);
  *bottom = (m_drawing_area.bottom + 1) * s32(m_resolution_scale);
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
in vec2 a_tex0;
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
    v_tex0 = a_tex0;

    // base_x,base_y,palette_x,palette_y
    v_texpage.x = (a_texpage & 15) * 64;
    v_texpage.y = ((a_texpage >> 4) & 1) * 256;
    v_texpage.z = ((a_texpage >> 16) & 63) * 16;
    v_texpage.w = ((a_texpage >> 22) & 511);
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW::GenerateFragmentShader(bool transparent, bool textured, TextureColorMode texture_color_mode,
                                           bool blending)
{
  std::stringstream ss;
  GenerateShaderHeader(ss);
  DefineMacro(ss, "TRANSPARENT", transparent);
  DefineMacro(ss, "TEXTURED", textured);
  DefineMacro(ss, "PALETTE",
              textured && (texture_color_mode == GPU::TextureColorMode::Palette4Bit ||
                           texture_color_mode == GPU::TextureColorMode::Palette8Bit));
  DefineMacro(ss, "PALETTE_4_BIT", textured && texture_color_mode == GPU::TextureColorMode::Palette4Bit);
  DefineMacro(ss, "PALETTE_8_BIT", textured && texture_color_mode == GPU::TextureColorMode::Palette8Bit);
  DefineMacro(ss, "BLENDING", blending);

  ss << R"(
in vec3 v_col0;
uniform vec2 u_transparent_alpha;
#if TEXTURED
  in vec2 v_tex0;
  flat in ivec4 v_texpage;
  uniform sampler2D samp0;
#endif

out vec4 o_col0;

#if TEXTURED
vec4 SampleFromVRAM(vec2 coord)
{
  // from 0..1 to 0..255
  ivec2 icoord = ivec2(coord * vec2(255.0));

  // adjust for tightly packed palette formats
  ivec2 index_coord = icoord;
  #if PALETTE_4_BIT
    index_coord.x /= 4;
  #elif PALETTE_8_BIT
    index_coord.x /= 2;
  #endif

  // fixup coords
  ivec2 vicoord = ivec2((v_texpage.x + index_coord.x) * RESOLUTION_SCALE,
                        fixYCoord((v_texpage.y + index_coord.y) * RESOLUTION_SCALE));

  // load colour/palette
  vec4 color = texelFetch(samp0, vicoord, 0);

  // apply palette
  #if PALETTE
    #if PALETTE_4_BIT
      int subpixel = int(icoord.x) & 3;
      uint vram_value = RGBA8ToRGBA5551(color);
      int palette_index = int((vram_value >> (subpixel * 4)) & 0x0Fu);
    #elif PALETTE_8_BIT
      int subpixel = int(icoord.x) & 1;
      uint vram_value = RGBA8ToRGBA5551(color);
      int palette_index = int((vram_value >> (subpixel * 8)) & 0xFFu);
    #endif
    ivec2 palette_icoord = ivec2((v_texpage.z + palette_index) * RESOLUTION_SCALE,
                                 fixYCoord(v_texpage.w * RESOLUTION_SCALE));
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
    #if BLENDING
      color = vec3((ivec3(v_col0 * 255.0) * ivec3(texcol.rgb * 255.0)) >> 7) / 255.0;
    #else
      color = texcol.rgb;
    #endif

    #if TRANSPARENT
      // Apply semitransparency. If not a semitransparent texel, destination alpha is ignored.
      if (texcol.a != 0)
        o_col0 = vec4(color * u_transparent_alpha.x, u_transparent_alpha.y);
      else
        o_col0 = vec4(color, 0.0);
    #else
      // Mask bit from texture.
      o_col0 = vec4(color, texcol.a);
    #endif
  #else
    #if TRANSPARENT
      o_col0 = vec4(v_col0 * u_transparent_alpha.x, u_transparent_alpha.y);
    #else
      // Mask bit is cleared for untextured polygons.
      o_col0 = vec4(v_col0, 0.0);
    #endif
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

GPU_HW::HWRenderBatch::Primitive GPU_HW::GetPrimitiveForCommand(RenderCommand rc)
{
  if (rc.primitive == Primitive::Line)
    return rc.polyline ? HWRenderBatch::Primitive::LineStrip : HWRenderBatch::Primitive::Lines;
  else if ((rc.primitive == Primitive::Polygon && rc.quad_polygon) || rc.primitive == Primitive::Rectangle)
    return HWRenderBatch::Primitive::TriangleStrip;
  else
    return HWRenderBatch::Primitive::Triangles;
}

void GPU_HW::InvalidateVRAMReadCache() {}

void GPU_HW::DispatchRenderCommand(RenderCommand rc, u32 num_vertices)
{
  if (rc.texture_enable)
  {
    // extract texture lut/page
    switch (rc.primitive)
    {
      case Primitive::Polygon:
      {
        if (rc.shading_enable)
          m_render_state.SetFromPolygonTexcoord(m_GP0_command[2], m_GP0_command[5]);
        else
          m_render_state.SetFromPolygonTexcoord(m_GP0_command[2], m_GP0_command[4]);
      }
      break;

      case Primitive::Rectangle:
      {
        m_render_state.SetFromRectangleTexcoord(m_GP0_command[2]);
        m_render_state.SetFromPageAttribute(Truncate16(m_GPUSTAT.bits));
      }
      break;

      default:
        break;
    }
  }

  // has any state changed which requires a new batch?
  const bool rc_transparency_enable = rc.IsTransparencyEnabled();
  const bool rc_texture_enable = rc.IsTextureEnabled();
  const bool rc_texture_blend_enable = rc.IsTextureBlendingEnabled();
  const HWRenderBatch::Primitive rc_primitive = GetPrimitiveForCommand(rc);
  const u32 max_added_vertices = num_vertices + 2;
  const bool buffer_overflow = (m_batch.vertices.size() + max_added_vertices) >= MAX_BATCH_VERTEX_COUNT;
  const bool rc_changed =
    m_batch.render_command_bits != rc.bits && m_batch.transparency_enable != rc_transparency_enable ||
    m_batch.texture_enable != rc_texture_enable || m_batch.texture_blending_enable != rc_texture_blend_enable ||
    m_batch.primitive != rc_primitive;
  const bool restart_line_strip = (rc_primitive == HWRenderBatch::Primitive::LineStrip);
  const bool needs_flush =
    !IsFlushed() && (m_render_state.IsTextureColorModeChanged() || m_render_state.IsTransparencyModeChanged() ||
                     buffer_overflow || rc_changed || restart_line_strip);
  if (needs_flush)
    FlushRender();

  // update state
  if (rc_changed)
  {
    m_batch.render_command_bits = rc.bits;
    m_batch.primitive = rc_primitive;
    m_batch.transparency_enable = rc_transparency_enable;
    m_batch.texture_enable = rc_texture_enable;
    m_batch.texture_blending_enable = rc_texture_blend_enable;
  }

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

  if (m_render_state.IsTextureColorModeChanged())
  {
    m_batch.texture_color_mode = m_render_state.texture_color_mode;
    m_render_state.ClearTextureColorModeChangedFlag();
  }

  if (m_render_state.IsTransparencyModeChanged())
  {
    m_batch.transparency_mode = m_render_state.transparency_mode;
    m_render_state.ClearTransparencyModeChangedFlag();
  }

  LoadVertices(rc, num_vertices);
}
