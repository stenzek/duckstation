#ifndef _DERIVED_SETTINGS_AND_CONSTANTS_H
#define _DERIVED_SETTINGS_AND_CONSTANTS_H

#include "helper-functions-and-macros.fxh"
#include "user-settings.fxh"

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


/////////////////////////////////  DESCRIPTION  ////////////////////////////////

//  These macros and constants can be used across the whole codebase.
//  Unlike the values in user-settings.cgh, end users shouldn't modify these.


///////////////////////////////  BEGIN INCLUDES  ///////////////////////////////

//#include "../user-settings.h"

//#include "user-cgp-constants.h"

/////////////////////////   BEGIN USER-CGP-CONSTANTS   /////////////////////////

#ifndef _USER_CGP_CONSTANTS_H
#define _USER_CGP_CONSTANTS_H

//  IMPORTANT:
//  These constants MUST be set appropriately for the settings in crt-royale.cgp
//  (or whatever related .cgp file you're using).  If they aren't, you're likely
//  to get artifacts, the wrong phosphor mask size, etc.  I wish these could be
//  set directly in the .cgp file to make things easier, but...they can't.

//  PASS SCALES AND RELATED CONSTANTS:
//  Copy the absolute scale_x for BLOOM_APPROX.  There are two major versions of
//  this shader: One does a viewport-scale bloom, and the other skips it.  The
//  latter benefits from a higher bloom_approx_scale_x, so save both separately:
static const float bloom_approx_scale_x = 4.0 / 3.0;
static const float max_viewport_size_x = 1080.0*1024.0*(4.0/3.0);
static const float bloom_diff_thresh_ = 1.0/256.0;

static const float bloom_approx_size_x = 320.0;
static const float bloom_approx_size_x_for_fake = 400.0;
//  Copy the viewport-relative scales of the phosphor mask resize passes
//  (MASK_RESIZE and the pass immediately preceding it):
static const float2 mask_resize_viewport_scale = float2(0.0625, 0.0625);
//  Copy the geom_max_aspect_ratio used to calculate the MASK_RESIZE scales, etc.:
static const float geom_max_aspect_ratio = 4.0/3.0;

//  PHOSPHOR MASK TEXTURE CONSTANTS:
//  Set the following constants to reflect the properties of the phosphor mask
//  texture named in crt-royale.cgp.  The shader optionally resizes a mask tile
//  based on user settings, then repeats a single tile until filling the screen.
//  The shader must know the input texture size (default 64x64), and to manually
//  resize, it must also know the horizontal triads per tile (default 8).
static const float2 mask_texture_small_size = float2(64.0, 64.0);
static const float2 mask_texture_large_size = float2(512.0, 512.0);
static const float mask_triads_per_tile = 8.0;
//  We need the average brightness of the phosphor mask to compensate for the
//  dimming it causes.  The following four values are roughly correct for the
//  masks included with the shader.  Update the value for any LUT texture you
//  change.  [Un]comment "#define PHOSPHOR_MASK_GRILLE14" depending on whether
//  the loaded aperture grille uses 14-pixel or 15-pixel stripes (default 15).
// #ifndef PHOSPHOR_MASK_GRILLE14
//     #define PHOSPHOR_MASK_GRILLE14 0
// #endif
static const float mask_grille14_avg_color = 50.6666666/255.0;
    //  TileableLinearApertureGrille14Wide7d33Spacing*.png
    //  TileableLinearApertureGrille14Wide10And6Spacing*.png
static const float mask_grille15_avg_color = 53.0/255.0;
    //  TileableLinearApertureGrille15Wide6d33Spacing*.png
    //  TileableLinearApertureGrille15Wide8And5d5Spacing*.png
static const float mask_slot_avg_color = 46.0/255.0;
    //  TileableLinearSlotMask15Wide9And4d5Horizontal8VerticalSpacing*.png
    //  TileableLinearSlotMaskTall15Wide9And4d5Horizontal9d14VerticalSpacing*.png
static const float mask_shadow_avg_color = 41.0/255.0;
    //  TileableLinearShadowMask*.png
    //  TileableLinearShadowMaskEDP*.png

