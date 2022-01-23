#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "align.h"
#include "assert.h"
#include "cd_image.h"
#include "cd_subchannel_replacement.h"
#include "error.h"
#include "file_system.h"
#include "libchdr/chd.h"
#include "log.h"
#include "platform.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
Log_SetChannel(CDImageCHD);

static std::optional<CDImage::TrackMode> ParseTrackModeString(const char* str)
{
  if (std::strncmp(str, "MODE2_FORM_MIX", 14) == 0)
    return CDImage::TrackMode::Mode2FormMix;
  else if (std::strncmp(str, "MODE2_FORM1", 10) == 0)
    return CDImage::TrackMode::Mode2Form1;
  else if (std::strncmp(str, "MODE2_FORM2", 10) == 0)
    return CDImage::TrackMode::Mode2Form2;
  else if (std::strncmp(str, "MODE2_RAW", 9) == 0)
    return CDImage::TrackMode::Mode2Raw;
  else if (std::strncmp(str, "MODE1_RAW", 9) == 0)
    return CDImage::TrackMode::Mode1Raw;
  else if (std::strncmp(str, "MODE1", 5) == 0)
    return CDImage::TrackMode::Mode1;
  else if (std::strncmp(str, "MODE2", 5) == 0)
    return CDImage::TrackMode::Mode2;
  else if (std::strncmp(str, "AUDIO", 5) == 0)
    return CDImage::TrackMode::Audio;
  else
    return std::nullopt;
}

class CDImageCHD : public CDImage
{
public:
  CDImageCHD(OpenFlags open_flags);
  ~CDImageCHD() override;

  bool Open(const char* filename, Common::Error* error);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  enum : u32
  {
    CHD_CD_SECTOR_DATA_SIZE = 2352 + 96,
    CHD_CD_TRACK_ALIGNMENT = 4
  };

  bool ReadHunk(u32 hunk_index);

  std::FILE* m_fp = nullptr;
  chd_file* m_chd = nullptr;
  u32 m_hunk_size = 0;
  u32 m_sectors_per_hunk = 0;

  std::vector<u8> m_hunk_buffer;
  u32 m_current_hunk_index = static_cast<u32>(-1);

  CDSubChannelReplacement m_sbi;
};

CDImageCHD::CDImageCHD(OpenFlags open_flags) : CDImage(open_flags) {}

CDImageCHD::~CDImageCHD()
{
  if (m_chd)
    chd_close(m_chd);
  if (m_fp)
    std::fclose(m_fp);
}

