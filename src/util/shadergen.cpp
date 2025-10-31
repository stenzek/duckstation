// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "shadergen.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"

#include <cstdio>
#include <cstring>
#include <iomanip>

#ifdef ENABLE_OPENGL
#include "opengl_loader.h"
#endif

LOG_CHANNEL(ShaderGen);

ShaderGen::ShaderGen(RenderAPI render_api, GPUShaderLanguage shader_language, bool supports_dual_source_blend,
                     bool supports_framebuffer_fetch)
  : m_render_api(render_api), m_shader_language(shader_language),
    m_glsl(shader_language == GPUShaderLanguage::GLSL || shader_language == GPUShaderLanguage::GLSLES ||
           shader_language == GPUShaderLanguage::GLSLVK),
    m_spirv(shader_language == GPUShaderLanguage::GLSLVK), m_supports_dual_source_blend(supports_dual_source_blend),
    m_supports_framebuffer_fetch(supports_framebuffer_fetch)
{
  if (m_glsl)
  {
#ifdef ENABLE_OPENGL
    if (m_render_api == RenderAPI::OpenGL || m_render_api == RenderAPI::OpenGLES)
    {
      m_glsl_version = GetGLSLVersion(render_api);
      m_glsl_version_string = GetGLSLVersionString(m_render_api, m_glsl_version);
      m_use_glsl_interface_blocks = UseGLSLInterfaceBlocks();
      m_use_glsl_binding_layout = UseGLSLBindingLayout();
    }
    else
    {
      m_use_glsl_interface_blocks = (shader_language == GPUShaderLanguage::GLSLVK);
      m_use_glsl_binding_layout = (shader_language == GPUShaderLanguage::GLSLVK);
    }

#ifdef _WIN32
    if (m_shader_language == GPUShaderLanguage::GLSL)
    {
      // SSAA with interface blocks is broken on AMD's OpenGL driver.
      const char* gl_vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
      if (std::strcmp(gl_vendor, "ATI Technologies Inc.") == 0)
        m_use_glsl_interface_blocks = false;
    }
#endif
#else
    m_use_glsl_interface_blocks = true;
    m_use_glsl_binding_layout = true;
#endif
  }
}

ShaderGen::~ShaderGen() = default;

GPUShaderLanguage ShaderGen::GetShaderLanguageForAPI(RenderAPI api)
{
  switch (api)
  {
    case RenderAPI::D3D11:
    case RenderAPI::D3D12:
      return GPUShaderLanguage::HLSL;

    case RenderAPI::Vulkan:
    case RenderAPI::Metal:
      return GPUShaderLanguage::GLSLVK;

    case RenderAPI::OpenGL:
      return GPUShaderLanguage::GLSL;

    case RenderAPI::OpenGLES:
      return GPUShaderLanguage::GLSLES;

    case RenderAPI::None:
    default:
      return GPUShaderLanguage::None;
  }
}

bool ShaderGen::UseGLSLInterfaceBlocks()
{
#ifdef ENABLE_OPENGL
  return (GLAD_GL_ES_VERSION_3_2 || GLAD_GL_VERSION_3_2);
#else
  return true;
#endif
}

bool ShaderGen::UseGLSLBindingLayout()
{
#ifdef ENABLE_OPENGL
  return (GLAD_GL_ES_VERSION_3_1 || GLAD_GL_VERSION_4_3 ||
          (GLAD_GL_ARB_explicit_attrib_location && GLAD_GL_ARB_explicit_uniform_location &&
           GLAD_GL_ARB_shading_language_420pack));
#else
  return true;
#endif
}

void ShaderGen::DefineMacro(std::stringstream& ss, const char* name, bool enabled) const
{
  ss << "#define " << name << " " << BoolToUInt32(enabled) << "\n";
}

void ShaderGen::DefineMacro(std::stringstream& ss, const char* name, s32 value) const
{
  ss << "#define " << name << " " << value << "\n";
}

u32 ShaderGen::GetGLSLVersion(RenderAPI render_api)
{
#ifdef ENABLE_OPENGL
  const char* glsl_version = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
  const bool glsl_es = (render_api == RenderAPI::OpenGLES);
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
    ERROR_LOG("Invalid GLSL version string: '{}' ('{}')", glsl_version, glsl_version_start);
    if (glsl_es)
    {
      major_version = 3;
      minor_version = 0;
    }
  }

  return (static_cast<u32>(major_version) * 100) + static_cast<u32>(minor_version);
