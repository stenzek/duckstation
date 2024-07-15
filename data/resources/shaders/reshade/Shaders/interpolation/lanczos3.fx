#include "ReShade.fxh"

/*
   Lanczos3 - Multipass code by Hyllian 2022.

*/


/*
   Copyright (C) 2010 Team XBMC
   http://www.xbmc.org
   Copyright (C) 2011 Stefanos A.
   http://www.opentk.com

This Program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

This Program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with XBMC; see the file COPYING.  If not, write to
the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
http://www.gnu.org/copyleft/gpl.html
*/

uniform float L3_PRESCALE <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 8.0;
	ui_step = 1.0;
	ui_label = "Prescale factor";
> = 1.0;


uniform bool LANCZOS3_ANTI_RINGING <
	ui_type = "radio";
	ui_label = "Lanczos3 Anti-Ringing";
> = true;

uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;
uniform float  BufferWidth < source = "bufferwidth"; >;

texture2D tLanczos3_P0{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA8;};
sampler2D sLanczos3_P0{Texture=tLanczos3_P0;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=POINT;MinFilter=POINT;};


#define AR_STRENGTH 1.0
#define FIX(c) (max(abs(c),1e-5))
#define PI     3.1415926535897932384626433832795
#define radius 3.0

float3 weight3(float x)
{
   float3 Sampling = FIX(2.0 * PI * float3(x - 1.5, x - 0.5, x + 0.5));

   // Lanczos3. Note: we normalize outside this function, so no point in multiplying by radius.
   return sin(Sampling) * sin(Sampling / radius) / (Sampling * Sampling);
}

float3 lanczos3ar(float fp, float3 C0, float3 C1, float3 C2, float3 C3, float3 C4, float3 C5)
{
    float3 w1 = weight3(0.5 - fp * 0.5);
    float3 w2 = weight3(1.0 - fp * 0.5);

    float sum   = dot(w1, 1.0.xxx) + dot(w2, 1.0.xxx);
    w1   /= sum;
    w2   /= sum;

    float3 color = mul(w1, float3x3( C0, C2, C4 )) + mul(w2, float3x3( C1, C3, C5));

    // Anti-ringing
    if (LANCZOS3_ANTI_RINGING == true)
    {
        float3 aux = color;
        float3 min_sample = min(min(C1, C2), min(C3, C4));
        float3 max_sample = max(max(C1, C2), max(C3, C4));
        color = clamp(color, min_sample, max_sample);
        color = lerp(aux, color, AR_STRENGTH*step(0.0, (C1-C2)*(C3-C4)));
    }

    return color;
}



float4 PS_Lanczos3_X(float4 vpos: SV_Position, float2 uv_tx : TEXCOORD) : SV_Target
{
    // Both dimensions are unfiltered, so it looks for lores pixels.
    float2 ps  = NormalizedNativePixelSize/L3_PRESCALE;
    float2 pos = uv_tx.xy/ps - float2(0.5, 0.0);
    float2 tc  = (floor(pos) + 0.5.xx) * ps;
    float2 fp  = frac(pos);

    float3 C0 = tex2D(ReShade::BackBuffer, tc + ps*float2(-2.0, 0.0)).rgb;
    float3 C1 = tex2D(ReShade::BackBuffer, tc + ps*float2(-1.0, 0.0)).rgb;
    float3 C2 = tex2D(ReShade::BackBuffer, tc + ps*float2( 0.0, 0.0)).rgb;
    float3 C3 = tex2D(ReShade::BackBuffer, tc + ps*float2( 1.0, 0.0)).rgb;
    float3 C4 = tex2D(ReShade::BackBuffer, tc + ps*float2( 2.0, 0.0)).rgb;
    float3 C5 = tex2D(ReShade::BackBuffer, tc + ps*float2( 3.0, 0.0)).rgb;

    float3 color = lanczos3ar(fp.x, C0, C1, C2, C3, C4, C5);

    return float4(color, 1.0);
}


float4 PS_Lanczos3_Y(float4 vpos: SV_Position, float2 uv_tx : TEXCOORD) : SV_Target
{
    // One must be careful here. Horizontal dimension is already filtered, so it looks for x in hires.
    float2 ps  = float2(1.0/BufferWidth, NormalizedNativePixelSize.y/L3_PRESCALE);
    float2 pos = uv_tx.xy/ps - float2(0.0, 0.5);
    float2 tc  = (floor(pos) + 0.5.xx) * ps;
    float2 fp  = frac(pos);

    float3 C0 = tex2D(sLanczos3_P0, tc + ps*float2(0.0, -2.0)).rgb;
    float3 C1 = tex2D(sLanczos3_P0, tc + ps*float2(0.0, -1.0)).rgb;
    float3 C2 = tex2D(sLanczos3_P0, tc + ps*float2(0.0,  0.0)).rgb;
    float3 C3 = tex2D(sLanczos3_P0, tc + ps*float2(0.0,  1.0)).rgb;
    float3 C4 = tex2D(sLanczos3_P0, tc + ps*float2(0.0,  2.0)).rgb;
    float3 C5 = tex2D(sLanczos3_P0, tc + ps*float2(0.0,  3.0)).rgb;

    float3 color = lanczos3ar(fp.y, C0, C1, C2, C3, C4, C5);

    return float4(color, 1.0);
}


technique Lanczos3
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader  = PS_Lanczos3_X;
		RenderTarget = tLanczos3_P0;
	}
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader  = PS_Lanczos3_Y;
	}
}
