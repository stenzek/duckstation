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
    m_glsl(render_api != HostDisplay::RenderAPI::D3D11), m_supports_dual_source_blend(supports_dual_source_blend),
    m_use_glsl_interface_blocks(false)
{
  if (m_glsl)
  {
    SetGLSLVersionString();

    m_use_glsl_interface_blocks = (GLAD_GL_ES_VERSION_3_2 || GLAD_GL_VERSION_3_2);
    m_use_glsl_binding_layout = UseGLSLBindingLayout();
  }
}

GPU_HW_ShaderGen::~GPU_HW_ShaderGen() = default;

bool GPU_HW_ShaderGen::UseGLSLBindingLayout()
{
  return (GLAD_GL_ES_VERSION_3_1 || GLAD_GL_VERSION_4_2 ||
          (GLAD_GL_ARB_explicit_attrib_location && GLAD_GL_ARB_explicit_uniform_location &&
           GLAD_GL_ARB_shading_language_420pack));
}

static void DefineMacro(std::stringstream& ss, const char* name, bool enabled)
{
  ss << "#define " << name << " " << BoolToUInt32(enabled) << "\n";
}

void GPU_HW_ShaderGen::SetGLSLVersionString()
{
  const char* glsl_version = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
  const bool glsl_es = (m_render_api == HostDisplay::RenderAPI::OpenGLES);
  Assert(glsl_version != nullptr);

  // Skip any strings in front of the version code.
  const char* glsl_version_start = glsl_version;
  while (*glsl_version_start != '\0' && (*glsl_version_start < '0' || *glsl_version_start > '9'))
    glsl_version_start++;

  int major_version = 0, minor_version = 0;
  if (std::sscanf(glsl_version_start, "%d.%d", &major_version, &minor_version) == 2)
  {
    // Cap at GLSL 4.3, we're not using anything newer for now.
    if (!glsl_es && (major_version > 4 || (major_version == 4 && minor_version > 30)))
    {
      major_version = 4;
      minor_version = 30;
    }
    else if (glsl_es && (major_version > 3 || (major_version == 3 && minor_version > 20)))
    {
      major_version = 3;
      minor_version = 20;
    }
  }
  else
  {
    Log_ErrorPrintf("Invalid GLSL version string: '%s' ('%s')", glsl_version, glsl_version_start);
    if (glsl_es)
    {
      major_version = 3;
      minor_version = 0;
    }
    m_glsl_version_string = glsl_es ? "300" : "130";
  }

  char buf[128];
  std::snprintf(buf, sizeof(buf), "#version %d%02d%s", major_version, minor_version,
                (glsl_es && major_version >= 3) ? " es" : "");
  m_glsl_version_string = buf;
}

void GPU_HW_ShaderGen::WriteHeader(std::stringstream& ss)
{
  if (m_render_api == HostDisplay::RenderAPI::OpenGL || m_render_api == HostDisplay::RenderAPI::OpenGLES)
    ss << m_glsl_version_string << "\n\n";

  // Extension enabling for OpenGL.
  if (m_render_api == HostDisplay::RenderAPI::OpenGLES)
  {
    // Enable EXT_blend_func_extended for dual-source blend on OpenGL ES.
    if (GLAD_GL_EXT_blend_func_extended)
      ss << "#extension GL_EXT_blend_func_extended : require\n";
  }
  else if (m_render_api == HostDisplay::RenderAPI::OpenGL)
  {
    // Need extensions for binding layout if GL<4.3.
    if (m_use_glsl_binding_layout && !GLAD_GL_VERSION_4_3)
    {
      ss << "#extension GL_ARB_explicit_attrib_location : require\n";
      ss << "#extension GL_ARB_explicit_uniform_location : require\n";
      ss << "#extension GL_ARB_shading_language_420pack : require\n";
    }

    if (!GLAD_GL_VERSION_3_2)
      ss << "#extension GL_ARB_uniform_buffer_object : require\n";
  }

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
    ss << "#define VECTOR_LT(a, b) (any(lessThan((a), (b))))\n";
    ss << "#define VECTOR_LE(a, b) (any(lessThanEqual((a), (b))))\n";
    ss << "#define VECTOR_GT(a, b) (any(greaterThan((a), (b))))\n";
    ss << "#define VECTOR_GE(a, b) (any(greaterThanEqual((a), (b))))\n";
    ss << "#define SAMPLE_TEXTURE(name, coords) texture(name, coords)\n";
    ss << "#define LOAD_TEXTURE(name, coords, mip) texelFetch(name, coords, mip)\n";
    ss << "#define LOAD_TEXTURE_OFFSET(name, coords, mip, offset) texelFetchOffset(name, coords, mip, offset)\n";
    ss << "#define LOAD_TEXTURE_BUFFER(name, index) texelFetch(name, index)\n";
  }
  else
  {
    ss << "#define HLSL 1\n";
    ss << "#define roundEven round\n";
    ss << "#define CONSTANT static const\n";
    ss << "#define VECTOR_EQ(a, b) (all((a) == (b)))\n";
    ss << "#define VECTOR_NEQ(a, b) (any((a) != (b)))\n";
    ss << "#define VECTOR_LT(a, b) (any((a) < (b)))\n";
    ss << "#define VECTOR_LE(a, b) (any((a) <= (b)))\n";
    ss << "#define VECTOR_GT(a, b) (any((a) > (b)))\n";
    ss << "#define VECTOR_GE(a, b) (any((a) >= (b)))\n";
    ss << "#define SAMPLE_TEXTURE(name, coords) name.Sample(name##_ss, coords)\n";
    ss << "#define LOAD_TEXTURE(name, coords, mip) name.Load(int3(coords, mip))\n";
    ss << "#define LOAD_TEXTURE_OFFSET(name, coords, mip, offset) name.Load(int3(coords, mip), offset)\n";
    ss << "#define LOAD_TEXTURE_BUFFER(name, index) name.Load(index)\n";
  }

  ss << "\n";
}

