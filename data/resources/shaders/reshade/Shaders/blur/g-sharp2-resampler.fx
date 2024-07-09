#include "ReShade.fxh"


/*
   G-sharp resampler 2.0 - dynamic range (upscaler, downsampler)
   
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



uniform float GSHARP0 <
	ui_type = "drag";
	ui_min = 0.75;
	ui_max = 8.0;
	ui_step = 0.05;
	ui_label = "Filter Range";
> = 2.45;

uniform float GBOOST <
	ui_type = "drag";
	ui_min = 1.0;
	ui_max = 2.5;
	ui_step = 0.05;
	ui_label = "Filter Boost (same range, speedup)";
> = 1.75;

uniform float GMAXSHARP <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 0.25;
	ui_step = 0.01;
	ui_label = "Filter Sharpness";
> = 0.1;

uniform float GPAR <
	ui_type = "drag";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.10;
	ui_label = "Anti-Ringing";
> = 0.50;


uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;
uniform float2 NormalizedInternalPixelSize < source = "normalized_internal_pixel_size"; >;

sampler2D sBackBuffer{Texture=ReShade::BackBufferTex;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=POINT;MinFilter=POINT;};

#define GMAXSHARP (0.25*GBOOST*GBOOST*GMAXSHARP)

float smothstep(float x)
{
	return exp(-2.33*x*x);
}

float getw(float x)
{
	float z = x/GBOOST;
	float y = smothstep(z);
	return max(y*y - GMAXSHARP, lerp(-GMAXSHARP, 0.0, x-1.0));
}


float4 PS_GSHARP2(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD0) : SV_Target
{
    float2 texCoord = vTexCoord;
    float4 SourceSize = float4(1.0 / NormalizedInternalPixelSize, NormalizedInternalPixelSize);

	float2 pos = vTexCoord * SourceSize.xy-0.5;
	float2 f =  -frac(pos);
	float2 tex = floor(pos)*SourceSize.zw + 0.5*SourceSize.zw;
	float3 color = 0.0.xxx;
	float2 dx  = float2(SourceSize.z, 0.0);
	float2 dy  = float2(0.0, SourceSize.w);
	
	float w, fp;
	float wsum = 0.0;
	float3 pixel;
	float3 cmax = 0.0.xxx;
	float3 cmin = 1.0.xxx;
	float FPR = GSHARP0;
	float FPR2 = 2.0*FPR;
	float FPR3 = FPR2*FPR2;
	float LOOPSIZE = ceil(FPR2);	
	float y = -LOOPSIZE+1.0;
	float x = 0.0;
	
	do
	{
		x = -LOOPSIZE + 1.0;
	
		do
		{
			fp = dot(float2(x+f.x,y+f.y),float2(x+f.x,y+f.y));
			if (fp >= FPR3) w = 0.0;
			else
			{
				pixel  = tex2D(sBackBuffer, tex + x*dx + y*dy).rgb;		
				fp = sqrt(fp)/FPR;
				w = getw(fp);				
				if (w >= 0.0) { cmin = min(cmin, pixel); cmax = max(cmax, pixel); }
				color = color + w * pixel;
				wsum   = wsum + w;
			}
			x = x + 1.0;
			
		} while (x <= LOOPSIZE);
		
		y = y + 1.0;
		
	} while (y <= LOOPSIZE);

	color = color / wsum;

	color = lerp(clamp(color, 0.0, 1.0), clamp(color, cmin, cmax), GPAR);
	
	return float4(color, 1.0);
}



technique GSHARP2
{
   pass
   {
   	VertexShader = PostProcessVS;
   	PixelShader  = PS_GSHARP2;
   }
}
