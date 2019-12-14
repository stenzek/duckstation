#pragma once
#include "types.h"
#include <optional>
#include <string>

struct Settings
{
  Settings();

  ConsoleRegion region = ConsoleRegion::Auto;

  CPUExecutionMode cpu_execution_mode = CPUExecutionMode::Interpreter;

  bool start_paused = false;
  bool speed_limiter_enabled = true;
  bool audio_sync_enabled = true;
  bool video_sync_enabled = true;

  GPURenderer gpu_renderer = GPURenderer::Software;
  u32 gpu_resolution_scale = 1;
  mutable u32 max_gpu_resolution_scale = 1;
  bool gpu_true_color = false;
  bool gpu_texture_filtering = false;
  bool gpu_force_progressive_scan = false;
  bool display_linear_filtering = true;
  bool display_fullscreen = false;

  struct DebugSettings
  {
    bool show_vram = false;
    bool dump_cpu_to_vram_copies = false;
    bool dump_vram_to_cpu_copies = false;

    // Mutable because the imgui window can close itself.
    mutable bool show_gpu_state = false;
    mutable bool show_cdrom_state = false;
    mutable bool show_spu_state = false;
    mutable bool show_timers_state = false;
    mutable bool show_mdec_state = false;
  } debugging;

  // TODO: Controllers, memory cards, etc.

  std::string bios_path;
  bool bios_patch_tty_enable = false;
  bool bios_patch_fast_boot = false;

  ControllerType controller_a_type = ControllerType::None;
  ControllerType controller_b_type = ControllerType::None;

  std::string memory_card_a_path;
  std::string memory_card_b_path;

  void SetDefaults();
  void Load(const char* filename);
  bool Save(const char* filename) const;

  static std::optional<ConsoleRegion> ParseConsoleRegionName(const char* str);
  static const char* GetConsoleRegionName(ConsoleRegion region);
  static const char* GetConsoleRegionDisplayName(ConsoleRegion region);

  static std::optional<CPUExecutionMode> ParseCPUExecutionMode(const char* str);
  static const char* GetCPUExecutionModeName(CPUExecutionMode mode);
  static const char* GetCPUExecutionModeDisplayName(CPUExecutionMode mode);

  static std::optional<GPURenderer> ParseRendererName(const char* str);
  static const char* GetRendererName(GPURenderer renderer);
  static const char* GetRendererDisplayName(GPURenderer renderer);

  static std::optional<ControllerType> ParseControllerTypeName(const char* str);
  static const char* GetControllerTypeName(ControllerType type);
  static const char* GetControllerTypeDisplayName(ControllerType type);
};
