#include "cdrom.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
Log_SetChannel(CDROM);

CDROM::CDROM() = default;

CDROM::~CDROM() = default;

bool CDROM::Initialize(DMA* dma, InterruptController* interrupt_controller)
{
  m_dma = dma;
  m_interrupt_controller = interrupt_controller;
  return true;
}

void CDROM::Reset()
{
  m_param_fifo.Clear();
  m_response_fifo.Clear();
  m_data_fifo.Clear();
}

bool CDROM::DoState(StateWrapper& sw)
{
  sw.Do(&m_state);
  sw.Do(&m_status.bits);
  sw.Do(&m_param_fifo);
  sw.Do(&m_response_fifo);
  sw.Do(&m_data_fifo);
  return !sw.HasError();
}

u8 CDROM::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0: // status register
      return m_status.bits;

    case 1: // always response FIFO
      return m_response_fifo.Pop();

    case 2: // always data FIFO
      return m_data_fifo.Pop();

    case 3:
    {
      switch (m_status.index)
      {
        case 0:
        case 2:
          return m_interrupt_enable_register | ~INTERRUPT_REGISTER_MASK;

        case 1:
        case 3:
          return m_interrupt_flag_register;
      }
    }
    break;
  }

  Log_ErrorPrintf("Unknown CDROM register read: offset=0x%02X, index=%d", offset,
                  ZeroExtend32(m_status.index.GetValue()));
  Panic("Unknown CDROM register");
  return 0;
}

void CDROM::WriteRegister(u32 offset, u8 value)
{
  switch (offset)
  {
    case 0:
    {
      Log_DebugPrintf("CDROM status register <- 0x%02X", ZeroExtend32(value));
      m_status.bits = (m_status.bits & static_cast<u8>(~3)) | (value & u8(3));
      return;
    }
    break;

    case 1:
    {
      switch (m_status.index)
      {
        case 0:
        {
          Log_DebugPrintf("CDROM command register <- 0x%02X", ZeroExtend32(value));
          if (m_state != State::Idle)
            Log_ErrorPrintf("Ignoring write (0x%02X) to command register in non-idle state", ZeroExtend32(value));
          else
            WriteCommand(value);

          return;
        }

        case 1:
        {
          Log_ErrorPrintf("Sound map data out <- 0x%02X", ZeroExtend32(value));
          return;
        }

        case 2:
        {
          Log_ErrorPrintf("Sound map coding info <- 0x%02X", ZeroExtend32(value));
          return;
        }

        case 3:
        {
          Log_ErrorPrintf("Audio volume for right-to-left output <- 0x%02X", ZeroExtend32(value));
          return;
        }
      }
    }
    break;

    case 2:
    {
      switch (m_status.index)
      {
        case 0:
        {
          if (m_param_fifo.IsFull())
          {
            Log_WarningPrintf("Parameter FIFO overflow");
            m_param_fifo.RemoveOne();
          }

          m_param_fifo.Push(value);
          return;
        }

        case 1:
        {
          Log_DebugPrintf("Interrupt enable register <- 0x%02X", ZeroExtend32(value));
          m_interrupt_enable_register = value & INTERRUPT_REGISTER_MASK;
          return;
        }

        case 2:
        {
          Log_ErrorPrintf("Audio volume for left-to-left output <- 0x%02X", ZeroExtend32(value));
          return;
        }

        case 3:
        {
          Log_ErrorPrintf("Audio volume for right-to-left output <- 0x%02X", ZeroExtend32(value));
          return;
        }
      }
    }
    break;

    case 3:
    {
      switch (m_status.index)
      {
        case 0:
        {
          Log_ErrorPrintf("Request register <- 0x%02X", value);
          return;
        }

        case 1:
        {
          Log_DebugPrintf("Interrupt flag register <- 0x%02X", value);
          m_interrupt_flag_register &= ~(value & INTERRUPT_REGISTER_MASK);
          Execute();
          return;
        }

        case 2:
        {
          Log_ErrorPrintf("Audio volume for left-to-right output <- 0x%02X", ZeroExtend32(value));
          return;
        }

        case 3:
        {
          Log_ErrorPrintf("Audio volume apply changes <- 0x%02X", ZeroExtend32(value));
          return;
        }
      }
    }
    break;
  }

  Log_ErrorPrintf("Unknown CDROM register write: offset=0x%02X, index=%d, value=0x%02X", offset,
                  ZeroExtend32(m_status.index.GetValue()), ZeroExtend32(value));
}

void CDROM::Execute() {}

void CDROM::WriteCommand(u8 command)
{
  Log_ErrorPrintf("CDROM write command 0x%02X", ZeroExtend32(command));
}
