#pragma once
#include "bitfield.h"
#include "progress_callback.h"
#include "types.h"
#include <array>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace Common {
class Error;
}

class CDImage
{
public:
  enum class OpenFlags : u8
  {
    None = 0,
    PreCache = (1 << 0), // Pre-cache image to RAM, if supported.
  };

  CDImage(OpenFlags open_flags);
  virtual ~CDImage();

  using LBA = u32;

  enum : u32
  {
    RAW_SECTOR_SIZE = 2352,
    DATA_SECTOR_SIZE = 2048,
    SECTOR_SYNC_SIZE = 12,
    SECTOR_HEADER_SIZE = 4,
    FRAMES_PER_SECOND = 75, // "sectors", or "timecode frames" (not "channel frames")
    SECONDS_PER_MINUTE = 60,
    FRAMES_PER_MINUTE = FRAMES_PER_SECOND * SECONDS_PER_MINUTE,
    SUBCHANNEL_BYTES_PER_FRAME = 12,
    LEAD_OUT_SECTOR_COUNT = 6750
  };

  enum : u8
  {
    LEAD_OUT_TRACK_NUMBER = 0xAA
  };

  enum class ReadMode : u32
  {
    DataOnly,  // 2048 bytes per sector.
    RawSector, // 2352 bytes per sector.
    RawNoSync, // 2340 bytes per sector.
  };

  enum class TrackMode : u32
  {
    Audio,        // 2352 bytes per sector
    Mode1,        // 2048 bytes per sector
    Mode1Raw,     // 2352 bytes per sector
    Mode2,        // 2336 bytes per sector
    Mode2Form1,   // 2048 bytes per sector
    Mode2Form2,   // 2324 bytes per sector
    Mode2FormMix, // 2332 bytes per sector
    Mode2Raw      // 2352 bytes per sector
  };

  struct SectorHeader
  {
    u8 minute;
    u8 second;
    u8 frame;
    u8 sector_mode;
  };

  struct Position
  {
    u8 minute;
    u8 second;
    u8 frame;

    static constexpr Position FromBCD(u8 minute, u8 second, u8 frame)
    {
      return Position{PackedBCDToBinary(minute), PackedBCDToBinary(second), PackedBCDToBinary(frame)};
    }

    static constexpr Position FromLBA(LBA lba)
    {
      const u8 frame = Truncate8(lba % FRAMES_PER_SECOND);
      lba /= FRAMES_PER_SECOND;

      const u8 second = Truncate8(lba % SECONDS_PER_MINUTE);
      lba /= SECONDS_PER_MINUTE;

      const u8 minute = Truncate8(lba);

      return Position{minute, second, frame};
    }

    LBA ToLBA() const
    {
      return ZeroExtend32(minute) * FRAMES_PER_MINUTE + ZeroExtend32(second) * FRAMES_PER_SECOND + ZeroExtend32(frame);
    }

    constexpr std::tuple<u8, u8, u8> ToBCD() const
    {
      return std::make_tuple<u8, u8, u8>(BinaryToBCD(minute), BinaryToBCD(second), BinaryToBCD(frame));
    }

    Position operator+(const Position& rhs) { return FromLBA(ToLBA() + rhs.ToLBA()); }
    Position& operator+=(const Position& pos)
    {
      *this = *this + pos;
      return *this;
    }

#define RELATIONAL_OPERATOR(op)                                                                                        \
  bool operator op(const Position& rhs) const                                                                          \
  {                                                                                                                    \
    return std::tie(minute, second, frame) op std::tie(rhs.minute, rhs.second, rhs.frame);                             \
  }

    RELATIONAL_OPERATOR(==);
    RELATIONAL_OPERATOR(!=);
    RELATIONAL_OPERATOR(<);
    RELATIONAL_OPERATOR(<=);
    RELATIONAL_OPERATOR(>);
    RELATIONAL_OPERATOR(>=);

#undef RELATIONAL_OPERATOR
  };

  union SubChannelQ
  {
    using Data = std::array<u8, SUBCHANNEL_BYTES_PER_FRAME>;

    union Control
    {
      u8 bits;

      BitField<u8, u8, 0, 4> adr;
      BitField<u8, bool, 4, 1> audio_preemphasis;
      BitField<u8, bool, 5, 1> digital_copy_permitted;
      BitField<u8, bool, 6, 1> data;
      BitField<u8, bool, 7, 1> four_channel_audio;

      Control& operator=(const Control& rhs)
      {
        bits = rhs.bits;
        return *this;
      }
    };

    struct
    {
      u8 control_bits;
      u8 track_number_bcd;
      u8 index_number_bcd;
      u8 relative_minute_bcd;
      u8 relative_second_bcd;
      u8 relative_frame_bcd;
      u8 reserved;
      u8 absolute_minute_bcd;
      u8 absolute_second_bcd;
      u8 absolute_frame_bcd;
      u16 crc;
    };

    Data data;

    static u16 ComputeCRC(const Data& data);

    Control GetControl() const { return Control{control_bits}; }
    bool IsData() const { return GetControl().data; }

    bool IsCRCValid() const;

    SubChannelQ& operator=(const SubChannelQ& q)
    {
      data = q.data;
      return *this;
    }
  };
  static_assert(sizeof(SubChannelQ) == SUBCHANNEL_BYTES_PER_FRAME, "SubChannelQ is correct size");

  struct Track
  {
    u32 track_number;
    LBA start_lba;
    u32 first_index;
    u32 length;
    TrackMode mode;
    SubChannelQ::Control control;
  };

