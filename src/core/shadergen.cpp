#include "shadergen.h"
#include "common/assert.h"
#include "common/gl/loader.h"
#include "common/log.h"
#include <cstdio>
#include <cstring>
Log_SetChannel(ShaderGen);

ShaderGen::ShaderGen(HostDisplay::RenderAPI render_api, bool supports_dual_source_blend)
  : m_render_api(render_api),
    m_glsl(render_api != HostDisplay::RenderAPI::D3D11 && render_api != HostDisplay::RenderAPI::D3D12),
    m_supports_dual_source_blend(supports_dual_source_blend), m_use_glsl_interface_blocks(false)
{
  if (m_glsl)
  {
    if (m_render_api == HostDisplay::RenderAPI::OpenGL || m_render_api == HostDisplay::RenderAPI::OpenGLES)
      SetGLSLVersionString();

    m_use_glsl_interface_blocks = (IsVulkan() || GLAD_GL_ES_VERSION_3_2 || GLAD_GL_VERSION_3_2);
    m_use_glsl_binding_layout = (IsVulkan() || UseGLSLBindingLayout());

    if (m_render_api == HostDisplay::RenderAPI::OpenGL)
    {
      // SSAA with interface blocks is broken on AMD's OpenGL driver.
      const char* gl_vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
      if (std::strcmp(gl_vendor, "ATI Technologies Inc.") == 0)
        m_use_glsl_interface_blocks = false;
    }
  }
}

ShaderGen::~ShaderGen() = default;

bool ShaderGen::UseGLSLBindingLayout()
{
  return (GLAD_GL_ES_VERSION_3_1 || GLAD_GL_VERSION_4_3 ||
          (GLAD_GL_ARB_explicit_attrib_location && GLAD_GL_ARB_explicit_uniform_location &&
           GLAD_GL_ARB_shading_language_420pack));
}

void ShaderGen::DefineMacro(std::stringstream& ss, const char* name, bool enabled)
{
  ss << "#define " << name << " " << BoolToUInt32(enabled) << "\n";
}

void ShaderGen::SetGLSLVersionString()
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

