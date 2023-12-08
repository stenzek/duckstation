// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include "util/audio_stream.h"

#include "common/log.h"
#include "common/settings_interface.h"
#include "common/small_string.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class RenderAPI : u8;
enum class MediaCaptureBackend : u8;

struct SettingInfo
{
  enum class Type
  {
    Boolean,
    Integer,
    IntegerList,
    Float,
    String,
    Path,
  };

  Type type;
  const char* name;
  const char* display_name;
  const char* description;
  const char* default_value;
  const char* min_value;
  const char* max_value;
  const char* step_value;
  const char* format;
  const char** options;
  float multiplier;

  const char* StringDefaultValue() const;
  bool BooleanDefaultValue() const;
  s32 IntegerDefaultValue() const;
  s32 IntegerMinValue() const;
  s32 IntegerMaxValue() const;
  s32 IntegerStepValue() const;
  float FloatDefaultValue() const;
  float FloatMinValue() const;
  float FloatMaxValue() const;
  float FloatStepValue() const;
};

struct Settings
{
  Settings();

  ConsoleRegion region = DEFAULT_CONSOLE_REGION;

  CPUExecutionMode cpu_execution_mode = DEFAULT_CPU_EXECUTION_MODE;
  CPUFastmemMode cpu_fastmem_mode = DEFAULT_CPU_FASTMEM_MODE;
  bool cpu_overclock_enable : 1 = false;
  bool cpu_overclock_active : 1 = false;
  bool cpu_recompiler_memory_exceptions : 1 = false;
  bool cpu_recompiler_block_linking : 1 = true;
  bool cpu_recompiler_icache : 1 = false;
  u32 cpu_overclock_numerator = 1;
  u32 cpu_overclock_denominator = 1;

  float emulation_speed = 1.0f;
  float fast_forward_speed = 0.0f;
  float turbo_speed = 0.0f;
  bool sync_to_host_refresh_rate : 1 = false;
  bool inhibit_screensaver : 1 = true;
  bool pause_on_focus_loss : 1 = false;
  bool pause_on_controller_disconnection : 1 = false;
  bool save_state_on_exit : 1 = true;
  bool create_save_state_backups : 1 = DEFAULT_SAVE_STATE_BACKUPS;
  bool confim_power_off : 1 = true;
  bool load_devices_from_save_states : 1 = false;
  bool apply_compatibility_settings : 1 = true;
  bool apply_game_settings : 1 = true;
  bool disable_all_enhancements : 1 = false;
  bool enable_discord_presence : 1 = false;

  bool rewind_enable : 1 = false;
  float rewind_save_frequency = 10.0f;
  u16 rewind_save_slots = 10;
  u8 runahead_frames = 0;

