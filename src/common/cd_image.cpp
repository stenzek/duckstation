#include "cd_image.h"
#include "assert.h"
#include "file_system.h"
#include "log.h"
#include "string_util.h"
#include <array>
Log_SetChannel(CDImage);

CDImage::CDImage() = default;

CDImage::~CDImage() = default;

u32 CDImage::GetBytesPerSector(TrackMode mode)
{
  static constexpr std::array<u32, 8> sizes = {{2352, 2048, 2352, 2336, 2048, 2324, 2332, 2352}};
  return sizes[static_cast<u32>(mode)];
}

std::unique_ptr<CDImage> CDImage::Open(const char* filename, Common::Error* error)
{
  const char* extension;

#ifdef __ANDROID__
  std::string filename_display_name(FileSystem::GetDisplayNameFromPath(filename));
  if (filename_display_name.empty())
    filename_display_name = filename;

  extension = std::strrchr(filename_display_name.c_str(), '.');
#else
  extension = std::strrchr(filename, '.');
#endif

  if (!extension)
  {
    Log_ErrorPrintf("Invalid filename: '%s'", filename);
    return nullptr;
  }

  if (StringUtil::Strcasecmp(extension, ".cue") == 0)
  {
    return OpenCueSheetImage(filename, error);
  }
  else if (StringUtil::Strcasecmp(extension, ".bin") == 0 || StringUtil::Strcasecmp(extension, ".img") == 0 ||
           StringUtil::Strcasecmp(extension, ".iso") == 0)
  {
    return OpenBinImage(filename, error);
  }
  else if (StringUtil::Strcasecmp(extension, ".chd") == 0)
  {
    return OpenCHDImage(filename, error);
  }
  else if (StringUtil::Strcasecmp(extension, ".ecm") == 0)
  {
    return OpenEcmImage(filename, error);
  }
  else if (StringUtil::Strcasecmp(extension, ".mds") == 0)
  {
    return OpenMdsImage(filename, error);
  }
  else if (StringUtil::Strcasecmp(extension, ".pbp") == 0)
  {
    return OpenPBPImage(filename, error);
  }
  else if (StringUtil::Strcasecmp(extension, ".m3u") == 0)
  {
    return OpenM3uImage(filename, error);
  }

  if (IsDeviceName(filename))
    return OpenDeviceImage(filename, error);

#undef CASE_COMPARE

  Log_ErrorPrintf("Unknown extension '%s' from filename '%s'", extension, filename);
  return nullptr;
}

CDImage::LBA CDImage::GetTrackStartPosition(u8 track) const
{
  Assert(track > 0 && track <= m_tracks.size());
  return m_tracks[track - 1].start_lba;
}

CDImage::Position CDImage::GetTrackStartMSFPosition(u8 track) const
{
  Assert(track > 0 && track <= m_tracks.size());
  return Position::FromLBA(m_tracks[track - 1].start_lba);
}

CDImage::LBA CDImage::GetTrackLength(u8 track) const
{
  Assert(track > 0 && track <= m_tracks.size());
  return m_tracks[track - 1].length;
}

CDImage::Position CDImage::GetTrackMSFLength(u8 track) const
{
  Assert(track > 0 && track <= m_tracks.size());
  return Position::FromLBA(m_tracks[track - 1].length);
}

CDImage::TrackMode CDImage::GetTrackMode(u8 track) const
{
  Assert(track > 0 && track <= m_tracks.size());
  return m_tracks[track - 1].mode;
}

CDImage::LBA CDImage::GetTrackIndexPosition(u8 track, u8 index) const
{
  for (const Index& current_index : m_indices)
  {
    if (current_index.track_number == track && current_index.index_number == index)
      return current_index.start_lba_on_disc;
  }

  return m_lba_count;
}

CDImage::LBA CDImage::GetTrackIndexLength(u8 track, u8 index) const
{
  for (const Index& current_index : m_indices)
  {
    if (current_index.track_number == track && current_index.index_number == index)
      return current_index.length;
  }

  return 0;
}

const CDImage::CDImage::Track& CDImage::GetTrack(u32 track) const
{
  Assert(track > 0 && track <= m_tracks.size());
  return m_tracks[track - 1];
}

const CDImage::CDImage::Index& CDImage::GetIndex(u32 i) const
{
  return m_indices[i];
}

bool CDImage::Seek(LBA lba)
{
  const Index* new_index;
  if (m_current_index && lba >= m_current_index->start_lba_on_disc &&
      (lba - m_current_index->start_lba_on_disc) < m_current_index->length)
  {
    new_index = m_current_index;
  }
  else
  {
    new_index = GetIndexForDiscPosition(lba);
    if (!new_index)
      return false;
  }

  const LBA new_index_offset = lba - new_index->start_lba_on_disc;
  if (new_index_offset >= new_index->length)
    return false;

  m_current_index = new_index;
  m_position_on_disc = lba;
  m_position_in_index = new_index_offset;
  m_position_in_track = new_index->start_lba_in_track + new_index_offset;
  return true;
}

