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

namespace CPU {
class Core;
class CodeCache;
} // namespace CPU

class Bus;
class DMA;
class GPU;
class CDROM;
class Pad;
class Controller;
class Timers;
class SPU;
class MDEC;
class SIO;

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

class System
{
public:
  enum : u32
  {
    // 5 megabytes is sufficient for now, at the moment they're around 4.2MB.
    MAX_SAVE_STATE_SIZE = 5 * 1024 * 1024
  };

  friend TimingEvent;

  ~System();

  /// Returns the preferred console type for a disc.
  static ConsoleRegion GetConsoleRegionForDiscRegion(DiscRegion region);

  /// Creates a new System.
  static std::unique_ptr<System> Create(HostInterface* host_interface);

  // Accessing components.
  HostInterface* GetHostInterface() const { return m_host_interface; }
  CPU::Core* GetCPU() const { return m_cpu.get(); }
  Bus* GetBus() const { return m_bus.get(); }
  DMA* GetDMA() const { return m_dma.get(); }
  CDROM* GetCDROM() const { return m_cdrom.get(); }
  Pad* GetPad() const { return m_pad.get(); }
  SPU* GetSPU() const { return m_spu.get(); }
  MDEC* GetMDEC() const { return m_mdec.get(); }

  ConsoleRegion GetRegion() const { return m_region; }
  bool IsPALRegion() const { return m_region == ConsoleRegion::PAL; }
  u32 GetFrameNumber() const { return m_frame_number; }
  u32 GetInternalFrameNumber() const { return m_internal_frame_number; }
  u32 GetGlobalTickCounter() const { return m_global_tick_counter; }
  void IncrementFrameNumber()
  {
    m_frame_number++;
    m_frame_done = true;
  }
  void IncrementInternalFrameNumber() { m_internal_frame_number++; }

  const std::string& GetRunningPath() const { return m_running_game_path; }
  const std::string& GetRunningCode() const { return m_running_game_code; }
  const std::string& GetRunningTitle() const { return m_running_game_title; }

  float GetFPS() const { return m_fps; }
  float GetVPS() const { return m_vps; }
  float GetEmulationSpeed() const { return m_speed; }
  float GetAverageFrameTime() const { return m_average_frame_time; }
  float GetWorstFrameTime() const { return m_worst_frame_time; }
  float GetThrottleFrequency() const { return m_throttle_frequency; }

  bool Boot(const SystemBootParameters& params);
  void Reset();

  bool LoadState(ByteStream* state);
  bool SaveState(ByteStream* state, u32 screenshot_size = 128);

  /// Recreates the GPU component, saving/loading the state so it is preserved. Call when the GPU renderer changes.
  bool RecreateGPU(GPURenderer renderer);

  /// Updates GPU settings, without recreating the renderer.
  void UpdateGPUSettings();

  /// Forcibly changes the CPU execution mode, ignoring settings.
  void SetCPUExecutionMode(CPUExecutionMode mode);

  void RunFrame();

  /// Adjusts the throttle frequency, i.e. how many times we should sleep per second.
  void SetThrottleFrequency(float frequency);

  /// Updates the throttle period, call when target emulation speed changes.
  void UpdateThrottlePeriod();

  /// Throttles the system, i.e. sleeps until it's time to execute the next frame.
  void Throttle();

  void UpdatePerformanceCounters();
  void ResetPerformanceCounters();

  bool LoadEXE(const char* filename, std::vector<u8>& bios_image);
  bool LoadEXEFromBuffer(const void* buffer, u32 buffer_size, std::vector<u8>& bios_image);
  bool LoadPSF(const char* filename, std::vector<u8>& bios_image);
  bool SetExpansionROM(const char* filename);

  // Adds ticks to the global tick counter, simulating the CPU being stalled.
  void StallCPU(TickCount ticks);

  // Access controllers for simulating input.
  Controller* GetController(u32 slot) const;
  void UpdateControllers();
  void UpdateControllerSettings();
  void ResetControllers();
  void UpdateMemoryCards();

  bool HasMedia() const;
  bool InsertMedia(const char* path);
  void RemoveMedia();

  /// Creates a new event.
  std::unique_ptr<TimingEvent> CreateTimingEvent(std::string name, TickCount period, TickCount interval,
                                                 TimingEventCallback callback, bool activate);

