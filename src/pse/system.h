#pragma once
#include "bus.h"
#include "dma.h"
#include "gpu.h"
#include "cpu_core.h"
#include "types.h"

class System
{
public:
  System();
  ~System();

  bool Initialize();
  void Reset();

  void RunFrame();

private:
  CPU::Core m_cpu;
  Bus m_bus;
  DMA m_dma;
  GPU m_gpu;
};
