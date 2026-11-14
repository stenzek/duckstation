// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0
//
// NOTE: Some parts of this file have more permissive licenses. They are marked appropriately.
//

#include "gpu_hw_shadergen.h"
#include "gpu_types.h"

#include "common/assert.h"

// For HW batch draws, Vulkan layout:
// - Set 0: UBO
// - Set 1: VRAM read texture/page texture
// - Set 2: Texture buffer
// - Set 3: Feedback loop/ROV
// For other renderers

GPU_HW_ShaderGen::GPU_HW_ShaderGen(RenderAPI render_api, bool supports_dual_source_blend,
                                   bool supports_framebuffer_fetch)
  : ShaderGen(render_api, GetShaderLanguageForAPI(render_api), supports_dual_source_blend, supports_framebuffer_fetch)
{
}

GPU_HW_ShaderGen::~GPU_HW_ShaderGen() = default;

void GPU_HW_ShaderGen::DeclareTextureBuffer(std::stringstream& ss, const char* name, bool is_int,
                                            bool is_unsigned) const
{
  if (m_glsl)
  {
    if (IsMetal()) // metal uses slot 1 in the texture descriptor state, TODO clean this up and make consistent
      ss << "layout(set = 1, binding = 0) ";
    if (m_spirv)
      ss << "layout(set = 2, binding = 0) ";
    else if (m_use_glsl_binding_layout)
      ss << "layout(binding = 1) ";

    ss << "uniform " << (is_int ? (is_unsigned ? "u" : "i") : "") << "samplerBuffer " << name << ";\n";
  }
  else
  {
    ss << "Buffer<" << (is_int ? (is_unsigned ? "uint4" : "int4") : "float4") << "> " << name << " : register(t1);\n";
  }
}

void GPU_HW_ShaderGen::DeclareImage(std::stringstream& ss, const char* name, u32 index, const char* hlsl_format,
                                    const char* glsl_format) const
{
  if (m_glsl)
  {
    if (m_spirv)
      ss << "layout(set = 3, binding = " << index;
    else
      ss << "layout(binding = " << index;

    ss << ", " << glsl_format << ") uniform restrict coherent image2D " << name << ";\n";
  }
  else
  {
    ss << "RasterizerOrderedTexture2D<" << hlsl_format << "> " << name << " : register(u" << index << ");\n";
  }
}

void GPU_HW_ShaderGen::WriteColorConversionFunctions(std::stringstream& ss) const
{
  ss << R"(
uint RGBA8ToRGBA5551(float4 v)
{
  uint r = uint(roundEven(v.r * 31.0));
  uint g = uint(roundEven(v.g * 31.0));
  uint b = uint(roundEven(v.b * 31.0));
  uint a = (v.a != 0.0) ? 1u : 0u;
  return (r) | (g << 5) | (b << 10) | (a << 15);
}

float4 RGBA5551ToRGBA8(uint v)
{
  uint r = (v & 31u);
  uint g = ((v >> 5) & 31u);
  uint b = ((v >> 10) & 31u);
  uint a = ((v >> 15) & 1u);

  return float4(float(r) / 31.0, float(g) / 31.0, float(b) / 31.0, float(a));
}
)";
}

void GPU_HW_ShaderGen::WriteBatchUniformBuffer(std::stringstream& ss) const
{
  DeclareUniformBuffer(ss,
                       {"uint2 u_texture_window_and", "uint2 u_texture_window_or", "float u_src_alpha_factor",
                        "float u_dst_alpha_factor", "uint u_interlaced_displayed_field",
                        "bool u_set_mask_while_drawing", "float u_resolution_scale", "float u_rcp_resolution_scale",
                        "float u_resolution_scale_minus_one"},
                       false);
}

std::string GPU_HW_ShaderGen::GenerateScreenVertexShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareVertexEntryPoint(ss, {"float2 a_pos", "float2 a_tex0"}, 0, 1, {}, false, "", false, false, false);
  ss << R"(
{
  // VRAM writes and depth clears reset reversed PGXP depth to zero.
  #if API_OPENGL || API_OPENGL_ES
    v_pos = float4(a_pos, -1.0f, 1.0f);
  #else
    v_pos = float4(a_pos, 0.0f, 1.0f);
  #endif
  v_tex0 = a_tex0;

  // NDC space Y flip in Vulkan.
  #if API_OPENGL || API_OPENGL_ES || API_VULKAN
    v_pos.y = -v_pos.y;
  #endif
}
)";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateBatchVertexShader(const BatchVertexShaderSelector sel) const
{
  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "TEXTURED", sel.textured);
  DefineMacro(ss, "PALETTE", sel.palette);
  DefineMacro(ss, "PAGE_TEXTURE", sel.page_texture);
  DefineMacro(ss, "UV_LIMITS", sel.uv_limits);
  DefineMacro(ss, "FORCE_ROUND_TEXCOORDS", sel.force_round_texcoords);
  DefineMacro(ss, "DISABLE_UPSCALED_DIRECT_TEXTURES", sel.disable_upscaled_direct_textures);
  DefineMacro(ss, "PGXP_DEPTH", sel.pgxp_depth);
  DefineMacro(ss, "UPSCALED", sel.upscaled);

  WriteBatchUniformBuffer(ss);

  if (sel.textured && sel.page_texture)
  {
    if (sel.uv_limits)
    {
      DeclareVertexEntryPoint(
        ss, {"float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage", "float4 a_uv_limits"}, 1, 1,
        {{"nointerpolation", "float4 v_uv_limits"}}, false, "", sel.msaa, sel.per_sample_shading,
        sel.disable_color_perspective);
    }
    else
    {
      DeclareVertexEntryPoint(ss, {"float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage"}, 1, 1, {},
                              false, "", sel.msaa, sel.per_sample_shading, sel.disable_color_perspective);
    }
  }
  else if (sel.textured)
  {
    if (sel.uv_limits)
    {
      DeclareVertexEntryPoint(
        ss, {"float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage", "float4 a_uv_limits"}, 1, 1,
        {{"nointerpolation", sel.palette ? "uint4 v_texpage" : "uint2 v_texpage"},
         {"nointerpolation", "float4 v_uv_limits"}},
        false, "", sel.msaa, sel.per_sample_shading, sel.disable_color_perspective);
    }
    else
    {
      DeclareVertexEntryPoint(ss, {"float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage"}, 1, 1,
                              {{"nointerpolation", sel.palette ? "uint4 v_texpage" : "uint2 v_texpage"}}, false, "",
                              sel.msaa, sel.per_sample_shading, sel.disable_color_perspective);
    }
  }
  else
  {
    DeclareVertexEntryPoint(ss, {"float4 a_pos", "float4 a_col0"}, 1, 0, {}, false, "", sel.msaa,
                            sel.per_sample_shading, sel.disable_color_perspective);
  }

  ss << R"(
{
  // Offset the vertex position by 0.5 to ensure correct interpolation of texture coordinates
  // at 1x resolution scale. This doesn't work at >1x, we adjust the texture coordinates before
  // uploading there instead.
  float vertex_offset = (UPSCALED == 0) ? 0.5 : 0.0;

  // 0..+1023 -> -1..1
  float pos_x = ((a_pos.x + vertex_offset) / 512.0) - 1.0;
  float pos_y = ((a_pos.y + vertex_offset) / -256.0) + 1.0;

#if PGXP_DEPTH
  // Interpolate reciprocal camera Z in screen space, preserving W for perspective correction.
  // Reversed depth retains precision at large distances and clears to zero.
  // Z holds a bias towards the camera that grows with draw order, so that coplanar polygons don't fight.
  float pos_z = (1.0 + a_pos.z) / max(a_pos.w * 65535.0, 1.0);
  float pos_w = a_pos.w;
#else
  float pos_z = a_pos.z;
  float pos_w = a_pos.w;
#endif

#if API_OPENGL || API_OPENGL_ES
  // 0..1 to -1..1 depth range.
  pos_z = (pos_z * 2.0) - 1.0;
#endif

  // NDC space Y flip in Vulkan.
#if API_OPENGL || API_OPENGL_ES || API_VULKAN
  pos_y = -pos_y;
#endif

  v_pos = float4(pos_x * pos_w, pos_y * pos_w, pos_z * pos_w, pos_w);

  v_col0 = a_col0;
  #if TEXTURED
    v_tex0 = float2(uint2(a_texcoord & 0xFFFFu, a_texcoord >> 16));
    #if !PALETTE && !PAGE_TEXTURE && !DISABLE_UPSCALED_DIRECT_TEXTURES
      v_tex0 *= u_resolution_scale;
    #endif

    #if !PAGE_TEXTURE
      // base_x,base_y,palette_x,palette_y
      v_texpage.x = (a_texpage & 15u) * 64u;
      v_texpage.y = ((a_texpage >> 4) & 1u) * 256u;
      #if PALETTE
        v_texpage.z = ((a_texpage >> 16) & 63u) * 16u;
        v_texpage.w = ((a_texpage >> 22) & 511u);
      #endif
    #endif

    #if UV_LIMITS
      v_uv_limits = a_uv_limits * 255.0;

      #if FORCE_ROUND_TEXCOORDS && PALETTE
        // Add 0.5 to the upper bounds when upscaling, to work around interpolation differences.
        // Limited to force-round-texcoord hack, to avoid breaking other games.
        v_uv_limits.zw += 0.5;
      #elif !PAGE_TEXTURE && !PALETTE && !DISABLE_UPSCALED_DIRECT_TEXTURES
        // Treat coordinates as being in upscaled space, and extend the UV range to all "upscaled"
        // pixels. This means 1-pixel-high polygon-based framebuffer effects won't be downsampled.
        // (e.g. Mega Man Legends 2 haze effect)
        v_uv_limits *= u_resolution_scale;
        v_uv_limits.zw += u_resolution_scale_minus_one;
      #endif
    #endif
  #endif
}
)";

  return std::move(ss).str();
}

