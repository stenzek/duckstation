// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

typedef struct zip zip_t;
typedef struct zip_error zip_error_t;
typedef struct zip_file zip_file_t;

class Error;
class ProgressCallback;

namespace ZipHelpers {

inline u32 DEFAULT_READ_CHUNK_SIZE = 4096;
inline u32 DEFAULT_EXTRACT_CHUNK_SIZE = 128 * 1024;

struct ZipDeleter
{
  void operator()(zip_t* zf);
};

struct ZipFileDeleter
{
  void operator()(zip_file_t* zf);
};

using ManagedZipT = std::unique_ptr<zip_t, ZipDeleter>;
using ManagedZipFileT = std::unique_ptr<zip_file_t, ZipFileDeleter>;

void SetErrorObject(Error* error, std::string_view msg, zip_error_t* ze, bool finalize = true);

ManagedZipT OpenManagedZipFile(const char* filename, int flags, Error* error = nullptr);
ManagedZipT OpenManagedZipCFile(std::FILE* fp, int flags, Error* error = nullptr);
ManagedZipT OpenManagedZipBuffer(const void* buffer, size_t size, int flags, bool free_buffer, Error* error = nullptr);

ManagedZipFileT OpenManagedFileInZip(zip_t* zip, const char* filename, u32 flags, Error* error = nullptr);
ManagedZipFileT OpenManagedFileIndexInZip(zip_t* zip, u64 index, u32 flags, Error* error = nullptr);

std::vector<std::string> ReadFileListInZip(zip_t* zip);
std::optional<u64> GetFileSizeInZip(zip_t* zip, const char* name, bool case_sensitive = true, Error* error = nullptr);
bool ExtractFileToDisk(zip_t* zip, const char* name, std::string disk_name, bool case_sensitive = true,
                       u32 chunk_size = DEFAULT_EXTRACT_CHUNK_SIZE, ProgressCallback* progress = nullptr,
                       Error* error = nullptr);
bool ExtractFileToDisk(zip_t* zip, const char* name, std::string disk_name, std::span<u8> chunk_buffer,
                       bool case_sensitive = true, ProgressCallback* progress = nullptr, Error* error = nullptr);
bool ExtractFileToDisk(zip_t* zip, const char* name, std::FILE* fp, std::span<u8> chunk_buffer,
                       bool case_sensitive = true, ProgressCallback* progress = nullptr, Error* error = nullptr);

std::optional<std::string> ReadFileInZipToString(zip_t* zip, const char* name, bool case_sensitive = true,
                                                 Error* error = nullptr);
std::optional<std::string> ReadFileInZipToString(zip_file_t* file, u32 chunk_size = DEFAULT_READ_CHUNK_SIZE,
                                                 Error* error = nullptr);

std::optional<std::vector<u8>> ReadBinaryFileInZip(zip_t* zip, const char* name, bool case_sensitive = true,
                                                   Error* error = nullptr);
std::optional<std::vector<u8>> ReadBinaryFileInZip(zip_file_t* file, u32 chunk_size = DEFAULT_READ_CHUNK_SIZE,
                                                   Error* error = nullptr);

} // namespace ZipHelpers