  GPURenderer gpu_renderer = DEFAULT_GPU_RENDERER;
  std::string gpu_adapter;
  u8 gpu_resolution_scale = 1;
  u8 gpu_multisamples = 1;
  u8 gpu_max_queued_frames = 2;
  bool gpu_use_thread : 1 = true;
  bool gpu_use_software_renderer_for_readbacks : 1 = false;
  bool gpu_use_debug_device : 1 = false;
  bool gpu_disable_shader_cache : 1 = false;
  bool gpu_disable_dual_source_blend : 1 = false;
  bool gpu_disable_framebuffer_fetch : 1 = false;
  bool gpu_disable_texture_buffers : 1 = false;
  bool gpu_disable_texture_copy_to_self : 1 = false;
  bool gpu_disable_memory_import : 1 = false;
  bool gpu_disable_raster_order_views : 1 = false;
  bool gpu_disable_compute_shaders : 1 = false;
  bool gpu_disable_compressed_textures : 1 = false;
  bool gpu_per_sample_shading : 1 = false;
  bool gpu_true_color : 1 = true;
  bool gpu_scaled_dithering : 1 = true;
  bool gpu_force_round_texcoords : 1 = false;
  bool gpu_accurate_blending : 1 = false;
  bool gpu_widescreen_hack : 1 = false;
  bool gpu_texture_cache : 1 = false;
  bool gpu_pgxp_enable : 1 = false;
  bool gpu_pgxp_culling : 1 = true;
  bool gpu_pgxp_texture_correction : 1 = true;
  bool gpu_pgxp_color_correction : 1 = false;
  bool gpu_pgxp_vertex_cache : 1 = false;
  bool gpu_pgxp_cpu : 1 = false;
  bool gpu_pgxp_preserve_proj_fp : 1 = false;
  bool gpu_pgxp_depth_buffer : 1 = false;
  bool gpu_pgxp_disable_2d : 1 = false;
  ForceVideoTimingMode gpu_force_video_timing = DEFAULT_FORCE_VIDEO_TIMING_MODE;
  GPUTextureFilter gpu_texture_filter = DEFAULT_GPU_TEXTURE_FILTER;
  GPUTextureFilter gpu_sprite_texture_filter = DEFAULT_GPU_TEXTURE_FILTER;
  GPULineDetectMode gpu_line_detect_mode = DEFAULT_GPU_LINE_DETECT_MODE;
  GPUDownsampleMode gpu_downsample_mode = DEFAULT_GPU_DOWNSAMPLE_MODE;
  u8 gpu_downsample_scale = 1;
  GPUWireframeMode gpu_wireframe_mode = DEFAULT_GPU_WIREFRAME_MODE;
  DisplayDeinterlacingMode display_deinterlacing_mode = DEFAULT_DISPLAY_DEINTERLACING_MODE;
  DisplayCropMode display_crop_mode = DEFAULT_DISPLAY_CROP_MODE;
  DisplayAspectRatio display_aspect_ratio = DEFAULT_DISPLAY_ASPECT_RATIO;
  DisplayAlignment display_alignment = DEFAULT_DISPLAY_ALIGNMENT;
  DisplayRotation display_rotation = DEFAULT_DISPLAY_ROTATION;
  DisplayScalingMode display_scaling = DEFAULT_DISPLAY_SCALING;
  DisplayExclusiveFullscreenControl display_exclusive_fullscreen_control = DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL;
  DisplayScreenshotMode display_screenshot_mode = DEFAULT_DISPLAY_SCREENSHOT_MODE;
  DisplayScreenshotFormat display_screenshot_format = DEFAULT_DISPLAY_SCREENSHOT_FORMAT;
  u8 display_screenshot_quality = DEFAULT_DISPLAY_SCREENSHOT_QUALITY;
  u16 display_aspect_ratio_custom_numerator = 0;
  u16 display_aspect_ratio_custom_denominator = 0;
  s16 display_active_start_offset = 0;
  s16 display_active_end_offset = 0;
  s8 display_line_start_offset = 0;
  s8 display_line_end_offset = 0;
  bool display_optimal_frame_pacing : 1 = false;
  bool display_pre_frame_sleep : 1 = false;
  bool display_skip_presenting_duplicate_frames : 1 = false;
  bool display_vsync : 1 = false;
  bool display_disable_mailbox_presentation : 1 = true;
  bool display_force_4_3_for_24bit : 1 = false;
  bool display_24bit_chroma_smoothing : 1 = false;
  bool display_show_fps : 1 = false;
  bool display_show_speed : 1 = false;
  bool display_show_gpu_stats : 1 = false;
  bool display_show_resolution : 1 = false;
  bool display_show_latency_stats : 1 = false;
  bool display_show_cpu_usage : 1 = false;
  bool display_show_gpu_usage : 1 = false;
  bool display_show_frame_times : 1 = false;
  bool display_show_status_indicators : 1 = true;
  bool display_show_inputs : 1 = false;
  bool display_show_enhancements : 1 = false;
  bool display_stretch_vertically : 1 = false;
  bool display_auto_resize_window : 1 = false;
  float display_pre_frame_sleep_buffer = DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER;
  float display_osd_scale = DEFAULT_OSD_SCALE;
  float display_osd_margin = 0.0f;
  float gpu_pgxp_tolerance = -1.0f;
  float gpu_pgxp_depth_clear_threshold = DEFAULT_GPU_PGXP_DEPTH_THRESHOLD / GPU_PGXP_DEPTH_THRESHOLD_SCALE;

