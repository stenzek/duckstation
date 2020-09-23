/*===============================================================================*\
|########################     [Dolphin FX Suite 2.20]      #######################|
|##########################        By Asmodean          ##########################|
||                                                                               ||
||          This program is free software; you can redistribute it and/or        ||
||          modify it under the terms of the GNU General Public License          ||
||          as published by the Free Software Foundation; either version 2       ||
||          of the License, or (at your option) any later version.               ||
||                                                                               ||
||          This program is distributed in the hope that it will be useful,      ||
||          but WITHOUT ANY WARRANTY; without even the implied warranty of       ||
||          MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        ||
||          GNU General Public License for more details. (C)2015                 ||
||                                                                               ||
|#################################################################################|
\*===============================================================================*/

// Sourced from https://raw.githubusercontent.com/Asmodean-/dolphin/89d640cd557189bb5f921fc219150c74c39bdc55/Data/Sys/Shaders/DolphinFX.glsl with modifications.

/*
[configuration]

[OptionRangeInteger]
GUIName = BloomType
OptionName = A_BLOOM_TYPE
MinValue = 0
MaxValue = 5
StepAmount = 1
DefaultValue = 0

[OptionRangeFloat]
GUIName = BloomStrength
OptionName = B_BLOOM_STRENGTH
MinValue = 0.000
MaxValue = 1.000
StepAmount = 0.001
DefaultValue = 0.220

[OptionRangeFloat]
GUIName = BlendStrength
OptionName = C_BLEND_STRENGTH
MinValue = 0.000
MaxValue = 1.200
StepAmount = 0.010
DefaultValue = 1.000

[OptionRangeFloat]
GUIName = BloomDefocus
OptionName = D_B_DEFOCUS
MinValue = 1.000
MaxValue = 4.000
StepAmount = 0.100
DefaultValue = 2.000

[OptionRangeFloat]
GUIName = BloomWidth
OptionName = D_BLOOM_WIDTH
MinValue = 1.000
MaxValue = 8.000
StepAmount = 0.100
DefaultValue = 3.200

[OptionRangeFloat]
GUIName = BloomReds
OptionName = E_BLOOM_REDS
MinValue = 0.000
MaxValue = 0.500
StepAmount = 0.001
DefaultValue = 0.020

[OptionRangeFloat]
GUIName = BloomGreens
OptionName = F_BLOOM_GREENS
MinValue = 0.000
MaxValue = 0.500
StepAmount = 0.001
DefaultValue = 0.010

[OptionRangeFloat]
GUIName = BloomBlues
OptionName = G_BLOOM_BLUES
MinValue = 0.000
MaxValue = 0.500
StepAmount = 0.001
DefaultValue = 0.010

[/configuration]
*/

//Average relative luminance
CONSTANT float3 lumCoeff = float3(0.2126729, 0.7151522, 0.0721750);
float AvgLuminance(float3 color)
{
    return sqrt(
    (color.x * color.x * lumCoeff.x) +
    (color.y * color.y * lumCoeff.y) +
    (color.z * color.z * lumCoeff.z));
}

float smootherstep(float a, float b, float x)
{
    x = saturate((x - a) / (b - a));
    return x*x*x*(x*(x * 6.0 - 15.0) + 10.0);
}

float3 BlendAddLight(float3 bloom, float3 blend)
{
    return saturate(bloom + blend);
}

float3 BlendScreen(float3 bloom, float3 blend)
{
    return (bloom + blend) - (bloom * blend);
}

float3 BlendAddGlow(float3 bloom, float3 blend)
{
    float glow = smootherstep(0.0, 1.0, AvgLuminance(bloom));
    return lerp(saturate(bloom + blend),
    (blend + blend) - (blend * blend), glow);
}

float3 BlendGlow(float3 bloom, float3 blend)
{
    float glow = smootherstep(0.0, 1.0, AvgLuminance(bloom));
    return lerp((bloom + blend) - (bloom * blend),
    (blend + blend) - (blend * blend), glow);
}

float3 BlendLuma(float3 bloom, float3 blend)
{
    float lumavg = smootherstep(0.0, 1.0, AvgLuminance(bloom + blend));
    return lerp((bloom * blend), (1.0 -
    ((1.0 - bloom) * (1.0 - blend))), lumavg);
}

float3 BlendOverlay(float3 bloom, float3 blend)
{
    float3 overlay = step(0.5, bloom);
    return lerp((bloom * blend * 2.0), (1.0 - (2.0 *
    (1.0 - bloom) * (1.0 - blend))), overlay);
}

