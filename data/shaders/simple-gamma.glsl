/*
[configuration]

[OptionRangeFloat]
GUIName = Gamma In
OptionName = GAMMA_IN
MinValue = 0.1
MaxValue = 10.0
StepAmount = 0.1
DefaultValue = 2.2

[OptionRangeFloat]
GUIName = Gamma Out
OptionName = GAMMA_OUT
MinValue = 0.1
MaxValue = 10.0
StepAmount = 0.1
DefaultValue = 2.2

[/configuration]
*/

void main()
{
  float4 color = Sample();
  float gamma_in = GetOption(GAMMA_IN);
  float gamma_out = 1.0f / GetOption(GAMMA_OUT);

  color.rgb = pow(color.rgb, float3(gamma_in, gamma_in, gamma_in));
  color.rgb = pow(color.rgb, float3(gamma_out, gamma_out, gamma_out));

  SetOutput(saturate(color));
}
