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
class System;

class Bus
{
public:
  Bus();
  ~Bus();

  bool Initialize(CPU::Core* cpu, DMA* dma, InterruptController* interrupt_controller, GPU* gpu, CDROM* cdrom, Pad* pad,
                  Timers* timers, SPU* spu);
  void Reset();
  bool DoState(StateWrapper& sw);

  bool ReadByte(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u8* value);
  bool ReadHalfWord(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u16* value);
  bool ReadWord(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u32* value);
  bool WriteByte(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u8 value);
  bool WriteHalfWord(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u16 value);
  bool WriteWord(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u32 value);

  template<MemoryAccessType type, MemoryAccessSize size>
  bool DispatchAccess(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u32& value);

  void PatchBIOS(u32 address, u32 value, u32 mask = UINT32_C(0xFFFFFFFF));
  void SetExpansionROM(std::vector<u8> data);

private:
  static constexpr u32 EXP1_BASE = 0x1F000000;
  static constexpr u32 EXP1_SIZE = 0x800000;
  static constexpr u32 EXP1_MASK = EXP1_SIZE - 1;
  static constexpr u32 PAD_BASE = 0x1F801040;
  static constexpr u32 PAD_SIZE = 0x10;
  static constexpr u32 PAD_MASK = PAD_SIZE - 1;
  static constexpr u32 SIO_BASE = 0x1F801050;
  static constexpr u32 SIO_SIZE = 0x10;
  static constexpr u32 SIO_MASK = SIO_SIZE - 1;
  static constexpr u32 INTERRUPT_CONTROLLER_BASE = 0x1F801070;
  static constexpr u32 INTERRUPT_CONTROLLER_SIZE = 0x08;
  static constexpr u32 INTERRUPT_CONTROLLER_MASK = INTERRUPT_CONTROLLER_SIZE - 1;
  static constexpr u32 DMA_BASE = 0x1F801080;
  static constexpr u32 DMA_SIZE = 0x80;
  static constexpr u32 DMA_MASK = DMA_SIZE - 1;
  static constexpr u32 TIMERS_BASE = 0x1F801100;
  static constexpr u32 TIMERS_SIZE = 0x40;
  static constexpr u32 TIMERS_MASK = TIMERS_SIZE - 1;
  static constexpr u32 CDROM_BASE = 0x1F801800;
  static constexpr u32 CDROM_SIZE = 0x04;
  static constexpr u32 CDROM_MASK = CDROM_SIZE - 1;
  static constexpr u32 GPU_BASE = 0x1F801810;
  static constexpr u32 GPU_SIZE = 0x10;
  static constexpr u32 GPU_MASK = GPU_SIZE - 1;
  static constexpr u32 SPU_BASE = 0x1F801C00;
  static constexpr u32 SPU_SIZE = 0x300;
  static constexpr u32 SPU_MASK = 0x3FF;
  static constexpr u32 EXP2_BASE = 0x1F802000;
  static constexpr u32 EXP2_SIZE = 0x2000;
  static constexpr u32 EXP2_MASK = EXP2_SIZE - 1;
  static constexpr u32 BIOS_BASE = 0x1FC00000;
  static constexpr u32 BIOS_SIZE = 0x80000;

  bool LoadBIOS();

  template<MemoryAccessType type, MemoryAccessSize size>
  bool DoRAMAccess(u32 offset, u32& value);

  template<MemoryAccessType type, MemoryAccessSize size>
  bool DoBIOSAccess(u32 offset, u32& value);

  bool DoInvalidAccess(MemoryAccessType type, MemoryAccessSize size, PhysicalMemoryAddress cpu_address,
                       PhysicalMemoryAddress bus_address, u32& value);

  bool DoReadEXP1(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteEXP1(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadEXP2(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteEXP2(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadPad(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWritePad(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadSIO(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteSIO(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadCDROM(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteCDROM(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadGPU(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteGPU(MemoryAccessSize size, u32 offset, u32 value);

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

  std::array<u8, 2097152> m_ram{}; // 2MB RAM
  std::array<u8, 524288> m_bios{}; // 512K BIOS ROM
  std::vector<u8> m_exp1_rom;

  String m_tty_line_buffer;
};

#include "bus.inl"