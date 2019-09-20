#include "cdrom.h"
#include "YBaseLib/Log.h"
#include "common/cd_image.h"
#include "common/state_wrapper.h"
#include "interrupt_controller.h"
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
  m_state = State::Idle;
  m_status.bits = 0;
  m_secondary_status.bits = 0;
  m_interrupt_enable_register = INTERRUPT_REGISTER_MASK;
  m_interrupt_flag_register = 0;
  m_param_fifo.Clear();
  m_response_fifo.Clear();
  m_data_fifo.Clear();
  UpdateStatusRegister();

  m_secondary_status.shell_open = true;
}

bool CDROM::DoState(StateWrapper& sw)
{
  sw.Do(&m_state);
  sw.Do(&m_status.bits);
  sw.Do(&m_secondary_status.bits);
  sw.Do(&m_interrupt_enable_register);
  sw.Do(&m_interrupt_flag_register);
  sw.Do(&m_param_fifo);
  sw.Do(&m_response_fifo);
  sw.Do(&m_data_fifo);
  return !sw.HasError();
}

bool CDROM::InsertMedia(const char* filename)
{
  auto media = std::make_unique<CDImage>();
  if (!media->Open(filename))
  {
    Log_ErrorPrintf("Failed to open media at '%s'", filename);
    return false;
  }

  if (HasMedia())
    RemoveMedia();

  m_media = std::move(media);
  m_secondary_status.shell_open = false;
  return true;
}

void CDROM::RemoveMedia()
{
  if (!m_media)
    return;

  // TODO: Error while reading?
  Log_InfoPrintf("Removing CD...");
  m_media.reset();
  m_secondary_status.shell_open = true;
}

u8 CDROM::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0: // status register
      return m_status.bits;

    case 1: // always response FIFO
    {
      const u8 value = m_response_fifo.Pop();
      UpdateStatusRegister();
      return value;
    }

    case 2: // always data FIFO
    {
      const u8 value = m_data_fifo.Pop();
      UpdateStatusRegister();
      return value;
    }

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
            ExecuteCommand(static_cast<Command>(value));

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
          UpdateStatusRegister();
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

void CDROM::SetInterrupt(Interrupt interrupt)
{
  m_interrupt_flag_register = static_cast<u8>(interrupt);
  if (HasPendingInterrupt())
    m_interrupt_controller->InterruptRequest(InterruptController::IRQ::CDROM);
}

void CDROM::UpdateStatusRegister()
{
  m_status.ADPBUSY = false;
  m_status.PRMEMPTY = m_param_fifo.IsEmpty();
  m_status.PRMWRDY = m_param_fifo.IsFull();
  m_status.RSLRRDY = !m_response_fifo.IsEmpty();
  m_status.DRQSTS = !m_data_fifo.IsEmpty();
  m_status.BUSYSTS = m_state != State::Idle;
}

void CDROM::Execute() {}

void CDROM::ExecuteCommand(Command command)
{
  Log_ErrorPrintf("CDROM write command 0x%02X", ZeroExtend32(static_cast<u8>(command)));

  switch (command)
  {
    case Command::Getstat:
    {
      Log_DebugPrintf("CDROM Getstat command");

      // if bit 0 or 2 is set, send an additional byte
      m_response_fifo.Push(m_secondary_status.bits);
      SetInterrupt(Interrupt::INT3);
      UpdateStatusRegister();
    }
    break;

    case Command::Test:
    {
      const u8 subcommand = m_param_fifo.Pop();
      ExecuteTestCommand(subcommand);
    }
    break;

    default:
      Panic("Unknown command");
      break;
  }
}

void CDROM::ExecuteTestCommand(u8 subcommand)
{
  switch (subcommand)
  {
    case 0x20: // Get CDROM BIOS Date/Version
    {
      Log_DebugPrintf("Get CDROM BIOS Date/Version");
      static constexpr u8 response[] = {0x94, 0x09, 0x19, 0xC0};
      m_response_fifo.PushRange(response, countof(response));
      m_param_fifo.Clear();
      SetInterrupt(Interrupt::INT3);
      UpdateStatusRegister();
      return;
    }

    default:
    {
      Log_ErrorPrintf("Unknown test command 0x%02X", subcommand);
      return;
    }
  }
}
