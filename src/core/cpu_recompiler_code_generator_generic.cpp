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


} // namespace CPU::Recompiler