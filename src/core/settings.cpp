// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "settings.h"
#include "achievements.h"
#include "controller.h"
#include "host.h"
#include "system.h"

#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/media_capture.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/memmap.h"
#include "common/path.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <numeric>

LOG_CHANNEL(Settings);

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

#ifdef DYNAMIC_HOST_PAGE_SIZE
// See note in settings.h - 16K ends up faster with LUT because of nearby code/data.
const CPUFastmemMode Settings::DEFAULT_CPU_FASTMEM_MODE =
  (MemMap::GetRuntimePageSize() > 4096) ? CPUFastmemMode::LUT : CPUFastmemMode::MMap;
#endif

#if defined(_WIN32)
const MediaCaptureBackend Settings::DEFAULT_MEDIA_CAPTURE_BACKEND = MediaCaptureBackend::MediaFoundation;
#elif !defined(__ANDROID__)
const MediaCaptureBackend Settings::DEFAULT_MEDIA_CAPTURE_BACKEND = MediaCaptureBackend::FFmpeg;
#else
const MediaCaptureBackend Settings::DEFAULT_MEDIA_CAPTURE_BACKEND = MediaCaptureBackend::MaxCount;
#endif

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

void Settings::Load(SettingsInterface& si, SettingsInterface& controller_si)
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
  inhibit_screensaver = si.GetBoolValue("Main", "InhibitScreensaver", true);
  start_paused = si.GetBoolValue("Main", "StartPaused", false);
  start_fullscreen = si.GetBoolValue("Main", "StartFullscreen", false);
  pause_on_focus_loss = si.GetBoolValue("Main", "PauseOnFocusLoss", false);
  pause_on_controller_disconnection = si.GetBoolValue("Main", "PauseOnControllerDisconnection", false);
  save_state_on_exit = si.GetBoolValue("Main", "SaveStateOnExit", true);
  create_save_state_backups = si.GetBoolValue("Main", "CreateSaveStateBackups", DEFAULT_SAVE_STATE_BACKUPS);
  confim_power_off = si.GetBoolValue("Main", "ConfirmPowerOff", true);
  load_devices_from_save_states = si.GetBoolValue("Main", "LoadDevicesFromSaveStates", false);
  apply_compatibility_settings = si.GetBoolValue("Main", "ApplyCompatibilitySettings", true);
  apply_game_settings = si.GetBoolValue("Main", "ApplyGameSettings", true);
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
  gpu_resolution_scale = static_cast<u8>(si.GetIntValue("GPU", "ResolutionScale", 1));
  gpu_multisamples = static_cast<u8>(si.GetIntValue("GPU", "Multisamples", 1));
  gpu_use_debug_device = si.GetBoolValue("GPU", "UseDebugDevice", false);
  gpu_disable_shader_cache = si.GetBoolValue("GPU", "DisableShaderCache", false);
  gpu_disable_dual_source_blend = si.GetBoolValue("GPU", "DisableDualSourceBlend", false);
  gpu_disable_framebuffer_fetch = si.GetBoolValue("GPU", "DisableFramebufferFetch", false);
  gpu_disable_texture_buffers = si.GetBoolValue("GPU", "DisableTextureBuffers", false);
  gpu_disable_texture_copy_to_self = si.GetBoolValue("GPU", "DisableTextureCopyToSelf", false);
  gpu_disable_memory_import = si.GetBoolValue("GPU", "DisableMemoryImport", false);
  gpu_disable_raster_order_views = si.GetBoolValue("GPU", "DisableRasterOrderViews", false);
  gpu_per_sample_shading = si.GetBoolValue("GPU", "PerSampleShading", false);
  gpu_use_thread = si.GetBoolValue("GPU", "UseThread", true);
  gpu_use_software_renderer_for_readbacks = si.GetBoolValue("GPU", "UseSoftwareRendererForReadbacks", false);
  gpu_true_color = si.GetBoolValue("GPU", "TrueColor", true);
  gpu_scaled_dithering = si.GetBoolValue("GPU", "ScaledDithering", true);
  gpu_force_round_texcoords = si.GetBoolValue("GPU", "ForceRoundTextureCoordinates", false);
  gpu_accurate_blending = si.GetBoolValue("GPU", "AccurateBlending", false);
  gpu_texture_filter =
    ParseTextureFilterName(
      si.GetStringValue("GPU", "TextureFilter", GetTextureFilterName(DEFAULT_GPU_TEXTURE_FILTER)).c_str())
      .value_or(DEFAULT_GPU_TEXTURE_FILTER);
  gpu_sprite_texture_filter =
    ParseTextureFilterName(
      si.GetStringValue("GPU", "SpriteTextureFilter", GetTextureFilterName(gpu_texture_filter)).c_str())
      .value_or(gpu_texture_filter);
  gpu_line_detect_mode =
    ParseLineDetectModeName(
      si.GetStringValue("GPU", "LineDetectMode", GetLineDetectModeName(DEFAULT_GPU_LINE_DETECT_MODE)).c_str())
      .value_or(DEFAULT_GPU_LINE_DETECT_MODE);
  gpu_downsample_mode =
    ParseDownsampleModeName(
      si.GetStringValue("GPU", "DownsampleMode", GetDownsampleModeName(DEFAULT_GPU_DOWNSAMPLE_MODE)).c_str())
      .value_or(DEFAULT_GPU_DOWNSAMPLE_MODE);
  gpu_downsample_scale = static_cast<u8>(si.GetUIntValue("GPU", "DownsampleScale", 1));
  gpu_wireframe_mode =
    ParseGPUWireframeMode(
      si.GetStringValue("GPU", "WireframeMode", GetGPUWireframeModeName(DEFAULT_GPU_WIREFRAME_MODE)).c_str())
      .value_or(DEFAULT_GPU_WIREFRAME_MODE);
  gpu_force_video_timing =
    ParseForceVideoTimingName(
      si.GetStringValue("GPU", "ForceVideoTiming", GetForceVideoTimingName(DEFAULT_FORCE_VIDEO_TIMING_MODE)).c_str())
      .value_or(DEFAULT_FORCE_VIDEO_TIMING_MODE);
  gpu_widescreen_hack = si.GetBoolValue("GPU", "WidescreenHack", false);
  gpu_texture_cache = si.GetBoolValue("GPU", "EnableTextureCache", false);
  display_24bit_chroma_smoothing = si.GetBoolValue("GPU", "ChromaSmoothing24Bit", false);
  gpu_pgxp_enable = si.GetBoolValue("GPU", "PGXPEnable", false);
  gpu_pgxp_culling = si.GetBoolValue("GPU", "PGXPCulling", true);
  gpu_pgxp_texture_correction = si.GetBoolValue("GPU", "PGXPTextureCorrection", true);
  gpu_pgxp_color_correction = si.GetBoolValue("GPU", "PGXPColorCorrection", false);
  gpu_pgxp_vertex_cache = si.GetBoolValue("GPU", "PGXPVertexCache", false);
  gpu_pgxp_cpu = si.GetBoolValue("GPU", "PGXPCPU", false);
  gpu_pgxp_preserve_proj_fp = si.GetBoolValue("GPU", "PGXPPreserveProjFP", false);
  gpu_pgxp_tolerance = si.GetFloatValue("GPU", "PGXPTolerance", -1.0f);
  gpu_pgxp_depth_buffer = si.GetBoolValue("GPU", "PGXPDepthBuffer", false);
  gpu_pgxp_disable_2d = si.GetBoolValue("GPU", "PGXPDisableOn2DPolygons", false);
  SetPGXPDepthClearThreshold(si.GetFloatValue("GPU", "PGXPDepthClearThreshold", DEFAULT_GPU_PGXP_DEPTH_THRESHOLD));

  display_deinterlacing_mode =
    ParseDisplayDeinterlacingMode(
      si.GetStringValue("GPU", "DeinterlacingMode", GetDisplayDeinterlacingModeName(DEFAULT_DISPLAY_DEINTERLACING_MODE))
        .c_str())
      .value_or(DEFAULT_DISPLAY_DEINTERLACING_MODE);
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
  display_rotation =
    ParseDisplayRotation(
      si.GetStringValue("Display", "Rotation", GetDisplayRotationName(DEFAULT_DISPLAY_ROTATION)).c_str())
      .value_or(DEFAULT_DISPLAY_ROTATION);
  display_scaling =
    ParseDisplayScaling(si.GetStringValue("Display", "Scaling", GetDisplayScalingName(DEFAULT_DISPLAY_SCALING)).c_str())
      .value_or(DEFAULT_DISPLAY_SCALING);
  display_exclusive_fullscreen_control =
    ParseDisplayExclusiveFullscreenControl(
      si.GetStringValue("Display", "ExclusiveFullscreenControl",
                        GetDisplayExclusiveFullscreenControlName(DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL))
        .c_str())
      .value_or(DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL);
  display_screenshot_mode =
    ParseDisplayScreenshotMode(
      si.GetStringValue("Display", "ScreenshotMode", GetDisplayScreenshotModeName(DEFAULT_DISPLAY_SCREENSHOT_MODE))
        .c_str())
      .value_or(DEFAULT_DISPLAY_SCREENSHOT_MODE);
  display_screenshot_format =
    ParseDisplayScreenshotFormat(si.GetStringValue("Display", "ScreenshotFormat",
                                                   GetDisplayScreenshotFormatName(DEFAULT_DISPLAY_SCREENSHOT_FORMAT))
                                   .c_str())
      .value_or(DEFAULT_DISPLAY_SCREENSHOT_FORMAT);
  display_screenshot_quality = static_cast<u8>(
    std::clamp<u32>(si.GetUIntValue("Display", "ScreenshotQuality", DEFAULT_DISPLAY_SCREENSHOT_QUALITY), 1, 100));
  display_optimal_frame_pacing = si.GetBoolValue("Display", "OptimalFramePacing", false);
  display_pre_frame_sleep = si.GetBoolValue("Display", "PreFrameSleep", false);
  display_pre_frame_sleep_buffer =
    si.GetFloatValue("Display", "PreFrameSleepBuffer", DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER);
  display_skip_presenting_duplicate_frames = si.GetBoolValue("Display", "SkipPresentingDuplicateFrames", false);
  display_vsync = si.GetBoolValue("Display", "VSync", false);
  display_disable_mailbox_presentation = si.GetBoolValue("Display", "DisableMailboxPresentation", false);
  display_force_4_3_for_24bit = si.GetBoolValue("Display", "Force4_3For24Bit", false);
  display_active_start_offset = static_cast<s16>(si.GetIntValue("Display", "ActiveStartOffset", 0));
  display_active_end_offset = static_cast<s16>(si.GetIntValue("Display", "ActiveEndOffset", 0));
  display_line_start_offset = static_cast<s8>(si.GetIntValue("Display", "LineStartOffset", 0));
  display_line_end_offset = static_cast<s8>(si.GetIntValue("Display", "LineEndOffset", 0));
  ImGuiManager::SetShowOSDMessages(si.GetBoolValue("Display", "ShowOSDMessages", true));
  display_show_fps = si.GetBoolValue("Display", "ShowFPS", false);
  display_show_speed = si.GetBoolValue("Display", "ShowSpeed", false);
  display_show_gpu_stats = si.GetBoolValue("Display", "ShowGPUStatistics", false);
  display_show_resolution = si.GetBoolValue("Display", "ShowResolution", false);
  display_show_latency_stats = si.GetBoolValue("Display", "ShowLatencyStatistics", false);
  display_show_cpu_usage = si.GetBoolValue("Display", "ShowCPU", false);
  display_show_gpu_usage = si.GetBoolValue("Display", "ShowGPU", false);
  display_show_frame_times = si.GetBoolValue("Display", "ShowFrameTimes", false);
  display_show_status_indicators = si.GetBoolValue("Display", "ShowStatusIndicators", true);
  display_show_inputs = si.GetBoolValue("Display", "ShowInputs", false);
  display_show_enhancements = si.GetBoolValue("Display", "ShowEnhancements", false);
  display_stretch_vertically = si.GetBoolValue("Display", "StretchVertically", false);
  display_auto_resize_window = si.GetBoolValue("Display", "AutoResizeWindow", false);
  display_osd_scale = si.GetFloatValue("Display", "OSDScale", DEFAULT_OSD_SCALE);

  save_state_compression = ParseSaveStateCompressionModeName(
                             si.GetStringValue("Main", "SaveStateCompression",
                                               GetSaveStateCompressionModeName(DEFAULT_SAVE_STATE_COMPRESSION_MODE))
                               .c_str())
                             .value_or(DEFAULT_SAVE_STATE_COMPRESSION_MODE);

  cdrom_readahead_sectors =
    static_cast<u8>(si.GetIntValue("CDROM", "ReadaheadSectors", DEFAULT_CDROM_READAHEAD_SECTORS));
  cdrom_mechacon_version =
    ParseCDROMMechVersionName(
      si.GetStringValue("CDROM", "MechaconVersion", GetCDROMMechVersionName(DEFAULT_CDROM_MECHACON_VERSION)).c_str())
      .value_or(DEFAULT_CDROM_MECHACON_VERSION);
  cdrom_region_check = si.GetBoolValue("CDROM", "RegionCheck", false);
  cdrom_subq_skew = si.GetBoolValue("CDROM", "SubQSkew", false);
  cdrom_load_image_to_ram = si.GetBoolValue("CDROM", "LoadImageToRAM", false);
  cdrom_load_image_patches = si.GetBoolValue("CDROM", "LoadImagePatches", false);
  cdrom_mute_cd_audio = si.GetBoolValue("CDROM", "MuteCDAudio", false);
  cdrom_read_speedup = si.GetIntValue("CDROM", "ReadSpeedup", 1);
  cdrom_seek_speedup = si.GetIntValue("CDROM", "SeekSpeedup", 1);

  audio_backend =
    AudioStream::ParseBackendName(
      si.GetStringValue("Audio", "Backend", AudioStream::GetBackendName(AudioStream::DEFAULT_BACKEND)).c_str())
      .value_or(AudioStream::DEFAULT_BACKEND);
  audio_driver = si.GetStringValue("Audio", "Driver");
  audio_output_device = si.GetStringValue("Audio", "OutputDevice");
  audio_stream_parameters.Load(si, "Audio");
  audio_output_volume = si.GetUIntValue("Audio", "OutputVolume", 100);
  audio_fast_forward_volume = si.GetUIntValue("Audio", "FastForwardVolume", 100);

  audio_output_muted = si.GetBoolValue("Audio", "OutputMuted", false);

  use_old_mdec_routines = si.GetBoolValue("Hacks", "UseOldMDECRoutines", false);
  export_shared_memory = si.GetBoolValue("Hacks", "ExportSharedMemory", false);
  pcdrv_enable = si.GetBoolValue("PCDrv", "Enabled", false);
  pcdrv_enable_writes = si.GetBoolValue("PCDrv", "EnableWrites", false);
  pcdrv_root = si.GetStringValue("PCDrv", "Root");

  dma_max_slice_ticks = si.GetIntValue("Hacks", "DMAMaxSliceTicks", DEFAULT_DMA_MAX_SLICE_TICKS);
  dma_halt_ticks = si.GetIntValue("Hacks", "DMAHaltTicks", DEFAULT_DMA_HALT_TICKS);
  gpu_fifo_size = static_cast<u32>(si.GetIntValue("Hacks", "GPUFIFOSize", DEFAULT_GPU_FIFO_SIZE));
  gpu_max_run_ahead = si.GetIntValue("Hacks", "GPUMaxRunAhead", DEFAULT_GPU_MAX_RUN_AHEAD);

  bios_tty_logging = si.GetBoolValue("BIOS", "TTYLogging", false);
  bios_patch_fast_boot = si.GetBoolValue("BIOS", "PatchFastBoot", DEFAULT_FAST_BOOT_VALUE);
  bios_fast_forward_boot = si.GetBoolValue("BIOS", "FastForwardBoot", false);

  multitap_mode =
    ParseMultitapModeName(
      controller_si.GetStringValue("ControllerPorts", "MultitapMode", GetMultitapModeName(DEFAULT_MULTITAP_MODE))
        .c_str())
      .value_or(DEFAULT_MULTITAP_MODE);

  const std::array<bool, 2> mtap_enabled = {{IsPort1MultitapEnabled(), IsPort2MultitapEnabled()}};
  for (u32 pad = 0; pad < NUM_CONTROLLER_AND_CARD_PORTS; pad++)
  {
    // Ignore types when multitap not enabled
    const auto [port, slot] = Controller::ConvertPadToPortAndSlot(pad);
    if (Controller::PadIsMultitapSlot(pad) && !mtap_enabled[port])
    {
      controller_types[pad] = ControllerType::None;
      continue;
    }

    const ControllerType default_type = (pad == 0) ? DEFAULT_CONTROLLER_1_TYPE : DEFAULT_CONTROLLER_2_TYPE;
    const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(controller_si.GetTinyStringValue(
      Controller::GetSettingsSection(pad).c_str(), "Type", Controller::GetControllerInfo(default_type)->name));
    controller_types[pad] = cinfo ? cinfo->type : default_type;
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
  log_to_console = si.GetBoolValue("Logging", "LogToConsole", false);
  log_to_debug = si.GetBoolValue("Logging", "LogToDebug", false);
  log_to_window = si.GetBoolValue("Logging", "LogToWindow", false);
  log_to_file = si.GetBoolValue("Logging", "LogToFile", false);

  debugging.show_vram = si.GetBoolValue("Debug", "ShowVRAM");
  debugging.dump_cpu_to_vram_copies = si.GetBoolValue("Debug", "DumpCPUToVRAMCopies");
  debugging.dump_vram_to_cpu_copies = si.GetBoolValue("Debug", "DumpVRAMToCPUCopies");

#ifndef __ANDROID__
  debugging.enable_gdb_server = si.GetBoolValue("Debug", "EnableGDBServer");
  debugging.gdb_server_port = static_cast<u16>(si.GetUIntValue("Debug", "GDBServerPort", DEFAULT_GDB_SERVER_PORT));
#endif

  debugging.show_gpu_state = si.GetBoolValue("Debug", "ShowGPUState");
  debugging.show_cdrom_state = si.GetBoolValue("Debug", "ShowCDROMState");
  debugging.show_spu_state = si.GetBoolValue("Debug", "ShowSPUState");
  debugging.show_timers_state = si.GetBoolValue("Debug", "ShowTimersState");
  debugging.show_mdec_state = si.GetBoolValue("Debug", "ShowMDECState");
  debugging.show_dma_state = si.GetBoolValue("Debug", "ShowDMAState");

  texture_replacements.enable_texture_replacements =
    si.GetBoolValue("TextureReplacements", "EnableTextureReplacements", false);
  texture_replacements.enable_vram_write_replacements =
    si.GetBoolValue("TextureReplacements", "EnableVRAMWriteReplacements", false);
  texture_replacements.preload_textures = si.GetBoolValue("TextureReplacements", "PreloadTextures", false);
  texture_replacements.dump_textures = si.GetBoolValue("TextureReplacements", "DumpTextures", false);
  texture_replacements.dump_replaced_textures = si.GetBoolValue("TextureReplacements", "DumpReplacedTextures", true);
  texture_replacements.dump_vram_writes = si.GetBoolValue("TextureReplacements", "DumpVRAMWrites", false);

  texture_replacements.config.dump_texture_pages = si.GetBoolValue("TextureReplacements", "DumpTexturePages", false);
  texture_replacements.config.dump_full_texture_pages =
    si.GetBoolValue("TextureReplacements", "DumpFullTexturePages", false);
  texture_replacements.config.dump_texture_force_alpha_channel =
    si.GetBoolValue("TextureReplacements", "DumpTextureForceAlphaChannel", false);
  texture_replacements.config.dump_vram_write_force_alpha_channel =
    si.GetBoolValue("TextureReplacements", "DumpVRAMWriteForceAlphaChannel", true);
  texture_replacements.config.dump_c16_textures = si.GetBoolValue("TextureReplacements", "DumpC16Textures", false);
  texture_replacements.config.reduce_palette_range = si.GetBoolValue("TextureReplacements", "ReducePaletteRange", true);
  texture_replacements.config.convert_copies_to_writes =
    si.GetBoolValue("TextureReplacements", "ConvertCopiesToWrites", false);
  texture_replacements.config.replacement_scale_linear_filter =
    si.GetBoolValue("TextureReplacements", "ReplacementScaleLinearFilter", false);

  texture_replacements.config.max_vram_write_splits = si.GetUIntValue("TextureReplacements", "MaxVRAMWriteSplits", 0u);
  texture_replacements.config.max_vram_write_coalesce_width =
    si.GetUIntValue("TextureReplacements", "MaxVRAMWriteCoalesceWidth", 0u);
  texture_replacements.config.max_vram_write_coalesce_height =
    si.GetUIntValue("TextureReplacements", "MaxVRAMWriteCoalesceHeight", 0u);

  texture_replacements.config.texture_dump_width_threshold =
    si.GetUIntValue("TextureReplacements", "DumpTextureWidthThreshold", 16);
  texture_replacements.config.texture_dump_height_threshold =
    si.GetUIntValue("TextureReplacements", "DumpTextureHeightThreshold", 16);
  texture_replacements.config.vram_write_dump_width_threshold =
    si.GetUIntValue("TextureReplacements", "DumpVRAMWriteWidthThreshold", 128);
  texture_replacements.config.vram_write_dump_height_threshold =
    si.GetUIntValue("TextureReplacements", "DumpVRAMWriteHeightThreshold", 128);

#ifdef __ANDROID__
  // Android users are incredibly silly and don't understand that stretch is in the aspect ratio list...
  if (si.GetBoolValue("Display", "Stretch", false))
    display_aspect_ratio = DisplayAspectRatio::MatchWindow;
#endif
}

