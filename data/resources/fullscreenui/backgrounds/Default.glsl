void main()
{
  // Radial gradient at (0.6, 0.4), moving horizontally slowly
  float r1 = length(v_tex0 - vec2(0.6, 0.4));
  float r2 = length(v_tex0 - vec2(0.61, 0.41));
  float r = mix(r1, r2, sin(u_time / 5.0));
  vec3 bg_color = vec3(0.1, 0.1, 0.6) * (1.0f - r);
  o_col0 = vec4(bg_color, 1.0);
}
