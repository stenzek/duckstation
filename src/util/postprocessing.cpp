// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "postprocessing.h"
#include "gpu_device.h"
#include "host.h"
#include "imgui_manager.h"
#include "postprocessing_shader.h"
#include "postprocessing_shader_fx.h"
#include "postprocessing_shader_glsl.h"
#include "shadergen.h"

// TODO: Remove me
#include "core/host.h"
#include "core/fullscreen_ui.h"
#include "core/settings.h"

#include "IconsFontAwesome5.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/progress_callback.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "fmt/format.h"

LOG_CHANNEL(PostProcessing);

namespace PostProcessing {
template<typename T>
static u32 ParseVector(std::string_view line, ShaderOption::ValueVector* values);

static TinyString ValueToString(ShaderOption::Type type, u32 vector_size, const ShaderOption::ValueVector& value);

static TinyString GetStageConfigSection(const char* section, u32 index);
static void CopyStageConfig(SettingsInterface& si, const char* section, u32 old_index, u32 new_index);
static void SwapStageConfig(SettingsInterface& si, const char* section, u32 lhs_index, u32 rhs_index);
static std::unique_ptr<Shader> TryLoadingShader(const std::string& shader_name, bool only_config, Error* error);
static SettingsInterface& GetLoadSettingsInterface(const char* section);

template<typename T>
ALWAYS_INLINE void ForAllChains(const T& F)
{
  F(DisplayChain);
  F(InternalChain);
}

Chain DisplayChain(Config::DISPLAY_CHAIN_SECTION);
Chain InternalChain(Config::INTERNAL_CHAIN_SECTION);

static Timer s_timer;

static std::unordered_map<u64, std::unique_ptr<GPUSampler>> s_samplers;
static std::unique_ptr<GPUTexture> s_dummy_texture;
} // namespace PostProcessing

template<typename T>
u32 PostProcessing::ParseVector(std::string_view line, ShaderOption::ValueVector* values)
{
  u32 index = 0;
  size_t start = 0;
  while (index < PostProcessing::ShaderOption::MAX_VECTOR_COMPONENTS)
  {
    while (start < line.size() && std::isspace(line[start]))
      start++;

    if (start >= line.size())
      break;

    size_t end = line.find(',', start);
    if (end == std::string_view::npos)
      end = line.size();

    const std::string_view component = line.substr(start, end - start);
    T value = StringUtil::FromChars<T>(component).value_or(static_cast<T>(0));
    if constexpr (std::is_same_v<T, float>)
      (*values)[index++].float_value = value;
    else if constexpr (std::is_same_v<T, s32>)
      (*values)[index++].int_value = value;

    start = end + 1;
  }

  const u32 size = index;

  for (; index < PostProcessing::ShaderOption::MAX_VECTOR_COMPONENTS; index++)
  {
    if constexpr (std::is_same_v<T, float>)
      (*values)[index++].float_value = 0.0f;
    else if constexpr (std::is_same_v<T, s32>)
      (*values)[index++].int_value = 0;
  }

  return size;
}

u32 PostProcessing::ShaderOption::ParseFloatVector(std::string_view line, ValueVector* values)
{
  return ParseVector<float>(line, values);
}

u32 PostProcessing::ShaderOption::ParseIntVector(std::string_view line, ValueVector* values)
{
  return ParseVector<s32>(line, values);
}

TinyString PostProcessing::ValueToString(ShaderOption::Type type, u32 vector_size,
                                         const ShaderOption::ValueVector& value)
{
  TinyString ret;

  for (u32 i = 0; i < vector_size; i++)
  {
    if (i > 0)
      ret.append(',');

    switch (type)
    {
      case ShaderOption::Type::Bool:
        ret.append((value[i].int_value != 0) ? "true" : "false");
        break;

      case ShaderOption::Type::Int:
        ret.append_format("{}", value[i].int_value);
        break;

      case ShaderOption::Type::Float:
        ret.append_format("{}", value[i].float_value);
        break;

      default:
        break;
    }
  }

  return ret;
}

