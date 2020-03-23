#include "gpu_hw_shadergen.h"
#include "common/assert.h"
#include "common/log.h"
#include <cstdio>
#include <glad.h>
Log_SetChannel(GPU_HW_ShaderGen);

GPU_HW_ShaderGen::GPU_HW_ShaderGen(HostDisplay::RenderAPI render_api, u32 resolution_scale, bool true_color,
                                   bool scaled_dithering, bool texture_filtering, bool supports_dual_source_blend)
  : m_render_api(render_api), m_resolution_scale(resolution_scale), m_true_color(true_color),
    m_scaled_dithering(scaled_dithering), m_texture_filering(texture_filtering),
    m_glsl(render_api != HostDisplay::RenderAPI::D3D11), m_glsl_es(render_api == HostDisplay::RenderAPI::OpenGLES),
    m_supports_dual_source_blend(supports_dual_source_blend)
{
  if (m_glsl)
    SetGLSLVersionString();
}

GPU_HW_ShaderGen::~GPU_HW_ShaderGen() = default;

static void DefineMacro(std::stringstream& ss, const char* name, bool enabled)
{
  ss << "#define " << name << " " << BoolToUInt32(enabled) << "\n";
}

void GPU_HW_ShaderGen::SetGLSLVersionString()
{
  const char* glsl_version = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
  Assert(glsl_version != nullptr);

  // Skip any strings in front of the version code.
  const char* glsl_version_start = glsl_version;
  while (*glsl_version_start != '\0' && (*glsl_version_start < '0' || *glsl_version_start > '9'))
    glsl_version_start++;

  int major_version = 0, minor_version = 0;
  if (std::sscanf(glsl_version_start, "%d.%d", &major_version, &minor_version) == 2)
  {
    // Cap at GLSL 3.3, we're not using anything newer for now.
    if (!m_glsl_es && major_version >= 4)
    {
      major_version = 3;
      minor_version = 30;
    }
    else if (m_glsl_es && (major_version > 3 || minor_version > 20))
    {
      major_version = 3;
      minor_version = 20;
    }
  }
  else
  {
    Log_ErrorPrintf("Invalid GLSL version string: '%s' ('%s')", glsl_version, glsl_version_start);
    if (m_glsl_es)
    {
      major_version = 3;
      minor_version = 0;
    }
    m_glsl_version_string = m_glsl_es ? "300" : "130";
  }

  char buf[128];
  std::snprintf(buf, sizeof(buf), "#version %d%02d%s", major_version, minor_version,
                (m_glsl_es && major_version >= 3) ? " es" : "");
  m_glsl_version_string = buf;
}

void GPU_HW_ShaderGen::WriteHeader(std::stringstream& ss)
{
  if (m_render_api == HostDisplay::RenderAPI::OpenGL || m_render_api == HostDisplay::RenderAPI::OpenGLES)
    ss << m_glsl_version_string << "\n\n";

  DefineMacro(ss, "API_OPENGL", m_render_api == HostDisplay::RenderAPI::OpenGL);
  DefineMacro(ss, "API_OPENGL_ES", m_render_api == HostDisplay::RenderAPI::OpenGLES);
  DefineMacro(ss, "API_D3D11", m_render_api == HostDisplay::RenderAPI::D3D11);

  if (m_render_api == HostDisplay::RenderAPI::OpenGLES)
  {
    ss << "precision highp float;\n";
    ss << "precision highp int;\n";
    ss << "precision highp sampler2D;\n";

    if (GLAD_GL_ES_VERSION_3_2)
      ss << "precision highp usamplerBuffer;\n";

    ss << "\n";
  }

  if (m_glsl)
  {
    ss << "#define GLSL 1\n";
    ss << "#define float2 vec2\n";
    ss << "#define float3 vec3\n";
    ss << "#define float4 vec4\n";
    ss << "#define int2 ivec2\n";
    ss << "#define int3 ivec3\n";
    ss << "#define int4 ivec4\n";
    ss << "#define uint2 uvec2\n";
    ss << "#define uint3 uvec3\n";
    ss << "#define uint4 uvec4\n";
    ss << "#define nointerpolation flat\n";
    ss << "#define frac fract\n";
    ss << "#define lerp mix\n";

    ss << "#define CONSTANT const\n";
    ss << "#define VECTOR_EQ(a, b) ((a) == (b))\n";
    ss << "#define VECTOR_NEQ(a, b) ((a) != (b))\n";
    ss << "#define SAMPLE_TEXTURE(name, coords) texture(name, coords)\n";
    ss << "#define LOAD_TEXTURE(name, coords, mip) texelFetch(name, coords, mip)\n";
    ss << "#define LOAD_TEXTURE_OFFSET(name, coords, mip, offset) texelFetchOffset(name, coords, mip, offset)\n";
    ss << "#define LOAD_TEXTURE_BUFFER(name, index) texelFetch(name, index)\n";
  }
  else
  {
    ss << "#define HLSL 1\n";
    ss << "#define CONSTANT static const\n";
    ss << "#define VECTOR_EQ(a, b) (all((a) == (b)))\n";
    ss << "#define VECTOR_NEQ(a, b) (any((a) != (b)))\n";
    ss << "#define SAMPLE_TEXTURE(name, coords) name.Sample(name##_ss, coords)\n";
    ss << "#define LOAD_TEXTURE(name, coords, mip) name.Load(int3(coords, mip))\n";
    ss << "#define LOAD_TEXTURE_OFFSET(name, coords, mip, offset) name.Load(int3(coords, mip), offset)\n";
    ss << "#define LOAD_TEXTURE_BUFFER(name, index) name.Load(index)\n";
  }

  ss << "\n";
}

