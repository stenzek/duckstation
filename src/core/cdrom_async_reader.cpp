// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cdrom_async_reader.h"
#include "host.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/threading.h"
#include "common/timer.h"

LOG_CHANNEL(CDROMAsyncReader);

namespace CDROMAsyncReader {

static bool IsReadaheadEnabled();
static void EmptyBuffers();
static bool ReadSectorIntoBuffer(std::unique_lock<Threading::Mutex>& lock);
static void ReadSectorNonThreaded(CDImage::LBA lba);
static bool InternalReadSectorUncached(CDImage::LBA lba, CDImage::SubChannelQ* subq, SectorBuffer* data);
static void CancelReadahead();

static void WorkerThreadEntryPoint();

struct Locals
{
  std::unique_ptr<CDImage> media;

  std::atomic<CDImage::LBA> next_position{};
  std::atomic_bool next_position_set{false};
  std::atomic_bool shutdown_flag{false};
  std::atomic_bool readahead_enabled{false};

  std::atomic_bool is_reading{false};
  std::atomic_bool can_readahead{false};
  std::atomic_bool seek_error{false};

  std::atomic<u32> buffer_front{0};
  std::atomic<u32> buffer_back{0};
  std::atomic<u32> buffer_count{0};

  std::vector<BufferSlot> buffers;

  Threading::Mutex mutex;
  Threading::ConditionVariable do_read_cv;
  Threading::ConditionVariable notify_read_complete_cv;

  Threading::Thread read_thread;
};

ALIGN_TO_CACHE_LINE static Locals s_locals;

} // namespace CDROMAsyncReader

CDImage::LBA CDROMAsyncReader::GetLastReadSector()
{
  return s_locals.buffers[s_locals.buffer_front.load(std::memory_order_acquire)].lba;
}

const CDROMAsyncReader::SectorBuffer& CDROMAsyncReader::GetSectorBuffer()
{
  return s_locals.buffers[s_locals.buffer_front.load(std::memory_order_acquire)].data;
}

const CDImage::SubChannelQ& CDROMAsyncReader::GetSectorSubQ()
{
  return s_locals.buffers[s_locals.buffer_front.load(std::memory_order_acquire)].subq;
}

u32 CDROMAsyncReader::GetBufferedSectorCount()
{
  return s_locals.buffer_count.load(std::memory_order_acquire);
}

bool CDROMAsyncReader::HasBufferedSectors()
{
  return (s_locals.buffer_count.load(std::memory_order_acquire) > 0);
}

u32 CDROMAsyncReader::GetReadaheadCount()
{
  return s_locals.readahead_enabled.load(std::memory_order_relaxed) ? static_cast<u32>(s_locals.buffers.size()) : 0;
}

bool CDROMAsyncReader::HasMedia()
{
  return static_cast<bool>(s_locals.media);
}

CDImage* CDROMAsyncReader::GetMedia()
{
  return s_locals.media.get();
}

const std::string& CDROMAsyncReader::GetMediaPath()
{
  return s_locals.media->GetPath();
}

bool CDROMAsyncReader::ProcessStartup(Error* error)
{
  if (!s_locals.read_thread.Start(&CDROMAsyncReader::WorkerThreadEntryPoint))
  {
    Error::SetStringView(error, "Failed to start CDROMAsyncReader thread");
    return false;
  }

  return true;
}

void CDROMAsyncReader::ProcessShutdown()
{
  {
    std::lock_guard lock(s_locals.mutex);
    s_locals.shutdown_flag.store(true, std::memory_order_release);
    s_locals.do_read_cv.notify_one();
  }

  s_locals.read_thread.Join();
}

void CDROMAsyncReader::SetReadaheadSectors(u32 readahead_sectors)
{
  CancelReadahead();
  EmptyBuffers();
  s_locals.buffers.clear();
  s_locals.buffers.resize(std::max(readahead_sectors, 1u));
  s_locals.readahead_enabled.store(readahead_sectors >= 1, std::memory_order_relaxed);
  DEV_LOG("Readahead set to {} sectors", readahead_sectors);
}

void CDROMAsyncReader::SetMedia(std::unique_ptr<CDImage> media)
{
  CancelReadahead();
  s_locals.media = std::move(media);
}

