// SPDX-FileCopyrightText: 2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "memory_card_image.h"

class MemoryCardIconCache
{
public:
  MemoryCardIconCache(std::string filename);
  ~MemoryCardIconCache();

  bool Reload();

  // NOTE: Only valid within this call to lookup.
  const MemoryCardImage::IconFrame* Lookup(std::string_view serial, std::string_view path);

private:
  enum : u32
  {
    MAX_SERIAL_LENGTH = 31,
  };

#pragma pack(push, 1)
  struct Entry
  {
    char serial[MAX_SERIAL_LENGTH];
    bool is_valid;
    s64 memcard_timestamp;
    MemoryCardImage::IconFrame icon;
  };
#pragma pack(pop)

  bool UpdateInFile(const Entry& entry);

  std::string m_filename;
  std::vector<Entry> m_entries;
};
