#pragma once
#include "common/timer.h"
#include "host_interface.h"
#include "settings.h"
#include "timing_event.h"
#include "types.h"
#include <memory>
#include <optional>
#include <string>

class ByteStream;
class CDImage;
class StateWrapper;

class Controller;

struct CheatCode;
class CheatList;

struct SystemBootParameters
{
  SystemBootParameters();
  SystemBootParameters(SystemBootParameters&& other);
  SystemBootParameters(std::string filename_);
  ~SystemBootParameters();

  std::string filename;
  std::optional<bool> override_fast_boot;
  std::optional<bool> override_fullscreen;
  std::optional<bool> override_start_paused;
  std::unique_ptr<ByteStream> state_stream;
  u32 media_playlist_index = 0;
  bool load_image_to_ram = false;
  bool force_software_renderer = false;
};

namespace System {

enum : u32
{
  // 5 megabytes is sufficient for now, at the moment they're around 4.3MB, or 10.3MB with 8MB RAM enabled.
  MAX_SAVE_STATE_SIZE = 11 * 1024 * 1024
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
  Paused
};

extern TickCount g_ticks_per_second;

/// Returns true if the filename is a PlayStation executable we can inject.
bool IsExeFileName(const char* path);

/// Returns true if the filename is a Portable Sound Format file we can uncompress/load.
bool IsPsfFileName(const char* path);

/// Returns true if the filename is one we can load.
bool IsLoadableFilename(const char* path);

/// Returns the preferred console type for a disc.
ConsoleRegion GetConsoleRegionForDiscRegion(DiscRegion region);

std::string GetExecutableNameForImage(CDImage* cdi);
bool ReadExecutableFromImage(CDImage* cdi, std::string* out_executable_name, std::vector<u8>* out_executable_data);

std::string GetGameHashCodeForImage(CDImage* cdi);
std::string GetGameCodeForImage(CDImage* cdi, bool fallback_to_hash);
std::string GetGameCodeForPath(const char* image_path, bool fallback_to_hash);
DiscRegion GetRegionForCode(std::string_view code);
DiscRegion GetRegionFromSystemArea(CDImage* cdi);
DiscRegion GetRegionForImage(CDImage* cdi);
DiscRegion GetRegionForExe(const char* path);
DiscRegion GetRegionForPsf(const char* path);
std::optional<DiscRegion> GetRegionForPath(const char* image_path);

State GetState();
void SetState(State new_state);
bool IsRunning();
bool IsPaused();
bool IsShutdown();
bool IsValid();

bool IsStartupCancelled();
void CancelPendingStartup();

ConsoleRegion GetRegion();
bool IsPALRegion();

ALWAYS_INLINE TickCount GetTicksPerSecond()
{
  return g_ticks_per_second;
}

ALWAYS_INLINE_RELEASE TickCount ScaleTicksToOverclock(TickCount ticks)
{
  if (!g_settings.cpu_overclock_active)
    return ticks;

  return static_cast<TickCount>((static_cast<u64>(static_cast<u32>(ticks)) * g_settings.cpu_overclock_numerator) /
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
void FrameDone();
void IncrementInternalFrameNumber();

const std::string& GetRunningPath();
const std::string& GetRunningCode();
const std::string& GetRunningTitle();

float GetFPS();
float GetVPS();
float GetEmulationSpeed();
float GetAverageFrameTime();
float GetWorstFrameTime();
float GetThrottleFrequency();

bool Boot(const SystemBootParameters& params);
void Reset();
void Shutdown();

bool LoadState(ByteStream* state, bool update_display = true);
bool SaveState(ByteStream* state, u32 screenshot_size = 256);

/// Recreates the GPU component, saving/loading the state so it is preserved. Call when the GPU renderer changes.
bool RecreateGPU(GPURenderer renderer, bool update_display = true);

void SingleStepCPU();
void RunFrame();
void RunFrames();

/// Sets target emulation speed.
float GetTargetSpeed();
void SetTargetSpeed(float speed);

/// Adjusts the throttle frequency, i.e. how many times we should sleep per second.
void SetThrottleFrequency(float frequency);

/// Updates the throttle period, call when target emulation speed changes.
void UpdateThrottlePeriod();
void ResetThrottler();

/// Throttles the system, i.e. sleeps until it's time to execute the next frame.
void Throttle();

void UpdatePerformanceCounters();
void ResetPerformanceCounters();

// Access controllers for simulating input.
Controller* GetController(u32 slot);
void UpdateControllers();
void UpdateControllerSettings();
void ResetControllers();
void UpdateMemoryCardTypes();
void UpdatePerGameMemoryCards();
bool HasMemoryCard(u32 slot);
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

//////////////////////////////////////////////////////////////////////////
// Memory Save States (Rewind and Runahead)
//////////////////////////////////////////////////////////////////////////
void CalculateRewindMemoryUsage(u32 num_saves, u64* ram_usage, u64* vram_usage);
void ClearMemorySaveStates();
void UpdateMemorySaveStateSettings();
bool LoadRewindState(u32 skip_saves = 0, bool consume_state = true);
bool IsRewinding();
void SetRewinding(bool enabled);
void SetRunaheadReplayFlag();

} // namespace System
