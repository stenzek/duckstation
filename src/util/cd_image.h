// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/bitfield.h"
#include "common/types.h"

#include <array>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

class Error;
class ProgressCallback;

class CDImage
{
public:
  CDImage();
  virtual ~CDImage();

  using LBA = u32;

  enum : u32
  {
    RAW_SECTOR_SIZE = 2352,
    DATA_SECTOR_SIZE = 2048,
    SECTOR_SYNC_SIZE = 12,
    SECTOR_HEADER_SIZE = 4,
    MODE1_HEADER_SIZE = 4,
    MODE2_HEADER_SIZE = 12,
    MODE2_DATA_SECTOR_SIZE = 2336, // header + edc
    FRAMES_PER_SECOND = 75,        // "sectors", or "timecode frames" (not "channel frames")
    SECONDS_PER_MINUTE = 60,
    FRAMES_PER_MINUTE = FRAMES_PER_SECOND * SECONDS_PER_MINUTE,
    SUBCHANNEL_BYTES_PER_FRAME = 12,
    LEAD_OUT_SECTOR_COUNT = 6750,
    ALL_SUBCODE_SIZE = 96,
    AUDIO_SAMPLE_RATE = 44100,
    AUDIO_CHANNELS = 2,
  };

  enum : u8
  {
    LEAD_OUT_TRACK_NUMBER = 0xAA
  };

  enum class TrackMode : u8
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

  enum class SubchannelMode : u8
  {
    None,           // no subcode data stored
    RawInterleaved, // raw interleaved 96 bytes per sector
    Raw,            // raw uninterleaved 96 bytes per sector
  };

  enum class PrecacheResult : u8
  {
    Unsupported,
    ReadError,
    Success,
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

    static Position FromBCD(u8 minute, u8 second, u8 frame);
    static Position FromLBA(LBA lba);

    LBA ToLBA() const;
    std::tuple<u8, u8, u8> ToBCD() const;

    Position operator+(const Position& rhs);
    Position& operator+=(const Position& pos);

    bool operator==(const Position& rhs) const;
    bool operator!=(const Position& rhs) const;
    bool operator<(const Position& rhs) const;
    bool operator<=(const Position& rhs) const;
    bool operator>(const Position& rhs) const;
    bool operator>=(const Position& rhs) const;
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

      Control() = default;

      Control(u8 bits_) : bits(bits_) {}
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

    Control GetControl() const { return Control(control_bits); }
    bool IsData() const { return GetControl().data; }

    bool IsCRCValid() const;
  };
  static_assert(sizeof(SubChannelQ) == SUBCHANNEL_BYTES_PER_FRAME, "SubChannelQ is correct size");

  struct Track
  {
    u32 track_number;
    LBA start_lba;
    u32 first_index;
    u32 length;
    TrackMode mode;
    SubchannelMode submode;
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
    SubchannelMode submode;
    SubChannelQ::Control control;
    bool is_pregap;
  };

  // Helper functions.
  static u32 GetBytesPerSector(TrackMode mode);
  static void DeinterleaveSubcode(const u8* subcode_in, u8* subcode_out);

  /// Returns a list of physical CD-ROM devices, .first being the device path, .second being the device name.
  static std::vector<std::pair<std::string, std::string>> GetDeviceList();

  /// Returns true if the specified filename is a CD-ROM device name.
  static bool IsDeviceName(const char* filename);

  /// Returns true if an overlayable patch file exists for the specified image path.
  static bool HasOverlayablePatch(const char* path);

  // Opening disc image.
  static std::unique_ptr<CDImage> Open(const char* path, bool allow_patches, Error* error);
  static std::unique_ptr<CDImage> OpenBinImage(const char* path, Error* error);
  static std::unique_ptr<CDImage> OpenCueSheetImage(const char* path, Error* error);
  static std::unique_ptr<CDImage> OpenCHDImage(const char* path, Error* error);
  static std::unique_ptr<CDImage> OpenMdsImage(const char* path, Error* error);
  static std::unique_ptr<CDImage> OpenPBPImage(const char* path, Error* error);
  static std::unique_ptr<CDImage> OpenCCDImage(const char* path, Error* error);
  static std::unique_ptr<CDImage> OpenM3uImage(const char* path, bool apply_patches, Error* error);
  static std::unique_ptr<CDImage> OpenDeviceImage(const char* path, Error* error);
  static std::unique_ptr<CDImage> CreateMemoryImage(CDImage* image, ProgressCallback* progress, Error* error);
  static std::unique_ptr<CDImage> OverlayPPFPatch(const char* path, std::unique_ptr<CDImage> parent_image,
                                                  Error* error);

  // Accessors.
  const std::string& GetPath() const { return m_filename; }
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

  // Seek to data LBA.
  bool Seek(LBA lba);

  // Seek to disc position (MSF).
  bool Seek(const Position& pos);

  // Seek to track and position.
  bool Seek(u32 track_number, const Position& pos_in_track);

  // Seek to track and LBA.
  bool Seek(u32 track_number, LBA lba);

  // Read a single raw sector, and subchannel from the current LBA.
  bool ReadRawSector(void* buffer, SubChannelQ* subq);

  /// Generates sub-channel Q given the specified position.
  bool GenerateSubChannelQ(SubChannelQ* subq, LBA lba) const;

  /// Generates sub-channel Q from the given index and index-offset.
  void GenerateSubChannelQ(SubChannelQ* subq, const Index& index, u32 index_offset) const;

  // Reads sub-channel Q for the specified index+LBA.
  virtual bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index);

  // Returns true if the image has replacement subchannel data.
  virtual bool HasSubchannelData() const;

  // Reads a single sector from an index.
  virtual bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) = 0;

  // Returns true if this image type has sub-images (e.g. m3u).
  virtual bool HasSubImages() const;

  // Returns the number of sub-images in this image, if the format supports multiple.
  virtual u32 GetSubImageCount() const;

  // Returns the current sub-image index, if any.
  virtual u32 GetCurrentSubImage() const;

  // Changes the current sub-image. If this fails, the image state is unchanged.
  virtual bool SwitchSubImage(u32 index, Error* error);

  // Retrieve sub-image metadata.
  virtual std::string GetSubImageTitle(u32 index) const;

  // Returns true if the source supports precaching, which may be more optimal than an in-memory copy.
  virtual PrecacheResult Precache(ProgressCallback* progress, Error* error);
  virtual bool IsPrecached() const;

  // Returns the size on disk of the image. This could be multiple files.
  // If this function returns -1, it means the size could not be computed.
  virtual s64 GetSizeOnDisk() const;

protected:
  void ClearTOC();
  void CopyTOC(const CDImage* image);

  const Index* GetIndexForDiscPosition(LBA pos) const;
  const Index* GetIndexForTrackPosition(u32 track_number, LBA track_pos) const;

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
};
