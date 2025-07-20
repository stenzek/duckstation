// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cd_image.h"
#include "cue_parser.h"
#include "host.h"
#include "wav_reader_writer.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include "fmt/format.h"
#include "libchdr/cdrom.h" // EDC functions

#include <algorithm>
#include <cinttypes>
#include <map>

LOG_CHANNEL(CDImage);

namespace {

class TrackFileInterface
{
public:
  explicit TrackFileInterface(std::string filename);
  virtual ~TrackFileInterface();

  ALWAYS_INLINE const std::string& GetFileName() const { return m_filename; }

  static std::unique_ptr<TrackFileInterface> OpenBinaryFile(const std::string_view filename, const std::string& path,
                                                            Error* error);

  virtual u64 GetSize() = 0;
  virtual u64 GetDiskSize() = 0;

  virtual bool Read(void* buffer, u64 offset, u32 size, Error* error) = 0;

protected:
  std::string m_filename;
};

class BinaryTrackFileInterface final : public TrackFileInterface
{
public:
  BinaryTrackFileInterface(std::string filename, FileSystem::ManagedCFilePtr file);
  ~BinaryTrackFileInterface() override;

  u64 GetSize() override;
  u64 GetDiskSize() override;

  bool Read(void* buffer, u64 offset, u32 size, Error* error) override;

private:
  FileSystem::ManagedCFilePtr m_file;
  u64 m_file_position = 0;
};

class ECMTrackFileInterface final : public TrackFileInterface
{
public:
  ECMTrackFileInterface(std::string filename, FileSystem::ManagedCFilePtr file);
  ~ECMTrackFileInterface() override;

  static std::unique_ptr<TrackFileInterface> Create(std::string filename, FileSystem::ManagedCFilePtr file,
                                                    Error* error);

  u64 GetSize() override;
  u64 GetDiskSize() override;

  bool Read(void* buffer, u64 offset, u32 size, Error* error) override;

private:
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

  bool BuildSectorMap(Error* error);
  bool ReadChunks(u32 disc_offset, u32 size);

  FileSystem::ManagedCFilePtr m_file;

  DataMap m_data_map;
  std::vector<u8> m_chunk_buffer;
  u32 m_chunk_start = 0;
  u32 m_lba_count = 0;
};

class WaveTrackFileInterface final : public TrackFileInterface
{
public:
  WaveTrackFileInterface(std::string filename, WAVReader reader);
  ~WaveTrackFileInterface() override;

  u64 GetSize() override;
  u64 GetDiskSize() override;

  bool Read(void* buffer, u64 offset, u32 size, Error* error) override;

private:
  WAVReader m_reader;
};

class CDImageCueSheet : public CDImage
{
public:
  CDImageCueSheet();
  ~CDImageCueSheet() override;

  bool OpenAndParseCueSheet(const char* path, Error* error);
  bool OpenAndParseSingleFile(const char* path, Error* error);

  s64 GetSizeOnDisk() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  std::vector<std::unique_ptr<TrackFileInterface>> m_files;
};

} // namespace

//////////////////////////////////////////////////////////////////////////

TrackFileInterface::TrackFileInterface(std::string filename) : m_filename(std::move(filename))
{
}

TrackFileInterface::~TrackFileInterface() = default;

BinaryTrackFileInterface::BinaryTrackFileInterface(std::string filename, FileSystem::ManagedCFilePtr file)
  : TrackFileInterface(std::move(filename)), m_file(std::move(file))
{
}

BinaryTrackFileInterface::~BinaryTrackFileInterface() = default;

std::unique_ptr<TrackFileInterface> TrackFileInterface::OpenBinaryFile(const std::string_view filename,
                                                                       const std::string& path, Error* error)
{
  std::unique_ptr<TrackFileInterface> fi;

  FileSystem::ManagedCFilePtr file =
    FileSystem::OpenManagedSharedCFile(path.c_str(), "rb", FileSystem::FileShareMode::DenyWrite, error);
  if (!file)
  {
    Error::AddPrefixFmt(error, "Failed to open '{}': ", FileSystem::GetDisplayNameFromPath(path));
    return fi;
  }

  // Check for ECM format.
  if (StringUtil::EndsWithNoCase(FileSystem::GetDisplayNameFromPath(path), ".ecm"))
    fi = ECMTrackFileInterface::Create(std::string(filename), std::move(file), error);
  else
    fi = std::make_unique<BinaryTrackFileInterface>(std::string(filename), std::move(file));

  return fi;
}