void GPU_HW_ShaderGen::WriteBatchTextureFilter(std::stringstream& ss, GPUTextureFilter texture_filter) const
{
  // JINC2 and xBRZ shaders originally from beetle-psx, modified to support filtering mask channel.
  if (texture_filter == GPUTextureFilter::Bilinear || texture_filter == GPUTextureFilter::BilinearBinAlpha)
  {
    ss << R"(
void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits,
                            out float4 texcol, out float ialpha)
{
  // Compute the coordinates of the four texels we will be interpolating between.
  // Clamp this to the triangle texture coordinates.
  float2 texel_top_left = frac(coords) - float2(0.5, 0.5);
  float2 texel_offset = sign(texel_top_left);
  float4 fcoords = max(coords.xyxy + float4(0.0, 0.0, texel_offset.x, texel_offset.y),
                        float4(0.0, 0.0, 0.0, 0.0));

  // Load four texels.
  float4 s00 = SampleFromVRAM(texpage, fcoords.xy, uv_limits);
  float4 s10 = SampleFromVRAM(texpage, fcoords.zy, uv_limits);
  float4 s01 = SampleFromVRAM(texpage, fcoords.xw, uv_limits);
  float4 s11 = SampleFromVRAM(texpage, fcoords.zw, uv_limits);

  // Compute alpha from how many texels aren't pixel color 0000h.
  float a00 = float(VECTOR_NEQ(s00, TRANSPARENT_PIXEL_COLOR));
  float a10 = float(VECTOR_NEQ(s10, TRANSPARENT_PIXEL_COLOR));
  float a01 = float(VECTOR_NEQ(s01, TRANSPARENT_PIXEL_COLOR));
  float a11 = float(VECTOR_NEQ(s11, TRANSPARENT_PIXEL_COLOR));

  // Bilinearly interpolate.
  float2 weights = abs(texel_top_left);
  texcol = lerp(lerp(s00, s10, weights.x), lerp(s01, s11, weights.x), weights.y);
  ialpha = lerp(lerp(a00, a10, weights.x), lerp(a01, a11, weights.x), weights.y);

  // Compensate for partially transparent sampling.
  if (ialpha > 0.0)
    texcol.rgb /= float3(ialpha, ialpha, ialpha);

#if !TEXTURE_ALPHA_BLENDING
  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
#endif
}
)";
  }
  else if (texture_filter == GPUTextureFilter::JINC2 || texture_filter == GPUTextureFilter::JINC2BinAlpha)
  {
    /*
       Hyllian's jinc windowed-jinc 2-lobe sharper with anti-ringing Shader

       Copyright (C) 2011-2016 Hyllian/Jararaca - sergiogdb@gmail.com

       Permission is hereby granted, free of charge, to any person obtaining a copy
       of this software and associated documentation files (the "Software"), to deal
       in the Software without restriction, including without limitation the rights
       to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
       copies of the Software, and to permit persons to whom the Software is
       furnished to do so, subject to the following conditions:

       The above copyright notice and this permission notice shall be included in
       all copies or substantial portions of the Software.

       THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
       IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
       FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
       AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
       LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
       OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
       THE SOFTWARE.
    */
    ss << R"(
CONSTANT float JINC2_WINDOW_SINC = 0.44;
CONSTANT float JINC2_SINC = 0.82;
CONSTANT float JINC2_AR_STRENGTH = 0.8;

CONSTANT   float halfpi            = 1.5707963267948966192313216916398;
CONSTANT   float pi                = 3.1415926535897932384626433832795;
CONSTANT   float wa                = 1.382300768;
CONSTANT   float wb                = 2.576105976;

// Calculates the distance between two points
float d(float2 pt1, float2 pt2)
{
  float2 v = pt2 - pt1;
  return sqrt(dot(v,v));
}

float min4(float a, float b, float c, float d)
{
    return min(a, min(b, min(c, d)));
}

float4 min4(float4 a, float4 b, float4 c, float4 d)
{
    return min(a, min(b, min(c, d)));
}

float max4(float a, float b, float c, float d)
{
  return max(a, max(b, max(c, d)));
}

float4 max4(float4 a, float4 b, float4 c, float4 d)
{
    return max(a, max(b, max(c, d)));
}

float4 resampler(float4 x)
{
   float4 res;

   // res = (x==float4(0.0, 0.0, 0.0, 0.0)) ?  float4(wa*wb)  :  sin(x*wa)*sin(x*wb)/(x*x);
   // Need to use mix(.., equal(..)) since we want zero check to be component wise
   float4 a = sin(x * wa) * sin(x * wb) / (x * x);
   float4 b = float4(wa*wb, wa*wb, wa*wb, wa*wb);
   bool4 s = VECTOR_COMP_EQ(x, float4(0.0, 0.0, 0.0, 0.0));
   return float4(s.x ? b.x : a.x, s.y ? b.y : a.y, s.z ? b.z : a.z, s.w ? b.w : a.w);
}

void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits,
                            out float4 texcol, out float ialpha)
{
    float4 weights[4];

    float2 dx = float2(1.0, 0.0);
    float2 dy = float2(0.0, 1.0);

    float2 pc = coords.xy;

    float2 tc = (floor(pc-float2(0.5,0.5))+float2(0.5,0.5));

    weights[0] = resampler(float4(d(pc, tc    -dx    -dy), d(pc, tc           -dy), d(pc, tc    +dx    -dy), d(pc, tc+2.0*dx    -dy)));
    weights[1] = resampler(float4(d(pc, tc    -dx       ), d(pc, tc              ), d(pc, tc    +dx       ), d(pc, tc+2.0*dx       )));
    weights[2] = resampler(float4(d(pc, tc    -dx    +dy), d(pc, tc           +dy), d(pc, tc    +dx    +dy), d(pc, tc+2.0*dx    +dy)));
    weights[3] = resampler(float4(d(pc, tc    -dx+2.0*dy), d(pc, tc       +2.0*dy), d(pc, tc    +dx+2.0*dy), d(pc, tc+2.0*dx+2.0*dy)));

    dx = dx;
    dy = dy;
    tc = tc;

#define sample_texel(coords) SampleFromVRAM(texpage, (coords), uv_limits)

    float4 c00 = sample_texel(tc    -dx    -dy);
    float a00 = float(VECTOR_NEQ(c00, TRANSPARENT_PIXEL_COLOR));
    float4 c10 = sample_texel(tc           -dy);
    float a10 = float(VECTOR_NEQ(c10, TRANSPARENT_PIXEL_COLOR));
    float4 c20 = sample_texel(tc    +dx    -dy);
    float a20 = float(VECTOR_NEQ(c20, TRANSPARENT_PIXEL_COLOR));
    float4 c30 = sample_texel(tc+2.0*dx    -dy);
    float a30 = float(VECTOR_NEQ(c30, TRANSPARENT_PIXEL_COLOR));
    float4 c01 = sample_texel(tc    -dx       );
    float a01 = float(VECTOR_NEQ(c01, TRANSPARENT_PIXEL_COLOR));
    float4 c11 = sample_texel(tc              );
    float a11 = float(VECTOR_NEQ(c11, TRANSPARENT_PIXEL_COLOR));
    float4 c21 = sample_texel(tc    +dx       );
    float a21 = float(VECTOR_NEQ(c21, TRANSPARENT_PIXEL_COLOR));
    float4 c31 = sample_texel(tc+2.0*dx       );
    float a31 = float(VECTOR_NEQ(c31, TRANSPARENT_PIXEL_COLOR));
    float4 c02 = sample_texel(tc    -dx    +dy);
    float a02 = float(VECTOR_NEQ(c02, TRANSPARENT_PIXEL_COLOR));
    float4 c12 = sample_texel(tc           +dy);
    float a12 = float(VECTOR_NEQ(c12, TRANSPARENT_PIXEL_COLOR));
    float4 c22 = sample_texel(tc    +dx    +dy);
    float a22 = float(VECTOR_NEQ(c22, TRANSPARENT_PIXEL_COLOR));
    float4 c32 = sample_texel(tc+2.0*dx    +dy);
    float a32 = float(VECTOR_NEQ(c32, TRANSPARENT_PIXEL_COLOR));
    float4 c03 = sample_texel(tc    -dx+2.0*dy);
    float a03 = float(VECTOR_NEQ(c03, TRANSPARENT_PIXEL_COLOR));
    float4 c13 = sample_texel(tc       +2.0*dy);
    float a13 = float(VECTOR_NEQ(c13, TRANSPARENT_PIXEL_COLOR));
    float4 c23 = sample_texel(tc    +dx+2.0*dy);
    float a23 = float(VECTOR_NEQ(c23, TRANSPARENT_PIXEL_COLOR));
    float4 c33 = sample_texel(tc+2.0*dx+2.0*dy);
    float a33 = float(VECTOR_NEQ(c33, TRANSPARENT_PIXEL_COLOR));

#undef sample_texel

    //  Get min/max samples
    float4 min_sample = min4(c11, c21, c12, c22);
    float min_sample_alpha = min4(a11, a21, a12, a22);
    float4 max_sample = max4(c11, c21, c12, c22);
    float max_sample_alpha = max4(a11, a21, a12, a22);

    float4 color;
    color = float4(dot(weights[0], float4(c00.x, c10.x, c20.x, c30.x)), dot(weights[0], float4(c00.y, c10.y, c20.y, c30.y)), dot(weights[0], float4(c00.z, c10.z, c20.z, c30.z)), dot(weights[0], float4(c00.w, c10.w, c20.w, c30.w)));
    color+= float4(dot(weights[1], float4(c01.x, c11.x, c21.x, c31.x)), dot(weights[1], float4(c01.y, c11.y, c21.y, c31.y)), dot(weights[1], float4(c01.z, c11.z, c21.z, c31.z)), dot(weights[1], float4(c01.w, c11.w, c21.w, c31.w)));
    color+= float4(dot(weights[2], float4(c02.x, c12.x, c22.x, c32.x)), dot(weights[2], float4(c02.y, c12.y, c22.y, c32.y)), dot(weights[2], float4(c02.z, c12.z, c22.z, c32.z)), dot(weights[2], float4(c02.w, c12.w, c22.w, c32.w)));
    color+= float4(dot(weights[3], float4(c03.x, c13.x, c23.x, c33.x)), dot(weights[3], float4(c03.y, c13.y, c23.y, c33.y)), dot(weights[3], float4(c03.z, c13.z, c23.z, c33.z)), dot(weights[3], float4(c03.w, c13.w, c23.w, c33.w)));
    color = color/(dot(weights[0], float4(1,1,1,1)) + dot(weights[1], float4(1,1,1,1)) + dot(weights[2], float4(1,1,1,1)) + dot(weights[3], float4(1,1,1,1)));

    float alpha;
    alpha = dot(weights[0], float4(a00, a10, a20, a30));
    alpha+= dot(weights[1], float4(a01, a11, a21, a31));
    alpha+= dot(weights[2], float4(a02, a12, a22, a32));
    alpha+= dot(weights[3], float4(a03, a13, a23, a33));
    //alpha = alpha/(weights[0].w + weights[1].w + weights[2].w + weights[3].w);
    alpha = alpha/(dot(weights[0], float4(1,1,1,1)) + dot(weights[1], float4(1,1,1,1)) + dot(weights[2], float4(1,1,1,1)) + dot(weights[3], float4(1,1,1,1)));

    // Anti-ringing
    float4 aux = color;
    float aux_alpha = alpha;
    color = clamp(color, min_sample, max_sample);
    alpha = clamp(alpha, min_sample_alpha, max_sample_alpha);
    color = lerp(aux, color, JINC2_AR_STRENGTH);
    alpha = lerp(aux_alpha, alpha, JINC2_AR_STRENGTH);

    // final sum and weight normalization
    ialpha = alpha;
    texcol = color;

    // Compensate for partially transparent sampling.
    if (ialpha > 0.0)
      texcol.rgb /= float3(ialpha, ialpha, ialpha);

#if !TEXTURE_ALPHA_BLENDING
  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
#endif
}
)";
  }
  else if (texture_filter == GPUTextureFilter::MonotonicCubic ||
           texture_filter == GPUTextureFilter::MonotonicCubicBinAlpha)
  {
    ss << R"(
float4 MonotonicSlope(float4 left, float4 right)
{
  float4 valid = step(float4(0.000001, 0.000001, 0.000001, 0.000001), left * right);
  float4 denominator = valid * (left + right) + (float4(1.0, 1.0, 1.0, 1.0) - valid);
  return valid * ((2.0 * left * right) / denominator);
}

float4 MonotonicInterpolate(float4 p0, float4 p1, float4 p2, float4 p3, float t)
{
  float4 m1 = MonotonicSlope(p1 - p0, p2 - p1);
  float4 m2 = MonotonicSlope(p2 - p1, p3 - p2);
  float t2 = t * t;
  float t3 = t2 * t;
  float4 value = (2.0 * t3 - 3.0 * t2 + 1.0) * p1 + (t3 - 2.0 * t2 + t) * m1 +
                 (-2.0 * t3 + 3.0 * t2) * p2 + (t3 - t2) * m2;
  return clamp(value, min(p1, p2), max(p1, p2));
}

void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits,
                            out float4 texcol, out float ialpha)
{
  float2 sample_pos = coords - float2(0.5, 0.5);
  float2 base = floor(sample_pos);
  float2 fpart = frac(sample_pos);
  float4 color_rows[4];
  float4 coverage_rows;

  for (int y = 0; y < 4; y++)
  {
    float4 samples[4];
    float4 coverage;
    for (int x = 0; x < 4; x++)
    {
      samples[x] = SampleFromVRAM(texpage, base + float2(float(x - 1), float(y - 1)), uv_limits);
      coverage[x] = float(VECTOR_NEQ(samples[x], TRANSPARENT_PIXEL_COLOR));
    }
    color_rows[y] = MonotonicInterpolate(samples[0], samples[1], samples[2], samples[3], fpart.x);
    coverage_rows[y] = MonotonicInterpolate(coverage.xxxx, coverage.yyyy, coverage.zzzz,
                                            coverage.wwww, fpart.x).x;
  }

  texcol = MonotonicInterpolate(color_rows[0], color_rows[1], color_rows[2], color_rows[3], fpart.y);
  ialpha = MonotonicInterpolate(coverage_rows.xxxx, coverage_rows.yyyy, coverage_rows.zzzz,
                                coverage_rows.wwww, fpart.y).x;
  if (ialpha > 0.0)
    texcol.rgb = saturate(texcol.rgb / float3(ialpha, ialpha, ialpha));

#if !TEXTURE_ALPHA_BLENDING
  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
#endif
}
)";
  }
  else if (texture_filter == GPUTextureFilter::AdaptiveDiagonal ||
           texture_filter == GPUTextureFilter::AdaptiveDiagonalBinAlpha)
  {
    ss << R"(
float AdaptiveDiagonalLuma(float4 color)
{
  return dot(color.rgb, float3(0.299, 0.587, 0.114));
}

void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits,
                            out float4 texcol, out float ialpha)
{
  float2 sample_pos = coords - float2(0.5, 0.5);
  float2 base = floor(sample_pos);
  float2 fpart = frac(sample_pos);

  float4 samples[16];
  float coverage[16];

  for (int y = 0; y < 4; y++)
  {
    for (int x = 0; x < 4; x++)
    {
      int index = y * 4 + x;
      samples[index] = SampleFromVRAM(texpage, base + float2(float(x - 1), float(y - 1)), uv_limits);
      coverage[index] = float(VECTOR_NEQ(samples[index], TRANSPARENT_PIXEL_COLOR));
    }
  }

  // Baseline interpolation. This guarantees sensible behaviour away from
  // strongly directional structure.
  float4 spatial = float4((1.0 - fpart.x) * (1.0 - fpart.y), fpart.x * (1.0 - fpart.y),
           (1.0 - fpart.x) * fpart.y, fpart.x * fpart.y);

  float4 bilinear = samples[5] * spatial.x + samples[6] * spatial.y +
    samples[9] * spatial.z + samples[10] * spatial.w;

  float bilinear_coverage = coverage[5] * spatial.x + coverage[6] * spatial.y +
    coverage[9] * spatial.z + coverage[10] * spatial.w;

  // Measure variation along the two diagonals.
  float down_mean = (AdaptiveDiagonalLuma(samples[0]) + AdaptiveDiagonalLuma(samples[5]) +
     AdaptiveDiagonalLuma(samples[10]) + AdaptiveDiagonalLuma(samples[15])) * 0.25;

  float up_mean = (AdaptiveDiagonalLuma(samples[12]) + AdaptiveDiagonalLuma(samples[9]) +
     AdaptiveDiagonalLuma(samples[6]) + AdaptiveDiagonalLuma(samples[3])) * 0.25;

  float4 down_delta = float4(AdaptiveDiagonalLuma(samples[0]),
           AdaptiveDiagonalLuma(samples[5]), AdaptiveDiagonalLuma(samples[10]),
           AdaptiveDiagonalLuma(samples[15])) - VECTOR_BROADCAST(float4, down_mean);

  float4 up_delta = float4(AdaptiveDiagonalLuma(samples[12]), AdaptiveDiagonalLuma(samples[9]),
           AdaptiveDiagonalLuma(samples[6]), AdaptiveDiagonalLuma(samples[3])) -
           VECTOR_BROADCAST(float4, up_mean);

  const float epsilon = 0.0001;

  float down_variance =
    epsilon + dot(down_delta, down_delta);

  float up_variance =
    epsilon + dot(up_delta, up_delta);

  // Project the fractional position onto each diagonal.
  float down_t = (fpart.x + fpart.y) * 0.5;
  float up_t = (fpart.x + (1.0 - fpart.y)) * 0.5;

  float4 down = lerp(samples[5], samples[10], down_t);
  float4 up = lerp(samples[9], samples[6], up_t);
  float down_coverage = lerp(coverage[5], coverage[10], down_t);
  float up_coverage = lerp(coverage[9], coverage[6], up_t);

  // Low variance means that diagonal is the more plausible continuation.
  float down_weight = 1.0 / down_variance;
  float up_weight = 1.0 / up_variance;
  float weight_sum = down_weight + up_weight;

  float4 directional_prediction = (down * down_weight + up * up_weight) / weight_sum;
  float directional_coverage = (down_coverage * down_weight + up_coverage * up_weight) / weight_sum;

  // IMPORTANT: closeness must be weighted using the SAME directional weights as the prediction. Taking max()
  // here associates the confidence of one diagonal with a prediction dominated by the other diagonal.
  float down_closeness = 1.0 - saturate(abs(fpart.x - fpart.y) * 2.0);
  float up_closeness = 1.0 - saturate(abs(fpart.x + fpart.y - 1.0) * 2.0);

  float directional_closeness = (down_weight * down_closeness + up_weight * up_closeness) / weight_sum;

  float anisotropy = abs(down_variance - up_variance) / (down_variance + up_variance);

  float adaptive_amount = anisotropy * directional_closeness;

  texcol = lerp(bilinear, directional_prediction, adaptive_amount);

  ialpha = saturate(lerp(bilinear_coverage, directional_coverage, adaptive_amount));

  if (ialpha > 0.0)
    texcol.rgb = saturate(texcol.rgb / float3(ialpha, ialpha, ialpha));
  else
    texcol.rgb = float3(0.0, 0.0, 0.0);

  texcol.a = saturate(texcol.a);

#if !TEXTURE_ALPHA_BLENDING
  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
#endif
}
)";
  }
  else if (texture_filter == GPUTextureFilter::DCCI || texture_filter == GPUTextureFilter::DCCIBinAlpha)
  {
    ss << R"(
float DCCILuma(float4 color)
{
  return dot(color.rgb, float3(0.299, 0.587, 0.114));
}

float4 DCCIMidpoint(float4 p0, float4 p1, float4 p2, float4 p3)
{
  // Cubic convolution at t = 0.5:
  // [-1, 9, 9, -1] / 16.
  return (-p0 + 9.0 * p1 + 9.0 * p2 - p3) * (1.0 / 16.0);
}

float DCCIMidpointCoverage(float p0, float p1, float p2, float p3)
{
  return (-p0 + 9.0 * p1 + 9.0 * p2 - p3) * (1.0 / 16.0);
}

float DCCIWeight(float d)
{
  // Reference DCCI uses 1 / (1 + d^k), k = 5.
  float d2 = d * d;
  return 1.0 / (1.0 + d2 * d2 * d);
}

void DCCISource(TEXPAGE_VALUE texpage, float2 p, float4 uv_limits,
                out float4 color, out float coverage)
{
  color = SampleFromVRAM(texpage, p, uv_limits);
  coverage = float(VECTOR_NEQ(color, TRANSPARENT_PIXEL_COLOR));
}

// First DCCI reconstruction stage.
//
// 'cell' is the upper-left source texel of the 2x2 source cell whose
// diagonal midpoint is being reconstructed. In the 2x lattice this
// corresponds to an odd/odd sample.
void DCCIStage1(TEXPAGE_VALUE texpage, float2 cell, float4 uv_limits,
                out float4 color, out float coverage)
{
  float4 s[16];
  float a[16];
  float l[16];

  for (int y = 0; y < 4; y++)
  {
    for (int x = 0; x < 4; x++)
    {
      int i = y * 4 + x;
      DCCISource(texpage,
                 cell + float2(float(x - 1), float(y - 1)),
                 uv_limits, s[i], a[i]);
      l[i] = DCCILuma(s[i]);
    }
  }

  // Exact type-1 direction detector from DCCI's 7x7 formulation,
  // reduced to the sixteen occupied source-lattice samples.
  //
  // d1: 45-degree gradient measure.
  float d1 =
    abs(l[4] - l[1]) +
    abs(l[8] - l[5]) + abs(l[5] - l[2]) +
    abs(l[12] - l[9]) + abs(l[9] - l[6]) + abs(l[6] - l[3]) +
    abs(l[13] - l[10]) + abs(l[10] - l[7]) +
    abs(l[14] - l[11]);

  // d2: 135-degree gradient measure.
  float d2 =
    abs(l[2] - l[7]) +
    abs(l[1] - l[6]) + abs(l[6] - l[11]) +
    abs(l[0] - l[5]) + abs(l[5] - l[10]) + abs(l[10] - l[15]) +
    abs(l[4] - l[9]) + abs(l[9] - l[14]) +
    abs(l[8] - l[13]);

  // v1 in the reference implementation: anti-diagonal.
  float4 p1 = DCCIMidpoint(s[12], s[9], s[6], s[3]);
  float p1a = DCCIMidpointCoverage(a[12], a[9], a[6], a[3]);

  // v2 in the reference implementation: main diagonal.
  float4 p2 = DCCIMidpoint(s[0], s[5], s[10], s[15]);
  float p2a = DCCIMidpointCoverage(a[0], a[5], a[10], a[15]);

  CONSTANT float DCCI_THRESHOLD = 1.15;

  if ((1.0 + d1) > DCCI_THRESHOLD * (1.0 + d2))
  {
    // Gradient is stronger in direction 1, interpolate along direction 2.
    color = p2;
    coverage = p2a;
  }
  else if ((1.0 + d2) > DCCI_THRESHOLD * (1.0 + d1))
  {
    color = p1;
    coverage = p1a;
  }
  else
  {
    float w1 = DCCIWeight(d1);
    float w2 = DCCIWeight(d2);
    float inv_sum = 1.0 / (w1 + w2);

    color = (w1 * p1 + w2 * p2) * inv_sum;
    coverage = (w1 * p1a + w2 * p2a) * inv_sum;
  }
}

// Fetch a point which is already known after DCCI stage 1.
//
// The 2x reconstruction lattice uses:
//   even/even -> original source samples
//   odd/odd   -> stage-1 diagonal samples
void DCCIKnown(TEXPAGE_VALUE texpage, float2 h, float4 uv_limits,
               out float4 color, out float coverage)
{
  int hx = int(h.x);
  int hy = int(h.y);

  if (((hx & 1) == 0) && ((hy & 1) == 0))
  {
    DCCISource(texpage, h * 0.5, uv_limits, color, coverage);
  }
  else
  {
    // This function is only called for points belonging to the
    // even-parity lattice, so the remaining possibility is odd/odd.
    DCCIStage1(texpage, floor(h * 0.5), uv_limits, color, coverage);
  }
}

// Second DCCI reconstruction stage.
//
// h has mixed parity (odd/even or even/odd). The surrounding
// even-parity lattice already consists of original samples plus the
// stage-1 diagonal samples.
void DCCIStage2(TEXPAGE_VALUE texpage, float2 h, float4 uv_limits,
                out float4 color, out float coverage)
{
  float4 s0,  s1,  s2,  s3;
  float4 s4,  s5,  s6,  s7;
  float4 s8,  s9,  s10, s11;
  float4 s12, s13, s14, s15;

  float a0,  a1,  a2,  a3;
  float a4,  a5,  a6,  a7;
  float a8,  a9,  a10, a11;
  float a12, a13, a14, a15;

  // Unique known samples required by the reference 5x5 direction
  // detector and the 7x7 cubic support.
  DCCIKnown(texpage, h + float2(-1.0, -2.0), uv_limits, s0,  a0);
  DCCIKnown(texpage, h + float2( 1.0, -2.0), uv_limits, s1,  a1);

  DCCIKnown(texpage, h + float2(-2.0, -1.0), uv_limits, s2,  a2);
  DCCIKnown(texpage, h + float2( 0.0, -1.0), uv_limits, s3,  a3);
  DCCIKnown(texpage, h + float2( 2.0, -1.0), uv_limits, s4,  a4);

  DCCIKnown(texpage, h + float2(-3.0,  0.0), uv_limits, s5,  a5);
  DCCIKnown(texpage, h + float2(-1.0,  0.0), uv_limits, s6,  a6);
  DCCIKnown(texpage, h + float2( 1.0,  0.0), uv_limits, s7,  a7);
  DCCIKnown(texpage, h + float2( 3.0,  0.0), uv_limits, s8,  a8);

  DCCIKnown(texpage, h + float2(-2.0,  1.0), uv_limits, s9,  a9);
  DCCIKnown(texpage, h + float2( 0.0,  1.0), uv_limits, s10, a10);
  DCCIKnown(texpage, h + float2( 2.0,  1.0), uv_limits, s11, a11);

  DCCIKnown(texpage, h + float2(-1.0,  2.0), uv_limits, s12, a12);
  DCCIKnown(texpage, h + float2( 1.0,  2.0), uv_limits, s13, a13);

  DCCIKnown(texpage, h + float2( 0.0, -3.0), uv_limits, s14, a14);
  DCCIKnown(texpage, h + float2( 0.0,  3.0), uv_limits, s15, a15);

  float l0  = DCCILuma(s0);
  float l1  = DCCILuma(s1);
  float l2  = DCCILuma(s2);
  float l3  = DCCILuma(s3);
  float l4  = DCCILuma(s4);
  float l6  = DCCILuma(s6);
  float l7  = DCCILuma(s7);
  float l9  = DCCILuma(s9);
  float l10 = DCCILuma(s10);
  float l11 = DCCILuma(s11);
  float l12 = DCCILuma(s12);
  float l13 = DCCILuma(s13);

  // Exact type-2/type-3 horizontal direction detector.
  float d1 =
    abs(l0 - l1) +
    abs(l6 - l7) +
    abs(l12 - l13) +
    abs(l2 - l3) + abs(l3 - l4) +
    abs(l9 - l10) + abs(l10 - l11);

  // Exact type-2/type-3 vertical direction detector.
  float d2 =
    abs(l2 - l9) +
    abs(l3 - l10) +
    abs(l4 - l11) +
    abs(l0 - l6) + abs(l6 - l12) +
    abs(l1 - l7) + abs(l7 - l13);

  // Horizontal candidate.
  float4 p1 = DCCIMidpoint(s5, s6, s7, s8);
  float p1a = DCCIMidpointCoverage(a5, a6, a7, a8);

  // Vertical candidate.
  float4 p2 = DCCIMidpoint(s14, s3, s10, s15);
  float p2a = DCCIMidpointCoverage(a14, a3, a10, a15);

  CONSTANT float DCCI_THRESHOLD = 1.15;

  if ((1.0 + d1) > DCCI_THRESHOLD * (1.0 + d2))
  {
    color = p2;
    coverage = p2a;
  }
  else if ((1.0 + d2) > DCCI_THRESHOLD * (1.0 + d1))
  {
    color = p1;
    coverage = p1a;
  }
  else
  {
    float w1 = DCCIWeight(d1);
    float w2 = DCCIWeight(d2);
    float inv_sum = 1.0 / (w1 + w2);

    color = (w1 * p1 + w2 * p2) * inv_sum;
    coverage = (w1 * p1a + w2 * p2a) * inv_sum;
  }
}

void DCCILattice(TEXPAGE_VALUE texpage, float2 h, float4 uv_limits,
                 out float4 color, out float coverage)
{
  int hx = int(h.x);
  int hy = int(h.y);
  bool x_odd = ((hx & 1) != 0);
  bool y_odd = ((hy & 1) != 0);

  if (!x_odd && !y_odd)
  {
    DCCISource(texpage, h * 0.5, uv_limits, color, coverage);
  }
  else if (x_odd && y_odd)
  {
    DCCIStage1(texpage, floor(h * 0.5), uv_limits, color, coverage);
  }
  else
  {
    DCCIStage2(texpage, h, uv_limits, color, coverage);
  }
}

void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits,
                            out float4 texcol, out float ialpha)
{
  // DCCI is a 2x reconstruction algorithm. Work in its canonical
  // high-resolution lattice, where source texels occupy even/even
  // positions.
  float2 source_pos = coords - float2(0.5, 0.5);
  float2 hpos = source_pos * 2.0;

  float2 hbase = floor(hpos);
  float2 fpart = frac(hpos);

  float4 c00, c10, c01, c11;
  float a00, a10, a01, a11;

  DCCILattice(texpage, hbase + float2(0.0, 0.0), uv_limits, c00, a00);
  DCCILattice(texpage, hbase + float2(1.0, 0.0), uv_limits, c10, a10);
  DCCILattice(texpage, hbase + float2(0.0, 1.0), uv_limits, c01, a01);
  DCCILattice(texpage, hbase + float2(1.0, 1.0), uv_limits, c11, a11);

  // Continuous sampling of the canonical DCCI 2x reconstruction.
  texcol = lerp(lerp(c00, c10, fpart.x),
                lerp(c01, c11, fpart.x), fpart.y);

  ialpha = lerp(lerp(a00, a10, fpart.x),
                 lerp(a01, a11, fpart.x), fpart.y);

  ialpha = saturate(ialpha);

  if (ialpha > 0.0)
    texcol.rgb = saturate(texcol.rgb / float3(ialpha, ialpha, ialpha));
  else
    texcol.rgb = float3(0.0, 0.0, 0.0);

  texcol.a = saturate(texcol.a);

#if !TEXTURE_ALPHA_BLENDING
  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
#endif
}
)";
  }
  else if (texture_filter == GPUTextureFilter::xBR || texture_filter == GPUTextureFilter::xBRBinAlpha)
  {
    /*
       Hyllian's xBR-vertex code and texel mapping

       Copyright (C) 2011/2016 Hyllian - sergiogdb@gmail.com

       Permission is hereby granted, free of charge, to any person obtaining a copy
       of this software and associated documentation files (the "Software"), to deal
       in the Software without restriction, including without limitation the rights
       to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
       copies of the Software, and to permit persons to whom the Software is
       furnished to do so, subject to the following conditions:

       The above copyright notice and this permission notice shall be included in
       all copies or substantial portions of the Software.

       THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
       IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
       FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
       AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
       LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
       OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
       THE SOFTWARE.
    */

    ss << R"(
CONSTANT int BLEND_NONE = 0;
CONSTANT int BLEND_NORMAL = 1;
CONSTANT int BLEND_DOMINANT = 2;
CONSTANT float LUMINANCE_WEIGHT = 1.0;
CONSTANT float EQUAL_COLOR_TOLERANCE = 0.1176470588235294;
CONSTANT float STEEP_DIRECTION_THRESHOLD = 2.2;
CONSTANT float DOMINANT_DIRECTION_THRESHOLD = 3.6;
CONSTANT float4 w = float4(0.2627, 0.6780, 0.0593, 0.5);

float DistYCbCr(float4 pixA, float4 pixB)
{
  const float scaleB = 0.5 / (1.0 - w.b);
  const float scaleR = 0.5 / (1.0 - w.r);
  float4 diff = pixA - pixB;
  float Y = dot(diff, w);
  float Cb = scaleB * (diff.b - Y);
  float Cr = scaleR * (diff.r - Y);

  return sqrt(((LUMINANCE_WEIGHT * Y) * (LUMINANCE_WEIGHT * Y)) + (Cb * Cb) + (Cr * Cr));
}

bool IsPixEqual(const float4 pixA, const float4 pixB)
{
  return (DistYCbCr(pixA, pixB) < EQUAL_COLOR_TOLERANCE);
}

float get_left_ratio(float2 center, float2 origin, float2 direction, float2 scale)
{
  float2 P0 = center - origin;
  float2 proj = direction * (dot(P0, direction) / dot(direction, direction));
  float2 distv = P0 - proj;
  float2 orth = float2(-direction.y, direction.x);
  float side = sign(dot(P0, orth));
  float v = side * length(distv * scale);

//  return step(0, v);
  return smoothstep(-sqrt(2.0)/2.0, sqrt(2.0)/2.0, v);
}

#define P(coord, xoffs, yoffs) SampleFromVRAM(texpage, coords + float2((xoffs), (yoffs)), uv_limits)

void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits,
                            out float4 texcol, out float ialpha)
{
  //---------------------------------------
  // Input Pixel Mapping:  -|x|x|x|-
  //                       x|A|B|C|x
  //                       x|D|E|F|x
  //                       x|G|H|I|x
  //                       -|x|x|x|-

  float2 scale = float2(8.0, 8.0);
  float2 pos = frac(coords.xy) - float2(0.5, 0.5);
  float2 coord = coords.xy - pos;

  float4 A = P(coord, -1,-1);
  float Aw = A.w;
  A.w = float(VECTOR_NEQ(A, TRANSPARENT_PIXEL_COLOR));
  float4 B = P(coord,  0,-1);
  float Bw = B.w;
  B.w = float(VECTOR_NEQ(B, TRANSPARENT_PIXEL_COLOR));
  float4 C = P(coord,  1,-1);
  float Cw = C.w;
  C.w = float(VECTOR_NEQ(C, TRANSPARENT_PIXEL_COLOR));
  float4 D = P(coord, -1, 0);
  float Dw = D.w;
  D.w = float(VECTOR_NEQ(D, TRANSPARENT_PIXEL_COLOR));
  float4 E = P(coord, 0, 0);
  float Ew = E.w;
  E.w = float(VECTOR_NEQ(E, TRANSPARENT_PIXEL_COLOR));
  float4 F = P(coord,  1, 0);
  float Fw = F.w;
  F.w = float(VECTOR_NEQ(F, TRANSPARENT_PIXEL_COLOR));
  float4 G = P(coord, -1, 1);
  float Gw = G.w;
  G.w = float(VECTOR_NEQ(G, TRANSPARENT_PIXEL_COLOR));
  float4 H = P(coord,  0, 1);
  float Hw = H.w;
  H.w = float(VECTOR_NEQ(H, TRANSPARENT_PIXEL_COLOR));
  float4 I = P(coord,  1, 1);
  float Iw = I.w;
  I.w = float(VECTOR_NEQ(H, TRANSPARENT_PIXEL_COLOR));

  // blendResult Mapping: x|y|
  //                      w|z|
  int4 blendResult = int4(BLEND_NONE,BLEND_NONE,BLEND_NONE,BLEND_NONE);

  // Preprocess corners
  // Pixel Tap Mapping: -|-|-|-|-
  //                    -|-|B|C|-
  //                    -|D|E|F|x
  //                    -|G|H|I|x
  //                    -|-|x|x|-
  if (!((VECTOR_EQ(E,F) && VECTOR_EQ(H,I)) || (VECTOR_EQ(E,H) && VECTOR_EQ(F,I))))
  {
    float dist_H_F = DistYCbCr(G, E) + DistYCbCr(E, C) + DistYCbCr(P(coord, 0,2), I) + DistYCbCr(I, P(coord, 2,0)) + (4.0 * DistYCbCr(H, F));
    float dist_E_I = DistYCbCr(D, H) + DistYCbCr(H, P(coord, 1,2)) + DistYCbCr(B, F) + DistYCbCr(F, P(coord, 2,1)) + (4.0 * DistYCbCr(E, I));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_H_F) < dist_E_I;
    blendResult.z = ((dist_H_F < dist_E_I) && VECTOR_NEQ(E,F) && VECTOR_NEQ(E,H)) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }


  // Pixel Tap Mapping: -|-|-|-|-
  //                    -|A|B|-|-
  //                    x|D|E|F|-
  //                    x|G|H|I|-
  //                    -|x|x|-|-
  if (!((VECTOR_EQ(D,E) && VECTOR_EQ(G,H)) || (VECTOR_EQ(D,G) && VECTOR_EQ(E,H))))
  {
    float dist_G_E = DistYCbCr(P(coord, -2,1)  , D) + DistYCbCr(D, B) + DistYCbCr(P(coord, -1,2), H) + DistYCbCr(H, F) + (4.0 * DistYCbCr(G, E));
    float dist_D_H = DistYCbCr(P(coord, -2,0)  , G) + DistYCbCr(G, P(coord, 0,2)) + DistYCbCr(A, E) + DistYCbCr(E, I) + (4.0 * DistYCbCr(D, H));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_D_H) < dist_G_E;
    blendResult.w = ((dist_G_E > dist_D_H) && VECTOR_NEQ(E,D) && VECTOR_NEQ(E,H)) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  // Pixel Tap Mapping: -|-|x|x|-
  //                    -|A|B|C|x
  //                    -|D|E|F|x
  //                    -|-|H|I|-
  //                    -|-|-|-|-
  if (!((VECTOR_EQ(B,C) && VECTOR_EQ(E,F)) || (VECTOR_EQ(B,E) && VECTOR_EQ(C,F))))
  {
    float dist_E_C = DistYCbCr(D, B) + DistYCbCr(B, P(coord, 1,-2)) + DistYCbCr(H, F) + DistYCbCr(F, P(coord, 2,-1)) + (4.0 * DistYCbCr(E, C));
    float dist_B_F = DistYCbCr(A, E) + DistYCbCr(E, I) + DistYCbCr(P(coord, 0,-2), C) + DistYCbCr(C, P(coord, 2,0)) + (4.0 * DistYCbCr(B, F));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_B_F) < dist_E_C;
    blendResult.y = ((dist_E_C > dist_B_F) && VECTOR_NEQ(E,B) && VECTOR_NEQ(E,F)) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  // Pixel Tap Mapping: -|x|x|-|-
  //                    x|A|B|C|-
  //                    x|D|E|F|-
  //                    -|G|H|-|-
  //                    -|-|-|-|-
  if (!((VECTOR_EQ(A,B) && VECTOR_EQ(D,E)) || (VECTOR_EQ(A,D) && VECTOR_EQ(B,E))))
  {
    float dist_D_B = DistYCbCr(P(coord, -2,0), A) + DistYCbCr(A, P(coord, 0,-2)) + DistYCbCr(G, E) + DistYCbCr(E, C) + (4.0 * DistYCbCr(D, B));
    float dist_A_E = DistYCbCr(P(coord, -2,-1), D) + DistYCbCr(D, H) + DistYCbCr(P(coord, -1,-2), B) + DistYCbCr(B, F) + (4.0 * DistYCbCr(A, E));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_D_B) < dist_A_E;
    blendResult.x = ((dist_D_B < dist_A_E) && VECTOR_NEQ(E,D) && VECTOR_NEQ(E,B)) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  float4 res = E;
  float resW = Ew;

  // Pixel Tap Mapping: -|-|-|-|-
  //                    -|-|B|C|-
  //                    -|D|E|F|x
  //                    -|G|H|I|x
  //                    -|-|x|x|-
  if(blendResult.z != BLEND_NONE)
  {
    float dist_F_G = DistYCbCr(F, G);
    float dist_H_C = DistYCbCr(H, C);
    bool doLineBlend = (blendResult.z == BLEND_DOMINANT ||
                !((blendResult.y != BLEND_NONE && !IsPixEqual(E, G)) || (blendResult.w != BLEND_NONE && !IsPixEqual(E, C)) ||
                  (IsPixEqual(G, H) && IsPixEqual(H, I) && IsPixEqual(I, F) && IsPixEqual(F, C) && !IsPixEqual(E, I))));

    float2 origin = float2(0.0, 1.0 / sqrt(2.0));
    float2 direction = float2(1.0, -1.0);
    if(doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_F_G <= dist_H_C) && VECTOR_NEQ(E,G) && VECTOR_NEQ(D,G);
      bool haveSteepLine = (STEEP_DIRECTION_THRESHOLD * dist_H_C <= dist_F_G) && VECTOR_NEQ(E,C) && VECTOR_NEQ(B,C);
      origin = haveShallowLine? float2(0.0, 0.25) : float2(0.0, 0.5);
      direction.x += haveShallowLine? 1.0: 0.0;
      direction.y -= haveSteepLine? 1.0: 0.0;
    }

    float4 blendPix = lerp(H,F, step(DistYCbCr(E, F), DistYCbCr(E, H)));
    float blendW = lerp(Hw,Fw, step(DistYCbCr(E, F), DistYCbCr(E, H)));
    res = lerp(res, blendPix, get_left_ratio(pos, origin, direction, scale));
    resW = lerp(resW, blendW, get_left_ratio(pos, origin, direction, scale));
  }

  // Pixel Tap Mapping: -|-|-|-|-
  //                    -|A|B|-|-
  //                    x|D|E|F|-
  //                    x|G|H|I|-
  //                    -|x|x|-|-
  if(blendResult.w != BLEND_NONE)
  {
    float dist_H_A = DistYCbCr(H, A);
    float dist_D_I = DistYCbCr(D, I);
    bool doLineBlend = (blendResult.w == BLEND_DOMINANT ||
                !((blendResult.z != BLEND_NONE && !IsPixEqual(E, A)) || (blendResult.x != BLEND_NONE && !IsPixEqual(E, I)) ||
                  (IsPixEqual(A, D) && IsPixEqual(D, G) && IsPixEqual(G, H) && IsPixEqual(H, I) && !IsPixEqual(E, G))));

    float2 origin = float2(-1.0 / sqrt(2.0), 0.0);
    float2 direction = float2(1.0, 1.0);
    if(doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_H_A <= dist_D_I) && VECTOR_NEQ(E,A) && VECTOR_NEQ(B,A);
      bool haveSteepLine  = (STEEP_DIRECTION_THRESHOLD * dist_D_I <= dist_H_A) && VECTOR_NEQ(E,I) && VECTOR_NEQ(F,I);
      origin = haveShallowLine? float2(-0.25, 0.0) : float2(-0.5, 0.0);
      direction.y += haveShallowLine? 1.0: 0.0;
      direction.x += haveSteepLine? 1.0: 0.0;
    }
    origin = origin;
    direction = direction;

    float4 blendPix = lerp(H,D, step(DistYCbCr(E, D), DistYCbCr(E, H)));
    float blendW = lerp(Hw,Dw, step(DistYCbCr(E, D), DistYCbCr(E, H)));
    res = lerp(res, blendPix, get_left_ratio(pos, origin, direction, scale));
    resW = lerp(resW, blendW, get_left_ratio(pos, origin, direction, scale));
  }

  // Pixel Tap Mapping: -|-|x|x|-
  //                    -|A|B|C|x
  //                    -|D|E|F|x
  //                    -|-|H|I|-
  //                    -|-|-|-|-
  if(blendResult.y != BLEND_NONE)
  {
    float dist_B_I = DistYCbCr(B, I);
    float dist_F_A = DistYCbCr(F, A);
    bool doLineBlend = (blendResult.y == BLEND_DOMINANT ||
                !((blendResult.x != BLEND_NONE && !IsPixEqual(E, I)) || (blendResult.z != BLEND_NONE && !IsPixEqual(E, A)) ||
                  (IsPixEqual(I, F) && IsPixEqual(F, C) && IsPixEqual(C, B) && IsPixEqual(B, A) && !IsPixEqual(E, C))));

    float2 origin = float2(1.0 / sqrt(2.0), 0.0);
    float2 direction = float2(-1.0, -1.0);

    if(doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_B_I <= dist_F_A) && VECTOR_NEQ(E,I) && VECTOR_NEQ(H,I);
      bool haveSteepLine  = (STEEP_DIRECTION_THRESHOLD * dist_F_A <= dist_B_I) && VECTOR_NEQ(E,A) && VECTOR_NEQ(D,A);
      origin = haveShallowLine? float2(0.25, 0.0) : float2(0.5, 0.0);
      direction.y -= haveShallowLine? 1.0: 0.0;
      direction.x -= haveSteepLine? 1.0: 0.0;
    }

    float4 blendPix = lerp(F,B, step(DistYCbCr(E, B), DistYCbCr(E, F)));
    float blendW = lerp(Fw,Bw, step(DistYCbCr(E, B), DistYCbCr(E, F)));
    res = lerp(res, blendPix, get_left_ratio(pos, origin, direction, scale));
    resW = lerp(resW, blendW, get_left_ratio(pos, origin, direction, scale));
  }

  // Pixel Tap Mapping: -|x|x|-|-
  //                    x|A|B|C|-
  //                    x|D|E|F|-
  //                    -|G|H|-|-
  //                    -|-|-|-|-
  if(blendResult.x != BLEND_NONE)
  {
    float dist_D_C = DistYCbCr(D, C);
    float dist_B_G = DistYCbCr(B, G);
    bool doLineBlend = (blendResult.x == BLEND_DOMINANT ||
                !((blendResult.w != BLEND_NONE && !IsPixEqual(E, C)) || (blendResult.y != BLEND_NONE && !IsPixEqual(E, G)) ||
                  (IsPixEqual(C, B) && IsPixEqual(B, A) && IsPixEqual(A, D) && IsPixEqual(D, G) && !IsPixEqual(E, A))));

    float2 origin = float2(0.0, -1.0 / sqrt(2.0));
    float2 direction = float2(-1.0, 1.0);
    if(doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_D_C <= dist_B_G) && VECTOR_NEQ(E,C) && VECTOR_NEQ(F,C);
      bool haveSteepLine  = (STEEP_DIRECTION_THRESHOLD * dist_B_G <= dist_D_C) && VECTOR_NEQ(E,G) && VECTOR_NEQ(H,G);
      origin = haveShallowLine? float2(0.0, -0.25) : float2(0.0, -0.5);
      direction.x -= haveShallowLine? 1.0: 0.0;
      direction.y += haveSteepLine? 1.0: 0.0;
    }

    float4 blendPix = lerp(D,B, step(DistYCbCr(E, B), DistYCbCr(E, D)));
    float blendW = lerp(Dw,Bw, step(DistYCbCr(E, B), DistYCbCr(E, D)));
    res = lerp(res, blendPix, get_left_ratio(pos, origin, direction, scale));
    resW = lerp(resW, blendW, get_left_ratio(pos, origin, direction, scale));
  }

  ialpha = res.w;
  texcol = float4(res.xyz, resW);

  // Compensate for partially transparent sampling.
  if (ialpha > 0.0)
    texcol.rgb /= float3(ialpha, ialpha, ialpha);

