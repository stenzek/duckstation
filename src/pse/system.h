#pragma once
#include "types.h"

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
  System();
  ~System();

  bool Initialize();
  void Reset();

  void RunFrame();

private:
  std::unique_ptr<CPU::Core> m_cpu;
  std::unique_ptr<Bus> m_bus;
  std::unique_ptr<DMA> m_dma;
  std::unique_ptr<GPU> m_gpu;
};
