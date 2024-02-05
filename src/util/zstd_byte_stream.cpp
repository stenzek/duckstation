// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "common/byte_stream.h"
#include "common/log.h"

#include <zstd.h>
#include <zstd_errors.h>

Log_SetChannel(ByteStream);

namespace {
class ZstdCompressStream final : public ByteStream
{
public:
  ZstdCompressStream(ByteStream* dst_stream, int compression_level) : m_dst_stream(dst_stream)
  {
    m_cstream = ZSTD_createCStream();
    ZSTD_CCtx_setParameter(m_cstream, ZSTD_c_compressionLevel, compression_level);
  }

  ~ZstdCompressStream() override
  {
    if (!m_done)
      Compress(ZSTD_e_end);

    ZSTD_freeCStream(m_cstream);
  }

  bool ReadByte(u8* pDestByte) override { return false; }

  u32 Read(void* pDestination, u32 ByteCount) override { return 0; }

  bool Read2(void* pDestination, u32 ByteCount, u32* pNumberOfBytesRead = nullptr) override { return false; }

  bool WriteByte(u8 SourceByte) override
  {
    if (m_input_buffer_wpos == INPUT_BUFFER_SIZE && !Compress(ZSTD_e_continue))
      return false;

    m_input_buffer[m_input_buffer_wpos++] = SourceByte;
    return true;
  }

  u32 Write(const void* pSource, u32 ByteCount) override
  {
    u32 remaining = ByteCount;
    const u8* read_ptr = static_cast<const u8*>(pSource);
    for (;;)
    {
      const u32 copy_size = std::min(INPUT_BUFFER_SIZE - m_input_buffer_wpos, remaining);
      std::memcpy(&m_input_buffer[m_input_buffer_wpos], read_ptr, copy_size);
      read_ptr += copy_size;
      remaining -= copy_size;
      m_input_buffer_wpos += copy_size;
      if (remaining == 0 || !Compress(ZSTD_e_continue))
        break;
    }

    return ByteCount - remaining;
  }

  bool Write2(const void* pSource, u32 ByteCount, u32* pNumberOfBytesWritten = nullptr) override
  {
    const u32 bytes_written = Write(pSource, ByteCount);
    if (pNumberOfBytesWritten)
      *pNumberOfBytesWritten = bytes_written;
    return (bytes_written == ByteCount);
  }

  bool SeekAbsolute(u64 Offset) override { return false; }

  bool SeekRelative(s64 Offset) override { return (Offset == 0); }

  bool SeekToEnd() override { return false; }

  u64 GetPosition() const override { return m_position; }

  u64 GetSize() const override { return 0; }

  bool Flush() override { return Compress(ZSTD_e_flush); }

  bool Discard() override { return true; }

  bool Commit() override { return Compress(ZSTD_e_end); }

private:
  enum : u32
  {
    INPUT_BUFFER_SIZE = 131072,
    OUTPUT_BUFFER_SIZE = 65536,
  };

  bool Compress(ZSTD_EndDirective action)
  {
    if (m_errorState || m_done)
      return false;

    ZSTD_inBuffer inbuf = {m_input_buffer, m_input_buffer_wpos, 0};

    for (;;)
    {
      ZSTD_outBuffer outbuf = {m_output_buffer, OUTPUT_BUFFER_SIZE, 0};

      const size_t ret = ZSTD_compressStream2(m_cstream, &outbuf, &inbuf, action);
      if (ZSTD_isError(ret))
      {
        Log_ErrorPrintf("ZSTD_compressStream2() failed: %u (%s)", static_cast<unsigned>(ZSTD_getErrorCode(ret)),
                        ZSTD_getErrorString(ZSTD_getErrorCode(ret)));
        SetErrorState();
        return false;
      }

      if (outbuf.pos > 0)
      {
        if (!m_dst_stream->Write2(m_output_buffer, static_cast<u32>(outbuf.pos)))
        {
          SetErrorState();
          return false;
        }

        outbuf.pos = 0;
      }

      if (action == ZSTD_e_end)
      {
        // break when compression output has finished
        if (ret == 0)
        {
          m_done = true;
          break;
        }
      }
      else
      {
        // break when all input data is consumed
        if (inbuf.pos == inbuf.size)
          break;
      }
    }

    m_position += m_input_buffer_wpos;
    m_input_buffer_wpos = 0;
    return true;
  }

  ByteStream* m_dst_stream;
  ZSTD_CStream* m_cstream = nullptr;
  u64 m_position = 0;
  u32 m_input_buffer_wpos = 0;
  bool m_done = false;

  u8 m_input_buffer[INPUT_BUFFER_SIZE];
  u8 m_output_buffer[OUTPUT_BUFFER_SIZE];
};
} // namespace

std::unique_ptr<ByteStream> ByteStream::CreateZstdCompressStream(ByteStream* src_stream, int compression_level)
{
  return std::make_unique<ZstdCompressStream>(src_stream, compression_level);
}

namespace {
class ZstdDecompressStream final : public ByteStream
{
public:
  ZstdDecompressStream(ByteStream* src_stream, u32 compressed_size)
    : m_src_stream(src_stream), m_bytes_remaining(compressed_size)
  {
    m_cstream = ZSTD_createDStream();
    m_in_buffer.src = m_input_buffer;
    Decompress();
  }

