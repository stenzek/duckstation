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

  bool HasSubchannelData() const override;

  u32 ReadSectorsFromIndex(std::span<Sector> sectors, const Index& index, LBA lba_in_index,
                           SectorReadMode mode) override;

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
      const u32 memory_sector_size = RAW_SECTOR_SIZE + (m_has_subchannel_data ? SUBCHANNEL_BYTES_PER_FRAME : 0);
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
  static constexpr u32 READ_BATCH_SIZE = 32;
  std::array<Sector, READ_BATCH_SIZE> read_buffer;
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

    index.file_index = 0;
    index.file_offset = memory_offset;
    index.file_sector_size = RAW_SECTOR_SIZE + (m_has_subchannel_data ? SUBCHANNEL_BYTES_PER_FRAME : 0);

    // Memory images store normalized raw sectors, regardless of how the source image stores them.
    if (index.mode != TrackMode::Audio)
      index.mode = (index.mode == TrackMode::Mode1 || index.mode == TrackMode::Mode1Raw) ? TrackMode::Mode1Raw :
                                                                                         TrackMode::Mode2Raw;

    for (u32 lba = 0; lba < index.length;)
    {
      const u32 count = std::min(index.length - lba, READ_BATCH_SIZE);
      const u32 count_read =
        image->ReadSectors(index.start_lba_on_disc + lba, std::span(read_buffer).first(count),
                           m_has_subchannel_data ? SectorReadMode::DataAndSubQ : SectorReadMode::DataOnly);
      if (count_read != count)
      {
        ERROR_LOG("Failed to read LBA {} in index {} (disc LBA {})", lba + count_read, i,
                  index.start_lba_on_disc + lba + count_read);
        return false;
      }

      for (u32 j = 0; j < count; j++)
      {
        u8* const sector_ptr = m_memory + memory_offset;
        std::memcpy(sector_ptr, read_buffer[j].data.data(), RAW_SECTOR_SIZE);
        if (m_has_subchannel_data)
        {
          std::memcpy(sector_ptr + RAW_SECTOR_SIZE, read_buffer[j].subq.data.data(), SUBCHANNEL_BYTES_PER_FRAME);
        }
        memory_offset += index.file_sector_size;
      }

      lba += count;
      sectors_read += count;
      progress->SetProgressValue(sectors_read);
    }
  }

  for (u32 i = 1; i <= image->GetTrackCount(); i++)
    m_tracks.push_back(image->GetTrack(i));

  Assert(memory_offset == m_memory_size);
  m_path = image->GetPath();
  m_lba_count = image->GetLBACount();

  return Seek(1, Position{0, 0, 0});
}

bool CDImageMemory::HasSubchannelData() const
{
  return m_has_subchannel_data;
}

u32 CDImageMemory::ReadSectorsFromIndex(std::span<Sector> sectors, const Index& index, LBA lba_in_index,
                                        SectorReadMode mode)
{
  DebugAssert(index.file_index == 0);

  u32 sectors_read = 0;
  for (Sector& sector : sectors)
  {
    const u64 memory_offset =
      index.file_offset + ((lba_in_index + sectors_read) * static_cast<u64>(index.file_sector_size));
    const size_t sector_size = static_cast<size_t>(index.file_sector_size);
    if ((memory_offset + sector_size) > m_memory_size)
      break;

    if (mode != SectorReadMode::SubQOnly)
    {
      // Don't copy subq into the receiving data buffer.
      const u32 data_size = index.file_sector_size - (m_has_subchannel_data ? SUBCHANNEL_BYTES_PER_FRAME : 0);
      std::memcpy(sector.data.data(), &m_memory[memory_offset], data_size);
    }

    // SubQ was generated by the caller for images which do not have replacement subchannel data.
    if (mode != SectorReadMode::DataOnly && m_has_subchannel_data)
    {
      std::memcpy(sector.subq.data.data(),
                  &m_memory[memory_offset + index.file_sector_size - SUBCHANNEL_BYTES_PER_FRAME],
                  SUBCHANNEL_BYTES_PER_FRAME);
    }

    sectors_read++;
  }

  return sectors_read;
}

bool CDImageMemory::IsPrecached() const
{
  return true;
}

std::unique_ptr<CDImage> CDImage::CreateMemoryImage(CDImage* image, ProgressCallback* progress, Error* error)
{
  std::unique_ptr<CDImageMemory> memory_image = std::make_unique<CDImageMemory>();
  if (!memory_image->CopyImage(image, progress, error))
    return {};

  return memory_image;
}
