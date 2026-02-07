// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "settings.h"
#include "achievements.h"
#include "controller.h"
#include "core.h"
#include "gte_types.h"
#include "imgui_overlays.h"
#include "system.h"
#include "video_thread.h"

#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/media_capture.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/memmap.h"
#include "common/path.h"
#include "common/string_util.h"

#include "IconsEmoji.h"
#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <numeric>

LOG_CHANNEL(Settings);

ALIGN_TO_CACHE_LINE Settings g_settings;
ALIGN_TO_CACHE_LINE GPUSettings g_gpu_settings;

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

void SettingInfo::CopyValue(SettingsInterface* dest_si, const SettingsInterface& src_si, const char* section) const
{
  switch (type)
  {
    case SettingInfo::Type::Boolean:
      dest_si->CopyBoolValue(src_si, section, name);
      break;
    case SettingInfo::Type::Integer:
    case SettingInfo::Type::IntegerList:
      dest_si->CopyIntValue(src_si, section, name);
      break;
    case SettingInfo::Type::Float:
      dest_si->CopyFloatValue(src_si, section, name);
      break;
    case SettingInfo::Type::String:
    case SettingInfo::Type::Path:
      dest_si->CopyStringValue(src_si, section, name);
      break;
    default:
      break;
  }
}

const std::array<float, 5> GPUSettings::DEFAULT_DISPLAY_OSD_MESSAGE_DURATIONS = {{
  15.0f,                             // Error
  10.0f,                             // Warning
  5.0f,                              // Info
  2.5f,                              // Quick
  std::numeric_limits<float>::max(), // Persistent
}};
static_assert(static_cast<size_t>(OSDMessageType::MaxCount) ==
              GPUSettings::DEFAULT_DISPLAY_OSD_MESSAGE_DURATIONS.size());

GPUSettings::GPUSettings()
{
  SetPGXPDepthClearThreshold(DEFAULT_GPU_PGXP_DEPTH_THRESHOLD);
}

float GPUSettings::GetPGXPDepthClearThreshold() const
{
  return gpu_pgxp_depth_clear_threshold * static_cast<float>(GTE::MAX_Z);
}

