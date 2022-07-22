#include "settings.h"
#include "achievements.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/make_array.h"
#include "common/path.h"
#include "common/string_util.h"
#include "controller.h"
#include "host.h"
#include "host_display.h"
#include "host_settings.h"
#include "system.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <numeric>
Log_SetChannel(Settings);

Settings g_settings;

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

Settings::Settings()
{
  controller_types[0] = DEFAULT_CONTROLLER_1_TYPE;
  memory_card_types[0] = DEFAULT_MEMORY_CARD_1_TYPE;
  for (u32 i = 1; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    controller_types[i] = DEFAULT_CONTROLLER_2_TYPE;
    memory_card_types[i] = DEFAULT_MEMORY_CARD_2_TYPE;
  }
}

bool Settings::HasAnyPerGameMemoryCards() const
{
  return std::any_of(memory_card_types.begin(), memory_card_types.end(), [](MemoryCardType t) {
    return (t == MemoryCardType::PerGame || t == MemoryCardType::PerGameTitle);
  });
}

std::array<TinyString, NUM_CONTROLLER_AND_CARD_PORTS> Settings::GeneratePortLabels() const
{
  static constexpr std::array<std::array<bool, NUM_MULTITAPS>, static_cast<size_t>(MultitapMode::Count)>
    multitap_enabled_on_port = {{{false, false}, {true, false}, {false, true}, {true, true}}};

  std::array<TinyString, NUM_CONTROLLER_AND_CARD_PORTS> labels;

  u32 logical_port = 0;
  for (u32 physical_port = 0; physical_port < NUM_MULTITAPS; physical_port++)
  {
    if (multitap_enabled_on_port[static_cast<size_t>(multitap_mode)][physical_port])
    {
      for (u32 i = 0; i < 4; i++)
      {
        labels[logical_port] = TinyString::FromFormat("Port %u%c", physical_port + 1u, 'A' + i);
        logical_port++;
      }
    }
    else
    {
      labels[logical_port] = TinyString::FromFormat("Port %u", physical_port + 1u);
      logical_port++;
    }
  }

  return labels;
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
    ParseConsoleRegionName(
      si.GetStringValue("Console", "Region", Settings::GetConsoleRegionName(Settings::DEFAULT_CONSOLE_REGION)).c_str())
      .value_or(DEFAULT_CONSOLE_REGION);
  enable_8mb_ram = si.GetBoolValue("Console", "Enable8MBRAM", false);

  emulation_speed = si.GetFloatValue("Main", "EmulationSpeed", 1.0f);
  fast_forward_speed = si.GetFloatValue("Main", "FastForwardSpeed", 0.0f);
  turbo_speed = si.GetFloatValue("Main", "TurboSpeed", 0.0f);
  sync_to_host_refresh_rate = si.GetBoolValue("Main", "SyncToHostRefreshRate", false);
  increase_timer_resolution = si.GetBoolValue("Main", "IncreaseTimerResolution", true);
  inhibit_screensaver = si.GetBoolValue("Main", "InhibitScreensaver", true);
  start_paused = si.GetBoolValue("Main", "StartPaused", false);
  start_fullscreen = si.GetBoolValue("Main", "StartFullscreen", false);
  pause_on_focus_loss = si.GetBoolValue("Main", "PauseOnFocusLoss", false);
  pause_on_menu = si.GetBoolValue("Main", "PauseOnMenu", true);
  save_state_on_exit = si.GetBoolValue("Main", "SaveStateOnExit", true);
  create_save_state_backups = si.GetBoolValue("Main", "CreateSaveStateBackups", true);
  confim_power_off = si.GetBoolValue("Main", "ConfirmPowerOff", true);
  load_devices_from_save_states = si.GetBoolValue("Main", "LoadDevicesFromSaveStates", false);
  apply_compatibility_settings = si.GetBoolValue("Main", "ApplyCompatibilitySettings", true);
  apply_game_settings = si.GetBoolValue("Main", "ApplyGameSettings", true);
  auto_load_cheats = si.GetBoolValue("Main", "AutoLoadCheats", true);
  disable_all_enhancements = si.GetBoolValue("Main", "DisableAllEnhancements", false);
  rewind_enable = si.GetBoolValue("Main", "RewindEnable", false);
  rewind_save_frequency = si.GetFloatValue("Main", "RewindFrequency", 10.0f);
  rewind_save_slots = static_cast<u32>(si.GetIntValue("Main", "RewindSaveSlots", 10));
  runahead_frames = static_cast<u32>(si.GetIntValue("Main", "RunaheadFrameCount", 0));

  cpu_execution_mode =
    ParseCPUExecutionMode(
      si.GetStringValue("CPU", "ExecutionMode", GetCPUExecutionModeName(DEFAULT_CPU_EXECUTION_MODE)).c_str())
      .value_or(DEFAULT_CPU_EXECUTION_MODE);
  cpu_overclock_numerator = std::max(si.GetIntValue("CPU", "OverclockNumerator", 1), 1);
  cpu_overclock_denominator = std::max(si.GetIntValue("CPU", "OverclockDenominator", 1), 1);
  cpu_overclock_enable = si.GetBoolValue("CPU", "OverclockEnable", false);
  UpdateOverclockActive();
  cpu_recompiler_memory_exceptions = si.GetBoolValue("CPU", "RecompilerMemoryExceptions", false);
  cpu_recompiler_block_linking = si.GetBoolValue("CPU", "RecompilerBlockLinking", true);
  cpu_recompiler_icache = si.GetBoolValue("CPU", "RecompilerICache", false);
  cpu_fastmem_mode = ParseCPUFastmemMode(
                       si.GetStringValue("CPU", "FastmemMode", GetCPUFastmemModeName(DEFAULT_CPU_FASTMEM_MODE)).c_str())
                       .value_or(DEFAULT_CPU_FASTMEM_MODE);

  gpu_renderer = ParseRendererName(si.GetStringValue("GPU", "Renderer", GetRendererName(DEFAULT_GPU_RENDERER)).c_str())
                   .value_or(DEFAULT_GPU_RENDERER);
  gpu_adapter = si.GetStringValue("GPU", "Adapter", "");
  gpu_resolution_scale = static_cast<u32>(si.GetIntValue("GPU", "ResolutionScale", 1));
  gpu_multisamples = static_cast<u32>(si.GetIntValue("GPU", "Multisamples", 1));
  gpu_use_debug_device = si.GetBoolValue("GPU", "UseDebugDevice", false);
  gpu_per_sample_shading = si.GetBoolValue("GPU", "PerSampleShading", false);
  gpu_use_thread = si.GetBoolValue("GPU", "UseThread", true);
  gpu_use_software_renderer_for_readbacks = si.GetBoolValue("GPU", "UseSoftwareRendererForReadbacks", false);
  gpu_threaded_presentation = si.GetBoolValue("GPU", "ThreadedPresentation", true);
  gpu_true_color = si.GetBoolValue("GPU", "TrueColor", true);
  gpu_scaled_dithering = si.GetBoolValue("GPU", "ScaledDithering", true);
  gpu_texture_filter =
    ParseTextureFilterName(
      si.GetStringValue("GPU", "TextureFilter", GetTextureFilterName(DEFAULT_GPU_TEXTURE_FILTER)).c_str())
      .value_or(DEFAULT_GPU_TEXTURE_FILTER);
  gpu_downsample_mode =
    ParseDownsampleModeName(
      si.GetStringValue("GPU", "DownsampleMode", GetDownsampleModeName(DEFAULT_GPU_DOWNSAMPLE_MODE)).c_str())
      .value_or(DEFAULT_GPU_DOWNSAMPLE_MODE);
  gpu_disable_interlacing = si.GetBoolValue("GPU", "DisableInterlacing", true);
  gpu_force_ntsc_timings = si.GetBoolValue("GPU", "ForceNTSCTimings", false);
  gpu_widescreen_hack = si.GetBoolValue("GPU", "WidescreenHack", false);
  gpu_24bit_chroma_smoothing = si.GetBoolValue("GPU", "ChromaSmoothing24Bit", false);
  gpu_pgxp_enable = si.GetBoolValue("GPU", "PGXPEnable", false);
  gpu_pgxp_culling = si.GetBoolValue("GPU", "PGXPCulling", true);
  gpu_pgxp_texture_correction = si.GetBoolValue("GPU", "PGXPTextureCorrection", true);
  gpu_pgxp_vertex_cache = si.GetBoolValue("GPU", "PGXPVertexCache", false);
  gpu_pgxp_cpu = si.GetBoolValue("GPU", "PGXPCPU", false);
  gpu_pgxp_preserve_proj_fp = si.GetBoolValue("GPU", "PGXPPreserveProjFP", false);
  gpu_pgxp_tolerance = si.GetFloatValue("GPU", "PGXPTolerance", -1.0f);
  gpu_pgxp_depth_buffer = si.GetBoolValue("GPU", "PGXPDepthBuffer", false);
  SetPGXPDepthClearThreshold(si.GetFloatValue("GPU", "PGXPDepthClearThreshold", DEFAULT_GPU_PGXP_DEPTH_THRESHOLD));

  display_crop_mode =
    ParseDisplayCropMode(
      si.GetStringValue("Display", "CropMode", GetDisplayCropModeName(DEFAULT_DISPLAY_CROP_MODE)).c_str())
      .value_or(DEFAULT_DISPLAY_CROP_MODE);
  display_aspect_ratio =
    ParseDisplayAspectRatio(
      si.GetStringValue("Display", "AspectRatio", GetDisplayAspectRatioName(DEFAULT_DISPLAY_ASPECT_RATIO)).c_str())
      .value_or(DEFAULT_DISPLAY_ASPECT_RATIO);
  display_aspect_ratio_custom_numerator = static_cast<u16>(
    std::clamp<int>(si.GetIntValue("Display", "CustomAspectRatioNumerator", 4), 1, std::numeric_limits<u16>::max()));
  display_aspect_ratio_custom_denominator = static_cast<u16>(
    std::clamp<int>(si.GetIntValue("Display", "CustomAspectRatioDenominator", 3), 1, std::numeric_limits<u16>::max()));
  display_force_4_3_for_24bit = si.GetBoolValue("Display", "Force4_3For24Bit", false);
  display_active_start_offset = static_cast<s16>(si.GetIntValue("Display", "ActiveStartOffset", 0));
  display_active_end_offset = static_cast<s16>(si.GetIntValue("Display", "ActiveEndOffset", 0));
  display_line_start_offset = static_cast<s8>(si.GetIntValue("Display", "LineStartOffset", 0));
  display_line_end_offset = static_cast<s8>(si.GetIntValue("Display", "LineEndOffset", 0));
  display_linear_filtering = si.GetBoolValue("Display", "LinearFiltering", true);
  display_integer_scaling = si.GetBoolValue("Display", "IntegerScaling", false);
  display_stretch = si.GetBoolValue("Display", "Stretch", false);
  display_post_processing = si.GetBoolValue("Display", "PostProcessing", false);
  display_show_osd_messages = si.GetBoolValue("Display", "ShowOSDMessages", true);
  display_show_fps = si.GetBoolValue("Display", "ShowFPS", false);
  display_show_speed = si.GetBoolValue("Display", "ShowSpeed", false);
  display_show_resolution = si.GetBoolValue("Display", "ShowResolution", false);
  display_show_cpu = si.GetBoolValue("Display", "ShowCPU", false);
  display_show_status_indicators = si.GetBoolValue("Display", "ShowStatusIndicators", true);
  display_show_inputs = si.GetBoolValue("Display", "ShowInputs", false);
  display_show_enhancements = si.GetBoolValue("Display", "ShowEnhancements", false);
  display_all_frames = si.GetBoolValue("Display", "DisplayAllFrames", false);
  display_internal_resolution_screenshots = si.GetBoolValue("Display", "InternalResolutionScreenshots", false);
  video_sync_enabled = si.GetBoolValue("Display", "VSync", DEFAULT_VSYNC_VALUE);
  display_post_process_chain = si.GetStringValue("Display", "PostProcessChain", "");
  display_max_fps = si.GetFloatValue("Display", "MaxFPS", DEFAULT_DISPLAY_MAX_FPS);
  display_osd_scale = si.GetFloatValue("Display", "OSDScale", DEFAULT_OSD_SCALE);

  cdrom_readahead_sectors =
    static_cast<u8>(si.GetIntValue("CDROM", "ReadaheadSectors", DEFAULT_CDROM_READAHEAD_SECTORS));
  cdrom_region_check = si.GetBoolValue("CDROM", "RegionCheck", false);
  cdrom_load_image_to_ram = si.GetBoolValue("CDROM", "LoadImageToRAM", false);
  cdrom_mute_cd_audio = si.GetBoolValue("CDROM", "MuteCDAudio", false);
  cdrom_read_speedup = si.GetIntValue("CDROM", "ReadSpeedup", 1);
  cdrom_seek_speedup = si.GetIntValue("CDROM", "SeekSpeedup", 1);

  audio_backend =
    ParseAudioBackend(si.GetStringValue("Audio", "Backend", GetAudioBackendName(DEFAULT_AUDIO_BACKEND)).c_str())
      .value_or(DEFAULT_AUDIO_BACKEND);
  audio_output_volume = si.GetIntValue("Audio", "OutputVolume", 100);
  audio_fast_forward_volume = si.GetIntValue("Audio", "FastForwardVolume", 100);
  audio_buffer_size = si.GetIntValue("Audio", "BufferSize", DEFAULT_AUDIO_BUFFER_SIZE);
  audio_resampling = si.GetBoolValue("Audio", "Resampling", true);
  audio_output_muted = si.GetBoolValue("Audio", "OutputMuted", false);
  audio_sync_enabled = si.GetBoolValue("Audio", "Sync", true);
  audio_dump_on_boot = si.GetBoolValue("Audio", "DumpOnBoot", false);

  dma_max_slice_ticks = si.GetIntValue("Hacks", "DMAMaxSliceTicks", DEFAULT_DMA_MAX_SLICE_TICKS);
  dma_halt_ticks = si.GetIntValue("Hacks", "DMAHaltTicks", DEFAULT_DMA_HALT_TICKS);
  gpu_fifo_size = static_cast<u32>(si.GetIntValue("Hacks", "GPUFIFOSize", DEFAULT_GPU_FIFO_SIZE));
  gpu_max_run_ahead = si.GetIntValue("Hacks", "GPUMaxRunAhead", DEFAULT_GPU_MAX_RUN_AHEAD);

  bios_patch_tty_enable = si.GetBoolValue("BIOS", "PatchTTYEnable", false);
  bios_patch_fast_boot = si.GetBoolValue("BIOS", "PatchFastBoot", DEFAULT_FAST_BOOT_VALUE);

  multitap_mode =
    ParseMultitapModeName(
      si.GetStringValue("ControllerPorts", "MultitapMode", GetMultitapModeName(DEFAULT_MULTITAP_MODE)).c_str())
      .value_or(DEFAULT_MULTITAP_MODE);

  controller_types[0] = ParseControllerTypeName(si.GetStringValue(Controller::GetSettingsSection(0).c_str(), "Type",
                                                                  GetControllerTypeName(DEFAULT_CONTROLLER_1_TYPE))
                                                  .c_str())
                          .value_or(DEFAULT_CONTROLLER_1_TYPE);

  const std::array<bool, 2> mtap_enabled = {{IsPort1MultitapEnabled(), IsPort2MultitapEnabled()}};
  for (u32 i = 1; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    // Ignore types when multitap not enabled
    const auto [port, slot] = Controller::ConvertPadToPortAndSlot(i);
    if (Controller::PadIsMultitapSlot(slot) && !mtap_enabled[port])
    {
      controller_types[i] = ControllerType::None;
      continue;
    }

    controller_types[i] = ParseControllerTypeName(si.GetStringValue(Controller::GetSettingsSection(i).c_str(), "Type",
                                                                    GetControllerTypeName(DEFAULT_CONTROLLER_2_TYPE))
                                                    .c_str())
                            .value_or(DEFAULT_CONTROLLER_2_TYPE);
  }

  memory_card_types[0] =
    ParseMemoryCardTypeName(
      si.GetStringValue("MemoryCards", "Card1Type", GetMemoryCardTypeName(DEFAULT_MEMORY_CARD_1_TYPE)).c_str())
      .value_or(DEFAULT_MEMORY_CARD_1_TYPE);
  memory_card_types[1] =
    ParseMemoryCardTypeName(
      si.GetStringValue("MemoryCards", "Card2Type", GetMemoryCardTypeName(DEFAULT_MEMORY_CARD_2_TYPE)).c_str())
      .value_or(DEFAULT_MEMORY_CARD_2_TYPE);
  memory_card_paths[0] = si.GetStringValue("MemoryCards", "Card1Path", "");
  memory_card_paths[1] = si.GetStringValue("MemoryCards", "Card2Path", "");
  memory_card_use_playlist_title = si.GetBoolValue("MemoryCards", "UsePlaylistTitle", true);

  achievements_enabled = si.GetBoolValue("Cheevos", "Enabled", false);
  achievements_test_mode = si.GetBoolValue("Cheevos", "TestMode", false);
  achievements_unofficial_test_mode = si.GetBoolValue("Cheevos", "UnofficialTestMode", false);
  achievements_use_first_disc_from_playlist = si.GetBoolValue("Cheevos", "UseFirstDiscFromPlaylist", true);
  achievements_rich_presence = si.GetBoolValue("Cheevos", "RichPresence", true);
  achievements_challenge_mode = si.GetBoolValue("Cheevos", "ChallengeMode", false);

  log_level = ParseLogLevelName(si.GetStringValue("Logging", "LogLevel", GetLogLevelName(DEFAULT_LOG_LEVEL)).c_str())
                .value_or(DEFAULT_LOG_LEVEL);
  log_filter = si.GetStringValue("Logging", "LogFilter", "");
  log_to_console = si.GetBoolValue("Logging", "LogToConsole", DEFAULT_LOG_TO_CONSOLE);
  log_to_debug = si.GetBoolValue("Logging", "LogToDebug", false);
  log_to_window = si.GetBoolValue("Logging", "LogToWindow", false);
  log_to_file = si.GetBoolValue("Logging", "LogToFile", false);

  debugging.show_vram = si.GetBoolValue("Debug", "ShowVRAM");
  debugging.dump_cpu_to_vram_copies = si.GetBoolValue("Debug", "DumpCPUToVRAMCopies");
  debugging.dump_vram_to_cpu_copies = si.GetBoolValue("Debug", "DumpVRAMToCPUCopies");
  debugging.enable_gdb_server = si.GetBoolValue("Debug", "EnableGDBServer");
  debugging.gdb_server_port = static_cast<u16>(si.GetIntValue("Debug", "GDBServerPort"));
  debugging.show_gpu_state = si.GetBoolValue("Debug", "ShowGPUState");
  debugging.show_cdrom_state = si.GetBoolValue("Debug", "ShowCDROMState");
  debugging.show_spu_state = si.GetBoolValue("Debug", "ShowSPUState");
  debugging.show_timers_state = si.GetBoolValue("Debug", "ShowTimersState");
  debugging.show_mdec_state = si.GetBoolValue("Debug", "ShowMDECState");
  debugging.show_dma_state = si.GetBoolValue("Debug", "ShowDMAState");

  texture_replacements.enable_vram_write_replacements =
    si.GetBoolValue("TextureReplacements", "EnableVRAMWriteReplacements", false);
  texture_replacements.preload_textures = si.GetBoolValue("TextureReplacements", "PreloadTextures", false);
  texture_replacements.dump_vram_writes = si.GetBoolValue("TextureReplacements", "DumpVRAMWrites", false);
  texture_replacements.dump_vram_write_force_alpha_channel =
    si.GetBoolValue("TextureReplacements", "DumpVRAMWriteForceAlphaChannel", true);
  texture_replacements.dump_vram_write_width_threshold =
    si.GetIntValue("TextureReplacements", "DumpVRAMWriteWidthThreshold", 128);
  texture_replacements.dump_vram_write_height_threshold =
    si.GetIntValue("TextureReplacements", "DumpVRAMWriteHeightThreshold", 128);
}