void Settings::Save(SettingsInterface& si, bool ignore_base) const
{
  si.SetStringValue("Console", "Region", GetConsoleRegionName(region));
  si.SetBoolValue("Console", "Enable8MBRAM", enable_8mb_ram);

  si.SetFloatValue("Main", "EmulationSpeed", emulation_speed);
  si.SetFloatValue("Main", "FastForwardSpeed", fast_forward_speed);
  si.SetFloatValue("Main", "TurboSpeed", turbo_speed);

  if (!ignore_base)
  {
    si.SetBoolValue("Main", "SyncToHostRefreshRate", sync_to_host_refresh_rate);
    si.SetBoolValue("Main", "InhibitScreensaver", inhibit_screensaver);
    si.SetBoolValue("Main", "StartPaused", start_paused);
    si.SetBoolValue("Main", "StartFullscreen", start_fullscreen);
    si.SetBoolValue("Main", "PauseOnFocusLoss", pause_on_focus_loss);
    si.SetBoolValue("Main", "PauseOnControllerDisconnection", pause_on_controller_disconnection);
    si.SetBoolValue("Main", "SaveStateOnExit", save_state_on_exit);
    si.SetBoolValue("Main", "CreateSaveStateBackups", create_save_state_backups);
    si.SetStringValue("Main", "SaveStateCompression", GetSaveStateCompressionModeName(save_state_compression));
    si.SetBoolValue("Main", "ConfirmPowerOff", confim_power_off);
    si.SetBoolValue("Main", "EnableDiscordPresence", enable_discord_presence);
  }

  si.SetBoolValue("Main", "LoadDevicesFromSaveStates", load_devices_from_save_states);
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

  if (!ignore_base)
  {
    si.SetBoolValue("GPU", "UseDebugDevice", gpu_use_debug_device);
    si.SetBoolValue("GPU", "DisableShaderCache", gpu_disable_shader_cache);
    si.SetBoolValue("GPU", "DisableDualSourceBlend", gpu_disable_dual_source_blend);
    si.SetBoolValue("GPU", "DisableFramebufferFetch", gpu_disable_framebuffer_fetch);
    si.SetBoolValue("GPU", "DisableTextureBuffers", gpu_disable_texture_buffers);
    si.SetBoolValue("GPU", "DisableTextureCopyToSelf", gpu_disable_texture_copy_to_self);
    si.SetBoolValue("GPU", "DisableMemoryImport", gpu_disable_memory_import);
    si.SetBoolValue("GPU", "DisableRasterOrderViews", gpu_disable_raster_order_views);
  }

  si.SetBoolValue("GPU", "PerSampleShading", gpu_per_sample_shading);
  si.SetBoolValue("GPU", "UseThread", gpu_use_thread);
  si.SetBoolValue("GPU", "UseSoftwareRendererForReadbacks", gpu_use_software_renderer_for_readbacks);
  si.SetBoolValue("GPU", "TrueColor", gpu_true_color);
  si.SetBoolValue("GPU", "ScaledDithering", gpu_scaled_dithering);
  si.SetBoolValue("GPU", "ForceRoundTextureCoordinates", gpu_force_round_texcoords);
  si.SetBoolValue("GPU", "AccurateBlending", gpu_accurate_blending);
  si.SetStringValue("GPU", "TextureFilter", GetTextureFilterName(gpu_texture_filter));
  si.SetStringValue(
    "GPU", "SpriteTextureFilter",
    (gpu_sprite_texture_filter != gpu_texture_filter) ? GetTextureFilterName(gpu_sprite_texture_filter) : "");
  si.SetStringValue("GPU", "LineDetectMode", GetLineDetectModeName(gpu_line_detect_mode));
  si.SetStringValue("GPU", "DownsampleMode", GetDownsampleModeName(gpu_downsample_mode));
  si.SetUIntValue("GPU", "DownsampleScale", gpu_downsample_scale);
  si.SetStringValue("GPU", "WireframeMode", GetGPUWireframeModeName(gpu_wireframe_mode));
  si.SetStringValue("GPU", "ForceVideoTiming", GetForceVideoTimingName(gpu_force_video_timing));
  si.SetBoolValue("GPU", "WidescreenHack", gpu_widescreen_hack);
  si.SetBoolValue("GPU", "EnableTextureCache", gpu_texture_cache);
  si.SetBoolValue("GPU", "ChromaSmoothing24Bit", display_24bit_chroma_smoothing);
  si.SetBoolValue("GPU", "PGXPEnable", gpu_pgxp_enable);
  si.SetBoolValue("GPU", "PGXPCulling", gpu_pgxp_culling);
  si.SetBoolValue("GPU", "PGXPTextureCorrection", gpu_pgxp_texture_correction);
  si.SetBoolValue("GPU", "PGXPColorCorrection", gpu_pgxp_color_correction);
  si.SetBoolValue("GPU", "PGXPVertexCache", gpu_pgxp_vertex_cache);
  si.SetBoolValue("GPU", "PGXPCPU", gpu_pgxp_cpu);
  si.SetBoolValue("GPU", "PGXPPreserveProjFP", gpu_pgxp_preserve_proj_fp);
  si.SetFloatValue("GPU", "PGXPTolerance", gpu_pgxp_tolerance);
  si.SetBoolValue("GPU", "PGXPDepthBuffer", gpu_pgxp_depth_buffer);
  si.SetBoolValue("GPU", "PGXPDisableOn2DPolygons", gpu_pgxp_disable_2d);
  si.SetFloatValue("GPU", "PGXPDepthClearThreshold", GetPGXPDepthClearThreshold());

  si.SetStringValue("GPU", "DeinterlacingMode", GetDisplayDeinterlacingModeName(display_deinterlacing_mode));
  si.SetStringValue("Display", "CropMode", GetDisplayCropModeName(display_crop_mode));
  si.SetIntValue("Display", "ActiveStartOffset", display_active_start_offset);
  si.SetIntValue("Display", "ActiveEndOffset", display_active_end_offset);
  si.SetIntValue("Display", "LineStartOffset", display_line_start_offset);
  si.SetIntValue("Display", "LineEndOffset", display_line_end_offset);
  si.SetBoolValue("Display", "Force4_3For24Bit", display_force_4_3_for_24bit);
  si.SetStringValue("Display", "AspectRatio", GetDisplayAspectRatioName(display_aspect_ratio));
  si.SetStringValue("Display", "Alignment", GetDisplayAlignmentName(display_alignment));
  si.SetStringValue("Display", "Rotation", GetDisplayRotationName(display_rotation));
  si.SetStringValue("Display", "Scaling", GetDisplayScalingName(display_scaling));
  si.SetBoolValue("Display", "OptimalFramePacing", display_optimal_frame_pacing);
  si.SetBoolValue("Display", "PreFrameSleep", display_pre_frame_sleep);
  si.SetBoolValue("Display", "SkipPresentingDuplicateFrames", display_skip_presenting_duplicate_frames);
  si.SetFloatValue("Display", "PreFrameSleepBuffer", display_pre_frame_sleep_buffer);
  si.SetBoolValue("Display", "VSync", display_vsync);
  si.SetBoolValue("Display", "DisableMailboxPresentation", display_disable_mailbox_presentation);
  si.SetStringValue("Display", "ExclusiveFullscreenControl",
                    GetDisplayExclusiveFullscreenControlName(display_exclusive_fullscreen_control));
  si.SetStringValue("Display", "ScreenshotMode", GetDisplayScreenshotModeName(display_screenshot_mode));
  si.SetStringValue("Display", "ScreenshotFormat", GetDisplayScreenshotFormatName(display_screenshot_format));
  si.SetUIntValue("Display", "ScreenshotQuality", display_screenshot_quality);
  si.SetIntValue("Display", "CustomAspectRatioNumerator", display_aspect_ratio_custom_numerator);
  si.GetIntValue("Display", "CustomAspectRatioDenominator", display_aspect_ratio_custom_denominator);
  if (!ignore_base)
  {
    si.SetBoolValue("Display", "ShowOSDMessages", ImGuiManager::IsShowingOSDMessages());
    si.SetBoolValue("Display", "ShowFPS", display_show_fps);
    si.SetBoolValue("Display", "ShowSpeed", display_show_speed);
    si.SetBoolValue("Display", "ShowResolution", display_show_resolution);
    si.SetBoolValue("Display", "ShowLatencyStatistics", display_show_latency_stats);
    si.SetBoolValue("Display", "ShowGPUStatistics", display_show_gpu_stats);
    si.SetBoolValue("Display", "ShowCPU", display_show_cpu_usage);
    si.SetBoolValue("Display", "ShowGPU", display_show_gpu_usage);
    si.SetBoolValue("Display", "ShowFrameTimes", display_show_frame_times);
    si.SetBoolValue("Display", "ShowStatusIndicators", display_show_status_indicators);
    si.SetBoolValue("Display", "ShowInputs", display_show_inputs);
    si.SetBoolValue("Display", "ShowEnhancements", display_show_enhancements);
    si.SetFloatValue("Display", "OSDScale", display_osd_scale);
  }

  si.SetBoolValue("Display", "StretchVertically", display_stretch_vertically);
  si.SetBoolValue("Display", "AutoResizeWindow", display_auto_resize_window);

  si.SetIntValue("CDROM", "ReadaheadSectors", cdrom_readahead_sectors);
  si.SetStringValue("CDROM", "MechaconVersion", GetCDROMMechVersionName(cdrom_mechacon_version));
  si.SetBoolValue("CDROM", "RegionCheck", cdrom_region_check);
  si.SetBoolValue("CDROM", "SubQSkew", cdrom_subq_skew);
  si.SetBoolValue("CDROM", "LoadImageToRAM", cdrom_load_image_to_ram);
  si.SetBoolValue("CDROM", "LoadImagePatches", cdrom_load_image_patches);
  si.SetBoolValue("CDROM", "MuteCDAudio", cdrom_mute_cd_audio);
  si.SetIntValue("CDROM", "ReadSpeedup", cdrom_read_speedup);
  si.SetIntValue("CDROM", "SeekSpeedup", cdrom_seek_speedup);

  si.SetStringValue("Audio", "Backend", AudioStream::GetBackendName(audio_backend));
  si.SetStringValue("Audio", "Driver", audio_driver.c_str());
  si.SetStringValue("Audio", "OutputDevice", audio_output_device.c_str());
  audio_stream_parameters.Save(si, "Audio");
  si.SetUIntValue("Audio", "OutputVolume", audio_output_volume);
  si.SetUIntValue("Audio", "FastForwardVolume", audio_fast_forward_volume);
  si.SetBoolValue("Audio", "OutputMuted", audio_output_muted);

  si.SetBoolValue("Hacks", "UseOldMDECRoutines", use_old_mdec_routines);
  si.SetBoolValue("Hacks", "ExportSharedMemory", export_shared_memory);

  if (!ignore_base)
  {
    si.SetIntValue("Hacks", "DMAMaxSliceTicks", dma_max_slice_ticks);
    si.SetIntValue("Hacks", "DMAHaltTicks", dma_halt_ticks);
    si.SetIntValue("Hacks", "GPUFIFOSize", gpu_fifo_size);
    si.SetIntValue("Hacks", "GPUMaxRunAhead", gpu_max_run_ahead);
  }

  si.SetBoolValue("PCDrv", "Enabled", pcdrv_enable);
  si.SetBoolValue("PCDrv", "EnableWrites", pcdrv_enable_writes);
  si.SetStringValue("PCDrv", "Root", pcdrv_root.c_str());

  si.SetBoolValue("BIOS", "TTYLogging", bios_tty_logging);
  si.SetBoolValue("BIOS", "PatchFastBoot", bios_patch_fast_boot);
  si.SetBoolValue("BIOS", "FastForwardBoot", bios_fast_forward_boot);

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(controller_types[i]);
    DebugAssert(cinfo);
    si.SetStringValue(Controller::GetSettingsSection(i).c_str(), "Type", cinfo->name);
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

  if (!ignore_base)
  {
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

#ifndef __ANDROID__
    si.SetBoolValue("Debug", "EnableGDBServer", debugging.enable_gdb_server);
    si.SetUIntValue("Debug", "GDBServerPort", debugging.gdb_server_port);
#endif

    si.SetBoolValue("Debug", "ShowGPUState", debugging.show_gpu_state);
    si.SetBoolValue("Debug", "ShowCDROMState", debugging.show_cdrom_state);
    si.SetBoolValue("Debug", "ShowSPUState", debugging.show_spu_state);
    si.SetBoolValue("Debug", "ShowTimersState", debugging.show_timers_state);
    si.SetBoolValue("Debug", "ShowMDECState", debugging.show_mdec_state);
    si.SetBoolValue("Debug", "ShowDMAState", debugging.show_dma_state);
  }

  si.SetBoolValue("TextureReplacements", "EnableTextureReplacements", texture_replacements.enable_texture_replacements);
  si.SetBoolValue("TextureReplacements", "EnableVRAMWriteReplacements",
                  texture_replacements.enable_vram_write_replacements);
  si.SetBoolValue("TextureReplacements", "PreloadTextures", texture_replacements.preload_textures);
  si.SetBoolValue("TextureReplacements", "DumpVRAMWrites", texture_replacements.dump_vram_writes);
  si.SetBoolValue("TextureReplacements", "DumpTextures", texture_replacements.dump_textures);
  si.SetBoolValue("TextureReplacements", "DumpReplacedTextures", texture_replacements.dump_replaced_textures);

  si.SetBoolValue("TextureReplacements", "DumpTexturePages", texture_replacements.config.dump_texture_pages);
  si.SetBoolValue("TextureReplacements", "DumpFullTexturePages", texture_replacements.config.dump_full_texture_pages);
  si.SetBoolValue("TextureReplacements", "DumpTextureForceAlphaChannel",
                  texture_replacements.config.dump_texture_force_alpha_channel);

  si.SetBoolValue("TextureReplacements", "DumpVRAMWriteForceAlphaChannel",
                  texture_replacements.config.dump_vram_write_force_alpha_channel);
  si.SetBoolValue("TextureReplacements", "DumpC16Textures", texture_replacements.config.dump_c16_textures);
  si.SetBoolValue("TextureReplacements", "ReducePaletteRange", texture_replacements.config.reduce_palette_range);
  si.SetBoolValue("TextureReplacements", "ConvertCopiesToWrites", texture_replacements.config.convert_copies_to_writes);
  si.SetBoolValue("TextureReplacements", "ReplacementScaleLinearFilter",
                  texture_replacements.config.replacement_scale_linear_filter);

  si.SetUIntValue("TextureReplacements", "MaxVRAMWriteSplits", texture_replacements.config.max_vram_write_splits);
  si.SetUIntValue("TextureReplacements", "MaxVRAMWriteCoalesceWidth",
                  texture_replacements.config.max_vram_write_coalesce_width);
  si.GetUIntValue("TextureReplacements", "MaxVRAMWriteCoalesceHeight",
                  texture_replacements.config.max_vram_write_coalesce_height);

  si.SetUIntValue("TextureReplacements", "DumpTextureWidthThreshold",
                  texture_replacements.config.texture_dump_width_threshold);
  si.SetUIntValue("TextureReplacements", "DumpTextureHeightThreshold",
                  texture_replacements.config.texture_dump_height_threshold);
  si.SetUIntValue("TextureReplacements", "DumpVRAMWriteWidthThreshold",
                  texture_replacements.config.vram_write_dump_width_threshold);
  si.SetUIntValue("TextureReplacements", "DumpVRAMWriteHeightThreshold",
                  texture_replacements.config.vram_write_dump_height_threshold);
}

