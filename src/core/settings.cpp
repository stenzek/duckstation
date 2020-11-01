#include "settings.h"
#include "common/file_system.h"
#include "common/make_array.h"
#include "common/string_util.h"
#include "host_interface.h"
#include <algorithm>
#include <array>
#include <numeric>

Settings g_settings;

SettingsInterface::~SettingsInterface() = default;

const char* SettingInfo::StringDefaultValue() const
{
  return default_value ? default_value : "";
}

bool SettingInfo::BooleanDefaultValue() const
{
  return default_value ? StringUtil::FromChars<bool>(default_value).value_or(false) : false;
}

s32 SettingInfo::IntegerDefaultValue() const
{
  return default_value ? StringUtil::FromChars<s32>(default_value).value_or(0) : 0;
}

s32 SettingInfo::IntegerMinValue() const
{
  static constexpr s32 fallback_value = std::numeric_limits<s32>::min();
  return min_value ? StringUtil::FromChars<s32>(min_value).value_or(fallback_value) : fallback_value;
}

s32 SettingInfo::IntegerMaxValue() const
{
  static constexpr s32 fallback_value = std::numeric_limits<s32>::max();
  return max_value ? StringUtil::FromChars<s32>(max_value).value_or(fallback_value) : fallback_value;
}

s32 SettingInfo::IntegerStepValue() const
{
  static constexpr s32 fallback_value = 1;
  return step_value ? StringUtil::FromChars<s32>(step_value).value_or(fallback_value) : fallback_value;
}

float SettingInfo::FloatDefaultValue() const
{
  return default_value ? StringUtil::FromChars<float>(default_value).value_or(0.0f) : 0.0f;
}

float SettingInfo::FloatMinValue() const
{
  static constexpr float fallback_value = std::numeric_limits<float>::min();
  return min_value ? StringUtil::FromChars<float>(min_value).value_or(fallback_value) : fallback_value;
}

float SettingInfo::FloatMaxValue() const
{
  static constexpr float fallback_value = std::numeric_limits<float>::max();
  return max_value ? StringUtil::FromChars<float>(max_value).value_or(fallback_value) : fallback_value;
}

float SettingInfo::FloatStepValue() const
{
  static constexpr float fallback_value = 0.1f;
  return step_value ? StringUtil::FromChars<float>(step_value).value_or(fallback_value) : fallback_value;
}

Settings::Settings() = default;

bool Settings::HasAnyPerGameMemoryCards() const
{
  return std::any_of(memory_card_types.begin(), memory_card_types.end(), [](MemoryCardType t) {
    return (t == MemoryCardType::PerGame || t == MemoryCardType::PerGameTitle);
  });
}

void Settings::CPUOverclockPercentToFraction(u32 percent, u32* numerator, u32* denominator)
{
  const u32 percent_gcd = std::gcd(percent, 100);
  *numerator = percent / percent_gcd;
  *denominator = 100u / percent_gcd;
}

u32 Settings::CPUOverclockFractionToPercent(u32 numerator, u32 denominator)
{
  return (numerator * 100u) / denominator;
}

void Settings::SetCPUOverclockPercent(u32 percent)
{
  CPUOverclockPercentToFraction(percent, &cpu_overclock_numerator, &cpu_overclock_denominator);
}

u32 Settings::GetCPUOverclockPercent() const
{
  return CPUOverclockFractionToPercent(cpu_overclock_numerator, cpu_overclock_denominator);
}

void Settings::UpdateOverclockActive()
{
  cpu_overclock_active = (cpu_overclock_enable && (cpu_overclock_numerator != 1 || cpu_overclock_denominator != 1));
}

