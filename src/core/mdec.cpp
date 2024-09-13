// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "mdec.h"
#include "cpu_core.h"
#include "dma.h"
#include "system.h"
#include "timing_event.h"

#include "util/imgui_manager.h"
#include "util/state_wrapper.h"

#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "common/gsvector.h"
#include "common/log.h"

#include "imgui.h"

#include <array>
#include <memory>

Log_SetChannel(MDEC);

namespace MDEC {
namespace {

static constexpr u32 DATA_IN_FIFO_SIZE = 1024;
static constexpr u32 DATA_OUT_FIFO_SIZE = 768;
static constexpr u32 NUM_BLOCKS = 6;
static constexpr TickCount TICKS_PER_BLOCK = 448;

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

} // namespace

static bool HasPendingBlockCopyOut();

static void SoftReset();
static void ResetDecoder();
static void UpdateStatus();

static u32 ReadDataRegister();
static void WriteCommandRegister(u32 value);
static void Execute();

static bool HandleDecodeMacroblockCommand();
static void HandleSetQuantTableCommand();
static void HandleSetScaleCommand();

static void SetScaleMatrix(const u16* values);
static bool DecodeMonoMacroblock();
static bool DecodeColoredMacroblock();
static void ScheduleBlockCopyOut(TickCount ticks);
static void CopyOutBlock(void* param, TickCount ticks, TickCount ticks_late);

static bool DecodeRLE_Old(s16* blk, const u8* qt);
static void IDCT_Old(s16* blk);
static void YUVToRGB_Old(u32 xx, u32 yy, const std::array<s16, 64>& Crblk, const std::array<s16, 64>& Cbblk,
                         const std::array<s16, 64>& Yblk);

static bool DecodeRLE_New(s16* blk, const u8* qt);
static void IDCT_New(s16* blk);
static void YUVToRGB_New(u32 xx, u32 yy, const std::array<s16, 64>& Crblk, const std::array<s16, 64>& Cbblk,
                         const std::array<s16, 64>& Yblk);

static void YUVToMono(const std::array<s16, 64>& Yblk);

namespace {
struct MDECState
{
  StatusRegister status = {};
  bool enable_dma_in = false;
  bool enable_dma_out = false;

  // Even though the DMA is in words, we access the FIFO as halfwords.
  InlineFIFOQueue<u16, DATA_IN_FIFO_SIZE / sizeof(u16)> data_in_fifo;
  InlineFIFOQueue<u32, DATA_OUT_FIFO_SIZE / sizeof(u32)> data_out_fifo;
  State state = State::Idle;
  u32 remaining_halfwords = 0;

  std::array<u8, 64> iq_uv{};
  std::array<u8, 64> iq_y{};

  alignas(VECTOR_ALIGNMENT) std::array<s16, 64> scale_table{};

  // blocks, for colour: 0 - Crblk, 1 - Cbblk, 2-5 - Y 1-4
  alignas(VECTOR_ALIGNMENT) std::array<std::array<s16, 64>, NUM_BLOCKS> blocks;
  u32 current_block = 0;        // block (0-5)
  u32 current_coefficient = 64; // k (in block)
  u16 current_q_scale = 0;

  alignas(VECTOR_ALIGNMENT) std::array<u32, 256> block_rgb{};
  TimingEvent block_copy_out_event{"MDEC Block Copy Out", 1, 1, &MDEC::CopyOutBlock, nullptr};

  u32 total_blocks_decoded = 0;
};
} // namespace

ALIGN_TO_CACHE_LINE static MDECState s_state;
} // namespace MDEC

void MDEC::Initialize()
{
  s_state.total_blocks_decoded = 0;
  Reset();
}

void MDEC::Shutdown()
{
  s_state.block_copy_out_event.Deactivate();
}

void MDEC::Reset()
{
  s_state.block_copy_out_event.Deactivate();
  SoftReset();
}

bool MDEC::DoState(StateWrapper& sw)
{
  sw.Do(&s_state.status.bits);
  sw.Do(&s_state.enable_dma_in);
  sw.Do(&s_state.enable_dma_out);
  sw.Do(&s_state.data_in_fifo);
  sw.Do(&s_state.data_out_fifo);
  sw.Do(&s_state.state);
  sw.Do(&s_state.remaining_halfwords);
  sw.Do(&s_state.iq_uv);
  sw.Do(&s_state.iq_y);

  if (sw.GetVersion() < 66) [[unlikely]]
  {
    std::array<u16, 64> old_scale_matrix;
    sw.Do(&old_scale_matrix);
    SetScaleMatrix(old_scale_matrix.data());
  }
  else
  {
    sw.Do(&s_state.scale_table);
  }

  sw.Do(&s_state.blocks);
  sw.Do(&s_state.current_block);
  sw.Do(&s_state.current_coefficient);
  sw.Do(&s_state.current_q_scale);
  sw.Do(&s_state.block_rgb);

  bool block_copy_out_pending = HasPendingBlockCopyOut();
  sw.Do(&block_copy_out_pending);
  if (sw.IsReading())
    s_state.block_copy_out_event.SetState(block_copy_out_pending);

  return !sw.HasError();
}

u32 MDEC::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0:
      return ReadDataRegister();

    case 4:
    {
      TRACE_LOG("MDEC status register -> 0x{:08X}", s_state.status.bits);
      return s_state.status.bits;
    }

      [[unlikely]] default:
      {
        ERROR_LOG("Unknown MDEC register read: 0x{:08X}", offset);
        return UINT32_C(0xFFFFFFFF);
      }
  }
}

