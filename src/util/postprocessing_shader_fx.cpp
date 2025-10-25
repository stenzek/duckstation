// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "postprocessing_shader_fx.h"
#include "image.h"
#include "input_manager.h"
#include "shadergen.h"

// TODO: Remove me
#include "core/gpu_thread.h"
#include "core/host.h"
#include "core/settings.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/progress_callback.h"
#include "common/string_util.h"

#include "effect_codegen.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"

#include "fmt/format.h"

#include <bitset>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>
#include <tuple>

LOG_CHANNEL(ReShadeFXShader);

static constexpr s32 DEFAULT_BUFFER_WIDTH = 3840;
static constexpr s32 DEFAULT_BUFFER_HEIGHT = 2160;

static bool PreprocessorFileExistsCallback(const std::string& path)
{
  if (Path::IsAbsolute(path))
    return FileSystem::FileExists(Path::ToNativePath(path).c_str());

  return Host::ResourceFileExists(path.c_str(), true);
}

static bool PreprocessorReadFileCallback(const std::string& path, std::string& data)
{
  std::optional<std::string> rdata;
  if (Path::IsAbsolute(path))
    rdata = FileSystem::ReadFileToString(Path::ToNativePath(path).c_str());
  else
    rdata = Host::ReadResourceFileToString(path.c_str(), true);
  if (!rdata.has_value())
    return false;

  data = std::move(rdata.value());
  return true;
}

static std::tuple<std::unique_ptr<reshadefx::codegen>, GPUShaderLanguage> CreateRFXCodegen(bool only_config,
                                                                                           Error* error)
{
  constexpr bool uniforms_to_spec_constants = false;

  if (only_config)
  {
    // Use SPIR-V for obtaining config, it's the fastest to generate.
    return std::make_tuple(std::unique_ptr<reshadefx::codegen>(
                             reshadefx::create_codegen_spirv(true, false, uniforms_to_spec_constants, false, false)),
                           GPUShaderLanguage::SPV);
  }

  // Should have a GPU device and be on the GPU thread.
  Assert(GPUThread::IsOnThread() && g_gpu_device);

  const bool debug_info = g_gpu_device->IsDebugDevice();
  const RenderAPI rapi = g_gpu_device->GetRenderAPI();
  [[maybe_unused]] const u32 rapi_version = g_gpu_device->GetRenderAPIVersion();

  switch (rapi)
  {
#ifdef _WIN32
    case RenderAPI::D3D11:
    case RenderAPI::D3D12:
    {
      // Use SPIR-V -> HLSL -> DXIL for D3D12. DXC can't handle texture parameters, which reshade generates.
      if (rapi == RenderAPI::D3D12 && rapi_version >= 1200)
      {
        return std::make_tuple(std::unique_ptr<reshadefx::codegen>(reshadefx::create_codegen_spirv(
                                 true, debug_info, uniforms_to_spec_constants, false, false, true)),
                               GPUShaderLanguage::SPV);
      }
      else
      {
        return std::make_tuple(std::unique_ptr<reshadefx::codegen>(reshadefx::create_codegen_hlsl(
                                 (rapi_version < 1100) ? 40 : 50, debug_info, uniforms_to_spec_constants)),
                               GPUShaderLanguage::HLSL);
      }
    }
    break;
#endif

    case RenderAPI::Vulkan:
    case RenderAPI::Metal:
    {
      return std::make_tuple(std::unique_ptr<reshadefx::codegen>(reshadefx::create_codegen_spirv(
                               true, debug_info, uniforms_to_spec_constants, false, (rapi == RenderAPI::Vulkan))),
                             GPUShaderLanguage::SPV);
    }

    case RenderAPI::OpenGL:
    case RenderAPI::OpenGLES:
    default:
    {
      // Binding layout is required for reshade.
      if (g_gpu_device && (!ShaderGen::UseGLSLInterfaceBlocks() || !ShaderGen::UseGLSLBindingLayout()))
      {
        Error::SetStringView(
          error,
          "ReShade post-processing requires an OpenGL driver that supports interface blocks and binding layout.");
        return {};
      }

      return std::make_tuple(std::unique_ptr<reshadefx::codegen>(reshadefx::create_codegen_glsl(
                               g_gpu_device ? ShaderGen::GetGLSLVersion(rapi) : 460, (rapi == RenderAPI::OpenGLES),
                               false, debug_info, uniforms_to_spec_constants, false, true)),
                             (rapi == RenderAPI::OpenGLES) ? GPUShaderLanguage::GLSLES : GPUShaderLanguage::GLSL);
    }
    break;
  }
}

static GPUTexture::Format MapTextureFormat(reshadefx::texture_format format)
{
  static constexpr GPUTexture::Format s_mapping[] = {
    GPUTexture::Format::Unknown, // unknown
    GPUTexture::Format::R8,      // r8
    GPUTexture::Format::R16,     // r16
    GPUTexture::Format::R16F,    // r16f
    GPUTexture::Format::R32I,    // r32i
    GPUTexture::Format::R32U,    // r32u
    GPUTexture::Format::R32F,    // r32f
    GPUTexture::Format::RG8,     // rg8
    GPUTexture::Format::RG16,    // rg16
    GPUTexture::Format::RG16F,   // rg16f
    GPUTexture::Format::RG32F,   // rg32f
    GPUTexture::Format::RGBA8,   // rgba8
    GPUTexture::Format::RGBA16,  // rgba16
    GPUTexture::Format::RGBA16F, // rgba16f
    GPUTexture::Format::RGBA32F, // rgba32f
    GPUTexture::Format::RGB10A2, // rgb10a2
  };
  DebugAssert(static_cast<u32>(format) < std::size(s_mapping));
  return s_mapping[static_cast<u32>(format)];
}

static GPUSampler::Config MapSampler(const reshadefx::sampler_desc& si)
{
  GPUSampler::Config config = GPUSampler::GetNearestConfig();

  switch (si.filter)
  {
    case reshadefx::filter_mode::min_mag_mip_point:
      config.min_filter = GPUSampler::Filter::Nearest;
      config.mag_filter = GPUSampler::Filter::Nearest;
      config.mip_filter = GPUSampler::Filter::Nearest;
      break;

    case reshadefx::filter_mode::min_mag_point_mip_linear:
      config.min_filter = GPUSampler::Filter::Nearest;
      config.mag_filter = GPUSampler::Filter::Nearest;
      config.mip_filter = GPUSampler::Filter::Linear;
      break;

    case reshadefx::filter_mode::min_point_mag_linear_mip_point:
      config.min_filter = GPUSampler::Filter::Linear;
      config.mag_filter = GPUSampler::Filter::Linear;
      config.mip_filter = GPUSampler::Filter::Nearest;
      break;

    case reshadefx::filter_mode::min_point_mag_mip_linear:
      config.min_filter = GPUSampler::Filter::Nearest;
      config.mag_filter = GPUSampler::Filter::Linear;
      config.mip_filter = GPUSampler::Filter::Linear;
      break;

    case reshadefx::filter_mode::min_linear_mag_mip_point:
      config.min_filter = GPUSampler::Filter::Linear;
      config.mag_filter = GPUSampler::Filter::Nearest;
      config.mip_filter = GPUSampler::Filter::Nearest;
      break;

    case reshadefx::filter_mode::min_linear_mag_point_mip_linear:
      config.min_filter = GPUSampler::Filter::Linear;
      config.mag_filter = GPUSampler::Filter::Nearest;
      config.mip_filter = GPUSampler::Filter::Linear;
      break;

    case reshadefx::filter_mode::min_mag_linear_mip_point:
      config.min_filter = GPUSampler::Filter::Linear;
      config.mag_filter = GPUSampler::Filter::Linear;
      config.mip_filter = GPUSampler::Filter::Nearest;
      break;

    case reshadefx::filter_mode::min_mag_mip_linear:
      config.min_filter = GPUSampler::Filter::Linear;
      config.mag_filter = GPUSampler::Filter::Linear;
      config.mip_filter = GPUSampler::Filter::Linear;
      break;

    default:
      break;
  }

  static constexpr auto map_address_mode = [](const reshadefx::texture_address_mode m) {
    switch (m)
    {
      case reshadefx::texture_address_mode::wrap:
        return GPUSampler::AddressMode::Repeat;
      case reshadefx::texture_address_mode::mirror:
        return GPUSampler::AddressMode::MirrorRepeat;
      case reshadefx::texture_address_mode::clamp:
        return GPUSampler::AddressMode::ClampToEdge;
      case reshadefx::texture_address_mode::border:
      default:
        return GPUSampler::AddressMode::ClampToBorder;
    }
  };

  config.address_u = map_address_mode(si.address_u);
  config.address_v = map_address_mode(si.address_v);
  config.address_w = map_address_mode(si.address_w);

  return config;
}

