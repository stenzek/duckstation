// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "mdec.h"
#include "cpu_core.h"
#include "dma.h"
#include "host.h"
#include "interrupt_controller.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/state_wrapper.h"

#include "common/bitfield.h"
#include "common/fifo_queue.h"
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

static bool DecodeMonoMacroblock();
static bool DecodeColoredMacroblock();
static void ScheduleBlockCopyOut(TickCount ticks);
static void CopyOutBlock(void* param, TickCount ticks, TickCount ticks_late);

// from nocash spec
static bool rl_decode_block(s16* blk, const u8* qt);
static void IDCT(s16* blk);
static void IDCT_New(s16* blk);
static void IDCT_Old(s16* blk);
static void yuv_to_rgb(u32 xx, u32 yy, const std::array<s16, 64>& Crblk, const std::array<s16, 64>& Cbblk,
                       const std::array<s16, 64>& Yblk);
static void y_to_mono(const std::array<s16, 64>& Yblk);

static StatusRegister s_status = {};
static bool s_enable_dma_in = false;
static bool s_enable_dma_out = false;

// Even though the DMA is in words, we access the FIFO as halfwords.
static InlineFIFOQueue<u16, DATA_IN_FIFO_SIZE / sizeof(u16)> s_data_in_fifo;
static InlineFIFOQueue<u32, DATA_OUT_FIFO_SIZE / sizeof(u32)> s_data_out_fifo;
static State s_state = State::Idle;
static u32 s_remaining_halfwords = 0;

static std::array<u8, 64> s_iq_uv{};
static std::array<u8, 64> s_iq_y{};

static std::array<s16, 64> s_scale_table{};

// blocks, for colour: 0 - Crblk, 1 - Cbblk, 2-5 - Y 1-4
static std::array<std::array<s16, 64>, NUM_BLOCKS> s_blocks;
static u32 s_current_block = 0;        // block (0-5)
static u32 s_current_coefficient = 64; // k (in block)
static u16 s_current_q_scale = 0;

alignas(16) static std::array<u32, 256> s_block_rgb{};
static std::unique_ptr<TimingEvent> s_block_copy_out_event;

static u32 s_total_blocks_decoded = 0;
} // namespace MDEC

void MDEC::Initialize()
{
  s_block_copy_out_event =
    TimingEvents::CreateTimingEvent("MDEC Block Copy Out", 1, 1, &MDEC::CopyOutBlock, nullptr, false);
  s_total_blocks_decoded = 0;
  Reset();
}

void MDEC::Shutdown()
{
  s_block_copy_out_event.reset();
}

void MDEC::Reset()
{
  s_block_copy_out_event->Deactivate();
  SoftReset();
}

bool MDEC::DoState(StateWrapper& sw)
{
  sw.Do(&s_status.bits);
  sw.Do(&s_enable_dma_in);
  sw.Do(&s_enable_dma_out);
  sw.Do(&s_data_in_fifo);
  sw.Do(&s_data_out_fifo);
  sw.Do(&s_state);
  sw.Do(&s_remaining_halfwords);
  sw.Do(&s_iq_uv);
  sw.Do(&s_iq_y);
  sw.Do(&s_scale_table);
  sw.Do(&s_blocks);
  sw.Do(&s_current_block);
  sw.Do(&s_current_coefficient);
  sw.Do(&s_current_q_scale);
  sw.Do(&s_block_rgb);

  bool block_copy_out_pending = HasPendingBlockCopyOut();
  sw.Do(&block_copy_out_pending);
  if (sw.IsReading())
    s_block_copy_out_event->SetState(block_copy_out_pending);

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
      Log_TracePrintf("MDEC status register -> 0x%08X", s_status.bits);
      return s_status.bits;
    }

    default:
    {
      Log_ErrorPrintf("Unknown MDEC register read: 0x%08X", offset);
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
      Log_DebugPrintf("MDEC control register <- 0x%08X", value);

      const ControlRegister cr{value};
      if (cr.reset)
        SoftReset();

      s_enable_dma_in = cr.enable_dma_in;
      s_enable_dma_out = cr.enable_dma_out;
      Execute();
      return;
    }

    default:
    {
      Log_ErrorPrintf("Unknown MDEC register write: 0x%08X <- 0x%08X", offset, value);
      return;
    }
  }
}