  SaveStateCompressionMode save_state_compression = DEFAULT_SAVE_STATE_COMPRESSION_MODE;

  u8 cdrom_readahead_sectors = DEFAULT_CDROM_READAHEAD_SECTORS;
  CDROMMechaconVersion cdrom_mechacon_version = DEFAULT_CDROM_MECHACON_VERSION;
  bool cdrom_region_check : 1 = false;
  bool cdrom_subq_skew : 1 = false;
  bool cdrom_load_image_to_ram : 1 = false;
  bool cdrom_load_image_patches : 1 = false;
  bool cdrom_mute_cd_audio : 1 = false;
  u32 cdrom_read_speedup = 1;
  u32 cdrom_seek_speedup = 1;

  std::string audio_driver;
  std::string audio_output_device;
  u32 audio_output_volume = 100;
  u32 audio_fast_forward_volume = 100;
  AudioStreamParameters audio_stream_parameters;
  AudioBackend audio_backend = AudioStream::DEFAULT_BACKEND;
  bool audio_output_muted : 1 = false;

  bool use_old_mdec_routines : 1 = false;
  bool pcdrv_enable : 1 = false;
  bool export_shared_memory : 1 = false;

  // timing hacks section
  TickCount dma_max_slice_ticks = DEFAULT_DMA_MAX_SLICE_TICKS;
  TickCount dma_halt_ticks = DEFAULT_DMA_HALT_TICKS;
  u32 gpu_fifo_size = DEFAULT_GPU_FIFO_SIZE;
  TickCount gpu_max_run_ahead = DEFAULT_GPU_MAX_RUN_AHEAD;

  // achievements
  bool achievements_enabled : 1 = false;
  bool achievements_hardcore_mode : 1 = false;
  bool achievements_notifications : 1 = true;
  bool achievements_leaderboard_notifications : 1 = true;
  bool achievements_sound_effects : 1 = true;
  bool achievements_overlays : 1 = true;
  bool achievements_encore_mode : 1 = false;
  bool achievements_spectator_mode : 1 = false;
  bool achievements_unofficial_test_mode : 1 = false;
  bool achievements_use_raintegration : 1 = false;
  s32 achievements_notification_duration = DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME;
  s32 achievements_leaderboard_duration = DEFAULT_LEADERBOARD_NOTIFICATION_TIME;

  struct DebugSettings
  {
#ifndef __ANDROID__
    u16 gdb_server_port = DEFAULT_GDB_SERVER_PORT;
    bool enable_gdb_server : 1 = false;
#endif

    bool show_vram : 1 = false;
    bool dump_cpu_to_vram_copies : 1 = false;
    bool dump_vram_to_cpu_copies : 1 = false;
  } debugging;

  // texture replacements
  struct TextureReplacementSettings
  {
    struct Configuration
    {
      static constexpr u32 DEFAULT_MAX_HASH_CACHE_ENTRIES = 1200;
      static constexpr u32 DEFAULT_MAX_HASH_CACHE_VRAM_USAGE_MB = 2048;
      static constexpr u32 DEFAULT_MAX_REPLACEMENT_CACHE_VRAM_USAGE_MB = 512;

      constexpr Configuration() = default;

      bool dump_texture_pages : 1 = false;
      bool dump_full_texture_pages : 1 = false;
      bool dump_texture_force_alpha_channel : 1 = false;
      bool dump_vram_write_force_alpha_channel : 1 = true;
      bool dump_c16_textures : 1 = false;
      bool reduce_palette_range : 1 = true;
      bool convert_copies_to_writes : 1 = false;
      bool replacement_scale_linear_filter = false;

      u32 max_hash_cache_entries = DEFAULT_MAX_HASH_CACHE_ENTRIES;
      u32 max_hash_cache_vram_usage_mb = DEFAULT_MAX_HASH_CACHE_VRAM_USAGE_MB;
      u32 max_replacement_cache_vram_usage_mb = DEFAULT_MAX_REPLACEMENT_CACHE_VRAM_USAGE_MB;

