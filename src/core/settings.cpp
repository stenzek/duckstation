// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "settings.h"
#include "achievements.h"
#include "cheats.h"
#include "controller.h"
#include "core.h"
#include "game_list.h"
#include "gpu_types.h"
#include "gte_types.h"
#include "host.h"
#include "imgui_overlays.h"
#include "system.h"

#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/media_capture.h"
#include "util/postprocessing.h"
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

const std::array<float, 4> GPUSettings::DEFAULT_DISPLAY_OSD_MESSAGE_DURATIONS = {{
  15.0f, // Error
  10.0f, // Warning
  5.0f,  // Info
  2.5f,  // Quick
}};
static_assert(static_cast<size_t>(OSDMessageType::Persistent) ==
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
#else
const MediaCaptureBackend Settings::DEFAULT_MEDIA_CAPTURE_BACKEND = MediaCaptureBackend::FFmpeg;
#endif

const char* const Settings::INTERFACE_SECTION_NAME = "Main";
const char* const Settings::UI_SECTION_NAME = "UI";
const char* const Settings::GPU_SECTION_NAME = "GPU";
const char* const Settings::DISPLAY_SECTION_NAME = "Display";
const char* const Settings::CONSOLE_SECTION_NAME = "Console";
const char* const Settings::CPU_SECTION_NAME = "CPU";
const char* const Settings::CDROM_SECTION_NAME = "CDROM";
const char* const Settings::AUDIO_SECTION_NAME = "Audio";
const char* const Settings::BIOS_SECTION_NAME = "BIOS";
const char* const Settings::MEMORY_CARDS_SECTION_NAME = "MemoryCards";
const char* const Settings::ACHIEVEMENTS_SECTION_NAME = "Cheevos";
const char* const Settings::HACKS_SECTION_NAME = "Hacks";
const char* const Settings::DEBUG_SECTION_NAME = "Debug";
const char* const Settings::SIO_SECTION_NAME = "SIO";
const char* const Settings::PCDRV_SECTION_NAME = "PCDrv";
const char* const Settings::PIO_SECTION_NAME = "PIO";
const char* const Settings::TEXTURE_REPLACEMENTS_SECTION_NAME = "TextureReplacements";
const char* const Settings::LOGGING_SECTION_NAME = "Logging";
const char* const Settings::CONTROLLER_PORTS_SECTION_NAME = "ControllerPorts";
const char* const Settings::HOTKEYS_SECTION_NAME = "Hotkeys";
const char* const Settings::BORDER_OVERLAY_SECTION_NAME = "BorderOverlay";
const char* const Settings::FOLDERS_SECTION_NAME = "Folders";
const char* const Settings::DEBUG_WINDOWS_SECTION_NAME = "DebugWindows";

std::span<const char* const> Settings::GetSectionSaveOrder()
{
  static const std::array order = {
    // clang-format off
    Cheats::PATCHES_CONFIG_SECTION,
    Cheats::CHEATS_CONFIG_SECTION,
    INTERFACE_SECTION_NAME,
    UI_SECTION_NAME,
    "AutoUpdater",
    FOLDERS_SECTION_NAME,
    GameList::CONFIG_SECTION_NAME,
    ACHIEVEMENTS_SECTION_NAME,
    LOGGING_SECTION_NAME,
    BIOS_SECTION_NAME,
    CONSOLE_SECTION_NAME,
    CPU_SECTION_NAME,
    GPU_SECTION_NAME,
    DISPLAY_SECTION_NAME,
    CDROM_SECTION_NAME,
    AUDIO_SECTION_NAME,
    MEMORY_CARDS_SECTION_NAME,
    TEXTURE_REPLACEMENTS_SECTION_NAME,
    MediaCapture::CONFIG_SECTION_NAME,
    PostProcessing::Config::INTERNAL_CHAIN_SECTION_NAME,
    PostProcessing::Config::DISPLAY_CHAIN_SECTION_NAME,
    BORDER_OVERLAY_SECTION_NAME,
    InputManager::SOURCES_CONFIG_SECTION,
    "SDLExtra",
    CONTROLLER_PORTS_SECTION_NAME,
    "Pad1",
    "Pad2",
    "Pad3",
    "Pad4",
    "Pad5",
    "Pad6",
    "Pad7",
    "Pad8",
    HOTKEYS_SECTION_NAME,
    PIO_SECTION_NAME,
    SIO_SECTION_NAME,
    PCDRV_SECTION_NAME,
    DEBUG_SECTION_NAME,
    DEBUG_WINDOWS_SECTION_NAME,
    HACKS_SECTION_NAME,
    // clang-format on
  };

  return order;
}

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
  return std::ranges::any_of(memory_card_types, [](MemoryCardType t) {
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
  TinyString skey;

  region =
    ParseConsoleRegionName(si.GetStringViewValue(CONSOLE_SECTION_NAME, "Region",
                                                 Settings::GetConsoleRegionName(Settings::DEFAULT_CONSOLE_REGION)))
      .value_or(DEFAULT_CONSOLE_REGION);
  cpu_enable_8mb_ram = si.GetBoolValue(CONSOLE_SECTION_NAME, "Enable8MBRAM", false);

  emulation_speed = si.GetFloatValue(INTERFACE_SECTION_NAME, "EmulationSpeed", 1.0f);
  fast_forward_speed = si.GetFloatValue(INTERFACE_SECTION_NAME, "FastForwardSpeed", 0.0f);
  turbo_speed = si.GetFloatValue(INTERFACE_SECTION_NAME, "TurboSpeed", 0.0f);
  sync_to_host_refresh_rate = si.GetBoolValue(INTERFACE_SECTION_NAME, "SyncToHostRefreshRate", false);
  inhibit_screensaver = si.GetBoolValue(INTERFACE_SECTION_NAME, "InhibitScreensaver", true);
  pause_on_focus_loss = si.GetBoolValue(INTERFACE_SECTION_NAME, "PauseOnFocusLoss", false);
  pause_on_controller_disconnection = si.GetBoolValue(INTERFACE_SECTION_NAME, "PauseOnControllerDisconnection", false);
  disable_background_input = si.GetBoolValue(INTERFACE_SECTION_NAME, "DisableBackgroundInput", false);
  save_state_on_exit = si.GetBoolValue(INTERFACE_SECTION_NAME, "SaveStateOnExit", true);
  create_save_state_backups =
    si.GetBoolValue(INTERFACE_SECTION_NAME, "CreateSaveStateBackups", DEFAULT_SAVE_STATE_BACKUPS);
  confim_power_off = si.GetBoolValue(INTERFACE_SECTION_NAME, "ConfirmPowerOff", true);
  load_devices_from_save_states = si.GetBoolValue(INTERFACE_SECTION_NAME, "LoadDevicesFromSaveStates", false);
  apply_compatibility_settings = si.GetBoolValue(INTERFACE_SECTION_NAME, "ApplyCompatibilitySettings", true);
  apply_game_settings = si.GetBoolValue(INTERFACE_SECTION_NAME, "ApplyGameSettings", true);
  disable_all_enhancements = si.GetBoolValue(INTERFACE_SECTION_NAME, "DisableAllEnhancements", false);
  enable_discord_presence = si.GetBoolValue(INTERFACE_SECTION_NAME, "EnableDiscordPresence", false);
  rewind_enable = si.GetBoolValue(INTERFACE_SECTION_NAME, "RewindEnable", false);
  rewind_save_frequency = si.GetFloatValue(INTERFACE_SECTION_NAME, "RewindFrequency", 10.0f);
  rewind_save_slots =
    static_cast<u16>(std::min(si.GetUIntValue(INTERFACE_SECTION_NAME, "RewindSaveSlots", 10u), 65535u));
  runahead_frames = static_cast<u8>(std::min(si.GetUIntValue(INTERFACE_SECTION_NAME, "RunaheadFrameCount", 0u), 255u));
  runahead_for_analog_input = si.GetBoolValue(INTERFACE_SECTION_NAME, "RunaheadForAnalogInput", false);

  cpu_execution_mode = ParseCPUExecutionMode(si.GetStringViewValue(CPU_SECTION_NAME, "ExecutionMode",
                                                                   GetCPUExecutionModeName(DEFAULT_CPU_EXECUTION_MODE)))
                         .value_or(DEFAULT_CPU_EXECUTION_MODE);
  cpu_overclock_numerator = std::max(si.GetUIntValue(CPU_SECTION_NAME, "OverclockNumerator", 1u), 1u);
  cpu_overclock_denominator = std::max(si.GetUIntValue(CPU_SECTION_NAME, "OverclockDenominator", 1u), 1u);
  cpu_overclock_enable = si.GetBoolValue(CPU_SECTION_NAME, "OverclockEnable", false);
  UpdateOverclockActive();
  cpu_recompiler_memory_exceptions = si.GetBoolValue(CPU_SECTION_NAME, "RecompilerMemoryExceptions", false);
  cpu_recompiler_block_linking = si.GetBoolValue(CPU_SECTION_NAME, "RecompilerBlockLinking", true);
  cpu_recompiler_icache = si.GetBoolValue(CPU_SECTION_NAME, "RecompilerICache", false);
  cpu_fastmem_mode = ParseCPUFastmemMode(si.GetStringViewValue(CPU_SECTION_NAME, "FastmemMode",
                                                               GetCPUFastmemModeName(DEFAULT_CPU_FASTMEM_MODE)))
                       .value_or(DEFAULT_CPU_FASTMEM_MODE);

  gpu_renderer =
    ParseRendererName(si.GetStringViewValue(GPU_SECTION_NAME, "Renderer", GetRendererName(DEFAULT_GPU_RENDERER)))
      .value_or(DEFAULT_GPU_RENDERER);
  gpu_adapter = si.GetStringViewValue(GPU_SECTION_NAME, "Adapter", "");
  gpu_resolution_scale = static_cast<u8>(si.GetUIntValue(GPU_SECTION_NAME, "ResolutionScale", 1u));
  gpu_automatic_resolution_scale = (gpu_resolution_scale == 0);
  gpu_multisamples = static_cast<u8>(si.GetUIntValue(GPU_SECTION_NAME, "Multisamples", 1u));
  gpu_use_debug_device = si.GetBoolValue(GPU_SECTION_NAME, "UseDebugDevice", false);
  gpu_use_debug_device_gpu_validation = si.GetBoolValue(GPU_SECTION_NAME, "UseGPUBasedValidation", false);
  gpu_prefer_gles_context = si.GetBoolValue(GPU_SECTION_NAME, "PreferGLESContext", DEFAULT_GPU_PREFER_GLES_CONTEXT);
  gpu_disable_shader_cache = si.GetBoolValue(GPU_SECTION_NAME, "DisableShaderCache", false);
  gpu_disable_dual_source_blend = si.GetBoolValue(GPU_SECTION_NAME, "DisableDualSourceBlend", false);
  gpu_disable_framebuffer_fetch = si.GetBoolValue(GPU_SECTION_NAME, "DisableFramebufferFetch", false);
  gpu_disable_texture_buffers = si.GetBoolValue(GPU_SECTION_NAME, "DisableTextureBuffers", false);
  gpu_disable_texture_copy_to_self = si.GetBoolValue(GPU_SECTION_NAME, "DisableTextureCopyToSelf", false);
  gpu_disable_memory_import = si.GetBoolValue(GPU_SECTION_NAME, "DisableMemoryImport", false);
  gpu_disable_raster_order_views = si.GetBoolValue(GPU_SECTION_NAME, "DisableRasterOrderViews", false);
  gpu_disable_compute_shaders = si.GetBoolValue(GPU_SECTION_NAME, "DisableComputeShaders", false);
  gpu_disable_compressed_textures = si.GetBoolValue(GPU_SECTION_NAME, "DisableCompressedTextures", false);
  gpu_per_sample_shading = si.GetBoolValue(GPU_SECTION_NAME, "PerSampleShading", false);
  gpu_use_thread = si.GetBoolValue(GPU_SECTION_NAME, "UseThread", true);
  gpu_max_queued_frames =
    static_cast<u8>(si.GetUIntValue(GPU_SECTION_NAME, "MaxQueuedFrames", DEFAULT_GPU_MAX_QUEUED_FRAMES));
  gpu_use_software_renderer_for_readbacks = si.GetBoolValue(GPU_SECTION_NAME, "UseSoftwareRendererForReadbacks", false);
  gpu_use_software_renderer_for_memory_states =
    si.GetBoolValue(GPU_SECTION_NAME, "UseSoftwareRendererForMemoryStates", false);
  gpu_scaled_interlacing = si.GetBoolValue(GPU_SECTION_NAME, "ScaledInterlacing", true);
  gpu_force_round_texcoords = si.GetBoolValue(GPU_SECTION_NAME, "ForceRoundTextureCoordinates", false);
  gpu_disable_upscaled_direct_textures = si.GetBoolValue(GPU_SECTION_NAME, "DisableUpscaledDirectTextures", false);
  gpu_filter_framebuffer_uploads = si.GetBoolValue(GPU_SECTION_NAME, "FilterFramebufferUploads", false);
  gpu_filter_framebuffer_uploads_minimum_width =
    std::clamp<u16>(si.GetSaturatedIntValue<u16>(GPU_SECTION_NAME, "FilterFramebufferUploadsMinimumWidth", 1), 1,
                    static_cast<u16>(VRAM_WIDTH));
  gpu_filter_framebuffer_uploads_minimum_height =
    std::clamp<u16>(si.GetSaturatedIntValue<u16>(GPU_SECTION_NAME, "FilterFramebufferUploadsMinimumHeight", 1), 1,
                    static_cast<u16>(VRAM_HEIGHT));
  gpu_texture_filter = ParseTextureFilterName(si.GetStringViewValue(GPU_SECTION_NAME, "TextureFilter",
                                                                    GetTextureFilterName(DEFAULT_GPU_TEXTURE_FILTER)))
                         .value_or(DEFAULT_GPU_TEXTURE_FILTER);
  gpu_sprite_texture_filter =
    ParseTextureFilterName(
      si.GetStringViewValue(GPU_SECTION_NAME, "SpriteTextureFilter", GetTextureFilterName(DEFAULT_GPU_TEXTURE_FILTER)))
      .value_or(DEFAULT_GPU_TEXTURE_FILTER);
  gpu_dithering_mode =
    ParseGPUDitheringModeName(
      si.GetStringViewValue(GPU_SECTION_NAME, "DitheringMode", GetGPUDitheringModeName(DEFAULT_GPU_DITHERING_MODE)))
      .value_or(DEFAULT_GPU_DITHERING_MODE);
  gpu_line_detect_mode =
    ParseLineDetectModeName(
      si.GetStringViewValue(GPU_SECTION_NAME, "LineDetectMode", GetLineDetectModeName(DEFAULT_GPU_LINE_DETECT_MODE)))
      .value_or(DEFAULT_GPU_LINE_DETECT_MODE);
  gpu_downsample_mode =
    ParseDownsampleModeName(
      si.GetStringViewValue(GPU_SECTION_NAME, "DownsampleMode", GetDownsampleModeName(DEFAULT_GPU_DOWNSAMPLE_MODE)))
      .value_or(DEFAULT_GPU_DOWNSAMPLE_MODE);
  gpu_downsample_scale = static_cast<u8>(si.GetUIntValue(GPU_SECTION_NAME, "DownsampleScale", 1));
  gpu_wireframe_mode = ParseGPUWireframeMode(si.GetStringViewValue(GPU_SECTION_NAME, "WireframeMode",
                                                                   GetGPUWireframeModeName(DEFAULT_GPU_WIREFRAME_MODE)))
                         .value_or(DEFAULT_GPU_WIREFRAME_MODE);
  gpu_force_video_timing =
    ParseForceVideoTimingName(si.GetStringViewValue(GPU_SECTION_NAME, "ForceVideoTiming",
                                                    GetForceVideoTimingName(DEFAULT_FORCE_VIDEO_TIMING_MODE)))
      .value_or(DEFAULT_FORCE_VIDEO_TIMING_MODE);
  gpu_disable_textures = si.GetBoolValue(GPU_SECTION_NAME, "DisableTextures", false);
  gpu_disable_vertex_lighting = si.GetBoolValue(GPU_SECTION_NAME, "DisableVertexLighting", false);
  gpu_widescreen_rendering = gpu_widescreen_hack = si.GetBoolValue(GPU_SECTION_NAME, "WidescreenHack", false);
  gpu_modulation_crop = si.GetBoolValue(GPU_SECTION_NAME, "EnableModulationCrop", false);
  gpu_texture_cache = si.GetBoolValue(GPU_SECTION_NAME, "EnableTextureCache", false);
  display_24bit_chroma_smoothing = si.GetBoolValue(GPU_SECTION_NAME, "ChromaSmoothing24Bit", false);
  gpu_pgxp_enable = si.GetBoolValue(GPU_SECTION_NAME, "PGXPEnable", false);
  LoadPGXPSettings(si);
  gpu_dump_fast_replay_mode = si.GetBoolValue(GPU_SECTION_NAME, "DumpFastReplayMode", false);
  display_deinterlacing_mode =
    ParseDisplayDeinterlacingMode(
      si.GetStringViewValue(GPU_SECTION_NAME, "DeinterlacingMode",
                            GetDisplayDeinterlacingModeName(DEFAULT_DISPLAY_DEINTERLACING_MODE)))
      .value_or(DEFAULT_DISPLAY_DEINTERLACING_MODE);

  display_crop_mode = ParseDisplayCropMode(si.GetStringViewValue(DISPLAY_SECTION_NAME, "CropMode",
                                                                 GetDisplayCropModeName(DEFAULT_DISPLAY_CROP_MODE)))
                        .value_or(DEFAULT_DISPLAY_CROP_MODE);
  display_aspect_ratio = ParseDisplayAspectRatio(si.GetStringViewValue(DISPLAY_SECTION_NAME, "AspectRatio"))
                           .value_or(DEFAULT_DISPLAY_ASPECT_RATIO);
  display_fine_crop_mode = ParseDisplayFineCropMode(si.GetStringViewValue(DISPLAY_SECTION_NAME, "FineCropMode"))
                             .value_or(DEFAULT_DISPLAY_FINE_CROP_MODE);
  display_fine_crop_amount[0] = si.GetSaturatedIntValue<s16>(DISPLAY_SECTION_NAME, "FineCropLeft", 0);
  display_fine_crop_amount[1] = si.GetSaturatedIntValue<s16>(DISPLAY_SECTION_NAME, "FineCropTop", 0);
  display_fine_crop_amount[2] = si.GetSaturatedIntValue<s16>(DISPLAY_SECTION_NAME, "FineCropRight", 0);
  display_fine_crop_amount[3] = si.GetSaturatedIntValue<s16>(DISPLAY_SECTION_NAME, "FineCropBottom", 0);
  display_alignment = ParseDisplayAlignment(si.GetStringViewValue(DISPLAY_SECTION_NAME, "Alignment",
                                                                  GetDisplayAlignmentName(DEFAULT_DISPLAY_ALIGNMENT)))
                        .value_or(DEFAULT_DISPLAY_ALIGNMENT);
  display_rotation = ParseDisplayRotation(si.GetStringViewValue(DISPLAY_SECTION_NAME, "Rotation",
                                                                GetDisplayRotationName(DEFAULT_DISPLAY_ROTATION)))
                       .value_or(DEFAULT_DISPLAY_ROTATION);
  display_scaling = ParseDisplayScaling(si.GetStringViewValue(DISPLAY_SECTION_NAME, "Scaling",
                                                              GetDisplayScalingName(DEFAULT_DISPLAY_SCALING)))
                      .value_or(DEFAULT_DISPLAY_SCALING);
  display_scaling_24bit = ParseDisplayScaling(si.GetStringViewValue(DISPLAY_SECTION_NAME, "Scaling24Bit",
                                                                    GetDisplayScalingName(DEFAULT_DISPLAY_SCALING)))
                            .value_or(DEFAULT_DISPLAY_SCALING);
  display_exclusive_fullscreen_control =
    ParseDisplayExclusiveFullscreenControl(
      si.GetStringViewValue(DISPLAY_SECTION_NAME, "ExclusiveFullscreenControl",
                            GetDisplayExclusiveFullscreenControlName(DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL)))
      .value_or(DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL);
  display_screenshot_mode =
    ParseDisplayScreenshotMode(si.GetStringViewValue(DISPLAY_SECTION_NAME, "ScreenshotMode",
                                                     GetDisplayScreenshotModeName(DEFAULT_DISPLAY_SCREENSHOT_MODE)))
      .value_or(DEFAULT_DISPLAY_SCREENSHOT_MODE);
  display_screenshot_format =
    ParseDisplayScreenshotFormat(
      si.GetStringViewValue(DISPLAY_SECTION_NAME, "ScreenshotFormat",
                            GetDisplayScreenshotFormatName(DEFAULT_DISPLAY_SCREENSHOT_FORMAT)))
      .value_or(DEFAULT_DISPLAY_SCREENSHOT_FORMAT);
  display_screenshot_filename_format =
    ParseCaptureFileNameFormat(
      si.GetStringViewValue(DISPLAY_SECTION_NAME, "ScreenshotFileNameFormat",
                            GetCaptureFileNameFormatName(DEFAULT_DISPLAY_SCREENSHOT_FILENAME_FORMAT)))
      .value_or(DEFAULT_DISPLAY_SCREENSHOT_FILENAME_FORMAT);
  display_screenshot_quality = static_cast<u8>(std::clamp<u32>(
    si.GetUIntValue(DISPLAY_SECTION_NAME, "ScreenshotQuality", DEFAULT_DISPLAY_SCREENSHOT_QUALITY), 1, 100));
  display_optimal_frame_pacing =
    si.GetBoolValue(DISPLAY_SECTION_NAME, "OptimalFramePacing", DEFAULT_OPTIMAL_FRAME_PACING);
  display_pre_frame_sleep = si.GetBoolValue(DISPLAY_SECTION_NAME, "PreFrameSleep", false);
  display_pre_frame_sleep_buffer =
    si.GetFloatValue(DISPLAY_SECTION_NAME, "PreFrameSleepBuffer", DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER);
  display_skip_presenting_duplicate_frames =
    si.GetBoolValue(DISPLAY_SECTION_NAME, "SkipPresentingDuplicateFrames", false);
  display_vsync = si.GetBoolValue(DISPLAY_SECTION_NAME, "VSync", false);
  display_disable_mailbox_presentation = si.GetBoolValue(DISPLAY_SECTION_NAME, "DisableMailboxPresentation", false);
  display_force_4_3_for_24bit = si.GetBoolValue(DISPLAY_SECTION_NAME, "Force4_3For24Bit", false);
  display_active_start_offset = static_cast<s16>(si.GetIntValue(DISPLAY_SECTION_NAME, "ActiveStartOffset", 0));
  display_active_end_offset = static_cast<s16>(si.GetIntValue(DISPLAY_SECTION_NAME, "ActiveEndOffset", 0));
  display_line_start_offset = static_cast<s8>(si.GetIntValue(DISPLAY_SECTION_NAME, "LineStartOffset", 0));
  display_line_end_offset = static_cast<s8>(si.GetIntValue(DISPLAY_SECTION_NAME, "LineEndOffset", 0));
  display_show_messages = si.GetBoolValue(DISPLAY_SECTION_NAME, "ShowOSDMessages", true);
  display_animate_messages = si.GetBoolValue(DISPLAY_SECTION_NAME, "AnimateOSDMessages", true);
  display_blur_message_backgrounds = si.GetBoolValue(DISPLAY_SECTION_NAME, "BlurOSDMessageBackgrounds", true);
  display_show_fps = si.GetBoolValue(DISPLAY_SECTION_NAME, "ShowFPS", false);
  display_show_speed = si.GetBoolValue(DISPLAY_SECTION_NAME, "ShowSpeed", false);
  display_show_gpu_stats = si.GetBoolValue(DISPLAY_SECTION_NAME, "ShowGPUStatistics", false);
  display_show_resolution = si.GetBoolValue(DISPLAY_SECTION_NAME, "ShowResolution", false);
  display_show_latency_stats = si.GetBoolValue(DISPLAY_SECTION_NAME, "ShowLatencyStatistics", false);
  display_show_cpu_usage = si.GetBoolValue(DISPLAY_SECTION_NAME, "ShowCPU", false);
  display_show_gpu_usage = si.GetBoolValue(DISPLAY_SECTION_NAME, "ShowGPU", false);
  display_show_frame_times = si.GetBoolValue(DISPLAY_SECTION_NAME, "ShowFrameTimes", false);
  display_show_status_indicators = si.GetBoolValue(DISPLAY_SECTION_NAME, "ShowStatusIndicators", true);
  display_show_inputs = si.GetBoolValue(DISPLAY_SECTION_NAME, "ShowInputs", false);
  display_show_enhancements = si.GetBoolValue(DISPLAY_SECTION_NAME, "ShowEnhancements", false);
  display_auto_resize_window = si.GetBoolValue(DISPLAY_SECTION_NAME, "AutoResizeWindow", false);
  display_osd_scale = si.GetFloatValue(DISPLAY_SECTION_NAME, "OSDScale", DEFAULT_OSD_SCALE);
  display_osd_margin =
    std::max(si.GetFloatValue(DISPLAY_SECTION_NAME, "OSDMargin", ImGuiManager::DEFAULT_SCREEN_MARGIN), 0.0f);

  for (size_t i = 0; i < display_osd_message_duration.size(); i++)
  {
    skey.format("OSD{}Duration", GetDisplayOSDMessageTypeName(static_cast<OSDMessageType>(i)));
    display_osd_message_duration[i] =
      si.GetFloatValue(DISPLAY_SECTION_NAME, skey.c_str(), DEFAULT_DISPLAY_OSD_MESSAGE_DURATIONS[i]);
  }
  display_osd_message_location =
    ParseNotificationLocation(si.GetStringViewValue(DISPLAY_SECTION_NAME, "OSDMessageLocation"))
      .value_or(DEFAULT_OSD_MESSAGE_LOCATION);

  save_state_compression =
    ParseSaveStateCompressionModeName(
      si.GetStringViewValue(INTERFACE_SECTION_NAME, "SaveStateCompression",
                            GetSaveStateCompressionModeName(DEFAULT_SAVE_STATE_COMPRESSION_MODE)))
      .value_or(DEFAULT_SAVE_STATE_COMPRESSION_MODE);

  cdrom_readahead_sectors =
    static_cast<u8>(si.GetIntValue(CDROM_SECTION_NAME, "ReadaheadSectors", DEFAULT_CDROM_READAHEAD_SECTORS));
  cdrom_mechacon_version =
    ParseCDROMMechVersionName(si.GetStringViewValue(CDROM_SECTION_NAME, "MechaconVersion",
                                                    GetCDROMMechVersionName(DEFAULT_CDROM_MECHACON_VERSION)))
      .value_or(DEFAULT_CDROM_MECHACON_VERSION);
  cdrom_region_check = si.GetBoolValue(CDROM_SECTION_NAME, "RegionCheck", false);
  cdrom_subq_skew = si.GetBoolValue(CDROM_SECTION_NAME, "SubQSkew", false);
  cdrom_load_image_to_ram = si.GetBoolValue(CDROM_SECTION_NAME, "LoadImageToRAM", false);
  cdrom_load_image_patches = si.GetBoolValue(CDROM_SECTION_NAME, "LoadImagePatches", false);
  cdrom_mute_cd_audio = si.GetBoolValue(CDROM_SECTION_NAME, "MuteCDAudio", false);
  cdrom_auto_disc_change = si.GetBoolValue(CDROM_SECTION_NAME, "AutoDiscChange", false);
  cdrom_read_speedup = si.GetSaturatedIntValue<u8>(CDROM_SECTION_NAME, "ReadSpeedup", 1);
  cdrom_seek_speedup = si.GetSaturatedIntValue<u8>(CDROM_SECTION_NAME, "SeekSpeedup", 1);
  cdrom_max_seek_speedup_cycles =
    std::max(si.GetUIntValue(CDROM_SECTION_NAME, "MaxSeekSpeedupCycles", DEFAULT_CDROM_MAX_SEEK_SPEEDUP_CYCLES), 1u);
  cdrom_max_read_speedup_cycles =
    std::max(si.GetUIntValue(CDROM_SECTION_NAME, "MaxReadSpeedupCycles", DEFAULT_CDROM_MAX_READ_SPEEDUP_CYCLES), 1u);
  mdec_disable_cdrom_speedup = si.GetBoolValue(CDROM_SECTION_NAME, "DisableSpeedupOnMDEC", false);

  audio_backend =
    AudioStream::ParseBackendName(
      si.GetStringViewValue(AUDIO_SECTION_NAME, "Backend", AudioStream::GetBackendName(AudioStream::DEFAULT_BACKEND)))
      .value_or(AudioStream::DEFAULT_BACKEND);
  audio_driver = si.GetStringViewValue(AUDIO_SECTION_NAME, "Driver");
  audio_output_device = si.GetStringViewValue(AUDIO_SECTION_NAME, "OutputDevice");
  audio_stream_parameters.Load(si, AUDIO_SECTION_NAME);
  audio_output_volume = si.GetSaturatedIntValue<u8>(AUDIO_SECTION_NAME, "OutputVolume", 100);
  audio_fast_forward_volume = si.GetSaturatedIntValue<u8>(AUDIO_SECTION_NAME, "FastForwardVolume", 100);
  audio_output_muted = si.GetBoolValue(AUDIO_SECTION_NAME, "OutputMuted", false);

  bios_tty_logging = si.GetBoolValue(BIOS_SECTION_NAME, "TTYLogging", false);
  bios_patch_fast_boot = si.GetBoolValue(BIOS_SECTION_NAME, "PatchFastBoot", DEFAULT_FAST_BOOT_VALUE);
  bios_fast_forward_boot = si.GetBoolValue(BIOS_SECTION_NAME, "FastForwardBoot", false);

  multitap_mode = ParseMultitapModeName(controller_si.GetStringViewValue(CONTROLLER_PORTS_SECTION_NAME, "MultitapMode",
                                                                         GetMultitapModeName(DEFAULT_MULTITAP_MODE)))
                    .value_or(DEFAULT_MULTITAP_MODE);

  const std::array<bool, 2> mtap_enabled = Controller::GetMultitapEnabledPorts(multitap_mode);
  for (u32 pad = 0; pad < NUM_CONTROLLER_AND_CARD_PORTS; pad++)
  {
    // Ignore types when multitap not enabled
    if (Controller::PadIsMultitapSlot(pad))
    {
      const auto& [port, slot] = Controller::ConvertPadToPortAndSlot(pad);
      if (!mtap_enabled[port])
      {
        controller_types[pad] = ControllerType::None;
        memory_card_types[pad] = MemoryCardType::None;
        continue;
      }
    }

    const ControllerType default_type = (pad == 0) ? DEFAULT_CONTROLLER_1_TYPE : DEFAULT_CONTROLLER_2_TYPE;
    const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(controller_si.GetStringViewValue(
      Controller::GetSettingsSection(pad).c_str(), "Type", Controller::GetControllerInfo(default_type).name));
    controller_types[pad] = cinfo ? cinfo->type : default_type;

    const MemoryCardType default_card_type = (pad == 0) ? DEFAULT_MEMORY_CARD_1_TYPE : DEFAULT_MEMORY_CARD_2_TYPE;
    skey.format("Card{}Type", pad + 1);
    memory_card_types[pad] = ParseMemoryCardTypeName(si.GetStringViewValue(MEMORY_CARDS_SECTION_NAME, skey.c_str()))
                               .value_or(default_card_type);
    skey.format("Card{}Path", pad + 1);
    memory_card_paths[pad] = si.GetStringViewValue(MEMORY_CARDS_SECTION_NAME, skey.c_str());
  }

  memory_card_use_playlist_title = si.GetBoolValue(MEMORY_CARDS_SECTION_NAME, "UsePlaylistTitle", true);
  memory_card_fast_forward_access = si.GetBoolValue(MEMORY_CARDS_SECTION_NAME, "FastForwardAccess", false);

  achievements_enabled = si.GetBoolValue(ACHIEVEMENTS_SECTION_NAME, "Enabled", false);
  achievements_hardcore_mode = si.GetBoolValue(ACHIEVEMENTS_SECTION_NAME, "ChallengeMode", false);
  achievements_encore_mode = si.GetBoolValue(ACHIEVEMENTS_SECTION_NAME, "EncoreMode", false);
  achievements_spectator_mode = si.GetBoolValue(ACHIEVEMENTS_SECTION_NAME, "SpectatorMode", false);
  achievements_track_unofficial = si.GetBoolValue(ACHIEVEMENTS_SECTION_NAME, "UnofficialTestMode", false);
  achievements_use_raintegration = si.GetBoolValue(ACHIEVEMENTS_SECTION_NAME, "UseRAIntegration", false);
  achievements_notifications = si.GetBoolValue(ACHIEVEMENTS_SECTION_NAME, "Notifications", true);
  achievements_leaderboard_notifications = si.GetBoolValue(ACHIEVEMENTS_SECTION_NAME, "LeaderboardNotifications", true);
  achievements_leaderboard_trackers = si.GetBoolValue(ACHIEVEMENTS_SECTION_NAME, "LeaderboardTrackers", true);
  achievements_sound_effects = si.GetBoolValue(ACHIEVEMENTS_SECTION_NAME, "SoundEffects", true);
  achievements_prefetch_badges =
    si.GetBoolValue(ACHIEVEMENTS_SECTION_NAME, "PrefetchBadges", DEFAULT_ACHIEVEMENT_BADGE_PREFETCH);
  achievements_rich_presence_monitor = si.GetBoolValue(ACHIEVEMENTS_SECTION_NAME, "RichPresenceMonitor", false);
  achievements_notification_location =
    ParseNotificationLocation(si.GetStringViewValue(ACHIEVEMENTS_SECTION_NAME, "NotificationLocation"))
      .value_or(DEFAULT_ACHIEVEMENT_NOTIFICATION_LOCATION);
  achievements_indicator_location =
    ParseNotificationLocation(si.GetStringViewValue(ACHIEVEMENTS_SECTION_NAME, "IndicatorLocation"))
      .value_or(DEFAULT_ACHIEVEMENT_INDICATOR_LOCATION);
  achievements_challenge_indicator_mode =
    ParseAchievementChallengeIndicatorMode(si.GetStringViewValue(ACHIEVEMENTS_SECTION_NAME, "ChallengeIndicatorMode"))
      .value_or(DEFAULT_ACHIEVEMENT_CHALLENGE_INDICATOR_MODE);
  achievements_progress_indicator_mode =
    ParseAchievementProgressIndicatorMode(si.GetStringViewValue(ACHIEVEMENTS_SECTION_NAME, "ProgressIndicatorMode"))
      .value_or(DEFAULT_ACHIEVEMENT_PROGRESS_INDICATOR_MODE);
  achievements_notification_duration = si.GetSaturatedIntValue<u8>(ACHIEVEMENTS_SECTION_NAME, "NotificationsDuration",
                                                                   DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME);
  achievements_leaderboard_duration = si.GetSaturatedIntValue<u8>(ACHIEVEMENTS_SECTION_NAME, "LeaderboardsDuration",
                                                                  DEFAULT_LEADERBOARD_NOTIFICATION_TIME);
  achievements_notification_scale =
    si.GetSaturatedIntValue<s16>(ACHIEVEMENTS_SECTION_NAME, "NotificationScale", ACHIEVEMENT_NOTIFICATION_SCALE_AUTO);
  achievements_indicator_scale =
    si.GetSaturatedIntValue<s16>(ACHIEVEMENTS_SECTION_NAME, "IndicatorScale", ACHIEVEMENT_NOTIFICATION_SCALE_AUTO);

  dma_max_slice_ticks = si.GetIntValue(HACKS_SECTION_NAME, "DMAMaxSliceTicks", DEFAULT_DMA_MAX_SLICE_TICKS);
  dma_halt_ticks = si.GetIntValue(HACKS_SECTION_NAME, "DMAHaltTicks", DEFAULT_DMA_HALT_TICKS);
  gpu_fifo_size = si.GetUIntValue(HACKS_SECTION_NAME, "GPUFIFOSize", DEFAULT_GPU_FIFO_SIZE);
  gpu_max_run_ahead = si.GetIntValue(HACKS_SECTION_NAME, "GPUMaxRunAhead", DEFAULT_GPU_MAX_RUN_AHEAD);
  mdec_use_old_routines = si.GetBoolValue(HACKS_SECTION_NAME, "UseOldMDECRoutines", false);
  export_shared_memory = si.GetBoolValue(HACKS_SECTION_NAME, "ExportSharedMemory", false);

  pcsx_expansion_region_enable = si.GetBoolValue(DEBUG_SECTION_NAME, "PCSXExpansionRegion", false);
  gpu_show_vram = si.GetBoolValue(DEBUG_SECTION_NAME, "ShowVRAM");
  gpu_dump_cpu_to_vram_copies = si.GetBoolValue(DEBUG_SECTION_NAME, "DumpCPUToVRAMCopies");
  gpu_dump_vram_to_cpu_copies = si.GetBoolValue(DEBUG_SECTION_NAME, "DumpVRAMToCPUCopies");

  enable_gdb_server = si.GetBoolValue(DEBUG_SECTION_NAME, "EnableGDBServer");
  gdb_server_port = static_cast<u16>(si.GetUIntValue(DEBUG_SECTION_NAME, "GDBServerPort", DEFAULT_GDB_SERVER_PORT));

  sio_redirect_to_tty = si.GetBoolValue(SIO_SECTION_NAME, "RedirectToTTY", false);

  pcdrv_enable = si.GetBoolValue(PCDRV_SECTION_NAME, "Enabled", false);
  pcdrv_enable_writes = si.GetBoolValue(PCDRV_SECTION_NAME, "EnableWrites", false);
  pcdrv_root = Path::ToNativePath(si.GetStringViewValue(PCDRV_SECTION_NAME, "Root"));

  debug_window_visibility = ImGuiManager::LoadDebugWindowVisibility(si);

  texture_replacements.enable_texture_replacements =
    si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "EnableTextureReplacements", false);
  texture_replacements.enable_vram_write_replacements =
    si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "EnableVRAMWriteReplacements", false);
  texture_replacements.always_track_uploads =
    si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "AlwaysTrackUploads", false);
  texture_replacements.preload_textures = si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "PreloadTextures", false);
  texture_replacements.dump_textures = si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpTextures", false);
  texture_replacements.dump_replaced_textures =
    si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpReplacedTextures", true);
  texture_replacements.dump_vram_writes = si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpVRAMWrites", false);

  texture_replacements.config.dump_texture_pages =
    si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpTexturePages", false);
  texture_replacements.config.dump_full_texture_pages =
    si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpFullTexturePages", false);
  texture_replacements.config.dump_texture_force_alpha_channel =
    si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpTextureForceAlphaChannel", false);
  texture_replacements.config.dump_vram_write_force_alpha_channel =
    si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpVRAMWriteForceAlphaChannel", true);
  texture_replacements.config.dump_c16_textures =
    si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpC16Textures", false);
  texture_replacements.config.reduce_palette_range =
    si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "ReducePaletteRange", true);
  texture_replacements.config.convert_copies_to_writes =
    si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "ConvertCopiesToWrites", false);
  texture_replacements.config.replacement_scale_linear_filter =
    si.GetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "ReplacementScaleLinearFilter", false);

  texture_replacements.config.max_hash_cache_entries =
    si.GetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "MaxHashCacheEntries",
                    TextureReplacementSettings::Configuration::DEFAULT_MAX_HASH_CACHE_ENTRIES);
  texture_replacements.config.max_hash_cache_vram_usage_mb =
    si.GetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "MaxHashCacheVRAMUsageMB",
                    TextureReplacementSettings::Configuration::DEFAULT_MAX_HASH_CACHE_VRAM_USAGE_MB);
  texture_replacements.config.max_replacement_cache_vram_usage_mb =
    si.GetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "MaxReplacementCacheVRAMUsage",
                    TextureReplacementSettings::Configuration::DEFAULT_MAX_REPLACEMENT_CACHE_VRAM_USAGE_MB);

  texture_replacements.config.max_vram_write_splits =
    si.GetSaturatedIntValue<u16>(TEXTURE_REPLACEMENTS_SECTION_NAME, "MaxVRAMWriteSplits", 0);
  texture_replacements.config.max_vram_write_coalesce_width =
    si.GetSaturatedIntValue<u16>(TEXTURE_REPLACEMENTS_SECTION_NAME, "MaxVRAMWriteCoalesceWidth", 0);
  texture_replacements.config.max_vram_write_coalesce_height =
    si.GetSaturatedIntValue<u16>(TEXTURE_REPLACEMENTS_SECTION_NAME, "MaxVRAMWriteCoalesceHeight", 0);

  texture_replacements.config.texture_dump_width_threshold =
    si.GetSaturatedIntValue<u16>(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpTextureWidthThreshold", 16);
  texture_replacements.config.texture_dump_height_threshold =
    si.GetSaturatedIntValue<u16>(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpTextureHeightThreshold", 16);
  texture_replacements.config.vram_write_dump_width_threshold =
    si.GetSaturatedIntValue<u16>(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpVRAMWriteWidthThreshold", 128);
  texture_replacements.config.vram_write_dump_height_threshold =
    si.GetSaturatedIntValue<u16>(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpVRAMWriteHeightThreshold", 128);

  pio_device_type = ParsePIODeviceTypeName(si.GetStringViewValue(PIO_SECTION_NAME, "DeviceType",
                                                                 GetPIODeviceTypeModeName(DEFAULT_PIO_DEVICE_TYPE)))
                      .value_or(DEFAULT_PIO_DEVICE_TYPE);
  pio_flash_image_path = si.GetStringViewValue(PIO_SECTION_NAME, "FlashImagePath");
  pio_flash_write_enable = si.GetBoolValue(PIO_SECTION_NAME, "FlashImageWriteEnable", false);
  pio_switch_active = si.GetBoolValue(PIO_SECTION_NAME, "SwitchActive", true);
}

void Settings::LoadPGXPSettings(const SettingsInterface& si)
{
  gpu_pgxp_culling = si.GetBoolValue(GPU_SECTION_NAME, "PGXPCulling", true);
  gpu_pgxp_texture_correction = si.GetBoolValue(GPU_SECTION_NAME, "PGXPTextureCorrection", true);
  gpu_pgxp_color_correction = si.GetBoolValue(GPU_SECTION_NAME, "PGXPColorCorrection", false);
  gpu_pgxp_vertex_cache = si.GetBoolValue(GPU_SECTION_NAME, "PGXPVertexCache", false);
  gpu_pgxp_cpu = si.GetBoolValue(GPU_SECTION_NAME, "PGXPCPU", false);
  gpu_pgxp_preserve_proj_fp = si.GetBoolValue(GPU_SECTION_NAME, "PGXPPreserveProjFP", false);
  gpu_pgxp_tolerance = si.GetFloatValue(GPU_SECTION_NAME, "PGXPTolerance", -1.0f);
  gpu_pgxp_depth_buffer = si.GetBoolValue(GPU_SECTION_NAME, "PGXPDepthBuffer", false);
  gpu_pgxp_disable_2d = si.GetBoolValue(GPU_SECTION_NAME, "PGXPDisableOn2DPolygons", false);
  gpu_pgxp_transparent_depth = si.GetBoolValue(GPU_SECTION_NAME, "PGXPTransparentDepthTest", false);
  SetPGXPDepthClearThreshold(
    si.GetFloatValue(GPU_SECTION_NAME, "PGXPDepthThreshold", DEFAULT_GPU_PGXP_DEPTH_THRESHOLD));
}

void Settings::Save(SettingsInterface& si, bool for_copy) const
{
  TinyString skey;

  si.SetStringValue(CONSOLE_SECTION_NAME, "Region", GetConsoleRegionName(region));
  si.SetBoolValue(CONSOLE_SECTION_NAME, "Enable8MBRAM", cpu_enable_8mb_ram);

  si.SetFloatValue(INTERFACE_SECTION_NAME, "EmulationSpeed", emulation_speed);
  si.SetFloatValue(INTERFACE_SECTION_NAME, "FastForwardSpeed", fast_forward_speed);
  si.SetFloatValue(INTERFACE_SECTION_NAME, "TurboSpeed", turbo_speed);

  if (!for_copy)
  {
    si.SetBoolValue(INTERFACE_SECTION_NAME, "SyncToHostRefreshRate", sync_to_host_refresh_rate);
    si.SetBoolValue(INTERFACE_SECTION_NAME, "InhibitScreensaver", inhibit_screensaver);
    si.SetBoolValue(INTERFACE_SECTION_NAME, "PauseOnFocusLoss", pause_on_focus_loss);
    si.SetBoolValue(INTERFACE_SECTION_NAME, "PauseOnControllerDisconnection", pause_on_controller_disconnection);
    si.SetBoolValue(INTERFACE_SECTION_NAME, "SaveStateOnExit", save_state_on_exit);
    si.SetBoolValue(INTERFACE_SECTION_NAME, "CreateSaveStateBackups", create_save_state_backups);
    si.SetStringValue(INTERFACE_SECTION_NAME, "SaveStateCompression",
                      GetSaveStateCompressionModeName(save_state_compression));
    si.SetBoolValue(INTERFACE_SECTION_NAME, "ConfirmPowerOff", confim_power_off);
    si.SetBoolValue(INTERFACE_SECTION_NAME, "EnableDiscordPresence", enable_discord_presence);
    si.SetBoolValue(INTERFACE_SECTION_NAME, "LoadDevicesFromSaveStates", load_devices_from_save_states);
    si.SetBoolValue(INTERFACE_SECTION_NAME, "ApplyCompatibilitySettings", apply_compatibility_settings);
    si.SetBoolValue(INTERFACE_SECTION_NAME, "ApplyGameSettings", apply_game_settings);
    si.SetBoolValue(INTERFACE_SECTION_NAME, "DisableAllEnhancements", disable_all_enhancements);
  }

  si.SetBoolValue(INTERFACE_SECTION_NAME, "DisableBackgroundInput", disable_background_input);

  si.SetBoolValue(INTERFACE_SECTION_NAME, "RewindEnable", rewind_enable);
  si.SetFloatValue(INTERFACE_SECTION_NAME, "RewindFrequency", rewind_save_frequency);
  si.SetUIntValue(INTERFACE_SECTION_NAME, "RewindSaveSlots", rewind_save_slots);
  si.SetUIntValue(INTERFACE_SECTION_NAME, "RunaheadFrameCount", runahead_frames);
  si.SetBoolValue(INTERFACE_SECTION_NAME, "RunaheadForAnalogInput", runahead_for_analog_input);

  si.SetStringValue(CPU_SECTION_NAME, "ExecutionMode", GetCPUExecutionModeName(cpu_execution_mode));
  si.SetBoolValue(CPU_SECTION_NAME, "OverclockEnable", cpu_overclock_enable);
  si.SetIntValue(CPU_SECTION_NAME, "OverclockNumerator", cpu_overclock_numerator);
  si.SetIntValue(CPU_SECTION_NAME, "OverclockDenominator", cpu_overclock_denominator);
  if (!for_copy)
  {
    si.SetBoolValue(CPU_SECTION_NAME, "RecompilerMemoryExceptions", cpu_recompiler_memory_exceptions);
    si.SetBoolValue(CPU_SECTION_NAME, "RecompilerBlockLinking", cpu_recompiler_block_linking);
    si.SetBoolValue(CPU_SECTION_NAME, "RecompilerICache", cpu_recompiler_icache);
    si.SetStringValue(CPU_SECTION_NAME, "FastmemMode", GetCPUFastmemModeName(cpu_fastmem_mode));
  }

  si.SetStringValue(GPU_SECTION_NAME, "Renderer", GetRendererName(gpu_renderer));
  si.SetStringValue(GPU_SECTION_NAME, "Adapter", gpu_adapter.c_str());
  si.SetUIntValue(GPU_SECTION_NAME, "ResolutionScale", gpu_resolution_scale);
  si.SetUIntValue(GPU_SECTION_NAME, "Multisamples", gpu_multisamples);

  if (!for_copy)
  {
    si.SetBoolValue(GPU_SECTION_NAME, "UseDebugDevice", gpu_use_debug_device);
    si.SetBoolValue(GPU_SECTION_NAME, "UseGPUBasedValidation", gpu_use_debug_device_gpu_validation);
    si.SetBoolValue(GPU_SECTION_NAME, "PreferGLESContext", gpu_prefer_gles_context);
    si.SetBoolValue(GPU_SECTION_NAME, "DisableShaderCache", gpu_disable_shader_cache);
    si.SetBoolValue(GPU_SECTION_NAME, "DisableDualSourceBlend", gpu_disable_dual_source_blend);
    si.SetBoolValue(GPU_SECTION_NAME, "DisableFramebufferFetch", gpu_disable_framebuffer_fetch);
    si.SetBoolValue(GPU_SECTION_NAME, "DisableTextureBuffers", gpu_disable_texture_buffers);
    si.SetBoolValue(GPU_SECTION_NAME, "DisableTextureCopyToSelf", gpu_disable_texture_copy_to_self);
    si.SetBoolValue(GPU_SECTION_NAME, "DisableMemoryImport", gpu_disable_memory_import);
    si.SetBoolValue(GPU_SECTION_NAME, "DisableRasterOrderViews", gpu_disable_raster_order_views);
    si.SetBoolValue(GPU_SECTION_NAME, "DisableComputeShaders", gpu_disable_compute_shaders);
    si.SetBoolValue(GPU_SECTION_NAME, "DisableCompressedTextures", gpu_disable_compressed_textures);
  }

  si.SetBoolValue(GPU_SECTION_NAME, "PerSampleShading", gpu_per_sample_shading);
  si.SetUIntValue(GPU_SECTION_NAME, "MaxQueuedFrames", gpu_max_queued_frames);
  si.SetBoolValue(GPU_SECTION_NAME, "UseThread", gpu_use_thread);
  si.SetBoolValue(GPU_SECTION_NAME, "UseSoftwareRendererForReadbacks", gpu_use_software_renderer_for_readbacks);
  si.SetBoolValue(GPU_SECTION_NAME, "UseSoftwareRendererForMemoryStates", gpu_use_software_renderer_for_memory_states);
  si.SetBoolValue(GPU_SECTION_NAME, "ScaledInterlacing", gpu_scaled_interlacing);
  si.SetBoolValue(GPU_SECTION_NAME, "ForceRoundTextureCoordinates", gpu_force_round_texcoords);
  si.SetBoolValue(GPU_SECTION_NAME, "DisableUpscaledDirectTextures", gpu_disable_upscaled_direct_textures);
  si.SetBoolValue(GPU_SECTION_NAME, "FilterFramebufferUploads", gpu_filter_framebuffer_uploads);
  si.SetUIntValue(GPU_SECTION_NAME, "FilterFramebufferUploadsMinimumWidth",
                  gpu_filter_framebuffer_uploads_minimum_width);
  si.SetUIntValue(GPU_SECTION_NAME, "FilterFramebufferUploadsMinimumHeight",
                  gpu_filter_framebuffer_uploads_minimum_height);
  si.SetStringValue(GPU_SECTION_NAME, "TextureFilter", GetTextureFilterName(gpu_texture_filter));
  si.SetStringValue(GPU_SECTION_NAME, "SpriteTextureFilter", GetTextureFilterName(gpu_sprite_texture_filter));
  si.SetStringValue(GPU_SECTION_NAME, "DitheringMode", GetGPUDitheringModeName(gpu_dithering_mode));
  si.SetStringValue(GPU_SECTION_NAME, "LineDetectMode", GetLineDetectModeName(gpu_line_detect_mode));
  si.SetStringValue(GPU_SECTION_NAME, "DownsampleMode", GetDownsampleModeName(gpu_downsample_mode));
  si.SetUIntValue(GPU_SECTION_NAME, "DownsampleScale", gpu_downsample_scale);
  si.SetStringValue(GPU_SECTION_NAME, "WireframeMode", GetGPUWireframeModeName(gpu_wireframe_mode));
  si.SetStringValue(GPU_SECTION_NAME, "ForceVideoTiming", GetForceVideoTimingName(gpu_force_video_timing));
  si.SetBoolValue(GPU_SECTION_NAME, "DisableTextures", gpu_disable_textures);
  si.SetBoolValue(GPU_SECTION_NAME, "DisableVertexLighting", gpu_disable_vertex_lighting);
  si.SetBoolValue(GPU_SECTION_NAME, "WidescreenHack", gpu_widescreen_rendering);
  si.SetBoolValue(GPU_SECTION_NAME, "EnableModulationCrop", gpu_modulation_crop);
  si.SetBoolValue(GPU_SECTION_NAME, "EnableTextureCache", gpu_texture_cache);
  si.SetBoolValue(GPU_SECTION_NAME, "ChromaSmoothing24Bit", display_24bit_chroma_smoothing);
  si.SetBoolValue(GPU_SECTION_NAME, "PGXPEnable", gpu_pgxp_enable);
  si.SetBoolValue(GPU_SECTION_NAME, "PGXPCulling", gpu_pgxp_culling);
  si.SetBoolValue(GPU_SECTION_NAME, "PGXPTextureCorrection", gpu_pgxp_texture_correction);
  si.SetBoolValue(GPU_SECTION_NAME, "PGXPColorCorrection", gpu_pgxp_color_correction);
  si.SetBoolValue(GPU_SECTION_NAME, "PGXPVertexCache", gpu_pgxp_vertex_cache);
  si.SetBoolValue(GPU_SECTION_NAME, "PGXPCPU", gpu_pgxp_cpu);
  si.SetBoolValue(GPU_SECTION_NAME, "PGXPPreserveProjFP", gpu_pgxp_preserve_proj_fp);
  si.SetFloatValue(GPU_SECTION_NAME, "PGXPTolerance", gpu_pgxp_tolerance);
  si.SetBoolValue(GPU_SECTION_NAME, "PGXPDepthBuffer", gpu_pgxp_depth_buffer);
  si.SetBoolValue(GPU_SECTION_NAME, "PGXPDisableOn2DPolygons", gpu_pgxp_disable_2d);
  si.SetBoolValue(GPU_SECTION_NAME, "PGXPTransparentDepthTest", gpu_pgxp_transparent_depth);
  si.SetFloatValue(GPU_SECTION_NAME, "PGXPDepthThreshold", GetPGXPDepthClearThreshold());
  si.SetBoolValue(GPU_SECTION_NAME, "DumpFastReplayMode", gpu_dump_fast_replay_mode);
  si.SetStringValue(GPU_SECTION_NAME, "DeinterlacingMode", GetDisplayDeinterlacingModeName(display_deinterlacing_mode));

  si.SetStringValue(DISPLAY_SECTION_NAME, "CropMode", GetDisplayCropModeName(display_crop_mode));
  si.SetBoolValue(DISPLAY_SECTION_NAME, "Force4_3For24Bit", display_force_4_3_for_24bit);
  si.SetStringValue(DISPLAY_SECTION_NAME, "AspectRatio", GetDisplayAspectRatioName(display_aspect_ratio).c_str());
  si.SetStringValue(DISPLAY_SECTION_NAME, "FineCropMode", GetDisplayFineCropModeName(display_fine_crop_mode));
  si.SetIntValue(DISPLAY_SECTION_NAME, "FineCropLeft", display_fine_crop_amount[0]);
  si.SetIntValue(DISPLAY_SECTION_NAME, "FineCropTop", display_fine_crop_amount[1]);
  si.SetIntValue(DISPLAY_SECTION_NAME, "FineCropRight", display_fine_crop_amount[2]);
  si.SetIntValue(DISPLAY_SECTION_NAME, "FineCropBottom", display_fine_crop_amount[3]);
  si.SetStringValue(DISPLAY_SECTION_NAME, "Alignment", GetDisplayAlignmentName(display_alignment));
  si.SetStringValue(DISPLAY_SECTION_NAME, "Rotation", GetDisplayRotationName(display_rotation));
  si.SetStringValue(DISPLAY_SECTION_NAME, "Scaling", GetDisplayScalingName(display_scaling));
  si.SetStringValue(DISPLAY_SECTION_NAME, "Scaling24Bit", GetDisplayScalingName(display_scaling_24bit));
  si.SetBoolValue(DISPLAY_SECTION_NAME, "OptimalFramePacing", display_optimal_frame_pacing);
  si.SetBoolValue(DISPLAY_SECTION_NAME, "PreFrameSleep", display_pre_frame_sleep);
  si.SetBoolValue(DISPLAY_SECTION_NAME, "SkipPresentingDuplicateFrames", display_skip_presenting_duplicate_frames);
  si.SetFloatValue(DISPLAY_SECTION_NAME, "PreFrameSleepBuffer", display_pre_frame_sleep_buffer);
  si.SetBoolValue(DISPLAY_SECTION_NAME, "VSync", display_vsync);
  si.SetBoolValue(DISPLAY_SECTION_NAME, "DisableMailboxPresentation", display_disable_mailbox_presentation);
  si.SetStringValue(DISPLAY_SECTION_NAME, "ExclusiveFullscreenControl",
                    GetDisplayExclusiveFullscreenControlName(display_exclusive_fullscreen_control));
  si.SetStringValue(DISPLAY_SECTION_NAME, "ScreenshotMode", GetDisplayScreenshotModeName(display_screenshot_mode));
  si.SetStringValue(DISPLAY_SECTION_NAME, "ScreenshotFormat",
                    GetDisplayScreenshotFormatName(display_screenshot_format));
  si.SetStringValue(DISPLAY_SECTION_NAME, "ScreenshotFileNameFormat",
                    GetCaptureFileNameFormatName(display_screenshot_filename_format));
  si.SetUIntValue(DISPLAY_SECTION_NAME, "ScreenshotQuality", display_screenshot_quality);
  if (!for_copy)
  {
    si.SetIntValue(DISPLAY_SECTION_NAME, "ActiveStartOffset", display_active_start_offset);
    si.SetIntValue(DISPLAY_SECTION_NAME, "ActiveEndOffset", display_active_end_offset);
    si.SetIntValue(DISPLAY_SECTION_NAME, "LineStartOffset", display_line_start_offset);
    si.SetIntValue(DISPLAY_SECTION_NAME, "LineEndOffset", display_line_end_offset);
  }

  if (!for_copy)
  {
    si.SetBoolValue(DISPLAY_SECTION_NAME, "ShowOSDMessages", display_show_messages);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "AnimateOSDMessages", display_animate_messages);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "BlurOSDMessageBackgrounds", display_blur_message_backgrounds);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "ShowFPS", display_show_fps);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "ShowSpeed", display_show_speed);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "ShowResolution", display_show_resolution);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "ShowLatencyStatistics", display_show_latency_stats);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "ShowGPUStatistics", display_show_gpu_stats);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "ShowCPU", display_show_cpu_usage);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "ShowGPU", display_show_gpu_usage);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "ShowFrameTimes", display_show_frame_times);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "ShowStatusIndicators", display_show_status_indicators);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "ShowInputs", display_show_inputs);
    si.SetBoolValue(DISPLAY_SECTION_NAME, "ShowEnhancements", display_show_enhancements);
    si.SetFloatValue(DISPLAY_SECTION_NAME, "OSDScale", display_osd_scale);
    si.SetFloatValue(DISPLAY_SECTION_NAME, "OSDMargin", display_osd_margin);

    for (size_t i = 0; i < display_osd_message_duration.size(); i++)
    {
      skey.format("OSD{}Duration", GetDisplayOSDMessageTypeName(static_cast<OSDMessageType>(i)));
      si.SetFloatValue(DISPLAY_SECTION_NAME, skey.c_str(), display_osd_message_duration[i]);
    }

    si.SetStringValue(DISPLAY_SECTION_NAME, "OSDMessageLocation",
                      GetNotificationLocationName(display_osd_message_location));
  }

  si.SetBoolValue(DISPLAY_SECTION_NAME, "AutoResizeWindow", display_auto_resize_window);

  si.SetBoolValue(CDROM_SECTION_NAME, "LoadImageToRAM", cdrom_load_image_to_ram);
  si.SetBoolValue(CDROM_SECTION_NAME, "LoadImagePatches", cdrom_load_image_patches);
  si.SetBoolValue(CDROM_SECTION_NAME, "MuteCDAudio", cdrom_mute_cd_audio);
  si.SetBoolValue(CDROM_SECTION_NAME, "AutoDiscChange", cdrom_auto_disc_change);
  si.SetUIntValue(CDROM_SECTION_NAME, "ReadSpeedup", cdrom_read_speedup);
  si.SetUIntValue(CDROM_SECTION_NAME, "SeekSpeedup", cdrom_seek_speedup);
  if (!for_copy)
  {
    si.SetStringValue(CDROM_SECTION_NAME, "MechaconVersion", GetCDROMMechVersionName(cdrom_mechacon_version));
    si.SetIntValue(CDROM_SECTION_NAME, "ReadaheadSectors", cdrom_readahead_sectors);
    si.SetBoolValue(CDROM_SECTION_NAME, "RegionCheck", cdrom_region_check);
    si.SetBoolValue(CDROM_SECTION_NAME, "SubQSkew", cdrom_subq_skew);
    si.SetBoolValue(CDROM_SECTION_NAME, "DisableSpeedupOnMDEC", mdec_disable_cdrom_speedup);
    si.SetUIntValue(CDROM_SECTION_NAME, "MaxSeekSpeedupCycles", cdrom_max_seek_speedup_cycles);
    si.SetUIntValue(CDROM_SECTION_NAME, "MaxReadSpeedupCycles", cdrom_max_read_speedup_cycles);
  }

  si.SetStringValue(AUDIO_SECTION_NAME, "Backend", AudioStream::GetBackendName(audio_backend));
  si.SetStringValue(AUDIO_SECTION_NAME, "Driver", audio_driver.c_str());
  si.SetStringValue(AUDIO_SECTION_NAME, "OutputDevice", audio_output_device.c_str());
  audio_stream_parameters.Save(si, AUDIO_SECTION_NAME);
  si.SetUIntValue(AUDIO_SECTION_NAME, "OutputVolume", audio_output_volume);
  si.SetUIntValue(AUDIO_SECTION_NAME, "FastForwardVolume", audio_fast_forward_volume);
  si.SetBoolValue(AUDIO_SECTION_NAME, "OutputMuted", audio_output_muted);

  si.SetBoolValue(BIOS_SECTION_NAME, "TTYLogging", bios_tty_logging);
  si.SetBoolValue(BIOS_SECTION_NAME, "PatchFastBoot", bios_patch_fast_boot);
  si.SetBoolValue(BIOS_SECTION_NAME, "FastForwardBoot", bios_fast_forward_boot);

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (!for_copy)
    {
      si.SetStringValue(Controller::GetSettingsSection(i).c_str(), "Type",
                        Controller::GetControllerInfo(controller_types[i]).name);
    }

    skey.format("Card{}Type", i + 1);
    si.SetStringValue(MEMORY_CARDS_SECTION_NAME, skey, GetMemoryCardTypeName(memory_card_types[i]));

    skey.format("Card{}Path", i + 1);
    if (!memory_card_paths[i].empty())
      si.SetStringValue(MEMORY_CARDS_SECTION_NAME, skey, memory_card_paths[i].c_str());
    else
      si.DeleteValue(MEMORY_CARDS_SECTION_NAME, skey);
  }

  si.SetBoolValue(MEMORY_CARDS_SECTION_NAME, "UsePlaylistTitle", memory_card_use_playlist_title);
  si.SetBoolValue(MEMORY_CARDS_SECTION_NAME, "FastForwardAccess", memory_card_fast_forward_access);

  if (!for_copy)
    si.SetStringValue(CONTROLLER_PORTS_SECTION_NAME, "MultitapMode", GetMultitapModeName(multitap_mode));

  si.SetBoolValue(ACHIEVEMENTS_SECTION_NAME, "Enabled", achievements_enabled);
  si.SetBoolValue(ACHIEVEMENTS_SECTION_NAME, "ChallengeMode", achievements_hardcore_mode);
  si.SetBoolValue(ACHIEVEMENTS_SECTION_NAME, "EncoreMode", achievements_encore_mode);
  si.SetBoolValue(ACHIEVEMENTS_SECTION_NAME, "SpectatorMode", achievements_spectator_mode);
  si.SetBoolValue(ACHIEVEMENTS_SECTION_NAME, "UnofficialTestMode", achievements_track_unofficial);
  si.SetBoolValue(ACHIEVEMENTS_SECTION_NAME, "UseRAIntegration", achievements_use_raintegration);
  si.SetBoolValue(ACHIEVEMENTS_SECTION_NAME, "Notifications", achievements_notifications);
  si.SetBoolValue(ACHIEVEMENTS_SECTION_NAME, "LeaderboardNotifications", achievements_leaderboard_notifications);
  si.SetBoolValue(ACHIEVEMENTS_SECTION_NAME, "LeaderboardTrackers", achievements_leaderboard_trackers);
  si.SetBoolValue(ACHIEVEMENTS_SECTION_NAME, "SoundEffects", achievements_sound_effects);
  si.SetBoolValue(ACHIEVEMENTS_SECTION_NAME, "PrefetchBadges", achievements_prefetch_badges);
  si.SetBoolValue(ACHIEVEMENTS_SECTION_NAME, "RichPresenceMonitor", achievements_rich_presence_monitor);
  si.SetStringValue(ACHIEVEMENTS_SECTION_NAME, "NotificationLocation",
                    GetNotificationLocationName(achievements_notification_location));
  si.SetStringValue(ACHIEVEMENTS_SECTION_NAME, "IndicatorLocation",
                    GetNotificationLocationName(achievements_indicator_location));
  si.SetStringValue(ACHIEVEMENTS_SECTION_NAME, "ChallengeIndicatorMode",
                    GetAchievementChallengeIndicatorModeName(achievements_challenge_indicator_mode));
  si.SetStringValue(ACHIEVEMENTS_SECTION_NAME, "ProgressIndicatorMode",
                    GetAchievementProgressIndicatorModeName(achievements_progress_indicator_mode));
  si.SetUIntValue(ACHIEVEMENTS_SECTION_NAME, "NotificationsDuration", achievements_notification_duration);
  si.SetUIntValue(ACHIEVEMENTS_SECTION_NAME, "LeaderboardsDuration", achievements_leaderboard_duration);
  si.SetIntValue(ACHIEVEMENTS_SECTION_NAME, "NotificationScale", achievements_notification_scale);
  si.SetIntValue(ACHIEVEMENTS_SECTION_NAME, "IndicatorScale", achievements_indicator_scale);

  if (!for_copy)
  {
    si.SetIntValue(HACKS_SECTION_NAME, "DMAMaxSliceTicks", dma_max_slice_ticks);
    si.SetIntValue(HACKS_SECTION_NAME, "DMAHaltTicks", dma_halt_ticks);
    si.SetIntValue(HACKS_SECTION_NAME, "GPUFIFOSize", gpu_fifo_size);
    si.SetIntValue(HACKS_SECTION_NAME, "GPUMaxRunAhead", gpu_max_run_ahead);
    si.SetBoolValue(HACKS_SECTION_NAME, "UseOldMDECRoutines", mdec_use_old_routines);
    si.SetBoolValue(HACKS_SECTION_NAME, "ExportSharedMemory", export_shared_memory);

    si.SetBoolValue(DEBUG_SECTION_NAME, "PCSXExpansionRegion", pcsx_expansion_region_enable);
    si.SetBoolValue(DEBUG_SECTION_NAME, "ShowVRAM", gpu_show_vram);
    si.SetBoolValue(DEBUG_SECTION_NAME, "DumpCPUToVRAMCopies", gpu_dump_cpu_to_vram_copies);
    si.SetBoolValue(DEBUG_SECTION_NAME, "DumpVRAMToCPUCopies", gpu_dump_vram_to_cpu_copies);

    si.SetBoolValue(DEBUG_SECTION_NAME, "EnableGDBServer", enable_gdb_server);
    si.SetUIntValue(DEBUG_SECTION_NAME, "GDBServerPort", gdb_server_port);

    si.SetBoolValue(SIO_SECTION_NAME, "RedirectToTTY", sio_redirect_to_tty);

    si.SetBoolValue(PCDRV_SECTION_NAME, "Enabled", pcdrv_enable);
    si.SetBoolValue(PCDRV_SECTION_NAME, "EnableWrites", pcdrv_enable_writes);
    si.SetStringValue(PCDRV_SECTION_NAME, "Root", pcdrv_root.c_str());

    ImGuiManager::SaveDebugWindowVisibility(si, debug_window_visibility);
  }

  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "EnableTextureReplacements",
                  texture_replacements.enable_texture_replacements);
  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "EnableVRAMWriteReplacements",
                  texture_replacements.enable_vram_write_replacements);
  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "AlwaysTrackUploads", texture_replacements.always_track_uploads);
  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "PreloadTextures", texture_replacements.preload_textures);
  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpVRAMWrites", texture_replacements.dump_vram_writes);
  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpTextures", texture_replacements.dump_textures);
  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpReplacedTextures",
                  texture_replacements.dump_replaced_textures);

  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpTexturePages",
                  texture_replacements.config.dump_texture_pages);
  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpFullTexturePages",
                  texture_replacements.config.dump_full_texture_pages);
  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpTextureForceAlphaChannel",
                  texture_replacements.config.dump_texture_force_alpha_channel);

  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpVRAMWriteForceAlphaChannel",
                  texture_replacements.config.dump_vram_write_force_alpha_channel);
  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpC16Textures", texture_replacements.config.dump_c16_textures);
  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "ReducePaletteRange",
                  texture_replacements.config.reduce_palette_range);
  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "ConvertCopiesToWrites",
                  texture_replacements.config.convert_copies_to_writes);
  si.SetBoolValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "ReplacementScaleLinearFilter",
                  texture_replacements.config.replacement_scale_linear_filter);

  si.SetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "MaxHashCacheEntries",
                  texture_replacements.config.max_hash_cache_entries);
  si.SetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "MaxHashCacheVRAMUsageMB",
                  texture_replacements.config.max_hash_cache_vram_usage_mb);
  si.SetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "MaxReplacementCacheVRAMUsage",
                  texture_replacements.config.max_replacement_cache_vram_usage_mb);

  si.SetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "MaxVRAMWriteSplits",
                  texture_replacements.config.max_vram_write_splits);
  si.SetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "MaxVRAMWriteCoalesceWidth",
                  texture_replacements.config.max_vram_write_coalesce_width);
  si.SetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "MaxVRAMWriteCoalesceHeight",
                  texture_replacements.config.max_vram_write_coalesce_height);

  si.SetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpTextureWidthThreshold",
                  texture_replacements.config.texture_dump_width_threshold);
  si.SetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpTextureHeightThreshold",
                  texture_replacements.config.texture_dump_height_threshold);
  si.SetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpVRAMWriteWidthThreshold",
                  texture_replacements.config.vram_write_dump_width_threshold);
  si.SetUIntValue(TEXTURE_REPLACEMENTS_SECTION_NAME, "DumpVRAMWriteHeightThreshold",
                  texture_replacements.config.vram_write_dump_height_threshold);

  si.SetStringValue(PIO_SECTION_NAME, "DeviceType", GetPIODeviceTypeModeName(pio_device_type));
  si.SetStringValue(PIO_SECTION_NAME, "FlashImagePath", pio_flash_image_path.c_str());
  si.SetBoolValue(PIO_SECTION_NAME, "FlashImageWriteEnable", pio_flash_write_enable);
  si.SetBoolValue(PIO_SECTION_NAME, "SwitchActive", pio_switch_active);
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
    gpu_disable_upscaled_direct_textures = false;
    gpu_filter_framebuffer_uploads = false;
    gpu_texture_filter = GPUTextureFilter::Nearest;
    gpu_sprite_texture_filter = GPUTextureFilter::Nearest;
    gpu_dithering_mode = GPUDitheringMode::Unscaled;
    gpu_line_detect_mode = GPULineDetectMode::Disabled;
    gpu_downsample_mode = GPUDownsampleMode::Disabled;
    gpu_wireframe_mode = GPUWireframeMode::Disabled;
    gpu_force_video_timing = ForceVideoTimingMode::Disabled;
    gpu_disable_textures = false;
    gpu_disable_vertex_lighting = false;
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
    pcsx_expansion_region_enable = false;
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
    region = ConsoleRegion::Auto;
    gpu_force_video_timing = ForceVideoTimingMode::Disabled;
    rewind_enable = false;
    if (cpu_overclock_enable && GetCPUOverclockPercent() < 100)
    {
      cpu_overclock_enable = false;
      UpdateOverclockActive();
    }

    enable_gdb_server = false;

    gpu_show_vram = false;
    gpu_dump_cpu_to_vram_copies = false;
    gpu_dump_vram_to_cpu_copies = false;

    debug_window_visibility = 0;
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
  if (debug_window_visibility != 0 && gpu_use_thread && gpu_max_queued_frames > 0)
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
  si.SetStringValue(LOGGING_SECTION_NAME, "LogLevel", GetLogLevelName(Log::DEFAULT_LOG_LEVEL));
  si.SetBoolValue(LOGGING_SECTION_NAME, "LogTimestamps", true);

