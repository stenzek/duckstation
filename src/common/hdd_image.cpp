#include "hdd_image.h"
#include "YBaseLib/FileSystem.h"
#include "YBaseLib/Log.h"
Log_SetChannel(HDDImage);

#pragma pack(push, 1)
static constexpr u32 LOG_FILE_MAGIC = 0x89374897;
struct LOG_FILE_HEADER
{
  u32 magic;
  u32 sector_size;
  u64 image_size;
  u32 sector_count;
  u32 version_number;
  u8 padding[12];
};
static constexpr u32 STATE_MAGIC = 0x92087348;
struct STATE_HEADER
{
  u32 magic;
  u32 sector_size;
  u64 image_size;
  u32 sector_count;
  u32 version_number;
  u32 num_sectors_in_state;
  u8 padding[8];
};
#pragma pack(pop)

static String GetLogFileName(const char* base_filename)
{
  return String::FromFormat("%s.log", base_filename);
}

static u64 GetSectorMapOffset(HDDImage::SectorIndex index)
{
  return sizeof(LOG_FILE_HEADER) + (static_cast<u64>(index) * sizeof(HDDImage::SectorIndex));
}

HDDImage::HDDImage(const std::string filename, ByteStream* base_stream, ByteStream* log_stream, u64 size,
                   u32 sector_size, u32 sector_count, u32 version_number, LogSectorMap log_sector_map)
  : m_filename(std::move(filename)), m_base_stream(base_stream), m_log_stream(log_stream), m_image_size(size),
    m_sector_size(sector_size), m_sector_count(sector_count), m_version_number(version_number),
    m_log_sector_map(std::move(log_sector_map))
{
  m_current_sector.data = std::make_unique<byte[]>(sector_size);
}

HDDImage::~HDDImage()
{
  m_base_stream->Release();
  m_log_stream->Release();
}

ByteStream* HDDImage::CreateLogFile(const char* filename, bool truncate_existing, bool atomic_update, u64 image_size,
                                    u32 sector_size, u32& num_sectors, u32 version_number, LogSectorMap& sector_map)
{
  if (sector_size == 0 || !Common::IsPow2(sector_size) ||
      ((image_size + (sector_size - 1)) / sector_size) >= std::numeric_limits<SectorIndex>::max())
  {
    return nullptr;
  }

  // Fill sector map with zeros.
  num_sectors = static_cast<u32>((image_size + (sector_size - 1)) / sector_size);
  sector_map.resize(static_cast<size_t>(num_sectors));
  std::fill_n(sector_map.begin(), static_cast<size_t>(num_sectors), InvalidSectorNumber);

  u32 open_flags = BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_WRITE | BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_SEEKABLE;
  if (truncate_existing)
    open_flags |= BYTESTREAM_OPEN_TRUNCATE;
  if (atomic_update)
    open_flags |= BYTESTREAM_OPEN_ATOMIC_UPDATE;

  ByteStream* log_stream = FileSystem::OpenFile(filename, open_flags);
  if (!log_stream)
    return nullptr;

  LOG_FILE_HEADER header = {};
  header.magic = LOG_FILE_MAGIC;
  header.sector_size = sector_size;
  header.image_size = image_size;
  header.sector_count = static_cast<u32>(num_sectors);
  header.version_number = version_number;

  // Write header and sector map to the file.
  if (!log_stream->Write2(&header, sizeof(header)) ||
      !log_stream->Write2(sector_map.data(), static_cast<u32>(sizeof(SectorIndex) * sector_map.size())))
  {
    log_stream->Release();
    FileSystem::DeleteFile(filename);
    return nullptr;
  }

  // Align the first sector to 4K, so we better utilize the OS's page cache.
  u64 pos = log_stream->GetPosition();
  if (!Common::IsAlignedPow2(pos, sector_size))
  {
    u64 padding_end = Common::AlignUpPow2(pos, sector_size);
    while (pos < padding_end)
    {
      u64 data = 0;
      u64 size = std::min(padding_end - pos, u64(sizeof(data)));
      if (!log_stream->Write2(&data, static_cast<u32>(size)))
      {
        log_stream->Release();
        FileSystem::DeleteFile(filename);
        return nullptr;
      }

      pos += size;
    }
  }

  if (!log_stream->Flush())
  {
    log_stream->Release();
    FileSystem::DeleteFile(filename);
    return nullptr;
  }

  return log_stream;
}

