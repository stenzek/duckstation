#ifndef _SCANLINE_FUNCTIONS_H
#define _SCANLINE_FUNCTIONS_H

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


///////////////////////////////  BEGIN INCLUDES  ///////////////////////////////

#include "bind-shader-params.fxh"
#include "gamma-management.fxh"
#include "special-functions.fxh"

////////////////////////////////  END INCLUDES  ////////////////////////////////

/////////////////////////////  SCANLINE FUNCTIONS  /////////////////////////////

float2 round_coord(
	const float2 c,
	const float2 starting_position,
    const float2 bin_size
) {
	const float2 adj_c = c - starting_position;
	return c - fmod(adj_c, bin_size) + bin_size * 0.5;
}


// Use preproc defs for these, so they work for arbitrary choices of float1/2/3/4
#define triangle_wave(t, f) abs(1 - 2*frac((t) * (f)))

#define sawtooth_incr_wave(t, f) frac((t) * (f))

// using fmod(-t*f, 1.0) outputs 0 at t == 0, but I want it to output 1
#define sawtooth_decr_wave(t, f) 1 - frac((t) * (f))


struct InterpolationFieldData {
    float triangle_wave_freq;
    bool field_parity;
    bool scanline_parity;
    bool wrong_field;
};

InterpolationFieldData precalc_interpolation_field_data(float2 texcoord) {
    InterpolationFieldData data;
    
	data.triangle_wave_freq = 2;

	const float field_wave = triangle_wave(texcoord.y + rcp(2*data.triangle_wave_freq), data.triangle_wave_freq * 0.5) * 2 - 1;
    data.scanline_parity = field_wave >= 0;

    return data;
}

InterpolationFieldData calc_interpolation_field_data(float2 texcoord, float scale) {
    InterpolationFieldData data;
    
	data.triangle_wave_freq = scale * rcp(scanline_thickness);
	// data.triangle_wave_freq = content_size.y * rcp(scanline_thickness);

	const bool frame_count_parity = (frame_count % 2 == 1) && (scanline_deinterlacing_mode != 1);
    data.field_parity = (frame_count_parity && !interlace_back_field_first) || (!frame_count_parity && interlace_back_field_first);

	const float field_wave = triangle_wave(texcoord.y + rcp(2*data.triangle_wave_freq), data.triangle_wave_freq * 0.5) * 2 - 1;
    data.scanline_parity = field_wave >= 0;

	const bool wrong_field_raw = (data.scanline_parity && !data.field_parity) || (!data.scanline_parity && data.field_parity);
	data.wrong_field = enable_interlacing && wrong_field_raw;

    return data;
}

