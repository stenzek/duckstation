/*
    Scanlines Sine Absolute Value
    An ultra light scanline shader
    by RiskyJumps
	license: public domain
*/

#include "ReShade.fxh"

uniform float texture_sizeY <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = BUFFER_HEIGHT;
	ui_label = "Scanlines Height [Scanlines-Absolute]";
> = 240.0;

uniform float amp <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 2.0;
	ui_step = 0.05;
	ui_label = "Amplitude [Scanlines-Absolute]";
> = 1.25;

uniform float phase <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 2.0;
	ui_step = 0.05;
	ui_label = "Phase [Scanlines-Absolute]";
> = 0.0;

uniform float lines_black <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.05;
	ui_label = "Lines Blacks [Scanlines-Absolute]";
> = 0.0;

uniform float lines_white <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 2.0;
	ui_step = 0.05;
	ui_label = "Lines Whites [Scanlines-Absolute]";
> = 1.0;
 
#define freq             0.500000
#define offset           0.000000
#define pi               3.141592654

float4 PS_ScanlinesAbs(float4 pos : SV_POSITION, float2 tex : TEXCOORD0) : SV_TARGET 
{
    float3 color = tex2D(ReShade::BackBuffer, tex).xyz;
    float grid;
 
    float lines;
	
	float omega = 2.0 * pi * freq;  // Angular frequency
	float angle = tex.y * omega * texture_sizeY + phase;
 
    lines = sin(angle);
    lines *= amp;
    lines += offset;
    lines = abs(lines) * (lines_white - lines_black) + lines_black;
    color *= lines;
 
    return color.xyzz;
}

technique ScanlinesAbs {
	pass ScanlinesAbsolute{
		VertexShader = PostProcessVS;
		PixelShader = PS_ScanlinesAbs;
	}
}