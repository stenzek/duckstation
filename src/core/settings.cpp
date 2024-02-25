// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "settings.h"
#include "achievements.h"
#include "controller.h"
#include "host.h"
#include "system.h"

#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

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
  save_state_on_exit = si.GetBoolValue("Main", "SaveStateOnExit", true);
  create_save_state_backups = si.GetBoolValue("Main", "CreateSaveStateBackups", DEFAULT_SAVE_STATE_BACKUPS);
  compress_save_states = si.GetBoolValue("Main", "CompressSaveStates", DEFAULT_SAVE_STATE_COMPRESSION);
  confim_power_off = si.GetBoolValue("Main", "ConfirmPowerOff", true);
  load_devices_from_save_states = si.GetBoolValue("Main", "LoadDevicesFromSaveStates", false);
  apply_compatibility_settings = si.GetBoolValue("Main", "ApplyCompatibilitySettings", true);
  apply_game_settings = si.GetBoolValue("Main", "ApplyGameSettings", true);
  auto_load_cheats = si.GetBoolValue("Main", "AutoLoadCheats", true);
  disable_all_enhancements = si.GetBoolValue("Main", "DisableAllEnhancements", false);
  enable_discord_presence = si.GetBoolValue("Main", "EnableDiscordPresence", false);
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
  gpu_disable_shader_cache = si.GetBoolValue("GPU", "DisableShaderCache", false);
  gpu_disable_dual_source_blend = si.GetBoolValue("GPU", "DisableDualSourceBlend", false);
  gpu_disable_framebuffer_fetch = si.GetBoolValue("GPU", "DisableFramebufferFetch", false);
  gpu_disable_texture_buffers = si.GetBoolValue("GPU", "DisableTextureBuffers", false);
  gpu_disable_texture_copy_to_self = si.GetBoolValue("GPU", "DisableTextureCopyToSelf", false);
  gpu_per_sample_shading = si.GetBoolValue("GPU", "PerSampleShading", false);
  gpu_use_thread = si.GetBoolValue("GPU", "UseThread", true);
  gpu_use_software_renderer_for_readbacks = si.GetBoolValue("GPU", "UseSoftwareRendererForReadbacks", false);
  gpu_threaded_presentation = si.GetBoolValue("GPU", "ThreadedPresentation", true);
  gpu_true_color = si.GetBoolValue("GPU", "TrueColor", true);
  gpu_debanding = si.GetBoolValue("GPU", "Debanding", false);
  gpu_scaled_dithering = si.GetBoolValue("GPU", "ScaledDithering", true);
  gpu_texture_filter =
    ParseTextureFilterName(
      si.GetStringValue("GPU", "TextureFilter", GetTextureFilterName(DEFAULT_GPU_TEXTURE_FILTER)).c_str())
      .value_or(DEFAULT_GPU_TEXTURE_FILTER);
  gpu_downsample_mode =
    ParseDownsampleModeName(
      si.GetStringValue("GPU", "DownsampleMode", GetDownsampleModeName(DEFAULT_GPU_DOWNSAMPLE_MODE)).c_str())
      .value_or(DEFAULT_GPU_DOWNSAMPLE_MODE);
  gpu_downsample_scale = static_cast<u8>(si.GetUIntValue("GPU", "DownsampleScale", 1));
  gpu_wireframe_mode =
    ParseGPUWireframeMode(
      si.GetStringValue("GPU", "WireframeMode", GetGPUWireframeModeName(DEFAULT_GPU_WIREFRAME_MODE)).c_str())
      .value_or(DEFAULT_GPU_WIREFRAME_MODE);
  gpu_disable_interlacing = si.GetBoolValue("GPU", "DisableInterlacing", true);
  gpu_force_ntsc_timings = si.GetBoolValue("GPU", "ForceNTSCTimings", false);
  gpu_widescreen_hack = si.GetBoolValue("GPU", "WidescreenHack", false);
  gpu_24bit_chroma_smoothing = si.GetBoolValue("GPU", "ChromaSmoothing24Bit", false);
  gpu_pgxp_enable = si.GetBoolValue("GPU", "PGXPEnable", false);
  gpu_pgxp_culling = si.GetBoolValue("GPU", "PGXPCulling", true);
  gpu_pgxp_texture_correction = si.GetBoolValue("GPU", "PGXPTextureCorrection", true);
  gpu_pgxp_color_correction = si.GetBoolValue("GPU", "PGXPColorCorrection", false);
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
  display_alignment =
    ParseDisplayAlignment(
      si.GetStringValue("Display", "Alignment", GetDisplayAlignmentName(DEFAULT_DISPLAY_ALIGNMENT)).c_str())
      .value_or(DEFAULT_DISPLAY_ALIGNMENT);
  display_scaling =
    ParseDisplayScaling(si.GetStringValue("Display", "Scaling", GetDisplayScalingName(DEFAULT_DISPLAY_SCALING)).c_str())
      .value_or(DEFAULT_DISPLAY_SCALING);
  display_exclusive_fullscreen_control =
    ParseDisplayExclusiveFullscreenControl(
      si.GetStringValue("Display", "ExclusiveFullscreenControl",
                        GetDisplayExclusiveFullscreenControlName(DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL))
        .c_str())
      .value_or(DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL);
  display_force_4_3_for_24bit = si.GetBoolValue("Display", "Force4_3For24Bit", false);
  display_active_start_offset = static_cast<s16>(si.GetIntValue("Display", "ActiveStartOffset", 0));
  display_active_end_offset = static_cast<s16>(si.GetIntValue("Display", "ActiveEndOffset", 0));
  display_line_start_offset = static_cast<s8>(si.GetIntValue("Display", "LineStartOffset", 0));
  display_line_end_offset = static_cast<s8>(si.GetIntValue("Display", "LineEndOffset", 0));
  display_show_osd_messages = si.GetBoolValue("Display", "ShowOSDMessages", true);
  display_show_fps = si.GetBoolValue("Display", "ShowFPS", false);
  display_show_speed = si.GetBoolValue("Display", "ShowSpeed", false);
  display_show_gpu_stats = si.GetBoolValue("Display", "ShowGPUStatistics", false);
  display_show_resolution = si.GetBoolValue("Display", "ShowResolution", false);
  display_show_cpu_usage = si.GetBoolValue("Display", "ShowCPU", false);
  display_show_gpu_usage = si.GetBoolValue("Display", "ShowGPU", false);
  display_show_frame_times = si.GetBoolValue("Display", "ShowFrameTimes", false);
  display_show_status_indicators = si.GetBoolValue("Display", "ShowStatusIndicators", true);
  display_show_inputs = si.GetBoolValue("Display", "ShowInputs", false);
  display_show_enhancements = si.GetBoolValue("Display", "ShowEnhancements", false);
  display_all_frames = si.GetBoolValue("Display", "DisplayAllFrames", false);
  display_internal_resolution_screenshots = si.GetBoolValue("Display", "InternalResolutionScreenshots", false);
  display_stretch_vertically = si.GetBoolValue("Display", "StretchVertically", false);
  video_sync_enabled = si.GetBoolValue("Display", "VSync", DEFAULT_VSYNC_VALUE);
  display_max_fps = si.GetFloatValue("Display", "MaxFPS", DEFAULT_DISPLAY_MAX_FPS);
  display_osd_scale = si.GetFloatValue("Display", "OSDScale", DEFAULT_OSD_SCALE);

  cdrom_readahead_sectors =
    static_cast<u8>(si.GetIntValue("CDROM", "ReadaheadSectors", DEFAULT_CDROM_READAHEAD_SECTORS));
  cdrom_mechacon_version =
    ParseCDROMMechVersionName(
      si.GetStringValue("CDROM", "MechaconVersion", GetCDROMMechVersionName(DEFAULT_CDROM_MECHACON_VERSION)).c_str())
      .value_or(DEFAULT_CDROM_MECHACON_VERSION);
  cdrom_region_check = si.GetBoolValue("CDROM", "RegionCheck", false);
  cdrom_load_image_to_ram = si.GetBoolValue("CDROM", "LoadImageToRAM", false);
  cdrom_load_image_patches = si.GetBoolValue("CDROM", "LoadImagePatches", false);
  cdrom_mute_cd_audio = si.GetBoolValue("CDROM", "MuteCDAudio", false);
  cdrom_read_speedup = si.GetIntValue("CDROM", "ReadSpeedup", 1);
  cdrom_seek_speedup = si.GetIntValue("CDROM", "SeekSpeedup", 1);

  audio_backend =
    ParseAudioBackend(si.GetStringValue("Audio", "Backend", GetAudioBackendName(DEFAULT_AUDIO_BACKEND)).c_str())
      .value_or(DEFAULT_AUDIO_BACKEND);
  audio_driver = si.GetStringValue("Audio", "Driver");
  audio_output_device = si.GetStringValue("Audio", "OutputDevice");
  audio_stretch_mode =
    AudioStream::ParseStretchMode(
      si.GetStringValue("Audio", "StretchMode", AudioStream::GetStretchModeName(DEFAULT_AUDIO_STRETCH_MODE)).c_str())
      .value_or(DEFAULT_AUDIO_STRETCH_MODE);
  audio_output_latency_ms = si.GetUIntValue("Audio", "OutputLatencyMS", DEFAULT_AUDIO_OUTPUT_LATENCY_MS);
  audio_buffer_ms = si.GetUIntValue("Audio", "BufferMS", DEFAULT_AUDIO_BUFFER_MS);
  audio_output_volume = si.GetUIntValue("Audio", "OutputVolume", 100);
  audio_fast_forward_volume = si.GetUIntValue("Audio", "FastForwardVolume", 100);

  audio_output_muted = si.GetBoolValue("Audio", "OutputMuted", false);
  audio_dump_on_boot = si.GetBoolValue("Audio", "DumpOnBoot", false);

  use_old_mdec_routines = si.GetBoolValue("Hacks", "UseOldMDECRoutines", false);
  pcdrv_enable = si.GetBoolValue("PCDrv", "Enabled", false);
  pcdrv_enable_writes = si.GetBoolValue("PCDrv", "EnableWrites", false);
  pcdrv_root = si.GetStringValue("PCDrv", "Root");

  dma_max_slice_ticks = si.GetIntValue("Hacks", "DMAMaxSliceTicks", DEFAULT_DMA_MAX_SLICE_TICKS);
  dma_halt_ticks = si.GetIntValue("Hacks", "DMAHaltTicks", DEFAULT_DMA_HALT_TICKS);
  gpu_fifo_size = static_cast<u32>(si.GetIntValue("Hacks", "GPUFIFOSize", DEFAULT_GPU_FIFO_SIZE));
  gpu_max_run_ahead = si.GetIntValue("Hacks", "GPUMaxRunAhead", DEFAULT_GPU_MAX_RUN_AHEAD);

  bios_tty_logging = si.GetBoolValue("BIOS", "TTYLogging", false);
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
  achievements_hardcore_mode = si.GetBoolValue("Cheevos", "ChallengeMode", false);
  achievements_notifications = si.GetBoolValue("Cheevos", "Notifications", true);
  achievements_leaderboard_notifications = si.GetBoolValue("Cheevos", "LeaderboardNotifications", true);
  achievements_sound_effects = si.GetBoolValue("Cheevos", "SoundEffects", true);
  achievements_overlays = si.GetBoolValue("Cheevos", "Overlays", true);
  achievements_encore_mode = si.GetBoolValue("Cheevos", "EncoreMode", false);
  achievements_spectator_mode = si.GetBoolValue("Cheevos", "SpectatorMode", false);
  achievements_unofficial_test_mode = si.GetBoolValue("Cheevos", "UnofficialTestMode", false);
  achievements_use_first_disc_from_playlist = si.GetBoolValue("Cheevos", "UseFirstDiscFromPlaylist", true);
  achievements_use_raintegration = si.GetBoolValue("Cheevos", "UseRAIntegration", false);
  achievements_notification_duration =
    si.GetIntValue("Cheevos", "NotificationsDuration", DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME);
  achievements_leaderboard_duration =
    si.GetIntValue("Cheevos", "LeaderboardsDuration", DEFAULT_LEADERBOARD_NOTIFICATION_TIME);

  log_level = ParseLogLevelName(si.GetStringValue("Logging", "LogLevel", GetLogLevelName(DEFAULT_LOG_LEVEL)).c_str())
                .value_or(DEFAULT_LOG_LEVEL);
  log_filter = si.GetStringValue("Logging", "LogFilter", "");
  log_timestamps = si.GetBoolValue("Logging", "LogTimestamps", true);
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

