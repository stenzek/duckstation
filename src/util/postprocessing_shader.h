// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/rectangle.h"
#include "common/timer.h"
#include "common/types.h"
#include "gpu_device.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

class GPUPipeline;
class GPUTexture;

class PostProcessingChain;

class PostProcessingShader
{
  friend PostProcessingChain;

public:
  struct Option
  {
    enum : u32
    {
      MAX_VECTOR_COMPONENTS = 4
    };

    enum class Type
    {
      Invalid,
      Bool,
      Int,
      Float
    };

    union Value
    {
      s32 int_value;
      float float_value;
    };
    static_assert(sizeof(Value) == sizeof(u32));

    using ValueVector = std::array<Value, MAX_VECTOR_COMPONENTS>;
    static_assert(sizeof(ValueVector) == sizeof(u32) * MAX_VECTOR_COMPONENTS);

    std::string name;
    std::string ui_name;
    std::string dependent_option;
    Type type;
    u32 vector_size;
    u32 buffer_size;
    u32 buffer_offset;
    ValueVector default_value;
    ValueVector min_value;
    ValueVector max_value;
    ValueVector step_value;
    ValueVector value;
  };

  PostProcessingShader();
  PostProcessingShader(std::string name);
  virtual ~PostProcessingShader();

  ALWAYS_INLINE const std::string& GetName() const { return m_name; }
  ALWAYS_INLINE const std::vector<Option>& GetOptions() const { return m_options; }
  ALWAYS_INLINE std::vector<Option>& GetOptions() { return m_options; }
  ALWAYS_INLINE bool HasOptions() const { return !m_options.empty(); }

  virtual bool IsValid() const = 0;

  const Option* GetOptionByName(const std::string_view& name) const;
  Option* GetOptionByName(const std::string_view& name);

  std::string GetConfigString() const;
  void SetConfigString(const std::string_view& str);

  virtual bool ResizeOutput(GPUTexture::Format format, u32 width, u32 height) = 0;

  virtual bool CompilePipeline(GPUTexture::Format format, u32 width, u32 height) = 0;

  virtual bool Apply(GPUTexture* input, GPUFramebuffer* final_target, s32 final_left, s32 final_top, s32 final_width,
                     s32 final_height, s32 orig_width, s32 orig_height, u32 target_width, u32 target_height) = 0;

protected:
  static void ParseKeyValue(const std::string_view& line, std::string_view* key, std::string_view* value);

  template<typename T>
  static u32 ParseVector(const std::string_view& line, PostProcessingShader::Option::ValueVector* values);

  std::string m_name;
  std::vector<Option> m_options;

  Common::Timer m_timer;
};
