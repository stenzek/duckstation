/*

    CRT - Guest - NTSC (Copyright (C) 2018-2024 guest(r) - guest.r@gmail.com)

    Incorporates many good ideas and suggestions from Dr. Venom.

    I would also like give thanks to many Libretro forums members for
    continuous feedbacks, suggestions and caring about the shader.

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.

    This program is distributed in the hopes that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc, 59
    Temple Place - STE 330, Boston, MA 02111-1307, USA.

    Ported to ReShade by DevilSingh with some help from guest(r)

    Clean up & Duckstation specific fixes & improvements by John Novak.
    Thanks to Hyllian for the help & tips.

*/

#include "ReShade.fxh"

// ---------------------------------------------------------------------------
// NTSC
// ---------------------------------------------------------------------------

uniform float quality <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.0;
    ui_step     = 1.0;
    ui_label    = "Values (Info Only): SVideo = 0 | Composite = 1.0 | RF = 2.0";
    ui_category = "NTSC";
> = 0.0;

uniform float cust_artifacting <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 5.0;
    ui_step     = 0.1;
    ui_label    = "NTSC Custom Artifacting Value";
    ui_category = "NTSC";
    ui_spacing  = 2;
> = 1.0;

uniform float cust_fringing <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 5.0;
    ui_step     = 0.1;
    ui_label    = "NTSC Custom Fringing Value";
    ui_category = "NTSC";
> = 1.0;

uniform int ntsc_fields <
    ui_type     = "combo";
    ui_items    = "Auto\0"
                  "No\0"
                  "Yes\0";

    ui_label    = "NTSC Merge Fields";
    ui_category = "NTSC";
> = 0;

uniform int ntsc_phase <
    ui_type     = "combo";
    ui_items    = "Auto\0"
                  "2 Phase\0"
                  "3 Phase\0"
                  "Mixed\0";

    ui_label    = "NTSC Phase";
    ui_category = "NTSC";
> = 0;

uniform float ntsc_scale <
    ui_type     = "drag";
    ui_min      = 0.2;
    ui_max      = 2.5;
    ui_step     = 0.025;
    ui_label    = "NTSC Resolution Scaling";
    ui_category = "NTSC";
> = 1.0;

uniform float ntsc_taps <
    ui_type     = "drag";
    ui_min      = 6.0;
    ui_max      = 32.0;
    ui_step     = 1.0;
    ui_label    = "NTSC # of Taps (Filter Width)";
    ui_category = "NTSC";
> = 32.0;

uniform float ntsc_cscale1 <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 4.00;
    ui_step     = 0.05;
    ui_label    = "NTSC Chroma Scaling/Bleeding (2 Phase)";
    ui_category = "NTSC";
    ui_spacing  = 2;
> = 1.0;

uniform float ntsc_cscale2 <
    ui_type     = "drag";
    ui_min      = 0.2;
    ui_max      = 2.25;
    ui_step     = 0.05;
    ui_label    = "NTSC Chroma Scaling/Bleeding (3 Phase)";
    ui_category = "NTSC";
> = 1.0;

uniform float ntsc_sat <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 0.01;
    ui_label    = "NTSC Color Saturation";
    ui_category = "NTSC";
> = 1.0;

uniform float ntsc_brt <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.5;
    ui_step     = 0.01;
    ui_label    = "NTSC Brightness";
    ui_category = "NTSC";
> = 1.0;

uniform float ntsc_gamma <
    ui_type     = "drag";
    ui_min      = 0.25;
    ui_max      = 2.5;
    ui_step     = 0.025;
    ui_label    = "NTSC Filtering Gamma Correction";
    ui_category = "NTSC";
> = 1.0;

uniform float ntsc_rainbow <
    ui_type     = "drag";
    ui_min      = -1.0;
    ui_max      = 1.0;
    ui_step     = 0.1;
    ui_label    = "NTSC Coloring/Rainbow Effect";
    ui_category = "NTSC";
> = 0.0;

uniform float ntsc_ring <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.1;
    ui_label    = "NTSC Anti-Ringing";
    ui_category = "NTSC";
    ui_spacing  = 2;
> = 0.5;

uniform float ntsc_shrp <
    ui_type     = "drag";
    ui_min      = -10.0;
    ui_max      = 10.0;
    ui_step     = 0.5;
    ui_label    = "NTSC Sharpness (Negative: Adaptive)";
    ui_category = "NTSC";
> = 0.0;

uniform float ntsc_shpe <
    ui_type     = "drag";
    ui_min      = 0.5;
    ui_max      = 1.0;
    ui_step     = 0.05;
    ui_label    = "NTSC Sharpness Shape";
    ui_category = "NTSC";
> = 0.75;

uniform float CSHARPEN <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 5.0;
    ui_step     = 0.1;
    ui_label    = "FSharpen - Sharpen Strength";
    ui_category = "FSharpen";
> = 0.0;

uniform float CCONTR <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.25;
    ui_step     = 0.01;
    ui_label    = "FSharpen - Sharpen Contrast/Ringing";
    ui_category = "FSharpen";
> = 0.05;

// ---------------------------------------------------------------------------
// FSharpen
// ---------------------------------------------------------------------------

uniform float CDETAILS <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.05;
    ui_label    = "FSharpen - Sharpen Details";
    ui_category = "FSharpen";
> = 1.0;

uniform float DEBLUR <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 7.0;
    ui_step     = 0.25;
    ui_label    = "FSharpen - Deblur Strength";
    ui_category = "FSharpen";
> = 1.0;

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

uniform float PR <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.5;
    ui_step     = 0.01;
    ui_label    = "Persistence 'R'";
    ui_category = "Persistence";
> = 0.32;

uniform float PG <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.5;
    ui_step     = 0.01;
    ui_label    = "Persistence 'G'";
    ui_category = "Persistence";
> = 0.32;

uniform float PB <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.5;
    ui_step     = 0.01;
    ui_label    = "Persistence 'B'";
    ui_category = "Persistence";
> = 0.32;

uniform float AS <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.6;
    ui_step     = 0.01;
    ui_label    = "Afterglow Strength";
    ui_category = "Persistence";
> = 0.2;

uniform float sat <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.01;
    ui_label    = "Afterglow Saturation";
    ui_category = "Persistence";
> = 0.5;

// ---------------------------------------------------------------------------
// Color
// ---------------------------------------------------------------------------

uniform int CS <
    ui_type     = "combo";
    ui_items    = "sRGB\0"
                  "Modern\0"
                  "DCI\0"
                  "Adobe\0"
                  "Rec. 2020\0";

    ui_label    = "Display Gamut";
    ui_category = "Color";
> = 0;

uniform int CP <
    ui_type     = "combo";
    ui_items    = "Off\0"
                  "EBU\0"
                  "P22\0"
                  "SMPTE-C\0"
                  "Philips\0"
                  "Trinitron 1\0"
                  "Trinitron 2\0";

    ui_label    = "CRT Profile";
    ui_category = "Color";
> = 0;

uniform int TNTC <
    ui_type     = "combo";
    ui_items    = "Off\0"
                  "Trinitron 1\0"
                  "Trinitron 2\0"
                  "Nec MultiSync\0"
                  "NTSC\0";

    ui_label    = "LUT Colors";
    ui_category = "Color";
> = 0;

uniform float LUTLOW <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 50.0;
    ui_step     = 1.0;
    ui_label    = "Fix LUT Dark Range";
    ui_category = "Color";
> = 5.0;

uniform float LUTBR <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.01;
    ui_label    = "Fix LUT Brightness";
    ui_category = "Color";
> = 1.0;

uniform float WP <
    ui_type     = "drag";
    ui_min      = -100.0;
    ui_max      = 100.0;
    ui_step     = 5.0;
    ui_label    = "Color Temperature %";
    ui_category = "Color";
> = 0.0;

uniform float wp_saturation <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 0.05;
    ui_label    = "Saturation Adjustment";
    ui_category = "Color";
> = 1.0;

uniform float clp <
    ui_type     = "drag";
    ui_min      = -1.0;
    ui_max      = 1.0;
    ui_step     = 0.05;
    ui_label    = "Clip Saturated Color Beams";
    ui_category = "Color";
> = 0.0;

// ---------------------------------------------------------------------------
// Brightness
// ---------------------------------------------------------------------------

uniform float gamma_i <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 5.0;
    ui_step     = 0.05;
    ui_label    = "Gamma Input";
    ui_category = "Brightness / Gamma";
> = 2.00;

uniform float gamma_o <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 5.0;
    ui_step     = 0.05;
    ui_label    = "Gamma Out";
    ui_category = "Brightness / Gamma";
> = 1.95;

uniform float gamma_c <
    ui_type     = "drag";
    ui_min      = 0.5;
    ui_max      = 2.0;
    ui_step     = 0.025;
    ui_label    = "Gamma Correct";
    ui_category = "Brightness / Gamma";
> = 1.0;

uniform float brightboost1 <
    ui_type     = "drag";
    ui_min      = 0.25;
    ui_max      = 10.0;
    ui_step     = 0.05;
    ui_label    = "Bright Boost Dark Pixels";
    ui_category = "Brightness / Gamma";
    ui_spacing  = 2;
> = 1.4;

uniform float brightboost2 <
    ui_type     = "drag";
    ui_min      = 0.25;
    ui_max      = 3.0;
    ui_step     = 0.025;
    ui_label    = "Bright Boost Bright Pixels";
    ui_category = "Brightness / Gamma";
> = 1.1;

uniform float pre_bb <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 0.01;
    ui_label    = "Brightness Adjustment";
    ui_category = "Brightness / Gamma";
> = 1.0;

uniform float contr <
    ui_type     = "drag";
    ui_min      = -2.0;
    ui_max      = 2.0;
    ui_step     = 0.05;
    ui_label    = "Contrast Adjustment";
    ui_category = "Brightness / Gamma";
> = 0.0;

uniform float sega_fix <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 1.0;
    ui_label    = "Sega Brightness Fix";
    ui_category = "Brightness / Gamma";
> = 0.0;

uniform float BP <
    ui_type     = "drag";
    ui_min      = -100.0;
    ui_max      = 25.0;
    ui_step     = 1.0;
    ui_label    = "Raise Black Level";
    ui_category = "Brightness / Gamma";
> = 0.0;

uniform float post_br <
    ui_type     = "drag";
    ui_min      = 0.25;
    ui_max      = 5.0;
    ui_step     = 0.01;
    ui_label    = "Post Brightness";
    ui_category = "Brightness / Gamma";
> = 1.0;

// ---------------------------------------------------------------------------
// Interlacing
// ---------------------------------------------------------------------------

uniform float interr <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 800.0;
    ui_step     = 25.0;
    ui_label    = "Interlace Trigger Resolution";
    ui_category = "Interlacing";
> = 400.0;

uniform int interm <
    ui_type     = "combo";
    ui_items    = "Off\0"
                  "Normal 1\0"
                  "Normal 2\0"
                  "Normal 3\0"
                  "Interpolation 1\0"
                  "Interpolation 2\0";

    ui_label    = "Interlace Mode";
    ui_category = "Interlacing";
> = 1;

uniform float iscanb <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.05;
    ui_label    = "Interlacing Scanlines Effect (Interlaced Brightness)";
    ui_category = "Interlacing";
> = 0.2;

uniform float iscans <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.05;
    ui_label    = "Interlacing Scanlines Saturation";
    ui_category = "Interlacing";
> = 0.25;

uniform float hiscan <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 1.0;
    ui_label    = "High Resolution Scanlines (Prepend A Scaler)";
    ui_category = "Interlacing";
> = 0.0;

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

uniform float intres <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 6.0;
    ui_step     = 0.5;
    ui_label    = "Internal Resolution Y: 0.5 | Y-Dowsample";
    ui_category = "Resolution";
> = 0.0;

uniform float downsample_levelx <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 0.05;
    ui_label    = "Downsampling-X (High-Res Content, Pre-Scalers)";
    ui_category = "Resolution";
> = 0.0;

uniform float downsample_levely <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 0.05;
    ui_label    = "Downsampling-Y (High-Res Content, Pre-Scalers)";
    ui_category = "Resolution";
> = 0.0;

// ---------------------------------------------------------------------------
// Sharpness
// ---------------------------------------------------------------------------

uniform float lsmooth <
    ui_type     = "drag";
    ui_min      = 0.5;
    ui_max      = 1.0;
    ui_step     = 0.01;
    ui_label    = "Raster Bloom Effect Smoothing";
    ui_category = "Sharpness";
> = 0.7;

uniform float HSHARPNESS <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 8.0;
    ui_step     = 0.05;
    ui_label    = "Horizontal Filter Range";
    ui_category = "Sharpness";
> = 1.6;

uniform float SIGMA_HOR <
    ui_type     = "drag";
    ui_min      = 0.1;
    ui_max      = 7.0;
    ui_step     = 0.025;
    ui_label    = "Horizontal Blur Sigma";
    ui_category = "Sharpness";
> = 0.8;

uniform float S_SHARPH <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 3.0;
    ui_step     = 0.05;
    ui_label    = "Substractive Sharpness";
    ui_category = "Sharpness";
> = 1.2;

uniform float HSHARP <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 0.1;
    ui_label    = "Sharpness Definition";
    ui_category = "Sharpness";
> = 1.2;

uniform float HARNG <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 4.0;
    ui_step     = 0.05;
    ui_label    = "Substractive Sharpness Ringing";
    ui_category = "Sharpness";
> = 0.3;

uniform float MAXS <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.3;
    ui_step     = 0.01;
    ui_label    = "Maximum Sharpness";
    ui_category = "Sharpness";
> = 0.18;

// ---------------------------------------------------------------------------
// Glow
// ---------------------------------------------------------------------------

uniform int m_glow <
    ui_type     = "combo";
    ui_items    = "Ordinary Glow\0"
                  "Magic Glow 1\0"
                  "Magic Glow 2\0";

    ui_label    = "Glow Type";
    ui_category = "Glow";
> = 0;

uniform float m_glow_cutoff <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.4;
    ui_step     = 0.01;
    ui_label    = "Magic Glow Cutoff";
    ui_category = "Glow";
> = 0.12;

uniform float m_glow_low <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 7.0;
    ui_step     = 0.05;
    ui_label    = "Magic Glow Low Strength";
    ui_category = "Glow";
> = 0.35;

uniform float m_glow_high <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 7.0;
    ui_step     = 0.1;
    ui_label    = "Magic Glow High Strength";
    ui_category = "Glow";
> = 5.0;

uniform float m_glow_dist <
    ui_type     = "drag";
    ui_min      = 0.2;
    ui_max      = 4.0;
    ui_step     = 0.05;
    ui_label    = "Magic Glow Distribution";
    ui_category = "Glow";
> = 1.0;

uniform float m_glow_mask <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 0.025;
    ui_label    = "Magic Glow Mask Strength";
    ui_category = "Glow";
> = 1.0;

uniform float FINE_GAUSS <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 5.0;
    ui_step     = 1.0;
    ui_label    = "Fine (Magic) Glow Sampling";
    ui_category = "Glow";
> = 1.0;

uniform float SIZEH <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 50.0;
    ui_step     = 1.0;
    ui_label    = "Horizontal Glow Radius";
    ui_category = "Glow";
    ui_spacing  = 2;
> = 6.0;

