const float PI = 3.14159265359;
const float SCALE = 150.0;
const float LENGTH = 7.5;

// https://www.shadertoy.com/view/Xt23Ry
float rand(float co) {
  return fract(sin(co * 91.3458) * 47453.5453);
}

vec3 background(vec2 pos) {
  // Radial gradient at (0.6, 0.4).
  float r = length(pos - vec2(0.6, 0.4));
  return vec3(r * 0.1);
}

// Inspired by https://www.shadertoy.com/view/Wtl3D7
vec3 trails(vec2 pos, vec3 bg_color, vec3 fg_color) {
  float cdist = length(pos) * SCALE;
  float rv = rand(ceil(cdist));
  float rotation = u_time * rv * 0.005;
  float nangle = atan(pos.y, pos.x) / PI;
  float intensity = smoothstep(rv, rv - 1.5, fract(nangle + rotation + rv * 0.1) * LENGTH) * step(0.1, cdist / SCALE);
  return mix(bg_color, fg_color * rv, intensity);
}

void main() {
  vec3 bg_color = background(v_tex0);
  vec3 fg_color = vec3(0.7, 0.7, 1.5);
  o_col0 = vec4(trails(v_tex0, bg_color, fg_color), 1.0);
}
