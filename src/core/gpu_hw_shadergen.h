#pragma once
#include "gpu_hw.h"
#include "shadergen.h"

class GPU_HW_ShaderGen : public ShaderGen
{
public:
  GPU_HW_ShaderGen(HostDisplay::RenderAPI render_api, u32 resolution_scale, u32 multisamples, bool per_sample_shading,
                   bool true_color, bool scaled_dithering, GPUTextureFilter texture_filtering, bool uv_limits,
                   bool pgxp_depth, bool supports_dual_source_blend);
  ~GPU_HW_ShaderGen();

  std::string GenerateBatchVertexShader(bool textured);
  std::string GenerateBatchFragmentShader(GPU_HW::BatchRenderMode transparency, GPUTextureMode texture_mode,
                                          bool dithering, bool interlacing);
  std::string GenerateDisplayFragmentShader(bool depth_24bit, GPU_HW::InterlacedRenderMode interlace_mode,
                                            bool smooth_chroma);
  std::string GenerateVRAMReadFragmentShader();
  std::string GenerateVRAMWriteFragmentShader(bool use_ssbo);
  std::string GenerateVRAMCopyFragmentShader();
  std::string GenerateVRAMFillFragmentShader(bool wrapped, bool interlaced);
  std::string GenerateVRAMUpdateDepthFragmentShader();

  std::string GenerateAdaptiveDownsampleMipFragmentShader(bool first_pass);
  std::string GenerateAdaptiveDownsampleBlurFragmentShader();
  std::string GenerateAdaptiveDownsampleCompositeFragmentShader();
  std::string GenerateBoxSampleDownsampleFragmentShader();

private:
  ALWAYS_INLINE bool UsingMSAA() const { return m_multisamples > 1; }
  ALWAYS_INLINE bool UsingPerSampleShading() const { return m_multisamples > 1 && m_per_sample_shading; }

  void WriteCommonFunctions(std::stringstream& ss);
  void WriteBatchUniformBuffer(std::stringstream& ss);
  void WriteBatchTextureFilter(std::stringstream& ss, GPUTextureFilter texture_filter);

  u32 m_resolution_scale;
  u32 m_multisamples;
  bool m_per_sample_shading;
  bool m_true_color;
  bool m_scaled_dithering;
  GPUTextureFilter m_texture_filter;
  bool m_uv_limits;
  bool m_pgxp_depth;
};