#else
  return 460;
#endif
}

TinyString ShaderGen::GetGLSLVersionString(RenderAPI render_api, u32 version)
{
  const bool glsl_es = (render_api == RenderAPI::OpenGLES);
  const u32 major_version = (version / 100);
  const u32 minor_version = (version % 100);

  return TinyString::from_format("#version {}{:02d}{}", major_version, minor_version,
                                 (glsl_es && major_version >= 3) ? " es" : "");
}

void ShaderGen::WriteHeader(std::stringstream& ss, bool enable_rov /* = false */,
                            bool enable_framebuffer_fetch /* = false */,
                            bool enable_dual_source_blend /* = false */) const
{
  DebugAssert((!enable_rov && !enable_framebuffer_fetch && !enable_dual_source_blend) ||
              (enable_rov && !enable_framebuffer_fetch && !enable_dual_source_blend) ||
              (enable_rov && !enable_framebuffer_fetch && !enable_dual_source_blend) ||
              (!enable_rov && enable_framebuffer_fetch && !enable_dual_source_blend) ||
              (!enable_rov && !enable_framebuffer_fetch && enable_dual_source_blend));
  if (m_shader_language == GPUShaderLanguage::GLSL || m_shader_language == GPUShaderLanguage::GLSLES)
    ss << m_glsl_version_string << "\n\n";
  else if (m_spirv)
    ss << "#version 450 core\n\n";

#ifdef __APPLE__
  // TODO: Do this for Vulkan as well.
  if (m_render_api == RenderAPI::Metal)
  {
    if (!m_supports_framebuffer_fetch)
      ss << "#extension GL_EXT_samplerless_texture_functions : require\n";
  }
#endif

#ifdef ENABLE_OPENGL
  // Extension enabling for OpenGL.
  if (enable_framebuffer_fetch &&
      (m_shader_language == GPUShaderLanguage::GLSL || m_shader_language == GPUShaderLanguage::GLSLES))
  {
    if (GLAD_GL_EXT_shader_framebuffer_fetch)
      ss << "#extension GL_EXT_shader_framebuffer_fetch : require\n";
    else if (GLAD_GL_ARM_shader_framebuffer_fetch)
      ss << "#extension GL_ARM_shader_framebuffer_fetch : require\n";
  }

  if (m_shader_language == GPUShaderLanguage::GLSLES)
  {
    // Enable EXT_blend_func_extended for dual-source blend on OpenGL ES.
    if (enable_dual_source_blend)
    {
      if (GLAD_GL_EXT_blend_func_extended)
        ss << "#extension GL_EXT_blend_func_extended : require\n";
      if (GLAD_GL_ARB_blend_func_extended)
        ss << "#extension GL_ARB_blend_func_extended : require\n";
    }
  }
  else if (m_shader_language == GPUShaderLanguage::GLSL)
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
  else if (m_shader_language == GPUShaderLanguage::GLSLVK)
  {
    if (enable_rov)
      ss << "#extension GL_ARB_fragment_shader_interlock : require\n";
  }
#endif

  DefineMacro(ss, "API_OPENGL", m_render_api == RenderAPI::OpenGL);
  DefineMacro(ss, "API_OPENGL_ES", m_render_api == RenderAPI::OpenGLES);
  DefineMacro(ss, "API_D3D11", m_render_api == RenderAPI::D3D11);
  DefineMacro(ss, "API_D3D12", m_render_api == RenderAPI::D3D12);
  DefineMacro(ss, "API_VULKAN", m_render_api == RenderAPI::Vulkan);
  DefineMacro(ss, "API_METAL", m_render_api == RenderAPI::Metal);

#ifdef ENABLE_OPENGL
  if (m_shader_language == GPUShaderLanguage::GLSLES)
  {
    ss << "precision highp float;\n";
    ss << "precision highp int;\n";
    ss << "precision highp sampler2D;\n";
    ss << "precision highp isampler2D;\n";
    ss << "precision highp usampler2D;\n";

    if (GLAD_GL_ES_VERSION_3_1)
      ss << "precision highp sampler2DMS;\n";

    if (GLAD_GL_ES_VERSION_3_2)
      ss << "precision highp usamplerBuffer;\n";

    ss << "\n";
  }
#endif

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
    ss << "#define bool2 bvec2\n";
    ss << "#define bool3 bvec3\n";
    ss << "#define bool4 bvec4\n";
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
    ss << "#define SAMPLE_TEXTURE_LEVEL_OFFSET(name, coords, level, offset) textureLodOffset(name, coords, level, "
          "offset)\n";
    ss << "#define LOAD_TEXTURE(name, coords, mip) texelFetch(name, coords, mip)\n";
    ss << "#define LOAD_TEXTURE_MS(name, coords, sample) texelFetch(name, coords, int(sample))\n";
    ss << "#define LOAD_TEXTURE_OFFSET(name, coords, mip, offset) texelFetchOffset(name, coords, mip, offset)\n";
    ss << "#define LOAD_TEXTURE_BUFFER(name, index) texelFetch(name, index)\n";
    ss << "#define BEGIN_ARRAY(type, size) type[size](\n";
    ss << "#define END_ARRAY )\n";
    ss << "#define VECTOR_BROADCAST(type, value) (type(value))\n";

    ss << "float saturate(float value) { return clamp(value, 0.0, 1.0); }\n"
          "float2 saturate(float2 value) { return clamp(value, float2(0.0, 0.0), float2(1.0, 1.0)); }\n"
          "float3 saturate(float3 value) { return clamp(value, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)); }\n"
          "float4 saturate(float4 value) { return clamp(value, float4(0.0, 0.0, 0.0, 0.0), float4(1.0, 1.0, 1.0, "
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
    ss << "#define bvec2 bool2\n";
    ss << "#define bvec3 bool3\n";
    ss << "#define bvec4 bool4\n";
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
    ss << "#define VECTOR_BROADCAST(type, value) ((type)(value))\n";
  }

  // Pack functions missing from GLSL ES 3.0.
  // We can't rely on __VERSION__ because Adreno is a broken turd and reports 300 even for GLES 3.2.
  if (!m_glsl || (m_shader_language == GPUShaderLanguage::GLSL && m_glsl_version < 400) ||
      (m_shader_language == GPUShaderLanguage::GLSLES && m_glsl_version < 310))
  {
    ss << "uint packUnorm4x8(float4 value) {\n"
          "  uint4 ret = uint4(round(saturate(value) * 255.0));\n"
          "  return ret.x | (ret.y << 8) | (ret.z << 16) | (ret.w << 24);\n"
          "}\n"
          "\n"
          "float4 unpackUnorm4x8(uint value) {\n"
          "  uint4 ret = uint4(value & 0xffu, (value >> 8) & 0xffu, (value >> 16) & 0xffu, value >> 24);\n"
          "  return float4(ret) / 255.0;\n"
          "}\n";
  }

  ss << "\n";

  m_has_uniform_buffer = false;
}