void MDEC::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0:
    {
      WriteCommandRegister(value);
      return;
    }

    case 4:
    {
      DEBUG_LOG("MDEC control register <- 0x{:08X}", value);

      const ControlRegister cr{value};
      if (cr.reset)
        SoftReset();

      s_state.enable_dma_in = cr.enable_dma_in;
      s_state.enable_dma_out = cr.enable_dma_out;
      Execute();
      return;
    }

      [[unlikely]] default:
      {
        ERROR_LOG("Unknown MDEC register write: 0x{:08X} <- 0x{:08X}", offset, value);
        return;
      }
  }
}

void MDEC::DMARead(u32* words, u32 word_count)
{
  if (s_state.data_out_fifo.GetSize() < word_count) [[unlikely]]
  {
    WARNING_LOG("Insufficient data in output FIFO (requested {}, have {})", word_count,
                s_state.data_out_fifo.GetSize());
  }

  const u32 words_to_read = std::min(word_count, s_state.data_out_fifo.GetSize());
  if (words_to_read > 0)
  {
    s_state.data_out_fifo.PopRange(words, words_to_read);
    words += words_to_read;
    word_count -= words_to_read;
  }

  DEBUG_LOG("DMA read complete, {} bytes left", s_state.data_out_fifo.GetSize() * sizeof(u32));
  if (s_state.data_out_fifo.IsEmpty())
    Execute();
}

void MDEC::DMAWrite(const u32* words, u32 word_count)
{
  if (s_state.data_in_fifo.GetSpace() < (word_count * 2)) [[unlikely]]
  {
    WARNING_LOG("Input FIFO overflow (writing {}, space {})", word_count * 2, s_state.data_in_fifo.GetSpace());
  }

  const u32 halfwords_to_write = std::min(word_count * 2, s_state.data_in_fifo.GetSpace() & ~u32(2));
  s_state.data_in_fifo.PushRange(reinterpret_cast<const u16*>(words), halfwords_to_write);
  Execute();
}

bool MDEC::HasPendingBlockCopyOut()
{
  return s_state.block_copy_out_event.IsActive();
}

void MDEC::SoftReset()
{
  s_state.status.bits = 0;
  s_state.enable_dma_in = false;
  s_state.enable_dma_out = false;
  s_state.data_in_fifo.Clear();
  s_state.data_out_fifo.Clear();
  s_state.state = State::Idle;
  s_state.remaining_halfwords = 0;
  s_state.current_block = 0;
  s_state.current_coefficient = 64;
  s_state.current_q_scale = 0;
  s_state.block_copy_out_event.Deactivate();
  UpdateStatus();
}

void MDEC::ResetDecoder()
{
  s_state.current_block = 0;
  s_state.current_coefficient = 64;
  s_state.current_q_scale = 0;
}

void MDEC::UpdateStatus()
{
  s_state.status.data_out_fifo_empty = s_state.data_out_fifo.IsEmpty();
  s_state.status.data_in_fifo_full = s_state.data_in_fifo.IsFull();

  s_state.status.command_busy = (s_state.state != State::Idle);
  s_state.status.parameter_words_remaining = Truncate16((s_state.remaining_halfwords / 2) - 1);
  s_state.status.current_block = (s_state.current_block + 4) % NUM_BLOCKS;

  // we always want data in if it's enabled
  const bool data_in_request = s_state.enable_dma_in && s_state.data_in_fifo.GetSpace() >= (32 * 2);
  s_state.status.data_in_request = data_in_request;
  DMA::SetRequest(DMA::Channel::MDECin, data_in_request);

  // we only want to send data out if we have some in the fifo
  const bool data_out_request = s_state.enable_dma_out && !s_state.data_out_fifo.IsEmpty();
  s_state.status.data_out_request = data_out_request;
  DMA::SetRequest(DMA::Channel::MDECout, data_out_request);
}

u32 MDEC::ReadDataRegister()
{
  if (s_state.data_out_fifo.IsEmpty())
  {
    // Stall the CPU until we're done processing.
    if (HasPendingBlockCopyOut())
    {
      DEV_LOG("MDEC data out FIFO empty on read - stalling CPU");
      CPU::AddPendingTicks(s_state.block_copy_out_event.GetTicksUntilNextExecution());
    }
    else
    {
      WARNING_LOG("MDEC data out FIFO empty on read and no data processing");
      return UINT32_C(0xFFFFFFFF);
    }
  }

  const u32 value = s_state.data_out_fifo.Pop();
  if (s_state.data_out_fifo.IsEmpty())
    Execute();
  else
    UpdateStatus();

  return value;
}

void MDEC::WriteCommandRegister(u32 value)
{
  TRACE_LOG("MDEC command/data register <- 0x{:08X}", value);

  s_state.data_in_fifo.Push(Truncate16(value));
  s_state.data_in_fifo.Push(Truncate16(value >> 16));

  Execute();
}

