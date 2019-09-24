#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <array>

class StateWrapper;

class System;
class DMA;
class InterruptController;

class SPU
{
public:
  SPU();
  ~SPU();

  bool Initialize(System* system, DMA* dma, InterruptController* interrupt_controller);
  void Reset();
  bool DoState(StateWrapper& sw);

  u16 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u16 value);

  u32 DMARead();
  void DMAWrite(u32 value);

private:
  static constexpr u32 RAM_SIZE = 512 * 1024;
  static constexpr u32 RAM_MASK = RAM_SIZE - 1;
  static constexpr u32 SPU_BASE = 0x1F801C00;

  enum class RAMTransferMode : u8
  {
    Stopped =0,
    ManualWrite = 1,
    DMAWrite = 2,
    DMARead = 3
  };

  union SPUCNT
  {
    u16 bits;

    BitField<u16, bool, 15, 1> enable;
    BitField<u16, bool, 14, 1> mute;
    BitField<u16, u8, 10, 4> noise_frequency_shift;
    BitField<u16, u8, 8, 2> noise_frequency_step;
    BitField<u16, bool, 7, 1> reverb_master_enable;
    BitField<u16, bool, 6, 1> irq9_enable;
    BitField<u16, RAMTransferMode, 4, 2> ram_transfer_mode;
    BitField<u16, bool, 3, 1> external_audio_reverb;
    BitField<u16, bool, 2, 1> cd_audio_reverb;
    BitField<u16, bool, 1, 1> external_audio_enable;
    BitField<u16, bool, 0, 1> cd_audio_enable;

    BitField<u16, u8, 0, 6> mode;
  };

  union SPUSTAT
  {
    u16 bits;

    BitField<u16, bool, 11, 1> second_half_capture_buffer;
    BitField<u16, bool, 10, 1> transfer_busy;
    BitField<u16, bool, 9, 1> dma_read_request;
    BitField<u16, bool, 8, 1> dma_write_request;
    BitField<u16, bool, 7, 1> dma_read_write_request;
    BitField<u16, bool, 6, 1> irq9_flag;
    BitField<u16, u8, 0, 6> mode;
  };

#if 0
  struct Voice
  {
    static constexpr u32 NUM_REGS = 8;
    static constexpr u32 NUM_FLAGS = 6;

    std::array<u16, NUM_REGS> regs;
    
  };
#endif

  void UpdateDMARequest();

  System* m_system = nullptr;
  DMA* m_dma = nullptr;
  InterruptController* m_interrupt_controller = nullptr;

  SPUCNT m_SPUCNT = {};
  SPUSTAT m_SPUSTAT = {};

  u16 m_transfer_address_reg = 0;
  u32 m_transfer_address = 0;

  std::array<u8, RAM_SIZE> m_ram{};
};