#ifdef __ANDROID__
  // Android users are incredibly silly and don't understand that stretch is in the aspect ratio list...
  if (si.GetBoolValue("Display", "Stretch", false))
    display_aspect_ratio = DisplayAspectRatio::MatchWindow;
#endif
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
  si.SetBoolValue("Main", "SaveStateOnExit", save_state_on_exit);
  si.SetBoolValue("Main", "CreateSaveStateBackups", create_save_state_backups);
  si.SetBoolValue("Main", "CompressSaveStates", compress_save_states);
  si.SetBoolValue("Main", "ConfirmPowerOff", confim_power_off);
  si.SetBoolValue("Main", "LoadDevicesFromSaveStates", load_devices_from_save_states);
  si.SetBoolValue("Main", "ApplyCompatibilitySettings", apply_compatibility_settings);
  si.SetBoolValue("Main", "ApplyGameSettings", apply_game_settings);
  si.SetBoolValue("Main", "AutoLoadCheats", auto_load_cheats);
  si.SetBoolValue("Main", "DisableAllEnhancements", disable_all_enhancements);
  si.SetBoolValue("Main", "EnableDiscordPresence", enable_discord_presence);
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
  si.SetBoolValue("GPU", "DisableShaderCache", gpu_disable_shader_cache);
  si.SetBoolValue("GPU", "DisableDualSourceBlend", gpu_disable_dual_source_blend);
  si.SetBoolValue("GPU", "DisableFramebufferFetch", gpu_disable_framebuffer_fetch);
  si.SetBoolValue("GPU", "DisableTextureBuffers", gpu_disable_texture_buffers);
  si.SetBoolValue("GPU", "DisableTextureCopyToSelf", gpu_disable_texture_copy_to_self);
  si.SetBoolValue("GPU", "PerSampleShading", gpu_per_sample_shading);
  si.SetBoolValue("GPU", "UseThread", gpu_use_thread);
  si.SetBoolValue("GPU", "ThreadedPresentation", gpu_threaded_presentation);
  si.SetBoolValue("GPU", "UseSoftwareRendererForReadbacks", gpu_use_software_renderer_for_readbacks);
  si.SetBoolValue("GPU", "TrueColor", gpu_true_color);
  si.SetBoolValue("GPU", "Debanding", gpu_debanding);
  si.SetBoolValue("GPU", "ScaledDithering", gpu_scaled_dithering);
  si.SetStringValue("GPU", "TextureFilter", GetTextureFilterName(gpu_texture_filter));
  si.SetStringValue("GPU", "DownsampleMode", GetDownsampleModeName(gpu_downsample_mode));
  si.SetUIntValue("GPU", "DownsampleScale", gpu_downsample_scale);
  si.SetStringValue("GPU", "WireframeMode", GetGPUWireframeModeName(gpu_wireframe_mode));
  si.SetBoolValue("GPU", "DisableInterlacing", gpu_disable_interlacing);
  si.SetBoolValue("GPU", "ForceNTSCTimings", gpu_force_ntsc_timings);
  si.SetBoolValue("GPU", "WidescreenHack", gpu_widescreen_hack);
  si.SetBoolValue("GPU", "ChromaSmoothing24Bit", gpu_24bit_chroma_smoothing);
  si.SetBoolValue("GPU", "PGXPEnable", gpu_pgxp_enable);
  si.SetBoolValue("GPU", "PGXPCulling", gpu_pgxp_culling);
  si.SetBoolValue("GPU", "PGXPTextureCorrection", gpu_pgxp_texture_correction);
  si.SetBoolValue("GPU", "PGXPColorCorrection", gpu_pgxp_color_correction);
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
  si.SetStringValue("Display", "Alignment", GetDisplayAlignmentName(display_alignment));
  si.SetStringValue("Display", "Scaling", GetDisplayScalingName(display_scaling));
  si.SetStringValue("Display", "ExclusiveFullscreenControl",
                    GetDisplayExclusiveFullscreenControlName(display_exclusive_fullscreen_control));
  si.SetIntValue("Display", "CustomAspectRatioNumerator", display_aspect_ratio_custom_numerator);
  si.GetIntValue("Display", "CustomAspectRatioDenominator", display_aspect_ratio_custom_denominator);
  si.SetBoolValue("Display", "ShowOSDMessages", display_show_osd_messages);
  si.SetBoolValue("Display", "ShowFPS", display_show_fps);
  si.SetBoolValue("Display", "ShowSpeed", display_show_speed);
  si.SetBoolValue("Display", "ShowResolution", display_show_resolution);
  si.SetBoolValue("Display", "ShowGPUStatistics", display_show_gpu_stats);
  si.SetBoolValue("Display", "ShowCPU", display_show_cpu_usage);
  si.SetBoolValue("Display", "ShowGPU", display_show_gpu_usage);
  si.SetBoolValue("Display", "ShowFrameTimes", display_show_frame_times);
  si.SetBoolValue("Display", "ShowStatusIndicators", display_show_status_indicators);
  si.SetBoolValue("Display", "ShowInputs", display_show_inputs);
  si.SetBoolValue("Display", "ShowEnhancements", display_show_enhancements);
  si.SetBoolValue("Display", "DisplayAllFrames", display_all_frames);
  si.SetBoolValue("Display", "InternalResolutionScreenshots", display_internal_resolution_screenshots);
  si.SetBoolValue("Display", "StretchVertically", display_stretch_vertically);
  si.SetBoolValue("Display", "VSync", video_sync_enabled);
  si.SetFloatValue("Display", "MaxFPS", display_max_fps);
  si.SetFloatValue("Display", "OSDScale", display_osd_scale);

  si.SetIntValue("CDROM", "ReadaheadSectors", cdrom_readahead_sectors);
  si.SetStringValue("CDROM", "MechaconVersion", GetCDROMMechVersionName(cdrom_mechacon_version));
  si.SetBoolValue("CDROM", "RegionCheck", cdrom_region_check);
  si.SetBoolValue("CDROM", "LoadImageToRAM", cdrom_load_image_to_ram);
  si.SetBoolValue("CDROM", "LoadImagePatches", cdrom_load_image_patches);
  si.SetBoolValue("CDROM", "MuteCDAudio", cdrom_mute_cd_audio);
  si.SetIntValue("CDROM", "ReadSpeedup", cdrom_read_speedup);
  si.SetIntValue("CDROM", "SeekSpeedup", cdrom_seek_speedup);

  si.SetStringValue("Audio", "Backend", GetAudioBackendName(audio_backend));
  si.SetStringValue("Audio", "Driver", audio_driver.c_str());
  si.SetStringValue("Audio", "OutputDevice", audio_output_device.c_str());
  si.SetStringValue("Audio", "StretchMode", AudioStream::GetStretchModeName(audio_stretch_mode));
  si.SetUIntValue("Audio", "BufferMS", audio_buffer_ms);
  si.SetUIntValue("Audio", "OutputLatencyMS", audio_output_latency_ms);
  si.SetUIntValue("Audio", "OutputVolume", audio_output_volume);
  si.SetUIntValue("Audio", "FastForwardVolume", audio_fast_forward_volume);
  si.SetBoolValue("Audio", "OutputMuted", audio_output_muted);
  si.SetBoolValue("Audio", "DumpOnBoot", audio_dump_on_boot);

  si.SetBoolValue("Hacks", "UseOldMDECRoutines", use_old_mdec_routines);
  si.SetIntValue("Hacks", "DMAMaxSliceTicks", dma_max_slice_ticks);
  si.SetIntValue("Hacks", "DMAHaltTicks", dma_halt_ticks);
  si.SetIntValue("Hacks", "GPUFIFOSize", gpu_fifo_size);
  si.SetIntValue("Hacks", "GPUMaxRunAhead", gpu_max_run_ahead);

  si.SetBoolValue("PCDrv", "Enabled", pcdrv_enable);
  si.SetBoolValue("PCDrv", "EnableWrites", pcdrv_enable_writes);
  si.SetStringValue("PCDrv", "Root", pcdrv_root.c_str());

  si.SetBoolValue("BIOS", "TTYLogging", bios_tty_logging);
  si.SetBoolValue("BIOS", "PatchFastBoot", bios_patch_fast_boot);

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    si.SetStringValue(Controller::GetSettingsSection(i).c_str(), "Type", GetControllerTypeName(controller_types[i]));

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
  si.SetBoolValue("Cheevos", "ChallengeMode", achievements_hardcore_mode);
  si.SetBoolValue("Cheevos", "Notifications", achievements_notifications);
  si.SetBoolValue("Cheevos", "LeaderboardNotifications", achievements_leaderboard_notifications);
  si.SetBoolValue("Cheevos", "SoundEffects", achievements_sound_effects);
  si.SetBoolValue("Cheevos", "Overlays", achievements_overlays);
  si.SetBoolValue("Cheevos", "EncoreMode", achievements_encore_mode);
  si.SetBoolValue("Cheevos", "SpectatorMode", achievements_spectator_mode);
  si.SetBoolValue("Cheevos", "UnofficialTestMode", achievements_unofficial_test_mode);
  si.SetBoolValue("Cheevos", "UseFirstDiscFromPlaylist", achievements_use_first_disc_from_playlist);
  si.SetBoolValue("Cheevos", "UseRAIntegration", achievements_use_raintegration);
  si.SetIntValue("Cheevos", "NotificationsDuration", achievements_notification_duration);
  si.SetIntValue("Cheevos", "LeaderboardsDuration", achievements_leaderboard_duration);

  si.SetStringValue("Logging", "LogLevel", GetLogLevelName(log_level));
  si.SetStringValue("Logging", "LogFilter", log_filter.c_str());
  si.SetBoolValue("Logging", "LogTimestamps", log_timestamps);
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
    g_settings.gpu_debanding = false;
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
    g_settings.use_old_mdec_routines = false;
    g_settings.pcdrv_enable = false;
    g_settings.bios_patch_fast_boot = false;
  }

  if (g_settings.pcdrv_enable && g_settings.pcdrv_root.empty())
  {
    Log_WarningPrintf("Disabling PCDrv because no root directory is specified.");
    g_settings.pcdrv_enable = false;
  }

  if (g_settings.gpu_pgxp_enable)
  {
    if (g_settings.gpu_renderer == GPURenderer::Software)
    {
      if (display_osd_messages)
      {
        Host::AddKeyedOSDMessage(
          "pgxp_disabled_sw",
          TRANSLATE_STR("OSDMessage", "PGXP is incompatible with the software renderer, disabling PGXP."), 10.0f);
      }
      g_settings.gpu_pgxp_enable = false;
    }
  }

