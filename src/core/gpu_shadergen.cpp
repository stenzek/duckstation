// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_shadergen.h"

GPUShaderGen::GPUShaderGen(RenderAPI render_api, bool supports_dual_source_blend, bool supports_framebuffer_fetch)
  : ShaderGen(render_api, GetShaderLanguageForAPI(render_api), supports_dual_source_blend, supports_framebuffer_fetch)
{
}

GPUShaderGen::~GPUShaderGen() = default;

void GPUShaderGen::WriteDisplayUniformBuffer(std::stringstream& ss) const
{
  DeclareUniformBuffer(ss, {"float4 u_src_size", "float4 u_clamp_rect", "float4 u_params"}, true);

  ss << R"(
float2 ClampUV(float2 uv) {
  return clamp(uv, u_clamp_rect.xy, u_clamp_rect.zw);
})";
}

std::string GPUShaderGen::GenerateDisplayVertexShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareVertexEntryPoint(ss, {"float2 a_pos", "float2 a_tex0"}, 0, 1, {}, false, "", false, false, false);
  ss << R"(
{
  v_pos = float4(a_pos, 0.0f, 1.0f);
  v_tex0 = a_tex0;

  // NDC space Y flip in Vulkan.
  #if API_VULKAN
    v_pos.y = -v_pos.y;
  #endif
}
)";

  return std::move(ss).str();
}

std::string GPUShaderGen::GenerateDisplayFragmentShader(bool clamp_uv, bool nearest) const
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0);
  DeclareFragmentEntryPoint(ss, 0, 1);
  ss << "{\n";

  if (clamp_uv)
    ss << "  float2 uv = ClampUV(v_tex0);\n";
  else
    ss << "  float2 uv = v_tex0;\n";

  // Work around nearest sampling precision issues on AMD graphics cards by adding 1/128 to UVs.
  if (nearest)
    ss << "  o_col0 = float4(LOAD_TEXTURE(samp0, int2((uv * u_src_size.xy) + (1.0 / 128.0)), 0).rgb, 1.0f);\n";
  else
    ss << "  o_col0 = float4(SAMPLE_TEXTURE(samp0, ClampUV(v_tex0)).rgb, 1.0f);\n";

  ss << "}\n";

  return std::move(ss).str();
}

std::string GPUShaderGen::GenerateDisplaySharpBilinearFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0, false);

  // Based on
  // https://github.com/rsn8887/Sharp-Bilinear-Shaders/blob/master/Copy_To_RetroPie/shaders/sharp-bilinear-simple.glsl
  DeclareFragmentEntryPoint(ss, 0, 1);
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

  return std::move(ss).str();
}

std::string GPUShaderGen::GenerateDisplayLanczosFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0, false);

  ss << R"(
CONSTANT int KERNEL_SIZE = 3;
CONSTANT float PI = 3.14159265359;

float lanczos(float x)
 {
    x = abs(x);
    if (x < 0.0001)
      return 1.0;

    if (x > float(KERNEL_SIZE))
      return 0.0;

    float px = PI * x;
    return (float(KERNEL_SIZE) * sin(px) * sin(px / float(KERNEL_SIZE))) / (px * px);
}

)";

  DeclareFragmentEntryPoint(ss, 0, 1);
  ss << R"(
{
  float2 pixel = v_tex0 * u_params.xy;
  float2 src_pixel = pixel * u_params.zw;
  float2 src = floor(src_pixel - 0.5) + 0.5;
    
  float3 color = float3(0.0, 0.0, 0.0);
  float total_weight = 0.0;
    
  for (int i = -KERNEL_SIZE; i <= KERNEL_SIZE; i++)
  {
      for (int j = -KERNEL_SIZE; j <= KERNEL_SIZE; j++)
      {
          float2 offset = float2(int2(i, j));
          float2 sample_pos = (src + offset) * u_src_size.zw;
          float2 dxdy = src_pixel - (src + offset);
          float weight = lanczos(dxdy.x) * lanczos(dxdy.y);
            
          color += SAMPLE_TEXTURE_LEVEL(samp0, ClampUV(sample_pos), 0.0).rgb * weight;
          total_weight += weight;
      }
  }
    
  o_col0 = float4(color / total_weight, 1.0);
})";

  return std::move(ss).str();
}

std::string GPUShaderGen::GenerateDeinterlaceWeaveFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"uint2 u_src_offset", "uint u_render_field"}, true);
  DeclareTexture(ss, "samp0", 0, false);

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true);
  ss << R"(
{
  uint2 fcoord = uint2(v_pos.xy);
  if ((fcoord.y & 1u) != u_render_field)
    discard;

  uint2 tcoord = u_src_offset + uint2(fcoord.x, (fcoord.y / 2u));
  o_col0 = LOAD_TEXTURE(samp0, int2(tcoord), 0);
})";

  return std::move(ss).str();
}

std::string GPUShaderGen::GenerateDeinterlaceBlendFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareTexture(ss, "samp0", 0, false);
  DeclareTexture(ss, "samp1", 1, false);

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true);
  ss << R"(
{
  uint2 uv = uint2(v_pos.xy);
  float4 c0 = LOAD_TEXTURE(samp0, int2(uv), 0);
  float4 c1 = LOAD_TEXTURE(samp1, int2(uv), 0);
  o_col0 = (c0 + c1) * 0.5f;
}
)";

  return std::move(ss).str();
}

