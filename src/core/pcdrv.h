// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "cpu_types.h"

//////////////////////////////////////////////////////////////////////////
// HLE Implementation of PCDrv
//////////////////////////////////////////////////////////////////////////

namespace PCDrv {
void Initialize();
void Reset();
void Shutdown();

bool HandleSyscall(u32 instruction_bits, CPU::Registers& regs);
}
