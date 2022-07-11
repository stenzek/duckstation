#pragma once
#include "types.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace BIOS {
enum : u32
{
  BIOS_BASE = 0x1FC00000,
  BIOS_SIZE = 0x80000,
  BIOS_SIZE_PS2 = 0x400000,
  BIOS_SIZE_PS3 = 0x3E66F0
};

using Image = std::vector<u8>;

struct Hash
{
  u8 bytes[16];

  ALWAYS_INLINE bool operator==(const Hash& bh) const { return (std::memcmp(bytes, bh.bytes, sizeof(bytes)) == 0); }
  ALWAYS_INLINE bool operator!=(const Hash& bh) const { return (std::memcmp(bytes, bh.bytes, sizeof(bytes)) != 0); }

  std::string ToString() const;
};

struct ImageInfo
{
  const char* description;
  ConsoleRegion region;
  Hash hash;
  bool patch_compatible;
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

Hash GetHash(const Image& image);
std::optional<Image> LoadImageFromFile(const char* filename);
std::optional<Hash> GetHashForFile(const char* filename);

const ImageInfo* GetImageInfoForHash(const Hash& hash);
bool IsValidHashForRegion(ConsoleRegion region, const Hash& hash);

void PatchBIOS(u8* image, u32 image_size, u32 address, u32 value, u32 mask = UINT32_C(0xFFFFFFFF));

bool PatchBIOSEnableTTY(u8* image, u32 image_size, const Hash& hash);
bool PatchBIOSFastBoot(u8* image, u32 image_size, const Hash& hash);
bool PatchBIOSForEXE(u8* image, u32 image_size, u32 r_pc, u32 r_gp, u32 r_sp, u32 r_fp);

bool IsValidPSExeHeader(const PSEXEHeader& header, u32 file_size);
DiscRegion GetPSExeDiscRegion(const PSEXEHeader& header);

/// Loads the BIOS image for the specified region.
std::optional<std::vector<u8>> GetBIOSImage(ConsoleRegion region);

/// Searches for a BIOS image for the specified region in the specified directory. If no match is found, the first
/// BIOS image within 512KB and 4MB will be used.
std::optional<std::vector<u8>> FindBIOSImageInDirectory(ConsoleRegion region, const char* directory);

/// Returns a list of filenames and descriptions for BIOS images in a directory.
std::vector<std::pair<std::string, const BIOS::ImageInfo*>> FindBIOSImagesInDirectory(const char* directory);

/// Returns true if any BIOS images are found in the configured BIOS directory.
bool HasAnyBIOSImages();
} // namespace BIOS