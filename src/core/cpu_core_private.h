#pragma once
#include "cpu_core.h"
#include "bus.h"

namespace CPU {

// exceptions
void RaiseException(Exception excode);
void RaiseException(u32 CAUSE_bits, u32 EPC);

ALWAYS_INLINE static bool HasPendingInterrupt()
{
  // const bool do_interrupt = g_state.m_cop0_regs.sr.IEc && ((g_state.m_cop0_regs.cause.Ip & g_state.m_cop0_regs.sr.Im)
  // != 0);
  const bool do_interrupt = g_state.cop0_regs.sr.IEc &&
                            (((g_state.cop0_regs.cause.bits & g_state.cop0_regs.sr.bits) & (UINT32_C(0xFF) << 8)) != 0);

  const bool interrupt_delay = g_state.interrupt_delay;
  g_state.interrupt_delay = false;

  return do_interrupt && !interrupt_delay;
}

ALWAYS_INLINE static void DispatchInterrupt()
{
  // If the instruction we're about to execute is a GTE instruction, delay dispatching the interrupt until the next
  // instruction. For some reason, if we don't do this, we end up with incorrectly sorted polygons and flickering..
  if (g_state.next_instruction.IsCop2Instruction())
    return;

  // Interrupt raising occurs before the start of the instruction.
  RaiseException(
    Cop0Registers::CAUSE::MakeValueForException(Exception::INT, g_state.next_instruction_is_branch_delay_slot,
                                                g_state.branch_was_taken, g_state.next_instruction.cop.cop_n),
    g_state.regs.pc);
}

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
ALWAYS_INLINE bool CompareICacheTag(VirtualMemoryAddress address)
{
  const u32 line = GetICacheLine(address);
  return (g_state.icache_tags[line] == GetICacheTagForAddress(address));
}

TickCount GetInstructionReadTicks(VirtualMemoryAddress address);
TickCount GetICacheFillTicks(VirtualMemoryAddress address);
u32 FillICache(VirtualMemoryAddress address);
void CheckAndUpdateICacheTags(u32 line_count, TickCount uncached_ticks);

// defined in cpu_memory.cpp - memory access functions which return false if an exception was thrown.
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