#ifndef _WIN32
  // On Linux, default the console to whether standard input is currently available.
  si.SetBoolValue(LOGGING_SECTION_NAME, "LogToConsole", Log::IsConsoleOutputCurrentlyAvailable());
#else
  si.SetBoolValue(LOGGING_SECTION_NAME, "LogToConsole", false);
#endif

  si.SetBoolValue(LOGGING_SECTION_NAME, "LogToDebug", false);
  si.SetBoolValue(LOGGING_SECTION_NAME, "LogToWindow", false);
  si.SetBoolValue(LOGGING_SECTION_NAME, "LogToFile", false);
  si.SetBoolValue(LOGGING_SECTION_NAME, "LogFileTimestamps", false);

  for (const char* channel_name : Log::GetChannelNames())
    si.SetBoolValue(LOGGING_SECTION_NAME, channel_name, true);
}

void Settings::UpdateLogConfig(const SettingsInterface& si)
{
  const Log::Level log_level =
    ParseLogLevelName(si.GetStringViewValue(LOGGING_SECTION_NAME, "LogLevel", GetLogLevelName(Log::DEFAULT_LOG_LEVEL)))
      .value_or(Log::DEFAULT_LOG_LEVEL);
  const bool log_timestamps = si.GetBoolValue(LOGGING_SECTION_NAME, "LogTimestamps", true);
  const bool log_to_console = si.GetBoolValue(LOGGING_SECTION_NAME, "LogToConsole", false);
  const bool log_to_debug = si.GetBoolValue(LOGGING_SECTION_NAME, "LogToDebug", false);
  const bool log_to_file = si.GetBoolValue(LOGGING_SECTION_NAME, "LogToFile", false);
  const bool log_file_timestamps = si.GetBoolValue(LOGGING_SECTION_NAME, "LogFileTimestamps", false);

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
    Log::SetLogChannelEnabled(static_cast<Log::Channel>(i),
                              si.GetBoolValue(LOGGING_SECTION_NAME, channel_names[i], true));
}