      u32 max_vram_write_splits = 0;
      u32 max_vram_write_coalesce_width = 0;
      u32 max_vram_write_coalesce_height = 0;
      u32 texture_dump_width_threshold = 16;
      u32 texture_dump_height_threshold = 16;

      u32 vram_write_dump_width_threshold = 128;
      u32 vram_write_dump_height_threshold = 128;

      bool operator==(const Configuration& rhs) const;
      bool operator!=(const Configuration& rhs) const;

      std::string ExportToYAML(bool comment) const;
    };

    bool enable_texture_replacements : 1 = false;
    bool enable_vram_write_replacements : 1 = false;
    bool preload_textures : 1 = false;

    bool dump_textures : 1 = false;
    bool dump_replaced_textures : 1 = true;
    bool dump_vram_writes : 1 = false;

    Configuration config;

    bool operator==(const TextureReplacementSettings& rhs) const;
    bool operator!=(const TextureReplacementSettings& rhs) const;
  } texture_replacements;

  bool bios_tty_logging : 1 = false;
  bool bios_patch_fast_boot : 1 = DEFAULT_FAST_BOOT_VALUE;
  bool bios_fast_forward_boot : 1 = false;
  bool enable_8mb_ram : 1 = false;
  bool gpu_dump_fast_replay_mode : 1 = false;

  std::array<ControllerType, NUM_CONTROLLER_AND_CARD_PORTS> controller_types{};
  std::array<MemoryCardType, NUM_CONTROLLER_AND_CARD_PORTS> memory_card_types{};
  std::array<std::string, NUM_CONTROLLER_AND_CARD_PORTS> memory_card_paths{};
  bool memory_card_use_playlist_title = true;

  MultitapMode multitap_mode = DEFAULT_MULTITAP_MODE;

  std::string pcdrv_root;
  bool pcdrv_enable_writes = false;

  ALWAYS_INLINE bool IsUsingSoftwareRenderer() const { return (gpu_renderer == GPURenderer::Software); }
  ALWAYS_INLINE bool IsUsingAccurateBlending() const { return (gpu_accurate_blending && !gpu_true_color); }
  ALWAYS_INLINE bool IsRunaheadEnabled() const { return (runahead_frames > 0); }

  ALWAYS_INLINE PGXPMode GetPGXPMode()
  {
    return gpu_pgxp_enable ? (gpu_pgxp_cpu ? PGXPMode::CPU : PGXPMode::Memory) : PGXPMode::Disabled;
  }

  ALWAYS_INLINE bool UsingPGXPDepthBuffer() const { return gpu_pgxp_enable && gpu_pgxp_depth_buffer; }
  ALWAYS_INLINE bool UsingPGXPCPUMode() const { return gpu_pgxp_enable && gpu_pgxp_cpu; }
  ALWAYS_INLINE float GetPGXPDepthClearThreshold() const
  {
    return gpu_pgxp_depth_clear_threshold * GPU_PGXP_DEPTH_THRESHOLD_SCALE;
  }
  ALWAYS_INLINE void SetPGXPDepthClearThreshold(float value)
  {
    gpu_pgxp_depth_clear_threshold = value / GPU_PGXP_DEPTH_THRESHOLD_SCALE;
  }

  ALWAYS_INLINE s32 GetAudioOutputVolume(bool fast_forwarding) const
  {
    return audio_output_muted ? 0 : (fast_forwarding ? audio_fast_forward_volume : audio_output_volume);
  }

  float GetDisplayAspectRatioValue() const;

  ALWAYS_INLINE bool IsPort1MultitapEnabled() const
  {
    return (multitap_mode == MultitapMode::Port1Only || multitap_mode == MultitapMode::BothPorts);
  }
  ALWAYS_INLINE bool IsPort2MultitapEnabled() const
  {
    return (multitap_mode == MultitapMode::Port2Only || multitap_mode == MultitapMode::BothPorts);
  }
  ALWAYS_INLINE bool IsMultitapPortEnabled(u32 port) const
  {
    return (port == 0) ? IsPort1MultitapEnabled() : IsPort2MultitapEnabled();
  }

