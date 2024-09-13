// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include "common/heap_array.h"
#include "common/small_string.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Error;

namespace BIOS {
enum : u32
{
  BIOS_BASE = 0x1FC00000,
  BIOS_SIZE = 0x80000,
  BIOS_SIZE_PS2 = 0x400000,
  BIOS_SIZE_PS3 = 0x3E66F0
};

struct ImageInfo
{
  static constexpr u32 HASH_SIZE = 16;
  using Hash = std::array<u8, HASH_SIZE>;

  enum class FastBootPatch : u8
  {
    Unsupported,
    Type1,
    Type2,
  };

  const char* description;
  ConsoleRegion region;
  Hash hash;
  FastBootPatch fastboot_patch;
  u8 priority;

  bool SupportsFastBoot() const { return (fastboot_patch != FastBootPatch::Unsupported); }

  static TinyString GetHashString(const Hash& hash);
};

struct Image
{
  const ImageInfo* info;
  ImageInfo::Hash hash;
  DynamicHeapArray<u8> data;
};

#pragma pack(push, 1)
struct PSEXEHeader
{
  char id[8];            // 0x000-0x007 PS-X EXE
  char pad1[8];          // 0x008-0x00F
  u32 initial_pc;        // 0x010
  u32 initial_gp;        // 0x014
  u32 load_address;      // 0x018
  u32 file_size;         // 0x01C excluding 0x800-byte header
  u32 unk0;              // 0x020
  u32 unk1;              // 0x024
  u32 memfill_start;     // 0x028
  u32 memfill_size;      // 0x02C
  u32 initial_sp_base;   // 0x030
  u32 initial_sp_offset; // 0x034
  u32 reserved[5];       // 0x038-0x04B
  char marker[0x7B4];    // 0x04C-0x7FF
};
static_assert(sizeof(PSEXEHeader) == 0x800);
#pragma pack(pop)

std::optional<Image> LoadImageFromFile(const char* filename, Error* error);

bool IsValidBIOSForRegion(ConsoleRegion console_region, ConsoleRegion bios_region);

bool PatchBIOSFastBoot(u8* image, u32 image_size, ImageInfo::FastBootPatch type);

bool IsValidPSExeHeader(const PSEXEHeader& header, size_t file_size);
DiscRegion GetPSExeDiscRegion(const PSEXEHeader& header);

/// Loads the BIOS image for the specified region.
std::optional<Image> GetBIOSImage(ConsoleRegion region, Error* error);

/// Searches for a BIOS image for the specified region in the specified directory. If no match is found, the first
/// BIOS image within 512KB and 4MB will be used.
std::optional<Image> FindBIOSImageInDirectory(ConsoleRegion region, const char* directory, Error* error);

/// Returns a list of filenames and descriptions for BIOS images in a directory.
std::vector<std::pair<std::string, const BIOS::ImageInfo*>> FindBIOSImagesInDirectory(const char* directory);

/// Returns true if any BIOS images are found in the configured BIOS directory.
bool HasAnyBIOSImages();
} // namespace BIOS