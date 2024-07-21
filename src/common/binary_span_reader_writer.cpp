// SPDX-FileCopyrightText: 2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "binary_span_reader_writer.h"
#include "assert.h"
#include "small_string.h"

BinarySpanReader::BinarySpanReader() = default;

BinarySpanReader::BinarySpanReader(std::span<const u8> buf) : m_buf(buf)
{
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

std::string_view BinarySpanReader::ReadCString()
{
  std::string_view ret;
  if (PeekCString(&ret))
    m_pos += ret.size() + 1;
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
  m_pos += sv.size() + 1;
  return true;
}

BinarySpanWriter::BinarySpanWriter() = default;

BinarySpanWriter::BinarySpanWriter(std::span<u8> buf) : m_buf(buf)
{
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