#if !TEXTURE_ALPHA_BLENDING
  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
#endif
}

#undef P

)";
  }
  else if (texture_filter == GPUTextureFilter::SharpBilinear)
  {
    ss << R"(
void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits,
                            out float4 texcol, out float ialpha)
{
  // Coordinates are normally in native texel units. One output pixel therefore
  // spans 1 / resolution_scale texels. Direct upscaled textures are the exception:
  // their texture coordinates have already been multiplied by resolution_scale.
#if UPSCALED && !PALETTE && !PAGE_TEXTURE && !DISABLE_UPSCALED_DIRECT_TEXTURES
  float filter_width = 1.0;
#else
  float filter_width = 1.0 / u_resolution_scale;
#endif

  // Determine the nearest texel and the neighboring texel in the direction of the current sample.
  float2 texel_center_offset = frac(coords) - float2(0.5, 0.5);
  float2 texel_offset = sign(texel_center_offset);

  float4 fcoords =
    max(coords.xyxy + float4(0.0, 0.0, texel_offset.x, texel_offset.y),
        float4(0.0, 0.0, 0.0, 0.0));

  float4 s00 = SampleFromVRAM(texpage, fcoords.xy, uv_limits);
  float4 s10 = SampleFromVRAM(texpage, fcoords.zy, uv_limits);
  float4 s01 = SampleFromVRAM(texpage, fcoords.xw, uv_limits);
  float4 s11 = SampleFromVRAM(texpage, fcoords.zw, uv_limits);

  float a00 = float(VECTOR_NEQ(s00, TRANSPARENT_PIXEL_COLOR));
  float a10 = float(VECTOR_NEQ(s10, TRANSPARENT_PIXEL_COLOR));
  float a01 = float(VECTOR_NEQ(s01, TRANSPARENT_PIXEL_COLOR));
  float a11 = float(VECTOR_NEQ(s11, TRANSPARENT_PIXEL_COLOR));

  // Keep the central portion of each texel nearest-neighbor sharp.
  // The transition across each texel boundary is one output pixel wide.
  float half_filter_width = filter_width * 0.5;
  float sharp_region = 0.5 - half_filter_width;

  float2 weights =
    saturate((abs(texel_center_offset) - sharp_region) / filter_width);

  texcol = lerp(lerp(s00, s10, weights.x),
                lerp(s01, s11, weights.x),
                weights.y);

  ialpha = lerp(lerp(a00, a10, weights.x),
                lerp(a01, a11, weights.x),
                weights.y);

  if (ialpha > 0.0)
    texcol.rgb /= float3(ialpha, ialpha, ialpha);

#if !TEXTURE_ALPHA_BLENDING
  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
#endif
}
)";
  }
  else if (texture_filter == GPUTextureFilter::MMPX)
  {
    ss << "#define src(xoffs, yoffs) packUnorm4x8(SampleFromVRAM(texpage, bcoords + float2((xoffs), (yoffs)), "
          "uv_limits))\n";

    /*
     * This part of the shader is from MMPX.glc from https://casual-effects.com/research/McGuire2021PixelArt/index.html
     * Copyright 2020 Morgan McGuire & Mara Gagiu.
     * Provided under the Open Source MIT license https://opensource.org/licenses/MIT
     */
    ss << R"(
uint luma(uint C) {
    uint alpha = (C & 0xFF000000u) >> 24;
    return (((C & 0x00FF0000u) >> 16) + ((C & 0x0000FF00u) >> 8) + (C & 0x000000FFu) + 1u) * (256u - alpha);
}

bool all_eq2(uint B, uint A0, uint A1) {
    return ((B ^ A0) | (B ^ A1)) == 0u;
}

bool all_eq3(uint B, uint A0, uint A1, uint A2) {
    return ((B ^ A0) | (B ^ A1) | (B ^ A2)) == 0u;
}

bool all_eq4(uint B, uint A0, uint A1, uint A2, uint A3) {
    return ((B ^ A0) | (B ^ A1) | (B ^ A2) | (B ^ A3)) == 0u;
}

bool any_eq3(uint B, uint A0, uint A1, uint A2) {
    return B == A0 || B == A1 || B == A2;
}

bool none_eq2(uint B, uint A0, uint A1) {
    return (B != A0) && (B != A1);
}

bool none_eq4(uint B, uint A0, uint A1, uint A2, uint A3) {
    return B != A0 && B != A1 && B != A2 && B != A3;
}

void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits, out float4 texcol, out float ialpha)
{
  float2 bcoords = floor(coords);

  uint A = src(-1, -1), B = src(+0, -1), C = src(+1, -1);
  uint D = src(-1, +0), E = src(+0, +0), F = src(+1, +0);
  uint G = src(-1, +1), H = src(+0, +1), I = src(+1, +1);

  uint J = E, K = E, L = E, M = E;

  if (((A ^ E) | (B ^ E) | (C ^ E) | (D ^ E) | (F ^ E) | (G ^ E) | (H ^ E) | (I ^ E)) != 0u) {
    uint P = src(+0, -2), S = src(+0, +2);
    uint Q = src(-2, +0), R = src(+2, +0);
    uint Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);

    // 1:1 slope rules
    if ((D == B && D != H && D != F) && (El >= Dl || E == A) && any_eq3(E, A, C, G) && ((El < Dl) || A != D || E != P || E != Q)) J = D;
    if ((B == F && B != D && B != H) && (El >= Bl || E == C) && any_eq3(E, A, C, I) && ((El < Bl) || C != B || E != P || E != R)) K = B;
    if ((H == D && H != F && H != B) && (El >= Hl || E == G) && any_eq3(E, A, G, I) && ((El < Hl) || G != H || E != S || E != Q)) L = H;
    if ((F == H && F != B && F != D) && (El >= Fl || E == I) && any_eq3(E, C, G, I) && ((El < Fl) || I != H || E != R || E != S)) M = F;

    // Intersection rules
    if ((E != F && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && (F != src(+3, +0))) K = M = F;
    if ((E != D && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && (D != src(-3, +0))) J = L = D;
    if ((E != H && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && (H != src(+0, +3))) L = M = H;
    if ((E != B && all_eq4(E, A, C, H, S) && all_eq2(B, D, F)) && (B != src(+0, -3))) J = K = B;
    if (Bl < El && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F)) J = K = B;
    if (Hl < El && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F)) L = M = H;
    if (Fl < El && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H)) K = M = F;
    if (Dl < El && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H)) J = L = D;

    // 2:1 slope rules
    if (H != B) {
      if (H != A && H != E && H != C) {
        if (all_eq3(H, G, F, R) && none_eq2(H, D, src(+2, -1))) L = M;
        if (all_eq3(H, I, D, Q) && none_eq2(H, F, src(-2, -1))) M = L;
      }

      if (B != I && B != G && B != E) {
        if (all_eq3(B, A, F, R) && none_eq2(B, D, src(+2, +1))) J = K;
        if (all_eq3(B, C, D, Q) && none_eq2(B, F, src(-2, +1))) K = J;
      }
    } // H !== B

    if (F != D) {
      if (D != I && D != E && D != C) {
        if (all_eq3(D, A, H, S) && none_eq2(D, B, src(+1, +2))) J = L;
        if (all_eq3(D, G, B, P) && none_eq2(D, H, src(+1, -2))) L = J;
      }

      if (F != E && F != A && F != G) {
        if (all_eq3(F, C, H, S) && none_eq2(F, B, src(-1, +2))) K = M;
        if (all_eq3(F, I, B, P) && none_eq2(F, H, src(-1, -2))) M = K;
      }
    } // F !== D
  } // not constant

  // select quadrant based on fractional part of texture coordinates
  float2 fpart = frac(coords);
  uint res = (fpart.x < 0.5f) ? ((fpart.y < 0.5f) ? J : L) : ((fpart.y < 0.5f) ? K : M);

  ialpha = float(res != 0u);
  texcol = unpackUnorm4x8(res);
}

#undef src
)";
  }
  else if (texture_filter == GPUTextureFilter::MMPXEnhanced)
  {
    ss << R"(

	#define srcf(xoffs,yoffs) \
	  SampleFromVRAM(texpage, bcoords + float2(xoffs, yoffs), uv_limits) * \
	  step(abs(bcoords.x + (xoffs) - clamp(bcoords.x + (xoffs), uv_limits.x, uv_limits.z)), 0.001) * \
	  step(abs(bcoords.y + (yoffs) - clamp(bcoords.y + (yoffs), uv_limits.y, uv_limits.w)), 0.001)

	#define src(xoffs,yoffs) packUnorm4x8(srcf(xoffs,yoffs))
	)";
    /* MMPX Enhanced
     * An optimized refinement of the original MMPX shader that addresses key
     * visual artifacts while fully preserving the baseline's signature performance and efficiency.
     *
     * License: MIT
     * (C) 2025-2026 by crashGG.
     */

    ss << R"(

float luma(float4 col) {

	//Use CRT-era BT.601 standard.
    float colBT601 = dot(col.rgb, float3(0.299, 0.587, 0.114));
	//*	x2 value for pure blue
	return colBT601 * (1.0 + step(col.r+col.g, 0.01));
}

float mixGate(float4 col1, float4 col2) {

	float4 diff = col1 - col2;

	float delta_range = max(diff.r, max(diff.g, diff.b)) - min(diff.r, min(diff.g, diff.b));

	float dot_diff = dot(diff, diff);

	float factor = (delta_range * delta_range) * 2.618034;

	return step(dot_diff, mix(0.75, 0.0, factor));
}
#if GLSL

	#define all_eq2(a, b1, b2) (a == b1 && a == b2)
	#define all_eq4(a, b1, b2, b3, b4) (a == b1 && a == b2 && a == b3 && a == b4)

	float checknonzero(float4 res) {
		uvec4 uBits = floatBitsToUint(res);
		return float((uBits.r | uBits.g | uBits.b | uBits.a) != 0u);
	}

#else
	//* faster on dx12
	#define all_eq2(a, b1, b2) \
		( ((a ^ b1) | (a ^ b2)) == 0u )

	#define all_eq4(a, b1, b2, b3, b4) \
		( ((a ^ b1) | (a ^ b2) | (a ^ b3) | (a ^ b4)) == 0u )

	float checknonzero(float4 res) {
		uint4 uBits = asuint(res);
		return float((uBits.r | uBits.g | uBits.b | uBits.a) != 0u);
	}

#endif

#define any_eq2(a, b1, b2) (a == b1 || a == b2)
#define none_eq2(a, b1, b2) (a!=b1 && a!=b2)
#define none_eq4(a, b1, b2, b3, b4) (a!=b1 && a!=b2 && a!=b3 && a!=b4)


float4 admixC(float4 vX, float4 vE) {

	float mixFactor = mixGate(vX, vE) * (-0.381966) + 1.0;

	return mix(vX,vE,mixFactor);
}

