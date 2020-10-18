#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <array>
#include <bitset>
#include <string>
#include <vector>

class StateWrapper;

namespace Bus {

enum : u32
{
  RAM_BASE = 0x00000000,
  RAM_SIZE = 0x200000,
  RAM_MASK = RAM_SIZE - 1,
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
  BIOS_BASE = 0x1FC00000,
  BIOS_SIZE = 0x80000,
  BIOS_MASK = 0x7FFFF,
};

enum : u32
{
  MEMCTRL_REG_COUNT = 9
};

void Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);

void SetExpansionROM(std::vector<u8> data);
void SetBIOS(const std::vector<u8>& image);

extern std::bitset<CPU_CODE_CACHE_PAGE_COUNT> m_ram_code_bits;
extern u8 g_ram[RAM_SIZE];   // 2MB RAM
extern u8 g_bios[BIOS_SIZE]; // 512K BIOS ROM

/// Flags a RAM region as code, so we know when to invalidate blocks.
ALWAYS_INLINE void SetRAMCodePage(u32 index) { m_ram_code_bits[index] = true; }

/// Unflags a RAM region as code, the code cache will no longer be notified when writes occur.
ALWAYS_INLINE void ClearRAMCodePage(u32 index) { m_ram_code_bits[index] = false; }

/// Clears all code bits for RAM regions.
ALWAYS_INLINE void ClearRAMCodePageFlags() { m_ram_code_bits.reset(); }

/// Returns the number of cycles stolen by DMA RAM access.
ALWAYS_INLINE TickCount GetDMARAMTickCount(u32 word_count)
{
  // DMA is using DRAM Hyper Page mode, allowing it to access DRAM rows at 1 clock cycle per word (effectively around
  // 17 clks per 16 words, due to required row address loading, probably plus some further minimal overload due to
  // refresh cycles). This is making DMA much faster than CPU memory accesses (CPU DRAM access takes 1 opcode cycle
  // plus 6 waitstates, ie. 7 cycles in total).
  return static_cast<TickCount>(word_count + ((word_count + 15) / 16));
}

} // namespace Bus
