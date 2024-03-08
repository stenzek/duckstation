// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "postprocessing_shader_glsl.h"
#include "shadergen.h"

#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"

#include <cctype>
#include <cstring>
#include <sstream>

Log_SetChannel(PostProcessing);

namespace {
class PostProcessingGLSLShaderGen : public ShaderGen
{
public:
  PostProcessingGLSLShaderGen(RenderAPI render_api, bool supports_dual_source_blend, bool supports_framebuffer_fetch);
  ~PostProcessingGLSLShaderGen();

  std::string GeneratePostProcessingVertexShader(const PostProcessing::GLSLShader& shader);
  std::string GeneratePostProcessingFragmentShader(const PostProcessing::GLSLShader& shader);

private:
  void WriteUniformBuffer(std::stringstream& ss, const PostProcessing::GLSLShader& shader, bool use_push_constants);
};
} // namespace

PostProcessing::GLSLShader::GLSLShader() = default;

PostProcessing::GLSLShader::GLSLShader(std::string name, std::string code) : m_code(code)
{
  m_name = std::move(name);
  LoadOptions();
}

PostProcessing::GLSLShader::~GLSLShader() = default;

bool PostProcessing::GLSLShader::LoadFromFile(std::string name, const char* filename, Error* error)
{
  std::optional<std::string> code = FileSystem::ReadFileToString(filename, error);
  if (!code.has_value() || code->empty())
    return false;

  return LoadFromString(std::move(name), code.value(), error);
}

bool PostProcessing::GLSLShader::LoadFromString(std::string name, std::string code, Error* error)
{
  m_name = std::move(name);
  m_code = std::move(code);
  m_options.clear();
  LoadOptions();
  return true;
}

bool PostProcessing::GLSLShader::IsValid() const
{
  return !m_name.empty() && !m_code.empty();
}

u32 PostProcessing::GLSLShader::GetUniformsSize() const
{
  // lazy packing. todo improve.
  return sizeof(CommonUniforms) + (sizeof(ShaderOption::ValueVector) * static_cast<u32>(m_options.size()));
}

void PostProcessing::GLSLShader::FillUniformBuffer(void* buffer, u32 texture_width, s32 texture_height,
                                                   s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                                                   s32 texture_view_height, u32 window_width, u32 window_height,
                                                   s32 original_width, s32 original_height, float time) const
{
  CommonUniforms* common = static_cast<CommonUniforms*>(buffer);

  const float rcp_texture_width = 1.0f / static_cast<float>(texture_width);
  const float rcp_texture_height = 1.0f / static_cast<float>(texture_height);
  common->src_rect[0] = static_cast<float>(texture_view_x) * rcp_texture_width;
  common->src_rect[1] = static_cast<float>(texture_view_y) * rcp_texture_height;
  common->src_rect[2] = (static_cast<float>(texture_view_x + texture_view_width - 1)) * rcp_texture_width;
  common->src_rect[3] = (static_cast<float>(texture_view_y + texture_view_height - 1)) * rcp_texture_height;
  common->src_size[0] = (static_cast<float>(texture_view_width)) * rcp_texture_width;
  common->src_size[1] = (static_cast<float>(texture_view_height)) * rcp_texture_height;
  common->resolution[0] = static_cast<float>(texture_width);
  common->resolution[1] = static_cast<float>(texture_height);
  common->rcp_resolution[0] = rcp_texture_width;
  common->rcp_resolution[1] = rcp_texture_height;
  common->window_resolution[0] = static_cast<float>(window_width);
  common->window_resolution[1] = static_cast<float>(window_height);
  common->rcp_window_resolution[0] = 1.0f / static_cast<float>(window_width);
  common->rcp_window_resolution[1] = 1.0f / static_cast<float>(window_height);

  // pad the "original size" relative to the positioning on the screen
  const float view_scale_x = static_cast<float>(original_width) / static_cast<float>(texture_view_width);
  const float view_scale_y = static_cast<float>(original_height) / static_cast<float>(texture_view_height);
  const s32 view_pad_x = texture_view_x + (texture_width - texture_view_width - texture_view_x);
  const s32 view_pad_y = texture_view_y + (texture_height - texture_view_height - texture_view_y);
  common->original_size[0] = static_cast<float>(original_width);
  common->original_size[1] = static_cast<float>(original_height);
  common->padded_original_size[0] = common->original_size[0] + static_cast<float>(view_pad_x) * view_scale_x;
  common->padded_original_size[1] = common->original_size[1] + static_cast<float>(view_pad_y) * view_scale_y;

  common->time = time;

  u8* option_values = reinterpret_cast<u8*>(common + 1);
  for (const ShaderOption& option : m_options)
  {
    std::memcpy(option_values, option.value.data(), sizeof(ShaderOption::ValueVector));
    option_values += sizeof(ShaderOption::ValueVector);
  }
}