void ShaderGen::WriteUniformBufferDeclaration(std::stringstream& ss, bool push_constant_on_vulkan) const
{
  if (m_shader_language == GPUShaderLanguage::GLSLVK)
  {
    if (m_render_api == RenderAPI::Vulkan && push_constant_on_vulkan)
    {
      ss << "layout(push_constant, row_major) uniform PushConstants\n";
    }
    else
    {
      ss << "layout(std140, row_major, set = 0, binding = 0) uniform UBOBlock\n";
      m_has_uniform_buffer = true;
    }
  }
  else if (m_glsl)
  {
    if (m_use_glsl_binding_layout)
      ss << "layout(std140, row_major, binding = 0) uniform UBOBlock\n";
    else
      ss << "layout(std140, row_major) uniform UBOBlock\n";

    m_has_uniform_buffer = true;
  }
  else
  {
    ss << "cbuffer UBOBlock : register(b0)\n";
    m_has_uniform_buffer = true;
  }
}

void ShaderGen::DeclareUniformBuffer(std::stringstream& ss, const std::initializer_list<const char*>& members,
                                     bool push_constant_on_vulkan) const
{
  WriteUniformBufferDeclaration(ss, push_constant_on_vulkan);

  ss << "{\n";
  for (const char* member : members)
    ss << member << ";\n";
  ss << "};\n\n";
}

