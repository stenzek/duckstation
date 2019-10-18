#include "cdrom.h"
#include "YBaseLib/Log.h"
#include "common/cd_image.h"
#include "common/state_wrapper.h"
#include "dma.h"
#include "imgui.h"
#include "interrupt_controller.h"
#include "spu.h"
#include "system.h"
Log_SetChannel(CDROM);

CDROM::CDROM() : m_sector_buffer(SECTOR_BUFFER_SIZE) {}

CDROM::~CDROM() = default;

bool CDROM::Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, SPU* spu)
{
  m_system = system;
  m_dma = dma;
  m_interrupt_controller = interrupt_controller;
  m_spu = spu;
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
  m_interrupt_enable_register = INTERRUPT_REGISTER_MASK;
  m_interrupt_flag_register = 0;
  m_setloc = {};
  m_setloc_dirty = false;
  m_filter_file_number = 0;
  m_filter_channel_number = 0;
  m_last_sector_header = {};
  m_last_sector_subheader = {};

  m_next_cd_audio_volume_matrix[0][0] = 0x80;
  m_next_cd_audio_volume_matrix[0][1] = 0x00;
  m_next_cd_audio_volume_matrix[1][0] = 0x00;
  m_next_cd_audio_volume_matrix[1][1] = 0x80;
  m_cd_audio_volume_matrix = m_next_cd_audio_volume_matrix;

  m_xa_last_samples.fill(0);
  for (u32 i = 0; i < 2; i++)
  {
    m_xa_resample_ring_buffer[i].fill(0);
    m_xa_resample_p = 0;
    m_xa_resample_sixstep = 6;
  }

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
  sw.Do(&m_interrupt_enable_register);
  sw.Do(&m_interrupt_flag_register);
  sw.DoPOD(&m_setloc);
  sw.Do(&m_setloc_dirty);
  sw.Do(&m_filter_file_number);
  sw.Do(&m_filter_channel_number);
  sw.DoPOD(&m_last_sector_header);
  sw.DoPOD(&m_last_sector_subheader);
  sw.Do(&m_cd_audio_volume_matrix);
  sw.Do(&m_next_cd_audio_volume_matrix);
  sw.Do(&m_xa_last_samples);
  sw.Do(&m_xa_resample_ring_buffer);
  sw.Do(&m_xa_resample_p);
  sw.Do(&m_xa_resample_sixstep);
  sw.Do(&m_param_fifo);
  sw.Do(&m_response_fifo);
  sw.Do(&m_data_fifo);
  sw.Do(&m_sector_buffer);

  u32 media_lba = m_media ? m_media->GetPositionOnDisc() : 0;
  std::string media_filename = m_media ? m_media->GetFileName() : std::string();
  sw.Do(&media_filename);
  sw.Do(&media_lba);

  if (sw.IsReading())
  {
    if (m_command_state == CommandState::WaitForExecute)
      m_system->SetDowncount(m_command_remaining_ticks);
    if (m_reading)
      m_system->SetDowncount(m_sector_read_remaining_ticks);

    // load up media if we had something in there before
    m_media.reset();
    if (!media_filename.empty())
    {
      m_media = CDImage::Open(media_filename.c_str());
      if (!m_media || !m_media->Seek(media_lba))
      {
        Log_ErrorPrintf("Failed to re-insert CD media from save state: '%s'. Ejecting.", media_filename.c_str());
        RemoveMedia();
      }
    }
  }

  return !sw.HasError();
}

bool CDROM::InsertMedia(const char* filename)
{
  auto media = CDImage::Open(filename);
  if (!media)
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
          Log_DebugPrintf("Audio volume for right-to-left output <- 0x%02X", ZeroExtend32(value));
          m_next_cd_audio_volume_matrix[1][0] = value;
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
          Log_DebugPrintf("Audio volume for left-to-left output <- 0x%02X", ZeroExtend32(value));
          m_next_cd_audio_volume_matrix[0][0] = value;
          return;
        }

        case 3:
        {
          Log_DebugPrintf("Audio volume for right-to-left output <- 0x%02X", ZeroExtend32(value));
          m_next_cd_audio_volume_matrix[1][0] = value;
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
          Log_DebugPrintf("Audio volume for left-to-right output <- 0x%02X", ZeroExtend32(value));
          m_next_cd_audio_volume_matrix[0][1] = value;
          return;
        }

        case 3:
        {
          Log_DebugPrintf("Audio volume apply changes <- 0x%02X", ZeroExtend32(value));
          m_cd_audio_volume_matrix = m_next_cd_audio_volume_matrix;
          return;
        }
      }
    }
    break;
  }

  Log_ErrorPrintf("Unknown CDROM register write: offset=0x%02X, index=%d, value=0x%02X", offset,
                  ZeroExtend32(m_status.index.GetValue()), ZeroExtend32(value));
}

