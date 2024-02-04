#ifndef _GEOMETRY_AA_LAST_PASS_H
#define _GEOMETRY_AA_LAST_PASS_H

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
#include "../lib/tex2Dantialias.fxh"
#include "../lib/geometry-functions.fxh"

// Disabled in the ReShade port because I don't know a good way to make these
// static AND global AND defined with sin(), cos(), or pow().

// #if !_RUNTIME_GEOMETRY_TILT
//     //  Create a local-to-global rotation matrix for the CRT's coordinate frame
//     //  and its global-to-local inverse.  See the vertex shader for details.
//     //  It's faster to compute these statically if possible.
//     static const float2 sin_tilt = sin(geom_tilt_angle_static);
//     static const float2 cos_tilt = cos(geom_tilt_angle_static);
//     static const float3x3 geom_local_to_global_static = float3x3(
//         cos_tilt.x, sin_tilt.y*sin_tilt.x, cos_tilt.y*sin_tilt.x,
//         0.0, cos_tilt.y, -sin_tilt.y,
//         -sin_tilt.x, sin_tilt.y*cos_tilt.x, cos_tilt.y*cos_tilt.x);
//     static const float3x3 geom_global_to_local_static = float3x3(
//         cos_tilt.x, 0.0, -sin_tilt.x,
//         sin_tilt.y*sin_tilt.x, cos_tilt.y, sin_tilt.y*cos_tilt.x,
//         cos_tilt.y*sin_tilt.x, -sin_tilt.y, cos_tilt.y*cos_tilt.x);
// #endif

float2x2 mul_scale(float2 scale, float2x2 mtrx)
{
    float4 temp_matrix = float4(mtrx[0][0], mtrx[0][1], mtrx[1][0], mtrx[1][1]) * scale.xxyy;
    return float2x2(temp_matrix.x, temp_matrix.y, temp_matrix.z, temp_matrix.w);
}


void geometryVS(
    in uint id : SV_VertexID,

    out float4 position : SV_Position,
    out float2 texcoord : TEXCOORD0,
    out float2 output_size_inv : TEXCOORD1,
    out float4 geom_aspect_and_overscan : TEXCOORD2,
    out float3 eye_pos_local : TEXCOORD3,
    out float3 global_to_local_row0 : TEXCOORD4,
    out float3 global_to_local_row1 : TEXCOORD5,
    out float3 global_to_local_row2 : TEXCOORD6
) {
    PostProcessVS(id, position, texcoord);

    output_size_inv = 1.0 / content_size;

    //  Get aspect/overscan vectors from scalar parameters (likely uniforms):
    const float viewport_aspect_ratio =  output_size_inv.y / output_size_inv.x;
    const float2 geom_aspect = get_aspect_vector(viewport_aspect_ratio);
    const float2 geom_overscan = get_geom_overscan_vector();
    geom_aspect_and_overscan = float4(geom_aspect, geom_overscan);

    #if _RUNTIME_GEOMETRY_TILT
        //  Create a local-to-global rotation matrix for the CRT's coordinate
        //  frame and its global-to-local inverse.  Rotate around the x axis
        //  first (pitch) and then the y axis (yaw) with yucky Euler angles.
        //  Positive angles go clockwise around the right-vec and up-vec.
        //  Runtime shader parameters prevent us from computing these globally,
        //  but we can still combine the pitch/yaw matrices by hand to cut a
        //  few instructions.  Note that cg matrices fill row1 first, then row2,
        //  etc. (row-major order).
        const float2 geom_tilt_angle = get_geom_tilt_angle_vector();
        const float2 sin_tilt = sin(geom_tilt_angle);
        const float2 cos_tilt = cos(geom_tilt_angle);
        //  Conceptual breakdown:
              static const float3x3 rot_x_matrix = float3x3(
                  1.0, 0.0, 0.0,
                  0.0, cos_tilt.y, -sin_tilt.y,
                  0.0, sin_tilt.y, cos_tilt.y);
              static const float3x3 rot_y_matrix = float3x3(
                  cos_tilt.x, 0.0, sin_tilt.x,
                  0.0, 1.0, 0.0,
                  -sin_tilt.x, 0.0, cos_tilt.x);
              static const float3x3 local_to_global =
                  mul(rot_y_matrix, rot_x_matrix);
/*              static const float3x3 global_to_local =
                  transpose(local_to_global);
        const float3x3 local_to_global = float3x3(
            cos_tilt.x, sin_tilt.y*sin_tilt.x, cos_tilt.y*sin_tilt.x,
            0.0, cos_tilt.y, sin_tilt.y,
            sin_tilt.x, sin_tilt.y*cos_tilt.x, cos_tilt.y*cos_tilt.x);
*/        //  This is a pure rotation, so transpose = inverse:
        const float3x3 global_to_local = transpose(local_to_global);
        //  Decompose the matrix into 3 float3's for output:
        global_to_local_row0 = float3(global_to_local[0][0], global_to_local[0][1], global_to_local[0][2]);//._m00_m01_m02);
        global_to_local_row1 = float3(global_to_local[1][0], global_to_local[1][1], global_to_local[1][2]);//._m10_m11_m12);
        global_to_local_row2 = float3(global_to_local[2][0], global_to_local[2][1], global_to_local[2][2]);//._m20_m21_m22);
    #else
        static const float3x3 global_to_local = geom_global_to_local_static;
        static const float3x3 local_to_global = geom_local_to_global_static;
    #endif

    //  Get an optimal eye position based on geom_view_dist, viewport_aspect,
    //  and CRT radius/rotation:
    #if _RUNTIME_GEOMETRY_MODE
        const float geom_mode = geom_mode_runtime;
    #else
        static const float geom_mode = geom_mode_static;
    #endif
    const float3 eye_pos_global = get_ideal_global_eye_pos(local_to_global, geom_aspect, geom_mode);
    eye_pos_local = mul(global_to_local, eye_pos_global);
}

