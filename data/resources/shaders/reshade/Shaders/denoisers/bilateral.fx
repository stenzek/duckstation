#include "ReShade.fxh"

/*
   Bilateral - Smart
   
   Copyright (C) 2024 guest(r)

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
   
*/ 


uniform float FRANGE <
    ui_type = "drag";
    ui_min = 1.0;
    ui_max = 10.0;
    ui_step = 1.0;
    ui_label = "Filter Range";
> = 5.0;

uniform float FBSMOOTH <
    ui_type = "drag";
    ui_min = 0.05;
    ui_max = 1.0;
    ui_step = 0.025;
    ui_label = "Filter Base Smoothing";
> = 0.3;

uniform float FSIGMA <
    ui_type = "drag";
    ui_min = 0.15;
    ui_max = 1.5;
    ui_step = 0.05;
    ui_label = "Filter Strength";
> = 1.0;

uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;
uniform float2 BufferToViewportRatio < source = "buffer_to_viewport_ratio"; >;
uniform float2 ViewportSize < source = "viewportsize"; >;

sampler2D sBackBuffer{Texture=ReShade::BackBufferTex;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=POINT;MinFilter=POINT;};

texture2D tBilateral_P0{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA8;};
sampler2D sBilateral_P0{Texture=tBilateral_P0;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=POINT;MinFilter=POINT;};

#define FSIGMA1 (1.0/FSIGMA)

#define COMPAT_TEXTURE(c,d) tex2D(c,d)

float wt(float3 A, float3 B)
{    
    return clamp(FBSMOOTH - 2.33*dot(abs(A-B),1.0.xxx)/(dot(A+B,1.0.xxx)+1.0), 0.0, 0.25);
}


float getw(float x, float3 c, float3 p)
{
    float y = pow(max(1.0-x,0.0), FSIGMA1);
    float d = wt(c,p);
    return y*d;
}



float4 PS_Bilateral_X(float4 position: SV_Position, float2 vTexCoord : TEXCOORD) : SV_Target
{
    float4 SourceSize   = float4((ViewportSize*BufferToViewportRatio), 1.0/(ViewportSize*BufferToViewportRatio));
//    float4 SourceSize   = float4(1.0/NormalizedNativePixelSize, NormalizedNativePixelSize);
    float2 pos = vTexCoord * SourceSize.xy;
    float f =  0.5-frac(pos.x);
    float2 tex = floor(pos)*SourceSize.zw + 0.5*SourceSize.zw;
    float2 dx  = float2(SourceSize.z, 0.0);
    
    float w, fp;
    float wsum = 0.0;
    float3 pixel;
    float FPR = FRANGE;
    float FPR1 = 1.0/FPR;
    float LOOPSIZE = FPR;
    float x = -FPR;

    float3 comp = COMPAT_TEXTURE(sBackBuffer, tex).rgb;
    float3 color = 0.0.xxx;
    
    do
    {
        pixel  = COMPAT_TEXTURE(sBackBuffer, tex + x*dx).rgb;        
        fp = min(abs(x+f),FPR)*FPR1;
        w = getw(fp,comp,pixel);            
        color = color + w * pixel;
        wsum   = wsum + w;

        x = x + 1.0;
        
    } while (x <= LOOPSIZE);

    color = color / wsum;

    return float4(color, 1.0);
}


float4 PS_Bilateral_Y(float4 position: SV_Position, float2 vTexCoord : TEXCOORD) : SV_Target
{
    float4 SourceSize   = float4((ViewportSize*BufferToViewportRatio), 1.0/(ViewportSize*BufferToViewportRatio));
    float2 pos = vTexCoord * SourceSize.xy;
    float f =  0.5-frac(pos.y);
    float2 tex = floor(pos)*SourceSize.zw + 0.5*SourceSize.zw;
    float2 dy  = float2(0.0, SourceSize.w);
    
    float w, fp;
    float wsum = 0.0;
    float3 pixel;
    float FPR = FRANGE;
    float FPR1 = 1.0/FPR;
    float LOOPSIZE = FPR;
    float y = -FPR;

    float3 comp = COMPAT_TEXTURE(sBilateral_P0, tex).rgb;
    float3 color = 0.0.xxx;
    
    do
    {
        pixel  = COMPAT_TEXTURE(sBilateral_P0, tex + y*dy).rgb;        
        fp = min(abs(y+f),FPR)*FPR1;
        w = getw(fp,comp,pixel);            
        color = color + w * pixel;
        wsum   = wsum + w;

        y = y + 1.0;
        
    } while (y <= LOOPSIZE);

    color = color / wsum;

    return float4(color, 1.0);
}

technique Bilateral
{

    pass
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_Bilateral_X;
        RenderTarget = tBilateral_P0;
    }
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_Bilateral_Y;
    }

}