bool PostProcessing::GLSLShader::CompilePipeline(GPUTexture::Format format, u32 width, u32 height, ProgressCallback* progress)
{
  if (m_pipeline)
    m_pipeline.reset();

  PostProcessingGLSLShaderGen shadergen(g_gpu_device->GetRenderAPI(), g_gpu_device->GetFeatures().dual_source_blend,
                                        g_gpu_device->GetFeatures().framebuffer_fetch);

  std::unique_ptr<GPUShader> vs =
    g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GeneratePostProcessingVertexShader(*this));
  std::unique_ptr<GPUShader> fs =
    g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GeneratePostProcessingFragmentShader(*this));
  if (!vs || !fs)
    return false;

  GPUPipeline::GraphicsConfig plconfig;
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndUBO;
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.SetTargetFormats(format);
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  plconfig.vertex_shader = vs.get();
  plconfig.fragment_shader = fs.get();
  plconfig.geometry_shader = nullptr;

  if (!(m_pipeline = g_gpu_device->CreatePipeline(plconfig)))
    return false;

  if (!m_sampler)
  {
    GPUSampler::Config config = GPUSampler::GetNearestConfig();
    config.address_u = GPUSampler::AddressMode::ClampToBorder;
    config.address_v = GPUSampler::AddressMode::ClampToBorder;
    config.border_color = 0xFF000000u;
    if (!(m_sampler = g_gpu_device->CreateSampler(config)))
      return false;
  }

  return true;
}

bool PostProcessing::GLSLShader::Apply(GPUTexture* input, GPUTexture* final_target, s32 final_left, s32 final_top,
                                       s32 final_width, s32 final_height, s32 orig_width, s32 orig_height,
                                       u32 target_width, u32 target_height)
{
  GL_SCOPE_FMT("GLSL Shader {}", m_name);

  // Assumes final stage has been cleared already.
  if (!final_target)
  {
    if (!g_gpu_device->BeginPresent(false))
      return false;
  }
  else
  {
    g_gpu_device->SetRenderTargets(&final_target, 1, nullptr);
    g_gpu_device->ClearRenderTarget(final_target, 0); // TODO: Could use an invalidate here too.
  }

  g_gpu_device->SetPipeline(m_pipeline.get());
  g_gpu_device->SetTextureSampler(0, input, m_sampler.get());
  g_gpu_device->SetViewportAndScissor(final_left, final_top, final_width, final_height);

  const u32 uniforms_size = GetUniformsSize();
  void* uniforms = g_gpu_device->MapUniformBuffer(uniforms_size);
  FillUniformBuffer(uniforms, input->GetWidth(), input->GetHeight(), final_left, final_top, final_width, final_height,
                    target_width, target_height, orig_width, orig_height,
                    static_cast<float>(PostProcessing::GetTimer().GetTimeSeconds()));
  g_gpu_device->UnmapUniformBuffer(uniforms_size);
  g_gpu_device->Draw(3, 0);
  return true;
}

bool PostProcessing::GLSLShader::ResizeOutput(GPUTexture::Format format, u32 width, u32 height)
{
  return true;
}

void PostProcessing::GLSLShader::LoadOptions()
{
  // Adapted from Dolphin's PostProcessingConfiguration::LoadOptions().
  constexpr char config_start_delimiter[] = "[configuration]";
  constexpr char config_end_delimiter[] = "[/configuration]";
  size_t configuration_start = m_code.find(config_start_delimiter);
  size_t configuration_end = m_code.find(config_end_delimiter);
  if (configuration_start == std::string::npos || configuration_end == std::string::npos)
  {
    // Issue loading configuration or there isn't one.
    return;
  }

  std::string configuration_string =
    m_code.substr(configuration_start + std::strlen(config_start_delimiter),
                  configuration_end - configuration_start - std::strlen(config_start_delimiter));

  std::istringstream in(configuration_string);

  ShaderOption current_option = {};
  while (!in.eof())
  {
    std::string line_str;
    if (std::getline(in, line_str))
    {
      std::string_view line_view = line_str;

      // Check for CRLF eol and convert it to LF
      if (!line_view.empty() && line_view.at(line_view.size() - 1) == '\r')
        line_view.remove_suffix(1);

      if (line_view.empty())
        continue;

      if (line_view[0] == '[')
      {
        size_t endpos = line_view.find("]");
        if (endpos != std::string::npos)
        {
          if (current_option.type != ShaderOption::Type::Invalid)
          {
            current_option.value = current_option.default_value;
            if (current_option.ui_name.empty())
              current_option.ui_name = current_option.name;

            if (!current_option.name.empty() && current_option.vector_size > 0)
              m_options.push_back(std::move(current_option));

            current_option = {};
          }

          // New section!
          std::string_view sub = line_view.substr(1, endpos - 1);
          if (sub == "OptionBool")
            current_option.type = ShaderOption::Type::Bool;
          else if (sub == "OptionRangeFloat")
            current_option.type = ShaderOption::Type::Float;
          else if (sub == "OptionRangeInteger")
            current_option.type = ShaderOption::Type::Int;
          else
            Log_ErrorPrintf("Invalid option type: '%s'", line_str.c_str());

          continue;
        }
      }

      if (current_option.type == ShaderOption::Type::Invalid)
        continue;

      std::string_view key, value;
      ParseKeyValue(line_view, &key, &value);
      if (!key.empty() && !value.empty())
      {
        if (key == "GUIName")
        {
          current_option.ui_name = value;
        }
        else if (key == "OptionName")
        {
          current_option.name = value;
        }
        else if (key == "DependentOption")
        {
          current_option.dependent_option = value;
        }
        else if (key == "MinValue" || key == "MaxValue" || key == "DefaultValue" || key == "StepAmount")
        {
          ShaderOption::ValueVector* dst_array;
          if (key == "MinValue")
            dst_array = &current_option.min_value;
          else if (key == "MaxValue")
            dst_array = &current_option.max_value;
          else if (key == "DefaultValue")
            dst_array = &current_option.default_value;
          else // if (key == "StepAmount")
            dst_array = &current_option.step_value;

          u32 size = 0;
          if (current_option.type == ShaderOption::Type::Bool)
            (*dst_array)[size++].int_value = StringUtil::FromChars<bool>(value).value_or(false) ? 1 : 0;
          else if (current_option.type == ShaderOption::Type::Float)
            size = PostProcessing::ShaderOption::ParseFloatVector(value, dst_array);
          else if (current_option.type == ShaderOption::Type::Int)
            size = PostProcessing::ShaderOption::ParseIntVector(value, dst_array);

          current_option.vector_size =
            (current_option.vector_size == 0) ? size : std::min(current_option.vector_size, size);
        }
        else
        {
          Log_ErrorPrintf("Invalid option key: '%s'", line_str.c_str());
        }
      }
    }
  }

  if (current_option.type != ShaderOption::Type::Invalid && !current_option.name.empty() &&
      current_option.vector_size > 0)
  {
    current_option.value = current_option.default_value;
    if (current_option.ui_name.empty())
      current_option.ui_name = current_option.name;

    m_options.push_back(std::move(current_option));
  }
}