void MDEC::DMARead(u32* words, u32 word_count)
{
  if (s_data_out_fifo.GetSize() < word_count) [[unlikely]]
  {
    Log_WarningPrintf("Insufficient data in output FIFO (requested %u, have %u)", word_count,
                      s_data_out_fifo.GetSize());
  }

  const u32 words_to_read = std::min(word_count, s_data_out_fifo.GetSize());
  if (words_to_read > 0)
  {
    s_data_out_fifo.PopRange(words, words_to_read);
    words += words_to_read;
    word_count -= words_to_read;
  }

  Log_DebugPrintf("DMA read complete, %u bytes left", static_cast<u32>(s_data_out_fifo.GetSize() * sizeof(u32)));
  if (s_data_out_fifo.IsEmpty())
    Execute();
}

void MDEC::DMAWrite(const u32* words, u32 word_count)
{
  if (s_data_in_fifo.GetSpace() < (word_count * 2)) [[unlikely]]
  {
    Log_WarningPrintf("Input FIFO overflow (writing %u, space %u)", word_count * 2, s_data_in_fifo.GetSpace());
  }

  const u32 halfwords_to_write = std::min(word_count * 2, s_data_in_fifo.GetSpace() & ~u32(2));
  s_data_in_fifo.PushRange(reinterpret_cast<const u16*>(words), halfwords_to_write);
  Execute();
}

bool MDEC::HasPendingBlockCopyOut()
{
  return s_block_copy_out_event->IsActive();
}

void MDEC::SoftReset()
{
  s_status.bits = 0;
  s_enable_dma_in = false;
  s_enable_dma_out = false;
  s_data_in_fifo.Clear();
  s_data_out_fifo.Clear();
  s_state = State::Idle;
  s_remaining_halfwords = 0;
  s_current_block = 0;
  s_current_coefficient = 64;
  s_current_q_scale = 0;
  s_block_copy_out_event->Deactivate();
  UpdateStatus();
}

void MDEC::ResetDecoder()
{
  s_current_block = 0;
  s_current_coefficient = 64;
  s_current_q_scale = 0;
}

void MDEC::UpdateStatus()
{
  s_status.data_out_fifo_empty = s_data_out_fifo.IsEmpty();
  s_status.data_in_fifo_full = s_data_in_fifo.IsFull();

  s_status.command_busy = (s_state != State::Idle);
  s_status.parameter_words_remaining = Truncate16((s_remaining_halfwords / 2) - 1);
  s_status.current_block = (s_current_block + 4) % NUM_BLOCKS;

  // we always want data in if it's enabled
  const bool data_in_request = s_enable_dma_in && s_data_in_fifo.GetSpace() >= (32 * 2);
  s_status.data_in_request = data_in_request;
  DMA::SetRequest(DMA::Channel::MDECin, data_in_request);

  // we only want to send data out if we have some in the fifo
  const bool data_out_request = s_enable_dma_out && !s_data_out_fifo.IsEmpty();
  s_status.data_out_request = data_out_request;
  DMA::SetRequest(DMA::Channel::MDECout, data_out_request);
}

u32 MDEC::ReadDataRegister()
{
  if (s_data_out_fifo.IsEmpty())
  {
    // Stall the CPU until we're done processing.
    if (HasPendingBlockCopyOut())
    {
      Log_DevPrint("MDEC data out FIFO empty on read - stalling CPU");
      CPU::AddPendingTicks(s_block_copy_out_event->GetTicksUntilNextExecution());
    }
    else
    {
      Log_WarningPrintf("MDEC data out FIFO empty on read and no data processing");
      return UINT32_C(0xFFFFFFFF);
    }
  }

  const u32 value = s_data_out_fifo.Pop();
  if (s_data_out_fifo.IsEmpty())
    Execute();
  else
    UpdateStatus();

  return value;
}

void MDEC::WriteCommandRegister(u32 value)
{
  Log_TracePrintf("MDEC command/data register <- 0x%08X", value);

  s_data_in_fifo.Push(Truncate16(value));
  s_data_in_fifo.Push(Truncate16(value >> 16));

  Execute();
}

