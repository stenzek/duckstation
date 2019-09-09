#include "dma.h"
#include "YBaseLib/Log.h"
#include "bus.h"
Log_SetChannel(DMA);

DMA::DMA() = default;

DMA::~DMA() = default;

bool DMA::Initialize(Bus* bus, GPU* gpu)
{
  m_bus = bus;
  m_gpu = gpu;
  return true;
}

void DMA::Reset()
{
  m_state = {};
  m_DPCR.bits = 0;
  m_DCIR = 0;
}

u32 DMA::ReadRegister(u32 offset)
{
  const u32 channel_index = offset >> 4;
  if (channel_index < 7)
  {
    switch (offset & UINT32_C(0x0F))
    {
      case 0x00:
        return m_state[channel_index].base_address;
      case 0x04:
        return m_state[channel_index].block_control.bits;
      case 0x08:
        return m_state[channel_index].channel_control.bits;
      default:
        break;
    }
  }
  else
  {
    if (offset == 0x70)
      return m_DPCR.bits;
    else if (offset == 0x74)
      return m_DCIR;
  }

  Log_ErrorPrintf("Unhandled register read: %02X", offset);
  return UINT32_C(0xFFFFFFFF);
}

void DMA::WriteRegister(u32 offset, u32 value)
{
  Log_ErrorPrintf("Unhandled register write: %02X <- %08X", offset, value);
}
