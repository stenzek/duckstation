// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

class StateWrapper;

namespace MDEC {

void Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);

// I/O
u32 ReadRegister(u32 offset);
void WriteRegister(u32 offset, u32 value);

void DMARead(u32* words, u32 word_count);
void DMAWrite(const u32* words, u32 word_count);

void DrawDebugStateWindow();

} // namespace MDEC
