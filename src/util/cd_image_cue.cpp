// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cd_image.h"
#include "cue_parser.h"
#include "wav_reader_writer.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include "fmt/format.h"

#include <algorithm>
#include <cinttypes>
#include <map>

LOG_CHANNEL(CDImage);

namespace {

class CDImageCueSheet : public CDImage
{
public:
  CDImageCueSheet();
  ~CDImageCueSheet() override;

  bool OpenAndParse(const char* filename, Error* error);

  s64 GetSizeOnDisk() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  class TrackFileInterface
  {
  public:
    TrackFileInterface(std::string filename);
    virtual ~TrackFileInterface();

    ALWAYS_INLINE const std::string& GetFilename() const { return m_filename; }

    virtual u64 GetSize() = 0;
    virtual u64 GetDiskSize() = 0;

    virtual bool Read(void* buffer, u64 offset, u32 size, Error* error) = 0;

  private:
    std::string m_filename;
  };

  struct BinaryTrackFileInterface final : public TrackFileInterface
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

  struct WaveTrackFileInterface final : public TrackFileInterface
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

  std::vector<std::unique_ptr<TrackFileInterface>> m_files;
};

} // namespace

CDImageCueSheet::TrackFileInterface::TrackFileInterface(std::string filename) : m_filename(std::move(filename))
{
}

CDImageCueSheet::TrackFileInterface::~TrackFileInterface() = default;

CDImageCueSheet::BinaryTrackFileInterface::BinaryTrackFileInterface(std::string filename,
                                                                    FileSystem::ManagedCFilePtr file)
  : TrackFileInterface(std::move(filename)), m_file(std::move(file))
{
}

CDImageCueSheet::BinaryTrackFileInterface::~BinaryTrackFileInterface() = default;

bool CDImageCueSheet::BinaryTrackFileInterface::Read(void* buffer, u64 offset, u32 size, Error* error)
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

u64 CDImageCueSheet::BinaryTrackFileInterface::GetSize()
{
  return static_cast<u64>(std::max<s64>(FileSystem::FSize64(m_file.get()), 0));
}

u64 CDImageCueSheet::BinaryTrackFileInterface::GetDiskSize()
{
  return static_cast<u64>(std::max<s64>(FileSystem::FSize64(m_file.get()), 0));
}

CDImageCueSheet::WaveTrackFileInterface::WaveTrackFileInterface(std::string filename, WAVReader reader)
  : TrackFileInterface(std::move(filename)), m_reader(std::move(reader))
{
}

CDImageCueSheet::WaveTrackFileInterface::~WaveTrackFileInterface() = default;

bool CDImageCueSheet::WaveTrackFileInterface::Read(void* buffer, u64 offset, u32 size, Error* error)
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

u64 CDImageCueSheet::WaveTrackFileInterface::GetSize()
{
  return Common::AlignUp(static_cast<u64>(m_reader.GetNumFrames()) * 4, 2352);
}

u64 CDImageCueSheet::WaveTrackFileInterface::GetDiskSize()
{
  return m_reader.GetFileSize();
}

CDImageCueSheet::CDImageCueSheet() = default;

CDImageCueSheet::~CDImageCueSheet() = default;

bool CDImageCueSheet::OpenAndParse(const char* filename, Error* error)
{
  std::FILE* fp = FileSystem::OpenSharedCFile(filename, "rb", FileSystem::FileShareMode::DenyWrite, error);
  if (!fp)
  {
    Error::AddPrefixFmt(error, "Failed to open cuesheet '{}': ", Path::GetFileName(filename));
    return false;
  }

  CueParser::File parser;
  if (!parser.Parse(fp, error))
  {
    std::fclose(fp);
    return false;
  }

  std::fclose(fp);

  m_filename = filename;

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
      if (m_files[track_file_index]->GetFilename() == track_filename)
        break;
    }
    if (track_file_index == m_files.size())
    {
      std::string track_full_filename =
        !Path::IsAbsolute(track_filename) ? Path::BuildRelativePath(m_filename, track_filename) : track_filename;
      Error track_error;
      std::unique_ptr<TrackFileInterface> track_file;

      if (track->file_format == CueParser::FileFormat::Binary)
      {
        FileSystem::ManagedCFilePtr track_fp =
          FileSystem::OpenManagedCFile(track_full_filename.c_str(), "rb", &track_error);
        if (!track_fp && track_file_index == 0)
        {
          // many users have bad cuesheets, or they're renamed the files without updating the cuesheet.
          // so, try searching for a bin with the same name as the cue, but only for the first referenced file.
          std::string alternative_filename = Path::ReplaceExtension(filename, "bin");
          track_fp = FileSystem::OpenManagedCFile(alternative_filename.c_str(), "rb");
          if (track_fp)
          {
            WARNING_LOG("Your cue sheet references an invalid file '{}', but this was found at '{}' instead.",
                        track_filename, alternative_filename);
            track_full_filename = std::move(alternative_filename);
          }
        }
        if (track_fp)
          track_file = std::make_unique<BinaryTrackFileInterface>(std::move(track_full_filename), std::move(track_fp));
      }
      else if (track->file_format == CueParser::FileFormat::Wave)
      {
        // Since all the frames are packed tightly in the wave file, we only need to get the start offset.
        WAVReader reader;
        if (reader.Open(track_full_filename.c_str(), &track_error))
        {
          if (reader.GetNumChannels() != AUDIO_CHANNELS || reader.GetSampleRate() != AUDIO_SAMPLE_RATE)
          {
            Error::SetStringFmt(error, "WAV files must be stereo and use a sample rate of 44100hz.");
            return false;
          }

          track_file = std::make_unique<WaveTrackFileInterface>(std::move(track_full_filename), std::move(reader));
        }
      }

      if (!track_file)
      {
        ERROR_LOG("Failed to open track filename '{}' (from '{}' and '{}'): {}", track_full_filename, track_filename,
                  filename, track_error.GetDescription());
        Error::SetStringFmt(error, "Failed to open track filename '{}' (from '{}' and '{}'): {}", track_full_filename,
                            track_filename, Path::GetFileName(filename), track_error.GetDescription());
        return false;
      }

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
        ERROR_LOG("Failed to open track {} in '{}': track start is out of range ({} vs {})", track_num, filename,
                  track_start, file_size);
        Error::SetStringFmt(error, "Failed to open track {} in '{}': track start is out of range ({} vs {}))",
                            track_num, Path::GetFileName(filename), track_start, file_size);
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
      // Some people have broken (older) dumps where a two second pregap was implicit but not specified in the cuesheet.
      // The problem is we can't tell between a missing implicit two second pregap and a zero second pregap. Most of
      // these seem to be a single bin file for all tracks. So if this is the case, we add the two seconds in if it's
      // not specified. If this is an audio CD (likely when track 1 is not data), we don't add these pregaps, and rely
      // on the cuesheet. If we did add them, it causes issues in some games (e.g. Dancing Stage featuring DREAMS COME
      // TRUE).
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
    ERROR_LOG("File '{}' contains no tracks", filename);
    Error::SetStringFmt(error, "File '{}' contains no tracks", Path::GetFileName(filename));
    return false;
  }

  m_lba_count = disc_lba;
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

std::unique_ptr<CDImage> CDImage::OpenCueSheetImage(const char* filename, Error* error)
{
  std::unique_ptr<CDImageCueSheet> image = std::make_unique<CDImageCueSheet>();
  if (!image->OpenAndParse(filename, error))
    return {};

  return image;
}