void Settings::Load(SettingsInterface& si)
{
  region =
    ParseConsoleRegionName(si.GetStringValue("Console", "Region", "NTSC-U").c_str()).value_or(DEFAULT_CONSOLE_REGION);

  emulation_speed = si.GetFloatValue("Main", "EmulationSpeed", 1.0f);
  speed_limiter_enabled = si.GetBoolValue("Main", "SpeedLimiterEnabled", true);
  increase_timer_resolution = si.GetBoolValue("Main", "IncreaseTimerResolution", true);
  start_paused = si.GetBoolValue("Main", "StartPaused", false);
  start_fullscreen = si.GetBoolValue("Main", "StartFullscreen", false);
  save_state_on_exit = si.GetBoolValue("Main", "SaveStateOnExit", true);
  confim_power_off = si.GetBoolValue("Main", "ConfirmPowerOff", true);
  load_devices_from_save_states = si.GetBoolValue("Main", "LoadDevicesFromSaveStates", false);
  apply_game_settings = si.GetBoolValue("Main", "ApplyGameSettings", true);
  auto_load_cheats = si.GetBoolValue("Main", "AutoLoadCheats", false);

  cpu_execution_mode =
    ParseCPUExecutionMode(
      si.GetStringValue("CPU", "ExecutionMode", GetCPUExecutionModeName(DEFAULT_CPU_EXECUTION_MODE)).c_str())
      .value_or(DEFAULT_CPU_EXECUTION_MODE);
  cpu_overclock_numerator = std::max(si.GetIntValue("CPU", "OverclockNumerator", 1), 1);
  cpu_overclock_denominator = std::max(si.GetIntValue("CPU", "OverclockDenominator", 1), 1);
  cpu_overclock_enable = si.GetBoolValue("CPU", "OverclockEnable", false);
  UpdateOverclockActive();
  cpu_recompiler_memory_exceptions = si.GetBoolValue("CPU", "RecompilerMemoryExceptions", false);
  cpu_recompiler_icache = si.GetBoolValue("CPU", "RecompilerICache", false);
  cpu_fastmem = si.GetBoolValue("CPU", "Fastmem", true);

  gpu_renderer = ParseRendererName(si.GetStringValue("GPU", "Renderer", GetRendererName(DEFAULT_GPU_RENDERER)).c_str())
                   .value_or(DEFAULT_GPU_RENDERER);
  gpu_adapter = si.GetStringValue("GPU", "Adapter", "");
  gpu_resolution_scale = static_cast<u32>(si.GetIntValue("GPU", "ResolutionScale", 1));
  gpu_multisamples = static_cast<u32>(si.GetIntValue("GPU", "Multisamples", 1));
  gpu_use_debug_device = si.GetBoolValue("GPU", "UseDebugDevice", false);
  gpu_per_sample_shading = si.GetBoolValue("GPU", "PerSampleShading", false);
  gpu_true_color = si.GetBoolValue("GPU", "TrueColor", true);
  gpu_scaled_dithering = si.GetBoolValue("GPU", "ScaledDithering", false);
  gpu_texture_filter =
    ParseTextureFilterName(
      si.GetStringValue("GPU", "TextureFilter", GetTextureFilterName(DEFAULT_GPU_TEXTURE_FILTER)).c_str())
      .value_or(DEFAULT_GPU_TEXTURE_FILTER);
  gpu_disable_interlacing = si.GetBoolValue("GPU", "DisableInterlacing", false);
  gpu_force_ntsc_timings = si.GetBoolValue("GPU", "ForceNTSCTimings", false);
  gpu_widescreen_hack = si.GetBoolValue("GPU", "WidescreenHack", false);
  gpu_pgxp_enable = si.GetBoolValue("GPU", "PGXPEnable", false);
  gpu_pgxp_culling = si.GetBoolValue("GPU", "PGXPCulling", true);
  gpu_pgxp_texture_correction = si.GetBoolValue("GPU", "PGXPTextureCorrection", true);
  gpu_pgxp_vertex_cache = si.GetBoolValue("GPU", "PGXPVertexCache", false);
  gpu_pgxp_cpu = si.GetBoolValue("GPU", "PGXPCPU", false);
  gpu_pgxp_preserve_proj_fp = si.GetBoolValue("GPU", "PGXPPreserveProjFP", false);

  display_crop_mode =
    ParseDisplayCropMode(
      si.GetStringValue("Display", "CropMode", GetDisplayCropModeName(DEFAULT_DISPLAY_CROP_MODE)).c_str())
      .value_or(DEFAULT_DISPLAY_CROP_MODE);
  display_aspect_ratio =
    ParseDisplayAspectRatio(
      si.GetStringValue("Display", "AspectRatio", GetDisplayAspectRatioName(DEFAULT_DISPLAY_ASPECT_RATIO)).c_str())
      .value_or(DEFAULT_DISPLAY_ASPECT_RATIO);
  display_force_4_3_for_24bit = si.GetBoolValue("Display", "Force4_3For24Bit", false);
  display_active_start_offset = static_cast<s16>(si.GetIntValue("Display", "ActiveStartOffset", 0));
  display_active_end_offset = static_cast<s16>(si.GetIntValue("Display", "ActiveEndOffset", 0));
  display_linear_filtering = si.GetBoolValue("Display", "LinearFiltering", true);
  display_integer_scaling = si.GetBoolValue("Display", "IntegerScaling", false);
  display_post_processing = si.GetBoolValue("Display", "PostProcessing", false);
  display_show_osd_messages = si.GetBoolValue("Display", "ShowOSDMessages", true);
  display_show_fps = si.GetBoolValue("Display", "ShowFPS", false);
  display_show_vps = si.GetBoolValue("Display", "ShowVPS", false);
  display_show_speed = si.GetBoolValue("Display", "ShowSpeed", false);
  display_show_resolution = si.GetBoolValue("Display", "ShowResolution", false);
  video_sync_enabled = si.GetBoolValue("Display", "VSync", true);
  display_post_process_chain = si.GetStringValue("Display", "PostProcessChain", "");

  cdrom_read_thread = si.GetBoolValue("CDROM", "ReadThread", true);
  cdrom_region_check = si.GetBoolValue("CDROM", "RegionCheck", true);
  cdrom_load_image_to_ram = si.GetBoolValue("CDROM", "LoadImageToRAM", false);
  cdrom_mute_cd_audio = si.GetBoolValue("CDROM", "MuteCDAudio", false);
  cdrom_read_speedup = si.GetIntValue("CDROM", "ReadSpeedup", 1);

  audio_backend =
    ParseAudioBackend(si.GetStringValue("Audio", "Backend", GetAudioBackendName(DEFAULT_AUDIO_BACKEND)).c_str())
      .value_or(DEFAULT_AUDIO_BACKEND);
  audio_output_volume = si.GetIntValue("Audio", "OutputVolume", 100);
  audio_buffer_size = si.GetIntValue("Audio", "BufferSize", HostInterface::DEFAULT_AUDIO_BUFFER_SIZE);
  audio_output_muted = si.GetBoolValue("Audio", "OutputMuted", false);
  audio_sync_enabled = si.GetBoolValue("Audio", "Sync", true);
  audio_dump_on_boot = si.GetBoolValue("Audio", "DumpOnBoot", false);

  dma_max_slice_ticks = si.GetIntValue("Hacks", "DMAMaxSliceTicks", DEFAULT_DMA_MAX_SLICE_TICKS);
  dma_halt_ticks = si.GetIntValue("Hacks", "DMAHaltTicks", DEFAULT_DMA_HALT_TICKS);
  gpu_fifo_size = static_cast<u32>(si.GetIntValue("Hacks", "GPUFIFOSize", DEFAULT_GPU_FIFO_SIZE));
  gpu_max_run_ahead = si.GetIntValue("Hacks", "GPUMaxRunAhead", DEFAULT_GPU_MAX_RUN_AHEAD);

  bios_patch_tty_enable = si.GetBoolValue("BIOS", "PatchTTYEnable", false);
  bios_patch_fast_boot = si.GetBoolValue("BIOS", "PatchFastBoot", false);

  controller_types[0] =
    ParseControllerTypeName(
      si.GetStringValue("Controller1", "Type", GetControllerTypeName(DEFAULT_CONTROLLER_1_TYPE)).c_str())
      .value_or(DEFAULT_CONTROLLER_1_TYPE);
  controller_types[1] =
    ParseControllerTypeName(
      si.GetStringValue("Controller2", "Type", GetControllerTypeName(DEFAULT_CONTROLLER_2_TYPE)).c_str())
      .value_or(DEFAULT_CONTROLLER_2_TYPE);

  memory_card_types[0] =
    ParseMemoryCardTypeName(
      si.GetStringValue("MemoryCards", "Card1Type", GetMemoryCardTypeName(DEFAULT_MEMORY_CARD_1_TYPE)).c_str())
      .value_or(DEFAULT_MEMORY_CARD_1_TYPE);
  memory_card_paths[0] =
    si.GetStringValue("MemoryCards", "Card1Path", "memcards" FS_OSPATH_SEPARATOR_STR "shared_card_1.mcd");
  memory_card_types[1] =
    ParseMemoryCardTypeName(
      si.GetStringValue("MemoryCards", "Card2Type", GetMemoryCardTypeName(DEFAULT_MEMORY_CARD_2_TYPE)).c_str())
      .value_or(DEFAULT_MEMORY_CARD_2_TYPE);
  memory_card_paths[1] =
    si.GetStringValue("MemoryCards", "Card2Path", "memcards" FS_OSPATH_SEPARATOR_STR "shared_card_2.mcd");
  memory_card_use_playlist_title = si.GetBoolValue("MemoryCards", "UsePlaylistTitle", true);

  log_level = ParseLogLevelName(si.GetStringValue("Logging", "LogLevel", GetLogLevelName(DEFAULT_LOG_LEVEL)).c_str())
                .value_or(DEFAULT_LOG_LEVEL);
  log_filter = si.GetStringValue("Logging", "LogFilter", "");
  log_to_console = si.GetBoolValue("Logging", "LogToConsole", false);
  log_to_debug = si.GetBoolValue("Logging", "LogToDebug", false);
  log_to_window = si.GetBoolValue("Logging", "LogToWindow", false);
  log_to_file = si.GetBoolValue("Logging", "LogToFile", false);

  debugging.show_vram = si.GetBoolValue("Debug", "ShowVRAM");
  debugging.dump_cpu_to_vram_copies = si.GetBoolValue("Debug", "DumpCPUToVRAMCopies");
  debugging.dump_vram_to_cpu_copies = si.GetBoolValue("Debug", "DumpVRAMToCPUCopies");
  debugging.show_gpu_state = si.GetBoolValue("Debug", "ShowGPUState");
  debugging.show_cdrom_state = si.GetBoolValue("Debug", "ShowCDROMState");
  debugging.show_spu_state = si.GetBoolValue("Debug", "ShowSPUState");
  debugging.show_timers_state = si.GetBoolValue("Debug", "ShowTimersState");
  debugging.show_mdec_state = si.GetBoolValue("Debug", "ShowMDECState");
  debugging.show_dma_state = si.GetBoolValue("Debug", "ShowDMAState");
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
  si.SetBoolValue("Main", "LoadDevicesFromSaveStates", load_devices_from_save_states);
  si.SetBoolValue("Main", "ApplyGameSettings", apply_game_settings);
  si.SetBoolValue("Main", "AutoLoadCheats", auto_load_cheats);

  si.SetStringValue("CPU", "ExecutionMode", GetCPUExecutionModeName(cpu_execution_mode));
  si.SetBoolValue("CPU", "OverclockEnable", cpu_overclock_enable);
  si.SetIntValue("CPU", "OverclockNumerator", cpu_overclock_numerator);
  si.SetIntValue("CPU", "OverclockDenominator", cpu_overclock_denominator);
  si.SetBoolValue("CPU", "RecompilerMemoryExceptions", cpu_recompiler_memory_exceptions);
  si.SetBoolValue("CPU", "RecompilerICache", cpu_recompiler_icache);
  si.SetBoolValue("CPU", "Fastmem", cpu_fastmem);

  si.SetStringValue("GPU", "Renderer", GetRendererName(gpu_renderer));
  si.SetStringValue("GPU", "Adapter", gpu_adapter.c_str());
  si.SetIntValue("GPU", "ResolutionScale", static_cast<long>(gpu_resolution_scale));
  si.SetIntValue("GPU", "Multisamples", static_cast<long>(gpu_multisamples));
  si.SetBoolValue("GPU", "UseDebugDevice", gpu_use_debug_device);
  si.SetBoolValue("GPU", "PerSampleShading", gpu_per_sample_shading);
  si.SetBoolValue("GPU", "TrueColor", gpu_true_color);
  si.SetBoolValue("GPU", "ScaledDithering", gpu_scaled_dithering);
  si.SetStringValue("GPU", "TextureFilter", GetTextureFilterName(gpu_texture_filter));
  si.SetBoolValue("GPU", "DisableInterlacing", gpu_disable_interlacing);
  si.SetBoolValue("GPU", "ForceNTSCTimings", gpu_force_ntsc_timings);
  si.SetBoolValue("GPU", "WidescreenHack", gpu_widescreen_hack);
  si.SetBoolValue("GPU", "PGXPEnable", gpu_pgxp_enable);
  si.SetBoolValue("GPU", "PGXPCulling", gpu_pgxp_culling);
  si.SetBoolValue("GPU", "PGXPTextureCorrection", gpu_pgxp_texture_correction);
  si.SetBoolValue("GPU", "PGXPVertexCache", gpu_pgxp_vertex_cache);
  si.SetBoolValue("GPU", "PGXPCPU", gpu_pgxp_cpu);
  si.SetBoolValue("GPU", "PGXPPreserveProjFP", gpu_pgxp_preserve_proj_fp);

  si.SetStringValue("Display", "CropMode", GetDisplayCropModeName(display_crop_mode));
  si.SetIntValue("Display", "ActiveStartOffset", display_active_start_offset);
  si.SetIntValue("Display", "ActiveEndOffset", display_active_end_offset);
  si.SetBoolValue("Display", "Force4_3For24Bit", display_force_4_3_for_24bit);
  si.SetStringValue("Display", "AspectRatio", GetDisplayAspectRatioName(display_aspect_ratio));
  si.SetBoolValue("Display", "LinearFiltering", display_linear_filtering);
  si.SetBoolValue("Display", "IntegerScaling", display_integer_scaling);
  si.SetBoolValue("Display", "PostProcessing", display_post_processing);
  si.SetBoolValue("Display", "ShowOSDMessages", display_show_osd_messages);
  si.SetBoolValue("Display", "ShowFPS", display_show_fps);
  si.SetBoolValue("Display", "ShowVPS", display_show_vps);
  si.SetBoolValue("Display", "ShowSpeed", display_show_speed);
  si.SetBoolValue("Display", "ShowResolution", display_show_speed);
  si.SetBoolValue("Display", "VSync", video_sync_enabled);
  if (display_post_process_chain.empty())
    si.DeleteValue("Display", "PostProcessChain");
  else
    si.SetStringValue("Display", "PostProcessChain", display_post_process_chain.c_str());

  si.SetBoolValue("CDROM", "ReadThread", cdrom_read_thread);
  si.SetBoolValue("CDROM", "RegionCheck", cdrom_region_check);
  si.SetBoolValue("CDROM", "LoadImageToRAM", cdrom_load_image_to_ram);
  si.SetBoolValue("CDROM", "MuteCDAudio", cdrom_mute_cd_audio);
  si.SetIntValue("CDROM", "ReadSpeedup", cdrom_read_speedup);

  si.SetStringValue("Audio", "Backend", GetAudioBackendName(audio_backend));
  si.SetIntValue("Audio", "OutputVolume", audio_output_volume);
  si.SetIntValue("Audio", "BufferSize", audio_buffer_size);
  si.SetBoolValue("Audio", "OutputMuted", audio_output_muted);
  si.SetBoolValue("Audio", "Sync", audio_sync_enabled);
  si.SetBoolValue("Audio", "DumpOnBoot", audio_dump_on_boot);

  si.SetIntValue("Hacks", "DMAMaxSliceTicks", dma_max_slice_ticks);
  si.SetIntValue("Hacks", "DMAHaltTicks", dma_halt_ticks);
  si.SetIntValue("Hacks", "GPUFIFOSize", gpu_fifo_size);
  si.SetIntValue("Hacks", "GPUMaxRunAhead", gpu_max_run_ahead);

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
  si.SetBoolValue("MemoryCards", "UsePlaylistTitle", memory_card_use_playlist_title);

  si.SetStringValue("Logging", "LogLevel", GetLogLevelName(log_level));
  si.SetStringValue("Logging", "LogFilter", log_filter.c_str());
  si.SetBoolValue("Logging", "LogToConsole", log_to_console);
  si.SetBoolValue("Logging", "LogToDebug", log_to_debug);
  si.SetBoolValue("Logging", "LogToWindow", log_to_window);
  si.SetBoolValue("Logging", "LogToFile", log_to_file);

  si.SetBoolValue("Debug", "ShowVRAM", debugging.show_vram);
  si.SetBoolValue("Debug", "DumpCPUToVRAMCopies", debugging.dump_cpu_to_vram_copies);
  si.SetBoolValue("Debug", "DumpVRAMToCPUCopies", debugging.dump_vram_to_cpu_copies);
  si.SetBoolValue("Debug", "ShowGPUState", debugging.show_gpu_state);
  si.SetBoolValue("Debug", "ShowCDROMState", debugging.show_cdrom_state);
  si.SetBoolValue("Debug", "ShowSPUState", debugging.show_spu_state);
  si.SetBoolValue("Debug", "ShowTimersState", debugging.show_timers_state);
  si.SetBoolValue("Debug", "ShowMDECState", debugging.show_mdec_state);
  si.SetBoolValue("Debug", "ShowDMAState", debugging.show_dma_state);
}

