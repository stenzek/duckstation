// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "system.h"
#include "achievements.h"
#include "bios.h"
#include "bus.h"
#include "cdrom.h"
#include "cheats.h"
#include "controller.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "cpu_pgxp.h"
#include "dma.h"
#include "fullscreen_ui.h"
#include "game_database.h"
#include "game_list.h"
#include "gpu.h"
#include "gpu_backend.h"
#include "gpu_dump.h"
#include "gpu_hw_texture_cache.h"
#include "gpu_presenter.h"
#include "gpu_thread.h"
#include "gte.h"
#include "host.h"
#include "imgui_overlays.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "memory_card.h"
#include "multitap.h"
#include "pad.h"
#include "pcdrv.h"
#include "performance_counters.h"
#include "pio.h"
#include "psf_loader.h"
#include "save_state_version.h"
#include "sio.h"
#include "spu.h"
#include "system_private.h"
#include "timers.h"

#include "scmversion/scmversion.h"

#include "util/audio_stream.h"
#include "util/cd_image.h"
#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/ini_settings_interface.h"
#include "util/input_manager.h"
#include "util/iso_reader.h"
#include "util/media_capture.h"
#include "util/platform_misc.h"
#include "util/postprocessing.h"
#include "util/sockets.h"
#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/binary_reader_writer.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/layered_settings_interface.h"
#include "common/log.h"
#include "common/memmap.h"
#include "common/path.h"
#include "common/ryml_helpers.h"
#include "common/string_util.h"
#include "common/task_queue.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome5.h"

#include "cpuinfo.h"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "imgui.h"
#include "xxhash.h"

#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <zlib.h>
#include <zstd.h>
#include <zstd_errors.h>

LOG_CHANNEL(System);

#ifdef _WIN32
#include "common/windows_headers.h"
#include <Objbase.h>
#endif

#ifndef __ANDROID__
#define ENABLE_DISCORD_PRESENCE 1
#define ENABLE_GDB_SERVER 1
#define ENABLE_SOCKET_MULTIPLEXER 1
#include "gdb_server.h"
#endif

// #define PROFILE_MEMORY_SAVE_STATES 1

SystemBootParameters::SystemBootParameters() = default;

SystemBootParameters::SystemBootParameters(const SystemBootParameters&) = default;

SystemBootParameters::SystemBootParameters(SystemBootParameters&& other) = default;

SystemBootParameters::SystemBootParameters(std::string filename_) : filename(std::move(filename_))
{
}

SystemBootParameters::~SystemBootParameters() = default;

namespace System {

static constexpr float PRE_FRAME_SLEEP_UPDATE_INTERVAL = 1.0f;
static constexpr const char FALLBACK_EXE_NAME[] = "PSX.EXE";
static constexpr u32 MAX_SKIPPED_DUPLICATE_FRAME_COUNT = 2; // 20fps minimum
static constexpr u32 MAX_SKIPPED_TIMEOUT_FRAME_COUNT = 1;   // 30fps minimum

namespace {

struct SaveStateBuffer
{
  std::string serial;
  std::string title;
  std::string media_path;
  u32 media_subimage_index;
  u32 version;
  Image screenshot;
  DynamicHeapArray<u8> state_data;
  size_t state_size;
};

} // namespace

static void CheckCacheLineSize();
static void LogStartupInformation();

static const SettingsInterface& GetControllerSettingsLayer(std::unique_lock<std::mutex>& lock);
static const SettingsInterface& GetHotkeySettingsLayer(std::unique_lock<std::mutex>& lock);

static std::string GetExecutableNameForImage(IsoReader& iso, bool strip_subdirectories);
static bool ReadExecutableFromImage(IsoReader& iso, std::string* out_executable_name,
                                    std::vector<u8>* out_executable_data);
static GameHash GetGameHashFromBuffer(std::string_view exe_name, std::span<const u8> exe_buffer,
                                      const IsoReader::ISOPrimaryVolumeDescriptor& iso_pvd, u32 track_1_length);

/// Settings that are looked up on demand.
static bool ShouldStartFullscreen();
static bool ShouldStartPaused();

/// Checks for settings changes, std::move() the old settings away for comparing beforehand.
static void CheckForSettingsChanges(const Settings& old_settings);
static void SetTaintsFromSettings();
static void WarnAboutStateTaints(u32 state_taints);
static void WarnAboutUnsafeSettings();
static void LogUnsafeSettingsToConsole(const SmallStringBase& messages);

static bool Initialize(std::unique_ptr<CDImage> disc, DiscRegion disc_region, bool force_software_renderer,
                       bool fullscreen, Error* error);
static bool LoadBIOS(Error* error);
static bool SetBootMode(BootMode new_boot_mode, DiscRegion disc_region, Error* error);
static void InternalReset();
static void ClearRunningGame();
static void DestroySystem();

static void RecreateGPU(GPURenderer new_renderer);
static std::string GetScreenshotPath(const char* extension);
static bool StartMediaCapture(std::string path, bool capture_video, bool capture_audio, u32 video_width,
                              u32 video_height);
static void StopMediaCapture(std::unique_ptr<MediaCapture> cap);

/// Returns true if boot is being fast forwarded.
static bool IsFastForwardingBoot();

/// Updates the throttle period, call when target emulation speed changes.
static void UpdateThrottlePeriod();
static void ResetThrottler();

/// Throttles the system, i.e. sleeps until it's time to execute the next frame.
static void Throttle(Timer::Value current_time, Timer::Value sleep_until);
static void AccumulatePreFrameSleepTime(Timer::Value current_time);
static void UpdateDisplayVSync();

static bool UpdateGameSettingsLayer();
static void UpdateInputSettingsLayer(std::string input_profile_name, std::unique_lock<std::mutex>& lock);
static void UpdateRunningGame(const std::string& path, CDImage* image, bool booting);
static bool CheckForRequiredSubQ(Error* error);

static void UpdateControllers();
static void ResetControllers();
static void UpdatePerGameMemoryCards();
static std::unique_ptr<MemoryCard> GetMemoryCardForSlot(u32 slot, MemoryCardType type);
static void UpdateMultitaps();

static std::string GetMediaPathFromSaveState(const char* path);
static bool SaveUndoLoadState();
static void UpdateMemorySaveStateSettings();
static bool LoadOneRewindState();
static bool LoadStateFromBuffer(const SaveStateBuffer& buffer, Error* error, bool update_display);
static bool LoadStateBufferFromFile(SaveStateBuffer* buffer, std::FILE* fp, Error* error, bool read_title,
                                    bool read_media_path, bool read_screenshot, bool read_data);
static bool ReadAndDecompressStateData(std::FILE* fp, std::span<u8> dst, u32 file_offset, u32 compressed_size,
                                       SAVE_STATE_HEADER::CompressionType method, Error* error);
static bool SaveStateToBuffer(SaveStateBuffer* buffer, Error* error, u32 screenshot_size = 256);
static bool SaveStateBufferToFile(const SaveStateBuffer& buffer, std::FILE* fp, Error* error,
                                  SaveStateCompressionMode compression_mode);
static u32 CompressAndWriteStateData(std::FILE* fp, std::span<const u8> src, SaveStateCompressionMode method,
                                     u32* header_type, Error* error);
static bool DoState(StateWrapper& sw, bool update_display);
static void DoMemoryState(StateWrapper& sw, MemorySaveState& mss, bool update_display);

static bool IsExecutionInterrupted();
static void CheckForAndExitExecution();

static void SetRewinding(bool enabled);
static void DoRewind();

static bool DoRunahead();

static bool ChangeGPUDump(std::string new_path);

static void UpdateSessionTime(const std::string& prev_serial);

#ifdef ENABLE_DISCORD_PRESENCE
static void InitializeDiscordPresence();
static void ShutdownDiscordPresence();
static void PollDiscordPresence();
#endif

namespace {

struct ALIGN_TO_CACHE_LINE StateVars
{
  TickCount ticks_per_second = 0;
  TickCount max_slice_ticks = 0;

  u32 frame_number = 0;
  u32 internal_frame_number = 0;

  ConsoleRegion region = ConsoleRegion::NTSC_U;
  State state = State::Shutdown;
  BootMode boot_mode = BootMode::None;

  bool system_executing = false;
  bool system_interrupted = false;
  bool frame_step_request = false;

  bool throttler_enabled = false;
  bool optimal_frame_pacing = false;
  bool skip_presenting_duplicate_frames = false;
  bool pre_frame_sleep = false;

  bool can_sync_to_host = false;
  bool syncing_to_host = false;

  bool fast_forward_enabled = false;
  bool turbo_enabled = false;

  bool runahead_replay_pending = false;

  u32 skipped_frame_count = 0;
  u32 last_presented_internal_frame_number = 0;

  float video_frame_rate = 0.0f;
  float target_speed = 0.0f;

  Timer::Value frame_period = 0;
  Timer::Value next_frame_time = 0;

  Timer::Value frame_start_time = 0;
  Timer::Value last_active_frame_time = 0;
  Timer::Value pre_frame_sleep_time = 0;
  Timer::Value max_active_frame_time = 0;
  Timer::Value last_pre_frame_sleep_update_time = 0;

  std::unique_ptr<MediaCapture> media_capture;
  std::unique_ptr<GPUDump::Player> gpu_dump_player;

  u32 runahead_frames = 0;
  u32 runahead_replay_frames = 0;

  s32 rewind_load_frequency = 0;
  s32 rewind_load_counter = 0;
  s32 rewind_save_frequency = 0;
  s32 rewind_save_counter = 0;

  std::vector<MemorySaveState> memory_save_states;
  u32 memory_save_state_front = 0;
  u32 memory_save_state_count = 0;

  const BIOS::ImageInfo* bios_image_info = nullptr;
  BIOS::ImageInfo::Hash bios_hash = {};
  u32 taints = 0;

  std::string running_game_path;
  std::string running_game_serial;
  std::string running_game_title;
  std::string exe_override;
  const GameDatabase::Entry* running_game_entry = nullptr;
  GameHash running_game_hash;
  bool running_game_custom_title = false;

  std::atomic_bool startup_cancelled{false};

  std::unique_ptr<INISettingsInterface> game_settings_interface;
  std::unique_ptr<INISettingsInterface> input_settings_interface;
  std::string input_profile_name;

  Threading::ThreadHandle cpu_thread_handle;

  // temporary save state, created when loading, used to undo load state
  std::optional<System::SaveStateBuffer> undo_load_state;

  // Used to track play time. We use a monotonic timer here, in case of clock changes.
  u64 session_start_time = 0;

  // internal async task counters
  std::atomic_uint32_t outstanding_save_state_tasks{0};

  // async task pool
  TaskQueue async_task_queue;

#ifdef ENABLE_SOCKET_MULTIPLEXER
  std::unique_ptr<SocketMultiplexer> socket_multiplexer;
#endif

#ifdef ENABLE_DISCORD_PRESENCE
  bool discord_presence_active = false;
  std::time_t discord_presence_time_epoch;
#endif
};

} // namespace

static StateVars s_state;

} // namespace System

static TinyString GetTimestampStringForFileName()
{
  return TinyString::from_format("{:%Y-%m-%d-%H-%M-%S}", fmt::localtime(std::time(nullptr)));
}

bool System::PerformEarlyHardwareChecks(Error* error)
{
  // This shouldn't fail... if it does, just hope for the best.
  cpuinfo_initialize();

#ifdef CPU_ARCH_X64
#ifdef CPU_ARCH_SSE41
  if (!cpuinfo_has_x86_sse4_1())
  {
    Error::SetStringFmt(
      error, "<h3>Your CPU does not support the SSE4.1 instruction set.</h3><p>SSE4.1 is required for this version of "
             "DuckStation. Please download and switch to the legacy SSE2 version.</p><p>You can download this from <a "
             "href=\"https://www.duckstation.org/\">www.duckstation.org</a> under \"Other Platforms\".");
    return false;
  }
#else
  if (cpuinfo_has_x86_sse4_1())
  {
    Error::SetStringFmt(
      error, "You are running the <strong>legacy SSE2 DuckStation executable</strong> on a CPU that supports the "
             "SSE4.1 instruction set.\nPlease download and switch to the regular, non-SSE2 version.\nYou can download "
             "this from <a href=\"https://www.duckstation.org/\">www.duckstation.org</a>.");
  }
#endif
#endif

#ifndef DYNAMIC_HOST_PAGE_SIZE
  // Check page size. If it doesn't match, it is a fatal error.
  const size_t runtime_host_page_size = MemMap::GetRuntimePageSize();
  if (runtime_host_page_size == 0)
  {
    Error::SetStringFmt(error, "Cannot determine size of page. Continuing with expectation of {} byte pages.",
                        runtime_host_page_size);
  }
  else if (HOST_PAGE_SIZE != runtime_host_page_size)
  {
    Error::SetStringFmt(
      error, "Page size mismatch. This build was compiled with {} byte pages, but the system has {} byte pages.",
      HOST_PAGE_SIZE, runtime_host_page_size);
    return false;
  }
#else
  if (HOST_PAGE_SIZE == 0 || HOST_PAGE_SIZE < MIN_HOST_PAGE_SIZE || HOST_PAGE_SIZE > MAX_HOST_PAGE_SIZE)
  {
    Error::SetStringFmt(error, "Page size of {} bytes is out of the range supported by this build: {}-{}.",
                        HOST_PAGE_SIZE, MIN_HOST_PAGE_SIZE, MAX_HOST_PAGE_SIZE);
    return false;
  }
#endif

  return true;
}

void System::CheckCacheLineSize()
{
  u32 max_line_size = 0;
  if (cpuinfo_initialize())
  {
    const u32 num_l1is = cpuinfo_get_l1i_caches_count();
    const u32 num_l1ds = cpuinfo_get_l1d_caches_count();
    const u32 num_l2s = cpuinfo_get_l2_caches_count();
    for (u32 i = 0; i < num_l1is; i++)
    {
      const cpuinfo_cache* cache = cpuinfo_get_l1i_cache(i);
      if (cache)
        max_line_size = std::max(max_line_size, cache->line_size);
    }
    for (u32 i = 0; i < num_l1ds; i++)
    {
      const cpuinfo_cache* cache = cpuinfo_get_l1d_cache(i);
      if (cache)
        max_line_size = std::max(max_line_size, cache->line_size);
    }
    for (u32 i = 0; i < num_l2s; i++)
    {
      const cpuinfo_cache* cache = cpuinfo_get_l2_cache(i);
      if (cache)
        max_line_size = std::max(max_line_size, cache->line_size);
    }
  }

  if (max_line_size == 0)
  {
    ERROR_LOG("Cannot determine size of cache line. Continuing with expectation of {} byte lines.",
              HOST_CACHE_LINE_SIZE);
  }
  else if (HOST_CACHE_LINE_SIZE != max_line_size)
  {
    // Not fatal, but does have performance implications.
    WARNING_LOG(
      "Cache line size mismatch. This build was compiled with {} byte lines, but the system has {} byte lines.",
      HOST_CACHE_LINE_SIZE, max_line_size);
  }
}

void System::LogStartupInformation()
{
#if !defined(CPU_ARCH_X64) || defined(CPU_ARCH_SSE41)
  const std::string_view suffix = {};
#else
  const std::string_view suffix = " [Legacy SSE2]";
#endif
  INFO_LOG("DuckStation for {} ({}){}", TARGET_OS_STR, CPU_ARCH_STR, suffix);
  INFO_LOG("Version: {} [{}]", g_scm_tag_str, g_scm_branch_str);
  INFO_LOG("SCM Timestamp: {}", g_scm_date_str);
  INFO_LOG("Build Timestamp: {} {}", __DATE__, __TIME__);
  if (const cpuinfo_package* package = cpuinfo_initialize() ? cpuinfo_get_package(0) : nullptr) [[likely]]
  {
    INFO_LOG("Host CPU: {}", package->name);
    INFO_LOG("CPU has {} logical processor(s) and {} core(s) across {} cluster(s).", package->processor_count,
             package->core_count, package->cluster_count);
  }

#ifdef DYNAMIC_HOST_PAGE_SIZE
  INFO_LOG("Host Page Size: {} bytes", HOST_PAGE_SIZE);
#endif
}

bool System::ProcessStartup(Error* error)
{
  Timer timer;

  // Allocate JIT memory as soon as possible.
  if (!CPU::CodeCache::ProcessStartup(error))
    return false;

  // g_settings is not valid at this point, query global config directly.
  const bool export_shared_memory = Host::GetBoolSettingValue("Hacks", "ExportSharedMemory", false);

  // Fastmem alloc *must* come after JIT alloc, otherwise it tends to eat the 4GB region after the executable on MacOS.
  if (!Bus::AllocateMemory(export_shared_memory, error))
  {
    CPU::CodeCache::ProcessShutdown();
    return false;
  }

  VERBOSE_LOG("Memory allocation took {} ms.", timer.GetTimeMilliseconds());

  CheckCacheLineSize();

  // Initialize rapidyaml before anything can use it.
  SetRymlCallbacks();

  return true;
}

void System::ProcessShutdown()
{
  Bus::ReleaseMemory();
  CPU::CodeCache::ProcessShutdown();
}

bool System::CPUThreadInitialize(Error* error, u32 async_worker_thread_count)
{
#ifdef _WIN32
  // On Win32, we have a bunch of things which use COM (e.g. SDL, Cubeb, etc).
  // We need to initialize COM first, before anything else does, because otherwise they might
  // initialize it in single-threaded/apartment mode, which can't be changed to multithreaded.
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "CoInitializeEx() failed: ", hr);
    return false;
  }
#endif

  s_state.cpu_thread_handle = Threading::ThreadHandle::GetForCallingThread();
  s_state.async_task_queue.SetWorkerCount(async_worker_thread_count);

  // This will call back to Host::LoadSettings() -> ReloadSources().
  LoadSettings(false);

  LogStartupInformation();

  GPUThread::Internal::ProcessStartup();

  if (g_settings.achievements_enabled)
    Achievements::Initialize();

#ifdef ENABLE_DISCORD_PRESENCE
  if (g_settings.enable_discord_presence)
    InitializeDiscordPresence();
#endif

  return true;
}

void System::CPUThreadShutdown()
{
#ifdef ENABLE_DISCORD_PRESENCE
  ShutdownDiscordPresence();
#endif

  Achievements::Shutdown(false);

  InputManager::CloseSources();

  s_state.async_task_queue.SetWorkerCount(0);
  s_state.cpu_thread_handle = {};

#ifdef _WIN32
  CoUninitialize();
#endif
}

const Threading::ThreadHandle& System::GetCPUThreadHandle()
{
  return s_state.cpu_thread_handle;
}

void System::SetCPUThreadHandle(Threading::ThreadHandle handle)
{
  s_state.cpu_thread_handle = std::move(handle);
}

void System::IdlePollUpdate()
{
  InputManager::PollSources();

#ifdef ENABLE_DISCORD_PRESENCE
  PollDiscordPresence();
#endif

  Achievements::IdleUpdate();

#ifdef ENABLE_SOCKET_MULTIPLEXER
  if (s_state.socket_multiplexer)
    s_state.socket_multiplexer->PollEventsWithTimeout(0);
#endif
}

System::State System::GetState()
{
  return s_state.state;
}

bool System::IsRunning()
{
  return s_state.state == State::Running;
}

ALWAYS_INLINE bool System::IsExecutionInterrupted()
{
  return s_state.state != State::Running || s_state.system_interrupted;
}

ALWAYS_INLINE_RELEASE void System::CheckForAndExitExecution()
{
  if (IsExecutionInterrupted()) [[unlikely]]
  {
    s_state.system_interrupted = false;

    TimingEvents::CancelRunningEvent();
    CPU::ExitExecution();
  }
}

bool System::IsPaused()
{
  return s_state.state == State::Paused;
}

bool System::IsShutdown()
{
  return s_state.state == State::Shutdown;
}

bool System::IsValid()
{
  return s_state.state == State::Running || s_state.state == State::Paused;
}

bool System::IsValidOrInitializing()
{
  return s_state.state == State::Starting || s_state.state == State::Running || s_state.state == State::Paused;
}

bool System::IsExecuting()
{
  DebugAssert(s_state.state != State::Shutdown);
  return s_state.system_executing;
}

bool System::IsReplayingGPUDump()
{
  return static_cast<bool>(s_state.gpu_dump_player);
}

size_t System::GetGPUDumpFrameCount()
{
  return s_state.gpu_dump_player ? s_state.gpu_dump_player->GetFrameCount() : 0;
}

bool System::IsStartupCancelled()
{
  return s_state.startup_cancelled.load(std::memory_order_acquire);
}

void System::CancelPendingStartup()
{
  if (s_state.state == State::Starting)
    s_state.startup_cancelled.store(true, std::memory_order_release);
}

void System::InterruptExecution()
{
  if (s_state.system_executing)
    s_state.system_interrupted = true;
}

ConsoleRegion System::GetRegion()
{
  return s_state.region;
}

DiscRegion System::GetDiscRegion()
{
  return CDROM::GetDiscRegion();
}

bool System::IsPALRegion()
{
  return s_state.region == ConsoleRegion::PAL;
}

std::string_view System::GetTaintDisplayName(Taint taint)
{
  static constexpr const std::array<const char*, static_cast<size_t>(Taint::MaxCount)> names = {{
    TRANSLATE_DISAMBIG_NOOP("System", "CPU Overclock", "Taint"),
    TRANSLATE_DISAMBIG_NOOP("System", "CD-ROM Read Speedup", "Taint"),
    TRANSLATE_DISAMBIG_NOOP("System", "CD-ROM Seek Speedup", "Taint"),
    TRANSLATE_DISAMBIG_NOOP("System", "Force Frame Timings", "Taint"),
    TRANSLATE_DISAMBIG_NOOP("System", "8MB RAM", "Taint"),
    TRANSLATE_DISAMBIG_NOOP("System", "Cheats", "Taint"),
    TRANSLATE_DISAMBIG_NOOP("System", "Game Patches", "Taint"),
  }};

  return Host::TranslateToStringView("System", names[static_cast<size_t>(taint)], "Taint");
}

const char* System::GetTaintName(Taint taint)
{
  static constexpr const std::array<const char*, static_cast<size_t>(Taint::MaxCount)> names = {{
    "CPUOverclock",
    "CDROMReadSpeedup",
    "CDROMSeekSpeedup",
    "ForceFrameTimings",
    "RAM8MB",
    "Cheats",
    "Patches",
  }};

  return names[static_cast<size_t>(taint)];
}

bool System::HasTaint(Taint taint)
{
  return (s_state.taints & (1u << static_cast<u8>(taint))) != 0u;
}

void System::SetTaint(Taint taint)
{
  if (!HasTaint(taint))
    WARNING_LOG("Setting system taint: {}", GetTaintName(taint));

  s_state.taints |= (1u << static_cast<u8>(taint));
}

TickCount System::GetTicksPerSecond()
{
  return s_state.ticks_per_second;
}

TickCount System::GetMaxSliceTicks()
{
  return s_state.max_slice_ticks;
}

void System::UpdateOverclock()
{
  s_state.ticks_per_second = ScaleTicksToOverclock(MASTER_CLOCK);
  s_state.max_slice_ticks = ScaleTicksToOverclock(MASTER_CLOCK / 10);
  SPU::CPUClockChanged();
  CDROM::CPUClockChanged();
  g_gpu.CPUClockChanged();
  Timers::CPUClocksChanged();
  UpdateThrottlePeriod();
}

GlobalTicks System::GetGlobalTickCounter()
{
  // When running events, the counter actually goes backwards, because the pending ticks are added in chunks.
  // So, we need to return the counter with all pending ticks added in such cases.
  return TimingEvents::IsRunningEvents() ? TimingEvents::GetEventRunTickCounter() :
                                           (TimingEvents::GetGlobalTickCounter() + CPU::GetPendingTicks());
}

u32 System::GetFrameNumber()
{
  return s_state.frame_number;
}

u32 System::GetInternalFrameNumber()
{
  return s_state.internal_frame_number;
}

const std::string& System::GetDiscPath()
{
  return s_state.running_game_path;
}
const std::string& System::GetGameSerial()
{
  return s_state.running_game_serial;
}

const std::string& System::GetGameTitle()
{
  return s_state.running_game_title;
}

const std::string& System::GetExeOverride()
{
  return s_state.exe_override;
}

const GameDatabase::Entry* System::GetGameDatabaseEntry()
{
  return s_state.running_game_entry;
}

GameHash System::GetGameHash()
{
  return s_state.running_game_hash;
}

bool System::IsRunningUnknownGame()
{
  return !s_state.running_game_entry;
}

System::BootMode System::GetBootMode()
{
  return s_state.boot_mode;
}

bool System::IsUsingPS2BIOS()
{
  return (s_state.bios_image_info && s_state.bios_image_info->fastboot_patch == BIOS::ImageInfo::FastBootPatch::Type2);
}

bool System::IsExePath(std::string_view path)
{
  return (StringUtil::EndsWithNoCase(path, ".exe") || StringUtil::EndsWithNoCase(path, ".psexe") ||
          StringUtil::EndsWithNoCase(path, ".ps-exe") || StringUtil::EndsWithNoCase(path, ".psx") ||
          StringUtil::EndsWithNoCase(path, ".cpe") || StringUtil::EndsWithNoCase(path, ".elf"));
}

bool System::IsPsfPath(std::string_view path)
{
  return (StringUtil::EndsWithNoCase(path, ".psf") || StringUtil::EndsWithNoCase(path, ".minipsf"));
}

bool System::IsGPUDumpPath(std::string_view path)
{
  return (StringUtil::EndsWithNoCase(path, ".psxgpu") || StringUtil::EndsWithNoCase(path, ".psxgpu.zst") ||
          StringUtil::EndsWithNoCase(path, ".psxgpu.xz"));
}

bool System::IsLoadablePath(std::string_view path)
{
  static constexpr const std::array extensions = {
    ".bin",    ".cue",        ".img",       ".iso", ".chd", ".ecm", ".mds", // discs
    ".exe",    ".psexe",      ".ps-exe",    ".psx", ".cpe", ".elf",         // exes
    ".psf",    ".minipsf",                                                  // psf
    ".psxgpu", ".psxgpu.zst", ".psxgpu.xz",                                 // gpu dump
    ".m3u",                                                                 // playlists
    ".pbp",
  };

  for (const char* test_extension : extensions)
  {
    if (StringUtil::EndsWithNoCase(path, test_extension))
      return true;
  }

  return false;
}

bool System::IsSaveStatePath(std::string_view path)
{
  return StringUtil::EndsWithNoCase(path, ".sav");
}

