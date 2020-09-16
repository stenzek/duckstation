#include "postprocessing_shadergen.h"

namespace FrontendCommon {

PostProcessingShaderGen::PostProcessingShaderGen(HostDisplay::RenderAPI render_api, bool supports_dual_source_blend)
  : ShaderGen(render_api, supports_dual_source_blend)
{
}

PostProcessingShaderGen::~PostProcessingShaderGen() = default;

std::string PostProcessingShaderGen::GeneratePostProcessingVertexShader(const PostProcessingShader& shader)
{
  std::stringstream ss;

  WriteHeader(ss);
  DeclareTexture(ss, "samp0", 0);
  WriteUniformBuffer(ss, shader, shader.UsePushConstants());

  DeclareVertexEntryPoint(ss, {}, 0, 1, {}, true);
  ss << R"(
{
  v_tex0 = float2(float((v_id << 1) & 2u), float(v_id & 2u));
  v_pos = float4(v_tex0 * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  #if API_OPENGL || API_OPENGL_ES || API_VULKAN
    v_pos.y = -v_pos.y;
  #endif
  v_tex0 = src_rect.xy + (src_size * v_tex0);
}
)";

  return ss.str();
}

std::string PostProcessingShaderGen::GeneratePostProcessingFragmentShader(const PostProcessingShader& shader)
{
  std::stringstream ss;

  WriteHeader(ss);
  DeclareTexture(ss, "samp0", 0);
  WriteUniformBuffer(ss, shader, shader.UsePushConstants());

  // Rename main, since we need to set up globals
  if (!m_glsl)
  {
    // TODO: vecn -> floatn

    ss << R"(
#define main real_main
static float2 v_tex0;
static float4 v_pos;
static float4 o_col0;
// Wrappers for sampling functions.
#define texture(sampler, coords) sampler.Sample(sampler##_ss, coords)
#define textureOffset(sampler, coords, offset) sampler.Sample(sampler##_ss, coords, offset)
#define gl_FragCoord v_pos
)";
  }
  else
  {
    if (m_use_glsl_interface_blocks)
    {
      if (IsVulkan())
        ss << "layout(location = 0) ";

      ss << "in VertexData {\n";
      ss << "  float2 v_tex0;\n";
      ss << "};\n";
    }
    else
    {
      ss << "in float2 v_tex0;\n";
    }

    if (m_use_glsl_binding_layout)
    {
      ss << "layout(location = 0) out float4 o_col0;\n";
    }
    else
    {
      ss << "out float4 o_col0;\n";
    }
  }

  ss << R"(
float4 Sample() { return texture(samp0, v_tex0); }
float4 SampleLocation(float2 location) { return texture(samp0, location); }
#define SampleOffset(offset) textureOffset(samp0, v_tex0, offset)
float2 GetFragCoord()
{
  return gl_FragCoord.xy;
}
float2 GetWindowResolution()
{
  return window_resolution;
}
float2 GetResolution()
{
  return resolution;
}
float2 GetInvResolution()
{
  return rcp_resolution;
}
float2 GetCoordinates()
{
  return v_tex0;
}
float GetTime()
{
  return time;
}
void SetOutput(float4 color)
{
  o_col0 = color;
}
#define GetOption(x) (x)
#define OptionEnabled(x) ((x) != 0)
)";

  ss << shader.GetCode();

  if (!m_glsl)
  {
    ss << R"(
#undef main
void main(in float2 v_tex0_ : TEXCOORD0, in float4 v_pos_ : SV_Position, out float4 o_col0_ : SV_Target)
{
  v_pos = v_pos_;
  v_tex0 = v_tex0_;
  real_main();
  o_col0_ = o_col0;
}
)";
  }

  return ss.str();
}

void PostProcessingShaderGen::WriteUniformBuffer(std::stringstream& ss, const PostProcessingShader& shader,
                                                 bool use_push_constants)
{
  u32 pad_counter = 0;

  WriteUniformBufferDeclaration(ss, use_push_constants);
  ss << "{\n";
  ss << "  float4 src_rect;\n";
  ss << "  float2 src_size;\n";
  ss << "  float2 resolution;\n";
  ss << "  float2 rcp_resolution;\n";
  ss << "  float2 window_resolution;\n";
  ss << "  float2 rcp_window_resolution;\n";
  ss << "  float time;\n";
  ss << "  float ubo_pad" << (pad_counter++) << ";\n";
  ss << "\n";

  static constexpr std::array<const char*, PostProcessingShader::Option::MAX_VECTOR_COMPONENTS + 1> vector_size_suffix =
    {{"", "", "2", "3", "4"}};
  for (const PostProcessingShader::Option& option : shader.GetOptions())
  {
    switch (option.type)
    {
      case PostProcessingShader::Option::Type::Bool:
        ss << "  int " << option.name << ";\n";
        for (u32 i = option.vector_size; i < PostProcessingShader::Option::MAX_VECTOR_COMPONENTS; i++)
          ss << "  int ubo_pad" << (pad_counter++) << ";\n";
        break;

      case PostProcessingShader::Option::Type::Int:
      {
        ss << "  int" << vector_size_suffix[option.vector_size] << " " << option.name << ";\n";
        for (u32 i = option.vector_size; i < PostProcessingShader::Option::MAX_VECTOR_COMPONENTS; i++)
          ss << "  int ubo_pad" << (pad_counter++) << ";\n";
      }
      break;

      case PostProcessingShader::Option::Type::Float:
      default:
      {
        ss << "  float" << vector_size_suffix[option.vector_size] << " " << option.name << ";\n";
        for (u32 i = option.vector_size; i < PostProcessingShader::Option::MAX_VECTOR_COMPONENTS; i++)
          ss << "  float ubo_pad" << (pad_counter++) << ";\n";
      }
      break;
    }
  }

  ss << "};\n\n";
}

} // namespace FrontendCommon