void GPU_HW_ShaderGen::WriteCommonFunctions(std::stringstream& ss)
{
  ss << "CONSTANT int RESOLUTION_SCALE = " << m_resolution_scale << ";\n";
  ss << "CONSTANT int2 VRAM_SIZE = int2(" << GPU::VRAM_WIDTH << ", " << GPU::VRAM_HEIGHT << ") * RESOLUTION_SCALE;\n";
  ss << "CONSTANT float2 RCP_VRAM_SIZE = float2(1.0, 1.0) / float2(VRAM_SIZE);\n";
  ss << R"(

float fixYCoord(float y)
{
#if API_OPENGL || API_OPENGL_ES
  return 1.0 - RCP_VRAM_SIZE.y - y;
#else
  return y;
#endif
}

int fixYCoord(int y)
{
#if API_OPENGL || API_OPENGL_ES
  return VRAM_SIZE.y - y - 1;
#else
  return y;
#endif
}

uint RGBA8ToRGBA5551(float4 v)
{
  uint r = uint(v.r * 255.0) >> 3;
  uint g = uint(v.g * 255.0) >> 3;
  uint b = uint(v.b * 255.0) >> 3;
  uint a = (v.a != 0.0) ? 1u : 0u;
  return (r) | (g << 5) | (b << 10) | (a << 15);
}

float4 RGBA5551ToRGBA8(uint v)
{
  uint r = (v & 31u);
  uint g = ((v >> 5) & 31u);
  uint b = ((v >> 10) & 31u);
  uint a = ((v >> 15) & 1u);

  // repeat lower bits
  r = (r << 3) | (r & 7u);
  g = (g << 3) | (g & 7u);
  b = (b << 3) | (b & 7u);

  return float4(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0, float(a));
}
)";
}

void GPU_HW_ShaderGen::DeclareUniformBuffer(std::stringstream& ss, const std::initializer_list<const char*>& members)
{
  if (m_glsl_es)
  {
    for (const char* member : members)
      ss << "uniform " << member << ";\n";
  }
  else
  {
    if (m_glsl)
      ss << "layout(std140) uniform UBOBlock\n";
    else
      ss << "cbuffer UBOBlock : register(b0)\n";

    ss << "{\n";
    for (const char* member : members)
      ss << member << ";\n";
    ss << "};\n\n";
  }
}

void GPU_HW_ShaderGen::DeclareTexture(std::stringstream& ss, const char* name, u32 index)
{
  if (m_glsl)
  {
    ss << "uniform sampler2D " << name << ";\n";
  }
  else
  {
    ss << "Texture2D " << name << " : register(t" << index << ");\n";
    ss << "SamplerState " << name << "_ss : register(s" << index << ");\n";
  }
}

void GPU_HW_ShaderGen::DeclareTextureBuffer(std::stringstream& ss, const char* name, u32 index, bool is_int,
                                            bool is_unsigned)
{
  if (m_glsl)
  {
    ss << "uniform " << (is_int ? (is_unsigned ? "u" : "i") : "") << "samplerBuffer " << name << ";\n";
  }
  else
  {
    ss << "Buffer<" << (is_int ? (is_unsigned ? "uint4" : "int4") : "float4") << "> " << name << " : register(t"
       << index << ");\n";
  }
}

