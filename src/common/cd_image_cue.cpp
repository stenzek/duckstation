#include "assert.h"
#include "cd_image.h"
#include "cd_subchannel_replacement.h"
#include "error.h"
#include "file_system.h"
#include "log.h"
#include <algorithm>
#include <cerrno>
#include <libcue/libcue.h>
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
  Cd* m_cd = nullptr;

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
  cd_delete(m_cd);
}

bool CDImageCueSheet::OpenAndParse(const char* filename, Common::Error* error)
{
  std::optional<std::string> cuesheet_string = FileSystem::ReadFileToString(filename);
  if (!cuesheet_string.has_value())
  {
    Log_ErrorPrintf("Failed to open cuesheet '%s': errno %d", filename, errno);
    if (error)
      error->SetErrno(errno);

    return false;
  }

  // work around cuesheet parsing issue - ensure the last character is a newline.
  if (!cuesheet_string->empty() && cuesheet_string->at(cuesheet_string->size() - 1) != '\n')
    *cuesheet_string += '\n';

  m_cd = cue_parse_string(cuesheet_string->c_str());
  if (!m_cd)
  {
    Log_ErrorPrintf("Failed to parse cuesheet '%s'", filename);
    if (error)
      error->SetMessage("Failed to parse cuesheet");

    return false;
  }

  // get the directory of the filename
  std::string basepath(FileSystem::GetPathDirectory(filename));
  basepath += "/";
  m_filename = filename;

  u32 disc_lba = 0;

  // for each track..
  const int num_tracks = cd_get_ntrack(m_cd);
  for (int track_num = 1; track_num <= num_tracks; track_num++)
  {
    const ::Track* track = cd_get_track(m_cd, track_num);
    if (!track || !track_get_filename(track))
    {
      Log_ErrorPrintf("Track/filename missing for track %d", track_num);
      if (error)
        error->SetFormattedMessage("Track/filename missing for track %d", track_num);

      return false;
    }

    const std::string track_filename = track_get_filename(track);
    long track_start = track_get_start(track);
    long track_length = track_get_length(track);

    u32 track_file_index = 0;
    for (; track_file_index < m_files.size(); track_file_index++)
    {
      const TrackFile& t = m_files[track_file_index];
      if (t.filename == track_filename)
        break;
    }
    if (track_file_index == m_files.size())
    {
      const std::string track_full_filename(basepath + track_filename);
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
    const TrackMode mode = static_cast<TrackMode>(track_get_mode(track));
    const u32 track_sector_size = GetBytesPerSector(mode);

    // precompute subchannel q flags for the whole track
    SubChannelQ::Control control{};
    control.data = mode != TrackMode::Audio;
    control.audio_preemphasis = track_is_set_flag(track, FLAG_PRE_EMPHASIS);
    control.digital_copy_permitted = track_is_set_flag(track, FLAG_COPY_PERMITTED);
    control.four_channel_audio = track_is_set_flag(track, FLAG_FOUR_CHANNEL);

    // determine the length from the file
    if (track_length < 0)
    {
      std::fseek(m_files[track_file_index].file, 0, SEEK_END);
      long file_size = std::ftell(m_files[track_file_index].file);
      std::fseek(m_files[track_file_index].file, 0, SEEK_SET);

      file_size /= track_sector_size;
      if (track_start >= file_size)
      {
        Log_ErrorPrintf("Failed to open track %u in '%s': track start is out of range (%ld vs %ld)", track_num,
                        filename, track_start, file_size);
        if (error)
        {
          error->SetFormattedMessage("Failed to open track %u in '%s': track start is out of range (%ld vs %ld)",
                                     track_num, filename, track_start, file_size);
        }
        return false;
      }

      track_length = file_size - track_start;
    }

    // Two seconds pregap for track 1 is assumed if not specified.
    // Some people have broken (older) dumps where a two second pregap was implicit but not specified in the cuesheet.
    // The problem is we can't tell between a missing implicit two second pregap and a zero second pregap. Most of these
    // seem to be a single bin file for all tracks. So if this is the case, we add the two seconds in if it's not
    // specified. If this is an audio CD (likely when track 1 is not data), we don't add these pregaps, and rely on the
    // cuesheet. If we did add them, it causes issues in some games (e.g. Dancing Stage featuring DREAMS COME TRUE).
    long pregap_frames = track_get_zero_pre(track);
    const bool pregap_in_file = pregap_frames > 0 && track_start >= pregap_frames;
    const bool is_multi_track_bin = (track_num > 1 && track_file_index == m_indices[0].file_index);
    const bool likely_audio_cd = static_cast<TrackMode>(track_get_mode(cd_get_track(m_cd, 1))) == TrackMode::Audio;
    if ((track_num == 1 || is_multi_track_bin) && pregap_frames < 0 && (track_num == 1 || !likely_audio_cd))
      pregap_frames = 2 * FRAMES_PER_SECOND;

    // create the index for the pregap
    if (pregap_frames > 0)
    {
      Index pregap_index = {};
      pregap_index.start_lba_on_disc = disc_lba;
      pregap_index.start_lba_in_track = static_cast<LBA>(static_cast<unsigned long>(-pregap_frames));
      pregap_index.length = pregap_frames;
      pregap_index.track_number = track_num;
      pregap_index.index_number = 0;
      pregap_index.mode = mode;
      pregap_index.control.bits = control.bits;
      pregap_index.is_pregap = true;
      if (pregap_in_file)
      {
        pregap_index.file_index = track_file_index;
        pregap_index.file_offset = static_cast<u64>(static_cast<s64>(track_start - pregap_frames)) * track_sector_size;
        pregap_index.file_sector_size = track_sector_size;
      }

      m_indices.push_back(pregap_index);

      disc_lba += pregap_index.length;
    }

    // add the track itself
    m_tracks.push_back(Track{static_cast<u32>(track_num), disc_lba, static_cast<u32>(m_indices.size()),
                             static_cast<u32>(track_length + pregap_frames), mode, control});

    // how many indices in this track?
    Index last_index;
    last_index.start_lba_on_disc = disc_lba;
    last_index.start_lba_in_track = 0;
    last_index.track_number = track_num;
    last_index.index_number = 1;
    last_index.file_index = track_file_index;
    last_index.file_sector_size = track_sector_size;
    last_index.file_offset = static_cast<u64>(static_cast<s64>(track_start)) * track_sector_size;
    last_index.mode = mode;
    last_index.control.bits = control.bits;
    last_index.is_pregap = false;

    long last_index_offset = track_start;
    for (int index_num = 1;; index_num++)
    {
      long index_offset = track_get_index(track, index_num);
      if (index_offset < 0)
        break;

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
    const long track_end_index = track_start + track_length;
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

  m_sbi.LoadSBI(FileSystem::ReplaceExtension(filename, "sbi").c_str());

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
