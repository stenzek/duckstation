// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cd_image.h"
#include "cd_subchannel_replacement.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include <algorithm>
#include <cerrno>
#include <map>
#include <unordered_map>

Log_SetChannel(CDImagePPF);

namespace {

enum : u32
{
  DESC_SIZE = 50,
  BLOCKCHECK_SIZE = 1024
};

class CDImagePPF : public CDImage
{
public:
  CDImagePPF();
  ~CDImagePPF() override;

  bool Open(const char* filename, std::unique_ptr<CDImage> parent_image);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;
  s64 GetSizeOnDisk() const override;

  std::string GetMetadata(std::string_view type) const override;
  std::string GetSubImageMetadata(u32 index, std::string_view type) const override;

  PrecacheResult Precache(ProgressCallback* progress = ProgressCallback::NullProgressCallback) override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  bool ReadV1Patch(std::FILE* fp);
  bool ReadV2Patch(std::FILE* fp);
  bool ReadV3Patch(std::FILE* fp);
  u32 ReadFileIDDiz(std::FILE* fp, u32 version);

  bool AddPatch(u64 offset, const u8* patch, u32 patch_size);

  std::unique_ptr<CDImage> m_parent_image;
  std::vector<u8> m_replacement_data;
  std::unordered_map<u32, u32> m_replacement_map;
  s64 m_patch_size = 0;
  u32 m_replacement_offset = 0;
};

} // namespace

CDImagePPF::CDImagePPF() = default;

CDImagePPF::~CDImagePPF() = default;

bool CDImagePPF::Open(const char* filename, std::unique_ptr<CDImage> parent_image)
{
  auto fp = FileSystem::OpenManagedSharedCFile(filename, "rb", FileSystem::FileShareMode::DenyWrite);
  if (!fp)
  {
    ERROR_LOG("Failed to open '{}'", Path::GetFileName(filename));
    return false;
  }

  m_patch_size = FileSystem::FSize64(fp.get());

  u32 magic;
  if (std::fread(&magic, sizeof(magic), 1, fp.get()) != 1)
  {
    ERROR_LOG("Failed to read magic from '{}'", Path::GetFileName(filename));
    return false;
  }

  // work out the offset from the start of the parent image which we need to patch
  // i.e. the two second implicit pregap on data sectors
  if (parent_image->GetTrack(1).mode != TrackMode::Audio)
    m_replacement_offset = parent_image->GetIndex(1).start_lba_on_disc;

  // copy all the stuff from the parent image
  m_filename = parent_image->GetFileName();
  m_tracks = parent_image->GetTracks();
  m_indices = parent_image->GetIndices();
  m_parent_image = std::move(parent_image);

  if (magic == 0x33465050) // PPF3
    return ReadV3Patch(fp.get());
  else if (magic == 0x32465050) // PPF2
    return ReadV2Patch(fp.get());
  else if (magic == 0x31465050) // PPF1
    return ReadV1Patch(fp.get());

  ERROR_LOG("Unknown PPF magic {:08X}", magic);
  return false;
}

u32 CDImagePPF::ReadFileIDDiz(std::FILE* fp, u32 version)
{
  const int lenidx = (version == 2) ? 4 : 2;

  u32 magic;
  if (std::fseek(fp, -(lenidx + 4), SEEK_END) != 0 || std::fread(&magic, sizeof(magic), 1, fp) != 1) [[unlikely]]
  {
    WARNING_LOG("Failed to read diz magic");
    return 0;
  }

  if (magic != 0x5A49442E) // .DIZ
    return 0;

  u32 dlen = 0;
  if (std::fseek(fp, -lenidx, SEEK_END) != 0 || std::fread(&dlen, lenidx, 1, fp) != 1) [[unlikely]]
  {
    WARNING_LOG("Failed to read diz length");
    return 0;
  }

  if (dlen > static_cast<u32>(std::ftell(fp))) [[unlikely]]
  {
    WARNING_LOG("diz length out of range");
    return 0;
  }

  std::string fdiz;
  fdiz.resize(dlen);
  if (std::fseek(fp, -(lenidx + 16 + static_cast<int>(dlen)), SEEK_END) != 0 ||
      std::fread(fdiz.data(), 1, dlen, fp) != dlen) [[unlikely]]
  {
    WARNING_LOG("Failed to read fdiz");
    return 0;
  }

  INFO_LOG("File_Id.diz: {}", fdiz);
  return dlen;
}

