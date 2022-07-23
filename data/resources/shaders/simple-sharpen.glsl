void main()
{
  vec2 uv = GetCoordinates();
  vec2 ts = GetInvResolution();

  vec4 sum = vec4(0.0, 0.0, 0.0, 0.0);
  sum += SampleLocation(uv + vec2(-1.0, 0.0) * ts) * -1.0;
  sum += SampleLocation(uv + vec2(0.0, -1.0) * ts) * -1.0;
  sum += SampleLocation(uv) * 5.0;
  sum += SampleLocation(uv + vec2(0.0, 1.0) * ts) * -1.0;
  sum += SampleLocation(uv + vec2(1.0, 0.0) * ts) * -1.0;

  SetOutput(saturate(sum));
}
