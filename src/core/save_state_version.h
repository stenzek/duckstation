#pragma once
#include "types.h"

static constexpr u32 SAVE_STATE_MAGIC = 0x43435544;
static constexpr u32 SAVE_STATE_VERSION = 55;
static constexpr u32 SAVE_STATE_MINIMUM_VERSION = 42;

static_assert(SAVE_STATE_VERSION >= SAVE_STATE_MINIMUM_VERSION);

#pragma pack(push, 4)
struct SAVE_STATE_HEADER
{
  enum : u32
  {
    MAX_TITLE_LENGTH = 128,
    MAX_GAME_CODE_LENGTH = 32
  };

  u32 magic;
  u32 version;
  char title[MAX_TITLE_LENGTH];
  char game_code[MAX_GAME_CODE_LENGTH];

  u32 media_filename_length;
  u32 offset_to_media_filename;
  u32 media_subimage_index;
  u32 unused_offset_to_playlist_filename; // Unused as of version 51.

  u32 screenshot_width;
  u32 screenshot_height;
  u32 screenshot_size;
  u32 offset_to_screenshot;

  u32 data_compression_type;
  u32 data_compressed_size;
  u32 data_uncompressed_size;
  u32 offset_to_data;
};
#pragma pack(pop)
