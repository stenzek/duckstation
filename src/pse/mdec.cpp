#include "mdec.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
#include "dma.h"
#include "interrupt_controller.h"
#include "system.h"
Log_SetChannel(MDEC);

MDEC::MDEC() = default;

MDEC::~MDEC() = default;

bool MDEC::Initialize(System* system, DMA* dma)
{
  m_system = system;
  m_dma = dma;
  return true;
}

void MDEC::Reset()
{
  SoftReset();
}

bool MDEC::DoState(StateWrapper& sw)
{
  sw.Do(&m_status.bits);
  sw.Do(&m_data_in_fifo);
  sw.Do(&m_data_out_fifo);
  sw.Do(&m_command);
  sw.Do(&m_command_parameter_count);
  sw.Do(&m_iq_uv);
  sw.Do(&m_iq_y);
  sw.Do(&m_scale_table);

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
      Log_DebugPrintf("MDEC status register -> 0x%08X", m_status.bits);
      return m_status.bits;
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

      m_status.data_in_request = cr.enable_dma_in;
      m_status.data_out_request = cr.enable_dma_out;
      m_dma->SetRequest(DMA::Channel::MDECin, cr.enable_dma_in);
      m_dma->SetRequest(DMA::Channel::MDECout, cr.enable_dma_out);

      return;
    }

    default:
    {
      Log_ErrorPrintf("Unknown MDEC register write: 0x%08X <- 0x%08X", offset, value);
      return;
    }
  }
}

u32 MDEC::DMARead()
{
  return ReadDataRegister();
}

void MDEC::DMAWrite(u32 value)
{
  WriteCommandRegister(value);
}

void MDEC::SoftReset()
{
  m_status = {};
  m_data_in_fifo.Clear();
  m_data_out_fifo.Clear();

  UpdateStatusRegister();
}

void MDEC::UpdateStatusRegister()
{
  m_status.data_out_fifo_empty = m_data_out_fifo.IsEmpty();
  m_status.data_in_fifo_full = m_data_in_fifo.IsFull();

  m_status.command_busy = !m_data_in_fifo.IsEmpty();
  if (!m_data_in_fifo.IsEmpty())
  {
    const CommandWord cw{m_data_in_fifo.Peek(0)};
    m_status.parameter_words_remaining = Truncate16(m_command_parameter_count - m_data_in_fifo.GetSize());
  }
  else
  {
    m_status.parameter_words_remaining = 0;
  }
}

u32 MDEC::ReadDataRegister()
{
  if (m_data_out_fifo.IsEmpty())
  {
    // Log_WarningPrintf("MDEC data out FIFO empty on read");
    return UINT32_C(0xFFFFFFFF);
  }

  const u32 value = m_data_out_fifo.Pop();
  UpdateStatusRegister();
  return value;
}

void MDEC::WriteCommandRegister(u32 value)
{
  Log_DebugPrintf("MDEC command/data register <- 0x%08X", value);

  if (m_data_in_fifo.IsEmpty())
  {
    // first word
    const CommandWord cw{value};
    m_command = cw.command;
    m_status.data_output_depth = cw.data_output_depth;
    m_status.data_output_signed = cw.data_output_signed;
    m_status.data_output_bit15 = cw.data_output_bit15;

    switch (cw.command)
    {
      case Command::DecodeMacroblock:
        m_command_parameter_count = ZeroExtend32(cw.parameter_word_count.GetValue());
        break;

      case Command::SetIqTab:
        m_command_parameter_count = 16 + (((value & 1) != 0) ? 16 : 0);
        break;

      case Command::SetScale:
        m_command_parameter_count = 32;
        break;

      default:
        Panic("Unknown command");
        break;
    }

    Log_DebugPrintf("MDEC command: 0x%08X (%u, %u words in parameter, %u expected)", cw.bits,
                    ZeroExtend32(static_cast<u8>(cw.command.GetValue())),
                    ZeroExtend32(cw.parameter_word_count.GetValue()), m_command_parameter_count);
  }

  m_data_in_fifo.Push(value);

  if (m_data_in_fifo.GetSize() <= m_command_parameter_count)
  {
    UpdateStatusRegister();
    return;
  }

  // pop command
  m_data_in_fifo.RemoveOne();
  switch (m_command)
  {
    case Command::DecodeMacroblock:
      HandleDecodeMacroblockCommand();
      break;

    case Command::SetIqTab:
      HandleSetQuantTableCommand();
      break;

    case Command::SetScale:
      HandleSetScaleCommand();
      break;
  }

  m_data_in_fifo.Clear();
  m_command = Command::None;
  m_command_parameter_count = 0;
  UpdateStatusRegister();
}

