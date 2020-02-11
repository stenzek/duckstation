#pragma once
#include "types.h"
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class SettingsInterface
{
public:
  virtual int GetIntValue(const char* section, const char* key, int default_value = 0) = 0;
  virtual float GetFloatValue(const char* section, const char* key, float default_value = 0.0f) = 0;
  virtual bool GetBoolValue(const char* section, const char* key, bool default_value = false) = 0;
  virtual std::string GetStringValue(const char* section, const char* key, const char* default_value = "") = 0;

  virtual void SetIntValue(const char* section, const char* key, int value) = 0;
  virtual void SetFloatValue(const char* section, const char* key, float value) = 0;
  virtual void SetBoolValue(const char* section, const char* key, bool value) = 0;
  virtual void SetStringValue(const char* section, const char* key, const char* value) = 0;

  virtual std::vector<std::string> GetStringList(const char* section, const char* key) = 0;
  virtual void SetStringList(const char* section, const char* key, const std::vector<std::string_view>& items) = 0;
  virtual bool RemoveFromStringList(const char* section, const char* key, const char* item) = 0;
  virtual bool AddToStringList(const char* section, const char* key, const char* item) = 0;

  virtual void DeleteValue(const char* section, const char* key) = 0;
};

struct Settings
{
  Settings();

  ConsoleRegion region = ConsoleRegion::Auto;

  CPUExecutionMode cpu_execution_mode = CPUExecutionMode::Interpreter;

  float emulation_speed = 1.0f;
  bool start_paused = false;
  bool speed_limiter_enabled = true;

  GPURenderer gpu_renderer = GPURenderer::Software;
  u32 gpu_resolution_scale = 1;
  mutable u32 max_gpu_resolution_scale = 1;
  bool gpu_true_color = false;
  bool gpu_texture_filtering = false;
  bool gpu_force_progressive_scan = false;
  bool gpu_use_debug_device = false;
  bool display_linear_filtering = true;
  bool display_fullscreen = false;
  bool video_sync_enabled = true;

  AudioBackend audio_backend = AudioBackend::Default;
  bool audio_sync_enabled = true;

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

  std::array<ControllerType, NUM_CONTROLLER_AND_CARD_PORTS> controller_types{};
  std::array<std::string, NUM_CONTROLLER_AND_CARD_PORTS> memory_card_paths{};

  void Load(SettingsInterface& si);
  void Save(SettingsInterface& si) const;

  static std::optional<ConsoleRegion> ParseConsoleRegionName(const char* str);
  static const char* GetConsoleRegionName(ConsoleRegion region);
  static const char* GetConsoleRegionDisplayName(ConsoleRegion region);

  static std::optional<CPUExecutionMode> ParseCPUExecutionMode(const char* str);
  static const char* GetCPUExecutionModeName(CPUExecutionMode mode);
  static const char* GetCPUExecutionModeDisplayName(CPUExecutionMode mode);

  static std::optional<GPURenderer> ParseRendererName(const char* str);
  static const char* GetRendererName(GPURenderer renderer);
  static const char* GetRendererDisplayName(GPURenderer renderer);

  static std::optional<AudioBackend> ParseAudioBackend(const char* str);
  static const char* GetAudioBackendName(AudioBackend backend);
  static const char* GetAudioBackendDisplayName(AudioBackend backend);

  static std::optional<ControllerType> ParseControllerTypeName(const char* str);
  static const char* GetControllerTypeName(ControllerType type);
  static const char* GetControllerTypeDisplayName(ControllerType type);
};
