/*
	SSAO by Constantine 'MadCake' Rudenko
 	
		based on HBAO (Horizon Based Ambient Occlusion)
		https://developer.download.nvidia.com/presentations/2008/SIGGRAPH/HBAO_SIG08b.pdf
		https://developer.nvidia.com/sites/default/files/akamai/gameworks/samples/DeinterleavedTexturing.pdf

	License: https://creativecommons.org/licenses/by/4.0/
	CC BY 4.0
	
	You are free to:

	Share — copy and redistribute the material in any medium or format
		
	Adapt — remix, transform, and build upon the material
	for any purpose, even commercially.

	The licensor cannot revoke these freedoms as long as you follow the license terms.
		
	Under the following terms:

	Attribution — You must give appropriate credit, provide a link to the license, and indicate if changes were made. 
	You may do so in any reasonable manner, but not in any way that suggests the licensor endorses you or your use.

	No additional restrictions — You may not apply legal terms or technological measures 
	that legally restrict others from doing anything the license permits.
*/

#include "ReShadeUI.fxh"

uniform float Strength < __UNIFORM_DRAG_FLOAT1
	ui_min = 0.0; ui_max = 16.0; ui_step = 0.1;
	ui_tooltip = "Strength of the effect (recommended 1.0 to 2.0)";
	ui_label = "Strength";
> = 1.4;

uniform int SampleDistance < __UNIFORM_SLIDER_INT1
	ui_min = 1; ui_max = 64;
	ui_tooltip = "Sampling disk radius (in pixels)";
	ui_label = "Sampling disk radius";
> = 32.0;

uniform int Quality <
	ui_type = "combo";
	ui_label = "Total number of samples";
	ui_tooltip = "Higher numbers yield better quality at the cost of performance";
	ui_items = "Samples: 4\0Samples: 8\0Samples: 16\0Samples: 24\0Samples: 32\0Samples: 36\0Samples: 48\0Samples: 64\0";
> = 1;

uniform float StartFade < __UNIFORM_DRAG_FLOAT1
	ui_min = 0.0; ui_max = 300.0; ui_step = 0.1;
	ui_tooltip = "AO starts fading when Z difference is greater than this,\nmust be smaller than \"Z difference end fade\"";
	ui_label = "Z difference start fade";
> = 0.4;

uniform float EndFade < __UNIFORM_DRAG_FLOAT1
	ui_min = 0.0; ui_max = 300.0; ui_step = 0.1;
	ui_tooltip = "AO completely fades when Z difference is greater than this,\nmust be bigger than \"Z difference start fade\"";
	ui_label = "Z difference end fade";
> = 0.6;

uniform float NormalBias < __UNIFORM_DRAG_FLOAT1
	ui_min = 0.0; ui_max = 1.0; ui_step = 0.025;
	ui_tooltip = "prevents self occlusion (recommended 0.1)";
	ui_label = "Normal bias";
> = 0.1;

uniform int DebugEnabled <
        ui_type = "combo";
        ui_label = "Enable Debug View";
        ui_items = "Disabled\0Blurred\0Before Blur\0";
> = 0;

uniform int Bilateral <
        ui_type = "combo";
        ui_label = "Fake bilateral filter";
        ui_items = "Disabled (cheaper, more blurry)\0Vertical first\0Horizontal first\0";
> = 2;

uniform int BlurRadius < __UNIFORM_SLIDER_INT1
	ui_min = 1.0; ui_max = 32.0;
	ui_tooltip = "Blur radius (in pixels)";
	ui_label = "Blur radius";
> = 3.0;

uniform float BlurQuality < __UNIFORM_DRAG_FLOAT1
		ui_min = 0.5; ui_max = 1.0; ui_step = 0.1;
		ui_label = "Blur Quality";
		ui_tooltip = "Blur quality (recommended 0.6)";
> = 0.6;

uniform float Gamma < __UNIFORM_DRAG_FLOAT1
		ui_min = 1.0; ui_max = 4.0; ui_step = 0.1;
		ui_label = "Gamma";
> = 2.2;

uniform float NormalPower < __UNIFORM_DRAG_FLOAT1
		ui_min = 0.5; ui_max = 8.0; ui_step = 0.1;
		ui_label = "Normal power";
        ui_tooltip = "Acts like softer version of normal bias without a threshold";
> = 1.4;

