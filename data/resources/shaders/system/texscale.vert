#version 460 core

layout(location = 0) out VertexData {
  vec2 v_tex0;
};

void main()
{
  v_tex0 = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2u));
  gl_Position = vec4(v_tex0 * vec2(2.0f, -2.0f) + vec2(-1.0f, 1.0f), 0.0f, 1.0f);
  #if API_OPENGL || API_OPENGL_ES || API_VULKAN
    gl_Position.y = -gl_Position.y;
  #endif
}
