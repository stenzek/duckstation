#pragma once
#include "common/bitfield.h"
#include "util/memory_arena.h"
#include "types.h"
#include <array>
#include <bitset>
#include <optional>
#include <string>
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
  EXP1_BASE = 0x1F000000,
  EXP1_SIZE = 0x800000,
  EXP1_MASK = EXP1_SIZE - 1,
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
  INTERRUPT_CONTROLLER_BASE = 0x1F801070,
  INTERRUPT_CONTROLLER_SIZE = 0x10,
  INTERRUPT_CONTROLLER_MASK = INTERRUPT_CONTROLLER_SIZE - 1,
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
  EXP3_SIZE = 0x1,
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

enum : size_t
{
  // Our memory arena contains storage for RAM.
  MEMORY_ARENA_SIZE = RAM_8MB_SIZE,

  // Offsets within the memory arena.
  MEMORY_ARENA_RAM_OFFSET = 0,

#ifdef WITH_MMAP_FASTMEM
  // Fastmem region size is 4GB to cover the entire 32-bit address space.
  FASTMEM_REGION_SIZE = UINT64_C(0x100000000),
#endif
};

enum : u32
{
  RAM_2MB_CODE_PAGE_COUNT = (RAM_2MB_SIZE + (HOST_PAGE_SIZE + 1)) / HOST_PAGE_SIZE,
  RAM_8MB_CODE_PAGE_COUNT = (RAM_8MB_SIZE + (HOST_PAGE_SIZE + 1)) / HOST_PAGE_SIZE,

  FASTMEM_LUT_NUM_PAGES = 0x100000, // 0x100000000 >> 12
  FASTMEM_LUT_NUM_SLOTS = FASTMEM_LUT_NUM_PAGES * 2,
};

bool Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);

CPUFastmemMode GetFastmemMode();
u8* GetFastmemBase();
void UpdateFastmemViews(CPUFastmemMode mode);
bool CanUseFastmemForAddress(VirtualMemoryAddress address);

void SetExpansionROM(std::vector<u8> data);
void SetBIOS(const std::vector<u8>& image);

extern std::bitset<RAM_8MB_CODE_PAGE_COUNT> m_ram_code_bits;
extern u8* g_ram;            // 2MB-8MB RAM
extern u32 g_ram_size;       // Active size of RAM.
extern u32 g_ram_mask;       // Active address bits for RAM.
extern u8 g_bios[BIOS_SIZE]; // 512K BIOS ROM

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

} // namespace Bus
