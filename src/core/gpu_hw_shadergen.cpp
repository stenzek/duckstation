#include "gpu_hw_shadergen.h"
#include "common/assert.h"
#include <cstdio>
#include <glad.h>

GPU_HW_ShaderGen::GPU_HW_ShaderGen(HostDisplay::RenderAPI render_api, u32 resolution_scale, u32 multisamples,
                                   bool per_sample_shading, bool true_color, bool scaled_dithering,
                                   GPUTextureFilter texture_filtering, bool uv_limits, bool supports_dual_source_blend)
  : ShaderGen(render_api, supports_dual_source_blend), m_resolution_scale(resolution_scale),
    m_multisamples(multisamples), m_true_color(true_color), m_per_sample_shading(per_sample_shading),
    m_scaled_dithering(scaled_dithering), m_texture_filter(texture_filtering), m_uv_limits(uv_limits)
{
}

GPU_HW_ShaderGen::~GPU_HW_ShaderGen() = default;

void GPU_HW_ShaderGen::WriteCommonFunctions(std::stringstream& ss)
{
  DefineMacro(ss, "MULTISAMPLING", UsingMSAA());

  ss << "CONSTANT uint RESOLUTION_SCALE = " << m_resolution_scale << "u;\n";
  ss << "CONSTANT uint2 VRAM_SIZE = uint2(" << VRAM_WIDTH << ", " << VRAM_HEIGHT << ") * RESOLUTION_SCALE;\n";
  ss << "CONSTANT float2 RCP_VRAM_SIZE = float2(1.0, 1.0) / float2(VRAM_SIZE);\n";
  ss << "CONSTANT uint MULTISAMPLES = " << m_multisamples << "u;\n";
  ss << "CONSTANT bool PER_SAMPLE_SHADING = " << (m_per_sample_shading ? "true" : "false") << ";\n";
  ss << R"(

float fixYCoord(float y)
{
#if API_OPENGL || API_OPENGL_ES
  return 1.0 - RCP_VRAM_SIZE.y - y;
#else
  return y;
#endif
}

uint fixYCoord(uint y)
{
#if API_OPENGL || API_OPENGL_ES
  return VRAM_SIZE.y - y - 1u;
#else
  return y;
#endif
}

uint RGBA8ToRGBA5551(float4 v)
{
  uint r = uint(roundEven(v.r * 255.0)) >> 3;
  uint g = uint(roundEven(v.g * 255.0)) >> 3;
  uint b = uint(roundEven(v.b * 255.0)) >> 3;
  uint a = (v.a != 0.0) ? 1u : 0u;
  return (r) | (g << 5) | (b << 10) | (a << 15);
}

float4 RGBA5551ToRGBA8(uint v)
{
  uint r = (v & 31u);
  uint g = ((v >> 5) & 31u);
  uint b = ((v >> 10) & 31u);
  uint a = ((v >> 15) & 1u);

  // repeat lower bits
  r = (r << 3) | (r & 7u);
  g = (g << 3) | (g & 7u);
  b = (b << 3) | (b & 7u);

  return float4(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0, float(a));
}
)";
}

void GPU_HW_ShaderGen::WriteBatchUniformBuffer(std::stringstream& ss)
{
  DeclareUniformBuffer(ss,
                       {"uint2 u_texture_window_and", "uint2 u_texture_window_or", "float u_src_alpha_factor",
                        "float u_dst_alpha_factor", "uint u_interlaced_displayed_field",
                        "bool u_set_mask_while_drawing"},
                       false);
}