void GPUSettings::SetPGXPDepthClearThreshold(float value)
{
  gpu_pgxp_depth_clear_threshold = value / static_cast<float>(GTE::MAX_Z);
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
  display_osd_margin = ImGuiManager::DEFAULT_SCREEN_MARGIN;
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

void Settings::Load(const SettingsInterface& si, const SettingsInterface& controller_si)
{
  region =
    ParseConsoleRegionName(
      si.GetStringValue("Console", "Region", Settings::GetConsoleRegionName(Settings::DEFAULT_CONSOLE_REGION)).c_str())
      .value_or(DEFAULT_CONSOLE_REGION);
  cpu_enable_8mb_ram = si.GetBoolValue("Console", "Enable8MBRAM", false);

  emulation_speed = si.GetFloatValue("Main", "EmulationSpeed", 1.0f);
  fast_forward_speed = si.GetFloatValue("Main", "FastForwardSpeed", 0.0f);
  turbo_speed = si.GetFloatValue("Main", "TurboSpeed", 0.0f);
  sync_to_host_refresh_rate = si.GetBoolValue("Main", "SyncToHostRefreshRate", false);
  inhibit_screensaver = si.GetBoolValue("Main", "InhibitScreensaver", true);
  pause_on_focus_loss = si.GetBoolValue("Main", "PauseOnFocusLoss", false);
  pause_on_controller_disconnection = si.GetBoolValue("Main", "PauseOnControllerDisconnection", false);
  disable_background_input = si.GetBoolValue("Main", "DisableBackgroundInput", false);
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
  rewind_save_slots = static_cast<u16>(std::min(si.GetUIntValue("Main", "RewindSaveSlots", 10u), 65535u));
  runahead_frames = static_cast<u8>(std::min(si.GetUIntValue("Main", "RunaheadFrameCount", 0u), 255u));
  runahead_for_analog_input = si.GetBoolValue("Main", "RunaheadForAnalogInput", false);

  cpu_execution_mode =
    ParseCPUExecutionMode(
      si.GetStringValue("CPU", "ExecutionMode", GetCPUExecutionModeName(DEFAULT_CPU_EXECUTION_MODE)).c_str())
      .value_or(DEFAULT_CPU_EXECUTION_MODE);
  cpu_overclock_numerator = std::max(si.GetUIntValue("CPU", "OverclockNumerator", 1u), 1u);
  cpu_overclock_denominator = std::max(si.GetUIntValue("CPU", "OverclockDenominator", 1u), 1u);
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
  gpu_resolution_scale = static_cast<u8>(si.GetUIntValue("GPU", "ResolutionScale", 1u));
  gpu_automatic_resolution_scale = (gpu_resolution_scale == 0);
  gpu_multisamples = static_cast<u8>(si.GetUIntValue("GPU", "Multisamples", 1u));
  gpu_use_debug_device = si.GetBoolValue("GPU", "UseDebugDevice", false);
  gpu_use_debug_device_gpu_validation = si.GetBoolValue("GPU", "UseGPUBasedValidation", false);
  gpu_prefer_gles_context = si.GetBoolValue("GPU", "PreferGLESContext", DEFAULT_GPU_PREFER_GLES_CONTEXT);
  gpu_disable_shader_cache = si.GetBoolValue("GPU", "DisableShaderCache", false);
  gpu_disable_dual_source_blend = si.GetBoolValue("GPU", "DisableDualSourceBlend", false);
  gpu_disable_framebuffer_fetch = si.GetBoolValue("GPU", "DisableFramebufferFetch", false);
  gpu_disable_texture_buffers = si.GetBoolValue("GPU", "DisableTextureBuffers", false);
  gpu_disable_texture_copy_to_self = si.GetBoolValue("GPU", "DisableTextureCopyToSelf", false);
  gpu_disable_memory_import = si.GetBoolValue("GPU", "DisableMemoryImport", false);
  gpu_disable_raster_order_views = si.GetBoolValue("GPU", "DisableRasterOrderViews", false);
  gpu_disable_compute_shaders = si.GetBoolValue("GPU", "DisableComputeShaders", false);
  gpu_disable_compressed_textures = si.GetBoolValue("GPU", "DisableCompressedTextures", false);
  gpu_per_sample_shading = si.GetBoolValue("GPU", "PerSampleShading", false);
  gpu_use_thread = si.GetBoolValue("GPU", "UseThread", true);
  gpu_max_queued_frames = static_cast<u8>(si.GetUIntValue("GPU", "MaxQueuedFrames", DEFAULT_GPU_MAX_QUEUED_FRAMES));
  gpu_use_software_renderer_for_readbacks = si.GetBoolValue("GPU", "UseSoftwareRendererForReadbacks", false);
  gpu_use_software_renderer_for_memory_states = si.GetBoolValue("GPU", "UseSoftwareRendererForMemoryStates", false);
  gpu_scaled_interlacing = si.GetBoolValue("GPU", "ScaledInterlacing", true);
  gpu_force_round_texcoords = si.GetBoolValue("GPU", "ForceRoundTextureCoordinates", false);
  gpu_texture_filter =
    ParseTextureFilterName(
      si.GetStringValue("GPU", "TextureFilter", GetTextureFilterName(DEFAULT_GPU_TEXTURE_FILTER)).c_str())
      .value_or(DEFAULT_GPU_TEXTURE_FILTER);
  gpu_sprite_texture_filter =
    ParseTextureFilterName(
      si.GetStringValue("GPU", "SpriteTextureFilter", GetTextureFilterName(DEFAULT_GPU_TEXTURE_FILTER)).c_str())
      .value_or(DEFAULT_GPU_TEXTURE_FILTER);
  gpu_dithering_mode =
    ParseGPUDitheringModeName(
      si.GetStringValue("GPU", "DitheringMode", GetGPUDitheringModeName(DEFAULT_GPU_DITHERING_MODE)).c_str())
      .value_or(DEFAULT_GPU_DITHERING_MODE);
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
  gpu_widescreen_rendering = gpu_widescreen_hack = si.GetBoolValue("GPU", "WidescreenHack", false);
  gpu_modulation_crop = si.GetBoolValue("GPU", "EnableModulationCrop", false);
  gpu_texture_cache = si.GetBoolValue("GPU", "EnableTextureCache", false);
  display_24bit_chroma_smoothing = si.GetBoolValue("GPU", "ChromaSmoothing24Bit", false);
  gpu_pgxp_enable = si.GetBoolValue("GPU", "PGXPEnable", false);
  LoadPGXPSettings(si);
  gpu_show_vram = si.GetBoolValue("Debug", "ShowVRAM");
  gpu_dump_cpu_to_vram_copies = si.GetBoolValue("Debug", "DumpCPUToVRAMCopies");
  gpu_dump_vram_to_cpu_copies = si.GetBoolValue("Debug", "DumpVRAMToCPUCopies");
  gpu_dump_fast_replay_mode = si.GetBoolValue("GPU", "DumpFastReplayMode", false);

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
    ParseDisplayAspectRatio(si.GetStringValue("Display", "AspectRatio")).value_or(DEFAULT_DISPLAY_ASPECT_RATIO);
  display_fine_crop_mode = ParseDisplayFineCropMode(si.GetStringValue("Display", "FineCropMode").c_str())
                             .value_or(DEFAULT_DISPLAY_FINE_CROP_MODE);
  display_fine_crop_amount[0] = static_cast<s16>(std::clamp<s32>(
    si.GetIntValue("Display", "FineCropLeft", 0), std::numeric_limits<s16>::min(), std::numeric_limits<s16>::max()));
  display_fine_crop_amount[1] = static_cast<s16>(std::clamp<s32>(
    si.GetIntValue("Display", "FineCropTop", 0), std::numeric_limits<s16>::min(), std::numeric_limits<s16>::max()));
  display_fine_crop_amount[2] = static_cast<s16>(std::clamp<s32>(
    si.GetIntValue("Display", "FineCropRight", 0), std::numeric_limits<s16>::min(), std::numeric_limits<s16>::max()));
  display_fine_crop_amount[3] = static_cast<s16>(std::clamp<s32>(
    si.GetIntValue("Display", "FineCropBottom", 0), std::numeric_limits<s16>::min(), std::numeric_limits<s16>::max()));
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
  display_scaling_24bit =
    ParseDisplayScaling(
      si.GetStringValue("Display", "Scaling24Bit", GetDisplayScalingName(DEFAULT_DISPLAY_SCALING)).c_str())
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
  display_optimal_frame_pacing = si.GetBoolValue("Display", "OptimalFramePacing", DEFAULT_OPTIMAL_FRAME_PACING);
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
  display_show_messages = si.GetBoolValue("Display", "ShowOSDMessages", true);
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
  display_auto_resize_window = si.GetBoolValue("Display", "AutoResizeWindow", false);
  display_osd_scale = si.GetFloatValue("Display", "OSDScale", DEFAULT_OSD_SCALE);
  display_osd_margin = std::max(si.GetFloatValue("Display", "OSDMargin", ImGuiManager::DEFAULT_SCREEN_MARGIN), 0.0f);

  for (size_t i = 0; i < static_cast<size_t>(OSDMessageType::Persistent); i++)
  {
    display_osd_message_duration[i] = si.GetFloatValue(
      "Display", TinyString::from_format("OSD{}Duration", GetDisplayOSDMessageTypeName(static_cast<OSDMessageType>(i))),
      DEFAULT_DISPLAY_OSD_MESSAGE_DURATIONS[i]);
  }
  display_osd_message_duration[static_cast<size_t>(OSDMessageType::Persistent)] = std::numeric_limits<float>::max();
  display_osd_message_location = ParseNotificationLocation(si.GetStringValue("Display", "OSDMessageLocation").c_str())
                                   .value_or(DEFAULT_OSD_MESSAGE_LOCATION);

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
  cdrom_ignore_host_subcode = si.GetBoolValue("CDROM", "IgnoreHostSubcode", false);
  cdrom_mute_cd_audio = si.GetBoolValue("CDROM", "MuteCDAudio", false);
  cdrom_auto_disc_change = si.GetBoolValue("CDROM", "AutoDiscChange", false);
  cdrom_read_speedup =
    Truncate8(std::min<u32>(si.GetUIntValue("CDROM", "ReadSpeedup", 1u), std::numeric_limits<u8>::max()));
  cdrom_seek_speedup =
    Truncate8(std::min<u32>(si.GetUIntValue("CDROM", "SeekSpeedup", 1u), std::numeric_limits<u8>::max()));
  cdrom_max_seek_speedup_cycles =
    std::max(si.GetUIntValue("CDROM", "MaxSeekSpeedupCycles", DEFAULT_CDROM_MAX_SEEK_SPEEDUP_CYCLES), 1u);
  cdrom_max_read_speedup_cycles =
    std::max(si.GetUIntValue("CDROM", "MaxReadSpeedupCycles", DEFAULT_CDROM_MAX_READ_SPEEDUP_CYCLES), 1u);
  mdec_disable_cdrom_speedup = si.GetBoolValue("CDROM", "DisableSpeedupOnMDEC", false);

  audio_backend =
    AudioStream::ParseBackendName(
      si.GetStringValue("Audio", "Backend", AudioStream::GetBackendName(AudioStream::DEFAULT_BACKEND)).c_str())
      .value_or(AudioStream::DEFAULT_BACKEND);
  audio_driver = si.GetStringValue("Audio", "Driver");
  audio_output_device = si.GetStringValue("Audio", "OutputDevice");
  audio_stream_parameters.Load(si, "Audio");
  audio_output_volume =
    Truncate8(std::min<u32>(si.GetUIntValue("Audio", "OutputVolume", 100), std::numeric_limits<u8>::max()));
  audio_fast_forward_volume =
    Truncate8(std::min<u32>(si.GetUIntValue("Audio", "FastForwardVolume", 100), std::numeric_limits<u8>::max()));

  audio_output_muted = si.GetBoolValue("Audio", "OutputMuted", false);

  mdec_use_old_routines = si.GetBoolValue("Hacks", "UseOldMDECRoutines", false);
  export_shared_memory = si.GetBoolValue("Hacks", "ExportSharedMemory", false);

  dma_max_slice_ticks = si.GetIntValue("Hacks", "DMAMaxSliceTicks", DEFAULT_DMA_MAX_SLICE_TICKS);
  dma_halt_ticks = si.GetIntValue("Hacks", "DMAHaltTicks", DEFAULT_DMA_HALT_TICKS);
  gpu_fifo_size = si.GetUIntValue("Hacks", "GPUFIFOSize", DEFAULT_GPU_FIFO_SIZE);
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
      Controller::GetSettingsSection(pad).c_str(), "Type", Controller::GetControllerInfo(default_type).name));
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
  memory_card_fast_forward_access = si.GetBoolValue("MemoryCards", "FastForwardAccess", false);

  achievements_enabled = si.GetBoolValue("Cheevos", "Enabled", false);
  achievements_hardcore_mode = si.GetBoolValue("Cheevos", "ChallengeMode", false);
  achievements_encore_mode = si.GetBoolValue("Cheevos", "EncoreMode", false);
  achievements_spectator_mode = si.GetBoolValue("Cheevos", "SpectatorMode", false);
  achievements_unofficial_test_mode = si.GetBoolValue("Cheevos", "UnofficialTestMode", false);
  achievements_use_raintegration = si.GetBoolValue("Cheevos", "UseRAIntegration", false);
  achievements_notifications = si.GetBoolValue("Cheevos", "Notifications", true);
  achievements_leaderboard_notifications = si.GetBoolValue("Cheevos", "LeaderboardNotifications", true);
  achievements_leaderboard_trackers = si.GetBoolValue("Cheevos", "LeaderboardTrackers", true);
  achievements_sound_effects = si.GetBoolValue("Cheevos", "SoundEffects", true);
  achievements_progress_indicators = si.GetBoolValue("Cheevos", "ProgressIndicators", true);
  achievements_notification_location =
    ParseNotificationLocation(si.GetStringValue("Cheevos", "NotificationLocation").c_str())
      .value_or(DEFAULT_ACHIEVEMENT_NOTIFICATION_LOCATION);
  achievements_indicator_location = ParseNotificationLocation(si.GetStringValue("Cheevos", "IndicatorLocation").c_str())
                                      .value_or(DEFAULT_ACHIEVEMENT_INDICATOR_LOCATION);
  achievements_challenge_indicator_mode =
    ParseAchievementChallengeIndicatorMode(si.GetStringValue("Cheevos", "ChallengeIndicatorMode").c_str())
      .value_or(DEFAULT_ACHIEVEMENT_CHALLENGE_INDICATOR_MODE);
  achievements_notification_duration =
    Truncate8(std::min<u32>(si.GetUIntValue("Cheevos", "NotificationsDuration", DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME),
                            std::numeric_limits<u8>::max()));
  achievements_leaderboard_duration =
    Truncate8(std::min<u32>(si.GetUIntValue("Cheevos", "LeaderboardsDuration", DEFAULT_LEADERBOARD_NOTIFICATION_TIME),
                            std::numeric_limits<u8>::max()));

#ifndef __ANDROID__
  enable_gdb_server = si.GetBoolValue("Debug", "EnableGDBServer");
  gdb_server_port = static_cast<u16>(si.GetUIntValue("Debug", "GDBServerPort", DEFAULT_GDB_SERVER_PORT));
