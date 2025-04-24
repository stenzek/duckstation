// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

static constexpr u32 SAVE_STATE_MAGIC = 0x43435544;
static constexpr u32 SAVE_STATE_VERSION = 82;
static constexpr u32 SAVE_STATE_MINIMUM_VERSION = 42;

static_assert(SAVE_STATE_VERSION >= SAVE_STATE_MINIMUM_VERSION);

#pragma pack(push, 4)
struct SAVE_STATE_HEADER
{
  enum : u32
  {
    MAX_TITLE_LENGTH = 128,
    MAX_SERIAL_LENGTH = 32,
    MAX_SAVE_STATE_SIZE = 32 * 1024 * 1024,
  };

  enum class CompressionType : u32
  {
    None = 0,
    Deflate = 1,
    Zstandard = 2,
  };

  u32 magic;
  u32 version;
  char title[MAX_TITLE_LENGTH];
  char serial[MAX_SERIAL_LENGTH];

  u32 media_path_length;
  u32 offset_to_media_path;
  u32 media_subimage_index;
  
  // Screenshot compression added in version 69.
  // Uncompressed size not stored, it can be inferred from width/height.
  u32 screenshot_compression_type;
  u32 screenshot_width;
  u32 screenshot_height;
  u32 screenshot_compressed_size;
  u32 offset_to_screenshot;

  u32 data_compression_type;
  u32 data_compressed_size;
  u32 data_uncompressed_size;
  u32 offset_to_data;
};
#pragma pack(pop)
