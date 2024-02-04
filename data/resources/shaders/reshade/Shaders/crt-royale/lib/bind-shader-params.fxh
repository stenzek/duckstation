#ifndef _BIND_SHADER_PARAMS_H
#define _BIND_SHADER_PARAMS_H

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


/////////////////////////////  SETTINGS MANAGEMENT  ////////////////////////////

///////////////////////////////  BEGIN INCLUDES  ///////////////////////////////
#include "helper-functions-and-macros.fxh"
#include "user-settings.fxh"
#include "derived-settings-and-constants.fxh"
#include "../version-number.fxh"

////////////////////////////////  END INCLUDES  ////////////////////////////////

//  Override some parameters for gamma-management.h and tex2Dantialias.h:
#ifndef _OVERRIDE_DEVICE_GAMMA
    #define _OVERRIDE_DEVICE_GAMMA 1
#endif

#if __RENDERER__ != 0x9000
    #define _DX9_ACTIVE 0
#else
    #define _DX9_ACTIVE 1
#endif

// #ifndef ANTIALIAS_OVERRIDE_BASICS
//     #define ANTIALIAS_OVERRIDE_BASICS 1
// #endif

// #ifndef ANTIALIAS_OVERRIDE_PARAMETERS
//     #define ANTIALIAS_OVERRIDE_PARAMETERS 1
// #endif

#ifndef ADVANCED_SETTINGS
    #define ADVANCED_SETTINGS 0
#endif 

// The width of the game's content
#ifndef CONTENT_WIDTH
	#define CONTENT_WIDTH BUFFER_WIDTH
#endif
// The height of the game's content
#ifndef CONTENT_HEIGHT
	#define CONTENT_HEIGHT BUFFER_HEIGHT
#endif

#if ADVANCED_SETTINGS == 1
    // Using vertex uncropping is marginally faster, but vulnerable to DX9 weirdness.
    // Most users will likely prefer the slower algorithm.
    #ifndef USE_VERTEX_UNCROPPING
        #define USE_VERTEX_UNCROPPING 0
    #endif

    #ifndef NUM_BEAMDIST_COLOR_SAMPLES
        #define NUM_BEAMDIST_COLOR_SAMPLES 1024
    #endif

    #ifndef NUM_BEAMDIST_DIST_SAMPLES
        #define NUM_BEAMDIST_DIST_SAMPLES 120
    #endif

    #ifndef BLOOMAPPROX_DOWNSIZING_FACTOR
        #define BLOOMAPPROX_DOWNSIZING_FACTOR 4.0
    #endif

    // Define this internal value, so ADVANCED_SETTINGS == 0 doesn't cause a redefinition error when
    //   NUM_BEAMDIST_COLOR_SAMPLES defined in the preset file. Also makes it easy to avoid bugs
    //   related to parentheses and order-of-operations when the user defines this arithmetically.
    static const uint num_beamdist_color_samples = uint(NUM_BEAMDIST_COLOR_SAMPLES);
    static const uint num_beamdist_dist_samples = uint(NUM_BEAMDIST_DIST_SAMPLES);
    static const float bloomapprox_downsizing_factor = float(BLOOMAPPROX_DOWNSIZING_FACTOR);
#else
    static const uint USE_VERTEX_CROPPING = 0;
    static const uint num_beamdist_color_samples = 1024;
    static const uint num_beamdist_dist_samples = 120;
    static const float bloomapprox_downsizing_factor = 4.0;
#endif

#ifndef HIDE_HELP_SECTIONS
    #define HIDE_HELP_SECTIONS 0
#endif


// Offset the center of the game's content (horizontal)
#ifndef CONTENT_CENTER_X
	#define CONTENT_CENTER_X 0
#endif
// Offset the center of the game's content (vertical)
#ifndef CONTENT_CENTER_Y
	#define CONTENT_CENTER_Y 0
#endif

// Wrap the content size in parenthesis for internal use, so the user doesn't have to
static const float2 content_size = float2(int(CONTENT_WIDTH), int(CONTENT_HEIGHT));

#ifndef ENABLE_PREBLUR
    #define ENABLE_PREBLUR 1
#endif


static const float2 buffer_size = float2(BUFFER_WIDTH, BUFFER_HEIGHT);


