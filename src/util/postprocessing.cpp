// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "postprocessing.h"
#include "gpu_device.h"
#include "host.h"
#include "imgui_manager.h"
#include "postprocessing_shader.h"
#include "postprocessing_shader_fx.h"
#include "postprocessing_shader_glsl.h"

// TODO: Remove me
#include "core/host.h"
#include "core/settings.h"

#include "IconsFontAwesome5.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "fmt/format.h"

Log_SetChannel(PostProcessing);

// TODO: ProgressCallbacks for shader compiling, it can be a bit slow.
// TODO: buffer width/height is wrong on resize, need to change it somehow.

namespace PostProcessing {
template<typename T>
static u32 ParseVector(const std::string_view& line, ShaderOption::ValueVector* values);

static TinyString ValueToString(ShaderOption::Type type, u32 vector_size, const ShaderOption::ValueVector& value);

static TinyString GetStageConfigSection(u32 index);
static void CopyStageConfig(SettingsInterface& si, u32 old_index, u32 new_index);
static void SwapStageConfig(SettingsInterface& si, u32 lhs_index, u32 rhs_index);
static std::unique_ptr<Shader> TryLoadingShader(const std::string& shader_name, bool only_config, Error* error);
static void ClearStagesWithError(const Error& error);
static SettingsInterface& GetLoadSettingsInterface();
static void LoadStages();
static void DestroyTextures();

static std::vector<std::unique_ptr<PostProcessing::Shader>> s_stages;
static bool s_enabled = false;

static GPUTexture::Format s_target_format = GPUTexture::Format::Unknown;
static u32 s_target_width = 0;
static u32 s_target_height = 0;
static Common::Timer s_timer;

static std::unique_ptr<GPUTexture> s_input_texture;

static std::unique_ptr<GPUTexture> s_output_texture;

static std::unordered_map<u64, std::unique_ptr<GPUSampler>> s_samplers;
static std::unique_ptr<GPUTexture> s_dummy_texture;
} // namespace PostProcessing

template<typename T>
u32 PostProcessing::ParseVector(const std::string_view& line, ShaderOption::ValueVector* values)
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

u32 PostProcessing::ShaderOption::ParseFloatVector(const std::string_view& line, ValueVector* values)
{
  return ParseVector<float>(line, values);
}

u32 PostProcessing::ShaderOption::ParseIntVector(const std::string_view& line, ValueVector* values)
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
        ret.append_fmt("{}", value[i].int_value);
        break;

      case ShaderOption::Type::Float:
        ret.append_fmt("{}", value[i].float_value);
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

    // swap any backslashes for forward slashes so the config is cross-platform
    for (size_t i = 0; i < fd.FileName.size(); i++)
    {
      if (fd.FileName[i] == '\\')
        fd.FileName[i] = '/';
    }

    if (std::none_of(names.begin(), names.end(), [&fd](const auto& other) { return fd.FileName == other.second; }))
    {
      std::string display_name = fmt::format(TRANSLATE_FS("PostProcessing", "{} [GLSL]"), fd.FileName);
      names.emplace_back(std::move(display_name), std::move(fd.FileName));
    }
  }

  FileSystem::FindFiles(Path::Combine(EmuFolders::Shaders, "reshade" FS_OSPATH_SEPARATOR_STR "Shaders").c_str(), "*.fx",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &results);
  FileSystem::FindFiles(
    Path::Combine(EmuFolders::Resources, "shaders" FS_OSPATH_SEPARATOR_STR "reshade" FS_OSPATH_SEPARATOR_STR "Shaders")
      .c_str(),
    "*.fx", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_KEEP_ARRAY, &results);
  std::sort(results.begin(), results.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.FileName < rhs.FileName; });

  for (FILESYSTEM_FIND_DATA& fd : results)
  {
    size_t pos = fd.FileName.rfind('.');
    if (pos != std::string::npos && pos > 0)
      fd.FileName.erase(pos);

    // swap any backslashes for forward slashes so the config is cross-platform
    for (size_t i = 0; i < fd.FileName.size(); i++)
    {
      if (fd.FileName[i] == '\\')
        fd.FileName[i] = '/';
    }

    if (std::none_of(names.begin(), names.end(), [&fd](const auto& other) { return fd.FileName == other.second; }))
    {
      std::string display_name = fmt::format(TRANSLATE_FS("PostProcessing", "{} [ReShade]"), fd.FileName);
      names.emplace_back(std::move(display_name), std::move(fd.FileName));
    }
  }

  return names;
}

