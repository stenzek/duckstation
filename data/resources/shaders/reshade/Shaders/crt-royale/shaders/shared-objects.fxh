#ifndef _SHARED_OBJECTS_H
#define _SHARED_OBJECTS_H

/////////////////////////////  GPL LICENSE NOTICE  /////////////////////////////

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


#include "../lib/helper-functions-and-macros.fxh"
#include "../lib/derived-settings-and-constants.fxh"
#include "../lib/bind-shader-params.fxh"


// Yes, the WIDTH/HEIGHT/SIZE defines are kinda weird.
// Yes, we have to have them or something similar. This is for D3D11 which
// returns (0, 0) when you call tex2Dsize() on the pass's render target.


// Pass 0 Buffer (cropPass)
//   Cannot be conditioned on __RENDERER__ b/c there are no
//     available buffers of the same size
//   Last usage is in interlacingPass
//     electronBeamPass -> beamConvergencePass
//     deinterlacePass -> phosphorMaskPass
//     brightpassPass -> bloomHorizontalPass
// #define TEX_CROP_WIDTH content_size.x
// #define TEX_CROP_HEIGHT content_size.y
// #define TEX_CROP_SIZE int2(TEX_CROP_WIDTH, TEX_CROP_HEIGHT)
// texture2D texCrop {
// 	Width = TEX_CROP_WIDTH;
// 	Height = TEX_CROP_HEIGHT;

// 	Format = RGBA16;
// };
// sampler2D samplerCrop { Texture = texCrop; };


// Pass 1 Buffer (interlacingPass)
//   Cannot be conditioned on __RENDERER__ b/c there are no
//     available buffers of the same size
//   Last usage is in electronBeamPass
//     beamConvergencPass -> freezeFramePass
//     phosphorMaskPass -> bloomHorizontalPass
// #define TEX_INTERLACED_WIDTH content_size.x
// #define TEX_INTERLACED_HEIGHT content_size.y
// #define TEX_INTERLACED_SIZE int2(TEX_INTERLACED_WIDTH, TEX_INTERLACED_HEIGHT)
// texture2D texInterlaced {
// 	Width = TEX_INTERLACED_WIDTH;
// 	Height = TEX_INTERLACED_HEIGHT;

// 	Format = RGBA16;
// };
// sampler2D samplerInterlaced { Texture = texInterlaced; };

// Pass 2 Buffer (electronBeamPass)
//   Last usage is in beamConvergencePass


#define TEX_PREBLUR_VERT_WIDTH content_size.x
#define TEX_PREBLUR_VERT_HEIGHT content_size.y
static const uint2 TEX_PREBLUR_SIZE = uint2(TEX_PREBLUR_VERT_WIDTH, TEX_PREBLUR_VERT_HEIGHT);
texture2D texPreblurVert < pooled = true; > {
	Width = TEX_PREBLUR_VERT_WIDTH;
	Height = TEX_PREBLUR_VERT_HEIGHT;

	Format = RGBA16;
};
sampler2D samplerPreblurVert { Texture = texPreblurVert; };

#define TEX_PREBLUR_HORIZ_WIDTH content_size.x
#define TEX_PREBLUR_HORIZ_HEIGHT content_size.y
static const uint2 TEX_PREBLUR_SIZE = uint2(TEX_PREBLUR_HORIZ_WIDTH, TEX_PREBLUR_HORIZ_HEIGHT);
texture2D texPreblurHoriz < pooled = true; > {
	Width = TEX_PREBLUR_HORIZ_WIDTH;
	Height = TEX_PREBLUR_HORIZ_HEIGHT;

	Format = RGBA16;
};
sampler2D samplerPreblurHoriz { Texture = texPreblurHoriz; };


#define TEX_BEAMDIST_WIDTH num_beamdist_color_samples
#define TEX_BEAMDIST_HEIGHT num_beamdist_dist_samples
#define TEX_BEAMDIST_SIZE int2(TEX_BEAMDIST_WIDTH, TEX_BEAMDIST_HEIGHT)
texture2D texBeamDist < pooled = false; > {
	Width = TEX_BEAMDIST_WIDTH;
	Height = TEX_BEAMDIST_HEIGHT;


	Format = RGB10A2;
};
sampler2D samplerBeamDist {
	Texture = texBeamDist;
	AddressV = WRAP;
};


