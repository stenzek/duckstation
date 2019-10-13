#include "mdec.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
#include "dma.h"
#include "interrupt_controller.h"
#include "system.h"
#include <imgui.h>
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
  sw.Do(&m_enable_dma_in);
  sw.Do(&m_enable_dma_out);
  sw.Do(&m_data_in_fifo);
  sw.Do(&m_data_out_fifo);
  sw.Do(&m_command);
  sw.Do(&m_remaining_words);
  sw.Do(&m_iq_uv);
  sw.Do(&m_iq_y);
  sw.Do(&m_scale_table);
  sw.Do(&m_blocks);
  sw.Do(&m_current_block);
  sw.Do(&m_current_coefficient);
  sw.Do(&m_current_q_scale);

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

      m_enable_dma_in = cr.enable_dma_in;
      m_enable_dma_out = cr.enable_dma_out;
      UpdateStatusRegister();
      UpdateDMARequest();
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
  m_enable_dma_in = false;
  m_enable_dma_out = false;
  m_data_in_fifo.Clear();
  m_data_out_fifo.Clear();
  UpdateStatusRegister();
  UpdateDMARequest();
}

void MDEC::UpdateStatusRegister()
{
  m_status.data_out_fifo_empty = m_data_out_fifo.IsEmpty();
  m_status.data_in_fifo_full = m_data_in_fifo.IsFull();

  m_status.command_busy = false;
  m_status.parameter_words_remaining = Truncate16(m_remaining_words - 1);
  m_status.current_block = (m_current_block + 4) % NUM_BLOCKS;
}

void MDEC::UpdateDMARequest()
{
  // we always want data in if it's enabled
  const bool data_in_request = m_enable_dma_in && m_data_in_fifo.GetSpace() >= (32 * 2) && !m_data_out_fifo.IsFull();
  m_status.data_in_request = data_in_request;
  m_dma->SetRequest(DMA::Channel::MDECin, data_in_request);

  // we only want to send data out if we have some in the fifo
  const bool data_out_request = m_enable_dma_out && !m_data_out_fifo.IsEmpty();
  m_status.data_out_request = data_out_request;
  m_dma->SetRequest(DMA::Channel::MDECout, data_out_request);
}

u32 MDEC::ReadDataRegister()
{
  if (m_data_out_fifo.IsEmpty())
  {
    Execute();

    if (m_data_out_fifo.IsEmpty())
    {
      Log_WarningPrintf("MDEC data out FIFO empty on read");
      return UINT32_C(0xFFFFFFFF);
    }
  }

  const u32 value = m_data_out_fifo.Pop();
  if (m_data_out_fifo.IsEmpty())
  {
    UpdateStatusRegister();
    UpdateDMARequest();
  }

  return value;
}

void MDEC::WriteCommandRegister(u32 value)
{
  Log_TracePrintf("MDEC command/data register <- 0x%08X", value);

  if (m_command == Command::None)
  {
    // first word
    const CommandWord cw{value};
    m_command = cw.command;
    m_status.data_output_depth = cw.data_output_depth;
    m_status.data_output_signed = cw.data_output_signed;
    m_status.data_output_bit15 = cw.data_output_bit15;
    m_data_out_fifo.Clear();

    switch (cw.command)
    {
      case Command::DecodeMacroblock:
        m_remaining_words = ZeroExtend32(cw.parameter_word_count.GetValue());
        break;

      case Command::SetIqTab:
        m_remaining_words = 16 + (((value & 1) != 0) ? 16 : 0);
        break;

      case Command::SetScale:
        m_remaining_words = 32;
        break;

      default:
        Panic("Unknown command");
        break;
    }

    Log_DebugPrintf("MDEC command: 0x%08X (%u, %u words in parameter, %u expected)", cw.bits,
                    ZeroExtend32(static_cast<u8>(cw.command.GetValue())),
                    ZeroExtend32(cw.parameter_word_count.GetValue()), m_remaining_words);
  }
  else
  {
    DebugAssert(m_remaining_words > 0);
    m_data_in_fifo.Push(Truncate16(value));
    m_data_in_fifo.Push(Truncate16(value >> 16));
    m_remaining_words--;
  }

  Execute();
}

void MDEC::Execute()
{
  switch (m_command)
  {
    case Command::DecodeMacroblock:
    {
      if (!HandleDecodeMacroblockCommand())
      {
        UpdateStatusRegister();
        UpdateDMARequest();
        return;
      }
    }
    break;

    case Command::SetIqTab:
    {
      if (!HandleSetQuantTableCommand())
      {
        UpdateStatusRegister();
        UpdateDMARequest();
        return;
      }
    }
    break;

    case Command::SetScale:
    {
      if (!HandleSetScaleCommand())
      {
        UpdateStatusRegister();
        UpdateDMARequest();
        return;
      }
    }
    break;

    default:
    {
      UpdateStatusRegister();
      UpdateDMARequest();
      return;
    }
    break;
  }

  m_data_in_fifo.Clear();
  m_command = Command::None;
  m_current_block = 0;
  m_current_coefficient = 64;
  m_current_q_scale = 0;
  UpdateStatusRegister();
  UpdateDMARequest();
}

