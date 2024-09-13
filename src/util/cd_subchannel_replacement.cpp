// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cd_subchannel_replacement.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include <algorithm>
#include <memory>
Log_SetChannel(CDSubChannelReplacement);

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

CDSubChannelReplacement::CDSubChannelReplacement() = default;

CDSubChannelReplacement::~CDSubChannelReplacement() = default;

static constexpr u32 MSFToLBA(u8 minute_bcd, u8 second_bcd, u8 frame_bcd)
{
  const u8 minute = PackedBCDToBinary(minute_bcd);
  const u8 second = PackedBCDToBinary(second_bcd);
  const u8 frame = PackedBCDToBinary(frame_bcd);

  return (ZeroExtend32(minute) * 60 * 75) + (ZeroExtend32(second) * 75) + ZeroExtend32(frame);
}

bool CDSubChannelReplacement::LoadSBI(const std::string& path)
{
  auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
  if (!fp)
    return false;

  char header[4];
  if (std::fread(header, sizeof(header), 1, fp.get()) != 1)
  {
    ERROR_LOG("Failed to read header for '{}'", path);
    return true;
  }

  static constexpr char expected_header[] = {'S', 'B', 'I', '\0'};
  if (std::memcmp(header, expected_header, sizeof(header)) != 0)
  {
    ERROR_LOG("Invalid header in '{}'", path);
    return true;
  }

  m_replacement_subq.clear();

  SBIFileEntry entry;
  while (std::fread(&entry, sizeof(entry), 1, fp.get()) == 1)
  {
    if (!IsValidPackedBCD(entry.minute_bcd) || !IsValidPackedBCD(entry.second_bcd) ||
        !IsValidPackedBCD(entry.frame_bcd))
    {
      ERROR_LOG("Invalid position [{:02x}:{:02x}:{:02x}] in '{}'", entry.minute_bcd, entry.second_bcd, entry.frame_bcd,
                path);
      return false;
    }

    if (entry.type != 1)
    {
      ERROR_LOG("Invalid type 0x{:02X} in '{}'", entry.type, path);
      return false;
    }

    const u32 lba = MSFToLBA(entry.minute_bcd, entry.second_bcd, entry.frame_bcd);

    CDImage::SubChannelQ subq;
    std::memcpy(subq.data.data(), entry.data, sizeof(entry.data));

    // generate an invalid crc by flipping all bits from the valid crc (will never collide)
    const u16 crc = subq.ComputeCRC(subq.data) ^ 0xFFFF;
    subq.data[10] = Truncate8(crc);
    subq.data[11] = Truncate8(crc >> 8);

    m_replacement_subq.emplace(lba, subq);
  }

  INFO_LOG("Loaded {} replacement sectors from SBI '{}'", m_replacement_subq.size(), path);
  return true;
}

bool CDSubChannelReplacement::LoadLSD(const std::string& path)
{
  auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
  if (!fp)
    return false;

  m_replacement_subq.clear();

  LSDFileEntry entry;
  while (std::fread(&entry, sizeof(entry), 1, fp.get()) == 1)
  {
    if (!IsValidPackedBCD(entry.minute_bcd) || !IsValidPackedBCD(entry.second_bcd) ||
        !IsValidPackedBCD(entry.frame_bcd))
    {
      ERROR_LOG("Invalid position [{:02x}:{:02x}:{:02x}] in '{}'", entry.minute_bcd, entry.second_bcd, entry.frame_bcd,
                path);
      return false;
    }

    const u32 lba = MSFToLBA(entry.minute_bcd, entry.second_bcd, entry.frame_bcd);

    CDImage::SubChannelQ subq;
    std::memcpy(subq.data.data(), entry.data, sizeof(entry.data));

    DEBUG_LOG("{:02x}:{:02x}:{:02x}: CRC {}", entry.minute_bcd, entry.second_bcd, entry.frame_bcd,
              subq.IsCRCValid() ? "VALID" : "INVALID");
    m_replacement_subq.emplace(lba, subq);
  }

  INFO_LOG("Loaded {} replacement sectors from LSD '{}'", m_replacement_subq.size(), path);
  return true;
}

bool CDSubChannelReplacement::LoadFromImagePath(std::string_view image_path)
{
  if (const std::string filename = Path::ReplaceExtension(image_path, "sbi"); LoadSBI(filename.c_str()))
    return true;

  if (const std::string filename = Path::ReplaceExtension(image_path, "lsd"); LoadLSD(filename.c_str()))
    return true;

  return false;
}

void CDSubChannelReplacement::AddReplacementSubChannelQ(u32 lba, const CDImage::SubChannelQ& subq)
{
  auto iter = m_replacement_subq.find(lba);
  if (iter != m_replacement_subq.end())
    iter->second.data = subq.data;
  else
    m_replacement_subq.emplace(lba, subq);
}

bool CDSubChannelReplacement::GetReplacementSubChannelQ(u8 minute_bcd, u8 second_bcd, u8 frame_bcd,
                                                        CDImage::SubChannelQ* subq) const
{
  return GetReplacementSubChannelQ(MSFToLBA(minute_bcd, second_bcd, frame_bcd), subq);
}

bool CDSubChannelReplacement::GetReplacementSubChannelQ(u32 lba, CDImage::SubChannelQ* subq) const
{
  const auto iter = m_replacement_subq.find(lba);
  if (iter == m_replacement_subq.cend())
    return false;

  *subq = iter->second;
  return true;
}
