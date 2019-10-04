#pragma once
#include "bus.h"

template<MemoryAccessType type, MemoryAccessSize size>
bool Bus::DoRAMAccess(u32 offset, u32& value)
{
  // TODO: Configurable mirroring.
  offset &= UINT32_C(0x1FFFFF);
  if constexpr (type == MemoryAccessType::Read)
  {
    if constexpr (size == MemoryAccessSize::Byte)
    {
      value = ZeroExtend32(m_ram[offset]);
    }
    else if constexpr (size == MemoryAccessSize::HalfWord)
    {
      u16 temp;
      std::memcpy(&temp, &m_ram[offset], sizeof(u16));
      value = ZeroExtend32(temp);
    }
    else if constexpr (size == MemoryAccessSize::Word)
    {
      std::memcpy(&value, &m_ram[offset], sizeof(u32));
    }
  }
  else
  {
    if constexpr (size == MemoryAccessSize::Byte)
    {
      m_ram[offset] = Truncate8(value);
    }
    else if constexpr (size == MemoryAccessSize::HalfWord)
    {
      const u16 temp = Truncate16(value);
      std::memcpy(&m_ram[offset], &temp, sizeof(u16));
    }
    else if constexpr (size == MemoryAccessSize::Word)
    {
      std::memcpy(&m_ram[offset], &value, sizeof(u32));
    }
  }

  return true;
}

template<MemoryAccessType type, MemoryAccessSize size>
bool Bus::DoBIOSAccess(u32 offset, u32& value)
{
  // TODO: Configurable mirroring.
  if constexpr (type == MemoryAccessType::Read)
  {
    offset &= UINT32_C(0x7FFFF);
    if constexpr (size == MemoryAccessSize::Byte)
    {
      value = ZeroExtend32(m_bios[offset]);
    }
    else if constexpr (size == MemoryAccessSize::HalfWord)
    {
      u16 temp;
      std::memcpy(&temp, &m_bios[offset], sizeof(u16));
      value = ZeroExtend32(temp);
    }
    else
    {
      std::memcpy(&value, &m_bios[offset], sizeof(u32));
    }
  }
  else
  {
    // Writes are ignored.
  }

  return true;
}

template<MemoryAccessType type, MemoryAccessSize size>
bool Bus::DispatchAccess(PhysicalMemoryAddress address, u32& value)
{
  if (address < 0x800000)
  {
    return DoRAMAccess<type, size>(address, value);
  }
  else if (address < EXP1_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (EXP1_BASE + EXP1_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadEXP1(size, address & EXP1_MASK, value) :
                                              DoWriteEXP1(size, address & EXP1_MASK, value);
  }
  else if (address < MEMCTRL_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (MEMCTRL_BASE + MEMCTRL_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadMemoryControl(size, address & PAD_MASK, value) :
                                              DoWriteMemoryControl(size, address & PAD_MASK, value);
  }
  else if (address < (PAD_BASE + PAD_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadPad(size, address & PAD_MASK, value) :
                                              DoWritePad(size, address & PAD_MASK, value);
  }
  else if (address < (SIO_BASE + SIO_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadSIO(size, address & SIO_MASK, value) :
                                              DoWriteSIO(size, address & SIO_MASK, value);
  }
  else if (address < (MEMCTRL2_BASE + MEMCTRL2_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadMemoryControl2(size, address & PAD_MASK, value) :
                                              DoWriteMemoryControl2(size, address & PAD_MASK, value);
  }
  else if (address < (INTERRUPT_CONTROLLER_BASE + INTERRUPT_CONTROLLER_SIZE))
  {
    return (type == MemoryAccessType::Read) ?
             DoReadInterruptController(size, address & INTERRUPT_CONTROLLER_MASK, value) :
             DoWriteInterruptController(size, address & INTERRUPT_CONTROLLER_MASK, value);
  }
  else if (address < (DMA_BASE + DMA_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadDMA(size, address & DMA_MASK, value) :
                                              DoWriteDMA(size, address & DMA_MASK, value);
  }
  else if (address < (TIMERS_BASE + TIMERS_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadTimers(size, address & TIMERS_MASK, value) :
                                              DoWriteTimers(size, address & TIMERS_MASK, value);
  }
  else if (address < CDROM_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (CDROM_BASE + GPU_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadCDROM(size, address & CDROM_MASK, value) :
                                              DoWriteCDROM(size, address & CDROM_MASK, value);
  }
  else if (address < (GPU_BASE + GPU_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadGPU(size, address & GPU_MASK, value) :
                                              DoWriteGPU(size, address & GPU_MASK, value);
  }
  else if (address < (MDEC_BASE + MDEC_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadMDEC(size, address & MDEC_MASK, value) :
                                              DoWriteMDEC(size, address & MDEC_MASK, value);
  }
  else if (address < SPU_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (SPU_BASE + SPU_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadSPU(size, address & SPU_MASK, value) :
                                              DoWriteSPU(size, address & SPU_MASK, value);
  }
  else if (address < EXP2_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (EXP2_BASE + EXP2_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadEXP2(size, address & EXP2_MASK, value) :
                                              DoWriteEXP2(size, address & EXP2_MASK, value);
  }
  else if (address < BIOS_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (BIOS_BASE + BIOS_SIZE))
  {
    return DoBIOSAccess<type, size>(static_cast<u32>(address - BIOS_BASE), value);
  }
  else
  {
    return DoInvalidAccess(type, size, address, value);
  }
}