bool CDImagePPF::ReadV1Patch(std::FILE* fp)
{
  char desc[DESC_SIZE + 1] = {};
  if (std::fseek(fp, 6, SEEK_SET) != 0 || std::fread(desc, sizeof(char), DESC_SIZE, fp) != DESC_SIZE) [[unlikely]]
  {
    ERROR_LOG("Failed to read description");
    return false;
  }

  u32 filelen;
  if (std::fseek(fp, 0, SEEK_END) != 0 || (filelen = static_cast<u32>(std::ftell(fp))) == 0 || filelen < 56)
    [[unlikely]]
  {
    ERROR_LOG("Invalid ppf file");
    return false;
  }

  u32 count = filelen - 56;
  if (count <= 0)
    return false;

  if (std::fseek(fp, 56, SEEK_SET) != 0)
    return false;

  std::vector<u8> temp;
  while (count > 0)
  {
    u32 offset;
    u8 chunk_size;
    if (std::fread(&offset, sizeof(offset), 1, fp) != 1 || std::fread(&chunk_size, sizeof(chunk_size), 1, fp) != 1)
      [[unlikely]]
    {
      ERROR_LOG("Incomplete ppf");
      return false;
    }

    temp.resize(chunk_size);
    if (std::fread(temp.data(), 1, chunk_size, fp) != chunk_size) [[unlikely]]
    {
      ERROR_LOG("Failed to read patch data");
      return false;
    }

    if (!AddPatch(offset, temp.data(), chunk_size)) [[unlikely]]
      return false;

    count -= sizeof(offset) + sizeof(chunk_size) + chunk_size;
  }

  INFO_LOG("Loaded {} replacement sectors from version 1 PPF", m_replacement_map.size());
  return true;
}

bool CDImagePPF::ReadV2Patch(std::FILE* fp)
{
  char desc[DESC_SIZE + 1] = {};
  if (std::fseek(fp, 6, SEEK_SET) != 0 || std::fread(desc, sizeof(char), DESC_SIZE, fp) != DESC_SIZE) [[unlikely]]
  {
    ERROR_LOG("Failed to read description");
    return false;
  }

  INFO_LOG("Patch description: {}", desc);

  const u32 idlen = ReadFileIDDiz(fp, 2);

  u32 origlen;
  if (std::fseek(fp, 56, SEEK_SET) != 0 || std::fread(&origlen, sizeof(origlen), 1, fp) != 1) [[unlikely]]
  {
    ERROR_LOG("Failed to read size");
    return false;
  }

  std::vector<u8> temp;
  temp.resize(BLOCKCHECK_SIZE);
  if (std::fread(temp.data(), 1, BLOCKCHECK_SIZE, fp) != BLOCKCHECK_SIZE) [[unlikely]]
  {
    ERROR_LOG("Failed to read blockcheck data");
    return false;
  }

  // do blockcheck
  {
    u32 blockcheck_src_sector = 16 + m_replacement_offset;
    u32 blockcheck_src_offset = 32;

    std::vector<u8> src_sector(RAW_SECTOR_SIZE);
    if (m_parent_image->Seek(blockcheck_src_sector) && m_parent_image->ReadRawSector(src_sector.data(), nullptr))
    {
      if (std::memcmp(&src_sector[blockcheck_src_offset], temp.data(), BLOCKCHECK_SIZE) != 0)
        WARNING_LOG("Blockcheck failed. The patch may not apply correctly.");
    }
    else
    {
      WARNING_LOG("Failed to read blockcheck sector {}", blockcheck_src_sector);
    }
  }

  u32 filelen;
  if (std::fseek(fp, 0, SEEK_END) != 0 || (filelen = static_cast<u32>(std::ftell(fp))) == 0 || filelen < 1084)
    [[unlikely]]
  {
    ERROR_LOG("Invalid ppf file");
    return false;
  }

  u32 count = filelen - 1084;
  if (idlen > 0)
    count -= (idlen + 38);

  if (count <= 0)
    return false;

  if (std::fseek(fp, 1084, SEEK_SET) != 0)
    return false;

  while (count > 0)
  {
    u32 offset;
    u8 chunk_size;
    if (std::fread(&offset, sizeof(offset), 1, fp) != 1 || std::fread(&chunk_size, sizeof(chunk_size), 1, fp) != 1)
      [[unlikely]]
    {
      ERROR_LOG("Incomplete ppf");
      return false;
    }

    temp.resize(chunk_size);
    if (std::fread(temp.data(), 1, chunk_size, fp) != chunk_size) [[unlikely]]
    {
      ERROR_LOG("Failed to read patch data");
      return false;
    }

    if (!AddPatch(offset, temp.data(), chunk_size))
      return false;

    count -= sizeof(offset) + sizeof(chunk_size) + chunk_size;
  }

  INFO_LOG("Loaded {} replacement sectors from version 2 PPF", m_replacement_map.size());
  return true;
}

