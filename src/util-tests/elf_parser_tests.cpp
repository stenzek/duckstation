// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "util/elf_file.h"

#include "common/error.h"

#include <gtest/gtest.h>

namespace {

// Helper to create minimal valid ELF file data
ELFFile::DataArray CreateValidELFData()
{
  // Create a minimal valid ELF file (32-bit MIPS)
  constexpr size_t elf_size = 768;
  ELFFile::DataArray data(elf_size);
  std::memset(data.data(), 0, data.size());

  // ELF Header
  auto* ehdr = reinterpret_cast<ELFFile::Elf32_Ehdr*>(data.data());
  ehdr->e_ident[0] = 0x7F; // Magic bytes
  ehdr->e_ident[1] = 'E';
  ehdr->e_ident[2] = 'L';
  ehdr->e_ident[3] = 'F';
  ehdr->e_type = ELFFile::ET_EXEC;
  ehdr->e_machine = ELFFile::EM_MIPS;
  ehdr->e_version = 1;
  ehdr->e_entry = 0x80010000;                  // Entry point
  ehdr->e_phoff = sizeof(ELFFile::Elf32_Ehdr); // Program header table right after ELF header
  ehdr->e_shoff =
    sizeof(ELFFile::Elf32_Ehdr) + 2 * sizeof(ELFFile::Elf32_Phdr); // Section header table after program headers
  ehdr->e_flags = 0;
  ehdr->e_ehsize = sizeof(ELFFile::Elf32_Ehdr);
  ehdr->e_phentsize = sizeof(ELFFile::Elf32_Phdr);
  ehdr->e_phnum = 2; // Two program headers
  ehdr->e_shentsize = sizeof(ELFFile::Elf32_Shdr);
  ehdr->e_shnum = 3;    // Three section headers (null + .text + .shstrtab)
  ehdr->e_shstrndx = 2; // String table is section 2

  // Program Headers
  auto* phdr = reinterpret_cast<ELFFile::Elf32_Phdr*>(data.data() + ehdr->e_phoff);

  // First program header - loadable segment with code
  phdr[0].p_type = ELFFile::PT_LOAD;
  phdr[0].p_offset = 0x100;     // Start of section data
  phdr[0].p_vaddr = 0x80010000; // Virtual address matching entry point
  phdr[0].p_filesz = 0x100;     // Size in file
  phdr[0].p_memsz = 0x100;      // Size in memory
  phdr[0].p_flags = 5;          // Read + execute
  phdr[0].p_align = 0x1000;     // Page alignment

  // Second program header - non-loadable segment
  phdr[1].p_type = ELFFile::PT_NOTE;

  // Section Headers
  auto* shdr = reinterpret_cast<ELFFile::Elf32_Shdr*>(data.data() + ehdr->e_shoff);

  // Section 0 - NULL section
  shdr[0].sh_name = 0;
  shdr[0].sh_type = ELFFile::SHT_NULL;

  // Section 1 - .text
  shdr[1].sh_name = 1; // Offset in string table
  shdr[1].sh_type = ELFFile::SHT_PROGBITS;
  shdr[1].sh_flags = 6; // Executable, allocated
  shdr[1].sh_addr = 0x80010000;
  shdr[1].sh_offset = 0x100; // Start of section data
  shdr[1].sh_size = 0x100;   // Size
  shdr[1].sh_link = 0;
  shdr[1].sh_info = 0;
  shdr[1].sh_addralign = 4;
  shdr[1].sh_entsize = 0;

  // Section 2 - .shstrtab (string table)
  shdr[2].sh_name = 7; // Offset in string table
  shdr[2].sh_type = ELFFile::SHT_STRTAB;
  shdr[2].sh_flags = 0;
  shdr[2].sh_addr = 0;
  shdr[2].sh_offset = 0x200; // String table location
  shdr[2].sh_size = 0x20;    // String table size
  shdr[2].sh_link = 0;
  shdr[2].sh_info = 0;
  shdr[2].sh_addralign = 1;
  shdr[2].sh_entsize = 0;

  // String table section
  char* strtab = reinterpret_cast<char*>(data.data() + shdr[2].sh_offset);
  strtab[0] = '\0';                // Empty string at index 0
  strcpy(strtab + 1, ".text");     // Section name at index 1
  strcpy(strtab + 7, ".shstrtab"); // Section name at index 7

  // Add some fake code to the .text section
  u8* text = data.data() + shdr[1].sh_offset;
  memset(text, 0xAA, shdr[1].sh_size);

  return data;
}

// Helper to create invalid ELF data (wrong magic)
ELFFile::DataArray CreateInvalidELFData()
{
  auto data = CreateValidELFData();
  data[1] = 'X'; // Corrupt magic number
  return data;
}

// Helper to create ELF with wrong machine type
ELFFile::DataArray CreateWrongMachineELFData()
{
  auto data = CreateValidELFData();
  auto* ehdr = reinterpret_cast<ELFFile::Elf32_Ehdr*>(data.data());
  ehdr->e_machine = 0x42; // Not MIPS
  return data;
}

// Helper to create ELF with missing entry point
ELFFile::DataArray CreateMissingEntryELFData()
{
  auto data = CreateValidELFData();
  auto* ehdr = reinterpret_cast<ELFFile::Elf32_Ehdr*>(data.data());
  // auto* phdr = reinterpret_cast<ELFFile::Elf32_Phdr*>(data.data() + ehdr->e_phoff);

  // Set entry point outside of loadable segment
  ehdr->e_entry = 0x90000000;

  return data;
}

// Helper to create ELF with out-of-range program header
ELFFile::DataArray CreateOutOfRangeProgramHeaderELFData()
{
  auto data = CreateValidELFData();
  auto* ehdr = reinterpret_cast<ELFFile::Elf32_Ehdr*>(data.data());
  auto* phdr = reinterpret_cast<ELFFile::Elf32_Phdr*>(data.data() + ehdr->e_phoff);

  // Set offset and size to be out of file range
  phdr[0].p_offset = static_cast<u32>(data.size()) - 10;
  phdr[0].p_filesz = 100;

  return data;
}

class ELFParserTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    valid_elf_data = CreateValidELFData();
    invalid_elf_data = CreateInvalidELFData();
    wrong_machine_elf_data = CreateWrongMachineELFData();
    missing_entry_elf_data = CreateMissingEntryELFData();
    out_of_range_program_header_elf_data = CreateOutOfRangeProgramHeaderELFData();
  }

  ELFFile::DataArray valid_elf_data;
  ELFFile::DataArray invalid_elf_data;
  ELFFile::DataArray wrong_machine_elf_data;
  ELFFile::DataArray missing_entry_elf_data;
  ELFFile::DataArray out_of_range_program_header_elf_data;
};

} // namespace

