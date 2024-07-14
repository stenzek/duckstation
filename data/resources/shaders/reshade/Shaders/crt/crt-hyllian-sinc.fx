#include "ReShade.fxh"

/*
    Hyllian's CRT-sinc Shader

    Copyright (C) 2011-2024 Hyllian

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



uniform int HFILTER_PROFILE <
    ui_type = "combo";
    ui_items = "Custom\0Composite\0Composite Soft\0";
    ui_label = "H-FILTER PROFILE";
> = 0;

uniform float SHP <
    ui_type = "drag";
    ui_min = 0.50;
    ui_max = 1.0;
    ui_step = 0.01;
    ui_label = "CUSTOM H-FILTER SHARPNESS";
> = 1.0;

uniform bool CRT_ANTI_RINGING <
    ui_type = "radio";
    ui_label = "ANTI RINGING";
> = true;

uniform bool SHARPNESS_HACK <
    ui_type = "radio";
    ui_label = "SHARPNESS HACK";
> = false;

uniform float CRT_InputGamma <
    ui_type = "drag";
    ui_min = 1.0;
    ui_max = 5.0;
    ui_step = 0.1;
    ui_label = "INPUT GAMMA";
> = 2.4;

uniform float CRT_OutputGamma <
    ui_type = "drag";
    ui_min = 1.0;
    ui_max = 5.0;
    ui_step = 0.05;
    ui_label = "OUTPUT GAMMA";
> = 2.2;

uniform int MASK_LAYOUT <
    ui_type = "combo";
    ui_items = "0-Off\0"
               "1-Aperture Classic\0""2-Aperture1 RGB 1080p\0""3-Aperture2 RGB 1080p\0""4-Aperture1 RGB 4k\0""5-Aperture2 RGB 4k\0""6-Aperture3 RGB 4k\0"
               "7-Shadow Classic\0""8-Shadow1 1080p\0""9-Shadow2 1080p\0""10-Shadow1 4k\0"
               "11-Slot1 1080p\0""12-Slot2 1080p\0""13-Slot1 4k\0""14-Slot1 4k\0""15-Slot1 8k\0";
    ui_category = "CRT Mask";
    ui_label = "MASK LAYOUT";
> = 1;

uniform int MONITOR_SUBPIXELS <
    ui_type = "combo";
    ui_items = "RGB\0BGR\0";
    ui_category = "CRT Mask";
    ui_label = "MONITOR SUBPIXELS LAYOUT";
> = 0;

uniform float BRIGHTBOOST <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 3.0;
    ui_step = 0.05;
    ui_label = "BRIGHTNESS BOOST";
> = 1.0;

uniform float BEAM_MIN_WIDTH <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.01;
    ui_label = "MIN BEAM WIDTH";
> = 0.86;

uniform float BEAM_MAX_WIDTH <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.01;
    ui_label = "MAX BEAM WIDTH";
> = 1.0;

uniform float SCANLINES_STRENGTH <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.01;
    ui_label = "SCANLINES STRENGTH";
> = 0.72;

uniform int SCANLINES_SHAPE <
    ui_type = "combo";
    ui_items = "Sinc\0Gaussian\0";
    ui_label = "SCANLINES SHAPE";
> = 1.0;

uniform float SCANLINES_CUTOFF <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1000.0;
    ui_step = 1.0;
    ui_label = "SCANLINES CUTOFF";
    ui_tooltip = "Max vertical native resolution above which scanlines are disabled.";
> = 390.0;

uniform bool SCANLINES_HIRES <
    ui_type = "radio";
    ui_label = "HIGH RESOLUTION SCANLINES";
> = false;

uniform float POST_BRIGHTNESS <
    ui_type = "drag";
    ui_min = 1.0;
    ui_max = 3.0;
    ui_step = 0.05;
    ui_label = "POST-BRIGHTNESS";
> = 1.00;

uniform bool VSCANLINES <
    ui_type = "radio";
    ui_label = "VERTICAL SCANLINES";
> = false;


uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;
uniform float  BufferWidth               < source = "bufferwidth"; >;
uniform float  BufferHeight              < source = "bufferheight"; >;
uniform float2 BufferToViewportRatio     < source = "buffer_to_viewport_ratio"; >;
uniform float2 ViewportSize              < source = "viewportsize"; >;
uniform float  ViewportWidth             < source = "viewportwidth"; >;
uniform float  ViewportHeight            < source = "viewportheight"; >;
uniform float  UpscaleMultiplier         < source = "upscale_multiplier"; >;


#include "../misc/include/mask.fxh"
#include "../misc/include/geom.fxh"


sampler2D sBackBuffer{Texture=ReShade::BackBufferTex;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=POINT;MinFilter=POINT;};

texture2D tBackBufferLinear{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA16f;};
sampler2D sBackBufferLinear{Texture=tBackBufferLinear;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=POINT;MinFilter=POINT;};

#define GAMMA_IN(color)    pow(color, float3(CRT_InputGamma, CRT_InputGamma, CRT_InputGamma))
#define GAMMA_OUT(color)   pow(color, float3(1.0 / CRT_OutputGamma, 1.0 / CRT_OutputGamma, 1.0 / CRT_OutputGamma))

#define SCANLINES_STRENGTH (-0.16*SCANLINES_SHAPE+SCANLINES_STRENGTH)
#define CORNER_SMOOTHNESS (80.0*pow(CORNER_SMOOTHNESS,10.0))

#define pi    3.1415926535897932384626433832795

#define RADIUS  2.0  // No need for more than 2-taps

float2 get_hfilter_profile()
{
    float2 hf_profile = float2(SHP, RADIUS);

    if      (HFILTER_PROFILE == 1) hf_profile = float2(0.78, 2.0); // SNES composite
    else if (HFILTER_PROFILE == 2) hf_profile = float2(0.65, 2.0); // Genesis composite

    return hf_profile;
}

/* Some window functions for tests. */
float4 sinc(float4 x)              { return sin(pi*x)*(1.0/(pi*x+0.001.xxxx)); }
float4 hann_window(float4 x)       { return 0.5 * ( 1.0 - cos( 0.5 * pi * ( x + 2.0 ) ) ); }
float4 blackman_window(float4 x)   { return 0.42 - 0.5*cos(0.5*pi*(x+2.0)) + 0.08*cos(pi*(x+2.0)); }
float4 lanczos(float4 x, float a)  { return sinc(x) * sinc(x / a); }
float4 blackman(float4 x, float a) { return sinc(x) * blackman_window(x); }
float4 hann(float4 x, float a)     { return sinc(x) * hann_window(x); }

