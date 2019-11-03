#pragma once
#include <sstream>
#include <string>
#include "gpu_hw.h"

class GPU_HW_ShaderGen
{
public:
  enum class Backend
  {
    OpenGL
  };

public:
  GPU_HW_ShaderGen(Backend backend, u32 resolution_scale, bool true_color);
  ~GPU_HW_ShaderGen();

  void Init(Backend backend, u32 resolution_scale, bool true_color);

  std::string GenerateBatchVertexShader(bool textured);
  std::string GenerateBatchFragmentShader(GPU_HW::BatchRenderMode transparency, GPU::TextureMode texture_mode, bool dithering);
  std::string GenerateScreenQuadVertexShader();
  std::string GenerateFillFragmentShader();
  std::string GenerateDisplayFragmentShader(bool depth_24bit, bool interlaced);
  std::string GenerateVRAMWriteFragmentShader();

  Backend m_backend;
  u32 m_resolution_scale;
  bool m_true_color;

private:
  void GenerateShaderHeader(std::stringstream& ss);
  void GenerateBatchUniformBuffer(std::stringstream& ss);
};
