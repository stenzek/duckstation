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

#undef FIRST_PASS
//////////////////////////////////  INCLUDES  //////////////////////////////////

//#include "../include/user-settings.fxh"
//#include "../include/derived-settings-and-constants.fxh"
#include "../include/bind-shader-params.fxh"
#include "../include/scanline-functions.fxh"
//#include "../include/gamma-management.fxh"

/////////////////////////////////  STRUCTURES  /////////////////////////////////

struct out_vertex_p1
{
    //  Use explicit semantics so COLORx doesn't clamp values outside [0, 1].
    float2 tex_uv                   : TEXCOORD1;
    float2 uv_step                  : TEXCOORD2;    //  uv size of a texel (x) and scanline (y)
    float2 il_step_multiple         : TEXCOORD3;    //  (1, 1) = progressive, (1, 2) = interlaced
    float pixel_height_in_scanlines : TEXCOORD4;    //  Height of an output pixel in scanlines
};


////////////////////////////////  VERTEX SHADER  ///////////////////////////////

// Vertex shader generating a triangle covering the entire screen
void VS_Scanlines_Vertical_Interlacing(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out out_vertex_p1 OUT)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    OUT.tex_uv = texcoord;

    float2 texture_size = VERTICAL_SCANLINES_texture_size;
    float2 output_size  = float2(TEXTURE_SIZE.x, VIEWPORT_SIZE.y);

    //  Detect interlacing: il_step_multiple indicates the step multiple between
    //  lines: 1 is for progressive sources, and 2 is for interlaced sources.
//    const float2 video_size = 1.0/NormalizedNativePixelSize;
    const float y_step = 1.0 + float(is_interlaced(video_size.y));
    OUT.il_step_multiple = float2(1.0, y_step);
    //  Get the uv tex coords step between one texel (x) and scanline (y):
    OUT.uv_step = OUT.il_step_multiple / texture_size;

    //  If shader parameters are used, {min, max}_{sigma, shape} are runtime
    //  values.  Compute {sigma, shape}_range outside of scanline_contrib() so
    //  they aren't computed once per scanline (6 times per fragment and up to
    //  18 times per vertex):
/*    const float sigma_range = max(beam_max_sigma, beam_min_sigma) -
        beam_min_sigma;
    const float shape_range = max(beam_max_shape, beam_min_shape) -
        beam_min_shape;
*/
    //  We need the pixel height in scanlines for antialiased/integral sampling:
    const float ph = (video_size.y / output_size.y) / 
        OUT.il_step_multiple.y;
    OUT.pixel_height_in_scanlines = ph;

}


///////////////////////////////  FRAGMENT SHADER  //////////////////////////////