ConsoleRegion System::GetConsoleRegionForDiscRegion(DiscRegion region)
{
  switch (region)
  {
    case DiscRegion::NTSC_J:
      return ConsoleRegion::NTSC_J;

    case DiscRegion::NTSC_U:
    case DiscRegion::Other:
    case DiscRegion::NonPS1:
    default:
      return ConsoleRegion::NTSC_U;

    case DiscRegion::PAL:
      return ConsoleRegion::PAL;
  }
}

std::string System::GetGameHashId(GameHash hash)
{
  return fmt::format("HASH-{:X}", hash);
}

bool System::GetGameDetailsFromImage(CDImage* cdi, std::string* out_id, GameHash* out_hash,
                                     std::string* out_executable_name, std::vector<u8>* out_executable_data)
{
  IsoReader iso;
  std::string id;
  std::string exe_name;
  std::vector<u8> exe_buffer;
  if (!iso.Open(cdi, 1) || !ReadExecutableFromImage(iso, &exe_name, &exe_buffer))
  {
    if (out_id)
      out_id->clear();
    if (out_hash)
      *out_hash = 0;
    if (out_executable_name)
      out_executable_name->clear();
    if (out_executable_data)
      out_executable_data->clear();
    return false;
  }

  // Always compute the hash.
  const GameHash hash = GetGameHashFromBuffer(exe_name, exe_buffer, iso.GetPVD(), cdi->GetTrackLength(1));
  DEV_LOG("Hash for '{}' - {:016X}", exe_name, hash);

  if (exe_name != FALLBACK_EXE_NAME)
  {
    // Strip off any subdirectories.
    const std::string::size_type slash = exe_name.rfind('\\');
    if (slash != std::string::npos)
      id = std::string_view(exe_name).substr(slash + 1);
    else
      id = exe_name;

    // SCES_123.45 -> SCES-12345
    for (std::string::size_type pos = 0; pos < id.size();)
    {
      if (id[pos] == '.')
      {
        id.erase(pos, 1);
        continue;
      }

      if (id[pos] == '_')
        id[pos] = '-';
      else
        id[pos] = static_cast<char>(std::toupper(id[pos]));

      pos++;
    }
  }

  if (out_id)
  {
    if (id.empty())
      *out_id = GetGameHashId(hash);
    else
      *out_id = std::move(id);
  }

  if (out_hash)
    *out_hash = hash;

  if (out_executable_name)
    *out_executable_name = std::move(exe_name);
  if (out_executable_data)
    *out_executable_data = std::move(exe_buffer);

  return true;
}

GameHash System::GetGameHashFromFile(const char* path)
{
  const std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(path);
  if (!data)
    return 0;

  return GetGameHashFromBuffer(FileSystem::GetDisplayNameFromPath(path), data->cspan());
}

GameHash System::GetGameHashFromBuffer(const std::string_view filename, const std::span<const u8> data)
{
  return GetGameHashFromBuffer(filename, data, IsoReader::ISOPrimaryVolumeDescriptor{}, 0);
}

std::string System::GetExecutableNameForImage(IsoReader& iso, bool strip_subdirectories)
{
  // Read SYSTEM.CNF
  std::vector<u8> system_cnf_data;
  if (!iso.ReadFile("SYSTEM.CNF", &system_cnf_data, IsoReader::ReadMode::Data))
    return FALLBACK_EXE_NAME;

  // Parse lines
  std::vector<std::pair<std::string, std::string>> lines;
  std::pair<std::string, std::string> current_line;
  bool reading_value = false;
  for (size_t pos = 0; pos < system_cnf_data.size(); pos++)
  {
    const char ch = static_cast<char>(system_cnf_data[pos]);
    if (ch == '\r' || ch == '\n')
    {
      if (!current_line.first.empty())
      {
        lines.push_back(std::move(current_line));
        current_line = {};
        reading_value = false;
      }
    }
    else if (ch == ' ' || (ch >= 0x09 && ch <= 0x0D))
    {
      continue;
    }
    else if (ch == '=' && !reading_value)
    {
      reading_value = true;
    }
    else
    {
      if (reading_value)
        current_line.second.push_back(ch);
      else
        current_line.first.push_back(ch);
    }
  }

  if (!current_line.first.empty())
    lines.push_back(std::move(current_line));

  // Find the BOOT line
  auto iter = std::find_if(lines.begin(), lines.end(),
                           [](const auto& it) { return StringUtil::Strcasecmp(it.first.c_str(), "boot") == 0; });
  if (iter == lines.end())
  {
    // Fallback to PSX.EXE
    return FALLBACK_EXE_NAME;
  }

  std::string code = iter->second;
  std::string::size_type pos;
  if (strip_subdirectories)
  {
    // cdrom:\SCES_123.45;1
    pos = code.rfind('\\');
    if (pos != std::string::npos)
    {
      code.erase(0, pos + 1);
    }
    else
    {
      // cdrom:SCES_123.45;1
      pos = code.rfind(':');
      if (pos != std::string::npos)
        code.erase(0, pos + 1);
    }
  }
  else
  {
    if (code.compare(0, 6, "cdrom:") == 0)
      code.erase(0, 6);
    else
      WARNING_LOG("Unknown prefix in executable path: '{}'", code);

    // remove leading slashes
    while (code[0] == '/' || code[0] == '\\')
      code.erase(0, 1);
  }

  // strip off ; or version number
  pos = code.rfind(';');
  if (pos != std::string::npos)
    code.erase(pos);

  return code;
}

std::string System::GetExecutableNameForImage(CDImage* cdi, bool strip_subdirectories)
{
  IsoReader iso;
  if (!iso.Open(cdi, 1))
    return {};

  return GetExecutableNameForImage(iso, strip_subdirectories);
}

bool System::ReadExecutableFromImage(CDImage* cdi, std::string* out_executable_name,
                                     std::vector<u8>* out_executable_data)
{
  IsoReader iso;
  if (!iso.Open(cdi, 1))
    return false;

  return ReadExecutableFromImage(iso, out_executable_name, out_executable_data);
}

bool System::ReadExecutableFromImage(IsoReader& iso, std::string* out_executable_name,
                                     std::vector<u8>* out_executable_data)
{
  std::string executable_path = GetExecutableNameForImage(iso, false);
  DEV_LOG("Executable path: '{}'", executable_path);
  if (!executable_path.empty() && out_executable_data)
  {
    if (!iso.ReadFile(executable_path, out_executable_data, IsoReader::ReadMode::Data))
    {
      ERROR_LOG("Failed to read executable '{}' from disc", executable_path);
      return false;
    }
  }

  if (out_executable_name)
    *out_executable_name = std::move(executable_path);

  return true;
}

GameHash System::GetGameHashFromBuffer(std::string_view exe_name, std::span<const u8> exe_buffer,
                                       const IsoReader::ISOPrimaryVolumeDescriptor& iso_pvd, u32 track_1_length)
{
  XXH64_state_t* state = XXH64_createState();
  XXH64_reset(state, 0x4242D00C);
  XXH64_update(state, exe_name.data(), exe_name.size());
  XXH64_update(state, exe_buffer.data(), exe_buffer.size());
  XXH64_update(state, &iso_pvd, sizeof(IsoReader::ISOPrimaryVolumeDescriptor));
  XXH64_update(state, &track_1_length, sizeof(track_1_length));
  const GameHash hash = XXH64_digest(state);
  XXH64_freeState(state);
  return hash;
}

DiscRegion System::GetRegionForSerial(const std::string_view serial)
{
  static constexpr const std::pair<const char*, DiscRegion> region_prefixes[] = {
    {"sces", DiscRegion::PAL},    {"sced", DiscRegion::PAL},    {"sles", DiscRegion::PAL},
    {"sled", DiscRegion::PAL},

    {"scps", DiscRegion::NTSC_J}, {"slps", DiscRegion::NTSC_J}, {"slpm", DiscRegion::NTSC_J},
    {"sczs", DiscRegion::NTSC_J}, {"papx", DiscRegion::NTSC_J},

    {"scus", DiscRegion::NTSC_U}, {"slus", DiscRegion::NTSC_U},
  };

  for (const auto& [prefix, region] : region_prefixes)
  {
    if (StringUtil::StartsWithNoCase(serial, prefix))
      return region;
  }

  return DiscRegion::Other;
}

DiscRegion System::GetRegionFromSystemArea(CDImage* cdi)
{
  // The license code is on sector 4 of the disc.
  std::array<u8, CDImage::RAW_SECTOR_SIZE> sector;
  std::span<const u8> sector_data;
  if (cdi->GetTrackMode(1) == CDImage::TrackMode::Audio || !cdi->Seek(1, 4) ||
      !cdi->ReadRawSector(sector.data(), nullptr) ||
      (sector_data = IsoReader::ExtractSectorData(sector, IsoReader::ReadMode::Data, nullptr)).empty())
  {
    return DiscRegion::Other;
  }

  static constexpr char ntsc_u_string[] = "          Licensed  by          Sony Computer Entertainment Amer  ica ";
  static constexpr char ntsc_j_string[] = "          Licensed  by          Sony Computer Entertainment Inc.";
  static constexpr char pal_string[] = "          Licensed  by          Sony Computer Entertainment Euro pe";

  // subtract one for the terminating null
  if (std::memcmp(sector_data.data(), ntsc_u_string, std::size(ntsc_u_string) - 1) == 0)
    return DiscRegion::NTSC_U;
  else if (std::memcmp(sector_data.data(), ntsc_j_string, std::size(ntsc_j_string) - 1) == 0)
    return DiscRegion::NTSC_J;
  else if (std::memcmp(sector_data.data(), pal_string, std::size(pal_string) - 1) == 0)
    return DiscRegion::PAL;
  else
    return DiscRegion::Other;
}

DiscRegion System::GetRegionForImage(CDImage* cdi)
{
  const DiscRegion system_area_region = GetRegionFromSystemArea(cdi);
  if (system_area_region != DiscRegion::Other)
    return system_area_region;

  IsoReader iso;
  if (!iso.Open(cdi, 1))
    return DiscRegion::NonPS1;

  // The executable must exist, because this just returns PSX.EXE if it doesn't.
  const std::string exename = GetExecutableNameForImage(iso, false);
  if (exename.empty() || !iso.FileExists(exename.c_str()))
    return DiscRegion::NonPS1;

  // Strip off any subdirectories.
  const std::string::size_type slash = exename.rfind('\\');
  if (slash != std::string::npos)
    return GetRegionForSerial(std::string_view(exename).substr(slash + 1));
  else
    return GetRegionForSerial(exename);
}

DiscRegion System::GetRegionForExe(const char* path)
{
  auto fp = FileSystem::OpenManagedCFile(path, "rb");
  if (!fp)
    return DiscRegion::Other;

  BIOS::PSEXEHeader header;
  if (std::fread(&header, sizeof(header), 1, fp.get()) != 1)
    return DiscRegion::Other;

  return BIOS::GetPSExeDiscRegion(header);
}

DiscRegion System::GetRegionForPsf(const char* path)
{
  PSFLoader::File psf;
  if (!psf.Load(path, nullptr))
    return DiscRegion::Other;

  return psf.GetRegion();
}

void System::RecreateGPU(GPURenderer renderer)
{
  FreeMemoryStateStorage(false, true, false);
  StopMediaCapture();

  Error error;
  if (!GPUThread::CreateGPUBackend(s_state.running_game_serial, renderer, true, false, false, &error))
  {
    ERROR_LOG("Failed to switch to {} renderer: {}", Settings::GetRendererName(renderer), error.GetDescription());
    Panic("Failed to switch renderer.");
  }

  ClearMemorySaveStates(true, false);

  g_gpu.UpdateDisplay(false);
  if (IsPaused())
    GPUThread::PresentCurrentFrame();
}

void System::LoadSettings(bool display_osd_messages)
{
  std::unique_lock<std::mutex> lock = Host::GetSettingsLock();
  const SettingsInterface& si = *Host::GetSettingsInterface();
  const SettingsInterface& controller_si = GetControllerSettingsLayer(lock);
  const SettingsInterface& hotkey_si = GetHotkeySettingsLayer(lock);
  const bool previous_safe_mode = g_settings.disable_all_enhancements;
  g_settings.Load(si, controller_si);

  // Global safe mode overrides game settings.
  g_settings.disable_all_enhancements =
    (g_settings.disable_all_enhancements ||
     Host::Internal::GetBaseSettingsLayer()->GetBoolValue("Main", "DisableAllEnhancements", false));

  Settings::UpdateLogConfig(si);
  Host::LoadSettings(si, lock);
  InputManager::ReloadSources(controller_si, lock);
  InputManager::ReloadBindings(controller_si, hotkey_si);

  // show safe mode warning if it's toggled on, or on startup
  if (IsValidOrInitializing() && (display_osd_messages || (!previous_safe_mode && g_settings.disable_all_enhancements)))
    WarnAboutUnsafeSettings();

  // apply compatibility settings
  if (g_settings.apply_compatibility_settings && s_state.running_game_entry)
    s_state.running_game_entry->ApplySettings(g_settings, display_osd_messages);

  // patch overrides take precedence over compat settings
  Cheats::ApplySettingOverrides();

  g_settings.FixIncompatibleSettings(si, display_osd_messages);
}

void System::ReloadInputSources()
{
  std::unique_lock<std::mutex> lock = Host::GetSettingsLock();
  const SettingsInterface& controller_si = GetControllerSettingsLayer(lock);
  InputManager::ReloadSources(controller_si, lock);

  // skip loading bindings if we're not running, since it'll get done on startup anyway
  if (IsValid())
  {
    const SettingsInterface& hotkey_si = GetHotkeySettingsLayer(lock);
    InputManager::ReloadBindings(controller_si, hotkey_si);
  }
}

void System::ReloadInputBindings()
{
  // skip loading bindings if we're not running, since it'll get done on startup anyway
  if (!IsValid())
    return;

  std::unique_lock<std::mutex> lock = Host::GetSettingsLock();
  const SettingsInterface& controller_si = GetControllerSettingsLayer(lock);
  const SettingsInterface& hotkey_si = GetHotkeySettingsLayer(lock);
  InputManager::ReloadBindings(controller_si, hotkey_si);
}

const SettingsInterface& System::GetControllerSettingsLayer(std::unique_lock<std::mutex>& lock)
{
  // Select input profile _or_ game settings, not both.
  if (const SettingsInterface* isi = Host::Internal::GetInputSettingsLayer())
  {
    return *isi;
  }
  else if (const SettingsInterface* gsi = Host::Internal::GetGameSettingsLayer();
           gsi && gsi->GetBoolValue("ControllerPorts", "UseGameSettingsForController", false))
  {
    return *gsi;
  }
  else
  {
    return *Host::Internal::GetBaseSettingsLayer();
  }
}

const SettingsInterface& System::GetHotkeySettingsLayer(std::unique_lock<std::mutex>& lock)
{
  // Only add input profile layer if the option is enabled.
  if (const SettingsInterface* isi = Host::Internal::GetInputSettingsLayer();
      isi && isi->GetBoolValue("ControllerPorts", "UseProfileHotkeyBindings", false))
  {
    return *isi;
  }
  else
  {
    return *Host::Internal::GetBaseSettingsLayer();
  }
}

void System::SetDefaultSettings(SettingsInterface& si)
{
  Settings temp;

  // we don't want to reset some things (e.g. OSD)
  temp.display_show_fps = g_settings.display_show_fps;
  temp.display_show_speed = g_settings.display_show_speed;
  temp.display_show_gpu_stats = g_settings.display_show_gpu_stats;
  temp.display_show_resolution = g_settings.display_show_resolution;
  temp.display_show_cpu_usage = g_settings.display_show_cpu_usage;
  temp.display_show_gpu_usage = g_settings.display_show_gpu_usage;
  temp.display_show_frame_times = g_settings.display_show_frame_times;

  // keep controller, we reset it elsewhere
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    temp.controller_types[i] = g_settings.controller_types[i];

  temp.Save(si, false);

  si.SetBoolValue("Main", "StartPaused", false);
  si.SetBoolValue("Main", "StartFullscreen", false);

  Settings::SetDefaultLogConfig(si);

#ifndef __ANDROID__
  si.SetStringValue("MediaCapture", "Backend", MediaCapture::GetBackendName(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND));
  si.SetStringValue("MediaCapture", "Container", Settings::DEFAULT_MEDIA_CAPTURE_CONTAINER);
  si.SetBoolValue("MediaCapture", "VideoCapture", true);
  si.SetUIntValue("MediaCapture", "VideoWidth", Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_WIDTH);
  si.SetUIntValue("MediaCapture", "VideoHeight", Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_HEIGHT);
  si.SetBoolValue("MediaCapture", "VideoAutoSize", false);
  si.SetUIntValue("MediaCapture", "VideoBitrate", Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_BITRATE);
  si.SetStringValue("MediaCapture", "VideoCodec", "");
  si.SetBoolValue("MediaCapture", "VideoCodecUseArgs", false);
  si.SetStringValue("MediaCapture", "AudioCodecArgs", "");
  si.SetBoolValue("MediaCapture", "AudioCapture", true);
  si.SetUIntValue("MediaCapture", "AudioBitrate", Settings::DEFAULT_MEDIA_CAPTURE_AUDIO_BITRATE);
  si.SetStringValue("MediaCapture", "AudioCodec", "");
  si.SetBoolValue("MediaCapture", "AudioCodecUseArgs", false);
  si.SetStringValue("MediaCapture", "AudioCodecArgs", "");
#endif
}

void System::ApplySettings(bool display_osd_messages)
{
  DEV_LOG("Applying settings...");

  // copy safe mode setting, so the osd check in LoadSettings() works
  const Settings old_settings = std::move(g_settings);
  g_settings = Settings();
  g_settings.disable_all_enhancements = old_settings.disable_all_enhancements;
  LoadSettings(display_osd_messages);

  // If we've disabled/enabled game settings, we need to reload without it.
  // Also reload cheats when safe mode is toggled, because patches might change.
  if (g_settings.apply_game_settings != old_settings.apply_game_settings)
  {
    UpdateGameSettingsLayer();
    LoadSettings(display_osd_messages);
  }

  CheckForSettingsChanges(old_settings);
  Host::CheckForSettingsChanges(old_settings);
}

void System::ReloadGameSettings(bool display_osd_messages)
{
  if (!IsValid() || !UpdateGameSettingsLayer())
    return;

  if (!IsReplayingGPUDump())
    Cheats::ReloadCheats(false, true, false, true, true);

  ApplySettings(display_osd_messages);
}

void System::ReloadInputProfile(bool display_osd_messages)
{
  if (!IsValid() || !s_state.game_settings_interface)
    return;

  // per-game configuration?
  if (s_state.game_settings_interface->GetBoolValue("ControllerPorts", "UseGameSettingsForController", false))
  {
    // update the whole game settings layer.
    UpdateGameSettingsLayer();
  }
  else if (std::string profile_name =
             s_state.game_settings_interface->GetStringValue("ControllerPorts", "InputProfileName");
           !profile_name.empty())
  {
    // only have to reload the input layer
    auto lock = Host::GetSettingsLock();
    UpdateInputSettingsLayer(std::move(profile_name), lock);
  }

  ApplySettings(display_osd_messages);
}

std::string System::GetInputProfilePath(std::string_view name)
{
  return Path::Combine(EmuFolders::InputProfiles, fmt::format("{}.ini", name));
}

std::string System::GetGameSettingsPath(std::string_view game_serial, bool ignore_disc_set)
{
  // multi-disc games => always use the first disc
  const GameDatabase::Entry* entry = ignore_disc_set ? nullptr : GameDatabase::GetEntryForSerial(game_serial);
  const std::string_view serial_for_path =
    (entry && !entry->disc_set_serials.empty()) ? entry->disc_set_serials.front() : game_serial;
  return Path::Combine(EmuFolders::GameSettings, fmt::format("{}.ini", Path::SanitizeFileName(serial_for_path)));
}

std::unique_ptr<INISettingsInterface> System::GetGameSettingsInterface(const GameDatabase::Entry* dbentry,
                                                                       std::string_view serial, bool create, bool quiet)
{
  std::unique_ptr<INISettingsInterface> ret;
  std::string path = GetGameSettingsPath(serial, false);

  if (FileSystem::FileExists(path.c_str()))
  {
    if (!quiet)
      INFO_COLOR_LOG(StrongCyan, "Loading game settings from '{}'...", Path::GetFileName(path));

    Error error;
    ret = std::make_unique<INISettingsInterface>(std::move(path));
    if (ret->Load(&error))
    {
      // Check for separate disc configuration.
      if (dbentry && !dbentry->disc_set_serials.empty() && dbentry->disc_set_serials.front() != serial)
      {
        if (ret->GetBoolValue("Main", "UseSeparateConfigForDiscSet", false))
        {
          if (!quiet)
          {
            INFO_COLOR_LOG(StrongCyan, "Using separate disc game settings serial {} for disc set {}", serial,
                           dbentry->disc_set_serials.front());
          }

          // Load the disc specific ini.
          path = GetGameSettingsPath(serial, true);
          if (FileSystem::FileExists(path.c_str()))
          {
            if (!ret->Load(std::move(path), &error))
            {
              if (!quiet)
              {
                ERROR_LOG("Failed to parse separate disc game settings ini '{}': {}", Path::GetFileName(ret->GetPath()),
                          error.GetDescription());
              }

              if (create)
                ret->Clear();
              else
                ret.reset();
            }
          }
          else
          {
            if (!quiet)
              INFO_COLOR_LOG(StrongCyan, "No separate disc game settings found (tried '{}')", Path::GetFileName(path));

            ret.reset();

            // return empty ini struct?
            if (create)
              ret = std::make_unique<INISettingsInterface>(std::move(path));
          }
        }
      }
    }
    else
    {
      if (!quiet)
      {
        ERROR_LOG("Failed to parse game settings ini '{}': {}", Path::GetFileName(ret->GetPath()),
                  error.GetDescription());
      }

      if (!create)
        ret.reset();
    }
  }
  else
  {
    if (!quiet)
      INFO_COLOR_LOG(StrongCyan, "No game settings found (tried '{}')", Path::GetFileName(path));

    // return empty ini struct?
    if (create)
      ret = std::make_unique<INISettingsInterface>(std::move(path));
  }

  return ret;
}

bool System::UpdateGameSettingsLayer()
{
  std::unique_ptr<INISettingsInterface> new_interface;
  if (g_settings.apply_game_settings && !s_state.running_game_serial.empty())
    new_interface = GetGameSettingsInterface(s_state.running_game_entry, s_state.running_game_serial, false, false);

  std::string input_profile_name;
  if (new_interface)
  {
    if (!new_interface->GetBoolValue("ControllerPorts", "UseGameSettingsForController", false))
      new_interface->GetStringValue("ControllerPorts", "InputProfileName", &input_profile_name);
  }

  if (!s_state.game_settings_interface && !new_interface && s_state.input_profile_name == input_profile_name)
    return false;

  auto lock = Host::GetSettingsLock();
  Host::Internal::SetGameSettingsLayer(new_interface.get(), lock);
  s_state.game_settings_interface = std::move(new_interface);

  UpdateInputSettingsLayer(std::move(input_profile_name), lock);
  return true;
}

void System::UpdateInputSettingsLayer(std::string input_profile_name, std::unique_lock<std::mutex>& lock)
{
  std::unique_ptr<INISettingsInterface> input_interface;
  if (!input_profile_name.empty())
  {
    std::string filename = GetInputProfilePath(input_profile_name);
    if (FileSystem::FileExists(filename.c_str()))
    {
      INFO_LOG("Loading input profile from '{}'...", Path::GetFileName(filename));
      input_interface = std::make_unique<INISettingsInterface>(std::move(filename));
      if (!input_interface->Load())
      {
        ERROR_LOG("Failed to parse input profile ini '{}'", Path::GetFileName(input_interface->GetPath()));
        input_interface.reset();
        input_profile_name = {};
      }
    }
    else
    {
      WARNING_LOG("No input profile found (tried '{}')", Path::GetFileName(filename));
      input_profile_name = {};
    }
  }

  Host::Internal::SetInputSettingsLayer(input_interface.get(), lock);
  s_state.input_settings_interface = std::move(input_interface);
  s_state.input_profile_name = std::move(input_profile_name);
}

void System::ResetSystem()
{
  if (!IsValid())
    return;

  if (!Achievements::ConfirmSystemReset())
    return;

  if (Achievements::ResetHardcoreMode(false))
  {
    // Make sure a pre-existing cheat file hasn't been loaded when resetting after enabling HC mode.
    Cheats::ReloadCheats(true, true, false, true, true);
    ApplySettings(false);
  }

  InternalReset();

  // Reset boot mode/reload BIOS if needed. Preserve exe/psf boot.
  const BootMode new_boot_mode = (s_state.boot_mode == BootMode::BootEXE || s_state.boot_mode == BootMode::BootPSF) ?
                                   s_state.boot_mode :
                                   (g_settings.bios_patch_fast_boot ? BootMode::FastBoot : BootMode::FullBoot);
  if (Error error; !SetBootMode(new_boot_mode, CDROM::GetDiscRegion(), &error))
    ERROR_LOG("Failed to reload BIOS on boot mode change, the system may be unstable: {}", error.GetDescription());

  // Have to turn on turbo if fast forwarding boot.
  if (IsFastForwardingBoot())
    UpdateSpeedLimiterState();

  Host::AddIconOSDMessage("SystemReset", ICON_FA_POWER_OFF, TRANSLATE_STR("OSDMessage", "System reset."),
                          Host::OSD_QUICK_DURATION);

  PerformanceCounters::Reset();
  ResetThrottler();
  InterruptExecution();
}

void System::PauseSystem(bool paused)
{
  if (paused == IsPaused() || !IsValid())
    return;

  s_state.state = (paused ? State::Paused : State::Running);
  SPU::GetOutputStream()->SetPaused(paused);
  GPUThread::RunOnThread([paused]() { GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::SystemPaused, paused); });

  if (paused)
  {
    InputManager::PauseVibration();
    InputManager::UpdateHostMouseMode();

    Achievements::OnSystemPaused(true);

    if (g_settings.inhibit_screensaver)
      PlatformMisc::ResumeScreensaver();

#ifdef ENABLE_GDB_SERVER
    GDBServer::OnSystemPaused();
#endif

    Host::OnSystemPaused();
    UpdateDisplayVSync();
    GPUThread::PresentCurrentFrame();
  }
  else
  {
    FullscreenUI::OnSystemResumed();

    InputManager::UpdateHostMouseMode();

    Achievements::OnSystemPaused(false);

    if (g_settings.inhibit_screensaver)
      PlatformMisc::SuspendScreensaver();

#ifdef ENABLE_GDB_SERVER
    GDBServer::OnSystemResumed();
#endif

    Host::OnSystemResumed();
    UpdateDisplayVSync();
    PerformanceCounters::Reset();
    ResetThrottler();
  }
}