void MDEC::Execute()
{
  for (;;)
  {
    switch (s_state.state)
    {
      case State::Idle:
      {
        if (s_state.data_in_fifo.GetSize() < 2)
          goto finished;

        // first word
        const CommandWord cw{ZeroExtend32(s_state.data_in_fifo.Peek(0)) |
                             (ZeroExtend32(s_state.data_in_fifo.Peek(1)) << 16)};
        s_state.status.data_output_depth = cw.data_output_depth;
        s_state.status.data_output_signed = cw.data_output_signed;
        s_state.status.data_output_bit15 = cw.data_output_bit15;
        s_state.data_in_fifo.Remove(2);
        s_state.data_out_fifo.Clear();

        u32 num_words;
        State new_state;
        switch (cw.command)
        {
          case Command::DecodeMacroblock:
            num_words = ZeroExtend32(cw.parameter_word_count.GetValue());
            new_state = State::DecodingMacroblock;
            break;

          case Command::SetIqTab:
            num_words = 16 + (((cw.bits & 1) != 0) ? 16 : 0);
            new_state = State::SetIqTable;
            break;

          case Command::SetScale:
            num_words = 32;
            new_state = State::SetScaleTable;
            break;

          default:
            [[unlikely]] DEV_LOG("Invalid MDEC command 0x{:08X}", cw.bits);
            num_words = cw.parameter_word_count.GetValue();
            new_state = State::NoCommand;
            break;
        }

        DEBUG_LOG("MDEC command: 0x{:08X} ({}, {} words in parameter, {} expected)", cw.bits,
                  static_cast<u8>(cw.command.GetValue()), cw.parameter_word_count.GetValue(), num_words);

        s_state.remaining_halfwords = num_words * 2;
        s_state.state = new_state;
        UpdateStatus();
        continue;
      }

      case State::DecodingMacroblock:
      {
        if (HandleDecodeMacroblockCommand())
        {
          // we should be writing out now
          DebugAssert(s_state.state == State::WritingMacroblock);
          goto finished;
        }

        if (s_state.remaining_halfwords == 0 && s_state.current_block != NUM_BLOCKS)
        {
          // expecting data, but nothing more will be coming. bail out
          ResetDecoder();
          s_state.state = State::Idle;
          continue;
        }

        goto finished;
      }

      case State::WritingMacroblock:
      {
        // this gets executed via the event, so if we get here, wait.
        goto finished;
      }

      case State::SetIqTable:
      {
        if (s_state.data_in_fifo.GetSize() < s_state.remaining_halfwords)
          goto finished;

        HandleSetQuantTableCommand();
        s_state.state = State::Idle;
        UpdateStatus();
        continue;
      }

      case State::SetScaleTable:
      {
        if (s_state.data_in_fifo.GetSize() < s_state.remaining_halfwords)
          goto finished;

        HandleSetScaleCommand();
        s_state.state = State::Idle;
        UpdateStatus();
        continue;
      }

      case State::NoCommand:
      {
        // can potentially have a large amount of halfwords, so eat them as we go
        const u32 words_to_consume = std::min(s_state.remaining_halfwords, s_state.data_in_fifo.GetSize());
        s_state.data_in_fifo.Remove(words_to_consume);
        s_state.remaining_halfwords -= words_to_consume;
        if (s_state.remaining_halfwords == 0)
          goto finished;

        s_state.state = State::Idle;
        UpdateStatus();
        continue;
      }

      default:
        UnreachableCode();
        return;
    }
  }

finished:
  // if we get here, it's because the FIFO is now empty
  UpdateStatus();
}

bool MDEC::HandleDecodeMacroblockCommand()
{
  if (s_state.status.data_output_depth <= DataOutputDepth_8Bit)
    return DecodeMonoMacroblock();
  else
    return DecodeColoredMacroblock();
}

bool MDEC::DecodeMonoMacroblock()
{
  // TODO: This should guard the output not the input
  if (!s_state.data_out_fifo.IsEmpty())
    return false;

  if (g_settings.use_old_mdec_routines) [[unlikely]]
  {
    if (!DecodeRLE_Old(s_state.blocks[0].data(), s_state.iq_y.data()))
      return false;

    IDCT_Old(s_state.blocks[0].data());
  }
  else
  {
    if (!DecodeRLE_New(s_state.blocks[0].data(), s_state.iq_y.data()))
      return false;

    IDCT_New(s_state.blocks[0].data());
  }

  DEBUG_LOG("Decoded mono macroblock, {} words remaining", s_state.remaining_halfwords / 2);
  ResetDecoder();
  s_state.state = State::WritingMacroblock;

  YUVToMono(s_state.blocks[0]);

  ScheduleBlockCopyOut(TICKS_PER_BLOCK * 6);

  s_state.total_blocks_decoded++;
  return true;
}

