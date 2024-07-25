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

struct out_vertex_p5
{
    float2 src_tex_uv_wrap              : TEXCOORD1;
    float2 resize_magnification_scale   : TEXCOORD2;
};


////////////////////////////////  VERTEX SHADER  ///////////////////////////////

// Vertex shader generating a triangle covering the entire screen
void VS_Mask_Resize_Vertical(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out out_vertex_p5 OUT)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    float2 tex_uv = texcoord;

    float2 texture_size = MASK_RESIZE_VERT_texture_size;
    float2 output_size  = float2(64.0, 0.0625*((VIEWPORT_SIZE).y));

    //  First estimate the viewport size (the user will get the wrong number of
    //  triads if it's wrong and mask_specify_num_triads is 1.0/true).
    const float viewport_y = output_size.y / mask_resize_viewport_scale.y;
//  Now get aspect_ratio from texture_size. 
//    const float aspect_ratio = geom_aspect_ratio_x / geom_aspect_ratio_y;
    const float aspect_ratio = texture_size.x / texture_size.y;
    const float2 estimated_viewport_size =
        float2(viewport_y * aspect_ratio, viewport_y);
    //  Estimate the output size of MASK_RESIZE (the next pass).  The estimated
    //  x component shouldn't matter, because we're not using the x result, and
    //  we're not swearing it's correct (if we did, the x result would influence
    //  the y result to maintain the tile aspect ratio).
    const float2 estimated_mask_resize_output_size =
        float2(output_size.y * aspect_ratio, output_size.y);
    //  Find the final intended [y] size of our resized phosphor mask tiles,
    //  then the tile size for the current pass (resize y only):
    const float2 mask_resize_tile_size = get_resized_mask_tile_size(
        estimated_viewport_size, estimated_mask_resize_output_size, false);
    const float2 pass_output_tile_size = float2(min(
        mask_resize_src_lut_size.x, output_size.x), mask_resize_tile_size.y);

    //  We'll render resized tiles until filling the output FBO or meeting a
    //  limit, so compute [wrapped] tile uv coords based on the output uv coords
    //  and the number of tiles that will fit in the FBO.
    const float2 output_tiles_this_pass = output_size / pass_output_tile_size;
    const float2 output_video_uv = tex_uv * texture_size / video_size;
    const float2 tile_uv_wrap = output_video_uv * output_tiles_this_pass;

    //  The input LUT is just a single mask tile, so texture uv coords are the
    //  same as tile uv coords (save frac() for the fragment shader).  The
    //  magnification scale is also straightforward:
    OUT.src_tex_uv_wrap = tile_uv_wrap;
    OUT.resize_magnification_scale =
        pass_output_tile_size / mask_resize_src_lut_size;
}


///////////////////////////////  FRAGMENT SHADER  //////////////////////////////

float4 PS_Mask_Resize_Vertical(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in out_vertex_p5 VAR) : SV_Target
{
    //  Resize the input phosphor mask tile to the final vertical size it will
    //  appear on screen.  Keep 1x horizontal size if possible (IN.output_size
    //  >= mask_resize_src_lut_size), and otherwise linearly sample horizontally
    //  to fit exactly one tile.  Lanczos-resizing the phosphor mask achieves
    //  much sharper results than mipmapping, and vertically resizing first
    //  minimizes the total number of taps required.  We output a number of
    //  resized tiles >= mask_resize_num_tiles for easier tiled sampling later.
    #ifdef PHOSPHOR_MASK_MANUALLY_RESIZE
        //  Discard unneeded fragments in case our profile allows real branches.
        const float2 tile_uv_wrap = VAR.src_tex_uv_wrap;
        if(get_mask_sample_mode() < 0.5 &&
            tile_uv_wrap.y <= mask_resize_num_tiles)
        {
            static const float src_dy = 1.0/mask_resize_src_lut_size.y;
            const float2 src_tex_uv = frac(VAR.src_tex_uv_wrap);
            float3 pixel_color;
            //  If mask_type is static, this branch will be resolved statically.
			#ifdef PHOSPHOR_MASK_RESIZE_MIPMAPPED_LUT
				if(mask_type < 0.5)
				{
					pixel_color = downsample_vertical_sinc_tiled(
						mask_grille_texture_large, src_tex_uv, mask_resize_src_lut_size,
						src_dy, VAR.resize_magnification_scale.y, 1.0);
				}
				else if(mask_type < 1.5)
				{
					pixel_color = downsample_vertical_sinc_tiled(
						mask_slot_texture_large, src_tex_uv, mask_resize_src_lut_size,
						src_dy, VAR.resize_magnification_scale.y, 1.0);
				}
				else
				{
					pixel_color = downsample_vertical_sinc_tiled(
						mask_shadow_texture_large, src_tex_uv, mask_resize_src_lut_size,
						src_dy, VAR.resize_magnification_scale.y, 1.0);
				}
			#else
				if(mask_type < 0.5)
				{
					pixel_color = downsample_vertical_sinc_tiled(
						mask_grille_texture_small, src_tex_uv, mask_resize_src_lut_size,
						src_dy, VAR.resize_magnification_scale.y, 1.0);
				}
				else if(mask_type < 1.5)
				{
					pixel_color = downsample_vertical_sinc_tiled(
						mask_slot_texture_small, src_tex_uv, mask_resize_src_lut_size,
						src_dy, VAR.resize_magnification_scale.y, 1.0);
				}
				else
				{
					pixel_color = downsample_vertical_sinc_tiled(
						mask_shadow_texture_small, src_tex_uv, mask_resize_src_lut_size,
						src_dy, VAR.resize_magnification_scale.y, 1.0);
				}
			#endif
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

