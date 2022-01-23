#include "assert.h"
#include "cd_image.h"
#include "cd_subchannel_replacement.h"
#include "file_system.h"
#include "log.h"
#include <algorithm>
#include <cerrno>
Log_SetChannel(CDImageMemory);

class CDImageMemory : public CDImage
{
public:
  CDImageMemory(OpenFlags open_flags);
  ~CDImageMemory() override;

  bool CopyImage(CDImage* image, ProgressCallback* progress);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  u8* m_memory = nullptr;
  u32 m_memory_sectors = 0;
  CDSubChannelReplacement m_sbi;
};

CDImageMemory::CDImageMemory(OpenFlags open_flags) : CDImage(open_flags) {}

CDImageMemory::~CDImageMemory()
{
  if (m_memory)
    std::free(m_memory);
}

bool CDImageMemory::CopyImage(CDImage* image, ProgressCallback* progress)
{
  // figure out the total number of sectors (not including blank pregaps)
  m_memory_sectors = 0;
  for (u32 i = 0; i < image->GetIndexCount(); i++)
  {
    const Index& index = image->GetIndex(i);
    if (index.file_sector_size > 0)
      m_memory_sectors += image->GetIndex(i).length;
  }

  if ((static_cast<u64>(RAW_SECTOR_SIZE) * static_cast<u64>(m_memory_sectors)) >=
      static_cast<u64>(std::numeric_limits<size_t>::max()))
  {
    progress->DisplayFormattedModalError("Insufficient address space");
    return false;
  }

  progress->SetFormattedStatusText("Allocating memory for %u sectors...", m_memory_sectors);

  m_memory =
    static_cast<u8*>(std::malloc(static_cast<size_t>(RAW_SECTOR_SIZE) * static_cast<size_t>(m_memory_sectors)));
  if (!m_memory)
  {
    progress->DisplayFormattedModalError("Failed to allocate memory for %u sectors", m_memory_sectors);
    return false;
  }

  progress->SetStatusText("Preloading CD image to RAM...");
  progress->SetProgressRange(m_memory_sectors);
  progress->SetProgressValue(0);

  u8* memory_ptr = m_memory;
  u32 sectors_read = 0;
  for (u32 i = 0; i < image->GetIndexCount(); i++)
  {
    const Index& index = image->GetIndex(i);
    if (index.file_sector_size == 0)
      continue;

    for (u32 lba = 0; lba < index.length; lba++)
    {
      if (!image->ReadSectorFromIndex(memory_ptr, index, lba))
      {
        Log_ErrorPrintf("Failed to read LBA %u in index %u", lba, i);
        return false;
      }

      progress->SetProgressValue(sectors_read);
      memory_ptr += RAW_SECTOR_SIZE;
      sectors_read++;
    }
  }

  for (u32 i = 1; i <= image->GetTrackCount(); i++)
    m_tracks.push_back(image->GetTrack(i));

  u32 current_offset = 0;
  for (u32 i = 0; i < image->GetIndexCount(); i++)
  {
    Index new_index = image->GetIndex(i);
    new_index.file_index = 0;
    if (new_index.file_sector_size > 0)
    {
      new_index.file_offset = current_offset;
      current_offset += new_index.length;
    }
    m_indices.push_back(new_index);
  }

  Assert(current_offset == m_memory_sectors);
  m_filename = image->GetFileName();
  m_lba_count = image->GetLBACount();

  m_sbi.LoadSBI(FileSystem::ReplaceExtension(m_filename, "sbi").c_str());

  return Seek(1, Position{0, 0, 0});
}

bool CDImageMemory::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  if (m_sbi.GetReplacementSubChannelQ(index.start_lba_on_disc + lba_in_index, subq))
    return true;

  return CDImage::ReadSubChannelQ(subq, index, lba_in_index);
}

bool CDImageMemory::HasNonStandardSubchannel() const
{
  return (m_sbi.GetReplacementSectorCount() > 0);
}

bool CDImageMemory::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  DebugAssert(index.file_index == 0);

  const u64 sector_number = index.file_offset + lba_in_index;
  if (sector_number >= m_memory_sectors)
    return false;

  const size_t file_offset = static_cast<size_t>(sector_number) * static_cast<size_t>(RAW_SECTOR_SIZE);
  std::memcpy(buffer, &m_memory[file_offset], RAW_SECTOR_SIZE);
  return true;
}

std::unique_ptr<CDImage>
CDImage::CreateMemoryImage(CDImage* image, ProgressCallback* progress /* = ProgressCallback::NullProgressCallback */)
{
  std::unique_ptr<CDImageMemory> memory_image = std::make_unique<CDImageMemory>(image->GetOpenFlags());
  if (!memory_image->CopyImage(image, progress))
    return {};

  return memory_image;
}