float4 admixK(float4 vX, float4 vE) {

    float4 diff = vX - vE;

	float mixFactor = clamp(dot(diff.rgb, diff.rgb), 0.0, 1.0) * 0.5 + 0.5; // xxx.alpha

	return mix(vX,vE,mixFactor);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits, out float4 texcol, out float ialpha)
{

	float2 bcoords = floor(coords);

	float4 vE = SampleFromVRAM(texpage, bcoords, uv_limits);

	float4 vB = srcf(0.0, -1.0);
	float4 vD = srcf(-1.0, 0.0);
	float4 vF = srcf(+1.0, 0.0);
	float4 vH = srcf(0.0, +1.0);

    uint E = packUnorm4x8(vE);
    uint B = packUnorm4x8(vB);
    uint D = packUnorm4x8(vD);
    uint F = packUnorm4x8(vF);
    uint H = packUnorm4x8(vH);

	// default pixel
	ialpha = float(E != 0u);
	texcol = vE;

bool skiprest = (E == D && E == F) || (E == B && E == H) || (B == H && D == F);
if (!skiprest) {

    // 5x5
    uint A = src(-1.0, -1.0);
    uint C = src(+1.0, -1.0);
    uint G = src(-1.0, +1.0);
    uint I = src(+1.0, +1.0);

	uint P  = src( 0.0, -2.0);
	uint Q  = src(-2.0,  0.0);
	uint R  = src(+2.0,  0.0);
	uint S  = src( 0.0, +2.0);

    float Bl = luma(vB) + float(B==0u) *2.0;
    float Dl = luma(vD) + float(D==0u) *2.0;
    float El = luma(vE) + float(E==0u) *2.0;
    float Fl = luma(vF) + float(F==0u) *2.0;
    float Hl = luma(vH) + float(H==0u) *2.0;

    float4 J = vE;    float4 K = vE;    float4 L = vE;    float4 M = vE;

if (B != H && D != F) {

	uint PA = src(-1.0, -2.0);
	uint PC = src(+1.0, -2.0);
	uint QA = src(-2.0, -1.0);
	uint QG = src(-2.0, +1.0);
	uint RC = src(+2.0, -1.0);
	uint RI = src(+2.0, +1.0);
	uint SG = src(-1.0, +2.0);
	uint SI = src(+1.0, +2.0);

// B - D
	if ( D == B && (El > Dl || El < Dl && E == A && B !=PA && D !=QA) && any_eq2(E, C, G) && (A != D || E != P || E != Q)
		) {
		J=vB;
		if (all_eq2(Q,D,C) && Q != QG && C != R) K=J;
		if (all_eq2(P,B,G) && P != PC && G != S) L=J;
		skiprest = true;
	}

// B - F
	if ( B == F && (El > Bl || El < Bl && E == C && B !=PC && F !=RC) && any_eq2(E, A, I) && (C != B || E != P || E != R)
	 ) {
		K=vB;
		if (all_eq2(R,F,A) && R != RI && A != Q) J=K;
		if (all_eq2(P,B,I) && P != PA && I != S) M=K;
		skiprest = true;
	}

// D - H
    if ( H == D && (El > Hl || El < Hl && E == G && D !=QG && H !=SG) && any_eq2(E, A, I) && (G != H || E != S || E != Q)
	 ) {
		L=vH;
		if (all_eq2(Q,D,I) && Q != QA && R != I) M=L;
		if (all_eq2(S,H,A) && S != SI && A != P) J=L;
		skiprest = true;
	}
// F - H
    if ( F == H && (El > Fl || El < Fl && E == I && F !=RI && H !=SI) && any_eq2(E, C, G) && (I != H || E != R || E != S)
	  ) {
		M=vH;
		if (all_eq2(R,F,G) && R != RC && Q != G) L=M;
		if (all_eq2(S,H,C) && S != SG && P != C) K=M;
		skiprest = true;
	}

}

skiprest = skiprest||E==0u||B==0u||D==0u||F==0u||H==0u;

if (!skiprest) {

/* Concave + Cross type */

    if (Bl < El && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F)) { J=admixC(vB,vE);	K=J;}
    if (Hl < El && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F)) { L=admixC(vH,vE);	M=L;}
    if (Fl < El && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H)) { K=admixC(vF,vE);	M=K;}
    if (Dl < El && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H)) { J=admixC(vD,vE);	L=J;}

/* K type */

    if ( (E != F && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && (F != src(+3.0, +0.0))) {K=admixK(vF,vE);	M=K;}
    if ( (E != D && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && (D != src(-3.0, +0.0))) {J=admixK(vD,vE);	L=J;}
    if ( (E != H && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && (H != src(+0.0, +3.0))) {L=admixK(vH,vE);	M=L;}
    if ( (E != B && all_eq4(E, A, C, H, S) && all_eq2(B, D, F)) && (B != src(+0.0, -3.0))) {J=admixK(vB,vE);	K=J;}
}
	//final write
	float2 fpart = frac(coords);

	float4 res = (fpart.x < 0.5) ? ((fpart.y < 0.5) ? J : L) : ((fpart.y < 0.5) ? K : M);

	ialpha = checknonzero(res);
	texcol = res;
}

}

#undef src
#undef srcf
#undef all_eq2
#undef all_eq4
#undef any_eq2
#undef none_eq2
#undef none_eq4

)";
  }
  else if (texture_filter == GPUTextureFilter::MMPXAdvanced)
  {
    ss << R"(

	#define srcf(xoffs,yoffs) \
	  SampleFromVRAM(texpage, bcoords + float2(xoffs, yoffs), uv_limits) * \
	  step(abs(bcoords.x + (xoffs) - clamp(bcoords.x + (xoffs), uv_limits.x, uv_limits.z)), 0.001) * \
	  step(abs(bcoords.y + (yoffs) - clamp(bcoords.y + (yoffs), uv_limits.y, uv_limits.w)), 0.001)

	#define src(xoffs,yoffs) packUnorm4x8(srcf(xoffs,yoffs))
	)";
/* =========================================================================
 * MMPX Advanced v3.5
 * =========================================================================
 * An optimized and heavily expanded derivative of the MMPX algorithm.
 * 
 * The baseline MMPX implementation relies on a minimalist rule-set, which 
 * inherently suffers from topological conflicts at complex intersections, 
 * manifesting as jarring structural artifacts, "bubbles," and "spurs." 
 * 
 * MMPX Advanced introduces comprehensive morphological analysis, utilizing a 
 * vast array of high-precision conditional predicates to expand the 
 * architectural logic by an order of magnitude. Through exhaustive conflict-
 * scenario analysis and granular edge-case resolution, this refinement 
 * completely eliminates morphological glitches. 
 * 
 * Furthermore, the integration of approximate pixel-matching logic effectively 
 * addresses unmapped topological configurations overlooked by the baseline 
 * specification, thereby delivering flawless detail reconstruction while 
 * preserving the authentic pixel-art aesthetic.

 * License: MIT
 * Copyright (c) 2025-2026 by crashGG.
 * ========================================================================= */

    ss << R"(

//RGB visual weight + alpha bias
float luma(float4 col) {

	//Use CRT-era BT.601 standard. Clamp range to [0.0 - 0.999]
	float colBT601 = min(dot(col.rgb, float3(0.299, 0.587, 0.114)), 0.999);
	//*	x2 value for pure blue
	return colBT601 * (1.0 + step(col.r+col.g, 0.01));
}

/* Constant Descriptions:
 * 0.145898    : (~0.382) ^2
 * 0.8541      : 1.0 -(~0.382) ^2
 * 0.0638587   : (RGB Euclidean distance scaled by ~0.382 twice) ^2
 * 0.75        : (RGB Euclidean distance scaled by  0.500) ^2
 */

// duck calculation: dot(diff,diff) directly incorporates alpha channel  //duck.alpha
// Note: Transparent duck pixels are determined outside sim and mixFactor functions

bool simb(float4 col1, float4 col2) {

	float4 diff = col1 - col2;

	// RGB color difference range (max_diff - min_diff)
	float delta_range = max(diff.r, max(diff.g, diff.b)) - min(diff.r, min(diff.g, diff.b));

	float dot_diff = dot(diff, diff);

	// Equivalent to (delta_range / 0.145898 )^2
	float factor = (delta_range * delta_range) * 46.9787;

	return dot_diff < mix(0.0638587, 0.0, factor);
}

bool sim(float4 col1, float4 col2) {

	float4 diff = col1 - col2;

	float delta_range = max(diff.r, max(diff.g, diff.b)) - min(diff.r, min(diff.g, diff.b));

	float dot_diff = dot(diff, diff);

	// Equivalent to (delta_range / 0.382 )^2
	float factor = (delta_range * delta_range) * 6.8541;

	return dot_diff < mix(0.0638587, 0.0, factor);
}

float mixGate(float4 col1, float4 col2) {

	float4 diff = col1 - col2;

	float delta_range = max(diff.r, max(diff.g, diff.b)) - min(diff.r, min(diff.g, diff.b));

	float dot_diff = dot(diff, diff);

	// Equivalent to (delta_range / 0.618 )^2
	float factor = (delta_range * delta_range) * 2.618034;

	return step(dot_diff, mix(0.75, 0.0, factor));
}

#define eq(a,b) (a==b)
#define ne(a,b) (a!=b)

#define any_eq2(a, b1, b2) (eq(a,b1)||eq(a,b2))
#define any_eq3(a, b1, b2, b3) (eq(a,b1)||eq(a,b2)||eq(a,b3))
#define any_eq4(a, b1, b2, b3, b4) (eq(a,b1)||eq(a,b2)||eq(a,b3)||eq(a,b4))
#define not_eq2(a, b1, b2)  (ne(a,b1) && ne(a,b2))
#define not_eq3(a, b1, b2, b3)  (ne(a,b1) && ne(a,b2) && ne(a,b3))
#define not_eq4(a, b1, b2, b3, b4) (ne(a,b1) && ne(a,b2) && ne(a,b3) && ne(a,b4))
#define not_eq5(a, b1, b2, b3, b4, b5) (ne(a,b1) && ne(a,b2) && ne(a,b3) && ne(a,b4) && ne(a,b5))


#if GLSL

	#define all_eq2(a, b1, b2) \
		( eq(a,b1) && eq(a,b2) )

	#define all_eq3(a, b1, b2, b3) \
		( eq(a,b1) && eq(a,b2) && eq(a,b3) )

	#define all_eq4(a, b1, b2, b3, b4) \
		( eq(a,b1) && eq(a,b2) && eq(a,b3) && eq(a,b4) )

    #define checkblack(col) all(lessThan((col).rgb, float3(0.1, 0.078, 0.1)))
    #define checkwhite(col) all(greaterThan((col).rgb, float3(0.92, 0.92, 0.92)))
	//#define checkeq(a,b) all(lessThan(abs((a).rgb-(b).rgb), float3(0.001, 0.001, 0.001)))

	bool checkeq(float4 a, float4 b) {
		uvec3 diff = floatBitsToUint(a.rgb) ^ floatBitsToUint(b.rgb);
		return ((diff.r | diff.g | diff.b) == 0u);
	}

	float checknonzero(float4 res) {
		uvec4 uBits = floatBitsToUint(res);
		return float((uBits.r | uBits.g | uBits.b | uBits.a) != 0u);
	}

#else
	//* faster on dx12
	#define all_eq2(a, b1, b2) \
		( ((a ^ b1) | (a ^ b2)) == 0u )

	#define all_eq3(a, b1, b2, b3) \
		( ((a ^ b1) | (a ^ b2) | (a ^ b3)) == 0u )

	#define all_eq4(a, b1, b2, b3, b4) \
		( ((a ^ b1) | (a ^ b2) | (a ^ b3) | (a ^ b4)) == 0u )

    #define checkblack(col) all((col).rgb < float3(0.1, 0.078, 0.1))
    #define checkwhite(col) all((col).rgb > float3(0.92, 0.92, 0.92))
	//#define checkeq(a,b) all(abs((a).rgb - (b).rgb) < float3(0.001, 0.001, 0.001))

	bool checkeq(float4 a, float4 b) {
		uint3 diff = asuint(a.rgb) ^ asuint(b.rgb);
		return ((diff.r | diff.g | diff.b) == 0u);
	}

	float checknonzero(float4 res) {
		uint4 uBits = asuint(res);
		return float((uBits.r | uBits.g | uBits.b | uBits.a) != 0u);
	}

#endif


// Debug Hooks
//#define testcolor float4(1.0, 0.0, 1.0, 1.0)  // Magenta
//#define testcolor2 float4(0.0, 1.0, 1.0, 1.0)  // Cyan
//#define testcolor3 float4(1.0, 1.0, 0.0, 1.0)  // Yellow
//#define testcolor4 float4(1.0, 1.0, 1.0, 1.0)  // White
#define slopeOFF float4(2.0, 2.0, 2.0, 2.0)
#define slopeBAD float4(4.0, 4.0, 4.0, 4.0)
#define theEXIT float4(8.0, 8.0, 8.0, 8.0)

#define mixXE mix(vX,vE,mixFactor)
#define mixXEoff mixXE+slopeOFF
#define Xoff vX+slopeOFF


float4 admixC(float4 vX, float4 vE) {
	float mixFactor = mixGate(vX, vE) * (-0.381966) + 1.0;

	return mixXE;
}

float4 admixK(float4 vX, float4 vE) {
    float4 diff = vX - vE;

	float mixFactor = clamp(dot(diff.rgb, diff.rgb), 0.0, 1.0) * 0.5 + 0.5; // xxx.alpha

	return mixXE;
}


//**************************************************************************************************************************************
//* 												main slope + X cross-processing mechanism						                *
//******************************************************************************************************************************** zz  *
float4 admixX( uint A, uint B, uint C, uint D, uint E, uint F, uint G, uint H, uint I
		  , uint P, uint PA, uint PC, uint Q, uint QA, uint QG, uint R, uint RC, uint RI, uint S, uint SG, uint SI, uint AA, uint CC, uint GG
		  , float El, float Bl, float Dl, float Fl, float Hl, bool simEC, bool simEG, bool simEI
		  , float4 vE, float4 vB, float4 vD
		  ) {


	bool doubleWall = eq(B,C) && eq(D,G);

	bool Bisblack = checkblack(vB);
	bool Disblack = checkblack(vD);	//xxx.alpha.duck
	bool triBlack = Bisblack && El >0.5 && (Fl<0.078 || Hl<0.078);
	bool transBD = B==0u||D==0u;	//xxx.alpha.duck
	bool border1dark = ne(B,D) && (Bisblack||Disblack) && E==0u;	//xxx.alpha.duck
	if (doubleWall||triBlack||transBD||border1dark) return slopeBAD;	//xxx.alpha.duck


//* =========================================
//*                    B != D
//* ==================================== zz =
if (ne(B,D)){

	float4 midBD = mix(vB,vD,0.5);		// xxx.alpha

	float4 vX = E==0u ? midBD :		// xxx.alpha.duck
				Bl>Dl ? vB : vD;

	float mixFactor = 0.145898 * mixGate(vX,vE) * float(E!=0u);	//xxx.alpha.duck

	if (eq(E,A)) {

		bool okB = El>Bl && eq(E,C) && eq(A,P) && eq(B,PC);
		bool okD = El>Bl && eq(E,G) && eq(A,Q) && eq(D,QG);

		return mixXEoff +float(!okB&&!okD)*2.0;
	}


    if (not_eq2(A,B,D)){
		bool na1 = all_eq2(A,P,Q);
		bool na2 = eq(A,P) && eq(B,PC) && ne(B,C);
		bool na3 = eq(A,Q) && eq(D,QG) && ne(D,G);

		bool na4 = eq(A,P) && eq(E,G);
		bool na5 = eq(A,Q) && eq(E,C);

		bool goodFH = min(Fl,Hl)>max(Bl,Dl)+0.145898||min(Fl,Hl)>=El;
		bool na6 = eq(E,C) && eq(B,P) && not_eq2(B,PA,PC) && goodFH;
		bool na7 = eq(E,G) && eq(D,Q) && not_eq2(D,QA,QG) && goodFH;

		bool na8 = eq(E,C) && eq(D,G) && ne(D,S) && (ne(D,Q)||ne(D,QG));
		bool na9 = eq(E,G) && eq(B,C) && ne(B,R) && (ne(B,P)||ne(B,PC));

		float4 vA = unpackUnorm4x8(A);
		float Al = luma(vA);

		int BDscore = int(eq(D,Q)&&ne(B,PC)) +int(eq(E,G)) +int(eq(B,C)) -int(eq(B,P)&&ne(D,QG)) -int(eq(D,G)) -int(eq(E,C));

		bool Disbetter = BDscore>0 && (Al>Bl||E==0u);	// xxx.alpha.duck
		bool Bisbetter = BDscore<0 && (Al>Dl||E==0u);	// xxx.alpha.duck

		vX = Disbetter ? vD :
			 Bisbetter ? vB :
						vX ;

		if (na1||na2||na3||na4||na5||na6||na7||na8||na9) return mixXEoff;
	}

	vX = eq(A,D)&&ne(E,G) ?  vB :
		 eq(A,B)&&ne(E,C) ?  vD :
				vX ;

	if (eq(F,H)) {


		bool FH1 = eq(E,C) && ne(D,G) && (ne(E,F)||ne(E,P));
		bool FH2 = eq(E,G) && ne(B,C) && (ne(E,F)||ne(E,Q));

		bool FH3 = ne(E,F) && eq(B,PC) && eq(F,RC);
		bool FH4 = ne(E,F) && eq(D,QG) && eq(H,SG);
		
		if (FH1||FH2||FH3||FH4) return mixXEoff;
	}

	bool ECG1 = all_eq2(E,C,G);
	bool ECG2 = eq(E,C) && eq(B,PC) && ne(B,P);
	bool ECG3 = eq(E,G) && eq(D,QG) && ne(D,Q);

    return mixXEoff +float(!ECG1&&!ECG2&&!ECG3)*2.0;
} //* B != D


						//*********  B == D  *********

	float4 vX = vB;

	float mixFactor = 0.145898 * mixGate(vX,vE) * float(E!=0u);	// xxx.alpha

	bool B_slope;		bool B_tower;		bool B_wall;
	bool D_slope;		bool D_tower;		bool D_wall;

	bool comboE3 = all_eq2(E,C,G);
	bool comboA3 = all_eq2(A,P,Q);

//* ===================================================
//*                   E - A  x cross
//* =========================================== zz ====
if (eq(E,A)) {


	bool Eisblack = checkblack(vE);

    if ( comboE3 && not_eq2(E,F,H) && eq(E,I) ) {

		mixFactor = 0.618034 - 1.618034 * mixFactor;
		return mixXEoff +float(Eisblack)*8.0; 
	}


    if ( comboA3 && eq(A,AA) && not_eq2(A,PA,QA) )  {

		mixFactor = 0.8541 -1.618034*mixFactor -0.236068*float(ne(B,PA)&&eq(PA, QA)) ;
		return mixXEoff +float(Eisblack)*8.0; 
	}



	if ( comboE3 && comboA3 &&
		(eq(B,PC) || eq(D,QG)) && eq(D,QA) && eq(B,PA)) {
		mixFactor = 0.8541 - 1.618034 * mixFactor;
		mixFactor = max(mixFactor, float(E==0u));	// xxx.alpha.duck
        return mixXEoff;
	}

	if ( comboE3 && eq(D,QA) && eq(B,PA)
		&& (  eq(A,P) && eq(D,QG) && eq(E,H)
			|| eq(A,Q) && eq(B,PC) && eq(E,F))
		) {
		mixFactor = 0.8541 - 1.618034 * mixFactor;
		mixFactor = max(mixFactor, float(E==0u));	// xxx.alpha.duck
        return mixXEoff;
		}

	mixFactor *= comboA3 ? 0.145898 : 1.0;

	bool ECG1 = comboA3;

	bool ECG2 = comboE3;

	B_slope = eq(B,PC) && not_eq3(B,P,C,PA);
	D_slope = eq(D,QG) && not_eq3(D,Q,G,QA);


	bool ECG3 = B_slope && eq(E,G);
	bool ECG4 = D_slope && eq(E,C);
	if (ECG1||ECG2||ECG3||ECG4) return mixXEoff;


//* E B D Zone Checkerboard Scoring Rule


	B_wall = eq(B,C) && not_eq3(B,PC,P,PA);
	D_wall = eq(D,G) && not_eq3(D,QG,Q,QA);

	B_tower = eq(B,P) && not_eq3(B,PC,C,PA);
	D_tower = eq(D,Q) && not_eq3(D,QG,G,QA);


//*	E Zone
int scoreE = 0;

	scoreE += int(eq(E,C)) * (
				1 + int(eq(F,H)) + int(eq(B,PC)&&ne(B,PA)) + int(eq(A,Q))
				);

	scoreE += int(eq(E,G)) * (
				1 + int(eq(F,H)) + int(eq(D,QG)&&ne(D,QA)) + int(eq(A,P))
				);


	scoreE += int( (B_slope||B_wall) && eq(A,Q) || (D_slope||D_wall) && eq(A,P) );



    bool En3 = all_eq2(E,F,H);

	bool EisWhite = checkwhite(vE);

	if ( scoreE==0 && !En3 ) {

		bool leftA= eq(A,QA) && not_eq4(A,PA,AA,P,Q);
		bool upA  = eq(A,PA) && not_eq4(A,QA,AA,P,Q);

		bool leftA_leftD = leftA == D_tower;
		bool upA_upB     =   upA == B_tower;

		bool upA_leftD   =   upA == D_tower;
		bool leftA_downD = leftA == D_wall;

		bool EAdarkerBD = Bl > El+0.333 && Bl < 0.92 ||Eisblack||EisWhite;

		bool bad1a = B_wall && eq(E,F) && ne(E,I) && (leftA || leftA_leftD || EAdarkerBD&&ne(A,P));
		bool bad1b = B_wall && D_tower && leftA && (EAdarkerBD&&ne(A,P));
		
		bool bad2a = D_wall && eq(E,H) && ne(E,I) && (upA || upA_upB || EAdarkerBD&&ne(A,Q));
		bool bad2b = D_wall && B_tower && upA  && (EAdarkerBD&&ne(A,Q));

		bool bad3 = B_wall && eq(E,H) && ne(E,I) && (upA || upA_leftD);
		bool bad4 = D_wall && eq(E,F) && ne(E,I) && (leftA || leftA_downD);

		if (bad1a||bad1b||bad2a||bad2b||bad3||bad4) return theEXIT;
    }

	scoreE += int(B_slope && eq(A,P) || D_slope && eq(A,Q));


int scoreZ = 0;

	bool ext_FH = any_eq2(F,RC,SG);

	bool En4 = En3 && eq(E,I);

    if ( !En4 && eq(F,H) ) {

		bool AEIstrong= EisWhite || Bl<0.92 && Fl<0.92 && min(Bl,Fl)-El>0.145898;
		
		bool badcond0 = scoreE==0 && (Eisblack && ne(E,F) || eq(E,I) && AEIstrong && !simEC && !simEG);

		bool badcond1 = En3 && !ext_FH;
		bool badcond2 = eq(F,I) && (Eisblack || EisWhite || !(ext_FH || any_eq2(F,C,R) || any_eq2(H,G,S)));

		bool condZ1 = eq(B,C) && ne(B,P);
		bool condZ2 = eq(D,G) && ne(D,Q);

		scoreZ = int((condZ1 || condZ2) && !badcond0 && !badcond1 && !badcond2);
    }


//*	A Zone

	bool An4 = all_eq3(A,AA,PA,QA);

	scoreZ += int(eq(PA,QA) && !An4 && scoreZ==1);


	bool Bn4 = all_eq3(B,P,PC,C);
	bool Dn4 = all_eq3(D,Q,QG,G);

	int in_B = int(eq(C,P) && !Bn4);
	int in_D = int(eq(G,Q) && !Dn4);
	
//*	B Zone
 int scoreB = 0;

	scoreB -= int(eq(B,PA)) * (
		1 + int(eq(C,P)) + int(eq(A,AA))
		);

	scoreB -= in_B * int(eq(A,AA));


//*  D Zone
 int scoreD = 0; 
 
	scoreD -= int(eq(D,QA)) * (
		1 + int(eq(G,Q)) + int(eq(A,AA))
		);

	scoreD -= in_D * int(eq(A,AA));

	scoreZ = clamp(scoreZ -in_B -in_D, 0, 1);

	int scoreFinal = scoreE + scoreB + scoreD + scoreZ ;
	scoreFinal += int(
				((scoreFinal | scoreB | scoreD) == 0) && 
				!Eisblack && !EisWhite && 
				(B_wall && D_tower || B_tower && D_wall) 
				);

	mixFactor *= scoreFinal>=2 ? 0.145898 : 1.0;
	bool closeB = all_eq4(E,C,F,P,PC) && eq(A,PA) && (ne(D,G) || eq(D,Q));
	bool closeD = all_eq4(E,H,G,Q,QG) && eq(A,QA) && (ne(B,C) || eq(B,P));

	return mixXE +float(scoreFinal<=0)*4.0 +float(closeB||closeD)*8.0;

}	//* E == A

		mixFactor *= comboA3 ? 0.145898 : 1.0;

//* ===============================================
//*              	E - C - G     main rules
//* ========================================= zz ==

    if (eq(E,C) ) {

		bool badC = ne(E,G) && all_eq2(B,A,PA) && all_eq3(E,F,P,PC);

		return mixXE +float(badC)*8.0;
	}

	if (eq(E,G)) {

		bool badC = ne(E,C) && all_eq2(D,A,QA) && all_eq3(E,H,Q,QG);

		return mixXE +float(badC)*8.0;
	}

	mixFactor = E==0u ? 1.0 : mixFactor;		// xxx.alpha // better than Exit

//* =======================================================
//*                  F - H / B+ D+ Extended Rules
//* ================================================= zz ==


	B_slope = eq(B,PC) && ne(B,P) && ne(B,C) && (ne(C,P)||eq(A,P));
	D_slope = eq(D,QG) && ne(D,Q) && ne(D,G) && (ne(G,Q)||eq(A,Q));

	bool B_slopeFoot = B_slope && (ne(B,RC) || eq(C,CC));
	bool D_slopeFoot = D_slope && (ne(D,SG) || eq(G,GG));

	bool B_slopeHead = B_slope && (ne(B,PA) || ne(A,AA));
	bool D_slopeHead = D_slope && (ne(D,QA) || ne(A,AA));


	bool B_slopeL2 = B_slopeHead && B_slopeFoot;
	bool D_slopeL2 = D_slopeHead && D_slopeFoot;
	B_tower = eq(B,P) && ne(B,PC) && ne(B,C);
	D_tower = eq(D,Q) && ne(D,QG) && ne(D,G);

	bool anyWall = eq(B,C) || eq(D,G);
	bool anySlope = eq(B,PC) || eq(D,QG);

	bool anyStickE = eq(E,F)||eq(E,H);
	

    if (ne(A,B)) {

        bool na1 = comboA3 && anySlope;

        bool na2a =  B_slopeL2 && D_slopeL2;
        bool na2b =  B_slope && D_tower;
        bool na2c =  B_tower && D_slope;

		bool na2 = na2a||na2b||na2c;

        bool na3 =  B_slopeFoot && eq(A,P);
        bool na4 =  D_slopeFoot && eq(A,Q);

        bool na5 =  B_slopeFoot && eq(A,Q) && ne(D,G);
        bool na6 =  D_slopeFoot && eq(A,P) && ne(B,C);

        bool na7a =  B_slopeHead && simEC;
        bool na7b =  D_slopeHead && simEG;
        bool na7c =  B_slopeL2 && (simEG && ne(D,G));
        bool na7d =  D_slopeL2 && (simEC && ne(B,C));


		bool na7 = na7a||na7b||na7c||na7d;


		bool na81 =  eq(A,Q) && ne(A,QA) && (simEC && ne(B,C)) && anySlope;
		bool na82 =  eq(A,P) && ne(A,PA) && (simEG && ne(D,G)) && anySlope;
		bool na8 = na81||na82;



        bool na9 =  (B_slopeL2 || D_slopeL2) && eq(F,H);
	

        bool na10 =  B_slopeL2 && eq(H,SG);
        bool na11 =  D_slopeL2 && eq(F,RC);

		mixFactor *= float(!na2);

		if (na1||na2||na3||na4||na5||na6||na7||na8||na9||na10||na11) return mixXE +float(!anyStickE)*2.0;


    }


	float E_lumDiff = mix(0.33, 0.10, clamp((El - 0.8541) * 6.8541, 0.0, 1.0));
	bool aloneE =  mixFactor<0.01 && !simEC && !simEG && !simEI && El-Fl>E_lumDiff && El-Hl>E_lumDiff;

	bool En3 = all_eq2(E,F,H);
	bool ext_FH = eq(F,H) && (eq(F,RC) || eq(H,SG));

	bool badEn3 = En3 && mixFactor<0.01 && !ext_FH && !simEC && !simEG && !simEI;

	bool En4 = En3 && eq(E,I);

	bool aloneEn4 = !ext_FH && !simEC && !simEG && eq(E,S)==eq(E,SI) && eq(E,R)==eq(E,RI);
	bool badEn4a = mixFactor<0.01 && aloneEn4;
	bool badEn4b = (eq(B,C) && eq(B,RC) && eq(E,R) || eq(D,G) && eq(D,SG) && eq(E,S)) && aloneEn4;
	bool badEn4c = eq(A,B) && anyWall;

	bool badEn4 = En4 && (badEn4a||badEn4b||badEn4c);


	if (!anyWall && !aloneE && !badEn4) {

		bool B_hook = eq(B,PA) && not_eq2(A,P,AA);
		bool D_hook = eq(D,QA) && not_eq2(A,Q,AA);

		bool nwall01 = ne(A,B) && (B_slopeL2&&D_hook||B_hook&&D_slopeL2);



		bool goodFH = min(Fl,Hl)>Bl+0.145898||min(Fl,Hl)>=El;
		bool nwall02 = ne(A,B) && B_tower  && goodFH && (ne(E,H)||eq(E,I)||simEC||simEG);
		bool nwall03 = ne(A,B) && D_tower  && goodFH && (ne(E,F)||eq(E,I)||simEC||simEG);


		bool aloneBD =  !anyWall && !anySlope && ne(B,P) && ne(D,Q);
		bool BDfirefly = eq(F,H) && eq(F,I) && El-Fl>0.06 && any_eq2(F,P,Q);
		bool shortFH = eq(F,H) && not_eq2(F,E,I) && !ext_FH;

		bool badBD1 =  aloneBD && (eq(A,B)||BDfirefly);
		bool badBD2 =  aloneBD && (eq(A,B)||ne(F,H)||BDfirefly||shortFH);

		bool nwall1 = comboA3 && eq(F,H) && !badBD1;
		bool nwall4 = comboA3 && ne(B,PA) && eq(PA,QA) && !badBD1;

		bool nwall2 = (comboA3||ne(A,B)&&eq(A,P)) && eq(B,PC) && eq(C,CC);
		bool nwall3 = (comboA3||ne(A,B)&&eq(A,Q)) && eq(D,QG) && eq(G,GG);





		bool nwall7 = eq(A,Q) && (ne(A,AA)||eq(A,B)||eq(A,PA)||eq(PA,QA)) && (ne(B,PA)||eq(B,P)) &&simEC && !badBD1;
		bool nwall8 = eq(A,P) && (ne(A,AA)||eq(A,B)||eq(A,QA)||eq(PA,QA)) && (ne(D,QA)||eq(D,Q)) &&simEG && !badBD1;


		bool nwall9 = (ne(A,B)||eq(A,P)||eq(A,Q)||eq(A,PA)||eq(A,QA)) && simEC && simEG && !badBD2;

		bool nwall10 = ne(A,B) && En4;


		if (nwall01||nwall02||nwall03||nwall1||nwall2||nwall3||nwall4||nwall7||nwall8||nwall9||nwall10)  return mixXEoff;

	}


	bool betterFH = eq(F,H)&&(ne(E,I)||not_eq2(F,RI,SI)) || En4;


	if (ne(A,B) && eq(B,C) && (ne(B,R)||eq(F,RC))) {
		bool bad1 = not_eq3(B,PC,PA,CC) && not_eq3(D,Q,QA,QG) && ne(F,H);
		bool bad2 = eq(H,I) && ne(F,H) && Bl>Hl-0.01;
		bool bad = bad1||bad2||badEn4;

		B_wall = not_eq2(B,P,PC);
		bool longH = B_wall && D_tower;
		bool snake = not_eq2(B,P,PA) && eq(C,CC) && ne(C,RC) && (eq(A,P)||eq(A,Q)||eq(PA,QA)||eq(A,QA)&&eq(D,Q)) && (ne(PC,RC)||eq(F,RC));
		bool good1 = all_eq2(F,I,RC);
		bool good2 = simEG;
		bool good3 = betterFH && (!aloneE && !badEn3 ||longH);
		bool good4 = B_wall && all_eq2(D,Q,QG);
		bool good = snake||good1||good2||good3||good4;
		return mixXE + float(bad||!good)*4.0 +float(!anyStickE)*2.0;
	}

	if (ne(A,B) && eq(D,G) && (ne(D,S)||eq(H,SG))) {

		bool bad1 = not_eq3(D,QG,QA,GG) && not_eq3(B,P,PA,PC) && ne(F,H);
		bool bad2 = eq(F,I) && ne(F,H) && Bl>Fl-0.01;
		bool bad = bad1||bad2||badEn4;
		D_wall = not_eq2(D,Q,QG);
		bool longV = D_wall && B_tower;
		bool snake = not_eq2(D,Q,QA) && eq(G,GG) && ne(G,SG) && (eq(A,P)||eq(A,Q)||eq(PA,QA)||eq(A,PA)&&eq(B,P)) && (ne(QG,SG)||eq(H,SG));
		bool good1 = all_eq2(H,I,SG);
		bool good2 = simEC;
		bool good3 = betterFH && (!aloneE && !badEn3 ||longV);
		bool good4 = D_wall && all_eq2(B,P,PC);
		bool good = snake||good1||good2||good3||good4;
		return mixXE + float(bad||!good)*4.0 +float(!anyStickE)*2.0;
	}

	bool ext1 = ne(A,B) && all_eq2(F,H,RC) && eq(B,PC) && eq(B,P)!=eq(B,C);
	bool ext2 = ne(A,B) && all_eq2(F,H,SG) && eq(D,QG) && eq(D,Q)!=eq(D,G);

	bool sideW1 = ne(A,B) && betterFH && (eq(B,PC) && ne(P,C) && ne(B,RC)) && !aloneE && !badEn3 && !badEn4;
	bool sideW2 = ne(A,B) && betterFH && (eq(D,QG) && ne(Q,G) && ne(D,SG)) && !aloneE && !badEn3 && !badEn4;

	bool lastgood = ext1||ext2||sideW1||sideW2;


	return mixXE +float(!lastgood)*4.0 +float(!anyStickE)*2.0;

}	//* admixX