bool System::SaveResumeState(Error* error)
{
  if (s_state.running_game_serial.empty())
  {
    Error::SetStringView(error, "Cannot save resume state without serial.");
    return false;
  }

  std::string path(GetGameSaveStateFileName(s_state.running_game_serial, -1));
  return SaveState(std::move(path), error, false, true);
}

bool System::BootSystem(SystemBootParameters parameters, Error* error)
{
  if (!parameters.save_state.empty())
  {
    // loading a state, so pull the media path from the save state to avoid a double change
    std::string state_media(GetMediaPathFromSaveState(parameters.save_state.c_str()));
    if (FileSystem::FileExists(state_media.c_str()))
      parameters.filename = std::move(state_media);
  }

  if (parameters.filename.empty())
    INFO_LOG("Boot Filename: <BIOS/Shell>");
  else
    INFO_LOG("Boot Filename: {}", parameters.filename);

  // Load CD image up and detect region.
  std::unique_ptr<CDImage> disc;
  ConsoleRegion auto_console_region = ConsoleRegion::NTSC_U;
  DiscRegion disc_region = DiscRegion::NonPS1;
  BootMode boot_mode = BootMode::FullBoot;
  std::string exe_override;
  std::unique_ptr<GPUDump::Player> gpu_dump;
  if (!parameters.filename.empty())
  {
    if (IsExePath(parameters.filename))
    {
      boot_mode = BootMode::BootEXE;
      exe_override = parameters.filename;
    }
    else if (IsPsfPath(parameters.filename))
    {
      boot_mode = BootMode::BootPSF;
      exe_override = parameters.filename;
    }
    else if (IsGPUDumpPath(parameters.filename))
    {
      gpu_dump = GPUDump::Player::Open(parameters.filename, error);
      if (!gpu_dump)
        return false;

      boot_mode = BootMode::ReplayGPUDump;
    }

    if (boot_mode == BootMode::BootEXE || boot_mode == BootMode::BootPSF)
    {
      const DiscRegion file_region = ((boot_mode == BootMode::BootEXE) ? GetRegionForExe(parameters.filename.c_str()) :
                                                                         GetRegionForPsf(parameters.filename.c_str()));
      INFO_LOG("EXE/PSF Region: {}", Settings::GetDiscRegionDisplayName(file_region));
      auto_console_region = GetConsoleRegionForDiscRegion(file_region);
    }
    else if (boot_mode != BootMode::ReplayGPUDump)
    {
      INFO_LOG("Loading CD image '{}'...", Path::GetFileName(parameters.filename));
      disc = CDImage::Open(parameters.filename.c_str(), g_settings.cdrom_load_image_patches, error);
      if (!disc)
      {
        Error::AddPrefixFmt(error, "Failed to open CD image '{}':\n", Path::GetFileName(parameters.filename));
        return false;
      }

      disc_region = GameList::GetCustomRegionForPath(parameters.filename).value_or(GetRegionForImage(disc.get()));
      auto_console_region = GetConsoleRegionForDiscRegion(disc_region);
      INFO_LOG("Auto-detected console {} region for '{}' (region {})",
               Settings::GetConsoleRegionName(auto_console_region), parameters.filename,
               Settings::GetDiscRegionName(disc_region));
    }
  }

  // Switch subimage.
  if (disc && parameters.media_playlist_index != 0 && !disc->SwitchSubImage(parameters.media_playlist_index, error))
  {
    Error::AddPrefixFmt(error, "Failed to switch to subimage {} in '{}':\n", parameters.media_playlist_index,
                        Path::GetFileName(parameters.filename));
    return false;
  }

  // Can't early cancel without destroying past this point.
  Assert(s_state.state == State::Shutdown);
  s_state.state = State::Starting;
  s_state.region = auto_console_region;
  s_state.startup_cancelled.store(false, std::memory_order_relaxed);
  s_state.gpu_dump_player = std::move(gpu_dump);
  std::atomic_thread_fence(std::memory_order_release);
  Host::OnSystemStarting();
  FullscreenUI::OnSystemStarting();

  // Update running game, this will apply settings as well.
  UpdateRunningGame(disc ? disc->GetPath() : parameters.filename, disc.get(), true);

  // Determine console region. Has to be done here, because gamesettings can override it.
  s_state.region = (g_settings.region == ConsoleRegion::Auto) ? auto_console_region : g_settings.region;
  INFO_LOG("Console Region: {}", Settings::GetConsoleRegionDisplayName(s_state.region));

  // Get boot EXE override.
  if (!parameters.override_exe.empty())
  {
    if (!FileSystem::FileExists(parameters.override_exe.c_str()) || !IsExePath(parameters.override_exe))
    {
      Error::SetStringFmt(error, "File '{}' is not a valid executable to boot.",
                          Path::GetFileName(parameters.override_exe));
      DestroySystem();
      return false;
    }

    INFO_LOG("Overriding boot executable: '{}'", parameters.override_exe);
    boot_mode = BootMode::BootEXE;
    exe_override = std::move(parameters.override_exe);
  }

  // Achievement hardcore checks before committing to anything.
  if (disc)
  {
    // Check for resuming with hardcore mode.
    if (parameters.disable_achievements_hardcore_mode)
      Achievements::DisableHardcoreMode();
    else
      Achievements::ResetHardcoreMode(true);

    if ((!parameters.save_state.empty() || !exe_override.empty()) && Achievements::IsHardcoreModeActive())
    {
      const bool is_exe_override_boot = parameters.save_state.empty();
      bool cancelled;
      if (FullscreenUI::IsInitialized())
      {
        Achievements::ConfirmHardcoreModeDisableAsync(is_exe_override_boot ?
                                                        TRANSLATE("Achievements", "Overriding executable") :
                                                        TRANSLATE("Achievements", "Resuming state"),
                                                      [parameters = std::move(parameters)](bool approved) mutable {
                                                        if (approved)
                                                        {
                                                          parameters.disable_achievements_hardcore_mode = true;
                                                          BootSystem(std::move(parameters), nullptr);
                                                        }
                                                      });
        cancelled = true;
      }
      else
      {
        cancelled = !Achievements::ConfirmHardcoreModeDisable(is_exe_override_boot ?
                                                                TRANSLATE("Achievements", "Overriding executable") :
                                                                TRANSLATE("Achievements", "Resuming state"));
      }

      if (cancelled)
      {
        // Technically a failure, but user-initiated. Returning false here would try to display a non-existent error.
        DestroySystem();
        return true;
      }
    }
  }

  // Are we fast booting? Must be checked after updating game settings.
  if (boot_mode == BootMode::FullBoot && disc_region != DiscRegion::NonPS1 &&
      parameters.override_fast_boot.value_or(static_cast<bool>(g_settings.bios_patch_fast_boot)))
  {
    boot_mode = BootMode::FastBoot;
  }

  // Load BIOS image, component setup, check for subchannel in games that need it.
  if (!SetBootMode(boot_mode, disc_region, error) ||
      !Initialize(std::move(disc), disc_region, parameters.force_software_renderer,
                  parameters.override_fullscreen.value_or(ShouldStartFullscreen()), error) ||
      !CheckForRequiredSubQ(error))
  {
    DestroySystem();
    return false;
  }

  s_state.exe_override = std::move(exe_override);

  UpdateControllers();
  UpdateMemoryCardTypes();
  UpdateMultitaps();
  InternalReset();

  // Good to go.
  s_state.state = State::Running;
  std::atomic_thread_fence(std::memory_order_release);
  SPU::GetOutputStream()->SetPaused(false);

  // Immediately pausing?
  const bool start_paused = (ShouldStartPaused() || parameters.override_start_paused.value_or(false));

  // try to load the state, if it fails, bail out
  if (!parameters.save_state.empty() && !LoadState(parameters.save_state.c_str(), error, false, start_paused))
  {
    Error::AddPrefixFmt(error, "Failed to load save state file '{}' for booting:\n",
                        Path::GetFileName(parameters.save_state));
    DestroySystem();
    return false;
  }

  InputManager::UpdateHostMouseMode();

  if (g_settings.inhibit_screensaver)
    PlatformMisc::SuspendScreensaver();

#ifdef ENABLE_GDB_SERVER
  if (g_settings.enable_gdb_server)
    GDBServer::Initialize(g_settings.gdb_server_port);
#endif

  Host::OnSystemStarted();

  if (parameters.load_image_to_ram || g_settings.cdrom_load_image_to_ram)
    CDROM::PrecacheMedia();

  if (parameters.start_media_capture)
    StartMediaCapture({});

  if (start_paused)
    PauseSystem(true);

  UpdateSpeedLimiterState();
  PerformanceCounters::Reset();
  ResetThrottler();
  return true;
}

bool System::Initialize(std::unique_ptr<CDImage> disc, DiscRegion disc_region, bool force_software_renderer,
                        bool fullscreen, Error* error)
{
  s_state.ticks_per_second = ScaleTicksToOverclock(MASTER_CLOCK);
  s_state.max_slice_ticks = ScaleTicksToOverclock(MASTER_CLOCK / 10);
  s_state.frame_number = 1;
  s_state.internal_frame_number = 0;

  s_state.target_speed = g_settings.emulation_speed;
  s_state.video_frame_rate = 60.0f;
  s_state.frame_period = 0;
  s_state.next_frame_time = 0;
  s_state.turbo_enabled = false;
  s_state.fast_forward_enabled = false;

  s_state.rewind_load_frequency = -1;
  s_state.rewind_load_counter = -1;
  s_state.rewind_save_frequency = -1;
  s_state.rewind_save_counter = -1;

  TimingEvents::Initialize();

  Bus::Initialize();
  CPU::Initialize();
  CDROM::Initialize();

  if (!PIO::Initialize(error))
    return false;

  // CDROM before GPU, that way we don't modeswitch.
  if (disc &&
      !CDROM::InsertMedia(std::move(disc), disc_region, s_state.running_game_serial, s_state.running_game_title, error))
    return false;

  // TODO: Drop class
  g_gpu.Initialize();

  // This can fail due to the application being closed during startup.
  if (!GPUThread::CreateGPUBackend(s_state.running_game_serial,
                                   force_software_renderer ? GPURenderer::Software : g_settings.gpu_renderer, false,
                                   fullscreen, false, error))
  {
    return false;
  }

  if (g_settings.gpu_pgxp_enable)
    CPU::PGXP::Initialize();

  // Was startup cancelled? (e.g. shading compilers took too long and the user closed the application)
  if (IsStartupCancelled())
  {
    Error::SetStringView(error, TRANSLATE_SV("System", "Startup was cancelled."));
    return false;
  }

  DMA::Initialize();
  Pad::Initialize();
  Timers::Initialize();
  SPU::Initialize();
  MDEC::Initialize();
  SIO::Initialize();
  PCDrv::Initialize();

  UpdateGTEAspectRatio();
  UpdateThrottlePeriod();
  UpdateMemorySaveStateSettings();

  if (!IsReplayingGPUDump())
  {
    Cheats::ReloadCheats(true, true, false, true, true);
    if (Cheats::HasAnySettingOverrides())
      ApplySettings(true);
  }

  PerformanceCounters::Clear();

  return true;
}

void System::DestroySystem()
{
  DebugAssert(!s_state.system_executing);
  if (s_state.state == State::Shutdown)
    return;

  if (s_state.media_capture)
    StopMediaCapture();

  s_state.gpu_dump_player.reset();

  s_state.undo_load_state.reset();

#ifdef ENABLE_GDB_SERVER
  GDBServer::Shutdown();
#endif

  GPUThread::RunOnThread([]() {
    GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::SystemPaused, false);
    Host::ClearOSDMessages(true);
  });

  InputManager::PauseVibration();
  InputManager::UpdateHostMouseMode();

  if (g_settings.inhibit_screensaver)
    PlatformMisc::ResumeScreensaver();

  FreeMemoryStateStorage(true, true, false);

  Cheats::UnloadAll();
  PCDrv::Shutdown();
  SIO::Shutdown();
  MDEC::Shutdown();
  SPU::Shutdown();
  Timers::Shutdown();
  Pad::Shutdown();
  CDROM::Shutdown();
  g_gpu.Shutdown();
  DMA::Shutdown();
  PIO::Shutdown();
  CPU::CodeCache::Shutdown();
  CPU::PGXP::Shutdown();
  CPU::Shutdown();
  Bus::Shutdown();
  TimingEvents::Shutdown();
  ClearRunningGame();
  GPUThread::DestroyGPUBackend();

  s_state.taints = 0;
  s_state.bios_hash = {};
  s_state.bios_image_info = nullptr;
  s_state.exe_override = {};
  s_state.boot_mode = BootMode::None;

  s_state.state = State::Shutdown;
  std::atomic_thread_fence(std::memory_order_release);

  // NOTE: Must come after DestroyGPUBackend(), otherwise landing page will display.
  FullscreenUI::OnSystemDestroyed();

  Host::OnSystemDestroyed();
}

void System::AbnormalShutdown(const std::string_view reason)
{
  if (!IsValid())
    return;

  ERROR_LOG("Abnormal shutdown: {}", reason);

  Host::OnSystemAbnormalShutdown(reason);

  // Immediately switch to destroying and exit execution to get out of here.
  s_state.state = State::Stopping;
  std::atomic_thread_fence(std::memory_order_release);
  if (s_state.system_executing)
    InterruptExecution();
  else
    DestroySystem();
}

void System::ClearRunningGame()
{
  UpdateSessionTime(s_state.running_game_serial);

  s_state.running_game_serial.clear();
  s_state.running_game_path.clear();
  s_state.running_game_title.clear();
  s_state.running_game_entry = nullptr;
  s_state.running_game_hash = 0;

  Host::OnGameChanged(s_state.running_game_path, s_state.running_game_serial, s_state.running_game_title,
                      s_state.running_game_hash);

  Achievements::GameChanged(s_state.running_game_path, nullptr, false);

  UpdateRichPresence(true);
}

void System::Execute()
{
  for (;;)
  {
    switch (s_state.state)
    {
      case State::Running:
      {
        s_state.system_executing = true;

        TimingEvents::CommitLeftoverTicks();

        if (s_state.gpu_dump_player) [[unlikely]]
          s_state.gpu_dump_player->Execute();
        else if (s_state.rewind_load_counter >= 0)
          DoRewind();
        else
          CPU::Execute();

        s_state.system_executing = false;
        continue;
      }

      case State::Stopping:
      {
        DestroySystem();
        return;
      }

      case State::Paused:
      default:
        return;
    }
  }
}

void System::FrameDone()
{
  // Generate any pending samples from the SPU before sleeping, this way we reduce the chances of underruns.
  // TODO: when running ahead, we can skip this (and the flush above)
  if (!IsReplayingGPUDump()) [[likely]]
  {
    SPU::GeneratePendingSamples();

    Cheats::ApplyFrameEndCodes();

    if (Achievements::IsActive())
      Achievements::FrameUpdate();
  }

#ifdef ENABLE_DISCORD_PRESENCE
  PollDiscordPresence();
#endif

#ifdef ENABLE_SOCKET_MULTIPLEXER
  if (s_state.socket_multiplexer)
    s_state.socket_multiplexer->PollEventsWithTimeout(0);
#endif

  // Save states for rewind and runahead.
  if (s_state.rewind_save_counter >= 0)
  {
    if (s_state.rewind_save_counter == 0)
    {
      SaveMemoryState(AllocateMemoryState());
      s_state.rewind_save_counter = s_state.rewind_save_frequency;
    }
    else
    {
      s_state.rewind_save_counter--;
    }
  }
  else if (s_state.runahead_frames > 0)
  {
    // We don't want to poll during replay, because otherwise we'll lose frames.
    if (s_state.runahead_replay_frames == 0)
    {
      // For runahead, poll input early, that way we can use the remainder of this frame to replay.
      // *technically* this means higher input latency (by less than a frame), but runahead itself
      // counter-acts that.
      Host::PumpMessagesOnCPUThread();
      InputManager::PollSources();
      CheckForAndExitExecution();
    }

    if (DoRunahead())
    {
      // running ahead, get it done as soon as possible
      return;
    }

    // Late submission of frame. This is needed because the input poll can determine whether we need to rewind.
    g_gpu.QueuePresentCurrentFrame();

    SaveMemoryState(AllocateMemoryState());
  }

  Timer::Value current_time = Timer::GetCurrentValue();
  GTE::UpdateFreecam(current_time);

  // Frame step after runahead, otherwise the pause takes precedence and the replay never happens.
  if (s_state.frame_step_request)
  {
    s_state.frame_step_request = false;
    PauseSystem(true);
  }

  // pre-frame sleep accounting (input lag reduction)
  const Timer::Value pre_frame_sleep_until = s_state.next_frame_time + s_state.pre_frame_sleep_time;
  s_state.last_active_frame_time = current_time - s_state.frame_start_time;
  if (s_state.pre_frame_sleep)
    AccumulatePreFrameSleepTime(current_time);

  // pre-frame sleep (input lag reduction)
  // if we're running over, then fall through to the normal Throttle() case which will fix up next_frame_time.
  if (s_state.pre_frame_sleep && pre_frame_sleep_until > current_time)
  {
    // don't sleep if it's under 1ms, because we're just going to overshoot (or spin).
    if (Timer::ConvertValueToMilliseconds(pre_frame_sleep_until - current_time) >= 1)
    {
      Throttle(current_time, pre_frame_sleep_until);
      current_time = Timer::GetCurrentValue();
    }
    else
    {
      // still need to update next_frame_time
      s_state.next_frame_time += s_state.frame_period;
    }
  }
  else
  {
    if (s_state.throttler_enabled)
      Throttle(current_time, s_state.next_frame_time);
  }

  s_state.frame_start_time = current_time;

  // Input poll already done above
  if (s_state.runahead_frames == 0)
  {
    Host::PumpMessagesOnCPUThread();
    InputManager::PollSources();
    CheckForAndExitExecution();
  }

  // Update input OSD if we're running
  if (g_settings.display_show_inputs)
    ImGuiManager::UpdateInputOverlay();
}

bool System::GetFramePresentationParameters(GPUBackendFramePresentationParameters* frame)
{
  const Timer::Value current_time = Timer::GetCurrentValue();

  frame->frame_number = s_state.frame_number;
  frame->internal_frame_number = s_state.internal_frame_number;

  // explicit present (frame pacing)
  const bool is_unique_frame = (s_state.last_presented_internal_frame_number != s_state.internal_frame_number);
  s_state.last_presented_internal_frame_number = s_state.internal_frame_number;

  const bool is_duplicate_frame = (s_state.skip_presenting_duplicate_frames && !is_unique_frame &&
                                   s_state.skipped_frame_count < MAX_SKIPPED_DUPLICATE_FRAME_COUNT);
  const bool skip_this_frame =
    ((is_duplicate_frame || (!s_state.optimal_frame_pacing && current_time > s_state.next_frame_time &&
                             s_state.skipped_frame_count < MAX_SKIPPED_TIMEOUT_FRAME_COUNT)) &&
     !IsExecutionInterrupted());
  const bool should_allow_present_skip = IsRunningAtNonStandardSpeed();
  frame->update_performance_counters = !is_duplicate_frame;
  frame->present_frame = !skip_this_frame;
  frame->allow_present_skip = should_allow_present_skip;
  frame->present_time = (s_state.optimal_frame_pacing && s_state.throttler_enabled && !IsExecutionInterrupted()) ?
                          s_state.next_frame_time :
                          0;

  // Video capture setup.
  frame->media_capture = nullptr;
  if (MediaCapture* cap = s_state.media_capture.get(); cap && cap->IsCapturingVideo())
  {
    frame->media_capture = cap;

    if (cap->GetVideoFPS() != s_state.video_frame_rate) [[unlikely]]
    {
      const std::string next_capture_path = cap->GetNextCapturePath();
      const u32 video_width = cap->GetVideoWidth();
      const u32 video_height = cap->GetVideoHeight();
      INFO_LOG("Video frame rate changed, switching to new capture file {}", Path::GetFileName(next_capture_path));

      const bool was_capturing_audio = cap->IsCapturingAudio();
      StopMediaCapture();
      StartMediaCapture(std::move(next_capture_path), true, was_capturing_audio, video_width, video_height);
      frame->media_capture = s_state.media_capture.get();
    }
  }

  if (!skip_this_frame)
  {
    s_state.skipped_frame_count = 0;
  }
  else
  {
    DEBUG_LOG("Skipping displaying frame");
    s_state.skipped_frame_count++;
  }

  // Still need to submit frame if we're capturing, even if it's a dupe.
  return (!is_duplicate_frame || frame->media_capture);
}

float System::GetVideoFrameRate()
{
  return s_state.video_frame_rate;
}

void System::SetVideoFrameRate(float frequency)
{
  if (s_state.video_frame_rate == frequency)
    return;

  s_state.video_frame_rate = frequency;
  UpdateThrottlePeriod();
}

void System::UpdateThrottlePeriod()
{
  if (s_state.target_speed > std::numeric_limits<double>::epsilon())
  {
    const double target_speed =
      std::max(static_cast<double>(s_state.target_speed), std::numeric_limits<double>::epsilon());
    s_state.frame_period =
      Timer::ConvertSecondsToValue(1.0 / (static_cast<double>(s_state.video_frame_rate) * target_speed));
  }
  else
  {
    s_state.frame_period = 1;
  }

  ResetThrottler();
}

void System::ResetThrottler()
{
  s_state.next_frame_time = Timer::GetCurrentValue() + s_state.frame_period;
  s_state.pre_frame_sleep_time = 0;
}

void System::Throttle(Timer::Value current_time, Timer::Value sleep_until)
{
  // If we're running too slow, advance the next frame time based on the time we lost. Effectively skips
  // running those frames at the intended time, because otherwise if we pause in the debugger, we'll run
  // hundreds of frames when we resume.
  if (current_time > sleep_until)
  {
    const Timer::Value diff = static_cast<s64>(current_time) - static_cast<s64>(s_state.next_frame_time);
    s_state.next_frame_time += (diff / s_state.frame_period) * s_state.frame_period + s_state.frame_period;
    return;
  }

#ifdef ENABLE_SOCKET_MULTIPLEXER
  // If we are using the socket multiplier, and have clients, then use it to sleep instead.
  // That way in a query->response->query->response chain, we don't process only one message per frame.
  if (s_state.socket_multiplexer && s_state.socket_multiplexer->HasAnyClientSockets())
  {
    Timer::Value poll_start_time = current_time;
    for (;;)
    {
      const u32 sleep_ms = static_cast<u32>(Timer::ConvertValueToMilliseconds(sleep_until - poll_start_time));
      s_state.socket_multiplexer->PollEventsWithTimeout(sleep_ms);
      poll_start_time = Timer::GetCurrentValue();
      if (poll_start_time >= sleep_until || (!g_settings.display_optimal_frame_pacing && sleep_ms == 0))
        break;
    }
  }
  else
  {
    // Use a spinwait if we undersleep for all platforms except android.. don't want to burn battery.
    // Linux also seems to do a much better job of waking up at the requested time.
#if !defined(__linux__)
    Timer::SleepUntil(sleep_until, g_settings.display_optimal_frame_pacing);
#else
    Timer::SleepUntil(sleep_until, false);
#endif
  }
#else
  // No spinwait on Android, see above.
  Timer::SleepUntil(sleep_until, false);
#endif

#if 0
  const Timer::Value time_after_sleep = Timer::GetCurrentValue();
  DEV_LOG("Asked for {:.2f} ms, slept for {:.2f} ms, {:.2f} ms {}",
          Timer::ConvertValueToMilliseconds(s_state.next_frame_time - current_time),
          Timer::ConvertValueToMilliseconds(time_after_sleep - current_time),
          Timer::ConvertValueToMilliseconds((time_after_sleep < s_state.next_frame_time) ?
                                              (s_state.next_frame_time - time_after_sleep) :
                                              (time_after_sleep - s_state.next_frame_time)),
          (time_after_sleep < s_state.next_frame_time) ? "early" : "late");
#endif

  s_state.next_frame_time += s_state.frame_period;
}

void System::SingleStepCPU()
{
  CPU::SetSingleStepFlag();

  // If this gets called when the system is executing, we're not going to end up here..
  if (IsPaused())
    PauseSystem(false);
}

void System::IncrementFrameNumber()
{
  s_state.frame_number++;
}

void System::IncrementInternalFrameNumber()
{
  if (IsFastForwardingBoot()) [[unlikely]]
  {
    // Need to turn off present throttle.
    s_state.internal_frame_number++;
    UpdateSpeedLimiterState();
    return;
  }

  s_state.internal_frame_number++;
}

bool System::DoState(StateWrapper& sw, bool update_display)
{
  if (!sw.DoMarker("System"))
    return false;

  if (sw.GetVersion() < 74) [[unlikely]]
  {
    u32 region32 = static_cast<u32>(s_state.region);
    sw.Do(&region32);
    s_state.region = static_cast<ConsoleRegion>(region32);
  }
  else
  {
    sw.Do(&s_state.region);
  }

  u32 state_taints = s_state.taints;
  sw.DoEx(&state_taints, 75, static_cast<u32>(0));
  if (state_taints != s_state.taints) [[unlikely]]
  {
    WarnAboutStateTaints(state_taints);
    s_state.taints |= state_taints;
  }

  sw.Do(&s_state.frame_number);
  sw.Do(&s_state.internal_frame_number);

  BIOS::ImageInfo::Hash bios_hash = s_state.bios_hash;
  sw.DoBytesEx(bios_hash.data(), BIOS::ImageInfo::HASH_SIZE, 58, s_state.bios_hash.data());
  if (bios_hash != s_state.bios_hash)
  {
    WARNING_LOG("BIOS hash mismatch: System: {} | State: {}", BIOS::ImageInfo::GetHashString(s_state.bios_hash),
                BIOS::ImageInfo::GetHashString(bios_hash));
    Host::AddIconOSDWarning(
      "StateBIOSMismatch", ICON_FA_EXCLAMATION_TRIANGLE,
      TRANSLATE_STR("System", "This save state was created with a different BIOS. This may cause stability issues."),
      Host::OSD_WARNING_DURATION);
  }

  if (!sw.DoMarker("CPU") || !CPU::DoState(sw))
    return false;

  if (sw.IsReading())
  {
    CPU::CodeCache::Reset();
    if (g_settings.gpu_pgxp_enable)
      CPU::PGXP::Reset();
  }

  if (!sw.DoMarker("Bus") || !Bus::DoState(sw))
    return false;

  if (!sw.DoMarker("DMA") || !DMA::DoState(sw))
    return false;

  if (!sw.DoMarker("InterruptController") || !InterruptController::DoState(sw))
    return false;

  if (!sw.DoMarker("GPU") || !g_gpu.DoState(sw))
    return false;

  if (!sw.DoMarker("CDROM") || !CDROM::DoState(sw))
    return false;

  if (!sw.DoMarker("Pad") || !Pad::DoState(sw, false))
    return false;

  if (!sw.DoMarker("Timers") || !Timers::DoState(sw))
    return false;

  if (!sw.DoMarker("SPU") || !SPU::DoState(sw))
    return false;

  if (!sw.DoMarker("MDEC") || !MDEC::DoState(sw))
    return false;

  if (!sw.DoMarker("SIO") || !SIO::DoState(sw))
    return false;

  if (sw.GetVersion() >= 77)
  {
    if (!sw.DoMarker("PIO") || !PIO::DoState(sw))
      return false;
  }

  if (!sw.DoMarker("Events") || !TimingEvents::DoState(sw))
    return false;

  if (!sw.DoMarker("Overclock"))
    return false;

  bool cpu_overclock_active = g_settings.cpu_overclock_active;
  u32 cpu_overclock_numerator = g_settings.cpu_overclock_numerator;
  u32 cpu_overclock_denominator = g_settings.cpu_overclock_denominator;
  sw.Do(&cpu_overclock_active);
  sw.Do(&cpu_overclock_numerator);
  sw.Do(&cpu_overclock_denominator);

  if (sw.IsReading() && (cpu_overclock_active != g_settings.cpu_overclock_active ||
                         (cpu_overclock_active && (g_settings.cpu_overclock_numerator != cpu_overclock_numerator ||
                                                   g_settings.cpu_overclock_denominator != cpu_overclock_denominator))))
  {
    Host::AddIconOSDMessage(
      "StateOverclockDifference", ICON_FA_EXCLAMATION_TRIANGLE,
      fmt::format(TRANSLATE_FS("System", "WARNING: CPU overclock ({}%) was different in save state ({}%)."),
                  g_settings.cpu_overclock_enable ? g_settings.GetCPUOverclockPercent() : 100u,
                  cpu_overclock_active ?
                    Settings::CPUOverclockFractionToPercent(cpu_overclock_numerator, cpu_overclock_denominator) :
                    100u),
      Host::OSD_WARNING_DURATION);
    UpdateOverclock();
  }

  if (!sw.DoMarkerEx("Cheevos", 56) || !Achievements::DoState(sw))
    return false;

  if (sw.HasError())
    return false;

  // If we're paused, need to update the display FB.
  if (update_display)
    g_gpu.UpdateDisplay(false);

  return true;
}

