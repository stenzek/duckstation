// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "compress_helpers.h"

#include "common/heap_array.h"
#include "common/string_pool.h"

#include <optional>
#include <string>
#include <string_view>

class Error;

class ObjectArchive
{
public:
  using ObjectData = DynamicHeapArray<u8>;
  using CompressType = CompressHelpers::CompressType;

  ObjectArchive();
  ~ObjectArchive();

  /// Error messages indicating the key does/does not exist in the archive.
  static const std::string_view ERROR_DESCRIPTION_DOES_NOT_EXIST;
  static const std::string_view ERROR_DESCRIPTION_ALREADY_EXISTS;

  /// Returns true if the archive has been successfully opened or created.
  bool IsOpen() const { return (m_index_file != nullptr); }

  /// Returns the number of entries currently stored in the archive.
  size_t GetSize() const { return m_index.size(); }

  /// Opens or creates an archive at the given base path. The index and blob files will be named
  /// "{base_path}.idx" and "{base_path}.bin" respectively. If the files already exist and match
  /// the given data version, they are opened; otherwise a new archive is created.
  bool OpenPath(std::string_view base_path, u32 data_version, Error* error);

  /// Opens an existing cache file. Ownership of the index_file and blob_file pointers are transferred to the
  /// ObjectArchive and they will be closed when the ObjectArchive is closed or goes out of scope.
  bool OpenFile(std::FILE* index_file, std::FILE* blob_file, u32 data_version, Error* error);

  /// Creates a cache file with already opened file pointers. Ownership of the index_file and blob_file pointers are
  /// transferred to the ObjectArchive and they will be closed when the ObjectArchive is closed or goes out of scope.
  bool CreateFile(std::FILE* index_file, std::FILE* blob_file, u32 data_version, Error* error);

  /// Removes all entries from the archive, truncating the blob file and resetting the index.
  /// The archive remains open and ready for new insertions. Returns true on success.
  bool Clear(Error* error);

  /// Closes the archive, releasing the index and blob file handles and clearing the in-memory index.
  void Close();

  /// Looks up an object by key. Returns the decompressed object data on success, or std::nullopt
  /// if the key is not found or an I/O error occurs.
  std::optional<ObjectData> Lookup(std::string_view key, Error* error);

  /// Returns true if the specified key exists in the archive.
  bool Contains(std::string_view key) const;

  /// Inserts an object into the archive under the given key. The data may optionally be compressed
  /// using the specified compression type. Returns false if the key already exists, the archive is
  /// not open, or an I/O error occurs.
  bool Insert(std::string_view key, std::span<const u8> data, CompressType compression, Error* error);
  bool Insert(std::string_view key, const void* data, size_t data_size, CompressType compression, Error* error);

  /// Returns the total size of all objects in the cache.
  u64 GetTotalObjectSize() const;

  /// Returns the total size of all objects in the cache, and the index.
  u64 GetTotalSize() const;

private:
  struct CacheIndexData
  {
    u32 file_offset;
    u32 compressed_size;
    u32 uncompressed_size;
    u32 key_offset;
    u32 key_size : 24;
    CompressType compress_type : 8;
  };
  using CacheIndex = std::vector<CacheIndexData>;

  bool CreateNew(const std::string& index_path, const std::string& blob_path, u32 version, Error* error);
  bool CreateNew(u32 version, Error* error);
  bool OpenExisting(const std::string& index_path, const std::string& blob_path, u32 version, Error* error);
  bool ReadExisting(u32 version, Error* error);

  std::string_view GetKeyString(const CacheIndexData& data) const;

  CacheIndex m_index;
  BumpStringPool m_key_pool;

  std::FILE* m_index_file = nullptr;
  std::FILE* m_blob_file = nullptr;
};
