/////////////////////////////  GPL LICENSE NOTICE  /////////////////////////////

//  crt-royale: A full-featured CRT shader, with cheese.
//  Copyright (C) 2014 TroggleMonkey <trogglemonkey@gmx.com>
//
//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU General Public License as published by the Free
//  Software Foundation; either version 2 of the License, or any later version.
//
//  This program is distributed in the hope that it will be useful, but WITHOUT
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
//  more details.
//
//  You should have received a copy of the GNU General Public License along with
//  this program; if not, write to the Free Software Foundation, Inc., 59 Temple
//  Place, Suite 330, Boston, MA 02111-1307 USA


/////////////////////////////  SETTINGS MANAGEMENT  ////////////////////////////

//  PASS SETTINGS:
//  gamma-management.h needs to know what kind of pipeline we're using and
//  what pass this is in that pipeline.  This will become obsolete if/when we
//  can #define things like this in the .cgp preset file.
#define FIRST_PASS
#define SIMULATE_CRT_ON_LCD


//////////////////////////////////  INCLUDES  //////////////////////////////////

#include "../include/user-settings.fxh"
#include "../include/bind-shader-params.fxh"
#include "../include/gamma-management.fxh"
#include "../include/scanline-functions.fxh"


/////////////////////////////////  STRUCTURES  /////////////////////////////////

struct out_vertex
{
    float2 tex_uv           : TEXCOORD1;
    float2 uv_step          : TEXCOORD2;
    float interlaced        : TEXCOORD3;
};

////////////////////////////////  VERTEX SHADER  ///////////////////////////////

// Vertex shader generating a triangle covering the entire screen
void VS_Linearize(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out out_vertex OUT)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    OUT.tex_uv = texcoord;
//    OUT.tex_uv = (floor(texcoord / NormalizedNativePixelSize)+float2(0.5,0.5)) * NormalizedNativePixelSize;
    //  Save the uv distance between texels:
    OUT.uv_step = NormalizedNativePixelSize;

    //  Detect interlacing: 1.0 = true, 0.0 = false.
    OUT.interlaced = is_interlaced(1.0/NormalizedNativePixelSize.y);
}


///////////////////////////////  FRAGMENT SHADER  //////////////////////////////

sampler2D sBackBuffer{Texture=ReShade::BackBufferTex;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=POINT;MinFilter=POINT;};

#define input_texture sBackBuffer

float4 PS_Linearize(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in out_vertex VAR) : SV_Target
{
    //  Linearize the input based on CRT gamma and bob interlaced fields.
    //  Bobbing ensures we can immediately blur without getting artifacts.
    //  Note: TFF/BFF won't matter for sources that double-weave or similar.
   // VAR.tex_uv = (floor(VAR.tex_uv / NormalizedNativePixelSize)+float2(0.5,0.5)) * NormalizedNativePixelSize;

    if(interlace_detect)
    {
        //  Sample the current line and an average of the previous/next line;
        //  tex2D_linearize will decode CRT gamma.  Don't bother branching:
        const float2 tex_uv = VAR.tex_uv;
        const float2 v_step = float2(0.0, VAR.uv_step.y);
        const float3 curr_line = tex2D_linearize_first(
            input_texture, tex_uv).rgb;
        const float3 last_line = tex2D_linearize_first(
            input_texture, tex_uv - v_step).rgb;
        const float3 next_line = tex2D_linearize_first(
            input_texture, tex_uv + v_step).rgb;
        const float3 interpolated_line = 0.5 * (last_line + next_line);
        //  If we're interlacing, determine which field curr_line is in:
        const float modulus = VAR.interlaced + 1.0;
        const float field_offset =
            fmod(FrameCount + float(interlace_bff), modulus);
        const float curr_line_texel = tex_uv.y / NormalizedNativePixelSize.y;
        //  Use under_half to fix a rounding bug around exact texel locations.
        const float line_num_last = floor(curr_line_texel - under_half);
        const float wrong_field = fmod(line_num_last + field_offset, modulus);
        //  Select the correct color, and output the result:
        const float3 color = lerp(curr_line, interpolated_line, wrong_field);
        return encode_output(float4(color, 1.0));
    }
    else
    {
        return encode_output(tex2D_linearize_first(input_texture, VAR.tex_uv));
    }
}

