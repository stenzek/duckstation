#pragma once
#include "bitfield.h"
#include "types.h"
#include <memory>
#include <tuple>
#include <vector>

class ByteStream;

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
    FRAMES_PER_SECOND = 75, // "sectors"
    SECONDS_PER_MINUTE = 60,
    FRAMES_PER_MINUTE = FRAMES_PER_SECOND * SECONDS_PER_MINUTE
  };

  enum class ReadMode : u32
  {
    DataOnly,  // 2048 bytes per sector.
    RawSector, // 2352 bytes per sector.
    RawNoSync, // 2340 bytes per sector.
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
      return Position{BCDToDecimal(minute), BCDToDecimal(second), BCDToDecimal(frame)};
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

    Position operator+(const Position& rhs) { return FromLBA(ToLBA() + rhs.ToLBA()); }
    Position& operator+=(const Position& pos)
    {
      *this = *this + pos;
      return *this;
    }

#define RELATIONAL_OPERATOR(op)                                                                                        \
  bool operator op (const Position& rhs) const                                                                         \
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

  // Opening disc image.
  static std::unique_ptr<CDImage> Open(const char* filename);
  static std::unique_ptr<CDImage> OpenBinImage(const char* filename);
  static std::unique_ptr<CDImage> OpenCueSheetImage(const char* filename);

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

  // Seek to data LBA.
  bool Seek(LBA lba);

  // Seek to disc position (MSF).
  bool Seek(const Position& pos);

  // Seek to track and position.
  bool Seek(u32 track_number, const Position& pos_in_track);

  // Read from the current LBA. Returns the number of sectors read.
  u32 Read(ReadMode read_mode, u32 sector_count, void* buffer);

  // Read a single raw sector from the current LBA.
  bool ReadRawSector(void* buffer);

protected:
  struct Track
  {
    u32 track_number;
    LBA start_lba;
    u32 first_index;
    u32 length;
  };

  struct Index
  {
    u64 file_offset;
    std::FILE* file;
    u32 file_sector_size;
    LBA start_lba_on_disc;
    u32 track_number;
    u32 index_number;
    LBA start_lba_in_track;
    u32 length;
    bool is_pregap;
  };

  const Index* GetIndexForDiscPosition(LBA pos);
  const Index* GetIndexForTrackPosition(u32 track_number, LBA track_pos);

  std::string m_filename;
  u32 m_lba_count = 0;

  std::vector<Track> m_tracks;
  std::vector<Index> m_indices;

  // Position on disc.
  LBA m_position_on_disc = 0;

  // Position in track/index.
  const Index* m_current_index = nullptr;
  LBA m_position_in_index = 0;
  LBA m_position_in_track = 0;
};
