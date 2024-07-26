#include "ReShade.fxh"

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


//  Ported to Duckstation (ReShade specs) by Hyllian (2024).

//  Set shader params for all passes here:

uniform float crt_gamma <
    ui_type = "drag";
    ui_min = 1.0;
    ui_max = 5.0;
    ui_step = 0.025;
    ui_label = "Simulated CRT Gamma";
    ui_category = "Display Settings";
> = 2.5;

uniform float lcd_gamma <
    ui_type = "drag";
    ui_min = 1.0;
    ui_max = 5.0;
    ui_step = 0.025;
    ui_label = "Your Display Gamma";
    ui_category = "Display Settings";
> = 2.2;

uniform float levels_contrast <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 4.0;
    ui_step = 0.015625;
    ui_label = "Contrast";
    ui_category = "Display Settings";
> = 1.0;

uniform float halation_weight <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.005;
    ui_label = "Halation Weight";
    ui_category = "Effects";
> = 0.0;

uniform float diffusion_weight <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.005;
    ui_label = "Diffusion Weight";
    ui_category = "Effects";
> = 0.075;

uniform float bloom_underestimate_levels <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 5.0;
    ui_step = 0.01;
    ui_label = "Bloom - Underestimate Levels";
    ui_category = "Effects";
> = 0.8;

uniform float bloom_excess <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.005;
    ui_label = "Bloom - Excess";
    ui_category = "Effects";
> = 0.0;

uniform float beam_min_sigma <
    ui_type = "drag";
    ui_min = 0.005;
    ui_max = 1.0;
    ui_step = 0.005;
    ui_label = "Min Sigma";
    ui_category = "Beam Dynamics";
> = 0.02;

uniform float beam_max_sigma <
    ui_type = "drag";
    ui_min = 0.005;
    ui_max = 1.0;
    ui_step = 0.005;
    ui_label = "Max Sigma";
    ui_category = "Beam Dynamics";
> = 0.3;

uniform float beam_spot_power <
    ui_type = "drag";
    ui_min = 0.01;
    ui_max = 16.0;
    ui_step = 0.01;
    ui_label = "Spot Power";
    ui_category = "Beam Dynamics";
> = 0.33;

uniform float beam_min_shape <
    ui_type = "drag";
    ui_min = 2.0;
    ui_max = 32.0;
    ui_step = 0.1;
    ui_label = "Min Shape";
    ui_category = "Beam Dynamics";
> = 2.0;

uniform float beam_max_shape <
    ui_type = "drag";
    ui_min = 2.0;
    ui_max = 32.0;
    ui_step = 0.1;
    ui_label = "Max Shape";
    ui_category = "Beam Dynamics";
> = 4.0;

uniform float beam_shape_power <
    ui_type = "drag";
    ui_min = 0.01;
    ui_max = 16.0;
    ui_step = 0.01;
    ui_label = "Shape Power";
    ui_category = "Beam Dynamics";
> = 0.25;

uniform int beam_horiz_filter <
    ui_type = "combo";
    ui_items = "Quilez\0Gaussian\0Lanczos\0";
    ui_label = "Horizontal Filter";
    ui_category = "Beam Dynamics";
> = 0;

uniform float beam_horiz_sigma <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 0.67;
    ui_step = 0.005;
    ui_label = "Horizontal Sigma";
    ui_category = "Beam Dynamics";
> = 0.35;

uniform float beam_horiz_linear_rgb_weight <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.01;
    ui_label = "Horiz Linear RGB Weight";
    ui_category = "Beam Dynamics";
> = 1.0;

uniform float convergence_offset_x_r <
    ui_type = "drag";
    ui_min = -4.0;
    ui_max = 4.0;
    ui_step = 0.05;
    ui_label = "Offset X Red";
    ui_category = "Convergence";
> = 0.0;

uniform float convergence_offset_x_g <
    ui_type = "drag";
    ui_min = -4.0;
    ui_max = 4.0;
    ui_step = 0.05;
    ui_label = "Offset X Green";
    ui_category = "Convergence";
> = 0.0;

uniform float convergence_offset_x_b <
    ui_type = "drag";
    ui_min = -4.0;
    ui_max = 4.0;
    ui_step = 0.05;
    ui_label = "Offset X Blue";
    ui_category = "Convergence";
> = 0.0;

uniform float convergence_offset_y_r <
    ui_type = "drag";
    ui_min = -2.0;
    ui_max = 2.0;
    ui_step = 0.05;
    ui_label = "Offset Y Red";
    ui_category = "Convergence";
> = 0.0;

uniform float convergence_offset_y_g <
    ui_type = "drag";
    ui_min = -2.0;
    ui_max = 2.0;
    ui_step = 0.05;
    ui_label = "Offset Y Green";
    ui_category = "Convergence";
