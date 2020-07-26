#pragma once
#include "cpu_code_cache.h"
#include "cpu_types.h"

namespace CPU {
struct CodeBlockInstruction;

namespace Recompiler::Thunks {

union RaiseExceptionInfo
{
  u32 bits;

  struct
  {
    u8 excode;
    bool BD;
    u8 CE;
    u8 unused;
  };
};

ALWAYS_INLINE u32 MakeRaiseExceptionInfo(Exception excode, const CodeBlockInstruction& cbi)
{
  RaiseExceptionInfo ri = {};
  ri.excode = static_cast<u8>(excode);
  ri.BD = cbi.is_branch_delay_slot;
  ri.CE = cbi.instruction.cop.cop_n;
  return ri.bits;
}

//////////////////////////////////////////////////////////////////////////
// Trampolines for calling back from the JIT
// Needed because we can't cast member functions to void*...
// TODO: Abuse carry flag or something else for exception
//////////////////////////////////////////////////////////////////////////
bool InterpretInstruction();
void RaiseException(u32 epc, u32 ri_bits);
void RaiseAddressException(u32 address, bool store, bool branch);

// Memory access functions for the JIT - MSB is set on exception.
u64 ReadMemoryByte(u32 pc, u32 address);
u64 ReadMemoryHalfWord(u32 pc, u32 address);
u64 ReadMemoryWord(u32 pc, u32 address);
bool WriteMemoryByte(u32 pc, u32 address, u8 value);
bool WriteMemoryHalfWord(u32 pc, u32 address, u16 value);
bool WriteMemoryWord(u32 pc, u32 address, u32 value);

} // namespace Recompiler::Thunks

} // namespace CPU