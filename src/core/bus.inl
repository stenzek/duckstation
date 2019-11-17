#pragma once
#include "bus.h"

template<MemoryAccessType type, MemoryAccessSize size>
TickCount Bus::DoRAMAccess(u32 offset, u32& value)
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

  // Nocash docs say RAM takes 6 cycles to access.
  return RAM_ACCESS_DELAY;
}

template<MemoryAccessType type, MemoryAccessSize size>
TickCount Bus::DoBIOSAccess(u32 offset, u32& value)
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

  return m_bios_access_time[static_cast<u32>(size)];
}

template<MemoryAccessType type, MemoryAccessSize size>
TickCount Bus::DispatchAccess(PhysicalMemoryAddress address, u32& value)
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
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadEXP1(size, address & EXP1_MASK);
    else
      DoWriteEXP1(size, address & EXP1_MASK, value);

    return m_exp1_access_time[static_cast<u32>(size)];
  }
  else if (address < MEMCTRL_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (MEMCTRL_BASE + MEMCTRL_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadMemoryControl(size, address & PAD_MASK);
    else
      DoWriteMemoryControl(size, address & PAD_MASK, value);

    return 1;
  }
  else if (address < (PAD_BASE + PAD_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadPad(size, address & PAD_MASK);
    else
      DoWritePad(size, address & PAD_MASK, value);

    return 1;
  }
  else if (address < (SIO_BASE + SIO_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadSIO(size, address & SIO_MASK);
    else
      DoWriteSIO(size, address & SIO_MASK, value);

    return 1;
  }
  else if (address < (MEMCTRL2_BASE + MEMCTRL2_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadMemoryControl2(size, address & PAD_MASK);
    else
      DoWriteMemoryControl2(size, address & PAD_MASK, value);

    return 1;
  }
  else if (address < (INTERRUPT_CONTROLLER_BASE + INTERRUPT_CONTROLLER_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadInterruptController(size, address & INTERRUPT_CONTROLLER_MASK);
    else
      DoWriteInterruptController(size, address & INTERRUPT_CONTROLLER_MASK, value);

    return 1;
  }
  else if (address < (DMA_BASE + DMA_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadDMA(size, address & DMA_MASK);
    else
      DoWriteDMA(size, address & DMA_MASK, value);

    return 1;
  }
  else if (address < (TIMERS_BASE + TIMERS_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadTimers(size, address & TIMERS_MASK);
    else
      DoWriteTimers(size, address & TIMERS_MASK, value);

    return 1;
  }
  else if (address < CDROM_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (CDROM_BASE + GPU_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadCDROM(size, address & CDROM_MASK);
    else
      DoWriteCDROM(size, address & CDROM_MASK, value);

    return m_cdrom_access_time[static_cast<u32>(size)];
  }
  else if (address < (GPU_BASE + GPU_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadGPU(size, address & GPU_MASK);
    else
      DoWriteGPU(size, address & GPU_MASK, value);

    return 1;
  }
  else if (address < (MDEC_BASE + MDEC_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadMDEC(size, address & MDEC_MASK);
    else
      DoWriteMDEC(size, address & MDEC_MASK, value);

    return 1;
  }
  else if (address < SPU_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (SPU_BASE + SPU_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadSPU(size, address & SPU_MASK);
    else
      DoWriteSPU(size, address & SPU_MASK, value);

    return m_spu_access_time[static_cast<u32>(size)];
  }
  else if (address < EXP2_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (EXP2_BASE + EXP2_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
      value = DoReadEXP2(size, address & EXP2_MASK);
    else
      DoWriteEXP2(size, address & EXP2_MASK, value);

    return m_exp2_access_time[static_cast<u32>(size)];
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