uniform float SIGMA_H <
    ui_type     = "drag";
    ui_min      = 0.2;
    ui_max      = 15.0;
    ui_step     = 0.05;
    ui_label    = "Horizontal Glow Sigma";
    ui_category = "Glow";
> = 1.2;

uniform float SIZEV <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 50.0;
    ui_step     = 1.0;
    ui_label    = "Vertical Glow Radius";
    ui_category = "Glow";
> = 6.0;

uniform float SIGMA_V <
    ui_type     = "drag";
    ui_min      = 0.2;
    ui_max      = 15.0;
    ui_step     = 0.05;
    ui_label    = "Vertical Glow Sigma";
    ui_category = "Glow";
> = 1.2;

uniform float glow <
    ui_type     = "drag";
    ui_min      = -2.0;
    ui_max      = 2.0;
    ui_step     = 0.01;
    ui_label    = "(Magic) Glow Strength";
    ui_category = "Glow";
    ui_spacing  = 2;
> = 0.08;

// ---------------------------------------------------------------------------
// Bloom / Halation
// ---------------------------------------------------------------------------

uniform float FINE_BLOOM <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 5.0;
    ui_step     = 1.0;
    ui_label    = "Fine Bloom/Halation Sampling";
    ui_category = "Bloom / Halation";
> = 1.0;

uniform float SIZEX <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 50.0;
    ui_step     = 1.0;
    ui_label    = "Horizontal Bloom/Halation Radius";
    ui_category = "Bloom / Halation";
> = 3.0;

uniform float SIGMA_X <
    ui_type     = "drag";
    ui_min      = 0.25;
    ui_max      = 15.0;
    ui_step     = 0.025;
    ui_label    = "Horizontal Bloom/Halation Sigma";
    ui_category = "Bloom / Halation";
> = 0.75;

uniform float SIZEY <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 50.0;
    ui_step     = 1.0;
    ui_label    = "Vertical Bloom/Halation Radius";
    ui_category = "Bloom / Halation";
> = 3.0;

uniform float SIGMA_Y <
    ui_type     = "drag";
    ui_min      = 0.25;
    ui_max      = 15.0;
    ui_step     = 0.025;
    ui_label    = "Vertical Bloom/Halation Sigma";
    ui_category = "Bloom / Halation";
> = 0.60;

uniform float blm_1 <
    ui_type     = "drag";
    ui_min      = -2.0;
    ui_max      = 2.0;
    ui_step     = 0.05;
    ui_label    = "Bloom Strength";
    ui_category = "Bloom / Halation";
    ui_spacing  = 2;
> = 0.0;

uniform float b_mask <
    ui_type     = "drag";
    ui_min      = -1.0;
    ui_max      = 1.0;
    ui_step     = 0.025;
    ui_label    = "Bloom Mask Strength";
    ui_category = "Bloom / Halation";
> = 0.0;

uniform float mask_bloom <
    ui_type     = "drag";
    ui_min      = -2.0;
    ui_max      = 2.0;
    ui_step     = 0.05;
    ui_label    = "Mask Bloom";
    ui_category = "Bloom / Halation";
> = 0.0;

uniform float bloom_dist <
    ui_type     = "drag";
    ui_min      = -2.0;
    ui_max      = 3.0;
    ui_step     = 0.05;
    ui_label    = "Bloom Distribution";
    ui_category = "Bloom / Halation";
> = 0.0;

uniform float halation <
    ui_type     = "drag";
    ui_min      = -2.0;
    ui_max      = 2.0;
    ui_step     = 0.025;
    ui_label    = "Halation Strength";
    ui_category = "Bloom / Halation";
    ui_spacing  = 2;
> = 0.0;

uniform float h_mask <
    ui_type     = "drag";
    ui_min      = -1.0;
    ui_max      = 1.0;
    ui_step     = 0.025;
    ui_label    = "Halation Mask Strength";
    ui_category = "Bloom / Halation";
> = 0.5;

// ---------------------------------------------------------------------------
// Scanlines
// ---------------------------------------------------------------------------

uniform int gsl <
    ui_type     = "combo";
    ui_items    = "Soft\0"
                  "Normal\0"
                  "Strong\0"
                  "Stronger\0";

    ui_label    = "Scanlines Type";
    ui_category = "Scanlines";
> = 0;

uniform float scanline1 <
    ui_type     = "drag";
    ui_min      = -20.0;
    ui_max      = 40.0;
    ui_step     = 0.5;
    ui_label    = "Scanlines Beam Shape Center";
    ui_category = "Scanlines";
> = 6.0;

uniform float scanline2 <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 70.0;
    ui_step     = 1.0;
    ui_label    = "Scanlines Beam Shape Edges";
    ui_category = "Scanlines";
> = 8.0;

uniform float beam_min <
    ui_type     = "drag";
    ui_min      = 0.25;
    ui_max      = 10.0;
    ui_step     = 0.05;
    ui_label    = "Scanlines Shape Dark Pixels";
    ui_category = "Scanlines";
> = 1.3;

uniform float beam_max <
    ui_type     = "drag";
    ui_min      = 0.2;
    ui_max      = 3.5;
    ui_step     = 0.025;
    ui_label    = "Scanlines Shape Bright Pixels";
    ui_category = "Scanlines";
> = 1.0;

uniform bool tds <
    ui_type     = "radio";
    ui_label    = "Thinner Dark Scanlines";
    ui_category = "Scanlines";
> = false;

uniform float beam_size <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.05;
    ui_label    = "Increased Bright Scanlines Beam";
    ui_category = "Scanlines";
> = 0.6;

uniform float scans <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 6.0;
    ui_step     = 0.1;
    ui_label    = "Scanlines Saturation / Mask Falloff";
    ui_category = "Scanlines";
> = 0.5;

uniform float scan_falloff <
    ui_type     = "drag";
    ui_min      = 0.1;
    ui_max      = 2.0;
    ui_step     = 0.025;
    ui_label    = "Scanlines Falloff";
    ui_category = "Scanlines";
> = 1.0;

uniform float spike <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 0.1;
    ui_label    = "Scanlines Spike Removal";
    ui_category = "Scanlines";
> = 1.0;

uniform float ssharp <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.3;
    ui_step     = 0.01;
    ui_label    = "Smart Sharpen Scanlines";
    ui_category = "Scanlines";
> = 0.0;

uniform float scangamma <
    ui_type     = "drag";
    ui_min      = 0.5;
    ui_max      = 5.0;
    ui_step     = 0.05;
    ui_label    = "Scanlines Gamma";
    ui_category = "Scanlines";
> = 2.4;

uniform float no_scanlines <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.5;
    ui_step     = 0.05;
    ui_label    = "No-Scanlines Mode";
    ui_category = "Scanlines";
> = 0.0;

// ---------------------------------------------------------------------------
// Scaling
// ---------------------------------------------------------------------------

uniform float IOS <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 4.0;
    ui_step     = 1.0;
    ui_label    = "Integer Scaling: Odd:Y | Even:X+Y";
    ui_category = "Scaling";
> = 0.0;

uniform float overscanx <
    ui_type     = "drag";
    ui_min      = -200.0;
    ui_max      = 200.0;
    ui_step     = 1.0;
    ui_label    = "Overscan X Original Pixels";
    ui_category = "Scaling";
> = 0.0;

uniform float overscany <
    ui_type     = "drag";
    ui_min      = -200.0;
    ui_max      = 200.0;
    ui_step     = 1.0;
    ui_label    = "Overscan Y Original Pixels";
    ui_category = "Scaling";
> = 0.0;

uniform float OS <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 1.0;
    ui_label    = "Raster Bloom Overscan Mode";
    ui_category = "Scaling";
> = 1.0;

uniform float blm_2 <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 20.0;
    ui_step     = 1.0;
    ui_label    = "Raster Bloom %";
    ui_category = "Scaling";
> = 0.0;

uniform int shadow_mask <
    ui_type     = "combo";
    ui_items    = "Off\0"
                  "CGWG\0"
                  "Lottes TV\0"
                  "Lottes Aperture\0"
                  "Lottes Stretched VGA\0"
                  "Lottes VGA\0"
                  "Trinitron 1\0"
                  "Trinitron 2\0"
                  "Trinitron B/W 1\0"
                  "Trinitron B/W 2\0"
                  "Trinitron Magenta/Green/Black\0"
                  "Trinitron RGBX\0"
                  "Trinitron 4k 1\0"
                  "Trinitron RRGGBBX\0"
                  "Trinitron 4k 2\0";

    ui_label    = "CRT Mask";
    ui_category = "Mask";
> = 1;

uniform float maskstr <
    ui_type     = "drag";
    ui_min      = -0.5;
    ui_max      = 1.0;
    ui_step     = 0.025;
    ui_label    = "Mask Strength (1, 6-14)";
    ui_category = "Mask";
> = 0.3;

uniform float mcut <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 0.05;
    ui_label    = "Mask 6-14 Low Strength";
    ui_category = "Mask";
> = 1.1;

uniform float maskboost <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 3.0;
    ui_step     = 0.05;
    ui_label    = "CRT Mask Boost";
    ui_category = "Mask";
> = 1.0;

uniform float masksize <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 4.0;
    ui_step     = 1.0;
    ui_label    = "CRT Mask Size";
    ui_category = "Mask";
> = 1.0;

uniform float mask_zoom <
    ui_type     = "drag";
    ui_min      = -5.0;
    ui_max      = 5.0;
    ui_step     = 1.0;
    ui_label    = "CRT Mask Zoom (+ Mask Width)";
    ui_category = "Mask";
> = 0.0;

uniform float zoom_mask <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.05;
    ui_label    = "CRT Mask Zoom Sharpen";
    ui_category = "Mask";
> = 0.0;

uniform float mshift <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.5;
    ui_label    = "(Transform to) Shadow Mask";
    ui_category = "Mask";
> = 0.0;

uniform int mask_layout <
    ui_type     = "combo";
    ui_items    = "RGB\0"
                  "BGR\0";

    ui_label    = "Mask Layout (Check LCD Panel)";
    ui_category = "Mask";
> = 0;

uniform float mask_drk <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 0.05;
    ui_label    = "Lottes Mask Dark";
    ui_category = "Mask";
    ui_spacing  = 2;
> = 0.5;

uniform float mask_lgt <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 0.05;
    ui_label    = "Lottes Mask Bright";
    ui_category = "Mask";
> = 1.5;

uniform float mask_gamma <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 5.0;
    ui_step     = 0.05;
    ui_label    = "Mask Gamma";
    ui_category = "Mask";
    ui_spacing  = 2;
> = 2.4;

uniform float slotmask1 <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.05;
    ui_label    = "Slot Mask Strength Bright Pixels";
    ui_category = "Mask";
    ui_spacing  = 2;
> = 0.0;

uniform float slotmask2 <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.05;
    ui_label    = "Slot Mask Strength Dark Pixels";
    ui_category = "Mask";
> = 0.0;

uniform float slotwidth <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 16.0;
    ui_step     = 1.0;
    ui_label    = "Slot Mask Width (0:Auto)";
    ui_category = "Mask";
> = 0.0;

uniform float double_slot <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 4.0;
    ui_step     = 1.0;
    ui_label    = "Slot Mask Height: 2x1 or 4x1";
    ui_category = "Mask";
> = 2.0;

uniform float slotms <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 4.0;
    ui_step     = 1.0;
    ui_label    = "Slot Mask Thickness";
    ui_category = "Mask";
> = 1.0;

uniform bool smoothmask <
    ui_type     = "radio";
    ui_label    = "Smooth Masks In Bright Scanlines";
    ui_category = "Mask";
    ui_spacing  = 2;
> = false;

uniform float smask_mit <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.05;
    ui_label    = "Mitigate Slot Mask Interaction";
    ui_category = "Mask";
> = 0.0;

uniform float bmask <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.25;
    ui_step     = 0.01;
    ui_label    = "Base (Black) Mask Strength";
    ui_category = "Mask";
> = 0.0;

uniform float mclip <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 0.025;
    ui_label    = "Preserve Mask Strength";
    ui_category = "Mask";
> = 0.0;

// ---------------------------------------------------------------------------
// Vignette / Border
// ---------------------------------------------------------------------------

uniform float vigstr <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 2.0;
    ui_step     = 0.05;
    ui_label    = "Vignette Strength";
    ui_category = "Vignette / Border";
> = 0.0;

uniform float vigdef <
    ui_type     = "drag";
    ui_min      = 0.5;
    ui_max      = 3.0;
    ui_step     = 0.1;
    ui_label    = "Vignette Size";
    ui_category = "Vignette / Border";
> = 1.0;

uniform float csize <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.25;
    ui_step     = 0.005;
    ui_label    = "Corner Size";
    ui_category = "Vignette / Border";
> = 0.0;

uniform float bsize <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 3.0;
    ui_step     = 0.01;
    ui_label    = "Border Size";
    ui_category = "Vignette / Border";
> = 0.01;

uniform float sborder <
    ui_type     = "drag";
    ui_min      = 0.25;
    ui_max      = 2.0;
    ui_step     = 0.05;
    ui_label    = "Border Intensity";
    ui_category = "Vignette / Border";
> = 0.75;

// ---------------------------------------------------------------------------
// Curvature
// ---------------------------------------------------------------------------

uniform float warpx <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.25;
    ui_step     = 0.01;
    ui_label    = "Curvature X";
    ui_category = "Curvature";
> = 0.0;

uniform float warpy <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.25;
    ui_step     = 0.01;
    ui_label    = "Curvature Y";
    ui_category = "Curvature";
> = 0.0;

uniform float c_shape <
    ui_type     = "drag";
    ui_min      = 0.05;
    ui_max      = 0.6;
    ui_step     = 0.05;
    ui_label    = "Curvature Shape";
    ui_category = "Curvature";
> = 0.25;

// ---------------------------------------------------------------------------
// Deconvergence
// ---------------------------------------------------------------------------

uniform float dctypex <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.75;
    ui_step     = 0.05;
    ui_label    = "Deconvergence Type X: 0:Static | Other:Dynamic";
    ui_category = "Deconvergence";
> = 0.0;

uniform float dctypey <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 0.75;
    ui_step     = 0.05;
    ui_label    = "Deconvergence Type Y: 0:Static | Other:Dynamic";
    ui_category = "Deconvergence";
> = 0.0;

uniform float deconrx <
    ui_type     = "drag";
    ui_min      = -15.0;
    ui_max      = 15.0;
    ui_step     = 0.25;
    ui_label    = "Horizontal Deconvergence 'R' Range";
    ui_category = "Deconvergence";
> = 0.0;

uniform float decongx <
    ui_type     = "drag";
    ui_min      = -15.0;
    ui_max      = 15.0;
    ui_step     = 0.25;
    ui_label    = "Horizontal Deconvergence 'G' Range";
    ui_category = "Deconvergence";
> = 0.0;

uniform float deconbx <
    ui_type     = "drag";
    ui_min      = -15.0;
    ui_max      = 15.0;
    ui_step     = 0.25;
    ui_label    = "Horizontal Deconvergence 'B' Range";
    ui_category = "Deconvergence";
> = 0.0;

uniform float deconry <
    ui_type     = "drag";
    ui_min      = -15.0;
    ui_max      = 15.0;
    ui_step     = 0.25;
    ui_label    = "Vertical Deconvergence 'R' Range";
    ui_category = "Deconvergence";
> = 0.0;

