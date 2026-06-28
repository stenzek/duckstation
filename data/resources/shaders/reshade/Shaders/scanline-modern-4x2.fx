/*
================================================================================
Scanline Modern 4x2

This shader is not designed to simply simulate the scanline + aperture grille 
effect of old CRT monitors. Instead, it aims to combine the advantages of sharp 
clarity on modern displays with retro games, enabling better pixel-level scaling.

The generation intensity of scanlines is dynamically quantized and adjusted based 
on the human eye's perceptual curve for chromatic brightness, rather than using 
rigid stripe overlay.

Core Features:
- Independent control over vertical scanline and horizontal aperture grille 
  intensity.
- Fixed vertical scanline period to 4 pixels and horizontal mask to 2 pixels 
  to establish a native physical grid on modern 2K/4K displays. This efficiently 
  masks and embellishes the glaring grid unevenness caused by non-integer scaling 
  when rendering high-res PSX low-poly/pixel titles.
- Leverages sub-pixel edge detection to smoothly transition and camouflage 
  misalignment artifacts where the virtual game pixels fail to align 
  pixel-to-pixel with the modern LCD hardware.
- Optimized scanline performance based on human eye brightness sensitivity curve: 
  scanlines are prominent in medium brightness areas and weakened in extreme 
  brightness areas.

(C) 2025-2026 by crashGG.
================================================================================
*/


#include "ReShade.fxh"

// --- UI Parameters ---
uniform float compY <
    ui_type = "drag";
    ui_min = 0.0; ui_max = 0.40; ui_step = 0.005;
    ui_label = "Scanline Intensity (Vertical)";
    ui_category = "Scanline Settings";
> = 0.075;

uniform float compX <
    ui_type = "drag";
    ui_min = 0.0; ui_max = 0.10; ui_step = 0.005;
    ui_label = "Aperture Grille Intensity (Horizontal)";
    ui_category = "Scanline Settings";
> = 0.02;

// --- Pixel Shader ---
float4 PS_SCANLINE4x2(float4 vpos : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_Target
{

    float2 texYplus1 = texcoord + float2(0.0, BUFFER_PIXEL_SIZE.y);
 
    // 5-tap sampling: center plus down, up, left, right neighboring pixels
    float3 texel = tex2D(ReShade::BackBuffer, texcoord).rgb;
    float3 tex_D = tex2D(ReShade::BackBuffer, texYplus1).rgb;
    float3 tex_U = tex2D(ReShade::BackBuffer, texcoord + float2(0.0, -BUFFER_PIXEL_SIZE.y)).rgb;
    float3 tex_L = tex2D(ReShade::BackBuffer, texcoord + float2(-BUFFER_PIXEL_SIZE.x, 0.0)).rgb;
    float3 tex_R = tex2D(ReShade::BackBuffer, texcoord + float2(BUFFER_PIXEL_SIZE.x, 0.0)).rgb;

    // Calculate luminance (r+g+b) and normalize to 1.0
    float lumaC = dot(texel, 0.3333333);
    float lumaD = dot(tex_D, 0.3333333);
    float lumaU = dot(tex_U, 0.3333333);
    float lumaL = dot(tex_L, 0.3333333);
    float lumaR = dot(tex_R, 0.3333333);

    // Square wave generator: 1-pixel phase shift on Y-axis to form a -1, +1, +1, -1 pattern
    float2 freq = float2(0.5, 0.25) * float2(BUFFER_WIDTH, BUFFER_HEIGHT);
    float2 phase = frac(texYplus1 * freq +0.01); // +0.01 to avoid precision drift at boundaries
    float2 wave = step(0.5, phase) * 2.0 - 1.0;

	//	Scanline edge detection
	float diffU = (lumaC - lumaU) * sign(wave.y);
	float diffD = (lumaC - lumaD) * sign(wave.y);
	float diffL = (lumaC - lumaL) * sign(wave.x);
	float diffR = (lumaC - lumaR) * sign(wave.x);

    // Modulate square wave amplitude via component-wise multiplication
	// During the dark cycle, the center pixel cannot be darker than its sides (by compXY threshold); 
	// during the light cycle, the center pixel cannot be brighter than its sides (by compXY threshold). 
	// Otherwise, fallback to the original pixel.
    float modY = compY * wave.y * (diffU < compY && diffD < compY);
    float modX = compX * wave.x * (diffL < compX && diffR < compX);


    // Dynamic quantization core logic: distance from each of the RGB channels to the median value 0.5 (vec3 [0.0 - 1.0])
    // The wave perturbation peaks around mid-tones (0.5) where dist approaches 0, and dampens at extreme brightness levels.
    float3 dist = abs(texel - 0.5) * 2.0;
	// Combined scanline lighting pass synthesis:
	// Overlay scanline oscillation onto the baseline brightness (1.0), then multiply back by the source texture.
    float3 final_brightness = 1.0 + (modX + modY) * (1.0 - dist);

    return float4(texel * final_brightness, 1.0);
}

// --- Techniques ---
technique SCANLINE_MODERN_4x2 {
    pass SCANLINE4x2{
        VertexShader = PostProcessVS;
        PixelShader = PS_SCANLINE4x2;
    }
}
