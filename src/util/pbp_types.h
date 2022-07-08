#pragma once
#include "common/types.h"
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace PBP {

enum : u32
{
  PBP_HEADER_OFFSET_COUNT = 8u,
  TOC_NUM_ENTRIES = 102u,
  BLOCK_TABLE_NUM_ENTRIES = 32256u,
  DISC_TABLE_NUM_ENTRIES = 5u,
  DECOMPRESSED_BLOCK_SIZE = 37632u // 2352 bytes per sector * 16 sectors per block
};

#pragma pack(push, 1)

struct PBPHeader
{
  u8 magic[4]; // "\0PBP"
  u32 version;

  union
  {
    u32 offsets[PBP_HEADER_OFFSET_COUNT];

    struct
    {
      u32 param_sfo_offset; // 0x00000028
      u32 icon0_png_offset;
      u32 icon1_png_offset;
      u32 pic0_png_offset;
      u32 pic1_png_offset;
      u32 snd0_at3_offset;
      u32 data_psp_offset;
      u32 data_psar_offset;
    };
  };
};
static_assert(sizeof(PBPHeader) == 0x28);

struct SFOHeader
{
  u8 magic[4]; // "\0PSF"
  u32 version;
  u32 key_table_offset;  // Relative to start of SFOHeader, 0x000000A4 expected
  u32 data_table_offset; // Relative to start of SFOHeader, 0x00000100 expected
  u32 num_table_entries; // 0x00000009
};
static_assert(sizeof(SFOHeader) == 0x14);

struct SFOIndexTableEntry
{
  u16 key_offset; // Relative to key_table_offset
  u16 data_type;
  u32 data_size;       // Size of actual data in bytes
  u32 data_total_size; // Size of data field in bytes, data_total_size >= data_size
  u32 data_offset;     // Relative to data_table_offset
};
static_assert(sizeof(SFOIndexTableEntry) == 0x10);

using SFOIndexTable = std::vector<SFOIndexTableEntry>;
using SFOTableDataValue = std::variant<std::string, u32>;
using SFOTable = std::map<std::string, SFOTableDataValue>;

struct BlockTableEntry
{
  u32 offset;
  u16 size;
  u16 marker;
  u8 checksum[0x10];
  u64 padding;
};
static_assert(sizeof(BlockTableEntry) == 0x20);

struct TOCEntry
{
  struct Timecode
  {
    u8 m;
    u8 s;
    u8 f;
  };

  u8 type;
  u8 unknown;
  u8 point;
  Timecode pregap_start;
  u8 zero;
  Timecode userdata_start;
};
static_assert(sizeof(TOCEntry) == 0x0A);

#if 0
struct AudioTrackTableEntry
{
  u32 block_offset;
  u32 block_size;
  u32 block_padding;
  u32 block_checksum;
};
static_assert(sizeof(CDDATrackTableEntry) == 0x10);
#endif

#pragma pack(pop)

} // namespace PBP
