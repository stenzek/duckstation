#include "postprocessing_shader.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/shadergen.h"
#include <cctype>
#include <cstring>
#include <sstream>
Log_SetChannel(PostProcessingShader);

namespace FrontendCommon {

void ParseKeyValue(const std::string_view& line, std::string_view* key, std::string_view* value)
{
  size_t key_start = 0;
  while (key_start < line.size() && std::isspace(line[key_start]))
    key_start++;

  size_t key_end = key_start;
  while (key_end < line.size() && (!std::isspace(line[key_end]) && line[key_end] != '='))
    key_end++;

  if (key_start == key_end || key_end == line.size())
    return;

  size_t value_start = key_end;
  while (value_start < line.size() && std::isspace(line[value_start]))
    value_start++;

  if (value_start == line.size() || line[value_start] != '=')
    return;

  value_start++;
  while (value_start < line.size() && std::isspace(line[value_start]))
    value_start++;

  size_t value_end = line.size();
  while (value_end > value_start && std::isspace(line[value_end - 1]))
    value_end--;

  if (value_start == value_end)
    return;

  *key = line.substr(key_start, key_end - key_start);
  *value = line.substr(value_start, value_end - value_start);
}

template<typename T>
u32 ParseVector(const std::string_view& line, PostProcessingShader::Option::ValueVector* values)
{
  u32 index = 0;
  size_t start = 0;
  while (index < PostProcessingShader::Option::MAX_VECTOR_COMPONENTS)
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

  for (; index < PostProcessingShader::Option::MAX_VECTOR_COMPONENTS; index++)
  {
    if constexpr (std::is_same_v<T, float>)
      (*values)[index++].float_value = 0.0f;
    else if constexpr (std::is_same_v<T, s32>)
      (*values)[index++].int_value = 0;
  }

  return size;
}

PostProcessingShader::PostProcessingShader() = default;

PostProcessingShader::PostProcessingShader(std::string name, std::string code) : m_name(name), m_code(code)
{
  LoadOptions();
}

PostProcessingShader::PostProcessingShader(const PostProcessingShader& copy)
  : m_name(copy.m_name), m_code(copy.m_code), m_options(copy.m_options)
{
}

PostProcessingShader::PostProcessingShader(PostProcessingShader& move)
  : m_name(std::move(move.m_name)), m_code(std::move(move.m_code)), m_options(std::move(move.m_options))
{
}

PostProcessingShader::~PostProcessingShader() = default;

bool PostProcessingShader::LoadFromFile(std::string name, const char* filename)
{
  std::optional<std::string> code = FileSystem::ReadFileToString(filename);
  if (!code.has_value() || code->empty())
    return false;

  return LoadFromString(std::move(name), code.value());
}

bool PostProcessingShader::LoadFromString(std::string name, std::string code)
{
  m_name = std::move(name);
  m_code = std::move(code);
  m_options.clear();
  LoadOptions();
  return true;
}

bool PostProcessingShader::IsValid() const
{
  return !m_name.empty() && !m_code.empty();
}

const PostProcessingShader::Option* PostProcessingShader::GetOptionByName(const std::string_view& name) const
{
  for (const Option& option : m_options)
  {
    if (option.name == name)
      return &option;
  }

  return nullptr;
}

FrontendCommon::PostProcessingShader::Option* PostProcessingShader::GetOptionByName(const std::string_view& name)
{
  for (Option& option : m_options)
  {
    if (option.name == name)
      return &option;
  }

  return nullptr;
}

std::string PostProcessingShader::GetConfigString() const
{
  std::stringstream ss;
  bool first = true;
  for (const Option& option : m_options)
  {
    if (!first)
      ss << ';';
    else
      first = false;

    ss << option.name;
    ss << '=';

    for (u32 i = 0; i < option.vector_size; i++)
    {
      if (i > 0)
        ss << ",";

      switch (option.type)
      {
        case Option::Type::Bool:
          ss << ((option.value[i].int_value != 0) ? "true" : "false");
          break;

        case Option::Type::Int:
          ss << option.value[i].int_value;
          break;

        case Option::Type::Float:
          ss << option.value[i].float_value;
          break;

        default:
          break;
      }
    }
  }

  return ss.str();
}

void PostProcessingShader::SetConfigString(const std::string_view& str)
{
  for (Option& option : m_options)
    option.value = option.default_value;

  size_t last_sep = 0;
  while (last_sep < str.size())
  {
    size_t next_sep = str.find(';', last_sep);
    if (next_sep == std::string_view::npos)
      next_sep = str.size();

    const std::string_view kv = str.substr(last_sep, next_sep - last_sep);
    std::string_view key, value;
    ParseKeyValue(kv, &key, &value);
    if (!key.empty() && !value.empty())
    {
      Option* option = GetOptionByName(key);
      if (option)
      {
        switch (option->type)
        {
          case Option::Type::Bool:
            option->value[0].int_value = StringUtil::FromChars<bool>(value).value_or(false) ? 1 : 0;
            break;

          case Option::Type::Int:
            ParseVector<s32>(value, &option->value);
            break;

          case Option::Type::Float:
            ParseVector<float>(value, &option->value);
            break;

          default:
            break;
        }
      }
    }

    last_sep = next_sep + 1;
  }
}

bool PostProcessingShader::UsePushConstants() const
{
  return GetUniformsSize() <= PUSH_CONSTANT_SIZE_THRESHOLD;
}

u32 PostProcessingShader::GetUniformsSize() const
{
  // lazy packing. todo improve.
  return sizeof(CommonUniforms) + (sizeof(Option::ValueVector) * static_cast<u32>(m_options.size()));
}

void PostProcessingShader::FillUniformBuffer(void* buffer, u32 texture_width, s32 texture_height, s32 texture_view_x,
                                             s32 texture_view_y, s32 texture_view_width, s32 texture_view_height,
                                             u32 window_width, u32 window_height, float time) const
{
  CommonUniforms* common = static_cast<CommonUniforms*>(buffer);

  // TODO: OpenGL?
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
  common->time = time;

  u8* option_values = reinterpret_cast<u8*>(common + 1);
  for (const Option& option : m_options)
  {
    std::memcpy(option_values, option.value.data(), sizeof(Option::ValueVector));
    option_values += sizeof(Option::ValueVector);
  }
}

FrontendCommon::PostProcessingShader& PostProcessingShader::operator=(const PostProcessingShader& copy)
{
  m_name = copy.m_name;
  m_code = copy.m_code;
  m_options = copy.m_options;
  return *this;
}

FrontendCommon::PostProcessingShader& PostProcessingShader::operator=(PostProcessingShader& move)
{
  m_name = std::move(move.m_name);
  m_code = std::move(move.m_code);
  m_options = std::move(move.m_options);
  return *this;
}

void PostProcessingShader::LoadOptions()
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