std::vector<std::pair<std::string, std::string>> PostProcessing::GetAvailableShaderNames()
{
  std::vector<std::pair<std::string, std::string>> names;

  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(Path::Combine(EmuFolders::Resources, "shaders").c_str(), "*.glsl",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_RELATIVE_PATHS, &results);
  FileSystem::FindFiles(EmuFolders::Shaders.c_str(), "*.glsl",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_RELATIVE_PATHS |
                          FILESYSTEM_FIND_KEEP_ARRAY,
                        &results);
  std::sort(results.begin(), results.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.FileName < rhs.FileName; });

  for (FILESYSTEM_FIND_DATA& fd : results)
  {
    size_t pos = fd.FileName.rfind('.');
    if (pos != std::string::npos && pos > 0)
      fd.FileName.erase(pos);

#ifdef _WIN32
    // swap any backslashes for forward slashes so the config is cross-platform
    StringUtil::ReplaceAll(&fd.FileName, '\\', '/');
#endif

    if (std::none_of(names.begin(), names.end(), [&fd](const auto& other) { return fd.FileName == other.second; }))
    {
      std::string display_name = fmt::format(TRANSLATE_FS("PostProcessing", "{} [GLSL]"), fd.FileName);
      names.emplace_back(std::move(display_name), std::move(fd.FileName));
    }
  }

  FileSystem::FindFiles(Path::Combine(EmuFolders::Shaders, "reshade" FS_OSPATH_SEPARATOR_STR "Shaders").c_str(), "*.fx",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_RELATIVE_PATHS, &results);
  FileSystem::FindFiles(
    Path::Combine(EmuFolders::Resources, "shaders" FS_OSPATH_SEPARATOR_STR "reshade" FS_OSPATH_SEPARATOR_STR "Shaders")
      .c_str(),
    "*.fx",
    FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_KEEP_ARRAY,
    &results);
  std::sort(results.begin(), results.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.FileName < rhs.FileName; });

  for (FILESYSTEM_FIND_DATA& fd : results)
  {
    size_t pos = fd.FileName.rfind('.');
    if (pos != std::string::npos && pos > 0)
      fd.FileName.erase(pos);

#ifdef _WIN32
    // swap any backslashes for forward slashes so the config is cross-platform
    StringUtil::ReplaceAll(&fd.FileName, '\\', '/');
#endif

    if (std::none_of(names.begin(), names.end(), [&fd](const auto& other) { return fd.FileName == other.second; }))
    {
      std::string display_name = fmt::format(TRANSLATE_FS("PostProcessing", "{} [ReShade]"), fd.FileName);
      names.emplace_back(std::move(display_name), std::move(fd.FileName));
    }
  }

  std::sort(names.begin(), names.end(),
            [](const std::pair<std::string, std::string>& lhs, const std::pair<std::string, std::string>& rhs) {
              return (StringUtil::Strcasecmp(lhs.first.c_str(), rhs.first.c_str()) < 0);
            });

  return names;
}

TinyString PostProcessing::GetStageConfigSection(const char* section, u32 index)
{
  return TinyString::from_format("{}/Stage{}", section, index + 1);
}

void PostProcessing::CopyStageConfig(SettingsInterface& si, const char* section, u32 old_index, u32 new_index)
{
  const TinyString old_section = GetStageConfigSection(section, old_index);
  const TinyString new_section = GetStageConfigSection(section, new_index);

  si.ClearSection(new_section);

  for (const auto& [key, value] : si.GetKeyValueList(old_section))
    si.SetStringValue(new_section, key.c_str(), value.c_str());
}

void PostProcessing::SwapStageConfig(SettingsInterface& si, const char* section, u32 lhs_index, u32 rhs_index)
{
  const TinyString lhs_section = GetStageConfigSection(section, lhs_index);
  const TinyString rhs_section = GetStageConfigSection(section, rhs_index);

  const std::vector<std::pair<std::string, std::string>> lhs_kvs = si.GetKeyValueList(lhs_section);
  si.ClearSection(lhs_section);

  const std::vector<std::pair<std::string, std::string>> rhs_kvs = si.GetKeyValueList(rhs_section);
  si.ClearSection(rhs_section);

  for (const auto& [key, value] : rhs_kvs)
    si.SetStringValue(lhs_section, key.c_str(), value.c_str());

  for (const auto& [key, value] : lhs_kvs)
    si.SetStringValue(rhs_section, key.c_str(), value.c_str());
}

