#pragma once
#include "cpu_core.h"

namespace CPU {

// exceptions
void RaiseException(Exception excode);
void RaiseException(u32 CAUSE_bits, u32 EPC);

// defined in cpu_memory.cpp - memory access functions which return false if an exception was thrown.
bool FetchInstruction();
bool ReadMemoryByte(VirtualMemoryAddress addr, u8* value);
bool ReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value);
bool ReadMemoryWord(VirtualMemoryAddress addr, u32* value);
bool WriteMemoryByte(VirtualMemoryAddress addr, u8 value);
bool WriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value);
bool WriteMemoryWord(VirtualMemoryAddress addr, u32 value);

} // namespace CPU