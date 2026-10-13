/*
    Black Frame Insertion
    Displays the image on one subframe, and black on the others.
    Requires a display refresh rate that is a multiple of the content's, e.g. 120hz for 60hz content, and VSync and
    Sync To Host Refresh Rate to be enabled. Otherwise subframes won't line up with refreshes, and you'll get tearing,
    flickering, or lag.
    license: public domain
*/

#include "ReShade.fxh"

uniform int LitSubFrame <
	ui_type = "slider";
	ui_min = 1;
	ui_max = 8;
	ui_step = 1;
	ui_label = "Lit Subframe";
	ui_text = "Requires a display refresh rate that is a multiple of the game's, e.g. 120hz for 60hz games, and VSync "
	          "and to be enabled, and Sync To Host Refresh Rate or a GSync/FreeSync display. Otherwise you will get tearing, "
	          "flickering, or lag. Subframes should be left on Automatic.";
	ui_tooltip = "Selects which subframe displays the image. Clamped to the number of subframes.";
> = 1;

uniform int CurrentSubFrame < source = "current_subframe"; >;
uniform int TotalSubFrames < source = "total_subframes"; >;

float4 PS_BlackFrameInsertion(float4 pos : SV_Position, float2 tex : TEXCOORD) : SV_Target
{
	float3 color = tex2D(ReShade::BackBuffer, tex).rgb;
	if (CurrentSubFrame != min(LitSubFrame, TotalSubFrames))
		color = float3(0.0, 0.0, 0.0);

	return float4(color, 1.0);
}

technique BlackFrameInsertion {
	pass BlackFrameInsertion {
		VertexShader = PostProcessVS;
		PixelShader = PS_BlackFrameInsertion;
	}
}
