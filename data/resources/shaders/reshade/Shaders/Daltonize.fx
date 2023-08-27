/**
 * Daltonization algorithm by daltonize.org
 * http://www.daltonize.org/2010/05/lms-daltonization-algorithm.html
 * Originally ported to ReShade by IDDQD, modified for ReShade 3.0 by crosire
 */

uniform int Type <
	ui_type = "combo";
	ui_items = "Protanopia\0Deuteranopia\0Tritanopia\0";
> = 0;

#include "ReShade.fxh"

float3 PS_DaltonizeFXmain(float4 vpos : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	float3 input = tex2D(ReShade::BackBuffer, texcoord).rgb;

	// RGB to LMS matrix conversion
	float OnizeL = (17.8824f * input.r) + (43.5161f * input.g) + (4.11935f * input.b);
	float OnizeM = (3.45565f * input.r) + (27.1554f * input.g) + (3.86714f * input.b);
	float OnizeS = (0.0299566f * input.r) + (0.184309f * input.g) + (1.46709f * input.b);
	
	// Simulate color blindness
	float Daltl, Daltm, Dalts;

	if (Type == 0) // Protanopia - reds are greatly reduced (1% men)
	{
		Daltl = 0.0f * OnizeL + 2.02344f * OnizeM + -2.52581f * OnizeS;
		Daltm = 0.0f * OnizeL + 1.0f * OnizeM + 0.0f * OnizeS;
		Dalts = 0.0f * OnizeL + 0.0f * OnizeM + 1.0f * OnizeS;
	}
	else if (Type == 1) // Deuteranopia - greens are greatly reduced (1% men)
	{
		Daltl = 1.0f * OnizeL + 0.0f * OnizeM + 0.0f * OnizeS;
		Daltm = 0.494207f * OnizeL + 0.0f * OnizeM + 1.24827f * OnizeS;
		Dalts = 0.0f * OnizeL + 0.0f * OnizeM + 1.0f * OnizeS;
	}
	else if (Type == 2) // Tritanopia - blues are greatly reduced (0.003% population)
	{
		Daltl = 1.0f * OnizeL + 0.0f * OnizeM + 0.0f * OnizeS;
		Daltm = 0.0f * OnizeL + 1.0f * OnizeM + 0.0f * OnizeS;
		Dalts = -0.395913f * OnizeL + 0.801109f * OnizeM + 0.0f * OnizeS;
	}
	
	// LMS to RGB matrix conversion
	float3 error;
	error.r = (0.0809444479f * Daltl) + (-0.130504409f * Daltm) + (0.116721066f * Dalts);
	error.g = (-0.0102485335f * Daltl) + (0.0540193266f * Daltm) + (-0.113614708f * Dalts);
	error.b = (-0.000365296938f * Daltl) + (-0.00412161469f * Daltm) + (0.693511405f * Dalts);
	
	// Isolate invisible colors to color vision deficiency (calculate error matrix)
	error = (input - error);
	
	// Shift colors towards visible spectrum (apply error modifications)
	float3 correction;
	correction.r = 0; // (error.r * 0.0) + (error.g * 0.0) + (error.b * 0.0);
	correction.g = (error.r * 0.7) + (error.g * 1.0); // + (error.b * 0.0);
	correction.b = (error.r * 0.7) + (error.b * 1.0); // + (error.g * 0.0);
	
	// Add compensation to original values
	correction = input + correction;
	
	return correction;
}

technique Daltonize
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_DaltonizeFXmain;
	}
}