// #if PHOSPHOR_MASK_GRILLE14
//     static const float mask_grille_avg_color = mask_grille14_avg_color;
// #else
    static const float mask_grille_avg_color = mask_grille15_avg_color;
// #endif


#endif  //  _USER_CGP_CONSTANTS_H

//////////////////////////   END USER-CGP-CONSTANTS   //////////////////////////

////////////////////////////////  END INCLUDES  ////////////////////////////////

///////////////////////////////  FIXED SETTINGS  ///////////////////////////////



#define _SIMULATE_CRT_ON_LCD 1
#define _SIMULATE_GBA_ON_LCD 2
#define _SIMULATE_LCD_ON_CRT 3
#define _SIMULATE_GBA_ON_CRT 4

//  Ensure the first pass decodes CRT gamma and the last encodes LCD gamma.
#define GAMMA_SIMULATION_MODE _SIMULATE_CRT_ON_LCD

//  Manually tiling a manually resized texture creates texture coord derivative
//  discontinuities and confuses anisotropic filtering, causing discolored tile
//  seams in the phosphor mask.  Workarounds:
//  a.) Using tex2Dlod disables anisotropic filtering for tiled masks.  It's
//      downgraded to tex2Dbias without _DRIVERS_ALLOW_TEX2DLOD #defined and
//      disabled without _DRIVERS_ALLOW_TEX2DBIAS #defined either.
//  b.) "Tile flat twice" requires drawing two full tiles without border padding
//      to the resized mask FBO, and it's incompatible with same-pass curvature.
//      (Same-pass curvature isn't used but could be in the future...maybe.)
//  c.) "Fix discontinuities" requires derivatives and drawing one tile with
//      border padding to the resized mask FBO, but it works with same-pass
//      curvature.  It's disabled without _DRIVERS_ALLOW_DERIVATIVES #defined.
//  Precedence: a, then, b, then c (if multiple strategies are #defined).
// #ifndef ANISOTROPIC_TILING_COMPAT_TEX2DLOD
//     #define ANISOTROPIC_TILING_COMPAT_TEX2DLOD 1              //  129.7 FPS, 4x, flat; 101.8 at fullscreen
// #endif
// #ifndef ANISOTROPIC_TILING_COMPAT_TILE_FLAT_TWICE
//     #define ANISOTROPIC_TILING_COMPAT_TILE_FLAT_TWICE 1       //  128.1 FPS, 4x, flat; 101.5 at fullscreen
// #endif
// #ifndef ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES
//     #define ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES 1   //  124.4 FPS, 4x, flat; 97.4 at fullscreen
// #endif
//  Also, manually resampling the phosphor mask is slightly blurrier with
//  anisotropic filtering.  (Resampling with mipmapping is even worse: It
//  creates artifacts, but only with the fully bloomed shader.)  The difference
//  is subtle with small triads, but you can fix it for a small cost.
// #ifndef ANISOTROPIC_RESAMPLING_COMPAT_TEX2DLOD
//     #define ANISOTROPIC_RESAMPLING_COMPAT_TEX2DLOD 0
// #endif


//////////////////////////////  DERIVED SETTINGS  //////////////////////////////

//  Intel HD 4000 GPU's can't handle manual mask resizing (for now), setting the
//  geometry mode at runtime, or a 4x4 true Gaussian resize.  Disable
//  incompatible settings ASAP.  (_INTEGRATED_GRAPHICS_COMPATIBILITY_MODE may be
//  #defined by either user-settings.h or a wrapper .cg that #includes the
//  current .cg pass.)
#if _INTEGRATED_GRAPHICS_COMPATIBILITY_MODE
    #if _PHOSPHOR_MASK_MANUALLY_RESIZE
        #undef _PHOSPHOR_MASK_MANUALLY_RESIZE
        #define _PHOSPHOR_MASK_MANUALLY_RESIZE 0
    #endif
    #if _RUNTIME_GEOMETRY_MODE
        #undef _RUNTIME_GEOMETRY_MODE
        #define _RUNTIME_GEOMETRY_MODE 0
    #endif
    //  Mode 2 (4x4 Gaussian resize) won't work, and mode 1 (3x3 blur) is
    //  inferior in most cases, so replace 2.0 with 0.0:
    static const float bloom_approx_filter = macro_cond(
        bloom_approx_filter_static > 1.5,
        0.0,
        bloom_approx_filter_static
    );