uniform int FOV < __UNIFORM_DRAG_FLOAT1
		ui_min = 40; ui_max = 180; ui_step = 1.0;
		ui_label = "FOV";
        ui_tooltip = "Leaving it at 90 regardless of your actual FOV provides accetable results";
> = 90;

uniform float DepthShrink < __UNIFORM_DRAG_FLOAT1
		ui_min = 0.0; ui_max = 1.0; ui_step = 0.05;
		ui_label = "Depth shrink";
        ui_tooltip = "Higher values cause AO to become finer on distant objects";
> = 0.65;


// DepthStartFade does not change much visually

/*
uniform float DepthStartFade < __UNIFORM_DRAG_FLOAT1
		ui_min = 0.0; ui_max = 4000.0; ui_step = 1.0;
		ui_label = "Depth start fade";
        ui_tooltip = "Start fading AO at this Z value";
> = 0.0;
*/

uniform float DepthEndFade < __UNIFORM_DRAG_FLOAT1
		ui_min = 0.0; ui_max = 1.0; ui_step = 0.001;
		ui_label = "Depth end fade";
        ui_tooltip = "AO completely fades at this Z value";
> = 1000;


#include "ReShade.fxh"

texture2D InterleavedAOTex  { Width = BUFFER_WIDTH / 2;   Height = BUFFER_HEIGHT / 2;   Format = R8; MipLevels = 1;};
texture2D InterleavedAOTex2 { Width = BUFFER_WIDTH / 2;   Height = BUFFER_HEIGHT / 2;   Format = R8; MipLevels = 1;};
texture2D InterleavedAOTex3 { Width = BUFFER_WIDTH / 2;   Height = BUFFER_HEIGHT / 2;   Format = R8; MipLevels = 1;};
texture2D InterleavedAOTex4 { Width = BUFFER_WIDTH / 2;   Height = BUFFER_HEIGHT / 2;   Format = R8; MipLevels = 1;};

texture2D AOTex	{ Width = BUFFER_WIDTH;   Height = BUFFER_HEIGHT;   Format = R8; MipLevels = 1;};
texture2D AOTex2	{ Width = BUFFER_WIDTH;   Height = BUFFER_HEIGHT;   Format = R8; MipLevels = 1;};
texture2D NormalTex	{ Width = BUFFER_WIDTH;   Height = BUFFER_HEIGHT;   Format = RGBA8; MipLevels = 1;};

sampler2D sAOTex { Texture = AOTex; };
sampler2D sAOTex2 { Texture = AOTex2; };
sampler2D sInterleavedAOTex { Texture = InterleavedAOTex; };
sampler2D sInterleavedAOTex2 { Texture = InterleavedAOTex2; };
sampler2D sInterleavedAOTex3 { Texture = InterleavedAOTex3; };
sampler2D sInterleavedAOTex4 { Texture = InterleavedAOTex4; };
sampler2D sNormalTex { Texture = NormalTex; };

float GetTrueDepth(float2 coords)
{
	return ReShade::GetLinearizedDepth(coords) * RESHADE_DEPTH_LINEARIZATION_FAR_PLANE;
}

float3 GetPosition(float2 coords)
{
	float2 fov;
	fov.x = FOV / 180.0 * 3.1415;
	fov.y = fov.x / BUFFER_ASPECT_RATIO; 
	float3 pos;
	pos.z = GetTrueDepth(coords.xy);
	coords.y = 1.0 - coords.y;
	pos.xy = coords.xy * 2.0 - 1.0;
	float2 h;
	h.x	= 1.0 / tan(fov.x * 0.5);
	h.y = 1.0 / tan(fov.y * 0.5);
	pos.xy /= h / pos.z;
	return pos;
}

float4 GetNormalFromDepth(float2 coords) 
{
	float3 centerPos = GetPosition(coords);
	
	float2 offx = float2(BUFFER_PIXEL_SIZE.x, 0);
	float2 offy = float2(0, BUFFER_PIXEL_SIZE.y);
	
	float3 ddx1 = GetPosition(coords + offx) - centerPos;
	float3 ddx2 = centerPos - GetPosition(coords - offx);

	float3 ddy1 = GetPosition(coords + offy) - centerPos;
	float3 ddy2 = centerPos - GetPosition(coords - offy);
	
	ddx1 = ddx1 + ddx2;
	ddy1 = ddy1 + ddy2;

	float4 normal;
	normal.xyz = normalize(cross(ddx1, ddy1));
	normal.w = centerPos.z;
	
	return normal;
}