System::MemorySaveState& System::AllocateMemoryState()
{
  const u32 max_count = static_cast<u32>(s_state.memory_save_states.size());
  DebugAssert(s_state.memory_save_state_count <= max_count);
  if (s_state.memory_save_state_count < max_count)
    s_state.memory_save_state_count++;

  MemorySaveState& ret = s_state.memory_save_states[s_state.memory_save_state_front];
  s_state.memory_save_state_front = (s_state.memory_save_state_front + 1) % max_count;
  return ret;
}

System::MemorySaveState& System::GetFirstMemoryState()
{
  const u32 max_count = static_cast<u32>(s_state.memory_save_states.size());
  DebugAssert(s_state.memory_save_state_count > 0);

  const s32 front =
    static_cast<s32>(s_state.memory_save_state_front) - static_cast<s32>(s_state.memory_save_state_count);
  const u32 idx = static_cast<u32>((front < 0) ? (front + static_cast<s32>(max_count)) : front);
  return s_state.memory_save_states[idx];
}

System::MemorySaveState& System::PopMemoryState()
{
  const u32 max_count = static_cast<u32>(s_state.memory_save_states.size());
  DebugAssert(s_state.memory_save_state_count > 0);
  s_state.memory_save_state_count--;

  const s32 front = static_cast<s32>(s_state.memory_save_state_front) - 1;
  s_state.memory_save_state_front = static_cast<u32>((front < 0) ? (front + static_cast<s32>(max_count)) : front);
  return s_state.memory_save_states[s_state.memory_save_state_front];
}

bool System::AllocateMemoryStates(size_t state_count, bool recycle_old_textures)
{
  DEV_LOG("Allocating {} memory save state slots", state_count);

  if (state_count != s_state.memory_save_states.size())
  {
    FreeMemoryStateStorage(true, true, recycle_old_textures);
    s_state.memory_save_states.resize(state_count);
  }

  // Allocate CPU buffers.
  // TODO: Maybe look at host memory limits here...
  const size_t size = GetMaxSaveStateSize();
  for (MemorySaveState& mss : s_state.memory_save_states)
  {
    mss.state_size = 0;
    if (mss.state_data.size() != size)
      mss.state_data.resize(size);
  }

  // Allocate GPU buffers.
  Error error;
  if (!GPUBackend::AllocateMemorySaveStates(s_state.memory_save_states, &error))
  {
    ERROR_LOG("Failed to allocate {} memory save states: {}", s_state.memory_save_states.size(),
              error.GetDescription());
    ERROR_LOG("Disabling runahead/rewind.");
    FreeMemoryStateStorage(true, true, false);
    s_state.runahead_frames = 0;
    s_state.memory_save_state_front = 0;
    s_state.memory_save_state_count = 0;
    s_state.rewind_load_frequency = -1;
    s_state.rewind_load_counter = -1;
    s_state.rewind_save_frequency = -1;
    s_state.rewind_save_counter = -1;
    return false;
  }

  return true;
}

void System::ClearMemorySaveStates(bool reallocate_resources, bool recycle_textures)
{
  s_state.memory_save_state_front = 0;
  s_state.memory_save_state_count = 0;

  if (reallocate_resources && !s_state.memory_save_states.empty())
  {
    if (!AllocateMemoryStates(s_state.memory_save_states.size(), recycle_textures))
      return;
  }

  // immediately save a rewind state next frame
  s_state.rewind_save_counter = (s_state.rewind_save_frequency > 0) ? 0 : -1;
}

void System::FreeMemoryStateStorage(bool release_memory, bool release_textures, bool recycle_textures)
{
  if (release_memory || release_textures)
  {
    // TODO: use non-copyable function, that way we don't need to store raw pointers
    std::vector<GPUTexture*> textures;
    bool gpu_thread_synced = false;

    for (MemorySaveState& mss : s_state.memory_save_states)
    {
      if ((mss.vram_texture || !mss.gpu_state_data.empty()) && !gpu_thread_synced)
      {
        gpu_thread_synced = true;
        GPUThread::SyncGPUThread(true);
      }

      if (mss.vram_texture)
      {
        if (textures.empty())
          textures.reserve(s_state.memory_save_states.size());

        textures.push_back(mss.vram_texture.release());
      }

      mss.gpu_state_data.deallocate();
      mss.gpu_state_size = 0;
      mss.state_data.deallocate();
      mss.state_size = 0;
    }

    if (!textures.empty())
    {
      GPUThread::RunOnThread([textures = std::move(textures), recycle_textures]() mutable {
        for (GPUTexture* texture : textures)
        {
          if (recycle_textures)
            g_gpu_device->RecycleTexture(std::unique_ptr<GPUTexture>(texture));
          else
            delete texture;
        }
      });
    }
  }

  if (release_memory)
  {
    s_state.memory_save_states = std::vector<MemorySaveState>();
    s_state.memory_save_state_front = 0;
    s_state.memory_save_state_count = 0;
  }
}