void GPU_HW_ShaderGen::DeclareVertexEntryPoint(std::stringstream& ss,
                                               const std::initializer_list<const char*>& attributes,
                                               u32 num_color_outputs, u32 num_texcoord_outputs,
                                               const std::initializer_list<const char*>& additional_outputs,
                                               bool declare_vertex_id)
{
  if (m_glsl)
  {
    for (const char* attribute : attributes)
      ss << "in " << attribute << ";\n";

    for (u32 i = 0; i < num_color_outputs; i++)
      ss << "out float4 v_col" << i << ";\n";

    for (u32 i = 0; i < num_texcoord_outputs; i++)
      ss << "out float2 v_tex" << i << ";\n";

    for (const char* output : additional_outputs)
      ss << output << ";\n";

    ss << "#define v_pos gl_Position\n\n";
    if (declare_vertex_id)
      ss << "#define v_id uint(gl_VertexID)\n";

    ss << "\n";
    ss << "void main()\n";
  }
  else
  {
    ss << "void main(\n";

    u32 attribute_counter = 0;
    for (const char* attribute : attributes)
    {
      ss << "  in " << attribute << " : ATTR" << attribute_counter << ",\n";
      attribute_counter++;
    }

    if (declare_vertex_id)
      ss << "  in uint v_id : SV_VertexID,\n";

    for (u32 i = 0; i < num_color_outputs; i++)
      ss << "  out float4 v_col" << i << " : COLOR" << i << ",\n";

    for (u32 i = 0; i < num_texcoord_outputs; i++)
      ss << "  out float2 v_tex" << i << " : TEXCOORD" << i << ",\n";

    u32 additional_counter = num_texcoord_outputs;
    for (const char* output : additional_outputs)
    {
      ss << "  " << output << " : TEXCOORD" << additional_counter << ",\n";
      additional_counter++;
    }

    ss << "  out float4 v_pos : SV_Position)\n";
  }
}

void GPU_HW_ShaderGen::DeclareFragmentEntryPoint(std::stringstream& ss, u32 num_color_inputs, u32 num_texcoord_inputs,
                                                 const std::initializer_list<const char*>& additional_inputs,
                                                 bool declare_fragcoord, bool dual_color_output)
{
  if (m_glsl)
  {
    for (u32 i = 0; i < num_color_inputs; i++)
      ss << "in float4 v_col" << i << ";\n";

    for (u32 i = 0; i < num_texcoord_inputs; i++)
      ss << "in float2 v_tex" << i << ";\n";

    for (const char* input : additional_inputs)
      ss << input << ";\n";

    if (declare_fragcoord)
      ss << "#define v_pos gl_FragCoord\n";

    ss << "out float4 o_col0;\n";
    if (dual_color_output)
      ss << "out float4 o_col1;\n";

    ss << "\n";

    ss << "void main()\n";
  }
  else
  {
    {
      ss << "void main(\n";

      for (u32 i = 0; i < num_color_inputs; i++)
        ss << "  in float4 v_col" << i << " : COLOR" << i << ",\n";

      for (u32 i = 0; i < num_texcoord_inputs; i++)
        ss << "  in float2 v_tex" << i << " : TEXCOORD" << i << ",\n";

      u32 additional_counter = num_texcoord_inputs;
      for (const char* output : additional_inputs)
      {
        ss << "  " << output << " : TEXCOORD" << additional_counter << ",\n";
        additional_counter++;
      }

      if (declare_fragcoord)
        ss << "  in float4 v_pos : SV_Position,\n";

      if (dual_color_output)
      {
        ss << "  out float4 o_col0 : SV_Target0,\n";
        ss << "  out float4 o_col1 : SV_Target1)\n";
      }
      else
      {
        ss << "  out float4 o_col0 : SV_Target)";
      }
    }
  }
}

void GPU_HW_ShaderGen::WriteBatchUniformBuffer(std::stringstream& ss)
{
  DeclareUniformBuffer(ss, {"int2 u_pos_offset", "uint2 u_texture_window_mask", "uint2 u_texture_window_offset",
                            "float u_src_alpha_factor", "float u_dst_alpha_factor", "bool u_set_mask_while_drawing"});
}