TinyString PostProcessing::GetStageConfigSection(u32 index)
{
  return TinyString::from_fmt("PostProcessing/Stage{}", index + 1);
}

void PostProcessing::CopyStageConfig(SettingsInterface& si, u32 old_index, u32 new_index)
{
  const TinyString old_section = GetStageConfigSection(old_index);
  const TinyString new_section = GetStageConfigSection(new_index);

  si.ClearSection(new_section);

  for (const auto& [key, value] : si.GetKeyValueList(old_section))
    si.SetStringValue(new_section, key.c_str(), value.c_str());
}

void PostProcessing::SwapStageConfig(SettingsInterface& si, u32 lhs_index, u32 rhs_index)
{
  const TinyString lhs_section = GetStageConfigSection(lhs_index);
  const TinyString rhs_section = GetStageConfigSection(rhs_index);

  const std::vector<std::pair<std::string, std::string>> lhs_kvs = si.GetKeyValueList(lhs_section);
  si.ClearSection(lhs_section);

  const std::vector<std::pair<std::string, std::string>> rhs_kvs = si.GetKeyValueList(rhs_section);
  si.ClearSection(rhs_section);

  for (const auto& [key, value] : rhs_kvs)
    si.SetStringValue(lhs_section, key.c_str(), value.c_str());

  for (const auto& [key, value] : lhs_kvs)
    si.SetStringValue(rhs_section, key.c_str(), value.c_str());
}

u32 PostProcessing::Config::GetStageCount(const SettingsInterface& si)
{
  return si.GetUIntValue("PostProcessing", "StageCount", 0u);
}

std::string PostProcessing::Config::GetStageShaderName(const SettingsInterface& si, u32 index)
{
  return si.GetStringValue(GetStageConfigSection(index), "ShaderName");
}

std::vector<PostProcessing::ShaderOption> PostProcessing::Config::GetStageOptions(const SettingsInterface& si,
                                                                                  u32 index)
{
  std::vector<PostProcessing::ShaderOption> ret;

  const TinyString section = GetStageConfigSection(index);
  const std::string shader_name = si.GetStringValue(section, "ShaderName");
  if (shader_name.empty())
    return ret;

  std::unique_ptr<Shader> shader = TryLoadingShader(shader_name, true, nullptr);
  if (!shader)
    return ret;

  shader->LoadOptions(si, section);
  ret = shader->TakeOptions();
  return ret;
}

