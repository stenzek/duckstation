#include "system.h"
#include "bus.h"
#include "cpu_core.h"
#include "dma.h"
#include "gpu.h"

System::System(HostInterface* host_interface) : m_host_interface(host_interface)
{
  m_cpu = std::make_unique<CPU::Core>();
  m_bus = std::make_unique<Bus>();
  m_dma = std::make_unique<DMA>();
  // m_gpu = std::make_unique<GPU>();
  m_gpu = GPU::CreateHardwareOpenGLRenderer();
}

System::~System() = default;

bool System::Initialize()
{
  if (!m_cpu->Initialize(m_bus.get()))
    return false;

  if (!m_bus->Initialize(this, m_dma.get(), m_gpu.get()))
    return false;

  if (!m_dma->Initialize(m_bus.get(), m_gpu.get()))
    return false;

  if (!m_gpu->Initialize(this, m_bus.get(), m_dma.get()))
    return false;

  return true;
}

void System::Reset()
{
  m_cpu->Reset();
  m_bus->Reset();
  m_dma->Reset();
  m_gpu->Reset();
}

void System::RunFrame()
{
  m_cpu->Execute();
}
