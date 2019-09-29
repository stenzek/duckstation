#pragma once

#include "YBaseLib/String.h"
#include "types.h"
#include <array>

class StateWrapper;

namespace CPU {
class Core;
}

class DMA;
class InterruptController;
class GPU;
class CDROM;
class Pad;
class Timers;
class SPU;
class MDEC;
class System;

class Bus
{
public:
  Bus();
  ~Bus();

  bool Initialize(CPU::Core* cpu, DMA* dma, InterruptController* interrupt_controller, GPU* gpu, CDROM* cdrom, Pad* pad,
                  Timers* timers, SPU* spu, MDEC* mdec);
  void Reset();
  bool DoState(StateWrapper& sw);

  bool ReadByte(PhysicalMemoryAddress address, u8* value);
  bool ReadHalfWord(PhysicalMemoryAddress address, u16* value);
  bool ReadWord(PhysicalMemoryAddress address, u32* value);
  bool WriteByte(PhysicalMemoryAddress address, u8 value);
  bool WriteHalfWord(PhysicalMemoryAddress address, u16 value);
  bool WriteWord(PhysicalMemoryAddress address, u32 value);

  template<MemoryAccessType type, MemoryAccessSize size>
  bool DispatchAccess(PhysicalMemoryAddress address, u32& value);

  void PatchBIOS(u32 address, u32 value, u32 mask = UINT32_C(0xFFFFFFFF));
  void SetExpansionROM(std::vector<u8> data);

private:
  enum : u32
  {
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

  union MEMCTRL
  {
    u32 regs[MEMCTRL_REG_COUNT];

    struct
    {
      u32 exp1_base;
      u32 exp2_base;
      u32 exp1_delay_size;
      u32 exp3_delay_size;
      u32 bios_delay_size;
      u32 spu_delay_size;
      u32 cdrom_delay_size;
      u32 exp2_delay_size;
      u32 common_delay_size;
    };
  };

  bool LoadBIOS();

  template<MemoryAccessType type, MemoryAccessSize size>
  bool DoRAMAccess(u32 offset, u32& value);

  template<MemoryAccessType type, MemoryAccessSize size>
  bool DoBIOSAccess(u32 offset, u32& value);

  bool DoInvalidAccess(MemoryAccessType type, MemoryAccessSize size, PhysicalMemoryAddress address, u32& value);

  bool DoReadEXP1(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteEXP1(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadEXP2(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteEXP2(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadMemoryControl(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteMemoryControl(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadMemoryControl2(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteMemoryControl2(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadPad(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWritePad(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadSIO(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteSIO(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadCDROM(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteCDROM(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadGPU(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteGPU(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadMDEC(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteMDEC(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadInterruptController(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteInterruptController(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadDMA(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteDMA(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadTimers(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteTimers(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadSPU(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteSPU(MemoryAccessSize size, u32 offset, u32 value);

  CPU::Core* m_cpu = nullptr;
  DMA* m_dma = nullptr;
  InterruptController* m_interrupt_controller = nullptr;
  GPU* m_gpu = nullptr;
  CDROM* m_cdrom = nullptr;
  Pad* m_pad = nullptr;
  Timers* m_timers = nullptr;
  SPU* m_spu = nullptr;
  MDEC* m_mdec = nullptr;

  std::array<u8, 2097152> m_ram{}; // 2MB RAM
  std::array<u8, 524288> m_bios{}; // 512K BIOS ROM
  std::vector<u8> m_exp1_rom;

  MEMCTRL m_MEMCTRL = {};
  u32 m_ram_size_reg = 0;

  String m_tty_line_buffer;
};

#include "bus.inl"