// The normalized center is 0.5 plus the normalized offset
static const float2 content_center = float2(CONTENT_CENTER_X, CONTENT_CENTER_Y) / buffer_size + 0.5;
// The content's normalized diameter d is its size divided by the buffer's size. The radius is d/2.
static const float2 content_radius = content_size / (2.0 * buffer_size);
static const float2 content_scale = content_size / buffer_size;

static const float content_left = content_center.x - content_radius.x;
static const float content_right = content_center.x + content_radius.x;
static const float content_upper = content_center.y - content_radius.y;
static const float content_lower = content_center.y + content_radius.y;

// The xy-offset of the top-left pixel in the content box
static const float2 content_offset = float2(content_left, content_upper);
static const float2 content_offset_from_right = float2(content_right, content_lower);

uniform uint frame_count < source = "framecount"; >;
uniform int overlay_active < source = "overlay_active"; >;

static const float gba_gamma = 3.5; //  Irrelevant but necessary to define.


// === HELP AND INFO ===

uniform int APPEND_VERSION_SUFFIX(version) <
	ui_text = "Version: " DOT_VERSION_STR;
	ui_label = " ";
	ui_type = "radio";
>;

uniform int basic_setup_help <
	ui_text = "1. Configure the Content Box if your game has letter-boxing.\n"
			  "2. Configure the Phosphor Mask.\n"
              "3. Configure the Scanlines.\n"
              "4. Configure the Colors and Effects.\n"
              "5. Configure the Screen Geometry.\n"
              "6. Configure or disable Preblur\n\n"
              "- In Preprocessor Definitions, set ADVANCED_SETTINGS to 1 to access more settings.\n";
	ui_category = "Basic Setup Instructions";
    ui_category_closed = true;
	ui_label = " ";
	ui_type = "radio";
    hidden = HIDE_HELP_SECTIONS;
>;

uniform int content_box_help <
	ui_text = "1. Expand the Preprocessor Definitions section.\n"
              "2. Set CONTENT_BOX_VISIBLE to 1.\n"
              "3. Use the \"CONTENT_\" parameters to configure the Content Box.\n"
			  "4. Align the content box with the border of your game.\n"
              "5. Set CONTENT_BOX_VISIBLE to 0 when you're done.\n\n"
              "Parameters to focus on:\n"
              "- CONTENT_HEIGHT and CONTENT_WIDTH\n"
              "- CONTENT_CENTER_X and CONTENT_CENTER_Y\n"
              "- CONTENT_BOX_INSCRIBED\n\n"
              "Fancy Trick 1:\n"
              "\tCONTENT_HEIGHT = BUFFER_HEIGHT\n"
              "\tCONTENT_WIDTH = CONTENT_HEIGHT * 4.0 / 3.0\n"
              "- Good if your game fills the screen vertically and has a 4:3 aspect ratio.\n"
              "- Will also rescale automatically if you resize the window.\n\n"
              "Fancy Trick 2:\n"
              "\tCONTENT_HEIGHT = CONTENT_WIDTH * 9.0 / 16.0\n"
              "\tCONTENT_WIDTH = 1500\n"
              "- Good if your game is 1500 pixels wide with a 16:9 aspect ratio.\n"
              "- Won't rescale automatically, but you'd only have to change the width.\n";
	ui_category = "Content Box Instructions";
    ui_category_closed = true;
	ui_label = " ";
	ui_type = "radio";
    hidden = HIDE_HELP_SECTIONS;
>;


// ==== PHOSPHOR MASK ====
uniform int mask_type <
        #if !HIDE_HELP_SECTIONS
        ui_text    = "Choose which kind of CRT you want.\n\n";
        #endif
        ui_label   = "Mask Type";
        ui_tooltip = "Selects the phosphor shape";
        ui_type    = "combo";
        ui_items   = "Grille\0"
                    "Slot\0"
                    "Shadow\0"
                    "LowRes Grille\0"
                    "LowRes Slot\0"
                    "LowRes Shadow\0";

    ui_category = "Phosphor Mask";
    ui_category_closed = true;
> = mask_type_static;

uniform uint mask_size_param <
        ui_label   = "Mask Size Param";
        ui_tooltip = "Switch between using Mask Triad Size or Mask Num Triads";
        ui_type    = "combo";
        ui_items   = "Triad Width\0"
                    "Num Triads Across\0";
        hidden     = !ADVANCED_SETTINGS;

    ui_spacing = 2;
    ui_category = "Phosphor Mask";
> = mask_size_param_static;