void MDEC::HandleDecodeMacroblockCommand()
{
  // TODO: Remove this copy and strict aliasing violation..
  std::vector<u16> temp(m_data_in_fifo.GetSize() * 2);
  m_data_in_fifo.PopRange(reinterpret_cast<u32*>(temp.data()), m_data_in_fifo.GetSize());

  const u16* src = temp.data();
  const u16* src_end = src + temp.size();

  if (m_status.data_output_depth <= DataOutputDepth_8Bit)
  {
    while (src != src_end)
    {
      src = DecodeMonoMacroblock(src, src_end);
      Log_InfoPrint("Decoded mono macroblock");
    }
  }
  else
  {
    while (src != src_end)
    {
      u32 old_offs = static_cast<u32>(src - temp.data());
      src = DecodeColoredMacroblock(src, src_end);
      Log_InfoPrintf("Decoded colour macroblock, ptr was %u, now %u", old_offs, static_cast<u32>(src - temp.data()));
    }
  }
}

const u16* MDEC::DecodeMonoMacroblock(const u16* src, const u16* src_end)
{
  std::array<s16, 64> Yblk;
  if (!rl_decode_block(Yblk.data(), src, src_end, m_iq_y.data()))
    return src_end;

  std::array<u8, 64> out_r;
  y_to_mono(Yblk, out_r);

  switch (m_status.data_output_depth)
  {
    case DataOutputDepth_4Bit:
    {
      const u8* in_ptr = out_r.data();
      for (u32 i = 0; i < (64 / 8); i++)
      {
        u32 value = ZeroExtend32(*(in_ptr++) >> 4);
        value |= ZeroExtend32(*(in_ptr++) >> 4) << 4;
        value |= ZeroExtend32(*(in_ptr++) >> 4) << 8;
        value |= ZeroExtend32(*(in_ptr++) >> 4) << 12;
        value |= ZeroExtend32(*(in_ptr++) >> 4) << 16;
        value |= ZeroExtend32(*(in_ptr++) >> 4) << 20;
        value |= ZeroExtend32(*(in_ptr++) >> 4) << 24;
        value |= ZeroExtend32(*(in_ptr++) >> 4) << 28;
        m_data_out_fifo.Push(value);
      }
    }
    break;

    case DataOutputDepth_8Bit:
    {
      const u8* in_ptr = out_r.data();
      for (u32 i = 0; i < (64 / 4); i++)
      {
        u32 value = ZeroExtend32(*in_ptr++);
        value |= ZeroExtend32(*in_ptr++) << 8;
        value |= ZeroExtend32(*in_ptr++) << 16;
        value |= ZeroExtend32(*in_ptr++) << 24;
        m_data_out_fifo.Push(value);
      }
    }
    break;

    default:
      break;
  }

  return src;
}

