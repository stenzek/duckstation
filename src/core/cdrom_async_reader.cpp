// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cdrom_async_reader.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/timer.h"

#include <limits>

LOG_CHANNEL(CDROMAsyncReader);

CDROMAsyncReader::CDROMAsyncReader() = default;

CDROMAsyncReader::~CDROMAsyncReader()
{
  StopThread();
}

u32 CDROMAsyncReader::GetBufferedSectorCount() const
{
  return m_buffered_sector_count.load(std::memory_order_acquire);
}

bool CDROMAsyncReader::HasBufferedSectors() const
{
  return (m_buffered_sector_count.load(std::memory_order_acquire) > 0);
}

u32 CDROMAsyncReader::GetReadaheadCount() const
{
  std::unique_lock lock(m_mutex);
  return m_readahead_count;
}

void CDROMAsyncReader::StartThread(u32 readahead_count)
{
  Assert(readahead_count > 0);
  if (IsUsingThread())
    StopThread();

  {
    std::unique_lock lock(m_mutex);
    Assert(!m_sector_borrowed.load(std::memory_order_acquire));
    Assert(readahead_count <= (std::numeric_limits<u32>::max() / 2));
    m_readahead_count = readahead_count;
    m_buffers.clear();
    // Retain up to one readahead window behind the current sector in addition to the forward window.
    m_buffers.resize(static_cast<size_t>(readahead_count) * 2);
    // Allocate the worker's staging storage before the thread starts. Batch reads never allocate while holding the
    // state mutex, keeping contended sections bounded to ring bookkeeping and buffer copies.
    m_read_buffer.clear();
    m_read_buffer.resize(readahead_count);
    EmptyBuffersLocked();
    m_next_position.reset();
    m_can_readahead = false;
    m_shutdown_flag = false;
  }

  m_read_thread = std::thread(&CDROMAsyncReader::WorkerThreadEntryPoint, this);
  INFO_LOG("Read thread started with {} sectors of readahead and {} sectors of history", readahead_count,
           readahead_count);
}

void CDROMAsyncReader::StopThread()
{
  if (!IsUsingThread())
    return;

  {
    std::unique_lock lock(m_mutex);
    Assert(!m_sector_borrowed.load(std::memory_order_acquire));
    m_request_generation++;
    m_next_position.reset();
    m_can_readahead = false;
    m_shutdown_flag = true;
    m_do_read_cv.notify_one();
    m_notify_read_complete_cv.notify_all();
  }

  m_read_thread.join();

  std::unique_lock lock(m_mutex);
  EmptyBuffersLocked();
  m_buffers.clear();
  m_read_buffer.clear();
  m_readahead_count = 0;
}

void CDROMAsyncReader::SetMedia(std::unique_ptr<CDImage> media)
{
  std::unique_lock lock(m_mutex);
  Assert(!m_sector_borrowed.load(std::memory_order_acquire));
  CancelReadaheadLocked(lock);
  m_media = std::move(media);
}

std::unique_ptr<CDImage> CDROMAsyncReader::RemoveMedia()
{
  std::unique_lock lock(m_mutex);
  Assert(!m_sector_borrowed.load(std::memory_order_acquire));
  CancelReadaheadLocked(lock);
  return std::move(m_media);
}

bool CDROMAsyncReader::Precache(ProgressCallback* callback, Error* error)
{
  std::unique_lock lock(m_mutex);
  Assert(!m_sector_borrowed.load(std::memory_order_acquire));
  CancelReadaheadLocked(lock);

  if (!m_media)
    return false;
  else if (m_media->IsPrecached())
    return true;

  const CDImage::PrecacheResult res = m_media->Precache(callback, error);
  if (res == CDImage::PrecacheResult::Unsupported)
  {
    // Fall back to copy precaching.
    std::unique_ptr<CDImage> memory_image = CDImage::CreateMemoryImage(m_media.get(), callback, error);
    if (memory_image)
    {
      m_media = std::move(memory_image);
      return true;
    }
    else
    {
      return false;
    }
  }

  return (res == CDImage::PrecacheResult::Success);
}

