// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"
#include "core/system.h"

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

bool CPU::SafeWriteMemoryByte(VirtualMemoryAddress addr, u8 value)
{
  return false;
}

bool CPU::SafeWriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value)
{
  return false;
}

bool CPU::SafeWriteMemoryWord(VirtualMemoryAddress addr, u32 value)
{
  return false;
}

void CPU::InvalidateICacheAt(VirtualMemoryAddress address)
{
}

u32 System::GetFrameNumber()
{
  return 0;
}

Controller* System::GetController(u32 slot)
{
  return nullptr;
}