PostProcessingGLSLShaderGen::PostProcessingGLSLShaderGen(RenderAPI render_api, bool supports_dual_source_blend,
                                                         bool supports_framebuffer_fetch)
  : ShaderGen(render_api, supports_dual_source_blend, supports_framebuffer_fetch)
{
}

PostProcessingGLSLShaderGen::~PostProcessingGLSLShaderGen() = default;

std::string PostProcessingGLSLShaderGen::GeneratePostProcessingVertexShader(const PostProcessing::GLSLShader& shader)
{
  std::stringstream ss;

  WriteHeader(ss);
  WriteUniformBuffer(ss, shader, false);
  DeclareTexture(ss, "samp0", 0);

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

std::string PostProcessingGLSLShaderGen::GeneratePostProcessingFragmentShader(const PostProcessing::GLSLShader& shader)
{
  std::stringstream ss;

  WriteHeader(ss);
  WriteUniformBuffer(ss, shader, false);
  DeclareTexture(ss, "samp0", 0);

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
      if (IsVulkan() || IsMetal())
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
float2 GetOriginalSize()
{
  return original_size;
}
float2 GetPaddedOriginalSize()
{
  return padded_original_size;
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

void PostProcessingGLSLShaderGen::WriteUniformBuffer(std::stringstream& ss, const PostProcessing::GLSLShader& shader,
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
  ss << "  float2 original_size;\n";
  ss << "  float2 padded_original_size;\n";
  ss << "  float time;\n";
  ss << "  float ubo_pad" << (pad_counter++) << ";\n";
  ss << "\n";

  static constexpr std::array<const char*, PostProcessing::ShaderOption::MAX_VECTOR_COMPONENTS + 1> vector_size_suffix =
    {{"", "", "2", "3", "4"}};
  for (const PostProcessing::ShaderOption& option : shader.GetOptions())
  {
    switch (option.type)
    {
      case PostProcessing::ShaderOption::Type::Bool:
        ss << "  int " << option.name << ";\n";
        for (u32 i = option.vector_size; i < PostProcessing::ShaderOption::MAX_VECTOR_COMPONENTS; i++)
          ss << "  int ubo_pad" << (pad_counter++) << ";\n";
        break;

      case PostProcessing::ShaderOption::Type::Int:
      {
        ss << "  int" << vector_size_suffix[option.vector_size] << " " << option.name << ";\n";
        for (u32 i = option.vector_size; i < PostProcessing::ShaderOption::MAX_VECTOR_COMPONENTS; i++)
          ss << "  int ubo_pad" << (pad_counter++) << ";\n";
      }
      break;

      case PostProcessing::ShaderOption::Type::Float:
      default:
      {
        ss << "  float" << vector_size_suffix[option.vector_size] << " " << option.name << ";\n";
        for (u32 i = option.vector_size; i < PostProcessing::ShaderOption::MAX_VECTOR_COMPONENTS; i++)
          ss << "  float ubo_pad" << (pad_counter++) << ";\n";
      }
      break;
    }
  }

  ss << "};\n\n";
}
