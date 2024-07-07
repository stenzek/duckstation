#include "ReShade.fxh"

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


uniform float CRTgamma <
    ui_type = "drag";
    ui_min = 0.1;
    ui_max = 5.0;
    ui_step = 0.1;
    ui_label = "CRTGeom Target Gamma";
> = 2.4;

uniform float monitorgamma <
    ui_type = "drag";
    ui_min = 0.1;
    ui_max = 5.0;
    ui_step = 0.1;
    ui_label = "CRTGeom Monitor Gamma";
> = 2.2;

uniform float d <
    ui_type = "drag";
    ui_category = "Curvature";
    ui_min = 0.1;
    ui_max = 3.0;
    ui_step = 0.1;
    ui_label = "CRTGeom Distance";
> = 1.5;

uniform bool CURVATURE <
    ui_category = "Curvature";
    ui_type = "radio";
    ui_label = "CRTGeom Curvature Toggle";
> = true;

uniform bool invert_aspect <
    ui_type = "radio";
    ui_category = "Curvature";
    ui_label = "CRTGeom Curvature Aspect Inversion";
> = false;

uniform float R <
    ui_type = "drag";
    ui_category = "Curvature";
    ui_min = 0.1;
    ui_max = 10.0;
    ui_step = 0.1;
    ui_label = "CRTGeom Curvature Radius";
> = 2.0;

uniform float cornersize <
    ui_type = "drag";
    ui_category = "Curvature";
    ui_min = 0.001;
    ui_max = 1.0;
    ui_step = 0.005;
    ui_label = "CRTGeom Corner Size";
> = 0.03;

uniform float cornersmooth <
    ui_type = "drag";
    ui_category = "Curvature";
    ui_min = 80.0;
    ui_max = 2000.0;
    ui_step = 100.0;
    ui_label = "CRTGeom Corner Smoothness";
> = 1000.0;

uniform float x_tilt <
    ui_type = "drag";
    ui_category = "Curvature";
    ui_min = -1.0;
    ui_max = 1.0;
    ui_step = 0.05;
    ui_label = "CRTGeom Horizontal Tilt";
> = 0.0;

uniform float y_tilt <
    ui_type = "drag";
    ui_category = "Curvature";
    ui_min = -1.0;
    ui_max = 1.0;
    ui_step = 0.05;
    ui_label = "CRTGeom Vertical Tilt";
> = 0.0;

uniform float overscan_x <
    ui_type = "drag";
    ui_min = -125.0;
    ui_max = 125.0;
    ui_step = 0.5;
    ui_label = "CRTGeom Horiz. Overscan %";
> = 100.0;

uniform float overscan_y <
    ui_type = "drag";
    ui_min = -125.0;
    ui_max = 125.0;
    ui_step = 0.5;
    ui_label = "CRTGeom Vert. Overscan %";
> = 100.0;

uniform float centerx <
    ui_type = "drag";
    ui_min = -100.0;
    ui_max = 100.0;
    ui_step = 0.1;
    ui_label = "Image Center X";
> = 0.00;

uniform float centery <
    ui_type = "drag";
    ui_min = -100.0;
    ui_max = 100.0;
    ui_step = 0.1;
    ui_label = "Image Center Y";
> = 0.00;

uniform float DOTMASK <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.05;
    ui_label = "CRTGeom Dot Mask Strength";
> = 0.3;

uniform float SHARPER <
    ui_type = "drag";
    ui_min = 1.0;
    ui_max = 3.0;
    ui_step = 1.0;
    ui_label = "CRTGeom Sharpness";
> = 1.0;

uniform float scanline_weight <
    ui_type = "drag";
    ui_min = 0.1;
    ui_max = 0.5;
    ui_step = 0.05;
    ui_label = "CRTGeom Scanline Weight";
> = 0.3;

uniform bool vertical_scanlines <
    ui_type = "radio";
    ui_label = "CRTGeom Vertical Scanlines";
> = false;

uniform float lum <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.01;
    ui_label = "CRTGeom Luminance";
