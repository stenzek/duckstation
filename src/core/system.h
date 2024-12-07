// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "settings.h"
#include "types.h"

#include "util/image.h"

#include <memory>
#include <optional>
#include <span>
#include <string>

namespace Threading {
class ThreadHandle;
}

class CDImage;
class Error;
class SmallStringBase;
class StateWrapper;
class SocketMultiplexer;

enum class GPUVSyncMode : u8;

class Controller;

class GPUTexture;
class MediaCapture;

namespace BIOS {
struct ImageInfo;
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
  std::optional<bool> override_fast_boot;
  std::optional<bool> override_fullscreen;
  std::optional<bool> override_start_paused;
  u32 media_playlist_index = 0;
  bool load_image_to_ram = false;
  bool force_software_renderer = false;
  bool disable_achievements_hardcore_mode = false;
  bool start_media_capture = false;
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

  Image screenshot;
};

namespace System {

enum : s32
{
  PER_GAME_SAVE_STATE_SLOTS = 10,
  GLOBAL_SAVE_STATE_SLOTS = 10
};

enum : TickCount
{
  MASTER_CLOCK = 44100 * 0x300 // 33868800Hz or 33.8688MHz, also used as CPU clock
};

enum class State : u8
{
  Shutdown,
  Starting,
  Running,
  Paused,
  Stopping,
};

enum class BootMode : u8
{
  None,
  FullBoot,
  FastBoot,
  BootEXE,
  BootPSF,
  ReplayGPUDump,
};

enum class Taint : u8
{
  CPUOverclock,
  CDROMReadSpeedup,
  CDROMSeekSpeedup,
  ForceFrameTimings,
  RAM8MB,
  Cheats,
  Patches,
  MaxCount,
};

/// Returns true if the path is a PlayStation executable we can inject.
bool IsExePath(std::string_view path);

/// Returns true if the path is a Portable Sound Format file we can uncompress/load.
bool IsPsfPath(std::string_view path);

/// Returns true if the path is a GPU dump that we can replay.
bool IsGPUDumpPath(std::string_view path);

/// Returns true if the path is one we can load.
bool IsLoadablePath(std::string_view path);

/// Returns true if the path is a save state.
bool IsSaveStatePath(std::string_view path);

/// Returns the preferred console type for a disc.
ConsoleRegion GetConsoleRegionForDiscRegion(DiscRegion region);

std::string GetExecutableNameForImage(CDImage* cdi, bool strip_subdirectories);
bool ReadExecutableFromImage(CDImage* cdi, std::string* out_executable_name, std::vector<u8>* out_executable_data);

std::string GetGameHashId(GameHash hash);
bool GetGameDetailsFromImage(CDImage* cdi, std::string* out_id, GameHash* out_hash);
GameHash GetGameHashFromFile(const char* path);
GameHash GetGameHashFromBuffer(const std::string_view filename, const std::span<const u8> data);
DiscRegion GetRegionForSerial(const std::string_view serial);
DiscRegion GetRegionFromSystemArea(CDImage* cdi);
DiscRegion GetRegionForImage(CDImage* cdi);
DiscRegion GetRegionForExe(const char* path);
DiscRegion GetRegionForPsf(const char* path);

/// Returns the path for the game settings ini file for the specified serial.
std::string GetGameSettingsPath(std::string_view game_serial);

/// Returns the path for the input profile ini file with the specified name (may not exist).
std::string GetInputProfilePath(std::string_view name);

State GetState();
void SetState(State new_state);
bool IsRunning();
bool IsPaused();
bool IsShutdown();
bool IsValid();
bool IsValidOrInitializing();
bool IsExecuting();
bool IsReplayingGPUDump();

bool IsStartupCancelled();
void CancelPendingStartup();
void InterruptExecution();

ConsoleRegion GetRegion();
DiscRegion GetDiscRegion();
bool IsPALRegion();

/// Taints - flags that are set on the system and only cleared on reset.
std::string_view GetTaintDisplayName(Taint taint);
const char* GetTaintName(Taint taint);
bool HasTaint(Taint taint);
void SetTaint(Taint taint);

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

TickCount GetTicksPerSecond();
TickCount GetMaxSliceTicks();
void UpdateOverclock();

GlobalTicks GetGlobalTickCounter();
u32 GetFrameNumber();
u32 GetInternalFrameNumber();

const std::string& GetDiscPath();
const std::string& GetGameSerial();
const std::string& GetGameTitle();
const std::string& GetExeOverride();
const GameDatabase::Entry* GetGameDatabaseEntry();
GameHash GetGameHash();
bool IsRunningUnknownGame();
BootMode GetBootMode();

/// Returns the time elapsed in the current play session.
u64 GetSessionPlayedTime();

const BIOS::ImageInfo* GetBIOSImageInfo();

void FormatLatencyStats(SmallStringBase& str);

/// Loads global settings (i.e. EmuConfig).
void LoadSettings(bool display_osd_messages);
void SetDefaultSettings(SettingsInterface& si);

/// Reloads settings, and applies any changes present.
void ApplySettings(bool display_osd_messages);

/// Reloads game specific settings, and applys any changes present.
void ReloadGameSettings(bool display_osd_messages);

/// Reloads input profile, depending on whether it is a specific profile or game configuration.
void ReloadInputProfile(bool display_osd_messages);

/// Reloads input sources.
void ReloadInputSources();

/// Reloads input bindings.
void ReloadInputBindings();

/// Reloads only controller settings.
void UpdateControllerSettings();

bool BootSystem(SystemBootParameters parameters, Error* error);
void PauseSystem(bool paused);
void ResetSystem();

/// Loads state from the specified path.
bool LoadState(const char* path, Error* error, bool save_undo_state);
bool SaveState(std::string path, Error* error, bool backup_existing_save, bool ignore_memcard_busy);
bool SaveResumeState(Error* error);

/// Runs the VM until the CPU execution is canceled.
void Execute();

void SingleStepCPU();

/// Sets target emulation speed.
float GetTargetSpeed();
float GetAudioNominalRate();

/// Returns true if fast forwarding or slow motion is currently active.
bool IsRunningAtNonStandardSpeed();

/// Adjusts the throttle frequency, i.e. how many times we should sleep per second.
float GetVideoFrameRate();
void SetVideoFrameRate(float frequency);

// Access controllers for simulating input.
Controller* GetController(u32 slot);
void UpdateMemoryCardTypes();
bool HasMemoryCard(u32 slot);
bool IsSavingMemoryCards();

/// Swaps memory cards in slot 1/2.
void SwapMemoryCards();

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
u32 GetMediaSubImageIndexForTitle(std::string_view title);

/// Returns the path to the specified playlist index.
std::string GetMediaSubImageTitle(u32 index);

/// Switches to the specified media/disc playlist index.
bool SwitchMediaSubImage(u32 index);

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

/// Returns the path to a save state file. Specifying an index of -1 is the "resume" save state.
std::string GetGameSaveStateFileName(std::string_view serial, s32 slot);

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
std::vector<SaveStateInfo> GetAvailableSaveStates(std::string_view serial);

/// Returns save state info if present. If serial is null or empty, assumes global state.
std::optional<SaveStateInfo> GetSaveStateInfo(std::string_view serial, s32 slot);

/// Returns save state info from opened save state stream.
std::optional<ExtendedSaveStateInfo> GetExtendedSaveStateInfo(const char* path);

/// Deletes save states for the specified game code. If resume is set, the resume state is deleted too.
void DeleteSaveStates(std::string_view serial, bool resume);

/// Returns the path to the memory card for the specified game, considering game settings.
std::string GetGameMemoryCardPath(std::string_view serial, std::string_view path, u32 slot,
                                  MemoryCardType* out_type = nullptr);

/// Returns intended output volume considering fast forwarding.
s32 GetAudioOutputVolume();
void UpdateVolume();

/// Saves a screenshot to the specified file. If no file name is provided, one will be generated automatically.
bool SaveScreenshot(const char* path = nullptr, DisplayScreenshotMode mode = g_settings.display_screenshot_mode,
                    DisplayScreenshotFormat format = g_settings.display_screenshot_format,
                    u8 quality = g_settings.display_screenshot_quality, bool compress_on_thread = true);

/// Starts/stops GPU dump/trace recording.
bool StartRecordingGPUDump(const char* path = nullptr, u32 num_frames = 1);
void StopRecordingGPUDump();

/// Returns the path that a new media capture would be saved to by default. Safe to call from any thread.
std::string GetNewMediaCapturePath(const std::string_view title, const std::string_view container);

/// Current media capture (if active).
MediaCapture* GetMediaCapture();

/// Media capture (video and/or audio). If no path is provided, one will be generated automatically.
bool StartMediaCapture(std::string path = {});
bool StartMediaCapture(std::string path, bool capture_video, bool capture_audio);
void StopMediaCapture();

/// Toggle Widescreen Hack and Aspect Ratio
void ToggleWidescreen();

/// Quick switch between software and hardware rendering.
void ToggleSoftwareRendering();

/// Resizes the render window to the display size, with an optional scale.
/// If the scale is set to 0, the internal resolution will be used, otherwise it is treated as a multiplier to 1x.
void RequestDisplaySize(float scale = 0.0f);

/// Renders the display.
bool PresentDisplay(bool explicit_present, u64 present_time);
void InvalidateDisplay();

//////////////////////////////////////////////////////////////////////////
// Memory Save States (Rewind and Runahead)
//////////////////////////////////////////////////////////////////////////
void CalculateRewindMemoryUsage(u32 num_saves, u32 resolution_scale, u64* ram_usage, u64* vram_usage);
void ClearMemorySaveStates(bool deallocate_resources);
void SetRunaheadReplayFlag();

/// Shared socket multiplexer, used by PINE/GDB/etc.
SocketMultiplexer* GetSocketMultiplexer();
void ReleaseSocketMultiplexer();

/// Called when rich presence changes.
void UpdateRichPresence(bool update_session_time);

} // namespace System

namespace Host {

/// Requests shut down of the current virtual machine.
void RequestSystemShutdown(bool allow_confirm, bool save_state);

} // namespace Host
