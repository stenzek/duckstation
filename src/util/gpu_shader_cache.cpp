// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_shader_cache.h"
#include "gpu_device.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/md5_digest.h"
#include "common/path.h"

#include "fmt/format.h"

#include "compress_helpers.h"

LOG_CHANNEL(GPUDevice);

struct CacheFileHeader
{
  u32 signature;
  u32 render_api_version;
  u32 cache_version;
};
static_assert(sizeof(CacheFileHeader) == 12, "Cache file header has no padding");

static constexpr u32 EXPECTED_SIGNATURE = 0x434B5544; // DUKC

static constexpr size_t KEY_COPY_SIZE = offsetof(GPUShaderCache::CacheIndexKey, unused);

template<typename A, typename B>
ALWAYS_INLINE static int CompareEntries(const A& a, const B& b)
{
  // don't compare file fields when looking up
  return std::memcmp(&a, &b, KEY_COPY_SIZE);
}

GPUShaderCache::GPUShaderCache()
{
  static_assert(std::is_standard_layout_v<CacheIndexKey> && std::is_trivially_copyable_v<CacheIndexKey>,
                "Cache key must be trivially copyable");
  static_assert(std::is_standard_layout_v<CacheIndexEntry> && std::is_trivially_copyable_v<CacheIndexEntry>,
                "Cache entry must be trivially copyable");
  static_assert(offsetof(CacheIndexKey, shader_type) == offsetof(CacheIndexEntry, shader_type) &&
                  offsetof(CacheIndexKey, shader_language) == offsetof(CacheIndexEntry, shader_language) &&
                  offsetof(CacheIndexKey, source_hash_low) == offsetof(CacheIndexEntry, source_hash_low) &&
                  offsetof(CacheIndexKey, source_hash_high) == offsetof(CacheIndexEntry, source_hash_high) &&
                  offsetof(CacheIndexKey, entry_point_low) == offsetof(CacheIndexEntry, entry_point_low) &&
                  offsetof(CacheIndexKey, entry_point_high) == offsetof(CacheIndexEntry, entry_point_high) &&
                  offsetof(CacheIndexKey, source_length) == offsetof(CacheIndexEntry, source_length),
                "Cache key and entry must have matching layout");
}

GPUShaderCache::~GPUShaderCache()
{
  Close();
}

bool GPUShaderCache::Open(std::string_view base_filename, u32 render_api_version, u32 cache_version)
{
  m_base_filename = base_filename;
  m_render_api_version = render_api_version;
  m_version = cache_version;

  if (base_filename.empty())
    return true;

  const std::string index_filename = fmt::format("{}.idx", m_base_filename);
  const std::string blob_filename = fmt::format("{}.bin", m_base_filename);
  return ReadExisting(index_filename, blob_filename);
}

bool GPUShaderCache::Create()
{
  const std::string index_filename = fmt::format("{}.idx", m_base_filename);
  const std::string blob_filename = fmt::format("{}.bin", m_base_filename);
  return CreateNew(index_filename, blob_filename);
}

void GPUShaderCache::Close()
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
}

void GPUShaderCache::Clear()
{
  if (!IsOpen())
    return;

  Close();

  WARNING_LOG("Clearing shader cache at {}.", Path::GetFileName(m_base_filename));

  const std::string index_filename = fmt::format("{}.idx", m_base_filename);
  const std::string blob_filename = fmt::format("{}.bin", m_base_filename);
  CreateNew(index_filename, blob_filename);
}