void CDROMAsyncReader::QueueReadSector(CDImage::LBA lba)
{
  if (!IsUsingThread())
  {
    ReadSectorNonThreaded(lba);
    return;
  }

  std::unique_lock lock(m_mutex);
  Assert(m_media && !m_buffers.empty());
  Assert(!m_sector_borrowed.load(std::memory_order_acquire));

  if (m_buffer_count > 0)
  {
    const u32 current_buffer = GetCurrentBufferLocked();

    // Don't re-read the same sector if it was the last one we read.
    // The CDC code does this when seeking->reading.
    if (m_buffers[current_buffer].lba == lba)
    {
      DEBUG_LOG("Skipping re-reading same sector {}", lba);
      return;
    }

    // Search the whole cache, including sectors behind the current position. Forward hits avoid seeks when the
    // emulated drive skips sectors, while history hits avoid physical reads when pause/position simulation moves
    // the drive back to a recently consumed sector.
    for (u32 offset = 0; offset < m_buffer_count; offset++)
    {
      const u32 buffer_index = (m_buffer_front + offset) % static_cast<u32>(m_buffers.size());
      if (m_buffers[buffer_index].lba == lba)
      {
        [[maybe_unused]] const bool history_hit = (offset < m_buffer_current_offset);
        m_buffer_current_offset = offset;
        const u32 forward_count = GetBufferedSectorCountLocked();
        UpdatePublishedCacheStateLocked();
        DEBUG_LOG("Sector cache {} hit for LBA {} ({} cached behind, {} buffered forward)",
                  history_hit ? "history" : "readahead", lba, m_buffer_current_offset, forward_count);

        // Physical devices refill half a window at a time to avoid a command (and potentially a full rotation) per
        // sector. Image files refill immediately and publish each sector as it completes, which keeps decompression
        // latency off the emulation thread and gives the worker a chance to observe a new request between sectors.
        const bool batch_readahead = m_media->IsPhysicalDevice();
        const u32 refill_threshold = m_readahead_count / 2;
        const u32 last_buffer = (m_buffer_front + m_buffer_count - 1) % static_cast<u32>(m_buffers.size());
        const bool error_queued = !m_buffers[last_buffer].result;
        const bool refill_needed =
          batch_readahead ? (forward_count <= refill_threshold) : (forward_count < m_readahead_count);
        m_can_readahead = (m_buffers[buffer_index].result && !error_queued && refill_needed);
        if (m_can_readahead)
          m_do_read_cv.notify_one();
        else if (error_queued)
          TRACE_LOG("Not refilling after sector {} because an error is already queued at LBA {}", lba,
                    m_buffers[last_buffer].lba);
        return;
      }
    }
  }

  // We need to toss away our readahead and start fresh.
  DEBUG_LOG("Sector cache miss, queueing read at {} (discarding {} cached sectors)", lba, m_buffer_count);
  m_request_generation++;
  m_next_position = lba;
  m_can_readahead = false;
  // Invalidate the lock-free fast path before returning. The worker will initialize the ring for this request, but a
  // caller must not borrow a sector left over from the previous position in the meantime.
  EmptyBuffersLocked();
  m_do_read_cv.notify_one();
  m_notify_read_complete_cv.notify_all();
}

bool CDROMAsyncReader::ReadSectorUncached(CDImage::LBA lba, CDImage::Sector* sector, CDImage::SectorReadMode mode)
{
  std::unique_lock lock(m_mutex);
  Assert(!m_sector_borrowed.load(std::memory_order_acquire));

  // Reserve exclusive access to the stateful CDImage backend. Keep the reservation in m_is_reading, but release the
  // state mutex so cache publication and cached reads are not blocked by the I/O itself.
  m_notify_read_complete_cv.wait(lock, [this]() { return !m_is_reading; });
  m_is_reading = true;
  lock.unlock();

  const bool result = InternalReadSectorUncached(lba, sector, mode);

  lock.lock();
  m_is_reading = false;
  m_notify_read_complete_cv.notify_all();
  return result;
}

bool CDROMAsyncReader::InternalReadSectorUncached(CDImage::LBA lba, CDImage::Sector* sector,
                                                  CDImage::SectorReadMode mode)
{
  if (!m_media || m_media->ReadSectors(lba, std::span<CDImage::Sector>(sector, 1), mode) != 1) [[unlikely]]
  {
    WARNING_LOG("Read of LBA {} failed", lba);
    return false;
  }

  return true;
}