static GPUPipeline::BlendState MapBlendState(const reshadefx::pass& pi)
{
  static constexpr auto map_blend_op = [](const reshadefx::blend_op o) {
    switch (o)
    {
      case reshadefx::blend_op::add:
        return GPUPipeline::BlendOp::Add;
      case reshadefx::blend_op::subtract:
        return GPUPipeline::BlendOp::Subtract;
      case reshadefx::blend_op::reverse_subtract:
        return GPUPipeline::BlendOp::ReverseSubtract;
      case reshadefx::blend_op::min:
        return GPUPipeline::BlendOp::Min;
      case reshadefx::blend_op::max:
      default:
        return GPUPipeline::BlendOp::Max;
    }
  };
  static constexpr auto map_blend_factor = [](const reshadefx::blend_factor f) {
    switch (f)
    {
      case reshadefx::blend_factor::zero:
        return GPUPipeline::BlendFunc::Zero;
      case reshadefx::blend_factor::one:
        return GPUPipeline::BlendFunc::One;
      case reshadefx::blend_factor::source_color:
        return GPUPipeline::BlendFunc::SrcColor;
      case reshadefx::blend_factor::one_minus_source_color:
        return GPUPipeline::BlendFunc::InvSrcColor;
      case reshadefx::blend_factor::dest_color:
        return GPUPipeline::BlendFunc::DstColor;
      case reshadefx::blend_factor::one_minus_dest_color:
        return GPUPipeline::BlendFunc::InvDstColor;
      case reshadefx::blend_factor::source_alpha:
        return GPUPipeline::BlendFunc::SrcAlpha;
      case reshadefx::blend_factor::one_minus_source_alpha:
        return GPUPipeline::BlendFunc::InvSrcAlpha;
      case reshadefx::blend_factor::dest_alpha:
      default:
        return GPUPipeline::BlendFunc::DstAlpha;
    }
  };

  GPUPipeline::BlendState bs = GPUPipeline::BlendState::GetNoBlendingState();
  bs.enable = (pi.blend_enable[0] != 0);
  bs.blend_op = map_blend_op(pi.color_blend_op[0]);
  bs.src_blend = map_blend_factor(pi.source_color_blend_factor[0]);
  bs.dst_blend = map_blend_factor(pi.dest_color_blend_factor[0]);
  bs.alpha_blend_op = map_blend_op(pi.alpha_blend_op[0]);
  bs.src_alpha_blend = map_blend_factor(pi.source_alpha_blend_factor[0]);
  bs.dst_alpha_blend = map_blend_factor(pi.dest_alpha_blend_factor[0]);
  bs.write_mask = pi.render_target_write_mask[0];
  return bs;
}

static GPUPipeline::Primitive MapPrimitive(reshadefx::primitive_topology topology)
{
  switch (topology)
  {
    case reshadefx::primitive_topology::point_list:
      return GPUPipeline::Primitive::Points;
    case reshadefx::primitive_topology::line_list:
      return GPUPipeline::Primitive::Lines;
    case reshadefx::primitive_topology::line_strip:
      Panic("Unhandled line strip");
    case reshadefx::primitive_topology::triangle_list:
      return GPUPipeline::Primitive::Triangles;
    case reshadefx::primitive_topology::triangle_strip:
    default:
      return GPUPipeline::Primitive::TriangleStrips;
  }
}

PostProcessing::ReShadeFXShader::ReShadeFXShader() = default;

PostProcessing::ReShadeFXShader::~ReShadeFXShader()
{
  for (Texture& tex : m_textures)
    g_gpu_device->RecycleTexture(std::move(tex.texture));
}

bool PostProcessing::ReShadeFXShader::LoadFromFile(std::string name, std::string filename, bool only_config,
                                                   Error* error)
{
  std::optional<std::string> data = FileSystem::ReadFileToString(filename.c_str(), error);
  if (!data.has_value())
  {
    ERROR_LOG("Failed to read '{}'.", filename);
    return false;
  }

  return LoadFromString(std::move(name), std::move(filename), std::move(data.value()), only_config, error);
}

bool PostProcessing::ReShadeFXShader::LoadFromString(std::string name, std::string filename, std::string code,
                                                     bool only_config, Error* error)
{
  DebugAssert(only_config || g_gpu_device);

  m_name = std::move(name);
  m_filename = std::move(filename);

  // Reshade's preprocessor expects this.
  if (code.empty() || code.back() != '\n')
    code.push_back('\n');

  // TODO: This could use spv, it's probably fastest.
  const auto& [cg, cg_language] = CreateRFXCodegen(only_config, error);
  if (!cg || !CreateModule(only_config ? DEFAULT_BUFFER_WIDTH : g_gpu_device->GetMainSwapChain()->GetWidth(),
                           only_config ? DEFAULT_BUFFER_HEIGHT : g_gpu_device->GetMainSwapChain()->GetHeight(),
                           cg.get(), cg_language, std::move(code), error))
  {
    return false;
  }

  const reshadefx::effect_module& temp_module = cg->module();
  if (!CreateOptions(temp_module, error))
    return false;

  // check limits
  if (!temp_module.techniques.empty())
  {
    bool has_passes = false;
    for (const reshadefx::technique& tech : temp_module.techniques)
    {
      for (const reshadefx::pass& pi : tech.passes)
      {
        has_passes = true;

        u32 max_rt = 0;
        for (u32 i = 0; i < std::size(pi.render_target_names); i++)
        {
          if (pi.render_target_names[i].empty())
            break;

          max_rt = std::max(max_rt, i);
        }

        if (max_rt > GPUDevice::MAX_RENDER_TARGETS)
        {
          Error::SetStringFmt(error, "Too many render targets ({}) in pass {}, only {} are supported.", max_rt, pi.name,
                              GPUDevice::MAX_RENDER_TARGETS);
          return false;
        }

        if (pi.sampler_bindings.size() > GPUDevice::MAX_TEXTURE_SAMPLERS)
        {
          Error::SetStringFmt(error, "Too many samplers ({}) in pass {}, only {} are supported.",
                              pi.sampler_bindings.size(), pi.name, GPUDevice::MAX_TEXTURE_SAMPLERS);
          return false;
        }
      }
    }
    if (!has_passes)
    {
      Error::SetString(error, "No passes defined in file.");
      return false;
    }
  }

  return true;
}

bool PostProcessing::ReShadeFXShader::WantsDepthBuffer() const
{
  return m_wants_depth_buffer;
}

bool PostProcessing::ReShadeFXShader::CreateModule(s32 buffer_width, s32 buffer_height, reshadefx::codegen* cg,
                                                   GPUShaderLanguage cg_language, std::string code, Error* error)
{
  reshadefx::preprocessor pp;
  pp.set_include_callbacks(PreprocessorFileExistsCallback, PreprocessorReadFileCallback);

  if (Path::IsAbsolute(m_filename))
  {
    // we're a real file, so include that directory
    pp.add_include_path(std::string(Path::GetDirectory(m_filename)));
  }
  else
  {
    // we're a resource, include the resource subdirectory, if there is one
    if (std::string_view resdir = Path::GetDirectory(m_filename); !resdir.empty())
      pp.add_include_path(std::string(resdir));
  }

  // root of the user directory, and resources
  pp.add_include_path(Path::Combine(EmuFolders::Shaders, "reshade" FS_OSPATH_SEPARATOR_STR "Shaders"));
  pp.add_include_path("shaders/reshade/Shaders");

  pp.add_macro_definition("__RESHADE__", "50901");
  pp.add_macro_definition("BUFFER_WIDTH", StringUtil::ToChars(buffer_width)); // TODO: can we make these uniforms?
  pp.add_macro_definition("BUFFER_HEIGHT", StringUtil::ToChars(buffer_height));
  pp.add_macro_definition("BUFFER_RCP_WIDTH", StringUtil::ToChars(1.0f / static_cast<float>(buffer_width)));
  pp.add_macro_definition("BUFFER_RCP_HEIGHT", StringUtil::ToChars(1.0f / static_cast<float>(buffer_height)));
  pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "32");
  pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN", "0");
  pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", "0");
  pp.add_macro_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", "1000.0");
  pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", "0");

  switch (cg_language)
  {
    case GPUShaderLanguage::HLSL:
      pp.add_macro_definition("__RENDERER__", "0x0B000");
      break;

    case GPUShaderLanguage::GLSL:
    case GPUShaderLanguage::GLSLES:
    case GPUShaderLanguage::GLSLVK:
    case GPUShaderLanguage::MSL:
    case GPUShaderLanguage::SPV:
      pp.add_macro_definition("__RENDERER__", "0x14300");
      break;

    default:
      UnreachableCode();
      break;
  }

  if (!pp.append_string(std::move(code), m_filename))
  {
    Error::SetStringFmt(error, "Failed to preprocess:\n{}", pp.errors());
    return false;
  }

  reshadefx::parser parser;
  if (!parser.parse(pp.output(), cg))
  {
    Error::SetStringFmt(error, "Failed to parse:\n{}", parser.errors());
    return false;
  }

  return true;
}

