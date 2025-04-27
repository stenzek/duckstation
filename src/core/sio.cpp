// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "sio.h"
#include "interrupt_controller.h"
#include "sio_connection.h"
#include "system.h"
#include "timing_event.h"

#include "util/state_wrapper.h"

#include "common/bitfield.h"
#include "common/bitutils.h"
#include "common/log.h"

#include "imgui.h"

#include <array>
#include <memory>

LOG_CHANNEL(SIO);

namespace SIO {
namespace {

enum : u32
{
  // Actually 8 bytes, but we allow a bit more due to network latency.
  RX_FIFO_SIZE = 32
};

union SIO_CTRL
{
  u16 bits;

  BitField<u16, bool, 0, 1> TXEN;
  BitField<u16, bool, 1, 1> DTROUTPUT;
  BitField<u16, bool, 2, 1> RXEN;
  BitField<u16, bool, 3, 1> TXOUTPUT;
  BitField<u16, bool, 4, 1> ACK;
  BitField<u16, bool, 5, 1> RTSOUTPUT;
  BitField<u16, bool, 6, 1> RESET;
  BitField<u16, u8, 8, 2> RXIMODE;
  BitField<u16, bool, 10, 1> TXINTEN;
  BitField<u16, bool, 11, 1> RXINTEN;
  BitField<u16, bool, 12, 1> DTRINTEN;
};

union SIO_STAT
{
  u32 bits;

  BitField<u32, bool, 0, 1> TXRDY;
  BitField<u32, bool, 1, 1> RXFIFONEMPTY;
  BitField<u32, bool, 2, 1> TXIDLE;
  BitField<u32, bool, 3, 1> RXPARITY;
  BitField<u32, bool, 4, 1> RXFIFOOVERRUN;
  BitField<u32, bool, 5, 1> RXBADSTOPBIT;
  BitField<u32, bool, 6, 1> RXINPUTLEVEL;
  BitField<u32, bool, 7, 1> DSRINPUTLEVEL;
  BitField<u32, bool, 8, 1> CTSINPUTLEVEL;
  BitField<u32, bool, 9, 1> INTR;
  BitField<u32, u32, 11, 15> TMR;
};

union SIO_MODE
{
  u16 bits;

  BitField<u16, u8, 0, 2> reload_factor;
  BitField<u16, u8, 2, 2> character_length;
  BitField<u16, bool, 4, 1> parity_enable;
  BitField<u16, u8, 5, 1> parity_type;
  BitField<u16, u8, 6, 2> stop_bit_length;
};

} // namespace

static void SoftReset();

static TickCount GetTicksBetweenTransfers();

static void UpdateTXRX();
static void SetInterrupt();

static void UpdateEvent();
static void TransferEvent(void* param, TickCount ticks, TickCount ticks_late);
static void TransferWithoutSync();
static void TransferWithSync();

namespace {

struct ALIGN_TO_CACHE_LINE SIOState
{
  SIO_CTRL ctrl = {};
  SIO_STAT stat = {};
  SIO_MODE mode = {};
  u16 baud_rate = 0;

  InlineFIFOQueue<u8, RX_FIFO_SIZE> data_in;

  u8 data_out = 0;
  bool data_out_full = false;
  bool latched_txen = false;

  bool sync_mode = true;
  bool sync_last_dtr = false;
  bool sync_last_rts = false;
  u8 sync_last_fifo_size = 0;
  u8 sync_remote_rx_fifo_size = 0;

  bool is_server = false;
  bool needs_initial_sync = false;

  TimingEvent transfer_event{"SIO Transfer", 1, 1, &SIO::TransferEvent, nullptr};

  std::unique_ptr<SIOConnection> connection;
};
} // namespace

static constexpr std::array<u32, 4> s_mul_factors = {{1, 16, 64, 0}};

static SIOState s_state;

} // namespace SIO

void SIO::Initialize()
{
  s_state.is_server = !IsDebuggerPresent();
  if (s_state.is_server)
    s_state.connection = SIOConnection::CreateSocketServer("0.0.0.0", 1337);
  else
    s_state.connection = SIOConnection::CreateSocketClient("127.0.0.1", 1337);

  s_state.stat.bits = 0;
  s_state.stat.CTSINPUTLEVEL = false;
  s_state.needs_initial_sync = true;
  Reset();
}

void SIO::Shutdown()
{
  s_state.connection.reset();
  s_state.transfer_event.Deactivate();
}

void SIO::Reset()
{
  SoftReset();
}

