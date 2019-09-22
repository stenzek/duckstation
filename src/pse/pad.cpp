#include "pad.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
#include "interrupt_controller.h"
#include "pad_device.h"
#include "system.h"
Log_SetChannel(Pad);

Pad::Pad() = default;

Pad::~Pad() = default;

bool Pad::Initialize(System* system, InterruptController* interrupt_controller)
{
  m_system = system;
  m_interrupt_controller = interrupt_controller;
  return true;
}

void Pad::Reset()
{
  SoftReset();
}

bool Pad::DoState(StateWrapper& sw)
{
  sw.Do(&m_state);
  sw.Do(&m_ticks_remaining);
  sw.Do(&m_JOY_CTRL.bits);
  sw.Do(&m_JOY_STAT.bits);
  sw.Do(&m_JOY_MODE.bits);
  sw.Do(&m_RX_FIFO);
  sw.Do(&m_TX_FIFO);
  return !sw.HasError();
}

u32 Pad::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00: // JOY_DATA
    {
      if (m_RX_FIFO.IsEmpty())
      {
        Log_WarningPrint("Read from RX fifo when empty");
        return 0;
      }

      const u8 value = m_RX_FIFO.Pop();
      UpdateJoyStat();
      Log_DebugPrintf("JOY_DATA (R) -> 0x%02X", ZeroExtend32(value));
      return ZeroExtend32(value);
    }

    case 0x04: // JOY_STAT
    {
      const u32 bits = m_JOY_STAT.bits;
      m_JOY_STAT.ACKINPUT = false;
      return bits;
    }

    case 0x08: // JOY_MODE
      return ZeroExtend32(m_JOY_MODE.bits);

    case 0x0A: // JOY_CTRL
      return ZeroExtend32(m_JOY_CTRL.bits);

    default:
      Log_ErrorPrintf("Unknown register read: 0x%X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void Pad::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00: // JOY_DATA
    {
      Log_DebugPrintf("JOY_DATA (W) <- 0x%02X", value);
      if (m_TX_FIFO.IsFull())
      {
        Log_WarningPrint("TX FIFO overrun");
        m_TX_FIFO.RemoveOne();
      }

      m_TX_FIFO.Push(Truncate8(value));

      if (!IsTransmitting() && CanTransfer())
        BeginTransfer();

      return;
    }

    case 0x0A: // JOY_CTRL
    {
      Log_DebugPrintf("JOY_CTRL <- 0x%04X", value);
      const bool old_select = m_JOY_CTRL.SELECT;

      m_JOY_CTRL.bits = Truncate16(value);
      if (m_JOY_CTRL.RESET)
        SoftReset();

      if (m_JOY_CTRL.ACK)
      {
        // reset stat bits
        m_JOY_STAT.INTR = false;
      }

      if (!m_JOY_CTRL.SELECT || !m_JOY_CTRL.TXEN)
      {
        if (IsTransmitting())
          EndTransfer();
      }
      else
      {
        if (!IsTransmitting() && CanTransfer())
          BeginTransfer();
      }

      return;
    }

    case 0x08: // JOY_MODE
    {
      Log_DebugPrintf("JOY_MODE <- 0x%08X", value);
      m_JOY_MODE.bits = Truncate16(value);
      return;
    }

    case 0x0E:
    {
      Log_WarningPrintf("JOY_BAUD <- 0x%08X", value);
      return;
    }

    default:
      Log_ErrorPrintf("Unknown register write: 0x%X <- 0x%08X", offset, value);
      return;
  }
}

void Pad::Execute(TickCount ticks)
{
  switch (m_state)
  {
    case State::Idle:
      break;

    case State::Transmitting:
    {
      m_ticks_remaining -= ticks;
      if (m_ticks_remaining <= 0)
        DoTransfer();
    }
    break;
  }
}

void Pad::SoftReset()
{
  if (IsTransmitting())
    EndTransfer();

  m_JOY_CTRL.bits = 0;
  m_JOY_STAT.bits = 0;
  m_JOY_MODE.bits = 0;
  m_RX_FIFO.Clear();
  m_TX_FIFO.Clear();
  UpdateJoyStat();
}

void Pad::UpdateJoyStat()
{
  m_JOY_STAT.RXFIFONEMPTY = !m_RX_FIFO.IsEmpty();
  m_JOY_STAT.TXDONE = m_TX_FIFO.IsEmpty();
  m_JOY_STAT.TXRDY = !m_TX_FIFO.IsFull();
}

void Pad::BeginTransfer()
{
  DebugAssert(m_state == State::Idle && CanTransfer());
  Log_DebugPrintf("Starting transfer");

  m_JOY_CTRL.RXEN = true;

  // The transfer or the interrupt must be delayed, otherwise the BIOS thinks there's no device detected.
  // It seems to do something resembling the following:
  //  1) Sets the control register up for transmitting, interrupt on ACK.
  //  2) Writes 0x01 to the TX FIFO.
  //  3) Delays for a bit.
  //  4) Writes ACK to the control register, clearing the interrupt flag.
  //  5) Clears IRQ7 in the interrupt controller.
  //  6) Waits until the RX FIFO is not empty, reads the first byte to $zero.
  //  7) Checks if the interrupt status register had IRQ7 set. If not, no device connected.
  //
  // Performing the transfer immediately will result in both the INTR bit and the bit in the interrupt
  // controller being discarded in (4)/(5), but this bit was set by the *new* transfer. Therefore, the
  // test in (7) will fail, and it won't send any more data. So, the transfer/interrupt must be delayed
  // until after (4) and (5) have been completed.

  m_system->Synchronize();
  m_state = State::Transmitting;
  m_ticks_remaining = TRANSFER_TICKS;
  m_system->SetDowncount(m_ticks_remaining);
}

void Pad::DoTransfer()
{
  Log_DebugPrintf("Transferring slot %d", m_JOY_CTRL.SLOT.GetValue());

  const std::shared_ptr<PadDevice>& dev = m_devices[m_JOY_CTRL.SLOT];
  if (!dev || !CanTransfer())
  {
    // no device present, don't set ACK and read hi-z
    m_TX_FIFO.Clear();
    m_RX_FIFO.Clear();
    m_RX_FIFO.Push(0xFF);
    UpdateJoyStat();
    EndTransfer();
    return;
  }

  // set rx?
  m_JOY_CTRL.RXEN = true;

  const u8 data_out = m_TX_FIFO.Pop();
  u8 data_in;
  m_JOY_STAT.ACKINPUT |= dev->Transfer(data_out, &data_in);
  m_RX_FIFO.Push(data_in);

  if (m_JOY_STAT.ACKINPUT && m_JOY_CTRL.ACKINTEN)
  {
    Log_DebugPrintf("Triggering interrupt");
    m_JOY_STAT.INTR = true;
    m_interrupt_controller->InterruptRequest(InterruptController::IRQ::IRQ7);
  }

  if (m_TX_FIFO.IsEmpty())
  {
    EndTransfer();
  }
  else
  {
    // queue the next byte
    m_ticks_remaining += TRANSFER_TICKS;
    m_system->SetDowncount(m_ticks_remaining);
  }

  UpdateJoyStat();
}

void Pad::EndTransfer()
{
  DebugAssert(m_state == State::Transmitting);
  Log_DebugPrintf("Ending transfer");

  m_state = State::Idle;
  m_ticks_remaining = 0;
}
