#include "ReShade.fxh"

/*
    Geom Shader - a modified CRT-Geom without CRT features made to be appended/integrated
    into any other shaders and provide curvature/warping/oversampling features.

    Adapted by Hyllian (2024).
*/


/*
    CRT-interlaced

    Copyright (C) 2010-2012 cgwg, Themaister and DOLLS

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.

    (cgwg gave their consent to have the original version of this shader
    distributed under the GPL in this message:

    http://board.byuu.org/viewtopic.php?p=26075#p26075

    "Feel free to distribute my shaders under the GPL. After all, the
    barrel distortion code was taken from the Curvature shader, which is
    under the GPL."
    )
    This shader variant is pre-configured with screen curvature
*/



uniform float geom_curvature <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 1.0;
	ui_label = "Geom Curvature Toggle";
> = 1.0;

uniform float geom_R <
	ui_type = "drag";
	ui_min = 0.1;
	ui_max = 10.0;
	ui_step = 0.1;
	ui_label = "Geom Curvature Radius";
> = 2.0;

uniform float geom_d <
	ui_type = "drag";
	ui_min = 0.1;
	ui_max = 3.0;
	ui_step = 0.1;
	ui_label = "Geom Distance";
> = 1.5;

uniform float geom_invert_aspect <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 1.0;
	ui_label = "Geom Curvature Aspect Inversion";
> = 0.0;

uniform float geom_cornersize <
	ui_type = "drag";
	ui_min = 0.001;
	ui_max = 1.0;
	ui_step = 0.005;
	ui_label = "Geom Corner Size";
> = 0.03;

uniform float geom_cornersmooth <
	ui_type = "drag";
	ui_min = 80.0;
	ui_max = 2000.0;
	ui_step = 100.0;
	ui_label = "Geom Corner Smoothness";
> = 1000.0;

uniform float geom_x_tilt <
	ui_type = "drag";
	ui_min = -0.5;
	ui_max = 0.5;
	ui_step = 0.05;
	ui_label = "Geom Horizontal Tilt";
> = 0.0;

uniform float geom_y_tilt <
	ui_type = "drag";
	ui_min = -0.5;
	ui_max = 0.5;
	ui_step = 0.05;
	ui_label = "Geom Vertical Tilt";
> = 0.0;

uniform float geom_overscan_x <
	ui_type = "drag";
	ui_min = -125.0;
	ui_max = 125.0;
	ui_step = 0.5;
	ui_label = "Geom Horiz. Overscan %";
> = 100.0;

uniform float geom_overscan_y <
	ui_type = "drag";
	ui_min = -125.0;
	ui_max = 125.0;
	ui_step = 0.5;
	ui_label = "Geom Vert. Overscan %";
> = 100.0;

uniform float geom_lum <
	ui_type = "drag";
	ui_min = 0.5;
	ui_max = 2.0;
	ui_step = 0.01;
	ui_label = "Geom Luminance";
> = 1.0;

uniform float geom_target_gamma <
	ui_type = "drag";
	ui_min = 0.1;
	ui_max = 5.0;
	ui_step = 0.1;
	ui_label = "Geom Target Gamma";
> = 2.4;

uniform float geom_monitor_gamma <
	ui_type = "drag";
	ui_min = 0.1;
	ui_max = 5.0;
	ui_step = 0.1;
	ui_label = "Geom Monitor Gamma";
> = 2.2;


uniform float2 BufferViewportRatio < source = "buffer_to_viewport_ratio"; >;
uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;
uniform float2 ViewportSize < source = "viewportsize"; >;

sampler2D sBackBuffer{Texture=ReShade::BackBufferTex;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=LINEAR;MinFilter=LINEAR;};

// Comment the next line to disable interpolation in linear gamma (and
// gain speed).
#define LINEAR_PROCESSING

// Enable 3x oversampling of the beam profile; improves moire effect caused by scanlines+curvature
#define OVERSAMPLE

// Use the older, purely gaussian beam profile; uncomment for speed
//#define USEGAUSSIAN

// Macros.
#define FIX(c) max(abs(c), 1e-5);
#define PI 3.141592653589

#ifdef LINEAR_PROCESSING
#       define TEX2D(c) pow(tex2D(sBackBuffer, (c)), float4(geom_target_gamma,geom_target_gamma,geom_target_gamma,geom_target_gamma))
#else
#       define TEX2D(c) tex2D(sBackBuffer, (c))
#endif

