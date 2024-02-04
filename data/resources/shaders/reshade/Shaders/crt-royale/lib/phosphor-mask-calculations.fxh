#ifndef _PHOSHOR_MASK_CALCULATIONS_H
#define _PHOSHOR_MASK_CALCULATIONS_H

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


/*
 *  Our goal is to use arithmetic to generate the phosphor mask.
 *  Phosphor masks are regular patterns, so we want something periodic.
 *  We need to avoid integer arithmetic because it tends to cause rounding errors.
 *  
 *  For all masks, we want to approximate a pulse wave in at least one dimension. This pulse wave
 *  will have narrow peaks, wide troughs, and constant periodicity.
 *    GRILLE will have a pulse wave along the x-axis and will be constant along the y-axis.
 *    SLOT and SHADOW will likely have a superposition of two out-of-phase pulse waves along each axis.
 *      For SHADOW, the width of the peaks will vary such that they generate ellipsoids on the screen.
 *
 *  We can get a periodic function by starting with a triangle wave: T(t, f) = abs(1 - 2*frac(t * f)).
 *    This function gives us a triangle wave with f cycles in the domain [0, 1].
 *    Note that T(0, f) = 1.
 *
 *  Then we can compose this with a sigmoid curve to squish the triangle wave into a pulse wave.
 *     P(s, p, q) = exp(q s - q/2) / (exp(q s - q/2) + exp(-p))
 *     s(t, f, o) = T(t*f - o, 1)
 *
 *     f is the number of pulses to render along the given axis.
 *     o is the channel's horizontal ofset along the given axis, normalized via the quotient raw_offset / raw_triad width.
 *     p and q control how closely P resembles an ideal pulse wave and also how wide the peaks and troughs are.
 *
 *  The interaction between p and q is rather complicated and difficult to describe, so they're not a good pair
 *  of parameters for users. But we have the info necessary to solve for p in terms of q.
 *    We know the width of a phosphor and the width of a triad, and we know the domain and range of P.
 *    We can choose a coordinate (t0, y0) that will denote the edge of the phosphor.
 *      Note that y0 = P(t0, p, q) for some p and q.
 *    We let t0 = raw_phosphor_width / raw_triad_width, since we need to respect the shape of the phosphor.
 *    We let the user define P(t0).
 *      Technically, this means the user is defining the brightness of the phosphor's furthest edge.
 *      Visually, this looks like the user is defining the width of the phosphor.
 *      We'll call this the Phosphor Thickness.
 *   We let the user define q.
 *      Technically, this means the user is defining the squareness of the pulse wave.
 *      Visually, this looks like the user is defining the sharpness of the phosphor.
 *      We'll call this the Phosphor Sharpness.
 *
 *   We can solve for p in terms of q very efficiently.
 *     p = (ln(y0 / (1 - y0)) - q) / (0.5 - 2 t0)
 *
 *     Note that, if you work through the algebra, you get a denominator of (t0 - 0.5).
 *       Using (0.5 - 2 t0) actually works better. It also matches up when you try plotting P and (t0, y0).
 *
 *  For the GRILLE and SLOT masks, we can compute p once and recycle it.
 *  For the SHADOW mask, we can either compute p on each iteration or find a way to interpolate between min_p and max_p.
 *
 *  One might expect it'd be way better to use a clamped triangle wave rather than a sigmoid or exponentiated cosine wave. 
 *  As far as I can tell, this ends up being incorrect surprisingly enough. Although it's a good bit faster,
 *  it has terrible aliasing artifacts at small scales. The other implementations are slower, but they produce
 *  evenly-sized RGB phosphors for a variety of configurations even when the triad width is 3 pixels. At that
 *  scale, the triangle wave approach produces triads where one of the phosphors is thicker than the others.
 *  Taking into account the compute_mask_factor trick, the triangle wave approach would be a negligible
 *  performance improvement at the cost of a large drop in visual quality and user friendliness.
 */


#include "bind-shader-params.fxh"
#include "scanline-functions.fxh"

