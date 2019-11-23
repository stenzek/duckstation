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

void CodeGenerator::EmitStoreInterpreterLoadDelay(Reg reg, const Value& value)
{
  DebugAssert(value.size == RegSize_32 && value.IsInHostRegister());
  EmitStoreCPUStructField(offsetof(Core, m_load_delay_reg), Value::FromConstantU8(static_cast<u8>(reg)));
  EmitStoreCPUStructField(offsetof(Core, m_load_delay_value), value);
  m_load_delay_dirty = true;
}

#if !defined(Y_CPU_X64)
void CodeGenerator::EmitBranch(Condition condition, Reg lr_reg, bool relative, const Value& branch_address)
{
  Panic("Not implemented");
}
void CodeGenerator::EmitRaiseException(Exception excode, Condition condition /* = Condition::Always */)
{
  Panic("Not implemented");
}
#endif

} // namespace CPU::Recompiler