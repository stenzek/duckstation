#include "cdrom.h"
#include "common/cd_image.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "dma.h"
#include "imgui.h"
#include "interrupt_controller.h"
#include "settings.h"
#include "spu.h"
#include "system.h"
Log_SetChannel(CDROM);

CDROM::CDROM()
{
  m_sector_buffer.reserve(RAW_SECTOR_OUTPUT_SIZE);
}

CDROM::~CDROM() = default;

void CDROM::Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, SPU* spu)
{
  m_system = system;
  m_dma = dma;
  m_interrupt_controller = interrupt_controller;
  m_spu = spu;
  m_command_event =
    m_system->CreateTimingEvent("CDROM Command Event", 1, 1, std::bind(&CDROM::ExecuteCommand, this), false);
  m_drive_event = m_system->CreateTimingEvent("CDROM Drive Event", 1, 1,
                                              std::bind(&CDROM::ExecuteDrive, this, std::placeholders::_2), false);

  if (m_system->GetSettings().cdrom_read_thread)
    m_reader.StartThread();
}

void CDROM::Reset()
{
  SoftReset();
}

void CDROM::SoftReset()
{
  m_command = Command::None;
  m_command_event->Deactivate();
  m_drive_state = DriveState::Idle;
  m_drive_event->Deactivate();
  m_status.bits = 0;
  m_secondary_status.bits = 0;
  m_mode.bits = 0;
  m_interrupt_enable_register = INTERRUPT_REGISTER_MASK;
  m_interrupt_flag_register = 0;
  m_pending_async_interrupt = 0;
  m_setloc_position = {};
  m_last_requested_sector = 0;
  if (m_reader.HasMedia())
    m_reader.QueueReadSector(m_last_requested_sector);
  m_setloc_pending = false;
  m_read_after_seek = false;
  m_play_after_seek = false;
  m_muted = false;
  m_adpcm_muted = false;
  m_filter_file_number = 0;
  m_filter_channel_number = 0;
  std::memset(&m_last_sector_header, 0, sizeof(m_last_sector_header));
  std::memset(&m_last_sector_subheader, 0, sizeof(m_last_sector_subheader));
  std::memset(&m_last_subq, 0, sizeof(m_last_subq));
  m_last_cdda_report_frame_nibble = 0xFF;
  m_cdda_report_delay = 0;

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
  m_async_response_fifo.Clear();
  m_data_fifo.Clear();
  m_sector_buffer.clear();

  UpdateStatusRegister();
}

bool CDROM::DoState(StateWrapper& sw)
{
  sw.Do(&m_command);
  sw.Do(&m_drive_state);
  sw.Do(&m_status.bits);
  sw.Do(&m_secondary_status.bits);
  sw.Do(&m_mode.bits);
  sw.Do(&m_interrupt_enable_register);
  sw.Do(&m_interrupt_flag_register);
  sw.Do(&m_pending_async_interrupt);
  sw.DoPOD(&m_setloc_position);
  sw.DoPOD(&m_last_requested_sector);
  sw.Do(&m_setloc_pending);
  sw.Do(&m_read_after_seek);
  sw.Do(&m_play_after_seek);
  sw.Do(&m_muted);
  sw.Do(&m_adpcm_muted);
  sw.Do(&m_filter_file_number);
  sw.Do(&m_filter_channel_number);
  sw.DoBytes(&m_last_sector_header, sizeof(m_last_sector_header));
  sw.DoBytes(&m_last_sector_subheader, sizeof(m_last_sector_subheader));
  sw.DoBytes(&m_last_subq, sizeof(m_last_subq));
  sw.Do(&m_last_cdda_report_frame_nibble);
  sw.Do(&m_cdda_report_delay);
  sw.Do(&m_play_track_number_bcd);
  sw.Do(&m_async_command_parameter);
  sw.Do(&m_cd_audio_volume_matrix);
  sw.Do(&m_next_cd_audio_volume_matrix);
  sw.Do(&m_xa_last_samples);
  sw.Do(&m_xa_resample_ring_buffer);
  sw.Do(&m_xa_resample_p);
  sw.Do(&m_xa_resample_sixstep);
  sw.Do(&m_param_fifo);
  sw.Do(&m_response_fifo);
  sw.Do(&m_async_response_fifo);
  sw.Do(&m_data_fifo);
  sw.Do(&m_sector_buffer);

  if (sw.IsReading())
  {
    if (m_reader.HasMedia())
      m_reader.QueueReadSector(m_last_requested_sector);
    UpdateCommandEvent();
    m_drive_event->SetState(!IsDriveIdle());
  }

  return !sw.HasError();
}

bool CDROM::HasMedia() const
{
  return m_reader.HasMedia();
}

std::string CDROM::GetMediaFileName() const
{
  return m_reader.GetMediaFileName();
}

void CDROM::InsertMedia(std::unique_ptr<CDImage> media)
{
  if (HasMedia())
    RemoveMedia();

  m_reader.SetMedia(std::move(media));
}

void CDROM::RemoveMedia()
{
  if (!m_reader.HasMedia())
    return;

  Log_InfoPrintf("Removing CD...");
  m_reader.RemoveMedia();

  m_secondary_status.shell_open = true;

  // If the drive was doing anything, we need to abort the command.
  if (m_drive_state != DriveState::Idle)
  {
    // TODO: Verify this.
    Log_WarningPrintf("Aborting drive operation");
    SendAsyncErrorResponse(0x08);
    m_drive_state = DriveState::Idle;
  }
}