  ALWAYS_INLINE static bool IsPerGameMemoryCardType(MemoryCardType type)
  {
    return (type == MemoryCardType::PerGame || type == MemoryCardType::PerGameTitle ||
            type == MemoryCardType::PerGameFileTitle);
  }
  bool HasAnyPerGameMemoryCards() const;

  /// Returns the default path to a memory card.
  static std::string GetDefaultSharedMemoryCardName(u32 slot);
  std::string GetSharedMemoryCardPath(u32 slot) const;

  /// Returns the default path to a memory card for a specific game.
  static std::string GetGameMemoryCardPath(std::string_view serial, u32 slot);

  static void CPUOverclockPercentToFraction(u32 percent, u32* numerator, u32* denominator);
  static u32 CPUOverclockFractionToPercent(u32 numerator, u32 denominator);

  void SetCPUOverclockPercent(u32 percent);
  u32 GetCPUOverclockPercent() const;
  void UpdateOverclockActive();

  enum : u32
  {
    DEFAULT_DMA_MAX_SLICE_TICKS = 1000,
    DEFAULT_DMA_HALT_TICKS = 100,
    DEFAULT_GPU_FIFO_SIZE = 16,
    DEFAULT_GPU_MAX_RUN_AHEAD = 128,
  };

  void Load(const SettingsInterface& si, const SettingsInterface& controller_si);
  void Save(SettingsInterface& si, bool ignore_base) const;
  static void Clear(SettingsInterface& si);

  void FixIncompatibleSettings(bool display_osd_messages);

  /// Initializes configuration.
  static void SetDefaultLogConfig(SettingsInterface& si);
  static void UpdateLogConfig(const SettingsInterface& si);

  static void SetDefaultControllerConfig(SettingsInterface& si);
  static void SetDefaultHotkeyConfig(SettingsInterface& si);

  static std::optional<Log::Level> ParseLogLevelName(const char* str);
  static const char* GetLogLevelName(Log::Level level);
  static const char* GetLogLevelDisplayName(Log::Level level);

  static std::optional<ConsoleRegion> ParseConsoleRegionName(const char* str);
  static const char* GetConsoleRegionName(ConsoleRegion region);
  static const char* GetConsoleRegionDisplayName(ConsoleRegion region);

  static std::optional<DiscRegion> ParseDiscRegionName(const char* str);
  static const char* GetDiscRegionName(DiscRegion region);
  static const char* GetDiscRegionDisplayName(DiscRegion region);

  static std::optional<CPUExecutionMode> ParseCPUExecutionMode(const char* str);
  static const char* GetCPUExecutionModeName(CPUExecutionMode mode);
  static const char* GetCPUExecutionModeDisplayName(CPUExecutionMode mode);

  static std::optional<CPUFastmemMode> ParseCPUFastmemMode(const char* str);
  static const char* GetCPUFastmemModeName(CPUFastmemMode mode);
  static const char* GetCPUFastmemModeDisplayName(CPUFastmemMode mode);

  static std::optional<GPURenderer> ParseRendererName(const char* str);
  static const char* GetRendererName(GPURenderer renderer);
  static const char* GetRendererDisplayName(GPURenderer renderer);
  static RenderAPI GetRenderAPIForRenderer(GPURenderer renderer);
  static GPURenderer GetRendererForRenderAPI(RenderAPI api);
  static GPURenderer GetAutomaticRenderer();

  static std::optional<GPUTextureFilter> ParseTextureFilterName(const char* str);
  static const char* GetTextureFilterName(GPUTextureFilter filter);
  static const char* GetTextureFilterDisplayName(GPUTextureFilter filter);

  static std::optional<GPULineDetectMode> ParseLineDetectModeName(const char* str);
  static const char* GetLineDetectModeName(GPULineDetectMode filter);
  static const char* GetLineDetectModeDisplayName(GPULineDetectMode filter);

  static std::optional<GPUDownsampleMode> ParseDownsampleModeName(const char* str);
  static const char* GetDownsampleModeName(GPUDownsampleMode mode);
  static const char* GetDownsampleModeDisplayName(GPUDownsampleMode mode);

