#pragma once
#include "YBaseLib/String.h"
#include "cpu_types.h"

namespace CPU {
void DisassembleInstruction(String* dest, u32 pc, u32 bits);
} // namespace CPU