void MDEC::Execute()
{
  for (;;)
  {
    switch (s_state)
    {
      case State::Idle:
      {
        if (s_data_in_fifo.GetSize() < 2)
          goto finished;

        // first word
        const CommandWord cw{ZeroExtend32(s_data_in_fifo.Peek(0)) | (ZeroExtend32(s_data_in_fifo.Peek(1)) << 16)};
        s_status.data_output_depth = cw.data_output_depth;
        s_status.data_output_signed = cw.data_output_signed;
        s_status.data_output_bit15 = cw.data_output_bit15;
        s_data_in_fifo.Remove(2);
        s_data_out_fifo.Clear();

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
            Log_DevPrintf("Invalid MDEC command 0x%08X", cw.bits);
            num_words = cw.parameter_word_count.GetValue();
            new_state = State::NoCommand;
            break;
        }

        Log_DebugPrintf("MDEC command: 0x%08X (%u, %u words in parameter, %u expected)", cw.bits,
                        ZeroExtend32(static_cast<u8>(cw.command.GetValue())),
                        ZeroExtend32(cw.parameter_word_count.GetValue()), num_words);

        s_remaining_halfwords = num_words * 2;
        s_state = new_state;
        UpdateStatus();
        continue;
      }

      case State::DecodingMacroblock:
      {
        if (HandleDecodeMacroblockCommand())
        {
          // we should be writing out now
          DebugAssert(s_state == State::WritingMacroblock);
          goto finished;
        }

        if (s_remaining_halfwords == 0 && s_current_block != NUM_BLOCKS)
        {
          // expecting data, but nothing more will be coming. bail out
          ResetDecoder();
          s_state = State::Idle;
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
        if (s_data_in_fifo.GetSize() < s_remaining_halfwords)
          goto finished;

        HandleSetQuantTableCommand();
        s_state = State::Idle;
        UpdateStatus();
        continue;
      }

      case State::SetScaleTable:
      {
        if (s_data_in_fifo.GetSize() < s_remaining_halfwords)
          goto finished;

        HandleSetScaleCommand();
        s_state = State::Idle;
        UpdateStatus();
        continue;
      }

      case State::NoCommand:
      {
        // can potentially have a large amount of halfwords, so eat them as we go
        const u32 words_to_consume = std::min(s_remaining_halfwords, s_data_in_fifo.GetSize());
        s_data_in_fifo.Remove(words_to_consume);
        s_remaining_halfwords -= words_to_consume;
        if (s_remaining_halfwords == 0)
          goto finished;

        s_state = State::Idle;
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
  if (s_status.data_output_depth <= DataOutputDepth_8Bit)
    return DecodeMonoMacroblock();
  else
    return DecodeColoredMacroblock();
}

bool MDEC::DecodeMonoMacroblock()
{
  // TODO: This should guard the output not the input
  if (!s_data_out_fifo.IsEmpty())
    return false;

  if (!rl_decode_block(s_blocks[0].data(), s_iq_y.data()))
    return false;

  IDCT(s_blocks[0].data());

  Log_DebugPrintf("Decoded mono macroblock, %u words remaining", s_remaining_halfwords / 2);
  ResetDecoder();
  s_state = State::WritingMacroblock;

  y_to_mono(s_blocks[0]);

  ScheduleBlockCopyOut(TICKS_PER_BLOCK * 6);

  s_total_blocks_decoded++;
  return true;
}

bool MDEC::DecodeColoredMacroblock()
{
  for (; s_current_block < NUM_BLOCKS; s_current_block++)
  {
    if (!rl_decode_block(s_blocks[s_current_block].data(), (s_current_block >= 2) ? s_iq_y.data() : s_iq_uv.data()))
      return false;

    IDCT(s_blocks[s_current_block].data());
  }

  if (!s_data_out_fifo.IsEmpty())
    return false;

  // done decoding
  Log_DebugPrintf("Decoded colored macroblock, %u words remaining", s_remaining_halfwords / 2);
  ResetDecoder();
  s_state = State::WritingMacroblock;

  yuv_to_rgb(0, 0, s_blocks[0], s_blocks[1], s_blocks[2]);
  yuv_to_rgb(8, 0, s_blocks[0], s_blocks[1], s_blocks[3]);
  yuv_to_rgb(0, 8, s_blocks[0], s_blocks[1], s_blocks[4]);
  yuv_to_rgb(8, 8, s_blocks[0], s_blocks[1], s_blocks[5]);
  s_total_blocks_decoded += 4;

  ScheduleBlockCopyOut(TICKS_PER_BLOCK * 6);
  return true;
}

void MDEC::ScheduleBlockCopyOut(TickCount ticks)
{
  DebugAssert(!HasPendingBlockCopyOut());
  Log_DebugPrintf("Scheduling block copy out in %d ticks", ticks);

  s_block_copy_out_event->SetIntervalAndSchedule(ticks);
}

void MDEC::CopyOutBlock(void* param, TickCount ticks, TickCount ticks_late)
{
  Assert(s_state == State::WritingMacroblock);
  s_block_copy_out_event->Deactivate();

  switch (s_status.data_output_depth)
  {
    case DataOutputDepth_4Bit:
    {
      const u32* in_ptr = s_block_rgb.data();
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
        s_data_out_fifo.Push(value);
      }
    }
    break;

    case DataOutputDepth_8Bit:
    {
      const u32* in_ptr = s_block_rgb.data();
      for (u32 i = 0; i < (64 / 4); i++)
      {
        u32 value = *in_ptr++;
        value |= *in_ptr++ << 8;
        value |= *in_ptr++ << 16;
        value |= *in_ptr++ << 24;
        s_data_out_fifo.Push(value);
      }
    }
    break;

    case DataOutputDepth_24Bit:
    {
      // pack tightly
      u32 index = 0;
      u32 state = 0;
      u32 rgb = 0;
      while (index < s_block_rgb.size())
      {
        switch (state)
        {
          case 0:
            rgb = s_block_rgb[index++]; // RGB-
            state = 1;
            break;
          case 1:
            rgb |= (s_block_rgb[index] & 0xFF) << 24; // RGBR
            s_data_out_fifo.Push(rgb);
            rgb = s_block_rgb[index] >> 8; // GB--
            index++;
            state = 2;
            break;
          case 2:
            rgb |= s_block_rgb[index] << 16; // GBRG
            s_data_out_fifo.Push(rgb);
            rgb = s_block_rgb[index] >> 16; // B---
            index++;
            state = 3;
            break;
          case 3:
            rgb |= s_block_rgb[index] << 8; // BRGB
            s_data_out_fifo.Push(rgb);
            index++;
            state = 0;
            break;
        }
      }
      break;
    }

    case DataOutputDepth_15Bit:
    {
      if (g_settings.use_old_mdec_routines) [[unlikely]]
      {
        const u16 a = ZeroExtend16(s_status.data_output_bit15.GetValue()) << 15;
        for (u32 i = 0; i < static_cast<u32>(s_block_rgb.size());)
        {
          u32 color = s_block_rgb[i++];
          u16 r = Truncate16((color >> 3) & 0x1Fu);
          u16 g = Truncate16((color >> 11) & 0x1Fu);
          u16 b = Truncate16((color >> 19) & 0x1Fu);
          const u16 color15a = r | (g << 5) | (b << 10) | (a << 15);

          color = s_block_rgb[i++];
          r = Truncate16((color >> 3) & 0x1Fu);
          g = Truncate16((color >> 11) & 0x1Fu);
          b = Truncate16((color >> 19) & 0x1Fu);
          const u16 color15b = r | (g << 5) | (b << 10) | (a << 15);

          s_data_out_fifo.Push(ZeroExtend32(color15a) | (ZeroExtend32(color15b) << 16));
        }
      }
      else
      {
        const u32 a = ZeroExtend32(s_status.data_output_bit15.GetValue()) << 15;
        for (u32 i = 0; i < static_cast<u32>(s_block_rgb.size());)
        {
#define E8TO5(color) (std::min<u32>((((color) + 4) >> 3), 0x1F))
          u32 color = s_block_rgb[i++];
          u32 r = E8TO5(color & 0xFFu);
          u32 g = E8TO5((color >> 8) & 0xFFu);
          u32 b = E8TO5((color >> 16) & 0xFFu);
          const u32 color15a = r | (g << 5) | (b << 10) | a;

          color = s_block_rgb[i++];
          r = E8TO5(color & 0xFFu);
          g = E8TO5((color >> 8) & 0xFFu);
          b = E8TO5((color >> 16) & 0xFFu);
          const u32 color15b = r | (g << 5) | (b << 10) | a;
#undef E8TO5

          s_data_out_fifo.Push(color15a | (color15b << 16));
        }
      }
    }
    break;

    default:
      break;
  }

  Log_DebugPrintf("Block copied out, fifo size = %u (%u bytes)", s_data_out_fifo.GetSize(),
                  static_cast<u32>(s_data_out_fifo.GetSize() * sizeof(u32)));

  // if we've copied out all blocks, command is complete
  s_state = (s_remaining_halfwords == 0) ? State::Idle : State::DecodingMacroblock;
  Execute();
}

static constexpr std::array<u8, 64> zagzig = {{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                               12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                               35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                               58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63}};

bool MDEC::rl_decode_block(s16* blk, const u8* qt)
{
  if (s_current_coefficient == 64)
  {
    std::fill_n(blk, 64, s16(0));

    // skip padding at start
    u16 n;
    for (;;)
    {
      if (s_data_in_fifo.IsEmpty() || s_remaining_halfwords == 0)
        return false;

      n = s_data_in_fifo.Pop();
      s_remaining_halfwords--;

      if (n == 0xFE00)
        continue;
      else
        break;
    }

    s_current_coefficient = 0;
    s_current_q_scale = (n >> 10) & 0x3F;
    s32 val =
      SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) * static_cast<s32>(ZeroExtend32(qt[s_current_coefficient]));

    if (s_current_q_scale == 0)
      val = SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) * 2;

    val = std::clamp(val, -0x400, 0x3FF);
    if (s_current_q_scale > 0)
      blk[zagzig[s_current_coefficient]] = static_cast<s16>(val);
    else
      blk[s_current_coefficient] = static_cast<s16>(val);
  }

  while (!s_data_in_fifo.IsEmpty() && s_remaining_halfwords > 0)
  {
    u16 n = s_data_in_fifo.Pop();
    s_remaining_halfwords--;

    s_current_coefficient += ((n >> 10) & 0x3F) + 1;
    if (s_current_coefficient < 64)
    {
      s32 val = (SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) *
                   static_cast<s32>(ZeroExtend32(qt[s_current_coefficient])) * static_cast<s32>(s_current_q_scale) +
                 4) /
                8;

      if (s_current_q_scale == 0)
        val = SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) * 2;

      val = std::clamp(val, -0x400, 0x3FF);
      if (s_current_q_scale > 0)
        blk[zagzig[s_current_coefficient]] = static_cast<s16>(val);
      else
        blk[s_current_coefficient] = static_cast<s16>(val);
    }

    if (s_current_coefficient >= 63)
    {
      s_current_coefficient = 64;
      return true;
    }
  }

  return false;
}