uniform float mask_triad_width <
        ui_label   = "Mask Triad Width";
        ui_tooltip = "The width of a triad in pixels";
        ui_type    = "slider";
        ui_min     = 1.0;
        ui_max     = 60.0;
        ui_step    = 0.1;

    ui_category = "Phosphor Mask";
> = mask_triad_width_static;

uniform float mask_num_triads_across <
        ui_label   = "Mask Num Triads Across";
        ui_tooltip = "The number of triads in the viewport (horizontally)";
        ui_type    = "drag";
        ui_min     = 1.0;
        ui_max     = 1280.0;
        ui_step    = 1.0;
        hidden     = !ADVANCED_SETTINGS;

    ui_category = "Phosphor Mask";
> = mask_num_triads_across_static;

uniform float scale_triad_height<
        ui_label   = "Scale Triad Height";
        ui_tooltip = "Scales the height of a triad";
        ui_type    = "drag";
        ui_min     = 0.01;
        ui_max     = 10.0;
        ui_step    = 0.001;

    ui_spacing = 2;
    ui_category = "Phosphor Mask";
> = 1.0;

uniform float2 phosphor_thickness <
        ui_label   = "Phosphor Thickness XY";
        ui_tooltip = "Makes the phosphors appear thicker in each direction";
        ui_type    = "drag";
        ui_min     = 0.01;
        ui_max     = 0.99;
        ui_step    = 0.01;
        // hidden     = !ADVANCED_SETTINGS;

    ui_category = "Phosphor Mask";
> = 0.2;

uniform float2 phosphor_sharpness <
        ui_label   = "Phosphor Sharpness XY";
        ui_tooltip = "Makes the phosphors appear more crisp in each direction";
        ui_type    = "drag";
        ui_min     = 1;
        ui_max     = 100;
        ui_step    = 1;
        // hidden     = !ADVANCED_SETTINGS;

    ui_category = "Phosphor Mask";
> = 50;

uniform float3 phosphor_offset_x <
        ui_label   = "Phosphor Offset RGB X";
        ui_tooltip = "Very slightly shifts the phosphor mask. Can help with subpixel alignment.";
        ui_type    = "drag";
        ui_min     = -1;
        ui_max     = 1;
        ui_step    = 0.01;
        // hidden     = !ADVANCED_SETTINGS;

    ui_spacing = 2;
    ui_category = "Phosphor Mask";
> = 0;

uniform float3 phosphor_offset_y <
        ui_label   = "Phosphor Offset RGB Y";
        ui_tooltip = "Very slightly shifts the phosphor mask. Can help with subpixel alignment.";
        ui_type    = "drag";
        ui_min     = -1;
        ui_max     = 1;
        ui_step    = 0.01;
        // hidden     = !ADVANCED_SETTINGS;

    ui_category = "Phosphor Mask";
> = 0;

// static const uint pixel_grid_mode = 0;
// static const float2 pixel_size = 1;
/*
// ==== PIXELATION ===
uniform uint pixel_grid_mode <
        #if !HIDE_HELP_SECTIONS
        ui_text    = "- Fix issues displaying pixel art.\n"
                     "- Force high-res games to look low-res.\n\n";
        #endif
        ui_label   = "Pixel Grid Param";
        ui_tooltip = "Switch between using Pixel Size or Num Pixels";
        ui_type    = "combo";
        ui_items   = "Pixel Size\0"
                    "Content Resolution\0";
        hidden     = !ADVANCED_SETTINGS;

    ui_category = "Pixelation";
    ui_category_closed = true;
> = 0;

uniform float2 pixel_size <
        #if !HIDE_HELP_SECTIONS && !ADVANCED_SETTINGS
        ui_text    = "- Fix issues displaying pixel art.\n"
                     "- Force high-res games to look low-res.\n\n";
        #endif
        ui_label   = "Pixel Size";
        ui_tooltip = "The size of an in-game pixel on screen, in real-world pixels";
        ui_type    = "slider";
        ui_min     = 1.0;
        ui_max     = 30.0;
        ui_step    = 1.0;

    ui_category = "Pixelation";
    ui_category_closed = true;
> = float2(1, 1);

uniform float2 pixel_grid_resolution <
        ui_label   = "Num Pixels";
        ui_tooltip = "The number of in-game pixels displayed on-screen in each direction";
        ui_type    = "drag";
        ui_min     = 1.0;
        ui_max     = 10000.0;
        ui_step    = 1.0;
        hidden     = !ADVANCED_SETTINGS;

    ui_category = "Pixelation";
> = content_size;
uniform float2 pixel_grid_offset <
        ui_label   = "Pixel Grid Offset";
        ui_tooltip = "Shifts the pixel-grid to help with alignment";
        ui_type    = "slider";
        ui_min     = -15.0;
        ui_max     = 15.0;
        ui_step    = 1.0;

    #if ADVANCED_SETTINGS
    ui_spacing = 2;
    #endif
    ui_category = "Pixelation";
> = float2(0, 0);
*/