void System::LoadMemoryState(MemorySaveState& mss, bool update_display)
{
#ifdef PROFILE_MEMORY_SAVE_STATES
  Timer load_timer;
#endif

  StateWrapper sw(mss.state_data.cspan(0, mss.state_size), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
  DoMemoryState(sw, mss, update_display);
  DebugAssert(!sw.HasError());

#ifdef PROFILE_MEMORY_SAVE_STATES
  DEV_LOG("Loaded frame {} from memory state slot {} took {:.4f} ms", s_state.frame_number,
          &mss - s_state.memory_save_states.data(), load_timer.GetTimeMilliseconds());
#else
  DEBUG_LOG("Loaded frame {} from memory state slot {}", s_state.frame_number,
            &mss - s_state.memory_save_states.data());
#endif
}

void System::SaveMemoryState(MemorySaveState& mss)
{
#ifdef PROFILE_MEMORY_SAVE_STATES
  Timer save_timer;
#endif

  StateWrapper sw(mss.state_data.span(), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  DoMemoryState(sw, mss, false);
  DebugAssert(!sw.HasError());
  mss.state_size = sw.GetPosition();

#ifdef PROFILE_MEMORY_SAVE_STATES
  DEV_LOG("Saving frame {} to memory state slot {} took {} bytes and {:.4f} ms", s_state.frame_number,
          &mss - s_state.memory_save_states.data(), mss.state_size, save_timer.GetTimeMilliseconds());
#else
  DEBUG_LOG("Saving frame {} to memory state slot {}", s_state.frame_number, &mss - s_state.memory_save_states.data());
#endif
}

void System::DoMemoryState(StateWrapper& sw, MemorySaveState& mss, bool update_display)
{
#if defined(_DEBUG) || defined(_DEVEL)
#define SAVE_COMPONENT(name, expr)                                                                                     \
  do                                                                                                                   \
  {                                                                                                                    \
    Assert(sw.DoMarker(name));                                                                                         \
    if (!(expr)) [[unlikely]]                                                                                          \
      Panic("Failed to memory save " name);                                                                            \
  } while (0)
#else
#define SAVE_COMPONENT(name, expr) expr
#endif

  sw.Do(&s_state.frame_number);
  sw.Do(&s_state.internal_frame_number);

  SAVE_COMPONENT("CPU", CPU::DoState(sw));

  // don't need to reset pgxp because the value checks will save us from broken rendering, and it
  // saves using imprecise values for a frame in 30fps games.
  // TODO: Save PGXP state to memory state instead. It'll be 8MB, but potentially worth it.
  if (sw.IsReading())
    CPU::CodeCache::InvalidateAllRAMBlocks();

  SAVE_COMPONENT("Bus", Bus::DoState(sw));
  SAVE_COMPONENT("DMA", DMA::DoState(sw));
  SAVE_COMPONENT("InterruptController", InterruptController::DoState(sw));

  g_gpu.DoMemoryState(sw, mss);

  SAVE_COMPONENT("CDROM", CDROM::DoState(sw));
  SAVE_COMPONENT("Pad", Pad::DoState(sw, true));
  SAVE_COMPONENT("Timers", Timers::DoState(sw));
  SAVE_COMPONENT("SPU", SPU::DoState(sw));
  SAVE_COMPONENT("MDEC", MDEC::DoState(sw));
  SAVE_COMPONENT("SIO", SIO::DoState(sw));
  SAVE_COMPONENT("Events", TimingEvents::DoState(sw));
  SAVE_COMPONENT("Achievements", Achievements::DoState(sw));

#undef SAVE_COMPONENT

  if (update_display)
    g_gpu.UpdateDisplay(false);
}

bool System::LoadBIOS(Error* error)
{
  std::optional<BIOS::Image> bios_image = BIOS::GetBIOSImage(s_state.region, error);
  if (!bios_image.has_value())
    return false;

  s_state.bios_image_info = bios_image->info;
  s_state.bios_hash = bios_image->hash;
  if (s_state.bios_image_info)
    INFO_LOG("Using BIOS: {}", s_state.bios_image_info->description);
  else
    WARNING_LOG("Using an unknown BIOS: {}", BIOS::ImageInfo::GetHashString(s_state.bios_hash));

  std::memcpy(Bus::g_bios, bios_image->data.data(), Bus::BIOS_SIZE);
  return true;
}

void System::InternalReset()
{
  if (IsShutdown())
    return;

  // reset and clear taints
  SetTaintsFromSettings();

  TimingEvents::Reset();
  CPU::Reset();
  CPU::CodeCache::Reset();
  if (g_settings.gpu_pgxp_enable)
    CPU::PGXP::Reset();

  Bus::Reset();
  PIO::Reset();
  DMA::Reset();
  InterruptController::Reset();
  g_gpu.Reset(true);
  CDROM::Reset();
  Pad::Reset();
  Timers::Reset();
  SPU::Reset();
  MDEC::Reset();
  SIO::Reset();
  PCDrv::Reset();
  Achievements::Reset();
  s_state.frame_number = 1;
  s_state.internal_frame_number = 0;
}

bool System::SetBootMode(BootMode new_boot_mode, DiscRegion disc_region, Error* error)
{
  // Can we actually fast boot? If starting, s_bios_image_info won't be valid.
  const bool can_fast_boot =
    ((disc_region != DiscRegion::NonPS1) &&
     (s_state.state == State::Starting || (s_state.bios_image_info && s_state.bios_image_info->SupportsFastBoot())));
  const System::BootMode actual_new_boot_mode =
    (new_boot_mode == BootMode::FastBoot || (new_boot_mode == BootMode::FullBoot && s_state.bios_image_info &&
                                             !s_state.bios_image_info->CanSlowBootDisc(disc_region))) ?
      (can_fast_boot ? BootMode::FastBoot : BootMode::FullBoot) :
      new_boot_mode;
  if (actual_new_boot_mode == s_state.boot_mode)
    return true;

  // Need to reload the BIOS to wipe out the patching.
  if (new_boot_mode != BootMode::ReplayGPUDump && !LoadBIOS(error))
    return false;

  // Handle the case of BIOSes not being able to full boot.
  s_state.boot_mode = (actual_new_boot_mode == BootMode::FullBoot && s_state.bios_image_info &&
                       !s_state.bios_image_info->CanSlowBootDisc(disc_region)) ?
                        BootMode::FastBoot :
                        actual_new_boot_mode;
  if (s_state.boot_mode == BootMode::FastBoot)
  {
    if (s_state.bios_image_info && s_state.bios_image_info->SupportsFastBoot())
    {
      // Patch BIOS, this sucks.
      INFO_LOG("Patching BIOS for fast boot.");
      if (!BIOS::PatchBIOSFastBoot(Bus::g_bios, Bus::BIOS_SIZE, s_state.bios_image_info->fastboot_patch))
        s_state.boot_mode = BootMode::FullBoot;
    }
    else
    {
      ERROR_LOG("Cannot fast boot, BIOS is incompatible.");
      s_state.boot_mode = BootMode::FullBoot;
    }
  }

  return true;
}

size_t System::GetMaxSaveStateSize()
{
  // 5 megabytes is sufficient for now, at the moment they're around 4.3MB, or 10.3MB with 8MB RAM enabled.
  static constexpr u32 MAX_2MB_SAVE_STATE_SIZE = 5 * 1024 * 1024;
  static constexpr u32 MAX_8MB_SAVE_STATE_SIZE = 11 * 1024 * 1024;
  const bool is_8mb_ram = (System::IsValid() ? (Bus::g_ram_size > Bus::RAM_2MB_SIZE) : g_settings.enable_8mb_ram);
  return is_8mb_ram ? MAX_8MB_SAVE_STATE_SIZE : MAX_2MB_SAVE_STATE_SIZE;
}

std::string System::GetMediaPathFromSaveState(const char* path)
{
  SaveStateBuffer buffer;
  auto fp = FileSystem::OpenManagedCFile(path, "rb", nullptr);
  if (fp)
    LoadStateBufferFromFile(&buffer, fp.get(), nullptr, false, true, false, false);

  return std::move(buffer.media_path);
}

bool System::LoadState(const char* path, Error* error, bool save_undo_state, bool force_update_display)
{
  if (!IsValid() || IsReplayingGPUDump())
  {
    Error::SetStringView(error, TRANSLATE_SV("System", "System is not in correct state."));
    return false;
  }

  if (Achievements::IsHardcoreModeActive())
  {
    Achievements::ConfirmHardcoreModeDisableAsync(
      TRANSLATE("Achievements", "Loading state"),
      [path = std::string(path), save_undo_state, force_update_display](bool approved) {
        if (approved)
          LoadState(path.c_str(), nullptr, save_undo_state, force_update_display);
      });
    return true;
  }

  FlushSaveStates();

  Timer load_timer;

  auto fp = FileSystem::OpenManagedCFile(path, "rb", error);
  if (!fp)
  {
    Error::AddPrefixFmt(error, "Failed to open '{}': ", Path::GetFileName(path));
    return false;
  }

  INFO_LOG("Loading state from '{}'...", path);

  Host::AddIconOSDMessage(
    "LoadState", ICON_EMOJI_OPEN_THE_FOLDER,
    fmt::format(TRANSLATE_FS("OSDMessage", "Loading state from '{}'..."), Path::GetFileName(path)),
    Host::OSD_QUICK_DURATION);

  if (save_undo_state)
    SaveUndoLoadState();

  SaveStateBuffer buffer;
  if (!LoadStateBufferFromFile(&buffer, fp.get(), error, false, true, false, true) ||
      !LoadStateFromBuffer(buffer, error, force_update_display || IsPaused()))
  {
    if (save_undo_state)
      UndoLoadState();

    return false;
  }

  VERBOSE_LOG("Loading state took {:.2f} msec", load_timer.GetTimeMilliseconds());
  return true;
}

bool System::LoadStateFromBuffer(const SaveStateBuffer& buffer, Error* error, bool update_display)
{
  Assert(IsValid());

  u32 media_subimage_index = (buffer.version >= 51) ? buffer.media_subimage_index : 0;
  if (!buffer.media_path.empty())
  {
    if (CDROM::HasMedia() && CDROM::GetMediaPath() == buffer.media_path &&
        CDROM::GetCurrentSubImage() == media_subimage_index)
    {
      INFO_LOG("Re-using same media '{}'", CDROM::GetMediaPath());
    }
    else
    {
      // needs new image
      Error local_error;
      std::unique_ptr<CDImage> new_disc =
        CDImage::Open(buffer.media_path.c_str(), g_settings.cdrom_load_image_patches, error ? error : &local_error);
      const DiscRegion new_disc_region =
        new_disc ? GameList::GetCustomRegionForPath(buffer.media_path).value_or(GetRegionForImage(new_disc.get())) :
                   DiscRegion::NonPS1;
      if (!new_disc ||
          (media_subimage_index != 0 && new_disc->HasSubImages() &&
           !new_disc->SwitchSubImage(media_subimage_index, error ? error : &local_error)) ||
          (UpdateRunningGame(buffer.media_path, new_disc.get(), false),
           !CDROM::InsertMedia(std::move(new_disc), new_disc_region, s_state.running_game_serial,
                               s_state.running_game_title, error ? error : &local_error)))
      {
        if (CDROM::HasMedia())
        {
          Host::AddOSDMessage(
            fmt::format(TRANSLATE_FS("OSDMessage", "Failed to open CD image from save state '{}': {}.\nUsing "
                                                   "existing image '{}', this may result in instability."),
                        buffer.media_path, error ? error->GetDescription() : local_error.GetDescription(),
                        Path::GetFileName(CDROM::GetMediaPath())),
            Host::OSD_CRITICAL_ERROR_DURATION);
        }
        else
        {
          Error::AddPrefixFmt(error, TRANSLATE_FS("System", "Failed to open CD image '{}' used by save state:\n"),
                              Path::GetFileName(buffer.media_path));
          return false;
        }
      }
      else if (g_settings.cdrom_load_image_to_ram)
      {
        CDROM::PrecacheMedia();
      }
    }
  }
  else
  {
    // state has no disc
    CDROM::RemoveMedia(false);
  }

  // ensure the correct card is loaded
  if (g_settings.HasAnyPerGameMemoryCards())
    UpdatePerGameMemoryCards();

  ClearMemorySaveStates(false, false);

  // Updating game/loading settings can turn on hardcore mode. Catch this.
  Achievements::DisableHardcoreMode();

  return LoadStateDataFromBuffer(buffer.state_data.cspan(0, buffer.state_size), buffer.version, error, update_display);
}

bool System::LoadStateDataFromBuffer(std::span<const u8> data, u32 version, Error* error, bool update_display)
{
  if (IsShutdown()) [[unlikely]]
  {
    Error::SetStringView(error, "System is invalid.");
    return 0;
  }

  StateWrapper sw(data, StateWrapper::Mode::Read, version);
  if (!DoState(sw, update_display))
  {
    Error::SetStringView(error, "Save state stream is corrupted.");
    return false;
  }

  InterruptExecution();

  PerformanceCounters::Reset();
  ResetThrottler();

  if (update_display)
    g_gpu.UpdateDisplay(true);

  return true;
}

bool System::LoadStateBufferFromFile(SaveStateBuffer* buffer, std::FILE* fp, Error* error, bool read_title,
                                     bool read_media_path, bool read_screenshot, bool read_data)
{
  const s64 file_size = FileSystem::FSize64(fp, error);
  if (file_size < 0)
    return false;

  DebugAssert(FileSystem::FTell64(fp) == 0);

  SAVE_STATE_HEADER header;
  if (std::fread(&header, sizeof(header), 1, fp) != 1 || header.magic != SAVE_STATE_MAGIC) [[unlikely]]
  {
    Error::SetErrno(error, "fread() for header failed: ", errno);
    return false;
  }

  if (header.version < SAVE_STATE_MINIMUM_VERSION)
  {
    Error::SetStringFmt(
      error, TRANSLATE_FS("System", "Save state is incompatible: minimum version is {0} but state is version {1}."),
      SAVE_STATE_MINIMUM_VERSION, header.version);
    return false;
  }

  if (header.version > SAVE_STATE_VERSION)
  {
    Error::SetStringFmt(
      error, TRANSLATE_FS("System", "Save state is incompatible: maximum version is {0} but state is version {1}."),
      SAVE_STATE_VERSION, header.version);
    return false;
  }

  // Validate offsets.
  if ((static_cast<s64>(header.offset_to_media_path) + header.media_path_length) > file_size ||
      (static_cast<s64>(header.offset_to_screenshot) + header.screenshot_compressed_size) > file_size ||
      header.screenshot_width >= 32768 || header.screenshot_height >= 32768 ||
      (static_cast<s64>(header.offset_to_data) + header.data_compressed_size) > file_size ||
      header.data_uncompressed_size > SAVE_STATE_HEADER::MAX_SAVE_STATE_SIZE) [[unlikely]]
  {
    Error::SetStringView(error, "Save state header is corrupted.");
    return false;
  }

  buffer->version = header.version;

  if (read_title)
  {
    buffer->title.assign(header.title, StringUtil::Strnlen(header.title, std::size(header.title)));
    buffer->serial.assign(header.serial, StringUtil::Strnlen(header.serial, std::size(header.serial)));
  }

  // Read media path.
  if (read_media_path)
  {
    buffer->media_path.resize(header.media_path_length);
    buffer->media_subimage_index = header.media_subimage_index;
    if (header.media_path_length > 0)
    {
      if (!FileSystem::FSeek64(fp, header.offset_to_media_path, SEEK_SET, error)) [[unlikely]]
        return false;

      if (std::fread(buffer->media_path.data(), buffer->media_path.length(), 1, fp) != 1) [[unlikely]]
      {
        Error::SetErrno(error, "fread() for media path failed: ", errno);
        return false;
      }
    }
  }

  // Read screenshot if requested.
  if (read_screenshot)
  {
    buffer->screenshot.Resize(header.screenshot_width, header.screenshot_height, ImageFormat::RGBA8, true);
    const u32 compressed_size =
      (header.version >= 69) ? header.screenshot_compressed_size : buffer->screenshot.GetStorageSize();
    const SAVE_STATE_HEADER::CompressionType compression_type =
      (header.version >= 69) ? static_cast<SAVE_STATE_HEADER::CompressionType>(header.screenshot_compression_type) :
                               SAVE_STATE_HEADER::CompressionType::None;
    if (!ReadAndDecompressStateData(fp, buffer->screenshot.GetPixelsSpan(), header.offset_to_screenshot,
                                    compressed_size, compression_type, error)) [[unlikely]]
    {
      return false;
    }
  }

  // Decompress state data.
  if (read_data)
  {
    buffer->state_data.resize(header.data_uncompressed_size);
    buffer->state_size = header.data_uncompressed_size;
    if (!ReadAndDecompressStateData(fp, buffer->state_data.span(), header.offset_to_data, header.data_compressed_size,
                                    static_cast<SAVE_STATE_HEADER::CompressionType>(header.data_compression_type),
                                    error)) [[unlikely]]
    {
      return false;
    }
  }

  return true;
}

bool System::ReadAndDecompressStateData(std::FILE* fp, std::span<u8> dst, u32 file_offset, u32 compressed_size,
                                        SAVE_STATE_HEADER::CompressionType method, Error* error)
{
  if (!FileSystem::FSeek64(fp, file_offset, SEEK_SET, error))
    return false;

  if (method == SAVE_STATE_HEADER::CompressionType::None)
  {
    // Feed through.
    if (std::fread(dst.data(), dst.size(), 1, fp) != 1) [[unlikely]]
    {
      Error::SetErrno(error, "fread() failed: ", errno);
      return false;
    }

    return true;
  }

  DynamicHeapArray<u8> compressed_data(compressed_size);
  if (std::fread(compressed_data.data(), compressed_data.size(), 1, fp) != 1)
  {
    Error::SetErrno(error, "fread() failed: ", errno);
    return false;
  }

  if (method == SAVE_STATE_HEADER::CompressionType::Deflate)
  {
    uLong source_len = compressed_size;
    uLong dest_len = static_cast<uLong>(dst.size());
    const int err = uncompress2(dst.data(), &dest_len, compressed_data.data(), &source_len);
    if (err != Z_OK) [[unlikely]]
    {
      Error::SetStringFmt(error, "uncompress2() failed: ", err);
      return false;
    }
    else if (dest_len < dst.size()) [[unlikely]]
    {
      Error::SetStringFmt(error, "Only decompressed {} of {} bytes", dest_len, dst.size());
      return false;
    }

    if (source_len < compressed_size) [[unlikely]]
      WARNING_LOG("Only consumed {} of {} compressed bytes", source_len, compressed_size);

    return true;
  }
  else if (method == SAVE_STATE_HEADER::CompressionType::Zstandard)
  {
    const size_t result = ZSTD_decompress(dst.data(), dst.size(), compressed_data.data(), compressed_size);
    if (ZSTD_isError(result)) [[unlikely]]
    {
      const char* errstr = ZSTD_getErrorString(ZSTD_getErrorCode(result));
      Error::SetStringFmt(error, "ZSTD_decompress() failed: {}", errstr ? errstr : "<unknown>");
      return false;
    }
    else if (result < dst.size())
    {
      Error::SetStringFmt(error, "Only decompressed {} of {} bytes", result, dst.size());
      return false;
    }

    return true;
  }
  else [[unlikely]]
  {
    Error::SetStringView(error, "Unknown method.");
    return false;
  }
}

bool System::SaveState(std::string path, Error* error, bool backup_existing_save, bool ignore_memcard_busy)
{
  if (!IsValid() || IsReplayingGPUDump())
  {
    Error::SetStringView(error, TRANSLATE_SV("System", "System is not in correct state."));
    return false;
  }
  else if (!ignore_memcard_busy && IsSavingMemoryCards())
  {
    Error::SetStringView(error, TRANSLATE_SV("System", "Cannot save state while memory card is being saved."));
    return false;
  }

  Timer save_timer;

  SaveStateBuffer buffer;
  if (!SaveStateToBuffer(&buffer, error, 256))
    return false;

  VERBOSE_LOG("Preparing state save took {:.2f} msec", save_timer.GetTimeMilliseconds());

  std::string osd_key = fmt::format("save_state_{}", path);
  Host::AddIconOSDMessage(osd_key, ICON_EMOJI_FLOPPY_DISK,
                          fmt::format(TRANSLATE_FS("System", "Saving state to '{}'."), Path::GetFileName(path)), 60.0f);

  // ensure multiple saves to the same path do not overlap
  FlushSaveStates();

  s_state.outstanding_save_state_tasks.fetch_add(1, std::memory_order_acq_rel);
  s_state.async_task_queue.SubmitTask([path = std::move(path), buffer = std::move(buffer), osd_key = std::move(osd_key),
                                       backup_existing_save, compression = g_settings.save_state_compression]() {
    INFO_LOG("Saving state to '{}'...", path);

    Error lerror;
    Timer lsave_timer;

    if (backup_existing_save && FileSystem::FileExists(path.c_str()))
    {
      const std::string backup_filename = Path::ReplaceExtension(path, "bak");
      if (!FileSystem::RenamePath(path.c_str(), backup_filename.c_str(), &lerror))
      {
        ERROR_LOG("Failed to rename save state backup '{}': {}", Path::GetFileName(backup_filename),
                  lerror.GetDescription());
      }
    }

    auto fp = FileSystem::CreateAtomicRenamedFile(path, &lerror);
    bool result = false;
    if (fp)
    {
      if (SaveStateBufferToFile(buffer, fp.get(), &lerror, compression))
        result = FileSystem::CommitAtomicRenamedFile(fp, &lerror);
      else
        FileSystem::DiscardAtomicRenamedFile(fp);
    }
    else
    {
      lerror.AddPrefixFmt("Cannot open '{}': ", Path::GetFileName(path));
    }

    VERBOSE_LOG("Saving state took {:.2f} msec", lsave_timer.GetTimeMilliseconds());

    s_state.outstanding_save_state_tasks.fetch_sub(1, std::memory_order_acq_rel);

    // don't display a resume state saved message in FSUI
    if (!IsValid())
      return;

    if (result)
    {
      Host::AddIconOSDMessage(std::move(osd_key), ICON_EMOJI_FLOPPY_DISK,
                              fmt::format(TRANSLATE_FS("System", "State saved to '{}'."), Path::GetFileName(path)),
                              Host::OSD_QUICK_DURATION);
    }
    else
    {
      Host::AddIconOSDMessage(std::move(osd_key), ICON_EMOJI_WARNING,
                              fmt::format(TRANSLATE_FS("System", "Failed to save state to '{0}':\n{1}"),
                                          Path::GetFileName(path), lerror.GetDescription()),
                              Host::OSD_ERROR_DURATION);
    }
  });

  return true;
}

void System::FlushSaveStates()
{
  while (s_state.outstanding_save_state_tasks.load(std::memory_order_acquire) > 0)
    WaitForAllAsyncTasks();
}

bool System::SaveStateToBuffer(SaveStateBuffer* buffer, Error* error, u32 screenshot_size /* = 256 */)
{
  buffer->title = s_state.running_game_title;
  buffer->serial = s_state.running_game_serial;
  buffer->version = SAVE_STATE_VERSION;
  buffer->media_subimage_index = 0;

  if (CDROM::HasMedia())
  {
    buffer->media_path = CDROM::GetMediaPath();
    buffer->media_subimage_index = CDROM::GetMedia()->HasSubImages() ? CDROM::GetMedia()->GetCurrentSubImage() : 0;
  }

  // save screenshot
  if (screenshot_size > 0)
  {
    Error screenshot_error;
    if (GPUBackend::RenderScreenshotToBuffer(screenshot_size, screenshot_size, false, true, &buffer->screenshot,
                                             &screenshot_error))
    {
      if (g_gpu_device->UsesLowerLeftOrigin())
        buffer->screenshot.FlipY();

      // Ensure it's RGBA8.
      if (buffer->screenshot.GetFormat() != ImageFormat::RGBA8)
      {
        std::optional<Image> screenshot_rgba8 = buffer->screenshot.ConvertToRGBA8(&screenshot_error);
        if (!screenshot_rgba8.has_value())
        {
          ERROR_LOG("Failed to convert {} screenshot to RGBA8: {}",
                    Image::GetFormatName(buffer->screenshot.GetFormat()), screenshot_error.GetDescription());
          buffer->screenshot.Invalidate();
        }
        else
        {
          buffer->screenshot = std::move(screenshot_rgba8.value());
        }
      }
    }
    else
    {
      WARNING_LOG("Failed to save {}x{} screenshot for save state: {}", screenshot_size, screenshot_size,
                  screenshot_error.GetDescription());
    }
  }

  // write data
  if (buffer->state_data.empty())
    buffer->state_data.resize(GetMaxSaveStateSize());

  return SaveStateDataToBuffer(buffer->state_data, &buffer->state_size, error);
}

bool System::SaveStateDataToBuffer(std::span<u8> data, size_t* data_size, Error* error)
{
  if (IsShutdown()) [[unlikely]]
  {
    Error::SetStringView(error, "System is invalid.");
    return 0;
  }

  StateWrapper sw(data, StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  if (!DoState(sw, false))
  {
    Error::SetStringView(error, "DoState() failed");
    return false;
  }

  *data_size = sw.GetPosition();
  return true;
}

bool System::SaveStateBufferToFile(const SaveStateBuffer& buffer, std::FILE* fp, Error* error,
                                   SaveStateCompressionMode compression)
{
  // Header gets rewritten below.
  SAVE_STATE_HEADER header = {};
  header.magic = SAVE_STATE_MAGIC;
  header.version = SAVE_STATE_VERSION;
  StringUtil::Strlcpy(header.title, s_state.running_game_title.c_str(), sizeof(header.title));
  StringUtil::Strlcpy(header.serial, s_state.running_game_serial.c_str(), sizeof(header.serial));

  u32 file_position = 0;
  DebugAssert(FileSystem::FTell64(fp) == static_cast<s64>(file_position));
  if (std::fwrite(&header, sizeof(header), 1, fp) != 1)
  {
    Error::SetErrno(error, "fwrite() for header failed: ", errno);
    return false;
  }
  file_position += sizeof(header);

  if (!buffer.media_path.empty())
  {
    DebugAssert(FileSystem::FTell64(fp) == static_cast<s64>(file_position));
    header.media_path_length = static_cast<u32>(buffer.media_path.length());
    header.offset_to_media_path = file_position;
    header.media_subimage_index = buffer.media_subimage_index;
    if (std::fwrite(buffer.media_path.data(), buffer.media_path.length(), 1, fp) != 1)
    {
      Error::SetErrno(error, "fwrite() for media path failed: ", errno);
      return false;
    }
    file_position += static_cast<u32>(buffer.media_path.length());
  }

  if (buffer.screenshot.IsValid())
  {
    DebugAssert(FileSystem::FTell64(fp) == static_cast<s64>(file_position));
    header.screenshot_width = buffer.screenshot.GetWidth();
    header.screenshot_height = buffer.screenshot.GetHeight();
    header.offset_to_screenshot = file_position;
    header.screenshot_compressed_size =
      CompressAndWriteStateData(fp,
                                std::span<const u8>(reinterpret_cast<const u8*>(buffer.screenshot.GetPixels()),
                                                    buffer.screenshot.GetPitch() * buffer.screenshot.GetHeight()),
                                compression, &header.screenshot_compression_type, error);
    if (header.screenshot_compressed_size == 0)
      return false;
    file_position += header.screenshot_compressed_size;
  }

  DebugAssert(buffer.state_size > 0);
  header.offset_to_data = file_position;
  header.data_uncompressed_size = static_cast<u32>(buffer.state_size);
  header.data_compressed_size = CompressAndWriteStateData(fp, buffer.state_data.cspan(0, buffer.state_size),
                                                          compression, &header.data_compression_type, error);
  if (header.data_compressed_size == 0)
    return false;

  INFO_LOG("Save state compression: screenshot {} => {} bytes, data {} => {} bytes",
           buffer.screenshot.GetPitch() * buffer.screenshot.GetHeight(), header.screenshot_compressed_size,
           buffer.state_size, header.data_compressed_size);

  if (!FileSystem::FSeek64(fp, 0, SEEK_SET, error))
    return false;

  // re-write header
  if (std::fwrite(&header, sizeof(header), 1, fp) != 1 || std::fflush(fp) != 0)
  {
    Error::SetErrno(error, "fwrite()/fflush() to rewrite header failed: {}", errno);
    return false;
  }

  return true;
}

u32 System::CompressAndWriteStateData(std::FILE* fp, std::span<const u8> src, SaveStateCompressionMode method,
                                      u32* header_type, Error* error)
{
  if (method == SaveStateCompressionMode::Uncompressed)
  {
    if (std::fwrite(src.data(), src.size(), 1, fp) != 1) [[unlikely]]
    {
      Error::SetStringFmt(error, "fwrite() failed: {}", errno);
      return 0;
    }

    *header_type = static_cast<u32>(SAVE_STATE_HEADER::CompressionType::None);
    return static_cast<u32>(src.size());
  }

  DynamicHeapArray<u8> buffer;
  u32 write_size;
  if (method >= SaveStateCompressionMode::DeflateLow && method <= SaveStateCompressionMode::DeflateHigh)
  {
    const size_t buffer_size = compressBound(static_cast<uLong>(src.size()));
    buffer.resize(buffer_size);

    uLongf compressed_size = static_cast<uLongf>(buffer_size);
    const int level =
      ((method == SaveStateCompressionMode::DeflateLow) ?
         Z_BEST_SPEED :
         ((method == SaveStateCompressionMode::DeflateHigh) ? Z_BEST_COMPRESSION : Z_DEFAULT_COMPRESSION));
    const int err = compress2(buffer.data(), &compressed_size, src.data(), static_cast<uLong>(src.size()), level);
    if (err != Z_OK) [[unlikely]]
    {
      Error::SetStringFmt(error, "compress2() failed: {}", err);
      return 0;
    }

    *header_type = static_cast<u32>(SAVE_STATE_HEADER::CompressionType::Deflate);
    write_size = static_cast<u32>(compressed_size);
  }
  else if (method >= SaveStateCompressionMode::ZstLow && method <= SaveStateCompressionMode::ZstHigh)
  {
    const size_t buffer_size = ZSTD_compressBound(src.size());
    buffer.resize(buffer_size);

    const int level =
      ((method == SaveStateCompressionMode::ZstLow) ? 1 : ((method == SaveStateCompressionMode::ZstHigh) ? 18 : 0));
    const size_t compressed_size = ZSTD_compress(buffer.data(), buffer_size, src.data(), src.size(), level);
    if (ZSTD_isError(compressed_size)) [[unlikely]]
    {
      const char* errstr = ZSTD_getErrorString(ZSTD_getErrorCode(compressed_size));
      Error::SetStringFmt(error, "ZSTD_compress() failed: {}", errstr ? errstr : "<unknown>");
      return 0;
    }

    *header_type = static_cast<u32>(SAVE_STATE_HEADER::CompressionType::Zstandard);
    write_size = static_cast<u32>(compressed_size);
  }
  else [[unlikely]]
  {
    Error::SetStringView(error, "Unknown method.");
    return 0;
  }

  if (std::fwrite(buffer.data(), write_size, 1, fp) != 1) [[unlikely]]
  {
    Error::SetStringFmt(error, "fwrite() failed: {}", errno);
    return 0;
  }

  return write_size;
}

float System::GetTargetSpeed()
{
  return s_state.target_speed;
}

float System::GetAudioNominalRate()
{
  return s_state.throttler_enabled ? s_state.target_speed : 1.0f;
}

void System::AccumulatePreFrameSleepTime(Timer::Value current_time)
{
  DebugAssert(s_state.pre_frame_sleep);

  s_state.max_active_frame_time = std::max(s_state.max_active_frame_time, s_state.last_active_frame_time);

  // in case one frame runs over, adjust to compensate
  const Timer::Value max_sleep_time_for_this_frame =
    s_state.frame_period - std::min(s_state.last_active_frame_time, s_state.frame_period);
  if (max_sleep_time_for_this_frame < s_state.pre_frame_sleep_time)
  {
    s_state.pre_frame_sleep_time =
      Common::AlignDown(max_sleep_time_for_this_frame, static_cast<unsigned int>(Timer::ConvertMillisecondsToValue(1)));
    DEV_LOG("Adjust pre-frame time to {} ms due to overrun of {} ms",
            Timer::ConvertValueToMilliseconds(s_state.pre_frame_sleep_time),
            Timer::ConvertValueToMilliseconds(s_state.last_active_frame_time));
  }

  if (Timer::ConvertValueToSeconds(current_time - s_state.last_pre_frame_sleep_update_time) >=
      PRE_FRAME_SLEEP_UPDATE_INTERVAL)
  {
    s_state.last_pre_frame_sleep_update_time = current_time;

    const Timer::Value expected_frame_time =
      s_state.max_active_frame_time + Timer::ConvertMillisecondsToValue(g_settings.display_pre_frame_sleep_buffer);
    s_state.pre_frame_sleep_time =
      Common::AlignDown(s_state.frame_period - std::min(expected_frame_time, s_state.frame_period),
                        static_cast<unsigned int>(Timer::ConvertMillisecondsToValue(1)));
    DEV_LOG("Set pre-frame time to {} ms (expected frame time of {} ms)",
            Timer::ConvertValueToMilliseconds(s_state.pre_frame_sleep_time),
            Timer::ConvertValueToMilliseconds(expected_frame_time));

    s_state.max_active_frame_time = 0;
  }
}

void System::FormatLatencyStats(SmallStringBase& str)
{
  AudioStream* audio_stream = SPU::GetOutputStream();
  const u32 audio_latency =
    AudioStream::GetMSForBufferSize(audio_stream->GetSampleRate(), audio_stream->GetBufferedFramesRelaxed());

  const double active_frame_time = std::ceil(Timer::ConvertValueToMilliseconds(s_state.last_active_frame_time));
  const double pre_frame_time = std::ceil(Timer::ConvertValueToMilliseconds(s_state.pre_frame_sleep_time));
  const double input_latency = std::ceil(
    Timer::ConvertValueToMilliseconds(s_state.frame_period - s_state.pre_frame_sleep_time) -
    Timer::ConvertValueToMilliseconds(static_cast<Timer::Value>(s_state.runahead_frames) * s_state.frame_period));

  str.format("AL: {}ms | AF: {:.0f}ms | PF: {:.0f}ms | IL: {:.0f}ms | QF: {}", audio_latency, active_frame_time,
             pre_frame_time, input_latency, GPUBackend::GetQueuedFrameCount());
}

void System::UpdateSpeedLimiterState()
{
  DebugAssert(IsValid());

  s_state.target_speed = IsFastForwardingBoot() ?
                           0.0f :
                           (s_state.turbo_enabled ? g_settings.turbo_speed :
                                                    (s_state.fast_forward_enabled ? g_settings.fast_forward_speed :
                                                                                    g_settings.emulation_speed));
  s_state.throttler_enabled = (s_state.target_speed != 0.0f);
  s_state.optimal_frame_pacing = (s_state.throttler_enabled && g_settings.display_optimal_frame_pacing);
  s_state.skip_presenting_duplicate_frames =
    s_state.throttler_enabled && g_settings.display_skip_presenting_duplicate_frames;
  s_state.pre_frame_sleep = s_state.optimal_frame_pacing && g_settings.display_pre_frame_sleep;
  s_state.can_sync_to_host = false;
  s_state.syncing_to_host = false;

  if (g_settings.sync_to_host_refresh_rate)
  {
    if (const float host_refresh_rate = GPUThread::GetRenderWindowInfo().surface_refresh_rate; host_refresh_rate > 0.0f)
    {
      const float ratio = host_refresh_rate / s_state.video_frame_rate;
      s_state.can_sync_to_host = (ratio >= 0.95f && ratio <= 1.05f);
      INFO_LOG("Refresh rate: Host={}hz Guest={}hz Ratio={} - {}", host_refresh_rate, s_state.video_frame_rate, ratio,
               s_state.can_sync_to_host ? "can sync" : "can't sync");

      s_state.syncing_to_host =
        (s_state.can_sync_to_host && g_settings.sync_to_host_refresh_rate && s_state.target_speed == 1.0f);
      if (s_state.syncing_to_host)
        s_state.target_speed = ratio;
    }
  }

  VERBOSE_LOG("Target speed: {}%", s_state.target_speed * 100.0f);
  VERBOSE_LOG("Preset timing: {}", s_state.optimal_frame_pacing ? "consistent" : "immediate");

  // Update audio output.
  AudioStream* stream = SPU::GetOutputStream();
  stream->SetOutputVolume(GetAudioOutputVolume());
  stream->SetNominalRate(GetAudioNominalRate());

  UpdateThrottlePeriod();
  ResetThrottler();
  UpdateDisplayVSync();

#ifdef __APPLE__
  // To get any resemblence of consistent frame times on MacOS, we need to tell the scheduler how often we need to run.
  // Assume a maximum of 7ms for running a frame. It'll be much lower than that, Apple Silicon is fast.
  constexpr u64 MAX_FRAME_TIME_NS = 7000000;
  static u64 last_scheduler_period = 0;
  const u64 new_scheduler_period = s_state.optimal_frame_pacing ? s_state.frame_period : 0;
  if (new_scheduler_period != last_scheduler_period)
  {
    if (s_state.cpu_thread_handle)
    {
      s_state.cpu_thread_handle.SetTimeConstraints(s_state.optimal_frame_pacing, new_scheduler_period,
                                                   MAX_FRAME_TIME_NS, new_scheduler_period);
    }
    const Threading::ThreadHandle& gpu_thread = GPUThread::Internal::GetThreadHandle();
    if (gpu_thread)
    {
      gpu_thread.SetTimeConstraints(s_state.optimal_frame_pacing, new_scheduler_period, MAX_FRAME_TIME_NS,
                                    new_scheduler_period);
    }
  }
#endif
}

void System::UpdateDisplayVSync()
{
  // Avoid flipping vsync on and off by manually throttling when vsync is on.
  const GPUVSyncMode vsync_mode = GetEffectiveVSyncMode();
  const bool allow_present_throttle = ShouldAllowPresentThrottle();

  VERBOSE_LOG("VSync: {}{}", GPUDevice::VSyncModeToString(vsync_mode),
              allow_present_throttle ? " (present throttle allowed)" : "");

  GPUThread::SetVSync(vsync_mode, allow_present_throttle);
}

GPUVSyncMode System::GetEffectiveVSyncMode()
{
  // Vsync off => always disabled.
  if (!g_settings.display_vsync)
    return GPUVSyncMode::Disabled;

  // If there's no VM, or we're using vsync for timing, then we always use double-buffered (blocking).
  // Try to keep the same present mode whether we're running or not, since it'll avoid flicker.
  const bool valid_vm = (s_state.state != State::Shutdown && s_state.state != State::Stopping);
  if (s_state.can_sync_to_host || (!valid_vm && g_settings.sync_to_host_refresh_rate) ||
      g_settings.display_disable_mailbox_presentation)
  {
    return GPUVSyncMode::FIFO;
  }

  // For PAL games, we always want to triple buffer, because otherwise we'll be tearing.
  // Or for when we aren't using sync-to-host-refresh, to avoid dropping frames.
  // Allow present skipping when running outside of normal speed, if mailbox isn't supported.
  return GPUVSyncMode::Mailbox;
}

bool System::ShouldAllowPresentThrottle()
{
  const bool valid_vm = (s_state.state != State::Shutdown && s_state.state != State::Stopping);
  return !valid_vm || IsRunningAtNonStandardSpeed();
}

bool System::IsFastForwardEnabled()
{
  return s_state.fast_forward_enabled;
}

void System::SetFastForwardEnabled(bool enabled)
{
  if (!IsValid())
    return;

  s_state.fast_forward_enabled = enabled;
  UpdateSpeedLimiterState();
}

bool System::IsTurboEnabled()
{
  return s_state.turbo_enabled;
}

void System::SetTurboEnabled(bool enabled)
{
  if (!IsValid())
    return;

  s_state.turbo_enabled = enabled;
  UpdateSpeedLimiterState();
}

void System::SetRewindState(bool enabled)
{
  if (!System::IsValid())
    return;

  if (!g_settings.rewind_enable)
  {
    if (enabled)
      Host::AddKeyedOSDMessage("SetRewindState", TRANSLATE_STR("OSDMessage", "Rewinding is not enabled."), 5.0f);

    return;
  }

  if (Achievements::IsHardcoreModeActive() && enabled)
  {
    Achievements::ConfirmHardcoreModeDisableAsync("Rewinding", [](bool approved) {
      if (approved)
        SetRewindState(true);
    });
    return;
  }

  System::SetRewinding(enabled);
  UpdateSpeedLimiterState();
}

void System::DoFrameStep()
{
  if (!IsValid())
    return;

  if (Achievements::IsHardcoreModeActive())
  {
    Achievements::ConfirmHardcoreModeDisableAsync("Frame stepping", [](bool approved) {
      if (approved)
        DoFrameStep();
    });
    return;
  }

  s_state.frame_step_request = true;
  PauseSystem(false);
}

Controller* System::GetController(u32 slot)
{
  return Pad::GetController(slot);
}

void System::UpdateControllers()
{
  auto lock = Host::GetSettingsLock();

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Pad::SetController(i, nullptr);

    const ControllerType type = g_settings.controller_types[i];
    if (type != ControllerType::None)
    {
      std::unique_ptr<Controller> controller = Controller::Create(type, i);
      if (controller)
      {
        controller->LoadSettings(*Host::GetSettingsInterface(), Controller::GetSettingsSection(i).c_str(), true);
        Pad::SetController(i, std::move(controller));
      }
    }
  }
}

void System::UpdateControllerSettings()
{
  auto lock = Host::GetSettingsLock();

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = Pad::GetController(i);
    if (controller)
      controller->LoadSettings(*Host::GetSettingsInterface(), Controller::GetSettingsSection(i).c_str(), false);
  }
}

void System::ResetControllers()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = Pad::GetController(i);
    if (controller)
      controller->Reset();
  }
}

std::unique_ptr<MemoryCard> System::GetMemoryCardForSlot(u32 slot, MemoryCardType type)
{
  // Disable memory cards when running PSFs.
  const bool is_running_psf = !s_state.running_game_path.empty() && IsPsfPath(s_state.running_game_path.c_str());
  if (is_running_psf)
    return nullptr;

  std::string message_key = fmt::format("MemoryCard{}SharedWarning", slot);

  switch (type)
  {
    case MemoryCardType::PerGame:
    {
      if (s_state.running_game_serial.empty())
      {
        Host::AddIconOSDMessage(
          std::move(message_key), ICON_FA_SD_CARD,
          fmt::format(TRANSLATE_FS("System", "Per-game memory card cannot be used for slot {} as the running "
                                             "game has no code. Using shared card instead."),
                      slot + 1u),
          Host::OSD_INFO_DURATION);
        return MemoryCard::Open(g_settings.GetSharedMemoryCardPath(slot));
      }
      else
      {
        Host::RemoveKeyedOSDMessage(std::move(message_key));
        return MemoryCard::Open(g_settings.GetGameMemoryCardPath(s_state.running_game_serial.c_str(), slot));
      }
    }

    case MemoryCardType::PerGameTitle:
    {
      if (s_state.running_game_title.empty())
      {
        Host::AddIconOSDMessage(
          std::move(message_key), ICON_FA_SD_CARD,
          fmt::format(TRANSLATE_FS("System", "Per-game memory card cannot be used for slot {} as the running "
                                             "game has no title. Using shared card instead."),
                      slot + 1u),
          Host::OSD_INFO_DURATION);
        return MemoryCard::Open(g_settings.GetSharedMemoryCardPath(slot));
      }
      else
      {
        std::string card_path;

        // Playlist - use title if different.
        if (HasMediaSubImages() && s_state.running_game_entry &&
            s_state.running_game_title != s_state.running_game_entry->title)
        {
          card_path = g_settings.GetGameMemoryCardPath(Path::SanitizeFileName(s_state.running_game_title), slot);
        }
        // Multi-disc game - use disc set name.
        else if (s_state.running_game_entry && !s_state.running_game_entry->disc_set_name.empty())
        {
          card_path =
            g_settings.GetGameMemoryCardPath(Path::SanitizeFileName(s_state.running_game_entry->disc_set_name), slot);
        }

        // But prefer a disc-specific card if one already exists.
        std::string disc_card_path = g_settings.GetGameMemoryCardPath(
          Path::SanitizeFileName((s_state.running_game_entry && !s_state.running_game_custom_title) ?
                                   s_state.running_game_entry->title :
                                   s_state.running_game_title),
          slot);
        if (disc_card_path != card_path)
        {
          if (card_path.empty() || !g_settings.memory_card_use_playlist_title ||
              FileSystem::FileExists(disc_card_path.c_str()))
          {
            if (g_settings.memory_card_use_playlist_title && !card_path.empty())
            {
              Host::AddIconOSDMessage(
                fmt::format("DiscSpecificMC{}", slot), ICON_FA_SD_CARD,
                fmt::format(TRANSLATE_FS("System", "Using disc-specific memory card '{}' instead of per-game card."),
                            Path::GetFileName(disc_card_path)),
                Host::OSD_INFO_DURATION);
            }

            card_path = std::move(disc_card_path);
          }
        }

        Host::RemoveKeyedOSDMessage(std::move(message_key));
        return MemoryCard::Open(card_path.c_str());
      }
    }

    case MemoryCardType::PerGameFileTitle:
    {
      const std::string display_name(FileSystem::GetDisplayNameFromPath(s_state.running_game_path));
      const std::string_view file_title(Path::GetFileTitle(display_name));
      if (file_title.empty())
      {
        Host::AddIconOSDMessage(
          std::move(message_key), ICON_FA_SD_CARD,
          fmt::format(TRANSLATE_FS("System", "Per-game memory card cannot be used for slot {} as the running "
                                             "game has no path. Using shared card instead."),
                      slot + 1u));
        return MemoryCard::Open(g_settings.GetSharedMemoryCardPath(slot));
      }
      else
      {
        Host::RemoveKeyedOSDMessage(std::move(message_key));
        return MemoryCard::Open(g_settings.GetGameMemoryCardPath(Path::SanitizeFileName(file_title).c_str(), slot));
      }
    }

    case MemoryCardType::Shared:
    {
      Host::RemoveKeyedOSDMessage(std::move(message_key));
      return MemoryCard::Open(g_settings.GetSharedMemoryCardPath(slot));
    }

    case MemoryCardType::NonPersistent:
    {
      Host::RemoveKeyedOSDMessage(std::move(message_key));
      return MemoryCard::Create();
    }

    case MemoryCardType::None:
    default:
    {
      Host::RemoveKeyedOSDMessage(std::move(message_key));
      return nullptr;
    }
  }
}