float4 GetNormalFromTexture(float2 coords)
{
	float4 normal = tex2D(sNormalTex, coords);
	normal.xyz = normal.xyz * 2.0 - float3(1,1,1);
	return normal;
}

float4 DepthNormalsPass(float4 vpos : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	float4 normal = GetNormalFromDepth(texcoord);
	normal.xyz = normal.xyz * 0.5 + 0.5;
	return normal;
}

float rand2D(float2 uv){
	uv = frac(uv);
	float x = frac(cos(uv.x*64)*256);
	float y = frac(cos(uv.y*137)*241);
	float z = x+y;
	return frac(cos((z)*107)*269);
}

float BlurAOFirstPass(float4 vpos : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	int debugEnabled = clamp(DebugEnabled, 0, 2);
	
	[branch]
	if (debugEnabled == 2)
	{
		return tex2D(sAOTex, texcoord).r;
	}

	float normal_bias = clamp(NormalBias, 0.0, 1.0);
	int bilateral = clamp(Bilateral, 0, 2);

	float range = clamp(BlurRadius, 1, 32);
	float fade_range = EndFade - StartFade;

	float tmp = 1.0 / (range * range);
	float gauss = 1.0;
	float helper = exp(tmp * 0.5);
	float helper2 = exp(tmp);
	float sum = tex2D(sAOTex, texcoord).r;
	float sumCoef = 1.0;
	
	float blurQuality = clamp(BlurQuality, 0.0, 1.0);
	range *= 3.0 * blurQuality;
	
	float2 off = float2(BUFFER_PIXEL_SIZE.x, 0);
	
	float4 my_normal;
	
	[branch]
	if (bilateral)
	{
		my_normal = GetNormalFromTexture(texcoord);
	}
	
	[branch]
	if (Bilateral == 2)
	{
		off = float2(0, BUFFER_PIXEL_SIZE.y);
	}
	
	float weights[32];
	weights[0] = 1;
	
	[unroll]
	for (int i = 1; i < 32; i++)
	{
		gauss = gauss / helper;
		helper = helper * helper2;
		weights[i] = gauss;
	}
	
	[loop]
	for(int k = 1; k < range; k++)
	{
		float weight = weights[abs(k)];
		
		[branch]
		if (bilateral)
		{
			float4 normal = GetNormalFromTexture(texcoord + off * k);
			float dotWeight = saturate((dot(my_normal.xyz, normal.xyz) - normal_bias) / (1.0 - normal_bias));
			normal *= pow(dotWeight, NormalPower);
			float zdiff = abs(my_normal.w - normal.w);
			[flatten]
			if (zdiff >= StartFade)
			{
				float fade = saturate(1.0 - (zdiff - StartFade) / fade_range);
				weight *= fade * fade;
			}
			sum += tex2D(sAOTex, texcoord + off * k).r * weight;
			sumCoef += weight;
		}
		else
		{
			sum += tex2D(sAOTex, texcoord + off * k).r * weight;
			sumCoef += weight;
		}
	}
	
	[loop]
	for(int k = 1; k < range; k++)
	{
		float weight = weights[abs(k)];
		
		[branch]
		if (bilateral)
		{
			float4 normal = GetNormalFromTexture(texcoord - off * k);
			float dotWeight = saturate((dot(my_normal.xyz, normal.xyz) - normal_bias) / (1.0 - normal_bias));
			normal *= pow(dotWeight, NormalPower);
			float zdiff = abs(my_normal.w - normal.w);
			[flatten]
			if (zdiff >= StartFade)
			{
				float fade = saturate(1.0 - (zdiff - StartFade) / fade_range);
				weight *= fade * fade;
			}
			sum += tex2D(sAOTex, texcoord - off * k).r * weight;
			sumCoef += weight;
		}
		else
		{
			sum += tex2D(sAOTex, texcoord - off * k).r * weight;
			sumCoef += weight;
		}
	}
	
	return sum / sumCoef;
}


