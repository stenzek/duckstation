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

#include "../include/user-settings.fxh"
#include "../include/derived-settings-and-constants.fxh"
#include "../include/bind-shader-params.fxh"

//////////////////////////////////  INCLUDES  //////////////////////////////////

#include "../include/gamma-management.fxh"
#include "../include/bloom-functions.fxh"
#include "../include/phosphor-mask-resizing.fxh"


/////////////////////////////////  STRUCTURES  /////////////////////////////////

struct out_vertex_p9
{
    float2 tex_uv               : TEXCOORD1;
    float2 bloom_dxdy           : TEXCOORD2;
    float bloom_sigma_runtime   : TEXCOORD3;
};


////////////////////////////////  VERTEX SHADER  ///////////////////////////////

// Vertex shader generating a triangle covering the entire screen
void VS_Bloom_Vertical(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out out_vertex_p9 OUT)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    float2 texture_size = BLOOM_VERTICAL_texture_size;
    float2 output_size  = VIEWPORT_SIZE;

    OUT.tex_uv = texcoord;

    //  Get the uv sample distance between output pixels.  Calculate dxdy like
    //  blurs/vertex-shader-blur-fast-vertical.h.
    const float2 dxdy_scale = video_size/output_size;
    const float2 dxdy = dxdy_scale/texture_size;
    //  This blur is vertical-only, so zero out the vertical offset:
    OUT.bloom_dxdy = float2(0.0, dxdy.y);

    //  Calculate a runtime bloom_sigma in case it's needed:
    const float mask_tile_size_x = get_resized_mask_tile_size(
        output_size, output_size * mask_resize_viewport_scale, false).x;
    OUT.bloom_sigma_runtime = get_min_sigma_to_blur_triad(
        mask_tile_size_x / mask_triads_per_tile, bloom_diff_thresh);
}


///////////////////////////////  FRAGMENT SHADER  //////////////////////////////

float4 PS_Bloom_Vertical(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in out_vertex_p9 VAR) : SV_Target
{
    //  Blur the brightpass horizontally with a 9/17/25/43x blur:
    const float bloom_sigma = get_final_bloom_sigma(VAR.bloom_sigma_runtime);
    const float3 color = tex2DblurNfast(BRIGHTPASS, VAR.tex_uv,
        VAR.bloom_dxdy, bloom_sigma);
    //  Encode and output the blurred image:
    return encode_output(float4(color, 1.0));
}