void GPU_HW_ShaderGen::WriteCommonFunctions(std::stringstream& ss)
{
  ss << "CONSTANT uint RESOLUTION_SCALE = " << m_resolution_scale << "u;\n";
  ss << "CONSTANT uint2 VRAM_SIZE = uint2(" << GPU::VRAM_WIDTH << ", " << GPU::VRAM_HEIGHT << ") * RESOLUTION_SCALE;\n";
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

uint fixYCoord(uint y)
{
#if API_OPENGL || API_OPENGL_ES
  return VRAM_SIZE.y - y - 1u;
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
  if (m_glsl)
  {
    if (m_use_glsl_binding_layout)
      ss << "layout(std140, binding = 1) uniform UBOBlock\n";
    else
      ss << "layout(std140) uniform UBOBlock\n";
  }
  else
  {
    ss << "cbuffer UBOBlock : register(b0)\n";
  }

  ss << "{\n";
  for (const char* member : members)
    ss << member << ";\n";
  ss << "};\n\n";
}

void GPU_HW_ShaderGen::DeclareTexture(std::stringstream& ss, const char* name, u32 index)
{
  if (m_glsl)
  {
    if (m_use_glsl_binding_layout)
      ss << "layout(binding = " << index << ") ";

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
    if (m_use_glsl_binding_layout)
      ss << "layout(binding = " << index << ") ";

    ss << "uniform " << (is_int ? (is_unsigned ? "u" : "i") : "") << "samplerBuffer " << name << ";\n";
  }
  else
  {
    ss << "Buffer<" << (is_int ? (is_unsigned ? "uint4" : "int4") : "float4") << "> " << name << " : register(t"
       << index << ");\n";
  }
}

void GPU_HW_ShaderGen::DeclareVertexEntryPoint(
  std::stringstream& ss, const std::initializer_list<const char*>& attributes, u32 num_color_outputs,
  u32 num_texcoord_outputs, const std::initializer_list<std::pair<const char*, const char*>>& additional_outputs,
  bool declare_vertex_id)
{
  if (m_glsl)
  {
    if (m_use_glsl_binding_layout)
    {
      u32 attribute_counter = 0;
      for (const char* attribute : attributes)
      {
        ss << "layout(location = " << attribute_counter << ") in " << attribute << ";\n";
        attribute_counter++;
      }
    }
    else
    {
      for (const char* attribute : attributes)
        ss << "in " << attribute << ";\n";
    }

    if (m_use_glsl_interface_blocks)
    {
      ss << "out VertexData {\n";
      for (u32 i = 0; i < num_color_outputs; i++)
        ss << "  float4 v_col" << i << ";\n";

      for (u32 i = 0; i < num_texcoord_outputs; i++)
        ss << "  float2 v_tex" << i << ";\n";

      for (const auto [qualifiers, name] : additional_outputs)
        ss << "  " << qualifiers << " " << name << ";\n";
      ss << "};\n";
    }
    else
    {
      for (u32 i = 0; i < num_color_outputs; i++)
        ss << "out float4 v_col" << i << ";\n";

      for (u32 i = 0; i < num_texcoord_outputs; i++)
        ss << "out float2 v_tex" << i << ";\n";

      for (const auto [qualifiers, name] : additional_outputs)
        ss << qualifiers << " out " << name << ";\n";
    }

    ss << "#define v_pos gl_Position\n\n";
    if (declare_vertex_id)
      ss << "#define v_id uint(gl_VertexID)\n";

    ss << "\n";
    ss << "void main()\n";
  }
  else
  {
    ss << "void main(\n";

    if (declare_vertex_id)
      ss << "  in uint v_id : SV_VertexID,\n";

    u32 attribute_counter = 0;
    for (const char* attribute : attributes)
    {
      ss << "  in " << attribute << " : ATTR" << attribute_counter << ",\n";
      attribute_counter++;
    }

    for (u32 i = 0; i < num_color_outputs; i++)
      ss << "  out float4 v_col" << i << " : COLOR" << i << ",\n";

    for (u32 i = 0; i < num_texcoord_outputs; i++)
      ss << "  out float2 v_tex" << i << " : TEXCOORD" << i << ",\n";

    u32 additional_counter = num_texcoord_outputs;
    for (const auto [qualifiers, name] : additional_outputs)
    {
      ss << "  " << qualifiers << " out " << name << " : TEXCOORD" << additional_counter << ",\n";
      additional_counter++;
    }

    ss << "  out float4 v_pos : SV_Position)\n";
  }
}

void GPU_HW_ShaderGen::DeclareFragmentEntryPoint(
  std::stringstream& ss, u32 num_color_inputs, u32 num_texcoord_inputs,
  const std::initializer_list<std::pair<const char*, const char*>>& additional_inputs,
  bool declare_fragcoord /* = false */, u32 num_color_outputs /* = 1 */, bool depth_output /* = false */)
{
  if (m_glsl)
  {
    if (m_use_glsl_interface_blocks)
    {
      ss << "in VertexData {\n";
      for (u32 i = 0; i < num_color_inputs; i++)
        ss << "  float4 v_col" << i << ";\n";

      for (u32 i = 0; i < num_texcoord_inputs; i++)
        ss << "  float2 v_tex" << i << ";\n";

      for (const auto [qualifiers, name] : additional_inputs)
        ss << "  " << qualifiers << " " << name << ";\n";
      ss << "};\n";
    }
    else
    {
      for (u32 i = 0; i < num_color_inputs; i++)
        ss << "in float4 v_col" << i << ";\n";

      for (u32 i = 0; i < num_texcoord_inputs; i++)
        ss << "in float2 v_tex" << i << ";\n";

      for (const auto [qualifiers, name] : additional_inputs)
        ss << qualifiers << " in " << name << ";\n";
    }

    if (declare_fragcoord)
      ss << "#define v_pos gl_FragCoord\n";

    if (depth_output)
      ss << "#define o_depth gl_FragDepth\n";

    if (m_use_glsl_binding_layout)
    {
      if (m_supports_dual_source_blend)
      {
        for (u32 i = 0; i < num_color_outputs; i++)
          ss << "layout(location = 0, index = " << i << ") out float4 o_col" << i << ";\n";
      }
      else
      {
        Assert(num_color_outputs <= 1);
        for (u32 i = 0; i < num_color_outputs; i++)
          ss << "layout(location = 0" << i << ") out float4 o_col" << i << ";\n";
      }
    }
    else
    {
      for (u32 i = 0; i < num_color_outputs; i++)
        ss << "out float4 o_col" << i << ";\n";
    }

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
      for (const auto [qualifiers, name] : additional_inputs)
      {
        ss << "  " << qualifiers << " in " << name << " : TEXCOORD" << additional_counter << ",\n";
        additional_counter++;
      }

      if (declare_fragcoord)
        ss << "  in float4 v_pos : SV_Position,\n";

      if (depth_output)
      {
        ss << "  out float o_depth : SV_Depth";
        if (num_color_outputs > 0)
          ss << ",\n";
        else
          ss << ")\n";
      }

      for (u32 i = 0; i < num_color_outputs; i++)
      {
        ss << "  out float4 o_col" << i << " : SV_Target" << i;

        if (i == (num_color_outputs - 1))
          ss << ")\n";
        else
          ss << ",\n";
      }
    }
  }
}

