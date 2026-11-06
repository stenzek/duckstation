// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

class ELFFile;
class Error;

namespace SymbolFile {

struct Symbol
{
  u32 address;
  u32 size; // Function size, or zero for labels/unknown sizes.
  std::string name;
};
using SymbolList = std::vector<Symbol>;

// Parsed symbols are appended to the list.
void ParseELF(const ELFFile& elf, SymbolList* symbols);
bool ParseMap(std::string_view text, SymbolList* symbols, Error* error);
bool Load(const char* path, SymbolList* symbols, Error* error);

void SortSymbols(SymbolList* symbols);

// Lookups require a sorted list. Exact labels take precedence over function ranges, which are assumed not to nest.
std::span<const Symbol> GetSymbolsAtAddress(const SymbolList& symbols, u32 address);
const Symbol* FindSymbol(const SymbolList& symbols, u32 address);

} // namespace SymbolFile