bool MDEC::HandleDecodeMacroblockCommand()
{
  if (m_status.data_output_depth <= DataOutputDepth_8Bit)
  {
    while (!m_data_in_fifo.IsEmpty())
    {
      if (!DecodeMonoMacroblock())
        break;
    }

    return m_data_in_fifo.IsEmpty() && m_remaining_words == 0;
  }
  else
  {
    while (!m_data_in_fifo.IsEmpty())
    {
      if (!DecodeColoredMacroblock())
        break;
    }

    return m_data_in_fifo.IsEmpty() && m_remaining_words == 0;
  }
}

bool MDEC::DecodeMonoMacroblock()
{
  // sufficient space in output?
  if (m_status.data_output_depth == DataOutputDepth_4Bit)
  {
    if (m_data_out_fifo.GetSpace() < (64 / 8))
      return false;
  }
  else
  {
    if (m_data_out_fifo.GetSpace() < (64 / 4))
      return false;
  }

  if (!rl_decode_block(m_blocks[0].data(), m_iq_y.data()))
    return false;

  IDCT(m_blocks[0].data());

  std::array<u8, 64> out_r;
  y_to_mono(m_blocks[0], out_r);

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

  m_debug_blocks_decoded++;
  return true;
}

bool MDEC::DecodeColoredMacroblock()
{
  // sufficient space in output?
  if (m_status.data_output_depth == DataOutputDepth_24Bit)
  {
    if (m_data_out_fifo.GetSpace() < (256 - (256 / 4)))
      return false;
  }
  else
  {
    if (m_data_out_fifo.GetSpace() < (256 / 2))
      return false;
  }

  for (; m_current_block < NUM_BLOCKS; m_current_block++)
  {
    if (!rl_decode_block(m_blocks[m_current_block].data(), (m_current_block >= 2) ? m_iq_y.data() : m_iq_uv.data()))
      return false;

    IDCT(m_blocks[m_current_block].data());
  }

  // done decoding
  m_current_block = 0;
  Log_DebugPrintf("Decoded colored macroblock");

  std::array<u32, 256> out_rgb;
  yuv_to_rgb(0, 0, m_blocks[0], m_blocks[1], m_blocks[2], out_rgb);
  yuv_to_rgb(8, 0, m_blocks[0], m_blocks[1], m_blocks[3], out_rgb);
  yuv_to_rgb(0, 8, m_blocks[0], m_blocks[1], m_blocks[4], out_rgb);
  yuv_to_rgb(8, 8, m_blocks[0], m_blocks[1], m_blocks[5], out_rgb);

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

  m_debug_blocks_decoded++;
  return true;
}

static constexpr std::array<u8, 64> zigzag = {{0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
                                               3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
                                               10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
                                               21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63}};
static constexpr std::array<u8, 64> zagzig = {{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                               12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                               35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                               58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63}};

bool MDEC::rl_decode_block(s16* blk, const u8* qt)
{
  if (m_current_coefficient == 64)
  {
    std::fill_n(blk, 64, s16(0));

    // skip padding at start
    u16 n;
    for (;;)
    {
      if (m_data_in_fifo.IsEmpty())
        return false;

      n = m_data_in_fifo.Pop();
      if (n == 0xFE00)
        continue;
      else
        break;
    }

    m_current_coefficient = 0;
    m_current_q_scale = (n >> 10) & 0x3F;
    s32 val =
      SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) * static_cast<s32>(ZeroExtend32(qt[m_current_coefficient]));

    if (m_current_q_scale == 0)
      val = SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) * 2;

    val = std::clamp(val, -0x400, 0x3FF);
    if (m_current_q_scale > 0)
      blk[zagzig[m_current_coefficient]] = static_cast<s16>(val);
    else if (m_current_q_scale == 0)
      blk[m_current_coefficient] = static_cast<s16>(val);
  }

  while (!m_data_in_fifo.IsEmpty())
  {
    u16 n = m_data_in_fifo.Pop();
    m_current_coefficient += ((n >> 10) & 0x3F) + 1;
    if (m_current_coefficient >= 64)
    {
      m_current_coefficient = 64;
      return true;
    }

    s32 val = (SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) *
                 static_cast<s32>(ZeroExtend32(qt[m_current_coefficient])) * static_cast<s32>(m_current_q_scale) +
               4) /
              8;

    if (m_current_q_scale == 0)
      val = SignExtendN<10, s32>(static_cast<s32>(n & 0x3FF)) * 2;

    val = std::clamp(val, -0x400, 0x3FF);
    // val = val * static_cast<s32>(ZeroExtend32(scalezag[i]));
    if (m_current_q_scale > 0)
      blk[zagzig[m_current_coefficient]] = static_cast<s16>(val);
    else if (m_current_q_scale == 0)
      blk[m_current_coefficient] = static_cast<s16>(val);
  }

  return false;
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

      // TODO: Signed output
      R += 128;
      G += 128;
      B += 128;

      rgb_out[(x + xx) + ((y + yy) * 16)] = ZeroExtend32(static_cast<u16>(R)) |
                                            (ZeroExtend32(static_cast<u16>(G)) << 8) |
                                            (ZeroExtend32(static_cast<u16>(B)) << 16);
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

