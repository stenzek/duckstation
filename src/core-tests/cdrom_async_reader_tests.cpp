// SPDX-FileCopyrightText: 2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cdrom_async_reader.h"

#include "common/assert.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace {
class TestCDImage final : public CDImage
{
public:
  struct ReadCall
  {
    LBA lba;
    u32 count;
  };

  explicit TestCDImage(bool is_physical_device = true) : m_is_physical_device(is_physical_device)
  {
    Track track = {};
    track.track_number = 1;
    track.start_lba = 0;
    track.first_index = 0;
    track.length = 1000;
    track.mode = TrackMode::Audio;
    track.submode = SubchannelMode::None;
    track.control = SubChannelQ::Control(0);
    m_tracks.push_back(track);

    Index index = {};
    index.file_sector_size = RAW_SECTOR_SIZE;
    index.start_lba_on_disc = 0;
    index.track_number = 1;
    index.index_number = 1;
    index.length = track.length;
    index.mode = track.mode;
    index.submode = track.submode;
    index.control = track.control;
    m_indices.push_back(index);

    m_lba_count = track.length;
    AddLeadOutIndex();
  }

  void SetFirstFailedLBA(std::optional<LBA> lba) { m_first_failed_lba = lba; }

  bool IsPhysicalDevice() const override { return m_is_physical_device; }

  void BlockNextRead()
  {
    std::unique_lock lock(m_mutex);
    m_block_next_read = true;
    m_read_blocked = false;
  }

  void WaitUntilReadIsBlocked()
  {
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this]() { return m_read_blocked; });
  }

  void UnblockRead()
  {
    std::unique_lock lock(m_mutex);
    m_block_next_read = false;
    m_cv.notify_all();
  }

  std::vector<ReadCall> GetReadCalls() const
  {
    std::unique_lock lock(m_mutex);
    return m_read_calls;
  }

  u32 ReadSectorsFromIndex(std::span<Sector> sectors, const Index& index, LBA lba_in_index,
                           SectorReadMode mode) override
  {
    EXPECT_EQ(mode, SectorReadMode::DataAndSubQ);
    const LBA first_lba = index.start_lba_on_disc + lba_in_index;
    {
      std::unique_lock lock(m_mutex);
      m_read_calls.push_back({first_lba, static_cast<u32>(sectors.size())});
      if (m_block_next_read)
      {
        m_read_blocked = true;
        m_cv.notify_all();
        m_cv.wait(lock, [this]() { return !m_block_next_read; });
      }
    }

    u32 count = static_cast<u32>(sectors.size());
    if (m_first_failed_lba.has_value())
    {
      if (first_lba >= m_first_failed_lba.value())
        count = 0;
      else if ((first_lba + count) > m_first_failed_lba.value())
        count = m_first_failed_lba.value() - first_lba;
    }

    for (u32 i = 0; i < count; i++)
      sectors[i].data.fill(static_cast<u8>(first_lba + i));
    return count;
  }

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::vector<ReadCall> m_read_calls;
  std::optional<LBA> m_first_failed_lba;
  bool m_block_next_read = false;
  bool m_read_blocked = false;
  bool m_is_physical_device = true;
};

void ExpectAndRelease(CDROMAsyncReader& reader, CDImage::LBA expected_lba, bool expected_result = true)
{
  const CDROMAsyncReader::ReadResult& result = reader.WaitForReadToComplete();
  EXPECT_EQ(result.lba, expected_lba);
  EXPECT_EQ(result.result, expected_result);
  if (expected_result)
    EXPECT_EQ(result.sector.data.front(), static_cast<u8>(expected_lba));
  reader.ReleaseSector();
}
} // namespace

