// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "cpu_types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Error;
class SmallStringBase;

namespace CPU {

struct AssemblyLabel
{
  std::string name;
  std::optional<u32> address;
  std::vector<u32> backreferences; // pc list
};
using AssemblyLabelList = std::vector<AssemblyLabel>;

bool AssembleInstruction(u32* dest, u32 pc, std::string_view text, Error* error = nullptr);
bool AssembleInstruction(u32* dest, u32 pc, std::string_view text, AssemblyLabelList* labels, Error* error = nullptr);
AssemblyLabel* DefineAssemblyLabel(AssemblyLabelList* labels, std::string_view name, u32 address,
                                   Error* error = nullptr);
bool FixupAssemblyLabelBackreferences(AssemblyLabel* label, void* userdata, Error* error,
                                      u32* (*instruction_reader)(u32 pc, void* userdata));
bool ContainsUnresolvedLabels(const AssemblyLabelList& labels);
void DisassembleInstruction(SmallStringBase* dest, u32 pc, u32 bits);
void DisassembleInstructionComment(SmallStringBase* dest, u32 pc, u32 bits);

const char* GetGTERegisterName(u32 index);
const char* GetCop0RegisterName(u32 index);

} // namespace CPU
