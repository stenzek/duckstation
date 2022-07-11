#include "postprocessing_chain.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string.h"
#include "common/path.h"
#include "core/host.h"
#include "core/settings.h"
#include "fmt/format.h"
#include <sstream>
Log_SetChannel(PostProcessingChain);

namespace FrontendCommon {

static bool TryLoadingShader(PostProcessingShader* shader, const std::string_view& shader_name)
{
  std::string filename(Path::Combine(EmuFolders::Shaders, fmt::format("{}.glsl", shader_name)));
  if (FileSystem::FileExists(filename.c_str()))
  {
    if (shader->LoadFromFile(std::string(shader_name), filename.c_str()))
      return true;
  }

  std::optional<std::string> resource_str(Host::ReadResourceFileToString(fmt::format("shaders" FS_OSPATH_SEPARATOR_STR "{}.glsl", shader_name).c_str()));
  if (resource_str.has_value() && shader->LoadFromString(std::string(shader_name), std::move(resource_str.value())))
    return true;

  Log_ErrorPrintf("Failed to load shader from '%s'", filename.c_str());
  return false;
}

PostProcessingChain::PostProcessingChain() = default;

PostProcessingChain::~PostProcessingChain() = default;

void PostProcessingChain::AddShader(PostProcessingShader shader)
{
  m_shaders.push_back(std::move(shader));
}

bool PostProcessingChain::AddStage(const std::string_view& name)
{
  PostProcessingShader shader;
  if (!TryLoadingShader(&shader, name))
    return false;

  m_shaders.push_back(std::move(shader));
  return true;
}

std::string PostProcessingChain::GetConfigString() const
{
  std::stringstream ss;
  bool first = true;

  for (const PostProcessingShader& shader : m_shaders)
  {
    if (!first)
      ss << ':';
    else
      first = false;

    ss << shader.GetName();
    std::string config_string = shader.GetConfigString();
    if (!config_string.empty())
      ss << ';' << config_string;
  }

  return ss.str();
}

bool PostProcessingChain::CreateFromString(const std::string_view& chain_config)
{
  std::vector<PostProcessingShader> shaders;

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
      PostProcessingShader shader;
      if (!TryLoadingShader(&shader, shader_name))
        return false;

      if (first_shader_sep < shader_config.size())
        shader.SetConfigString(shader_config.substr(first_shader_sep + 1));

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

  PostProcessingShader shader = std::move(m_shaders[index]);
  m_shaders.erase(m_shaders.begin() + index);
  m_shaders.insert(m_shaders.begin() + (index - 1u), std::move(shader));
}

void PostProcessingChain::MoveStageDown(u32 index)
{
  Assert(index < m_shaders.size());
  if (index == (m_shaders.size() - 1u))
    return;

  PostProcessingShader shader = std::move(m_shaders[index]);
  m_shaders.erase(m_shaders.begin() + index);
  m_shaders.insert(m_shaders.begin() + (index + 1u), std::move(shader));
}

void PostProcessingChain::ClearStages()
{
  m_shaders.clear();
}

} // namespace FrontendCommon