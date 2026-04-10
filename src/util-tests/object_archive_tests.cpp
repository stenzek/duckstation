// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "util/object_archive.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <fmt/core.h>
#include <numeric>
#include <vector>

namespace {

/// RAII helper that creates a pair of temporary files suitable for use as ObjectArchive index/blob
/// files. The files are deleted when the helper goes out of scope.
class TempArchiveFiles
{
public:
  TempArchiveFiles()
  {
    const std::string base = Path::Combine(FileSystem::GetWorkingDirectory(), "duckstation_oa_test");
    m_index_file = FileSystem::OpenTemporaryCFile(base, &m_index_path);
    m_blob_file = FileSystem::OpenTemporaryCFile(base, &m_blob_path);
  }

  ~TempArchiveFiles()
  {
    if (m_index_file)
      std::fclose(m_index_file);
    if (m_blob_file)
      std::fclose(m_blob_file);
    if (!m_index_path.empty())
      FileSystem::DeleteFile(m_index_path.c_str());
    if (!m_blob_path.empty())
      FileSystem::DeleteFile(m_blob_path.c_str());
  }

  bool IsValid() const { return m_index_file != nullptr && m_blob_file != nullptr; }

  /// Releases ownership of the FILE pointers (caller takes ownership).
  std::pair<std::FILE*, std::FILE*> Release()
  {
    auto result = std::make_pair(m_index_file, m_blob_file);
    m_index_file = nullptr;
    m_blob_file = nullptr;
    return result;
  }

  /// Reopens the files for reading+writing (e.g. after an archive has closed them).
  bool Reopen()
  {
    m_index_file = FileSystem::OpenCFile(m_index_path.c_str(), "r+b");
    m_blob_file = FileSystem::OpenCFile(m_blob_path.c_str(), "r+b");
    return IsValid();
  }

  std::FILE* IndexFile() const { return m_index_file; }
  std::FILE* BlobFile() const { return m_blob_file; }
  const std::string& IndexPath() const { return m_index_path; }
  const std::string& BlobPath() const { return m_blob_path; }

private:
  std::FILE* m_index_file = nullptr;
  std::FILE* m_blob_file = nullptr;
  std::string m_index_path;
  std::string m_blob_path;
};

} // namespace

static constexpr u32 TEST_VERSION = 1;

// ---------------------------------------------------------------------------
// Basic lifecycle
// ---------------------------------------------------------------------------

TEST(ObjectArchive, CreateAndOpen)
{
  TempArchiveFiles files;
  ASSERT_TRUE(files.IsValid());

  ObjectArchive archive;
  auto [idx, blob] = files.Release();
  Error error;
  ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();
  EXPECT_TRUE(archive.IsOpen());
  EXPECT_EQ(archive.GetSize(), 0u);
}

TEST(ObjectArchive, InsertToClosedArchive)
{
  ObjectArchive archive;
  EXPECT_FALSE(archive.IsOpen());

  const u8 data[] = {1, 2, 3};
  Error error;
  EXPECT_FALSE(archive.Insert("key", data, sizeof(data), ObjectArchive::CompressType::Uncompressed, &error));
}

TEST(ObjectArchive, EmptyKeyRejected)
{
  TempArchiveFiles files;
  ASSERT_TRUE(files.IsValid());

  ObjectArchive archive;
  auto [idx, blob] = files.Release();
  Error error;
  ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();

  const u8 data[] = {0xAA};
  EXPECT_FALSE(archive.Insert("", std::span<const u8>(data), ObjectArchive::CompressType::Uncompressed, &error));
}

// ---------------------------------------------------------------------------
// Round-trip (uncompressed)
// ---------------------------------------------------------------------------

