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

Log_SetChannel(GPUShaderCache);

#pragma pack(push, 1)
struct CacheFileHeader
{
  u32 signature;
  u32 render_api_version;
  u32 cache_version;
};
struct CacheIndexEntry
{
  u8 shader_type;
  u8 shader_language;
  u8 unused[2];
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

static constexpr u32 EXPECTED_SIGNATURE = 0x434B5544; // DUKC

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

  std::fseek(m_blob_file, 0, SEEK_END);
  const u32 blob_file_size = static_cast<u32>(std::ftell(m_blob_file));

  for (;;)
  {
    CacheIndexEntry entry;
    if (std::fread(&entry, sizeof(entry), 1, m_index_file) != 1 ||
        (entry.file_offset + entry.compressed_size) > blob_file_size) [[unlikely]]
    {
      if (std::feof(m_index_file))
        break;

      ERROR_LOG("Failed to read entry from '{}', corrupt file?", Path::GetFileName(index_filename));
      m_index.clear();
      std::fclose(m_blob_file);
      m_blob_file = nullptr;
      std::fclose(m_index_file);
      m_index_file = nullptr;
      return false;
    }

    const CacheIndexKey key{entry.shader_type,     entry.shader_language, {},
                            entry.source_length,   entry.source_hash_low, entry.source_hash_high,
                            entry.entry_point_low, entry.entry_point_high};
    const CacheIndexData data{entry.file_offset, entry.compressed_size, entry.uncompressed_size};
    m_index.emplace(key, data);
  }

  // ensure we don't write before seeking
  std::fseek(m_index_file, 0, SEEK_END);

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

  CacheIndexKey key = {};
  key.shader_type = static_cast<u8>(stage);
  key.shader_language = static_cast<u8>(language);

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

  auto iter = m_index.find(key);
  if (iter != m_index.end())
  {
    DynamicHeapArray<u8> compressed_data(iter->second.compressed_size);

    if (std::fseek(m_blob_file, iter->second.file_offset, SEEK_SET) != 0 ||
        std::fread(compressed_data.data(), iter->second.compressed_size, 1, m_blob_file) != 1) [[unlikely]]
    {
      ERROR_LOG("Read {} byte {} shader from file failed", iter->second.compressed_size,
                GPUShader::GetStageName(static_cast<GPUShaderStage>(key.shader_type)));
    }
    else
    {
      Error error;
      ret = CompressHelpers::DecompressBuffer(CompressHelpers::CompressType::Zstandard,
                                              CompressHelpers::OptionalByteBuffer(std::move(compressed_data)),
                                              iter->second.uncompressed_size, &error);
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

  CacheIndexData idata;
  idata.file_offset = static_cast<u32>(std::ftell(m_blob_file));
  idata.compressed_size = static_cast<u32>(compress_buffer->size());
  idata.uncompressed_size = data_size;

  CacheIndexEntry entry = {};
  entry.shader_type = static_cast<u8>(key.shader_type);
  entry.shader_language = static_cast<u8>(key.shader_language);
  entry.source_length = key.source_length;
  entry.source_hash_low = key.source_hash_low;
  entry.source_hash_high = key.source_hash_high;
  entry.entry_point_low = key.entry_point_low;
  entry.entry_point_high = key.entry_point_high;
  entry.file_offset = idata.file_offset;
  entry.compressed_size = idata.compressed_size;
  entry.uncompressed_size = idata.uncompressed_size;

  if (std::fwrite(compress_buffer->data(), compress_buffer->size(), 1, m_blob_file) != 1 ||
      std::fflush(m_blob_file) != 0 || std::fwrite(&entry, sizeof(entry), 1, m_index_file) != 1 ||
      std::fflush(m_index_file) != 0) [[unlikely]]
  {
    ERROR_LOG("Failed to write {} byte {} shader blob to file", data_size,
              GPUShader::GetStageName(static_cast<GPUShaderStage>(key.shader_type)));
    return false;
  }

  DEV_LOG("Cached compressed {} shader: {} -> {} bytes",
          GPUShader::GetStageName(static_cast<GPUShaderStage>(key.shader_type)), data_size, compress_buffer->size());
  m_index.emplace(key, idata);
  return true;
}
