/*
[configuration]

[OptionRangeFloat]
GUIName = Active Line Brightness
OptionName = BRIGHTEN
MinValue = 0.1
MaxValue = 2.0
StepAmount = 0.1
DefaultValue = 1.1

[OptionRangeFloat]
GUIName = Inactive Line Darkness
OptionName = DARKEN
MinValue = 0.1
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.5

[OptionRangeFloat]
GUIName = Scanline Thickness
OptionName = THICKNESS
MinValue = 0.1
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.5

[OptionRangeFloat]
GUIName = Scanline Spacing
OptionName = SPACING
MinValue = 0.1
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.25

[/configuration]
*/

float3 RGBToYUV(float3 rgb)
{
  return float3(dot(rgb.rgb, float3(0.299, 0.587, 0.114)),
                dot(rgb.rgb, float3(-0.14713, -0.28886, 0.436)),
                dot(rgb.rgb, float3(0.615, -0.51499, -0.10001)));
}

float3 YUVToRGB(float3 yuv)
{
  return float3(dot(yuv, float3(1.0f, 0.0, 1.13983)),
                dot(yuv, float3(1.0f, -0.39465, -0.58060)),
                dot(yuv, float3(1.0f, 2.03211, 0.0)));
}

void main()
{
  float2 pos = GetFragCoord();
  float4 color = Sample();
  float3 yuv = RGBToYUV(color.rgb);

  float thickness = GetOption(THICKNESS);
  float spacing = GetOption(SPACING);
  yuv.r *= (frac(pos.y * spacing) > thickness) ? (1.0 - GetOption(DARKEN)) : GetOption(BRIGHTEN);

  color.rgb = YUVToRGB(yuv);
  SetOutput(color);
}
