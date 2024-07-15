#include "ReShade.fxh"

/*
   Bicubic multipass Shader

   Copyright (C) 2011-2022 Hyllian - sergiogdb@gmail.com

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

uniform int BICUBIC_FILTER <
	ui_type = "combo";
	ui_items = "Bicubic\0Catmull-Rom\0B-Spline\0Hermite\0";
	ui_label = "Bicubic Filter";
	ui_tooltip = "Bicubic: balanced. Catmull-Rom: sharp. B-Spline: blurred. Hermite: soft pixelized.";
> = 0;

uniform float B_PRESCALE <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 8.0;
	ui_step = 1.0;
	ui_label = "Prescale factor";
> = 1.0;

uniform bool B_ANTI_RINGING <
	ui_type = "radio";
	ui_label = "Anti-Ringing";
> = false;

uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;
uniform float  BufferWidth < source = "bufferwidth"; >;

texture2D tBicubic_P0{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA8;};
sampler2D sBicubic_P0{Texture=tBicubic_P0;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=POINT;MinFilter=POINT;};


#define AR_STRENGTH 1.0

// Classic Mitchell-Netravali bicubic parameters

float4x4 get_inv()
{
	float bf = 1.0/3.0;
	float cf = 1.0/3.0;

	if (BICUBIC_FILTER == 1) {bf = 0.0;     cf = 0.5;}
	if (BICUBIC_FILTER == 2) {bf = 1.0;     cf = 0.0;}
	if (BICUBIC_FILTER == 3) {bf = 0.0;     cf = 0.0;}

        return float4x4(            (-bf - 6.0*cf)/6.0,         (3.0*bf + 12.0*cf)/6.0,     (-3.0*bf - 6.0*cf)/6.0,             bf/6.0,
                                        (12.0 - 9.0*bf - 6.0*cf)/6.0, (-18.0 + 12.0*bf + 6.0*cf)/6.0,                      0.0, (6.0 - 2.0*bf)/6.0,
                                       -(12.0 - 9.0*bf - 6.0*cf)/6.0, (18.0 - 15.0*bf - 12.0*cf)/6.0,      (3.0*bf + 6.0*cf)/6.0,             bf/6.0,
                                                   (bf + 6.0*cf)/6.0,                           -cf,                      0.0,               0.0);
         
         
}

float3 bicubic_ar(float fp, float3 C0, float3 C1, float3 C2, float3 C3)
{
    float4 weights = mul(get_inv(), float4(fp*fp*fp, fp*fp, fp, 1.0));
    float3 color   = mul(weights, float4x3( C0, C1, C2, C3 ));

    // Anti-ringing
    if (B_ANTI_RINGING == true)
    {
        float3 aux = color;
        float3 min_sample = min(min(C0, C1), min(C2, C3));
        float3 max_sample = max(max(C0, C1), max(C2, C3));
        color = clamp(color, min_sample, max_sample);
        color = lerp(aux, color, step(0.0, (C0-C1)*(C2-C3)));
    }

    return color;
}


float4 PS_Bicubic_X(float4 vpos: SV_Position, float2 uv_tx : TEXCOORD) : SV_Target
{
    // Both dimensions are unfiltered, so it looks for lores pixels.
    float2 ps  = NormalizedNativePixelSize/B_PRESCALE;
    float2 pos = uv_tx.xy/ps - float2(0.5, 0.0);
    float2 tc  = (floor(pos) + 0.5.xx) * ps;
    float2 fp  = frac(pos);

    float3 C0 = tex2D(ReShade::BackBuffer, tc + ps*float2(-1.0, 0.0)).rgb;
    float3 C1 = tex2D(ReShade::BackBuffer, tc + ps*float2( 0.0, 0.0)).rgb;
    float3 C2 = tex2D(ReShade::BackBuffer, tc + ps*float2( 1.0, 0.0)).rgb;
    float3 C3 = tex2D(ReShade::BackBuffer, tc + ps*float2( 2.0, 0.0)).rgb;

    float3 color = bicubic_ar(fp.x, C0, C1, C2, C3);

    return float4(color, 1.0);
}


float4 PS_Bicubic_Y(float4 vpos: SV_Position, float2 uv_tx : TEXCOORD) : SV_Target
{
    // One must be careful here. Horizontal dimension is already filtered, so it looks for x in hires.
    float2 ps  = float2(1.0/BufferWidth, NormalizedNativePixelSize.y/B_PRESCALE);
    float2 pos = uv_tx.xy/ps - float2(0.0, 0.5);
    float2 tc  = (floor(pos) + 0.5.xx) * ps;
    float2 fp  = frac(pos);

    float3 C0 = tex2D(sBicubic_P0, tc + ps*float2(0.0, -1.0)).rgb;
    float3 C1 = tex2D(sBicubic_P0, tc + ps*float2(0.0,  0.0)).rgb;
    float3 C2 = tex2D(sBicubic_P0, tc + ps*float2(0.0,  1.0)).rgb;
    float3 C3 = tex2D(sBicubic_P0, tc + ps*float2(0.0,  2.0)).rgb;

    float3 color = bicubic_ar(fp.y, C0, C1, C2, C3);

    return float4(color, 1.0);
}


technique Bicubic
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader  = PS_Bicubic_X;
		RenderTarget = tBicubic_P0;
	}
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader  = PS_Bicubic_Y;
	}
}