ByteStream* HDDImage::OpenLogFile(const char* filename, u64 image_size, u32& sector_size, u32& num_sectors,
                                  u32& version_number, LogSectorMap& sector_map)
{
  ByteStream* log_stream =
    FileSystem::OpenFile(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_WRITE | BYTESTREAM_OPEN_SEEKABLE);
  if (!log_stream)
    return nullptr;

  // Read in the image header.
  LOG_FILE_HEADER header;
  if (!log_stream->Read2(&header, sizeof(header)) || header.magic != LOG_FILE_MAGIC ||
      header.image_size != image_size || header.sector_size == 0 || !Common::IsPow2(header.sector_size))
  {
    Log_ErrorPrintf("Log file '%s': Invalid header", filename);
    log_stream->Release();
    return nullptr;
  }

  num_sectors = static_cast<u32>(image_size / header.sector_size);
  if (num_sectors == 0 || header.sector_count != static_cast<u32>(num_sectors))
  {
    Log_ErrorPrintf("Log file '%s': Corrupted header", filename);
    log_stream->Release();
    return nullptr;
  }

  // Read in the sector map.
  sector_size = header.sector_size;
  version_number = header.version_number;
  sector_map.resize(num_sectors);
  if (!log_stream->Read2(sector_map.data(), static_cast<u32>(sizeof(SectorIndex) * num_sectors)))
  {
    Log_ErrorPrintf("Failed to read sector map from '%s'", filename);
    log_stream->Release();
    return nullptr;
  }

  return log_stream;
}

HDDImage::SectorBuffer& HDDImage::GetSector(SectorIndex sector_index)
{
  if (m_current_sector.sector_number == sector_index)
    return m_current_sector;

  // Unload current sector and replace it.
  ReleaseSector(m_current_sector);
  LoadSector(m_current_sector, sector_index);
  return m_current_sector;
}

void HDDImage::LoadSector(SectorBuffer& buf, SectorIndex sector_index)
{
  Assert(sector_index != InvalidSectorNumber && sector_index < m_log_sector_map.size());
  if (m_log_sector_map[sector_index] == InvalidSectorNumber)
    LoadSectorFromImage(buf, sector_index);
  else
    LoadSectorFromLog(buf, sector_index);
}

std::unique_ptr<HDDImage> HDDImage::Create(const char* filename, u64 size_in_bytes,
                                           u32 sector_size /*= DefaultReplaySectorSize*/)
{
  String log_filename = GetLogFileName(filename);
  if (FileSystem::FileExists(filename) || FileSystem::FileExists(log_filename))
    return nullptr;

  ByteStream* base_stream = FileSystem::OpenFile(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_WRITE |
                                                             BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_SEEKABLE);
  if (!base_stream)
    return nullptr;

  // Write zeros to the image file.
  u64 image_size = 0;
  while (image_size < size_in_bytes)
  {
    u64 data = 0;
    const u32 to_write = static_cast<u32>(std::min(size_in_bytes - image_size, u64(sizeof(data))));
    if (!base_stream->Write2(&data, to_write))
    {
      base_stream->Release();
      FileSystem::DeleteFile(filename);
      return nullptr;
    }

    image_size += to_write;
  }

  // Create the log.
  u32 sector_count;
  LogSectorMap sector_map;
  ByteStream* log_stream =
    CreateLogFile(log_filename, false, false, image_size, sector_size, sector_count, 0, sector_map);
  if (!log_stream)
  {
    base_stream->Release();
    return nullptr;
  }

  return std::unique_ptr<HDDImage>(
    new HDDImage(filename, base_stream, log_stream, image_size, sector_size, sector_count, 0, std::move(sector_map)));
}

