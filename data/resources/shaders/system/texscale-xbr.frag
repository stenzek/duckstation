#version 460 core

layout(location = 0) in VertexData {
  vec2 v_tex0;
};

layout(location = 0) out vec4 dest;

TEXTURE_LAYOUT(0) uniform sampler2D samp0;

vec4 SrcGet(vec2 uv)
{
  return texelFetch(samp0, ivec2(uv), 0);
}

// XBR.pix 
// Copyright 2020 Morgan McGuire & Mara Gagiu, 
// provided under the Open Source MIT license https://opensource.org/licenses/MIT

#define XBR_Y_WEIGHT 48.0
#define XBR_EQ_THRESHOLD 15.0
#define XBR_LV1_COEFFICIENT 0.5
#define XBR_LV2_COEFFICIENT 2.0
// END PARAMETERS //


// XBR GLSL implementation source:
// https://github.com/libretro/glsl-shaders/blob/master/xbr/shaders/xbr-lv2.glsl 
/*
   Hyllian's xBR-lv2 Shader
   Copyright (C) 2011-2015 Hyllian - sergiogdb@gmail.com

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

   Incorporates some of the ideas from SABR shader. Thanks to Joshua Street.

*/

// Uncomment just one of the three params below to choose the corner detection
#define CORNER_A
//#define CORNER_B
//#define CORNER_C
//#define CORNER_D

#ifndef CORNER_A
  #define SMOOTH_TIPS
#endif

#define XBR_SCALE 2.0
#define lv2_cf XBR_LV2_COEFFICIENT
//=================================================================================
// XBR Helper Functions
//=================================================================================
const  float coef    = 2.0;
const  vec3  rgbw          = vec3(14.352, 28.176, 5.472);
const  vec4  eq_threshold  = vec4(15.0, 15.0, 15.0, 15.0);

const  vec4 delta   = vec4(1.0/XBR_SCALE, 1.0/XBR_SCALE, 1.0/XBR_SCALE, 1.0/XBR_SCALE);
const  vec4 delta_l = vec4(0.5/XBR_SCALE, 1.0/XBR_SCALE, 0.5/XBR_SCALE, 1.0/XBR_SCALE);
const  vec4 delta_u = delta_l.yxwz;

const  vec4 Ao = vec4( 1.0, -1.0, -1.0, 1.0 );
const  vec4 Bo = vec4( 1.0,  1.0, -1.0,-1.0 );
const  vec4 Co = vec4( 1.5,  0.5, -0.5, 0.5 );
const  vec4 Ax = vec4( 1.0, -1.0, -1.0, 1.0 );
const  vec4 Bx = vec4( 0.5,  2.0, -0.5,-2.0 );
const  vec4 Cx = vec4( 1.0,  1.0, -0.5, 0.0 );
const  vec4 Ay = vec4( 1.0, -1.0, -1.0, 1.0 );
const  vec4 By = vec4( 2.0,  0.5, -2.0,-0.5 );
const  vec4 Cy = vec4( 2.0,  0.0, -1.0, 0.5 );
const  vec4 Ci = vec4(0.25, 0.25, 0.25, 0.25);

// Difference between vector components.
vec4 df(vec4 A, vec4 B)
{
    return vec4(abs(A-B));
}

// Compare two vectors and return their components are different.
vec4 diff(vec4 A, vec4 B)
{
    return vec4(notEqual(A, B));
}

// Determine if two vector components are equal based on a threshold.
vec4 eq(vec4 A, vec4 B)
{
    return (step(df(A, B), vec4(XBR_EQ_THRESHOLD)));
}

// Determine if two vector components are NOT equal based on a threshold.
vec4 neq(vec4 A, vec4 B)
{
    return (vec4(1.0, 1.0, 1.0, 1.0) - eq(A, B));
}

// Weighted distance.
vec4 wd(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h)
{
    return (df(a,b) + df(a,c) + df(d,e) + df(d,f) + 4.0*df(g,h));
}

