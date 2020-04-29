#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <array>
#include <bitset>
#include <string>
#include <vector>

class StateWrapper;

namespace CPU {
class Core;
class CodeCache;
} // namespace CPU

class DMA;
class InterruptController;
class GPU;
class CDROM;
class Pad;
class Timers;
class SPU;
class MDEC;
class SIO;
class System;

class Bus
{
  friend DMA;

public:
  Bus();
  ~Bus();

  void Initialize(CPU::Core* cpu, CPU::CodeCache* cpu_code_cache, DMA* dma, InterruptController* interrupt_controller,
                  GPU* gpu, CDROM* cdrom, Pad* pad, Timers* timers, SPU* spu, MDEC* mdec, SIO* sio);
  void Reset();
  bool DoState(StateWrapper& sw);

  bool ReadByte(PhysicalMemoryAddress address, u8* value);
  bool ReadHalfWord(PhysicalMemoryAddress address, u16* value);
  bool ReadWord(PhysicalMemoryAddress address, u32* value);
  bool WriteByte(PhysicalMemoryAddress address, u8 value);
  bool WriteHalfWord(PhysicalMemoryAddress address, u16 value);
  bool WriteWord(PhysicalMemoryAddress address, u32 value);

  template<MemoryAccessType type, MemoryAccessSize size>
  TickCount DispatchAccess(PhysicalMemoryAddress address, u32& value);

  // Optimized variant for burst/multi-word read/writing.
  TickCount ReadWords(PhysicalMemoryAddress address, u32* words, u32 word_count);
  TickCount WriteWords(PhysicalMemoryAddress address, const u32* words, u32 word_count);

  void SetExpansionROM(std::vector<u8> data);
  void SetBIOS(const std::vector<u8>& image);

  // changing interfaces
  void SetGPU(GPU* gpu) { m_gpu = gpu; }

  /// Returns the address which should be used for code caching (i.e. removes mirrors).
  ALWAYS_INLINE static PhysicalMemoryAddress UnmirrorAddress(PhysicalMemoryAddress address)
  {
    // RAM
    if (address < 0x800000)
      return address & UINT32_C(0x1FFFFF);
    else
      return address;
  }

  /// Returns true if the address specified is cacheable (RAM or BIOS).
  ALWAYS_INLINE static bool IsCacheableAddress(PhysicalMemoryAddress address)
  {
    return (address < RAM_MIRROR_END) || (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_SIZE));
  }

  /// Returns true if the address specified is writable (RAM).
  ALWAYS_INLINE static bool IsRAMAddress(PhysicalMemoryAddress address) { return address < RAM_MIRROR_END; }

  /// Flags a RAM region as code, so we know when to invalidate blocks.
  ALWAYS_INLINE void SetRAMCodePage(u32 index) { m_ram_code_bits[index] = true; }

  /// Unflags a RAM region as code, the code cache will no longer be notified when writes occur.
  ALWAYS_INLINE void ClearRAMCodePage(u32 index) { m_ram_code_bits[index] = false; }

  /// Clears all code bits for RAM regions.
  ALWAYS_INLINE void ClearRAMCodePageFlags() { m_ram_code_bits.reset(); }

