// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/heap_array.h"
#include "common/types.h"

#include <functional>

class Error;

class ELFFile
{
public:
  using DataArray = DynamicHeapArray<u8>;

  // ELF header constants
  static constexpr u8 EI_NIDENT = 16;
  static constexpr u16 ET_EXEC = 2;
  static constexpr u16 ET_DYN = 3;
  static constexpr u16 EM_MIPS = 8;
  static constexpr u16 SHN_UNDEF = 0;
  static constexpr u32 SHT_NULL = 0;
  static constexpr u32 SHT_PROGBITS = 1;
  static constexpr u32 SHT_SYMTAB = 2;
  static constexpr u32 SHT_STRTAB = 3;
  static constexpr u32 SHT_RELA = 4;
  static constexpr u32 SHT_HASH = 5;
  static constexpr u32 SHT_DYNAMIC = 6;
  static constexpr u32 SHT_NOTE = 7;
  static constexpr u32 SHT_NOBITS = 8;
  static constexpr u32 SHT_REL = 9;
  static constexpr u32 SHT_SHLIB = 10;
  static constexpr u32 SHT_DYNSYM = 11;
  static constexpr u32 SHT_NUM = 12;
  static constexpr u32 PT_NULL = 0;
  static constexpr u32 PT_LOAD = 1;
  static constexpr u32 PT_DYNAMIC = 2;
  static constexpr u32 PT_INTERP = 3;
  static constexpr u32 PT_NOTE = 4;
  static constexpr u32 PT_SHLIB = 5;
  static constexpr u32 PT_PHDR = 6;
  static constexpr u32 PT_TLS = 7;

  // ELF Header structure
  struct Elf32_Ehdr
  {
    u8 e_ident[EI_NIDENT]; // Magic number and other information
    u16 e_type;            // Object file type
    u16 e_machine;         // Architecture
    u32 e_version;         // Object file version
    u32 e_entry;           // Entry point virtual address
    u32 e_phoff;           // Program header table file offset
    u32 e_shoff;           // Section header table file offset
    u32 e_flags;           // Processor-specific flags
    u16 e_ehsize;          // ELF header size in bytes
    u16 e_phentsize;       // Program header table entry size
    u16 e_phnum;           // Program header table entry count
    u16 e_shentsize;       // Section header table entry size
    u16 e_shnum;           // Section header table entry count
    u16 e_shstrndx;        // Section header string table index
  };

  // Section header structure
  struct Elf32_Shdr
  {
    u32 sh_name;      // Section name (string tbl index)
    u32 sh_type;      // Section type
    u32 sh_flags;     // Section flags
    u32 sh_addr;      // Section virtual addr at execution
    u32 sh_offset;    // Section file offset
    u32 sh_size;      // Section size in bytes
    u32 sh_link;      // Link to another section
    u32 sh_info;      // Additional section information
    u32 sh_addralign; // Section alignment
    u32 sh_entsize;   // Entry size if section holds table
  };

  // Program header structure
  struct Elf32_Phdr
  {
    u32 p_type;
    u32 p_offset;
    u32 p_vaddr;
    u32 p_paddr;
    u32 p_filesz;
    u32 p_memsz;
    u32 p_flags;
    u32 p_align;
  };

public:
  ELFFile();
  ~ELFFile();

  static bool IsValidElfHeader(const std::span<const u8> data, Error* error = nullptr);
  static bool IsValidElfHeader(const Elf32_Ehdr& header, Error* error = nullptr);

  const Elf32_Ehdr& GetELFHeader() const;
  u32 GetEntryPoint() const;

  const Elf32_Shdr* GetSectionHeader(u32 index) const;
  std::string_view GetSectionName(const Elf32_Shdr& section) const;
  u32 GetSectionCount() const;

  const Elf32_Phdr* GetProgramHeader(u32 index) const;
  u32 GetProgramHeaderCount() const;

  bool Open(const char* path, Error* error);
  bool Open(DataArray data, Error* error);

  using LoadExecutableSectionCallback =
    std::function<bool(std::span<const u8> data, u32 dest_vaddr, u32 dest_size, Error* error)>;
  bool LoadExecutableSections(const LoadExecutableSectionCallback& callback, Error* error) const;

private:
  DataArray m_data;
};
