// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "common/timer.h"
#include "settings.h"
#include "timing_event.h"
#include "types.h"
#include <memory>
#include <optional>
#include <string>

class ByteStream;
class CDImage;
class Error;
class StateWrapper;

class Controller;

struct CheatCode;
class CheatList;

class GPUTexture;
class GrowableMemoryByteStream;

namespace BIOS {
struct ImageInfo;
struct Hash;
} // namespace BIOS

namespace GameDatabase {
struct Entry;
}

struct SystemBootParameters
{
  SystemBootParameters();
  SystemBootParameters(const SystemBootParameters&);
  SystemBootParameters(SystemBootParameters&&);
  SystemBootParameters(std::string filename_);
  ~SystemBootParameters();

  std::string filename;
  std::string save_state;
  std::string override_exe;
  std::string override_bios;
  std::optional<bool> override_fast_boot;
  std::optional<bool> override_fullscreen;
  std::optional<bool> override_start_paused;
  u32 media_playlist_index = 0;
  bool load_image_to_ram = false;
  bool force_software_renderer = false;
  bool fast_forward_to_first_frame = false;
  bool disable_achievements_hardcore_mode = false;
};

struct SaveStateInfo
{
  std::string path;
  std::time_t timestamp;
  s32 slot;
  bool global;
};

struct ExtendedSaveStateInfo
{
  std::string title;
  std::string serial;
  std::string media_path;
  std::time_t timestamp;

  u32 screenshot_width;
  u32 screenshot_height;
  std::vector<u32> screenshot_data;
};

