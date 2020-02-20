Texture2D samp0 : register(t0);
SamplerState samp0_ss : register(s0);

void main(in float2 v_tex0 : TEXCOORD0,
          out float4 o_col0 : SV_Target)
{
  o_col0 = samp0.Sample(samp0_ss, v_tex0);
}