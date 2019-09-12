#include "gpu_hw.h"
#include "YBaseLib/Assert.h"
#include <sstream>

GPU_HW::GPU_HW() = default;

GPU_HW::~GPU_HW() = default;

void GPU_HW::LoadVertices(RenderCommand rc, u32 num_vertices)
{
  switch (rc.primitive)
  {
    case Primitive::Polygon:
    {
      const u32 first_colour = m_GP0_command[0] & UINT32_C(0x00FFFFFF);
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;

      // if we're drawing quads, we need to create a degenerate triangle to restart the triangle strip
      if (rc.quad_polygon && !m_vertex_staging.empty())
        m_vertex_staging.push_back(m_vertex_staging.back());

      u32 buffer_pos = 1;
      for (u32 i = 0; i < num_vertices; i++)
      {
        HWVertex hw_vert;
        hw_vert.color = (shaded && i > 0) ? (m_GP0_command[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_colour;

        const VertexPosition vp{m_GP0_command[buffer_pos++]};
        hw_vert.x = vp.x();
        hw_vert.y = vp.y();

        if (textured)
          hw_vert.texcoord = (m_GP0_command[buffer_pos++] & UINT32_C(0x0000FFFF));

        m_vertex_staging.push_back(hw_vert);
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

std::string GPU_HW::GenerateVertexShader(bool textured)
{
  std::stringstream ss;
  ss << "#version 330 core\n";
  if (textured)
    ss << "#define TEXTURED 1\n";
  else
    ss << "/* #define TEXTURED 0 */\n";

  ss << R"(
in uivec2 a_position;
in uint a_texcoord;
in vec4 a_color;

out vec4 v_color;
#if TEXTURED
  out vec2 v_texcoord;
#endif

void main()
{
  // -1024..+1023 -> -1..1
  float gl_x = (a_position.x < 0) ? (float(a_position.x) / 1024.0) : (float(a_position.x) / 1023.0);
  float gl_y = (a_position.y < 0) ? -(float(a_position.y) / 1024.0) : -(float(a_position.y) / 1023.0);
  gl_Position = vec4(gl_x, gl_y, 0.0, 1.0);

  v_color = a_color;
  #if TEXTURED
    v_texcoord = a_texcoord;
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW::GenerateFragmentShader(bool textured)
{
  std::stringstream ss;
  ss << "#version 330 core\n";
  if (textured)
    ss << "#define TEXTURED 1\n";
  else
    ss << "/* #define TEXTURED 0 */\n";

  ss << R"(
in vec4 v_color;
#if TEXTURED
  in vec2 v_texcoord;
#endif

out vec4 ocol0;

void main()
{
  //ocol0 = v_color;
  ocol0 = vec4(1.0, 0.5, 0.5, 1.0);
}
)";

  return ss.str();
}
