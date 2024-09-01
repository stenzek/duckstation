// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "binary_reader_writer.h"
#include "assert.h"
#include "error.h"
#include "small_string.h"

#include "common/file_system.h"

BinarySpanReader::BinarySpanReader() = default;

BinarySpanReader::BinarySpanReader(std::span<const u8> buf) : m_buf(buf)
{
}

BinarySpanReader::BinarySpanReader(BinarySpanReader&& move) : m_buf(std::move(move.m_buf)), m_pos(move.m_pos)
{
  move.m_pos = 0;
}

BinarySpanReader& BinarySpanReader::operator=(BinarySpanReader&& move)
{
  m_buf = std::move(move.m_buf);
  m_pos = move.m_pos;
  move.m_pos = 0;
  return *this;
}

bool BinarySpanReader::PeekCString(std::string_view* dst)
{
  size_t pos = m_pos;
  size_t size = 0;
  while (pos < m_buf.size())
  {
    if (m_buf[pos] == 0)
      break;

    pos++;
    size++;
  }

  if (pos == m_buf.size())
    return false;

  *dst = std::string_view(reinterpret_cast<const char*>(&m_buf[m_pos]), size);
  return true;
}

bool BinarySpanReader::PeekSizePrefixedString(std::string_view* dst)
{
  u32 length;
  if (!PeekU32(&length) || (m_pos + sizeof(length) + length) > m_buf.size()) [[unlikely]]
    return false;

  *dst = std::string_view(reinterpret_cast<const char*>(&m_buf[m_pos + sizeof(length)]), length);
  return true;
}

std::span<const u8> BinarySpanReader::GetRemainingSpan(size_t size) const
{
  DebugAssert(size <= GetBufferRemaining());
  return m_buf.subspan(m_pos, size);
}

std::span<const u8> BinarySpanReader::GetRemainingSpan() const
{
  return m_buf.subspan(m_pos, m_buf.size() - m_pos);
}

void BinarySpanReader::IncrementPosition(size_t size)
{
  DebugAssert(size < GetBufferRemaining());
  m_pos += size;
}

bool BinarySpanReader::ReadCString(std::string* dst)
{
  std::string_view sv;
  if (!PeekCString(&sv))
    return false;

  dst->assign(sv);
  m_pos += sv.size() + 1;
  return true;
}

bool BinarySpanReader::ReadCString(std::string_view* dst)
{
  if (!PeekCString(dst))
    return false;

  m_pos += dst->size() + 1;
  return true;
}

bool BinarySpanReader::ReadCString(SmallStringBase* dst)
{
  std::string_view sv;
  if (!PeekCString(&sv))
    return false;

  dst->assign(sv);
  m_pos += sv.size() + 1;
  return true;
}

bool BinarySpanReader::ReadSizePrefixedString(std::string* dst)
{
  std::string_view sv;
  if (!PeekSizePrefixedString(&sv))
    return false;

  dst->assign(sv);
  m_pos += sizeof(u32) + sv.size();
  return true;
}

bool BinarySpanReader::ReadSizePrefixedString(std::string_view* dst)
{
  if (!PeekSizePrefixedString(dst))
    return false;

  m_pos += sizeof(u32) + dst->size();
  return true;
}

bool BinarySpanReader::ReadSizePrefixedString(SmallStringBase* dst)
{
  std::string_view sv;
  if (!PeekSizePrefixedString(&sv))
    return false;

  dst->assign(sv);
  m_pos += sizeof(u32) + sv.size();
  return true;
}

std::string_view BinarySpanReader::ReadCString()
{
  std::string_view ret;
  if (PeekCString(&ret))
    m_pos += ret.size() + 1;
  return ret;
}

std::string_view BinarySpanReader::ReadSizePrefixedString()
{
  std::string_view ret;
  if (PeekSizePrefixedString(&ret))
    m_pos += sizeof(u32) + ret.size();
  return ret;
}

bool BinarySpanReader::PeekCString(std::string* dst)
{
  std::string_view sv;
  if (!PeekCString(&sv))
    return false;

  dst->assign(sv);
  return true;
}

bool BinarySpanReader::PeekCString(SmallStringBase* dst)
{
  std::string_view sv;
  if (!PeekCString(&sv))
    return false;

  dst->assign(sv);
  return true;
}

bool BinarySpanReader::PeekSizePrefixedString(std::string* dst)
{
  std::string_view sv;
  if (!PeekSizePrefixedString(&sv))
    return false;

  dst->assign(sv);
  return true;
}

bool BinarySpanReader::PeekSizePrefixedString(SmallStringBase* dst)
{
  std::string_view sv;
  if (!PeekSizePrefixedString(&sv))
    return false;

  dst->assign(sv);
  return true;
}

BinarySpanWriter::BinarySpanWriter() = default;

BinarySpanWriter::BinarySpanWriter(std::span<u8> buf) : m_buf(buf)
{
}

BinarySpanWriter::BinarySpanWriter(BinarySpanWriter&& move) : m_buf(std::move(move.m_buf)), m_pos(move.m_pos)
{
  move.m_pos = 0;
}

BinarySpanWriter& BinarySpanWriter::operator=(BinarySpanWriter&& move)
{
  m_buf = std::move(move.m_buf);
  m_pos = move.m_pos;
  move.m_pos = 0;
  return *this;
}

std::span<u8> BinarySpanWriter::GetRemainingSpan(size_t size) const
{
  DebugAssert(size <= GetBufferRemaining());
  return m_buf.subspan(m_pos, size);
}

std::span<u8> BinarySpanWriter::GetRemainingSpan() const
{
  return m_buf.subspan(m_pos, m_buf.size() - m_pos);
}