u32 PostProcessing::Config::GetStageCount(const SettingsInterface& si, const char* section)
{
  return si.GetUIntValue(section, "StageCount", 0u);
}

std::string PostProcessing::Config::GetStageShaderName(const SettingsInterface& si, const char* section, u32 index)
{
  return si.GetStringValue(GetStageConfigSection(section, index), "ShaderName");
}

std::vector<PostProcessing::ShaderOption> PostProcessing::Config::GetStageOptions(const SettingsInterface& si,
                                                                                  const char* section, u32 index)
{
  std::vector<PostProcessing::ShaderOption> ret;

  const TinyString stage_section = GetStageConfigSection(section, index);
  const std::string shader_name = si.GetStringValue(stage_section, "ShaderName");
  if (shader_name.empty())
    return ret;

  std::unique_ptr<Shader> shader = TryLoadingShader(shader_name, true, nullptr);
  if (!shader)
    return ret;

  shader->LoadOptions(si, stage_section);
  ret = shader->TakeOptions();
  return ret;
}

std::vector<PostProcessing::ShaderOption> PostProcessing::Config::GetShaderOptions(const std::string& shader_name,
                                                                                   Error* error)
{
  std::vector<PostProcessing::ShaderOption> ret;
  std::unique_ptr<Shader> shader = TryLoadingShader(shader_name, true, error);
  if (!shader)
    return ret;

  ret = shader->TakeOptions();
  return ret;
}

bool PostProcessing::Config::AddStage(SettingsInterface& si, const char* section, const std::string& shader_name,
                                      Error* error)
{
  std::unique_ptr<Shader> shader = TryLoadingShader(shader_name, true, error);
  if (!shader)
    return false;

  const u32 index = GetStageCount(si, section);
  si.SetUIntValue(section, "StageCount", index + 1);

  const TinyString stage_section = GetStageConfigSection(section, index);
  si.SetStringValue(stage_section, "ShaderName", shader->GetName().c_str());

#if 0
  // Leave options unset for now.
  for (const ShaderOption& option : shader->GetOptions())
  {
    si.SetStringValue(section, option.name.c_str(),
                      ValueToString(option.type, option.vector_size, option.default_value));
  }
#endif

  return true;
}

void PostProcessing::Config::RemoveStage(SettingsInterface& si, const char* section, u32 index)
{
  const u32 stage_count = GetStageCount(si, section);
  if (index >= stage_count)
    return;

  for (u32 i = index; i < (stage_count - 1); i++)
    CopyStageConfig(si, section, i + 1, i);

  si.ClearSection(GetStageConfigSection(section, stage_count - 1));
  si.SetUIntValue(section, "StageCount", stage_count - 1);
}

void PostProcessing::Config::MoveStageUp(SettingsInterface& si, const char* section, u32 index)
{
  const u32 stage_count = GetStageCount(si, section);
  if (index == 0 || index >= stage_count)
    return;

  SwapStageConfig(si, section, index, index - 1);
}

void PostProcessing::Config::MoveStageDown(SettingsInterface& si, const char* section, u32 index)
{
  const u32 stage_count = GetStageCount(si, section);
  if ((index + 1) >= stage_count)
    return;

  SwapStageConfig(si, section, index, index + 1);
}

void PostProcessing::Config::SetStageOption(SettingsInterface& si, const char* section, u32 index,
                                            const ShaderOption& option)
{
  const TinyString stage_section = GetStageConfigSection(section, index);
  si.SetStringValue(stage_section, option.name.c_str(), ValueToString(option.type, option.vector_size, option.value));
}

void PostProcessing::Config::UnsetStageOption(SettingsInterface& si, const char* section, u32 index,
                                              const ShaderOption& option)
{
  const TinyString stage_section = GetStageConfigSection(section, index);
  si.DeleteValue(stage_section, option.name.c_str());
}