  static std::optional<GPUWireframeMode> ParseGPUWireframeMode(const char* str);
  static const char* GetGPUWireframeModeName(GPUWireframeMode mode);
  static const char* GetGPUWireframeModeDisplayName(GPUWireframeMode mode);

  static std::optional<GPUDumpCompressionMode> ParseGPUDumpCompressionMode(const char* str);
  static const char* GetGPUDumpCompressionModeName(GPUDumpCompressionMode mode);
  static const char* GetGPUDumpCompressionModeDisplayName(GPUDumpCompressionMode mode);

  static std::optional<DisplayDeinterlacingMode> ParseDisplayDeinterlacingMode(const char* str);
  static const char* GetDisplayDeinterlacingModeName(DisplayDeinterlacingMode mode);
  static const char* GetDisplayDeinterlacingModeDisplayName(DisplayDeinterlacingMode mode);

  static std::optional<DisplayCropMode> ParseDisplayCropMode(const char* str);
  static const char* GetDisplayCropModeName(DisplayCropMode crop_mode);
  static const char* GetDisplayCropModeDisplayName(DisplayCropMode crop_mode);

  static std::optional<DisplayAspectRatio> ParseDisplayAspectRatio(const char* str);
  static const char* GetDisplayAspectRatioName(DisplayAspectRatio ar);
  static const char* GetDisplayAspectRatioDisplayName(DisplayAspectRatio ar);

  static std::optional<DisplayAlignment> ParseDisplayAlignment(const char* str);
  static const char* GetDisplayAlignmentName(DisplayAlignment alignment);
  static const char* GetDisplayAlignmentDisplayName(DisplayAlignment alignment);

  static std::optional<DisplayRotation> ParseDisplayRotation(const char* str);
  static const char* GetDisplayRotationName(DisplayRotation alignment);
  static const char* GetDisplayRotationDisplayName(DisplayRotation alignment);

  static std::optional<DisplayScalingMode> ParseDisplayScaling(const char* str);
  static const char* GetDisplayScalingName(DisplayScalingMode mode);
  static const char* GetDisplayScalingDisplayName(DisplayScalingMode mode);

  static std::optional<ForceVideoTimingMode> ParseForceVideoTimingName(const char* str);
  static const char* GetForceVideoTimingName(ForceVideoTimingMode mode);
  static const char* GetForceVideoTimingDisplayName(ForceVideoTimingMode mode);

  static std::optional<DisplayExclusiveFullscreenControl> ParseDisplayExclusiveFullscreenControl(const char* str);
  static const char* GetDisplayExclusiveFullscreenControlName(DisplayExclusiveFullscreenControl mode);
  static const char* GetDisplayExclusiveFullscreenControlDisplayName(DisplayExclusiveFullscreenControl mode);

  static std::optional<DisplayScreenshotMode> ParseDisplayScreenshotMode(const char* str);
  static const char* GetDisplayScreenshotModeName(DisplayScreenshotMode mode);
  static const char* GetDisplayScreenshotModeDisplayName(DisplayScreenshotMode mode);

  static std::optional<DisplayScreenshotFormat> ParseDisplayScreenshotFormat(const char* str);
  static const char* GetDisplayScreenshotFormatName(DisplayScreenshotFormat mode);
  static const char* GetDisplayScreenshotFormatDisplayName(DisplayScreenshotFormat mode);
  static const char* GetDisplayScreenshotFormatExtension(DisplayScreenshotFormat mode);

  static std::optional<MemoryCardType> ParseMemoryCardTypeName(const char* str);
  static const char* GetMemoryCardTypeName(MemoryCardType type);
  static const char* GetMemoryCardTypeDisplayName(MemoryCardType type);

  static std::optional<MultitapMode> ParseMultitapModeName(const char* str);
  static const char* GetMultitapModeName(MultitapMode mode);
  static const char* GetMultitapModeDisplayName(MultitapMode mode);

  static std::optional<CDROMMechaconVersion> ParseCDROMMechVersionName(const char* str);
  static const char* GetCDROMMechVersionName(CDROMMechaconVersion mode);
  static const char* GetCDROMMechVersionDisplayName(CDROMMechaconVersion mode);