  struct Index
  {
    u64 file_offset;
    u32 file_index;
    u32 file_sector_size;
    LBA start_lba_on_disc;
    u32 track_number;
    u32 index_number;
    LBA start_lba_in_track;
    u32 length;
    TrackMode mode;
    SubChannelQ::Control control;
    bool is_pregap;
  };

  // Helper functions.
  static u32 GetBytesPerSector(TrackMode mode);

  /// Returns a list of physical CD-ROM devices, .first being the device path, .second being the device name.
  static std::vector<std::pair<std::string, std::string>> GetDeviceList();

  /// Returns true if the specified filename is a CD-ROM device name.
  static bool IsDeviceName(const char* filename);

  // Opening disc image.
  static std::unique_ptr<CDImage> Open(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenBinImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenCueSheetImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenCHDImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenEcmImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenMdsImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenPBPImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenM3uImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenDeviceImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage>
  CreateMemoryImage(CDImage* image, ProgressCallback* progress = ProgressCallback::NullProgressCallback);
  static std::unique_ptr<CDImage> OverlayPPFPatch(const char* filename, OpenFlags open_flags,
                                                  std::unique_ptr<CDImage> parent_image,
                                                  ProgressCallback* progress = ProgressCallback::NullProgressCallback);

  // Accessors.
  const std::string& GetFileName() const { return m_filename; }
  LBA GetPositionOnDisc() const { return m_position_on_disc; }
  Position GetMSFPositionOnDisc() const { return Position::FromLBA(m_position_on_disc); }
  LBA GetPositionInTrack() const { return m_position_in_track; }
  Position GetMSFPositionInTrack() const { return Position::FromLBA(m_position_in_track); }
  LBA GetLBACount() const { return m_lba_count; }
  u32 GetIndexNumber() const { return m_current_index->index_number; }
  u32 GetTrackNumber() const { return m_current_index->track_number; }
  u32 GetTrackCount() const { return static_cast<u32>(m_tracks.size()); }
  LBA GetTrackStartPosition(u8 track) const;
  Position GetTrackStartMSFPosition(u8 track) const;
  LBA GetTrackLength(u8 track) const;
  Position GetTrackMSFLength(u8 track) const;
  TrackMode GetTrackMode(u8 track) const;
  LBA GetTrackIndexPosition(u8 track, u8 index) const;
  LBA GetTrackIndexLength(u8 track, u8 index) const;
  u32 GetFirstTrackNumber() const { return m_tracks.front().track_number; }
  u32 GetLastTrackNumber() const { return m_tracks.back().track_number; }
  u32 GetIndexCount() const { return static_cast<u32>(m_indices.size()); }
  const std::vector<Track>& GetTracks() const { return m_tracks; }
  const std::vector<Index>& GetIndices() const { return m_indices; }
  const Track& GetTrack(u32 track) const;
  const Index& GetIndex(u32 i) const;
  OpenFlags GetOpenFlags() const { return m_open_flags; }

  // Seek to data LBA.
  bool Seek(LBA lba);

  // Seek to disc position (MSF).
  bool Seek(const Position& pos);

  // Seek to track and position.
  bool Seek(u32 track_number, const Position& pos_in_track);

  // Seek to track and LBA.
  bool Seek(u32 track_number, LBA lba);

  // Read from the current LBA. Returns the number of sectors read.
  u32 Read(ReadMode read_mode, u32 sector_count, void* buffer);

  // Read a single raw sector, and subchannel from the current LBA.
  bool ReadRawSector(void* buffer, SubChannelQ* subq);

  // Reads sub-channel Q for the specified index+LBA.
  virtual bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index);

  // Returns true if the image has replacement subchannel data.
  virtual bool HasNonStandardSubchannel() const;

  // Reads a single sector from an index.
  virtual bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) = 0;

  // Retrieve image metadata.
  virtual std::string GetMetadata(const std::string_view& type) const;

  // Returns true if this image type has sub-images (e.g. m3u).
  virtual bool HasSubImages() const;

  // Returns the number of sub-images in this image, if the format supports multiple.
  virtual u32 GetSubImageCount() const;

  // Returns the current sub-image index, if any.
  virtual u32 GetCurrentSubImage() const;

  // Changes the current sub-image. If this fails, the image state is unchanged.
  virtual bool SwitchSubImage(u32 index, Common::Error* error);

  // Retrieve sub-image metadata.
  virtual std::string GetSubImageMetadata(u32 index, const std::string_view& type) const;

protected:
  void ClearTOC();
  void CopyTOC(const CDImage* image);

  const Index* GetIndexForDiscPosition(LBA pos);
  const Index* GetIndexForTrackPosition(u32 track_number, LBA track_pos);

  /// Generates sub-channel Q given the specified position.
  bool GenerateSubChannelQ(SubChannelQ* subq, LBA lba);

  /// Generates sub-channel Q from the given index and index-offset.
  void GenerateSubChannelQ(SubChannelQ* subq, const Index& index, u32 index_offset);

  /// Synthesis of lead-out data.
  void AddLeadOutIndex();

  std::string m_filename;
  u32 m_lba_count = 0;

  std::vector<Track> m_tracks;
  std::vector<Index> m_indices;

private:
  // Position on disc.
  LBA m_position_on_disc = 0;

  // Position in track/index.
  const Index* m_current_index = nullptr;
  LBA m_position_in_index = 0;
  LBA m_position_in_track = 0;

  OpenFlags m_open_flags;
};

IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(CDImage::OpenFlags);