uniform float decongy <
    ui_type     = "drag";
    ui_min      = -15.0;
    ui_max      = 15.0;
    ui_step     = 0.25;
    ui_label    = "Vertical Deconvergence 'G' Range";
    ui_category = "Deconvergence";
> = 0.0;

uniform float deconby <
    ui_type     = "drag";
    ui_min      = -15.0;
    ui_max      = 15.0;
    ui_step     = 0.25;
    ui_label    = "Vertical Deconvergence 'B' Range";
    ui_category = "Deconvergence";
> = 0.0;

uniform float decons <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 3.0;
    ui_step     = 0.1;
    ui_label    = "Deconvergence Strength";
    ui_category = "Deconvergence";
> = 1.0;

// ---------------------------------------------------------------------------
// Noise
// ---------------------------------------------------------------------------

uniform float barspeed <
    ui_type     = "drag";
    ui_min      = 5.0;
    ui_max      = 200.0;
    ui_step     = 1.0;
    ui_label    = "Hum Bar Speed";
    ui_category = "Hum Bar";
> = 50.0;

uniform float barintensity <
    ui_type     = "drag";
    ui_min      = -1.0;
    ui_max      = 1.0;
    ui_step     = 0.01;
    ui_label    = "Hum Bar Intensity";
    ui_category = "Hum Bar";
> = 0.0;

uniform float bardir <
    ui_type     = "drag";
    ui_min      = 0.0;
    ui_max      = 1.0;
    ui_step     = 1.0;
    ui_label    = "Hum Bar Direction";
    ui_category = "Hum Bar";
> = 0.0;

// ---------------------------------------------------------------------------
// Noise
// ---------------------------------------------------------------------------

uniform float addnoised <
    ui_type     = "drag";
    ui_min      = -1.0;
    ui_max      = 1.0;
    ui_step     = 0.02;
    ui_label    = "Add Noise";
    ui_category = "Noise";
> = 0.0;

uniform float noiseresd <
    ui_type     = "drag";
    ui_min      = 1.0;
    ui_max      = 10.0;
    ui_step     = 1.0;
    ui_label    = "Noise Resolution";
    ui_category = "Noise";
> = 2.0;

uniform int noisetype <
    ui_type     = "combo";
    ui_items    = "Colored\0"
                  "Luma\0";

    ui_label    = "Noise Type";
    ui_category = "Noise";
> = 0;


uniform float  FrameCount     < source = "framecount"; >;

uniform float  NativeWidth    < source = "nativewidth"; >;
uniform float  NativeHeight   < source = "nativeheight"; >;
uniform float  InternalWidth  < source = "internalwidth"; >;
uniform float  InternalHeight < source = "internalheight"; >;
uniform float  BufferWidth    < source = "bufferwidth"; >;
uniform float  BufferHeight   < source = "bufferheight"; >;

uniform float  ViewportX      < source = "viewportx"; >;
uniform float  ViewportY      < source = "viewporty"; >;
uniform float2 ViewportOffset < source = "viewportoffset"; >;
uniform float  ViewportWidth  < source = "viewportwidth"; >;
uniform float  ViewportHeight < source = "viewportheight"; >;
uniform float2 ViewportSize   < source = "viewportsize"; >;

// InternalSize / NativeSize
uniform float  UpscaleMultiplier < source = "upscale_multiplier"; >;

// ViewportSize / InternalSize
uniform float2 InternalPixelSize < source = "internal_pixel_size"; >;

// ViewportSize / InternalSize / BufferSize
uniform float2 NormalizedInternalPixelSize < source = "normalized_internal_pixel_size"; >;

// ViewportSize / NativeSize
uniform float2 NativePixelSize < source = "native_pixel_size"; >;

// ViewportSize / NativeSize / BufferSize
uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;

// BufferSize / ViewportSize
uniform float2 BufferToViewportRatio < source = "buffer_to_viewport_ratio"; >;

#ifndef Resolution_X
#define Resolution_X BUFFER_WIDTH
#endif

#ifndef Resolution_Y
#define Resolution_Y BUFFER_HEIGHT
#endif

#define SIGNAL1      float2(4.0 * Resolution_X, Resolution_Y)
#define SIGNAL2      float2(2.0 * Resolution_X, Resolution_Y)

#define OutputSize   float4(BUFFER_SCREEN_SIZE, 1.0 / BUFFER_SCREEN_SIZE)

#define TextureSize  (1.0 / NormalizedNativePixelSize)
#define OriginalSize float4(TextureSize, 1.0 / TextureSize)

#define InputSize    float2(800.00000000, 600.00000000)
#define SourceSize   float4(InputSize, 1.0 / InputSize)

#define fuxcoord     (texcoord * 1.00001)
#define scans        1.5 * scans
#define eps          1e-8
#define pii          3.14159265
#define fracoord     (fuxcoord * OutputSize.xy)

#define COMPAT_TEXTURE(c, d)    tex2D(c, d)

#define NTSC_01      float4(SIGNAL1, 1.0 / SIGNAL1)
#define NTSC_02      float4(SIGNAL2, 1.0 / SIGNAL2)

#define mix_m        float3x3(BRIGHTNESS, ARTIFACT,       ARTIFACT, \
                              FRINGING,   2.0*SATURATION, 0.0,      \
                              FRINGING,   0.0,            2.0*SATURATION)

#define rgb_m        float3x3(0.299 , 0.587,  0.114, \
                              0.596, -0.274, -0.322, \
                              0.211, -0.523,  0.312)

#define yiq_m        float3x3(1.000,  0.956,  0.621, \
                              1.000, -0.272, -0.647, \
                              1.000, -1.106,  1.703)

#define tex_1        texcoord - float2(0.25 * OriginalSize.z / 4.0, 0.0)
#define tex_2        texcoord - float2(0.25 * OriginalSize.z / 4.0, 0.0)

#define inv_sqr_h    1.0 / (2.0 * SIGMA_H * SIGMA_H)
#define inv_sqr_v    1.0 / (2.0 * SIGMA_V * SIGMA_V)
#define inv_sqr_x    1.0 / (2.0 * SIGMA_X * SIGMA_X)
#define inv_sqr_y    1.0 / (2.0 * SIGMA_Y * SIGMA_Y)

#define fetch_offset1(dx)   tex2D(NTSC_S03, tex_1 + dx).xyz + \
                            tex2D(NTSC_S03, tex_1 - dx).xyz

#define fetch_offset2(dx)   float3(tex2D(NTSC_S03, tex_1 + dx.xz).x +  \
                                   tex2D(NTSC_S03, tex_1 - dx.xz).x,   \
                                   tex2D(NTSC_S03, tex_1 + dx.yz).yz + \
                                   tex2D(NTSC_S03, tex_1 - dx.yz).yz)

#define NTSC_S00     ReShade::BackBuffer

texture NTSC_T01
{
        Width  = 1.0 * Resolution_X;
        Height = Resolution_Y;
        Format = RGBA32F;
};
sampler NTSC_S01
{
        Texture   = NTSC_T01;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = POINT;
        MinFilter = POINT;
        MipFilter = POINT;
};

texture NTSC_T02
{
        Width  = 1.0 * Resolution_X;
        Height = Resolution_Y;
        Format = RGBA16F;
};
sampler NTSC_S02
{
        Texture   = NTSC_T02;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = POINT;
        MinFilter = POINT;
        MipFilter = POINT;
};

texture NTSC_T03
{
        Width  = 4.0 * Resolution_X;
        Height = Resolution_Y;
        Format = RGBA16F;
};
sampler NTSC_S03
{
        Texture   = NTSC_T03;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = LINEAR;
        MinFilter = LINEAR;
        MipFilter = LINEAR;
};

texture NTSC_T04
{
        Width  = 2.0 * Resolution_X;
        Height = Resolution_Y;
        Format = RGBA16F;
};
sampler NTSC_S04
{
        Texture   = NTSC_T04;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = LINEAR;
        MinFilter = LINEAR;
        MipFilter = LINEAR;
};

texture NTSC_T05
{
        Width  = 2.0 * Resolution_X;
        Height = Resolution_Y;
        Format = RGBA16F;
};
sampler NTSC_S05
{
        Texture   = NTSC_T05;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = LINEAR;
        MinFilter = LINEAR;
        MipFilter = LINEAR;
};

texture NTSC_T06
{
        Width  = 2.0 * Resolution_X;
        Height = Resolution_Y;
        Format = RGBA16F;
};
sampler NTSC_S06
{
        Texture   = NTSC_T06;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = LINEAR;
        MinFilter = LINEAR;
        MipFilter = LINEAR;
};

texture NTSC_T07
{
        Width  = 2.0 * Resolution_X;
        Height = Resolution_Y;
        Format = RGBA16F;
};
sampler NTSC_S07
{
        Texture   = NTSC_T07;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = LINEAR;
        MinFilter = LINEAR;
        MipFilter = LINEAR;
};

texture NTSC_T08
{
        Width  = 2.0 * Resolution_X;
        Height = Resolution_Y;
        Format = RGBA16F;
};
sampler NTSC_S08
{
        Texture   = NTSC_T08;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = LINEAR;
        MinFilter = LINEAR;
        MipFilter = LINEAR;
};

texture NTSC_T09
{
        Width  = 1.0 * BUFFER_WIDTH;
        Height = Resolution_Y;
        Format = RGBA16F;
};
sampler NTSC_S09
{
        Texture   = NTSC_T09;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = LINEAR;
        MinFilter = LINEAR;
        MipFilter = LINEAR;
};

texture NTSC_T10
{
        Width  = 1.0 * 800.00000000;
        Height = 600.00000000;
        Format = RGBA16F;
};
sampler NTSC_S10
{
        Texture   = NTSC_T10;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = LINEAR;
        MinFilter = LINEAR;
        MipFilter = LINEAR;
};

texture NTSC_T11
{
        Width  = 1.0 * 800.00000000;
        Height = 600.00000000;
        Format = RGBA16F;
};
sampler NTSC_S11
{
        Texture   = NTSC_T11;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = LINEAR;
        MinFilter = LINEAR;
        MipFilter = LINEAR;
};

texture NTSC_T12
{
        Width  = 1.0 * 800.00000000;
        Height = 600.00000000;
        Format = RGBA16F;
};
sampler NTSC_S12
{
        Texture   = NTSC_T12;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = LINEAR;
        MinFilter = LINEAR;
        MipFilter = LINEAR;
};

texture NTSC_T13
{
        Width  = 1.0 * 800.00000000;
        Height = 600.00000000;
        Format = RGBA16F;
};
sampler NTSC_S13
{
        Texture   = NTSC_T13;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = LINEAR;
        MinFilter = LINEAR;
        MipFilter = LINEAR;
};

texture NTSC_T14
{
        Width  = 1.0 * BUFFER_WIDTH;
        Height = BUFFER_HEIGHT;
        Format = RGBA16F;
};
sampler NTSC_S14
{
        Texture   = NTSC_T14;
        AddressU  = BORDER;
        AddressV  = BORDER;
        AddressW  = BORDER;
        MagFilter = LINEAR;
        MinFilter = LINEAR;
        MipFilter = LINEAR;
};

texture NTSC_001 < source = "CRT-LUT-1.png"; >
{
        Width  = 1024;
        Height = 32;
};
sampler NTSC_L01
{
        Texture = NTSC_001;
};

texture NTSC_002 < source = "CRT-LUT-2.png"; >
{
        Width  = 1024;
        Height = 32;
};
sampler NTSC_L02
{
        Texture = NTSC_002;
};

texture NTSC_003 < source = "CRT-LUT-3.png"; > {
        Width  = 1024;
        Height = 32;
};
sampler NTSC_L03
{
        Texture = NTSC_003;
};

texture NTSC_004 < source = "CRT-LUT-4.png"; >
{
        Width  = 1024;
        Height = 32;
};
sampler NTSC_L04
{
        Texture = NTSC_004;
};

float3 fix_lut(float3 lut, float3 ref)
{
    float r = length(ref);
    float l = length(lut);
    float m = max(max(ref.r, ref.g), ref.b);
    ref     = normalize(lut + 0.0000001) * lerp(r, l, pow(m, 1.25));
    return lerp(lut, ref, LUTBR);
}

float vignette(float2 pos)
{
    float2 b = vigdef * float2(1.0, ViewportWidth / ViewportHeight) * 0.125;
    pos = clamp(pos, 0.0, 1.0);
    pos = abs(2.0 * (pos - 0.5));
    float2 res = lerp(0.0.xx, 1.0.xx, smoothstep(1.0.xx, 1.0.xx - b, sqrt(pos)));
    res = pow(res, 0.70.xx);
    return max(lerp(1.0, sqrt(res.x * res.y), vigstr), 0.0);
}

float contrast(float x)
{
    return max(lerp(x, smoothstep(0.0, 1.0, x), contr), 0.0);
}

float dist(float3 A, float3 B)
{
    float r  = 0.5 * (A.r + B.r);
    float3 d = A - B;
    float3 c = float3(2. + r, 4., 3. - r);
    return sqrt(dot(c * d, d)) / 3.;
}

float3 plant(float3 tar, float r)
{
    float t = max(max(tar.r, tar.g), tar.b) + 0.00001;
    return tar * r / t;
}

float3 fetch_pixel(float2 coord)
{
    float2 dx = float2(NTSC_02.z, 0.0) * downsample_levelx * ViewportWidth  / NativeWidth;
    float2 dy = float2(0.0, NTSC_02.w) * downsample_levely * ViewportHeight / NativeHeight;
    float2 d1 = dx + dy;
    float2 d2 = dx - dy;
    float sum = 15.0;

    float3 result = 3.0 * COMPAT_TEXTURE(NTSC_S06, coord).rgb +
                    2.0 * COMPAT_TEXTURE(NTSC_S06, coord + dx).rgb +
                    2.0 * COMPAT_TEXTURE(NTSC_S06, coord - dx).rgb +
                    2.0 * COMPAT_TEXTURE(NTSC_S06, coord + dy).rgb +
                    2.0 * COMPAT_TEXTURE(NTSC_S06, coord - dy).rgb +
                    COMPAT_TEXTURE(NTSC_S06, coord + d1).rgb +
                    COMPAT_TEXTURE(NTSC_S06, coord - d1).rgb +
                    COMPAT_TEXTURE(NTSC_S06, coord + d2).rgb +
                    COMPAT_TEXTURE(NTSC_S06, coord - d2).rgb;

    return result / sum;
}

float crthd_h(float x, float y)
{
    float invsigmah = 1.0 / (2.0 * SIGMA_HOR * SIGMA_HOR * y * y);
    return exp(-x * x * invsigmah);
}

float gauss_h(float x)
{
    return exp(-x * x * inv_sqr_h);
}

float gauss_v(float x)
{
    return exp(-x * x * inv_sqr_v);
}

float bloom_h(float x)
{
    return exp(-x * x * inv_sqr_x);
}

float bloom_v(float x)
{
    return exp(-x * x * inv_sqr_y);
}

float mod(float x,float y)
{
    return x-y* floor(x/y);
}

float st0(float x)
{
    return exp2(-10.0 * x * x);
}

float st1(float x)
{
    return exp2(-8.0 * x * x);
}