std::string GPU_HW_ShaderGen::GenerateBatchVertexShader(bool textured)
{
  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "TEXTURED", textured);

  WriteCommonFunctions(ss);
  WriteBatchUniformBuffer(ss);

  if (textured)
  {
    DeclareVertexEntryPoint(ss, {"int2 a_pos", "float4 a_col0", "int a_texcoord", "int a_texpage"}, 1, 1,
                            {"nointerpolation out int4 v_texpage"});
  }
  else
  {
    DeclareVertexEntryPoint(ss, {"int2 a_pos", "float4 a_col0"}, 1, 0, {});
  }

  ss << R"(
{
  // 0..+1023 -> -1..1
  float pos_x = (float(a_pos.x + u_pos_offset.x) / 512.0) - 1.0;
  float pos_y = (float(a_pos.y + u_pos_offset.y) / -256.0) + 1.0;
  v_pos = float4(pos_x, pos_y, 0.0, 1.0);

  v_col0 = a_col0;
  #if TEXTURED
    // Fudge the texture coordinates by half a pixel in screen-space.
    // This fixes the rounding/interpolation error on NVIDIA GPUs with shared edges between triangles.
    v_tex0 = float2(float(a_texcoord & 0xFFFF) + (RCP_VRAM_SIZE.x * 0.5),
                    float(a_texcoord >> 16) + (RCP_VRAM_SIZE.y * 0.5));

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
  const bool textured = (texture_mode != GPU::TextureMode::Disabled);
  const bool use_dual_source = m_supports_dual_source_blend &&
                               (transparency != GPU_HW::BatchRenderMode::TransparencyDisabled || m_texture_filering);

  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "TRANSPARENCY", transparency != GPU_HW::BatchRenderMode::TransparencyDisabled);
  DefineMacro(ss, "TRANSPARENCY_ONLY_OPAQUE", transparency == GPU_HW::BatchRenderMode::OnlyOpaque);
  DefineMacro(ss, "TRANSPARENCY_ONLY_TRANSPARENCY", transparency == GPU_HW::BatchRenderMode::OnlyTransparent);
  DefineMacro(ss, "TEXTURED", textured);
  DefineMacro(ss, "PALETTE",
              actual_texture_mode == GPU::TextureMode::Palette4Bit ||
                actual_texture_mode == GPU::TextureMode::Palette8Bit);
  DefineMacro(ss, "PALETTE_4_BIT", actual_texture_mode == GPU::TextureMode::Palette4Bit);
  DefineMacro(ss, "PALETTE_8_BIT", actual_texture_mode == GPU::TextureMode::Palette8Bit);
  DefineMacro(ss, "RAW_TEXTURE", raw_texture);
  DefineMacro(ss, "DITHERING", dithering);
  DefineMacro(ss, "DITHERING_SCALED", m_scaled_dithering);
  DefineMacro(ss, "TRUE_COLOR", m_true_color);
  DefineMacro(ss, "TEXTURE_FILTERING", m_texture_filering);
  DefineMacro(ss, "USE_DUAL_SOURCE", use_dual_source);

  WriteCommonFunctions(ss);
  WriteBatchUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0);

  if (m_glsl)
    ss << "CONSTANT int[16] s_dither_values = int[16]( ";
  else
    ss << "CONSTANT int s_dither_values[] = {";
  for (u32 i = 0; i < 16; i++)
  {
    if (i > 0)
      ss << ", ";
    ss << GPU::DITHER_MATRIX[i / 4][i % 4];
  }
  if (m_glsl)
    ss << " );\n";
  else
    ss << "};\n";

  ss << R"(
int3 ApplyDithering(int2 coord, int3 icol)
{
  int2 fc = coord & int2(3, 3);
  int offset = s_dither_values[fc.y * 4 + fc.x];
  return icol + int3(offset, offset, offset);
}

int3 TruncateTo15Bit(int3 icol)
{
  icol = clamp(icol, int3(0, 0, 0), int3(255, 255, 255));
  return (icol & int3(~7, ~7, ~7)) | ((icol >> 3) & int3(7, 7, 7));
}

#if TEXTURED
CONSTANT float4 TRANSPARENT_PIXEL_COLOR = float4(0.0, 0.0, 0.0, 0.0);

int2 ApplyTextureWindow(int2 coords)
{
  uint x = (uint(coords.x) & ~(u_texture_window_mask.x * 8u)) | ((u_texture_window_offset.x & u_texture_window_mask.x) * 8u);
  uint y = (uint(coords.y) & ~(u_texture_window_mask.y * 8u)) | ((u_texture_window_offset.y & u_texture_window_mask.y) * 8u);
  return int2(int(x), int(y));
}  