// ==== SCANLINES ====
uniform uint scanline_thickness <
        #if !HIDE_HELP_SECTIONS
        ui_text    = "Configure the electron beams and interlacing.\n\n";
        #endif
        ui_label   = "Scanline Thickness";
        ui_tooltip = "Sets the height of each scanline";
        ui_type    = "slider";
        ui_min     = 1;
        ui_max     = 30;
        ui_step    = 1;

    ui_category = "Scanlines";
    ui_category_closed = true;
> = 2;

uniform float scanline_offset <
        ui_label   = "Scanline Offset";
        ui_tooltip = "Vertically shifts the scanlines to help with alignment";
        ui_type    = "slider";
        ui_min     = -30;
        ui_max     = 30;
        ui_step    = 1;
        hidden     = !ADVANCED_SETTINGS;

    ui_category = "Scanlines";
> = 0;

uniform uint beam_shape_mode <
        ui_label   = "Beam Shape Mode";
        ui_tooltip = "Select the kind of beam to use.";
        ui_type    = "combo";
        ui_items   = "Digital (Fast)\0"
                    "Linear (Simple)\0"
                    "Gaussian (Realistic)\0"
                    "Multi-Source Gaussian (Expensive)\0";

    ui_category = "Scanlines";
> = 1;

uniform bool enable_interlacing <
        ui_label   = "Enable Interlacing";

    ui_spacing = 5;
    ui_category = "Scanlines";
> = false;

uniform bool interlace_back_field_first <
        ui_label   = "Draw Back-Field First";
        ui_tooltip = "Draw odd-numbered scanlines first (often has no effect)";

    ui_category = "Scanlines";
> = interlace_back_field_first_static;

uniform uint scanline_deinterlacing_mode <
        ui_label   = "Deinterlacing Mode";
        ui_tooltip = "Selects the deinterlacing algorithm, if any.";
        ui_type    = "combo";
        ui_items   = "None\0"
                     "Fake-Progressive\0"  
                     "Weaving\0"
                     "Blended Weaving\0";

    ui_category = "Scanlines";
> = 1;

uniform float deinterlacing_blend_gamma <
        ui_label   = "Deinterlacing Blend Gamma";
        ui_tooltip = "Nudge this if deinterlacing changes your colors too much";
        ui_type    = "slider";
        ui_min     = 0.01;
        ui_max     = 5.0;
        ui_step    = 0.01;

    ui_category = "Scanlines";
> = 1.0;

uniform float linear_beam_thickness <
        ui_label   = "Linear Beam Thickness";
        ui_tooltip = "Linearly widens or narrows the beam";
        ui_type    = "slider";
        ui_min     = 0.01;
        ui_max     = 3.0;
        ui_step    = 0.01;

    ui_spacing = 5;
    ui_category = "Scanlines";
> = 1.0;

uniform float gaussian_beam_min_sigma <
        ui_label   = "Gaussian Beam Min Sigma";
        ui_tooltip = "For Gaussian Beam Shape, sets thickness of dim pixels";
        ui_type    = "drag";
        ui_min     = 0.0;
        ui_step    = 0.01;

    ui_spacing = 5;
    ui_category = "Scanlines";
> = gaussian_beam_min_sigma_static;

uniform float gaussian_beam_max_sigma <
        ui_label   = "Gaussian Beam Max Sigma";
        ui_tooltip = "For Gaussian Beam Shape, sets thickness of bright pixels";
        ui_type    = "drag";
        ui_min     = 0.0;
        ui_step    = 0.01;

    ui_category = "Scanlines";
> = gaussian_beam_max_sigma_static;

uniform float gaussian_beam_spot_power <
        ui_label   = "Gaussian Beam Spot Power";
        ui_tooltip = "For Gaussian Beam Shape, balances between Min and Max Sigma";
        ui_type    = "drag";
        ui_min     = 0.0;
        ui_step    = 0.01;

    ui_category = "Scanlines";
