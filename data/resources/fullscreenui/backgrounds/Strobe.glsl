void main()
{
  vec2 uv = v_tex0 * 2.0 - 1.0;
  uv.x *= u_display_size.x / u_display_size.y;
    
  vec2 center = vec2(cos(u_time * 0.2) * 1.5, (sin(u_time * 0.2) * 0.2) * 0.5 + 1.2);
  float dist = length(uv - center);
  float gradient = smoothstep(0.0, 1.0, dist);
  o_col0 = vec4(mix(vec3(0.5, 0.5, 0.9), vec3(0.05, 0.05, 0.2), gradient), 1.0);
}