void GPU_HW_ShaderGen::WriteBatchUniformBuffer(std::stringstream& ss)
{
  DeclareUniformBuffer(ss,
                       {"uint2 u_texture_window_mask", "uint2 u_texture_window_offset", "float u_src_alpha_factor",
                        "float u_dst_alpha_factor", "uint u_interlaced_displayed_field", "uint u_base_vertex_depth_id",
                        "bool u_check_mask_before_draw", "bool u_set_mask_while_drawing"});
}

std::string GPU_HW_ShaderGen::GenerateBatchVertexShader(bool textured)
{
  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "TEXTURED", textured);

  WriteCommonFunctions(ss);
  WriteBatchUniformBuffer(ss);

  ss << "CONSTANT float EPSILON = 0.00001;\n";

  if (textured)
  {
    DeclareVertexEntryPoint(ss, {"int2 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage"}, 1, 1,
                            {{"nointerpolation", "uint4 v_texpage"}, {"nointerpolation", "float v_depth"}}, true);
  }
  else
  {
    DeclareVertexEntryPoint(ss, {"int2 a_pos", "float4 a_col0"}, 1, 0, {{"nointerpolation", "float v_depth"}}, true);
  }

  ss << R"(
{
  // Offset the vertex position by 0.5 to ensure correct interpolation of texture coordinates
  // at 1x resolution scale. This doesn't work at >1x, we adjust the texture coordinates before
  // uploading there instead.
  float vertex_offset = (RESOLUTION_SCALE == 1u) ? 0.5 : 0.0;

  // 0..+1023 -> -1..1
  float pos_x = ((float(a_pos.x) + vertex_offset) / 512.0) - 1.0;
  float pos_y = ((float(a_pos.y) + vertex_offset) / -256.0) + 1.0;

  // OpenGL seems to be off by one pixel in the Y direction due to lower-left origin, but only on
  // Intel and NVIDIA drivers. AMD is fine...
#if API_OPENGL || API_OPENGL_ES
  pos_y += EPSILON;
#endif
  v_pos = float4(pos_x, pos_y, 0.0, 1.0);

#if API_D3D11
  v_depth = 1.0 - (float(u_base_vertex_depth_id + (u_check_mask_before_draw ? 0u : v_id)) / 65535.0);
#else
  v_depth = 1.0 - (float(v_id - u_base_vertex_depth_id) / 65535.0);
#endif

  v_col0 = a_col0;
  #if TEXTURED
    // Fudge the texture coordinates by half a pixel in screen-space.
    // This fixes the rounding/interpolation error on NVIDIA GPUs with shared edges between triangles.
    v_tex0 = float2(float(a_texcoord & 0xFFFFu) + EPSILON, float(a_texcoord >> 16) + EPSILON);

    // base_x,base_y,palette_x,palette_y
    v_texpage.x = (a_texpage & 15u) * 64u * RESOLUTION_SCALE;
    v_texpage.y = ((a_texpage >> 4) & 1u) * 256u * RESOLUTION_SCALE;
    v_texpage.z = ((a_texpage >> 16) & 63u) * 16u * RESOLUTION_SCALE;
    v_texpage.w = ((a_texpage >> 22) & 511u) * RESOLUTION_SCALE;
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateBatchFragmentShader(GPU_HW::BatchRenderMode transparency,
                                                          GPU::TextureMode texture_mode, bool dithering,
                                                          bool interlacing)
{
  const GPU::TextureMode actual_texture_mode = texture_mode & ~GPU::TextureMode::RawTextureBit;
  const bool raw_texture = (texture_mode & GPU::TextureMode::RawTextureBit) == GPU::TextureMode::RawTextureBit;
  const bool textured = (texture_mode != GPU::TextureMode::Disabled);
  const bool use_dual_source =
    m_supports_dual_source_blend && ((transparency != GPU_HW::BatchRenderMode::TransparencyDisabled &&
                                      transparency != GPU_HW::BatchRenderMode::OnlyOpaque) ||
                                     m_texture_filering);

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
  DefineMacro(ss, "INTERLACING", interlacing);
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
uint3 ApplyDithering(uint2 coord, uint3 icol)
{
  #if DITHERING_SCALED
    uint2 fc = coord & uint2(3u, 3u);
  #else
    uint2 fc = (coord / uint2(RESOLUTION_SCALE, RESOLUTION_SCALE)) & uint2(3u, 3u);
  #endif
  int offset = s_dither_values[fc.y * 4u + fc.x];

  #if !TRUE_COLOR
    return uint3(clamp((int3(icol) + int3(offset, offset, offset)) >> 3, 0, 31));
  #else
    return uint3(clamp(int3(icol) + int3(offset, offset, offset), 0, 255));
  #endif
}

#if TEXTURED
CONSTANT float4 TRANSPARENT_PIXEL_COLOR = float4(0.0, 0.0, 0.0, 0.0);

uint2 ApplyTextureWindow(uint2 coords)
{
  uint x = (uint(coords.x) & ~(u_texture_window_mask.x * 8u)) | ((u_texture_window_offset.x & u_texture_window_mask.x) * 8u);
  uint y = (uint(coords.y) & ~(u_texture_window_mask.y * 8u)) | ((u_texture_window_offset.y & u_texture_window_mask.y) * 8u);
  return uint2(x, y);
}  

float4 SampleFromVRAM(uint4 texpage, uint2 icoord)
{
  icoord = ApplyTextureWindow(icoord);

  // adjust for tightly packed palette formats
  uint2 index_coord = icoord;
  #if PALETTE_4_BIT
    index_coord.x /= 4u;
  #elif PALETTE_8_BIT
    index_coord.x /= 2u;
  #endif

  // fixup coords
  uint2 vicoord = uint2(texpage.x + index_coord.x * RESOLUTION_SCALE, fixYCoord(texpage.y + index_coord.y * RESOLUTION_SCALE));

  // load colour/palette
  float4 color = LOAD_TEXTURE(samp0, int2(vicoord), 0);

  // apply palette
  #if PALETTE
    #if PALETTE_4_BIT
      uint subpixel = icoord.x & 3u;
      uint vram_value = RGBA8ToRGBA5551(color);
      uint palette_index = (vram_value >> (subpixel * 4u)) & 0x0Fu;
    #elif PALETTE_8_BIT
      uint subpixel = icoord.x & 1u;
      uint vram_value = RGBA8ToRGBA5551(color);
      uint palette_index = (vram_value >> (subpixel * 8u)) & 0xFFu;
    #endif
    uint2 palette_icoord = uint2(texpage.z + (palette_index * RESOLUTION_SCALE), fixYCoord(texpage.w));
    color = LOAD_TEXTURE(samp0, int2(palette_icoord), 0);
  #endif

  return color;
}
#endif
)";

  if (textured)
  {
    DeclareFragmentEntryPoint(ss, 1, 1, {{"nointerpolation", "uint4 v_texpage"}, {"nointerpolation", "float v_depth"}},
                              true, use_dual_source ? 2 : 1, true);
  }
  else
  {
    DeclareFragmentEntryPoint(ss, 1, 0, {{"nointerpolation", "float v_depth"}}, true, use_dual_source ? 2 : 1, true);
  }

  ss << R"(
{
  uint3 vertcol = uint3(v_col0.rgb * float3(255.0, 255.0, 255.0));

  bool semitransparent;
  uint3 icolor;
  float ialpha;
  float oalpha;

  #if INTERLACING
    if (((fixYCoord(uint(v_pos.y)) / RESOLUTION_SCALE) & 1u) == u_interlaced_displayed_field)
      discard;
  #endif

  #if TEXTURED
    #if TEXTURE_FILTERING
      // Compute the coordinates of the four texels we will be interpolating between.
      // TODO: Find some way to clamp this to the triangle texture coordinates?
      float2 texel_top_left = frac(v_tex0) - float2(0.5, 0.5);
      float2 texel_offset = sign(texel_top_left);
      float4 fcoords = max(v_tex0.xyxy + float4(0.0, 0.0, texel_offset.x, texel_offset.y),
                           float4(0.0, 0.0, 0.0, 0.0));

      // Load four texels.
      float4 s00 = SampleFromVRAM(v_texpage, uint2(fcoords.xy));
      float4 s10 = SampleFromVRAM(v_texpage, uint2(fcoords.zy));
      float4 s01 = SampleFromVRAM(v_texpage, uint2(fcoords.xw));
      float4 s11 = SampleFromVRAM(v_texpage, uint2(fcoords.zw));

      // Compute alpha from how many texels aren't pixel color 0000h.
      float a00 = float(VECTOR_NEQ(s00, TRANSPARENT_PIXEL_COLOR));
      float a10 = float(VECTOR_NEQ(s10, TRANSPARENT_PIXEL_COLOR));
      float a01 = float(VECTOR_NEQ(s01, TRANSPARENT_PIXEL_COLOR));
      float a11 = float(VECTOR_NEQ(s11, TRANSPARENT_PIXEL_COLOR));

      // Bilinearly interpolate.
      float2 weights = abs(texel_top_left);
      float4 texcol = lerp(lerp(s00, s10, weights.x), lerp(s01, s11, weights.x), weights.y);
      ialpha = lerp(lerp(a00, a10, weights.x), lerp(a01, a11, weights.x), weights.y);
      if (ialpha < 0.5)
        discard;

      texcol.rgb /= float3(ialpha, ialpha, ialpha);
      semitransparent = (texcol.a != 0.0);
    #else
      // With the vertex offset applied at 1x resolution scale, we want to round the texture coordinates.
      // Floor them otherwise, as it currently breaks when upscaling as the vertex offset is not applied.
      float4 texcol = SampleFromVRAM(v_texpage, uint2((RESOLUTION_SCALE == 1u) ? roundEven(v_tex0) : floor(v_tex0)));
      if (VECTOR_EQ(texcol, TRANSPARENT_PIXEL_COLOR))
        discard;

      semitransparent = (texcol.a != 0.0);
      ialpha = 1.0;
    #endif

    // If not using true color, truncate the framebuffer colors to 5-bit.
    #if !TRUE_COLOR
      icolor = uint3(texcol.rgb * float3(255.0, 255.0, 255.0)) >> 3;
      #if !RAW_TEXTURE
        icolor = (icolor * vertcol) >> 4;
        #if DITHERING
          icolor = ApplyDithering(uint2(v_pos.xy), icolor);
        #else
          icolor = min(icolor >> 3, uint3(31u, 31u, 31u));
        #endif
      #endif
    #else
      icolor = uint3(texcol.rgb * float3(255.0, 255.0, 255.0));
      #if !RAW_TEXTURE
        icolor = (icolor * vertcol) >> 7;
        #if DITHERING
          icolor = ApplyDithering(uint2(v_pos.xy), icolor);
        #else
          icolor = min(icolor, uint3(255u, 255u, 255u));
        #endif
      #endif
    #endif

    // Compute output alpha (mask bit)
    oalpha = float(u_set_mask_while_drawing ? 1 : int(semitransparent));
  #else
    // All pixels are semitransparent for untextured polygons.
    semitransparent = true;
    icolor = vertcol;
    ialpha = 1.0;

    #if DITHERING
      icolor = ApplyDithering(uint2(v_pos.xy), icolor);
    #else
      #if !TRUE_COLOR
        icolor >>= 3;
      #endif
    #endif

    // However, the mask bit is cleared if set mask bit is false.
    oalpha = float(u_set_mask_while_drawing);
  #endif

  // Premultiply alpha so we don't need to use a colour output for it.
  float premultiply_alpha = ialpha;
  #if TRANSPARENCY
    premultiply_alpha = ialpha * (semitransparent ? u_src_alpha_factor : 1.0);
  #endif

  float3 color;
  #if !TRUE_COLOR
    // We want to apply the alpha before the truncation to 16-bit, otherwise we'll be passing a 32-bit precision color
    // into the blend unit, which can cause a small amount of error to accumulate.
    color = floor(float3(icolor) * premultiply_alpha) / float3(31.0, 31.0, 31.0);
  #else
    // True color is actually simpler here since we want to preserve the precision.
    color = (float3(icolor) * premultiply_alpha) / float3(255.0, 255.0, 255.0);
  #endif

  #if TRANSPARENCY
    // Apply semitransparency. If not a semitransparent texel, destination alpha is ignored.
    if (semitransparent)
    {
      #if TRANSPARENCY_ONLY_OPAQUE
        discard;
      #endif

      #if USE_DUAL_SOURCE
        o_col0 = float4(color, oalpha);
        o_col1 = float4(0.0, 0.0, 0.0, u_dst_alpha_factor / ialpha);
      #else
        o_col0 = float4(color, u_dst_alpha_factor / ialpha);
      #endif

      o_depth = oalpha * v_depth;
    }
    else
    {
      #if TRANSPARENCY_ONLY_TRANSPARENCY
        discard;
      #endif

      #if TRANSPARENCY_ONLY_OPAQUE
        // We don't output the second color here because it's not used.
        o_col0 = float4(color, oalpha);
      #elif USE_DUAL_SOURCE
        o_col0 = float4(color, oalpha);
        o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
      #else
        o_col0 = float4(color, 1.0 - ialpha);
      #endif

      o_depth = oalpha * v_depth;
    }
  #else
    // Non-transparency won't enable blending so we can write the mask here regardless.
    o_col0 = float4(color, oalpha);

    #if USE_DUAL_SOURCE
      o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
    #endif

    o_depth = oalpha * v_depth;
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

  ss << R"(
CONSTANT float2 WIDTH = (1.0 / float2(VRAM_SIZE)) * float2(RESOLUTION_SCALE, RESOLUTION_SCALE);
)";

  // GS is a pain, too different between HLSL and GLSL...
  if (m_glsl)
  {
    ss << R"(
in VertexData {
  float4 v_col0;
  nointerpolation float v_depth;
} in_data[];

out VertexData {
  float4 v_col0;
  nointerpolation float v_depth;
} out_data;

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

void main() {
  float2 dir = normalize(gl_in[1].gl_Position.xy - gl_in[0].gl_Position.xy);
  float2 normal = cross(float3(dir, 0.0), float3(0.0, 0.0, 1.0)).xy * WIDTH;
  float4 offset = float4(normal, 0.0, 0.0);

  // top-left
  out_data.v_col0 = in_data[0].v_col0;
  out_data.v_depth = in_data[0].v_depth;
  gl_Position = gl_in[0].gl_Position - offset;
  EmitVertex();

  // top-right
  out_data.v_col0 = in_data[0].v_col0;
  out_data.v_depth = in_data[0].v_depth;
  gl_Position = gl_in[0].gl_Position + offset;
  EmitVertex();

  // bottom-left
  out_data.v_col0 = in_data[1].v_col0;
  out_data.v_depth = in_data[1].v_depth;
  gl_Position = gl_in[1].gl_Position - offset;
  EmitVertex();

  // bottom-right
  out_data.v_col0 = in_data[1].v_col0;
  out_data.v_depth = in_data[1].v_depth;
  gl_Position = gl_in[1].gl_Position + offset;
  EmitVertex();

  EndPrimitive();
}
)";
  }
  else
  {
    ss << R"(
struct Vertex
{
  float4 col0 : COLOR0;
  float depth : TEXCOORD0;
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
  v.depth = input[0].depth;
  v.pos = input[0].pos - offset;
  output.Append(v);

  // top-right
  v.col0 = input[0].col0;
  v.depth = input[0].depth;
  v.pos = input[0].pos + offset;
  output.Append(v);

  // bottom-left
  v.col0 = input[1].col0;
  v.depth = input[1].depth;
  v.pos = input[1].pos - offset;
  output.Append(v);

  // bottom-right
  v.col0 = input[1].col0;
  v.depth = input[1].depth;
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
  DeclareFragmentEntryPoint(ss, 0, 1, {}, false, 1, true);

  ss << R"(
{
  o_col0 = u_fill_color;
  o_depth = u_fill_color.a;
}
)";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateInterlacedFillFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteCommonFunctions(ss);
  DeclareUniformBuffer(ss, {"float4 u_fill_color", "uint u_interlaced_displayed_field"});
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 1, true);

  ss << R"(
{
  if (((fixYCoord(uint(v_pos.y)) / RESOLUTION_SCALE) & 1u) == u_interlaced_displayed_field)
    discard;

  o_col0 = u_fill_color;
  o_depth = u_fill_color.a;
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
  DeclareFragmentEntryPoint(ss, 0, 1, {}, false, 1);

  ss << R"(
{
  float2 coords = u_src_rect.xy + v_tex0 * u_src_rect.zw;
  o_col0 = SAMPLE_TEXTURE(samp0, coords);
}
)";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateDisplayFragmentShader(bool depth_24bit, GPU_HW::InterlacedRenderMode interlace_mode)
{
  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "DEPTH_24BIT", depth_24bit);
  DefineMacro(ss, "INTERLACED", interlace_mode != GPU_HW::InterlacedRenderMode::None);
  DefineMacro(ss, "INTERLEAVED", interlace_mode == GPU_HW::InterlacedRenderMode::InterleavedFields);

  WriteCommonFunctions(ss);
  DeclareUniformBuffer(ss, {"uint2 u_vram_offset", "uint u_crop_left", "uint u_field_offset"});
  DeclareTexture(ss, "samp0", 0);

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 1);
  ss << R"(
{
  uint2 icoords = uint2(v_pos.xy);

  #if INTERLACED
    if (((icoords.y / RESOLUTION_SCALE) & 1u) != u_field_offset)
      discard;

    #if !INTERLEAVED
      icoords.y /= 2u;
    #endif
  #endif

  #if DEPTH_24BIT
    // relative to start of scanout
    uint relative_x = (icoords.x + u_crop_left) / RESOLUTION_SCALE;
    uint2 vram_coords = u_vram_offset + uint2(((relative_x * 3u) / 2u) * RESOLUTION_SCALE, icoords.y);

    // load adjacent 16-bit texels
    uint s0 = RGBA8ToRGBA5551(LOAD_TEXTURE(samp0, int2(vram_coords % VRAM_SIZE), 0));
    uint s1 = RGBA8ToRGBA5551(LOAD_TEXTURE(samp0, int2((vram_coords + uint2(RESOLUTION_SCALE, 0)) % VRAM_SIZE), 0));
    
    // select which part of the combined 16-bit texels we are currently shading
    uint s1s0 = ((s1 << 16) | s0) >> ((relative_x & 1u) * 8u);
    
    // extract components and normalize
    o_col0 = float4(float(s1s0 & 0xFFu) / 255.0, float((s1s0 >> 8u) & 0xFFu) / 255.0,
                    float((s1s0 >> 16u) & 0xFFu) / 255.0, 1.0);
  #else
    // load and return
    uint2 vram_coords = u_vram_offset + uint2(icoords.x + u_crop_left, icoords.y);
    o_col0 = LOAD_TEXTURE(samp0, int2(vram_coords % VRAM_SIZE), 0);
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
  DeclareUniformBuffer(ss, {"uint2 u_base_coords", "uint2 u_size"});

  DeclareTexture(ss, "samp0", 0);

  ss << R"(
uint SampleVRAM(uint2 coords)
{
  if (RESOLUTION_SCALE == 1u)
    return RGBA8ToRGBA5551(LOAD_TEXTURE(samp0, int2(coords), 0));

  // Box filter for downsampling.
  float4 value = float4(0.0, 0.0, 0.0, 0.0);
  uint2 base_coords = coords * uint2(RESOLUTION_SCALE, RESOLUTION_SCALE);
  for (uint offset_x = 0u; offset_x < RESOLUTION_SCALE; offset_x++)
  {
    for (uint offset_y = 0u; offset_y < RESOLUTION_SCALE; offset_y++)
      value += LOAD_TEXTURE(samp0, int2(base_coords + uint2(offset_x, offset_y)), 0);
  }
  value /= float(RESOLUTION_SCALE * RESOLUTION_SCALE);
  return RGBA8ToRGBA5551(value);
}
)";

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 1);
  ss << R"(
{
  uint2 sample_coords = uint2(uint(v_pos.x) * 2u, uint(v_pos.y));

  #if API_OPENGL || API_OPENGL_ES
    // Lower-left origin flip for OpenGL.
    // We want to write the image out upside-down so we can read it top-to-bottom.
    sample_coords.y = u_size.y - sample_coords.y - 1u;
  #endif

  sample_coords += u_base_coords;

  // We're encoding as 32-bit, so the output width is halved and we pack two 16-bit pixels in one 32-bit pixel.
  uint left = SampleVRAM(sample_coords);
  uint right = SampleVRAM(uint2(sample_coords.x + 1u, sample_coords.y));

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
  DeclareUniformBuffer(ss, {"uint2 u_base_coords", "uint2 u_size", "uint u_buffer_base_offset", "uint u_mask_or_bits",
                            "float u_depth_value"});

  DeclareTextureBuffer(ss, "samp0", 0, true, true);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 1, true);
  ss << R"(
{
  uint2 coords = uint2(v_pos.xy) / uint2(RESOLUTION_SCALE, RESOLUTION_SCALE);
  uint2 offset = coords - u_base_coords;

  #if API_OPENGL || API_OPENGL_ES
    // Lower-left origin flip for OpenGL
    offset.y = u_size.y - offset.y - 1u;
  #endif

  uint buffer_offset = u_buffer_base_offset + (offset.y * u_size.x) + offset.x;
  uint value = LOAD_TEXTURE_BUFFER(samp0, int(buffer_offset)).r | u_mask_or_bits;
  
  o_col0 = RGBA5551ToRGBA8(value);
  o_depth = (o_col0.a == 1.0) ? u_depth_value : 0.0;
})";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMCopyFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteCommonFunctions(ss);
  DeclareUniformBuffer(ss, {"uint2 u_src_coords", "uint2 u_dst_coords", "uint2 u_end_coords", "uint2 u_size",
                            "bool u_set_mask_bit", "float u_depth_value"});

  DeclareTexture(ss, "samp0", 0);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 1, true);
  ss << R"(
{
  uint2 dst_coords = uint2(v_pos.xy);

  // make sure it's not oversized and out of range
  if (VECTOR_LT(dst_coords, u_dst_coords) && VECTOR_GE(dst_coords, u_end_coords))
    discard;

  // find offset from the start of the row/column
  uint2 offset;
  offset.x = (dst_coords.x < u_dst_coords.x) ? ((VRAM_SIZE.x - 1u) - u_dst_coords.x + dst_coords.x) : (dst_coords.x - u_dst_coords.x);
  offset.y = (dst_coords.y < u_dst_coords.y) ? ((VRAM_SIZE.y - 1u) - u_dst_coords.y + dst_coords.y) : (dst_coords.y - u_dst_coords.y);

  // find the source coordinates to copy from
  uint2 src_coords = (u_src_coords + offset) % VRAM_SIZE;

  // sample and apply mask bit
  float4 color = LOAD_TEXTURE(samp0, int2(src_coords), 0);
  o_col0 = float4(color.xyz, u_set_mask_bit ? 1.0 : color.a);
  o_depth = (u_set_mask_bit ? 1.0f : ((o_col0.a == 1.0) ? u_depth_value : 0.0));
})";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMUpdateDepthFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareTexture(ss, "samp0", 0);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 0, true);

  ss << R"(
{
  o_depth = LOAD_TEXTURE(samp0, int2(v_pos.xy), 0).a;
}
)";

  return ss.str();
}
