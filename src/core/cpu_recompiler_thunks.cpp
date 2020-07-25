#include "cpu_recompiler_thunks.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"

namespace CPU::Recompiler {

u32 Thunks::MakeRaiseExceptionInfo(Exception excode, const CodeBlockInstruction& cbi)
{
  RaiseExceptionInfo ri = {};
  ri.excode = static_cast<u8>(excode);
  ri.BD = cbi.is_branch_delay_slot;
  ri.CE = cbi.instruction.cop.cop_n;
  return ri.bits;
}

// TODO: Port thunks to "ASM routines", i.e. code in the jit buffer.

u64 Thunks::ReadMemoryByte(Core* cpu, u32 pc, u32 address)
{
  cpu->m_current_instruction_pc = pc;

  u32 temp = 0;
  const TickCount cycles = cpu->DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(address, temp);
  if (cycles < 0)
  {
    cpu->RaiseException(Exception::DBE);
    return UINT64_C(0xFFFFFFFFFFFFFFFF);
  }

  cpu->m_pending_ticks += cycles;
  return ZeroExtend64(temp);
}

u64 Thunks::ReadMemoryHalfWord(Core* cpu, u32 pc, u32 address)
{
  cpu->m_current_instruction_pc = pc;

  if (!cpu->DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(address))
    return UINT64_C(0xFFFFFFFFFFFFFFFF);

  u32 temp = 0;
  const TickCount cycles = cpu->DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(address, temp);
  if (cycles < 0)
  {
    cpu->RaiseException(Exception::DBE);
    return UINT64_C(0xFFFFFFFFFFFFFFFF);
  }

  cpu->m_pending_ticks += cycles;
  return ZeroExtend64(temp);
}

u64 Thunks::ReadMemoryWord(Core* cpu, u32 pc, u32 address)
{
  cpu->m_current_instruction_pc = pc;

  if (!cpu->DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::Word>(address))
    return UINT64_C(0xFFFFFFFFFFFFFFFF);

  u32 temp = 0;
  const TickCount cycles = cpu->DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(address, temp);
  if (cycles < 0)
  {
    cpu->RaiseException(Exception::DBE);
    return UINT64_C(0xFFFFFFFFFFFFFFFF);
  }

  cpu->m_pending_ticks += cycles;
  return ZeroExtend64(temp);
}

bool Thunks::WriteMemoryByte(Core* cpu, u32 pc, u32 address, u8 value)
{
  cpu->m_current_instruction_pc = pc;

  u32 temp = ZeroExtend32(value);
  const TickCount cycles = cpu->DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte>(address, temp);
  if (cycles < 0)
  {
    cpu->RaiseException(Exception::DBE);
    return false;
  }

  DebugAssert(cycles == 0);
  return true;
}

bool Thunks::WriteMemoryHalfWord(Core* cpu, u32 pc, u32 address, u16 value)
{
  cpu->m_current_instruction_pc = pc;

  if (!cpu->DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(address))
    return false;

  u32 temp = ZeroExtend32(value);
  const TickCount cycles = cpu->DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(address, temp);
  if (cycles < 0)
  {
    cpu->RaiseException(Exception::DBE);
    return false;
  }

  DebugAssert(cycles == 0);
  return true;
}

bool Thunks::WriteMemoryWord(Core* cpu, u32 pc, u32 address, u32 value)
{
  cpu->m_current_instruction_pc = pc;

  if (!cpu->DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::Word>(address))
    return false;

  const TickCount cycles = cpu->DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(address, value);
  if (cycles < 0)
  {
    cpu->RaiseException(Exception::DBE);
    return false;
  }

  DebugAssert(cycles == 0);
  return true;
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

void Thunks::RaiseException(Core* cpu, u32 epc, u32 ri_bits)
{
  const RaiseExceptionInfo ri{ri_bits};
  cpu->RaiseException(static_cast<Exception>(ri.excode), epc, ri.BD, cpu->m_current_instruction_was_branch_taken,
                      ri.CE);
}

void Thunks::RaiseAddressException(Core* cpu, u32 address, bool store, bool branch)
{
  cpu->m_cop0_regs.BadVaddr = address;
  if (branch)
    cpu->RaiseException(Exception::AdEL, address, false, false, 0);
  else
    cpu->RaiseException(store ? Exception::AdES : Exception::AdEL);
}
} // namespace CPU::Recompiler