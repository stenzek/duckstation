// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "types.h"
#include "util/cd_image.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

class ProgressCallback;

class CDROMAsyncReader
{
public:
  struct ReadResult
  {
    CDImage::LBA lba = 0;
    CDImage::Sector sector;
    bool result = false;
  };

  CDROMAsyncReader();
  ~CDROMAsyncReader();

  u32 GetBufferedSectorCount() const;
  bool HasBufferedSectors() const;
  u32 GetReadaheadCount() const;

  bool HasMedia() const { return static_cast<bool>(m_media); }
  const CDImage* GetMedia() const { return m_media.get(); }
  CDImage* GetMedia() { return m_media.get(); }
  const std::string& GetMediaPath() const { return m_media->GetPath(); }

  bool IsUsingThread() const { return m_read_thread.joinable(); }
  void StartThread(u32 readahead_count = 8);
  void StopThread();

  void SetMedia(std::unique_ptr<CDImage> media);
  std::unique_ptr<CDImage> RemoveMedia();

  /// Precaches image, either to memory, or using the underlying image precache.
  bool Precache(ProgressCallback* callback, Error* error);

  void QueueReadSector(CDImage::LBA lba);

  /// Returns a borrowed result which remains valid until ReleaseSector() is called.
  const ReadResult& WaitForReadToComplete();
  void ReleaseSector();
  void WaitForIdle();

  /// Bypasses the sector cache and reads directly from the image.
  bool ReadSectorUncached(CDImage::LBA lba, CDImage::Sector* sector,
                          CDImage::SectorReadMode mode = CDImage::SectorReadMode::DataAndSubQ);

private:
  u32 GetCurrentBufferLocked() const;
  u32 GetBufferedSectorCountLocked() const;
  void UpdatePublishedCacheStateLocked();
  void EmptyBuffersLocked();
  bool ReadSectorBatch(std::unique_lock<std::mutex>& lock);
  void ReadSectorNonThreaded(CDImage::LBA lba);
  bool InternalReadSectorUncached(CDImage::LBA lba, CDImage::Sector* sector, CDImage::SectorReadMode mode);
  void CancelReadaheadLocked(std::unique_lock<std::mutex>& lock);

  void WorkerThreadEntryPoint();

  std::unique_ptr<CDImage> m_media;

  // Protects ring topology, request transitions, and condition-variable predicates. It is never held while accessing
  // the CDImage backend; m_is_reading reserves exclusive backend access across those unlocked operations.
  mutable std::mutex m_mutex;
  std::thread m_read_thread;
  std::condition_variable m_do_read_cv;
  std::condition_variable m_notify_read_complete_cv;

  std::optional<CDImage::LBA> m_next_position;
  CDImage::LBA m_next_read_lba = 0;
  u64 m_request_generation = 0;
  bool m_shutdown_flag = true;
  bool m_is_reading = false;
  bool m_can_readahead = false;

  // Published while m_mutex is held. An acquire load of m_buffered_sector_count makes the selected slot contents and
  // m_published_buffer visible to the lock-free cached read path.
  std::atomic<u32> m_published_buffer{0};
  std::atomic<u32> m_buffered_sector_count{0};
  std::atomic<u32> m_cached_behind_count{0};
  std::atomic_bool m_sector_borrowed{false};

  // Core-thread-owned pin for the returned reference. The worker can append or evict history while borrowed, but
  // preserves the physical slot selected by m_published_buffer until the core queues another position.
  u32 m_borrowed_buffer = 0;

  std::vector<ReadResult> m_buffers;
  // Worker-owned staging storage keeps the ring immutable while a batch read is in flight.
  std::vector<CDImage::Sector> m_read_buffer;
  u32 m_readahead_count = 0;
  u32 m_buffer_front = 0;
  u32 m_buffer_back = 0;
  u32 m_buffer_count = 0;
  u32 m_buffer_current_offset = 0;
};
