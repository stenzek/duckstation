#include "ReShade.fxh"

//  CrashGG presents

//  'XY-Pos-free' 

//  A super-simple shader refined from the super-fast crt-cyclon.fx, It only provides
//  the functions of free pixel stretching and position translation on the XY axis.
//  Suitable for users who only want to fine-tune the screen zoom and position and do not like the bundled CRT-like effects.
//  Fixed some bugs in the original version, adjusted the step progress and the range.

//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU General Public License as published by the Free
//  Software Foundation; either version 2 of the License, or (at your option)
//  any later version.


uniform float zoomx <
	ui_type = "drag";
	ui_min = -0.3000;
	ui_max = 0.3000;
	ui_step = 0.0005;
	ui_label = "Zoom Image X";
> = 0.0000;

uniform float zoomy <
	ui_type = "drag";
	ui_min = -0.3000;
	ui_max = 0.3000;
	ui_step = 0.0005;
	ui_label = "Zoom Image Y";
> = 0.0000;

uniform float centerx <
	ui_type = "drag";
	ui_min = -9.99;
	ui_max = 9.99;
	ui_step = 0.01;
	ui_label = "Image Center X";
> = 0.00;

uniform float centery <
	ui_type = "drag";
	ui_min = -9.99;
	ui_max = 9.99;
	ui_step = 0.01;
	ui_label = "Image Center Y";
> = 0.00;


float2 Warp(float2 pos)
{
    pos = pos*2.0-1.0;
    pos *= float2(1.0+pos.y*pos.y*0, 1.0+pos.x*pos.x*0);
    pos = pos*0.5+0.5;

    return pos;
}


float4 CRT_CYCLON_PS(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD0) : SV_Target
{
// zoom in and center screen
    float2 pos = Warp((vTexCoord*float2(1.0-zoomx,1.0-zoomy)-float2(centerx,centery)/100.0));

// Convergence
    float3 res = tex2D(ReShade::BackBuffer,pos).rgb;

// Vignette
    float x = 0.0;
	
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
