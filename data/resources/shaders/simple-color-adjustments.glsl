/*
[configuration]

[OptionRangeFloat]
GUIName = Brightness
OptionName = BRIGHTNESS
MinValue = -1.0
MaxValue = 1.0
StepAmount = 0.01
DefaultValue = 0.0

[OptionRangeFloat]
GUIName = Contrast
OptionName = CONTRAST
MinValue = 0.0
MaxValue = 3.0
StepAmount = 0.01
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Saturation
OptionName = SATURATION
MinValue = 0.0
MaxValue = 3.0
StepAmount = 0.01
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Gamma
OptionName = GAMMA
MinValue = 0.1
MaxValue = 3.0
StepAmount = 0.01
DefaultValue = 1.0

[/configuration]
*/

void main()
{
  float4 color = Sample();

  // Adjust contrast around mid-gray, then apply an additive brightness offset.
  color.rgb = (color.rgb - 0.5f) * GetOption(CONTRAST) + 0.5f + GetOption(BRIGHTNESS);

  // 0 saturation produces grayscale, 1 preserves the original saturation.
  float luma = dot(color.rgb, float3(0.2126f, 0.7152f, 0.0722f));
  color.rgb = lerp(float3(luma, luma, luma), color.rgb, GetOption(SATURATION));

  // Clamp before pow() to avoid negative bases. Higher gamma brightens midtones.
  float inv_gamma = 1.0f / GetOption(GAMMA);
  color.rgb = pow(saturate(color.rgb), float3(inv_gamma, inv_gamma, inv_gamma));

  SetOutput(color);
}