float3 sw0(float x, float color, float scanline, float3 c)
{
    float3 xe = lerp(1.0.xxx + scans, 1.0.xxx, c);
    float tmp = lerp(beam_min, beam_max, color);
    float ex  = x * tmp;
    ex = (gsl > 0) ? ex * ex : lerp(ex * ex, ex * ex * ex, 0.4);
    return exp2(-scanline * ex * xe);
}

float3 sw1(float x, float color, float scanline, float3 c)
{
    float3 xe = lerp(1.0.xxx + scans, 1.0.xxx, c);
    x = lerp(x, beam_min * x, max(x - 0.4 * color, 0.0));
    float tmp = lerp(1.2 * beam_min, beam_max, color);
    float ex  = x * tmp;
    return exp2(-scanline * ex * ex * xe);
}

float3 sw2(float x, float color, float scanline, float3 c)
{
    float3 xe = lerp(1.0.xxx + scans, 1.0.xxx, c);
    float tmp = lerp((2.5 - 0.5 * color) * beam_min, beam_max, color);
    tmp = lerp(beam_max, tmp, pow(x, color + 0.3));
    float ex  = x * tmp;
    return exp2(-scanline * ex * ex * xe);
}

float2 overscan(float2 pos, float dx, float dy)
{
    pos = pos * 2.0 - 1.0;
    pos *= float2(dx, dy);
    return pos * 0.5 + 0.5;
}

float2 warp(float2 pos)
{
    pos = pos * 2.0 - 1.0;
    pos = lerp(pos,
               float2(pos.x * rsqrt(1.0 - c_shape * pos.y * pos.y),
                      pos.y * rsqrt(1.0 - c_shape * pos.x * pos.x)),
               float2(warpx, warpy) / c_shape);

    return pos * 0.5 + 0.5;
}

float3 gc(float3 c)
{
    float mc = max(max(c.r, c.g), c.b);
    float mg = pow(mc, 1.0 / gamma_c);
    return c * mg / (mc + eps);
}

float3 rgb2yiq(float3 r)
{
    return mul(rgb_m, r);
}

float3 yiq2rgb(float3 y)
{
    return mul(yiq_m, y);
}

float get_luma(float3 c)
{
    return dot(c, float3(0.2989, 0.5870, 0.1140));
}

float3 crt_mask(float2 pos, float mx, float mb)
{
    float3 mask = mask_drk;
    float3 one  = 1.0;

    // CGWG
    if (shadow_mask == 1) {
        float mc = 1.0 - max(maskstr, 0.0);
        pos.x    = frac(pos.x * 0.5);

        if (pos.x < 0.49) {
            mask.r = 1.0;
            mask.g = mc;
            mask.b = 1.0;
        } else {
            mask.r = mc;
            mask.g = 1.0;
            mask.b = mc;
        }

    // Lottes - Very compressed TV style shadow mask
    } else if (shadow_mask == 2) {
        float lane = mask_lgt;
        float odd  = 0.0;

        if (frac(pos.x / 6.0) < 0.49) {
            odd = 1.0;
        }
        if (frac((pos.y + odd) / 2.0) < 0.49) {
            lane = mask_drk;
        }

        pos.x = floor(mod(pos.x, 3.0));

        if      (pos.x < 0.5) mask.r = mask_lgt;
        else if (pos.x < 1.5) mask.g = mask_lgt;
        else                  mask.b = mask_lgt;

        mask *= lane;

    // Lottes - Aperture-grille
    } else if (shadow_mask == 3) {
        pos.x = floor(mod(pos.x, 3.0));

        if      (pos.x < 0.5) mask.r = mask_lgt;
        else if (pos.x < 1.5) mask.g = mask_lgt;
        else                  mask.b = mask_lgt;

    // Lottes - Stretched VGA style shadow mask (same as prior shaders)
    } else if (shadow_mask == 4) {
        pos.x += pos.y * 3.0;
        pos.x = frac(pos.x / 6.0);

        if      (pos.x < 0.3) mask.r = mask_lgt;
        else if (pos.x < 0.6) mask.g = mask_lgt;
        else                  mask.b = mask_lgt;

    // Lottes - VGA style shadow mask
    } else if (shadow_mask == 5) {
        pos.xy = floor(pos.xy * float2(1.0, 0.5));
        pos.x += pos.y * 3.0;
        pos.x = frac(pos.x / 6.0);

        if      (pos.x < 0.3) mask.r = mask_lgt;
        else if (pos.x < 0.6) mask.g = mask_lgt;
        else                  mask.b = mask_lgt;

    // Trinitron mask 1
    } else if (shadow_mask == 6) {
        mask  = 0.0;
        pos.x = frac(pos.x / 2.0);

        if (pos.x < 0.49) {
            mask.r = 1.0;
            mask.b = 1.0;
        } else {
            mask.g = 1.0;
        }
        mask = clamp(lerp(lerp(one, mask, mcut),
                          lerp(one, mask, maskstr), mx),
                     0.0, 1.0);

    // Trinitron mask 2
    } else if (shadow_mask == 7) {
        mask  = 0.0;
        pos.x = floor(mod(pos.x, 3.0));

        if      (pos.x < 0.5) mask.r = 1.0;
        else if (pos.x < 1.5) mask.g = 1.0;
        else                  mask.b = 1.0;

        mask = clamp(lerp(lerp(one, mask, mcut),
                          lerp(one, mask, maskstr), mx),
                     0.0, 1.0);

    // Trinitron B/W mask 1
    } else if (shadow_mask == 8) {
        mask  = 0.0;
        pos.x = frac(pos.x / 2.0);

        if (pos.x < 0.49) mask = 0.0.xxx;
        else              mask = 1.0.xxx;

        mask = clamp(lerp(lerp(one, mask, mcut),
                          lerp(one, mask, maskstr), mx),
                     0.0, 1.0);

    // Trinitron B/W mask 2
    } else if (shadow_mask == 9) {
        mask  = 0.0;
        pos.x = frac(pos.x / 3.0);

        if      (pos.x < 0.3) mask = 0.0.xxx;
        else if (pos.x < 0.6) mask = 1.0.xxx;
        else                  mask = 1.0.xxx;

        mask = clamp(lerp(lerp(one, mask, mcut), lerp(one, mask, maskstr), mx),
                     0.0, 1.0);

    // Trinitron Magenta - Green - Black mask
    } else if (shadow_mask == 10) {
        mask  = 0.0;
        pos.x = frac(pos.x / 3.0);

        if      (pos.x < 0.3) mask    = 0.0.xxx;
        else if (pos.x < 0.6) mask.rb = 1.0.xx;
        else                  mask.g  = 1.0;

        mask = clamp(lerp(lerp(one, mask, mcut),
                          lerp(one, mask, maskstr), mx),
                     0.0, 1.0);

    // Trinitron RGBX mask
    } else if (shadow_mask == 11) {
        mask  = 0.0;
        pos.x = frac(pos.x * 0.25);

        if      (pos.x < 0.2) mask   = 0.0.xxx;
        else if (pos.x < 0.4) mask.r = 1.0;
        else if (pos.x < 0.7) mask.g = 1.0;
        else                  mask.b = 1.0;

        mask = clamp(lerp(lerp(one, mask, mcut),
                          lerp(one, mask, maskstr), mx),
                     0.0, 1.0);

    // Trinitron 4k mask 1
    } else if (shadow_mask == 12) {
        mask  = 0.0;
        pos.x = frac(pos.x * 0.25);

        if      (pos.x < 0.2) mask.r  = 1.0;
        else if (pos.x < 0.4) mask.rg = 1.0.xx;
        else if (pos.x < 0.7) mask.gb = 1.0.xx;
        else                  mask.b  = 1.0;

        mask = clamp(lerp(lerp(one, mask, mcut),
                          lerp(one, mask, maskstr), mx),
                     0.0, 1.0);

    // Trinitron RRGGBBX mask
    } else if (shadow_mask == 13) {
        mask  = 0.0;
        pos.x = floor(mod(pos.x, 7.0));

        if      (pos.x < 0.5) mask   = 0.0.xxx;
        else if (pos.x < 2.5) mask.r = 1.0;
        else if (pos.x < 4.5) mask.g = 1.0;
        else                  mask.b = 1.0;

        mask = clamp(lerp(lerp(one, mask, mcut),
                          lerp(one, mask, maskstr), mx),
                     0.0, 1.0);

    // Trinitron 4k mask 2
    } else {
        mask  = 0.0;
        pos.x = floor(mod(pos.x, 6.0));

        if      (pos.x < 0.5) mask     = 0.0.xxx;
        else if (pos.x < 1.5) mask.r   = 1.0;
        else if (pos.x < 2.5) mask.rg  = 1.0.xx;
        else if (pos.x < 3.5) mask.rgb = 1.0.xxx;
        else if (pos.x < 4.5) mask.gb  = 1.0.xx;
        else                  mask.b   = 1.0;

        mask = clamp(lerp(lerp(one, mask, mcut),
                          lerp(one, mask, maskstr), mx),
                     0.0, 1.0);
    }

    if (mask_layout > 0.5) {
        mask = mask.rbg;
    }

    float maskmin = min(min(mask.r, mask.g), mask.b);

    return (mask - maskmin) * (1.0 + (maskboost - 1.0) * mb) + maskmin;
}

float slt_mask(float2 pos, float m, float swidth)
{
    if ((slotmask1 + slotmask2) == 0.0) {
        return 1.0;
    } else {
        pos.y = floor(pos.y / slotms);

        float mlen = swidth * 2.0;

        float px = floor(mod(pos.x, 0.99999 * mlen));
        float py = floor(frac(pos.y / (2.0 * double_slot)) * 2.0 * double_slot);

        float slot_dark = lerp(1.0 - slotmask2, 1.0 - slotmask1, m);
        float slot      = 1.0;

        if (py == 0.0 && px < swidth) {
            slot = slot_dark;
        } else if (py == double_slot && px >= swidth) {
            slot = slot_dark;
        }
        return slot;
    }
}

float humbars(float pos)
{
    if (barintensity == 0.0) {
        return 1.0;
    } else {
        pos = (barintensity >= 0.0) ? pos : (1.0 - pos);
        pos = frac(pos + mod(FrameCount, barspeed) / (barspeed - 1.0));
        pos = (barintensity < 0.0) ? pos : (1.0 - pos);

        return (1.0 - barintensity) + barintensity * pos;
    }
}

float corner(float2 pos)
{
    float vp_ratio = ViewportWidth / ViewportHeight;
    float2 bc = bsize * float2(1.0, vp_ratio) * 0.05;

    pos = clamp(pos, 0.0, 1.0);
    pos = abs(2.0 * (pos - 0.5));

    float csz = lerp(400.0, 7.0, pow(4.0 * csize, 0.10));
    float crn = dot(pow(pos, csz.xx * float2(1.0, 1.0 / vp_ratio)), 1.0.xx);
    crn = (csize == 0.0) ? max(pos.x, pos.y) : pow(crn, 1.0 / csz);

    pos = max(pos, crn);

    float2 rs = (bsize == 0.0) ? 1.0.xx
                               : lerp(0.0.xx, 1.0.xx,
                                      smoothstep(1.0.xx, 1.0.xx - bc, sqrt(pos)));

    rs = pow(rs, sborder.xx);

    return sqrt(rs.x * rs.y);
}

float3 declip(float3 c, float b)
{
    float m = max(max(c.r, c.g), c.b);
    if (m > b) {
        c = c * b / m;
    }
    return c;
}

float igc(float mc)
{
    return pow(mc, gamma_c);
}

float3 noise(float3 v)
{
    if (addnoised < 0.0) {
        v.z = -addnoised;
    } else {
        v.z = mod(v.z, 6001.0) / 1753.0;
    }
    v = frac(v) + frac(v * 1e4) + frac(v * 1e-4);
    v += float3(0.12345, 0.6789, 0.314159);
    v = frac(v * dot(v, v) * 123.456);
    v = frac(v * dot(v, v) * 123.456);
    v = frac(v * dot(v, v) * 123.456);
    v = frac(v * dot(v, v) * 123.456);
    return v;
}

void bring_pixel(inout float3 c, inout float3 b, inout float3 g, float2 coord,
                 float2 boord)
{
    float stepx = OutputSize.z;
    float stepy = OutputSize.w;

    float2 dx = float2(stepx, 0.0);
    float2 dy = float2(0.0, stepy);

    float posx = 2.0 * coord.x - 1.0;
    float posy = 2.0 * coord.y - 1.0;

    if (dctypex > 0.025) {
        posx = sign(posx) * pow(abs(posx), 1.05 - dctypex);
        dx   = posx * dx;
    }
    if (dctypey > 0.025) {
        posy = sign(posy) * pow(abs(posy), 1.05 - dctypey);
        dy   = posy * dy;
    }

    float2 rc = deconrx * dx + deconry * dy;
    float2 gc = decongx * dx + decongy * dy;
    float2 bc = deconbx * dx + deconby * dy;

    float r1 = COMPAT_TEXTURE(NTSC_S14, coord + rc).r;
    float g1 = COMPAT_TEXTURE(NTSC_S14, coord + gc).g;
    float b1 = COMPAT_TEXTURE(NTSC_S14, coord + bc).b;

    float ds = decons;
    float3 d = float3(r1, g1, b1);

    c  = clamp(lerp(c, d, ds), 0.0, 1.0);
    r1 = COMPAT_TEXTURE(NTSC_S13, boord + rc).r;
    g1 = COMPAT_TEXTURE(NTSC_S13, boord + gc).g;
    b1 = COMPAT_TEXTURE(NTSC_S13, boord + bc).b;
    d  = float3(r1, g1, b1);

    b = g = lerp(b, d, min(ds, 1.0));

    r1 = COMPAT_TEXTURE(NTSC_S11, boord + rc).r;
    g1 = COMPAT_TEXTURE(NTSC_S11, boord + gc).g;
    b1 = COMPAT_TEXTURE(NTSC_S11, boord + bc).b;
    d  = float3(r1, g1, b1);
    g  = lerp(g, d, min(ds, 1.0));
}

float4 AfterglowPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float2 dx = float2(OriginalSize.z, 0.0);
    float2 dy = float2(0.0, OriginalSize.w);

    float w = 1.0;

    float2 tc = texcoord + float2(mod(ViewportX + 1.0, 2.0) * 1.0 / BufferWidth,
                                  mod(ViewportY + 1.0, 2.0) * 1.0 / BufferHeight);

    float3 color0 = COMPAT_TEXTURE(NTSC_S00, tc.xy).rgb;
    float3 color1 = COMPAT_TEXTURE(NTSC_S00, tc.xy - dx).rgb;
    float3 color2 = COMPAT_TEXTURE(NTSC_S00, tc.xy + dx).rgb;
    float3 color3 = COMPAT_TEXTURE(NTSC_S00, tc.xy - dy).rgb;
    float3 color4 = COMPAT_TEXTURE(NTSC_S00, tc.xy + dy).rgb;

    float3 clr = (2.5 * color0 + color1 + color2 + color3 + color4) / 6.5;
    float3 a = COMPAT_TEXTURE(NTSC_S01, texcoord.xy).rgb;

    if ((color0.r + color0.g + color0.b < 5.0 / 255.0)) {
        w = 0.0;
    }

    float3 result = lerp(max(lerp(clr, a, 0.49 + float3(PR, PG, PB)) - 1.25 / 255.0,
                             0.0),
                         clr,
                         w);

    return float4(result, w);
}