#ifndef ENABLE_MMAP_FASTMEM
  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::MMap)
  {
    Log_WarningPrintf("mmap fastmem is not available on this platform, using LUT instead.");
    g_settings.cpu_fastmem_mode = CPUFastmemMode::LUT;
  }
#endif

#if defined(__ANDROID__) && defined(__arm__) && !defined(__aarch64__) && !defined(_M_ARM64)
  if (g_settings.rewind_enable)
  {
    Host::AddKeyedOSDMessage("rewind_disabled_android",
                             TRANSLATE_STR("OSDMessage", "Rewind is not supported on 32-bit ARM for Android."), 30.0f);
    g_settings.rewind_enable = false;
  }
  if (g_settings.IsRunaheadEnabled())
  {
    Host::AddKeyedOSDMessage("rewind_disabled_android",
                             TRANSLATE_STR("OSDMessage", "Runahead is not supported on 32-bit ARM for Android."),
                             30.0f);
    g_settings.runahead_frames = 0;
  }
#endif

  if (g_settings.IsRunaheadEnabled() && g_settings.rewind_enable)
  {
    Host::AddKeyedOSDMessage("rewind_disabled_android",
                             TRANSLATE_STR("OSDMessage", "Rewind is disabled because runahead is enabled."), 10.0f);
    g_settings.rewind_enable = false;
  }

  if (g_settings.IsRunaheadEnabled())
  {
    // Block linking is good for performance, but hurts when regularly loading (i.e. runahead), since everything has to
    // be unlinked. Which would be thousands of blocks.
    if (g_settings.cpu_recompiler_block_linking)
    {
      Log_WarningPrintf("Disabling block linking due to runahead.");
      g_settings.cpu_recompiler_block_linking = false;
    }
  }

  // if challenge mode is enabled, disable things like rewind since they use save states
  if (Achievements::IsHardcoreModeActive())
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