float get_gaussian_sigma(const float color, const float sigma_range)
{
    //  Requires:   Globals:
    //              1.) gaussian_beam_min_sigma and gaussian_beam_max_sigma are global floats
    //                  containing the desired minimum and maximum beam standard
    //                  deviations, for dim and bright colors respectively.
    //              2.) gaussian_beam_max_sigma must be > 0.0
    //              3.) gaussian_beam_min_sigma must be in (0.0, gaussian_beam_max_sigma]
    //              4.) gaussian_beam_spot_power must be defined as a global float.
    //              Parameters:
    //              1.) color is the underlying source color along a scanline
    //              2.) sigma_range = gaussian_beam_max_sigma - gaussian_beam_min_sigma; we take
    //                  sigma_range as a parameter to avoid repeated computation
    //                  when beam_{min, max}_sigma are runtime shader parameters
    //  Optional:   Users may set beam_spot_shape_function to 1 to define the
    //              inner f(color) subfunction (see below) as:
    //                  f(color) = sqrt(1.0 - (color - 1.0)*(color - 1.0))
    //              Otherwise (technically, if beam_spot_shape_function < 0.5):
    //                  f(color) = pow(color, gaussian_beam_spot_power)
    //  Returns:    The standard deviation of the Gaussian beam for "color:"
    //                  sigma = gaussian_beam_min_sigma + sigma_range * f(color)
    //  Details/Discussion:
    //  The beam's spot shape vaguely resembles an aspect-corrected f() in the
    //  range [0, 1] (not quite, but it's related).  f(color) = color makes
    //  spots look like diamonds, and a spherical function or cube balances
    //  between variable width and a soft/realistic shape.   A gaussian_beam_spot_power
    //  > 1.0 can produce an ugly spot shape and more initial clipping, but the
    //  final shape also differs based on the horizontal resampling filter and
    //  the phosphor bloom.  For instance, resampling horizontally in nonlinear
    //  light and/or with a sharp (e.g. Lanczos) filter will sharpen the spot
    //  shape, but a sixth root is still quite soft.  A power function (default
    //  1.0/3.0 gaussian_beam_spot_power) is most flexible, but a fixed spherical curve
    //  has the highest variability without an awful spot shape.
    //
    //  gaussian_beam_min_sigma affects scanline sharpness/aliasing in dim areas, and its
    //  difference from gaussian_beam_max_sigma affects beam width variability.  It only
    //  affects clipping [for pure Gaussians] if gaussian_beam_spot_power > 1.0 (which is
    //  a conservative estimate for a more complex constraint).
    //
    //  gaussian_beam_max_sigma affects clipping and increasing scanline width/softness
    //  as color increases.  The wider this is, the more scanlines need to be
    //  evaluated to avoid distortion.  For a pure Gaussian, the max_beam_sigma
    //  at which the first unused scanline always has a weight < 1.0/255.0 is:
    //      num scanlines = 2, max_beam_sigma = 0.2089; distortions begin ~0.34
    //      num scanlines = 3, max_beam_sigma = 0.3879; distortions begin ~0.52
    //      num scanlines = 4, max_beam_sigma = 0.5723; distortions begin ~0.70
    //      num scanlines = 5, max_beam_sigma = 0.7591; distortions begin ~0.89
    //      num scanlines = 6, max_beam_sigma = 0.9483; distortions begin ~1.08
    //  Generalized Gaussians permit more leeway here as steepness increases.
    if(beam_spot_shape_function < 0.5)
    {
        //  Use a power function:
        return gaussian_beam_min_sigma + sigma_range * pow(color, gaussian_beam_spot_power);
    }
    else
    {
        //  Use a spherical function:
        const float color_minus_1 = color - 1;
        return gaussian_beam_min_sigma + sigma_range * sqrt(1.0 - color_minus_1*color_minus_1);
    }
}

float get_generalized_gaussian_beta(const float color, const float shape_range)
{
    //  Requires:   Globals:
    //              1.) gaussian_beam_min_shape and gaussian_beam_max_shape are global floats
    //                  containing the desired min/max generalized Gaussian
    //                  beta parameters, for dim and bright colors respectively.
    //              2.) gaussian_beam_max_shape must be >= 2.0
    //              3.) gaussian_beam_min_shape must be in [2.0, gaussian_beam_max_shape]
    //              4.) gaussian_beam_shape_power must be defined as a global float.
    //              Parameters:
    //              1.) color is the underlying source color along a scanline
    //              2.) shape_range = gaussian_beam_max_shape - gaussian_beam_min_shape; we take
    //                  shape_range as a parameter to avoid repeated computation
    //                  when beam_{min, max}_shape are runtime shader parameters
    //  Returns:    The type-I generalized Gaussian "shape" parameter beta for
    //              the given color.
    //  Details/Discussion:
    //  Beta affects the scanline distribution as follows:
    //  a.) beta < 2.0 narrows the peak to a spike with a discontinuous slope
    //  b.) beta == 2.0 just degenerates to a Gaussian
    //  c.) beta > 2.0 flattens and widens the peak, then drops off more steeply
    //      than a Gaussian.  Whereas high sigmas widen and soften peaks, high
    //      beta widen and sharpen peaks at the risk of aliasing.
    //  Unlike high gaussian_beam_spot_powers, high gaussian_beam_shape_powers actually soften shape
    //  transitions, whereas lower ones sharpen them (at the risk of aliasing).
    return gaussian_beam_min_shape + shape_range * pow(color, gaussian_beam_shape_power);
}

