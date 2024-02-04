#ifndef _BRIGHTPASS_H
#define _BRIGHTPASS_H

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
#include "../lib/phosphor-mask-calculations.fxh"
#include "../lib/scanline-functions.fxh"
#include "../lib/bloom-functions.fxh"
#include "../lib/blur-functions.fxh"


void brightpassVS(
    in uint id : SV_VertexID,

    out float4 position : SV_Position,
    out float2 texcoord : TEXCOORD0,
    out float bloom_sigma_runtime : TEXCOORD1
) {
    PostProcessVS(id, position, texcoord);

    bloom_sigma_runtime = get_min_sigma_to_blur_triad(calc_triad_size().x, bloom_diff_thresh_);
}

void brightpassPS(
    in float4 pos : SV_Position,
    in float2 texcoord : TEXCOORD0,
    in float bloom_sigma_runtime : TEXCOORD1,

    out float4 color : SV_Target
) {
    //  Sample the masked scanlines:
    const float3 intensity_dim = tex2D_linearize(samplerMaskedScanlines, texcoord, get_intermediate_gamma()).rgb;
    //  Get the full intensity, including auto-undimming, and mask compensation:
    const float mask_amplify = get_mask_amplify();
    const float3 intensity = intensity_dim * rcp(levels_autodim_temp) * mask_amplify * levels_contrast;

    //  Sample BLOOM_APPROX to estimate what a straight blur of masked scanlines
    //  would look like, so we can estimate how much energy we'll receive from
    //  blooming neighbors:
    const float3 phosphor_blur_approx = levels_contrast * tex2D_linearize(samplerBloomApproxHoriz, texcoord, get_intermediate_gamma()).rgb;

    //  Compute the blur weight for the center texel and the maximum energy we
    //  expect to receive from neighbors:
    const float bloom_sigma = get_final_bloom_sigma(bloom_sigma_runtime);
    const float center_weight = get_center_weight(bloom_sigma);
    const float3 max_area_contribution_approx =
        max(float3(0.0, 0.0, 0.0), phosphor_blur_approx - center_weight * intensity);
    //  Assume neighbors will blur 100% of their intensity (blur_ratio = 1.0),
    //  because it actually gets better results (on top of being very simple),
    //  but adjust all intensities for the user's desired underestimate factor:
    const float3 area_contrib_underestimate = bloom_underestimate_levels * max_area_contribution_approx;
    const float3 intensity_underestimate = bloom_underestimate_levels * intensity;
    //  Calculate the blur_ratio, the ratio of intensity we want to blur:
    const float3 blur_ratio_temp =
        ((float3(1.0, 1.0, 1.0) - area_contrib_underestimate) /
        intensity_underestimate - float3(1.0, 1.0, 1.0)) / (center_weight - 1.0);
    const float3 blur_ratio = saturate(blur_ratio_temp);
    //  Calculate the brightpass based on the auto-dimmed, unamplified, masked
    //  scanlines, encode if necessary, and return!
    const float3 brightpass = intensity_dim *
        lerp(blur_ratio, float3(1.0, 1.0, 1.0), bloom_excess);
    
    color = encode_output(float4(brightpass, 1.0), get_intermediate_gamma());
}

#endif  //  _BRIGHTPASS_H