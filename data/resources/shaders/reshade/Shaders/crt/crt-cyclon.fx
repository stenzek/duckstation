#include "ReShade.fxh"


//  DariusG presents

//  'crt-Cyclon' 

//  Why? Because it's speedy!

//  A super-fast shader based on the magnificent crt-Geom, optimized for full speed 
//  on a Xiaomi Note 3 Pro cellphone (around 170(?) gflops gpu or so)

//  This shader uses parts from:
//  crt-Geom (scanlines)
//  Quillez (main filter)
//  Grade (some primaries)
//  Dogway's inverse Gamma
//  Masks-slot-color handling, tricks etc are mine.

//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU General Public License as published by the Free
//  Software Foundation; either version 2 of the License, or (at your option)
//  any later version.



uniform float SCANLINE <
	ui_type = "drag";
	ui_min = 0.2;
	ui_max = 0.6;
	ui_step = 0.05;
	ui_label = "Scanline Weight";
> = 0.3;

uniform bool INTERLACE <
	ui_type = "radio";
	ui_label = "Interlacing On/Off";
> = 1.0;

uniform float bogus_msk <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 0.0;
	ui_step = 0.0;
	ui_label = " [ MASK SETTINGS ] ";
> = 0.0;

uniform float M_TYPE <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 1.0;
	ui_label = "Mask Type: -1:None, 0:CGWG, 1:RGB";
> = 1.0;

uniform float MSIZE <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 2.0;
	ui_step = 1.0;
	ui_label = "Mask Size";
> = 1.0;

uniform bool SLOT <
	ui_type = "radio";
	ui_label = "Slot Mask On/Off";
> = 1.0;

uniform float SLOTW <
	ui_type = "drag";
	ui_min = 2.0;
	ui_max = 3.0;
	ui_step = 1.0;
	ui_label = "Slot Mask Width";
> = 3.0;

uniform float BGR <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 1.0;
	ui_label = "Subpixels BGR/RGB";
> = 0.0;

uniform float Maskl <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.05;
	ui_label = "Mask Brightness Dark";
> = 0.3;

uniform float Maskh <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.05;
	ui_label = "Mask Brightness Bright";
> = 0.75;

uniform float bogus_geom <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 0.0;
	ui_step = 0.0;
	ui_label = " [ GEOMETRY SETTINGS ] ";
> = 0.0;

uniform bool bzl <
	ui_type = "radio";
	ui_label = "Bezel On/Off";
> = 1.0;

uniform float ambient <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.05;
	ui_label = "Ambient Light";
> = 0.40;

uniform float zoomx <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.005;
	ui_label = "Zoom Image X";
> = 0.0;

uniform float zoomy <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.005;
	ui_label = "Zoom Image Y";
> = 0.0;

uniform float centerx <
	ui_type = "drag";
	ui_min = -5.0;
	ui_max = 5.0;
	ui_step = 0.05;
	ui_label = "Image Center X";
> = 0.0;

uniform float centery <
	ui_type = "drag";
	ui_min = -5.0;
	ui_max = 5.0;
	ui_step = 0.05;
	ui_label = "Image Center Y";
> = 0.0;

uniform float WARPX <
	ui_type = "drag";
	ui_min = 0.00;
	ui_max = 0.25;
	ui_step = 0.01;
	ui_label = "Curvature Horizontal";
> = 0.02;

uniform float WARPY <
	ui_type = "drag";
	ui_min = 0.00;
	ui_max = 0.25;
	ui_step = 0.01;
	ui_label = "Curvature Vertical";
> = 0.01;

uniform bool vig <
	ui_type = "radio";
	ui_label = "Vignette On/Off";
> = 1.0;

uniform float bogus_col <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 0.0;
	ui_step = 0.0;
	ui_label = " [ COLOR SETTINGS ] ";
> = 0.0;

uniform float BR_DEP <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 0.333;
	ui_step = 0.01;
	ui_label = "Scan/Mask Brightness Dependence";
> = 0.2;

uniform float c_space <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 3.0;
	ui_step = 1.0;
	ui_label = "Color Space: sRGB,PAL,NTSC-U,NTSC-J";
> = 0.0;

