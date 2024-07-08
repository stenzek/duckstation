#include "ReShade.fxh"

/*
  *******  Super XBR Shader  *******
   
  Copyright (c) 2015-2024 Hyllian - sergiogdb@gmail.com

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


uniform float XBR_EDGE_STR_P0 <
    ui_category = "Super-xBR:";
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 5.0;
    ui_step = 0.5;
    ui_label = "Xbr - Edge Strength";
> = 5.0;

uniform float XBR_WEIGHT <
    ui_category = "Super-xBR:";
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.1;
    ui_label = "Xbr - Filter Weight";
> = 1.0;


uniform float JINC2_WINDOW_SINC <
    ui_category = "Jinc2:";
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.01;
    ui_label = "Window Sinc Param";
> = 0.5;

uniform float JINC2_SINC <
    ui_category = "Jinc2:";
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.01;
    ui_label = "Sinc Param";
> = 0.88;

uniform float JINC2_AR_STRENGTH <
    ui_category = "Jinc2:";
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.01;
    ui_label = "Anti-ringing Strength";
> = 0.5;

uniform float2 BufferToViewportRatio < source = "buffer_to_viewport_ratio"; >;
uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;

texture2D tBackBufferY{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA8;};
sampler2D sBackBufferY{Texture=tBackBufferY;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=POINT;MinFilter=POINT;};

texture2D tSuper_xBR_P0 < pooled = true; > {Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA8;};
sampler2D sSuper_xBR_P0{Texture=tSuper_xBR_P0;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=POINT;MinFilter=POINT;};

texture2D tSuper_xBR_P1 < pooled = true; > {Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA8;};
sampler2D sSuper_xBR_P1{Texture=tSuper_xBR_P1;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=POINT;MinFilter=POINT;};

texture2D tSuper_xBR_P2 < pooled = true; > {Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA8;};
sampler2D sSuper_xBR_P2{Texture=tSuper_xBR_P2;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=POINT;MinFilter=POINT;};

#define weight1 (XBR_WEIGHT*1.29633/10.0)
#define weight2 (XBR_WEIGHT*1.75068/10.0/2.0)
#define limits  (XBR_EDGE_STR_P0+0.000001)

static const float3 Y = float3(.2126,.7152,.0722);
static const float wp0[6] = {2.0, 1.0, -1.0, 4.0, -1.0, 1.0};
static const float wp1[6] = {1.0, 0.0,  0.0, 0.0,  0.0, 0.0};
static const float wp2[6] = {0.0, 0.0,  0.0, 1.0,  0.0, 0.0};

float luma(float3 color)
{
    return dot(color, Y);
}

float df(float A, float B)
{
    return abs(A-B);
}

/*
                             P1
     |P0|B |C |P1|         C     F4          |a0|b1|c2|d3|
     |D |E |F |F4|      B     F     I4       |b0|c1|d2|e3|   |e1|i1|i2|e2|
     |G |H |I |I4|   P0    E  A  I     P3    |c0|d1|e2|f3|   |e3|i3|i4|e4|
     |P2|H5|I5|P3|      D     H     I5       |d0|e1|f2|g3|
                          G     H5
                             P2
*/


float d_wd(float wp[6], float b0, float b1, float c0, float c1, float c2, float d0, float d1, float d2, float d3, float e1, float e2, float e3, float f2, float f3)
{
    return (wp[0]*(df(c1,c2) + df(c1,c0) + df(e2,e1) + df(e2,e3)) + wp[1]*(df(d2,d3) + df(d0,d1)) + wp[2]*(df(d1,d3) + df(d0,d2)) + wp[3]*df(d1,d2) + wp[4]*(df(c0,c2) + df(e1,e3)) + wp[5]*(df(b0,b1) + df(f2,f3)));
}