> = gaussian_beam_spot_power_static;

uniform float gaussian_beam_min_shape <
        ui_label   = "Gaussian Beam Min Shape";
        ui_tooltip = "For Gaussian Beam Shape, sets sharpness of dim pixels";
        ui_type    = "drag";
        ui_min     = 0.0;
        ui_step    = 0.01;
        hidden     = !ADVANCED_SETTINGS;

    ui_spacing = 2;
    ui_category = "Scanlines";
> = gaussian_beam_min_shape_static;

uniform float gaussian_beam_max_shape <
        ui_label   = "Gaussian Beam Max Shape";
        ui_tooltip = "For Gaussian Beam Shape, sets sharpness of bright pixels";
        ui_type    = "drag";
        ui_min     = 0.0;
        ui_step    = 0.01;
        hidden     = !ADVANCED_SETTINGS;

    ui_category = "Scanlines";
> = gaussian_beam_max_shape_static;

uniform float gaussian_beam_shape_power <
        ui_label   = "Gaussian Beam Shape Power";
        ui_tooltip = "For Gaussian Beam Shape, balances between Min and Max Shape";
        ui_type    = "drag";
        ui_min     = 0.0;
        ui_step    = 0.01;
        hidden     = !ADVANCED_SETTINGS;

    ui_category = "Scanlines";
> = gaussian_beam_shape_power_static;

uniform float3 convergence_offset_x <
        ui_label   = "Convergence Offset X RGB";
        ui_tooltip = "Shift the color channels horizontally";
        ui_type    = "drag";
        ui_min     = -10;
        ui_max     = 10;
        ui_step    = 0.05;
        hidden     = !ADVANCED_SETTINGS;

    ui_spacing = 5;
    ui_category = "Scanlines";
> = 0;
uniform float3 convergence_offset_y <
        ui_label   = "Convergence Offset Y RGB";
        ui_tooltip = "Shift the color channels vertically";
        ui_type    = "drag";
        ui_min     = -10;
        ui_max     = 10;
        ui_step    = 0.05;
        hidden     = !ADVANCED_SETTINGS;
    ui_category = "Scanlines";
> = 0;

static uint beam_horiz_filter = beam_horiz_filter_static;
static float beam_horiz_sigma = beam_horiz_sigma_static;
static float beam_horiz_linear_rgb_weight = beam_horiz_linear_rgb_weight_static;

// ==== IMAGE COLORIZATION ====
uniform float crt_gamma <
        #if !HIDE_HELP_SECTIONS
        ui_text    = "Apply gamma, contrast, and blurring.\n\n";
        #endif
        ui_label   = "CRT Gamma";
        ui_tooltip = "The gamma-level of the original content";
        ui_type    = "slider";
        ui_min     = 1.0;
        ui_max     = 5.0;
        ui_step    = 0.01;

    ui_category = "Colors and Effects";
    ui_category_closed = true;
> = crt_gamma_static;

uniform float lcd_gamma <
        ui_label   = "LCD Gamma";
        ui_tooltip = "The gamma-level of your display";
        ui_type    = "slider";
        ui_min     = 1.0;
        ui_max     = 5.0;
        ui_step    = 0.01;

    ui_category = "Colors and Effects";
> = lcd_gamma_static;

uniform float levels_contrast <
        ui_label   = "Levels Contrast";
        ui_tooltip = "Sets the contrast of the CRT";
        ui_type    = "slider";
        ui_min     = 0.0;
        ui_max     = 4.0;
        ui_step    = 0.01;

    ui_spacing = 5;
    ui_category = "Colors and Effects";
> = levels_contrast_static;

uniform float halation_weight <
        ui_label   = "Halation";
        ui_tooltip = "Desaturation due to eletrons exciting the wrong phosphors";
        ui_type    = "slider";
        ui_min     = 0.0;
        ui_max     = 1.0;
        ui_step    = 0.01;
        
    ui_spacing = 2;
    ui_category = "Colors and Effects";
> = halation_weight_static;

uniform float diffusion_weight <
        ui_label   = "Diffusion";
        ui_tooltip = "Blurring due to refraction from the screen's glass";
        ui_type    = "slider";
        ui_min     = 0.0;
        ui_max     = 1.0;
        ui_step    = 0.01;

    ui_category = "Colors and Effects";
