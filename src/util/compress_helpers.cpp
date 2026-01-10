// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
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

#include <zlib.h>
#include <zstd.h>
#include <zstd_errors.h>

LOG_CHANNEL(CompressHelpers);

// TODO: Use streaming API to avoid mallocing the whole input buffer. But one read() call is probably still faster..

namespace CompressHelpers {
static std::optional<CompressType> GetCompressType(const std::string_view path, Error* error);

static const char* ZlibErrorToString(int res);
static std::optional<size_t> GetDeflateDecompressedSize(std::span<const u8> data, Error* error);
static bool DecompressDeflate(std::span<u8> dst, size_t uncompressed_size, std::span<const u8> data, Error* error);

static std::optional<size_t> GetZstdDecompressedSize(std::span<const u8> data, Error* error);
static bool DecompressZstd(std::span<u8> dst, size_t uncompressed_size, std::span<const u8> data, Error* error);

template<typename T>
static bool DecompressHelper(ByteBuffer& ret, CompressType type, T data, std::optional<size_t> decompressed_size,
                             Error* error);

template<typename T>
static bool CompressHelper(ByteBuffer& ret, CompressType type, T data, int clevel, Error* error);

static void Init7ZCRCTables();
static bool XzCompress(ByteBuffer& ret, const u8* data, size_t data_size, int clevel, Error* error);

static std::once_flag s_lzma_crc_table_init;

} // namespace CompressHelpers

std::optional<CompressHelpers::CompressType> CompressHelpers::GetCompressType(const std::string_view path, Error* error)
{
  const std::string_view extension = Path::GetExtension(path);
  if (StringUtil::EqualNoCase(extension, "zst"))
    return CompressType::Zstandard;
  else if (StringUtil::EqualNoCase(extension, "xz"))
    return CompressType::XZ;

  return CompressType::Uncompressed;
}

const char* CompressHelpers::ZlibErrorToString(int res)
{
  // clang-format off
  switch (res)
  {
  case Z_OK: return "Z_OK";
  case Z_STREAM_END: return "Z_STREAM_END";
  case Z_NEED_DICT: return "Z_NEED_DICT";
  case Z_ERRNO: return "Z_ERRNO";
  case Z_STREAM_ERROR: return "Z_STREAM_ERROR";
  case Z_DATA_ERROR: return "Z_DATA_ERROR";
  case Z_MEM_ERROR: return "Z_MEM_ERROR";
  case Z_BUF_ERROR: return "Z_BUF_ERROR";
  case Z_VERSION_ERROR: return "Z_VERSION_ERROR";
  default: return "Z_UNKNOWN_ERROR";
  }
  // clang-format on
}

std::optional<size_t> CompressHelpers::GetDeflateDecompressedSize(std::span<const u8> data, Error* error)
{
  z_stream zs;
  int res = inflateInit(&zs);
  if (res != Z_OK)
  {
    Error::SetStringFmt(error, "inflateInit() failed: {} ({})", ZlibErrorToString(res), res);
    return false;
  }

  u8 temp_buf[1024];

  zs.next_in = const_cast<u8*>(data.data());
  zs.avail_in = static_cast<u32>(data.size());

  while (zs.avail_in > 0)
  {
    zs.next_out = temp_buf;
    zs.avail_out = sizeof(temp_buf);

    res = inflate(&zs, Z_FINISH);
    if (res != Z_OK)
    {
      Error::SetStringFmt(error, "inflate() failed: {} ({})", ZlibErrorToString(res), res);
      inflateEnd(&zs);
      return std::nullopt;
    }
  }

  inflateEnd(&zs);
  return zs.total_out;
}

bool CompressHelpers::DecompressDeflate(std::span<u8> dst, size_t uncompressed_size, std::span<const u8> data,
                                        Error* error)
{
  unsigned long src_len = static_cast<unsigned long>(data.size());
  unsigned long dest_len = static_cast<unsigned long>(uncompressed_size);
  const int res = uncompress2(dst.data(), &dest_len, data.data(), &src_len);
  if (res != Z_OK)
  {
    Error::SetStringFmt(error, "uncompress2() failed: {} ({})", ZlibErrorToString(res), res);
    return false;
  }

  return true;
}

std::optional<size_t> CompressHelpers::GetZstdDecompressedSize(std::span<const u8> data, Error* error)
{
  const unsigned long long runtime_decompressed_size = ZSTD_getFrameContentSize(data.data(), data.size());
  if (runtime_decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN || runtime_decompressed_size == ZSTD_CONTENTSIZE_ERROR ||
      runtime_decompressed_size >= std::numeric_limits<size_t>::max()) [[unlikely]]
  {
    Error::SetStringView(error, "Failed to get uncompressed size.");
    return false;
  }

  return static_cast<size_t>(runtime_decompressed_size);
}