> = 0.0;

uniform float convergence_offset_y_b <
    ui_type = "drag";
    ui_min = -2.0;
    ui_max = 2.0;
    ui_step = 0.05;
    ui_label = "Offset Y Blue";
    ui_category = "Convergence";
> = 0.0;

uniform int mask_type <
    ui_type = "combo";
    ui_items = "Aperture Grille\0Slot Mask\0Shadow Mask\0";
    ui_label = "Type";
    ui_category = "Mask";
> = 0;

uniform float mask_sample_mode_desired <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 2.0;
    ui_step = 1.;
    ui_label = "Sample Mode";
    ui_category = "Mask";
> = 0.0;

uniform float mask_specify_num_triads <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 1.0;
    ui_label = "Specify Number of Triads";
    ui_category = "Mask";
> = 0.0;

uniform float mask_triad_size_desired <
    ui_type = "drag";
    ui_min = 1.0;
    ui_max = 18.0;
    ui_step = 0.125;
    ui_label = "Triad Size Desired";
    ui_category = "Mask";
> = 3.0;

uniform float mask_num_triads_desired <
    ui_type = "drag";
    ui_min = 342.0;
    ui_max = 1920.0;
    ui_step = 1.0;
    ui_label = "Number of Triads Desired";
    ui_category = "Mask";
> = 480.0;

uniform bool interlace_detect <
    ui_type = "radio";
    ui_label = "Enable Interlacing Detection";
    ui_category = "Interlacing";
> = true;

uniform bool interlace_bff <
    ui_type = "radio";
    ui_label = "Bottom Field First";
    ui_category = "Interlacing";
> = false;

uniform bool interlace_1080i <
    ui_type = "radio";
    ui_label = "Detect 1080i";
    ui_category = "Interlacing";
> = false;


uniform float  FrameCount < source = "framecount"; >;
uniform float2 BufferToViewportRatio < source = "buffer_to_viewport_ratio"; >;
uniform float2 InternalPixelSize < source = "internal_pixel_size"; >;
uniform float2 NativePixelSize < source = "native_pixel_size"; >;
uniform float2 NormalizedInternalPixelSize < source = "normalized_internal_pixel_size"; >;
uniform float2 NormalizedNativePixelSize < source = "normalized_native_pixel_size"; >;
uniform float  UpscaleMultiplier < source = "upscale_multiplier"; >;
uniform float2 ViewportSize < source = "viewportsize"; >;
uniform float  ViewportWidth < source = "viewportwidth"; >;
uniform float  ViewportHeight < source = "viewportheight"; >;

#include "../misc/include/geom.fxh"

#define VIEWPORT_SIZE (ViewportSize*BufferToViewportRatio)
#define TEXTURE_SIZE  (1.0/NormalizedNativePixelSize)

#define ORIG_LINEARIZED_texture_size    TEXTURE_SIZE
#define VERTICAL_SCANLINES_texture_size TEXTURE_SIZE
#define BLOOM_APPROX_texture_size       TEXTURE_SIZE
#define BLUR9FAST_VERTICAL_texture_size TEXTURE_SIZE
#define HALATION_BLUR_texture_size      TEXTURE_SIZE
#define MASK_RESIZE_VERT_texture_size   TEXTURE_SIZE
#define MASK_RESIZE_texture_size        float2(64.0,0.0625*((VIEWPORT_SIZE).y))
#define MASKED_SCANLINES_texture_size   (0.0625*VIEWPORT_SIZE)
#define BRIGHTPASS_texture_size         VIEWPORT_SIZE
#define BLOOM_VERTICAL_texture_size     VIEWPORT_SIZE
#define BLOOM_HORIZONTAL_texture_size   VIEWPORT_SIZE

#define ORIG_LINEARIZED_video_size      ORIG_LINEARIZED_texture_size
#define VERTICAL_SCANLINES_video_size   VERTICAL_SCANLINES_texture_size
#define BLOOM_APPROX_video_size         BLOOM_APPROX_texture_size
#define BLUR9FAST_VERTICAL_video_size   BLUR9FAST_VERTICAL_texture_size
#define HALATION_BLUR_video_size        HALATION_BLUR_texture_size
#define MASK_RESIZE_VERT_video_size     MASK_RESIZE_VERT_texture_size
#define MASK_RESIZE_video_size          MASK_RESIZE_texture_size
#define MASKED_SCANLINES_video_size     MASKED_SCANLINES_texture_size
#define BRIGHTPASS_video_size           BRIGHTPASS_texture_size
#define BLOOM_VERTICAL_video_size       BLOOM_VERTICAL_texture_size
#define BLOOM_HORIZONTAL_video_size     BLOOM_HORIZONTAL_texture_size

#define video_size texture_size


