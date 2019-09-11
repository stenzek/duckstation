#include "system.h"

System::System() = default;

System::~System() = default;

bool System::Initialize()
{
  if (!m_cpu.Initialize(&m_bus))
    return false;

  if (!m_bus.Initialize(this, &m_dma, &m_gpu))
    return false;

  if (!m_dma.Initialize(&m_bus, &m_gpu))
    return false;

  if (!m_gpu.Initialize(&m_bus, &m_dma))
    return false;

  return true;
}

void System::Reset()
{
  m_cpu.Reset();
  m_bus.Reset();
  m_dma.Reset();
  m_gpu.Reset();
}

void System::RunFrame()
{
  m_cpu.Execute();
}
