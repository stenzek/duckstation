// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class CDImage;

class Error;
class ProgressCallback;

class IsoReader
{
public:
  enum : u32
  {
    SECTOR_SIZE = 2048
  };

#pragma pack(push, 1)

  struct ISOVolumeDescriptorHeader
  {
    u8 type_code;
    char standard_identifier[5];
    u8 version;
  };
  static_assert(sizeof(ISOVolumeDescriptorHeader) == 7);

  struct ISOBootRecord
  {
    ISOVolumeDescriptorHeader header;
    char boot_system_identifier[32];
    char boot_identifier[32];
    u8 data[1977];
  };
  static_assert(sizeof(ISOBootRecord) == 2048);

  struct ISOPVDDateTime
  {
    char year[4];
    char month[2];
    char day[2];
    char hour[2];
    char minute[2];
    char second[2];
    char milliseconds[2];
    s8 gmt_offset;
  };
  static_assert(sizeof(ISOPVDDateTime) == 17);

  struct ISOPrimaryVolumeDescriptor
  {
    ISOVolumeDescriptorHeader header;
    u8 unused;
    char system_identifier[32];
    char volume_identifier[32];
    char unused2[8];
    u32 total_sectors_le;
    u32 total_sectors_be;
    char unused3[32];
    u16 volume_set_size_le;
    u16 volume_set_size_be;
    u16 volume_sequence_number_le;
    u16 volume_sequence_number_be;
    u16 block_size_le;
    u16 block_size_be;
    u32 path_table_size_le;
    u32 path_table_size_be;
    u32 path_table_location_le;
    u32 optional_path_table_location_le;
    u32 path_table_location_be;
    u32 optional_path_table_location_be;
    u8 root_directory_entry[34];
    char volume_set_identifier[128];
    char publisher_identifier[128];
    char data_preparer_identifier[128];
    char application_identifier[128];
    char copyright_file_identifier[38];
    char abstract_file_identifier[36];
    char bibliographic_file_identifier[37];
    ISOPVDDateTime volume_creation_time;
    ISOPVDDateTime volume_modification_time;
    ISOPVDDateTime volume_expiration_time;
    ISOPVDDateTime volume_effective_time;
    u8 structure_version;
    u8 unused4;
    u8 application_used[512];
    u8 reserved[653];
  };
  static_assert(sizeof(ISOPrimaryVolumeDescriptor) == 2048);

  struct ISODirectoryEntryDateTime
  {
    u8 years_since_1900;
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
    s8 gmt_offset;

    std::string GetFormattedTime() const;
  };

  enum ISODirectoryEntryFlags : u8
  {
    ISODirectoryEntryFlag_Hidden = (1 << 0),
    ISODirectoryEntryFlag_Directory = (1 << 1),
    ISODirectoryEntryFlag_AssociatedFile = (1 << 2),
    ISODirectoryEntryFlag_ExtendedAttributePresent = (1 << 3),
    ISODirectoryEntryFlag_OwnerGroupPermissions = (1 << 4),
    ISODirectoryEntryFlag_MoreExtents = (1 << 7),
  };

  struct ISODirectoryEntry
  {
    u8 entry_length;
    u8 extended_attribute_length;
    u32 location_le;
    u32 location_be;
    u32 length_le;
    u32 length_be;
    ISODirectoryEntryDateTime recoding_time;
    ISODirectoryEntryFlags flags;
    u8 interleaved_unit_size;
    u8 interleaved_gap_size;
    u16 sequence_le;
    u16 sequence_be;
    u8 filename_length;

    ALWAYS_INLINE bool IsDirectory() const { return (flags & ISODirectoryEntryFlag_Directory); }
    ALWAYS_INLINE u32 GetSizeInSectors() const { return (length_le + (SECTOR_SIZE - 1)) / SECTOR_SIZE; }
  };

#pragma pack(pop)

  enum class ReadMode : u8
  {
    Data,
    Mode2,
    Raw,
  };

  IsoReader();
  ~IsoReader();

  static std::string_view RemoveVersionIdentifierFromPath(std::string_view path);

  static u32 GetReadModeSectorSize(ReadMode mode);
  static std::span<const u8> ExtractSectorData(std::span<const u8> raw_sector, ReadMode mode, Error* error);

  ALWAYS_INLINE const CDImage* GetImage() const { return m_image; }
  ALWAYS_INLINE u32 GetTrackNumber() const { return m_track_number; }
  ALWAYS_INLINE u32 GetPVDLBA() const { return m_pvd_lba; }
  ALWAYS_INLINE const ISOPrimaryVolumeDescriptor& GetPVD() const { return m_pvd; }

  bool Open(CDImage* image, u32 track_number, Error* error = nullptr);

  std::vector<std::string> GetFilesInDirectory(std::string_view path, Error* error = nullptr);
  std::vector<std::pair<std::string, ISODirectoryEntry>> GetEntriesInDirectory(std::string_view path,
                                                                               Error* error = nullptr);

  std::optional<ISODirectoryEntry> LocateFile(std::string_view path, Error* error);

  bool FileExists(std::string_view path, Error* error = nullptr);
  bool DirectoryExists(std::string_view path, Error* error = nullptr);
  bool ReadFile(std::string_view path, std::vector<u8>* data, ReadMode read_mode, Error* error = nullptr);
  bool ReadFile(const ISODirectoryEntry& de, std::vector<u8>* data, ReadMode read_mode, Error* error = nullptr);

  bool WriteFileToStream(std::string_view path, std::FILE* fp, ReadMode read_mode, Error* error = nullptr,
                         ProgressCallback* progress = nullptr);
  bool WriteFileToStream(const ISODirectoryEntry& de, std::FILE* fp, ReadMode read_mode, Error* error = nullptr,
                         ProgressCallback* progress = nullptr);

private:
  static std::string_view GetDirectoryEntryFileName(std::span<const u8, SECTOR_SIZE> sector, u32 de_sector_offset);

  bool ReadSector(std::span<u8, SECTOR_SIZE> buf, u32 lsn, Error* error);
  bool ReadPVD(Error* error);

  std::optional<ISODirectoryEntry> LocateFile(std::string_view path, std::span<u8, SECTOR_SIZE> sector_buffer,
                                              u32 directory_record_lba, u32 directory_record_size, Error* error);

  CDImage* m_image;
  u32 m_track_number;

  ISOPrimaryVolumeDescriptor m_pvd = {};
  u32 m_pvd_lba = 0;
};
