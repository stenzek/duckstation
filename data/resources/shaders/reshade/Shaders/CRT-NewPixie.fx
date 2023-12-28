#include "ReShade.fxh"

// newpixie CRT
// by Mattias Gustavsson
// adapted for slang by hunterk

/*
------------------------------------------------------------------------------
This software is available under 2 licenses - you may choose the one you like.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2016 Mattias Gustavsson
Permission is hereby granted, free of charge, to any person obtaining a copy of 
this software and associated documentation files (the "Software"), to deal in 
the Software without restriction, including without limitation the rights to 
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies 
of the Software, and to permit persons to whom the Software is furnished to do 
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all 
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this 
software, either in source code form or as a compiled binary, for any purpose, 
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this 
software dedicate any and all copyright interest in the software to the public 
domain. We make this dedication for the benefit of the public at large and to 
the detriment of our heirs and successors. We intend this dedication to be an 
overt act of relinquishment in perpetuity of all present and future rights to 
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN 
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION 
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
*/

uniform float acc_modulate <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.01;
	ui_label = "Accumulate Modulation [CRT-NewPixie]";
> = 0.65;


texture2D tAccTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler sAccTex { Texture=tAccTex; };

//PASS 1
float3 PrevColor(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	return tex2D(ReShade::BackBuffer, uv).rgb;
}

float4 PS_NewPixie_Accum(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
   float4 a = tex2D(sAccTex, uv.xy) * float4(acc_modulate,acc_modulate,acc_modulate,acc_modulate);
   float4 b = tex2D(ReShade::BackBuffer, uv.xy);
   return max( a, b * 0.96 );
}

//PASS 2 AND 3
texture GaussianBlurTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler GaussianBlurSampler { Texture = GaussianBlurTex;};

uniform float blur_x <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 5.0;
	ui_step = 0.25;
	ui_label = "Horizontal Blur [CRT-NewPixie]"; 
> = 1.0;

uniform float blur_y <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 5.0;
	ui_step = 0.25;
	ui_label = "Vertical Blur [CRT-NewPixie]";
> = 1.0;

float4 PS_NewPixie_Blur(float4 pos : SV_Position, float2 uv_tx : TEXCOORD0) : SV_Target
{
   float2 blur = float2(blur_x, blur_y) * ReShade::PixelSize.xy;
   float2 uv = uv_tx.xy;
   float4 sum = tex2D( ReShade::BackBuffer, uv ) * 0.2270270270;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x - 4.0 * blur.x, uv.y - 4.0 * blur.y ) ) * 0.0162162162;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x - 3.0 * blur.x, uv.y - 3.0 * blur.y ) ) * 0.0540540541;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x - 2.0 * blur.x, uv.y - 2.0 * blur.y ) ) * 0.1216216216;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x - 1.0 * blur.x, uv.y - 1.0 * blur.y ) ) * 0.1945945946;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x + 1.0 * blur.x, uv.y + 1.0 * blur.y ) ) * 0.1945945946;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x + 2.0 * blur.x, uv.y + 2.0 * blur.y ) ) * 0.1216216216;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x + 3.0 * blur.x, uv.y + 3.0 * blur.y ) ) * 0.0540540541;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x + 4.0 * blur.x, uv.y + 4.0 * blur.y ) ) * 0.0162162162;
   return sum;
}

//PASS 4
uniform int FCount < source = "framecount"; >;
texture tFrame < source = "crt-newpixie/crtframe.png"; >
{
	Width = 1024;
	Height = 1024;
	MipLevels = 1;
};

sampler sFrame { Texture = tFrame; AddressU = BORDER; AddressV = BORDER; MinFilter = LINEAR; MagFilter = LINEAR;};

uniform bool use_frame <
	ui_type = "boolean";
	ui_label = "Use Frame Image [CRT-NewPixie]";
> = false;

uniform float curvature <
	ui_type = "drag";
	ui_min = 0.0001;
	ui_max = 4.0;
	ui_step = 0.25;
	ui_label = "Curvature [CRT-NewPixie]";
> = 2.0;

uniform bool wiggle_toggle <
	ui_type = "boolean";
	ui_label = "Interference [CRT-NewPixie]";
> = false;

uniform bool scanroll <
	ui_type = "boolean";
	ui_label = "Rolling Scanlines [CRT-NewPixie]";
> = true;

