// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "postprocessing_chain.h"
#include "gpu_device.h"
#include "postprocessing_shader_glsl.h"

#include "core/host.h"
#include "core/settings.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string.h"

#include "fmt/format.h"
#include <sstream>

Log_SetChannel(PostProcessingChain);

static std::unique_ptr<PostProcessingShader> TryLoadingShader(const std::string_view& shader_name)
{
  std::string filename(Path::Combine(EmuFolders::Shaders, fmt::format("{}.glsl", shader_name)));
  if (FileSystem::FileExists(filename.c_str()))
  {
    std::unique_ptr<PostProcessingShaderGLSL> shader = std::make_unique<PostProcessingShaderGLSL>();
    if (shader->LoadFromFile(std::string(shader_name), filename.c_str()))
      return shader;
  }

  std::optional<std::string> resource_str(
    Host::ReadResourceFileToString(fmt::format("shaders" FS_OSPATH_SEPARATOR_STR "{}.glsl", shader_name).c_str()));
  if (resource_str.has_value())
  {
    std::unique_ptr<PostProcessingShaderGLSL> shader = std::make_unique<PostProcessingShaderGLSL>();
    if (shader->LoadFromString(std::string(shader_name), std::move(resource_str.value())))
      return shader;
  }

  Log_ErrorPrintf(fmt::format("Failed to load shader '{}'", shader_name).c_str());
  return {};
}

PostProcessingChain::PostProcessingChain() = default;

PostProcessingChain::~PostProcessingChain() = default;

void PostProcessingChain::AddShader(std::unique_ptr<PostProcessingShader> shader)
{
  m_shaders.push_back(std::move(shader));
}

bool PostProcessingChain::AddStage(const std::string_view& name)
{
  std::unique_ptr<PostProcessingShader> shader = TryLoadingShader(name);
  if (!shader)
    return false;

  m_shaders.push_back(std::move(shader));
  return true;
}

std::string PostProcessingChain::GetConfigString() const
{
  std::stringstream ss;
  bool first = true;

  for (const auto& shader : m_shaders)
  {
    if (!first)
      ss << ':';
    else
      first = false;

    ss << shader->GetName();
    std::string config_string = shader->GetConfigString();
    if (!config_string.empty())
      ss << ';' << config_string;
  }

  return ss.str();
}

bool PostProcessingChain::CreateFromString(const std::string_view& chain_config)
{
  std::vector<std::unique_ptr<PostProcessingShader>> shaders;

  size_t last_sep = 0;
  while (last_sep < chain_config.size())
  {
    size_t next_sep = chain_config.find(':', last_sep);
    if (next_sep == std::string::npos)
      next_sep = chain_config.size();

    const std::string_view shader_config = chain_config.substr(last_sep, next_sep - last_sep);
    size_t first_shader_sep = shader_config.find(';');
    if (first_shader_sep == std::string::npos)
      first_shader_sep = shader_config.size();

    const std::string_view shader_name = shader_config.substr(0, first_shader_sep);
    if (!shader_name.empty())
    {
      std::unique_ptr<PostProcessingShader> shader = TryLoadingShader(shader_name);
      if (!shader)
        return false;

      if (first_shader_sep < shader_config.size())
        shader->SetConfigString(shader_config.substr(first_shader_sep + 1));

      shaders.push_back(std::move(shader));
    }

    last_sep = next_sep + 1;
  }

  if (shaders.empty())
  {
    Log_ErrorPrintf("Postprocessing chain is empty!");
    return false;
  }

  m_shaders = std::move(shaders);
  Log_InfoPrintf("Loaded postprocessing chain of %zu shaders", m_shaders.size());
  return true;
}

std::vector<std::string> PostProcessingChain::GetAvailableShaderNames()
{
  std::vector<std::string> names;

  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(Path::Combine(EmuFolders::Resources, "shaders").c_str(), "*.glsl",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_RELATIVE_PATHS, &results);
  FileSystem::FindFiles(EmuFolders::Shaders.c_str(), "*.glsl",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_RELATIVE_PATHS |
                          FILESYSTEM_FIND_KEEP_ARRAY,
                        &results);

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

    if (std::none_of(names.begin(), names.end(), [&fd](const std::string& other) { return fd.FileName == other; }))
    {
      names.push_back(std::move(fd.FileName));
    }
  }

  return names;
}