// aspect ratio
#define aspect     (geom_invert_aspect>0.5?float2(0.75,1.0):float2(1.0,0.75))
#define overscan   (float2(1.01,1.01));


struct ST_VertexOut
{
    float2 sinangle    : TEXCOORD1;
    float2 cosangle    : TEXCOORD2;
    float3 stretch     : TEXCOORD3;
    float2 TextureSize : TEXCOORD4;
};


float vs_intersect(float2 xy, float2 sinangle, float2 cosangle)
{
    float A = dot(xy,xy) + geom_d*geom_d;
    float B = 2.0*(geom_R*(dot(xy,sinangle)-geom_d*cosangle.x*cosangle.y)-geom_d*geom_d);
    float C = geom_d*geom_d + 2.0*geom_R*geom_d*cosangle.x*cosangle.y;
    
    return (-B-sqrt(B*B-4.0*A*C))/(2.0*A);
}

float2 vs_bkwtrans(float2 xy, float2 sinangle, float2 cosangle)
{
    float c     = vs_intersect(xy, sinangle, cosangle);
    float2 point  = (float2(c, c)*xy - float2(-geom_R, -geom_R)*sinangle) / float2(geom_R, geom_R);
    float2 poc    = point/cosangle;
    
    float2 tang   = sinangle/cosangle;
    float A     = dot(tang, tang) + 1.0;
    float B     = -2.0*dot(poc, tang);
    float C     = dot(poc, poc) - 1.0;
    
    float a     = (-B + sqrt(B*B - 4.0*A*C))/(2.0*A);
    float2 uv     = (point - a*sinangle)/cosangle;
    float r     = FIX(geom_R*acos(a));
    
    return uv*r/sin(r/geom_R);
}

float2 vs_fwtrans(float2 uv, float2 sinangle, float2 cosangle)
{
    float r = FIX(sqrt(dot(uv,uv)));
    uv *= sin(r/geom_R)/r;
    float x = 1.0-cos(r/geom_R);
    float D = geom_d/geom_R + x*cosangle.x*cosangle.y+dot(uv,sinangle);
    
    return geom_d*(uv*cosangle-x*sinangle)/D;
}

float3 vs_maxscale(float2 sinangle, float2 cosangle)
{
    float2 c  = vs_bkwtrans(-geom_R * sinangle / (1.0 + geom_R/geom_d*cosangle.x*cosangle.y), sinangle, cosangle);
    float2 a  = float2(0.5,0.5)*aspect;
    
    float2 lo = float2(vs_fwtrans(float2(-a.x,  c.y), sinangle, cosangle).x,
                   vs_fwtrans(float2( c.x, -a.y), sinangle, cosangle).y)/aspect;

    float2 hi = float2(vs_fwtrans(float2(+a.x,  c.y), sinangle, cosangle).x,
                   vs_fwtrans(float2( c.x, +a.y), sinangle, cosangle).y)/aspect;
    
    return float3((hi+lo)*aspect*0.5,max(hi.x-lo.x,hi.y-lo.y));
}



// Vertex shader generating a triangle covering the entire screen
void VS_CRT_Geom(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out ST_VertexOut vVARS)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

 //   float2 SourceSize = 1.0/NormalizedNativePixelSize;
    float2 SourceSize = ViewportSize*BufferViewportRatio;

    // Precalculate a bunch of useful values we'll need in the fragment
    // shader.
    vVARS.sinangle    = sin(float2(geom_x_tilt, geom_y_tilt));
    vVARS.cosangle    = cos(float2(geom_x_tilt, geom_y_tilt));
    vVARS.stretch     = vs_maxscale(vVARS.sinangle, vVARS.cosangle);
    vVARS.TextureSize = float2(SourceSize.x, SourceSize.y);
}



float intersect(float2 xy, float2 sinangle, float2 cosangle)
{
    float A = dot(xy,xy) + geom_d*geom_d;
    float B, C;

       B = 2.0*(geom_R*(dot(xy,sinangle) - geom_d*cosangle.x*cosangle.y) - geom_d*geom_d);
       C = geom_d*geom_d + 2.0*geom_R*geom_d*cosangle.x*cosangle.y;

    return (-B-sqrt(B*B - 4.0*A*C))/(2.0*A);
}