TEST(ObjectArchive, InsertAndLookupRoundTrip)
{
  TempArchiveFiles files;
  ASSERT_TRUE(files.IsValid());

  ObjectArchive archive;
  auto [idx, blob] = files.Release();
  Error error;
  ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();

  const u8 payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
  ASSERT_TRUE(
    archive.Insert("test_key", payload, sizeof(payload), ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();

  auto result = archive.Lookup("test_key", &error);
  ASSERT_TRUE(result.has_value()) << error.GetDescription();
  ASSERT_EQ(result->size(), sizeof(payload));
  EXPECT_EQ(std::memcmp(result->data(), payload, sizeof(payload)), 0);
}

// ---------------------------------------------------------------------------
// Round-trip (compressed)
// ---------------------------------------------------------------------------

TEST(ObjectArchive, InsertAndLookupCompressed)
{
  TempArchiveFiles files;
  ASSERT_TRUE(files.IsValid());

  ObjectArchive archive;
  auto [idx, blob] = files.Release();
  Error error;
  ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();

  // Create a payload large enough for compression to be meaningful.
  std::vector<u8> payload(4096);
  for (size_t i = 0; i < payload.size(); i++)
    payload[i] = static_cast<u8>(i & 0xFF);

  ASSERT_TRUE(archive.Insert("compressed_key", std::span<const u8>(payload),
                              ObjectArchive::CompressType::Zstandard, &error))
    << error.GetDescription();

  auto result = archive.Lookup("compressed_key", &error);
  ASSERT_TRUE(result.has_value()) << error.GetDescription();
  ASSERT_EQ(result->size(), payload.size());
  EXPECT_EQ(std::memcmp(result->data(), payload.data(), payload.size()), 0);
}

// ---------------------------------------------------------------------------
// Duplicate key rejection
// ---------------------------------------------------------------------------

TEST(ObjectArchive, DuplicateKeyRejected)
{
  TempArchiveFiles files;
  ASSERT_TRUE(files.IsValid());

  ObjectArchive archive;
  auto [idx, blob] = files.Release();
  Error error;
  ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();

  const u8 data1[] = {1};
  const u8 data2[] = {2};
  ASSERT_TRUE(
    archive.Insert("dup", std::span<const u8>(data1), ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();
  EXPECT_FALSE(
    archive.Insert("dup", std::span<const u8>(data2), ObjectArchive::CompressType::Uncompressed, &error));
}

// ---------------------------------------------------------------------------
// Missing key
// ---------------------------------------------------------------------------

TEST(ObjectArchive, MissingKeyReturnsNullopt)
{
  TempArchiveFiles files;
  ASSERT_TRUE(files.IsValid());

  ObjectArchive archive;
  auto [idx, blob] = files.Release();
  Error error;
  ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();

  // Insert one key so the index is non-empty.
  const u8 data[] = {0x42};
  ASSERT_TRUE(
    archive.Insert("exists", std::span<const u8>(data), ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();

  auto result = archive.Lookup("does_not_exist", &error);
  EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Multiple keys with correct isolation
// ---------------------------------------------------------------------------

TEST(ObjectArchive, MultipleKeysCorrectIsolation)
{
  TempArchiveFiles files;
  ASSERT_TRUE(files.IsValid());

  ObjectArchive archive;
  auto [idx, blob] = files.Release();
  Error error;
  ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();

  const u8 a_data[] = {0xAA};
  const u8 m_data[] = {0xBB, 0xCC};
  const u8 z_data[] = {0xDD, 0xEE, 0xFF};

  ASSERT_TRUE(
    archive.Insert("aaa", std::span<const u8>(a_data), ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();
  ASSERT_TRUE(
    archive.Insert("mmm", std::span<const u8>(m_data), ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();
  ASSERT_TRUE(
    archive.Insert("zzz", std::span<const u8>(z_data), ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();

  auto ra = archive.Lookup("aaa", &error);
  ASSERT_TRUE(ra.has_value()) << error.GetDescription();
  ASSERT_EQ(ra->size(), sizeof(a_data));
  EXPECT_EQ((*ra)[0], 0xAA);

  auto rm = archive.Lookup("mmm", &error);
  ASSERT_TRUE(rm.has_value()) << error.GetDescription();
  ASSERT_EQ(rm->size(), sizeof(m_data));
  EXPECT_EQ((*rm)[0], 0xBB);
  EXPECT_EQ((*rm)[1], 0xCC);

  auto rz = archive.Lookup("zzz", &error);
  ASSERT_TRUE(rz.has_value()) << error.GetDescription();
  ASSERT_EQ(rz->size(), sizeof(z_data));
  EXPECT_EQ((*rz)[0], 0xDD);
}

// ---------------------------------------------------------------------------
// Clear and re-insert
// ---------------------------------------------------------------------------

TEST(ObjectArchive, ClearAndReinsert)
{
  TempArchiveFiles files;
  ASSERT_TRUE(files.IsValid());

  ObjectArchive archive;
  auto [idx, blob] = files.Release();
  Error error;
  ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();

  const u8 data1[] = {0x11, 0x22};
  ASSERT_TRUE(
    archive.Insert("key1", std::span<const u8>(data1), ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();
  EXPECT_EQ(archive.GetSize(), 1u);

  ASSERT_TRUE(archive.Clear(&error)) << error.GetDescription();
  EXPECT_EQ(archive.GetSize(), 0u);

  // After clear, lookup should fail.
  auto result = archive.Lookup("key1", &error);
  EXPECT_FALSE(result.has_value());

  // Re-insertion should succeed.
  const u8 data2[] = {0x33, 0x44, 0x55};
  ASSERT_TRUE(
    archive.Insert("key2", std::span<const u8>(data2), ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();
  EXPECT_EQ(archive.GetSize(), 1u);

  auto result2 = archive.Lookup("key2", &error);
  ASSERT_TRUE(result2.has_value()) << error.GetDescription();
  ASSERT_EQ(result2->size(), sizeof(data2));
  EXPECT_EQ(std::memcmp(result2->data(), data2, sizeof(data2)), 0);
}

// ---------------------------------------------------------------------------
// Close and reopen (persistence)
// ---------------------------------------------------------------------------

TEST(ObjectArchive, CloseAndReopenPersistence)
{
  TempArchiveFiles files;
  ASSERT_TRUE(files.IsValid());

  const u8 payload[] = {0xCA, 0xFE, 0xBA, 0xBE};

  // Create and insert.
  {
    ObjectArchive archive;
    auto [idx, blob] = files.Release();
    Error error;
    ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();
    ASSERT_TRUE(archive.Insert("persist", std::span<const u8>(payload),
                                ObjectArchive::CompressType::Uncompressed, &error))
      << error.GetDescription();
    archive.Close();
  }

  // Reopen and verify.
  ASSERT_TRUE(files.Reopen());
  {
    ObjectArchive archive;
    auto [idx, blob] = files.Release();
    Error error;
    ASSERT_TRUE(archive.OpenFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();
    EXPECT_EQ(archive.GetSize(), 1u);

    auto result = archive.Lookup("persist", &error);
    ASSERT_TRUE(result.has_value()) << error.GetDescription();
    ASSERT_EQ(result->size(), sizeof(payload));
    EXPECT_EQ(std::memcmp(result->data(), payload, sizeof(payload)), 0);
  }
}

// ---------------------------------------------------------------------------
// Version mismatch: open with wrong version, then create fresh
// ---------------------------------------------------------------------------

TEST(ObjectArchive, VersionMismatchCreatesEmpty)
{
  TempArchiveFiles files;
  ASSERT_TRUE(files.IsValid());

  // Create an archive at version 1 with some data.
  {
    ObjectArchive archive;
    auto [idx, blob] = files.Release();
    Error error;
    ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();

    const u8 data[] = {0x01, 0x02};
    ASSERT_TRUE(
      archive.Insert("v1_key", std::span<const u8>(data), ObjectArchive::CompressType::Uncompressed, &error))
      << error.GetDescription();
    archive.Close();
  }

  // Attempt to open with a different version — should fail.
  ASSERT_TRUE(files.Reopen());
  {
    ObjectArchive archive;
    auto [idx, blob] = files.Release();
    Error error;
    EXPECT_FALSE(archive.OpenFile(idx, blob, TEST_VERSION + 1, &error));
    EXPECT_FALSE(archive.IsOpen());
  }

  // Now create a fresh archive at the new version — should be empty.
  ASSERT_TRUE(files.Reopen());
  {
    ObjectArchive archive;
    auto [idx, blob] = files.Release();
    Error error;
    ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION + 1, &error)) << error.GetDescription();
    EXPECT_TRUE(archive.IsOpen());
    EXPECT_EQ(archive.GetSize(), 0u);
  }
}

// ---------------------------------------------------------------------------
// Large number of objects inserted and looked up in unsorted order
// ---------------------------------------------------------------------------

TEST(ObjectArchive, LargeNumberOfObjectsUnsorted)
{
  TempArchiveFiles files;
  ASSERT_TRUE(files.IsValid());

  ObjectArchive archive;
  auto [idx, blob] = files.Release();
  Error error;
  ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();

  static constexpr size_t NUM_OBJECTS = 200;

  // Build keys in a deliberately unsorted order by shuffling indices.
  std::vector<size_t> order(NUM_OBJECTS);
  std::iota(order.begin(), order.end(), 0u);

  // Simple deterministic shuffle (swap i with i*7+3 mod N).
  for (size_t i = 0; i < NUM_OBJECTS; i++)
  {
    const size_t j = (i * 7 + 3) % NUM_OBJECTS;
    std::swap(order[i], order[j]);
  }

  // Insert in shuffled order.
  for (const size_t i : order)
  {
    const std::string key = fmt::format("object_{:04}", i);
    // Each object's payload is 8 bytes encoding its index.
    u8 payload[8];
    std::memset(payload, 0, sizeof(payload));
    std::memcpy(payload, &i, sizeof(i));

    ASSERT_TRUE(archive.Insert(key, payload, sizeof(payload), ObjectArchive::CompressType::Uncompressed, &error))
      << "Failed to insert key '" << key << "': " << error.GetDescription();
  }

  EXPECT_EQ(archive.GetSize(), NUM_OBJECTS);

  // Look up every object in a different shuffled order.
  std::vector<size_t> lookup_order(NUM_OBJECTS);
  std::iota(lookup_order.begin(), lookup_order.end(), 0u);
  for (size_t i = 0; i < NUM_OBJECTS; i++)
  {
    const size_t j = (i * 13 + 7) % NUM_OBJECTS;
    std::swap(lookup_order[i], lookup_order[j]);
  }

  for (const size_t i : lookup_order)
  {
    const std::string key = fmt::format("object_{:04}", i);
    auto result = archive.Lookup(key, &error);
    ASSERT_TRUE(result.has_value()) << "Lookup failed for key '" << key << "': " << error.GetDescription();
    ASSERT_EQ(result->size(), 8u);

    size_t stored_index = 0;
    std::memcpy(&stored_index, result->data(), sizeof(stored_index));
    EXPECT_EQ(stored_index, i) << "Data mismatch for key '" << key << "'";
  }
}