void ShaderGen::DeclareTexture(std::stringstream& ss, const char* name, u32 index, bool multisampled /* = false */,
                               bool is_int /* = false */, bool is_unsigned /* = false */) const
{
  if (m_glsl)
  {
    if (m_spirv)
      ss << "layout(set = " << ((m_has_uniform_buffer || IsMetal()) ? 1 : 0) << ", binding = " << index << ") ";
    else if (m_use_glsl_binding_layout)
      ss << "layout(binding = " << index << ") ";

    ss << "uniform " << (is_int ? (is_unsigned ? "u" : "i") : "") << (multisampled ? "sampler2DMS " : "sampler2D ")
       << name << ";\n";
  }
  else
  {
    ss << (multisampled ? "Texture2DMS<" : "Texture2D<") << (is_int ? (is_unsigned ? "uint4" : "int4") : "float4")
       << "> " << name << " : register(t" << index << ");\n";
    ss << "SamplerState " << name << "_ss : register(s" << index << ");\n";
  }
}

void ShaderGen::DeclareTextureBuffer(std::stringstream& ss, const char* name, u32 index, bool is_int,
                                     bool is_unsigned) const
{
  if (m_glsl)
  {
    if (m_spirv)
      ss << "layout(set = " << ((m_has_uniform_buffer || IsMetal()) ? 1 : 0) << ", binding = " << index << ") ";
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

void ShaderGen::DeclareImage(std::stringstream& ss, const char* name, u32 index, bool is_float /* = false */,
                             bool is_int /* = false */, bool is_unsigned /* = false */) const
{
  if (m_glsl)
  {
    if (m_spirv)
      ss << "layout(set = " << (m_has_uniform_buffer ? 2 : 1) << ", binding = " << index;
    else
      ss << "layout(binding = " << index;

    ss << ", " << (is_int ? (is_unsigned ? "rgba8ui" : "rgba8i") : "rgba8") << ") "
       << "uniform restrict coherent image2D " << name << ";\n";
  }
  else
  {
    ss << "RasterizerOrderedTexture2D<"
       << (is_int ? (is_unsigned ? "uint4" : "int4") : (is_float ? "float4" : "unorm float4")) << "> " << name
       << " : register(u" << index << ");\n";
  }
}

const char* ShaderGen::GetInterpolationQualifier(bool interface_block, bool centroid_interpolation,
                                                 bool sample_interpolation, bool is_out) const
{
#ifdef ENABLE_OPENGL
  const bool shading_language_420pack = GLAD_GL_ARB_shading_language_420pack;
#else
  const bool shading_language_420pack = false;
#endif
  if (m_glsl && interface_block && (!m_spirv && !shading_language_420pack))
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
  bool declare_vertex_id /* = false */, const char* output_block_suffix /* = "" */, bool msaa /* = false */,
  bool ssaa /* = false */, bool noperspective_color /* = false */) const
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
      const char* qualifier = GetInterpolationQualifier(true, msaa, ssaa, true);

      if (m_spirv)
        ss << "layout(location = 0) ";

      ss << "out VertexData" << output_block_suffix << " {\n";
      for (u32 i = 0; i < num_color_outputs; i++)
        ss << "  " << (noperspective_color ? "noperspective " : "") << qualifier << "float4 v_col" << i << ";\n";

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
      const char* qualifier = GetInterpolationQualifier(false, msaa, ssaa, true);

      u32 location = 0;
      for (u32 i = 0; i < num_color_outputs; i++)
      {
        if (m_spirv)
          ss << "layout(location = " << location++ << ") ";

        ss << qualifier << (noperspective_color ? "noperspective " : "") << "out float4 v_col" << i << ";\n";
      }

      for (u32 i = 0; i < num_texcoord_outputs; i++)
      {
        if (m_spirv)
          ss << "layout(location = " << location++ << ") ";

        ss << qualifier << "out float2 v_tex" << i << ";\n";
      }

      for (const auto& [qualifiers, name] : additional_outputs)
      {
        if (m_spirv)
          ss << "layout(location = " << location++ << ") ";

        const char* qualifier_to_use = (std::strlen(qualifiers) > 0) ? qualifiers : qualifier;
        ss << qualifier_to_use << " out " << name << ";\n";
      }
    }

    ss << "#define v_pos gl_Position\n\n";
    if (declare_vertex_id)
    {
      if (m_spirv)
        ss << "#define v_id uint(gl_VertexIndex)\n";
      else
        ss << "#define v_id uint(gl_VertexID)\n";
    }

    ss << "\n";
    ss << "void main()\n";
  }
  else
  {
    const char* qualifier = GetInterpolationQualifier(false, msaa, ssaa, true);

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
      ss << "  " << qualifier << (noperspective_color ? "noperspective " : "") << "out float4 v_col" << i << " : COLOR"
         << i << ",\n";

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
  const std::initializer_list<std::pair<const char*, const char*>>& additional_inputs /* =  */,
  bool declare_fragcoord /* = false */, u32 num_color_outputs /* = 1 */, bool dual_source_output /* = false */,
  bool depth_output /* = false */, bool msaa /* = false */, bool ssaa /* = false */,
  bool declare_sample_id /* = false */, bool noperspective_color /* = false */, bool feedback_loop /* = false */,
  bool rov /* = false */) const
{
  if (m_glsl)
  {
    if (num_color_inputs > 0 || num_texcoord_inputs > 0 || additional_inputs.size() > 0)
    {
      if (m_use_glsl_interface_blocks)
      {
        const char* qualifier = GetInterpolationQualifier(true, msaa, ssaa, false);

        if (m_spirv)
          ss << "layout(location = 0) ";

        ss << "in VertexData {\n";
        for (u32 i = 0; i < num_color_inputs; i++)
          ss << "  " << qualifier << (noperspective_color ? "noperspective " : "") << "float4 v_col" << i << ";\n";

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
        const char* qualifier = GetInterpolationQualifier(false, msaa, ssaa, false);

        u32 location = 0;
        for (u32 i = 0; i < num_color_inputs; i++)
        {
          if (m_spirv)
            ss << "layout(location = " << location++ << ") ";

          ss << qualifier << (noperspective_color ? "noperspective " : "") << "in float4 v_col" << i << ";\n";
        }

        for (u32 i = 0; i < num_texcoord_inputs; i++)
        {
          if (m_spirv)
            ss << "layout(location = " << location++ << ") ";

          ss << qualifier << "in float2 v_tex" << i << ";\n";
        }

        for (const auto& [qualifiers, name] : additional_inputs)
        {
          if (m_spirv)
            ss << "layout(location = " << location++ << ") ";

          const char* qualifier_to_use = (std::strlen(qualifiers) > 0) ? qualifiers : qualifier;
          ss << qualifier_to_use << " in " << name << ";\n";
        }
      }
    }

    if (declare_fragcoord)
      ss << "#define v_pos gl_FragCoord\n";

    if (declare_sample_id)
      ss << "#define f_sample_index uint(gl_SampleID)\n";

    if (depth_output)
      ss << "#define o_depth gl_FragDepth\n";

    const char* target_0_qualifier = "out";

    if (feedback_loop)
    {
      Assert(!rov);

#ifdef ENABLE_OPENGL
      if (m_render_api == RenderAPI::OpenGL || m_render_api == RenderAPI::OpenGLES)
      {
        Assert(m_supports_framebuffer_fetch);
        if (GLAD_GL_EXT_shader_framebuffer_fetch)
        {
          target_0_qualifier = "inout";
          ss << "#define LAST_FRAG_COLOR o_col0\n";
        }
        else if (GLAD_GL_ARM_shader_framebuffer_fetch)
        {
          ss << "#define LAST_FRAG_COLOR gl_LastFragColorARM\n";
        }
      }
#endif
#ifdef ENABLE_VULKAN
      if (m_render_api == RenderAPI::Vulkan)
      {
        ss << "layout(input_attachment_index = 0, set = 2, binding = 0) uniform "
           << (msaa ? "subpassInputMS" : "subpassInput") << " u_input_rt; \n";
        ss << "#define LAST_FRAG_COLOR " << (msaa ? "subpassLoad(u_input_rt, gl_SampleID)" : "subpassLoad(u_input_rt)")
           << "\n";
      }
#endif
#ifdef __APPLE__
      if (m_render_api == RenderAPI::Metal)
      {
        if (m_supports_framebuffer_fetch)
        {
          // Set doesn't matter, because it's transformed to color0.
          ss << "layout(input_attachment_index = 0, set = 2, binding = 0) uniform "
             << (msaa ? "subpassInputMS" : "subpassInput") << " u_input_rt; \n";
          ss << "#define LAST_FRAG_COLOR "
             << (msaa ? "subpassLoad(u_input_rt, gl_SampleID)" : "subpassLoad(u_input_rt)") << "\n";
        }
        else
        {
          ss << "layout(set = 2, binding = 0) uniform " << (msaa ? "texture2DMS" : "texture2D") << " u_input_rt;\n";
          ss << "#define LAST_FRAG_COLOR texelFetch(u_input_rt, int2(gl_FragCoord.xy), " << (msaa ? "gl_SampleID" : "0")
             << ")\n";
        }
      }
#endif
    }
    else if (rov)
    {
      ss << "layout(pixel_interlock_ordered) in;\n";
      ss << "#define ROV_LOAD(name, coords) imageLoad(name, ivec2(coords))\n";
      ss << "#define ROV_STORE(name, coords, value) imageStore(name, ivec2(coords), value)\n";
      ss << "#define BEGIN_ROV_REGION beginInvocationInterlockARB()\n";
      ss << "#define END_ROV_REGION endInvocationInterlockARB()\n";
    }

    if (m_use_glsl_binding_layout)
    {
      if (dual_source_output && m_supports_dual_source_blend && num_color_outputs > 1)
      {
        for (u32 i = 0; i < num_color_outputs; i++)
        {
          ss << "layout(location = 0, index = " << i << ") " << ((i == 0) ? target_0_qualifier : "out")
             << " float4 o_col" << i << ";\n";
        }
      }
      else
      {
        for (u32 i = 0; i < num_color_outputs; i++)
        {
          ss << "layout(location = " << i << ") " << ((i == 0) ? target_0_qualifier : "out") << " float4 o_col" << i
             << ";\n";
        }
      }
    }
    else
    {
      for (u32 i = 0; i < num_color_outputs; i++)
        ss << ((i == 0) ? target_0_qualifier : "out") << " float4 o_col" << i << ";\n";
    }

    ss << "\n";

    ss << "void main()\n";
  }
  else
  {
    if (rov)
    {
      ss << "#define ROV_LOAD(name, coords) name[uint2(coords)]\n";
      ss << "#define ROV_STORE(name, coords, value) name[uint2(coords)] = value\n";
      ss << "#define BEGIN_ROV_REGION\n";
      ss << "#define END_ROV_REGION\n";
    }

    const char* qualifier = GetInterpolationQualifier(false, msaa, ssaa, false);

    ss << "void main(\n";

    bool first = true;
    for (u32 i = 0; i < num_color_inputs; i++)
    {
      ss << (first ? "" : ",\n") << "  " << qualifier << (noperspective_color ? "noperspective " : "")
         << "in float4 v_col" << i << " : COLOR" << i;
      first = false;
    }

    for (u32 i = 0; i < num_texcoord_inputs; i++)
    {
      ss << (first ? "" : ",\n") << "  " << qualifier << "in float2 v_tex" << i << " : TEXCOORD" << i;
      first = false;
    }

    u32 additional_counter = num_texcoord_inputs;
    for (const auto& [qualifiers, name] : additional_inputs)
    {
      const char* qualifier_to_use = (std::strlen(qualifiers) > 0) ? qualifiers : qualifier;
      ss << (first ? "" : ",\n") << "  " << qualifier_to_use << " in " << name << " : TEXCOORD" << additional_counter;
      additional_counter++;
      first = false;
    }

    if (declare_fragcoord)
    {
      ss << (first ? "" : ",\n") << "  in float4 v_pos : SV_Position";
      first = false;
    }
    if (declare_sample_id)
    {
      ss << (first ? "" : ",\n") << "  in uint f_sample_index : SV_SampleIndex";
      first = false;
    }

    if (depth_output)
    {
      ss << (first ? "" : ",\n") << "  out float o_depth : SV_Depth";
      first = false;
    }
    for (u32 i = 0; i < num_color_outputs; i++)
    {
      ss << (first ? "" : ",\n") << "  out float4 o_col" << i << " : SV_Target" << i;
      first = false;
    }

    ss << ")";
  }
}

