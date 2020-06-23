#include "cd_subchannel_replacement.h"
#include "log.h"
#include "file_system.h"
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

bool CDSubChannelReplacement::LoadSBI(const char* path)
{
  auto fp = FileSystem::OpenManagedCFile(path, "rb");
  if (!fp)
    return false;

  char header[4];
  if (std::fread(header, sizeof(header), 1, fp.get()) != 1)
  {
    Log_ErrorPrintf("Failed to read header for '%s'", path);
    return true;
  }

  static constexpr char expected_header[] = {'S', 'B', 'I', '\0'};
  if (std::memcmp(header, expected_header, sizeof(header)) != 0)
  {
    Log_ErrorPrintf("Invalid header in '%s'", path);
    return true;
  }

  SBIFileEntry entry;
  while (std::fread(&entry, sizeof(entry), 1, fp.get()) == 1)
  {
    if (!IsValidPackedBCD(entry.minute_bcd) || !IsValidPackedBCD(entry.second_bcd) ||
        !IsValidPackedBCD(entry.frame_bcd))
    {
      Log_ErrorPrintf("Invalid position [%02x:%02x:%02x] in '%s'", entry.minute_bcd, entry.second_bcd, entry.frame_bcd,
                      path);
      return false;
    }

    if (entry.type != 1)
    {
      Log_ErrorPrintf("Invalid type 0x%02X in '%s'", path);
      return false;
    }

    const u32 lba = MSFToLBA(entry.minute_bcd, entry.second_bcd, entry.frame_bcd);

    ReplacementData subq_data;
    std::copy_n(entry.data, countof(entry.data), subq_data.data());

    // generate an invalid crc by flipping all bits from the valid crc (will never collide)
    const u16 crc = CDImage::SubChannelQ::ComputeCRC(subq_data) ^ 0xFFFF;
    subq_data[10] = Truncate8(crc);
    subq_data[11] = Truncate8(crc >> 8);

    m_replacement_subq.emplace(lba, subq_data);
  }

  Log_InfoPrintf("Loaded %zu replacement sectors from '%s'", m_replacement_subq.size(), path);
  return true;
}

bool CDSubChannelReplacement::GetReplacementSubChannelQ(u8 minute_bcd, u8 second_bcd, u8 frame_bcd,
                                                        ReplacementData& subq_data) const
{
  return GetReplacementSubChannelQ(MSFToLBA(minute_bcd, second_bcd, frame_bcd), subq_data);
}

bool CDSubChannelReplacement::GetReplacementSubChannelQ(u32 lba, ReplacementData& subq_data) const
{
  const auto iter = m_replacement_subq.find(lba);
  if (iter == m_replacement_subq.cend())
    return false;

  subq_data = iter->second;
  return true;
}