std::string GPU_HW_ShaderGen::GenerateBatchVertexShader(bool textured)
{
  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "TEXTURED", textured);
  DefineMacro(ss, "UV_LIMITS", m_uv_limits);

  WriteCommonFunctions(ss);
  WriteBatchUniformBuffer(ss);

  ss << "CONSTANT float EPSILON = 0.00001;\n";

  if (textured)
  {
    if (m_uv_limits)
    {
      DeclareVertexEntryPoint(
        ss, {"float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage", "float4 a_uv_limits"}, 1, 1,
        {{"nointerpolation", "uint4 v_texpage"}, {"nointerpolation", "float4 v_uv_limits"}}, false, "", UsingMSAA(),
        UsingPerSampleShading());
    }
    else
    {
      DeclareVertexEntryPoint(ss, {"float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage"}, 1, 1,
                              {{"nointerpolation", "uint4 v_texpage"}}, false, "", UsingMSAA(),
                              UsingPerSampleShading());
    }
  }
  else
  {
    DeclareVertexEntryPoint(ss, {"float4 a_pos", "float4 a_col0"}, 1, 0, {}, false, "", UsingMSAA(),
                            UsingPerSampleShading());
  }

  ss << R"(
{
  // Offset the vertex position by 0.5 to ensure correct interpolation of texture coordinates
  // at 1x resolution scale. This doesn't work at >1x, we adjust the texture coordinates before
  // uploading there instead.
  float vertex_offset = (RESOLUTION_SCALE == 1u) ? 0.5 : 0.0;

  // 0..+1023 -> -1..1
  float pos_x = ((a_pos.x + vertex_offset) / 512.0) - 1.0;
  float pos_y = ((a_pos.y + vertex_offset) / -256.0) + 1.0;
  float pos_z = a_pos.z;
  float pos_w = a_pos.w;

#if API_OPENGL || API_OPENGL_ES
  // OpenGL seems to be off by one pixel in the Y direction due to lower-left origin, but only on
  // Intel and NVIDIA drivers. AMD is fine...
  pos_y += EPSILON;

  // 0..1 to -1..1 depth range.
  pos_z = (pos_z * 2.0) - 1.0;
#endif

  // NDC space Y flip in Vulkan.
#if API_VULKAN
  pos_y = -pos_y;
#endif

  v_pos = float4(pos_x * pos_w, pos_y * pos_w, pos_z * pos_w, pos_w);

  v_col0 = a_col0;
  #if TEXTURED
    // Fudge the texture coordinates by half a pixel in screen-space.
    // This fixes the rounding/interpolation error on NVIDIA GPUs with shared edges between triangles.
    v_tex0 = float2(float((a_texcoord & 0xFFFFu) * RESOLUTION_SCALE) + EPSILON,
                    float((a_texcoord >> 16) * RESOLUTION_SCALE) + EPSILON);

    // base_x,base_y,palette_x,palette_y
    v_texpage.x = (a_texpage & 15u) * 64u * RESOLUTION_SCALE;
    v_texpage.y = ((a_texpage >> 4) & 1u) * 256u * RESOLUTION_SCALE;
    v_texpage.z = ((a_texpage >> 16) & 63u) * 16u * RESOLUTION_SCALE;
    v_texpage.w = ((a_texpage >> 22) & 511u) * RESOLUTION_SCALE;

    #if UV_LIMITS
      v_uv_limits = a_uv_limits * float4(255.0, 255.0, 255.0, 255.0);
    #endif
  #endif
}
)";

  return ss.str();
}

void GPU_HW_ShaderGen::WriteBatchTextureFilter(std::stringstream& ss, GPUTextureFilter texture_filter)
{
  // JINC2 and xBRZ shaders originally from beetle-psx, modified to support filtering mask channel.
  if (texture_filter == GPUTextureFilter::Bilinear || texture_filter == GPUTextureFilter::BilinearBinAlpha)
  {
    DefineMacro(ss, "BINALPHA", texture_filter == GPUTextureFilter::BilinearBinAlpha);
    ss << R"(
void FilteredSampleFromVRAM(uint4 texpage, float2 coords, float4 uv_limits,
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

#if BINALPHA
  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
#endif
}
)";
  }
  else if (texture_filter == GPUTextureFilter::JINC2 || texture_filter == GPUTextureFilter::JINC2BinAlpha)
  {
    DefineMacro(ss, "BINALPHA", texture_filter == GPUTextureFilter::JINC2BinAlpha);
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
   res = lerp(sin(x*wa)*sin(x*wb)/(x*x), float4(wa*wb, wa*wb, wa*wb, wa*wb), VECTOR_COMP_EQ(x,float4(0.0, 0.0, 0.0, 0.0)));

   return res;
}

