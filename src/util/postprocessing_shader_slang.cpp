// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "postprocessing_shader_slang.h"
#include "dyn_shaderc.h"
#include "dyn_spirv_cross.h"
#include "image.h"
#include "shadergen.h"
#include "spirv_module.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/gsvector.h"
#include "common/gsvector_formatter.h"
#include "common/heterogeneous_containers.h"
#include "common/intrin.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <bitset>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

LOG_CHANNEL(PostProcessing);

// TODO:
//  - Need some sort of cache for the UBO/push constant layout so we don't need to go through SPIR-V Cross every time.

namespace PostProcessing {

static constexpr float DEFAULT_OPTION_STEP = 0.02f;

static constexpr u32 MAX_SLANG_INCLUDE_DEPTH = 16;
static constexpr u32 MAX_PUSH_CONSTANT_SIZE = 128;

namespace SlangShaderStage {
enum Id
{
  Common,
  Vertex,
  Fragment,
  MaxCount,
};
}

namespace BuiltinUniform {
enum : s32
{
  MVP = -1,
  OutputSize = -2,
  FinalViewportSize = -3,
  FrameCount = -4,
  FrameDirection = -5,
  TextureSize = -6,
  Zero = -7,
};
}

class SlangPresetParser
{
public:
  SlangPresetParser();
  ~SlangPresetParser();

  bool Parse(std::string_view path, std::string_view contents, Error* error);

  bool ContainsValue(std::string_view key) const;
  std::string_view GetStringValue(std::string_view key, std::string_view def) const;
  bool GetBoolValue(std::string_view key, bool def) const;
  s32 GetIntValue(std::string_view key, int def) const;
  u32 GetUIntValue(std::string_view key, u32 def) const;
  float GetFloatValue(std::string_view key, float def) const;

  bool ContainsIndexedValue(std::string_view key, u32 idx) const;
  std::string_view GetIndexedStringValue(std::string_view key, u32 idx, std::string_view def) const;
  bool GetIndexedBoolValue(std::string_view key, u32 idx, bool def) const;
  s32 GetIndexedIntValue(std::string_view key, u32 idx, int def) const;
  u32 GetIndexedUIntValue(std::string_view key, u32 idx, u32 def) const;
  float GetIndexedFloatValue(std::string_view key, u32 idx, float def) const;

private:
  static bool GetLine(const std::string_view& contents, std::string_view* line, size_t& offset);

  PreferUnorderedStringMap<std::string> m_options;
};

class SlangShaderPreprocessor
{
public:
  SlangShaderPreprocessor(Error* error);
  ~SlangShaderPreprocessor();

  const std::string& GetShaderName() const { return m_shader_name; }
  std::string TakeShaderName() { return std::move(m_shader_name); }
  bool HasShaderName() const { return !m_shader_name.empty(); }

  const std::vector<ShaderOption>& GetOptions() const { return m_options; }
  std::vector<ShaderOption> TakeOptions() { return std::move(m_options); }
  bool HasOptions() const { return !m_options.empty(); }

  bool HasOutputFormat() const { return m_output_format.has_value(); }
  GPUTexture::Format GetOutputFormat() const { return m_output_format.value_or(GPUTexture::Format::RGBA8); }

  std::string GetVertexShader() const;
  std::string GetFragmentShader() const;

  bool ParseFile(std::string_view base_path, std::string_view path);

private:
  static std::optional<std::pair<std::string, std::string>> ReadShaderFile(std::string_view base_path,
                                                                           std::string_view path, Error* error);

  std::string_view GetCurrentFilename() const;

  bool GetLine(std::string_view* line);

  bool HandleIncludeDirective(std::string_view line);
  bool HandlePragmaDirective(std::string_view line);
  bool HandlePragmaStage(std::span<const std::string_view> tokens);
  bool HandlePragmaName(std::span<const std::string_view> tokens);
  bool HandlePragmaParameter(std::span<const std::string_view> tokens);
  bool HandlePragmaFormat(std::span<const std::string_view> tokens);

  template<typename... T>
  void Write(fmt::format_string<T...> fmt, T&&... args);

  template<typename... T>
  void SetError(fmt::format_string<T...> fmt, T&&... args);

  Error* m_error;
  std::string_view m_path;
  std::string_view m_contents;

  size_t m_current_offset = 0;
  size_t m_current_line_offset = 0;
  u32 m_current_line_number = 0;
  bool m_needs_line_reset = false;
  bool m_needs_version_directive = true;

  u32 m_include_depth = 0;
  SlangShaderStage::Id m_current_stage = SlangShaderStage::Common;
  std::bitset<SlangShaderStage::MaxCount> m_defined_stages = {};

  std::string m_shader_code[SlangShaderStage::MaxCount];
  std::string m_shader_name;
  std::vector<ShaderOption> m_options;
  std::optional<GPUTexture::Format> m_output_format;
};

namespace {

struct SlangShaderVertex
{
  float position[4];
  float texcoord[2];
};

} // namespace

} // namespace PostProcessing

inline PostProcessing::SlangPresetParser::SlangPresetParser() = default;

inline PostProcessing::SlangPresetParser::~SlangPresetParser() = default;

inline bool PostProcessing::SlangPresetParser::GetLine(const std::string_view& contents, std::string_view* line,
                                                       size_t& offset)
{
  const size_t length = contents.length();
  if (offset == length)
    return false;

  size_t end_position = offset;
  for (; end_position < length; end_position++)
  {
    // ignore carriage returns
    if (contents[end_position] == '\r')
      continue;

    if (contents[end_position] == '\n')
      break;
  }

  *line = contents.substr(offset, end_position - offset);
  offset = std::min(end_position + 1, length);
  return true;
}

inline bool PostProcessing::SlangPresetParser::Parse(std::string_view path, std::string_view contents, Error* error)
{
  u32 line_number = 0;
  size_t offset = 0;
  std::string_view line;

  while (GetLine(contents, &line, offset))
  {
    line_number++;

    const std::string_view clean_line = StringUtil::StripWhitespace(line);
    if (clean_line.empty() || clean_line[0] == '#')
      continue;

    std::string_view key, value;
    if (!StringUtil::ParseAssignmentString(clean_line, &key, &value) || key.empty())
    {
      Error::SetStringFmt(error, "{}:{} Malformed preset line", Path::GetFileName(path), line_number);
      return false;
    }

    if (m_options.find(key) != m_options.end())
    {
      WARNING_LOG("{}:{} Duplicate preset key '{}'", Path::GetFileName(path), line_number, key);
      continue;
    }

    // quotes appear in some...
    std::string fixed_value(value);
    std::string::size_type pos;
    while ((pos = fixed_value.find('"')) != std::string::npos)
      fixed_value.erase(pos, 1);

    m_options.emplace(key, std::move(fixed_value));
  }

  return true;
}

inline bool PostProcessing::SlangPresetParser::ContainsValue(std::string_view key) const
{
  return (m_options.find(key) != m_options.end());
}

inline std::string_view PostProcessing::SlangPresetParser::GetStringValue(std::string_view key,
                                                                          std::string_view def) const
{
  const auto iter = m_options.find(key);
  return (iter != m_options.end()) ? iter->second : def;
}

inline bool PostProcessing::SlangPresetParser::GetBoolValue(std::string_view key, bool def) const
{
  const auto iter = m_options.find(key);
  return (iter != m_options.end()) ? StringUtil::FromChars<bool>(iter->second).value_or(def) : def;
}

inline int PostProcessing::SlangPresetParser::GetIntValue(std::string_view key, int def) const
{
  const auto iter = m_options.find(key);
  return (iter != m_options.end()) ? StringUtil::FromChars<s32>(iter->second).value_or(def) : def;
}

inline float PostProcessing::SlangPresetParser::GetFloatValue(std::string_view key, float def) const
{
  const auto iter = m_options.find(key);
  return (iter != m_options.end()) ? StringUtil::FromChars<float>(iter->second).value_or(def) : def;
}

inline u32 PostProcessing::SlangPresetParser::GetUIntValue(std::string_view key, u32 def) const
{
  const auto iter = m_options.find(key);
  return (iter != m_options.end()) ? StringUtil::FromChars<u32>(iter->second).value_or(def) : def;
}

inline bool PostProcessing::SlangPresetParser::ContainsIndexedValue(std::string_view key, u32 idx) const
{
  const TinyString real_key = TinyString::from_format("{}{}", key, idx);
  return ContainsValue(real_key);
}

inline std::string_view PostProcessing::SlangPresetParser::GetIndexedStringValue(std::string_view key, u32 idx,
                                                                                 std::string_view def) const
{
  const TinyString real_key = TinyString::from_format("{}{}", key, idx);
  return GetStringValue(real_key, def);
}

inline bool PostProcessing::SlangPresetParser::GetIndexedBoolValue(std::string_view key, u32 idx, bool def) const
{
  const TinyString real_key = TinyString::from_format("{}{}", key, idx);
  return GetBoolValue(real_key, def);
}

inline s32 PostProcessing::SlangPresetParser::GetIndexedIntValue(std::string_view key, u32 idx, int def) const
{
  const TinyString real_key = TinyString::from_format("{}{}", key, idx);
  return GetIntValue(real_key, def);
}

inline u32 PostProcessing::SlangPresetParser::GetIndexedUIntValue(std::string_view key, u32 idx, u32 def) const
{
  const TinyString real_key = TinyString::from_format("{}{}", key, idx);
  return GetUIntValue(real_key, def);
}

