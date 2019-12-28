#pragma once
#include "cpu_types.h"

class JitCodeBuffer;

namespace CPU {

struct CodeBlockInstruction;

class Core;

namespace Recompiler {

class Thunks
{
public:
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

  static u32 MakeRaiseExceptionInfo(Exception excode, const CodeBlockInstruction& cbi);

  //////////////////////////////////////////////////////////////////////////
  // Trampolines for calling back from the JIT
  // Needed because we can't cast member functions to void*...
  // TODO: Abuse carry flag or something else for exception
  //////////////////////////////////////////////////////////////////////////
  static u64 ReadMemoryByte(Core* cpu, u32 pc, u32 address);
  static u64 ReadMemoryHalfWord(Core* cpu, u32 pc, u32 address);
  static u64 ReadMemoryWord(Core* cpu, u32 pc, u32 address);
  static bool WriteMemoryByte(Core* cpu, u32 pc, u32 address, u8 value);
  static bool WriteMemoryHalfWord(Core* cpu, u32 pc, u32 address, u16 value);
  static bool WriteMemoryWord(Core* cpu, u32 pc, u32 address, u32 value);
  static bool InterpretInstruction(Core* cpu);
  static void UpdateLoadDelay(Core* cpu);
  static void RaiseException(Core* cpu, u32 epc, u32 ri_bits);
  static void RaiseAddressException(Core* cpu, u32 address, bool store, bool branch);
  static void ExecuteGTEInstruction(Core* cpu, u32 instruction_bits);
  static u32 ReadGTERegister(Core* cpu, u32 reg);
  static void WriteGTERegister(Core* cpu, u32 reg, u32 value);
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

} // namespace Recompiler

} // namespace CPU