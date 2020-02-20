cbuffer UBOBlock : register(b0)
{
  float4 u_src_rect;
};

void main(in uint vertex_id : SV_VertexID,
          out float2 v_tex0 : TEXCOORD0,
          out float4 o_pos : SV_Position)
{
  float2 pos = float2(float((vertex_id << 1) & 2u), float(vertex_id & 2u));
  v_tex0 = u_src_rect.xy + pos * u_src_rect.zw;
  o_pos = float4(pos * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
