// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "symbol_file.h"
#include "elf_file.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/string_util.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

void SymbolFile::ParseELF(const ELFFile& elf, SymbolList* symbols)
{
  elf.EnumerateSymbols([symbols](u32 address, u32 size, u8 type, std::string_view name) {
    symbols->push_back({address, (type == ELFFile::STT_FUNC) ? size : 0, std::string(name)});
  });
}

bool SymbolFile::ParseMap(std::string_view text, SymbolList* symbols, Error* error)
{
  // "0xSTART 0xSIZE", returns the end address of the section.
  const auto parse_section_end = [](std::string_view str) -> u32 {
    const size_t split = str.find_first_of(" \t");
    if (!str.starts_with("0x") || split == std::string_view::npos)
      return 0;
    std::string_view size_str = StringUtil::StripWhitespace(str.substr(split));
    size_str = size_str.substr(0, size_str.find_first_of(" \t"));
    const std::optional<u32> start = StringUtil::FromChars<u32>(str.substr(2, split - 2), 16);
    const std::optional<u32> size =
      size_str.starts_with("0x") ? StringUtil::FromChars<u32>(size_str.substr(2), 16) : std::nullopt;
    return (start.has_value() && size.has_value()) ? (start.value() + size.value()) : 0;
  };

  // Maps don't record symbol sizes, so assume each runs until the next symbol or the end of its section.
  size_t section_first = symbols->size();
  u32 section_end = 0;
  const auto finish_section = [symbols, &section_first, &section_end]() {
    std::stable_sort(symbols->begin() + section_first, symbols->end(),
                     [](const Symbol& lhs, const Symbol& rhs) { return lhs.address < rhs.address; });
    u32 next_address = section_end;
    for (size_t i = symbols->size(); i > section_first;)
    {
      i--;
      Symbol& symbol = (*symbols)[i];
      if ((i + 1) < symbols->size() && (*symbols)[i + 1].address != symbol.address)
        next_address = (*symbols)[i + 1].address;
      symbol.size = (next_address > symbol.address) ? (next_address - symbol.address) : 0;
    }
    section_first = symbols->size();
  };

  bool found_map = false;
  bool in_section = false;
  bool section_wrapped = false;
  while (!text.empty())
  {
    const size_t newline = text.find('\n');
    const std::string_view line = text.substr(0, newline);
    text = (newline == std::string_view::npos) ? std::string_view() : text.substr(newline + 1);
    const std::string_view trimmed = StringUtil::StripWhitespace(line);
    if (trimmed == "Linker script and memory map")
    {
      found_map = true;
      continue;
    }
    if (!found_map || trimmed.empty())
      continue;
    if (trimmed == "Cross Reference Table")
      break;

    // Output sections start in column zero, everything else is indented.
    if (line.front() != ' ' && line.front() != '\t')
    {
      finish_section();
      in_section = trimmed.starts_with('.') && !trimmed.starts_with(".debug");
      const size_t name_end = trimmed.find_first_of(" \t");
      section_wrapped = (name_end == std::string_view::npos);
      section_end = section_wrapped ? 0 : parse_section_end(StringUtil::StripWhitespace(trimmed.substr(name_end)));
      continue;
    }

    // Long section names put the address and size on the next line.
    if (section_wrapped)
    {
      section_wrapped = false;
      section_end = parse_section_end(trimmed);
      if (section_end != 0)
        continue;
    }

    // Symbol lines are "0xADDRESS name". Skip input section contributions (second number), assignments, etc.
    if (!in_section || !trimmed.starts_with("0x"))
      continue;
    const size_t split = trimmed.find_first_of(" \t");
    if (split == std::string_view::npos)
      continue;
    const std::string_view name = StringUtil::StripWhitespace(trimmed.substr(split));
    if (name.empty() || name.starts_with("0x") || name.starts_with('(') || name.find(" = ") != std::string_view::npos)
      continue;

    const std::optional<u32> address = StringUtil::FromChars<u32>(trimmed.substr(2, split - 2), 16);
    if (!address.has_value())
    {
      Error::SetStringView(error, "Invalid address in GNU linker map.");
      return false;
    }
    symbols->push_back({address.value(), 0, std::string(name)});
  }
  finish_section();
  if (!found_map)
  {
    Error::SetStringView(error, "Not a GNU linker map (missing linker script and memory map).");
    return false;
  }
  return true;
}

bool SymbolFile::Load(const char* path, SymbolList* symbols, Error* error)
{
  auto fp = FileSystem::OpenManagedCFile(path, "rb", error);
  if (!fp)
    return false;

  const s64 size = FileSystem::FSize64(fp.get(), error);
  if (size < 0)
    return false;
  if (size >= 32 * 1024 * 1024)
  {
    Error::SetStringView(error, "Symbol file is too large.");
    return false;
  }
  ELFFile::DataArray data(static_cast<size_t>(size));
  if (size > 0 && std::fread(data.data(), data.size(), 1, fp.get()) != 1)
  {
    Error::SetErrno(error, "Failed to read symbol file: ", errno);
    return false;
  }
  if (data.size() >= 4 && std::memcmp(data.data(), "\177ELF", 4) == 0)
  {
    ELFFile elf;
    if (!elf.Open(std::move(data), error))
      return false;
    ParseELF(elf, symbols);
    return true;
  }
  return ParseMap(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), symbols, error);
}

void SymbolFile::SortSymbols(SymbolList* symbols)
{
  std::ranges::sort(*symbols, [](const Symbol& lhs, const Symbol& rhs) {
    if (lhs.address != rhs.address)
      return lhs.address < rhs.address;
    if (lhs.name != rhs.name)
      return lhs.name < rhs.name;
    return lhs.size > rhs.size;
  });
  symbols->erase(std::unique(symbols->begin(), symbols->end(),
                             [](const Symbol& lhs, const Symbol& rhs) {
                               return lhs.address == rhs.address && lhs.name == rhs.name;
                             }),
                 symbols->end());
}

std::span<const SymbolFile::Symbol> SymbolFile::GetSymbolsAtAddress(const SymbolList& symbols, u32 address)
{
  const auto first = std::ranges::lower_bound(symbols, address, {}, &Symbol::address);
  const auto last = std::ranges::upper_bound(first, symbols.end(), address, {}, &Symbol::address);
  return std::span(first, last);
}

const SymbolFile::Symbol* SymbolFile::FindSymbol(const SymbolList& symbols, u32 address)
{
  auto it = std::ranges::lower_bound(symbols, address, {}, &Symbol::address);
  if (it != symbols.end() && it->address == address)
    return &*it;

  // Skip labels, functions don't nest, so the closest preceding sized symbol is the only candidate.
  while (it != symbols.begin())
  {
    --it;
    if (it->size != 0)
      return ((address - it->address) < it->size) ? &*it : nullptr;
  }
  return nullptr;
}