void CDROM::SetUseReadThread(bool enabled)
{
  if (enabled == m_reader.IsUsingThread())
    return;

  if (enabled)
    m_reader.StartThread();
  else
    m_reader.StopThread();
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
      Log_TracePrintf("CDROM status register <- 0x%02X", value);
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
          Log_DebugPrintf("CDROM command register <- 0x%02X", value);
          if (HasPendingCommand())
          {
            Log_WarningPrintf("Cancelling pending command 0x%02X", static_cast<u8>(m_command));
            m_command = Command::None;
          }

          BeginCommand(static_cast<Command>(value));
          return;
        }

        case 1:
        {
          Log_ErrorPrintf("Sound map data out <- 0x%02X", value);
          return;
        }

        case 2:
        {
          Log_ErrorPrintf("Sound map coding info <- 0x%02X", value);
          return;
        }

        case 3:
        {
          Log_DebugPrintf("Audio volume for right-to-left output <- 0x%02X", value);
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
          Log_DebugPrintf("Interrupt enable register <- 0x%02X", value);
          m_interrupt_enable_register = value & INTERRUPT_REGISTER_MASK;
          UpdateInterruptRequest();
          return;
        }

        case 2:
        {
          Log_DebugPrintf("Audio volume for left-to-left output <- 0x%02X", value);
          m_next_cd_audio_volume_matrix[0][0] = value;
          return;
        }

        case 3:
        {
          Log_DebugPrintf("Audio volume for right-to-left output <- 0x%02X", value);
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
          if (m_interrupt_flag_register == 0)
          {
            if (HasPendingAsyncInterrupt())
              DeliverAsyncInterrupt();
            else
              UpdateCommandEvent();
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
          Log_DebugPrintf("Audio volume for left-to-right output <- 0x%02X", value);
          m_next_cd_audio_volume_matrix[0][1] = value;
          return;
        }

        case 3:
        {
          Log_DebugPrintf("Audio volume apply changes <- 0x%02X", value);
          m_adpcm_muted = ConvertToBoolUnchecked(value & u8(0x01));
          if (value & 0x20)
            m_cd_audio_volume_matrix = m_next_cd_audio_volume_matrix;
          return;
        }
      }
    }
    break;
  }

  Log_ErrorPrintf("Unknown CDROM register write: offset=0x%02X, index=%d, value=0x%02X", offset,
                  m_status.index.GetValue(), value);
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
  UpdateInterruptRequest();
}

void CDROM::SetAsyncInterrupt(Interrupt interrupt)
{
  Assert(m_pending_async_interrupt == 0);
  m_pending_async_interrupt = static_cast<u8>(interrupt);
  if (!HasPendingInterrupt())
    DeliverAsyncInterrupt();
}

void CDROM::ClearAsyncInterrupt()
{
  m_pending_async_interrupt = 0;
  m_async_response_fifo.Clear();
}

void CDROM::DeliverAsyncInterrupt()
{
  Assert(m_pending_async_interrupt != 0 && !HasPendingInterrupt());
  Log_DevPrintf("Delivering async interrupt %u", m_pending_async_interrupt);

  m_response_fifo.Clear();
  m_response_fifo.PushFromQueue(&m_async_response_fifo);
  m_interrupt_flag_register = m_pending_async_interrupt;
  m_pending_async_interrupt = 0;
  UpdateInterruptRequest();
  UpdateStatusRegister();
  UpdateCommandEvent();
}

void CDROM::SendACKAndStat()
{
  m_response_fifo.Push(m_secondary_status.bits);
  SetInterrupt(Interrupt::ACK);
}

void CDROM::SendErrorResponse(u8 stat_bits /* = STAT_ERROR */, u8 reason /* = 0x80 */)
{
  m_response_fifo.Push(m_secondary_status.bits | stat_bits);
  m_response_fifo.Push(reason);
  SetInterrupt(Interrupt::Error);
}

void CDROM::SendAsyncErrorResponse(u8 stat_bits /* = STAT_ERROR */, u8 reason /* = 0x80 */)
{
  m_async_response_fifo.Push(m_secondary_status.bits | stat_bits);
  m_async_response_fifo.Push(reason);
  SetAsyncInterrupt(Interrupt::Error);
}

void CDROM::UpdateStatusRegister()
{
  m_status.ADPBUSY = false;
  m_status.PRMEMPTY = m_param_fifo.IsEmpty();
  m_status.PRMWRDY = !m_param_fifo.IsFull();
  m_status.RSLRRDY = !m_response_fifo.IsEmpty();
  m_status.DRQSTS = !m_data_fifo.IsEmpty();
  m_status.BUSYSTS = HasPendingCommand();

  m_dma->SetRequest(DMA::Channel::CDROM, m_status.DRQSTS);
}

void CDROM::UpdateInterruptRequest()
{
  if ((m_interrupt_flag_register & m_interrupt_enable_register) == 0)
    return;

  m_interrupt_controller->InterruptRequest(InterruptController::IRQ::CDROM);
}

TickCount CDROM::GetAckDelayForCommand() const
{
  const u32 default_ack_delay = 10000;
  if (m_command == Command::Init || m_command == Command::ReadTOC)
    return 60000;
  else
    return default_ack_delay;
}

TickCount CDROM::GetTicksForRead() const
{
  return m_mode.double_speed ? (MASTER_CLOCK / 150) : (MASTER_CLOCK / 75);
}

TickCount CDROM::GetTicksForSeek() const
{
  const CDImage::LBA current_lba = m_secondary_status.motor_on ? m_reader.GetLastReadSector() : 0;
  const CDImage::LBA new_lba = m_setloc_position.ToLBA();
  const u32 lba_diff = static_cast<u32>((new_lba > current_lba) ? (new_lba - current_lba) : (current_lba - new_lba));

  // const TickCount ticks = static_cast<TickCount>(20000 + lba_diff * 100);

  // Formula from Mednafen.
  TickCount ticks = std::max<TickCount>(20000, lba_diff * MASTER_CLOCK * 1000 / (72 * 60 * 75) / 1000);
  if (!m_secondary_status.motor_on)
    ticks += MASTER_CLOCK;
  else if (m_drive_state == DriveState::Idle) // paused
    ticks += 1237952 << (BoolToUInt8(!m_mode.double_speed));
  if (lba_diff >= 2550)
    ticks += static_cast<TickCount>(u64(MASTER_CLOCK) * 300 / 1000);

  Log_DevPrintf("Seek time for %u LBAs: %d", lba_diff, ticks);
  return ticks;
}

void CDROM::BeginCommand(Command command)
{
  m_command = command;
  m_command_event->SetDowncount(GetAckDelayForCommand());
  UpdateCommandEvent();
  UpdateStatusRegister();
}

void CDROM::EndCommand()
{
  m_param_fifo.Clear();

  m_command = Command::None;
  m_command_event->Deactivate();
  UpdateStatusRegister();
}