std::string GPUShaderGen::GenerateFastMADReconstructFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"uint u_current_field", "uint u_height"}, true);
  DeclareTexture(ss, "samp0", 0, false);
  DeclareTexture(ss, "samp1", 1, false);
  DeclareTexture(ss, "samp2", 2, false);
  DeclareTexture(ss, "samp3", 3, false);

  ss << R"(
CONSTANT float3 SENSITIVITY = float3(0.08f, 0.08f, 0.08f);
)";

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true);
  ss << R"(
{
  int2 uv = int2(int(v_pos.x), int(v_pos.y) >> 1);
  float3 cur = LOAD_TEXTURE(samp0, uv, 0).rgb;

  float3 hn = LOAD_TEXTURE(samp0, uv + int2(0, -1), 0).rgb;
  float3 cn = LOAD_TEXTURE(samp1, uv, 0).rgb;
  float3 ln = LOAD_TEXTURE(samp0, uv + int2(0, 1), 0).rgb;

  float3 ho = LOAD_TEXTURE(samp2, uv + int2(0, -1), 0).rgb;
  float3 co = LOAD_TEXTURE(samp3, uv, 0).rgb;
  float3 lo = LOAD_TEXTURE(samp2, uv + int2(0, 1), 0).rgb;

  float3 mh = abs(hn.rgb - ho.rgb) - SENSITIVITY;
  float3 mc = abs(cn.rgb - co.rgb) - SENSITIVITY;
  float3 ml = abs(ln.rgb - lo.rgb) - SENSITIVITY;
  float3 mmaxv = max(mh, max(mc, ml));
  float mmax = max(mmaxv.r, max(mmaxv.g, mmaxv.b));

  // Is pixel F [n][ x , y ] present in the Current Field f [n] ?
  uint row = uint(v_pos.y);
  if ((row & 1u) == u_current_field)
  {
    // Directly uses the pixel from the Current Field
    o_col0.rgb = cur;
  }
  else if (row > 0u && row < u_height && mmax > 0.0f)
  {
    // Reconstructs the missing pixel as the average of the same pixel from the line above and the
    // line below it in the Current Field.
    o_col0.rgb = (hn + ln) / 2.0;
  }
  else
  {
    // Reconstructs the missing pixel as the same pixel from the Previous Field.
    o_col0.rgb = cn;
  }
  o_col0.a = 1.0f;
}
)";

  return std::move(ss).str();
}

std::string GPUShaderGen::GenerateChromaSmoothingFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"uint2 u_sample_offset", "uint2 u_clamp_size"}, true);
  DeclareTexture(ss, "samp0", 0);

  ss << R"(
float3 RGBToYUV(float3 rgb)
{
  return float3(dot(rgb.rgb, float3(0.299f, 0.587f, 0.114f)),
                dot(rgb.rgb, float3(-0.14713f, -0.28886f, 0.436f)),
                dot(rgb.rgb, float3(0.615f, -0.51499f, -0.10001f)));
}

float3 YUVToRGB(float3 yuv)
{
  return float3(dot(yuv, float3(1.0f, 0.0f, 1.13983f)),
                dot(yuv, float3(1.0f, -0.39465f, -0.58060f)),
                dot(yuv, float3(1.0f, 2.03211f, 0.0f)));
}

float3 SampleVRAMAverage2x2(uint2 icoords)
{
  float3 value = LOAD_TEXTURE(samp0, int2(icoords), 0).rgb;
  value += LOAD_TEXTURE(samp0, int2(icoords + uint2(0, 1)), 0).rgb;
  value += LOAD_TEXTURE(samp0, int2(icoords + uint2(1, 0)), 0).rgb;
  value += LOAD_TEXTURE(samp0, int2(icoords + uint2(1, 1)), 0).rgb;
  return value * 0.25;
}
)";

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true);
  ss << R"(
{
  uint2 icoords = uint2(v_pos.xy) + u_sample_offset;
  int2 base = int2(icoords) - 1;
  uint2 low = uint2(max(base & ~1, int2(0, 0)));
  uint2 high = min(low + 2u, u_clamp_size);
  float2 coeff = vec2(base & 1) * 0.5 + 0.25;

  float3 p = LOAD_TEXTURE(samp0, int2(icoords), 0).rgb;
  float3 p00 = SampleVRAMAverage2x2(low);
  float3 p01 = SampleVRAMAverage2x2(uint2(low.x, high.y));
  float3 p10 = SampleVRAMAverage2x2(uint2(high.x, low.y));
  float3 p11 = SampleVRAMAverage2x2(high);

  float3 s = lerp(lerp(p00, p10, coeff.x),
                  lerp(p01, p11, coeff.x),
                  coeff.y);

  float y = RGBToYUV(p).x;
  float2 uv = RGBToYUV(s).yz;
  o_col0 = float4(YUVToRGB(float3(y, uv)), 1.0);
}
)";

  return std::move(ss).str();
}
