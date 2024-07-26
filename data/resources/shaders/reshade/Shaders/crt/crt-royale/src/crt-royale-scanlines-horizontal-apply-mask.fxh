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

#include "../include/scanline-functions.fxh"
#include "../include/phosphor-mask-resizing.fxh"
#include "../include/bloom-functions.fxh"
#include "../include/gamma-management.fxh"


///////////////////////////////////  HELPERS  //////////////////////////////////

float4 tex2Dtiled_mask_linearize(const sampler2D tex,
    const float2 tex_uv)
{
    //  If we're manually tiling a texture, anisotropic filtering can get
    //  confused.  One workaround is to just select the lowest mip level:
    #ifdef PHOSPHOR_MASK_MANUALLY_RESIZE
        #ifdef ANISOTROPIC_TILING_COMPAT_TEX2DLOD
            //  TODO: Use tex2Dlod_linearize with a calculated mip level.
            return tex2Dlod_linearize(tex, float4(tex_uv, 0.0, 0.0));
        #else
            #ifdef ANISOTROPIC_TILING_COMPAT_TEX2DBIAS
                return tex2Dbias_linearize(tex, float4(tex_uv, 0.0, -16.0));
            #else
                return tex2D_linearize(tex, tex_uv);
            #endif
        #endif
    #else
        return tex2D_linearize(tex, tex_uv);
    #endif
}


/////////////////////////////////  STRUCTURES  /////////////////////////////////


struct out_vertex_p7
{
    //  Use explicit semantics so COLORx doesn't clamp values outside [0, 1].
    float2 video_uv                     : TEXCOORD1;
    float2 scanline_tex_uv              : TEXCOORD2;
    float2 blur3x3_tex_uv               : TEXCOORD3;
    float2 halation_tex_uv              : TEXCOORD4;
    float2 scanline_texture_size_inv    : TEXCOORD5;
    float4 mask_tile_start_uv_and_size  : TEXCOORD6;
    float2 mask_tiles_per_screen        : TEXCOORD7;
};


////////////////////////////////  VERTEX SHADER  ///////////////////////////////


// Vertex shader generating a triangle covering the entire screen
void VS_Scanlines_Horizontal_Apply_Mask(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD, out out_vertex_p7 OUT)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    float2 tex_uv = texcoord;

    float2 texture_size = MASKED_SCANLINES_texture_size;
    float2 output_size  = VIEWPORT_SIZE;

    //  Our various input textures use different coords.
    const float2 video_uv = tex_uv * texture_size/video_size;
    const float2 scanline_texture_size_inv =
        1.0.xx/VERTICAL_SCANLINES_texture_size;
    OUT.video_uv = video_uv;
    OUT.scanline_tex_uv = video_uv * VERTICAL_SCANLINES_video_size *
        scanline_texture_size_inv;
    OUT.blur3x3_tex_uv = video_uv * BLOOM_APPROX_video_size /
        BLOOM_APPROX_texture_size;
    OUT.halation_tex_uv = video_uv * HALATION_BLUR_video_size /
        HALATION_BLUR_texture_size;
    OUT.scanline_texture_size_inv = scanline_texture_size_inv;

    //  Get a consistent name for the final mask texture size.  Sample mode 0
    //  uses the manually resized mask, but ignore it if we never resized.
    #ifdef PHOSPHOR_MASK_MANUALLY_RESIZE
        const float mask_sample_mode = get_mask_sample_mode();
        const float2 mask_resize_texture_size = mask_sample_mode < 0.5 ?
            MASKED_SCANLINES_texture_size : mask_texture_large_size;
        const float2 mask_resize_video_size = mask_sample_mode < 0.5 ?
            MASKED_SCANLINES_video_size : mask_texture_large_size;
    #else
        const float2 mask_resize_texture_size = mask_texture_large_size;
        const float2 mask_resize_video_size = mask_texture_large_size;
    #endif
    //  Compute mask tile dimensions, starting points, etc.:
    float2 mask_tiles_per_screen;
    OUT.mask_tile_start_uv_and_size = get_mask_sampling_parameters(
        mask_resize_texture_size, mask_resize_video_size, output_size,
        mask_tiles_per_screen);
    OUT.mask_tiles_per_screen = mask_tiles_per_screen;
}


