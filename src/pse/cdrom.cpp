#include "cdrom.h"
#include "YBaseLib/Log.h"
#include "common/cd_image.h"
#include "common/state_wrapper.h"
#include "interrupt_controller.h"
#include "system.h"
Log_SetChannel(CDROM);

CDROM::CDROM() = default;

CDROM::~CDROM() = default;

bool CDROM::Initialize(System* system, DMA* dma, InterruptController* interrupt_controller)
{
  m_system = system;
  m_dma = dma;
  m_interrupt_controller = interrupt_controller;
  return true;
}

void CDROM::Reset()
{
  m_command_state = CommandState::Idle;
  m_status.bits = 0;
  m_secondary_status.bits = 0;
  m_interrupt_enable_register = INTERRUPT_REGISTER_MASK;
  m_interrupt_flag_register = 0;
  m_param_fifo.Clear();
  m_response_fifo.Clear();
  m_data_fifo.Clear();
  UpdateStatusRegister();
}

bool CDROM::DoState(StateWrapper& sw)
{
  sw.Do(&m_command_state);
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
  // m_secondary_status.shell_open = false;
  return true;
}

void CDROM::RemoveMedia()
{
  if (!m_media)
    return;

  // TODO: Error while reading?
  Log_InfoPrintf("Removing CD...");
  m_media.reset();
  // m_secondary_status.shell_open = true;
}

