// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include "util/cd_image.h"

#include <array>
#include <memory>

class Error;
class ProgressCallback;

namespace CDROMAsyncReader {

using SectorBuffer = std::array<u8, CDImage::RAW_SECTOR_SIZE>;

struct BufferSlot
{
  CDImage::LBA lba;
  SectorBuffer data;
  CDImage::SubChannelQ subq;
  bool result;
};

CDImage::LBA GetLastReadSector();
const SectorBuffer& GetSectorBuffer();
const CDImage::SubChannelQ& GetSectorSubQ();
u32 GetBufferedSectorCount();
bool HasBufferedSectors();
u32 GetReadaheadCount();

bool HasMedia();
CDImage* GetMedia();
const std::string& GetMediaPath();

// TODO: FIXME: Make global shutdown
bool IsUsingThread();
void StartThread(u32 readahead_count = 8);
void StopThread();

void SetMedia(std::unique_ptr<CDImage> media);
std::unique_ptr<CDImage> RemoveMedia();

/// Precaches image, either to memory, or using the underlying image precache.
bool Precache(ProgressCallback* callback, Error* error);

void QueueReadSector(CDImage::LBA lba);

bool WaitForReadToComplete();
void WaitForIdle();

/// Bypasses the sector cache and reads directly from the image.
bool ReadSectorUncached(CDImage::LBA lba, CDImage::SubChannelQ* subq, SectorBuffer* data);

} // namespace CDROMAsyncReader