bool SIO::DoState(StateWrapper& sw)
{
  const bool dtr = s_state.stat.DSRINPUTLEVEL;
  const bool rts = s_state.stat.CTSINPUTLEVEL;

  sw.Do(&s_state.ctrl.bits);
  sw.Do(&s_state.stat.bits);
  sw.Do(&s_state.mode.bits);
  sw.Do(&s_state.baud_rate);

  s_state.stat.DSRINPUTLEVEL = dtr;
  s_state.stat.CTSINPUTLEVEL = rts;

  UpdateEvent();
  UpdateTXRX();

  return !sw.HasError();
}

void SIO::SoftReset()
{
  s_state.ctrl.bits = 0;
  s_state.stat.RXPARITY = false;
  s_state.stat.RXFIFOOVERRUN = false;
  s_state.stat.RXBADSTOPBIT = false;
  s_state.stat.INTR = false;
  s_state.mode.bits = 0;
  s_state.baud_rate = 0xDC;
  s_state.data_in.Clear();
  s_state.data_out = 0;
  s_state.data_out_full = false;

  UpdateEvent();
  UpdateTXRX();
}

void SIO::UpdateTXRX()
{
  s_state.stat.TXRDY = !s_state.data_out_full && s_state.stat.CTSINPUTLEVEL;
  s_state.stat.TXIDLE = !s_state.data_out_full;
  s_state.stat.RXFIFONEMPTY = !s_state.data_in.IsEmpty();
}

void SIO::SetInterrupt()
{
  DEV_LOG("Set SIO IRQ");
  s_state.stat.INTR = true;
  InterruptController::SetLineState(InterruptController::IRQ::SIO, true);
}

