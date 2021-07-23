#include "common/log.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_recompiler_code_generator.h"
#include "settings.h"
Log_SetChannel(Recompiler::CodeGenerator);

namespace CPU::Recompiler {

void CodeGenerator::EmitLoadGuestRegister(HostReg host_reg, Reg guest_reg)
{
  EmitLoadCPUStructField(host_reg, RegSize_32, State::GPRRegisterOffset(static_cast<u32>(guest_reg)));
}

void CodeGenerator::EmitStoreGuestRegister(Reg guest_reg, const Value& value)
{
  DebugAssert(value.size == RegSize_32);
  EmitStoreCPUStructField(State::GPRRegisterOffset(static_cast<u32>(guest_reg)), value);
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
  if (address.IsConstant() && !SpeculativeIsCacheIsolated())
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

      if (g_settings.IsUsingFastmem() && Bus::IsRAMAddress(static_cast<u32>(address.constant_value)))
      {
        // have to mask away the high bits for mirrors, since we don't map them in fastmem
        EmitLoadGuestRAMFastmem(Value::FromConstantU32(static_cast<u32>(address.constant_value) & Bus::g_ram_mask),
                                size, result);
      }
      else
      {
        EmitLoadGlobal(result.GetHostRegister(), size, ptr);
      }

      m_delayed_cycles_add += read_ticks;
      return result;
    }
  }

  Value result = m_register_cache.AllocateScratch(HostPointerSize);

  const bool use_fastmem =
    (address_spec ? Bus::CanUseFastmemForAddress(*address_spec) : true) && !SpeculativeIsCacheIsolated();
  if (address_spec)
  {
    if (!use_fastmem)
    {
      Log_ProfilePrintf("Non-constant load at 0x%08X, speculative address 0x%08X, using fastmem = %s", cbi.pc,
                        *address_spec, use_fastmem ? "yes" : "no");
    }
  }
  else
  {
    Log_ProfilePrintf("Non-constant load at 0x%08X, speculative address UNKNOWN, using fastmem = %s", cbi.pc,
                      use_fastmem ? "yes" : "no");
  }

  if (g_settings.IsUsingFastmem() && use_fastmem)
  {
    EmitLoadGuestMemoryFastmem(cbi, address, size, result);
  }
  else
  {
    AddPendingCycles(true);
    m_register_cache.FlushCallerSavedGuestRegisters(true, true);
    EmitLoadGuestMemorySlowmem(cbi, address, size, result, false);
  }

  // Downcast to ignore upper 56/48/32 bits. This should be a noop.
  if (result.size != size)
  {
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
  }

  return result;
}

void CodeGenerator::EmitStoreGuestMemory(const CodeBlockInstruction& cbi, const Value& address,
                                         const SpeculativeValue& address_spec, RegSize size, const Value& value)
{
  if (address.IsConstant() && !SpeculativeIsCacheIsolated())
  {
    void* ptr = GetDirectWriteMemoryPointer(
      static_cast<u32>(address.constant_value),
      (size == RegSize_8) ? MemoryAccessSize::Byte :
                            ((size == RegSize_16) ? MemoryAccessSize::HalfWord : MemoryAccessSize::Word));
    if (ptr)
    {
      if (value.size != size)
        EmitStoreGlobal(ptr, value.ViewAsSize(size));
      else
        EmitStoreGlobal(ptr, value);

      return;
    }
  }

  const bool use_fastmem =
    (address_spec ? Bus::CanUseFastmemForAddress(*address_spec) : true) && !SpeculativeIsCacheIsolated();
  if (address_spec)
  {
    if (!use_fastmem)
    {
      Log_ProfilePrintf("Non-constant store at 0x%08X, speculative address 0x%08X, using fastmem = %s", cbi.pc,
                        *address_spec, use_fastmem ? "yes" : "no");
    }
  }
  else
  {
    Log_ProfilePrintf("Non-constant store at 0x%08X, speculative address UNKNOWN, using fastmem = %s", cbi.pc,
                      use_fastmem ? "yes" : "no");
  }

  if (g_settings.IsUsingFastmem() && use_fastmem)
  {
    EmitStoreGuestMemoryFastmem(cbi, address, size, value);
  }
  else
  {
    AddPendingCycles(true);
    m_register_cache.FlushCallerSavedGuestRegisters(true, true);
    EmitStoreGuestMemorySlowmem(cbi, address, size, value, false);
  }
}