/*
 *  The GRILLE mask consists of an array of vertical stripes, so each channel will vary along the x-axis and will be constant
 *  along the y-axis.
 *
 *  It has the following dimensions:
 *    Phosphors are 18 units wide with unbounded height.
 *    Phosphors in a triad are 2 units apart.
 *    Triads are 6 units apart.
 *    Triad centers are 64 units apart.
 *    The phosphors follow an RGB pattern.
 *    The left-most phosphor is red and offset by 3 units to the right.
 */
static const float grille_raw_phosphor_width = 18;
static const float grille_raw_phosphor_gap = 2;
static const float grille_raw_triad_horiz_gap = 6;
static const float grille_raw_triad_width = 3*grille_raw_phosphor_width + 2*grille_raw_phosphor_gap + grille_raw_triad_horiz_gap;

static const float grille_raw_r_offset = (grille_raw_triad_horiz_gap + grille_raw_phosphor_width) / 2;
static const float grille_raw_g_offset = grille_raw_r_offset + grille_raw_phosphor_width + grille_raw_phosphor_gap;
static const float grille_raw_b_offset = grille_raw_g_offset + grille_raw_phosphor_width + grille_raw_phosphor_gap;
static const float3 grille_norm_center_offsets = float3(
    grille_raw_r_offset,
    grille_raw_g_offset,
    grille_raw_b_offset
) / grille_raw_triad_width;

static const float grille_edge_t = grille_raw_phosphor_width / 2;
static const float grille_edge_norm_t = grille_edge_t / grille_raw_triad_width;


/*
 *  The SLOT mask consists of an array of rectangles, so each channel will vary along both the x- and y-axes.
 *
 *  It has the following dimensions:
 *    Phosphors are 18 units wide and 66 units tall.
 *    Phosphors in a triad are 2 units apart.
 *    Triads are 6 units apart horizontally and 6 units apart vertically.
 *    Triad centers are 64 units apart horizontally and 73 units apart vertically.
 *    The phosphors follow an RGB pattern.
 *    The upper-left-most phosphor is red and offset by 3 units to the right and 3 units down.
 */
static const float slot_raw_phosphor_width = 18;
static const float slot_raw_phosphor_gap = 2;
static const float slot_raw_triad_horiz_gap = 6;
static const float slot_raw_triad_width = 3*slot_raw_phosphor_width + 2*slot_raw_phosphor_gap + slot_raw_triad_horiz_gap;

static const float slot_raw_phosphor_height = 66;
static const float slot_raw_triad_vert_gap = 6;
static const float slot_raw_triad_height = slot_raw_phosphor_height + slot_raw_triad_vert_gap;

static const float slot_aspect_ratio = slot_raw_triad_height / slot_raw_triad_width;

static const float slot_raw_r_offset_x = (slot_raw_triad_horiz_gap + slot_raw_phosphor_width) / 2;
static const float slot_raw_g_offset_x = slot_raw_r_offset_x + slot_raw_phosphor_width + slot_raw_phosphor_gap;
static const float slot_raw_b_offset_x = slot_raw_g_offset_x + slot_raw_phosphor_width + slot_raw_phosphor_gap;
static const float3 slot_norm_center_offsets_x = float3(
    slot_raw_r_offset_x,
    slot_raw_g_offset_x,
    slot_raw_b_offset_x
) / slot_raw_triad_width;
static const float3 slot_norm_center_offsets_y = float3(0.5, 0.5, 0.5);

static const float slot_edge_tx = slot_raw_phosphor_width / 2;
// We draw the slot mask as two sets of columns. To do that, we have to pretend the horizontal gap is the size of a whole triad.
//   Then we need to halve the position of the phosphor edge.
static const float slot_edge_norm_tx = 0.5 * slot_edge_tx / slot_raw_triad_width;
static const float slot_edge_ty = slot_raw_phosphor_height / 2;
static const float slot_edge_norm_ty = slot_edge_ty / slot_raw_triad_height;