void System::UpdateMemoryCardTypes()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Pad::SetMemoryCard(i, nullptr);

    const MemoryCardType type = g_settings.memory_card_types[i];
    std::unique_ptr<MemoryCard> card = GetMemoryCardForSlot(i, type);
    if (card)
    {
      if (const std::string& filename = card->GetFilename(); !filename.empty())
        INFO_LOG("Memory Card Slot {}: {}", i + 1, filename);

      Pad::SetMemoryCard(i, std::move(card));
    }
  }
}

void System::UpdatePerGameMemoryCards()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    const MemoryCardType type = g_settings.memory_card_types[i];
    if (!Settings::IsPerGameMemoryCardType(type))
      continue;

    Pad::SetMemoryCard(i, nullptr);

    std::unique_ptr<MemoryCard> card = GetMemoryCardForSlot(i, type);
    if (card)
    {
      if (const std::string& filename = card->GetFilename(); !filename.empty())
        INFO_LOG("Memory Card Slot {}: {}", i + 1, filename);

      Pad::SetMemoryCard(i, std::move(card));
    }
  }
}

bool System::HasMemoryCard(u32 slot)
{
  return (Pad::GetMemoryCard(slot) != nullptr);
}

bool System::IsSavingMemoryCards()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    MemoryCard* card = Pad::GetMemoryCard(i);
    if (card && card->IsOrWasRecentlyWriting())
      return true;
  }

  return false;
}

void System::SwapMemoryCards()
{
  if (!IsValid())
    return;

  std::unique_ptr<MemoryCard> first = Pad::RemoveMemoryCard(0);
  std::unique_ptr<MemoryCard> second = Pad::RemoveMemoryCard(1);
  Pad::SetMemoryCard(0, std::move(second));
  Pad::SetMemoryCard(1, std::move(first));

  if (HasMemoryCard(0) && HasMemoryCard(1))
  {
    Host::AddOSDMessage(TRANSLATE_STR("OSDMessage", "Swapped memory card ports. Both ports have a memory card."),
                        10.0f);
  }
  else if (HasMemoryCard(1))
  {
    Host::AddOSDMessage(
      TRANSLATE_STR("OSDMessage", "Swapped memory card ports. Port 2 has a memory card, Port 1 is empty."), 10.0f);
  }
  else if (HasMemoryCard(0))
  {
    Host::AddOSDMessage(
      TRANSLATE_STR("OSDMessage", "Swapped memory card ports. Port 1 has a memory card, Port 2 is empty."), 10.0f);
  }
  else
  {
    Host::AddOSDMessage(TRANSLATE_STR("OSDMessage", "Swapped memory card ports. Neither port has a memory card."),
                        10.0f);
  }
}

void System::UpdateMultitaps()
{
  switch (g_settings.multitap_mode)
  {
    case MultitapMode::Disabled:
    {
      Pad::GetMultitap(0)->SetEnable(false, 0);
      Pad::GetMultitap(1)->SetEnable(false, 1);
    }
    break;

    case MultitapMode::Port1Only:
    {
      Pad::GetMultitap(0)->SetEnable(true, 0);
      Pad::GetMultitap(1)->SetEnable(false, 1);
    }
    break;

    case MultitapMode::Port2Only:
    {
      Pad::GetMultitap(0)->SetEnable(false, 0);
      Pad::GetMultitap(1)->SetEnable(true, 1);
    }
    break;

    case MultitapMode::BothPorts:
    {
      Pad::GetMultitap(0)->SetEnable(true, 0);
      Pad::GetMultitap(1)->SetEnable(true, 1);
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

bool System::DumpRAM(const char* filename)
{
  if (!IsValid())
    return false;

  return FileSystem::WriteBinaryFile(filename, Bus::g_unprotected_ram, Bus::g_ram_size);
}

bool System::DumpVRAM(const char* filename)
{
  if (!IsValid())
    return false;

  return g_gpu.DumpVRAMToFile(filename);
}

bool System::DumpSPURAM(const char* filename)
{
  if (!IsValid())
    return false;

  return FileSystem::WriteBinaryFile(filename, SPU::GetRAM().data(), SPU::RAM_SIZE);
}

bool System::HasMedia()
{
  return CDROM::HasMedia();
}

std::string System::GetMediaFileName()
{
  if (!CDROM::HasMedia())
    return {};

  return CDROM::GetMediaPath();
}

bool System::InsertMedia(const char* path)
{
  if (IsGPUDumpPath(path)) [[unlikely]]
    return ChangeGPUDump(path);

  ClearMemorySaveStates(true, true);

  Error error;
  std::unique_ptr<CDImage> image = CDImage::Open(path, g_settings.cdrom_load_image_patches, &error);
  const DiscRegion region =
    image ? GameList::GetCustomRegionForPath(path).value_or(GetRegionForImage(image.get())) : DiscRegion::NonPS1;
  if (!image ||
      (UpdateRunningGame(path, image.get(), false),
       !CDROM::InsertMedia(std::move(image), region, s_state.running_game_serial, s_state.running_game_title, &error)))
  {
    Host::AddIconOSDWarning(
      "DiscInserted", ICON_FA_COMPACT_DISC,
      fmt::format(TRANSLATE_FS("OSDMessage", "Failed to open disc image '{}': {}."), path, error.GetDescription()),
      Host::OSD_ERROR_DURATION);
    return false;
  }

  INFO_LOG("Inserted media from {} ({}, {})", s_state.running_game_path, s_state.running_game_serial,
           s_state.running_game_title);
  if (g_settings.cdrom_load_image_to_ram)
    CDROM::PrecacheMedia();

  Host::AddIconOSDMessage("DiscInserted", ICON_FA_COMPACT_DISC,
                          fmt::format(TRANSLATE_FS("OSDMessage", "Inserted disc '{}' ({})."),
                                      s_state.running_game_title, s_state.running_game_serial),
                          Host::OSD_INFO_DURATION);

  if (g_settings.HasAnyPerGameMemoryCards())
  {
    Host::AddIconOSDMessage("ReloadMemoryCardsFromGameChange", ICON_FA_SD_CARD,
                            TRANSLATE_STR("System", "Game changed, reloading memory cards."), Host::OSD_INFO_DURATION);
    UpdatePerGameMemoryCards();
  }

  return true;
}

void System::RemoveMedia()
{
  ClearMemorySaveStates(true, true);
  CDROM::RemoveMedia(false);
}

void System::UpdateRunningGame(const std::string& path, CDImage* image, bool booting)
{
  if (!booting && s_state.running_game_path == path)
    return;

  const std::string prev_serial = std::move(s_state.running_game_serial);

  s_state.running_game_path.clear();
  s_state.running_game_serial = {};
  s_state.running_game_title.clear();
  s_state.running_game_entry = nullptr;
  s_state.running_game_hash = 0;
  s_state.running_game_custom_title = false;

  if (!path.empty())
  {
    s_state.running_game_path = path;
    s_state.running_game_title = GameList::GetCustomTitleForPath(s_state.running_game_path);
    s_state.running_game_custom_title = !s_state.running_game_title.empty();

    if (IsExePath(path))
    {
      if (s_state.running_game_title.empty())
        s_state.running_game_title = Path::GetFileTitle(FileSystem::GetDisplayNameFromPath(path));

      s_state.running_game_hash = GetGameHashFromFile(s_state.running_game_path.c_str());
      if (s_state.running_game_hash != 0)
        s_state.running_game_serial = GetGameHashId(s_state.running_game_hash);
    }
    else if (IsPsfPath(path))
    {
      // TODO: We could pull the title from the PSF.
      if (s_state.running_game_title.empty())
        s_state.running_game_title = Path::GetFileTitle(path);
    }
    else if (IsGPUDumpPath(path))
    {
      DebugAssert(s_state.gpu_dump_player);
      if (s_state.gpu_dump_player)
      {
        s_state.running_game_serial = s_state.gpu_dump_player->GetSerial();
        if (!s_state.running_game_serial.empty())
        {
          s_state.running_game_entry = GameDatabase::GetEntryForSerial(s_state.running_game_serial);
          if (s_state.running_game_entry && s_state.running_game_title.empty())
            s_state.running_game_title = s_state.running_game_entry->title;
          else if (s_state.running_game_title.empty())
            s_state.running_game_title = s_state.running_game_serial;
        }
      }
    }
    else if (image)
    {
      // Data discs should try to pull the title from the serial.
      if (image->GetTrack(1).mode != CDImage::TrackMode::Audio)
      {
        std::string id;
        GetGameDetailsFromImage(image, &id, &s_state.running_game_hash);

        s_state.running_game_entry = GameDatabase::GetEntryForGameDetails(id, s_state.running_game_hash);
        if (s_state.running_game_entry)
        {
          s_state.running_game_serial = s_state.running_game_entry->serial;
          if (s_state.running_game_title.empty())
            s_state.running_game_title = s_state.running_game_entry->title;
        }
        else
        {
          s_state.running_game_serial = std::move(id);

          // Don't display device names for unknown physical discs.
          if (s_state.running_game_title.empty() && !CDImage::IsDeviceName(path.c_str()))
            s_state.running_game_title = Path::GetFileTitle(FileSystem::GetDisplayNameFromPath(path));
        }

        if (image->HasSubImages())
        {
          std::string image_title = image->GetMetadata("title");
          if (!image_title.empty())
          {
            s_state.running_game_title = std::move(image_title);
            s_state.running_game_custom_title = false;
          }
        }
      }
      else
      {
        // Audio CDs can get the path from the filename, assuming it's not a physical disc.
        if (!CDImage::IsDeviceName(path.c_str()))
          s_state.running_game_title = Path::GetFileTitle(FileSystem::GetDisplayNameFromPath(path));
      }
    }
  }

  UpdateGameSettingsLayer();

  if (!IsReplayingGPUDump())
  {
    Achievements::GameChanged(s_state.running_game_path, image, booting);

    // Cheats are loaded later in Initialize().
    if (!booting)
      Cheats::ReloadCheats(true, true, false, true, true);
  }

  ApplySettings(true);

  if (s_state.running_game_serial != prev_serial)
  {
    GPUThread::SetGameSerial(s_state.running_game_serial);
    UpdateSessionTime(prev_serial);
  }

  UpdateRichPresence(booting);

  FullscreenUI::OnRunningGameChanged(s_state.running_game_path, s_state.running_game_serial,
                                     s_state.running_game_title);

  Host::OnGameChanged(s_state.running_game_path, s_state.running_game_serial, s_state.running_game_title,
                      s_state.running_game_hash);
}

bool System::CheckForRequiredSubQ(Error* error)
{
  if (IsReplayingGPUDump() || !s_state.running_game_entry ||
      !s_state.running_game_entry->HasTrait(GameDatabase::Trait::IsLibCryptProtected) ||
      CDROM::HasNonStandardOrReplacementSubQ())
  {
    return true;
  }

  WARNING_LOG("SBI file missing but required for {} ({})", s_state.running_game_serial, s_state.running_game_title);

  if (Host::GetBoolSettingValue("CDROM", "AllowBootingWithoutSBIFile", false))
  {
    if (Host::ConfirmMessage(
          "Confirm Unsupported Configuration",
          LargeString::from_format(
            TRANSLATE_FS("System", "You are attempting to run a libcrypt protected game without an SBI file:\n\n{0}: "
                                   "{1}\n\nThe game will likely not run properly.\n\nPlease check the README for "
                                   "instructions on how to add an SBI file.\n\nDo you wish to continue?"),
            s_state.running_game_serial, s_state.running_game_title)))
    {
      return true;
    }
  }

  Error::SetStringFmt(
    error,
    TRANSLATE_FS("System", "You are attempting to run a libcrypt protected game without an SBI file:\n\n{0}: "
                           "{1}\n\nYour dump is incomplete, you must add the SBI file to run this game. \n\nThe "
                           "name of the SBI file must match the name of the disc image."),
    s_state.running_game_serial, s_state.running_game_title);

  return false;
}

bool System::HasMediaSubImages()
{
  const CDImage* cdi = CDROM::GetMedia();
  return cdi ? cdi->HasSubImages() : false;
}

u32 System::GetMediaSubImageCount()
{
  const CDImage* cdi = CDROM::GetMedia();
  return cdi ? cdi->GetSubImageCount() : 0;
}

u32 System::GetMediaSubImageIndex()
{
  const CDImage* cdi = CDROM::GetMedia();
  return cdi ? cdi->GetCurrentSubImage() : 0;
}

u32 System::GetMediaSubImageIndexForTitle(std::string_view title)
{
  const CDImage* cdi = CDROM::GetMedia();
  if (!cdi)
    return 0;

  const u32 count = cdi->GetSubImageCount();
  for (u32 i = 0; i < count; i++)
  {
    if (title == cdi->GetSubImageMetadata(i, "title"))
      return i;
  }

  return std::numeric_limits<u32>::max();
}

std::string System::GetMediaSubImageTitle(u32 index)
{
  const CDImage* cdi = CDROM::GetMedia();
  if (!cdi)
    return {};

  return cdi->GetSubImageMetadata(index, "title");
}

bool System::SwitchMediaSubImage(u32 index)
{
  if (!CDROM::HasMedia())
    return false;

  ClearMemorySaveStates(true, true);

  std::unique_ptr<CDImage> image = CDROM::RemoveMedia(true);
  Assert(image);

  Error error;
  bool okay = image->SwitchSubImage(index, &error);
  std::string title, subimage_title;
  if (okay)
  {
    const DiscRegion region =
      GameList::GetCustomRegionForPath(image->GetPath()).value_or(GetRegionForImage(image.get()));
    subimage_title = image->GetSubImageMetadata(index, "title");
    title = image->GetMetadata("title");
    UpdateRunningGame(image->GetPath(), image.get(), false);
    okay =
      CDROM::InsertMedia(std::move(image), region, s_state.running_game_serial, s_state.running_game_title, &error);
  }
  if (!okay)
  {
    Host::AddIconOSDMessage("MediaSwitchSubImage", ICON_FA_COMPACT_DISC,
                            fmt::format(TRANSLATE_FS("System", "Failed to switch to subimage {} in '{}': {}."),
                                        index + 1u, FileSystem::GetDisplayNameFromPath(image->GetPath()),
                                        error.GetDescription()),
                            Host::OSD_INFO_DURATION);

    // restore old disc
    const DiscRegion region =
      GameList::GetCustomRegionForPath(image->GetPath()).value_or(GetRegionForImage(image.get()));
    UpdateRunningGame(image->GetPath(), image.get(), false);
    CDROM::InsertMedia(std::move(image), region, s_state.running_game_serial, s_state.running_game_title, nullptr);
    return false;
  }

  Host::AddIconOSDMessage(
    "MediaSwitchSubImage", ICON_FA_COMPACT_DISC,
    fmt::format(TRANSLATE_FS("System", "Switched to sub-image {} ({}) in '{}'."), subimage_title, index + 1u, title),
    Host::OSD_INFO_DURATION);
  return true;
}

bool System::ShouldStartFullscreen()
{
  return Host::GetBoolSettingValue("Main", "StartFullscreen", false);
}

bool System::ShouldStartPaused()
{
  return Host::GetBoolSettingValue("Main", "StartPaused", false);
}

void System::CheckForSettingsChanges(const Settings& old_settings)
{
  if (IsValid())
  {
    ClearMemorySaveStates(false, false);

    if (g_settings.disable_all_enhancements != old_settings.disable_all_enhancements)
      Cheats::ReloadCheats(false, true, false, true, true);

    if (g_settings.cpu_overclock_active != old_settings.cpu_overclock_active ||
        (g_settings.cpu_overclock_active &&
         (g_settings.cpu_overclock_numerator != old_settings.cpu_overclock_numerator ||
          g_settings.cpu_overclock_denominator != old_settings.cpu_overclock_denominator)))
    {
      UpdateOverclock();
    }

    if (g_settings.audio_backend != old_settings.audio_backend ||
        g_settings.audio_driver != old_settings.audio_driver ||
        g_settings.audio_output_device != old_settings.audio_output_device)
    {
      if (g_settings.audio_backend != old_settings.audio_backend)
      {
        Host::AddIconOSDMessage("AudioBackendSwitch", ICON_FA_HEADPHONES,
                                fmt::format(TRANSLATE_FS("OSDMessage", "Switching to {} audio backend."),
                                            AudioStream::GetBackendDisplayName(g_settings.audio_backend)),
                                Host::OSD_INFO_DURATION);
      }

      SPU::RecreateOutputStream();
    }
    if (g_settings.audio_stream_parameters.stretch_mode != old_settings.audio_stream_parameters.stretch_mode)
      SPU::GetOutputStream()->SetStretchMode(g_settings.audio_stream_parameters.stretch_mode);
    if (g_settings.audio_stream_parameters != old_settings.audio_stream_parameters)
    {
      SPU::RecreateOutputStream();
      UpdateSpeedLimiterState();
    }

    if (g_settings.emulation_speed != old_settings.emulation_speed)
      UpdateThrottlePeriod();

    if (g_settings.cpu_execution_mode != old_settings.cpu_execution_mode)
    {
      Host::AddIconOSDMessage("CPUExecutionModeSwitch", ICON_FA_MICROCHIP,
                              fmt::format(TRANSLATE_FS("OSDMessage", "Switching to {} CPU execution mode."),
                                          TRANSLATE_SV("CPUExecutionMode", Settings::GetCPUExecutionModeDisplayName(
                                                                             g_settings.cpu_execution_mode))),
                              Host::OSD_INFO_DURATION);
      CPU::UpdateDebugDispatcherFlag();
      InterruptExecution();
    }

    if (CPU::GetCurrentExecutionMode() != CPUExecutionMode::Interpreter &&
        (g_settings.cpu_recompiler_memory_exceptions != old_settings.cpu_recompiler_memory_exceptions ||
         g_settings.cpu_recompiler_block_linking != old_settings.cpu_recompiler_block_linking ||
         g_settings.cpu_recompiler_icache != old_settings.cpu_recompiler_icache ||
         g_settings.bios_tty_logging != old_settings.bios_tty_logging))
    {
      Host::AddIconOSDMessage("CPUFlushAllBlocks", ICON_FA_MICROCHIP,
                              TRANSLATE_STR("OSDMessage", "Recompiler options changed, flushing all blocks."),
                              Host::OSD_INFO_DURATION);
      CPU::CodeCache::Reset();
      CPU::g_state.bus_error = false;
    }
    else if (g_settings.cpu_execution_mode == CPUExecutionMode::Interpreter &&
             g_settings.bios_tty_logging != old_settings.bios_tty_logging)
    {
      // TTY interception requires debug dispatcher.
      if (CPU::UpdateDebugDispatcherFlag())
        InterruptExecution();
    }

    if (g_settings.cpu_fastmem_mode != old_settings.cpu_fastmem_mode)
    {
      // Reallocate fastmem area, even if it's not being used.
      Bus::RemapFastmemViews();
      CPU::CodeCache::Reset();
      InterruptExecution();
    }

    SPU::GetOutputStream()->SetOutputVolume(GetAudioOutputVolume());

    // CPU side GPU settings
    if (g_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode ||
        g_settings.gpu_fifo_size != old_settings.gpu_fifo_size ||
        g_settings.gpu_max_run_ahead != old_settings.gpu_max_run_ahead ||
        g_settings.gpu_force_video_timing != old_settings.gpu_force_video_timing ||
        g_settings.display_crop_mode != old_settings.display_crop_mode ||
        g_settings.display_aspect_ratio != old_settings.display_aspect_ratio)
    {
      g_gpu.UpdateSettings(old_settings);
    }

    if (g_settings.gpu_renderer != old_settings.gpu_renderer)
    {
      // RecreateGPU() also pushes new settings to the thread.
      Host::AddIconOSDMessage("RendererSwitch", ICON_FA_PAINT_ROLLER,
                              fmt::format(TRANSLATE_FS("OSDMessage", "Switching to {}{} GPU renderer."),
                                          Settings::GetRendererName(g_settings.gpu_renderer),
                                          g_settings.gpu_use_debug_device ? " (debug)" : ""),
                              Host::OSD_INFO_DURATION);
      RecreateGPU(g_settings.gpu_renderer);
    }
    else if (g_settings.gpu_resolution_scale != old_settings.gpu_resolution_scale ||
             g_settings.gpu_multisamples != old_settings.gpu_multisamples ||
             g_settings.gpu_per_sample_shading != old_settings.gpu_per_sample_shading ||
             g_settings.gpu_max_queued_frames != old_settings.gpu_max_queued_frames ||
             g_settings.gpu_use_software_renderer_for_readbacks !=
               old_settings.gpu_use_software_renderer_for_readbacks ||
             g_settings.gpu_true_color != old_settings.gpu_true_color ||
             g_settings.gpu_scaled_dithering != old_settings.gpu_scaled_dithering ||
             g_settings.gpu_force_round_texcoords != old_settings.gpu_force_round_texcoords ||
             g_settings.gpu_accurate_blending != old_settings.gpu_accurate_blending ||
             g_settings.gpu_texture_filter != old_settings.gpu_texture_filter ||
             g_settings.gpu_sprite_texture_filter != old_settings.gpu_sprite_texture_filter ||
             g_settings.gpu_line_detect_mode != old_settings.gpu_line_detect_mode ||
             g_settings.gpu_downsample_mode != old_settings.gpu_downsample_mode ||
             g_settings.gpu_downsample_scale != old_settings.gpu_downsample_scale ||
             g_settings.gpu_wireframe_mode != old_settings.gpu_wireframe_mode ||
             g_settings.gpu_texture_cache != old_settings.gpu_texture_cache ||
             g_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode ||
             g_settings.display_24bit_chroma_smoothing != old_settings.display_24bit_chroma_smoothing ||
             g_settings.display_aspect_ratio != old_settings.display_aspect_ratio ||
             g_settings.display_scaling != old_settings.display_scaling ||
             g_settings.display_alignment != old_settings.display_alignment ||
             g_settings.display_rotation != old_settings.display_rotation ||
             g_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode ||
             g_settings.display_osd_scale != old_settings.display_osd_scale ||
             g_settings.display_osd_margin != old_settings.display_osd_margin ||
             g_settings.gpu_pgxp_enable != old_settings.gpu_pgxp_enable ||
             g_settings.gpu_pgxp_texture_correction != old_settings.gpu_pgxp_texture_correction ||
             g_settings.gpu_pgxp_color_correction != old_settings.gpu_pgxp_color_correction ||
             g_settings.gpu_pgxp_depth_buffer != old_settings.gpu_pgxp_depth_buffer ||
             g_settings.display_active_start_offset != old_settings.display_active_start_offset ||
             g_settings.display_active_end_offset != old_settings.display_active_end_offset ||
             g_settings.display_line_start_offset != old_settings.display_line_start_offset ||
             g_settings.display_line_end_offset != old_settings.display_line_end_offset ||
             g_settings.gpu_show_vram != old_settings.gpu_show_vram ||
             g_settings.rewind_enable != old_settings.rewind_enable ||
             g_settings.runahead_frames != old_settings.runahead_frames ||
             g_settings.texture_replacements != old_settings.texture_replacements)
    {
      GPUThread::UpdateSettings(true, false, false);

      // NOTE: Must come after the GPU thread settings update, otherwise it allocs the wrong size textures.
      const bool use_existing_textures = (g_settings.gpu_resolution_scale == old_settings.gpu_resolution_scale);
      ClearMemorySaveStates(true, use_existing_textures);

      if (IsPaused())
        GPUThread::PresentCurrentFrame();
    }
    else if (const bool device_settings_changed = g_settings.AreGPUDeviceSettingsChanged(old_settings);
             device_settings_changed || g_settings.display_show_fps != old_settings.display_show_fps ||
             g_settings.display_show_speed != old_settings.display_show_speed ||
             g_settings.display_show_gpu_stats != old_settings.display_show_gpu_stats ||
             g_settings.display_show_resolution != old_settings.display_show_resolution ||
             g_settings.display_show_latency_stats != old_settings.display_show_latency_stats ||
             g_settings.display_show_cpu_usage != old_settings.display_show_cpu_usage ||
             g_settings.display_show_gpu_usage != old_settings.display_show_gpu_usage ||
             g_settings.display_show_latency_stats != old_settings.display_show_latency_stats ||
             g_settings.display_show_frame_times != old_settings.display_show_frame_times ||
             g_settings.display_show_status_indicators != old_settings.display_show_status_indicators ||
             g_settings.display_show_inputs != old_settings.display_show_inputs ||
             g_settings.display_show_enhancements != old_settings.display_show_enhancements ||
             g_settings.display_auto_resize_window != old_settings.display_auto_resize_window ||
             g_settings.display_screenshot_mode != old_settings.display_screenshot_mode ||
             g_settings.display_screenshot_format != old_settings.display_screenshot_format ||
             g_settings.display_screenshot_quality != old_settings.display_screenshot_quality)
    {
      if (device_settings_changed)
      {
        // device changes are super icky, we need to purge and recreate any rewind states
        FreeMemoryStateStorage(false, true, false);
        StopMediaCapture();
        GPUThread::UpdateSettings(true, true, g_settings.gpu_use_thread != old_settings.gpu_use_thread);
        ClearMemorySaveStates(true, false);

        // and display the current frame on the new device
        g_gpu.UpdateDisplay(false);
        if (IsPaused())
          GPUThread::PresentCurrentFrame();
      }
      else
      {
        // don't need to represent here, because the OSD isn't visible while paused anyway
        GPUThread::UpdateSettings(true, false, false);
      }
    }
    else
    {
      // still need to update debug windows
      GPUThread::UpdateSettings(false, false, false);
    }

    if (g_settings.gpu_widescreen_hack != old_settings.gpu_widescreen_hack ||
        g_settings.display_aspect_ratio != old_settings.display_aspect_ratio ||
        (g_settings.display_aspect_ratio == DisplayAspectRatio::Custom &&
         (g_settings.display_aspect_ratio_custom_numerator != old_settings.display_aspect_ratio_custom_numerator ||
          g_settings.display_aspect_ratio_custom_denominator != old_settings.display_aspect_ratio_custom_denominator)))
    {
      UpdateGTEAspectRatio();
    }

    if (g_settings.gpu_pgxp_enable != old_settings.gpu_pgxp_enable ||
        (g_settings.gpu_pgxp_enable && (g_settings.gpu_pgxp_culling != old_settings.gpu_pgxp_culling ||
                                        g_settings.gpu_pgxp_vertex_cache != old_settings.gpu_pgxp_vertex_cache ||
                                        g_settings.gpu_pgxp_cpu != old_settings.gpu_pgxp_cpu)))
    {
      if (old_settings.gpu_pgxp_enable)
        CPU::PGXP::Shutdown();

      if (g_settings.gpu_pgxp_enable)
        CPU::PGXP::Initialize();

      CPU::CodeCache::Reset();
      CPU::UpdateDebugDispatcherFlag();
      InterruptExecution();
    }

    if (g_settings.cdrom_readahead_sectors != old_settings.cdrom_readahead_sectors)
      CDROM::SetReadaheadSectors(g_settings.cdrom_readahead_sectors);

    bool controllers_updated = false;
    for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    {
      if (g_settings.controller_types[i] != old_settings.controller_types[i])
      {
        UpdateControllers();
        ResetControllers();
        controllers_updated = true;
        break;
      }
    }

    if (!controllers_updated)
      UpdateControllerSettings();

    if (g_settings.memory_card_types != old_settings.memory_card_types ||
        g_settings.memory_card_paths != old_settings.memory_card_paths ||
        (g_settings.memory_card_use_playlist_title != old_settings.memory_card_use_playlist_title))
    {
      UpdateMemoryCardTypes();
    }

    if (g_settings.rewind_enable != old_settings.rewind_enable ||
        g_settings.rewind_save_frequency != old_settings.rewind_save_frequency ||
        g_settings.rewind_save_slots != old_settings.rewind_save_slots ||
        g_settings.runahead_frames != old_settings.runahead_frames)
    {
      UpdateMemorySaveStateSettings();
    }

    if (g_settings.audio_backend != old_settings.audio_backend ||
        g_settings.emulation_speed != old_settings.emulation_speed ||
        g_settings.fast_forward_speed != old_settings.fast_forward_speed ||
        g_settings.display_optimal_frame_pacing != old_settings.display_optimal_frame_pacing ||
        g_settings.display_skip_presenting_duplicate_frames != old_settings.display_skip_presenting_duplicate_frames ||
        g_settings.display_pre_frame_sleep != old_settings.display_pre_frame_sleep ||
        g_settings.display_pre_frame_sleep_buffer != old_settings.display_pre_frame_sleep_buffer ||
        g_settings.display_vsync != old_settings.display_vsync ||
        g_settings.display_disable_mailbox_presentation != old_settings.display_disable_mailbox_presentation ||
        g_settings.sync_to_host_refresh_rate != old_settings.sync_to_host_refresh_rate)
    {
      UpdateSpeedLimiterState();
    }

    if (g_settings.multitap_mode != old_settings.multitap_mode)
      UpdateMultitaps();

    if (g_settings.pio_device_type != old_settings.pio_device_type ||
        g_settings.pio_flash_image_path != old_settings.pio_flash_image_path ||
        g_settings.pio_flash_write_enable != old_settings.pio_flash_write_enable ||
        g_settings.pio_switch_active != old_settings.pio_switch_active)
    {
      PIO::UpdateSettings(old_settings);
    }

    if (g_settings.inhibit_screensaver != old_settings.inhibit_screensaver)
    {
      if (g_settings.inhibit_screensaver)
        PlatformMisc::SuspendScreensaver();
      else
        PlatformMisc::ResumeScreensaver();
    }

#ifdef ENABLE_GDB_SERVER
    if (g_settings.enable_gdb_server != old_settings.enable_gdb_server ||
        g_settings.gdb_server_port != old_settings.gdb_server_port)
    {
      GDBServer::Shutdown();
      if (g_settings.enable_gdb_server)
        GDBServer::Initialize(g_settings.gdb_server_port);
    }
#endif
  }
  else
  {
    if (GPUThread::IsFullscreenUIRequested())
    {
      // handle device setting updates as well
      if (g_settings.gpu_renderer != old_settings.gpu_renderer || g_settings.AreGPUDeviceSettingsChanged(old_settings))
        GPUThread::UpdateSettings(false, true, g_settings.gpu_use_thread != old_settings.gpu_use_thread);

      if (g_settings.display_vsync != old_settings.display_vsync ||
          g_settings.display_disable_mailbox_presentation != old_settings.display_disable_mailbox_presentation)
      {
        UpdateDisplayVSync();
      }
    }
  }

  Achievements::UpdateSettings(old_settings);

#ifdef ENABLE_DISCORD_PRESENCE
  if (g_settings.enable_discord_presence != old_settings.enable_discord_presence)
  {
    if (g_settings.enable_discord_presence)
      InitializeDiscordPresence();
    else
      ShutdownDiscordPresence();
  }
#endif

  if (g_settings.export_shared_memory != old_settings.export_shared_memory) [[unlikely]]
  {
    Error error;
    if (!Bus::ReallocateMemoryMap(g_settings.export_shared_memory, &error)) [[unlikely]]
    {
      if (IsValid())
      {
        AbnormalShutdown(fmt::format("Failed to reallocate memory map: {}", error.GetDescription()));
        return;
      }
    }
  }

  if (g_settings.gpu_use_thread && g_settings.gpu_max_queued_frames != old_settings.gpu_max_queued_frames) [[unlikely]]
    GPUThread::SyncGPUThread(false);
}

void System::SetTaintsFromSettings()
{
  s_state.taints = 0;

  if (g_settings.cdrom_read_speedup > 1)
    SetTaint(Taint::CDROMReadSpeedup);
  if (g_settings.cdrom_seek_speedup > 1)
    SetTaint(Taint::CDROMSeekSpeedup);
  if (g_settings.cpu_overclock_active)
    SetTaint(Taint::CPUOverclock);
  if (g_settings.gpu_force_video_timing != ForceVideoTimingMode::Disabled)
    SetTaint(Taint::ForceFrameTimings);
  if (g_settings.enable_8mb_ram)
    SetTaint(Taint::RAM8MB);
  if (Cheats::GetActivePatchCount() > 0)
    SetTaint(Taint::Patches);
  if (Cheats::GetActiveCheatCount() > 0)
    SetTaint(Taint::Cheats);
}

void System::WarnAboutStateTaints(u32 state_taints)
{
  const u32 taints_active_in_file = state_taints & ~s_state.taints;
  if (taints_active_in_file == 0)
    return;

  LargeString messages;
  for (u32 i = 0; i < static_cast<u32>(Taint::MaxCount); i++)
  {
    if (!(taints_active_in_file & (1u << i)))
      continue;

    if (messages.empty())
    {
      messages.append_format(
        "{} {}\n", ICON_EMOJI_WARNING,
        TRANSLATE_SV("System", "This save state was created with the following tainted options, and may\n"
                               "       be unstable. You will need to reset the system to clear any effects."));
    }

    messages.append("        \u2022 ");
    messages.append(GetTaintDisplayName(static_cast<Taint>(i)));
    messages.append('\n');
  }

  Host::AddKeyedOSDWarning("SystemTaintsFromState", std::string(messages.view()), Host::OSD_WARNING_DURATION);
}

void System::WarnAboutUnsafeSettings()
{
  LargeString messages;
  const auto append = [&messages](const char* icon, std::string_view msg) {
    messages.append_format("{} {}\n", icon, msg);
  };
  const auto append_format = [&messages]<typename... T>(const char* icon, fmt::format_string<T...> fmt, T&&... args) {
    messages.append_format("{} ", icon);
    messages.append_vformat(fmt, fmt::make_format_args(args...));
    messages.append('\n');
  };

  if (!g_settings.disable_all_enhancements)
  {
    if (g_settings.cpu_overclock_active)
    {
      append_format(
        ICON_EMOJI_WARNING, TRANSLATE_FS("System", "CPU clock speed is set to {}% ({} / {}). This may crash games."),
        g_settings.GetCPUOverclockPercent(), g_settings.cpu_overclock_numerator, g_settings.cpu_overclock_denominator);
    }
    if (g_settings.cdrom_read_speedup != 1 || g_settings.cdrom_seek_speedup != 1)
      append(ICON_EMOJI_WARNING, TRANSLATE_SV("System", "CD-ROM read/seek speedup is enabled. This may crash games."));
    if (g_settings.gpu_force_video_timing != ForceVideoTimingMode::Disabled)
      append(ICON_FA_TV, TRANSLATE_SV("System", "Force frame timings is enabled. Games may run at incorrect speeds."));
    if (!g_settings.IsUsingSoftwareRenderer())
    {
      if (g_settings.gpu_multisamples != 1)
      {
        append(ICON_EMOJI_WARNING,
               TRANSLATE_SV("System", "Multisample anti-aliasing is enabled, some games may not render correctly."));
      }
      if (g_settings.gpu_resolution_scale > 1 && g_settings.gpu_force_round_texcoords)
      {
        append(
          ICON_EMOJI_WARNING,
          TRANSLATE_SV("System", "Round upscaled texture coordinates is enabled. This may cause rendering errors."));
      }
    }
    if (g_settings.enable_8mb_ram)
    {
      append(ICON_EMOJI_WARNING,
             TRANSLATE_SV("System", "8MB RAM is enabled, this may be incompatible with some games."));
    }
    if (g_settings.cpu_execution_mode == CPUExecutionMode::CachedInterpreter)
    {
      append(ICON_EMOJI_WARNING,
             TRANSLATE_SV("System", "Cached interpreter is being used, this may be incompatible with some games."));
    }

    // Always display TC warning.
    if (g_settings.gpu_texture_cache)
    {
      append(
        ICON_FA_PAINT_ROLLER,
        TRANSLATE_SV("System",
                     "Texture cache is enabled. This feature is experimental, some games may not render correctly."));
    }

    // Potential performance issues.
    if (g_settings.cpu_fastmem_mode != Settings::DEFAULT_CPU_FASTMEM_MODE)
    {
      append_format(ICON_EMOJI_WARNING,
                    TRANSLATE_FS("System", "Fastmem mode is set to {}, this will reduce performance."),
                    Settings::GetCPUFastmemModeName(g_settings.cpu_fastmem_mode));
    }
  }

  if (g_settings.disable_all_enhancements)
  {
    append(ICON_EMOJI_WARNING, TRANSLATE_SV("System", "Safe mode is enabled."));

#define APPEND_SUBMESSAGE(msg)                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    messages.append("        \u2022 ");                                                                                \
    messages.append(msg);                                                                                              \
    messages.append('\n');                                                                                             \
  } while (0)

    if (g_settings.cpu_overclock_active)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "Overclock disabled."));
    if (g_settings.enable_8mb_ram)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "8MB RAM disabled."));
    if (g_settings.gpu_resolution_scale != 1)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "Resolution scale set to 1x."));
    if (g_settings.gpu_multisamples != 1)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "Multisample anti-aliasing disabled."));
    if (g_settings.gpu_true_color)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "True color disabled."));
    if (g_settings.gpu_texture_filter != GPUTextureFilter::Nearest ||
        g_settings.gpu_sprite_texture_filter != GPUTextureFilter::Nearest)
    {
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "Texture filtering disabled."));
    }
    if (g_settings.display_deinterlacing_mode == DisplayDeinterlacingMode::Progressive)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "Interlaced rendering enabled."));
    if (g_settings.gpu_force_video_timing != ForceVideoTimingMode::Disabled)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "Video timings set to default."));
    if (g_settings.gpu_widescreen_hack)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "Widescreen rendering disabled."));
    if (g_settings.gpu_pgxp_enable)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "PGXP disabled."));
    if (g_settings.gpu_texture_cache)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "GPU texture cache disabled."));
    if (g_settings.display_24bit_chroma_smoothing)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "FMV chroma smoothing disabled."));
    if (g_settings.cdrom_read_speedup != 1)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "CD-ROM read speedup disabled."));
    if (g_settings.cdrom_seek_speedup != 1)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "CD-ROM seek speedup disabled."));
    if (g_settings.cdrom_mute_cd_audio)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "Mute CD-ROM audio disabled."));
    if (g_settings.texture_replacements.enable_vram_write_replacements)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "VRAM write texture replacements disabled."));
    if (g_settings.use_old_mdec_routines)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "Use old MDEC routines disabled."));
    if (g_settings.pio_device_type != PIODeviceType::None)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "PIO device removed."));
    if (g_settings.pcdrv_enable)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "PCDrv disabled."));
    if (g_settings.bios_patch_fast_boot)
      APPEND_SUBMESSAGE(TRANSLATE_SV("System", "Fast boot disabled."));

