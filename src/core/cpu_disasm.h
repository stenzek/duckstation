// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "common/string.h"
#include "cpu_types.h"

namespace CPU {
void DisassembleInstruction(String* dest, u32 pc, u32 bits);
void DisassembleInstructionComment(String* dest, u32 pc, u32 bits, Registers* regs);
} // namespace CPU