bool PostProcessing::Config::AddStage(SettingsInterface& si, const std::string& shader_name, Error* error)
{
  std::unique_ptr<Shader> shader = TryLoadingShader(shader_name, true, error);
  if (!shader)
    return false;

  const u32 index = GetStageCount(si);
  si.SetUIntValue("PostProcessing", "StageCount", index + 1);

  const TinyString section = GetStageConfigSection(index);
  si.SetStringValue(section, "ShaderName", shader->GetName().c_str());

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

void PostProcessing::Config::RemoveStage(SettingsInterface& si, u32 index)
{
  const u32 stage_count = GetStageCount(si);
  if (index >= stage_count)
    return;

  for (u32 i = index; i < (stage_count - 1); i++)
    CopyStageConfig(si, i + 1, i);

  si.ClearSection(GetStageConfigSection(stage_count - 1));
  si.SetUIntValue("PostProcessing", "StageCount", stage_count - 1);
}

void PostProcessing::Config::MoveStageUp(SettingsInterface& si, u32 index)
{
  const u32 stage_count = GetStageCount(si);
  if (index == 0 || index >= stage_count)
    return;

  SwapStageConfig(si, index, index - 1);
}

void PostProcessing::Config::MoveStageDown(SettingsInterface& si, u32 index)
{
  const u32 stage_count = GetStageCount(si);
  if ((index + 1) >= stage_count)
    return;

  SwapStageConfig(si, index, index + 1);
}

void PostProcessing::Config::SetStageOption(SettingsInterface& si, u32 index, const ShaderOption& option)
{
  const TinyString section = GetStageConfigSection(index);
  si.SetStringValue(section, option.name.c_str(), ValueToString(option.type, option.vector_size, option.value));
}

void PostProcessing::Config::UnsetStageOption(SettingsInterface& si, u32 index, const ShaderOption& option)
{
  const TinyString section = GetStageConfigSection(index);
  si.DeleteValue(section, option.name.c_str());
}

void PostProcessing::Config::ClearStages(SettingsInterface& si)
{
  const u32 count = GetStageCount(si);
  for (s32 i = static_cast<s32>(count - 1); i >= 0; i--)
    si.ClearSection(GetStageConfigSection(static_cast<u32>(i)));
  si.SetUIntValue("PostProcessing", "StageCount", 0);
}

bool PostProcessing::IsActive()
{
  return s_enabled && !s_stages.empty();
}

bool PostProcessing::IsEnabled()
{
  return s_enabled;
}

void PostProcessing::SetEnabled(bool enabled)
{
  s_enabled = enabled;
}

std::unique_ptr<PostProcessing::Shader> PostProcessing::TryLoadingShader(const std::string& shader_name,
                                                                         bool only_config, Error* error)
{
  std::string filename;
  std::optional<std::string> resource_str;

  // Try reshade first.
  filename = Path::Combine(
    EmuFolders::Shaders,
    fmt::format("reshade" FS_OSPATH_SEPARATOR_STR "Shaders" FS_OSPATH_SEPARATOR_STR "{}.fx", shader_name));
  if (FileSystem::FileExists(filename.c_str()))
  {
    std::unique_ptr<ReShadeFXShader> shader = std::make_unique<ReShadeFXShader>();
    if (shader->LoadFromFile(std::string(shader_name), filename.c_str(), only_config, error))
      return shader;
  }

  filename = Path::Combine(EmuFolders::Shaders, fmt::format("{}.glsl", shader_name));
  if (FileSystem::FileExists(filename.c_str()))
  {
    std::unique_ptr<GLSLShader> shader = std::make_unique<GLSLShader>();
    if (shader->LoadFromFile(std::string(shader_name), filename.c_str(), error))
      return shader;
  }

  filename =
    fmt::format("shaders/reshade" FS_OSPATH_SEPARATOR_STR "Shaders" FS_OSPATH_SEPARATOR_STR "{}.fx", shader_name);
  resource_str = Host::ReadResourceFileToString(filename.c_str());
  if (resource_str.has_value())
  {
    std::unique_ptr<ReShadeFXShader> shader = std::make_unique<ReShadeFXShader>();
    if (shader->LoadFromString(std::string(shader_name), std::move(filename), std::move(resource_str.value()),
                               only_config, error))
    {
      return shader;
    }
  }

  filename = fmt::format("shaders" FS_OSPATH_SEPARATOR_STR "{}.glsl", shader_name);
  resource_str = Host::ReadResourceFileToString(filename.c_str());
  if (resource_str.has_value())
  {
    std::unique_ptr<GLSLShader> shader = std::make_unique<GLSLShader>();
    if (shader->LoadFromString(std::string(shader_name), std::move(resource_str.value()), error))
      return shader;
  }

  Log_ErrorFmt("Failed to load shader '{}'", shader_name);
  return {};
}

void PostProcessing::ClearStagesWithError(const Error& error)
{
  std::string msg = error.GetDescription();
  Host::AddIconOSDMessage(
    "PostProcessLoadFail", ICON_FA_EXCLAMATION_TRIANGLE,
    fmt::format(TRANSLATE_FS("OSDMessage", "Failed to load post-processing chain: {}"),
                msg.empty() ? TRANSLATE_SV("PostProcessing", "Unknown Error") : std::string_view(msg)),
    Host::OSD_ERROR_DURATION);
  s_stages.clear();
}

SettingsInterface& PostProcessing::GetLoadSettingsInterface()
{
  // If PostProcessing/Enable is set in the game settings interface, use that.
  // Otherwise, use the base settings.

  SettingsInterface* game_si = Host::Internal::GetGameSettingsLayer();
  if (game_si && game_si->ContainsValue("PostProcessing", "Enabled"))
    return *game_si;
  else
    return *Host::Internal::GetBaseSettingsLayer();
}

void PostProcessing::Initialize()
{
  LoadStages();
}

void PostProcessing::LoadStages()
{
  auto lock = Host::GetSettingsLock();
  SettingsInterface& si = GetLoadSettingsInterface();

  s_enabled = si.GetBoolValue("PostProcessing", "Enabled", false);

  const u32 stage_count = Config::GetStageCount(si);
  if (stage_count == 0)
    return;

  Error error;

  for (u32 i = 0; i < stage_count; i++)
  {
    std::string stage_name = Config::GetStageShaderName(si, i);
    if (stage_name.empty())
    {
      error.SetString(fmt::format("No stage name in stage {}.", i + 1));
      ClearStagesWithError(error);
      return;
    }

    lock.unlock();

    std::unique_ptr<Shader> shader = TryLoadingShader(stage_name, false, &error);
    if (!shader)
    {
      ClearStagesWithError(error);
      return;
    }

    lock.lock();
    shader->LoadOptions(si, GetStageConfigSection(i));
    s_stages.push_back(std::move(shader));
  }

  if (stage_count > 0)
  {
    s_timer.Reset();
    Log_DevPrintf("Loaded %u post-processing stages.", stage_count);
  }
}

void PostProcessing::UpdateSettings()
{
  auto lock = Host::GetSettingsLock();
  SettingsInterface& si = GetLoadSettingsInterface();

  s_enabled = si.GetBoolValue("PostProcessing", "Enabled", false);

  const u32 stage_count = Config::GetStageCount(si);
  if (stage_count == 0)
  {
    s_stages.clear();
    return;
  }

  Error error;

  s_stages.resize(stage_count);

  for (u32 i = 0; i < stage_count; i++)
  {
    std::string stage_name = Config::GetStageShaderName(si, i);
    if (stage_name.empty())
    {
      error.SetString(fmt::format("No stage name in stage {}.", i + 1));
      ClearStagesWithError(error);
      return;
    }

    if (!s_stages[i] || stage_name != s_stages[i]->GetName())
    {
      if (i < s_stages.size())
        s_stages[i].reset();

      // Force recompile.
      s_target_format = GPUTexture::Format::Unknown;

      lock.unlock();

      std::unique_ptr<Shader> shader = TryLoadingShader(stage_name, false, &error);
      if (!shader)
      {
        ClearStagesWithError(error);
        return;
      }

      if (i < s_stages.size())
        s_stages[i] = std::move(shader);
      else
        s_stages.push_back(std::move(shader));

      lock.lock();
    }

    s_stages[i]->LoadOptions(si, GetStageConfigSection(i));
  }

  if (stage_count > 0)
  {
    s_timer.Reset();
    Log_DevPrintf("Loaded %u post-processing stages.", stage_count);
  }
}

void PostProcessing::Toggle()
{
  if (s_stages.empty())
  {
    Host::AddIconOSDMessage("PostProcessing", ICON_FA_PAINT_ROLLER,
                            TRANSLATE_STR("OSDMessage", "No post-processing shaders are selected."),
                            Host::OSD_QUICK_DURATION);
    return;
  }

  const bool new_enabled = !s_enabled;
  Host::AddIconOSDMessage("PostProcessing", ICON_FA_PAINT_ROLLER,
                          new_enabled ? TRANSLATE_STR("OSDMessage", "Post-processing is now enabled.") :
                                        TRANSLATE_STR("OSDMessage", "Post-processing is now disabled."),
                          Host::OSD_QUICK_DURATION);
  s_enabled = new_enabled;
  if (s_enabled)
    s_timer.Reset();
}

bool PostProcessing::ReloadShaders()
{
  if (s_stages.empty())
  {
    Host::AddIconOSDMessage("PostProcessing", ICON_FA_PAINT_ROLLER,
                            TRANSLATE_STR("OSDMessage", "No post-processing shaders are selected."),
                            Host::OSD_QUICK_DURATION);
    return false;
  }

  decltype(s_stages)().swap(s_stages);
  DestroyTextures();
  LoadStages();

  Host::AddIconOSDMessage("PostProcessing", ICON_FA_PAINT_ROLLER,
                          TRANSLATE_STR("OSDMessage", "Post-processing shaders reloaded."), Host::OSD_QUICK_DURATION);
  return true;
}

void PostProcessing::Shutdown()
{
  g_gpu_device->RecycleTexture(std::move(s_dummy_texture));
  s_samplers.clear();
  s_enabled = false;
  decltype(s_stages)().swap(s_stages);
  DestroyTextures();
}

GPUTexture* PostProcessing::GetInputTexture()
{
  return s_input_texture.get();
}

const Common::Timer& PostProcessing::GetTimer()
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
    Log_ErrorFmt("Failed to create GPU sampler with config={:X}", config.key);

  it = s_samplers.emplace(config.key, std::move(sampler)).first;
  return it->second.get();
}

