#pragma once
#include "bus.h"
#include "cpu_core.h"

namespace CPU {

// exceptions
void RaiseException(Exception excode);
void RaiseException(u32 CAUSE_bits, u32 EPC);

ALWAYS_INLINE bool HasPendingInterrupt()
{
  return g_state.cop0_regs.sr.IEc &&
         (((g_state.cop0_regs.cause.bits & g_state.cop0_regs.sr.bits) & (UINT32_C(0xFF) << 8)) != 0);
}

ALWAYS_INLINE void CheckForPendingInterrupt()
{
  if (HasPendingInterrupt())
    g_state.downcount = 0;
}

void DispatchInterrupt();

// icache stuff
ALWAYS_INLINE bool IsCachedAddress(VirtualMemoryAddress address)
{
  // KUSEG, KSEG0
  return (address >> 29) <= 4;
}
ALWAYS_INLINE u32 GetICacheLine(VirtualMemoryAddress address)
{
  return ((address >> 4) & 0xFFu);
}
ALWAYS_INLINE u32 GetICacheLineOffset(VirtualMemoryAddress address)
{
  return (address & (ICACHE_LINE_SIZE - 1));
}
ALWAYS_INLINE u32 GetICacheTagForAddress(VirtualMemoryAddress address)
{
  return (address & ICACHE_TAG_ADDRESS_MASK);
}
ALWAYS_INLINE u32 GetICacheFillTagForAddress(VirtualMemoryAddress address)
{
  static const u32 invalid_bits[4] = {0, 1, 3, 7};
  return GetICacheTagForAddress(address) | invalid_bits[(address >> 2) & 0x03u];
}
ALWAYS_INLINE u32 GetICacheTagMaskForAddress(VirtualMemoryAddress address)
{
  const u32 offset = (address >> 2) & 0x03u;
  static const u32 mask[4] = {ICACHE_TAG_ADDRESS_MASK | 1, ICACHE_TAG_ADDRESS_MASK | 2, ICACHE_TAG_ADDRESS_MASK | 4,
                              ICACHE_TAG_ADDRESS_MASK | 8};
  return mask[(address >> 2) & 0x03u];
}

ALWAYS_INLINE bool CompareICacheTag(VirtualMemoryAddress address)
{
  const u32 line = GetICacheLine(address);
  return ((g_state.icache_tags[line] & GetICacheTagMaskForAddress(address)) == GetICacheTagForAddress(address));
}

TickCount GetInstructionReadTicks(VirtualMemoryAddress address);
TickCount GetICacheFillTicks(VirtualMemoryAddress address);
u32 FillICache(VirtualMemoryAddress address);
void CheckAndUpdateICacheTags(u32 line_count, TickCount uncached_ticks);

ALWAYS_INLINE Segment GetSegmentForAddress(VirtualMemoryAddress address)
{
  switch ((address >> 29))
  {
    case 0x00: // KUSEG 0M-512M
    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
      return Segment::KUSEG;

    case 0x04: // KSEG0 - physical memory cached
      return Segment::KSEG0;

    case 0x05: // KSEG1 - physical memory uncached
      return Segment::KSEG1;

    case 0x06: // KSEG2
    case 0x07: // KSEG2
    default:
      return Segment::KSEG2;
  }
}

ALWAYS_INLINE PhysicalMemoryAddress VirtualAddressToPhysical(VirtualMemoryAddress address)
{
  return (address & PHYSICAL_MEMORY_ADDRESS_MASK);
}

ALWAYS_INLINE VirtualMemoryAddress PhysicalAddressToVirtual(PhysicalMemoryAddress address, Segment segment)
{
  static constexpr std::array<VirtualMemoryAddress, 4> bases = {{0x00000000, 0x80000000, 0xA0000000, 0xE0000000}};
  return bases[static_cast<u32>(segment)] | address;
}

// defined in bus.cpp - memory access functions which return false if an exception was thrown.
bool FetchInstruction();
bool SafeReadInstruction(VirtualMemoryAddress addr, u32* value);
bool ReadMemoryByte(VirtualMemoryAddress addr, u8* value);
bool ReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value);
bool ReadMemoryWord(VirtualMemoryAddress addr, u32* value);
bool WriteMemoryByte(VirtualMemoryAddress addr, u8 value);
bool WriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value);
bool WriteMemoryWord(VirtualMemoryAddress addr, u32 value);
void* GetDirectReadMemoryPointer(VirtualMemoryAddress address, MemoryAccessSize size, TickCount* read_ticks);
void* GetDirectWriteMemoryPointer(VirtualMemoryAddress address, MemoryAccessSize size);

} // namespace CPU