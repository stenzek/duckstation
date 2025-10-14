// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0
//
// NOTE: Some parts of this file have more permissive licenses. They are marked appropriately.
//

#include "gpu_hw_shadergen.h"

#include "common/assert.h"

GPU_HW_ShaderGen::GPU_HW_ShaderGen(RenderAPI render_api, bool supports_dual_source_blend,
                                   bool supports_framebuffer_fetch)
  : ShaderGen(render_api, GetShaderLanguageForAPI(render_api), supports_dual_source_blend, supports_framebuffer_fetch)
{
}

GPU_HW_ShaderGen::~GPU_HW_ShaderGen() = default;

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
  // Depth set to 1 for PGXP depth buffer.
  v_pos = float4(a_pos, 1.0f, 1.0f);
  v_tex0 = a_tex0;

  // NDC space Y flip in Vulkan.
  #if API_OPENGL || API_OPENGL_ES || API_VULKAN
    v_pos.y = -v_pos.y;
  #endif
}
)";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateBatchVertexShader(bool upscaled, bool msaa, bool per_sample_shading,
                                                        bool textured, bool palette, bool page_texture, bool uv_limits,
                                                        bool force_round_texcoords, bool pgxp_depth,
                                                        bool disable_color_perspective) const
{
  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "TEXTURED", textured);
  DefineMacro(ss, "PALETTE", palette);
  DefineMacro(ss, "PAGE_TEXTURE", page_texture);
  DefineMacro(ss, "UV_LIMITS", uv_limits);
  DefineMacro(ss, "FORCE_ROUND_TEXCOORDS", force_round_texcoords);
  DefineMacro(ss, "PGXP_DEPTH", pgxp_depth);
  DefineMacro(ss, "UPSCALED", upscaled);

  WriteBatchUniformBuffer(ss);

  if (textured && page_texture)
  {
    if (uv_limits)
    {
      DeclareVertexEntryPoint(
        ss, {"float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage", "float4 a_uv_limits"}, 1, 1,
        {{"nointerpolation", "float4 v_uv_limits"}}, false, "", msaa, per_sample_shading, disable_color_perspective);
    }
    else
    {
      DeclareVertexEntryPoint(ss, {"float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage"}, 1, 1, {},
                              false, "", msaa, per_sample_shading, disable_color_perspective);
    }
  }
  else if (textured)
  {
    if (uv_limits)
    {
      DeclareVertexEntryPoint(
        ss, {"float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage", "float4 a_uv_limits"}, 1, 1,
        {{"nointerpolation", palette ? "uint4 v_texpage" : "uint2 v_texpage"},
         {"nointerpolation", "float4 v_uv_limits"}},
        false, "", msaa, per_sample_shading, disable_color_perspective);
    }
    else
    {
      DeclareVertexEntryPoint(ss, {"float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage"}, 1, 1,
                              {{"nointerpolation", palette ? "uint4 v_texpage" : "uint2 v_texpage"}}, false, "", msaa,
                              per_sample_shading, disable_color_perspective);
    }
  }
  else
  {
    DeclareVertexEntryPoint(ss, {"float4 a_pos", "float4 a_col0"}, 1, 0, {}, false, "", msaa, per_sample_shading,
                            disable_color_perspective);
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
  // Ignore mask Z when using PGXP depth.
  float pos_z = a_pos.w;
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
    #if !PALETTE && !PAGE_TEXTURE
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
      #elif !PAGE_TEXTURE && !PALETTE
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
  float4 s00 = SampleFromVRAM(texpage, clamp(fcoords.xy, uv_limits.xy, uv_limits.zw));
  float4 s10 = SampleFromVRAM(texpage, clamp(fcoords.zy, uv_limits.xy, uv_limits.zw));
  float4 s01 = SampleFromVRAM(texpage, clamp(fcoords.xw, uv_limits.xy, uv_limits.zw));
  float4 s11 = SampleFromVRAM(texpage, clamp(fcoords.zw, uv_limits.xy, uv_limits.zw));

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

#define sample_texel(coords) SampleFromVRAM(texpage, clamp((coords), uv_limits.xy, uv_limits.zw))

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

#define P(coord, xoffs, yoffs) SampleFromVRAM(texpage, clamp(coords + float2((xoffs), (yoffs)), uv_limits.xy, uv_limits.zw))

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
  else if (texture_filter == GPUTextureFilter::MMPX)
  {
    ss << "#define src(xoffs, yoffs) packUnorm4x8(SampleFromVRAM(texpage, clamp(bcoords + float2((xoffs), (yoffs)), "
          "uv_limits.xy, uv_limits.zw)))\n";

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
    ss << "#define src(xoffs, yoffs) packUnorm4x8(SampleFromVRAM(texpage, clamp(bcoords + float2((xoffs), (yoffs)), "
          "uv_limits.xy, uv_limits.zw)))\n";

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


// Two-stage weak blending, mix/none
uint admix2d(uint a, uint b) {
    float4 a_float = unpackUnorm4x8(a);
    float4 b_float = unpackUnorm4x8(b);
    float3 diff_rgb = a_float.rgb - b_float.rgb;
    float rgbDist = dot(diff_rgb, diff_rgb);
    
    // Combine conditional judgments (reduce branches)
    bool aIsBlack = dot(a_float.rgb, a_float.rgb) < 0.01;
    //bool aIsTransparent = a_float.a < 0.01;
    //bool bIsTransparent = b_float.a < 0.01;
    
    if (aIsBlack ) return b;

    // Determine blending mode based on distance
    float4 result;
    if (rgbDist < 1.0) {
        // Close distance: linearly blend RGB and Alpha
        result = (a_float + b_float) * 0.5;
    } else {
        // Far distance: return b
        result = b_float;
    }
    
    // Repack as uint
    return packUnorm4x8(result);
}


/*=============================================================================
Auxiliary function for 4-pixel cross determination: scores the number of matches at specific positions of the pattern.
Three pattern conditions are determined, requiring 6 points to be satisfied.
                ┌───┬───┬───┐                ┌───┬───┬───┐
                │ A │ B │ C │                │ A │ B │ 1 │
                ├───┼───┼───┤                ├───┼───┼───┤
                │ D │ E │ F │       =>   L   │ B │ A │ 2 │
                ├───┼───┼───┤                ├───┼───┼───┤
                │ G │ H │ I │                │ 5 │ 4 │ 3 │
                └───┴───┴───┘                └───┴───┴───┘
=============================================================================*/

bool countPatternMatches(uint LA, uint LB, uint L1, uint L2, uint L3, uint L4, uint L5) {

    int score1 = 0; // Diagonal pattern 1
    int score2 = 0; // Diagonal pattern 2
    int score3 = 0; // Horizontal/vertical line pattern
    int scoreBonus = 0;
    
    // Replace Euclidean formula with dot product to save a square root calculation
    float4 a_float = unpackUnorm4x8(LA);
    float4 b_float = unpackUnorm4x8(LB);
    float3 diff_rgb = a_float.rgb - b_float.rgb;
    float rgbDist = dot(diff_rgb, diff_rgb);
 
    // Add details for very close colors, reduce details for highly different colors (font edges)
    if (rgbDist < 0.06386) { // Point set after quadratic golden section, colors are quite close
        scoreBonus += 1;
    } else if (rgbDist > 2.18847) { // Point set after quadratic golden section, significant difference
        scoreBonus -= 1;
      } 

    // Diagonals use a deduction system: deduct points for crosses, add back if conditions are met
    // 1. Diagonal pattern ╲ (Condition: B = 2 or 4)
    if (LB == L2 || LB == L4) {
        score1 -= int(LB == L2 && LA == L1) * 1;    	// A-1 and B-2 form a cross, deduct points
        score1 -= int(LB == L4 && LA == L5) * 1;   		// A-5 and B-4 form a cross, deduct points

        // If the following triangular pattern is satisfied, offset the above cross deductions
        score1 += int(LB == L1 && L1 == L2) * 1;   		// B-1-2 form a triangular pattern, add points
        score1 += int(LB == L4 && L4 == L5) * 1;   		// B-4-5 form a triangular pattern, add points
        score1 += int(L2 == L3 && L3 == L4) * 1;   		// 2-3-4 form a triangular pattern, add points
        
        score1 += scoreBonus + 6;
    } 

    // 2. Diagonal pattern ╱ (Condition: A = 1 or 5)
    if (LA == L1 || LA == L5) {
        score2 -= int(LB == L2 && LA == L1) * 1;    	// A-1 and B-2 form a cross, deduct points
        score2 -= int(LB == L4 && LA == L5) * 1;   		// A-5 and B-4 form a cross, deduct points
        score2 -= int(LA == L3) * 1;    					// A-3 forms a cross, deduct points				

        // If the following triangular pattern is satisfied, offset the above cross deductions
        score2 += int(LB == L1 && L1 == L2) * 1;   		// B-1-2 form a triangular pattern, add points
        score2 += int(LB == L4 && L4 == L5) * 1;   		// B-4-5 form a triangular pattern, add points
        score2 += int(L2 == L3 && L3 == L4) * 1;   		// 2-3-4 form a triangular pattern, add points
    
        score2 += scoreBonus + 6;
    } 

    // 3. Horizontal/vertical line pattern (Condition: horizontal continuity) uses a point addition system, passes only if conditions are met
    if (LA == L2 || LB == L1 || LA == L4 || LB == L5 || (L1 == L2 && L2 == L3) || (L3 == L4 && L4 == L5)) {
        score3 += int(LA == L2);    	// A equals 2, +1
        score3 += int(LB == L1);    	// B equals 1, +1
        score3 += int(L3 == L4);    	// 3 equals 4, +1
        score3 += int(L4 == L5);    	// 4 equals 5, +1
        score3 += int(L3 == L4 && L4 == L5); // 3-4-5 continuous

        score3 += int(LB == L5);    	// B equals 5, +1
        score3 += int(LA == L4);    	// A equals 4, +1
        score3 += int(L2 == L3);    	// 2 equals 3, +1
        score3 += int(L1 == L2);    	// 1 equals 2, +1
        score3 += int(L1 == L2 && L2 == L3); // 1-2-3 continuous

        // A x 4 square
        score3 += int(LA == L2 && L2 == L3 && L3 == L4) * 2;

        // Patch for the previous rule to avoid bubbles in large cross patterns. Some games use single-side patterns, 
        // so it's best to expand for bilateral judgment (Work in Progress)
        score3 -= int(LB == L1 && L1 == L5 && LA == L2 && L2 == L4)*3; 

        score3 -= int(LA == L1 && LA == L5); // Deduct points if both L1 and L5 are A to avoid excessive scores 
                                           // and prevent the pattern from becoming a diagonal pattern.
        
        // Extra points
        score3 += scoreBonus; // Experience: Even with very close colors, do not add too many points, 
                             // as some Z-shaped crosses may produce bubbles.
    } 

    // Take the maximum of the four scores
    int score = max(max(score1, score2), score3);
    
    return score < 6; // Requires 6 points to be satisfied
}

void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits, out float4 texcol, out float ialpha)
{

  float2 bcoords = floor(coords);

  uint A = src(-1, -1), B = src(+0, -1), C = src(+1, -1);
  uint D = src(-1, +0), E = src(+0, +0), F = src(+1, +0);
  uint G = src(-1, +1), H = src(+0, +1), I = src(+1, +1);

  uint J = E, K = E, L = E, M = E;

  // Explicitly initialize with the central pixel E by default
  uint res = E;
  ialpha = float(res != 0u);
  texcol = unpackUnorm4x8(res);
  
  if (((A ^ E) | (B ^ E) | (C ^ E) | (D ^ E) | (F ^ E) | (G ^ E) | (H ^ E) | (I ^ E)) == 0u) return;

    uint P = src(+0, -2), S = src(+0, +2);
    uint Q = src(-2, +0), R = src(+2, +0);
    uint Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);

	
    // Check the cross state of every 4 pixels in a "field" shape, and pass five surrounding pixels for pattern judgment
    if (A == E && B == D && A != B && countPatternMatches(A, B, C, F, I, H, G)) return;
    if (C == E && B == F && C != B && countPatternMatches(C, B, A, D, G, H, I)) return;
    if (G == E && D == H && G != H && countPatternMatches(G, H, I, F, C, B, A)) return;
    if (I == E && F == H && I != H && countPatternMatches(I, H, G, D, A, B, C)) return;


    // main mmpx logic

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

    // Use conditional weak blending instead of pixel copying to eliminate artifacts on straight lines
    if (Bl < El && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F)) {J=admix2d(B,J); K=admix2d(B,K);}
    if (Hl < El && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F)) {L=admix2d(H,L); M=admix2d(H,M);}
    if (Fl < El && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H)) {K=admix2d(F,K); M=admix2d(F,M);}
    if (Dl < El && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H)) {J=admix2d(D,J); L=admix2d(D,L);}

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


  // select quadrant based on fractional part of texture coordinates
  float2 fpart = frac(coords);
  res = (fpart.x < 0.5f) ? ((fpart.y < 0.5f) ? J : L) : ((fpart.y < 0.5f) ? K : M);

  ialpha = float(res != 0u);
  texcol = unpackUnorm4x8(res);
}

