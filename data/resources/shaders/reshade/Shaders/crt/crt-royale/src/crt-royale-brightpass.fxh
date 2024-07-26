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
#include "../include/blur-functions.fxh"
#include "../include/phosphor-mask-resizing.fxh"
#include "../include/scanline-functions.fxh"
#include "../include/bloom-functions.fxh"

/////////////////////////////////  STRUCTURES  /////////////////////////////////

struct out_vertex_p8
{
    float2 video_uv                     : TEXCOORD1;
    float2 scanline_tex_uv              : TEXCOORD2;
    float2 blur3x3_tex_uv               : TEXCOORD3;
    float bloom_sigma_runtime           : TEXCOORD4;
};


////////////////////////////////  VERTEX SHADER  ///////////////////////////////

// Vertex shader generating a triangle covering the entire screen
void VS_Brightpass(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out out_vertex_p8 OUT)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    float2 tex_uv = texcoord;

    float2 texture_size = BRIGHTPASS_texture_size;
    float2 output_size  = VIEWPORT_SIZE;

    //  Our various input textures use different coords:
    const float2 video_uv = tex_uv * texture_size/video_size;
    OUT.video_uv = video_uv;
    OUT.scanline_tex_uv = video_uv * MASKED_SCANLINES_video_size /
        MASKED_SCANLINES_texture_size;
    OUT.blur3x3_tex_uv = video_uv * BLOOM_APPROX_video_size / BLOOM_APPROX_texture_size;

    //  Calculate a runtime bloom_sigma in case it's needed:
    const float mask_tile_size_x = get_resized_mask_tile_size(
        output_size, output_size * mask_resize_viewport_scale, false).x;
    OUT.bloom_sigma_runtime = get_min_sigma_to_blur_triad(
        mask_tile_size_x / mask_triads_per_tile, bloom_diff_thresh);
}


///////////////////////////////  FRAGMENT SHADER  //////////////////////////////

float4 PS_Brightpass(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in out_vertex_p8 VAR) : SV_Target
{
    //  Sample the masked scanlines:
    const float3 intensity_dim =
        tex2D_linearize(MASKED_SCANLINES, VAR.scanline_tex_uv).rgb;
    //  Get the full intensity, including auto-undimming, and mask compensation:
    const float auto_dim_factor = levels_autodim_temp;
    const float undim_factor = 1.0/auto_dim_factor;
    const float mask_amplify = get_mask_amplify();
    const float3 intensity = intensity_dim * undim_factor * mask_amplify *
        levels_contrast;

    //  Sample BLOOM_APPROX to estimate what a straight blur of masked scanlines
    //  would look like, so we can estimate how much energy we'll receive from
    //  blooming neighbors:
    const float3 phosphor_blur_approx = levels_contrast * tex2D_linearize(
        BLOOM_APPROX, VAR.blur3x3_tex_uv).rgb;

    //  Compute the blur weight for the center texel and the maximum energy we
    //  expect to receive from neighbors:
    const float bloom_sigma = get_final_bloom_sigma(VAR.bloom_sigma_runtime);
    const float center_weight = get_center_weight(bloom_sigma);
    const float3 max_area_contribution_approx =
        max(0.0.xxx, phosphor_blur_approx - center_weight * intensity);
    //  Assume neighbors will blur 100% of their intensity (blur_ratio = 1.0),
    //  because it actually gets better results (on top of being very simple),
    //  but adjust all intensities for the user's desired underestimate factor:
    const float3 area_contrib_underestimate =
        bloom_underestimate_levels * max_area_contribution_approx;
    const float3 intensity_underestimate =
        bloom_underestimate_levels * intensity;
    //  Calculate the blur_ratio, the ratio of intensity we want to blur:
    #ifdef BRIGHTPASS_AREA_BASED
        //  This area-based version changes blur_ratio more smoothly and blurs
        //  more, clipping less but offering less phosphor differentiation:
        const float3 phosphor_blur_underestimate = bloom_underestimate_levels *
            phosphor_blur_approx;
        const float3 soft_intensity = max(intensity_underestimate,
            phosphor_blur_underestimate * mask_amplify);
        const float3 blur_ratio_temp =
            ((1.0.xxx - area_contrib_underestimate) /
            soft_intensity - 1.0.xxx) / (center_weight - 1.0);
    #else
        const float3 blur_ratio_temp =
            ((1.0.xxx - area_contrib_underestimate) /
            intensity_underestimate - 1.0.xxx) / (center_weight - 1.0);
    #endif
    const float3 blur_ratio = clamp(blur_ratio_temp, 0.0, 1.0);
    //  Calculate the brightpass based on the auto-dimmed, unamplified, masked
    //  scanlines, encode if necessary, and return!
    const float3 brightpass = intensity_dim *
        lerp(blur_ratio, 1.0.xxx, bloom_excess);
    return encode_output(float4(brightpass, 1.0));
}

