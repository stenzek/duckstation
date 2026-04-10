// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "object_archive.h"

#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/timer.h"

#include <algorithm>
#include <fmt/core.h>

LOG_CHANNEL(CompressHelpers);

namespace {

#pragma pack(push, 4)
struct CacheFileHeader
{
  u32 signature;
  u32 cache_version;
};
struct CacheIndexEntryHeader
{
  u32 file_offset;
  u32 compressed_size;
  u32 uncompressed_size;
  u16 key_size_low;
  u8 key_size_high;
  u8 compress_type;

  ALWAYS_INLINE u32 GetKeySize() const { return (ZeroExtend32(key_size_high) << 16) | ZeroExtend32(key_size_low); }
  ALWAYS_INLINE void SetKeySize(u32 size)
  {
    key_size_low = Truncate16(size);
    key_size_high = Truncate8(size >> 16);
  }
};
#pragma pack(pop)

} // namespace

static constexpr u32 EXPECTED_SIGNATURE = 0x41435544; // DUCA
static constexpr u32 MAX_KEY_SIZE = (1 << 24) - 1;

ObjectArchive::ObjectArchive() = default;

ObjectArchive::~ObjectArchive()
{
  Close();
}

constinit const std::string_view ObjectArchive::ERROR_DESCRIPTION_DOES_NOT_EXIST = "Key not found in archive.";
constinit const std::string_view ObjectArchive::ERROR_DESCRIPTION_ALREADY_EXISTS = "Key already exists in archive.";

void ObjectArchive::Close()
{
  if (m_index_file)
  {
    std::fclose(m_index_file);
    m_index_file = nullptr;
  }
  if (m_blob_file)
  {
    std::fclose(m_blob_file);
    m_blob_file = nullptr;
  }
  m_index.clear();
}

bool ObjectArchive::Clear(Error* error)
{
  if (!IsOpen())
    return true;

  WARNING_LOG("Clearing object cache");

  if (!FileSystem::FSeek64(m_index_file, 0, SEEK_SET, error) || !FileSystem::FSeek64(m_blob_file, 0, SEEK_SET, error) ||
      !FileSystem::FTruncate64(m_blob_file, 0, error) ||
      !FileSystem::FTruncate64(m_index_file, sizeof(CacheFileHeader), error) ||
      !FileSystem::FSeek64(m_index_file, 0, SEEK_END, error))
  {
    ERROR_LOG("Failed to seek/truncate object cache");
    Close();
    return false;
  }

  m_index.clear();
  return true;
}

bool ObjectArchive::OpenPath(std::string_view base_path, u32 data_version, Error* error)
{
  Close();

  const std::string index_filename = fmt::format("{}.idx", base_path);
  const std::string blob_filename = fmt::format("{}.bin", base_path);

  if (FileSystem::FileExists(index_filename.c_str()))
  {
    Error open_error;
    if (OpenExisting(index_filename, blob_filename, data_version, &open_error))
      return true;

    ERROR_LOG("Failed to open existing object archive index '{}': {}", Path::GetFileName(index_filename),
              open_error.GetDescription());
  }

  return CreateNew(index_filename, blob_filename, data_version, error);
}

bool ObjectArchive::OpenFile(std::FILE* index_file, std::FILE* blob_file, u32 data_version, Error* error)
{
  Close();

  m_index_file = index_file;
  m_blob_file = blob_file;

  return ReadExisting(data_version, error);
}

bool ObjectArchive::CreateFile(std::FILE* index_file, std::FILE* blob_file, u32 data_version, Error* error)
{
  Close();

  m_index_file = index_file;
  m_blob_file = blob_file;

  return CreateNew(data_version, error);
}

