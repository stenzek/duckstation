#include "gpu_hw_shadergen.h"

GPU_HW_ShaderGen::GPU_HW_ShaderGen(Backend backend, u32 resolution_scale, bool true_color)
  : m_backend(backend), m_resolution_scale(resolution_scale), m_true_color(true_color)
{
}

GPU_HW_ShaderGen::~GPU_HW_ShaderGen() = default;

static void DefineMacro(std::stringstream& ss, const char* name, bool enabled)
{
  if (enabled)
    ss << "#define " << name << " 1\n";
  else
    ss << "/* #define " << name << " 0 */\n";
}

void GPU_HW_ShaderGen::GenerateShaderHeader(std::stringstream& ss)
{
  ss << "#version 330 core\n\n";
  ss << "const int RESOLUTION_SCALE = " << m_resolution_scale << ";\n";
  ss << "const ivec2 VRAM_SIZE = ivec2(" << GPU::VRAM_WIDTH << ", " << GPU::VRAM_HEIGHT << ") * RESOLUTION_SCALE;\n";
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
  uint r = (v & 31u);
  uint g = ((v >> 5) & 31u);
  uint b = ((v >> 10) & 31u);
  uint a = ((v >> 15) & 1u);

  // repeat lower bits
  r = (r << 3) | (r & 7u);
  g = (g << 3) | (g & 7u);
  b = (b << 3) | (b & 7u);

  return vec4(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0, float(a));
}
)";
}

void GPU_HW_ShaderGen::GenerateBatchUniformBuffer(std::stringstream& ss)
{
  ss << R"(
uniform UBOBlock {
  ivec2 u_pos_offset;
  uvec2 u_texture_window_mask;
  uvec2 u_texture_window_offset;
  float u_src_alpha_factor;
  float u_dst_alpha_factor;
};
)";
}

