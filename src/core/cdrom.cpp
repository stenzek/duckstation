#include "cdrom.h"
#include "YBaseLib/Log.h"
#include "common/cd_image.h"
#include "common/state_wrapper.h"
#include "interrupt_controller.h"
#include "system.h"
Log_SetChannel(CDROM);

CDROM::CDROM() : m_sector_buffer(SECTOR_BUFFER_SIZE) {}

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
  if (m_media)
    m_media->Seek(0);

  SoftReset();
}

void CDROM::SoftReset()
{
  m_command_state = CommandState::Idle;
  m_command = Command::Sync;
  m_command_stage = 0;
  m_command_remaining_ticks = 0;
  m_sector_read_remaining_ticks = 0;
  m_reading = false;
  m_muted = false;
  m_status.bits = 0;
  m_secondary_status.bits = 0;
  m_mode.bits = 0;
  m_setloc = {};
  m_setloc_dirty = false;
  m_last_sector_header = {};
  m_last_sector_subheader = {};
  m_interrupt_enable_register = INTERRUPT_REGISTER_MASK;
  m_interrupt_flag_register = 0;
  m_param_fifo.Clear();
  m_response_fifo.Clear();
  m_data_fifo.Clear();
  m_sector_buffer.clear();
  UpdateStatusRegister();
}

bool CDROM::DoState(StateWrapper& sw)
{
  sw.Do(&m_command);
  sw.Do(&m_command_stage);
  sw.Do(&m_command_remaining_ticks);
  sw.Do(&m_sector_read_remaining_ticks);
  sw.Do(&m_reading);
  sw.Do(&m_muted);
  sw.Do(&m_setloc.minute);
  sw.Do(&m_setloc.second);
  sw.Do(&m_setloc.frame);
  sw.Do(&m_setloc_dirty);
  sw.Do(&m_command_state);
  sw.Do(&m_status.bits);
  sw.Do(&m_secondary_status.bits);
  sw.Do(&m_mode.bits);
  sw.DoPOD(&m_setloc);
  sw.Do(&m_setloc_dirty);
  sw.DoPOD(&m_last_sector_header);
  sw.DoPOD(&m_last_sector_subheader);
  sw.Do(&m_interrupt_enable_register);
  sw.Do(&m_interrupt_flag_register);
  sw.Do(&m_param_fifo);
  sw.Do(&m_response_fifo);
  sw.Do(&m_data_fifo);
  sw.Do(&m_sector_buffer);

  u64 media_lba = m_media ? m_media->GetCurrentLBA() : 0;
  sw.Do(&m_media_filename);
  sw.Do(&media_lba);

  if (sw.IsReading())
  {
    if (m_command_state == CommandState::WaitForExecute)
      m_system->SetDowncount(m_command_remaining_ticks);
    if (m_reading)
      m_system->SetDowncount(m_sector_read_remaining_ticks);

    // load up media if we had something in there before
    m_media.reset();
    if (!m_media_filename.empty())
    {
      m_media = std::make_unique<CDImage>();
      if (!m_media->Open(m_media_filename.c_str()) || !m_media->Seek(media_lba))
      {
        Log_ErrorPrintf("Failed to re-insert CD media from save state: '%s'. Ejecting.", m_media_filename.c_str());
        RemoveMedia();
      }
    }
  }

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
  m_media_filename = filename;
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
  m_media_filename.clear();
  // m_secondary_status.shell_open = true;
}

