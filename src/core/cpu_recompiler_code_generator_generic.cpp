#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_recompiler_code_generator.h"
#include "settings.h"
#include "common/log.h"
Log_SetChannel(Recompiler::CodeGenerator);

namespace CPU::Recompiler {

void CodeGenerator::EmitLoadGuestRegister(HostReg host_reg, Reg guest_reg)
{
  EmitLoadCPUStructField(host_reg, RegSize_32, CalculateRegisterOffset(guest_reg));
}

void CodeGenerator::EmitStoreGuestRegister(Reg guest_reg, const Value& value)
{
  DebugAssert(value.size == RegSize_32);
  EmitStoreCPUStructField(CalculateRegisterOffset(guest_reg), value);
}

void CodeGenerator::EmitStoreInterpreterLoadDelay(Reg reg, const Value& value)
{
  DebugAssert(value.size == RegSize_32 && value.IsInHostRegister());
  EmitStoreCPUStructField(offsetof(State, load_delay_reg), Value::FromConstantU8(static_cast<u8>(reg)));
  EmitStoreCPUStructField(offsetof(State, load_delay_value), value);
  m_load_delay_dirty = true;
}

Value CodeGenerator::EmitLoadGuestMemory(const CodeBlockInstruction& cbi, const Value& address,
                                         const SpeculativeValue& address_spec, RegSize size)
{
  if (address.IsConstant())
  {
    TickCount read_ticks;
    void* ptr = GetDirectReadMemoryPointer(
      static_cast<u32>(address.constant_value),
      (size == RegSize_8) ? MemoryAccessSize::Byte :
                            ((size == RegSize_16) ? MemoryAccessSize::HalfWord : MemoryAccessSize::Word),
      &read_ticks);
    if (ptr)
    {
      Value result = m_register_cache.AllocateScratch(size);
      EmitLoadGlobal(result.GetHostRegister(), size, ptr);
      m_delayed_cycles_add += read_ticks;
      return result;
    }
  }

  AddPendingCycles(true);

  const bool use_fastmem = address_spec ? Bus::CanUseFastmemForAddress(*address_spec) : true;
  if (address_spec)
  {
    if (!use_fastmem)
      Log_DevPrintf("Non-constant load at 0x%08X, speculative address 0x%08X, using fastmem = %s", cbi.pc,
                    *address_spec, use_fastmem ? "yes" : "no");
  }
  else
  {
    Log_DevPrintf("Non-constant load at 0x%08X, speculative address UNKNOWN, using fastmem = %s", cbi.pc,
                  use_fastmem ? "yes" : "no");
  }

  Value result = m_register_cache.AllocateScratch(RegSize_64);
  if (g_settings.IsUsingFastmem() && use_fastmem)
  {
    EmitLoadGuestMemoryFastmem(cbi, address, size, result);
  }
  else
  {
    m_register_cache.FlushCallerSavedGuestRegisters(true, true);
    EmitLoadGuestMemorySlowmem(cbi, address, size, result, false);
  }

  // Downcast to ignore upper 56/48/32 bits. This should be a noop.
  switch (size)
  {
    case RegSize_8:
      ConvertValueSizeInPlace(&result, RegSize_8, false);
      break;

    case RegSize_16:
      ConvertValueSizeInPlace(&result, RegSize_16, false);
      break;

    case RegSize_32:
      ConvertValueSizeInPlace(&result, RegSize_32, false);
      break;

    default:
      UnreachableCode();
      break;
  }

  return result;
}

void CodeGenerator::EmitStoreGuestMemory(const CodeBlockInstruction& cbi, const Value& address,
                                         const SpeculativeValue& address_spec, const Value& value)
{
  if (address.IsConstant())
  {
    void* ptr = GetDirectWriteMemoryPointer(
      static_cast<u32>(address.constant_value),
      (value.size == RegSize_8) ? MemoryAccessSize::Byte :
                                  ((value.size == RegSize_16) ? MemoryAccessSize::HalfWord : MemoryAccessSize::Word));
    if (ptr)
    {
      EmitStoreGlobal(ptr, value);
      return;
    }
  }

  AddPendingCycles(true);

  const bool use_fastmem = address_spec ? Bus::CanUseFastmemForAddress(*address_spec) : true;
  if (address_spec)
  {
    if (!use_fastmem)
      Log_DevPrintf("Non-constant store at 0x%08X, speculative address 0x%08X, using fastmem = %s", cbi.pc,
                    *address_spec, use_fastmem ? "yes" : "no");
  }
  else
  {
    Log_DevPrintf("Non-constant store at 0x%08X, speculative address UNKNOWN, using fastmem = %s", cbi.pc,
                  use_fastmem ? "yes" : "no");
  }

  if (g_settings.IsUsingFastmem() && use_fastmem)
  {
    EmitStoreGuestMemoryFastmem(cbi, address, value);
  }
  else
  {
    m_register_cache.FlushCallerSavedGuestRegisters(true, true);
    EmitStoreGuestMemorySlowmem(cbi, address, value, false);
  }
}

#ifndef CPU_X64

void CodeGenerator::EmitICacheCheckAndUpdate()
{
  Value pc = CalculatePC();
  Value temp = m_register_cache.AllocateScratch(RegSize_32);
  m_register_cache.InhibitAllocation();

  EmitShr(temp.GetHostRegister(), pc.GetHostRegister(), RegSize_32, Value::FromConstantU32(29));
  LabelType is_cached;
  LabelType ready_to_execute;
  EmitConditionalBranch(Condition::LessEqual, false, temp.GetHostRegister(), Value::FromConstantU32(4), &is_cached);
  EmitLoadCPUStructField(temp.host_reg, RegSize_32, offsetof(State, pending_ticks));
  EmitAdd(temp.host_reg, temp.host_reg, Value::FromConstantU32(static_cast<u32>(m_block->uncached_fetch_ticks)), false);
  EmitStoreCPUStructField(offsetof(State, pending_ticks), temp);
  EmitBranch(&ready_to_execute);
  EmitBindLabel(&is_cached);

  // cached path
  EmitAnd(pc.GetHostRegister(), pc.GetHostRegister(), Value::FromConstantU32(ICACHE_TAG_ADDRESS_MASK));
  VirtualMemoryAddress current_address = (m_block->instructions[0].pc & ICACHE_TAG_ADDRESS_MASK);
  for (u32 i = 0; i < m_block->icache_line_count; i++, current_address += ICACHE_LINE_SIZE)
  {
    const TickCount fill_ticks = GetICacheFillTicks(current_address);
    if (fill_ticks <= 0)
      continue;

    const u32 line = GetICacheLine(current_address);
    const u32 offset = offsetof(State, icache_tags) + (line * sizeof(u32));
    LabelType cache_hit;

    EmitLoadCPUStructField(temp.GetHostRegister(), RegSize_32, offset);
    EmitConditionalBranch(Condition::Equal, false, temp.GetHostRegister(), pc, &cache_hit);
    EmitLoadCPUStructField(temp.host_reg, RegSize_32, offsetof(State, pending_ticks));
    EmitStoreCPUStructField(offset, pc);
    EmitAdd(temp.host_reg, temp.host_reg, Value::FromConstantU32(static_cast<u32>(fill_ticks)), false);
    EmitStoreCPUStructField(offsetof(State, pending_ticks), temp);
    EmitBindLabel(&cache_hit);
    EmitAdd(pc.GetHostRegister(), pc.GetHostRegister(), Value::FromConstantU32(ICACHE_LINE_SIZE), false);
  }

  EmitBindLabel(&ready_to_execute);
  m_register_cache.UnunhibitAllocation();
}

#endif

} // namespace CPU::Recompiler
