// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cdrom_subq_replacement.h"
#include "settings.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"

#include <algorithm>
#include <array>
#include <memory>

LOG_CHANNEL(CDROM);

#pragma pack(push, 1)
struct SBIFileEntry
{
  u8 minute_bcd;
  u8 second_bcd;
  u8 frame_bcd;
  u8 type;
  u8 data[10];
};
struct LSDFileEntry
{
  u8 minute_bcd;
  u8 second_bcd;
  u8 frame_bcd;
  u8 data[12];
};
static_assert(sizeof(LSDFileEntry) == 15);
#pragma pack(pop)

CDROMSubQReplacement::CDROMSubQReplacement() = default;

CDROMSubQReplacement::~CDROMSubQReplacement() = default;

std::unique_ptr<CDROMSubQReplacement> CDROMSubQReplacement::LoadSBI(const std::string& path, Error* error)
{
  auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb", error);
  if (!fp)
    return {};

  static constexpr char expected_header[] = {'S', 'B', 'I', '\0'};

  char header[4];
  if (std::fread(header, sizeof(header), 1, fp.get()) != 1 || std::memcmp(header, expected_header, sizeof(header)) != 0)
  {
    Error::SetStringFmt(error, "Invalid header in '{}'", Path::GetFileName(path));
    return {};
  }

  std::unique_ptr<CDROMSubQReplacement> ret = std::make_unique<CDROMSubQReplacement>();

  SBIFileEntry entry;
  while (std::fread(&entry, sizeof(entry), 1, fp.get()) == 1)
  {
    if (!IsValidPackedBCD(entry.minute_bcd) || !IsValidPackedBCD(entry.second_bcd) ||
        !IsValidPackedBCD(entry.frame_bcd))
    {
      Error::SetStringFmt(error, "Invalid position [{:02x}:{:02x}:{:02x}] in '{}'", entry.minute_bcd, entry.second_bcd,
                          entry.frame_bcd, Path::GetFileName(path));
      return {};
    }

    if (entry.type != 1)
    {
      Error::SetStringFmt(error, "Invalid type 0x{:02X} in '{}'", entry.type, Path::GetFileName(path));
      return {};
    }

    const u32 lba = CDImage::Position::FromBCD(entry.minute_bcd, entry.second_bcd, entry.frame_bcd).ToLBA();

    CDImage::SubChannelQ subq;
    std::memcpy(subq.data.data(), entry.data, sizeof(entry.data));

    // generate an invalid crc by flipping all bits from the valid crc (will never collide)
    const u16 crc = subq.ComputeCRC(subq.data) ^ 0xFFFF;
    subq.data[10] = Truncate8(crc);
    subq.data[11] = Truncate8(crc >> 8);

    ret->m_replacement_subq.emplace(lba, subq);
  }

  INFO_LOG("Loaded {} replacement sectors from SBI '{}'", ret->m_replacement_subq.size(), Path::GetFileName(path));
  return ret;
}

std::unique_ptr<CDROMSubQReplacement> CDROMSubQReplacement::LoadLSD(const std::string& path, Error* error)
{
  auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb", error);
  if (!fp)
    return {};

  std::unique_ptr<CDROMSubQReplacement> ret = std::make_unique<CDROMSubQReplacement>();

  LSDFileEntry entry;
  while (std::fread(&entry, sizeof(entry), 1, fp.get()) == 1)
  {
    if (!IsValidPackedBCD(entry.minute_bcd) || !IsValidPackedBCD(entry.second_bcd) ||
        !IsValidPackedBCD(entry.frame_bcd))
    {
      Error::SetStringFmt(error, "Invalid position [{:02x}:{:02x}:{:02x}] in '{}'", entry.minute_bcd, entry.second_bcd,
                          entry.frame_bcd, Path::GetFileName(path));
      return {};
    }

    const u32 lba = CDImage::Position::FromBCD(entry.minute_bcd, entry.second_bcd, entry.frame_bcd).ToLBA();

    CDImage::SubChannelQ subq;
    std::memcpy(subq.data.data(), entry.data, sizeof(entry.data));

    DEBUG_LOG("{:02x}:{:02x}:{:02x}: CRC {}", entry.minute_bcd, entry.second_bcd, entry.frame_bcd,
              subq.IsCRCValid() ? "VALID" : "INVALID");
    ret->m_replacement_subq.emplace(lba, subq);
  }

  INFO_LOG("Loaded {} replacement sectors from LSD '{}'", ret->m_replacement_subq.size(), path);
  return ret;
}

