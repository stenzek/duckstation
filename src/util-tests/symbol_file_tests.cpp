// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "util/elf_file.h"
#include "util/symbol_file.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/scoped_guard.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <functional>

namespace {

ELFFile::DataArray CreateSymbolELF()
{
  ELFFile::DataArray data(2048);
  std::memset(data.data(), 0, data.size());
  auto& hdr = *reinterpret_cast<ELFFile::Elf32_Ehdr*>(data.data());
  std::memcpy(hdr.e_ident, "\177ELF\1\1\1", 7);
  hdr.e_type = ELFFile::ET_EXEC;
  hdr.e_machine = ELFFile::EM_MIPS;
  hdr.e_version = 1;
  hdr.e_ehsize = sizeof(hdr);
  hdr.e_shoff = 64;
  hdr.e_shentsize = sizeof(ELFFile::Elf32_Shdr);
  hdr.e_shnum = 5;
  auto* sections = reinterpret_cast<ELFFile::Elf32_Shdr*>(data.data() + hdr.e_shoff);
  sections[1].sh_type = ELFFile::SHT_NOBITS;
  sections[1].sh_flags = ELFFile::SHF_ALLOC;
  sections[2].sh_type = ELFFile::SHT_SYMTAB;
  sections[2].sh_offset = 512;
  sections[2].sh_entsize = sizeof(ELFFile::Elf32_Sym);
  sections[2].sh_link = 3;
  sections[3].sh_type = ELFFile::SHT_STRTAB;
  sections[3].sh_offset = 1024;
  sections[3].sh_size = 1;
  sections[4].sh_type = ELFFile::SHT_PROGBITS; // Non-allocated debug section.
  auto* symbols = reinterpret_cast<ELFFile::Elf32_Sym*>(data.data() + sections[2].sh_offset);
  size_t count = 1;
  const auto add = [&](const char* name, u32 address, u32 size, u8 info, u16 section) {
    const u32 offset = sections[3].sh_size;
    const u32 length = static_cast<u32>(std::strlen(name) + 1);
    std::memcpy(data.data() + sections[3].sh_offset + offset, name, length);
    sections[3].sh_size += length;
    symbols[count++] = {offset, address, size, info, 0, section};
  };
  add("_start", 0x80010000, 0, 0x12, 1);
  add("main", 0x8001005C, 9904, 0x12, 1);
  add("tick", 0x800139F0, 52, 0x02, 1);
  add("main_alias", 0x8001005C, 9904, 0x22, 1);
  add("data", 0x8001A800, 4, 0x11, 1);
  add("_bss_init", 0x80010044, 0, 0, 1);
  add("undefined", 0, 0, 0x12, 0);
  add("file.c", 0, 0, 4, 0xFFF1);
  add("RAM_SIZE", 0x200000, 0, 0x10, 0xFFF1);
  add("common", 4, 4, 0x11, 0xFFF2);
  add("debug", 0x10, 4, 0x12, 4);
  add("section", 0x80010000, 0, 3, 1);
  sections[2].sh_size = static_cast<u32>(count * sizeof(ELFFile::Elf32_Sym));
  return data;
}

ELFFile::Elf32_Shdr& Section(ELFFile::DataArray& data, u32 index)
{
  const auto& hdr = *reinterpret_cast<const ELFFile::Elf32_Ehdr*>(data.data());
  return *reinterpret_cast<ELFFile::Elf32_Shdr*>(data.data() + hdr.e_shoff + hdr.e_shentsize * index);
}

ELFFile::Elf32_Sym& Symbol(ELFFile::DataArray& data, u32 index)
{
  return *reinterpret_cast<ELFFile::Elf32_Sym*>(data.data() + Section(data, 2).sh_offset +
                                                sizeof(ELFFile::Elf32_Sym) * index);
}

bool ParseELF(ELFFile::DataArray data, SymbolFile::SymbolList* symbols, Error* error)
{
  ELFFile elf;
  if (!elf.Open(std::move(data), error))
    return false;
  SymbolFile::ParseELF(elf, symbols);
  return true;
}

// Reduced from gpu-quad-dreq.map, retaining the structures that can otherwise
// be mistaken for symbols. No dependency on the external test executable.
constexpr std::string_view MAP = R"(
Discarded input sections
 .text.discarded 0x00000000 0x20 unused.o
                0x00000000 discarded_label
Memory Configuration
ram 0x80010000 0x001f0000 xrw
Linker script and memory map
LOAD gpu-quad-dreq.o
                0x00200000 RAM_SIZE = DEFINED (RAM_SIZE)?RAM_SIZE:0x200000
.PSX_EXE_Header
                0x8000f800 0x800
                0x8000f800 0x1 BYTE 0x50
                0x80010000 . = (. + 0x7c8)
.text           0x80010000 0x8c30
 .start         0x80010000 0x5c crt0.o
                0x80010000 _start
 .text.startup
                0x8001005c 0x26b0 gpu-quad-dreq.o
                0x8001005c main
                0x80012bf4 operator new(unsigned int)
                0x80012c14 operator delete(void*, unsigned int)
                0x80018c30 . = ALIGN (0x10)
 *fill*         0x80018c24 0xc
.text_memcpy    0x80018c30 0x28
 .text_memcpy   0x80018c30 0x28 memory-s.o
                0x80018c30 memcpy
.rodata         0x80018c64 0x166c
 .rodata.str1.4
                0x80018c64 0x11d4 xprintf.o
                0x5f (size before relaxing)
.data           0x8001a2d0 0x530
                0x8001a464 superTestInstance
                0x8001a800 __bss_start = .
/DISCARD/
                0x80010000 discarded_again
.debug_info     0x00000000 0x100
                0x00000010 debug_label
Cross Reference Table
                0x80010000 cross_reference
)";

