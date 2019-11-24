#pragma once
#include "gpu_hw.h"
#include "host_display.h"
#include <sstream>
#include <string>

class GPU_HW_ShaderGen
{
public:
  GPU_HW_ShaderGen(HostDisplay::RenderAPI render_api, u32 resolution_scale, bool true_color,
                   bool supports_dual_source_belnd);
  ~GPU_HW_ShaderGen();

  std::string GenerateBatchVertexShader(bool textured);
  std::string GenerateBatchFragmentShader(GPU_HW::BatchRenderMode transparency, GPU::TextureMode texture_mode,
                                          bool dithering);
  std::string GenerateBatchLineExpandGeometryShader();
  std::string GenerateScreenQuadVertexShader();
  std::string GenerateFillFragmentShader();
  std::string GenerateCopyFragmentShader();
  std::string GenerateDisplayFragmentShader(bool depth_24bit, bool interlaced);
  std::string GenerateVRAMReadFragmentShader();
  std::string GenerateVRAMWriteFragmentShader();

  HostDisplay::RenderAPI m_render_api;
  u32 m_resolution_scale;
  bool m_true_color;
  bool m_glsl;
  bool m_glsl_es;
  bool m_supports_dual_source_blend;

  std::string m_glsl_version_string;

private:
  void SetGLSLVersionString();
  void WriteHeader(std::stringstream& ss);
  void DeclareUniformBuffer(std::stringstream& ss, const std::initializer_list<const char*>& members);
  void DeclareTexture(std::stringstream& ss, const char* name, u32 index);
  void DeclareTextureBuffer(std::stringstream& ss, const char* name, u32 index, bool is_int, bool is_unsigned);
  void DeclareVertexEntryPoint(std::stringstream& ss, const std::initializer_list<const char*>& attributes,
                               u32 num_color_outputs, u32 num_texcoord_outputs,
                               const std::initializer_list<const char*>& additional_outputs,
                               bool declare_vertex_id = false);
  void DeclareFragmentEntryPoint(std::stringstream& ss, u32 num_color_inputs, u32 num_texcoord_inputs,
                                 const std::initializer_list<const char*>& additional_inputs,
                                 bool declare_fragcoord = false, bool dual_color_output = false);

  void WriteCommonFunctions(std::stringstream& ss);
  void WriteBatchUniformBuffer(std::stringstream& ss);
};