void FilteredSampleFromVRAM(uint4 texpage, float2 coords, float4 uv_limits,
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

#if BINALPHA
  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
#endif
}
)";
  }
  else if (texture_filter == GPUTextureFilter::xBR || texture_filter == GPUTextureFilter::xBRBinAlpha)
  {
    DefineMacro(ss, "BINALPHA", texture_filter == GPUTextureFilter::xBRBinAlpha);
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

void FilteredSampleFromVRAM(uint4 texpage, float2 coords, float4 uv_limits,
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

#if BINALPHA
  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
#endif
}

#undef P

)";
  }
}

std::string GPU_HW_ShaderGen::GenerateBatchFragmentShader(GPU_HW::BatchRenderMode transparency,
                                                          GPUTextureMode texture_mode, bool dithering, bool interlacing)
{
  const GPUTextureMode actual_texture_mode = texture_mode & ~GPUTextureMode::RawTextureBit;
  const bool raw_texture = (texture_mode & GPUTextureMode::RawTextureBit) == GPUTextureMode::RawTextureBit;
  const bool textured = (texture_mode != GPUTextureMode::Disabled);
  const bool use_dual_source =
    m_supports_dual_source_blend && ((transparency != GPU_HW::BatchRenderMode::TransparencyDisabled &&
                                      transparency != GPU_HW::BatchRenderMode::OnlyOpaque) ||
                                     m_texture_filter != GPUTextureFilter::Nearest);

  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "TRANSPARENCY", transparency != GPU_HW::BatchRenderMode::TransparencyDisabled);
  DefineMacro(ss, "TRANSPARENCY_ONLY_OPAQUE", transparency == GPU_HW::BatchRenderMode::OnlyOpaque);
  DefineMacro(ss, "TRANSPARENCY_ONLY_TRANSPARENT", transparency == GPU_HW::BatchRenderMode::OnlyTransparent);
  DefineMacro(ss, "TEXTURED", textured);
  DefineMacro(ss, "PALETTE",
              actual_texture_mode == GPUTextureMode::Palette4Bit || actual_texture_mode == GPUTextureMode::Palette8Bit);
  DefineMacro(ss, "PALETTE_4_BIT", actual_texture_mode == GPUTextureMode::Palette4Bit);
  DefineMacro(ss, "PALETTE_8_BIT", actual_texture_mode == GPUTextureMode::Palette8Bit);
  DefineMacro(ss, "RAW_TEXTURE", raw_texture);
  DefineMacro(ss, "DITHERING", dithering);
  DefineMacro(ss, "DITHERING_SCALED", m_scaled_dithering);
  DefineMacro(ss, "INTERLACING", interlacing);
  DefineMacro(ss, "TRUE_COLOR", m_true_color);
  DefineMacro(ss, "TEXTURE_FILTERING", m_texture_filter != GPUTextureFilter::Nearest);
  DefineMacro(ss, "UV_LIMITS", m_uv_limits);
  DefineMacro(ss, "USE_DUAL_SOURCE", use_dual_source);

  WriteCommonFunctions(ss);
  WriteBatchUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0);

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
  #if DITHERING_SCALED
    uint2 fc = coord & uint2(3u, 3u);
  #else
    uint2 fc = (coord / uint2(RESOLUTION_SCALE, RESOLUTION_SCALE)) & uint2(3u, 3u);
  #endif
  int offset = s_dither_values[fc.y * 4u + fc.x];

  #if !TRUE_COLOR
    return uint3(clamp((int3(icol) + int3(offset, offset, offset)) >> 3, 0, 31));
  #else
    return uint3(clamp(int3(icol) + int3(offset, offset, offset), 0, 255));
  #endif
}