std::unique_ptr<HDDImage> HDDImage::Open(const char* filename, u32 sector_size /* = DefaultReplaySectorSize */)
{
  ByteStream* base_stream =
    FileSystem::OpenFile(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_WRITE | BYTESTREAM_OPEN_SEEKABLE);
  if (!base_stream)
    return nullptr;

  u64 image_size = base_stream->GetSize();
  if (image_size == 0)
  {
    base_stream->Release();
    return nullptr;
  }

  String log_filename = GetLogFileName(filename);
  u32 sector_count;
  u32 version_number = 0;
  LogSectorMap sector_map;
  ByteStream* log_stream;
  if (FileSystem::FileExists(log_filename))
  {
    log_stream = OpenLogFile(log_filename, image_size, sector_size, sector_count, version_number, sector_map);
    if (!log_stream)
    {
      Log_ErrorPrintf("Failed to read log file for image '%s'.", filename);
      base_stream->Release();
      return nullptr;
    }
  }
  else
  {
    Log_InfoPrintf("Log file not found for image '%s', creating.", filename);
    log_stream =
      CreateLogFile(log_filename, false, false, image_size, sector_size, sector_count, version_number, sector_map);
    if (!log_stream)
    {
      Log_ErrorPrintf("Failed to create log file for image '%s'.", filename);
      base_stream->Release();
      return nullptr;
    }
  }

  Log_DevPrintf("Opened image '%s' with log file '%s' (sector size %u)", filename, log_filename.GetCharArray(),
                sector_size);
  return std::unique_ptr<HDDImage>(new HDDImage(filename, base_stream, log_stream, image_size, sector_size,
                                                sector_count, version_number, std::move(sector_map)));
}

void HDDImage::LoadSectorFromImage(SectorBuffer& buf, SectorIndex sector_index)
{
  if (!m_base_stream->SeekAbsolute(GetFileOffset(sector_index)) || !m_base_stream->Read2(buf.data.get(), m_sector_size))
    Panic("Failed to read from base image.");

  buf.sector_number = sector_index;
  buf.dirty = false;
  buf.in_log = false;
}

void HDDImage::LoadSectorFromLog(SectorBuffer& buf, SectorIndex sector_index)
{
  DebugAssert(sector_index < m_sector_count);

  const SectorIndex log_sector_index = m_log_sector_map[sector_index];
  Assert(log_sector_index != InvalidSectorNumber);
  if (!m_log_stream->SeekAbsolute(GetFileOffset(log_sector_index)) ||
      !m_log_stream->Read2(buf.data.get(), m_sector_size))
  {
    Panic("Failed to read from log file.");
  }

  buf.sector_number = sector_index;
  buf.dirty = false;
  buf.in_log = true;
}

void HDDImage::WriteSectorToLog(SectorBuffer& buf)
{
  DebugAssert(buf.dirty && buf.sector_number < m_sector_count);

  // Is the sector currently in the log?
  if (!buf.in_log)
  {
    Assert(m_log_sector_map[buf.sector_number] == InvalidSectorNumber);

    // Need to allocate it in the log file.
    if (!m_log_stream->SeekToEnd())
      Panic("Failed to seek to end of log.");

    const u64 sector_offset = m_log_stream->GetPosition();
    const SectorIndex log_sector_number = static_cast<SectorIndex>(sector_offset / m_sector_size);
    Log_DevPrintf("Allocating log sector %u to sector %u", buf.sector_number, log_sector_number);
    m_log_sector_map[buf.sector_number] = log_sector_number;

    // Update log sector map in file.
    if (!m_log_stream->SeekAbsolute(GetSectorMapOffset(buf.sector_number)) ||
        !m_log_stream->Write2(&log_sector_number, sizeof(SectorIndex)))
    {
      Panic("Failed to update sector map in log file.");
    }

    buf.in_log = true;
  }

  // Write to the log.
  const SectorIndex log_sector_index = m_log_sector_map[buf.sector_number];
  Assert(log_sector_index != InvalidSectorNumber);
  if (!m_log_stream->SeekAbsolute(GetFileOffset(log_sector_index)) ||
      !m_log_stream->Write2(buf.data.get(), m_sector_size))
  {
    Panic("Failed to write sector to log file.");
  }

  buf.dirty = false;
}

