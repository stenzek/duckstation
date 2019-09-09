#include "system.h"

System::System() = default;

System::~System() = default;

bool System::Initialize()
{
  if (!m_cpu.Initialize(&m_bus))
    return false;

  if (!m_bus.Initialize(this, &m_dma, nullptr))
    return false;

  if (!m_dma.Initialize(&m_bus, nullptr))
    return false;

  return true;
}

void System::Reset()
{
  m_cpu.Reset();
  m_bus.Reset();
}

void System::RunFrame()
{
  m_cpu.Execute();
}