const u16* MDEC::DecodeColoredMacroblock(const u16* src, const u16* src_end)
{
  std::array<s16, 64> Crblk;
  std::array<s16, 64> Cbblk;
  std::array<std::array<s16, 64>, 4> Yblk;
  std::array<u32, 256> out_rgb;

  if (!rl_decode_block(Crblk.data(), src, src_end, m_iq_uv.data()) ||
      !rl_decode_block(Cbblk.data(), src, src_end, m_iq_uv.data()) ||
      !rl_decode_block(Yblk[0].data(), src, src_end, m_iq_y.data()) ||
      !rl_decode_block(Yblk[1].data(), src, src_end, m_iq_y.data()) ||
      !rl_decode_block(Yblk[2].data(), src, src_end, m_iq_y.data()) ||
      !rl_decode_block(Yblk[3].data(), src, src_end, m_iq_y.data()))
  {
    return src_end;
  }

  yuv_to_rgb(0, 0, Crblk, Cbblk, Yblk[0], out_rgb);
  yuv_to_rgb(0, 8, Crblk, Cbblk, Yblk[1], out_rgb);
  yuv_to_rgb(8, 0, Crblk, Cbblk, Yblk[2], out_rgb);
  yuv_to_rgb(8, 8, Crblk, Cbblk, Yblk[3], out_rgb);

  switch (m_status.data_output_depth)
  {
    case DataOutputDepth_24Bit:
    {
      // pack tightly
      u32 index = 0;
      u32 state = 0;
      u32 rgb = 0;
      while (index < out_rgb.size())
      {
        switch (state)
        {
          case 0:
            rgb = out_rgb[index++]; // RGB-
            state = 1;
            break;
          case 1:
            rgb |= (out_rgb[index] & 0xFF) << 24; // RGBR
            m_data_out_fifo.Push(rgb);
            rgb = out_rgb[index] >> 8; // GB--
            index++;
            state = 2;
            break;
          case 2:
            rgb |= out_rgb[index] << 16; // GBRG
            m_data_out_fifo.Push(rgb);
            rgb = out_rgb[index] >> 16; // B---
            index++;
            state = 3;
            break;
          case 3:
            rgb |= out_rgb[index] << 8; // BRGB
            m_data_out_fifo.Push(rgb);
            index++;
            state = 0;
            break;
        }
      }
      break;
    }

    case DataOutputDepth_15Bit:
    {
      const u16 a = ZeroExtend16(m_status.data_output_bit15.GetValue());
      for (u32 i = 0; i < static_cast<u32>(out_rgb.size());)
      {
        u32 color = out_rgb[i++];
        u16 r = Truncate16((color >> 3) & 0x1Fu);
        u16 g = Truncate16((color >> 11) & 0x1Fu);
        u16 b = Truncate16((color >> 19) & 0x1Fu);
        const u16 color15a = r | (g << 5) | (b << 10) | (a << 15);

        color = out_rgb[i++];
        r = Truncate16((color >> 3) & 0x1Fu);
        g = Truncate16((color >> 11) & 0x1Fu);
        b = Truncate16((color >> 19) & 0x1Fu);
        const u16 color15b = r | (g << 5) | (b << 10) | (a << 15);

        m_data_out_fifo.Push(ZeroExtend32(color15a) | (ZeroExtend32(color15b) << 16));
      }
    }
    break;

    default:
      break;
  }

  return src;
}

static constexpr std::array<u8, 64> zigzag = {{0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
                                               3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
                                               10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
                                               21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63}};

bool MDEC::rl_decode_block(s16* blk, const u16*& src, const u16* src_end, const u8* qt)
{
  std::fill_n(blk, 64, s16(0));

  // skip padding
  u16 n;
  for (;;)
  {
    if (src == src_end)
      return false;

    n = *(src++);
    if (n == 0xFE00)
      continue;
    else
      break;
  }

  u32 k = 0;
  u16 q_scale = (n >> 10) & 0x3F;
  s32 val = SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) * static_cast<s32>(ZeroExtend32(qt[k]));

  for (;;)
  {
    if (q_scale == 0)
      val = SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) * 2;

    val = std::clamp(val, -0x400, 0x3FF);
    // val = val * static_cast<s32>(ZeroExtend32(scalezag[i]));
    if (q_scale > 0)
      blk[zigzag[k]] = static_cast<s16>(val);
    else if (q_scale == 0)
      blk[k] = static_cast<s16>(val);

    if (src == src_end)
      break;

    n = *(src++);
    k += ((n >> 10) & 0x3F) + 1;
    if (k >= 64)
      break;

    val = (SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) * static_cast<s32>(ZeroExtend32(qt[k])) *
             static_cast<s32>(q_scale) +
           4) /
          8;
  }