inline float PostProcessing::SlangPresetParser::GetIndexedFloatValue(std::string_view key, u32 idx, float def) const
{
  const TinyString real_key = TinyString::from_format("{}{}", key, idx);
  return GetFloatValue(real_key, def);
}

inline PostProcessing::SlangShaderPreprocessor::SlangShaderPreprocessor(Error* error) : m_error(error)
{
}

inline PostProcessing::SlangShaderPreprocessor::~SlangShaderPreprocessor() = default;

inline std::optional<std::pair<std::string, std::string>>
PostProcessing::SlangShaderPreprocessor::ReadShaderFile(std::string_view base_path, std::string_view path, Error* error)
{
  std::string real_path(path);
  if (!Path::IsAbsolute(path))
    real_path = Path::BuildRelativePath(base_path, path);
  Path::ToNativePath(&real_path);

  std::optional<std::pair<std::string, std::string>> result;
  std::optional<std::string> shader_contents = FileSystem::ReadFileToString(real_path.c_str(), error);
  if (shader_contents.has_value())
    result.emplace(std::move(real_path), std::move(shader_contents.value()));
  else
    Error::AddPrefixFmt(error, "Failed to read shader file '{}': ", path);

  return result;
}

template<typename... T>
inline void PostProcessing::SlangShaderPreprocessor::Write(fmt::format_string<T...> fmt, T&&... args)
{
  fmt::vformat_to(std::back_inserter(m_shader_code[m_current_stage]), fmt, fmt::make_format_args(args...));
}

template<typename... T>
inline void PostProcessing::SlangShaderPreprocessor::SetError(fmt::format_string<T...> fmt, T&&... args)
{
  std::string msg = fmt::format("{}:{}: {}", GetCurrentFilename(), m_current_line_number,
                                fmt::vformat(fmt, fmt::make_format_args(args...)));
  ERROR_LOG(msg);
  Error::SetString(m_error, std::move(msg));
}

inline std::string PostProcessing::SlangShaderPreprocessor::GetVertexShader() const
{
  return fmt::format("{}\n{}", m_shader_code[SlangShaderStage::Common], m_shader_code[SlangShaderStage::Vertex]);
}

inline std::string PostProcessing::SlangShaderPreprocessor::GetFragmentShader() const
{
  return fmt::format("{}\n{}", m_shader_code[SlangShaderStage::Common], m_shader_code[SlangShaderStage::Fragment]);
}

inline bool PostProcessing::SlangShaderPreprocessor::ParseFile(std::string_view base_path, std::string_view path)
{
  const std::optional<std::pair<std::string, std::string>> opt_file_contents = ReadShaderFile(base_path, path, m_error);
  if (!opt_file_contents.has_value())
    return false;

  std::string_view previous_path = m_path;
  std::string_view previous_contents = m_contents;
  size_t previous_offset = m_current_offset;
  size_t previous_line_offset = m_current_line_offset;
  u32 previous_line_number = m_current_line_number;
  auto previous_defined_stages = m_defined_stages;

  m_path = opt_file_contents->first;
  m_contents = opt_file_contents->second;
  m_current_offset = 0;
  m_current_line_offset = 0;
  m_current_line_number = 0;
  m_needs_line_reset = true;

  std::string_view line;
  while (GetLine(&line))
  {
    std::string_view clean_line = StringUtil::StripWhitespace(line);

    // TODO: Handle block comments, not just line comments
    std::string_view::size_type pos = clean_line.find("//");
    if (pos != std::string_view::npos)
      clean_line = StringUtil::StripWhitespace(clean_line.substr(0, pos));

    // Is this a preprocessor directive?
    if (clean_line.size() > 0 && clean_line[0] == '#')
    {
      const std::string_view line_without_hash = StringUtil::StripWhitespace(clean_line.substr(1));
      if (line_without_hash.starts_with("include "))
      {
        if (!HandleIncludeDirective(line_without_hash))
          return false;

        // Don't forward line
        continue;
      }
      else if (line_without_hash.starts_with("pragma "))
      {
        if (!HandlePragmaDirective(line_without_hash))
          return false;
        else
          continue;
      }
    }

    if (m_needs_version_directive)
    {
      // skip empty lines before version
      if (clean_line.empty())
        continue;

      const std::string_view line_without_hash = StringUtil::StripWhitespace(clean_line.substr(1));
      if (clean_line[0] != '#' || !line_without_hash.starts_with("version "))
      {
        SetError("First line of file must be a #version directive.");
        return false;
      }

      // forward through version directive without #line
      m_needs_version_directive = false;
    }
    else if (m_needs_line_reset)
    {
      // For debugging purposes, we want to keep track of line numbers and filenames
      m_needs_line_reset = false;
      Write("#line {} \"{}\"\n", m_current_line_number, GetCurrentFilename());
    }

    // Forward text through to shader code
    m_shader_code[m_current_stage] += line;
    m_shader_code[m_current_stage] += '\n';
  }

  m_defined_stages = previous_defined_stages;
  m_current_line_number = previous_line_number;
  m_current_line_offset = previous_line_offset;
  m_current_offset = previous_offset;
  m_contents = previous_contents;
  m_path = previous_path;
  return true;
}

inline bool PostProcessing::SlangShaderPreprocessor::HandleIncludeDirective(std::string_view line)
{
  if (m_include_depth >= MAX_SLANG_INCLUDE_DEPTH)
  {
    SetError("Maximum include depth exceeded");
    return false;
  }

  const std::string_view operand = StringUtil::StripWhitespace(line.substr(7));
  const char quote_char = operand.empty() ? 0 : operand[0];
  if (operand.size() < 2 || operand.front() != quote_char || operand.back() != quote_char ||
      operand.find(quote_char, 1) != (operand.size() - 1))
  {
    SetError("Malformed #include directive");
    return false;
  }

  if (!ParseFile(m_path, operand.substr(1, operand.size() - 2)))
    return false;

  m_needs_line_reset = true;
  return true;
}

inline bool PostProcessing::SlangShaderPreprocessor::HandlePragmaDirective(std::string_view line)
{
  llvm::SmallVector<std::string_view, 8> tokens;

  // Need to parse manually because strings can be quoted.
  std::string_view::size_type current_pos = 0;
  while (current_pos < line.size())
  {
    // Skip whitespace
    while (current_pos < line.size() && StringUtil::IsWhitespace(line[current_pos]))
      current_pos++;
    if (current_pos >= line.size())
      break;

    std::string_view::size_type start_pos = current_pos;
    if (line[current_pos] == '"')
    {
      // Quoted string
      current_pos++;
      start_pos = current_pos;
      while (current_pos < line.size() && line[current_pos] != '"')
        current_pos++;

      tokens.push_back(line.substr(start_pos, current_pos - start_pos));
      if (current_pos < line.size())
        current_pos++;
    }
    else
    {
      // Unquoted token
      while (current_pos < line.size() && !StringUtil::IsWhitespace(line[current_pos]))
        current_pos++;
      tokens.push_back(line.substr(start_pos, current_pos - start_pos));
    }
  }

  if (tokens.size() < 2)
  {
    SetError("Malformed #pragma directive");
    return false;
  }

  const std::span<const std::string_view> operands = std::span<const std::string_view>(tokens).subspan(2);
  if (tokens[1] == "stage")
  {
    return HandlePragmaStage(operands);
  }
  else if (tokens[1] == "name")
  {
    return HandlePragmaName(operands);
  }
  else if (tokens[1] == "parameter")
  {
    return HandlePragmaParameter(operands);
  }
  else if (tokens[1] == "format")
  {
    return HandlePragmaFormat(operands);
  }
  else
  {
    SetError("Unknown #pragma directive '{}'", tokens[1]);
    return false;
  }
}

inline bool PostProcessing::SlangShaderPreprocessor::HandlePragmaStage(std::span<const std::string_view> tokens)
{
  if (tokens.size() != 1)
  {
    SetError("Malformed #pragma stage directive");
    return false;
  }

  if (tokens[0] == "vertex")
  {
    m_current_stage = SlangShaderStage::Vertex;
  }
  else if (tokens[0] == "fragment")
  {
    m_current_stage = SlangShaderStage::Fragment;
  }
  else
  {
    SetError("Unknown shader stage '{}'", tokens[0]);
    return false;
  }

  m_needs_line_reset = true;
  return true;
}

inline bool PostProcessing::SlangShaderPreprocessor::HandlePragmaName(std::span<const std::string_view> tokens)
{
  if (!m_shader_name.empty())
  {
    SetError("Shader name already set");
    return false;
  }
  else if (tokens.size() != 1 || tokens[0].empty())
  {
    SetError("Malformed #pragma name directive");
    return false;
  }

  m_shader_name = tokens[0];
  return true;
}