TEST(CDROMAsyncReader, ReadsAndRefillsInBatches)
{
  auto image = std::make_unique<TestCDImage>();
  TestCDImage* image_ptr = image.get();

  CDROMAsyncReader reader;
  reader.SetMedia(std::move(image));
  reader.StartThread(8);
  reader.QueueReadSector(100);
  reader.WaitForIdle();

  ASSERT_EQ(image_ptr->GetReadCalls().size(), 1u);
  EXPECT_EQ(image_ptr->GetReadCalls()[0].lba, 100u);
  EXPECT_EQ(image_ptr->GetReadCalls()[0].count, 8u);

  for (CDImage::LBA lba = 100; lba < 104; lba++)
  {
    ExpectAndRelease(reader, lba);
    reader.QueueReadSector(lba + 1);
  }
  reader.WaitForIdle();

  const std::vector<TestCDImage::ReadCall> calls = image_ptr->GetReadCalls();
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[1].lba, 108u);
  EXPECT_EQ(calls[1].count, 4u);
}

TEST(CDROMAsyncReader, FileImagesReadAndRefillIncrementally)
{
  auto image = std::make_unique<TestCDImage>(false);
  TestCDImage* image_ptr = image.get();

  CDROMAsyncReader reader;
  reader.SetMedia(std::move(image));
  reader.StartThread(8);
  reader.QueueReadSector(100);
  reader.WaitForIdle();

  // File images publish each sector independently so a slow decompression cannot hold the entire forward window.
  std::vector<TestCDImage::ReadCall> calls = image_ptr->GetReadCalls();
  ASSERT_EQ(calls.size(), 8u);
  for (u32 i = 0; i < calls.size(); i++)
  {
    EXPECT_EQ(calls[i].lba, 100u + i);
    EXPECT_EQ(calls[i].count, 1u);
  }

  // Replenish immediately after consuming one sector instead of waiting for half the window to become empty. This
  // preserves the old steady-state behavior when fast-forwarding while physical devices retain batched refills.
  ExpectAndRelease(reader, 100);
  reader.QueueReadSector(101);
  reader.WaitForIdle();

  calls = image_ptr->GetReadCalls();
  ASSERT_EQ(calls.size(), 9u);
  EXPECT_EQ(calls.back().lba, 108u);
  EXPECT_EQ(calls.back().count, 1u);
}

TEST(CDROMAsyncReader, RetainsConsumedSectorsAfterRefill)
{
  auto image = std::make_unique<TestCDImage>();
  TestCDImage* image_ptr = image.get();

  CDROMAsyncReader reader;
  reader.SetMedia(std::move(image));
  reader.StartThread(8);
  reader.QueueReadSector(5);
  reader.WaitForIdle();

  // Move four sectors forward, which triggers a refill of sectors 13 through 16.
  for (CDImage::LBA lba = 5; lba < 9; lba++)
  {
    ExpectAndRelease(reader, lba);
    reader.QueueReadSector(lba + 1);
  }
  reader.WaitForIdle();

  ASSERT_EQ(image_ptr->GetReadCalls().size(), 2u);
  EXPECT_EQ(image_ptr->GetReadCalls()[0].lba, 5u);
  EXPECT_EQ(image_ptr->GetReadCalls()[0].count, 8u);
  EXPECT_EQ(image_ptr->GetReadCalls()[1].lba, 13u);
  EXPECT_EQ(image_ptr->GetReadCalls()[1].count, 4u);

  // Sector 5 is behind the current position but still resident in the history half of the cache.
  reader.QueueReadSector(5);
  ExpectAndRelease(reader, 5);
  EXPECT_EQ(image_ptr->GetReadCalls().size(), 2u);
}

TEST(CDROMAsyncReader, PreservesPartialBatchBeforeError)
{
  auto image = std::make_unique<TestCDImage>();
  TestCDImage* image_ptr = image.get();
  image_ptr->SetFirstFailedLBA(103);

  CDROMAsyncReader reader;
  reader.SetMedia(std::move(image));
  reader.StartThread(8);
  reader.QueueReadSector(100);
  reader.WaitForIdle();

  for (CDImage::LBA lba = 100; lba < 103; lba++)
  {
    ExpectAndRelease(reader, lba);
    reader.QueueReadSector(lba + 1);
  }
  ExpectAndRelease(reader, 103, false);
  reader.WaitForIdle();

  // The queued error terminates this readahead run; consuming its successful prefix must not retry the same failure.
  EXPECT_EQ(image_ptr->GetReadCalls().size(), 1u);
}

