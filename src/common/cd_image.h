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