#endif

  texture_replacements.enable_texture_replacements =
    si.GetBoolValue("TextureReplacements", "EnableTextureReplacements", false);
  texture_replacements.enable_vram_write_replacements =
    si.GetBoolValue("TextureReplacements", "EnableVRAMWriteReplacements", false);
  texture_replacements.always_track_uploads = si.GetBoolValue("TextureReplacements", "AlwaysTrackUploads", false);
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

  texture_replacements.config.max_hash_cache_entries =
    si.GetUIntValue("TextureReplacements", "MaxHashCacheEntries",
                    TextureReplacementSettings::Configuration::DEFAULT_MAX_HASH_CACHE_ENTRIES);
  texture_replacements.config.max_hash_cache_vram_usage_mb =
    si.GetUIntValue("TextureReplacements", "MaxHashCacheVRAMUsageMB",
                    TextureReplacementSettings::Configuration::DEFAULT_MAX_HASH_CACHE_VRAM_USAGE_MB);
  texture_replacements.config.max_replacement_cache_vram_usage_mb =
    si.GetUIntValue("TextureReplacements", "MaxReplacementCacheVRAMUsage",
                    TextureReplacementSettings::Configuration::DEFAULT_MAX_REPLACEMENT_CACHE_VRAM_USAGE_MB);

  texture_replacements.config.max_vram_write_splits = Truncate16(
    std::min<u32>(si.GetUIntValue("TextureReplacements", "MaxVRAMWriteSplits", 0u), std::numeric_limits<u16>::max()));
  texture_replacements.config.max_vram_write_coalesce_width = Truncate16(std::min<u32>(
    si.GetUIntValue("TextureReplacements", "MaxVRAMWriteCoalesceWidth", 0u), std::numeric_limits<u16>::max()));
  texture_replacements.config.max_vram_write_coalesce_height = Truncate16(std::min<u32>(
    si.GetUIntValue("TextureReplacements", "MaxVRAMWriteCoalesceHeight", 0u), std::numeric_limits<u16>::max()));

  texture_replacements.config.texture_dump_width_threshold = Truncate16(std::min<u32>(
    si.GetUIntValue("TextureReplacements", "DumpTextureWidthThreshold", 16), std::numeric_limits<u16>::max()));
  texture_replacements.config.texture_dump_height_threshold = Truncate16(std::min<u32>(
    si.GetUIntValue("TextureReplacements", "DumpTextureHeightThreshold", 16), std::numeric_limits<u16>::max()));
  texture_replacements.config.vram_write_dump_width_threshold = Truncate16(std::min<u32>(
    si.GetUIntValue("TextureReplacements", "DumpVRAMWriteWidthThreshold", 128), std::numeric_limits<u16>::max()));
  texture_replacements.config.vram_write_dump_height_threshold = Truncate16(std::min<u32>(
    si.GetUIntValue("TextureReplacements", "DumpVRAMWriteHeightThreshold", 128), std::numeric_limits<u16>::max()));

  pio_device_type = ParsePIODeviceTypeName(
                      si.GetTinyStringValue("PIO", "DeviceType", GetPIODeviceTypeModeName(DEFAULT_PIO_DEVICE_TYPE)))
                      .value_or(DEFAULT_PIO_DEVICE_TYPE);
  pio_flash_image_path = si.GetStringValue("PIO", "FlashImagePath");
  pio_flash_write_enable = si.GetBoolValue("PIO", "FlashImageWriteEnable", false);
  pio_switch_active = si.GetBoolValue("PIO", "SwitchActive", true);
  sio_redirect_to_tty = si.GetBoolValue("SIO", "RedirectToTTY", false);

  pcdrv_enable = si.GetBoolValue("PCDrv", "Enabled", false);
  pcdrv_enable_writes = si.GetBoolValue("PCDrv", "EnableWrites", false);
  pcdrv_root = si.GetStringValue("PCDrv", "Root");

#ifdef __ANDROID__
  // Android users are incredibly silly and don't understand that stretch is in the aspect ratio list...
  if (si.GetBoolValue("Display", "Stretch", false))
    display_aspect_ratio = DisplayAspectRatio::MatchWindow;
#endif
}

void Settings::LoadPGXPSettings(const SettingsInterface& si)
{
  gpu_pgxp_culling = si.GetBoolValue("GPU", "PGXPCulling", true);
  gpu_pgxp_texture_correction = si.GetBoolValue("GPU", "PGXPTextureCorrection", true);
  gpu_pgxp_color_correction = si.GetBoolValue("GPU", "PGXPColorCorrection", false);
  gpu_pgxp_vertex_cache = si.GetBoolValue("GPU", "PGXPVertexCache", false);
  gpu_pgxp_cpu = si.GetBoolValue("GPU", "PGXPCPU", false);
  gpu_pgxp_preserve_proj_fp = si.GetBoolValue("GPU", "PGXPPreserveProjFP", false);
  gpu_pgxp_tolerance = si.GetFloatValue("GPU", "PGXPTolerance", -1.0f);
  gpu_pgxp_depth_buffer = si.GetBoolValue("GPU", "PGXPDepthBuffer", false);
  gpu_pgxp_disable_2d = si.GetBoolValue("GPU", "PGXPDisableOn2DPolygons", false);
  gpu_pgxp_transparent_depth = si.GetBoolValue("GPU", "PGXPTransparentDepthTest", false);
  SetPGXPDepthClearThreshold(si.GetFloatValue("GPU", "PGXPDepthThreshold", DEFAULT_GPU_PGXP_DEPTH_THRESHOLD));
}

