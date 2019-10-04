#pragma once
#include "types.h"

enum class GPUBackend
{
  OpenGL
};

struct Settings
{
  Settings();

  GPUBackend gpu_backend = GPUBackend::OpenGL;
  u32 gpu_resolution_scale = 1;
  u32 max_gpu_resolution_scale = 1;
  
  // TODO: Controllers, memory cards, etc.
};

