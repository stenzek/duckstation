// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

class StateWrapper;

namespace DMA {

enum : u32
{
  NUM_CHANNELS = 7
};

enum class Channel : u32
{
  MDECin = 0,
  MDECout = 1,
  GPU = 2,
  CDROM = 3,
  SPU = 4,
  PIO = 5,
  OTC = 6
};

void Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);

u32 ReadRegister(u32 offset);
void WriteRegister(u32 offset, u32 value);

void SetRequest(Channel channel, bool request);

void DrawDebugStateWindow();

} // namespace DMA