#undef src
)";
  }
  else if (texture_filter == GPUTextureFilter::Scale2x)
  {
    // Based on https://www.scale2x.it/algorithm
    ss << R"(
#define src(xoffs, yoffs) packUnorm4x8(SampleFromVRAM(texpage, clamp(bcoords + float2((xoffs), (yoffs)), uv_limits.xy, uv_limits.zw)))

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
#define src(xoffs, yoffs) packUnorm4x8(SampleFromVRAM(texpage, clamp(bcoords + float2((xoffs), (yoffs)), uv_limits.xy, uv_limits.zw)))

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

std::string GPU_HW_ShaderGen::GenerateBatchFragmentShader(
  GPU_HW::BatchRenderMode render_mode, GPUTransparencyMode transparency, GPU_HW::BatchTextureMode texture_mode,
  GPUTextureFilter texture_filtering, bool is_blended_texture_filtering, bool upscaled, bool msaa,
  bool per_sample_shading, bool uv_limits, bool force_round_texcoords, bool true_color, bool dithering,
  bool scaled_dithering, bool disable_color_perspective, bool interlacing, bool scaled_interlacing, bool check_mask,
  bool write_mask_as_depth, bool use_rov, bool use_rov_depth, bool rov_depth_test, bool rov_depth_write) const
{
  DebugAssert(!true_color || !dithering); // Should not be doing dithering+true color.

  DebugAssert(transparency == GPUTransparencyMode::Disabled || render_mode == GPU_HW::BatchRenderMode::ShaderBlend);
  DebugAssert((!rov_depth_test && !rov_depth_write) || (use_rov && use_rov_depth));

  const bool textured = (texture_mode != GPU_HW::BatchTextureMode::Disabled);
  const bool palette =
    (texture_mode == GPU_HW::BatchTextureMode::Palette4Bit || texture_mode == GPU_HW::BatchTextureMode::Palette8Bit);
  const bool page_texture = (texture_mode == GPU_HW::BatchTextureMode::PageTexture);
  const bool shader_blending = (render_mode == GPU_HW::BatchRenderMode::ShaderBlend);
  const bool use_dual_source = (!shader_blending && !use_rov && m_supports_dual_source_blend &&
                                ((render_mode != GPU_HW::BatchRenderMode::TransparencyDisabled &&
                                  render_mode != GPU_HW::BatchRenderMode::OnlyOpaque) ||
                                 is_blended_texture_filtering));

  std::stringstream ss;
  WriteHeader(ss, use_rov, shader_blending && !use_rov, use_dual_source);
  DefineMacro(ss, "TRANSPARENCY", render_mode != GPU_HW::BatchRenderMode::TransparencyDisabled);
  DefineMacro(ss, "TRANSPARENCY_ONLY_OPAQUE", render_mode == GPU_HW::BatchRenderMode::OnlyOpaque);
  DefineMacro(ss, "TRANSPARENCY_ONLY_TRANSPARENT", render_mode == GPU_HW::BatchRenderMode::OnlyTransparent);
  DefineMacro(ss, "TRANSPARENCY_MODE", static_cast<s32>(transparency));
  DefineMacro(ss, "SHADER_BLENDING", shader_blending);
  DefineMacro(ss, "CHECK_MASK_BIT", check_mask);
  DefineMacro(ss, "TEXTURED", textured);
  DefineMacro(ss, "PALETTE", palette);
  DefineMacro(ss, "PALETTE_4_BIT", texture_mode == GPU_HW::BatchTextureMode::Palette4Bit);
  DefineMacro(ss, "PALETTE_8_BIT", texture_mode == GPU_HW::BatchTextureMode::Palette8Bit);
  DefineMacro(ss, "PAGE_TEXTURE", page_texture);
  DefineMacro(ss, "DITHERING", dithering);
  DefineMacro(ss, "DITHERING_SCALED", dithering && scaled_dithering);
  DefineMacro(ss, "INTERLACING", interlacing);
  DefineMacro(ss, "INTERLACING_SCALED", interlacing && scaled_interlacing);
  DefineMacro(ss, "TRUE_COLOR", true_color);
  DefineMacro(ss, "TEXTURE_FILTERING", texture_filtering != GPUTextureFilter::Nearest);
  DefineMacro(ss, "TEXTURE_ALPHA_BLENDING", is_blended_texture_filtering);
  DefineMacro(ss, "UV_LIMITS", uv_limits);
  DefineMacro(ss, "USE_ROV", use_rov);
  DefineMacro(ss, "USE_ROV_DEPTH", use_rov_depth);
  DefineMacro(ss, "ROV_DEPTH_TEST", rov_depth_test);
  DefineMacro(ss, "ROV_DEPTH_WRITE", rov_depth_write);
  DefineMacro(ss, "USE_DUAL_SOURCE", use_dual_source);
  DefineMacro(ss, "WRITE_MASK_AS_DEPTH", write_mask_as_depth);
  DefineMacro(ss, "FORCE_ROUND_TEXCOORDS", force_round_texcoords);
  DefineMacro(ss, "UPSCALED", upscaled);

  // Used for converting to normalized coordinates for sampling.
  ss << "CONSTANT float2 RCP_VRAM_SIZE = float2(1.0 / float(" << VRAM_WIDTH << "), 1.0 / float(" << VRAM_HEIGHT
     << "));\n";

  WriteColorConversionFunctions(ss);
  WriteBatchUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0);

  if (use_rov)
  {
    DeclareImage(ss, "rov_color", 0);
    if (use_rov_depth)
      DeclareImage(ss, "rov_depth", 1, true);
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

uint2 ApplyTextureWindow(uint2 coords)
{
  uint x = (uint(coords.x) & u_texture_window_and.x) | u_texture_window_or.x;
  uint y = (uint(coords.y) & u_texture_window_and.y) | u_texture_window_or.y;
  return uint2(x, y);
}

uint2 FloatToIntegerCoords(float2 coords)
{
  // With the vertex offset applied at 1x resolution scale, we want to round the texture coordinates.
  // Floor them otherwise, as it currently breaks when upscaling as the vertex offset is not applied.
  return uint2((UPSCALED == 0 || FORCE_ROUND_TEXCOORDS != 0) ? roundEven(coords) : floor(coords));
}

#if PAGE_TEXTURE

float4 SampleFromPageTexture(float2 coords)
{
  // Cached textures.
  uint2 icoord = ApplyTextureWindow(FloatToIntegerCoords(coords));
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

float4 SampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords)
{
  #if PAGE_TEXTURE
    return SampleFromPageTexture(coords);
  #elif PALETTE
    uint2 icoord = ApplyTextureWindow(FloatToIntegerCoords(coords));

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
      uint2 icoord = ApplyTextureWindow(FloatToIntegerCoords(coords));
      uint2 vicoord = (texpage.xy + icoord) & uint2(1023, 511);
      return LOAD_TEXTURE(samp0, int2(vicoord), 0);
    #else
      // Coordinates are already upscaled, we need to downscale them to apply the texture
      // window, then re-upscale/offset. We can't round here, because it could result in
      // going outside of the texture window.
      float2 ncoords = coords * u_rcp_resolution_scale;
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

  const u32 num_fragment_outputs = use_rov ? 0 : (use_dual_source ? 2 : 1);
  if (textured && page_texture)
  {
    if (texture_filtering != GPUTextureFilter::Nearest)
      WriteBatchTextureFilter(ss, texture_filtering);

    if (uv_limits)
    {
      DeclareFragmentEntryPoint(ss, 1, 1, {{"nointerpolation", "float4 v_uv_limits"}}, true, num_fragment_outputs,
                                use_dual_source, write_mask_as_depth, msaa, per_sample_shading, false,
                                disable_color_perspective, shader_blending && !use_rov, use_rov);
    }
    else
    {
      DeclareFragmentEntryPoint(ss, 1, 1, {}, true, num_fragment_outputs, use_dual_source, write_mask_as_depth, msaa,
                                per_sample_shading, false, disable_color_perspective, shader_blending && !use_rov,
                                use_rov);
    }
  }
  else if (textured)
  {
    if (texture_filtering != GPUTextureFilter::Nearest)
      WriteBatchTextureFilter(ss, texture_filtering);

    if (uv_limits)
    {
      DeclareFragmentEntryPoint(ss, 1, 1,
                                {{"nointerpolation", palette ? "uint4 v_texpage" : "uint2 v_texpage"},
                                 {"nointerpolation", "float4 v_uv_limits"}},
                                true, num_fragment_outputs, use_dual_source, write_mask_as_depth, msaa,
                                per_sample_shading, false, disable_color_perspective, shader_blending && !use_rov,
                                use_rov);
    }
    else
    {
      DeclareFragmentEntryPoint(ss, 1, 1, {{"nointerpolation", palette ? "uint4 v_texpage" : "uint2 v_texpage"}}, true,
                                num_fragment_outputs, use_dual_source, write_mask_as_depth, msaa, per_sample_shading,
                                false, disable_color_perspective, shader_blending && !use_rov, use_rov);
    }
  }
  else
  {
    DeclareFragmentEntryPoint(ss, 1, 0, {}, true, num_fragment_outputs, use_dual_source, write_mask_as_depth, msaa,
                              per_sample_shading, false, disable_color_perspective, shader_blending && !use_rov,
                              use_rov);
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
      #if UV_LIMITS
        texcol = SampleFromPageTexture(clamp(v_tex0, v_uv_limits.xy, v_uv_limits.zw));
      #else
        texcol = SampleFromPageTexture(v_tex0);
      #endif
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
      #if UV_LIMITS
        texcol = SampleFromVRAM(v_texpage, clamp(v_tex0, v_uv_limits.xy, v_uv_limits.zw));
      #else
        texcol = SampleFromVRAM(v_texpage, v_tex0);
      #endif
      if (VECTOR_EQ(texcol, TRANSPARENT_PIXEL_COLOR))
        discard;

      ialpha = 1.0;
    #endif

    semitransparent = (texcol.a >= 0.5);

    // If not using true color, truncate the framebuffer colors to 5-bit.
    #if !TRUE_COLOR
      icolor = uint3(texcol.rgb * float3(255.0, 255.0, 255.0)) >> 3;
      icolor = (icolor * vertcol) >> 4;
      #if DITHERING
        icolor = ApplyDithering(fragpos, icolor);
      #else
        icolor = min(icolor >> 3, uint3(31u, 31u, 31u));
      #endif
    #else
      icolor = uint3(texcol.rgb * float3(255.0, 255.0, 255.0));
      icolor = (icolor * vertcol) >> 7;
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
        discarded = (v_pos.z > bg_depth);
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
          ROV_STORE(rov_depth, fragpos, float4(v_pos.z, 0.0, 0.0, 0.0));
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
    o_col1 = float4(LoadDepth(wrapped_coords), 0.0, 0.0, 0.0);
  #endif
}
)";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMReplacementBlitFragmentShader() const
{
  std::stringstream ss;
  WriteHeader(ss);
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

std::string GPU_HW_ShaderGen::GenerateVRAMReadFragmentShader(u32 resolution_scale, u32 multisamples) const
{
  const bool msaa = (multisamples > 1);

  std::stringstream ss;
  WriteHeader(ss);
  WriteColorConversionFunctions(ss);

  DefineMacro(ss, "MULTISAMPLING", msaa);
  ss << "CONSTANT uint RESOLUTION_SCALE = " << resolution_scale << "u;\n";
  ss << "CONSTANT uint MULTISAMPLES = " << multisamples << "u;\n";

  DeclareUniformBuffer(ss, {"uint2 u_base_coords", "uint2 u_size"}, true);
  DeclareTexture(ss, "samp0", 0, msaa);

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
  for (uint offset_x = 0u; offset_x < RESOLUTION_SCALE; offset_x++)
  {
    for (uint offset_y = 0u; offset_y < RESOLUTION_SCALE; offset_y++)
      value += LoadVRAM(int2(base_coords + uint2(offset_x, offset_y)));
  }
  value /= float(RESOLUTION_SCALE * RESOLUTION_SCALE);
  return RGBA8ToRGBA5551(value);
}
)";

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

std::string GPU_HW_ShaderGen::GenerateVRAMWriteFragmentShader(bool use_buffer, bool use_ssbo, bool write_mask_as_depth,
                                                              bool write_depth_as_rt) const
{
  Assert(!write_mask_as_depth || (write_mask_as_depth != write_depth_as_rt));

  std::stringstream ss;
  WriteHeader(ss);
  WriteColorConversionFunctions(ss);

  DefineMacro(ss, "WRITE_MASK_AS_DEPTH", write_mask_as_depth);
  DefineMacro(ss, "WRITE_DEPTH_AS_RT", write_depth_as_rt);
  DefineMacro(ss, "USE_BUFFER", use_buffer);

  ss << "CONSTANT float2 VRAM_SIZE = float2(" << VRAM_WIDTH << ".0, " << VRAM_HEIGHT << ".0);\n";

  DeclareUniformBuffer(ss,
                       {"float2 u_base_coords", "float2 u_end_coords", "float2 u_size", "float u_resolution_scale",
                        "uint u_buffer_base_offset", "uint u_mask_or_bits", "float u_depth_value"},
                       true);

  if (!use_buffer)
  {
    DeclareTexture(ss, "samp0", 0, false, true, true);
  }
  else if (use_ssbo && m_glsl)
  {
    ss << "layout(std430";
    if (IsVulkan())
      ss << ", set = 0, binding = 0";
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
    DeclareTextureBuffer(ss, "samp0", 0, true, true);
    ss << "#define GET_VALUE(buffer_offset) (LOAD_TEXTURE_BUFFER(samp0, int(buffer_offset)).r)\n\n";
  }

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 1 + BoolToUInt32(write_depth_as_rt), false, write_mask_as_depth);
  ss << R"(
{
  float2 coords = floor(v_pos.xy / u_resolution_scale);

  // make sure it's not oversized and out of range
  if ((coords.x < u_base_coords.x && coords.x >= u_end_coords.x) ||
      (coords.y < u_base_coords.y && coords.y >= u_end_coords.y))
  {
    discard;
  }

  // find offset from the start of the row/column
  float2 offset;
  offset.x = (coords.x < u_base_coords.x) ? (VRAM_SIZE.x - u_base_coords.x + coords.x) : (coords.x - u_base_coords.x);
  offset.y = (coords.y < u_base_coords.y) ? (VRAM_SIZE.y - u_base_coords.y + coords.y) : (coords.y - u_base_coords.y);

#if !USE_BUFFER
  uint value = LOAD_TEXTURE(samp0, int2(offset), 0).x;
#else
  uint buffer_offset = u_buffer_base_offset + uint((offset.y * u_size.x) + offset.x);
  uint value = GET_VALUE(buffer_offset) | u_mask_or_bits;
#endif

  o_col0 = RGBA5551ToRGBA8(value);
#if WRITE_MASK_AS_DEPTH
  o_depth = (o_col0.a == 1.0) ? u_depth_value : 0.0;
#elif WRITE_DEPTH_AS_RT
  o_col1 = float4(1.0f, 0.0f, 0.0f, 0.0f);
#endif
})";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMCopyFragmentShader(bool write_mask_as_depth, bool write_depth_as_rt) const
{
  Assert(!write_mask_as_depth || (write_mask_as_depth != write_depth_as_rt));

  // TODO: This won't currently work because we can't bind the texture to both the shader and framebuffer.
  const bool msaa = false;

  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "WRITE_MASK_AS_DEPTH", write_mask_as_depth);
  DefineMacro(ss, "WRITE_DEPTH_AS_RT", write_depth_as_rt);
  DefineMacro(ss, "MSAA_COPY", msaa);

  DeclareUniformBuffer(ss,
                       {"float2 u_src_coords", "float2 u_dst_coords", "float2 u_end_coords", "float2 u_vram_size",
                        "float u_resolution_scale", "bool u_set_mask_bit", "float u_depth_value"},
                       true);

  DeclareTexture(ss, "samp0", 0, msaa);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 1 + BoolToUInt32(write_depth_as_rt), false, write_mask_as_depth, false,
                            false, msaa);
  ss << R"(
{
  float2 dst_coords = floor(v_pos.xy);

  // make sure it's not oversized and out of range
  if ((dst_coords.x < u_dst_coords.x && dst_coords.x >= u_end_coords.x) ||
      (dst_coords.y < u_dst_coords.y && dst_coords.y >= u_end_coords.y))
  {
    discard;
  }

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
#if WRITE_MASK_AS_DEPTH
  o_depth = (u_set_mask_bit ? 1.0f : ((o_col0.a == 1.0) ? u_depth_value : 0.0));
#elif WRITE_DEPTH_AS_RT
  o_col1 = float4(1.0f, 0.0f, 0.0f, 0.0f);
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
    ss, {"uint2 u_dst_coords", "uint2 u_end_coords", "float4 u_fill_color", "uint u_interlaced_displayed_field"}, true);

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
  o_col1 = float4(1.0f, 0.0f, 0.0f, 0.0f);
#endif
})";

  return std::move(ss).str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMUpdateDepthFragmentShader(bool msaa) const
{
  std::stringstream ss;
  WriteHeader(ss);
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

std::string GPU_HW_ShaderGen::GenerateVRAMCopyDepthFragmentShader(bool msaa) const
{
  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "MULTISAMPLED", msaa);
  DeclareTexture(ss, "samp0", 0, msaa);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, msaa, 1, false, false, msaa, msaa, msaa);

  ss << R"(
{
#if MULTISAMPLED
  o_col0 = float4(LOAD_TEXTURE_MS(samp0, int2(v_pos.xy), int(f_sample_index)).r, 0.0, 0.0, 0.0);
#else
  o_col0 = float4(SAMPLE_TEXTURE(samp0, v_tex0).r, 0.0, 0.0, 0.0);
#endif
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
  o_col0 = float4(1.0f, 0.0f, 0.0f, 0.0f);
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