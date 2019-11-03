#pragma once
#include "gpu_hw.h"
#include <sstream>
#include <string>

class GPU_HW_ShaderGen
{
public:
  enum class API
  {
    OpenGL,
    Direct3D
  };

public:
  GPU_HW_ShaderGen(API backend, u32 resolution_scale, bool true_color);
  ~GPU_HW_ShaderGen();

  void Init(API backend, u32 resolution_scale, bool true_color);

  std::string GenerateBatchVertexShader(bool textured);
  std::string GenerateBatchFragmentShader(GPU_HW::BatchRenderMode transparency, GPU::TextureMode texture_mode,
                                          bool dithering);
  std::string GenerateScreenQuadVertexShader();
  std::string GenerateFillFragmentShader();
  std::string GenerateDisplayFragmentShader(bool depth_24bit, bool interlaced);
  std::string GenerateVRAMWriteFragmentShader();

  API m_backend;
  u32 m_resolution_scale;
  bool m_true_color;
  bool m_glsl;

private:
  void WriteHeader(std::stringstream& ss);
  void DeclareUniformBuffer(std::stringstream& ss, const std::initializer_list<const char*>& members);
  void DeclareTexture(std::stringstream& ss, const char* name, u32 index);
  void DeclareTextureBuffer(std::stringstream& ss, const char* name, u32 index, bool is_int, bool is_unsigned);
  void DeclareVertexEntryPoint(std::stringstream& ss, const std::initializer_list<const char*>& attributes,
                               u32 num_color_outputs, u32 num_texcoord_outputs,
                               const std::initializer_list<const char*>& additional_outputs);
  void DeclareFragmentEntryPoint(std::stringstream& ss, u32 num_color_inputs, u32 num_texcoord_inputs,
                                 const std::initializer_list<const char*>& additional_inputs,
                                 bool declare_fragcoord = false, bool dual_color_output = false);

  void WriteCommonFunctions(std::stringstream& ss);
  void WriteBatchUniformBuffer(std::stringstream& ss);
};
