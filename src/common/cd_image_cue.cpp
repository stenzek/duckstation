#include "assert.h"
#include "cd_image.h"
#include "cd_subchannel_replacement.h"
#include "cue_parser.h"
#include "error.h"
#include "file_system.h"
#include "log.h"
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <map>
Log_SetChannel(CDImageCueSheet);

class CDImageCueSheet : public CDImage
{
public:
  CDImageCueSheet();
  ~CDImageCueSheet() override;

  bool OpenAndParse(const char* filename, Common::Error* error);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  struct TrackFile
  {
    std::string filename;
    std::FILE* file;
    u64 file_position;
  };

  std::vector<TrackFile> m_files;
  CDSubChannelReplacement m_sbi;
};

CDImageCueSheet::CDImageCueSheet() = default;

CDImageCueSheet::~CDImageCueSheet()
{
  std::for_each(m_files.begin(), m_files.end(), [](TrackFile& t) { std::fclose(t.file); });
}

bool CDImageCueSheet::OpenAndParse(const char* filename, Common::Error* error)
{
  std::FILE* fp = FileSystem::OpenCFile(filename, "rb");
  if (!fp)
  {
    Log_ErrorPrintf("Failed to open cuesheet '%s': errno %d", filename, errno);
    if (error)
      error->SetErrno(errno);

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

    const std::string track_filename(track->file);
    LBA track_start = track->start.ToLBA();

    u32 track_file_index = 0;
    for (; track_file_index < m_files.size(); track_file_index++)
    {
      const TrackFile& t = m_files[track_file_index];
      if (t.filename == track_filename)
        break;
    }
    if (track_file_index == m_files.size())
    {
      const std::string track_full_filename(!FileSystem::IsAbsolutePath(track_filename) ?
                                              FileSystem::BuildRelativePath(m_filename, track_filename) :
                                              track_filename);
      std::FILE* track_fp = FileSystem::OpenCFile(track_full_filename.c_str(), "rb");
      if (!track_fp && track_file_index == 0)
      {
        // many users have bad cuesheets, or they're renamed the files without updating the cuesheet.
        // so, try searching for a bin with the same name as the cue, but only for the first referenced file.
        const std::string alternative_filename(FileSystem::ReplaceExtension(filename, "bin"));
        track_fp = FileSystem::OpenCFile(alternative_filename.c_str(), "rb");
        if (track_fp)
        {
          Log_WarningPrintf("Your cue sheet references an invalid file '%s', but this was found at '%s' instead.",
                            track_filename.c_str(), alternative_filename.c_str());
        }
      }

      if (!track_fp)
      {
        Log_ErrorPrintf("Failed to open track filename '%s' (from '%s' and '%s'): errno %d",
                        track_full_filename.c_str(), track_filename.c_str(), filename, errno);
        if (error)
        {
          error->SetFormattedMessage("Failed to open track filename '%s' (from '%s' and '%s'): errno %d",
                                     track_full_filename.c_str(), track_filename.c_str(), filename, errno);
        }

        return false;
      }

      m_files.push_back(TrackFile{std::move(track_filename), track_fp, 0});
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
      FileSystem::FSeek64(m_files[track_file_index].file, 0, SEEK_END);
      u64 file_size = static_cast<u64>(FileSystem::FTell64(m_files[track_file_index].file));
      FileSystem::FSeek64(m_files[track_file_index].file, 0, SEEK_SET);

      file_size /= track_sector_size;
      if (track_start >= file_size)
      {
        Log_ErrorPrintf("Failed to open track %u in '%s': track start is out of range (%u vs %" PRIu64 ")", track_num,
                        filename, track_start, file_size);
        if (error)
        {
          error->SetFormattedMessage("Failed to open track %u in '%s': track start is out of range (%u vs %" PRIu64 ")",
                                     track_num, filename, track_start, file_size);
        }
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
        pregap_index.control.bits = control.bits;
        pregap_index.is_pregap = true;
        m_indices.push_back(pregap_index);

        disc_lba += pregap_index.length;
      }
    }

    // add the track itself
    m_tracks.push_back(
      Track{track_num, disc_lba, static_cast<u32>(m_indices.size()), track_length + pregap_frames, mode, control});

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
    Log_ErrorPrintf("File '%s' contains no tracks", filename);
    if (error)
      error->SetFormattedMessage("File '%s' contains no tracks", filename);
    return false;
  }

  m_lba_count = disc_lba;
  AddLeadOutIndex();

  m_sbi.LoadSBIFromImagePath(filename);

  return Seek(1, Position{0, 0, 0});
}

bool CDImageCueSheet::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  if (m_sbi.GetReplacementSubChannelQ(index.start_lba_on_disc + lba_in_index, subq))
    return true;

  return CDImage::ReadSubChannelQ(subq, index, lba_in_index);
}

bool CDImageCueSheet::HasNonStandardSubchannel() const
{
  return (m_sbi.GetReplacementSectorCount() > 0);
}

bool CDImageCueSheet::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  DebugAssert(index.file_index < m_files.size());

  TrackFile& tf = m_files[index.file_index];
  const u64 file_position = index.file_offset + (static_cast<u64>(lba_in_index) * index.file_sector_size);
  if (tf.file_position != file_position)
  {
    if (std::fseek(tf.file, static_cast<long>(file_position), SEEK_SET) != 0)
      return false;

    tf.file_position = file_position;
  }

  if (std::fread(buffer, index.file_sector_size, 1, tf.file) != 1)
  {
    std::fseek(tf.file, static_cast<long>(tf.file_position), SEEK_SET);
    return false;
  }

  tf.file_position += index.file_sector_size;
  return true;
}

std::unique_ptr<CDImage> CDImage::OpenCueSheetImage(const char* filename, Common::Error* error)
{
  std::unique_ptr<CDImageCueSheet> image = std::make_unique<CDImageCueSheet>();
  if (!image->OpenAndParse(filename, error))
    return {};

  return image;
}
