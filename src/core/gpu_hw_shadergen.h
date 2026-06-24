// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_hw.h"

#include "util/shadergen.h"

class GPU_HW_ShaderGen : public ShaderGen
{
public:
  struct BatchVertexShaderSelector
  {
    bool upscaled : 1;
    bool msaa : 1;
    bool per_sample_shading : 1;
    bool textured : 1;
    bool palette : 1;
    bool page_texture : 1;
    bool uv_limits : 1;
    bool force_round_texcoords : 1;
    bool pgxp_depth : 1;
    bool disable_color_perspective : 1;
  };

  struct BatchFragmentShaderSelector
  {
    GPU_HW::BatchRenderMode render_mode;
    GPUTransparencyMode transparency;
    GPU_HW::BatchTextureMode texture_mode;
    GPUTextureFilter texture_filtering;
    bool is_blended_texture_filtering;
    bool upscaled : 1;
    bool msaa : 1;
    bool per_sample_shading : 1;
    bool uv_limits : 1;
    bool force_round_texcoords : 1;
    bool modulation_crop : 1;
    bool true_color : 1;
    bool dithering : 1;
    bool scaled_dithering : 1;
    bool disable_color_perspective : 1;
    bool interlacing : 1;
    bool scaled_interlacing : 1;
    bool check_mask : 1;
    bool write_mask_as_depth : 1;
    bool use_rov : 1;
    bool use_rov_depth : 1;
    bool rov_depth_test : 1;
    bool rov_depth_write : 1;
  };

public:
  GPU_HW_ShaderGen(RenderAPI render_api, bool supports_dual_source_blend, bool supports_framebuffer_fetch);
  ~GPU_HW_ShaderGen();

  std::string GenerateScreenVertexShader() const;

  std::string GenerateBatchVertexShader(const BatchVertexShaderSelector sel) const;
  std::string GenerateBatchFragmentShader(const BatchFragmentShaderSelector sel) const;
  std::string GenerateWireframeGeometryShader() const;
  std::string GenerateWireframeFragmentShader() const;
  std::string GenerateVRAMReadFragmentShader(u32 resolution_scale, u32 multisamples) const;
  std::string GenerateVRAMWriteFragmentShader(bool use_buffer, bool use_ssbo, bool write_mask_as_depth,
                                              bool write_depth_as_rt) const;
  std::string GenerateVRAMCopyFragmentShader(bool write_mask_as_depth, bool write_depth_as_rt) const;
  std::string GenerateVRAMFillFragmentShader(bool wrapped, bool interlaced, bool write_mask_as_depth,
                                             bool write_depth_as_rt) const;
  std::string GenerateVRAMUpdateDepthFragmentShader(bool msaa) const;
  std::string GenerateVRAMCopyDepthFragmentShader(bool msaa) const;
  std::string GenerateVRAMClearDepthFragmentShader(bool write_depth_as_rt) const;
  std::string GenerateVRAMExtractFragmentShader(u32 resolution_scale, u32 multisamples, bool color_24bit,
                                                bool depth_buffer) const;
  std::string GenerateVRAMReplacementBlitFragmentShader() const;

  std::string GenerateAdaptiveDownsampleVertexShader() const;
  std::string GenerateAdaptiveDownsampleMipFragmentShader() const;
  std::string GenerateAdaptiveDownsampleBlurFragmentShader() const;
  std::string GenerateAdaptiveDownsampleCompositeFragmentShader() const;
  std::string GenerateBoxSampleDownsampleFragmentShader(u32 factor) const;

  std::string GenerateReplacementMergeFragmentShader(bool replacement, bool semitransparent,
                                                     bool bilinear_filter) const;

private:
  void WriteColorConversionFunctions(std::stringstream& ss) const;
  void WriteBatchUniformBuffer(std::stringstream& ss) const;
  void WriteBatchTextureFilter(std::stringstream& ss, GPUTextureFilter texture_filter) const;
  void WriteAdaptiveDownsampleUniformBuffer(std::stringstream& ss) const;
};
