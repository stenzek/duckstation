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
bool Bus::DispatchAccess(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u32& value)
{
  if (bus_address < 0x800000)
  {
    return DoRAMAccess<type, size>(bus_address, value);
  }
  else if (bus_address < INTERRUPT_CONTROLLER_BASE)
  {
    return DoInvalidAccess(type, size, cpu_address, bus_address, value);
  }
  else if (bus_address < (INTERRUPT_CONTROLLER_BASE + INTERRUPT_CONTROLLER_SIZE))
  {
    return (type == MemoryAccessType::Read) ?
             DoReadInterruptController(size, bus_address & INTERRUPT_CONTROLLER_MASK, value) :
             DoWriteInterruptController(size, bus_address & INTERRUPT_CONTROLLER_MASK, value);
  }
  else if (bus_address < DMA_BASE)
  {
    return DoInvalidAccess(type, size, cpu_address, bus_address, value);
  }
  else if (bus_address < (DMA_BASE + DMA_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadDMA(size, bus_address & DMA_MASK, value) :
                                              DoWriteDMA(size, bus_address & DMA_MASK, value);
  }
  else if (bus_address < CDROM_BASE)
  {
    return DoInvalidAccess(type, size, cpu_address, bus_address, value);
  }
  else if (bus_address < (CDROM_BASE + GPU_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadCDROM(size, bus_address & CDROM_MASK, value) :
                                              DoWriteCDROM(size, bus_address & CDROM_MASK, value);
  }
  else if (bus_address < GPU_BASE)
  {
    return DoInvalidAccess(type, size, cpu_address, bus_address, value);
  }
  else if (bus_address < (GPU_BASE + GPU_SIZE))
  {
    return (type == MemoryAccessType::Read) ? DoReadGPU(size, bus_address & GPU_MASK, value) :
                                              DoWriteGPU(size, bus_address & GPU_MASK, value);
  }
  else if (bus_address < SPU_BASE)
  {
    return DoInvalidAccess(type, size, cpu_address, bus_address, value);
  }
  else if (bus_address < (SPU_BASE + SPU_SIZE))
  {
    return (type == MemoryAccessType::Read) ? ReadSPU(size, bus_address & SPU_MASK, value) :
                                              WriteSPU(size, bus_address & SPU_MASK, value);
  }
  else if (bus_address < EXP2_BASE)
  {
    return DoInvalidAccess(type, size, cpu_address, bus_address, value);
  }
  else if (bus_address < (EXP2_BASE + EXP2_SIZE))
  {
    return (type == MemoryAccessType::Read) ? ReadExpansionRegion2(size, bus_address & EXP2_MASK, value) :
                                              WriteExpansionRegion2(size, bus_address & EXP2_MASK, value);
  }
  else if (bus_address < BIOS_BASE)
  {
    return DoInvalidAccess(type, size, cpu_address, bus_address, value);
  }
  else if (bus_address < (BIOS_BASE + BIOS_SIZE))
  {
    return DoBIOSAccess<type, size>(static_cast<u32>(bus_address - BIOS_BASE), value);
  }
  else if (bus_address < 0x1FFE0000)
  {
    return DoInvalidAccess(type, size, cpu_address, bus_address, value);
  }
  else if (bus_address < 0x1FFE0200) // I/O Ports (Cache Control)
  {
    return DoInvalidAccess(type, size, cpu_address, bus_address, value);
  }
  else
  {
    return DoInvalidAccess(type, size, cpu_address, bus_address, value);
  }
}
