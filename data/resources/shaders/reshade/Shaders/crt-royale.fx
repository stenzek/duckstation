#include "ReShade.fxh"

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

// Enable or disable the shader
#ifndef CONTENT_BOX_VISIBLE
	#define CONTENT_BOX_VISIBLE 0
#endif

#include "crt-royale/shaders/content-box.fxh"

#if !CONTENT_BOX_VISIBLE
	#include "crt-royale/shaders/input-blurring.fxh"
	#include "crt-royale/shaders/electron-beams.fxh"
	#include "crt-royale/shaders/blurring.fxh"
	#include "crt-royale/shaders/deinterlace.fxh"
	#include "crt-royale/shaders/phosphor-mask.fxh"
	#include "crt-royale/shaders/brightpass.fxh"
	#include "crt-royale/shaders/bloom.fxh"
	#include "crt-royale/shaders/geometry-aa-last-pass.fxh"
#endif


technique CRT_Royale
{
	// Toggle the content box to help users configure it
	#if CONTENT_BOX_VISIBLE
		pass contentBoxPass
		{
			// content-box.fxh
			// Draw a box that displays the crop we'll perform.
			VertexShader = PostProcessVS;
			PixelShader = contentBoxPixelShader;
		}
	#else
		#if ENABLE_PREBLUR
			pass PreblurVert
			{
				// input-blurring.fxh
				// Optionally blur the input buffer a little
				VertexShader = contentCropVS;
				PixelShader = preblurVertPS;

				RenderTarget = texPreblurVert;

				PrimitiveTopology = TRIANGLESTRIP;
				VertexCount = 4;
			}
			pass PreblurHoriz
			{
				// input-blurring.fxh
				VertexShader = PostProcessVS;
				PixelShader = preblurHorizPS;
				
				RenderTarget = texPreblurHoriz;
			}
		#endif
		pass beamDistPass
		{
			// electron-beams.fxh
			// Simulate emission of the interlaced video as electron beams. 	
			VertexShader = calculateBeamDistsVS;
			PixelShader = calculateBeamDistsPS;

			RenderTarget = texBeamDist;

			// This lets us improve performance by only computing the mask every k frames
			ClearRenderTargets = false;
		}
		pass electronBeamPass
		{
			// electron-beams.fxh
			// Simulate emission of the interlaced video as electron beams. 	
			VertexShader = simulateEletronBeamsVS;
			PixelShader = simulateEletronBeamsPS;

			RenderTarget = texElectronBeams;

			// If the preblur passes are disabled, we have to crop in this pass
			#if !ENABLE_PREBLUR
			PrimitiveTopology = TRIANGLESTRIP;
			VertexCount = 4;
			#endif
		}
		pass beamConvergencePass
		{
			// electron-beams.fxh
			// Simulate beam convergence miscalibration
			//   Not to be confused with beam purity
			VertexShader = beamConvergenceVS;
			PixelShader = beamConvergencePS;

			RenderTarget = texBeamConvergence;
		}
		pass bloomApproxPassVert
		{
			// bloom.fxh
			VertexShader = PostProcessVS;
			PixelShader = approximateBloomVertPS;
			
			RenderTarget = texBloomApproxVert;
		}
		pass bloomApproxPassHoriz
		{
			// bloom.fxh
			VertexShader = PostProcessVS;
			PixelShader = approximateBloomHorizPS;
			
			RenderTarget = texBloomApproxHoriz;
		}
		pass blurVerticalPass
		{
			// blurring.fxh
			// Vertically blur the approx bloom
			VertexShader = blurVerticalVS;
			PixelShader = blurVerticalPS;
			
			RenderTarget = texBlurVertical;
		}
		pass blurHorizontalPass
		{
			// blurring.fxh
			// Horizontally blur the approx bloom
			VertexShader = blurHorizontalVS;
			PixelShader = blurHorizontalPS;
			
			RenderTarget = texBlurHorizontal;
		}
		pass deinterlacePass
		{
			// deinterlace.fxh
			// Optionally deinterlace the video if interlacing is enabled.
			//   Can help approximate the original crt-royale's appearance
			//   without some issues like image retention.
			VertexShader = deinterlaceVS;
			PixelShader = deinterlacePS;
			
			RenderTarget = texDeinterlace;
		}
		pass freezeFramePass
		{
			// deinterlace.fxh
			// Capture the current frame, so we can use it in the next
			//   frame's deinterlacing pass.
			VertexShader = freezeFrameVS;
			PixelShader = freezeFramePS;

			RenderTarget = texFreezeFrame;

			// Explicitly disable clearing render targets
			//   scanlineBlendPass will not work properly if this ever defaults to true
			ClearRenderTargets = false;
		}
		pass generatePhosphorMask
		{
			// phosphor-mask.fxh
			VertexShader = generatePhosphorMaskVS;
			PixelShader = generatePhosphorMaskPS;

			RenderTarget = texPhosphorMask;

			// This lets us improve performance by only computing the mask every k frames
			ClearRenderTargets = false;
			
			PrimitiveTopology = TRIANGLESTRIP;
			VertexCount = 4;
		}
		pass applyPhosphormask
		{
			// phosphor-mask.fxh
			// Tile the scaled phosphor mask and apply it to
			//   the deinterlaced image.
			VertexShader = PostProcessVS;
			PixelShader = applyComputedPhosphorMaskPS;
			
			RenderTarget = texMaskedScanlines;
			// RenderTarget = texGeometry;
		}
		pass brightpassPass
		{
			// brightpass.fxh
			// Apply a brightpass filter for the bloom effect
			VertexShader = brightpassVS;
			PixelShader = brightpassPS;
			
			RenderTarget = texBrightpass;
		}
		pass bloomVerticalPass
		{
			// bloom.fxh
			// Blur vertically for the bloom effect
			VertexShader = bloomVerticalVS;
			PixelShader = bloomVerticalPS;
			
			RenderTarget = texBloomVertical;
		}
		pass bloomHorizontalPass
		{
			// bloom.fxh
			// Blur horizontally for the bloom effect.
			//   Also apply various color changes and effects.
			VertexShader = bloomHorizontalVS;
			PixelShader = bloomHorizontalPS;
			
			RenderTarget = texBloomHorizontal;
		}
		pass geometryPass
		{
			// geometry-aa-last-pass.fxh
			// Apply screen geometry and anti-aliasing.
			VertexShader = geometryVS;
			PixelShader = geometryPS;

			RenderTarget = texGeometry;
		}
		pass uncropPass
		{
			// content-box.fxh
			// Uncrop the video, so we draw the game's content
			//   in the same position it started in.
			VertexShader = contentUncropVS;
			PixelShader = uncropContentPixelShader;
			
			PrimitiveTopology = TRIANGLESTRIP;
			VertexCount = 4;
		}
	#endif
}