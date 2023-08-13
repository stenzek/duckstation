// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "gpu_shader_cache.h"
#include "gpu_device.h"

#include "common/file_system.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/md5_digest.h"

#include "fmt/format.h"

#include "zstd.h"
#include "zstd_errors.h"

Log_SetChannel(GPUShaderCache);

#pragma pack(push, 1)
struct CacheIndexEntry
{
  u32 shader_type;
  u32 source_length;
  u64 source_hash_low;
  u64 source_hash_high;
  u64 entry_point_low;
  u64 entry_point_high;
  u32 file_offset;
  u32 compressed_size;
  u32 uncompressed_size;
};
#pragma pack(pop)

GPUShaderCache::GPUShaderCache() = default;

GPUShaderCache::~GPUShaderCache()
{
  Close();
}

bool GPUShaderCache::CacheIndexKey::operator==(const CacheIndexKey& key) const
{
  return (std::memcmp(this, &key, sizeof(*this)) == 0);
}

bool GPUShaderCache::CacheIndexKey::operator!=(const CacheIndexKey& key) const
{
  return (std::memcmp(this, &key, sizeof(*this)) != 0);
}

std::size_t GPUShaderCache::CacheIndexEntryHash::operator()(const CacheIndexKey& e) const noexcept
{
  std::size_t h = 0;
  hash_combine(h, e.entry_point_low, e.entry_point_high, e.source_hash_low, e.source_hash_high, e.source_length,
               e.shader_type);
  return h;
}

bool GPUShaderCache::Open(const std::string_view& base_filename, u32 version)
{
  m_base_filename = base_filename;
  m_version = version;

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

  Log_WarningPrintf("Clearing shader cache at %s.", m_base_filename.c_str());

  const std::string index_filename = fmt::format("{}.idx", m_base_filename);
  const std::string blob_filename = fmt::format("{}.bin", m_base_filename);
  CreateNew(index_filename, blob_filename);
}

bool GPUShaderCache::CreateNew(const std::string& index_filename, const std::string& blob_filename)
{
  if (FileSystem::FileExists(index_filename.c_str()))
  {
    Log_WarningPrintf("Removing existing index file '%s'", index_filename.c_str());
    FileSystem::DeleteFile(index_filename.c_str());
  }
  if (FileSystem::FileExists(blob_filename.c_str()))
  {
    Log_WarningPrintf("Removing existing blob file '%s'", blob_filename.c_str());
    FileSystem::DeleteFile(blob_filename.c_str());
  }

  m_index_file = FileSystem::OpenCFile(index_filename.c_str(), "wb");
  if (!m_index_file)
  {
    Log_ErrorPrintf("Failed to open index file '%s' for writing", index_filename.c_str());
    return false;
  }

  if (std::fwrite(&m_version, sizeof(m_version), 1, m_index_file) != 1)
  {
    Log_ErrorPrintf("Failed to write version to index file '%s'", index_filename.c_str());
    std::fclose(m_index_file);
    m_index_file = nullptr;
    FileSystem::DeleteFile(index_filename.c_str());
    return false;
  }

  m_blob_file = FileSystem::OpenCFile(blob_filename.c_str(), "w+b");
  if (!m_blob_file)
  {
    Log_ErrorPrintf("Failed to open blob file '%s' for writing", blob_filename.c_str());
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
      Log_WarningPrintf("Failed to open shader cache index with EACCES, are you running two instances?");
      return true;
    }

    return false;
  }

  u32 file_version = 0;
  if (std::fread(&file_version, sizeof(file_version), 1, m_index_file) != 1 || file_version != m_version)
  {
    Log_ErrorPrintf("Bad file/data version in '%s'", index_filename.c_str());
    std::fclose(m_index_file);
    m_index_file = nullptr;
    return false;
  }

  m_blob_file = FileSystem::OpenCFile(blob_filename.c_str(), "a+b");
  if (!m_blob_file)
  {
    Log_ErrorPrintf("Blob file '%s' is missing", blob_filename.c_str());
    std::fclose(m_index_file);
    m_index_file = nullptr;
    return false;
  }

  std::fseek(m_blob_file, 0, SEEK_END);
  const u32 blob_file_size = static_cast<u32>(std::ftell(m_blob_file));

  for (;;)
  {
    CacheIndexEntry entry;
    if (std::fread(&entry, sizeof(entry), 1, m_index_file) != 1 ||
        (entry.file_offset + entry.compressed_size) > blob_file_size)
    {
      if (std::feof(m_index_file))
        break;

      Log_ErrorPrintf("Failed to read entry from '%s', corrupt file?", index_filename.c_str());
      m_index.clear();
      std::fclose(m_blob_file);
      m_blob_file = nullptr;
      std::fclose(m_index_file);
      m_index_file = nullptr;
      return false;
    }

    const CacheIndexKey key{entry.shader_type,      entry.source_length,   entry.source_hash_low,
                            entry.source_hash_high, entry.entry_point_low, entry.entry_point_high};
    const CacheIndexData data{entry.file_offset, entry.compressed_size, entry.uncompressed_size};
    m_index.emplace(key, data);
  }

  // ensure we don't write before seeking
  std::fseek(m_index_file, 0, SEEK_END);

  Log_DevPrintf("Read %zu entries from '%s'", m_index.size(), index_filename.c_str());
  return true;
}