> = 0.0;

uniform float interlace_detect <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 1.0;
    ui_label = "CRTGeom Interlacing Simulation";
> = 1.0;



uniform float  FrameCount < source = "framecount"; >;
uniform float2 BufferToViewportRatio < source = "buffer_to_viewport_ratio"; >;
uniform float2 InternalPixelSize < source = "internal_pixel_size"; >;
uniform float2 NativePixelSize < source = "native_pixel_size"; >;
uniform float2 NormalizedInternalPixelSize < source = "normalized_internal_pixel_size"; >;
uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;
uniform float  UpscaleMultiplier < source = "upscale_multiplier"; >;
uniform float2 ViewportSize < source = "viewportsize"; >;
uniform float  ViewportWidth < source = "viewportwidth"; >;
uniform float  ViewportHeight < source = "viewportheight"; >;

sampler2D sBackBuffer{Texture=ReShade::BackBufferTex;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=POINT;MinFilter=POINT;};

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
#       define TEX2D(c) pow(tex2D(sBackBuffer, (c)), float4(CRTgamma,CRTgamma,CRTgamma,CRTgamma))
#else
#       define TEX2D(c) tex2D(sBackBuffer, (c))
#endif

// aspect ratio
#define aspect     (invert_aspect==true?float2(ViewportHeight/ViewportWidth,1.0):float2(1.0,ViewportHeight/ViewportWidth))
#define overscan   (float2(1.01,1.01));


struct ST_VertexOut
{
    float2 sinangle    : TEXCOORD1;
    float2 cosangle    : TEXCOORD2;
    float3 stretch     : TEXCOORD3;
    float2 ilfac       : TEXCOORD4;
    float2 one         : TEXCOORD5;
    float  mod_factor  : TEXCOORD6;
    float2 TextureSize : TEXCOORD7;
};


float vs_intersect(float2 xy, float2 sinangle, float2 cosangle)
{
    float A = dot(xy,xy) + d*d;
    float B = 2.0*(R*(dot(xy,sinangle)-d*cosangle.x*cosangle.y)-d*d);
    float C = d*d + 2.0*R*d*cosangle.x*cosangle.y;
    
    return (-B-sqrt(B*B-4.0*A*C))/(2.0*A);
}

float2 vs_bkwtrans(float2 xy, float2 sinangle, float2 cosangle)
{
    float c     = vs_intersect(xy, sinangle, cosangle);
    float2 point  = (float2(c, c)*xy - float2(-R, -R)*sinangle) / float2(R, R);
    float2 poc    = point/cosangle;
    
    float2 tang   = sinangle/cosangle;
    float A     = dot(tang, tang) + 1.0;
    float B     = -2.0*dot(poc, tang);
    float C     = dot(poc, poc) - 1.0;
    
    float a     = (-B + sqrt(B*B - 4.0*A*C))/(2.0*A);
    float2 uv     = (point - a*sinangle)/cosangle;
    float r     = FIX(R*acos(a));
    
    return uv*r/sin(r/R);
}

float2 vs_fwtrans(float2 uv, float2 sinangle, float2 cosangle)
{
    float r = FIX(sqrt(dot(uv,uv)));
    uv *= sin(r/R)/r;
    float x = 1.0-cos(r/R);
    float D = d/R + x*cosangle.x*cosangle.y+dot(uv,sinangle);
    
    return d*(uv*cosangle-x*sinangle)/D;
}

float3 vs_maxscale(float2 sinangle, float2 cosangle)
{
    float2 c  = vs_bkwtrans(-R * sinangle / (1.0 + R/d*cosangle.x*cosangle.y), sinangle, cosangle);
    float2 a  = float2(0.5,0.5)*aspect;
    
    float2 lo = float2(vs_fwtrans(float2(-a.x,  c.y), sinangle, cosangle).x,
                   vs_fwtrans(float2( c.x, -a.y), sinangle, cosangle).y)/aspect;

    float2 hi = float2(vs_fwtrans(float2(+a.x,  c.y), sinangle, cosangle).x,
                   vs_fwtrans(float2( c.x, +a.y), sinangle, cosangle).y)/aspect;
    
    return float3((hi+lo)*aspect*0.5,max(hi.x-lo.x,hi.y-lo.y));
}

