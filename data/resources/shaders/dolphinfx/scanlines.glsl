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
GUIName = ScanlineType
OptionName = A_SCANLINE_TYPE
MinValue = 0
MaxValue = 2
StepAmount = 1
DefaultValue = 0

[OptionRangeFloat]
GUIName = ScanlineIntensity
OptionName = B_SCANLINE_INTENSITY
MinValue = 0.15
MaxValue = 0.30
StepAmount = 0.01
DefaultValue = 0.18

[OptionRangeFloat]
GUIName = ScanlineThickness
OptionName = B_SCANLINE_THICKNESS
MinValue = 0.20
MaxValue = 0.80
StepAmount = 0.01
DefaultValue = 0.50

[OptionRangeFloat]
GUIName = ScanlineBrightness
OptionName = B_SCANLINE_BRIGHTNESS
MinValue = 0.50
MaxValue = 2.00
StepAmount = 0.01
DefaultValue = 1.10

[OptionRangeFloat]
GUIName = ScanlineSpacing
OptionName = B_SCANLINE_SPACING
MinValue = 0.10
MaxValue = 0.99
StepAmount = 0.01
DefaultValue = 0.25

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

void main()
{
    float4 color = Sample();
    float4 intensity = float4(0.0, 0.0, 0.0, 0.0);

    if (GetOption(A_SCANLINE_TYPE) == 0) { //X coord scanlines
    if (fract(gl_FragCoord.y * GetOption(B_SCANLINE_SPACING)) > GetOption(B_SCANLINE_THICKNESS))
    {
        intensity = float4(0.0, 0.0, 0.0, 0.0);
    } 
    else
    {
        intensity = smoothstep(0.2, GetOption(B_SCANLINE_BRIGHTNESS), color) +
        normalize(float4(color.xyz, AvgLuminance(color.xyz)));
    } }

    else if (GetOption(A_SCANLINE_TYPE) == 1) { //Y coord scanlines
    if (fract(gl_FragCoord.x * GetOption(B_SCANLINE_SPACING)) > GetOption(B_SCANLINE_THICKNESS))
    {
        intensity = float4(0.0, 0.0, 0.0, 0.0);
    }
    else
    {
        intensity = smoothstep(0.2, GetOption(B_SCANLINE_BRIGHTNESS), color) +
        normalize(float4(color.xyz, AvgLuminance(color.xyz)));
    } }

    else if (GetOption(A_SCANLINE_TYPE) == 2) { //XY coord scanlines
    if (fract(gl_FragCoord.x * GetOption(B_SCANLINE_SPACING)) > GetOption(B_SCANLINE_THICKNESS) &&
        fract(gl_FragCoord.y * GetOption(B_SCANLINE_SPACING)) > GetOption(B_SCANLINE_THICKNESS))
    {
        intensity = float4(0.0, 0.0, 0.0, 0.0);
    }
    else
    {
        intensity = smoothstep(0.2, GetOption(B_SCANLINE_BRIGHTNESS), color) +
        normalize(float4(color.xyz, AvgLuminance(color.xyz)));
    } }

    float level = (4.0-GetCoordinates().x) * GetOption(B_SCANLINE_INTENSITY);

    color = intensity * (0.5 - level) + color * 1.1;

    SetOutput(saturate(color));
}