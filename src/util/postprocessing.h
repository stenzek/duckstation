// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_device.h"

#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace Common {
class Timer;
}

class GPUSampler;
class GPUTexture;

class Error;
class SettingsInterface;

namespace PostProcessing {
struct ShaderOption
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
  std::string category;
  std::string tooltip;
  Type type;
  u32 vector_size;
  u32 buffer_size;
  u32 buffer_offset;
  ValueVector default_value;
  ValueVector min_value;
  ValueVector max_value;
  ValueVector step_value;
  ValueVector value;
  std::vector<std::string> choice_options;

  static u32 ParseIntVector(const std::string_view& line, ValueVector* values);
  static u32 ParseFloatVector(const std::string_view& line, ValueVector* values);

  static constexpr ValueVector MakeIntVector(s32 x, s32 y = 0, s32 z = 0, s32 w = 0)
  {
    ValueVector ret = {};
    ret[0].int_value = x;
    ret[1].int_value = y;
    ret[2].int_value = z;
    ret[3].int_value = w;
    return ret;
  }

  static constexpr ValueVector MakeFloatVector(float x, float y = 0, float z = 0, float w = 0)
  {
    ValueVector ret = {};
    ret[0].float_value = x;
    ret[1].float_value = y;
    ret[2].float_value = z;
    ret[3].float_value = w;
    return ret;
  }
};

// [display_name, filename]
std::vector<std::pair<std::string, std::string>> GetAvailableShaderNames();

namespace Config {
u32 GetStageCount(const SettingsInterface& si);
std::string GetStageShaderName(const SettingsInterface& si, u32 index);
std::vector<ShaderOption> GetStageOptions(const SettingsInterface& si, u32 index);

bool AddStage(SettingsInterface& si, const std::string& shader_name, Error* error);
void RemoveStage(SettingsInterface& si, u32 index);
void MoveStageUp(SettingsInterface& si, u32 index);
void MoveStageDown(SettingsInterface& si, u32 index);
void SetStageOption(SettingsInterface& si, u32 index, const ShaderOption& option);
void UnsetStageOption(SettingsInterface& si, u32 index, const ShaderOption& option);
void ClearStages(SettingsInterface& si);
} // namespace Config

bool IsActive();
bool IsEnabled();
void SetEnabled(bool enabled);

void Initialize();

/// Reloads configuration.
void UpdateSettings();

/// Temporarily toggles post-processing on/off.
void Toggle();

/// Reloads post processing shaders with the current configuration.
bool ReloadShaders();

void Shutdown();

GPUTexture* GetInputTexture();
const Common::Timer& GetTimer();

bool CheckTargets(GPUTexture::Format target_format, u32 target_width, u32 target_height);

bool Apply(GPUTexture* final_target, s32 final_left, s32 final_top, s32 final_width, s32 final_height, s32 orig_width,
           s32 orig_height);

GPUSampler* GetSampler(const GPUSampler::Config& config);
GPUTexture* GetDummyTexture();

}; // namespace PostProcessing