/*
 *  The SHADOW mask consists of an array of circles, so each channel will vary along both the x- and y-axes.
 *
 *  It has the following dimensions:
 *    Phosphors are 21 units in diameter.
 *    All phosphors are 0 units apart.
 *    Triad centers are 63 units apart horizontally and 21 units apart vertically.
 *    The phosphors follow a GBR pattern on odd rows and RBG on even rows.
 *    The upper-left-most phosphor is green and centered on the corner of the screen.
 */
static const float shadow_raw_phosphor_diam = 21;
static const float shadow_raw_phosphor_gap = 0;
static const float shadow_raw_triad_horiz_gap = 0;
static const float shadow_raw_triad_vert_gap = 0;

static const float shadow_raw_triad_width = 3*shadow_raw_phosphor_diam + 2*shadow_raw_phosphor_gap + shadow_raw_triad_horiz_gap;
static const float shadow_raw_triad_height = shadow_raw_phosphor_diam + shadow_raw_triad_vert_gap;

static const float shadow_aspect_ratio = shadow_raw_triad_height / shadow_raw_triad_width;

static const float shadow_raw_g_offset_x = 0;
static const float shadow_raw_b_offset_x = shadow_raw_g_offset_x + shadow_raw_phosphor_diam + shadow_raw_phosphor_gap;
static const float shadow_raw_r_offset_x = shadow_raw_b_offset_x + shadow_raw_phosphor_diam + shadow_raw_phosphor_gap;
static const float3 shadow_norm_center_offsets_x = float3(
    shadow_raw_r_offset_x,
    shadow_raw_g_offset_x,
    shadow_raw_b_offset_x
) / shadow_raw_triad_width;

static const float3 shadow_norm_center_offsets_y = float3(0.0, 0.0, 0.0);

static const float shadow_edge_tx = shadow_raw_phosphor_diam / 2;
static const float shadow_edge_norm_tx = shadow_edge_tx / shadow_raw_triad_width;
static const float shadow_edge_ty = shadow_raw_phosphor_diam / 2;
// We draw the shadow mask as two sets of rows. To do that, we have to pretend the vertical gap is the size of a whole triad.
//   Then we need to halve the position of the phosphor edge.
static const float shadow_edge_norm_ty = 0.5 * shadow_edge_ty / shadow_raw_triad_height;
static const float shadow_norm_phosphor_rad = (shadow_raw_phosphor_diam/2) / shadow_raw_triad_width;


/*
 *  The SMALL GRILLE mask is composed of magenta and green stripes.
 *  Sourced from http://filthypants.blogspot.com/2020/02/crt-shader-masks.html
 *  
 *  It has the following dimensions:
 *    Stripes are 32 units wide.
 *    Stripes in a triad are 0 units apart.
 *    Triads are 0 units apart horizontally.
 *    
 *  Each triad has two quads, side-by-side and aligned.
 *    Neighboring triads are offset vertically.
 *    Below is an array of 2 triads.
 *    x's denote magenta stripes, and o's denote green ones.
 *
 *      xxooxxoo
 *      xxooxxoo
 *      xxooxxoo
 *      xxooxxoo
 *      xxooxxoo
 *      xxooxxoo
 *
 *    The phosphors follow a MG pattern.
 *    The left-most phosphor is magenta and offset by 16 units to the right.
 */

static const float smallgrille_raw_stripe_width = 32;
static const float smallgrille_raw_triad_width = 2*smallgrille_raw_stripe_width;

static const float smallgrille_raw_r_offset_x = 0.5 * smallgrille_raw_stripe_width;
static const float smallgrille_raw_g_offset_x = smallgrille_raw_r_offset_x + smallgrille_raw_stripe_width;
static const float smallgrille_raw_b_offset_x = smallgrille_raw_r_offset_x;
static const float3 smallgrille_norm_center_offsets_x = float3(
    smallgrille_raw_r_offset_x,
    smallgrille_raw_g_offset_x,
    smallgrille_raw_b_offset_x
) / smallgrille_raw_triad_width;

static const float smallgrille_edge_t = 0.5 * smallgrille_raw_stripe_width;
static const float smallgrille_edge_norm_t = smallgrille_edge_t / smallgrille_raw_triad_width;