void Settings::UpdateLogSettings()
{
  Log::SetLogLevel(log_level);
  Log::SetLogFilter(log_filter);
  Log::SetConsoleOutputParams(log_to_console, log_timestamps);
  Log::SetDebugOutputParams(log_to_debug);

  if (log_to_file)
  {
    Log::SetFileOutputParams(log_to_file, Path::Combine(EmuFolders::DataRoot, "duckstation.log").c_str(),
                             log_timestamps);
  }
  else
  {
    Log::SetFileOutputParams(false, nullptr);
  }
}

void Settings::SetDefaultControllerConfig(SettingsInterface& si)
{
  // Global Settings
  si.SetStringValue("ControllerPorts", "MultitapMode", GetMultitapModeName(Settings::DEFAULT_MULTITAP_MODE));
  si.SetFloatValue("ControllerPorts", "PointerXScale", 8.0f);
  si.SetFloatValue("ControllerPorts", "PointerYScale", 8.0f);
  si.SetBoolValue("ControllerPorts", "PointerXInvert", false);
  si.SetBoolValue("ControllerPorts", "PointerYInvert", false);

  // Default pad types and parameters.
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    const std::string section(Controller::GetSettingsSection(i));
    si.ClearSection(section.c_str());
    si.SetStringValue(section.c_str(), "Type", Controller::GetDefaultPadType(i));
  }

