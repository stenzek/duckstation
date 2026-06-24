// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "cpu_types.h"

#include <string_view>

class Error;
class SmallStringBase;

namespace CPU {

bool AssembleInstruction(u32* dest, u32 pc, std::string_view text, Error* error = nullptr);
void DisassembleInstruction(SmallStringBase* dest, u32 pc, u32 bits);
void DisassembleInstructionComment(SmallStringBase* dest, u32 pc, u32 bits);

const char* GetGTERegisterName(u32 index);
const char* GetCop0RegisterName(u32 index);

} // namespace CPU
