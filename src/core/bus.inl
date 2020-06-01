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
    const u32 page_index = offset / CPU_CODE_CACHE_PAGE_SIZE;
    if (m_ram_code_bits[page_index])
      DoInvalidateCodeCache(page_index);

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

  return (type == MemoryAccessType::Read) ? 4 : 0;
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
    {
      value = DoReadEXP1(size, address & EXP1_MASK);
      return m_exp1_access_time[static_cast<u32>(size)];
    }
    else
    {
      DoWriteEXP1(size, address & EXP1_MASK, value);
      return 0;
    }
  }
  else if (address < MEMCTRL_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (MEMCTRL_BASE + MEMCTRL_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      value = DoReadMemoryControl(size, address & PAD_MASK);
      return 2;
    }
    else
    {
      DoWriteMemoryControl(size, address & PAD_MASK, value);
      return 0;
    }
  }
  else if (address < (PAD_BASE + PAD_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      value = DoReadPad(size, address & PAD_MASK);
      return 2;
    }
    else
    {
      DoWritePad(size, address & PAD_MASK, value);
      return 0;
    }
  }
  else if (address < (SIO_BASE + SIO_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      value = DoReadSIO(size, address & SIO_MASK);
      return 2;
    }
    else
    {
      DoWriteSIO(size, address & SIO_MASK, value);
      return 0;
    }
  }
  else if (address < (MEMCTRL2_BASE + MEMCTRL2_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      value = DoReadMemoryControl2(size, address & PAD_MASK);
      return 2;
    }
    else
    {
      DoWriteMemoryControl2(size, address & PAD_MASK, value);
      return 0;
    }
  }
  else if (address < (INTERRUPT_CONTROLLER_BASE + INTERRUPT_CONTROLLER_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      value = DoReadInterruptController(size, address & INTERRUPT_CONTROLLER_MASK);
      return 2;
    }
    else
    {
      DoWriteInterruptController(size, address & INTERRUPT_CONTROLLER_MASK, value);
      return 0;
    }
  }
  else if (address < (DMA_BASE + DMA_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      value = DoReadDMA(size, address & DMA_MASK);
      return 2;
    }
    else
    {
      DoWriteDMA(size, address & DMA_MASK, value);
      return 0;
    }
  }
  else if (address < (TIMERS_BASE + TIMERS_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      value = DoReadTimers(size, address & TIMERS_MASK);
      return 2;
    }
    else
    {
      DoWriteTimers(size, address & TIMERS_MASK, value);
      return 0;
    }
  }
  else if (address < CDROM_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (CDROM_BASE + GPU_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      value = DoReadCDROM(size, address & CDROM_MASK);
      return m_cdrom_access_time[static_cast<u32>(size)];
    }
    else
    {
      DoWriteCDROM(size, address & CDROM_MASK, value);
      return 0;
    }
  }
  else if (address < (GPU_BASE + GPU_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      value = DoReadGPU(size, address & GPU_MASK);
      return 2;
    }
    else
    {
      DoWriteGPU(size, address & GPU_MASK, value);
      return 0;
    }
  }
  else if (address < (MDEC_BASE + MDEC_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      value = DoReadMDEC(size, address & MDEC_MASK);
      return 2;
    }
    else
    {
      DoWriteMDEC(size, address & MDEC_MASK, value);
      return 0;
    }
  }
  else if (address < SPU_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (SPU_BASE + SPU_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      value = DoReadSPU(size, address & SPU_MASK);
      return m_spu_access_time[static_cast<u32>(size)];
    }
    else
    {
      DoWriteSPU(size, address & SPU_MASK, value);
      return 0;
    }
  }
  else if (address < EXP2_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (EXP2_BASE + EXP2_SIZE))
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      value = DoReadEXP2(size, address & EXP2_MASK);
      return m_exp2_access_time[static_cast<u32>(size)];
    }
    else
    {
      DoWriteEXP2(size, address & EXP2_MASK, value);
      return 0;
    }
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
