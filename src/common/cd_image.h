#pragma once
#include "bitfield.h"
#include "types.h"

class ByteStream;

class CDImage
{
public:
  CDImage();
  ~CDImage();

  enum : u32
  {
    RAW_SECTOR_SIZE = 2352,
    DATA_SECTOR_SIZE = 2048,
    SECTOR_SYNC_SIZE = 12,
    SECTOR_HEADER_SIZE = 4,
    SECTOR_XA_SUBHEADER_SIZE = 4,
    FRAMES_PER_SECOND = 75, // "sectors"
    SECONDS_PER_MINUTE = 60,
    FRAMES_PER_MINUTE = FRAMES_PER_SECOND * SECONDS_PER_MINUTE,
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

  struct XASubHeader
  {
    u8 file_number;
    u8 channel_number;
    union Submode
    {
      u8 bits;
      BitField<u8, bool, 0, 1> eor;
      BitField<u8, bool, 1, 1> video;
      BitField<u8, bool, 2, 1> audio;
      BitField<u8, bool, 3, 1> data;
      BitField<u8, bool, 4, 1> trigger;
      BitField<u8, bool, 5, 1> form2;
      BitField<u8, bool, 6, 1> realtime;
      BitField<u8, bool, 7, 1> eof;
    } submode;
    union Codinginfo
    {
      u8 bits;

      BitField<u8, u8, 0, 2> mono_stereo;
      BitField<u8, u8, 2, 2> sample_rate;
      BitField<u8, u8, 4, 2> bits_per_sample;
      BitField<u8, bool, 6, 1> emphasis;
    } codinginfo;
  };

  // Conversion helpers.
  static constexpr u64 MSFToLBA(u32 pregap_seconds, u32 minute, u32 second, u32 frame);
  static constexpr void LBAToMSF(u32 pregap_seconds, u64 lba, u32* minute, u32* second, u32* frame);

  // Accessors.
  u64 GetCurrentLBA() const { return m_current_lba; }
  u64 GetLBACount() const { return m_lba_count; }

  bool Open(const char* path);

  // Seek to data LBA.
  bool Seek(u64 lba);

  // Seek to audio timestamp (MSF).
  bool Seek(u32 minute, u32 second, u32 frame);

  // Seek and read at the same time.
  u32 Read(ReadMode read_mode, u64 lba, u32 sector_count, void* buffer);
  u32 Read(ReadMode read_mode, u32 minute, u32 second, u32 frame, u32 sector_count, void* buffer);

  // Read from the current LBA. Returns the number of sectors read.
  u32 Read(ReadMode read_mode, u32 sector_count, void* buffer);

private:
  // TODO: Multiple data files from cue sheet
  ByteStream* m_data_file = nullptr;

  // Pregap size.
  u32 m_pregap_seconds = 2;

  // Current LBA/total LBAs.
  u64 m_current_lba = 0;
  u64 m_lba_count = 0;
};
