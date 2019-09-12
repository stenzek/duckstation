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
        else
          hw_vert.texcoord = 0;

        m_vertex_staging.push_back(hw_vert);
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

void GPU_HW::CalcViewport(int* x, int* y, int* width, int* height)
{
  *x = m_drawing_offset.x;
  *y = m_drawing_offset.y;
  *width = std::max(static_cast<int>(VRAM_WIDTH - m_drawing_offset.x), 1);
  *height = std::max(static_cast<int>(VRAM_HEIGHT - m_drawing_offset.y), 1);
}

void GPU_HW::CalcScissorRect(int* left, int* top, int* right, int* bottom)
{
  *left = m_drawing_area.top_left_x;
  *right = m_drawing_area.bottom_right_x;
  *top = m_drawing_area.top_left_y;
  *bottom = m_drawing_area.bottom_right_y;
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
in ivec2 a_position;
in vec4 a_color;
in uint a_texcoord;

out vec4 v_color;
#if TEXTURED
  out vec2 v_texcoord;
#endif

void main()
{
  // 0..+1023 -> -1..1
  float pos_x = (float(a_position.x) / 511.5) - 1.0;
  float pos_y = (float(a_position.y) / 255.5) + 1.0;
  gl_Position = vec4(pos_x, pos_y, 0.0, 1.0);

  v_color = a_color;
  #if TEXTURED
    v_texcoord = vec2(float(a_texcoord & 0xFFu) / 256.0, float((a_texcoord >> 8) & 0xFFu) / 256.0);
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
  ocol0 = v_color;
  //ocol0 = vec4(1.0, 0.5, 0.5, 1.0);
}
)";

  return ss.str();
}