bool GPUShaderCache::CreateNew(const std::string& index_filename, const std::string& blob_filename)
{
  if (FileSystem::FileExists(index_filename.c_str()))
  {
    WARNING_LOG("Removing existing index file '{}'", Path::GetFileName(index_filename));
    FileSystem::DeleteFile(index_filename.c_str());
  }
  if (FileSystem::FileExists(blob_filename.c_str()))
  {
    WARNING_LOG("Removing existing blob file '{}'", Path::GetFileName(blob_filename));
    FileSystem::DeleteFile(blob_filename.c_str());
  }

  m_index_file = FileSystem::OpenCFile(index_filename.c_str(), "wb");
  if (!m_index_file) [[unlikely]]
  {
    ERROR_LOG("Failed to open index file '{}' for writing", Path::GetFileName(index_filename));
    return false;
  }

  const CacheFileHeader file_header = {
    .signature = EXPECTED_SIGNATURE, .render_api_version = m_render_api_version, .cache_version = m_version};
  if (std::fwrite(&file_header, sizeof(file_header), 1, m_index_file) != 1) [[unlikely]]
  {
    ERROR_LOG("Failed to write version to index file '{}'", Path::GetFileName(index_filename));
    std::fclose(m_index_file);
    m_index_file = nullptr;
    FileSystem::DeleteFile(index_filename.c_str());
    return false;
  }

  m_blob_file = FileSystem::OpenCFile(blob_filename.c_str(), "w+b");
  if (!m_blob_file) [[unlikely]]
  {
    ERROR_LOG("Failed to open blob file '{}' for writing", Path::GetFileName(blob_filename));
    std::fclose(m_index_file);
    m_index_file = nullptr;
    FileSystem::DeleteFile(index_filename.c_str());
    return false;
  }

  return true;
}

bool GPUShaderCache::ReadExisting(const std::string& index_filename, const std::string& blob_filename)
{
  m_index_file = FileSystem::OpenCFile(index_filename.c_str(), "r+b");
  if (!m_index_file)
  {
    // special case here: when there's a sharing violation (i.e. two instances running),
    // we don't want to blow away the cache. so just continue without a cache.
    if (errno == EACCES)
    {
      WARNING_LOG("Failed to open shader cache index with EACCES, are you running two instances?");
      return true;
    }

    return false;
  }

  CacheFileHeader file_header;
  if (std::fread(&file_header, sizeof(file_header), 1, m_index_file) != 1 ||
      file_header.signature != EXPECTED_SIGNATURE || file_header.render_api_version != m_render_api_version ||
      file_header.cache_version != m_version) [[unlikely]]
  {
    ERROR_LOG("Bad file/data version in '{}'", Path::GetFileName(index_filename));
    std::fclose(m_index_file);
    m_index_file = nullptr;
    return false;
  }

  m_blob_file = FileSystem::OpenCFile(blob_filename.c_str(), "a+b");
  if (!m_blob_file) [[unlikely]]
  {
    ERROR_LOG("Blob file '{}' is missing", Path::GetFileName(blob_filename));
    std::fclose(m_index_file);
    m_index_file = nullptr;
    return false;
  }

  const s64 start_pos = FileSystem::FTell64(m_index_file);
  s64 end_pos;
  if (start_pos < 0 || !FileSystem::FSeek64(m_index_file, 0, SEEK_END, nullptr) ||
      (end_pos = FileSystem::FTell64(m_index_file)) < 0 ||
      !FileSystem::FSeek64(m_index_file, start_pos, SEEK_SET, nullptr) ||
      ((end_pos - start_pos) % sizeof(CacheIndexEntry)) != 0) [[unlikely]]
  {
    ERROR_LOG("Failed to seek in index file '{}'", Path::GetFileName(index_filename));
    std::fclose(m_blob_file);
    m_blob_file = nullptr;
    std::fclose(m_index_file);
    m_index_file = nullptr;
    return false;
  }

  const size_t num_entries = static_cast<size_t>((end_pos - start_pos) / sizeof(CacheIndexEntry));
  m_index.resize(num_entries);

  if (std::fread(m_index.data(), sizeof(CacheIndexEntry), num_entries, m_index_file) != num_entries) [[unlikely]]
  {
    ERROR_LOG("Failed to read entries from index file '{}'", Path::GetFileName(index_filename));
    m_index.clear();
    std::fclose(m_blob_file);
    m_blob_file = nullptr;
    std::fclose(m_index_file);
    m_index_file = nullptr;
    return false;
  }

  // ensure we don't write before seeking
  FileSystem::FSeek64(m_index_file, 0, SEEK_END);

  // the index won't be sorted initially, since the file is append only
  std::ranges::sort(m_index,
                    [](const CacheIndexEntry& a, const CacheIndexEntry& b) { return (CompareEntries(a, b) < 0); });

  DEV_LOG("Read {} entries from '{}'", m_index.size(), Path::GetFileName(index_filename));
  return true;
}