TEST(SymbolFile, ELFSymbolsOwnNamesAndKeepLocalWeakAndZeroSize)
{
  SymbolFile::SymbolList symbols;
  Error error;
  ASSERT_TRUE(ParseELF(CreateSymbolELF(), &symbols, &error)) << error.GetDescription();
  ASSERT_EQ(symbols.size(), 6u);
  EXPECT_EQ(symbols[0].name, "_start");
  EXPECT_EQ(symbols[0].size, 0u);
  EXPECT_EQ(symbols[1].name, "main");
  EXPECT_EQ(symbols[1].address, 0x8001005Cu);
  EXPECT_EQ(symbols[1].size, 9904u);
  EXPECT_EQ(symbols[2].name, "tick");
  EXPECT_EQ(symbols[2].size, 52u);
  EXPECT_EQ(symbols[3].name, "main_alias");
  EXPECT_EQ(symbols[4].size, 0u);
  EXPECT_EQ(symbols[5].name, "_bss_init");
}

TEST(SymbolFile, ELFDynamicAndStrippedSymbols)
{
  auto data = CreateSymbolELF();
  Section(data, 2).sh_type = ELFFile::SHT_DYNSYM;
  SymbolFile::SymbolList symbols;
  Error error;
  ASSERT_TRUE(ParseELF(std::move(data), &symbols, &error));
  EXPECT_EQ(symbols.size(), 6u);
  data = CreateSymbolELF();
  reinterpret_cast<ELFFile::Elf32_Ehdr*>(data.data())->e_shnum = 0;
  symbols.clear();
  ASSERT_TRUE(ParseELF(std::move(data), &symbols, &error));
  EXPECT_TRUE(symbols.empty());
}

TEST(SymbolFile, ELFSkipsMalformedTables)
{
  using Mutator = std::function<void(ELFFile::DataArray&)>;
  const Mutator cases[] = {
    [](auto& data) { reinterpret_cast<ELFFile::Elf32_Ehdr*>(data.data())->e_shoff = 0xFFFFFFF0; },
    [](auto& data) { reinterpret_cast<ELFFile::Elf32_Ehdr*>(data.data())->e_shentsize = 1; },
    [](auto& data) { Section(data, 2).sh_offset = 0xFFFFFFF0; },
    [](auto& data) { Section(data, 2).sh_entsize = 0; },
    [](auto& data) { Section(data, 2).sh_size = 0xFFFFFFFF; },
    [](auto& data) { Section(data, 2).sh_link = 99; },
    [](auto& data) { Section(data, 3).sh_type = ELFFile::SHT_PROGBITS; },
    [](auto& data) { Section(data, 3).sh_size = 0xFFFFFFFF; },
  };
  for (size_t i = 0; i < std::size(cases); i++)
  {
    SCOPED_TRACE(i);
    auto data = CreateSymbolELF();
    cases[i](data);
    SymbolFile::SymbolList symbols;
    Error error;
    EXPECT_TRUE(ParseELF(std::move(data), &symbols, &error));
    EXPECT_TRUE(symbols.empty());
  }

  // Bad individual entries are skipped, unterminated names are cut off at the end of the table.
  auto data = CreateSymbolELF();
  Symbol(data, 2).st_name = Section(data, 3).sh_size;
  Symbol(data, 3).st_shndx = 99;
  Section(data, 3).sh_size = Symbol(data, 6).st_name + 6;
  SymbolFile::SymbolList symbols;
  Error error;
  ASSERT_TRUE(ParseELF(std::move(data), &symbols, &error));
  ASSERT_EQ(symbols.size(), 4u);
  EXPECT_EQ(symbols[0].name, "_start");
  EXPECT_EQ(symbols[3].name, "_bss_i");
}

