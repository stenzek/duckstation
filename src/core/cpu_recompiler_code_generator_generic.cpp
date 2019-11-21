#include "cpu_recompiler_code_generator.h"

namespace CPU::Recompiler {

#if !defined(Y_CPU_X64)
void CodeGenerator::AlignCodeBuffer(JitCodeBuffer* code_buffer) {}
#endif

void CodeGenerator::EmitLoadGuestRegister(HostReg host_reg, Reg guest_reg)
{
  EmitLoadCPUStructField(host_reg, RegSize_32, CalculateRegisterOffset(guest_reg));
}

void CodeGenerator::EmitStoreGuestRegister(Reg guest_reg, const Value& value)
{
  DebugAssert(value.size == RegSize_32);
  EmitStoreCPUStructField(CalculateRegisterOffset(guest_reg), value);
}

void CodeGenerator::EmitStoreLoadDelay(Reg reg, const Value& value)
{
  DebugAssert(value.size == RegSize_32 && value.IsInHostRegister());
  EmitStoreCPUStructField(offsetof(Core, m_load_delay_reg), Value::FromConstantU8(static_cast<u8>(reg)));
  EmitStoreCPUStructField(offsetof(Core, m_load_delay_value), value);

  // We don't want to allocate a register since this could be in a block exit, so re-use the value.
  if (m_register_cache.IsGuestRegisterCached(reg))
  {
    EmitStoreCPUStructField(offsetof(Core, m_load_delay_old_value), m_register_cache.ReadGuestRegister(reg));
  }
  else
  {
    EmitPushHostReg(value.host_reg);
    EmitLoadCPUStructField(value.host_reg, RegSize_32, CalculateRegisterOffset(reg));
    EmitStoreCPUStructField(offsetof(Core, m_load_delay_old_value), value);
    EmitPopHostReg(value.host_reg);
  }

  m_load_delay_dirty = true;
}

} // namespace CPU::Recompiler