std::unique_ptr<CDImage> CDROMAsyncReader::RemoveMedia()
{
  CancelReadahead();
  return std::move(s_locals.media);
}

bool CDROMAsyncReader::Precache(ProgressCallback* callback, Error* error)
{
  WaitForIdle();

  std::unique_lock lock(s_locals.mutex);
  if (!s_locals.media)
    return false;
  else if (s_locals.media->IsPrecached())
    return true;

  const CDImage::PrecacheResult res = s_locals.media->Precache(callback, error);
  if (res == CDImage::PrecacheResult::Unsupported)
  {
    // fall back to copy precaching
    std::unique_ptr<CDImage> memory_image = CDImage::CreateMemoryImage(s_locals.media.get(), callback, error);
    if (memory_image)
    {
      const CDImage::LBA lba = s_locals.media->GetPositionOnDisc();
      if (!memory_image->Seek(lba)) [[unlikely]]
      {
        ERROR_LOG("Failed to seek to LBA {} in memory image", lba);
        return false;
      }

      s_locals.media.reset();
      s_locals.media = std::move(memory_image);
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
  if (!IsReadaheadEnabled())
  {
    ReadSectorNonThreaded(lba);
    return;
  }

  const u32 buffer_count = s_locals.buffer_count.load(std::memory_order_acquire);
  if (buffer_count > 0)
  {
    // don't re-read the same sector if it was the last one we read
    // the CDC code does this when seeking->reading
    const u32 buffer_front = s_locals.buffer_front.load(std::memory_order_acquire);
    if (s_locals.buffers[buffer_front].lba == lba)
    {
      DEBUG_LOG("Skipping re-reading same sector {}", lba);
      return;
    }

    // did we readahead to the correct sector?
    const u32 next_buffer = (buffer_front + 1) % static_cast<u32>(s_locals.buffers.size());
    if (s_locals.buffer_count > 1 && s_locals.buffers[next_buffer].lba == lba)
    {
      // great, don't need a seek, but still kick the thread to start reading ahead again
      DEBUG_LOG("Readahead buffer hit for sector {}", lba);
      std::unique_lock lock(s_locals.mutex);
      s_locals.buffer_front.store(next_buffer, std::memory_order_release);
      s_locals.buffer_count.fetch_sub(1, std::memory_order_release);
      s_locals.can_readahead.store(true, std::memory_order_release);
      s_locals.do_read_cv.notify_one();
      return;
    }
  }

  // we need to toss away our readahead and start fresh
  DEBUG_LOG("Readahead buffer miss, queueing seek to {}", lba);
  std::unique_lock lock(s_locals.mutex);
  s_locals.next_position.store(lba, std::memory_order_relaxed);
  s_locals.next_position_set.store(true, std::memory_order_release);
  s_locals.do_read_cv.notify_one();
}

bool CDROMAsyncReader::ReadSectorUncached(CDImage::LBA lba, CDImage::SubChannelQ* subq, SectorBuffer* data)
{
  if (!IsReadaheadEnabled())
    return InternalReadSectorUncached(lba, subq, data);

  std::unique_lock lock(s_locals.mutex);

  // wait until the read thread is idle
  s_locals.notify_read_complete_cv.wait(lock, []() { return !s_locals.is_reading.load(std::memory_order_acquire); });

  // read while the lock is held so it has to wait
  const CDImage::LBA prev_lba = s_locals.media->GetPositionOnDisc();
  const bool result = InternalReadSectorUncached(lba, subq, data);
  if (!s_locals.media->Seek(prev_lba)) [[unlikely]]
  {
    ERROR_LOG("Failed to re-seek to cached position {}", prev_lba);
    s_locals.can_readahead.store(false, std::memory_order_release);
  }

  return result;
}

bool CDROMAsyncReader::InternalReadSectorUncached(CDImage::LBA lba, CDImage::SubChannelQ* subq, SectorBuffer* data)
{
  if (s_locals.media->GetPositionOnDisc() != lba && !s_locals.media->Seek(lba)) [[unlikely]]
  {
    WARNING_LOG("Seek to LBA {} failed", lba);
    return false;
  }

  if (!s_locals.media->ReadRawSector(data, subq)) [[unlikely]]
  {
    WARNING_LOG("Read of LBA {} failed", lba);
    return false;
  }

  return true;
}

bool CDROMAsyncReader::WaitForReadToComplete()
{
  if (!s_locals.next_position_set.load(std::memory_order_acquire) &&
      s_locals.buffer_count.load(std::memory_order_acquire) > 0)
  {
    const u32 buffer_index = s_locals.buffer_front.load(std::memory_order_acquire);
    TRACE_LOG("Returning sector {}", s_locals.buffers[buffer_index].lba);
    return s_locals.buffers[buffer_index].result;
  }

  Timer wait_timer;
  DEBUG_LOG("Sector read pending, waiting");

  std::unique_lock lock(s_locals.mutex);
  s_locals.notify_read_complete_cv.wait(lock, []() {
    return (s_locals.buffer_count.load(std::memory_order_acquire) > 0 ||
            s_locals.seek_error.load(std::memory_order_acquire)) &&
           !s_locals.next_position_set.load(std::memory_order_acquire);
  });
  if (s_locals.seek_error.load(std::memory_order_acquire)) [[unlikely]]
  {
    s_locals.seek_error.store(false, std::memory_order_release);
    return false;
  }

  const u32 front = s_locals.buffer_front.load(std::memory_order_acquire);
  const double wait_time = wait_timer.GetTimeMilliseconds();
  if (wait_time > 1.0f) [[unlikely]]
    WARNING_LOG("Had to wait {:.2f} msec for LBA {}", wait_time, s_locals.buffers[front].lba);

  TRACE_LOG("Returning sector {} after waiting", s_locals.buffers[front].lba);
  return s_locals.buffers[front].result;
}

void CDROMAsyncReader::WaitForIdle()
{
  if (!IsReadaheadEnabled())
    return;

  std::unique_lock lock(s_locals.mutex);
  s_locals.notify_read_complete_cv.wait(lock, []() {
    return (!s_locals.is_reading.load(std::memory_order_acquire) &&
            !s_locals.next_position_set.load(std::memory_order_acquire));
  });
}

bool CDROMAsyncReader::IsReadaheadEnabled()
{
  // NOTE: Not called on worker thread.
  DebugAssert(Host::IsOnCoreThread());
  return s_locals.readahead_enabled.load(std::memory_order_relaxed);
}

void CDROMAsyncReader::EmptyBuffers()
{
  s_locals.buffer_front.store(0, std::memory_order_release);
  s_locals.buffer_back.store(0, std::memory_order_release);
  s_locals.buffer_count.store(0, std::memory_order_release);
}

bool CDROMAsyncReader::ReadSectorIntoBuffer(std::unique_lock<Threading::Mutex>& lock)
{
  Timer timer;

  const u32 slot = s_locals.buffer_back.load(std::memory_order_acquire);
  s_locals.buffer_back.store((slot + 1) % static_cast<u32>(s_locals.buffers.size()));

  BufferSlot& buffer = s_locals.buffers[slot];
  buffer.lba = s_locals.media->GetPositionOnDisc();
  s_locals.is_reading.store(true, std::memory_order_release);
  lock.unlock();

  TRACE_LOG("Reading LBA {}...", buffer.lba);

  const bool read_result = s_locals.media->ReadRawSector(buffer.data.data(), &buffer.subq);
  buffer.result = read_result;
  if (buffer.result) [[likely]]
  {
    const double read_time = timer.GetTimeMilliseconds();
    if (read_time > 1.0f) [[unlikely]]
      DEV_LOG("Read LBA {} took {:.2f} msec", buffer.lba, read_time);
  }
  else
  {
    ERROR_LOG("Read of LBA {} failed", buffer.lba);
  }

  lock.lock();
  s_locals.is_reading.store(false, std::memory_order_release);
  s_locals.buffer_count.fetch_add(1, std::memory_order_release);
  s_locals.notify_read_complete_cv.notify_all();
  return read_result;
}

void CDROMAsyncReader::ReadSectorNonThreaded(CDImage::LBA lba)
{
  Timer timer;

  Assert(!s_locals.buffers.empty());
  s_locals.seek_error.store(false, std::memory_order_release);
  EmptyBuffers();

  if (s_locals.media->GetPositionOnDisc() != lba && !s_locals.media->Seek(lba))
  {
    WARNING_LOG("Seek to LBA {} failed", lba);
    s_locals.seek_error.store(true, std::memory_order_release);
    return;
  }

  BufferSlot& buffer = s_locals.buffers.front();
  buffer.lba = s_locals.media->GetPositionOnDisc();

  TRACE_LOG("Reading LBA {}...", buffer.lba);

  buffer.result = s_locals.media->ReadRawSector(buffer.data.data(), &buffer.subq);
  if (buffer.result) [[likely]]
  {
    const double read_time = timer.GetTimeMilliseconds();
    if (read_time > 1.0f) [[unlikely]]
      DEV_LOG("Read LBA {} took {:.2f} msec", buffer.lba, read_time);
  }
  else
  {
    ERROR_LOG("Read of LBA {} failed", buffer.lba);
  }

  s_locals.buffer_count.fetch_add(1, std::memory_order_release);
}

void CDROMAsyncReader::CancelReadahead()
{
  DEV_LOG("Cancelling readahead");

  std::unique_lock lock(s_locals.mutex);

  // wait until the read thread is idle
  s_locals.notify_read_complete_cv.wait(lock, []() {
    return !s_locals.is_reading.load(std::memory_order_acquire) &&
           !s_locals.next_position_set.load(std::memory_order_acquire);
  });

  // prevent it from doing any more when it re-acquires the lock
  s_locals.can_readahead.store(false, std::memory_order_release);
  EmptyBuffers();
}

void CDROMAsyncReader::WorkerThreadEntryPoint()
{
  std::unique_lock lock(s_locals.mutex);

  for (;;)
  {
    s_locals.do_read_cv.wait(lock, []() {
      return (s_locals.shutdown_flag.load(std::memory_order_acquire) ||
              s_locals.next_position_set.load(std::memory_order_acquire) ||
              s_locals.can_readahead.load(std::memory_order_acquire));
    });
    if (s_locals.shutdown_flag.load(std::memory_order_relaxed))
      break;

    for (;;)
    {
      if (s_locals.next_position_set.load(std::memory_order_acquire))
      {
        // discard buffers, we're seeking to a new location
        const CDImage::LBA seek_location = s_locals.next_position.load(std::memory_order_relaxed);
        EmptyBuffers();
        s_locals.next_position_set.store(false, std::memory_order_release);
        s_locals.seek_error.store(false, std::memory_order_release);
        s_locals.is_reading.store(true, std::memory_order_release);
        lock.unlock();

        // seek without lock held in case it takes time
        DEBUG_LOG("Seeking to LBA {}...", seek_location);
        const bool seek_result =
          (s_locals.media->GetPositionOnDisc() == seek_location || s_locals.media->Seek(seek_location));

        lock.lock();
        s_locals.is_reading.store(false, std::memory_order_release);

        // did another request come in? abort if so
        if (s_locals.next_position_set.load(std::memory_order_acquire))
          continue;

        // did we fail the seek?
        if (!seek_result) [[unlikely]]
        {
          // add the error result, and don't try to read ahead
          WARNING_LOG("Seek to LBA {} failed", seek_location);
          s_locals.seek_error.store(true, std::memory_order_release);
          s_locals.notify_read_complete_cv.notify_all();
          break;
        }

        // go go read ahead!
        s_locals.can_readahead.store(true, std::memory_order_release);
      }

      if (!s_locals.can_readahead.load(std::memory_order_acquire))
        break;

      // readahead time! read as many sectors as we have space for
      DEBUG_LOG("Reading ahead {} sectors...",
                static_cast<u32>(s_locals.buffers.size()) - s_locals.buffer_count.load(std::memory_order_acquire));
      while (s_locals.buffer_count.load(std::memory_order_acquire) < static_cast<u32>(s_locals.buffers.size()))
      {
        if (s_locals.next_position_set.load(std::memory_order_acquire))
        {
          // a seek request came in while we're reading, so bail out
          break;
        }

        // stop reading if we hit the end or get an error
        if (!ReadSectorIntoBuffer(lock))
          break;
      }

      // readahead buffer is full or errored at this point
      s_locals.can_readahead.store(false, std::memory_order_release);
      break;
    }
  }
}
