// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "types.h"
#include <memory>

class StateWrapper;

class Controller;
class MemoryCard;
class Multitap;

namespace Pad {

void Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw, bool is_memory_state);

Controller* GetController(u32 slot);
void SetController(u32 slot, std::unique_ptr<Controller> dev);

MemoryCard* GetMemoryCard(u32 slot);
void SetMemoryCard(u32 slot, std::unique_ptr<MemoryCard> dev);
std::unique_ptr<MemoryCard> RemoveMemoryCard(u32 slot);

Multitap* GetMultitap(u32 slot);

u32 ReadRegister(u32 offset);
void WriteRegister(u32 offset, u32 value);

bool IsTransmitting();

} // namespace Pad