void Settings::Save(SettingsInterface& si) const
{
  si.SetStringValue("Console", "Region", GetConsoleRegionName(region));
  si.SetBoolValue("Console", "Enable8MBRAM", enable_8mb_ram);

  si.SetFloatValue("Main", "EmulationSpeed", emulation_speed);
  si.SetFloatValue("Main", "FastForwardSpeed", fast_forward_speed);
  si.SetFloatValue("Main", "TurboSpeed", turbo_speed);
  si.SetBoolValue("Main", "SyncToHostRefreshRate", sync_to_host_refresh_rate);
  si.SetBoolValue("Main", "IncreaseTimerResolution", increase_timer_resolution);
  si.SetBoolValue("Main", "InhibitScreensaver", inhibit_screensaver);
  si.SetBoolValue("Main", "StartPaused", start_paused);
  si.SetBoolValue("Main", "StartFullscreen", start_fullscreen);
  si.SetBoolValue("Main", "PauseOnFocusLoss", pause_on_focus_loss);
  si.SetBoolValue("Main", "PauseOnMenu", pause_on_menu);
  si.SetBoolValue("Main", "SaveStateOnExit", save_state_on_exit);
  si.SetBoolValue("Main", "CreateSaveStateBackups", create_save_state_backups);
  si.SetBoolValue("Main", "ConfirmPowerOff", confim_power_off);
  si.SetBoolValue("Main", "LoadDevicesFromSaveStates", load_devices_from_save_states);
  si.SetBoolValue("Main", "ApplyCompatibilitySettings", apply_compatibility_settings);
  si.SetBoolValue("Main", "ApplyGameSettings", apply_game_settings);
  si.SetBoolValue("Main", "AutoLoadCheats", auto_load_cheats);
  si.SetBoolValue("Main", "DisableAllEnhancements", disable_all_enhancements);
  si.SetBoolValue("Main", "RewindEnable", rewind_enable);
  si.SetFloatValue("Main", "RewindFrequency", rewind_save_frequency);
  si.SetIntValue("Main", "RewindSaveSlots", rewind_save_slots);
  si.SetIntValue("Main", "RunaheadFrameCount", runahead_frames);

  si.SetStringValue("CPU", "ExecutionMode", GetCPUExecutionModeName(cpu_execution_mode));
  si.SetBoolValue("CPU", "OverclockEnable", cpu_overclock_enable);
  si.SetIntValue("CPU", "OverclockNumerator", cpu_overclock_numerator);
  si.SetIntValue("CPU", "OverclockDenominator", cpu_overclock_denominator);
  si.SetBoolValue("CPU", "RecompilerMemoryExceptions", cpu_recompiler_memory_exceptions);
  si.SetBoolValue("CPU", "RecompilerBlockLinking", cpu_recompiler_block_linking);
  si.SetBoolValue("CPU", "RecompilerICache", cpu_recompiler_icache);
  si.SetStringValue("CPU", "FastmemMode", GetCPUFastmemModeName(cpu_fastmem_mode));

  si.SetStringValue("GPU", "Renderer", GetRendererName(gpu_renderer));
  si.SetStringValue("GPU", "Adapter", gpu_adapter.c_str());
  si.SetIntValue("GPU", "ResolutionScale", static_cast<long>(gpu_resolution_scale));
  si.SetIntValue("GPU", "Multisamples", static_cast<long>(gpu_multisamples));
  si.SetBoolValue("GPU", "UseDebugDevice", gpu_use_debug_device);
  si.SetBoolValue("GPU", "PerSampleShading", gpu_per_sample_shading);
  si.SetBoolValue("GPU", "UseThread", gpu_use_thread);
  si.SetBoolValue("GPU", "ThreadedPresentation", gpu_threaded_presentation);
  si.SetBoolValue("GPU", "UseSoftwareRendererForReadbacks", gpu_use_software_renderer_for_readbacks);
  si.SetBoolValue("GPU", "TrueColor", gpu_true_color);
  si.SetBoolValue("GPU", "ScaledDithering", gpu_scaled_dithering);
  si.SetStringValue("GPU", "TextureFilter", GetTextureFilterName(gpu_texture_filter));
  si.SetStringValue("GPU", "DownsampleMode", GetDownsampleModeName(gpu_downsample_mode));
  si.SetBoolValue("GPU", "DisableInterlacing", gpu_disable_interlacing);
  si.SetBoolValue("GPU", "ForceNTSCTimings", gpu_force_ntsc_timings);
  si.SetBoolValue("GPU", "WidescreenHack", gpu_widescreen_hack);
  si.SetBoolValue("GPU", "ChromaSmoothing24Bit", gpu_24bit_chroma_smoothing);
  si.SetBoolValue("GPU", "PGXPEnable", gpu_pgxp_enable);
  si.SetBoolValue("GPU", "PGXPCulling", gpu_pgxp_culling);
  si.SetBoolValue("GPU", "PGXPTextureCorrection", gpu_pgxp_texture_correction);
  si.SetBoolValue("GPU", "PGXPVertexCache", gpu_pgxp_vertex_cache);
  si.SetBoolValue("GPU", "PGXPCPU", gpu_pgxp_cpu);
  si.SetBoolValue("GPU", "PGXPPreserveProjFP", gpu_pgxp_preserve_proj_fp);
  si.SetFloatValue("GPU", "PGXPTolerance", gpu_pgxp_tolerance);
  si.SetBoolValue("GPU", "PGXPDepthBuffer", gpu_pgxp_depth_buffer);
  si.SetFloatValue("GPU", "PGXPDepthClearThreshold", GetPGXPDepthClearThreshold());

  si.SetStringValue("Display", "CropMode", GetDisplayCropModeName(display_crop_mode));
  si.SetIntValue("Display", "ActiveStartOffset", display_active_start_offset);
  si.SetIntValue("Display", "ActiveEndOffset", display_active_end_offset);
  si.SetIntValue("Display", "LineStartOffset", display_line_start_offset);
  si.SetIntValue("Display", "LineEndOffset", display_line_end_offset);
  si.SetBoolValue("Display", "Force4_3For24Bit", display_force_4_3_for_24bit);
  si.SetStringValue("Display", "AspectRatio", GetDisplayAspectRatioName(display_aspect_ratio));
  si.SetIntValue("Display", "CustomAspectRatioNumerator", display_aspect_ratio_custom_numerator);
  si.GetIntValue("Display", "CustomAspectRatioDenominator", display_aspect_ratio_custom_denominator);
  si.SetBoolValue("Display", "LinearFiltering", display_linear_filtering);
  si.SetBoolValue("Display", "IntegerScaling", display_integer_scaling);
  si.SetBoolValue("Display", "Stretch", display_stretch);
  si.SetBoolValue("Display", "PostProcessing", display_post_processing);
  si.SetBoolValue("Display", "ShowOSDMessages", display_show_osd_messages);
  si.SetBoolValue("Display", "ShowFPS", display_show_fps);
  si.SetBoolValue("Display", "ShowSpeed", display_show_speed);
  si.SetBoolValue("Display", "ShowResolution", display_show_resolution);
  si.SetBoolValue("Display", "ShowCPU", display_show_cpu);
  si.SetBoolValue("Display", "ShowStatusIndicators", display_show_status_indicators);
  si.SetBoolValue("Display", "ShowInputs", display_show_inputs);
  si.SetBoolValue("Display", "ShowEnhancements", display_show_enhancements);
  si.SetBoolValue("Display", "DisplayAllFrames", display_all_frames);
  si.SetBoolValue("Display", "InternalResolutionScreenshots", display_internal_resolution_screenshots);
  si.SetBoolValue("Display", "VSync", video_sync_enabled);
  if (display_post_process_chain.empty())
    si.DeleteValue("Display", "PostProcessChain");
  else
    si.SetStringValue("Display", "PostProcessChain", display_post_process_chain.c_str());
  si.SetFloatValue("Display", "MaxFPS", display_max_fps);
  si.SetFloatValue("Display", "OSDScale", display_osd_scale);

  si.SetIntValue("CDROM", "ReadaheadSectors", cdrom_readahead_sectors);
  si.SetBoolValue("CDROM", "RegionCheck", cdrom_region_check);
  si.SetBoolValue("CDROM", "LoadImageToRAM", cdrom_load_image_to_ram);
  si.SetBoolValue("CDROM", "MuteCDAudio", cdrom_mute_cd_audio);
  si.SetIntValue("CDROM", "ReadSpeedup", cdrom_read_speedup);
  si.SetIntValue("CDROM", "SeekSpeedup", cdrom_seek_speedup);

  si.SetStringValue("Audio", "Backend", GetAudioBackendName(audio_backend));
  si.SetIntValue("Audio", "OutputVolume", audio_output_volume);
  si.SetIntValue("Audio", "FastForwardVolume", audio_fast_forward_volume);
  si.SetIntValue("Audio", "BufferSize", audio_buffer_size);
  si.SetBoolValue("Audio", "Resampling", audio_resampling);
  si.SetBoolValue("Audio", "OutputMuted", audio_output_muted);
  si.SetBoolValue("Audio", "Sync", audio_sync_enabled);
  si.SetBoolValue("Audio", "DumpOnBoot", audio_dump_on_boot);

  si.SetIntValue("Hacks", "DMAMaxSliceTicks", dma_max_slice_ticks);
  si.SetIntValue("Hacks", "DMAHaltTicks", dma_halt_ticks);
  si.SetIntValue("Hacks", "GPUFIFOSize", gpu_fifo_size);
  si.SetIntValue("Hacks", "GPUMaxRunAhead", gpu_max_run_ahead);

  si.SetBoolValue("BIOS", "PatchTTYEnable", bios_patch_tty_enable);
  si.SetBoolValue("BIOS", "PatchFastBoot", bios_patch_fast_boot);

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    si.SetStringValue(TinyString::FromFormat("Controller%u", i + 1u), "Type",
                      GetControllerTypeName(controller_types[i]));
  }

  si.SetStringValue("MemoryCards", "Card1Type", GetMemoryCardTypeName(memory_card_types[0]));
  si.SetStringValue("MemoryCards", "Card2Type", GetMemoryCardTypeName(memory_card_types[1]));
  if (!memory_card_paths[0].empty())
    si.SetStringValue("MemoryCards", "Card1Path", memory_card_paths[0].c_str());
  else
    si.DeleteValue("MemoryCards", "Card1Path");

  if (!memory_card_paths[1].empty())
    si.SetStringValue("MemoryCards", "Card2Path", memory_card_paths[1].c_str());
  else
    si.DeleteValue("MemoryCards", "Card2Path");

  si.SetBoolValue("MemoryCards", "UsePlaylistTitle", memory_card_use_playlist_title);

  si.SetStringValue("ControllerPorts", "MultitapMode", GetMultitapModeName(multitap_mode));

  si.SetBoolValue("Cheevos", "Enabled", achievements_enabled);
  si.SetBoolValue("Cheevos", "TestMode", achievements_test_mode);
  si.SetBoolValue("Cheevos", "UnofficialTestMode", achievements_unofficial_test_mode);
  si.SetBoolValue("Cheevos", "UseFirstDiscFromPlaylist", achievements_use_first_disc_from_playlist);
  si.SetBoolValue("Cheevos", "RichPresence", achievements_rich_presence);
  si.SetBoolValue("Cheevos", "ChallengeMode", achievements_challenge_mode);

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

  si.SetBoolValue("TextureReplacements", "EnableVRAMWriteReplacements",
                  texture_replacements.enable_vram_write_replacements);
  si.SetBoolValue("TextureReplacements", "PreloadTextures", texture_replacements.preload_textures);
  si.SetBoolValue("TextureReplacements", "DumpVRAMWrites", texture_replacements.dump_vram_writes);
  si.SetBoolValue("TextureReplacements", "DumpVRAMWriteForceAlphaChannel",
                  texture_replacements.dump_vram_write_force_alpha_channel);
  si.SetIntValue("TextureReplacements", "DumpVRAMWriteWidthThreshold",
                 texture_replacements.dump_vram_write_width_threshold);
  si.SetIntValue("TextureReplacements", "DumpVRAMWriteHeightThreshold",
                 texture_replacements.dump_vram_write_height_threshold);
}