float3 get_raw_interpolated_color(const float3 color0,
    const float3 color1, const float3 color2, const float3 color3,
    const float4 weights)
{
    //  Use max to avoid bizarre artifacts from negative colors:
    const float4x3 mtrx = float4x3(color0, color1, color2, color3);
    const float3 m = mul(weights, mtrx);
    return max(m, 0.0);
}

float3 get_interpolated_linear_color(const float3 color0, const float3 color1,
    const float3 color2, const float3 color3, const float4 weights)
{
    //  Requires:   1.) Requirements of include/gamma-management.h must be met:
    //                  intermediate_gamma must be globally defined, and input
    //                  colors are interpreted as linear RGB unless you #define
    //                  GAMMA_ENCODE_EVERY_FBO (in which case they are
    //                  interpreted as gamma-encoded with intermediate_gamma).
    //              2.) color0-3 are colors sampled from a texture with tex2D().
    //                  They are interpreted as defined in requirement 1.
    //              3.) weights contains weights for each color, summing to 1.0.
    //              4.) beam_horiz_linear_rgb_weight must be defined as a global
    //                  float in [0.0, 1.0] describing how much blending should
    //                  be done in linear RGB (rest is gamma-corrected RGB).
    //              5.) _RUNTIME_SCANLINES_HORIZ_FILTER_COLORSPACE must be #defined
    //                  if beam_horiz_linear_rgb_weight is anything other than a
    //                  static constant, or we may try branching at runtime
    //                  without dynamic branches allowed (slow).
    //  Returns:    Return an interpolated color lookup between the four input
    //              colors based on the weights in weights.  The final color will
    //              be a linear RGB value, but the blending will be done as
    //              indicated above.
    const float intermediate_gamma = get_intermediate_gamma();
    const float inv_intermediate_gamma = 1.0 / intermediate_gamma;
    //  Branch if beam_horiz_linear_rgb_weight is static (for free) or if the
    //  profile allows dynamic branches (faster than computing extra pows):
    #if !_RUNTIME_SCANLINES_HORIZ_FILTER_COLORSPACE
        #define SCANLINES_BRANCH_FOR_LINEAR_RGB_WEIGHT
    #else
        #if _DRIVERS_ALLOW_DYNAMIC_BRANCHES
            #define SCANLINES_BRANCH_FOR_LINEAR_RGB_WEIGHT
        #endif
    #endif
    #ifdef SCANLINES_BRANCH_FOR_LINEAR_RGB_WEIGHT
        //  beam_horiz_linear_rgb_weight is static, so we can branch:
        #ifdef GAMMA_ENCODE_EVERY_FBO
            const float3 gamma_mixed_color = pow(
                get_raw_interpolated_color(color0, color1, color2, color3, weights),
                intermediate_gamma);
            if(beam_horiz_linear_rgb_weight > 0.0)
            {
                const float3 linear_mixed_color = get_raw_interpolated_color(
                    pow(color0, intermediate_gamma),
                    pow(color1, intermediate_gamma),
                    pow(color2, intermediate_gamma),
                    pow(color3, intermediate_gamma),
                    weights);
                return lerp(gamma_mixed_color, linear_mixed_color, beam_horiz_linear_rgb_weight);
            }
            else
            {
                return gamma_mixed_color;
            }
        #else
            const float3 linear_mixed_color = get_raw_interpolated_color(
                color0, color1, color2, color3, weights);
            if(beam_horiz_linear_rgb_weight < 1.0)
            {
                const float3 gamma_mixed_color = get_raw_interpolated_color(
                    pow(color0, inv_intermediate_gamma),
                    pow(color1, inv_intermediate_gamma),
                    pow(color2, inv_intermediate_gamma),
                    pow(color3, inv_intermediate_gamma),
                    weights);
                return lerp(gamma_mixed_color, linear_mixed_color, beam_horiz_linear_rgb_weight);
            }
            else
            {
                return linear_mixed_color;
            }
        #endif  //  GAMMA_ENCODE_EVERY_FBO
    #else
        #ifdef GAMMA_ENCODE_EVERY_FBO
            //  Inputs: color0-3 are colors in gamma-encoded RGB.
            const float3 gamma_mixed_color = pow(get_raw_interpolated_color(
                color0, color1, color2, color3, weights), intermediate_gamma);
            const float3 linear_mixed_color = get_raw_interpolated_color(
                pow(color0, intermediate_gamma),
                pow(color1, intermediate_gamma),
                pow(color2, intermediate_gamma),
                pow(color3, intermediate_gamma),
                weights);
            return lerp(gamma_mixed_color, linear_mixed_color, beam_horiz_linear_rgb_weight);
        #else
            //  Inputs: color0-3 are colors in linear RGB.
            const float3 linear_mixed_color = get_raw_interpolated_color(
                color0, color1, color2, color3, weights);
            const float3 gamma_mixed_color = get_raw_interpolated_color(
                    pow(color0, inv_intermediate_gamma),
                    pow(color1, inv_intermediate_gamma),
                    pow(color2, inv_intermediate_gamma),
                    pow(color3, inv_intermediate_gamma),
                    weights);
            // wtf fixme
//			const float beam_horiz_linear_rgb_weight1 = 1.0;
            return lerp(gamma_mixed_color, linear_mixed_color,
                beam_horiz_linear_rgb_weight);
        #endif  //  GAMMA_ENCODE_EVERY_FBO
    #endif  //  SCANLINES_BRANCH_FOR_LINEAR_RGB_WEIGHT
}

