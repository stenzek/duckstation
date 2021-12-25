#include "assert.h"
#include "cd_image.h"
#include "cd_subchannel_replacement.h"
#include "error.h"
#include "file_system.h"
#include "log.h"
#include <algorithm>
#include <cerrno>
#include <map>
Log_SetChannel(CDImageMds);

#pragma pack(push, 1)
struct TrackEntry
{
  u8 track_type;
  u8 has_subchannel_data;
  u8 unk1;
  u8 unk2;
  u8 track_number;
  u8 unk3[4];
  u8 start_m;
  u8 start_s;
  u8 start_f;
  u32 extra_offset;
  u8 unk4[24];
  u32 track_offset_in_mdf;
  u8 unk5[36];
};
static_assert(sizeof(TrackEntry) == 0x50, "TrackEntry is 0x50 bytes");
#pragma pack(pop)

class CDImageMds : public CDImage
{
public:
  CDImageMds();
  ~CDImageMds() override;

  bool OpenAndParse(const char* filename, Common::Error* error);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  std::FILE* m_mdf_file = nullptr;
  u64 m_mdf_file_position = 0;
  CDSubChannelReplacement m_sbi;
};

CDImageMds::CDImageMds() = default;

CDImageMds::~CDImageMds()
{
  if (m_mdf_file)
    std::fclose(m_mdf_file);
}

bool CDImageMds::OpenAndParse(const char* filename, Common::Error* error)
{
  std::FILE* mds_fp = FileSystem::OpenCFile(filename, "rb");
  if (!mds_fp)
  {
    Log_ErrorPrintf("Failed to open mds '%s': errno %d", filename, errno);
    if (error)
      error->SetErrno(errno);

    return false;
  }

  std::optional<std::vector<u8>> mds_data_opt(FileSystem::ReadBinaryFile(mds_fp));
  std::fclose(mds_fp);
  if (!mds_data_opt.has_value() || mds_data_opt->size() < 0x54)
  {
    Log_ErrorPrintf("Failed to read mds file '%s'", filename);
    if (error)
      error->SetFormattedMessage("Failed to read mds file '%s'", filename);

    return false;
  }

  std::string mdf_filename(FileSystem::ReplaceExtension(filename, "mdf"));
  m_mdf_file = FileSystem::OpenCFile(mdf_filename.c_str(), "rb");
  if (!m_mdf_file)
  {
    Log_ErrorPrintf("Failed to open mdf file '%s': errno %d", mdf_filename.c_str(), errno);
    if (error)
      error->SetFormattedMessage("Failed to open mdf file '%s': errno %d", mdf_filename.c_str(), errno);

    return false;
  }

  const std::vector<u8>& mds = mds_data_opt.value();
  static constexpr char expected_signature[] = "MEDIA DESCRIPTOR";
  if (std::memcmp(&mds[0], expected_signature, sizeof(expected_signature) - 1) != 0)
  {
    Log_ErrorPrintf("Incorrect signature in '%s'", filename);
    if (error)
      error->SetFormattedMessage("Incorrect signature in '%s'", filename);

    return false;
  }

  u32 session_offset;
  std::memcpy(&session_offset, &mds[0x50], sizeof(session_offset));
  if ((session_offset + 24) > mds.size())
  {
    Log_ErrorPrintf("Invalid session offset in '%s'", filename);
    if (error)
      error->SetFormattedMessage("Invalid session offset in '%s'", filename);

    return false;
  }

  u16 track_count;
  u32 track_offset;
  std::memcpy(&track_count, &mds[session_offset + 14], sizeof(track_count));
  std::memcpy(&track_offset, &mds[session_offset + 20], sizeof(track_offset));
  if (track_count > 99 || track_offset >= mds.size())
  {
    Log_ErrorPrintf("Invalid track count/block offset %u/%u in '%s'", track_count, track_offset, filename);
    if (error)
      error->SetFormattedMessage("Invalid track count/block offset %u/%u in '%s'", track_count, track_offset, filename);

    return false;
  }

  while ((track_offset + sizeof(TrackEntry)) <= mds.size())
  {
    TrackEntry track;
    std::memcpy(&track, &mds[track_offset], sizeof(track));
    if (track.track_number < 0xA0)
      break;

    track_offset += sizeof(TrackEntry);
  }

  for (u32 track_number = 1; track_number <= track_count; track_number++)
  {
    if ((track_offset + sizeof(TrackEntry)) > mds.size())
    {
      Log_ErrorPrintf("End of file in '%s' at track %u", filename, track_number);
      if (error)
        error->SetFormattedMessage("End of file in '%s' at track %u", filename, track_number);

      return false;
    }

    TrackEntry track;
    std::memcpy(&track, &mds[track_offset], sizeof(track));
    track_offset += sizeof(TrackEntry);

    if (PackedBCDToBinary(track.track_number) != track_number)
    {
      Log_ErrorPrintf("Unexpected track number 0x%02X in track %u", track.track_number, track_number);
      if (error)
        error->SetFormattedMessage("Unexpected track number 0x%02X in track %u", track.track_number, track_number);

      return false;
    }

    const bool contains_subchannel = (track.has_subchannel_data != 0);
    const u32 track_sector_size = (contains_subchannel ? 2448 : RAW_SECTOR_SIZE);
    const TrackMode mode = (track.track_type == 0xA9) ? TrackMode::Audio : TrackMode::Mode2Raw;

    if ((track.extra_offset + sizeof(u32) + sizeof(u32)) > mds.size())
    {
      Log_ErrorPrintf("Invalid extra offset %u in track %u", track.extra_offset, track_number);
      if (error)
        error->SetFormattedMessage("Invalid extra offset %u in track %u", track.extra_offset, track_number);

      return false;
    }

    u32 track_start_lba = Position::FromBCD(track.start_m, track.start_s, track.start_f).ToLBA();
    u32 track_file_offset = track.track_offset_in_mdf;

    u32 track_pregap;
    u32 track_length;
    std::memcpy(&track_pregap, &mds[track.extra_offset], sizeof(track_pregap));
    std::memcpy(&track_length, &mds[track.extra_offset + sizeof(u32)], sizeof(track_length));

    // precompute subchannel q flags for the whole track
    // todo: pull from mds?
    SubChannelQ::Control control{};
    control.data = mode != TrackMode::Audio;

    // create the index for the pregap
    if (track_pregap > 0)
    {
      if (track_pregap > track_start_lba)
      {
        Log_ErrorPrintf("Track pregap %u is too large for start lba %u", track_pregap, track_start_lba);
        if (error)
          error->SetFormattedMessage("Track pregap %u is too large for start lba %u", track_pregap, track_start_lba);

        return false;
      }

      Index pregap_index = {};
      pregap_index.start_lba_on_disc = track_start_lba - track_pregap;
      pregap_index.start_lba_in_track = static_cast<LBA>(-static_cast<s32>(track_pregap));
      pregap_index.length = track_pregap;
      pregap_index.track_number = track_number;
      pregap_index.index_number = 0;
      pregap_index.mode = mode;
      pregap_index.control.bits = control.bits;
      pregap_index.is_pregap = true;

      const bool pregap_in_file = (track_number > 1);
      if (pregap_in_file)
      {
        pregap_index.file_index = 0;
        pregap_index.file_offset = track_file_offset;
        pregap_index.file_sector_size = track_sector_size;
        track_file_offset += track_pregap * track_sector_size;
      }

      m_indices.push_back(pregap_index);
    }

    // add the track itself
    m_tracks.push_back(Track{static_cast<u32>(track_number), track_start_lba, static_cast<u32>(m_indices.size()),
                             static_cast<u32>(track_length), mode, control});

    // how many indices in this track?
    Index last_index;
    last_index.start_lba_on_disc = track_start_lba;
    last_index.start_lba_in_track = 0;
    last_index.track_number = track_number;
    last_index.index_number = 1;
    last_index.file_index = 0;
    last_index.file_sector_size = track_sector_size;
    last_index.file_offset = track_file_offset;
    last_index.mode = mode;
    last_index.control.bits = control.bits;
    last_index.is_pregap = false;
    last_index.length = track_length;
    m_indices.push_back(last_index);
  }

  if (m_tracks.empty())
  {
    Log_ErrorPrintf("File '%s' contains no tracks", filename);
    if (error)
      error->SetFormattedMessage("File '%s' contains no tracks", filename);

    return false;
  }

  m_lba_count = m_tracks.back().start_lba + m_tracks.back().length;
  AddLeadOutIndex();

  m_sbi.LoadSBIFromImagePath(filename);

  return Seek(1, Position{0, 0, 0});
}