std::string GPU_HW_ShaderGen::GenerateBatchVertexShader(bool textured)
{
  std::stringstream ss;
  GenerateShaderHeader(ss);
  DefineMacro(ss, "TEXTURED", textured);
  GenerateBatchUniformBuffer(ss);

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

std::string GPU_HW_ShaderGen::GenerateBatchFragmentShader(GPU_HW::BatchRenderMode transparency,
                                                     GPU::TextureMode texture_mode, bool dithering)
{
  const GPU::TextureMode actual_texture_mode = texture_mode & ~GPU::TextureMode::RawTextureBit;
  const bool raw_texture = (texture_mode & GPU::TextureMode::RawTextureBit) == GPU::TextureMode::RawTextureBit;

  std::stringstream ss;
  GenerateShaderHeader(ss);
  GenerateBatchUniformBuffer(ss);
  DefineMacro(ss, "TRANSPARENCY", transparency != GPU_HW::BatchRenderMode::TransparencyDisabled);
  DefineMacro(ss, "TRANSPARENCY_ONLY_OPAQUE", transparency == GPU_HW::BatchRenderMode::OnlyOpaque);
  DefineMacro(ss, "TRANSPARENCY_ONLY_TRANSPARENCY", transparency == GPU_HW::BatchRenderMode::OnlyTransparent);
  DefineMacro(ss, "TEXTURED", actual_texture_mode != GPU::TextureMode::Disabled);
  DefineMacro(ss, "PALETTE",
              actual_texture_mode == GPU::TextureMode::Palette4Bit ||
                actual_texture_mode == GPU::TextureMode::Palette8Bit);
  DefineMacro(ss, "PALETTE_4_BIT", actual_texture_mode == GPU::TextureMode::Palette4Bit);
  DefineMacro(ss, "PALETTE_8_BIT", actual_texture_mode == GPU::TextureMode::Palette8Bit);
  DefineMacro(ss, "RAW_TEXTURE", raw_texture);
  DefineMacro(ss, "DITHERING", dithering);
  DefineMacro(ss, "TRUE_COLOR", m_true_color);

  ss << "const int[16] s_dither_values = int[16]( ";
  for (u32 i = 0; i < 16; i++)
  {
    if (i > 0)
      ss << ", ";
    ss << GPU::DITHER_MATRIX[i / 4][i % 4];
  }
  ss << " );\n";

  ss << R"(
in vec3 v_col0;
#if TEXTURED
  in vec2 v_tex0;
  flat in ivec4 v_texpage;
  uniform sampler2D samp0;
#endif

out vec4 o_col0;

ivec3 ApplyDithering(ivec3 icol)
{
  ivec2 fc = (ivec2(gl_FragCoord.xy) / ivec2(RESOLUTION_SCALE, RESOLUTION_SCALE)) & ivec2(3, 3);
  int offset = s_dither_values[fc.y * 4 + fc.x];
  return icol + ivec3(offset, offset, offset);
}

ivec3 TruncateTo15Bit(ivec3 icol)
{
  icol = clamp(icol, ivec3(0, 0, 0), ivec3(255, 255, 255));
  return (icol & ivec3(~7, ~7, ~7)) | ((icol >> 3) & ivec3(7, 7, 7));
}

#if TEXTURED
ivec2 ApplyNativeTextureWindow(ivec2 coords)
{
  uint x = (uint(coords.x) & ~(u_texture_window_mask.x * 8u)) | ((u_texture_window_offset.x & u_texture_window_mask.x) * 8u);
  uint y = (uint(coords.y) & ~(u_texture_window_mask.y * 8u)) | ((u_texture_window_offset.y & u_texture_window_mask.y) * 8u);
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

ivec4 SampleFromVRAM(vec2 coord)
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

  return ivec4(color * vec4(255.0, 255.0, 255.0, 255.0));
}
#endif

void main()
{
  ivec3 vertcol = ivec3(v_col0 * vec3(255.0, 255.0, 255.0));

  bool semitransparent;
  bool new_mask_bit;
  ivec3 icolor;

  #if TEXTURED
    ivec4 texcol = SampleFromVRAM(v_tex0);
    if (texcol == ivec4(0.0, 0.0, 0.0, 0.0))
      discard;

    // Grab semitransparent bit from the texture color.
    semitransparent = (texcol.a != 0);

    #if RAW_TEXTURE
      icolor = texcol.rgb;
    #else
      icolor = (vertcol * texcol.rgb) >> 7;
    #endif
  #else
    // All pixels are semitransparent for untextured polygons.
    semitransparent = true;
    icolor = vertcol;
  #endif

  // Apply dithering
  #if DITHERING
    icolor = ApplyDithering(icolor);
  #endif

  // Clip to 15-bit range
  #if !TRUE_COLOR
    icolor = TruncateTo15Bit(icolor);
  #endif

  // Normalize
  vec3 color = vec3(icolor) / vec3(255.0, 255.0, 255.0);

  #if TRANSPARENCY
    // Apply semitransparency. If not a semitransparent texel, destination alpha is ignored.
    if (semitransparent)
    {
      #if TRANSPARENCY_ONLY_OPAQUE
        discard;
      #endif
      o_col0 = vec4(color * u_src_alpha_factor, u_dst_alpha_factor);
    }
    else
    {
      #if TRANSPARENCY_ONLY_TRANSPARENCY
        discard;
      #endif
      o_col0 = vec4(color, 0.0);
    }
  #else
    o_col0 = vec4(color, 0.0);
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateScreenQuadVertexShader()
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

std::string GPU_HW_ShaderGen::GenerateFillFragmentShader()
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

std::string GPU_HW_ShaderGen::GenerateDisplayFragmentShader(bool depth_24bit, bool interlaced)
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

std::string GPU_HW_ShaderGen::GenerateVRAMWriteFragmentShader()
{
  std::stringstream ss;
  GenerateShaderHeader(ss);

  ss << R"(

uniform ivec2 u_base_coords;
uniform ivec2 u_size;
uniform usamplerBuffer samp0;

out vec4 o_col0;

void main()
{
  ivec2 coords = ivec2(gl_FragCoord.xy) / ivec2(RESOLUTION_SCALE, RESOLUTION_SCALE);
  ivec2 offset = coords - u_base_coords;
  offset.y = u_size.y - offset.y - 1;

  int buffer_offset = offset.y * u_size.x + offset.x;
  uint value = texelFetch(samp0, buffer_offset).r;
  
  o_col0 = RGBA5551ToRGBA8(value);
})";

  return ss.str();
}