void BinarySpanWriter::IncrementPosition(size_t size)
{
  DebugAssert(size < GetBufferRemaining());
  m_pos += size;
}

bool BinarySpanWriter::WriteCString(std::string_view val)
{
  if ((m_pos + val.size() + 1) > m_buf.size()) [[unlikely]]
    return false;

  if (!val.empty())
    std::memcpy(&m_buf[m_pos], val.data(), val.size());

  m_buf[m_pos + val.size()] = 0;
  m_pos += val.size() + 1;
  return true;
}

bool BinarySpanWriter::WriteSizePrefixedString(std::string_view val)
{
  if (val.size() > std::numeric_limits<u32>::max() || (m_pos + sizeof(u32) + val.size()) > m_buf.size()) [[unlikely]]
    return false;

  const u32 usize = static_cast<u32>(val.size());
  std::memcpy(&m_buf[m_pos], &usize, sizeof(usize));
  m_pos += sizeof(usize);
  if (val.size() > 0)
  {
    std::memcpy(&m_buf[m_pos], val.data(), val.size());
    m_pos += val.size();
  }

  return true;
}

BinaryFileReader::BinaryFileReader() : m_fp(nullptr), m_size(0), m_good(false)
{
}

BinaryFileReader::BinaryFileReader(std::FILE* fp)
  : m_fp(fp), m_size(fp ? FileSystem::FSize64(fp) : 0), m_good(fp != nullptr)
{
}

BinaryFileReader::BinaryFileReader(BinaryFileReader&& move) : m_fp(move.m_fp), m_size(move.m_size), m_good(move.m_good)
{
  move.m_fp = nullptr;
  move.m_size = 0;
  move.m_good = false;
}

BinaryFileReader& BinaryFileReader::operator=(BinaryFileReader&& move)
{
  m_fp = move.m_fp;
  m_size = move.m_size;
  m_good = move.m_good;

  move.m_fp = nullptr;
  move.m_size = 0;
  move.m_good = false;

  return *this;
}

bool BinaryFileReader::IsAtEnd()
{
  return (FileSystem::FTell64(m_fp) == m_size);
}

bool BinaryFileReader::ReadCString(std::string* dst)
{
  dst->clear();

  while (m_good)
  {
    u8 val;
    if ((m_good = std::fread(&val, sizeof(val), 1, m_fp) == 1))
    {
      if (val == 0)
        break;
      else
        dst->push_back(static_cast<char>(val));
    }
  }

  return m_good;
}

bool BinaryFileReader::ReadCString(SmallStringBase* dst)
{
  dst->clear();

  while (m_good)
  {
    u8 val;
    if ((m_good = std::fread(&val, sizeof(val), 1, m_fp) == 1))
    {
      if (val == 0)
        break;
      else
        dst->push_back(static_cast<char>(val));
    }
  }

  return m_good;
}

bool BinaryFileReader::ReadSizePrefixedString(std::string* dst)
{
  u32 length;
  if (!ReadU32(&length)) [[unlikely]]
    return false;

  dst->resize(length);
  return (length == 0 || Read(dst->data(), dst->length()));
}

bool BinaryFileReader::ReadSizePrefixedString(SmallStringBase* dst)
{
  u32 length;
  if (!ReadU32(&length)) [[unlikely]]
    return false;

  dst->resize(length);
  return (length == 0 || Read(dst->data(), dst->length()));
}

std::string BinaryFileReader::ReadCString()
{
  std::string ret;
  if (!ReadCString(&ret))
    ret = {};
  return ret;
}

std::string BinaryFileReader::ReadSizePrefixedString()
{
  std::string ret;
  if (!ReadSizePrefixedString(&ret))
    ret = {};
  return ret;
}

BinaryFileWriter::BinaryFileWriter() : m_fp(nullptr), m_good(false)
{
}

BinaryFileWriter::BinaryFileWriter(std::FILE* fp) : m_fp(fp), m_good(fp != nullptr)
{
}

BinaryFileWriter::BinaryFileWriter(BinaryFileWriter&& move) : m_fp(move.m_fp), m_good(move.m_good)
{
  move.m_fp = nullptr;
  move.m_good = false;
}

BinaryFileWriter& BinaryFileWriter::operator=(BinaryFileWriter&& move)
{
  m_fp = move.m_fp;
  m_good = move.m_good;

  move.m_fp = nullptr;
  move.m_good = false;

  return *this;
}

bool BinaryFileWriter::WriteCString(std::string_view val)
{
  if (!val.empty() && (!m_good && std::fwrite(val.data(), val.length(), 1, m_fp) != 1)) [[unlikely]]
    return false;

  const u8 terminator = 0;
  return (m_good = (m_good && std::fwrite(&terminator, 1, 1, m_fp) == 1));
}

bool BinaryFileWriter::WriteSizePrefixedString(std::string_view val)
{
  if (val.size() > std::numeric_limits<u32>::max()) [[unlikely]]
    return false;

  const u32 usize = static_cast<u32>(val.size());
  return (m_good = (m_good && std::fwrite(&usize, sizeof(usize), 1, m_fp) == 1 &&
                    (val.empty() || std::fwrite(val.data(), val.size(), 1, m_fp) == 1)));
}

bool BinaryFileWriter::Flush(Error* error)
{
  if (!m_good)
  {
    Error::SetStringView(error, "Write error previously occurred.");
    return false;
  }

  if (!(m_good = (m_good && std::fflush(m_fp) == 0)))
  {
    Error::SetErrno(error, "fflush() failed: ", errno);
    return false;
  }

  return true;
}
