// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "types.h"

#include "common/bitfield.h"

#include <array>
#include <bitset>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class StateWrapper;

namespace Bus {

enum : u32
{
  RAM_BASE = 0x00000000,
  RAM_2MB_SIZE = 0x200000,
  RAM_2MB_MASK = RAM_2MB_SIZE - 1,
  RAM_8MB_SIZE = 0x800000,
  RAM_8MB_MASK = RAM_8MB_SIZE - 1,
  RAM_MIRROR_END = 0x800000,
  RAM_MIRROR_SIZE = 0x800000,
  EXP1_BASE = 0x1F000000,
  EXP1_SIZE = 0x800000,
  EXP1_MASK = EXP1_SIZE - 1,
  HW_BASE = 0x1F801000,
  HW_SIZE = 0x1000,
  MEMCTRL_BASE = 0x1F801000,
  MEMCTRL_SIZE = 0x40,
  MEMCTRL_MASK = MEMCTRL_SIZE - 1,
  PAD_BASE = 0x1F801040,
  PAD_SIZE = 0x10,
  PAD_MASK = PAD_SIZE - 1,
  SIO_BASE = 0x1F801050,
  SIO_SIZE = 0x10,
  SIO_MASK = SIO_SIZE - 1,
  MEMCTRL2_BASE = 0x1F801060,
  MEMCTRL2_SIZE = 0x10,
  MEMCTRL2_MASK = MEMCTRL2_SIZE - 1,
  INTC_BASE = 0x1F801070,
  INTC_SIZE = 0x10,
  INTERRUPT_CONTROLLER_MASK = INTC_SIZE - 1,
  DMA_BASE = 0x1F801080,
  DMA_SIZE = 0x80,
  DMA_MASK = DMA_SIZE - 1,
  TIMERS_BASE = 0x1F801100,
  TIMERS_SIZE = 0x40,
  TIMERS_MASK = TIMERS_SIZE - 1,
  CDROM_BASE = 0x1F801800,
  CDROM_SIZE = 0x10,
  CDROM_MASK = CDROM_SIZE - 1,
  GPU_BASE = 0x1F801810,
  GPU_SIZE = 0x10,
  GPU_MASK = GPU_SIZE - 1,
  MDEC_BASE = 0x1F801820,
  MDEC_SIZE = 0x10,
  MDEC_MASK = MDEC_SIZE - 1,
  SPU_BASE = 0x1F801C00,
  SPU_SIZE = 0x400,
  SPU_MASK = 0x3FF,
  EXP2_BASE = 0x1F802000,
  EXP2_SIZE = 0x2000,
  EXP2_MASK = EXP2_SIZE - 1,
  EXP3_BASE = 0x1FA00000,
  EXP3_SIZE = 0x200000,
  EXP3_MASK = EXP3_SIZE - 1,
  BIOS_BASE = 0x1FC00000,
  BIOS_SIZE = 0x80000,
  BIOS_MASK = 0x7FFFF,
};

enum : u32
{
  MEMCTRL_REG_COUNT = 9
};

enum : TickCount
{
  RAM_READ_TICKS = 6
};

enum : u32
{
  RAM_2MB_CODE_PAGE_COUNT = (RAM_2MB_SIZE + (HOST_PAGE_SIZE - 1)) / HOST_PAGE_SIZE,
  RAM_8MB_CODE_PAGE_COUNT = (RAM_8MB_SIZE + (HOST_PAGE_SIZE - 1)) / HOST_PAGE_SIZE,

  MEMORY_LUT_PAGE_SIZE = 4096,
  MEMORY_LUT_PAGE_SHIFT = 12,
  MEMORY_LUT_PAGE_MASK = MEMORY_LUT_PAGE_SIZE - 1,
  MEMORY_LUT_SIZE = 0x100000,                 // 0x100000000 >> 12
  MEMORY_LUT_SLOTS = MEMORY_LUT_SIZE * 3 * 2, // [size][read_write]

  FASTMEM_LUT_PAGE_SIZE = 4096,
  FASTMEM_LUT_PAGE_MASK = FASTMEM_LUT_PAGE_SIZE - 1,
  FASTMEM_LUT_PAGE_SHIFT = 12,
  FASTMEM_LUT_SIZE = 0x100000,              // 0x100000000 >> 12
  FASTMEM_LUT_SLOTS = FASTMEM_LUT_SIZE * 2, // [isc]
};

#ifdef ENABLE_MMAP_FASTMEM
// Fastmem region size is 4GB to cover the entire 32-bit address space.
static constexpr size_t FASTMEM_ARENA_SIZE = UINT64_C(0x100000000);
#endif

bool AllocateMemory();
void ReleaseMemory();

bool Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);