TEST_F(ELFParserTest, ValidELFHeader)
{
  Error error;
  EXPECT_TRUE(ELFFile::IsValidElfHeader(valid_elf_data.cspan(), &error));
  EXPECT_FALSE(error.IsValid());

  // Test the static header checking method
  const auto& header = *reinterpret_cast<const ELFFile::Elf32_Ehdr*>(valid_elf_data.data());
  EXPECT_TRUE(ELFFile::IsValidElfHeader(header, &error));
}

TEST_F(ELFParserTest, InvalidELFHeader)
{
  Error error;
  EXPECT_FALSE(ELFFile::IsValidElfHeader(invalid_elf_data.cspan(), &error));
  EXPECT_TRUE(error.IsValid());
}

TEST_F(ELFParserTest, WrongMachineType)
{
  Error error;
  EXPECT_FALSE(ELFFile::IsValidElfHeader(wrong_machine_elf_data.cspan(), &error));
  EXPECT_TRUE(error.IsValid());
}

TEST_F(ELFParserTest, TooSmallBuffer)
{
  Error error;
  std::span<const u8> small_span = valid_elf_data.cspan(10); // Too small for header
  EXPECT_FALSE(ELFFile::IsValidElfHeader(small_span, &error));
  EXPECT_TRUE(error.IsValid());
}

TEST_F(ELFParserTest, OpenValidELF)
{
  ELFFile elf;
  Error error;
  EXPECT_TRUE(elf.Open(std::move(valid_elf_data), &error));
  EXPECT_FALSE(error.IsValid());
}

TEST_F(ELFParserTest, OpenInvalidELF)
{
  ELFFile elf;
  Error error;
  EXPECT_FALSE(elf.Open(invalid_elf_data, &error));
  EXPECT_TRUE(error.IsValid());
}

TEST_F(ELFParserTest, OpenWrongMachineELF)
{
  ELFFile elf;
  Error error;
  EXPECT_FALSE(elf.Open(wrong_machine_elf_data, &error));
  EXPECT_TRUE(error.IsValid());
}

TEST_F(ELFParserTest, GetELFHeader)
{
  ELFFile elf;
  Error error;
  ASSERT_TRUE(elf.Open(std::move(valid_elf_data), &error));

  const auto& header = elf.GetELFHeader();
  EXPECT_EQ(header.e_type, ELFFile::ET_EXEC);
  EXPECT_EQ(header.e_machine, ELFFile::EM_MIPS);
  EXPECT_EQ(header.e_entry, 0x80010000);
}

TEST_F(ELFParserTest, GetEntryPoint)
{
  ELFFile elf;
  Error error;
  ASSERT_TRUE(elf.Open(std::move(valid_elf_data), &error));

  EXPECT_EQ(elf.GetEntryPoint(), 0x80010000);
}

TEST_F(ELFParserTest, GetSectionCount)
{
  ELFFile elf;
  Error error;
  ASSERT_TRUE(elf.Open(std::move(valid_elf_data), &error));

  EXPECT_EQ(elf.GetSectionCount(), 3u);
}