static bool HasAnnotationWithName(const reshadefx::uniform& uniform, const std::string_view annotation_name)
{
  for (const reshadefx::annotation& an : uniform.annotations)
  {
    if (an.name == annotation_name)
      return true;
  }

  return false;
}

static std::string_view GetStringAnnotationValue(const std::vector<reshadefx::annotation>& annotations,
                                                 const std::string_view annotation_name,
                                                 const std::string_view default_value)
{
  for (const reshadefx::annotation& an : annotations)
  {
    if (an.name != annotation_name)
      continue;

    if (an.type.base != reshadefx::type::t_string)
      continue;

    return an.value.string_data;
  }

  return default_value;
}

static bool GetBooleanAnnotationValue(const std::vector<reshadefx::annotation>& annotations,
                                      const std::string_view annotation_name, bool default_value)
{
  for (const reshadefx::annotation& an : annotations)
  {
    if (an.name != annotation_name)
      continue;

    if (an.type.base != reshadefx::type::t_bool)
      continue;

    return (an.value.as_int[0] != 0);
  }

  return default_value;
}

static PostProcessing::ShaderOption::ValueVector
GetVectorAnnotationValue(const reshadefx::uniform& uniform, const std::string_view annotation_name,
                         const PostProcessing::ShaderOption::ValueVector& default_value)
{
  PostProcessing::ShaderOption::ValueVector vv = default_value;
  for (const reshadefx::annotation& an : uniform.annotations)
  {
    if (an.name != annotation_name)
      continue;

    const u32 components = std::min<u32>(an.type.components(), PostProcessing::ShaderOption::MAX_VECTOR_COMPONENTS);

    if (an.type.base == uniform.type.base || (an.type.is_integral() && uniform.type.is_integral())) // int<->uint
    {
      if (components > 0)
        std::memcpy(&vv[0].float_value, &an.value.as_float[0], sizeof(float) * components);

      break;
    }
    else if (an.type.base == reshadefx::type::t_string)
    {
      // Convert from string.
      if (uniform.type.base == reshadefx::type::t_float)
      {
        if (an.value.string_data == "BUFFER_WIDTH")
          vv[0].float_value = DEFAULT_BUFFER_WIDTH;
        else if (an.value.string_data == "BUFFER_HEIGHT")
          vv[0].float_value = DEFAULT_BUFFER_HEIGHT;
        else
          vv[0].float_value = StringUtil::FromChars<float>(an.value.string_data).value_or(1000.0f);
      }
      else if (uniform.type.base == reshadefx::type::t_int)
      {
        if (an.value.string_data == "BUFFER_WIDTH")
          vv[0].int_value = DEFAULT_BUFFER_WIDTH;
        else if (an.value.string_data == "BUFFER_HEIGHT")
          vv[0].int_value = DEFAULT_BUFFER_HEIGHT;
        else
          vv[0].int_value = StringUtil::FromChars<s32>(an.value.string_data).value_or(1000);
      }
      else
      {
        ERROR_LOG("Unhandled string value for '{}' (annotation type: {}, uniform type {})", uniform.name,
                  an.type.description(), uniform.type.description());
      }

      break;
    }
    else if (an.type.base == reshadefx::type::t_int)
    {
      // Convert from int.
      if (uniform.type.base == reshadefx::type::t_float)
      {
        for (u32 i = 0; i < components; i++)
          vv[i].float_value = static_cast<float>(an.value.as_int[i]);
      }
      else if (uniform.type.base == reshadefx::type::t_bool)
      {
        for (u32 i = 0; i < components; i++)
          vv[i].int_value = (an.value.as_int[i] != 0) ? 1 : 0;
      }
    }
    else if (an.type.base == reshadefx::type::t_float)
    {
      // Convert from float.
      if (uniform.type.base == reshadefx::type::t_int)
      {
        for (u32 i = 0; i < components; i++)
          vv[i].int_value = static_cast<int>(an.value.as_float[i]);
      }
      else if (uniform.type.base == reshadefx::type::t_bool)
      {
        for (u32 i = 0; i < components; i++)
          vv[i].int_value = (an.value.as_float[i] != 0.0f) ? 1 : 0;
      }
    }

    break;
  }

  return vv;
}