/*
 *  The SMALL SLOT mask is composed of magenta and green quads.
 *  Sourced from http://filthypants.blogspot.com/2020/02/crt-shader-masks.html
 *  
 *  It has the following dimensions:
 *    Quads are 32 units wide and 48 units tall.
 *    Quads in a triad are 0 units apart.
 *    Triads are 0 units apart horizontally and 16 units apart vertically.
 *    
 *  Each triad has two quads, side-by-side and aligned.
 *    Neighboring triads are offset vertically.
 *    Below is a 2x2 matrix of 4 triads.
 *    x's denote magenta quads, and o's denote green ones.
 *
 *      xxoo    
 *      xxooxxoo
 *      xxooxxoo
 *          xxoo
 *      xxoo    
 *      xxooxxoo
 *      xxooxxoo
 *          xxoo
 *
 *    The phosphors follow a MG pattern.
 *    The upper-left-most phosphor is magenta and offset by 16 units to the right and 16 units down.
 */

static const float smallslot_raw_quad_width = 32;
static const float smallslot_raw_triad_width = 2*smallslot_raw_quad_width;

static const float smallslot_raw_quad_height = 1.5 * smallslot_raw_quad_width;
static const float smallslot_raw_triad_vert_gap = 0.5 * smallslot_raw_quad_width;
static const float smallslot_raw_triad_height = smallslot_raw_quad_height + smallslot_raw_triad_vert_gap;

static const float smallslot_aspect_ratio = smallslot_raw_triad_height / smallslot_raw_triad_width;

static const float smallslot_raw_r_offset_x = 0.5 * smallslot_raw_quad_width;
static const float smallslot_raw_g_offset_x = smallslot_raw_r_offset_x + smallslot_raw_quad_width;
static const float smallslot_raw_b_offset_x = smallslot_raw_r_offset_x;
static const float3 smallslot_norm_center_offsets_x = float3(
    smallslot_raw_r_offset_x,
    smallslot_raw_g_offset_x,
    smallslot_raw_b_offset_x
) / smallslot_raw_triad_width;

static const float3 smallslot_norm_center_offsets_y1 = 0.5 * smallslot_raw_quad_height / smallslot_raw_triad_height;
static const float3 smallslot_norm_center_offsets_y2 = smallslot_norm_center_offsets_y1 + smallslot_raw_triad_vert_gap / smallslot_raw_triad_height;

static const float smallslot_edge_tx = 0.5 * smallslot_raw_quad_width;
// We draw the slot mask as two sets of columns. To do that, we have to pretend the horizontal gap is the size of a whole triad.
//   Then we need to halve the position of the phosphor edge.
static const float smallslot_edge_norm_tx = 0.5 * smallslot_edge_tx / smallslot_raw_triad_width;
static const float smallslot_edge_ty = smallslot_raw_quad_height / 2;
static const float smallslot_edge_norm_ty = smallslot_edge_ty / smallslot_raw_triad_height;

/*
 *  The SMALL SHADOW mask is composed of magenta and green quads.
 *  Sourced from http://filthypants.blogspot.com/2020/02/crt-shader-masks.html
 *  
 *  It has the following dimensions:
 *    Quads are 17 units wide and 17 units tall.
 *    Quads in a triad are 0 units apart.
 *    Triads are 0 units apart horizontally and 0 units apart vertically.
 *    
 *  Each triad has two quads, side-by-side and aligned.
 *    Neighboring triads are offset vertically.
 *    Below is a 2x2 matrix of 4 triads.
 *    x's denote magenta quads, and o's denote green ones.
 *
 *      xxooxxoo
 *      xxooxxoo
 *      ooxxooxx
 *      ooxxooxx
 *
 *    The phosphors follow a MG pattern.
 *    The upper-left-most phosphor is magenta and offset by 16 units to the right and 16 units down.
 */

static const float smallshadow_raw_quad_width = 17;
static const float smallshadow_raw_triad_width = 2 * smallshadow_raw_quad_width;