TEST(CDROMAsyncReader, UncachedSubQOnlySkipsGeneratedDataRead)
{
  auto image = std::make_unique<TestCDImage>();
  TestCDImage* image_ptr = image.get();

  CDROMAsyncReader reader;
  reader.SetMedia(std::move(image));

  CDImage::Sector sector;
  sector.data.fill(0x5A);
  ASSERT_TRUE(reader.ReadSectorUncached(100, &sector, CDImage::SectorReadMode::SubQOnly));
  EXPECT_TRUE(image_ptr->GetReadCalls().empty());
  EXPECT_TRUE(sector.subq.IsCRCValid());
  EXPECT_TRUE(std::all_of(sector.data.begin(), sector.data.end(), [](u8 value) { return value == 0x5A; }));
}

TEST(CDROMAsyncReader, CachedSectorRemainsAccessibleDuringBatchRead)
{
  auto image = std::make_unique<TestCDImage>();
  TestCDImage* image_ptr = image.get();

  CDROMAsyncReader reader;
  reader.SetMedia(std::move(image));
  reader.StartThread(4);
  reader.QueueReadSector(100);
  reader.WaitForIdle();

  ExpectAndRelease(reader, 100);
  reader.QueueReadSector(101);
  ExpectAndRelease(reader, 101);

  // Advancing to sector 102 makes the four-sector ring half empty and starts a two-sector refill. Keep that backend
  // call blocked while another thread exercises the same borrow path used by the emulation thread.
  image_ptr->BlockNextRead();
  reader.QueueReadSector(102);
  image_ptr->WaitUntilReadIsBlocked();

  std::future<std::pair<CDImage::LBA, bool>> cached_result =
    std::async(std::launch::async, [&reader]() -> std::pair<CDImage::LBA, bool> {
      const CDROMAsyncReader::ReadResult& result = reader.WaitForReadToComplete();
      const std::pair<CDImage::LBA, bool> copy = {result.lba, result.result};
      reader.ReleaseSector();
      return copy;
    });

  const std::future_status cached_status = cached_result.wait_for(std::chrono::seconds(1));
  image_ptr->UnblockRead();
  ASSERT_EQ(cached_status, std::future_status::ready) << "Cached-sector access waited for the in-flight backend read";

  const auto [lba, result] = cached_result.get();
  EXPECT_EQ(lba, 102u);
  EXPECT_TRUE(result);
}

TEST(CDROMAsyncReader, CachedSectorRemainsAccessibleDuringUncachedRead)
{
  auto image = std::make_unique<TestCDImage>();
  TestCDImage* image_ptr = image.get();

  CDROMAsyncReader reader;
  reader.SetMedia(std::move(image));
  reader.StartThread(4);
  reader.QueueReadSector(100);
  reader.WaitForIdle();

  // An uncached read needs exclusive use of the stateful CDImage backend, but it must not retain the state mutex
  // while waiting for the device. Cached-sector access should remain entirely independent of that slow operation.
  image_ptr->BlockNextRead();
  CDImage::Sector uncached_sector;
  std::future<bool> uncached_result = std::async(
    std::launch::async, [&reader, &uncached_sector]() { return reader.ReadSectorUncached(200, &uncached_sector); });
  image_ptr->WaitUntilReadIsBlocked();

  std::future<std::pair<CDImage::LBA, bool>> cached_result =
    std::async(std::launch::async, [&reader]() -> std::pair<CDImage::LBA, bool> {
      const CDROMAsyncReader::ReadResult& result = reader.WaitForReadToComplete();
      const std::pair<CDImage::LBA, bool> copy = {result.lba, result.result};
      reader.ReleaseSector();
      return copy;
    });

  const std::future_status cached_status = cached_result.wait_for(std::chrono::seconds(1));
  image_ptr->UnblockRead();
  ASSERT_EQ(cached_status, std::future_status::ready) << "Cached-sector access waited for the uncached backend read";

  const auto [lba, result] = cached_result.get();
  EXPECT_EQ(lba, 100u);
  EXPECT_TRUE(result);
  EXPECT_TRUE(uncached_result.get());
  EXPECT_EQ(uncached_sector.data.front(), static_cast<u8>(200));
}