void CDROM::ExecuteCommand()
{
  Log_DevPrintf("CDROM executing command 0x%02X", ZeroExtend32(static_cast<u8>(m_command)));

  if (!m_response_fifo.IsEmpty())
  {
    Log_DebugPrintf("Response FIFO not empty on command begin");
    m_response_fifo.Clear();
  }

  switch (m_command)
  {
    case Command::Getstat:
    {
      Log_DebugPrintf("CDROM Getstat command");

      // if bit 0 or 2 is set, send an additional byte
      SendACKAndStat();

      // shell open bit is cleared after sending the status
      if (HasMedia())
        m_secondary_status.shell_open = false;

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
      Log_DebugPrintf("CDROM GetID command");
      if (!HasMedia())
      {
        SendErrorResponse(STAT_ERROR, 0x80);
      }
      else
      {
        SendACKAndStat();

        m_drive_state = DriveState::ReadingID;
        m_drive_event->Schedule(18000);
      }

      EndCommand();
      return;
    }

    case Command::ReadTOC:
    {
      Log_DebugPrintf("CDROM ReadTOC command");
      if (!HasMedia())
      {
        SendErrorResponse(STAT_ERROR, 0x80);
      }
      else
      {
        SendACKAndStat();

        m_drive_state = DriveState::ReadingTOC;
        m_drive_event->Schedule(MASTER_CLOCK / 2); // half a second
      }

      EndCommand();
      return;
    }

    case Command::Setfilter:
    {
      const u8 file = m_param_fifo.Peek(0);
      const u8 channel = m_param_fifo.Peek(1);
      Log_DebugPrintf("CDROM setfilter command 0x%02X 0x%02X", ZeroExtend32(file), ZeroExtend32(channel));
      m_filter_file_number = file;
      m_filter_channel_number = channel;
      SendACKAndStat();
      EndCommand();
      return;
    }

    case Command::Setmode:
    {
      const u8 mode = m_param_fifo.Peek(0);
      Log_DebugPrintf("CDROM setmode command 0x%02X", ZeroExtend32(mode));

      m_mode.bits = mode;
      SendACKAndStat();
      EndCommand();
      return;
    }

    case Command::Setloc:
    {
      // TODO: Verify parameter count
      m_setloc_position.minute = PackedBCDToBinary(m_param_fifo.Peek(0));
      m_setloc_position.second = PackedBCDToBinary(m_param_fifo.Peek(1));
      m_setloc_position.frame = PackedBCDToBinary(m_param_fifo.Peek(2));
      m_setloc_pending = true;
      Log_DebugPrintf("CDROM setloc command (%02X, %02X, %02X)", ZeroExtend32(m_param_fifo.Peek(0)),
                      ZeroExtend32(m_param_fifo.Peek(1)), ZeroExtend32(m_param_fifo.Peek(2)));
      SendACKAndStat();
      EndCommand();
      return;
    }

    case Command::SeekL:
    case Command::SeekP:
    {
      const bool logical = (m_command == Command::SeekL);
      Log_DebugPrintf("CDROM %s command", logical ? "SeekL" : "SeekP");
      if (!HasMedia())
      {
        SendErrorResponse(STAT_ERROR, 0x80);
      }
      else
      {
        SendACKAndStat();
        BeginSeeking(logical, false, false);
      }

      EndCommand();
      return;
    }

    case Command::SetSession:
    {
      const u8 session = m_param_fifo.IsEmpty() ? 0 : m_param_fifo.Peek(0);
      Log_DebugPrintf("CDROM SetSession command, session=%u", session);

      if (!HasMedia() || m_drive_state == DriveState::Reading || m_drive_state == DriveState::Playing)
      {
        SendErrorResponse(STAT_ERROR, 0x80);
      }
      else if (session == 0)
      {
        SendErrorResponse(STAT_ERROR, 0x10);
      }
      else
      {
        SendACKAndStat();

        m_async_command_parameter = session;
        m_drive_state = DriveState::ChangingSession;
        m_drive_event->Schedule(MASTER_CLOCK / 2); // half a second
      }

      EndCommand();
      return;
    }

    case Command::ReadN:
    case Command::ReadS:
    {
      Log_DebugPrintf("CDROM read command");
      if (!HasMedia())
      {
        SendErrorResponse(STAT_ERROR, 0x80);
      }
      else
      {
        SendACKAndStat();
        BeginReading();
      }

      EndCommand();
      return;
    }

    case Command::Play:
    {
      u8 track = m_param_fifo.IsEmpty() ? 0 : m_param_fifo.Peek(0);
      Log_DebugPrintf("CDROM play command, track=%u", track);

      if (!HasMedia())
      {
        SendErrorResponse(STAT_ERROR, 0x80);
      }
      else
      {
        SendACKAndStat();
        BeginPlaying(track);
      }

      EndCommand();
      return;
    }

    case Command::Pause:
    {
      const bool was_reading = (m_drive_state == DriveState::Reading || m_drive_state == DriveState::Playing);
      const TickCount pause_time = was_reading ? (m_mode.double_speed ? 2000000 : 1000000) : 7000;
      Log_DebugPrintf("CDROM pause command");
      SendACKAndStat();

      m_drive_state = DriveState::Pausing;
      m_drive_event->Schedule(pause_time);

      EndCommand();
      return;
    }

    case Command::Stop:
    {
      const bool was_motor_on = m_secondary_status.motor_on;
      const TickCount stop_time = was_motor_on ? (m_mode.double_speed ? 25000000 : 13000000) : 7000;
      Log_DebugPrintf("CDROM stop command");
      SendACKAndStat();

      m_drive_state = DriveState::Stopping;
      m_drive_event->Schedule(stop_time);

      EndCommand();
      return;
    }

    case Command::Init:
    {
      Log_DebugPrintf("CDROM init command");
      SendACKAndStat();

      m_secondary_status.ClearActiveBits();
      m_mode.bits = 0;

      m_drive_state = DriveState::SpinningUp;
      m_drive_event->Schedule(80000);

      EndCommand();
      return;
    }
    break;

    case Command::MotorOn:
    {
      Log_DebugPrintf("CDROM motor on command");
      if (m_secondary_status.motor_on)
      {
        SendErrorResponse(STAT_ERROR, 0x20);
      }
      else
      {
        SendACKAndStat();

        m_drive_state = DriveState::SpinningUp;
        m_drive_event->Schedule(80000);
      }

      EndCommand();
      return;
    }
    break;

    case Command::Mute:
    {
      Log_DebugPrintf("CDROM mute command");
      m_muted = true;
      SendACKAndStat();
      EndCommand();
    }
    break;

    case Command::Demute:
    {
      Log_DebugPrintf("CDROM demute command");
      m_muted = false;
      SendACKAndStat();
      EndCommand();
    }
    break;

    case Command::GetlocL:
    {
      Log_DebugPrintf("CDROM GetlocL command - header %s [%02X:%02X:%02X]",
                      m_secondary_status.header_valid ? "valid" : "invalid", m_last_sector_header.minute,
                      m_last_sector_header.second, m_last_sector_header.frame);
      if (!m_secondary_status.header_valid)
      {
        SendErrorResponse(STAT_ERROR, 0x80);
      }
      else
      {
        m_response_fifo.PushRange(reinterpret_cast<const u8*>(&m_last_sector_header), sizeof(m_last_sector_header));
        m_response_fifo.PushRange(reinterpret_cast<const u8*>(&m_last_sector_subheader),
                                  sizeof(m_last_sector_subheader));
        SetInterrupt(Interrupt::ACK);
      }

      EndCommand();
      return;
    }

    case Command::GetlocP:
    {
      Log_DebugPrintf("CDROM GetlocP command");
      m_response_fifo.Push(m_last_subq.track_number_bcd);
      m_response_fifo.Push(m_last_subq.index_number_bcd);
      m_response_fifo.Push(m_last_subq.relative_minute_bcd);
      m_response_fifo.Push(m_last_subq.relative_second_bcd);
      m_response_fifo.Push(m_last_subq.relative_frame_bcd);
      m_response_fifo.Push(m_last_subq.absolute_minute_bcd);
      m_response_fifo.Push(m_last_subq.absolute_second_bcd);
      m_response_fifo.Push(m_last_subq.absolute_frame_bcd);
      SetInterrupt(Interrupt::ACK);
      EndCommand();
    }
    break;

    case Command::GetTN:
    {
      Log_DebugPrintf("CDROM GetTN command");
      if (HasMedia())
      {
        m_reader.WaitForReadToComplete();

        m_response_fifo.Push(m_secondary_status.bits);
        m_response_fifo.Push(BinaryToBCD(Truncate8(m_reader.GetMedia()->GetTrackNumber())));
        m_response_fifo.Push(BinaryToBCD(Truncate8(m_reader.GetMedia()->GetTrackCount())));
        SetInterrupt(Interrupt::ACK);
      }
      else
      {
        SendErrorResponse(STAT_ERROR, 0x80);
      }

      EndCommand();
    }
    break;

    case Command::GetTD:
    {
      Log_DebugPrintf("CDROM GetTD command");
      Assert(m_param_fifo.GetSize() >= 1);
      const u8 track = PackedBCDToBinary(m_param_fifo.Peek());

      if (!HasMedia())
      {
        SendErrorResponse(STAT_ERROR, 0x80);
      }
      else if (track > m_reader.GetMedia()->GetTrackCount())
      {
        SendErrorResponse(STAT_ERROR, 0x10);
      }
      else
      {
        CDImage::Position pos;
        if (track == 0)
          pos = CDImage::Position::FromLBA(m_reader.GetMedia()->GetLBACount());
        else
          pos = m_reader.GetMedia()->GetTrackStartMSFPosition(track);

        m_response_fifo.Push(m_secondary_status.bits);
        m_response_fifo.Push(BinaryToBCD(Truncate8(pos.minute)));
        m_response_fifo.Push(BinaryToBCD(Truncate8(pos.second)));
        SetInterrupt(Interrupt::ACK);
      }

      EndCommand();
    }
    break;

    case Command::Getparam:
    {
      Log_DebugPrintf("CDROM Getparam command");

      m_response_fifo.Push(m_secondary_status.bits);
      m_response_fifo.Push(m_mode.bits);
      m_response_fifo.Push(0);
      m_response_fifo.Push(m_filter_file_number);
      m_response_fifo.Push(m_filter_channel_number);
      SetInterrupt(Interrupt::ACK);
      EndCommand();
    }
    break;

    default:
    {
      Log_ErrorPrintf("Unknown CDROM command 0x%04X with %u parameters, please report", static_cast<u16>(m_command),
                      m_param_fifo.GetSize());
      Panic("Unknown CDROM command");
    }
    break;
  }
}