#else
    static const float bloom_approx_filter = bloom_approx_filter_static;
#endif

//  Disable slow runtime paths if static parameters are used.  Most of these
//  won't be a problem anyway once the params are disabled, but some will.
#if !_RUNTIME_SHADER_PARAMS_ENABLE
    #if _RUNTIME_PHOSPHOR_BLOOM_SIGMA
        #undef _RUNTIME_PHOSPHOR_BLOOM_SIGMA
        #define _RUNTIME_PHOSPHOR_BLOOM_SIGMA 0
    #endif
    #if _RUNTIME_ANTIALIAS_WEIGHTS
        #undef _RUNTIME_ANTIALIAS_WEIGHTS
        #define _RUNTIME_ANTIALIAS_WEIGHTS 0
    #endif
    #if _RUNTIME_ANTIALIAS_SUBPIXEL_OFFSETS
        #undef _RUNTIME_ANTIALIAS_SUBPIXEL_OFFSETS
        #define _RUNTIME_ANTIALIAS_SUBPIXEL_OFFSETS 0
    #endif
    #if _RUNTIME_SCANLINES_HORIZ_FILTER_COLORSPACE
        #undef _RUNTIME_SCANLINES_HORIZ_FILTER_COLORSPACE
        #define _RUNTIME_SCANLINES_HORIZ_FILTER_COLORSPACE 0
    #endif
    #if _RUNTIME_GEOMETRY_TILT
        #undef _RUNTIME_GEOMETRY_TILT
        #define _RUNTIME_GEOMETRY_TILT 0
    #endif
    #if _RUNTIME_GEOMETRY_MODE
        #undef _RUNTIME_GEOMETRY_MODE
        #define _RUNTIME_GEOMETRY_MODE 0
    #endif
    // #if FORCE_RUNTIME_PHOSPHOR_MASK_MODE_TYPE_SELECT
    //     #undef FORCE_RUNTIME_PHOSPHOR_MASK_MODE_TYPE_SELECT
    //     #define FORCE_RUNTIME_PHOSPHOR_MASK_MODE_TYPE_SELECT 0
    // #endif
#endif

//  Make tex2Dbias a backup for tex2Dlod for wider compatibility.
// #if ANISOTROPIC_TILING_COMPAT_TEX2DLOD
//     #define ANISOTROPIC_TILING_COMPAT_TEX2DBIAS
// #endif
// #if ANISOTROPIC_RESAMPLING_COMPAT_TEX2DLOD
//     #define ANISOTROPIC_RESAMPLING_COMPAT_TEX2DBIAS
// #endif
//  Rule out unavailable anisotropic compatibility strategies:
#if !_DRIVERS_ALLOW_DERIVATIVES
    // #if ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES
    //     #undef ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES
    //     #define ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES 0
    // #endif
#endif
// #if !_DRIVERS_ALLOW_TEX2DLOD
    // #if ANISOTROPIC_TILING_COMPAT_TEX2DLOD
    //     #undef ANISOTROPIC_TILING_COMPAT_TEX2DLOD
    //     #define ANISOTROPIC_TILING_COMPAT_TEX2DLOD 0
    // #endif
    // #if ANISOTROPIC_RESAMPLING_COMPAT_TEX2DLOD
    //     #undef ANISOTROPIC_RESAMPLING_COMPAT_TEX2DLOD
    //     #define ANISOTROPIC_RESAMPLING_COMPAT_TEX2DLOD 0
    // #endif
    // #ifdef ANTIALIAS_DISABLE_ANISOTROPIC
    //     #undef ANTIALIAS_DISABLE_ANISOTROPIC
    // #endif
// #endif
// #if !_DRIVERS_ALLOW_TEX2DBIAS
    // #ifdef ANISOTROPIC_TILING_COMPAT_TEX2DBIAS
    //     #undef ANISOTROPIC_TILING_COMPAT_TEX2DBIAS
    // #endif
    // #ifdef ANISOTROPIC_RESAMPLING_COMPAT_TEX2DBIAS
    //     #undef ANISOTROPIC_RESAMPLING_COMPAT_TEX2DBIAS
    // #endif
