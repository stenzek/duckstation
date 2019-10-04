#pragma once
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "types.h"
#include <array>

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
  static constexpr u32 DATA_IN_FIFO_SIZE = 1048576;
  static constexpr u32 DATA_OUT_FIFO_SIZE = 1048576;

  enum DataOutputDepth : u8
  {
    DataOutputDepth_4Bit = 0,
    DataOutputDepth_8Bit = 1,
    DataOutputDepth_24Bit = 2,
    DataOutputDepth_15Bit = 3
  };

  enum class Command : u8
  {
    None = 0,
    DecodeMacroblock = 1,
    SetIqTab = 2,
    SetScale = 3
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

  union CommandWord
  {
    u32 bits;

    BitField<u32, Command, 29, 3> command;
    BitField<u32, DataOutputDepth, 27, 2> data_output_depth;
    BitField<u32, bool, 26, 1> data_output_signed;
    BitField<u32, u8, 25, 1> data_output_bit15;
    BitField<u32, u16, 0, 16> parameter_word_count;
  };

  void SoftReset();
  void UpdateStatusRegister();

  u32 ReadDataRegister();
  void WriteCommandRegister(u32 value);

  void HandleDecodeMacroblockCommand();
  void HandleSetQuantTableCommand();
  void HandleSetScaleCommand();

  const u16* DecodeColoredMacroblock(const u16* src, const u16* src_end);
  const u16* DecodeMonoMacroblock(const u16* src, const u16* src_end);

  // from nocash spec
  bool rl_decode_block(s16* blk, const u16*& src, const u16* src_end, const u8* qt);
  void IDCT(s16* blk);
  void yuv_to_rgb(u32 xx, u32 yy, const std::array<s16, 64>& Crblk, const std::array<s16, 64>& Cbblk,
                  const std::array<s16, 64>& Yblk, std::array<u32, 256>& rgb_out);
  void y_to_mono(const std::array<s16, 64>& Yblk, std::array<u8, 64>& r_out);

  System* m_system = nullptr;
  DMA* m_dma = nullptr;

  StatusRegister m_status = {};

  InlineFIFOQueue<u32, DATA_IN_FIFO_SIZE> m_data_in_fifo;
  InlineFIFOQueue<u32, DATA_OUT_FIFO_SIZE> m_data_out_fifo;
  Command m_command = Command::None;
  u32 m_command_parameter_count = 0;

  std::array<u8, 64> m_iq_uv{};
  std::array<u8, 64> m_iq_y{};

  std::array<s16, 64> m_scale_table{};
};