void ShaderGen::WriteHeader(std::stringstream& ss)
{
  if (m_render_api == HostDisplay::RenderAPI::OpenGL || m_render_api == HostDisplay::RenderAPI::OpenGLES)
    ss << m_glsl_version_string << "\n\n";
  else if (m_render_api == HostDisplay::RenderAPI::Vulkan)
    ss << "#version 450 core\n\n";

  // Extension enabling for OpenGL.
  if (m_render_api == HostDisplay::RenderAPI::OpenGLES)
  {
    // Enable EXT_blend_func_extended for dual-source blend on OpenGL ES.
    if (GLAD_GL_EXT_blend_func_extended)
      ss << "#extension GL_EXT_blend_func_extended : require\n";
    if (GLAD_GL_ARB_blend_func_extended)
      ss << "#extension GL_ARB_blend_func_extended : require\n";

    // Test for V3D driver - we have to fudge coordinates slightly.
    if (std::strstr(reinterpret_cast<const char*>(glGetString(GL_VENDOR)), "Broadcom") &&
        std::strstr(reinterpret_cast<const char*>(glGetString(GL_RENDERER)), "V3D"))
    {
      ss << "#define DRIVER_V3D 1\n";
    }
    else if (std::strstr(reinterpret_cast<const char*>(glGetString(GL_RENDERER)), "PowerVR"))
    {
      ss << "#define DRIVER_POWERVR 1\n";
    }
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

    // Enable SSBOs if it's not required by the version.
    if (!GLAD_GL_VERSION_4_3 && !GLAD_GL_ES_VERSION_3_1 && GLAD_GL_ARB_shader_storage_buffer_object)
      ss << "#extension GL_ARB_shader_storage_buffer_object : require\n";
  }

  DefineMacro(ss, "API_OPENGL", m_render_api == HostDisplay::RenderAPI::OpenGL);
  DefineMacro(ss, "API_OPENGL_ES", m_render_api == HostDisplay::RenderAPI::OpenGLES);
  DefineMacro(ss, "API_D3D11", m_render_api == HostDisplay::RenderAPI::D3D11);
  DefineMacro(ss, "API_D3D12", m_render_api == HostDisplay::RenderAPI::D3D12);
  DefineMacro(ss, "API_VULKAN", m_render_api == HostDisplay::RenderAPI::Vulkan);

  if (m_render_api == HostDisplay::RenderAPI::OpenGLES)
  {
    ss << "precision highp float;\n";
    ss << "precision highp int;\n";
    ss << "precision highp sampler2D;\n";

    if (GLAD_GL_ES_VERSION_3_1)
      ss << "precision highp sampler2DMS;\n";

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
    ss << "#define float2x2 mat2\n";
    ss << "#define float3x3 mat3\n";
    ss << "#define float4x4 mat4\n";
    ss << "#define mul(x, y) ((x) * (y))\n";
    ss << "#define nointerpolation flat\n";
    ss << "#define frac fract\n";
    ss << "#define lerp mix\n";

    ss << "#define CONSTANT const\n";
    ss << "#define GLOBAL\n";
    ss << "#define FOR_UNROLL for\n";
    ss << "#define FOR_LOOP for\n";
    ss << "#define IF_BRANCH if\n";
    ss << "#define IF_FLATTEN if\n";
    ss << "#define VECTOR_EQ(a, b) ((a) == (b))\n";
    ss << "#define VECTOR_NEQ(a, b) ((a) != (b))\n";
    ss << "#define VECTOR_COMP_EQ(a, b) equal((a), (b))\n";
    ss << "#define VECTOR_COMP_NEQ(a, b) notEqual((a), (b))\n";
    ss << "#define SAMPLE_TEXTURE(name, coords) texture(name, coords)\n";
    ss << "#define SAMPLE_TEXTURE_OFFSET(name, coords, offset) textureOffset(name, coords, offset)\n";
    ss << "#define SAMPLE_TEXTURE_LEVEL(name, coords, level) textureLod(name, coords, level)\n";
    ss << "#define SAMPLE_TEXTURE_LEVEL_OFFSET(name, coords, level, offset) textureLod(name, coords, level, offset)\n";
    ss << "#define LOAD_TEXTURE(name, coords, mip) texelFetch(name, coords, mip)\n";
    ss << "#define LOAD_TEXTURE_MS(name, coords, sample) texelFetch(name, coords, int(sample))\n";
    ss << "#define LOAD_TEXTURE_OFFSET(name, coords, mip, offset) texelFetchOffset(name, coords, mip, offset)\n";
    ss << "#define LOAD_TEXTURE_BUFFER(name, index) texelFetch(name, index)\n";
    ss << "#define BEGIN_ARRAY(type, size) type[size](\n";
    ss << "#define END_ARRAY )\n";

    ss << "float saturate(float value) { return clamp(value, 0.0, 1.0); }\n";
    ss << "float2 saturate(float2 value) { return clamp(value, float2(0.0, 0.0), float2(1.0, 1.0)); }\n";
    ss << "float3 saturate(float3 value) { return clamp(value, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)); }\n";
    ss << "float4 saturate(float4 value) { return clamp(value, float4(0.0, 0.0, 0.0, 0.0), float4(1.0, 1.0, 1.0, "
          "1.0)); }\n";
  }
  else
  {
    ss << "#define HLSL 1\n";
    ss << "#define roundEven round\n";
    ss << "#define mix lerp\n";
    ss << "#define fract frac\n";
    ss << "#define vec2 float2\n";
    ss << "#define vec3 float3\n";
    ss << "#define vec4 float4\n";
    ss << "#define ivec2 int2\n";
    ss << "#define ivec3 int3\n";
    ss << "#define ivec4 int4\n";
    ss << "#define uivec2 uint2\n";
    ss << "#define uivec3 uint3\n";
    ss << "#define uivec4 uint4\n";
    ss << "#define mat2 float2x2\n";
    ss << "#define mat3 float3x3\n";
    ss << "#define mat4 float4x4\n";
    ss << "#define CONSTANT static const\n";
    ss << "#define GLOBAL static\n";
    ss << "#define FOR_UNROLL [unroll] for\n";
    ss << "#define FOR_LOOP [loop] for\n";
    ss << "#define IF_BRANCH [branch] if\n";
    ss << "#define IF_FLATTEN [flatten] if\n";
    ss << "#define VECTOR_EQ(a, b) (all((a) == (b)))\n";
    ss << "#define VECTOR_NEQ(a, b) (any((a) != (b)))\n";
    ss << "#define VECTOR_COMP_EQ(a, b) ((a) == (b))\n";
    ss << "#define VECTOR_COMP_NEQ(a, b) ((a) != (b))\n";
    ss << "#define SAMPLE_TEXTURE(name, coords) name.Sample(name##_ss, coords)\n";
    ss << "#define SAMPLE_TEXTURE_OFFSET(name, coords, offset) name.Sample(name##_ss, coords, offset)\n";
    ss << "#define SAMPLE_TEXTURE_LEVEL(name, coords, level) name.SampleLevel(name##_ss, coords, level)\n";
    ss << "#define SAMPLE_TEXTURE_LEVEL_OFFSET(name, coords, level, offset) name.SampleLevel(name##_ss, coords, level, "
          "offset)\n";
    ss << "#define LOAD_TEXTURE(name, coords, mip) name.Load(int3(coords, mip))\n";
    ss << "#define LOAD_TEXTURE_MS(name, coords, sample) name.Load(coords, sample)\n";
    ss << "#define LOAD_TEXTURE_OFFSET(name, coords, mip, offset) name.Load(int3(coords, mip), offset)\n";
    ss << "#define LOAD_TEXTURE_BUFFER(name, index) name.Load(index)\n";
    ss << "#define BEGIN_ARRAY(type, size) {\n";
    ss << "#define END_ARRAY }\n";
  }

  ss << "\n";
}