bool CompressHelpers::DecompressZstd(std::span<u8> dst, size_t uncompressed_size, std::span<const u8> data,
                                     Error* error)
{
  if (dst.size() < uncompressed_size)
  {
    Error::SetStringFmt(error, "Destination buffer is too small, expected {}, got {}", uncompressed_size, dst.size());
    return false;
  }

  const size_t result = ZSTD_decompress(dst.data(), dst.size(), data.data(), data.size());
  if (ZSTD_isError(result)) [[unlikely]]
  {
    const char* errstr = ZSTD_getErrorString(ZSTD_getErrorCode(result));
    Error::SetStringFmt(error, "ZSTD_decompress() failed: {}", errstr ? errstr : "<unknown>");
    return false;
  }
  else if (result != uncompressed_size) [[unlikely]]
  {
    Error::SetStringFmt(error, "ZSTD_decompress() only returned {} of {} bytes.", result, uncompressed_size);
    return false;
  }

  return true;
}

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

namespace CompressHelpers {

namespace {
class XzDecompressor
{
public:
  XzDecompressor();
  ~XzDecompressor();

  ALWAYS_INLINE size_t GetStreamSize() const { return stream_size; }

  bool Init(std::span<const u8> data, Error* error);

  bool Decompress(std::span<u8> dst, Error* error);

private:
  static constexpr size_t kInputBufSize = static_cast<size_t>(1) << 18;

  struct MyInStream
  {
    ISeekInStream vt;
    const u8* data;
    size_t data_size;
    size_t data_pos;
  };

  CLookToRead2 look_stream = {};
  MyInStream mis = {};
  CXzs xzs = {};
  size_t stream_size = 0;
};
} // namespace
} // namespace CompressHelpers

CompressHelpers::XzDecompressor::XzDecompressor()
{
  Xzs_Construct(&xzs);
}

CompressHelpers::XzDecompressor::~XzDecompressor()
{
  Xzs_Free(&xzs, &g_Alloc);

  if (look_stream.buf)
    ISzAlloc_Free(&g_Alloc, look_stream.buf);
}