void CDROM::ExecuteTestCommand(u8 subcommand)
{
  switch (subcommand)
  {
    case 0x04: // Reset SCEx counters
    {
      Log_DebugPrintf("Reset SCEx counters");
      m_secondary_status.motor_on = true;
      m_response_fifo.Push(m_secondary_status.bits);
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case 0x05: // Read SCEx counters
    {
      Log_DebugPrintf("Read SCEx counters");
      m_response_fifo.Push(m_secondary_status.bits);
      m_response_fifo.Push(0); // # of TOC reads?
      m_response_fifo.Push(0); // # of SCEx strings received
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case 0x20: // Get CDROM BIOS Date/Version
    {
      Log_DebugPrintf("Get CDROM BIOS Date/Version");

      static constexpr u8 response[] = {0x95, 0x05, 0x16, 0xC1};
      m_response_fifo.PushRange(response, countof(response));
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case 0x22:
    {
      Log_DebugPrintf("Get CDROM region ID string");

      switch (m_system->GetRegion())
      {
        case ConsoleRegion::NTSC_J:
        {
          static constexpr u8 response[] = {'f', 'o', 'r', ' ', 'J', 'a', 'p', 'a', 'n'};
          m_response_fifo.PushRange(response, countof(response));
        }
        break;

        case ConsoleRegion::PAL:
        {
          static constexpr u8 response[] = {'f', 'o', 'r', ' ', 'E', 'u', 'r', 'o', 'p', 'e'};
          m_response_fifo.PushRange(response, countof(response));
        }
        break;

        case ConsoleRegion::NTSC_U:
        default:
        {
          static constexpr u8 response[] = {'f', 'o', 'r', ' ', 'U', '/', 'C'};
          m_response_fifo.PushRange(response, countof(response));
        }
        break;
      }

      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    default:
    {
      Log_ErrorPrintf("Unknown test command 0x%02X, %u parameters", subcommand, m_param_fifo.GetSize());
      Panic("Unknown test command");
      return;
    }
  }
}

void CDROM::UpdateCommandEvent()
{
  // if there's a pending interrupt, we can't execute the command yet
  // so deactivate it until the interrupt is acknowledged
  if (!HasPendingCommand() || HasPendingInterrupt())
  {
    m_command_event->Deactivate();
    return;
  }
  else if (HasPendingCommand())
  {
    m_command_event->Activate();
  }
}

void CDROM::ExecuteDrive(TickCount ticks_late)
{
  switch (m_drive_state)
  {
    case DriveState::SpinningUp:
      DoSpinUpComplete();
      break;

    case DriveState::SeekingPhysical:
    case DriveState::SeekingLogical:
      DoSeekComplete(ticks_late);
      break;

    case DriveState::Pausing:
      DoPauseComplete();
      break;

    case DriveState::Stopping:
      DoStopComplete();
      break;

    case DriveState::ReadingID:
      DoIDRead();
      break;

    case DriveState::ReadingTOC:
      DoTOCRead();
      break;

    case DriveState::Reading:
    case DriveState::Playing:
      DoSectorRead();
      break;

    case DriveState::ChangingSession:
      DoChangeSessionComplete();
      break;

    case DriveState::Idle:
    default:
      break;
  }
}

void CDROM::BeginReading(TickCount ticks_late)
{
  Log_DebugPrintf("Starting reading @ LBA %u", m_last_requested_sector);
  if (m_setloc_pending)
  {
    BeginSeeking(true, true, false);
    return;
  }

  m_secondary_status.ClearActiveBits();
  m_secondary_status.motor_on = true;

  // TODO: Should the sector buffer be cleared here?
  m_sector_buffer.clear();

  const TickCount ticks = GetTicksForRead();
  m_drive_state = DriveState::Reading;
  m_drive_event->SetInterval(ticks);
  m_drive_event->Schedule(ticks - ticks_late);

  m_reader.QueueReadSector(m_last_requested_sector);
}

void CDROM::BeginPlaying(u8 track_bcd, TickCount ticks_late)
{
  Log_DebugPrintf("Starting playing CDDA track %x", track_bcd);
  m_last_cdda_report_frame_nibble = 0xFF;
  m_cdda_report_delay = CDImage::FRAMES_PER_SECOND;
  m_play_track_number_bcd = track_bcd;

  // if track zero, start from current position
  if (track_bcd != 0)
  {
    // play specific track?
    if (track_bcd > m_reader.GetMedia()->GetTrackCount())
    {
      // restart current track
      track_bcd = BinaryToBCD(Truncate8(m_reader.GetMedia()->GetTrackNumber()));
    }

    m_setloc_position = m_reader.GetMedia()->GetTrackStartMSFPosition(PackedBCDToBinary(track_bcd));
    m_setloc_pending = true;
  }

  if (m_setloc_pending)
  {
    BeginSeeking(false, false, true);
    return;
  }

  m_secondary_status.ClearActiveBits();
  m_secondary_status.motor_on = true;
  m_secondary_status.playing_cdda = true;

  // TODO: Should the sector buffer be cleared here?
  m_sector_buffer.clear();

  const TickCount ticks = GetTicksForRead();
  m_drive_state = DriveState::Playing;
  m_drive_event->SetInterval(ticks);
  m_drive_event->Schedule(ticks - ticks_late);

  m_reader.QueueReadSector(m_last_requested_sector);
}

void CDROM::BeginSeeking(bool logical, bool read_after_seek, bool play_after_seek)
{
  if (!m_setloc_pending)
    Log_WarningPrintf("Seeking without setloc set");

  m_read_after_seek = read_after_seek;
  m_play_after_seek = play_after_seek;
  m_setloc_pending = false;

  Log_DebugPrintf("Seeking to [%02u:%02u:%02u] (LBA %u) (%s)", m_setloc_position.minute, m_setloc_position.second,
                  m_setloc_position.frame, m_setloc_position.ToLBA(), logical ? "logical" : "physical");

  const TickCount seek_time = GetTicksForSeek();

  m_secondary_status.ClearActiveBits();
  m_secondary_status.motor_on = true;
  m_secondary_status.seeking = true;
  m_sector_buffer.clear();

  m_drive_state = logical ? DriveState::SeekingLogical : DriveState::SeekingPhysical;
  m_drive_event->SetIntervalAndSchedule(seek_time);

  m_last_requested_sector = m_setloc_position.ToLBA();
  m_reader.QueueReadSector(m_last_requested_sector);
}

void CDROM::DoSpinUpComplete()
{
  m_drive_state = DriveState::Idle;
  m_drive_event->Deactivate();

  m_secondary_status.motor_on = true;

  m_async_response_fifo.Clear();
  m_async_response_fifo.Push(m_secondary_status.bits);
  SetAsyncInterrupt(Interrupt::Complete);
}

void CDROM::DoSeekComplete(TickCount ticks_late)
{
  const bool logical = (m_drive_state == DriveState::SeekingLogical);
  m_drive_state = DriveState::Idle;
  m_drive_event->Deactivate();
  m_secondary_status.ClearActiveBits();
  m_sector_buffer.clear();

  bool seek_okay = m_reader.WaitForReadToComplete();
  if (seek_okay)
  {
    m_last_subq = m_reader.GetSectorSubQ();

    // seek and update sub-q for ReadP command
    DebugAssert(m_last_requested_sector == m_reader.GetLastReadSector());
    const auto [seek_mm, seek_ss, seek_ff] = CDImage::Position::FromLBA(m_last_requested_sector).ToBCD();
    seek_okay = (m_last_subq.IsCRCValid() && m_last_subq.absolute_minute_bcd == seek_mm &&
                 m_last_subq.absolute_second_bcd == seek_ss && m_last_subq.absolute_frame_bcd == seek_ff);
    if (seek_okay)
    {
      // check for data header for logical seeks
      if (logical)
      {
        ProcessDataSectorHeader(m_reader.GetSectorBuffer().data(), false);

        // ensure the location matches up (it should)
        seek_okay = (m_last_sector_header.minute == seek_mm && m_last_sector_header.second == seek_ss &&
                     m_last_sector_header.frame == seek_ff);
      }
    }
  }

  if (seek_okay)
  {
    // seek complete, transition to play/read if requested
    // INT2 is not sent on play/read
    if (m_read_after_seek)
    {
      BeginReading(ticks_late);
    }
    else if (m_play_after_seek)
    {
      BeginPlaying(m_play_track_number_bcd, ticks_late);
    }
    else
    {
      m_async_response_fifo.Push(m_secondary_status.bits);
      SetAsyncInterrupt(Interrupt::Complete);
    }
  }
  else
  {
    CDImage::Position pos(CDImage::Position::FromLBA(m_last_requested_sector));
    Log_WarningPrintf("%s seek to [%02u:%02u:%02u] failed", logical ? "Logical" : "Physical", pos.minute, pos.second,
                      pos.frame);
    SendAsyncErrorResponse(STAT_SEEK_ERROR, 0x04);
  }

  m_setloc_pending = false;
  m_read_after_seek = false;
  m_play_after_seek = false;
  UpdateStatusRegister();
}

void CDROM::DoPauseComplete()
{
  Log_DebugPrintf("Pause complete");
  m_drive_state = DriveState::Idle;
  m_drive_event->Deactivate();
  m_secondary_status.ClearActiveBits();
  m_sector_buffer.clear();

  m_async_response_fifo.Clear();
  m_async_response_fifo.Push(m_secondary_status.bits);
  SetAsyncInterrupt(Interrupt::Complete);
}

void CDROM::DoStopComplete()
{
  Log_DebugPrintf("Stop complete");
  m_drive_state = DriveState::Idle;
  m_drive_event->Deactivate();
  m_secondary_status.ClearActiveBits();
  m_secondary_status.motor_on = false;
  m_sector_buffer.clear();

  m_async_response_fifo.Clear();
  m_async_response_fifo.Push(m_secondary_status.bits);
  SetAsyncInterrupt(Interrupt::Complete);
}

void CDROM::DoChangeSessionComplete()
{
  Log_DebugPrintf("Changing session complete");
  m_drive_state = DriveState::Idle;
  m_drive_event->Deactivate();
  m_secondary_status.ClearActiveBits();
  m_secondary_status.motor_on = true;

  m_async_response_fifo.Clear();
  if (m_async_command_parameter == 0x01)
  {
    m_async_response_fifo.Push(m_secondary_status.bits);
    SetAsyncInterrupt(Interrupt::Complete);
  }
  else
  {
    // we don't emulate multisession discs.. for now
    SendAsyncErrorResponse(STAT_SEEK_ERROR, 0x40);
  }
}

void CDROM::DoIDRead()
{
  // TODO: This should depend on the disc type/region...

  Log_DebugPrintf("ID read complete");
  m_drive_state = DriveState::Idle;
  m_drive_event->Deactivate();
  m_secondary_status.ClearActiveBits();
  m_secondary_status.motor_on = true;
  m_sector_buffer.clear();

  static constexpr u8 response2[] = {0x00, 0x20, 0x00, 0x53, 0x43, 0x45, 0x41}; // last byte is 0x49 for EU
  m_async_response_fifo.Clear();
  m_async_response_fifo.Push(m_secondary_status.bits);
  m_async_response_fifo.PushRange(response2, countof(response2));
  SetAsyncInterrupt(Interrupt::Complete);
}

void CDROM::DoTOCRead()
{
  Log_DebugPrintf("TOC read complete");
  m_drive_state = DriveState::Idle;
  m_drive_event->Deactivate();
  m_async_response_fifo.Clear();
  m_async_response_fifo.Push(m_secondary_status.bits);
  SetAsyncInterrupt(Interrupt::Complete);
}

void CDROM::DoSectorRead()
{
  if (!m_reader.WaitForReadToComplete())
    Panic("Sector read failed");

  // TODO: Error handling
  // TODO: Check SubQ checksum.
  const CDImage::SubChannelQ& subq = m_reader.GetSectorSubQ();
  const bool is_data_sector = subq.control.data;
  if (!is_data_sector)
  {
    if (m_play_track_number_bcd == 0)
    {
      // track number was not specified, but we've found the track now
      Log_DebugPrintf("Setting playing track number to %u", m_play_track_number_bcd);
      m_play_track_number_bcd = subq.track_number_bcd;
    }
    else if (m_mode.auto_pause && subq.track_number_bcd != m_play_track_number_bcd)
    {
      // we don't want to update the position if the track changes, so we check it before reading the actual sector.
      Log_DevPrintf("Auto pause at the end of track %u (LBA %u)", m_play_track_number_bcd,
                    m_reader.GetLastReadSector());

      ClearAsyncInterrupt();
      m_async_response_fifo.Push(m_secondary_status.bits);
      SetAsyncInterrupt(Interrupt::DataEnd);

      m_secondary_status.ClearActiveBits();
      m_drive_state = DriveState::Idle;
      m_drive_event->Deactivate();
      return;
    }
  }

  if (subq.IsCRCValid())
  {
    m_last_subq = subq;

    if (is_data_sector && m_drive_state == DriveState::Reading)
    {
      ProcessDataSector(m_reader.GetSectorBuffer().data(), subq);
    }
    else if (!is_data_sector && m_drive_state == DriveState::Playing)
    {
      ProcessCDDASector(m_reader.GetSectorBuffer().data(), subq);
    }
    else if (m_drive_state != DriveState::Reading && m_drive_state != DriveState::Playing)
    {
      Panic("Not reading or playing");
    }
    else
    {
      Log_WarningPrintf("Skipping sector %u as it is a %s sector and we're not %s", m_reader.GetLastReadSector(),
                        is_data_sector ? "data" : "audio", is_data_sector ? "reading" : "playing");
    }
  }
  else
  {
    const CDImage::Position pos(CDImage::Position::FromLBA(m_reader.GetLastReadSector()));
    Log_DevPrintf("Skipping sector %u [%02u:%02u:%02u] due to invalid subchannel Q", m_reader.GetLastReadSector(),
                  pos.minute, pos.second, pos.frame);
  }

  m_last_requested_sector++;
  m_reader.QueueReadSector(m_last_requested_sector);
}

void CDROM::ProcessDataSectorHeader(const u8* raw_sector, bool set_valid)
{
  std::memcpy(&m_last_sector_header, &raw_sector[SECTOR_SYNC_SIZE], sizeof(m_last_sector_header));
  std::memcpy(&m_last_sector_subheader, &raw_sector[SECTOR_SYNC_SIZE + sizeof(m_last_sector_header)],
              sizeof(m_last_sector_subheader));
  m_secondary_status.header_valid |= set_valid;
}

void CDROM::ProcessDataSector(const u8* raw_sector, const CDImage::SubChannelQ& subq)
{
  ProcessDataSectorHeader(raw_sector, true);

  Log_DevPrintf("Read sector %u: mode %u submode 0x%02X", m_last_requested_sector,
                ZeroExtend32(m_last_sector_header.sector_mode), ZeroExtend32(m_last_sector_subheader.submode.bits));

  if (m_mode.xa_enable && m_last_sector_header.sector_mode == 2)
  {
    if (m_last_sector_subheader.submode.eof)
    {
      Log_WarningPrintf("End of CD-XA file");
    }

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
        ProcessXAADPCMSector(raw_sector, subq);
      }

      // Audio+realtime sectors aren't delivered to the CPU.
      return;
    }
  }

  // Deliver to CPU
  if (HasPendingAsyncInterrupt())
  {
    Log_WarningPrintf("Data interrupt was not delivered");
    ClearAsyncInterrupt();
  }
  if (!m_sector_buffer.empty())
  {
    Log_WarningPrintf("Sector buffer was not empty");
  }

  Assert(!m_mode.ignore_bit);
  if (m_mode.read_raw_sector)
  {
    m_sector_buffer.resize(RAW_SECTOR_OUTPUT_SIZE);
    std::memcpy(m_sector_buffer.data(), raw_sector + SECTOR_SYNC_SIZE, RAW_SECTOR_OUTPUT_SIZE);
  }
  else
  {
    // TODO: This should actually depend on the mode...
    Assert(m_last_sector_header.sector_mode == 2);
    m_sector_buffer.resize(DATA_SECTOR_OUTPUT_SIZE);
    std::memcpy(m_sector_buffer.data(), raw_sector + CDImage::SECTOR_SYNC_SIZE + 12, DATA_SECTOR_OUTPUT_SIZE);
  }

  m_async_response_fifo.Push(m_secondary_status.bits);
  SetAsyncInterrupt(Interrupt::DataReady);
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

static constexpr s32 ApplyVolume(s16 sample, u8 volume)
{
  return s32(sample) * static_cast<s32>(ZeroExtend32(volume)) >> 7;
}

static constexpr s16 SaturateVolume(s32 volume)
{
  return static_cast<s16>(std::clamp<s32>(volume, -0x8000, 0x7FFF));
}

template<bool STEREO, bool SAMPLE_RATE>
static void ResampleXAADPCM(const s16* frames_in, u32 num_frames_in, SPU* spu, s16* left_ringbuf, s16* right_ringbuf,
                            u8* p_ptr, u8* sixstep_ptr, const std::array<std::array<u8, 2>, 2>& volume_matrix)
{
  u8 p = *p_ptr;
  u8 sixstep = *sixstep_ptr;

  spu->EnsureCDAudioSpace(((num_frames_in * 7) / 6) << BoolToUInt8(SAMPLE_RATE));

  for (u32 in_sample_index = 0; in_sample_index < num_frames_in; in_sample_index++)
  {
    const s16 left = *(frames_in++);
    const s16 right = STEREO ? *(frames_in++) : left;

    if constexpr (!STEREO)
    {
      UNREFERENCED_VARIABLE(right);
    }

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

          const s16 left_out = SaturateVolume(ApplyVolume(left_interp, volume_matrix[0][0]) +
                                              ApplyVolume(right_interp, volume_matrix[1][0]));
          const s16 right_out = SaturateVolume(ApplyVolume(left_interp, volume_matrix[1][0]) +
                                               ApplyVolume(right_interp, volume_matrix[1][1]));

          spu->AddCDAudioSample(left_out, right_out);
        }
      }
    }
  }

  *p_ptr = p;
  *sixstep_ptr = sixstep;
}

