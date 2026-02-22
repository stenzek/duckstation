// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "video_shadergen.h"

VideoShaderGen::VideoShaderGen(RenderAPI render_api, bool supports_dual_source_blend, bool supports_framebuffer_fetch)
  : ShaderGen(render_api, GetShaderLanguageForAPI(render_api), supports_dual_source_blend, supports_framebuffer_fetch)
{
}

VideoShaderGen::~VideoShaderGen() = default;

void VideoShaderGen::WriteDisplayUniformBuffer(std::stringstream& ss) const
{
  DeclareUniformBuffer(ss, {"float4 u_src_size", "float4 u_clamp_rect", "float4 u_params"}, true);

  ss << R"(
float2 ClampUV(float2 uv) {
  return clamp(uv, u_clamp_rect.xy, u_clamp_rect.zw);
})";
}

std::string VideoShaderGen::GenerateDisplayFragmentShader(bool clamp_uv, bool nearest) const
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

std::string VideoShaderGen::GenerateDisplaySharpBilinearFragmentShader() const
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

std::string VideoShaderGen::GenerateDisplayHybridBilinearFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0, false);

  // Based on
  // https://github.com/rsn8887/Sharp-Bilinear-Shaders/blob/master/Copy_To_RetroPie/shaders/sharp-bilinear-simple.glsl
  // and
  // https://30fps.net/pages/pixelart-scaling/
  DeclareFragmentEntryPoint(ss, 0, 1);
  ss << R"(
{
  float2 scale = u_params.xy;
  float2 region_range = u_params.zw;

  float2 texel = v_tex0 * u_src_size.xy;
  float2 texel_floored = floor(texel);
  float2 s = frac(texel);

  float f_x = s.x;

  float center_dist_y = s.y - 0.5;
  float f_y = (center_dist_y - clamp(center_dist_y, -region_range.y, region_range.y)) * scale.y + 0.5;

  float2 f = float2(f_x, f_y);
  float2 mod_texel = texel_floored + f;

  o_col0 = float4(SAMPLE_TEXTURE(samp0, ClampUV(mod_texel * u_src_size.zw)).rgb, 1.0f);
})";

  return std::move(ss).str();
}

std::string VideoShaderGen::GenerateDisplayLanczosFragmentShader() const
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

  float px = PI * x;
  float v = (float(KERNEL_SIZE) * sin(px) * sin(px / float(KERNEL_SIZE))) / (px * px);
  v = (x < 0.0001) ? 1.0 : v;
  v = (x > float(KERNEL_SIZE)) ? 0.0 : v;
  return v;
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

std::string VideoShaderGen::GenerateDeinterlaceWeaveFragmentShader() const
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

std::string VideoShaderGen::GenerateDeinterlaceBlendFragmentShader() const
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

std::string VideoShaderGen::GenerateFastMADReconstructFragmentShader() const
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

std::string VideoShaderGen::GenerateChromaSmoothingFragmentShader() const
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


std::string VideoShaderGen::GenerateGaussianBlurFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);

  // Push constants: blur direction scaled by texel size (x_dir/width, y_dir/height)
  DeclareUniformBuffer(ss, {"float2 u_blur_direction"}, true);
  DeclareTexture(ss, "samp0", 0);

  // https://lisyarus.github.io/blog/posts/blur-coefficients-generator.html
  ss << R"(
#define SAMPLE_COUNT 51

CONSTANT float OFFSETS[SAMPLE_COUNT] = BEGIN_ARRAY(float, SAMPLE_COUNT)
-49.48625474420898,
-47.48680987236723,
-45.487365028249265,
-43.48792020937041,
-41.48847541349859,
-39.489030638720266,
-37.48958588350915,
-35.4901411467948,
-33.4906964280263,
-31.491251727224242,
-29.491807045011395,
-27.49236238260802,
-25.492917741773358,
-23.49347312466684,
-21.494028533593767,
-19.494583970587684,
-17.495139436764795,
-15.495694931364218,
-13.496250450358671,
-11.496805984481735,
-9.4973615164671,
-7.4979170172278025,
-5.498472440614654,
-3.499027716274998,
-1.4995827399773034,
0.4998604030295424,
2.4993052677880936,
4.49875010249702,
6.49819474232353,
8.497639273161903,
10.497083752229047,
12.496528216381938,
14.495972688240492,
16.495417180667026,
18.49486170001805,
20.494306248481625,
22.493750825736388,
24.493195430109466,
26.492640059366707,
28.49208471123504,
30.491529383731546,
32.49097407535482,
34.49041878517991,
36.489863512887176,
38.48930825874717,
40.48875302357802,
42.48819780868647,
44.487642615801114,
46.48708744700325,
48.48653230465919,
50
END_ARRAY;

CONSTANT float WEIGHTS[SAMPLE_COUNT] = BEGIN_ARRAY(float, SAMPLE_COUNT)
0.007513423475429531,
0.008368117282429276,
0.009278720225566868,
0.010242803730923997,
0.011256933323611454,
0.012316628231973985,
0.01341634118491324,
0.014549460952054555,
0.015708339685614185,
0.01688434651082837,
0.018067948093349114,
0.019248816108009714,
0.020415960670453225,
0.021557887902735384,
0.022662778921221517,
0.023718686696996897,
0.024713746483065166,
0.025636394864714064,
0.026475592002049145,
0.027221041324064367,
0.027863400821996745,
0.028394480188328117,
0.028807418359571794,
0.029096836539455953,
0.0292589624882016,
0.029291557704336432,
0.029193981299093227,
0.02896784211118028,
0.02861608944472498,
0.028143312416344088,
0.027555648377203514,
0.026860643781139608,
0.026067088326352954,
0.025184826961982806,
0.024224554967243656,
0.023197601727336745,
0.022115709035765915,
0.020990809745219975,
0.01983481237649993,
0.018659396893463107,
0.017475826285540615,
0.016294777898156646,
0.015126197649723018,
0.013979179408723471,
0.012861870913287592,
0.01178140673479658,
0.01074386794996874,
0.00975426742164888,
0.008816558921066281,
0.00793366777146425,
0.003653437810178696
END_ARRAY;
)";

  DeclareFragmentEntryPoint(ss, 0, 1);
  ss << R"(
{
  float3 result = float3(0.0f, 0.0f, 0.0f);
  for (int i = 0; i < SAMPLE_COUNT; i++)
  {
    float2 offset = u_blur_direction * OFFSETS[i];
    float3 color = SAMPLE_TEXTURE(samp0, v_tex0 + offset).rgb;
    result += color * WEIGHTS[i];
  }

  o_col0 = float4(result, 1.0);
})";

  return std::move(ss).str();
}
