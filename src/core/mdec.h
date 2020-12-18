#pragma once
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "types.h"
#include <array>
#include <memory>

class StateWrapper;

class TimingEvent;

class MDEC
{
public:
  MDEC();
  ~MDEC();

  void Initialize();
  void Shutdown();
  void Reset();
  bool DoState(StateWrapper& sw);

  // I/O
  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

  void DMARead(u32* words, u32 word_count);
  void DMAWrite(const u32* words, u32 word_count);

  void DrawDebugStateWindow();

private:
  static constexpr u32 DATA_IN_FIFO_SIZE = 1024;
  static constexpr u32 DATA_OUT_FIFO_SIZE = 768;
  static constexpr u32 NUM_BLOCKS = 6;

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

  enum class State : u8
  {
    Idle,
    DecodingMacroblock,
    WritingMacroblock,
    SetIqTable,
    SetScaleTable,
    NoCommand
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

  bool HasPendingBlockCopyOut() const;

  void SoftReset();
  void ResetDecoder();
  void UpdateStatus();

  u32 ReadDataRegister();
  void WriteCommandRegister(u32 value);
  void Execute();

  bool HandleDecodeMacroblockCommand();
  void HandleSetQuantTableCommand();
  void HandleSetScaleCommand();

  bool DecodeMonoMacroblock();
  bool DecodeColoredMacroblock();
  void ScheduleBlockCopyOut(TickCount ticks);
  void CopyOutBlock();

  // from nocash spec
  bool rl_decode_block(s16* blk, const u8* qt);
  void IDCT(s16* blk);
  void yuv_to_rgb(u32 xx, u32 yy, const std::array<s16, 64>& Crblk, const std::array<s16, 64>& Cbblk,
                  const std::array<s16, 64>& Yblk);
  void y_to_mono(const std::array<s16, 64>& Yblk);

  StatusRegister m_status = {};
  bool m_enable_dma_in = false;
  bool m_enable_dma_out = false;

  // Even though the DMA is in words, we access the FIFO as halfwords.
  InlineFIFOQueue<u16, DATA_IN_FIFO_SIZE / sizeof(u16)> m_data_in_fifo;
  InlineFIFOQueue<u32, DATA_OUT_FIFO_SIZE / sizeof(u32)> m_data_out_fifo;
  State m_state = State::Idle;
  u32 m_remaining_halfwords = 0;

  std::array<u8, 64> m_iq_uv{};
  std::array<u8, 64> m_iq_y{};

  std::array<s16, 64> m_scale_table{};

  // blocks, for colour: 0 - Crblk, 1 - Cbblk, 2-5 - Y 1-4
  std::array<std::array<s16, 64>, NUM_BLOCKS> m_blocks;
  u32 m_current_block = 0;        // block (0-5)
  u32 m_current_coefficient = 64; // k (in block)
  u16 m_current_q_scale = 0;

  std::array<u32, 256> m_block_rgb{};
  std::unique_ptr<TimingEvent> m_block_copy_out_event;

  u32 m_total_blocks_decoded = 0;
};

extern MDEC g_mdec;