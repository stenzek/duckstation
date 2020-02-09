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
class InterruptController;
class GPU;
class CDROM;
class Pad;
class Controller;
class Timers;
class SPU;
class MDEC;
class SIO;

class System
{
  friend TimingEvent;

public:
  ~System();

  /// Creates a new System.
  static std::unique_ptr<System> Create(HostInterface* host_interface);

  // Accessing components.
  HostInterface* GetHostInterface() const { return m_host_interface; }
  CPU::Core* GetCPU() const { return m_cpu.get(); }
  Bus* GetBus() const { return m_bus.get(); }
  DMA* GetDMA() const { return m_dma.get(); }
  InterruptController* GetInterruptController() const { return m_interrupt_controller.get(); }
  GPU* GetGPU() const { return m_gpu.get(); }
  CDROM* GetCDROM() const { return m_cdrom.get(); }
  Pad* GetPad() const { return m_pad.get(); }
  Timers* GetTimers() const { return m_timers.get(); }
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

  const Settings& GetSettings() { return m_host_interface->GetSettings(); }

  const std::string& GetRunningPath() const { return m_running_game_path; }
  const std::string& GetRunningCode() const { return m_running_game_code; }
  const std::string& GetRunningTitle() const { return m_running_game_title; }

  float GetFPS() const { return m_fps; }
  float GetVPS() const { return m_vps; }
  float GetEmulationSpeed() const { return m_speed; }
  float GetAverageFrameTime() const { return m_average_frame_time; }
  float GetWorstFrameTime() const { return m_worst_frame_time; }

  bool Boot(const char* filename);
  void Reset();

  bool LoadState(ByteStream* state);
  bool SaveState(ByteStream* state);

  /// Recreates the GPU component, saving/loading the state so it is preserved. Call when the GPU renderer changes.
  bool RecreateGPU(GPURenderer renderer);

  /// Updates GPU settings, without recreating the renderer.
  void UpdateGPUSettings();

  /// Forcibly changes the CPU execution mode, ignoring settings.
  void SetCPUExecutionMode(CPUExecutionMode mode);

  void RunFrame();

  /// Adjusts the throttle frequency, i.e. how many times we should sleep per second.
  void SetThrottleFrequency(double frequency) { m_throttle_period = static_cast<s64>(1000000000.0 / frequency); }

  /// Throttles the system, i.e. sleeps until it's time to execute the next frame.
  void Throttle();

  void UpdatePerformanceCounters();
  void ResetPerformanceCounters();

  bool LoadEXE(const char* filename, std::vector<u8>& bios_image);
  bool SetExpansionROM(const char* filename);

  // Adds ticks to the global tick counter, simulating the CPU being stalled.
  void StallCPU(TickCount ticks);

  // Access controllers for simulating input.
  Controller* GetController(u32 slot) const;
  void UpdateControllers();
  void UpdateMemoryCards();

  bool HasMedia() const;
  bool InsertMedia(const char* path);
  void RemoveMedia();

  /// Creates a new event.
  std::unique_ptr<TimingEvent> CreateTimingEvent(std::string name, TickCount period, TickCount interval,
                                                 TimingEventCallback callback, bool activate);

private:
  System(HostInterface* host_interface);

  bool DoState(StateWrapper& sw);
  bool CreateGPU(GPURenderer renderer);

  void InitializeComponents();
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
  std::unique_ptr<InterruptController> m_interrupt_controller;
  std::unique_ptr<GPU> m_gpu;
  std::unique_ptr<CDROM> m_cdrom;
  std::unique_ptr<Pad> m_pad;
  std::unique_ptr<Timers> m_timers;
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

  u64 m_last_throttle_time = 0;
  s64 m_throttle_period = INT64_C(1000000000) / 60;
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
};