texture2D tmask_grille_texture_small < source = "crt-royale/TileableLinearApertureGrille15Wide8And5d5SpacingResizeTo64.png"; > {Width=64.0;Height=64.0;MipLevels=0;};
texture2D tmask_slot_texture_small < source = "crt-royale/TileableLinearSlotMaskTall15Wide9And4d5Horizontal9d14VerticalSpacingResizeTo64.png"; > {Width=64.0;Height=64.0;MipLevels=0;};
texture2D tmask_shadow_texture_small < source = "crt-royale/TileableLinearShadowMaskEDPResizeTo64.png"; > {Width=64.0;Height=64.0;MipLevels=0;};

texture2D tmask_grille_texture_large < source = "crt-royale/TileableLinearApertureGrille15Wide8And5d5Spacing.png"; > {Width=512.0;Height=512.0;MipLevels=4;};
texture2D tmask_slot_texture_large < source = "crt-royale/TileableLinearSlotMaskTall15Wide9And4d5Horizontal9d14VerticalSpacing.png"; > {Width=512.0;Height=512.0;MipLevels=4;};
texture2D tmask_shadow_texture_large < source = "crt-royale/TileableLinearShadowMaskEDP.png"; > {Width=512.0;Height=512.0;MipLevels=4;};

sampler2D mask_grille_texture_small { Texture = tmask_grille_texture_small; AddressU = REPEAT; AddressV = REPEAT; MinFilter = POINT; MagFilter = POINT;};
sampler2D mask_slot_texture_small { Texture = tmask_slot_texture_small; AddressU = REPEAT; AddressV = REPEAT; MinFilter = POINT; MagFilter = POINT;};
sampler2D mask_shadow_texture_small { Texture = tmask_shadow_texture_small; AddressU = REPEAT; AddressV = REPEAT; MinFilter = POINT; MagFilter = POINT;};

sampler2D mask_grille_texture_large { Texture = tmask_grille_texture_large; AddressU = REPEAT; AddressV = REPEAT; MinFilter = POINT; MagFilter = POINT;};
sampler2D mask_slot_texture_large { Texture = tmask_slot_texture_large; AddressU = REPEAT; AddressV = REPEAT; MinFilter = POINT; MagFilter = POINT;};
sampler2D mask_shadow_texture_large { Texture = tmask_shadow_texture_large; AddressU = REPEAT; AddressV = REPEAT; MinFilter = POINT; MagFilter = POINT;};


#ifndef DEBUG_PASSES
    #define DEBUG_PASSES 11
#endif



texture2D tORIG_LINEARIZED{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA16f;};
sampler2D ORIG_LINEARIZED{Texture=tORIG_LINEARIZED;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=LINEAR;MinFilter=LINEAR;};

#if (DEBUG_PASSES > 1)
texture2D tVERTICAL_SCANLINES{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA16f;};
sampler2D VERTICAL_SCANLINES{Texture=tVERTICAL_SCANLINES;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=LINEAR;MinFilter=LINEAR;};
#endif
#if (DEBUG_PASSES > 2)
texture2D tBLOOM_APPROX{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA16f;};
sampler2D BLOOM_APPROX{Texture=tBLOOM_APPROX;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=LINEAR;MinFilter=LINEAR;};
#endif

#if (DEBUG_PASSES > 3)
// Need checking if it's really necessary to rendertarget.
texture2D tBLUR9FAST_VERTICAL{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA16f;};
sampler2D BLUR9FAST_VERTICAL{Texture=tBLUR9FAST_VERTICAL;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=LINEAR;MinFilter=LINEAR;};
#endif
#if (DEBUG_PASSES > 4)

texture2D tHALATION_BLUR{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA16f;};
sampler2D HALATION_BLUR{Texture=tHALATION_BLUR;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=LINEAR;MinFilter=LINEAR;};
#endif
#if (DEBUG_PASSES > 5)

texture2D tMASK_RESIZE_VERTICAL{Width=64.0;Height=BUFFER_HEIGHT*0.0625;Format=RGBA8;};
sampler2D MASK_RESIZE_VERTICAL{Texture=tMASK_RESIZE_VERTICAL;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=POINT;MinFilter=POINT;};
#endif
#if (DEBUG_PASSES > 6)

texture2D tMASK_RESIZE{Width=BUFFER_WIDTH*0.0625;Height=BUFFER_HEIGHT*0.0625;Format=RGBA8;};
sampler2D MASK_RESIZE{Texture=tMASK_RESIZE;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=POINT;MinFilter=POINT;};
#endif
#if (DEBUG_PASSES > 7)

texture2D tMASKED_SCANLINES{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA16f;};
sampler2D MASKED_SCANLINES{Texture=tMASKED_SCANLINES;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=LINEAR;MinFilter=LINEAR;};
#endif
#if (DEBUG_PASSES > 8)