void MDEC::IDCT(s16* blk)
{
  // people have made texture packs using the old conversion routines.. best to just leave them be.
  if (g_settings.use_old_mdec_routines) [[unlikely]]
    IDCT_Old(blk);
  else
    IDCT_New(blk);
}

void MDEC::IDCT_New(s16* blk)
{
  std::array<s32, 64> temp;
  for (u32 x = 0; x < 8; x++)
  {
    for (u32 y = 0; y < 8; y++)
    {
      // TODO: We could alter zigzag and invert scale_table to get these in row-major order,
      // in which case we could do optimize this to a vector multiply.
      s32 sum = 0;
      for (u32 z = 0; z < 8; z++)
        sum += s32(blk[y + z * 8]) * s32(s_scale_table[x + z * 8] / 8);
      temp[x + y * 8] = static_cast<s32>((sum + 0xfff) / 0x2000);
    }
  }
  for (u32 x = 0; x < 8; x++)
  {
    for (u32 y = 0; y < 8; y++)
    {
      s32 sum = 0;
      for (u32 z = 0; z < 8; z++)
        sum += temp[y + z * 8] * s32(s_scale_table[x + z * 8] / 8);
      blk[x + y * 8] = static_cast<s16>(std::clamp<s32>((sum + 0xfff) / 0x2000, -128, 127));
    }
  }
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
        sum += s32(blk[u * 8 + x]) * s32(s_scale_table[u * 8 + y]);
      temp_buffer[x + y * 8] = sum;
    }
  }
  for (u32 x = 0; x < 8; x++)
  {
    for (u32 y = 0; y < 8; y++)
    {
      s64 sum = 0;
      for (u32 u = 0; u < 8; u++)
        sum += s64(temp_buffer[u + y * 8]) * s32(s_scale_table[u * 8 + x]);

      blk[x + y * 8] =
        static_cast<s16>(std::clamp<s32>(SignExtendN<9, s32>((sum >> 32) + ((sum >> 31) & 1)), -128, 127));
    }
  }
}

