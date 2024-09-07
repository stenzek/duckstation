// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_device.h"

#include <array>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace Common {
class Timer;
}

class GPUSampler;
class GPUTexture;

class Error;
class SettingsInterface;
class ProgressCallback;

namespace PostProcessing {
class Shader;

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

  static u32 ParseIntVector(std::string_view line, ValueVector* values);
  static u32 ParseFloatVector(std::string_view line, ValueVector* values);

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

namespace Config {
static constexpr const char* DISPLAY_CHAIN_SECTION = "PostProcessing";
static constexpr const char* INTERNAL_CHAIN_SECTION = "InternalPostProcessing";

u32 GetStageCount(const SettingsInterface& si, const char* section);
std::string GetStageShaderName(const SettingsInterface& si, const char* section, u32 index);
std::vector<ShaderOption> GetStageOptions(const SettingsInterface& si, const char* section, u32 index);
std::vector<ShaderOption> GetShaderOptions(const std::string& shader_name, Error* error);

bool AddStage(SettingsInterface& si, const char* section, const std::string& shader_name, Error* error);
void RemoveStage(SettingsInterface& si, const char* section, u32 index);
void MoveStageUp(SettingsInterface& si, const char* section, u32 index);
void MoveStageDown(SettingsInterface& si, const char* section, u32 index);
void SetStageOption(SettingsInterface& si, const char* section, u32 index, const ShaderOption& option);
void UnsetStageOption(SettingsInterface& si, const char* section, u32 index, const ShaderOption& option);
void ClearStages(SettingsInterface& si, const char* section);
} // namespace Config

class Chain
{
public:
  Chain(const char* section);
  ~Chain();

  ALWAYS_INLINE bool HasStages() const { return m_stages.empty(); }
  ALWAYS_INLINE bool NeedsDepthBuffer() const { return m_needs_depth_buffer; }
  ALWAYS_INLINE GPUTexture* GetInputTexture() const { return m_input_texture.get(); }
  ALWAYS_INLINE GPUTexture* GetOutputTexture() const { return m_output_texture.get(); }

  bool IsActive() const;
  bool IsInternalChain() const;

  void UpdateSettings(std::unique_lock<std::mutex>& settings_lock);

  void LoadStages();
  void ClearStages();
  void DestroyTextures();

  /// Temporarily toggles post-processing on/off.
  void Toggle();

  bool CheckTargets(GPUTexture::Format target_format, u32 target_width, u32 target_height,
                    ProgressCallback* progress = nullptr);

  GPUDevice::PresentResult Apply(GPUTexture* input_color, GPUTexture* input_depth, GPUTexture* final_target,
                                 const GSVector4i final_rect, s32 orig_width, s32 orig_height, s32 native_width,
                                 s32 native_height);

private:
  void ClearStagesWithError(const Error& error);

  const char* m_section;

  GPUTexture::Format m_target_format = GPUTexture::Format::Unknown;
  u32 m_target_width = 0;
  u32 m_target_height = 0;
  bool m_enabled = false;
  bool m_wants_depth_buffer = false;
  bool m_needs_depth_buffer = false;

  std::vector<std::unique_ptr<PostProcessing::Shader>> m_stages;
  std::unique_ptr<GPUTexture> m_input_texture;
  std::unique_ptr<GPUTexture> m_output_texture;
};

// [display_name, filename]
std::vector<std::pair<std::string, std::string>> GetAvailableShaderNames();

void Initialize();

/// Reloads configuration.
void UpdateSettings();

/// Reloads post processing shaders with the current configuration.
bool ReloadShaders();

void Shutdown();

GPUSampler* GetSampler(const GPUSampler::Config& config);
GPUTexture* GetDummyTexture();

const Common::Timer& GetTimer();

extern Chain DisplayChain;
extern Chain InternalChain;

}; // namespace PostProcessing