void Settings::SetDefaultControllerConfig(SettingsInterface& si)
{
  // Global Settings
  si.SetStringValue(CONTROLLER_PORTS_SECTION_NAME, "MultitapMode",
                    GetMultitapModeName(Settings::DEFAULT_MULTITAP_MODE));
  si.SetFloatValue(CONTROLLER_PORTS_SECTION_NAME, "PointerXScale", 8.0f);
  si.SetFloatValue(CONTROLLER_PORTS_SECTION_NAME, "PointerYScale", 8.0f);
  si.SetBoolValue(CONTROLLER_PORTS_SECTION_NAME, "PointerXInvert", false);
  si.SetBoolValue(CONTROLLER_PORTS_SECTION_NAME, "PointerYInvert", false);

  // Default pad types and parameters.
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    const std::string section(Controller::GetSettingsSection(i));
    si.ClearSection(section.c_str());
    si.SetStringValue(section.c_str(), "Type", Controller::GetControllerInfo(GetDefaultControllerType(i)).name);
  }

  // Use the automapper to set this up.
  if (GenericInputBindingMapping mapping; InputManager::GetGenericBindingMapping("Keyboard", &mapping, nullptr))
    InputManager::MapController(si, 0, mapping, true);
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

std::optional<Log::Level> Settings::ParseLogLevelName(std::string_view str)
{
  int index = 0;
  for (const char* name : s_log_level_names)
  {
    if (str == name)
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

std::optional<ConsoleRegion> Settings::ParseConsoleRegionName(std::string_view str)
{
  int index = 0;
  for (const char* name : s_console_region_names)
  {
    if (str == name)
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
  TRANSLATE_DISAMBIG_NOOP("Settings", "NTSC-J", "DiscRegion"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "NTSC-U/C", "DiscRegion"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "PAL", "DiscRegion"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Other", "DiscRegion"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Non-PS1", "DiscRegion"),
};

std::optional<DiscRegion> Settings::ParseDiscRegionName(std::string_view str)
{
  int index = 0;
  for (const char* name : s_disc_region_names)
  {
    if (str == name)
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

std::optional<CPUExecutionMode> Settings::ParseCPUExecutionMode(std::string_view str)
{
  u8 index = 0;
  for (const char* name : s_cpu_execution_mode_names)
  {
    if (str == name)
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
  TRANSLATE_DISAMBIG_NOOP("Settings", "MMap (Fastest)", "CPUFastmemMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "LUT (Faster)", "CPUFastmemMode"),
};

std::optional<CPUFastmemMode> Settings::ParseCPUFastmemMode(std::string_view str)
{
  u8 index = 0;
  for (const char* name : s_cpu_fastmem_mode_names)
  {
    if (str == name)
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

std::optional<GPURenderer> Settings::ParseRendererName(std::string_view str)
{
  int index = 0;
  for (const char* name : s_gpu_renderer_names)
  {
    if (str == name)
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
  "Nearest",
  "Bilinear",
  "BilinearBinAlpha",
  "JINC2",
  "JINC2BinAlpha",
  "MonotonicCubic",
  "MonotonicCubicBinAlpha",
  "AdaptiveDiagonal",
  "AdaptiveDiagonalBinAlpha",
  "DCCI",
  "DCCIBinAlpha",
  "xBR",
  "xBRBinAlpha",
  "SharpBilinear",
  "Scale2x",
  "Scale3x",
  "MMPX",
  "MMPXEnhanced",
  "MMPXAdvanced",
};
static constexpr const std::array s_texture_filter_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Nearest-Neighbor", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Bilinear (No Edge Blending)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "JINC2 (Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "JINC2 (Slow, No Edge Blending)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Monotonic Cubic (Very Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Monotonic Cubic (Very Slow, No Edge Blending)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Adaptive Diagonal (Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Adaptive Diagonal (Slow, No Edge Blending)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "DCCI (Extremely Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "DCCI (Extremely Slow, No Edge Blending)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "xBR (Very Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "xBR (Very Slow, No Edge Blending)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Sharp Bilinear", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Scale2x (EPX)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Scale3x (Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "MMPX (Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "MMPX Enhanced (Slow)", "GPUTextureFilter"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "MMPX Advanced (Very Slow)", "GPUTextureFilter"),
};
static_assert(s_texture_filter_names.size() == static_cast<size_t>(GPUTextureFilter::Count));
static_assert(s_texture_filter_display_names.size() == static_cast<size_t>(GPUTextureFilter::Count));

std::optional<GPUTextureFilter> Settings::ParseTextureFilterName(std::string_view str)
{
  int index = 0;
  for (const char* name : s_texture_filter_names)
  {
    if (str == name)
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

std::optional<GPUDitheringMode> Settings::ParseGPUDitheringModeName(std::string_view str)
{
  int index = 0;
  for (const char* name : s_gpu_dithering_mode_names)
  {
    if (str == name)
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

std::optional<GPULineDetectMode> Settings::ParseLineDetectModeName(std::string_view str)
{
  int index = 0;
  for (const char* name : s_line_detect_mode_names)
  {
    if (str == name)
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

std::optional<GPUDownsampleMode> Settings::ParseDownsampleModeName(std::string_view str)
{
  int index = 0;
  for (const char* name : s_downsample_mode_names)
  {
    if (str == name)
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

std::optional<GPUWireframeMode> Settings::ParseGPUWireframeMode(std::string_view str)
{
  int index = 0;
  for (const char* name : s_wireframe_mode_names)
  {
    if (str == name)
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

std::optional<GPUDumpCompressionMode> Settings::ParseGPUDumpCompressionMode(std::string_view str)
{
  int index = 0;
  for (const char* name : s_gpu_dump_compression_mode_names)
  {
    if (str == name)
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

std::optional<DisplayDeinterlacingMode> Settings::ParseDisplayDeinterlacingMode(std::string_view str)
{
  int index = 0;
  for (const char* name : s_display_deinterlacing_mode_names)
  {
    if (str == name)
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

std::optional<DisplayCropMode> Settings::ParseDisplayCropMode(std::string_view str)
{
  int index = 0;
  for (const char* name : s_display_crop_mode_names)
  {
    if (str == name)
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

std::optional<DisplayFineCropMode> Settings::ParseDisplayFineCropMode(std::string_view str)
{
  int index = 0;
  for (const char* name : s_display_fine_crop_mode_names)
  {
    if (str == name)
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
  TRANSLATE_DISAMBIG_NOOP("Settings", "Stretch To Fill", "DisplayAspectRatio");
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

std::optional<DisplayAlignment> Settings::ParseDisplayAlignment(std::string_view str)
{
  int index = 0;
  for (const char* name : s_display_alignment_names)
  {
    if (str == name)
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

std::optional<DisplayRotation> Settings::ParseDisplayRotation(std::string_view str)
{
  int index = 0;
  for (const char* name : s_display_rotation_names)
  {
    if (str == name)
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
  TRANSLATE_DISAMBIG_NOOP("Settings", "NTSC (60 Hz)", "ForceVideoTiming"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "PAL (50 Hz)", "ForceVideoTiming"),
};

std::optional<ForceVideoTimingMode> Settings::ParseForceVideoTimingName(std::string_view str)
{
  int index = 0;
  for (const char* name : s_display_force_video_timing_names)
  {
    if (str == name)
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

std::optional<DisplayScalingMode> Settings::ParseDisplayScaling(std::string_view str)
{
  int index = 0;
  for (const char* name : s_display_scaling_names)
  {
    if (str == name)
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

std::optional<DisplayExclusiveFullscreenControl> Settings::ParseDisplayExclusiveFullscreenControl(std::string_view str)
{
  int index = 0;
  for (const char* name : s_display_exclusive_fullscreen_mode_names)
  {
    if (str == name)
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

std::optional<DisplayScreenshotMode> Settings::ParseDisplayScreenshotMode(std::string_view str)
{
  int index = 0;
  for (const char* name : s_display_screenshot_mode_names)
  {
    if (str == name)
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

std::optional<DisplayScreenshotFormat> Settings::ParseDisplayScreenshotFormat(std::string_view str)
{
  int index = 0;
  for (const char* name : s_display_screenshot_format_names)
  {
    if (str == name)
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

static constexpr const std::array s_capture_file_name_format_names = {
  "Timestamp",
  "TitleAndTimestamp",
  "TimestampInFolder",
  "TitleAndTimestampInFolder",
};
static_assert(s_capture_file_name_format_names.size() == static_cast<size_t>(CaptureFileNameFormat::Count));
static constexpr const std::array s_capture_file_name_format_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Timestamp", "CaptureFileNameFormat"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Game and Timestamp", "CaptureFileNameFormat"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Timestamp in Game Folder", "CaptureFileNameFormat"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Game and Timestamp in Game Folder", "CaptureFileNameFormat"),
};
static_assert(s_capture_file_name_format_names.size() == static_cast<size_t>(CaptureFileNameFormat::Count));

std::optional<CaptureFileNameFormat> Settings::ParseCaptureFileNameFormat(std::string_view str)
{
  int index = 0;
  for (const char* name : s_capture_file_name_format_names)
  {
    if (str == name)
      return static_cast<CaptureFileNameFormat>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetCaptureFileNameFormatName(CaptureFileNameFormat format)
{
  return s_capture_file_name_format_names[static_cast<size_t>(format)];
}

const char* Settings::GetCaptureFileNameFormatDisplayName(CaptureFileNameFormat format)
{
  return Host::TranslateToCString("Settings", s_capture_file_name_format_display_names[static_cast<size_t>(format)],
                                  "CaptureFileNameFormat");
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

std::optional<NotificationLocation> Settings::ParseNotificationLocation(std::string_view str)
{
  int index = 0;
  for (const char* name : s_notification_location_names)
  {
    if (str == name)
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

std::optional<AchievementChallengeIndicatorMode> Settings::ParseAchievementChallengeIndicatorMode(std::string_view str)
{
  int index = 0;
  for (const char* name : s_achievement_challenge_indicator_mode_names)
  {
    if (str == name)
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

static constexpr const std::array s_achievement_progress_indicator_mode_names = {
  "Disabled",
  "Icon",
  "IconAndTitle",
};
static_assert(s_achievement_progress_indicator_mode_names.size() ==
              static_cast<size_t>(AchievementProgressIndicatorMode::MaxCount));
static constexpr const std::array s_achievement_progress_indicator_mode_display_names = {
  TRANSLATE_DISAMBIG_NOOP("Settings", "Disabled", "AchievementProgressIndicatorMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Show Icon", "AchievementProgressIndicatorMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Show Icon and Title", "AchievementProgressIndicatorMode"),
};
static_assert(s_achievement_progress_indicator_mode_display_names.size() ==
              static_cast<size_t>(AchievementProgressIndicatorMode::MaxCount));

std::optional<AchievementProgressIndicatorMode> Settings::ParseAchievementProgressIndicatorMode(std::string_view str)
{
  int index = 0;
  for (const char* name : s_achievement_progress_indicator_mode_names)
  {
    if (str == name)
      return static_cast<AchievementProgressIndicatorMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetAchievementProgressIndicatorModeName(AchievementProgressIndicatorMode mode)
{
  return s_achievement_progress_indicator_mode_names[static_cast<size_t>(mode)];
}

const char* Settings::GetAchievementProgressIndicatorModeDisplayName(AchievementProgressIndicatorMode mode)
{
  return Host::TranslateToCString("Settings",
                                  s_achievement_progress_indicator_mode_display_names[static_cast<size_t>(mode)],
                                  "AchievementProgressIndicatorMode");
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

std::optional<MemoryCardType> Settings::ParseMemoryCardTypeName(std::string_view str)
{
  int index = 0;
  for (const char* name : s_memory_card_type_names)
  {
    if (str == name)
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

const char* Settings::GetDefaultSharedMemoryCardName(u32 slot)
{
  static constexpr std::array<const char*, NUM_CONTROLLER_AND_CARD_PORTS> default_names = {{
    "shared_card_1.mcd",
    "shared_card_2.mcd",
    "shared_card_3.mcd",
    "shared_card_4.mcd",
    "shared_card_5.mcd",
    "shared_card_6.mcd",
    "shared_card_7.mcd",
    "shared_card_8.mcd",
  }};
  return default_names[std::min(slot, NUM_CONTROLLER_AND_CARD_PORTS - 1)];
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

std::optional<MultitapMode> Settings::ParseMultitapModeName(std::string_view str)
{
  u32 index = 0;
  for (const char* name : s_multitap_enable_mode_names)
  {
    if (str == name)
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

std::optional<CDROMMechaconVersion> Settings::ParseCDROMMechVersionName(std::string_view str)
{
  u32 index = 0;
  for (const char* name : s_mechacon_version_names)
  {
    if (str == name)
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

std::optional<SaveStateCompressionMode> Settings::ParseSaveStateCompressionModeName(std::string_view str)
{
  u32 index = 0;
  for (const char* name : s_save_state_compression_mode_names)
  {
    if (str == name)
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

std::optional<PIODeviceType> Settings::ParsePIODeviceTypeName(std::string_view str)
{
  u32 index = 0;
  for (const char* name : s_pio_device_type_names)
  {
    if (str == name)
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

static void EnsureFolderExists(const std::string& path);
static std::string LoadPathFromSettings(const SettingsInterface& si, const std::string& root, const char* section,
                                        const char* name, const char* def);

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
std::string Overlays;
std::string Patches;
std::string Resources;
std::string SaveStates;
std::string Screenshots;
std::string Shaders;
std::string Subchannels;
std::string Textures;
std::string UserResources;
std::string Videos;

} // namespace EmuFolders

std::string EmuFolders::GetDefaultPath(const std::string* ref_folder)
{
  // clang-format off
  std::string_view subdir;
  if (ref_folder == &Bios) subdir = "bios";
  else if (ref_folder == &Cache) subdir = "cache";
  else if (ref_folder == &Cheats) subdir = "cheats";
  else if (ref_folder == &Covers) subdir = "covers";
  else if (ref_folder == &GameIcons) subdir = "gameicons";
  else if (ref_folder == &GameSettings) subdir = "gamesettings";
  else if (ref_folder == &InputProfiles) subdir = "inputprofiles";
  else if (ref_folder == &MemoryCards) subdir = "memcards";
  else if (ref_folder == &Overlays) subdir = "resources" FS_OSPATH_SEPARATOR_STR "overlays";
  else if (ref_folder == &Patches) subdir = "patches";
  else if (ref_folder == &SaveStates) subdir = "savestates";
  else if (ref_folder == &Screenshots) subdir = "screenshots";
  else if (ref_folder == &Shaders) subdir = "shaders";
  else if (ref_folder == &Subchannels) subdir = "subchannels";
  else if (ref_folder == &Textures) subdir = "textures";
  else if (ref_folder == &UserResources) subdir = "resources";
  else if (ref_folder == &Videos) subdir = "videos";
  // clang-format on

  return Path::Combine(DataRoot, subdir);
}

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
  Overlays = Path::Combine(DataRoot, "resources" FS_OSPATH_SEPARATOR_STR "overlays");
  Patches = Path::Combine(DataRoot, "patches");
  SaveStates = Path::Combine(DataRoot, "savestates");
  Screenshots = Path::Combine(DataRoot, "screenshots");
  Shaders = Path::Combine(DataRoot, "shaders");
  Subchannels = Path::Combine(DataRoot, "subchannels");
  Textures = Path::Combine(DataRoot, "textures");
  UserResources = Path::Combine(DataRoot, "resources");
  Videos = Path::Combine(DataRoot, "videos");
}

std::string EmuFolders::LoadPathFromSettings(const SettingsInterface& si, const std::string& root, const char* section,
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

void EmuFolders::LoadConfig(const SettingsInterface& si)
{
  Bios = LoadPathFromSettings(si, DataRoot, Settings::BIOS_SECTION_NAME, "SearchDirectory", "bios");
  Cache = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "Cache", "cache");
  Cheats = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "Cheats", "cheats");
  Covers = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "Covers", "covers");
  GameIcons = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "GameIcons", "gameicons");
  GameSettings = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "GameSettings", "gamesettings");
  InputProfiles = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "InputProfiles", "inputprofiles");
  MemoryCards = LoadPathFromSettings(si, DataRoot, Settings::MEMORY_CARDS_SECTION_NAME, "Directory", "memcards");
  Overlays = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "Overlays",
                                  "resources" FS_OSPATH_SEPARATOR_STR "overlays");
  Patches = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "Patches", "patches");
  SaveStates = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "SaveStates", "savestates");
  Screenshots = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "Screenshots", "screenshots");
  Shaders = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "Shaders", "shaders");
  Subchannels = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "Subchannels", "subchannels");
  Textures = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "Textures", "textures");
  UserResources = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "UserResources", "resources");
  Videos = LoadPathFromSettings(si, DataRoot, Settings::FOLDERS_SECTION_NAME, "Videos", "videos");

  DEV_LOG("BIOS Directory: {}", Bios);
  DEV_LOG("Cache Directory: {}", Cache);
  DEV_LOG("Cheats Directory: {}", Cheats);
  DEV_LOG("Covers Directory: {}", Covers);
  DEV_LOG("Game Icons Directory: {}", GameIcons);
  DEV_LOG("Game Settings Directory: {}", GameSettings);
  DEV_LOG("Input Profile Directory: {}", InputProfiles);
  DEV_LOG("MemoryCards Directory: {}", MemoryCards);
  DEV_LOG("Overlays Directory: {}", Overlays);
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
  si.SetStringValue(Settings::BIOS_SECTION_NAME, "SearchDirectory", Path::MakeRelative(Bios, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "Cache", Path::MakeRelative(Cache, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "Cheats", Path::MakeRelative(Cheats, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "Covers", Path::MakeRelative(Covers, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "GameIcons", Path::MakeRelative(GameIcons, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "GameSettings", Path::MakeRelative(GameSettings, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "InputProfiles",
                    Path::MakeRelative(InputProfiles, DataRoot).c_str());
  si.SetStringValue(Settings::MEMORY_CARDS_SECTION_NAME, "Directory",
                    Path::MakeRelative(MemoryCards, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "Overlays", Path::MakeRelative(Overlays, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "Patches", Path::MakeRelative(Patches, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "SaveStates", Path::MakeRelative(SaveStates, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "Screenshots", Path::MakeRelative(Screenshots, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "Shaders", Path::MakeRelative(Shaders, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "Subchannels", Path::MakeRelative(Subchannels, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "Textures", Path::MakeRelative(Textures, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "UserResources",
                    Path::MakeRelative(UserResources, DataRoot).c_str());
  si.SetStringValue(Settings::FOLDERS_SECTION_NAME, "Videos", Path::MakeRelative(Videos, DataRoot).c_str());
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
  EnsureFolderExists(Overlays); // Must come after UserResources because it is under it.
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