void Settings::Clear(SettingsInterface& si)
{
  si.ClearSection("Main");
  si.ClearSection("Console");
  si.ClearSection("CPU");
  si.ClearSection("GPU");
  si.ClearSection("Display");
  si.ClearSection("CDROM");
  si.ClearSection("Audio");
  si.ClearSection("Hacks");
  si.ClearSection("PCDrv");
  si.ClearSection("BIOS");

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    si.ClearSection(Controller::GetSettingsSection(i).c_str());

  si.ClearSection("MemoryCards");

  // Can't wipe out this section, because input profiles.
  si.DeleteValue("ControllerPorts", "MultitapMode");

  si.ClearSection("Cheevos");
  si.ClearSection("Logging");
  si.ClearSection("Debug");
  si.ClearSection("TextureReplacements");
}

bool Settings::TextureReplacementSettings::Configuration::operator==(const Configuration& rhs) const
{
  return (dump_texture_pages == rhs.dump_texture_pages && dump_full_texture_pages == rhs.dump_full_texture_pages &&
          dump_texture_force_alpha_channel == rhs.dump_texture_force_alpha_channel &&
          dump_vram_write_force_alpha_channel == rhs.dump_vram_write_force_alpha_channel &&
          dump_c16_textures == rhs.dump_c16_textures && reduce_palette_range == rhs.reduce_palette_range &&
          convert_copies_to_writes == rhs.convert_copies_to_writes &&
          replacement_scale_linear_filter == rhs.replacement_scale_linear_filter &&
          max_vram_write_splits == rhs.max_vram_write_splits &&
          max_vram_write_coalesce_width == rhs.max_vram_write_coalesce_width &&
          max_vram_write_coalesce_height == rhs.max_vram_write_coalesce_height &&
          texture_dump_width_threshold == rhs.texture_dump_width_threshold &&
          texture_dump_height_threshold == rhs.texture_dump_height_threshold &&
          vram_write_dump_width_threshold == rhs.vram_write_dump_width_threshold &&
          vram_write_dump_height_threshold == rhs.vram_write_dump_height_threshold);
}

