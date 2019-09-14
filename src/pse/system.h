#pragma once
#include "types.h"

class HostInterface;

namespace CPU
{
class Core;
}

class Bus;
class DMA;
class GPU;

class System
{
public:
  System(HostInterface* host_interface);
  ~System();

  HostInterface* GetHostInterface() const { return m_host_interface; }

  u32 GetFrameNumber() const { return m_frame_number; }
  void IncrementFrameNumber() { m_frame_number++; }

  bool Initialize();
  void Reset();

  void RunFrame();

  bool LoadEXE(const char* filename);

private:
  HostInterface* m_host_interface;
  std::unique_ptr<CPU::Core> m_cpu;
  std::unique_ptr<Bus> m_bus;
  std::unique_ptr<DMA> m_dma;
  std::unique_ptr<GPU> m_gpu;
  u32 m_frame_number = 1;
};