uniform float EXT_GAMMA <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 1.0;
	ui_label = "External Gamma In (Glow etc)";
> = 0.0;

uniform float SATURATION <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 2.0;
	ui_step = 0.05;
	ui_label = "Saturation";
> = 1.0;

uniform float BRIGHTNESS_ <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 2.0;
	ui_step = 0.01;
	ui_label = "Brightness, Sega fix:1.06";
> = 1.0;

uniform float BLACK  <
	ui_type = "drag";
	ui_min = -0.20;
	ui_max = 0.20;
	ui_step = 0.01;
	ui_label = "Black Level";
> = 0.0;

uniform float RG <
	ui_type = "drag";
	ui_min = -0.25;
	ui_max = 0.25;
	ui_step = 0.01;
	ui_label = "Green <-to-> Red Hue";
> = 0.0;

uniform float RB <
	ui_type = "drag";
	ui_min = -0.25;
	ui_max = 0.25;
	ui_step = 0.01;
	ui_label = "Blue <-to-> Red Hue";
> = 0.0;

uniform float GB <
	ui_type = "drag";
	ui_min = -0.25;
	ui_max = 0.25;
	ui_step = 0.01;
	ui_label = "Blue <-to-> Green Hue";
> = 0.0;

uniform float bogus_con <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 0.0;
	ui_step = 0.0;
	ui_label = " [ CONVERGENCE SETTINGS ] ";
> = 0.0;

uniform float C_STR <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 0.5;
	ui_step = 0.05;
	ui_label = "Convergence Overall Strength";
> = 0.0;

uniform float CONV_R <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.05;
	ui_label = "Convergence Red X-Axis";
> = 0.0;

uniform float CONV_G <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.05;
	ui_label = "Convergence Green X-axis";
> = 0.0;

uniform float CONV_B <
	ui_type = "drag";
	ui_min = -1.0;
	ui_max = 1.0;
	ui_step = 0.05;
	ui_label = "Convergence Blue X-Axis";
> = 0.0;

uniform float POTATO <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 1.0;
	ui_label = "Potato Boost(Simple Gamma, adjust Mask)";
> = 0.0;


#define blck ((1.0)/(1.0-BLACK))
#define pi 3.1415926535897932384626433

uniform float2 BufferViewportRatio < source = "buffer_to_viewport_ratio"; >;
uniform float2 InternalPixelSize < source = "internal_pixel_size"; >;
uniform float2 NativePixelSize < source = "native_pixel_size"; >;
uniform float2 NormalizedInternalPixelSize < source = "normalized_internal_pixel_size"; >;
uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;
uniform float  UpscaleMultiplier < source = "upscale_multiplier"; >;
uniform float2 ViewportSize < source = "viewportsize"; >;
uniform int FrameCount < source = "framecount"; >;

sampler2D sBackBuffer{Texture=ReShade::BackBufferTex;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=LINEAR;MinFilter=LINEAR;MipFilter=LINEAR;};

texture tBezel < source = "crt-cyclon/bezel.png"; >
{
	Width = BUFFER_WIDTH;
	Height = BUFFER_HEIGHT;
	MipLevels = 1;
};

sampler sBezel { Texture = tBezel; AddressU = BORDER; AddressV = BORDER; MinFilter = LINEAR; MagFilter = LINEAR;};

float3 Mask(float2 pos, float CGWG)
{
    float3 mask = float3(CGWG,CGWG,CGWG);
    
    
if (M_TYPE == 0.0){

    if (POTATO == 1.0) { float pot = (1.0-CGWG)*sin(pos.x*pi)+CGWG; return float3(pot,pot,pot); }
    else{
    	    float m = frac(pos.x*0.5);

	    if (m<0.5) mask.rb = float2(1.0,1.0);
    	    else mask.g = 1.0;

	    return mask;
	}
}

if (M_TYPE == 1.0){

    if (POTATO == 1.0) { float pot = (1.0-CGWG)*sin(pos.x*pi*0.6667)+CGWG;  return float3(pot,pot,pot );}
    else{
    float m = frac(pos.x*0.3333);

    if (m<0.3333) mask.rgb = (BGR == 0.0) ? float3(mask.r, mask.g, 1.0) : float3(1.0, mask.g, mask.b);
    else if (m<0.6666)         mask.g = 1.0;
    else          mask.rgb = (BGR == 0.0) ? float3(1.0, mask.g, mask.b) : float3(mask.r, mask.g, 1.0);
    return mask;
    }
}
    else return float3(1.0,1.0,1.0);

}