float2 bkwtrans(float2 xy, float2 sinangle, float2 cosangle)
{
    float c     = intersect(xy, sinangle, cosangle);
    float2 point  = (float2(c, c)*xy - float2(-geom_R, -geom_R)*sinangle) / float2(geom_R, geom_R);
    float2 poc    = point/cosangle;
    float2 tang   = sinangle/cosangle;

    float A     = dot(tang, tang) + 1.0;
    float B     = -2.0*dot(poc, tang);
    float C     = dot(poc, poc) - 1.0;

    float a     = (-B + sqrt(B*B - 4.0*A*C)) / (2.0*A);
    float2 uv     = (point - a*sinangle) / cosangle;
    float r     = FIX(geom_R*acos(a));
    
    return uv*r/sin(r/geom_R);
}

float2 fwtrans(float2 uv, float2 sinangle, float2 cosangle)
{
    float r = FIX(sqrt(dot(uv, uv)));
    uv *= sin(r/geom_R)/r;
    float x = 1.0 - cos(r/geom_R);
    float D;
    
      D = geom_d/geom_R + x*cosangle.x*cosangle.y + dot(uv,sinangle);

    return geom_d*(uv*cosangle - x*sinangle)/D;
}

float3 maxscale(float2 sinangle, float2 cosangle)
{
       float2 c = bkwtrans(-geom_R * sinangle / (1.0 + geom_R/geom_d*cosangle.x*cosangle.y), sinangle, cosangle);
       float2 a = float2(0.5, 0.5)*aspect;

       float2 lo = float2(fwtrans(float2(-a.x,  c.y), sinangle, cosangle).x,
                      fwtrans(float2( c.x, -a.y), sinangle, cosangle).y)/aspect;
       float2 hi = float2(fwtrans(float2(+a.x,  c.y), sinangle, cosangle).x,
                      fwtrans(float2( c.x, +a.y), sinangle, cosangle).y)/aspect;

       return float3((hi+lo)*aspect*0.5,max(hi.x-lo.x, hi.y-lo.y));
}

float2 transform(float2 coord, float2 sinangle, float2 cosangle, float3 stretch)
{
    coord = (coord - float2(0.5, 0.5))*aspect*stretch.z + stretch.xy;
    
    return (bkwtrans(coord, sinangle, cosangle) /
        float2(geom_overscan_x / 100.0, geom_overscan_y / 100.0)/aspect + float2(0.5, 0.5));
}

float corner(float2 coord)
{
    coord = (coord - float2(0.5, 0.5)) * float2(geom_overscan_x / 100.0, geom_overscan_y / 100.0) + float2(0.5, 0.5);
    coord = min(coord, float2(1.0, 1.0) - coord) * aspect;
    float2 cdist = float2(geom_cornersize, geom_cornersize);
    coord = (cdist - min(coord, cdist));
    float dist = sqrt(dot(coord, coord));
    
      return clamp((cdist.x - dist)*geom_cornersmooth, 0.0, 1.0);
}

float fwidth(float value){
  return abs(ddx(value)) + abs(ddy(value));
}


float4 PS_CRT_Geom(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in ST_VertexOut vVARS) : SV_Target
{
    // Texture coordinates of the texel containing the active pixel.
    float2 xy;

    if (geom_curvature > 0.5)
      xy = transform(vTexCoord, vVARS.sinangle, vVARS.cosangle, vVARS.stretch);
    else
      xy = vTexCoord;

    float cval = corner(xy);

    float2 uv_ratio = frac((xy * vVARS.TextureSize - float2(0.5, 0.5)) / vVARS.TextureSize);

    float4 col = TEX2D(xy);

#ifndef LINEAR_PROCESSING
    col  = pow(col , float4(geom_target_gamma, geom_target_gamma, geom_target_gamma, geom_target_gamma));
#endif

    col.rgb *= (geom_lum * step(0.0, uv_ratio.y));

    float3 mul_res = col.rgb * float3(cval, cval, cval);

    // Convert the image gamma for display on our output device.
    mul_res = pow(mul_res, float3(1.0 / geom_monitor_gamma, 1.0 / geom_monitor_gamma, 1.0 / geom_monitor_gamma));

    return float4(mul_res, 1.0);
}


technique CRT_Geom
{
    pass
    {
        VertexShader = VS_CRT_Geom;
        PixelShader  = PS_CRT_Geom;
    }
}