bool CDImagePPF::ReadV3Patch(std::FILE* fp)
{
  char desc[DESC_SIZE + 1] = {};
  if (std::fseek(fp, 6, SEEK_SET) != 0 || std::fread(desc, sizeof(char), DESC_SIZE, fp) != DESC_SIZE)
  {
    ERROR_LOG("Failed to read description");
    return false;
  }

  INFO_LOG("Patch description: {}", desc);

  u32 idlen = ReadFileIDDiz(fp, 3);

  u8 image_type;
  u8 block_check;
  u8 undo;
  if (std::fseek(fp, 56, SEEK_SET) != 0 || std::fread(&image_type, sizeof(image_type), 1, fp) != 1 ||
      std::fread(&block_check, sizeof(block_check), 1, fp) != 1 || std::fread(&undo, sizeof(undo), 1, fp) != 1)
  {
    ERROR_LOG("Failed to read headers");
    return false;
  }

  // TODO: Blockcheck

  std::fseek(fp, 0, SEEK_END);
  u32 count = static_cast<u32>(std::ftell(fp));

  u32 seekpos = (block_check) ? 1084 : 60;
  if (seekpos >= count)
  {
    ERROR_LOG("File is too short");
    return false;
  }

  count -= seekpos;
  if (idlen > 0)
  {
    const u32 extralen = idlen + 18 + 16 + 2;
    if (count < extralen)
    {
      ERROR_LOG("File is too short (diz)");
      return false;
    }

    count -= extralen;
  }

  if (std::fseek(fp, seekpos, SEEK_SET) != 0)
    return false;

  std::vector<u8> temp;

  while (count > 0)
  {
    u64 offset;
    u8 chunk_size;
    if (std::fread(&offset, sizeof(offset), 1, fp) != 1 || std::fread(&chunk_size, sizeof(chunk_size), 1, fp) != 1)
    {
      ERROR_LOG("Incomplete ppf");
      return false;
    }

    temp.resize(chunk_size);
    if (std::fread(temp.data(), 1, chunk_size, fp) != chunk_size)
    {
      ERROR_LOG("Failed to read patch data");
      return false;
    }

    if (!AddPatch(offset, temp.data(), chunk_size))
      return false;

    count -= sizeof(offset) + sizeof(chunk_size) + chunk_size;
  }

  INFO_LOG("Loaded {} replacement sectors from version 3 PPF", m_replacement_map.size());
  return true;
}

bool CDImagePPF::AddPatch(u64 offset, const u8* patch, u32 patch_size)
{
  DEBUG_LOG("Starting applying patch of {} bytes at at offset {}", patch_size, offset);

  while (patch_size > 0)
  {
    const u32 sector_index = Truncate32(offset / RAW_SECTOR_SIZE) + m_replacement_offset;
    const u32 sector_offset = Truncate32(offset % RAW_SECTOR_SIZE);
    if (sector_index >= m_parent_image->GetLBACount())
    {
      ERROR_LOG("Sector {} in patch is out of range", sector_index);
      return false;
    }

    const u32 bytes_to_patch = std::min(patch_size, RAW_SECTOR_SIZE - sector_offset);

    auto iter = m_replacement_map.find(sector_index);
    if (iter == m_replacement_map.end())
    {
      const u32 replacement_buffer_start = static_cast<u32>(m_replacement_data.size());
      m_replacement_data.resize(m_replacement_data.size() + RAW_SECTOR_SIZE);
      if (!m_parent_image->Seek(sector_index) ||
          !m_parent_image->ReadRawSector(&m_replacement_data[replacement_buffer_start], nullptr))
      {
        ERROR_LOG("Failed to read sector {} from parent image", sector_index);
        return false;
      }

      iter = m_replacement_map.emplace(sector_index, replacement_buffer_start).first;
    }

    // patch it!
    DEBUG_LOG("  Patching {} bytes at sector {} offset {}", bytes_to_patch, sector_index, sector_offset);
    std::memcpy(&m_replacement_data[iter->second + sector_offset], patch, bytes_to_patch);
    offset += bytes_to_patch;
    patch += bytes_to_patch;
    patch_size -= bytes_to_patch;
  }

  return true;
}

bool CDImagePPF::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  return m_parent_image->ReadSubChannelQ(subq, index, lba_in_index);
}

bool CDImagePPF::HasNonStandardSubchannel() const
{
  return m_parent_image->HasNonStandardSubchannel();
}

std::string CDImagePPF::GetMetadata(std::string_view type) const
{
  return m_parent_image->GetMetadata(type);
}

std::string CDImagePPF::GetSubImageMetadata(u32 index, std::string_view type) const
{
  // We only support a single sub-image for patched games.
  std::string ret;
  if (index == 0)
    ret = m_parent_image->GetSubImageMetadata(index, type);

  return ret;
}

CDImage::PrecacheResult CDImagePPF::Precache(ProgressCallback* progress /*= ProgressCallback::NullProgressCallback*/)
{
  return m_parent_image->Precache(progress);
}

bool CDImagePPF::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  DebugAssert(index.file_index == 0);

  const u32 sector_number = index.start_lba_on_disc + lba_in_index;
  const auto it = m_replacement_map.find(sector_number);
  if (it == m_replacement_map.end())
    return m_parent_image->ReadSectorFromIndex(buffer, index, lba_in_index);

  std::memcpy(buffer, &m_replacement_data[it->second], RAW_SECTOR_SIZE);
  return true;
}

s64 CDImagePPF::GetSizeOnDisk() const
{
  return m_patch_size + m_parent_image->GetSizeOnDisk();
}

std::unique_ptr<CDImage>
CDImage::OverlayPPFPatch(const char* filename, std::unique_ptr<CDImage> parent_image,
                         ProgressCallback* progress /* = ProgressCallback::NullProgressCallback */)
{
  std::unique_ptr<CDImagePPF> ppf_image = std::make_unique<CDImagePPF>();
  if (!ppf_image->Open(filename, std::move(parent_image)))
    return {};

  return ppf_image;
}