const CDROMAsyncReader::ReadResult& CDROMAsyncReader::WaitForReadToComplete()
{
  Timer wait_timer;

  // Publication stores the slot index before the forward count with release semantics. Once the count is non-zero,
  // the current result is fully initialized and the worker will not overwrite that selected slot. QueueReadSector()
  // is not legal while a result is borrowed, so pinning it here keeps the returned reference valid without locking.
  if (m_buffered_sector_count.load(std::memory_order_acquire) > 0)
  {
    const bool was_borrowed = m_sector_borrowed.exchange(true, std::memory_order_acq_rel);
    Assert(!was_borrowed);
    m_borrowed_buffer = m_published_buffer.load(std::memory_order_relaxed);

    const ReadResult& result = m_buffers[m_borrowed_buffer];
    TRACE_LOG("Borrowing cached sector {} ({} cached behind, {} buffered forward)", result.lba,
              m_cached_behind_count.load(std::memory_order_relaxed),
              m_buffered_sector_count.load(std::memory_order_relaxed));
    return result;
  }

  std::unique_lock lock(m_mutex);
  Assert(!m_sector_borrowed.load(std::memory_order_acquire));

  if (m_buffer_count == 0 || m_next_position.has_value())
    DEBUG_LOG("Sector read pending, waiting");

  m_notify_read_complete_cv.wait(lock, [this]() { return (m_buffer_count > 0 && !m_next_position.has_value()); });

  m_borrowed_buffer = GetCurrentBufferLocked();
  m_sector_borrowed.store(true, std::memory_order_release);
  const ReadResult& result = m_buffers[m_borrowed_buffer];
  const double wait_time = wait_timer.GetTimeMilliseconds();
  if (wait_time > 1.0f) [[unlikely]]
    WARNING_LOG("Had to wait {:.2f} msec for LBA {}", wait_time, result.lba);

  TRACE_LOG("Borrowing sector {} ({} cached behind, {} buffered forward)", result.lba, m_buffer_current_offset,
            GetBufferedSectorCountLocked());
  return result;
}

void CDROMAsyncReader::ReleaseSector()
{
  Assert(m_sector_borrowed.load(std::memory_order_acquire));
  TRACE_LOG("Releasing sector {}", m_buffers[m_borrowed_buffer].lba);
  m_sector_borrowed.store(false, std::memory_order_release);
}

void CDROMAsyncReader::WaitForIdle()
{
  if (!IsUsingThread())
    return;

  std::unique_lock lock(m_mutex);
  Assert(!m_sector_borrowed.load(std::memory_order_acquire));
  m_notify_read_complete_cv.wait(
    lock, [this]() { return (!m_is_reading && !m_next_position.has_value() && !m_can_readahead); });
}

u32 CDROMAsyncReader::GetCurrentBufferLocked() const
{
  Assert(m_buffer_count > 0 && m_buffer_current_offset < m_buffer_count);
  return (m_buffer_front + m_buffer_current_offset) % static_cast<u32>(m_buffers.size());
}

u32 CDROMAsyncReader::GetBufferedSectorCountLocked() const
{
  Assert(m_buffer_current_offset <= m_buffer_count);
  return m_buffer_count - m_buffer_current_offset;
}

void CDROMAsyncReader::UpdatePublishedCacheStateLocked()
{
  if (m_buffer_count > 0)
  {
    m_published_buffer.store(GetCurrentBufferLocked(), std::memory_order_relaxed);
    m_cached_behind_count.store(m_buffer_current_offset, std::memory_order_relaxed);
    // This release publishes both the selected index and all writes to the selected ReadResult.
    m_buffered_sector_count.store(GetBufferedSectorCountLocked(), std::memory_order_release);
  }
  else
  {
    // Clear availability first. The other published values are irrelevant until a later non-zero release store.
    m_buffered_sector_count.store(0, std::memory_order_release);
    m_published_buffer.store(0, std::memory_order_relaxed);
    m_cached_behind_count.store(0, std::memory_order_relaxed);
  }
}

