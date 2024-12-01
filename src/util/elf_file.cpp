// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "elf_file.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"

LOG_CHANNEL(FileLoader);

static constexpr const u8 EXPECTED_ELF_HEADER[4] = {'\177', 'E', 'L', 'F'};
static constexpr s64 MAX_ELF_FILE_SIZE = 32 * 1024 * 1024;

ELFFile::ELFFile() = default;

ELFFile::~ELFFile() = default;

const ELFFile::Elf32_Ehdr& ELFFile::GetELFHeader() const
{
  return *reinterpret_cast<const Elf32_Ehdr*>(&m_data[0]);
}

u32 ELFFile::GetEntryPoint() const
{
  return GetELFHeader().e_entry;
}

const ELFFile::Elf32_Shdr* ELFFile::GetSectionHeader(u32 index) const
{
  const Elf32_Ehdr& hdr = GetELFHeader();
  if (index == SHN_UNDEF || index >= hdr.e_shnum || hdr.e_shentsize < sizeof(Elf32_Shdr))
    return nullptr;

  const size_t offset = hdr.e_shoff + index * static_cast<size_t>(hdr.e_shentsize);
  if ((offset + sizeof(Elf32_Shdr)) > m_data.size())
    return nullptr;

  return reinterpret_cast<const Elf32_Shdr*>(&m_data[offset]);
}

std::string_view ELFFile::GetSectionName(const Elf32_Shdr& section) const
{
  const Elf32_Shdr* strhdr = GetSectionHeader(GetELFHeader().e_shstrndx);
  if (!strhdr || section.sh_name >= strhdr->sh_size)
    return std::string_view();

  const size_t file_offset = strhdr->sh_offset;
  const u32 start_offset = section.sh_name;
  u32 current_offset = start_offset;
  while (current_offset < strhdr->sh_size && (current_offset + file_offset) < m_data.size())
  {
    if (m_data[file_offset + current_offset] == '\0')
      break;

    current_offset++;
  }
  if (current_offset == start_offset)
    return std::string_view();

  return std::string_view(reinterpret_cast<const char*>(&m_data[file_offset + start_offset]),
                          current_offset - start_offset);
}

u32 ELFFile::GetSectionCount() const
{
  return GetELFHeader().e_shnum;
}

const ELFFile::Elf32_Phdr* ELFFile::GetProgramHeader(u32 index) const
{
  const Elf32_Ehdr& hdr = GetELFHeader();
  if (index >= hdr.e_phnum || hdr.e_phentsize < sizeof(Elf32_Phdr))
    return nullptr;

  const size_t offset = hdr.e_phoff + index * static_cast<size_t>(hdr.e_phentsize);
  if ((offset + sizeof(Elf32_Phdr)) > m_data.size())
    return nullptr;

  return reinterpret_cast<const Elf32_Phdr*>(&m_data[offset]);
}

u32 ELFFile::GetProgramHeaderCount() const
{
  return GetELFHeader().e_phnum;
}

bool ELFFile::Open(const char* path, Error* error)
{
  auto fp = FileSystem::OpenManagedCFile(path, "rb", error);
  if (!fp)
    return false;

  const s64 size = FileSystem::FSize64(fp.get(), error);
  if (size < 0)
    return false;
  if (size >= MAX_ELF_FILE_SIZE)
  {
    Error::SetStringView(error, "File is too large.");
    return false;
  }

  DataArray data(static_cast<size_t>(size));
  if (std::fread(data.data(), data.size(), 1, fp.get()) != 1)
  {
    Error::SetErrno(error, "fread() failed: ", errno);
    return false;
  }

  return Open(std::move(data), error);
}

bool ELFFile::Open(DataArray data, Error* error)
{
  m_data = std::move(data);

  if (m_data.size() < sizeof(Elf32_Ehdr) ||
      std::memcmp(m_data.data(), EXPECTED_ELF_HEADER, sizeof(EXPECTED_ELF_HEADER)) != 0)
  {
    Error::SetStringView(error, "Invalid header.");
    return false;
  }

  const Elf32_Ehdr& hdr = GetELFHeader();
  if (hdr.e_machine != EM_MIPS)
  {
    Error::SetStringFmt(error, "Unsupported machine type {}.", hdr.e_machine);
    return false;
  }

  const u32 section_count = GetSectionCount();
  const u32 proghdr_count = GetProgramHeaderCount();

  DEV_LOG("ELF Sections={} ProgramHeaders={} Entry=0x{:08X}", section_count, proghdr_count, hdr.e_entry);

  for (u32 i = 0; i < section_count; i++)
  {
    const Elf32_Shdr* shdr = GetSectionHeader(i);
    if (!shdr)
      continue;

    DEV_LOG("Section {}: Name={} Size={}", i, GetSectionName(*shdr), shdr->sh_size);
  }

  for (u32 i = 0; i < proghdr_count; i++)
  {
    const Elf32_Phdr* phdr = GetProgramHeader(i);
    if (!phdr || phdr->p_type != PT_LOAD)
      continue;

    DEV_LOG("Program Header {}: Load {} at 0x{:08X}", i, phdr->p_filesz, phdr->p_vaddr);
  }

  return true;
}

bool ELFFile::LoadExecutableSections(const LoadExecutableSectionCallback& callback, Error* error) const
{
  const u32 entry = GetELFHeader().e_entry;
  bool loaded_entry = false;

  const u32 ph_count = GetProgramHeaderCount();
  for (u32 i = 0; i < ph_count; i++)
  {
    const Elf32_Phdr* phdr = GetProgramHeader(i);
    if (!phdr)
    {
      Error::SetStringFmt(error, "Failed to find program header {}", i);
      return false;
    }

    if (phdr->p_type != PT_LOAD)
    {
      // ignore section
      continue;
    }

    std::span<const u8> data;
    if (phdr->p_filesz > 0)
    {
      if ((phdr->p_offset + static_cast<size_t>(phdr->p_filesz)) > m_data.size())
      {
        Error::SetStringFmt(error, "Program header {} is out of file range {} {} {}", i, phdr->p_offset, phdr->p_filesz,
                            m_data.size());
        return false;
      }

      data = m_data.cspan(phdr->p_offset, phdr->p_filesz);
    }

    if (!callback(data, phdr->p_vaddr, std::max(phdr->p_memsz, phdr->p_filesz), error))
      return false;

    loaded_entry |= (entry >= phdr->p_vaddr && entry < (phdr->p_vaddr + phdr->p_memsz));
  }

  if (!loaded_entry)
  {
    Error::SetStringFmt(error, "Entry point 0x{:08X} not loaded.", entry);
    return false;
  }

  return true;
}

bool ELFFile::IsValidElfHeader(const std::span<const u8> data, Error* error /*= nullptr*/)
{
  if (data.size() < sizeof(Elf32_Ehdr))
  {
    Error::SetStringView(error, "Invalid header.");
    return false;
  }

  return IsValidElfHeader(reinterpret_cast<const Elf32_Ehdr&>(*data.data()), error);
}

bool ELFFile::IsValidElfHeader(const Elf32_Ehdr& header, Error* error /* = nullptr */)
{
  if (std::memcmp(header.e_ident, EXPECTED_ELF_HEADER, sizeof(EXPECTED_ELF_HEADER)) != 0)
  {
    Error::SetStringView(error, "Invalid header.");
    return false;
  }

  if (header.e_machine != EM_MIPS)
  {
    Error::SetStringFmt(error, "Unsupported machine type {}.", header.e_machine);
    return false;
  }

  // probably fine
  return true;
}