inline bool PostProcessing::SlangShaderPreprocessor::HandlePragmaParameter(std::span<const std::string_view> tokens)
{
  // #pragma parameter IDENTIFIER "DESCRIPTION" INITIAL MINIMUM MAXIMUM [STEP]
  if (tokens.size() < 5 || tokens.size() > 6)
  {
    SetError("Malformed #pragma parameter directive");
    return false;
  }
  else if (std::ranges::any_of(m_options, [&tokens](const ShaderOption& opt) { return opt.name == tokens[0]; }))
  {
    WARNING_LOG("Duplicate shader option name '{}'", tokens[0]);
    return true;
  }

  ShaderOption option = {};
  option.type = ShaderOption::Type::Float;
  option.vector_size = 1;
  option.name = tokens[0];
  option.ui_name = tokens[1];
  option.default_value = ShaderOption::MakeFloatVector(StringUtil::FromChars<float>(tokens[2]).value_or(0.0f));
  option.min_value = ShaderOption::MakeFloatVector(StringUtil::FromChars<float>(tokens[3]).value_or(0.0f));
  option.max_value = ShaderOption::MakeFloatVector(StringUtil::FromChars<float>(tokens[4]).value_or(1.0f));
  option.step_value = ShaderOption::MakeFloatVector(
    StringUtil::FromChars<float>((tokens.size() > 5) ? tokens[5] : std::string_view()).value_or(DEFAULT_OPTION_STEP));
  option.value = option.default_value;

  // If it's a float with 0/1 range and a step of 1, it's probably a bool
  if (option.min_value[0].float_value == 0.0f && option.max_value[0].float_value == 1.0f &&
      option.step_value[0].float_value == 1.0f)
  {
    option.type = ShaderOption::Type::Bool;
    option.default_value[0].int_value = (option.default_value[0].float_value != 0.0f) ? 1 : 0;
    option.min_value[0].int_value = 0;
    option.max_value[0].int_value = 1;
    option.step_value[0].int_value = 1;
    option.value[0].int_value = option.default_value[0].int_value;
  }

  DEV_LOG("Adding shader option {} (default: {}, min: {}, max: {}, step: {})", option.name,
          option.default_value[0].float_value, option.min_value[0].float_value, option.max_value[0].float_value,
          option.step_value[0].float_value);

  m_options.push_back(std::move(option));
  return true;
}

bool PostProcessing::SlangShaderPreprocessor::HandlePragmaFormat(std::span<const std::string_view> tokens)
{
  if (tokens.size() != 1 || tokens[0].empty())
  {
    SetError("Malformed #pragma format directive");
    return false;
  }

  static constexpr const std::pair<std::string_view, GPUTexture::Format> format_map[] = {
    //{"A2B10G10R10_UINT_PACK32", GPUTexture::Format::RGB10A2UI},
    {"A2B10G10R10_UNORM_PACK32", GPUTexture::Format::RGB10A2},
    {"R16G16B16A16_SFLOAT", GPUTexture::Format::RGBA16F},
    //{"R16G16B16A16_SINT", GPUTexture::Format::RGBA16I},
    //{"R16G16B16A16_UINT", GPUTexture::Format::RGBA16U},
    {"R16G16_SFLOAT", GPUTexture::Format::RG16F},
    //{"R16G16_SINT", GPUTexture::Format::RG16I},
    //{"R16G16_UINT", GPUTexture::Format::RG16U},
    {"R16_SFLOAT", GPUTexture::Format::R16F},
    //{"R16_SINT", GPUTexture::Format::R16I},
    //{"R16_UINT", GPUTexture::Format::R16U},
    {"R32G32B32A32_SFLOAT", GPUTexture::Format::RGBA32F},
    //{"R32G32B32A32_SINT", GPUTexture::Format::RGBA32I},
    //{"R32G32B32A32_UINT", GPUTexture::Format::RGBA32U},
    {"R32G32_SFLOAT", GPUTexture::Format::RG32F},
    //{"R32G32_SINT", GPUTexture::Format::RG32I},
    //{"R32G32_UINT", GPUTexture::Format::RG32U},
    {"R32_SFLOAT", GPUTexture::Format::R32F},
    {"R32_SINT", GPUTexture::Format::R32I},
    {"R32_UINT", GPUTexture::Format::R32U},
    //{"R8G8B8A8_SINT", GPUTexture::Format::RGBA8I},
    {"R8G8B8A8_SRGB", GPUTexture::Format::SRGBA8},
    //{"R8G8B8A8_UINT", GPUTexture::Format::RGBA8U},
    {"R8G8B8A8_UNORM", GPUTexture::Format::RGBA8},
    //{"R8G8_SINT", GPUTexture::Format::RG8I},
    //{"R8G8_UINT", GPUTexture::Format::RG8U},
    {"R8G8_UNORM", GPUTexture::Format::RG8},
    //{"R8_SINT", GPUTexture::Format::R8I},
    //{"R8_UINT", GPUTexture::Format::R8U},
    {"R8_UNORM", GPUTexture::Format::R8},
  };

  static_assert(
    []() {
      for (size_t i = 1; i < std::size(format_map); i++)
      {
        if (format_map[i - 1].first >= format_map[i].first)
          return false;
      }
      return true;
    }(),
    "format_map is sorted");

  const std::string_view format_name = tokens[0];
  const auto iter = std::lower_bound(std::begin(format_map), std::end(format_map), format_name,
                                     [](const auto& it, const std::string_view& value) { return (it.first < value); });
  if (iter == std::end(format_map) || iter->first != format_name)
  {
    SetError("Unknown texture format '{}'", format_name);
    return false;
  }

  m_output_format = iter->second;
  return true;
}

inline bool PostProcessing::SlangShaderPreprocessor::GetLine(std::string_view* line)
{
  const size_t length = m_contents.length();
  if (m_current_offset == length)
  {
    m_current_line_offset = m_current_offset;
    return false;
  }

  size_t end_position = m_current_offset;
  for (; end_position < length; end_position++)
  {
    // ignore carriage returns
    if (m_contents[end_position] == '\r')
      continue;

    if (m_contents[end_position] == '\n')
      break;
  }

  m_current_line_number++;
  m_current_line_offset = m_current_offset;
  *line = m_contents.substr(m_current_offset, end_position - m_current_offset);
  m_current_offset = std::min(end_position + 1, length);
  return true;
}

inline std::string_view PostProcessing::SlangShaderPreprocessor::GetCurrentFilename() const
{
  return Path::GetFileName(m_path);
}

PostProcessing::SlangShader::SlangShader() = default;

PostProcessing::SlangShader::~SlangShader()
{
  for (Texture& tex : m_textures)
  {
    g_gpu_device->RecycleTexture(std::move(tex.texture));
    g_gpu_device->RecycleTexture(std::move(tex.feedback_texture));
  }
}

bool PostProcessing::SlangShader::LoadFromFile(std::string name, const char* path, Error* error)
{
  std::optional<std::string> code = FileSystem::ReadFileToString(path, error);
  if (!code.has_value() || code->empty())
    return false;

  return LoadFromString(std::move(name), path, code.value(), error);
}

bool PostProcessing::SlangShader::LoadFromString(std::string name, std::string_view path, std::string_view code,
                                                 Error* error)
{
  m_name = std::move(name);
  m_options.clear();

  return ParsePresetFile(path, code, error);
}

bool PostProcessing::SlangShader::WantsUnscaledInput() const
{
  return true;
}

bool PostProcessing::SlangShader::ParsePresetFile(std::string_view path, std::string_view code, Error* error)
{
  SlangPresetParser pp;
  if (!pp.Parse(path, code, error))
    return false;

  const u32 num_shaders = pp.GetUIntValue("shaders", 0);
  if (num_shaders == 0)
  {
    Error::SetStringView(error, "No shaders defined in preset");
    return false;
  }

  // Textures must be parsed first, since the passes may reference them.
  if (pp.ContainsValue("textures"))
  {
    if (!ParsePresetTextures(path, pp, error))
      return false;
  }

  // Parse passes in order.
  for (u32 i = 0; i < num_shaders; i++)
  {
    if (!ParsePresetPass(path, pp, i, (i == (num_shaders - 1)), error))
      return false;
  }

  // We defer reflection until pipeline compilation, so we don't need to do anything else here.
  return true;
}

bool PostProcessing::SlangShader::ParseScaleType(ScaleType* dst, std::string_view value, Error* error)
{
  if (StringUtil::EqualNoCase(value, "source"))
  {
    *dst = ScaleType::Source;
    return true;
  }

  if (StringUtil::EqualNoCase(value, "viewport"))
  {
    *dst = ScaleType::Viewport;
    return true;
  }

  if (StringUtil::EqualNoCase(value, "absolute"))
  {
    *dst = ScaleType::Absolute;
    return true;
  }

  if (StringUtil::EqualNoCase(value, "original"))
  {
    *dst = ScaleType::Original;
    return true;
  }

  Error::SetStringFmt(error, "Invalid scale type '{}'", value);
  return false;
}

GPUSampler::AddressMode PostProcessing::SlangShader::ParseWrapMode(std::string_view value)
{
  if (value == "repeat")
    return GPUSampler::AddressMode::Repeat;
  if (value == "clamp_to_edge")
    return GPUSampler::AddressMode::ClampToEdge;
  if (value == "clamp_to_border")
    return GPUSampler::AddressMode::ClampToBorder;
  if (value == "mirrored_repeat")
    return GPUSampler::AddressMode::MirrorRepeat;

  WARNING_LOG("Invalid wrap mode '{}', defaulting to border", value);
  return GPUSampler::AddressMode::ClampToBorder;
}

bool PostProcessing::SlangShader::ParsePresetTextures(std::string_view preset_path, const SlangPresetParser& parser,
                                                      Error* error)

