#include "pad.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
#include "interrupt_controller.h"
#include "pad_device.h"
Log_SetChannel(Pad);

Pad::Pad() = default;

Pad::~Pad() = default;

bool Pad::Initialize(InterruptController* interrupt_controller)
{
  m_interrupt_controller = interrupt_controller;
  return true;
}

void Pad::Reset()
{
  SoftReset();
}

bool Pad::DoState(StateWrapper& sw)
{
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
      return m_JOY_STAT.bits;

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

      if (m_JOY_CTRL.SELECT)
        DoTransfer();

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
        m_JOY_STAT.ACKINPUTLEVEL = false;
        m_JOY_STAT.INTR = false;
        m_JOY_CTRL.ACK = true;
      }

      if (!old_select && m_JOY_CTRL.SELECT && !m_TX_FIFO.IsEmpty())
        DoTransfer();

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

void Pad::SoftReset()
{
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

void Pad::DoTransfer()
{
  Log_DebugPrintf("Transferring slot %d", m_JOY_CTRL.SLOT.GetValue());

  const std::shared_ptr<PadDevice>& dev = m_devices[m_JOY_CTRL.SLOT];
  if (!dev)
  {
    // no device present, don't set ACK and read hi-z
    m_TX_FIFO.Clear();
    m_RX_FIFO.Clear();
    m_RX_FIFO.Push(0xFF);
    UpdateJoyStat();
    return;
  }

  while (!m_TX_FIFO.IsEmpty())
  {
    const u8 data_out = m_TX_FIFO.Pop();
    u8 data_in;
    m_JOY_STAT.ACKINPUTLEVEL |= dev->Transfer(data_out, &data_in);
    m_RX_FIFO.Push(data_in);
    m_JOY_CTRL.RXEN = true;
  }

  if (m_JOY_STAT.ACKINPUTLEVEL && m_JOY_CTRL.ACKINTEN)
  {
    m_JOY_STAT.INTR = true;
    m_interrupt_controller->InterruptRequest(InterruptController::IRQ::IRQ7);
  }

  UpdateJoyStat();
}