  static std::optional<SaveStateCompressionMode> ParseSaveStateCompressionModeName(const char* str);
  static const char* GetSaveStateCompressionModeName(SaveStateCompressionMode mode);
  static const char* GetSaveStateCompressionModeDisplayName(SaveStateCompressionMode mode);

  static constexpr GPURenderer DEFAULT_GPU_RENDERER = GPURenderer::Automatic;
  static constexpr GPUTextureFilter DEFAULT_GPU_TEXTURE_FILTER = GPUTextureFilter::Nearest;
  static constexpr GPULineDetectMode DEFAULT_GPU_LINE_DETECT_MODE = GPULineDetectMode::Disabled;
  static constexpr GPUDownsampleMode DEFAULT_GPU_DOWNSAMPLE_MODE = GPUDownsampleMode::Disabled;
  static constexpr GPUWireframeMode DEFAULT_GPU_WIREFRAME_MODE = GPUWireframeMode::Disabled;
  static constexpr GPUDumpCompressionMode DEFAULT_GPU_DUMP_COMPRESSION_MODE = GPUDumpCompressionMode::ZstDefault;
  static constexpr ConsoleRegion DEFAULT_CONSOLE_REGION = ConsoleRegion::Auto;
  static constexpr float DEFAULT_GPU_PGXP_DEPTH_THRESHOLD = 300.0f;
  static constexpr float GPU_PGXP_DEPTH_THRESHOLD_SCALE = 4096.0f;
  static constexpr u8 DEFAULT_GPU_MAX_QUEUED_FRAMES = 2; // TODO: Maybe lower? But that means fast CPU threads would
                                                         // always stall, could be a problem for power management.

  // Prefer recompiler when supported.
#ifdef ENABLE_RECOMPILER
  static constexpr CPUExecutionMode DEFAULT_CPU_EXECUTION_MODE = CPUExecutionMode::Recompiler;
#else
  static constexpr CPUExecutionMode DEFAULT_CPU_EXECUTION_MODE = CPUExecutionMode::CachedInterpreter;
#endif

  // LUT still ends up faster on Apple Silicon for now, because of 16K pages.
#ifdef DYNAMIC_HOST_PAGE_SIZE
  static const CPUFastmemMode DEFAULT_CPU_FASTMEM_MODE;
#elif defined(ENABLE_MMAP_FASTMEM) && (!defined(__APPLE__) || !defined(__aarch64__))
  static constexpr CPUFastmemMode DEFAULT_CPU_FASTMEM_MODE = CPUFastmemMode::MMap;
#else
  static constexpr CPUFastmemMode DEFAULT_CPU_FASTMEM_MODE = CPUFastmemMode::LUT;
#endif

  static constexpr DisplayDeinterlacingMode DEFAULT_DISPLAY_DEINTERLACING_MODE = DisplayDeinterlacingMode::Progressive;
  static constexpr DisplayCropMode DEFAULT_DISPLAY_CROP_MODE = DisplayCropMode::Overscan;
  static constexpr DisplayAspectRatio DEFAULT_DISPLAY_ASPECT_RATIO = DisplayAspectRatio::Auto;
  static constexpr DisplayAlignment DEFAULT_DISPLAY_ALIGNMENT = DisplayAlignment::Center;
  static constexpr DisplayRotation DEFAULT_DISPLAY_ROTATION = DisplayRotation::Normal;
  static constexpr DisplayScalingMode DEFAULT_DISPLAY_SCALING = DisplayScalingMode::BilinearSmooth;
  static constexpr ForceVideoTimingMode DEFAULT_FORCE_VIDEO_TIMING_MODE = ForceVideoTimingMode::Disabled;
  static constexpr DisplayExclusiveFullscreenControl DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL =
    DisplayExclusiveFullscreenControl::Automatic;
  static constexpr DisplayScreenshotMode DEFAULT_DISPLAY_SCREENSHOT_MODE = DisplayScreenshotMode::ScreenResolution;
  static constexpr DisplayScreenshotFormat DEFAULT_DISPLAY_SCREENSHOT_FORMAT = DisplayScreenshotFormat::PNG;
  static constexpr u8 DEFAULT_DISPLAY_SCREENSHOT_QUALITY = 85;
  static constexpr float DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER = 2.0f;
  static constexpr float DEFAULT_OSD_SCALE = 100.0f;

