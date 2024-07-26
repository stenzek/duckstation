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

#include "../include/phosphor-mask-resizing.fxh"


/////////////////////////////////  STRUCTURES  /////////////////////////////////

struct out_vertex_p6
{
    float2 src_tex_uv_wrap              : TEXCOORD1;
    float2 tile_uv_wrap                 : TEXCOORD2;
    float2 resize_magnification_scale   : TEXCOORD3;
    float2 src_dxdy                     : TEXCOORD4;
    float2 tile_size_uv                 : TEXCOORD5;
    float2 input_tiles_per_texture      : TEXCOORD6;
};


////////////////////////////////  VERTEX SHADER  ///////////////////////////////

// Vertex shader generating a triangle covering the entire screen
void VS_Mask_Resize_Horizontal(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out out_vertex_p6 OUT)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    float2 tex_uv = texcoord;

    float2 texture_size = MASK_RESIZE_texture_size;
    float2 output_size  = 0.0625*(VIEWPORT_SIZE);

    //  First estimate the viewport size (the user will get the wrong number of
    //  triads if it's wrong and mask_specify_num_triads is 1.0/true).
    const float2 estimated_viewport_size =
        output_size / mask_resize_viewport_scale;
    //  Find the final size of our resized phosphor mask tiles.  We probably
    //  estimated the viewport size and MASK_RESIZE output size differently last
    //  pass, so do not swear they were the same. ;)
    const float2 mask_resize_tile_size = get_resized_mask_tile_size(
        estimated_viewport_size, output_size, false);

    //  We'll render resized tiles until filling the output FBO or meeting a
    //  limit, so compute [wrapped] tile uv coords based on the output uv coords
    //  and the number of tiles that will fit in the FBO.
    const float2 output_tiles_this_pass = output_size / mask_resize_tile_size;
    const float2 output_video_uv = tex_uv * texture_size / video_size;
    const float2 tile_uv_wrap = output_video_uv * output_tiles_this_pass;

    //  Get the texel size of an input tile and related values:
    const float2 input_tile_size = float2(min(
        mask_resize_src_lut_size.x, video_size.x), mask_resize_tile_size.y);
    const float2 tile_size_uv = input_tile_size / texture_size;
    const float2 input_tiles_per_texture = texture_size / input_tile_size;

    //  Derive [wrapped] texture uv coords from [wrapped] tile uv coords and
    //  the tile size in uv coords, and save frac() for the fragment shader.
    const float2 src_tex_uv_wrap = tile_uv_wrap * tile_size_uv;

    //  Output the values we need, including the magnification scale and step:
    OUT.tile_uv_wrap = tile_uv_wrap;
    OUT.src_tex_uv_wrap = src_tex_uv_wrap;
    OUT.resize_magnification_scale = mask_resize_tile_size / input_tile_size;
    OUT.src_dxdy = float2(1.0/texture_size.x, 0.0);
    OUT.tile_size_uv = tile_size_uv;
    OUT.input_tiles_per_texture = input_tiles_per_texture;
}


///////////////////////////////  FRAGMENT SHADER  //////////////////////////////

float4 PS_Mask_Resize_Horizontal(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in out_vertex_p6 VAR) : SV_Target
{
    //  The input contains one mask tile horizontally and a number vertically.
    //  Resize the tile horizontally to its final screen size and repeat it
    //  until drawing at least mask_resize_num_tiles, leaving it unchanged
    //  vertically.  Lanczos-resizing the phosphor mask achieves much sharper
    //  results than mipmapping, outputting >= mask_resize_num_tiles makes for
    //  easier tiled sampling later.
    #ifdef PHOSPHOR_MASK_MANUALLY_RESIZE
        //  Discard unneeded fragments in case our profile allows real branches.
        float2 texture_size = MASK_RESIZE_texture_size;
        const float2 tile_uv_wrap = VAR.tile_uv_wrap;
        if(get_mask_sample_mode() < 0.5 &&
            max(tile_uv_wrap.x, tile_uv_wrap.y) <= mask_resize_num_tiles)
        {
            const float src_dx = VAR.src_dxdy.x;
            const float2 src_tex_uv = frac(VAR.src_tex_uv_wrap);
            const float3 pixel_color = downsample_horizontal_sinc_tiled(MASK_RESIZE_VERTICAL,
                src_tex_uv, texture_size, VAR.src_dxdy.x,
                VAR.resize_magnification_scale.x, VAR.tile_size_uv.x);
            //  The input LUT was linear RGB, and so is our output:
            return float4(pixel_color, 1.0);
        }
        else
        {
            discard;
        }
    #else
        discard;
        return 1.0.xxxx;
    #endif
}

