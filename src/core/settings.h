#pragma once
#include "types.h"

struct Settings
{
  enum class GPURenderer
  {
    HardwareD3D11,
    HardwareOpenGL,
    Software
  };

  Settings();

  bool start_paused = false;

  GPURenderer gpu_renderer = GPURenderer::Software;
  u32 gpu_resolution_scale = 1;
  u32 max_gpu_resolution_scale = 1;
  bool gpu_vsync = true;
  bool gpu_true_color = false;
  bool display_linear_filtering = true;

  struct DebugSettings
  {
    bool show_gpu_state = false;
    bool show_gpu_renderer_stats = false;
    bool show_vram = false;
    bool dump_cpu_to_vram_copies = false;
    bool dump_vram_to_cpu_copies = false;

    bool show_cdrom_state = false;
    bool show_spu_state = false;
    bool show_timers_state = false;
    bool show_mdec_state = false;
  } debugging;

  // TODO: Controllers, memory cards, etc.

  std::string memory_card_a_filename;
  std::string memory_card_b_filename;
};
