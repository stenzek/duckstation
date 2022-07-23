/*
[configuration]

[OptionRangeFloat]
GUIName = Brightness Scale
OptionName = BRIGHTNESS_SCALE
MinValue = 0.1
MaxValue = 5.0
StepAmount = 0.1
DefaultValue = 1.0

[/configuration]
*/

void main()
{
  float4 color = Sample();
  float brightness_scale = GetOption(BRIGHTNESS_SCALE);

  // rgb->yuv
  float3 yuv;
  yuv.r = dot(color.rgb, float3(0.299f, 0.587f, 0.114f));
  yuv.g = dot(color.rgb, float3(-0.14713f, -0.28886f, 0.436f));
  yuv.b = dot(color.rgb, float3(0.615f, -0.51499f, -0.10001f));

  // apply brightness to y
  yuv.r = saturate(yuv.r * brightness_scale);

  // yuv->rgb
  color.r = dot(yuv, float3(1.0f, 0.0f, 1.13983f));
  color.g = dot(yuv, float3(1.0f, -0.39465f, -0.58060f));
  color.b = dot(yuv, float3(1.0f, 2.03211f, 0.0f));
  color.rgb = saturate(color.rgb);

  SetOutput(saturate(color));
}