void Settings::Save(SettingsInterface& si, bool ignore_base) const
{
  si.SetStringValue("Console", "Region", GetConsoleRegionName(region));
  si.SetBoolValue("Console", "Enable8MBRAM", cpu_enable_8mb_ram);

  si.SetFloatValue("Main", "EmulationSpeed", emulation_speed);
  si.SetFloatValue("Main", "FastForwardSpeed", fast_forward_speed);
  si.SetFloatValue("Main", "TurboSpeed", turbo_speed);

  if (!ignore_base)
  {
    si.SetBoolValue("Main", "SyncToHostRefreshRate", sync_to_host_refresh_rate);
    si.SetBoolValue("Main", "InhibitScreensaver", inhibit_screensaver);
    si.SetBoolValue("Main", "PauseOnFocusLoss", pause_on_focus_loss);
    si.SetBoolValue("Main", "PauseOnControllerDisconnection", pause_on_controller_disconnection);
    si.SetBoolValue("Main", "SaveStateOnExit", save_state_on_exit);
    si.SetBoolValue("Main", "CreateSaveStateBackups", create_save_state_backups);
    si.SetStringValue("Main", "SaveStateCompression", GetSaveStateCompressionModeName(save_state_compression));
    si.SetBoolValue("Main", "ConfirmPowerOff", confim_power_off);
    si.SetBoolValue("Main", "EnableDiscordPresence", enable_discord_presence);
  }

  si.SetBoolValue("Main", "DisableBackgroundInput", disable_background_input);

  si.SetBoolValue("Main", "LoadDevicesFromSaveStates", load_devices_from_save_states);
  si.SetBoolValue("Main", "DisableAllEnhancements", disable_all_enhancements);
  si.SetBoolValue("Main", "RewindEnable", rewind_enable);
  si.SetFloatValue("Main", "RewindFrequency", rewind_save_frequency);
  si.SetUIntValue("Main", "RewindSaveSlots", rewind_save_slots);
  si.SetUIntValue("Main", "RunaheadFrameCount", runahead_frames);
  si.SetBoolValue("Main", "RunaheadForAnalogInput", runahead_for_analog_input);

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
  si.SetUIntValue("GPU", "ResolutionScale", gpu_resolution_scale);
  si.SetUIntValue("GPU", "Multisamples", gpu_multisamples);

  if (!ignore_base)
  {
    si.SetBoolValue("GPU", "UseDebugDevice", gpu_use_debug_device);
    si.SetBoolValue("GPU", "UseGPUBasedValidation", gpu_use_debug_device_gpu_validation);
    si.SetBoolValue("GPU", "PreferGLESContext", gpu_prefer_gles_context);
    si.SetBoolValue("GPU", "DisableShaderCache", gpu_disable_shader_cache);
    si.SetBoolValue("GPU", "DisableDualSourceBlend", gpu_disable_dual_source_blend);
    si.SetBoolValue("GPU", "DisableFramebufferFetch", gpu_disable_framebuffer_fetch);
    si.SetBoolValue("GPU", "DisableTextureBuffers", gpu_disable_texture_buffers);
    si.SetBoolValue("GPU", "DisableTextureCopyToSelf", gpu_disable_texture_copy_to_self);
    si.SetBoolValue("GPU", "DisableMemoryImport", gpu_disable_memory_import);
    si.SetBoolValue("GPU", "DisableRasterOrderViews", gpu_disable_raster_order_views);
    si.SetBoolValue("GPU", "DisableComputeShaders", gpu_disable_compute_shaders);
    si.SetBoolValue("GPU", "DisableCompressedTextures", gpu_disable_compressed_textures);
  }

  si.SetBoolValue("GPU", "PerSampleShading", gpu_per_sample_shading);
  si.SetUIntValue("GPU", "MaxQueuedFrames", gpu_max_queued_frames);
  si.SetBoolValue("GPU", "UseThread", gpu_use_thread);
  si.SetBoolValue("GPU", "UseSoftwareRendererForReadbacks", gpu_use_software_renderer_for_readbacks);
  si.SetBoolValue("GPU", "UseSoftwareRendererForMemoryStates", gpu_use_software_renderer_for_memory_states);
  si.SetBoolValue("GPU", "ScaledInterlacing", gpu_scaled_interlacing);
  si.SetBoolValue("GPU", "ForceRoundTextureCoordinates", gpu_force_round_texcoords);
  si.SetStringValue("GPU", "TextureFilter", GetTextureFilterName(gpu_texture_filter));
  si.SetStringValue("GPU", "SpriteTextureFilter", GetTextureFilterName(gpu_sprite_texture_filter));
  si.SetStringValue("GPU", "DitheringMode", GetGPUDitheringModeName(gpu_dithering_mode));
  si.SetStringValue("GPU", "LineDetectMode", GetLineDetectModeName(gpu_line_detect_mode));
  si.SetStringValue("GPU", "DownsampleMode", GetDownsampleModeName(gpu_downsample_mode));
  si.SetUIntValue("GPU", "DownsampleScale", gpu_downsample_scale);
  si.SetStringValue("GPU", "WireframeMode", GetGPUWireframeModeName(gpu_wireframe_mode));
  si.SetStringValue("GPU", "ForceVideoTiming", GetForceVideoTimingName(gpu_force_video_timing));
  si.SetBoolValue("GPU", "WidescreenHack", gpu_widescreen_rendering);
  si.SetBoolValue("GPU", "EnableModulationCrop", gpu_modulation_crop);
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
  si.SetBoolValue("GPU", "PGXPTransparentDepthTest", gpu_pgxp_transparent_depth);
  si.SetFloatValue("GPU", "PGXPDepthThreshold", GetPGXPDepthClearThreshold());
  si.SetBoolValue("Debug", "ShowVRAM", gpu_show_vram);
  si.SetBoolValue("Debug", "DumpCPUToVRAMCopies", gpu_dump_cpu_to_vram_copies);
  si.SetBoolValue("Debug", "DumpVRAMToCPUCopies", gpu_dump_vram_to_cpu_copies);
  si.SetBoolValue("GPU", "DumpFastReplayMode", gpu_dump_fast_replay_mode);

  si.SetStringValue("GPU", "DeinterlacingMode", GetDisplayDeinterlacingModeName(display_deinterlacing_mode));
  si.SetStringValue("Display", "CropMode", GetDisplayCropModeName(display_crop_mode));
  si.SetIntValue("Display", "ActiveStartOffset", display_active_start_offset);
  si.SetIntValue("Display", "ActiveEndOffset", display_active_end_offset);
  si.SetIntValue("Display", "LineStartOffset", display_line_start_offset);
  si.SetIntValue("Display", "LineEndOffset", display_line_end_offset);
  si.SetBoolValue("Display", "Force4_3For24Bit", display_force_4_3_for_24bit);
  si.SetStringValue("Display", "AspectRatio", GetDisplayAspectRatioName(display_aspect_ratio).c_str());
  si.SetStringValue("Display", "FineCropMode", GetDisplayFineCropModeName(display_fine_crop_mode));
  si.SetIntValue("Display", "FineCropLeft", display_fine_crop_amount[0]);
  si.SetIntValue("Display", "FineCropTop", display_fine_crop_amount[1]);
  si.SetIntValue("Display", "FineCropRight", display_fine_crop_amount[2]);
  si.SetIntValue("Display", "FineCropBottom", display_fine_crop_amount[3]);
  si.SetStringValue("Display", "Alignment", GetDisplayAlignmentName(display_alignment));
  si.SetStringValue("Display", "Rotation", GetDisplayRotationName(display_rotation));
  si.SetStringValue("Display", "Scaling", GetDisplayScalingName(display_scaling));
  si.SetStringValue("Display", "Scaling24Bit", GetDisplayScalingName(display_scaling_24bit));
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
  if (!ignore_base)
  {
    si.SetBoolValue("Display", "ShowOSDMessages", display_show_messages);
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
    si.SetFloatValue("Display", "OSDMargin", display_osd_margin);

    for (size_t i = 0; i < static_cast<size_t>(OSDMessageType::Persistent); i++)
    {
      si.SetFloatValue(
        "Display",
        TinyString::from_format("OSD{}Duration", GetDisplayOSDMessageTypeName(static_cast<OSDMessageType>(i))),
        display_osd_message_duration[i]);
    }

    si.SetStringValue("Display", "OSDMessageLocation", GetNotificationLocationName(display_osd_message_location));
  }

  si.SetBoolValue("Display", "AutoResizeWindow", display_auto_resize_window);

  si.SetIntValue("CDROM", "ReadaheadSectors", cdrom_readahead_sectors);
  si.SetStringValue("CDROM", "MechaconVersion", GetCDROMMechVersionName(cdrom_mechacon_version));
  si.SetBoolValue("CDROM", "RegionCheck", cdrom_region_check);
  si.SetBoolValue("CDROM", "SubQSkew", cdrom_subq_skew);
  si.SetBoolValue("CDROM", "LoadImageToRAM", cdrom_load_image_to_ram);
  si.SetBoolValue("CDROM", "LoadImagePatches", cdrom_load_image_patches);
  si.SetBoolValue("CDROM", "IgnoreHostSubcode", cdrom_ignore_host_subcode);
  si.SetBoolValue("CDROM", "MuteCDAudio", cdrom_mute_cd_audio);
  si.SetBoolValue("CDROM", "AutoDiscChange", cdrom_auto_disc_change);
  si.SetUIntValue("CDROM", "ReadSpeedup", cdrom_read_speedup);
  si.SetUIntValue("CDROM", "SeekSpeedup", cdrom_seek_speedup);
  si.SetUIntValue("CDROM", "MaxReadSpeedupCycles", cdrom_max_seek_speedup_cycles);
  si.SetUIntValue("CDROM", "MaxSeekSpeedupCycles", cdrom_max_read_speedup_cycles);
  si.SetBoolValue("CDROM", "DisableSpeedupOnMDEC", mdec_disable_cdrom_speedup);

  si.SetStringValue("Audio", "Backend", AudioStream::GetBackendName(audio_backend));
  si.SetStringValue("Audio", "Driver", audio_driver.c_str());
  si.SetStringValue("Audio", "OutputDevice", audio_output_device.c_str());
  audio_stream_parameters.Save(si, "Audio");
  si.SetUIntValue("Audio", "OutputVolume", audio_output_volume);
  si.SetUIntValue("Audio", "FastForwardVolume", audio_fast_forward_volume);
  si.SetBoolValue("Audio", "OutputMuted", audio_output_muted);

  si.SetBoolValue("Hacks", "UseOldMDECRoutines", mdec_use_old_routines);
  si.SetBoolValue("Hacks", "ExportSharedMemory", export_shared_memory);

  if (!ignore_base)
  {
    si.SetIntValue("Hacks", "DMAMaxSliceTicks", dma_max_slice_ticks);
    si.SetIntValue("Hacks", "DMAHaltTicks", dma_halt_ticks);
    si.SetIntValue("Hacks", "GPUFIFOSize", gpu_fifo_size);
    si.SetIntValue("Hacks", "GPUMaxRunAhead", gpu_max_run_ahead);
  }

  si.SetBoolValue("BIOS", "TTYLogging", bios_tty_logging);
  si.SetBoolValue("BIOS", "PatchFastBoot", bios_patch_fast_boot);
  si.SetBoolValue("BIOS", "FastForwardBoot", bios_fast_forward_boot);

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    si.SetStringValue(Controller::GetSettingsSection(i).c_str(), "Type",
                      Controller::GetControllerInfo(controller_types[i]).name);
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
  si.SetBoolValue("MemoryCards", "FastForwardAccess", memory_card_fast_forward_access);

  si.SetStringValue("ControllerPorts", "MultitapMode", GetMultitapModeName(multitap_mode));

  si.SetBoolValue("Cheevos", "Enabled", achievements_enabled);
  si.SetBoolValue("Cheevos", "ChallengeMode", achievements_hardcore_mode);
  si.SetBoolValue("Cheevos", "EncoreMode", achievements_encore_mode);
  si.SetBoolValue("Cheevos", "SpectatorMode", achievements_spectator_mode);
  si.SetBoolValue("Cheevos", "UnofficialTestMode", achievements_unofficial_test_mode);
  si.SetBoolValue("Cheevos", "UseRAIntegration", achievements_use_raintegration);
  si.SetBoolValue("Cheevos", "Notifications", achievements_notifications);
  si.SetBoolValue("Cheevos", "LeaderboardNotifications", achievements_leaderboard_notifications);
  si.SetBoolValue("Cheevos", "LeaderboardTrackers", achievements_leaderboard_trackers);
  si.SetBoolValue("Cheevos", "SoundEffects", achievements_sound_effects);
  si.SetBoolValue("Cheevos", "ProgressIndicators", achievements_progress_indicators);
  si.SetStringValue("Cheevos", "NotificationLocation", GetNotificationLocationName(achievements_notification_location));
  si.SetStringValue("Cheevos", "IndicatorLocation", GetNotificationLocationName(achievements_indicator_location));
  si.SetStringValue("Cheevos", "ChallengeIndicatorMode",
                    GetAchievementChallengeIndicatorModeName(achievements_challenge_indicator_mode));
  si.SetUIntValue("Cheevos", "NotificationsDuration", achievements_notification_duration);
  si.SetUIntValue("Cheevos", "LeaderboardsDuration", achievements_leaderboard_duration);

#ifndef __ANDROID__
  si.SetBoolValue("Debug", "EnableGDBServer", enable_gdb_server);
  si.SetUIntValue("Debug", "GDBServerPort", gdb_server_port);
#endif

  si.SetBoolValue("TextureReplacements", "EnableTextureReplacements", texture_replacements.enable_texture_replacements);
  si.SetBoolValue("TextureReplacements", "EnableVRAMWriteReplacements",
                  texture_replacements.enable_vram_write_replacements);
  si.SetBoolValue("TextureReplacements", "AlwaysTrackUploads", texture_replacements.always_track_uploads);
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

  si.SetUIntValue("TextureReplacements", "MaxHashCacheEntries", texture_replacements.config.max_hash_cache_entries);
  si.SetUIntValue("TextureReplacements", "MaxHashCacheVRAMUsageMB",
                  texture_replacements.config.max_hash_cache_vram_usage_mb);
  si.SetUIntValue("TextureReplacements", "MaxReplacementCacheVRAMUsage",
                  texture_replacements.config.max_replacement_cache_vram_usage_mb);

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

  si.SetStringValue("PIO", "DeviceType", GetPIODeviceTypeModeName(pio_device_type));
  si.SetStringValue("PIO", "FlashImagePath", pio_flash_image_path.c_str());
  si.SetBoolValue("PIO", "FlashImageWriteEnable", pio_flash_write_enable);
  si.SetBoolValue("PIO", "SwitchActive", pio_switch_active);
  si.SetBoolValue("SIO", "RedirectToTTY", sio_redirect_to_tty);

  si.SetBoolValue("PCDrv", "Enabled", pcdrv_enable);
  si.SetBoolValue("PCDrv", "EnableWrites", pcdrv_enable_writes);
  si.SetStringValue("PCDrv", "Root", pcdrv_root.c_str());
}

