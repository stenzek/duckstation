#include "ReShade.fxh"

/*
   Copyright (C) 2016 guest(r) - guest.r@gmail.com

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

uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;

sampler2D sBackBuffer{Texture=ReShade::BackBufferTex;AddressU=CLAMP;AddressV=CLAMP;AddressW=CLAMP;MagFilter=POINT;MinFilter=POINT;};

static const float3 dt = float3(1.0,1.0,1.0);

float3 texture2d(sampler2D tex, float2 coord, float4 yx) {

    float3 s00 = tex2D(tex, coord + yx.zw).xyz; 
    float3 s20 = tex2D(tex, coord + yx.xw).xyz; 
    float3 s22 = tex2D(tex, coord + yx.xy).xyz; 
    float3 s02 = tex2D(tex, coord + yx.zy).xyz; 

    float m1=dot(abs(s00-s22),dt)+0.001;
    float m2=dot(abs(s02-s20),dt)+0.001;

    return 0.5*(m2*(s00+s22)+m1*(s02+s20))/(m1+m2);
}



float4 PS_aa_shader_40(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD0) : SV_Target
{
    // Calculating texel coordinates
    float2 size     = 4.0 / NormalizedNativePixelSize;
    float2 inv_size = 1.0 / size;

    float4 yx = float4(inv_size, -inv_size);
    
    float2 OGL2Pos = vTexCoord * size;

    float2 fp = frac(OGL2Pos);
    float2 dx = float2(inv_size.x,0.0);
    float2 dy = float2(0.0, inv_size.y);
    float2 g1 = float2(inv_size.x,inv_size.y);
    float2 g2 = float2(-inv_size.x,inv_size.y);
    
    float2 pC4 = floor(OGL2Pos) * 1.0001 * inv_size;    
    
    // Reading the texels
    float3 C1 = texture2d(sBackBuffer, pC4 - dy, yx);
    float3 C0 = texture2d(sBackBuffer, pC4 - g1, yx); 
    float3 C2 = texture2d(sBackBuffer, pC4 - g2, yx);
    float3 C3 = texture2d(sBackBuffer, pC4 - dx, yx);
    float3 C4 = texture2d(sBackBuffer, pC4     , yx);
    float3 C5 = texture2d(sBackBuffer, pC4 + dx, yx);
    float3 C6 = texture2d(sBackBuffer, pC4 + g2, yx);
    float3 C7 = texture2d(sBackBuffer, pC4 + dy, yx);
    float3 C8 = texture2d(sBackBuffer, pC4 + g1, yx);
    
    float3 ul, ur, dl, dr;
    float m1, m2;
    
    m1 = dot(abs(C0-C4),dt)+0.001;
    m2 = dot(abs(C1-C3),dt)+0.001;
    ul = (m2*(C0+C4)+m1*(C1+C3))/(m1+m2);  
    
    m1 = dot(abs(C1-C5),dt)+0.001;
    m2 = dot(abs(C2-C4),dt)+0.001;
    ur = (m2*(C1+C5)+m1*(C2+C4))/(m1+m2);
    
    m1 = dot(abs(C3-C7),dt)+0.001;
    m2 = dot(abs(C6-C4),dt)+0.001;
    dl = (m2*(C3+C7)+m1*(C6+C4))/(m1+m2);
    
    m1 = dot(abs(C4-C8),dt)+0.001;
    m2 = dot(abs(C5-C7),dt)+0.001;
    dr = (m2*(C4+C8)+m1*(C5+C7))/(m1+m2);
    
    float3 c11 = 0.5*((dr*fp.x+dl*(1-fp.x))*fp.y+(ur*fp.x+ul*(1-fp.x))*(1-fp.y) );

    return float4(c11, 1.0);
}



technique aa_shader_40
{
   pass
   {
       VertexShader = PostProcessVS;
       PixelShader  = PS_aa_shader_40;
   }
}