void Settings::FixIncompatibleSettings(bool display_osd_messages)
{
  if (g_settings.disable_all_enhancements)
  {
    Log_WarningPrintf("All enhancements disabled by config setting.");
    g_settings.cpu_overclock_enable = false;
    g_settings.cpu_overclock_active = false;
    g_settings.enable_8mb_ram = false;
    g_settings.gpu_resolution_scale = 1;
    g_settings.gpu_multisamples = 1;
    g_settings.gpu_per_sample_shading = false;
    g_settings.gpu_true_color = false;
    g_settings.gpu_scaled_dithering = false;
    g_settings.gpu_texture_filter = GPUTextureFilter::Nearest;
    g_settings.gpu_disable_interlacing = false;
    g_settings.gpu_force_ntsc_timings = false;
    g_settings.gpu_widescreen_hack = false;
    g_settings.gpu_pgxp_enable = false;
    g_settings.gpu_24bit_chroma_smoothing = false;
    g_settings.cdrom_read_speedup = 1;
    g_settings.cdrom_seek_speedup = 1;
    g_settings.cdrom_mute_cd_audio = false;
    g_settings.texture_replacements.enable_vram_write_replacements = false;
    g_settings.bios_patch_fast_boot = false;
    g_settings.bios_patch_tty_enable = false;
  }

  if (g_settings.display_integer_scaling && g_settings.display_linear_filtering)
  {
    Log_WarningPrintf("Disabling linear filter due to integer upscaling.");
    g_settings.display_linear_filtering = false;
  }

  if (g_settings.display_integer_scaling && g_settings.display_stretch)
  {
    Log_WarningPrintf("Disabling stretch due to integer upscaling.");
    g_settings.display_stretch = false;
  }

  if (g_settings.gpu_pgxp_enable)
  {
    if (g_settings.gpu_renderer == GPURenderer::Software)
    {
      if (display_osd_messages)
      {
        Host::AddOSDMessage(
          Host::TranslateStdString("OSDMessage", "PGXP is incompatible with the software renderer, disabling PGXP."),
          10.0f);
      }
      g_settings.gpu_pgxp_enable = false;
    }
  }

#ifndef WITH_MMAP_FASTMEM
  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::MMap)
  {
    Log_WarningPrintf("mmap fastmem is not available on this platform, using LUT instead.");
    g_settings.cpu_fastmem_mode = CPUFastmemMode::LUT;
  }
