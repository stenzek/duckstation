// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/hash_combine.h"
#include "common/heap_array.h"
#include "common/types.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class GPUShaderStage : u8;
enum class GPUShaderLanguage : u8;

class GPUShaderCache
{
public:
  using ShaderBinary = DynamicHeapArray<u8>;

  struct alignas(8) CacheIndexKey
  {
    u8 shader_type;
    u8 shader_language;
    u8 unused[2];
    u32 source_length;
    u64 source_hash_low;
    u64 source_hash_high;
    u64 entry_point_low;
    u64 entry_point_high;

    bool operator==(const CacheIndexKey& key) const;
    bool operator!=(const CacheIndexKey& key) const;
  };
  static_assert(sizeof(CacheIndexKey) == 40, "Cache key has no padding");

  struct CacheIndexEntryHash
  {
    std::size_t operator()(const CacheIndexKey& e) const noexcept;
  };

  GPUShaderCache();
  ~GPUShaderCache();

  ALWAYS_INLINE const std::string& GetBaseFilename() const { return m_base_filename; }
  ALWAYS_INLINE u32 GetVersion() const { return m_version; }

  bool IsOpen() const { return (m_index_file != nullptr); }

  bool Open(std::string_view base_filename, u32 render_api_version, u32 cache_version);
  bool Create();
  void Close();

  static CacheIndexKey GetCacheKey(GPUShaderStage stage, GPUShaderLanguage language, std::string_view shader_code,
                                   std::string_view entry_point);

  std::optional<ShaderBinary> Lookup(const CacheIndexKey& key);
  bool Insert(const CacheIndexKey& key, const void* data, u32 data_size);
  void Clear();

private:
  struct CacheIndexData
  {
    u32 file_offset;
    u32 compressed_size;
    u32 uncompressed_size;
  };

  using CacheIndex = std::unordered_map<CacheIndexKey, CacheIndexData, CacheIndexEntryHash>;

  bool CreateNew(const std::string& index_filename, const std::string& blob_filename);
  bool ReadExisting(const std::string& index_filename, const std::string& blob_filename);

  CacheIndex m_index;

  std::string m_base_filename;
  u32 m_render_api_version = 0;
  u32 m_version = 0;

  std::FILE* m_index_file = nullptr;
  std::FILE* m_blob_file = nullptr;
};
