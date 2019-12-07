#pragma once
#include "types.h"
#include "host_interface.h"
#include <memory>
#include <optional>

class ByteStream;
class CDImage;
class StateWrapper;

namespace CPU {
class Core;
class CodeCache;
}

class Bus;
class DMA;
class InterruptController;
class GPU;
class CDROM;
class Pad;
class PadDevice;
class Timers;
class SPU;
class MDEC;
class SIO;

class System
{
public:
  ~System();

  /// Returns true if the filename is a PlayStation executable we can inject.
  static bool IsPSExe(const char* filename);

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
  void IncrementFrameNumber() { m_frame_number++; }
  void IncrementInternalFrameNumber() { m_internal_frame_number++; }

  const Settings& GetSettings() { return m_host_interface->GetSettings(); }

  bool Boot(const char* filename);
  void Reset();

  bool LoadState(ByteStream* state);
  bool SaveState(ByteStream* state);

  /// Recreates the GPU component, saving/loading the state so it is preserved. Call when the GPU renderer changes.
  bool RecreateGPU();

  void RunFrame();

  bool LoadEXE(const char* filename, std::vector<u8>& bios_image);
  bool SetExpansionROM(const char* filename);

  void SetDowncount(TickCount downcount);
  void Synchronize();

  // Adds ticks to the global tick counter, simulating the CPU being stalled.
  void StallCPU(TickCount ticks);

  void SetController(u32 slot, std::shared_ptr<PadDevice> dev);
  void UpdateMemoryCards();
  void UpdateCPUExecutionMode();

  bool HasMedia() const;
  bool InsertMedia(const char* path);
  void RemoveMedia();

private:
  System(HostInterface* host_interface);

  bool DoState(StateWrapper& sw);
  bool CreateGPU();

  void InitializeComponents();

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
};