void ShaderGen::WriteUniformBufferDeclaration(std::stringstream& ss, bool push_constant_on_vulkan)
{
  if (IsVulkan())
  {
    if (push_constant_on_vulkan)
      ss << "layout(push_constant) uniform PushConstants\n";
    else
      ss << "layout(std140, set = 0, binding = 0) uniform UBOBlock\n";
  }
  else if (m_glsl)
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
}

void ShaderGen::DeclareUniformBuffer(std::stringstream& ss, const std::initializer_list<const char*>& members,
                                     bool push_constant_on_vulkan)
{
  WriteUniformBufferDeclaration(ss, push_constant_on_vulkan);

  ss << "{\n";
  for (const char* member : members)
    ss << member << ";\n";
  ss << "};\n\n";
}

void ShaderGen::DeclareTexture(std::stringstream& ss, const char* name, u32 index, bool multisampled /* = false */)
{
  if (m_glsl)
  {
    if (IsVulkan())
      ss << "layout(set = 0, binding = " << (index + 1u) << ") ";
    else if (m_use_glsl_binding_layout)
      ss << "layout(binding = " << index << ") ";

    ss << "uniform " << (multisampled ? "sampler2DMS " : "sampler2D ") << name << ";\n";
  }
  else
  {
    ss << (multisampled ? "Texture2DMS<float4> " : "Texture2D ") << name << " : register(t" << index << ");\n";
    ss << "SamplerState " << name << "_ss : register(s" << index << ");\n";
  }
}

void ShaderGen::DeclareTextureBuffer(std::stringstream& ss, const char* name, u32 index, bool is_int, bool is_unsigned)
{
  if (m_glsl)
  {
    if (IsVulkan())
      ss << "layout(set = 0, binding = " << index << ") ";
    else if (m_use_glsl_binding_layout)
      ss << "layout(binding = " << index << ") ";

    ss << "uniform " << (is_int ? (is_unsigned ? "u" : "i") : "") << "samplerBuffer " << name << ";\n";
  }
  else
  {
    ss << "Buffer<" << (is_int ? (is_unsigned ? "uint4" : "int4") : "float4") << "> " << name << " : register(t"
       << index << ");\n";
  }
}

const char* ShaderGen::GetInterpolationQualifier(bool interface_block, bool centroid_interpolation,
                                                 bool sample_interpolation, bool is_out) const
{
  if (m_glsl && interface_block && (!IsVulkan() && !GLAD_GL_ARB_shading_language_420pack))
  {
    return (sample_interpolation ? (is_out ? "sample out " : "sample in ") :
                                   (centroid_interpolation ? (is_out ? "centroid out " : "centroid in ") : ""));
  }
  else
  {
    return (sample_interpolation ? "sample " : (centroid_interpolation ? "centroid " : ""));
  }
}