using MemoryReadHandler = u32 (*)(VirtualMemoryAddress address);
using MemoryWriteHandler = void (*)(VirtualMemoryAddress, u32);

void** GetMemoryHandlers(bool isolate_cache, bool swap_caches);

template<typename FP>
ALWAYS_INLINE_RELEASE static FP* OffsetHandlerArray(void** handlers, MemoryAccessSize size, MemoryAccessType type)
{
  return reinterpret_cast<FP*>(handlers +
                               (((static_cast<size_t>(size) * 2) + static_cast<size_t>(type)) * MEMORY_LUT_SIZE));
}

CPUFastmemMode GetFastmemMode();
void* GetFastmemBase(bool isc);
void UpdateFastmemViews(CPUFastmemMode mode);
bool CanUseFastmemForAddress(VirtualMemoryAddress address);

void SetExpansionROM(std::vector<u8> data);

extern std::bitset<RAM_8MB_CODE_PAGE_COUNT> g_ram_code_bits;
extern u8* g_ram;             // 2MB-8MB RAM
extern u8* g_unprotected_ram; // RAM without page protection, use for debugger access.
extern u32 g_ram_size;        // Active size of RAM.
extern u32 g_ram_mask;        // Active address bits for RAM.
extern u8* g_bios;            // 512K BIOS ROM
extern std::array<TickCount, 3> g_exp1_access_time;
extern std::array<TickCount, 3> g_exp2_access_time;
extern std::array<TickCount, 3> g_bios_access_time;
extern std::array<TickCount, 3> g_cdrom_access_time;
extern std::array<TickCount, 3> g_spu_access_time;

/// Returns true if the address specified is writable (RAM).
ALWAYS_INLINE static bool IsRAMAddress(PhysicalMemoryAddress address)
{
  return address < RAM_MIRROR_END;
}

/// Returns the code page index for a RAM address.
ALWAYS_INLINE static u32 GetRAMCodePageIndex(PhysicalMemoryAddress address)
{
  return (address & g_ram_mask) / HOST_PAGE_SIZE;
}

/// Returns true if the specified page contains code.
bool IsRAMCodePage(u32 index);

/// Flags a RAM region as code, so we know when to invalidate blocks.
void SetRAMCodePage(u32 index);

/// Unflags a RAM region as code, the code cache will no longer be notified when writes occur.
void ClearRAMCodePage(u32 index);

/// Clears all code bits for RAM regions.
void ClearRAMCodePageFlags();

/// Returns true if the specified address is in a code page.
bool IsCodePageAddress(PhysicalMemoryAddress address);

/// Returns true if the range specified overlaps with a code page.
bool HasCodePagesInRange(PhysicalMemoryAddress start_address, u32 size);

/// Returns the number of cycles stolen by DMA RAM access.
ALWAYS_INLINE TickCount GetDMARAMTickCount(u32 word_count)
{
  // DMA is using DRAM Hyper Page mode, allowing it to access DRAM rows at 1 clock cycle per word (effectively around
  // 17 clks per 16 words, due to required row address loading, probably plus some further minimal overload due to
  // refresh cycles). This is making DMA much faster than CPU memory accesses (CPU DRAM access takes 1 opcode cycle
  // plus 6 waitstates, ie. 7 cycles in total).
  return static_cast<TickCount>(word_count + ((word_count + 15) / 16));
}

enum class MemoryRegion
{
  RAM,
  RAMMirror1,
  RAMMirror2,
  RAMMirror3,
  EXP1,
  Scratchpad,
  BIOS,
  Count
};

std::optional<MemoryRegion> GetMemoryRegionForAddress(PhysicalMemoryAddress address);
PhysicalMemoryAddress GetMemoryRegionStart(MemoryRegion region);
PhysicalMemoryAddress GetMemoryRegionEnd(MemoryRegion region);
u8* GetMemoryRegionPointer(MemoryRegion region);
std::optional<PhysicalMemoryAddress> SearchMemory(PhysicalMemoryAddress start_address, const u8* pattern,
                                                  const u8* mask, u32 pattern_length);

// TTY Logging.
void AddTTYCharacter(char ch);
void AddTTYString(const std::string_view& str);

} // namespace Bus