float hv_wd(float wp[6], float i1, float i2, float i3, float i4, float e1, float e2, float e3, float e4)
{
    return ( wp[3]*(df(i1,i2)+df(i3,i4)) + wp[0]*(df(i1,e1)+df(i2,e2)+df(i3,e3)+df(i4,e4)) + wp[2]*(df(i1,e2)+df(i3,e4)+df(e1,i2)+df(e3,i4)));
}

float3 min4(float3 a, float3 b, float3 c, float3 d)
{
    return min(a, min(b, min(c, d)));
}

float3 max4(float3 a, float3 b, float3 c, float3 d)
{
    return max(a, max(b, max(c, d)));
}

float max4float(float a, float b, float c, float d)
{
    return max(a, max(b, max(c, d)));
}

float3 super_xbr(float wp[6], float4 P0, float4  B, float4  C, float4 P1, float4  D, float4  E, float4  F, float4 F4, float4  G, float4  H, float4  I, float4 I4, float4 P2, float4 H5, float4 I5, float4 P3)
{
    float b  =  B.a; float  c =  C.a; float  d =  D.a; float  e =  E.a;
    float f  =  F.a; float  g =  G.a; float  h =  H.a; float  i =  I.a;
    float i4 = I4.a; float p0 = P0.a; float i5 = I5.a; float p1 = P1.a;
    float h5 = H5.a; float p2 = P2.a; float f4 = F4.a; float p3 = P3.a;

    /* Calc edgeness in diagonal directions. */
    float d_edge  = (d_wd( wp, d, b, g, e, c, p2, h, f, p1, h5, i, f4, i5, i4 ) - d_wd( wp, c, f4, b, f, i4, p0, e, i, p3, d, h, i5, g, h5 ));

    /* Calc edgeness in horizontal/vertical directions. */
    float hv_edge = (hv_wd(wp, f, i, e, h, c, i5, b, h5) - hv_wd(wp, e, f, h, i, d, f4, g, i4));

    float edge_strength = smoothstep(0.0, limits, abs(d_edge));
    
    float4 w1, w2;
    float3 c3, c4;

    /* Filter weights. Two taps only. */
    w1 = float4(-weight1, weight1+0.50, weight1+0.50, -weight1);
    w2 = float4(-weight2, weight2+0.25, weight2+0.25, -weight2);
    c3 = mul(w2, float4x3(D.rgb+G.rgb, E.rgb+H.rgb, F.rgb+I.rgb, F4.rgb+I4.rgb));
    c4 = mul(w2, float4x3(C.rgb+B.rgb, F.rgb+E.rgb, I.rgb+H.rgb, I5.rgb+H5.rgb));

    /* Filtering and normalization in four direction generating four colors. */
    float3 c1 = mul(w1, float4x3( P2.rgb,   H.rgb,   F.rgb,   P1.rgb ));
    float3 c2 = mul(w1, float4x3( P0.rgb,   E.rgb,   I.rgb,   P3.rgb ));

    /* Smoothly blends the two strongest directions (one in diagonal and the other in vert/horiz direction). */
    float3 color =  lerp(lerp(c1, c2, step(0.0, d_edge)), lerp(c3, c4, step(0.0, hv_edge)), 1. - edge_strength);

    /* Anti-ringing code. */
    float3 min_sample = min4( E.rgb, F.rgb, H.rgb, I.rgb );
    float3 max_sample = max4( E.rgb, F.rgb, H.rgb, I.rgb );
    color = clamp(color, min_sample, max_sample);

    return color;
}

float4 PS_BackBufferY(float4 pos: SV_Position, float2 vTexCoord : TEXCOORD) : SV_Target
{
    float2 tc = (floor(vTexCoord / NormalizedNativePixelSize)+float2(0.5,0.5)) * NormalizedNativePixelSize;

    float3 color = tex2D(ReShade::BackBuffer, tc).rgb;

    return float4(color, luma(color));
}