float scanlineWeights(float distance, float3 color, float x)
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
    float wid = SCANLINE + 0.15 * dot(color, float3(0.25-0.8*x, 0.25-0.8*x, 0.25-0.8*x));   //0.8 vignette strength
    float weights = distance / wid;
    return 0.4 * exp(-weights * weights ) / wid;
    }

#define pwr float3(1.0/((-1.0*SCANLINE+1.0)*(-0.8*CGWG+1.0))-1.2,1.0/((-1.0*SCANLINE+1.0)*(-0.8*CGWG+1.0))-1.2,1.0/((-1.0*SCANLINE+1.0)*(-0.8*CGWG+1.0))-1.2)
// Returns gamma corrected output, compensated for scanline+mask embedded gamma
float3 inv_gamma(float3 col, float3 power)
{
    float3 cir  = col-1.0;
         cir *= cir;
         col  = lerp(sqrt(col),sqrt(1.0-cir),power);
    return col;
}

// standard 6500k
static const float3x3 PAL = float3x3(                    
1.0740  ,   -0.0574 ,   -0.0119 ,
0.0384  ,   0.9699  ,   -0.0059 ,
-0.0079 ,   0.0204  ,   0.9884  );

// standard 6500k
static const float3x3 NTSC = float3x3(                   
0.9318  ,   0.0412  ,   0.0217  ,
0.0135  ,   0.9711  ,   0.0148  ,
0.0055  ,   -0.0143 ,   1.0085  );

// standard 8500k
static const float3x3 NTSC_J = float3x3(                    
0.9501  ,   -0.0431 ,   0.0857  ,
0.0265  ,   0.9278  ,   0.0432  ,
0.0011  ,   -0.0206 ,   1.3153  );

float3 slot(float2 pos)
{
    float h = frac(pos.x/SLOTW);
    float v = frac(pos.y);
    
    float odd;
    if (v<0.5) odd = 0.0; else odd = 1.0;

if (odd == 0.0)
    {if (h<0.5) return float3(0.5,0.5,0.5); else return float3(1.5,1.5,1.5);}

else if (odd == 1.0)
    {if (h<0.5) return float3(1.5,1.5,1.5); else return float3(0.5,0.5,0.5);}
}

float2 Warp(float2 pos)
{
    pos = pos*2.0-1.0;
    pos *= float2(1.0+pos.y*pos.y*WARPX, 1.0+pos.x*pos.x*WARPY);
    pos = pos*0.5+0.5;

    return pos;
}

uniform float2 BufferHeight < source = "bufferheight"; >;