texture2D tBRIGHTPASS{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA16f;};
sampler2D BRIGHTPASS{Texture=tBRIGHTPASS;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=LINEAR;MinFilter=LINEAR;};
#endif

#if (DEBUG_PASSES > 9)
texture2D tBLOOM_VERTICAL{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA16f;};
sampler2D BLOOM_VERTICAL{Texture=tBLOOM_VERTICAL;AddressU=BORDER;AddressV=BORDER;AddressW=BORDER;MagFilter=LINEAR;MinFilter=LINEAR;};
#endif



#include "crt-royale/src/crt-royale-first-pass-linearize-crt-gamma-bob-fields.fxh"

#if (DEBUG_PASSES > 1)
#include "crt-royale/src/crt-royale-scanlines-vertical-interlacing.fxh"
#endif
#if (DEBUG_PASSES > 2)
#include "crt-royale/src/crt-royale-bloom-approx.fxh"
#endif
#if (DEBUG_PASSES > 3)
#include "crt-royale/src/blur9fast-vertical.fxh"
#endif
#if (DEBUG_PASSES > 4)
#include "crt-royale/src/blur9fast-horizontal.fxh"
#endif
#if (DEBUG_PASSES > 5)
#include "crt-royale/src/crt-royale-mask-resize-vertical.fxh"
#endif
#if (DEBUG_PASSES > 6)
#include "crt-royale/src/crt-royale-mask-resize-horizontal.fxh"
#endif
#if (DEBUG_PASSES > 7)
#include "crt-royale/src/crt-royale-scanlines-horizontal-apply-mask.fxh"
#endif
#if (DEBUG_PASSES > 8)
#include "crt-royale/src/crt-royale-brightpass.fxh"
#endif
#if (DEBUG_PASSES > 9)
#include "crt-royale/src/crt-royale-bloom-vertical.fxh"
#endif
#if (DEBUG_PASSES > 10)
#include "crt-royale/src/crt-royale-bloom-horizontal-reconstitute.fxh"
#endif


technique CRT_Royale
{
   pass
   {
       VertexShader = VS_Linearize;
       PixelShader  = PS_Linearize;
       RenderTarget = tORIG_LINEARIZED;
   }
#if (DEBUG_PASSES > 1)
   pass
   {
       VertexShader = VS_Scanlines_Vertical_Interlacing;
       PixelShader  = PS_Scanlines_Vertical_Interlacing;
       RenderTarget = tVERTICAL_SCANLINES;
   }
#endif
#if (DEBUG_PASSES > 2)
   pass
   {
       VertexShader = VS_Bloom_Approx;
       PixelShader  = PS_Bloom_Approx;
       RenderTarget = tBLOOM_APPROX;
   }
#endif
#if (DEBUG_PASSES > 3)
   pass
   {
       VertexShader = VS_Blur9Fast_Vertical;
       PixelShader  = PS_Blur9Fast_Vertical;
       RenderTarget = tBLUR9FAST_VERTICAL;
   }
#endif
#if (DEBUG_PASSES > 4)
   pass
   {
       VertexShader = VS_Blur9Fast_Horizontal;
       PixelShader  = PS_Blur9Fast_Horizontal;
       RenderTarget = tHALATION_BLUR;
   }
#endif
#if (DEBUG_PASSES > 5)
   pass
   {
       VertexShader = VS_Mask_Resize_Vertical;
       PixelShader  = PS_Mask_Resize_Vertical;
       RenderTarget = tMASK_RESIZE_VERTICAL;
   }
#endif
#if (DEBUG_PASSES > 6)
   pass
   {
       VertexShader = VS_Mask_Resize_Horizontal;
       PixelShader  = PS_Mask_Resize_Horizontal;
       RenderTarget = tMASK_RESIZE;
   }
#endif
#if (DEBUG_PASSES > 7)
   pass
   {
       VertexShader = VS_Scanlines_Horizontal_Apply_Mask;
       PixelShader  = PS_Scanlines_Horizontal_Apply_Mask;
       RenderTarget = tMASKED_SCANLINES;
   }
#endif
#if (DEBUG_PASSES > 8)
   pass
   {
       VertexShader = VS_Brightpass;
       PixelShader  = PS_Brightpass;
       RenderTarget = tBRIGHTPASS;
   }
#endif
#if (DEBUG_PASSES > 9)
   pass
   {
       VertexShader = VS_Bloom_Vertical;
       PixelShader  = PS_Bloom_Vertical;
       RenderTarget = tBLOOM_VERTICAL;
   }
#endif
#if (DEBUG_PASSES > 10)
   pass
   {
       VertexShader = VS_Bloom_Horizontal;
       PixelShader  = PS_Bloom_Horizontal;
   }
#endif
}
