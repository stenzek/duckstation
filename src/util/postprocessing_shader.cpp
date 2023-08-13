// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "postprocessing_shader.h"

#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"

#include <cctype>
#include <cstring>
#include <sstream>

Log_SetChannel(PostProcessingShader);

void PostProcessingShader::ParseKeyValue(const std::string_view& line, std::string_view* key, std::string_view* value)
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
u32 PostProcessingShader::ParseVector(const std::string_view& line, PostProcessingShader::Option::ValueVector* values)
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

template u32 PostProcessingShader::ParseVector<s32>(const std::string_view& line,
                                                    PostProcessingShader::Option::ValueVector* values);
template u32 PostProcessingShader::ParseVector<float>(const std::string_view& line,
                                                      PostProcessingShader::Option::ValueVector* values);

PostProcessingShader::PostProcessingShader() = default;

PostProcessingShader::PostProcessingShader(std::string name) : m_name(std::move(name))
{
}

PostProcessingShader::~PostProcessingShader() = default;

bool PostProcessingShader::IsValid() const
{
  return false;
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

PostProcessingShader::Option* PostProcessingShader::GetOptionByName(const std::string_view& name)
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