{
  const std::string_view textures_str = parser.GetStringValue("textures", {});
  const std::vector<std::string_view> texture_names = StringUtil::SplitString(textures_str, ';');

  for (const std::string_view orig_name : texture_names)
  {
    const std::string_view name = StringUtil::StripWhitespace(orig_name);
    if (name.empty())
      continue;

    if (std::ranges::any_of(m_textures, [&name](const Texture& t) { return t.name == name; }))
    {
      Error::SetStringFmt(error, "Duplicate texture name '{}'", name);
      return false;
    }

    const std::string_view filename = parser.GetStringValue(name, {});
    if (filename.empty())
    {
      Error::SetStringFmt(error, "Texture '{}' has no path defined", name);
      return false;
    }

    std::string path =
      Path::IsAbsolute(filename) ? std::string(filename) : Path::BuildRelativePath(preset_path, filename);
    Path::ToNativePath(&path);
    if (!FileSystem::FileExists(path.c_str()))
    {
      Error::SetStringFmt(error, "Texture file '{}' does not exist", filename);
      return false;
    }

    const bool linear_filter = parser.GetBoolValue(TinyString::from_format("{}_linear", name), true);

    DEV_LOG("Adding LUT texture '{}' from file '{}' (linear filter: {})", name, path, linear_filter);

    Texture& tex = m_textures.emplace_back();
    tex.name = name;
    tex.path = std::move(path);
    tex.linear_filter = linear_filter;
  }

  return true;
}

bool PostProcessing::SlangShader::ParsePresetPass(std::string_view preset_path, const SlangPresetParser& parser,
                                                  u32 idx, bool is_final_pass, Error* error)
{
  const std::string_view shader_path = parser.GetIndexedStringValue("shader", idx, {});
  if (shader_path.empty())
  {
    Error::SetStringFmt(error, "Shader {} has no path defined", idx);
    return false;
  }

  SlangShaderPreprocessor preprocessor(error);
  if (!preprocessor.ParseFile(preset_path, shader_path))
    return false;

  Pass pass = {};
  pass.name = Path::GetFileTitle(shader_path);
  pass.vertex_shader_code = preprocessor.GetVertexShader();
  pass.fragment_shader_code = preprocessor.GetFragmentShader();

  // TODO: texture mipmap

  // FRAME COUNT MOD

  pass.frame_count_mod = parser.GetIndexedUIntValue("frame_count_mod", idx, 0);

  // MIPMAP INPUT - applies to previous pass

  if (parser.GetIndexedBoolValue("mipmap_input", idx, false))
  {
    // Just in case there are some bad files out there...
    if (idx > 0)
    {
      Assert(m_passes[idx - 1].output_texture_id >= 0);

      Texture& prev_rt = m_textures[m_passes[idx - 1].output_texture_id];
      if (!prev_rt.generate_mipmaps)
      {
        DEV_LOG("Enabling mipmap input for pass [{}]{} ({})", idx - 1, m_passes[idx - 1].name, prev_rt.name);
        prev_rt.generate_mipmaps = true;
      }
    }
    else
    {
      ERROR_LOG("Pass [{}]{} cannot use mipmap_input since it has no preceding pass", idx, pass.name);
    }
  }

  // OUTPUT FORMAT

  pass.output_format = preprocessor.GetOutputFormat();
  if (!preprocessor.HasOutputFormat())
  {
    // srgb > float apparently takes precedence
    if (parser.GetIndexedBoolValue("srgb_framebuffer", idx, false))
      pass.output_format = GPUTexture::Format::SRGBA8;
    else if (parser.GetIndexedBoolValue("float_framebuffer", idx, false))
      pass.output_format = GPUTexture::Format::RGBA16F;
  }

  // OUTPUT SCALE

  // Defaults to source scale.
  pass.output_scale[0] = {ScaleType::Source, 1.0f};
  pass.output_scale[1] = {ScaleType::Source, 1.0f};

  for (u32 i = 0; i < 2; i++)
  {
    const std::string_view type_key = i ? "scale_type_y" : "scale_type_x";
    const std::string_view value_key = i ? "scale_y" : "scale_x";
    if (!parser.ContainsIndexedValue(type_key, idx))
      continue;

    if (!ParseScaleType(&pass.output_scale[i].first, parser.GetIndexedStringValue(type_key, idx, {}), error))
      return false;

    pass.output_scale[i].second = parser.GetIndexedFloatValue(value_key, idx, 1.0f);
  }

  // non x/y takes precendence
  if (parser.ContainsIndexedValue("scale_type", idx))
  {
    if (!ParseScaleType(&pass.output_scale[0].first, parser.GetIndexedStringValue("scale_type", idx, {}), error))
      return false;
    pass.output_scale[0].second = parser.GetIndexedFloatValue("scale", idx, 1.0f);
    pass.output_scale[1] = pass.output_scale[0];
  }

  // SAMPLER

  const bool linear_sampler = parser.GetIndexedBoolValue("filter_linear", idx, true);
  pass.output_sampler_config = linear_sampler ? GPUSampler::GetLinearConfig() : GPUSampler::GetNearestConfig();

  // Default mode appears to be clamp-to-border?
  const GPUSampler::AddressMode mode = ParseWrapMode(parser.GetIndexedStringValue("wrap_mode", idx, "clamp_to_border"));
  pass.output_sampler_config.address_u = mode;
  pass.output_sampler_config.address_v = mode;
  pass.output_sampler_config.address_w = mode;

  // FRAMEBUFFER

  // Use the alias as the framebuffer name if specified, otherwise generate one.
  const std::string fb_name =
    preprocessor.HasShaderName() ? preprocessor.TakeShaderName() : fmt::format("__ds__pass_{}__", idx);
  if (std::ranges::any_of(m_textures, [&fb_name](const Texture& t) { return (t.name == fb_name); }))
  {
    Error::SetStringFmt(error, "Duplicate texture name '{}' when allocating pass framebuffer", fb_name);
    return false;
  }

  // Can bypass the framebuffer when the scale is not defined in the last pass, but allocate a texture anyway in case.
  pass.output_texture_id = static_cast<s32>(m_textures.size());
  Texture& tex = m_textures.emplace_back();
  tex.name = std::move(fb_name);
  tex.linear_filter = linear_sampler;

  DEV_LOG("Allocating output framebuffer for pass {}: '{}' ({})", idx, tex.name, pass.output_texture_id);

  // ALIASES

  if (const std::string_view alias = parser.GetIndexedStringValue("alias", idx, {}); !alias.empty())
  {
    if (std::ranges::any_of(m_aliases, [&alias](const auto& it) { return (it.first == alias); }))
    {
      Error::SetStringFmt(error, "Duplicate alias '{}' in pass {}", alias, idx);
      return false;
    }

    DEV_LOG("Alias {} to [{}]{}", alias, idx, pass.name);
    m_aliases.emplace_back(alias, idx);
  }

  // OPTIONS

  if (preprocessor.HasOptions())
  {
    if (m_options.empty())
    {
      m_options = preprocessor.TakeOptions();
    }
    else
    {
      // Need to merge options.
      for (ShaderOption& option : preprocessor.TakeOptions())
      {
        if (std::ranges::any_of(m_options, [&option](const ShaderOption& o) { return (o.name == option.name); }))
        {
          // Already defined.
          continue;
        }

        m_options.push_back(std::move(option));
      }
    }
  }

  m_passes.push_back(std::move(pass));
  return true;
}

static std::optional<DynamicHeapArray<u32>> CompileToSPV(shaderc_shader_kind stage, std::string_view code, Error* error)
{
  std::optional<DynamicHeapArray<u32>> ret;
  if (!dyn_libs::OpenShaderc(error))
    return ret;

  const bool generate_debug_info = (g_gpu_device && g_gpu_device->IsDebugDevice());
  const shaderc_compile_options_t options = dyn_libs::shaderc_compile_options_initialize();
  AssertMsg(options, "shaderc_compile_options_initialize() failed");

  dyn_libs::shaderc_compile_options_set_source_language(options, shaderc_source_language_glsl);
  dyn_libs::shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, 0);
  dyn_libs::shaderc_compile_options_set_generate_debug_info(options, generate_debug_info, false);
  dyn_libs::shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_zero);

  const shaderc_compilation_result_t result = dyn_libs::shaderc_compile_into_spv(
    dyn_libs::g_shaderc_compiler, code.data(), code.length(), stage, "source", "main", options);
  const shaderc_compilation_status status =
    result ? dyn_libs::shaderc_result_get_compilation_status(result) : shaderc_compilation_status_internal_error;
  if (status != shaderc_compilation_status_success)
  {
    const std::string_view errors(result ? dyn_libs::shaderc_result_get_error_message(result) : "null result object");
    Error::SetStringFmt(error, "Failed to compile shader to SPIR-V: {}\n{}",
                        dyn_libs::shaderc_compilation_status_to_string(status), errors);
    ERROR_LOG("Failed to compile shader to SPIR-V: {}\n{}", dyn_libs::shaderc_compilation_status_to_string(status),
              errors);
    GPUDevice::DumpBadShader(code, errors);
  }
  else
  {
    const size_t num_warnings = dyn_libs::shaderc_result_get_num_warnings(result);
    if (num_warnings > 0)
      WARNING_LOG("Shader compiled with warnings:\n{}", dyn_libs::shaderc_result_get_error_message(result));

    const size_t spirv_size = dyn_libs::shaderc_result_get_length(result);
    Assert(spirv_size > 0 && (spirv_size % sizeof(u32)) == 0);
    ret.emplace();
    ret->resize(spirv_size / sizeof(u32));
    std::memcpy(ret->data(), dyn_libs::shaderc_result_get_bytes(result), spirv_size);
  }

  dyn_libs::shaderc_result_release(result);
  dyn_libs::shaderc_compile_options_release(options);
  return ret;
}