bool BinaryTrackFileInterface::Read(void* buffer, u64 offset, u32 size, Error* error)
{
  if (m_file_position != offset)
  {
    if (!FileSystem::FSeek64(m_file.get(), static_cast<s64>(offset), SEEK_SET, error)) [[unlikely]]
      return false;

    m_file_position = offset;
  }

  if (std::fread(buffer, size, 1, m_file.get()) != 1) [[unlikely]]
  {
    Error::SetErrno(error, "fread() failed: ", errno);

    // position is indeterminate now
    m_file_position = std::numeric_limits<decltype(m_file_position)>::max();
    return false;
  }

  m_file_position += size;
  return true;
}

u64 BinaryTrackFileInterface::GetSize()
{
  return static_cast<u64>(std::max<s64>(FileSystem::FSize64(m_file.get()), 0));
}

u64 BinaryTrackFileInterface::GetDiskSize()
{
  return static_cast<u64>(std::max<s64>(FileSystem::FSize64(m_file.get()), 0));
}

//////////////////////////////////////////////////////////////////////////

ECMTrackFileInterface::ECMTrackFileInterface(std::string path, FileSystem::ManagedCFilePtr file)
  : TrackFileInterface(std::move(path)), m_file(std::move(file))
{
}

ECMTrackFileInterface::~ECMTrackFileInterface()
{
}

std::unique_ptr<TrackFileInterface> ECMTrackFileInterface::Create(std::string filename,
                                                                  FileSystem::ManagedCFilePtr file, Error* error)
{
  std::unique_ptr<ECMTrackFileInterface> fi =
    std::make_unique<ECMTrackFileInterface>(std::move(filename), std::move(file));
  if (!fi->BuildSectorMap(error))
    fi.reset();

  return fi;
}

bool ECMTrackFileInterface::BuildSectorMap(Error* error)
{
  const s64 file_size = FileSystem::FSize64(m_file.get(), error);
  if (file_size <= 0)
    return false;

  char header[4];
  if (std::fread(header, sizeof(header), 1, m_file.get()) != 1 || header[0] != 'E' || header[1] != 'C' ||
      header[2] != 'M' || header[3] != 0)
  {
    ERROR_LOG("Failed to read/invalid header");
    Error::SetStringView(error, "Failed to read/invalid header");
    return false;
  }

  // build sector map
  u32 file_offset = Truncate32(FileSystem::FTell64(m_file.get()));
  u32 disc_offset = 0;

  for (;;)
  {
    int bits = std::fgetc(m_file.get());
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
      bits = std::fgetc(m_file.get());
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

    if (FileSystem::FSeek64(m_file.get(), file_offset, SEEK_SET) != 0)
    {
      ERROR_LOG("Failed to seek to offset {} after {} chunks", file_offset, m_data_map.size());
      Error::SetStringFmt(error, "Failed to seek to offset {} after {} chunks", file_offset, m_data_map.size());
      return false;
    }
  }

  m_lba_count = disc_offset / CDImage::RAW_SECTOR_SIZE;
  if ((disc_offset % CDImage::RAW_SECTOR_SIZE) != 0)
    WARNING_LOG("ECM image is misaligned with offset {}", disc_offset);

  if (m_data_map.empty() || m_lba_count == 0)
  {
    ERROR_LOG("No data in image '{}'", m_filename);
    Error::SetStringView(error, "No sectors found");
    return false;
  }

  return true;
}