void HDDImage::ReleaseSector(SectorBuffer& buf)
{
  // Write it to the log file if it's changed.
  if (m_current_sector.dirty)
    WriteSectorToLog(m_current_sector);

  m_current_sector.sector_number = InvalidSectorNumber;
}

void HDDImage::ReleaseAllSectors()
{
  if (m_current_sector.sector_number == InvalidSectorNumber)
    return;

  ReleaseSector(m_current_sector);
}

void HDDImage::Read(void* buffer, u64 offset, u32 size)
{
  Assert((offset + size) <= m_image_size);

  byte* buf = reinterpret_cast<byte*>(buffer);
  while (size > 0)
  {
    // Find the sector that this offset lives in.
    const SectorIndex sector_index = static_cast<SectorIndex>(offset / m_sector_size);
    const u32 offset_in_sector = static_cast<u32>(offset % m_sector_size);
    const u32 size_to_read = std::min(size, m_sector_size - offset_in_sector);

    // Load the sector, and read the sub-sector.
    const SectorBuffer& sec = GetSector(sector_index);
    std::memcpy(buf, &sec.data[offset_in_sector], size_to_read);
    buf += size_to_read;
    offset += size_to_read;
    size -= size_to_read;
  }
}

void HDDImage::Write(const void* buffer, u64 offset, u32 size)
{
  Assert((offset + size) <= m_image_size);

  const byte* buf = reinterpret_cast<const byte*>(buffer);
  while (size > 0)
  {
    // Find the sector that this offset lives in.
    const SectorIndex sector_index = static_cast<SectorIndex>(offset / m_sector_size);
    const u32 offset_in_sector = static_cast<u32>(offset % m_sector_size);
    const u32 size_to_write = std::min(size, m_sector_size - offset_in_sector);

    // Load the sector, and update it.
    SectorBuffer& sec = GetSector(sector_index);
    std::memcpy(&sec.data[offset_in_sector], buf, size_to_write);
    sec.dirty = true;
    buf += size_to_write;
    offset += size_to_write;
    size -= size_to_write;
  }
}

bool HDDImage::LoadState(ByteStream* stream)
{
  ReleaseAllSectors();

  // Read header in from stream. It may not be valid.
  STATE_HEADER header;
  if (!stream->Read2(&header, sizeof(header)) || header.magic != STATE_MAGIC || header.image_size != m_image_size ||
      header.sector_size != m_sector_size || header.sector_count != m_sector_count)
  {
    Log_ErrorPrintf("Corrupted save state.");
    return false;
  }

  // The version number could have changed, which means we committed since this state was saved.
  if (header.version_number != m_version_number)
  {
    Log_ErrorPrintf("Incorrect version number in save state (%u, should be %u), it is a stale state",
                    header.version_number, m_version_number);
    return false;
  }

  // Okay, everything seems fine. We can now throw away the current log file, and re-write it.
  LogSectorMap new_sector_map;
  ByteStream* new_log_stream = CreateLogFile(GetLogFileName(m_filename.c_str()), true, true, m_image_size,
                                             m_sector_size, m_sector_count, m_version_number, new_sector_map);
  if (!new_log_stream)
    return false;

  // Write sectors from log.
  for (u32 i = 0; i < header.num_sectors_in_state; i++)
  {
    const SectorIndex log_sector_index = static_cast<SectorIndex>(new_log_stream->GetPosition() / m_sector_size);
    SectorIndex sector_index;
    if (!stream->Read2(&sector_index, sizeof(sector_index)) || sector_index >= m_sector_count ||
        !ByteStream_CopyBytes(stream, m_sector_size, new_log_stream))
    {
      Log_ErrorPrintf("Failed to copy new sector from save state.");
      new_log_stream->Discard();
      new_log_stream->Release();
      return false;
    }

    // Update new sector map.
    new_sector_map[sector_index] = log_sector_index;
  }

  // Write the new sector map.
  if (!new_log_stream->SeekAbsolute(GetSectorMapOffset(0)) ||
      !new_log_stream->Write2(new_sector_map.data(), sizeof(SectorIndex) * m_sector_count))
  {
    Log_ErrorPrintf("Failed to write new sector map from save state.");
    new_log_stream->Discard();
    new_log_stream->Release();
    return false;
  }

  // Commit the stream, replacing the existing file. Then swap the pointers, since we may as well use the existing one.
  m_log_stream->Release();
  new_log_stream->Flush();
  new_log_stream->Commit();
  m_log_stream = new_log_stream;
  m_log_sector_map = std::move(new_sector_map);
  return true;
}