float3 BloomCorrection(float3 color)
{
    float3 bloom = color;

    bloom.r = 2.0 / 3.0 * (1.0 - (bloom.r * bloom.r));
    bloom.g = 2.0 / 3.0 * (1.0 - (bloom.g * bloom.g));
    bloom.b = 2.0 / 3.0 * (1.0 - (bloom.b * bloom.b));

    bloom.r = saturate(color.r + GetOption(E_BLOOM_REDS) * bloom.r);
    bloom.g = saturate(color.g + GetOption(F_BLOOM_GREENS) * bloom.g);
    bloom.b = saturate(color.b + GetOption(G_BLOOM_BLUES) * bloom.b);

    color = saturate(bloom);

    return color;
}

float4 PyramidFilter(float2 texcoord, float2 width)
{
    float4 X = SampleLocation(texcoord + float2(0.5, 0.5) * width);
    float4 Y = SampleLocation(texcoord + float2(-0.5,  0.5) * width);
    float4 Z = SampleLocation(texcoord + float2(0.5, -0.5) * width);
    float4 W = SampleLocation(texcoord + float2(-0.5, -0.5) * width);

    return (X + Y + Z + W) / 4.0;
}

float3 Blend(float3 bloom, float3 blend)
{
         if (GetOption(A_BLOOM_TYPE) == 0) { return BlendGlow(bloom, blend); }
    else if (GetOption(A_BLOOM_TYPE) == 1) { return BlendAddGlow(bloom, blend); }
    else if (GetOption(A_BLOOM_TYPE) == 2) { return BlendAddLight(bloom, blend); }
    else if (GetOption(A_BLOOM_TYPE) == 3) { return BlendScreen(bloom, blend); }
    else if (GetOption(A_BLOOM_TYPE) == 4) { return BlendLuma(bloom, blend); }
    else /*if (GetOption(A_BLOOM_TYPE) == 5) */ { return BlendOverlay(bloom, blend); }
}

void main()
{
    float4 color = Sample();
    float2 texcoord = GetCoordinates();
    float2 pixelSize = GetInvResolution();

    float anflare = 4.0;

    float2 defocus = float2(GetOption(D_B_DEFOCUS), GetOption(D_B_DEFOCUS));
    float4 bloom = PyramidFilter(texcoord, pixelSize * defocus);

    float2 dx = float2(pixelSize.x * GetOption(D_BLOOM_WIDTH), 0.0);
    float2 dy = float2(0.0, pixelSize.y * GetOption(D_BLOOM_WIDTH));

    float2 mdx = mul(dx, 2.0);
    float2 mdy = mul(dy, 2.0);

    float4 blend = bloom * 0.22520613262190495;

    blend += 0.002589001911021066 * SampleLocation(texcoord - mdx + mdy);
    blend += 0.010778807494659370 * SampleLocation(texcoord - dx + mdy);
    blend += 0.024146616900339800 * SampleLocation(texcoord + mdy);
    blend += 0.010778807494659370 * SampleLocation(texcoord + dx + mdy);
    blend += 0.002589001911021066 * SampleLocation(texcoord + mdx + mdy);

    blend += 0.010778807494659370 * SampleLocation(texcoord - mdx + dy);
    blend += 0.044875475183061630 * SampleLocation(texcoord - dx + dy);
    blend += 0.100529757860782610 * SampleLocation(texcoord + dy);
    blend += 0.044875475183061630 * SampleLocation(texcoord + dx + dy);
    blend += 0.010778807494659370 * SampleLocation(texcoord + mdx + dy);

    blend += 0.024146616900339800 * SampleLocation(texcoord - mdx);
    blend += 0.100529757860782610 * SampleLocation(texcoord - dx);
    blend += 0.100529757860782610 * SampleLocation(texcoord + dx);
    blend += 0.024146616900339800 * SampleLocation(texcoord + mdx);

    blend += 0.010778807494659370 * SampleLocation(texcoord - mdx - dy);
    blend += 0.044875475183061630 * SampleLocation(texcoord - dx - dy);
    blend += 0.100529757860782610 * SampleLocation(texcoord - dy);
    blend += 0.044875475183061630 * SampleLocation(texcoord + dx - dy);
    blend += 0.010778807494659370 * SampleLocation(texcoord + mdx - dy);

    blend += 0.002589001911021066 * SampleLocation(texcoord - mdx - mdy);
    blend += 0.010778807494659370 * SampleLocation(texcoord - dx - mdy);
    blend += 0.024146616900339800 * SampleLocation(texcoord - mdy);
    blend += 0.010778807494659370 * SampleLocation(texcoord + dx - mdy);
    blend += 0.002589001911021066 * SampleLocation(texcoord + mdx - mdy);
    blend = lerp(color, blend, GetOption(C_BLEND_STRENGTH));

    bloom.xyz = Blend(bloom.xyz, blend.xyz);
    bloom.xyz = BloomCorrection(bloom.xyz);

    color.a = AvgLuminance(color.xyz);
    bloom.a = AvgLuminance(bloom.xyz);
    bloom.a *= anflare;

    SetOutput(lerp(color, bloom, GetOption(B_BLOOM_STRENGTH)));
}