  static constexpr u8 DEFAULT_CDROM_READAHEAD_SECTORS = 8;
  static constexpr CDROMMechaconVersion DEFAULT_CDROM_MECHACON_VERSION = CDROMMechaconVersion::VC1A;

  static constexpr ControllerType DEFAULT_CONTROLLER_1_TYPE = ControllerType::AnalogController;
  static constexpr ControllerType DEFAULT_CONTROLLER_2_TYPE = ControllerType::None;
  static constexpr MemoryCardType DEFAULT_MEMORY_CARD_1_TYPE = MemoryCardType::PerGameTitle;
  static constexpr MemoryCardType DEFAULT_MEMORY_CARD_2_TYPE = MemoryCardType::None;
  static constexpr MultitapMode DEFAULT_MULTITAP_MODE = MultitapMode::Disabled;

  static constexpr s32 DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME = 5;
  static constexpr s32 DEFAULT_LEADERBOARD_NOTIFICATION_TIME = 10;

  static constexpr Log::Level DEFAULT_LOG_LEVEL = Log::Level::Info;

  static constexpr SaveStateCompressionMode DEFAULT_SAVE_STATE_COMPRESSION_MODE = SaveStateCompressionMode::ZstDefault;

  static const MediaCaptureBackend DEFAULT_MEDIA_CAPTURE_BACKEND;
  static constexpr const char* DEFAULT_MEDIA_CAPTURE_CONTAINER = "mp4";
  static constexpr u32 DEFAULT_MEDIA_CAPTURE_VIDEO_WIDTH = 640;
  static constexpr u32 DEFAULT_MEDIA_CAPTURE_VIDEO_HEIGHT = 480;
  static constexpr u32 DEFAULT_MEDIA_CAPTURE_VIDEO_BITRATE = 6000;
  static constexpr u32 DEFAULT_MEDIA_CAPTURE_AUDIO_BITRATE = 128;

  // Android doesn't create settings until they're first opened, so we have to override the defaults here.
#ifndef __ANDROID__
  static constexpr bool DEFAULT_SAVE_STATE_BACKUPS = true;
  static constexpr bool DEFAULT_FAST_BOOT_VALUE = false;
  static constexpr u16 DEFAULT_GDB_SERVER_PORT = 2345;
#else
  static constexpr bool DEFAULT_SAVE_STATE_BACKUPS = false;
  static constexpr bool DEFAULT_FAST_BOOT_VALUE = true;
#endif
};

// TODO: Use smaller copy for GPU thread copy.
ALIGN_TO_CACHE_LINE extern Settings g_settings;     // CPU thread copy.
ALIGN_TO_CACHE_LINE extern Settings g_gpu_settings; // GPU thread copy.

namespace EmuFolders {
extern std::string AppRoot;
extern std::string DataRoot;
extern std::string Bios;
extern std::string Cache;
extern std::string Cheats;
extern std::string Covers;
extern std::string GameIcons;
extern std::string GameSettings;
extern std::string InputProfiles;
extern std::string MemoryCards;
extern std::string Patches;
extern std::string Resources;
extern std::string SaveStates;
extern std::string Screenshots;
extern std::string Shaders;
extern std::string Subchannels;
extern std::string Textures;
extern std::string UserResources;
extern std::string Videos;

// Assumes that AppRoot and DataRoot have been initialized.
void SetDefaults();
bool EnsureFoldersExist();
void LoadConfig(SettingsInterface& si);
void Save(SettingsInterface& si);

/// Updates the variables in the EmuFolders namespace, reloading subsystems if needed.
void Update();

/// Returns the path to a resource file, allowing the user to override it.
std::string GetOverridableResourcePath(std::string_view name);

/// Returns true if the application is running in portable mode.
bool IsRunningInPortableMode();
} // namespace EmuFolders