static const float smallshadow_raw_quad_height = 17;
static const float smallshadow_raw_triad_height = smallshadow_raw_quad_height;

static const float smallshadow_aspect_ratio = smallshadow_raw_triad_height / smallshadow_raw_triad_width;

static const float smallshadow_raw_r_offset_x = 0.5 * smallshadow_raw_quad_width;
static const float smallshadow_raw_g_offset_x = smallshadow_raw_r_offset_x + smallshadow_raw_quad_width;
static const float smallshadow_raw_b_offset_x = smallshadow_raw_r_offset_x;
static const float3 smallshadow_norm_center_offsets_x = float3(
    smallshadow_raw_r_offset_x,
    smallshadow_raw_g_offset_x,
    smallshadow_raw_b_offset_x
) / smallshadow_raw_triad_width;

static const float3 smallshadow_norm_center_offsets_y = 0.5 * smallshadow_raw_triad_height;

static const float smallshadow_edge_tx = 0.5 * smallshadow_raw_quad_width;
static const float smallshadow_edge_norm_tx = smallshadow_edge_tx / smallshadow_raw_triad_width;
static const float smallshadow_edge_ty = 0.5 * smallshadow_raw_quad_height;
// We draw the shadow mask as two sets of rows. To do that, we have to pretend the vertical gap is the size of a whole triad.
//   Then we need to halve the position of the phosphor edge.
static const float smallshadow_edge_norm_ty = 0.5 * smallshadow_edge_ty / smallshadow_raw_triad_height;




float get_selected_aspect_ratio() {
    float aspect_ratio;
    [flatten]
    if (mask_type == 0 || mask_type == 3) {
            aspect_ratio = scale_triad_height;
    }
    else if (mask_type == 1 || mask_type == 4) {
            aspect_ratio = scale_triad_height * slot_aspect_ratio;
    }
    else {
            aspect_ratio = scale_triad_height * shadow_aspect_ratio;
    }
    [flatten]
    switch (mask_type) {
        case 0:
            aspect_ratio = scale_triad_height;
            break;
        case 1:
            aspect_ratio = scale_triad_height * slot_aspect_ratio;
            break;
        case 2:
            aspect_ratio = scale_triad_height * shadow_aspect_ratio;
            break;
        case 3:
            aspect_ratio = scale_triad_height;
            break;
        case 4:
            aspect_ratio = scale_triad_height * smallslot_aspect_ratio;
            break;
        default:
            aspect_ratio = scale_triad_height * smallshadow_aspect_ratio;
            break;
    }

    return aspect_ratio;
}

float2 calc_triad_size() {
    const float aspect_ratio = get_selected_aspect_ratio();

    [branch]
    if (mask_size_param == 0) {
        return float2(1, aspect_ratio) * mask_triad_width;
    }
    else {
        float triad_width = content_size.x * rcp(mask_num_triads_across);
        return float2(1, aspect_ratio) * triad_width;
    }

}

float2 calc_phosphor_viewport_frequency_factor() {
    const float aspect_ratio = get_selected_aspect_ratio();

    float2 triad_size_factor;
    float2 num_triads_factor;
    [branch]
    if (geom_rotation_mode == 0 || geom_rotation_mode == 2) {
        triad_size_factor = content_size * rcp(mask_triad_width * float2(1, aspect_ratio));
        num_triads_factor = mask_num_triads_across * float2(1, content_size.y * rcp(content_size.x) * rcp(aspect_ratio));
    }
    else {
        triad_size_factor = content_size * rcp(mask_triad_width * float2(1, aspect_ratio)).yx;
        num_triads_factor = mask_num_triads_across * float2(1, content_size.y * rcp(content_size.x) * rcp(aspect_ratio)).yx;
    }

    return ((mask_size_param == 0) ? triad_size_factor : num_triads_factor);
}


/*
 *  We have a pulse wave f(t0_norm, p, q) = y0 with unknown p.
 *  This function solves for p.
 */
#define calculate_phosphor_p_value(t0_norm, y0, q) (log((y0) * rcp(1 - (y0))) - (q) * (0.5 - 2*(t0_norm)))

