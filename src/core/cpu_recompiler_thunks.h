#pragma once
#include "common/jit_code_buffer.h"
#include "cpu_core.h"
#include <array>

namespace CPU::Recompiler {

class Thunks
{
public:
  //////////////////////////////////////////////////////////////////////////
  // Trampolines for calling back from the JIT
  // Needed because we can't cast member functions to void*...
  // TODO: Abuse carry flag or something else for exception
  //////////////////////////////////////////////////////////////////////////
  static u64 ReadMemoryByte(Core* cpu, u32 address);
  static u64 ReadMemoryHalfWord(Core* cpu, u32 address);
  static u64 ReadMemoryWord(Core* cpu, u32 address);
  static bool WriteMemoryByte(Core* cpu, u32 address, u8 value);
  static bool WriteMemoryHalfWord(Core* cpu, u32 address, u16 value);
  static bool WriteMemoryWord(Core* cpu, u32 address, u32 value);
  static bool InterpretInstruction(Core* cpu);
  static void UpdateLoadDelay(Core* cpu);
  static void RaiseException(Core* cpu, u8 excode);
  static void RaiseAddressException(Core* cpu, u32 address, bool store, bool branch);
};

class ASMFunctions
{
public:
  bool (*read_memory_byte)(u32 address, u8* value);
  bool (*read_memory_word)(u32 address, u16* value);
  bool (*read_memory_dword)(u32 address, u32* value);
  void (*write_memory_byte)(u32 address, u8 value);
  void (*write_memory_word)(u32 address, u16 value);
  void (*write_memory_dword)(u32 address, u32 value);

  void Generate(JitCodeBuffer* code_buffer);
};

} // namespace CPU_X86::Recompiler