void PostProcessing::Config::ClearStages(SettingsInterface& si, const char* section)
{
  const u32 count = GetStageCount(si, section);
  for (s32 i = static_cast<s32>(count - 1); i >= 0; i--)
    si.ClearSection(GetStageConfigSection(section, static_cast<u32>(i)));
  si.SetUIntValue(section, "StageCount", 0);
}

PostProcessing::Chain::Chain(const char* section) : m_section(section)
{
}

PostProcessing::Chain::~Chain() = default;

GPUTexture* PostProcessing::Chain::GetTextureUnusedAtEndOfChain() const
{
  return (m_stages.size() % 2) ? m_output_texture.get() : m_input_texture.get();
}

bool PostProcessing::Chain::IsActive() const
{
  return m_enabled && !m_stages.empty();
}

bool PostProcessing::Chain::IsInternalChain() const
{
  return (this == &InternalChain);
}

void PostProcessing::Chain::ClearStagesWithError(const Error& error)
{
  std::string msg = error.GetDescription();
  Host::AddIconOSDMessage(
    "PostProcessLoadFail", ICON_FA_EXCLAMATION_TRIANGLE,
    fmt::format(TRANSLATE_FS("OSDMessage", "Failed to load post-processing chain: {}"),
                msg.empty() ? TRANSLATE_SV("PostProcessing", "Unknown Error") : std::string_view(msg)),
    Host::OSD_ERROR_DURATION);
  m_stages.clear();
}

void PostProcessing::Chain::LoadStages()
{
  auto lock = Host::GetSettingsLock();
  SettingsInterface& si = GetLoadSettingsInterface(m_section);

  m_enabled = si.GetBoolValue(m_section, "Enabled", false);
  m_wants_depth_buffer = false;

  const u32 stage_count = Config::GetStageCount(si, m_section);
  if (stage_count == 0)
    return;

  Error error;
  LoadingScreenProgressCallback progress;
  progress.SetProgressRange(stage_count);

  for (u32 i = 0; i < stage_count; i++)
  {
    std::string stage_name = Config::GetStageShaderName(si, m_section, i);
    if (stage_name.empty())
    {
      error.SetString(fmt::format("No stage name in stage {}.", i + 1));
      ClearStagesWithError(error);
      return;
    }

    lock.unlock();
    progress.FormatStatusText("Loading shader {}...", stage_name);

    std::unique_ptr<Shader> shader = TryLoadingShader(stage_name, false, &error);
    if (!shader)
    {
      ClearStagesWithError(error);
      return;
    }

    lock.lock();
    shader->LoadOptions(si, GetStageConfigSection(m_section, i));
    m_stages.push_back(std::move(shader));

    progress.IncrementProgressValue();
  }

  if (stage_count > 0)
    DEV_LOG("Loaded {} post-processing stages.", stage_count);

  // precompile shaders
  if (!IsInternalChain() && g_gpu_device && g_gpu_device->HasMainSwapChain())
  {
    CheckTargets(g_gpu_device->GetMainSwapChain()->GetFormat(), g_gpu_device->GetMainSwapChain()->GetWidth(),
                 g_gpu_device->GetMainSwapChain()->GetHeight(), &progress);
  }

  // must be down here, because we need to compile first, triggered by CheckTargets()
  for (std::unique_ptr<Shader>& shader : m_stages)
    m_wants_depth_buffer |= shader->WantsDepthBuffer();
  m_needs_depth_buffer = m_enabled && m_wants_depth_buffer;
  if (m_wants_depth_buffer)
    DEV_LOG("Depth buffer is needed.");
}

void PostProcessing::Chain::ClearStages()
{
  decltype(m_stages)().swap(m_stages);
}

