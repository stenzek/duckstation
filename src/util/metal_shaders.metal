/// A custom resolve kernel that averages color at all sample points.
#include <metal_stdlib>
using namespace metal;

// https://developer.apple.com/documentation/metal/metal_sample_code_library/improving_edge-rendering_quality_with_multisample_antialiasing_msaa?language=objc
kernel void colorResolveKernel(texture2d_ms<float, access::read> multisampledTexture [[texture(0)]],
                               texture2d<float, access::write> resolvedTexture [[texture(1)]],
                               uint2 gid [[thread_position_in_grid]])
{
  // The grid is rounded up to the threadgroup size, and out-of-bounds accesses are undefined.
  if (gid.x >= resolvedTexture.get_width() || gid.y >= resolvedTexture.get_height() ||
      gid.x >= multisampledTexture.get_width() || gid.y >= multisampledTexture.get_height())
  {
    return;
  }

  const uint count = multisampledTexture.get_num_samples();

  float4 resolved_color = 0;

  for (uint i = 0; i < count; ++i)
  {
    resolved_color += multisampledTexture.read(gid, i);
  }

  resolved_color /= count;

  resolvedTexture.write(resolved_color, gid);
}

struct DepthClearUBO
{
  float depth;
};

struct DepthClearOut
{
  float4 pos [[position]];
};

vertex DepthClearOut depthClearVertex(constant DepthClearUBO& ubo [[buffer(0)]], uint vertexId [[vertex_id]])
{
  DepthClearOut out = {};
  float2 uv = float2(float((vertexId << uint(1)) & 2u), float(vertexId & 2u));
  out.pos = float4((uv * float2(2.0, -2.0)) + float2(-1.0, 1.0), ubo.depth, 1.0);
  return out;
}

fragment void depthClearFragment()
{
}

struct DepthResolveOut
{
  float depth [[depth(any)]];
};

// Depth textures cannot be written to from a compute kernel, so the resolve is drawn instead.
fragment DepthResolveOut depthResolveFragment(DepthClearOut in [[stage_in]],
                                              depth2d_ms<float, access::read> multisampledTexture [[texture(0)]])
{
  const uint2 pos = uint2(in.pos.xy);
  const uint count = multisampledTexture.get_num_samples();

  float resolved_depth = 0;

  for (uint i = 0; i < count; ++i)
  {
    resolved_depth += multisampledTexture.read(pos, i);
  }

  DepthResolveOut out = {};
  out.depth = resolved_depth / count;
  return out;
}