bool CDImageMds::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  if (m_sbi.GetReplacementSubChannelQ(index.start_lba_on_disc + lba_in_index, subq))
    return true;

  return CDImage::ReadSubChannelQ(subq, index, lba_in_index);
}

bool CDImageMds::HasNonStandardSubchannel() const
{
  return (m_sbi.GetReplacementSectorCount() > 0);
}

bool CDImageMds::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  const u64 file_position = index.file_offset + (static_cast<u64>(lba_in_index) * index.file_sector_size);
  if (m_mdf_file_position != file_position)
  {
    if (std::fseek(m_mdf_file, static_cast<long>(file_position), SEEK_SET) != 0)
      return false;

    m_mdf_file_position = file_position;
  }

  // we don't want the subchannel data
  const u32 read_size = RAW_SECTOR_SIZE;
  if (std::fread(buffer, read_size, 1, m_mdf_file) != 1)
  {
    std::fseek(m_mdf_file, static_cast<long>(m_mdf_file_position), SEEK_SET);
    return false;
  }

  m_mdf_file_position += read_size;
  return true;
}

std::unique_ptr<CDImage> CDImage::OpenMdsImage(const char* filename, Common::Error* error)
{
  std::unique_ptr<CDImageMds> image = std::make_unique<CDImageMds>();
  if (!image->OpenAndParse(filename, error))
    return {};

  return image;
}