u8 CDROM::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0: // status register
      Log_DebugPrintf("CDROM read status register <- 0x%08X", m_status.bits);
      return m_status.bits;

    case 1: // always response FIFO
    {
      const u8 value = m_response_fifo.Pop();
      UpdateStatusRegister();
      Log_DebugPrintf("CDROM read response FIFO <- 0x%08X", ZeroExtend32(value));
      return value;
    }

    case 2: // always data FIFO
    {
      const u8 value = m_data_fifo.Pop();
      UpdateStatusRegister();
      Log_DebugPrintf("CDROM read data FIFO <- 0x%08X", ZeroExtend32(value));
      return value;
    }

    case 3:
    {
      switch (m_status.index)
      {
        case 0:
        case 2:
          Log_DebugPrintf("CDROM read interrupt enable register <- 0x%02X",
                          ZeroExtend32(m_interrupt_enable_register | ~INTERRUPT_REGISTER_MASK));
          return m_interrupt_enable_register | ~INTERRUPT_REGISTER_MASK;

        case 1:
        case 3:
          Log_DebugPrintf("CDROM read interrupt flag register <- 0x%02X", ZeroExtend32(m_interrupt_flag_register));
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
          if (m_command_state == CommandState::Idle)
            BeginCommand(static_cast<Command>(value));
          else
            Log_ErrorPrintf("Ignoring write (0x%02X) to command register in non-idle state", ZeroExtend32(value));

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
          Log_DebugPrintf("Request register <- 0x%02X", value);
          const RequestRegister rr{value};
          // if (!rr.BFRD)
          // m_data_fifo.Clear();

          return;
        }

        case 1:
        {
          Log_DebugPrintf("Interrupt flag register <- 0x%02X", value);
          m_interrupt_flag_register &= ~(value & INTERRUPT_REGISTER_MASK);
          if (m_interrupt_flag_register == 0 && m_command_state == CommandState::WaitForIRQClear)
          {
            m_system->Synchronize();
            m_command_state = CommandState::WaitForExecute;
            m_system->SetDowncount(m_command_remaining_ticks);
          }

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

u32 CDROM::DMARead()
{
  if (m_data_fifo.IsEmpty())
  {
    Log_ErrorPrintf("DMA read on empty data FIFO");
    return UINT32_C(0xFFFFFFFF);
  }

  u32 data;
  if (m_data_fifo.GetSize() >= sizeof(data))
  {
    std::memcpy(&data, m_data_fifo.GetFrontPointer(), sizeof(data));
    m_data_fifo.Remove(sizeof(data));
  }
  else
  {
    Log_WarningPrintf("Unaligned DMA read on FIFO(%u)", m_data_fifo.GetSize());
    data = 0;
    std::memcpy(&data, m_data_fifo.GetFrontPointer(), m_data_fifo.GetSize());
    m_data_fifo.Clear();
  }

  // Log_DebugPrintf("DMA Read -> 0x%08X", data);
  return data;
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
  m_status.PRMWRDY = !m_param_fifo.IsFull();
  m_status.RSLRRDY = !m_response_fifo.IsEmpty();
  m_status.DRQSTS = !m_data_fifo.IsEmpty();
  m_status.BUSYSTS = m_command_state != CommandState::Idle;
}

u32 CDROM::GetTicksForCommand() const
{
  return 100;
}

void CDROM::Execute(TickCount ticks)
{
  switch (m_command_state)
  {
    case CommandState::Idle:
    case CommandState::WaitForIRQClear:
      break;

    case CommandState::WaitForExecute:
    {
      m_command_remaining_ticks -= ticks;
      if (m_command_remaining_ticks <= 0)
        ExecuteCommand();
    }
    break;

    default:
      UnreachableCode();
      break;
  }

  if (m_reading)
  {
    m_sector_read_remaining_ticks -= ticks;
    if (m_sector_read_remaining_ticks <= 0)
      DoSectorRead();
  }
}

void CDROM::BeginCommand(Command command)
{
  m_response_fifo.Clear();

  m_system->Synchronize();
  m_command = command;
  m_command_stage = 0;
  m_command_remaining_ticks = GetTicksForCommand();
  m_command_state = CommandState::WaitForExecute;
  m_system->SetDowncount(m_command_remaining_ticks);
  UpdateStatusRegister();
}

void CDROM::NextCommandStage(bool wait_for_irq, u32 time)
{
  // prevent re-execution when synchronizing below
  m_command_state = CommandState::WaitForIRQClear;
  m_command_remaining_ticks = time;
  m_command_stage++;
  if (wait_for_irq)
    return;

  m_system->Synchronize();
  m_command_state = CommandState::WaitForExecute;
  m_system->SetDowncount(m_command_remaining_ticks);
  UpdateStatusRegister();
}

void CDROM::EndCommand()
{
  m_param_fifo.Clear();

  m_command_state = CommandState::Idle;
  m_command = Command::Sync;
  m_command_stage = 0;
  m_command_remaining_ticks = 0;
  UpdateStatusRegister();
}

void CDROM::ExecuteCommand()
{
  Log_DevPrintf("CDROM executing command 0x%02X stage %u", ZeroExtend32(static_cast<u8>(m_command)), m_command_stage);

  switch (m_command)
  {
    case Command::Getstat:
    {
      Log_DebugPrintf("CDROM Getstat command");

      // if bit 0 or 2 is set, send an additional byte
      m_response_fifo.Push(m_secondary_status.bits);
      SetInterrupt(Interrupt::INT3);
      EndCommand();
      return;
    }

    case Command::Test:
    {
      const u8 subcommand = m_param_fifo.Pop();
      ExecuteTestCommand(subcommand);
      return;
    }

    case Command::GetID:
    {
      Log_DebugPrintf("CDROM GetID command - stage %u", m_command_stage);
      if (m_command_stage == 0)
      {
        if (!HasMedia())
        {
          static constexpr u8 response[] = {0x11, 0x80};
          m_response_fifo.PushRange(response, countof(response));
          SetInterrupt(Interrupt::INT5);
          EndCommand();
        }
        else
        {
          // INT3(stat), ...
          m_response_fifo.Push(m_secondary_status.bits);
          SetInterrupt(Interrupt::INT3);
          NextCommandStage(true, GetTicksForCommand());
        }
      }
      else
      {
        static constexpr u8 response2[] = {0x02, 0x00, 0x20, 0x00, 0x53, 0x43, 0x45, 0x41}; // last byte is 0x49 for EU
        m_response_fifo.PushRange(response2, countof(response2));
        SetInterrupt(Interrupt::INT2);
        EndCommand();
      }

      return;
    }

    case Command::Setloc:
    {
      // TODO: Verify parameter count
      m_setloc.minute = BCDToDecimal(m_param_fifo.Peek(0));
      m_setloc.second = BCDToDecimal(m_param_fifo.Peek(1));
      m_setloc.frame = BCDToDecimal(m_param_fifo.Peek(2));
      Log_DebugPrintf("CDROM setloc command (%u, %u, %u)", ZeroExtend32(m_setloc.minute), ZeroExtend32(m_setloc.second),
                      ZeroExtend32(m_setloc.frame));
      m_response_fifo.Push(m_secondary_status.bits);
      SetInterrupt(Interrupt::INT3);
      EndCommand();
      return;
    }

    case Command::SeekL:
    case Command::SeekP:
    {
      // TODO: Data vs audio mode
      Log_DebugPrintf("CDROM seek command");

      if (m_command_stage == 0)
      {
        StopReading();
        if (!m_media || !m_media->Seek(m_setloc.minute, m_setloc.second - 2 /* pregap */, m_setloc.frame))
        {
          Panic("Error in Setloc command");
          return;
        }

        m_secondary_status.motor_on = true;
        m_secondary_status.seeking = true;
        m_response_fifo.Push(m_secondary_status.bits);
        SetInterrupt(Interrupt::INT3);
        NextCommandStage(false, 100);
      }
      else
      {
        m_secondary_status.seeking = false;
        m_response_fifo.Push(m_secondary_status.bits);
        SetInterrupt(Interrupt::INT2);
        EndCommand();
      }

      return;
    }

    case Command::Setmode:
    {
      const u8 mode = m_param_fifo.Peek(0);
      Log_DebugPrintf("CDROM setmode command 0x%02X", ZeroExtend32(mode));
      StopReading();

      m_mode.bits = mode;
      m_response_fifo.Push(m_secondary_status.bits);
      SetInterrupt(Interrupt::INT3);
      EndCommand();
      return;
    }

    case Command::ReadN:
    {
      Log_DebugPrintf("CDROM read command");
      StopReading();
      EndCommand();
      BeginReading();
      m_response_fifo.Push(m_secondary_status.bits);
      SetInterrupt(Interrupt::INT3);
      return;
    }

    case Command::Pause:
    {
      if (m_command_stage == 0)
      {
        Log_DebugPrintf("CDROM pause command");
        m_response_fifo.Push(m_secondary_status.bits);
        SetInterrupt(Interrupt::INT3);
        StopReading();
        NextCommandStage(true, 1000);
      }
      else
      {
        m_response_fifo.Push(m_secondary_status.bits);
        SetInterrupt(Interrupt::INT2);
        EndCommand();
      }

      return;
    }

    case Command::Init:
    {
      if (m_command_stage == 0)
      {
        Log_DebugPrintf("CDROM init command");
        m_response_fifo.Push(m_secondary_status.bits);
        SetInterrupt(Interrupt::INT3);
        StopReading();
        NextCommandStage(true, 1000);
      }
      else
      {
        m_response_fifo.Push(m_secondary_status.bits);
        SetInterrupt(Interrupt::INT2);
        EndCommand();
      }

      return;
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
      SetInterrupt(Interrupt::INT3);
      EndCommand();
      return;
    }

    case 0x22:
    {
      Log_DebugPrintf("Get CDROM region ID string");
      static constexpr u8 response[] = {'f', 'o', 'r', ' ', 'U', '/', 'C'};
      m_response_fifo.PushRange(response, countof(response));
      SetInterrupt(Interrupt::INT3);
      EndCommand();
      return;
    }

    default:
    {
      Log_ErrorPrintf("Unknown test command 0x%02X", subcommand);
      return;
    }
  }
}

void CDROM::BeginReading()
{
  Log_DebugPrintf("Starting reading");
  m_system->Synchronize();

  m_secondary_status.motor_on = true;
  m_secondary_status.seeking = false;
  m_secondary_status.reading = true;

  m_reading = true;
  m_sector_read_remaining_ticks = 4000;
  m_system->SetDowncount(m_sector_read_remaining_ticks);
  UpdateStatusRegister();
}

void CDROM::DoSectorRead()
{
  if (HasPendingInterrupt())
  {
    // can't read with a pending interrupt?
    Log_WarningPrintf("Missed sector read...");
    //m_sector_read_remaining_ticks += 10;
    //m_system->SetDowncount(m_sector_read_remaining_ticks);
    //return;
  }

  Log_DebugPrintf("Reading sector %llu", m_media->GetCurrentLBA());

  // TODO: Error handling
  u8 buffer[CDImage::RAW_SECTOR_SIZE];
  m_media->Read(m_mode.read_raw_sector ? CDImage::ReadMode::RawNoSync : CDImage::ReadMode::DataOnly, 1, buffer);
  m_data_fifo.Clear();
  m_data_fifo.PushRange(buffer, m_mode.read_raw_sector ? CDImage::RAW_SECTOR_SIZE : CDImage::DATA_SECTOR_SIZE);
  m_response_fifo.Push(m_secondary_status.bits);
  SetInterrupt(Interrupt::INT1);
  UpdateStatusRegister();

  m_sector_read_remaining_ticks += 4000;
  m_system->SetDowncount(m_sector_read_remaining_ticks);
}

void CDROM::StopReading()
{
  if (!m_reading)
    return;

  Log_DebugPrintf("Stopping reading");
  m_secondary_status.reading = false;
  m_reading = false;
}