#if TEXTURED
CONSTANT float4 TRANSPARENT_PIXEL_COLOR = float4(0.0, 0.0, 0.0, 0.0);

uint2 ApplyTextureWindow(uint2 coords)
{
  uint x = (uint(coords.x) & u_texture_window_and.x) | u_texture_window_or.x;
  uint y = (uint(coords.y) & u_texture_window_and.y) | u_texture_window_or.y;
  return uint2(x, y);
}

uint2 ApplyUpscaledTextureWindow(uint2 coords)
{
  uint2 native_coords = coords / uint2(RESOLUTION_SCALE, RESOLUTION_SCALE);
  uint2 coords_offset = coords % uint2(RESOLUTION_SCALE, RESOLUTION_SCALE);
  return (ApplyTextureWindow(native_coords) * uint2(RESOLUTION_SCALE, RESOLUTION_SCALE)) + coords_offset;
}

uint2 FloatToIntegerCoords(float2 coords)
{
  // With the vertex offset applied at 1x resolution scale, we want to round the texture coordinates.
  // Floor them otherwise, as it currently breaks when upscaling as the vertex offset is not applied.
  return uint2((RESOLUTION_SCALE == 1u) ? roundEven(coords) : floor(coords));
}

float4 SampleFromVRAM(uint4 texpage, float2 coords)
{
  #if PALETTE
    uint2 icoord = ApplyTextureWindow(FloatToIntegerCoords(coords));
    uint2 index_coord = icoord;
    #if PALETTE_4_BIT
      index_coord.x /= 4u;
    #elif PALETTE_8_BIT
      index_coord.x /= 2u;
    #endif

    // fixup coords
    uint2 vicoord = uint2(texpage.x + index_coord.x * RESOLUTION_SCALE, fixYCoord(texpage.y + index_coord.y * RESOLUTION_SCALE));

    // load colour/palette
    float4 texel = SAMPLE_TEXTURE(samp0, float2(vicoord) * RCP_VRAM_SIZE);
    uint vram_value = RGBA8ToRGBA5551(texel);

    // apply palette
    #if PALETTE_4_BIT
      uint subpixel = icoord.x & 3u;
      uint palette_index = (vram_value >> (subpixel * 4u)) & 0x0Fu;
    #elif PALETTE_8_BIT
      uint subpixel = icoord.x & 1u;
      uint palette_index = (vram_value >> (subpixel * 8u)) & 0xFFu;
    #endif

    // sample palette
    uint2 palette_icoord = uint2(texpage.z + (palette_index * RESOLUTION_SCALE), fixYCoord(texpage.w));
    return SAMPLE_TEXTURE(samp0, float2(palette_icoord) * RCP_VRAM_SIZE);
  #else
    // Direct texturing. Render-to-texture effects. Use upscaled coordinates.
    uint2 icoord = ApplyUpscaledTextureWindow(FloatToIntegerCoords(coords));    
    uint2 direct_icoord = uint2(texpage.x + icoord.x, fixYCoord(texpage.y + icoord.y));
    return SAMPLE_TEXTURE(samp0, float2(direct_icoord) * RCP_VRAM_SIZE);
  #endif
}