u8 CDROM::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0: // status register
      Log_TracePrintf("CDROM read status register <- 0x%08X", m_status.bits);
      return m_status.bits;

    case 1: // always response FIFO
    {
      if (m_response_fifo.IsEmpty())
      {
        Log_DebugPrintf("Response FIFO empty on read");
        return 0xFF;
      }

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
        {
          const u8 value = m_interrupt_enable_register | ~INTERRUPT_REGISTER_MASK;
          Log_DebugPrintf("CDROM read interrupt enable register <- 0x%02X", ZeroExtend32(value));
          return value;
        }

        case 1:
        case 3:
        {
          const u8 value = m_interrupt_flag_register | ~INTERRUPT_REGISTER_MASK;
          Log_DebugPrintf("CDROM read interrupt flag register <- 0x%02X", ZeroExtend32(value));
          return value;
        }
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
      Log_TracePrintf("CDROM status register <- 0x%02X", ZeroExtend32(value));
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
          // TODO: sector buffer is not the data fifo
          Log_DebugPrintf("Request register <- 0x%02X", value);
          const RequestRegister rr{value};
          Assert(!rr.SMEN);
          if (rr.BFRD)
          {
            LoadDataFIFO();
          }
          else
          {
            Log_DebugPrintf("Clearing data FIFO");
            m_data_fifo.Clear();
          }

          UpdateStatusRegister();
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

          // Bit 6 clears the parameter FIFO.
          if (value & 0x40)
          {
            m_param_fifo.Clear();
            UpdateStatusRegister();
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

  // Log_DebugPrintf("DMA Read -> 0x%08X (%u remaining)", data, m_data_fifo.GetSize());
  return data;
}

void CDROM::SetInterrupt(Interrupt interrupt)
{
  m_interrupt_flag_register = static_cast<u8>(interrupt);
  if (HasPendingInterrupt())
    m_interrupt_controller->InterruptRequest(InterruptController::IRQ::CDROM);
}

void CDROM::PushStatResponse(Interrupt interrupt /*= Interrupt::ACK*/)
{
  m_response_fifo.Push(m_secondary_status.bits);
  SetInterrupt(interrupt);
}

void CDROM::UpdateStatusRegister()
{
  m_status.ADPBUSY = false;
  m_status.PRMEMPTY = m_param_fifo.IsEmpty();
  m_status.PRMWRDY = !m_param_fifo.IsFull();
  m_status.RSLRRDY = !m_response_fifo.IsEmpty();
  m_status.DRQSTS = !m_data_fifo.IsEmpty();
  m_status.BUSYSTS = m_command_state == CommandState::WaitForExecute;
}

u32 CDROM::GetAckDelayForCommand() const
{
  const u32 default_ack_delay = 20000;
  if (m_command == Command::Init)
    return 60000;
  else
    return default_ack_delay;
}

u32 CDROM::GetTicksForRead() const
{
  return m_mode.double_speed ? (MASTER_CLOCK / 150) : (MASTER_CLOCK / 75);
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
      else
        m_system->SetDowncount(m_command_remaining_ticks);
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
    else
      m_system->SetDowncount(m_sector_read_remaining_ticks);
  }
}

void CDROM::BeginCommand(Command command)
{
  m_response_fifo.Clear();
  m_system->Synchronize();

  m_command = command;
  m_command_stage = 0;
  m_command_remaining_ticks = GetAckDelayForCommand();
  if (m_command_remaining_ticks == 0)
  {
    ExecuteCommand();
  }
  else
  {
    m_command_state = CommandState::WaitForExecute;
    m_system->SetDowncount(m_command_remaining_ticks);
    UpdateStatusRegister();
  }
}

