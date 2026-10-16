void main()
{
  vec2 pos = v_tex0 - vec2(0.5);
  pos.x *= u_display_size.x / u_display_size.y;
  float time = u_time * 0.09;
  float pixel_width = u_rcp_display_size.y;

  // Normalize the theme's accent so only its hue matters, and square it for a more saturated variant.
  vec3 tint = u_accent_color.rgb / max(max(u_accent_color.r, u_accent_color.g), max(u_accent_color.b, 0.001));
  vec3 deep_tint = tint * tint;

  // A subdued gradient of the accent keeps menu text readable above the waves.
  // Light themes use a pale gradient instead, with the ribbons tinting it.
  vec2 light_pos = pos - vec2(0.35, 0.12);
  float spot = exp(-dot(light_pos, light_pos) * 2.5);
  vec3 color = deep_tint * (0.035 + 0.07 * v_tex0.y + 0.065 * spot);
  vec3 light_color = vec3(0.95 - 0.02 * v_tex0.y) - (vec3(1.0) - tint) * (0.05 + 0.12 * v_tex0.y - 0.05 * spot);
  vec3 rim_color = mix(vec3(1.0), tint, 0.7) * 0.85;
  vec3 light_ribbon_color = vec3(1.0) - tint * 0.8;

  for (int i = 0; i < 6; i++)
  {
    float layer = float(i);
    float phase = layer * 0.38;
    float center = 0.12 + (layer - 2.5) * 0.018;
    center += 0.10 * sin(pos.x * 2.0 - time + phase);
    center += 0.045 * sin(pos.x * 3.2 + time * 0.65 + phase * 0.7);

    // The signed width lets each ribbon turn over and narrow into a fold.
    float width = 0.055 * sin(pos.x * 1.6 + time * 0.45 + phase) + 0.025;
    float half_width = abs(width);
    float offset = pos.y - center;
    float distance = abs(offset);
    float aa = max(fwidth(offset), pixel_width);
    float coverage = 1.0 - smoothstep(max(half_width - aa, 0.0), half_width + aa, distance);
    float surface = 1.0 - smoothstep(0.0, max(half_width, pixel_width), distance);

    float upper_edge = abs(offset - width);
    float lower_edge = abs(offset + width);
    float upper_rim = 1.0 - smoothstep(aa, aa * 3.0, upper_edge);
    float lower_rim = 1.0 - smoothstep(aa, aa * 2.0, lower_edge);
    float glow = exp(-upper_edge * upper_edge / 0.00045);

    // Fade the highlights at the folds, where the two edges meet.
    float fold = smoothstep(0.0, 0.025, half_width);
    float strength = 0.55 + layer * 0.075;
    float body = coverage * (0.055 + surface * 0.10) * strength;
    float rim = (upper_rim * 0.22 + lower_rim * 0.07) * fold * strength;
    float halo = glow * 0.08 * fold * strength;
    vec3 ribbon_color = mix(deep_tint * 0.50, tint * 0.68, layer / 5.0);
    color += ribbon_color * body + rim_color * rim + deep_tint * 0.45 * halo;
    light_color -= light_ribbon_color * (body * 1.2 + rim * 0.55 + halo * 0.6);
  }

  // Gently darken the corners without changing the ribbons' proportions.
  vec2 vignette_pos = v_tex0 - vec2(0.5);
  float vignette = dot(vignette_pos, vignette_pos);
  color *= 1.0 - vignette * 0.55;
  light_color *= 1.0 - vignette * 0.20;
  color = mix(color, light_color, u_light_theme);
  o_col0 = vec4(color, 1.0);
}
