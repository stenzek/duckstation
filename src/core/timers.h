// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

class StateWrapper;

namespace Timers {

void Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);

void SetGate(u32 timer, bool state);

void DrawDebugStateWindow();

void CPUClocksChanged();

// dot clock/hblank/sysclk div 8
bool IsUsingExternalClock(u32 timer);
bool IsSyncEnabled(u32 timer);

// queries for GPU
bool IsExternalIRQEnabled(u32 timer);

TickCount GetTicksUntilIRQ(u32 timer);

void AddTicks(u32 timer, TickCount ticks);

u32 ReadRegister(u32 offset);
void WriteRegister(u32 offset, u32 value);

} // namespace Timers