bool Settings::TextureReplacementSettings::Configuration::operator==(const Configuration& rhs) const
{
  return (max_hash_cache_entries == rhs.max_hash_cache_entries &&
          max_hash_cache_vram_usage_mb == rhs.max_hash_cache_vram_usage_mb &&
          max_replacement_cache_vram_usage_mb == rhs.max_replacement_cache_vram_usage_mb &&
          max_vram_write_splits == rhs.max_vram_write_splits &&
          max_vram_write_coalesce_width == rhs.max_vram_write_coalesce_width &&
          max_vram_write_coalesce_height == rhs.max_vram_write_coalesce_height &&
          texture_dump_width_threshold == rhs.texture_dump_width_threshold &&
          texture_dump_height_threshold == rhs.texture_dump_height_threshold &&
          vram_write_dump_width_threshold == rhs.vram_write_dump_width_threshold &&
          vram_write_dump_height_threshold == rhs.vram_write_dump_height_threshold &&
          dump_texture_pages == rhs.dump_texture_pages && dump_full_texture_pages == rhs.dump_full_texture_pages &&
          dump_texture_force_alpha_channel == rhs.dump_texture_force_alpha_channel &&
          dump_vram_write_force_alpha_channel == rhs.dump_vram_write_force_alpha_channel &&
          dump_c16_textures == rhs.dump_c16_textures && reduce_palette_range == rhs.reduce_palette_range &&
          convert_copies_to_writes == rhs.convert_copies_to_writes &&
          replacement_scale_linear_filter == rhs.replacement_scale_linear_filter);
}

bool Settings::TextureReplacementSettings::Configuration::operator!=(const Configuration& rhs) const
{
  return !operator==(rhs);
}

bool Settings::TextureReplacementSettings::operator==(const TextureReplacementSettings& rhs) const
{
  return (enable_texture_replacements == rhs.enable_texture_replacements &&
          enable_vram_write_replacements == rhs.enable_vram_write_replacements &&
          always_track_uploads == rhs.always_track_uploads && preload_textures == rhs.preload_textures &&
          dump_textures == rhs.dump_textures && dump_replaced_textures == rhs.dump_replaced_textures &&
          dump_vram_writes == rhs.dump_vram_writes && config == rhs.config);
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

# Sets the maximum size of the hash cache that manages texture replacements.
# Generally the default is sufficient, but some games may require increasing the
# size. Do not set too high, otherwise mobile drivers will break.
{}MaxHashCacheEntries: {}

# Sets the maximum amount of VRAM in megabytes that the hash cache can utilize.
# Keep in mind your target system requirements, using too much VRAM will result
# in swapping and significantly decreased performance.
{}MaxHashCacheVRAMUsageMB: {}

# Sets the maximum amount of VRAM in megabytes that are reserved for the cache of
# replacement textures. The cache usage for any given texture is approximately the
# same size as the uncompressed source image on disk.
{}MaxReplacementCacheVRAMUsage: {}

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
                     comment_str, max_hash_cache_entries,              // MaxHashCacheEntries
                     comment_str, max_hash_cache_vram_usage_mb,        // MaxHashCacheVRAMUsageMB
                     comment_str, max_replacement_cache_vram_usage_mb, // MaxReplacementCacheVRAMUsage
                     comment_str, replacement_scale_linear_filter);    // ReplacementScaleLinearFilter
}

void Settings::ApplySettingRestrictions()
{
  if (disable_all_enhancements)
  {
    region = ConsoleRegion::Auto;
    cpu_overclock_enable = false;
    cpu_overclock_active = false;
    cpu_enable_8mb_ram = false;
    gpu_resolution_scale = 1;
    gpu_multisamples = 1;
    gpu_automatic_resolution_scale = false;
    gpu_per_sample_shading = false;
    gpu_scaled_interlacing = false;
    gpu_force_round_texcoords = false;
    gpu_texture_filter = GPUTextureFilter::Nearest;
    gpu_sprite_texture_filter = GPUTextureFilter::Nearest;
    gpu_dithering_mode = GPUDitheringMode::Unscaled;
    gpu_line_detect_mode = GPULineDetectMode::Disabled;
    gpu_downsample_mode = GPUDownsampleMode::Disabled;
    gpu_wireframe_mode = GPUWireframeMode::Disabled;
    gpu_force_video_timing = ForceVideoTimingMode::Disabled;
    gpu_widescreen_rendering = false;
    gpu_widescreen_hack = false;
    gpu_modulation_crop = false;
    gpu_texture_cache = false;
    gpu_pgxp_enable = false;
    display_deinterlacing_mode = DisplayDeinterlacingMode::Adaptive;
    display_24bit_chroma_smoothing = false;
    cdrom_read_speedup = 1;
    cdrom_seek_speedup = 1;
    cdrom_mute_cd_audio = false;
    cdrom_region_check = false;
    cdrom_subq_skew = false;
    cdrom_mechacon_version = DEFAULT_CDROM_MECHACON_VERSION;
    apply_compatibility_settings = true;
    texture_replacements.enable_vram_write_replacements = false;
    mdec_use_old_routines = false;
    bios_patch_fast_boot = false;
    runahead_frames = 0;
    runahead_for_analog_input = false;
    rewind_enable = false;
    pio_device_type = PIODeviceType::None;
    pcdrv_enable = false;
    dma_max_slice_ticks = DEFAULT_DMA_MAX_SLICE_TICKS;
    dma_halt_ticks = DEFAULT_DMA_HALT_TICKS;
    gpu_fifo_size = DEFAULT_GPU_FIFO_SIZE;
    gpu_max_run_ahead = DEFAULT_GPU_MAX_RUN_AHEAD;
  }

  // if challenge mode is enabled, disable things like rewind since they use save states
  if (Achievements::IsHardcoreModeActive())
  {
    emulation_speed = (emulation_speed != 0.0f) ? std::max(emulation_speed, 1.0f) : 0.0f;
    fast_forward_speed = (fast_forward_speed != 0.0f) ? std::max(fast_forward_speed, 1.0f) : 0.0f;
    turbo_speed = (turbo_speed != 0.0f) ? std::max(turbo_speed, 1.0f) : 0.0f;
    rewind_enable = false;
    if (cpu_overclock_enable && GetCPUOverclockPercent() < 100)
    {
      cpu_overclock_enable = false;
      UpdateOverclockActive();
    }

#ifndef __ANDROID__
    enable_gdb_server = false;
#endif

    gpu_show_vram = false;
    gpu_dump_cpu_to_vram_copies = false;
    gpu_dump_vram_to_cpu_copies = false;
  }
}

void Settings::FixIncompatibleSettings(const SettingsInterface& si, bool display_osd_messages)
{
  // fast forward boot requires fast boot
  bios_fast_forward_boot = bios_patch_fast_boot && bios_fast_forward_boot;

  if (pcdrv_enable && pcdrv_root.empty() && display_osd_messages)
  {
    Host::AddKeyedOSDMessage(OSDMessageType::Warning, "pcdrv_disabled_no_root",
                             TRANSLATE_STR("OSDMessage", "Disabling PCDrv because no root directory is specified."));
    pcdrv_enable = false;
  }

  if (gpu_pgxp_enable && gpu_renderer == GPURenderer::Software)
  {
    if (display_osd_messages)
    {
      Host::AddKeyedOSDMessage(
        OSDMessageType::Warning, "pgxp_disabled_sw",
        TRANSLATE_STR("OSDMessage", "PGXP is incompatible with the software renderer, disabling PGXP."));
    }
    gpu_pgxp_enable = false;
  }
  else if (!gpu_pgxp_enable)
  {
    gpu_pgxp_culling = false;
    gpu_pgxp_texture_correction = false;
    gpu_pgxp_color_correction = false;
    gpu_pgxp_vertex_cache = false;
    gpu_pgxp_cpu = false;
    gpu_pgxp_preserve_proj_fp = false;
    gpu_pgxp_depth_buffer = false;
    gpu_pgxp_disable_2d = false;
    gpu_pgxp_transparent_depth = false;
  }

  // texture replacements are not available without the TC or with the software renderer
  texture_replacements.enable_texture_replacements &= (gpu_renderer != GPURenderer::Software && gpu_texture_cache);
  texture_replacements.enable_vram_write_replacements &= (gpu_renderer != GPURenderer::Software);

  // GPU thread should be disabled if any debug windows are active, since they will be racing to read CPU thread state.
  if (gpu_use_thread && gpu_max_queued_frames > 0 && ImGuiManager::AreAnyDebugWindowsEnabled(si))
  {
    WARNING_LOG("Setting maximum queued frames to 0 because one or more debug windows are enabled.");
    gpu_max_queued_frames = 0;
  }

#ifndef ENABLE_MMAP_FASTMEM
  if (cpu_fastmem_mode == CPUFastmemMode::MMap)
  {
    WARNING_LOG("mmap fastmem is not available on this platform, using LUT instead.");
    cpu_fastmem_mode = CPUFastmemMode::LUT;
  }
#endif

  // fastmem should be off if we're not using the recompiler, save the allocation
  if (cpu_execution_mode != CPUExecutionMode::Recompiler)
    cpu_fastmem_mode = CPUFastmemMode::Disabled;

  if (IsRunaheadEnabled() && rewind_enable)
  {
    if (display_osd_messages)
    {
      Host::AddIconOSDMessage(OSDMessageType::Warning, "RewindDisabled", ICON_EMOJI_WARNING,
                              TRANSLATE_STR("System", "Rewind has been disabled."),
                              TRANSLATE_STR("System", "Rewind and runahead cannot be used at the same time."));
    }

    rewind_enable = false;
  }

  if (IsRunaheadEnabled())
  {
    // Block linking is good for performance, but hurts when regularly loading (i.e. runahead), since everything has to
    // be unlinked. Which would be thousands of blocks.
    if (cpu_recompiler_block_linking)
    {
      WARNING_LOG("Disabling block linking due to runahead.");
      cpu_recompiler_block_linking = false;
    }
  }

  // Don't waste time running the software renderer for CPU-only rewind when rewind isn't enabled.
  gpu_use_software_renderer_for_memory_states &= rewind_enable;
}

