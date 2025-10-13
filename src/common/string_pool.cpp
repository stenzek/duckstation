// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "string_pool.h"

BumpStringPool::BumpStringPool() = default;

BumpStringPool::~BumpStringPool() = default;

std::string_view BumpStringPool::GetString(Offset offset) const
{
  if (offset == InvalidOffset || offset >= m_buffer.size())
    return {};

  const char* str = m_buffer.data() + offset;
  return std::string_view(str);
}

void BumpStringPool::Clear()
{
  m_buffer.clear();
}

size_t BumpStringPool::GetSize() const
{
  return m_buffer.size();
}

bool BumpStringPool::IsEmpty() const
{
  return m_buffer.empty();
}

void BumpStringPool::Reserve(size_t size)
{
  m_buffer.reserve(size);
}

BumpStringPool::Offset BumpStringPool::AddString(std::string_view str)
{
  if (str.empty())
    return InvalidOffset;

  const Offset offset = static_cast<Offset>(m_buffer.size());
  const size_t required_size = str.size() + 1; // +1 for null terminator

  m_buffer.reserve(m_buffer.size() + required_size);
  m_buffer.insert(m_buffer.end(), str.begin(), str.end());
  m_buffer.push_back('\0');

  return offset;
}

std::string_view StringPool::GetString(Offset offset) const
{
  if (offset == InvalidOffset || offset >= m_buffer.size())
    return {};

  const char* str = m_buffer.data() + offset;
  return std::string_view(str);
}

void StringPool::Clear()
{
  m_buffer.clear();
  m_string_map.clear();
}

size_t StringPool::GetSize() const
{
  return m_buffer.size();
}

bool StringPool::IsEmpty() const
{
  return m_buffer.empty();
}

size_t StringPool::GetCount() const
{
  return m_string_map.size();
}

void StringPool::Reserve(size_t size)
{
  m_buffer.reserve(size);
}

StringPool::StringPool() = default;

StringPool::~StringPool() = default;

StringPool::Offset StringPool::AddString(std::string_view str)
{
  if (str.empty())
    return InvalidOffset;

  // Check if string already exists
  auto it = m_string_map.find(str);
  if (it != m_string_map.end())
    return it->second;

  // Add new string to buffer
  const Offset offset = static_cast<Offset>(m_buffer.size());
  const size_t required_size = str.size() + 1; // +1 for null terminator

  m_buffer.reserve(m_buffer.size() + required_size);
  m_buffer.insert(m_buffer.end(), str.begin(), str.end());
  m_buffer.push_back('\0');

  // Store string_view pointing to buffer in map
  std::string_view stored_str(m_buffer.data() + offset, str.size());
  m_string_map.emplace(stored_str, offset);

  return offset;
}
