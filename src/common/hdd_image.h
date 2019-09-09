#pragma once
#include "YBaseLib/ByteStream.h"
#include "types.h"
#include <memory>
#include <string>
#include <vector>

class HDDImage
{
public:
  using SectorIndex = u32;

  static constexpr u32 InvalidSectorNumber = UINT32_C(0xFFFFFFFF);
  static constexpr u32 DefaultSectorSize = 4096;

  static std::unique_ptr<HDDImage> Create(const char* filename, u64 size_in_bytes, u32 sector_size = DefaultSectorSize);
  static std::unique_ptr<HDDImage> Open(const char* filename, u32 sector_size = DefaultSectorSize);

  ~HDDImage();

  const u64 GetImageSize() const { return m_image_size; }
  const u32 GetSectorSize() const { return m_sector_size; }
  const u32 GetSectorCount() const { return m_sector_count; }

  void Read(void* buffer, u64 offset, u32 size);
  void Write(const void* buffer, u64 offset, u32 size);

  /// Erases the current replay log, and replaces it with the log from the specified stream.
  bool LoadState(ByteStream* stream);

  /// Copies the current state of the replay log to the specified stream, so it can be restored later.
  bool SaveState(ByteStream* stream);

  /// Flushes any buffered sectors to the backing file/log.
  void Flush();

  /// Commits all changes made in the replay log to the base image.
  void CommitLog();

  /// Erases any changes made in the replay log, restoring the image to its base state.
  void RevertLog();

private:
  using LogSectorMap = std::vector<SectorIndex>;
  struct SectorBuffer
  {
    std::unique_ptr<byte[]> data;
    SectorIndex sector_number = InvalidSectorNumber;
    bool in_log = false;
    bool dirty = false;
  };

  HDDImage(const std::string filename, ByteStream* base_stream, ByteStream* log_stream, u64 size, u32 sector_size,
           u32 sector_count, u32 version_number, LogSectorMap log_sector_map);

  static ByteStream* CreateLogFile(const char* filename, bool truncate_existing, bool atomic_update, u64 image_size,
                                   u32 sector_size, u32& num_sectors, u32 version_number, LogSectorMap& sector_map);
  static ByteStream* OpenLogFile(const char* filename, u64 image_size, u32& sector_size, u32& num_sectors,
                                 u32& version_number, LogSectorMap& sector_map);

  // Returns the offset in the image (either log or base) for the specified sector.
  u64 GetFileOffset(SectorIndex sector_index) const
  {
    return static_cast<u64>(sector_index) * static_cast<u64>(m_sector_size);
  }

  // Returns whether the specified sector is in the log (true), or in the base image (false).
  bool IsSectorInLog(SectorIndex sector_index) const { return (m_log_sector_map[sector_index] != InvalidSectorNumber); }

  // Currently, we only have one sector open. But we could change this in the future.
  SectorBuffer& GetSector(SectorIndex sector_index);

  void LoadSector(SectorBuffer& buf, SectorIndex sector_index);
  void LoadSectorFromImage(SectorBuffer& buf, SectorIndex sector_index);
  void LoadSectorFromLog(SectorBuffer& buf, SectorIndex sector_index);
  void WriteSectorToLog(SectorBuffer& buf);
  void ReleaseSector(SectorBuffer& buf);
  void ReleaseAllSectors();

  std::string m_filename;

  ByteStream* m_base_stream;
  ByteStream* m_log_stream;

  u64 m_image_size;
  u32 m_sector_size;
  u32 m_sector_count;
  u32 m_version_number;

  LogSectorMap m_log_sector_map;

  SectorBuffer m_current_sector;
};