bool Settings::AreGPUDeviceSettingsChanged(const Settings& old_settings) const
{
  return (gpu_adapter != old_settings.gpu_adapter || gpu_use_thread != old_settings.gpu_use_thread ||
          gpu_use_debug_device != old_settings.gpu_use_debug_device ||
          gpu_use_debug_device_gpu_validation != old_settings.gpu_use_debug_device_gpu_validation ||
          gpu_prefer_gles_context != old_settings.gpu_prefer_gles_context ||
          gpu_disable_shader_cache != old_settings.gpu_disable_shader_cache ||
          gpu_disable_dual_source_blend != old_settings.gpu_disable_dual_source_blend ||
          gpu_disable_framebuffer_fetch != old_settings.gpu_disable_framebuffer_fetch ||
          gpu_disable_texture_buffers != old_settings.gpu_disable_texture_buffers ||
          gpu_disable_texture_copy_to_self != old_settings.gpu_disable_texture_copy_to_self ||
          gpu_disable_memory_import != old_settings.gpu_disable_memory_import ||
          gpu_disable_raster_order_views != old_settings.gpu_disable_raster_order_views ||
          gpu_disable_compute_shaders != old_settings.gpu_disable_compute_shaders ||
          gpu_disable_compressed_textures != old_settings.gpu_disable_compressed_textures ||
          display_exclusive_fullscreen_control != old_settings.display_exclusive_fullscreen_control);
}

void Settings::SetDefaultLogConfig(SettingsInterface& si)
{
  si.SetStringValue("Logging", "LogLevel", GetLogLevelName(Log::DEFAULT_LOG_LEVEL));
  si.SetBoolValue("Logging", "LogTimestamps", true);

#if !defined(_WIN32) && !defined(__ANDROID__)
  // On Linux, default the console to whether standard input is currently available.
  si.SetBoolValue("Logging", "LogToConsole", Log::IsConsoleOutputCurrentlyAvailable());
#else
  si.SetBoolValue("Logging", "LogToConsole", false);
#endif

  si.SetBoolValue("Logging", "LogToDebug", false);
  si.SetBoolValue("Logging", "LogToWindow", false);
  si.SetBoolValue("Logging", "LogToFile", false);
  si.SetBoolValue("Logging", "LogFileTimestamps", false);

  for (const char* channel_name : Log::GetChannelNames())
    si.SetBoolValue("Logging", channel_name, true);
}

void Settings::UpdateLogConfig(const SettingsInterface& si)
{
  const Log::Level log_level =
    ParseLogLevelName(si.GetStringValue("Logging", "LogLevel", GetLogLevelName(Log::DEFAULT_LOG_LEVEL)).c_str())
      .value_or(Log::DEFAULT_LOG_LEVEL);
  const bool log_timestamps = si.GetBoolValue("Logging", "LogTimestamps", true);
  const bool log_to_console = si.GetBoolValue("Logging", "LogToConsole", false);
  const bool log_to_debug = si.GetBoolValue("Logging", "LogToDebug", false);
  const bool log_to_file = si.GetBoolValue("Logging", "LogToFile", false);
  const bool log_file_timestamps = si.GetBoolValue("Logging", "LogFileTimestamps", false);

  Log::SetLogLevel(log_level);
  Log::SetConsoleOutputParams(log_to_console, log_timestamps);
  Log::SetDebugOutputParams(log_to_debug);

  if (log_to_file)
  {
    Log::SetFileOutputParams(log_to_file, Path::Combine(EmuFolders::DataRoot, "duckstation.log").c_str(),
                             log_file_timestamps);
  }
  else
  {
    Log::SetFileOutputParams(false, nullptr);
  }

  const auto channel_names = Log::GetChannelNames();
  for (size_t i = 0; i < channel_names.size(); i++)
    Log::SetLogChannelEnabled(static_cast<Log::Channel>(i), si.GetBoolValue("Logging", channel_names[i], true));
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
    si.SetStringValue(section.c_str(), "Type", Controller::GetControllerInfo(GetDefaultControllerType(i)).name);
  }

#ifndef __ANDROID__
  // Use the automapper to set this up.
  InputManager::MapController(si, 0, InputManager::GetGenericBindingMapping("Keyboard"), true);
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
};
static constexpr const std::array s_cpu_execution_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Interpreter (Slowest)", "CPUExecutionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Cached Interpreter (Faster)", "CPUExecutionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Recompiler (Fastest)", "CPUExecutionMode"),
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
    case GPURenderer::HardwareMetal:
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
      return GPUDevice::GetPreferredAPI(Host::GetRenderWindowInfoType());
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

static constexpr const std::array s_texture_filter_names = {
  "Nearest",     "Bilinear", "BilinearBinAlpha", "JINC2", "JINC2BinAlpha", "xBR",
  "xBRBinAlpha", "Scale2x",  "Scale3x",          "MMPX",  "MMPXEnhanced",
};
static constexpr const std::array s_texture_filter_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Nearest-Neighbor", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear (No Edge Blending)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "JINC2 (Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "JINC2 (Slow, No Edge Blending)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "xBR (Very Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "xBR (Very Slow, No Edge Blending)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Scale2x (EPX)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Scale3x (Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "MMPX (Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "MMPX Enhanced (Slow)", "GPUTextureFilter"),
};
static_assert(s_texture_filter_names.size() == static_cast<size_t>(GPUTextureFilter::Count));
static_assert(s_texture_filter_display_names.size() == static_cast<size_t>(GPUTextureFilter::Count));

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

static constexpr const std::array s_gpu_dithering_mode_names = {
  "Unscaled", "UnscaledShaderBlend", "Scaled", "ScaledShaderBlend", "TrueColor", "TrueColorFull",
};
static constexpr const std::array s_gpu_dithering_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Unscaled", "GPUDitheringMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Unscaled (Shader Blending)", "GPUDitheringMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Scaled", "GPUDitheringMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Scaled (Shader Blending)", "GPUDitheringMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "True Color", "GPUDitheringMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "True Color (Full)", "GPUDitheringMode"),
};
static_assert(s_gpu_dithering_mode_names.size() == static_cast<size_t>(GPUDitheringMode::MaxCount));
static_assert(s_gpu_dithering_mode_display_names.size() == static_cast<size_t>(GPUDitheringMode::MaxCount));