bool ObjectArchive::CreateNew(const std::string& index_path, const std::string& blob_path, u32 version, Error* error)
{
  if (FileSystem::FileExists(blob_path.c_str()))
  {
    WARNING_LOG("Removing existing blob file '{}'", Path::GetFileName(blob_path));
    FileSystem::DeleteFile(blob_path.c_str());
  }
  if (FileSystem::FileExists(index_path.c_str()))
  {
    WARNING_LOG("Removing existing index file '{}'", Path::GetFileName(index_path));
    FileSystem::DeleteFile(index_path.c_str());
  }

  m_index_file = FileSystem::OpenCFile(index_path.c_str(), "wb", error);
  if (!m_index_file) [[unlikely]]
  {
    ERROR_LOG("Failed to open index file '{}' for writing", Path::GetFileName(index_path));
    return false;
  }

  m_blob_file = FileSystem::OpenCFile(blob_path.c_str(), "w+b", error);
  if (!m_blob_file) [[unlikely]]
  {
    ERROR_LOG("Failed to open blob file '{}' for writing", Path::GetFileName(blob_path));
    Close();
    FileSystem::DeleteFile(index_path.c_str());
    return false;
  }

  if (!CreateNew(version, error))
  {
    ERROR_LOG("Failed to create to index file '{}'", Path::GetFileName(index_path));
    Close();
    FileSystem::DeleteFile(blob_path.c_str());
    FileSystem::DeleteFile(index_path.c_str());
    return false;
  }

  return true;
}

bool ObjectArchive::CreateNew(u32 version, Error* error)
{
  CacheFileHeader file_header;
  file_header.signature = EXPECTED_SIGNATURE;
  file_header.cache_version = version;
  if (std::fwrite(&file_header, sizeof(file_header), 1, m_index_file) != 1) [[unlikely]]
  {
    Error::SetErrno(error, "fwrite() for version failed: ", errno);
    return false;
  }

  return true;
}

bool ObjectArchive::OpenExisting(const std::string& index_path, const std::string& blob_path, u32 version, Error* error)
{
  m_index_file = FileSystem::OpenCFile(index_path.c_str(), "r+b", error);
  if (!m_index_file)
  {
    // special case here: when there's a sharing violation (i.e. two instances running),
    // we don't want to blow away the cache. so just continue without a cache.
    if (errno == EACCES)
    {
      WARNING_LOG("Failed to open archive index with EACCES, are you running two instances?");
      return true;
    }

    return false;
  }

  m_blob_file = FileSystem::OpenCFile(blob_path.c_str(), "a+b", error);
  if (!m_blob_file) [[unlikely]]
  {
    ERROR_LOG("Blob file '{}' is missing", Path::GetFileName(blob_path));
    Close();
    return false;
  }

  return ReadExisting(version, error);
}