void PostProcessing::Chain::UpdateSettings(std::unique_lock<std::mutex>& settings_lock)
{
  SettingsInterface& si = GetLoadSettingsInterface(m_section);

  m_enabled = si.GetBoolValue(m_section, "Enabled", false);

  const u32 stage_count = Config::GetStageCount(si, m_section);
  if (stage_count == 0)
  {
    m_stages.clear();
    return;
  }

  Error error;

  m_stages.resize(stage_count);

  LoadingScreenProgressCallback progress;
  progress.SetProgressRange(stage_count);

  const GPUTexture::Format prev_format = m_target_format;
  m_wants_depth_buffer = false;

  for (u32 i = 0; i < stage_count; i++)
  {
    std::string stage_name = Config::GetStageShaderName(si, m_section, i);
    if (stage_name.empty())
    {
      error.SetString(fmt::format("No stage name in stage {}.", i + 1));
      ClearStagesWithError(error);
      return;
    }

    if (!m_stages[i] || stage_name != m_stages[i]->GetName())
    {
      if (i < m_stages.size())
        m_stages[i].reset();

      // Force recompile.
      m_target_format = GPUTexture::Format::Unknown;

      settings_lock.unlock();

      std::unique_ptr<Shader> shader = TryLoadingShader(stage_name, false, &error);
      if (!shader)
      {
        ClearStagesWithError(error);
        return;
      }

      if (i < m_stages.size())
        m_stages[i] = std::move(shader);
      else
        m_stages.push_back(std::move(shader));

      settings_lock.lock();
    }

    m_stages[i]->LoadOptions(si, GetStageConfigSection(m_section, i));
  }

  if (prev_format != GPUTexture::Format::Unknown)
    CheckTargets(prev_format, m_target_width, m_target_height, &progress);

  if (stage_count > 0)
  {
    s_timer.Reset();
    DEV_LOG("Loaded {} post-processing stages.", stage_count);
  }

  // must be down here, because we need to compile first, triggered by CheckTargets()
  for (std::unique_ptr<Shader>& shader : m_stages)
    m_wants_depth_buffer |= shader->WantsDepthBuffer();
  m_needs_depth_buffer = m_enabled && m_wants_depth_buffer;
  if (m_wants_depth_buffer)
    DEV_LOG("Depth buffer is needed.");
}

void PostProcessing::Chain::Toggle()
{
  if (m_stages.empty())
  {
    Host::AddIconOSDMessage("PostProcessing", ICON_FA_PAINT_ROLLER,
                            TRANSLATE_STR("OSDMessage", "No post-processing shaders are selected."),
                            Host::OSD_QUICK_DURATION);
    return;
  }

  const bool new_enabled = !m_enabled;
  Host::AddIconOSDMessage("PostProcessing", ICON_FA_PAINT_ROLLER,
                          new_enabled ? TRANSLATE_STR("OSDMessage", "Post-processing is now enabled.") :
                                        TRANSLATE_STR("OSDMessage", "Post-processing is now disabled."),
                          Host::OSD_QUICK_DURATION);
  m_enabled = new_enabled;
  m_needs_depth_buffer = new_enabled && m_wants_depth_buffer;
  if (m_enabled)
    s_timer.Reset();
}