GPUShaderCache::CacheIndexKey GPUShaderCache::GetCacheKey(GPUShaderStage stage, GPUShaderLanguage language,
                                                          std::string_view shader_code, std::string_view entry_point)
{
  union
  {
    struct
    {
      u64 hash_low;
      u64 hash_high;
    };
    u8 hash[16];
  } h;

  CacheIndexKey key;
  key.shader_type = static_cast<u32>(stage);
  key.shader_language = static_cast<u32>(language);

  MD5Digest digest;
  digest.Update(shader_code.data(), static_cast<u32>(shader_code.length()));
  digest.Final(h.hash);
  key.source_hash_low = h.hash_low;
  key.source_hash_high = h.hash_high;
  key.source_length = static_cast<u32>(shader_code.length());

  digest.Reset();
  digest.Update(entry_point.data(), static_cast<u32>(entry_point.length()));
  digest.Final(h.hash);
  key.entry_point_low = h.hash_low;
  key.entry_point_high = h.hash_high;

  return key;
}

std::optional<GPUShaderCache::ShaderBinary> GPUShaderCache::Lookup(const CacheIndexKey& key)
{
  std::optional<ShaderBinary> ret;

  const auto iter =
    std::lower_bound(m_index.begin(), m_index.end(), key,
                     [](const CacheIndexEntry& a, const CacheIndexKey& b) { return (CompareEntries(a, b) < 0); });
  if (iter != m_index.end() && CompareEntries(*iter, key) == 0)
  {
    DynamicHeapArray<u8> compressed_data(iter->compressed_size);

    if (std::fseek(m_blob_file, iter->file_offset, SEEK_SET) != 0 ||
        std::fread(compressed_data.data(), iter->compressed_size, 1, m_blob_file) != 1) [[unlikely]]
    {
      ERROR_LOG("Read {} byte {} shader from file failed", iter->compressed_size,
                GPUShader::GetStageName(static_cast<GPUShaderStage>(key.shader_type)));
    }
    else
    {
      Error error;
      ret = CompressHelpers::DecompressBuffer(CompressHelpers::CompressType::Zstandard,
                                              CompressHelpers::OptionalByteBuffer(std::move(compressed_data)),
                                              iter->uncompressed_size, &error);
      if (!ret.has_value()) [[unlikely]]
        ERROR_LOG("Failed to decompress shader: {}", error.GetDescription());
    }
  }

  return ret;
}

bool GPUShaderCache::Insert(const CacheIndexKey& key, const void* data, u32 data_size)
{
  Error error;
  CompressHelpers::OptionalByteBuffer compress_buffer =
    CompressHelpers::CompressToBuffer(CompressHelpers::CompressType::Zstandard, data, data_size, -1, &error);
  if (!compress_buffer.has_value()) [[unlikely]]
  {
    ERROR_LOG("Failed to compress shader: {}", error.GetDescription());
    return false;
  }

  if (!m_blob_file || std::fseek(m_blob_file, 0, SEEK_END) != 0)
    return false;

  auto iter =
    std::lower_bound(m_index.begin(), m_index.end(), key,
                     [](const CacheIndexEntry& a, const CacheIndexKey& b) { return (CompareEntries(a, b) < 0); });
  iter = m_index.emplace(iter);
  std::memcpy(&(*iter), &key, KEY_COPY_SIZE);
  iter->file_offset = static_cast<u32>(std::ftell(m_blob_file));
  iter->compressed_size = static_cast<u32>(compress_buffer->size());
  iter->uncompressed_size = data_size;

  if (std::fwrite(compress_buffer->data(), compress_buffer->size(), 1, m_blob_file) != 1 ||
      std::fflush(m_blob_file) != 0 || std::fwrite(&(*iter), sizeof(CacheIndexEntry), 1, m_index_file) != 1 ||
      std::fflush(m_index_file) != 0) [[unlikely]]
  {
    ERROR_LOG("Failed to write {} byte {} shader blob to file", data_size,
              GPUShader::GetStageName(static_cast<GPUShaderStage>(key.shader_type)));
    m_index.erase(iter);
    return false;
  }

  DEV_LOG("Cached compressed {} shader: {} -> {} bytes",
          GPUShader::GetStageName(static_cast<GPUShaderStage>(key.shader_type)), data_size, compress_buffer->size());

  return true;
}
