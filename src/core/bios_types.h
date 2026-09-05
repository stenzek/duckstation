// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <array>
#include <cstddef>

namespace BIOS {

inline constexpr VirtualMemoryAddress PCB_TABLE_ADDRESS = 0x00000108;
inline constexpr VirtualMemoryAddress TCB_TABLE_ADDRESS = 0x00000110;
inline constexpr u32 KERNEL_CONTROL_BLOCK_MEMORY_SIZE = 0x2000;
inline constexpr u32 THREAD_HANDLE_BASE = 0xFF000000;

enum class ThreadStatus : u32
{
  Free = 0x1000,
  Used = 0x4000,
};

struct ControlBlockTableEntry
{
  VirtualMemoryAddress address;
  u32 size;
};
static_assert(sizeof(ControlBlockTableEntry) == 0x08);

struct ProcessControlBlock
{
  VirtualMemoryAddress current_thread;
};
static_assert(sizeof(ProcessControlBlock) == 0x04);

struct ThreadControlBlock
{
  ThreadStatus status;
  u32 unknown_04;
  std::array<u32, 32> regs;
  u32 epc;
  u32 hi;
  u32 lo;
  u32 sr;
  u32 cause;
  std::array<u32, 9> unused_9c;
};

static_assert(offsetof(ThreadControlBlock, status) == 0x00);
static_assert(offsetof(ThreadControlBlock, regs) == 0x08);
static_assert(offsetof(ThreadControlBlock, epc) == 0x88);
static_assert(offsetof(ThreadControlBlock, hi) == 0x8C);
static_assert(offsetof(ThreadControlBlock, lo) == 0x90);
static_assert(offsetof(ThreadControlBlock, sr) == 0x94);
static_assert(offsetof(ThreadControlBlock, cause) == 0x98);
static_assert(offsetof(ThreadControlBlock, unused_9c) == 0x9C);
static_assert(sizeof(ThreadControlBlock) == 0xC0);

inline constexpr u32 MAX_THREAD_CONTROL_BLOCKS = KERNEL_CONTROL_BLOCK_MEMORY_SIZE / sizeof(ThreadControlBlock);

} // namespace BIOS