// #endif
//  Prioritize anisotropic tiling compatibility strategies by performance and
//  disable unused strategies.  This concentrates all the nesting in one place.
// #if ANISOTROPIC_TILING_COMPAT_TEX2DLOD
//     #ifdef ANISOTROPIC_TILING_COMPAT_TEX2DBIAS
//         #undef ANISOTROPIC_TILING_COMPAT_TEX2DBIAS
//     #endif
//     #if ANISOTROPIC_TILING_COMPAT_TILE_FLAT_TWICE
//         #undef ANISOTROPIC_TILING_COMPAT_TILE_FLAT_TWICE
//         #define ANISOTROPIC_TILING_COMPAT_TILE_FLAT_TWICE 0
//     #endif
//     #if ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES
//         #undef ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES
//         #define ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES 0
//     #endif
//     #else
//     #ifdef ANISOTROPIC_TILING_COMPAT_TEX2DBIAS
//         #if ANISOTROPIC_TILING_COMPAT_TILE_FLAT_TWICE
//             #undef ANISOTROPIC_TILING_COMPAT_TILE_FLAT_TWICE
//             #define ANISOTROPIC_TILING_COMPAT_TILE_FLAT_TWICE 0
//         #endif
//         #if ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES
//             #undef ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES
//             #define ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES 0
//         #endif
//     #else
//         //  ANISOTROPIC_TILING_COMPAT_TILE_FLAT_TWICE is only compatible with
//         //  flat texture coords in the same pass, but that's all we use.
//         #if ANISOTROPIC_TILING_COMPAT_TILE_FLAT_TWICE
//             #if ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES
//                 #undef ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES
//                 #define ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES 0
//             #endif
//         #endif
//     #endif
// #endif
//  The tex2Dlod and tex2Dbias strategies share a lot in common, and we can
//  reduce some #ifdef nesting in the next section by essentially OR'ing them:
// #if ANISOTROPIC_TILING_COMPAT_TEX2DLOD
//     #define ANISOTROPIC_TILING_COMPAT_TEX2DLOD_FAMILY
// #endif
// #ifdef ANISOTROPIC_TILING_COMPAT_TEX2DBIAS
//     #define ANISOTROPIC_TILING_COMPAT_TEX2DLOD_FAMILY
// #endif
//  Prioritize anisotropic resampling compatibility strategies the same way:
// #if ANISOTROPIC_RESAMPLING_COMPAT_TEX2DLOD
//     #ifdef ANISOTROPIC_RESAMPLING_COMPAT_TEX2DBIAS
//         #undef ANISOTROPIC_RESAMPLING_COMPAT_TEX2DBIAS
//     #endif
// #endif


///////////////////////  DERIVED PHOSPHOR MASK CONSTANTS  //////////////////////

//  If we can use the large mipmapped LUT without mipmapping artifacts, we
//  should: It gives us more options for using fewer samples.
// #if USE_LARGE_PHOSPHOR_MASK
    // #if ANISOTROPIC_RESAMPLING_COMPAT_TEX2DLOD
    //     //  TODO: Take advantage of this!
    //     #define PHOSPHOR_MASK_RESIZE_MIPMAPPED_LUT
    //     static const float2 mask_resize_src_lut_size = mask_texture_large_size;
    // #else
static const float2 mask_resize_src_lut_size = mask_texture_large_size;
    // #endif
// #else
//     static const float2 mask_resize_src_lut_size = mask_texture_small_size;
// #endif

static const float tile_aspect_inv = mask_resize_src_lut_size.y/mask_resize_src_lut_size.x;


//  tex2D's sampler2D parameter MUST be a uniform global, a uniform input to
//  main_fragment, or a static alias of one of the above.  This makes it hard
//  to select the phosphor mask at runtime: We can't even assign to a uniform
//  global in the vertex shader or select a sampler2D in the vertex shader and
//  pass it to the fragment shader (even with explicit TEXUNIT# bindings),
//  because it just gives us the input texture or a black screen.  However, we
//  can get around these limitations by calling tex2D three times with different
//  uniform samplers (or resizing the phosphor mask three times altogether).
//  With dynamic branches, we can process only one of these branches on top of
//  quickly discarding fragments we don't need (cgc seems able to overcome
//  limigations around dependent texture fetches inside of branches).  Without
//  dynamic branches, we have to process every branch for every fragment...which
//  is slower.  Runtime sampling mode selection is slower without dynamic
//  branches as well.  Let the user's static #defines decide if it's worth it.
#if _DRIVERS_ALLOW_DYNAMIC_BRANCHES
    #define _RUNTIME_PHOSPHOR_MASK_MODE_TYPE_SELECT