float3 tsample( sampler samp, float2 tc, float offs, float2 resolution )
{
	tc = tc * float2(1.025, 0.92) + float2(-0.0125, 0.04);
	float3 s = pow( abs( tex2D( samp, float2( tc.x, 1.0-tc.y ) ).rgb), float3( 2.2,2.2,2.2 ) );
	return s*float3(1.25,1.25,1.25);
}
		
float3 filmic( float3 LinearColor )
{
	float3 x = max( float3(0.0,0.0,0.0), LinearColor-float3(0.004,0.004,0.004));
    return (x*(6.2*x+0.5))/(x*(6.2*x+1.7)+0.06);
}
		
float2 curve( float2 uv )
{
    uv = (uv - 0.5);// * 2.0;
    uv *= float2(0.925, 1.095);  
    uv *= curvature;
    uv.x *= 1.0 + pow((abs(uv.y) / 4.0), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 3.0), 2.0);
    uv /= curvature;
    uv  += 0.5;
    uv =  uv *0.92 + 0.04;
    return uv;
}
		
float rand(float2 co){ return frac(sin(dot(co.xy ,float2(12.9898,78.233))) * 43758.5453); }
    
#define resolution ReShade::ScreenSize.xy
#define mod(x,y) (x-y*floor(x/y))

