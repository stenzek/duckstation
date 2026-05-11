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
- For PS1 games:Set to match the original internal resolution for pixel-perfect scanline alignment.

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

// --- UI Uniforms ---
uniform float oriVert <
    ui_type = "drag";
    ui_min = 192.0; ui_max = 288.0; ui_step = 8.0;
    ui_label = "Source Vertical Resolution";
    ui_tooltip = "Set to match the original internal resolution for pixel-perfect scanline alignment.";
    ui_category = "Scanline Settings";
> = 240.0;

uniform float sinCompY <
    ui_type = "drag";
    ui_min = 0.0; ui_max = 0.50; ui_step = 0.01;
    ui_label = "Scanline Intensity (Vertical)";
    ui_category = "Scanline Settings";
> = 0.10;

uniform float CompXlevl <
    ui_type = "drag";
    ui_min = 0.0; ui_max = 20.0; ui_step = 1.0;
    ui_label = "Shadow Mask Strength (Horizontal)";
    ui_category = "Scanline Settings";
> = 3.0;

uniform float densY <
    ui_type = "slider";
    ui_min = 1.0; ui_max = 4.0; ui_step = 1.0;
    ui_label = "Scanline Density";
    ui_category = "Scanline Settings";
> = 2.0;

uniform float densX <
    ui_type = "slider";
    ui_min = 1.0; ui_max = 4.0; ui_step = 1.0;
    ui_label = "Shadow Mask Density";
    ui_category = "Scanline Settings";
> = 3.0;

static const float PI = 3.1415926536;

// --- Vertex to Fragment Bridge ---
struct v2f {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float2 omega : TEXCOORD1;
};

// --- Vertex Shader ---
v2f VS_Scanline(uint id : SV_VertexID) {
    v2f o;
    // Standard full-screen triangle generation
    o.uv.x = (id == 2) ? 2.0 : 0.0;
    o.uv.y = (id == 1) ? 2.0 : 0.0;
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    // Calculate vertical scaling factor based on source vs. output height
    // BUFFER_RCP_HEIGHT is a pre-calculated constant (1.0 / Height) to avoid runtime division
    float scale = oriVert * BUFFER_RCP_HEIGHT;

    // Calculate angular frequency (omega) to control sine wave cycles
    // Locked to source resolution via scale factor to ensure grid alignment across varying viewports
    o.omega = PI * 2.0 * float2(BUFFER_WIDTH * densX, BUFFER_HEIGHT * densY) * scale;

    return o;
}

// --- Pixel Shader ---
float4 PS_Scanline(v2f i) : SV_Target {

    // Step size for horizontal mask intensity
    float sinCompX = CompXlevl * 0.005;

    // Sample source texture (backbuffer)
    float3 texel = tex2D(ReShade::BackBuffer, i.uv).rgb;

    // Map texture coordinates to sine wave phase
    float2 tex_omega_product = i.uv * i.omega;

    // Generate periodic luminosity fluctuation [-1.0, 1.0]
    // Apply -0.5 * PI phase shift to align wave troughs (dark lines) with pixel boundaries
    float2 sine_wave = sin(tex_omega_product - 0.5 * PI);

    // 1. Modulate sine wave intensity per axis
    // 2. Linear accumulation of horizontal/vertical waves for final gain scalar
    float total_sine_fluctuation = (sinCompX * sine_wave.x) + (sinCompY * sine_wave.y);

    // Luma-dependent weighting: calculate distance from mid-tone (0.5)
    // Scanline depth is maximized at 0.5 luma and attenuated at extremes to simulate CRT bloom
    float3 dist = abs(texel - 0.5) * 2.0;
    
    // Composite final brightness:
    // Apply modulation gain adjusted by local luma distance, then multiply by source
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