void CDROMAsyncReader::EmptyBuffersLocked()
{
  Assert(!m_sector_borrowed.load(std::memory_order_acquire));
  m_buffer_front = 0;
  m_buffer_back = 0;
  m_buffer_count = 0;
  m_buffer_current_offset = 0;
  UpdatePublishedCacheStateLocked();
}

bool CDROMAsyncReader::ReadSectorBatch(std::unique_lock<std::mutex>& lock)
{
  Timer timer;

  // An uncached read or precache operation may have reserved the backend while leaving the state mutex unlocked.
  m_notify_read_complete_cv.wait(
    lock, [this]() { return (!m_is_reading || m_shutdown_flag || m_next_position.has_value()); });
  if (m_shutdown_flag || m_next_position.has_value())
    return false;

  const u32 forward_count = GetBufferedSectorCountLocked();
  Assert(forward_count < m_readahead_count);
  const u32 forward_slots = m_readahead_count - forward_count;
  const u32 sectors_to_read = m_media->IsPhysicalDevice() ? forward_slots : 1;
  Assert(sectors_to_read > 0);

  const u64 generation = m_request_generation;
  const CDImage::LBA first_lba = m_next_read_lba;
  m_is_reading = true;

  // Never hold the state mutex while accessing the image. Physical-device reads can take a full rotation, while
  // the emulation thread must remain able to borrow and release sectors which are already in the ring.
  lock.unlock();

  TRACE_LOG("Reading {} sectors starting at LBA {}...", sectors_to_read, first_lba);
  const u32 sectors_read = m_media->ReadSectors(
    first_lba, std::span<CDImage::Sector>(m_read_buffer).first(sectors_to_read), CDImage::SectorReadMode::DataAndSubQ);

  lock.lock();
  m_is_reading = false;
  m_notify_read_complete_cv.notify_all();

  // A newer request arrived while the lock was released. Its handler will reset the ring.
  if (generation != m_request_generation || m_shutdown_flag || m_next_position.has_value())
  {
    DEBUG_LOG("Discarding stale batch at LBA {} (request generation {} is now {})", first_lba, generation,
              m_request_generation);
    return false;
  }

  Assert(sectors_read <= sectors_to_read);

  // Make room only after the read completes, so history remains available to the emulation thread during slow
  // physical I/O. If it moved farther back while the mutex was released, the batch may no longer fit without
  // evicting its current sector; discard that now-unneeded readahead instead.
  const u32 results_to_queue = sectors_read + ((sectors_read < sectors_to_read) ? 1u : 0u);
  const u32 free_slots = static_cast<u32>(m_buffers.size()) - m_buffer_count;
  const u32 history_to_evict = (results_to_queue > free_slots) ? (results_to_queue - free_slots) : 0;
  if (history_to_evict > m_buffer_current_offset)
  {
    DEBUG_LOG("Discarding completed batch at LBA {} because the current cache position moved backward", first_lba);
    return false;
  }
  else if (history_to_evict > 0)
  {
    DEBUG_LOG("Evicting {} oldest cached sectors to append batch at LBA {}", history_to_evict, first_lba);
    m_buffer_front = (m_buffer_front + history_to_evict) % static_cast<u32>(m_buffers.size());
    m_buffer_count -= history_to_evict;
    m_buffer_current_offset -= history_to_evict;
  }

  for (u32 i = 0; i < sectors_read; i++)
  {
    ReadResult& result = m_buffers[m_buffer_back];
    result.lba = first_lba + i;
    result.sector = std::move(m_read_buffer[i]);
    result.result = true;
    m_buffer_back = (m_buffer_back + 1) % static_cast<u32>(m_buffers.size());
    m_buffer_count++;
  }

  m_next_read_lba = first_lba + sectors_read;
  if (sectors_read < sectors_to_read) [[unlikely]]
  {
    // Preserve the successful prefix, then queue a failure at the exact LBA which could not be read. Consumers can
    // process every completed sector before receiving the error, matching CDImage's partial-read contract.
    ReadResult& error_result = m_buffers[m_buffer_back];
    error_result.lba = m_next_read_lba;
    error_result.sector = {};
    error_result.result = false;
    m_buffer_back = (m_buffer_back + 1) % static_cast<u32>(m_buffers.size());
    m_buffer_count++;
    ERROR_LOG("Batch read at LBA {} returned {} of {} sectors; first failed LBA is {}", first_lba, sectors_read,
              sectors_to_read, error_result.lba);
  }

  UpdatePublishedCacheStateLocked();
  m_notify_read_complete_cv.notify_all();

  const double read_time = timer.GetTimeMilliseconds();
  if (read_time > 1.0f) [[unlikely]]
    DEV_LOG("Read {} of {} sectors at LBA {} in {:.2f} msec", sectors_read, sectors_to_read, first_lba, read_time);

  return (sectors_read == sectors_to_read);
}