/*
 *  If we don't rescale the phosphor_thickness parameter, it has a logarithmic effect on the phosphor shape.
 *  Rescaling it makes it look closer to a linear effect.
 */
#define linearize_phosphor_thickness_param(p) (1 - exp(-(p)))


/*
 *  Generates a grille mask with the desired resolution and sharpness.
 */
float3 get_phosphor_intensity_grille(
    const float2 texcoord,
    const float2 viewport_frequency_factor,
    const float2 grille_pq
) {
    float3 center_offsets = (geom_rotation_mode == 2 || geom_rotation_mode == 3) ?
        grille_norm_center_offsets.bgr : grille_norm_center_offsets;
        
    center_offsets += phosphor_offset_x * 0.5;

    float3 theta = triangle_wave(texcoord.x * viewport_frequency_factor.x - center_offsets, 1);
    float3 alpha = exp((theta - 0.5) * grille_pq.y);
    return alpha * rcp(alpha + grille_pq.x);
}


/*
 *  Generates a slot mask with the desired resolution and sharpness.
 */
float3 get_phosphor_intensity_slot(
    const float2 texcoord,
    const float2 viewport_frequency_factor,
    const float2 slot_pq_x,
    const float2 slot_pq_y
) {
    float3 center_offsets_x = (geom_rotation_mode == 2 || geom_rotation_mode == 3) ?
        slot_norm_center_offsets_x.bgr : slot_norm_center_offsets_x;
    float3 center_offsets_y = (geom_rotation_mode == 2 || geom_rotation_mode == 3) ?
        slot_norm_center_offsets_y.bgr : slot_norm_center_offsets_y;
        
    center_offsets_x += phosphor_offset_x * 0.5;
    center_offsets_y += phosphor_offset_y * 0.5;

    float3 theta_x1 = triangle_wave(texcoord.x * viewport_frequency_factor.x - center_offsets_x, 0.5);
    float3 alpha_x1 = exp((theta_x1 - 0.5) * slot_pq_x.y);
    alpha_x1 *= rcp(alpha_x1 + slot_pq_x.x);
    
    float3 theta_x2 = triangle_wave(texcoord.x * viewport_frequency_factor.x - center_offsets_x + 1, 0.5);
    float3 alpha_x2 = exp((theta_x2 - 0.5) * slot_pq_x.y);
    alpha_x2 *= rcp(alpha_x2 + slot_pq_x.x);

    float3 theta_y1 = triangle_wave(texcoord.y * viewport_frequency_factor.y - center_offsets_y, 1);
    float3 alpha_y1 = exp((theta_y1 - 0.5) * slot_pq_y.y);
    alpha_y1 *= rcp(alpha_y1 + slot_pq_y.x);
    
    float3 theta_y2 = triangle_wave(texcoord.y * viewport_frequency_factor.y - center_offsets_y + 0.5, 1);
    float3 alpha_y2 = exp((theta_y2 - 0.5) * slot_pq_y.y);
    alpha_y2 *= rcp(alpha_y2 + slot_pq_y.x);

    return alpha_x1 * alpha_y1 + alpha_x2 * alpha_y2;
}

/*
 *  Generates a shadow mask with the desired resolution and sharpness.
 */
