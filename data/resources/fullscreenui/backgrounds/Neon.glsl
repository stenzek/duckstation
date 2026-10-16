// Retro neon horizon, with a scrolling perspective grid under a starry sky.
const float HORIZON = 0.62;

float hash(vec2 p)
{
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// Returns the line's coverage in x, and its glow in y. Both fade out once the lines get too dense to resolve.
vec2 grid_line(float coord, float aa)
{
  aa = max(aa, 0.0001);
  float dist = abs(fract(coord - 0.5) - 0.5);
  float coverage = 1.0 - smoothstep(0.0, aa * 1.5, dist);
  float glow = max(1.0 - dist / (aa * 6.0), 0.0);
  return vec2(coverage, glow * glow) * (1.0 - smoothstep(0.1, 0.4, aa));
}

void main()
{
  // Normalize the theme's accent so only its hue matters, and square it for a more saturated variant.
  vec3 tint = u_accent_color.rgb / max(max(u_accent_color.r, u_accent_color.g), max(u_accent_color.b, 0.001));
  vec3 deep_tint = tint * tint;
  vec3 light_ink = vec3(1.0) - tint * 0.8;

  vec2 pos = vec2((v_tex0.x - 0.5) * (u_display_size.x / u_display_size.y), v_tex0.y);
  float pixel_height = u_rcp_display_size.y;
  float floor_mask = smoothstep(HORIZON - pixel_height, HORIZON + pixel_height, pos.y);
  float sky_t = smoothstep(0.0, HORIZON, pos.y);
  sky_t *= sky_t;
  float haze = exp(-abs(pos.y - HORIZON) * 10.0);

  // Sky, brightening towards the horizon.
  vec3 color = deep_tint * (0.015 + 0.085 * sky_t + 0.3 * haze);
  vec3 light_color = vec3(0.97) - (vec3(1.0) - tint) * (0.04 + 0.14 * sky_t);

  // Sparse twinkling stars, fading out towards the horizon.
  vec2 star_pos = pos * 60.0;
  vec2 star_cell = floor(star_pos);
  float star_hash = hash(star_cell);
  vec2 star_offset = star_pos - star_cell - (vec2(0.25) + 0.5 * fract(star_hash * vec2(13.7, 7.3)));
  float star = (1.0 - smoothstep(0.0, 1.5 * pixel_height, length(star_offset) / 60.0)) * step(0.97, star_hash);
  star *= (0.5 + 0.5 * sin(u_time + star_hash * 50.0)) * (1.0 - sky_t);
  color += mix(vec3(1.0), tint, 0.3) * star * 0.6;

  // Perspective grid floor, scrolling towards the viewer.
  float depth = 1.0 / max(pos.y - HORIZON, 0.001);
  vec2 grid = vec2(pos.x * depth * 2.5, depth * 1.5 + u_time * 0.25);
  vec2 grid_aa = fwidth(grid);
  vec2 line_x = grid_line(grid.x, grid_aa.x);
  vec2 line_z = grid_line(grid.y, grid_aa.y);
  float near_fade = 0.4 + 0.6 * smoothstep(0.0, 0.15, pos.y - HORIZON);
  float grid_coverage = max(line_x.x, line_z.x) * near_fade;
  float grid_glow = max(line_x.y, line_z.y) * near_fade;
  vec3 floor_color = deep_tint * (0.02 + 0.3 * haze);
  floor_color += mix(tint, vec3(1.0), 0.3) * (grid_coverage * 0.7 + grid_glow * 0.2);
  vec3 light_floor_color = vec3(0.94) - (vec3(1.0) - tint) * 0.15;
  light_floor_color -= light_ink * (grid_coverage * 0.5 + grid_glow * 0.12);
  color = mix(color, floor_color, floor_mask);
  light_color = mix(light_color, light_floor_color, floor_mask);

  // Gently darken the corners.
  vec2 vignette_pos = v_tex0 - vec2(0.5);
  float vignette = dot(vignette_pos, vignette_pos);
  color *= 1.0 - vignette * 0.55;
  light_color *= 1.0 - vignette * 0.20;
  o_col0 = vec4(mix(color, light_color, u_light_theme), 1.0);
}