void MDEC::yuv_to_rgb(u32 xx, u32 yy, const std::array<s16, 64>& Crblk, const std::array<s16, 64>& Cbblk,
                      const std::array<s16, 64>& Yblk)
{
  const s16 addval = s_status.data_output_signed ? 0 : 0x80;
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

      s_block_rgb[(x + xx) + ((y + yy) * 16)] = ZeroExtend32(static_cast<u16>(R)) |
                                                (ZeroExtend32(static_cast<u16>(G)) << 8) |
                                                (ZeroExtend32(static_cast<u16>(B)) << 16);
    }
  }
}

void MDEC::y_to_mono(const std::array<s16, 64>& Yblk)
{
  for (u32 i = 0; i < 64; i++)
  {
    s16 Y = Yblk[i];
    Y = SignExtendN<10, s16>(Y);
    Y = std::clamp<s16>(Y, -128, 127);
    Y += 128;
    s_block_rgb[i] = static_cast<u32>(Y) & 0xFF;
  }
}

void MDEC::HandleSetQuantTableCommand()
{
  DebugAssert(s_remaining_halfwords >= 32);

  // TODO: Remove extra copies..
  std::array<u16, 32> packed_data;
  s_data_in_fifo.PopRange(packed_data.data(), static_cast<u32>(packed_data.size()));
  s_remaining_halfwords -= 32;
  std::memcpy(s_iq_y.data(), packed_data.data(), s_iq_y.size());

  if (s_remaining_halfwords > 0)
  {
    DebugAssert(s_remaining_halfwords >= 32);

    s_data_in_fifo.PopRange(packed_data.data(), static_cast<u32>(packed_data.size()));
    std::memcpy(s_iq_uv.data(), packed_data.data(), s_iq_uv.size());
  }
}

