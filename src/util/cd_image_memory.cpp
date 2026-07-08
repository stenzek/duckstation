// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cd_image.h"
#include "translation.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/progress_callback.h"

#include <algorithm>
#include <cerrno>

LOG_CHANNEL(CDImage);

namespace {

class CDImageMemory : public CDImage
{
public:
  CDImageMemory();
  ~CDImageMemory() override;

  bool CopyImage(CDImage* image, ProgressCallback* progress, Error* error);

  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;
  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;

  bool HasSubchannelData() const override { return m_has_subchannel_data; }
  bool IsPrecached() const override;

private:
  u8* m_memory = nullptr;
  size_t m_memory_size = 0;
  bool m_has_subchannel_data = false;
};

} // namespace

CDImageMemory::CDImageMemory() = default;

CDImageMemory::~CDImageMemory()
{
  if (m_memory)
    std::free(m_memory);
}

bool CDImageMemory::CopyImage(CDImage* image, ProgressCallback* progress, Error* error)
{
  // figure out the total number of sectors (not including blank pregaps)
  m_has_subchannel_data = image->HasSubchannelData();

  u64 total_size = 0;
  for (u32 i = 0; i < image->GetIndexCount(); i++)
  {
    const Index& index = image->GetIndex(i);
    if (index.file_sector_size > 0)
    {
      const u32 memory_sector_size =
        GetBytesPerSector(index.mode) + (m_has_subchannel_data ? SUBCHANNEL_BYTES_PER_FRAME : 0);
      total_size += static_cast<u64>(index.length) * static_cast<u64>(memory_sector_size);
    }
  }

  if (total_size == 0 || total_size >= static_cast<u64>(std::numeric_limits<size_t>::max()))
  {
    Error::SetStringView(error, "Insufficient address space");
    return false;
  }

  progress->SetTitle(TRANSLATE_SV("CDImage", "Preload Image To RAM"));
  progress->FormatStatusText(TRANSLATE_FS("CDImage", "Allocating {} MB memory for precaching..."),
                             (total_size + 1048575) / 1048576);

  m_memory_size = static_cast<size_t>(total_size);
  m_memory = static_cast<u8*>(std::malloc(m_memory_size));
  if (!m_memory)
  {
    Error::SetStringFmt(error, "Failed to allocate {} MB of memory", (total_size + 1048575) / 1048576);
    return false;
  }

  progress->SetProgressRange(image->GetLBACount());
  progress->SetProgressValue(0);

  u32 sectors_read = 0;
  size_t memory_offset = 0;
  m_indices.reserve(image->GetIndexCount());
  for (u32 i = 0; i < image->GetIndexCount(); i++)
  {
    Index& index = m_indices.emplace_back(image->GetIndex(i));
    if (index.file_sector_size == 0)
    {
      progress->SetProgressValue(sectors_read += index.length);
      continue;
    }

    progress->FormatStatusText(TRANSLATE_FS("CDImage", "Loading Track {0} ({1})..."), index.track_number,
                               GetTrackModeDisplayName(index.mode));

    if (!image->Seek(index.start_lba_on_disc))
    {
      ERROR_LOG("Failed to seek to LBA {} in index {}", index.start_lba_on_disc, i);
      return false;
    }

    index.file_index = 0;
    index.file_offset = memory_offset;
    index.file_sector_size = GetBytesPerSector(index.mode) + (m_has_subchannel_data ? SUBCHANNEL_BYTES_PER_FRAME : 0);

    for (u32 lba = 0; lba < index.length; lba++)
    {
      u8* const sector_ptr = m_memory + memory_offset;
      SubChannelQ* const subq_ptr =
        m_has_subchannel_data ?
          reinterpret_cast<SubChannelQ*>(sector_ptr + index.file_sector_size - SUBCHANNEL_BYTES_PER_FRAME) :
          nullptr;
      if (!image->ReadRawSector(sector_ptr, subq_ptr))
      {
        ERROR_LOG("Failed to read LBA {} in index {} (disc LBA {})", lba, i, index.start_lba_on_disc + lba);
        return false;
      }

      memory_offset += index.file_sector_size;
      progress->SetProgressValue(sectors_read++);
    }
  }

  for (u32 i = 1; i <= image->GetTrackCount(); i++)
    m_tracks.push_back(image->GetTrack(i));

  Assert(memory_offset == m_memory_size);
  m_filename = image->GetPath();
  m_lba_count = image->GetLBACount();

  return Seek(1, Position{0, 0, 0});
}

bool CDImageMemory::IsPrecached() const
{
  return true;
}

bool CDImageMemory::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  DebugAssert(index.file_index == 0);

  const u64 memory_offset = (index.file_offset + (lba_in_index * static_cast<u64>(index.file_sector_size)));
  const size_t sector_size = static_cast<size_t>(index.file_sector_size);
  if ((memory_offset + sector_size) > m_memory_size)
    return false;

  // don't copy subq into the receiving buffer
  std::memcpy(buffer, &m_memory[memory_offset],
              index.file_sector_size - (m_has_subchannel_data ? SUBCHANNEL_BYTES_PER_FRAME : 0));
  return true;
}

bool CDImageMemory::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  // generate subq for non-file indices
  if (!m_has_subchannel_data || index.file_sector_size == 0)
  {
    GenerateSubChannelQ(subq, index, lba_in_index);
    return true;
  }

  const u64 memory_offset = (index.file_offset + (lba_in_index * static_cast<u64>(index.file_sector_size)));
  const size_t sector_size = static_cast<size_t>(index.file_sector_size);
  if ((memory_offset + sector_size) > m_memory_size)
    return false;

  std::memcpy(subq->data.data(), &m_memory[memory_offset + index.file_sector_size - SUBCHANNEL_BYTES_PER_FRAME],
              SUBCHANNEL_BYTES_PER_FRAME);
  return true;
}

std::unique_ptr<CDImage> CDImage::CreateMemoryImage(CDImage* image, ProgressCallback* progress, Error* error)
{
  std::unique_ptr<CDImageMemory> memory_image = std::make_unique<CDImageMemory>();
  if (!memory_image->CopyImage(image, progress, error))
    return {};

  return memory_image;
}
