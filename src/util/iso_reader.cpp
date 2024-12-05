// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "iso_reader.h"
#include "cd_image.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/progress_callback.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <cctype>

IsoReader::IsoReader() = default;

IsoReader::~IsoReader() = default;

std::string_view IsoReader::RemoveVersionIdentifierFromPath(std::string_view path)
{
  const std::string_view::size_type pos = path.find(';');
  return (pos != std::string_view::npos) ? path.substr(0, pos) : path;
}

bool IsoReader::Open(CDImage* image, u32 track_number, Error* error)
{
  m_image = image;
  m_track_number = track_number;

  if (image->GetTrackMode(static_cast<u8>(track_number)) == CDImage::TrackMode::Audio)
  {
    Error::SetStringFmt(error, "Track {} is an audio track.", track_number);
    return false;
  }

  if (!ReadPVD(error))
    return false;

  return true;
}

bool IsoReader::ReadSector(u8* buf, u32 lsn, Error* error)
{
  if (!m_image->Seek(m_track_number, lsn))
  {
    Error::SetString(error, fmt::format("Failed to seek to LSN #{}", lsn));
    return false;
  }

  if (m_image->Read(CDImage::ReadMode::DataOnly, 1, buf) != 1)
  {
    Error::SetString(error, fmt::format("Failed to read LSN #{}", lsn));
    return false;
  }

  return true;
}

bool IsoReader::ReadPVD(Error* error)
{
  // volume descriptor start at sector 16
  static constexpr u32 START_SECTOR = 16;

  // try only a maximum of 256 volume descriptors
  for (u32 i = 0; i < 256; i++)
  {
    u8 buffer[SECTOR_SIZE];
    if (!ReadSector(buffer, START_SECTOR + i, error))
      return false;

    const ISOVolumeDescriptorHeader* header = reinterpret_cast<ISOVolumeDescriptorHeader*>(buffer);
    if (std::memcmp(header->standard_identifier, "CD001", 5) != 0)
      continue;
    else if (header->type_code != 1)
      continue;
    else if (header->type_code == 255)
      break;

    m_pvd_lba = START_SECTOR + i;
    std::memcpy(&m_pvd, buffer, sizeof(ISOPrimaryVolumeDescriptor));
    return true;
  }

  Error::SetString(error, "Failed to find the Primary Volume Descriptor.");
  return false;
}

std::optional<IsoReader::ISODirectoryEntry> IsoReader::LocateFile(std::string_view path, Error* error)
{
  const ISODirectoryEntry* root_de = reinterpret_cast<const ISODirectoryEntry*>(m_pvd.root_directory_entry);
  if (path.empty() || path == "/" || path == "\\")
  {
    // locating the root directory
    return *root_de;
  }

  // start at the root directory
  u8 sector_buffer[SECTOR_SIZE];
  return LocateFile(path, sector_buffer, root_de->location_le, root_de->length_le, error);
}

std::string_view IsoReader::GetDirectoryEntryFileName(const u8* sector, u32 de_sector_offset)
{
  const ISODirectoryEntry* de = reinterpret_cast<const ISODirectoryEntry*>(sector + de_sector_offset);
  if ((sizeof(ISODirectoryEntry) + de->filename_length) > de->entry_length ||
      (sizeof(ISODirectoryEntry) + de->filename_length + de_sector_offset) > SECTOR_SIZE)
  {
    return std::string_view();
  }

  const char* str = reinterpret_cast<const char*>(sector + de_sector_offset + sizeof(ISODirectoryEntry));
  if (de->filename_length == 1)
  {
    if (str[0] == '\0')
      return ".";
    else if (str[0] == '\1')
      return "..";
  }

  // Strip any version information like the PS2 BIOS does.
  u32 length_without_version = 0;
  for (; length_without_version < de->filename_length; length_without_version++)
  {
    if (str[length_without_version] == ';' || str[length_without_version] == '\0')
      break;
  }

  return std::string_view(str, length_without_version);
}