bool ObjectArchive::ReadExisting(u32 version, Error* error)
{
  CacheFileHeader file_header;
  if (std::fread(&file_header, sizeof(file_header), 1, m_index_file) != 1 ||
      file_header.signature != EXPECTED_SIGNATURE || file_header.cache_version != version) [[unlikely]]
  {
    Error::SetStringFmt(error, "Bad file/data version (expected {}, got {})", version, file_header.cache_version);
    Close();
    return false;
  }

  const s64 index_file_size = FileSystem::FSize64(m_index_file);
  if (index_file_size < 0 || index_file_size > 1 * 1048576)
  {
    Error::SetStringFmt(error, "Index file is too large ({} bytes)", index_file_size);
    Close();
    return false;
  }

  const s64 blob_file_size = FileSystem::FSize64(m_blob_file, error);
  if (blob_file_size < 0)
  {
    Close();
    return false;
  }

  // preallocate string storage, this will overshoot a bit since we don't know the actual key sizes, but it should be
  // good enough to avoid fragmentation and multiple resizes in most cases.
  m_key_pool.Reserve(static_cast<size_t>(index_file_size));

  Timer timer;
  std::string key;
  for (;;)
  {
    CacheIndexEntryHeader key_header;
    u32 key_size;

    if (std::fread(&key_header, sizeof(key_header), 1, m_index_file) != 1 ||
        (key_size = key_header.GetKeySize()) == 0 || key_size > MAX_KEY_SIZE ||
        (key_header.file_offset + key_header.compressed_size) > blob_file_size ||
        key_header.compress_type >= static_cast<u8>(CompressType::Count) ||
        (key.resize(key_size), std::fread(key.data(), key_size, 1, m_index_file)) != 1) [[unlikely]]
    {
      if (std::feof(m_index_file))
        break;

      Error::SetErrno(error, "fread() failed: ", errno);
      Close();
      return false;
    }

    const BumpStringPool::Offset offset = m_key_pool.AddString(key);
    m_index.emplace_back(key_header.file_offset, key_header.compressed_size, key_header.uncompressed_size,
                         static_cast<u32>(offset), key_size, static_cast<CompressType>(key_header.compress_type));
  }

  // ensure we don't write before seeking
  if (!FileSystem::FSeek64(m_index_file, 0, SEEK_END, error))
  {
    Close();
    return false;
  }

  // ensure index is sorted, the file is written out of order so it probably won't be
  std::sort(m_index.begin(), m_index.end(),
            [this](const CacheIndexData& a, const CacheIndexData& b) { return (GetKeyString(a) < GetKeyString(b)); });

  // there shouldn't be any duplicates
  for (size_t i = 1; i < m_index.size(); i++)
  {
    if (GetKeyString(m_index[i - 1]) == GetKeyString(m_index[i]))
    {
      Error::SetStringFmt(error, "Duplicate key '{}' in index file, corrupt file?", GetKeyString(m_index[i]));
      Close();
      return false;
    }
  }

  DEV_LOG("Read {} entries in {:.2f} ms", m_index.size(), timer.GetTimeMilliseconds());
  return true;
}

std::string_view ObjectArchive::GetKeyString(const CacheIndexData& data) const
{
  return m_key_pool.GetString(data.key_offset, data.key_size);
}

std::optional<ObjectArchive::ObjectData> ObjectArchive::Lookup(std::string_view key, Error* error)
{
  const auto iter = std::lower_bound(
    m_index.begin(), m_index.end(), key,
    [this](const CacheIndexData& entry, const std::string_view& key) { return (GetKeyString(entry) < key); });
  if (iter == m_index.end() || GetKeyString(*iter) != key)
  {
    Error::SetStringView(error, ERROR_DESCRIPTION_DOES_NOT_EXIST);
    return std::nullopt;
  }

  ObjectData data(iter->compressed_size);
  if (std::fseek(m_blob_file, iter->file_offset, SEEK_SET) != 0 ||
      std::fread(data.data(), iter->compressed_size, 1, m_blob_file) != 1) [[unlikely]]
  {
    ERROR_LOG("failed to read {} byte object at offset {} for key '{}'", iter->compressed_size, iter->file_offset, key);
    Error::SetErrno(error, errno);
    return std::nullopt;
  }

  if (iter->compress_type == CompressType::Uncompressed)
    return std::optional<ObjectData>(std::move(data));

  ObjectData uncompressed_data;
  if (!CompressHelpers::DecompressBuffer(uncompressed_data, iter->compress_type, data.cspan(), iter->uncompressed_size,
                                         error))
  {
    ERROR_LOG("Decompress {} byte object failed", iter->uncompressed_size);
    return std::nullopt;
  }

  return std::optional<ObjectData>(std::move(uncompressed_data));
}

bool ObjectArchive::Contains(std::string_view key) const
{
  if (key.empty() || key.size() > MAX_KEY_SIZE || !IsOpen()) [[unlikely]]
    return false;

  const auto iter = std::lower_bound(
    m_index.begin(), m_index.end(), key,
    [this](const CacheIndexData& entry, const std::string_view& key) { return (GetKeyString(entry) < key); });
  return (iter != m_index.end() && GetKeyString(*iter) == key);
}