bool MDEC::DecodeColoredMacroblock()
{
  if (g_settings.use_old_mdec_routines) [[unlikely]]
  {
    for (; s_state.current_block < NUM_BLOCKS; s_state.current_block++)
    {
      if (!DecodeRLE_Old(s_state.blocks[s_state.current_block].data(),
                         (s_state.current_block >= 2) ? s_state.iq_y.data() : s_state.iq_uv.data()))
        return false;

      IDCT_Old(s_state.blocks[s_state.current_block].data());
    }

    if (!s_state.data_out_fifo.IsEmpty())
      return false;

    // done decoding
    DEBUG_LOG("Decoded colored macroblock, {} words remaining", s_state.remaining_halfwords / 2);
    ResetDecoder();
    s_state.state = State::WritingMacroblock;

    YUVToRGB_Old(0, 0, s_state.blocks[0], s_state.blocks[1], s_state.blocks[2]);
    YUVToRGB_Old(8, 0, s_state.blocks[0], s_state.blocks[1], s_state.blocks[3]);
    YUVToRGB_Old(0, 8, s_state.blocks[0], s_state.blocks[1], s_state.blocks[4]);
    YUVToRGB_Old(8, 8, s_state.blocks[0], s_state.blocks[1], s_state.blocks[5]);
  }
  else
  {
    for (; s_state.current_block < NUM_BLOCKS; s_state.current_block++)
    {
      if (!DecodeRLE_New(s_state.blocks[s_state.current_block].data(),
                         (s_state.current_block >= 2) ? s_state.iq_y.data() : s_state.iq_uv.data()))
        return false;

      IDCT_New(s_state.blocks[s_state.current_block].data());
    }

    if (!s_state.data_out_fifo.IsEmpty())
      return false;

    // done decoding
    DEBUG_LOG("Decoded colored macroblock, {} words remaining", s_state.remaining_halfwords / 2);
    ResetDecoder();
    s_state.state = State::WritingMacroblock;

    YUVToRGB_New(0, 0, s_state.blocks[0], s_state.blocks[1], s_state.blocks[2]);
    YUVToRGB_New(8, 0, s_state.blocks[0], s_state.blocks[1], s_state.blocks[3]);
    YUVToRGB_New(0, 8, s_state.blocks[0], s_state.blocks[1], s_state.blocks[4]);
    YUVToRGB_New(8, 8, s_state.blocks[0], s_state.blocks[1], s_state.blocks[5]);
  }

  s_state.total_blocks_decoded += 4;

  ScheduleBlockCopyOut(TICKS_PER_BLOCK * 6);
  return true;
}

void MDEC::ScheduleBlockCopyOut(TickCount ticks)
{
  DebugAssert(!HasPendingBlockCopyOut());
  DEBUG_LOG("Scheduling block copy out in {} ticks", ticks);

  s_state.block_copy_out_event.SetIntervalAndSchedule(ticks);
}