float4 PS_Super_xBR_P0(float4 pos: SV_Position, float2 vTexCoord : TEXCOORD) : SV_Target
{
    float2 ps = NormalizedNativePixelSize;

    float4 P0 = tex2D(sBackBufferY, vTexCoord.xy + ps*float2(-1.0, -1.0));
    float4  B = tex2D(sBackBufferY, vTexCoord.xy + ps*float2( 0.0, -1.0));
    float4  C = tex2D(sBackBufferY, vTexCoord.xy + ps*float2( 1.0, -1.0));
    float4 P1 = tex2D(sBackBufferY, vTexCoord.xy + ps*float2( 2.0, -1.0));

    float4  D = tex2D(sBackBufferY, vTexCoord.xy + ps*float2(-1.0,  0.0));
    float4  E = tex2D(sBackBufferY, vTexCoord.xy                        );
    float4  F = tex2D(sBackBufferY, vTexCoord.xy + ps*float2( 1.0,  0.0));
    float4 F4 = tex2D(sBackBufferY, vTexCoord.xy + ps*float2( 2.0,  0.0));

    float4  G = tex2D(sBackBufferY, vTexCoord.xy + ps*float2(-1.0,  1.0));
    float4  H = tex2D(sBackBufferY, vTexCoord.xy + ps*float2( 0.0,  1.0));
    float4  I = tex2D(sBackBufferY, vTexCoord.xy + ps*float2( 1.0,  1.0));
    float4 I4 = tex2D(sBackBufferY, vTexCoord.xy + ps*float2( 2.0,  1.0));

    float4 P2 = tex2D(sBackBufferY, vTexCoord.xy + ps*float2(-1.0,  2.0));
    float4 H5 = tex2D(sBackBufferY, vTexCoord.xy + ps*float2( 0.0,  2.0));
    float4 I5 = tex2D(sBackBufferY, vTexCoord.xy + ps*float2( 1.0,  2.0));
    float4 P3 = tex2D(sBackBufferY, vTexCoord.xy + ps*float2( 2.0,  2.0));

    float3 color = super_xbr(wp0, P0,  B,  C, P1,  D,  E,  F, F4,  G,  H,  I, I4, P2, H5, I5, P3);

    return float4(color, luma(color));
}




float4 PS_Super_xBR_P1(float4 pos: SV_Position, float2 vTexCoord : TEXCOORD) : SV_Target
{
    float2 ps = NormalizedNativePixelSize;

    //Skip pixels on wrong grid
    float2 fp  = frac(vTexCoord.xy / ps);
    float2 dir = fp - float2(0.5,0.5);

    if ((dir.x*dir.y)>0.0)
    {
     return lerp( tex2D(sBackBufferY, vTexCoord), tex2D(sSuper_xBR_P0, vTexCoord), step(0.0, dir.x));
    }
    else
    {
        float2 g1 = (fp.x>0.5) ? float2(0.5*ps.x, 0.0) : float2(0.0, 0.5*ps.y);
        float2 g2 = (fp.x>0.5) ? float2(0.0, 0.5*ps.y) : float2(0.5*ps.x, 0.0);

        float4 P0 = tex2D( sBackBufferY, vTexCoord -3.0*g1        );
        float4 P1 = tex2D(sSuper_xBR_P0, vTexCoord         -3.0*g2);
        float4 P2 = tex2D(sSuper_xBR_P0, vTexCoord         +3.0*g2);
        float4 P3 = tex2D( sBackBufferY, vTexCoord +3.0*g1        );

        float4  B = tex2D(sSuper_xBR_P0, vTexCoord -2.0*g1     -g2);
        float4  C = tex2D( sBackBufferY, vTexCoord     -g1 -2.0*g2);
        float4  D = tex2D(sSuper_xBR_P0, vTexCoord -2.0*g1     +g2);
        float4  E = tex2D( sBackBufferY, vTexCoord     -g1        );
        float4  F = tex2D(sSuper_xBR_P0, vTexCoord             -g2);
        float4  G = tex2D( sBackBufferY, vTexCoord     -g1 +2.0*g2);
        float4  H = tex2D(sSuper_xBR_P0, vTexCoord             +g2);
        float4  I = tex2D( sBackBufferY, vTexCoord     +g1        );

        float4 F4 = tex2D(sBackBufferY, vTexCoord     +g1 -2.0*g2);
        float4 I4 = tex2D(      sSuper_xBR_P0, vTexCoord +2.0*g1     -g2);
        float4 H5 = tex2D(sBackBufferY, vTexCoord     +g1 +2.0*g2);
        float4 I5 = tex2D(      sSuper_xBR_P0, vTexCoord +2.0*g1     +g2);

        float3 color = super_xbr(wp1, P0,  B,  C, P1,  D,  E,  F, F4,  G,  H,  I, I4, P2, H5, I5, P3);

        return float4(color, luma(color));
    }
}


