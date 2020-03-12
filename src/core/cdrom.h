#pragma once
#include "cdrom_async_reader.h"
#include "common/bitfield.h"
#include "common/cd_image.h"
#include "common/cd_xa.h"
#include "common/fifo_queue.h"
#include "common/heap_array.h"
#include "types.h"
#include <array>
#include <string>
#include <vector>

class StateWrapper;

class System;
class TimingEvent;
class DMA;
class InterruptController;
class SPU;

class CDROM
{
public:
  CDROM();
  ~CDROM();

  void Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, SPU* spu);
  void Reset();
  bool DoState(StateWrapper& sw);

  bool HasMedia() const;
  std::string GetMediaFileName() const;
  void InsertMedia(std::unique_ptr<CDImage> media);
  void RemoveMedia();

  // I/O
  u8 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u8 value);
  void DMARead(u32* words, u32 word_count);

  // Render statistics debug window.
  void DrawDebugWindow();

  void SetUseReadThread(bool enabled);

private:
  enum : u32
  {
    RAW_SECTOR_OUTPUT_SIZE = CDImage::RAW_SECTOR_SIZE - CDImage::SECTOR_SYNC_SIZE,
    DATA_SECTOR_OUTPUT_SIZE = CDImage::DATA_SECTOR_SIZE,
    SECTOR_SYNC_SIZE = CDImage::SECTOR_SYNC_SIZE,
    SECTOR_HEADER_SIZE = CDImage::SECTOR_HEADER_SIZE,
    XA_RESAMPLE_RING_BUFFER_SIZE = 32,
    XA_RESAMPLE_ZIGZAG_TABLE_SIZE = 29,
    XA_RESAMPLE_NUM_ZIGZAG_TABLES = 7,

    PARAM_FIFO_SIZE = 16,
    RESPONSE_FIFO_SIZE = 16,
    DATA_FIFO_SIZE = RAW_SECTOR_OUTPUT_SIZE,
    NUM_INTERRUPTS = 32
  };

  static constexpr u8 INTERRUPT_REGISTER_MASK = 0x1F;

  enum class Interrupt : u8
  {
    DataReady = 0x01,
    Complete = 0x02,
    ACK = 0x03,
    DataEnd = 0x04,
    Error = 0x05
  };

  enum class Command : u16
  {
    Sync = 0x00,
    Getstat = 0x01,
    Setloc = 0x02,
    Play = 0x03,
    Forward = 0x04,
    Backward = 0x05,
    ReadN = 0x06,
    MotorOn = 0x07,
    Stop = 0x08,
    Pause = 0x09,
    Init = 0x0A,
    Mute = 0x0B,
    Demute = 0x0C,
    Setfilter = 0x0D,
    Setmode = 0x0E,
    Getparam = 0x0F,
    GetlocL = 0x10,
    GetlocP = 0x11,
    SetSession = 0x12,
    GetTN = 0x13,
    GetTD = 0x14,
    SeekL = 0x15,
    SeekP = 0x16,
    SetClock = 0x17,
    GetClock = 0x18,
    Test = 0x19,
    GetID = 0x1A,
    ReadS = 0x1B,
    Reset = 0x1C,
    GetQ = 0x1D,
    ReadTOC = 0x1E,
    VideoCD = 0x1F,

    None = 0xFFFF
  };

  enum class DriveState : u8
  {
    Idle,
    SpinningUp,
    SeekingPhysical,
    SeekingLogical,
    ReadingID,
    ReadingTOC,
    Reading,
    Playing,
    Pausing,
    Stopping,
    ChangingSession
  };

  union StatusRegister
  {
    u8 bits;
    BitField<u8, u8, 0, 2> index;
    BitField<u8, bool, 2, 1> ADPBUSY;
    BitField<u8, bool, 3, 1> PRMEMPTY;
    BitField<u8, bool, 4, 1> PRMWRDY;
    BitField<u8, bool, 5, 1> RSLRRDY;
    BitField<u8, bool, 6, 1> DRQSTS;
    BitField<u8, bool, 7, 1> BUSYSTS;
  };

  enum StatBits : u8
  {
    STAT_ERROR = (1 << 0),
    STAT_MOTOR_ON = (1 << 1),
    STAT_SEEK_ERROR = (1 << 2),
    STAT_ID_ERROR = (1 << 3),
    STAT_SHELL_OPEN = (1 << 4),
    STAT_HEADER_VALID = (1 << 5),
    STAT_SEEKING = (1 << 6),
    STAT_PLAYING_CDDA = (1 << 7)
  };

  union SecondaryStatusRegister
  {
    u8 bits;
    BitField<u8, bool, 0, 1> error;
    BitField<u8, bool, 1, 1> motor_on;
    BitField<u8, bool, 2, 1> seek_error;
    BitField<u8, bool, 3, 1> id_error;
    BitField<u8, bool, 4, 1> shell_open;
    BitField<u8, bool, 5, 1> header_valid;
    BitField<u8, bool, 6, 1> seeking;
    BitField<u8, bool, 7, 1> playing_cdda;

    /// Clears the CDDA/seeking/header valid bits.
    ALWAYS_INLINE void ClearActiveBits()
    {
      bits &= ~(STAT_HEADER_VALID | STAT_SEEKING | STAT_PLAYING_CDDA);
    }
  };

  union ModeRegister
  {
    u8 bits;
    BitField<u8, bool, 0, 1> cdda;
    BitField<u8, bool, 1, 1> auto_pause;
    BitField<u8, bool, 2, 1> report_audio;
    BitField<u8, bool, 3, 1> xa_filter;
    BitField<u8, bool, 4, 1> ignore_bit;
    BitField<u8, bool, 5, 1> read_raw_sector;
    BitField<u8, bool, 6, 1> xa_enable;
    BitField<u8, bool, 7, 1> double_speed;
  };

  union RequestRegister
  {
    u8 bits;
    BitField<u8, bool, 5, 1> SMEN;
    BitField<u8, bool, 6, 1> BFWR;
    BitField<u8, bool, 7, 1> BFRD;
  };

  void SoftReset();

  bool IsDriveIdle() const { return m_drive_state == DriveState::Idle; }
  bool HasPendingCommand() const { return m_command != Command::None; }
  bool HasPendingInterrupt() const { return m_interrupt_flag_register != 0; }
  bool HasPendingAsyncInterrupt() const { return m_pending_async_interrupt != 0; }
  void SetInterrupt(Interrupt interrupt);
  void SetAsyncInterrupt(Interrupt interrupt);
  void ClearAsyncInterrupt();
  void DeliverAsyncInterrupt();
  void SendACKAndStat();
  void SendErrorResponse(u8 stat_bits = STAT_ERROR, u8 reason = 0x80);
  void SendAsyncErrorResponse(u8 stat_bits = STAT_ERROR, u8 reason = 0x80);
  void UpdateStatusRegister();
  void UpdateInterruptRequest();

  TickCount GetAckDelayForCommand() const;
  TickCount GetTicksForRead() const;
  TickCount GetTicksForSeek() const;
  void BeginCommand(Command command); // also update status register
  void EndCommand();                  // also updates status register
  void ExecuteCommand();
  void ExecuteTestCommand(u8 subcommand);
  void UpdateCommandEvent();
  void ExecuteDrive(TickCount ticks_late);
  void BeginReading(TickCount ticks_late = 0);
  void BeginPlaying(u8 track_bcd, TickCount ticks_late = 0);
  void DoSpinUpComplete();
  void DoSeekComplete(TickCount ticks_late);
  void DoPauseComplete();
  void DoStopComplete();
  void DoChangeSessionComplete();
  void DoIDRead();
  void DoTOCRead();
  void DoSectorRead();
  void ProcessDataSectorHeader(const u8* raw_sector, bool set_valid);
  void ProcessDataSector(const u8* raw_sector, const CDImage::SubChannelQ& subq);
  void ProcessXAADPCMSector(const u8* raw_sector, const CDImage::SubChannelQ& subq);
  void ProcessCDDASector(const u8* raw_sector, const CDImage::SubChannelQ& subq);
  void BeginSeeking(bool logical, bool read_after_seek, bool play_after_seek);
  void LoadDataFIFO();

  System* m_system = nullptr;
  DMA* m_dma = nullptr;
  InterruptController* m_interrupt_controller = nullptr;
  SPU* m_spu = nullptr;
  std::unique_ptr<TimingEvent> m_command_event;
  std::unique_ptr<TimingEvent> m_drive_event;

  Command m_command = Command::None;
  DriveState m_drive_state = DriveState::Idle;
  DiscRegion m_disc_region = DiscRegion::Other;

  StatusRegister m_status = {};
  SecondaryStatusRegister m_secondary_status = {};
  ModeRegister m_mode = {};

  u8 m_interrupt_enable_register = INTERRUPT_REGISTER_MASK;
  u8 m_interrupt_flag_register = 0;
  u8 m_pending_async_interrupt = 0;

  CDImage::Position m_setloc_position = {};
  CDImage::LBA m_last_requested_sector{};
  bool m_setloc_pending = false;
  bool m_read_after_seek = false;
  bool m_play_after_seek = false;

  bool m_muted = false;
  bool m_adpcm_muted = false;

  u8 m_filter_file_number = 0;
  u8 m_filter_channel_number = 0;

  CDImage::SectorHeader m_last_sector_header{};
  CDXA::XASubHeader m_last_sector_subheader{};
  CDImage::SubChannelQ m_last_subq{};
  u8 m_last_cdda_report_frame_nibble = 0xFF;
  u8 m_cdda_report_delay = 0x00;
  u8 m_play_track_number_bcd = 0xFF;
  u8 m_async_command_parameter = 0x00;

  std::array<std::array<u8, 2>, 2> m_cd_audio_volume_matrix{};
  std::array<std::array<u8, 2>, 2> m_next_cd_audio_volume_matrix{};

  std::array<s32, 4> m_xa_last_samples{};
  std::array<std::array<s16, XA_RESAMPLE_RING_BUFFER_SIZE>, 2> m_xa_resample_ring_buffer{};
  u8 m_xa_resample_p = 0;
  u8 m_xa_resample_sixstep = 6;

  InlineFIFOQueue<u8, PARAM_FIFO_SIZE> m_param_fifo;
  InlineFIFOQueue<u8, RESPONSE_FIFO_SIZE> m_response_fifo;
  InlineFIFOQueue<u8, RESPONSE_FIFO_SIZE> m_async_response_fifo;
  HeapFIFOQueue<u8, DATA_FIFO_SIZE> m_data_fifo;
  std::vector<u8> m_sector_buffer;

  CDROMAsyncReader m_reader;
};
