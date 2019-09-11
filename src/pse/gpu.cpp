#include "gpu.h"
#include "YBaseLib/Log.h"
#include "bus.h"
#include "dma.h"
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
  UpdateDMARequest();
}

void GPU::UpdateDMARequest()
{
  const bool request = m_GPUSTAT.dma_direction != DMADirection::Off;
  m_GPUSTAT.dma_data_request = request;
  m_dma->SetRequest(DMA::Channel::GPU, request);
}

u32 GPU::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00:
      return ReadGPUREAD();

    case 0x04:
      return m_GPUSTAT.bits;

    default:
      Log_ErrorPrintf("Unhandled register read: %02X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void GPU::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00:
      WriteGP0(value);
      return;

    case 0x04:
      WriteGP1(value);
      return;

    default:
      Log_ErrorPrintf("Unhandled register write: %02X <- %08X", offset, value);
      return;
  }
}

u32 GPU::DMARead()
{
  if (m_GPUSTAT.dma_direction != DMADirection::GPUREADtoCPU)
  {
    Log_ErrorPrintf("Invalid DMA direction from GPU DMA read");
    return UINT32_C(0xFFFFFFFF);
  }

  return ReadGPUREAD();
}

void GPU::DMAWrite(u32 value)
{
  Log_ErrorPrintf("GPU DMA Write %08X", value);
}

u32 GPU::ReadGPUREAD()
{
  Log_ErrorPrintf("GPUREAD not implemented");
  return UINT32_C(0xFFFFFFFF);
}

void GPU::WriteGP0(u32 value)
{
  const u8 command = Truncate8(value >> 24);
  Log_ErrorPrintf("Unimplemented GP0 command 0x%02X", command);
}

void GPU::WriteGP1(u32 value)
{
  const u8 command = Truncate8(value >> 24);
  const u32 param = value & UINT32_C(0x00FFFFFF);
  switch (command)
  {
    case 0x04: // DMA Direction
    {
      m_GPUSTAT.dma_direction = static_cast<DMADirection>(param);
      Log_DebugPrintf("DMA direction <- 0x%02X", static_cast<u32>(m_GPUSTAT.dma_direction.GetValue()));
      UpdateDMARequest();
    }
    break;

    default:
      Log_ErrorPrintf("Unimplemented GP1 command 0x%02X", command);
      break;
  }
}
