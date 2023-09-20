// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "gpu_shadergen.h"

GPUShaderGen::GPUShaderGen(RenderAPI render_api, bool supports_dual_source_blend, bool supports_framebuffer_fetch)
  : ShaderGen(render_api, supports_dual_source_blend, supports_framebuffer_fetch)
{
}

GPUShaderGen::~GPUShaderGen() = default;

void GPUShaderGen::WriteDisplayUniformBuffer(std::stringstream& ss)
{
  DeclareUniformBuffer(ss, {"float4 u_src_rect", "float4 u_src_size", "float4 u_clamp_rect", "float4 u_params"}, true);

  ss << R"(
float2 ClampUV(float2 uv) {
  return clamp(uv, u_clamp_rect.xy, u_clamp_rect.zw);
})";
}

std::string GPUShaderGen::GenerateDisplayVertexShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareVertexEntryPoint(ss, {}, 0, 1, {}, true);
  ss << R"(
{
  float2 pos = float2(float((v_id << 1) & 2u), float(v_id & 2u));
  v_tex0 = u_src_rect.xy + pos * u_src_rect.zw;
  v_pos = float4(pos * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  #if API_VULKAN
    v_pos.y = -v_pos.y;
  #endif
}
)";

  return ss.str();
}

std::string GPUShaderGen::GenerateDisplayFragmentShader(bool clamp_uv)
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, false, 1);
  if (clamp_uv)
    ss << "{\n  o_col0 = float4(SAMPLE_TEXTURE(samp0, ClampUV(v_tex0)).rgb, 1.0f);\n }";
  else
    ss << "{\n  o_col0 = float4(SAMPLE_TEXTURE(samp0, v_tex0).rgb, 1.0f);\n }";

  return ss.str();
}

std::string GPUShaderGen::GenerateDisplaySharpBilinearFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0, false);

  // Based on
  // https://github.com/rsn8887/Sharp-Bilinear-Shaders/blob/master/Copy_To_RetroPie/shaders/sharp-bilinear-simple.glsl
  DeclareFragmentEntryPoint(ss, 0, 1, {}, false, 1, false, false, false, false);
  ss << R"(
{
  float2 scale = u_params.xy;
  float2 region_range = u_params.zw;

  float2 texel = v_tex0 * u_src_size.xy;
  float2 texel_floored = floor(texel);
  float2 s = frac(texel);

  float2 center_dist = s - 0.5;
  float2 f = (center_dist - clamp(center_dist, -region_range, region_range)) * scale + 0.5;
  float2 mod_texel = texel_floored + f;

  o_col0 = float4(SAMPLE_TEXTURE(samp0, ClampUV(mod_texel * u_src_size.zw)).rgb, 1.0f);
})";

  return ss.str();
}
