// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "heterogeneous_containers.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class BumpStringPool
{
public:
  using Offset = size_t;
  static constexpr Offset InvalidOffset = static_cast<Offset>(-1);

  BumpStringPool();
  ~BumpStringPool();

  // Adds a string to the pool and returns its offset
  [[nodiscard]] Offset AddString(std::string_view str);

  // Retrieves a string view from the pool using an offset
  [[nodiscard]] std::string_view GetString(Offset offset) const;

  // Clears all strings from the pool
  void Clear();

  // Returns the total size of the pool in bytes
  [[nodiscard]] size_t GetSize() const;

  // Returns whether the pool is empty
  [[nodiscard]] bool IsEmpty() const;

  // Reserves space in the buffer to avoid frequent reallocations
  void Reserve(size_t size);

private:
  std::vector<char> m_buffer;
};

class StringPool
{
public:
  using Offset = size_t;
  static constexpr Offset InvalidOffset = static_cast<Offset>(-1);

  StringPool();
  ~StringPool();

  // Adds a string to the pool and returns its offset. If the string already exists, returns the existing offset.
  [[nodiscard]] Offset AddString(std::string_view str);

  // Retrieves a string view from the pool using an offset
  [[nodiscard]] std::string_view GetString(Offset offset) const;

  // Clears all strings from the pool
  void Clear();

  // Returns the total size of the pool in bytes
  [[nodiscard]] size_t GetSize() const;

  // Returns whether the pool is empty
  [[nodiscard]] bool IsEmpty() const;

  // Returns the number of unique strings in the pool
  [[nodiscard]] size_t GetCount() const;

  // Reserves space in the buffer to avoid frequent reallocations
  void Reserve(size_t size);

private:
  std::vector<char> m_buffer;
  UnorderedStringMap<Offset> m_string_map;
};
