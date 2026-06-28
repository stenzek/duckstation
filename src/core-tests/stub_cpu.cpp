// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"

CPU::State CPU::g_state;

bool CPU::SafeReadMemoryByte(VirtualMemoryAddress addr, u8* value)
{
  (void)addr;
  *value = 0;
  return false;
}

bool CPU::SafeReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value)
{
  (void)addr;
  *value = 0;
  return false;
}

bool CPU::SafeReadMemoryWord(VirtualMemoryAddress addr, u32* value)
{
  (void)addr;
  *value = 0;
  return false;
}
