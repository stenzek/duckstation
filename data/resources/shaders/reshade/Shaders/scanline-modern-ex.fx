#include "ReShade.fxh"

/*

This shader is not designed to simply simulate the scanline + aperture grille effect of old CRT monitors. 
Instead, it aims to combine the advantages of sharp clarity on modern displays with retro games, 
enabling better pixel-level scaling.
The generation intensity of scanlines is dynamically quantized and adjusted based on the human eye's 
perceptual curve for chromatic brightness, rather than using rigid stripe overlay.

Core Features:
- Supports independent adjustment of vertical scanline and horizontal aperture grille intensity/density, 
  adapting to different resolutions (1080P/4K / high-magnification scaling);
- Default parameters are suitable for most pixel games scaled up on modern resolution screens, 
  with lossless brightness/color;
- Optimized scanline performance based on human eye brightness sensitivity curve: 
  scanlines are prominent in medium brightness areas and weakened in extreme brightness areas;

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
    ui_min = 0.0; ui_max = 0.50; ui_step = 0.01;
    ui_label = "Scanline Intensity (Vertical)";
    ui_category = "Scanline Settings";
> = 0.10;

uniform float CompXlevl <
    ui_type = "drag";
    ui_min = 0.0; ui_max = 20.0; ui_step = 0.5;
    ui_label = "Aperture Grille Level (Horizontal)";
    ui_category = "Scanline Settings";
> = 3.0;

uniform float densY <
    ui_type = "drag";
    ui_min = 2.0; ui_max = 10.0; ui_step = 0.5;
    ui_label = "Scanline Period (Vertical Pixels)";
    ui_tooltip = "Number of physical screen pixels per full scanline cycle. Integer values yield better results.";
    ui_category = "Scanline Settings";
> = 4.0;

uniform float densX <
    ui_type = "drag";
    ui_min = 2.0; ui_max = 10.0; ui_step = 0.5;
    ui_label = "Aperture Grille Period (Horizontal Pixels)";
    ui_tooltip = "Number of physical screen pixels per full aperture grille cycle. Integer values yield better results.";
    ui_category = "Scanline Settings";
> = 2.0;

static const float PI = 3.1415926536;

// --- Structs ---
struct v2f {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float2 omega : TEXCOORD1;
};

// --- Vertex Shader ---
v2f VS_Scanline(uint id : SV_VertexID) {
    v2f o;
    // Standard full-screen triangle logic for ReShade.fxh
    o.uv.x = (id == 2) ? 2.0 : 0.0;
    o.uv.y = (id == 1) ? 2.0 : 0.0;
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);


    // Compute angular frequency to control sine wave periods.
    // Approach: Use exact multi-integer frequencies to guarantee perfect alignment between scanlines and the native pixel grid.
    // Logic: Angular Frequency = 2 * PI * (Screen Resolution / Target Pixel Period)

    o.omega = PI * 2.0 * float2(BUFFER_WIDTH, BUFFER_HEIGHT) / float2(densX, densY);

    return o;
}

// --- Pixel Shader ---
float4 PS_Scanline(v2f i) : SV_Target {

    // Granularity step of 0.005 per level
    float sinCompX = CompXlevl * 0.005;

    // Center point tap
    float3 texel = tex2D(ReShade::BackBuffer, i.uv).rgb;

    // Signal shaping: Map UV coordinates to sine wave phase
    float2 tex_omega_product = i.uv * i.omega;

	// Calculate horizontal and vertical sine wave oscillations to generate periodic brightness variations in [-1.0, 1.0].
	// Applies a -0.5 * PI (90-degree) phase shift to anchor the wave troughs (-1.0) exactly at pixel boundaries.

	// Note: Adjusts phase when density is 2.0 to prevent scanline cancellation caused by Nyquist spatial sampling dead zones.
    float2 sine_wave = sin(tex_omega_product - 0.5 * PI * float2(densX>2.0,densY>2.0));

	// 1. Modulate sine wave intensity via component-wise multiplication.
	// 2. Linearly blend horiz/vert sine waves to synthesize the final scalar gain for the luminance oscillation.
    float total_sine_fluctuation = (sinCompX * sine_wave.x) + (sinCompY * sine_wave.y);

    // Core dynamic quantization logic: distance from the mid-gray value (0.5) per channel, yielding vec3 [0.0 - 1.0].
    // 'dist' approaches 0 near mid-tones (0.5) where attenuation is maximum; oscillation dampens at extreme brightness levels.
    float3 dist = abs(texel - 0.5) * 2.0;
    
	// Composite final scanline lighting effects:
	// Modulate base brightness (1.0) with scanline oscillation, then multiply back into the original texel.
    float3 final_brightness = 1.0 + total_sine_fluctuation * (1.0 - dist);

    float3 scanline = texel * final_brightness;

    return float4(scanline, 1.0);
}

// --- Techniques ---
technique Scanline_Modern {
    pass {
        VertexShader = VS_Scanline;
        PixelShader = PS_Scanline;
    }
}
