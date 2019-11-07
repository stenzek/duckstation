#pragma once
#include "types.h"
#include <optional>

struct Settings
{
  enum class GPURenderer
  {
    HardwareD3D11,
    HardwareOpenGL,
    Software,
    Count
  };

  Settings();

  bool start_paused = false;
  bool speed_limiter_enabled = true;

  GPURenderer gpu_renderer = GPURenderer::Software;
  u32 gpu_resolution_scale = 1;
  u32 max_gpu_resolution_scale = 1;
  bool gpu_vsync = true;
  bool gpu_true_color = false;
  bool display_linear_filtering = true;
  bool display_fullscreen = false;

  struct DebugSettings
  {
    bool show_gpu_state = false;
    bool show_vram = false;
    bool dump_cpu_to_vram_copies = false;
    bool dump_vram_to_cpu_copies = false;

    bool show_cdrom_state = false;
    bool show_spu_state = false;
    bool show_timers_state = false;
    bool show_mdec_state = false;
  } debugging;

  // TODO: Controllers, memory cards, etc.

  std::string bios_path;
  std::string memory_card_a_path;
  std::string memory_card_b_path;

  void SetDefaults();
  void Load(const char* filename);
  bool Save(const char* filename) const;

  static std::optional<GPURenderer> ParseRendererName(const char* str);
  static const char* GetRendererName(GPURenderer renderer);
  static const char* GetRendererDisplayName(GPURenderer renderer);
};
