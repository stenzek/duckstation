#pragma once
#include "common/cd_image.h"
#include "types.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <thread>

class CDROMAsyncReader
{
public:
  using SectorBuffer = std::array<u8, CDImage::RAW_SECTOR_SIZE>;

  CDROMAsyncReader();
  ~CDROMAsyncReader();

  const CDImage::LBA GetLastReadSector() const { return m_last_read_sector; }
  const SectorBuffer& GetSectorBuffer() const { return m_sector_buffer; }
  const CDImage::SubChannelQ& GetSectorSubQ() const { return m_subq; }
  const bool HasMedia() const { return static_cast<bool>(m_media); }
  const CDImage* GetMedia() const { return m_media.get(); }
  const std::string GetMediaFileName() const { return m_media ? m_media->GetFileName() : std::string(); }

  bool IsUsingThread() const { return m_read_thread.joinable(); }
  void StartThread();
  void StopThread();

  void SetMedia(std::unique_ptr<CDImage> media);
  void RemoveMedia();

  void QueueReadSector(CDImage::LBA lba);
  void QueueReadNextSector();

  bool WaitForReadToComplete();

  /// Bypasses the sector cache and reads directly from the image.
  bool ReadSectorUncached(CDImage::LBA lba, CDImage::SubChannelQ* subq, SectorBuffer* data);

private:
  void DoSectorRead();
  void WorkerThreadEntryPoint();

  std::unique_ptr<CDImage> m_media;

  std::mutex m_mutex;
  std::thread m_read_thread;
  std::condition_variable m_do_read_cv;
  std::condition_variable m_notify_read_complete_cv;

  CDImage::LBA m_next_position{};
  std::atomic_bool m_next_position_set{false};
  std::atomic_bool m_sector_read_pending{false};
  std::atomic_bool m_shutdown_flag{true};

  CDImage::LBA m_last_read_sector{};
  CDImage::SubChannelQ m_subq{};
  SectorBuffer m_sector_buffer{};
  std::atomic_bool m_sector_read_result{false};
};