bool Settings::TextureReplacementSettings::Configuration::operator!=(const Configuration& rhs) const
{
  return !operator==(rhs);
}

bool Settings::TextureReplacementSettings::operator==(const TextureReplacementSettings& rhs) const
{
  return (enable_texture_replacements == rhs.enable_texture_replacements &&
          enable_vram_write_replacements == rhs.enable_vram_write_replacements &&
          preload_textures == rhs.preload_textures && dump_textures == rhs.dump_textures &&
          dump_replaced_textures == rhs.dump_replaced_textures && dump_vram_writes == rhs.dump_vram_writes &&
          config == rhs.config);
}

bool Settings::TextureReplacementSettings::operator!=(const TextureReplacementSettings& rhs) const
{
  return !operator==(rhs);
}

std::string Settings::TextureReplacementSettings::Configuration::ExportToYAML(bool comment) const
{
  static constexpr const char CONFIG_TEMPLATE[] = R"(# DuckStation Texture Replacement Configuration
# This file allows you to set a per-game configuration for the dumping and
# replacement system, avoiding the need to use the normal per-game settings
# when moving files to a different computer. It also allows for the definition
# of texture aliases, for reducing duplicate files.
#
# All options are commented out by default. If an option is commented, the user's
# current setting will be used instead. If an option is defined in this file, it
# will always take precedence over the user's choice.

# Enables texture page dumping mode.
# Instead of tracking VRAM writes and attempting to identify the "real" size of
# textures, create sub-rectangles from pages based on how they are drawn. In
# most games, this will lead to significant duplication in dumps, and reduce
# replacement reliability. However, some games are incompatible with write
# tracking, and must use page mode.
{}DumpTexturePages: {}

# Dumps full texture pages instead of sub-rectangles.
# 256x256 pages will be dumped/replaced instead.
{}DumpFullTexturePages: {}

# Enables the dumping of direct textures (i.e. C16 format).
# Most games do not use direct textures, and when they do, it is usually for
# post-processing or FMVs. Ignoring C16 textures typically reduces garbage/false
# positive texture dumps, however, some games may require it.
{}DumpC16Textures: {}

# Reduces the size of palettes (i.e. CLUTs) to only those indices that are used.
# This can help reduce duplication and improve replacement reliability in games
# that use 8-bit textures, but do not reserve or use the full 1x256 region in
# video memory for storage of the palette. When replacing textures dumped with
# this option enabled, CPU usage on the GPU thread does increase trivially,
# however, generally it is worthwhile for the reliability improvement. Games
# that require this option include Metal Gear Solid.
{}ReducePaletteRange: {}

# Converts VRAM copies to VRAM writes, when a copy of performed into a previously
# tracked VRAM write. This is required for some games that construct animated
# textures by copying and replacing small portions of the texture with the parts
# that are animated. Generally this option will cause duplication when dumping,
# but it is required in some games, such as Final Fantasy VIII.
{}ConvertCopiesToWrites: {}

# Determines the maximum number of times a VRAM write/upload can be split, before
# it is discarded and no longer tracked. This is required for games that partially
# overwrite texture data, such as Gran Turismo.
{}MaxVRAMWriteSplits: {}

# Determines the maximum size of an incoming VRAM write that will be merged with
# another write to the left/above of the incoming write. Needed for games that
# upload textures one line at a time. These games will log "tracking VRAM write
# of Nx1" repeatedly during loading. If the upload size is 1, then you can set
# the corresponding maximum coalesce dimension to 1 to merge these uploads,
# which should enable these textures to be dumped/replaced.
{}MaxVRAMWriteCoalesceWidth: {}
{}MaxVRAMWriteCoalesceHeight: {}

# Determines the minimum size of a texture that will be dumped. Textures with a
# width/height smaller than this value will be ignored.
{}DumpTextureWidthThreshold: {}
{}DumpTextureHeightThreshold: {}

# Determines the minimum size of a VRAM write that will be dumped, in background
# dumping mode. Uploads smaller than this size will be ignored.
{}DumpVRAMWriteWidthThreshold: {}
{}DumpVRAMWriteHeightThreshold: {}

# Enables the use of a bilinear filter when scaling replacement textures.
# If more than one replacement texture in a 256x256 texture page has a different
# scaling over the native resolution, or the texture page is not covered, a
# bilinear filter will be used to resize/stretch the replacement texture, and/or
# the original native data.
{}ReplacementScaleLinearFilter: {}

# Use this section to define replacement aliases. One line per replacement
# texture, with the key set to the source ID, and the value set to the filename
# which should be loaded as a replacement. For example, without the newline,
# or keep the multi-line separator.
#Aliases:
  # Alias-Texture-Name: Path-To-Texture
  #  texupload-P4-AAAAAAAAAAAAAAAA-BBBBBBBBBBBBBBBB-64x256-0-192-64x64-P0-14: |
  #    texupload-P4-BBBBBBBBBBBBBBBB-BBBBBBBBBBBBBBBB-64x256-0-64-64x64-P0-13.png
  #  texupload-P4-AAAAAAAAAAAAAAAA-BBBBBBBBBBBBBBBB-64x256-0-192-64x64-P0-14: mytexture.png
)";

  const std::string_view comment_str = comment ? "#" : "";
  return fmt::format(CONFIG_TEMPLATE, comment_str, dump_texture_pages, // DumpTexturePages
                     comment_str, dump_full_texture_pages,             // DumpFullTexturePages
                     comment_str, dump_c16_textures,                   // DumpC16Textures
                     comment_str, reduce_palette_range,                // ReducePaletteRange
                     comment_str, convert_copies_to_writes,            // ConvertCopiesToWrites
                     comment_str, max_vram_write_splits,               // MaxVRAMWriteSplits
                     comment_str, max_vram_write_coalesce_width,       // MaxVRAMWriteCoalesceWidth
                     comment_str, max_vram_write_coalesce_height,      // MaxVRAMWriteCoalesceHeight
                     comment_str, texture_dump_width_threshold,        // DumpTextureWidthThreshold
                     comment_str, texture_dump_height_threshold,       // DumpTextureHeightThreshold
                     comment_str, vram_write_dump_width_threshold,     // DumpVRAMWriteWidthThreshold
                     comment_str, vram_write_dump_height_threshold,    // DumpVRAMWriteHeightThreshold
                     comment_str, replacement_scale_linear_filter);    // ReplacementScaleLinearFilter
}