float4 PS_NewPixie_Final(float4 pos: SV_Position, float2 uv_tx : TEXCOORD0) : SV_Target
{
   // stop time variable so the screen doesn't wiggle
   float time = mod(FCount, 849.0) * 36.0;
    float2 uv = uv_tx.xy;
	uv.y = 1.0 - uv_tx.y;
    /* Curve */
    float2 curved_uv = lerp( curve( uv ), uv, 0.4 );
    float scale = -0.101;
    float2 scuv = curved_uv*(1.0-scale)+scale/2.0+float2(0.003, -0.001);
	
    uv = scuv;
		
    /* Main color, Bleed */
    float3 col;
	
    float x = wiggle_toggle* sin(0.1*time+curved_uv.y*13.0)*sin(0.23*time+curved_uv.y*19.0)*sin(0.3+0.11*time+curved_uv.y*23.0)*0.0012;
    float o =sin(uv_tx.y*1.5)/resolution.x;
    x+=o*0.25;
   // make time do something again
    time = float(mod(FCount, 640) * 1); 
    col.r = tsample(ReShade::BackBuffer,float2(x+scuv.x+0.0009,scuv.y+0.0009),resolution.y/800.0, resolution ).x+0.02;
    col.g = tsample(ReShade::BackBuffer,float2(x+scuv.x+0.0000,scuv.y-0.0011),resolution.y/800.0, resolution ).y+0.02;
    col.b = tsample(ReShade::BackBuffer,float2(x+scuv.x-0.0015,scuv.y+0.0000),resolution.y/800.0, resolution ).z+0.02;
    float i = clamp(col.r*0.299 + col.g*0.587 + col.b*0.114, 0.0, 1.0 );
    i = pow( 1.0 - pow(i,2.0), 1.0 );
    i = (1.0-i) * 0.85 + 0.15; 
    
    /* Ghosting */
    float ghs = 0.15;
    float3 r = tsample(GaussianBlurSampler, float2(x-0.014*1.0, -0.027)*0.85+0.007*float2( 0.35*sin(1.0/7.0 + 15.0*curved_uv.y + 0.9*time), 
        0.35*sin( 2.0/7.0 + 10.0*curved_uv.y + 1.37*time) )+float2(scuv.x+0.001,scuv.y+0.001),
        5.5+1.3*sin( 3.0/9.0 + 31.0*curved_uv.x + 1.70*time),resolution).xyz*float3(0.5,0.25,0.25);
    float3 g = tsample(GaussianBlurSampler, float2(x-0.019*1.0, -0.020)*0.85+0.007*float2( 0.35*cos(1.0/9.0 + 15.0*curved_uv.y + 0.5*time), 
        0.35*sin( 2.0/9.0 + 10.0*curved_uv.y + 1.50*time) )+float2(scuv.x+0.000,scuv.y-0.002),
        5.4+1.3*sin( 3.0/3.0 + 71.0*curved_uv.x + 1.90*time),resolution).xyz*float3(0.25,0.5,0.25);
    float3 b = tsample(GaussianBlurSampler, float2(x-0.017*1.0, -0.003)*0.85+0.007*float2( 0.35*sin(2.0/3.0 + 15.0*curved_uv.y + 0.7*time), 
        0.35*cos( 2.0/3.0 + 10.0*curved_uv.y + 1.63*time) )+float2(scuv.x-0.002,scuv.y+0.000),
        5.3+1.3*sin( 3.0/7.0 + 91.0*curved_uv.x + 1.65*time),resolution).xyz*float3(0.25,0.25,0.5);
		
    col += float3(ghs*(1.0-0.299),ghs*(1.0-0.299),ghs*(1.0-0.299))*pow(clamp(float3(3.0,3.0,3.0)*r,float3(0.0,0.0,0.0),float3(1.0,1.0,1.0)),float3(2.0,2.0,2.0))*float3(i,i,i);
    col += float3(ghs*(1.0-0.587),ghs*(1.0-0.587),ghs*(1.0-0.587))*pow(clamp(float3(3.0,3.0,3.0)*g,float3(0.0,0.0,0.0),float3(1.0,1.0,1.0)),float3(2.0,2.0,2.0))*float3(i,i,i);
    col += float3(ghs*(1.0-0.114),ghs*(1.0-0.114),ghs*(1.0-0.114))*pow(clamp(float3(3.0,3.0,3.0)*b,float3(0.0,0.0,0.0),float3(1.0,1.0,1.0)),float3(2.0,2.0,2.0))*float3(i,i,i);
		
    /* Level adjustment (curves) */
    col *= float3(0.95,1.05,0.95);
    col = clamp(col*1.3 + 0.75*col*col + 1.25*col*col*col*col*col,float3(0.0,0.0,0.0),float3(10.0,10.0,10.0));
		
    /* Vignette */
    float vig = (0.1 + 1.0*16.0*curved_uv.x*curved_uv.y*(1.0-curved_uv.x)*(1.0-curved_uv.y));
    vig = 1.3*pow(vig,0.5);
    col *= vig;
    
    time *= scanroll;
		
    /* Scanlines */
    float scans = clamp( 0.35+0.18*sin(6.0*time-curved_uv.y*resolution.y*1.5), 0.0, 1.0);
    float s = pow(scans,0.9);
    col = col * float3(s,s,s);
		
    /* Vertical lines (shadow mask) */
    col*=1.0-0.23*(clamp((mod(uv_tx.xy.x, 3.0))/2.0,0.0,1.0));
		
    /* Tone map */
    col = filmic( col );
		
    /* Noise */
    /*float2 seed = floor(curved_uv*resolution.xy*float2(0.5))/resolution.xy;*/
    float2 seed = curved_uv*resolution.xy;;
    /* seed = curved_uv; */
    col -= 0.015*pow(float3(rand( seed +time ), rand( seed +time*2.0 ), rand( seed +time * 3.0 ) ), float3(1.5,1.5,1.5) );
		
    /* Flicker */
    col *= (1.0-0.004*(sin(50.0*time+curved_uv.y*2.0)*0.5+0.5));
		
    /* Clamp */
//    if(max(abs(uv.x-0.5),abs(uv.y-0.5))>0.5)
//        col = float3(0.0,0.0,0.0);
		
//    if (curved_uv.x < 0.0 || curved_uv.x > 1.0)
//        col *= 0.0;
//    if (curved_uv.y < 0.0 || curved_uv.y > 1.0)
//        col *= 0.0;

    uv = curved_uv;
	
    /* Frame */
    float2 fscale = float2( 0.026, -0.018);//float2( -0.018, -0.013 );
    uv = float2(uv.x, 1.-uv.y);
    float4 f= tex2D(sFrame,uv_tx.xy);//*((1.0)+2.0*fscale)-fscale-float2(-0.0, 0.005));
    f.xyz = lerp( f.xyz, float3(0.5,0.5,0.5), 0.5 );
    float fvig = clamp( -0.00+512.0*uv.x*uv.y*(1.0-uv.x)*(1.0-uv.y), 0.2, 0.8 );
    col = lerp( col, lerp( max( col, 0.0), pow( abs( f.xyz ), float3( 1.4,1.4,1.4 ) ) * fvig, f.w * f.w), float3( use_frame,use_frame,use_frame ) );
	
    return float4( col, 1.0 );
}

technique CRTNewPixie
{
	pass PS_CRTNewPixie_Accum
	{
		VertexShader=PostProcessVS;
		PixelShader=PS_NewPixie_Accum;
	}
	
	pass PS_CRTNewPixie_SaveBuffer {
		VertexShader=PostProcessVS;
		PixelShader=PrevColor;
		RenderTarget = tAccTex;
	}
	
	pass PS_CRTNewPixie_Blur
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_NewPixie_Blur;
		RenderTarget = GaussianBlurTex;
	}
	
	pass PS_CRTNewPixie_Final
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_NewPixie_Final;
	}
}
