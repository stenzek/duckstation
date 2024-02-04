#ifndef _PHOSPHOR_MASK_H
#define _PHOSPHOR_MASK_H

/////////////////////////////////  MIT LICENSE  ////////////////////////////////

//  Copyright (C) 2022 Alex Gunter
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to
//  deal in the Software without restriction, including without limitation the
//  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
//  sell copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//  
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
//  IN THE SOFTWARE.


#include "../lib/bind-shader-params.fxh"
#include "../lib/phosphor-mask-calculations.fxh"

#include "shared-objects.fxh"


// Split into 64 segments that overlap a little bit
static const float num_segments = 64;
static const float segment_offset = 0.015625;  // 1/64
static const float segment_width = 0.0234375;  // 1/128

void generatePhosphorMaskVS(
    in uint id : SV_VertexID,

    out float4 position : SV_Position,
    out float2 texcoord : TEXCOORD0,
    out float2 viewport_frequency_factor: TEXCOORD1,
    out float2 mask_pq_x : TEXCOORD2,
    out float2 mask_pq_y : TEXCOORD3
) {
    const float screen_segment_idx = frame_count % num_segments;
    const float left_coord = lerp(segment_offset * screen_segment_idx, 0, overlay_active > 0);
    const float right_coord = lerp(left_coord + segment_width, 1, overlay_active > 0);
    const float pos_center = 2 * (left_coord + 0.5 * segment_width - 0.5);
    const float pos_left = lerp(pos_center - segment_width, -1, overlay_active > 0);
    const float pos_right = lerp(pos_center + segment_width, 1, overlay_active > 0);

    #if _DX9_ACTIVE
        texcoord.x = (id == 1 || id == 3) ? right_coord : left_coord;
        texcoord.y = (id > 1) ? 1 : 0;

        position.x = (id == 1 || id == 3) ?  pos_right : pos_left;
        position.y = (id > 1) ? -1 :  1;
        position.zw = 1;
    #else
        texcoord.x = (id & 1) ? right_coord : left_coord;
        texcoord.y = (id & 2) ? 1 : 0;

        position.x = (id & 1) ?  pos_right : pos_left;
        position.y = (id & 2) ? -1 :  1;
        position.zw = 1;
    #endif

    viewport_frequency_factor = calc_phosphor_viewport_frequency_factor();

    // We don't alter these based on screen rotation because they're independent of screen dimensions.
    float edge_norm_tx;
    float edge_norm_ty;
    [flatten]
    switch (mask_type) {
        case 0:
            edge_norm_tx = grille_edge_norm_t;
            break;
        case 1:
            edge_norm_tx = slot_edge_norm_tx;
            edge_norm_ty = slot_edge_norm_ty;
            break;
        case 2:
            edge_norm_tx = shadow_edge_norm_tx;
            edge_norm_ty = shadow_edge_norm_ty;
            break;
        case 3:
            edge_norm_tx = smallgrille_edge_norm_t;
            break;
        case 4:
            edge_norm_tx = smallslot_edge_norm_tx;
            edge_norm_ty = smallslot_edge_norm_ty;
            break;
        default:
            edge_norm_tx = smallshadow_edge_norm_tx;
            edge_norm_ty = smallshadow_edge_norm_ty;
            break;
    }
    
    const float2 thickness_scaled = linearize_phosphor_thickness_param(phosphor_thickness);
    const float mask_p_x = exp(-calculate_phosphor_p_value(edge_norm_tx, thickness_scaled.x, phosphor_sharpness.x));
    const float mask_p_y = exp(-calculate_phosphor_p_value(edge_norm_ty, thickness_scaled.y, phosphor_sharpness.y));
    mask_pq_x = float2(mask_p_x, phosphor_sharpness.x);
    mask_pq_y = float2(mask_p_y, phosphor_sharpness.y);
}

void generatePhosphorMaskPS(
    in float4 pos : SV_Position,
    in float2 texcoord : TEXCOORD0,
    in float2 viewport_frequency_factor: TEXCOORD1,
    in float2 mask_pq_x : TEXCOORD2,
    in float2 mask_pq_y : TEXCOORD3,
    
    out float4 color : SV_Target
) {
    [branch]
    if (geom_rotation_mode == 1 || geom_rotation_mode == 3) {
        texcoord = texcoord.yx;
        viewport_frequency_factor = viewport_frequency_factor.yx;
    }

    float3 phosphor_color;
    [branch]
    if (mask_type == 0) {
        phosphor_color = get_phosphor_intensity_grille(
            texcoord,
            viewport_frequency_factor,
            mask_pq_x
        );
    }
    else if (mask_type == 1) {
        phosphor_color = get_phosphor_intensity_slot(
            texcoord,
            viewport_frequency_factor,
            mask_pq_x,
            mask_pq_y
        );
    }
    else if (mask_type == 2) {
        phosphor_color = get_phosphor_intensity_shadow(
            texcoord,
            viewport_frequency_factor,
            float2(mask_pq_x.y, mask_pq_y.y)
        );
    }
    else if (mask_type == 3) {
        phosphor_color = get_phosphor_intensity_grille_small(
            texcoord,
            viewport_frequency_factor,
            mask_pq_x
        );
    }
    else if (mask_type == 4) {
        phosphor_color = get_phosphor_intensity_slot_small(
            texcoord,
            viewport_frequency_factor,
            mask_pq_x,
            mask_pq_y
        );
    }
    else {
        phosphor_color = get_phosphor_intensity_shadow_small(
            texcoord,
            viewport_frequency_factor,
            mask_pq_x,
            mask_pq_y
        );
    }

    color = float4(phosphor_color, 1.0);
}


void applyComputedPhosphorMaskPS(
    in float4 pos : SV_Position,
    in float2 texcoord : TEXCOORD0,
    
    out float4 color : SV_Target
) {
    bool use_deinterlacing_tex = enable_interlacing && (
        scanline_deinterlacing_mode == 2 || scanline_deinterlacing_mode == 3
    );

    float3 scanline_color_dim;
    [branch]
    if (use_deinterlacing_tex) scanline_color_dim = tex2D(samplerDeinterlace, texcoord).rgb;
    else scanline_color_dim = tex2D(samplerBeamConvergence, texcoord).rgb;

    const float3 phosphor_color = tex2D(samplerPhosphorMask, texcoord).rgb;

    //  Sample the halation texture (auto-dim to match the scanlines), and
    //  account for both horizontal and vertical convergence offsets, given
    //  in units of texels horizontally and same-field scanlines vertically:
    const float3 halation_color = tex2D_linearize(samplerBlurHorizontal, texcoord, get_intermediate_gamma()).rgb;

    //  Apply halation: Halation models electrons flying around under the glass
    //  and hitting the wrong phosphors (of any color).  It desaturates, so
    //  average the halation electrons to a scalar.  Reduce the local scanline
    //  intensity accordingly to conserve energy.
    const float halation_intensity_dim_scalar = dot(halation_color, float3(1, 1, 1)) / 3.0;
    const float3 halation_intensity_dim = halation_intensity_dim_scalar;
    const float3 electron_intensity_dim = lerp(scanline_color_dim, halation_intensity_dim, halation_weight);

    //  Apply the phosphor mask:
    const float3 phosphor_emission_dim = electron_intensity_dim * phosphor_color;
    
    color = float4(phosphor_emission_dim, 1.0);
}

#endif  //  _PHOSPHOR_MASK_H