bool PostProcessing::SlangShader::ReflectPass(Pass& pass, Error* error)
{
  std::optional<DynamicHeapArray<u32>> vs_spv = CompileToSPV(shaderc_vertex_shader, pass.vertex_shader_code, error);
  std::optional<DynamicHeapArray<u32>> fs_spv = CompileToSPV(shaderc_fragment_shader, pass.fragment_shader_code, error);
  if (!vs_spv.has_value() || !fs_spv.has_value())
    return false;

  pass.vertex_shader_spv = std::move(vs_spv.value());
  pass.fragment_shader_spv = std::move(fs_spv.value());

  if (!ReflectShader(pass, pass.vertex_shader_spv, GPUShaderStage::Vertex, error) ||
      !ReflectShader(pass, pass.fragment_shader_spv, GPUShaderStage::Fragment, error))
  {
    return false;
  }

  pass.is_reflected = true;
  return true;
}

bool PostProcessing::SlangShader::ReflectShader(Pass& pass, std::span<u32> spv, GPUShaderStage stage, Error* error)
{
  if (!dyn_libs::OpenSpirvCross(error))
    return false;

  spvc_context sctx;
  spvc_result sres;
  if ((sres = dyn_libs::spvc_context_create(&sctx)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_context_create() failed: {}", static_cast<int>(sres));
    return false;
  }

  const ScopedGuard sctx_guard = [&sctx]() { dyn_libs::spvc_context_destroy(sctx); };

  dyn_libs::spvc_context_set_error_callback(
    sctx,
    [](void* error, const char* errormsg) {
      ERROR_LOG("SPIRV-Cross reported an error: {}", errormsg);
      Error::SetStringView(static_cast<Error*>(error), errormsg);
    },
    error);

  spvc_parsed_ir sir;
  if ((sres = dyn_libs::spvc_context_parse_spirv(sctx, reinterpret_cast<const u32*>(spv.data()), spv.size(), &sir)) !=
      SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_context_parse_spirv() failed: {}", static_cast<int>(sres));
    return false;
  }

  spvc_compiler scompiler;
  if ((sres = dyn_libs::spvc_context_create_compiler(sctx, SPVC_BACKEND_NONE /*SPVC_BACKEND_GLSL*/, sir,
                                                     SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &scompiler)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_context_create_compiler() failed: {}", static_cast<int>(sres));
    return false;
  }

  spvc_compiler_options soptions;
  if ((sres = dyn_libs::spvc_compiler_create_compiler_options(scompiler, &soptions)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_compiler_create_compiler_options() failed: {}", static_cast<int>(sres));
    return false;
  }

  spvc_resources resources;
  if ((sres = dyn_libs::spvc_compiler_create_shader_resources(scompiler, &resources)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_compiler_create_shader_resources() failed: {}", static_cast<int>(sres));
    return false;
  }

  // Need to know if there's UBOs for mapping.
  const spvc_reflected_resource *ubos, *push_constants, *textures;
  size_t ubos_count, push_constants_count, textures_count;
  if ((sres = dyn_libs::spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &ubos,
                                                                  &ubos_count)) != SPVC_SUCCESS ||
      (sres = dyn_libs::spvc_resources_get_resource_list_for_type(
         resources, SPVC_RESOURCE_TYPE_PUSH_CONSTANT, &push_constants, &push_constants_count)) != SPVC_SUCCESS ||
      (sres = dyn_libs::spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE,
                                                                  &textures, &textures_count)) != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_resources_get_resource_list_for_type() failed: {}", static_cast<int>(sres));
    return false;
  }

  // Should only have one uniform buffer/push constant.
  if (ubos_count > 1)
  {
    Error::SetStringFmt(error, "{} uniform buffers found, only zero or one is supported.", ubos_count);
    return false;
  }
  else if (ubos_count > 0)
  {
    if (!ReflectPassUniforms(scompiler, ubos[0], pass, false, error))
      return false;
  }

  if (push_constants_count > 1)
  {
    Error::SetStringFmt(error, "{} push constant blocks found, only zero or one is supported.", push_constants_count);
    return false;
  }
  else if (push_constants_count > 0)
  {
    if (!ReflectPassUniforms(scompiler, push_constants[0], pass, true, error))
      return false;
  }

  // TEXTURES

  std::optional<SPIRVModule> mutable_spv;
  for (const spvc_reflected_resource& tex : std::span<const spvc_reflected_resource>(textures, textures_count))
  {
    if (stage != GPUShaderStage::Fragment)
    {
      Error::SetStringFmt(error, "Textures can only be used in fragment shaders, found in stage {}",
                          GPUShader::GetStageName(stage));
      return false;
    }

    const std::optional<TextureID> tex_id = FindTextureByName(tex.name, error);
    if (!tex_id.has_value())
      return false;

    const unsigned orig_descriptor_set =
      dyn_libs::spvc_compiler_get_decoration(scompiler, tex.id, SpvDecorationDescriptorSet);
    const unsigned orig_binding = dyn_libs::spvc_compiler_get_decoration(scompiler, tex.id, SpvDecorationBinding);
    if (orig_descriptor_set != 0)
    {
      Error::SetStringFmt(error, "Texture '{}' is in descriptor set {}, only set 0 is supported", tex.name,
                          orig_descriptor_set);
      return false;
    }

    if (orig_binding < 1)
    {
      Error::SetStringFmt(error, "Texture '{}' has invalid binding {}, must be 1 or higher", tex.name, orig_binding);
      return false;
    }

    // Remap to descriptor set #1, subtract UBO binding.
    if (!mutable_spv.has_value() && !(mutable_spv = SPIRVModule::Get(spv, error)).has_value())
      return false;

    const unsigned binding = orig_binding - 1;
    if (!mutable_spv->SetDecoration(tex.id, SpvDecorationDescriptorSet, 1, error) ||
        !mutable_spv->SetDecoration(tex.id, SpvDecorationBinding, binding, error))
    {
      Error::AddPrefixFmt(error, "Failed to remap texture '{}' to descriptor set 1, binding {}: ", tex.name, binding);
      return false;
    }

    if (binding >= GPUDevice::MAX_TEXTURE_SAMPLERS)
    {
      Error::SetStringFmt(error, "Texture '{}' has binding {} which exceeds the maximum of {}", tex.name, binding,
                          GPUDevice::MAX_TEXTURE_SAMPLERS - 1);
      return false;
    }

    if (const auto iter =
          std::ranges::find_if(pass.samplers, [&binding](const auto& it) { return (it.second == binding); });
        iter != pass.samplers.end())
    {
      if (iter->first != tex_id.value())
      {
        Error::SetStringFmt(error, "Binding {} is used for multiple textures ({} and {})", binding, tex_id.value(),
                            tex.name);
        return false;
      }

      // don't duplicate it
      DEV_LOG("Texture {} @ {} (already present)", GetTextureNameForID(tex_id.value()), binding);
      continue;
    }

    DEV_LOG("Texture {} @ {}", GetTextureNameForID(tex_id.value()), binding);
    pass.samplers.emplace_back(tex_id.value(), binding);
  }

  return true;
}

bool PostProcessing::SlangShader::ReflectPassUniforms(const spvc_compiler& scompiler,
                                                      const spvc_reflected_resource& resource, Pass& pass,
                                                      bool push_constant, Error* error)
{
  const spvc_type type_handle = dyn_libs::spvc_compiler_get_type_handle(scompiler, resource.base_type_id);
  if (!type_handle)
  {
    Error::SetStringFmt(error, "spvc_compiler_get_type_handle() failed for resource '{}'", resource.name);
    return false;
  }

  size_t struct_size = 0;
  if (const spvc_result sres = dyn_libs::spvc_compiler_get_declared_struct_size(scompiler, type_handle, &struct_size);
      sres != SPVC_SUCCESS)
  {
    Error::SetStringFmt(error, "spvc_compiler_get_declared_struct_size() failed for resource '{}': {}", resource.name,
                        static_cast<int>(sres));
    return false;
  }

  if (push_constant)
  {
    pass.push_constants_size =
      static_cast<u32>(Common::AlignUpPow2(struct_size, GPUDevice::BASE_UNIFORM_BUFFER_ALIGNMENT));
    DEV_LOG("Pass '{}' has push constants '{}' of size {} ({} aligned) bytes", pass.name, resource.name, struct_size,
            pass.push_constants_size);
    if (pass.push_constants_size > MAX_PUSH_CONSTANT_SIZE)
    {
      Error::SetStringFmt(error, "Push constant block '{}' is too large: {} bytes (max is {})", resource.name,
                          pass.push_constants_size, MAX_PUSH_CONSTANT_SIZE);
      return false;
    }
  }
  else
  {
    pass.uniforms_size = static_cast<u32>(Common::AlignUpPow2(struct_size, GPUDevice::BASE_UNIFORM_BUFFER_ALIGNMENT));
    DEV_LOG("Pass '{}' has uniform buffer '{}' of size {} ({} aligned) bytes", pass.name, resource.name, struct_size,
            pass.uniforms_size);
  }

  for (unsigned member_idx = 0;; member_idx++)
  {
    const char* member_name = dyn_libs::spvc_compiler_get_member_name(scompiler, resource.base_type_id, member_idx);
    if (!member_name || member_name[0] == '\0')
      break;

    const u32 offset =
      dyn_libs::spvc_compiler_get_member_decoration(scompiler, resource.base_type_id, member_idx, SpvDecorationOffset);

    size_t member_size = 0;
    if (const spvc_result sres =
          dyn_libs::spvc_compiler_get_declared_struct_member_size(scompiler, type_handle, member_idx, &member_size);
        sres != SPVC_SUCCESS)
    {
      Error::SetStringFmt(error, "spvc_compiler_get_declared_struct_member_size() failed for member '{}' of '{}': {}",
                          member_name, resource.name, static_cast<int>(sres));
      return false;
    }
    else if (member_size > std::numeric_limits<u16>::max())
    {
      // Shouldn't ever happen.
      ERROR_LOG("Member '{}' exceeded size limit: {} bytes", member_name, member_size);
      member_size = std::numeric_limits<u16>::max();
    }

    const std::optional<Uniform> uni_info =
      GetUniformInfo(member_name, push_constant, offset, static_cast<u16>(member_size), error);
    if (!uni_info.has_value())
      return false;

    // De-duplicate and check that the same offset isn't being used differently in VS vs FS.
    const auto iter = std::ranges::find_if(pass.uniforms, [&uni_info](const Uniform& u) {
      return (u.push_constant == uni_info->push_constant && u.offset == uni_info->offset);
    });
    if (iter != pass.uniforms.end())
    {
      if (iter->type != uni_info->type || iter->associated_texture != uni_info->associated_texture)
      {
        Error::SetStringFmt(error, "Conflicting definitions for uniform '{}' at offset {}", member_name, offset);
        return false;
      }

      // Already present, ignore.
      continue;
    }

    // TODO: check for overlap?

    pass.uniforms.push_back(uni_info.value());
  }

  return true;
}

bool PostProcessing::SlangShader::CompilePass(Pass& pass, Error* error)
{
  const std::unique_ptr<GPUShader> vs = g_gpu_device->CreateShader(
    GPUShaderStage::Vertex, GPUShaderLanguage::SPV,
    std::string_view(reinterpret_cast<const char*>(pass.vertex_shader_spv.data()), pass.vertex_shader_spv.size_bytes()),
    error);
  if (!vs)
    return false;
  const std::unique_ptr<GPUShader> fs =
    g_gpu_device->CreateShader(GPUShaderStage::Fragment, GPUShaderLanguage::SPV,
                               std::string_view(reinterpret_cast<const char*>(pass.fragment_shader_spv.data()),
                                                pass.fragment_shader_spv.size_bytes()),
                               error);
  if (!fs)
    return false;

  static constexpr std::array vertex_attributes = {
    GPUPipeline::VertexAttribute::Make(0, GPUPipeline::VertexAttribute::Semantic::TexCoord, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 4,
                                       offsetof(SlangShaderVertex, position)),
    GPUPipeline::VertexAttribute::Make(1, GPUPipeline::VertexAttribute::Semantic::TexCoord, 1,
                                       GPUPipeline::VertexAttribute::Type::Float, 2,
                                       offsetof(SlangShaderVertex, texcoord)),
  };

  GPUPipeline::GraphicsConfig plconfig;
  plconfig.layout = GPUPipeline::Layout::MultiTextureAndUBOAndPushConstants;
  plconfig.primitive = GPUPipeline::Primitive::TriangleStrips;
  plconfig.input_layout = {vertex_attributes, static_cast<u32>(sizeof(SlangShaderVertex))};
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  plconfig.vertex_shader = vs.get();
  plconfig.fragment_shader = fs.get();
  plconfig.geometry_shader = nullptr;
  plconfig.SetTargetFormats(pass.output_format);

  pass.pipeline = g_gpu_device->CreatePipeline(plconfig, error);
  if (!pass.pipeline)
    return false;

  return true;
}

bool PostProcessing::SlangShader::UploadLUTTextures(Error* error)
{
  for (Texture& tex : m_textures)
  {
    if (tex.path.empty() || tex.texture)
      continue;

    Assert(!tex.needs_feedback);

    Image image;
    if (!image.LoadFromFile(tex.path.c_str(), error) ||
        !(tex.texture = g_gpu_device->FetchAndUploadTextureImage(image, GPUTexture::Flags::None, error)))
    {
      Error::AddPrefixFmt(error, "Failed to load LUT texture from file '{}': ", Path::GetFileName(tex.path));
      return false;
    }

    // Use clamp to border for LUTs by default.
    GPUSampler::Config sampler_config =
      tex.linear_filter ? GPUSampler::GetLinearConfig() : GPUSampler::GetNearestConfig();
    sampler_config.address_u = GPUSampler::AddressMode::ClampToBorder;
    sampler_config.address_v = GPUSampler::AddressMode::ClampToBorder;
    if (!(tex.sampler = g_gpu_device->GetSampler(sampler_config, error)))
      return false;
  }

  return true;
}

std::optional<PostProcessing::SlangShader::TextureID>
PostProcessing::SlangShader::FindTextureByName(std::string_view name, Error* error)
{
  const auto enable_feedback_on_texture = [this](TextureID tid, Error* error) -> std::optional<TextureID> {
    if (!m_textures[tid].path.empty())
    {
      Error::SetStringFmt(error, "Cannot enable feedback on texture '{}' loaded from file", m_textures[tid].name);
      return std::nullopt;
    }

    DEV_LOG("Enabling feedback on texture [{}]{}", tid, m_textures[tid].name);
    m_textures[tid].needs_feedback = true;
    return (tid | TEXTURE_ID_FEEDBACK);
  };

  // Special cases.

  // Original or input.
  if (name == "Original")
    return TEXTURE_ID_ORIGINAL;

  // Previous pass or original.
  if (name == "Source")
    return TEXTURE_ID_SOURCE;

  // Previous pass N output.
  if (name.starts_with("OriginalHistory"))
  {
    const std::optional<u32> index = StringUtil::FromChars<u32>(name.substr(15));
    if (index.value_or(MAX_ORIGINAL_HISTORY_SIZE + 1) > MAX_ORIGINAL_HISTORY_SIZE)
    {
      Error::SetStringFmt(error, "Invalid OriginalHistory suffix '{}'", name);
      return std::nullopt;
    }

    const size_t required_size = index.value() + 2; // +1 for zero index, +1 for current frame
    if (required_size >= m_original_history_textures.size())
    {
      DEV_COLOR_LOG(StrongYellow, "Expanding original history texture array to size {}", required_size);
      m_original_history_textures.resize(required_size);
    }

    return (TEXTURE_ID_ORIGINAL_HISTORY_START + index.value());
  }

  // Previous pass N output.
  if (name.starts_with("PassOutput"))
  {
    const std::optional<u32> index = StringUtil::FromChars<u32>(name.substr(10));
    if (index.value_or(static_cast<u32>(m_passes.size())) >= m_passes.size())
    {
      Error::SetStringFmt(error, "Invalid PassOutput texture name '{}'", name);
      return std::nullopt;
    }

    return m_passes[index.value()].output_texture_id;
  }

  // Previous pass N previous output.
  if (name.starts_with("PassFeedback"))
  {
    const std::optional<u32> index = StringUtil::FromChars<u32>(name.substr(12));
    if (index.value_or(static_cast<u32>(m_passes.size())) >= m_passes.size())
    {
      Error::SetStringFmt(error, "Invalid PassFeedback texture name '{}'", name);
      return std::nullopt;
    }

    return enable_feedback_on_texture(m_passes[index.value()].output_texture_id, error);
  }

  // User texture by name.
  if (name.starts_with("User"))
  {
    const std::optional<u32> index = StringUtil::FromChars<u32>(name.substr(4));
    if (index.value_or(static_cast<u32>(m_textures.size())) >= m_textures.size() ||
        m_textures[index.value()].path.empty())
    {
      Error::SetStringFmt(error, "Invalid User texture index '{}'", name);
      return std::nullopt;
    }

    return static_cast<TextureID>(index.value());
  }

  // Feedback of named texture.
  if (name.ends_with("Feedback"))
  {
    const std::string_view base_name = name.substr(0, name.length() - 8);
    std::optional<TextureID> base_tid = FindTextureByName(base_name, error);
    if (!base_tid.has_value())
    {
      Error::AddSuffix(error, "for feedback");
      return std::nullopt;
    }

    return enable_feedback_on_texture(base_tid.value(), error);
  }

  const auto it =
    std::find_if(m_textures.begin(), m_textures.end(), [&name](const Texture& t) { return t.name == name; });
  if (it != m_textures.end())
    return static_cast<TextureID>(std::distance(m_textures.begin(), it));

  // check aliases.
  const auto alias_it = std::ranges::find_if(m_aliases, [&name](const auto& it) { return (it.first == name); });
  if (alias_it != m_aliases.end())
  {
    Assert(alias_it->second < m_passes.size());
    return m_passes[alias_it->second].output_texture_id;
  }

  Error::SetStringFmt(error, "Failed to resolve texture named '{}'", name);
  return std::nullopt;
}

std::optional<PostProcessing::SlangShader::Uniform> PostProcessing::SlangShader::GetUniformInfo(std::string_view name,
                                                                                                bool push_constant,
                                                                                                u32 offset, u16 size,
                                                                                                Error* error)
{
  struct BuiltinUniformEntry
  {
    const char* name;
    s32 type;
    u32 expected_size;
  };

  static constexpr const BuiltinUniformEntry builtins[] = {
    {"MVP", BuiltinUniform::MVP, 64},
    {"OutputSize", BuiltinUniform::OutputSize, 16},
    {"ViewportSize", BuiltinUniform::FinalViewportSize, 16},
    {"FrameCount", BuiltinUniform::FrameCount, 4},
    {"FrameDirection", BuiltinUniform::FrameDirection, 4},
  };

  for (const BuiltinUniformEntry& entry : builtins)
  {
    if (name == entry.name)
    {
      if (size != entry.expected_size)
        WARNING_LOG("Builtin uniform '{}' has incorrect size {}, expected {}", name, size, entry.expected_size);

      return Uniform{entry.type, 0, push_constant, size, offset};
    }
  }

  // Texture related.
  if (name.ends_with("Size"))
  {
    const std::string_view tex_name = name.substr(0, name.length() - 4);
    const std::optional<TextureID> tex_id = FindTextureByName(tex_name, error);
    if (!tex_id.has_value())
    {
      // Apparently we should just fill with zero in this case.
      WARNING_LOG("Texture '{}' not found for uniform '{}'", tex_name, name);
      return Uniform{BuiltinUniform::Zero, 0, push_constant, size, offset};
    }

    return Uniform{BuiltinUniform::TextureSize, tex_id.value(), push_constant, size, offset};
  }

  // Check options.
  for (size_t i = 0; i < m_options.size(); i++)
  {
    if (m_options[i].name == name)
    {
      if (size != 4)
        WARNING_LOG("Uniform option '{}' has incorrect size {}, expected 4", name, size);

      return Uniform{static_cast<s32>(i), 0, push_constant, size, offset};
    }
  }

  WARNING_LOG("Failed to resolve uniform named '{}'", name);
  return Uniform{BuiltinUniform::Zero, 0, push_constant, size, offset};
}

bool PostProcessing::SlangShader::CompilePipeline(GPUTexture::Format format, u32 width, u32 height, Error* error,
                                                  ProgressCallback* progress)
{
  // skip if format hasn't changed
  if (m_output_framebuffer_format == format)
    return true;

  for (Pass& pass : m_passes)
  {
    if (!pass.is_reflected && !ReflectPass(pass, error))
      return false;

    pass.pipeline.reset();
    if (!CompilePass(pass, error))
      return false;
  }

  // don't compile a blit pipeline if it's not needed
  m_output_blit_pipeline.reset();
  m_output_blit_pipeline = CreateBlitPipeline(format, error);
  if (!m_output_blit_pipeline)
    return false;

  if (!UploadLUTTextures(error))
    return false;

  m_output_framebuffer_format = format;
  return true;
}

std::unique_ptr<GPUPipeline> PostProcessing::SlangShader::CreateBlitPipeline(GPUTexture::Format format, Error* error)
{
  const RenderAPI rapi = g_gpu_device->GetRenderAPI();
  const ShaderGen shadergen(rapi, ShaderGen::GetShaderLanguageForAPI(rapi), false, false);

  const std::unique_ptr<GPUShader> vs = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                                   shadergen.GenerateScreenQuadVertexShader(), error);
  if (!vs)
    return {};
  const std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                                                   shadergen.GenerateCopyFragmentShader(false));
  if (!fs)
    return {};

  GPUPipeline::GraphicsConfig plconfig;
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
  plconfig.primitive = GPUPipeline::Primitive::TriangleStrips;
  plconfig.input_layout = {};
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  plconfig.vertex_shader = vs.get();
  plconfig.fragment_shader = fs.get();
  plconfig.geometry_shader = nullptr;
  plconfig.SetTargetFormats(format);

  return g_gpu_device->CreatePipeline(plconfig, error);
}