float4 admixS( uint A, uint B, uint C, uint D, uint E, uint F, uint G, uint H, uint I
		   , uint Q, uint R, uint RC, uint RI, uint S, uint SG, uint SI, uint CC, bool simEC
		   , float4 vE, float4 vF
		   ) {


    bool bad1 = any_eq2(F,C,I);

    bool bad2 =  (eq(F,RI) || eq(G,S) || eq(R, RI)) && ne(R,I);

    bool bad3 = eq(H, S) && not_eq2(H,I,SG);

    bool bad4 =  eq(R, RC) || eq(G,SG);

	bool extC = any_eq2(C,RC,CC);


	#define vX vF
	float mixFactor = 0.381966 * mixGate(vX,vE) * float(E!=0u);	// xxx.alpha.duck

	mixFactor =  (bad1||bad2||bad3||bad4||F==0u) ? 1.0 : mixFactor;	// xxx.alpha.duck

	if ( all_eq2(E,C,D) ) {
		mixFactor = ( !checkwhite(vE) || extC ) ? mixFactor : 1.0;
		return mixXE;
		}

	bool ok1 = eq(E,C) && eq(B,D) && (extC || any_eq2(D,Q,A) ||eq(A,Q));

	bool ok2 = simEC && eq(E,D) && eq(B,C);

	bool ok3 = (simEC || mixFactor>0.01) && all_eq2(B,C,D) && (extC || any_eq2(Q,A,D)) && !all_eq2(E,A,I);

	mixFactor = ok1||ok2||ok3 ? mixFactor : 1.0;
	return mixXE;
}
)"
//**************************************************************************************************zz**
          R"(
void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits, out float4 texcol, out float ialpha)
{

  float2 bcoords = floor(coords);

	float4 vE = SampleFromVRAM(texpage, bcoords, uv_limits);

	float4 vB = srcf(0.0, -1.0);
	float4 vD = srcf(-1.0, 0.0);
	float4 vF = srcf(+1.0, 0.0);
	float4 vH = srcf(0.0, +1.0);

    uint E = packUnorm4x8(vE);
    uint B = packUnorm4x8(vB);
    uint D = packUnorm4x8(vD);
    uint F = packUnorm4x8(vF);
    uint H = packUnorm4x8(vH);

	// default pixel
	ialpha = float(E != 0u);
	texcol = vE;


//* Skip processing for horizontal/vertical 3-pixel identical & mirror symmetric patterns
bool skiprest = all_eq2(E,D,F) || all_eq2(E,B,H) || eq(B,H)&&eq(D,F);
if (!skiprest) {



    // 5x5
	float4 vA = srcf(-1.0, -1.0);
	float4 vC = srcf(+1.0, -1.0);
	float4 vG = srcf(-1.0, +1.0);
	float4 vI = srcf(+1.0, +1.0);


	uint A = packUnorm4x8(vA);
	uint C = packUnorm4x8(vC);
	uint G = packUnorm4x8(vG);
	uint I = packUnorm4x8(vI);

	bool simEA = sim(vE,vA) && ((E==0u)==(A==0u));	//* xxx.alpha.duck
	bool simEC = sim(vE,vC) && ((E==0u)==(C==0u));
	bool simEG = sim(vE,vG) && ((E==0u)==(G==0u));
	bool simEI = sim(vE,vI) && ((E==0u)==(I==0u));

	uint P  = src( 0.0, -2.0);
	uint Q  = src(-2.0,  0.0);
	uint R  = src(+2.0,  0.0);
	uint S  = src( 0.0, +2.0);

    float Bl = luma(vB) + float(B==0u);	// xxx.alpha.duck
    float Dl = luma(vD) + float(D==0u);
    float El = luma(vE) + float(E==0u);
    float Fl = luma(vF) + float(F==0u);
    float Hl = luma(vH) + float(H==0u);

    float4 J = vE;    float4 K = vE;    float4 L = vE;    float4 M = vE;


if (ne(B,H) && ne(D,F)) {


	uint PA = src(-1.0, -2.0);
	uint PC = src(+1.0, -2.0);
	uint QA = src(-2.0, -1.0);
	uint QG = src(-2.0, +1.0); //*             AA   PA   [P]  PC   CC
	uint RC = src(+2.0, -1.0); //*               -|----|----|----|-
	uint RI = src(+2.0, +1.0); //*             QA |  A |  B | C  | RC
	uint SG = src(-1.0, +2.0); //*               -|----|----|----|-
	uint SI = src(+1.0, +2.0); //*            [Q] |  D |  E | F  | [R]
	uint AA = src(-2.0, -2.0); //*               -|----|----|----|-
	uint CC = src(+2.0, -2.0); //*             QG |  G |  H | I  | RI
	uint GG = src(-2.0, +2.0); //*               -|----|----|----|-
	uint II = src(+2.0, +2.0); //*             GG   SG   [S]  SI   II



//*	B - D
	bool slope1 = ( ne(D,H) && ne(B,F) &&
					(eq(E,A)&&El<min(Bl,Dl)||El>max(Bl,Dl)) && (not_eq2(A,B,D) || ne(E,P) || ne(E,Q)) &&
					( eq(B,D) &&(eq(E,A)||any_eq3(B,P,C,PC)||any_eq3(D,Q,G,QG)||simEC||simEG) || simb(vB,vD)&&(eq(F,H)||any_eq2(E,C,G)) )
				);
//*	B - F
	bool slope2 = ( ne(B,D) && ne(F,H) &&
					(eq(E,C)&&El<min(Bl,Fl)||El>max(Bl,Fl)) && (not_eq2(C,B,F) || ne(E,P) || ne(E,R)) &&
					( eq(B,F)&&(eq(E,C)||any_eq3(B,P,A,PA)||any_eq3(F,R,I,RI)||simEI||simEA) || simb(vB,vF)&&(eq(D,H)||any_eq2(E,A,I)) )
				);
//*	D - H
	bool slope3 = ( ne(F,H) && ne(B,D) &&
					(eq(E,G)&&El<min(Dl,Hl)||El>max(Dl,Hl))  &&  (not_eq2(G,D,H) || ne(E,S) || ne(E,Q)) &&
					( eq(D,H) &&(eq(E,G)||any_eq3(D,A,Q,QA)||any_eq3(H,S,I,SI)||simEA||simEI) || simb(vD,vH)&&(eq(B,F)||any_eq2(E,A,I)) )
				);
//*	F - H
	bool slope4 = ( ne(B,F) && ne(D,H) &&
					(eq(E,I)&&El<min(Fl,Hl)||El>max(Fl,Hl))  &&  (not_eq2(I,F,H) || ne(E,R) || ne(E,S)) &&
					( eq(F,H) &&(eq(E,I)||any_eq3(F,R,C,RC)||any_eq3(H,S,G,SG)||simEG||simEC) || simb(vF,vH)&&(eq(B,D)||any_eq2(E,C,G)) )
				);

    bool slope1end = false;  bool slope2end = false;  bool slope3end = false;  bool slope4end = false;

	if (slope1) {
		J=admixX(A,B,C,D,E,F,G,H,I
				,P,PA,PC,Q,QA,QG,R,RC,RI,S,SG,SI,AA,CC,GG
				,El,Bl,Dl,Fl,Hl, simEC,simEG,simEI
				,vE,vB,vD
				);
		bool slope1ok = (J.b < 1.01) && max(sign(El-Dl)*(Fl-El),sign(El-Dl)*(Hl-El))>-0.1;
		bool isCopy=checkeq(J,vD);
		float4 mixL2 = mix(J,vE,0.145898);
		bool longH = slope1ok && all_eq2(D,C,Q) && ne(D,QG) && (ne(D,R)||eq(D,RI)) && (ne(D,QA)||eq(D,A));
		bool longV = slope1ok && all_eq2(D,G,P) && ne(D,PC) && (ne(D,S)||eq(D,SI)) && (ne(D,PA)||eq(D,A));
		K= longH ? isCopy ? J : mixL2 : K;
		L= longV ? isCopy ? J : mixL2 : L;
		slope1end = (J.b < 3.99);

		J = (J.b > 3.99) ? vE :
			(J.b > 1.01) ? (J - 2.0) :
			J;
	}

	if (slope2) {
		K=admixX(C,F,I,B,E,H,A,D,G
				,R,RC,RI,P,PC,PA,S,SI,SG,Q,QA,QG,CC,II,AA
				,El,Fl,Bl,Hl,Dl, simEI,simEA,simEG
				,vE,vF,vB
				);

		bool slope2ok = K.b<1.01 && max(sign(El-Fl)*(Dl-El),sign(El-Fl)*(Hl-El))>-0.1;
		bool isCopy=checkeq(K,vF);
		float4 mixL2 = mix(K,vE,0.145898);
		bool longH = slope2ok && all_eq2(F,A,R) && ne(F,RI) && (ne(F,RC)||eq(F,C)) && (ne(F,Q)||eq(F,QG));
		bool longV = slope2ok && all_eq2(F,I,P) && ne(F,PA) && (ne(F,PC)||eq(F,C)) && (ne(F,S)||eq(F,SG));
		J= longH ? isCopy ? K : mixL2 : J;
		M= longV ? isCopy ? K : mixL2 : M;
		slope2end = (K.b < 3.99);

		K = (K.b > 3.99) ? vE :
			(K.b > 1.01) ? (K - 2.0) :
			K;
	}

	if (slope3) {
		L=admixX(G,D,A,H,E,B,I,F,C
				,Q,QG,QA,S,SG,SI,P,PA,PC,R,RI,RC,GG,AA,II
				,El,Dl,Hl,Bl,Fl, simEA,simEI,simEC
				,vE,vD,vH
				);

		bool slope3ok = (L.b < 1.01) && max(sign(El-Dl)*(Bl-El),sign(El-Dl)*(Fl-El))>-0.1;
		bool isCopy=checkeq(L,vD);
		float4 mixL2 = mix(L,vE,0.145898);
		bool longH = slope3ok && all_eq2(D,I,Q) && ne(D,QA) && (ne(D,QG)||eq(D,G)) && (ne(D,R)||eq(D,RC));
		bool longV = slope3ok && all_eq2(D,A,S) && ne(D,SI) && (ne(D,SG)||eq(D,G)) && (ne(D,P)||eq(D,PC));
		M= longH ? isCopy ? L : mixL2 : M;
		J= longV ? isCopy ? L : mixL2 : J;

		slope3end = (L.b < 3.99);
		L = (L.b > 3.99) ? vE :
			(L.b > 1.01) ? (L - 2.0) :
			L;
	}

	if (slope4) {
		M=admixX(I,H,G,F,E,D,C,B,A
				,S,SI,SG,R,RI,RC,Q,QG,QA,P,PC,PA,II,GG,CC
				,El,Hl,Fl,Dl,Bl, simEG,simEC,simEA
				,vE,vH,vF
				);

		bool slope4ok = (M.b < 1.01) && max(sign(El-Fl)*(Bl-El), sign(El-Fl)*(Dl-El))>-0.1;
		bool isCopy=checkeq(M,vF);
		float4 mixL2 = mix(M,vE,0.145898);
		bool longH = slope4ok && all_eq2(F,G,R) && ne(F,RC) && (ne(F,RI)||eq(F,I)) && (ne(F,Q)||eq(F,QA));
		bool longV = slope4ok && all_eq2(F,C,S) && ne(F,SG) && (ne(F,SI)||eq(F,I)) && (ne(F,P)||eq(F,PA));
		L= longH ? isCopy ? M : mixL2 : L;
		K= longV ? isCopy ? M : mixL2 : K;

		slope4end = (M.b < 3.99);
		M = (M.b > 3.99) ? vE :
			(M.b > 1.01) ? (M - 2.0) :
			M;
	}

skiprest = slope1||slope2||slope3||slope4;



//* =========================  2:1 staggered slope  (new rule)  ===========================

    bool bottonH = not_eq5(H,E,A,C,D,F);

		if (bottonH && !slope4end && !slope2 && !slope3 && ne(B,F) &&
            ne(E,F) && eq(H,R) && eq(F,G) ) {
            M = admixS( A, B, C, D, E, F, G, H, I
                      , Q, R, RC, RI, S, SG, SI, CC, simEC
                      , vE, vF
                      );
            skiprest = true;}

        if (bottonH && !slope3end && !slope1 && !slope4 && ne(B,D) &&
             ne(E,D) && eq(H,Q) && eq(D,I) ) {
            L = admixS( C, B, A, F, E, D, I, H, G
                      , R, Q, QA, QG, S, SI, SG, AA, simEA
                      , vE, vD
                      );
            skiprest = true;}


    bool topH = not_eq5(B,E,G,I,D,F);

        if (topH && !slope2end && !slope1 && !slope4 && ne(F,H) &&
              ne(E,F) && eq(B,R) && eq(A,F) ) {
            K = admixS( G, H, I, D, E, F, A, B, C
                      , Q, R, RI, RC, P, PA, PC, II, simEI
                      , vE, vF
                      );
            skiprest = true;}

        if (topH && !slope1end && !slope2 && !slope3 && ne(D,H) &&
             ne(E,D) && eq(B,Q) && eq(C,D) ) {
            J = admixS( I, H, G, F, E, D, C, B, A
                      , R, Q, QG, QA, P, PC, PA, GG, simEG
                      , vE, vD
                      );
            skiprest = true;}


    bool leftV = not_eq5(D,E,C,I,B,H);

        if (leftV && !slope3end && !slope1 && !slope4 && ne(F,H) &&
              ne(E,H) && eq(D,S) && eq(A,H) ) {
            L = admixS( C, F, I, B, E, H, A, D, G
                      , P, S, SI, SG, Q, QA, QG, II, simEI
                      , vE, vH
                      );
            skiprest = true;}

        if (leftV && !slope1end && !slope2 && !slope3 && ne(B,F) &&
              ne(E,B) && eq(D,P) && eq(B,G) ) {
            J = admixS( I, F, C, H, E, B, G, D, A
                      , S, P, PC, PA, Q, QG, QA, CC, simEC
                      , vE, vB
                      );
            skiprest = true;}



    bool rightV = not_eq5(F,E,A,G,B,H);

        if (rightV && !slope4end && !slope2 && !slope3 && ne(D,H) &&
              ne(E,H) && eq(F,S) && eq(H,C) ) {
            M = admixS( A, D, G, B, E, H, C, F, I
                      , P, S, SG, SI, R, RC, RI, GG, simEG
                      , vE, vH
                      );
            skiprest = true;}

        if (rightV && !slope2end && !slope1 && !slope4 && ne(B,D) &&
             ne(E,B) && eq(F,P) && eq(B,I) ) {
            K = admixS( G, D, A, H, E, B, I, F, C
                      , S, P, PA, PC, R, RI, RC, AA, simEA
                      , vE, vB
                      );
            skiprest = true;}

}

skiprest = skiprest||E==0u||B==0u||D==0u||F==0u||H==0u;	// xxx.alpha

if (!skiprest) {

//* =================== Concave+Cross pattern (P100) =================

 //* top [B]
    if( Bl<El && not_eq4(E,A,C,D,F) && all_eq4(E,G,H,I,S) ) {

	bool DD = eq(A, D);
	bool FF = eq(C, F);

	Bl = any_eq2(B, A, C) ? -1.0 : Bl;

	float tinS = eq(D,F) && DD!=FF ? 2.0 : 0.0;

	float brightLoss = float(ne(D,F)) + abs(Dl - El) * 1.618034;
	
	Dl = Dl - step(El,Dl)*brightLoss - float(DD) + tinS;
	Fl = Fl - step(El,Fl)*brightLoss - float(FF) + tinS;

	float maxl = max(Bl, max(Dl, Fl));

	float4 vT =	any_eq2(B,D,F) ? vB :
			maxl <= 0.0 ? vE :
			maxl <= Dl ? vD :
			maxl <= Fl ? vF :
						vB ;
	vT = admixC(vT,vE);

	bool nolink = ne(D,F) && !DD && !FF;

	J = Dl<0.0&&nolink ? vE : vT;
    K = Fl<0.0&&nolink ? vE : vT;
}

 //* bottom (H)
if ( Hl<El && not_eq4(E,G,I,D,F) && all_eq4(E,A,B,C,P) ) {

	bool DD = eq(D, G);
	bool FF = eq(F, I);

	Hl = any_eq2(H, G, I) ? -1.0 : Hl;

	float tinS = eq(D,F) && DD!=FF ? 2.0 : 0.0;

	float brightLoss = float(ne(D,F)) + abs(Dl - El) * 1.618034;

	Dl = Dl - step(El,Dl)*brightLoss - float(DD) + tinS;
	Fl = Fl - step(El,Fl)*brightLoss - float(FF) + tinS;

	float maxl = max(Hl, max(Dl, Fl));
	
	float4 vT = any_eq2(H,D,F) ? vH :
			maxl <= 0.0 ? vE :
			maxl <= Dl ? vD :
			maxl <= Fl ? vF :
						vH ;
	vT=admixC(vT,vE);

	bool nolink = ne(D,F) && !DD && !FF;

	L = Dl<0.0&&nolink ? vE : vT;
    M = Fl<0.0&&nolink ? vE : vT;
}

 //* right [F]
if ( Fl<El && not_eq4(E,B,C,H,I) && all_eq4(E,A,D,G,Q) ) {

	bool BB = eq(B, C);
	bool HH = eq(H, I);

	Fl = any_eq2(F, C, I) ? -1.0 : Fl;

	float tinS = eq(B,H) && BB!=HH ? 2.0 : 0.0;

	float brightLoss = float(ne(B,H)) + abs(Bl - El) * 1.618034;

	Bl = Bl - step(El,Bl)*brightLoss - float(BB) + tinS;
	Hl = Hl - step(El,Hl)*brightLoss - float(HH) + tinS;

	float maxl = max(Fl, max(Bl, Hl));

	float4 vT =any_eq2(F,B,H) ? vF :
			maxl <= 0.0 ? vE :
			maxl <= Bl ? vB :
			maxl <= Hl ? vH :
						vF ;
	vT=admixC(vT,vE);

	bool nolink = ne(B,H) && !BB && !HH;

	K = Bl<0.0&&nolink ? vE : vT;
    M = Hl<0.0&&nolink ? vE : vT;
}

 //* left [D]
if ( Dl<El && not_eq4(E,A,B,G,H) && all_eq4(E,C,F,I,R) ) {

	bool BB = eq(B, A);
	bool HH = eq(H, G);

	Dl = any_eq2(D, A, G) ? -1.0 : Dl;

	float tinS = eq(B,H) && BB!=HH ? 2.0 : 0.0;

	float brightLoss = float(ne(B,H)) + abs(Bl - El) * 1.618034;

	Bl = Bl - step(El,Bl)*brightLoss - float(BB) + tinS;
	Hl = Hl - step(El,Hl)*brightLoss - float(HH) + tinS;

	float maxl = max(Dl, max(Bl, Hl));

	float4 vT =any_eq2(D,B,H) ? vD :
			maxl <= 0.0 ? vE :
			maxl <= Bl ? vB :
			maxl <= Hl ? vH :
						vD ;
	vT=admixC(vT,vE);

	bool nolink = ne(B,H) && !BB && !HH;

	J = Bl<0.0&&nolink ? vE : vT;
    L = Hl<0.0&&nolink ? vE : vT;
}

//* ===================  SKorpion pattern (P99) ==================


if (ne(E,F) && all_eq2(B,F,H) && all_eq3(E,D,C,I) && (eq(E,Q)||El>Fl) && ne(F,src(+3.0, 0.0)) ) {K=admixK(vF,vE); M=K;}	//* RIGHT
if (ne(E,D) && all_eq2(B,D,H) && all_eq3(E,F,A,G) && (eq(E,R)||El>Dl) && ne(D,src(-3.0, 0.0)) ) {J=admixK(vD,vE); L=J;}	//* LEFT
if (ne(E,H) && all_eq2(D,F,H) && all_eq3(E,B,G,I) && (eq(E,P)||El>Hl) && ne(H,src(0.0, +3.0)) ) {L=admixK(vH,vE); M=L;}	//* BOTTOM
if (ne(E,B) && all_eq2(B,D,F) && all_eq3(E,H,A,C) && (eq(E,S)||El>Bl) && ne(B,src(0.0, -3.0)) ) {J=admixK(vB,vE); K=J;}	//* TOP

}

	//final write
	float2 fpart = frac(coords);

	float4 res = (fpart.x < 0.5) ? ((fpart.y < 0.5) ? J : L) : ((fpart.y < 0.5) ? K : M);

	ialpha = checknonzero(res);
	texcol = res;
}
	
}