  ~ZstdDecompressStream() override { ZSTD_freeDStream(m_cstream); }

  bool ReadByte(u8* pDestByte) override { return Read(pDestByte, sizeof(u8)) == sizeof(u8); }

  u32 Read(void* pDestination, u32 ByteCount) override
  {
    u8* write_ptr = static_cast<u8*>(pDestination);
    u32 remaining = ByteCount;
    for (;;)
    {
      const u32 copy_size = std::min<u32>(m_output_buffer_wpos - m_output_buffer_rpos, remaining);
      std::memcpy(write_ptr, &m_output_buffer[m_output_buffer_rpos], copy_size);
      m_output_buffer_rpos += copy_size;
      write_ptr += copy_size;
      remaining -= copy_size;
      if (remaining == 0 || !Decompress())
        break;
    }

    return ByteCount - remaining;
  }

  bool Read2(void* pDestination, u32 ByteCount, u32* pNumberOfBytesRead = nullptr) override
  {
    const u32 bytes_read = Read(pDestination, ByteCount);
    if (pNumberOfBytesRead)
      *pNumberOfBytesRead = bytes_read;
    return (bytes_read == ByteCount);
  }

  bool WriteByte(u8 SourceByte) override { return false; }

  u32 Write(const void* pSource, u32 ByteCount) override { return 0; }

  bool Write2(const void* pSource, u32 ByteCount, u32* pNumberOfBytesWritten = nullptr) override { return false; }

  bool SeekAbsolute(u64 Offset) override { return false; }

  bool SeekRelative(s64 Offset) override
  {
    if (Offset < 0)
      return false;
    else if (Offset == 0)
      return true;

    s64 remaining = Offset;
    for (;;)
    {
      const s64 skip = std::min<s64>(m_output_buffer_wpos - m_output_buffer_rpos, remaining);
      remaining -= skip;
      m_output_buffer_rpos += static_cast<u32>(skip);
      if (remaining == 0)
        return true;
      else if (!Decompress())
        return false;
    }
  }

  bool SeekToEnd() override { return false; }

  u64 GetPosition() const override { return 0; }

  u64 GetSize() const override { return 0; }

  bool Flush() override { return true; }

  bool Discard() override { return true; }

  bool Commit() override { return true; }

private:
  enum : u32
  {
    INPUT_BUFFER_SIZE = 65536,
    OUTPUT_BUFFER_SIZE = 131072,
  };

  bool Decompress()
  {
    if (m_output_buffer_rpos != m_output_buffer_wpos)
    {
      const u32 move_size = m_output_buffer_wpos - m_output_buffer_rpos;
      std::memmove(&m_output_buffer[0], &m_output_buffer[m_output_buffer_rpos], move_size);
      m_output_buffer_rpos = move_size;
      m_output_buffer_wpos = move_size;
    }
    else
    {
      m_output_buffer_rpos = 0;
      m_output_buffer_wpos = 0;
    }

    ZSTD_outBuffer outbuf = {m_output_buffer, OUTPUT_BUFFER_SIZE - m_output_buffer_wpos, 0};
    while (outbuf.pos == 0)
    {
      if (m_in_buffer.pos == m_in_buffer.size && !m_errorState)
      {
        const u32 requested_size = std::min<u32>(m_bytes_remaining, INPUT_BUFFER_SIZE);
        const u32 bytes_read = m_src_stream->Read(m_input_buffer, requested_size);
        m_in_buffer.size = bytes_read;
        m_in_buffer.pos = 0;
        m_bytes_remaining -= bytes_read;
        if (bytes_read != requested_size || m_bytes_remaining == 0)
        {
          m_errorState = true;
          break;
        }
      }

      size_t ret = ZSTD_decompressStream(m_cstream, &outbuf, &m_in_buffer);
      if (ZSTD_isError(ret))
      {
        Log_ErrorPrintf("ZSTD_decompressStream() failed: %u (%s)", static_cast<unsigned>(ZSTD_getErrorCode(ret)),
                        ZSTD_getErrorString(ZSTD_getErrorCode(ret)));
        m_in_buffer.pos = m_in_buffer.size;
        m_output_buffer_rpos = 0;
        m_output_buffer_wpos = 0;
        m_errorState = true;
        return false;
      }
    }

    m_output_buffer_wpos = static_cast<u32>(outbuf.pos);
    return true;
  }

  ByteStream* m_src_stream;
  ZSTD_DStream* m_cstream = nullptr;
  ZSTD_inBuffer m_in_buffer = {};
  u32 m_output_buffer_rpos = 0;
  u32 m_output_buffer_wpos = 0;
  u32 m_bytes_remaining;
  bool m_errorState = false;

  u8 m_input_buffer[INPUT_BUFFER_SIZE];
  u8 m_output_buffer[OUTPUT_BUFFER_SIZE];
};
} // namespace

std::unique_ptr<ByteStream> ByteStream::CreateZstdDecompressStream(ByteStream* src_stream, u32 compressed_size)
{
  return std::make_unique<ZstdDecompressStream>(src_stream, compressed_size);
}