///////////////////////////////  FRAGMENT SHADER  //////////////////////////////

float4 PS_Scanlines_Horizontal_Apply_Mask(float4 vpos: SV_Position, float2 vTexCoord : TEXCOORD, in out_vertex_p7 VAR) : SV_Target
{
    //  This pass: Sample (misconverged?) scanlines to the final horizontal
    //  resolution, apply halation (bouncing electrons), and apply the phosphor
    //  mask.  Fake a bloom if requested.  Unless we fake a bloom, the output
    //  will be dim from the scanline auto-dim, mask dimming, and low gamma.

    //  Horizontally sample the current row (a vertically interpolated scanline)
    //  and account for horizontal convergence offsets, given in units of texels.
  //  float2 VERTICAL_SCANLINES_texture_size = float2(1.0/NormalizedNativePixelSize.x, ViewportSize.y*BufferToViewportRatio.y);

    float2 output_size  = VIEWPORT_SIZE;

    const float3 scanline_color_dim = sample_rgb_scanline_horizontal(
        VERTICAL_SCANLINES, VAR.scanline_tex_uv,
        VERTICAL_SCANLINES_texture_size, VAR.scanline_texture_size_inv);
    const float auto_dim_factor = levels_autodim_temp;

    //  Sample the phosphor mask:
    const float2 tile_uv_wrap = VAR.video_uv * VAR.mask_tiles_per_screen;
    const float2 mask_tex_uv = convert_phosphor_tile_uv_wrap_to_tex_uv(
        tile_uv_wrap, VAR.mask_tile_start_uv_and_size);
    float3 phosphor_mask_sample;
    #ifdef PHOSPHOR_MASK_MANUALLY_RESIZE
        const bool sample_orig_luts = get_mask_sample_mode() > 0.5;
    #else
        static const bool sample_orig_luts = true;
    #endif
    if(sample_orig_luts)
    {
        //  If mask_type is static, this branch will be resolved statically.
        if(mask_type < 0.5)
        {
            phosphor_mask_sample = tex2D_linearize(
                mask_grille_texture_large, mask_tex_uv).rgb;
        }
        else if(mask_type < 1.5)
        {
            phosphor_mask_sample = tex2D_linearize(
                mask_slot_texture_large, mask_tex_uv).rgb;
        }
        else
        {
            phosphor_mask_sample = tex2D_linearize(
                mask_shadow_texture_large, mask_tex_uv).rgb;
        }
    }
    else
    {
        //  Sample the resized mask, and avoid tiling artifacts:
        phosphor_mask_sample = tex2Dtiled_mask_linearize(
            MASK_RESIZE, mask_tex_uv).rgb;
    }

    //  Sample the halation texture (auto-dim to match the scanlines), and
    //  account for both horizontal and vertical convergence offsets, given
    //  in units of texels horizontally and same-field scanlines vertically:
    const float3 halation_color = tex2D_linearize(
        HALATION_BLUR, VAR.halation_tex_uv).rgb;

    //  Apply halation: Halation models electrons flying around under the glass
    //  and hitting the wrong phosphors (of any color).  It desaturates, so
    //  average the halation electrons to a scalar.  Reduce the local scanline
    //  intensity accordingly to conserve energy.
    const float3 halation_intensity_dim =
        dot(halation_color, auto_dim_factor.xxx/3.0).xxx;
    const float3 electron_intensity_dim = lerp(scanline_color_dim,
        halation_intensity_dim, halation_weight);

    //  Apply the phosphor mask:
    const float3 phosphor_emission_dim = electron_intensity_dim *
        phosphor_mask_sample;

    #ifdef PHOSPHOR_BLOOM_FAKE
        //  The BLOOM_APPROX pass approximates a blurred version of a masked
        //  and scanlined image.  It's usually used to compute the brightpass,
        //  but we can also use it to fake the bloom stage entirely.  Caveats:
        //  1.) A fake bloom is conceptually different, since we're mixing in a
        //      fully blurred low-res image, and the biggest implication are:
        //  2.) If mask_amplify is incorrect, results deteriorate more quickly.
        //  3.) The inaccurate blurring hurts quality in high-contrast areas.
        //  4.) The bloom_underestimate_levels parameter seems less sensitive.
        //  Reverse the auto-dimming and amplify to compensate for mask dimming:
        #define PHOSPHOR_BLOOM_FAKE_WITH_SIMPLE_BLEND
        #ifdef PHOSPHOR_BLOOM_FAKE_WITH_SIMPLE_BLEND
            static const float blur_contrast = 1.05;
        #else
            static const float blur_contrast = 1.0;
        #endif
        const float mask_amplify = get_mask_amplify();
        const float undim_factor = 1.0/auto_dim_factor;
        const float3 phosphor_emission =
            phosphor_emission_dim * undim_factor * mask_amplify;
        //  Get a phosphor blur estimate, accounting for convergence offsets:
        const float3 electron_intensity = electron_intensity_dim * undim_factor;
        const float3 phosphor_blur_approx_soft = tex2D_linearize(
            BLOOM_APPROX, VAR.blur3x3_tex_uv).rgb;
        const float3 phosphor_blur_approx = lerp(phosphor_blur_approx_soft,
            electron_intensity, 0.1) * blur_contrast;
        //  We could blend between phosphor_emission and phosphor_blur_approx,
        //  solving for the minimum blend_ratio that avoids clipping past 1.0:
        //      1.0 >= total_intensity
        //      1.0 >= phosphor_emission * (1.0 - blend_ratio) +
        //              phosphor_blur_approx * blend_ratio
        //      blend_ratio = (phosphor_emission - 1.0)/
        //          (phosphor_emission - phosphor_blur_approx);
        //  However, this blurs far more than necessary, because it aims for
        //  full brightness, not minimal blurring.  To fix it, base blend_ratio
        //  on a max area intensity only so it varies more smoothly:
        const float3 phosphor_blur_underestimate =
            phosphor_blur_approx * bloom_underestimate_levels;
        const float3 area_max_underestimate =
            phosphor_blur_underestimate * mask_amplify;
        #ifdef PHOSPHOR_BLOOM_FAKE_WITH_SIMPLE_BLEND
            const float3 blend_ratio_temp =
                (area_max_underestimate - 1.0.xxx) /
                (area_max_underestimate - phosphor_blur_underestimate);
        #else
            //  Try doing it like an area-based brightpass.  This is nearly
            //  identical, but it's worth toying with the code in case I ever
            //  find a way to make it look more like a real bloom.  (I've had
            //  some promising textures from combining an area-based blend ratio
            //  for the phosphor blur and a more brightpass-like blend-ratio for
            //  the phosphor emission, but I haven't found a way to make the
            //  brightness correct across the whole color range, especially with
            //  different bloom_underestimate_levels values.)
            const float desired_triad_size = lerp(mask_triad_size_desired,
                output_size.x/mask_num_triads_desired,
                mask_specify_num_triads);
            const float bloom_sigma = get_min_sigma_to_blur_triad(
                desired_triad_size, bloom_diff_thresh);
            const float center_weight = get_center_weight(bloom_sigma);
            const float3 max_area_contribution_approx =
                max(0.0.xxx, phosphor_blur_approx -
                center_weight * phosphor_emission);
            const float3 area_contrib_underestimate =
                bloom_underestimate_levels * max_area_contribution_approx;
            const float3 blend_ratio_temp =
                ((1.0.xxx - area_contrib_underestimate) /
                area_max_underestimate - 1.0.xxx) / (center_weight - 1.0);
        #endif
        //  Clamp blend_ratio in case it's out-of-range, but be SUPER careful:
        //  min/max/clamp are BIZARRELY broken with lerp (optimization bug?),
        //  and this redundant sequence avoids bugs, at least on nVidia cards:
        const float3 blend_ratio_clamped = max(clamp(blend_ratio_temp, 0.0, 1.0), 0.0);
        const float3 blend_ratio = lerp(blend_ratio_clamped, 1.0.xxx, bloom_excess);
        //  Blend the blurred and unblurred images:
        const float3 phosphor_emission_unclipped =
            lerp(phosphor_emission, phosphor_blur_approx, blend_ratio);
        //  Simulate refractive diffusion by reusing the halation sample.
        const float3 pixel_color = lerp(phosphor_emission_unclipped,
            halation_color, diffusion_weight);
    #else
        const float3 pixel_color = phosphor_emission_dim;
    #endif
    //  Encode if necessary, and output.
    return encode_output(float4(pixel_color, 1.0));
}