float3 BlurAOSecondPass(float4 vpos : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	int debugEnabled = clamp(DebugEnabled, 0, 2);
	
	[branch]
	if (debugEnabled == 2)
	{
		return tex2D(sAOTex2, texcoord).r;
	}
	
	int bilateral = clamp(Bilateral, 0, 2);
	float normal_bias = clamp(NormalBias, 0.0, 1.0);

	float range = clamp(BlurRadius, 1, 32);
	float fade_range = EndFade - StartFade;

	float tmp = 1.0 / (range * range);
	float gauss = 1.0;
	float helper = exp(tmp * 0.5);
	float helper2 = exp(tmp);
	float sum = tex2D(sAOTex, texcoord).r;
	float sumCoef = 1.0;
	
	float blurQuality = clamp(BlurQuality, 0.0, 1.0);
	range *= 3.0 * blurQuality;
	
	float2 off = float2(0, BUFFER_PIXEL_SIZE.y);
	
	float4 my_normal;
	[branch]
	if (bilateral)
	{
		my_normal = GetNormalFromTexture(texcoord);
	}
	
	[branch]
	if (Bilateral == 2)
	{
		off = float2(BUFFER_PIXEL_SIZE.x, 0);
	}
	
	float weights[32];
	weights[0] = 1;
	
	[unroll]
	for (int i = 1; i < 32; i++)
	{
		gauss = gauss / helper;
		helper = helper * helper2;
		weights[i] = gauss;
	}
	
	[loop]
	for(int k = 1; k < range; k++)
	{
		float weight = weights[abs(k)];
		
		[branch]
		if (bilateral)
		{
			float4 normal = GetNormalFromTexture(texcoord + off * k);
			weight *= max(0.0, dot(my_normal.xyz, normal.xyz));
			float zdiff = abs(my_normal.w - normal.w);
			zdiff *= zdiff;
			[flatten]
			if (zdiff >= StartFade)
			{
				float fade = saturate(1.0 - (zdiff - StartFade) / fade_range);
				weight *= fade * fade;
			}
			sum += tex2D(sAOTex2, texcoord + off * k).r * weight;
			sumCoef += weight;
		}
		else
		{
			sum += tex2D(sAOTex2, texcoord + off * k).r * weight;
			sumCoef += weight;
		}
	}
	
	[loop]
	for(int k = 1; k < range; k++)
	{
		float weight = weights[abs(k)];
		
		[branch]
		if (bilateral)
		{
			float4 normal = GetNormalFromTexture(texcoord - off * k);
			weight *= saturate((dot(my_normal.xyz, normal.xyz) - normal_bias) / (1.0 - normal_bias));
			float zdiff = abs(my_normal.w - normal.w);
			if (zdiff >= StartFade)
			{
				float fade = saturate(1.0 - (zdiff - StartFade) / fade_range);
				weight *= fade * fade;
			}
			sum += tex2D(sAOTex2, texcoord - off * k).r * weight;
			sumCoef += weight;
		}
		else
		{
			sum += tex2D(sAOTex2, texcoord - off * k).r * weight;
			sumCoef += weight;
		}
	}
	
	[branch]
	if (debugEnabled)
	{
		return sum / sumCoef;
	}
	else
	{
			float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
		return color * sum / sumCoef;
	}
}

float2 ensure_1px_offset(float2 ray)
{
	float2 ray_in_pixels = ray / BUFFER_PIXEL_SIZE;
	float coef = max(abs(ray_in_pixels.x), abs(ray_in_pixels.y));
	[flatten]
	if (coef < 1.0)
	{
		ray /= coef;
	}
	return ray;
}

float3 MergeAOPass(float4 vpos : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	uint2 texcoord_px = texcoord / BUFFER_PIXEL_SIZE;
	
	/*
	if (texcoord_px.x % 2 && texcoord_px.y % 2)
		return 0;
	if (texcoord_px.x % 2)
		return float3(1,0,0);
	if (texcoord_px.y % 2)
		return float3(0,1,0);
	return float3(0,0,1);*/
	
	if (texcoord_px.x % 2 && texcoord_px.y % 2)
		return tex2D(sInterleavedAOTex4, texcoord).r;
	if (texcoord_px.x % 2)
		return tex2D(sInterleavedAOTex2, texcoord).r;
	if (texcoord_px.y % 2)
		return tex2D(sInterleavedAOTex3, texcoord).r;
	return tex2D(sInterleavedAOTex, texcoord).r;
}

