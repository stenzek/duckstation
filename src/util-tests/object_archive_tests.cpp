// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "util/object_archive.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/types.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
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

class TempArchivePath
{
public:
  TempArchivePath()
  {
    const std::string base = Path::Combine(FileSystem::GetWorkingDirectory(), "duckstation_oa_path_test");
    std::FILE* fp = FileSystem::OpenTemporaryCFile(base, &m_base_path);
    if (fp)
    {
      std::fclose(fp);
      FileSystem::DeleteFile(m_base_path.c_str());
    }
  }

  ~TempArchivePath()
  {
    if (!m_base_path.empty())
    {
      FileSystem::DeleteFile(fmt::format("{}.idx", m_base_path).c_str());
      FileSystem::DeleteFile(fmt::format("{}.bin", m_base_path).c_str());
    }
  }

  bool IsValid() const { return !m_base_path.empty(); }
  const std::string& GetPath() const { return m_base_path; }

private:
  std::string m_base_path;
};

static ObjectArchive::KeySpan StringToCacheKey(std::string_view sv)
{
  return ObjectArchive::KeySpan(reinterpret_cast<const u8*>(sv.data()), sv.size());
}

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

TEST(ObjectArchive, OpenPathInvalidationStatus)
{
  TempArchivePath path;
  ASSERT_TRUE(path.IsValid());

  const u8 payload[] = {0xCA, 0xFE};
  Error error;
  bool was_invalidated = true;
  {
    ObjectArchive archive;
    ASSERT_TRUE(archive.OpenPath(path.GetPath(), TEST_VERSION, &error, &was_invalidated)) << error.GetDescription();
    EXPECT_FALSE(was_invalidated);
    ASSERT_TRUE(archive.Insert(StringToCacheKey("persist"), payload, ObjectArchive::CompressType::Uncompressed, &error))
      << error.GetDescription();
  }

  was_invalidated = true;
  {
    ObjectArchive archive;
    ASSERT_TRUE(archive.OpenPath(path.GetPath(), TEST_VERSION, &error, &was_invalidated)) << error.GetDescription();
    EXPECT_FALSE(was_invalidated);
    EXPECT_TRUE(archive.Contains(StringToCacheKey("persist")));
  }

  was_invalidated = false;
  {
    ObjectArchive archive;
    ASSERT_TRUE(archive.OpenPath(path.GetPath(), TEST_VERSION + 1, &error, &was_invalidated)) << error.GetDescription();
    EXPECT_TRUE(was_invalidated);
    EXPECT_EQ(archive.GetSize(), 0u);
  }

  const std::string index_path = fmt::format("{}.idx", path.GetPath());
  FileSystem::ManagedCFilePtr index_file = FileSystem::OpenManagedCFile(index_path.c_str(), "r+b", &error);
  ASSERT_TRUE(index_file) << error.GetDescription();
  const u32 invalid_signature = 0;
  ASSERT_EQ(std::fwrite(&invalid_signature, sizeof(invalid_signature), 1, index_file.get()), 1u);
  index_file.reset();

  was_invalidated = false;
  {
    ObjectArchive archive;
    ASSERT_TRUE(archive.OpenPath(path.GetPath(), TEST_VERSION + 1, &error, &was_invalidated)) << error.GetDescription();
    EXPECT_TRUE(was_invalidated);
    EXPECT_EQ(archive.GetSize(), 0u);
  }
}