#undef src
)";
  }
  else if (texture_filter == GPUTextureFilter::Scale2x)
  {
    // Based on https://www.scale2x.it/algorithm
    ss << R"(
#define src(xoffs, yoffs) packUnorm4x8(SampleFromVRAM(texpage, bcoords + float2((xoffs), (yoffs)), uv_limits))

void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits, out float4 texcol, out float ialpha)
{
	float2 bcoords = floor(coords);

	uint E = src(+0, +0);
	uint B = src(+0, - 1);
	uint D = src(-1, +0);
	uint F = src(+1, +0);
	uint H = src(+0, +1);

	uint J = (D == B && B != F && D != H) ? D : E;
	uint K = (B == F && D != F && H != F) ? F : E;
	uint L = (H == D && F != D && B != D) ? D : E;
	uint M = (H == F && D != H && B != F) ? F : E;

	// select quadrant based on fractional part of texture coordinates
	float2 fpart = frac(coords);
	uint res = (fpart.x < 0.5f) ? ((fpart.y < 0.5f) ? J : L) : ((fpart.y < 0.5f) ? K : M);

	ialpha = float(res != 0u);
	texcol = unpackUnorm4x8(res);
}

#undef src
)";
  }
  else if (texture_filter == GPUTextureFilter::Scale3x)
  {
    // Based on https://www.scale2x.it/algorithm
    ss << R"(
#define src(xoffs, yoffs) packUnorm4x8(SampleFromVRAM(texpage, bcoords + float2((xoffs), (yoffs)), uv_limits))

void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits, out float4 texcol, out float ialpha)
{
	float2 bcoords = floor(coords);

	uint E = src(+0, +0);
	uint B = src(+0, -1);
	uint D = src(-1, +0);
	uint F = src(+1, +0);
	uint H = src(+0, +1);

	uint res = E;
	if (B != H && D != F) {
		uint A = src(-1, -1);
		uint C = src(+1, -1);
		uint G = src(-1, +1);
		uint I = src(+1, +1);

		uint E0 = (D == B) ? D : E;
		uint E1 = (D == B && E != C) || (B == F && E != A) ? B : E;
		uint E2 = (B == F) ? F : E;
		uint E3 = (D == B && E != G) || (D == H && E != A) ? D : E;
		uint E4 = E;
		uint E5 = (B == F && E != I) || (H == F && E != C) ? F : E;
		uint E6 = (D == H) ? D : E;
		uint E7 = (D == H && E != I) || (H == F && E != G) ? H : E;
		uint E8 = (H == F) ? F : E;

		// select quadrant based on fractional part of texture coordinates
		float2 fpart = frac(coords);
		uint R0, R1, R2;
		if (fpart.y < 0.34f) {
			R0 = E0;
			R1 = E1;
			R2 = E2;
		} else if (fpart.y < 0.67f) {
			R0 = E3;
			R1 = E4;
			R2 = E5;
		} else {
			R0 = E6;
			R1 = E7;
			R2 = E8;
		}

		res = (fpart.x < 0.34f) ? R0 : ((fpart.x < 0.67f) ? R1 : R2);
	}

	ialpha = float(res != 0u);
	texcol = unpackUnorm4x8(res);
}

#undef src
)";
  }
}

std::string GPU_HW_ShaderGen::GenerateBatchFragmentShader(const BatchFragmentShaderSelector sel) const
{
  DebugAssert(!sel.true_color || !sel.dithering); // Should not be doing dithering+true color.

  DebugAssert(sel.transparency == GPUTransparencyMode::Disabled ||
              sel.render_mode == GPU_HW::BatchRenderMode::ShaderBlend);
  DebugAssert((!sel.rov_depth_test && !sel.rov_depth_write) || (sel.use_rov && sel.use_rov_depth));

  const bool textured = (sel.texture_mode != GPU_HW::BatchTextureMode::Disabled);
  const bool palette = (sel.texture_mode == GPU_HW::BatchTextureMode::Palette4Bit ||
                        sel.texture_mode == GPU_HW::BatchTextureMode::Palette8Bit);
  const bool page_texture = (sel.texture_mode == GPU_HW::BatchTextureMode::PageTexture);
  const bool shader_blending = (sel.render_mode == GPU_HW::BatchRenderMode::ShaderBlend);
  const bool use_dual_source = (!shader_blending && !sel.use_rov && m_supports_dual_source_blend &&
                                ((sel.render_mode != GPU_HW::BatchRenderMode::TransparencyDisabled &&
                                  sel.render_mode != GPU_HW::BatchRenderMode::OnlyOpaque) ||
                                 sel.is_blended_texture_filtering));

  std::stringstream ss;
  WriteHeader(ss, sel.use_rov, shader_blending && !sel.use_rov, use_dual_source);
  DefineMacro(ss, "TRANSPARENCY", sel.render_mode != GPU_HW::BatchRenderMode::TransparencyDisabled);
  DefineMacro(ss, "TRANSPARENCY_ONLY_OPAQUE", sel.render_mode == GPU_HW::BatchRenderMode::OnlyOpaque);
  DefineMacro(ss, "TRANSPARENCY_ONLY_TRANSPARENT", sel.render_mode == GPU_HW::BatchRenderMode::OnlyTransparent);
  DefineMacro(ss, "TRANSPARENCY_MODE", static_cast<s32>(sel.transparency));
  DefineMacro(ss, "SHADER_BLENDING", shader_blending);
  DefineMacro(ss, "CHECK_MASK_BIT", sel.check_mask);
  DefineMacro(ss, "TEXTURED", textured);
  DefineMacro(ss, "PALETTE", palette);
  DefineMacro(ss, "PALETTE_4_BIT", sel.texture_mode == GPU_HW::BatchTextureMode::Palette4Bit);
  DefineMacro(ss, "PALETTE_8_BIT", sel.texture_mode == GPU_HW::BatchTextureMode::Palette8Bit);
  DefineMacro(ss, "PAGE_TEXTURE", page_texture);
  DefineMacro(ss, "DITHERING", sel.dithering);
  DefineMacro(ss, "DITHERING_SCALED", sel.dithering && sel.scaled_dithering);
  DefineMacro(ss, "INTERLACING", sel.interlacing);
  DefineMacro(ss, "INTERLACING_SCALED", sel.interlacing && sel.scaled_interlacing);
  DefineMacro(ss, "MODULATION_CROP", sel.modulation_crop);
  DefineMacro(ss, "TRUE_COLOR", sel.true_color);
  DefineMacro(ss, "TEXTURE_FILTERING", sel.texture_filtering != GPUTextureFilter::Nearest);
  DefineMacro(ss, "TEXTURE_ALPHA_BLENDING", sel.is_blended_texture_filtering);
  DefineMacro(ss, "UV_LIMITS", sel.uv_limits);
  DefineMacro(ss, "USE_ROV", sel.use_rov);
  DefineMacro(ss, "USE_ROV_DEPTH", sel.use_rov_depth);
  DefineMacro(ss, "ROV_DEPTH_TEST", sel.rov_depth_test);
  DefineMacro(ss, "ROV_DEPTH_WRITE", sel.rov_depth_write);
  DefineMacro(ss, "USE_DUAL_SOURCE", use_dual_source);
  DefineMacro(ss, "WRITE_MASK_AS_DEPTH", sel.write_mask_as_depth);
  DefineMacro(ss, "FORCE_ROUND_TEXCOORDS", sel.force_round_texcoords);
  DefineMacro(ss, "DISABLE_UPSCALED_DIRECT_TEXTURES", sel.disable_upscaled_direct_textures);
  DefineMacro(ss, "UPSCALED", sel.upscaled);

  // Used for converting to normalized coordinates for sampling.
  ss << "CONSTANT float2 RCP_VRAM_SIZE = float2(1.0 / float(" << VRAM_WIDTH << "), 1.0 / float(" << VRAM_HEIGHT
     << "));\n";

  WriteColorConversionFunctions(ss);
  WriteBatchUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0);

  if (sel.use_rov)
  {
    DeclareImage(ss, "rov_color", 0, "unorm float4", "rgba8");
    if (sel.use_rov_depth)
      DeclareImage(ss, "rov_depth", 1, "float", "r32f");
  }

  if (m_glsl)
    ss << "CONSTANT int[16] s_dither_values = int[16]( ";
  else
    ss << "CONSTANT int s_dither_values[] = {";
  for (u32 i = 0; i < 16; i++)
  {
    if (i > 0)
      ss << ", ";
    ss << DITHER_MATRIX[i / 4][i % 4];
  }
  if (m_glsl)
    ss << " );\n";
  else
    ss << "};\n";

  ss << R"(
uint3 ApplyDithering(uint2 coord, uint3 icol)
{
  #if (DITHERING_SCALED != 0 || UPSCALED == 0)
    uint2 fc = coord & uint2(3u, 3u);
  #else
    uint2 fc = uint2(float2(coord) * u_rcp_resolution_scale) & uint2(3u, 3u);
  #endif
  int offset = s_dither_values[fc.y * 4u + fc.x];
  return uint3(clamp((int3(icol) + offset) >> 3, 0, 31));
}

#if TEXTURED
CONSTANT float4 TRANSPARENT_PIXEL_COLOR = float4(0.0, 0.0, 0.0, 0.0);

#if PALETTE
  #define TEXPAGE_VALUE uint4
#else
  #define TEXPAGE_VALUE uint2
#endif

#if UV_LIMITS
  #define DECLARE_UV_LIMITS(coords, uv_limits) coords, uv_limits
  #define APPLY_UV_LIMITS(coords, uv_limits) clamp((coords), uv_limits.xy, uv_limits.zw)
#else
  #define DECLARE_UV_LIMITS(coords, uv_limits) coords
  #define APPLY_UV_LIMITS(coords, uv_limits) (coords)
#endif

uint2 ApplyTextureWindow(uint2 coords)
{
  uint x = (uint(coords.x) & u_texture_window_and.x) | u_texture_window_or.x;
  uint y = (uint(coords.y) & u_texture_window_and.y) | u_texture_window_or.y;
  return uint2(x, y);
}

uint2 FloatToIntegerCoords(DECLARE_UV_LIMITS(float2 coords, float4 uv_limits))
{
  // With the vertex offset applied at 1x resolution scale, we want to round the texture coordinates.
  // Floor them otherwise, as it currently breaks when upscaling as the vertex offset is not applied.
  // Apply UV limits in the opposite order, because flooring will never round up, whereas rounding can.
#if UPSCALED == 0 || FORCE_ROUND_TEXCOORDS != 0
  float2 rounded_coords = roundEven(coords);
  float2 clamped_coords = APPLY_UV_LIMITS(rounded_coords, uv_limits);
  return uint2(clamped_coords);
#else
  float2 clamped_coords = APPLY_UV_LIMITS(coords, uv_limits);
  float2 floored_coords = floor(clamped_coords);
  return uint2(floored_coords);
#endif
}

#if PAGE_TEXTURE

float4 SampleFromPageTexture(DECLARE_UV_LIMITS(float2 coords, float4 uv_limits))
{
  // Cached textures.
  uint2 icoord = ApplyTextureWindow(FloatToIntegerCoords(DECLARE_UV_LIMITS(coords, uv_limits)));
#if UPSCALED
  float2 fpart = frac(coords);
  coords = (float2(icoord) + fpart);
#else
  // Drop fractional part.
  coords = float2(icoord);
#endif

  // Normalize.
  coords = coords * (1.0f / 256.0f);
  return SAMPLE_TEXTURE(samp0, coords);
}

#endif

#if !PAGE_TEXTURE || TEXTURE_FILTERING

float4 SampleFromVRAM(TEXPAGE_VALUE texpage, DECLARE_UV_LIMITS(float2 coords, float4 uv_limits))
{
  #if PAGE_TEXTURE
    return SampleFromPageTexture(DECLARE_UV_LIMITS(coords, uv_limits));
  #elif PALETTE
    uint2 icoord = ApplyTextureWindow(FloatToIntegerCoords(DECLARE_UV_LIMITS(coords, uv_limits)));

    uint2 vicoord;
    #if PALETTE_4_BIT
      // 4bit will never wrap, since it's in the last texpage row.
      vicoord = uint2(texpage.x + (icoord.x / 4u), texpage.y + icoord.y);
    #elif PALETTE_8_BIT
      // 8bit can wrap in the X direction.
      vicoord = uint2((texpage.x + (icoord.x / 2u)) & 0x3FFu, texpage.y + icoord.y);
    #endif

    // load colour/palette
    // use texelFetch()/load for native resolution to work around point sampling precision
    // in some drivers, such as older AMD and Mali Midgard
    #if !UPSCALED
      float4 texel = LOAD_TEXTURE(samp0, int2(vicoord), 0);
    #else
      float4 texel = SAMPLE_TEXTURE_LEVEL(samp0, float2(vicoord) * RCP_VRAM_SIZE, 0.0);
    #endif
    uint vram_value = RGBA8ToRGBA5551(texel);

    // apply palette
    #if PALETTE_4_BIT
      uint subpixel = icoord.x & 3u;
      uint palette_index = (vram_value >> (subpixel * 4u)) & 0x0Fu;
      uint2 palette_icoord = uint2((texpage.z + palette_index), texpage.w);
    #elif PALETTE_8_BIT
      // can only wrap in X direction for 8-bit, 4-bit will fit in texpage size.
      uint subpixel = icoord.x & 1u;
      uint palette_index = (vram_value >> (subpixel * 8u)) & 0xFFu;
      uint2 palette_icoord = uint2(((texpage.z + palette_index) & 0x3FFu), texpage.w);
    #endif

    #if !UPSCALED
      return LOAD_TEXTURE(samp0, int2(palette_icoord), 0);
    #else
      return SAMPLE_TEXTURE_LEVEL(samp0, float2(palette_icoord) * RCP_VRAM_SIZE, 0.0);
    #endif
  #else
    // Direct texturing - usually render-to-texture effects.
    #if !UPSCALED
      uint2 icoord = ApplyTextureWindow(FloatToIntegerCoords(DECLARE_UV_LIMITS(coords, uv_limits)));
      uint2 vicoord = (texpage.xy + icoord) & uint2(1023, 511);
      return LOAD_TEXTURE(samp0, int2(vicoord), 0);
    #elif DISABLE_UPSCALED_DIRECT_TEXTURES
      // Treat direct textures as native resolution, so filtering offsets move to adjacent native texels instead of
      // duplicated upscaled texels.
      uint2 icoord = ApplyTextureWindow(FloatToIntegerCoords(DECLARE_UV_LIMITS(coords, uv_limits)));
      uint2 vicoord = (texpage.xy + icoord) & uint2(1023, 511);
      return SAMPLE_TEXTURE_LEVEL(samp0, float2(vicoord) * RCP_VRAM_SIZE, 0.0);
    #else
      // Coordinates are already upscaled, we need to downscale them to apply the texture
      // window, then re-upscale/offset. We can't round here, because it could result in
      // going outside of the texture window.
      float2 ncoords = APPLY_UV_LIMITS(coords, uv_limits) * u_rcp_resolution_scale;
      float2 nfpart = frac(ncoords);
      uint2 nicoord = ApplyTextureWindow(uint2(floor(ncoords)));
      uint2 nvicoord = (texpage.xy + nicoord) & uint2(1023, 511);
      ncoords = (float2(nvicoord) + nfpart);
      return SAMPLE_TEXTURE_LEVEL(samp0, ncoords * RCP_VRAM_SIZE, 0.0);
    #endif
  #endif
}

#endif // !PAGE_TEXTURE || TEXTURE_FILTERING

