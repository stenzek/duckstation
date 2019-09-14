#pragma once
#include "common/bitfield.h"
#include "cpu_types.h"
#include "types.h"

class StateWrapper;

class Bus;

namespace CPU {

class Core
{
public:
  static constexpr VirtualMemoryAddress RESET_VECTOR = 0xbfc00000;

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

  u8 ReadMemoryByte(VirtualMemoryAddress addr);
  u16 ReadMemoryHalfWord(VirtualMemoryAddress addr);
  u32 ReadMemoryWord(VirtualMemoryAddress addr);
  void WriteMemoryByte(VirtualMemoryAddress addr, u8 value);
  void WriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value);
  void WriteMemoryWord(VirtualMemoryAddress addr, u32 value);

  // state helpers
  bool InUserMode() const { return m_cop0_regs.sr.KUc; }
  bool InKernelMode() const { return !m_cop0_regs.sr.KUc; }

  void DisassembleAndPrint(u32 addr);

  // Fetches the instruction at m_regs.npc
  void FetchInstruction();
  void ExecuteInstruction(Instruction inst, u32 inst_pc);
  void ExecuteCop0Instruction(Instruction inst, u32 inst_pc);
  void Branch(u32 target);
  void RaiseException(u32 inst_pc, Exception excode);

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

  // load delays
  Reg m_load_delay_reg = Reg::count;
  u32 m_load_delay_old_value = 0;
  Reg m_next_load_delay_reg = Reg::count;
  u32 m_next_load_delay_old_value = 0;

  u32 m_cache_control = 0;

  Cop0Registers m_cop0_regs = {};
};

extern bool TRACE_EXECUTION;

} // namespace CPU

#include "cpu_core.inl"