void Settings::FixIncompatibleSettings(bool display_osd_messages)
{
  if (g_settings.disable_all_enhancements)
  {
    g_settings.cpu_overclock_enable = false;
    g_settings.cpu_overclock_active = false;
    g_settings.enable_8mb_ram = false;
    g_settings.gpu_resolution_scale = 1;
    g_settings.gpu_multisamples = 1;
    g_settings.gpu_per_sample_shading = false;
    g_settings.gpu_true_color = false;
    g_settings.gpu_scaled_dithering = false;
    g_settings.gpu_force_round_texcoords = false;
    g_settings.gpu_texture_filter = GPUTextureFilter::Nearest;
    g_settings.gpu_sprite_texture_filter = GPUTextureFilter::Nearest;
    g_settings.gpu_line_detect_mode = GPULineDetectMode::Disabled;
    g_settings.gpu_force_video_timing = ForceVideoTimingMode::Disabled;
    g_settings.gpu_widescreen_hack = false;
    g_settings.gpu_texture_cache = false;
    g_settings.gpu_pgxp_enable = false;
    g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Adaptive;
    g_settings.display_24bit_chroma_smoothing = false;
    g_settings.cdrom_read_speedup = 1;
    g_settings.cdrom_seek_speedup = 1;
    g_settings.cdrom_mute_cd_audio = false;
    g_settings.texture_replacements.enable_vram_write_replacements = false;
    g_settings.use_old_mdec_routines = false;
    g_settings.pcdrv_enable = false;
    g_settings.bios_patch_fast_boot = false;
  }

  // fast forward boot requires fast boot
  g_settings.bios_fast_forward_boot = g_settings.bios_patch_fast_boot && g_settings.bios_fast_forward_boot;

  if (g_settings.pcdrv_enable && g_settings.pcdrv_root.empty())
  {
    Host::AddKeyedOSDMessage("pcdrv_disabled_no_root",
                             TRANSLATE_STR("OSDMessage", "Disabling PCDrv because no root directory is specified."),
                             Host::OSD_WARNING_DURATION);
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
  else
  {
    g_settings.gpu_pgxp_culling = false;
    g_settings.gpu_pgxp_texture_correction = false;
    g_settings.gpu_pgxp_color_correction = false;
    g_settings.gpu_pgxp_vertex_cache = false;
    g_settings.gpu_pgxp_cpu = false;
    g_settings.gpu_pgxp_preserve_proj_fp = false;
    g_settings.gpu_pgxp_depth_buffer = false;
    g_settings.gpu_pgxp_disable_2d = false;
  }

#ifndef ENABLE_MMAP_FASTMEM
  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::MMap)
  {
    WARNING_LOG("mmap fastmem is not available on this platform, using LUT instead.");
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
    Host::AddKeyedOSDMessage("rewind_disabled",
                             TRANSLATE_STR("OSDMessage", "Rewind is disabled because runahead is enabled."),
                             Host::OSD_WARNING_DURATION);
    g_settings.rewind_enable = false;
  }

  if (g_settings.IsRunaheadEnabled())
  {
    // Block linking is good for performance, but hurts when regularly loading (i.e. runahead), since everything has to
    // be unlinked. Which would be thousands of blocks.
    if (g_settings.cpu_recompiler_block_linking)
    {
      WARNING_LOG("Disabling block linking due to runahead.");
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
  const bool any_logs_enabled = (log_to_console || log_to_debug || log_to_window || log_to_file);
  Log::SetLogLevel(any_logs_enabled ? log_level : Log::Level::None);
  Log::SetLogFilter(any_logs_enabled ? std::string_view(log_filter) : std::string_view());
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

static constexpr const std::array s_log_level_names = {
  "None", "Error", "Warning", "Info", "Verbose", "Dev", "Debug", "Trace",
};
static constexpr const std::array s_log_level_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "None", "LogLevel"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Error", "LogLevel"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Warning", "LogLevel"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Information", "LogLevel"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Verbose", "LogLevel"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Developer", "LogLevel"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Debug", "LogLevel"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Trace", "LogLevel"),
};

std::optional<Log::Level> Settings::ParseLogLevelName(const char* str)
{
  int index = 0;
  for (const char* name : s_log_level_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<Log::Level>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetLogLevelName(Log::Level level)
{
  return s_log_level_names[static_cast<size_t>(level)];
}

const char* Settings::GetLogLevelDisplayName(Log::Level level)
{
  return Host::TranslateToCString("Settings", s_log_level_display_names[static_cast<size_t>(level)], "LogLevel");
}

static constexpr const std::array s_console_region_names = {
  "Auto",
  "NTSC-J",
  "NTSC-U",
  "PAL",
};
static constexpr const std::array s_console_region_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Auto-Detect", "ConsoleRegion"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "NTSC-J (Japan)", "ConsoleRegion"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "NTSC-U/C (US, Canada)", "ConsoleRegion"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "PAL (Europe, Australia)", "ConsoleRegion"),
};

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
  return s_console_region_names[static_cast<size_t>(region)];
}

const char* Settings::GetConsoleRegionDisplayName(ConsoleRegion region)
{
  return Host::TranslateToCString("Settings", s_console_region_display_names[static_cast<size_t>(region)],
                                  "ConsoleRegion");
}

static constexpr const std::array s_disc_region_names = {
  "NTSC-J", "NTSC-U", "PAL", "Other", "Non-PS1",
};
static constexpr const std::array s_disc_region_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "NTSC-J (Japan)", "DiscRegion"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "NTSC-U/C (US, Canada)", "DiscRegion"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "PAL (Europe, Australia)", "DiscRegion"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Other", "DiscRegion"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Non-PS1", "DiscRegion"),
};

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
  return s_disc_region_names[static_cast<size_t>(region)];
}

