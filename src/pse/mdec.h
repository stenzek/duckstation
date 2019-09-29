#pragma once
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "types.h"
#include <string>
#include <vector>

class StateWrapper;

class System;
class DMA;

class MDEC
{
public:
  MDEC();
  ~MDEC();

  bool Initialize(System* system, DMA* dma);
  void Reset();
  bool DoState(StateWrapper& sw);

  // I/O
  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

  u32 DMARead();
  void DMAWrite(u32 value);

private:
  static constexpr u32 DATA_IN_FIFO_SIZE = 256;
  static constexpr u32 DATA_OUT_FIFO_SIZE = 256;

  enum DataOutputDepth : u8
  {
    DataOutputDepth_4Bit = 0,
    DataOutputDepth_8Bit = 1,
    DataOutputDepth_24Bit = 2,
    DataOutputDepth_15Bit = 3
  };

  union StatusRegister
  {
    u32 bits;

    BitField<u32, bool, 31, 1> data_out_fifo_empty;
    BitField<u32, bool, 30, 1> data_in_fifo_full;
    BitField<u32, bool, 29, 1> command_busy;
    BitField<u32, bool, 28, 1> data_in_request;
    BitField<u32, bool, 27, 1> data_out_request;
    BitField<u32, DataOutputDepth, 25, 2> data_output_depth;
    BitField<u32, bool, 24, 1> data_output_signed;
    BitField<u32, u8, 23, 1> data_output_bit15;
    BitField<u32, u8, 16, 3> current_block;
    BitField<u32, u16, 0, 16> parameter_words_remaining;
  };

  union ControlRegister
  {
    u32 bits;
    BitField<u32, bool, 31, 1> reset;
    BitField<u32, bool, 30, 1> enable_dma_in;
    BitField<u32, bool, 29, 1> enable_dma_out;
  };

  void SoftReset();
  void UpdateStatusRegister();

  void WriteCommandRegister(u32 value);
  u32 ReadDataRegister();

  void HandleCommand();
  
  System* m_system = nullptr;
  DMA* m_dma = nullptr;

  StatusRegister m_status_register = {};
  
  InlineFIFOQueue<u32, DATA_IN_FIFO_SIZE> m_data_in_fifo;
  InlineFIFOQueue<u32, DATA_OUT_FIFO_SIZE> m_data_out_fifo;
};