float4 PS_Scanlines_Vertical_Interlacing(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in out_vertex_p1 VAR) : SV_Target
{
    //  This pass: Sample multiple (misconverged?) scanlines to the final
    //  vertical resolution.  Temporarily auto-dim the output to avoid clipping.

    //  Read some attributes into local variables:
    const float2 texture_size = VERTICAL_SCANLINES_texture_size;
    const float2 texture_size_inv = 1.0/texture_size;
    const float2 uv_step = VAR.uv_step;
    const float2 il_step_multiple = VAR.il_step_multiple;
    const float frame_count = FrameCount;
    const float ph = VAR.pixel_height_in_scanlines;

    //  Get the uv coords of the previous scanline (in this field), and the
    //  scanline's distance from this sample, in scanlines.
    float dist;
    const float2 scanline_uv = get_last_scanline_uv(VAR.tex_uv, texture_size,
        texture_size_inv, il_step_multiple, frame_count, dist);

    //  Consider 2, 3, 4, or 6 scanlines numbered 0-5: The previous and next
    //  scanlines are numbered 2 and 3.  Get scanline colors colors (ignore
    //  horizontal sampling, since since IN.output_size.x = video_size.x).
    //  NOTE: Anisotropic filtering creates interlacing artifacts, which is why
    //  ORIG_LINEARIZED bobbed any interlaced input before this pass.
    const float2 v_step = float2(0.0, uv_step.y);
    const float3 scanline2_color = tex2D_linearize(ORIG_LINEARIZED, scanline_uv).rgb;
    const float3 scanline3_color =
        tex2D_linearize(ORIG_LINEARIZED, scanline_uv + v_step).rgb;
    float3 scanline0_color, scanline1_color, scanline4_color, scanline5_color,
        scanline_outside_color;
    float dist_round;
    //  Use scanlines 0, 1, 4, and 5 for a total of 6 scanlines:
    if(beam_num_scanlines > 5.5)
    {
        scanline1_color =
            tex2D_linearize(ORIG_LINEARIZED, scanline_uv - v_step).rgb;
        scanline4_color =
            tex2D_linearize(ORIG_LINEARIZED, scanline_uv + 2.0 * v_step).rgb;
        scanline0_color =
            tex2D_linearize(ORIG_LINEARIZED, scanline_uv - 2.0 * v_step).rgb;
        scanline5_color =
            tex2D_linearize(ORIG_LINEARIZED, scanline_uv + 3.0 * v_step).rgb;
    }
    //  Use scanlines 1, 4, and either 0 or 5 for a total of 5 scanlines:
    else if(beam_num_scanlines > 4.5)
    {
        scanline1_color =
            tex2D_linearize(ORIG_LINEARIZED, scanline_uv - v_step).rgb;
        scanline4_color =
            tex2D_linearize(ORIG_LINEARIZED, scanline_uv + 2.0 * v_step).rgb;
        //  dist is in [0, 1]
        dist_round = round(dist);
        const float2 sample_0_or_5_uv_off =
            lerp(-2.0 * v_step, 3.0 * v_step, dist_round);
        //  Call this "scanline_outside_color" to cope with the conditional
        //  scanline number:
        scanline_outside_color = tex2D_linearize(
            ORIG_LINEARIZED, scanline_uv + sample_0_or_5_uv_off).rgb;
    }
    //  Use scanlines 1 and 4 for a total of 4 scanlines:
    else if(beam_num_scanlines > 3.5)
    {
        scanline1_color =
            tex2D_linearize(ORIG_LINEARIZED, scanline_uv - v_step).rgb;
        scanline4_color =
            tex2D_linearize(ORIG_LINEARIZED, scanline_uv + 2.0 * v_step).rgb;
    }
    //  Use scanline 1 or 4 for a total of 3 scanlines:
    else if(beam_num_scanlines > 2.5)
    {
        //  dist is in [0, 1]
        dist_round = round(dist);
        const float2 sample_1or4_uv_off =
            lerp(-v_step, 2.0 * v_step, dist_round);
        scanline_outside_color = tex2D_linearize(
            ORIG_LINEARIZED, scanline_uv + sample_1or4_uv_off).rgb;
    }
    
    //  Compute scanline contributions, accounting for vertical convergence.
    //  Vertical convergence offsets are in units of current-field scanlines.
    //  dist2 means "positive sample distance from scanline 2, in scanlines:"
    float3 dist2 = dist.xxx;
    if(beam_misconvergence)
    {
        const float3 convergence_offsets_vert_rgb =
            get_convergence_offsets_y_vector();
        dist2 = dist.xxx - convergence_offsets_vert_rgb;
    }
    //  Calculate {sigma, shape}_range outside of scanline_contrib so it's only
    //  done once per pixel (not 6 times) with runtime params.  Don't reuse the
    //  vertex shader calculations, so static versions can be constant-folded.
    const float sigma_range = max(beam_max_sigma, beam_min_sigma) -
        beam_min_sigma;
    const float shape_range = max(beam_max_shape, beam_min_shape) -
        beam_min_shape;
    //  Calculate and sum final scanline contributions, starting with lines 2/3.
    //  There is no normalization step, because we're not interpolating a
    //  continuous signal.  Instead, each scanline is an additive light source.
    const float3 scanline2_contrib = scanline_contrib(dist2,
        scanline2_color, ph, sigma_range, shape_range);
    const float3 scanline3_contrib = scanline_contrib(abs(1.0.xxx - dist2),
        scanline3_color, ph, sigma_range, shape_range);
    float3 scanline_intensity = scanline2_contrib + scanline3_contrib;
    if(beam_num_scanlines > 5.5)
    {
        const float3 scanline0_contrib =
            scanline_contrib(dist2 + 2.0.xxx, scanline0_color,
                ph, sigma_range, shape_range);
        const float3 scanline1_contrib =
            scanline_contrib(dist2 + 1.0.xxx, scanline1_color,
                ph, sigma_range, shape_range);
        const float3 scanline4_contrib =
            scanline_contrib(abs(2.0.xxx - dist2), scanline4_color,
                ph, sigma_range, shape_range);
        const float3 scanline5_contrib =
            scanline_contrib(abs(3.0.xxx - dist2), scanline5_color,
                ph, sigma_range, shape_range);
        scanline_intensity += scanline0_contrib + scanline1_contrib +
            scanline4_contrib + scanline5_contrib;
    }
    else if(beam_num_scanlines > 4.5)
    {
        const float3 scanline1_contrib =
            scanline_contrib(dist2 + 1.0.xxx, scanline1_color,
                ph, sigma_range, shape_range);
        const float3 scanline4_contrib =
            scanline_contrib(abs(2.0.xxx - dist2), scanline4_color,
                ph, sigma_range, shape_range);
        const float3 dist0or5 = lerp(
            dist2 + 2.0.xxx, 3.0.xxx - dist2, dist_round);
        const float3 scanline0or5_contrib = scanline_contrib(
            dist0or5, scanline_outside_color, ph, sigma_range, shape_range);
        scanline_intensity += scanline1_contrib + scanline4_contrib +
            scanline0or5_contrib;
    }
    else if(beam_num_scanlines > 3.5)
    {
        const float3 scanline1_contrib =
            scanline_contrib(dist2 + 1.0.xxx, scanline1_color,
                ph, sigma_range, shape_range);
        const float3 scanline4_contrib =
            scanline_contrib(abs(2.0.xxx - dist2), scanline4_color,
                ph, sigma_range, shape_range);
        scanline_intensity += scanline1_contrib + scanline4_contrib;
    }
    else if(beam_num_scanlines > 2.5)
    {
        const float3 dist1or4 = lerp(
            dist2 + 1.0.xxx, 2.0.xxx - dist2, dist_round);
        const float3 scanline1or4_contrib = scanline_contrib(
            dist1or4, scanline_outside_color, ph, sigma_range, shape_range);
        scanline_intensity += scanline1or4_contrib;
    }

    //  Auto-dim the image to avoid clipping, encode if necessary, and output.
    //  My original idea was to compute a minimal auto-dim factor and put it in
    //  the alpha channel, but it wasn't working, at least not reliably.  This
    //  is faster anyway, levels_autodim_temp = 0.5 isn't causing banding.
    return encode_output(float4(scanline_intensity * levels_autodim_temp, 1.0));
}

