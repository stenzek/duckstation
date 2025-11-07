// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "postprocessing_shader_glsl.h"
#include "shadergen.h"

#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"

#include <cctype>
#include <cstring>
#include <sstream>

LOG_CHANNEL(PostProcessing);

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

u32 PostProcessing::GLSLShader::GetUniformsSize() const
{
  // lazy packing. todo improve.
  return sizeof(CommonUniforms) + (sizeof(ShaderOption::ValueVector) * static_cast<u32>(m_options.size()));
}

void PostProcessing::GLSLShader::FillUniformBuffer(void* buffer, s32 viewport_x, s32 viewport_y, s32 viewport_width,
                                                   s32 viewport_height, u32 window_width, u32 window_height,
                                                   s32 original_width, s32 original_height, s32 native_width,
                                                   s32 native_height, float time) const
{
  CommonUniforms* common = static_cast<CommonUniforms*>(buffer);

  const float internal_pixel_width = static_cast<float>(viewport_width) / static_cast<float>(original_width);
  const float internal_pixel_height = static_cast<float>(viewport_height) / static_cast<float>(original_height);
  const float native_pixel_width = (static_cast<float>(viewport_width) / static_cast<float>(native_width));
  const float native_pixel_height = (static_cast<float>(viewport_height) / static_cast<float>(native_height));
  common->src_rect[0] = static_cast<float>(viewport_x) / static_cast<float>(window_width);
  common->src_rect[1] = static_cast<float>(viewport_y) / static_cast<float>(window_height);
  common->src_rect[2] = (static_cast<float>(viewport_x + viewport_width - 1)) / static_cast<float>(window_width);
  common->src_rect[3] = (static_cast<float>(viewport_y + viewport_height - 1)) / static_cast<float>(window_height);
  common->src_size[0] = static_cast<float>(viewport_width) / static_cast<float>(window_width);
  common->src_size[1] = static_cast<float>(viewport_height) / static_cast<float>(window_height);
  common->window_size[0] = static_cast<float>(window_width);
  common->window_size[1] = static_cast<float>(window_height);
  common->rcp_window_size[0] = 1.0f / static_cast<float>(window_width);
  common->rcp_window_size[1] = 1.0f / static_cast<float>(window_height);
  common->viewport_size[0] = static_cast<float>(viewport_width);
  common->viewport_size[1] = static_cast<float>(viewport_height);
  common->window_to_viewport_ratio[0] = static_cast<float>(window_width) / static_cast<float>(viewport_width);
  common->window_to_viewport_ratio[1] = static_cast<float>(window_height) / static_cast<float>(viewport_height);
  common->internal_size[0] = static_cast<float>(original_width);
  common->internal_size[1] = static_cast<float>(original_height);
  common->internal_pixel_size[0] = internal_pixel_width;
  common->internal_pixel_size[1] = internal_pixel_height;
  common->norm_internal_pixel_size[0] = internal_pixel_width / static_cast<float>(window_width);
  common->norm_internal_pixel_size[1] = internal_pixel_height / static_cast<float>(window_height);
  common->native_size[0] = static_cast<float>(native_width);
  common->native_size[1] = static_cast<float>(native_height);
  common->native_pixel_size[0] = native_pixel_width;
  common->native_pixel_size[1] = native_pixel_height;
  common->norm_native_pixel_size[0] = native_pixel_width / static_cast<float>(window_width);
  common->norm_native_pixel_size[1] = native_pixel_height / static_cast<float>(window_height);
  common->upscale_multiplier = static_cast<float>(original_width) / static_cast<float>(native_width);
  common->time = time;

  u8* option_values = reinterpret_cast<u8*>(common + 1);
  for (const ShaderOption& option : m_options)
  {
    std::memcpy(option_values, option.value.data(), sizeof(ShaderOption::ValueVector));
    option_values += sizeof(ShaderOption::ValueVector);
  }
}

