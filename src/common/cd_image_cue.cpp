#include "assert.h"
#include "cd_image.h"
#include "cd_subchannel_replacement.h"
#include "file_system.h"
#include "log.h"
#include <libcue/libcue.h>
#include <algorithm>
#include <map>
Log_SetChannel(CDImageCueSheet);

class CDImageCueSheet : public CDImage
{
public:
  CDImageCueSheet();
  ~CDImageCueSheet() override;

  bool OpenAndParse(const char* filename);

  bool ReadSubChannelQ(SubChannelQ* subq) override;

private:
  Cd* m_cd = nullptr;
  std::map<std::string, std::FILE*> m_files;
  CDSubChannelReplacement m_sbi;
};

CDImageCueSheet::CDImageCueSheet() = default;

CDImageCueSheet::~CDImageCueSheet()
{
  std::for_each(m_files.begin(), m_files.end(), [](const auto& it) { std::fclose(it.second); });
  cd_delete(m_cd);
}

static std::string ReplaceExtension(std::string_view path, std::string_view new_extension)
{
  std::string_view::size_type pos = path.rfind('.');
  if (pos == std::string::npos)
    return std::string(path);

  std::string ret(path, 0, pos + 1);
  ret.append(new_extension);
  return ret;
}

bool CDImageCueSheet::OpenAndParse(const char* filename)
{
  std::FILE* cue_fp = FileSystem::OpenCFile(filename, "rb");
  if (!cue_fp)
  {
    Log_ErrorPrintf("Failed to open cuesheet '%s'", filename);
    return false;
  }

  m_cd = cue_parse_file(cue_fp);
  std::fclose(cue_fp);
  if (!m_cd)
  {
    Log_ErrorPrintf("Failed to parse cuesheet '%s'", filename);
    return false;
  }

  // get the directory of the filename
  std::string basepath = FileSystem::GetPathDirectory(filename) + "/";
  m_filename = filename;

  u32 disc_lba = 0;

  // "last track" subchannel q - used for the pregap
  SubChannelQ::Control last_track_control{};

  // for each track..
  const int num_tracks = cd_get_ntrack(m_cd);
  for (int track_num = 1; track_num <= num_tracks; track_num++)
  {
    const ::Track* track = cd_get_track(m_cd, track_num);
    const std::string track_filename = track_get_filename(track);
    long track_start = track_get_start(track);
    long track_length = track_get_length(track);

    auto it = m_files.find(track_filename);
    if (it == m_files.end())
    {
      std::string track_full_filename = basepath + track_filename;
      std::FILE* track_fp = FileSystem::OpenCFile(track_full_filename.c_str(), "rb");
      if (!track_fp)
      {
        Log_ErrorPrintf("Failed to open track filename '%s' (from '%s' and '%s')", track_full_filename.c_str(),
                        track_filename.c_str(), filename);
        return false;
      }

      it = m_files.emplace(track_filename, track_fp).first;
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
      std::fseek(it->second, 0, SEEK_END);
      long file_size = std::ftell(it->second);
      std::fseek(it->second, 0, SEEK_SET);

      file_size /= track_sector_size;
      Assert(track_start < file_size);
      track_length = file_size - track_start;
    }

    // two seconds pregap for track 1 is assumed if not specified
    long pregap_frames = track_get_zero_pre(track);
    bool pregap_in_file = pregap_frames > 0 && track_start >= pregap_frames;
    if (pregap_frames < 0 && mode != TrackMode::Audio)
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
      pregap_index.control.bits = (track_num > 1) ? last_track_control.bits : control.bits;
      pregap_index.is_pregap = true;
      if (pregap_in_file)
      {
        pregap_index.file = it->second;
        pregap_index.file_offset = static_cast<u64>(static_cast<s64>(track_start - pregap_frames)) * track_sector_size;
        pregap_index.file_sector_size = track_sector_size;
      }

      m_indices.push_back(pregap_index);

      disc_lba += pregap_index.length;
    }

    // add the track itself
    m_tracks.push_back(
      Track{static_cast<u32>(track_num), disc_lba, static_cast<u32>(m_indices.size()), static_cast<u32>(track_length)});
    last_track_control.bits = control.bits;

    // how many indices in this track?
    Index last_index;
    last_index.start_lba_on_disc = disc_lba;
    last_index.start_lba_in_track = 0;
    last_index.track_number = track_num;
    last_index.index_number = 1;
    last_index.file = it->second;
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

  m_lba_count = disc_lba;

  m_sbi.LoadSBI(ReplaceExtension(filename, "sbi").c_str());

  return Seek(1, Position{0, 0, 0});
}

bool CDImageCueSheet::ReadSubChannelQ(SubChannelQ* subq)
{
  if (m_sbi.GetReplacementSubChannelQ(m_position_on_disc, subq->data))
    return true;

  return CDImage::ReadSubChannelQ(subq);
}

std::unique_ptr<CDImage> CDImage::OpenCueSheetImage(const char* filename)
{
  std::unique_ptr<CDImageCueSheet> image = std::make_unique<CDImageCueSheet>();
  if (!image->OpenAndParse(filename))
    return {};

  return image;
}