void CDROM::NextCommandStage(bool wait_for_irq, u32 time)
{
  // prevent re-execution when synchronizing below
  m_command_state = CommandState::WaitForIRQClear;
  m_command_remaining_ticks = time;
  m_command_stage++;
  UpdateStatusRegister();
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
      SetInterrupt(Interrupt::ACK);
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
          SetInterrupt(Interrupt::ACK);
          NextCommandStage(true, 18000);
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
      m_setloc_dirty = true;
      Log_DebugPrintf("CDROM setloc command (%u, %u, %u)", ZeroExtend32(m_setloc.minute), ZeroExtend32(m_setloc.second),
                      ZeroExtend32(m_setloc.frame));
      m_response_fifo.Push(m_secondary_status.bits);
      SetInterrupt(Interrupt::ACK);
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
        Assert(m_setloc_dirty);
        StopReading();
        if (!m_media || !m_media->Seek(m_setloc.minute, m_setloc.second, m_setloc.frame))
        {
          Panic("Error in Setloc command");
          return;
        }

        m_setloc_dirty = false;
        m_secondary_status.motor_on = true;
        m_secondary_status.seeking = true;
        m_response_fifo.Push(m_secondary_status.bits);
        SetInterrupt(Interrupt::ACK);
        NextCommandStage(false, 20000);
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

    case Command::Setfilter:
    {
      const u8 file = m_param_fifo.Peek(0);
      const u8 channel = m_param_fifo.Peek(1);
      Log_WarningPrintf("CDROM setfilter command 0x%02X 0x%02X", ZeroExtend32(file), ZeroExtend32(channel));
      PushStatResponse(Interrupt::ACK);
      EndCommand();
      return;
    }

    case Command::Setmode:
    {
      const u8 mode = m_param_fifo.Peek(0);
      Log_DebugPrintf("CDROM setmode command 0x%02X", ZeroExtend32(mode));

      m_mode.bits = mode;
      m_response_fifo.Push(m_secondary_status.bits);
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case Command::ReadN:
    case Command::ReadS:
    {
      Log_DebugPrintf("CDROM read command");

      // TODO: Seek timing and clean up...
      if (m_setloc_dirty)
      {
        if (!m_media || !m_media->Seek(m_setloc.minute, m_setloc.second, m_setloc.frame))
        {
          Panic("Seek error");
        }
        m_setloc_dirty = false;
      }

      EndCommand();
      BeginReading();
      m_response_fifo.Push(m_secondary_status.bits);
      SetInterrupt(Interrupt::ACK);
      return;
    }

    case Command::Pause:
    {
      if (m_command_stage == 0)
      {
        const bool was_reading = m_reading;
        Log_DebugPrintf("CDROM pause command");
        m_response_fifo.Push(m_secondary_status.bits);
        SetInterrupt(Interrupt::ACK);
        StopReading();
        NextCommandStage(true, was_reading ? (m_mode.double_speed ? 2000000 : 1000000) : 7000);
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
        SetInterrupt(Interrupt::ACK);
        StopReading();
        NextCommandStage(true, 8000);
      }
      else
      {
        m_mode.bits = 0;
        m_secondary_status.bits = 0;
        m_secondary_status.motor_on = true;
        m_response_fifo.Push(m_secondary_status.bits);
        SetInterrupt(Interrupt::INT2);
        EndCommand();
      }

      return;
    }
    break;

    case Command::Demute:
    {
      Log_DebugPrintf("CDROM demute command");
      m_muted = false;
      m_response_fifo.Push(m_secondary_status.bits);
      SetInterrupt(Interrupt::ACK);
      EndCommand();
    }
    break;

    case Command::GetlocL:
    {
      Log_DebugPrintf("CDROM GetlocL command");
      m_response_fifo.PushRange(reinterpret_cast<const u8*>(&m_last_sector_header), sizeof(m_last_sector_header));
      m_response_fifo.PushRange(reinterpret_cast<const u8*>(&m_last_sector_subheader), sizeof(m_last_sector_subheader));
      SetInterrupt(Interrupt::ACK);
      EndCommand();
    }
    break;

    case Command::GetlocP:
    {
      // TODO: Subchannel Q support..
      Log_DebugPrintf("CDROM GetlocP command");
      m_response_fifo.Push(1);                                         // track number
      m_response_fifo.Push(1);                                         // index
      m_response_fifo.Push(DecimalToBCD(m_last_sector_header.minute)); // minute within track
      m_response_fifo.Push(DecimalToBCD(m_last_sector_header.second)); // second within track
      m_response_fifo.Push(DecimalToBCD(m_last_sector_header.frame));  // frame within track
      m_response_fifo.Push(DecimalToBCD(m_last_sector_header.minute)); // minute on entire disc
      m_response_fifo.Push(DecimalToBCD(m_last_sector_header.second)); // second on entire disc
      m_response_fifo.Push(DecimalToBCD(m_last_sector_header.frame));  // frame on entire disc
      SetInterrupt(Interrupt::ACK);
      EndCommand();
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
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case 0x22:
    {
      Log_DebugPrintf("Get CDROM region ID string");
      static constexpr u8 response[] = {'f', 'o', 'r', ' ', 'U', '/', 'C'};
      m_response_fifo.PushRange(response, countof(response));
      SetInterrupt(Interrupt::ACK);
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

  m_secondary_status.motor_on = true;
  m_secondary_status.seeking = false;
  m_secondary_status.reading = true;

  m_reading = true;
  m_sector_read_remaining_ticks = GetTicksForRead();
  m_system->SetDowncount(m_sector_read_remaining_ticks);
  UpdateStatusRegister();
}

void CDROM::DoSectorRead()
{
  if (HasPendingInterrupt())
  {
    // can't read with a pending interrupt?
    Log_WarningPrintf("Missed sector read...");
    // m_sector_read_remaining_ticks += 10;
    // m_system->SetDowncount(m_sector_read_remaining_ticks);
    // return;
  }
  if (!m_sector_buffer.empty())
  {
    Log_WarningPrintf("Sector buffer was not empty");
  }

  // TODO: Error handling
  // TODO: Sector buffer should be two sectors?
  Assert(!m_mode.ignore_bit);
  m_sector_buffer.resize(RAW_SECTOR_SIZE);
  m_media->Read(CDImage::ReadMode::RawSector, 1, m_sector_buffer.data());
  std::memcpy(&m_last_sector_header, &m_sector_buffer[SECTOR_SYNC_SIZE], sizeof(m_last_sector_header));
  std::memcpy(&m_last_sector_subheader, &m_sector_buffer[SECTOR_SYNC_SIZE + sizeof(m_last_sector_header)],
              sizeof(m_last_sector_subheader));
  Log_DevPrintf("Read sector %llu: mode %u submode 0x%02X", m_media->GetCurrentLBA(),
                ZeroExtend32(m_last_sector_header.sector_mode), ZeroExtend32(m_last_sector_subheader.submode.bits));

  bool pass_to_cpu = true;
  if (m_mode.xa_enable && m_last_sector_header.sector_mode == 2)
  {
    if (m_last_sector_subheader.submode.realtime && m_last_sector_subheader.submode.audio)
    {
      // TODO: Decode audio sector. Do we still transfer this to the CPU?
      Log_WarningPrintf("Decode CD-XA audio sector");
      m_sector_buffer.clear();
      pass_to_cpu = false;
    }

    if (m_last_sector_subheader.submode.eof)
    {
      Log_WarningPrintf("End of CD-XA file");
    }
  }

  if (pass_to_cpu)
  {
    m_response_fifo.Push(m_secondary_status.bits);
    SetInterrupt(Interrupt::INT1);
    UpdateStatusRegister();
  }

  m_sector_read_remaining_ticks += GetTicksForRead();
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

void CDROM::LoadDataFIFO()
{
  // any data to load?
  if (m_sector_buffer.empty())
  {
    Log_DevPrintf("Attempting to load empty sector buffer");
    return;
  }

  if (m_mode.read_raw_sector)
  {
    m_data_fifo.PushRange(m_sector_buffer.data() + CDImage::SECTOR_SYNC_SIZE,
                          CDImage::RAW_SECTOR_SIZE - CDImage::SECTOR_SYNC_SIZE);
  }
  else
  {
    m_data_fifo.PushRange(m_sector_buffer.data() + CDImage::SECTOR_SYNC_SIZE + 12, CDImage::DATA_SECTOR_SIZE);
  }

  Log_DebugPrintf("Loaded %u bytes to data FIFO", m_data_fifo.GetSize());
  m_sector_buffer.clear();
}