bool CDImage::Seek(u32 track_number, const Position& pos_in_track)
{
  if (track_number < 1 || track_number > m_tracks.size())
    return false;

  const Track& track = m_tracks[track_number - 1];
  const LBA pos_lba = pos_in_track.ToLBA();
  if (pos_lba >= track.length)
    return false;

  return Seek(track.start_lba + pos_lba);
}

bool CDImage::Seek(const Position& pos)
{
  return Seek(pos.ToLBA());
}

bool CDImage::Seek(u32 track_number, LBA lba)
{
  if (track_number < 1 || track_number > m_tracks.size())
    return false;

  const Track& track = m_tracks[track_number - 1];
  return Seek(track.start_lba + lba);
}

u32 CDImage::Read(ReadMode read_mode, u32 sector_count, void* buffer)
{
  u8* buffer_ptr = static_cast<u8*>(buffer);
  u32 sectors_read = 0;
  for (; sectors_read < sector_count; sectors_read++)
  {
    // get raw sector
    u8 raw_sector[RAW_SECTOR_SIZE];
    if (!ReadRawSector(raw_sector, nullptr))
      break;

    switch (read_mode)
    {
      case ReadMode::DataOnly:
        std::memcpy(buffer_ptr, raw_sector + 24, DATA_SECTOR_SIZE);
        buffer_ptr += DATA_SECTOR_SIZE;
        break;

      case ReadMode::RawNoSync:
        std::memcpy(buffer_ptr, raw_sector + SECTOR_SYNC_SIZE, RAW_SECTOR_SIZE - SECTOR_SYNC_SIZE);
        buffer_ptr += RAW_SECTOR_SIZE - SECTOR_SYNC_SIZE;
        break;

      case ReadMode::RawSector:
        std::memcpy(buffer_ptr, raw_sector, RAW_SECTOR_SIZE);
        buffer_ptr += RAW_SECTOR_SIZE;
        break;

      default:
        UnreachableCode();
        break;
    }
  }

  return sectors_read;
}

bool CDImage::ReadRawSector(void* buffer, SubChannelQ* subq)
{
  if (m_position_in_index == m_current_index->length)
  {
    if (!Seek(m_position_on_disc))
      return false;
  }

  if (buffer)
  {
    if (m_current_index->file_sector_size > 0)
    {
      // TODO: This is where we'd reconstruct the header for other mode tracks.
      if (!ReadSectorFromIndex(buffer, *m_current_index, m_position_in_index))
      {
        Log_ErrorPrintf("Read of LBA %u failed", m_position_on_disc);
        Seek(m_position_on_disc);
        return false;
      }
    }
    else
    {
      if (m_current_index->track_number == LEAD_OUT_TRACK_NUMBER)
      {
        // Lead-out area.
        std::fill(static_cast<u8*>(buffer), static_cast<u8*>(buffer) + RAW_SECTOR_SIZE, u8(0xAA));
      }
      else
      {
        // This in an implicit pregap. Return silence.
        std::fill(static_cast<u8*>(buffer), static_cast<u8*>(buffer) + RAW_SECTOR_SIZE, u8(0));
      }
    }
  }

  if (subq && !ReadSubChannelQ(subq, *m_current_index, m_position_in_index))
  {
    Log_ErrorPrintf("Subchannel read of LBA %u failed", m_position_on_disc);
    Seek(m_position_on_disc);
    return false;
  }

  m_position_on_disc++;
  m_position_in_index++;
  m_position_in_track++;
  return true;
}

bool CDImage::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  GenerateSubChannelQ(subq, index, lba_in_index);
  return true;
}

bool CDImage::HasNonStandardSubchannel() const
{
  return false;
}

std::string CDImage::GetMetadata(const std::string_view& type) const
{
  std::string result;
  if (type == "title")
  {
    const std::string display_name(FileSystem::GetDisplayNameFromPath(m_filename));
    result = FileSystem::StripExtension(display_name);
  }

  return result;
}

bool CDImage::HasSubImages() const
{
  return false;
}

u32 CDImage::GetSubImageCount() const
{
  return 0;
}

u32 CDImage::GetCurrentSubImage() const
{
  return 0;
}

bool CDImage::SwitchSubImage(u32 index, Common::Error* error)
{
  return false;
}

std::string CDImage::GetSubImageMetadata(u32 index, const std::string_view& type) const
{
  return {};
}

void CDImage::ClearTOC()
{
  m_lba_count = 0;
  m_indices.clear();
  m_tracks.clear();
  m_current_index = nullptr;
  m_position_in_index = 0;
  m_position_in_track = 0;
  m_position_on_disc = 0;
}

void CDImage::CopyTOC(const CDImage* image)
{
  m_lba_count = image->m_lba_count;
  m_indices.clear();
  m_indices.reserve(image->m_indices.size());

  // Damn bitfield copy constructor...
  for (const Index& index : image->m_indices)
  {
    Index new_index;
    std::memcpy(&new_index, &index, sizeof(new_index));
    m_indices.push_back(new_index);
  }
  for (const Track& track : image->m_tracks)
  {
    Track new_track;
    std::memcpy(&new_track, &track, sizeof(new_track));
    m_tracks.push_back(new_track);
  }
  m_current_index = nullptr;
  m_position_in_index = 0;
  m_position_in_track = 0;
  m_position_on_disc = 0;
}