  /// Returns the number of entries in the media/disc playlist.
  ALWAYS_INLINE u32 GetMediaPlaylistCount() const { return static_cast<u32>(m_media_playlist.size()); }

  /// Returns the current image from the media/disc playlist.
  u32 GetMediaPlaylistIndex() const;

  /// Returns the path to the specified playlist index.
  const std::string& GetMediaPlaylistPath(u32 index) const { return m_media_playlist[index]; }

  /// Adds a new path to the media playlist.
  bool AddMediaPathToPlaylist(const std::string_view& path);

  /// Removes a path from the media playlist.
  bool RemoveMediaPathFromPlaylist(const std::string_view& path);
  bool RemoveMediaPathFromPlaylist(u32 index);

  /// Changes a path from the media playlist.
  bool ReplaceMediaPathFromPlaylist(u32 index, const std::string_view& path);

  /// Switches to the specified media/disc playlist index.
  bool SwitchMediaFromPlaylist(u32 index);

private:
  System(HostInterface* host_interface);

  /// Opens CD image, preloading if needed.
  std::unique_ptr<CDImage> OpenCDImage(const char* path, bool force_preload);

  bool DoLoadState(ByteStream* stream, bool init_components, bool force_software_renderer);
  bool DoState(StateWrapper& sw);
  bool CreateGPU(GPURenderer renderer);

  bool InitializeComponents(bool force_software_renderer);
  void DestroyComponents();

  // Active event management
  void AddActiveEvent(TimingEvent* event);
  void RemoveActiveEvent(TimingEvent* event);
  void SortEvents();

  // Runs any pending events. Call when CPU downcount is zero.
  void RunEvents();

  // Updates the downcount of the CPU (event scheduling).
  void UpdateCPUDowncount();

  bool DoEventsState(StateWrapper& sw);

  // Event lookup, use with care.
  // If you modify an event, call SortEvents afterwards.
  TimingEvent* FindActiveEvent(const char* name);

  // Event enumeration, use with care.
  // Don't remove an event while enumerating the list, as it will invalidate the iterator.
  template<typename T>
  void EnumerateActiveEvents(T callback) const
  {
    for (const TimingEvent* ev : m_events)
      callback(ev);
  }

  void UpdateRunningGame(const char* path, CDImage* image);

  HostInterface* m_host_interface;
  std::unique_ptr<CPU::Core> m_cpu;
  std::unique_ptr<CPU::CodeCache> m_cpu_code_cache;
  std::unique_ptr<Bus> m_bus;
  std::unique_ptr<DMA> m_dma;
  std::unique_ptr<CDROM> m_cdrom;
  std::unique_ptr<Pad> m_pad;
  std::unique_ptr<SPU> m_spu;
  std::unique_ptr<MDEC> m_mdec;
  std::unique_ptr<SIO> m_sio;
  ConsoleRegion m_region = ConsoleRegion::NTSC_U;
  CPUExecutionMode m_cpu_execution_mode = CPUExecutionMode::Interpreter;
  u32 m_frame_number = 1;
  u32 m_internal_frame_number = 1;
  u32 m_global_tick_counter = 0;

  std::vector<TimingEvent*> m_events;
  u32 m_last_event_run_time = 0;
  bool m_running_events = false;
  bool m_events_need_sorting = false;
  bool m_frame_done = false;

  std::string m_running_game_path;
  std::string m_running_game_code;
  std::string m_running_game_title;

  float m_throttle_frequency = 60.0f;
  s32 m_throttle_period = 0;
  u64 m_last_throttle_time = 0;
  Common::Timer m_throttle_timer;
  Common::Timer m_speed_lost_time_timestamp;

  float m_average_frame_time_accumulator = 0.0f;
  float m_worst_frame_time_accumulator = 0.0f;

  float m_vps = 0.0f;
  float m_fps = 0.0f;
  float m_speed = 0.0f;
  float m_worst_frame_time = 0.0f;
  float m_average_frame_time = 0.0f;
  u32 m_last_frame_number = 0;
  u32 m_last_internal_frame_number = 0;
  u32 m_last_global_tick_counter = 0;
  Common::Timer m_fps_timer;
  Common::Timer m_frame_timer;

  // Playlist of disc images.
  std::vector<std::string> m_media_playlist;
};

extern std::unique_ptr<System> g_system;