bool PostProcessing::ReShadeFXShader::CreateOptions(const reshadefx::effect_module& mod, Error* error)
{
  for (const reshadefx::uniform& ui : mod.uniforms)
  {
    SourceOptionType so;
    if (!GetSourceOption(ui, &so, error))
      return false;
    if (so != SourceOptionType::None)
    {
      DEV_LOG("Add source based option {} at offset {} ({})", static_cast<u32>(so), ui.offset, ui.name);

      SourceOption sopt;
      sopt.source = so;
      sopt.offset = ui.offset;

      const ShaderOption::ValueVector min =
        GetVectorAnnotationValue(ui, "min", ShaderOption::MakeFloatVector(0, 0, 0, 0));
      const ShaderOption::ValueVector max =
        GetVectorAnnotationValue(ui, "max", ShaderOption::MakeFloatVector(1, 1, 1, 1));
      const ShaderOption::ValueVector smoothing =
        GetVectorAnnotationValue(ui, "smoothing", ShaderOption::MakeFloatVector(0));
      const ShaderOption::ValueVector step =
        GetVectorAnnotationValue(ui, "step", ShaderOption::MakeFloatVector(0, 1, 0, 0));

      sopt.min = min[0].float_value;
      sopt.max = max[0].float_value;
      sopt.smoothing = smoothing[0].float_value;
      std::memcpy(&sopt.step[0], &step[0].float_value, sizeof(sopt.value));
      std::memcpy(&sopt.value[0], &ui.initializer_value.as_float[0], sizeof(sopt.value));

      m_source_options.push_back(std::move(sopt));
      continue;
    }

    ShaderOption opt;
    opt.name = ui.name;
    opt.category = GetStringAnnotationValue(ui.annotations, "ui_category", std::string_view());
    opt.tooltip = GetStringAnnotationValue(ui.annotations, "ui_tooltip", std::string_view());
    opt.help_text = GetStringAnnotationValue(ui.annotations, "ui_text", std::string_view());

    if (!GetBooleanAnnotationValue(ui.annotations, "hidden", false))
    {
      opt.ui_name = GetStringAnnotationValue(ui.annotations, "ui_label", std::string_view());
      if (opt.ui_name.empty())
        opt.ui_name = ui.name;
    }

    const std::string_view ui_type = GetStringAnnotationValue(ui.annotations, "ui_type", std::string_view());

    switch (ui.type.base)
    {
      case reshadefx::type::t_float:
        opt.type = ShaderOption::Type::Float;
        break;

      case reshadefx::type::t_int:
      case reshadefx::type::t_uint:
        opt.type = ShaderOption::Type::Int;
        break;

      case reshadefx::type::t_bool:
        opt.type = ShaderOption::Type::Bool;
        break;

      default:
        Error::SetStringFmt(error, "Unhandled uniform type {} ({})", static_cast<u32>(ui.type.base), ui.name);
        return false;
    }

    opt.buffer_offset = ui.offset;
    opt.buffer_size = ui.size;
    opt.vector_size = ui.type.components();
    if (opt.vector_size == 0 || opt.vector_size > ShaderOption::MAX_VECTOR_COMPONENTS)
    {
      Error::SetStringFmt(error, "Unhandled vector size {} ({})", static_cast<u32>(ui.type.components()), ui.name);
      return false;
    }

    opt.min_value = GetVectorAnnotationValue(ui, "ui_min", opt.default_value);
    opt.max_value = GetVectorAnnotationValue(ui, "ui_max", opt.default_value);
    ShaderOption::ValueVector default_step = {};
    switch (opt.type)
    {
      case ShaderOption::Type::Float:
      {
        for (u32 i = 0; i < opt.vector_size; i++)
        {
          const float range = opt.max_value[i].float_value - opt.min_value[i].float_value;
          default_step[i].float_value = range / 100.0f;
        }
      }
      break;

      case ShaderOption::Type::Int:
      {
        for (u32 i = 0; i < opt.vector_size; i++)
        {
          const s32 range = opt.max_value[i].int_value - opt.min_value[i].int_value;
          default_step[i].int_value = std::max(range / 100, 1);
        }
      }
      break;

      default:
        break;
    }
    opt.step_value = GetVectorAnnotationValue(ui, "ui_step", default_step);

    // set a default maximum based on step if there isn't one
    if (!HasAnnotationWithName(ui, "ui_max") && HasAnnotationWithName(ui, "ui_step"))
    {
      for (u32 i = 0; i < opt.vector_size; i++)
      {
        switch (opt.type)
        {
          case ShaderOption::Type::Float:
            opt.max_value[i].float_value = opt.min_value[i].float_value + (opt.step_value[i].float_value * 100.0f);
            break;
          case ShaderOption::Type::Int:
            opt.max_value[i].int_value = opt.min_value[i].int_value + (opt.step_value[i].int_value * 100);
            break;
          default:
            break;
        }
      }
    }

    if (ui.has_initializer_value)
    {
      std::memcpy(&opt.default_value[0].float_value, &ui.initializer_value.as_float[0],
                  sizeof(float) * opt.vector_size);
    }
    else
    {
      opt.default_value = {};
    }

    // Assume default if user doesn't set it.
    opt.value = opt.default_value;

    if (!ui_type.empty() && opt.vector_size > 1)
    {
      WARNING_LOG("Uniform '{}' has UI type of '{}' but is vector not scalar ({}), ignoring", opt.name, ui_type,
                  opt.vector_size);
    }
    else if (!ui_type.empty())
    {
      if ((ui_type == "combo" || ui_type == "radio") && opt.type == ShaderOption::Type::Int)
      {
        const std::string_view ui_values = GetStringAnnotationValue(ui.annotations, "ui_items", std::string_view());

        size_t start_pos = 0;
        while (start_pos < ui_values.size())
        {
          size_t end_pos = start_pos;
          while (end_pos < ui_values.size() && ui_values[end_pos] != '\0')
            end_pos++;

          const size_t len = end_pos - start_pos;
          if (len > 0)
            opt.choice_options.emplace_back(ui_values.substr(start_pos, len));
          start_pos = end_pos + 1;
        }

        // update max if it hasn't been specified
        const size_t num_choices = opt.choice_options.size();
        if (num_choices > 0)
          opt.max_value[0].int_value = std::max(static_cast<s32>(num_choices - 1), opt.max_value[0].int_value);
      }
    }

    OptionList::iterator iter = std::find_if(m_options.begin(), m_options.end(),
                                             [&opt](const ShaderOption& it) { return it.category == opt.category; });
    if (iter != m_options.end())
    {
      // insert at the end of this category
      while (iter != m_options.end() && iter->category == opt.category)
        ++iter;
    }
    m_options.insert(iter, std::move(opt));
  }

  m_uniforms_size = mod.total_uniform_size;
  DEV_LOG("{}: {} options", m_filename, m_options.size());
  return true;
}

bool PostProcessing::ReShadeFXShader::GetSourceOption(const reshadefx::uniform& ui, SourceOptionType* si, Error* error)
{
  // TODO: Rewrite these to a lookup table instead, this if chain is terrible.
  const std::string_view source = GetStringAnnotationValue(ui.annotations, "source", {});
  if (!source.empty())
  {
    if (source == "timer")
    {
      if (ui.type.base != reshadefx::type::t_float || ui.type.components() > 1)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for timer source in uniform '{}'", ui.type.description(),
                            ui.name);
        return false;
      }

      *si = SourceOptionType::Timer;
      return true;
    }
    else if (source == "framecount")
    {
      if ((!ui.type.is_integral() && !ui.type.is_floating_point()) || ui.type.components() > 1)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for timer source in uniform '{}'", ui.type.description(),
                            ui.name);
        return false;
      }

      *si = (ui.type.base == reshadefx::type::t_float) ? SourceOptionType::FrameCountF : SourceOptionType::FrameCount;
      return true;
    }
    else if (source == "frametime")
    {
      if ((!ui.type.is_integral() && !ui.type.is_floating_point()) || ui.type.components() > 1)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for timer source in uniform '{}'", ui.type.description(),
                            ui.name);
        return false;
      }

      // If it's an integer type, value is going to be garbage, user can deal with it.
      *si = SourceOptionType::FrameTime;
      return true;
    }
    else if (source == "pingpong")
    {
      if (!ui.type.is_floating_point() || ui.type.components() < 2)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for pingpong source in uniform '{}'", ui.type.description(),
                            ui.name);
        return false;
      }

      *si = SourceOptionType::PingPong;
      return true;
    }
    else if (source == "mousepoint")
    {
      if (!ui.type.is_floating_point() || ui.type.components() < 2)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for mousepoint source in uniform '{}'", ui.type.description(),
                            ui.name);
        return false;
      }

      *si = SourceOptionType::MousePoint;
      return true;
    }
    else if (source == "mousebutton")
    {
      WARNING_LOG("Ignoring mousebutton source in uniform '{}', not supported.", ui.name);
      *si = SourceOptionType::Zero;
      return true;
    }
    else if (source == "random")
    {
      if ((!ui.type.is_floating_point() && !ui.type.is_integral()) || ui.type.components() != 1)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' ({} components) for random source in uniform '{}'",
                            ui.type.description(), ui.type.components(), ui.name);
        return false;
      }

      // TODO: This is missing min/max handling.
      *si = (ui.type.base == reshadefx::type::t_float) ? SourceOptionType::RandomF : SourceOptionType::Random;
      return true;
    }
    else if (source == "overlay_active")
    {
      *si = SourceOptionType::Zero;
      return true;
    }
    else if (source == "has_depth")
    {
      *si = SourceOptionType::HasDepth;
      return true;
    }
    else if (source == "bufferwidth")
    {
      *si = (ui.type.base == reshadefx::type::t_float) ? SourceOptionType::BufferWidthF : SourceOptionType::BufferWidth;
      return true;
    }
    else if (source == "bufferheight")
    {
      *si =
        (ui.type.base == reshadefx::type::t_float) ? SourceOptionType::BufferHeightF : SourceOptionType::BufferHeight;
      return true;
    }
    else if (source == "internalwidth")
    {
      *si =
        (ui.type.base == reshadefx::type::t_float) ? SourceOptionType::InternalWidthF : SourceOptionType::InternalWidth;
      return true;
    }
    else if (source == "internalheight")
    {
      *si = (ui.type.base == reshadefx::type::t_float) ? SourceOptionType::InternalHeightF :
                                                         SourceOptionType::InternalHeight;
      return true;
    }
    else if (source == "nativewidth")
    {
      *si = (ui.type.base == reshadefx::type::t_float) ? SourceOptionType::NativeWidthF : SourceOptionType::NativeWidth;
      return true;
    }
    else if (source == "nativeheight")
    {
      *si =
        (ui.type.base == reshadefx::type::t_float) ? SourceOptionType::NativeHeightF : SourceOptionType::NativeHeight;
      return true;
    }
    else if (source == "upscale_multiplier")
    {
      if (!ui.type.is_floating_point() || ui.type.components() != 1)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for {} source in uniform '{}'", ui.type.description(), source,
                            ui.name);
        return false;
      }

      *si = SourceOptionType::UpscaleMultiplier;
      return true;
    }
    else if (source == "viewportx")
    {
      if (!ui.type.is_floating_point() || ui.type.components() != 1)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for {} source in uniform '{}'", ui.type.description(), source,
                            ui.name);
        return false;
      }

      *si = SourceOptionType::ViewportX;
      return true;
    }
    else if (source == "viewporty")
    {
      if (!ui.type.is_floating_point() || ui.type.components() != 1)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for {} source in uniform '{}'", ui.type.description(), source,
                            ui.name);
        return false;
      }

      *si = SourceOptionType::ViewportY;
      return true;
    }
    else if (source == "viewportwidth")
    {
      if (!ui.type.is_floating_point() || ui.type.components() != 1)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for {} source in uniform '{}'", ui.type.description(), source,
                            ui.name);
        return false;
      }

      *si = SourceOptionType::ViewportWidth;
      return true;
    }
    else if (source == "viewportheight")
    {
      if (!ui.type.is_floating_point() || ui.type.components() != 1)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for {} source in uniform '{}'", ui.type.description(), source,
                            ui.name);
        return false;
      }

      *si = SourceOptionType::ViewportHeight;
      return true;
    }
    else if (source == "viewportoffset")
    {
      if (!ui.type.is_floating_point() || ui.type.components() != 2)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for {} source in uniform '{}'", ui.type.description(), source,
                            ui.name);
        return false;
      }

      *si = SourceOptionType::ViewportOffset;
      return true;
    }
    else if (source == "viewportsize")
    {
      if (!ui.type.is_floating_point() || ui.type.components() != 2)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for {} source in uniform '{}'", ui.type.description(), source,
                            ui.name);
        return false;
      }

      *si = SourceOptionType::ViewportSize;
      return true;
    }
    else if (source == "internal_pixel_size")
    {
      if (!ui.type.is_floating_point() || ui.type.components() != 2)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for {} source in uniform '{}'", ui.type.description(), source,
                            ui.name);
        return false;
      }

      *si = SourceOptionType::InternalPixelSize;
      return true;
    }
    else if (source == "normalized_internal_pixel_size")
    {
      if (!ui.type.is_floating_point() || ui.type.components() != 2)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for {} source in uniform '{}'", ui.type.description(), source,
                            ui.name);
        return false;
      }

      *si = SourceOptionType::InternalNormPixelSize;
      return true;
    }
    else if (source == "native_pixel_size")
    {
      if (!ui.type.is_floating_point() || ui.type.components() != 2)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for {} source in uniform '{}'", ui.type.description(), source,
                            ui.name);
        return false;
      }

      *si = SourceOptionType::NativePixelSize;
      return true;
    }
    else if (source == "normalized_native_pixel_size")
    {
      if (!ui.type.is_floating_point() || ui.type.components() != 2)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for {} source in uniform '{}'", ui.type.description(), source,
                            ui.name);
        return false;
      }

      *si = SourceOptionType::NativeNormPixelSize;
      return true;
    }
    else if (source == "buffer_to_viewport_ratio")
    {
      if (!ui.type.is_floating_point() || ui.type.components() != 2)
      {
        Error::SetStringFmt(error, "Unexpected type '{}' for {} source in uniform '{}'", ui.type.description(), source,
                            ui.name);
        return false;
      }

      *si = SourceOptionType::BufferToViewportRatio;
      return true;
    }
    else
    {
      Error::SetStringFmt(error, "Unknown source '{}' in uniform '{}'", source, ui.name);
      return false;
    }
  }

  if (ui.has_initializer_value)
  {
    if (ui.initializer_value.string_data == "BUFFER_WIDTH")
    {
      *si = (ui.type.base == reshadefx::type::t_float) ? SourceOptionType::BufferWidthF : SourceOptionType::BufferWidth;
      return true;
    }
    else if (ui.initializer_value.string_data == "BUFFER_HEIGHT")
    {
      *si =
        (ui.type.base == reshadefx::type::t_float) ? SourceOptionType::BufferHeightF : SourceOptionType::BufferHeight;
      return true;
    }
  }

  *si = SourceOptionType::None;
  return true;
}

