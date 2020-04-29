#include "settings.h"
#include "common/string_util.h"
#include <array>

Settings::Settings() = default;

void Settings::Load(SettingsInterface& si)
{
  region =
    ParseConsoleRegionName(si.GetStringValue("Console", "Region", "NTSC-U").c_str()).value_or(ConsoleRegion::NTSC_U);

  emulation_speed = si.GetFloatValue("Main", "EmulationSpeed", 1.0f);
  speed_limiter_enabled = si.GetBoolValue("Main", "SpeedLimiterEnabled", true);
  increase_timer_resolution = si.GetBoolValue("Main", "IncreaseTimerResolution", true);
  start_paused = si.GetBoolValue("Main", "StartPaused", false);
  start_fullscreen = si.GetBoolValue("Main", "StartFullscreen", false);
  save_state_on_exit = si.GetBoolValue("Main", "SaveStateOnExit", true);
  confim_power_off = si.GetBoolValue("Main", "ConfirmPowerOff", true);

  cpu_execution_mode = ParseCPUExecutionMode(si.GetStringValue("CPU", "ExecutionMode", "Interpreter").c_str())
                         .value_or(CPUExecutionMode::Interpreter);

  gpu_renderer = ParseRendererName(si.GetStringValue("GPU", "Renderer", GetRendererName(DEFAULT_GPU_RENDERER)).c_str())
                   .value_or(DEFAULT_GPU_RENDERER);
  gpu_resolution_scale = static_cast<u32>(si.GetIntValue("GPU", "ResolutionScale", 1));
  gpu_use_debug_device = si.GetBoolValue("GPU", "UseDebugDevice", false);
  gpu_true_color = si.GetBoolValue("GPU", "TrueColor", true);
  gpu_scaled_dithering = si.GetBoolValue("GPU", "ScaledDithering", false);
  gpu_texture_filtering = si.GetBoolValue("GPU", "TextureFiltering", false);
  gpu_disable_interlacing = si.GetBoolValue("GPU", "DisableInterlacing", true);
  gpu_force_ntsc_timings = si.GetBoolValue("GPU", "ForceNTSCTimings", false);

  display_crop_mode = ParseDisplayCropMode(
                        si.GetStringValue("Display", "CropMode", GetDisplayCropModeName(DisplayCropMode::None)).c_str())
                        .value_or(DisplayCropMode::None);
  display_aspect_ratio =
    ParseDisplayAspectRatio(
      si.GetStringValue("Display", "AspectRatio", GetDisplayAspectRatioName(DisplayAspectRatio::R4_3)).c_str())
      .value_or(DisplayAspectRatio::R4_3);
  display_linear_filtering = si.GetBoolValue("Display", "LinearFiltering", true);
  display_show_osd_messages = si.GetBoolValue("Display", "ShowOSDMessages", true);
  display_show_fps = si.GetBoolValue("Display", "ShowFPS", false);
  display_show_vps = si.GetBoolValue("Display", "ShowVPS", false);
  display_show_speed = si.GetBoolValue("Display", "ShowSpeed", false);
  video_sync_enabled = si.GetBoolValue("Display", "VSync", true);

  cdrom_read_thread = si.GetBoolValue("CDROM", "ReadThread", true);
  cdrom_region_check = si.GetBoolValue("CDROM", "RegionCheck", true);

  audio_backend =
    ParseAudioBackend(si.GetStringValue("Audio", "Backend", "Cubeb").c_str()).value_or(AudioBackend::Cubeb);
  audio_sync_enabled = si.GetBoolValue("Audio", "Sync", true);
  audio_dump_on_boot = si.GetBoolValue("Audio", "DumpOnBoot", false);

  dma_max_slice_ticks = si.GetIntValue("Hacks", "DMAMaxSliceTicks", 1000);
  dma_halt_ticks = si.GetIntValue("Hacks", "DMAHaltTicks", 100);
  gpu_fifo_size = static_cast<u32>(si.GetIntValue("Hacks", "GPUFIFOSize", 128));
  gpu_max_run_ahead = si.GetIntValue("Hacks", "GPUMaxRunAhead", 128);

  bios_path = si.GetStringValue("BIOS", "Path", "bios/scph1001.bin");
  bios_patch_tty_enable = si.GetBoolValue("BIOS", "PatchTTYEnable", true);
  bios_patch_fast_boot = si.GetBoolValue("BIOS", "PatchFastBoot", false);

  controller_types[0] = ParseControllerTypeName(si.GetStringValue("Controller1", "Type", "DigitalController").c_str())
                          .value_or(ControllerType::DigitalController);
  controller_types[1] =
    ParseControllerTypeName(si.GetStringValue("Controller2", "Type", "None").c_str()).value_or(ControllerType::None);

  // NOTE: The default value here if not present in the config is shared, but SetDefaultSettings() makes per-game.
  // This is so we don't break older builds which had the shared card by default.
  memory_card_types[0] =
    ParseMemoryCardTypeName(
      si.GetStringValue("MemoryCards", "Card1Type", GetMemoryCardTypeName(MemoryCardType::Shared)).c_str())
      .value_or(MemoryCardType::Shared);
  memory_card_paths[0] = si.GetStringValue("MemoryCards", "Card1Path", "memcards/shared_card_1.mcd");
  memory_card_types[1] =
    ParseMemoryCardTypeName(
      si.GetStringValue("MemoryCards", "Card2Type", GetMemoryCardTypeName(MemoryCardType::None)).c_str())
      .value_or(MemoryCardType::None);
  memory_card_paths[1] = si.GetStringValue("MemoryCards", "Card2Path", "memcards/shared_card_2.mcd");

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

  si.SetFloatValue("Main", "EmulationSpeed", emulation_speed);
  si.SetBoolValue("Main", "SpeedLimiterEnabled", speed_limiter_enabled);
  si.SetBoolValue("Main", "IncreaseTimerResolution", increase_timer_resolution);
  si.SetBoolValue("Main", "StartPaused", start_paused);
  si.SetBoolValue("Main", "StartFullscreen", start_fullscreen);
  si.SetBoolValue("Main", "SaveStateOnExit", save_state_on_exit);
  si.SetBoolValue("Main", "ConfirmPowerOff", confim_power_off);

  si.SetStringValue("CPU", "ExecutionMode", GetCPUExecutionModeName(cpu_execution_mode));

  si.SetStringValue("GPU", "Renderer", GetRendererName(gpu_renderer));
  si.SetIntValue("GPU", "ResolutionScale", static_cast<long>(gpu_resolution_scale));
  si.SetBoolValue("GPU", "UseDebugDevice", gpu_use_debug_device);
  si.SetBoolValue("GPU", "TrueColor", gpu_true_color);
  si.SetBoolValue("GPU", "ScaledDithering", gpu_scaled_dithering);
  si.SetBoolValue("GPU", "TextureFiltering", gpu_texture_filtering);
  si.SetBoolValue("GPU", "DisableInterlacing", gpu_disable_interlacing);
  si.SetBoolValue("GPU", "ForceNTSCTimings", gpu_force_ntsc_timings);

  si.SetStringValue("Display", "CropMode", GetDisplayCropModeName(display_crop_mode));
  si.SetStringValue("Display", "AspectRatio", GetDisplayAspectRatioName(display_aspect_ratio));
  si.SetBoolValue("Display", "LinearFiltering", display_linear_filtering);
  si.SetBoolValue("Display", "ShowOSDMessages", display_show_osd_messages);
  si.SetBoolValue("Display", "ShowFPS", display_show_fps);
  si.SetBoolValue("Display", "ShowVPS", display_show_vps);
  si.SetBoolValue("Display", "ShowSpeed", display_show_speed);
  si.SetBoolValue("Display", "VSync", video_sync_enabled);

  si.SetBoolValue("CDROM", "ReadThread", cdrom_read_thread);
  si.SetBoolValue("CDROM", "RegionCheck", cdrom_region_check);

  si.SetStringValue("Audio", "Backend", GetAudioBackendName(audio_backend));
  si.SetBoolValue("Audio", "Sync", audio_sync_enabled);
  si.SetBoolValue("Audio", "DumpOnBoot", audio_dump_on_boot);

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

  si.SetStringValue("MemoryCards", "Card1Type", GetMemoryCardTypeName(memory_card_types[0]));
  si.SetStringValue("MemoryCards", "Card1Path", memory_card_paths[0].c_str());
  si.SetStringValue("MemoryCards", "Card2Type", GetMemoryCardTypeName(memory_card_types[1]));
  si.SetStringValue("MemoryCards", "Card2Path", memory_card_paths[1].c_str());

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

static std::array<const char*, 4> s_disc_region_names = {{"NTSC-J", "NTSC-U", "PAL", "Other"}};
static std::array<const char*, 4> s_disc_region_display_names = {
  {"NTSC-J (Japan)", "NTSC-U (US)", "PAL (Europe, Australia)", "Other"}};

std::optional<DiscRegion> Settings::ParseDiscRegionName(const char* str)
{
  int index = 0;
  for (const char* name : s_console_region_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<DiscRegion>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetDiscRegionName(DiscRegion region)
{
  return s_disc_region_names[static_cast<int>(region)];
}

const char* Settings::GetDiscRegionDisplayName(DiscRegion region)
{
  return s_disc_region_display_names[static_cast<int>(region)];
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

static std::array<const char*, 3> s_display_crop_mode_names = {{"None", "Overscan", "Borders"}};
static std::array<const char*, 3> s_display_crop_mode_display_names = {{"None", "Only Overscan Area", "All Borders"}};

std::optional<DisplayCropMode> Settings::ParseDisplayCropMode(const char* str)
{
  int index = 0;
  for (const char* name : s_display_crop_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<DisplayCropMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetDisplayCropModeName(DisplayCropMode crop_mode)
{
  return s_display_crop_mode_names[static_cast<int>(crop_mode)];
}

const char* Settings::GetDisplayCropModeDisplayName(DisplayCropMode crop_mode)
{
  return s_display_crop_mode_display_names[static_cast<int>(crop_mode)];
}

static std::array<const char*, 3> s_display_aspect_ratio_names = {{"4:3", "16:9", "1:1"}};
static constexpr std::array<float, 3> s_display_aspect_ratio_values = {{4.0f / 3.0f, 16.0f / 9.0f, 1.0f}};

std::optional<DisplayAspectRatio> Settings::ParseDisplayAspectRatio(const char* str)
{
  int index = 0;
  for (const char* name : s_display_aspect_ratio_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<DisplayAspectRatio>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetDisplayAspectRatioName(DisplayAspectRatio ar)
{
  return s_display_aspect_ratio_names[static_cast<int>(ar)];
}

float Settings::GetDisplayAspectRatioValue(DisplayAspectRatio ar)
{
  return s_display_aspect_ratio_values[static_cast<int>(ar)];
}

static std::array<const char*, 3> s_audio_backend_names = {{"Null", "Cubeb", "SDL"}};
static std::array<const char*, 3> s_audio_backend_display_names = {{"Null (No Output)", "Cubeb", "SDL"}};

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

static std::array<const char*, 5> s_controller_type_names = {
  {"None", "DigitalController", "AnalogController", "NamcoGunCon", "PlayStationMouse"}};
static std::array<const char*, 5> s_controller_display_names = {
  {"None", "Digital Controller", "Analog Controller (DualShock)", "Namco GunCon", "PlayStation Mouse"}};

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

static std::array<const char*, 4> s_memory_card_type_names = {{"None", "Shared", "PerGame", "PerGameTitle"}};
static std::array<const char*, 4> s_memory_card_type_display_names = {{"No Memory Card", "Shared Between All Games",
                                                                       "Separate Card Per Game (Game Code)",
                                                                       "Seperate Card Per Game (Game Title)"}};

std::optional<MemoryCardType> Settings::ParseMemoryCardTypeName(const char* str)
{
  int index = 0;
  for (const char* name : s_memory_card_type_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<MemoryCardType>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetMemoryCardTypeName(MemoryCardType type)
{
  return s_memory_card_type_names[static_cast<int>(type)];
}

const char* Settings::GetMemoryCardTypeDisplayName(MemoryCardType type)
{
  return s_memory_card_type_display_names[static_cast<int>(type)];
}