float3 get_scanline_color(const sampler2D tex, const float2 scanline_uv,
    const float2 uv_step_x, const float4 weights)
{
    //  Requires:   1.) scanline_uv must be vertically snapped to the caller's
    //                  desired line or scanline and horizontally snapped to the
    //                  texel just left of the output pixel (color1)
    //              2.) uv_step_x must contain the horizontal uv distance
    //                  between texels.
    //              3.) weights must contain interpolation filter weights for
    //                  color0, color1, color2, and color3, where color1 is just
    //                  left of the output pixel.
    //  Returns:    Return a horizontally interpolated texture lookup using 2-4
    //              nearby texels, according to weights and the conventions of
    //              get_interpolated_linear_color().
    //  We can ignore the outside texture lookups for Quilez resampling.
    const float3 color1 = tex2D_linearize(tex, scanline_uv, get_input_gamma()).rgb;
    const float3 color2 = tex2D_linearize(tex, scanline_uv + uv_step_x, get_input_gamma()).rgb;
    float3 color0 = float3(0.0, 0.0, 0.0);
    float3 color3 = float3(0.0, 0.0, 0.0);
    if(beam_horiz_filter > 0.5)
    {
        color0 = tex2D_linearize(tex, scanline_uv - uv_step_x, get_input_gamma()).rgb;
        color3 = tex2D_linearize(tex, scanline_uv + 2.0 * uv_step_x, get_input_gamma()).rgb;
    }
    //  Sample the texture as-is, whether it's linear or gamma-encoded:
    //  get_interpolated_linear_color() will handle the difference.
    return get_interpolated_linear_color(color0, color1, color2, color3, weights);
}