bool PostProcessing::ReShadeFXShader::CreatePasses(GPUTexture::Format backbuffer_format,
                                                   const reshadefx::effect_module& mod, Error* error)
{
  u32 total_passes = 0;
  for (const reshadefx::technique& tech : mod.techniques)
    total_passes += static_cast<u32>(tech.passes.size());
  if (total_passes == 0)
  {
    Error::SetString(error, "No passes defined.");
    return false;
  }

  m_passes.reserve(total_passes);

  // Named render targets.
  for (const reshadefx::texture& ti : mod.textures)
  {
    Texture tex;
    tex.storage_access = false;
    tex.render_target = false;
    tex.render_target_width = 0;
    tex.render_target_height = 0;

    if (!ti.semantic.empty())
    {
      DEV_LOG("Ignoring semantic {} texture {}", ti.semantic, ti.unique_name);
      continue;
    }
    if (ti.render_target || ti.storage_access)
    {
      tex.format = MapTextureFormat(ti.format);
      tex.render_target = true;
      tex.storage_access = ti.storage_access;
      tex.render_target_width = ti.width;
      tex.render_target_height = ti.height;
      DEV_LOG("Creating {}x{} render target{} '{}' {}", ti.width, ti.height,
              ti.storage_access ? " with storage access" : "", ti.unique_name, GPUTexture::GetFormatName(tex.format));
    }
    else
    {
      const std::string_view source = GetStringAnnotationValue(ti.annotations, "source", {});
      if (source.empty())
      {
        Error::SetStringFmt(error, "Non-render target texture '{}' is missing source.", ti.unique_name);
        return false;
      }

      Image image;
      if (const std::string image_path =
            Path::Combine(EmuFolders::Shaders, Path::Combine("reshade" FS_OSPATH_SEPARATOR_STR "Textures", source));
          !image.LoadFromFile(image_path.c_str()))
      {
        // Might be a base file/resource instead.
        const std::string resource_name = Path::Combine("shaders/reshade/Textures", source);
        if (std::optional<DynamicHeapArray<u8>> resdata = Host::ReadResourceFile(resource_name.c_str(), true, error);
            !resdata.has_value() || !image.LoadFromBuffer(resource_name.c_str(), resdata->cspan(), error))
        {
          Error::AddPrefixFmt(error, "Failed to load image '{}' (from '{}'): ", source, image_path);
          return false;
        }
      }

      tex.texture = g_gpu_device->FetchTexture(image.GetWidth(), image.GetHeight(), 1, 1, 1, GPUTexture::Type::Texture,
                                               GPUTexture::Format::RGBA8, GPUTexture::Flags::None, image.GetPixels(),
                                               image.GetPitch(), error);
      if (!tex.texture)
        return false;

      DEV_LOG("Loaded {}x{} texture ({})", image.GetWidth(), image.GetHeight(), source);
    }

    tex.reshade_name = ti.unique_name;
    m_textures.push_back(std::move(tex));
  }

  // need potentially up to two backbuffers
  std::array<std::optional<TextureID>, 2> backbuffer_texture_ids;
  std::optional<TextureID> read_backbuffer;
  u32 current_backbuffer = 0;

  for (const reshadefx::technique& tech : mod.techniques)
  {
    for (const reshadefx::pass& pi : tech.passes)
    {
      const bool is_final = (&tech == &mod.techniques.back() && &pi == &tech.passes.back());

      Pass pass;
      pass.num_vertices = pi.num_vertices;
      pass.is_compute = !pi.cs_entry_point.empty();
      pass.clear_render_targets = pi.clear_render_targets;
      pass.dispatch_size[0] = Truncate16(pi.viewport_width);
      pass.dispatch_size[1] = Truncate16(pi.viewport_height);
      pass.dispatch_size[2] = Truncate16(pi.viewport_dispatch_z);

      if (pass.is_compute)
      {
        if (is_final)
        {
          Error::SetStringFmt(error, "Compute pass '{}' cannot be final pass", pi.name);
          return false;
        }
        else if (!pi.render_target_names[0].empty())
        {
          Error::SetStringFmt(error, "Compute pass '{}' has render target", pi.name);
          return false;
        }

        TextureID rts[GPUDevice::MAX_RENDER_TARGETS];
        for (TextureID& rt : rts)
          rt = static_cast<TextureID>(m_textures.size());

        // storage images => bind RT as image
        for (const reshadefx::storage_binding& sb : pi.storage_bindings)
        {
          if (sb.binding >= GPUDevice::MAX_RENDER_TARGETS)
          {
            Error::SetStringFmt(error, "Compute pass '{}' has render target has out-of-range image binding {}", pi.name,
                                sb.binding);
            return false;
          }

          if (rts[sb.binding] != static_cast<TextureID>(m_textures.size()))
            continue;

          for (const reshadefx::texture& ti : mod.textures)
          {
            if (ti.unique_name == sb.texture_name)
            {
              // must be a render target, or another texture
              for (u32 i = 0; i < static_cast<u32>(m_textures.size()); i++)
              {
                if (m_textures[i].reshade_name == ti.unique_name)
                {
                  // hook it up
                  rts[sb.binding] = static_cast<TextureID>(i);
                  break;
                }
              }

              break;
            }
          }

          if (rts[sb.binding] == static_cast<TextureID>(m_textures.size()))
          {
            Error::SetStringFmt(error, "Compute pass '{}' has unknown image '{}' at binding {}", pi.name,
                                sb.texture_name, sb.binding);
            return false;
          }
        }

        // must be consecutive
        for (u32 i = 0; i < GPUDevice::MAX_RENDER_TARGETS; i++)
        {
          if (rts[i] == static_cast<TextureID>(m_textures.size()))
            continue;

          if (i > 0 && rts[i - 1] == static_cast<TextureID>(m_textures.size()))
          {
            Error::SetStringFmt(error, "Compute pass '{}' has non-consecutive image bindings (index {})", pi.name, i);
            return false;
          }

          pass.render_targets.push_back(rts[i]);
        }
      }
      else if (is_final)
      {
        pass.render_targets.push_back(OUTPUT_COLOR_TEXTURE);
      }
      else if (!pi.render_target_names[0].empty())
      {
        for (const std::string& rtname : pi.render_target_names)
        {
          if (rtname.empty())
            break;

          TextureID rt = static_cast<TextureID>(m_textures.size());
          for (u32 i = 0; i < static_cast<u32>(m_textures.size()); i++)
          {
            if (m_textures[i].reshade_name == rtname)
            {
              rt = static_cast<TextureID>(i);
              break;
            }
          }
          if (rt == static_cast<TextureID>(m_textures.size()))
          {
            Error::SetStringFmt(error, "Unknown texture '{}' used as render target in pass '{}'", rtname, pi.name);
            return false;
          }

          pass.render_targets.push_back(rt);
        }
      }
      else
      {
        // swap to the other backbuffer, sample from the previous written
        if (backbuffer_texture_ids[current_backbuffer].has_value())
        {
          read_backbuffer = backbuffer_texture_ids[current_backbuffer];
          current_backbuffer ^= 1;
        }

        if (!backbuffer_texture_ids[current_backbuffer].has_value())
        {
          Texture new_rt;
          new_rt.format = backbuffer_format;
          new_rt.render_target = true;
          new_rt.storage_access = false;
          new_rt.render_target_width = 0;
          new_rt.render_target_height = 0;
          new_rt.reshade_name = fmt::format("| BackBuffer{} |", current_backbuffer);

          backbuffer_texture_ids[current_backbuffer] = static_cast<TextureID>(m_textures.size());
          m_textures.push_back(std::move(new_rt));
        }

        pass.render_targets.push_back(backbuffer_texture_ids[current_backbuffer].value());
      }

      u32 texture_slot = 0;
      Assert(pi.texture_bindings.size() == pi.sampler_bindings.size());
      for (size_t tb_index = 0; tb_index < pi.texture_bindings.size(); tb_index++)
      {
        const reshadefx::texture_binding& tb = pi.texture_bindings[tb_index];
        const reshadefx::sampler_binding& sb = pi.sampler_bindings[tb_index];

        Sampler sampler;
        sampler.slot = texture_slot++;

        sampler.texture_id = static_cast<TextureID>(m_textures.size());
        for (const reshadefx::texture& ti : mod.textures)
        {
          if (ti.unique_name == tb.texture_name)
          {
            sampler.reshade_name = ti.unique_name; // TODO: REMOVE THIS

            // found the texture, now look for our side of it
            if (ti.semantic == "COLOR")
            {
              sampler.texture_id = read_backbuffer.value_or(INPUT_COLOR_TEXTURE);
              break;
            }
            else if (ti.semantic == "DEPTH")
            {
              sampler.texture_id = INPUT_DEPTH_TEXTURE;
              m_wants_depth_buffer = true;
              break;
            }
            else if (!ti.semantic.empty())
            {
              Error::SetStringFmt(error, "Unknown semantic {} in texture {}", ti.semantic, ti.name);
              return false;
            }

            // must be a render target, or another texture
            for (u32 i = 0; i < static_cast<u32>(m_textures.size()); i++)
            {
              if (m_textures[i].reshade_name == ti.unique_name)
              {
                // hook it up
                sampler.texture_id = static_cast<TextureID>(i);
                break;
              }
            }

            break;
          }
        }

        if (sampler.texture_id == static_cast<TextureID>(m_textures.size()))
        {
          Error::SetStringFmt(error, "Unknown texture {} in pass {}", tb.texture_name, pi.name);
          return false;
        }

        DEV_LOG("Pass {} Texture {} => {}", pi.name, tb.texture_name, sampler.texture_id);

        sampler.sampler = g_gpu_device->GetSampler(MapSampler(sb));
        if (!sampler.sampler)
        {
          Error::SetString(error, "Failed to create sampler.");
          return false;
        }

        pass.samplers.push_back(std::move(sampler));
      }

#ifdef ENABLE_GPU_OBJECT_NAMES
      pass.name = std::move(pi.name);
#endif
      m_passes.push_back(std::move(pass));
    }
  }

  return true;
}