TEST_F(ELFParserTest, GetValidSectionHeader)
{
  ELFFile elf;
  Error error;
  ASSERT_TRUE(elf.Open(std::move(valid_elf_data), &error));

  // Get .text section
  const auto* section = elf.GetSectionHeader(1);
  ASSERT_NE(section, nullptr);
  EXPECT_EQ(section->sh_type, ELFFile::SHT_PROGBITS);
  EXPECT_EQ(section->sh_addr, 0x80010000);
}

TEST_F(ELFParserTest, GetInvalidSectionHeader)
{
  ELFFile elf;
  Error error;
  ASSERT_TRUE(elf.Open(std::move(valid_elf_data), &error));

  // Try to get a section with an out-of-bounds index
  EXPECT_EQ(elf.GetSectionHeader(99), nullptr);

  // Try the special "undefined" section
  EXPECT_EQ(elf.GetSectionHeader(ELFFile::SHN_UNDEF), nullptr);
}

TEST_F(ELFParserTest, GetSectionName)
{
  ELFFile elf;
  Error error;
  ASSERT_TRUE(elf.Open(std::move(valid_elf_data), &error));

  // Get .text section name
  const auto* section = elf.GetSectionHeader(1);
  ASSERT_NE(section, nullptr);
  EXPECT_EQ(elf.GetSectionName(*section), ".text");

  // Get .shstrtab section name
  const auto* strtab_section = elf.GetSectionHeader(2);
  ASSERT_NE(strtab_section, nullptr);
  EXPECT_EQ(elf.GetSectionName(*strtab_section), ".shstrtab");
}

TEST_F(ELFParserTest, GetProgramHeaderCount)
{
  ELFFile elf;
  Error error;
  ASSERT_TRUE(elf.Open(std::move(valid_elf_data), &error));

  EXPECT_EQ(elf.GetProgramHeaderCount(), 2u);
}

TEST_F(ELFParserTest, GetValidProgramHeader)
{
  ELFFile elf;
  Error error;
  ASSERT_TRUE(elf.Open(std::move(valid_elf_data), &error));

  // Get loadable segment
  const auto* phdr = elf.GetProgramHeader(0);
  ASSERT_NE(phdr, nullptr);
  EXPECT_EQ(phdr->p_type, ELFFile::PT_LOAD);
  EXPECT_EQ(phdr->p_vaddr, 0x80010000u);

  // Get note segment
  const auto* phdr2 = elf.GetProgramHeader(1);
  ASSERT_NE(phdr2, nullptr);
  EXPECT_EQ(phdr2->p_type, ELFFile::PT_NOTE);
}

TEST_F(ELFParserTest, GetInvalidProgramHeader)
{
  ELFFile elf;
  Error error;
  ASSERT_TRUE(elf.Open(std::move(valid_elf_data), &error));

  // Try to get a program header with an out-of-bounds index
  EXPECT_EQ(elf.GetProgramHeader(99), nullptr);
}

TEST_F(ELFParserTest, LoadExecutableSections)
{
  ELFFile elf;
  Error error;
  ASSERT_TRUE(elf.Open(std::move(valid_elf_data), &error));

  // Track loaded sections
  struct LoadedSection
  {
    std::vector<u8> data;
    u32 vaddr;
    u32 size;
  };
  std::vector<LoadedSection> loaded_sections;

  bool result = elf.LoadExecutableSections(
    [&loaded_sections](std::span<const u8> data, u32 dest_vaddr, u32 dest_size, Error* error) {
      LoadedSection section;
      section.data.assign(data.begin(), data.end());
      section.vaddr = dest_vaddr;
      section.size = dest_size;
      loaded_sections.push_back(std::move(section));
      return true;
    },
    &error);

  EXPECT_TRUE(result);
  EXPECT_FALSE(error.IsValid());

  // We should have loaded one section (the loadable segment)
  ASSERT_EQ(loaded_sections.size(), 1u);
  EXPECT_EQ(loaded_sections[0].vaddr, 0x80010000u);
  EXPECT_EQ(loaded_sections[0].size, 0x100u);
  EXPECT_EQ(loaded_sections[0].data[0], 0xAAu); // Check first byte of our fake code
}

TEST_F(ELFParserTest, MissingEntryPoint)
{
  ELFFile elf;
  Error error;
  ASSERT_TRUE(elf.Open(std::move(missing_entry_elf_data), &error));

  bool result = elf.LoadExecutableSections(
    [](std::span<const u8> data, u32 dest_vaddr, u32 dest_size, Error* error) { return true; }, &error);

  EXPECT_FALSE(result);
  EXPECT_TRUE(error.IsValid());
}

TEST_F(ELFParserTest, OutOfRangeProgramHeader)
{
  ELFFile elf;
  Error error;
  ASSERT_TRUE(elf.Open(std::move(out_of_range_program_header_elf_data), &error));

  bool result = elf.LoadExecutableSections(
    [](std::span<const u8> data, u32 dest_vaddr, u32 dest_size, Error* error) { return true; }, &error);

  EXPECT_FALSE(result);
  EXPECT_TRUE(error.IsValid());
}