const CDImage::Index* CDImage::GetIndexForDiscPosition(LBA pos)
{
  for (const Index& index : m_indices)
  {
    if (pos < index.start_lba_on_disc)
      continue;

    const LBA index_offset = pos - index.start_lba_on_disc;
    if (index_offset >= index.length)
      continue;

    return &index;
  }

  return nullptr;
}

const CDImage::Index* CDImage::GetIndexForTrackPosition(u32 track_number, LBA track_pos)
{
  if (track_number < 1 || track_number > m_tracks.size())
    return nullptr;

  const Track& track = m_tracks[track_number - 1];
  if (track_pos >= track.length)
    return nullptr;

  return GetIndexForDiscPosition(track.start_lba + track_pos);
}

bool CDImage::GenerateSubChannelQ(SubChannelQ* subq, LBA lba)
{
  const Index* index = GetIndexForDiscPosition(lba);
  if (!index)
    return false;

  const u32 index_offset = index->start_lba_on_disc - lba;
  GenerateSubChannelQ(subq, *index, index_offset);
  return true;
}

void CDImage::GenerateSubChannelQ(SubChannelQ* subq, const Index& index, u32 index_offset)
{
  subq->control_bits = index.control.bits;
  subq->track_number_bcd = (index.track_number <= m_tracks.size() ? BinaryToBCD(static_cast<u8>(index.track_number)) :
                                                                    static_cast<u8>(index.track_number));
  subq->index_number_bcd = BinaryToBCD(static_cast<u8>(index.index_number));

  Position relative_position;
  if (index.is_pregap)
  {
    // position should count down to the end of the pregap
    relative_position = Position::FromLBA(index.length - index_offset - 1);
  }
  else
  {
    // count up from the start of the track
    relative_position = Position::FromLBA(index.start_lba_in_track + index_offset);
  }

  std::tie(subq->relative_minute_bcd, subq->relative_second_bcd, subq->relative_frame_bcd) = relative_position.ToBCD();

  subq->reserved = 0;

  const Position absolute_position = Position::FromLBA(index.start_lba_on_disc + index_offset);
  std::tie(subq->absolute_minute_bcd, subq->absolute_second_bcd, subq->absolute_frame_bcd) = absolute_position.ToBCD();
  subq->crc = SubChannelQ::ComputeCRC(subq->data);
}

void CDImage::AddLeadOutIndex()
{
  Assert(!m_indices.empty());
  const Index& last_index = m_indices.back();

  Index index = {};
  index.start_lba_on_disc = last_index.start_lba_on_disc + last_index.length;
  index.length = LEAD_OUT_SECTOR_COUNT;
  index.track_number = LEAD_OUT_TRACK_NUMBER;
  index.index_number = 0;
  index.control.bits = last_index.control.bits;
  m_indices.push_back(index);
}

u16 CDImage::SubChannelQ::ComputeCRC(const Data& data)
{
  static constexpr std::array<u16, 256> crc16_table = {
    {0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7, 0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD,
     0xE1CE, 0xF1EF, 0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6, 0x9339, 0x8318, 0xB37B, 0xA35A,
     0xD3BD, 0xC39C, 0xF3FF, 0xE3DE, 0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485, 0xA56A, 0xB54B,
     0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D, 0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
     0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC, 0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861,
     0x2802, 0x3823, 0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B, 0x5AF5, 0x4AD4, 0x7AB7, 0x6A96,
     0x1A71, 0x0A50, 0x3A33, 0x2A12, 0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A, 0x6CA6, 0x7C87,
     0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41, 0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
     0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70, 0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A,
     0x9F59, 0x8F78, 0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F, 0x1080, 0x00A1, 0x30C2, 0x20E3,
     0x5004, 0x4025, 0x7046, 0x6067, 0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E, 0x02B1, 0x1290,
     0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256, 0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
     0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405, 0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E,
     0xC71D, 0xD73C, 0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634, 0xD94C, 0xC96D, 0xF90E, 0xE92F,
     0x99C8, 0x89E9, 0xB98A, 0xA9AB, 0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3, 0xCB7D, 0xDB5C,
     0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A, 0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
     0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9, 0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83,
     0x1CE0, 0x0CC1, 0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8, 0x6E17, 0x7E36, 0x4E55, 0x5E74,
     0x2E93, 0x3EB2, 0x0ED1, 0x1EF0}};

  u16 value = 0;
  for (u32 i = 0; i < 10; i++)
    value = crc16_table[(value >> 8) ^ data[i]] ^ (value << 8);

  return ~(value >> 8) | (~(value) << 8);
}

bool CDImage::SubChannelQ::IsCRCValid() const
{
  return crc == ComputeCRC(data);
}