#pragma once
#include "settings.h"
#include "types.h"
#include <memory>

class ByteStream;
class StateWrapper;

class HostInterface;

namespace CPU {
class Core;
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

class System
{
public:
  System(HostInterface* host_interface, const Settings& settings);
  ~System();

  HostInterface* GetHostInterface() const { return m_host_interface; }
  CPU::Core* GetCPU() const { return m_cpu.get(); }
  Bus* GetBus() const { return m_bus.get(); }
  GPU* GetGPU() const { return m_gpu.get(); }

  u32 GetFrameNumber() const { return m_frame_number; }
  u32 GetInternalFrameNumber() const { return m_internal_frame_number; }
  u32 GetGlobalTickCounter() const { return m_global_tick_counter; }
  void IncrementFrameNumber() { m_frame_number++; }
  void IncrementInternalFrameNumber() { m_internal_frame_number++; }

  Settings& GetSettings() { return m_settings; }
  void UpdateSettings();

  bool Initialize();
  void Reset();

  bool LoadState(ByteStream* state);
  bool SaveState(ByteStream* state);

  void RunFrame();

  bool LoadEXE(const char* filename);
  bool SetExpansionROM(const char* filename);

  void SetDowncount(TickCount downcount);
  void Synchronize();

  void SetController(u32 slot, std::shared_ptr<PadDevice> dev);
  void SetMemoryCard(u32 slot, std::shared_ptr<PadDevice> dev);

  bool HasMedia() const;
  bool InsertMedia(const char* path);
  void RemoveMedia();

private:
  bool DoState(StateWrapper& sw);

  HostInterface* m_host_interface;
  std::unique_ptr<CPU::Core> m_cpu;
  std::unique_ptr<Bus> m_bus;
  std::unique_ptr<DMA> m_dma;
  std::unique_ptr<InterruptController> m_interrupt_controller;
  std::unique_ptr<GPU> m_gpu;
  std::unique_ptr<CDROM> m_cdrom;
  std::unique_ptr<Pad> m_pad;
  std::unique_ptr<Timers> m_timers;
  std::unique_ptr<SPU> m_spu;
  std::unique_ptr<MDEC> m_mdec;
  u32 m_frame_number = 1;
  u32 m_internal_frame_number = 1;
  u32 m_global_tick_counter = 0;

  Settings m_settings;
};