bool CDImageCHD::Open(const char* filename, Common::Error* error)
{
  Assert(!m_fp);
  m_fp = FileSystem::OpenCFile(filename, "rb");
  if (!m_fp)
  {
    Log_ErrorPrintf("Failed to open CHD '%s': errno %d", filename, errno);
    if (error)
      error->SetErrno(errno);

    return false;
  }

  chd_error err = chd_open_file(m_fp, CHD_OPEN_READ, nullptr, &m_chd);
  if (err != CHDERR_NONE)
  {
    Log_ErrorPrintf("Failed to open CHD '%s': %s", filename, chd_error_string(err));
    if (error)
      error->SetMessage(chd_error_string(err));

    return false;
  }

  if ((GetOpenFlags() & OpenFlags::PreCache) != OpenFlags::None)
  {
    const chd_error err = chd_precache(m_chd);
    if (err != CHDERR_NONE)
    {
      Log_WarningPrintf("Failed to pre-cache CHD '%s': %s", filename, chd_error_string(err));
      if (error)
        error->SetMessage(chd_error_string(err));
    }
  }

  const chd_header* header = chd_get_header(m_chd);
  m_hunk_size = header->hunkbytes;
  if ((m_hunk_size % CHD_CD_SECTOR_DATA_SIZE) != 0)
  {
    Log_ErrorPrintf("Hunk size (%u) is not a multiple of %u", m_hunk_size, CHD_CD_SECTOR_DATA_SIZE);
    if (error)
      error->SetFormattedMessage("Hunk size (%u) is not a multiple of %u", m_hunk_size, CHD_CD_SECTOR_DATA_SIZE);

    return false;
  }

  m_sectors_per_hunk = m_hunk_size / CHD_CD_SECTOR_DATA_SIZE;
  m_hunk_buffer.resize(m_hunk_size);
  m_filename = filename;

  u32 disc_lba = 0;
  u64 file_lba = 0;

  // for each track..
  int num_tracks = 0;
  for (;;)
  {
    char metadata_str[256];
    char type_str[256];
    char subtype_str[256];
    char pgtype_str[256];
    char pgsub_str[256];
    u32 metadata_length;

    int track_num = 0, frames = 0, pregap_frames = 0, postgap_frames = 0;
    err = chd_get_metadata(m_chd, CDROM_TRACK_METADATA2_TAG, num_tracks, metadata_str, sizeof(metadata_str),
                           &metadata_length, nullptr, nullptr);
    if (err == CHDERR_NONE)
    {
      if (std::sscanf(metadata_str, CDROM_TRACK_METADATA2_FORMAT, &track_num, type_str, subtype_str, &frames,
                      &pregap_frames, pgtype_str, pgsub_str, &postgap_frames) != 8)
      {
        Log_ErrorPrintf("Invalid track v2 metadata: '%s'", metadata_str);
        if (error)
          error->SetFormattedMessage("Invalid track v2 metadata: '%s'", metadata_str);

        return false;
      }
    }
    else
    {
      // try old version
      err = chd_get_metadata(m_chd, CDROM_TRACK_METADATA_TAG, num_tracks, metadata_str, sizeof(metadata_str),
                             &metadata_length, nullptr, nullptr);
      if (err != CHDERR_NONE)
      {
        // not found, so no more tracks
        break;
      }

      if (std::sscanf(metadata_str, CDROM_TRACK_METADATA_FORMAT, &track_num, type_str, subtype_str, &frames) != 4)
      {
        Log_ErrorPrintf("Invalid track metadata: '%s'", metadata_str);
        if (error)
          error->SetFormattedMessage("Invalid track v2 metadata: '%s'", metadata_str);

        return false;
      }
    }

    if (track_num != (num_tracks + 1))
    {
      Log_ErrorPrintf("Incorrect track number at index %d, expected %d got %d", num_tracks, (num_tracks + 1),
                      track_num);
      if (error)
      {
        error->SetFormattedMessage("Incorrect track number at index %d, expected %d got %d", num_tracks,
                                   (num_tracks + 1), track_num);
      }

      return false;
    }

    std::optional<TrackMode> mode = ParseTrackModeString(type_str);
    if (!mode.has_value())
    {
      Log_ErrorPrintf("Invalid track mode: '%s'", type_str);
      if (error)
        error->SetFormattedMessage("Invalid track mode: '%s'", type_str);

      return false;
    }

    // precompute subchannel q flags for the whole track
    SubChannelQ::Control control{};
    control.data = mode.value() != TrackMode::Audio;

    // two seconds pregap for track 1 is assumed if not specified
    const bool pregap_in_file = (pregap_frames > 0 && pgtype_str[0] == 'V');
    if (pregap_frames <= 0 && mode != TrackMode::Audio)
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
      pregap_index.mode = mode.value();
      pregap_index.control.bits = control.bits;
      pregap_index.is_pregap = true;

      if (pregap_in_file)
      {
        if (pregap_frames > frames)
        {
          Log_ErrorPrintf("Pregap length %u exceeds track length %u", pregap_frames, frames);
          if (error)
            error->SetFormattedMessage("Pregap length %u exceeds track length %u", pregap_frames, frames);

          return false;
        }

        pregap_index.file_index = 0;
        pregap_index.file_offset = file_lba;
        pregap_index.file_sector_size = CHD_CD_SECTOR_DATA_SIZE;
        file_lba += pregap_frames;
        frames -= pregap_frames;
      }

      m_indices.push_back(pregap_index);
      disc_lba += pregap_frames;
    }

    // add the track itself
    m_tracks.push_back(Track{static_cast<u32>(track_num), disc_lba, static_cast<u32>(m_indices.size()),
                             static_cast<u32>(frames + pregap_frames), mode.value(), control});

    // how many indices in this track?
    Index index = {};
    index.start_lba_on_disc = disc_lba;
    index.start_lba_in_track = 0;
    index.track_number = track_num;
    index.index_number = 1;
    index.file_index = 0;
    index.file_sector_size = CHD_CD_SECTOR_DATA_SIZE;
    index.file_offset = file_lba;
    index.mode = mode.value();
    index.control.bits = control.bits;
    index.is_pregap = false;
    index.length = static_cast<u32>(frames);
    m_indices.push_back(index);

    disc_lba += index.length;
    file_lba += index.length;
    num_tracks++;

    // each track is padded to a multiple of 4 frames, see chdman source.
    file_lba = Common::AlignUp(file_lba, CHD_CD_TRACK_ALIGNMENT);
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

bool CDImageCHD::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  if (m_sbi.GetReplacementSubChannelQ(index.start_lba_on_disc + lba_in_index, subq))
    return true;

  // TODO: Read subchannel data from CHD

  return CDImage::ReadSubChannelQ(subq, index, lba_in_index);
}

