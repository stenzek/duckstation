#ifndef BIND_SHADER_PARAMS_H
#define BIND_SHADER_PARAMS_H

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

#include "helper-functions-and-macros.fxh"
#include "user-settings.fxh"
#include "derived-settings-and-constants.fxh"

//  Override some parameters for gamma-management.h and tex2Dantialias.h:
#define OVERRIDE_DEVICE_GAMMA
static const float gba_gamma = 3.5; //  Irrelevant but necessary to define.
#define ANTIALIAS_OVERRIDE_BASICS
#define ANTIALIAS_OVERRIDE_PARAMETERS

//  Disable runtime shader params if the user doesn't explicitly want them.
//  Static constants will be defined in place of uniforms of the same name.
#ifndef RUNTIME_SHADER_PARAMS_ENABLE
    #undef PARAMETER_UNIFORM
#endif

//  Bind option names to shader parameter uniforms or static constants.
#ifdef PARAMETER_UNIFORM
    uniform float crt_gamma;
    uniform float lcd_gamma;
    uniform float levels_contrast;
    uniform float halation_weight;
    uniform float diffusion_weight;
    uniform float bloom_underestimate_levels;
    uniform float bloom_excess;
    uniform float beam_min_sigma;
    uniform float beam_max_sigma;
    uniform float beam_spot_power;
    uniform float beam_min_shape;
    uniform float beam_max_shape;
    uniform float beam_shape_power;
    uniform float beam_horiz_sigma;
    #ifdef RUNTIME_SCANLINES_HORIZ_FILTER_COLORSPACE
        uniform float beam_horiz_filter;
        uniform float beam_horiz_linear_rgb_weight;
    #else
        static const float beam_horiz_filter = clamp(beam_horiz_filter_static, 0.0, 2.0);
        static const float beam_horiz_linear_rgb_weight = clamp(beam_horiz_linear_rgb_weight_static, 0.0, 1.0);
    #endif
    uniform float convergence_offset_x_r;
    uniform float convergence_offset_x_g;
    uniform float convergence_offset_x_b;
    uniform float convergence_offset_y_r;
    uniform float convergence_offset_y_g;
    uniform float convergence_offset_y_b;
    #ifdef RUNTIME_PHOSPHOR_MASK_MODE_TYPE_SELECT
        uniform float mask_type;
    #else
        static const float mask_type = clamp(mask_type_static, 0.0, 2.0);
    #endif
    uniform float mask_sample_mode_desired;
    uniform float mask_specify_num_triads;
    uniform float mask_triad_size_desired;
    uniform float mask_num_triads_desired;
    uniform float aa_subpixel_r_offset_x_runtime;
    uniform float aa_subpixel_r_offset_y_runtime;
    #ifdef RUNTIME_ANTIALIAS_WEIGHTS
        uniform float aa_cubic_c;
        uniform float aa_gauss_sigma;
    #else
        static const float aa_cubic_c = aa_cubic_c_static;                              //  Clamp to [0, 4]?
        static const float aa_gauss_sigma = max(FIX_ZERO(0.0), aa_gauss_sigma_static);  //  Clamp to [FIXZERO(0), 1]?
    #endif
    uniform float geom_mode_runtime;
    uniform float geom_radius;
    uniform float geom_view_dist;
    uniform float geom_tilt_angle_x;
    uniform float geom_tilt_angle_y;
    uniform float geom_aspect_ratio_x;
    uniform float geom_aspect_ratio_y;
    uniform float geom_overscan_x;
    uniform float geom_overscan_y;
    uniform float border_size;
    uniform float border_darkness;
    uniform float border_compress;
    uniform float interlace_bff;
    uniform float interlace_1080i;
#else
    //  Use constants from user-settings.h, and limit ranges appropriately:
