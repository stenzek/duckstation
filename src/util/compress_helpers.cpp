// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "compress_helpers.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/string_util.h"

#include <zstd.h>
#include <zstd_errors.h>

// TODO: Use streaming API to avoid mallocing the whole input buffer. But one read() call is probably still faster..

namespace CompressHelpers {
static std::optional<CompressType> GetCompressType(const std::string_view path, Error* error);

template<typename T>
static bool DecompressHelper(OptionalByteBuffer& ret, CompressType type, T data,
                             std::optional<size_t> decompressed_size, Error* error);

template<typename T>
static bool CompressHelper(OptionalByteBuffer& ret, CompressType type, T data, int clevel, Error* error);
} // namespace CompressHelpers

std::optional<CompressHelpers::CompressType> CompressHelpers::GetCompressType(const std::string_view path, Error* error)
{
  const std::string_view extension = Path::GetExtension(path);
  if (StringUtil::EqualNoCase(extension, "zst"))
    return CompressType::Zstandard;

  return CompressType::Uncompressed;
}

template<typename T>
bool CompressHelpers::DecompressHelper(CompressHelpers::OptionalByteBuffer& ret, CompressType type, T data,
                                       std::optional<size_t> decompressed_size, Error* error)
{
  if (data.size() == 0) [[unlikely]]
  {
    Error::SetStringView(error, "Buffer is empty.");
    return false;
  }

  switch (type)
  {
    case CompressType::Uncompressed:
    {
      ret = ByteBuffer(std::move(data));
      return true;
    }

    case CompressType::Zstandard:
    {
      size_t real_decompressed_size;
      if (!decompressed_size.has_value())
      {
        const unsigned long long runtime_decompressed_size = ZSTD_getFrameContentSize(data.data(), data.size());
        if (runtime_decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN ||
            runtime_decompressed_size == ZSTD_CONTENTSIZE_ERROR ||
            runtime_decompressed_size >= std::numeric_limits<size_t>::max()) [[unlikely]]
        {
          Error::SetStringView(error, "Failed to get uncompressed size.");
          return false;
        }

        real_decompressed_size = static_cast<size_t>(runtime_decompressed_size);
      }
      else
      {
        real_decompressed_size = decompressed_size.value();
      }

      ret = DynamicHeapArray<u8>(real_decompressed_size);

      const size_t result = ZSTD_decompress(ret->data(), ret->size(), data.data(), data.size());
      if (ZSTD_isError(result)) [[unlikely]]
      {
        const char* errstr = ZSTD_getErrorString(ZSTD_getErrorCode(result));
        Error::SetStringFmt(error, "ZSTD_decompress() failed: {}", errstr ? errstr : "<unknown>");
        ret.reset();
        return false;
      }
      else if (result != real_decompressed_size) [[unlikely]]
      {
        Error::SetStringFmt(error, "ZSTD_decompress() only returned {} of {} bytes.", result, real_decompressed_size);
        ret.reset();
        return false;
      }

      return true;
    }
    break;

      DefaultCaseIsUnreachable()
  }
}

template<typename T>
bool CompressHelpers::CompressHelper(OptionalByteBuffer& ret, CompressType type, T data, int clevel, Error* error)
{
  if (data.size() == 0) [[unlikely]]
  {
    Error::SetStringView(error, "Buffer is empty.");
    return false;
  }

  switch (type)
  {
    case CompressType::Uncompressed:
    {
      ret = ByteBuffer(std::move(data));
      return true;
    }

    case CompressType::Zstandard:
    {
      const size_t compressed_size = ZSTD_compressBound(data.size());
      if (compressed_size == 0) [[unlikely]]
      {
        Error::SetStringView(error, "ZSTD_compressBound() failed.");
        return false;
      }

      ret = ByteBuffer(compressed_size);

      const size_t result = ZSTD_compress(ret->data(), compressed_size, data.data(), data.size(),
                                          (clevel < 0) ? 0 : std::clamp(clevel, 1, 22));
      if (ZSTD_isError(result)) [[unlikely]]
      {
        const char* errstr = ZSTD_getErrorString(ZSTD_getErrorCode(result));
        Error::SetStringFmt(error, "ZSTD_compress() failed: {}", errstr ? errstr : "<unknown>");
        return false;
      }

      ret->resize(result);
      return true;
    }

      DefaultCaseIsUnreachable()
  }
}