> = diffusion_weight_static;

uniform float blur_radius <
        ui_label   = "Blur Radius";
        ui_tooltip = "Scales the radius of the halation and diffusion effects";
        ui_type    = "slider";
        ui_min     = 0.01;
        ui_max     = 5.0;
        ui_step    = 0.01;
        hidden     = !ADVANCED_SETTINGS;

    ui_category = "Colors and Effects";
> = 1.0;

uniform float bloom_underestimate_levels <
        ui_label   = "Bloom Underestimation";
        ui_tooltip = "Scale the bloom effect's intensity";
        ui_type    = "drag";
        ui_min     = FIX_ZERO(0.0);
        ui_step    = 0.01;

    ui_spacing = 2;
    ui_category = "Colors and Effects";
> = bloom_underestimate_levels_static;

uniform float bloom_excess <
        ui_label   = "Bloom Excess";
        ui_tooltip = "Extra bloom applied to all colors";
        ui_type    = "slider";
        ui_min     = 0.0;
        ui_max     = 1.0;
        ui_step    = 0.01;

    ui_category = "Colors and Effects";
> = bloom_excess_static;

uniform float2 aa_subpixel_r_offset_runtime <
        ui_label   = "AA Subpixel R Offet XY";
        ui_type    = "drag";
        ui_min     = -0.5;
        ui_max     = 0.5;
        ui_step    = 0.01;
        hidden     = !ADVANCED_SETTINGS || !_RUNTIME_ANTIALIAS_SUBPIXEL_OFFSETS;

    ui_category = "Colors and Effects";
> = aa_subpixel_r_offset_static;

static const float aa_cubic_c = aa_cubic_c_static;
static const float aa_gauss_sigma = aa_gauss_sigma_static;


// ==== GEOMETRY ====
uniform uint geom_rotation_mode <
        #if !HIDE_HELP_SECTIONS
        ui_text    = "Change the geometry of the screen's glass.\n\n";
        #endif
        ui_label   = "Rotate Screen";
        ui_type    = "combo";
        ui_items   = "0 degrees\0"
                     "90 degrees\0"
                     "180 degrees\0"
                     "270 degrees\0";

    ui_category = "Screen Geometry";
    ui_category_closed = true;
> = 0;
uniform uint geom_mode_runtime <
        ui_label   = "Geometry Mode";
        ui_tooltip = "Select screen curvature type";
        ui_type    = "combo";
        ui_items   = "Flat\0"
                    "Spherical\0"
                    "Spherical (Alt)\0"
                    "Cylindrical (Trinitron)\0";
                    
    ui_category = "Screen Geometry";
> = geom_mode_static;

uniform float geom_radius <
        ui_label   = "Geometry Radius";
        ui_tooltip = "Select screen curvature radius";
        ui_type    = "slider";
        ui_min     = 1.0 / (2.0 * pi);
        ui_max     = 1024;
        ui_step    = 0.01;

    ui_category = "Screen Geometry";
> = geom_radius_static;

uniform float geom_view_dist <
        ui_label   = "View Distance";
        ui_type    = "slider";
        ui_min     = 0.5;
        ui_max     = 1024;
        ui_step    = 0.01;
        hidden     = !ADVANCED_SETTINGS;

    ui_spacing = 2;
    ui_category = "Screen Geometry";
> = geom_view_dist_static;

uniform float2 geom_tilt_angle <
        ui_label   = "Screen Tilt Angles";
        ui_type    = "drag";
        ui_min     = -pi;
        ui_max     = pi;
        ui_step    = 0.01;
        hidden     = !ADVANCED_SETTINGS;

    ui_category = "Screen Geometry";
> = geom_tilt_angle_static;

uniform float2 geom_aspect_ratio <
        ui_label   = "Screen Aspect Ratios";
        ui_type    = "drag";
        ui_min     = 1.0;
        ui_step    = 0.01;
        hidden     = !ADVANCED_SETTINGS;

    ui_category = "Screen Geometry";
> = float2(geom_aspect_ratio_static, 1);
uniform float2 geom_overscan <
        ui_label   = "Geom Overscan";
        ui_type    = "drag";
        ui_min     = FIX_ZERO(0.0);
        ui_step    = 0.01;
        hidden     = !ADVANCED_SETTINGS;

    ui_spacing = 2;
    ui_category = "Screen Geometry";