float4 resampler4(float4 x, float2 hfp)
{
    return blackman(x * hfp.x, hfp.y);
}


#define wa    (0.5*pi)
#define wb    (pi)

float3 resampler3(float3 x)
{
    float3 res;

    res.x = (x.x<=0.001) ?  1.0  :  sin(x.x*wa)*sin(x.x*wb)/(wa*wb*x.x*x.x);
    res.y = (x.y<=0.001) ?  1.0  :  sin(x.y*wa)*sin(x.y*wb)/(wa*wb*x.y*x.y);
    res.z = (x.z<=0.001) ?  1.0  :  sin(x.z*wa)*sin(x.z*wb)/(wa*wb*x.z*x.z);

    return res;
}

float3 get_scanlines(float3 d0, float3 d1, float3 color0, float3 color1)
{
    if (SCANLINES_SHAPE > 0.5) {
        d0 = exp(-16.0*d0*d0);
        d1 = exp(-16.0*d1*d1);
    }
    else {
        d0 = clamp(2.0*d0, 0.0, 1.0);
        d1 = clamp(2.0*d1, 0.0, 1.0);
        d0 = resampler3(d0);
        d1 = resampler3(d1);
    }

    return (BRIGHTBOOST*(color0*d0+color1*d1));
}

float4 PS_BackBufferLinear(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD) : SV_Target
{
//    float2 tc = (floor(vTexCoord / NormalizedNativePixelSize) + 0.5.xx) * NormalizedNativePixelSize;

    return float4(GAMMA_IN(tex2D(sBackBuffer, vTexCoord).rgb), 1.0);
}

struct ST_VertexOut
{
    float2 sinangle    : TEXCOORD1;
    float2 cosangle    : TEXCOORD2;
    float3 stretch     : TEXCOORD3;
    float2 TextureSize : TEXCOORD4;
};


// Vertex shader generating a triangle covering the entire screen
void VS_CRT_Geom(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out ST_VertexOut vVARS)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    // Screen centering
    texcoord = texcoord - float2(centerx,centery)/100.0;

    float2 SourceSize = 1.0/NormalizedNativePixelSize;
    float shp_hack = 1.0 + float(SHARPNESS_HACK);

 
    // Precalculate a bunch of useful values we'll need in the fragment
    // shader.
    vVARS.sinangle    = sin(float2(geom_x_tilt, geom_y_tilt));
    vVARS.cosangle    = cos(float2(geom_x_tilt, geom_y_tilt));
    vVARS.stretch     = maxscale(vVARS.sinangle, vVARS.cosangle);
    vVARS.TextureSize = lerp(float2(shp_hack*SourceSize.x, SourceSize.y), float2(SourceSize.x, shp_hack*SourceSize.y), VSCANLINES);
}


