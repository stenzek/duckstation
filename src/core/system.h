#pragma once
#include "common/timer.h"
#include "host_interface.h"
#include "timing_event.h"
#include "types.h"
#include <memory>
#include <optional>
#include <string>

class ByteStream;
class CDImage;
class StateWrapper;

class Controller;

struct SystemBootParameters
{
  SystemBootParameters();
  SystemBootParameters(std::string filename_);
  SystemBootParameters(const SystemBootParameters& copy);
  ~SystemBootParameters();

  std::string filename;
  std::optional<bool> override_fast_boot;
  std::optional<bool> override_fullscreen;
  std::unique_ptr<ByteStream> state_stream;
  u32 media_playlist_index = 0;
  bool load_image_to_ram = false;
  bool force_software_renderer = false;
};

namespace System {

enum : u32
{
  // 5 megabytes is sufficient for now, at the moment they're around 4.2MB.
  MAX_SAVE_STATE_SIZE = 5 * 1024 * 1024
};

enum class State
{
  Shutdown,
  Starting,
  Running,
  Paused
};

/// Returns the preferred console type for a disc.
ConsoleRegion GetConsoleRegionForDiscRegion(DiscRegion region);

State GetState();
void SetState(State new_state);
bool IsRunning();
bool IsPaused();
bool IsShutdown();
bool IsValid();

ConsoleRegion GetRegion();
bool IsPALRegion();
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

bool LoadState(ByteStream* state);
bool SaveState(ByteStream* state, u32 screenshot_size = 128);

/// Recreates the GPU component, saving/loading the state so it is preserved. Call when the GPU renderer changes.
bool RecreateGPU(GPURenderer renderer);

void RunFrame();

/// Adjusts the throttle frequency, i.e. how many times we should sleep per second.
void SetThrottleFrequency(float frequency);

/// Updates the throttle period, call when target emulation speed changes.
void UpdateThrottlePeriod();

/// Throttles the system, i.e. sleeps until it's time to execute the next frame.
void Throttle();

void UpdatePerformanceCounters();
void ResetPerformanceCounters();

// Access controllers for simulating input.
Controller* GetController(u32 slot);
void UpdateControllers();
void UpdateControllerSettings();
void ResetControllers();
void UpdateMemoryCards();

bool HasMedia();
bool InsertMedia(const char* path);
void RemoveMedia();

/// Returns the number of entries in the media/disc playlist.
u32 GetMediaPlaylistCount();

/// Returns the current image from the media/disc playlist.
u32 GetMediaPlaylistIndex();

/// Returns the path to the specified playlist index.
const std::string& GetMediaPlaylistPath(u32 index);

/// Adds a new path to the media playlist.
bool AddMediaPathToPlaylist(const std::string_view& path);

/// Removes a path from the media playlist.
bool RemoveMediaPathFromPlaylist(const std::string_view& path);
bool RemoveMediaPathFromPlaylist(u32 index);

/// Changes a path from the media playlist.
bool ReplaceMediaPathFromPlaylist(u32 index, const std::string_view& path);

/// Switches to the specified media/disc playlist index.
bool SwitchMediaFromPlaylist(u32 index);

} // namespace System