bool MDEC::HandleSetQuantTableCommand()
{
  if (m_remaining_words > 0)
    return false;

  // TODO: Remove extra copies..
  std::array<u16, 32> packed_data;
  m_data_in_fifo.PopRange(packed_data.data(), static_cast<u32>(packed_data.size()));
  std::memcpy(m_iq_y.data(), packed_data.data(), m_iq_y.size());

  if (!m_data_in_fifo.IsEmpty())
  {
    m_data_in_fifo.PopRange(packed_data.data(), static_cast<u32>(packed_data.size()));
    std::memcpy(m_iq_uv.data(), packed_data.data(), m_iq_uv.size());
  }

  return true;
}

bool MDEC::HandleSetScaleCommand()
{
  if (m_remaining_words > 0)
    return false;

  // TODO: Remove extra copies..
  std::array<u16, 64> packed_data;
  m_data_in_fifo.PopRange(packed_data.data(), static_cast<u32>(packed_data.size()));
  std::memcpy(m_scale_table.data(), packed_data.data(), m_scale_table.size() * sizeof(s16));
  return true;
}

void MDEC::DrawDebugMenu()
{
  ImGui::MenuItem("MDEC", nullptr, &m_debug_show_state);
}

void MDEC::DrawDebugWindow()
{
  if (!m_debug_show_state)
    return;

  ImGui::SetNextWindowSize(ImVec2(300, 350), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("MDEC State", &m_debug_show_state))
  {
    ImGui::End();
    return;
  }

  if (m_debug_blocks_decoded > 0)
  {
    m_debug_last_blocks_decoded = m_debug_blocks_decoded;
    m_debug_blocks_decoded = 0;
  }

  static constexpr std::array<const char*, 4> command_names = {{"None", "Decode Macroblock", "SetIqTab", "SetScale"}};
  static constexpr std::array<const char*, 4> output_depths = {{"4-bit", "8-bit", "24-bit", "15-bit"}};
  static constexpr std::array<const char*, 6> block_names = {{"Crblk", "Cbblk", "Y1", "Y2", "Y3", "Y4"}};

  ImGui::Text("Blocks Decoded: %u (%ux8, 320x%u)", m_debug_last_blocks_decoded, m_debug_last_blocks_decoded * 8,
              m_debug_last_blocks_decoded * 8 / (320 / 8) * 8);
  ImGui::Text("Data-In FIFO Size: %u (%u bytes)", m_data_in_fifo.GetSize(), m_data_in_fifo.GetSize() * 4);
  ImGui::Text("Data-Out FIFO Size: %u (%u bytes)", m_data_out_fifo.GetSize(), m_data_out_fifo.GetSize() * 4);
  ImGui::Text("DMA Enable: %s%s", m_enable_dma_in ? "In " : "", m_enable_dma_out ? "Out" : "");
  ImGui::Text("Current Command: %s", command_names[static_cast<u8>(m_command)]);
  ImGui::Text("Current Block: %s", block_names[m_current_block]);
  ImGui::Text("Current Coefficient: %u", m_current_coefficient);

  if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Text("Data-Out FIFO Empty: %s", m_status.data_out_fifo_empty ? "Yes" : "No");
    ImGui::Text("Data-In FIFO Full: %s", m_status.data_in_fifo_full ? "Yes" : "No");
    ImGui::Text("Command Busy: %s", m_status.command_busy ? "Yes" : "No");
    ImGui::Text("Data-In Request: %s", m_status.data_in_request ? "Yes" : "No");
    ImGui::Text("Output Depth: %s", output_depths[static_cast<u8>(m_status.data_output_depth.GetValue())]);
    ImGui::Text("Output Signed: %s", m_status.data_output_signed ? "Yes" : "No");
    ImGui::Text("Output Bit 15: %u", ZeroExtend32(m_status.data_output_bit15.GetValue()));
    ImGui::Text("Current Block: %u", ZeroExtend32(m_status.current_block.GetValue()));
    ImGui::Text("Parameter Words Remaining: %d",
                static_cast<s32>(SignExtend32(m_status.parameter_words_remaining.GetValue())));
  }

  ImGui::End();
}