  Option current_option = {};
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
          if (current_option.type != Option::Type::Invalid)
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
            current_option.type = Option::Type::Bool;
          else if (sub == "OptionRangeFloat")
            current_option.type = Option::Type::Float;
          else if (sub == "OptionRangeInteger")
            current_option.type = Option::Type::Int;
          else
            Log_ErrorPrintf("Invalid option type: '%s'", line_str.c_str());

          continue;
        }
      }

      if (current_option.type == Option::Type::Invalid)
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
          Option::ValueVector* dst_array;
          if (key == "MinValue")
            dst_array = &current_option.min_value;
          else if (key == "MaxValue")
            dst_array = &current_option.max_value;
          else if (key == "DefaultValue")
            dst_array = &current_option.default_value;
          else // if (key == "StepAmount")
            dst_array = &current_option.step_value;

          u32 size = 0;
          if (current_option.type == Option::Type::Bool)
            (*dst_array)[size++].int_value = StringUtil::FromChars<bool>(value).value_or(false) ? 1 : 0;
          else if (current_option.type == Option::Type::Float)
            size = ParseVector<float>(value, dst_array);
          else if (current_option.type == Option::Type::Int)
            size = ParseVector<s32>(value, dst_array);

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

  if (current_option.type != Option::Type::Invalid && !current_option.name.empty() && current_option.vector_size > 0)
  {
    current_option.value = current_option.default_value;
    if (current_option.ui_name.empty())
      current_option.ui_name = current_option.name;

    m_options.push_back(std::move(current_option));
  }
}

} // namespace FrontendCommon