float4 CRT_CYCLON_PS(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD0) : SV_Target
{
    float4 SourceSize = float4(1.0 / NormalizedNativePixelSize, NormalizedNativePixelSize);
    float2 OutputSize = ViewportSize;

    float2 scale = BufferViewportRatio.xy;
    float2 warpcoords = (vTexCoord-float2(0.5,0.5)) * BufferViewportRatio + float2(0.5,0.5);

// Hue matrix inside main() to avoid GLES error
float3x3 hue = float3x3(
    1.0, -RG, -RB,
    RG, 1.0, -GB,
    RB, GB, 1.0
);
// zoom in and center screen for bezel
    float2 pos = Warp((vTexCoord*float2(1.0-zoomx,1.0-zoomy)-float2(centerx,centery)/100.0));
    float4 bez = float4(0.0,0.0,0.0,0.0);
//    if (bzl == 1.0) bez = tex2D(sBezel,vTexCoord*SourceSize.xy/OriginalSize.xy*0.97+float2(0.015,0.015));   
//    if (bzl == 1.0) bez = tex2D(sBezel,vTexCoord*scale*0.97+float2(0.015,0.015));   
    if (bzl == true) bez = tex2D(sBezel,warpcoords*0.97+float2(0.015,0.015));  // This fix Bezel to adjust to Game's aspect ratio. 

    bez.rgb = lerp(bez.rgb, float3(ambient,ambient,ambient),0.5);

    float2 bpos = pos;
    float2 ps = SourceSize.zw;
    float2 dx = float2(ps.x,0.0);
// Quilez
    float2 ogl2 = pos*SourceSize.xy;
    float2 i = floor(pos*SourceSize.xy) + 0.5;
    float f = ogl2.y - i.y;
    pos.y = (i.y + 4.0*f*f*f)*ps.y; // smooth
    pos.x = lerp(pos.x, i.x*ps.x, 0.2);

// Convergence
    float3  res0 = tex2D(sBackBuffer,pos).rgb;
    float resr = tex2D(sBackBuffer,pos + dx*CONV_R).r;
    float resb = tex2D(sBackBuffer,pos + dx*CONV_B).b;
    float resg = tex2D(sBackBuffer,pos + dx*CONV_G).g;

    float3 res = float3(  res0.r*(1.0-C_STR) +  resr*C_STR,
                      res0.g*(1.0-C_STR) +  resg*C_STR,
                      res0.b*(1.0-C_STR) +  resb*C_STR 
                   );
// Vignette
    float x = 0.0;
    if (vig == true){
    x = vTexCoord.x*scale.x-0.5;
//    x = vTexCoord.x-0.5;
    x = x*x;}

    float l = dot(float3(BR_DEP,BR_DEP,BR_DEP),res);
 
 // Color Spaces   
    if(EXT_GAMMA != 1.0) res *= res;
    if (c_space != 0.0) {
    if (c_space == 1.0) res = mul(PAL,res);
    if (c_space == 2.0) res = mul(NTSC,res);
    if (c_space == 3.0) res = mul(NTSC_J,res);
// Apply CRT-like luminances
    res /= float3(0.24,0.69,0.07);
    res *= float3(0.29,0.6,0.11); 
    res = clamp(res,0.0,1.0);
    }
    float s = frac(bpos.y*SourceSize.y-0.5);
// handle interlacing
    if (SourceSize.y > 400.0) 
    {
        s = frac(bpos.y*SourceSize.y/2.0-0.5);
//        if (INTERLACE == 1.0) s = mod(float(FrameCount),2.0) < 1.0 ? s: s+0.5;
        if (INTERLACE == true) s = (float(FrameCount) % 2.0) < 1.0 ? s: s+0.5;
    }
// Calculate CRT-Geom scanlines weight and apply
    float weight  = scanlineWeights(s, res, x);
    float weight2 = scanlineWeights(1.0-s, res, x);
    res *= weight + weight2;

// Masks
    float2 xy = vTexCoord*OutputSize.xy*scale/MSIZE;    
//    float2 xy = vTexCoord*OutputSize.xy/MSIZE;    
    float CGWG = lerp(Maskl, Maskh, l);
    res *= Mask(xy, CGWG);
// Apply slot mask on top of Trinitron-like mask
    if (SLOT == true) res *= lerp(slot(xy/2.0),float3(1.0,1.0,1.0),CGWG);
    
    if (POTATO == 0.0) res = inv_gamma(res,pwr);
    else {res = sqrt(res); res *= lerp(1.3,1.1,l);}

// Saturation
    float lum = dot(float3(0.29,0.60,0.11),res);
    res = lerp(float3(lum,lum,lum),res,SATURATION);

// Brightness, Hue and Black Level
    res *= BRIGHTNESS_;
    res = mul(hue,res);
    res -= float3(BLACK,BLACK,BLACK);
    res *= blck;
// Apply bezel code, adapted from New-Pixie
    if (bzl == true)
    res.rgb = lerp(res.rgb, lerp(max(res.rgb, 0.0), pow( abs(bez.rgb), float3( 1.4,1.4,1.4 ) ), bez.w * bez.w), float3( 1.0,1.0,1.0 ) );


    return float4(res, 1.0);
}



technique CRT_CYCLON
{
   pass PS_CRT_CYCLON
   {
   	VertexShader = PostProcessVS;
   	PixelShader  = CRT_CYCLON_PS;
   }
}