static std::array<const char*, LOGLEVEL_COUNT> s_log_level_names = {
  {"None", "Error", "Warning", "Perf", "Success", "Info", "Dev", "Profile", "Debug", "Trace"}};
static std::array<const char*, LOGLEVEL_COUNT> s_log_level_display_names = {
  {TRANSLATABLE("LogLevel", "None"), TRANSLATABLE("LogLevel", "Error"), TRANSLATABLE("LogLevel", "Warning"),
   TRANSLATABLE("LogLevel", "Performance"), TRANSLATABLE("LogLevel", "Success"),
   TRANSLATABLE("LogLevel", "Information"), TRANSLATABLE("LogLevel", "Developer"), TRANSLATABLE("LogLevel", "Profile"),
   TRANSLATABLE("LogLevel", "Debug"), TRANSLATABLE("LogLevel", "Trace")}};

std::optional<LOGLEVEL> Settings::ParseLogLevelName(const char* str)
{
  int index = 0;
  for (const char* name : s_log_level_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<LOGLEVEL>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetLogLevelName(LOGLEVEL level)
{
  return s_log_level_names[static_cast<int>(level)];
}

const char* Settings::GetLogLevelDisplayName(LOGLEVEL level)
{
  return s_log_level_display_names[static_cast<int>(level)];
}

static std::array<const char*, 4> s_console_region_names = {{"Auto", "NTSC-J", "NTSC-U", "PAL"}};
static std::array<const char*, 4> s_console_region_display_names = {
  {TRANSLATABLE("ConsoleRegion", "Auto-Detect"), TRANSLATABLE("ConsoleRegion", "NTSC-J (Japan)"),
   TRANSLATABLE("ConsoleRegion", "NTSC-U/C (US, Canada)"), TRANSLATABLE("ConsoleRegion", "PAL (Europe, Australia)")}};

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
  {TRANSLATABLE("DiscRegion", "NTSC-J (Japan)"), TRANSLATABLE("DiscRegion", "NTSC-U/C (US, Canada)"),
   TRANSLATABLE("DiscRegion", "PAL (Europe, Australia)"), TRANSLATABLE("DiscRegion", "Other")}};

std::optional<DiscRegion> Settings::ParseDiscRegionName(const char* str)
{
  int index = 0;
  for (const char* name : s_disc_region_names)
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
  {TRANSLATABLE("CPUExecutionMode", "Intepreter (Slowest)"),
   TRANSLATABLE("CPUExecutionMode", "Cached Interpreter (Faster)"),
   TRANSLATABLE("CPUExecutionMode", "Recompiler (Fastest)")}};

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

static constexpr auto s_gpu_renderer_names = make_array(
#ifdef WIN32
  "D3D11",
#endif
  "Vulkan", "OpenGL", "Software");
static constexpr auto s_gpu_renderer_display_names = make_array(
#ifdef WIN32
  TRANSLATABLE("GPURenderer", "Hardware (D3D11)"),
#endif
  TRANSLATABLE("GPURenderer", "Hardware (Vulkan)"), TRANSLATABLE("GPURenderer", "Hardware (OpenGL)"),
  TRANSLATABLE("GPURenderer", "Software"));

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

static constexpr auto s_texture_filter_names = make_array("Nearest", "Bilinear", "JINC2", "xBR");
static constexpr auto s_texture_filter_display_names =
  make_array(TRANSLATABLE("GPUTextureFilter", "Nearest-Neighbor"), TRANSLATABLE("GPUTextureFilter", "Bilinear"),
             TRANSLATABLE("GPUTextureFilter", "JINC2"), TRANSLATABLE("GPUTextureFilter", "xBR"));

std::optional<GPUTextureFilter> Settings::ParseTextureFilterName(const char* str)
{
  int index = 0;
  for (const char* name : s_texture_filter_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<GPUTextureFilter>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetTextureFilterName(GPUTextureFilter filter)
{
  return s_texture_filter_names[static_cast<int>(filter)];
}

const char* Settings::GetTextureFilterDisplayName(GPUTextureFilter filter)
{
  return s_texture_filter_display_names[static_cast<int>(filter)];
}

static std::array<const char*, 3> s_display_crop_mode_names = {{"None", "Overscan", "Borders"}};
static std::array<const char*, 3> s_display_crop_mode_display_names = {
  {TRANSLATABLE("DisplayCropMode", "None"), TRANSLATABLE("DisplayCropMode", "Only Overscan Area"),
   TRANSLATABLE("DisplayCropMode", "All Borders")}};

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

static std::array<const char*, 8> s_display_aspect_ratio_names = {
  {"4:3", "16:9", "16:10", "21:9", "8:7", "2:1 (VRAM 1:1)", "1:1", "PAR 1:1"}};
static constexpr std::array<float, 8> s_display_aspect_ratio_values = {
  {4.0f / 3.0f, 16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f, 8.0f / 7.0f, 2.0f / 1.0f, 1.0f, -1.0f}};

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

static std::array<const char*, 3> s_audio_backend_names = {{
  "Null",
  "Cubeb",
#ifndef ANDROID
  "SDL",
#else
  "OpenSLES",
#endif
}};
static std::array<const char*, 3> s_audio_backend_display_names = {{
  TRANSLATABLE("AudioBackend", "Null (No Output)"),
  TRANSLATABLE("AudioBackend", "Cubeb"),
#ifndef ANDROID
  TRANSLATABLE("AudioBackend", "SDL"),
#else
  TRANSLATABLE("AudioBackend", "OpenSL ES"),
#endif
}};

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

static std::array<const char*, 6> s_controller_type_names = {
  {"None", "DigitalController", "AnalogController", "NamcoGunCon", "PlayStationMouse", "NeGcon"}};
static std::array<const char*, 6> s_controller_display_names = {
  {TRANSLATABLE("ControllerType", "None"), TRANSLATABLE("ControllerType", "Digital Controller"),
   TRANSLATABLE("ControllerType", "Analog Controller (DualShock)"), TRANSLATABLE("ControllerType", "Namco GunCon"),
   TRANSLATABLE("ControllerType", "PlayStation Mouse"), TRANSLATABLE("ControllerType", "NeGcon")}};

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
static std::array<const char*, 4> s_memory_card_type_display_names = {
  {TRANSLATABLE("MemoryCardType", "No Memory Card"), TRANSLATABLE("MemoryCardType", "Shared Between All Games"),
   TRANSLATABLE("MemoryCardType", "Separate Card Per Game (Game Code)"),
   TRANSLATABLE("MemoryCardType", "Separate Card Per Game (Game Title)")}};

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
