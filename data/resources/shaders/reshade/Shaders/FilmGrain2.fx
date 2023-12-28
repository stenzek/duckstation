/**
 * Film Grain post-process shader v1.1
 * Martins Upitis (martinsh) devlog-martinsh.blogspot.com 2013
 *
 * This work is licensed under a Creative Commons Attribution 3.0 Unported License.
 * So you are free to share, modify and adapt it for your needs, and even use it for commercial use.
 *
 * Uses perlin noise shader by toneburst from http://machinesdontcare.wordpress.com/2009/06/25/3d-perlin-noise-sphere-vertex-shader-sourcecode/
 */

#include "ReShadeUI.fxh"

uniform float grainamount < __UNIFORM_SLIDER_FLOAT1
	ui_min = 0.0; ui_max = 0.2;
	ui_label = "Amount";
> = 0.05;
uniform float coloramount < __UNIFORM_SLIDER_FLOAT1
	ui_min = 0.0; ui_max = 1.0;
	ui_label = "Color Amount";
> = 0.6;
uniform float lumamount < __UNIFORM_SLIDER_FLOAT1
	ui_min = 0.0; ui_max = 1.0;
	ui_label = "Luminance Amount";
> = 1.0;

uniform float grainsize < __UNIFORM_SLIDER_FLOAT1
	ui_min = 1.5; ui_max = 2.5;
	ui_label = "Grain Particle Size";
> = 1.6;

#include "ReShade.fxh"

uniform float timer < source = "timer"; >;

float4 rnm(in float2 tc)
{
	// A random texture generator, but you can also use a pre-computed perturbation texture
	float noise = sin(dot(tc, float2(12.9898, 78.233))) * 43758.5453;

	float noiseR = frac(noise) * 2.0 - 1.0;
	float noiseG = frac(noise * 1.2154) * 2.0 - 1.0;
	float noiseB = frac(noise * 1.3453) * 2.0 - 1.0;
	float noiseA = frac(noise * 1.3647) * 2.0 - 1.0;

	return float4(noiseR, noiseG, noiseB, noiseA);
}
float pnoise3D(in float3 p)
{
	// Perm texture texel-size
	static const float permTexUnit = 1.0 / 256.0;
	// Half perm texture texel-size
	static const float permTexUnitHalf = 0.5 / 256.0;

	// Integer part
	// Scaled so +1 moves permTexUnit texel and offset 1/2 texel to sample texel centers
	float3 pi = permTexUnit * floor(p) + permTexUnitHalf;
	// Fractional part for interpolation
	float3 pf = frac(p);

	// Noise contributions from (x=0, y=0), z=0 and z=1
	float perm00 = rnm(pi.xy).a;
	float3 grad000 = rnm(float2(perm00, pi.z)).rgb * 4.0 - 1.0;
	float n000 = dot(grad000, pf);
	float3 grad001 = rnm(float2(perm00, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
	float n001 = dot(grad001, pf - float3(0.0, 0.0, 1.0));

	// Noise contributions from (x=0, y=1), z=0 and z=1
	float perm01 = rnm(pi.xy + float2(0.0, permTexUnit)).a;
	float3 grad010 = rnm(float2(perm01, pi.z)).rgb * 4.0 - 1.0;
	float n010 = dot(grad010, pf - float3(0.0, 1.0, 0.0));
	float3 grad011 = rnm(float2(perm01, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
	float n011 = dot(grad011, pf - float3(0.0, 1.0, 1.0));

	// Noise contributions from (x=1, y=0), z=0 and z=1
	float perm10 = rnm(pi.xy + float2(permTexUnit, 0.0)).a;
	float3 grad100 = rnm(float2(perm10, pi.z)).rgb * 4.0 - 1.0;
	float n100 = dot(grad100, pf - float3(1.0, 0.0, 0.0));
	float3 grad101 = rnm(float2(perm10, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
	float n101 = dot(grad101, pf - float3(1.0, 0.0, 1.0));

	// Noise contributions from (x=1, y=1), z=0 and z=1
	float perm11 = rnm(pi.xy + float2(permTexUnit, permTexUnit)).a;
	float3 grad110 = rnm(float2(perm11, pi.z)).rgb * 4.0 - 1.0;
	float n110 = dot(grad110, pf - float3(1.0, 1.0, 0.0));
	float3 grad111 = rnm(float2(perm11, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
	float n111 = dot(grad111, pf - float3(1.0, 1.0, 1.0));

	// Blend contributions along x
	float fade_x = pf.x * pf.x * pf.x * (pf.x * (pf.x * 6.0 - 15.0) + 10.0);
	float4 n_x = lerp(float4(n000, n001, n010, n011), float4(n100, n101, n110, n111), fade_x);

	// Blend contributions along y
	float fade_y = pf.y * pf.y * pf.y * (pf.y * (pf.y * 6.0 - 15.0) + 10.0);
	float2 n_xy = lerp(n_x.xy, n_x.zw, fade_y);

	// Blend contributions along z
	float fade_z = pf.z * pf.z * pf.z * (pf.z * (pf.z * 6.0 - 15.0) + 10.0);
	float n_xyz = lerp(n_xy.x, n_xy.y, fade_z);

	// We're done, return the final noise value.
	return n_xyz;
}

float2 coordRot(in float2 tc, in float angle)
{
	float rotX = ((tc.x * 2.0 - 1.0) * BUFFER_ASPECT_RATIO * cos(angle)) - ((tc.y * 2.0 - 1.0) * sin(angle));
	float rotY = ((tc.y * 2.0 - 1.0) * cos(angle)) + ((tc.x * 2.0 - 1.0) * BUFFER_ASPECT_RATIO * sin(angle));
	rotX = ((rotX / BUFFER_ASPECT_RATIO) * 0.5 + 0.5);
	rotY = rotY * 0.5 + 0.5;

	return float2(rotX, rotY);
}

float4 main(float4 vpos : SV_Position, float2 texCoord : TexCoord) : SV_Target
{
	float3 rotOffset = float3(1.425, 3.892, 5.835); // Rotation offset values	
	float2 rotCoordsR = coordRot(texCoord, timer + rotOffset.x);
	float3 noise = pnoise3D(float3(rotCoordsR * BUFFER_SCREEN_SIZE / grainsize, 0.0)).xxx;

	if (coloramount > 0)
	{
		float2 rotCoordsG = coordRot(texCoord, timer + rotOffset.y);
		float2 rotCoordsB = coordRot(texCoord, timer + rotOffset.z);
		noise.g = lerp(noise.r, pnoise3D(float3(rotCoordsG * BUFFER_SCREEN_SIZE / grainsize, 1.0)), coloramount);
		noise.b = lerp(noise.r, pnoise3D(float3(rotCoordsB * BUFFER_SCREEN_SIZE / grainsize, 2.0)), coloramount);
	}

	float3 col = tex2D(ReShade::BackBuffer, texCoord).rgb;

	const float3 lumcoeff = float3(0.299, 0.587, 0.114);
	float luminance = lerp(0.0, dot(col, lumcoeff), lumamount);
	float lum = smoothstep(0.2, 0.0, luminance);
	lum += luminance;


	noise = lerp(noise, 0.0, pow(lum, 4.0));
	col = col + noise * grainamount;

	return float4(col, 1.0);
}

technique FilmGrain2
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = main;
	}
}
