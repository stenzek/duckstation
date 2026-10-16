const float PI = 3.14159265359;
const float SCALE = 150.0;
const float LENGTH = 7.5;

// https://www.shadertoy.com/view/Xt23Ry
float rand(float co) {
  return fract(sin(co * 91.3458) * 47453.5453);
}

float background(vec2 pos) {
  // Radial gradient at (0.6, 0.4).
  return length(pos - vec2(0.6, 0.4));
}

// Inspired by https://www.shadertoy.com/view/Wtl3D7
// Returns the trail's coverage in x, and its brightness in y.
vec2 trails(vec2 pos) {
  float cdist = length(pos) * SCALE;
  float rv = rand(ceil(cdist));
  float rotation = u_time * rv * 0.005;
  float nangle = atan(pos.y, pos.x) / PI;
  float intensity = smoothstep(rv, rv - 1.5, fract(nangle + rotation + rv * 0.1) * LENGTH) * step(0.1, cdist / SCALE);
  return vec2(intensity, rv);
}

void main() {
  // Normalize the theme's accent so only its hue matters, and square it for a more saturated variant.
  vec3 tint = u_accent_color.rgb / max(max(u_accent_color.r, u_accent_color.g), max(u_accent_color.b, 0.001));
  vec3 deep_tint = tint * tint;

  float r = background(v_tex0);
  vec2 trail = trails(v_tex0);
  vec3 bg_color = deep_tint * r * 0.1;
  vec3 fg_color = mix(vec3(1.0), deep_tint, 0.55) * 1.5;
  vec3 color = mix(bg_color, fg_color * trail.y, trail.x);

  // Light themes draw tinted trails over a pale background.
  vec3 light_bg_color = vec3(0.97) - (vec3(1.0) - tint) * r * 0.15;
  vec3 light_color = light_bg_color - (vec3(1.0) - tint * 0.8) * (trail.x * trail.y);
  o_col0 = vec4(mix(color, light_color, u_light_theme), 1.0);
}
