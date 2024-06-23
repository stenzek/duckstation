#include "ReShade.fxh"

/*
    Deblur-Luma Shader
    
    Copyright (C) 2005 - 2024 guest(r) - guest.r@gmail.com

    Luma adaptation by Hyllian 	

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

uniform float OFFSET <
    ui_type = "drag";
    ui_min = 0.25;
    ui_max = 4.0;
    ui_step = 0.25;
    ui_label = "Deblur offset";
> = 2.0;

uniform float DEBLUR <
    ui_type = "drag";
    ui_min = 1.0;
    ui_max = 7.0;
    ui_step = 0.25;
    ui_label = "Deblur str.";
> = 1.75;

uniform float SMART <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.05;
    ui_label = "Smart deblur";
> = 1.0;

uniform float2 ViewportSize < source = "viewportsize"; >;


static const float3 luma = float3(0.299,0.587,0.114);
static const float4  res = float4(0.0001, 0.0001, 0.0001, 0.0001);
static const float4  uno = float4(1.,1.,1.,1.);


float min8(float4 a4, float4 b4)
{
     float4 ab4 = min(a4, b4); float2 ab2 = min(ab4.xy, ab4.zw); return min(ab2.x, ab2.y);
}

float max8(float4 a4, float4 b4)
{
     float4 ab4 = max(a4, b4); float2 ab2 = max(ab4.xy, ab4.zw); return max(ab2.x, ab2.y);
}


struct ST_VertexOut
{
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
};


// Vertex shader generating a triangle covering the entire screen
void VS_Deblur_Luma(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out ST_VertexOut vVARS)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    float dx = OFFSET/ViewportSize.x;
    float dy = OFFSET/ViewportSize.y;

    vVARS.t1 = texcoord.xxxy + float4( -dx, 0.0, dx, -dy); //  c00 c10 c20
    vVARS.t2 = texcoord.xxxy + float4( -dx, 0.0, dx, 0.0); //  c01 c11 c21
    vVARS.t3 = texcoord.xxxy + float4( -dx, 0.0, dx,  dy); //  c02 c12 c22
}


float4 PS_Deblur_Luma(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in ST_VertexOut vVARS) : SV_Target
{

    float3 c11 = tex2D(ReShade::BackBuffer, vVARS.t2.yw).xyz;  
    float3 c00 = tex2D(ReShade::BackBuffer, vVARS.t1.xw).xyz;
    float3 c20 = tex2D(ReShade::BackBuffer, vVARS.t1.zw).xyz;
    float3 c22 = tex2D(ReShade::BackBuffer, vVARS.t3.zw).xyz;
    float3 c02 = tex2D(ReShade::BackBuffer, vVARS.t3.xw).xyz;
    float3 c10 = tex2D(ReShade::BackBuffer, vVARS.t1.yw).xyz;     
    float3 c21 = tex2D(ReShade::BackBuffer, vVARS.t2.zw).xyz;
    float3 c12 = tex2D(ReShade::BackBuffer, vVARS.t3.yw).xyz;
    float3 c01 = tex2D(ReShade::BackBuffer, vVARS.t2.xw).xyz;

    float4x3 chv = float4x3(c10, c01, c21, c12);
    float4x3 cdi = float4x3(c00, c02, c20, c22);

    float4  CHV = mul(chv, luma);
    float4  CDI = mul(cdi, luma);
    float C11 = dot(c11, luma);

    float mn1 = min8(CHV, CDI);
    float mx1 = max8(CHV, CDI);
 
    float2 mnmx = float2(min(C11, mn1), max(C11, mx1));

    float2 dif = abs(float2(C11, C11) - mnmx) + res.xy;
    
    dif = pow(dif, float2(DEBLUR, DEBLUR));

    float D11 = dot(dif, mnmx.yx)/(dif.x + dif.y);   

    float k11 = 1.0/(abs(C11 - D11) + res.x);  

    float4 khv = float4(1.0/(abs(CHV-float4(D11, D11, D11, D11)) + res));
    float4 kdi = float4(1.0/(abs(CDI-float4(D11, D11, D11, D11)) + res));

    float avg = (dot(khv + kdi, uno) + k11)/10.0;
    
    khv = max(khv-float4(avg, avg, avg, avg), float4(0.0, 0.0, 0.0, 0.0));
    kdi = max(kdi-float4(avg, avg, avg, avg), float4(0.0, 0.0, 0.0, 0.0));
    k11 = max(k11-avg, 0.0);

    float3 d11 = (mul(khv, chv) + mul(kdi, cdi) + (k11 + res.x)*c11) / (dot(khv + kdi, uno) + k11 + res.x);

    float contrast = mnmx.y - mnmx.x;
    c11 = lerp(c11, d11, clamp(1.75*contrast-0.125, 0.0, 1.0));
    c11 = lerp(d11, c11, SMART);   
    
    return float4(c11, 1.0);
}


technique Deblur_Luma
{
    pass
    {
    	VertexShader = VS_Deblur_Luma;
    	PixelShader  = PS_Deblur_Luma;
    }
}
