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

      if (!m_bus->DispatchAccess<type, size>(address, address & UINT32_C(0x1FFFFFFF), value))
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
      if (!m_bus->DispatchAccess<type, size>(address, address & UINT32_C(0x1FFFFFFF), value))
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

} // namespace CPU