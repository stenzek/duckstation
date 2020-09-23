#include "bios.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/md5_digest.h"
#include "cpu_disasm.h"
#include <array>
#include <cerrno>
Log_SetChannel(BIOS);

namespace BIOS {
static constexpr Hash MakeHashFromString(const char str[])
{
  Hash h{};
  for (int i = 0; str[i] != '\0'; i++)
  {
    u8 nibble = 0;
    char ch = str[i];
    if (ch >= '0' && ch <= '9')
      nibble = str[i] - '0';
    else if (ch >= 'a' && ch <= 'z')
      nibble = 0xA + (str[i] - 'a');
    else if (ch >= 'A' && ch <= 'Z')
      nibble = 0xA + (str[i] - 'A');

    h.bytes[i / 2] |= nibble << (((i & 1) ^ 1) * 4);
  }
  return h;
}

std::string Hash::ToString() const
{
  char str[33];
  std::snprintf(str, sizeof(str), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", bytes[0],
                bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
                bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  return str;
}

static constexpr std::array<ImageInfo, 12> s_image_infos = {{
  {"SCPH-1000, DTL-H1000 (v1.0 J)", ConsoleRegion::NTSC_J, MakeHashFromString("239665b1a3dade1b5a52c06338011044")},
  {"SCPH-1001, 5003, DTL-H1201, H3001 (v2.2 12-04-95 A)", ConsoleRegion::NTSC_U,
   MakeHashFromString("924e392ed05558ffdb115408c263dccf")},
  {"SCPH-1002, DTL-H1002 (v2.0 05-10-95 E)", ConsoleRegion::PAL,
   MakeHashFromString("54847e693405ffeb0359c6287434cbef")},
  {"SCPH-3000, DTL-H1000H (v1.1 01-22-95)", ConsoleRegion::NTSC_J,
   MakeHashFromString("849515939161e62f6b866f6853006780")},
  {"SCPH-5500 (v3.0 09-09-96 J)", ConsoleRegion::NTSC_J, MakeHashFromString("8dd7d5296a650fac7319bce665a6a53c")},
  {"SCPH-5501, 5503, 7003 (v3.0 11-18-96 A)", ConsoleRegion::NTSC_U,
   MakeHashFromString("490f666e1afb15b7362b406ed1cea246")},
  {"SCPH-5502, 5552 (v3.0 01-06-97 E)", ConsoleRegion::PAL, MakeHashFromString("32736f17079d0b2b7024407c39bd3050")},
  {"SCPH-7000, 7500, 9000 (v4.0 08-18-97 J)", ConsoleRegion::NTSC_J,
   MakeHashFromString("8e4c14f567745eff2f0408c8129f72a6")},
  {"SCPH-7000W (v4.1 11-14-97 A)", ConsoleRegion::NTSC_J, MakeHashFromString("b84be139db3ee6cbd075630aa20a6553")},
  {"SCPH-7001, 7501, 7503, 9001, 9003, 9903 (v4.1 12-16-97 A)", ConsoleRegion::NTSC_U,
   MakeHashFromString("1e68c231d0896b7eadcad1d7d8e76129")},
  {"SCPH-7002, 7502, 9002 (v4.1 12-16-97 E)", ConsoleRegion::PAL,
   MakeHashFromString("b9d9a0286c33dc6b7237bb13cd46fdee")},
  {"POPS-660 (PSP)", ConsoleRegion::Auto, MakeHashFromString("c53ca5908936d412331790f4426c6c33")},
}};

Hash GetHash(const Image& image)
{
  Hash hash;
  MD5Digest digest;
  digest.Update(image.data(), static_cast<u32>(image.size()));
  digest.Final(hash.bytes);
  return hash;
}

std::optional<Image> LoadImageFromFile(const char* filename)
{
  Image ret(BIOS_SIZE);
  auto fp = FileSystem::OpenManagedCFile(filename, "rb");
  if (!fp)
  {
    Log_ErrorPrintf("Failed to open BIOS image '%s', errno=%d", filename, errno);
    return std::nullopt;
  }

  std::fseek(fp.get(), 0, SEEK_END);
  const u32 size = static_cast<u32>(std::ftell(fp.get()));
  std::fseek(fp.get(), 0, SEEK_SET);

  if (size != BIOS_SIZE)
  {
    Log_ErrorPrintf("BIOS image '%s' mismatch, expecting %u bytes, got %u bytes", filename, BIOS_SIZE, size);
    return std::nullopt;
  }

  if (std::fread(ret.data(), 1, ret.size(), fp.get()) != ret.size())
  {
    Log_ErrorPrintf("Failed to read BIOS image '%s'", filename);
    return std::nullopt;
  }

  return ret;
}

std::optional<Hash> GetHashForFile(const char* filename)
{
  auto image = LoadImageFromFile(filename);
  if (!image)
    return std::nullopt;

  return GetHash(*image);
}

const ImageInfo* GetImageInfoForHash(const Hash& hash)
{
  for (const ImageInfo& ii : s_image_infos)
  {
    if (ii.hash == hash)
      return &ii;
  }

  return nullptr;
}

bool IsValidHashForRegion(ConsoleRegion region, const Hash& hash)
{
  const ImageInfo* ii = GetImageInfoForHash(hash);
  if (!ii)
    return false;

  return (ii->region == ConsoleRegion::Auto || ii->region == region);
}

void PatchBIOS(Image& bios, u32 address, u32 value, u32 mask /*= UINT32_C(0xFFFFFFFF)*/)
{
  const u32 phys_address = address & UINT32_C(0x1FFFFFFF);
  const u32 offset = phys_address - BIOS_BASE;
  Assert(phys_address >= BIOS_BASE && offset < BIOS_SIZE);

  u32 existing_value;
  std::memcpy(&existing_value, &bios[offset], sizeof(existing_value));
  u32 new_value = (existing_value & ~mask) | value;
  std::memcpy(&bios[offset], &new_value, sizeof(new_value));

  SmallString old_disasm, new_disasm;
  CPU::DisassembleInstruction(&old_disasm, address, existing_value);
  CPU::DisassembleInstruction(&new_disasm, address, new_value);
  Log_DevPrintf("BIOS-Patch 0x%08X (+0x%X): 0x%08X %s -> %08X %s", address, offset, existing_value,
                old_disasm.GetCharArray(), new_value, new_disasm.GetCharArray());
}

bool PatchBIOSEnableTTY(Image& image, const Hash& hash)
{
  const ImageInfo* ii = GetImageInfoForHash(hash);
  if (!ii)
  {
    Log_WarningPrintf("Incompatible version for TTY patch: %s", hash.ToString().c_str());
    return false;
  }

  Log_InfoPrintf("Patching BIOS to enable TTY/printf");
  PatchBIOS(image, 0x1FC06F0C, 0x24010001);
  PatchBIOS(image, 0x1FC06F14, 0xAF81A9C0);
  return true;
}

bool PatchBIOSFastBoot(Image& image, const Hash& hash)
{
  const ImageInfo* ii = GetImageInfoForHash(hash);
  if (!ii)
  {
    Log_WarningPrintf("Incompatible version for fast-boot patch: %s", hash.ToString().c_str());
    return false;
  }

  // Replace the shell entry point with a return back to the bootstrap.
  Log_InfoPrintf("Patching BIOS to skip intro");
  PatchBIOS(image, 0x1FC18000, 0x03E00008);
  PatchBIOS(image, 0x1FC18004, 0x00000000);
  return true;
}

bool PatchBIOSForEXE(Image& image, u32 r_pc, u32 r_gp, u32 r_sp, u32 r_fp)
{
  // pc has to be done first because we can't load it in the delay slot
  PatchBIOS(image, 0xBFC06FF0, UINT32_C(0x3C080000) | r_pc >> 16);                // lui $t0, (r_pc >> 16)
  PatchBIOS(image, 0xBFC06FF4, UINT32_C(0x35080000) | (r_pc & UINT32_C(0xFFFF))); // ori $t0, $t0, (r_pc & 0xFFFF)
  PatchBIOS(image, 0xBFC06FF8, UINT32_C(0x3C1C0000) | r_gp >> 16);                // lui $gp, (r_gp >> 16)
  PatchBIOS(image, 0xBFC06FFC, UINT32_C(0x379C0000) | (r_gp & UINT32_C(0xFFFF))); // ori $gp, $gp, (r_gp & 0xFFFF)

  if (r_sp != 0)
  {
    PatchBIOS(image, 0xBFC07000, UINT32_C(0x3C1D0000) | r_sp >> 16);                // lui $sp, (r_sp >> 16)
    PatchBIOS(image, 0xBFC07004, UINT32_C(0x37BD0000) | (r_sp & UINT32_C(0xFFFF))); // ori $sp, $sp, (r_sp & 0xFFFF)
  }
  else
  {
    PatchBIOS(image, 0xBFC07000, UINT32_C(0x00000000)); // nop
    PatchBIOS(image, 0xBFC07004, UINT32_C(0x00000000)); // nop
  }
  if (r_fp != 0)
  {
    PatchBIOS(image, 0xBFC07008, UINT32_C(0x3C1E0000) | r_fp >> 16);                // lui $fp, (r_fp >> 16)
    PatchBIOS(image, 0xBFC0700C, UINT32_C(0x01000008));                             // jr $t0
    PatchBIOS(image, 0xBFC07010, UINT32_C(0x37DE0000) | (r_fp & UINT32_C(0xFFFF))); // ori $fp, $fp, (r_fp & 0xFFFF)
  }
  else
  {
    PatchBIOS(image, 0xBFC07008, UINT32_C(0x00000000)); // nop
    PatchBIOS(image, 0xBFC0700C, UINT32_C(0x01000008)); // jr $t0
    PatchBIOS(image, 0xBFC07010, UINT32_C(0x00000000)); // nop
  }

  return true;
}

bool IsValidPSExeHeader(const PSEXEHeader& header, u32 file_size)
{
  static constexpr char expected_id[] = {'P', 'S', '-', 'X', ' ', 'E', 'X', 'E'};
  if (std::memcmp(header.id, expected_id, sizeof(expected_id)) != 0)
    return false;

  if (header.file_size > file_size)
    return false;

  return true;
}

} // namespace BIOS
