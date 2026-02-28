// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "string_pool.h"

#include <algorithm>

BumpStringPool::BumpStringPool() = default;

BumpStringPool::~BumpStringPool() = default;

std::string_view BumpStringPool::GetString(Offset offset) const
{
  if (offset == InvalidOffset || offset >= m_buffer.size())
    return {};

  const char* str = m_buffer.data() + offset;
  return std::string_view(str);
}

std::string_view BumpStringPool::GetString(Offset offset, size_t length) const
{
  if ((offset + length) > m_buffer.size())
    return {};

  return std::string_view(m_buffer.data() + offset, length);
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

BumpUniqueStringPool::BumpUniqueStringPool() = default;

BumpUniqueStringPool::~BumpUniqueStringPool() = default;

std::string_view BumpUniqueStringPool::GetStringRefView(const StringRef& ref) const
{
  return std::string_view(m_buffer.data() + ref.offset, ref.length);
}

BumpUniqueStringPool::Offset BumpUniqueStringPool::AddString(std::string_view str)
{
  if (str.empty())
    return InvalidOffset;

  // Binary search the sorted refs array for an existing match.
  auto it = std::lower_bound(m_sorted_refs.begin(), m_sorted_refs.end(), str,
                             [this](const StringRef& ref, std::string_view s) { return GetStringRefView(ref) < s; });
  if (it != m_sorted_refs.end() && GetStringRefView(*it) == str)
    return it->offset;

  // Not found, append to the buffer.
  const Offset offset = static_cast<Offset>(m_buffer.size());
  m_buffer.insert(m_buffer.end(), str.begin(), str.end());
  m_buffer.push_back('\0');

  // Insert into sorted position.
  m_sorted_refs.insert(it, StringRef{offset, str.size()});

  return offset;
}

std::string_view BumpUniqueStringPool::GetString(Offset offset) const
{
  if (offset >= m_buffer.size())
    return {};

  const char* str = m_buffer.data() + offset;
  return std::string_view(str);
}

std::string_view BumpUniqueStringPool::GetString(Offset offset, size_t length) const
{
  if ((offset + length) > m_buffer.size())
    return {};

  return std::string_view(m_buffer.data() + offset, length);
}

void BumpUniqueStringPool::Clear()
{
  m_buffer.clear();
  m_sorted_refs.clear();
}

size_t BumpUniqueStringPool::GetSize() const
{
  return m_buffer.size();
}

bool BumpUniqueStringPool::IsEmpty() const
{
  return m_buffer.empty();
}

size_t BumpUniqueStringPool::GetCount() const
{
  return m_sorted_refs.size();
}

void BumpUniqueStringPool::Reserve(size_t num_strings, size_t storage_size)
{
  m_sorted_refs.reserve(num_strings);
  m_buffer.reserve(storage_size);
}

std::string_view StringPool::GetString(Offset offset) const
{
  if (offset >= m_buffer.size())
    return {};

  const char* str = m_buffer.data() + offset;
  return std::string_view(str);
}

std::string_view StringPool::GetString(Offset offset, size_t length) const
{
  if ((offset + length) > m_buffer.size())
    return {};

  return std::string_view(m_buffer.data() + offset, length);
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
