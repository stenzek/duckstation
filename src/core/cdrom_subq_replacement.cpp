// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
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

std::unique_ptr<CDROMSubQReplacement> CDROMSubQReplacement::LoadSBI(const std::string& path, std::FILE* fp,
                                                                    Error* error)
{
  static constexpr char expected_header[] = {'S', 'B', 'I', '\0'};

  char header[4];
  if (std::fread(header, sizeof(header), 1, fp) != 1 || std::memcmp(header, expected_header, sizeof(header)) != 0)
  {
    Error::SetStringFmt(error, "Invalid header in '{}'", Path::GetFileName(path));
    return {};
  }

  std::unique_ptr<CDROMSubQReplacement> ret = std::make_unique<CDROMSubQReplacement>();

  SBIFileEntry entry;
  while (std::fread(&entry, sizeof(entry), 1, fp) == 1)
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

std::unique_ptr<CDROMSubQReplacement> CDROMSubQReplacement::LoadLSD(const std::string& path, std::FILE* fp,
                                                                    Error* error)
{
  std::unique_ptr<CDROMSubQReplacement> ret = std::make_unique<CDROMSubQReplacement>();

  LSDFileEntry entry;
  while (std::fread(&entry, sizeof(entry), 1, fp) == 1)
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
                                        std::string_view serial, std::string_view title, std::string_view save_title,
                                        Error* error)
{
  struct FileLoader
  {
    const char* extension;
    std::unique_ptr<CDROMSubQReplacement> (*func)(const std::string&, std::FILE* fp, Error*);
  };
  static constexpr const FileLoader loaders[] = {
    {"sbi", &CDROMSubQReplacement::LoadSBI},
    {"lsd", &CDROMSubQReplacement::LoadLSD},
  };

  const std::string& image_path = image->GetPath();
  std::string display_name;
  std::string path;
  bool result = true;

  const auto try_path = [&path, &ret, &error, &result](const FileLoader& loader) -> bool {
    if (const FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(path.c_str(), "rb"))
    {
      *ret = loader.func(path, fp.get(), error);
      result = static_cast<bool>(*ret);
      if (!result)
        Error::AddPrefixFmt(error, "Failed to load subchannel data from {}: ", Path::GetFileName(path));
      return true;
    }

    return false;
  };

  const auto search_in_subchannels = [&path, &try_path](std::string_view base_name) -> bool {
    if (base_name.empty())
      return false;

    for (const FileLoader& loader : loaders)
    {
      path = Path::Combine(EmuFolders::Subchannels, TinyString::from_format("{}.{}", base_name, loader.extension));
      if (try_path(loader))
        return true;
    }

    return false;
  };

  // Try sbi/lsd in the directory first.
  if (!CDImage::IsDeviceName(image_path.c_str()))
  {
    display_name = FileSystem::GetDisplayNameFromPath(image_path);

    for (const FileLoader& loader : loaders)
    {
      path = Path::BuildRelativePath(
        image_path, SmallString::from_format("{}.{}", Path::GetFileTitle(display_name), loader.extension));
      if (try_path(loader))
        return result;
    }

    // For subimages, we need to check the suffix too.
    if (image->HasSubImages())
    {
      for (const FileLoader& loader : loaders)
      {
        path = Path::BuildRelativePath(image_path,
                                       SmallString::from_format("{}_{}.{}", Path::GetFileTitle(display_name),
                                                                image->GetCurrentSubImage() + 1, loader.extension));
        if (try_path(loader))
          return result;
      }
    }

    // Try the file title first inside subchannels (most specific).
    if (!display_name.empty() && search_in_subchannels(Path::GetFileTitle(display_name)))
      return result;
  }

  // If this fails, try the subchannel directory with serial/title.
  if (search_in_subchannels(serial) || search_in_subchannels(title))
    return result;

  // Try save title next if it's different from title.
  if (!save_title.empty() && save_title != title)
  {
    if (search_in_subchannels(save_title))
      return result;
  }

  // Nothing.
  return true;
}

const CDImage::SubChannelQ* CDROMSubQReplacement::GetReplacementSubQ(u32 lba) const
{
  const auto iter = m_replacement_subq.find(lba);
  return (iter != m_replacement_subq.end()) ? &iter->second : nullptr;
}