void CDROM::ProcessXAADPCMSector(const u8* raw_sector, const CDImage::SubChannelQ& subq)
{
  std::array<s16, CDXA::XA_ADPCM_SAMPLES_PER_SECTOR_4BIT> sample_buffer;
  CDXA::DecodeADPCMSector(raw_sector, sample_buffer.data(), m_xa_last_samples.data());

  // Only send to SPU if we're not muted.
  if (m_muted || m_adpcm_muted)
    return;

  m_spu->GeneratePendingSamples();

  if (m_last_sector_subheader.codinginfo.IsStereo())
  {
    const u32 num_samples = m_last_sector_subheader.codinginfo.GetSamplesPerSector() / 2;
    if (m_last_sector_subheader.codinginfo.IsHalfSampleRate())
    {
      ResampleXAADPCM<true, true>(sample_buffer.data(), num_samples, m_spu, m_xa_resample_ring_buffer[0].data(),
                                  m_xa_resample_ring_buffer[1].data(), &m_xa_resample_p, &m_xa_resample_sixstep,
                                  m_cd_audio_volume_matrix);
    }
    else
    {
      ResampleXAADPCM<true, false>(sample_buffer.data(), num_samples, m_spu, m_xa_resample_ring_buffer[0].data(),
                                   m_xa_resample_ring_buffer[1].data(), &m_xa_resample_p, &m_xa_resample_sixstep,
                                   m_cd_audio_volume_matrix);
    }
  }
  else
  {
    const u32 num_samples = m_last_sector_subheader.codinginfo.GetSamplesPerSector();
    if (m_last_sector_subheader.codinginfo.IsHalfSampleRate())
    {
      ResampleXAADPCM<false, true>(sample_buffer.data(), num_samples, m_spu, m_xa_resample_ring_buffer[0].data(),
                                   m_xa_resample_ring_buffer[1].data(), &m_xa_resample_p, &m_xa_resample_sixstep,
                                   m_cd_audio_volume_matrix);
    }
    else
    {
      ResampleXAADPCM<false, false>(sample_buffer.data(), num_samples, m_spu, m_xa_resample_ring_buffer[0].data(),
                                    m_xa_resample_ring_buffer[1].data(), &m_xa_resample_p, &m_xa_resample_sixstep,
                                    m_cd_audio_volume_matrix);
    }
  }
}