const char* PostProcessing::ReShadeFXShader::GetTextureNameForID(TextureID id) const
{
  if (id == INPUT_COLOR_TEXTURE)
    return "Input Color Texture / Backbuffer";
  else if (id == INPUT_DEPTH_TEXTURE)
    return "Input Depth Texture";
  else if (id == OUTPUT_COLOR_TEXTURE)
    return "Output Color Texture";
  else if (id < 0 || static_cast<size_t>(id) >= m_textures.size())
    return "UNKNOWN";
  else
    return m_textures[static_cast<size_t>(id)].reshade_name.c_str();
}

GPUTexture* PostProcessing::ReShadeFXShader::GetTextureByID(TextureID id, GPUTexture* input_color,
                                                            GPUTexture* input_depth, GPUTexture* final_target) const
{
  if (id < 0)
  {
    if (id == INPUT_COLOR_TEXTURE)
    {
      return input_color;
    }
    else if (id == INPUT_DEPTH_TEXTURE)
    {
      return input_depth ? input_depth : g_gpu_device->GetEmptyTexture();
    }
    else if (id == OUTPUT_COLOR_TEXTURE)
    {
      return final_target;
    }
    else
    {
      Panic("Unexpected reserved texture ID");
      return nullptr;
    }
  }

  if (static_cast<size_t>(id) >= m_textures.size())
    Panic("Unexpected texture ID");

  return m_textures[static_cast<size_t>(id)].texture.get();
}

