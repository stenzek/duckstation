#include "ReShade.fxh"

/*

This shader is not designed to simply simulate the scanline + cross grid effect of old CRT monitors. 
Instead, it aims to combine the advantages of sharp clarity on modern displays with retro games, 
enabling better pixel-level scaling.
The generation intensity of scanlines is dynamically quantized and adjusted based on the human eye's 
perceptual curve for chromatic brightness, rather than using rigid stripe overlay.

Core Features:
- Supports independent adjustment of vertical scanline and horizontal crossline intensity/density, 
  adapting to different resolutions (1080P/4K / high-magnification scaling);
- Default parameters are suitable for most pixel games scaled up on large modern 4K resolution screens, 
  with lossless brightness/color;
- Optimized scanline performance based on human eye brightness sensitivity curve: 
  scanlines are prominent in medium brightness areas and weakened in extreme brightness areas;
- Adjustable color channel quantization attenuation, suitable for games like GBA that have 
  hardware-encoded gamma bias (over-bright/grayed out) due to lack of backlighting. 
  It can perfectly restore vivid colors. 

    Perceptual Sensitivity Curve:
    Sensitivity
      ↑
      |        Peak Sensitivity (0.4-0.6 Luma)
      |           / \
      |          /   \
      |         /     \____ High-luma roll-off
      |        /
      +--------------------→ Luminance (0.0 - 1.0)

	 * (C) 2025-2026 by crashGG.
*/

// --- UI Parameters ---

uniform float sinCompY <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_label = "Vertical Scanline Intensity";
    ui_tooltip = "Intensity of horizontal lines (Y-axis oscillation).";
> = 0.1;

uniform float sinCompX <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 0.10;
    ui_step = 0.01;
    ui_label = "Horizontal Grid Intensity";
    ui_tooltip = "Intensity of vertical lines (X-axis oscillation), creating a shadow mask effect.";
> = 0.01;

uniform float densY <
    ui_type = "drag";
    ui_min = 2.0;
    ui_max = 6.0;
    ui_step = 1.0;
    ui_label = "Scanline Density";
    ui_tooltip = "Frequency of scanlines. Recommended higher for high resolution.";
> = 3.0;

uniform float densX <
    ui_type = "drag";
    ui_min = 0.5;
    ui_max = 1.0;
    ui_step = 0.5;
    ui_label = "Grid Density";
    ui_tooltip = "Density multiplier for the horizontal grid.";
> = 1.0;

uniform float colAtten <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 2.0;
    ui_label = "Chroma Attenuation";
    ui_tooltip = "Adjusts color quantization depth. Useful for correcting excessively bright graphics (e.g., GBA).";
> = 0.0;

#define PI 3.1415926536

// --- Pixel Shader ---

float4 PS_SineScanline(float4 vpos : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_Target
{
    // 1. Texture Sampling
    // Apply a micro-offset (1.0001) to UVs to prevent edge bleeding on certain hardware
    float2 uv = texcoord * 1.0001;
    float3 res = tex2D(ReShade::BackBuffer, uv).rgb;

    // 2. Frequency Calculation (Omega)
    // Map coordinate space to angular frequency. 
    // Uses 1.999 factor on Y-axis to avoid integer-multiple aliasing (Moire patterns).
	float inv_densY = 1.0 / densY;
    float2 omega = PI * float2(BUFFER_WIDTH * densX, BUFFER_HEIGHT * inv_densY * 1.999);

    // 3. Sine Wave Generation
    // Project UVs into periodic sine space for smooth transitions
    float2 tex_omega_product = uv * omega;
    float2 sine_wave = sin(tex_omega_product);
    
    // 4. Amplitude Modulation
    float2 scaled_sine_wave = float2(sinCompX, sinCompY) * sine_wave;
    
    // 5. Signal Summation
    // Combine X and Y oscillations into a single scalar fluctuation value
    float total_sine_fluctuation = scaled_sine_wave.x + scaled_sine_wave.y;

    // 6. Luma-Aware Perceptual Weighting (dist)
    // Calculates squared distance from neutral gray (0.5).
    // This ensures scanlines fade out in pure blacks and pure whites.
    float3 dist_linear = abs(res - 0.5) * 2.0;
    float3 dist = dist_linear * dist_linear;

    // 7. Final Luma Modulation
    // Component A: Chroma attenuation based on grid intensity
    // Component B: Dynamic sine oscillation
    // Both components are gated by the 'weight' (1.0 - dist) for perceptual balance.
    float3 weight = 1.0 - dist;
    float3 final_brightness = 1.0 - ((sinCompX + sinCompY) * weight * colAtten) + total_sine_fluctuation * weight;

    // 8. Output Composition
    float3 scanline = res * final_brightness;

    return float4(scanline, 1.0);
}

// --- Technique Definition ---

technique Modern_Sine_Scanlines 
{
    pass P0 
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_SineScanline;
    }
}
