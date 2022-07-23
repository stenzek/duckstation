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

[OptionRangeFloat]
GUIName = EdgeStrength
OptionName = A_EDGE_STRENGTH
MinValue = 0.00
MaxValue = 4.00
StepAmount = 0.01
DefaultValue = 1.00

[OptionRangeFloat]
GUIName = EdgeFilter
OptionName = B_EDGE_FILTER
MinValue = 0.25
MaxValue = 1.00
StepAmount = 0.01
DefaultValue = 0.60

[OptionRangeFloat]
GUIName = EdgeThickness
OptionName = C_EDGE_THICKNESS
MinValue = 0.25
MaxValue = 2.00
StepAmount = 0.01
DefaultValue = 1.00

[OptionRangeInteger]
GUIName = PaletteType
OptionName = D_PALETTE_TYPE
MinValue = 0
MaxValue = 2
StepAmount = 1
DefaultValue = 1

[OptionRangeInteger]
GUIName = UseYuvLuma
OptionName = E_YUV_LUMA
MinValue = 0
MaxValue = 1
StepAmount = 1
DefaultValue = 0

[OptionRangeInteger]
GUIName = ColourRounding
OptionName = G_COLOR_ROUNDING
MinValue = 0
MaxValue = 1
StepAmount = 1
DefaultValue = 1

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

float3 YUVtoRGB(float3 YUV)
{
   const float3x3 m = float3x3(
    1.000, 0.000, 1.28033,
    1.000,-0.21482,-0.38059,
    1.000, 2.12798, 0.000 );

    return mul(m, YUV);
}

float3 RGBtoYUV(float3 RGB)
{
    const float3x3 m = float3x3(
    0.2126, 0.7152, 0.0722,
   -0.09991,-0.33609, 0.436,
    0.615, -0.55861, -0.05639 );

    return mul(m, RGB);
}

void main()
{
    float4 color = Sample();
    float2 texcoord = GetCoordinates();
    float2 pixelSize = GetInvResolution();
    float2 texSize = GetResolution();

    float3 yuv;
    float3 sum = color.rgb;

    const int NUM = 9;
    const float2 RoundingOffset = float2(0.25, 0.25);
    const float3 thresholds = float3(9.0, 8.0, 6.0);

    float lum[NUM];
    float3 col[NUM];
    float2 set[NUM] = BEGIN_ARRAY(float2, NUM)
    float2(-0.0078125, -0.0078125),
    float2(0.00, -0.0078125),
    float2(0.0078125, -0.0078125),
    float2(-0.0078125, 0.00),
    float2(0.00, 0.00),
    float2(0.0078125, 0.00),
    float2(-0.0078125, 0.0078125),
    float2(0.00, 0.0078125),
    float2(0.0078125, 0.0078125) END_ARRAY;

    for (int i = 0; i < NUM; i++)
    {
        col[i] = SampleLocation(texcoord + set[i] * RoundingOffset).rgb;

        if (GetOption(G_COLOR_ROUNDING) == 1) {
        col[i].r = round(col[i].r * thresholds.r) / thresholds.r;
        col[i].g = round(col[i].g * thresholds.g) / thresholds.g;
        col[i].b = round(col[i].b * thresholds.b) / thresholds.b; }

        lum[i] = AvgLuminance(col[i].xyz);
        yuv = RGBtoYUV(col[i]);

        if (GetOption(E_YUV_LUMA) == 0)
        { yuv.r = round(yuv.r * thresholds.r) / thresholds.r; }
        else
        { yuv.r = saturate(round(yuv.r * lum[i]) / thresholds.r + lum[i]); }

        yuv = YUVtoRGB(yuv);
        sum += yuv;
    }

    float3 shadedColor = (sum / NUM);
    float2 pixel = float2((1.0/texSize.x) * GetOption(C_EDGE_THICKNESS),
                          (1.0/texSize.y) * GetOption(C_EDGE_THICKNESS));

    float edgeX = dot(SampleLocation(texcoord + pixel).rgb, lumCoeff);
    edgeX = dot(float4(SampleLocation(texcoord - pixel).rgb, edgeX), float4(lumCoeff, -1.0));

    float edgeY = dot(SampleLocation(texcoord + float2(pixel.x, -pixel.y)).rgb, lumCoeff);
    edgeY = dot(float4(SampleLocation(texcoord + float2(-pixel.x, pixel.y)).rgb, edgeY), float4(lumCoeff, -1.0));

    float edge = dot(float2(edgeX, edgeY), float2(edgeX, edgeY));

    if (GetOption(D_PALETTE_TYPE) == 0)
        { color.rgb = lerp(color.rgb, color.rgb + pow(edge, GetOption(B_EDGE_FILTER)) * -GetOption(A_EDGE_STRENGTH), GetOption(A_EDGE_STRENGTH)); }
    else if (GetOption(D_PALETTE_TYPE) == 1)
        { color.rgb = lerp(color.rgb + pow(edge, GetOption(B_EDGE_FILTER)) * -GetOption(A_EDGE_STRENGTH), shadedColor, 0.25); }
    else if (GetOption(D_PALETTE_TYPE) == 2)
        { color.rgb = lerp(shadedColor + edge * -GetOption(A_EDGE_STRENGTH), pow(edge, GetOption(B_EDGE_FILTER)) * -GetOption(A_EDGE_STRENGTH) + color.rgb, 0.50); }

    color.a = AvgLuminance(color.rgb);

    SetOutput(saturate(color));
}
