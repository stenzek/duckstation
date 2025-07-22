// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <memory>
#include <string>
#include <tuple>
#include <utility>

class Error;
class CDImage;
class StateWrapper;

namespace CDROM {

void Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);

bool HasMedia();
const std::string& GetMediaPath();
u32 GetCurrentSubImage();
const CDImage* GetMedia();
DiscRegion GetDiscRegion();
bool IsMediaPS1Disc();
bool IsMediaAudioCD();
bool DoesMediaRegionMatchConsole();

bool InsertMedia(std::unique_ptr<CDImage>& media, DiscRegion region, std::string_view serial, std::string_view title,
                 Error* error);
std::unique_ptr<CDImage> RemoveMedia(bool for_disc_swap);
bool PrecacheMedia();
bool HasNonStandardOrReplacementSubQ();

void CPUClockChanged();

// I/O
u8 ReadRegister(u32 offset);
void WriteRegister(u32 offset, u8 value);
void DMARead(u32* words, u32 word_count);

// Render statistics debug window.
void DrawDebugWindow(float scale);

void SetReadaheadSectors(u32 readahead_sectors);
void DisableReadSpeedup();

/// Reads a frame from the audio FIFO, used by the SPU.
std::tuple<s16, s16> GetAudioFrame();

} // namespace CDROM
