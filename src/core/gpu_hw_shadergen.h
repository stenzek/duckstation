#pragma once
#include "gpu_hw.h"
#include "host_display.h"
#include <sstream>
#include <string>

class GPU_HW_ShaderGen
{
public:
  GPU_HW_ShaderGen(HostDisplay::RenderAPI render_api, u32 resolution_scale, bool true_color, bool scaled_dithering,
                   bool texture_filtering, bool supports_dual_source_blend);
  ~GPU_HW_ShaderGen();

  static bool UseGLSLBindingLayout();

  std::string GenerateBatchVertexShader(bool textured);
  std::string GenerateBatchFragmentShader(GPU_HW::BatchRenderMode transparency, GPU::TextureMode texture_mode,
                                          bool dithering, bool interlacing);
  std::string GenerateBatchLineExpandGeometryShader();
  std::string GenerateScreenQuadVertexShader();
  std::string GenerateFillFragmentShader();
  std::string GenerateInterlacedFillFragmentShader();
  std::string GenerateCopyFragmentShader();
  std::string GenerateDisplayFragmentShader(bool depth_24bit, GPU_HW::InterlacedRenderMode interlace_mode);
  std::string GenerateVRAMReadFragmentShader();
  std::string GenerateVRAMWriteFragmentShader(bool use_ssbo);
  std::string GenerateVRAMCopyFragmentShader();
  std::string GenerateVRAMUpdateDepthFragmentShader();

private:
  ALWAYS_INLINE bool IsVulkan() const { return (m_render_api == HostDisplay::RenderAPI::Vulkan); }

  void SetGLSLVersionString();
  void WriteHeader(std::stringstream& ss);
  void DeclareUniformBuffer(std::stringstream& ss, const std::initializer_list<const char*>& members,
                            bool push_constant_on_vulkan);
  void DeclareTexture(std::stringstream& ss, const char* name, u32 index);
  void DeclareTextureBuffer(std::stringstream& ss, const char* name, u32 index, bool is_int, bool is_unsigned);
  void DeclareVertexEntryPoint(std::stringstream& ss, const std::initializer_list<const char*>& attributes,
                               u32 num_color_outputs, u32 num_texcoord_outputs,
                               const std::initializer_list<std::pair<const char*, const char*>>& additional_outputs,
                               bool declare_vertex_id = false);
  void DeclareFragmentEntryPoint(std::stringstream& ss, u32 num_color_inputs, u32 num_texcoord_inputs,
                                 const std::initializer_list<std::pair<const char*, const char*>>& additional_inputs,
                                 bool declare_fragcoord = false, u32 num_color_outputs = 1, bool depth_output = false);

  void WriteCommonFunctions(std::stringstream& ss);
  void WriteBatchUniformBuffer(std::stringstream& ss);

  HostDisplay::RenderAPI m_render_api;
  u32 m_resolution_scale;
  bool m_true_color;
  bool m_scaled_dithering;
  bool m_texture_filering;
  bool m_glsl;
  bool m_supports_dual_source_blend;
  bool m_use_glsl_interface_blocks;
  bool m_use_glsl_binding_layout;

  std::string m_glsl_version_string;
};