#endif // TEXTURED
)";

  const u32 num_fragment_outputs = sel.use_rov ? 0 : (use_dual_source ? 2 : 1);
  if (textured && page_texture)
  {
    if (sel.texture_filtering != GPUTextureFilter::Nearest)
      WriteBatchTextureFilter(ss, sel.texture_filtering);

    if (sel.uv_limits)
    {
      DeclareFragmentEntryPoint(ss, 1, 1, {{"nointerpolation", "float4 v_uv_limits"}}, true, num_fragment_outputs,
                                use_dual_source, sel.write_mask_as_depth, sel.msaa, sel.per_sample_shading, false,
                                sel.disable_color_perspective, shader_blending && !sel.use_rov, sel.use_rov);
    }
    else
    {
      DeclareFragmentEntryPoint(ss, 1, 1, {}, true, num_fragment_outputs, use_dual_source, sel.write_mask_as_depth,
                                sel.msaa, sel.per_sample_shading, false, sel.disable_color_perspective,
                                shader_blending && !sel.use_rov, sel.use_rov);
    }
  }
  else if (textured)
  {
    if (sel.texture_filtering != GPUTextureFilter::Nearest)
      WriteBatchTextureFilter(ss, sel.texture_filtering);

    if (sel.uv_limits)
    {
      DeclareFragmentEntryPoint(ss, 1, 1,
                                {{"nointerpolation", palette ? "uint4 v_texpage" : "uint2 v_texpage"},
                                 {"nointerpolation", "float4 v_uv_limits"}},
                                true, num_fragment_outputs, use_dual_source, sel.write_mask_as_depth, sel.msaa,
                                sel.per_sample_shading, false, sel.disable_color_perspective,
                                shader_blending && !sel.use_rov, sel.use_rov);
    }
    else
    {
      DeclareFragmentEntryPoint(ss, 1, 1, {{"nointerpolation", palette ? "uint4 v_texpage" : "uint2 v_texpage"}}, true,
                                num_fragment_outputs, use_dual_source, sel.write_mask_as_depth, sel.msaa,
                                sel.per_sample_shading, false, sel.disable_color_perspective,
                                shader_blending && !sel.use_rov, sel.use_rov);
    }
  }
  else
  {
    DeclareFragmentEntryPoint(ss, 1, 0, {}, true, num_fragment_outputs, use_dual_source, sel.write_mask_as_depth,
                              sel.msaa, sel.per_sample_shading, false, sel.disable_color_perspective,
                              shader_blending && !sel.use_rov, sel.use_rov);
  }

  ss << R"(
{
  uint3 vertcol = uint3(v_col0.rgb * float3(255.0, 255.0, 255.0));
  uint2 fragpos = uint2(v_pos.xy);

  bool semitransparent;
  uint3 icolor;
  float ialpha;
  float oalpha;

  #if INTERLACING
    #if INTERLACING_SCALED || !UPSCALED
      if ((fragpos.y & 1u) == u_interlaced_displayed_field)
        discard;
    #else
      if ((uint(v_pos.y * u_rcp_resolution_scale) & 1u) == u_interlaced_displayed_field)
        discard;
    #endif
  #endif

  #if TEXTURED
    float4 texcol;
    #if PAGE_TEXTURE && !TEXTURE_FILTERING
      texcol = SampleFromPageTexture(DECLARE_UV_LIMITS(v_tex0, v_uv_limits));
      if (VECTOR_EQ(texcol, TRANSPARENT_PIXEL_COLOR))
        discard;

      ialpha = 1.0;
    #elif TEXTURE_FILTERING
      #if PAGE_TEXTURE
        FilteredSampleFromVRAM(VECTOR_BROADCAST(TEXPAGE_VALUE, 0u), v_tex0, v_uv_limits, texcol, ialpha);
      #else
        FilteredSampleFromVRAM(v_texpage, v_tex0, v_uv_limits, texcol, ialpha);
      #endif
      if (ialpha < 0.5)
        discard;
    #else
      texcol = SampleFromVRAM(v_texpage, DECLARE_UV_LIMITS(v_tex0, v_uv_limits));
      if (VECTOR_EQ(texcol, TRANSPARENT_PIXEL_COLOR))
        discard;

      ialpha = 1.0;
    #endif

    semitransparent = (texcol.a >= 0.5);

    // If not using true color, truncate the framebuffer colors to 5-bit.
    #if !TRUE_COLOR
      icolor = uint3(texcol.rgb * float3(255.0, 255.0, 255.0)) >> 3;
      #if MODULATION_CROP
        icolor = (icolor * (vertcol >> 3)) >> 1;
      #else
        icolor = (icolor * vertcol) >> 4;
      #endif
      #if DITHERING
        icolor = ApplyDithering(fragpos, icolor);
      #else
        icolor = min(icolor >> 3, uint3(31u, 31u, 31u));
      #endif
    #else
      icolor = uint3(texcol.rgb * float3(255.0, 255.0, 255.0));
      #if MODULATION_CROP
        icolor = (icolor * (vertcol >> 3)) >> 4;
      #else
        icolor = (icolor * vertcol) >> 7;
      #endif
      icolor = min(icolor, uint3(255u, 255u, 255u));
    #endif

    // Compute output alpha (mask bit)
    oalpha = float(u_set_mask_while_drawing ? 1 : int(semitransparent));
  #else
    // All pixels are semitransparent for untextured polygons.
    semitransparent = true;
    icolor = vertcol;
    ialpha = 1.0;

    #if DITHERING
      icolor = ApplyDithering(fragpos, icolor);
    #else
      #if !TRUE_COLOR
        icolor >>= 3;
      #endif
    #endif

    // However, the mask bit is cleared if set mask bit is false.
    oalpha = float(u_set_mask_while_drawing);
  #endif

  #if SHADER_BLENDING
    #if USE_ROV
      BEGIN_ROV_REGION;
      float4 bg_col = ROV_LOAD(rov_color, fragpos);
      float4 o_col0;
      bool discarded = false;

      #if ROV_DEPTH_TEST
        float bg_depth = ROV_LOAD(rov_depth, fragpos).r;
        discarded = (v_pos.z < bg_depth);
      #endif
      #if CHECK_MASK_BIT
        discarded = discarded || (bg_col.a != 0.0);
      #endif        
    #else
      float4 bg_col = LAST_FRAG_COLOR;
      #if CHECK_MASK_BIT
        if (bg_col.a != 0.0)
          discard;
      #endif
    #endif

    // Work in normalized space for true colour, matches HW blend.
    float4 fg_col = float4(float3(icolor), oalpha);
    #if TRUE_COLOR
      fg_col.rgb /= 255.0;
    #elif TRANSPARENCY // rgb not used in check-mask only
      bg_col.rgb = roundEven(bg_col.rgb * 31.0);
    #endif

    o_col0.a = fg_col.a;

    #if TEXTURE_FILTERING && TEXTURE_ALPHA_BLENDING
      #if TRANSPARENCY_MODE == 0 // Half BG + Half FG.
        o_col0.rgb = (bg_col.rgb * saturate(0.5 / ialpha)) + (fg_col.rgb * (ialpha * 0.5));
      #elif TRANSPARENCY_MODE == 1 // BG + FG
        o_col0.rgb = (bg_col.rgb * saturate(1.0 / ialpha)) + (fg_col.rgb * ialpha);
      #elif TRANSPARENCY_MODE == 2 // BG - FG
        o_col0.rgb = (bg_col.rgb * saturate(1.0 / ialpha)) - (fg_col.rgb * ialpha);
      #elif TRANSPARENCY_MODE == 3 // BG + 1/4 FG.
        o_col0.rgb = (bg_col.rgb * saturate(1.0 / ialpha)) + (fg_col.rgb * (0.25 * ialpha));
      #else
        o_col0.rgb = (fg_col.rgb * ialpha) + (bg_col.rgb * (1.0 - ialpha));
      #endif
    #else
      #if TRANSPARENCY_MODE == 0 // Half BG + Half FG.
        o_col0.rgb = (bg_col.rgb * 0.5) + (fg_col.rgb * 0.5);
      #elif TRANSPARENCY_MODE == 1 // BG + FG
        o_col0.rgb = bg_col.rgb + fg_col.rgb;
      #elif TRANSPARENCY_MODE == 2 // BG - FG
        o_col0.rgb = bg_col.rgb - fg_col.rgb;
      #elif TRANSPARENCY_MODE == 3 // BG + 1/4 FG.
        o_col0.rgb = bg_col.rgb + (fg_col.rgb * 0.25);
      #else
        o_col0.rgb = fg_col.rgb;
      #endif
    #endif

    // 16-bit truncation.
    #if !TRUE_COLOR && TRANSPARENCY
      o_col0.rgb = floor(o_col0.rgb);
    #endif

    #if TRANSPARENCY
      // If pixel isn't marked as semitransparent, replace with previous colour.
      o_col0 = semitransparent ? o_col0 : fg_col;
    #endif

    // Normalize for non-true-color.
    #if !TRUE_COLOR
      o_col0.rgb /= 31.0;
    #endif

    #if USE_ROV
      if (!discarded)
      {
        ROV_STORE(rov_color, fragpos, o_col0);
        #if USE_ROV_DEPTH && ROV_DEPTH_WRITE
          ROV_STORE_SCALAR(rov_depth, fragpos, v_pos.z);
        #endif
      }
      END_ROV_REGION;
    #endif
  #else
    // Premultiply alpha so we don't need to use a colour output for it.
    float premultiply_alpha = ialpha;
    #if TRANSPARENCY
      premultiply_alpha = ialpha * (semitransparent ? u_src_alpha_factor : 1.0);
    #endif

    float3 color;
    #if !TRUE_COLOR
      // We want to apply the alpha before the truncation to 16-bit, otherwise we'll be passing a 32-bit precision color
      // into the blend unit, which can cause a small amount of error to accumulate.
      color = floor(float3(icolor) * premultiply_alpha) / 31.0;
    #else
      // True color is actually simpler here since we want to preserve the precision.
      color = (float3(icolor) * premultiply_alpha) / 255.0;
    #endif

    #if TRANSPARENCY && TEXTURED
      // Apply semitransparency. If not a semitransparent texel, destination alpha is ignored.
      if (semitransparent)
      {
        #if USE_DUAL_SOURCE
          o_col0 = float4(color, oalpha);
          o_col1 = float4(0.0, 0.0, 0.0, u_dst_alpha_factor / ialpha);
        #else
          o_col0 = float4(color, oalpha);
        #endif

        #if WRITE_MASK_AS_DEPTH
          o_depth = oalpha * v_pos.z;
        #endif

        #if TRANSPARENCY_ONLY_OPAQUE
          discard;
        #endif
      }
      else
      {
        #if USE_DUAL_SOURCE
          o_col0 = float4(color, oalpha);
          o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
        #else
          o_col0 = float4(color, oalpha);
        #endif

        #if WRITE_MASK_AS_DEPTH
          o_depth = oalpha * v_pos.z;
        #endif

        #if TRANSPARENCY_ONLY_TRANSPARENT
          discard;
        #endif
      }
    #elif TRANSPARENCY
      // We shouldn't be rendering opaque geometry only when untextured, so no need to test/discard here.
      #if USE_DUAL_SOURCE
        o_col0 = float4(color, oalpha);
        o_col1 = float4(0.0, 0.0, 0.0, u_dst_alpha_factor / ialpha);
      #else
        o_col0 = float4(color, oalpha);
      #endif

      #if WRITE_MASK_AS_DEPTH
        o_depth = oalpha * v_pos.z;
      #endif
    #else
      // Non-transparency won't enable blending so we can write the mask here regardless.
      o_col0 = float4(color, oalpha);

      #if USE_DUAL_SOURCE
        o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
      #endif

      #if WRITE_MASK_AS_DEPTH
        o_depth = oalpha * v_pos.z;
      #endif
    #endif
  #endif
}
)";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMExtractFragmentShader(u32 resolution_scale, u32 multisamples,
                                                                bool color_24bit, bool depth_buffer) const
{
  const bool msaa = (multisamples > 1);

  std::stringstream ss;
  WriteHeader(ss);
  WriteColorConversionFunctions(ss);

  DefineMacro(ss, "COLOR_24BIT", color_24bit);
  DefineMacro(ss, "DEPTH_BUFFER", depth_buffer);
  DefineMacro(ss, "MULTISAMPLING", msaa);
  ss << "CONSTANT uint RESOLUTION_SCALE = " << resolution_scale << "u;\n";
  ss << "CONSTANT uint2 VRAM_SIZE = uint2(" << VRAM_WIDTH << ", " << VRAM_HEIGHT << ") * RESOLUTION_SCALE;\n";
  ss << "CONSTANT uint MULTISAMPLES = " << multisamples << "u;\n";

  DeclareUniformBuffer(ss, {"uint2 u_vram_offset", "float u_skip_x", "float u_line_skip"}, true);
  DeclareTexture(ss, "samp0", 0, msaa);
  if (depth_buffer)
    DeclareTexture(ss, "samp1", 1, msaa);

  ss << R"(
float4 LoadVRAM(int2 coords)
{
#if MULTISAMPLING
  float4 value = LOAD_TEXTURE_MS(samp0, coords, 0u);
  FOR_UNROLL (uint sample_index = 1u; sample_index < MULTISAMPLES; sample_index++)
    value += LOAD_TEXTURE_MS(samp0, coords, sample_index);
  value /= float(MULTISAMPLES);
  return value;
#else
  return LOAD_TEXTURE(samp0, coords, 0);
#endif
}

#if DEPTH_BUFFER
float LoadDepth(int2 coords)
{
  // Need to duplicate because different types in different languages...
#if MULTISAMPLING
  float value = LOAD_TEXTURE_MS(samp1, coords, 0u).r;
  FOR_UNROLL (uint sample_index = 1u; sample_index < MULTISAMPLES; sample_index++)
    value += LOAD_TEXTURE_MS(samp1, coords, sample_index).r;
  value /= float(MULTISAMPLES);
  return value;
#else
  return LOAD_TEXTURE(samp1, coords, 0).r;
#endif
}
#endif

float3 SampleVRAM24(uint2 icoords)
{
  // load adjacent 16-bit texels
  uint2 clamp_size = uint2(1024, 512);

  // relative to start of scanout
  uint2 vram_coords = u_vram_offset + uint2((icoords.x * 3u) / 2u, icoords.y);
  uint s0 = RGBA8ToRGBA5551(LoadVRAM(int2((vram_coords % clamp_size) * RESOLUTION_SCALE)));
  uint s1 = RGBA8ToRGBA5551(LoadVRAM(int2(((vram_coords + uint2(1, 0)) % clamp_size) * RESOLUTION_SCALE)));

  // select which part of the combined 16-bit texels we are currently shading
  uint s1s0 = ((s1 << 16) | s0) >> ((icoords.x & 1u) * 8u);

  // extract components and normalize
  return float3(float(s1s0 & 0xFFu) / 255.0, float((s1s0 >> 8u) & 0xFFu) / 255.0,
                float((s1s0 >> 16u) & 0xFFu) / 255.0);
}
)";

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, depth_buffer ? 2 : 1);
  ss << R"(
{
  // Have to floor because SV_Position is at the pixel center.
  float2 v_pos_floored = floor(v_pos.xy);
  uint2 icoords = uint2(v_pos_floored.x + u_skip_x, v_pos_floored.y * u_line_skip);
  int2 wrapped_coords = int2((icoords + u_vram_offset) % VRAM_SIZE);

  #if COLOR_24BIT
    o_col0 = float4(SampleVRAM24(icoords), 1.0);
  #else
    o_col0 = float4(LoadVRAM(wrapped_coords).rgb, 1.0);
  #endif

  #if DEPTH_BUFFER
    // Post-processing expects linear camera Z normalized to the GTE depth range.
    float depth = LoadDepth(wrapped_coords);
    o_col1 = float4((depth > 0.0) ? min(1.0 / (depth * 65535.0), 1.0) : 1.0, 0.0, 0.0, 0.0);
  #endif
}
)";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMReplacementBlitFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  m_has_uniform_buffer = true;
  DeclareTexture(ss, "samp0", 0);
  DeclareFragmentEntryPoint(ss, 0, 1);

  ss << R"(
{
  o_col0 = SAMPLE_TEXTURE(samp0, v_tex0);
}
)";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateWireframeGeometryShader() const
{
  std::stringstream ss;
  WriteHeader(ss);

  if (m_glsl)
  {
    ss << R"(
layout(triangles) in;
layout(line_strip, max_vertices = 6) out;

void main()
{
  gl_Position = gl_in[0].gl_Position;
  EmitVertex();
  gl_Position = gl_in[1].gl_Position;
  EmitVertex();
  EndPrimitive();
  gl_Position = gl_in[1].gl_Position;
  EmitVertex();
  gl_Position = gl_in[2].gl_Position;
  EmitVertex();
  EndPrimitive();
  gl_Position = gl_in[2].gl_Position;
  EmitVertex();
  gl_Position = gl_in[0].gl_Position;
  EmitVertex();
  EndPrimitive();
}
)";
  }
  else
  {
    ss << R"(
struct GSInput
{
  float4 col0 : COLOR0;
  float4 pos : SV_Position;
};

struct GSOutput
{
  float4 pos : SV_Position;
};

GSOutput GetVertex(GSInput vi)
{
  GSOutput vo;
  vo.pos = vi.pos;
  return vo;
}

[maxvertexcount(6)]
void main(triangle GSInput input[3], inout LineStream<GSOutput> output)
{
  output.Append(GetVertex(input[0]));
  output.Append(GetVertex(input[1]));
  output.RestartStrip();

  output.Append(GetVertex(input[1]));
  output.Append(GetVertex(input[2]));
  output.RestartStrip();

  output.Append(GetVertex(input[2]));
  output.Append(GetVertex(input[0]));
  output.RestartStrip();
}
)";
  }

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateWireframeFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);

  DeclareFragmentEntryPoint(ss, 0, 0);
  ss << R"(
{
  o_col0 = float4(1.0, 1.0, 1.0, 0.5);
}
)";

  return std::move(ss).str();
}

void GPU_HW_ShaderGen::WriteVRAMReadFunctions(std::stringstream& ss, u32 resolution_scale, u32 multisamples) const
{
  const bool msaa = (multisamples > 1);
  DefineMacro(ss, "MULTISAMPLING", msaa);
  ss << "CONSTANT uint RESOLUTION_SCALE = " << resolution_scale << "u;\n";
  ss << "CONSTANT uint MULTISAMPLES = " << multisamples << "u;\n";

  ss << R"(
float4 LoadVRAM(int2 coords)
{
#if MULTISAMPLING
  float4 value = LOAD_TEXTURE_MS(samp0, coords, 0u);
  FOR_UNROLL (uint sample_index = 1u; sample_index < MULTISAMPLES; sample_index++)
    value += LOAD_TEXTURE_MS(samp0, coords, sample_index);
  value /= float(MULTISAMPLES);
  return value;
#else
  return LOAD_TEXTURE(samp0, coords, 0);
#endif
}

uint SampleVRAM(uint2 coords)
{
  if (RESOLUTION_SCALE == 1u)
    return RGBA8ToRGBA5551(LoadVRAM(int2(coords)));

  // Box filter for downsampling.
  float4 value = float4(0.0, 0.0, 0.0, 0.0);
  uint2 base_coords = coords * uint2(RESOLUTION_SCALE, RESOLUTION_SCALE);
  FOR_LOOP (uint offset_x = 0u; offset_x < RESOLUTION_SCALE; offset_x++)
  {
    FOR_LOOP (uint offset_y = 0u; offset_y < RESOLUTION_SCALE; offset_y++)
      value += LoadVRAM(int2(base_coords + uint2(offset_x, offset_y)));
  }
  value /= float(RESOLUTION_SCALE * RESOLUTION_SCALE);
  return RGBA8ToRGBA5551(value);
}
)";
}