std::optional<IsoReader::ISODirectoryEntry> IsoReader::LocateFile(std::string_view path, u8* sector_buffer,
                                                                  u32 directory_record_lba, u32 directory_record_size,
                                                                  Error* error)
{
  if (directory_record_size == 0)
  {
    Error::SetString(error, fmt::format("Directory entry record size 0 while looking for '{}'", path));
    return std::nullopt;
  }

  // strip any leading slashes
  size_t path_component_start = 0;
  while (path_component_start < path.length() &&
         (path[path_component_start] == '/' || path[path_component_start] == '\\'))
  {
    path_component_start++;
  }

  size_t path_component_length = 0;
  while ((path_component_start + path_component_length) < path.length() &&
         path[path_component_start + path_component_length] != '/' &&
         path[path_component_start + path_component_length] != '\\')
  {
    path_component_length++;
  }

  const std::string_view path_component = path.substr(path_component_start, path_component_length);
  if (path_component.empty())
  {
    Error::SetString(error, fmt::format("Empty path component in {}", path));
    return std::nullopt;
  }

  // start reading directory entries
  const u32 num_sectors = (directory_record_size + (SECTOR_SIZE - 1)) / SECTOR_SIZE;
  for (u32 i = 0; i < num_sectors; i++)
  {
    if (!ReadSector(sector_buffer, directory_record_lba + i, error))
      return std::nullopt;

    u32 sector_offset = 0;
    while ((sector_offset + sizeof(ISODirectoryEntry)) < SECTOR_SIZE)
    {
      const ISODirectoryEntry* de = reinterpret_cast<const ISODirectoryEntry*>(&sector_buffer[sector_offset]);
      if (de->entry_length < sizeof(ISODirectoryEntry))
        break;

      const std::string_view de_filename = GetDirectoryEntryFileName(sector_buffer, sector_offset);
      sector_offset += de->entry_length;

      // Empty file would be pretty strange..
      if (de_filename.empty() || de_filename == "." || de_filename == "..")
        continue;

      if (de_filename.length() != path_component.length() ||
          StringUtil::Strncasecmp(de_filename.data(), path_component.data(), path_component.length()) != 0)
      {
        continue;
      }

      // found it. is this the file we're looking for?
      if ((path_component_start + path_component_length) == path.length())
        return *de;

      // if it is a directory, recurse into it
      if (de->flags & ISODirectoryEntryFlag_Directory)
      {
        return LocateFile(path.substr(path_component_start + path_component_length), sector_buffer, de->location_le,
                          de->length_le, error);
      }

      // we're looking for a directory but got a file
      Error::SetString(error, fmt::format("Looking for directory '{}' but got file", path_component));
      return std::nullopt;
    }
  }

  Error::SetString(error, fmt::format("Path component '{}' not found", path_component));
  return std::nullopt;
}