#ifndef __ANDROID__
  // Use the automapper to set this up.
  InputManager::MapController(si, 0, InputManager::GetGenericBindingMapping("Keyboard"));
#endif
}

static constexpr const std::array s_log_level_names = {"None",    "Error", "Warning", "Perf",  "Info",
                                                       "Verbose", "Dev",   "Profile", "Debug", "Trace"};
static constexpr const std::array s_log_level_display_names = {
  TRANSLATE_NOOP("LogLevel", "None"),        TRANSLATE_NOOP("LogLevel", "Error"),
  TRANSLATE_NOOP("LogLevel", "Warning"),     TRANSLATE_NOOP("LogLevel", "Performance"),
  TRANSLATE_NOOP("LogLevel", "Information"), TRANSLATE_NOOP("LogLevel", "Verbose"),
  TRANSLATE_NOOP("LogLevel", "Developer"),   TRANSLATE_NOOP("LogLevel", "Profile"),
  TRANSLATE_NOOP("LogLevel", "Debug"),       TRANSLATE_NOOP("LogLevel", "Trace")};

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
  return Host::TranslateToCString("LogLevel", s_log_level_display_names[static_cast<int>(level)]);
}

static constexpr const std::array s_console_region_names = {"Auto", "NTSC-J", "NTSC-U", "PAL"};
static constexpr const std::array s_console_region_display_names = {
  TRANSLATE_NOOP("ConsoleRegion", "Auto-Detect"), TRANSLATE_NOOP("ConsoleRegion", "NTSC-J (Japan)"),
  TRANSLATE_NOOP("ConsoleRegion", "NTSC-U/C (US, Canada)"), TRANSLATE_NOOP("ConsoleRegion", "PAL (Europe, Australia)")};

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
  return Host::TranslateToCString("ConsoleRegion", s_console_region_display_names[static_cast<int>(region)]);
}

static constexpr const std::array s_disc_region_names = {"NTSC-J", "NTSC-U", "PAL", "Other", "Non-PS1"};
static constexpr const std::array s_disc_region_display_names = {
  TRANSLATE_NOOP("DiscRegion", "NTSC-J (Japan)"), TRANSLATE_NOOP("DiscRegion", "NTSC-U/C (US, Canada)"),
  TRANSLATE_NOOP("DiscRegion", "PAL (Europe, Australia)"), TRANSLATE_NOOP("DiscRegion", "Other"),
  TRANSLATE_NOOP("DiscRegion", "Non-PS1")};

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
  return Host::TranslateToCString("DiscRegion", s_disc_region_display_names[static_cast<int>(region)]);
}

static constexpr const std::array s_cpu_execution_mode_names = {"Interpreter", "CachedInterpreter", "Recompiler",
                                                                "NewRec"};
static constexpr const std::array s_cpu_execution_mode_display_names = {
  TRANSLATE_NOOP("CPUExecutionMode", "Interpreter (Slowest)"),
  TRANSLATE_NOOP("CPUExecutionMode", "Cached Interpreter (Faster)"),
  TRANSLATE_NOOP("CPUExecutionMode", "Recompiler (Fastest)"),
  TRANSLATE_NOOP("CPUExecutionMode", "New Recompiler (Experimental)")};

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
  return Host::TranslateToCString("CPUExecutionMode", s_cpu_execution_mode_display_names[static_cast<u8>(mode)]);
}

static constexpr const std::array s_cpu_fastmem_mode_names = {"Disabled", "MMap", "LUT"};
static constexpr const std::array s_cpu_fastmem_mode_display_names = {
  TRANSLATE_NOOP("CPUFastmemMode", "Disabled (Slowest)"),
  TRANSLATE_NOOP("CPUFastmemMode", "MMap (Hardware, Fastest, 64-Bit Only)"),
  TRANSLATE_NOOP("CPUFastmemMode", "LUT (Faster)")};

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
  return Host::TranslateToCString("CPUFastmemMode", s_cpu_fastmem_mode_display_names[static_cast<u8>(mode)]);
}

static constexpr const std::array s_gpu_renderer_names = {
  "Automatic",
#ifdef _WIN32
  "D3D11",     "D3D12",
#endif
#ifdef __APPLE__
  "Metal",
#endif
#ifdef ENABLE_VULKAN
  "Vulkan",
#endif
#ifdef ENABLE_OPENGL
  "OpenGL",
#endif
  "Software",
};
static constexpr const std::array s_gpu_renderer_display_names = {
  TRANSLATE_NOOP("GPURenderer", "Automatic"),
#ifdef _WIN32
  TRANSLATE_NOOP("GPURenderer", "Hardware (D3D11)"),  TRANSLATE_NOOP("GPURenderer", "Hardware (D3D12)"),
#endif
#ifdef __APPLE__
  TRANSLATE_NOOP("GPURenderer", "Hardware (Metal)"),
#endif
#ifdef ENABLE_VULKAN
  TRANSLATE_NOOP("GPURenderer", "Hardware (Vulkan)"),
#endif
#ifdef ENABLE_OPENGL
  TRANSLATE_NOOP("GPURenderer", "Hardware (OpenGL)"),
#endif
  TRANSLATE_NOOP("GPURenderer", "Software"),
};

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
  return Host::TranslateToCString("GPURenderer", s_gpu_renderer_display_names[static_cast<int>(renderer)]);
}

RenderAPI Settings::GetRenderAPIForRenderer(GPURenderer renderer)
{
  switch (renderer)
  {
#ifdef _WIN32
    case GPURenderer::HardwareD3D11:
      return RenderAPI::D3D11;
    case GPURenderer::HardwareD3D12:
      return RenderAPI::D3D12;
#endif
#ifdef __APPLE__
      return RenderAPI::Metal;
#endif
#ifdef ENABLE_VULKAN
    case GPURenderer::HardwareVulkan:
      return RenderAPI::Vulkan;
#endif
#ifdef ENABLE_OPENGL
    case GPURenderer::HardwareOpenGL:
      return RenderAPI::OpenGL;
#endif
    case GPURenderer::Software:
    case GPURenderer::Automatic:
    default:
      return GPUDevice::GetPreferredAPI();
  }
}

static constexpr const std::array s_texture_filter_names = {"Nearest",       "Bilinear", "BilinearBinAlpha", "JINC2",
                                                            "JINC2BinAlpha", "xBR",      "xBRBinAlpha"};
