#include "gte.h"

namespace GTE {

Core::Core() = default;

Core::~Core() = default;

void Core::Initialize() {}

void Core::Reset()
{
  m_regs = {};
}

bool Core::DoState(StateWrapper& sw)
{
  sw.DoPOD(&m_regs);
  return !sw.HasError();
}

void Core::ExecuteInstruction(Instruction inst) {}

} // namespace GTE