bool PostProcessing::Chain::CheckTargets(GPUTexture::Format target_format, u32 target_width, u32 target_height,
                                         ProgressCallback* progress /* = nullptr */)
{
  if (m_target_format == target_format && m_target_width == target_width && m_target_height == target_height)
    return true;

  Error error;

  if (!IsInternalChain() && (!m_rotated_copy_pipeline || m_target_format != target_format))
  {
    const RenderAPI rapi = g_gpu_device->GetRenderAPI();
    const ShaderGen shadergen(rapi, ShaderGen::GetShaderLanguageForAPI(rapi), false, false);
    const std::unique_ptr<GPUShader> vso = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                                      shadergen.GenerateRotateVertexShader(), &error);
    const std::unique_ptr<GPUShader> fso = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                                                      shadergen.GenerateRotateFragmentShader(), &error);
    if (!vso || !fso)
    {
      ERROR_LOG("Failed to compile post-processing rotate shaders: {}", error.GetDescription());
      return false;
    }
    GL_OBJECT_NAME(vso, "Post-processing rotate blit VS");
    GL_OBJECT_NAME(vso, "Post-processing rotate blit FS");

    const GPUPipeline::GraphicsConfig config = {.layout = GPUPipeline::Layout::SingleTextureAndPushConstants,
                                                .primitive = GPUPipeline::Primitive::Triangles,
                                                .input_layout = {},
                                                .rasterization = GPUPipeline::RasterizationState::GetNoCullState(),
                                                .depth = GPUPipeline::DepthState::GetNoTestsState(),
                                                .blend = GPUPipeline::BlendState::GetNoBlendingState(),
                                                .vertex_shader = vso.get(),
                                                .geometry_shader = nullptr,
                                                .fragment_shader = fso.get(),
                                                .color_formats = {target_format},
                                                .depth_format = GPUTexture::Format::Unknown,
                                                .samples = 1,
                                                .per_sample_shading = false,
                                                .render_pass_flags = GPUPipeline::NoRenderPassFlags};
    m_rotated_copy_pipeline = g_gpu_device->CreatePipeline(config, &error);
    if (!m_rotated_copy_pipeline)
    {
      ERROR_LOG("Failed to compile post-processing rotate pipeline: {}", error.GetDescription());
      return false;
    }
    GL_OBJECT_NAME(m_rotated_copy_pipeline, "Post-processing rotate pipeline");
  }

  // In case any allocs fail.
  DestroyTextures();

  if (!(m_input_texture =
          g_gpu_device->FetchTexture(target_width, target_height, 1, 1, 1, GPUTexture::Type::RenderTarget,
                                     target_format, GPUTexture::Flags::None, nullptr, 0, &error)) ||
      !(m_output_texture =
          g_gpu_device->FetchTexture(target_width, target_height, 1, 1, 1, GPUTexture::Type::RenderTarget,
                                     target_format, GPUTexture::Flags::None, nullptr, 0, &error)))
  {
    ERROR_LOG("Failed to create input/output textures: {}", error.GetDescription());
    DestroyTextures();
    return false;
  }

  if (!progress)
    progress = ProgressCallback::NullProgressCallback;

  progress->SetProgressRange(static_cast<u32>(m_stages.size()));
  progress->SetProgressValue(0);

  m_wants_depth_buffer = false;

  for (size_t i = 0; i < m_stages.size(); i++)
  {
    Shader* const shader = m_stages[i].get();

    progress->FormatStatusText("Compiling {}...", shader->GetName());

    if (!shader->CompilePipeline(target_format, target_width, target_height, &error, progress) ||
        !shader->ResizeOutput(target_format, target_width, target_height, &error))
    {
      ERROR_LOG("Failed to compile post-processing shader '{}':\n{}", shader->GetName(), error.GetDescription());
      Host::AddIconOSDMessage(
        "PostProcessLoadFail", ICON_FA_EXCLAMATION_TRIANGLE,
        fmt::format("Failed to compile post-processing shader '{}'. Disabling post-processing.", shader->GetName()));
      m_enabled = false;
      return false;
    }

    progress->SetProgressValue(static_cast<u32>(i + 1));
    m_wants_depth_buffer |= shader->WantsDepthBuffer();
  }

  m_target_format = target_format;
  m_target_width = target_width;
  m_target_height = target_height;
  m_needs_depth_buffer = m_enabled && m_wants_depth_buffer;
  return true;
}

void PostProcessing::Chain::DestroyTextures()
{
  m_target_format = GPUTexture::Format::Unknown;
  m_target_width = 0;
  m_target_height = 0;

  g_gpu_device->RecycleTexture(std::move(m_output_texture));
  g_gpu_device->RecycleTexture(std::move(m_input_texture));
}

void PostProcessing::Chain::DestroyPipelines()
{
  m_rotated_copy_pipeline.reset();
}

