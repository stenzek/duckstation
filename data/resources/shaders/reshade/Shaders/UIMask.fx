/*
	Simple UIMask shader by luluco250

	I have no idea why this was never ported back to ReShade 3.0 from 2.0,
	but if you missed it, here it is.

	It doesn't feature the auto mask from the original shader.

	It does feature a new multi-channnel masking feature. UI masks can now contain
	separate 'modes' within each of the three color channels.

	For example, you can have the regular hud on the red channel (the default one),
	a mask for an inventory screen on the green channel and a mask for a quest menu
	on the blue channel. You can then use keyboard keys to toggle each channel on or off.

	Multiple channels can be active at once, they'll just add up to mask the image.

	Simple/legacy masks are not affected by this, they'll work just as you'd expect,
	so you can still make simple black and white masks that use all color channels, it'll
	be no different than just having it on a single channel.

	Tips:

	--You can adjust how much it will affect your HUD by changing "Mask Intensity".

	--You don't actually need to place the UIMask_Bottom technique at the bottom of
	  your shader pipeline, if you have any effects that don't necessarily affect
	  the visibility of the HUD you can place it before that.
	  For instance, if you use color correction shaders like LUT, you might want
	  to place UIMask_Bottom just before that.

	--Preprocessor flags:
	  --UIMASK_MULTICHANNEL:
		Enables having up to three different masks on each color channel.

	--Refer to this page for keycodes:
	  https://msdn.microsoft.com/en-us/library/windows/desktop/dd375731(v=vs.85).aspx

	--To make a custom mask:

	  1-Take a screenshot of your game with the HUD enabled,
	   preferrably with any effects disabled for maximum visibility.

	  2-Open the screenshot with your preferred image editor program, I use GIMP.

	  3-Make a background white layer if there isn't one already.
		Be sure to leave it behind your actual screenshot for the while.

	  4-Make an empty layer for the mask itself, you can call it "mask".

	  5-Having selected the mask layer, paint the places where HUD constantly is,
		such as health bars, important messages, minimaps etc.

	  6-Delete or make your screenshot layer invisible.

	  7-Before saving your mask, let's do some gaussian blurring to improve it's look and feel:
		For every step of blurring you want to do, make a new layer, such as:
		Mask - Blur16x16
		Mask - Blur8x8
		Mask - Blur4x4
		Mask - Blur2x2
		Mask - NoBlur
		You should use your image editor's default gaussian blurring filter, if there is one.
		This avoids possible artifacts and makes the mask blend more easily on the eyes.
		You may not need this if your mask is accurate enough and/or the HUD is simple enough.

	  8-Now save the final image with a unique name such as "MyUIMask.png" in your textures folder.

	  9-Set the preprocessor definition UIMASK_TEXTURE to the unique name of your image, with quotes.
	    You're done!


	MIT Licensed:

	Copyright (c) 2017 Lucas Melo

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all
	copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	SOFTWARE.
*/

//#region Preprocessor

#include "ReShade.fxh"
#include "ReShadeUI.fxh"

#ifndef UIMASK_MULTICHANNEL
	#define UIMASK_MULTICHANNEL 0
#endif

#if !UIMASK_MULTICHANNEL
	#define TEXFORMAT R8
#else
	#define TEXFORMAT RGBA8
#endif

#ifndef UIMASK_TEXTURE
	#define UIMASK_TEXTURE "UIMask.png"
#endif

//#endregion

