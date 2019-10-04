#include "spu.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
#include "dma.h"
#include "interrupt_controller.h"
#include "system.h"
Log_SetChannel(SPU);

SPU::SPU() = default;

SPU::~SPU() = default;

bool SPU::Initialize(System* system, DMA* dma, InterruptController* interrupt_controller)
{
  m_system = system;
  m_dma = dma;
  m_interrupt_controller = interrupt_controller;
  return true;
}

void SPU::Reset()
{
  m_SPUCNT.bits = 0;
  m_SPUSTAT.bits = 0;
  m_transfer_address = 0;
  m_transfer_address_reg = 0;
}

bool SPU::DoState(StateWrapper& sw)
{
  sw.Do(&m_SPUCNT.bits);
  sw.Do(&m_SPUSTAT.bits);
  sw.Do(&m_transfer_address);
  sw.Do(&m_transfer_address_reg);
  return !sw.HasError();
}

u16 SPU::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x1F801DA6 - SPU_BASE:
      Log_DebugPrintf("SPU transfer address register -> 0x%04X", ZeroExtend32(m_transfer_address_reg));
      return m_transfer_address_reg;

    case 0x1F801DA8 - SPU_BASE:
      Log_ErrorPrintf("SPU transfer data register read");
      return UINT16_C(0xFFFF);

    case 0x1F801DAA - SPU_BASE:
      Log_DebugPrintf("SPU control register -> 0x%04X", ZeroExtend32(m_SPUCNT.bits));
      return m_SPUCNT.bits;

    case 0x1F801DAE - SPU_BASE:
      Log_DebugPrintf("SPU status register -> 0x%04X", ZeroExtend32(m_SPUCNT.bits));
      return m_SPUSTAT.bits;

    default:
      Log_ErrorPrintf("Unknown SPU register read: offset 0x%X (address 0x%08X)", offset, offset | SPU_BASE);
      return UINT16_C(0xFFFF);
  }
}

void SPU::WriteRegister(u32 offset, u16 value)
{
  switch (offset)
  {
    case 0x1F801DA6 - SPU_BASE:
    {
      Log_DebugPrintf("SPU transfer address register <- 0x%04X", ZeroExtend32(value));
      m_transfer_address_reg = value;
      m_transfer_address = ZeroExtend32(value) * 8;
      return;
    }

    case 0x1F801DA8 - SPU_BASE:
    {
      Log_TracePrintf("SPU transfer data register <- 0x%04X (RAM offset 0x%08X)", ZeroExtend32(value),
                      m_transfer_address);
      RAMTransferWrite(value);
      return;
    }

    case 0x1F801DAA - SPU_BASE:
    {
      Log_DebugPrintf("SPU control register <- 0x%04X", ZeroExtend32(value));
      m_SPUCNT.bits = value;
      UpdateDMARequest();
      return;
    }

      // read-only registers
    case 0x1F801DAE - SPU_BASE:
    {
      return;
    }

    default:
    {
      Log_ErrorPrintf("Unknown SPU register write: offset 0x%X (address 0x%08X) value 0x%04X", offset,
                      offset | SPU_BASE, ZeroExtend32(value));
      return;
    }
  }
}

u32 SPU::DMARead()
{
  const u16 lsb = RAMTransferRead();
  const u16 msb = RAMTransferRead();
  return ZeroExtend32(lsb) | (ZeroExtend32(msb) << 16);
}

void SPU::DMAWrite(u32 value)
{
  // two 16-bit writes to prevent out-of-bounds
  RAMTransferWrite(Truncate16(value));
  RAMTransferWrite(Truncate16(value >> 16));
}

void SPU::UpdateDMARequest()
{
  const RAMTransferMode mode = m_SPUCNT.ram_transfer_mode;
  const bool request = (mode == RAMTransferMode::DMAWrite || mode == RAMTransferMode::DMARead);
  m_dma->SetRequest(DMA::Channel::SPU, request);
}

u16 SPU::RAMTransferRead()
{
  u16 value;
  std::memcpy(&value, &m_ram[m_transfer_address], sizeof(value));
  m_transfer_address = (m_transfer_address + sizeof(value)) & RAM_MASK;
  return value;
}

void SPU::RAMTransferWrite(u16 value)
{
  std::memcpy(&m_ram[m_transfer_address], &value, sizeof(value));
  m_transfer_address = (m_transfer_address + sizeof(value)) & RAM_MASK;
}