// #else
    // #if FORCE_RUNTIME_PHOSPHOR_MASK_MODE_TYPE_SELECT
    //     #define _RUNTIME_PHOSPHOR_MASK_MODE_TYPE_SELECT
    // #endif
#endif

//  We need to render some minimum number of tiles in the resize passes.
//  We need at least 1.0 just to repeat a single tile, and we need extra
//  padding beyond that for anisotropic filtering, discontinuitity fixing,
//  antialiasing, same-pass curvature (not currently used), etc.  First
//  determine how many border texels and tiles we need, based on how the result
//  will be sampled:
#ifdef GEOMETRY_EARLY
    static const float max_subpixel_offset = aa_subpixel_r_offset_static.x;
    //  Most antialiasing filters have a base radius of 4.0 pixels:
    static const float max_aa_base_pixel_border = 4.0 +
        max_subpixel_offset;
#else
    static const float max_aa_base_pixel_border = 0.0;
#endif
//  Anisotropic filtering adds about 0.5 to the pixel border:
// #ifndef ANISOTROPIC_TILING_COMPAT_TEX2DLOD_FAMILY
static const float max_aniso_pixel_border = max_aa_base_pixel_border + 0.5;
// #else
//     static const float max_aniso_pixel_border = max_aa_base_pixel_border;
// #endif
//  Fixing discontinuities adds 1.0 more to the pixel border:
// #if ANISOTROPIC_TILING_COMPAT_FIX_DISCONTINUITIES
//     static const float max_tiled_pixel_border = max_aniso_pixel_border + 1.0;
// #else
    static const float max_tiled_pixel_border = max_aniso_pixel_border;
// #endif
//  Convert the pixel border to an integer texel border.  Assume same-pass
//  curvature about triples the texel frequency:
#ifdef GEOMETRY_EARLY
    #define max_mask_texel_border macro_ceil(max_tiled_pixel_border * 3.0f)
#else
    #define max_mask_texel_border macro_ceil(max_tiled_pixel_border)
#endif
//  Convert the texel border to a tile border using worst-case assumptions:
static const float max_mask_tile_border = max_mask_texel_border/
(mask_min_allowed_triad_size * mask_triads_per_tile);

//  Finally, set the number of resized tiles to render to MASK_RESIZE, and set
//  the starting texel (inside borders) for sampling it.
#ifndef GEOMETRY_EARLY
    // #if ANISOTROPIC_TILING_COMPAT_TILE_FLAT_TWICE
        //  Special case: Render two tiles without borders.  Anisotropic
        //  filtering doesn't seem to be a problem here.
        // static const float mask_resize_num_tiles = 1.0 + 1.0;
        // static const float mask_start_texels = 0.0;
    // #else
    static const float mask_resize_num_tiles = 1.0 + 2.0 * max_mask_tile_border;
    static const float mask_start_texels = max_mask_texel_border;
    // #endif
#else
    static const float mask_resize_num_tiles = 1.0 + 2.0*max_mask_tile_border;
    static const float mask_start_texels = max_mask_texel_border;
#endif

//  We have to fit mask_resize_num_tiles into an FBO with a viewport scale of
//  mask_resize_viewport_scale.  This limits the maximum final triad size.
//  Estimate the minimum number of triads we can split the screen into in each
//  dimension (we'll be as correct as mask_resize_viewport_scale is):
static const float mask_resize_num_triads = mask_resize_num_tiles * mask_triads_per_tile;
static const float2 min_allowed_viewport_triads =
float2(mask_resize_num_triads, mask_resize_num_triads) / mask_resize_viewport_scale;



#endif  //  _DERIVED_SETTINGS_AND_CONSTANTS_H