bool PostProcessing::ReShadeFXShader::CompilePipeline(GPUTexture::Format format, u32 width, u32 height, Error* error,
                                                      ProgressCallback* progress)
{
  m_textures.clear();
  m_passes.clear();
  m_wants_depth_buffer = false;

  std::string fxcode;
  if (!PreprocessorReadFileCallback(m_filename, fxcode))
  {
    ERROR_LOG("Failed to re-read shader for pipeline: '{}'", m_filename);
    return false;
  }

  // Reshade's preprocessor expects this.
  if (fxcode.empty() || fxcode.back() != '\n')
    fxcode.push_back('\n');

  const auto& [cg, cg_language] = CreateRFXCodegen(false, error);
  if (!cg)
    return false;

  if (!CreateModule(width, height, cg.get(), cg_language, std::move(fxcode), error))
  {
    Error::AddPrefix(error, "Failed to create module: ");
    return false;
  }

  const reshadefx::effect_module& mod = cg->module();
  DebugAssert(!mod.techniques.empty());

  if (!CreatePasses(format, mod, error))
  {
    Error::AddPrefix(error, "Failed to create passes: ");
    return false;
  }

  auto get_shader = [cg_language, &cg, error](const std::string& name, GPUShaderStage stage) {
    const std::string real_code = cg->finalize_code_for_entry_point(name);
    const char* entry_point = (cg_language == GPUShaderLanguage::HLSL) ? name.c_str() : "main";

    std::unique_ptr<GPUShader> sshader = g_gpu_device->CreateShader(stage, cg_language, real_code, error, entry_point);
    if (!sshader)
      Error::AddPrefixFmt(error, "Failed to compile function '{}': ", name);

    return sshader;
  };

  GPUPipeline::GraphicsConfig plconfig;
  plconfig.layout = GPUPipeline::Layout::MultiTextureAndUBO;
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.depth_format = GPUTexture::Format::Unknown;
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;

  GPUPipeline::ComputeConfig cplconfig;
  cplconfig.layout = GPUPipeline::Layout::ComputeMultiTextureAndUBO;

  progress->PushState();

  size_t total_passes = 0;
  for (const reshadefx::technique& tech : mod.techniques)
    total_passes += tech.passes.size();
  progress->SetProgressRange(static_cast<u32>(total_passes));
  progress->SetProgressValue(0);

  u32 passnum = 0;
  for (const reshadefx::technique& tech : mod.techniques)
  {
    for (const reshadefx::pass& info : tech.passes)
    {
      DebugAssert(passnum < m_passes.size());
      Pass& pass = m_passes[passnum++];

      if (!info.cs_entry_point.empty())
      {
        if (!info.vs_entry_point.empty() || !info.ps_entry_point.empty())
        {
          Error::SetStringFmt(error, "Pass {} has both graphics and compute shaders", info.name);
          progress->PopState();
          return false;
        }

        auto cs = get_shader(info.cs_entry_point, GPUShaderStage::Compute);
        if (!cs)
        {
          progress->PopState();
          return false;
        }

        cplconfig.compute_shader = cs.get();

        pass.pipeline = g_gpu_device->CreatePipeline(cplconfig, error);
        if (!pass.pipeline)
        {
          Error::AddPrefixFmt(error, "Failed to create compute pipeline for pass '{}': ", info.name);
          progress->PopState();
          return false;
        }
      }
      else
      {
        auto vs = get_shader(info.vs_entry_point, GPUShaderStage::Vertex);
        auto fs = get_shader(info.ps_entry_point, GPUShaderStage::Fragment);
        if (!vs || !fs)
        {
          progress->PopState();
          return false;
        }

        for (size_t i = 0; i < pass.render_targets.size(); i++)
        {
          plconfig.color_formats[i] =
            ((pass.render_targets[i] >= 0) ? m_textures[pass.render_targets[i]].format : format);
        }
        for (size_t i = pass.render_targets.size(); i < GPUDevice::MAX_RENDER_TARGETS; i++)
          plconfig.color_formats[i] = GPUTexture::Format::Unknown;
        plconfig.depth_format = GPUTexture::Format::Unknown;

        plconfig.blend = MapBlendState(info);
        plconfig.primitive = MapPrimitive(info.topology);
        plconfig.vertex_shader = vs.get();
        plconfig.fragment_shader = fs.get();
        plconfig.geometry_shader = nullptr;
        if (!plconfig.vertex_shader || !plconfig.fragment_shader)
        {
          progress->PopState();
          return false;
        }

        pass.pipeline = g_gpu_device->CreatePipeline(plconfig, error);
        if (!pass.pipeline)
        {
          Error::AddPrefixFmt(error, "Failed to create pipeline for pass '{}': ", info.name);
          progress->PopState();
          return false;
        }
      }

      progress->SetProgressValue(passnum);
    }
  }

  progress->PopState();

  return true;
}

bool PostProcessing::ReShadeFXShader::ResizeTargets(u32 source_width, u32 source_height,
                                                    GPUTexture::Format target_format, u32 target_width,
                                                    u32 target_height, u32 viewport_width, u32 viewport_height,
                                                    Error* error)
{
  for (Texture& tex : m_textures)
  {
    if (!tex.render_target)
      continue;

    const u32 t_width = (tex.render_target_width > 0) ? tex.render_target_width : target_width;
    const u32 t_height = (tex.render_target_height > 0) ? tex.render_target_height : target_height;
    if (!g_gpu_device->ResizeTexture(&tex.texture, t_width, t_height, GPUTexture::Type::RenderTarget, tex.format,
                                     tex.storage_access ? GPUTexture::Flags::AllowBindAsImage : GPUTexture::Flags::None,
                                     false, error))
    {
      return false;
    }
  }

  return true;
}

