// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "compress_helpers.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"

#include "7zCrc.h"
#include "Alloc.h"
#include "Xz.h"
#include "XzCrc64.h"
#include "XzEnc.h"

#include <zstd.h>
#include <zstd_errors.h>

LOG_CHANNEL(CompressHelpers);

// TODO: Use streaming API to avoid mallocing the whole input buffer. But one read() call is probably still faster..

namespace CompressHelpers {
static std::optional<CompressType> GetCompressType(const std::string_view path, Error* error);

template<typename T>
static bool DecompressHelper(OptionalByteBuffer& ret, CompressType type, T data,
                             std::optional<size_t> decompressed_size, Error* error);

template<typename T>
static bool CompressHelper(OptionalByteBuffer& ret, CompressType type, T data, int clevel, Error* error);

static void Init7ZCRCTables();
static bool XzCompress(OptionalByteBuffer& ret, const u8* data, size_t data_size, int clevel, Error* error);
static bool XzDecompress(OptionalByteBuffer& ret, const u8* data, size_t data_size, Error* error);

static std::once_flag s_lzma_crc_table_init;
} // namespace CompressHelpers

void CompressHelpers::Init7ZCRCTables()
{
  std::call_once(s_lzma_crc_table_init, []() {
    CrcGenerateTable();
    Crc64GenerateTable();
  });
}

const char* CompressHelpers::SZErrorToString(int res)
{
  // clang-format off
  switch (res)
  {
  case SZ_OK: return "SZ_OK";
  case SZ_ERROR_DATA: return "SZ_ERROR_DATA";
  case SZ_ERROR_MEM: return "SZ_ERROR_MEM";
  case SZ_ERROR_CRC: return "SZ_ERROR_CRC";
  case SZ_ERROR_UNSUPPORTED: return "SZ_ERROR_UNSUPPORTED";
  case SZ_ERROR_PARAM: return "SZ_ERROR_PARAM";
  case SZ_ERROR_INPUT_EOF: return "SZ_ERROR_INPUT_EOF";
  case SZ_ERROR_OUTPUT_EOF: return "SZ_ERROR_OUTPUT_EOF";
  case SZ_ERROR_READ: return "SZ_ERROR_READ";
  case SZ_ERROR_WRITE: return "SZ_ERROR_WRITE";
  case SZ_ERROR_PROGRESS: return "SZ_ERROR_PROGRESS";
  case SZ_ERROR_FAIL: return "SZ_ERROR_FAIL";
  case SZ_ERROR_THREAD: return "SZ_ERROR_THREAD";
  case SZ_ERROR_ARCHIVE: return "SZ_ERROR_ARCHIVE";
  case SZ_ERROR_NO_ARCHIVE: return "SZ_ERROR_NO_ARCHIVE";
  default: return "SZ_UNKNOWN";
  }
  // clang-format on
}

bool CompressHelpers::XzCompress(OptionalByteBuffer& ret, const u8* data, size_t data_size, int clevel, Error* error)
{
  Init7ZCRCTables();

  struct MemoryInStream
  {
    ISeqInStream vt;
    const u8* buffer;
    size_t buffer_size;
    size_t read_pos;
  };
  MemoryInStream mis = {{.Read = [](const ISeqInStream* p, void* buf, size_t* size) -> SRes {
                          MemoryInStream* mis = Z7_CONTAINER_FROM_VTBL(p, MemoryInStream, vt);
                          const size_t avail = mis->buffer_size - mis->read_pos;
                          const size_t copy = std::min(avail, *size);

                          std::memcpy(buf, &mis->buffer[mis->read_pos], copy);
                          mis->read_pos += copy;
                          *size = copy;
                          return SZ_OK;
                        }},
                        data,
                        data_size,
                        0};

  // Bit crap, extra copy here..
  struct DumpOutStream
  {
    ISeqOutStream vt;
    DynamicHeapArray<u8> out_data;
    size_t out_pos;
  };
  DumpOutStream dos = {.vt = {.Write = [](const ISeqOutStream* p, const void* buf, size_t size) -> size_t {
                         DumpOutStream* dos = Z7_CONTAINER_FROM_VTBL(p, DumpOutStream, vt);
                         if ((dos->out_pos + size) > dos->out_data.size())
                           dos->out_data.resize(std::max(dos->out_pos + size, dos->out_data.size() * 2));
                         std::memcpy(&dos->out_data[dos->out_pos], buf, size);
                         dos->out_pos += size;
                         return size;
                       }},
                       .out_data = DynamicHeapArray<u8>(data_size / 2),
                       .out_pos = 0};

  CXzProps props;
  XzProps_Init(&props);
  props.lzma2Props.lzmaProps.level = std::clamp(clevel, 1, 9);

  const SRes res = Xz_Encode(&dos.vt, &mis.vt, &props, nullptr);
  if (res != SZ_OK)
  {
    Error::SetStringFmt(error, "Xz_Encode() failed: {} ({})", SZErrorToString(res), static_cast<int>(res));
    return false;
  }

  dos.out_data.resize(dos.out_pos);
  ret = OptionalByteBuffer(std::move(dos.out_data));
  return true;
}

