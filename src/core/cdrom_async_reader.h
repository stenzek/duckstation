#pragma once
#include "util/cd_image.h"
#include "types.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <thread>

class ProgressCallback;

class CDROMAsyncReader
{
public:
  using SectorBuffer = std::array<u8, CDImage::RAW_SECTOR_SIZE>;

  struct BufferSlot
  {
    CDImage::LBA lba;
    SectorBuffer data;
    CDImage::SubChannelQ subq;
    bool result;
  };

  CDROMAsyncReader();
  ~CDROMAsyncReader();

  const CDImage::LBA GetLastReadSector() const { return m_buffers[m_buffer_front.load()].lba; }
  const SectorBuffer& GetSectorBuffer() const { return m_buffers[m_buffer_front.load()].data; }
  const CDImage::SubChannelQ& GetSectorSubQ() const { return m_buffers[m_buffer_front.load()].subq; }
  const u32 GetBufferedSectorCount() const { return m_buffer_count.load(); }
  const bool HasBufferedSectors() const { return (m_buffer_count.load() > 0); }
  const u32 GetReadaheadCount() const { return static_cast<u32>(m_buffers.size()); }

  const bool HasMedia() const { return static_cast<bool>(m_media); }
  const CDImage* GetMedia() const { return m_media.get(); }
  const std::string& GetMediaFileName() const { return m_media->GetFileName(); }

  bool IsUsingThread() const { return m_read_thread.joinable(); }
  void StartThread(u32 readahead_count = 8);
  void StopThread();

  void SetMedia(std::unique_ptr<CDImage> media);
  std::unique_ptr<CDImage> RemoveMedia();

  /// Precaches image, either to memory, or using the underlying image precache.
  bool Precache(ProgressCallback* callback);

  void QueueReadSector(CDImage::LBA lba);

  bool WaitForReadToComplete();
  void WaitForIdle();

  /// Bypasses the sector cache and reads directly from the image.
  bool ReadSectorUncached(CDImage::LBA lba, CDImage::SubChannelQ* subq, SectorBuffer* data);

private:
  void EmptyBuffers();
  bool ReadSectorIntoBuffer(std::unique_lock<std::mutex>& lock);
  void ReadSectorNonThreaded(CDImage::LBA lba);
  bool InternalReadSectorUncached(CDImage::LBA lba, CDImage::SubChannelQ* subq, SectorBuffer* data);
  void CancelReadahead();

  void WorkerThreadEntryPoint();

  std::unique_ptr<CDImage> m_media;

  std::mutex m_mutex;
  std::thread m_read_thread;
  std::condition_variable m_do_read_cv;
  std::condition_variable m_notify_read_complete_cv;

  std::atomic<CDImage::LBA> m_next_position{};
  std::atomic_bool m_next_position_set{false};
  std::atomic_bool m_shutdown_flag{true};

  std::atomic_bool m_is_reading{false};
  std::atomic_bool m_can_readahead{false};
  std::atomic_bool m_seek_error{false};

  std::vector<BufferSlot> m_buffers;
  std::atomic<u32> m_buffer_front{0};
  std::atomic<u32> m_buffer_back{0};
  std::atomic<u32> m_buffer_count{0};
};
