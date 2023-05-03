// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "cpu_types.h"
#include "types.h"

//////////////////////////////////////////////////////////////////////////
// HLE Implementation of PCDrv
//////////////////////////////////////////////////////////////////////////

namespace PCDrv {
void Initialize();
void Reset();
void Shutdown();

bool HandleSyscall(u32 instruction_bits, CPU::Registers& regs);
}
