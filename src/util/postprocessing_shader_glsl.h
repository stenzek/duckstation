// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

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

  bool LoadFromFile(std::string name, const char* filename, Error* error);
  bool LoadFromString(std::string name, std::string code, Error* error);

  bool CompilePipeline(GPUTexture::Format format, u32 width, u32 height, Error* error,
                       ProgressCallback* progress) override;

  GPUDevice::PresentResult Apply(GPUTexture* input_color, GPUTexture* input_depth, GPUTexture* final_target,
                                 GSVector4i final_rect, s32 orig_width, s32 orig_height, s32 native_width,
                                 s32 native_height, u32 target_width, u32 target_height, float time) override;

private:
  struct CommonUniforms
  {
    float src_rect[4];
    float src_size[2];
    float window_size[2];
    float rcp_window_size[2];
    float viewport_size[2];
    float window_to_viewport_ratio[2];
    float internal_size[2];
    float internal_pixel_size[2];
    float norm_internal_pixel_size[2];
    float native_size[2];
    float native_pixel_size[2];
    float norm_native_pixel_size[2];
    float upscale_multiplier;
    float time;
  };

  void LoadOptions();

  u32 GetUniformsSize() const;
  void FillUniformBuffer(void* buffer, s32 viewport_x, s32 viewport_y, s32 viewport_width, s32 viewport_height,
                         u32 window_width, u32 window_height, s32 original_width, s32 original_height, s32 native_width,
                         s32 native_height, float time) const;

  std::string m_code;

  std::unique_ptr<GPUPipeline> m_pipeline;
  std::unique_ptr<GPUSampler> m_sampler;
  GPUTexture::Format m_output_format = GPUTexture::Format::Unknown;
};

} // namespace PostProcessing