CompressHelpers::OptionalByteBuffer CompressHelpers::DecompressBuffer(CompressType type, std::span<const u8> data,
                                                                      std::optional<size_t> decompressed_size,
                                                                      Error* error)
{
  CompressHelpers::OptionalByteBuffer ret;
  DecompressHelper(ret, type, data, decompressed_size, error);
  return ret;
}

CompressHelpers::OptionalByteBuffer CompressHelpers::DecompressBuffer(CompressType type, OptionalByteBuffer data,
                                                                      std::optional<size_t> decompressed_size,
                                                                      Error* error)
{
  OptionalByteBuffer ret;
  if (data.has_value())
  {
    DecompressHelper(ret, type, std::move(data.value()), decompressed_size, error);
  }
  else
  {
    if (error && !error->IsValid())
      error->SetStringView("Data buffer is empty.");
  }

  return ret;
}

CompressHelpers::OptionalByteBuffer CompressHelpers::DecompressFile(std::string_view path, std::span<const u8> data,
                                                                    std::optional<size_t> decompressed_size,
                                                                    Error* error)
{
  OptionalByteBuffer ret;
  const std::optional<CompressType> type = GetCompressType(path, error);
  if (type.has_value())
    ret = DecompressBuffer(type.value(), data, decompressed_size, error);
  return ret;
}

CompressHelpers::OptionalByteBuffer CompressHelpers::DecompressFile(std::string_view path, OptionalByteBuffer data,
                                                                    std::optional<size_t> decompressed_size,
                                                                    Error* error)
{
  OptionalByteBuffer ret;
  const std::optional<CompressType> type = GetCompressType(path, error);
  if (type.has_value())
    ret = DecompressBuffer(type.value(), std::move(data), decompressed_size, error);
  return ret;
}

CompressHelpers::OptionalByteBuffer
CompressHelpers::DecompressFile(const char* path, std::optional<size_t> decompressed_size, Error* error)
{
  OptionalByteBuffer ret;
  const std::optional<CompressType> type = GetCompressType(path, error);
  if (type.has_value())
    ret = DecompressFile(type.value(), path, decompressed_size, error);
  return ret;
}

CompressHelpers::OptionalByteBuffer CompressHelpers::DecompressFile(CompressType type, const char* path,
                                                                    std::optional<size_t> decompressed_size,
                                                                    Error* error)
{
  OptionalByteBuffer ret;
  OptionalByteBuffer data = FileSystem::ReadBinaryFile(path, error);
  if (data.has_value())
    ret = DecompressBuffer(type, std::move(data), decompressed_size, error);
  return ret;
}

CompressHelpers::OptionalByteBuffer CompressHelpers::CompressToBuffer(CompressType type, std::span<const u8> data,
                                                                      int clevel, Error* error)
{
  OptionalByteBuffer ret;
  CompressHelper(ret, type, data, clevel, error);
  return ret;
}

CompressHelpers::OptionalByteBuffer CompressHelpers::CompressToBuffer(CompressType type, const void* data,
                                                                      size_t data_size, int clevel, Error* error)
{
  OptionalByteBuffer ret;
  CompressHelper(ret, type, std::span<const u8>(static_cast<const u8*>(data), data_size), clevel, error);
  return ret;
}

CompressHelpers::OptionalByteBuffer CompressHelpers::CompressToBuffer(CompressType type, OptionalByteBuffer data,
                                                                      int clevel, Error* error)
{
  OptionalByteBuffer ret;
  CompressHelper(ret, type, std::move(data.value()), clevel, error);
  return ret;
}

bool CompressHelpers::CompressToFile(const char* path, std::span<const u8> data, int clevel, bool atomic_write,
                                     Error* error)
{
  const std::optional<CompressType> type = GetCompressType(path, error);
  if (!type.has_value())
    return false;

  return CompressToFile(type.value(), path, data, clevel, atomic_write, error);
}

bool CompressHelpers::CompressToFile(CompressType type, const char* path, std::span<const u8> data, int clevel,
                                     bool atomic_write, Error* error)
{
  const OptionalByteBuffer cdata = CompressToBuffer(type, data, clevel, error);
  if (!cdata.has_value())
    return false;

  return atomic_write ? FileSystem::WriteAtomicRenamedFile(path, cdata->data(), cdata->size(), error) :
                        FileSystem::WriteBinaryFile(path, cdata->data(), cdata->size(), error);
}