std::string ShaderGen::GeneratePassthroughVertexShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareVertexEntryPoint(ss, {"float2 a_pos", "float2 a_tex0"}, 0, 1, {}, false, "", false, false, false);
  ss << R"(
{
  v_pos = float4(a_pos, 0.0f, 1.0f);
  v_tex0 = a_tex0;

  // NDC space Y flip in Vulkan.
  #if API_VULKAN
    v_pos.y = -v_pos.y;
  #endif
}
)";

  return std::move(ss).str();
}

std::string ShaderGen::GenerateScreenQuadVertexShader(float z /* = 0.0f */) const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareVertexEntryPoint(ss, {}, 0, 1, {}, true);
  ss << "{\n";
  ss << "  v_tex0 = float2(float((v_id << 1) & 2u), float(v_id & 2u));\n";
  ss << "  v_pos = float4(v_tex0 * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), " << std::fixed << z << "f, 1.0f);\n";
  ss << "  #if API_OPENGL || API_OPENGL_ES || API_VULKAN\n";
  ss << "    v_pos.y = -v_pos.y;\n";
  ss << "  #endif\n";
  ss << "}\n";

  return std::move(ss).str();
}

std::string ShaderGen::GenerateFillFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"float4 u_fill_color"}, true);
  DeclareFragmentEntryPoint(ss, 0, 1);

  ss << R"(
{
  o_col0 = u_fill_color;
}
)";

  return std::move(ss).str();
}

