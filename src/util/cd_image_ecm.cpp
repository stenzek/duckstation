// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cd_image.h"
#include "cd_subchannel_replacement.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include "libchdr/cdrom.h"

#include <array>
#include <map>

Log_SetChannel(CDImageEcm);

namespace {

class CDImageEcm : public CDImage
{
public:
  CDImageEcm();
  ~CDImageEcm() override;

  bool Open(const char* filename, Error* error);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;
  s64 GetSizeOnDisk() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  bool ReadChunks(u32 disc_offset, u32 size);

  std::FILE* m_fp = nullptr;

  enum class SectorType : u32
  {
    Raw = 0x00,
    Mode1 = 0x01,
    Mode2Form1 = 0x02,
    Mode2Form2 = 0x03,
    Count,
  };

  static constexpr std::array<u32, static_cast<u32>(SectorType::Count)> s_sector_sizes = {
    0x930, // raw
    0x803, // mode1
    0x804, // mode2form1
    0x918, // mode2form2
  };

  static constexpr std::array<u32, static_cast<u32>(SectorType::Count)> s_chunk_sizes = {
    0,    // raw
    2352, // mode1
    2336, // mode2form1
    2336, // mode2form2
  };

  struct SectorEntry
  {
    u32 file_offset;
    u32 chunk_size;
    SectorType type;
  };

  using DataMap = std::map<u32, SectorEntry>;

  DataMap m_data_map;
  std::vector<u8> m_chunk_buffer;
  u32 m_chunk_start = 0;

  CDSubChannelReplacement m_sbi;
};

} // namespace

CDImageEcm::CDImageEcm() = default;

CDImageEcm::~CDImageEcm()
{
  if (m_fp)
    std::fclose(m_fp);
}

