#pragma once
#include "common/bitfield.h"
#include "cpu_types.h"
#include "gte_types.h"
#include "types.h"
#include <array>
#include <optional>

class StateWrapper;

namespace CPU {

enum : VirtualMemoryAddress
{
  RESET_VECTOR = UINT32_C(0xBFC00000)
};
enum : PhysicalMemoryAddress
{
  DCACHE_LOCATION = UINT32_C(0x1F800000),
  DCACHE_LOCATION_MASK = UINT32_C(0xFFFFFC00),
  DCACHE_OFFSET_MASK = UINT32_C(0x000003FF),
  DCACHE_SIZE = UINT32_C(0x00000400)
};

struct State
{
  // ticks the CPU has executed
  TickCount pending_ticks = 0;
  TickCount downcount = MAX_SLICE_SIZE;

  Registers regs = {};
  Cop0Registers cop0_regs = {};
  Instruction next_instruction = {};

  // address of the instruction currently being executed
  Instruction current_instruction = {};
  u32 current_instruction_pc = 0;
  bool current_instruction_in_branch_delay_slot = false;
  bool current_instruction_was_branch_taken = false;
  bool next_instruction_is_branch_delay_slot = false;
  bool branch_was_taken = false;
  bool exception_raised = false;
  bool interrupt_delay = false;

  // load delays
  Reg load_delay_reg = Reg::count;
  u32 load_delay_value = 0;
  Reg next_load_delay_reg = Reg::count;
  u32 next_load_delay_value = 0;

  u32 cache_control = 0;

  // GTE registers are stored here so we can access them on ARM with a single instruction
  GTE::Regs gte_regs = {};

  // data cache (used as scratchpad)
  std::array<u8, DCACHE_SIZE> dcache = {};
};

extern State g_state;

void Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);

/// Executes interpreter loop.
void Execute();

ALWAYS_INLINE Registers& GetRegs() { return g_state.regs; }

ALWAYS_INLINE TickCount GetPendingTicks() { return g_state.pending_ticks; }
ALWAYS_INLINE void ResetPendingTicks() { g_state.pending_ticks = 0; }
ALWAYS_INLINE void AddPendingTicks(TickCount ticks) { g_state.pending_ticks += ticks; }

ALWAYS_INLINE TickCount GetDowncount() { return g_state.downcount; }
ALWAYS_INLINE void SetDowncount(TickCount downcount) { g_state.downcount = downcount; }

// state helpers
ALWAYS_INLINE bool InUserMode() { return g_state.cop0_regs.sr.KUc; }
ALWAYS_INLINE bool InKernelMode() { return !g_state.cop0_regs.sr.KUc; }

// Memory reads variants which do not raise exceptions.
bool SafeReadMemoryByte(VirtualMemoryAddress addr, u8* value);
bool SafeReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value);
bool SafeReadMemoryWord(VirtualMemoryAddress addr, u32* value);
bool SafeWriteMemoryByte(VirtualMemoryAddress addr, u8 value);
bool SafeWriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value);
bool SafeWriteMemoryWord(VirtualMemoryAddress addr, u32 value);

// External IRQs
void SetExternalInterrupt(u8 bit);
void ClearExternalInterrupt(u8 bit);
bool HasPendingInterrupt();
void DispatchInterrupt();

void DisassembleAndPrint(u32 addr);
void DisassembleAndLog(u32 addr);
void DisassembleAndPrint(u32 addr, u32 instructions_before, u32 instructions_after);

// Write to CPU execution log file.
void WriteToExecutionLog(const char* format, ...);

extern bool TRACE_EXECUTION;
extern bool LOG_EXECUTION;

} // namespace CPU
