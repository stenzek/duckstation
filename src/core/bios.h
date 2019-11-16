#pragma once
#include "types.h"
#include <optional>
#include <string_view>
#include <vector>

namespace BIOS {
enum : u32
{
  BIOS_BASE = 0x1FC00000,
  BIOS_SIZE = 0x80000
};

using Image = std::vector<u8>;

struct Hash
{
  u8 bytes[16];

  ALWAYS_INLINE bool operator==(const Hash& bh) const { return (std::memcmp(bytes, bh.bytes, sizeof(bytes)) == 0); }
  ALWAYS_INLINE bool operator!=(const Hash& bh) const { return (std::memcmp(bytes, bh.bytes, sizeof(bytes)) != 0); }

  std::string ToString() const;
};

Hash GetHash(const Image& image);
std::optional<Image> LoadImageFromFile(std::string_view filename);
std::optional<Hash> GetHashForFile(std::string_view filename);

bool IsValidHashForRegion(ConsoleRegion region, const Hash& hash);

void PatchBIOS(Image& image, u32 address, u32 value, u32 mask = UINT32_C(0xFFFFFFFF));

bool PatchBIOSEnableTTY(Image& image, const Hash& hash);
bool PatchBIOSFastBoot(Image& image, const Hash& hash);
bool PatchBIOSForEXE(Image& image, u32 r_pc, u32 r_gp, u32 r_sp, u32 r_fp);
} // namespace BIOS