float4 PreShaderPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    const float3x3 Profile0 = float3x3(0.412391, 0.212639, 0.019331,
                                       0.357584, 0.715169, 0.119195,
                                       0.180481, 0.072192, 0.950532);

    const float3x3 Profile1 = float3x3(0.430554, 0.222004, 0.020182,
                                       0.341550, 0.706655, 0.129553,
                                       0.178352, 0.071341, 0.939322);

    const float3x3 Profile2 = float3x3(0.396686, 0.210299, 0.006131,
                                       0.372504, 0.713766, 0.115356,
                                       0.181266, 0.075936, 0.967571);

    const float3x3 Profile3 = float3x3(0.393521, 0.212376, 0.018739,
                                       0.365258, 0.701060, 0.111934,
                                       0.191677, 0.086564, 0.958385);

    const float3x3 Profile4 = float3x3(0.392258, 0.209410, 0.016061,
                                       0.351135, 0.725680, 0.093636,
                                       0.166603, 0.064910, 0.850324);

    const float3x3 Profile5 = float3x3( 0.377923, 0.195679, 0.010514,
                                        0.317366, 0.722319, 0.097826,
                                        0.207738, 0.082002, 1.076960);


    const float3x3 ToRGB    = float3x3( 3.240970, -0.969244,  0.055630,
                                       -1.537383,  1.875968, -0.203977,
                                       -0.498611,  0.041555,  1.056972);

    const float3x3 ToModern = float3x3( 2.791723, -0.894766,  0.041678,
                                       -1.173165,  1.815586, -0.130886,
                                       -0.440973,  0.032000,  1.002034);

    const float3x3 ToDCI    = float3x3( 2.493497, -0.829489,  0.035846,
                                       -0.931384,  1.762664, -0.076172,
                                       -0.402711,  0.023625,  0.956885);

    const float3x3 ToAdobe  = float3x3( 2.041588, -0.969244,  0.013444,
                                       -0.565007,  1.875968, -0.118360,
                                       -0.344731,  0.041555,  1.015175);

    const float3x3 ToREC    = float3x3( 1.716651, -0.666684,  0.017640,
                                       -0.355671,  1.616481, -0.042771,
                                       -0.253366,  0.015769,  0.942103);


    const float3x3 D65_to_D55 = float3x3(0.4850339153, 0.2500956126, 0.0227359648,
                                         0.3488957224, 0.6977914447, 0.1162985741,
                                         0.1302823568, 0.0521129427, 0.6861537456);

    const float3x3 D65_to_D93 = float3x3(0.3412754080, 0.1759701322, 0.0159972847,
                                         0.3646170520, 0.7292341040, 0.1215390173,
                                         0.2369894093, 0.0947957637, 1.2481442225);

    float2 tc = texcoord + float2(mod(ViewportX + 1.0, 2.0) * 1.0 / BufferWidth,
                                  mod(ViewportY + 1.0, 2.0) * 1.0 / BufferHeight);

    float4 imgcolor  = COMPAT_TEXTURE(NTSC_S00, tc);
    float4 afterglow = COMPAT_TEXTURE(NTSC_S01, tc);

    float w = 1.0 - afterglow.w;
    float l = length(afterglow.rgb);

    afterglow.rgb = AS * w * normalize(pow(afterglow.rgb + 0.01, sat)) * l;

    float bp = w * BP / 255.0;

    if (sega_fix > 0.5) {
        imgcolor.rgb = imgcolor.rgb * (255.0 / 239.0);
    }

    imgcolor.rgb = min(imgcolor.rgb, 1.0);
    float3 color = imgcolor.rgb;

    if (TNTC == 0) {
        color.rgb = imgcolor.rgb;
    } else {
        float lutlow = LUTLOW / 255.0;
        float invLS  = 1.0 / 32.0;

        float3 lut_ref = imgcolor.rgb +
                         lutlow * (1.0 - pow(imgcolor.rgb, 0.333.xxx));

        float lutb = lut_ref.b * (1.0 - 0.5 * invLS);
        lut_ref.rg = lut_ref.rg * (1.0 - invLS) + 0.5 * invLS;

        float tile1 = ceil(lutb * (32.0 - 1.0));
        float tile0 = max(tile1 - 1.0, 0.0);

        float f = frac(lutb * (32.0 - 1.0));
        if (f == 0.0) {
            f = 1.0;
        }

        float2 coord0 = float2(tile0 + lut_ref.r, lut_ref.g) *
                        float2(invLS, 1.0);

        float2 coord1 = float2(tile1 + lut_ref.r, lut_ref.g) *
                        float2(invLS, 1.0);

        float4 color1, color2, res;

        if (TNTC == 1) {
            color1 = COMPAT_TEXTURE(NTSC_L01, coord0);
            color2 = COMPAT_TEXTURE(NTSC_L01, coord1);
            res    = lerp(color1, color2, f);

        } else if (TNTC == 2) {
            color1 = COMPAT_TEXTURE(NTSC_L02, coord0);
            color2 = COMPAT_TEXTURE(NTSC_L02, coord1);
            res    = lerp(color1, color2, f);

        } else if (TNTC == 3) {
            color1 = COMPAT_TEXTURE(NTSC_L03, coord0);
            color2 = COMPAT_TEXTURE(NTSC_L03, coord1);
            res    = lerp(color1, color2, f);

        } else if (TNTC == 4) {
            color1 = COMPAT_TEXTURE(NTSC_L04, coord0);
            color2 = COMPAT_TEXTURE(NTSC_L04, coord1);
            res    = lerp(color1, color2, f);
        }

        res.rgb = fix_lut(res.rgb, imgcolor.rgb);
        color = lerp(imgcolor.rgb, res.rgb, min(float(TNTC), 1.0));
    }

    float3 c = clamp(color, 0.0, 1.0);
    float3x3 m_o;

    float p;
    if      (CS == 0) { p = 2.2; m_o = ToRGB;    }
    else if (CS == 1) { p = 2.2; m_o = ToModern; }
    else if (CS == 2) { p = 2.6; m_o = ToDCI;    }
    else if (CS == 3) { p = 2.2; m_o = ToAdobe;  }
    else if (CS == 4) { p = 2.4; m_o = ToREC;    }

    color = pow(c, p);
    float3x3 m_i;

    if      (CP == 1) m_i = Profile0;
    else if (CP == 2) m_i = Profile1;
    else if (CP == 3) m_i = Profile2;
    else if (CP == 4) m_i = Profile3;
    else if (CP == 5) m_i = Profile4;
    else if (CP == 6) m_i = Profile5;

    color = mul(color, m_i);
    color = mul(color, m_o);
    color = clamp(color, 0.0, 1.0);
    color = pow(color, 1.0 / p);

    if (CP == 0) {
        color = c;
    }

    float3 scolor1 = plant(pow(color, wp_saturation),
                           max(max(color.r, color.g), color.b));

    float luma = dot(color, float3(0.299, 0.587, 0.114));

    float3 scolor2 = lerp(luma, color, wp_saturation);

    color = (wp_saturation > 1.0) ? scolor1 : scolor2;
    color = plant(color, contrast(max(max(color.r, color.g), color.b)));
    p     = 2.2;
    color = clamp(color, 0.0, 1.0);
    color = pow(color, p);

    float3 warmer = mul(color, D65_to_D55);
    warmer = mul(warmer, ToRGB);

    float3 cooler = mul(color, D65_to_D93);
    cooler = mul(cooler, ToRGB);

    float m     = abs(WP) / 100.0;
    float3 comp = (WP < 0.0) ? cooler : warmer;

    color = lerp(color, comp, m);
    color = pow(max(color, 0.0), 1.0 / p);

    if (BP > -0.5) {
        color = color + afterglow.rgb + bp;
    } else {
        color = max(color + BP / 255.0, 0.0) /
                    (1.0 + BP / 255.0 * step(-BP / 255.0,
                                             max(max(color.r, color.g), color.b))) +
                afterglow.rgb;
    }

    color = min(color * pre_bb, 1.0);

    return float4(color, vignette(tc));
}

float4 Signal_1_PS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float pix_res = min(ntsc_scale, 1.0);

    float phase = (ntsc_phase == 0) ? ((NativeWidth > 300.0) ? 2.0 : 3.0)
                                    : ((ntsc_phase > 1)      ? 3.0 : 2.0);
    if (ntsc_phase == 3) {
        phase = 3.0;
    }

    float res  = ntsc_scale;
    float mod1 = 2.0;
    float mod2 = 3.0;

    float CHROMA_MOD_FREQ = (phase < 2.5) ? (4.0 * pii / 15.0) : (pii / 3.0);
    float ARTIFACT   = cust_artifacting;
    float FRINGING   = cust_fringing;
    float BRIGHTNESS = ntsc_brt;
    float SATURATION = ntsc_sat;
    float MERGE      = 0.0;
    float mix1       = 0.0;

    if      (ntsc_fields == 0 && phase == 3.0) MERGE = 1.0;
    else if (ntsc_fields == 1)                 MERGE = 0.0;
    else if (ntsc_fields == 2)                 MERGE = 1.0;

    float2 pix_no = texcoord * OriginalSize.xy * pix_res * float2(4.0, 1.0);
    float3 col0   = tex2D(NTSC_S02, texcoord).rgb;

    float3 yiq1 = rgb2yiq(col0);
    float c0    = yiq1.x;
    yiq1.x      = pow(yiq1.x, ntsc_gamma);
    float lum   = yiq1.x;
    float2 dx   = float2(OriginalSize.z, 0.0);

    float3 c1 = tex2D(NTSC_S02, texcoord - dx).rgb;
    float3 c2 = tex2D(NTSC_S02, texcoord + dx).rgb;

    if (abs(ntsc_rainbow) > 0.025) {
        float2 dy = float2(0.0, OriginalSize.w);

        float3 c3 = tex2D(NTSC_S02, texcoord + dy).rgb;
        float3 c4 = tex2D(NTSC_S02, texcoord + dx + dy).rgb;
        float3 c5 = tex2D(NTSC_S02, texcoord + dx + dx).rgb;
        float3 c6 = tex2D(NTSC_S02, texcoord + dx * 3.0).rgb;

        c1.x = get_luma(c1);
        c2.x = get_luma(c2);
        c3.x = get_luma(c3);
        c4.x = get_luma(c4);
        c5.x = get_luma(c5);
        c6.x = get_luma(c6);

        float mix2 = min(5.0 * min(min(abs(c0   - c1.x), abs(c0   - c2.x)),
                                   min(abs(c2.x - c5.x), abs(c5.x - c6.x))),
                         1.0);

        float bar1 = 1.0 - min(7.0 * min(max(max(c0,   c3.x) - 0.15, 0.0),
                                         max(max(c2.x, c4.x) - 0.15, 0.0)),
                               1.0);

        float bar2 = step(abs(c1.x - c2.x) + abs(c0 - c5.x) + abs(c2.x - c6.x),
                          0.325);

        mix1 = bar1 * bar2 * mix2 * (1.0 - min(10.0 * min(abs(c0 - c3.x), abs(c2.x - c4.x)), 1.0));
        mix1 = mix1 * ntsc_rainbow;
    }

    if (ntsc_phase == 3) {
        float mix3 = min(5.0 * abs(c1.x - c2.x), 1.0);

        c1.x   = pow(c1.x, ntsc_gamma);
        c2.x   = pow(c2.x, ntsc_gamma);

        yiq1.x = lerp(min(0.5 * (yiq1.x + max(c1.x, c2.x)),
                          max(yiq1.x, min(c1.x, c2.x))),
                      yiq1.x,
                      mix3);
    }

    float3 yiq2 = yiq1;
    float3 yiqs = yiq1;
    float3 yiqz = yiq1;

    float taps_comp = 1.0 + 2.0 * step(ntsc_taps, 15.5);

    if (MERGE > 0.5) {
        float chroma_phase2 = (phase < 2.5) ? pii * (mod(pix_no.y, mod1) + mod(FrameCount + 1, 2.))
                                            : 0.6667 * pii * (mod(pix_no.y, mod2) + mod(FrameCount + 1, 2.));

        float mod_phase2 = chroma_phase2 * (1.0 - mix1) + pix_no.x * CHROMA_MOD_FREQ * taps_comp;

        float i_mod2 = cos(mod_phase2);
        float q_mod2 = sin(mod_phase2);
        yiq2.yz *= float2(i_mod2, q_mod2);
        yiq2 = mul(mix_m, yiq2);
        yiq2.yz *= float2(i_mod2, q_mod2);

        if (res > 1.025) {
            mod_phase2 = chroma_phase2 * (1.0 - mix1) +
                         res * pix_no.x * CHROMA_MOD_FREQ * taps_comp;

            i_mod2 = cos(mod_phase2);
            q_mod2 = sin(mod_phase2);
            yiqs.yz *= float2(i_mod2, q_mod2);
            yiq2.x = dot(yiqs, mix_m[0]);
        }
    }

    float chroma_phase1 = (phase < 2.5) ? pii * (mod(pix_no.y, mod1) + mod(FrameCount, 2.))
                                        : 0.6667 * pii * (mod(pix_no.y, mod2) + mod(FrameCount, 2.));

    float mod_phase1 = chroma_phase1 * (1.0 - mix1) + pix_no.x * CHROMA_MOD_FREQ * taps_comp;

    float i_mod1 = cos(mod_phase1);
    float q_mod1 = sin(mod_phase1);

    yiq1.yz *= float2(i_mod1, q_mod1);
    yiq1 = mul(mix_m, yiq1);
    yiq1.yz *= float2(i_mod1, q_mod1);

    if (res > 1.025) {
        mod_phase1 = chroma_phase1 * (1.0 - mix1) + res * pix_no.x * CHROMA_MOD_FREQ * taps_comp;
        i_mod1 = cos(mod_phase1);
        q_mod1 = sin(mod_phase1);
        yiqz.yz *= float2(i_mod1, q_mod1);
        yiq1.x = dot(yiqz, mix_m[0]);
    }

    if (ntsc_phase == 3) {
        yiq1.x = lum;
        yiq2.x = lum;
    }

    yiq1 = (MERGE < 0.5) ? yiq1 : 0.5 * (yiq1 + yiq2);

    return float4(yiq1, lum);
}

