// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "bus.h"
#include "cpu_core.h"

struct fastjmp_buf;

namespace CPU {

// Memory address mask used for fetching as well as loadstores (removes cached/uncached/user/kernel bits).
enum : PhysicalMemoryAddress
{
  KSEG_MASK = 0x1FFFFFFF,
  KUSEG_MASK = 0x7FFFFFFF,
};

void SetPC(u32 new_pc);

// exceptions
void RaiseException(Exception excode);
void RaiseException(u32 CAUSE_bits, u32 EPC);
void RaiseBreakException(u32 CAUSE_bits, u32 EPC, u32 instruction_bits);

ALWAYS_INLINE static bool HasPendingInterrupt()
{
  return g_state.cop0_regs.sr.IEc &&
         (((g_state.cop0_regs.cause.bits & g_state.cop0_regs.sr.bits) & (UINT32_C(0xFF) << 8)) != 0);
}

ALWAYS_INLINE static void CheckForPendingInterrupt()
{
  if (HasPendingInterrupt())
    g_state.downcount = 0;
}

void DispatchInterrupt();

// access to execution jump buffer, use with care!
fastjmp_buf* GetExecutionJmpBuf();

// icache stuff
ALWAYS_INLINE static bool IsCachedAddress(VirtualMemoryAddress address)
{
  // KUSEG, KSEG0
  return (address >> 29) <= 4;
}
ALWAYS_INLINE static u32 GetICacheLine(VirtualMemoryAddress address)
{
  return ((address >> 4) & 0xFFu);
}
ALWAYS_INLINE static u32 GetICacheLineOffset(VirtualMemoryAddress address)
{
  return (address & (ICACHE_LINE_SIZE - 1));
}
ALWAYS_INLINE static u32 GetICacheLineWordOffset(VirtualMemoryAddress address)
{
  return (address >> 2) & 0x03u;
}
ALWAYS_INLINE static u32 GetICacheTagForAddress(VirtualMemoryAddress address)
{
  return (address & ICACHE_TAG_ADDRESS_MASK);
}
ALWAYS_INLINE static u32 GetICacheFillTagForAddress(VirtualMemoryAddress address)
{
  static const u32 invalid_bits[4] = {0, 1, 3, 7};
  return GetICacheTagForAddress(address) | invalid_bits[(address >> 2) & 0x03u];
}
ALWAYS_INLINE static u32 GetICacheTagMaskForAddress(VirtualMemoryAddress address)
{
  static const u32 mask[4] = {ICACHE_TAG_ADDRESS_MASK | 1, ICACHE_TAG_ADDRESS_MASK | 2, ICACHE_TAG_ADDRESS_MASK | 4,
                              ICACHE_TAG_ADDRESS_MASK | 8};
  return mask[(address >> 2) & 0x03u];
}

ALWAYS_INLINE static bool CompareICacheTag(VirtualMemoryAddress address)
{
  const u32 line = GetICacheLine(address);
  return ((g_state.icache_tags[line] & GetICacheTagMaskForAddress(address)) == GetICacheTagForAddress(address));
}

TickCount GetInstructionReadTicks(VirtualMemoryAddress address);
TickCount GetICacheFillTicks(VirtualMemoryAddress address);
u32 FillICache(VirtualMemoryAddress address);
void CheckAndUpdateICacheTags(u32 line_count);

ALWAYS_INLINE static Segment GetSegmentForAddress(VirtualMemoryAddress address)
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

ALWAYS_INLINE static constexpr PhysicalMemoryAddress VirtualAddressToPhysical(VirtualMemoryAddress address)
{
  // KUSEG goes to the first 2GB, others are only 512MB.
  return (address & ((address & 0x80000000u) ? KSEG_MASK : KUSEG_MASK));
}

ALWAYS_INLINE static VirtualMemoryAddress PhysicalAddressToVirtual(PhysicalMemoryAddress address, Segment segment)
{
  static constexpr std::array<VirtualMemoryAddress, 4> bases = {{0x00000000, 0x80000000, 0xA0000000, 0xE0000000}};
  return bases[static_cast<u32>(segment)] | address;
}

Bus::MemoryReadHandler GetMemoryReadHandler(VirtualMemoryAddress address, MemoryAccessSize size);
Bus::MemoryWriteHandler GetMemoryWriteHandler(VirtualMemoryAddress address, MemoryAccessSize size);

// memory access functions which return false if an exception was thrown.
bool SafeReadInstruction(VirtualMemoryAddress addr, u32* value);
void* GetDirectReadMemoryPointer(VirtualMemoryAddress address, MemoryAccessSize size, TickCount* read_ticks);
void* GetDirectWriteMemoryPointer(VirtualMemoryAddress address, MemoryAccessSize size);

ALWAYS_INLINE static void AddGTETicks(TickCount ticks)
{
  g_state.gte_completion_tick = g_state.pending_ticks + ticks + 1;
}

ALWAYS_INLINE static void StallUntilGTEComplete()
{
  g_state.pending_ticks =
    (g_state.gte_completion_tick > g_state.pending_ticks) ? g_state.gte_completion_tick : g_state.pending_ticks;
}

ALWAYS_INLINE static void AddMulDivTicks(TickCount ticks)
{
  g_state.muldiv_completion_tick = g_state.pending_ticks + ticks;
}

ALWAYS_INLINE static void StallUntilMulDivComplete()
{
  g_state.pending_ticks =
    (g_state.muldiv_completion_tick > g_state.pending_ticks) ? g_state.muldiv_completion_tick : g_state.pending_ticks;
}

ALWAYS_INLINE static constexpr TickCount GetMultTicks(s32 rs)
{
  // Subtract one because of the instruction cycle.
  if (rs < 0)
    return (rs >= -2048) ? (6 - 1) : ((rs >= -1048576) ? (9 - 1) : (13 - 1));
  else
    return (rs < 0x800) ? (6 - 1) : ((rs < 0x100000) ? (9 - 1) : (13 - 1));
}

ALWAYS_INLINE static constexpr TickCount GetMultTicks(u32 rs)
{
  return (rs < 0x800) ? (6 - 1) : ((rs < 0x100000) ? (9 - 1) : (13 - 1));
}

ALWAYS_INLINE static constexpr TickCount GetDivTicks()
{
  return (36 - 1);
}

// kernel call interception
void HandleA0Syscall();
void HandleB0Syscall();

#ifdef ENABLE_RECOMPILER

namespace RecompilerThunks {

//////////////////////////////////////////////////////////////////////////
// Trampolines for calling back from the JIT
// Needed because we can't cast member functions to void*...
// TODO: Abuse carry flag or something else for exception
//////////////////////////////////////////////////////////////////////////
bool InterpretInstruction();
bool InterpretInstructionPGXP();

// Memory access functions for the JIT - MSB is set on exception.
u64 ReadMemoryByte(u32 address);
u64 ReadMemoryHalfWord(u32 address);
u64 ReadMemoryWord(u32 address);
u32 WriteMemoryByte(u32 address, u32 value);
u32 WriteMemoryHalfWord(u32 address, u32 value);
u32 WriteMemoryWord(u32 address, u32 value);

// Unchecked memory access variants. No alignment or bus exceptions.
u32 UncheckedReadMemoryByte(u32 address);
u32 UncheckedReadMemoryHalfWord(u32 address);
u32 UncheckedReadMemoryWord(u32 address);
void UncheckedWriteMemoryByte(u32 address, u32 value);
void UncheckedWriteMemoryHalfWord(u32 address, u32 value);
void UncheckedWriteMemoryWord(u32 address, u32 value);

} // namespace RecompilerThunks

#endif

} // namespace CPU