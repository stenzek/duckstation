void main()
{
  // Normalize the theme's accent so only its hue matters, and square it for a more saturated variant.
  vec3 tint = u_accent_color.rgb / max(max(u_accent_color.r, u_accent_color.g), max(u_accent_color.b, 0.001));
  vec3 deep_tint = tint * tint;

  // Radial gradient at (0.6, 0.4), moving horizontally slowly
  float r1 = length(v_tex0 - vec2(0.6, 0.4));
  float r2 = length(v_tex0 - vec2(0.61, 0.41));
  float r = mix(r1, r2, sin(u_time / 5.0));
  vec3 bg_color = deep_tint * 0.6 * (1.0f - r);

  // Light themes fade from white at the centre to a pale tint at the edges.
  vec3 light_bg_color = vec3(1.0) - (vec3(1.0) - tint * 0.8) * (0.05 + 0.25 * r);
  o_col0 = vec4(mix(bg_color, light_bg_color, u_light_theme), 1.0);
}
