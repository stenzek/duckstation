#pragma once
#include "common/string.h"
#include "cpu_types.h"

namespace CPU {
void DisassembleInstruction(String* dest, u32 pc, u32 bits, Registers* regs = nullptr);
} // namespace CPU