GPUShaderCache::CacheIndexKey GPUShaderCache::GetCacheKey(GPUShaderStage stage, const std::string_view& shader_code,
                                                          const std::string_view& entry_point)
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

  CacheIndexKey key = {};
  key.shader_type = static_cast<u32>(stage);

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

bool GPUShaderCache::Lookup(const CacheIndexKey& key, ShaderBinary* binary)
{
  auto iter = m_index.find(key);
  if (iter == m_index.end())
    return false;

  binary->resize(iter->second.uncompressed_size);

  DynamicHeapArray<u8> compressed_data(iter->second.compressed_size);

  if (std::fseek(m_blob_file, iter->second.file_offset, SEEK_SET) != 0 ||
      std::fread(compressed_data.data(), iter->second.compressed_size, 1, m_blob_file) != 1)
  {
    Log_ErrorPrintf("Read %u byte %s shader from file failed", iter->second.compressed_size,
                    GPUShader::GetStageName(static_cast<GPUShaderStage>(key.shader_type)));
    return false;
  }

  const size_t decompress_result =
    ZSTD_decompress(binary->data(), binary->size(), compressed_data.data(), compressed_data.size());
  if (ZSTD_isError(decompress_result))
  {
    Log_ErrorPrintf("Failed to decompress shader: %s", ZSTD_getErrorName(decompress_result));
    return false;
  }

  return true;
}

bool GPUShaderCache::Insert(const CacheIndexKey& key, const void* data, u32 data_size)
{
  DynamicHeapArray<u8> compress_buffer(ZSTD_compressBound(data_size));
  const size_t compress_result = ZSTD_compress(compress_buffer.data(), compress_buffer.size(), data, data_size, 0);
  if (ZSTD_isError(compress_result))
  {
    Log_ErrorPrintf("Failed to compress shader: %s", ZSTD_getErrorName(compress_result));
    return false;
  }

  if (!m_blob_file || std::fseek(m_blob_file, 0, SEEK_END) != 0)
    return false;

  CacheIndexData idata;
  idata.file_offset = static_cast<u32>(std::ftell(m_blob_file));
  idata.compressed_size = static_cast<u32>(compress_result);
  idata.uncompressed_size = data_size;

  CacheIndexEntry entry = {};
  entry.shader_type = static_cast<u32>(key.shader_type);
  entry.source_length = key.source_length;
  entry.source_hash_low = key.source_hash_low;
  entry.source_hash_high = key.source_hash_high;
  entry.entry_point_low = key.entry_point_low;
  entry.entry_point_high = key.entry_point_high;
  entry.file_offset = idata.file_offset;
  entry.compressed_size = idata.compressed_size;
  entry.uncompressed_size = idata.uncompressed_size;

  if (std::fwrite(compress_buffer.data(), compress_result, 1, m_blob_file) != 1 || std::fflush(m_blob_file) != 0 ||
      std::fwrite(&entry, sizeof(entry), 1, m_index_file) != 1 || std::fflush(m_index_file) != 0)
  {
    Log_ErrorPrintf("Failed to write %u byte %s shader blob to file", data_size,
                    GPUShader::GetStageName(static_cast<GPUShaderStage>(key.shader_type)));
    return false;
  }

  Log_DevPrintf("Cached compressed %s shader: %u -> %u bytes",
                GPUShader::GetStageName(static_cast<GPUShaderStage>(key.shader_type)), data_size,
                static_cast<u32>(compress_result));
  m_index.emplace(key, idata);
  return true;
}