TEST(SymbolFile, GNUMapSymbolsAndFalsePositives)
{
  SymbolFile::SymbolList symbols;
  Error error;
  ASSERT_TRUE(SymbolFile::ParseMap(MAP, &symbols, &error));
  ASSERT_EQ(symbols.size(), 6u);
  EXPECT_EQ(symbols[0].name, "_start");
  EXPECT_EQ(symbols[0].address, 0x80010000u);
  EXPECT_EQ(symbols[1].name, "main");
  EXPECT_EQ(symbols[1].address, 0x8001005Cu);
  EXPECT_EQ(symbols[2].name, "operator new(unsigned int)");
  EXPECT_EQ(symbols[3].name, "operator delete(void*, unsigned int)");
  EXPECT_EQ(symbols[4].name, "memcpy");
  EXPECT_EQ(symbols[4].address, 0x80018C30u);
  EXPECT_EQ(symbols[5].name, "superTestInstance");

  // Sizes run to the next symbol or the end of the output section.
  EXPECT_EQ(symbols[0].size, 0x5Cu);
  EXPECT_EQ(symbols[1].size, 0x2B98u);
  EXPECT_EQ(symbols[2].size, 0x20u);
  EXPECT_EQ(symbols[3].size, 0x601Cu);
  EXPECT_EQ(symbols[4].size, 0x28u);
  EXPECT_EQ(symbols[5].size, 0x39Cu);
}

TEST(SymbolFile, GNUMapSizesWithWrappedSectionsAndAliases)
{
  SymbolFile::SymbolList symbols;
  Error error;
  ASSERT_TRUE(SymbolFile::ParseMap("Linker script and memory map\n"
                                   ".text.a_very_long_section_name\n"
                                   "                0x80010000      0x100\n"
                                   "                0x80010040                second\n"
                                   "                0x80010000                first\n"
                                   "                0x80010000                first_alias\n"
                                   ".data           0x80020000       0x10 load address 0x80010100\n"
                                   "                0x80020008                variable\n",
                                   &symbols, &error));
  ASSERT_EQ(symbols.size(), 4u);
  EXPECT_EQ(symbols[0].name, "first");
  EXPECT_EQ(symbols[0].size, 0x40u);
  EXPECT_EQ(symbols[1].name, "first_alias");
  EXPECT_EQ(symbols[1].size, 0x40u);
  EXPECT_EQ(symbols[2].name, "second");
  EXPECT_EQ(symbols[2].size, 0xC0u);
  EXPECT_EQ(symbols[3].name, "variable");
  EXPECT_EQ(symbols[3].size, 0x8u);
}

TEST(SymbolFile, GNUMapCRLFTabsAliasesAndUnalignedLabels)
{
  SymbolFile::SymbolList symbols;
  Error error;
  ASSERT_TRUE(SymbolFile::ParseMap("Linker script and memory map\r\n.text\t0x80010000 0x20\r\n"
                                   "\t0x80010000\tmain\r\n\t0x80010000\talias\r\n\t0x80010001\tbyte_label",
                                   &symbols, &error));
  ASSERT_EQ(symbols.size(), 3u);
  EXPECT_EQ(symbols[1].name, "alias");
  EXPECT_EQ(symbols[1].size, 1u);
  EXPECT_EQ(symbols[2].address, 0x80010001u);
  EXPECT_EQ(symbols[2].size, 0x1Fu);
}

TEST(SymbolFile, GNUMapErrors)
{
  for (const std::string_view text : {"random text", "Linker script and memory map\n.text\n 0x100000000 overflow"})
  {
    SymbolFile::SymbolList symbols;
    Error error;
    EXPECT_FALSE(SymbolFile::ParseMap(text, &symbols, &error));
    EXPECT_TRUE(error.IsValid());
  }
  SymbolFile::SymbolList symbols;
  Error error;
  ASSERT_TRUE(SymbolFile::ParseMap("Linker script and memory map\n", &symbols, &error));
  EXPECT_TRUE(symbols.empty());
}