static constexpr const std::array s_texture_filter_display_names = {
  TRANSLATE_NOOP("GPUTextureFilter", "Nearest-Neighbor"),
  TRANSLATE_NOOP("GPUTextureFilter", "Bilinear"),
  TRANSLATE_NOOP("GPUTextureFilter", "Bilinear (No Edge Blending)"),
  TRANSLATE_NOOP("GPUTextureFilter", "JINC2 (Slow)"),
  TRANSLATE_NOOP("GPUTextureFilter", "JINC2 (Slow, No Edge Blending)"),
  TRANSLATE_NOOP("GPUTextureFilter", "xBR (Very Slow)"),
  TRANSLATE_NOOP("GPUTextureFilter", "xBR (Very Slow, No Edge Blending)")};

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
  return Host::TranslateToCString("GPUTextureFilter", s_texture_filter_display_names[static_cast<int>(filter)]);
}

static constexpr const std::array s_downsample_mode_names = {"Disabled", "Box", "Adaptive"};
static constexpr const std::array s_downsample_mode_display_names = {
  TRANSLATE_NOOP("GPUDownsampleMode", "Disabled"),
  TRANSLATE_NOOP("GPUDownsampleMode", "Box (Downsample 3D/Smooth All)"),
  TRANSLATE_NOOP("GPUDownsampleMode", "Adaptive (Preserve 3D/Smooth 2D)")};

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
  return Host::TranslateToCString("GPUDownsampleMode", s_downsample_mode_display_names[static_cast<int>(mode)]);
}

static constexpr const std::array s_wireframe_mode_names = {"Disabled", "OverlayWireframe", "OnlyWireframe"};
static constexpr const std::array s_wireframe_mode_display_names = {
  TRANSLATE_NOOP("GPUWireframeMode", "Disabled"), TRANSLATE_NOOP("GPUWireframeMode", "Overlay Wireframe"),
  TRANSLATE_NOOP("GPUWireframeMode", "Only Wireframe")};

