#include "ReShade.fxh"

/*
    zfast_crt_geo - A simple, fast CRT shader.

    Copyright (C) 2017 Greg Hogan (SoltanGris42)
    Copyright (C) 2023 Jose Linares (Dogway)

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.


Notes:  This shader does scaling with a weighted linear filter
        based on the algorithm by Iñigo Quilez here:
        https://iquilezles.org/articles/texture/
        but modified to be somewhat sharper. Then a scanline effect that varies
        based on pixel brightness is applied along with a monochrome aperture mask.
        This shader runs at ~60fps on the Chromecast HD (10GFlops) on a 1080p display.
        (https://forums.libretro.com/t/android-googletv-compatible-shaders-nitpicky)

Dogway: I modified zfast_crt.glsl shader to include screen curvature,
        vignetting, round corners and phosphor*temperature. Horizontal pixel is left out
        from the Quilez' algo (read above) to provide a more S-Video like horizontal blur.
        The scanlines and mask are also now performed in the recommended linear light.
        For this to run smoothly on GPU deprived platforms like the Chromecast and
        older consoles, I had to remove several parameters and hardcode them into the shader.
        Another POV is to run the shader on handhelds like the Switch or SteamDeck so they consume less battery.

*/


uniform float SCANLINE_WEIGHT <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 15.0;
    ui_step = 0.5;
    ui_label = "Scanline Amount";
> = 7.0;

uniform float MASK_DARK <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.05;
    ui_label = "Mask Effect Amount";
> = 0.5;

uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;
uniform float BufferWidth < source = "bufferwidth"; >;
uniform float BufferHeight < source = "bufferheight"; >;

sampler2D sBackBuffer{Texture=ReShade::BackBufferTex;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=LINEAR;MinFilter=LINEAR;};

struct ST_VertexOut
{
    float2 invDims : TEXCOORD1;
};

// Vertex shader generating a triangle covering the entire screen
void VS_CRT_Geo_zFast(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out ST_VertexOut vVARS)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    vVARS.invDims = NormalizedNativePixelSize;
}


#define MSCL (BufferHeight > 1499.0 ? 0.3333 : 0.5)
// This compensates the scanline+mask embedded gamma from the beam dynamics
#define pwr ((1.0/((-0.0325*SCANLINE_WEIGHT+1.0)*(-0.311*MASK_DARK+1.0))-1.2).xxx)



// NTSC-J (D93) -> Rec709 D65 Joint Matrix (with D93 simulation)
// This is compensated for a linearization hack (RGB*RGB and then sqrt())
static const float3x3 P22D93 = float3x3(
     1.00000, 0.00000, -0.06173,
     0.07111, 0.96887, -0.01136,
     0.00000, 0.08197,  1.07280);


// Returns gamma corrected output, compensated for scanline+mask embedded gamma
float3 inv_gamma(float3 col, float3 power)
{
    float3 cir  = col-1.0;
         cir *= cir;
         col  = lerp(sqrt(col),sqrt(1.0-cir),power);
    return col;
}

float2 Warp(float2 pos)
{
    pos  = pos*2.0-1.0;
    pos *= float2(1.0 + (pos.y*pos.y)*0.0276, 1.0 + (pos.x*pos.x)*0.0414);
    return pos*0.5 + 0.5;
}


float4 PS_CRT_Geo_zFast(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD0, in ST_VertexOut vVARS) : SV_Target
{
    float2 pos   = vTexCoord;
    float2 xy     = Warp(pos);

    float2 corn   = min(xy,1.0-xy); // This is used to mask the rounded
           corn.x = 0.0001/corn.x;  // corners later on

          pos *= (1.0 - pos.xy);
    float vig   = pos.x * pos.y * 46.0;
          vig   = min(sqrt(vig), 1.0);


    // Of all the pixels that are mapped onto the texel we are
    // currently rendering, which pixel are we currently rendering?
    float ratio_scale = xy.y / NormalizedNativePixelSize.y - 0.5;
    // Snap to the center of the underlying texel.
    float i = floor(ratio_scale) + 0.5;

    // This is just like "Quilez Scaling" but sharper
    float f = ratio_scale - i;
    float Y = f*f;
    float p = (i + 4.0*Y*f)*vVARS.invDims.y;

    float whichmask = floor(vTexCoord.x*BufferWidth)*(-MSCL);
    float mask = 1.0 + float(frac(whichmask) < MSCL)*(-MASK_DARK);
    float3 colour = tex2D(sBackBuffer, float2(xy.x,p)).rgb;

    colour = max(mul(P22D93 * vig, colour*colour), 0.0.xxx);

    float scanLineWeight = (1.5 - SCANLINE_WEIGHT*(Y - Y*Y));

    if (corn.y <= corn.x || corn.x < 0.0001 )
    colour = 0.0.xxx;

    return float4(inv_gamma(colour.rgb*lerp(scanLineWeight*mask, 1.0, colour.r*0.26667+colour.g*0.26667+colour.b*0.26667),pwr),1.0);
}



technique CRT_Geo_zFast
{
   pass
   {
       VertexShader = VS_CRT_Geo_zFast;
       PixelShader  = PS_CRT_Geo_zFast;
   }
}
