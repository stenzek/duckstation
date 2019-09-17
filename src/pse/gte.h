#pragma once
#include "common/state_wrapper.h"
#include "gte_types.h"

namespace GTE {

class Core
{
public:
  Core();
  ~Core();

  void Initialize();
  void Reset();
  bool DoState(StateWrapper& sw);

  u32 ReadRegister(u32 index) const { return m_regs.r32[index]; }
  void WriteRegister(u32 index, u32 value) { m_regs.r32[index] = value; }

  u32 ReadDataRegister(u32 index) const { return m_regs.r32[index]; }
  void WriteDataRegister(u32 index, u32 value) { m_regs.r32[index] = value; }

  u32 ReadControlRegister(u32 index) const { return m_regs.r32[index + 32]; }
  void WriteControlRegister(u32 index, u32 value) { m_regs.r32[index + 32] = value; }

  void ExecuteInstruction(Instruction inst);

private:
  Regs m_regs = {};
};

} // namespace GTE