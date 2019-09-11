#include "gpu.h"
#include "YBaseLib/Log.h"
#include "bus.h"
Log_SetChannel(GPU);

GPU::GPU() = default;

GPU::~GPU() = default;

bool GPU::Initialize(Bus* bus, DMA* dma)
{
  m_bus = bus;
  m_dma = dma;
  return true;
}

void GPU::Reset()
{
  SoftReset();
}

void GPU::SoftReset()
{
  m_GPUSTAT.bits = 0x14802000;
}

u32 GPU::ReadRegister(u32 offset)
{
  if (offset == 0x00)
  {
    // GPUREAD
    Log_ErrorPrintf("GPUREAD");
    return 0;
  }
  else if (offset == 0x04)
  {
    // GPUSTAT
    return m_GPUSTAT.bits;
  }

  Log_ErrorPrintf("Unhandled register read: %02X", offset);
  return UINT32_C(0xFFFFFFFF);
}

void GPU::WriteRegister(u32 offset, u32 value)
{
  if (offset == 0x00)
    WriteGP0(value);
  else if (offset == 0x04)
    WriteGP1(value);
  else
    Log_ErrorPrintf("Unhandled register write: %02X <- %08X", offset, value);
}

void GPU::WriteGP0(u32 value)
{
  const u8 command = Truncate8(value >> 24);
  Log_ErrorPrintf("Unimplemented GP0 command 0x%02X", command);
}

void GPU::WriteGP1(u32 value)
{
  const u8 command = Truncate8(value >> 24);
  Log_ErrorPrintf("Unimplemented GP1 command 0x%02X", command);
}
