#include "cd_subchannel_replacement.h"
#include "file_system.h"
#include "log.h"
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
      Log_ErrorPrintf("Invalid type 0x%02X in '%s'", entry.type, path);
      return false;
    }

    const u32 lba = MSFToLBA(entry.minute_bcd, entry.second_bcd, entry.frame_bcd);

    CDImage::SubChannelQ subq;
    std::copy_n(entry.data, countof(entry.data), subq.data.data());

    // generate an invalid crc by flipping all bits from the valid crc (will never collide)
    const u16 crc = subq.ComputeCRC(subq.data) ^ 0xFFFF;
    subq.data[10] = Truncate8(crc);
    subq.data[11] = Truncate8(crc >> 8);

    m_replacement_subq.emplace(lba, subq);
  }

  Log_InfoPrintf("Loaded %zu replacement sectors from '%s'", m_replacement_subq.size(), path);
  return true;
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