// Pass 2 Buffer (electronBeamPass)
//   Last usage is in beamConvergencePass
#define TEX_ELECTRONBEAMS_WIDTH content_size.x
#define TEX_ELECTRONBEAMS_HEIGHT content_size.y
#define TEX_ELECTRONBEAMS_SIZE int2(TEX_ELECTRONBEAMS_WIDTH, TEX_ELECTRONBEAMS_HEIGHT)
texture2D texElectronBeams < pooled = true; > {
	Width = TEX_ELECTRONBEAMS_WIDTH;
	Height = TEX_ELECTRONBEAMS_HEIGHT;

	Format = RGBA16;
};
sampler2D samplerElectronBeams {
	Texture = texElectronBeams;

	AddressU = BORDER;
	AddressV = BORDER;
};
// 	#define texElectronBeams texCrop
// 	#define samplerElectronBeams samplerCrop


// Pass 3 Buffer (beamConvergencPass)
//   Last usage is freezeFramePass
#define TEX_BEAMCONVERGENCE_WIDTH content_size.x
#define TEX_BEAMCONVERGENCE_HEIGHT content_size.y
#define TEX_BEAMCONVERGENCE_SIZE int2(TEX_BEAMCONVERGENCE_WIDTH, TEX_BEAMCONVERGENCE_HEIGHT)
texture2D texBeamConvergence < pooled = true; > {
	Width = TEX_BEAMCONVERGENCE_WIDTH;
	Height = TEX_BEAMCONVERGENCE_HEIGHT;
	
	Format = RGBA16;
};
sampler2D samplerBeamConvergence { Texture = texBeamConvergence; };
// #define texBeamConvergence texInterlaced
// #define samplerBeamConvergence samplerInterlaced


/*
// Pass 4 Buffer (bloomApproxPass)
//   Cannot be conditioned on __RENDERER__ b/c there are no
//     available buffers of the same size
//   Last usage is in brightpassPass
#define TEX_BLOOMAPPROX_WIDTH 320
#define TEX_BLOOMAPPROX_HEIGHT 240
#define TEX_BLOOMAPPROX_SIZE int2(TEX_BLOOMAPPROX_WIDTH, TEX_BLOOMAPPROX_HEIGHT)
texture2D texBloomApprox {
	Width = TEX_BLOOMAPPROX_WIDTH;
	Height = TEX_BLOOMAPPROX_HEIGHT;

	Format = RGBA16;
};
sampler2D samplerBloomApprox { Texture = texBloomApprox; };
*/

// Pass 4a Buffer (bloomApproxVerticalPass)
//   Cannot be conditioned on __RENDERER__ b/c there are no
//     available buffers of the same size
//   Last usage is in brightpassPass
#define TEX_BLOOMAPPROXVERT_WIDTH content_size.x
// #define TEX_BLOOMAPPROXVERT_HEIGHT 240
#define TEX_BLOOMAPPROXVERT_HEIGHT int(content_size.y / bloomapprox_downsizing_factor)
#define TEX_BLOOMAPPROXVERT_SIZE int2(TEX_BLOOMAPPROXVERT_WIDTH, TEX_BLOOMAPPROXVERT_HEIGHT)
texture2D texBloomApproxVert < pooled = true; > {
	Width = TEX_BLOOMAPPROXVERT_WIDTH;
	Height = TEX_BLOOMAPPROXVERT_HEIGHT;

	Format = RGBA16;
};
sampler2D samplerBloomApproxVert { Texture = texBloomApproxVert; };

// Pass 4b Buffer (bloomApproxHorizontalPass)
//   Cannot be conditioned on __RENDERER__ b/c there are no
//     available buffers of the same size
//   Last usage is in brightpassPass
// #define TEX_BLOOMAPPROXHORIZ_WIDTH 320
// #define TEX_BLOOMAPPROXHORIZ_HEIGHT 240
#define TEX_BLOOMAPPROXHORIZ_WIDTH int(content_size.x / bloomapprox_downsizing_factor)
#define TEX_BLOOMAPPROXHORIZ_HEIGHT TEX_BLOOMAPPROXVERT_HEIGHT
#define TEX_BLOOMAPPROXHORIZ_SIZE int2(TEX_BLOOMAPPROXHORIZ_WIDTH, TEX_BLOOMAPPROXHORIZ_HEIGHT)
texture2D texBloomApproxHoriz < pooled = true; > {
	Width = TEX_BLOOMAPPROXHORIZ_WIDTH;
	Height = TEX_BLOOMAPPROXHORIZ_HEIGHT;

	Format = RGBA16;
};
sampler2D samplerBloomApproxHoriz { Texture = texBloomApproxHoriz; };

