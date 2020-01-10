#pragma once
#include "common/align.h"
#include "bus.h"
#include "cpu_core.h"

namespace CPU {

template<MemoryAccessType type, MemoryAccessSize size>
TickCount Core::DoMemoryAccess(VirtualMemoryAddress address, u32& value)
{
  switch (address >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    {
      if constexpr (type == MemoryAccessType::Write)
      {
        if (m_cop0_regs.sr.Isc)
          return 0;
      }

      const PhysicalMemoryAddress phys_addr = address & UINT32_C(0x1FFFFFFF);
      if ((phys_addr & DCACHE_LOCATION_MASK) == DCACHE_LOCATION)
      {
        DoScratchpadAccess<type, size>(phys_addr, value);
        return 0;
      }

      return m_bus->DispatchAccess<type, size>(phys_addr, value);
    }

    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
    {
      // Above 512mb raises an exception.
      return -1;
    }

    case 0x04: // KSEG0 - physical memory cached
    {
      if constexpr (type == MemoryAccessType::Write)
      {
        if (m_cop0_regs.sr.Isc)
          return 0;
      }

      const PhysicalMemoryAddress phys_addr = address & UINT32_C(0x1FFFFFFF);
      if ((phys_addr & DCACHE_LOCATION_MASK) == DCACHE_LOCATION)
      {
        DoScratchpadAccess<type, size>(phys_addr, value);
        return 0;
      }

      return m_bus->DispatchAccess<type, size>(phys_addr, value);
    }
    break;

    case 0x05: // KSEG1 - physical memory uncached
    {
      const PhysicalMemoryAddress phys_addr = address & UINT32_C(0x1FFFFFFF);
      return m_bus->DispatchAccess<type, size>(phys_addr, value);
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

        return 0;
      }
      else
      {
        return -1;
      }
    }

    default:
      UnreachableCode();
      return false;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
bool CPU::Core::DoAlignmentCheck(VirtualMemoryAddress address)
{
  if constexpr (size == MemoryAccessSize::HalfWord)
  {
    if (Common::IsAlignedPow2(address, 2))
      return true;
  }
  else if constexpr (size == MemoryAccessSize::Word)
  {
    if (Common::IsAlignedPow2(address, 4))
      return true;
  }
  else
  {
    return true;
  }

  m_cop0_regs.BadVaddr = address;
  RaiseException(type == MemoryAccessType::Read ? Exception::AdEL : Exception::AdES);
  return false;
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