void CDROMAsyncReader::ReadSectorNonThreaded(CDImage::LBA lba)
{
  Timer timer;
  Assert(!m_sector_borrowed.load(std::memory_order_acquire));

  // No worker exists in this mode, so read into a local result without holding the state mutex and publish it only
  // after the backend operation has completed.
  ReadResult new_result;
  new_result.lba = lba;

  TRACE_LOG("Reading LBA {}...", new_result.lba);
  new_result.result = InternalReadSectorUncached(lba, &new_result.sector, CDImage::SectorReadMode::DataAndSubQ);
  if (new_result.result) [[likely]]
  {
    const double read_time = timer.GetTimeMilliseconds();
    if (read_time > 1.0f) [[unlikely]]
      DEV_LOG("Read LBA {} took {:.2f} msec", new_result.lba, read_time);
  }
  else
  {
    ERROR_LOG("Read of LBA {} failed", new_result.lba);
  }

  std::unique_lock lock(m_mutex);
  Assert(!m_sector_borrowed.load(std::memory_order_acquire));
  m_buffers.resize(1);
  EmptyBuffersLocked();
  m_buffers.front() = std::move(new_result);
  m_buffer_count = 1;
  m_buffer_back = 0;
  UpdatePublishedCacheStateLocked();
}

void CDROMAsyncReader::CancelReadaheadLocked(std::unique_lock<std::mutex>& lock)
{
  DEV_LOG("Cancelling readahead ({} sectors cached, generation {})", m_buffer_count, m_request_generation);

  m_request_generation++;
  m_next_position.reset();
  m_can_readahead = false;

  // Wait until an in-flight read observes the generation change and becomes idle.
  m_notify_read_complete_cv.wait(lock, [this]() { return !m_is_reading; });
  EmptyBuffersLocked();
}

void CDROMAsyncReader::WorkerThreadEntryPoint()
{
  std::unique_lock lock(m_mutex);

  for (;;)
  {
    m_do_read_cv.wait(lock, [this]() { return (m_shutdown_flag || m_next_position.has_value() || m_can_readahead); });
    if (m_shutdown_flag)
      break;

    for (;;)
    {
      if (m_next_position.has_value())
      {
        // Discard buffers and start reading at the new location. CDImage reads use explicit LBAs, so this does not
        // need a separate blocking seek operation.
        const CDImage::LBA read_location = m_next_position.value();
        EmptyBuffersLocked();
        m_next_position.reset();
        m_next_read_lba = read_location;
        m_can_readahead = true;
      }

      if (!m_can_readahead)
        break;

      // Physical devices fill the available forward space in one request, while image files publish each sector as
      // it completes. The other half of the ring retains recently consumed sectors and is evicted only when a
      // completed read needs the space.
      DEBUG_LOG("Reading ahead {} sectors from LBA {} ({} cached behind, {} buffered forward)",
                m_readahead_count - GetBufferedSectorCountLocked(), m_next_read_lba, m_buffer_current_offset,
                GetBufferedSectorCountLocked());
      while (m_can_readahead && GetBufferedSectorCountLocked() < m_readahead_count)
      {
        if (m_next_position.has_value())
        {
          // A new request came in while we were reading, so bail out and service it.
          break;
        }

        // Stop reading if we hit the end or get an error. A partial batch remains queued ahead of its error result.
        if (!ReadSectorBatch(lock))
          break;
      }

      if (m_next_position.has_value())
        continue;

      // The forward window is full, the request moved backward, or a read errored at this point.
      m_can_readahead = false;
      m_notify_read_complete_cv.notify_all();
      break;
    }
  }

  m_is_reading = false;
  m_can_readahead = false;
  m_notify_read_complete_cv.notify_all();
}