/*    static const float crt_gamma = macro_max(0.0, crt_gamma_static);
    static const float lcd_gamma = macro_max(0.0, lcd_gamma_static);
    static const float levels_contrast = macro_clamp(levels_contrast_static, 0.0, 4.0);
    static const float halation_weight = macro_clamp(halation_weight_static, 0.0, 1.0);
    static const float diffusion_weight = macro_clamp(diffusion_weight_static, 0.0, 1.0);
    static const float bloom_underestimate_levels = macro_max(FIX_ZERO(0.0), bloom_underestimate_levels_static);
    static const float bloom_excess = macro_clamp(bloom_excess_static, 0.0, 1.0);
    static const float beam_min_sigma = macro_max(FIX_ZERO(0.0), beam_min_sigma_static);
    static const float beam_max_sigma = macro_max(beam_min_sigma, beam_max_sigma_static);
    static const float beam_spot_power = macro_max(beam_spot_power_static, 0.0);
    static const float beam_min_shape = macro_max(2.0, beam_min_shape_static);
    static const float beam_max_shape = macro_max(beam_min_shape, beam_max_shape_static);
    static const float beam_shape_power = macro_max(0.0, beam_shape_power_static);
    static const float beam_horiz_filter = macro_clamp(beam_horiz_filter_static, 0.0, 2.0);
    static const float beam_horiz_sigma = macro_max(FIX_ZERO(0.0), beam_horiz_sigma_static);
    static const float beam_horiz_linear_rgb_weight = macro_clamp(beam_horiz_linear_rgb_weight_static, 0.0, 1.0);
*/    //  Unpack static vector elements to match scalar uniforms:
/*    static const float convergence_offset_x_r = macro_clamp(convergence_offsets_r_static.x, -4.0, 4.0);
    static const float convergence_offset_x_g = macro_clamp(convergence_offsets_g_static.x, -4.0, 4.0);
    static const float convergence_offset_x_b = macro_clamp(convergence_offsets_b_static.x, -4.0, 4.0);
    static const float convergence_offset_y_r = macro_clamp(convergence_offsets_r_static.y, -4.0, 4.0);
    static const float convergence_offset_y_g = macro_clamp(convergence_offsets_g_static.y, -4.0, 4.0);
    static const float convergence_offset_y_b = macro_clamp(convergence_offsets_b_static.y, -4.0, 4.0);
    static const float mask_type = macro_clamp(mask_type_static, 0.0, 2.0);
    static const float mask_sample_mode_desired = macro_clamp(mask_sample_mode_static, 0.0, 2.0);
    static const float mask_specify_num_triads = macro_clamp(mask_specify_num_triads_static, 0.0, 1.0);
    static const float mask_triad_size_desired = macro_clamp(mask_triad_size_desired_static, 1.0, 18.0);
    static const float mask_num_triads_desired = macro_clamp(mask_num_triads_desired_static, 342.0, 1920.0);
    static const float aa_subpixel_r_offset_x_runtime = macro_clamp(aa_subpixel_r_offset_static.x, -0.5, 0.5);
    static const float aa_subpixel_r_offset_y_runtime = macro_clamp(aa_subpixel_r_offset_static.y, -0.5, 0.5);
    static const float aa_cubic_c = aa_cubic_c_static;                              //  Clamp to [0, 4]?
    static const float aa_gauss_sigma = macro_max(FIX_ZERO(0.0), aa_gauss_sigma_static);  //  Clamp to [FIXZERO(0), 1]?
    static const float geom_mode_runtime = macro_clamp(geom_mode_static, 0.0, 3.0);
    static const float geom_radius = macro_max(1.0/(2.0*pi), geom_radius_static);         //  Clamp to [1/(2*pi), 1024]?
    static const float geom_view_dist = macro_max(0.5, geom_view_dist_static);            //  Clamp to [0.5, 1024]?
    static const float geom_tilt_angle_x = macro_clamp(geom_tilt_angle_static.x, -pi, pi);
    static const float geom_tilt_angle_y = macro_clamp(geom_tilt_angle_static.y, -pi, pi);
    static const float geom_aspect_ratio_x = geom_aspect_ratio_static;              //  Force >= 1?
    static const float geom_aspect_ratio_y = 1.0;
    static const float geom_overscan_x = macro_max(FIX_ZERO(0.0), geom_overscan_static.x);
    static const float geom_overscan_y = macro_max(FIX_ZERO(0.0), geom_overscan_static.y);
    static const float border_size = macro_clamp(border_size_static, 0.0, 0.5);           //  0.5 reaches to image center
    static const float border_darkness = macro_max(0.0, border_darkness_static);
    static const float border_compress = macro_max(1.0, border_compress_static);          //  < 1.0 darkens whole image
    static const float interlace_bff = float(interlace_bff_static);
    static const float interlace_1080i = float(interlace_1080i_static);
*/
#endif