float c_df(vec3 c1, vec3 c2) 
{
      vec3 df = abs(c1 - c2);
      return df.r + df.g + df.b;
}

vec4 XBR()
{
    vec4 proxy_dest = vec4(0, 0, 0, 1);
    ivec2 tex_fetch_coords = ivec2(gl_FragCoord.xy / 2.0);
    ivec2 tex_coords = ivec2(gl_FragCoord.xy);



    vec4 edri, edr, edr_l, edr_u, px; // px = pixel, edr = edge detection rule
    vec4 irlv0, irlv1, irlv2l, irlv2u, block_3d;
    vec4 fx, fx_l, fx_u; // inequations of straight lines.

    vec2 fp  = fract(gl_FragCoord.xy / 2.0);

    vec3 A1 = SrcGet(tex_fetch_coords + ivec2(-1, -2)).xyz;
    vec3 B1 = SrcGet(tex_fetch_coords + ivec2( 0, -2)).xyz;
    vec3 C1 = SrcGet(tex_fetch_coords + ivec2(+1, -2)).xyz;
    vec3 A  = SrcGet(tex_fetch_coords + ivec2(-1, -1)).xyz;
    vec3 B  = SrcGet(tex_fetch_coords + ivec2( 0, -1)).xyz;
    vec3 C  = SrcGet(tex_fetch_coords + ivec2(+1, -1)).xyz;
    vec3 D  = SrcGet(tex_fetch_coords + ivec2(-1,  0)).xyz;
    vec4 Eo = SrcGet(tex_fetch_coords);
    vec3 E  = Eo.xyz;
    vec3 F  = SrcGet(tex_fetch_coords + ivec2(+1,  0)).xyz;
    vec3 G  = SrcGet(tex_fetch_coords + ivec2(-1, +1)).xyz;
    vec3 H  = SrcGet(tex_fetch_coords + ivec2( 0, +1)).xyz;
    vec3 I  = SrcGet(tex_fetch_coords + ivec2(+1, +1)).xyz;
    vec3 G5 = SrcGet(tex_fetch_coords + ivec2(-1, +2)).xyz;
    vec3 H5 = SrcGet(tex_fetch_coords + ivec2( 0, +2) ).xyz;
    vec3 I5 = SrcGet(tex_fetch_coords + ivec2(+1, +2)).xyz;
    vec3 A0 = SrcGet(tex_fetch_coords + ivec2(-2, -1)).xyz;
    vec3 D0 = SrcGet(tex_fetch_coords + ivec2(-2,  0)).xyz;
    vec3 G0 = SrcGet(tex_fetch_coords + ivec2(-2, +1)).xyz;
    vec3 C4 = SrcGet(tex_fetch_coords + ivec2(+2, -1)).xyz;
    vec3 F4 = SrcGet(tex_fetch_coords + ivec2(+2,  0)).xyz;
    vec3 I4 = SrcGet(tex_fetch_coords + ivec2(+2, +1)).xyz;

    vec4 b  = vec4(dot(B ,rgbw), dot(D ,rgbw), dot(H ,rgbw), dot(F ,rgbw));
    vec4 c  = vec4(dot(C ,rgbw), dot(A ,rgbw), dot(G ,rgbw), dot(I ,rgbw));
    vec4 d  = b.yzwx;
    vec4 e  = vec4(dot(E,rgbw));
    vec4 f  = b.wxyz;
    vec4 g  = c.zwxy;
    vec4 h  = b.zwxy;
    vec4 i  = c.wxyz;
    vec4 i4 = vec4(dot(I4,rgbw), dot(C1,rgbw), dot(A0,rgbw), dot(G5,rgbw));
    vec4 i5 = vec4(dot(I5,rgbw), dot(C4,rgbw), dot(A1,rgbw), dot(G0,rgbw));
    vec4 h5 = vec4(dot(H5,rgbw), dot(F4,rgbw), dot(B1,rgbw), dot(D0,rgbw));
    vec4 f4 = h5.yzwx;

    // These inequations define the line below which interpolation occurs.
    fx   = (Ao*fp.y+Bo*fp.x); 
    fx_l = (Ax*fp.y+Bx*fp.x);
    fx_u = (Ay*fp.y+By*fp.x);

    irlv1 = irlv0 = diff(e,f) * diff(e,h);
    
#ifdef CORNER_B

    // E1/K case (X odd, Y even) 
    irlv1      = (irlv0 * ( neq(f,b) * neq(h,d) + eq(e,i) * neq(f,i4) * neq(h,i5) + eq(e,g) + eq(e,c) ) );

#endif
#ifdef CORNER_D

    // E3/M case (X odd, Y odd) 
    vec4 c1 = i4.yzwx;
    vec4 g0 = i5.wxyz;
    irlv1     = (irlv0  *  ( neq(f,b) * neq(h,d) + eq(e,i) * neq(f,i4) * neq(h,i5) + eq(e,g) + eq(e,c) ) * (diff(f,f4) * diff(f,i) + diff(h,h5) * diff(h,i) + diff(h,g) + diff(f,c) + eq(b,c1) * eq(d,g0)));

#endif
#ifdef CORNER_C

    irlv1     = (irlv0  * ( neq(f,b) * neq(f,c) + neq(h,d) * neq(h,g) + eq(e,i) * (neq(f,f4) * neq(f,i4) + neq(h,h5) * neq(h,i5)) + eq(e,g) + eq(e,c)) );
    
#endif

    irlv2l = diff(e,g) * diff(d,g);
    irlv2u = diff(e,c) * diff(b,c);

    vec4 fx45i = clamp((fx   + delta   -Co - Ci)/(2.0*delta  ), 0.0, 1.0);
    vec4 fx45  = clamp((fx   + delta   -Co     )/(2.0*delta  ), 0.0, 1.0);
    vec4 fx30  = clamp((fx_l + delta_l -Cx     )/(2.0*delta_l), 0.0, 1.0);
    vec4 fx60  = clamp((fx_u + delta_u -Cy     )/(2.0*delta_u), 0.0, 1.0);

    vec4 wd1 = wd( e, c,  g, i, h5, f4, h, f);
    vec4 wd2 = wd( h, d, i5, f, i4,  b, e, i);

    edri  = step(wd1, wd2) * irlv0;
    edr   = step(wd1 + vec4(0.1, 0.1, 0.1, 0.1), wd2) * step(vec4(0.5, 0.5, 0.5, 0.5), irlv1);
    edr_l = step( lv2_cf*df(f,g), df(h,c) ) * irlv2l * edr;
    edr_u = step( lv2_cf*df(h,c), df(f,g) ) * irlv2u * edr;

    fx45  = edr   * fx45;
    fx30  = edr_l * fx30;
    fx60  = edr_u * fx60;
    fx45i = edri  * fx45i;

    px = step(df(e,f), df(e,h));

#ifdef SMOOTH_TIPS
    //vec4 maximos = max(max(fx30, fx60), max(fx45, fx45i));
#endif
#ifndef SMOOTH_TIPS
    vec4 maximos = max(max(fx30, fx60), fx45);
#endif

    vec3 res1 = E;
    res1 = mix(res1, mix(H, F, px.x), maximos.x);
    res1 = mix(res1, mix(B, D, px.z), maximos.z);
    
    vec3 res2 = E;
    res2 = mix(res2, mix(F, B, px.y), maximos.y);
    res2 = mix(res2, mix(D, H, px.w), maximos.w);
    
    vec3 res = mix(res1, res2, step(c_df(E, res1), c_df(E, res2)));

    proxy_dest.rgb = res;
    proxy_dest.a = Eo.a;
    return proxy_dest;
}

void main () {
    dest = XBR();
}