// Code snippet borrowed from crt-cyclon. (credits to DariusG)
float2 Warp(float2 pos)
{
    pos = pos*2.0 - 1.0;
    pos *= float2(1.0 + pos.y*pos.y*0, 1.0 + pos.x*pos.x*0);
    pos = pos*0.5 + 0.5;

    return pos;
}


// Vertex shader generating a triangle covering the entire screen
void VS_CRT_Geom(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out ST_VertexOut vVARS)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    // center screen
    texcoord = Warp(texcoord - float2(centerx,centery)/100.0);

    float2 SourceSize = 1.0/NormalizedNativePixelSize;
    float2 OutputSize = ViewportSize*BufferToViewportRatio;

    // Precalculate a bunch of useful values we'll need in the fragment
    // shader.
    vVARS.sinangle    = sin(float2(x_tilt, y_tilt));
    vVARS.cosangle    = cos(float2(x_tilt, y_tilt));
    vVARS.stretch     = vs_maxscale(vVARS.sinangle, vVARS.cosangle);
    
    if(vertical_scanlines == false)
    {
       vVARS.TextureSize = float2(SHARPER * SourceSize.x, SourceSize.y);
       
       vVARS.ilfac = float2(1.0, clamp(floor(SourceSize.y/(interlace_detect > 0.5 ? 200.0 : 1000)),  1.0, 2.0));

       // The size of one texel, in texture-coordinates.
       vVARS.one = vVARS.ilfac / vVARS.TextureSize;

       // Resulting X pixel-coordinate of the pixel we're drawing.
       vVARS.mod_factor = texcoord.x * SourceSize.x * OutputSize.x / SourceSize.x;
    }else{
       vVARS.TextureSize = float2(SourceSize.x, SHARPER * SourceSize.y);

       vVARS.ilfac = float2(clamp(floor(SourceSize.x/(interlace_detect > 0.5 ? 200.0 : 1000)),  1.0, 2.0), 1.0);

       // The size of one texel, in texture-coordinates.
       vVARS.one = vVARS.ilfac / vVARS.TextureSize;

       // Resulting X pixel-coordinate of the pixel we're drawing.
       vVARS.mod_factor = texcoord.y * SourceSize.y * OutputSize.y / SourceSize.y;
    }
}



float intersect(float2 xy, float2 sinangle, float2 cosangle)
{
    float A = dot(xy,xy) + d*d;
    float B, C;

    if(vertical_scanlines == false)
    {
       B = 2.0*(R*(dot(xy,sinangle) - d*cosangle.x*cosangle.y) - d*d);
       C = d*d + 2.0*R*d*cosangle.x*cosangle.y;
    }else{
       B = 2.0*(R*(dot(xy,sinangle) - d*cosangle.y*cosangle.x) - d*d);
       C = d*d + 2.0*R*d*cosangle.y*cosangle.x;
    }

    return (-B-sqrt(B*B - 4.0*A*C))/(2.0*A);
}

float2 bkwtrans(float2 xy, float2 sinangle, float2 cosangle)
{
    float c     = intersect(xy, sinangle, cosangle);
    float2 point  = (float2(c, c)*xy - float2(-R, -R)*sinangle) / float2(R, R);
    float2 poc    = point/cosangle;
    float2 tang   = sinangle/cosangle;

    float A     = dot(tang, tang) + 1.0;
    float B     = -2.0*dot(poc, tang);
    float C     = dot(poc, poc) - 1.0;

    float a     = (-B + sqrt(B*B - 4.0*A*C)) / (2.0*A);
    float2 uv     = (point - a*sinangle) / cosangle;
    float r     = FIX(R*acos(a));
    
    return uv*r/sin(r/R);
}

