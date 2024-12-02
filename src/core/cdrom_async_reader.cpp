// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cdrom_async_reader.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/timer.h"
LOG_CHANNEL(CDROMAsyncReader);

CDROMAsyncReader::CDROMAsyncReader() = default;

CDROMAsyncReader::~CDROMAsyncReader()
{
  StopThread();
}

void CDROMAsyncReader::StartThread(u32 readahead_count)
{
  if (IsUsingThread())
    StopThread();

  m_buffers.clear();
  m_buffers.resize(readahead_count);
  EmptyBuffers();

  m_shutdown_flag.store(false);
  m_read_thread = std::thread(&CDROMAsyncReader::WorkerThreadEntryPoint, this);
  INFO_LOG("Read thread started with readahead of {} sectors", readahead_count);
}

void CDROMAsyncReader::StopThread()
{
  if (!IsUsingThread())
    return;

  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_shutdown_flag.store(true);
    m_do_read_cv.notify_one();
  }

  m_read_thread.join();
  EmptyBuffers();
  m_buffers.clear();
}

void CDROMAsyncReader::SetMedia(std::unique_ptr<CDImage> media)
{
  if (IsUsingThread())
    CancelReadahead();

  m_media = std::move(media);
}

std::unique_ptr<CDImage> CDROMAsyncReader::RemoveMedia()
{
  if (IsUsingThread())
    CancelReadahead();

  return std::move(m_media);
}

