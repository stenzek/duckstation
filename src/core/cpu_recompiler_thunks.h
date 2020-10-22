#pragma once
#include "cpu_code_cache.h"
#include "cpu_types.h"

namespace CPU {
struct CodeBlockInstruction;

namespace Recompiler::Thunks {

//////////////////////////////////////////////////////////////////////////
// Trampolines for calling back from the JIT
// Needed because we can't cast member functions to void*...
// TODO: Abuse carry flag or something else for exception
//////////////////////////////////////////////////////////////////////////
bool InterpretInstruction();
bool InterpretInstructionPGXP();
void CheckAndUpdateICache(u32 pc, u32 line_count);

// Memory access functions for the JIT - MSB is set on exception.
u64 ReadMemoryByte(u32 address);
u64 ReadMemoryHalfWord(u32 address);
u64 ReadMemoryWord(u32 address);
u32 WriteMemoryByte(u32 address, u8 value);
u32 WriteMemoryHalfWord(u32 address, u16 value);
u32 WriteMemoryWord(u32 address, u32 value);

// Unchecked memory access variants. No alignment or bus exceptions.
u32 UncheckedReadMemoryByte(u32 address);
u32 UncheckedReadMemoryHalfWord(u32 address);
u32 UncheckedReadMemoryWord(u32 address);
void UncheckedWriteMemoryByte(u32 address, u8 value);
void UncheckedWriteMemoryHalfWord(u32 address, u16 value);
void UncheckedWriteMemoryWord(u32 address, u32 value);

void UpdateFastmemMapping();

} // namespace Recompiler::Thunks

} // namespace CPU