bool CompressHelpers::XzDecompressor::Init(std::span<const u8> data, Error* error)
{
  Init7ZCRCTables();

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

  mis = {.vt = {.Read = [](const ISeekInStream* p, void* buf, size_t* size) -> SRes {
                  MyInStream* mis = Z7_CONTAINER_FROM_VTBL(p, MyInStream, vt);
                  const size_t size_to_read = *size;
                  const size_t size_to_copy = std::min(size_to_read, mis->data_size - mis->data_pos);
                  std::memcpy(buf, &mis->data[mis->data_pos], size_to_copy);
                  mis->data_pos += size_to_copy;
                  return (size_to_copy == size_to_read) ? SZ_OK : SZ_ERROR_READ;
                },
                .Seek = [](const ISeekInStream* p, Int64* pos, ESzSeek origin) -> SRes {
                  MyInStream* mis = Z7_CONTAINER_FROM_VTBL(p, MyInStream, vt);
                  static_assert(SZ_SEEK_CUR == SEEK_CUR && SZ_SEEK_SET == SEEK_SET && SZ_SEEK_END == SEEK_END);
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
         .data = data.data(),
         .data_size = data.size(),
         .data_pos = 0};

  // Read blocks
  Int64 start_pos = static_cast<Int64>(mis.data_size);
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
  for (int sn = static_cast<int>(xzs.num - 1); sn >= 0; sn--)
  {
    const CXzStream& stream = xzs.streams[sn];
    for (size_t bn = 0; bn < stream.numBlocks; bn++)
    {
      const CXzBlockSizes& block = stream.blocks[bn];
      stream_size += block.unpackSize;
    }
  }

  return true;
}

bool CompressHelpers::XzDecompressor::Decompress(std::span<u8> dst, Error* error)
{
  if (dst.size() < stream_size)
  {
    Error::SetStringFmt(error, "Destination buffer is too small, expected {}, got {}", stream_size, dst.size());
    return false;
  }

  size_t out_pos = 0;

  CXzUnpacker unpacker = {};
  XzUnpacker_Construct(&unpacker, &g_Alloc);

  for (int sn = static_cast<int>(xzs.num - 1); sn >= 0; sn--)
  {
    const CXzStream& stream = xzs.streams[sn];
    size_t src_offset = stream.startOffset + XZ_STREAM_HEADER_SIZE;
    if (src_offset >= mis.data_size)
      break;

    for (size_t bn = 0; bn < stream.numBlocks; bn++)
    {
      const CXzBlockSizes& block = stream.blocks[bn];

      XzUnpacker_Init(&unpacker);
      unpacker.streamFlags = stream.flags;
      XzUnpacker_PrepareToRandomBlockDecoding(&unpacker);
      XzUnpacker_SetOutBuf(&unpacker, &dst[out_pos], dst.size() - out_pos);

      const size_t orig_compressed_size =
        std::min<size_t>(Common::AlignUpPow2(block.totalSize, 4),
                         static_cast<size_t>(mis.data_size - src_offset)); // LZMA blocks are 4 byte aligned?;

      SizeT block_uncompressed_size = block.unpackSize;
      SizeT block_compressed_size = orig_compressed_size;

      ECoderStatus status;
      SRes res = XzUnpacker_Code(&unpacker, nullptr, &block_uncompressed_size, &mis.data[src_offset],
                                 &block_compressed_size, true, CODER_FINISH_END, &status);
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

  if (out_pos != stream_size)
  {
    Error::SetStringFmt(error, "Only decompressed {} of {} bytes", out_pos, stream_size);
    return false;
  }

  return true;
}

bool CompressHelpers::XzCompress(ByteBuffer& ret, const u8* data, size_t data_size, int clevel, Error* error)
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

  if (ret.empty())
    ret.resize(data_size / 2);

  // Bit crap, extra copy here..
  struct DumpOutStream
  {
    ISeqOutStream vt;
    DynamicHeapArray<u8>* out_data;
    size_t out_pos;
  };
  DumpOutStream dos = {.vt = {.Write = [](const ISeqOutStream* p, const void* buf, size_t size) -> size_t {
                         DumpOutStream* dos = Z7_CONTAINER_FROM_VTBL(p, DumpOutStream, vt);
                         if ((dos->out_pos + size) > dos->out_data->size())
                           dos->out_data->resize(std::max(dos->out_pos + size, dos->out_data->size() * 2));
                         std::memcpy(dos->out_data->data() + dos->out_pos, buf, size);
                         dos->out_pos += size;
                         return size;
                       }},
                       .out_data = &ret,
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

  ret.resize(dos.out_pos);
  return true;
}

std::optional<size_t> CompressHelpers::GetDecompressedSize(CompressType type, std::span<const u8> data,
                                                           Error* error /*= nullptr*/)
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
      return data.size();
    }

    case CompressType::Deflate:
    {
      return GetDeflateDecompressedSize(data, error);
    }

    case CompressType::Zstandard:
    {
      return GetZstdDecompressedSize(data, error);
    }

    case CompressType::XZ:
    {
      XzDecompressor dc;
      if (!dc.Init(data, error))
        return std::nullopt;

      return dc.GetStreamSize();
    }

      DefaultCaseIsUnreachable()
  }
}

template<typename T>
bool CompressHelpers::DecompressHelper(ByteBuffer& ret, CompressType type, T data,
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
      if constexpr (std::is_same_v<T, ByteBuffer>)
        ret = std::move(data);
      else
        ret = ByteBuffer(std::move(data));
      return true;
    }

    case CompressType::Deflate:
    {
      if (!decompressed_size.has_value() && !(decompressed_size = GetDeflateDecompressedSize(data, error)).has_value())
        return false;

      ret.resize(decompressed_size.value());
      return DecompressDeflate(ret.span(), decompressed_size.value(), data, error);
    }

    case CompressType::Zstandard:
    {
      if (!decompressed_size.has_value() && !(decompressed_size = GetZstdDecompressedSize(data, error)).has_value())
        return false;

      ret.resize(decompressed_size.value());
      return DecompressZstd(ret.span(), decompressed_size.value(), data, error);
    }

    case CompressType::XZ:
    {
      XzDecompressor dc;
      if (!dc.Init(data, error))
        return false;

      ret.resize(dc.GetStreamSize());
      return dc.Decompress(ret.span(), error);
    }

      DefaultCaseIsUnreachable()
  }
}

std::optional<size_t> CompressHelpers::DecompressBuffer(std::span<u8> dst, CompressType type, std::span<const u8> data,
                                                        std::optional<size_t> decompressed_size /*= std::nullopt*/,
                                                        Error* error /*= nullptr*/)
{
  if (data.size() == 0) [[unlikely]]
  {
    Error::SetStringView(error, "Buffer is empty.");
    return std::nullopt;
  }

  switch (type)
  {
    case CompressType::Uncompressed:
    {
      if (dst.size() < data.size())
      {
        Error::SetStringFmt(error, "Destination buffer is too small, expected {}, got {}", data.size(), dst.size());
        return std::nullopt;
      }

      std::memcpy(dst.data(), data.data(), data.size());
      return data.size();
    }

    case CompressType::Deflate:
    {
      if (!decompressed_size.has_value() && !(decompressed_size = GetZstdDecompressedSize(data, error)).has_value())
        return std::nullopt;

      return DecompressDeflate(dst, decompressed_size.value(), data, error);
    }

    case CompressType::Zstandard:
    {
      if (!decompressed_size.has_value() && !(decompressed_size = GetZstdDecompressedSize(data, error)).has_value())
        return std::nullopt;

      if (!DecompressZstd(dst, decompressed_size.value(), data, error))
        return std::nullopt;

      return decompressed_size;
    }

    case CompressType::XZ:
    {
      XzDecompressor dc;
      if (!dc.Init(data, error))
        return false;

      if (!dc.Decompress(dst, error))
        return std::nullopt;

      return decompressed_size;
    }

      DefaultCaseIsUnreachable()
  }
}