bool ECMTrackFileInterface::ReadChunks(u32 disc_offset, u32 size)
{
  DataMap::iterator next =
    m_data_map.lower_bound((disc_offset > CDImage::RAW_SECTOR_SIZE) ? (disc_offset - CDImage::RAW_SECTOR_SIZE) : 0);
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
    if (current == m_data_map.end() || FileSystem::FSeek64(m_file.get(), current->second.file_offset, SEEK_SET) != 0)
      return false;

    const u32 chunk_size = current->second.chunk_size;
    const u32 chunk_start = static_cast<u32>(m_chunk_buffer.size());
    m_chunk_buffer.resize(chunk_start + chunk_size);

    if (current->second.type == SectorType::Raw)
    {
      if (std::fread(&m_chunk_buffer[chunk_start], chunk_size, 1, m_file.get()) != 1)
        return false;

      total_bytes_read += chunk_size;
    }
    else
    {
      // u8* sector = &m_chunk_buffer[chunk_start];
      u8 sector[CDImage::RAW_SECTOR_SIZE];

      // TODO: needed?
      std::memset(sector, 0, CDImage::RAW_SECTOR_SIZE);
      std::memset(sector + 1, 0xFF, 10);

      u32 skip;
      switch (current->second.type)
      {
        case SectorType::Mode1:
        {
          sector[0x0F] = 0x01;
          if (std::fread(sector + 0x00C, 0x003, 1, m_file.get()) != 1 ||
              std::fread(sector + 0x010, 0x800, 1, m_file.get()) != 1)
          {
            return false;
          }

          edc_set(&sector[2064], edc_compute(sector, 2064));
          ecc_generate(sector);
          skip = 0;
        }
        break;

        case SectorType::Mode2Form1:
        {
          sector[0x0F] = 0x02;
          if (std::fread(sector + 0x014, 0x804, 1, m_file.get()) != 1)
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
          if (std::fread(sector + 0x014, 0x918, 1, m_file.get()) != 1)
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

u64 ECMTrackFileInterface::GetSize()
{
  return static_cast<u64>(m_lba_count) * static_cast<u64>(CDImage::RAW_SECTOR_SIZE);
}

u64 ECMTrackFileInterface::GetDiskSize()
{
  return static_cast<u64>(std::max<s64>(FileSystem::FSize64(m_file.get()), 0));
}

bool ECMTrackFileInterface::Read(void* buffer, u64 offset, u32 size, Error* error)
{
  const u64 file_end = offset + size;
  if (offset < m_chunk_start || file_end > (m_chunk_start + m_chunk_buffer.size()))
  {
    if (!ReadChunks(Truncate32(offset), CDImage::RAW_SECTOR_SIZE))
      return false;
  }

  DebugAssert(offset >= m_chunk_start && file_end <= (m_chunk_start + m_chunk_buffer.size()));

  const size_t chunk_offset = static_cast<size_t>(offset - m_chunk_start);
  std::memcpy(buffer, &m_chunk_buffer[chunk_offset], CDImage::RAW_SECTOR_SIZE);
  return true;
}

//////////////////////////////////////////////////////////////////////////

WaveTrackFileInterface::WaveTrackFileInterface(std::string filename, WAVReader reader)
  : TrackFileInterface(std::move(filename)), m_reader(std::move(reader))
{
}

WaveTrackFileInterface::~WaveTrackFileInterface() = default;

bool WaveTrackFileInterface::Read(void* buffer, u64 offset, u32 size, Error* error)
{
  // Should always be a multiple of 4 (sizeof frame).
  if ((offset & 3) != 0 || (size & 3) != 0) [[unlikely]]
    return false;

  // We shouldn't have any extra CD frames.
  const u32 frame_number = Truncate32(offset / 4);
  if (frame_number >= m_reader.GetNumFrames()) [[unlikely]]
  {
    Error::SetStringView(error, "Attempted read past end of WAV file");
    return false;
  }

  // Do we need to pad the read?
  const u32 num_frames = size / 4;
  const u32 num_frames_to_read = std::min(num_frames, m_reader.GetNumFrames() - frame_number);
  if (num_frames_to_read > 0)
  {
    if (!m_reader.SeekToFrame(frame_number, error) || !m_reader.ReadFrames(buffer, num_frames_to_read, error))
      return false;
  }

  // Padding.
  const u32 padding = num_frames - num_frames_to_read;
  if (padding > 0)
    std::memset(static_cast<u8*>(buffer) + (num_frames_to_read * 4), 0, 4 * padding);

  return true;
}

u64 WaveTrackFileInterface::GetSize()
{
  return Common::AlignUp(static_cast<u64>(m_reader.GetNumFrames()) * 4, 2352);
}

u64 WaveTrackFileInterface::GetDiskSize()
{
  return m_reader.GetFileSize();
}

CDImageCueSheet::CDImageCueSheet() = default;

CDImageCueSheet::~CDImageCueSheet() = default;

bool CDImageCueSheet::OpenAndParseCueSheet(const char* path, Error* error)
{
  std::FILE* fp = FileSystem::OpenSharedCFile(path, "rb", FileSystem::FileShareMode::DenyWrite, error);
  if (!fp)
  {
    Error::AddPrefixFmt(error, "Failed to open cuesheet '{}': ", Path::GetFileName(path));
    return false;
  }

  CueParser::File parser;
  if (!parser.Parse(fp, error))
  {
    std::fclose(fp);
    return false;
  }

  std::fclose(fp);

  m_filename = path;

  u32 disc_lba = 0;

  // for each track..
  for (u32 track_num = 1; track_num <= CueParser::MAX_TRACK_NUMBER; track_num++)
  {
    const CueParser::Track* track = parser.GetTrack(track_num);
    if (!track)
      break;

    const std::string& track_filename = track->file;
    LBA track_start = track->start.ToLBA();

    u32 track_file_index = 0;
    for (; track_file_index < m_files.size(); track_file_index++)
    {
      if (m_files[track_file_index]->GetFileName() == track_filename)
        break;
    }
    if (track_file_index == m_files.size())
    {
      std::string track_full_path =
        !Path::IsAbsolute(track_filename) ? Path::BuildRelativePath(m_filename, track_filename) : track_filename;
      Error track_error;
      std::unique_ptr<TrackFileInterface> track_file;

      if (track->file_format == CueParser::FileFormat::Binary)
      {
        track_file = TrackFileInterface::OpenBinaryFile(track_filename, track_full_path, error);
        if (!track_file && track_file_index == 0)
        {
          // many users have bad cuesheets, or they're renamed the files without updating the cuesheet.
          // so, try searching for a bin with the same name as the cue, but only for the first referenced file.
          std::string alternative_filename = Path::ReplaceExtension(path, "bin");
          track_file = TrackFileInterface::OpenBinaryFile(track_filename, alternative_filename, error);
          if (track_file)
          {
            WARNING_LOG("Your cue sheet references an invalid file '{}', but this was found at '{}' instead.",
                        track_filename, alternative_filename);
          }
        }
      }
      else if (track->file_format == CueParser::FileFormat::Wave)
      {
        // Since all the frames are packed tightly in the wave file, we only need to get the start offset.
        WAVReader reader;
        if (reader.Open(track_full_path.c_str(), error))
        {
          if (reader.GetNumChannels() != AUDIO_CHANNELS || reader.GetSampleRate() != AUDIO_SAMPLE_RATE)
          {
            Error::SetStringFmt(error,
                                TRANSLATE_FS("CDImage", "{0} uses a sample rate of {1}hz and has {2} channels.\n"
                                                        "WAV files must be stereo and use a sample rate of 44100hz."),
                                Path::GetFileName(track_filename), reader.GetSampleRate(), reader.GetNumChannels());
            return false;
          }

          track_file = std::make_unique<WaveTrackFileInterface>(track_filename, std::move(reader));
        }
        else
        {
          Error::AddPrefixFmt(error, "Failed to open '{}': ", track_filename);
        }
      }

      if (!track_file)
        return false;

      m_files.push_back(std::move(track_file));
    }

    // data type determines the sector size
    const TrackMode mode = track->mode;
    const u32 track_sector_size = GetBytesPerSector(mode);

    // precompute subchannel q flags for the whole track
    SubChannelQ::Control control{};
    control.data = mode != TrackMode::Audio;
    control.audio_preemphasis = track->HasFlag(CueParser::TrackFlag::PreEmphasis);
    control.digital_copy_permitted = track->HasFlag(CueParser::TrackFlag::CopyPermitted);
    control.four_channel_audio = track->HasFlag(CueParser::TrackFlag::FourChannelAudio);

    // determine the length from the file
    LBA track_length;
    if (!track->length.has_value())
    {
      u64 file_size = m_files[track_file_index]->GetSize();

      file_size /= track_sector_size;
      if (track_start >= file_size)
      {
        ERROR_LOG("Failed to open track {} in '{}': track start is out of range ({} vs {})", track_num, path,
                  track_start, file_size);
        Error::SetStringFmt(error, "Failed to open track {} in '{}': track start is out of range ({} vs {}))",
                            track_num, Path::GetFileName(path), track_start, file_size);
        return false;
      }

      track_length = static_cast<LBA>(file_size - track_start);
    }
    else
    {
      track_length = track->length.value().ToLBA();
    }

    const Position* index0 = track->GetIndex(0);
    LBA pregap_frames;
    if (index0)
    {
      // index 1 is always present, so this is safe
      pregap_frames = track->GetIndex(1)->ToLBA() - index0->ToLBA();

      // Pregap/index 0 is in the file, easy.
      Index pregap_index = {};
      pregap_index.start_lba_on_disc = disc_lba;
      pregap_index.start_lba_in_track = static_cast<LBA>(-static_cast<s32>(pregap_frames));
      pregap_index.length = pregap_frames;
      pregap_index.track_number = track_num;
      pregap_index.index_number = 0;
      pregap_index.mode = mode;
      pregap_index.submode = CDImage::SubchannelMode::None;
      pregap_index.control.bits = control.bits;
      pregap_index.is_pregap = true;
      pregap_index.file_index = track_file_index;
      pregap_index.file_offset = static_cast<u64>(static_cast<s64>(track_start - pregap_frames)) * track_sector_size;
      pregap_index.file_sector_size = track_sector_size;

      m_indices.push_back(pregap_index);

      disc_lba += pregap_index.length;
    }
    else
    {
      // Two seconds pregap for track 1 is assumed if not specified.
      // Some people have broken (older) dumps where a two second pregap was implicit but not specified in the
      // cuesheet. The problem is we can't tell between a missing implicit two second pregap and a zero second pregap.
      // Most of these seem to be a single bin file for all tracks. So if this is the case, we add the two seconds in
      // if it's not specified. If this is an audio CD (likely when track 1 is not data), we don't add these pregaps,
      // and rely on the cuesheet. If we did add them, it causes issues in some games (e.g. Dancing Stage featuring
      // DREAMS COME TRUE).
      const bool is_multi_track_bin = (track_num > 1 && track_file_index == m_indices[0].file_index);
      const bool likely_audio_cd = (parser.GetTrack(1)->mode == TrackMode::Audio);

      pregap_frames = track->zero_pregap.has_value() ? track->zero_pregap->ToLBA() : 0;
      if ((track_num == 1 || is_multi_track_bin) && !track->zero_pregap.has_value() &&
          (track_num == 1 || !likely_audio_cd))
      {
        pregap_frames = 2 * FRAMES_PER_SECOND;
      }

      // create the index for the pregap
      if (pregap_frames > 0)
      {
        Index pregap_index = {};
        pregap_index.start_lba_on_disc = disc_lba;
        pregap_index.start_lba_in_track = static_cast<LBA>(-static_cast<s32>(pregap_frames));
        pregap_index.length = pregap_frames;
        pregap_index.track_number = track_num;
        pregap_index.index_number = 0;
        pregap_index.mode = mode;
        pregap_index.submode = CDImage::SubchannelMode::None;
        pregap_index.control.bits = control.bits;
        pregap_index.is_pregap = true;
        m_indices.push_back(pregap_index);

        disc_lba += pregap_index.length;
      }
    }

    // add the track itself
    m_tracks.push_back(Track{track_num, disc_lba, static_cast<u32>(m_indices.size()), track_length + pregap_frames,
                             mode, SubchannelMode::None, control});

    // how many indices in this track?
    Index last_index;
    last_index.start_lba_on_disc = disc_lba;
    last_index.start_lba_in_track = 0;
    last_index.track_number = track_num;
    last_index.index_number = 1;
    last_index.file_index = track_file_index;
    last_index.file_sector_size = track_sector_size;
    last_index.file_offset = static_cast<u64>(track_start) * track_sector_size;
    last_index.mode = mode;
    last_index.submode = CDImage::SubchannelMode::None;
    last_index.control.bits = control.bits;
    last_index.is_pregap = false;

    u32 last_index_offset = track_start;
    for (u32 index_num = 1;; index_num++)
    {
      const Position* pos = track->GetIndex(index_num);
      if (!pos)
        break;

      const u32 index_offset = pos->ToLBA();

      // add an index between the track indices
      if (index_offset > last_index_offset)
      {
        last_index.length = index_offset - last_index_offset;
        m_indices.push_back(last_index);

        disc_lba += last_index.length;
        last_index.start_lba_in_track += last_index.length;
        last_index.start_lba_on_disc = disc_lba;
        last_index.length = 0;
      }

      last_index.file_offset = index_offset * last_index.file_sector_size;
      last_index.index_number = static_cast<u32>(index_num);
      last_index_offset = index_offset;
    }

    // and the last index is added here
    const u32 track_end_index = track_start + track_length;
    DebugAssert(track_end_index >= last_index_offset);
    if (track_end_index > last_index_offset)
    {
      last_index.length = track_end_index - last_index_offset;
      m_indices.push_back(last_index);

      disc_lba += last_index.length;
    }
  }

  if (m_tracks.empty())
  {
    ERROR_LOG("File '{}' contains no tracks", path);
    Error::SetStringFmt(error, "File '{}' contains no tracks", Path::GetFileName(path));
    return false;
  }

  m_lba_count = disc_lba;
  AddLeadOutIndex();

  return Seek(1, Position{0, 0, 0});
}

bool CDImageCueSheet::OpenAndParseSingleFile(const char* path, Error* error)
{
  m_filename = path;

  std::unique_ptr<TrackFileInterface> fi = TrackFileInterface::OpenBinaryFile(Path::GetFileName(path), path, error);
  if (!fi)
    return false;

  const u32 track_sector_size = RAW_SECTOR_SIZE;
  m_lba_count = Truncate32(fi->GetSize() / track_sector_size);
  m_files.push_back(std::move(fi));

  SubChannelQ::Control control = {};
  TrackMode mode = TrackMode::Mode2Raw;
  control.data = mode != TrackMode::Audio;

  // Two seconds default pregap.
  const u32 pregap_frames = 2 * FRAMES_PER_SECOND;
  Index pregap_index = {};
  pregap_index.file_sector_size = track_sector_size;
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
  data_index.file_sector_size = track_sector_size;
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
  m_tracks.push_back(Track{static_cast<u32>(1), data_index.start_lba_on_disc, static_cast<u32>(0),
                           m_lba_count + pregap_frames, mode, SubchannelMode::None, control});

  AddLeadOutIndex();

  return Seek(1, Position{0, 0, 0});
}

bool CDImageCueSheet::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  DebugAssert(index.file_index < m_files.size());

  TrackFileInterface* tf = m_files[index.file_index].get();
  const u64 file_position = index.file_offset + (static_cast<u64>(lba_in_index) * index.file_sector_size);
  Error error;
  if (!tf->Read(buffer, file_position, index.file_sector_size, &error)) [[unlikely]]
  {
    ERROR_LOG("Failed to read LBA {}: {}", lba_in_index, error.GetDescription());
    return false;
  }

  return true;
}

s64 CDImageCueSheet::GetSizeOnDisk() const
{
  // Doesn't include the cue.. but they're tiny anyway, whatever.
  u64 size = 0;
  for (const std::unique_ptr<TrackFileInterface>& tf : m_files)
    size += tf->GetDiskSize();
  return size;
}

std::unique_ptr<CDImage> CDImage::OpenCueSheetImage(const char* path, Error* error)
{
  std::unique_ptr<CDImageCueSheet> image = std::make_unique<CDImageCueSheet>();
  if (!image->OpenAndParseCueSheet(path, error))
    image.reset();

  return image;
}

std::unique_ptr<CDImage> CDImage::OpenBinImage(const char* path, Error* error)
{
  std::unique_ptr<CDImageCueSheet> image = std::make_unique<CDImageCueSheet>();
  if (!image->OpenAndParseSingleFile(path, error))
    image.reset();

  return image;
}