private:
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
    MEMCTRL2_MASK = MEMCTRL_SIZE - 1,
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
    SPU_SIZE = 0x300,
    SPU_MASK = 0x3FF,
    EXP2_BASE = 0x1F802000,
    EXP2_SIZE = 0x2000,
    EXP2_MASK = EXP2_SIZE - 1,
    BIOS_BASE = 0x1FC00000,
    BIOS_SIZE = 0x80000
  };

  enum : u32
  {
    MEMCTRL_REG_COUNT = 9
  };

  enum : TickCount
  {
    RAM_READ_ACCESS_DELAY = 5,  // Nocash docs say RAM takes 6 cycles to access. Subtract one because we already add a
                                // tick for the instruction.
    RAM_WRITE_ACCESS_DELAY = 0, // Writes are free unless we're executing more than 4 stores in a row.
  };

  union MEMDELAY
  {
    u32 bits;

    BitField<u32, u8, 4, 4> access_time; // cycles
    BitField<u32, bool, 8, 1> use_com0_time;
    BitField<u32, bool, 9, 1> use_com1_time;
    BitField<u32, bool, 10, 1> use_com2_time;
    BitField<u32, bool, 11, 1> use_com3_time;
    BitField<u32, bool, 12, 1> data_bus_16bit;
    BitField<u32, u8, 16, 5> memory_window_size;

    static constexpr u32 WRITE_MASK = 0b10101111'00011111'11111111'11111111;
  };

  union COMDELAY
  {
    u32 bits;

    BitField<u32, u8, 0, 4> com0;
    BitField<u32, u8, 4, 4> com1;
    BitField<u32, u8, 8, 4> com2;
    BitField<u32, u8, 12, 4> com3;
    BitField<u32, u8, 16, 2> comunk;

    static constexpr u32 WRITE_MASK = 0b00000000'00000011'11111111'11111111;
  };

  union MEMCTRL
  {
    u32 regs[MEMCTRL_REG_COUNT];

    struct
    {
      u32 exp1_base;
      u32 exp2_base;
      MEMDELAY exp1_delay_size;
      MEMDELAY exp3_delay_size;
      MEMDELAY bios_delay_size;
      MEMDELAY spu_delay_size;
      MEMDELAY cdrom_delay_size;
      MEMDELAY exp2_delay_size;
      COMDELAY common_delay;
    };
  };

  static std::tuple<TickCount, TickCount, TickCount> CalculateMemoryTiming(MEMDELAY mem_delay, COMDELAY common_delay);
  void RecalculateMemoryTimings();

  template<MemoryAccessType type, MemoryAccessSize size>
  TickCount DoRAMAccess(u32 offset, u32& value);

  template<MemoryAccessType type, MemoryAccessSize size>
  TickCount DoBIOSAccess(u32 offset, u32& value);

  TickCount DoInvalidAccess(MemoryAccessType type, MemoryAccessSize size, PhysicalMemoryAddress address, u32& value);

  u32 DoReadEXP1(MemoryAccessSize size, u32 offset);
  void DoWriteEXP1(MemoryAccessSize size, u32 offset, u32 value);

  u32 DoReadEXP2(MemoryAccessSize size, u32 offset);
  void DoWriteEXP2(MemoryAccessSize size, u32 offset, u32 value);

  u32 DoReadMemoryControl(MemoryAccessSize size, u32 offset);
  void DoWriteMemoryControl(MemoryAccessSize size, u32 offset, u32 value);

  u32 DoReadMemoryControl2(MemoryAccessSize size, u32 offset);
  void DoWriteMemoryControl2(MemoryAccessSize size, u32 offset, u32 value);

  u32 DoReadPad(MemoryAccessSize size, u32 offset);
  void DoWritePad(MemoryAccessSize size, u32 offset, u32 value);

  u32 DoReadSIO(MemoryAccessSize size, u32 offset);
  void DoWriteSIO(MemoryAccessSize size, u32 offset, u32 value);

  u32 DoReadCDROM(MemoryAccessSize size, u32 offset);
  void DoWriteCDROM(MemoryAccessSize size, u32 offset, u32 value);

  u32 DoReadGPU(MemoryAccessSize size, u32 offset);
  void DoWriteGPU(MemoryAccessSize size, u32 offset, u32 value);

  u32 DoReadMDEC(MemoryAccessSize size, u32 offset);
  void DoWriteMDEC(MemoryAccessSize size, u32 offset, u32 value);

  u32 DoReadInterruptController(MemoryAccessSize size, u32 offset);
  void DoWriteInterruptController(MemoryAccessSize size, u32 offset, u32 value);

  u32 DoReadDMA(MemoryAccessSize size, u32 offset);
  void DoWriteDMA(MemoryAccessSize size, u32 offset, u32 value);

  u32 DoReadTimers(MemoryAccessSize size, u32 offset);
  void DoWriteTimers(MemoryAccessSize size, u32 offset, u32 value);

  u32 DoReadSPU(MemoryAccessSize size, u32 offset);
  void DoWriteSPU(MemoryAccessSize size, u32 offset, u32 value);

  void DoInvalidateCodeCache(u32 page_index);

  /// Direct access to RAM - used by DMA.
  ALWAYS_INLINE u8* GetRAM() { return m_ram.data(); }

  /// Returns the number of cycles stolen by DMA RAM access.
  ALWAYS_INLINE static TickCount GetDMARAMTickCount(u32 word_count)
  {
    // DMA is using DRAM Hyper Page mode, allowing it to access DRAM rows at 1 clock cycle per word (effectively around
    // 17 clks per 16 words, due to required row address loading, probably plus some further minimal overload due to
    // refresh cycles). This is making DMA much faster than CPU memory accesses (CPU DRAM access takes 1 opcode cycle
    // plus 6 waitstates, ie. 7 cycles in total).
    return static_cast<TickCount>(word_count + ((word_count + 15) / 16));
  }

  /// Invalidates any code pages which overlap the specified range.
  ALWAYS_INLINE void InvalidateCodePages(PhysicalMemoryAddress address, u32 word_count)
  {
    const u32 start_page = address / CPU_CODE_CACHE_PAGE_SIZE;
    const u32 end_page = (address + word_count * sizeof(u32)) / CPU_CODE_CACHE_PAGE_SIZE;
    for (u32 page = start_page; page <= end_page; page++)
    {
      if (m_ram_code_bits[page])
        DoInvalidateCodeCache(page);
    }
  }

  CPU::Core* m_cpu = nullptr;
  CPU::CodeCache* m_cpu_code_cache = nullptr;
  DMA* m_dma = nullptr;
  InterruptController* m_interrupt_controller = nullptr;
  GPU* m_gpu = nullptr;
  CDROM* m_cdrom = nullptr;
  Pad* m_pad = nullptr;
  Timers* m_timers = nullptr;
  SPU* m_spu = nullptr;
  MDEC* m_mdec = nullptr;
  SIO* m_sio = nullptr;

  std::array<TickCount, 3> m_exp1_access_time = {};
  std::array<TickCount, 3> m_exp2_access_time = {};
  std::array<TickCount, 3> m_bios_access_time = {};
  std::array<TickCount, 3> m_cdrom_access_time = {};
  std::array<TickCount, 3> m_spu_access_time = {};

  std::bitset<CPU_CODE_CACHE_PAGE_COUNT> m_ram_code_bits{};
  std::array<u8, RAM_SIZE> m_ram{};   // 2MB RAM
  std::array<u8, BIOS_SIZE> m_bios{}; // 512K BIOS ROM
  std::vector<u8> m_exp1_rom;

  MEMCTRL m_MEMCTRL = {};
  u32 m_ram_size_reg = 0;

  std::string m_tty_line_buffer;
};

#include "bus.inl"