bool CompressHelpers::XzDecompress(OptionalByteBuffer& ret, const u8* data, size_t data_size, Error* error)
{
  static constexpr size_t kInputBufSize = static_cast<size_t>(1) << 18;

  Init7ZCRCTables();

  struct MyInStream
  {
    ISeekInStream vt;
    const u8* data;
    size_t data_size;
    size_t data_pos;
  };

  MyInStream mis = {.vt = {.Read = [](const ISeekInStream* p, void* buf, size_t* size) -> SRes {
                             MyInStream* mis = Z7_CONTAINER_FROM_VTBL(p, MyInStream, vt);
                             const size_t size_to_read = *size;
                             const size_t size_to_copy = std::min(size_to_read, mis->data_size - mis->data_pos);
                             std::memcpy(buf, &mis->data[mis->data_pos], size_to_copy);
                             mis->data_pos += size_to_copy;
                             return (size_to_copy == size_to_read) ? SZ_OK : SZ_ERROR_READ;
                           },
                           .Seek = [](const ISeekInStream* p, Int64* pos, ESzSeek origin) -> SRes {
                             MyInStream* mis = Z7_CONTAINER_FROM_VTBL(p, MyInStream, vt);
                             static_assert(SZ_SEEK_CUR == SEEK_CUR && SZ_SEEK_SET == SEEK_SET &&
                                           SZ_SEEK_END == SEEK_END);
                             if (origin == SZ_SEEK_SET)
                             {
                               if (*pos < 0 || static_cast<size_t>(*pos) > mis->data_size)
                                 return SZ_ERROR_READ;
                               mis->data_pos = static_cast<size_t>(*pos);
                               return SZ_OK;
                             }
                             else if (origin == SZ_SEEK_END)
                             {
                               mis->data_pos = mis->data_size;
                               *pos = static_cast<s64>(mis->data_pos);
                               return SZ_OK;
                             }
                             else if (origin == SZ_SEEK_CUR)
                             {
                               const s64 new_pos = static_cast<s64>(mis->data_pos) + *pos;
                               if (new_pos < 0 || static_cast<size_t>(new_pos) > mis->data_size)
                                 return SZ_ERROR_READ;
                               mis->data_pos = static_cast<size_t>(new_pos);
                               *pos = new_pos;
                               return SZ_OK;
                             }
                             else
                             {
                               return SZ_ERROR_READ;
                             }
                           }},
                    .data = data,
                    .data_size = data_size,
                    .data_pos = 0};

  CLookToRead2 look_stream = {};
  LookToRead2_INIT(&look_stream);
  LookToRead2_CreateVTable(&look_stream, False);
  look_stream.realStream = &mis.vt;
  look_stream.bufSize = kInputBufSize;
  look_stream.buf = static_cast<Byte*>(ISzAlloc_Alloc(&g_Alloc, kInputBufSize));
  if (!look_stream.buf)
  {
    Error::SetString(error, "Failed to allocate lookahead buffer");
    return false;
  }
  const ScopedGuard guard = [&look_stream]() {
    if (look_stream.buf)
      ISzAlloc_Free(&g_Alloc, look_stream.buf);
  };

  // Read blocks
  CXzs xzs;
  Xzs_Construct(&xzs);
  const ScopedGuard xzs_guard([&xzs]() { Xzs_Free(&xzs, &g_Alloc); });

  Int64 start_pos = static_cast<Int64>(data_size);
  SRes res = Xzs_ReadBackward(&xzs, &look_stream.vt, &start_pos, nullptr, &g_Alloc);
  if (res != SZ_OK)
  {
    Error::SetStringFmt(error, "Xzs_ReadBackward() failed: {} ({})", SZErrorToString(res), res);
    return false;
  }

  const size_t num_blocks = Xzs_GetNumBlocks(&xzs);
  if (num_blocks == 0)
  {
    Error::SetString(error, "Stream has no blocks.");
    return false;
  }

  // compute output size
  size_t stream_size = 0;
  for (int sn = static_cast<int>(xzs.num - 1); sn >= 0; sn--)
  {
    const CXzStream& stream = xzs.streams[sn];
    for (size_t bn = 0; bn < stream.numBlocks; bn++)
    {
      const CXzBlockSizes& block = stream.blocks[bn];
      stream_size += block.unpackSize;
    }
  }

  if (stream_size == 0)
  {
    Error::SetString(error, "Stream is empty.");
    return false;
  }

  ByteBuffer out_buffer(stream_size);
  size_t out_pos = 0;

  CXzUnpacker unpacker = {};
  XzUnpacker_Construct(&unpacker, &g_Alloc);

  for (int sn = static_cast<int>(xzs.num - 1); sn >= 0; sn--)
  {
    const CXzStream& stream = xzs.streams[sn];
    size_t src_offset = stream.startOffset + XZ_STREAM_HEADER_SIZE;
    if (src_offset >= data_size)
      break;

    for (size_t bn = 0; bn < stream.numBlocks; bn++)
    {
      const CXzBlockSizes& block = stream.blocks[bn];

      XzUnpacker_Init(&unpacker);
      unpacker.streamFlags = stream.flags;
      XzUnpacker_PrepareToRandomBlockDecoding(&unpacker);
      XzUnpacker_SetOutBuf(&unpacker, &out_buffer[out_pos], out_buffer.size() - out_pos);

      const size_t orig_compressed_size =
        std::min<size_t>(Common::AlignUpPow2(block.totalSize, 4),
                         static_cast<size_t>(data_size - src_offset)); // LZMA blocks are 4 byte aligned?;

      SizeT block_uncompressed_size = block.unpackSize;
      SizeT block_compressed_size = orig_compressed_size;

      ECoderStatus status;
      res = XzUnpacker_Code(&unpacker, nullptr, &block_uncompressed_size, &data[src_offset], &block_compressed_size,
                            true, CODER_FINISH_END, &status);
      if (res != SZ_OK || status != CODER_STATUS_FINISHED_WITH_MARK) [[unlikely]]
      {
        Error::SetStringFmt(error, "XzUnpacker_Code() failed: {} ({}) (status {})", SZErrorToString(res), res,
                            static_cast<unsigned>(status));
        return false;
      }

      if (block_compressed_size != orig_compressed_size || block_uncompressed_size != block.unpackSize)
      {
        WARNING_LOG("Decompress size mismatch: {}/{} vs {}/{}", block_compressed_size, block_uncompressed_size,
                    orig_compressed_size, block.unpackSize);
      }

      out_pos += block_uncompressed_size;
      src_offset += block_compressed_size;
    }
  }

  if (out_pos != out_buffer.size())
  {
    Error::SetStringFmt(error, "Only decompressed {} of {} bytes", out_pos, out_buffer.size());
    return false;
  }

  ret = std::move(out_buffer);
  return true;
}

std::optional<CompressHelpers::CompressType> CompressHelpers::GetCompressType(const std::string_view path, Error* error)
{
  const std::string_view extension = Path::GetExtension(path);
  if (StringUtil::EqualNoCase(extension, "zst"))
    return CompressType::Zstandard;
  else if (StringUtil::EqualNoCase(extension, "xz"))
    return CompressType::XZ;

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

    case CompressType::XZ:
    {
      return XzDecompress(ret, data.data(), data.size(), error);
    }

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

    case CompressType::XZ:
    {
      return XzCompress(ret, data.data(), data.size(), clevel, error);
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