> = geom_overscan_static;

// ==== BORDER ====
uniform float border_size <
        #if !HIDE_HELP_SECTIONS
        ui_text    = "Apply a thin vignette to the edge of the screen.\n\n";
        #endif
        ui_label   = "Border Size";
        ui_category_closed = true;
        ui_type    = "slider";
        ui_min     = 0.0;
        ui_max     = 0.5;
        ui_step    = 0.01;

    ui_category = "Screen Border";
> = border_size_static;

uniform float border_darkness <
        ui_label   = "Border Darkness";
        ui_type    = "drag";
        ui_min     = 0.0;
        ui_step    = 0.01;

    ui_category = "Screen Border";
> = border_darkness_static;

uniform float border_compress <
        ui_label   = "Border Compress";
        ui_type    = "drag";
        ui_min     = 0.0;
        ui_step    = 0.01;
        
    ui_category = "Screen Border";
> = border_compress_static;

// ==== PREBLUR ====
#if ENABLE_PREBLUR
    uniform float2 preblur_effect_radius <
            #if !HIDE_HELP_SECTIONS
            ui_text    = "- Apply a linear blur to the input image. Kind of like an NTSC/Composite shader, but much faster.\n"
                         "- If you want to use an NTSC shader or don't like this effect, disable it by setting ENABLE_PREBLUR to 0\n"
                         "- If you leave all of these set to 0, then they don't do anything. Consider disabling the effect to improve performance.\n\n";
            #endif
            ui_type    = "drag";
            ui_min     = 0;
            ui_max     = 100;
            ui_step    = 1;
            ui_label   = "Effect Radius XY";
            ui_tooltip = "The radius of the effect visible on the screen (measured in pixels)";
        
        ui_category   = "Pre-Blur";
        ui_category_closed = true;
    > = 0;
    uniform uint2 preblur_sampling_radius <
            ui_type = "drag";
            ui_min = 0;
            ui_max = 100;
            ui_step = 1;
            ui_label = "Sampling Radius XY";
            ui_tooltip = "The number of samples to take on either side of each pixel";
        
        ui_category   = "Pre-Blur";
    > = 0;
#else
    static const float2 preblur_effect_radius = 0;
    static const uint2 preblur_sampling_radius = 0;
#endif

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
    return geom_overscan;
}

float2 get_geom_tilt_angle_vector()
{
    return geom_tilt_angle;
}

float3 get_convergence_offsets_x_vector()
{
    return convergence_offset_x;
}

float3 get_convergence_offsets_y_vector()
{
    return convergence_offset_y;
}

float2 get_convergence_offsets_r_vector()
{
    return float2(convergence_offset_x.r, convergence_offset_y.r);
}

float2 get_convergence_offsets_g_vector()
{
    return float2(convergence_offset_x.g, convergence_offset_y.g);
}

float2 get_convergence_offsets_b_vector()
{
    return float2(convergence_offset_x.b, convergence_offset_y.b);
}

float2 get_aa_subpixel_r_offset()
{
    #if _RUNTIME_ANTIALIAS_WEIGHTS
        #if _RUNTIME_ANTIALIAS_SUBPIXEL_OFFSETS
            //  WARNING: THIS IS EXTREMELY EXPENSIVE.
            return aa_subpixel_r_offset_runtime;
        #else
            return aa_subpixel_r_offset_static;
        #endif
    #else
        return aa_subpixel_r_offset_static;
    #endif
}

//  Provide accessors settings which still need "cooking:"
float get_mask_amplify()
{
    static const float mask_grille_amplify = 1.0/mask_grille_avg_color;
    static const float mask_slot_amplify = 1.0/mask_slot_avg_color;
    static const float mask_shadow_amplify = 1.0/mask_shadow_avg_color;

    float mask_amplify;
    [flatten]
    switch (mask_type) {
        case 0:
            mask_amplify = mask_grille_amplify;
            break;
        case 1:
            mask_amplify = mask_slot_amplify;
            break;
        case 2:
            mask_amplify = mask_shadow_amplify;
            break;
        case 3:
            mask_amplify = mask_grille_amplify;
            break;
        case 4:
            mask_amplify = mask_slot_amplify;
            break;
        default:
            mask_amplify = mask_shadow_amplify;
            break;
                    
    }
    
    return mask_amplify;
}

#endif  //  _BIND_SHADER_PARAMS_H