bool PostProcessing::GLSLShader::CompilePipeline(GPUTexture::Format format, u32 width, u32 height, Error* error,
                                                 ProgressCallback* progress)
{
  if (m_output_format == format)
    return true;

  m_pipeline.reset();
  m_output_format = GPUTexture::Format::Unknown;

  PostProcessingGLSLShaderGen shadergen(g_gpu_device->GetRenderAPI(), g_gpu_device->GetFeatures().dual_source_blend,
                                        g_gpu_device->GetFeatures().framebuffer_fetch);

  std::unique_ptr<GPUShader> vs = g_gpu_device->CreateShader(
    GPUShaderStage::Vertex, shadergen.GetLanguage(), shadergen.GeneratePostProcessingVertexShader(*this), error);
  std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(
    GPUShaderStage::Fragment, shadergen.GetLanguage(), shadergen.GeneratePostProcessingFragmentShader(*this), error);
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

  if (!(m_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
    return false;

  if (!m_sampler)
  {
    GPUSampler::Config config = GPUSampler::GetNearestConfig();
    config.address_u = GPUSampler::AddressMode::ClampToBorder;
    config.address_v = GPUSampler::AddressMode::ClampToBorder;
    config.border_color = 0xFF000000u;
    if (!(m_sampler = g_gpu_device->CreateSampler(config, error)))
      return false;
  }

  m_output_format = format;
  return true;
}

GPUDevice::PresentResult PostProcessing::GLSLShader::Apply(GPUTexture* input_color, GPUTexture* input_depth,
                                                           GPUTexture* final_target, GSVector4i final_rect,
                                                           s32 orig_width, s32 orig_height, s32 native_width,
                                                           s32 native_height, u32 target_width, u32 target_height,
                                                           float time)
{
  GL_SCOPE_FMT("GLSL Shader {}", m_name);

  // Assumes final stage has been cleared already.
  if (!final_target)
  {
    const GPUDevice::PresentResult pres = g_gpu_device->BeginPresent(g_gpu_device->GetMainSwapChain());
    if (pres != GPUDevice::PresentResult::OK)
      return pres;
  }
  else
  {
    g_gpu_device->SetRenderTargets(&final_target, 1, nullptr);
    g_gpu_device->ClearRenderTarget(final_target,
                                    GPUDevice::DEFAULT_CLEAR_COLOR); // TODO: Could use an invalidate here too.
  }

  g_gpu_device->SetPipeline(m_pipeline.get());
  g_gpu_device->SetTextureSampler(0, input_color, m_sampler.get());

  // need to flip the rect, since we're not drawing the entire fb
  const GSVector4i real_final_rect =
    g_gpu_device->UsesLowerLeftOrigin() ? g_gpu_device->FlipToLowerLeft(final_rect, target_height) : final_rect;
  g_gpu_device->SetViewportAndScissor(real_final_rect);

  const u32 uniforms_size = GetUniformsSize();
  void* uniforms = g_gpu_device->MapUniformBuffer(uniforms_size);
  FillUniformBuffer(uniforms, real_final_rect.left, real_final_rect.top, real_final_rect.width(),
                    real_final_rect.height(), target_width, target_height, orig_width, orig_height, native_width,
                    native_height, time);
  g_gpu_device->UnmapUniformBuffer(uniforms_size);
  g_gpu_device->Draw(3, 0);
  return GPUDevice::PresentResult::OK;
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
            ERROR_LOG("Invalid option type: '{}'", line_str);

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
          ERROR_LOG("Invalid option key: '{}'", line_str);
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
  : ShaderGen(render_api, GPUShaderLanguage::GLSLVK, supports_dual_source_blend, supports_framebuffer_fetch)
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
  v_tex0 = u_src_rect.xy + (u_src_size * v_tex0);
}
)";

  return std::move(ss).str();
}