const char* Settings::GetDiscRegionDisplayName(DiscRegion region)
{
  return Host::TranslateToCString("Settings", s_disc_region_display_names[static_cast<size_t>(region)], "DiscRegion");
}

static constexpr const std::array s_cpu_execution_mode_names = {
  "Interpreter",
  "CachedInterpreter",
  "Recompiler",
  "NewRec",
};
static constexpr const std::array s_cpu_execution_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Interpreter (Slowest)", "CPUExecutionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Cached Interpreter (Faster)", "CPUExecutionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Recompiler (Fastest)", "CPUExecutionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "New Recompiler (Experimental)", "CPUExecutionMode"),
};

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
  return Host::TranslateToCString("Settings", s_cpu_execution_mode_display_names[static_cast<size_t>(mode)],
                                  "CPUExecutionMode");
}

static constexpr const std::array s_cpu_fastmem_mode_names = {
  "Disabled",
  "MMap",
  "LUT",
};
static constexpr const std::array s_cpu_fastmem_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Disabled (Slowest)", "CPUFastmemMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "MMap (Hardware, Fastest, 64-Bit Only)", "CPUFastmemMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "LUT (Faster)", "CPUFastmemMode"),
};

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
  return Host::TranslateToCString("Settings", s_cpu_fastmem_mode_display_names[static_cast<size_t>(mode)],
                                  "CPUFastmemMode");
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
  TRANSLATE_DISAMBIG_NOOP("Settings", "Automatic", "GPURenderer"),
#ifdef _WIN32
  TRANSLATE_DISAMBIG_NOOP("Settings", "Direct3D 11", "GPURenderer"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Direct3D 12", "GPURenderer"),
#endif
#ifdef __APPLE__
  TRANSLATE_DISAMBIG_NOOP("Settings", "Metal", "GPURenderer"),
#endif
#ifdef ENABLE_VULKAN
  TRANSLATE_DISAMBIG_NOOP("Settings", "Vulkan", "GPURenderer"),
#endif
#ifdef ENABLE_OPENGL
  TRANSLATE_DISAMBIG_NOOP("Settings", "OpenGL", "GPURenderer"),
#endif
  TRANSLATE_DISAMBIG_NOOP("Settings", "Software", "GPURenderer"),
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
  return s_gpu_renderer_names[static_cast<size_t>(renderer)];
}

const char* Settings::GetRendererDisplayName(GPURenderer renderer)
{
  return Host::TranslateToCString("Settings", s_gpu_renderer_display_names[static_cast<size_t>(renderer)],
                                  "GPURenderer");
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

GPURenderer Settings::GetRendererForRenderAPI(RenderAPI api)
{
  switch (api)
  {
#ifdef _WIN32
    case RenderAPI::D3D11:
      return GPURenderer::HardwareD3D11;

    case RenderAPI::D3D12:
      return GPURenderer::HardwareD3D12;
#endif

#ifdef __APPLE__
    case RenderAPI::Metal:
      return GPURenderer::HardwareMetal;
#endif

#ifdef ENABLE_VULKAN
    case RenderAPI::Vulkan:
      return GPURenderer::HardwareVulkan;
#endif

#ifdef ENABLE_OPENGL
    case RenderAPI::OpenGL:
    case RenderAPI::OpenGLES:
      return GPURenderer::HardwareOpenGL;
#endif

    default:
      return GPURenderer::Automatic;
  }
}

GPURenderer Settings::GetAutomaticRenderer()
{
  return GetRendererForRenderAPI(GPUDevice::GetPreferredAPI());
}

static constexpr const std::array s_texture_filter_names = {
  "Nearest", "Bilinear", "BilinearBinAlpha", "JINC2", "JINC2BinAlpha", "xBR", "xBRBinAlpha",
};
static constexpr const std::array s_texture_filter_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Nearest-Neighbor", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear (No Edge Blending)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "JINC2 (Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "JINC2 (Slow, No Edge Blending)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "xBR (Very Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "xBR (Very Slow, No Edge Blending)", "GPUTextureFilter"),
};

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
  return s_texture_filter_names[static_cast<size_t>(filter)];
}

const char* Settings::GetTextureFilterDisplayName(GPUTextureFilter filter)
{
  return Host::TranslateToCString("Settings", s_texture_filter_display_names[static_cast<size_t>(filter)],
                                  "GPUTextureFilter");
}

static constexpr const std::array s_line_detect_mode_names = {
  "Disabled",
  "Quads",
  "BasicTriangles",
  "AggressiveTriangles",
};
static constexpr const std::array s_line_detect_mode_detect_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Disabled", "GPULineDetectMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Quads", "GPULineDetectMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Triangles (Basic)", "GPULineDetectMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Triangles (Aggressive)", "GPULineDetectMode"),
};