// Pass 5 Buffer (blurVerticalPass)
//   Cannot be conditioned on __RENDERER__ b/c there are no
//     available buffers of the same size
//   Last usage is blurHorizontalPass
#define TEX_BLURVERTICAL_WIDTH TEX_BLOOMAPPROXHORIZ_WIDTH
#define TEX_BLURVERTICAL_HEIGHT TEX_BLOOMAPPROXHORIZ_HEIGHT
#define TEX_BLURVERTICAL_SIZE int2(TEX_BLURVERTICAL_WIDTH, TEX_BLURVERTICAL_HEIGHT)
texture2D texBlurVertical < pooled = true; > {
	Width = TEX_BLURVERTICAL_WIDTH;
	Height = TEX_BLURVERTICAL_HEIGHT;

	Format = RGBA16;
};
sampler2D samplerBlurVertical { Texture = texBlurVertical; };


// Pass 6 Buffer (blurHorizontalPass)
//   Cannot be conditioned on __RENDERER__ b/c there are no
//     available buffers of the same size
//   Last usage is bloomHorizontalPass
#define TEX_BLURHORIZONTAL_WIDTH TEX_BLOOMAPPROXHORIZ_WIDTH
#define TEX_BLURHORIZONTAL_HEIGHT TEX_BLOOMAPPROXHORIZ_HEIGHT
#define TEX_BLURHORIZONTAL_SIZE int2(TEX_BLURHORIZONTAL_WIDTH, TEX_BLURHORIZONTAL_HEIGHT)
texture2D texBlurHorizontal < pooled = true; > {
	Width = TEX_BLURHORIZONTAL_WIDTH;
	Height = TEX_BLURHORIZONTAL_HEIGHT;

	Format = RGBA16;
};
sampler2D samplerBlurHorizontal { Texture = texBlurHorizontal; };


// Pass 7 (deinterlacePass)
//   Last usage is phosphorMaskPass
#define TEX_DEINTERLACE_WIDTH content_size.x
#define TEX_DEINTERLACE_HEIGHT content_size.y
#define TEX_DEINTERLACE_SIZE int2(TEX_DEINTERLACE_WIDTH, TEX_DEINTERLACE_HEIGHT)
#if _DX9_ACTIVE == 0
	texture2D texDeinterlace < pooled = true; > {
		Width = TEX_DEINTERLACE_WIDTH;
		Height = TEX_DEINTERLACE_HEIGHT;

		Format = RGBA16;
	};
	sampler2D samplerDeinterlace { Texture = texDeinterlace; };
#else
	#define texDeinterlace texElectronBeams
	#define samplerDeinterlace samplerElectronBeams
#endif

// Pass 8 (freezeFramePass)
// Do not condition this on __RENDERER__. It will not work if another
//   pass corrupts it.
#define TEX_FREEZEFRAME_WIDTH content_size.x
#define TEX_FREEZEFRAME_HEIGHT content_size.y
#define TEX_FREEZEFRAME_SIZE int2(TEX_FREEZEFRAME_WIDTH, TEX_FREEZEFRAME_HEIGHT
texture2D texFreezeFrame < pooled = false; > {
	Width = TEX_FREEZEFRAME_WIDTH;
	Height = TEX_FREEZEFRAME_HEIGHT;

	Format = RGBA16;
};
sampler2D samplerFreezeFrame { Texture = texFreezeFrame; };


// Pass 10 Mask Texture (phosphorMaskResizeHorizontalPass)
//   Cannot be conditioned on __RENDERER__ b/c there are no
//     available buffers of the same size
#define TEX_PHOSPHORMASK_WIDTH content_size.x
#define TEX_PHOSPHORMASK_HEIGHT content_size.y
#define TEX_PHOSPHORMASKL_SIZE int2(TEX_PHOSPHORMASK_WIDTH, TEX_PHOSPHORMASK_HEIGHT)
texture2D texPhosphorMask < pooled = false; > {
	Width = TEX_PHOSPHORMASK_WIDTH;
	Height = TEX_PHOSPHORMASK_HEIGHT;
	
	Format = RGBA16;
};
sampler2D samplerPhosphorMask { Texture = texPhosphorMask; };