float3 sample_single_scanline_horizontal(const sampler2D tex,
    const float2 tex_uv, const float2 tex_size,
    const float2 texture_size_inv)
{
    //  TODO: Add function requirements.
    //  Snap to the previous texel and get sample dists from 2/4 nearby texels:
    const float2 curr_texel = tex_uv * tex_size;
    //  Use under_half to fix a rounding bug right around exact texel locations.
    const float2 prev_texel = floor(curr_texel - under_half) +  0.5;
    const float2 prev_texel_hor = float2(prev_texel.x, curr_texel.y);
    const float2 prev_texel_hor_uv = prev_texel_hor * texture_size_inv;
    const float prev_dist = curr_texel.x - prev_texel_hor.x;
    const float4 sample_dists = float4(1.0 + prev_dist, prev_dist,
        1.0 - prev_dist, 2.0 - prev_dist);
    //  Get Quilez, Lanczos2, or Gaussian resize weights for 2/4 nearby texels:
    float4 weights;
    if (beam_horiz_filter < 0.5) {
        //  None:
        weights = float4(0, 1, 0, 0);
    }
    else if(beam_horiz_filter < 1.5)
    {
        //  Quilez:
        const float x = sample_dists.y;
        const float w2 = x*x*x*(x*(x*6.0 - 15.0) + 10.0);
        weights = float4(0.0, 1.0 - w2, w2, 0.0);
    }
    else if(beam_horiz_filter < 2.5)
    {
        //  Gaussian:
        float inner_denom_inv = 1.0/(2.0*beam_horiz_sigma*beam_horiz_sigma);
        weights = exp(-(sample_dists*sample_dists)*inner_denom_inv);
    }
    else
    {
        //  Lanczos2:
        const float4 pi_dists = FIX_ZERO(sample_dists * pi);
        weights = 2.0 * sin(pi_dists) * sin(pi_dists * 0.5) /
            (pi_dists * pi_dists);
    }
    //  Ensure the weight sum == 1.0:
    const float4 final_weights = weights/dot(weights, float4(1.0, 1.0, 1.0, 1.0));
    //  Get the interpolated horizontal scanline color:
    const float2 uv_step_x = float2(texture_size_inv.x, 0.0);
    return get_scanline_color(
        tex, prev_texel_hor_uv, uv_step_x, final_weights);
}

float3 sample_rgb_scanline(
    const sampler2D tex,
    const float2 tex_uv, const float2 tex_size,
    const float2 texture_size_inv
) {
    if (beam_misconvergence) {
        const float3 convergence_offsets_rgb_x = get_convergence_offsets_x_vector();
        const float3 convergence_offsets_rgb_y = get_convergence_offsets_y_vector();

        const float3 offset_u_rgb = convergence_offsets_rgb_x * texture_size_inv.x;
        const float3 offset_v_rgb = convergence_offsets_rgb_y * texture_size_inv.y;

        const float2 scanline_uv_r = tex_uv - float2(offset_u_rgb.r, offset_v_rgb.r);
        const float2 scanline_uv_g = tex_uv - float2(offset_u_rgb.g, offset_v_rgb.g);
        const float2 scanline_uv_b = tex_uv - float2(offset_u_rgb.b, offset_v_rgb.b);
        
        /**/
        const float4 sample_r = tex2D(tex, scanline_uv_r);
        const float4 sample_g = tex2D(tex, scanline_uv_g);
        const float4 sample_b = tex2D(tex, scanline_uv_b);
        /**/

        /*
        const float3 sample_r = sample_single_scanline_horizontal(
            tex, scanline_uv_r, tex_size, texture_size_inv);
        const float3 sample_g = sample_single_scanline_horizontal(
            tex, scanline_uv_g, tex_size, texture_size_inv);
        const float3 sample_b = sample_single_scanline_horizontal(
            tex, scanline_uv_b, tex_size, texture_size_inv);
        */

        return float3(sample_r.r, sample_g.g, sample_b.b);
    }
    else {
        // return tex2D(tex, tex_uv).rgb;
        return sample_single_scanline_horizontal(tex, tex_uv, tex_size, texture_size_inv);
    }
}