std::string ShaderGen::GenerateFillFragmentShader(const GSVector4 fixed_color) const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareFragmentEntryPoint(ss, 0, 0);

  ss << "{\n";
  ss << "  o_col0 = float4(" << std::fixed << fixed_color.x << ", " << fixed_color.y << ", " << fixed_color.z << ", "
     << fixed_color.w << ");\n";
  ss << "}\n";

  return std::move(ss).str();
}

std::string ShaderGen::GenerateCopyFragmentShader(bool offset) const
{
  std::stringstream ss;
  WriteHeader(ss);
  if (offset)
    DeclareUniformBuffer(ss, {"float4 u_src_rect"}, true);

  DeclareTexture(ss, "samp0", 0);
  DeclareFragmentEntryPoint(ss, 0, 1);

  if (offset)
  {
    ss << R"(
{
  float2 coords = u_src_rect.xy + v_tex0 * u_src_rect.zw;
  o_col0 = SAMPLE_TEXTURE(samp0, coords);
}
)";
  }
  else
  {
    ss << R"(
{
  o_col0 = SAMPLE_TEXTURE(samp0, v_tex0);
}
)";
  }

  return std::move(ss).str();
}

std::string ShaderGen::GenerateImGuiVertexShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"float4x4 ProjectionMatrix"}, false);
  DeclareVertexEntryPoint(ss, {"float2 a_pos", "float2 a_tex0", "float4 a_col0"}, 1, 1, {}, false);
  ss << R"(
{
  v_pos = mul(ProjectionMatrix, float4(a_pos, 0.f, 1.f));
  v_col0 = a_col0;
  v_tex0 = a_tex0;
  #if API_VULKAN
    v_pos.y = -v_pos.y;
  #endif
}
)";

  return std::move(ss).str();
}

std::string ShaderGen::GenerateImGuiFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareTexture(ss, "samp0", 0);
  DeclareFragmentEntryPoint(ss, 1, 1);

  ss << R"(
{
  o_col0 = v_col0 * SAMPLE_TEXTURE(samp0, v_tex0);
}
)";

  return std::move(ss).str();
}

std::string ShaderGen::GenerateFadeFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"float u_tex0_weight", "float u_tex1_weight"}, true);
  DeclareTexture(ss, "samp0", 0);
  DeclareTexture(ss, "samp1", 1);
  DeclareFragmentEntryPoint(ss, 0, 1);

  ss << R"(
{
  o_col0 = SAMPLE_TEXTURE(samp0, v_tex0) * u_tex0_weight;
  o_col0 += SAMPLE_TEXTURE(samp1, v_tex0) * u_tex1_weight;
  o_col0.a = 1.0f;
}
)";

  return std::move(ss).str();
}