float GetOcclusion(float2 texcoord, float angle_jitter)
{
	float3 position = GetPosition(texcoord);
	float3 normal = GetNormalFromTexture(texcoord).xyz;
	
	int quality = clamp(Quality, 0, 7);
	
	int num_angle_samples;
	int num_distance_samples;
	
	[branch]
	switch(quality)
	{
		case 0:
			num_angle_samples = 4;
			num_distance_samples = 1;
			break;
		case 1:
			num_angle_samples = 4;
			num_distance_samples = 2;
			break;
		case 2:
			num_angle_samples = 4;
			num_distance_samples = 4;
			break;
		case 3:
			num_angle_samples = 6;
			num_distance_samples = 4;
			break;
		case 4:
			num_angle_samples = 8;
			num_distance_samples = 4;
			break;
		case 5:
			num_angle_samples = 6;
			num_distance_samples = 6;
			break;
		case 6:
			num_angle_samples = 8;
			num_distance_samples = 6;
			break;
		case 7:
			num_angle_samples = 8;
			num_distance_samples = 8;
			break;
	}
	
	int sample_dist = clamp(SampleDistance, 1, 128);
	float normal_bias = clamp(NormalBias, 0.0, 1.0);
	
	float occlusion = 0.0;
	float fade_range = EndFade - StartFade;

	[loop]
	for (int i = 0; i < num_angle_samples; i++)
	{
		float angle = 3.1415 * 2.0 / num_angle_samples * (i + angle_jitter);
		
		float2 ray;
		ray.x = sin(angle);
		ray.y = cos(angle);
		ray *= BUFFER_PIXEL_SIZE * sample_dist;
		ray /= 1.0 + position.z * lerp(0, 0.05, pow(DepthShrink,4));
		
		float angle_occlusion = 0.0;
		
		[loop]
		for (int k = 0; k < num_distance_samples; k++)
		{
			float radius_jitter = rand2D(texcoord + float2(i, 0));
			float radius_coef = (k + 1.0 - radius_jitter) / num_distance_samples;
			radius_coef = max(radius_coef, 0.01);
			float2 scaled_ray = ensure_1px_offset(ray * radius_coef);
			float2 sample_coord = texcoord + scaled_ray;
			float3 sampled_position = GetPosition(sample_coord);
			float3 v = sampled_position - position;
			float ray_occlusion = dot(normal.xyz, normalize(v));
			ray_occlusion = max(ray_occlusion, 0.0);	// not just warning suppression, leave it be!
			ray_occlusion = pow (ray_occlusion, NormalPower);
			ray_occlusion = saturate((ray_occlusion - normal_bias) / (1.0 - normal_bias));
			float zdiff = abs(v.z);
			[flatten]
			if (zdiff >= StartFade)
			{
				float fade = saturate(1.0 - (zdiff - StartFade) / fade_range);
				ray_occlusion *= fade * fade;
			}

			float fade_coef = float(num_distance_samples - k) / num_distance_samples;
			fade_coef *= fade_coef;
			angle_occlusion = max(angle_occlusion, ray_occlusion * fade_coef);
		}
		
		occlusion += angle_occlusion;
	}
	occlusion /=  num_angle_samples * 2.0;
	occlusion *= saturate(1.0 - (position.z / DepthEndFade / RESHADE_DEPTH_LINEARIZATION_FAR_PLANE));
	occlusion = saturate(1.0 - occlusion * Strength);
	occlusion = pow(occlusion, Gamma);
	
	
	return occlusion;
}

void SSAOPass(float4 vpos : SV_Position, in float2 texcoord : TexCoord,
				out float occlusion_1 : SV_Target0, out float occlusion_2 : SV_Target1, out float occlusion_3 : SV_Target2, out float occlusion_4 : SV_Target3)
{
	occlusion_1 = GetOcclusion(texcoord + float2(-0.25,  0.25) * BUFFER_PIXEL_SIZE,  0.00);
	occlusion_2 = GetOcclusion(texcoord + float2( 0.25,  0.25) * BUFFER_PIXEL_SIZE,  0.25);
	occlusion_3 = GetOcclusion(texcoord + float2(-0.25, -0.25) * BUFFER_PIXEL_SIZE,  0.50);
	occlusion_4 = GetOcclusion(texcoord + float2( 0.25, -0.25) * BUFFER_PIXEL_SIZE,  0.75);
}

technique MC_DAO
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = DepthNormalsPass;
		RenderTarget0 = NormalTex;
	}
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = SSAOPass;
		RenderTarget0 = InterleavedAOTex;
		RenderTarget1 = InterleavedAOTex2;
		RenderTarget2 = InterleavedAOTex3;
		RenderTarget3 = InterleavedAOTex4;
	}
	pass 
	{
		VertexShader = PostProcessVS;
		PixelShader = MergeAOPass;
		RenderTarget0 = AOTex;
	}
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = BlurAOFirstPass;
		RenderTarget0 = AOTex2;
	}
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = BlurAOSecondPass;
	}
}
