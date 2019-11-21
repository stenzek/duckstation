#include "cpu_recompiler_thunks.h"

namespace CPU::Recompiler {

// TODO: Port thunks to "ASM routines", i.e. code in the jit buffer.

u64 Thunks::ReadMemoryByte(Core* cpu, u32 address)
{
  u32 temp = 0;
  const TickCount cycles = cpu->DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(address, temp);
  if (cycles < 0)
  {
    cpu->RaiseException(Exception::DBE);
    return UINT64_C(0xFFFFFFFFFFFFFFFF);
  }

  cpu->AddTicks(cycles - 1);
  return ZeroExtend64(temp);
}

u64 Thunks::ReadMemoryHalfWord(Core* cpu, u32 address)
{
  if (!cpu->DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(address))
    return UINT64_C(0xFFFFFFFFFFFFFFFF);

  u32 temp = 0;
  const TickCount cycles = cpu->DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(address, temp);
  if (cycles < 0)
  {
    cpu->RaiseException(Exception::DBE);
    return UINT64_C(0xFFFFFFFFFFFFFFFF);
  }

  cpu->AddTicks(cycles - 1);
  return ZeroExtend64(temp);
}

u64 Thunks::ReadMemoryWord(Core* cpu, u32 address)
{
  if (!cpu->DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::Word>(address))
    return UINT64_C(0xFFFFFFFFFFFFFFFF);

  u32 temp = 0;
  const TickCount cycles = cpu->DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(address, temp);
  if (cycles < 0)
  {
    cpu->RaiseException(Exception::DBE);
    return UINT64_C(0xFFFFFFFFFFFFFFFF);
  }

  cpu->AddTicks(cycles - 1);
  return ZeroExtend64(temp);
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

void Thunks::UpdateLoadDelay(Core* cpu)
{
  cpu->UpdateLoadDelay();
}

} // namespace CPU::Recompiler