void MDEC::CopyOutBlock(void* param, TickCount ticks, TickCount ticks_late)
{
  Assert(s_state.state == State::WritingMacroblock);
  s_state.block_copy_out_event.Deactivate();

  switch (s_state.status.data_output_depth)
  {
    // Not worth vectorizing these, they're basically never used.
    case DataOutputDepth_4Bit:
    {
      const u32* in_ptr = s_state.block_rgb.data();
      for (u32 i = 0; i < (64 / 8); i++)
      {
        u32 value = *(in_ptr++) >> 4;
        value |= (*(in_ptr++) >> 4) << 4;
        value |= (*(in_ptr++) >> 4) << 8;
        value |= (*(in_ptr++) >> 4) << 12;
        value |= (*(in_ptr++) >> 4) << 16;
        value |= (*(in_ptr++) >> 4) << 20;
        value |= (*(in_ptr++) >> 4) << 24;
        value |= (*(in_ptr++) >> 4) << 28;
        s_state.data_out_fifo.Push(value);
      }
    }
    break;

    case DataOutputDepth_8Bit:
    {
      const u32* in_ptr = s_state.block_rgb.data();
      for (u32 i = 0; i < (64 / 4); i++)
      {
        u32 value = *in_ptr++;
        value |= *in_ptr++ << 8;
        value |= *in_ptr++ << 16;
        value |= *in_ptr++ << 24;
        s_state.data_out_fifo.Push(value);
      }
    }
    break;

    case DataOutputDepth_24Bit:
    {
#ifndef CPU_ARCH_SIMD
      // pack tightly
      u32 index = 0;
      u32 state = 0;
      u32 rgb = 0;
      while (index < s_state.block_rgb.size())
      {
        switch (state)
        {
          case 0:
            rgb = s_state.block_rgb[index++]; // RGB-
            state = 1;
            break;
          case 1:
            rgb |= (s_state.block_rgb[index] & 0xFF) << 24; // RGBR
            s_state.data_out_fifo.Push(rgb);
            rgb = s_state.block_rgb[index] >> 8; // GB--
            index++;
            state = 2;
            break;
          case 2:
            rgb |= s_state.block_rgb[index] << 16; // GBRG
            s_state.data_out_fifo.Push(rgb);
            rgb = s_state.block_rgb[index] >> 16; // B---
            index++;
            state = 3;
            break;
          case 3:
            rgb |= s_state.block_rgb[index] << 8; // BRGB
            s_state.data_out_fifo.Push(rgb);
            index++;
            state = 0;
            break;
        }
      }
#else
      static constexpr GSVector4i mask00 = GSVector4i::cxpr8(0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
      static constexpr GSVector4i mask01 =
        GSVector4i::cxpr8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 4);
      static constexpr GSVector4i mask11 =
        GSVector4i::cxpr8(5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1, -1, -1, -1, -1);
      static constexpr GSVector4i mask12 = GSVector4i::cxpr8(-1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 4, 5, 6, 8, 9);
      static constexpr GSVector4i mask22 =
        GSVector4i::cxpr8(10, 12, 13, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
      static constexpr GSVector4i mask23 = GSVector4i::cxpr8(-1, -1, -1, -1, 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14);

      // This is really awful, but the FIFO sucks...
      alignas(VECTOR_ALIGNMENT) u32 rgb[256 * 3 / 4];
      u32* rgbp = rgb;

      for (u32 index = 0; index < s_state.block_rgb.size(); index += 16)
      {
        const GSVector4i rgbx0 = GSVector4i::load<false>(&s_state.block_rgb[index]);
        const GSVector4i rgbx1 = GSVector4i::load<false>(&s_state.block_rgb[index + 4]);
        const GSVector4i rgbx2 = GSVector4i::load<false>(&s_state.block_rgb[index + 8]);
        const GSVector4i rgbx3 = GSVector4i::load<false>(&s_state.block_rgb[index + 12]);

        GSVector4i::store<true>(&rgbp[0], rgbx0.shuffle8(mask00) | rgbx1.shuffle8(mask01));
        GSVector4i::store<true>(&rgbp[4], rgbx1.shuffle8(mask11) | rgbx2.shuffle8(mask12));
        GSVector4i::store<true>(&rgbp[8], rgbx2.shuffle8(mask22) | rgbx3.shuffle8(mask23));
        rgbp += 12;
      }

      s_state.data_out_fifo.PushRange(rgb, std::size(rgb));
#endif
      break;
    }

    case DataOutputDepth_15Bit:
    {
      if (g_settings.use_old_mdec_routines) [[unlikely]]
      {
        const u16 a = ZeroExtend16(s_state.status.data_output_bit15.GetValue()) << 15;
        for (u32 i = 0; i < static_cast<u32>(s_state.block_rgb.size());)
        {
          u32 color = s_state.block_rgb[i++];
          u16 r = Truncate16((color >> 3) & 0x1Fu);
          u16 g = Truncate16((color >> 11) & 0x1Fu);
          u16 b = Truncate16((color >> 19) & 0x1Fu);
          const u16 color15a = r | (g << 5) | (b << 10) | (a << 15);

          color = s_state.block_rgb[i++];
          r = Truncate16((color >> 3) & 0x1Fu);
          g = Truncate16((color >> 11) & 0x1Fu);
          b = Truncate16((color >> 19) & 0x1Fu);
          const u16 color15b = r | (g << 5) | (b << 10) | (a << 15);

          s_state.data_out_fifo.Push(ZeroExtend32(color15a) | (ZeroExtend32(color15b) << 16));
        }
      }
      else
      {
#ifndef CPU_ARCH_SIMD
        const u32 a = ZeroExtend32(s_state.status.data_output_bit15.GetValue()) << 15;
        for (u32 i = 0; i < static_cast<u32>(s_state.block_rgb.size());)
        {
#define E8TO5(color) (std::min<u32>((((color) + 4) >> 3), 0x1F))
          u32 color = s_state.block_rgb[i++];
          u32 r = E8TO5(color & 0xFFu);
          u32 g = E8TO5((color >> 8) & 0xFFu);
          u32 b = E8TO5((color >> 16) & 0xFFu);
          const u32 color15a = r | (g << 5) | (b << 10) | a;

          color = s_state.block_rgb[i++];
          r = E8TO5(color & 0xFFu);
          g = E8TO5((color >> 8) & 0xFFu);
          b = E8TO5((color >> 16) & 0xFFu);
          const u32 color15b = r | (g << 5) | (b << 10) | a;
#undef E8TO5

          s_state.data_out_fifo.Push(color15a | (color15b << 16));
        }
#else
        // This is really awful, but the FIFO sucks...
        alignas(VECTOR_ALIGNMENT) u32 rgb[256 / 2];
        u32* rgbp = rgb;

        const GSVector4i a = s_state.status.data_output_bit15 ? GSVector4i::cxpr(0x8000u) : GSVector4i::cxpr(0);
        for (u32 i = 0; i < static_cast<u32>(s_state.block_rgb.size()); i += 8)
        {
          GSVector4i rgb0 = GSVector4i::load<true>(&s_state.block_rgb[i]);
          GSVector4i rgb1 = GSVector4i::load<true>(&s_state.block_rgb[i + 4]);

          static constexpr auto rgb32_to_rgba5551 = [](const GSVector4i& rgb32, const GSVector4i& a) {
            const GSVector4i r =
              (rgb32 & GSVector4i::cxpr(0xff)).add32(GSVector4i::cxpr(4)).srl32(3).min_u32(GSVector4i::cxpr(0x1F));
            const GSVector4i g = (rgb32.srl32<8>() & GSVector4i::cxpr(0xff))
                                   .add32(GSVector4i::cxpr(4))
                                   .srl32(3)
                                   .min_u32(GSVector4i::cxpr(0x1F))
                                   .sll32<5>();
            const GSVector4i b = (rgb32.srl32<16>() & GSVector4i::cxpr(0xff))
                                   .add32(GSVector4i::cxpr(4))
                                   .srl32(3)
                                   .min_u32(GSVector4i::cxpr(0x1F))
                                   .sll32<10>();
            return (r | g | b | a);
          };

          rgb0 = rgb32_to_rgba5551(rgb0, a);
          rgb1 = rgb32_to_rgba5551(rgb1, a);

          const GSVector4i packed_rgb0_rb1 = rgb0.pu32(rgb1);
          GSVector4i::store<true>(rgbp, packed_rgb0_rb1);
          rgbp += sizeof(GSVector4i) / sizeof(u32);
        }

        s_state.data_out_fifo.PushRange(rgb, std::size(rgb));
#endif
      }
    }
    break;

    default:
      break;
  }

  DEBUG_LOG("Block copied out, fifo size = {} ({} bytes)", s_state.data_out_fifo.GetSize(),
            s_state.data_out_fifo.GetSize() * sizeof(u32));

  // if we've copied out all blocks, command is complete
  s_state.state = (s_state.remaining_halfwords == 0) ? State::Idle : State::DecodingMacroblock;
  Execute();
}

bool MDEC::DecodeRLE_Old(s16* blk, const u8* qt)
{
  static constexpr std::array<u8, 64> zagzig = {{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                                 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                                 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                                 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63}};

  if (s_state.current_coefficient == 64)
  {
    std::fill_n(blk, 64, s16(0));

    // skip padding at start
    u16 n;
    for (;;)
    {
      if (s_state.data_in_fifo.IsEmpty() || s_state.remaining_halfwords == 0)
        return false;

      n = s_state.data_in_fifo.Pop();
      s_state.remaining_halfwords--;

      if (n == 0xFE00)
        continue;
      else
        break;
    }

    s_state.current_coefficient = 0;
    s_state.current_q_scale = (n >> 10) & 0x3F;
    s32 val = SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) *
              static_cast<s32>(ZeroExtend32(qt[s_state.current_coefficient]));

    if (s_state.current_q_scale == 0)
      val = SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) * 2;

    val = std::clamp(val, -0x400, 0x3FF);
    if (s_state.current_q_scale > 0)
      blk[zagzig[s_state.current_coefficient]] = static_cast<s16>(val);
    else
      blk[s_state.current_coefficient] = static_cast<s16>(val);
  }

  while (!s_state.data_in_fifo.IsEmpty() && s_state.remaining_halfwords > 0)
  {
    u16 n = s_state.data_in_fifo.Pop();
    s_state.remaining_halfwords--;

    s_state.current_coefficient += ((n >> 10) & 0x3F) + 1;
    if (s_state.current_coefficient < 64)
    {
      s32 val =
        (SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) *
           static_cast<s32>(ZeroExtend32(qt[s_state.current_coefficient])) * static_cast<s32>(s_state.current_q_scale) +
         4) /
        8;

      if (s_state.current_q_scale == 0)
        val = SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) * 2;

      val = std::clamp(val, -0x400, 0x3FF);
      if (s_state.current_q_scale > 0)
        blk[zagzig[s_state.current_coefficient]] = static_cast<s16>(val);
      else
        blk[s_state.current_coefficient] = static_cast<s16>(val);
    }

    if (s_state.current_coefficient >= 63)
    {
      s_state.current_coefficient = 64;
      return true;
    }
  }

  return false;
}