#undef APPEND_SUBMESSAGE
  }

  if (!g_settings.apply_compatibility_settings)
  {
    append(ICON_EMOJI_WARNING,
           TRANSLATE_STR("System", "Compatibility settings are not enabled. Some games may not function correctly."));
  }

  if (g_settings.cdrom_subq_skew)
    append(ICON_EMOJI_WARNING, TRANSLATE_SV("System", "CD-ROM SubQ Skew is enabled. This will break games."));

  if (!messages.empty())
  {
    if (messages.back() == '\n')
      messages.pop_back();

    LogUnsafeSettingsToConsole(messages);

    // Force the message, but use a reduced duration if they have OSD messages disabled.
    Host::AddKeyedOSDWarning("performance_settings_warning", std::string(messages.view()),
                             ImGuiManager::IsShowingOSDMessages() ? Host::OSD_INFO_DURATION : Host::OSD_QUICK_DURATION);
  }
  else
  {
    Host::RemoveKeyedOSDWarning("performance_settings_warning");
  }
}

void System::LogUnsafeSettingsToConsole(const SmallStringBase& messages)
{
  // a not-great way of getting rid of the icons for the console message
  LargeString console_messages = messages;
  for (;;)
  {
    const s32 pos = console_messages.find("\xef");
    if (pos >= 0)
    {
      console_messages.erase(pos, 3);
      console_messages.insert(pos, "[Unsafe Settings]");
    }
    else
    {
      break;
    }
  }
  WARNING_LOG(console_messages);
}

void System::CalculateRewindMemoryUsage(u32 num_saves, u32 resolution_scale, u64* ram_usage, u64* vram_usage)
{
  const u64 real_resolution_scale = std::max<u64>(g_settings.gpu_resolution_scale, 1u);
  *ram_usage = GetMaxSaveStateSize() * static_cast<u64>(num_saves);
  *vram_usage = ((VRAM_WIDTH * real_resolution_scale) * (VRAM_HEIGHT * real_resolution_scale) * 4) *
                static_cast<u64>(g_settings.gpu_multisamples) * static_cast<u64>(num_saves);
}

void System::UpdateMemorySaveStateSettings()
{
  const bool any_memory_states_active = (g_settings.IsRunaheadEnabled() || g_settings.rewind_enable);
  FreeMemoryStateStorage(true, true, any_memory_states_active);

  if (IsReplayingGPUDump()) [[unlikely]]
  {
    s_state.rewind_save_counter = -1;
    s_state.runahead_frames = 0;
    return;
  }

  u32 num_slots = 0;
  if (g_settings.rewind_enable && !g_settings.IsRunaheadEnabled())
  {
    s_state.rewind_save_frequency =
      static_cast<s32>(std::ceil(g_settings.rewind_save_frequency * s_state.video_frame_rate));
    s_state.rewind_save_counter = 0;
    num_slots = g_settings.rewind_save_slots;

    u64 ram_usage, vram_usage;
    CalculateRewindMemoryUsage(g_settings.rewind_save_slots, g_settings.gpu_resolution_scale, &ram_usage, &vram_usage);
    INFO_LOG("Rewind is enabled, saving every {} frames, with {} slots and {}MB RAM and {}MB VRAM usage",
             std::max(s_state.rewind_save_frequency, 1), g_settings.rewind_save_slots, ram_usage / 1048576,
             vram_usage / 1048576);
  }
  else
  {
    s_state.rewind_save_frequency = -1;
    s_state.rewind_save_counter = -1;
  }

  s_state.rewind_load_frequency = -1;
  s_state.rewind_load_counter = -1;

  s_state.runahead_frames = g_settings.runahead_frames;
  s_state.runahead_replay_pending = false;
  if (s_state.runahead_frames > 0)
  {
    INFO_LOG("Runahead is active with {} frames", s_state.runahead_frames);
    num_slots = s_state.runahead_frames;
  }

  // allocate storage for memory save states
  if (num_slots > 0)
    AllocateMemoryStates(num_slots, true);

  // reenter execution loop, don't want to try to save a state now if runahead was turned off
  InterruptExecution();
}

bool System::LoadOneRewindState()
{
  if (s_state.memory_save_state_count == 0)
    return false;

  // keep the last state so we can go back to it with smaller frequencies
  LoadMemoryState((s_state.memory_save_state_count > 1) ? PopMemoryState() : GetFirstMemoryState(), true);

  // back in time, need to reset perf counters
  GPUThread::RunOnThread(&PerformanceCounters::Reset);

  return true;
}

bool System::IsRewinding()
{
  return (s_state.rewind_load_frequency >= 0);
}

void System::SetRewinding(bool enabled)
{
  const bool was_enabled = IsRewinding();

  if (enabled)
  {
    // Try to rewind at the replay speed, or one per second maximum.
    const float load_frequency = std::min(g_settings.rewind_save_frequency, 1.0f);
    s_state.rewind_load_frequency = static_cast<s32>(std::ceil(load_frequency * s_state.video_frame_rate));
    s_state.rewind_load_counter = 0;

    if (!was_enabled && s_state.system_executing)
    {
      // Drop the last save if we just created it, since we don't want to rewind to where we are.
      if (s_state.rewind_save_counter == s_state.rewind_save_frequency && s_state.memory_save_state_count > 0)
        PopMemoryState();

      s_state.system_interrupted = true;
    }
  }
  else
  {
    s_state.rewind_load_frequency = -1;
    s_state.rewind_load_counter = -1;

    if (was_enabled)
    {
      // reset perf counters to avoid the spike
      GPUThread::RunOnThread(&PerformanceCounters::Reset);

      // and wait the full frequency before filling a new rewind slot
      s_state.rewind_save_counter = s_state.rewind_save_frequency;
    }
  }
}

void System::DoRewind()
{
  if (s_state.rewind_load_counter == 0)
  {
    LoadOneRewindState();
    s_state.rewind_load_counter = s_state.rewind_load_frequency;
  }
  else
  {
    s_state.rewind_load_counter--;
  }

  GPUThread::PresentCurrentFrame();

  Host::PumpMessagesOnCPUThread();
  IdlePollUpdate();

  // get back into it straight away if we're no longer rewinding
  if (!IsRewinding())
    return;

  Throttle(Timer::GetCurrentValue(), s_state.next_frame_time);
}

bool System::IsRunaheadActive()
{
  return (s_state.runahead_frames > 0);
}

bool System::DoRunahead()
{
#ifdef PROFILE_MEMORY_SAVE_STATES
  static Timer replay_timer;
#endif

  if (s_state.runahead_replay_pending)
  {
#ifdef PROFILE_MEMORY_SAVE_STATES
    DEBUG_LOG("runahead starting at frame {}", s_state.frame_number);
    replay_timer.Reset();
#endif

    // we need to replay and catch up - load the state,
    s_state.runahead_replay_pending = false;
    if (s_state.memory_save_state_count == 0)
      return false;

    LoadMemoryState(GetFirstMemoryState(), false);

    // figure out how many frames we need to run to catch up
    s_state.runahead_replay_frames = s_state.memory_save_state_count;

    // and throw away all the states, forcing us to catch up below
    ClearMemorySaveStates(false, false);

    // run the frames with no audio
    SPU::SetAudioOutputMuted(true);

#ifdef PROFILE_MEMORY_SAVE_STATES
    VERBOSE_LOG("Rewound to frame {}, took {:.2f} ms", s_state.frame_number, replay_timer.GetTimeMilliseconds());
#endif

    // we don't want to save the frame we just loaded. but we are "one frame ahead", because the frame we just tossed
    // was never saved, so return but don't decrement the counter
    InterruptExecution();
    CheckForAndExitExecution();
    return true;
  }
  else if (s_state.runahead_replay_frames == 0)
  {
    return false;
  }

  s_state.runahead_replay_frames--;
  if (s_state.runahead_replay_frames > 0)
  {
    // keep running ahead
    SaveMemoryState(AllocateMemoryState());
    return true;
  }

#ifdef PROFILE_MEMORY_SAVE_STATES
  VERBOSE_LOG("Running {} frames to catch up took {:.2f} ms", s_state.runahead_frames,
              replay_timer.GetTimeMilliseconds());
#endif

  // we're all caught up. this frame gets saved in DoMemoryStates().
  SPU::SetAudioOutputMuted(false);

#ifdef PROFILE_MEMORY_SAVE_STATES
  DEV_LOG("runahead ending at frame {}, took {:.2f} ms", s_state.frame_number, replay_timer.GetTimeMilliseconds());
#endif

  return false;
}

void System::SetRunaheadReplayFlag()
{
  if (s_state.runahead_frames == 0 || s_state.memory_save_state_count == 0)
    return;

#ifdef PROFILE_MEMORY_SAVE_STATES
  DEV_LOG("Runahead rewind pending...");
#endif

  s_state.runahead_replay_pending = true;
}

void System::ShutdownSystem(bool save_resume_state)
{
  if (!IsValid())
    return;

  if (save_resume_state)
  {
    Error error;
    if (!SaveResumeState(&error))
    {
      Host::ReportErrorAsync(
        TRANSLATE_SV("System", "Error"),
        fmt::format(TRANSLATE_FS("System", "Failed to save resume state: {}"), error.GetDescription()));
    }
  }

  s_state.state = State::Stopping;
  std::atomic_thread_fence(std::memory_order_release);
  if (!s_state.system_executing)
    DestroySystem();
}

bool System::CanUndoLoadState()
{
  return s_state.undo_load_state.has_value();
}

std::optional<ExtendedSaveStateInfo> System::GetUndoSaveStateInfo()
{
  std::optional<ExtendedSaveStateInfo> ssi;
  if (s_state.undo_load_state.has_value())
  {
    ssi.emplace();
    ssi->title = s_state.undo_load_state->title;
    ssi->serial = s_state.undo_load_state->serial;
    ssi->media_path = s_state.undo_load_state->media_path;
    ssi->screenshot = s_state.undo_load_state->screenshot;
    ssi->timestamp = 0;
  }

  return ssi;
}

bool System::UndoLoadState()
{
  if (!s_state.undo_load_state.has_value())
    return false;

  Assert(IsValid());

  Error error;
  if (!LoadStateFromBuffer(s_state.undo_load_state.value(), &error, IsPaused()))
  {
    Host::ReportErrorAsync("Error",
                           fmt::format("Failed to load undo state, resetting system:\n", error.GetDescription()));
    s_state.undo_load_state.reset();
    ResetSystem();
    return false;
  }

  INFO_LOG("Loaded undo save state.");
  s_state.undo_load_state.reset();
  return true;
}

bool System::SaveUndoLoadState()
{
  if (!s_state.undo_load_state.has_value())
    s_state.undo_load_state.emplace();

  Error error;
  if (!SaveStateToBuffer(&s_state.undo_load_state.value(), &error))
  {
    Host::AddOSDMessage(
      fmt::format(TRANSLATE_FS("OSDMessage", "Failed to save undo load state:\n{}"), error.GetDescription()),
      Host::OSD_CRITICAL_ERROR_DURATION);
    s_state.undo_load_state.reset();
    return false;
  }

  INFO_LOG("Saved undo load state: {} bytes", s_state.undo_load_state->state_size);
  return true;
}

bool System::IsRunningAtNonStandardSpeed()
{
  if (!IsValid())
    return false;

  return (s_state.target_speed != 1.0f && !s_state.syncing_to_host);
}

bool System::IsFastForwardingBoot()
{
  return (g_settings.bios_fast_forward_boot && s_state.internal_frame_number == 0 &&
          s_state.boot_mode == BootMode::FastBoot);
}

u8 System::GetAudioOutputVolume()
{
  return g_settings.GetAudioOutputVolume(IsRunningAtNonStandardSpeed());
}

void System::UpdateVolume()
{
  if (!IsValid())
    return;

  SPU::GetOutputStream()->SetOutputVolume(GetAudioOutputVolume());
}

std::string System::GetScreenshotPath(const char* extension)
{
  const std::string sanitized_name = Path::SanitizeFileName(System::GetGameTitle());
  std::string basename;
  if (sanitized_name.empty())
    basename = fmt::format("{}", GetTimestampStringForFileName());
  else
    basename = fmt::format("{} {}", sanitized_name, GetTimestampStringForFileName());

  std::string path = fmt::format("{}" FS_OSPATH_SEPARATOR_STR "{}.{}", EmuFolders::Screenshots, basename, extension);

  // handle quick screenshots to the same filename
  u32 next_suffix = 1;
  while (FileSystem::FileExists(path.c_str()))
  {
    path =
      fmt::format("{}" FS_OSPATH_SEPARATOR_STR "{} ({}).{}", EmuFolders::Screenshots, basename, next_suffix, extension);
    next_suffix++;
  }

  return path;
}

void System::SaveScreenshot(const char* path, DisplayScreenshotMode mode, DisplayScreenshotFormat format, u8 quality)
{
  if (!IsValid())
    return;

  std::string auto_path;
  if (!path)
    path = (auto_path = GetScreenshotPath(Settings::GetDisplayScreenshotFormatExtension(format))).c_str();

  GPUBackend::RenderScreenshotToFile(path, mode, quality, true);
}

bool System::StartRecordingGPUDump(const char* path /*= nullptr*/, u32 num_frames /*= 0*/)
{
  if (!IsValid() || IsReplayingGPUDump())
    return false;

  std::string auto_path;
  if (!path)
    path = (auto_path = GetScreenshotPath("psxgpu")).c_str();

  return g_gpu.StartRecordingGPUDump(path, num_frames);
}

void System::StopRecordingGPUDump()
{
  if (!IsValid())
    return;

  g_gpu.StopRecordingGPUDump();
}

static std::string_view GetCaptureTypeForMessage(bool capture_video, bool capture_audio)
{
  return capture_video ? (capture_audio ? TRANSLATE_SV("System", "capturing audio and video") :
                                          TRANSLATE_SV("System", "capturing video")) :
                         TRANSLATE_SV("System", "capturing audio");
}

MediaCapture* System::GetMediaCapture()
{
  return s_state.media_capture.get();
}

std::string System::GetNewMediaCapturePath(const std::string_view title, const std::string_view container)
{
  const std::string sanitized_name = Path::SanitizeFileName(title);
  std::string path;
  if (sanitized_name.empty())
  {
    path = Path::Combine(EmuFolders::Videos, fmt::format("{}.{}", GetTimestampStringForFileName(), container));
  }
  else
  {
    path = Path::Combine(EmuFolders::Videos,
                         fmt::format("{} {}.{}", sanitized_name, GetTimestampStringForFileName(), container));
  }

  return path;
}