// Pass 11 Buffer (phosphorMaskPass)
//   Last usage is bloomHorizontalPass
#define TEX_MASKEDSCANLINES_WIDTH content_size.x
#define TEX_MASKEDSCANLINES_HEIGHT content_size.y
#define TEX_MASKEDSCANLINES_SIZE int2(TEX_MASKEDSCANLINES_WIDTH, TEX_MASKEDSCANLINES_HEIGHT)

#if _DX9_ACTIVE == 0
	texture2D texMaskedScanlines < pooled = true; > {
		Width = TEX_MASKEDSCANLINES_WIDTH;
		Height = TEX_MASKEDSCANLINES_HEIGHT;

		Format = RGBA16;
	};
	sampler2D samplerMaskedScanlines { Texture = texMaskedScanlines; };
#else
	#define texMaskedScanlines texBeamConvergence
	#define samplerMaskedScanlines samplerBeamConvergence
#endif


// Pass 12 Buffer (brightpassPass)
//   Last usage is bloomHorizontalPass
#define TEX_BRIGHTPASS_WIDTH content_size.x
#define TEX_BRIGHTPASS_HEIGHT content_size.y
#define TEX_BRIGHTPASS_SIZE int2(TEX_BRIGHTPASS_WIDTH, TEX_BRIGHTPASS_HEIGHT)

#if _DX9_ACTIVE == 0
	texture2D texBrightpass < pooled = true; > {
		Width = TEX_BRIGHTPASS_WIDTH;
		Height = TEX_BRIGHTPASS_HEIGHT;

		Format = RGBA16;
	};
	sampler2D samplerBrightpass { Texture = texBrightpass; };
#else
	#define texBrightpass texElectronBeams
	#define samplerBrightpass samplerElectronBeams
#endif


// Pass 13 Buffer (bloomVerticalPass)
//   Cannot be conditioned on __RENDERER__ b/c there are no
//     available buffers of the same size
//   Last usage is bloomHorizontalPass
#define TEX_BLOOMVERTICAL_WIDTH content_size.x
#define TEX_BLOOMVERTICAL_HEIGHT content_size.y
#define TEX_BLOOMVERTICAL_SIZE int2(TEX_BLOOMVERTICAL_WIDTH, TEX_BLOOMVERTICAL_HEIGHT)
texture2D texBloomVertical < pooled = true; > {
	Width = TEX_BLOOMVERTICAL_WIDTH;
	Height = TEX_BLOOMVERTICAL_HEIGHT;

	Format = RGBA16;
};
sampler2D samplerBloomVertical { Texture = texBloomVertical; };


// Pass 14 Buffer (bloomHorizontalPass)
//   Cannot be conditioned on __RENDERER__ b/c there are no
//     available buffers of the same size
//   Last usage is geometryPass
#define TEX_BLOOMHORIZONTAL_WIDTH content_size.x
#define TEX_BLOOMHORIZONTAL_HEIGHT content_size.y
#define TEX_BLOOMHORIZONTAL_SIZE int2(TEX_BLOOMHORIZONTAL_WIDTH, TEX_BLOOMHORIZONTAL_HEIGHT)
texture2D texBloomHorizontal < pooled = true; > {
	Width = TEX_BLOOMHORIZONTAL_WIDTH;
	Height = TEX_BLOOMHORIZONTAL_HEIGHT;

	Format = RGBA16;
};
sampler2D samplerBloomHorizontal { Texture = texBloomHorizontal; };


// Pass 15 Buffer (geometryPass)
//   Last usage is uncropPass
#define TEX_GEOMETRY_WIDTH content_size.x
#define TEX_GEOMETRY_HEIGHT content_size.y
#define TEX_GEOMETRY_SIZE int2(TEX_GEOMETRY_WIDTH, TEX_GEOMETRY_HEIGHT)

#if _DX9_ACTIVE == 0
	texture2D texGeometry < pooled = true; > {
		Width = TEX_GEOMETRY_WIDTH;
		Height = TEX_GEOMETRY_HEIGHT;

		Format = RGBA16;
	};
	sampler2D samplerGeometry { Texture = texGeometry; };
#else
	#define texGeometry texElectronBeams
	#define samplerGeometry samplerElectronBeams
#endif

#endif  // _SHARED_OBJECTS_H