float2 fwtrans(float2 uv, float2 sinangle, float2 cosangle)
{
    float r = FIX(sqrt(dot(uv, uv)));
    uv *= sin(r/R)/r;
    float x = 1.0 - cos(r/R);
    float D;
    
    if(vertical_scanlines == false)
      D = d/R + x*cosangle.x*cosangle.y + dot(uv,sinangle);
    else
      D = d/R + x*cosangle.y*cosangle.x + dot(uv,sinangle);

    return d*(uv*cosangle - x*sinangle)/D;
}

float3 maxscale(float2 sinangle, float2 cosangle)
{
   if(vertical_scanlines == false)
   {
       float2 c = bkwtrans(-R * sinangle / (1.0 + R/d*cosangle.x*cosangle.y), sinangle, cosangle);
       float2 a = float2(0.5, 0.5)*aspect;

       float2 lo = float2(fwtrans(float2(-a.x,  c.y), sinangle, cosangle).x,
                      fwtrans(float2( c.x, -a.y), sinangle, cosangle).y)/aspect;
       float2 hi = float2(fwtrans(float2(+a.x,  c.y), sinangle, cosangle).x,
                      fwtrans(float2( c.x, +a.y), sinangle, cosangle).y)/aspect;

       return float3((hi+lo)*aspect*0.5,max(hi.x-lo.x, hi.y-lo.y));
   }else{
       float2 c = bkwtrans(-R * sinangle / (1.0 + R/d*cosangle.y*cosangle.x), sinangle, cosangle);
       float2 a = float2(0.5, 0.5)*aspect;

       float2 lo = float2(fwtrans(float2(-a.y,  c.x), sinangle, cosangle).y,
                      fwtrans(float2( c.y, -a.x), sinangle, cosangle).x)/aspect;
       float2 hi = float2(fwtrans(float2(+a.y,  c.x), sinangle, cosangle).y,
                      fwtrans(float2( c.y, +a.x), sinangle, cosangle).x)/aspect;

       return float3((hi+lo)*aspect*0.5,max(hi.y-lo.y, hi.x-lo.x));
   }
}

// Calculate the influence of a scanline on the current pixel.
//
// 'distance' is the distance in texture coordinates from the current
// pixel to the scanline in question.
// 'color' is the colour of the scanline at the horizontal location of
// the current pixel.
float4 scanlineWeights(float distance, float4 color)
{
    // "wid" controls the width of the scanline beam, for each RGB
    // channel The "weights" lines basically specify the formula
    // that gives you the profile of the beam, i.e. the intensity as
    // a function of distance from the vertical center of the
    // scanline. In this case, it is gaussian if width=2, and
    // becomes nongaussian for larger widths. Ideally this should
    // be normalized so that the integral across the beam is
    // independent of its width. That is, for a narrower beam
    // "weights" should have a higher peak at the center of the
    // scanline than for a wider beam.
    #ifdef USEGAUSSIAN
        float4 wid = 0.3 + 0.1 * pow(color, float4(3.0, 3.0, 3.0, 3.0));
        float dsw  = distance / scanline_weight;
        float4 weights = float4(dsw, dsw, dsw, dsw);
        
        return (lum + 0.4) * exp(-weights * weights) / wid;
    #else
        float4 wid = 2.0 + 2.0 * pow(color, float4(4.0, 4.0, 4.0, 4.0));
        float dsw  = distance / scanline_weight;
        float4 weights = float4(dsw, dsw, dsw, dsw);
        
        return (lum + 1.4) * exp(-pow(weights * rsqrt(0.5 * wid), wid)) / (0.6 + 0.2 * wid);
    #endif
}

float2 transform(float2 coord, float2 sinangle, float2 cosangle, float3 stretch)
{
    coord = (coord - float2(0.5, 0.5))*aspect*stretch.z + stretch.xy;
    
    return (bkwtrans(coord, sinangle, cosangle) /
        float2(overscan_x / 100.0, overscan_y / 100.0)/aspect + float2(0.5, 0.5));
}