float4 PS_Super_xBR_P2(float4 pos: SV_Position, float2 vTexCoord : TEXCOORD) : SV_Target
{
    float2 ps = 0.5*NormalizedNativePixelSize;

    float4 P0 = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2(-2.0, -2.0));
    float4  B = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2(-1.0, -2.0));
    float4  C = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2( 0.0, -2.0));
    float4 P1 = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2( 1.0, -2.0));

    float4  D = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2(-2.0, -1.0));
    float4  E = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2(-1.0, -1.0));
    float4  F = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2( 0.0, -1.0));
    float4 F4 = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2( 1.0, -1.0));

    float4  G = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2(-2.0,  0.0));
    float4  H = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2(-1.0,  0.0));
    float4  I = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2( 0.0,  0.0));
    float4 I4 = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2( 1.0,  0.0));

    float4 P2 = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2(-2.0,  1.0));
    float4 H5 = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2(-1.0,  1.0));
    float4 I5 = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2( 0.0,  1.0));
    float4 P3 = tex2D(sSuper_xBR_P1, vTexCoord.xy + ps*float2( 1.0,  1.0));

    float3 color = super_xbr(wp2, P0,  B,  C, P1,  D,  E,  F, F4,  G,  H,  I, I4, P2, H5, I5, P3);

    return float4(color, 1.0);
}


#define halfpi  1.5707963267948966192313216916398
#define pi    3.1415926535897932384626433832795
#define wa    (JINC2_WINDOW_SINC*pi)
#define wb    (JINC2_SINC*pi)



// Calculates the distance between two points
float dst(float2 pt1, float2 pt2)
{
    float2 v = pt2 - pt1;
    return sqrt(dot(v,v));
}



float4 resampler(float4 x)
{
    float4 res;

    res.x = (x.x==0.0) ?  (wa*wb)  :  sin(x.x*wa)*sin(x.x*wb)/(x.x*x.x);
    res.y = (x.y==0.0) ?  (wa*wb)  :  sin(x.y*wa)*sin(x.y*wb)/(x.y*x.y);
    res.z = (x.z==0.0) ?  (wa*wb)  :  sin(x.z*wa)*sin(x.z*wb)/(x.z*x.z);
    res.w = (x.w==0.0) ?  (wa*wb)  :  sin(x.w*wa)*sin(x.w*wb)/(x.w*x.w);
     
    return res;
}