float4 Signal_2_PS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float chroma_filter_2_phase[33] = {
            0.001384762, 0.001678312, 0.002021715, 0.002420562, 0.002880460,
            0.003406879, 0.004004985, 0.004679445, 0.005434218, 0.006272332,
            0.007195654, 0.008204665, 0.009298238, 0.010473450, 0.011725413,
            0.013047155, 0.014429548, 0.015861306, 0.017329037, 0.018817382,
            0.020309220, 0.021785952, 0.023227857, 0.024614500, 0.025925203,
            0.027139546, 0.028237893, 0.029201910, 0.030015081, 0.030663170,
            0.031134640, 0.031420995, 0.031517031};

    float chroma_filter_3_phase[25] = {
            -0.000118847, -0.000271306, -0.000502642, -0.000930833,
            -0.001451013, -0.002064744, -0.002700432, -0.003241276,
            -0.003524948, -0.003350284, -0.002491729, -0.000721149,
             0.002164659,  0.006313635,  0.011789103,  0.018545660,
             0.026414396,  0.035100710,  0.044196567,  0.053207202,
             0.061590275,  0.068803602,  0.074356193,  0.077856564,
             0.079052396};

    float luma_filter_2_phase[33] = {
            -0.000174844, -0.000205844, -0.000149453, -0.000051693,
             0.000000000, -0.000066171, -0.000245058, -0.000432928,
            -0.000472644, -0.000252236,  0.000198929,  0.000687058,
             0.000944112,  0.000803467,  0.000363199,  0.000013422,
             0.000253402,  0.001339461,  0.002932972,  0.003983485,
             0.003026683, -0.001102056, -0.008373026, -0.016897700,
            -0.022914480, -0.021642347, -0.028863273,  0.027271957,
             0.054921920,  0.098342579,  0.139044281,  0.168055832,
             0.178571429};

    float luma_filter_3_phase[25] = {
            -0.000012020, -0.000022146, -0.000013155, -0.000012020,
            -0.000049979, -0.000113940, -0.000122150, -0.000005612,
             0.000170516,  0.000237199,  0.000169640,  0.000285688,
             0.000984574,  0.002018683,  0.002002275, -0.005909882,
            -0.012049081, -0.018222860, -0.022606931,  0.002460860,
             0.035868225,  0.084016453,  0.135563500,  0.175261268,
             0.220176552};

    float luma_filter_4_phase[25] = {
            -0.000472644, -0.000252236,  0.000198929,  0.000687058,
             0.000944112,  0.000803467,  0.000363199,  0.000013422,
             0.000253402,  0.001339461,  0.002932972,  0.003983485,
             0.003026683, -0.001102056, -0.008373026, -0.016897700,
            -0.022914480, -0.021642347, -0.028863273,  0.027271957,
             0.054921920,  0.098342579,  0.139044281,  0.168055832,
             0.178571429};

    const int TAPS_2_phase = 32;
    const int TAPS_3_phase = 24;

    float res     = ntsc_scale;
    float3 signal = 0.0;
    float2 one    = 0.25 * OriginalSize.zz / res;
    float phase   = (ntsc_phase == 0) ? ((NativeWidth > 300.0) ? 2.0 : 3.0)
                                      : ((ntsc_phase > 1)      ? 3.0 : 2.0);

    if (ntsc_phase == 3) {
        phase = 3.0;
        luma_filter_3_phase = luma_filter_4_phase;
    }

    float3 wsum  = 0.0.xxx;
    float3 sums  = wsum;
    float3 tmps  = wsum;
    float offset = 0.0;
    int i        = 0;
    float j      = 0.0;

    if (phase < 2.5) {
        float loop    = max(ntsc_taps, 8.0);
        float2 dx     = float2(one.x, 0.0);
        float2 xd     = dx;
        int loopstart = int(TAPS_2_phase - loop);

        float taps = 0.0;
        float laps = ntsc_taps + 1.0;
        float ssub = loop - loop / ntsc_cscale1;

        for (i = loopstart; i < 32; i++) {
            offset = float(i - loopstart);
            j      = offset + 1.0;
            xd     = (offset - loop) * dx;
            sums   = fetch_offset1(xd);
            taps   = max(j - ssub, 0.0);
            tmps   = float3(luma_filter_2_phase[i], taps.xx);
            wsum   = wsum + tmps;
            signal += sums * tmps;
        }
        taps = laps - ssub;
        tmps = float3(luma_filter_2_phase[TAPS_2_phase], taps.xx);
        wsum = wsum + wsum + tmps;

        signal += tex2D(NTSC_S03, tex_1).xyz * tmps;
        signal = signal / wsum;

    } else {
        float loop = min(ntsc_taps, TAPS_3_phase);
        one.y      = one.y / ntsc_cscale2;
        float3 dx  = float3(one.x, one.y, 0.0);
        float3 xd  = dx;

        int loopstart = int(24.0 - loop);

        for (i = loopstart; i < 24; i++) {
            offset = float(i - loopstart);
            j      = offset + 1.0;
            xd.xy  = (offset - loop) * dx.xy;
            sums   = fetch_offset2(xd);
            tmps   = float3(luma_filter_3_phase[i],
                            chroma_filter_3_phase[i].xx);

            wsum   = wsum + tmps;
            signal += sums * tmps;
        }
        tmps = float3(luma_filter_3_phase[TAPS_3_phase],
                      chroma_filter_3_phase[TAPS_3_phase],
                      chroma_filter_3_phase[TAPS_3_phase]);

        wsum = wsum + wsum + tmps;
        signal += tex2D(NTSC_S03, tex_1).xyz * tmps;
        signal = signal / wsum;
    }

    if (ntsc_ring > 0.05) {
        float2 dx = float2(OriginalSize.z / min(res, 1.0), 0.0);

        float a = tex2D(NTSC_S03, tex_1 - 1.5 * dx).a;
        float b = tex2D(NTSC_S03, tex_1 - 0.5 * dx).a;
        float c = tex2D(NTSC_S03, tex_1 + 1.5 * dx).a;
        float d = tex2D(NTSC_S03, tex_1 + 0.5 * dx).a;
        float e = tex2D(NTSC_S03, tex_1).a;

        signal.x = lerp(signal.x,
                        clamp(signal.x,
                              min(min(min(a, b), min(c, d)), e),
                              max(max(max(a, b), max(c, d)), e)),
                        ntsc_ring);
    }

    float3 x   = rgb2yiq(tex2D(NTSC_S02, tex_1).rgb);
    signal.x   = clamp(signal.x, -1.0, 1.0);
    float3 rgb = signal;

    return float4(rgb, x.x);
}

float4 Signal_3_PS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float2 dx     = float2(0.25 * OriginalSize.z, 0.0) / 4.0;
    float2 tcoord = tex_2 + dx;
    float2 offset = float2(0.5 * OriginalSize.z, 0.0);

    float3 ll1 = tex2D(NTSC_S04, tcoord + offset).xyz;
    float3 ll2 = tex2D(NTSC_S04, tcoord - offset).xyz;
    float3 ll3 = tex2D(NTSC_S04, tcoord + 0.50 * offset).xyz;
    float3 ll4 = tex2D(NTSC_S04, tcoord - 0.50 * offset).xyz;
    float3 ref = tex2D(NTSC_S04, tcoord).xyz;

    float lum1 = min(tex2D(NTSC_S04, tex_2 - dx).a,
                     tex2D(NTSC_S04, tex_2 + dx).a);

    float lum2 = max(ref.x, 0.0);

    float dif  = max(max(abs(ll1.x - ll2.x), abs(ll1.y - ll2.y)),
                     max(abs(ll1.z - ll2.z), abs(ll1.x * ll1.x - ll2.x * ll2.x)));

    float dff  = max(max(abs(ll3.x - ll4.x), abs(ll3.y - ll4.y)),
                     max(abs(ll3.z - ll4.z), abs(ll3.x * ll3.x - ll4.x * ll4.x)));

    float lc = (1.0 - smoothstep(0.10, 0.20, abs(lum2 - lum1))) * pow(dff, 0.125);
    float sweight = smoothstep(0.05 - 0.03 * lc, 0.45 - 0.40 * lc, dif);

    float3 signal = ref;

    if (abs(ntsc_shrp) > -0.1) {
        float lummix = lerp(lum2, lum1, 0.1 * abs(ntsc_shrp));
        float lm1 = lerp(lum2 * lum2, lum1 * lum1, 0.1 * abs(ntsc_shrp));
        lm1       = sqrt(lm1);

        float lm2 = lerp(sqrt(lum2), sqrt(lum1), 0.1 * abs(ntsc_shrp));
        lm2       = lm2 * lm2;

        float k1  = abs(lummix - lm1) + 0.00001;
        float k2  = abs(lummix - lm2) + 0.00001;
        lummix    = min((k2 * lm1 + k1 * lm2) / (k1 + k2), 1.0);

        signal.x = lerp(lum2, lummix, smoothstep(0.25, 0.4, pow(dff, 0.125)));
        signal.x = min(signal.x, max(ntsc_shpe * signal.x, lum2));
    } else {
        signal.x = clamp(signal.x, 0.0, 1.0);
    }

    float3 rgb = signal;
    if (ntsc_shrp < -0.1) {
        rgb.x = lerp(ref.x, rgb.x, sweight);
    }

    rgb.x = pow(rgb.x, 1.0 / ntsc_gamma);
    rgb   = clamp(yiq2rgb(rgb), 0.0, 1.0);

    return float4(rgb, 1.0);
}

float4 SharpnessPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float2 g01 = float2(-0.5 * OriginalSize.z, 0.0);
    float2 g21 = float2(0.5 * OriginalSize.z, 0.0);
    float3 c01 = tex2D(NTSC_S05, texcoord + g01).rgb;
    float3 c21 = tex2D(NTSC_S05, texcoord + g21).rgb;
    float3 c11 = tex2D(NTSC_S05, texcoord).rgb;
    float3 b11 = 0.5 * (c01 + c21);

    float contrast = max(max(c11.r, c11.g), c11.b);
    contrast       = lerp(2.0 * CCONTR, CCONTR, contrast);

    float3 mn      = min(min(c01, c21), c11);
    float3 mn1     = min(mn, c11 * (1.0 - contrast));
    float3 mx      = max(max(c01, c21), c11);
    float3 mx1     = max(mx, c11 * (1.0 + contrast));
    float3 dif     = pow(mx1 - mn1 + 0.0001, 0.75);

    float3 sharpen = lerp(CSHARPEN * CDETAILS, CSHARPEN, dif);
    float3 res     = clamp(lerp(c11, b11, -sharpen), mn1, mx1);

    if (DEBLUR > 1.125) {
        c01 = tex2D(NTSC_S02, texcoord + 2.0 * g01).rgb;
        c21 = tex2D(NTSC_S02, texcoord + 2.0 * g21).rgb;
        c11 = tex2D(NTSC_S02, texcoord).rgb;

        mn1 = sqrt(min(min(c01, c21), c11) * mn);
        mx1 = sqrt(max(max(c01, c21), c11) * mx);

        float3 dif1 = max(res - mn1, 0.0) + 0.00001;
        dif1        = pow(dif1, DEBLUR.xxx);

        float3 dif2 = max(mx1 - res, 0.0) + 0.00001;
        dif2        = pow(dif2, DEBLUR.xxx);

        float3 ratio = dif1 / (dif1 + dif2);

        sharpen = min(lerp(mn1, mx1, ratio),
                      pow(res, lerp(0.75.xxx, 1.10.xxx, res)));

        res   = rgb2yiq(res);
        res.x = dot(sharpen, float3(0.2989, 0.5870, 0.1140));
        res   = max(yiq2rgb(res), 0.0);
    }
    return float4(res, 1.0);
}

float4 LuminancePS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float m = max(log2(NTSC_02.x), log2(NTSC_02.y));

    m = floor(max(m, 1.0)) - 1.0;

    float2 dx = float2(1.0 / NTSC_02.x, 0.0);
    float2 dy = float2(0.0, 1.0 / NTSC_02.y);
    float2 x2 = 2.0 * dx;
    float2 y2 = 2.0 * dy;

    float ltotal = 0.0;

    ltotal += length(tex2Dlod(NTSC_S06, float4(float2(0.3, 0.3), m, 0)).rgb);
    ltotal += length(tex2Dlod(NTSC_S06, float4(float2(0.3, 0.7), m, 0)).rgb);
    ltotal += length(tex2Dlod(NTSC_S06, float4(float2(0.7, 0.3), m, 0)).rgb);
    ltotal += length(tex2Dlod(NTSC_S06, float4(float2(0.7, 0.7), m, 0)).rgb);

    ltotal *= 0.25;
    ltotal = pow(0.577350269 * ltotal, 0.7);

    float lhistory = tex2D(NTSC_S07, 0.5).a;

    ltotal = lerp(ltotal, lhistory, lsmooth);

    float3 l1 = COMPAT_TEXTURE(NTSC_S06, fuxcoord.xy).rgb;
    float3 r1 = COMPAT_TEXTURE(NTSC_S06, fuxcoord.xy + dx).rgb;
    float3 l2 = COMPAT_TEXTURE(NTSC_S06, fuxcoord.xy - dx).rgb;
    float3 r2 = COMPAT_TEXTURE(NTSC_S06, fuxcoord.xy + x2).rgb;

    float c1  = dist(l2, l1);
    float c2  = dist(l1, r1);
    float c3  = dist(r2, r1);

    return float4(c1, c2, c3, ltotal);
}

float4 LinearizePS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float3 c1 = COMPAT_TEXTURE(NTSC_S06, fuxcoord).rgb;
    float3 c2 = COMPAT_TEXTURE(NTSC_S06, fuxcoord + float2(0.0, OriginalSize.w)).rgb;

    if ((downsample_levelx + downsample_levely) > 0.025) {
        c1 = fetch_pixel(fuxcoord);
        c2 = fetch_pixel(fuxcoord + float2(0.0, OriginalSize.w));
    }

    float3 c       = c1;
    float intera   = 1.0;
    float gamma_in = clamp(gamma_i, 1.0, 5.0);

    float m1  = max(max(c1.r, c1.g), c1.b);
    float m2  = max(max(c2.r, c2.g), c2.b);
    float3 df = abs(c1 - c2);
    float d   = max(max(df.r, df.g), df.b);

    if (interm == 2) {
        d = lerp(0.1 * d, 10.0 * d, step(m1 / (m2 + 0.0001), m2 / (m1 + 0.0001)));
    }

    float r = m1;

    float yres_div = 1.0;
    if (intres > 1.25) {
        yres_div = intres;
    }

    bool hscans = (hiscan > 0.5);

    if (interr <= NativeHeight / yres_div && interm > 0 && intres != 1.0 &&
            intres != 0.5 || hscans) {
        intera = 0.25;

        float liine_no = clamp(floor(mod(OriginalSize.y * fuxcoord.y, 2.0)), 0.0, 1.0);
        float frame_no = clamp(floor(mod(FrameCount, 2.0)), 0.0, 1.0);

        float ii = abs(liine_no - frame_no);

        if (interm < 4) {
            c2 = plant(lerp(c2, c2 * c2, iscans), max(max(c2.r, c2.g), c2.b));
            r  = clamp(max(m1 * ii, (1.0 - iscanb) * min(m1, m2)), 0.0, 1.0);
            c  = plant(lerp(lerp(c1,
                                c2,
                                min(lerp(m1, 1.0 - m2, min(m1, 1.0 - m1)) / (d + 0.00001),
                                    1.0)),
                           c1,
                           ii),
                      r);

            if (interm == 3) {
                c = (1.0 - 0.5 * iscanb) * lerp(c2, c1, ii);
            }
        }
        if (interm == 4) {
            c = plant(lerp(c, c * c, 0.5 * iscans),
                      max(max(c.r, c.g), c.b)) * (1.0 - 0.5 * iscanb);
        }
        if (interm == 5) {
            c = lerp(c2, c1, 0.5);
            c = plant(lerp(c, c * c, 0.5 * iscans),
                      max(max(c.r, c.g), c.b)) * (1.0 - 0.5 * iscanb);
        }
        if (hscans) {
            c = c1;
        }
    }

    c = pow(c, gamma_in);

    if (fuxcoord.x > 0.5) {
        gamma_in = intera;
    } else {
        gamma_in = 1.0 / gamma_in;
    }

    return float4(c, gamma_in);
}