GPUDevice::PresentResult PostProcessing::ReShadeFXShader::Apply(GPUTexture* input_color, GPUTexture* input_depth,
                                                                GPUTexture* final_target, GSVector4i final_rect,
                                                                s32 orig_width, s32 orig_height, s32 native_width,
                                                                s32 native_height, u32 target_width, u32 target_height,
                                                                float time)
{
  GL_SCOPE_FMT("PostProcessingShaderFX {}", m_name);

  m_frame_count++;

  // Reshade timer variable is in milliseconds.
  time *= 1000.0f;

  if (m_uniforms_size > 0)
  {
    GL_SCOPE_FMT("Uniforms: {} bytes", m_uniforms_size);

    u8* uniforms = static_cast<u8*>(g_gpu_device->MapUniformBuffer(m_uniforms_size));
    for (const ShaderOption& opt : m_options)
    {
      DebugAssert((opt.buffer_offset + opt.buffer_size) <= m_uniforms_size);
      std::memcpy(uniforms + opt.buffer_offset, &opt.value[0].float_value, opt.buffer_size);
    }
    for (const SourceOption& so : m_source_options)
    {
      u8* dst = uniforms + so.offset;
      switch (so.source)
      {
        case SourceOptionType::Zero:
        {
          const u32 value = 0;
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::HasDepth:
        {
          const u32 value = BoolToUInt32(input_depth != nullptr);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::Timer:
        {
          std::memcpy(dst, &time, sizeof(time));
        }
        break;

        case SourceOptionType::FrameTime:
        {
          const float value = static_cast<float>(m_frame_timer.GetTimeMilliseconds());
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::FrameCount:
        {
          std::memcpy(dst, &m_frame_count, sizeof(m_frame_count));
        }
        break;

        case SourceOptionType::FrameCountF:
        {
          const float value = static_cast<float>(m_frame_count);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::PingPong:
        {
          float increment = so.step[1] == 0 ?
                              so.step[0] :
                              (so.step[0] + std::fmod(static_cast<float>(std::rand()), so.step[1] - so.step[0] + 1));

          std::array<float, 2> value = {so.value[0].float_value, so.value[1].float_value};
          if (value[1] >= 0)
          {
            increment = std::max(increment - std::max(0.0f, so.smoothing - (so.max - value[0])), 0.05f);
            increment *= static_cast<float>(m_frame_timer.GetTimeMilliseconds() * 1e-9);

            if ((value[0] += increment) >= so.max)
            {
              value[0] = so.max;
              value[1] = -1;
            }
          }
          else
          {
            increment = std::max(increment - std::max(0.0f, so.smoothing - (value[0] - so.min)), 0.05f);
            increment *= static_cast<float>(m_frame_timer.GetTimeMilliseconds() * 1e-9);

            if ((value[0] -= increment) <= so.min)
            {
              value[0] = so.min;
              value[1] = +1;
            }
          }

          std::memcpy(dst, value.data(), sizeof(value));
        }
        break;

        case SourceOptionType::MousePoint:
        {
          const std::pair<float, float> mpos = InputManager::GetPointerAbsolutePosition(0);
          std::memcpy(dst, &mpos.first, sizeof(float));
          std::memcpy(dst + sizeof(float), &mpos.second, sizeof(float));
        }
        break;

        case SourceOptionType::Random:
        {
          const s32 rv = m_random() % 32767; // reshade uses rand(), which on some platforms has a 0x7fff maximum.
          std::memcpy(dst, &rv, sizeof(rv));
        }
        break;
        case SourceOptionType::RandomF:
        {
          const float rv = (m_random() - m_random.min()) / static_cast<float>(m_random.max() - m_random.min());
          std::memcpy(dst, &rv, sizeof(rv));
        }
        break;

        case SourceOptionType::BufferWidth:
        case SourceOptionType::BufferHeight:
        {
          const s32 value = (so.source == SourceOptionType::BufferWidth) ? static_cast<s32>(target_width) :
                                                                           static_cast<s32>(target_height);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::BufferWidthF:
        case SourceOptionType::BufferHeightF:
        {
          const float value = (so.source == SourceOptionType::BufferWidthF) ? static_cast<float>(target_width) :
                                                                              static_cast<float>(target_height);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::InternalWidth:
        case SourceOptionType::InternalHeight:
        {
          const s32 value = (so.source == SourceOptionType::InternalWidth) ? static_cast<s32>(orig_width) :
                                                                             static_cast<s32>(orig_height);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::InternalWidthF:
        case SourceOptionType::InternalHeightF:
        {
          const float value = (so.source == SourceOptionType::InternalWidthF) ? static_cast<float>(orig_width) :
                                                                                static_cast<float>(orig_height);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::NativeWidth:
        case SourceOptionType::NativeHeight:
        {
          const s32 value = (so.source == SourceOptionType::NativeWidth) ? static_cast<s32>(native_width) :
                                                                           static_cast<s32>(native_height);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::NativeWidthF:
        case SourceOptionType::NativeHeightF:
        {
          const float value = (so.source == SourceOptionType::NativeWidthF) ? static_cast<float>(native_width) :
                                                                              static_cast<float>(native_height);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::UpscaleMultiplier:
        {
          const float value = static_cast<float>(orig_width) / static_cast<float>(native_width);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::ViewportX:
        {
          const float value = static_cast<float>(final_rect.left);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::ViewportY:
        {
          const float value = static_cast<float>(final_rect.top);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::ViewportWidth:
        {
          const float value = static_cast<float>(final_rect.width());
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::ViewportHeight:
        {
          const float value = static_cast<float>(final_rect.height());
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::ViewportOffset:
        {
          GSVector4::storel<false>(dst, GSVector4(final_rect));
        }
        break;

        case SourceOptionType::ViewportSize:
        {
          const float value[2] = {static_cast<float>(final_rect.width()), static_cast<float>(final_rect.height())};
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::InternalPixelSize:
        {
          const float value[2] = {static_cast<float>(final_rect.width()) / static_cast<float>(orig_width),
                                  static_cast<float>(final_rect.height()) / static_cast<float>(orig_height)};
          std::memcpy(dst, value, sizeof(value));
        }
        break;

        case SourceOptionType::InternalNormPixelSize:
        {
          const float value[2] = {(static_cast<float>(final_rect.width()) / static_cast<float>(orig_width)) /
                                    static_cast<float>(target_width),
                                  (static_cast<float>(final_rect.height()) / static_cast<float>(orig_height)) /
                                    static_cast<float>(target_height)};
          std::memcpy(dst, value, sizeof(value));
        }
        break;

        case SourceOptionType::NativePixelSize:
        {
          const float value[2] = {static_cast<float>(final_rect.width()) / static_cast<float>(native_width),
                                  static_cast<float>(final_rect.height()) / static_cast<float>(native_height)};
          std::memcpy(dst, value, sizeof(value));
        }
        break;

        case SourceOptionType::NativeNormPixelSize:
        {
          const float value[2] = {(static_cast<float>(final_rect.width()) / static_cast<float>(native_width)) /
                                    static_cast<float>(target_width),
                                  (static_cast<float>(final_rect.height()) / static_cast<float>(native_height)) /
                                    static_cast<float>(target_height)};
          std::memcpy(dst, value, sizeof(value));
        }
        break;

        case SourceOptionType::BufferToViewportRatio:
        {
          const float value[2] = {static_cast<float>(target_width) / static_cast<float>(final_rect.width()),
                                  static_cast<float>(target_height) / static_cast<float>(final_rect.height())};
          std::memcpy(dst, value, sizeof(value));
        }
        break;

        default:
          UnreachableCode();
          break;
      }
    }
    g_gpu_device->UnmapUniformBuffer(m_uniforms_size);
  }

  for (const Pass& pass : m_passes)
  {
    GL_SCOPE_FMT("Draw pass {}", pass.name.c_str());
    DebugAssert(!pass.render_targets.empty());

    // Sucks doing this twice, but we need to set the RT first (for DX11), and transition layouts (for VK).
    for (const Sampler& sampler : pass.samplers)
    {
      GPUTexture* const tex = GetTextureByID(sampler.texture_id, input_color, input_depth, final_target);
      if (tex)
        tex->MakeReadyForSampling();
    }

    if (pass.render_targets.size() == 1 && pass.render_targets[0] == OUTPUT_COLOR_TEXTURE && !final_target)
    {
      // Special case: drawing to final buffer.
      GPUSwapChain* swap_chain = g_gpu_device->GetMainSwapChain();
      const GPUDevice::PresentResult pres = g_gpu_device->BeginPresent(swap_chain);
      if (pres != GPUDevice::PresentResult::OK)
        return pres;

      g_gpu_device->SetViewportAndScissor(GSVector4i::loadh(swap_chain->GetSizeVec()));
    }
    else
    {
      std::array<GPUTexture*, GPUDevice::MAX_RENDER_TARGETS> render_targets;
      for (size_t i = 0; i < pass.render_targets.size(); i++)
      {
        GL_INS_FMT("Render Target {}: ID {} [{}]", i, pass.render_targets[i],
                   GetTextureNameForID(pass.render_targets[i]));
        render_targets[i] = GetTextureByID(pass.render_targets[i], input_color, input_depth, final_target);
        DebugAssert(render_targets[i]);

        if (pass.clear_render_targets)
          g_gpu_device->ClearRenderTarget(render_targets[i], 0);
      }

      if (!pass.is_compute)
      {
        g_gpu_device->SetRenderTargets(render_targets.data(), static_cast<u32>(pass.render_targets.size()), nullptr);
        if (!pass.render_targets.empty())
          g_gpu_device->SetViewportAndScissor(GSVector4i::loadh(render_targets[0]->GetSizeVec()));
      }
      else
      {
        g_gpu_device->SetRenderTargets(render_targets.data(), static_cast<u32>(pass.render_targets.size()), nullptr,
                                       GPUPipeline::BindRenderTargetsAsImages);
      }
    }

    g_gpu_device->SetPipeline(pass.pipeline.get());

    // Set all inputs first, before the render pass starts.
    std::bitset<GPUDevice::MAX_TEXTURE_SAMPLERS> bound_textures = {};
    for (const Sampler& sampler : pass.samplers)
    {
      // Can't bind the RT as a sampler.
      if (std::any_of(pass.render_targets.begin(), pass.render_targets.end(),
                      [&sampler](TextureID rt) { return rt == sampler.texture_id; }))
      {
        GL_INS_FMT("Not binding RT sampler {}: ID {} [{}]", sampler.slot, sampler.texture_id,
                   GetTextureNameForID(sampler.texture_id));
        continue;
      }

      GL_INS_FMT("Texture Sampler {}: ID {} [{}]", sampler.slot, sampler.texture_id,
                 GetTextureNameForID(sampler.texture_id));
      g_gpu_device->SetTextureSampler(
        sampler.slot, GetTextureByID(sampler.texture_id, input_color, input_depth, final_target), sampler.sampler);
      bound_textures[sampler.slot] = true;
    }

    // Ensure RT wasn't left bound as a previous output, it breaks VK/DX12.
    // TODO: Maybe move this into the backend? Not sure...
    for (u32 i = 0; i < GPUDevice::MAX_TEXTURE_SAMPLERS; i++)
    {
      if (!bound_textures[i])
        g_gpu_device->SetTextureSampler(i, nullptr, nullptr);
    }

    // TODO: group size is incorrect for Metal
    if (pass.is_compute)
      g_gpu_device->Dispatch(pass.dispatch_size[0], pass.dispatch_size[1], pass.dispatch_size[2], 1, 1, 1);
    else
      g_gpu_device->Draw(pass.num_vertices, 0);
  }

  // Don't leave any textures bound.
  for (u32 i = 0; i < GPUDevice::MAX_TEXTURE_SAMPLERS; i++)
    g_gpu_device->SetTextureSampler(i, nullptr, nullptr);

  m_frame_timer.Reset();
  return GPUDevice::PresentResult::OK;
}
