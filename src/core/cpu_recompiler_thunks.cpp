#include "cpu_recompiler_thunks.h"

namespace CPU::Recompiler {

// TODO: Port thunks to "ASM routines", i.e. code in the jit buffer.

bool Thunks::ReadMemoryByte(Core* cpu, u32 address, u8* value)
{
  return cpu->ReadMemoryByte(address, value);
}

bool Thunks::ReadMemoryHalfWord(Core* cpu, u32 address, u16* value)
{
  return cpu->ReadMemoryHalfWord(address, value);
}

bool Thunks::ReadMemoryWord(Core* cpu, u32 address, u32* value)
{
  return cpu->ReadMemoryWord(address, value);
}

bool Thunks::WriteMemoryByte(Core* cpu, u32 address, u8 value)
{
  return cpu->WriteMemoryByte(address, value);
}

bool Thunks::WriteMemoryHalfWord(Core* cpu, u32 address, u16 value)
{
  return cpu->WriteMemoryHalfWord(address, value);
}

bool Thunks::WriteMemoryWord(Core* cpu, u32 address, u32 value)
{
  return cpu->WriteMemoryWord(address, value);
}

bool Thunks::InterpretInstruction(Core* cpu)
{
  cpu->ExecuteInstruction();
  return cpu->m_exception_raised;
}

} // namespace CPU::Recompiler