float4 HGaussianPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float4 GaussSize = OriginalSize *
                       lerp(1.0.xxxx,
                            float4(FINE_GAUSS, FINE_GAUSS, 1.0 / FINE_GAUSS, 1.0 / FINE_GAUSS),
                            min(FINE_GAUSS - 1.0, 1.0));

    float f = frac(GaussSize.x * texcoord.x);
    f = 0.5 - f;

    float2 tex = floor(GaussSize.xy * texcoord) * GaussSize.zw + 0.5 * GaussSize.zw;
    float3 color = 0.0;
    float2 dx = float2(GaussSize.z, 0.0);

    float3 pixel;
    float w;
    float wsum = 0.0;
    float n = -SIZEH;

    do {
        pixel = COMPAT_TEXTURE(NTSC_S08, tex + n * dx).rgb;
        if (m_glow > 0) {
            pixel = max(pixel - m_glow_cutoff, 0.0);
            pixel = plant(pixel,
                          max(max(max(pixel.r, pixel.g), pixel.b) - m_glow_cutoff, 0.0));
        }

        w = gauss_h(n + f);
        color = color + w * pixel;
        wsum = wsum + w;
        n = n + 1.0;
    } while (n <= SIZEH);

    color = color / wsum;

    return float4(color, 1.0);
}

float4 VGaussianPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float4 GaussSize = float4(SourceSize.x, OriginalSize.y, SourceSize.z, OriginalSize.w) *
                       lerp(1.0.xxxx,
                            float4(FINE_GAUSS, FINE_GAUSS, 1.0 / FINE_GAUSS, 1.0 / FINE_GAUSS),
                            min(FINE_GAUSS - 1.0, 1.0));

    float f = frac(GaussSize.y * texcoord.y);
    f = 0.5 - f;

    float2 tex = floor(GaussSize.xy * texcoord) * GaussSize.zw + 0.5 * GaussSize.zw;
    float3 color = 0.0;
    float2 dy = float2(0.0, GaussSize.w);

    float3 pixel;
    float w;
    float wsum = 0.0;
    float n = -SIZEV;

    do {
        pixel = COMPAT_TEXTURE(NTSC_S10, tex + n * dy).rgb;

        w = gauss_v(n + f);
        color = color + w * pixel;
        wsum = wsum + w;
        n = n + 1.0;
    } while (n <= SIZEV);

    color = color / wsum;

    return float4(color, 1.0);
}

float4 BloomHorzPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float4 BloomSize = OriginalSize *
                       lerp(1.0.xxxx,
                            float4(FINE_BLOOM, FINE_BLOOM, 1.0 / FINE_BLOOM, 1.0 / FINE_BLOOM),
                            min(FINE_BLOOM - 1.0, 1.0));

    float f = frac(BloomSize.x * texcoord.x);
    f = 0.5 - f;

    float2 tex = floor(BloomSize.xy * texcoord) * BloomSize.zw + 0.5 * BloomSize.zw;
    float4 color = 0.0;
    float2 dx = float2(BloomSize.z, 0.0);

    float4 pixel;
    float w;
    float wsum = 0.0;
    float n = -SIZEX;

    do {
        pixel = COMPAT_TEXTURE(NTSC_S08, tex + n * dx);

        w = bloom_h(n + f);
        pixel.a = max(max(pixel.r, pixel.g), pixel.b);
        pixel.a *= pixel.a * pixel.a;
        color = color + w * pixel;
        wsum  = wsum + w;
        n = n + 1.0;
    } while (n <= SIZEX);

    color = color / wsum;

    return float4(color.rgb, pow(color.a, 0.333333));
}

float4 BloomVertPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float4 BloomSize = float4(SourceSize.x, OriginalSize.y, SourceSize.z, OriginalSize.w) *
                       lerp(1.0.xxxx,
                            float4(FINE_BLOOM, FINE_BLOOM, 1.0 / FINE_BLOOM, 1.0 / FINE_BLOOM),
                            min(FINE_BLOOM - 1.0, 1.0));

    float f = frac(BloomSize.y * texcoord.y);
    f = 0.5 - f;

    float2 tex = floor(BloomSize.xy * texcoord) * BloomSize.zw + 0.5 * BloomSize.zw;
    float4 color = 0.0;
    float2 dy = float2(0.0, BloomSize.w);

    float4 pixel;
    float w;
    float wsum = 0.0;
    float n = -SIZEY;

    do {
        pixel = COMPAT_TEXTURE(NTSC_S12, tex + n * dy);

        w = bloom_v(n + f);
        pixel.a *= pixel.a * pixel.a;
        color = color + w * pixel;
        wsum  = wsum + w;

        n = n + 1.0;
    } while (n <= SIZEY);

    color = color / wsum;

    return float4(color.rgb, pow(color.a, 0.175000));
}

float4 NTSC_TV1_PS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float2 prescalex = float2(tex2Dsize(NTSC_S08, 0)) / OriginalSize.xy;

    float4 PALSize = OriginalSize * float4(prescalex.x,
                                           prescalex.y,
                                           1.0 / prescalex.x,
                                           1.0 / prescalex.y);

    float f = frac(PALSize.x * fuxcoord.x);
    f = 0.5 - f;

    float2 tex   = floor(PALSize.xy * fuxcoord) * PALSize.zw + 0.5 * PALSize.zw;
    float3 color = 0.0.xxx;
    float scolor = 0.0;
    float2 dx    = float2(PALSize.z, 0.0);

    float3 pixel;
    float w          = 0.0;
    float swsum      = 0.0;
    float wsum       = 0.0;
    float xs         = prescalex.x * 0.5;
    float hsharpness = HSHARPNESS * xs;

    float3 cmax      = 0.0.xxx;
    float3 cmin      = 1.0.xxx;
    float sharp      = crthd_h(hsharpness, xs) * S_SHARPH;
    float maxsharp   = MAXS;
    float FPR        = hsharpness;
    float fpx        = 0.0;
    float sp         = 0.0;
    float sw         = 0.0;
    float ts         = 0.025;

    float3 luma = float3(0.2126, 0.7152, 0.0722);

    float LOOPSIZE = ceil(2.0 * FPR);
    float CLPSIZE  = round(2.0 * LOOPSIZE / 3.0);
    float n        = -LOOPSIZE;

    do {
        pixel = COMPAT_TEXTURE(NTSC_S08, tex + n * dx).rgb;

        sp  = max(max(pixel.r, pixel.g), pixel.b);
        w   = crthd_h(n + f, xs) - sharp;
        fpx = abs(n + f - sign(n) * FPR) / FPR;

        if (abs(n) <= CLPSIZE) {
            cmax = max(cmax, pixel);
            cmin = min(cmin, pixel);
        }
        if (w < 0.0) {
            w = clamp(w, lerp(-maxsharp, 0.0, pow(clamp(fpx, 0.0, 1.0), HSHARP)), 0.0);
        }

        color  = color + w * pixel;
        wsum   = wsum + w;
        sw     = max(w, 0.0) * (dot(pixel, luma) + ts);
        scolor = scolor + sw * sp;
        swsum  = swsum + sw;
        n      = n + 1.0;
    } while (n <= LOOPSIZE);

    color  = color  / wsum;
    scolor = scolor / swsum;

    color  = clamp(lerp(clamp(color, cmin, cmax), color, HARNG), 0.0, 1.0);
    scolor = clamp(lerp(max(max(color.r, color.g), color.b), scolor, spike), 0.0, 1.0);

    return float4(color, scolor);
}

float4 NTSC_TV2_PS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float prescalex = tex2Dsize(NTSC_S08, 0).x / (2.0 * OriginalSize.x);

    float4 PALSize = OriginalSize * float4(prescalex, 1.0, 1.0 / prescalex, 1.0);

    float gamma_in = 1.0 / COMPAT_TEXTURE(NTSC_S08, 0.25).a;
    float lum      = COMPAT_TEXTURE(NTSC_S07, 0.5).a;
    float intera   = COMPAT_TEXTURE(NTSC_S08, float2(0.75, 0.25)).a;
    bool hscans    = (hiscan > 0.5);
    bool interb    = (((intera < 0.35) || (no_scanlines > 0.025)) && !hscans);

    PALSize *= float4(2.0, 1.0, 0.5, 1.0);

    float SourceY = PALSize.y;

    float sy = 1.0;

    if (intres == 1.0) {
        sy = max(floor(SourceY / 199.0), 1.0);
    }
    if (intres > 0.25 && intres != 1.0) {
        sy = intres;
    }

    PALSize *= float4(1.0, 1.0 / sy, 1.0, sy);
    float2 lexcoord = fuxcoord.xy;

    if (IOS > 0.0 && !interb) {
        float2 ofactor   = OutputSize.xy / OriginalSize.xy;
        float2 intfactor = (IOS < 2.5) ? floor(ofactor) : ceil(ofactor);
        float2 diff      = ofactor / intfactor;
        float scan       = diff.y;

        lexcoord = overscan(lexcoord, scan, scan);

        if (IOS == 1.0 || IOS == 3.0) {
            lexcoord = float2(fuxcoord.x, lexcoord.y);
        }
    }

    float factor = 1.0 + (1.0 - 0.5 * OS) * blm_2 / 100.0 - lum * blm_2 / 100.0;

    lexcoord = overscan(lexcoord, factor, factor);
    lexcoord = overscan(lexcoord, (OriginalSize.x - overscanx * BufferToViewportRatio.x) / OriginalSize.x,
                                  (OriginalSize.y - overscany * BufferToViewportRatio.y) / OriginalSize.y);

    float2 pos     = warp(lexcoord);
    float2 coffset = 0.5;
    float2 ps      = PALSize.zw;
    float2 OGL2Pos = pos * PALSize.xy - coffset;
    float2 fp      = frac(OGL2Pos);
    float2 dx      = float2(ps.x, 0.0);
    float2 dy      = float2(0.0, ps.y);
    float f        = fp.y;
    float2 pC4     = floor(OGL2Pos) * ps + 0.5 * ps;
    pC4.x          = pos.x;

    if (intres == 0.5 && prescalex < 1.5) {
        pC4.y = floor(pC4.y * OriginalSize.y) * OriginalSize.w + 0.5 * OriginalSize.w;
    }
    if (interb && no_scanlines < 0.025 || hscans) {
        pC4.y = pos.y;
    } else if (interb) {
        pC4.y = pC4.y + smoothstep(0.40 - 0.5 * no_scanlines, 0.60 + 0.5 * no_scanlines, f) * PALSize.w;
    }

    float3 color1  = COMPAT_TEXTURE(NTSC_S09, pC4).rgb;
    float3 scolor1 = COMPAT_TEXTURE(NTSC_S09, pC4).aaa;

    if (!interb) {
        color1 = pow(color1, scangamma / gamma_in);
    }

    pC4 += dy;
    if (intres == 0.5 && prescalex < 1.5) {
        pC4.y = floor((pos.y + 0.33 * dy.y) * OriginalSize.y) * OriginalSize.w +
                0.5 * OriginalSize.w;
    }

    float3 color2  = COMPAT_TEXTURE(NTSC_S09, pC4).rgb;
    float3 scolor2 = COMPAT_TEXTURE(NTSC_S09, pC4).aaa;

    if (!interb) {
        color2 = pow(color2, scangamma / gamma_in);
    }

    float3 ctmp  = color1;
    float w3     = 1.0;
    float3 color = color1;
    float3 one   = 1.0;

    if (hscans) {
        color2  = color1;
        scolor2 = scolor1;
    }

    if (!interb || hscans) {
        float3 luma = float3(0.2126, 0.7152, 0.0722);
        float ssub  = ssharp * max(abs(scolor1.x - scolor2.x),
                                   abs(dot(color1, luma) - dot(color2, luma)));

        float shape1 = lerp(scanline1, scanline2 + ssub * scolor1.x * 35.0, f);
        float shape2 = lerp(scanline1, scanline2 + ssub * scolor2.x * 35.0, 1.0 - f);

        float wt1 = st0(f);
        float wt2 = st0(1.0 - f);

        float3 color0  = color1 * wt1 + color2 * wt2;
        float3 scolor0 = scolor1 * wt1 + scolor2 * wt2;
        ctmp           = color0 / (wt1 + wt2);
        float3 sctmp   = scolor0 / (wt1 + wt2);

        float3 w1, w2;

        float3 cref1 = lerp(sctmp, scolor1, beam_size);
        float creff1 = pow(max(max(cref1.r, cref1.g), cref1.b), scan_falloff);
        float3 cref2 = lerp(sctmp, scolor2, beam_size);
        float creff2 = pow(max(max(cref2.r, cref2.g), cref2.b), scan_falloff);

        if (tds) {
            shape1 = lerp(scanline2, shape1, creff1);
            shape2 = lerp(scanline2, shape2, creff2);
        }

        float f1 = f;
        float f2 = 1.0 - f;
        float m1 = max(max(color1.r, color1.g), color1.b) + eps;
        float m2 = max(max(color2.r, color2.g), color2.b) + eps;

        cref1 = color1 / m1;
        cref2 = color2 / m2;

        if (gsl < 2) {
            w1 = sw0(f1, creff1, shape1, cref1);
            w2 = sw0(f2, creff2, shape2, cref2);
        } else if (gsl == 2) {
            w1 = sw1(f1, creff1, shape1, cref1);
            w2 = sw1(f2, creff2, shape2, cref2);
        } else {
            w1 = sw2(f1, creff1, shape1, cref1);
            w2 = sw2(f2, creff2, shape2, cref2);
        }

        float3 w3 = w1 + w2;
        float wf1 = max(max(w3.r, w3.g), w3.b);

        if (wf1 > 1.0) {
            wf1 = 1.0 / wf1;
            w1 *= wf1, w2 *= wf1;
        }

        if (abs(clp) > 0.005) {
            sy  = m1;
            one = (clp > 0.0) ? w1 : 1.0.xxx;

            float sat = 1.0001 - min(min(cref1.r, cref1.g), cref1.b);

            color1 = lerp(color1,
                          plant(pow(color1, 0.70.xxx - 0.325 * sat), sy),
                          pow(sat, 0.3333) * one * abs(clp));

            sy  = m2;
            one = (clp > 0.0) ? w2 : 1.0.xxx;
            sat = 1.0001 - min(min(cref2.r, cref2.g), cref2.b);

            color2 = lerp(color2,
                          plant(pow(color2, 0.70.xxx - 0.325 * sat), sy),
                          pow(sat, 0.3333) * one * abs(clp));
        }
        color = (gc(color1) * w1 + gc(color2) * w2);
        color = min(color, 1.0);
    }

    if (interb) {
        color = gc(color1);
    }

    float colmx = max(max(ctmp.r, ctmp.g), ctmp.b);
    if (!interb) {
        color = pow(color, gamma_in / scangamma);
    }

    return float4(color, colmx);
}