std::optional<GPUWireframeMode> Settings::ParseGPUWireframeMode(const char* str)
{
  int index = 0;
  for (const char* name : s_wireframe_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<GPUWireframeMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetGPUWireframeModeName(GPUWireframeMode mode)
{
  return s_wireframe_mode_names[static_cast<int>(mode)];
}

const char* Settings::GetGPUWireframeModeDisplayName(GPUWireframeMode mode)
{
  return Host::TranslateToCString("GPUWireframeMode", s_wireframe_mode_display_names[static_cast<int>(mode)]);
}

static constexpr const std::array s_display_crop_mode_names = {"None", "Overscan", "Borders"};
static constexpr const std::array s_display_crop_mode_display_names = {
  TRANSLATE_NOOP("DisplayCropMode", "None"), TRANSLATE_NOOP("DisplayCropMode", "Only Overscan Area"),
  TRANSLATE_NOOP("DisplayCropMode", "All Borders")};

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
  return Host::TranslateToCString("DisplayCropMode", s_display_crop_mode_display_names[static_cast<int>(crop_mode)]);
}

static constexpr const std::array s_display_aspect_ratio_names = {
#ifndef __ANDROID__
  TRANSLATE_NOOP("DisplayAspectRatio", "Auto (Game Native)"),
  TRANSLATE_NOOP("DisplayAspectRatio", "Stretch To Fill"),
  TRANSLATE_NOOP("DisplayAspectRatio", "Custom"),
#else
  "Auto (Game Native)",
  "Auto (Match Window)",
  "Custom",
#endif
  "4:3",
  "16:9",
  "19:9",
  "20:9",
  "PAR 1:1"};
static constexpr const std::array s_display_aspect_ratio_values = {
  -1.0f, -1.0f, -1.0f, 4.0f / 3.0f, 16.0f / 9.0f, 19.0f / 9.0f, 20.0f / 9.0f, -1.0f};

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

const char* Settings::GetDisplayAspectRatioDisplayName(DisplayAspectRatio ar)
{
  return Host::TranslateToCString("DisplayAspectRatio", s_display_aspect_ratio_names[static_cast<int>(ar)]);
}

float Settings::GetDisplayAspectRatioValue() const
{
  switch (display_aspect_ratio)
  {
    case DisplayAspectRatio::MatchWindow:
    {
      if (!g_gpu_device)
        return s_display_aspect_ratio_values[static_cast<int>(DEFAULT_DISPLAY_ASPECT_RATIO)];

      return static_cast<float>(g_gpu_device->GetWindowWidth()) / static_cast<float>(g_gpu_device->GetWindowHeight());
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

static constexpr const std::array s_display_alignment_names = {"LeftOrTop", "Center", "RightOrBottom"};
static constexpr const std::array s_display_alignment_display_names = {
  TRANSLATE_NOOP("DisplayAlignment", "Left / Top"), TRANSLATE_NOOP("DisplayAlignment", "Center"),
  TRANSLATE_NOOP("DisplayAlignment", "Right / Bottom")};

std::optional<DisplayAlignment> Settings::ParseDisplayAlignment(const char* str)
{
  int index = 0;
  for (const char* name : s_display_alignment_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<DisplayAlignment>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetDisplayAlignmentName(DisplayAlignment alignment)
{
  return s_display_alignment_names[static_cast<int>(alignment)];
}

const char* Settings::GetDisplayAlignmentDisplayName(DisplayAlignment alignment)
{
  return Host::TranslateToCString("DisplayAlignment", s_display_alignment_display_names[static_cast<int>(alignment)]);
}

static constexpr const std::array s_display_scaling_names = {
  "Nearest",
  "BilinearSmooth",
  "NearestInteger",
  "BilinearSharp",
};
static constexpr const std::array s_display_scaling_display_names = {
  TRANSLATE_NOOP("DisplayScalingMode", "Nearest-Neighbor"),
  TRANSLATE_NOOP("DisplayScalingMode", "Bilinear (Smooth)"),
  TRANSLATE_NOOP("DisplayScalingMode", "Nearest-Neighbor (Integer)"),
  TRANSLATE_NOOP("DisplayScalingMode", "Bilinear (Sharp)"),
};

std::optional<DisplayScalingMode> Settings::ParseDisplayScaling(const char* str)
{
  int index = 0;
  for (const char* name : s_display_scaling_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<DisplayScalingMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetDisplayScalingName(DisplayScalingMode mode)
{
  return s_display_scaling_names[static_cast<int>(mode)];
}

const char* Settings::GetDisplayScalingDisplayName(DisplayScalingMode mode)
{
  return Host::TranslateToCString("DisplayScalingMode", s_display_scaling_display_names[static_cast<int>(mode)]);
}

static constexpr const std::array s_display_exclusive_fullscreen_mode_names = {
  "Automatic",
  "Disallowed",
  "Allowed",
};
static constexpr const std::array s_display_exclusive_fullscreen_mode_display_names = {
  TRANSLATE_NOOP("Settings", "Automatic (Default)"),
  TRANSLATE_NOOP("Settings", "Disallowed"),
  TRANSLATE_NOOP("Settings", "Allowed"),
};

std::optional<DisplayExclusiveFullscreenControl> Settings::ParseDisplayExclusiveFullscreenControl(const char* str)
{
  int index = 0;
  for (const char* name : s_display_exclusive_fullscreen_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<DisplayExclusiveFullscreenControl>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetDisplayExclusiveFullscreenControlName(DisplayExclusiveFullscreenControl mode)
{
  return s_display_exclusive_fullscreen_mode_names[static_cast<int>(mode)];
}

const char* Settings::GetDisplayExclusiveFullscreenControlDisplayName(DisplayExclusiveFullscreenControl mode)
{
  return Host::TranslateToCString("Settings",
                                  s_display_exclusive_fullscreen_mode_display_names[static_cast<int>(mode)]);
}

static constexpr const std::array s_audio_backend_names = {
  "Null",
#ifdef ENABLE_CUBEB
  "Cubeb",
#endif
#ifdef _WIN32
  "XAudio2",
#endif
#ifdef __ANDROID__
  "AAudio",  "OpenSLES",
#endif
};
static constexpr const std::array s_audio_backend_display_names = {
  TRANSLATE_NOOP("AudioBackend", "Null (No Output)"),
#ifdef ENABLE_CUBEB
  TRANSLATE_NOOP("AudioBackend", "Cubeb"),
#endif
#ifdef _WIN32
  TRANSLATE_NOOP("AudioBackend", "XAudio2"),
#endif
#ifdef __ANDROID__
  "AAudio",
  "OpenSL ES",
#endif
};

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
  return Host::TranslateToCString("AudioBackend", s_audio_backend_display_names[static_cast<int>(backend)]);
}

static constexpr const std::array s_controller_type_names = {
  "None", "DigitalController", "AnalogController", "AnalogJoystick", "GunCon", "PlayStationMouse", "NeGcon"};
static constexpr const std::array s_controller_display_names = {
  TRANSLATE_NOOP("ControllerType", "None"),
  TRANSLATE_NOOP("ControllerType", "Digital Controller"),
  TRANSLATE_NOOP("ControllerType", "Analog Controller (DualShock)"),
  TRANSLATE_NOOP("ControllerType", "Analog Joystick"),
  TRANSLATE_NOOP("ControllerType", "GunCon"),
  TRANSLATE_NOOP("ControllerType", "PlayStation Mouse"),
  TRANSLATE_NOOP("ControllerType", "NeGcon")};

std::optional<ControllerType> Settings::ParseControllerTypeName(std::string_view str)
{
  int index = 0;
  for (const char* name : s_controller_type_names)
  {
    if (StringUtil::EqualNoCase(str, name))
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
  return Host::TranslateToCString("ControllerType", s_controller_display_names[static_cast<int>(type)]);
}

static constexpr const std::array s_memory_card_type_names = {"None",         "Shared",           "PerGame",
                                                              "PerGameTitle", "PerGameFileTitle", "NonPersistent"};
static constexpr const std::array s_memory_card_type_display_names = {
  TRANSLATE_NOOP("MemoryCardType", "No Memory Card"),
  TRANSLATE_NOOP("MemoryCardType", "Shared Between All Games"),
  TRANSLATE_NOOP("MemoryCardType", "Separate Card Per Game (Serial)"),
  TRANSLATE_NOOP("MemoryCardType", "Separate Card Per Game (Title)"),
  TRANSLATE_NOOP("MemoryCardType", "Separate Card Per Game (File Title)"),
  TRANSLATE_NOOP("MemoryCardType", "Non-Persistent Card (Do Not Save)")};

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
  return Host::TranslateToCString("MemoryCardType", s_memory_card_type_display_names[static_cast<int>(type)]);
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

std::string Settings::GetGameMemoryCardPath(const std::string_view& serial, u32 slot)
{
  return Path::Combine(EmuFolders::MemoryCards, fmt::format("{}_{}.mcd", serial, slot + 1));
}

static constexpr const std::array s_multitap_enable_mode_names = {"Disabled", "Port1Only", "Port2Only", "BothPorts"};
static constexpr const std::array s_multitap_enable_mode_display_names = {
  TRANSLATE_NOOP("MultitapMode", "Disabled"), TRANSLATE_NOOP("MultitapMode", "Enable on Port 1 Only"),
  TRANSLATE_NOOP("MultitapMode", "Enable on Port 2 Only"), TRANSLATE_NOOP("MultitapMode", "Enable on Ports 1 and 2")};

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
  return Host::TranslateToCString("MultitapMode", s_multitap_enable_mode_display_names[static_cast<size_t>(mode)]);
}

static constexpr const std::array s_mechacon_version_names = {"VC0A", "VC0B", "VC1A", "VC1B", "VD1",  "VC2", "VC1",
                                                              "VC2J", "VC2A", "VC2B", "VC3A", "VC3B", "VC3C"};
static constexpr const std::array s_mechacon_version_display_names = {
  "94/09/19 (VC0A)", "94/11/18 (VC0B)", "95/05/16 (VC1A)", "95/07/24 (VC1B)", "95/07/24 (VD1)",
  "96/08/15 (VC2)",  "96/08/18 (VC1)",  "96/09/12 (VC2J)", "97/01/10 (VC2A)", "97/08/14 (VC2B)",
  "98/06/10 (VC3A)", "99/02/01 (VC3B)", "01/03/06 (VC3C)"};

std::optional<CDROMMechaconVersion> Settings::ParseCDROMMechVersionName(const char* str)
{
  u32 index = 0;
  for (const char* name : s_mechacon_version_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<CDROMMechaconVersion>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetCDROMMechVersionName(CDROMMechaconVersion mode)
{
  return s_mechacon_version_names[static_cast<u32>(mode)];
}

const char* Settings::GetCDROMMechVersionDisplayName(CDROMMechaconVersion mode)
{
  return s_mechacon_version_display_names[static_cast<size_t>(mode)];
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
std::string EmuFolders::UserResources;

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
  UserResources = Path::Combine(DataRoot, "resources");
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
  SaveStates = LoadPathFromSettings(si, DataRoot, "Folders", "SaveStates", "savestates");
  Screenshots = LoadPathFromSettings(si, DataRoot, "Folders", "Screenshots", "screenshots");
  Shaders = LoadPathFromSettings(si, DataRoot, "Folders", "Shaders", "shaders");
  Textures = LoadPathFromSettings(si, DataRoot, "Folders", "Textures", "textures");
  UserResources = LoadPathFromSettings(si, DataRoot, "Folders", "UserResources", "resources");

  Log_DevFmt("BIOS Directory: {}", Bios);
  Log_DevFmt("Cache Directory: {}", Cache);
  Log_DevFmt("Cheats Directory: {}", Cheats);
  Log_DevFmt("Covers Directory: {}", Covers);
  Log_DevFmt("Dumps Directory: {}", Dumps);
  Log_DevFmt("Game Settings Directory: {}", GameSettings);
  Log_DevFmt("Input Profile Directory: {}", InputProfiles);
  Log_DevFmt("MemoryCards Directory: {}", MemoryCards);
  Log_DevFmt("Resources Directory: {}", Resources);
  Log_DevFmt("SaveStates Directory: {}", SaveStates);
  Log_DevFmt("Screenshots Directory: {}", Screenshots);
  Log_DevFmt("Shaders Directory: {}", Shaders);
  Log_DevFmt("Textures Directory: {}", Textures);
  Log_DevFmt("User Resources Directory: {}", UserResources);
}

void EmuFolders::Save(SettingsInterface& si)
{
  // convert back to relative
  si.SetStringValue("BIOS", "SearchDirectory", Path::MakeRelative(Bios, DataRoot).c_str());
  si.SetStringValue("Folders", "Cache", Path::MakeRelative(Cache, DataRoot).c_str());
  si.SetStringValue("Folders", "Cheats", Path::MakeRelative(Cheats, DataRoot).c_str());
  si.SetStringValue("Folders", "Covers", Path::MakeRelative(Covers, DataRoot).c_str());
  si.SetStringValue("Folders", "Dumps", Path::MakeRelative(Dumps, DataRoot).c_str());
  si.SetStringValue("Folders", "GameSettings", Path::MakeRelative(GameSettings, DataRoot).c_str());
  si.SetStringValue("Folders", "InputProfiles", Path::MakeRelative(InputProfiles, DataRoot).c_str());
  si.SetStringValue("MemoryCards", "Directory", Path::MakeRelative(MemoryCards, DataRoot).c_str());
  si.SetStringValue("Folders", "SaveStates", Path::MakeRelative(SaveStates, DataRoot).c_str());
  si.SetStringValue("Folders", "Screenshots", Path::MakeRelative(Screenshots, DataRoot).c_str());
  si.SetStringValue("Folders", "Shaders", Path::MakeRelative(Shaders, DataRoot).c_str());
  si.SetStringValue("Folders", "Textures", Path::MakeRelative(Textures, DataRoot).c_str());
  si.SetStringValue("Folders", "UserResources", Path::MakeRelative(UserResources, DataRoot).c_str());
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
  result = FileSystem::EnsureDirectoryExists(Path::Combine(Shaders, "reshade").c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(
             Path::Combine(Shaders, "reshade" FS_OSPATH_SEPARATOR_STR "Shaders").c_str(), false) &&
           result;
  result = FileSystem::EnsureDirectoryExists(
             Path::Combine(Shaders, "reshade" FS_OSPATH_SEPARATOR_STR "Textures").c_str(), false) &&
           result;
  result = FileSystem::EnsureDirectoryExists(Textures.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(UserResources.c_str(), false) && result;
  return result;
}

std::string EmuFolders::GetOverridableResourcePath(std::string_view name)
{
  std::string upath = Path::Combine(UserResources, name);
  if (FileSystem::FileExists(upath.c_str()))
  {
    if (UserResources != Resources)
      Log_WarningFmt("Using user-provided resource file {}", name);
  }
  else
  {
    upath = Path::Combine(Resources, name);
  }

  return upath;
}

static const char* s_log_filters[] = {
  "Achievements",
  "AnalogController",
  "AnalogJoystick",
  "AudioStream",
  "AutoUpdaterDialog",
  "BIOS",
  "Bus",
  "ByteStream",
  "CDImage",
  "CDImageBin",
  "CDImageCHD",
  "CDImageCueSheet",
  "CDImageDevice",
  "CDImageEcm",
  "CDImageMds",
  "CDImageMemory",
  "CDImagePBP",
  "CDImagePPF",
  "CDROM",
  "CDROMAsyncReader",
  "CDSubChannelReplacement",
  "CPU::CodeCache",
  "CPU::Core",
  "CPU::Recompiler",
  "Common::PageFaultHandler",
  "ControllerBindingWidget",
  "CueParser",
  "Cheats",
  "DMA",
  "DisplayWidget",
  "FileSystem",
  "FullscreenUI",
  "GDBConnection",
  "GDBProtocol",
  "GDBServer",
  "GPU",
  "GPUBackend",
  "GPUDevice",
  "GPUShaderCache",
  "GPUTexture",
  "GPU_HW",
  "GPU_SW",
  "GameDatabase",
  "GameList",
  "GunCon",
  "HTTPDownloader",
  "Host",
  "HostInterfaceProgressCallback",
  "INISettingsInterface",
  "ISOReader",
  "ImGuiFullscreen",
  "ImGuiManager",
  "Image",
  "InputManager",
  "InterruptController",
  "JitCodeBuffer",
  "MDEC",
  "MainWindow",
  "MemoryArena",
  "MemoryCard",
  "Multitap",
  "NoGUIHost",
  "PCDrv",
  "PGXP",
  "PSFLoader",
  "Pad",
  "PlatformMisc",
  "PlayStationMouse",
  "PostProcessing",
  "ProgressCallback",
  "QTTranslations",
  "QtHost",
  "ReShadeFXShader",
  "Recompiler::CodeGenerator",
  "RegTestHost",
  "SDLInputSource",
  "SIO",
  "SPIRVCompiler",
  "SPU",
  "Settings",
  "ShaderGen",
  "StateWrapper",
  "System",
  "TextureReplacements",
  "Timers",
  "TimingEvents",
  "WAVWriter",
  "WindowInfo",

#ifdef ENABLE_CUBEB
  "CubebAudioStream",
#endif

#ifdef ENABLE_OPENGL
  "OpenGLContext",
  "OpenGLDevice",
#endif

#ifdef ENABLE_VULKAN
  "VulkanDevice",
#endif

#if defined(_WIN32)
  "D3D11Device",
  "D3D12Device",
  "D3D12StreamBuffer",
  "D3DCommon",
  "DInputSource",
  "Win32ProgressCallback",
  "Win32RawInputSource",
  "XAudio2AudioStream",
  "XInputSource",
#elif defined(__APPLE__)
  "CocoaNoGUIPlatform",
  "CocoaProgressCallback",
  "MetalDevice",
#else
  "X11NoGUIPlatform",
  "WaylandNoGUIPlatform",
#endif
};

std::span<const char*> Settings::GetLogFilters()
{
  return s_log_filters;
}