void CDROM::ProcessCDDASector(const u8* raw_sector, const CDImage::SubChannelQ& subq)
{
  // For CDDA sectors, the whole sector contains the audio data.
  Log_DevPrintf("Read sector %u as CDDA", m_last_requested_sector);

  // Skip the pregap, and don't report on it.
  if (subq.index_number_bcd == 0)
    return;

  if (m_mode.report_audio)
  {
    const u8 frame_nibble = subq.absolute_frame_bcd >> 4;
    if (m_last_cdda_report_frame_nibble != frame_nibble && (m_cdda_report_delay == 0 || --m_cdda_report_delay == 0))
    {
      m_last_cdda_report_frame_nibble = frame_nibble;

      Log_DebugPrintf("CDDA report at track[%02x] index[%02x] rel[%02x:%02x:%02x]", subq.track_number_bcd,
                      subq.index_number_bcd, subq.relative_minute_bcd, subq.relative_second_bcd,
                      subq.relative_frame_bcd);

      ClearAsyncInterrupt();
      m_async_response_fifo.Push(m_secondary_status.bits);
      m_async_response_fifo.Push(subq.track_number_bcd);
      m_async_response_fifo.Push(subq.index_number_bcd);
      if (subq.absolute_frame_bcd & 0x10)
      {
        m_async_response_fifo.Push(subq.relative_minute_bcd);
        m_async_response_fifo.Push(0x80 | subq.relative_second_bcd);
        m_async_response_fifo.Push(subq.relative_frame_bcd);
      }
      else
      {
        m_async_response_fifo.Push(subq.absolute_minute_bcd);
        m_async_response_fifo.Push(subq.absolute_second_bcd);
        m_async_response_fifo.Push(subq.absolute_frame_bcd);
      }

      m_async_response_fifo.Push(0); // peak low
      m_async_response_fifo.Push(0); // peak high
      SetAsyncInterrupt(Interrupt::DataReady);
    }
  }

  // Apply volume when pushing sectors to SPU.
  if (m_muted)
    return;

  m_spu->GeneratePendingSamples();

  constexpr bool is_stereo = true;
  constexpr u32 num_samples = RAW_SECTOR_OUTPUT_SIZE / sizeof(s16) / (is_stereo ? 2 : 1);
  m_spu->EnsureCDAudioSpace(num_samples);

  const u8* sector_ptr = raw_sector;
  for (u32 i = 0; i < num_samples; i++)
  {
    s16 samp_left, samp_right;
    std::memcpy(&samp_left, sector_ptr, sizeof(samp_left));
    std::memcpy(&samp_right, sector_ptr + sizeof(s16), sizeof(samp_right));
    sector_ptr += sizeof(s16) * 2;

    const s16 left = SaturateVolume(ApplyVolume(samp_left, m_cd_audio_volume_matrix[0][0]) +
                                    ApplyVolume(samp_right, m_cd_audio_volume_matrix[0][1]));
    const s16 right = SaturateVolume(ApplyVolume(samp_left, m_cd_audio_volume_matrix[1][0]) +
                                     ApplyVolume(samp_right, m_cd_audio_volume_matrix[1][1]));
    m_spu->AddCDAudioSample(left, right);
  }
}

