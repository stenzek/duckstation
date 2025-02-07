// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/shadergen.h"

class GPUShaderGen : public ShaderGen
{
public:
  GPUShaderGen(RenderAPI render_api, bool supports_dual_source_blend, bool supports_framebuffer_fetch);
  ~GPUShaderGen();

  std::string GenerateDisplayVertexShader() const;
  std::string GenerateDisplayFragmentShader(bool clamp_uv, bool nearest) const;
  std::string GenerateDisplaySharpBilinearFragmentShader() const;
  std::string GenerateDisplayLanczosFragmentShader() const;

  std::string GenerateDeinterlaceWeaveFragmentShader() const;
  std::string GenerateDeinterlaceBlendFragmentShader() const;
  std::string GenerateFastMADReconstructFragmentShader() const;

  std::string GenerateChromaSmoothingFragmentShader() const;

private:
  void WriteDisplayUniformBuffer(std::stringstream& ss) const;
};