bool CDImageCHD::HasNonStandardSubchannel() const
{
  return (m_sbi.GetReplacementSectorCount() > 0);
}

// There's probably a more efficient way of doing this with vectorization...
ALWAYS_INLINE static void CopyAndSwap(void* dst_ptr, const u8* src_ptr, u32 data_size)
{
  u8* dst_ptr_byte = static_cast<u8*>(dst_ptr);
#if defined(CPU_X64) || defined(CPU_AARCH64)
  const u32 num_values = data_size / 8;
  for (u32 i = 0; i < num_values; i++)
  {
    u64 value;
    std::memcpy(&value, src_ptr, sizeof(value));
    value = ((value >> 8) & UINT64_C(0x00FF00FF00FF00FF)) | ((value << 8) & UINT64_C(0xFF00FF00FF00FF00));
    std::memcpy(dst_ptr_byte, &value, sizeof(value));
    src_ptr += sizeof(value);
    dst_ptr_byte += sizeof(value);
  }
#elif defined(CPU_X86) || defined(CPU_ARM)
  const u32 num_values = data_size / 4;
  for (u32 i = 0; i < num_values; i++)
  {
    u32 value;
    std::memcpy(&value, src_ptr, sizeof(value));
    value = ((value >> 8) & UINT32_C(0x00FF00FF)) | ((value << 8) & UINT32_C(0xFF00FF00));
    std::memcpy(dst_ptr_byte, &value, sizeof(value));
    src_ptr += sizeof(value);
    dst_ptr_byte += sizeof(value);
  }
#else
  const u32 num_values = data_size / sizeof(u16);
  for (u32 i = 0; i < num_values; i++)
  {
    u16 value;
    std::memcpy(&value, src_ptr, sizeof(value));
    value = (value << 8) | (value >> 8);
    std::memcpy(dst_ptr_byte, &value, sizeof(value));
    src_ptr += sizeof(value);
    dst_ptr_byte += sizeof(value);
  }
#endif
}

bool CDImageCHD::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  const u32 disc_frame = static_cast<LBA>(index.file_offset) + lba_in_index;
  const u32 hunk_index = static_cast<u32>(disc_frame / m_sectors_per_hunk);
  const u32 hunk_offset = static_cast<u32>((disc_frame % m_sectors_per_hunk) * CHD_CD_SECTOR_DATA_SIZE);
  DebugAssert((m_hunk_size - hunk_offset) >= CHD_CD_SECTOR_DATA_SIZE);

  if (m_current_hunk_index != hunk_index && !ReadHunk(hunk_index))
    return false;

  // Audio data is in big-endian, so we have to swap it for little endian hosts...
  if (index.mode == TrackMode::Audio)
    CopyAndSwap(buffer, &m_hunk_buffer[hunk_offset], RAW_SECTOR_SIZE);
  else
    std::memcpy(buffer, &m_hunk_buffer[hunk_offset], RAW_SECTOR_SIZE);

  return true;
}

bool CDImageCHD::ReadHunk(u32 hunk_index)
{
  const chd_error err = chd_read(m_chd, hunk_index, m_hunk_buffer.data());
  if (err != CHDERR_NONE)
  {
    Log_ErrorPrintf("chd_read(%u) failed: %s", hunk_index, chd_error_string(err));

    // data might have been partially written
    m_current_hunk_index = static_cast<u32>(-1);
    return false;
  }

  m_current_hunk_index = hunk_index;
  return true;
}

std::unique_ptr<CDImage> CDImage::OpenCHDImage(const char* filename, OpenFlags open_flags, Common::Error* error)
{
  std::unique_ptr<CDImageCHD> image = std::make_unique<CDImageCHD>(open_flags);
  if (!image->Open(filename, error))
    return {};

  return image;
}