namespace UIMask
{

//#region Uniforms

uniform int _Help
<
	ui_label = " ";
	ui_text =
		"For more detailed instructions, see the text at the top of this "
		"effect's shader file (UIMask.fx).\n"
		"\n"
		"Available preprocessor definitions:\n"
		"  UIMASK_MULTICHANNEL:\n"
		"    If set to 1, each of the RGB color channels in the texture is "
		"treated as a separate mask.\n"
		"\n"
		"How to create a mask:\n"
		"\n"
		"1. Take a screenshot with the game's UI appearing.\n"
		"2. Open the screenshot in an image editor, GIMP or Photoshop are "
		"recommended.\n"
		"3. Create a new layer over the screenshot layer, fill it with black.\n"
		"4. Reduce the layer opacity so you can see the screenshot layer "
		"below.\n"
		"5. Cover the UI with white to mask it from effects. The stronger the "
		"mask white color, the more opaque the mask will be.\n"
		"6. Set the mask layer opacity back to 100%.\n"
		"7. Save the image in one of your texture folders, making sure to "
		"use a unique name such as: \"MyUIMask.png\"\n"
		"8. Set the preprocessor definition UIMASK_TEXTURE to the name of "
		"your image, with quotes: \"MyUIMask.png\"\n"
		;
	ui_category = "Help";
	ui_category_closed = true;
	ui_type = "radio";
>;

uniform float fMask_Intensity
<
	__UNIFORM_SLIDER_FLOAT1

	ui_label = "Mask Intensity";
	ui_tooltip =
		"How much to mask effects from affecting the original image.\n"
		"\nDefault: 1.0";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.001;
> = 1.0;

uniform bool bDisplayMask <
	ui_label = "Display Mask";
	ui_tooltip =
		"Display the mask texture.\n"
		"Useful for testing multiple channels or simply the mask itself.\n"
		"\nDefault: Off";
> = false;

#if UIMASK_MULTICHANNEL

uniform bool bToggleRed <
	ui_label = "Toggle Red Channel";
	ui_tooltip = "Toggle UI masking for the red channel.\n"
		     "Right click to assign a hotkey.\n"
		     "\nDefault: On";
> = true;

uniform bool bToggleGreen <
	ui_label = "Toggle Green Channel";
	ui_tooltip = "Toggle UI masking for the green channel.\n"
		     "Right click to assign a hotkey."
		     "\nDefault: On";
> = true;

uniform bool bToggleBlue <
	ui_label = "Toggle Blue Channel";
	ui_tooltip = "Toggle UI masking for the blue channel.\n"
		     "Right click to assign a hotkey."
		     "\nDefault: On";
> = true;

#endif

//#endregion

//#region Textures

texture BackupTex
{
	Width = BUFFER_WIDTH;
	Height = BUFFER_HEIGHT;
};
sampler Backup
{
	Texture = BackupTex;
};

texture MaskTex <source=UIMASK_TEXTURE;>
{
	Width = BUFFER_WIDTH;
	Height = BUFFER_HEIGHT;
	Format = TEXFORMAT;
};
sampler Mask
{
	Texture = MaskTex;
};

//#endregion

//#region Shaders

float4 BackupPS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {
	return tex2D(ReShade::BackBuffer, uv);
}

float4 MainPS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {
	float4 color = tex2D(ReShade::BackBuffer, uv);
	float4 backup = tex2D(Backup, uv);

	#if !UIMASK_MULTICHANNEL
		float mask = tex2D(Mask, uv).r;
	#else
		float3 mask_rgb = tex2D(Mask, uv).rgb;

		// This just works, it basically adds masking with each channel that has
		// been toggled.
		float mask = saturate(
			1.0 - dot(1.0 - mask_rgb,
				float3(bToggleRed, bToggleGreen, bToggleBlue)));
	#endif

	color = lerp(color, backup, mask * fMask_Intensity);
	color = bDisplayMask ? mask : color;

	return color;
}

//#endregion

//#region Techniques

technique UIMask_Top
<
	ui_tooltip = "Place this *above* the effects to be masked.";
>
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = BackupPS;
		RenderTarget = BackupTex;
	}
}

technique UIMask_Bottom
<
	ui_tooltip =
		"Place this *below* the effects to be masked.\n"
		"If you want to add a toggle key for the effect, set it to this one.";
>
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = MainPS;
	}
}

//#endregion

} // Namespace.