bool CDROMSubQReplacement::LoadForImage(std::unique_ptr<CDROMSubQReplacement>* ret, CDImage* image,
                                        std::string_view serial, std::string_view title, Error* error)
{
  struct FileLoader
  {
    const char* extension;
    std::unique_ptr<CDROMSubQReplacement> (*func)(const std::string&, Error*);
  };
  static constexpr const FileLoader loaders[] = {
    {"sbi", &CDROMSubQReplacement::LoadSBI},
    {"lsd", &CDROMSubQReplacement::LoadLSD},
  };

  const std::string& image_path = image->GetPath();
  std::string path;

  // Try sbi/lsd in the directory first.
  if (!CDImage::IsDeviceName(image_path.c_str()))
  {
    for (const FileLoader& loader : loaders)
    {
      path = Path::ReplaceExtension(image_path, loader.extension);
      if (FileSystem::FileExists(path.c_str()))
      {
        *ret = loader.func(path, error);
        if (!static_cast<bool>(*ret))
          Error::AddPrefixFmt(error, "Failed to load subchannel data from {}: ", Path::GetFileName(path));

        return static_cast<bool>(*ret);
      }
    }
  }

  // For subimages, we need to check the suffix too.
  if (image->HasSubImages())
  {
    for (const FileLoader& loader : loaders)
    {
      path = Path::BuildRelativePath(image_path,
                                     SmallString::from_format("{}_{}.{}", Path::GetFileName(image_path),
                                                              image->GetCurrentSubImage() + 1, loader.extension));
      if (FileSystem::FileExists(path.c_str()))
      {
        *ret = loader.func(path, error);
        if (!static_cast<bool>(*ret))
          Error::AddPrefixFmt(error, "Failed to load subchannel data from {}: ", Path::GetFileName(path));

        return static_cast<bool>(*ret);
      }
    }
  }

  // If this fails, try the subchannel directory with serial/title.
  if (!serial.empty())
  {
    for (const FileLoader& loader : loaders)
    {
      path = Path::Combine(EmuFolders::Subchannels, TinyString::from_format("{}.{}", serial, loader.extension));
      if (FileSystem::FileExists(path.c_str()))
      {
        *ret = loader.func(path, error);
        if (!static_cast<bool>(*ret))
          Error::AddPrefixFmt(error, "Failed to load subchannel data from {}: ", Path::GetFileName(path));

        return static_cast<bool>(*ret);
      }
    }
  }

  if (!title.empty())
  {
    for (const FileLoader& loader : loaders)
    {
      path = Path::Combine(EmuFolders::Subchannels, TinyString::from_format("{}.{}", title, loader.extension));
      if (FileSystem::FileExists(path.c_str()))
      {
        *ret = loader.func(path, error);
        if (!static_cast<bool>(*ret))
          Error::AddPrefixFmt(error, "Failed to load subchannel data from {}: ", Path::GetFileName(path));

        return static_cast<bool>(*ret);
      }
    }
  }

  // Nothing.
  return true;
}

const CDImage::SubChannelQ* CDROMSubQReplacement::GetReplacementSubQ(u32 lba) const
{
  const auto iter = m_replacement_subq.find(lba);
  return (iter != m_replacement_subq.end()) ? &iter->second : nullptr;
}