float corner(float2 coord)
{
    coord = min(coord, float2(1.0, 1.0) - coord) * aspect;
    float2 cdist = float2(cornersize, cornersize);
    coord = (cdist - min(coord, cdist));
    float dist = sqrt(dot(coord, coord));
    
    if(vertical_scanlines == false)
      return clamp((cdist.x - dist)*cornersmooth, 0.0, 1.0);
    else
      return clamp((cdist.y - dist)*cornersmooth, 0.0, 1.0);
}

float fwidth(float value){
  return abs(ddx(value)) + abs(ddy(value));
}



float4 PS_CRT_Geom(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in ST_VertexOut vVARS) : SV_Target
{
    // Here's a helpful diagram to keep in mind while trying to
    // understand the code:
    //
    //  |      |      |      |      |
    // -------------------------------
    //  |      |      |      |      |
    //  |  01  |  11  |  21  |  31  | <-- current scanline
    //  |      | @    |      |      |
    // -------------------------------
    //  |      |      |      |      |
    //  |  02  |  12  |  22  |  32  | <-- next scanline
    //  |      |      |      |      |
    // -------------------------------
    //  |      |      |      |      |
    //
    // Each character-cell represents a pixel on the output
    // surface, "@" represents the current pixel (always somewhere
    // in the bottom half of the current scan-line, or the top-half
    // of the next scanline). The grid of lines represents the
    // edges of the texels of the underlying texture.

    // Texture coordinates of the texel containing the active pixel.
    float2 xy;

    if (CURVATURE == true)
      xy = transform(vTexCoord, vVARS.sinangle, vVARS.cosangle, vVARS.stretch);
    else
      xy = vTexCoord;

    float cval = corner((xy-float2(0.5,0.5)) * BufferToViewportRatio + float2(0.5,0.5));

    // Of all the pixels that are mapped onto the texel we are
    // currently rendering, which pixel are we currently rendering?
   float2 ilvec;
   if(vertical_scanlines == false)
      ilvec = float2(0.0, vVARS.ilfac.y * interlace_detect > 1.5 ? (float(FrameCount) % 2.0) : 0.0);
   else
      ilvec = float2(vVARS.ilfac.x * interlace_detect > 1.5 ? (float(FrameCount) % 2.0) : 0.0, 0.0);

    float2 ratio_scale = (xy * vVARS.TextureSize - float2(0.5, 0.5) + ilvec) / vVARS.ilfac;
    float2 uv_ratio = frac(ratio_scale);

    // Snap to the center of the underlying texel.
    xy = (floor(ratio_scale)*vVARS.ilfac + float2(0.5, 0.5) - ilvec) / vVARS.TextureSize;

    // Calculate Lanczos scaling coefficients describing the effect
    // of various neighbour texels in a scanline on the current
    // pixel.
    float4 coeffs;
    if(vertical_scanlines == false)
      coeffs = PI * float4(1.0 + uv_ratio.x, uv_ratio.x, 1.0 - uv_ratio.x, 2.0 - uv_ratio.x);
    else
      coeffs = PI * float4(1.0 + uv_ratio.y, uv_ratio.y, 1.0 - uv_ratio.y, 2.0 - uv_ratio.y);

    // Prevent division by zero.
    coeffs = FIX(coeffs);

    // Lanczos2 kernel.
    coeffs = 2.0 * sin(coeffs) * sin(coeffs / 2.0) / (coeffs * coeffs);

    // Normalize.
    coeffs /= dot(coeffs, float4(1.0, 1.0, 1.0, 1.0));

    // Calculate the effective colour of the current and next
    // scanlines at the horizontal location of the current pixel,
    // using the Lanczos coefficients above.
    float4 col, col2;
    if(vertical_scanlines == false)
    {
       col = clamp(
           mul(coeffs, float4x4(
               TEX2D(xy + float2(-vVARS.one.x, 0.0)),
               TEX2D(xy),
               TEX2D(xy + float2(vVARS.one.x, 0.0)),
               TEX2D(xy + float2(2.0 * vVARS.one.x, 0.0))
           )),
           0.0, 1.0
       );
       col2 = clamp(
           mul(coeffs, float4x4(
               TEX2D(xy + float2(-vVARS.one.x, vVARS.one.y)),
               TEX2D(xy + float2(0.0, vVARS.one.y)),
               TEX2D(xy + vVARS.one),
               TEX2D(xy + float2(2.0 * vVARS.one.x, vVARS.one.y))
           )),
           0.0, 1.0
       );
    }else{
       col = clamp(
           mul(coeffs, float4x4(
               TEX2D(xy + float2(0.0, -vVARS.one.y)),
               TEX2D(xy),
               TEX2D(xy + float2(0.0, vVARS.one.y)),
               TEX2D(xy + float2(0.0, 2.0 * vVARS.one.y))
           )),
           0.0, 1.0
       );
       col2 = clamp(
           mul(coeffs, float4x4(
               TEX2D(xy + float2(vVARS.one.x, -vVARS.one.y)),
               TEX2D(xy + float2(vVARS.one.x, 0.0)),
               TEX2D(xy + vVARS.one),
               TEX2D(xy + float2(vVARS.one.x, 2.0 * vVARS.one.y))
           )),
           0.0, 1.0
       );
    }

#ifndef LINEAR_PROCESSING
    col  = pow(col , float4(CRTgamma, CRTgamma, CRTgamma, CRTgamma));
    col2 = pow(col2, float4(CRTgamma, CRTgamma, CRTgamma, CRTgamma));
#endif

    // Calculate the influence of the current and next scanlines on
    // the current pixel.
    float4 weights, weights2;
    if(vertical_scanlines == false)
    {
       weights  = scanlineWeights(uv_ratio.y, col);
       weights2 = scanlineWeights(1.0 - uv_ratio.y, col2);

   #ifdef OVERSAMPLE
       float filter  = fwidth(ratio_scale.y);
       uv_ratio.y    = uv_ratio.y + 1.0/3.0*filter;
       weights       = (weights  + scanlineWeights(uv_ratio.y, col))/3.0;
       weights2      = (weights2 + scanlineWeights(abs(1.0 - uv_ratio.y), col2))/3.0;
       uv_ratio.y    = uv_ratio.y - 2.0/3.0*filter;
       weights       = weights  + scanlineWeights(abs(uv_ratio.y), col)/3.0;
       weights2      = weights2 + scanlineWeights(abs(1.0 - uv_ratio.y), col2)/3.0;
   #endif
    }else{
       weights  = scanlineWeights(uv_ratio.x, col);
       weights2 = scanlineWeights(1.0 - uv_ratio.x, col2);

   #ifdef OVERSAMPLE
       float filter  = fwidth(ratio_scale.x);
       uv_ratio.x    = uv_ratio.x + 1.0/3.0*filter;
       weights       = (weights  + scanlineWeights(uv_ratio.x, col))/3.0;
       weights2      = (weights2 + scanlineWeights(abs(1.0 - uv_ratio.x), col2))/3.0;
       uv_ratio.x    = uv_ratio.x - 2.0/3.0*filter;
       weights       = weights  + scanlineWeights(abs(uv_ratio.x), col)/3.0;
       weights2      = weights2 + scanlineWeights(abs(1.0 - uv_ratio.x), col2)/3.0;
   #endif
    }

    float3 mul_res  = (col * weights + col2 * weights2).rgb;
    mul_res *= float3(cval, cval, cval);

    // dot-mask emulation:
    // Output pixels are alternately tinted green and magenta.
    float3 dotMaskWeights = lerp(
        float3(1.0, 1.0 - DOTMASK, 1.0),
        float3(1.0 - DOTMASK, 1.0, 1.0 - DOTMASK),
        floor((vVARS.mod_factor % 2.0))
    );
      
    mul_res *= dotMaskWeights;

    // Convert the image gamma for display on our output device.
    mul_res = pow(mul_res, float3(1.0 / monitorgamma, 1.0 / monitorgamma, 1.0 / monitorgamma));

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