#undef READ_SRC

  // insufficient coefficients
  if (k < 64)
  {
    Log_DebugPrintf("Only %u of 64 coefficients in block, skipping", k);
    return false;
  }

  IDCT(blk);
  return true;
}

void MDEC::IDCT(s16* blk)
{
  std::array<s64, 64> temp_buffer;
  for (u32 x = 0; x < 8; x++)
  {
    for (u32 y = 0; y < 8; y++)
    {
      s64 sum = 0;
      for (u32 u = 0; u < 8; u++)
        sum += s32(blk[u * 8 + x]) * s32(m_scale_table[u * 8 + y]);
      temp_buffer[x + y * 8] = sum;
    }
  }
  for (u32 x = 0; x < 8; x++)
  {
    for (u32 y = 0; y < 8; y++)
    {
      s64 sum = 0;
      for (u32 u = 0; u < 8; u++)
        sum += s64(temp_buffer[u + y * 8]) * s32(m_scale_table[u * 8 + x]);

      blk[x + y * 8] =
        static_cast<s16>(std::clamp<s32>(SignExtendN<9, s32>((sum >> 32) + ((sum >> 31) & 1)), -128, 127));
    }
  }
}

void MDEC::yuv_to_rgb(u32 xx, u32 yy, const std::array<s16, 64>& Crblk, const std::array<s16, 64>& Cbblk,
                      const std::array<s16, 64>& Yblk, std::array<u32, 256>& rgb_out)
{
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
      R = static_cast<s16>(std::clamp(static_cast<int>(Y) + R, -128, 127));
      G = static_cast<s16>(std::clamp(static_cast<int>(Y) + G, -128, 127));
      B = static_cast<s16>(std::clamp(static_cast<int>(Y) + B, -128, 127));

      R += 128;
      G += 128;
      B += 128;

      rgb_out[(x + xx) + ((y + yy) * 16)] = ZeroExtend32(static_cast<u16>(R)) |
                                            (ZeroExtend32(static_cast<u16>(G)) << 8) |
                                            (ZeroExtend32(static_cast<u16>(B)) << 16) | UINT32_C(0xFF000000);
    }
  }
}

void MDEC::y_to_mono(const std::array<s16, 64>& Yblk, std::array<u8, 64>& r_out)
{
  for (u32 i = 0; i < 64; i++)
  {
    s16 Y = Yblk[i];
    Y = SignExtendN<10, s16>(Y);
    Y = std::clamp<s16>(Y, -128, 127);
    Y += 128;
    r_out[i] = static_cast<u8>(Y);
  }
}

void MDEC::HandleSetQuantTableCommand()
{
  // TODO: Remove extra copies..
  std::array<u32, 16> packed_data;
  m_data_in_fifo.PopRange(packed_data.data(), static_cast<u32>(packed_data.size()));
  std::memcpy(m_iq_y.data(), packed_data.data(), m_iq_y.size());

  if (!m_data_in_fifo.IsEmpty())
  {
    m_data_in_fifo.PopRange(packed_data.data(), static_cast<u32>(packed_data.size()));
    std::memcpy(m_iq_uv.data(), packed_data.data(), m_iq_uv.size());
  }
}

void MDEC::HandleSetScaleCommand()
{
  // TODO: Remove extra copies..
  std::array<u32, 32> packed_data;
  m_data_in_fifo.PopRange(packed_data.data(), static_cast<u32>(packed_data.size()));
  std::memcpy(m_scale_table.data(), packed_data.data(), m_scale_table.size() * sizeof(s16));
}