void MDEC::IDCT_Old(s16* blk)
{
  std::array<s64, 64> temp_buffer;
  for (u32 x = 0; x < 8; x++)
  {
    for (u32 y = 0; y < 8; y++)
    {
      s64 sum = 0;
      for (u32 u = 0; u < 8; u++)
        sum += s32(blk[u * 8 + x]) * s32(s_state.scale_table[y * 8 + u]);
      temp_buffer[x + y * 8] = sum;
    }
  }
  for (u32 x = 0; x < 8; x++)
  {
    for (u32 y = 0; y < 8; y++)
    {
      s64 sum = 0;
      for (u32 u = 0; u < 8; u++)
        sum += s64(temp_buffer[u + y * 8]) * s32(s_state.scale_table[x * 8 + u]);

      blk[x + y * 8] =
        static_cast<s16>(std::clamp<s32>(SignExtendN<9, s32>((sum >> 32) + ((sum >> 31) & 1)), -128, 127));
    }
  }
}

void MDEC::YUVToRGB_Old(u32 xx, u32 yy, const std::array<s16, 64>& Crblk, const std::array<s16, 64>& Cbblk,
                        const std::array<s16, 64>& Yblk)
{
  const s16 addval = s_state.status.data_output_signed ? 0 : 0x80;
  for (u32 y = 0; y < 8; y++)
  {
    for (u32 x = 0; x < 8; x++)
    {
      s16 R = Crblk[((x + xx) / 2) + ((y + yy) / 2) * 8];
      s16 B = Cbblk[((x + xx) / 2) + ((y + yy) / 2) * 8];
      s16 G = static_cast<s16>((-0.3437f * static_cast<float>(B)) + (-0.7143f * static_cast<float>(R)));

      R = static_cast<s16>(1.402f * static_cast<float>(R));
      B = static_cast<s16>(1.772f * static_cast<float>(B));

      s16 Y = Yblk[x + y * 8];
      R = static_cast<s16>(std::clamp(static_cast<int>(Y) + R, -128, 127)) + addval;
      G = static_cast<s16>(std::clamp(static_cast<int>(Y) + G, -128, 127)) + addval;
      B = static_cast<s16>(std::clamp(static_cast<int>(Y) + B, -128, 127)) + addval;

      s_state.block_rgb[(x + xx) + ((y + yy) * 16)] = ZeroExtend32(static_cast<u16>(R)) |
                                                      (ZeroExtend32(static_cast<u16>(G)) << 8) |
                                                      (ZeroExtend32(static_cast<u16>(B)) << 16);
    }
  }
}