void ShaderGen::DeclareVertexEntryPoint(
  std::stringstream& ss, const std::initializer_list<const char*>& attributes, u32 num_color_outputs,
  u32 num_texcoord_outputs, const std::initializer_list<std::pair<const char*, const char*>>& additional_outputs,
  bool declare_vertex_id /* = false */, const char* output_block_suffix /* = "" */,
  bool centroid_interpolation /* = false */, bool sample_interpolation /* = false */)
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
      const char* qualifier = GetInterpolationQualifier(true, centroid_interpolation, sample_interpolation, true);

      if (IsVulkan())
        ss << "layout(location = 0) ";

      ss << "out VertexData" << output_block_suffix << " {\n";
      for (u32 i = 0; i < num_color_outputs; i++)
        ss << "  " << qualifier << "float4 v_col" << i << ";\n";

      for (u32 i = 0; i < num_texcoord_outputs; i++)
        ss << "  " << qualifier << "float2 v_tex" << i << ";\n";

      for (const auto& [qualifiers, name] : additional_outputs)
      {
        const char* qualifier_to_use = (std::strlen(qualifiers) > 0) ? qualifiers : qualifier;
        ss << "  " << qualifier_to_use << " " << name << ";\n";
      }
      ss << "};\n";
    }
    else
    {
      const char* qualifier = GetInterpolationQualifier(false, centroid_interpolation, sample_interpolation, true);

      for (u32 i = 0; i < num_color_outputs; i++)
        ss << qualifier << "out float4 v_col" << i << ";\n";

      for (u32 i = 0; i < num_texcoord_outputs; i++)
        ss << qualifier << "out float2 v_tex" << i << ";\n";

      for (const auto& [qualifiers, name] : additional_outputs)
      {
        const char* qualifier_to_use = (std::strlen(qualifiers) > 0) ? qualifiers : qualifier;
        ss << qualifier_to_use << " out " << name << ";\n";
      }
    }

    ss << "#define v_pos gl_Position\n\n";
    if (declare_vertex_id)
    {
      if (IsVulkan())
        ss << "#define v_id uint(gl_VertexIndex)\n";
      else
        ss << "#define v_id uint(gl_VertexID)\n";
    }

    ss << "\n";
    ss << "void main()\n";
  }
  else
  {
    const char* qualifier = GetInterpolationQualifier(false, centroid_interpolation, sample_interpolation, true);

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
      ss << "  " << qualifier << "out float4 v_col" << i << " : COLOR" << i << ",\n";

    for (u32 i = 0; i < num_texcoord_outputs; i++)
      ss << "  " << qualifier << "out float2 v_tex" << i << " : TEXCOORD" << i << ",\n";

    u32 additional_counter = num_texcoord_outputs;
    for (const auto& [qualifiers, name] : additional_outputs)
    {
      const char* qualifier_to_use = (std::strlen(qualifiers) > 0) ? qualifiers : qualifier;
      ss << "  " << qualifier_to_use << " out " << name << " : TEXCOORD" << additional_counter << ",\n";
      additional_counter++;
    }

    ss << "  out float4 v_pos : SV_Position)\n";
  }
}