float3 sample_rgb_scanline_horizontal(const sampler2D tex,
    const float2 tex_uv, const float2 tex_size,
    const float2 texture_size_inv)
{
    //  TODO: Add function requirements.
    //  Rely on a helper to make convergence easier.
    if(beam_misconvergence)
    {
        const float3 convergence_offsets_rgb = get_convergence_offsets_x_vector();
        const float3 offset_u_rgb = convergence_offsets_rgb * texture_size_inv.xxx;
        const float2 scanline_uv_r = tex_uv - float2(offset_u_rgb.r, 0.0);
        const float2 scanline_uv_g = tex_uv - float2(offset_u_rgb.g, 0.0);
        const float2 scanline_uv_b = tex_uv - float2(offset_u_rgb.b, 0.0);
        const float3 sample_r = sample_single_scanline_horizontal(
            tex, scanline_uv_r, tex_size, texture_size_inv);
        const float3 sample_g = sample_single_scanline_horizontal(
            tex, scanline_uv_g, tex_size, texture_size_inv);
        const float3 sample_b = sample_single_scanline_horizontal(
            tex, scanline_uv_b, tex_size, texture_size_inv);
        return float3(sample_r.r, sample_g.g, sample_b.b);
    }
    else
    {
        return sample_single_scanline_horizontal(tex, tex_uv, tex_size, texture_size_inv);
    }
}

float3 get_averaged_scanline_sample(
    sampler2D tex, const float2 texcoord,
    const float scanline_start_y, const float v_step_y,
    const float input_gamma
) {
    // Sample `scanline_thickness` vertically-contiguous pixels and average them.
    float3 interpolated_line = 0.0;
    for (int i = 0; i < scanline_thickness; i++) {
        float4 coord = float4(texcoord.x, scanline_start_y + i * v_step_y, 0, 0);
        interpolated_line += tex2Dlod_linearize(tex, coord, input_gamma).rgb;
    }
    interpolated_line /= float(scanline_thickness);

    return interpolated_line;
}

float get_beam_strength(float dist, float color,
    const float sigma_range, const float shape_range)
{
    // entry point in original is scanline_contrib()
    // this is based on scanline_gaussian_sampled_contrib() from original

    //  See scanline_gaussian_integral_contrib() for detailed comments!
    //  gaussian sample = 1/(sigma*sqrt(2*pi)) * e**(-(x**2)/(2*sigma**2))
    const float sigma = get_gaussian_sigma(color, sigma_range);
    //  Avoid repeated divides:
    const float sigma_inv = 1.0 / sigma;
    const float inner_denom_inv = 0.5 * sigma_inv * sigma_inv;
    const float outer_denom_inv = sigma_inv/sqrt(2.0*pi);

    return color*exp(-(dist*dist)*inner_denom_inv)*outer_denom_inv;
}

float get_gaussian_beam_strength(
    float dist,
    float color,
    const float sigma_range,
    const float shape_range
) {
    // entry point in original is scanline_contrib()
    // this is based on scanline_generalized_gaussian_sampled_contrib() from original

    //  See scanline_generalized_gaussian_integral_contrib() for details!
    //  generalized sample =
    //      beta/(2*alpha*gamma(1/beta)) * e**(-(|x|/alpha)**beta)
    const float alpha = sqrt(2.0) * get_gaussian_sigma(color, sigma_range);
    const float beta = get_generalized_gaussian_beta(color, shape_range);
    //  Avoid repeated divides:
    const float alpha_inv = 1.0 / alpha;
    const float beta_inv = 1.0 / beta;
    const float scale = color * beta * 0.5 * alpha_inv / gamma_impl(beta_inv, beta);
    
    return scale * exp(-pow(abs(dist*alpha_inv), beta));
}

float get_linear_beam_strength(
    const float dist,
    const float color,
    const float num_pixels,
    const bool interlaced
) {
    const float p = color * (1 - abs(dist));
    return clamp(p, 0, color);
}


#endif  //  _SCANLINE_FUNCTIONS_H