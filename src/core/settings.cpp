#include "settings.h"
#include "common/string_util.h"
#include <array>

Settings::Settings() = default;

void Settings::Load(SettingsInterface& si)
{
  region =
    ParseConsoleRegionName(si.GetStringValue("Console", "Region", "NTSC-U").c_str()).value_or(ConsoleRegion::NTSC_U);

  emulation_speed = si.GetFloatValue("General", "EmulationSpeed", 1.0f);
  speed_limiter_enabled = si.GetBoolValue("General", "SpeedLimiterEnabled", true);
  start_paused = si.GetBoolValue("General", "StartPaused", false);

  cpu_execution_mode = ParseCPUExecutionMode(si.GetStringValue("CPU", "ExecutionMode", "Interpreter").c_str())
                         .value_or(CPUExecutionMode::Interpreter);

  gpu_renderer =
    ParseRendererName(si.GetStringValue("GPU", "Renderer", "OpenGL").c_str()).value_or(GPURenderer::HardwareOpenGL);
  gpu_resolution_scale = static_cast<u32>(si.GetIntValue("GPU", "ResolutionScale", 1));
  gpu_true_color = si.GetBoolValue("GPU", "TrueColor", false);
  gpu_texture_filtering = si.GetBoolValue("GPU", "TextureFiltering", false);
  gpu_force_progressive_scan = si.GetBoolValue("GPU", "ForceProgressiveScan", true);
  gpu_use_debug_device = si.GetBoolValue("GPU", "UseDebugDevice", false);

  display_linear_filtering = si.GetBoolValue("Display", "LinearFiltering", true);
  display_fullscreen = si.GetBoolValue("Display", "Fullscreen", false);
  video_sync_enabled = si.GetBoolValue("Display", "VSync", true);

  audio_backend =
    ParseAudioBackend(si.GetStringValue("Audio", "Backend", "Default").c_str()).value_or(AudioBackend::Default);
  audio_sync_enabled = si.GetBoolValue("Audio", "Sync", true);

  bios_path = si.GetStringValue("BIOS", "Path", "scph1001.bin");
  bios_patch_tty_enable = si.GetBoolValue("BIOS", "PatchTTYEnable", true);
  bios_patch_fast_boot = si.GetBoolValue("BIOS", "PatchFastBoot", false);

  controller_types[0] = ParseControllerTypeName(si.GetStringValue("Controller1", "Type", "DigitalController").c_str())
                          .value_or(ControllerType::DigitalController);
  controller_types[1] =
    ParseControllerTypeName(si.GetStringValue("Controller2", "Type", "None").c_str()).value_or(ControllerType::None);

  memory_card_paths[0] = si.GetStringValue("MemoryCards", "Card1Path", "memory_card_1.mcd");
  memory_card_paths[1] = si.GetStringValue("MemoryCards", "Card2Path", "");

  debugging.show_vram = si.GetBoolValue("Debug", "ShowVRAM");
  debugging.dump_cpu_to_vram_copies = si.GetBoolValue("Debug", "DumpCPUToVRAMCopies");
  debugging.dump_vram_to_cpu_copies = si.GetBoolValue("Debug", "DumpVRAMToCPUCopies");
  debugging.show_gpu_state = si.GetBoolValue("Debug", "ShowGPUState");
  debugging.show_cdrom_state = si.GetBoolValue("Debug", "ShowCDROMState");
  debugging.show_spu_state = si.GetBoolValue("Debug", "ShowSPUState");
  debugging.show_timers_state = si.GetBoolValue("Debug", "ShowTimersState");
  debugging.show_mdec_state = si.GetBoolValue("Debug", "ShowMDECState");
}

void Settings::Save(SettingsInterface& si) const
{
  si.SetStringValue("Console", "Region", GetConsoleRegionName(region));

  si.SetFloatValue("General", "EmulationSpeed", emulation_speed);
  si.SetBoolValue("General", "SpeedLimiterEnabled", speed_limiter_enabled);
  si.SetBoolValue("General", "StartPaused", start_paused);

  si.SetStringValue("CPU", "ExecutionMode", GetCPUExecutionModeName(cpu_execution_mode));

  si.SetStringValue("GPU", "Renderer", GetRendererName(gpu_renderer));
  si.SetIntValue("GPU", "ResolutionScale", static_cast<long>(gpu_resolution_scale));
  si.SetBoolValue("GPU", "TrueColor", gpu_true_color);
  si.SetBoolValue("GPU", "TextureFiltering", gpu_texture_filtering);
  si.SetBoolValue("GPU", "ForceProgressiveScan", gpu_force_progressive_scan);
  si.SetBoolValue("GPU", "UseDebugDevice", gpu_use_debug_device);

  si.SetBoolValue("Display", "LinearFiltering", display_linear_filtering);
  si.SetBoolValue("Display", "Fullscreen", display_fullscreen);
  si.SetBoolValue("Display", "VSync", video_sync_enabled);

  si.SetStringValue("Audio", "Backend", GetAudioBackendName(audio_backend));
  si.SetBoolValue("Audio", "Sync", audio_sync_enabled);

  si.SetStringValue("BIOS", "Path", bios_path.c_str());
  si.SetBoolValue("BIOS", "PatchTTYEnable", bios_patch_tty_enable);
  si.SetBoolValue("BIOS", "PatchFastBoot", bios_patch_fast_boot);

  if (controller_types[0] != ControllerType::None)
    si.SetStringValue("Controller1", "Type", GetControllerTypeName(controller_types[0]));
  else
    si.DeleteValue("Controller1", "Type");

  if (controller_types[1] != ControllerType::None)
    si.SetStringValue("Controller2", "Type", GetControllerTypeName(controller_types[1]));
  else
    si.DeleteValue("Controller2", "Type");

  if (!memory_card_paths[0].empty())
    si.SetStringValue("MemoryCards", "Card1Path", memory_card_paths[0].c_str());
  else
    si.DeleteValue("MemoryCards", "Card1Path");

  if (!memory_card_paths[1].empty())
    si.SetStringValue("MemoryCards", "Card2Path", memory_card_paths[1].c_str());
  else
    si.DeleteValue("MemoryCards", "Card2Path");

  si.SetBoolValue("Debug", "ShowVRAM", debugging.show_vram);
  si.SetBoolValue("Debug", "DumpCPUToVRAMCopies", debugging.dump_cpu_to_vram_copies);
  si.SetBoolValue("Debug", "DumpVRAMToCPUCopies", debugging.dump_vram_to_cpu_copies);
  si.SetBoolValue("Debug", "ShowGPUState", debugging.show_gpu_state);
  si.SetBoolValue("Debug", "ShowCDROMState", debugging.show_cdrom_state);
  si.SetBoolValue("Debug", "ShowSPUState", debugging.show_spu_state);
  si.SetBoolValue("Debug", "ShowTimersState", debugging.show_timers_state);
  si.SetBoolValue("Debug", "ShowMDECState", debugging.show_mdec_state);
}