/*
//  Provide accessors for vector constants that pack scalar uniforms:
float2 get_aspect_vector(const float geom_aspect_ratio)
{
    //  Get an aspect ratio vector.  Enforce geom_max_aspect_ratio, and prevent
    //  the absolute scale from affecting the uv-mapping for curvature:
    const float geom_clamped_aspect_ratio =
        min(geom_aspect_ratio, geom_max_aspect_ratio);
    const float2 geom_aspect =
        normalize(float2(geom_clamped_aspect_ratio, 1.0));
    return geom_aspect;
}

float2 get_geom_overscan_vector()
{
    return float2(geom_overscan_x, geom_overscan_y);
}

float2 get_geom_tilt_angle_vector()
{
    return float2(geom_tilt_angle_x, geom_tilt_angle_y);
}
*/
float3 get_convergence_offsets_x_vector()
{
    return float3(convergence_offset_x_r, convergence_offset_x_g,
        convergence_offset_x_b);
}

float3 get_convergence_offsets_y_vector()
{
    return float3(convergence_offset_y_r, convergence_offset_y_g,
        convergence_offset_y_b);
}

float2 get_convergence_offsets_r_vector()
{
    return float2(convergence_offset_x_r, convergence_offset_y_r);
}

float2 get_convergence_offsets_g_vector()
{
    return float2(convergence_offset_x_g, convergence_offset_y_g);
}

float2 get_convergence_offsets_b_vector()
{
    return float2(convergence_offset_x_b, convergence_offset_y_b);
}
/*
float2 get_aa_subpixel_r_offset()
{
    #ifdef RUNTIME_ANTIALIAS_WEIGHTS
        #ifdef RUNTIME_ANTIALIAS_SUBPIXEL_OFFSETS
            //  WARNING: THIS IS EXTREMELY EXPENSIVE.
            return float2(aa_subpixel_r_offset_x_runtime,
                aa_subpixel_r_offset_y_runtime);
        #else
            return aa_subpixel_r_offset_static;
        #endif
    #else
        return aa_subpixel_r_offset_static;
    #endif
}
*/
//  Provide accessors settings which still need "cooking:"
float get_mask_amplify()
{
    static const float mask_grille_amplify = 1.0/mask_grille_avg_color;
    static const float mask_slot_amplify = 1.0/mask_slot_avg_color;
    static const float mask_shadow_amplify = 1.0/mask_shadow_avg_color;
    return mask_type < 0.5 ? mask_grille_amplify :
        mask_type < 1.5 ? mask_slot_amplify :
        mask_shadow_amplify;
}

float get_mask_sample_mode()
{
    #ifdef RUNTIME_PHOSPHOR_MASK_MODE_TYPE_SELECT
        #ifdef PHOSPHOR_MASK_MANUALLY_RESIZE
            return mask_sample_mode_desired;
        #else
            return clamp(mask_sample_mode_desired, 1.0, 2.0);
        #endif
    #else
        #ifdef PHOSPHOR_MASK_MANUALLY_RESIZE
            return mask_sample_mode_static;
        #else
            return clamp(mask_sample_mode_static, 1.0, 2.0);
        #endif
    #endif
}


#endif  //  BIND_SHADER_PARAMS_H