std::optional<GPULineDetectMode> Settings::ParseLineDetectModeName(const char* str)
{
  int index = 0;
  for (const char* name : s_line_detect_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<GPULineDetectMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetLineDetectModeName(GPULineDetectMode mode)
{
  return s_line_detect_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetLineDetectModeDisplayName(GPULineDetectMode mode)
{
  return Host::TranslateToCString("Settings", s_line_detect_mode_detect_names[static_cast<size_t>(mode)],
                                  "GPULineDetectMode");
}

static constexpr const std::array s_downsample_mode_names = {"Disabled", "Box", "Adaptive"};
static constexpr const std::array s_downsample_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Disabled", "GPUDownsampleMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Box (Downsample 3D/Smooth All)", "GPUDownsampleMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Adaptive (Preserve 3D/Smooth 2D)", "GPUDownsampleMode")};

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
  return s_downsample_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetDownsampleModeDisplayName(GPUDownsampleMode mode)
{
  return Host::TranslateToCString("Settings", s_downsample_mode_display_names[static_cast<size_t>(mode)],
                                  "GPUDownsampleMode");
}

static constexpr const std::array s_wireframe_mode_names = {"Disabled", "OverlayWireframe", "OnlyWireframe"};
static constexpr const std::array s_wireframe_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Disabled", "GPUWireframeMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Overlay Wireframe", "GPUWireframeMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Only Wireframe", "GPUWireframeMode")};

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
  return s_wireframe_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetGPUWireframeModeDisplayName(GPUWireframeMode mode)
{
  return Host::TranslateToCString("Settings", s_wireframe_mode_display_names[static_cast<size_t>(mode)],
                                  "GPUWireframeMode");
}

static constexpr const std::array s_display_deinterlacing_mode_names = {
  "Disabled", "Weave", "Blend", "Adaptive", "Progressive",
};
static constexpr const std::array s_display_deinterlacing_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Disabled (Flickering)", "DisplayDeinterlacingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Weave (Combing)", "DisplayDeinterlacingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Blend (Blur)", "DisplayDeinterlacingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Adaptive (FastMAD)", "DisplayDeinterlacingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Progressive (Optimal)", "DisplayDeinterlacingMode"),
};

std::optional<DisplayDeinterlacingMode> Settings::ParseDisplayDeinterlacingMode(const char* str)
{
  int index = 0;
  for (const char* name : s_display_deinterlacing_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<DisplayDeinterlacingMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetDisplayDeinterlacingModeName(DisplayDeinterlacingMode mode)
{
  return s_display_deinterlacing_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetDisplayDeinterlacingModeDisplayName(DisplayDeinterlacingMode mode)
{
  return Host::TranslateToCString("Settings", s_display_deinterlacing_mode_display_names[static_cast<size_t>(mode)],
                                  "DisplayDeinterlacingMode");
}

static constexpr const std::array s_display_crop_mode_names = {"None", "Overscan", "Borders"};
static constexpr const std::array s_display_crop_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "None", "DisplayCropMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Only Overscan Area", "DisplayCropMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "All Borders", "DisplayCropMode"),
};

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
  return s_display_crop_mode_names[static_cast<size_t>(crop_mode)];
}

const char* Settings::GetDisplayCropModeDisplayName(DisplayCropMode crop_mode)
{
  return Host::TranslateToCString("Settings", s_display_crop_mode_display_names[static_cast<size_t>(crop_mode)],
                                  "DisplayCropMode");
}

static constexpr const std::array s_display_aspect_ratio_names = {
#ifndef __ANDROID__
  TRANSLATE_DISAMBIG_NOOP("Settings", "Auto (Game Native)", "DisplayAspectRatio"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Stretch To Fill", "DisplayAspectRatio"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Custom", "DisplayAspectRatio"),
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
  return s_display_aspect_ratio_names[static_cast<size_t>(ar)];
}

const char* Settings::GetDisplayAspectRatioDisplayName(DisplayAspectRatio ar)
{
  return Host::TranslateToCString("Settings", s_display_aspect_ratio_names[static_cast<size_t>(ar)],
                                  "DisplayAspectRatio");
}

float Settings::GetDisplayAspectRatioValue() const
{
  switch (display_aspect_ratio)
  {
    case DisplayAspectRatio::MatchWindow:
    {
      if (!g_gpu_device)
        return s_display_aspect_ratio_values[static_cast<size_t>(DEFAULT_DISPLAY_ASPECT_RATIO)];

      return static_cast<float>(g_gpu_device->GetWindowWidth()) / static_cast<float>(g_gpu_device->GetWindowHeight());
    }

    case DisplayAspectRatio::Custom:
    {
      return static_cast<float>(display_aspect_ratio_custom_numerator) /
             static_cast<float>(display_aspect_ratio_custom_denominator);
    }

    default:
    {
      return s_display_aspect_ratio_values[static_cast<size_t>(display_aspect_ratio)];
    }
  }
}

static constexpr const std::array s_display_alignment_names = {"LeftOrTop", "Center", "RightOrBottom"};
static constexpr const std::array s_display_alignment_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Left / Top", "DisplayAlignment"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Center", "DisplayAlignment"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Right / Bottom", "DisplayAlignment"),
};

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
  return s_display_alignment_names[static_cast<size_t>(alignment)];
}

const char* Settings::GetDisplayAlignmentDisplayName(DisplayAlignment alignment)
{
  return Host::TranslateToCString("Settings", s_display_alignment_display_names[static_cast<size_t>(alignment)],
                                  "DisplayAlignment");
}

static constexpr const std::array s_display_rotation_names = {"Normal", "Rotate90", "Rotate180", "Rotate270"};
static constexpr const std::array s_display_rotation_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "No Rotation", "DisplayRotation"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Rotate 90° (Clockwise)", "DisplayRotation"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Rotate 180° (Vertical Flip)", "DisplayRotation"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Rotate 270° (Clockwise)", "DisplayRotation"),
};

std::optional<DisplayRotation> Settings::ParseDisplayRotation(const char* str)
{
  int index = 0;
  for (const char* name : s_display_rotation_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<DisplayRotation>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetDisplayRotationName(DisplayRotation rotation)
{
  return s_display_rotation_names[static_cast<size_t>(rotation)];
}

const char* Settings::GetDisplayRotationDisplayName(DisplayRotation rotation)
{
  return Host::TranslateToCString("Settings", s_display_rotation_display_names[static_cast<size_t>(rotation)],
                                  "DisplayRotation");
}

static constexpr const std::array s_display_force_video_timing_names = {
  "Disabled",
  "NTSC",
  "PAL",
};

static constexpr const std::array s_display_force_video_timing_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Disabled", "ForceVideoTiming"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "NTSC (60hz)", "ForceVideoTiming"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "PAL (50hz)", "ForceVideoTiming"),
};

std::optional<ForceVideoTimingMode> Settings::ParseForceVideoTimingName(const char* str)
{
  int index = 0;
  for (const char* name : s_display_force_video_timing_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<ForceVideoTimingMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetForceVideoTimingName(ForceVideoTimingMode mode)
{
  return s_display_force_video_timing_names[static_cast<size_t>(mode)];
}

const char* Settings::GetForceVideoTimingDisplayName(ForceVideoTimingMode mode)
{
  return Host::TranslateToCString("Settings", s_display_force_video_timing_display_names[static_cast<size_t>(mode)],
                                  "ForceVideoTiming");
}

static constexpr const std::array s_display_scaling_names = {
  "Nearest", "NearestInteger", "BilinearSmooth", "BilinearSharp", "BilinearInteger",
};
static constexpr const std::array s_display_scaling_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Nearest-Neighbor", "DisplayScalingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Nearest-Neighbor (Integer)", "DisplayScalingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear (Smooth)", "DisplayScalingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear (Sharp)", "DisplayScalingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear (Integer)", "DisplayScalingMode"),
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
  return s_display_scaling_names[static_cast<size_t>(mode)];
}

const char* Settings::GetDisplayScalingDisplayName(DisplayScalingMode mode)
{
  return Host::TranslateToCString("Settings", s_display_scaling_display_names[static_cast<size_t>(mode)],
                                  "DisplayScalingMode");
}

static constexpr const std::array s_display_exclusive_fullscreen_mode_names = {
  "Automatic",
  "Disallowed",
  "Allowed",
};
static constexpr const std::array s_display_exclusive_fullscreen_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Automatic", "DisplayExclusiveFullscreenControl"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Disallowed", "DisplayExclusiveFullscreenControl"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Allowed", "DisplayExclusiveFullscreenControl"),
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
  return s_display_exclusive_fullscreen_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetDisplayExclusiveFullscreenControlDisplayName(DisplayExclusiveFullscreenControl mode)
{
  return Host::TranslateToCString("Settings",
                                  s_display_exclusive_fullscreen_mode_display_names[static_cast<size_t>(mode)],
                                  "DisplayExclusiveFullscreenControl");
}

static constexpr const std::array s_display_screenshot_mode_names = {
  "ScreenResolution",
  "InternalResolution",
  "UncorrectedInternalResolution",
};
static constexpr const std::array s_display_screenshot_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Screen Resolution", "DisplayScreenshotMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Internal Resolution", "DisplayScreenshotMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Internal Resolution (Aspect Uncorrected)", "DisplayScreenshotMode"),
};

std::optional<DisplayScreenshotMode> Settings::ParseDisplayScreenshotMode(const char* str)
{
  int index = 0;
  for (const char* name : s_display_screenshot_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<DisplayScreenshotMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetDisplayScreenshotModeName(DisplayScreenshotMode mode)
{
  return s_display_screenshot_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetDisplayScreenshotModeDisplayName(DisplayScreenshotMode mode)
{
  return Host::TranslateToCString("Settings", s_display_screenshot_mode_display_names[static_cast<size_t>(mode)],
                                  "DisplayScreenshotMode");
}

static constexpr const std::array s_display_screenshot_format_names = {
  "PNG",
  "JPEG",
  "WebP",
};
static constexpr const std::array s_display_screenshot_format_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "PNG", "DisplayScreenshotFormat"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "JPEG", "DisplayScreenshotFormat"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "WebP", "DisplayScreenshotFormat"),
};
static constexpr const std::array s_display_screenshot_format_extensions = {
  "png",
  "jpg",
  "webp",
};

std::optional<DisplayScreenshotFormat> Settings::ParseDisplayScreenshotFormat(const char* str)
{
  int index = 0;
  for (const char* name : s_display_screenshot_format_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<DisplayScreenshotFormat>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetDisplayScreenshotFormatName(DisplayScreenshotFormat format)
{
  return s_display_screenshot_format_names[static_cast<size_t>(format)];
}

const char* Settings::GetDisplayScreenshotFormatDisplayName(DisplayScreenshotFormat mode)
{
  return Host::TranslateToCString("Settings", s_display_screenshot_format_display_names[static_cast<size_t>(mode)],
                                  "DisplayScreenshotFormat");
}

const char* Settings::GetDisplayScreenshotFormatExtension(DisplayScreenshotFormat format)
{
  return s_display_screenshot_format_extensions[static_cast<size_t>(format)];
}

static constexpr const std::array s_memory_card_type_names = {
  "None", "Shared", "PerGame", "PerGameTitle", "PerGameFileTitle", "NonPersistent",
};
static constexpr const std::array s_memory_card_type_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "No Memory Card", "MemoryCardType"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Shared Between All Games", "MemoryCardType"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Separate Card Per Game (Serial)", "MemoryCardType"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Separate Card Per Game (Title)", "MemoryCardType"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Separate Card Per Game (File Title)", "MemoryCardType"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Non-Persistent Card (Do Not Save)", "MemoryCardType"),
};

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
  return s_memory_card_type_names[static_cast<size_t>(type)];
}

const char* Settings::GetMemoryCardTypeDisplayName(MemoryCardType type)
{
  return Host::TranslateToCString("Settings", s_memory_card_type_display_names[static_cast<size_t>(type)],
                                  "MemoryCardType");
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

std::string Settings::GetGameMemoryCardPath(std::string_view serial, u32 slot)
{
  return Path::Combine(EmuFolders::MemoryCards, fmt::format("{}_{}.mcd", serial, slot + 1));
}

static constexpr const std::array s_multitap_enable_mode_names = {
  "Disabled",
  "Port1Only",
  "Port2Only",
  "BothPorts",
};
static constexpr const std::array s_multitap_enable_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Disabled", "MultitapMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Enable on Port 1 Only", "MultitapMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Enable on Port 2 Only", "MultitapMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Enable on Ports 1 and 2", "MultitapMode"),
};

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
  return Host::TranslateToCString("Settings", s_multitap_enable_mode_display_names[static_cast<size_t>(mode)],
                                  "MultitapMode");
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

static constexpr const std::array s_save_state_compression_mode_names = {
  "Uncompressed", "DeflateLow", "DeflateDefault", "DeflateHigh", "ZstLow", "ZstDefault", "ZstHigh",
};
static constexpr const std::array s_save_state_compression_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Uncompressed", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Deflate (Low)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Deflate (Default)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Deflate (High)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Zstandard (Low)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Zstandard (Default)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Zstandard (High)", "SaveStateCompressionMode"),
};
static_assert(s_save_state_compression_mode_names.size() == static_cast<size_t>(SaveStateCompressionMode::Count));
static_assert(s_save_state_compression_mode_display_names.size() ==
              static_cast<size_t>(SaveStateCompressionMode::Count));

