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
  DCACHE_SIZE = UINT32_C(0x00000400),
  ICACHE_SIZE = UINT32_C(0x00001000),
  ICACHE_SLOTS = ICACHE_SIZE / sizeof(u32),
  ICACHE_LINE_SIZE = 16,
  ICACHE_LINES = ICACHE_SIZE / ICACHE_LINE_SIZE,
  ICACHE_SLOTS_PER_LINE = ICACHE_SLOTS / ICACHE_LINES,
  ICACHE_TAG_ADDRESS_MASK = 0xFFFFFFF0u
};

enum : u32
{
  ICACHE_DISABLED_BIT = 0x01,
  ICACHE_INVALD_BIT = 0x02,
};

union CacheControl
{
  u32 bits;

  BitField<u32, bool, 0, 1> lock_mode;
  BitField<u32, bool, 1, 1> invalidate_mode;
  BitField<u32, bool, 2, 1> tag_test_mode;
  BitField<u32, bool, 3, 1> dcache_scratchpad;
  BitField<u32, bool, 7, 1> dcache_enable;
  BitField<u32, u8, 8, 2> icache_fill_size;   // actually dcache? icache always fills to 16 bytes
  BitField<u32, bool, 11, 1> icache_enable;
};

struct State
{
  // ticks the CPU has executed
  TickCount pending_ticks = 0;
  TickCount downcount = 0;

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
  bool frame_done = false;

  // load delays
  Reg load_delay_reg = Reg::count;
  u32 load_delay_value = 0;
  Reg next_load_delay_reg = Reg::count;
  u32 next_load_delay_value = 0;

  CacheControl cache_control{ 0 };

  // GTE registers are stored here so we can access them on ARM with a single instruction
  GTE::Regs gte_regs = {};

  u8* fastmem_base = nullptr;

  // data cache (used as scratchpad)
  std::array<u8, DCACHE_SIZE> dcache = {};
  std::array<u32, ICACHE_LINES> icache_tags = {};
  std::array<u8, ICACHE_SIZE> icache_data = {};
};

extern State g_state;

void Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);
void ClearICache();

/// Executes interpreter loop.
void Execute();

ALWAYS_INLINE Registers& GetRegs() { return g_state.regs; }

ALWAYS_INLINE TickCount GetPendingTicks() { return g_state.pending_ticks; }
ALWAYS_INLINE void ResetPendingTicks() { g_state.pending_ticks = 0; }
ALWAYS_INLINE void AddPendingTicks(TickCount ticks) { g_state.pending_ticks += ticks; }

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

void DisassembleAndPrint(u32 addr);
void DisassembleAndLog(u32 addr);
void DisassembleAndPrint(u32 addr, u32 instructions_before, u32 instructions_after);

// Write to CPU execution log file.
void WriteToExecutionLog(const char* format, ...);

extern bool TRACE_EXECUTION;
extern bool LOG_EXECUTION;

} // namespace CPU
