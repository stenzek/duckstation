void main()
{
  vec2 pos = v_tex0 - vec2(0.5);
  pos.x *= u_display_size.x / u_display_size.y;
  float time = u_time * 0.09;
  float pixel_width = u_rcp_display_size.y;

  // A subdued blue gradient keeps menu text readable above the waves.
  vec3 color = mix(vec3(0.008, 0.015, 0.035), vec3(0.025, 0.055, 0.105), v_tex0.y);
  vec2 light_pos = pos - vec2(0.35, 0.12);
  color += vec3(0.015, 0.035, 0.065) * exp(-dot(light_pos, light_pos) * 2.5);

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
    vec3 ribbon_color = mix(vec3(0.10, 0.28, 0.50), vec3(0.28, 0.48, 0.68), layer / 5.0);
    color += ribbon_color * coverage * (0.055 + surface * 0.10) * strength;
    color += vec3(0.48, 0.68, 0.85) * (upper_rim * 0.22 + lower_rim * 0.07) * fold * strength;
    color += vec3(0.10, 0.25, 0.45) * glow * 0.08 * fold * strength;
  }

  // Gently darken the corners without changing the ribbons' proportions.
  vec2 vignette_pos = v_tex0 - vec2(0.5);
  color *= 1.0 - dot(vignette_pos, vignette_pos) * 0.55;
  o_col0 = vec4(color, 1.0);
}