float4 SampleFromVRAM(int4 texpage, int2 icoord)
{
  icoord = ApplyTextureWindow(icoord);

  // adjust for tightly packed palette formats
  int2 index_coord = icoord;
  #if PALETTE_4_BIT
    index_coord.x /= 4;
  #elif PALETTE_8_BIT
    index_coord.x /= 2;
  #endif

  // fixup coords
  int2 vicoord = int2(texpage.x + index_coord.x * RESOLUTION_SCALE, fixYCoord(texpage.y + index_coord.y * RESOLUTION_SCALE));

  // load colour/palette
  float4 color = LOAD_TEXTURE(samp0, vicoord, 0);

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
    int2 palette_icoord = int2(texpage.z + (palette_index * RESOLUTION_SCALE), fixYCoord(texpage.w));
    color = LOAD_TEXTURE(samp0, palette_icoord, 0);
  #endif

  return color;
}
#endif
)";

  if (textured)
  {
    DeclareFragmentEntryPoint(ss, 1, 1, {"nointerpolation in int4 v_texpage"}, true, use_dual_source);
  }
  else
  {
    DeclareFragmentEntryPoint(ss, 1, 0, {}, true, use_dual_source);
  }

  ss << R"(
{
  int3 vertcol = int3(v_col0.rgb * float3(255.0, 255.0, 255.0));

  bool semitransparent;
  int3 icolor;
  float ialpha;
  float oalpha;

  #if TEXTURED
    #if TEXTURE_FILTERING
      int2 icoord = int2(v_tex0);
      float2 pcoord = frac(v_tex0) - float2(0.5, 0.5);
      float2 poffs = sign(pcoord);
      pcoord = abs(pcoord);

      // TODO: Clamp to page
      float4 tl = SampleFromVRAM(v_texpage, int2(v_tex0));
      float4 tr = SampleFromVRAM(v_texpage, int2(min(v_tex0.x + poffs.x, 255.0), v_tex0.y));
      float4 bl = SampleFromVRAM(v_texpage, int2(v_tex0.x, min(v_tex0.y + poffs.y, 255.0)));
      float4 br = SampleFromVRAM(v_texpage, int2(min(v_tex0.x + poffs.x, 255.0), min(v_tex0.y + poffs.y, 255.0)));

      // Compute alpha from how many texels aren't pixel color 0000h.
      float tl_a = float(VECTOR_NEQ(tl, TRANSPARENT_PIXEL_COLOR));
      float tr_a = float(VECTOR_NEQ(tr, TRANSPARENT_PIXEL_COLOR));
      float bl_a = float(VECTOR_NEQ(bl, TRANSPARENT_PIXEL_COLOR));
      float br_a = float(VECTOR_NEQ(br, TRANSPARENT_PIXEL_COLOR));

      // Bilinearly interpolate.
      float4 texcol = lerp(lerp(tl, tr, pcoord.x), lerp(bl, br, pcoord.x), pcoord.y);
      ialpha = lerp(lerp(tl_a, tr_a, pcoord.x), lerp(bl_a, br_a, pcoord.x), pcoord.y);
      if (ialpha == 0.0)
        discard;

      texcol.rgb /= float3(ialpha, ialpha, ialpha);
      semitransparent = (texcol.a != 0.0);
    #else
      float4 texcol = SampleFromVRAM(v_texpage, int2(v_tex0));
      if (VECTOR_EQ(texcol, TRANSPARENT_PIXEL_COLOR))
        discard;

      semitransparent = (texcol.a != 0.0);
      ialpha = 1.0;
    #endif

    #if RAW_TEXTURE
      icolor = int3(texcol.rgb * float3(255.0, 255.0, 255.0));
    #else
      icolor = (vertcol * int3(texcol.rgb * float3(255.0, 255.0, 255.0))) >> 7;
    #endif

    // Compute output alpha (mask bit)
    oalpha = float(u_set_mask_while_drawing ? 1 : int(semitransparent));
  #else
    // All pixels are semitransparent for untextured polygons.
    semitransparent = true;
    icolor = vertcol;
    ialpha = 1.0;

    // However, the mask bit is cleared if set mask bit is false.
    oalpha = float(u_set_mask_while_drawing);
  #endif

  // Apply dithering
  #if DITHERING
    #if DITHERING_SCALED
      icolor = ApplyDithering(int2(v_pos.xy), icolor);
    #else
      icolor = ApplyDithering(int2(v_pos.xy) / int2(RESOLUTION_SCALE, RESOLUTION_SCALE), icolor);
    #endif
  #endif

  // Clip to 15-bit range
  #if !TRUE_COLOR
    icolor = TruncateTo15Bit(icolor);
  #endif

  // Normalize
  float3 color = float3(icolor) / float3(255.0, 255.0, 255.0);

  #if TRANSPARENCY
    // Apply semitransparency. If not a semitransparent texel, destination alpha is ignored.
    if (semitransparent)
    {
      #if TRANSPARENCY_ONLY_OPAQUE
        discard;
      #endif

      #if USE_DUAL_SOURCE
        o_col0 = float4(color * (u_src_alpha_factor * ialpha), oalpha);
        o_col1 = float4(0.0, 0.0, 0.0, u_dst_alpha_factor / ialpha);
      #else
        o_col0 = float4(color * (u_src_alpha_factor * ialpha), u_dst_alpha_factor / ialpha);
      #endif
    }
    else
    {
      #if TRANSPARENCY_ONLY_TRANSPARENCY
        discard;
      #endif

      #if USE_DUAL_SOURCE
        o_col0 = float4(color * ialpha, oalpha);
        o_col1 = float4(0.0, 0.0, 0.0, 0.0);
      #else
        o_col0 = float4(color * ialpha, 1.0 - ialpha);
      #endif
    }
  #else
    // Non-transparency won't enable blending so we can write the mask here regardless.
    o_col0 = float4(color * ialpha, oalpha);

    #if USE_DUAL_SOURCE
      o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
    #endif
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateBatchLineExpandGeometryShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteCommonFunctions(ss);

  // GS is a pain, too different between HLSL and GLSL...
  if (m_glsl)
  {
  }
  else
  {
    ss << R"(
CONSTANT float2 WIDTH = (1.0 / float2(VRAM_SIZE)) * float2(RESOLUTION_SCALE, RESOLUTION_SCALE);

struct Vertex
{
  float4 col0 : COLOR0;
  float4 pos : SV_Position;
};

[maxvertexcount(4)]
void main(line Vertex input[2], inout TriangleStream<Vertex> output)
{
  Vertex v;

  float2 dir = normalize(input[1].pos.xy - input[0].pos.xy);
  float2 normal = cross(float3(dir, 0.0), float3(0.0, 0.0, 1.0)).xy * WIDTH;
  float4 offset = float4(normal, 0.0, 0.0);

  // top-left
  v.col0 = input[0].col0;
  v.pos = input[0].pos - offset;
  output.Append(v);

  // top-right
  v.col0 = input[0].col0;
  v.pos = input[0].pos + offset;
  output.Append(v);

  // bottom-left
  v.col0 = input[1].col0;
  v.pos = input[1].pos - offset;
  output.Append(v);

  // bottom-right
  v.col0 = input[1].col0;
  v.pos = input[1].pos + offset;
  output.Append(v);

  output.RestartStrip();
}
)";
  }

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateScreenQuadVertexShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareVertexEntryPoint(ss, {}, 0, 1, {}, true);
  ss << R"(
{
  v_tex0 = float2(float((v_id << 1) & 2u), float(v_id & 2u));
  v_pos = float4(v_tex0 * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  #if API_OPENGL || API_OPENGL_ES
    v_pos.y = -gl_Position.y;
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateFillFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"float4 u_fill_color"});
  DeclareFragmentEntryPoint(ss, 0, 1, {}, false, false);

  ss << R"(
{
  o_col0 = u_fill_color;
}
)";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateCopyFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"float4 u_src_rect"});
  DeclareTexture(ss, "samp0", 0);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, false, false);

  ss << R"(
{
    float2 coords = u_src_rect.xy + v_tex0 * u_src_rect.zw;
    o_col0 = SAMPLE_TEXTURE(samp0, coords);
}
)";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateDisplayFragmentShader(bool depth_24bit, bool interlaced)
{
  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "DEPTH_24BIT", depth_24bit);
  DefineMacro(ss, "INTERLACED", interlaced);

  WriteCommonFunctions(ss);
  DeclareUniformBuffer(ss, {"int3 u_base_coords"});
  DeclareTexture(ss, "samp0", 0);

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, false);
  ss << R"(
{
  int2 icoords = int2(v_pos.xy);

  #if INTERLACED
    if (((icoords.y - u_base_coords.z) & 1) != 0)
      discard;
  #endif

  #if DEPTH_24BIT
    // compute offset in dwords from the start of the 24-bit values
    int2 base = int2(u_base_coords.x, u_base_coords.y + icoords.y);
    int xoff = int(icoords.x);
    int dword_index = (xoff / 2) + (xoff / 4);

    // sample two adjacent dwords, or four 16-bit values as the 24-bit value will lie somewhere between these
    uint s0 = RGBA8ToRGBA5551(LOAD_TEXTURE(samp0, int2(base.x + dword_index * 2 + 0, base.y), 0));
    uint s1 = RGBA8ToRGBA5551(LOAD_TEXTURE(samp0, int2(base.x + dword_index * 2 + 1, base.y), 0));
    uint s2 = RGBA8ToRGBA5551(LOAD_TEXTURE(samp0, int2(base.x + (dword_index + 1) * 2 + 0, base.y), 0));
    uint s3 = RGBA8ToRGBA5551(LOAD_TEXTURE(samp0, int2(base.x + (dword_index + 1) * 2 + 1, base.y), 0));

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
    o_col0 = float4(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0, 1.0);
  #else
    // load and return
    o_col0 = LOAD_TEXTURE(samp0, u_base_coords.xy + icoords, 0);
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMReadFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteCommonFunctions(ss);
  DeclareUniformBuffer(ss, {"int2 u_base_coords", "int2 u_size"});

  DeclareTexture(ss, "samp0", 0);

  ss << R"(
uint SampleVRAM(int2 coords)
{
  if (RESOLUTION_SCALE == 1)
    return RGBA8ToRGBA5551(LOAD_TEXTURE(samp0, coords, 0));

  // Box filter for downsampling.
  float4 value = float4(0.0, 0.0, 0.0, 0.0);
  int2 base_coords = coords * int2(RESOLUTION_SCALE, RESOLUTION_SCALE);
  for (int offset_x = 0; offset_x < RESOLUTION_SCALE; offset_x++)
  {
    for (int offset_y = 0; offset_y < RESOLUTION_SCALE; offset_y++)
      value += LOAD_TEXTURE(samp0, base_coords + int2(offset_x, offset_y), 0);
  }
  value /= float(RESOLUTION_SCALE * RESOLUTION_SCALE);
  return RGBA8ToRGBA5551(value);
}
)";

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, false);
  ss << R"(
{
  int2 sample_coords = int2(int(v_pos.x) * 2, int(v_pos.y));

  #if API_OPENGL || API_OPENGL_ES
    // Lower-left origin flip for OpenGL.
    // We want to write the image out upside-down so we can read it top-to-bottom.
    sample_coords.y = u_size.y - sample_coords.y - 1;
  #endif

  sample_coords += u_base_coords;

  // We're encoding as 32-bit, so the output width is halved and we pack two 16-bit pixels in one 32-bit pixel.
  uint left = SampleVRAM(sample_coords);
  uint right = SampleVRAM(int2(sample_coords.x + 1, sample_coords.y));

  o_col0 = float4(float(left & 0xFFu), float((left >> 8) & 0xFFu),
                  float(right & 0xFFu), float((right >> 8) & 0xFFu))
            / float4(255.0, 255.0, 255.0, 255.0);
})";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMWriteFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteCommonFunctions(ss);
  DeclareUniformBuffer(ss, {"int2 u_base_coords", "int2 u_size", "int u_buffer_base_offset"});

  DeclareTextureBuffer(ss, "samp0", 0, true, true);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, false);
  ss << R"(
{
  int2 coords = int2(v_pos.xy) / int2(RESOLUTION_SCALE, RESOLUTION_SCALE);
  int2 offset = coords - u_base_coords;

  #if API_OPENGL || API_OPENGL_ES
    // Lower-left origin flip for OpenGL
    offset.y = u_size.y - offset.y - 1;
  #endif

  int buffer_offset = u_buffer_base_offset + (offset.y * u_size.x) + offset.x;
  uint value = LOAD_TEXTURE_BUFFER(samp0, buffer_offset).r;
  
  o_col0 = RGBA5551ToRGBA8(value);
})";

  return ss.str();
}
