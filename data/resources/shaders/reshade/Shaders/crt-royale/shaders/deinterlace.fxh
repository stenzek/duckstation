#ifndef _DEINTERLACE_H
#define _DEINTERLACE_H

/////////////////////////////////  MIT LICENSE  ////////////////////////////////

//  Copyright (C) 2020 Alex Gunter
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


#include "../lib/user-settings.fxh"
#include "../lib/derived-settings-and-constants.fxh"
#include "../lib/bind-shader-params.fxh"
#include "../lib/gamma-management.fxh"
#include "../lib/scanline-functions.fxh"



void freezeFrameVS(
    in uint id : SV_VertexID,

    out float4 position : SV_Position,
    out float2 texcoord : TEXCOORD0
) {
    float use_deinterlacing_tex = enable_interlacing && (
        scanline_deinterlacing_mode == 2 || scanline_deinterlacing_mode == 3
    );

	texcoord.x = (id == 2) ? use_deinterlacing_tex*2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2, -2) + float2(-1, 1), 0, 1);
}

void freezeFramePS(
    in float4 pos : SV_Position,
    in float2 texcoord : TEXCOORD0,

    out float4 color : SV_Target
) {
    color = tex2D(samplerBeamConvergence, texcoord);
}


void deinterlaceVS(
    in uint id : SV_VertexID,

    out float4 position : SV_Position,
    out float2 texcoord : TEXCOORD0,
    out float2 v_step : TEXCOORD1
) {
    freezeFrameVS(id, position, texcoord);
    
    v_step = float2(0.0, scanline_thickness * rcp(TEX_FREEZEFRAME_HEIGHT));
}


void deinterlacePS(
    in float4 pos : SV_Position,
    in float2 texcoord : TEXCOORD0,
    in float2 v_step : TEXCOORD1,
    
    out float4 color : SV_Target
) {
    // float2 scanline_offset_norm;
    // float triangle_wave_freq;
    // bool field_parity;
    // bool wrong_field;
    // calc_wrong_field(texcoord, scanline_offset_norm, triangle_wave_freq, field_parity, wrong_field);
    
    float2 rotated_coord = lerp(texcoord.yx, texcoord, geom_rotation_mode == 0 || geom_rotation_mode == 2);
    float scale = lerp(CONTENT_WIDTH, CONTENT_HEIGHT, geom_rotation_mode == 0 || geom_rotation_mode == 2);

    InterpolationFieldData interpolation_data = calc_interpolation_field_data(rotated_coord, scale);

    // TODO: add scanline_parity to calc_wrong_field()

    // Weaving
    // Sample texcoord from this frame and the previous frame
    // If we're in the correct field, use the current sample
    // If we're in the wrong field, average the current and prev samples
    //   In this case, we're probably averaging a color with 0 and producing a brightness of 0.5.
    [branch]
    if (enable_interlacing && scanline_deinterlacing_mode == 2) {
        // const float cur_scanline_idx = get_curr_scanline_idx(texcoord.y, content_size.y);
        // const float wrong_field = curr_line_is_wrong_field(cur_scanline_idx);
        
        const float4 cur_line_color = tex2D_nograd(samplerBeamConvergence, texcoord);
        const float4 cur_line_prev_color = tex2D_nograd(samplerFreezeFrame, texcoord);

        const float4 avg_color = (cur_line_color + cur_line_prev_color) / 2.0;

        // Multiply by 1.5, so each pair of scanlines has total brightness 2
        const float4 raw_out_color = lerp(1.5*cur_line_color, avg_color, interpolation_data.wrong_field);
        color = encode_output(raw_out_color, deinterlacing_blend_gamma);
    }
    // Blended Weaving
    // Sample texcoord from this frame
    // From the previous frame, sample the current scanline's sibling
    //   Do this by shifting up or down by a line
    // If we're in the correct field, use the current sample
    // If we're in the wrong field, average the current and prev samples
    //   In this case, we're averaging two fully illuminated colors
    else if (enable_interlacing && scanline_deinterlacing_mode == 3) {
        const float2 raw_offset = lerp(1, -1, interpolation_data.scanline_parity) * v_step;
        const float2 curr_offset = lerp(0, raw_offset, interpolation_data.wrong_field);
        const float2 prev_offset = lerp(raw_offset, 0, interpolation_data.wrong_field);

        const float4 cur_line_color = tex2D_nograd(samplerBeamConvergence, texcoord + curr_offset);
        const float4 prev_line_color = tex2D_nograd(samplerFreezeFrame, texcoord + prev_offset);

        const float4 avg_color = (cur_line_color + prev_line_color) / 2.0;
        const float4 raw_out_color = lerp(cur_line_color, avg_color, interpolation_data.wrong_field);
        color = encode_output(raw_out_color, deinterlacing_blend_gamma);
    }
    // No temporal blending
    else {
        color = tex2D_nograd(samplerBeamConvergence, texcoord);
    }
}

#endif  //  _DEINTERLACE_H