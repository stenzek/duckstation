// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "gte_types.h"

class StateWrapper;

struct DisplayAspectRatio;

namespace GTE {

void Reset();
bool DoState(StateWrapper& sw);
void SetAspectRatio(const DisplayAspectRatio& aspect);

// control registers are offset by +32
u32 ReadRegister(u32 index);
void WriteRegister(u32 index, u32 value);

// use with care, direct register access
u32* GetRegisterPtr(u32 index);

void ExecuteInstruction(u32 inst_bits);

using InstructionImpl = void (*)(Instruction);
InstructionImpl GetInstructionImpl(u32 inst_bits, TickCount* ticks);

void DrawFreecamWindow(float scale);

bool IsFreecamEnabled();
void SetFreecamEnabled(bool enabled);
void SetFreecamMoveAxis(u32 axis, float x);
void SetFreecamRotateAxis(u32 axis, float x);
void UpdateFreecam(u64 current_time);
void ResetFreecam();

} // namespace GTE