std::string GPU_HW_ShaderGen::GenerateVRAMReadComputeShader(u32 resolution_scale, u32 multisamples) const
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteColorConversionFunctions(ss);
  DeclareUniformBuffer(ss, {"uint2 u_base_coords", "uint2 u_size", "uint u_output_offset", "uint u_output_stride"},
                       false);
  DeclareTexture(ss, "samp0", 0, multisamples > 1);
  WriteVRAMReadFunctions(ss, resolution_scale, multisamples);
  DeclareComputeOutputBuffer(ss, "output_buffer");
  if (m_glsl)
    ss << "shared uint s_right[32];\n#define GROUP_BARRIER barrier()\n";
  else
    ss << "groupshared uint s_right[32];\n#define GROUP_BARRIER GroupMemoryBarrierWithGroupSync()\n";
  DeclareComputeEntryPoint(ss, 8, 8, 1);
  ss << R"(
{
  // One pixel per thread, odd lanes hand their pixel to the even lane to pack. Sampling two pixels per thread
  // doubles the stride between lanes' fetches, which is much slower on some GPUs (35x on NVIDIA at 12x/8xMSAA).
  uint2 pos = GLOBAL_ID.xy;
  bool in_bounds = (pos.x < u_size.x && pos.y < u_size.y);
  uint pair_index = ((pos.y & 7u) * 8u + (pos.x & 7u)) >> 1;
  uint value = 0u;
  if (in_bounds)
    value = SampleVRAM(u_base_coords + pos);
  if ((pos.x & 1u) != 0u)
    s_right[pair_index] = value;
  GROUP_BARRIER;
  if (in_bounds && (pos.x & 1u) == 0u)
    output_buffer[u_output_offset + pos.y * u_output_stride + (pos.x >> 1)] = value | (s_right[pair_index] << 16u);
})";
  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMReadFragmentShader(u32 resolution_scale, u32 multisamples) const
{
  const bool msaa = (multisamples > 1);

  std::stringstream ss;
  WriteHeader(ss);
  WriteColorConversionFunctions(ss);

  DeclareUniformBuffer(ss, {"uint2 u_base_coords", "uint2 u_size"}, true);
  DeclareTexture(ss, "samp0", 0, msaa);
  WriteVRAMReadFunctions(ss, resolution_scale, multisamples);

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 1);
  ss << R"(
{
  uint2 sample_coords = uint2(uint(v_pos.x) * 2u, uint(v_pos.y));
  sample_coords += u_base_coords;

  // We're encoding as 32-bit, so the output width is halved and we pack two 16-bit pixels in one 32-bit pixel.
  uint left = SampleVRAM(sample_coords);
  uint right = SampleVRAM(uint2(sample_coords.x + 1u, sample_coords.y));

  o_col0 = float4(float(left & 0xFFu), float((left >> 8) & 0xFFu),
                  float(right & 0xFFu), float((right >> 8) & 0xFFu))
            / float4(255.0, 255.0, 255.0, 255.0);
})";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMWriteFragmentShader(bool use_buffer, bool use_ssbo,
                                                              GPUTextureFilter texture_filter, bool write_mask_as_depth,
                                                              bool write_depth_as_rt, bool check_mask, bool use_rov,
                                                              bool msaa) const
{
  Assert(!write_mask_as_depth || (write_mask_as_depth != write_depth_as_rt));
  Assert(!check_mask || !write_mask_as_depth);
  Assert(!use_rov || check_mask);

  std::stringstream ss;
  WriteHeader(ss, use_rov, check_mask && !use_rov);
  WriteColorConversionFunctions(ss);

  DefineMacro(ss, "WRITE_MASK_AS_DEPTH", write_mask_as_depth);
  DefineMacro(ss, "WRITE_DEPTH_AS_RT", write_depth_as_rt);
  DefineMacro(ss, "CHECK_MASK", check_mask);
  DefineMacro(ss, "USE_ROV", use_rov);
  DefineMacro(ss, "USE_BUFFER", use_buffer);
  const bool filtering = (texture_filter != GPUTextureFilter::Nearest);
  if (filtering)
    DefineMacro(ss, "TEXTURE_ALPHA_BLENDING", false);

  ss << "CONSTANT float2 VRAM_SIZE = float2(" << VRAM_WIDTH << ".0, " << VRAM_HEIGHT << ".0);\n";

  DeclareUniformBuffer(ss,
                       {"float2 u_base_coords", "float2 u_end_coords", "float2 u_size", "float u_resolution_scale",
                        "uint u_buffer_base_offset", "uint u_mask_or_bits", "float u_depth_value"},
                       false);

  if (!use_buffer)
  {
    DeclareTexture(ss, "samp0", 0, false, true, true);
  }
  else if (use_ssbo && m_glsl)
  {
    ss << "layout(std430";
    if (IsVulkan())
      ss << ", set = 2, binding = 0";
    else if (IsMetal())
      ss << ", set = 1, binding = 0";
    else if (m_use_glsl_binding_layout)
      ss << ", binding = 0";

    ss << ") readonly restrict buffer SSBO {\n";
    ss << "  uint ssbo_data[];\n";
    ss << "};\n\n";

    ss << "#define GET_VALUE(buffer_offset) (ssbo_data[(buffer_offset) / 2u] >> (((buffer_offset) % 2u) * 16u))\n\n";
  }
  else
  {
    DeclareTextureBuffer(ss, "samp1", true, true);
    ss << "#define GET_VALUE(buffer_offset) (LOAD_TEXTURE_BUFFER(samp1, int(buffer_offset)).r)\n\n";
  }

  if (filtering)
  {
    ss << R"(
#define TEXPAGE_VALUE uint
CONSTANT float4 TRANSPARENT_PIXEL_COLOR = float4(0.0, 0.0, 0.0, 0.0);

float4 SampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits)
{
  uint2 icoords = uint2(floor(clamp(coords, uv_limits.xy, uv_limits.zw)));
#if !USE_BUFFER
  uint value = LOAD_TEXTURE(samp0, int2(icoords), 0).x;
#else
  uint buffer_offset = u_buffer_base_offset + icoords.y * uint(u_size.x) + icoords.x;
  uint value = GET_VALUE(buffer_offset);
#endif
  // We unconditionally set the mask bit to ensure that no pixels will be flagged as zero alpha/transparent.
  // Not doing this breaks JINC2 and any other filters which do use the alpha values.
  return float4(RGBA5551ToRGBA8(value).rgb, 1.0);
}
)";

    WriteBatchTextureFilter(ss, texture_filter);
  }

  if (use_rov)
  {
    DeclareImage(ss, "rov_color", 0, "unorm float4", "rgba8");
    if (write_depth_as_rt)
      DeclareImage(ss, "rov_depth", 1, "float", "r32f");
  }

  // MSAA is only needed for the subpass input. Centroid inputs wouldn't match the screen vertex shader on OpenGL.
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, use_rov ? 0 : (1 + BoolToUInt32(write_depth_as_rt)), false,
                            write_mask_as_depth, msaa && (IsVulkan() || IsMetal()), false, false, false,
                            check_mask && !use_rov, use_rov);
  ss << R"(
{
  float2 native_coords = v_pos.xy / u_resolution_scale;
  float2 coords = floor(native_coords);

  // make sure it's not oversized and out of range
  if ((coords.x < u_base_coords.x && coords.x >= u_end_coords.x) ||
      (coords.y < u_base_coords.y && coords.y >= u_end_coords.y))
  {
    discard;
  }

#if USE_ROV
  float4 o_col0;
#elif CHECK_MASK
  // don't overwrite pixels with the mask bit set
  if (LAST_FRAG_COLOR.a != 0.0)
    discard;
#endif

  // find offset from the start of the row/column
  float2 offset;
  offset.x = (coords.x < u_base_coords.x) ? (VRAM_SIZE.x - u_base_coords.x + coords.x) : (coords.x - u_base_coords.x);
  offset.y = (coords.y < u_base_coords.y) ? (VRAM_SIZE.y - u_base_coords.y + coords.y) : (coords.y - u_base_coords.y);

  #if !USE_BUFFER
    uint value = LOAD_TEXTURE(samp0, int2(offset), 0).x;
  #else
    uint buffer_offset = u_buffer_base_offset + uint((offset.y * u_size.x) + offset.x);
    uint value = GET_VALUE(buffer_offset);
  #endif
)";

  if (filtering)
  {
    ss << R"(
  float4 original_color = RGBA5551ToRGBA8(value | u_mask_or_bits);
  uint2 frag_coords = uint2(v_pos.xy);
  uint2 canonical_coords = uint2(coords * u_resolution_scale);
  bool canonical = (frag_coords.x == canonical_coords.x && frag_coords.y == canonical_coords.y);

  float4 filtered_color;
  float filtered_alpha;
  FilteredSampleFromVRAM(0u, offset + frac(native_coords), float4(0.0, 0.0, u_size - float2(1.0, 1.0)),
                         filtered_color, filtered_alpha);

  // Native-resolution texture and 24-bit scanout fetches use the canonical sample. The mask bit belongs
  // to the original VRAM pixel and must not be filtered.
  o_col0 = float4(canonical ? original_color.rgb : filtered_color.rgb, original_color.a);
)";
  }
  else
  {
    ss << "  o_col0 = RGBA5551ToRGBA8(value | u_mask_or_bits);\n";
  }

  ss << R"(
#if USE_ROV
  BEGIN_ROV_REGION;
  if (ROV_LOAD(rov_color, v_pos.xy).a == 0.0)
  {
    ROV_STORE(rov_color, v_pos.xy, o_col0);
  #if WRITE_DEPTH_AS_RT
    ROV_STORE_SCALAR(rov_depth, v_pos.xy, 0.0f);
  #endif
  }
  END_ROV_REGION;
#elif WRITE_MASK_AS_DEPTH
  o_depth = (o_col0.a == 1.0) ? u_depth_value : 0.0;
#elif WRITE_DEPTH_AS_RT
  o_col1 = float4(0.0f, 0.0f, 0.0f, 0.0f);
#endif
})";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMCopyFragmentShader(bool write_mask_as_depth, bool write_depth_as_rt,
                                                             bool check_mask, bool use_rov, bool msaa) const
{
  Assert(!write_mask_as_depth || (write_mask_as_depth != write_depth_as_rt));
  Assert(!check_mask || !write_mask_as_depth);
  Assert(!use_rov || check_mask);

  // TODO: This won't currently work because we can't bind the texture to both the shader and framebuffer.
  const bool msaa_copy = false;

  std::stringstream ss;
  WriteHeader(ss, use_rov, check_mask && !use_rov);
  DefineMacro(ss, "WRITE_MASK_AS_DEPTH", write_mask_as_depth);
  DefineMacro(ss, "WRITE_DEPTH_AS_RT", write_depth_as_rt);
  DefineMacro(ss, "CHECK_MASK", check_mask);
  DefineMacro(ss, "USE_ROV", use_rov);
  DefineMacro(ss, "MSAA_COPY", msaa_copy);

  DeclareUniformBuffer(ss,
                       {"float2 u_src_coords", "float2 u_dst_coords", "float2 u_end_coords", "float2 u_vram_size",
                        "float u_resolution_scale", "bool u_set_mask_bit", "float u_depth_value"},
                       false);

  DeclareTexture(ss, "samp0", 0, msaa_copy);
  if (use_rov)
  {
    DeclareImage(ss, "rov_color", 0, "unorm float4", "rgba8");
    if (write_depth_as_rt)
      DeclareImage(ss, "rov_depth", 1, "float", "r32f");
  }

  // MSAA is only needed for the subpass input. Centroid inputs wouldn't match the screen vertex shader on OpenGL.
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, use_rov ? 0 : (1 + BoolToUInt32(write_depth_as_rt)), false,
                            write_mask_as_depth, msaa && (IsVulkan() || IsMetal()), false, msaa_copy, false,
                            check_mask && !use_rov, use_rov);
  ss << R"(
{
  float2 dst_coords = floor(v_pos.xy);

  // make sure it's not oversized and out of range
  if ((dst_coords.x < u_dst_coords.x && dst_coords.x >= u_end_coords.x) ||
      (dst_coords.y < u_dst_coords.y && dst_coords.y >= u_end_coords.y))
  {
    discard;
  }

#if USE_ROV
  float4 o_col0;
#elif CHECK_MASK
  // don't overwrite pixels with the mask bit set
  if (LAST_FRAG_COLOR.a != 0.0)
    discard;
#endif

  // find offset from the start of the row/column
  float2 offset;
  offset.x = (dst_coords.x < u_dst_coords.x) ? (u_vram_size.x - u_dst_coords.x + dst_coords.x) : (dst_coords.x - u_dst_coords.x);
  offset.y = (dst_coords.y < u_dst_coords.y) ? (u_vram_size.y - u_dst_coords.y + dst_coords.y) : (dst_coords.y - u_dst_coords.y);

  // find the source coordinates to copy from
  float2 offset_coords = u_src_coords + offset;
  float2 src_coords = offset_coords - (floor(offset_coords / u_vram_size) * u_vram_size);

  // sample and apply mask bit
#if MSAA_COPY
  float4 color = LOAD_TEXTURE_MS(samp0, int2(src_coords), f_sample_index);
#else
  float4 color = LOAD_TEXTURE(samp0, int2(src_coords), 0);
#endif
  o_col0 = float4(color.xyz, u_set_mask_bit ? 1.0 : color.a);
#if USE_ROV
  BEGIN_ROV_REGION;
  if (ROV_LOAD(rov_color, v_pos.xy).a == 0.0)
  {
    ROV_STORE(rov_color, v_pos.xy, o_col0);
  #if WRITE_DEPTH_AS_RT
    ROV_STORE_SCALAR(rov_depth, v_pos.xy, 0.0f);
  #endif
  }
  END_ROV_REGION;
#elif WRITE_MASK_AS_DEPTH
  o_depth = (u_set_mask_bit ? 1.0f : ((o_col0.a == 1.0) ? u_depth_value : 0.0));
#elif WRITE_DEPTH_AS_RT
  o_col1 = float4(0.0f, 0.0f, 0.0f, 0.0f);
#endif
})";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMFillFragmentShader(bool wrapped, bool interlaced, bool write_mask_as_depth,
                                                             bool write_depth_as_rt) const
{
  Assert(!write_mask_as_depth || (write_mask_as_depth != write_depth_as_rt));

  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "WRITE_MASK_AS_DEPTH", write_mask_as_depth);
  DefineMacro(ss, "WRITE_DEPTH_AS_RT", write_depth_as_rt);
  DefineMacro(ss, "WRAPPED", wrapped);
  DefineMacro(ss, "INTERLACED", interlaced);

  DeclareUniformBuffer(
    ss, {"uint2 u_dst_coords", "uint2 u_end_coords", "float4 u_fill_color", "uint u_interlaced_displayed_field"},
    false);

  DeclareFragmentEntryPoint(ss, 0, 1, {}, interlaced || wrapped, 1 + BoolToUInt32(write_depth_as_rt), false,
                            write_mask_as_depth, false, false, false);
  ss << R"(
{
#if INTERLACED || WRAPPED
  uint2 dst_coords = uint2(v_pos.xy);
#endif

#if INTERLACED
  if ((dst_coords.y & 1u) == u_interlaced_displayed_field)
    discard;
#endif

#if WRAPPED
  // make sure it's not oversized and out of range
  if ((dst_coords.x < u_dst_coords.x && dst_coords.x >= u_end_coords.x) ||
      (dst_coords.y < u_dst_coords.y && dst_coords.y >= u_end_coords.y))
  {
    discard;
  }
#endif

  o_col0 = u_fill_color;
#if WRITE_MASK_AS_DEPTH
  o_depth = u_fill_color.a;
#elif WRITE_DEPTH_AS_RT
  o_col1 = float4(0.0f, 0.0f, 0.0f, 0.0f);
#endif
})";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMUpdateDepthFragmentShader(bool msaa) const
{
  std::stringstream ss;
  WriteHeader(ss);
  m_has_uniform_buffer = true;
  DefineMacro(ss, "MULTISAMPLING", msaa);
  DeclareTexture(ss, "samp0", 0, msaa);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 0, false, true, false, false, msaa);

  ss << R"(
{
#if MULTISAMPLING
  o_depth = LOAD_TEXTURE_MS(samp0, int2(v_pos.xy), f_sample_index).a;
#else
  o_depth = LOAD_TEXTURE(samp0, int2(v_pos.xy), 0).a;
#endif
}
)";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMCopyDepthFragmentShader(bool msaa, bool merge) const
{
  std::stringstream ss;
  WriteHeader(ss);
  m_has_uniform_buffer = true;
  DefineMacro(ss, "MULTISAMPLED", msaa);
  DefineMacro(ss, "MERGE", merge);
  DeclareTexture(ss, "samp0", 0, msaa);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, msaa, 1, false, false, msaa, msaa, msaa);

  ss << R"(
{
#if MULTISAMPLED
  float depth = LOAD_TEXTURE_MS(samp0, int2(v_pos.xy), int(f_sample_index)).r;
#else
  float depth = SAMPLE_TEXTURE(samp0, v_tex0).r;
#endif

#if MERGE
  // Keep the previous value where nothing has been drawn since the last clear.
  if (depth == 0.0)
    discard;
#endif

  o_col0 = float4(depth, 0.0, 0.0, 0.0);
}
)";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMClearDepthFragmentShader(bool write_depth_as_rt) const
{
  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "WRITE_DEPTH_AS_RT", write_depth_as_rt);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, false, BoolToUInt32(write_depth_as_rt), false, false, false, false, false);

  ss << R"(
{
#if WRITE_DEPTH_AS_RT
  o_col0 = float4(0.0f, 0.0f, 0.0f, 0.0f);
#endif
}
)";

  return std::move(ss).str();
}

void GPU_HW_ShaderGen::WriteAdaptiveDownsampleUniformBuffer(std::stringstream& ss) const
{
  DeclareUniformBuffer(ss, {"float2 u_uv_min", "float2 u_uv_max", "float2 u_pixel_size", "float u_lod"}, true);
}

std::string GPU_HW_ShaderGen::GenerateAdaptiveDownsampleVertexShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteAdaptiveDownsampleUniformBuffer(ss);
  DeclareVertexEntryPoint(ss, {}, 0, 1, {}, true);
  ss << R"(
{
  v_tex0 = float2(float((v_id << 1) & 2u), float(v_id & 2u));
  v_pos = float4(v_tex0 * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  v_tex0 = u_uv_min + (u_uv_max - u_uv_min) * v_tex0;
  #if API_OPENGL || API_OPENGL_ES || API_VULKAN
    v_pos.y = -v_pos.y;
  #endif
}
)";
  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateAdaptiveDownsampleMipFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteAdaptiveDownsampleUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0, false);
  DeclareFragmentEntryPoint(ss, 0, 1);
  ss << R"(
{
  // Gather 4 samples for bilinear filtering.
  float2 uv = v_tex0 - u_pixel_size; // * 0.25 done on CPU
  float4 c00 = SAMPLE_TEXTURE_LEVEL_OFFSET(samp0, uv, u_lod, int2(0, 0));
  float4 c01 = SAMPLE_TEXTURE_LEVEL_OFFSET(samp0, uv, u_lod, int2(0, 1));
  float4 c10 = SAMPLE_TEXTURE_LEVEL_OFFSET(samp0, uv, u_lod, int2(1, 0));
  float4 c11 = SAMPLE_TEXTURE_LEVEL_OFFSET(samp0, uv, u_lod, int2(1, 1));
  float3 cavg = (c00.rgb + c01.rgb + c10.rgb + c11.rgb) * 0.25;

  // Compute variance between pixels with logarithmic scaling to aggressively reduce along the edges.
  float variance =
    1.0 - log2(1000.0 * (dot(c00.rgb - cavg.rgb, c00.rgb - cavg.rgb) + dot(c01.rgb - cavg, c01.rgb - cavg) +
                         dot(c10.rgb - cavg.rgb, c10.rgb - cavg.rgb) + dot(c11.rgb - cavg, c11.rgb - cavg)) +
               1.0);

  // Write variance to the alpha channel, weighted by the previous LOD's variance.
  // There's no variance in the first LOD.
  float aavg = (c00.a + c01.a + c10.a + c11.a) * 0.25;
  o_col0.rgb = cavg.rgb;
  o_col0.a = variance * ((u_lod == 0.0) ? 1.0 : aavg);
}
)";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateAdaptiveDownsampleBlurFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteColorConversionFunctions(ss);
  WriteAdaptiveDownsampleUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0, false);
  DeclareFragmentEntryPoint(ss, 0, 1);
  ss << R"(
{
  // Bog standard blur kernel unrolled for speed:
  // [ 0.0625, 0.125, 0.0625
  //   0.125,  0.25,  0.125
  //   0.0625, 0.125, 0.0625 ]
  //
  // Can't use offset for sampling here, because we need to clamp, and the source texture is larger.
  //
#define KERNEL_SAMPLE(weight, xoff, yoff)                                                                              \
  (weight) * SAMPLE_TEXTURE_LEVEL(                                                                                     \
               samp0, clamp((v_tex0 + float2(float(xoff), float(yoff)) * u_pixel_size), u_uv_min, u_uv_max), 0.0)      \
               .a
  float blur = KERNEL_SAMPLE(0.0625, -1, -1);
  blur += KERNEL_SAMPLE(0.0625, 1, -1);
  blur += KERNEL_SAMPLE(0.0625, -1, 1);
  blur += KERNEL_SAMPLE(0.0625, 1, 1);
  blur += KERNEL_SAMPLE(0.125, 0, -1);
  blur += KERNEL_SAMPLE(0.125, -1, 0);
  blur += KERNEL_SAMPLE(0.125, 1, 0);
  blur += KERNEL_SAMPLE(0.125, 0, 1);
  blur += KERNEL_SAMPLE(0.25, 0, 0);
  o_col0 = float4(blur, blur, blur, blur);
}
)";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateAdaptiveDownsampleCompositeFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteAdaptiveDownsampleUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0, false);
  DeclareTexture(ss, "samp1", 1, false);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true);
  ss << R"(
{
  // Sample the mip level determined by the weight texture. samp0 is trilinear, so it will blend between levels.
  o_col0 = float4(SAMPLE_TEXTURE_LEVEL(samp0, v_tex0, SAMPLE_TEXTURE(samp1, v_tex0).r * u_lod).rgb, 1.0);
}
)";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateBoxSampleDownsampleFragmentShader(u32 factor) const
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"uint2 u_base_coords"}, true);
  DeclareTexture(ss, "samp0", 0, false);

  ss << "CONSTANT uint FACTOR = " << factor << "u;\n";

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true);
  ss << R"(
{
  float3 color = float3(0.0, 0.0, 0.0);
  uint2 base_coords = u_base_coords + uint2(v_pos.xy) * uint2(FACTOR, FACTOR);
  for (uint offset_x = 0u; offset_x < FACTOR; offset_x++)
  {
    for (uint offset_y = 0u; offset_y < FACTOR; offset_y++)
      color += LOAD_TEXTURE(samp0, int2(base_coords + uint2(offset_x, offset_y)), 0).rgb;
  }
  color /= float(FACTOR * FACTOR);
  o_col0 = float4(color, 1.0);
}
)";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateReplacementMergeFragmentShader(bool replacement, bool semitransparent,
                                                                     bool bilinear_filter) const
{
  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "REPLACEMENT", replacement);
  DefineMacro(ss, "SEMITRANSPARENT", semitransparent);
  DefineMacro(ss, "BILINEAR_FILTER", bilinear_filter);
  DeclareUniformBuffer(ss, {"float4 u_src_rect", "float4 u_texture_size"}, true);
  DeclareTexture(ss, "samp0", 0);
  DeclareFragmentEntryPoint(ss, 0, 1);

  ss << R"(
{
  float2 start_coords = u_src_rect.xy + v_tex0 * u_src_rect.zw;

#if BILINEAR_FILTER
  // Compute the coordinates of the four texels we will be interpolating between.
  // Clamp this to the triangle texture coordinates.
  float2 coords = start_coords * u_texture_size.xy;
  float2 texel_top_left = frac(coords) - float2(0.5, 0.5);
  float2 texel_offset = sign(texel_top_left);
  float4 fcoords = max(coords.xyxy + float4(0.0, 0.0, texel_offset.x, texel_offset.y),
                        float4(0.0, 0.0, 0.0, 0.0)) * u_texture_size.zwzw;

  // Load four texels.
  float4 s00 = SAMPLE_TEXTURE_LEVEL(samp0, fcoords.xy, 0.0);
  float4 s10 = SAMPLE_TEXTURE_LEVEL(samp0, fcoords.zy, 0.0);
  float4 s01 = SAMPLE_TEXTURE_LEVEL(samp0, fcoords.xw, 0.0);
  float4 s11 = SAMPLE_TEXTURE_LEVEL(samp0, fcoords.zw, 0.0);

  // Bilinearly interpolate.
  float2 weights = abs(texel_top_left);
  float4 color = lerp(lerp(s00, s10, weights.x), lerp(s01, s11, weights.x), weights.y);
  float orig_alpha = float(color.a > 0.0);

  #if !SEMITRANSPARENT
    // Compute alpha from how many texels aren't pixel color 0000h.
    float a00 = float(VECTOR_NEQ(s00, float4(0.0, 0.0, 0.0, 0.0)));
    float a10 = float(VECTOR_NEQ(s10, float4(0.0, 0.0, 0.0, 0.0)));
    float a01 = float(VECTOR_NEQ(s01, float4(0.0, 0.0, 0.0, 0.0)));
    float a11 = float(VECTOR_NEQ(s11, float4(0.0, 0.0, 0.0, 0.0)));
    color.a = lerp(lerp(a00, a10, weights.x), lerp(a01, a11, weights.x), weights.y);

    // Compensate for partially transparent sampling.
    color.rgb /= (color.a != 0.0) ? color.a : 1.0;

    // Use binary alpha.
    color.a = (color.a >= 0.5) ? 1.0 : 0.0;
  #endif
#else
  float4 color = SAMPLE_TEXTURE_LEVEL(samp0, start_coords, 0.0);
  float orig_alpha = color.a;
#endif
  o_col0.rgb = color.rgb;

  // Alpha processing.
  #if REPLACEMENT
    #if SEMITRANSPARENT
      // Map anything not 255 to 1 for semitransparent, otherwise zero for opaque.
      o_col0.a = (color.a <= 0.95f) ? 1.0f : 0.0f;
      o_col0.a = VECTOR_EQ(color, float4(0.0, 0.0, 0.0, 0.0)) ? 0.0f : o_col0.a;
    #else
      // Map anything with an alpha below 0.5 to transparent.
      // Leave (0,0,0,0) as 0000 for opaque replacements for cutout alpha.
      float alpha = float(color.a >= 0.5);
      o_col0.rgb = lerp(float3(0.0, 0.0, 0.0), o_col0.rgb, alpha);

      // We can't simply clear the alpha channel unconditionally here, because that
      // would result in any black pixels with zero alpha being transparency-culled.
      // Instead, we set it to a minimum value (2/255 in case of rounding error, I
      // don't trust drivers here) so that transparent polygons in the source still
      // set bit 15 to zero in the framebuffer, but are not transparency-culled.
      // Silent Hill needs it to be zero, I'm not aware of anything that needs
      // specific values yet. If it did, we'd need a different dumping technique.
      o_col0.a = lerp(0.0, 2.0 / 255.0, alpha);
    #endif
  #else
    // Preserve original bit 15 for non-replacements.
    o_col0.a = orig_alpha;
  #endif
}
)";

  return std::move(ss).str();
}