void MDEC::HandleSetScaleCommand()
{
  DebugAssert(s_remaining_halfwords == 64);

  // TODO: Remove extra copies..
  std::array<u16, 64> packed_data;
  s_data_in_fifo.PopRange(packed_data.data(), static_cast<u32>(packed_data.size()));
  s_remaining_halfwords -= 32;
  std::memcpy(s_scale_table.data(), packed_data.data(), s_scale_table.size() * sizeof(s16));
}

void MDEC::DrawDebugStateWindow()
{
  const float framebuffer_scale = Host::GetOSDScale();

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

  ImGui::Text("Blocks Decoded: %u", s_total_blocks_decoded);
  ImGui::Text("Data-In FIFO Size: %u (%u bytes)", s_data_in_fifo.GetSize(), s_data_in_fifo.GetSize() * 4);
  ImGui::Text("Data-Out FIFO Size: %u (%u bytes)", s_data_out_fifo.GetSize(), s_data_out_fifo.GetSize() * 4);
  ImGui::Text("DMA Enable: %s%s", s_enable_dma_in ? "In " : "", s_enable_dma_out ? "Out" : "");
  ImGui::Text("Current State: %s", state_names[static_cast<u8>(s_state)]);
  ImGui::Text("Current Block: %s", block_names[s_current_block]);
  ImGui::Text("Current Coefficient: %u", s_current_coefficient);

  if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Text("Data-Out FIFO Empty: %s", s_status.data_out_fifo_empty ? "Yes" : "No");
    ImGui::Text("Data-In FIFO Full: %s", s_status.data_in_fifo_full ? "Yes" : "No");
    ImGui::Text("Command Busy: %s", s_status.command_busy ? "Yes" : "No");
    ImGui::Text("Data-In Request: %s", s_status.data_in_request ? "Yes" : "No");
    ImGui::Text("Output Depth: %s", output_depths[static_cast<u8>(s_status.data_output_depth.GetValue())]);
    ImGui::Text("Output Signed: %s", s_status.data_output_signed ? "Yes" : "No");
    ImGui::Text("Output Bit 15: %u", ZeroExtend32(s_status.data_output_bit15.GetValue()));
    ImGui::Text("Current Block: %u", ZeroExtend32(s_status.current_block.GetValue()));
    ImGui::Text("Parameter Words Remaining: %d",
                static_cast<s32>(SignExtend32(s_status.parameter_words_remaining.GetValue())));
  }

  ImGui::End();
}