TEST(ObjectArchive, InsertToClosedArchive)
{
  ObjectArchive archive;
  EXPECT_FALSE(archive.IsOpen());

  const u8 data[] = {1, 2, 3};
  Error error;
  EXPECT_FALSE(archive.Insert(StringToCacheKey("key"), data, ObjectArchive::CompressType::Uncompressed, &error));
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
  EXPECT_FALSE(archive.Insert({}, std::span<const u8>(data), ObjectArchive::CompressType::Uncompressed, &error));
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
  ASSERT_TRUE(archive.Insert(StringToCacheKey("test_key"), payload, ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();

  auto result = archive.Lookup(StringToCacheKey("test_key"), &error);
  ASSERT_TRUE(result.has_value()) << error.GetDescription();
  ASSERT_EQ(result->size(), sizeof(payload));
  EXPECT_EQ(std::memcmp(result->data(), payload, sizeof(payload)), 0);
}

TEST(ObjectArchive, BinaryKeyRoundTrip)
{
  TempArchiveFiles files;
  ASSERT_TRUE(files.IsValid());

  static constexpr std::array<u8, 6> key1 = {0x00, 0x61, 0x00, 0x62, 0x00, 0xFF};
  static constexpr std::array<u8, 6> key2 = {0x00, 0x61, 0x00, 0x62, 0x01, 0xFF};
  static constexpr std::array<u8, 3> payload1 = {0x12, 0x34, 0x56};
  static constexpr std::array<u8, 2> payload2 = {0xAB, 0xCD};
  const std::string_view key1_string(reinterpret_cast<const char*>(key1.data()), key1.size());

  {
    ObjectArchive archive;
    auto [idx, blob] = files.Release();
    Error error;
    ASSERT_TRUE(archive.CreateFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();
    ASSERT_TRUE(archive.Insert(key1, payload1, ObjectArchive::CompressType::Uncompressed, &error))
      << error.GetDescription();
    ASSERT_TRUE(archive.Insert(key2, payload2, ObjectArchive::CompressType::Uncompressed, &error))
      << error.GetDescription();

    EXPECT_TRUE(archive.Contains(key1));
    EXPECT_TRUE(archive.Contains(key2));
    EXPECT_TRUE(archive.Contains(StringToCacheKey(key1_string)));
    EXPECT_FALSE(archive.Contains(ObjectArchive::KeySpan(key1).first(key1.size() - 1)));
  }

  ASSERT_TRUE(files.Reopen());
  {
    ObjectArchive archive;
    auto [idx, blob] = files.Release();
    Error error;
    ASSERT_TRUE(archive.OpenFile(idx, blob, TEST_VERSION, &error)) << error.GetDescription();

    const std::optional<ObjectArchive::ObjectData> result1 = archive.Lookup(StringToCacheKey(key1_string), &error);
    ASSERT_TRUE(result1.has_value()) << error.GetDescription();
    ASSERT_EQ(result1->size(), payload1.size());
    EXPECT_EQ(std::memcmp(result1->data(), payload1.data(), payload1.size()), 0);

    const std::optional<ObjectArchive::ObjectData> result2 = archive.Lookup(ObjectArchive::KeySpan(key2), &error);
    ASSERT_TRUE(result2.has_value()) << error.GetDescription();
    ASSERT_EQ(result2->size(), payload2.size());
    EXPECT_EQ(std::memcmp(result2->data(), payload2.data(), payload2.size()), 0);
  }
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

  ASSERT_TRUE(
    archive.Insert(StringToCacheKey("compressed_key"), payload, ObjectArchive::CompressType::Zstandard, &error))
    << error.GetDescription();

  auto result = archive.Lookup(StringToCacheKey("compressed_key"), &error);
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
  ASSERT_TRUE(archive.Insert(StringToCacheKey("dup"), data1, ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();
  EXPECT_FALSE(archive.Insert(StringToCacheKey("dup"), data2, ObjectArchive::CompressType::Uncompressed, &error));
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
  ASSERT_TRUE(archive.Insert(StringToCacheKey("exists"), data, ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();

  auto result = archive.Lookup(StringToCacheKey("does_not_exist"), &error);
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

  ASSERT_TRUE(archive.Insert(StringToCacheKey("aaa"), a_data, ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();
  ASSERT_TRUE(archive.Insert(StringToCacheKey("mmm"), m_data, ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();
  ASSERT_TRUE(archive.Insert(StringToCacheKey("zzz"), z_data, ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();

  auto ra = archive.Lookup(StringToCacheKey("aaa"), &error);
  ASSERT_TRUE(ra.has_value()) << error.GetDescription();
  ASSERT_EQ(ra->size(), sizeof(a_data));
  EXPECT_EQ((*ra)[0], 0xAA);

  auto rm = archive.Lookup(StringToCacheKey("mmm"), &error);
  ASSERT_TRUE(rm.has_value()) << error.GetDescription();
  ASSERT_EQ(rm->size(), sizeof(m_data));
  EXPECT_EQ((*rm)[0], 0xBB);
  EXPECT_EQ((*rm)[1], 0xCC);

  auto rz = archive.Lookup(StringToCacheKey("zzz"), &error);
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
  ASSERT_TRUE(archive.Insert(StringToCacheKey("key1"), data1, ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();
  EXPECT_EQ(archive.GetSize(), 1u);

  ASSERT_TRUE(archive.Clear(&error)) << error.GetDescription();
  EXPECT_EQ(archive.GetSize(), 0u);

  // After clear, lookup should fail.
  auto result = archive.Lookup(StringToCacheKey("key1"), &error);
  EXPECT_FALSE(result.has_value());

  // Re-insertion should succeed.
  const u8 data2[] = {0x33, 0x44, 0x55};
  ASSERT_TRUE(archive.Insert(StringToCacheKey("key2"), data2, ObjectArchive::CompressType::Uncompressed, &error))
    << error.GetDescription();
  EXPECT_EQ(archive.GetSize(), 1u);

  auto result2 = archive.Lookup(StringToCacheKey("key2"), &error);
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
    ASSERT_TRUE(archive.Insert(StringToCacheKey("persist"), payload, ObjectArchive::CompressType::Uncompressed, &error))
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

    auto result = archive.Lookup(StringToCacheKey("persist"), &error);
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
    ASSERT_TRUE(archive.Insert(StringToCacheKey("v1_key"), data, ObjectArchive::CompressType::Uncompressed, &error))
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

    ASSERT_TRUE(archive.Insert(StringToCacheKey(key), payload, ObjectArchive::CompressType::Uncompressed, &error))
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
    auto result = archive.Lookup(StringToCacheKey(key), &error);
    ASSERT_TRUE(result.has_value()) << "Lookup failed for key '" << key << "': " << error.GetDescription();
    ASSERT_EQ(result->size(), 8u);

    size_t stored_index = 0;
    std::memcpy(&stored_index, result->data(), sizeof(stored_index));
    EXPECT_EQ(stored_index, i) << "Data mismatch for key '" << key << "'";
  }
}
