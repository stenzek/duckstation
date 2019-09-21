#include "cd_image.h"
#include "YBaseLib/ByteStream.h"
#include "YBaseLib/Log.h"
Log_SetChannel(CDImage);

CDImage::CDImage() = default;

CDImage::~CDImage()
{
  if (m_data_file)
    m_data_file->Release();
}

constexpr u64 CDImage::MSFToLBA(u32 minute, u32 second, u32 frame)
{
  return ZeroExtend64(minute) * FRAMES_PER_MINUTE + ZeroExtend64(second) * FRAMES_PER_SECOND + ZeroExtend64(frame);
}

constexpr void CDImage::LBAToMSF(u64 lba, u32* minute, u32* second, u32* frame)
{
  const u32 offset = lba % FRAMES_PER_MINUTE;
  *minute = Truncate32(lba / FRAMES_PER_MINUTE);
  *second = Truncate32(offset / FRAMES_PER_SECOND);
  *frame = Truncate32(offset % FRAMES_PER_SECOND);
}

bool CDImage::Open(const char* path)
{
  Assert(!m_data_file);

  if (!ByteStream_OpenFileStream(path, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_SEEKABLE, &m_data_file))
  {
    Log_ErrorPrintf("Failed to open '%s'", path);
    return false;
  }

  m_lba_count = m_data_file->GetSize() / RAW_SECTOR_SIZE;
  return true;
}

bool CDImage::Seek(u64 lba)
{
  if (lba >= m_lba_count)
    return false;

  if (!m_data_file->SeekAbsolute(lba * RAW_SECTOR_SIZE))
    return false;

  m_current_lba = lba;
  return true;
}

bool CDImage::Seek(u32 minute, u32 second, u32 frame)
{
  return Seek(MSFToLBA(minute, second, frame));
}

u32 CDImage::Read(ReadMode read_mode, u64 lba, u32 sector_count, void* buffer)
{
  if (!Seek(lba))
    return false;

  return Read(read_mode, sector_count, buffer);
}

u32 CDImage::Read(ReadMode read_mode, u32 minute, u32 second, u32 frame, u32 sector_count, void* buffer)
{
  if (!Seek(minute, second, frame))
    return false;

  return Read(read_mode, sector_count, buffer);
}

u32 CDImage::Read(ReadMode read_mode, u32 sector_count, void* buffer)
{
  char* buffer_ptr = static_cast<char*>(buffer);
  u32 sectors_read = 0;
  for (; sectors_read < sector_count; sectors_read++)
  {
    if (m_current_lba == m_lba_count)
      break;

    // get raw sector
    char raw_sector[RAW_SECTOR_SIZE];
    if (!m_data_file->Read2(raw_sector, RAW_SECTOR_SIZE))
    {
      Log_ErrorPrintf("Read of LBA %llu failed", m_current_lba);
      m_data_file->SeekAbsolute(m_current_lba * RAW_SECTOR_SIZE);
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

    m_current_lba++;
    sectors_read++;
  }

  return sectors_read;
}
