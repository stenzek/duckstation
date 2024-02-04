#ifndef _BLOOM_H
#define _BLOOM_H

/////////////////////////////  GPL LICENSE NOTICE  /////////////////////////////

//  crt-royale: A full-featured CRT shader, with cheese.
//  Copyright (C) 2014 TroggleMonkey <trogglemonkey@gmx.com>
//
//  crt-royale-reshade: A port of TroggleMonkey's crt-royale from libretro to ReShade.
//  Copyright (C) 2020 Alex Gunter <akg7634@gmail.com>
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

#include "../lib/user-settings.fxh"
#include "../lib/derived-settings-and-constants.fxh"
#include "../lib/bind-shader-params.fxh"
#include "../lib/gamma-management.fxh"
#include "../lib/downsampling-functions.fxh"
#include "../lib/blur-functions.fxh"
#include "../lib/bloom-functions.fxh"

#include "shared-objects.fxh"


void approximateBloomVertPS(
    in float4 pos : SV_Position,
    in float2 texcoord : TEXCOORD0,

    out float4 color : SV_Target
) {
    const float2 delta_uv = blur_radius * float2(0.0, rcp(TEX_BEAMCONVERGENCE_HEIGHT));

    color = float4(opaque_linear_downsample(
        samplerBeamConvergence, texcoord,
        uint((bloomapprox_downsizing_factor - 1)/2),
        delta_uv
    ), 1);
}

void approximateBloomHorizPS(
    in float4 pos : SV_Position,
    in float2 texcoord : TEXCOORD0,

    out float4 color : SV_Target
) {
    const float2 delta_uv = blur_radius * float2(rcp(TEX_BEAMCONVERGENCE_WIDTH), 0.0);

    color = float4(opaque_linear_downsample(
        samplerBloomApproxVert, texcoord,
        uint((bloomapprox_downsizing_factor - 1)/2),
        delta_uv
    ), 1);
}


void bloomHorizontalVS(
    in uint id : SV_VertexID,

    out float4 position : SV_Position,
    out float2 texcoord : TEXCOORD0,
    out float bloom_sigma_runtime : TEXCOORD1
) {
    PostProcessVS(id, position, texcoord);
    
    bloom_sigma_runtime = get_min_sigma_to_blur_triad(calc_triad_size().x, bloom_diff_thresh_);
}

void bloomHorizontalPS(
    in float4 pos : SV_Position,
    in float2 texcoord : TEXCOORD0,
    in float bloom_sigma_runtime : TEXCOORD1,

    out float4 color : SV_Target
) {
    const float2 bloom_dxdy = float2(rcp(TEX_BLOOMVERTICAL_WIDTH), 0);

    //  Blur the vertically blurred brightpass horizontally by 9/17/25/43x:
    const float bloom_sigma = get_final_bloom_sigma(bloom_sigma_runtime);
    const float3 blurred_brightpass = tex2DblurNfast(samplerBloomVertical,
        texcoord, bloom_dxdy, bloom_sigma, get_intermediate_gamma());

    //  Sample the masked scanlines.  Alpha contains the auto-dim factor:
    const float3 intensity_dim = tex2D_linearize(samplerMaskedScanlines, texcoord, get_intermediate_gamma()).rgb;
    const float auto_dim_factor = levels_autodim_temp;
    const float undim_factor = 1.0/auto_dim_factor;

    //  Calculate the mask dimpass, add it to the blurred brightpass, and
    //  undim (from scanline auto-dim) and amplify (from mask dim) the result:
    const float mask_amplify = get_mask_amplify();
    const float3 brightpass = tex2D_linearize(samplerBrightpass, texcoord, get_intermediate_gamma()).rgb;
    const float3 dimpass = intensity_dim - brightpass;
    const float3 phosphor_bloom = (dimpass + blurred_brightpass) *
        mask_amplify * undim_factor * levels_contrast;

    //  Sample the halation texture, and let some light bleed into refractive
    //  diffusion.  Conceptually this occurs before the phosphor bloom, but
    //  adding it in earlier passes causes black crush in the diffusion colors.
    const float3 raw_diffusion_color = tex2D_linearize(samplerBlurHorizontal, texcoord, get_intermediate_gamma()).rgb;
    const float3 raw_halation_color = dot(raw_diffusion_color, float3(1, 1, 1)) / 3.0;
    const float3 diffusion_color = levels_contrast * lerp(raw_diffusion_color, raw_halation_color, halation_weight);
    const float3 final_bloom = lerp(phosphor_bloom, diffusion_color, diffusion_weight);

    //  Encode and output the bloomed image:
    color = encode_output(float4(final_bloom, 1.0), get_intermediate_gamma());
}


void bloomVerticalVS(
    in uint id : SV_VertexID,

    out float4 position : SV_Position,
    out float2 texcoord : TEXCOORD0,
    out float bloom_sigma_runtime : TEXCOORD1
) {
    PostProcessVS(id, position, texcoord);
    
    bloom_sigma_runtime = get_min_sigma_to_blur_triad(calc_triad_size().x, bloom_diff_thresh_);
}

void bloomVerticalPS(
    in float4 pos : SV_Position,
    in float2 texcoord : TEXCOORD0,
    in float bloom_sigma_runtime : TEXCOORD1,

    out float4 color : SV_Target
) {
    const float2 bloom_dxdy = float2(0, rcp(TEX_BLOOMVERTICAL_HEIGHT));

    //  Blur the brightpass horizontally with a 9/17/25/43x blur:
    const float bloom_sigma = get_final_bloom_sigma(bloom_sigma_runtime);
    const float3 color3 = tex2DblurNfast(samplerBrightpass, texcoord,
        bloom_dxdy, bloom_sigma, get_intermediate_gamma());

    //  Encode and output the blurred image:
    color = encode_output(float4(color3, 1.0), get_intermediate_gamma());
}

#endif  //  _BLOOM_H