#endif
)";

  if (textured)
  {
    if (m_texture_filter != GPUTextureFilter::Nearest)
      WriteBatchTextureFilter(ss, m_texture_filter);

    if (m_uv_limits)
    {
      DeclareFragmentEntryPoint(ss, 1, 1,
                                {{"nointerpolation", "uint4 v_texpage"}, {"nointerpolation", "float4 v_uv_limits"}},
                                true, use_dual_source ? 2 : 1, true, UsingMSAA(), UsingPerSampleShading());
    }
    else
    {
      DeclareFragmentEntryPoint(ss, 1, 1, {{"nointerpolation", "uint4 v_texpage"}}, true, use_dual_source ? 2 : 1, true,
                                UsingMSAA(), UsingPerSampleShading());
    }
  }
  else
  {
    DeclareFragmentEntryPoint(ss, 1, 0, {}, true, use_dual_source ? 2 : 1, true, UsingMSAA(), UsingPerSampleShading());
  }

  ss << R"(
{
  uint3 vertcol = uint3(v_col0.rgb * float3(255.0, 255.0, 255.0));

  bool semitransparent;
  uint3 icolor;
  float ialpha;
  float oalpha;

  #if INTERLACING
    if ((fixYCoord(uint(v_pos.y)) & 1u) == u_interlaced_displayed_field)
      discard;
  #endif

  #if TEXTURED

    // We can't currently use upscaled coordinate for palettes because of how they're packed.
    // Not that it would be any benefit anyway, render-to-texture effects don't use palettes.
    float2 coords = v_tex0;
    #if PALETTE
      coords /= float2(RESOLUTION_SCALE, RESOLUTION_SCALE);
    #endif

    #if UV_LIMITS
      float4 uv_limits = v_uv_limits;
      #if !PALETTE
        uv_limits *= float4(RESOLUTION_SCALE, RESOLUTION_SCALE, RESOLUTION_SCALE, RESOLUTION_SCALE);
      #endif
    #endif

    float4 texcol;
    #if TEXTURE_FILTERING
      FilteredSampleFromVRAM(v_texpage, coords, uv_limits, texcol, ialpha);
      if (ialpha < 0.5)
        discard;
    #else
      #if UV_LIMITS
        texcol = SampleFromVRAM(v_texpage, clamp(coords, uv_limits.xy, uv_limits.zw));
      #else
        texcol = SampleFromVRAM(v_texpage, coords);
      #endif
      if (VECTOR_EQ(texcol, TRANSPARENT_PIXEL_COLOR))
        discard;

      ialpha = 1.0;
    #endif

    semitransparent = (texcol.a >= 0.5);

    // If not using true color, truncate the framebuffer colors to 5-bit.
    #if !TRUE_COLOR
      icolor = uint3(texcol.rgb * float3(255.0, 255.0, 255.0)) >> 3;
      #if !RAW_TEXTURE
        icolor = (icolor * vertcol) >> 4;
        #if DITHERING
          icolor = ApplyDithering(uint2(v_pos.xy), icolor);
        #else
          icolor = min(icolor >> 3, uint3(31u, 31u, 31u));
        #endif
      #endif
    #else
      icolor = uint3(texcol.rgb * float3(255.0, 255.0, 255.0));
      #if !RAW_TEXTURE
        icolor = (icolor * vertcol) >> 7;
        #if DITHERING
          icolor = ApplyDithering(uint2(v_pos.xy), icolor);
        #else
          icolor = min(icolor, uint3(255u, 255u, 255u));
        #endif
      #endif
    #endif

    // Compute output alpha (mask bit)
    oalpha = float(u_set_mask_while_drawing ? 1 : int(semitransparent));
  #else
    // All pixels are semitransparent for untextured polygons.
    semitransparent = true;
    icolor = vertcol;
    ialpha = 1.0;

    #if DITHERING
      icolor = ApplyDithering(uint2(v_pos.xy), icolor);
    #else
      #if !TRUE_COLOR
        icolor >>= 3;
      #endif
    #endif

    // However, the mask bit is cleared if set mask bit is false.
    oalpha = float(u_set_mask_while_drawing);
  #endif

  // Premultiply alpha so we don't need to use a colour output for it.
  float premultiply_alpha = ialpha;
  #if TRANSPARENCY
    premultiply_alpha = ialpha * (semitransparent ? u_src_alpha_factor : 1.0);
  #endif

  float3 color;
  #if !TRUE_COLOR
    // We want to apply the alpha before the truncation to 16-bit, otherwise we'll be passing a 32-bit precision color
    // into the blend unit, which can cause a small amount of error to accumulate.
    color = floor(float3(icolor) * premultiply_alpha) / float3(31.0, 31.0, 31.0);
  #else
    // True color is actually simpler here since we want to preserve the precision.
    color = (float3(icolor) * premultiply_alpha) / float3(255.0, 255.0, 255.0);
  #endif

  #if TRANSPARENCY
    // Apply semitransparency. If not a semitransparent texel, destination alpha is ignored.
    if (semitransparent)
    {
      #if TRANSPARENCY_ONLY_OPAQUE
        discard;
      #endif

      #if USE_DUAL_SOURCE
        o_col0 = float4(color, oalpha);
        o_col1 = float4(0.0, 0.0, 0.0, u_dst_alpha_factor / ialpha);
      #else
        o_col0 = float4(color, u_dst_alpha_factor / ialpha);
      #endif

      o_depth = oalpha * v_pos.z;
    }
    else
    {
      #if TRANSPARENCY_ONLY_TRANSPARENT
        discard;
      #endif

      #if TRANSPARENCY_ONLY_OPAQUE
        // We don't output the second color here because it's not used (except for filtering).
        o_col0 = float4(color, oalpha);
        #if USE_DUAL_SOURCE
          o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
        #endif
      #else
        #if USE_DUAL_SOURCE
          o_col0 = float4(color, oalpha);
          o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
        #else
          o_col0 = float4(color, 1.0 - ialpha);
        #endif
      #endif

      o_depth = oalpha * v_pos.z;
    }
  #else
    // Non-transparency won't enable blending so we can write the mask here regardless.
    o_col0 = float4(color, oalpha);

    #if USE_DUAL_SOURCE
      o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
    #endif

    o_depth = oalpha * v_pos.z;
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateInterlacedFillFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteCommonFunctions(ss);
  DeclareUniformBuffer(ss, {"float4 u_fill_color", "uint u_interlaced_displayed_field"}, true);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 1, true);

  ss << R"(
{
  if ((fixYCoord(uint(v_pos.y)) & 1u) == u_interlaced_displayed_field)
    discard;

  o_col0 = u_fill_color;
  o_depth = u_fill_color.a;
}
)";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateDisplayFragmentShader(bool depth_24bit,
                                                            GPU_HW::InterlacedRenderMode interlace_mode,
                                                            bool smooth_chroma)
{
  std::stringstream ss;
  WriteHeader(ss);
  DefineMacro(ss, "DEPTH_24BIT", depth_24bit);
  DefineMacro(ss, "INTERLACED", interlace_mode != GPU_HW::InterlacedRenderMode::None);
  DefineMacro(ss, "INTERLEAVED", interlace_mode == GPU_HW::InterlacedRenderMode::InterleavedFields);
  DefineMacro(ss, "SMOOTH_CHROMA", smooth_chroma);

  WriteCommonFunctions(ss);
  DeclareUniformBuffer(ss, {"uint2 u_vram_offset", "uint u_crop_left", "uint u_field_offset"}, true);
  DeclareTexture(ss, "samp0", 0, UsingMSAA());

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

float4 LoadVRAM(int2 coords)
{
#if MULTISAMPLING
  float4 value = LOAD_TEXTURE_MS(samp0, coords, 0u);
  for (uint sample_index = 1u; sample_index < MULTISAMPLES; sample_index++)
    value += LOAD_TEXTURE_MS(samp0, coords, sample_index);
  value /= float(MULTISAMPLES);
  return value;
#else
  return LOAD_TEXTURE(samp0, coords, 0);
#endif
}

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

float3 SampleVRAMAverage2x2(uint2 icoords)
{
  float3 value = SampleVRAM24(icoords);
  value += SampleVRAM24(icoords + uint2(0, 1));
  value += SampleVRAM24(icoords + uint2(1, 0));
  value += SampleVRAM24(icoords + uint2(1, 1));
  return value * 0.25;
}

float3 SampleVRAM24Smoothed(uint2 icoords)
{
  int2 base = int2(icoords) - 1;
  uint2 low = uint2(max(base & ~1, int2(0, 0)));
  uint2 high = low + 2u;
  float2 coeff = vec2(base & 1) * 0.5 + 0.25;

  float3 p = SampleVRAM24(icoords);
  float3 p00 = SampleVRAMAverage2x2(low);
  float3 p01 = SampleVRAMAverage2x2(uint2(low.x, high.y));
  float3 p10 = SampleVRAMAverage2x2(uint2(high.x, low.y));
  float3 p11 = SampleVRAMAverage2x2(high);

  float3 s = lerp(lerp(p00, p10, coeff.x),
                  lerp(p01, p11, coeff.x),
                  coeff.y);

  float y = RGBToYUV(p).x;
  float2 uv = RGBToYUV(s).yz;
  return YUVToRGB(float3(y, uv));
}
)";

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 1);
  ss << R"(
{
  uint2 icoords = uint2(v_pos.xy) + uint2(u_crop_left, 0u);

  #if INTERLACED
    if ((fixYCoord(icoords.y) & 1u) != u_field_offset)
      discard;

    #if !INTERLEAVED
      icoords.y /= 2u;
    #else
      icoords.y &= ~1u;
    #endif
  #endif

  #if DEPTH_24BIT
    #if SMOOTH_CHROMA
      o_col0 = float4(SampleVRAM24Smoothed(icoords), 1.0);
    #else
      o_col0 = float4(SampleVRAM24(icoords), 1.0);
    #endif    
  #else
    o_col0 = float4(LoadVRAM(int2((icoords + u_vram_offset) % VRAM_SIZE)).rgb, 1.0);
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMReadFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteCommonFunctions(ss);
  DeclareUniformBuffer(ss, {"uint2 u_base_coords", "uint2 u_size"}, true);

  DeclareTexture(ss, "samp0", 0, UsingMSAA());

  ss << R"(
float4 LoadVRAM(int2 coords)
{
#if MULTISAMPLING
  float4 value = LOAD_TEXTURE_MS(samp0, coords, 0u);
  for (uint sample_index = 1u; sample_index < MULTISAMPLES; sample_index++)
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

  #if API_OPENGL || API_OPENGL_ES
    // Lower-left origin flip for OpenGL.
    // We want to write the image out upside-down so we can read it top-to-bottom.
    sample_coords.y = u_size.y - sample_coords.y - 1u;
  #endif

  sample_coords += u_base_coords;

  // We're encoding as 32-bit, so the output width is halved and we pack two 16-bit pixels in one 32-bit pixel.
  uint left = SampleVRAM(sample_coords);
  uint right = SampleVRAM(uint2(sample_coords.x + 1u, sample_coords.y));

  o_col0 = float4(float(left & 0xFFu), float((left >> 8) & 0xFFu),
                  float(right & 0xFFu), float((right >> 8) & 0xFFu))
            / float4(255.0, 255.0, 255.0, 255.0);
})";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMWriteFragmentShader(bool use_ssbo)
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteCommonFunctions(ss);
  DeclareUniformBuffer(ss,
                       {"uint2 u_base_coords", "uint2 u_end_coords", "uint2 u_size", "uint u_buffer_base_offset",
                        "uint u_mask_or_bits", "float u_depth_value"},
                       true);

  if (use_ssbo && m_glsl)
  {
    ss << "layout(std430";
    if (IsVulkan())
      ss << ", set = 0, binding = 0";
    else if (m_use_glsl_binding_layout)
      ss << ", binding = 0";

    ss << ") buffer SSBO {\n";
    ss << "  uint ssbo_data[];\n";
    ss << "};\n\n";

    ss << "#define GET_VALUE(buffer_offset) (ssbo_data[(buffer_offset) / 2u] >> (((buffer_offset) % 2u) * 16u))\n\n";
  }
  else
  {
    DeclareTextureBuffer(ss, "samp0", 0, true, true);
    ss << "#define GET_VALUE(buffer_offset) (LOAD_TEXTURE_BUFFER(samp0, int(buffer_offset)).r)\n\n";
  }

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 1, true);
  ss << R"(
{
  uint2 coords = uint2(uint(v_pos.x) / RESOLUTION_SCALE, fixYCoord(uint(v_pos.y)) / RESOLUTION_SCALE);

  // make sure it's not oversized and out of range
  if ((coords.x < u_base_coords.x && coords.x >= u_end_coords.x) ||
      (coords.y < u_base_coords.y && coords.y >= u_end_coords.y))
  {
    discard;
  }


  // find offset from the start of the row/column
  uint2 offset;
  offset.x = (coords.x < u_base_coords.x) ? ((VRAM_SIZE.x / RESOLUTION_SCALE) - u_base_coords.x + coords.x) : (coords.x - u_base_coords.x);
  offset.y = (coords.y < u_base_coords.y) ? ((VRAM_SIZE.y / RESOLUTION_SCALE) - u_base_coords.y + coords.y) : (coords.y - u_base_coords.y);

  uint buffer_offset = u_buffer_base_offset + (offset.y * u_size.x) + offset.x;
  uint value = GET_VALUE(buffer_offset) | u_mask_or_bits;
  
  o_col0 = RGBA5551ToRGBA8(value);
  o_depth = (o_col0.a == 1.0) ? u_depth_value : 0.0;
})";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMCopyFragmentShader()
{
  // TODO: This won't currently work because we can't bind the texture to both the shader and framebuffer.
  const bool msaa = false;

  std::stringstream ss;
  WriteHeader(ss);
  WriteCommonFunctions(ss);
  DeclareUniformBuffer(ss,
                       {"uint2 u_src_coords", "uint2 u_dst_coords", "uint2 u_end_coords", "uint2 u_size",
                        "bool u_set_mask_bit", "float u_depth_value"},
                       true);

  DeclareTexture(ss, "samp0", 0, msaa);
  DefineMacro(ss, "MSAA_COPY", msaa);
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 1, true, false, false, msaa);
  ss << R"(
{
  uint2 dst_coords = uint2(v_pos.xy);

  // make sure it's not oversized and out of range
  if ((dst_coords.x < u_dst_coords.x && dst_coords.x >= u_end_coords.x) ||
      (dst_coords.y < u_dst_coords.y && dst_coords.y >= u_end_coords.y))
  {
    discard;
  }

  // find offset from the start of the row/column
  uint2 offset;
  offset.x = (dst_coords.x < u_dst_coords.x) ? (VRAM_SIZE.x - u_dst_coords.x + dst_coords.x) : (dst_coords.x - u_dst_coords.x);
  offset.y = (dst_coords.y < u_dst_coords.y) ? (VRAM_SIZE.y - u_dst_coords.y + dst_coords.y) : (dst_coords.y - u_dst_coords.y);

  // find the source coordinates to copy from
  uint2 src_coords = (u_src_coords + offset) % VRAM_SIZE;

  // sample and apply mask bit
#if MSAA_COPY
  float4 color = LOAD_TEXTURE_MS(samp0, int2(src_coords), f_sample_index);
#else
  float4 color = LOAD_TEXTURE(samp0, int2(src_coords), 0);
#endif
  o_col0 = float4(color.xyz, u_set_mask_bit ? 1.0 : color.a);
  o_depth = (u_set_mask_bit ? 1.0f : ((o_col0.a == 1.0) ? u_depth_value : 0.0));
})";

  return ss.str();
}

std::string GPU_HW_ShaderGen::GenerateVRAMUpdateDepthFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteCommonFunctions(ss);
  DeclareTexture(ss, "samp0", 0, UsingMSAA());
  DeclareFragmentEntryPoint(ss, 0, 1, {}, true, 0, true, false, false, UsingMSAA());

  ss << R"(
{
#if MULTISAMPLING
  o_depth = LOAD_TEXTURE_MS(samp0, int2(v_pos.xy), f_sample_index).a;
#else
  o_depth = LOAD_TEXTURE(samp0, int2(v_pos.xy), 0).a;
#endif
}
)";

  return ss.str();
}