float4 PS_CRT_Hyllian(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD0, in ST_VertexOut vVARS) : SV_Target
{
    float2 OutputSize = float2(BufferWidth, BufferHeight);

    float2 TextureSize = vVARS.TextureSize;

    float2 dx = lerp(float2(1.0/TextureSize.x, 0.0), float2(0.0, 1.0/TextureSize.y), VSCANLINES);
    float2 dy = lerp(float2(0.0, 1.0/TextureSize.y), float2(1.0/TextureSize.x, 0.0), VSCANLINES);

    // Texture coordinates of the texel containing the active pixel.
    float2 WarpedTexCoord = (geom_curvature == true) ? transform(vTexCoord, vVARS.sinangle, vVARS.cosangle, vVARS.stretch) : vTexCoord;

    float cval = corner((WarpedTexCoord-0.5.xx) * BufferToViewportRatio + 0.5.xx);

    float2 pix_coord = WarpedTexCoord*TextureSize - 0.5.xx;
  
    float2 tc = ( (SCANLINES_HIRES == true) ? (lerp(float2(floor(pix_coord.x), pix_coord.y), float2(pix_coord.x, floor(pix_coord.y)), VSCANLINES) + float2(0.5, 0.5)) : (floor(pix_coord) + float2(0.5, 0.5)) )/TextureSize;

    float2 fp = lerp(frac(pix_coord), frac(pix_coord.yx), VSCANLINES);

    float3 c00 = tex2D(sBackBufferLinear, tc     - dx).xyz;
    float3 c01 = tex2D(sBackBufferLinear, tc         ).xyz;
    float3 c02 = tex2D(sBackBufferLinear, tc     + dx).xyz;
    float3 c03 = tex2D(sBackBufferLinear, tc + 2.0*dx).xyz;

    float3 c10, c11, c12, c13;

    if (SCANLINES_HIRES == false)
    {
        c10 = tex2D(sBackBufferLinear, tc     - dx + dy).xyz;
        c11 = tex2D(sBackBufferLinear, tc          + dy).xyz;
        c12 = tex2D(sBackBufferLinear, tc     + dx + dy).xyz;
        c13 = tex2D(sBackBufferLinear, tc + 2.0*dx + dy).xyz;
    }
    else { c10 = c00; c11 = c01; c12 = c02; c13 = c03;}

    float4x3 color_matrix0 = float4x3(c00, c01, c02, c03);
    float4x3 color_matrix1 = float4x3(c10, c11, c12, c13);

    float2 hfp = get_hfilter_profile();

    float4 weights = resampler4(float4(1.0+fp.x, fp.x, 1.0-fp.x, 2.0-fp.x), hfp);

    float3 color0   = mul(weights, color_matrix0)/dot(weights, 1.0.xxxx);
    float3 color1   = mul(weights, color_matrix1)/dot(weights, 1.0.xxxx);

    // Get min/max samples
    float3 min_sample0 = min(c01,c02);
    float3 max_sample0 = max(c01,c02);
    float3 min_sample1 = min(c11,c12);
    float3 max_sample1 = max(c11,c12);
  
    // Anti-ringing
    float3 aux = color0;
    color0 = clamp(color0, min_sample0, max_sample0);
    color0 = lerp(aux, color0, CRT_ANTI_RINGING);
    aux = color1;
    color1 = clamp(color1, min_sample1, max_sample1);
    color1 = lerp(aux, color1, CRT_ANTI_RINGING);

    float pos0 = fp.y;
    float pos1 = 1 - fp.y;

    float3 lum0 = lerp(BEAM_MIN_WIDTH.xxx, BEAM_MAX_WIDTH.xxx, color0);
    float3 lum1 = lerp(BEAM_MIN_WIDTH.xxx, BEAM_MAX_WIDTH.xxx, color1);

    float3 d0 = SCANLINES_STRENGTH*pos0/(lum0*lum0+0.0000001.xxx);
    float3 d1 = SCANLINES_STRENGTH*pos1/(lum1*lum1+0.0000001.xxx);

    float3 color  = (vVARS.TextureSize.y <= SCANLINES_CUTOFF) ? get_scanlines(d0, d1, color0, color1) : tex2D(sBackBufferLinear, WarpedTexCoord.xy).xyz;

    color *=  BRIGHTBOOST;

    color  = GAMMA_OUT(color);

    float2 mask_coords =vTexCoord.xy * OutputSize.xy;

    mask_coords = lerp(mask_coords.xy, mask_coords.yx, VSCANLINES);

    color.rgb*=GAMMA_OUT(mask_weights(mask_coords, MASK_LAYOUT, MONITOR_SUBPIXELS, MASK_DARK_STRENGTH, MASK_LIGHT_STRENGTH));
    
    float4 res = float4(POST_BRIGHTNESS*color, 1.0);

    res.rgb = res.rgb * cval.xxx;

    return float4(res.rgb, 1.0);
}

technique CRT_Hyllian
{
   pass
   {
       VertexShader = PostProcessVS;
       PixelShader  = PS_BackBufferLinear;
       RenderTarget = tBackBufferLinear;
   }
   pass
   {
       VertexShader = VS_CRT_Geom;
       PixelShader  = PS_CRT_Hyllian;
   }
}