bool MDEC::DecodeRLE_New(s16* blk, const u8* qt)
{
  // Swapped to row-major so we can vectorize the IDCT.
  static constexpr std::array<u8, 64> zigzag = {{0,  8,  1,  2,  9,  16, 24, 17, 10, 3,  4,  11, 18, 25, 32, 40,
                                                 33, 26, 19, 12, 5,  6,  13, 20, 27, 34, 41, 48, 56, 49, 42, 35,
                                                 28, 21, 14, 7,  15, 22, 29, 36, 43, 50, 57, 58, 51, 44, 37, 30,
                                                 23, 31, 38, 45, 52, 59, 60, 53, 46, 39, 47, 54, 61, 62, 55, 63}};

  if (s_state.current_coefficient == 64)
  {
    std::fill_n(blk, 64, s16(0));

    // skip padding at start
    u16 n;
    for (;;)
    {
      if (s_state.data_in_fifo.IsEmpty() || s_state.remaining_halfwords == 0)
        return false;

      n = s_state.data_in_fifo.Pop();
      s_state.remaining_halfwords--;

      if (n == 0xFE00)
        continue;
      else
        break;
    }

    s_state.current_coefficient = 0;
    s_state.current_q_scale = n >> 10;

    // Store the DCT blocks with an additional 4 bits of precision.
    const s32 val = SignExtendN<10, s32>(static_cast<s32>(n));
    const s32 coeff =
      (s_state.current_q_scale == 0) ? (val << 5) : (((val * qt[0]) << 4) + (val ? ((val < 0) ? 8 : -8) : 0));
    blk[zigzag[0]] = static_cast<s16>(std::clamp(coeff, -0x4000, 0x3FFF));
  }

  while (!s_state.data_in_fifo.IsEmpty() && s_state.remaining_halfwords > 0)
  {
    u16 n = s_state.data_in_fifo.Pop();
    s_state.remaining_halfwords--;

    s_state.current_coefficient += ((n >> 10) + 1);
    if (s_state.current_coefficient < 64)
    {
      const s32 val = SignExtendN<10, s32>(n);
      const s32 scq = static_cast<s32>(s_state.current_q_scale * qt[s_state.current_coefficient]);
      const s32 coeff = (scq == 0) ? (val << 5) : ((((val * scq) >> 3) << 4) + (val ? ((val < 0) ? 8 : -8) : 0));
      blk[zigzag[s_state.current_coefficient]] = static_cast<s16>(std::clamp(coeff, -0x4000, 0x3FFF));
    }

    if (s_state.current_coefficient >= 63)
    {
      s_state.current_coefficient = 64;
      return true;
    }
  }

  return false;
}

static s16 IDCTRow(const s16* blk, const s16* idct_matrix)
{
  // IDCT matrix is -32768..32767, block is -16384..16383. 4 adds can happen without overflow.
  GSVector4i sum = GSVector4i::load<false>(blk).madd_s16(GSVector4i::load<true>(idct_matrix)).addp_s32();
  return static_cast<s16>(((static_cast<s64>(sum.extract32<0>()) + static_cast<s64>(sum.extract32<1>())) + 0x20000) >>
                          18);
}

void MDEC::IDCT_New(s16* blk)
{
  alignas(VECTOR_ALIGNMENT) std::array<s16, 64> temp;
  for (u32 x = 0; x < 8; x++)
  {
    for (u32 y = 0; y < 8; y++)
      temp[y * 8 + x] = IDCTRow(&blk[x * 8], &s_state.scale_table[y * 8]);
  }
  for (u32 x = 0; x < 8; x++)
  {
    for (u32 y = 0; y < 8; y++)
    {
      const s32 sum = IDCTRow(&temp[x * 8], &s_state.scale_table[y * 8]);
      blk[x * 8 + y] = static_cast<s16>(std::clamp(SignExtendN<9, s32>(sum), -128, 127));
    }
  }
}

void MDEC::YUVToRGB_New(u32 xx, u32 yy, const std::array<s16, 64>& Crblk, const std::array<s16, 64>& Cbblk,
                        const std::array<s16, 64>& Yblk)
{
  const GSVector4i addval = s_state.status.data_output_signed ? GSVector4i::cxpr(0) : GSVector4i::cxpr(0x80808080);
  for (u32 y = 0; y < 8; y++)
  {
    const GSVector4i Cr = GSVector4i::loadl(&Crblk[(xx / 2) + ((y + yy) / 2) * 8]).s16to32();
    const GSVector4i Cb = GSVector4i::loadl(&Cbblk[(xx / 2) + ((y + yy) / 2) * 8]).s16to32();
    const GSVector4i Y = GSVector4i::load<true>(&Yblk[y * 8]);

    // BT.601 YUV->RGB coefficients, rounding formula from Mednafen.
    // r = clamp(sext9(Y + (((359 * Cr) + 0x80) >> 8)), -128, 127) + addval;
    // g = clamp(sext9(Y + ((((-88 * Cb) & ~0x1F) + ((-183 * Cr) & ~0x07) + 0x80) >> 8)), -128, 127) + addval
    // b = clamp(sext9<9, s32>(Y + (((454 * Cb) + 0x80) >> 8)), -128, 127) + addval

    // Need to do the multiply as 32-bit, since 127 * 359 is greater than INT16_MAX.
    // upl16(self) = interleave XYZW0000 -> XXYYZZWW.
    const GSVector4i Crmul = Cr.mul32l(GSVector4i::cxpr(359)).add16(GSVector4i::cxpr(0x80)).sra32<8>().ps32();
    const GSVector4i Cbmul = Cb.mul32l(GSVector4i::cxpr(454)).add16(GSVector4i::cxpr(0x80)).sra32<8>().ps32();
    const GSVector4i CrCbmul = (Cb.mul32l(GSVector4i::cxpr(-88)) & GSVector4i::cxpr(~0x1F))
                                 .add32(Cr.mul32l(GSVector4i::cxpr(-183)) & GSVector4i::cxpr(~0x07))
                                 .add32(GSVector4i::cxpr(0x80))
                                 .sra32<8>()
                                 .ps32();
    const GSVector4i r = Crmul.upl16(Crmul).add16(Y).sll16<7>().sra16<7>().ps16().add8(addval);
    const GSVector4i g = CrCbmul.upl16(CrCbmul).add16(Y).sll16<7>().sra16<7>().ps16().add8(addval);
    const GSVector4i b = Cbmul.upl16(Cbmul).add16(Y).sll16<7>().sra16<7>().ps16().add8(addval);
    const GSVector4i rg = r.upl8(g);
    const GSVector4i b0 = b.upl8();
    const GSVector4i rgblow = rg.upl16(b0);
    const GSVector4i rgbhigh = rg.uph16(b0);

    u32* const out_row = &s_state.block_rgb[xx + ((y + yy) * 16)];
    GSVector4i::store<false>(&out_row[0], rgblow);
    GSVector4i::store<false>(&out_row[4], rgbhigh);
  }
}

