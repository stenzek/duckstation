// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "postprocessing_shader.h"

namespace PostProcessing {

class GLSLShader final : public Shader
{
public:
  GLSLShader();
  GLSLShader(std::string name, std::string code);
  ~GLSLShader();

  ALWAYS_INLINE const std::string& GetCode() const { return m_code; }

  bool IsValid() const override;

  bool LoadFromFile(std::string name, const char* filename, Error* error);
  bool LoadFromString(std::string name, std::string code, Error* error);

  bool ResizeOutput(GPUTexture::Format format, u32 width, u32 height) override;
  bool CompilePipeline(GPUTexture::Format format, u32 width, u32 height) override;
  bool Apply(GPUTexture* input, GPUTexture* final_target, s32 final_left, s32 final_top, s32 final_width,
             s32 final_height, s32 orig_width, s32 orig_height, u32 target_width, u32 target_height) override;

private:
  struct CommonUniforms
  {
    float src_rect[4];
    float src_size[2];
    float resolution[2];
    float rcp_resolution[2];
    float window_resolution[2];
    float rcp_window_resolution[2];
    float original_size[2];
    float padded_original_size[2];
    float time;
    float padding;
  };

  void LoadOptions();

  u32 GetUniformsSize() const;
  void FillUniformBuffer(void* buffer, u32 texture_width, s32 texture_height, s32 texture_view_x, s32 texture_view_y,
                         s32 texture_view_width, s32 texture_view_height, u32 window_width, u32 window_height,
                         s32 original_width, s32 original_height, float time) const;

  std::string m_code;

  std::unique_ptr<GPUPipeline> m_pipeline;
  std::unique_ptr<GPUSampler> m_sampler;
};

} // namespace PostProcessing