void geometryPS(
    in float4 position : SV_Position,
    in float2 texcoord : TEXCOORD0,
    in float2 output_size_inv : TEXCOORD1,
    in float4 geom_aspect_and_overscan : TEXCOORD2,
    in float3 eye_pos_local : TEXCOORD3,
    in float3 global_to_local_row0 : TEXCOORD4,
    in float3 global_to_local_row1 : TEXCOORD5,
    in float3 global_to_local_row2 : TEXCOORD6,

    out float4 color : SV_Target
) {
    //  Localize some parameters:
    const float2 geom_aspect = geom_aspect_and_overscan.xy;
    const float2 geom_overscan = geom_aspect_and_overscan.zw;
    #if _RUNTIME_GEOMETRY_TILT
        const float3x3 global_to_local = float3x3(global_to_local_row0,
            global_to_local_row1, global_to_local_row2);
    #else
        static const float3x3 global_to_local = geom_global_to_local_static;
    #endif
    #if _RUNTIME_GEOMETRY_MODE
        const float geom_mode = geom_mode_runtime;
    #else
        static const float geom_mode = geom_mode_static;
    #endif

    //  Get flat and curved texture coords for the current fragment point sample
    //  and a pixel_to_tangent_video_uv matrix for transforming pixel offsets:
    //  video_uv = relative position in video frame, mapped to [0.0, 1.0] range
    //  tex_uv = relative position in padded texture, mapped to [0.0, 1.0] range
    const float2 flat_video_uv = texcoord;
    float2x2 pixel_to_video_uv;
    float2 video_uv_no_geom_overscan;
    if(geom_mode > 0.5)
    {
        video_uv_no_geom_overscan =
            get_curved_video_uv_coords_and_tangent_matrix(flat_video_uv,
                eye_pos_local, output_size_inv, geom_aspect,
                geom_mode, global_to_local, pixel_to_video_uv);
    }
    else
    {
        video_uv_no_geom_overscan = flat_video_uv;
        pixel_to_video_uv = float2x2(
            output_size_inv.x, 0.0, 0.0, output_size_inv.y);
    }
    //  Correct for overscan here (not in curvature code):
    const float2 video_uv =
        (video_uv_no_geom_overscan - float2(0.5, 0.5))/geom_overscan + float2(0.5, 0.5);
    const float2 tex_uv = video_uv;

    //  Get a matrix transforming pixel vectors to tex_uv vectors:
    const float2x2 pixel_to_tex_uv =
        mul_scale(1.0 / geom_overscan, pixel_to_video_uv);

    //  Sample!  Skip antialiasing if antialias_level < 0.5 or both of these hold:
    //  1.) Geometry/curvature isn't used
    //  2.) Overscan == float2(1.0, 1.0)
    //  Skipping AA is sharper, but it's only faster with dynamic branches.
    const float2 abs_aa_r_offset = abs(get_aa_subpixel_r_offset());
    // this next check seems to always return true, even when it shouldn't so disabling it for now
    const bool need_subpixel_aa = false;//abs_aa_r_offset.x + abs_aa_r_offset.y > 0.0;
    float3 raw_color;

    if(antialias_level > 0.5 && (geom_mode > 0.5 || any(bool2((geom_overscan.x != 1.0), (geom_overscan.y != 1.0)))))
    {
        //  Sample the input with antialiasing (due to sharp phosphors, etc.):
        raw_color = tex2Daa(samplerBloomHorizontal, tex_uv, pixel_to_tex_uv, float(frame_count), get_intermediate_gamma());
    }
    else if(antialias_level > 0.5 && need_subpixel_aa)
    {
        //  Sample at each subpixel location:
        raw_color = tex2Daa_subpixel_weights_only(
            samplerBloomHorizontal, tex_uv, pixel_to_tex_uv, get_intermediate_gamma());
    }
    else
    {
        raw_color = tex2D_linearize(samplerBloomHorizontal, tex_uv, get_intermediate_gamma()).rgb;
    }

    //  Dim borders and output the final result:
    const float border_dim_factor = get_border_dim_factor(video_uv, geom_aspect);
    const float3 final_color = raw_color * border_dim_factor;

    color = encode_output(float4(final_color, 1.0), get_output_gamma());
}

#endif  //  _GEOMETRY_AA_LAST_PASS_H