std::optional<SaveStateCompressionMode> Settings::ParseSaveStateCompressionModeName(const char* str)
{
  u32 index = 0;
  for (const char* name : s_save_state_compression_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<SaveStateCompressionMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetSaveStateCompressionModeName(SaveStateCompressionMode mode)
{
  return s_save_state_compression_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetSaveStateCompressionModeDisplayName(SaveStateCompressionMode mode)
{
  return Host::TranslateToCString("Settings", s_save_state_compression_mode_display_names[static_cast<size_t>(mode)],
                                  "SaveStateCompressionMode");
}

std::string EmuFolders::AppRoot;
std::string EmuFolders::DataRoot;
std::string EmuFolders::Bios;
std::string EmuFolders::Cache;
std::string EmuFolders::Cheats;
std::string EmuFolders::Covers;
std::string EmuFolders::Dumps;
std::string EmuFolders::GameIcons;
std::string EmuFolders::GameSettings;
std::string EmuFolders::InputProfiles;
std::string EmuFolders::MemoryCards;
std::string EmuFolders::Patches;
std::string EmuFolders::Resources;
std::string EmuFolders::SaveStates;
std::string EmuFolders::Screenshots;
std::string EmuFolders::Shaders;
std::string EmuFolders::Textures;
std::string EmuFolders::UserResources;
std::string EmuFolders::Videos;

void EmuFolders::SetDefaults()
{
  Bios = Path::Combine(DataRoot, "bios");
  Cache = Path::Combine(DataRoot, "cache");
  Cheats = Path::Combine(DataRoot, "cheats");
  Covers = Path::Combine(DataRoot, "covers");
  Dumps = Path::Combine(DataRoot, "dump");
  GameIcons = Path::Combine(DataRoot, "gameicons");
  GameSettings = Path::Combine(DataRoot, "gamesettings");
  InputProfiles = Path::Combine(DataRoot, "inputprofiles");
  MemoryCards = Path::Combine(DataRoot, "memcards");
  Patches = Path::Combine(DataRoot, "patches");
  SaveStates = Path::Combine(DataRoot, "savestates");
  Screenshots = Path::Combine(DataRoot, "screenshots");
  Shaders = Path::Combine(DataRoot, "shaders");
  Textures = Path::Combine(DataRoot, "textures");
  UserResources = Path::Combine(DataRoot, "resources");
  Videos = Path::Combine(DataRoot, "videos");
}

static std::string LoadPathFromSettings(SettingsInterface& si, const std::string& root, const char* section,
                                        const char* name, const char* def)
{
  std::string value = si.GetStringValue(section, name, def);
  if (value.empty())
    value = def;
  if (!Path::IsAbsolute(value))
    value = Path::Combine(root, value);
  value = Path::RealPath(value);
  return value;
}

void EmuFolders::LoadConfig(SettingsInterface& si)
{
  Bios = LoadPathFromSettings(si, DataRoot, "BIOS", "SearchDirectory", "bios");
  Cache = LoadPathFromSettings(si, DataRoot, "Folders", "Cache", "cache");
  Cheats = LoadPathFromSettings(si, DataRoot, "Folders", "Cheats", "cheats");
  Covers = LoadPathFromSettings(si, DataRoot, "Folders", "Covers", "covers");
  Dumps = LoadPathFromSettings(si, DataRoot, "Folders", "Dumps", "dump");
  GameIcons = LoadPathFromSettings(si, DataRoot, "Folders", "GameIcons", "gameicons");
  GameSettings = LoadPathFromSettings(si, DataRoot, "Folders", "GameSettings", "gamesettings");
  InputProfiles = LoadPathFromSettings(si, DataRoot, "Folders", "InputProfiles", "inputprofiles");
  MemoryCards = LoadPathFromSettings(si, DataRoot, "MemoryCards", "Directory", "memcards");
  Patches = LoadPathFromSettings(si, DataRoot, "Folders", "Patches", "patches");
  SaveStates = LoadPathFromSettings(si, DataRoot, "Folders", "SaveStates", "savestates");
  Screenshots = LoadPathFromSettings(si, DataRoot, "Folders", "Screenshots", "screenshots");
  Shaders = LoadPathFromSettings(si, DataRoot, "Folders", "Shaders", "shaders");
  Textures = LoadPathFromSettings(si, DataRoot, "Folders", "Textures", "textures");
  UserResources = LoadPathFromSettings(si, DataRoot, "Folders", "UserResources", "resources");
  Videos = LoadPathFromSettings(si, DataRoot, "Folders", "Videos", "videos");

  DEV_LOG("BIOS Directory: {}", Bios);
  DEV_LOG("Cache Directory: {}", Cache);
  DEV_LOG("Cheats Directory: {}", Cheats);
  DEV_LOG("Covers Directory: {}", Covers);
  DEV_LOG("Dumps Directory: {}", Dumps);
  DEV_LOG("Game Icons Directory: {}", GameIcons);
  DEV_LOG("Game Settings Directory: {}", GameSettings);
  DEV_LOG("Input Profile Directory: {}", InputProfiles);
  DEV_LOG("MemoryCards Directory: {}", MemoryCards);
  DEV_LOG("Patches Directory: {}", Patches);
  DEV_LOG("Resources Directory: {}", Resources);
  DEV_LOG("SaveStates Directory: {}", SaveStates);
  DEV_LOG("Screenshots Directory: {}", Screenshots);
  DEV_LOG("Shaders Directory: {}", Shaders);
  DEV_LOG("Textures Directory: {}", Textures);
  DEV_LOG("User Resources Directory: {}", UserResources);
  DEV_LOG("Videos Directory: {}", Videos);
}

void EmuFolders::Save(SettingsInterface& si)
{
  // convert back to relative
  si.SetStringValue("BIOS", "SearchDirectory", Path::MakeRelative(Bios, DataRoot).c_str());
  si.SetStringValue("Folders", "Cache", Path::MakeRelative(Cache, DataRoot).c_str());
  si.SetStringValue("Folders", "Cheats", Path::MakeRelative(Cheats, DataRoot).c_str());
  si.SetStringValue("Folders", "Covers", Path::MakeRelative(Covers, DataRoot).c_str());
  si.SetStringValue("Folders", "Dumps", Path::MakeRelative(Dumps, DataRoot).c_str());
  si.SetStringValue("Folders", "GameIcons", Path::MakeRelative(GameIcons, DataRoot).c_str());
  si.SetStringValue("Folders", "GameSettings", Path::MakeRelative(GameSettings, DataRoot).c_str());
  si.SetStringValue("Folders", "InputProfiles", Path::MakeRelative(InputProfiles, DataRoot).c_str());
  si.SetStringValue("MemoryCards", "Directory", Path::MakeRelative(MemoryCards, DataRoot).c_str());
  si.SetStringValue("Folders", "Patches", Path::MakeRelative(Patches, DataRoot).c_str());
  si.SetStringValue("Folders", "SaveStates", Path::MakeRelative(SaveStates, DataRoot).c_str());
  si.SetStringValue("Folders", "Screenshots", Path::MakeRelative(Screenshots, DataRoot).c_str());
  si.SetStringValue("Folders", "Shaders", Path::MakeRelative(Shaders, DataRoot).c_str());
  si.SetStringValue("Folders", "Textures", Path::MakeRelative(Textures, DataRoot).c_str());
  si.SetStringValue("Folders", "UserResources", Path::MakeRelative(UserResources, DataRoot).c_str());
  si.SetStringValue("Folders", "Videos", Path::MakeRelative(Videos, DataRoot).c_str());
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
  result = FileSystem::EnsureDirectoryExists(Path::Combine(Cache, "achievement_images").c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Cheats.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Covers.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Dumps.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(GameIcons.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(GameSettings.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(InputProfiles.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(MemoryCards.c_str(), false) && result;
  result = FileSystem::EnsureDirectoryExists(Patches.c_str(), false) && result;
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
  result = FileSystem::EnsureDirectoryExists(Videos.c_str(), false) && result;
  return result;
}

std::string EmuFolders::GetOverridableResourcePath(std::string_view name)
{
  std::string upath = Path::Combine(UserResources, name);
  if (FileSystem::FileExists(upath.c_str()))
  {
    if (UserResources != Resources)
      WARNING_LOG("Using user-provided resource file {}", name);
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

#ifndef __ANDROID__
  "CubebAudioStream",
  "SDLAudioStream",
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
