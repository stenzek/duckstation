#pragma once
#include "common/types.h"

// Physical memory addresses are 32-bits wide
using PhysicalMemoryAddress = u32;
using VirtualMemoryAddress = u32;

enum class MemoryAccessType : u32
{
  Read,
  Write
};
enum class MemoryAccessSize : u32
{
  Byte,
  HalfWord,
  Word
};

using TickCount = s32;

static constexpr TickCount MASTER_CLOCK = 44100 * 0x300; // 33868800Hz or 33.8688MHz, also used as CPU clock
static constexpr TickCount MAX_CPU_SLICE_SIZE = MASTER_CLOCK / 10;