bool HDDImage::SaveState(ByteStream* stream)
{
  ReleaseAllSectors();

  // Precompute how many sectors are committed to the log.
  u32 log_sector_count = 0;
  for (SectorIndex sector_index = 0; sector_index < m_sector_count; sector_index++)
  {
    if (IsSectorInLog(sector_index))
      log_sector_count++;
  }

  // Construct header.
  STATE_HEADER header = {};
  header.magic = STATE_MAGIC;
  header.sector_size = m_sector_size;
  header.image_size = m_image_size;
  header.sector_count = m_sector_count;
  header.version_number = m_version_number;
  header.num_sectors_in_state = log_sector_count;
  if (!stream->Write2(&header, sizeof(header)))
  {
    Log_ErrorPrintf("Failed to write log header to save state.");
    return false;
  }

  // Copy each sector from the replay log.
  for (SectorIndex sector_index = 0; sector_index < m_sector_count; sector_index++)
  {
    if (!IsSectorInLog(sector_index))
      continue;

    if (!m_log_stream->SeekAbsolute(GetFileOffset(m_log_sector_map[sector_index])) ||
        !stream->Write2(&sector_index, sizeof(sector_index)) ||
        !ByteStream_CopyBytes(m_log_stream, m_sector_size, stream))
    {
      Log_ErrorPrintf("Failed to write log sector to save state.");
      return false;
    }
  }

  return true;
}

void HDDImage::Flush()
{
  if (!m_current_sector.dirty)
    return;

  WriteSectorToLog(m_current_sector);

  // Ensure the stream isn't buffering.
  if (!m_log_stream->Flush())
    Panic("Failed to flush log stream.");
}

void HDDImage::CommitLog()
{
  Log_InfoPrintf("Committing log for '%s'.", m_filename.c_str());
  ReleaseAllSectors();

  for (SectorIndex sector_index = 0; sector_index < m_sector_count; sector_index++)
  {
    if (!IsSectorInLog(sector_index))
      continue;

    // Read log sector to buffer, then write it to the base image.
    // No need to update the log map, since we trash it anyway.
    if (!m_log_stream->SeekAbsolute(GetFileOffset(m_log_sector_map[sector_index])) ||
        !m_base_stream->SeekAbsolute(GetFileOffset(sector_index)) ||
        ByteStream_CopyBytes(m_log_stream, m_sector_size, m_base_stream) != m_sector_size)
    {
      Panic("Failed to transfer sector from log to base image.");
    }
  }

  // Increment the version number, to invalidate old save states.
  m_version_number++;

  // Truncate the log, and re-create it.
  m_log_stream->Release();
  m_log_stream = CreateLogFile(GetLogFileName(m_filename.c_str()), true, false, m_image_size, m_sector_size,
                               m_sector_count, m_version_number, m_log_sector_map);
}

void HDDImage::RevertLog()
{
  Log_InfoPrintf("Reverting log for '%s'", m_filename.c_str());
  ReleaseAllSectors();

  m_log_stream->Release();
  m_log_stream = CreateLogFile(GetLogFileName(m_filename.c_str()), true, false, m_image_size, m_sector_size,
                               m_sector_count, m_version_number, m_log_sector_map);
  if (!m_log_stream)
  {
    Log_ErrorPrintf("Failed to recreate log file for image '%s'", m_filename.c_str());
    Panic("Failed to recreate log file.");
  }
}