float4 PS_Jinc2(float4 pos: SV_Position, float2 vTexCoord : TEXCOORD) : SV_Target
{
    float2 ps = 0.5*NormalizedNativePixelSize;

    float3 color;
    float4x4 weights;

    float2 dx = float2(1.0, 0.0);
    float2 dy = float2(0.0, 1.0);

    float2 pc = vTexCoord / ps;

    float2 tc = (floor(pc-float2(0.5,0.5))+float2(0.5,0.5));
     
    weights[0] = resampler(float4(dst(pc, tc    -dx    -dy), dst(pc, tc           -dy), dst(pc, tc    +dx    -dy), dst(pc, tc+2.0*dx    -dy)));
    weights[1] = resampler(float4(dst(pc, tc    -dx       ), dst(pc, tc              ), dst(pc, tc    +dx       ), dst(pc, tc+2.0*dx       )));
    weights[2] = resampler(float4(dst(pc, tc    -dx    +dy), dst(pc, tc           +dy), dst(pc, tc    +dx    +dy), dst(pc, tc+2.0*dx    +dy)));
    weights[3] = resampler(float4(dst(pc, tc    -dx+2.0*dy), dst(pc, tc       +2.0*dy), dst(pc, tc    +dx+2.0*dy), dst(pc, tc+2.0*dx+2.0*dy)));

    //weights[0][0] = weights[0][3] = weights[3][0] = weights[3][3] = 0.0;

    dx = dx * ps;
    dy = dy * ps;
    tc = tc * ps;
     
    // reading the texels
     
    float3 c00 = tex2D(sSuper_xBR_P2, tc    -dx    -dy).xyz;
    float3 c10 = tex2D(sSuper_xBR_P2, tc           -dy).xyz;
    float3 c20 = tex2D(sSuper_xBR_P2, tc    +dx    -dy).xyz;
    float3 c30 = tex2D(sSuper_xBR_P2, tc+2.0*dx    -dy).xyz;
    float3 c01 = tex2D(sSuper_xBR_P2, tc    -dx       ).xyz;
    float3 c11 = tex2D(sSuper_xBR_P2, tc              ).xyz;
    float3 c21 = tex2D(sSuper_xBR_P2, tc    +dx       ).xyz;
    float3 c31 = tex2D(sSuper_xBR_P2, tc+2.0*dx       ).xyz;
    float3 c02 = tex2D(sSuper_xBR_P2, tc    -dx    +dy).xyz;
    float3 c12 = tex2D(sSuper_xBR_P2, tc           +dy).xyz;
    float3 c22 = tex2D(sSuper_xBR_P2, tc    +dx    +dy).xyz;
    float3 c32 = tex2D(sSuper_xBR_P2, tc+2.0*dx    +dy).xyz;
    float3 c03 = tex2D(sSuper_xBR_P2, tc    -dx+2.0*dy).xyz;
    float3 c13 = tex2D(sSuper_xBR_P2, tc       +2.0*dy).xyz;
    float3 c23 = tex2D(sSuper_xBR_P2, tc    +dx+2.0*dy).xyz;
    float3 c33 = tex2D(sSuper_xBR_P2, tc+2.0*dx+2.0*dy).xyz;

    color = mul(weights[0], float4x3(c00, c10, c20, c30));
    color+= mul(weights[1], float4x3(c01, c11, c21, c31));
    color+= mul(weights[2], float4x3(c02, c12, c22, c32));
    color+= mul(weights[3], float4x3(c03, c13, c23, c33));
    color = color/(dot(mul(weights, float4(1.,1.,1.,1.)), float4(1.,1.,1.,1.)));

    // Anti-ringing
    //  Get min/max samples
    float3 min_sample = min4(c11, c21, c12, c22);
    float3 max_sample = max4(c11, c21, c12, c22);

    float3 aux = color;

    color = clamp(color, min_sample, max_sample);
    color = lerp(aux, color, JINC2_AR_STRENGTH);

    return float4(color, 1.0);
}


technique Super_xBR
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_BackBufferY;
        RenderTarget = tBackBufferY;
    }
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_Super_xBR_P0;
        RenderTarget = tSuper_xBR_P0;
    }
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_Super_xBR_P1;
        RenderTarget = tSuper_xBR_P1;
    }
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_Super_xBR_P2;
        RenderTarget = tSuper_xBR_P2;
    }
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_Jinc2;
    }
}