std::optional<GPUDitheringMode> Settings::ParseGPUDitheringModeName(const char* str)
{
  int index = 0;
  for (const char* name : s_gpu_dithering_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<GPUDitheringMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetGPUDitheringModeName(GPUDitheringMode mode)
{
  return s_gpu_dithering_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetGPUDitheringModeDisplayName(GPUDitheringMode mode)
{
  return Host::TranslateToCString("Settings", s_gpu_dithering_mode_display_names[static_cast<size_t>(mode)],
                                  "GPUDitheringMode");
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

static constexpr const std::array s_gpu_dump_compression_mode_names = {"Disabled", "ZstLow",    "ZstDefault", "ZstHigh",
                                                                       "XZLow",    "XZDefault", "XZHigh"};
static constexpr const std::array s_gpu_dump_compression_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Disabled", "GPUDumpCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Zstandard (Low)", "GPUDumpCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Zstandard (Default)", "GPUDumpCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Zstandard (High)", "GPUDumpCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "XZ (Low)", "GPUDumpCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "XZ (Default)", "GPUDumpCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "XZ (High)", "GPUDumpCompressionMode"),
};
static_assert(s_gpu_dump_compression_mode_names.size() == static_cast<size_t>(GPUDumpCompressionMode::MaxCount));
static_assert(s_gpu_dump_compression_mode_display_names.size() ==
              static_cast<size_t>(GPUDumpCompressionMode::MaxCount));

std::optional<GPUDumpCompressionMode> Settings::ParseGPUDumpCompressionMode(const char* str)
{
  int index = 0;
  for (const char* name : s_gpu_dump_compression_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<GPUDumpCompressionMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetGPUDumpCompressionModeName(GPUDumpCompressionMode mode)
{
  return s_gpu_dump_compression_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetGPUDumpCompressionModeDisplayName(GPUDumpCompressionMode mode)
{
  return Host::TranslateToCString("Settings", s_gpu_dump_compression_mode_display_names[static_cast<size_t>(mode)],
                                  "GPUDumpCompressionMode");
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

static constexpr const std::array s_display_crop_mode_names = {
  "None", "Overscan", "OverscanUncorrected", "Borders", "BordersUncorrected",
};
static constexpr const std::array s_display_crop_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "None", "DisplayCropMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Only Overscan Area", "DisplayCropMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Only Overscan Area (Aspect Uncorrected)", "DisplayCropMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "All Borders", "DisplayCropMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "All Borders (Aspect Uncorrected)", "DisplayCropMode"),
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

static constexpr const std::array s_display_fine_crop_mode_names = {
  "None",
  "VideoResolution",
  "InternalResolution",
  "WindowResolution",
};
static constexpr const std::array s_display_fine_crop_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "None", "DisplayFineCropMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Video Resolution", "DisplayFineCropMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Internal Resolution", "DisplayFineCropMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Window Resolution", "DisplayFineCropMode"),
};
static_assert(s_display_fine_crop_mode_names.size() == static_cast<size_t>(DisplayFineCropMode::MaxCount));
static_assert(s_display_fine_crop_mode_display_names.size() == static_cast<size_t>(DisplayFineCropMode::MaxCount));

std::optional<DisplayFineCropMode> Settings::ParseDisplayFineCropMode(const char* str)
{
  int index = 0;
  for (const char* name : s_display_fine_crop_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<DisplayFineCropMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetDisplayFineCropModeName(DisplayFineCropMode mode)
{
  return s_display_fine_crop_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetDisplayFineCropModeDisplayName(DisplayFineCropMode mode)
{
  return Host::TranslateToCString("Settings", s_display_fine_crop_mode_display_names[static_cast<size_t>(mode)],
                                  "DisplayFineCropMode");
}

static constexpr const std::string_view s_auto_aspect_ratio_name =
  TRANSLATE_DISAMBIG_NOOP("Settings", "Auto (Game Native)", "DisplayAspectRatio");
static constexpr const std::string_view s_stretch_aspect_ratio_name =
#ifndef __ANDROID__
  TRANSLATE_DISAMBIG_NOOP("Settings", "Stretch To Fill", "DisplayAspectRatio");
#else
  "Auto (Match Window)";
#endif
static constexpr const std::string_view s_par_1_1_aspect_ratio_name =
  TRANSLATE_DISAMBIG_NOOP("Settings", "PAR 1:1", "DisplayAspectRatio");

std::optional<DisplayAspectRatio> Settings::ParseDisplayAspectRatio(std::string_view str)
{
  std::optional<DisplayAspectRatio> ret;

  // Special cases.
  if (str == s_auto_aspect_ratio_name)
  {
    ret.emplace(DisplayAspectRatio::Auto());
  }
  else if (str == s_stretch_aspect_ratio_name)
  {
    ret.emplace(DisplayAspectRatio::Stretch());
  }
  else if (str == s_par_1_1_aspect_ratio_name)
  {
    ret.emplace(DisplayAspectRatio::PAR1_1());
  }
  else
  {
    const std::string_view::size_type pos = str.find(':');
    if (pos != std::string_view::npos)
    {
      const std::optional<s16> num = StringUtil::FromChars<s16>(str.substr(0, pos));
      const std::optional<s16> denom = StringUtil::FromChars<s16>(str.substr(pos + 1));
      if (num.has_value() && denom.has_value() && num.value() > 0 && denom.value() > 0)
        ret.emplace(num.value(), denom.value());
    }
  }

  return ret;
}

TinyString Settings::GetDisplayAspectRatioName(DisplayAspectRatio ar)
{
  TinyString ret;

  // Special cases.
  if (ar == DisplayAspectRatio::Auto())
    ret = s_auto_aspect_ratio_name;
  else if (ar == DisplayAspectRatio::Stretch())
    ret = s_stretch_aspect_ratio_name;
  else if (ar == DisplayAspectRatio::PAR1_1())
    ret = s_par_1_1_aspect_ratio_name;
  else
    ret.format("{}:{}", ar.numerator, ar.denominator);

  return ret;
}

TinyString Settings::GetDisplayAspectRatioDisplayName(DisplayAspectRatio ar)
{
  TinyString ret;

  // Special cases.
  if (ar == DisplayAspectRatio::Auto())
    ret = Host::TranslateToStringView("Settings", s_auto_aspect_ratio_name, "DisplayAspectRatio");
  else if (ar == DisplayAspectRatio::Stretch())
    ret = Host::TranslateToStringView("Settings", s_stretch_aspect_ratio_name, "DisplayAspectRatio");
  else if (ar == DisplayAspectRatio::PAR1_1())
    ret = Host::TranslateToStringView("Settings", s_par_1_1_aspect_ratio_name, "DisplayAspectRatio");
  else
    ret.format("{}:{}", ar.numerator, ar.denominator);

  return ret;
}

std::span<const DisplayAspectRatio> Settings::GetPredefinedDisplayAspectRatios()
{
  static constexpr const std::array s_predefined_aspect_ratios = {
    DisplayAspectRatio::Auto(), DisplayAspectRatio::Stretch(), DisplayAspectRatio{4, 3},
    DisplayAspectRatio{16, 9},  DisplayAspectRatio{19, 9},     DisplayAspectRatio{20, 9},
    DisplayAspectRatio{21, 9},  DisplayAspectRatio{16, 10},    DisplayAspectRatio::PAR1_1(),
  };
  return s_predefined_aspect_ratios;
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
  TRANSLATE_DISAMBIG_NOOP("Settings", "Auto-Detect", "ForceVideoTiming"),
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
  "Nearest", "NearestInteger", "BilinearSmooth", "BilinearHybrid", "BilinearSharp", "BilinearInteger", "Lanczos",
};
static constexpr const std::array s_display_scaling_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Nearest-Neighbor", "DisplayScalingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Nearest-Neighbor (Integer)", "DisplayScalingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear (Smooth)", "DisplayScalingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear (Hybrid)", "DisplayScalingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear (Sharp)", "DisplayScalingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear (Integer)", "DisplayScalingMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Lanczos (Sharp)", "DisplayScalingMode"),
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

std::optional<DisplayScreenshotFormat> Settings::GetDisplayScreenshotFormatFromFileName(const std::string_view filename)
{
  const std::string_view extension = Path::GetExtension(filename);
  int index = 0;
  for (const char* name : s_display_screenshot_format_extensions)
  {
    if (StringUtil::EqualNoCase(extension, name))
      return static_cast<DisplayScreenshotFormat>(index);

    index++;
  }

  return std::nullopt;
}

static constexpr const std::array s_display_osd_message_type_names = {
  "Error", "Warning", "Info", "Quick", "Persistent",
};
static_assert(s_display_osd_message_type_names.size() == static_cast<size_t>(OSDMessageType::MaxCount));

const char* Settings::GetDisplayOSDMessageTypeName(OSDMessageType type)
{
  return s_display_osd_message_type_names[static_cast<size_t>(type)];
}

static constexpr const std::array s_notification_location_names = {
  "TopLeft", "TopCenter", "TopRight", "BottomLeft", "BottomCenter", "BottomRight",
};
static_assert(s_notification_location_names.size() == static_cast<size_t>(NotificationLocation::MaxCount));
static constexpr const std::array s_notification_location_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Top Left", "NotificationLocation"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Top Center", "NotificationLocation"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Top Right", "NotificationLocation"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bottom Left", "NotificationLocation"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bottom Center", "NotificationLocation"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bottom Right", "NotificationLocation"),
};
static_assert(s_notification_location_display_names.size() == static_cast<size_t>(NotificationLocation::MaxCount));

std::optional<NotificationLocation> Settings::ParseNotificationLocation(const char* str)
{
  int index = 0;
  for (const char* name : s_notification_location_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<NotificationLocation>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetNotificationLocationName(NotificationLocation location)
{
  return s_notification_location_names[static_cast<size_t>(location)];
}

const char* Settings::GetNotificationLocationDisplayName(NotificationLocation location)
{
  return Host::TranslateToCString("Settings", s_notification_location_display_names[static_cast<size_t>(location)],
                                  "NotificationLocation");
}

static constexpr const std::array s_achievement_challenge_indicator_mode_names = {
  "Disabled",
  "PersistentIcon",
  "TemporaryIcon",
  "Notification",
};
static_assert(s_achievement_challenge_indicator_mode_names.size() ==
              static_cast<size_t>(AchievementChallengeIndicatorMode::MaxCount));
static constexpr const std::array s_achievement_challenge_indicator_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Disabled", "AchievementChallengeIndicatorMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Show Persistent Icons", "AchievementChallengeIndicatorMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Show Temporary Icons", "AchievementChallengeIndicatorMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Show Notifications", "AchievementChallengeIndicatorMode"),
};
static_assert(s_achievement_challenge_indicator_mode_display_names.size() ==
              static_cast<size_t>(AchievementChallengeIndicatorMode::MaxCount));

std::optional<AchievementChallengeIndicatorMode> Settings::ParseAchievementChallengeIndicatorMode(const char* str)
{
  int index = 0;
  for (const char* name : s_achievement_challenge_indicator_mode_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<AchievementChallengeIndicatorMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetAchievementChallengeIndicatorModeName(AchievementChallengeIndicatorMode mode)
{
  return s_achievement_challenge_indicator_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetAchievementChallengeIndicatorModeDisplayName(AchievementChallengeIndicatorMode mode)
{
  return Host::TranslateToCString("Settings",
                                  s_achievement_challenge_indicator_mode_display_names[static_cast<size_t>(mode)],
                                  "AchievementChallengeIndicatorMode");
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
  "Uncompressed", "DeflateLow", "DeflateDefault", "DeflateHigh", "ZstLow",
  "ZstDefault",   "ZstHigh",    "XZLow",          "XZDefault",   "XZHigh",
};
static constexpr const std::array s_save_state_compression_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Uncompressed", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Deflate (Low)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Deflate (Default)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Deflate (High)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Zstandard (Low)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Zstandard (Default)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Zstandard (High)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "XZ (Low)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "XZ (Default)", "SaveStateCompressionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "XZ (High)", "SaveStateCompressionMode"),
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

static constexpr const std::array s_pio_device_type_names = {
  "None",
  "XplorerCart",
};
static constexpr const std::array s_pio_device_type_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "None", "PIODeviceType"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Xplorer/Xploder Cartridge", "PIODeviceType"),
};
static_assert(s_pio_device_type_names.size() == static_cast<size_t>(PIODeviceType::MaxCount));
static_assert(s_pio_device_type_display_names.size() == static_cast<size_t>(PIODeviceType::MaxCount));

std::optional<PIODeviceType> Settings::ParsePIODeviceTypeName(const char* str)
{
  u32 index = 0;
  for (const char* name : s_pio_device_type_names)
  {
    if (StringUtil::Strcasecmp(name, str) == 0)
      return static_cast<PIODeviceType>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetPIODeviceTypeModeName(PIODeviceType type)
{
  return s_pio_device_type_names[static_cast<size_t>(type)];
}

const char* Settings::GetPIODeviceTypeModeDisplayName(PIODeviceType type)
{
  return Host::TranslateToCString("Settings", s_pio_device_type_display_names[static_cast<size_t>(type)],
                                  "PIODeviceType");
}

namespace EmuFolders {

std::string AppRoot;
std::string DataRoot;
std::string Bios;
std::string Cache;
std::string Cheats;
std::string Covers;
std::string GameIcons;
std::string GameSettings;
std::string InputProfiles;
std::string MemoryCards;
std::string Patches;
std::string Resources;
std::string SaveStates;
std::string Screenshots;
std::string Shaders;
std::string Subchannels;
std::string Textures;
std::string UserResources;
std::string Videos;

static void EnsureFolderExists(const std::string& path);

} // namespace EmuFolders

void EmuFolders::SetDefaults()
{
  Bios = Path::Combine(DataRoot, "bios");
  Cache = Path::Combine(DataRoot, "cache");
  Cheats = Path::Combine(DataRoot, "cheats");
  Covers = Path::Combine(DataRoot, "covers");
  GameIcons = Path::Combine(DataRoot, "gameicons");
  GameSettings = Path::Combine(DataRoot, "gamesettings");
  InputProfiles = Path::Combine(DataRoot, "inputprofiles");
  MemoryCards = Path::Combine(DataRoot, "memcards");
  Patches = Path::Combine(DataRoot, "patches");
  SaveStates = Path::Combine(DataRoot, "savestates");
  Screenshots = Path::Combine(DataRoot, "screenshots");
  Shaders = Path::Combine(DataRoot, "shaders");
  Subchannels = Path::Combine(DataRoot, "subchannels");
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
  GameIcons = LoadPathFromSettings(si, DataRoot, "Folders", "GameIcons", "gameicons");
  GameSettings = LoadPathFromSettings(si, DataRoot, "Folders", "GameSettings", "gamesettings");
  InputProfiles = LoadPathFromSettings(si, DataRoot, "Folders", "InputProfiles", "inputprofiles");
  MemoryCards = LoadPathFromSettings(si, DataRoot, "MemoryCards", "Directory", "memcards");
  Patches = LoadPathFromSettings(si, DataRoot, "Folders", "Patches", "patches");
  SaveStates = LoadPathFromSettings(si, DataRoot, "Folders", "SaveStates", "savestates");
  Screenshots = LoadPathFromSettings(si, DataRoot, "Folders", "Screenshots", "screenshots");
  Shaders = LoadPathFromSettings(si, DataRoot, "Folders", "Shaders", "shaders");
  Subchannels = LoadPathFromSettings(si, DataRoot, "Folders", "Subchannels", "subchannels");
  Textures = LoadPathFromSettings(si, DataRoot, "Folders", "Textures", "textures");
  UserResources = LoadPathFromSettings(si, DataRoot, "Folders", "UserResources", "resources");
  Videos = LoadPathFromSettings(si, DataRoot, "Folders", "Videos", "videos");

  DEV_LOG("BIOS Directory: {}", Bios);
  DEV_LOG("Cache Directory: {}", Cache);
  DEV_LOG("Cheats Directory: {}", Cheats);
  DEV_LOG("Covers Directory: {}", Covers);
  DEV_LOG("Game Icons Directory: {}", GameIcons);
  DEV_LOG("Game Settings Directory: {}", GameSettings);
  DEV_LOG("Input Profile Directory: {}", InputProfiles);
  DEV_LOG("MemoryCards Directory: {}", MemoryCards);
  DEV_LOG("Patches Directory: {}", Patches);
  DEV_LOG("Resources Directory: {}", Resources);
  DEV_LOG("SaveStates Directory: {}", SaveStates);
  DEV_LOG("Screenshots Directory: {}", Screenshots);
  DEV_LOG("Shaders Directory: {}", Shaders);
  DEV_LOG("Subchannels Directory: {}", Subchannels);
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
  si.SetStringValue("Folders", "GameIcons", Path::MakeRelative(GameIcons, DataRoot).c_str());
  si.SetStringValue("Folders", "GameSettings", Path::MakeRelative(GameSettings, DataRoot).c_str());
  si.SetStringValue("Folders", "InputProfiles", Path::MakeRelative(InputProfiles, DataRoot).c_str());
  si.SetStringValue("MemoryCards", "Directory", Path::MakeRelative(MemoryCards, DataRoot).c_str());
  si.SetStringValue("Folders", "Patches", Path::MakeRelative(Patches, DataRoot).c_str());
  si.SetStringValue("Folders", "SaveStates", Path::MakeRelative(SaveStates, DataRoot).c_str());
  si.SetStringValue("Folders", "Screenshots", Path::MakeRelative(Screenshots, DataRoot).c_str());
  si.SetStringValue("Folders", "Shaders", Path::MakeRelative(Shaders, DataRoot).c_str());
  si.SetStringValue("Folders", "Subchannels", Path::MakeRelative(Subchannels, DataRoot).c_str());
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
    const auto lock = Core::GetSettingsLock();
    LoadConfig(*Core::GetBaseSettingsLayer());
    EnsureFoldersExist();
  }

  if (old_gamesettings != EmuFolders::GameSettings || old_inputprofiles != EmuFolders::InputProfiles)
    System::ReloadGameSettings(false);

  if (System::IsValid() && old_memorycards != EmuFolders::MemoryCards)
    System::UpdateMemoryCards();
}

void EmuFolders::EnsureFolderExists(const std::string& path)
{
  Error error;
  if (!FileSystem::EnsureDirectoryExists(path.c_str(), false, &error))
    ERROR_LOG("Failed to create directory {}: {}", path, error.GetDescription());
}

void EmuFolders::EnsureFoldersExist()
{
  EnsureFolderExists(Bios);
  EnsureFolderExists(Cache);
  EnsureFolderExists(Path::Combine(Cache, "achievement_images"));
  EnsureFolderExists(Cheats);
  EnsureFolderExists(Covers);
  EnsureFolderExists(GameIcons);
  EnsureFolderExists(GameSettings);
  EnsureFolderExists(InputProfiles);
  EnsureFolderExists(MemoryCards);
  EnsureFolderExists(Patches);
  EnsureFolderExists(SaveStates);
  EnsureFolderExists(Screenshots);
  EnsureFolderExists(Shaders);
  EnsureFolderExists(Path::Combine(Shaders, "reshade"));
  EnsureFolderExists(Path::Combine(Shaders, "reshade" FS_OSPATH_SEPARATOR_STR "Shaders"));
  EnsureFolderExists(Path::Combine(Shaders, "reshade" FS_OSPATH_SEPARATOR_STR "Textures"));
  EnsureFolderExists(Path::Combine(Shaders, "slang"));
  EnsureFolderExists(Subchannels);
  EnsureFolderExists(Textures);
  EnsureFolderExists(UserResources);
  EnsureFolderExists(Videos);
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

bool EmuFolders::IsRunningInPortableMode()
{
  return (AppRoot == DataRoot);
}
