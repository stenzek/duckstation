// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "postprocessing_shader_fx.h"
#include "image.h"
#include "input_manager.h"
#include "shadergen.h"

// TODO: Remove me
#include "core/host.h"
#include "core/settings.h"

#include "common/assert.h"
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

Log_SetChannel(ReShadeFXShader);

static constexpr s32 DEFAULT_BUFFER_WIDTH = 3840;
static constexpr s32 DEFAULT_BUFFER_HEIGHT = 2160;

static RenderAPI GetRenderAPI()
{
  return g_gpu_device ? g_gpu_device->GetRenderAPI() : RenderAPI::D3D11;
}

static bool PreprocessorFileExistsCallback(const std::string& path)
{
  if (Path::IsAbsolute(path))
    return FileSystem::FileExists(path.c_str());

  return Host::ResourceFileExists(path.c_str(), true);
}

static bool PreprocessorReadFileCallback(const std::string& path, std::string& data)
{
  std::optional<std::string> rdata;
  if (Path::IsAbsolute(path))
    rdata = FileSystem::ReadFileToString(path.c_str());
  else
    rdata = Host::ReadResourceFileToString(path.c_str(), true);
  if (!rdata.has_value())
    return false;

  data = std::move(rdata.value());
  return true;
}

static std::unique_ptr<reshadefx::codegen> CreateRFXCodegen()
{
  const bool debug_info = g_gpu_device ? g_gpu_device->IsDebugDevice() : false;
  const bool uniforms_to_spec_constants = false;
  const RenderAPI rapi = GetRenderAPI();

  switch (rapi)
  {
    case RenderAPI::None:
    case RenderAPI::D3D11:
    case RenderAPI::D3D12:
    {
      return std::unique_ptr<reshadefx::codegen>(
        reshadefx::create_codegen_hlsl(50, debug_info, uniforms_to_spec_constants));
    }

    case RenderAPI::Vulkan:
    case RenderAPI::Metal:
    {
      return std::unique_ptr<reshadefx::codegen>(reshadefx::create_codegen_glsl(
        false, true, debug_info, uniforms_to_spec_constants, false, (rapi == RenderAPI::Vulkan)));
    }

    case RenderAPI::OpenGL:
    case RenderAPI::OpenGLES:
    default:
    {
      return std::unique_ptr<reshadefx::codegen>(reshadefx::create_codegen_glsl(
        (rapi == RenderAPI::OpenGLES), false, debug_info, uniforms_to_spec_constants, false, true));
    }
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

static GPUSampler::Config MapSampler(const reshadefx::sampler_info& si)
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

static GPUPipeline::BlendState MapBlendState(const reshadefx::pass_info& pi)
{
  static constexpr auto map_blend_op = [](const reshadefx::pass_blend_op o) {
    switch (o)
    {
      case reshadefx::pass_blend_op::add:
        return GPUPipeline::BlendOp::Add;
      case reshadefx::pass_blend_op::subtract:
        return GPUPipeline::BlendOp::Subtract;
      case reshadefx::pass_blend_op::reverse_subtract:
        return GPUPipeline::BlendOp::ReverseSubtract;
      case reshadefx::pass_blend_op::min:
        return GPUPipeline::BlendOp::Min;
      case reshadefx::pass_blend_op::max:
      default:
        return GPUPipeline::BlendOp::Max;
    }
  };
  static constexpr auto map_blend_factor = [](const reshadefx::pass_blend_factor f) {
    switch (f)
    {
      case reshadefx::pass_blend_factor::zero:
        return GPUPipeline::BlendFunc::Zero;
      case reshadefx::pass_blend_factor::one:
        return GPUPipeline::BlendFunc::One;
      case reshadefx::pass_blend_factor::source_color:
        return GPUPipeline::BlendFunc::SrcColor;
      case reshadefx::pass_blend_factor::one_minus_source_color:
        return GPUPipeline::BlendFunc::InvSrcColor;
      case reshadefx::pass_blend_factor::dest_color:
        return GPUPipeline::BlendFunc::DstColor;
      case reshadefx::pass_blend_factor::one_minus_dest_color:
        return GPUPipeline::BlendFunc::InvDstColor;
      case reshadefx::pass_blend_factor::source_alpha:
        return GPUPipeline::BlendFunc::SrcAlpha;
      case reshadefx::pass_blend_factor::one_minus_source_alpha:
        return GPUPipeline::BlendFunc::InvSrcAlpha;
      case reshadefx::pass_blend_factor::dest_alpha:
      default:
        return GPUPipeline::BlendFunc::DstAlpha;
    }
  };

  GPUPipeline::BlendState bs = GPUPipeline::BlendState::GetNoBlendingState();
  bs.enable = (pi.blend_enable[0] != 0);
  bs.blend_op = map_blend_op(pi.blend_op[0]);
  bs.src_blend = map_blend_factor(pi.src_blend[0]);
  bs.dst_blend = map_blend_factor(pi.dest_blend[0]);
  bs.alpha_blend_op = map_blend_op(pi.blend_op_alpha[0]);
  bs.src_alpha_blend = map_blend_factor(pi.src_blend_alpha[0]);
  bs.dst_alpha_blend = map_blend_factor(pi.dest_blend_alpha[0]);
  bs.write_mask = pi.color_write_mask[0];
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
    Log_ErrorFmt("Failed to read '{}'.", filename);
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

  reshadefx::module temp_module;
  if (!CreateModule(only_config ? DEFAULT_BUFFER_WIDTH : g_gpu_device->GetWindowWidth(),
                    only_config ? DEFAULT_BUFFER_HEIGHT : g_gpu_device->GetWindowHeight(), &temp_module,
                    std::move(code), error))
  {
    return false;
  }

  if (!CreateOptions(temp_module, error))
    return false;

  // check limits
  if (!temp_module.techniques.empty())
  {
    bool has_passes = false;
    for (const reshadefx::technique_info& tech : temp_module.techniques)
    {
      for (const reshadefx::pass_info& pi : tech.passes)
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
          Error::SetString(error, fmt::format("Too many render targets ({}) in pass {}, only {} are supported.", max_rt,
                                              pi.name, GPUDevice::MAX_RENDER_TARGETS));
          return false;
        }

        if (pi.samplers.size() > GPUDevice::MAX_TEXTURE_SAMPLERS)
        {
          Error::SetString(error, fmt::format("Too many samplers ({}) in pass {}, only {} are supported.",
                                              pi.samplers.size(), pi.name, GPUDevice::MAX_TEXTURE_SAMPLERS));
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

  // Might go invalid when creating pipelines.
  m_valid = true;
  return true;
}

bool PostProcessing::ReShadeFXShader::IsValid() const
{
  return m_valid;
}

bool PostProcessing::ReShadeFXShader::CreateModule(s32 buffer_width, s32 buffer_height, reshadefx::module* mod,
                                                   std::string code, Error* error)
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
  pp.add_macro_definition("BUFFER_WIDTH", std::to_string(buffer_width)); // TODO: can we make these uniforms?
  pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(buffer_height));
  pp.add_macro_definition("BUFFER_RCP_WIDTH", std::to_string(1.0f / static_cast<float>(buffer_width)));
  pp.add_macro_definition("BUFFER_RCP_HEIGHT", std::to_string(1.0f / static_cast<float>(buffer_height)));
  pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "32");

  switch (GetRenderAPI())
  {
    case RenderAPI::D3D11:
    case RenderAPI::D3D12:
      pp.add_macro_definition("__RENDERER__", "0x0B000");
      break;

    case RenderAPI::OpenGL:
    case RenderAPI::OpenGLES:
    case RenderAPI::Vulkan:
    case RenderAPI::Metal:
      pp.add_macro_definition("__RENDERER__", "0x14300");
      break;

    default:
      UnreachableCode();
      break;
  }

  if (!pp.append_string(std::move(code), m_filename))
  {
    Error::SetString(error, fmt::format("Failed to preprocess:\n{}", pp.errors()));
    return false;
  }

  std::unique_ptr<reshadefx::codegen> cg = CreateRFXCodegen();
  if (!cg)
    return false;

  reshadefx::parser parser;
  if (!parser.parse(pp.output(), cg.get()))
  {
    Error::SetString(error, fmt::format("Failed to parse:\n{}", parser.errors()));
    return false;
  }

  cg->write_result(*mod);

  // FileSystem::WriteBinaryFile("D:\\out.txt", mod->code.data(), mod->code.size());
  return true;
}

static bool HasAnnotationWithName(const reshadefx::uniform_info& uniform, const std::string_view& annotation_name)
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
GetVectorAnnotationValue(const reshadefx::uniform_info& uniform, const std::string_view annotation_name,
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
        Log_ErrorFmt("Unhandled string value for '{}' (annotation type: {}, uniform type {})", uniform.name,
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

bool PostProcessing::ReShadeFXShader::CreateOptions(const reshadefx::module& mod, Error* error)
{
  for (const reshadefx::uniform_info& ui : mod.uniforms)
  {
    SourceOptionType so;
    if (!GetSourceOption(ui, &so, error))
      return false;
    if (so != SourceOptionType::None)
    {
      Log_DevFmt("Add source based option {} at offset {} ({})", static_cast<u32>(so), ui.offset, ui.name);

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
        Error::SetString(error, fmt::format("Unhandled uniform type {} ({})", static_cast<u32>(ui.type.base), ui.name));
        return false;
    }

    opt.buffer_offset = ui.offset;
    opt.buffer_size = ui.size;
    opt.vector_size = ui.type.components();
    if (opt.vector_size == 0 || opt.vector_size > ShaderOption::MAX_VECTOR_COMPONENTS)
    {
      Error::SetString(error,
                       fmt::format("Unhandled vector size {} ({})", static_cast<u32>(ui.type.components()), ui.name));
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
      Log_WarningFmt("Uniform '{}' has UI type of '{}' but is vector not scalar ({}), ignoring", opt.name, ui_type,
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

    m_options.push_back(std::move(opt));
  }

  // sort based on category
  std::sort(m_options.begin(), m_options.end(),
            [](const ShaderOption& lhs, const ShaderOption& rhs) { return lhs.category < rhs.category; });

  m_uniforms_size = mod.total_uniform_size;
  Log_DevFmt("{}: {} options", m_filename, m_options.size());
  return true;
}

bool PostProcessing::ReShadeFXShader::GetSourceOption(const reshadefx::uniform_info& ui, SourceOptionType* si,
                                                      Error* error)
{
  const std::string_view source = GetStringAnnotationValue(ui.annotations, "source", {});
  if (!source.empty())
  {
    if (source == "timer")
    {
      if (ui.type.base != reshadefx::type::t_float || ui.type.components() > 1)
      {
        Error::SetString(
          error, fmt::format("Unexpected type '{}' for timer source in uniform '{}'", ui.type.description(), ui.name));
        return false;
      }

      *si = SourceOptionType::Timer;
      return true;
    }
    else if (source == "framecount")
    {
      if ((!ui.type.is_integral() && !ui.type.is_floating_point()) || ui.type.components() > 1)
      {
        Error::SetString(
          error, fmt::format("Unexpected type '{}' for timer source in uniform '{}'", ui.type.description(), ui.name));
        return false;
      }

      *si = (ui.type.base == reshadefx::type::t_float) ? SourceOptionType::FrameCountF : SourceOptionType::FrameCount;
      return true;
    }
    else if (source == "frametime")
    {
      if (ui.type.base != reshadefx::type::t_float || ui.type.components() > 1)
      {
        Error::SetString(
          error, fmt::format("Unexpected type '{}' for timer source in uniform '{}'", ui.type.description(), ui.name));
        return false;
      }

      *si = SourceOptionType::FrameTime;
      return true;
    }
    else if (source == "pingpong")
    {
      if (!ui.type.is_floating_point() || ui.type.components() < 2)
      {
        Error::SetString(error, fmt::format("Unexpected type '{}' for pingpong source in uniform '{}'",
                                            ui.type.description(), ui.name));
        return false;
      }

      *si = SourceOptionType::PingPong;
      return true;
    }
    else if (source == "mousepoint")
    {
      if (!ui.type.is_floating_point() || ui.type.components() < 2)
      {
        Error::SetString(error, fmt::format("Unexpected type '{}' for mousepoint source in uniform '{}'",
                                            ui.type.description(), ui.name));
        return false;
      }

      *si = SourceOptionType::MousePoint;
      return true;
    }
    else if (source == "mousebutton")
    {
      Log_WarningFmt("Ignoring mousebutton source in uniform '{}', not supported.", ui.name);
      *si = SourceOptionType::Zero;
      return true;
    }
    else if (source == "random")
    {
      if ((!ui.type.is_floating_point() && !ui.type.is_integral()) || ui.type.components() != 1)
      {
        Error::SetString(error, fmt::format("Unexpected type '{}' ({} components) for random source in uniform '{}'",
                                            ui.type.description(), ui.type.components(), ui.name));
        return false;
      }

      // TODO: This is missing min/max handling.
      *si = (ui.type.base == reshadefx::type::t_float) ? SourceOptionType::RandomF : SourceOptionType::Random;
      return true;
    }
    else if (source == "overlay_active" || source == "has_depth")
    {
      *si = SourceOptionType::Zero;
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
    else
    {
      Error::SetString(error, fmt::format("Unknown source '{}' in uniform '{}'", source, ui.name));
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

bool PostProcessing::ReShadeFXShader::CreatePasses(GPUTexture::Format backbuffer_format, reshadefx::module& mod,
                                                   Error* error)
{
  u32 total_passes = 0;
  for (const reshadefx::technique_info& tech : mod.techniques)
    total_passes += static_cast<u32>(tech.passes.size());
  if (total_passes == 0)
  {
    Error::SetString(error, "No passes defined.");
    return false;
  }

  m_passes.reserve(total_passes);

  // Named render targets.
  for (const reshadefx::texture_info& ti : mod.textures)
  {
    Texture tex;

    if (!ti.semantic.empty())
    {
      Log_DevFmt("Ignoring semantic {} texture {}", ti.semantic, ti.unique_name);
      continue;
    }
    if (ti.render_target)
    {
      tex.rt_scale = 1.0f;
      tex.format = MapTextureFormat(ti.format);
      Log_DevFmt("Creating render target '{}' {}", ti.unique_name, GPUTexture::GetFormatName(tex.format));
    }
    else
    {
      const std::string_view source = GetStringAnnotationValue(ti.annotations, "source", {});
      if (source.empty())
      {
        Error::SetString(error, fmt::format("Non-render target texture '{}' is missing source.", ti.unique_name));
        return false;
      }

      RGBA8Image image;
      if (const std::string image_path =
            Path::Combine(EmuFolders::Shaders, Path::Combine("reshade" FS_OSPATH_SEPARATOR_STR "Textures", source));
          !image.LoadFromFile(image_path.c_str()))
      {
        // Might be a base file/resource instead.
        const std::string resource_name = Path::Combine("shaders/reshade/Textures", source);
        if (std::optional<std::vector<u8>> resdata = Host::ReadResourceFile(resource_name.c_str(), true);
            !resdata.has_value() || !image.LoadFromBuffer(resource_name.c_str(), resdata->data(), resdata->size()))
        {
          Error::SetString(error, fmt::format("Failed to load image '{}' (from '{}')", source, image_path).c_str());
          return false;
        }
      }

      tex.rt_scale = 0.0f;
      tex.texture = g_gpu_device->FetchTexture(image.GetWidth(), image.GetHeight(), 1, 1, 1, GPUTexture::Type::Texture,
                                               GPUTexture::Format::RGBA8, image.GetPixels(), image.GetPitch());
      if (!tex.texture)
      {
        Error::SetString(
          error, fmt::format("Failed to create {}x{} texture ({})", image.GetWidth(), image.GetHeight(), source));
        return false;
      }

      Log_DevFmt("Loaded {}x{} texture ({})", image.GetWidth(), image.GetHeight(), source);
    }

    tex.reshade_name = ti.unique_name;
    m_textures.push_back(std::move(tex));
  }

  for (reshadefx::technique_info& tech : mod.techniques)
  {
    for (reshadefx::pass_info& pi : tech.passes)
    {
      const bool is_final = (&tech == &mod.techniques.back() && &pi == &tech.passes.back());

      Pass pass;
      pass.num_vertices = pi.num_vertices;

      if (is_final)
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
            Error::SetString(error,
                             fmt::format("Unknown texture '{}' used as render target in pass '{}'", rtname, pi.name));
            return false;
          }

          pass.render_targets.push_back(rt);
        }
      }
      else
      {
        Texture new_rt;
        new_rt.rt_scale = 1.0f;
        new_rt.format = backbuffer_format;
        pass.render_targets.push_back(static_cast<TextureID>(m_textures.size()));
        m_textures.push_back(std::move(new_rt));
      }

      u32 texture_slot = 0;
      for (const reshadefx::sampler_info& si : pi.samplers)
      {
        Sampler sampler;
        sampler.slot = texture_slot++;
        sampler.reshade_name = si.unique_name;

        sampler.texture_id = static_cast<TextureID>(m_textures.size());
        for (const reshadefx::texture_info& ti : mod.textures)
        {
          if (ti.unique_name == si.texture_name)
          {
            // found the texture, now look for our side of it
            if (ti.semantic == "COLOR")
            {
              sampler.texture_id = INPUT_COLOR_TEXTURE;
              break;
            }
            else if (ti.semantic == "DEPTH")
            {
              Log_WarningFmt("Shader '{}' uses input depth as '{}' which is not supported.", m_name, si.texture_name);
              sampler.texture_id = INPUT_DEPTH_TEXTURE;
              break;
            }
            else if (!ti.semantic.empty())
            {
              Error::SetString(error, fmt::format("Unknown semantic {} in texture {}", ti.semantic, ti.name));
              return false;
            }

            // must be a render target, or another texture
            for (u32 i = 0; i < static_cast<u32>(m_textures.size()); i++)
            {
              if (m_textures[i].reshade_name == si.texture_name)
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
          Error::SetString(
            error, fmt::format("Unknown texture {} (sampler {}) in pass {}", si.texture_name, si.name, pi.name));
          return false;
        }

        Log_DevFmt("Pass {} Texture {} => {}", pi.name, si.texture_name, sampler.texture_id);

        sampler.sampler = GetSampler(MapSampler(si));
        if (!sampler.sampler)
        {
          Error::SetString(error, "Failed to create sampler.");
          return false;
        }

        pass.samplers.push_back(std::move(sampler));
      }

#ifdef _DEBUG
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

GPUTexture* PostProcessing::ReShadeFXShader::GetTextureByID(TextureID id, GPUTexture* input,
                                                            GPUTexture* final_target) const
{
  if (id < 0)
  {
    if (id == INPUT_COLOR_TEXTURE)
    {
      return input;
    }
    else if (id == INPUT_DEPTH_TEXTURE)
    {
      return PostProcessing::GetDummyTexture();
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

bool PostProcessing::ReShadeFXShader::CompilePipeline(GPUTexture::Format format, u32 width, u32 height,
                                                      ProgressCallback* progress)
{
  const RenderAPI api = g_gpu_device->GetRenderAPI();
  const bool needs_main_defn = (api != RenderAPI::D3D11 && api != RenderAPI::D3D12);

  m_valid = false;
  m_textures.clear();
  m_passes.clear();

  std::string fxcode;
  if (!PreprocessorReadFileCallback(m_filename, fxcode))
  {
    Log_ErrorFmt("Failed to re-read shader for pipeline: '{}'", m_filename);
    return false;
  }

  // Reshade's preprocessor expects this.
  if (fxcode.empty() || fxcode.back() != '\n')
    fxcode.push_back('\n');

  Error error;
  reshadefx::module mod;
  if (!CreateModule(width, height, &mod, std::move(fxcode), &error))
  {
    Log_ErrorPrintf("Failed to create module for '%s': %s", m_name.c_str(), error.GetDescription().c_str());
    return false;
  }

  DebugAssert(!mod.techniques.empty());

  if (!CreatePasses(format, mod, &error))
  {
    Log_ErrorPrintf("Failed to create passes for '%s': %s", m_name.c_str(), error.GetDescription().c_str());
    return false;
  }

  const std::string_view code(mod.code.data(), mod.code.size());

  auto get_shader = [api, needs_main_defn, &code](const std::string& name, const std::span<Sampler> samplers,
                                                  GPUShaderStage stage) {
    std::string real_code;
    if (needs_main_defn)
    {
      // dFdx/dFdy are not defined in the vertex shader.
      const char* defns = (stage == GPUShaderStage::Vertex) ? "#define dFdx(x) x\n#define dFdy(x) x\n" : "";
      const char* precision = (api == RenderAPI::OpenGLES) ?
                                "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\n" :
                                "";
      real_code = fmt::format("#version {}\n#define ENTRY_POINT_{}\n{}\n{}\n{}",
                              (api == RenderAPI::OpenGLES) ? "320 es" : "460 core", name, defns, precision, code);

      for (const Sampler& sampler : samplers)
      {
        std::string decl = fmt::format("binding = /*SAMPLER:{}*/0", sampler.reshade_name);
        std::string replacement = fmt::format("binding = {}", sampler.slot);
        StringUtil::ReplaceAll(&real_code, decl, replacement);
      }
    }
    else
    {
      real_code = std::string(code);

      for (const Sampler& sampler : samplers)
      {
        std::string decl = fmt::format("__{}_t : register( t0);", sampler.reshade_name);
        std::string replacement =
          fmt::format("__{}_t : register({}t{});", sampler.reshade_name, (sampler.slot < 10) ? " " : "", sampler.slot);
        StringUtil::ReplaceAll(&real_code, decl, replacement);

        decl = fmt::format("__{}_s : register( s0);", sampler.reshade_name);
        replacement =
          fmt::format("__{}_s : register({}s{});", sampler.reshade_name, (sampler.slot < 10) ? " " : "", sampler.slot);
        StringUtil::ReplaceAll(&real_code, decl, replacement);
      }
    }

    // FileSystem::WriteStringToFile("D:\\foo.txt", real_code);

    std::unique_ptr<GPUShader> sshader =
      g_gpu_device->CreateShader(stage, real_code, needs_main_defn ? "main" : name.c_str());
    if (!sshader)
      Log_ErrorPrintf("Failed to compile function '%s'", name.c_str());

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

  progress->PushState();

  size_t total_passes = 0;
  for (const reshadefx::technique_info& tech : mod.techniques)
    total_passes += tech.passes.size();
  progress->SetProgressRange(static_cast<u32>(total_passes));
  progress->SetProgressValue(0);

  u32 passnum = 0;
  for (const reshadefx::technique_info& tech : mod.techniques)
  {
    for (const reshadefx::pass_info& info : tech.passes)
    {
      DebugAssert(passnum < m_passes.size());
      Pass& pass = m_passes[passnum++];

      auto vs = get_shader(info.vs_entry_point, pass.samplers, GPUShaderStage::Vertex);
      auto fs = get_shader(info.ps_entry_point, pass.samplers, GPUShaderStage::Fragment);
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

      pass.pipeline = g_gpu_device->CreatePipeline(plconfig);
      if (!pass.pipeline)
      {
        Log_ErrorPrintf("Failed to create pipeline for pass '%s'", info.name.c_str());
        progress->PopState();
        return false;
      }

      progress->SetProgressValue(passnum);
    }
  }

  progress->PopState();

  m_valid = true;
  return true;
}

bool PostProcessing::ReShadeFXShader::ResizeOutput(GPUTexture::Format format, u32 width, u32 height)
{
  m_valid = false;

  for (Texture& tex : m_textures)
  {
    if (tex.rt_scale == 0.0f)
      continue;

    g_gpu_device->RecycleTexture(std::move(tex.texture));

    const u32 t_width = std::max(static_cast<u32>(static_cast<float>(width) * tex.rt_scale), 1u);
    const u32 t_height = std::max(static_cast<u32>(static_cast<float>(height) * tex.rt_scale), 1u);
    tex.texture = g_gpu_device->FetchTexture(t_width, t_height, 1, 1, 1, GPUTexture::Type::RenderTarget, tex.format);
    if (!tex.texture)
    {
      Log_ErrorPrintf("Failed to create %ux%u texture", t_width, t_height);
      return {};
    }
  }

  m_valid = true;
  return true;
}

bool PostProcessing::ReShadeFXShader::Apply(GPUTexture* input, GPUTexture* final_target, s32 final_left, s32 final_top,
                                            s32 final_width, s32 final_height, s32 orig_width, s32 orig_height,
                                            u32 target_width, u32 target_height)
{
  GL_PUSH_FMT("PostProcessingShaderFX {}", m_name);

  m_frame_count++;

  // Reshade always draws at full size.
  g_gpu_device->SetViewportAndScissor(0, 0, target_width, target_height);

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

        case SourceOptionType::Timer:
        {
          const float value = static_cast<float>(PostProcessing::GetTimer().GetTimeMilliseconds());
          std::memcpy(dst, &value, sizeof(value));
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
          const s32 value =
            (so.source == SourceOptionType::BufferWidth) ? static_cast<s32>(orig_width) : static_cast<s32>(orig_height);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOptionType::InternalWidthF:
        case SourceOptionType::InternalHeightF:
        {
          const float value = (so.source == SourceOptionType::BufferWidthF) ? static_cast<float>(orig_width) :
                                                                              static_cast<float>(orig_height);
          std::memcpy(dst, &value, sizeof(value));
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
      GPUTexture* const tex = GetTextureByID(sampler.texture_id, input, final_target);
      if (tex)
        tex->MakeReadyForSampling();
    }

    if (pass.render_targets.size() == 1 && pass.render_targets[0] == OUTPUT_COLOR_TEXTURE && !final_target)
    {
      // Special case: drawing to final buffer.
      if (!g_gpu_device->BeginPresent(false))
      {
        GL_POP();
        return false;
      }
    }
    else
    {
      std::array<GPUTexture*, GPUDevice::MAX_RENDER_TARGETS> render_targets;
      for (size_t i = 0; i < pass.render_targets.size(); i++)
      {
        GL_INS_FMT("Render Target {}: ID {} [{}]", i, pass.render_targets[i],
                   GetTextureNameForID(pass.render_targets[i]));
        render_targets[i] = GetTextureByID(pass.render_targets[i], input, final_target);
        DebugAssert(render_targets[i]);
      }

      g_gpu_device->SetRenderTargets(render_targets.data(), static_cast<u32>(pass.render_targets.size()), nullptr);
    }

    g_gpu_device->SetPipeline(pass.pipeline.get());

    // Set all inputs first, before the render pass starts.
    std::bitset<GPUDevice::MAX_TEXTURE_SAMPLERS> bound_textures = {};
    for (const Sampler& sampler : pass.samplers)
    {
      GL_INS_FMT("Texture Sampler {}: ID {} [{}]", sampler.slot, sampler.texture_id,
                 GetTextureNameForID(sampler.texture_id));
      g_gpu_device->SetTextureSampler(sampler.slot, GetTextureByID(sampler.texture_id, input, final_target),
                                      sampler.sampler);
      bound_textures[sampler.slot] = true;
    }

    // Ensure RT wasn't left bound as a previous output, it breaks VK/DX12.
    // TODO: Maybe move this into the backend? Not sure...
    for (u32 i = 0; i < GPUDevice::MAX_TEXTURE_SAMPLERS; i++)
    {
      if (!bound_textures[i])
        g_gpu_device->SetTextureSampler(i, nullptr, nullptr);
    }

    g_gpu_device->Draw(pass.num_vertices, 0);
  }

  GL_POP();
  m_frame_timer.Reset();
  return true;
}