std::string PostProcessingGLSLShaderGen::GeneratePostProcessingFragmentShader(const PostProcessing::GLSLShader& shader)
{
  std::stringstream ss;

  WriteHeader(ss);
  WriteUniformBuffer(ss, shader, false);
  DeclareTexture(ss, "samp0", 0);

  if (m_use_glsl_interface_blocks)
    ss << "layout(location = 0) in VertexData { vec2 v_tex0; };\n";
  else
    ss << "layout(location = 0) in vec2 v_tex0;\n";

  ss << R"(
layout(location = 0) out float4 o_col0;

float4 Sample() { return texture(samp0, v_tex0); }
float4 SampleLocation(float2 location) { return texture(samp0, location); }
#define SampleOffset(offset) textureOffset(samp0, v_tex0, offset)
float2 GetFragCoord() { return gl_FragCoord.xy; }
float2 GetCoordinates() { return v_tex0; }
float2 GetWindowSize() { return u_window_size; }
float2 GetInvWindowSize() { return u_rcp_window_size; }
float2 GetViewportSize() { return u_viewport_size; }
float2 GetWindowToViewportRatio() { return u_window_to_viewport_ratio; }
float2 GetInternalSize() { return u_internal_size; }
float2 GetInternalPixelSize() { return u_internal_pixel_size; }
float2 GetInvInternalPixelSize() { return u_norm_internal_pixel_size; }
float2 GetNativeSize() { return u_native_size; }
float2 GetNativePixelSize() { return u_native_pixel_size; }
float2 GetInvNativePixelSize() { return u_norm_native_pixel_size; }
float GetUpscaleMultiplier() { return u_upscale_multiplier; }
float GetTime() { return u_time; }
void SetOutput(float4 color) { o_col0 = color; }

// Deprecated, only present for backwards compatibility.
float2 GetResolution() { return u_window_size; }
float2 GetInvResolution() { return u_rcp_window_size; }
float2 GetOriginalSize() { return u_internal_size; }
float2 GetPaddedOriginalSize() { return u_internal_size * u_window_to_viewport_ratio; }
float2 GetWindowResolution() { return u_window_size; }

#define GetOption(x) (x)
#define OptionEnabled(x) ((x) != 0)
)";

  ss << shader.GetCode();
  return std::move(ss).str();
}

void PostProcessingGLSLShaderGen::WriteUniformBuffer(std::stringstream& ss, const PostProcessing::GLSLShader& shader,
                                                     bool use_push_constants)
{
  u32 pad_counter = 0;

  WriteUniformBufferDeclaration(ss, use_push_constants);
  ss << "{\n";
  ss << "  float4 u_src_rect;\n";
  ss << "  float2 u_src_size;\n";
  ss << "  float2 u_window_size;\n";
  ss << "  float2 u_rcp_window_size;\n";
  ss << "  float2 u_viewport_size;\n";
  ss << "  float2 u_window_to_viewport_ratio;\n";
  ss << "  float2 u_internal_size;\n";
  ss << "  float2 u_internal_pixel_size;\n";
  ss << "  float2 u_norm_internal_pixel_size;\n";
  ss << "  float2 u_native_size;\n";
  ss << "  float2 u_native_pixel_size;\n";
  ss << "  float2 u_norm_native_pixel_size;\n";
  ss << "  float u_upscale_multiplier;\n";
  ss << "  float u_time;\n";
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
          ss << "  int u_ubo_pad" << (pad_counter++) << ";\n";
        break;

      case PostProcessing::ShaderOption::Type::Int:
      {
        ss << "  int" << vector_size_suffix[option.vector_size] << " " << option.name << ";\n";
        for (u32 i = option.vector_size; i < PostProcessing::ShaderOption::MAX_VECTOR_COMPONENTS; i++)
          ss << "  int u_ubo_pad" << (pad_counter++) << ";\n";
      }
      break;

      case PostProcessing::ShaderOption::Type::Float:
      default:
      {
        ss << "  float" << vector_size_suffix[option.vector_size] << " " << option.name << ";\n";
        for (u32 i = option.vector_size; i < PostProcessing::ShaderOption::MAX_VECTOR_COMPONENTS; i++)
          ss << "  float u_ubo_pad" << (pad_counter++) << ";\n";
      }
      break;
    }
  }

  ss << "};\n\n";
}