TEST(CDROMAsyncReader, BorrowedSectorRemainsStableDuringRefillPublication)
{
  auto image = std::make_unique<TestCDImage>();
  TestCDImage* image_ptr = image.get();

  CDROMAsyncReader reader;
  reader.SetMedia(std::move(image));
  reader.StartThread(4);
  reader.QueueReadSector(100);
  reader.WaitForIdle();

  ExpectAndRelease(reader, 100);
  reader.QueueReadSector(101);
  ExpectAndRelease(reader, 101);

  image_ptr->BlockNextRead();
  reader.QueueReadSector(102);
  image_ptr->WaitUntilReadIsBlocked();

  // Keep the reference borrowed while the worker appends its completed batch. Publication may update atomic cache
  // metadata, but must not relocate or overwrite the selected ring slot until the emulation thread releases it.
  const CDROMAsyncReader::ReadResult& borrowed_result = reader.WaitForReadToComplete();
  ASSERT_EQ(borrowed_result.lba, 102u);
  ASSERT_TRUE(borrowed_result.result);
  ASSERT_EQ(borrowed_result.sector.data.front(), static_cast<u8>(102));

  image_ptr->UnblockRead();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (reader.GetBufferedSectorCount() < 4 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  const bool refill_published = (reader.GetBufferedSectorCount() == 4);
  EXPECT_EQ(borrowed_result.lba, 102u);
  EXPECT_TRUE(borrowed_result.result);
  EXPECT_EQ(borrowed_result.sector.data.front(), static_cast<u8>(102));
  reader.ReleaseSector();

  ASSERT_TRUE(refill_published) << "Worker did not publish the completed refill while a cached sector was borrowed";
  reader.WaitForIdle();
}

TEST(CDROMAsyncReader, BackwardHitDuringRefillDoesNotEvictCurrentSector)
{
  auto image = std::make_unique<TestCDImage>();
  TestCDImage* image_ptr = image.get();

  CDROMAsyncReader reader;
  reader.SetMedia(std::move(image));
  reader.StartThread(4);
  reader.QueueReadSector(0);
  reader.WaitForIdle();

  // Fill both halves of the cache, leaving sectors 0 through 4 behind the current position and 5 through 7 ahead.
  for (CDImage::LBA lba = 0; lba < 5; lba++)
  {
    ExpectAndRelease(reader, lba);
    reader.QueueReadSector(lba + 1);
    reader.WaitForIdle();
  }

  ExpectAndRelease(reader, 5);
  image_ptr->BlockNextRead();
  reader.QueueReadSector(6);
  image_ptr->WaitUntilReadIsBlocked();

  // The new position needs slots which the in-flight batch intended to reclaim. The batch must be discarded rather
  // than evicting sector 0 out from under the cache hit.
  reader.QueueReadSector(0);
  image_ptr->UnblockRead();
  reader.WaitForIdle();
  ExpectAndRelease(reader, 0);

  const std::vector<TestCDImage::ReadCall> calls = image_ptr->GetReadCalls();
  ASSERT_FALSE(calls.empty());
  EXPECT_EQ(calls.back().lba, 8u);
}

TEST(CDROMAsyncReader, DiscardsStaleInFlightBatch)
{
  auto image = std::make_unique<TestCDImage>();
  TestCDImage* image_ptr = image.get();
  image_ptr->BlockNextRead();

  CDROMAsyncReader reader;
  reader.SetMedia(std::move(image));
  reader.StartThread(4);
  reader.QueueReadSector(100);
  image_ptr->WaitUntilReadIsBlocked();

  // Queueing a new location changes the request generation while the old device/image request is still in flight.
  reader.QueueReadSector(200);
  image_ptr->UnblockRead();
  reader.WaitForIdle();

  ExpectAndRelease(reader, 200);
  const std::vector<TestCDImage::ReadCall> calls = image_ptr->GetReadCalls();
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].lba, 100u);
  EXPECT_EQ(calls[1].lba, 200u);
}
