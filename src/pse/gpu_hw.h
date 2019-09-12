#pragma once
#include "gpu.h"
#include <string>
#include <vector>

class GPU_HW : public GPU
{
public:
  GPU_HW();
  virtual ~GPU_HW();

protected:
  struct HWVertex
  {
    s32 x;
    s32 y;
    u32 color;
    u32 texcoord;
  };

  void LoadVertices(RenderCommand rc, u32 num_vertices);

  std::string GenerateVertexShader(bool textured);
  std::string GenerateFragmentShader(bool textured);

  std::vector<HWVertex> m_vertex_staging;
};