TEST(SymbolFile, GNUMapAssignmentsDoNotHideOperatorNames)
{
  SymbolFile::SymbolList symbols;
  Error error;
  ASSERT_TRUE(SymbolFile::ParseMap("Linker script and memory map\n.text\n"
                                   " 0x80010000 foo = (.)\n"
                                   " 0x80010000 [!provide] PROVIDE (foo = .)\n"
                                   " 0x80010000 PROVIDE (foo = .)\n"
                                   " 0x80010000 operator==(Thing const&, Thing const&)\n"
                                   " 0x80010004 Thing::operator=(Thing const&)\n",
                                   &symbols, &error));
  ASSERT_EQ(symbols.size(), 2u);
  EXPECT_EQ(symbols[0].name, "operator==(Thing const&, Thing const&)");
  EXPECT_EQ(symbols[1].name, "Thing::operator=(Thing const&)");
}

TEST(SymbolFile, LoadDetectsFileContents)
{
  std::string path;
  auto fp =
    FileSystem::OpenTemporaryManagedCFile(Path::Combine(FileSystem::GetWorkingDirectory(), "symbol-file-test"), &path);
  ASSERT_TRUE(fp);
  fp.reset();
  const ScopedGuard cleanup([&path]() { FileSystem::DeleteFile(path.c_str()); });
  const auto data = CreateSymbolELF();
  ASSERT_TRUE(FileSystem::WriteBinaryFile(path.c_str(), data.cspan()));
  SymbolFile::SymbolList symbols;
  Error error;
  ASSERT_TRUE(SymbolFile::Load(path.c_str(), &symbols, &error));
  EXPECT_EQ(symbols.size(), 6u);
  ASSERT_TRUE(FileSystem::WriteStringToFile(path.c_str(), MAP));
  symbols.clear();
  ASSERT_TRUE(SymbolFile::Load(path.c_str(), &symbols, &error));
  ASSERT_EQ(symbols.size(), 6u);
  EXPECT_EQ(symbols[2].name, "operator new(unsigned int)");
}

TEST(SymbolFile, SortGroupsAliasesAndKeepsLargestDuplicate)
{
  SymbolFile::SymbolList symbols = {
    {0x1004, 0, "next"},
    {0x1000, 0, "function"},
    {0x1000, 32, "function"},
    {0x1000, 0, "alias"},
  };
  SymbolFile::SortSymbols(&symbols);
  ASSERT_EQ(symbols.size(), 3u);
  const auto labels = SymbolFile::GetSymbolsAtAddress(symbols, 0x1000);
  ASSERT_EQ(labels.size(), 2u);
  EXPECT_EQ(labels[0].name, "alias");
  EXPECT_EQ(labels[1].name, "function");
  EXPECT_EQ(labels[1].size, 32u);
  EXPECT_TRUE(SymbolFile::GetSymbolsAtAddress(symbols, 0x1008).empty());
}

TEST(SymbolFile, LookupUsesExactLabelsAndFunctionSizes)
{
  SymbolFile::SymbolList symbols = {
    {0x1000, 32, "func"}, {0x1000, 0, "alias"}, {0x1008, 0, "label"}, {0x1020, 8, "next"}, {0x1040, 0, "data"},
  };
  SymbolFile::SortSymbols(&symbols);
  EXPECT_EQ(SymbolFile::FindSymbol(symbols, 0x1000)->name, "alias");
  EXPECT_EQ(SymbolFile::FindSymbol(symbols, 0x1004)->name, "func");
  EXPECT_EQ(SymbolFile::FindSymbol(symbols, 0x1008)->name, "label");
  EXPECT_EQ(SymbolFile::FindSymbol(symbols, 0x100C)->name, "func");
  EXPECT_EQ(SymbolFile::FindSymbol(symbols, 0x1024)->name, "next");
  EXPECT_EQ(SymbolFile::FindSymbol(symbols, 0x1028), nullptr);
  EXPECT_EQ(SymbolFile::FindSymbol(symbols, 0x1040)->name, "data");
  EXPECT_EQ(SymbolFile::FindSymbol(symbols, 0x1044), nullptr);
  EXPECT_EQ(SymbolFile::FindSymbol(symbols, 0xFFC), nullptr);
  EXPECT_EQ(SymbolFile::FindSymbol({}, 0x1000), nullptr);
}

} // namespace
