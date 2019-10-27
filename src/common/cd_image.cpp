#include "cd_image.h"
#include "YBaseLib/ByteStream.h"
#include "YBaseLib/Log.h"
Log_SetChannel(CDImage);

CDImage::CDImage() = default;

CDImage::~CDImage() = default;

std::unique_ptr<CDImage> CDImage::Open(const char* filename)
{
  const char* extension = std::strrchr(filename, '.');
  if (!extension)
  {
    Log_ErrorPrintf("Invalid filename: '%s'", filename);
    return nullptr;
  }

#ifdef _MSC_VER
#define CASE_COMPARE _stricmp
#else
#define CASE_COMPARE strcasecmp
#endif

  if (CASE_COMPARE(extension, ".cue") == 0)
    return OpenCueSheetImage(filename);
  else if (CASE_COMPARE(extension, ".bin") == 0 || CASE_COMPARE(extension, ".img") == 0)
    return OpenBinImage(filename);

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

  const u32 new_index_offset = lba - new_index->start_lba_on_disc;
  if (new_index_offset >= new_index->length)
    return false;

  const u64 new_file_offset = new_index->file_offset + (u64(new_index_offset) * new_index->file_sector_size);
  if (new_index->file && std::fseek(new_index->file, static_cast<long>(new_file_offset), SEEK_SET) != 0)
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
  const u32 pos_lba = pos_in_track.ToLBA();
  if (pos_lba >= track.length)
    return false;

  return Seek(track.start_lba + pos_lba);
}

bool CDImage::Seek(const Position& pos)
{
  return Seek(pos.ToLBA());
}

u32 CDImage::Read(ReadMode read_mode, u32 sector_count, void* buffer)
{
  char* buffer_ptr = static_cast<char*>(buffer);
  u32 sectors_read = 0;
  for (; sectors_read < sector_count; sectors_read++)
  {
    if (m_position_in_index == m_current_index->length)
    {
      if (!Seek(m_position_on_disc))
        break;
    }

    Assert(m_current_index->file);

    // get raw sector
    char raw_sector[RAW_SECTOR_SIZE];
    if (std::fread(raw_sector, RAW_SECTOR_SIZE, 1, m_current_index->file) != 1)
    {
      Log_ErrorPrintf("Read of LBA %u failed", m_position_on_disc);
      Seek(m_position_on_disc);
      return false;
    }

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

    m_position_on_disc++;
    m_position_in_index++;
    m_position_in_track++;
    sectors_read++;
  }

  return sectors_read;
}

bool CDImage::ReadRawSector(void* buffer)
{
  if (m_position_in_index == m_current_index->length)
  {
    if (!Seek(m_position_on_disc))
      return false;
  }

  Assert(m_current_index->file);

  // get raw sector
  if (std::fread(buffer, RAW_SECTOR_SIZE, 1, m_current_index->file) != 1)
  {
    Log_ErrorPrintf("Read of LBA %u failed", m_position_on_disc);
    Seek(m_position_on_disc);
    return false;
  }

  m_position_on_disc++;
  m_position_in_index++;
  m_position_in_track++;
  return true;
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