void MDEC::YUVToMono(const std::array<s16, 64>& Yblk)
{
  const s32 addval = s_state.status.data_output_signed ? 0 : 0x80;
  for (u32 i = 0; i < 64; i++)
    s_state.block_rgb[i] = static_cast<u32>(std::clamp(SignExtendN<9, s32>(Yblk[i]), -128, 127) + addval);
}

void MDEC::HandleSetQuantTableCommand()
{
  DebugAssert(s_state.remaining_halfwords >= 32);

  // TODO: Remove extra copies..
  std::array<u16, 32> packed_data;
  s_state.data_in_fifo.PopRange(packed_data.data(), static_cast<u32>(packed_data.size()));
  s_state.remaining_halfwords -= 32;
  std::memcpy(s_state.iq_y.data(), packed_data.data(), s_state.iq_y.size());

  if (s_state.remaining_halfwords > 0)
  {
    DebugAssert(s_state.remaining_halfwords >= 32);

    s_state.data_in_fifo.PopRange(packed_data.data(), static_cast<u32>(packed_data.size()));
    std::memcpy(s_state.iq_uv.data(), packed_data.data(), s_state.iq_uv.size());
  }
}

void MDEC::HandleSetScaleCommand()
{
  DebugAssert(s_state.remaining_halfwords == 64);

  std::array<u16, 64> packed_data;
  s_state.data_in_fifo.PopRange(packed_data.data(), static_cast<u32>(packed_data.size()));
  s_state.remaining_halfwords -= 32;
  SetScaleMatrix(packed_data.data());
}

void MDEC::SetScaleMatrix(const u16* values)
{
  for (u32 y = 0; y < 8; y++)
  {
    for (u32 x = 0; x < 8; x++)
      s_state.scale_table[y * 8 + x] = values[x * 8 + y];
  }
}

void MDEC::DrawDebugStateWindow()
{
  const float framebuffer_scale = ImGuiManager::GetGlobalScale();

  ImGui::SetNextWindowSize(ImVec2(300.0f * framebuffer_scale, 350.0f * framebuffer_scale), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("MDEC State", nullptr))
  {
    ImGui::End();
    return;
  }

  static constexpr std::array<const char*, 5> state_names = {
    {"None", "Decoding Macroblock", "Writing Macroblock", "SetIqTab", "SetScale"}};
  static constexpr std::array<const char*, 4> output_depths = {{"4-bit", "8-bit", "24-bit", "15-bit"}};
  static constexpr std::array<const char*, 7> block_names = {{"Crblk", "Cbblk", "Y1", "Y2", "Y3", "Y4", "Output"}};

  ImGui::Text("Blocks Decoded: %u", s_state.total_blocks_decoded);
  ImGui::Text("Data-In FIFO Size: %u (%u bytes)", s_state.data_in_fifo.GetSize(), s_state.data_in_fifo.GetSize() * 4);
  ImGui::Text("Data-Out FIFO Size: %u (%u bytes)", s_state.data_out_fifo.GetSize(),
              s_state.data_out_fifo.GetSize() * 4);
  ImGui::Text("DMA Enable: %s%s", s_state.enable_dma_in ? "In " : "", s_state.enable_dma_out ? "Out" : "");
  ImGui::Text("Current State: %s", state_names[static_cast<u8>(s_state.state)]);
  ImGui::Text("Current Block: %s", block_names[s_state.current_block]);
  ImGui::Text("Current Coefficient: %u", s_state.current_coefficient);

  if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Text("Data-Out FIFO Empty: %s", s_state.status.data_out_fifo_empty ? "Yes" : "No");
    ImGui::Text("Data-In FIFO Full: %s", s_state.status.data_in_fifo_full ? "Yes" : "No");
    ImGui::Text("Command Busy: %s", s_state.status.command_busy ? "Yes" : "No");
    ImGui::Text("Data-In Request: %s", s_state.status.data_in_request ? "Yes" : "No");
    ImGui::Text("Output Depth: %s", output_depths[static_cast<u8>(s_state.status.data_output_depth.GetValue())]);
    ImGui::Text("Output Signed: %s", s_state.status.data_output_signed ? "Yes" : "No");
    ImGui::Text("Output Bit 15: %u", ZeroExtend32(s_state.status.data_output_bit15.GetValue()));
    ImGui::Text("Current Block: %u", ZeroExtend32(s_state.status.current_block.GetValue()));
    ImGui::Text("Parameter Words Remaining: %d",
                static_cast<s32>(SignExtend32(s_state.status.parameter_words_remaining.GetValue())));
  }

  ImGui::End();
}