#endif

#if defined(__ANDROID__) && defined(__arm__) && !defined(__aarch64__) && !defined(_M_ARM64)
  if (g_settings.rewind_enable)
  {
    Host::AddOSDMessage(Host::TranslateStdString("OSDMessage", "Rewind is not supported on 32-bit ARM for Android."),
                        30.0f);
    g_settings.rewind_enable = false;
  }
#endif

  // if challenge mode is enabled, disable things like rewind since they use save states
  if (Achievements::ChallengeModeActive())
  {
    g_settings.emulation_speed =
      (g_settings.emulation_speed != 0.0f) ? std::max(g_settings.emulation_speed, 1.0f) : 0.0f;
    g_settings.fast_forward_speed =
      (g_settings.fast_forward_speed != 0.0f) ? std::max(g_settings.fast_forward_speed, 1.0f) : 0.0f;
    g_settings.turbo_speed = (g_settings.turbo_speed != 0.0f) ? std::max(g_settings.turbo_speed, 1.0f) : 0.0f;
    g_settings.rewind_enable = false;
    g_settings.auto_load_cheats = false;
    if (g_settings.cpu_overclock_enable && g_settings.GetCPUOverclockPercent() < 100)
    {
      g_settings.cpu_overclock_enable = false;
      g_settings.UpdateOverclockActive();
    }
    g_settings.debugging.enable_gdb_server = false;
    g_settings.debugging.show_vram = false;
    g_settings.debugging.show_gpu_state = false;
    g_settings.debugging.show_cdrom_state = false;
    g_settings.debugging.show_spu_state = false;
    g_settings.debugging.show_timers_state = false;
    g_settings.debugging.show_mdec_state = false;
    g_settings.debugging.show_dma_state = false;
    g_settings.debugging.dump_cpu_to_vram_copies = false;
    g_settings.debugging.dump_vram_to_cpu_copies = false;
  }
}