u32 SIO::ReadRegister(u32 offset, u32 read_size)
{
  switch (offset)
  {
    case 0x00: // SIO_DATA
    {
      s_state.transfer_event.InvokeEarly(false);

      const u32 data_in_size = std::min(s_state.data_in.GetSize(), 4u);
      u32 res = 0;
      switch (data_in_size)
      {
        case 4:
          res = ZeroExtend32(s_state.data_in.Peek(3)) << 24;
          [[fallthrough]];

        case 3:
          res |= ZeroExtend32(s_state.data_in.Peek(2)) << 16;
          [[fallthrough]];

        case 2:
          res |= ZeroExtend32(s_state.data_in.Peek(1)) << 8;
          [[fallthrough]];

        case 1:
          res |= ZeroExtend32(s_state.data_in.Peek(0));
          s_state.data_in.Remove(std::min(s_state.data_in.GetSize(), read_size));
          break;

        case 0:
        default:
          res = 0xFFFFFFFFu;
          break;
      }

      WARNING_LOG("Read SIO_DATA -> 0x{:08X}", res);
      UpdateTXRX();
      return res;
    }

    case 0x04: // SIO_STAT
    {
      s_state.transfer_event.InvokeEarly(false);

      const u32 bits = s_state.stat.bits;
      DEBUG_LOG("Read SIO_STAT -> 0x{:08X}", bits);
      return bits;
    }

    case 0x08: // SIO_MODE
      return ZeroExtend32(s_state.mode.bits);

    case 0x0A: // SIO_CTRL
      return ZeroExtend32(s_state.ctrl.bits);

    case 0x0E: // SIO_BAUD
      return ZeroExtend32(s_state.baud_rate);

    [[unlikely]] default:
      ERROR_LOG("Unknown register read: 0x{:X}", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void SIO::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00: // SIO_DATA
    {
      WARNING_LOG("SIO_DATA (W) <- 0x{:02X}", value);
      s_state.transfer_event.InvokeEarly(false);

      if (s_state.data_out_full)
        WARNING_LOG("SIO TX buffer overflow, lost 0x{:02X} when writing 0x{:02X}", s_state.data_out, value);

      s_state.data_out = Truncate8(value);
      s_state.data_out_full = true;
      UpdateTXRX();

      s_state.transfer_event.InvokeEarly(false);
      return;
    }

    case 0x0A: // SIO_CTRL
    {
      DEV_LOG("SIO_CTRL <- 0x{:04X}", value);
      s_state.transfer_event.InvokeEarly(false);

      s_state.ctrl.bits = Truncate16(value);
      if (s_state.ctrl.RESET)
        SoftReset();

      if (s_state.ctrl.ACK)
      {
        s_state.stat.RXPARITY = false;
        s_state.stat.RXFIFOOVERRUN = false;
        s_state.stat.RXBADSTOPBIT = false;
        s_state.stat.INTR = false;
        InterruptController::SetLineState(InterruptController::IRQ::SIO, false);
      }

      if (!s_state.ctrl.RXEN)
      {
        WARNING_LOG("Clearing Input FIFO");
        s_state.data_in.Clear();
        s_state.stat.RXFIFOOVERRUN = false;
        UpdateTXRX();
      }
      /*if (!m_ctrl.TXEN)
      {
        WARNING_LOG("Clearing output fifo");
        m_data_out_full = false;
        UpdateTXRX();
      }*/

      return;
    }

    case 0x08: // SIO_MODE
    {
      DEV_LOG("SIO_MODE <- 0x{:08X}", value);
      s_state.mode.bits = Truncate16(value);
      return;
    }

    case 0x0E:
    {
      DEBUG_LOG("SIO_BAUD <- 0x{:08X}", value);
      s_state.baud_rate = Truncate16(value);
      return;
    }

    default:
      ERROR_LOG("Unknown register write: 0x{:X} <- 0x{:08X}", offset, value);
      return;
  }
}

void SIO::DrawDebugStateWindow(float scale)
{
  static const ImVec4 active_color{1.0f, 1.0f, 1.0f, 1.0f};
  static const ImVec4 inactive_color{0.4f, 0.4f, 0.4f, 1.0f};

  ImGui::Text("Connected: ");
  ImGui::SameLine();
  if (s_state.connection && s_state.connection->IsConnected())
    ImGui::TextColored(active_color, "Yes (%s)", s_state.is_server ? "server" : "client");
  else
    ImGui::TextColored(inactive_color, "No");

  ImGui::Text("Status: ");
  ImGui::SameLine();

  float pos = ImGui::GetCursorPosX();
  ImGui::TextColored(s_state.stat.TXRDY ? active_color : inactive_color, "TXRDY");
  ImGui::SameLine();
  ImGui::TextColored(s_state.stat.RXFIFONEMPTY ? active_color : inactive_color, "RXFIFONEMPTY");
  ImGui::SameLine();
  ImGui::TextColored(s_state.stat.TXIDLE ? active_color : inactive_color, "TXIDLE");
  ImGui::SameLine();
  ImGui::TextColored(s_state.stat.RXPARITY ? active_color : inactive_color, "RXPARITY");
  ImGui::SameLine();
  ImGui::TextColored(s_state.stat.RXFIFOOVERRUN ? active_color : inactive_color, "RXFIFOOVERRUN");
  ImGui::SetCursorPosX(pos);
  ImGui::TextColored(s_state.stat.RXBADSTOPBIT ? active_color : inactive_color, "RXBADSTOPBIT");
  ImGui::SameLine();
  ImGui::TextColored(s_state.stat.RXINPUTLEVEL ? active_color : inactive_color, "RXINPUTLEVEL");
  ImGui::SameLine();
  ImGui::TextColored(s_state.stat.DSRINPUTLEVEL ? active_color : inactive_color, "DSRINPUTLEVEL");
  ImGui::SameLine();
  ImGui::TextColored(s_state.stat.CTSINPUTLEVEL ? active_color : inactive_color, "CTSINPUTLEVEL");
  ImGui::SameLine();
  ImGui::TextColored(s_state.stat.INTR ? active_color : inactive_color, "INTR");

  ImGui::NewLine();

  ImGui::Text("Control: ");
  ImGui::SameLine();

  pos = ImGui::GetCursorPosX();
  ImGui::TextColored(s_state.ctrl.TXEN ? active_color : inactive_color, "TXEN");
  ImGui::SameLine();
  ImGui::TextColored(s_state.ctrl.DTROUTPUT ? active_color : inactive_color, "DTROUTPUT");
  ImGui::SameLine();
  ImGui::TextColored(s_state.ctrl.RXEN ? active_color : inactive_color, "RXEN");
  ImGui::SameLine();
  ImGui::TextColored(s_state.ctrl.TXOUTPUT ? active_color : inactive_color, "TXOUTPUT");
  ImGui::SameLine();
  ImGui::TextColored(s_state.ctrl.RTSOUTPUT ? active_color : inactive_color, "RTSOUTPUT");
  ImGui::SetCursorPosX(pos);
  ImGui::TextColored(s_state.ctrl.TXINTEN ? active_color : inactive_color, "TXINTEN");
  ImGui::SameLine();
  ImGui::TextColored(s_state.ctrl.RXINTEN ? active_color : inactive_color, "RXINTEN");
  ImGui::SameLine();
  ImGui::TextColored(s_state.ctrl.RXINTEN ? active_color : inactive_color, "RXIMODE: %u",
                     s_state.ctrl.RXIMODE.GetValue());

  ImGui::NewLine();

  ImGui::Text("Mode: ");
  ImGui::Text("  Reload Factor: %u", s_mul_factors[s_state.mode.reload_factor]);
  ImGui::Text("  Character Length: %u", s_state.mode.character_length.GetValue());
  ImGui::Text("  Parity Enable: %s", s_state.mode.parity_enable ? "Yes" : "No");
  ImGui::Text("  Parity Type: %u", s_state.mode.parity_type.GetValue());
  ImGui::Text("  Stop Bit Length: %u", s_state.mode.stop_bit_length.GetValue());

  ImGui::NewLine();

  ImGui::Text("Baud Rate: %u", s_state.baud_rate);

  ImGui::NewLine();

  ImGui::TextColored(s_state.data_out_full ? active_color : inactive_color, "Output buffer: 0x%02X", s_state.data_out);

  ImGui::Text("Input buffer: ");
  for (u32 i = 0; i < s_state.data_in.GetSize(); i++)
  {
    ImGui::SameLine();
    ImGui::Text("0x%02X ", s_state.data_in.Peek(i));
  }
}

TickCount SIO::GetTicksBetweenTransfers()
{
  const u32 factor = s_mul_factors[s_state.mode.reload_factor];
  const u32 ticks = std::max<u32>((s_state.baud_rate * factor) & ~u32(1), factor);

  return static_cast<TickCount>(ticks);
}

void SIO::UpdateEvent()
{
  if (!s_state.connection)
  {
    s_state.transfer_event.Deactivate();
    s_state.stat.CTSINPUTLEVEL = true;
    s_state.stat.DSRINPUTLEVEL = false;
    s_state.sync_last_dtr = false;
    s_state.sync_last_rts = false;
    s_state.sync_remote_rx_fifo_size = 0;
    return;
  }

  TickCount ticks = GetTicksBetweenTransfers();
  if (ticks == 0)
    ticks = System::GetMaxSliceTicks();

  if (s_state.transfer_event.GetPeriod() == ticks && s_state.transfer_event.IsActive())
    return;

  s_state.transfer_event.Deactivate();
  s_state.transfer_event.SetPeriodAndSchedule(ticks);
}

void SIO::TransferEvent(void* param, TickCount ticks, TickCount ticks_late)
{
  if (s_state.sync_mode)
    TransferWithSync();
  else
    TransferWithoutSync();
}

void SIO::TransferWithoutSync()
{
  // bytes aren't transmitted when CTS isn't set (i.e. there's nothing on the other side)
  if (s_state.connection && s_state.connection->IsConnected())
  {
    s_state.stat.CTSINPUTLEVEL = true;
    s_state.stat.DSRINPUTLEVEL = true;

    if (s_state.ctrl.RXEN)
    {
      u8 data_in;
      u32 data_in_size = s_state.connection->Read(&data_in, sizeof(data_in), 0);
      if (data_in_size > 0)
      {
        if (s_state.data_in.IsFull())
        {
          WARNING_LOG("FIFO overrun");
          s_state.data_in.RemoveOne();
          s_state.stat.RXFIFOOVERRUN = true;
        }

        s_state.data_in.Push(data_in);

        if (s_state.ctrl.RXINTEN)
          SetInterrupt();
      }
    }

    if (s_state.ctrl.TXEN && s_state.data_out_full)
    {
      const u8 data_out = s_state.data_out;
      s_state.data_out_full = false;

      const u32 data_sent = s_state.connection->Write(&data_out, sizeof(data_out));
      if (data_sent != sizeof(data_out))
        WARNING_LOG("Failed to send 0x{:02X} to connection", data_out);

      if (s_state.ctrl.TXINTEN)
        SetInterrupt();
    }
  }
  else
  {
    s_state.stat.CTSINPUTLEVEL = false;
    s_state.stat.DSRINPUTLEVEL = false;
  }

  UpdateTXRX();
}

void SIO::TransferWithSync()
{
  union SyncByte
  {
    BitField<u8, bool, 0, 1> has_data;
    BitField<u8, bool, 1, 1> dtr_level;
    BitField<u8, bool, 2, 1> rts_level;
    BitField<u8, u8, 3, 5> rx_fifo_size;

    u8 bits;
  };

  if (!s_state.connection || !s_state.connection->IsConnected())
  {
    s_state.stat.CTSINPUTLEVEL = true;
    s_state.stat.DSRINPUTLEVEL = false;
    s_state.sync_last_dtr = false;
    s_state.sync_last_rts = false;
    UpdateTXRX();
    return;
  }

  u8 buf[2] = {};
  bool send_reply = std::exchange(s_state.needs_initial_sync, false);
  if (s_state.connection->HasData())
  {
    while (s_state.connection->Read(buf, sizeof(buf), sizeof(buf)) != 0)
    {
      // INFO_LOG("In: {:02X} {:02X}", buf[0], buf[1]);

      const SyncByte sb{buf[0]};

      if (sb.has_data)
      {
        WARNING_LOG("Received: {:02X}", buf[1]);
        send_reply = true;

        if (s_state.data_in.IsFull())
        {
          WARNING_LOG("RX OVERRUN");
          s_state.stat.RXFIFOOVERRUN = true;
        }
        else
        {
          s_state.data_in.Push(buf[1]);
        }

        const u32 rx_threshold = 1u << s_state.ctrl.RXIMODE;
        if (s_state.ctrl.RXINTEN && !s_state.stat.INTR && s_state.data_in.GetSize() >= rx_threshold)
        {
          WARNING_LOG("Setting RX interrupt");
          SetInterrupt();
        }
      }

      if (!s_state.stat.DSRINPUTLEVEL && sb.dtr_level)
        WARNING_LOG("DSR active");
      else if (s_state.stat.DSRINPUTLEVEL && !sb.dtr_level)
        WARNING_LOG("DSR inactive");
      if (!s_state.stat.CTSINPUTLEVEL && sb.rts_level)
        WARNING_LOG("Remote RTS active, setting CTS");
      else if (s_state.stat.CTSINPUTLEVEL && !sb.rts_level)
        WARNING_LOG("Remote RTS inactive, clearing CTS");

      if (sb.dtr_level && !s_state.stat.DSRINPUTLEVEL && s_state.ctrl.DTRINTEN)
      {
        WARNING_LOG("Setting DSR interrupt");
        SetInterrupt();
      }

      s_state.stat.DSRINPUTLEVEL = sb.dtr_level;
      s_state.stat.CTSINPUTLEVEL = sb.rts_level;
      s_state.sync_remote_rx_fifo_size = sb.rx_fifo_size;
    }
  }

  const bool dtr_level = s_state.ctrl.DTROUTPUT;
  const bool rts_level = s_state.ctrl.RTSOUTPUT;
  const bool tx = (s_state.ctrl.TXEN || s_state.latched_txen) && s_state.stat.CTSINPUTLEVEL && s_state.data_out_full &&
                  s_state.sync_remote_rx_fifo_size < RX_FIFO_SIZE;
  s_state.latched_txen = s_state.ctrl.TXEN;
  if (dtr_level != s_state.sync_last_dtr || rts_level != s_state.sync_last_rts ||
      s_state.data_in.GetSize() != s_state.sync_last_fifo_size || tx || send_reply)
  {
    if (s_state.sync_last_dtr != dtr_level)
      WARNING_LOG("OUR DTR level => {}", dtr_level);

    if (s_state.sync_last_rts != rts_level)
      WARNING_LOG("OUR RTS level => {}", rts_level);

    s_state.sync_last_dtr = dtr_level;
    s_state.sync_last_rts = rts_level;
    s_state.sync_last_fifo_size = Truncate8(s_state.data_in.GetSize());

    SyncByte sb{0};
    sb.has_data = tx;
    sb.dtr_level = dtr_level;
    sb.rts_level = rts_level;
    sb.rx_fifo_size = Truncate8(s_state.data_in.GetSize());

    buf[0] = sb.bits;
    buf[1] = 0;
    if (tx)
    {
      s_state.sync_remote_rx_fifo_size++;

      WARNING_LOG("Sending: {:02X}, remote fifo now {} bytes", s_state.data_out, s_state.sync_remote_rx_fifo_size);
      buf[1] = s_state.data_out;
      s_state.data_out_full = false;

      if (s_state.ctrl.TXINTEN)
      {
        WARNING_LOG("Setting TX interrupt");
        SetInterrupt();
      }
    }

    // INFO_LOG("Out: {:02X} {:02X}", buf[0], buf[1]);
    if (s_state.connection->Write(buf, sizeof(buf)) != sizeof(buf))
      WARNING_LOG("Write failed");
  }

  UpdateTXRX();
}