float4 ChromaticPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float gamma_in = 1.0 / COMPAT_TEXTURE(NTSC_S08, 0.25).a;
    float lum      = COMPAT_TEXTURE(NTSC_S07, 0.5).a;
    float intera   = COMPAT_TEXTURE(NTSC_S08, float2(0.75, 0.25)).a;

    bool interb = ((intera < 0.35 || no_scanlines > 0.025) && (hiscan < 0.5));

    float2 lexcoord = fuxcoord.xy;

    if (IOS > 0.0 && !interb) {
        float2 ofactor   = OutputSize.xy / OriginalSize.xy;
        float2 intfactor = (IOS < 2.5) ? floor(ofactor) : ceil(ofactor);

        float2 diff = ofactor / intfactor;
        float scan  = diff.y;

        lexcoord = overscan(lexcoord, scan, scan);

        if (IOS == 1.0 || IOS == 3.0) {
            lexcoord = float2(fuxcoord.x, lexcoord.y);
        }
    }

    float factor = 1.0 + (1.0 - 0.5 * OS) * blm_2 / 100.0 - lum * blm_2 / 100.0;

    lexcoord = overscan(lexcoord, factor, factor);
    lexcoord = overscan(lexcoord, (OriginalSize.x - overscanx * BufferToViewportRatio.x) / OriginalSize.x,
                                  (OriginalSize.y - overscany * BufferToViewportRatio.y) / OriginalSize.y);

    float2 pos0 = warp(fuxcoord.xy);
    float2 pos1 = fuxcoord.xy;
    float2 pos  = warp(lexcoord);

    float3 color = COMPAT_TEXTURE(NTSC_S14, pos1).rgb;
    float3 Bloom = COMPAT_TEXTURE(NTSC_S13, pos).rgb;
    float3 Glow  = COMPAT_TEXTURE(NTSC_S11, pos).rgb;

    if ((abs(deconrx) + abs(deconry) + abs(decongx) + abs(decongy) +
         abs(deconbx) + abs(deconby)) > 0.2) {
        bring_pixel(color, Bloom, Glow, pos1, pos);
    }

    float cm    = igc(max(max(color.r, color.g), color.b));
    float mx1   = COMPAT_TEXTURE(NTSC_S14, pos1).a;
    float colmx = max(mx1, cm);
    float w3    = min((cm + 0.0001) / (colmx + 0.0005), 1.0);

    if (interb) {
        w3 = 1.00;
    }

    float2 dx = float2(0.001, 0.0);

    float mx0 = COMPAT_TEXTURE(NTSC_S14, pos1 - dx).a;
    float mx2 = COMPAT_TEXTURE(NTSC_S14, pos1 + dx).a;
    float mxg = max(max(mx0, mx1), max(mx2, cm));
    float mx  = pow(mxg, 1.40 / gamma_in);

    dx = float2(OriginalSize.z, 0.0) * 0.25;

    mx0 = COMPAT_TEXTURE(NTSC_S14, pos1 - dx).a;
    mx2 = COMPAT_TEXTURE(NTSC_S14, pos1 + dx).a;

    float mb = (1.0 - min(abs(mx0 - mx2) / (0.5 + mx1), 1.0));

    float3 orig1 = color;
    float3 one   = 1.0;

    float3 cmask = one;
    float3 dmask = one;
    float3 emask = one;

    float mwidths[15] = {0.0,  2.0, 3.0, 3.0,  6.0, 6.0, 2.4, 3.5, 2.4,
                         3.25, 3.5, 4.5, 4.25, 7.5, 6.25};

    float mwidth = mwidths[shadow_mask];

    float mask_compensate = frac(mwidth);

    if (shadow_mask > 0) {
        float2 maskcoord  = fracoord.xy * 1.00001;
        float2 scoord     = maskcoord;
        mwidth            = floor(mwidth) * masksize;
        float swidth      = mwidth;
        bool zoomed       = (abs(mask_zoom) > 0.75);
        float mscale      = 1.0;
        float2 maskcoord0 = maskcoord;
        maskcoord.y       = floor(maskcoord.y / masksize);
        float mwidth1     = max(mwidth + mask_zoom, 2.0);

        if (mshift > 0.25) {
            float stagg_lvl = 1.0;
            if (frac(mshift) > 0.25) {
                stagg_lvl = 2.0;
            }
            float next_line = float(floor(mod(maskcoord.y, 2.0 * stagg_lvl)) <
                                    stagg_lvl);
            maskcoord0.x    = maskcoord0.x + next_line * 0.5 * mwidth1;
        }

        maskcoord = maskcoord0 / masksize;

        if (!zoomed) {
            cmask *= crt_mask(floor(maskcoord), mx, mb);
        } else {
            mscale = mwidth1 / mwidth;

            float mlerp = frac(maskcoord.x / mscale);

            if (zoom_mask > 0.025) {
                mlerp = clamp((1.0 + zoom_mask) * mlerp - 0.5 * zoom_mask, 0.0, 1.0);
            }

            float mcoord = floor(maskcoord.x / mscale);
            if (shadow_mask == 13 && mask_zoom == -2.0) {
                mcoord = ceil(maskcoord.x / mscale);
            }

            cmask *= lerp(crt_mask(float2(mcoord, maskcoord.y), mx, mb),
                          crt_mask(float2(mcoord + 1.0, maskcoord.y), mx, mb),
                          mlerp);
        }

        if (slotwidth > 0.5) {
            swidth = slotwidth;
        }

        float smask     = 1.0;
        float sm_offset = 0.0;
        bool bsm_offset = (shadow_mask == 1 || shadow_mask == 3 ||
                           shadow_mask == 6 || shadow_mask == 7 ||
                           shadow_mask == 9 || shadow_mask == 12);

        if (zoomed) {
            if (mask_layout < 0.5 && bsm_offset) {
                sm_offset = 1.0;
            } else if (bsm_offset) {
                sm_offset = -1.0;
            }
        }

        swidth = round(swidth * mscale);
        smask  = slt_mask(scoord + float2(sm_offset, 0.0), mx, swidth);
        smask  = clamp(smask + lerp(smask_mit, 0.0,
                                   min(w3,
                                       pow(w3 * max(max(orig1.r, orig1.g), orig1.b),
                                           0.33333))),
                      0.0,
                      1.0);

        emask = cmask;
        cmask *= smask;
        dmask = cmask;

        if (abs(mask_bloom) > 0.025) {
            float maxbl = max(max(max(Bloom.r, Bloom.g), Bloom.b), mxg);
            maxbl = maxbl * max(lerp(1.0, 2.0 - colmx, bloom_dist), 0.0);

            if (mask_bloom > 0.025) {
                cmask = max(min(cmask + maxbl * mask_bloom, 1.0), cmask);
            } else {
                cmask = max(lerp(cmask,
                                 cmask * (1.0 - 0.5 * maxbl) + plant(pow(Bloom, 0.35.xxx), maxbl),
                                 -mask_bloom),
                            cmask);
            }
        }

        color = pow(color, mask_gamma / gamma_in);
        color = color * cmask;
        color = min(color, 1.0);
        color = pow(color, gamma_in / mask_gamma);

        cmask = min(cmask, 1.0);
        dmask = min(dmask, 1.0);
    }

    float dark_compensate = lerp(max(clamp(lerp(mcut, maskstr, mx), 0.0, 1.0) -
                                             1.0 + mask_compensate,
                                     0.0) + 1.0,
                                 1.0,
                                 mx);

    if (shadow_mask == 0) {
        dark_compensate = 1.0;
    }

    float bb = lerp(brightboost1, brightboost2, mx) * dark_compensate;
    color *= bb;

    float3 Ref = COMPAT_TEXTURE(NTSC_S08, pos).rgb;
    float maxb = COMPAT_TEXTURE(NTSC_S13, pos).a;

    float vig  = COMPAT_TEXTURE(NTSC_S02,
                                clamp((pos - 0.5) * BufferToViewportRatio + 0.5,
                                      0.0 + 0.5 * OriginalSize.zw,
                                      1.0 - 0.5 * OriginalSize.zw)).a;

    float3 bcmask = lerp(one, cmask, b_mask);
    float3 hcmask = lerp(one, cmask, h_mask);

    float3 Bloom1 = Bloom;

    if (abs(blm_1) > 0.025) {
        if (blm_1 < -0.01) {
            Bloom1 = plant(Bloom, maxb);
        }

        Bloom1 = min(Bloom1 * (orig1 + color),
                     max(0.5 * (colmx + orig1 - color), 0.001 * Bloom1));

        Bloom1 = 0.5 * (Bloom1 +
                        lerp(Bloom1, lerp(colmx * orig1, Bloom1, 0.5), 1.0 - color));

        Bloom1 = bcmask * Bloom1 * max(lerp(1.0, 2.0 - colmx, bloom_dist), 0.0);

        color = pow(pow(color, mask_gamma / gamma_in) +
                            abs(blm_1) * pow(Bloom1, mask_gamma / gamma_in),
                    gamma_in / mask_gamma);
    }

    if (!interb) {
        color = declip(min(color, 1.0), lerp(1.0, w3, 0.6));
    }

    if (halation > 0.01) {
        Bloom = 0.5 * (Bloom + Bloom * Bloom);

        float mbl = max(max(Bloom.r, Bloom.g), Bloom.b);
        float mxh = colmx + colmx * colmx;

        Bloom = plant(Bloom, max(1.25 * (mbl - 0.1375), 0.165 * mxh * (1.0 + w3)));
        Bloom = max((2.0 * lerp(maxb * maxb, maxb, colmx) -
                     0.5 * max(max(Ref.r, Ref.g), Ref.b)),
                    0.25) *
                Bloom;

        Bloom = min((2.5 - colmx + 0.5 * color) *
                            plant(0.375 + orig1,
                                  lerp(0.5 * (1.0 + w3), (0.50 + w3) / 1.5, colmx)) *
                            hcmask * Bloom,
                    1.0 - color);

        color = pow(pow(color, mask_gamma / gamma_in) +
                            halation * pow(Bloom, mask_gamma / gamma_in),
                    gamma_in / mask_gamma);

    } else if (halation < -0.01) {
        float mbl = max(max(Bloom.r, Bloom.g), Bloom.b);

        Bloom = plant(Bloom + Ref + orig1 + Bloom * Bloom * Bloom,
                      min(mbl * mbl, 0.75));

        color = color +
                2.0 * lerp(1.0, w3, 0.5 * colmx) * hcmask * Bloom * (-halation);
    }

    float w = 0.25 + 0.60 * lerp(w3, 1.0, sqrt(colmx));

    if (smoothmask) {
        color = min(color, 1.0);
        color = max(min(color / w3, 1.0) * w3, min(orig1 * bb, color * (1.0 - w3)));
    }

    if (m_glow == 0) {
        Glow = lerp(Glow, 0.25 * color, colmx);
    } else {
        float3 orig2 = plant(orig1 + 0.001 * Ref, 1.0);

        maxb  = max(max(Glow.r, Glow.g), Glow.b);
        Bloom = plant(Glow, 1.0);
        Ref   = abs(orig2 - Bloom);

        mx0 = max(max(orig2.r, orig2.g), orig2.b) -
              min(min(orig2.r, orig2.g), orig2.b);

        mx2 = max(max(Bloom.r, Bloom.g), Bloom.b) -
              min(min(Bloom.r, Bloom.g), Bloom.b);

        Bloom = lerp(maxb * min(Bloom, orig2),
                     w * lerp(lerp(Glow,
                                   max(max(Ref.r, Ref.g), Ref.b) * Glow,
                                   max(mx, mx0)),
                              lerp(color, Glow, mx2),
                              max(mx0, mx2) * Ref),
                     min(sqrt((1.10 - mx0) * (0.10 + mx2)), 1.0));

        if (m_glow == 2) {
            Glow = lerp(0.5 * Glow * Glow, Bloom, Bloom);
        }

        Glow = lerp(m_glow_low * Glow,
                    m_glow_high * Bloom,
                    pow(colmx, m_glow_dist / gamma_in));
    }

    if (m_glow == 0) {
        if (glow >= 0.0) {
            color = color + 0.5 * Glow * glow;
        } else {
            color = color + abs(glow) * min(emask * emask, 1.0) * Glow;
        }
    } else {
        float3 fmask = clamp(lerp(one, dmask, m_glow_mask), 0.0, 1.0);
        color        = color + abs(glow) * fmask * Glow;
    }

    color = min(color, 1.0);
    color = min(color, max(orig1, color) * lerp(one, dmask, mclip));
    color = pow(color, 1.0 / gamma_o);

    float rc = 0.6 * sqrt(max(max(color.r, color.g), color.b)) + 0.4;

    if (abs(addnoised) > 0.01) {
        float3 noise0 = noise(float3(floor(OutputSize.xy * fuxcoord / noiseresd),
                                     FrameCount));
        if (noisetype == 0) {
            color = lerp(color, noise0, 0.25 * abs(addnoised) * rc);
        } else {
            color = min(color * lerp(1.0, 1.5 * noise0.x, 0.5 * abs(addnoised)), 1.0);
        }
    }

    colmx = max(max(orig1.r, orig1.g), orig1.b);
    color = color + bmask * lerp(emask,
                                 0.125 * (1.0 - colmx) * color,
                                 min(20.0 * colmx, 1.0));

    return float4(color * vig * humbars(lerp(pos.y, pos.x, bardir)) * post_br *
                          corner((pos0 - 0.5) * BufferToViewportRatio + 0.5),
                  1.0);
}

technique CRT_Guest_NTSC
{
    pass Afterglow
    {
        VertexShader = PostProcessVS;
        PixelShader  = AfterglowPS;
        RenderTarget = NTSC_T01;
    }

    pass PreShader
    {
        VertexShader = PostProcessVS;
        PixelShader  = PreShaderPS;
        RenderTarget = NTSC_T02;
    }

    pass NTSCPASS1
    {
        VertexShader = PostProcessVS;
        PixelShader  = Signal_1_PS;
        RenderTarget = NTSC_T03;
    }

    pass NTSCPASS2
    {
        VertexShader = PostProcessVS;
        PixelShader  = Signal_2_PS;
        RenderTarget = NTSC_T04;
    }

    pass NTSCPASS3
    {
        VertexShader = PostProcessVS;
        PixelShader  = Signal_3_PS;
        RenderTarget = NTSC_T05;
    }

    pass Sharpness
    {
        VertexShader = PostProcessVS;
        PixelShader  = SharpnessPS;
        RenderTarget = NTSC_T06;
    }

    pass Luminance
    {
        VertexShader = PostProcessVS;
        PixelShader  = LuminancePS;
        RenderTarget = NTSC_T07;
    }

    pass Linearize
    {
        VertexShader = PostProcessVS;
        PixelShader  = LinearizePS;
        RenderTarget = NTSC_T08;
    }

    pass CRT_Pass1
    {
        VertexShader = PostProcessVS;
        PixelShader  = NTSC_TV1_PS;
        RenderTarget = NTSC_T09;
    }

    pass GaussianX
    {
        VertexShader = PostProcessVS;
        PixelShader  = HGaussianPS;
        RenderTarget = NTSC_T10;
    }

    pass GaussianY
    {
        VertexShader = PostProcessVS;
        PixelShader  = VGaussianPS;
        RenderTarget = NTSC_T11;
    }

    pass BloomHorz
    {
        VertexShader = PostProcessVS;
        PixelShader  = BloomHorzPS;
        RenderTarget = NTSC_T12;
    }

    pass BloomVert
    {
        VertexShader = PostProcessVS;
        PixelShader  = BloomVertPS;
        RenderTarget = NTSC_T13;
    }

    pass CRT_Pass2
    {
        VertexShader = PostProcessVS;
        PixelShader  = NTSC_TV2_PS;
        RenderTarget = NTSC_T14;
    }

    pass Chromatic
    {
        VertexShader = PostProcessVS;
        PixelShader  = ChromaticPS;
    }
}