bool CDImageEcm::Open(const char* filename, Error* error)
{
  m_filename = filename;
  m_fp = FileSystem::OpenSharedCFile(filename, "rb", FileSystem::FileShareMode::DenyWrite, error);
  if (!m_fp)
  {
    Error::AddPrefixFmt(error, "Failed to open binfile '{}': ", Path::GetFileName(filename));
    return false;
  }

  s64 file_size;
  if (FileSystem::FSeek64(m_fp, 0, SEEK_END) != 0 || (file_size = FileSystem::FTell64(m_fp)) <= 0 ||
      FileSystem::FSeek64(m_fp, 0, SEEK_SET) != 0)
  {
    ERROR_LOG("Get file size failed: errno {}", errno);
    if (error)
      error->SetErrno(errno);

    return false;
  }

  char header[4];
  if (std::fread(header, sizeof(header), 1, m_fp) != 1 || header[0] != 'E' || header[1] != 'C' || header[2] != 'M' ||
      header[3] != 0)
  {
    ERROR_LOG("Failed to read/invalid header");
    Error::SetStringView(error, "Failed to read/invalid header");
    return false;
  }

  // build sector map
  u32 file_offset = static_cast<u32>(std::ftell(m_fp));
  u32 disc_offset = 0;

  for (;;)
  {
    int bits = std::fgetc(m_fp);
    if (bits == EOF)
    {
      ERROR_LOG("Unexpected EOF after {} chunks", m_data_map.size());
      Error::SetStringFmt(error, "Unexpected EOF after {} chunks", m_data_map.size());
      return false;
    }

    file_offset++;
    const SectorType type = static_cast<SectorType>(static_cast<u32>(bits) & 0x03u);
    u32 count = (static_cast<u32>(bits) >> 2) & 0x1F;
    u32 shift = 5;
    while (bits & 0x80)
    {
      bits = std::fgetc(m_fp);
      if (bits == EOF)
      {
        ERROR_LOG("Unexpected EOF after {} chunks", m_data_map.size());
        Error::SetStringFmt(error, "Unexpected EOF after {} chunks", m_data_map.size());
        return false;
      }

      count |= (static_cast<u32>(bits) & 0x7F) << shift;
      shift += 7;
      file_offset++;
    }

    if (count == 0xFFFFFFFFu)
      break;

    // for this sector
    count++;

    if (count >= 0x80000000u)
    {
      ERROR_LOG("Corrupted header after {} chunks", m_data_map.size());
      Error::SetStringFmt(error, "Corrupted header after {} chunks", m_data_map.size());
      return false;
    }

    if (type == SectorType::Raw)
    {
      while (count > 0)
      {
        const u32 size = std::min<u32>(count, 2352);
        m_data_map.emplace(disc_offset, SectorEntry{file_offset, size, type});
        disc_offset += size;
        file_offset += size;
        count -= size;

        if (static_cast<s64>(file_offset) > file_size)
        {
          ERROR_LOG("Out of file bounds after {} chunks", m_data_map.size());
          Error::SetStringFmt(error, "Out of file bounds after {} chunks", m_data_map.size());
        }
      }
    }
    else
    {
      const u32 size = s_sector_sizes[static_cast<u32>(type)];
      const u32 chunk_size = s_chunk_sizes[static_cast<u32>(type)];
      for (u32 i = 0; i < count; i++)
      {
        m_data_map.emplace(disc_offset, SectorEntry{file_offset, chunk_size, type});
        disc_offset += chunk_size;
        file_offset += size;

        if (static_cast<s64>(file_offset) > file_size)
        {
          ERROR_LOG("Out of file bounds after {} chunks", m_data_map.size());
          Error::SetStringFmt(error, "Out of file bounds after {} chunks", m_data_map.size());
        }
      }
    }

    if (std::fseek(m_fp, file_offset, SEEK_SET) != 0)
    {
      ERROR_LOG("Failed to seek to offset {} after {} chunks", file_offset, m_data_map.size());
      Error::SetStringFmt(error, "Failed to seek to offset {} after {} chunks", file_offset, m_data_map.size());
      return false;
    }
  }

  if (m_data_map.empty())
  {
    ERROR_LOG("No data in image '{}'", filename);
    Error::SetStringFmt(error, "No data in image '{}'", filename);
    return false;
  }

  m_lba_count = disc_offset / RAW_SECTOR_SIZE;
  if ((disc_offset % RAW_SECTOR_SIZE) != 0)
    WARNING_LOG("ECM image is misaligned with offset {}", disc_offset);
  if (m_lba_count == 0)
    return false;

  SubChannelQ::Control control = {};
  TrackMode mode = TrackMode::Mode2Raw;
  control.data = mode != TrackMode::Audio;

  // Two seconds default pregap.
  const u32 pregap_frames = 2 * FRAMES_PER_SECOND;
  Index pregap_index = {};
  pregap_index.file_sector_size = RAW_SECTOR_SIZE;
  pregap_index.start_lba_on_disc = 0;
  pregap_index.start_lba_in_track = static_cast<LBA>(-static_cast<s32>(pregap_frames));
  pregap_index.length = pregap_frames;
  pregap_index.track_number = 1;
  pregap_index.index_number = 0;
  pregap_index.mode = mode;
  pregap_index.submode = CDImage::SubchannelMode::None;
  pregap_index.control.bits = control.bits;
  pregap_index.is_pregap = true;
  m_indices.push_back(pregap_index);

  // Data index.
  Index data_index = {};
  data_index.file_index = 0;
  data_index.file_offset = 0;
  data_index.file_sector_size = RAW_SECTOR_SIZE;
  data_index.start_lba_on_disc = pregap_index.length;
  data_index.track_number = 1;
  data_index.index_number = 1;
  data_index.start_lba_in_track = 0;
  data_index.length = m_lba_count;
  data_index.mode = mode;
  data_index.submode = CDImage::SubchannelMode::None;
  data_index.control.bits = control.bits;
  m_indices.push_back(data_index);

  // Assume a single track.
  m_tracks.push_back(Track{static_cast<u32>(1), data_index.start_lba_on_disc, static_cast<u32>(0), m_lba_count, mode,
                           SubchannelMode::None, control});

  AddLeadOutIndex();

  m_sbi.LoadFromImagePath(filename);

  m_chunk_buffer.reserve(RAW_SECTOR_SIZE * 2);
  return Seek(1, Position{0, 0, 0});
}