GPUDevice::PresentResult PostProcessing::Chain::Apply(GPUTexture* input_color, GPUTexture* input_depth,
                                                      GPUTexture* final_target, GSVector4i final_rect, s32 orig_width,
                                                      s32 orig_height, s32 native_width, s32 native_height)
{
  GL_SCOPE_FMT("{} Apply", m_section);

  GPUTexture* output = m_output_texture.get();
  input_color->MakeReadyForSampling();
  if (input_depth)
    input_depth->MakeReadyForSampling();

  GPUTexture* draw_final_target = final_target;
  const WindowInfo::PreRotation prerotation =
    final_target ? WindowInfo::PreRotation::Identity : g_gpu_device->GetMainSwapChain()->GetPreRotation();
  if (prerotation != WindowInfo::PreRotation::Identity)
  {
    // We have prerotation and post processing. This is messy, since we need to run the shader on the "real" size,
    // then copy it across to the rotated image. We can use the input or output texture from the chain, whichever
    // was not the last that was drawn to.
    draw_final_target = GetTextureUnusedAtEndOfChain();
  }

  for (const std::unique_ptr<Shader>& stage : m_stages)
  {
    const bool is_final = (stage.get() == m_stages.back().get());

    if (const GPUDevice::PresentResult pres =
          stage->Apply(input_color, input_depth, is_final ? draw_final_target : output, final_rect, orig_width,
                       orig_height, native_width, native_height, m_target_width, m_target_height);
        pres != GPUDevice::PresentResult::OK)
    {
      return pres;
    }

    if (!is_final)
    {
      output->MakeReadyForSampling();
      input_color = output;
      output = (output == m_output_texture.get()) ? m_input_texture.get() : m_output_texture.get();
    }
  }

  if (prerotation != WindowInfo::PreRotation::Identity)
  {
    draw_final_target->MakeReadyForSampling();

    // Rotate and blit to final swap chain.
    GPUSwapChain* const swap_chain = g_gpu_device->GetMainSwapChain();
    if (const GPUDevice::PresentResult pres = g_gpu_device->BeginPresent(swap_chain);
        pres != GPUDevice::PresentResult::OK)
    {
      return pres;
    }

    GL_PUSH_FMT("Apply swap chain pre-rotation");

    const GSMatrix2x2 rotmat = GSMatrix2x2::Rotation(WindowInfo::GetZRotationForPreRotation(prerotation));
    g_gpu_device->SetPipeline(m_rotated_copy_pipeline.get());
    g_gpu_device->PushUniformBuffer(&rotmat, sizeof(rotmat));
    g_gpu_device->SetTextureSampler(0, draw_final_target, g_gpu_device->GetNearestSampler());
    g_gpu_device->SetViewportAndScissor(0, 0, swap_chain->GetPostRotatedWidth(), swap_chain->GetPostRotatedHeight());
    g_gpu_device->Draw(3, 0);

    GL_POP();
  }

  return GPUDevice::PresentResult::OK;
}

void PostProcessing::Initialize()
{
  DisplayChain.LoadStages();
  InternalChain.LoadStages();
  s_timer.Reset();
}

void PostProcessing::UpdateSettings()
{
  auto lock = Host::GetSettingsLock();
  ForAllChains([&lock](Chain& chain) { chain.UpdateSettings(lock); });
}

void PostProcessing::Shutdown()
{
  g_gpu_device->RecycleTexture(std::move(s_dummy_texture));
  s_samplers.clear();
  ForAllChains([](Chain& chain) {
    chain.ClearStages();
    chain.DestroyPipelines();
    chain.DestroyTextures();
  });
}

bool PostProcessing::ReloadShaders()
{
  if (!DisplayChain.HasStages() && !InternalChain.HasStages())
  {
    Host::AddIconOSDMessage("PostProcessing", ICON_FA_PAINT_ROLLER,
                            TRANSLATE_STR("OSDMessage", "No post-processing shaders are selected."),
                            Host::OSD_QUICK_DURATION);
    return false;
  }

  ForAllChains([](Chain& chain) {
    chain.ClearStages();
    chain.DestroyPipelines();
    chain.DestroyTextures();
    chain.LoadStages();
  });
  s_timer.Reset();

  Host::AddIconOSDMessage("PostProcessing", ICON_FA_PAINT_ROLLER,
                          TRANSLATE_STR("OSDMessage", "Post-processing shaders reloaded."), Host::OSD_QUICK_DURATION);
  return true;
}