#if 0 // Not used

void CodeGenerator::EmitICacheCheckAndUpdate()
{
  Value temp = m_register_cache.AllocateScratch(RegSize_32);

  if (GetSegmentForAddress(m_pc) >= Segment::KSEG1)
  {
    EmitLoadCPUStructField(temp.GetHostRegister(), RegSize_32, offsetof(State, pending_ticks));
    EmitAdd(temp.GetHostRegister(), temp.GetHostRegister(),
            Value::FromConstantU32(static_cast<u32>(m_block->uncached_fetch_ticks)), false);
    EmitStoreCPUStructField(offsetof(State, pending_ticks), temp);
  }
  else
  {
    // cached path
    Value temp2 = m_register_cache.AllocateScratch(RegSize_32);

    m_register_cache.InhibitAllocation();

    VirtualMemoryAddress current_pc = m_pc & ICACHE_TAG_ADDRESS_MASK;
    for (u32 i = 0; i < m_block->icache_line_count; i++, current_pc += ICACHE_LINE_SIZE)
    {
      const VirtualMemoryAddress tag = GetICacheTagForAddress(current_pc);
      const TickCount fill_ticks = GetICacheFillTicks(current_pc);
      if (fill_ticks <= 0)
        continue;

      const u32 line = GetICacheLine(current_pc);
      const u32 offset = offsetof(State, icache_tags) + (line * sizeof(u32));
      LabelType cache_hit;

      EmitLoadCPUStructField(temp.GetHostRegister(), RegSize_32, offset);
      EmitCopyValue(temp2.GetHostRegister(), Value::FromConstantU32(current_pc));
      EmitCmp(temp2.GetHostRegister(), temp);
      EmitConditionalBranch(Condition::Equal, false, temp.GetHostRegister(), temp2, &cache_hit);

      EmitLoadCPUStructField(temp.GetHostRegister(), RegSize_32, offsetof(State, pending_ticks));
      EmitStoreCPUStructField(offset, temp2);
      EmitAdd(temp.GetHostRegister(), temp.GetHostRegister(), Value::FromConstantU32(static_cast<u32>(fill_ticks)),
              false);
      EmitStoreCPUStructField(offsetof(State, pending_ticks), temp);
      EmitBindLabel(&cache_hit);
    }

    m_register_cache.UninhibitAllocation();
  }
}

#endif

#if 0 // Not Used

void CodeGenerator::EmitStallUntilGTEComplete()
{
  Value pending_ticks = m_register_cache.AllocateScratch(RegSize_32);
  Value gte_completion_tick = m_register_cache.AllocateScratch(RegSize_32);
  EmitLoadCPUStructField(pending_ticks.GetHostRegister(), RegSize_32, offsetof(State, pending_ticks));
  EmitLoadCPUStructField(gte_completion_tick.GetHostRegister(), RegSize_32, offsetof(State, gte_completion_tick));

  // commit cycles here, should always be nonzero
  if (m_delayed_cycles_add > 0)
  {
    EmitAdd(pending_ticks.GetHostRegister(), pending_ticks.GetHostRegister(),
      Value::FromConstantU32(m_delayed_cycles_add), false);
    m_delayed_cycles_add = 0;
  }

  LabelType gte_done;
  EmitSub(gte_completion_tick.GetHostRegister(), gte_completion_tick.GetHostRegister(), pending_ticks, true);
  EmitConditionalBranch(Condition::Below, false, &gte_done);

  // add stall ticks
  EmitAdd(pending_ticks.GetHostRegister(), pending_ticks.GetHostRegister(), gte_completion_tick, false);

  // store new ticks
  EmitBindLabel(&gte_done);
  EmitStoreCPUStructField(offsetof(State, pending_ticks), pending_ticks);
}

#endif

} // namespace CPU::Recompiler
