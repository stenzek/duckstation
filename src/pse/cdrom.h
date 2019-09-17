#pragma once
#include "types.h"
#include "common/bitfield.h"
#include "common/fifo_queue.h"

class StateWrapper;

class DMA;
class InterruptController;

class CDROM
{
public:
  CDROM();
  ~CDROM();

  bool Initialize(DMA* dma, InterruptController* interrupt_controller);
  void Reset();
  bool DoState(StateWrapper& sw);

  // I/O
  u8 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u8 value);

  void Execute();

private:
  static constexpr u32 PARAM_FIFO_SIZE = 16;
  static constexpr u32 RESPONSE_FIFO_SIZE = 16;
  static constexpr u32 DATA_FIFO_SIZE = 4096;
  static constexpr u32 NUM_INTERRUPTS = 32;
  static constexpr u8 INTERRUPT_REGISTER_MASK = 0x1F;

  bool HasPendingInterrupt() const { return m_interrupt_flag_register != 0; }
  void WriteCommand(u8 command);

  DMA* m_dma;
  InterruptController* m_interrupt_controller;

  enum class State : u32
  {
    Idle
  };

  State m_state = State::Idle;

  union
  {
    u8 bits;
    BitField<u8, u8, 0, 2> index;
    BitField<u8, bool, 2, 1> ADPBUSY;
    BitField<u8, bool, 3, 1> PRMEMPTY;
    BitField<u8, bool, 4, 1> PRMWRDY;
    BitField<u8, bool, 5, 1> RSLRRDY;
    BitField<u8, bool, 6, 1> DRQSTS;
    BitField<u8, bool, 7, 1> BUSYSTS;
  } m_status = {};

  u8 m_interrupt_enable_register = INTERRUPT_REGISTER_MASK;
  u8 m_interrupt_flag_register = 0;

  InlineFIFOQueue<u8, PARAM_FIFO_SIZE> m_param_fifo;
  InlineFIFOQueue<u8, RESPONSE_FIFO_SIZE> m_response_fifo;
  HeapFIFOQueue<u8, DATA_FIFO_SIZE> m_data_fifo;
};