void CDROM::DMARead(u32* words, u32 word_count)
{
  const u32 words_in_fifo = m_data_fifo.GetSize() / 4;
  if (words_in_fifo < word_count)
  {
    Log_ErrorPrintf("DMA read on empty/near-empty data FIFO");
    std::memset(words + words_in_fifo, 0, sizeof(u32) * (word_count - words_in_fifo));
  }

  const u32 bytes_to_read = std::min<u32>(word_count * sizeof(u32), m_data_fifo.GetSize());
  m_data_fifo.PopRange(reinterpret_cast<u8*>(words), bytes_to_read);
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

void CDROM::SendErrorResponse(u8 reason /*= 0x80*/)
{
  m_response_fifo.Push(m_secondary_status.bits | 0x01);
  m_response_fifo.Push(reason);
  SetInterrupt(Interrupt::INT5);
}

void CDROM::UpdateStatusRegister()
{
  m_status.ADPBUSY = false;
  m_status.PRMEMPTY = m_param_fifo.IsEmpty();
  m_status.PRMWRDY = !m_param_fifo.IsFull();
  m_status.RSLRRDY = !m_response_fifo.IsEmpty();
  m_status.DRQSTS = !m_data_fifo.IsEmpty();
  m_status.BUSYSTS = m_command_state == CommandState::WaitForExecute;

  m_dma->SetRequest(DMA::Channel::CDROM, m_status.DRQSTS);
}

u32 CDROM::GetAckDelayForCommand() const
{
  const u32 default_ack_delay = 2000;
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
      Log_DebugPrintf("CDROM setloc command (%02X, %02X, %02X)", ZeroExtend32(m_param_fifo.Peek(0)),
                      ZeroExtend32(m_param_fifo.Peek(1)), ZeroExtend32(m_param_fifo.Peek(2)));
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
        if (!m_media || !m_media->Seek(m_setloc))
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
      Log_DebugPrintf("CDROM setfilter command 0x%02X 0x%02X", ZeroExtend32(file), ZeroExtend32(channel));
      m_filter_file_number = file;
      m_filter_channel_number = channel;
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
        if (!m_media || !m_media->Seek(m_setloc))
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

    case Command::Mute:
    {
      Log_DebugPrintf("CDROM mute command");
      m_muted = true;
      m_response_fifo.Push(m_secondary_status.bits);
      SetInterrupt(Interrupt::ACK);
      EndCommand();
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
      m_response_fifo.Push(1);                           // track number
      m_response_fifo.Push(1);                           // index
      m_response_fifo.Push(m_last_sector_header.minute); // minute within track
      m_response_fifo.Push(m_last_sector_header.second); // second within track
      m_response_fifo.Push(m_last_sector_header.frame);  // frame within track
      m_response_fifo.Push(m_last_sector_header.minute); // minute on entire disc
      m_response_fifo.Push(m_last_sector_header.second); // second on entire disc
      m_response_fifo.Push(m_last_sector_header.frame);  // frame on entire disc
      SetInterrupt(Interrupt::ACK);
      EndCommand();
    }
    break;

    case Command::GetTN:
    {
      Log_DebugPrintf("CDROM GetTN command");
      if (m_media)
      {
        m_response_fifo.Push(m_secondary_status.bits);
        m_response_fifo.Push(DecimalToBCD(Truncate8(m_media->GetTrackNumber())));
        m_response_fifo.Push(DecimalToBCD(Truncate8(m_media->GetTrackCount())));
        SetInterrupt(Interrupt::ACK);
      }
      else
      {
        SendErrorResponse(0x80);
      }

      EndCommand();
    }
    break;

    case Command::GetTD:
    {
      Log_DebugPrintf("CDROM GetTD command");
      Assert(m_param_fifo.GetSize() >= 1);
      const u8 track = BCDToDecimal(m_param_fifo.Peek());

      if (!m_media)
      {
        SendErrorResponse(0x80);
      }
      else if (track > m_media->GetTrackCount())
      {
        SendErrorResponse(0x10);
      }
      else
      {
        CDImage::Position pos;
        if (track == 0)
          pos = CDImage::Position::FromLBA(m_media->GetLBACount());
        else
          pos = m_media->GetTrackStartMSFPosition(track);

        m_response_fifo.Push(m_secondary_status.bits);
        m_response_fifo.Push(DecimalToBCD(Truncate8(pos.minute)));
        m_response_fifo.Push(DecimalToBCD(Truncate8(pos.second)));
        SetInterrupt(Interrupt::ACK);
      }

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
  Log_DevPrintf("Read sector %u: mode %u submode 0x%02X", m_media->GetPositionOnDisc(),
                ZeroExtend32(m_last_sector_header.sector_mode), ZeroExtend32(m_last_sector_subheader.submode.bits));

  bool pass_to_cpu = true;
  if (m_mode.xa_enable && m_last_sector_header.sector_mode == 2)
  {
    if (m_last_sector_subheader.submode.realtime && m_last_sector_subheader.submode.audio)
    {
      // Check for automatic ADPCM filter.
      if (m_mode.xa_filter && (m_last_sector_subheader.file_number != m_filter_file_number ||
                               m_last_sector_subheader.channel_number != m_filter_channel_number))
      {
        Log_DebugPrintf("Skipping sector due to filter mismatch (expected %u/%u got %u/%u)", m_filter_file_number,
                        m_filter_channel_number, m_last_sector_subheader.file_number,
                        m_last_sector_subheader.channel_number);
      }
      else
      {
        ProcessXAADPCMSector();
      }

      // Audio+realtime sectors aren't delivered to the CPU.
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

static std::array<std::array<s16, 29>, 7> s_zigzag_table = {
  {{0,      0x0,     0x0,     0x0,    0x0,     -0x0002, 0x000A,  -0x0022, 0x0041, -0x0054,
    0x0034, 0x0009,  -0x010A, 0x0400, -0x0A78, 0x234C,  0x6794,  -0x1780, 0x0BCD, -0x0623,
    0x0350, -0x016D, 0x006B,  0x000A, -0x0010, 0x0011,  -0x0008, 0x0003,  -0x0001},
   {0,       0x0,    0x0,     -0x0002, 0x0,    0x0003,  -0x0013, 0x003C,  -0x004B, 0x00A2,
    -0x00E3, 0x0132, -0x0043, -0x0267, 0x0C9D, 0x74BB,  -0x11B4, 0x09B8,  -0x05BF, 0x0372,
    -0x01A8, 0x00A6, -0x001B, 0x0005,  0x0006, -0x0008, 0x0003,  -0x0001, 0x0},
   {0,      0x0,     -0x0001, 0x0003,  -0x0002, -0x0005, 0x001F,  -0x004A, 0x00B3, -0x0192,
    0x02B1, -0x039E, 0x04F8,  -0x05A6, 0x7939,  -0x05A6, 0x04F8,  -0x039E, 0x02B1, -0x0192,
    0x00B3, -0x004A, 0x001F,  -0x0005, -0x0002, 0x0003,  -0x0001, 0x0,     0x0},
   {0,       -0x0001, 0x0003,  -0x0008, 0x0006, 0x0005,  -0x001B, 0x00A6, -0x01A8, 0x0372,
    -0x05BF, 0x09B8,  -0x11B4, 0x74BB,  0x0C9D, -0x0267, -0x0043, 0x0132, -0x00E3, 0x00A2,
    -0x004B, 0x003C,  -0x0013, 0x0003,  0x0,    -0x0002, 0x0,     0x0,    0x0},
   {-0x0001, 0x0003,  -0x0008, 0x0011,  -0x0010, 0x000A, 0x006B,  -0x016D, 0x0350, -0x0623,
    0x0BCD,  -0x1780, 0x6794,  0x234C,  -0x0A78, 0x0400, -0x010A, 0x0009,  0x0034, -0x0054,
    0x0041,  -0x0022, 0x000A,  -0x0001, 0x0,     0x0001, 0x0,     0x0,     0x0},
   {0x0002,  -0x0008, 0x0010,  -0x0023, 0x002B, 0x001A,  -0x00EB, 0x027B,  -0x0548, 0x0AFA,
    -0x16FA, 0x53E0,  0x3C07,  -0x1249, 0x080E, -0x0347, 0x015B,  -0x0044, -0x0017, 0x0046,
    -0x0023, 0x0011,  -0x0005, 0x0,     0x0,    0x0,     0x0,     0x0,     0x0},
   {-0x0005, 0x0011,  -0x0023, 0x0046, -0x0017, -0x0044, 0x015B,  -0x0347, 0x080E, -0x1249,
    0x3C07,  0x53E0,  -0x16FA, 0x0AFA, -0x0548, 0x027B,  -0x00EB, 0x001A,  0x002B, -0x0023,
    0x0010,  -0x0008, 0x0002,  0x0,    0x0,     0x0,     0x0,     0x0,     0x0}}};

static s16 ZigZagInterpolate(const s16* ringbuf, const s16* table, u8 p)
{
  s32 sum = 0;
  for (u8 i = 0; i < 29; i++)
    sum += (s32(ringbuf[(p - i) & 0x1F]) * s32(table[i])) / 0x8000;

  return static_cast<s16>(std::clamp<s32>(sum, -0x8000, 0x7FFF));
}

static constexpr s16 ApplyVolume(s16 sample, u8 volume)
{
  return static_cast<s16>(s32(sample) * static_cast<s32>(ZeroExtend32(volume)) / 0x80);
}

template<bool STEREO, bool SAMPLE_RATE>
static void ResampleXAADPCM(const s16* samples_in, u32 num_samples_in, SPU* spu,
                            std::array<std::array<s16, CDROM::XA_RESAMPLE_RING_BUFFER_SIZE>, 2>& ring_buffer, u8* p_ptr,
                            u8* sixstep_ptr, const std::array<std::array<u8, 2>, 2>& volume_matrix)
{
  s16* left_ringbuf = ring_buffer[0].data();
  s16* right_ringbuf = ring_buffer[1].data();
  u8 p = *p_ptr;
  u8 sixstep = *sixstep_ptr;

  for (u32 in_sample_index = 0; in_sample_index < num_samples_in; in_sample_index++)
  {
    const s16 left = *(samples_in++);
    const s16 right = STEREO ? *(samples_in++) : left;

    for (u32 sample_dup = 0; sample_dup < (SAMPLE_RATE ? 2 : 1); sample_dup++)
    {
      left_ringbuf[p] = left;
      if constexpr (STEREO)
        right_ringbuf[p] = right;
      p = (p + 1) % 32;
      sixstep--;

      if (sixstep == 0)
      {
        sixstep = 6;
        for (u32 j = 0; j < 7; j++)
        {
          const s16 left_interp = ZigZagInterpolate(left_ringbuf, s_zigzag_table[j].data(), p);
          const s16 right_interp = STEREO ? ZigZagInterpolate(right_ringbuf, s_zigzag_table[j].data(), p) : left_interp;

          const s16 left_out =
            ApplyVolume(left_interp, volume_matrix[0][0]) + ApplyVolume(right_interp, volume_matrix[1][0]);
          const s16 right_out =
            ApplyVolume(left_interp, volume_matrix[1][0]) + ApplyVolume(right_interp, volume_matrix[1][1]);

          spu->AddCDAudioSample(left_out, right_out);
        }
      }
    }
  }

  *p_ptr = p;
  *sixstep_ptr = sixstep;
}

void CDROM::ProcessXAADPCMSector()
{
  std::array<s16, CDXA::XA_ADPCM_SAMPLES_PER_SECTOR_4BIT> sample_buffer;
  CDXA::DecodeADPCMSector(m_sector_buffer.data(), sample_buffer.data(), m_xa_last_samples.data());

  // Only send to SPU if we're not muted.
  if (m_muted)
    return;

  if (m_last_sector_subheader.codinginfo.IsStereo())
  {
    const u32 num_samples = m_last_sector_subheader.codinginfo.GetSamplesPerSector() / 2;
    m_spu->EnsureCDAudioSpace(num_samples);

    if (m_last_sector_subheader.codinginfo.IsHalfSampleRate())
    {
      ResampleXAADPCM<true, true>(sample_buffer.data(), num_samples, m_spu, m_xa_resample_ring_buffer, &m_xa_resample_p,
                                  &m_xa_resample_sixstep, m_cd_audio_volume_matrix);
    }
    else
    {
      ResampleXAADPCM<true, false>(sample_buffer.data(), num_samples, m_spu, m_xa_resample_ring_buffer,
                                   &m_xa_resample_p, &m_xa_resample_sixstep, m_cd_audio_volume_matrix);
    }
  }
  else
  {
    const u32 num_samples = m_last_sector_subheader.codinginfo.GetSamplesPerSector();
    m_spu->EnsureCDAudioSpace(num_samples);

    if (m_last_sector_subheader.codinginfo.IsHalfSampleRate())
    {
      ResampleXAADPCM<false, true>(sample_buffer.data(), num_samples, m_spu, m_xa_resample_ring_buffer,
                                   &m_xa_resample_p, &m_xa_resample_sixstep, m_cd_audio_volume_matrix);
    }
    else
    {
      ResampleXAADPCM<false, false>(sample_buffer.data(), num_samples, m_spu, m_xa_resample_ring_buffer,
                                    &m_xa_resample_p, &m_xa_resample_sixstep, m_cd_audio_volume_matrix);
    }
  }
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

void CDROM::DrawDebugWindow()
{
  static const ImVec4 active_color{1.0f, 1.0f, 1.0f, 1.0f};
  static const ImVec4 inactive_color{0.4f, 0.4f, 0.4f, 1.0f};

  if (!m_show_cdrom_state)
    return;

  ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("CDROM State", &m_show_cdrom_state))
  {
    ImGui::End();
    return;
  }

  // draw voice states
  if (ImGui::CollapsingHeader("Media", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (m_media)
    {
      const auto [disc_minute, disc_second, disc_frame] = m_media->GetMSFPositionOnDisc();
      const auto [track_minute, track_second, track_frame] = m_media->GetMSFPositionInTrack();

      ImGui::Text("Filename: %s", m_media->GetFileName().c_str());
      ImGui::Text("Disc Position: MSF[%02u:%02u:%02u] LBA[%u]", disc_minute, disc_second, disc_frame,
                  m_media->GetPositionOnDisc());
      ImGui::Text("Track Position: Number[%u] MSF[%02u:%02u:%02u] LBA[%u]", m_media->GetTrackNumber(), track_minute,
                  track_second, track_frame, m_media->GetPositionInTrack());
      ImGui::Text("Last Sector: %02X:%02X:%02X (Mode %u)", m_last_sector_header.minute, m_last_sector_header.second,
                  m_last_sector_header.frame, m_last_sector_header.sector_mode);
    }
    else
    {
      ImGui::Text("No media inserted.");
    }
  }

  if (ImGui::CollapsingHeader("Status/Mode", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Columns(3);

    ImGui::Text("Status");
    ImGui::NextColumn();
    ImGui::Text("Secondary Status");
    ImGui::NextColumn();
    ImGui::Text("Mode Status");
    ImGui::NextColumn();

    ImGui::TextColored(m_status.ADPBUSY ? active_color : inactive_color, "ADPBUSY: %s",
                       m_status.ADPBUSY ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_secondary_status.error ? active_color : inactive_color, "Error: %s",
                       m_secondary_status.error ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_mode.cdda ? active_color : inactive_color, "CDDA: %s", m_mode.cdda ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextColored(m_status.PRMEMPTY ? active_color : inactive_color, "PRMEMPTY: %s",
                       m_status.PRMEMPTY ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_secondary_status.motor_on ? active_color : inactive_color, "Motor On: %s",
                       m_secondary_status.motor_on ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_mode.auto_pause ? active_color : inactive_color, "Auto Pause: %s",
                       m_mode.auto_pause ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextColored(m_status.PRMWRDY ? active_color : inactive_color, "PRMWRDY: %s",
                       m_status.PRMWRDY ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_secondary_status.seek_error ? active_color : inactive_color, "Seek Error: %s",
                       m_secondary_status.seek_error ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_mode.report_audio ? active_color : inactive_color, "Report Audio: %s",
                       m_mode.report_audio ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextColored(m_status.RSLRRDY ? active_color : inactive_color, "RSLRRDY: %s",
                       m_status.RSLRRDY ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_secondary_status.id_error ? active_color : inactive_color, "ID Error: %s",
                       m_secondary_status.id_error ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_mode.xa_filter ? active_color : inactive_color, "XA Filter: %s (File %u Channel %u)",
                       m_mode.xa_filter ? "Yes" : "No", m_filter_file_number, m_filter_channel_number);
    ImGui::NextColumn();

    ImGui::TextColored(m_status.DRQSTS ? active_color : inactive_color, "DRQSTS: %s", m_status.DRQSTS ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_secondary_status.shell_open ? active_color : inactive_color, "Shell Open: %s",
                       m_secondary_status.shell_open ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_mode.ignore_bit ? active_color : inactive_color, "Ignore Bit: %s",
                       m_mode.ignore_bit ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextColored(m_status.BUSYSTS ? active_color : inactive_color, "BUSYSTS: %s",
                       m_status.BUSYSTS ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_secondary_status.reading ? active_color : inactive_color, "Reading: %s",
                       m_secondary_status.reading ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_mode.read_raw_sector ? active_color : inactive_color, "Read Raw Sectors: %s",
                       m_mode.read_raw_sector ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::NextColumn();
    ImGui::TextColored(m_secondary_status.seeking ? active_color : inactive_color, "Seeking: %s",
                       m_secondary_status.seeking ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_mode.xa_enable ? active_color : inactive_color, "XA Enable: %s",
                       m_mode.xa_enable ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::NextColumn();
    ImGui::TextColored(m_secondary_status.playing_cdda ? active_color : inactive_color, "Playing CDDA: %s",
                       m_secondary_status.playing_cdda ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(m_mode.double_speed ? active_color : inactive_color, "Double Speed: %s",
                       m_mode.double_speed ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::Columns(1);
    ImGui::NewLine();

    ImGui::Text("Interrupt Enable Register: 0x%02X", m_interrupt_enable_register);
    ImGui::Text("Interrupt Flag Register: 0x%02X", m_interrupt_flag_register);
  }

  if (ImGui::CollapsingHeader("CD Audio", ImGuiTreeNodeFlags_DefaultOpen))
  {
    const bool playing_anything = m_reading && m_mode.xa_enable;
    ImGui::TextColored(playing_anything ? active_color : inactive_color, "Playing: %s",
                       (m_reading && m_mode.xa_enable) ? "XA-ADPCM" : "Disabled");
    ImGui::TextColored(m_muted ? inactive_color : active_color, "Muted: %s", m_muted ? "Yes" : "No");
    ImGui::Text("Left Output: Left Channel=%02X (%u%%), Right Channel=%02X (%u%%)", m_cd_audio_volume_matrix[0][0],
                ZeroExtend32(m_cd_audio_volume_matrix[0][0]) * 100 / 0x80, m_cd_audio_volume_matrix[0][1],
                ZeroExtend32(m_cd_audio_volume_matrix[0][1]) * 100 / 0x80);
    ImGui::Text("Right Output: Left Channel=%02X (%u%%), Right Channel=%02X (%u%%)", m_cd_audio_volume_matrix[1][0],
                ZeroExtend32(m_cd_audio_volume_matrix[1][0]) * 100 / 0x80, m_cd_audio_volume_matrix[1][1],
                ZeroExtend32(m_cd_audio_volume_matrix[1][1]) * 100 / 0x80);
  }

  ImGui::End();
}

void CDROM::DrawDebugMenu()
{
  ImGui::MenuItem("CDROM", nullptr, &m_show_cdrom_state);
}
