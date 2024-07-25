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
#include "../include/scanline-functions.fxh"

/////////////////////////////////  STRUCTURES  /////////////////////////////////

struct out_vertex_p10
{
    float2 video_uv            : TEXCOORD1;
    float2 bloom_dxdy          : TEXCOORD2;
    float bloom_sigma_runtime  : TEXCOORD3;
    float2 sinangle            : TEXCOORD4;
    float2 cosangle            : TEXCOORD5;
    float3 stretch             : TEXCOORD6;
};


////////////////////////////////  VERTEX SHADER  ///////////////////////////////

// Vertex shader generating a triangle covering the entire screen
void VS_Bloom_Horizontal(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out out_vertex_p10 OUT)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    float2 texture_size = BLOOM_HORIZONTAL_texture_size;
    float2 output_size  = VIEWPORT_SIZE;

    // Screen centering
    texcoord = texcoord - float2(centerx,centery)/100.0;

    float2 tex_uv = texcoord;

    //  Our various input textures use different coords:
    const float2 video_uv = tex_uv * texture_size/video_size;
    OUT.video_uv = video_uv;

    //  We're horizontally blurring the bloom input (vertically blurred
    //  brightpass).  Get the uv distance between output pixels / input texels
    //  in the horizontal direction (this pass must NOT resize):
    OUT.bloom_dxdy = float2(1.0/texture_size.x, 0.0);

    //  Calculate a runtime bloom_sigma in case it's needed:
    const float mask_tile_size_x = get_resized_mask_tile_size(
        output_size, output_size * mask_resize_viewport_scale, false).x;
    OUT.bloom_sigma_runtime = get_min_sigma_to_blur_triad(
        mask_tile_size_x / mask_triads_per_tile, bloom_diff_thresh);

    // Precalculate a bunch of useful values we'll need in the fragment
    // shader.
    OUT.sinangle    = sin(float2(geom_x_tilt, geom_y_tilt));
    OUT.cosangle    = cos(float2(geom_x_tilt, geom_y_tilt));
    OUT.stretch     = maxscale(OUT.sinangle, OUT.cosangle);
}


///////////////////////////////  FRAGMENT SHADER  //////////////////////////////

float4 PS_Bloom_Horizontal(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in out_vertex_p10 VAR) : SV_Target
{
    VAR.video_uv = (geom_curvature == true) ? transform(VAR.video_uv, VAR.sinangle, VAR.cosangle, VAR.stretch) : VAR.video_uv;

    float cval = corner((VAR.video_uv-0.5.xx) * BufferToViewportRatio + 0.5.xx);

    //  Blur the vertically blurred brightpass horizontally by 9/17/25/43x:
    const float bloom_sigma = get_final_bloom_sigma(VAR.bloom_sigma_runtime);
    const float3 blurred_brightpass = tex2DblurNfast(BLOOM_VERTICAL,
        VAR.video_uv, VAR.bloom_dxdy, bloom_sigma);

    //  Sample the masked scanlines.  Alpha contains the auto-dim factor:
    const float3 intensity_dim =
        tex2D_linearize(MASKED_SCANLINES, VAR.video_uv).rgb;
    const float auto_dim_factor = levels_autodim_temp;
    const float undim_factor = 1.0/auto_dim_factor;

    //  Calculate the mask dimpass, add it to the blurred brightpass, and
    //  undim (from scanline auto-dim) and amplify (from mask dim) the result:
    const float mask_amplify = get_mask_amplify();
    const float3 brightpass = tex2D_linearize(BRIGHTPASS,
        VAR.video_uv).rgb;
    const float3 dimpass = intensity_dim - brightpass;
    const float3 phosphor_bloom = (dimpass + blurred_brightpass) *
        mask_amplify * undim_factor * levels_contrast;

    //  Sample the halation texture, and let some light bleed into refractive
    //  diffusion.  Conceptually this occurs before the phosphor bloom, but
    //  adding it in earlier passes causes black crush in the diffusion colors.
    const float3 diffusion_color = levels_contrast * tex2D_linearize(
        HALATION_BLUR, VAR.video_uv).rgb;
    float3 final_bloom = lerp(phosphor_bloom,
        diffusion_color, diffusion_weight);

    final_bloom = (geom_curvature == true) ? final_bloom * cval.xxx : final_bloom;

    final_bloom = pow(final_bloom.rgb, 1.0/get_output_gamma());

    //  Encode and output the bloomed image:
    return encode_output(float4(final_bloom, 1.0));
}