void ShaderGen::DeclareFragmentEntryPoint(
  std::stringstream& ss, u32 num_color_inputs, u32 num_texcoord_inputs,
  const std::initializer_list<std::pair<const char*, const char*>>& additional_inputs,
  bool declare_fragcoord /* = false */, u32 num_color_outputs /* = 1 */, bool depth_output /* = false */,
  bool centroid_interpolation /* = false */, bool sample_interpolation /* = false */,
  bool declare_sample_id /* = false */)
{
  if (m_glsl)
  {
    if (m_use_glsl_interface_blocks)
    {
      const char* qualifier = GetInterpolationQualifier(true, centroid_interpolation, sample_interpolation, false);

      if (IsVulkan())
        ss << "layout(location = 0) ";

      ss << "in VertexData {\n";
      for (u32 i = 0; i < num_color_inputs; i++)
        ss << "  " << qualifier << "float4 v_col" << i << ";\n";

      for (u32 i = 0; i < num_texcoord_inputs; i++)
        ss << "  " << qualifier << "float2 v_tex" << i << ";\n";

      for (const auto& [qualifiers, name] : additional_inputs)
      {
        const char* qualifier_to_use = (std::strlen(qualifiers) > 0) ? qualifiers : qualifier;
        ss << "  " << qualifier_to_use << " " << name << ";\n";
      }
      ss << "};\n";
    }
    else
    {
      const char* qualifier = GetInterpolationQualifier(false, centroid_interpolation, sample_interpolation, false);

      for (u32 i = 0; i < num_color_inputs; i++)
        ss << qualifier << "in float4 v_col" << i << ";\n";

      for (u32 i = 0; i < num_texcoord_inputs; i++)
        ss << qualifier << "in float2 v_tex" << i << ";\n";

      for (const auto& [qualifiers, name] : additional_inputs)
      {
        const char* qualifier_to_use = (std::strlen(qualifiers) > 0) ? qualifiers : qualifier;
        ss << qualifier_to_use << " in " << name << ";\n";
      }
    }

    if (declare_fragcoord)
      ss << "#define v_pos gl_FragCoord\n";

    if (declare_sample_id)
      ss << "#define f_sample_index uint(gl_SampleID)\n";

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
          ss << "layout(location = " << i << ") out float4 o_col" << i << ";\n";
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
    const char* qualifier = GetInterpolationQualifier(false, centroid_interpolation, sample_interpolation, false);

    ss << "void main(\n";

    for (u32 i = 0; i < num_color_inputs; i++)
      ss << "  " << qualifier << "in float4 v_col" << i << " : COLOR" << i << ",\n";

    for (u32 i = 0; i < num_texcoord_inputs; i++)
      ss << "  " << qualifier << "in float2 v_tex" << i << " : TEXCOORD" << i << ",\n";

    u32 additional_counter = num_texcoord_inputs;
    for (const auto& [qualifiers, name] : additional_inputs)
    {
      const char* qualifier_to_use = (std::strlen(qualifiers) > 0) ? qualifiers : qualifier;
      ss << "  " << qualifier_to_use << " in " << name << " : TEXCOORD" << additional_counter << ",\n";
      additional_counter++;
    }

    if (declare_fragcoord)
      ss << "  in float4 v_pos : SV_Position,\n";
    if (declare_sample_id)
      ss << "  in uint f_sample_index : SV_SampleIndex,\n";

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

std::string ShaderGen::GenerateScreenQuadVertexShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareVertexEntryPoint(ss, {}, 0, 1, {}, true);
  ss << R"(
{
  v_tex0 = float2(float((v_id << 1) & 2u), float(v_id & 2u));
  v_pos = float4(v_tex0 * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  #if API_OPENGL || API_OPENGL_ES || API_VULKAN
    v_pos.y = -v_pos.y;
  #endif
}
)";

  return ss.str();
}

std::string ShaderGen::GenerateUVQuadVertexShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"float2 u_uv_min", "float2 u_uv_max"}, true);
  DeclareVertexEntryPoint(ss, {}, 0, 1, {}, true);
  ss << R"(
{
  v_tex0 = float2(float((v_id << 1) & 2u), float(v_id & 2u));
  v_pos = float4(v_tex0 * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  v_tex0 = u_uv_min + (u_uv_max - u_uv_min) * v_tex0;
  #if API_OPENGL || API_OPENGL_ES || API_VULKAN
    v_pos.y = -v_pos.y;
  #endif
}
)";

  return ss.str();
}

std::string ShaderGen::GenerateFillFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"float4 u_fill_color"}, true);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, false, 1, true);

  ss << R"(
{
  o_col0 = u_fill_color;
  o_depth = u_fill_color.a;
}
)";

  return ss.str();
}

std::string ShaderGen::GenerateCopyFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"float4 u_src_rect"}, true);
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

std::string ShaderGen::GenerateSampleFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareTexture(ss, "samp0", 0);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, false, 1);

  ss << R"(
{
  o_col0 = SAMPLE_TEXTURE(samp0, v_tex0);
}
)";

  return ss.str();
}
