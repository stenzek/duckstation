#pragma once
#include "YBaseLib/Assert.h"
#include "bus.h"
#include "cpu_core.h"

namespace CPU {

template<MemoryAccessType type, MemoryAccessSize size, bool is_instruction_fetch, bool raise_exceptions>
bool Core::DoMemoryAccess(VirtualMemoryAddress address, u32& value)
{
  switch (address >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    {
      if constexpr (type == MemoryAccessType::Write)
      {
        if (m_cop0_regs.sr.Isc)
          return true;
      }

      const PhysicalMemoryAddress phys_addr = address & UINT32_C(0x1FFFFFFF);
      if ((phys_addr & DCACHE_LOCATION_MASK) == DCACHE_LOCATION)
      {
        DoScratchpadAccess<type, size>(phys_addr, value);
        return true;
      }

      if (!m_bus->DispatchAccess<type, size>(address, phys_addr, value))
      {
        Panic("Bus error");
        return false;
      }

      return true;
    }

    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
    {
      // Above 512mb raises an exception.
      Panic("Bad user access");
      return false;
    }

    case 0x04: // KSEG0 - physical memory cached
    {
      if constexpr (type == MemoryAccessType::Write)
      {
        if (m_cop0_regs.sr.Isc)
          return true;
      }

      const PhysicalMemoryAddress phys_addr = address & UINT32_C(0x1FFFFFFF);
      if ((phys_addr & DCACHE_LOCATION_MASK) == DCACHE_LOCATION)
      {
        DoScratchpadAccess<type, size>(phys_addr, value);
        return true;
      }

      if (!m_bus->DispatchAccess<type, size>(address, address & UINT32_C(0x1FFFFFFF), value))
      {
        Panic("Bus error");
        return false;
      }

      return true;
    }
    break;

    case 0x05: // KSEG1 - physical memory uncached
    {
      const PhysicalMemoryAddress phys_addr = address & UINT32_C(0x1FFFFFFF);
      if (!m_bus->DispatchAccess<type, size>(address, phys_addr, value))
      {
        Panic("Bus error");
        return false;
      }

      return true;
    }
    break;

    case 0x06: // KSEG2
    case 0x07: // KSEG2
    {
      if (address == 0xFFFE0130)
      {
        if constexpr (type == MemoryAccessType::Read)
          value = m_cache_control;
        else
          WriteCacheControl(value);

        return true;
      }
      else
      {
        Panic("KSEG2 access");
        return false;
      }
    }

    default:
      UnreachableCode();
      return false;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
void CPU::Core::DoScratchpadAccess(PhysicalMemoryAddress address, u32& value)
{
  const PhysicalMemoryAddress cache_offset = address & DCACHE_OFFSET_MASK;
  if constexpr (size == MemoryAccessSize::Byte)
  {
    if constexpr (type == MemoryAccessType::Read)
      value = ZeroExtend32(m_dcache[cache_offset]);
    else
      m_dcache[cache_offset] = Truncate8(value);
  }
  else if constexpr (size == MemoryAccessSize::HalfWord)
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      u16 temp;
      std::memcpy(&temp, &m_dcache[cache_offset], sizeof(temp));
      value = ZeroExtend32(temp);
    }
    else
    {
      u16 temp = Truncate16(value);
      std::memcpy(&m_dcache[cache_offset], &temp, sizeof(temp));
    }
  }
  else if constexpr (size == MemoryAccessSize::Word)
  {
    if constexpr (type == MemoryAccessType::Read)
      std::memcpy(&value, &m_dcache[cache_offset], sizeof(value));
    else
      std::memcpy(&m_dcache[cache_offset], &value, sizeof(value));
  }
}

} // namespace CPU