bool ObjectArchive::Insert(std::string_view key, std::span<const u8> data, CompressType compression, Error* error)
{
  return Insert(key, data.data(), data.size(), compression, error);
}

bool ObjectArchive::Insert(std::string_view key, const void* data, size_t data_size, CompressType compression,
                           Error* error)
{
  if (key.empty() || key.size() > MAX_KEY_SIZE) [[unlikely]]
  {
    Error::SetStringView(error, "Invalid key size.");
    return false;
  }
  else if (!IsOpen()) [[unlikely]]
  {
    Error::SetStringView(error, "Archive is not open.");
    return false;
  }

  const auto iter = std::lower_bound(
    m_index.begin(), m_index.end(), key,
    [this](const CacheIndexData& entry, const std::string_view& key) { return (GetKeyString(entry) < key); });
  if (iter != m_index.end() && GetKeyString(*iter) == key)
  {
    Error::SetStringView(error, ERROR_DESCRIPTION_ALREADY_EXISTS);
    return false;
  }

  DynamicHeapArray<u8> compress_buffer;
  const void* write_data = data;
  size_t write_size = data_size;
  if (compression != CompressType::Uncompressed)
  {
    if (!CompressHelpers::CompressToBuffer(compress_buffer, compression,
                                           std::span<const u8>(static_cast<const u8*>(data), data_size), -1, error))
    {
      ERROR_LOG("Compress {} byte object failed", data_size);
      return false;
    }

    DEV_LOG("Cached compressed object: {} -> {} bytes ({:.1f}%)", data_size, compress_buffer.size(),
            (static_cast<float>(data_size) / static_cast<float>(compress_buffer.size())) * 100.0f);

    write_data = compress_buffer.data();
    write_size = compress_buffer.size();
  }

  if (!m_blob_file || !FileSystem::FSeek64(m_blob_file, 0, SEEK_END, error))
    return false;

  const s64 file_offset = FileSystem::FTell64(m_blob_file);
  if (file_offset < 0)
  {
    Error::SetErrno(error, "ftell() failed: ", errno);
    return false;
  }

  CacheIndexData idata;
  idata.file_offset = static_cast<u32>(file_offset);
  idata.compressed_size = static_cast<u32>(write_size);
  idata.uncompressed_size = static_cast<u32>(data_size);
  idata.key_offset = static_cast<u32>(m_key_pool.AddString(key));
  idata.key_size = static_cast<u32>(key.size());
  idata.compress_type = compression;

  CacheIndexEntryHeader key_header = {};
  key_header.file_offset = idata.file_offset;
  key_header.compressed_size = idata.compressed_size;
  key_header.uncompressed_size = idata.uncompressed_size;
  key_header.SetKeySize(idata.key_size);
  key_header.compress_type = static_cast<u8>(compression);

  if (std::fwrite(write_data, write_size, 1, m_blob_file) != 1 || std::fflush(m_blob_file) != 0 ||
      std::fwrite(&key_header, sizeof(key_header), 1, m_index_file) != 1 ||
      std::fwrite(key.data(), static_cast<u32>(key.size()), 1, m_index_file) != 1 || std::fflush(m_index_file) != 0)
    [[unlikely]]
  {
    Error::SetErrno(error, "fwrite() failed: ", errno);
    ERROR_LOG("Failed to write {} byte object", data_size);
    return false;
  }

  m_index.emplace(iter, idata);
  return true;
}

u64 ObjectArchive::GetTotalObjectSize() const
{
  u64 total_size = 0;
  for (const CacheIndexData& entry : m_index)
    total_size += entry.uncompressed_size;
  return total_size;
}

u64 ObjectArchive::GetTotalSize() const
{
  u64 total_size = 0;
  for (const CacheIndexData& entry : m_index)
    total_size += entry.compressed_size + sizeof(CacheIndexEntryHeader) + entry.key_size;
  total_size += sizeof(CacheFileHeader);
  return total_size;
}
