#pragma once
#include "common/bitfield.h"
#include "cpu_types.h"
#include "types.h"
#include <array>

class StateWrapper;

class Bus;

namespace CPU {

class Core
{
public:
  static constexpr VirtualMemoryAddress RESET_VECTOR = UINT32_C(0xBFC00000);
  static constexpr PhysicalMemoryAddress DCACHE_LOCATION = UINT32_C(0x1F800000);
  static constexpr PhysicalMemoryAddress DCACHE_LOCATION_MASK = UINT32_C(0xFFFFFC00);
  static constexpr PhysicalMemoryAddress DCACHE_OFFSET_MASK = UINT32_C(0x000003FF);
  static constexpr PhysicalMemoryAddress DCACHE_SIZE = UINT32_C(0x00000400);

  Core();
  ~Core();

  bool Initialize(Bus* bus);
  void Reset();
  bool DoState(StateWrapper& sw);

  void Execute();

  const Registers& GetRegs() const { return m_regs; }
  Registers& GetRegs() { return m_regs; }

  // Sets the PC and flushes the pipeline.
  void SetPC(u32 new_pc);

  bool SafeReadMemoryByte(VirtualMemoryAddress addr, u8* value);
  bool SafeReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value);
  bool SafeReadMemoryWord(VirtualMemoryAddress addr, u32* value);
  bool SafeWriteMemoryByte(VirtualMemoryAddress addr, u8 value);
  bool SafeWriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value);
  bool SafeWriteMemoryWord(VirtualMemoryAddress addr, u32 value);

private:
  template<MemoryAccessType type, MemoryAccessSize size, bool is_instruction_fetch, bool raise_exceptions>
  bool DoMemoryAccess(VirtualMemoryAddress address, u32& value);

  template<MemoryAccessType type, MemoryAccessSize size>
  bool DoAlignmentCheck(VirtualMemoryAddress address);

  template<MemoryAccessType type, MemoryAccessSize size>
  void DoScratchpadAccess(PhysicalMemoryAddress address, u32& value);

  bool ReadMemoryByte(VirtualMemoryAddress addr, u8* value);
  bool ReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value);
  bool ReadMemoryWord(VirtualMemoryAddress addr, u32* value);
  bool WriteMemoryByte(VirtualMemoryAddress addr, u8 value);
  bool WriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value);
  bool WriteMemoryWord(VirtualMemoryAddress addr, u32 value);

  // state helpers
  bool InUserMode() const { return m_cop0_regs.sr.KUc; }
  bool InKernelMode() const { return !m_cop0_regs.sr.KUc; }

  void DisassembleAndPrint(u32 addr);

  // Fetches the instruction at m_regs.npc
  bool FetchInstruction();
  void ExecuteInstruction(Instruction inst);
  void ExecuteCop0Instruction(Instruction inst);
  void Branch(u32 target);

  // exceptions
  u32 GetExceptionVector(Exception excode) const;
  void RaiseException(Exception excode, u8 coprocessor = 0);

  // flushes any load delays if present
  void FlushLoadDelay();

  // clears pipeline of load/branch delays
  void FlushPipeline();

  // helper functions for registers which aren't writable
  u32 ReadReg(Reg rs);
  void WriteReg(Reg rd, u32 value);

  // helper for generating a load delay write
  void WriteRegDelayed(Reg rd, u32 value);

  // write to cache control register
  void WriteCacheControl(u32 value);

  Bus* m_bus = nullptr;
  Registers m_regs = {};
  Instruction m_next_instruction = {};
  bool m_in_branch_delay_slot = false;
  bool m_branched = false;

  // address of the instruction currently being executed
  u32 m_current_instruction_pc = 0;

  // load delays
  Reg m_load_delay_reg = Reg::count;
  u32 m_load_delay_old_value = 0;
  Reg m_next_load_delay_reg = Reg::count;
  u32 m_next_load_delay_old_value = 0;

  u32 m_cache_control = 0;

  Cop0Registers m_cop0_regs = {};

  // data cache (used as scratchpad)
  std::array<u8, DCACHE_SIZE> m_dcache = {};
};

extern bool TRACE_EXECUTION;

} // namespace CPU

#include "cpu_core.inl"