bool CDImageEcm::ReadChunks(u32 disc_offset, u32 size)
{
  DataMap::iterator next =
    m_data_map.lower_bound((disc_offset > RAW_SECTOR_SIZE) ? (disc_offset - RAW_SECTOR_SIZE) : 0);
  DataMap::iterator current = m_data_map.begin();
  while (next != m_data_map.end() && next->first <= disc_offset)
    current = next++;

  // extra bytes if we need to buffer some at the start
  m_chunk_start = current->first;
  m_chunk_buffer.clear();
  if (m_chunk_start < disc_offset)
    size += (disc_offset - current->first);

  u32 total_bytes_read = 0;
  while (total_bytes_read < size)
  {
    if (current == m_data_map.end() || std::fseek(m_fp, current->second.file_offset, SEEK_SET) != 0)
      return false;

    const u32 chunk_size = current->second.chunk_size;
    const u32 chunk_start = static_cast<u32>(m_chunk_buffer.size());
    m_chunk_buffer.resize(chunk_start + chunk_size);

    if (current->second.type == SectorType::Raw)
    {
      if (std::fread(&m_chunk_buffer[chunk_start], chunk_size, 1, m_fp) != 1)
        return false;

      total_bytes_read += chunk_size;
    }
    else
    {
      // u8* sector = &m_chunk_buffer[chunk_start];
      u8 sector[RAW_SECTOR_SIZE];

      // TODO: needed?
      std::memset(sector, 0, RAW_SECTOR_SIZE);
      std::memset(sector + 1, 0xFF, 10);

      u32 skip;
      switch (current->second.type)
      {
        case SectorType::Mode1:
        {
          sector[0x0F] = 0x01;
          if (std::fread(sector + 0x00C, 0x003, 1, m_fp) != 1 || std::fread(sector + 0x010, 0x800, 1, m_fp) != 1)
            return false;

          edc_set(&sector[2064], edc_compute(sector, 2064));
          ecc_generate(sector);
          skip = 0;
        }
        break;

        case SectorType::Mode2Form1:
        {
          sector[0x0F] = 0x02;
          if (std::fread(sector + 0x014, 0x804, 1, m_fp) != 1)
            return false;

          sector[0x10] = sector[0x14];
          sector[0x11] = sector[0x15];
          sector[0x12] = sector[0x16];
          sector[0x13] = sector[0x17];

          edc_set(&sector[2072], edc_compute(&sector[16], 2056));
          ecc_generate(sector);
          skip = 0x10;
        }
        break;

        case SectorType::Mode2Form2:
        {
          sector[0x0F] = 0x02;
          if (std::fread(sector + 0x014, 0x918, 1, m_fp) != 1)
            return false;

          sector[0x10] = sector[0x14];
          sector[0x11] = sector[0x15];
          sector[0x12] = sector[0x16];
          sector[0x13] = sector[0x17];

          edc_set(&sector[2348], edc_compute(&sector[16], 2332));
          skip = 0x10;
        }
        break;

        default:
          UnreachableCode();
          return false;
      }

      std::memcpy(&m_chunk_buffer[chunk_start], sector + skip, chunk_size);
      total_bytes_read += chunk_size;
    }

    ++current;
  }

  return true;
}

bool CDImageEcm::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  if (m_sbi.GetReplacementSubChannelQ(index.start_lba_on_disc + lba_in_index, subq))
    return true;

  return CDImage::ReadSubChannelQ(subq, index, lba_in_index);
}

bool CDImageEcm::HasNonStandardSubchannel() const
{
  return (m_sbi.GetReplacementSectorCount() > 0);
}

bool CDImageEcm::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  const u32 file_start = static_cast<u32>(index.file_offset) + (lba_in_index * index.file_sector_size);
  const u32 file_end = file_start + RAW_SECTOR_SIZE;

  if (file_start < m_chunk_start || file_end > (m_chunk_start + m_chunk_buffer.size()))
  {
    if (!ReadChunks(file_start, RAW_SECTOR_SIZE))
      return false;
  }

  DebugAssert(file_start >= m_chunk_start && file_end <= (m_chunk_start + m_chunk_buffer.size()));

  const size_t chunk_offset = static_cast<size_t>(file_start - m_chunk_start);
  std::memcpy(buffer, &m_chunk_buffer[chunk_offset], RAW_SECTOR_SIZE);
  return true;
}

s64 CDImageEcm::GetSizeOnDisk() const
{
  return FileSystem::FSize64(m_fp);
}

std::unique_ptr<CDImage> CDImage::OpenEcmImage(const char* filename, Error* error)
{
  std::unique_ptr<CDImageEcm> image = std::make_unique<CDImageEcm>();
  if (!image->Open(filename, error))
    return {};

  return image;
}