std::unique_ptr<PostProcessing::Shader> PostProcessing::TryLoadingShader(const std::string& shader_name,
                                                                         bool only_config, Error* error)
{
  std::string filename;
  std::optional<std::string> resource_str;
  Error local_error;
  if (!error)
    error = &local_error;

  // Try reshade first.
  filename = Path::Combine(
    EmuFolders::Shaders,
    fmt::format("reshade" FS_OSPATH_SEPARATOR_STR "Shaders" FS_OSPATH_SEPARATOR_STR "{}.fx", shader_name));
  if (FileSystem::FileExists(filename.c_str()))
  {
    std::unique_ptr<ReShadeFXShader> shader = std::make_unique<ReShadeFXShader>();
    if (!shader->LoadFromFile(shader_name, filename.c_str(), only_config, error))
    {
      ERROR_LOG("Failed to load shader '{}': {}", shader_name, error->GetDescription());
      shader.reset();
    }
    return shader;
  }

  filename = Path::Combine(EmuFolders::Shaders, fmt::format("{}.glsl", shader_name));
  if (FileSystem::FileExists(filename.c_str()))
  {
    std::unique_ptr<GLSLShader> shader = std::make_unique<GLSLShader>();
    if (!shader->LoadFromFile(shader_name, filename.c_str(), error))
    {
      ERROR_LOG("Failed to load shader '{}': {}", shader_name, error->GetDescription());
      shader.reset();
    }

    return shader;
  }

  filename =
    fmt::format("shaders/reshade" FS_OSPATH_SEPARATOR_STR "Shaders" FS_OSPATH_SEPARATOR_STR "{}.fx", shader_name);
  resource_str = Host::ReadResourceFileToString(filename.c_str(), true, error);
  if (resource_str.has_value())
  {
    std::unique_ptr<ReShadeFXShader> shader = std::make_unique<ReShadeFXShader>();
    if (!shader->LoadFromString(shader_name, std::move(filename), std::move(resource_str.value()), only_config, error))
    {
      ERROR_LOG("Failed to load shader '{}': {}", shader_name, error->GetDescription());
      shader.reset();
    }

    return shader;
  }

  filename = fmt::format("shaders" FS_OSPATH_SEPARATOR_STR "{}.glsl", shader_name);
  resource_str = Host::ReadResourceFileToString(filename.c_str(), true, error);
  if (resource_str.has_value())
  {
    std::unique_ptr<GLSLShader> shader = std::make_unique<GLSLShader>();
    if (!shader->LoadFromString(shader_name, std::move(resource_str.value()), error))
    {
      ERROR_LOG("Failed to load shader '{}': {}", shader_name, error->GetDescription());
      shader.reset();
    }

    return shader;
  }

  Error::SetStringFmt(error, "Failed to locate shader '{}'", shader_name);
  return {};
}

SettingsInterface& PostProcessing::GetLoadSettingsInterface(const char* section)
{
  // If PostProcessing/Enable is set in the game settings interface, use that.
  // Otherwise, use the base settings.

  SettingsInterface* game_si = Host::Internal::GetGameSettingsLayer();
  if (game_si && game_si->ContainsValue(section, "Enabled"))
    return *game_si;
  else
    return *Host::Internal::GetBaseSettingsLayer();
}

const Timer& PostProcessing::GetTimer()
{
  return s_timer;
}

GPUSampler* PostProcessing::GetSampler(const GPUSampler::Config& config)
{
  auto it = s_samplers.find(config.key);
  if (it != s_samplers.end())
    return it->second.get();

  std::unique_ptr<GPUSampler> sampler = g_gpu_device->CreateSampler(config);
  if (!sampler)
    ERROR_LOG("Failed to create GPU sampler with config={:X}", config.key);

  it = s_samplers.emplace(config.key, std::move(sampler)).first;
  return it->second.get();
}

GPUTexture* PostProcessing::GetDummyTexture()
{
  if (s_dummy_texture)
    return s_dummy_texture.get();

  const u32 zero = 0;
  s_dummy_texture = g_gpu_device->FetchTexture(1, 1, 1, 1, 1, GPUTexture::Type::Texture, GPUTexture::Format::RGBA8,
                                               GPUTexture::Flags::None, &zero, sizeof(zero));
  if (!s_dummy_texture)
    ERROR_LOG("Failed to create dummy texture.");

  return s_dummy_texture.get();
}