void CDROM::LoadDataFIFO()
{
  // any data to load?
  if (m_sector_buffer.empty())
  {
    Log_DevPrintf("Attempting to load empty sector buffer");
    return;
  }

  m_data_fifo.Clear();
  m_data_fifo.PushRange(m_sector_buffer.data(), static_cast<u32>(m_sector_buffer.size()));
  m_sector_buffer.clear();

  Log_DebugPrintf("Loaded %u bytes to data FIFO", m_data_fifo.GetSize());
}

void CDROM::DrawDebugWindow()
{
  static const ImVec4 active_color{1.0f, 1.0f, 1.0f, 1.0f};
  static const ImVec4 inactive_color{0.4f, 0.4f, 0.4f, 1.0f};
  const float framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale.x;

  ImGui::SetNextWindowSize(ImVec2(800.0f * framebuffer_scale, 500.0f * framebuffer_scale), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("CDROM State", &m_system->GetSettings().debugging.show_cdrom_state))
  {
    ImGui::End();
    return;
  }

  // draw voice states
  if (ImGui::CollapsingHeader("Media", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (HasMedia())
    {
      const CDImage* media = m_reader.GetMedia();
      const auto [disc_minute, disc_second, disc_frame] = media->GetMSFPositionOnDisc();
      const auto [track_minute, track_second, track_frame] = media->GetMSFPositionInTrack();

      ImGui::Text("Filename: %s", media->GetFileName().c_str());
      ImGui::Text("Disc Position: MSF[%02u:%02u:%02u] LBA[%u]", disc_minute, disc_second, disc_frame,
                  media->GetPositionOnDisc());
      ImGui::Text("Track Position: Number[%u] MSF[%02u:%02u:%02u] LBA[%u]", media->GetTrackNumber(), track_minute,
                  track_second, track_frame, media->GetPositionInTrack());
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
    static constexpr std::array<const char*, 11> drive_state_names = {
      {"Idle", "Spinning Up", "Seeking (Physical)", "Seeking (Logical)", "Reading ID", "Reading TOC", "Reading",
       "Playing", "Pausing", "Stopping", "Changing Session"}};

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
    ImGui::TextColored(m_secondary_status.header_valid ? active_color : inactive_color, "Header Valid: %s",
                       m_secondary_status.header_valid ? "Yes" : "No");
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

    if (HasPendingCommand())
    {
      ImGui::TextColored(active_color, "Command: 0x%02X (%d ticks remaining)", static_cast<u8>(m_command),
                         m_command_event->IsActive() ? m_command_event->GetTicksUntilNextExecution() : 0);
    }
    else
    {
      ImGui::TextColored(inactive_color, "Command: None");
    }

    if (IsDriveIdle())
    {
      ImGui::TextColored(inactive_color, "Drive: Idle");
    }
    else
    {
      ImGui::TextColored(active_color, "Drive: %s (%d ticks remaining)",
                         drive_state_names[static_cast<u8>(m_drive_state)],
                         m_drive_event->IsActive() ? m_drive_event->GetTicksUntilNextExecution() : 0);
    }

    ImGui::Text("Interrupt Enable Register: 0x%02X", m_interrupt_enable_register);
    ImGui::Text("Interrupt Flag Register: 0x%02X", m_interrupt_flag_register);
  }

  if (ImGui::CollapsingHeader("CD Audio", ImGuiTreeNodeFlags_DefaultOpen))
  {
    const bool playing_anything =
      (m_secondary_status.header_valid && m_mode.xa_enable) || m_secondary_status.playing_cdda;
    ImGui::TextColored(playing_anything ? active_color : inactive_color, "Playing: %s",
                       (m_secondary_status.header_valid && m_mode.xa_enable) ?
                         "XA-ADPCM" :
                         (m_secondary_status.playing_cdda ? "CDDA" : "Disabled"));
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