GPUDevice::PresentResult PostProcessing::SlangShader::Apply(GPUTexture* input_color, GPUTexture* input_depth,
                                                            GPUTexture* final_target, GSVector4i final_rect,
                                                            s32 orig_width, s32 orig_height, s32 native_width,
                                                            s32 native_height, u32 target_width, u32 target_height,
                                                            float time)
{
  const auto bind_final_target = [](GPUTexture* final_target, const GSVector4i& final_rect) {
    if (!final_target)
    {
      return g_gpu_device->BeginPresent(g_gpu_device->GetMainSwapChain());
    }
    else
    {
      // not drawing to the swap chain, so any area outside final_rect needs to be cleared
      if (final_target->GetSizeVec().eq(final_rect.rsize()))
      {
        g_gpu_device->InvalidateRenderTarget(final_target);
        g_gpu_device->SetRenderTarget(final_target, nullptr);
      }
      else
      {
        g_gpu_device->SetRenderTarget(final_target, nullptr);
        g_gpu_device->ClearRenderTarget(final_target, GPUDevice::DEFAULT_CLEAR_COLOR);
      }

      return GPUDevice::PresentResult::OK;
    }
  };

  GL_SCOPE_FMT("Slang Shader {}", m_name);

  // TODO: Extract out
  if (!m_original_history_textures.empty())
  {
    GL_INS_FMT("Updating original history texture");

    // Need to copy the input before drawing, because of swap chain
    Error error;
    if (!g_gpu_device->ResizeTexture(&m_original_history_textures[0], input_color->GetWidth(), input_color->GetHeight(),
                                     GPUTexture::Type::Texture, input_color->GetFormat(), GPUTexture::Flags::None,
                                     false, &error))
    {
      ERROR_LOG("Failed to resize original history texture: {}", error.GetDescription());
    }
    else
    {
      g_gpu_device->CopyTextureRegion(m_original_history_textures[0].get(), 0, 0, 0, 0, input_color, 0, 0, 0, 0,
                                      input_color->GetWidth(), input_color->GetHeight());
    }
  }

  m_frame_count++;

  // Vertices are computed and uploaded once.
  static constexpr std::array<std::array<SlangShaderVertex, 4>, 3> vertices = {{
    // position                    texcoord
    {{
      // D3D
      {{-1.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
      {{1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
      {{-1.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
      {{1.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
    }},
    {{
      // Vulkan
      {{-1.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
      {{1.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
      {{-1.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
      {{1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
    }},
    {{
      // OpenGL
      {{-1.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
      {{1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
      {{-1.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
      {{1.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
    }},
  }};
  const RenderAPI rapi = g_gpu_device->GetRenderAPI();
  const bool vbuf_index =
    (rapi == RenderAPI::Vulkan) ? 1 : ((rapi == RenderAPI::OpenGL || rapi == RenderAPI::OpenGLES) ? 2 : 0);
  u32 base_vertex;
  g_gpu_device->UploadVertexBuffer(vertices[vbuf_index].data(), sizeof(SlangShaderVertex),
                                   static_cast<u32>(vertices[0].size()), &base_vertex);

  GPUTexture* last_framebuffer = nullptr;
  for (size_t i = 0; i < m_passes.size(); i++)
  {
    const Pass& pass = m_passes[i];
    GL_SCOPE_FMT("Pass {}: {}", i, pass.name);

    // Sucks doing this twice, but we need to set the RT first (for DX11), and transition layouts (for VK).
    std::array<GPUTexture*, GPUDevice::MAX_TEXTURE_SAMPLERS> textures = {};
    std::array<GPUSampler*, GPUDevice::MAX_TEXTURE_SAMPLERS> samplers = {};
    for (const auto& [tex_id, bind_point] : pass.samplers)
    {
      GL_INS_FMT("Texture {}: ID {} [{}]", bind_point, tex_id, GetTextureNameForID(tex_id));
      std::tie(textures[bind_point], samplers[bind_point]) = GetTextureByID(pass, tex_id, input_color);
      if (textures[bind_point])
        textures[bind_point]->MakeReadyForSampling();
    }

    Texture& output_texture = m_textures[static_cast<size_t>(pass.output_texture_id)];
    GL_INS_FMT("Output Texture: {}", output_texture.name);

    // If there's no padding, we can write directly to the output framebuffer.
    // The shader might use gl_FragCoord, so we need (0, 0) to point to the space being shaded,
    // because pillarboxes aren't post-processed with slang.
    GPUTexture* const framebuffer = m_textures[static_cast<size_t>(pass.output_texture_id)].texture.get();
    if (i == (m_passes.size() - 1) && !output_texture.needs_feedback && framebuffer->GetWidth() == target_width &&
        framebuffer->GetHeight() == target_height)
    {
      // last pass, can write directly to final target
      GL_INS("Last pass writing directly to final target");

      last_framebuffer = final_target;
      if (const GPUDevice::PresentResult pres = bind_final_target(final_target, final_rect);
          pres != GPUDevice::PresentResult::OK)
      {
        return pres;
      }
    }
    else
    {
      last_framebuffer = framebuffer;
      g_gpu_device->InvalidateRenderTarget(framebuffer);
      g_gpu_device->SetRenderTarget(framebuffer, nullptr);
    }

    const GSVector2i output_size = framebuffer->GetSizeVec();
    g_gpu_device->SetViewportAndScissor(GSVector4i::loadh(output_size));
    g_gpu_device->SetPipeline(pass.pipeline.get());
    for (size_t j = 0; j < textures.size(); j++)
      g_gpu_device->SetTextureSampler(static_cast<u32>(j), textures[j], samplers[j]);

    alignas(VECTOR_ALIGNMENT) u8 push_constant_data[MAX_PUSH_CONSTANT_SIZE];
    BindPassUniforms(pass, push_constant_data, input_color, output_size, final_rect, orig_width, orig_height,
                     native_width, native_height, target_width, target_height, time);
    if (pass.push_constants_size > 0)
      g_gpu_device->DrawWithPushConstants(4, base_vertex, push_constant_data, pass.push_constants_size);
    else
      g_gpu_device->Draw(4, base_vertex);

    if (framebuffer->GetLevels() > 1)
    {
      GL_INS("Generating mipmaps for output texture");
      framebuffer->GenerateMipmaps();
    }

    if (output_texture.needs_feedback)
      std::swap(output_texture.feedback_texture, output_texture.texture);
  }

  // blit pass if required
  if (last_framebuffer != final_target)
  {
    GL_SCOPE_FMT("Blit {} to target: {}", last_framebuffer->GetSizeVec(), final_rect);

    last_framebuffer->MakeReadyForSampling();
    if (const GPUDevice::PresentResult pres = bind_final_target(final_target, final_rect);
        pres != GPUDevice::PresentResult::OK)
    {
      return pres;
    }

    g_gpu_device->SetViewportAndScissor(final_rect);
    g_gpu_device->SetPipeline(m_output_blit_pipeline.get());
    g_gpu_device->SetTextureSampler(0, last_framebuffer, g_gpu_device->GetNearestSampler());
    g_gpu_device->Draw(4, 0);
  }

  // TODO: get rid of the rotate, replace with head ptr instead
  if (!m_original_history_textures.empty())
  {
    std::rotate(m_original_history_textures.rbegin(), m_original_history_textures.rbegin() + 1,
                m_original_history_textures.rend());
  }

  return GPUDevice::PresentResult::OK;
}

bool PostProcessing::SlangShader::ResizeTargets(u32 source_width, u32 source_height, GPUTexture::Format target_format,
                                                u32 target_width, u32 target_height, u32 viewport_width,
                                                u32 viewport_height, Error* error)
{
  static constexpr auto apply_scale = [](ScaleType type, float val, u32 source_dim, u32 viewport_dim,
                                         u32 original_dim) {
    switch (type)
    {
      case ScaleType::Source:
        return static_cast<u32>(static_cast<float>(source_dim) * val);

      case ScaleType::Viewport:
        return static_cast<u32>(static_cast<float>(viewport_dim) * val);

      case ScaleType::Absolute:
        return static_cast<u32>(val);

        DefaultCaseIsUnreachable();
    }
  };

  u32 prev_width = source_width;
  u32 prev_height = source_height;

  for (Pass& pass : m_passes)
  {
    if (pass.output_texture_id < 0)
      continue;

    const u32 tex_width =
      apply_scale(pass.output_scale[0].first, pass.output_scale[0].second, prev_width, viewport_width, source_width);
    const u32 tex_height =
      apply_scale(pass.output_scale[1].first, pass.output_scale[1].second, prev_height, viewport_height, source_height);

    // Source for the next pass is the previous pass's output.
    prev_width = tex_width;
    prev_height = tex_height;

    Texture& tex = m_textures[static_cast<size_t>(pass.output_texture_id)];
    if (!tex.texture || tex.texture->GetWidth() != tex_width || tex.texture->GetHeight() != tex_height)
    {
      const u32 levels = tex.generate_mipmaps ? GPUTexture::GetFullMipmapCount(tex_width, tex_height) : 1;
      const GPUTexture::Flags flags = (levels > 1) ? GPUTexture::Flags::AllowGenerateMipmaps : GPUTexture::Flags::None;
      g_gpu_device->RecycleTexture(std::move(tex.texture));
      g_gpu_device->RecycleTexture(std::move(tex.feedback_texture));
      tex.texture = g_gpu_device->FetchTexture(tex_width, tex_height, 1, levels, 1, GPUTexture::Type::RenderTarget,
                                               pass.output_format, flags, nullptr, 0, error);
      if (!tex.texture)
        return false;

      if (tex.needs_feedback)
      {
        tex.feedback_texture =
          g_gpu_device->FetchTexture(tex_width, tex_height, 1, levels, 1, GPUTexture::Type::RenderTarget,
                                     pass.output_format, flags, nullptr, 0, error);
        if (!tex.feedback_texture)
        {
          g_gpu_device->RecycleTexture(std::move(tex.texture));
          return false;
        }

        // in case it's read this frame, pre-clear it
        g_gpu_device->ClearRenderTarget(tex.feedback_texture.get(), GPUDevice::DEFAULT_CLEAR_COLOR);
      }
    }

    if (!tex.sampler)
    {
      if (!(tex.sampler = g_gpu_device->GetSampler(pass.output_sampler_config, error)))
        return false;
    }
  }

  return true;
}

TinyString PostProcessing::SlangShader::GetTextureNameForID(TextureID id) const
{
  TinyString ret;
  if (id == TEXTURE_ID_ORIGINAL)
  {
    ret = "Original/Input Color Texture";
  }
  else if (id == TEXTURE_ID_SOURCE)
  {
    ret = "Source/Last Color Texture";
  }
  else if (id >= TEXTURE_ID_ORIGINAL_HISTORY_START)
  {
    ret.format("Original/Input Color History Texture {}", id - TEXTURE_ID_ORIGINAL_HISTORY_START);
  }
  else
  {
    const bool is_feedback = ((id & TEXTURE_ID_FEEDBACK) != 0);
    const size_t idx = static_cast<size_t>(id & ~TEXTURE_ID_FEEDBACK);
    if (idx >= m_textures.size())
      ret = "UNKNOWN";
    else
      ret.format("{}{}", m_textures[idx].name, is_feedback ? " (Feedback)" : "");
  }

  return ret;
}

std::tuple<GPUTexture*, GPUSampler*> PostProcessing::SlangShader::GetTextureByID(const Pass& pass, TextureID id,
                                                                                 GPUTexture* input_color) const
{
  if (id == TEXTURE_ID_SOURCE)
  {
    const size_t this_pass_idx = static_cast<size_t>(std::distance(&m_passes[0], &pass));
    if (this_pass_idx > 0)
    {
      // Not the first pass, return the last pass's output texture.
      id = m_passes[this_pass_idx - 1].output_texture_id;
    }
    else
    {
      // First pass, no last texture.
      id = TEXTURE_ID_ORIGINAL;
    }
  }

  if (id == TEXTURE_ID_ORIGINAL)
    return {input_color, g_gpu_device->GetNearestSampler()};

  if (id >= TEXTURE_ID_ORIGINAL_HISTORY_START)
  {
    const size_t history_idx = static_cast<size_t>(id - TEXTURE_ID_ORIGINAL_HISTORY_START);
    DebugAssert(history_idx < m_original_history_textures.size());
    return {m_original_history_textures[history_idx].get(), g_gpu_device->GetNearestSampler()};
  }

  const bool is_feedback = ((id & TEXTURE_ID_FEEDBACK) != 0);
  const size_t idx = static_cast<size_t>(id & ~TEXTURE_ID_FEEDBACK);
  if (idx >= m_textures.size())
    Panic("Unexpected texture ID");

  const Texture& tex = m_textures[idx];
  return {is_feedback ? tex.feedback_texture.get() : tex.texture.get(), tex.sampler};
}

void PostProcessing::SlangShader::BindPassUniforms(const Pass& pass, u8* const push_constant_data,
                                                   GPUTexture* input_color, GSVector2i output_size,
                                                   GSVector4i final_rect, s32 orig_width, s32 orig_height,
                                                   s32 native_width, s32 native_height, u32 target_width,
                                                   u32 target_height, float time)
{
  GL_INS_FMT("Uniform buffer: {} bytes", pass.uniforms_size);
  GL_INS_FMT("Push constants: {} bytes", pass.push_constants_size);

  u8* const ubo_data =
    (pass.uniforms_size > 0) ? static_cast<u8*>(g_gpu_device->MapUniformBuffer(pass.uniforms_size)) : nullptr;

  for (const Uniform& ui : pass.uniforms)
  {
    u8* const dst = (ui.push_constant ? push_constant_data : ubo_data) + ui.offset;
    if (ui.type < 0)
    {
      switch (ui.type)
      {
        case BuiltinUniform::MVP:
        {
          GSMatrix4x4::Identity().store(dst);
        }
        break;

        case BuiltinUniform::OutputSize:
        {
          const GSVector2 v = GSVector2(output_size);
          GSVector4::store<false>(dst, GSVector4::xyxy(v, GSVector2::cxpr(1.0f, 1.0f) / v));
        }
        break;

        case BuiltinUniform::FinalViewportSize:
        {
          const GSVector2 v = GSVector2(GSVector2i(target_width, target_height));
          GSVector4::store<false>(dst, GSVector4::xyxy(v, GSVector2::cxpr(1.0f, 1.0f) / v));
        }
        break;

        case BuiltinUniform::FrameCount:
        {
          const u32 val = (pass.frame_count_mod != 0) ? (m_frame_count % pass.frame_count_mod) : m_frame_count;
          std::memcpy(dst, &val, sizeof(u32));
        }
        break;

        case BuiltinUniform::FrameDirection:
        {
          const u32 val = 1;
          std::memcpy(dst, &val, sizeof(u32));
        }
        break;

        case BuiltinUniform::TextureSize:
        {
          const GPUTexture* texture = std::get<0>(GetTextureByID(pass, ui.associated_texture, input_color));
          DebugAssert(texture);

          const GSVector2 v = GSVector2(texture->GetSizeVec());
          GSVector4::store<false>(dst, GSVector4::xyxy(v, GSVector2::cxpr(1.0f, 1.0f) / v));
        }
        break;

        case BuiltinUniform::Zero:
        {
          if (ui.size == sizeof(float))
            std::memset(dst, 0, sizeof(float));
          else if (ui.size == sizeof(float) * 4)
            std::memset(dst, 0, sizeof(float) * 4);
          else
            std::memset(dst, 0, ui.size);
        }
        break;

          DefaultCaseIsUnreachable();
      }
    }
    else
    {
      DebugAssert(static_cast<u32>(ui.type) < m_options.size());

      const ShaderOption& option = m_options[static_cast<u32>(ui.type)];
      const float value = ((option.type == ShaderOption::Type::Bool) ? (option.value[0].int_value ? 1.0f : 0.0f) :
                                                                       option.value[0].float_value);
      std::memcpy(dst, &value, sizeof(value));
    }
  }

  if (pass.uniforms_size > 0)
    g_gpu_device->UnmapUniformBuffer(pass.uniforms_size);
}
