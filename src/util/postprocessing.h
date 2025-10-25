// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_device.h"

#include "common/timer.h"

#include <array>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

class GPUPipeline;
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

  Type type;
  u32 vector_size;
  u32 buffer_size;
  u32 buffer_offset;
  ValueVector default_value;
  ValueVector min_value;
  ValueVector max_value;
  ValueVector step_value;
  ValueVector value;
  std::string name;
  std::string ui_name;
  std::string dependent_option;
  std::string category;
  std::string tooltip;
  std::string help_text;
  std::vector<std::string> choice_options;

  bool ShouldHide() const;

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
inline constexpr const char* DISPLAY_CHAIN_SECTION = "PostProcessing";
inline constexpr const char* INTERNAL_CHAIN_SECTION = "InternalPostProcessing";

bool IsEnabled(const SettingsInterface& si, const char* section);
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

class Chain final
{
public:
  explicit Chain(const char* section);
  ~Chain();

  ALWAYS_INLINE bool HasStages() const { return !m_stages.empty(); }
  ALWAYS_INLINE bool NeedsDepthBuffer() const { return m_needs_depth_buffer; }
  ALWAYS_INLINE bool WantsUnscaledInput() const { return m_wants_unscaled_input; }
  ALWAYS_INLINE GPUTexture* GetInputTexture() const { return m_input_texture.get(); }
  ALWAYS_INLINE GPUTexture* GetOutputTexture() const { return m_output_texture.get(); }

  /// Returns either the input or output texture, whichever isn't the destination after the final pass.
  GPUTexture* GetTextureUnusedAtEndOfChain() const;

  bool IsActive() const;

  void UpdateSettings(std::unique_lock<std::mutex>& settings_lock, const SettingsInterface& si);

  void LoadStages(std::unique_lock<std::mutex>& settings_lock, const SettingsInterface& si,
                  bool preload_swap_chain_size);

  /// Temporarily toggles post-processing on/off.
  void Toggle();

  bool CheckTargets(GPUTexture::Format source_format, u32 source_width, u32 source_height,
                    GPUTexture::Format target_format, u32 target_width, u32 target_height, u32 viewport_width,
                    u32 viewport_height, ProgressCallback* progress = nullptr);

  GPUDevice::PresentResult Apply(GPUTexture* input_color, GPUTexture* input_depth, GPUTexture* final_target,
                                 const GSVector4i final_rect, s32 orig_width, s32 orig_height, s32 native_width,
                                 s32 native_height);

private:
  void ClearStagesWithError(const Error& error);
  void DestroyTextures();

  const char* m_section;

  u32 m_source_width = 0;
  u32 m_source_height = 0;
  u32 m_target_width = 0;
  u32 m_target_height = 0;
  u32 m_viewport_width = 0;
  u32 m_viewport_height = 0;
  GPUTexture::Format m_source_format = GPUTexture::Format::Unknown;
  GPUTexture::Format m_target_format = GPUTexture::Format::Unknown;
  bool m_enabled = false;
  bool m_wants_depth_buffer = false;
  bool m_needs_depth_buffer = false;
  bool m_wants_unscaled_input = false;

  std::vector<std::unique_ptr<PostProcessing::Shader>> m_stages;
  std::unique_ptr<GPUTexture> m_input_texture;
  std::unique_ptr<GPUTexture> m_output_texture;

  static Timer::Value s_start_time;
};

// [display_name, filename]
std::vector<std::pair<std::string, std::string>> GetAvailableShaderNames();

}; // namespace PostProcessing