std::vector<std::string> IsoReader::GetFilesInDirectory(std::string_view path, Error* error)
{
  std::string base_path(path);
  u32 directory_record_lsn;
  u32 directory_record_length;
  if (base_path.empty())
  {
    // root directory
    const ISODirectoryEntry* root_de = reinterpret_cast<const ISODirectoryEntry*>(m_pvd.root_directory_entry);
    directory_record_lsn = root_de->location_le;
    directory_record_length = root_de->length_le;
  }
  else
  {
    auto directory_de = LocateFile(base_path, error);
    if (!directory_de.has_value())
      return {};

    if ((directory_de->flags & ISODirectoryEntryFlag_Directory) == 0)
    {
      Error::SetString(error, fmt::format("Path '{}' is not a directory, can't list", path));
      return {};
    }

    directory_record_lsn = directory_de->location_le;
    directory_record_length = directory_de->length_le;

    if (base_path[base_path.size() - 1] != '/')
      base_path += '/';
  }

  // start reading directory entries
  const u32 num_sectors = (directory_record_length + (SECTOR_SIZE - 1)) / SECTOR_SIZE;
  std::vector<std::string> files;
  u8 sector_buffer[SECTOR_SIZE];
  for (u32 i = 0; i < num_sectors; i++)
  {
    if (!ReadSector(sector_buffer, directory_record_lsn + i, error))
      break;

    u32 sector_offset = 0;
    while ((sector_offset + sizeof(ISODirectoryEntry)) < SECTOR_SIZE)
    {
      const ISODirectoryEntry* de = reinterpret_cast<const ISODirectoryEntry*>(&sector_buffer[sector_offset]);
      if (de->entry_length < sizeof(ISODirectoryEntry))
        break;

      const std::string_view de_filename = GetDirectoryEntryFileName(sector_buffer, sector_offset);
      sector_offset += de->entry_length;

      // Empty file would be pretty strange..
      if (de_filename.empty() || de_filename == "." || de_filename == "..")
        continue;

      files.push_back(fmt::format("{}{}", base_path, de_filename));
    }
  }

  return files;
}

std::vector<std::pair<std::string, IsoReader::ISODirectoryEntry>>
IsoReader::GetEntriesInDirectory(std::string_view path, Error* error /*= nullptr*/)
{
  std::string base_path(path);
  u32 directory_record_lsn;
  u32 directory_record_length;
  if (base_path.empty())
  {
    // root directory
    const ISODirectoryEntry* root_de = reinterpret_cast<const ISODirectoryEntry*>(m_pvd.root_directory_entry);
    directory_record_lsn = root_de->location_le;
    directory_record_length = root_de->length_le;
  }
  else
  {
    auto directory_de = LocateFile(base_path, error);
    if (!directory_de.has_value())
      return {};

    if ((directory_de->flags & ISODirectoryEntryFlag_Directory) == 0)
    {
      Error::SetString(error, fmt::format("Path '{}' is not a directory, can't list", path));
      return {};
    }

    directory_record_lsn = directory_de->location_le;
    directory_record_length = directory_de->length_le;

    if (base_path[base_path.size() - 1] != '/')
      base_path += '/';
  }

  // start reading directory entries
  const u32 num_sectors = (directory_record_length + (SECTOR_SIZE - 1)) / SECTOR_SIZE;
  std::vector<std::pair<std::string, IsoReader::ISODirectoryEntry>> files;
  u8 sector_buffer[SECTOR_SIZE];
  for (u32 i = 0; i < num_sectors; i++)
  {
    if (!ReadSector(sector_buffer, directory_record_lsn + i, error))
      break;

    u32 sector_offset = 0;
    while ((sector_offset + sizeof(ISODirectoryEntry)) < SECTOR_SIZE)
    {
      const ISODirectoryEntry* de = reinterpret_cast<const ISODirectoryEntry*>(&sector_buffer[sector_offset]);
      if (de->entry_length < sizeof(ISODirectoryEntry))
        break;

      const std::string_view de_filename = GetDirectoryEntryFileName(sector_buffer, sector_offset);
      sector_offset += de->entry_length;

      // Empty file would be pretty strange..
      if (de_filename.empty() || de_filename == "." || de_filename == "..")
        continue;

      files.emplace_back(fmt::format("{}{}", base_path, de_filename), *de);
    }
  }

  return files;
}

bool IsoReader::FileExists(std::string_view path, Error* error)
{
  auto de = LocateFile(path, error);
  if (!de)
    return false;

  return (de->flags & ISODirectoryEntryFlag_Directory) == 0;
}

bool IsoReader::DirectoryExists(std::string_view path, Error* error)
{
  auto de = LocateFile(path, error);
  if (!de)
    return false;

  return (de->flags & ISODirectoryEntryFlag_Directory) == ISODirectoryEntryFlag_Directory;
}

bool IsoReader::ReadFile(std::string_view path, std::vector<u8>* data, Error* error)
{
  auto de = LocateFile(path, error);
  if (!de)
    return false;

  return ReadFile(de.value(), data, error);
}