void PostProcessingChain::RemoveStage(u32 index)
{
  Assert(index < m_shaders.size());
  m_shaders.erase(m_shaders.begin() + index);
}

void PostProcessingChain::MoveStageUp(u32 index)
{
  Assert(index < m_shaders.size());
  if (index == 0)
    return;

  auto shader = std::move(m_shaders[index]);
  m_shaders.erase(m_shaders.begin() + index);
  m_shaders.insert(m_shaders.begin() + (index - 1u), std::move(shader));
}

void PostProcessingChain::MoveStageDown(u32 index)
{
  Assert(index < m_shaders.size());
  if (index == (m_shaders.size() - 1u))
    return;

  auto shader = std::move(m_shaders[index]);
  m_shaders.erase(m_shaders.begin() + index);
  m_shaders.insert(m_shaders.begin() + (index + 1u), std::move(shader));
}

void PostProcessingChain::ClearStages()
{
  m_shaders.clear();
}

bool PostProcessingChain::CheckTargets(GPUTexture::Format format, u32 target_width, u32 target_height)
{
  if (m_target_format == format && m_target_width == target_width && m_target_height == target_height)
    return true;

  // In case any allocs fail.
  m_target_format = GPUTexture::Format::Unknown;
  m_target_width = 0;
  m_target_height = 0;
  m_output_framebuffer.reset();
  m_output_texture.reset();
  m_input_framebuffer.reset();
  m_input_texture.reset();

  if (!(m_input_texture =
          g_gpu_device->CreateTexture(target_width, target_height, 1, 1, 1, GPUTexture::Type::RenderTarget, format)) ||
      !(m_input_framebuffer = g_gpu_device->CreateFramebuffer(m_input_texture.get())))
  {
    return false;
  }

  if (!(m_output_texture =
          g_gpu_device->CreateTexture(target_width, target_height, 1, 1, 1, GPUTexture::Type::RenderTarget, format)) ||
      !(m_output_framebuffer = g_gpu_device->CreateFramebuffer(m_output_texture.get())))
  {
    return false;
  }

  for (auto& shader : m_shaders)
  {
    if (!shader->CompilePipeline(format, target_width, target_height) ||
        !shader->ResizeOutput(format, target_width, target_height))
    {
      Log_ErrorPrintf("Failed to compile one or more post-processing shaders, disabling.");
      return false;
    }
  }

  m_target_format = format;
  m_target_width = target_width;
  m_target_height = target_height;

  return true;
}

bool PostProcessingChain::Apply(GPUFramebuffer* final_target, s32 final_left, s32 final_top, s32 final_width,
                                s32 final_height, s32 orig_width, s32 orig_height)
{
  GL_SCOPE("PostProcessingChain Apply");

  const u32 target_width = final_target ? final_target->GetWidth() : g_gpu_device->GetWindowWidth();
  const u32 target_height = final_target ? final_target->GetHeight() : g_gpu_device->GetWindowHeight();
  const GPUTexture::Format target_format =
    final_target ? final_target->GetRT()->GetFormat() : g_gpu_device->GetWindowFormat();
  if (!CheckTargets(target_format, target_width, target_height))
    return false;

  g_gpu_device->SetViewportAndScissor(final_left, final_top, final_width, final_height);

  GPUTexture* input = m_input_texture.get();
  GPUFramebuffer* input_fb = m_input_framebuffer.get();
  GPUTexture* output = m_output_texture.get();
  GPUFramebuffer* output_fb = m_output_framebuffer.get();
  input->MakeReadyForSampling();

  for (const std::unique_ptr<PostProcessingShader>& stage : m_shaders)
  {
    const bool is_final = (stage.get() == m_shaders.back().get());

    if (!stage->Apply(input, is_final ? nullptr : output_fb, final_left, final_top, final_width, final_height,
                      orig_width, orig_height, m_target_width, m_target_height))
    {
      return false;
    }

    if (!is_final)
    {
      output->MakeReadyForSampling();
      std::swap(input, output);
      std::swap(input_fb, output_fb);
    }
  }

  return true;
}
