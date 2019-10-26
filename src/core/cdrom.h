#pragma once
#include "common/bitfield.h"
#include "common/cd_image.h"
#include "common/cd_xa.h"
#include "common/fifo_queue.h"
#include "types.h"
#include <array>
#include <string>
#include <vector>

class StateWrapper;

class System;
class DMA;
class InterruptController;
class SPU;

class CDROM
{
public:
  enum : u32
  {
    RAW_SECTOR_SIZE = CDImage::RAW_SECTOR_SIZE,
    SECTOR_SYNC_SIZE = CDImage::SECTOR_SYNC_SIZE,
    SECTOR_HEADER_SIZE = CDImage::SECTOR_HEADER_SIZE,
    XA_RESAMPLE_RING_BUFFER_SIZE = 32,
    XA_RESAMPLE_ZIGZAG_TABLE_SIZE = 29,
    XA_RESAMPLE_NUM_ZIGZAG_TABLES = 7
  };

  CDROM();
  ~CDROM();

  bool Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, SPU* spu);
  void Reset();
  bool DoState(StateWrapper& sw);

  bool HasMedia() const { return static_cast<bool>(m_media); }
  bool InsertMedia(const char* filename);
  void RemoveMedia();

  // I/O
  u8 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u8 value);
  void DMARead(u32* words, u32 word_count);

  void Execute(TickCount ticks);

  // Render statistics debug window.
  void DrawDebugWindow();

private:
  static constexpr u32 PARAM_FIFO_SIZE = 16;
  static constexpr u32 RESPONSE_FIFO_SIZE = 16;
  static constexpr u32 DATA_FIFO_SIZE = 4096;
  static constexpr u32 NUM_INTERRUPTS = 32;
  static constexpr u32 SECTOR_BUFFER_SIZE = (2352 - 12);
  static constexpr u8 INTERRUPT_REGISTER_MASK = 0x1F;

  enum class Interrupt : u8
  {
    INT1 = 0x01,
    INT2 = 0x02,
    ACK = 0x03,
    INT4 = 0x04,
    INT5 = 0x05
  };

  enum class Command : u8
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
    VideoCD = 0x1F
  };

  enum class CommandState : u32
  {
    Idle,
    WaitForExecute,
    WaitForIRQClear
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

  union SecondaryStatusRegister
  {
    u8 bits;
    BitField<u8, bool, 0, 1> error;
    BitField<u8, bool, 1, 1> motor_on;
    BitField<u8, bool, 2, 1> seek_error;
    BitField<u8, bool, 3, 1> id_error;
    BitField<u8, bool, 4, 1> shell_open;
    BitField<u8, bool, 5, 1> reading;
    BitField<u8, bool, 6, 1> seeking;
    BitField<u8, bool, 7, 1> playing_cdda;

    bool IsReadingOrPlaying() const { return reading || playing_cdda; }
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

  bool HasPendingInterrupt() const { return m_interrupt_flag_register != 0; }
  void SetInterrupt(Interrupt interrupt);
  void PushStatResponse(Interrupt interrupt = Interrupt::ACK);
  void SendErrorResponse(u8 reason = 0x80);
  void UpdateStatusRegister();

  u32 GetAckDelayForCommand() const;
  u32 GetTicksForRead() const;
  void BeginCommand(Command command); // also update status register
  void NextCommandStage(bool wait_for_irq, u32 time);
  void EndCommand(); // also updates status register
  void ExecuteCommand();
  void ExecuteTestCommand(u8 subcommand);
  void BeginReading(bool cdda);
  void DoSectorRead();
  void ProcessDataSector();
  void ProcessXAADPCMSector();
  void ProcessCDDASector();
  void StopReading();
  void LoadDataFIFO();

  System* m_system = nullptr;
  DMA* m_dma = nullptr;
  InterruptController* m_interrupt_controller = nullptr;
  SPU* m_spu = nullptr;
  std::unique_ptr<CDImage> m_media;

  CommandState m_command_state = CommandState::Idle;
  Command m_command = Command::Sync;
  u32 m_command_stage = 0;
  TickCount m_command_remaining_ticks = 0;

  TickCount m_sector_read_remaining_ticks = 0;
  bool m_muted = false;
  bool m_adpcm_muted = false;

  StatusRegister m_status = {};
  SecondaryStatusRegister m_secondary_status = {};
  ModeRegister m_mode = {};

  u8 m_interrupt_enable_register = INTERRUPT_REGISTER_MASK;
  u8 m_interrupt_flag_register = 0;

  CDImage::Position m_pending_location = {};
  bool m_location_pending = false;

  u8 m_filter_file_number = 0;
  u8 m_filter_channel_number = 0;

  CDImage::SectorHeader m_last_sector_header = {};
  CDXA::XASubHeader m_last_sector_subheader = {};

  std::array<std::array<u8, 2>, 2> m_cd_audio_volume_matrix{};
  std::array<std::array<u8, 2>, 2> m_next_cd_audio_volume_matrix{};

  std::array<s32, 4> m_xa_last_samples{};
  std::array<std::array<s16, XA_RESAMPLE_RING_BUFFER_SIZE>, 2> m_xa_resample_ring_buffer{};
  u8 m_xa_resample_p = 0;
  u8 m_xa_resample_sixstep = 6;

  InlineFIFOQueue<u8, PARAM_FIFO_SIZE> m_param_fifo;
  InlineFIFOQueue<u8, RESPONSE_FIFO_SIZE> m_response_fifo;
  HeapFIFOQueue<u8, DATA_FIFO_SIZE> m_data_fifo;
  std::vector<u8> m_sector_buffer;
};