bool CDROMAsyncReader::Precache(ProgressCallback* callback)
{
  WaitForIdle();

  std::unique_lock lock(m_mutex);
  if (!m_media)
    return false;
  else if (m_media->IsPrecached())
    return true;

  const CDImage::PrecacheResult res = m_media->Precache(callback);
  if (res == CDImage::PrecacheResult::Unsupported)
  {
    // fall back to copy precaching
    std::unique_ptr<CDImage> memory_image = CDImage::CreateMemoryImage(m_media.get(), callback);
    if (memory_image)
    {
      const CDImage::LBA lba = m_media->GetPositionOnDisc();
      if (!memory_image->Seek(lba)) [[unlikely]]
      {
        ERROR_LOG("Failed to seek to LBA {} in memory image", lba);
        return false;
      }

      m_media.reset();
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

  const u32 buffer_count = m_buffer_count.load();
  if (buffer_count > 0)
  {
    // don't re-read the same sector if it was the last one we read
    // the CDC code does this when seeking->reading
    const u32 buffer_front = m_buffer_front.load();
    if (m_buffers[buffer_front].lba == lba)
    {
      DEBUG_LOG("Skipping re-reading same sector {}", lba);
      return;
    }

    // did we readahead to the correct sector?
    const u32 next_buffer = (buffer_front + 1) % static_cast<u32>(m_buffers.size());
    if (m_buffer_count > 1 && m_buffers[next_buffer].lba == lba)
    {
      // great, don't need a seek, but still kick the thread to start reading ahead again
      DEBUG_LOG("Readahead buffer hit for sector {}", lba);
      m_buffer_front.store(next_buffer);
      m_buffer_count.fetch_sub(1);
      m_can_readahead.store(true);
      m_do_read_cv.notify_one();
      return;
    }
  }

  // we need to toss away our readahead and start fresh
  DEBUG_LOG("Readahead buffer miss, queueing seek to {}", lba);
  std::unique_lock<std::mutex> lock(m_mutex);
  m_next_position_set.store(true);
  m_next_position = lba;
  m_do_read_cv.notify_one();
}

bool CDROMAsyncReader::ReadSectorUncached(CDImage::LBA lba, CDImage::SubChannelQ* subq, SectorBuffer* data)
{
  if (!IsUsingThread())
    return InternalReadSectorUncached(lba, subq, data);

  std::unique_lock lock(m_mutex);

  // wait until the read thread is idle
  m_notify_read_complete_cv.wait(lock, [this]() { return !m_is_reading.load(); });

  // read while the lock is held so it has to wait
  const CDImage::LBA prev_lba = m_media->GetPositionOnDisc();
  const bool result = InternalReadSectorUncached(lba, subq, data);
  if (!m_media->Seek(prev_lba)) [[unlikely]]
  {
    ERROR_LOG("Failed to re-seek to cached position {}", prev_lba);
    m_can_readahead.store(false);
  }

  return result;
}

bool CDROMAsyncReader::InternalReadSectorUncached(CDImage::LBA lba, CDImage::SubChannelQ* subq, SectorBuffer* data)
{
  if (m_media->GetPositionOnDisc() != lba && !m_media->Seek(lba)) [[unlikely]]
  {
    WARNING_LOG("Seek to LBA {} failed", lba);
    return false;
  }

  if (!m_media->ReadRawSector(data, subq)) [[unlikely]]
  {
    WARNING_LOG("Read of LBA {} failed", lba);
    return false;
  }

  return true;
}

bool CDROMAsyncReader::WaitForReadToComplete()
{
  // Safe without locking with memory_order_seq_cst.
  if (!m_next_position_set.load() && m_buffer_count.load() > 0)
  {
    TRACE_LOG("Returning sector {}", m_buffers[m_buffer_front.load()].lba);
    return m_buffers[m_buffer_front.load()].result;
  }

  Timer wait_timer;
  DEBUG_LOG("Sector read pending, waiting");

  std::unique_lock<std::mutex> lock(m_mutex);
  m_notify_read_complete_cv.wait(
    lock, [this]() { return (m_buffer_count.load() > 0 || m_seek_error.load()) && !m_next_position_set.load(); });
  if (m_seek_error.load()) [[unlikely]]
  {
    m_seek_error.store(false);
    return false;
  }

  const u32 front = m_buffer_front.load();
  const double wait_time = wait_timer.GetTimeMilliseconds();
  if (wait_time > 1.0f) [[unlikely]]
    WARNING_LOG("Had to wait {:.2f} msec for LBA {}", wait_time, m_buffers[front].lba);

  TRACE_LOG("Returning sector {} after waiting", m_buffers[front].lba);
  return m_buffers[front].result;
}

void CDROMAsyncReader::WaitForIdle()
{
  if (!IsUsingThread())
    return;

  std::unique_lock<std::mutex> lock(m_mutex);
  m_notify_read_complete_cv.wait(lock, [this]() { return (!m_is_reading.load() && !m_next_position_set.load()); });
}

void CDROMAsyncReader::EmptyBuffers()
{
  m_buffer_front.store(0);
  m_buffer_back.store(0);
  m_buffer_count.store(0);
}

bool CDROMAsyncReader::ReadSectorIntoBuffer(std::unique_lock<std::mutex>& lock)
{
  Timer timer;

  const u32 slot = m_buffer_back.load();
  m_buffer_back.store((slot + 1) % static_cast<u32>(m_buffers.size()));

  BufferSlot& buffer = m_buffers[slot];
  buffer.lba = m_media->GetPositionOnDisc();
  m_is_reading.store(true);
  lock.unlock();

  TRACE_LOG("Reading LBA {}...", buffer.lba);

  buffer.result = m_media->ReadRawSector(buffer.data.data(), &buffer.subq);
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
  m_is_reading.store(false);
  m_buffer_count.fetch_add(1);
  m_notify_read_complete_cv.notify_all();
  return true;
}

void CDROMAsyncReader::ReadSectorNonThreaded(CDImage::LBA lba)
{
  Timer timer;

  m_buffers.resize(1);
  m_seek_error.store(false);
  EmptyBuffers();

  if (m_media->GetPositionOnDisc() != lba && !m_media->Seek(lba))
  {
    WARNING_LOG("Seek to LBA {} failed", lba);
    m_seek_error.store(true);
    return;
  }

  BufferSlot& buffer = m_buffers.front();
  buffer.lba = m_media->GetPositionOnDisc();

  TRACE_LOG("Reading LBA {}...", buffer.lba);

  buffer.result = m_media->ReadRawSector(buffer.data.data(), &buffer.subq);
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

  m_buffer_count.fetch_add(1);
}

void CDROMAsyncReader::CancelReadahead()
{
  DEV_LOG("Cancelling readahead");

  std::unique_lock lock(m_mutex);

  // wait until the read thread is idle
  m_notify_read_complete_cv.wait(lock, [this]() { return !m_is_reading.load(); });

  // prevent it from doing any more when it re-acquires the lock
  m_can_readahead.store(false);
  EmptyBuffers();
}

void CDROMAsyncReader::WorkerThreadEntryPoint()
{
  std::unique_lock lock(m_mutex);

  for (;;)
  {
    m_do_read_cv.wait(
      lock, [this]() { return (m_shutdown_flag.load() || m_next_position_set.load() || m_can_readahead.load()); });
    if (m_shutdown_flag.load())
      break;

    for (;;)
    {
      if (m_next_position_set.load())
      {
        // discard buffers, we're seeking to a new location
        const CDImage::LBA seek_location = m_next_position.load();
        EmptyBuffers();
        m_next_position_set.store(false);
        m_seek_error.store(false);
        m_is_reading.store(true);
        lock.unlock();

        // seek without lock held in case it takes time
        DEBUG_LOG("Seeking to LBA {}...", seek_location);
        const bool seek_result = (m_media->GetPositionOnDisc() == seek_location || m_media->Seek(seek_location));

        lock.lock();
        m_is_reading.store(false);

        // did another request come in? abort if so
        if (m_next_position_set.load())
          continue;

        // did we fail the seek?
        if (!seek_result) [[unlikely]]
        {
          // add the error result, and don't try to read ahead
          WARNING_LOG("Seek to LBA {} failed", seek_location);
          m_seek_error.store(true);
          m_notify_read_complete_cv.notify_all();
          break;
        }

        // go go read ahead!
        m_can_readahead.store(true);
      }

      if (!m_can_readahead.load())
        break;

      // readahead time! read as many sectors as we have space for
      DEBUG_LOG("Reading ahead {} sectors...", static_cast<u32>(m_buffers.size()) - m_buffer_count.load());
      while (m_buffer_count.load() < static_cast<u32>(m_buffers.size()))
      {
        if (m_next_position_set.load())
        {
          // a seek request came in while we're reading, so bail out
          break;
        }

        // stop reading if we hit the end or get an error
        if (!ReadSectorIntoBuffer(lock))
          break;
      }

      // readahead buffer is full or errored at this point
      m_can_readahead.store(false);
      break;
    }
  }
}
