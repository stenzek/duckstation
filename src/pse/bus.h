#pragma once

#include "YBaseLib/String.h"
#include "types.h"
#include <array>

class StateWrapper;
class DMA;
class GPU;
class System;

class Bus
{
public:
  Bus();
  ~Bus();

  bool Initialize(System* system, DMA* dma, GPU* gpu);
  void Reset();
  bool DoState(StateWrapper& sw);

  bool ReadByte(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u8* value);
  bool ReadWord(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u16* value);
  bool ReadDWord(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u32* value);
  bool WriteByte(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u8 value);
  bool WriteWord(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u16 value);
  bool WriteDWord(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u32 value);

  template<MemoryAccessType type, MemoryAccessSize size>
  bool DispatchAccess(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u32& value);

private:
  static constexpr u32 DMA_BASE = 0x1F801080;
  static constexpr u32 DMA_SIZE = 0x80;
  static constexpr u32 DMA_MASK = DMA_SIZE - 1;
  static constexpr u32 SPU_BASE = 0x1F801C00;
  static constexpr u32 SPU_SIZE = 0x300;
  static constexpr u32 SPU_MASK = 0x3FF;
  static constexpr u32 EXP2_BASE = 0x1F802000;
  static constexpr u32 EXP2_SIZE = 0x2000;
  static constexpr u32 EXP2_MASK = EXP2_SIZE - 1;

  bool LoadBIOS();

  template<MemoryAccessType type, MemoryAccessSize size>
  bool DoRAMAccess(u32 offset, u32& value);

  template<MemoryAccessType type, MemoryAccessSize size>
  bool DoBIOSAccess(u32 offset, u32& value);

  bool DoInvalidAccess(MemoryAccessType type, MemoryAccessSize size, PhysicalMemoryAddress cpu_address,
                       PhysicalMemoryAddress bus_address, u32& value);

  bool ReadExpansionRegion2(MemoryAccessSize size, u32 offset, u32& value);
  bool WriteExpansionRegion2(MemoryAccessSize size, u32 offset, u32 value);

  bool ReadSPU(MemoryAccessSize size, u32 offset, u32& value);
  bool WriteSPU(MemoryAccessSize size, u32 offset, u32 value);

  bool DoReadDMA(MemoryAccessSize size, u32 offset, u32& value);
  bool DoWriteDMA(MemoryAccessSize size, u32 offset, u32& value);

  DMA* m_dma = nullptr;
  GPU* m_gpu = nullptr;

  std::array<u8, 2097152> m_ram{}; // 2MB RAM
  std::array<u8, 524288> m_bios{}; // 512K BIOS ROM

  String m_tty_line_buffer;
};

#include "bus.inl"