template<typename T>
bool CompressHelpers::CompressHelper(ByteBuffer& ret, CompressType type, T data, int clevel, Error* error)
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
      if constexpr (std::is_same_v<T, ByteBuffer>)
        ret = std::move(data);
      else
        ret = ByteBuffer(std::move(data));
      return true;
    }

    case CompressType::Deflate:
    {
      unsigned long compressed_size = compressBound(static_cast<uLong>(data.size()));
      if (compressed_size == 0) [[unlikely]]
      {
        Error::SetStringView(error, "ZSTD_compressBound() failed.");
        return false;
      }

      ret.resize(compressed_size);

      const int err = compress2(ret.data(), &compressed_size, data.data(), static_cast<uLong>(data.size()), clevel);
      if (err != Z_OK) [[unlikely]]
      {
        Error::SetStringFmt(error, "compress2() failed: {} ({})", ZlibErrorToString(err), err);
        return false;
      }

      ret.resize(compressed_size);
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

      ret.resize(compressed_size);

      const size_t result = ZSTD_compress(ret.data(), compressed_size, data.data(), data.size(),
                                          (clevel < 0) ? 0 : std::clamp(clevel, 1, 22));
      if (ZSTD_isError(result)) [[unlikely]]
      {
        const char* errstr = ZSTD_getErrorString(ZSTD_getErrorCode(result));
        Error::SetStringFmt(error, "ZSTD_compress() failed: {}", errstr ? errstr : "<unknown>");
        return false;
      }

      ret.resize(result);
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
  CompressHelpers::OptionalByteBuffer ret = ByteBuffer();
  if (!DecompressHelper(ret.value(), type, data, decompressed_size, error))
    ret.reset();
  return ret;
}

CompressHelpers::OptionalByteBuffer CompressHelpers::DecompressBuffer(CompressType type, OptionalByteBuffer data,
                                                                      std::optional<size_t> decompressed_size,
                                                                      Error* error)
{
  OptionalByteBuffer ret;
  if (data.has_value())
  {
    ret = ByteBuffer();
    if (!DecompressHelper(ret.value(), type, std::move(data.value()), decompressed_size, error))
      ret.reset();
  }
  else
  {
    if (error && !error->IsValid())
      error->SetStringView("Data buffer is empty.");
  }

  return ret;
}

bool CompressHelpers::DecompressBuffer(ByteBuffer& dst, CompressType type, std::span<const u8> data,
                                       std::optional<size_t> decompressed_size /*= std::nullopt*/,
                                       Error* error /*= nullptr*/)
{
  return DecompressHelper(dst, type, data, decompressed_size, error);
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
  OptionalByteBuffer ret = ByteBuffer();
  if (!CompressHelper(ret.value(), type, data, clevel, error))
    ret.reset();
  return ret;
}

CompressHelpers::OptionalByteBuffer CompressHelpers::CompressToBuffer(CompressType type, const void* data,
                                                                      size_t data_size, int clevel, Error* error)
{
  OptionalByteBuffer ret = ByteBuffer();
  if (!CompressHelper(ret.value(), type, std::span<const u8>(static_cast<const u8*>(data), data_size), clevel, error))
    ret.reset();
  return ret;
}

CompressHelpers::OptionalByteBuffer CompressHelpers::CompressToBuffer(CompressType type, OptionalByteBuffer data,
                                                                      int clevel, Error* error)
{
  OptionalByteBuffer ret = ByteBuffer();
  if (!CompressHelper(ret.value(), type, std::move(data.value()), clevel, error))
    ret.reset();
  return ret;
}

bool CompressHelpers::CompressToBuffer(ByteBuffer& dst, CompressType type, std::span<const u8> data,
                                       int clevel /*= -1*/, Error* error /*= nullptr*/)
{
  return CompressHelper(dst, type, data, clevel, error);
}

bool CompressHelpers::CompressToBuffer(ByteBuffer& dst, CompressType type, ByteBuffer data, int clevel /*= -1*/,
                                       Error* error /*= nullptr*/)
{
  return CompressHelper(dst, type, std::move(data), clevel, error);
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