static std::array<const char*, 4> s_console_region_names = {{"Auto", "NTSC-J", "NTSC-U", "PAL"}};
static std::array<const char*, 4> s_console_region_display_names = {
  {"Auto-Detect", "NTSC-J (Japan)", "NTSC-U (US)", "PAL (Europe, Australia)"}};

std::optional<ConsoleRegion> Settings::ParseConsoleRegionName(const char* str)
{
  int index = 0;
  for (const char* name : s_console_region_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<ConsoleRegion>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetConsoleRegionName(ConsoleRegion region)
{
  return s_console_region_names[static_cast<int>(region)];
}

const char* Settings::GetConsoleRegionDisplayName(ConsoleRegion region)
{
  return s_console_region_display_names[static_cast<int>(region)];
}

static std::array<const char*, 3> s_cpu_execution_mode_names = {{"Interpreter", "CachedInterpreter", "Recompiler"}};
static std::array<const char*, 3> s_cpu_execution_mode_display_names = {
  {"Intepreter (Slowest)", "Cached Interpreter (Faster)", "Recompiler (Fastest)"}};

std::optional<CPUExecutionMode> Settings::ParseCPUExecutionMode(const char* str)
{
  u8 index = 0;
  for (const char* name : s_cpu_execution_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<CPUExecutionMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetCPUExecutionModeName(CPUExecutionMode mode)
{
  return s_cpu_execution_mode_names[static_cast<u8>(mode)];
}

const char* Settings::GetCPUExecutionModeDisplayName(CPUExecutionMode mode)
{
  return s_cpu_execution_mode_display_names[static_cast<u8>(mode)];
}

static std::array<const char*, 3> s_gpu_renderer_names = {{
#ifdef WIN32
  "D3D11",
#endif
  "OpenGL", "Software"}};
static std::array<const char*, 3> s_gpu_renderer_display_names = {{
#ifdef WIN32
  "Hardware (D3D11)",
#endif
  "Hardware (OpenGL)", "Software"}};

std::optional<GPURenderer> Settings::ParseRendererName(const char* str)
{
  int index = 0;
  for (const char* name : s_gpu_renderer_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<GPURenderer>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetRendererName(GPURenderer renderer)
{
  return s_gpu_renderer_names[static_cast<int>(renderer)];
}

const char* Settings::GetRendererDisplayName(GPURenderer renderer)
{
  return s_gpu_renderer_display_names[static_cast<int>(renderer)];
}

static std::array<const char*, 3> s_audio_backend_names = {{"Null", "Default", "Cubeb"}};
static std::array<const char*, 3> s_audio_backend_display_names = {{"Null (No Output)", "Default", "Cubeb"}};

std::optional<AudioBackend> Settings::ParseAudioBackend(const char* str)
{
  int index = 0;
  for (const char* name : s_audio_backend_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<AudioBackend>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetAudioBackendName(AudioBackend backend)
{
  return s_audio_backend_names[static_cast<int>(backend)];
}

const char* Settings::GetAudioBackendDisplayName(AudioBackend backend)
{
  return s_audio_backend_display_names[static_cast<int>(backend)];
}

static std::array<const char*, 3> s_controller_type_names = {{"None", "DigitalController", "AnalogController"}};
static std::array<const char*, 3> s_controller_display_names = {
  {"None", "Digital Controller", "Analog Controller (DualShock)"}};

std::optional<ControllerType> Settings::ParseControllerTypeName(const char* str)
{
  int index = 0;
  for (const char* name : s_controller_type_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<ControllerType>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetControllerTypeName(ControllerType type)
{
  return s_controller_type_names[static_cast<int>(type)];
}

const char* Settings::GetControllerTypeDisplayName(ControllerType type)
{
  return s_controller_display_names[static_cast<int>(type)];
}