bool System::StartMediaCapture(std::string path)
{
  const bool capture_video = Host::GetBoolSettingValue("MediaCapture", "VideoCapture", true);
  const bool capture_audio = Host::GetBoolSettingValue("MediaCapture", "AudioCapture", true);

  // Auto size is more complex.
  if (capture_video && Host::GetBoolSettingValue("MediaCapture", "VideoAutoSize", false))
  {
    // need to query this on the GPU thread
    GPUThread::RunOnBackend(
      [path = std::move(path), capture_audio, mode = g_settings.display_screenshot_mode](GPUBackend* backend) mutable {
        if (!backend)
          return;

        // Prefer aligning for non-window size.
        const GSVector2i video_size = backend->GetPresenter().CalculateScreenshotSize(mode);
        u32 video_width = static_cast<u32>(video_size.x);
        u32 video_height = static_cast<u32>(video_size.y);
        if (mode != DisplayScreenshotMode::ScreenResolution)
          MediaCapture::AdjustVideoSize(&video_width, &video_height);

        // fire back to the CPU thread to actually start the capture
        Host::RunOnCPUThread([path = std::move(path), capture_audio, video_width, video_height]() mutable {
          StartMediaCapture(std::move(path), true, capture_audio, video_width, video_height);
        });
      },
      false, true);
    return true;
  }

  u32 video_width =
    Host::GetUIntSettingValue("MediaCapture", "VideoWidth", Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_WIDTH);
  u32 video_height =
    Host::GetUIntSettingValue("MediaCapture", "VideoHeight", Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_HEIGHT);
  MediaCapture::AdjustVideoSize(&video_width, &video_height);

  return StartMediaCapture(std::move(path), capture_video, capture_audio, video_width, video_height);
}

bool System::StartMediaCapture(std::string path, bool capture_video, bool capture_audio, u32 video_width,
                               u32 video_height)
{
  if (!IsValid())
    return false;

  if (s_state.media_capture)
    StopMediaCapture();

  const WindowInfo& main_window_info = GPUThread::GetRenderWindowInfo();
  const GPUTexture::Format capture_format =
    main_window_info.IsSurfaceless() ? GPUTexture::Format::RGBA8 : main_window_info.surface_format;

  // TODO: Render anamorphic capture instead?
  constexpr float aspect = 1.0f;

  if (path.empty())
  {
    path =
      GetNewMediaCapturePath(GetGameTitle(), Host::GetStringSettingValue("MediaCapture", "Container",
                                                                         Settings::DEFAULT_MEDIA_CAPTURE_CONTAINER));
  }

  const MediaCaptureBackend backend =
    MediaCapture::ParseBackendName(
      Host::GetStringSettingValue("MediaCapture", "Backend",
                                  MediaCapture::GetBackendName(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND))
        .c_str())
      .value_or(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND);

  Error error;
  s_state.media_capture = MediaCapture::Create(backend, &error);
  if (!s_state.media_capture ||
      !s_state.media_capture->BeginCapture(
        s_state.video_frame_rate, aspect, video_width, video_height, capture_format, SPU::SAMPLE_RATE, std::move(path),
        capture_video, Host::GetSmallStringSettingValue("MediaCapture", "VideoCodec"),
        Host::GetUIntSettingValue("MediaCapture", "VideoBitrate", Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_BITRATE),
        Host::GetBoolSettingValue("MediaCapture", "VideoCodecUseArgs", false) ?
          Host::GetStringSettingValue("MediaCapture", "AudioCodecArgs") :
          std::string(),
        capture_audio, Host::GetSmallStringSettingValue("MediaCapture", "AudioCodec"),
        Host::GetUIntSettingValue("MediaCapture", "AudioBitrate", Settings::DEFAULT_MEDIA_CAPTURE_AUDIO_BITRATE),
        Host::GetBoolSettingValue("MediaCapture", "AudioCodecUseArgs", false) ?
          Host::GetStringSettingValue("MediaCapture", "AudioCodecArgs") :
          std::string(),
        &error))
  {
    Host::AddIconOSDWarning(
      "MediaCapture", ICON_FA_EXCLAMATION_TRIANGLE,
      fmt::format(TRANSLATE_FS("System", "Failed to create media capture: {0}"), error.GetDescription()),
      Host::OSD_ERROR_DURATION);
    s_state.media_capture.reset();
    Host::OnMediaCaptureStopped();
    return false;
  }

  Host::AddIconOSDMessage(fmt::format("MediaCapture_{}", s_state.media_capture->GetPath()), ICON_FA_CAMERA,
                          fmt::format(TRANSLATE_FS("System", "Starting {0} to '{1}'."),
                                      GetCaptureTypeForMessage(s_state.media_capture->IsCapturingVideo(),
                                                               s_state.media_capture->IsCapturingAudio()),
                                      Path::GetFileName(s_state.media_capture->GetPath())),
                          Host::OSD_INFO_DURATION);

  Host::OnMediaCaptureStarted();
  return true;
}

void System::StopMediaCapture()
{
  if (!s_state.media_capture)
    return;

  if (s_state.media_capture->IsCapturingVideo())
  {
    // If we're capturing video, we need to finish the capture on the GPU thread.
    // This is because it owns texture objects, and OpenGL is not thread-safe.
    GPUThread::RunOnThread(
      [cap = s_state.media_capture.release()]() mutable { StopMediaCapture(std::unique_ptr<MediaCapture>(cap)); });
  }
  else
  {
    // Otherwise, we can do it on the CPU thread.
    StopMediaCapture(std::move(s_state.media_capture));
  }

  Host::OnMediaCaptureStopped();
}

void System::StopMediaCapture(std::unique_ptr<MediaCapture> cap)
{
  const bool was_capturing_audio = cap->IsCapturingAudio();
  const bool was_capturing_video = cap->IsCapturingVideo();

  Error error;
  std::string osd_key = fmt::format("MediaCapture_{}", cap->GetPath());
  if (cap->EndCapture(&error))
  {
    Host::AddIconOSDMessage(std::move(osd_key), ICON_FA_CAMERA,
                            fmt::format(TRANSLATE_FS("System", "Stopped {0} to '{1}'."),
                                        GetCaptureTypeForMessage(was_capturing_video, was_capturing_audio),
                                        Path::GetFileName(cap->GetPath())),
                            Host::OSD_INFO_DURATION);
  }
  else
  {
    Host::AddIconOSDWarning(std::move(osd_key), ICON_FA_EXCLAMATION_TRIANGLE,
                            fmt::format(TRANSLATE_FS("System", "Stopped {0}: {1}."),
                                        GetCaptureTypeForMessage(was_capturing_video, was_capturing_audio),
                                        error.GetDescription()),
                            Host::OSD_INFO_DURATION);
  }
}

std::string System::GetGameSaveStateFileName(std::string_view serial, s32 slot)
{
  if (slot < 0)
    return Path::Combine(EmuFolders::SaveStates, fmt::format("{}_resume.sav", serial));
  else
    return Path::Combine(EmuFolders::SaveStates, fmt::format("{}_{}.sav", serial, slot));
}

std::string System::GetGlobalSaveStateFileName(s32 slot)
{
  if (slot < 0)
    return Path::Combine(EmuFolders::SaveStates, "resume.sav");
  else
    return Path::Combine(EmuFolders::SaveStates, fmt::format("savestate_{}.sav", slot));
}

std::vector<SaveStateInfo> System::GetAvailableSaveStates(std::string_view serial)
{
  std::vector<SaveStateInfo> si;
  std::string path;

  auto add_path = [&si](std::string path, s32 slot, bool global) {
    FILESYSTEM_STAT_DATA sd;
    if (!FileSystem::StatFile(path.c_str(), &sd))
      return;

    si.push_back(SaveStateInfo{std::move(path), sd.ModificationTime, static_cast<s32>(slot), global});
  };

  FlushSaveStates();

  if (!serial.empty())
  {
    add_path(GetGameSaveStateFileName(serial, -1), -1, false);
    for (s32 i = 1; i <= PER_GAME_SAVE_STATE_SLOTS; i++)
      add_path(GetGameSaveStateFileName(serial, i), i, false);
  }

  for (s32 i = 1; i <= GLOBAL_SAVE_STATE_SLOTS; i++)
    add_path(GetGlobalSaveStateFileName(i), i, true);

  return si;
}

std::optional<SaveStateInfo> System::GetSaveStateInfo(std::string_view serial, s32 slot)
{
  const bool global = serial.empty();
  std::string path = global ? GetGlobalSaveStateFileName(slot) : GetGameSaveStateFileName(serial, slot);

  FlushSaveStates();

  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
    return std::nullopt;

  return SaveStateInfo{std::move(path), sd.ModificationTime, slot, global};
}

std::optional<ExtendedSaveStateInfo> System::GetExtendedSaveStateInfo(const char* path)
{
  std::optional<ExtendedSaveStateInfo> ssi;

  FlushSaveStates();

  Error error;
  auto fp = FileSystem::OpenManagedCFile(path, "rb", &error);
  if (fp)
  {
    ssi.emplace();

    SaveStateBuffer buffer;
    if (LoadStateBufferFromFile(&buffer, fp.get(), &error, true, true, true, false)) [[likely]]
    {
      ssi->title = std::move(buffer.title);
      ssi->serial = std::move(buffer.serial);
      ssi->media_path = std::move(buffer.media_path);
      ssi->screenshot = std::move(buffer.screenshot);

      FILESYSTEM_STAT_DATA sd;
      ssi->timestamp = FileSystem::StatFile(fp.get(), &sd) ? sd.ModificationTime : 0;
    }
    else
    {
      ssi->title = error.GetDescription();
      ssi->timestamp = 0;
    }
  }

  return ssi;
}

void System::DeleteSaveStates(std::string_view serial, bool resume)
{
  const std::vector<SaveStateInfo> states(GetAvailableSaveStates(serial));
  for (const SaveStateInfo& si : states)
  {
    if (si.global || (!resume && si.slot < 0))
      continue;

    INFO_LOG("Removing save state '{}'", Path::GetFileName(si.path));

    Error error;
    if (!FileSystem::DeleteFile(si.path.c_str(), &error)) [[unlikely]]
      ERROR_LOG("Failed to delete save state file '{}': {}", Path::GetFileName(si.path), error.GetDescription());
  }
}

std::string System::GetGameMemoryCardPath(std::string_view serial, std::string_view path, u32 slot,
                                          MemoryCardType* out_type)
{
  const char* section = "MemoryCards";
  const TinyString type_key = TinyString::from_format("Card{}Type", slot + 1);
  const MemoryCardType default_type =
    (slot == 0) ? Settings::DEFAULT_MEMORY_CARD_1_TYPE : Settings::DEFAULT_MEMORY_CARD_2_TYPE;
  const MemoryCardType global_type =
    Settings::ParseMemoryCardTypeName(
      Host::GetBaseTinyStringSettingValue(section, type_key, Settings::GetMemoryCardTypeName(default_type)))
      .value_or(default_type);

  MemoryCardType type = global_type;
  std::unique_ptr<INISettingsInterface> ini;
  if (!serial.empty())
  {
    ini = GetGameSettingsInterface(GameDatabase::GetEntryForSerial(serial), serial, false, true);
    if (ini && ini->ContainsValue(section, type_key))
    {
      type = Settings::ParseMemoryCardTypeName(
               ini->GetTinyStringValue(section, type_key, Settings::GetMemoryCardTypeName(global_type)))
               .value_or(global_type);
    }
  }
  else if (type == MemoryCardType::PerGame)
  {
    // always shared without serial
    type = MemoryCardType::Shared;
  }

  if (out_type)
    *out_type = type;

  std::string ret;
  switch (type)
  {
    case MemoryCardType::None:
      break;

    case MemoryCardType::Shared:
    {
      const TinyString path_key = TinyString::from_format("Card{}Path", slot + 1);
      std::string global_path =
        Host::GetBaseStringSettingValue(section, path_key, Settings::GetDefaultSharedMemoryCardName(slot + 1).c_str());
      if (ini && ini->ContainsValue(section, path_key))
        ret = ini->GetStringValue(section, path_key, global_path.c_str());
      else
        ret = std::move(global_path);

      if (!Path::IsAbsolute(ret))
        ret = Path::Combine(EmuFolders::MemoryCards, ret);
    }
    break;

    case MemoryCardType::PerGame:
      ret = g_settings.GetGameMemoryCardPath(serial, slot);
      break;

    case MemoryCardType::PerGameTitle:
    {
      const GameDatabase::Entry* entry = GameDatabase::GetEntryForSerial(serial);
      if (entry)
      {
        ret = g_settings.GetGameMemoryCardPath(Path::SanitizeFileName(entry->title), slot);

        // Use disc set name if there isn't a per-disc card present.
        const bool global_use_playlist_title = Host::GetBaseBoolSettingValue(section, "UsePlaylistTitle", true);
        const bool use_playlist_title =
          ini ? ini->GetBoolValue(section, "UsePlaylistTitle", global_use_playlist_title) : global_use_playlist_title;
        if (!entry->disc_set_name.empty() && use_playlist_title && !FileSystem::FileExists(ret.c_str()))
          ret = g_settings.GetGameMemoryCardPath(Path::SanitizeFileName(entry->disc_set_name), slot);
      }
      else
      {
        ret = g_settings.GetGameMemoryCardPath(
          Path::SanitizeFileName(Path::GetFileTitle(FileSystem::GetDisplayNameFromPath(path))), slot);
      }
    }
    break;

    case MemoryCardType::PerGameFileTitle:
    {
      ret = g_settings.GetGameMemoryCardPath(
        Path::SanitizeFileName(Path::GetFileTitle(FileSystem::GetDisplayNameFromPath(path))), slot);
    }
    break;
    default:
      break;
  }

  return ret;
}

std::string System::GetMostRecentResumeSaveStatePath()
{
  FlushSaveStates();

  std::vector<FILESYSTEM_FIND_DATA> files;
  if (!FileSystem::FindFiles(EmuFolders::SaveStates.c_str(), "*resume.sav", FILESYSTEM_FIND_FILES, &files) ||
      files.empty())
  {
    return {};
  }

  FILESYSTEM_FIND_DATA* most_recent = &files[0];
  for (FILESYSTEM_FIND_DATA& file : files)
  {
    if (file.ModificationTime > most_recent->ModificationTime)
      most_recent = &file;
  }

  return std::move(most_recent->FileName);
}

std::string System::GetCheatFileName()
{
  std::string ret;

  const std::string& title = System::GetGameTitle();
  if (!title.empty())
    ret = Path::Combine(EmuFolders::Cheats, fmt::format("{}.cht", title.c_str()));

  return ret;
}

void System::ToggleWidescreen()
{
  g_settings.gpu_widescreen_hack = !g_settings.gpu_widescreen_hack;

  const DisplayAspectRatio user_ratio =
    Settings::ParseDisplayAspectRatio(
      Host::GetStringSettingValue("Display", "AspectRatio",
                                  Settings::GetDisplayAspectRatioName(Settings::DEFAULT_DISPLAY_ASPECT_RATIO))
        .c_str())
      .value_or(DisplayAspectRatio::Auto);
  ;

  if (user_ratio == DisplayAspectRatio::Auto || user_ratio == DisplayAspectRatio::PAR1_1 ||
      user_ratio == DisplayAspectRatio::R4_3)
  {
    g_settings.display_aspect_ratio = g_settings.gpu_widescreen_hack ? DisplayAspectRatio::R16_9 : user_ratio;
  }
  else
  {
    g_settings.display_aspect_ratio = g_settings.gpu_widescreen_hack ? user_ratio : DisplayAspectRatio::Auto;
  }

  if (g_settings.gpu_widescreen_hack)
  {
    Host::AddKeyedOSDMessage(
      "WidescreenHack",
      fmt::format(TRANSLATE_FS("OSDMessage", "Widescreen hack is now enabled, and aspect ratio is set to {}."),
                  Settings::GetDisplayAspectRatioDisplayName(g_settings.display_aspect_ratio)),
      5.0f);
  }
  else
  {
    Host::AddKeyedOSDMessage(
      "WidescreenHack",
      fmt::format(TRANSLATE_FS("OSDMessage", "Widescreen hack is now disabled, and aspect ratio is set to {}."),
                  Settings::GetDisplayAspectRatioDisplayName(g_settings.display_aspect_ratio), 5.0f));
  }

  UpdateGTEAspectRatio();
}

void System::ToggleSoftwareRendering()
{
  if (IsShutdown() || g_settings.gpu_renderer == GPURenderer::Software)
    return;

  const GPURenderer new_renderer =
    GPUBackend::IsUsingHardwareBackend() ? GPURenderer::Software : g_settings.gpu_renderer;

  Host::AddIconOSDMessage("SoftwareRendering", ICON_FA_PAINT_ROLLER,
                          fmt::format(TRANSLATE_FS("OSDMessage", "Switching to {} renderer..."),
                                      Settings::GetRendererDisplayName(new_renderer)),
                          Host::OSD_QUICK_DURATION);

  RecreateGPU(new_renderer);
}

void System::RequestDisplaySize(float scale /*= 0.0f*/)
{
  if (!IsValid())
    return;

  if (scale == 0.0f)
    scale = GPUBackend::IsUsingHardwareBackend() ? static_cast<float>(g_settings.gpu_resolution_scale) : 1.0f;

  float requested_width, requested_height;
  if (g_settings.gpu_show_vram)
  {
    requested_width = static_cast<float>(VRAM_WIDTH) * scale;
    requested_height = static_cast<float>(VRAM_HEIGHT) * scale;
  }
  else
  {
    requested_width = static_cast<float>(g_gpu.GetCRTCDisplayWidth()) * scale;
    requested_height = static_cast<float>(g_gpu.GetCRTCDisplayHeight()) * scale;
    g_gpu.ApplyPixelAspectRatioToSize(g_gpu.ComputePixelAspectRatio(), &requested_width, &requested_height);
  }

  if (g_settings.display_rotation == DisplayRotation::Rotate90 ||
      g_settings.display_rotation == DisplayRotation::Rotate270)
  {
    std::swap(requested_width, requested_height);
  }

  Host::RequestResizeHostDisplay(static_cast<s32>(std::ceil(requested_width)),
                                 static_cast<s32>(std::ceil(requested_height)));
}

void System::DisplayWindowResized()
{
  if (!IsValid())
    return;

  UpdateGTEAspectRatio();
}

void System::UpdateGTEAspectRatio()
{
  if (!IsValidOrInitializing())
    return;

  DisplayAspectRatio gte_ar = g_settings.display_aspect_ratio;
  u32 custom_num = 0;
  u32 custom_denom = 0;
  if (!g_settings.gpu_widescreen_hack)
  {
    // No WS hack => no correction.
    gte_ar = DisplayAspectRatio::R4_3;
  }
  else if (gte_ar == DisplayAspectRatio::Custom)
  {
    // Custom AR => use values.
    custom_num = g_settings.display_aspect_ratio_custom_numerator;
    custom_denom = g_settings.display_aspect_ratio_custom_denominator;
  }
  else if (gte_ar == DisplayAspectRatio::MatchWindow)
  {
    if (const WindowInfo& main_window_info = GPUThread::GetRenderWindowInfo(); !main_window_info.IsSurfaceless())
    {
      // Pre-apply the native aspect ratio correction to the window size.
      // MatchWindow does not correct the display aspect ratio, so we need to apply it here.
      const float correction = g_gpu.ComputeAspectRatioCorrection();
      custom_num =
        static_cast<u32>(std::max(std::round(static_cast<float>(main_window_info.surface_width) / correction), 1.0f));
      custom_denom = std::max<u32>(main_window_info.surface_height, 1u);
      gte_ar = DisplayAspectRatio::Custom;
    }
    else
    {
      // Assume 4:3 until we get a window.
      gte_ar = DisplayAspectRatio::R4_3;
    }
  }

  GTE::SetAspectRatio(gte_ar, custom_num, custom_denom);
}

bool System::ChangeGPUDump(std::string new_path)
{
  Error error;
  std::unique_ptr<GPUDump::Player> new_dump = GPUDump::Player::Open(std::move(new_path), &error);
  if (!new_dump)
  {
    Host::ReportErrorAsync("Error", fmt::format(TRANSLATE_FS("Failed to change GPU dump: {}", error.GetDescription())));
    return false;
  }

  s_state.gpu_dump_player = std::move(new_dump);
  s_state.region = s_state.gpu_dump_player->GetRegion();
  UpdateRunningGame(s_state.gpu_dump_player->GetPath(), nullptr, false);

  // current player object has been changed out, toss call stack
  InterruptExecution();
  return true;
}

void System::UpdateSessionTime(const std::string& prev_serial)
{
  const Timer::Value ctime = Timer::GetCurrentValue();
  if (!prev_serial.empty() && GameList::IsGameListLoaded())
  {
    // round up to seconds
    const std::time_t etime =
      static_cast<std::time_t>(std::round(Timer::ConvertValueToSeconds(ctime - s_state.session_start_time)));
    const std::time_t wtime = std::time(nullptr);
    GameList::AddPlayedTimeForSerial(prev_serial, wtime, etime);
  }

  s_state.session_start_time = ctime;
}

u64 System::GetSessionPlayedTime()
{
  const Timer::Value ctime = Timer::GetCurrentValue();
  return static_cast<u64>(std::round(Timer::ConvertValueToSeconds(ctime - s_state.session_start_time)));
}

void System::QueueAsyncTask(std::function<void()> function)
{
  s_state.async_task_queue.SubmitTask(std::move(function));
}

void System::WaitForAllAsyncTasks()
{
  s_state.async_task_queue.WaitForAll();
}

SocketMultiplexer* System::GetSocketMultiplexer()
{
#ifdef ENABLE_SOCKET_MULTIPLEXER
  if (s_state.socket_multiplexer)
    return s_state.socket_multiplexer.get();

  Error error;
  s_state.socket_multiplexer = SocketMultiplexer::Create(&error);
  if (s_state.socket_multiplexer)
    INFO_LOG("Created socket multiplexer.");
  else
    ERROR_LOG("Failed to create socket multiplexer: {}", error.GetDescription());

  return s_state.socket_multiplexer.get();
#else
  ERROR_LOG("This build does not support sockets.");
  return nullptr;
#endif
}

void System::ReleaseSocketMultiplexer()
{
#ifdef ENABLE_SOCKET_MULTIPLEXER
  if (!s_state.socket_multiplexer || s_state.socket_multiplexer->HasAnyOpenSockets())
    return;

  INFO_LOG("Destroying socket multiplexer.");
  s_state.socket_multiplexer.reset();
#endif
}

#ifdef ENABLE_DISCORD_PRESENCE

#include "discord_rpc.h"

#define DISCORD_RPC_FUNCTIONS(X)                                                                                       \
  X(Discord_Initialize)                                                                                                \
  X(Discord_Shutdown)                                                                                                  \
  X(Discord_RunCallbacks)                                                                                              \
  X(Discord_UpdatePresence)                                                                                            \
  X(Discord_ClearPresence)

namespace dyn_libs {
static bool OpenDiscordRPC(Error* error);
static void CloseDiscordRPC();

static DynamicLibrary s_discord_rpc_library;

#define ADD_FUNC(F) static decltype(&::F) F;
DISCORD_RPC_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC
} // namespace dyn_libs

bool dyn_libs::OpenDiscordRPC(Error* error)
{
  if (s_discord_rpc_library.IsOpen())
    return true;

  const std::string libname = DynamicLibrary::GetVersionedFilename("discord-rpc");
  if (!s_discord_rpc_library.Open(libname.c_str(), error))
  {
    Error::AddPrefix(error, "Failed to load discord-rpc: ");
    return false;
  }

#define LOAD_FUNC(F)                                                                                                   \
  if (!s_discord_rpc_library.GetSymbol(#F, &F))                                                                        \
  {                                                                                                                    \
    Error::SetStringFmt(error, "Failed to find function {}", #F);                                                      \
    CloseDiscordRPC();                                                                                                 \
    return false;                                                                                                      \
  }
  DISCORD_RPC_FUNCTIONS(LOAD_FUNC)
#undef LOAD_FUNC

  return true;
}

void dyn_libs::CloseDiscordRPC()
{
  if (!s_discord_rpc_library.IsOpen())
    return;

#define UNLOAD_FUNC(F) F = nullptr;
  DISCORD_RPC_FUNCTIONS(UNLOAD_FUNC)
#undef UNLOAD_FUNC

  s_discord_rpc_library.Close();
}

void System::InitializeDiscordPresence()
{
  if (s_state.discord_presence_active)
    return;

  Error error;
  if (!dyn_libs::OpenDiscordRPC(&error))
  {
    ERROR_LOG("Failed to open discord-rpc: {}", error.GetDescription());
    return;
  }

  DiscordEventHandlers handlers = {};
  dyn_libs::Discord_Initialize("705325712680288296", &handlers, 0, nullptr);
  s_state.discord_presence_active = true;

  UpdateRichPresence(true);
}

void System::ShutdownDiscordPresence()
{
  if (!s_state.discord_presence_active)
    return;

  dyn_libs::Discord_ClearPresence();
  dyn_libs::Discord_Shutdown();
  dyn_libs::CloseDiscordRPC();

  s_state.discord_presence_active = false;
}

void System::UpdateRichPresence(bool update_session_time)
{
  if (!s_state.discord_presence_active)
    return;

  if (update_session_time)
    s_state.discord_presence_time_epoch = std::time(nullptr);

  // https://discord.com/developers/docs/rich-presence/how-to#updating-presence-update-presence-payload-fields
  DiscordRichPresence rp = {};
  rp.largeImageKey = "duckstation_logo";
  rp.largeImageText = "DuckStation PS1/PSX Emulator";
  rp.startTimestamp = s_state.discord_presence_time_epoch;
  rp.details = "No Game Running";
  if (IsValidOrInitializing())
  {
    // Use disc set name if it's not a custom title.
    if (s_state.running_game_entry && !s_state.running_game_entry->disc_set_name.empty() &&
        s_state.running_game_title == s_state.running_game_entry->title)
    {
      rp.details = s_state.running_game_entry->disc_set_name.c_str();
    }
    else
    {
      rp.details = s_state.running_game_title.empty() ? "Unknown Game" : s_state.running_game_title.c_str();
    }
  }

  const auto lock = Achievements::GetLock();

  std::string state_string;
  if (Achievements::HasRichPresence())
    rp.state = (state_string = StringUtil::Ellipsise(Achievements::GetRichPresenceString(), 128)).c_str();

  if (const std::string& icon_url = Achievements::GetGameIconURL(); !icon_url.empty())
    rp.largeImageKey = icon_url.c_str();

  dyn_libs::Discord_UpdatePresence(&rp);
}

void System::PollDiscordPresence()
{
  if (!s_state.discord_presence_active)
    return;

  dyn_libs::Discord_RunCallbacks();
}

#else

void System::UpdateRichPresence(bool update_session_time)
{
}

#endif