namespace System {

enum : u32
{
  // 5 megabytes is sufficient for now, at the moment they're around 4.3MB, or 10.3MB with 8MB RAM enabled.
  MAX_SAVE_STATE_SIZE = 11 * 1024 * 1024,
};

enum : s32
{
  PER_GAME_SAVE_STATE_SLOTS = 10,
  GLOBAL_SAVE_STATE_SLOTS = 10
};

enum : TickCount
{
  MASTER_CLOCK = 44100 * 0x300 // 33868800Hz or 33.8688MHz, also used as CPU clock
};

enum class State
{
  Shutdown,
  Starting,
  Running,
  Paused,
  Stopping,
};

using GameHash = u64;

extern TickCount g_ticks_per_second;

/// Returns true if the filename is a PlayStation executable we can inject.
bool IsExeFileName(const std::string_view& path);

/// Returns true if the filename is a Portable Sound Format file we can uncompress/load.
bool IsPsfFileName(const std::string_view& path);

/// Returns true if the filename is one we can load.
bool IsLoadableFilename(const std::string_view& path);

/// Returns true if the filename is a save state.
bool IsSaveStateFilename(const std::string_view& path);

/// Returns the preferred console type for a disc.
ConsoleRegion GetConsoleRegionForDiscRegion(DiscRegion region);

std::string GetExecutableNameForImage(CDImage* cdi, bool strip_subdirectories);
bool ReadExecutableFromImage(CDImage* cdi, std::string* out_executable_name, std::vector<u8>* out_executable_data);

std::string GetGameHashId(GameHash hash);
bool GetGameDetailsFromImage(CDImage* cdi, std::string* out_id, GameHash* out_hash);
DiscRegion GetRegionForSerial(std::string_view serial);
DiscRegion GetRegionFromSystemArea(CDImage* cdi);
DiscRegion GetRegionForImage(CDImage* cdi);
DiscRegion GetRegionForExe(const char* path);
DiscRegion GetRegionForPsf(const char* path);
std::optional<DiscRegion> GetRegionForPath(const char* image_path);

/// Returns the path for the game settings ini file for the specified serial.
std::string GetGameSettingsPath(const std::string_view& game_serial);

/// Returns the path for the input profile ini file with the specified name (may not exist).
std::string GetInputProfilePath(const std::string_view& name);

State GetState();
void SetState(State new_state);
bool IsRunning();
bool IsExecutionInterrupted();
bool IsPaused();
bool IsShutdown();
bool IsValid();
bool IsValidOrInitializing();
bool IsExecuting();

bool IsStartupCancelled();
void CancelPendingStartup();
void InterruptExecution();

ConsoleRegion GetRegion();
DiscRegion GetDiscRegion();
bool IsPALRegion();

ALWAYS_INLINE TickCount GetTicksPerSecond()
{
  return g_ticks_per_second;
}

ALWAYS_INLINE_RELEASE TickCount ScaleTicksToOverclock(TickCount ticks)
{
  if (!g_settings.cpu_overclock_active)
    return ticks;

  return static_cast<TickCount>(((static_cast<u64>(static_cast<u32>(ticks)) * g_settings.cpu_overclock_numerator) +
                                 (g_settings.cpu_overclock_denominator - 1)) /
                                g_settings.cpu_overclock_denominator);
}

ALWAYS_INLINE_RELEASE TickCount UnscaleTicksToOverclock(TickCount ticks, TickCount* remainder)
{
  if (!g_settings.cpu_overclock_active)
    return ticks;

  const u64 num =
    (static_cast<u32>(ticks) * static_cast<u64>(g_settings.cpu_overclock_denominator)) + static_cast<u32>(*remainder);
  const TickCount t = static_cast<u32>(num / g_settings.cpu_overclock_numerator);
  *remainder = static_cast<u32>(num % g_settings.cpu_overclock_numerator);
  return t;
}

TickCount GetMaxSliceTicks();
void UpdateOverclock();

/// Injects a PS-EXE into memory at its specified load location. If patch_loader is set, the BIOS will be patched to
/// direct execution to this executable.
bool InjectEXEFromBuffer(const void* buffer, u32 buffer_size, bool patch_loader = true);

u32 GetFrameNumber();
u32 GetInternalFrameNumber();
void IncrementInternalFrameNumber();
void FrameDone();

const std::string& GetDiscPath();
const std::string& GetGameSerial();
const std::string& GetGameTitle();
const GameDatabase::Entry* GetGameDatabaseEntry();
GameHash GetGameHash();
bool IsRunningUnknownGame();
bool WasFastBooted();

/// Returns the time elapsed in the current play session.
u64 GetSessionPlayedTime();

const BIOS::ImageInfo* GetBIOSImageInfo();
const BIOS::Hash& GetBIOSHash();

// TODO: Move to PerformanceMetrics
static constexpr u32 NUM_FRAME_TIME_SAMPLES = 150;
using FrameTimeHistory = std::array<float, NUM_FRAME_TIME_SAMPLES>;

float GetFPS();
float GetVPS();
float GetEmulationSpeed();
float GetAverageFrameTime();
float GetMinimumFrameTime();
float GetMaximumFrameTime();
float GetThrottleFrequency();
float GetCPUThreadUsage();
float GetCPUThreadAverageTime();
float GetSWThreadUsage();
float GetSWThreadAverageTime();
float GetGPUUsage();
float GetGPUAverageTime();
const FrameTimeHistory& GetFrameTimeHistory();
u32 GetFrameTimeHistoryPos();

/// Loads global settings (i.e. EmuConfig).
void LoadSettings(bool display_osd_messages);
void SetDefaultSettings(SettingsInterface& si);

/// Reloads settings, and applies any changes present.
void ApplySettings(bool display_osd_messages);

/// Reloads game specific settings, and applys any changes present.
bool ReloadGameSettings(bool display_osd_messages);

bool BootSystem(SystemBootParameters parameters);
void PauseSystem(bool paused);
void ResetSystem();

/// Loads state from the specified filename.
bool LoadState(const char* filename);
bool SaveState(const char* filename, bool backup_existing_save);
bool SaveResumeState();

/// Memory save states - only for internal use.
struct MemorySaveState
{
  std::unique_ptr<GPUTexture> vram_texture;
  std::unique_ptr<GrowableMemoryByteStream> state_stream;
};
bool SaveMemoryState(MemorySaveState* mss);
bool LoadMemoryState(const MemorySaveState& mss);
bool LoadStateFromStream(ByteStream* stream, bool update_display, bool ignore_media = false);
bool SaveStateToStream(ByteStream* state, u32 screenshot_size = 256, u32 compression_method = 0,
                       bool ignore_media = false);

/// Runs the VM until the CPU execution is canceled.
void Execute();

/// Switches the GPU renderer by saving state, recreating the display window, and restoring state (if needed).
void RecreateSystem();

/// Recreates the GPU component, saving/loading the state so it is preserved. Call when the GPU renderer changes.
bool RecreateGPU(GPURenderer renderer, bool force_recreate_device = false, bool update_display = true);

void SingleStepCPU();

/// Sets target emulation speed.
float GetTargetSpeed();

/// Adjusts the throttle frequency, i.e. how many times we should sleep per second.
void SetThrottleFrequency(float frequency);

/// Updates the throttle period, call when target emulation speed changes.
void UpdateThrottlePeriod();
void ResetThrottler();

void UpdatePerformanceCounters();
void ResetPerformanceCounters();

/// Resets vsync/max present fps state.
void UpdateDisplaySync();

// Access controllers for simulating input.
Controller* GetController(u32 slot);
void UpdateControllers();
void UpdateControllerSettings();
void ResetControllers();
void UpdateMemoryCardTypes();
void UpdatePerGameMemoryCards();
bool HasMemoryCard(u32 slot);

/// Swaps memory cards in slot 1/2.
void SwapMemoryCards();

void UpdateMultitaps();

/// Dumps RAM to a file.
bool DumpRAM(const char* filename);

/// Dumps video RAM to a file.
bool DumpVRAM(const char* filename);

/// Dumps sound RAM to a file.
bool DumpSPURAM(const char* filename);

bool HasMedia();
std::string GetMediaFileName();
bool InsertMedia(const char* path);
void RemoveMedia();

/// Returns true if this is a multi-subimage image (e.g. m3u).
bool HasMediaSubImages();

/// Returns the number of entries in the media/disc playlist.
u32 GetMediaSubImageCount();

/// Returns the current image from the media/disc playlist.
u32 GetMediaSubImageIndex();

/// Returns the index of the specified path in the playlist, or UINT32_MAX if it does not exist.
u32 GetMediaSubImageIndexForTitle(const std::string_view& title);

/// Returns the path to the specified playlist index.
std::string GetMediaSubImageTitle(u32 index);

/// Switches to the specified media/disc playlist index.
bool SwitchMediaSubImage(u32 index);

/// Returns true if there is currently a cheat list.
bool HasCheatList();

/// Accesses the current cheat list.
CheatList* GetCheatList();

/// Applies a single cheat code.
void ApplyCheatCode(const CheatCode& code);

/// Sets or clears the provided cheat list, applying every frame.
void SetCheatList(std::unique_ptr<CheatList> cheats);

/// Checks for settings changes, std::move() the old settings away for comparing beforehand.
void CheckForSettingsChanges(const Settings& old_settings);

/// Updates throttler.
void UpdateSpeedLimiterState();

/// Toggles fast forward state.
bool IsFastForwardEnabled();
void SetFastForwardEnabled(bool enabled);

/// Toggles turbo state.
bool IsTurboEnabled();
void SetTurboEnabled(bool enabled);

/// Toggles rewind state.
bool IsRewinding();
void SetRewindState(bool enabled);

void DoFrameStep();
void DoToggleCheats();

/// Returns the path to a save state file. Specifying an index of -1 is the "resume" save state.
std::string GetGameSaveStateFileName(const std::string_view& serial, s32 slot);

/// Returns the path to a save state file. Specifying an index of -1 is the "resume" save state.
std::string GetGlobalSaveStateFileName(s32 slot);

/// Returns the most recent resume save state.
std::string GetMostRecentResumeSaveStatePath();

/// Returns the path to the cheat file for the specified game title.
std::string GetCheatFileName();

/// Powers off the system, optionally saving the resume state.
void ShutdownSystem(bool save_resume_state);

/// Returns true if an undo load state exists.
bool CanUndoLoadState();

/// Returns save state info for the undo slot, if present.
std::optional<ExtendedSaveStateInfo> GetUndoSaveStateInfo();

/// Undoes a load state, i.e. restores the state prior to the load.
bool UndoLoadState();

/// Returns a list of save states for the specified game code.
std::vector<SaveStateInfo> GetAvailableSaveStates(const char* serial);

/// Returns save state info if present. If serial is null or empty, assumes global state.
std::optional<SaveStateInfo> GetSaveStateInfo(const char* serial, s32 slot);

/// Returns save state info from opened save state stream.
std::optional<ExtendedSaveStateInfo> GetExtendedSaveStateInfo(const char* path);

/// Deletes save states for the specified game code. If resume is set, the resume state is deleted too.
void DeleteSaveStates(const char* serial, bool resume);

/// Returns intended output volume considering fast forwarding.
s32 GetAudioOutputVolume();
void UpdateVolume();

/// Returns true if currently dumping audio.
bool IsDumpingAudio();

/// Starts dumping audio to a file. If no file name is provided, one will be generated automatically.
bool StartDumpingAudio(const char* filename = nullptr);

/// Stops dumping audio to file if it has been started.
void StopDumpingAudio();

/// Saves a screenshot to the specified file. IF no file name is provided, one will be generated automatically.
bool SaveScreenshot(const char* filename = nullptr, bool full_resolution = true, bool apply_aspect_ratio = true,
                    bool compress_on_thread = true);

/// Loads the cheat list from the specified file.
bool LoadCheatList(const char* filename);

/// Loads the cheat list for the current game title from the user directory.
bool LoadCheatListFromGameTitle();

/// Loads the cheat list for the current game code from the built-in code database.
bool LoadCheatListFromDatabase();

/// Saves the current cheat list to the game title's file.
bool SaveCheatList();

/// Saves the current cheat list to the specified file.
bool SaveCheatList(const char* filename);

/// Deletes the cheat list, if present.
bool DeleteCheatList();

/// Removes all cheats from the cheat list.
void ClearCheatList(bool save_to_file);

/// Enables/disabled the specified cheat code.
void SetCheatCodeState(u32 index, bool enabled, bool save_to_file);

/// Immediately applies the specified cheat code.
void ApplyCheatCode(u32 index);

/// Toggle Widescreen Hack and Aspect Ratio
void ToggleWidescreen();

/// Returns true if fast forwarding or slow motion is currently active.
bool IsRunningAtNonStandardSpeed();

/// Returns true if vsync should be used.
bool ShouldUseVSync();

/// Quick switch between software and hardware rendering.
void ToggleSoftwareRendering();

/// Resizes the render window to the display size, with an optional scale.
/// If the scale is set to 0, the internal resolution will be used, otherwise it is treated as a multiplier to 1x.
void RequestDisplaySize(float scale = 0.0f);

/// Call when host display size changes, use with "match display" aspect ratio setting.
void HostDisplayResized();

/// Renders the display.
bool PresentDisplay(bool allow_skip_present);
void InvalidateDisplay();

//////////////////////////////////////////////////////////////////////////
// Memory Save States (Rewind and Runahead)
//////////////////////////////////////////////////////////////////////////
void CalculateRewindMemoryUsage(u32 num_saves, u32 resolution_scale, u64* ram_usage, u64* vram_usage);
void ClearMemorySaveStates();
void UpdateMemorySaveStateSettings();
bool LoadRewindState(u32 skip_saves = 0, bool consume_state = true);
void SetRunaheadReplayFlag();

#ifdef ENABLE_DISCORD_PRESENCE
/// Called when rich presence changes.
void UpdateDiscordPresence(bool update_session_time);
#endif

namespace Internal {
/// Called on process startup.
bool ProcessStartup();

/// Called on process shutdown.
void ProcessShutdown();

/// Polls input, updates subsystems which are present while paused/inactive.
void IdlePollUpdate();
} // namespace Internal

} // namespace System

namespace Host {
/// Called with the settings lock held, when system settings are being loaded (should load input sources, etc).
void LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock);

/// Called after settings are updated.
void CheckForSettingsChanges(const Settings& old_settings);

/// Called when the VM is starting initialization, but has not been completed yet.
void OnSystemStarting();

/// Called when the VM is created.
void OnSystemStarted();

/// Called when the VM is shut down or destroyed.
void OnSystemDestroyed();

/// Called when the VM is paused.
void OnSystemPaused();

/// Called when the VM is resumed after being paused.
void OnSystemResumed();

/// Called when the pause state changes, or fullscreen UI opens.
void OnIdleStateChanged();

/// Called when performance metrics are updated, approximately once a second.
void OnPerformanceCountersUpdated();

/// Provided by the host; called when the running executable changes.
void OnGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name);

/// Provided by the host; called once per frame at guest vsync.
void PumpMessagesOnCPUThread();

/// Requests a specific display window size.
void RequestResizeHostDisplay(s32 width, s32 height);

/// Requests shut down of the current virtual machine.
void RequestSystemShutdown(bool allow_confirm, bool save_state);
} // namespace Host