GPUTexture* PostProcessing::GetDummyTexture()
{
  if (s_dummy_texture)
    return s_dummy_texture.get();

  const u32 zero = 0;
  s_dummy_texture = g_gpu_device->FetchTexture(1, 1, 1, 1, 1, GPUTexture::Type::Texture, GPUTexture::Format::RGBA8,
                                               &zero, sizeof(zero));
  if (!s_dummy_texture)
    Log_ErrorPrint("Failed to create dummy texture.");

  return s_dummy_texture.get();
}

bool PostProcessing::CheckTargets(GPUTexture::Format target_format, u32 target_width, u32 target_height)
{
  if (s_target_format == target_format && s_target_width == target_width && s_target_height == target_height)
    return true;

  // In case any allocs fail.
  DestroyTextures();

  if (!(s_input_texture = g_gpu_device->FetchTexture(target_width, target_height, 1, 1, 1,
                                                     GPUTexture::Type::RenderTarget, target_format)) ||
      !(s_output_texture = g_gpu_device->FetchTexture(target_width, target_height, 1, 1, 1,
                                                      GPUTexture::Type::RenderTarget, target_format)))
  {
    return false;
  }

  for (auto& shader : s_stages)
  {
    if (!shader->CompilePipeline(target_format, target_width, target_height) ||
        !shader->ResizeOutput(target_format, target_width, target_height))
    {
      Log_ErrorPrintf("Failed to compile one or more post-processing shaders, disabling.");
      Host::AddIconOSDMessage(
        "PostProcessLoadFail", ICON_FA_EXCLAMATION_TRIANGLE,
        fmt::format("Failed to compile post-processing shader '{}'. Disabling post-processing.", shader->GetName()));
      s_enabled = false;
      return false;
    }
  }

  s_target_format = target_format;
  s_target_width = target_width;
  s_target_height = target_height;
  return true;
}