float3 get_phosphor_intensity_shadow(
    const float2 texcoord,
    const float2 viewport_frequency_factor,
    const float2 shadow_q
) {
    float3 center_offsets_x = (geom_rotation_mode == 2 || geom_rotation_mode == 3) ?
        shadow_norm_center_offsets_x.bgr : shadow_norm_center_offsets_x;
    float3 center_offsets_y = (geom_rotation_mode == 2 || geom_rotation_mode == 3) ?
        shadow_norm_center_offsets_y.bgr : shadow_norm_center_offsets_y;
        
    center_offsets_x += phosphor_offset_x * 0.5;
    center_offsets_y += phosphor_offset_y * 0.5;

    const float2 thickness_scaled = linearize_phosphor_thickness_param(phosphor_thickness);

    const float3 x_adj = texcoord.x * viewport_frequency_factor.x - center_offsets_x;    
    const float3 y_adj = texcoord.y * viewport_frequency_factor.y - center_offsets_y;

    const float3 texcoord_x_periodic1 = shadow_norm_phosphor_rad * triangle_wave(x_adj * 3 - 0.5, 1.0);
    const float3 texcoord_x_periodic2 = shadow_norm_phosphor_rad * triangle_wave(x_adj * 3, 1.0);
    const float3 ty1 = sqrt(
        shadow_norm_phosphor_rad*shadow_norm_phosphor_rad - texcoord_x_periodic1*texcoord_x_periodic1
    );
    const float3 ty2 = sqrt(
        shadow_norm_phosphor_rad*shadow_norm_phosphor_rad - texcoord_x_periodic2*texcoord_x_periodic2
    );

    const float shadow_px = exp(-calculate_phosphor_p_value(shadow_edge_norm_tx, thickness_scaled.x, shadow_q.x));
    const float3 shadow_py1 = exp(-calculate_phosphor_p_value(ty1 * 0.5 * rcp(shadow_aspect_ratio), thickness_scaled.y, shadow_q.y));
    const float3 shadow_py2 = exp(-calculate_phosphor_p_value(ty2 * 0.5 * rcp(shadow_aspect_ratio), thickness_scaled.y, shadow_q.y));

    float3 theta_x1 = triangle_wave(x_adj, 1);
    float3 alpha_x1 = exp((theta_x1 - 0.5) * shadow_q.x);
    alpha_x1 *= rcp(alpha_x1 + shadow_px);
    
    float3 theta_x2 = triangle_wave(x_adj + 0.5, 1);
    float3 alpha_x2 = exp((theta_x2 - 0.5) * shadow_q.x);
    alpha_x2 *= rcp(alpha_x2 + shadow_px);
    
    float3 theta_y1 = triangle_wave(y_adj, 0.5);
    float3 alpha_y1 = exp((theta_y1 - 0.5) * shadow_q.y);
    alpha_y1 *= rcp(alpha_y1 + shadow_py1);
    
    float3 theta_y2 = triangle_wave(y_adj + 1, 0.5);
    float3 alpha_y2 = exp((theta_y2 - 0.5) * shadow_q.y);
    alpha_y2 *= rcp(alpha_y2 + shadow_py2);

    return alpha_x1 * alpha_y1 + alpha_x2 * alpha_y2;
}

float3 get_phosphor_intensity_grille_small(
    const float2 texcoord,
    const float2 viewport_frequency_factor,
    const float2 grille_pq_x
) {
    float3 center_offsets_x = (geom_rotation_mode == 2 || geom_rotation_mode == 3) ?
        smallgrille_norm_center_offsets_x.grg : smallgrille_norm_center_offsets_x;
        
    center_offsets_x += phosphor_offset_x * 0.5;
    
    float3 theta = triangle_wave(texcoord.x * viewport_frequency_factor.x - center_offsets_x, 1);
    float3 alpha = exp((theta - 0.5) * grille_pq_x.y);
    alpha *= rcp(alpha + grille_pq_x.x);

    // Taking a sqrt here helps hide the gaps between the pixels when the triad size is small
    return sqrt(alpha);
}

