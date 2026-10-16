void main()
{
  // Normalize the theme's accent so only its hue matters, and square it for a more saturated variant.
  vec3 tint = u_accent_color.rgb / max(max(u_accent_color.r, u_accent_color.g), max(u_accent_color.b, 0.001));
  vec3 deep_tint = tint * tint;

  vec2 uv = v_tex0 * 2.0 - 1.0;
  uv.x *= u_display_size.x / u_display_size.y;
    
  vec2 center = vec2(cos(u_time * 0.2) * 1.5, (sin(u_time * 0.2) * 0.2) * 0.5 + 1.2);
  float dist = length(uv - center);
  float gradient = smoothstep(0.0, 1.0, dist);
  vec3 color = mix(mix(vec3(1.0), deep_tint, 0.5) * 0.9, deep_tint * 0.2, gradient);

  // Light themes use a tinted spot on a pale background.
  vec3 light_color = mix(vec3(1.0) - (vec3(1.0) - tint * 0.8) * 0.55, vec3(0.96) - (vec3(1.0) - tint) * 0.06, gradient);
  o_col0 = vec4(mix(color, light_color, u_light_theme), 1.0);
}
