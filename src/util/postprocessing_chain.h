// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "gpu_device.h"
#include "postprocessing_shader.h"

#include "common/timer.h"

#include <memory>
#include <string_view>
#include <vector>

class GPUSampler;
class GPUFramebuffer;
class GPUTexture;

class PostProcessingChain
{
public:
  PostProcessingChain();
  ~PostProcessingChain();

  static std::vector<std::string> GetAvailableShaderNames();

  ALWAYS_INLINE bool IsEmpty() const { return m_shaders.empty(); }
  ALWAYS_INLINE u32 GetStageCount() const { return static_cast<u32>(m_shaders.size()); }
  ALWAYS_INLINE const PostProcessingShader* GetShaderStage(u32 i) const { return m_shaders[i].get(); }
  ALWAYS_INLINE PostProcessingShader* GetShaderStage(u32 i) { return m_shaders[i].get(); }
  ALWAYS_INLINE GPUTexture* GetInputTexture() const { return m_input_texture.get(); }
  ALWAYS_INLINE GPUFramebuffer* GetInputFramebuffer() const { return m_input_framebuffer.get(); }

  void AddShader(std::unique_ptr<PostProcessingShader> shader);
  bool AddStage(const std::string_view& name);
  void RemoveStage(u32 index);
  void MoveStageUp(u32 index);
  void MoveStageDown(u32 index);
  void ClearStages();

  std::string GetConfigString() const;

  bool CreateFromString(const std::string_view& chain_config);

  bool CheckTargets(GPUTexture::Format target_format, u32 target_width, u32 target_height);

  bool Apply(GPUFramebuffer* final_target, s32 final_left, s32 final_top, s32 final_width, s32 final_height,
             s32 orig_width, s32 orig_height);

private:
  std::vector<std::unique_ptr<PostProcessingShader>> m_shaders;

  GPUTexture::Format m_target_format = GPUTexture::Format::Unknown;
  u32 m_target_width = 0;
  u32 m_target_height = 0;

  std::unique_ptr<GPUTexture> m_input_texture;
  std::unique_ptr<GPUFramebuffer> m_input_framebuffer;

  std::unique_ptr<GPUTexture> m_output_texture;
  std::unique_ptr<GPUFramebuffer> m_output_framebuffer;
};