void PostProcessing::DestroyTextures()
{
  s_target_format = GPUTexture::Format::Unknown;
  s_target_width = 0;
  s_target_height = 0;

  g_gpu_device->RecycleTexture(std::move(s_output_texture));
  g_gpu_device->RecycleTexture(std::move(s_input_texture));
}

bool PostProcessing::Apply(GPUTexture* final_target, s32 final_left, s32 final_top, s32 final_width, s32 final_height,
                           s32 orig_width, s32 orig_height)
{
  GL_SCOPE("PostProcessing Apply");

  const u32 target_width = final_target ? final_target->GetWidth() : g_gpu_device->GetWindowWidth();
  const u32 target_height = final_target ? final_target->GetHeight() : g_gpu_device->GetWindowHeight();
  const GPUTexture::Format target_format = final_target ? final_target->GetFormat() : g_gpu_device->GetWindowFormat();
  if (!CheckTargets(target_format, target_width, target_height))
    return false;

  GPUTexture* input = s_input_texture.get();
  GPUTexture* output = s_output_texture.get();
  input->MakeReadyForSampling();

  for (const std::unique_ptr<Shader>& stage : s_stages)
  {
    const bool is_final = (stage.get() == s_stages.back().get());

    if (!stage->Apply(input, is_final ? final_target : output, final_left, final_top, final_width, final_height,
                      orig_width, orig_height, s_target_width, s_target_height))
    {
      return false;
    }

    if (!is_final)
    {
      output->MakeReadyForSampling();
      std::swap(input, output);
    }
  }

  return true;
}