float3 get_phosphor_intensity_slot_small(
    const float2 texcoord,
    const float2 viewport_frequency_factor,
    const float2 slot_pq_x,
    const float2 slot_pq_y
) {
    float3 center_offsets_x = (geom_rotation_mode == 2 || geom_rotation_mode == 3) ?
        smallslot_norm_center_offsets_x.grg : smallslot_norm_center_offsets_x;
    float3 center_offsets_y1 = (geom_rotation_mode == 2 || geom_rotation_mode == 3) ?
        smallslot_norm_center_offsets_y1.grg : smallslot_norm_center_offsets_y1;
    float3 center_offsets_y2 = (geom_rotation_mode == 2 || geom_rotation_mode == 3) ?
        smallslot_norm_center_offsets_y2.grg : smallslot_norm_center_offsets_y2;
        
    center_offsets_x += phosphor_offset_x * 0.5;
    center_offsets_y1 += phosphor_offset_y * 0.5;
    center_offsets_y2 += phosphor_offset_y * 0.5;

    float3 theta_x1 = triangle_wave(texcoord.x * viewport_frequency_factor.x - center_offsets_x, 0.5);
    float3 alpha_x1 = exp((theta_x1 - 0.5) * slot_pq_x.y);
    alpha_x1 *= rcp(alpha_x1 + slot_pq_x.x);
    
    float3 theta_x2 = triangle_wave(texcoord.x * viewport_frequency_factor.x - center_offsets_x + 1, 0.5);
    float3 alpha_x2 = exp((theta_x2 - 0.5) * slot_pq_x.y);
    alpha_x2 *= rcp(alpha_x2 + slot_pq_x.x);

    float3 theta_y1 = triangle_wave(texcoord.y * viewport_frequency_factor.y - center_offsets_y1, 1);
    float3 alpha_y1 = exp((theta_y1 - 0.5) * slot_pq_y.y);
    alpha_y1 *= rcp(alpha_y1 + slot_pq_y.x);
    
    float3 theta_y2 = triangle_wave(texcoord.y * viewport_frequency_factor.y - center_offsets_y2 + 0.5, 1);
    float3 alpha_y2 = exp((theta_y2 - 0.5) * slot_pq_y.y);
    alpha_y2 *= rcp(alpha_y2 + slot_pq_y.x);

    // Taking a sqrt here helps hide the gaps between the pixels when the triad size is small
    return (alpha_x1 * alpha_y1 + alpha_x2 * alpha_y2);
}

float3 get_phosphor_intensity_shadow_small(
    const float2 texcoord,
    const float2 viewport_frequency_factor,
    const float2 shadow_pq_x,
    const float2 shadow_pq_y
) {
    float3 center_offsets_x = (geom_rotation_mode == 2 || geom_rotation_mode == 3) ?
        smallshadow_norm_center_offsets_x.grg : smallshadow_norm_center_offsets_x;
    float3 center_offsets_y = (geom_rotation_mode == 2 || geom_rotation_mode == 3) ?
        smallshadow_norm_center_offsets_y.grg : smallshadow_norm_center_offsets_y;

    center_offsets_x += phosphor_offset_x * 0.5;
    center_offsets_y += phosphor_offset_y * 0.5;
    
    float3 theta_x1 = triangle_wave(texcoord.x * viewport_frequency_factor.x - center_offsets_x, 1);
    float3 alpha_x1 = exp((theta_x1 - 0.5) * shadow_pq_x.y);
    alpha_x1 *= rcp(alpha_x1 + shadow_pq_x.x);
    
    float3 theta_x2 = triangle_wave(texcoord.x * viewport_frequency_factor.x - center_offsets_x + 0.5, 1);
    float3 alpha_x2 = exp((theta_x2 - 0.5) * shadow_pq_x.y);
    alpha_x2 *= rcp(alpha_x2 + shadow_pq_x.x);

    float3 theta_y1 = triangle_wave(texcoord.y * viewport_frequency_factor.y - center_offsets_y, 0.5);
    float3 alpha_y1 = exp((theta_y1 - 0.5) * shadow_pq_y.y);
    alpha_y1 *= rcp(alpha_y1 + shadow_pq_y.x);
    
    float3 theta_y2 = triangle_wave(texcoord.y * viewport_frequency_factor.y - center_offsets_y + 1, 0.5);
    float3 alpha_y2 = exp((theta_y2 - 0.5) * shadow_pq_y.y);
    alpha_y2 *= rcp(alpha_y2 + shadow_pq_y.x);

    // Taking a sqrt here helps hide the gaps between the pixels when the triad size is small
    return sqrt(alpha_x1 * alpha_y1 + alpha_x2 * alpha_y2);
}

#endif  //  _PHOSHOR_MASK_CALCULATIONS_H