bool IsoReader::ReadFile(const ISODirectoryEntry& de, std::vector<u8>* data, Error* error /*= nullptr*/)
{
  if (de.flags & ISODirectoryEntryFlag_Directory)
  {
    Error::SetString(error, "File is a directory");
    return false;
  }

  if (de.length_le == 0)
  {
    data->clear();
    return true;
  }

  const u32 num_sectors = (de.length_le + (SECTOR_SIZE - 1)) / SECTOR_SIZE;
  data->resize(num_sectors * static_cast<size_t>(SECTOR_SIZE));
  for (u32 i = 0, lsn = de.location_le; i < num_sectors; i++, lsn++)
  {
    if (!ReadSector(data->data() + (i * SECTOR_SIZE), lsn, error))
      return false;
  }

  // Might not be sector aligned, so reduce it back.
  data->resize(de.length_le);
  return true;
}

bool IsoReader::WriteFileToStream(std::string_view path, std::FILE* fp, Error* error /* = nullptr */,
                                  ProgressCallback* progress /* = nullptr */)
{
  auto de = LocateFile(path, error);
  if (!de)
    return false;

  return WriteFileToStream(de.value(), fp, error, progress);
}

bool IsoReader::WriteFileToStream(const ISODirectoryEntry& de, std::FILE* fp, Error* error /* = nullptr */,
                                  ProgressCallback* progress /* = nullptr */)
{
  if (de.flags & ISODirectoryEntryFlag_Directory)
  {
    Error::SetString(error, "File is a directory");
    return false;
  }

  if (!FileSystem::FSeek64(fp, 0, SEEK_SET, error))
    return false;

  if (de.length_le == 0)
    return FileSystem::FTruncate64(fp, 0, error);

  const u32 num_sectors = (de.length_le + (SECTOR_SIZE - 1)) / SECTOR_SIZE;
  u32 file_pos = 0;
  u8 sector_buffer[SECTOR_SIZE];

  if (progress)
  {
    progress->SetProgressRange(de.length_le);
    progress->SetProgressValue(0);
  }

  for (u32 i = 0, lsn = de.location_le; i < num_sectors; i++, lsn++)
  {
    if (!ReadSector(sector_buffer, lsn, error))
      return false;

    const u32 write_size = std::min<u32>(de.length_le - file_pos, SECTOR_SIZE);
    if (std::fwrite(sector_buffer, write_size, 1, fp) != 1)
    {
      Error::SetErrno(error, "fwrite() failed: ", errno);
      return false;
    }

    file_pos += write_size;
    if (progress)
    {
      progress->SetProgressValue(file_pos);
      if (progress->IsCancelled())
      {
        Error::SetStringView(error, "Operation was cancelled.");
        return false;
      }
    }
  }

  if (std::fflush(fp) != 0)
  {
    Error::SetErrno(error, "fflush() failed: ", errno);
    return false;
  }

  return true;
}

std::string IsoReader::ISODirectoryEntryDateTime::GetFormattedTime() const
{
  // need to apply the UTC offset, so first convert to unix time
  struct tm utime;
  utime.tm_year = years_since_1900;
  utime.tm_mon = (month > 0) ? (month - 1) : 0;
  utime.tm_mday = (day > 0) ? (day - 1) : 0;
  utime.tm_hour = hour;
  utime.tm_min = minute;
  utime.tm_sec = second;

  const s32 uts_offset = static_cast<s32>(gmt_offset) * 3600;
  const time_t uts = std::mktime(&utime) + uts_offset;

  struct tm ltime;
#ifdef _MSC_VER
  localtime_s(&ltime, &uts);
#else
  localtime_r(&uts, &ltime);
#endif

  char buf[128];
  const size_t len = std::strftime(buf, std::size(buf), "%c", &ltime);
  return std::string(buf, len);
}
