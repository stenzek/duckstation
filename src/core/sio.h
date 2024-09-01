// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

class StateWrapper;

namespace SIO {

void Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);

u32 ReadRegister(u32 offset);
void WriteRegister(u32 offset, u32 value);

} // namespace SIO