static std::array<const char*, LOGLEVEL_COUNT> s_log_level_names = {
  {"None", "Error", "Warning", "Perf", "Info", "Verbose", "Dev", "Profile", "Debug", "Trace"}};
static std::array<const char*, LOGLEVEL_COUNT> s_log_level_display_names = {
  {TRANSLATABLE("LogLevel", "None"), TRANSLATABLE("LogLevel", "Error"), TRANSLATABLE("LogLevel", "Warning"),
   TRANSLATABLE("LogLevel", "Performance"), TRANSLATABLE("LogLevel", "Information"),
   TRANSLATABLE("LogLevel", "Verbose"), TRANSLATABLE("LogLevel", "Developer"), TRANSLATABLE("LogLevel", "Profile"),
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
  {TRANSLATABLE("CPUExecutionMode", "Interpreter (Slowest)"),
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

static std::array<const char*, static_cast<u32>(CPUFastmemMode::Count)> s_cpu_fastmem_mode_names = {
  {"Disabled", "MMap", "LUT"}};
static std::array<const char*, static_cast<u32>(CPUFastmemMode::Count)> s_cpu_fastmem_mode_display_names = {
  {TRANSLATABLE("CPUFastmemMode", "Disabled (Slowest)"),
   TRANSLATABLE("CPUFastmemMode", "MMap (Hardware, Fastest, 64-Bit Only)"),
   TRANSLATABLE("CPUFastmemMode", "LUT (Faster)")}};

std::optional<CPUFastmemMode> Settings::ParseCPUFastmemMode(const char* str)
{
  u8 index = 0;
  for (const char* name : s_cpu_fastmem_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<CPUFastmemMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetCPUFastmemModeName(CPUFastmemMode mode)
{
  return s_cpu_fastmem_mode_names[static_cast<u8>(mode)];
}

const char* Settings::GetCPUFastmemModeDisplayName(CPUFastmemMode mode)
{
  return s_cpu_fastmem_mode_display_names[static_cast<u8>(mode)];
}

static constexpr auto s_gpu_renderer_names = make_array(
#ifdef _WIN32
  "D3D11", "D3D12",
#endif
  "Vulkan", "OpenGL", "Software");
static constexpr auto s_gpu_renderer_display_names = make_array(
#ifdef _WIN32
  TRANSLATABLE("GPURenderer", "Hardware (D3D11)"), TRANSLATABLE("GPURenderer", "Hardware (D3D12)"),
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

static constexpr auto s_texture_filter_names =
  make_array("Nearest", "Bilinear", "BilinearBinAlpha", "JINC2", "JINC2BinAlpha", "xBR", "xBRBinAlpha");
static constexpr auto s_texture_filter_display_names =
  make_array(TRANSLATABLE("GPUTextureFilter", "Nearest-Neighbor"), TRANSLATABLE("GPUTextureFilter", "Bilinear"),
             TRANSLATABLE("GPUTextureFilter", "Bilinear (No Edge Blending)"), TRANSLATABLE("GPUTextureFilter", "JINC2"),
             TRANSLATABLE("GPUTextureFilter", "JINC2 (No Edge Blending)"), TRANSLATABLE("GPUTextureFilter", "xBR"),
             TRANSLATABLE("GPUTextureFilter", "xBR (No Edge Blending)"));

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

static constexpr auto s_downsample_mode_names = make_array("Disabled", "Box", "Adaptive");
static constexpr auto s_downsample_mode_display_names = make_array(
  TRANSLATABLE("GPUDownsampleMode", "Disabled"), TRANSLATABLE("GPUDownsampleMode", "Box (Downsample 3D/Smooth All)"),
  TRANSLATABLE("GPUDownsampleMode", "Adaptive (Preserve 3D/Smooth 2D)"));

std::optional<GPUDownsampleMode> Settings::ParseDownsampleModeName(const char* str)
{
  int index = 0;
  for (const char* name : s_downsample_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<GPUDownsampleMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetDownsampleModeName(GPUDownsampleMode mode)
{
  return s_downsample_mode_names[static_cast<int>(mode)];
}

const char* Settings::GetDownsampleModeDisplayName(GPUDownsampleMode mode)
{
  return s_downsample_mode_display_names[static_cast<int>(mode)];
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

static std::array<const char*, static_cast<size_t>(DisplayAspectRatio::Count)> s_display_aspect_ratio_names = {
  {TRANSLATABLE("DisplayAspectRatio", "Auto (Game Native)"), TRANSLATABLE("DisplayAspectRatio", "Auto (Match Window)"),
   TRANSLATABLE("DisplayAspectRatio", "Custom"), "4:3", "16:9", "19:9", "20:9", "PAR 1:1"}};
static constexpr std::array<float, static_cast<size_t>(DisplayAspectRatio::Count)> s_display_aspect_ratio_values = {
  {-1.0f, -1.0f, -1.0f, 4.0f / 3.0f, 16.0f / 9.0f, 19.0f / 9.0f, 20.0f / 9.0f, -1.0f}};

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

float Settings::GetDisplayAspectRatioValue() const
{
  switch (display_aspect_ratio)
  {
    case DisplayAspectRatio::MatchWindow:
    {
      if (!g_host_display)
        return s_display_aspect_ratio_values[static_cast<int>(DEFAULT_DISPLAY_ASPECT_RATIO)];

      const u32 width = g_host_display->GetWindowWidth();
      const u32 height = g_host_display->GetWindowHeight() - g_host_display->GetDisplayTopMargin();
      return static_cast<float>(width) / static_cast<float>(height);
    }

    case DisplayAspectRatio::Custom:
    {
      return static_cast<float>(display_aspect_ratio_custom_numerator) /
             static_cast<float>(display_aspect_ratio_custom_denominator);
    }

    default:
    {
      return s_display_aspect_ratio_values[static_cast<int>(display_aspect_ratio)];
    }
  }
}

static const auto s_audio_backend_names = make_array("Null", "Cubeb"
#ifdef _WIN32
                                                     ,
                                                     "XAudio2"
#endif
#ifndef ANDROID
                                                     ,
                                                     "SDL"
#else
                                                     ,
                                                     "OpenSLES"
#endif
);
static const auto s_audio_backend_display_names =
  make_array(TRANSLATABLE("AudioBackend", "Null (No Output)"), TRANSLATABLE("AudioBackend", "Cubeb")
#ifdef _WIN32
                                                                 ,
             TRANSLATABLE("AudioBackend", "XAudio2")
#endif
#ifndef ANDROID
               ,
             TRANSLATABLE("AudioBackend", "SDL")
#else
                                                                 ,
             TRANSLATABLE("AudioBackend", "OpenSL ES")
#endif
  );

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

static std::array<const char*, 7> s_controller_type_names = {
  {"None", "DigitalController", "AnalogController", "AnalogJoystick", "GunCon", "PlayStationMouse", "NeGcon"}};
static std::array<const char*, 7> s_controller_display_names = {
  {TRANSLATABLE("ControllerType", "None"), TRANSLATABLE("ControllerType", "Digital Controller"),
   TRANSLATABLE("ControllerType", "Analog Controller (DualShock)"), TRANSLATABLE("ControllerType", "Analog Joystick"),
   TRANSLATABLE("ControllerType", "GunCon"), TRANSLATABLE("ControllerType", "PlayStation Mouse"),
   TRANSLATABLE("ControllerType", "NeGcon")}};

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

static std::array<const char*, 6> s_memory_card_type_names = {
  {"None", "Shared", "PerGame", "PerGameTitle", "PerGameFileTitle", "NonPersistent"}};
static std::array<const char*, 6> s_memory_card_type_display_names = {
  {TRANSLATABLE("MemoryCardType", "No Memory Card"), TRANSLATABLE("MemoryCardType", "Shared Between All Games"),
   TRANSLATABLE("MemoryCardType", "Separate Card Per Game (Game Code)"),
   TRANSLATABLE("MemoryCardType", "Separate Card Per Game (Game Title)"),
   TRANSLATABLE("MemoryCardType", "Separate Card Per Game (File Title)"),
   TRANSLATABLE("MemoryCardType", "Non-Persistent Card (Do Not Save)")}};

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

std::string Settings::GetDefaultSharedMemoryCardName(u32 slot)
{
  return fmt::format("shared_card_{}.mcd", slot + 1);
}

std::string Settings::GetSharedMemoryCardPath(u32 slot) const
{
  std::string ret;

  if (memory_card_paths[slot].empty())
    ret = Path::Combine(EmuFolders::MemoryCards, GetDefaultSharedMemoryCardName(slot));
  else if (!Path::IsAbsolute(memory_card_paths[slot]))
    ret = Path::Combine(EmuFolders::MemoryCards, memory_card_paths[slot]);
  else
    ret = memory_card_paths[slot];

  return ret;
}

std::string Settings::GetGameMemoryCardPath(const char* game_code, u32 slot)
{
  return Path::Combine(EmuFolders::MemoryCards, fmt::format("{}_{}.mcd", game_code, slot + 1));
}

static std::array<const char*, 4> s_multitap_enable_mode_names = {{"Disabled", "Port1Only", "Port2Only", "BothPorts"}};
static std::array<const char*, 4> s_multitap_enable_mode_display_names = {
  {TRANSLATABLE("MultitapMode", "Disabled"), TRANSLATABLE("MultitapMode", "Enable on Port 1 Only"),
   TRANSLATABLE("MultitapMode", "Enable on Port 2 Only"), TRANSLATABLE("MultitapMode", "Enable on Ports 1 and 2")}};

std::optional<MultitapMode> Settings::ParseMultitapModeName(const char* str)
{
  u32 index = 0;
  for (const char* name : s_multitap_enable_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<MultitapMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetMultitapModeName(MultitapMode mode)
{
  return s_multitap_enable_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetMultitapModeDisplayName(MultitapMode mode)
{
  return s_multitap_enable_mode_display_names[static_cast<size_t>(mode)];
}

std::string EmuFolders::AppRoot;
std::string EmuFolders::DataRoot;
std::string EmuFolders::Bios;
std::string EmuFolders::Cache;
std::string EmuFolders::Cheats;
std::string EmuFolders::Covers;
std::string EmuFolders::Dumps;
std::string EmuFolders::GameSettings;
std::string EmuFolders::InputProfiles;
std::string EmuFolders::MemoryCards;
std::string EmuFolders::Resources;
std::string EmuFolders::SaveStates;
std::string EmuFolders::Screenshots;
std::string EmuFolders::Shaders;
std::string EmuFolders::Textures;

void EmuFolders::SetDefaults()
{
  Bios = Path::Combine(DataRoot, "bios");
  Cache = Path::Combine(DataRoot, "cache");
  Cheats = Path::Combine(DataRoot, "cheats");
  Covers = Path::Combine(DataRoot, "covers");
  Dumps = Path::Combine(DataRoot, "dump");
  GameSettings = Path::Combine(DataRoot, "gamesettings");
  InputProfiles = Path::Combine(DataRoot, "inputprofiles");
  MemoryCards = Path::Combine(DataRoot, "memcards");
  SaveStates = Path::Combine(DataRoot, "savestates");
  Screenshots = Path::Combine(DataRoot, "screenshots");
  Shaders = Path::Combine(DataRoot, "shaders");
  Textures = Path::Combine(DataRoot, "textures");
}

static std::string LoadPathFromSettings(SettingsInterface& si, const std::string& root, const char* section,
                                        const char* name, const char* def)
{
  std::string value = si.GetStringValue(section, name, def);
  if (value.empty())
    value = def;
  if (!Path::IsAbsolute(value))
    value = Path::Combine(root, value);
  return value;
}

void EmuFolders::LoadConfig(SettingsInterface& si)
{
  Bios = LoadPathFromSettings(si, DataRoot, "BIOS", "SearchDirectory", "bios");
  Cache = LoadPathFromSettings(si, DataRoot, "Folders", "Cache", "cache");
  Cheats = LoadPathFromSettings(si, DataRoot, "Folders", "Cheats", "cheats");
  Covers = LoadPathFromSettings(si, DataRoot, "Folders", "Covers", "covers");
  Dumps = LoadPathFromSettings(si, DataRoot, "Folders", "Dumps", "dump");
  GameSettings = LoadPathFromSettings(si, DataRoot, "Folders", "GameSettings", "gamesettings");
  InputProfiles = LoadPathFromSettings(si, DataRoot, "Folders", "InputProfiles", "inputprofiles");
  MemoryCards = LoadPathFromSettings(si, DataRoot, "MemoryCards", "Directory", "memcards");
  SaveStates = LoadPathFromSettings(si, DataRoot, "Folders", "Savestates", "savestates");
  Screenshots = LoadPathFromSettings(si, DataRoot, "Folders", "Snapshots", "screenshots");
  Shaders = LoadPathFromSettings(si, DataRoot, "Folders", "Snapshots", "shaders");
  Textures = LoadPathFromSettings(si, DataRoot, "Folders", "Textures", "textures");

  Log_DevPrintf("BIOS Directory: %s", Bios.c_str());
  Log_DevPrintf("Cache Directory: %s", Cache.c_str());
  Log_DevPrintf("Cheats Directory: %s", Cheats.c_str());
  Log_DevPrintf("Covers Directory: %s", Covers.c_str());
  Log_DevPrintf("Dumps Directory: %s", Dumps.c_str());
  Log_DevPrintf("Game Settings Directory: %s", GameSettings.c_str());
  Log_DevPrintf("Input Profile Directory: %s", InputProfiles.c_str());
  Log_DevPrintf("MemoryCards Directory: %s", MemoryCards.c_str());
  Log_DevPrintf("SaveStates Directory: %s", SaveStates.c_str());
  Log_DevPrintf("Screenshots Directory: %s", Screenshots.c_str());
  Log_DevPrintf("Shaders Directory: %s", Shaders.c_str());
  Log_DevPrintf("Textures Directory: %s", Textures.c_str());
}

void EmuFolders::Save(SettingsInterface& si)
{
  // convert back to relative
  si.SetStringValue("BIOS", "SearchDirectory", Path::MakeRelative(Bios, DataRoot).c_str());
  si.SetStringValue("Folders", "Cache", Path::MakeRelative(Cache, DataRoot).c_str());
  si.SetStringValue("Folders", "Cheats", Path::MakeRelative(Cheats, DataRoot).c_str());
  si.SetStringValue("Folders", "Covers", Path::MakeRelative(Covers, DataRoot).c_str());
  si.SetStringValue("Folders", "Dumps", Path::MakeRelative(Dumps, DataRoot).c_str());
  si.SetStringValue("Folders", "GameSettings", Path::MakeRelative(Dumps, GameSettings).c_str());
  si.SetStringValue("Folders", "InputProfiles", Path::MakeRelative(InputProfiles, DataRoot).c_str());
  si.SetStringValue("MemoryCards", "Directory", Path::MakeRelative(MemoryCards, DataRoot).c_str());
  si.SetStringValue("Folders", "SaveStates", Path::MakeRelative(SaveStates, DataRoot).c_str());
  si.SetStringValue("Folders", "Screenshots", Path::MakeRelative(Screenshots, DataRoot).c_str());
  si.SetStringValue("Folders", "Shaders", Path::MakeRelative(Shaders, DataRoot).c_str());
  si.SetStringValue("Folders", "Textures", Path::MakeRelative(Textures, DataRoot).c_str());
}

void EmuFolders::Update()
{
  const std::string old_gamesettings(EmuFolders::GameSettings);
  const std::string old_inputprofiles(EmuFolders::InputProfiles);
  const std::string old_memorycards(EmuFolders::MemoryCards);

  // have to manually grab the lock here, because of the ReloadGameSettings() below.
  {
    auto lock = Host::GetSettingsLock();
    LoadConfig(*Host::Internal::GetBaseSettingsLayer());
    EnsureFoldersExist();
  }

  if (old_gamesettings != EmuFolders::GameSettings || old_inputprofiles != EmuFolders::InputProfiles)
    System::ReloadGameSettings(false);

  if (System::IsValid() && old_memorycards != EmuFolders::MemoryCards)
    System::UpdateMemoryCardTypes();
}

bool EmuFolders::EnsureFoldersExist()
{
  bool result = FileSystem::EnsureDirectoryExists(Bios.c_str(), false);
  result = FileSystem::EnsureDirectoryExists(Cache.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Path::Combine(Cache, "achievement_badge").c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Path::Combine(Cache, "achievement_gameicon").c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Cheats.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Covers.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Dumps.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Path::Combine(Dumps, "audio").c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Path::Combine(Dumps, "textures").c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(GameSettings.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(InputProfiles.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(MemoryCards.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(SaveStates.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Screenshots.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Shaders.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Textures.c_str(), false) && result;
  return result;
}
