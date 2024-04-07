// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "cdrom.h"
#include "cdrom_async_reader.h"
#include "dma.h"
#include "host.h"
#include "host_interface_progress_callback.h"
#include "interrupt_controller.h"
#include "settings.h"
#include "spu.h"
#include "system.h"

#include "util/cd_image.h"
#include "util/cd_xa.h"
#include "util/imgui_manager.h"
#include "util/iso_reader.h"
#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "common/file_system.h"
#include "common/heap_array.h"
#include "common/intrin.h"
#include "common/log.h"

#include "imgui.h"

#include <cmath>
#include <map>
#include <vector>

Log_SetChannel(CDROM);

namespace CDROM {
namespace {

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
  NUM_SECTOR_BUFFERS = 8,
  AUDIO_FIFO_SIZE = 44100 * 2,
  AUDIO_FIFO_LOW_WATERMARK = 10,

  INIT_TICKS = 4000000,
  ID_READ_TICKS = 33868,
  MOTOR_ON_RESPONSE_TICKS = 400000,

  MAX_FAST_FORWARD_RATE = 12,
  FAST_FORWARD_RATE_STEP = 4,

  MINIMUM_INTERRUPT_DELAY = 5000,
  INTERRUPT_DELAY_CYCLES = 2000,
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
  Getmode = 0x0F,
  GetlocL = 0x10,
  GetlocP = 0x11,
  ReadT = 0x12,
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
  ShellOpening,
  UNUSED_Resetting,
  SeekingPhysical,
  SeekingLogical,
  UNUSED_ReadingID,
  UNUSED_ReadingTOC,
  Reading,
  Playing,
  UNUSED_Pausing,
  UNUSED_Stopping,
  ChangingSession,
  SpinningUp,
  SeekingImplicit,
  ChangingSpeedOrTOCRead
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
  STAT_READING = (1 << 5),
  STAT_SEEKING = (1 << 6),
  STAT_PLAYING_CDDA = (1 << 7)
};

enum ErrorReason : u8
{
  ERROR_REASON_INVALID_ARGUMENT = 0x10,
  ERROR_REASON_INCORRECT_NUMBER_OF_PARAMETERS = 0x20,
  ERROR_REASON_INVALID_COMMAND = 0x40,
  ERROR_REASON_NOT_READY = 0x80
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

  /// Clears the CDDA/seeking bits.
  ALWAYS_INLINE void ClearActiveBits() { bits &= ~(STAT_SEEKING | STAT_READING | STAT_PLAYING_CDDA); }

  /// Sets the bits for seeking.
  ALWAYS_INLINE void SetSeeking()
  {
    bits = (bits & ~(STAT_READING | STAT_PLAYING_CDDA)) | (STAT_MOTOR_ON | STAT_SEEKING);
  }

  /// Sets the bits for reading/playing.
  ALWAYS_INLINE void SetReadingBits(bool audio)
  {
    bits = (bits & ~(STAT_SEEKING | STAT_READING | STAT_PLAYING_CDDA)) |
           ((audio) ? (STAT_MOTOR_ON | STAT_PLAYING_CDDA) : (STAT_MOTOR_ON | STAT_READING));
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

} // namespace

static void SoftReset(TickCount ticks_late);

static bool IsDriveIdle();
static bool IsMotorOn();
static bool IsSeeking();
static bool IsReadingOrPlaying();
static bool CanReadMedia();
static bool HasPendingCommand();
static bool HasPendingInterrupt();
static bool HasPendingAsyncInterrupt();
static void AddCDAudioFrame(s16 left, s16 right);

static s32 ApplyVolume(s16 sample, u8 volume);
static s16 SaturateVolume(s32 volume);

static void SetInterrupt(Interrupt interrupt);
static void SetAsyncInterrupt(Interrupt interrupt);
static void ClearAsyncInterrupt();
static void DeliverAsyncInterrupt(void*, TickCount ticks, TickCount ticks_late);
static void QueueDeliverAsyncInterrupt();
static void SendACKAndStat();
static void SendErrorResponse(u8 stat_bits = STAT_ERROR, u8 reason = 0x80);
static void SendAsyncErrorResponse(u8 stat_bits = STAT_ERROR, u8 reason = 0x80);
static void UpdateStatusRegister();
static void UpdateInterruptRequest();
static bool HasPendingDiscEvent();

static TickCount GetAckDelayForCommand(Command command);
static TickCount GetTicksForSpinUp();
static TickCount GetTicksForIDRead();
static TickCount GetTicksForRead();
static TickCount GetTicksForSeek(CDImage::LBA new_lba, bool ignore_speed_change = false);
static TickCount GetTicksForStop(bool motor_was_on);
static TickCount GetTicksForSpeedChange();
static TickCount GetTicksForTOCRead();
static CDImage::LBA GetNextSectorToBeRead();
static bool CompleteSeek();

static void BeginCommand(Command command); // also update status register
static void EndCommand();                  // also updates status register
static void ExecuteCommand(void*, TickCount ticks, TickCount ticks_late);
static void ExecuteTestCommand(u8 subcommand);
static void ExecuteCommandSecondResponse(void*, TickCount ticks, TickCount ticks_late);
static void QueueCommandSecondResponse(Command command, TickCount ticks);
static void ClearCommandSecondResponse();
static void UpdateCommandEvent();
static void ExecuteDrive(void*, TickCount ticks, TickCount ticks_late);
static void ClearDriveState();
static void BeginReading(TickCount ticks_late = 0, bool after_seek = false);
static void BeginPlaying(u8 track, TickCount ticks_late = 0, bool after_seek = false);
static void DoShellOpenComplete(TickCount ticks_late);
static void DoSeekComplete(TickCount ticks_late);
static void DoStatSecondResponse();
static void DoChangeSessionComplete();
static void DoSpinUpComplete();
static void DoSpeedChangeOrImplicitTOCReadComplete();
static void DoIDRead();
static void DoSectorRead();
static void ProcessDataSectorHeader(const u8* raw_sector);
static void ProcessDataSector(const u8* raw_sector, const CDImage::SubChannelQ& subq);
static void ProcessXAADPCMSector(const u8* raw_sector, const CDImage::SubChannelQ& subq);
static void ProcessCDDASector(const u8* raw_sector, const CDImage::SubChannelQ& subq, bool subq_valid);
static void StopReadingWithDataEnd();
static void StartMotor();
static void StopMotor();
static void BeginSeeking(bool logical, bool read_after_seek, bool play_after_seek);
static void UpdatePositionWhileSeeking();
static void UpdatePhysicalPosition(bool update_logical);
static void SetHoldPosition(CDImage::LBA lba, bool update_subq);
static void ResetCurrentXAFile();
static void ResetAudioDecoder();
static void LoadDataFIFO();
static void ClearSectorBuffers();

template<bool STEREO, bool SAMPLE_RATE>
static void ResampleXAADPCM(const s16* frames_in, u32 num_frames_in);

static TinyString LBAToMSFString(CDImage::LBA lba);

static void CreateFileMap();
static void CreateFileMap(IsoReader& iso, const std::string_view& dir);
static const std::string* LookupFileMap(u32 lba, u32* start_lba, u32* end_lba);

static std::unique_ptr<TimingEvent> s_command_event;
static std::unique_ptr<TimingEvent> s_command_second_response_event;
static std::unique_ptr<TimingEvent> s_async_interrupt_event;
static std::unique_ptr<TimingEvent> s_drive_event;

static Command s_command = Command::None;
static Command s_command_second_response = Command::None;
static DriveState s_drive_state = DriveState::Idle;
static DiscRegion s_disc_region = DiscRegion::Other;

static StatusRegister s_status = {};
static SecondaryStatusRegister s_secondary_status = {};
static ModeRegister s_mode = {};

static u8 s_interrupt_enable_register = INTERRUPT_REGISTER_MASK;
static u8 s_interrupt_flag_register = 0;
static u8 s_pending_async_interrupt = 0;
static u32 s_last_interrupt_time = 0;

static CDImage::Position s_setloc_position = {};
static CDImage::LBA s_requested_lba{};
static CDImage::LBA s_current_lba{}; // this is the hold position
static CDImage::LBA s_seek_start_lba{};
static CDImage::LBA s_seek_end_lba{};
static CDImage::LBA s_physical_lba{}; // current position of the disc with respect to time
static u32 s_physical_lba_update_tick = 0;
static u32 s_physical_lba_update_carry = 0;
static bool s_setloc_pending = false;
static bool s_read_after_seek = false;
static bool s_play_after_seek = false;

static bool s_muted = false;
static bool s_adpcm_muted = false;

static u8 s_xa_filter_file_number = 0;
static u8 s_xa_filter_channel_number = 0;
static u8 s_xa_current_file_number = 0;
static u8 s_xa_current_channel_number = 0;
static u8 s_xa_current_set = false;

static CDImage::SectorHeader s_last_sector_header{};
static CDXA::XASubHeader s_last_sector_subheader{};
static bool s_last_sector_header_valid = false; // TODO: Rename to "logical pause" or something.
static CDImage::SubChannelQ s_last_subq{};
static u8 s_last_cdda_report_frame_nibble = 0xFF;
static u8 s_play_track_number_bcd = 0xFF;
static u8 s_async_command_parameter = 0x00;
static s8 s_fast_forward_rate = 0;

static std::array<std::array<u8, 2>, 2> s_cd_audio_volume_matrix{};
static std::array<std::array<u8, 2>, 2> s_next_cd_audio_volume_matrix{};

static std::array<s32, 4> s_xa_last_samples{};
static std::array<std::array<s16, XA_RESAMPLE_RING_BUFFER_SIZE>, 2> s_xa_resample_ring_buffer{};
static u8 s_xa_resample_p = 0;
static u8 s_xa_resample_sixstep = 6;

static InlineFIFOQueue<u8, PARAM_FIFO_SIZE> s_param_fifo;
static InlineFIFOQueue<u8, RESPONSE_FIFO_SIZE> s_response_fifo;
static InlineFIFOQueue<u8, RESPONSE_FIFO_SIZE> s_async_response_fifo;
static HeapFIFOQueue<u8, DATA_FIFO_SIZE> s_data_fifo;

struct SectorBuffer
{
  FixedHeapArray<u8, RAW_SECTOR_OUTPUT_SIZE> data;
  u32 size;
};

static u32 s_current_read_sector_buffer = 0;
static u32 s_current_write_sector_buffer = 0;
static std::array<SectorBuffer, NUM_SECTOR_BUFFERS> s_sector_buffers;

static CDROMAsyncReader s_reader;

// two 16-bit samples packed in 32-bits
static HeapFIFOQueue<u32, AUDIO_FIFO_SIZE> s_audio_fifo;

static std::map<u32, std::pair<u32, std::string>> s_file_map;
static bool s_file_map_created = false;
static bool s_show_current_file = false;

static constexpr std::array<const char*, 15> s_drive_state_names = {
  {"Idle", "Opening Shell", "Resetting", "Seeking (Physical)", "Seeking (Logical)", "Reading ID", "Reading TOC",
   "Reading", "Playing", "Pausing", "Stopping", "Changing Session", "Spinning Up", "Seeking (Implicit)",
   "Changing Speed/Implicit TOC Read"}};

struct CommandInfo
{
  const char* name;
  u8 min_parameters;
  u8 max_parameters;
};

static std::array<CommandInfo, 255> s_command_info = {{
  {"Sync", 0, 0},     {"Getstat", 0, 0},   {"Setloc", 3, 3},  {"Play", 0, 1},    {"Forward", 0, 0}, {"Backward", 0, 0},
  {"ReadN", 0, 0},    {"Standby", 0, 0},   {"Stop", 0, 0},    {"Pause", 0, 0},   {"Init", 0, 0},    {"Mute", 0, 0},
  {"Demute", 0, 0},   {"Setfilter", 2, 2}, {"Setmode", 1, 1}, {"Getmode", 0, 0}, {"GetlocL", 0, 0}, {"GetlocP", 0, 0},
  {"ReadT", 1, 1},    {"GetTN", 0, 0},     {"GetTD", 1, 1},   {"SeekL", 0, 0},   {"SeekP", 0, 0},   {"SetClock", 0, 0},
  {"GetClock", 0, 0}, {"Test", 1, 16},     {"GetID", 0, 0},   {"ReadS", 0, 0},   {"Reset", 0, 0},   {"GetQ", 2, 2},
  {"ReadTOC", 0, 0},  {"VideoCD", 6, 16},  {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0}, {"Unknown", 0, 0},
  {"Unknown", 0, 0},  {"Unknown", 0, 0},   {nullptr, 0, 0} // Unknown
}};

} // namespace CDROM

void CDROM::Initialize()
{
  s_command_event =
    TimingEvents::CreateTimingEvent("CDROM Command Event", 1, 1, &CDROM::ExecuteCommand, nullptr, false);
  s_command_second_response_event = TimingEvents::CreateTimingEvent(
    "CDROM Command Second Response Event", 1, 1, &CDROM::ExecuteCommandSecondResponse, nullptr, false);
  s_async_interrupt_event = TimingEvents::CreateTimingEvent("CDROM Async Interrupt Event", INTERRUPT_DELAY_CYCLES, 1,
                                                            &CDROM::DeliverAsyncInterrupt, nullptr, false);
  s_drive_event = TimingEvents::CreateTimingEvent("CDROM Drive Event", 1, 1, &CDROM::ExecuteDrive, nullptr, false);

  if (g_settings.cdrom_readahead_sectors > 0)
    s_reader.StartThread(g_settings.cdrom_readahead_sectors);

  Reset();
}

void CDROM::Shutdown()
{
  s_file_map.clear();
  s_file_map_created = false;
  s_show_current_file = false;

  s_drive_event.reset();
  s_async_interrupt_event.reset();
  s_command_second_response_event.reset();
  s_command_event.reset();
  s_reader.StopThread();
  s_reader.RemoveMedia();
}

void CDROM::Reset()
{
  s_command = Command::None;
  s_command_event->Deactivate();
  ClearCommandSecondResponse();
  ClearDriveState();
  s_status.bits = 0;
  s_secondary_status.bits = 0;
  s_secondary_status.motor_on = CanReadMedia();
  s_secondary_status.shell_open = !CanReadMedia();
  s_mode.bits = 0;
  s_mode.read_raw_sector = true;
  s_interrupt_enable_register = INTERRUPT_REGISTER_MASK;
  s_interrupt_flag_register = 0;
  s_last_interrupt_time = System::GetGlobalTickCounter() - MINIMUM_INTERRUPT_DELAY;
  ClearAsyncInterrupt();
  s_setloc_position = {};
  s_seek_start_lba = 0;
  s_seek_end_lba = 0;
  s_setloc_pending = false;
  s_read_after_seek = false;
  s_play_after_seek = false;
  s_muted = false;
  s_adpcm_muted = false;
  s_xa_filter_file_number = 0;
  s_xa_filter_channel_number = 0;
  s_xa_current_file_number = 0;
  s_xa_current_channel_number = 0;
  s_xa_current_set = false;
  std::memset(&s_last_sector_header, 0, sizeof(s_last_sector_header));
  std::memset(&s_last_sector_subheader, 0, sizeof(s_last_sector_subheader));
  s_last_sector_header_valid = false;
  std::memset(&s_last_subq, 0, sizeof(s_last_subq));
  s_last_cdda_report_frame_nibble = 0xFF;

  s_next_cd_audio_volume_matrix[0][0] = 0x80;
  s_next_cd_audio_volume_matrix[0][1] = 0x00;
  s_next_cd_audio_volume_matrix[1][0] = 0x00;
  s_next_cd_audio_volume_matrix[1][1] = 0x80;
  s_cd_audio_volume_matrix = s_next_cd_audio_volume_matrix;
  ResetAudioDecoder();

  s_param_fifo.Clear();
  s_response_fifo.Clear();
  s_async_response_fifo.Clear();
  s_data_fifo.Clear();

  s_current_read_sector_buffer = 0;
  s_current_write_sector_buffer = 0;
  for (u32 i = 0; i < NUM_SECTOR_BUFFERS; i++)
  {
    s_sector_buffers[i].data.fill(0);
    s_sector_buffers[i].size = 0;
  }

  UpdateStatusRegister();

  SetHoldPosition(0, true);
}

void CDROM::SoftReset(TickCount ticks_late)
{
  const bool was_double_speed = s_mode.double_speed;

  ClearCommandSecondResponse();
  ClearDriveState();
  s_secondary_status.bits = 0;
  s_secondary_status.motor_on = CanReadMedia();
  s_secondary_status.shell_open = !CanReadMedia();
  s_mode.bits = 0;
  s_mode.read_raw_sector = true;
  ClearAsyncInterrupt();
  s_setloc_position = {};
  s_setloc_pending = false;
  s_read_after_seek = false;
  s_play_after_seek = false;
  s_muted = false;
  s_adpcm_muted = false;
  s_last_cdda_report_frame_nibble = 0xFF;

  ResetAudioDecoder();

  s_param_fifo.Clear();
  s_async_response_fifo.Clear();
  s_data_fifo.Clear();

  s_current_read_sector_buffer = 0;
  s_current_write_sector_buffer = 0;
  for (u32 i = 0; i < NUM_SECTOR_BUFFERS; i++)
  {
    s_sector_buffers[i].data.fill(0);
    s_sector_buffers[i].size = 0;
  }

  UpdateStatusRegister();

  if (HasMedia())
  {
    const TickCount speed_change_ticks = was_double_speed ? GetTicksForSpeedChange() : 0;
    const TickCount seek_ticks = (s_current_lba != 0) ? GetTicksForSeek(0) : 0;
    const TickCount total_ticks = std::max<TickCount>(speed_change_ticks + seek_ticks, INIT_TICKS) - ticks_late;
    Log_DevPrintf("CDROM init total disc ticks = %d (speed change = %d, seek = %d)", total_ticks, speed_change_ticks,
                  seek_ticks);

    if (s_current_lba != 0)
    {
      s_drive_state = DriveState::SeekingImplicit;
      s_drive_event->SetIntervalAndSchedule(total_ticks);
      s_requested_lba = 0;
      s_reader.QueueReadSector(s_requested_lba);
      s_seek_start_lba = s_current_lba;
      s_seek_end_lba = 0;
    }
    else
    {
      s_drive_state = DriveState::ChangingSpeedOrTOCRead;
      s_drive_event->Schedule(total_ticks);
    }
  }
}

bool CDROM::DoState(StateWrapper& sw)
{
  sw.Do(&s_command);
  sw.DoEx(&s_command_second_response, 53, Command::None);
  sw.Do(&s_drive_state);
  sw.Do(&s_status.bits);
  sw.Do(&s_secondary_status.bits);
  sw.Do(&s_mode.bits);

  bool current_double_speed = s_mode.double_speed;
  sw.Do(&current_double_speed);

  sw.Do(&s_interrupt_enable_register);
  sw.Do(&s_interrupt_flag_register);
  sw.DoEx(&s_last_interrupt_time, 57, System::GetGlobalTickCounter() - MINIMUM_INTERRUPT_DELAY);
  sw.Do(&s_pending_async_interrupt);
  sw.DoPOD(&s_setloc_position);
  sw.Do(&s_current_lba);
  sw.Do(&s_seek_start_lba);
  sw.Do(&s_seek_end_lba);
  sw.DoEx(&s_physical_lba, 49, s_current_lba);
  sw.DoEx(&s_physical_lba_update_tick, 49, static_cast<u32>(0));
  sw.DoEx(&s_physical_lba_update_carry, 54, static_cast<u32>(0));
  sw.Do(&s_setloc_pending);
  sw.Do(&s_read_after_seek);
  sw.Do(&s_play_after_seek);
  sw.Do(&s_muted);
  sw.Do(&s_adpcm_muted);
  sw.Do(&s_xa_filter_file_number);
  sw.Do(&s_xa_filter_channel_number);
  sw.Do(&s_xa_current_file_number);
  sw.Do(&s_xa_current_channel_number);
  sw.Do(&s_xa_current_set);
  sw.DoBytes(&s_last_sector_header, sizeof(s_last_sector_header));
  sw.DoBytes(&s_last_sector_subheader, sizeof(s_last_sector_subheader));
  sw.Do(&s_last_sector_header_valid);
  sw.DoBytes(&s_last_subq, sizeof(s_last_subq));
  sw.Do(&s_last_cdda_report_frame_nibble);
  sw.Do(&s_play_track_number_bcd);
  sw.Do(&s_async_command_parameter);

  sw.DoEx(&s_fast_forward_rate, 49, static_cast<s8>(0));

  sw.Do(&s_cd_audio_volume_matrix);
  sw.Do(&s_next_cd_audio_volume_matrix);
  sw.Do(&s_xa_last_samples);
  sw.Do(&s_xa_resample_ring_buffer);
  sw.Do(&s_xa_resample_p);
  sw.Do(&s_xa_resample_sixstep);
  sw.Do(&s_param_fifo);
  sw.Do(&s_response_fifo);
  sw.Do(&s_async_response_fifo);
  sw.Do(&s_data_fifo);

  sw.Do(&s_current_read_sector_buffer);
  sw.Do(&s_current_write_sector_buffer);
  for (u32 i = 0; i < NUM_SECTOR_BUFFERS; i++)
  {
    sw.Do(&s_sector_buffers[i].data);
    sw.Do(&s_sector_buffers[i].size);
  }

  sw.Do(&s_audio_fifo);
  sw.Do(&s_requested_lba);

  if (sw.IsReading())
  {
    if (s_reader.HasMedia())
      s_reader.QueueReadSector(s_requested_lba);
    UpdateCommandEvent();
    s_drive_event->SetState(!IsDriveIdle());

    // Time will get fixed up later.
    s_command_second_response_event->SetState(s_command_second_response != Command::None);
  }

  return !sw.HasError();
}

bool CDROM::HasMedia()
{
  return s_reader.HasMedia();
}

const std::string& CDROM::GetMediaFileName()
{
  return s_reader.GetMediaFileName();
}

const CDImage* CDROM::GetMedia()
{
  return s_reader.GetMedia();
}

DiscRegion CDROM::GetDiscRegion()
{
  return s_disc_region;
}

bool CDROM::IsMediaPS1Disc()
{
  return (s_disc_region != DiscRegion::NonPS1);
}

bool CDROM::IsMediaAudioCD()
{
  if (!s_reader.HasMedia())
    return false;

  // Check for an audio track as the first track.
  return (s_reader.GetMedia()->GetTrackMode(1) == CDImage::TrackMode::Audio);
}

bool CDROM::DoesMediaRegionMatchConsole()
{
  if (!g_settings.cdrom_region_check)
    return true;

  if (s_disc_region == DiscRegion::Other)
    return false;

  return System::GetRegion() == System::GetConsoleRegionForDiscRegion(s_disc_region);
}

bool CDROM::IsDriveIdle()
{
  return s_drive_state == DriveState::Idle;
}

bool CDROM::IsMotorOn()
{
  return s_secondary_status.motor_on;
}

bool CDROM::IsSeeking()
{
  return (s_drive_state == DriveState::SeekingLogical || s_drive_state == DriveState::SeekingPhysical ||
          s_drive_state == DriveState::SeekingImplicit);
}

bool CDROM::IsReadingOrPlaying()
{
  return (s_drive_state == DriveState::Reading || s_drive_state == DriveState::Playing);
}

bool CDROM::CanReadMedia()
{
  return (s_drive_state != DriveState::ShellOpening && s_reader.HasMedia());
}

void CDROM::InsertMedia(std::unique_ptr<CDImage> media, DiscRegion region)
{
  if (CanReadMedia())
    RemoveMedia(true);

  Log_InfoPrintf("Inserting new media, disc region: %s, console region: %s", Settings::GetDiscRegionName(region),
                 Settings::GetConsoleRegionName(System::GetRegion()));

  s_disc_region = region;
  s_reader.SetMedia(std::move(media));
  SetHoldPosition(0, true);

  // motor automatically spins up
  if (s_drive_state != DriveState::ShellOpening)
    StartMotor();

  if (s_show_current_file)
    CreateFileMap();
}

std::unique_ptr<CDImage> CDROM::RemoveMedia(bool for_disc_swap)
{
  if (!HasMedia())
    return nullptr;

  // Add an additional two seconds to the disc swap, some games don't like it happening too quickly.
  TickCount stop_ticks = GetTicksForStop(true);
  if (for_disc_swap)
    stop_ticks += System::ScaleTicksToOverclock(System::MASTER_CLOCK * 2);

  Log_InfoPrintf("Removing CD...");
  std::unique_ptr<CDImage> image = s_reader.RemoveMedia();

  if (s_show_current_file)
    CreateFileMap();

  s_last_sector_header_valid = false;

  s_secondary_status.motor_on = false;
  s_secondary_status.shell_open = true;
  s_secondary_status.ClearActiveBits();
  s_disc_region = DiscRegion::NonPS1;

  // If the drive was doing anything, we need to abort the command.
  ClearDriveState();
  ClearCommandSecondResponse();
  s_command = Command::None;
  s_command_event->Deactivate();

  // The console sends an interrupt when the shell is opened regardless of whether a command was executing.
  if (HasPendingAsyncInterrupt())
    ClearAsyncInterrupt();
  SendAsyncErrorResponse(STAT_ERROR, 0x08);

  // Begin spin-down timer, we can't swap the new disc in immediately for some games (e.g. Metal Gear Solid).
  if (for_disc_swap)
  {
    s_drive_state = DriveState::ShellOpening;
    s_drive_event->SetIntervalAndSchedule(stop_ticks);
  }

  return image;
}

bool CDROM::PrecacheMedia()
{
  if (!s_reader.HasMedia())
    return false;

  if (s_reader.GetMedia()->HasSubImages() && s_reader.GetMedia()->GetSubImageCount() > 1)
  {
    Host::AddFormattedOSDMessage(15.0f,
                                 TRANSLATE("OSDMessage", "CD image preloading not available for multi-disc image '%s'"),
                                 FileSystem::GetDisplayNameFromPath(s_reader.GetMedia()->GetFileName()).c_str());
    return false;
  }

  HostInterfaceProgressCallback callback;
  if (!s_reader.Precache(&callback))
  {
    Host::AddOSDMessage(TRANSLATE_STR("OSDMessage", "Precaching CD image failed, it may be unreliable."), 15.0f);
    return false;
  }

  return true;
}

TinyString CDROM::LBAToMSFString(CDImage::LBA lba)
{
  const auto pos = CDImage::Position::FromLBA(lba);
  return TinyString::from_format("{:02d}:{:02d}:{:02d}", pos.minute, pos.second, pos.frame);
}

void CDROM::SetReadaheadSectors(u32 readahead_sectors)
{
  const bool want_thread = (readahead_sectors > 0);
  if (want_thread == s_reader.IsUsingThread() && s_reader.GetReadaheadCount() == readahead_sectors)
    return;

  if (want_thread)
    s_reader.StartThread(readahead_sectors);
  else
    s_reader.StopThread();

  if (HasMedia())
    s_reader.QueueReadSector(s_requested_lba);
}

void CDROM::CPUClockChanged()
{
  // reschedule the disc read event
  if (IsReadingOrPlaying())
    s_drive_event->SetInterval(GetTicksForRead());
}

u8 CDROM::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0: // status register
      Log_TracePrintf("CDROM read status register -> 0x%08X", s_status.bits);
      return s_status.bits;

    case 1: // always response FIFO
    {
      if (s_response_fifo.IsEmpty())
      {
        Log_DevPrint("Response FIFO empty on read");
        return 0x00;
      }

      const u8 value = s_response_fifo.Pop();
      UpdateStatusRegister();
      Log_DebugPrintf("CDROM read response FIFO -> 0x%08X", ZeroExtend32(value));
      return value;
    }

    case 2: // always data FIFO
    {
      const u8 value = s_data_fifo.Pop();
      UpdateStatusRegister();
      Log_DebugPrintf("CDROM read data FIFO -> 0x%08X", ZeroExtend32(value));
      return value;
    }

    case 3:
    {
      if (s_status.index & 1)
      {
        const u8 value = s_interrupt_flag_register | ~INTERRUPT_REGISTER_MASK;
        Log_DebugPrintf("CDROM read interrupt flag register -> 0x%02X", ZeroExtend32(value));
        return value;
      }
      else
      {
        const u8 value = s_interrupt_enable_register | ~INTERRUPT_REGISTER_MASK;
        Log_DebugPrintf("CDROM read interrupt enable register -> 0x%02X", ZeroExtend32(value));
        return value;
      }
    }
    break;
  }

  Log_ErrorPrintf("Unknown CDROM register read: offset=0x%02X, index=%d", offset,
                  ZeroExtend32(s_status.index.GetValue()));
  Panic("Unknown CDROM register");
}

void CDROM::WriteRegister(u32 offset, u8 value)
{
  if (offset == 0)
  {
    Log_TracePrintf("CDROM status register <- 0x%02X", value);
    s_status.bits = (s_status.bits & static_cast<u8>(~3)) | (value & u8(3));
    return;
  }

  const u32 reg = (s_status.index * 3u) + (offset - 1);
  switch (reg)
  {
    case 0:
    {
      Log_DebugPrintf("CDROM command register <- 0x%02X (%s)", value, s_command_info[value].name);
      BeginCommand(static_cast<Command>(value));
      return;
    }

    case 1:
    {
      if (s_param_fifo.IsFull())
      {
        Log_WarningPrintf("Parameter FIFO overflow");
        s_param_fifo.RemoveOne();
      }

      s_param_fifo.Push(value);
      UpdateStatusRegister();
      return;
    }

    case 2:
    {
      Log_DebugPrintf("Request register <- 0x%02X", value);
      const RequestRegister rr{value};

      // Sound map is not currently implemented, haven't found anything which uses it.
      if (rr.SMEN)
        Log_ErrorPrintf("Sound map enable set");
      if (rr.BFWR)
        Log_ErrorPrintf("Buffer write enable set");

      if (rr.BFRD)
      {
        LoadDataFIFO();
      }
      else
      {
        Log_DebugPrintf("Clearing data FIFO");
        s_data_fifo.Clear();
      }

      UpdateStatusRegister();
      return;
    }

    case 3:
    {
      Log_ErrorPrintf("Sound map data out <- 0x%02X", value);
      return;
    }

    case 4:
    {
      Log_DebugPrintf("Interrupt enable register <- 0x%02X", value);
      s_interrupt_enable_register = value & INTERRUPT_REGISTER_MASK;
      UpdateInterruptRequest();
      return;
    }

    case 5:
    {
      Log_DebugPrintf("Interrupt flag register <- 0x%02X", value);
      s_interrupt_flag_register &= ~(value & INTERRUPT_REGISTER_MASK);
      if (s_interrupt_flag_register == 0)
      {
        InterruptController::SetLineState(InterruptController::IRQ::CDROM, false);
        if (HasPendingAsyncInterrupt())
          QueueDeliverAsyncInterrupt();
        else
          UpdateCommandEvent();
      }

      // Bit 6 clears the parameter FIFO.
      if (value & 0x40)
      {
        s_param_fifo.Clear();
        UpdateStatusRegister();
      }

      return;
    }

    case 6:
    {
      Log_ErrorPrintf("Sound map coding info <- 0x%02X", value);
      return;
    }

    case 7:
    {
      Log_DebugPrintf("Audio volume for left-to-left output <- 0x%02X", value);
      s_next_cd_audio_volume_matrix[0][0] = value;
      return;
    }

    case 8:
    {
      Log_DebugPrintf("Audio volume for left-to-right output <- 0x%02X", value);
      s_next_cd_audio_volume_matrix[0][1] = value;
      return;
    }

    case 9:
    {
      Log_DebugPrintf("Audio volume for right-to-right output <- 0x%02X", value);
      s_next_cd_audio_volume_matrix[1][1] = value;
      return;
    }

    case 10:
    {
      Log_DebugPrintf("Audio volume for right-to-left output <- 0x%02X", value);
      s_next_cd_audio_volume_matrix[1][0] = value;
      return;
    }

    case 11:
    {
      Log_DebugPrintf("Audio volume apply changes <- 0x%02X", value);

      const bool adpcm_muted = ConvertToBoolUnchecked(value & u8(0x01));
      if (adpcm_muted != s_adpcm_muted ||
          (value & 0x20 && std::memcmp(s_cd_audio_volume_matrix.data(), s_next_cd_audio_volume_matrix.data(),
                                       sizeof(s_cd_audio_volume_matrix)) != 0))
      {
        if (HasPendingDiscEvent())
          s_drive_event->InvokeEarly();
        SPU::GeneratePendingSamples();
      }

      s_adpcm_muted = adpcm_muted;
      if (value & 0x20)
        s_cd_audio_volume_matrix = s_next_cd_audio_volume_matrix;
      return;
    }

    default:
    {
      Log_ErrorPrintf("Unknown CDROM register write: offset=0x%02X, index=%d, reg=%u, value=0x%02X", offset,
                      s_status.index.GetValue(), reg, value);
      return;
    }
  }
}

void CDROM::DMARead(u32* words, u32 word_count)
{
  const u32 words_in_fifo = s_data_fifo.GetSize() / 4;
  if (words_in_fifo < word_count)
  {
    Log_ErrorPrintf("DMA read on empty/near-empty data FIFO");
    std::memset(words + words_in_fifo, 0, sizeof(u32) * (word_count - words_in_fifo));
  }

  const u32 bytes_to_read = std::min<u32>(word_count * sizeof(u32), s_data_fifo.GetSize());
  s_data_fifo.PopRange(reinterpret_cast<u8*>(words), bytes_to_read);
}

bool CDROM::HasPendingCommand()
{
  return s_command != Command::None;
}

bool CDROM::HasPendingInterrupt()
{
  return s_interrupt_flag_register != 0;
}

bool CDROM::HasPendingAsyncInterrupt()
{
  return s_pending_async_interrupt != 0;
}

void CDROM::SetInterrupt(Interrupt interrupt)
{
  s_interrupt_flag_register = static_cast<u8>(interrupt);
  s_last_interrupt_time = System::GetGlobalTickCounter();
  UpdateInterruptRequest();
}

void CDROM::SetAsyncInterrupt(Interrupt interrupt)
{
  if (s_interrupt_flag_register == static_cast<u8>(interrupt))
  {
    Log_DevPrintf("Not setting async interrupt %u because there is already one unacknowledged",
                  static_cast<u8>(interrupt));
    s_async_response_fifo.Clear();
    return;
  }

  Assert(s_pending_async_interrupt == 0);
  s_pending_async_interrupt = static_cast<u8>(interrupt);
  if (!HasPendingInterrupt())
    QueueDeliverAsyncInterrupt();
}

void CDROM::ClearAsyncInterrupt()
{
  s_pending_async_interrupt = 0;
  s_async_interrupt_event->Deactivate();
  s_async_response_fifo.Clear();
}

void CDROM::QueueDeliverAsyncInterrupt()
{
  // Why do we need this mess? A few games, such as Ogre Battle, like to spam GetlocL or GetlocP while
  // XA playback is going. The problem is, when that happens and an INT1 also comes in. Instead of
  // reading the interrupt flag, reading the FIFO, and then clearing the interrupt, they clear the
  // interrupt, then read the FIFO. If an INT1 comes in during that time, it'll read the INT1 response
  // instead of the INT3 response, and the game gets confused. So, we just delay INT1s a bit, if there
  // has been any recent INT3s - give it enough time to read the response out. The real console does
  // something similar anyway, the INT1 task won't run immediately after the INT3 is cleared.

  if (!HasPendingAsyncInterrupt())
    return;

  // underflows here are okay
  const u32 diff = System::GetGlobalTickCounter() - s_last_interrupt_time;
  if (diff >= MINIMUM_INTERRUPT_DELAY)
  {
    DeliverAsyncInterrupt(nullptr, 0, 0);
  }
  else
  {
    Log_DevPrintf("Delaying async interrupt %u because it's been %u cycles since last interrupt",
                  s_pending_async_interrupt, diff);
    s_async_interrupt_event->Schedule(INTERRUPT_DELAY_CYCLES);
  }
}

void CDROM::DeliverAsyncInterrupt(void*, TickCount ticks, TickCount ticks_late)
{
  if (HasPendingInterrupt())
  {
    // This shouldn't really happen, because we should block command execution.. but just in case.
    if (!s_async_interrupt_event->IsActive())
      s_async_interrupt_event->Schedule(INTERRUPT_DELAY_CYCLES);
  }
  else
  {
    s_async_interrupt_event->Deactivate();

    Assert(s_pending_async_interrupt != 0 && !HasPendingInterrupt());
    Log_DebugPrintf("Delivering async interrupt %u", s_pending_async_interrupt);

    if (s_pending_async_interrupt == static_cast<u8>(Interrupt::DataReady))
      s_current_read_sector_buffer = s_current_write_sector_buffer;

    s_response_fifo.Clear();
    s_response_fifo.PushFromQueue(&s_async_response_fifo);
    s_interrupt_flag_register = s_pending_async_interrupt;
    s_pending_async_interrupt = 0;
    UpdateInterruptRequest();
    UpdateStatusRegister();
    UpdateCommandEvent();
  }
}

void CDROM::SendACKAndStat()
{
  s_response_fifo.Push(s_secondary_status.bits);
  SetInterrupt(Interrupt::ACK);
}

void CDROM::SendErrorResponse(u8 stat_bits /* = STAT_ERROR */, u8 reason /* = 0x80 */)
{
  s_response_fifo.Push(s_secondary_status.bits | stat_bits);
  s_response_fifo.Push(reason);
  SetInterrupt(Interrupt::Error);
}

void CDROM::SendAsyncErrorResponse(u8 stat_bits /* = STAT_ERROR */, u8 reason /* = 0x80 */)
{
  s_async_response_fifo.Push(s_secondary_status.bits | stat_bits);
  s_async_response_fifo.Push(reason);
  SetAsyncInterrupt(Interrupt::Error);
}

void CDROM::UpdateStatusRegister()
{
  s_status.ADPBUSY = false;
  s_status.PRMEMPTY = s_param_fifo.IsEmpty();
  s_status.PRMWRDY = !s_param_fifo.IsFull();
  s_status.RSLRRDY = !s_response_fifo.IsEmpty();
  s_status.DRQSTS = !s_data_fifo.IsEmpty();
  s_status.BUSYSTS = HasPendingCommand();

  DMA::SetRequest(DMA::Channel::CDROM, s_status.DRQSTS);
}

void CDROM::UpdateInterruptRequest()
{
  InterruptController::SetLineState(InterruptController::IRQ::CDROM,
                                    (s_interrupt_flag_register & s_interrupt_enable_register) != 0);
}

bool CDROM::HasPendingDiscEvent()
{
  return (s_drive_event->IsActive() && s_drive_event->GetTicksUntilNextExecution() <= 0);
}

TickCount CDROM::GetAckDelayForCommand(Command command)
{
  if (command == Command::Init)
  {
    // Init takes longer.
    return 80000;
  }

  // Tests show that the average time to acknowledge a command is significantly higher when a disc is in the drive,
  // presumably because the controller is busy doing discy-things.
  constexpr u32 default_ack_delay_no_disc = 15000;
  constexpr u32 default_ack_delay_with_disc = 25000;
  return CanReadMedia() ? default_ack_delay_with_disc : default_ack_delay_no_disc;
}

TickCount CDROM::GetTicksForSpinUp()
{
  // 1 second
  return System::GetTicksPerSecond();
}

TickCount CDROM::GetTicksForIDRead()
{
  TickCount ticks = ID_READ_TICKS;
  if (s_drive_state == DriveState::SpinningUp)
    ticks += s_drive_event->GetTicksUntilNextExecution();

  return ticks;
}

TickCount CDROM::GetTicksForRead()
{
  const TickCount tps = System::GetTicksPerSecond();

  if (g_settings.cdrom_read_speedup > 1 && !s_mode.cdda && !s_mode.xa_enable && s_mode.double_speed)
    return tps / (150 * g_settings.cdrom_read_speedup);

  return s_mode.double_speed ? (tps / 150) : (tps / 75);
}

TickCount CDROM::GetTicksForSeek(CDImage::LBA new_lba, bool ignore_speed_change)
{
  static constexpr TickCount MIN_TICKS = 20000;

  if (g_settings.cdrom_seek_speedup == 0)
    return MIN_TICKS;

  u32 ticks = static_cast<u32>(MIN_TICKS);
  if (IsSeeking())
    ticks += s_drive_event->GetTicksUntilNextExecution();
  else
    UpdatePhysicalPosition(false);

  const u32 ticks_per_sector =
    s_mode.double_speed ? static_cast<u32>(System::MASTER_CLOCK / 150) : static_cast<u32>(System::MASTER_CLOCK / 75);
  const u32 ticks_per_second = static_cast<u32>(System::MASTER_CLOCK);
  const CDImage::LBA current_lba = IsMotorOn() ? (IsSeeking() ? s_seek_end_lba : s_physical_lba) : 0;
  const u32 lba_diff = static_cast<u32>((new_lba > current_lba) ? (new_lba - current_lba) : (current_lba - new_lba));

  // Motor spin-up time.
  if (!IsMotorOn())
  {
    ticks +=
      (s_drive_state == DriveState::SpinningUp) ? s_drive_event->GetTicksUntilNextExecution() : GetTicksForSpinUp();
    if (s_drive_state == DriveState::ShellOpening || s_drive_state == DriveState::SpinningUp)
      ClearDriveState();
  }

  if (lba_diff < 32)
  {
    // Special case: when we land exactly on the right sector, we're already too late.
    ticks += ticks_per_sector * std::min<u32>(5u, (lba_diff == 0) ? 4u : lba_diff);
  }
  else
  {
    // This is a still not a very accurate model, but it's roughly in line with the behavior of hardware tests.
    const float disc_distance = 0.2323384936f * std::log(static_cast<float>((new_lba / 4500) + 1u));

    float seconds;
    if (lba_diff <= CDImage::FRAMES_PER_SECOND)
    {
      // 30ms + (diff * 30ms) + (disc distance * 30ms)
      seconds = 0.03f + ((static_cast<float>(lba_diff) / static_cast<float>(CDImage::FRAMES_PER_SECOND)) * 0.03f) +
                (disc_distance * 0.03f);
    }
    else if (lba_diff <= CDImage::FRAMES_PER_MINUTE)
    {
      // 150ms + (diff * 30ms) + (disc distance * 50ms)
      seconds = 0.15f + ((static_cast<float>(lba_diff) / static_cast<float>(CDImage::FRAMES_PER_MINUTE)) * 0.03f) +
                (disc_distance * 0.05f);
    }
    else
    {
      // 200ms + (diff * 500ms)
      seconds = 0.2f + ((static_cast<float>(lba_diff) / static_cast<float>(72 * CDImage::FRAMES_PER_MINUTE)) * 0.4f);
    }

    ticks += static_cast<u32>(seconds * static_cast<float>(ticks_per_second));
  }

  if (s_drive_state == DriveState::ChangingSpeedOrTOCRead && !ignore_speed_change)
  {
    // we're still reading the TOC, so add that time in
    const TickCount remaining_change_ticks = s_drive_event->GetTicksUntilNextExecution();
    ticks += remaining_change_ticks;

    Log_DevPrintf("Seek time for %u LBAs: %d (%.3f ms) (%d for speed change/implicit TOC read)", lba_diff, ticks,
                  (static_cast<float>(ticks) / static_cast<float>(ticks_per_second)) * 1000.0f, remaining_change_ticks);
  }
  else
  {
    Log_DevPrintf("Seek time for %u LBAs: %d (%.3f ms)", lba_diff, ticks,
                  (static_cast<float>(ticks) / static_cast<float>(ticks_per_second)) * 1000.0f);
  }

  if (g_settings.cdrom_seek_speedup > 1)
    ticks = std::min<u32>(ticks / g_settings.cdrom_seek_speedup, MIN_TICKS);

  return System::ScaleTicksToOverclock(static_cast<TickCount>(ticks));
}

TickCount CDROM::GetTicksForStop(bool motor_was_on)
{
  return System::ScaleTicksToOverclock(motor_was_on ? (s_mode.double_speed ? 25000000 : 13000000) : 7000);
}

TickCount CDROM::GetTicksForSpeedChange()
{
  static constexpr u32 ticks_single_to_double = static_cast<u32>(0.8 * static_cast<double>(System::MASTER_CLOCK));
  static constexpr u32 ticks_double_to_single = static_cast<u32>(1.0 * static_cast<double>(System::MASTER_CLOCK));
  return System::ScaleTicksToOverclock(s_mode.double_speed ? ticks_single_to_double : ticks_double_to_single);
}

TickCount CDROM::GetTicksForTOCRead()
{
  if (!HasMedia())
    return 0;

  return System::GetTicksPerSecond() / 2u;
}

CDImage::LBA CDROM::GetNextSectorToBeRead()
{
  if (!IsReadingOrPlaying())
    return s_current_lba;

  s_reader.WaitForReadToComplete();
  return s_reader.GetLastReadSector();
}

void CDROM::BeginCommand(Command command)
{
  TickCount ack_delay = GetAckDelayForCommand(command);

  if (HasPendingCommand())
  {
    // The behavior here is kinda.. interesting. Some commands seem to take precedence over others, for example
    // sending a Nop command followed by a GetlocP will return the GetlocP response, and the same for the inverse.
    // However, other combinations result in strange behavior, for example sending a Setloc followed by a ReadN will
    // fail with ERROR_REASON_INCORRECT_NUMBER_OF_PARAMETERS. This particular example happens in Voice Idol
    // Collection - Pool Bar Story, and the loading time is lengthened as well as audio slowing down if this
    // behavior is not correct. So, let's use a heuristic; if the number of parameters of the "old" command is
    // greater than the "new" command, empty the FIFO, which will return the error when the command executes.
    // Otherwise, override the command with the new one.
    if (s_command_info[static_cast<u8>(s_command)].min_parameters >
        s_command_info[static_cast<u8>(command)].min_parameters)
    {
      Log_WarningPrintf("Ignoring command 0x%02X (%s) and emptying FIFO as 0x%02x (%s) is still pending",
                        static_cast<u8>(command), s_command_info[static_cast<u8>(command)].name,
                        static_cast<u8>(s_command), s_command_info[static_cast<u8>(s_command)].name);
      s_param_fifo.Clear();
      return;
    }

    Log_WarningPrintf("Cancelling pending command 0x%02X (%s) for new command 0x%02X (%s)", static_cast<u8>(s_command),
                      s_command_info[static_cast<u8>(s_command)].name, static_cast<u8>(command),
                      s_command_info[static_cast<u8>(command)].name);

    // subtract the currently-elapsed ack ticks from the new command
    if (s_command_event->IsActive())
    {
      const TickCount elapsed_ticks = s_command_event->GetInterval() - s_command_event->GetTicksUntilNextExecution();
      ack_delay = std::max(ack_delay - elapsed_ticks, 1);
      s_command_event->Deactivate();
    }
  }

  s_command = command;
  s_command_event->SetIntervalAndSchedule(ack_delay);
  UpdateCommandEvent();
  UpdateStatusRegister();
}

void CDROM::EndCommand()
{
  s_param_fifo.Clear();

  s_command = Command::None;
  s_command_event->Deactivate();
  UpdateStatusRegister();
}

void CDROM::ExecuteCommand(void*, TickCount ticks, TickCount ticks_late)
{
  const CommandInfo& ci = s_command_info[static_cast<u8>(s_command)];
  if (Log_DevVisible()) [[unlikely]]
  {
    SmallString params;
    for (u32 i = 0; i < s_param_fifo.GetSize(); i++)
      params.append_format("{}0x{:02X}", (i == 0) ? "" : ", ", s_param_fifo.Peek(i));
    Log_DevFmt("CDROM executing command 0x{:02X} ({}), stat = 0x{:02X}, params = [{}]", static_cast<u8>(s_command),
               ci.name, s_secondary_status.bits, params);
  }

  if (s_param_fifo.GetSize() < ci.min_parameters || s_param_fifo.GetSize() > ci.max_parameters) [[unlikely]]
  {
    Log_WarningFmt("Incorrect parameters for command 0x{:02X} ({}), expecting {}-{} got {}", static_cast<u8>(s_command),
                   ci.name, ci.min_parameters, ci.max_parameters, s_param_fifo.GetSize());
    SendErrorResponse(STAT_ERROR, ERROR_REASON_INCORRECT_NUMBER_OF_PARAMETERS);
    EndCommand();
    return;
  }

  if (!s_response_fifo.IsEmpty())
  {
    Log_DebugPrintf("Response FIFO not empty on command begin");
    s_response_fifo.Clear();
  }

  switch (s_command)
  {
    case Command::Getstat:
    {
      Log_DebugPrintf("CDROM Getstat command");

      // if bit 0 or 2 is set, send an additional byte
      SendACKAndStat();

      // shell open bit is cleared after sending the status
      if (CanReadMedia())
        s_secondary_status.shell_open = false;

      EndCommand();
      return;
    }

    case Command::Test:
    {
      const u8 subcommand = s_param_fifo.Pop();
      ExecuteTestCommand(subcommand);
      return;
    }

    case Command::GetID:
    {
      Log_DebugPrintf("CDROM GetID command");
      ClearCommandSecondResponse();

      if (!CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        SendACKAndStat();
        QueueCommandSecondResponse(Command::GetID, GetTicksForIDRead());
      }

      EndCommand();
      return;
    }

    case Command::ReadTOC:
    {
      Log_DebugPrintf("CDROM ReadTOC command");
      ClearCommandSecondResponse();

      if (!CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        SendACKAndStat();
        SetHoldPosition(0, true);
        QueueCommandSecondResponse(Command::ReadTOC, GetTicksForTOCRead());
      }

      EndCommand();
      return;
    }

    case Command::Setfilter:
    {
      const u8 file = s_param_fifo.Peek(0);
      const u8 channel = s_param_fifo.Peek(1);
      Log_DebugPrintf("CDROM setfilter command 0x%02X 0x%02X", ZeroExtend32(file), ZeroExtend32(channel));
      s_xa_filter_file_number = file;
      s_xa_filter_channel_number = channel;
      s_xa_current_set = false;
      SendACKAndStat();
      EndCommand();
      return;
    }

    case Command::Setmode:
    {
      const u8 mode = s_param_fifo.Peek(0);
      const bool speed_change = (mode & 0x80) != (s_mode.bits & 0x80);
      Log_DevPrintf("CDROM setmode command 0x%02X", ZeroExtend32(mode));

      s_mode.bits = mode;
      SendACKAndStat();
      EndCommand();

      if (speed_change)
      {
        if (s_drive_state == DriveState::ChangingSpeedOrTOCRead)
        {
          // cancel the speed change if it's less than a quarter complete
          if (s_drive_event->GetTicksUntilNextExecution() >= (GetTicksForSpeedChange() / 4))
          {
            Log_DevPrintf("Cancelling speed change event");
            ClearDriveState();
          }
        }
        else if (s_drive_state != DriveState::SeekingImplicit && s_drive_state != DriveState::ShellOpening)
        {
          // if we're seeking or reading, we need to add time to the current seek/read
          const TickCount change_ticks = GetTicksForSpeedChange();
          if (s_drive_state != DriveState::Idle)
          {
            Log_DevPrintf("Drive is %s, delaying event by %d ticks for speed change to %s-speed",
                          s_drive_state_names[static_cast<u8>(s_drive_state)], change_ticks,
                          s_mode.double_speed ? "double" : "single");
            s_drive_event->Delay(change_ticks);
          }
          else
          {
            Log_DevPrintf("Drive is idle, speed change takes %d ticks", change_ticks);
            s_drive_state = DriveState::ChangingSpeedOrTOCRead;
            s_drive_event->Schedule(change_ticks);
          }
        }
      }

      return;
    }

    case Command::Setloc:
    {
      const u8 mm = s_param_fifo.Peek(0);
      const u8 ss = s_param_fifo.Peek(1);
      const u8 ff = s_param_fifo.Peek(2);
      Log_DevPrintf("CDROM setloc command (%02X, %02X, %02X)", mm, ss, ff);

      // MM must be BCD, SS must be BCD and <0x60, FF must be BCD and <0x75
      if (((mm & 0x0F) > 0x09) || (mm > 0x99) || ((ss & 0x0F) > 0x09) || (ss >= 0x60) || ((ff & 0x0F) > 0x09) ||
          (ff >= 0x75))
      {
        Log_ErrorPrintf("Invalid/out of range seek to %02X:%02X:%02X", mm, ss, ff);
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_ARGUMENT);
      }
      else
      {
        SendACKAndStat();

        s_setloc_position.minute = PackedBCDToBinary(mm);
        s_setloc_position.second = PackedBCDToBinary(ss);
        s_setloc_position.frame = PackedBCDToBinary(ff);
        s_setloc_pending = true;
      }

      EndCommand();
      return;
    }

    case Command::SeekL:
    case Command::SeekP:
    {
      const bool logical = (s_command == Command::SeekL);
      Log_DebugPrintf("CDROM %s command", logical ? "SeekL" : "SeekP");

      if (IsSeeking())
        UpdatePositionWhileSeeking();

      if (!CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        SendACKAndStat();
        BeginSeeking(logical, false, false);
      }

      EndCommand();
      return;
    }

    case Command::ReadT:
    {
      const u8 session = s_param_fifo.Peek(0);
      Log_DebugPrintf("CDROM ReadT command, session=%u", session);

      if (!CanReadMedia() || s_drive_state == DriveState::Reading || s_drive_state == DriveState::Playing)
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else if (session == 0)
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_ARGUMENT);
      }
      else
      {
        ClearCommandSecondResponse();
        SendACKAndStat();

        s_async_command_parameter = session;
        s_drive_state = DriveState::ChangingSession;
        s_drive_event->Schedule(GetTicksForTOCRead());
      }

      EndCommand();
      return;
    }

    case Command::ReadN:
    case Command::ReadS:
    {
      Log_DebugPrintf("CDROM read command");
      if (!CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else if ((!IsMediaPS1Disc() || !DoesMediaRegionMatchConsole()) && !s_mode.cdda)
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_COMMAND);
      }
      else
      {
        SendACKAndStat();

        if ((!s_setloc_pending || s_setloc_position.ToLBA() == GetNextSectorToBeRead()) &&
            (s_drive_state == DriveState::Reading || (IsSeeking() && s_read_after_seek)))
        {
          Log_DevPrintf("Ignoring read command with %s setloc, already reading/reading after seek",
                        s_setloc_pending ? "pending" : "same");
          s_setloc_pending = false;
        }
        else
        {
          if (IsSeeking())
            UpdatePositionWhileSeeking();

          BeginReading();
        }
      }

      EndCommand();
      return;
    }

    case Command::Play:
    {
      const u8 track = s_param_fifo.IsEmpty() ? 0 : PackedBCDToBinary(s_param_fifo.Peek(0));
      Log_DebugPrintf("CDROM play command, track=%u", track);

      if (!CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        SendACKAndStat();

        if (track == 0 && (!s_setloc_pending || s_setloc_position.ToLBA() == GetNextSectorToBeRead()) &&
            (s_drive_state == DriveState::Playing || (IsSeeking() && s_play_after_seek)))
        {
          Log_DevPrintf("Ignoring play command with no/same setloc, already playing/playing after seek");
          s_fast_forward_rate = 0;
        }
        else
        {
          if (IsSeeking())
            UpdatePositionWhileSeeking();

          BeginPlaying(track);
        }
      }

      EndCommand();
      return;
    }

    case Command::Forward:
    {
      if (s_drive_state != DriveState::Playing || !CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        SendACKAndStat();

        if (s_fast_forward_rate < 0)
          s_fast_forward_rate = 0;

        s_fast_forward_rate += static_cast<s8>(FAST_FORWARD_RATE_STEP);
        s_fast_forward_rate = std::min<s8>(s_fast_forward_rate, static_cast<s8>(MAX_FAST_FORWARD_RATE));
      }

      EndCommand();
      return;
    }

    case Command::Backward:
    {
      if (s_drive_state != DriveState::Playing || !CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        SendACKAndStat();

        if (s_fast_forward_rate > 0)
          s_fast_forward_rate = 0;

        s_fast_forward_rate -= static_cast<s8>(FAST_FORWARD_RATE_STEP);
        s_fast_forward_rate = std::max<s8>(s_fast_forward_rate, -static_cast<s8>(MAX_FAST_FORWARD_RATE));
      }

      EndCommand();
      return;
    }

    case Command::Pause:
    {
      const bool was_reading = (s_drive_state == DriveState::Reading || s_drive_state == DriveState::Playing);
      const TickCount pause_time = was_reading ? (s_mode.double_speed ? 2000000 : 1000000) : 7000;

      ClearCommandSecondResponse();
      SendACKAndStat();

      if (s_drive_state == DriveState::SeekingLogical || s_drive_state == DriveState::SeekingPhysical)
      {
        // TODO: On console, this returns an error. But perhaps only during the coarse/fine seek part? Needs more
        // hardware tests.
        Log_WarningPrintf("CDROM Pause command while seeking from %u to %u - jumping to seek target", s_seek_start_lba,
                          s_seek_end_lba);
        s_read_after_seek = false;
        s_play_after_seek = false;
        CompleteSeek();
      }
      else
      {
        // Stop reading.
        s_drive_state = DriveState::Idle;
        s_drive_event->Deactivate();
        s_secondary_status.ClearActiveBits();
      }

      // Reset audio buffer here - control room cutscene audio repeats in Dino Crisis otherwise.
      ResetAudioDecoder();

      QueueCommandSecondResponse(Command::Pause, pause_time);

      EndCommand();
      return;
    }

    case Command::Stop:
    {
      const TickCount stop_time = GetTicksForStop(IsMotorOn());
      ClearCommandSecondResponse();
      SendACKAndStat();

      StopMotor();
      QueueCommandSecondResponse(Command::Stop, stop_time);

      EndCommand();
      return;
    }

    case Command::Init:
    {
      Log_DebugPrintf("CDROM init command");

      if (s_command_second_response == Command::Init)
      {
        // still pending
        EndCommand();
        return;
      }

      SendACKAndStat();

      if (IsSeeking())
        UpdatePositionWhileSeeking();

      SoftReset(ticks_late);

      QueueCommandSecondResponse(Command::Init, INIT_TICKS);
      EndCommand();
    }
    break;

    case Command::MotorOn:
    {
      Log_DebugPrintf("CDROM motor on command");
      if (IsMotorOn())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INCORRECT_NUMBER_OF_PARAMETERS);
      }
      else
      {
        SendACKAndStat();

        // still pending?
        if (s_command_second_response == Command::MotorOn)
        {
          EndCommand();
          return;
        }

        if (CanReadMedia())
          StartMotor();

        QueueCommandSecondResponse(Command::MotorOn, MOTOR_ON_RESPONSE_TICKS);
      }

      EndCommand();
      return;
    }
    break;

    case Command::Mute:
    {
      Log_DebugPrintf("CDROM mute command");
      s_muted = true;
      SendACKAndStat();
      EndCommand();
    }
    break;

    case Command::Demute:
    {
      Log_DebugPrintf("CDROM demute command");
      s_muted = false;
      SendACKAndStat();
      EndCommand();
    }
    break;

    case Command::GetlocL:
    {
      if (!s_last_sector_header_valid)
      {
        Log_DevPrintf("CDROM GetlocL command - header invalid, status 0x%02X", s_secondary_status.bits);
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        UpdatePhysicalPosition(true);

        Log_DebugPrintf("CDROM GetlocL command - [%02X:%02X:%02X]", s_last_sector_header.minute,
                        s_last_sector_header.second, s_last_sector_header.frame);

        s_response_fifo.PushRange(reinterpret_cast<const u8*>(&s_last_sector_header), sizeof(s_last_sector_header));
        s_response_fifo.PushRange(reinterpret_cast<const u8*>(&s_last_sector_subheader),
                                  sizeof(s_last_sector_subheader));
        SetInterrupt(Interrupt::ACK);
      }

      EndCommand();
      return;
    }

    case Command::GetlocP:
    {
      if (!CanReadMedia())
      {
        Log_DebugPrintf("CDROM GetlocP command - not ready");
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        if (IsSeeking())
          UpdatePositionWhileSeeking();
        else
          UpdatePhysicalPosition(false);

        Log_DevPrintf("CDROM GetlocP command - T%02x I%02x R[%02x:%02x:%02x] A[%02x:%02x:%02x]",
                      s_last_subq.track_number_bcd, s_last_subq.index_number_bcd, s_last_subq.relative_minute_bcd,
                      s_last_subq.relative_second_bcd, s_last_subq.relative_frame_bcd, s_last_subq.absolute_minute_bcd,
                      s_last_subq.absolute_second_bcd, s_last_subq.absolute_frame_bcd);

        s_response_fifo.Push(s_last_subq.track_number_bcd);
        s_response_fifo.Push(s_last_subq.index_number_bcd);
        s_response_fifo.Push(s_last_subq.relative_minute_bcd);
        s_response_fifo.Push(s_last_subq.relative_second_bcd);
        s_response_fifo.Push(s_last_subq.relative_frame_bcd);
        s_response_fifo.Push(s_last_subq.absolute_minute_bcd);
        s_response_fifo.Push(s_last_subq.absolute_second_bcd);
        s_response_fifo.Push(s_last_subq.absolute_frame_bcd);
        SetInterrupt(Interrupt::ACK);
      }

      EndCommand();
      return;
    }

    case Command::GetTN:
    {
      Log_DebugPrintf("CDROM GetTN command");
      if (CanReadMedia())
      {
        Log_DevPrintf("GetTN -> %u %u", s_reader.GetMedia()->GetFirstTrackNumber(),
                      s_reader.GetMedia()->GetLastTrackNumber());

        s_response_fifo.Push(s_secondary_status.bits);
        s_response_fifo.Push(BinaryToBCD(Truncate8(s_reader.GetMedia()->GetFirstTrackNumber())));
        s_response_fifo.Push(BinaryToBCD(Truncate8(s_reader.GetMedia()->GetLastTrackNumber())));
        SetInterrupt(Interrupt::ACK);
      }
      else
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }

      EndCommand();
    }
    break;

    case Command::GetTD:
    {
      Log_DebugPrintf("CDROM GetTD command");
      Assert(s_param_fifo.GetSize() >= 1);

      if (!CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
        EndCommand();
        return;
      }

      const u8 track_bcd = s_param_fifo.Peek();
      if (!IsValidPackedBCD(track_bcd))
      {
        Log_ErrorFmt("Invalid track number in GetTD: {:02X}", track_bcd);
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_ARGUMENT);
        EndCommand();
        return;
      }

      const u8 track = PackedBCDToBinary(track_bcd);
      if (track > s_reader.GetMedia()->GetTrackCount())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_ARGUMENT);
      }
      else
      {
        CDImage::Position pos;
        if (track == 0)
          pos = CDImage::Position::FromLBA(s_reader.GetMedia()->GetLBACount());
        else
          pos = s_reader.GetMedia()->GetTrackStartMSFPosition(track);

        s_response_fifo.Push(s_secondary_status.bits);
        s_response_fifo.Push(BinaryToBCD(Truncate8(pos.minute)));
        s_response_fifo.Push(BinaryToBCD(Truncate8(pos.second)));
        Log_DevPrintf("GetTD %u -> %u %u", track, pos.minute, pos.second);

        SetInterrupt(Interrupt::ACK);
      }

      EndCommand();
    }
    break;

    case Command::Getmode:
    {
      Log_DebugPrintf("CDROM Getmode command");

      s_response_fifo.Push(s_secondary_status.bits);
      s_response_fifo.Push(s_mode.bits);
      s_response_fifo.Push(0);
      s_response_fifo.Push(s_xa_filter_file_number);
      s_response_fifo.Push(s_xa_filter_channel_number);
      SetInterrupt(Interrupt::ACK);
      EndCommand();
    }
    break;

    case Command::Sync:
    {
      Log_DebugPrintf("CDROM sync command");

      SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_COMMAND);
      EndCommand();
    }
    break;

    case Command::VideoCD:
    {
      Log_DebugPrintf("CDROM VideoCD command");
      SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_COMMAND);

      // According to nocash this doesn't clear the parameter FIFO.
      s_command = Command::None;
      s_command_event->Deactivate();
      UpdateStatusRegister();
    }
    break;

    default:
    {
      Log_ErrorPrintf("Unknown CDROM command 0x%04X with %u parameters, please report", static_cast<u16>(s_command),
                      s_param_fifo.GetSize());
      SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_COMMAND);
      EndCommand();
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
      s_secondary_status.motor_on = true;
      s_response_fifo.Push(s_secondary_status.bits);
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case 0x05: // Read SCEx counters
    {
      Log_DebugPrintf("Read SCEx counters");
      s_response_fifo.Push(s_secondary_status.bits);
      s_response_fifo.Push(0); // # of TOC reads?
      s_response_fifo.Push(0); // # of SCEx strings received
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case 0x20: // Get CDROM BIOS Date/Version
    {
      Log_DebugPrintf("Get CDROM BIOS Date/Version");

      static constexpr const u8 version_table[][4] = {
        {0x94, 0x09, 0x19, 0xC0}, // PSX (PU-7)               19 Sep 1994, version vC0 (a)
        {0x94, 0x11, 0x18, 0xC0}, // PSX (PU-7)               18 Nov 1994, version vC0 (b)
        {0x95, 0x05, 0x16, 0xC1}, // PSX (EARLY-PU-8)         16 May 1995, version vC1 (a)
        {0x95, 0x07, 0x24, 0xC1}, // PSX (LATE-PU-8)          24 Jul 1995, version vC1 (b)
        {0x95, 0x07, 0x24, 0xD1}, // PSX (LATE-PU-8,debug ver)24 Jul 1995, version vD1 (debug)
        {0x96, 0x08, 0x15, 0xC2}, // PSX (PU-16, Video CD)    15 Aug 1996, version vC2 (VCD)
        {0x96, 0x08, 0x18, 0xC1}, // PSX (LATE-PU-8,yaroze)   18 Aug 1996, version vC1 (yaroze)
        {0x96, 0x09, 0x12, 0xC2}, // PSX (PU-18) (japan)      12 Sep 1996, version vC2 (a.jap)
        {0x97, 0x01, 0x10, 0xC2}, // PSX (PU-18) (us/eur)     10 Jan 1997, version vC2 (a)
        {0x97, 0x08, 0x14, 0xC2}, // PSX (PU-20)              14 Aug 1997, version vC2 (b)
        {0x98, 0x06, 0x10, 0xC3}, // PSX (PU-22)              10 Jul 1998, version vC3 (a)
        {0x99, 0x02, 0x01, 0xC3}, // PSX/PSone (PU-23, PM-41) 01 Feb 1999, version vC3 (b)
        {0xA1, 0x03, 0x06, 0xC3}, // PSone/late (PM-41(2))    06 Jun 2001, version vC3 (c)
      };

      s_response_fifo.PushRange(version_table[static_cast<u8>(g_settings.cdrom_mechacon_version)],
                                countof(version_table[0]));
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case 0x22:
    {
      Log_DebugPrintf("Get CDROM region ID string");

      switch (System::GetRegion())
      {
        case ConsoleRegion::NTSC_J:
        {
          static constexpr u8 response[] = {'f', 'o', 'r', ' ', 'J', 'a', 'p', 'a', 'n'};
          s_response_fifo.PushRange(response, countof(response));
        }
        break;

        case ConsoleRegion::PAL:
        {
          static constexpr u8 response[] = {'f', 'o', 'r', ' ', 'E', 'u', 'r', 'o', 'p', 'e'};
          s_response_fifo.PushRange(response, countof(response));
        }
        break;

        case ConsoleRegion::NTSC_U:
        default:
        {
          static constexpr u8 response[] = {'f', 'o', 'r', ' ', 'U', '/', 'C'};
          s_response_fifo.PushRange(response, countof(response));
        }
        break;
      }

      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    default:
    {
      Log_ErrorPrintf("Unknown test command 0x%02X, %u parameters", subcommand, s_param_fifo.GetSize());
      SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_COMMAND);
      EndCommand();
      return;
    }
  }
}

void CDROM::ExecuteCommandSecondResponse(void*, TickCount ticks, TickCount ticks_late)
{
  switch (s_command_second_response)
  {
    case Command::GetID:
      DoIDRead();
      break;

    case Command::ReadTOC:
    case Command::Pause:
    case Command::Init:
    case Command::MotorOn:
    case Command::Stop:
      DoStatSecondResponse();
      break;

    default:
      break;
  }

  s_command_second_response = Command::None;
  s_command_second_response_event->Deactivate();
}

void CDROM::QueueCommandSecondResponse(Command command, TickCount ticks)
{
  ClearCommandSecondResponse();
  s_command_second_response = command;
  s_command_second_response_event->Schedule(ticks);
}

void CDROM::ClearCommandSecondResponse()
{
  if (s_command_second_response != Command::None)
  {
    Log_DevPrintf("Cancelling pending command 0x%02X (%s) second response", static_cast<u16>(s_command_second_response),
                  s_command_info[static_cast<u16>(s_command_second_response)].name);
  }

  s_command_second_response_event->Deactivate();
  s_command_second_response = Command::None;
}

void CDROM::UpdateCommandEvent()
{
  // if there's a pending interrupt, we can't execute the command yet
  // so deactivate it until the interrupt is acknowledged
  if (!HasPendingCommand() || HasPendingInterrupt() || HasPendingAsyncInterrupt())
  {
    s_command_event->Deactivate();
    return;
  }
  else if (HasPendingCommand())
  {
    s_command_event->Activate();
  }
}

void CDROM::ExecuteDrive(void*, TickCount ticks, TickCount ticks_late)
{
  switch (s_drive_state)
  {
    case DriveState::ShellOpening:
      DoShellOpenComplete(ticks_late);
      break;

    case DriveState::SeekingPhysical:
    case DriveState::SeekingLogical:
      DoSeekComplete(ticks_late);
      break;

    case DriveState::SeekingImplicit:
      CompleteSeek();
      break;

    case DriveState::Reading:
    case DriveState::Playing:
      DoSectorRead();
      break;

    case DriveState::ChangingSession:
      DoChangeSessionComplete();
      break;

    case DriveState::SpinningUp:
      DoSpinUpComplete();
      break;

    case DriveState::ChangingSpeedOrTOCRead:
      DoSpeedChangeOrImplicitTOCReadComplete();
      break;

      // old states, no longer used, but kept for save state compatibility
    case DriveState::UNUSED_ReadingID:
    {
      ClearDriveState();
      DoIDRead();
    }
    break;

    case DriveState::UNUSED_Resetting:
    case DriveState::UNUSED_ReadingTOC:
    {
      ClearDriveState();
      DoStatSecondResponse();
    }
    break;

    case DriveState::UNUSED_Pausing:
    {
      ClearDriveState();
      s_secondary_status.ClearActiveBits();
      DoStatSecondResponse();
    }
    break;

    case DriveState::UNUSED_Stopping:
    {
      ClearDriveState();
      StopMotor();
      DoStatSecondResponse();
    }
    break;

    case DriveState::Idle:
    default:
      break;
  }
}

void CDROM::ClearDriveState()
{
  s_drive_state = DriveState::Idle;
  s_drive_event->Deactivate();
}

void CDROM::BeginReading(TickCount ticks_late /* = 0 */, bool after_seek /* = false */)
{
  ClearSectorBuffers();

  if (!after_seek && s_setloc_pending)
  {
    BeginSeeking(true, true, false);
    return;
  }

  // If we were seeking, we want to start reading from the seek target, not the current sector
  // Fixes crash in Disney's The Lion King - Simba's Mighty Adventure.
  if (IsSeeking())
  {
    Log_DevPrintf("Read command while seeking, scheduling read after seek %u -> %u finishes in %d ticks",
                  s_seek_start_lba, s_seek_end_lba, s_drive_event->GetTicksUntilNextExecution());

    // Implicit seeks won't trigger the read, so swap it for a logical.
    if (s_drive_state == DriveState::SeekingImplicit)
      s_drive_state = DriveState::SeekingLogical;

    s_read_after_seek = true;
    s_play_after_seek = false;
    return;
  }

  Log_DebugPrintf("Starting reading @ LBA %u", s_current_lba);

  const TickCount ticks = GetTicksForRead();
  const TickCount first_sector_ticks = ticks + (after_seek ? 0 : GetTicksForSeek(s_current_lba)) - ticks_late;

  ClearCommandSecondResponse();
  ResetAudioDecoder();

  s_drive_state = DriveState::Reading;
  s_drive_event->SetInterval(ticks);
  s_drive_event->Schedule(first_sector_ticks);
  s_current_read_sector_buffer = 0;
  s_current_write_sector_buffer = 0;

  s_requested_lba = s_current_lba;
  s_reader.QueueReadSector(s_requested_lba);
}

void CDROM::BeginPlaying(u8 track, TickCount ticks_late /* = 0 */, bool after_seek /* = false */)
{
  Log_DebugPrintf("Starting playing CDDA track %x", track);
  s_last_cdda_report_frame_nibble = 0xFF;
  s_play_track_number_bcd = track;
  s_fast_forward_rate = 0;

  // if track zero, start from current position
  if (track != 0)
  {
    // play specific track?
    if (track > s_reader.GetMedia()->GetTrackCount())
    {
      // restart current track
      track = Truncate8(s_reader.GetMedia()->GetTrackNumber());
    }

    s_setloc_position = s_reader.GetMedia()->GetTrackStartMSFPosition(track);
    s_setloc_pending = true;
  }

  if (s_setloc_pending)
  {
    BeginSeeking(false, false, true);
    return;
  }

  const TickCount ticks = GetTicksForRead();
  const TickCount first_sector_ticks = ticks + (after_seek ? 0 : GetTicksForSeek(s_current_lba, true)) - ticks_late;

  ClearCommandSecondResponse();
  ClearSectorBuffers();
  ResetAudioDecoder();

  s_drive_state = DriveState::Playing;
  s_drive_event->SetInterval(ticks);
  s_drive_event->Schedule(first_sector_ticks);
  s_current_read_sector_buffer = 0;
  s_current_write_sector_buffer = 0;

  s_requested_lba = s_current_lba;
  s_reader.QueueReadSector(s_requested_lba);
}

void CDROM::BeginSeeking(bool logical, bool read_after_seek, bool play_after_seek)
{
  if (!s_setloc_pending)
    Log_WarningPrintf("Seeking without setloc set");

  s_read_after_seek = read_after_seek;
  s_play_after_seek = play_after_seek;

  // TODO: Pending should stay set on seek command.
  s_setloc_pending = false;

  Log_DebugPrintf("Seeking to [%02u:%02u:%02u] (LBA %u) (%s)", s_setloc_position.minute, s_setloc_position.second,
                  s_setloc_position.frame, s_setloc_position.ToLBA(), logical ? "logical" : "physical");

  const CDImage::LBA seek_lba = s_setloc_position.ToLBA();
  const TickCount seek_time = GetTicksForSeek(seek_lba, play_after_seek);

  ClearCommandSecondResponse();
  ResetAudioDecoder();

  s_secondary_status.SetSeeking();
  s_last_sector_header_valid = false;

  s_drive_state = logical ? DriveState::SeekingLogical : DriveState::SeekingPhysical;
  s_drive_event->SetIntervalAndSchedule(seek_time);

  s_seek_start_lba = s_current_lba;
  s_seek_end_lba = seek_lba;
  s_requested_lba = seek_lba;
  s_reader.QueueReadSector(s_requested_lba);
}

void CDROM::UpdatePositionWhileSeeking()
{
  DebugAssert(IsSeeking());

  const float completed_frac = 1.0f - std::min(static_cast<float>(s_drive_event->GetTicksUntilNextExecution()) /
                                                 static_cast<float>(s_drive_event->GetInterval()),
                                               1.0f);

  CDImage::LBA current_lba;
  if (s_seek_end_lba > s_seek_start_lba)
  {
    current_lba =
      s_seek_start_lba +
      std::max<CDImage::LBA>(
        static_cast<CDImage::LBA>(static_cast<float>(s_seek_end_lba - s_seek_start_lba) * completed_frac), 1);
  }
  else if (s_seek_end_lba < s_seek_start_lba)
  {
    current_lba =
      s_seek_start_lba -
      std::max<CDImage::LBA>(
        static_cast<CDImage::LBA>(static_cast<float>(s_seek_start_lba - s_seek_end_lba) * completed_frac), 1);
  }
  else
  {
    // strange seek...
    return;
  }

  Log_DevPrintf("Update position while seeking from %u to %u - %u (%.2f)", s_seek_start_lba, s_seek_end_lba,
                current_lba, completed_frac);

  // access the image directly since we want to preserve the cached data for the seek complete
  CDImage::SubChannelQ subq;
  if (!s_reader.ReadSectorUncached(current_lba, &subq, nullptr))
    Log_ErrorPrintf("Failed to read subq for sector %u for physical position", current_lba);
  else if (subq.IsCRCValid())
    s_last_subq = subq;

  s_current_lba = current_lba;
  s_physical_lba = current_lba;
  s_physical_lba_update_tick = System::GetGlobalTickCounter();
  s_physical_lba_update_carry = 0;
}

void CDROM::UpdatePhysicalPosition(bool update_logical)
{
  const u32 ticks = System::GetGlobalTickCounter();
  if (IsSeeking() || IsReadingOrPlaying() || !IsMotorOn())
  {
    // If we're seeking+reading the first sector (no stat bits set), we need to return the set/current lba, not the last
    // physical LBA. Failing to do so may result in a track-jumped position getting returned in GetlocP, which causes
    // Mad Panic Coaster to go into a seek+play loop.
    if ((s_secondary_status.bits & (STAT_READING | STAT_PLAYING_CDDA | STAT_MOTOR_ON)) == STAT_MOTOR_ON &&
        s_current_lba != s_physical_lba)
    {
      Log_WarningPrintf("Jumping to hold position [%u->%u] while %s first sector", s_physical_lba, s_current_lba,
                        (s_drive_state == DriveState::Reading) ? "reading" : "playing");
      SetHoldPosition(s_current_lba, true);
    }

    // Otherwise, this gets updated by the read event.
    return;
  }

  const u32 ticks_per_read = GetTicksForRead();
  const u32 diff = ticks - s_physical_lba_update_tick + s_physical_lba_update_carry;
  const u32 sector_diff = diff / ticks_per_read;
  const u32 carry = diff % ticks_per_read;
  if (sector_diff > 0)
  {
    CDImage::LBA hold_offset;
    CDImage::LBA sectors_per_track;

    // hardware tests show that it holds much closer to the target sector in logical mode
    if (s_last_sector_header_valid)
    {
      hold_offset = 2;
      sectors_per_track = 4;
    }
    else
    {
      hold_offset = 0;
      sectors_per_track =
        static_cast<CDImage::LBA>(7.0f + 2.811844405f * std::log(static_cast<float>(s_current_lba / 4500u) + 1u));
    }

    const CDImage::LBA hold_position = s_current_lba + hold_offset;
    const CDImage::LBA base =
      (hold_position >= (sectors_per_track - 1)) ? (hold_position - (sectors_per_track - 1)) : hold_position;
    if (s_physical_lba < base)
      s_physical_lba = base;

    const CDImage::LBA old_offset = s_physical_lba - base;
    const CDImage::LBA new_offset = (old_offset + sector_diff) % sectors_per_track;
    const CDImage::LBA new_physical_lba = base + new_offset;
#ifdef _DEBUG
    Log_DevPrintf("Tick diff %u, sector diff %u, old pos %s, new pos %s", diff, sector_diff,
                  LBAToMSFString(s_physical_lba).c_str(), LBAToMSFString(new_physical_lba).c_str());
#endif
    if (s_physical_lba != new_physical_lba)
    {
      s_physical_lba = new_physical_lba;

      CDImage::SubChannelQ subq;
      CDROMAsyncReader::SectorBuffer raw_sector;
      if (!s_reader.ReadSectorUncached(new_physical_lba, &subq, update_logical ? &raw_sector : nullptr))
      {
        Log_ErrorPrintf("Failed to read subq for sector %u for physical position", new_physical_lba);
      }
      else
      {
        if (subq.IsCRCValid())
          s_last_subq = subq;

        if (update_logical)
          ProcessDataSectorHeader(raw_sector.data());
      }

      s_physical_lba_update_tick = ticks;
      s_physical_lba_update_carry = carry;
    }
  }
}

void CDROM::SetHoldPosition(CDImage::LBA lba, bool update_subq)
{
  if (update_subq && s_physical_lba != lba && CanReadMedia())
  {
    CDImage::SubChannelQ subq;
    if (!s_reader.ReadSectorUncached(lba, &subq, nullptr))
      Log_ErrorPrintf("Failed to read subq for sector %u for physical position", lba);
    else if (subq.IsCRCValid())
      s_last_subq = subq;
  }

  s_current_lba = lba;
  s_physical_lba = lba;
  s_physical_lba_update_tick = System::GetGlobalTickCounter();
  s_physical_lba_update_carry = 0;
}

void CDROM::DoShellOpenComplete(TickCount ticks_late)
{
  // media is now readable (if any)
  ClearDriveState();

  if (CanReadMedia())
    StartMotor();
}

bool CDROM::CompleteSeek()
{
  const bool logical = (s_drive_state == DriveState::SeekingLogical);
  ClearDriveState();

  bool seek_okay = s_reader.WaitForReadToComplete();
  if (seek_okay)
  {
    const CDImage::SubChannelQ& subq = s_reader.GetSectorSubQ();
    if (subq.IsCRCValid())
    {
      // seek and update sub-q for ReadP command
      s_last_subq = subq;
      const auto [seek_mm, seek_ss, seek_ff] = CDImage::Position::FromLBA(s_reader.GetLastReadSector()).ToBCD();
      seek_okay = (subq.IsCRCValid() && subq.absolute_minute_bcd == seek_mm && subq.absolute_second_bcd == seek_ss &&
                   subq.absolute_frame_bcd == seek_ff);
      if (seek_okay)
      {
        if (subq.IsData())
        {
          if (logical)
          {
            ProcessDataSectorHeader(s_reader.GetSectorBuffer().data());
            seek_okay = (s_last_sector_header.minute == seek_mm && s_last_sector_header.second == seek_ss &&
                         s_last_sector_header.frame == seek_ff);
          }
        }
        else
        {
          if (logical)
          {
            Log_WarningPrintf("Logical seek to non-data sector [%02x:%02x:%02x]%s", seek_mm, seek_ss, seek_ff,
                              s_read_after_seek ? ", reading after seek" : "");

            // If CDDA mode isn't enabled and we're reading an audio sector, we need to fail the seek.
            // Test cases:
            //  - Wizard's Harmony does a logical seek to an audio sector, and expects it to succeed.
            //  - Vib-ribbon starts a read at an audio sector, and expects it to fail.
            if (s_read_after_seek)
              seek_okay = s_mode.cdda;
          }
        }

        if (subq.track_number_bcd == CDImage::LEAD_OUT_TRACK_NUMBER)
        {
          Log_WarningPrintf("Invalid seek to lead-out area (LBA %u)", s_reader.GetLastReadSector());
          seek_okay = false;
        }
      }
    }

    s_current_lba = s_reader.GetLastReadSector();
  }

  s_physical_lba = s_current_lba;
  s_physical_lba_update_tick = System::GetGlobalTickCounter();
  s_physical_lba_update_carry = 0;
  return seek_okay;
}

void CDROM::DoSeekComplete(TickCount ticks_late)
{
  const bool logical = (s_drive_state == DriveState::SeekingLogical);
  const bool seek_okay = CompleteSeek();
  if (seek_okay)
  {
    // seek complete, transition to play/read if requested
    // INT2 is not sent on play/read
    if (s_read_after_seek)
    {
      BeginReading(ticks_late, true);
    }
    else if (s_play_after_seek)
    {
      BeginPlaying(0, ticks_late, true);
    }
    else
    {
      s_secondary_status.ClearActiveBits();
      s_async_response_fifo.Push(s_secondary_status.bits);
      SetAsyncInterrupt(Interrupt::Complete);
    }
  }
  else
  {
    Log_WarningPrintf("%s seek to [%s] failed", logical ? "Logical" : "Physical",
                      LBAToMSFString(s_reader.GetLastReadSector()).c_str());
    s_secondary_status.ClearActiveBits();
    SendAsyncErrorResponse(STAT_SEEK_ERROR, 0x04);
    s_last_sector_header_valid = false;
  }

  s_setloc_pending = false;
  s_read_after_seek = false;
  s_play_after_seek = false;
  UpdateStatusRegister();
}

void CDROM::DoStatSecondResponse()
{
  // Mainly for Reset/MotorOn.
  if (!CanReadMedia())
  {
    SendAsyncErrorResponse(STAT_ERROR, 0x08);
    return;
  }

  s_async_response_fifo.Clear();
  s_async_response_fifo.Push(s_secondary_status.bits);
  SetAsyncInterrupt(Interrupt::Complete);
}

void CDROM::DoChangeSessionComplete()
{
  Log_DebugPrintf("Changing session complete");
  ClearDriveState();
  s_secondary_status.ClearActiveBits();
  s_secondary_status.motor_on = true;

  s_async_response_fifo.Clear();
  if (s_async_command_parameter == 0x01)
  {
    s_async_response_fifo.Push(s_secondary_status.bits);
    SetAsyncInterrupt(Interrupt::Complete);
  }
  else
  {
    // we don't emulate multisession discs.. for now
    SendAsyncErrorResponse(STAT_SEEK_ERROR, 0x40);
  }
}

void CDROM::DoSpinUpComplete()
{
  Log_DebugPrintf("Spinup complete");
  s_drive_state = DriveState::Idle;
  s_drive_event->Deactivate();
  s_secondary_status.ClearActiveBits();
  s_secondary_status.motor_on = true;
}

void CDROM::DoSpeedChangeOrImplicitTOCReadComplete()
{
  Log_DebugPrintf("Speed change/implicit TOC read complete");
  s_drive_state = DriveState::Idle;
  s_drive_event->Deactivate();
}

void CDROM::DoIDRead()
{
  Log_DebugPrintf("ID read complete");
  s_secondary_status.ClearActiveBits();
  s_secondary_status.motor_on = CanReadMedia();

  // TODO: Audio CD.
  u8 stat_byte = s_secondary_status.bits;
  u8 flags_byte = 0;
  if (!CanReadMedia())
  {
    stat_byte |= STAT_ID_ERROR;
    flags_byte |= (1 << 6); // Disc Missing
  }
  else
  {
    if (IsMediaAudioCD())
    {
      stat_byte |= STAT_ID_ERROR;
      flags_byte |= (1 << 7) | (1 << 4); // Unlicensed + Audio CD
    }
    else if (!IsMediaPS1Disc() || !DoesMediaRegionMatchConsole())
    {
      stat_byte |= STAT_ID_ERROR;
      flags_byte |= (1 << 7); // Unlicensed
    }
  }

  s_async_response_fifo.Clear();
  s_async_response_fifo.Push(stat_byte);
  s_async_response_fifo.Push(flags_byte);
  s_async_response_fifo.Push(0x20); // TODO: Disc type from TOC
  s_async_response_fifo.Push(0x00); // TODO: Session info?

  static constexpr u32 REGION_STRING_LENGTH = 4;
  static constexpr std::array<std::array<u8, REGION_STRING_LENGTH>, static_cast<size_t>(DiscRegion::Count)>
    region_strings = {{{'S', 'C', 'E', 'I'}, {'S', 'C', 'E', 'A'}, {'S', 'C', 'E', 'E'}, {0, 0, 0, 0}, {0, 0, 0, 0}}};
  s_async_response_fifo.PushRange(region_strings[static_cast<u8>(s_disc_region)].data(), REGION_STRING_LENGTH);

  SetAsyncInterrupt((flags_byte != 0) ? Interrupt::Error : Interrupt::Complete);
}

void CDROM::StopReadingWithDataEnd()
{
  ClearAsyncInterrupt();
  s_async_response_fifo.Push(s_secondary_status.bits);
  SetAsyncInterrupt(Interrupt::DataEnd);

  s_secondary_status.ClearActiveBits();
  ClearDriveState();
}

void CDROM::StartMotor()
{
  if (s_drive_state == DriveState::SpinningUp)
  {
    Log_DevPrintf("Starting motor - already spinning up");
    return;
  }

  Log_DevPrintf("Starting motor");
  s_drive_state = DriveState::SpinningUp;
  s_drive_event->Schedule(GetTicksForSpinUp());
}

void CDROM::StopMotor()
{
  s_secondary_status.ClearActiveBits();
  s_secondary_status.motor_on = false;
  ClearDriveState();
  SetHoldPosition(0, false);
  s_last_sector_header_valid = false; // TODO: correct?
}

void CDROM::DoSectorRead()
{
  // TODO: Queue the next read here and swap the buffer.
  // TODO: Error handling
  if (!s_reader.WaitForReadToComplete())
    Panic("Sector read failed");

  s_current_lba = s_reader.GetLastReadSector();
  s_physical_lba = s_current_lba;
  s_physical_lba_update_tick = System::GetGlobalTickCounter();
  s_physical_lba_update_carry = 0;

  s_secondary_status.SetReadingBits(s_drive_state == DriveState::Playing);

  const CDImage::SubChannelQ& subq = s_reader.GetSectorSubQ();
  const bool subq_valid = subq.IsCRCValid();
  if (subq_valid)
  {
    s_last_subq = subq;
  }
  else
  {
    Log_DevPrintf("Sector %u [%s] has invalid subchannel Q", s_current_lba, LBAToMSFString(s_current_lba).c_str());
  }

  if (subq.track_number_bcd == CDImage::LEAD_OUT_TRACK_NUMBER)
  {
    Log_DevPrintf("Read reached lead-out area of disc at LBA %u, stopping", s_reader.GetLastReadSector());
    StopReadingWithDataEnd();
    StopMotor();
    return;
  }

  const bool is_data_sector = subq.IsData();
  if (!is_data_sector)
  {
    if (s_play_track_number_bcd == 0)
    {
      // track number was not specified, but we've found the track now
      s_play_track_number_bcd = subq.track_number_bcd;
      Log_DebugPrintf("Setting playing track number to %u", s_play_track_number_bcd);
    }
    else if (s_mode.auto_pause && subq.track_number_bcd != s_play_track_number_bcd)
    {
      // we don't want to update the position if the track changes, so we check it before reading the actual sector.
      Log_DevPrintf("Auto pause at the start of track %02x (LBA %u)", s_last_subq.track_number_bcd, s_current_lba);
      StopReadingWithDataEnd();
      return;
    }
  }
  else
  {
    ProcessDataSectorHeader(s_reader.GetSectorBuffer().data());
  }

  u32 next_sector = s_current_lba + 1u;
  if (is_data_sector && s_drive_state == DriveState::Reading)
  {
    ProcessDataSector(s_reader.GetSectorBuffer().data(), subq);
  }
  else if (!is_data_sector &&
           (s_drive_state == DriveState::Playing || (s_drive_state == DriveState::Reading && s_mode.cdda)))
  {
    ProcessCDDASector(s_reader.GetSectorBuffer().data(), subq, subq_valid);

    if (s_fast_forward_rate != 0)
      next_sector = s_current_lba + SignExtend32(s_fast_forward_rate);
  }
  else if (s_drive_state != DriveState::Reading && s_drive_state != DriveState::Playing)
  {
    Panic("Not reading or playing");
  }
  else
  {
    Log_WarningPrintf("Skipping sector %u as it is a %s sector and we're not %s", s_current_lba,
                      is_data_sector ? "data" : "audio", is_data_sector ? "reading" : "playing");
  }

  s_requested_lba = next_sector;
  s_reader.QueueReadSector(s_requested_lba);
}

ALWAYS_INLINE_RELEASE void CDROM::ProcessDataSectorHeader(const u8* raw_sector)
{
  std::memcpy(&s_last_sector_header, &raw_sector[SECTOR_SYNC_SIZE], sizeof(s_last_sector_header));
  std::memcpy(&s_last_sector_subheader, &raw_sector[SECTOR_SYNC_SIZE + sizeof(s_last_sector_header)],
              sizeof(s_last_sector_subheader));
  s_last_sector_header_valid = true;
}

ALWAYS_INLINE_RELEASE void CDROM::ProcessDataSector(const u8* raw_sector, const CDImage::SubChannelQ& subq)
{
  const u32 sb_num = (s_current_write_sector_buffer + 1) % NUM_SECTOR_BUFFERS;
  Log_DevPrintf("Read sector %u [%s]: mode %u submode 0x%02X into buffer %u", s_current_lba,
                LBAToMSFString(s_current_lba).c_str(), ZeroExtend32(s_last_sector_header.sector_mode),
                ZeroExtend32(s_last_sector_subheader.submode.bits), sb_num);

  if (s_mode.xa_enable && s_last_sector_header.sector_mode == 2)
  {
    if (s_last_sector_subheader.submode.realtime && s_last_sector_subheader.submode.audio)
    {
      ProcessXAADPCMSector(raw_sector, subq);

      // Audio+realtime sectors aren't delivered to the CPU.
      return;
    }
  }

  // TODO: How does XA relate to this buffering?
  SectorBuffer* sb = &s_sector_buffers[sb_num];
  if (sb->size > 0)
  {
    Log_DevPrintf("Sector buffer %u was not read, previous sector dropped",
                  (s_current_write_sector_buffer - 1) % NUM_SECTOR_BUFFERS);
  }

  if (s_mode.ignore_bit)
    Log_WarningPrintf("SetMode.4 bit set on read of sector %u", s_current_lba);

  if (s_mode.read_raw_sector)
  {
    std::memcpy(sb->data.data(), raw_sector + SECTOR_SYNC_SIZE, RAW_SECTOR_OUTPUT_SIZE);
    sb->size = RAW_SECTOR_OUTPUT_SIZE;
  }
  else
  {
    // TODO: This should actually depend on the mode...
    if (s_last_sector_header.sector_mode != 2)
    {
      Log_WarningPrintf("Ignoring non-mode2 sector at %u", s_current_lba);
      return;
    }

    std::memcpy(sb->data.data(), raw_sector + CDImage::SECTOR_SYNC_SIZE + 12, DATA_SECTOR_OUTPUT_SIZE);
    sb->size = DATA_SECTOR_OUTPUT_SIZE;
  }

  s_current_write_sector_buffer = sb_num;

  // Deliver to CPU
  if (HasPendingAsyncInterrupt())
  {
    Log_WarningPrintf("Data interrupt was not delivered");
    ClearAsyncInterrupt();
  }

  if (HasPendingInterrupt())
  {
    const u32 sectors_missed = (s_current_write_sector_buffer - s_current_read_sector_buffer) % NUM_SECTOR_BUFFERS;
    if (sectors_missed > 1)
      Log_WarningPrintf("Interrupt not processed in time, missed %u sectors", sectors_missed - 1);
  }

  s_async_response_fifo.Push(s_secondary_status.bits);
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

std::tuple<s16, s16> CDROM::GetAudioFrame()
{
  const u32 frame = s_audio_fifo.IsEmpty() ? 0u : s_audio_fifo.Pop();
  const s16 left = static_cast<s16>(Truncate16(frame));
  const s16 right = static_cast<s16>(Truncate16(frame >> 16));
  const s16 left_out = SaturateVolume(ApplyVolume(left, s_cd_audio_volume_matrix[0][0]) +
                                      ApplyVolume(right, s_cd_audio_volume_matrix[1][0]));
  const s16 right_out = SaturateVolume(ApplyVolume(left, s_cd_audio_volume_matrix[0][1]) +
                                       ApplyVolume(right, s_cd_audio_volume_matrix[1][1]));
  return std::tuple<s16, s16>(left_out, right_out);
}

void CDROM::AddCDAudioFrame(s16 left, s16 right)
{
  s_audio_fifo.Push(ZeroExtend32(static_cast<u16>(left)) | (ZeroExtend32(static_cast<u16>(right)) << 16));
}

s32 CDROM::ApplyVolume(s16 sample, u8 volume)
{
  return s32(sample) * static_cast<s32>(ZeroExtend32(volume)) >> 7;
}

s16 CDROM::SaturateVolume(s32 volume)
{
  return static_cast<s16>((volume < -0x8000) ? -0x8000 : ((volume > 0x7FFF) ? 0x7FFF : volume));
}

template<bool STEREO, bool SAMPLE_RATE>
void CDROM::ResampleXAADPCM(const s16* frames_in, u32 num_frames_in)
{
  // Since the disc reads and SPU are running at different speeds, we might be _slightly_ behind, which is fine, since
  // the SPU will over-read in the next batch to catch up.
  if (s_audio_fifo.GetSize() > AUDIO_FIFO_LOW_WATERMARK)
  {
    Log_DevPrintf("Dropping %u XA frames because audio FIFO still has %u frames", num_frames_in,
                  s_audio_fifo.GetSize());
    return;
  }

  s16* left_ringbuf = s_xa_resample_ring_buffer[0].data();
  s16* right_ringbuf = s_xa_resample_ring_buffer[1].data();
  u8 p = s_xa_resample_p;
  u8 sixstep = s_xa_resample_sixstep;
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
          AddCDAudioFrame(left_interp, right_interp);
        }
      }
    }
  }

  s_xa_resample_p = p;
  s_xa_resample_sixstep = sixstep;
}

void CDROM::ResetCurrentXAFile()
{
  s_xa_current_channel_number = 0;
  s_xa_current_file_number = 0;
  s_xa_current_set = false;
}

void CDROM::ResetAudioDecoder()
{
  ResetCurrentXAFile();

  s_xa_last_samples.fill(0);
  for (u32 i = 0; i < 2; i++)
  {
    s_xa_resample_ring_buffer[i].fill(0);
    s_xa_resample_p = 0;
    s_xa_resample_sixstep = 6;
  }
  s_audio_fifo.Clear();
}

ALWAYS_INLINE_RELEASE void CDROM::ProcessXAADPCMSector(const u8* raw_sector, const CDImage::SubChannelQ& subq)
{
  // Check for automatic ADPCM filter.
  if (s_mode.xa_filter && (s_last_sector_subheader.file_number != s_xa_filter_file_number ||
                           s_last_sector_subheader.channel_number != s_xa_filter_channel_number))
  {
    Log_DebugPrintf("Skipping sector due to filter mismatch (expected %u/%u got %u/%u)", s_xa_filter_file_number,
                    s_xa_filter_channel_number, s_last_sector_subheader.file_number,
                    s_last_sector_subheader.channel_number);
    return;
  }

  // Track the current file being played. If this is not set by the filter, it'll be set by the first file/sector which
  // is read. Fixes audio in Tomb Raider III menu.
  if (!s_xa_current_set)
  {
    // Some games (Taxi 2 and Blues Blues) have junk audio sectors with a channel number of 255.
    // We need to skip them otherwise it ends up playing the incorrect file.
    // TODO: Verify with a hardware test.
    if (s_last_sector_subheader.channel_number == 255 && (!s_mode.xa_filter || s_xa_filter_channel_number != 255))
    {
      Log_WarningPrintf("Skipping XA file with file number %u and channel number %u (submode 0x%02X coding 0x%02X)",
                        s_last_sector_subheader.file_number, s_last_sector_subheader.channel_number,
                        s_last_sector_subheader.submode.bits, s_last_sector_subheader.codinginfo.bits);
      return;
    }

    s_xa_current_file_number = s_last_sector_subheader.file_number;
    s_xa_current_channel_number = s_last_sector_subheader.channel_number;
    s_xa_current_set = true;
  }
  else if (s_last_sector_subheader.file_number != s_xa_current_file_number ||
           s_last_sector_subheader.channel_number != s_xa_current_channel_number)
  {
    Log_DebugPrintf("Skipping sector due to current file mismatch (expected %u/%u got %u/%u)", s_xa_current_file_number,
                    s_xa_current_channel_number, s_last_sector_subheader.file_number,
                    s_last_sector_subheader.channel_number);
    return;
  }

  // Reset current file on EOF, and play the file in the next sector.
  if (s_last_sector_subheader.submode.eof)
    ResetCurrentXAFile();

  std::array<s16, CDXA::XA_ADPCM_SAMPLES_PER_SECTOR_4BIT> sample_buffer;
  CDXA::DecodeADPCMSector(raw_sector, sample_buffer.data(), s_xa_last_samples.data());

  // Only send to SPU if we're not muted.
  if (s_muted || s_adpcm_muted || g_settings.cdrom_mute_cd_audio)
    return;

  SPU::GeneratePendingSamples();

  if (s_last_sector_subheader.codinginfo.IsStereo())
  {
    const u32 num_samples = s_last_sector_subheader.codinginfo.GetSamplesPerSector() / 2;
    if (s_last_sector_subheader.codinginfo.IsHalfSampleRate())
      ResampleXAADPCM<true, true>(sample_buffer.data(), num_samples);
    else
      ResampleXAADPCM<true, false>(sample_buffer.data(), num_samples);
  }
  else
  {
    const u32 num_samples = s_last_sector_subheader.codinginfo.GetSamplesPerSector();
    if (s_last_sector_subheader.codinginfo.IsHalfSampleRate())
      ResampleXAADPCM<false, true>(sample_buffer.data(), num_samples);
    else
      ResampleXAADPCM<false, false>(sample_buffer.data(), num_samples);
  }
}

static s16 GetPeakVolume(const u8* raw_sector, u8 channel)
{
  static constexpr u32 NUM_SAMPLES = CDImage::RAW_SECTOR_SIZE / sizeof(s16);

#if defined(CPU_ARCH_SSE) || defined(CPU_ARCH_NEON)

  static_assert(Common::IsAlignedPow2(NUM_SAMPLES, 8));
  const u8* current_ptr = raw_sector;
  s16 v_peaks[8];

#if defined(CPU_ARCH_SSE)
  __m128i v_peak = _mm_set1_epi16(0);
  for (u32 i = 0; i < NUM_SAMPLES; i += 8)
  {
    __m128i val = _mm_loadu_si128(reinterpret_cast<const __m128i*>(current_ptr));
    v_peak = _mm_max_epi16(val, v_peak);
    current_ptr += 16;
  }
  _mm_store_si128(reinterpret_cast<__m128i*>(v_peaks), v_peak);
#elif defined(CPU_ARCH_NEON)
  int16x8_t v_peak = vdupq_n_s16(0);
  for (u32 i = 0; i < NUM_SAMPLES; i += 8)
  {
    int16x8_t val = vld1q_s16(reinterpret_cast<const s16*>(current_ptr));
    v_peak = vmaxq_s16(val, v_peak);
    current_ptr += 16;
  }
  vst1q_s16(v_peaks, v_peak);
#endif

  if (channel == 0)
    return std::max(v_peaks[0], std::max(v_peaks[2], std::max(v_peaks[4], v_peaks[6])));
  else
    return std::max(v_peaks[1], std::max(v_peaks[3], std::max(v_peaks[5], v_peaks[7])));
#else
  const u8* current_ptr = raw_sector + (channel * sizeof(s16));
  s16 peak = 0;

  for (u32 i = 0; i < NUM_SAMPLES; i += 2)
  {
    s16 sample;
    std::memcpy(&sample, current_ptr, sizeof(sample));
    peak = std::max(peak, sample);
    current_ptr += sizeof(s16) * 2;
  }

  return peak;
#endif
}

ALWAYS_INLINE_RELEASE void CDROM::ProcessCDDASector(const u8* raw_sector, const CDImage::SubChannelQ& subq,
                                                    bool subq_valid)
{
  // For CDDA sectors, the whole sector contains the audio data.
  Log_DevPrintf("Read sector %u as CDDA", s_current_lba);

  // The reporting doesn't happen if we're reading with the CDDA mode bit set.
  if (s_drive_state == DriveState::Playing && s_mode.report_audio && subq_valid)
  {
    const u8 frame_nibble = subq.absolute_frame_bcd >> 4;

    if (s_last_cdda_report_frame_nibble != frame_nibble)
    {
      s_last_cdda_report_frame_nibble = frame_nibble;

      ClearAsyncInterrupt();
      s_async_response_fifo.Push(s_secondary_status.bits);
      s_async_response_fifo.Push(subq.track_number_bcd);
      s_async_response_fifo.Push(subq.index_number_bcd);
      if (subq.absolute_frame_bcd & 0x10)
      {
        s_async_response_fifo.Push(subq.relative_minute_bcd);
        s_async_response_fifo.Push(0x80 | subq.relative_second_bcd);
        s_async_response_fifo.Push(subq.relative_frame_bcd);
      }
      else
      {
        s_async_response_fifo.Push(subq.absolute_minute_bcd);
        s_async_response_fifo.Push(subq.absolute_second_bcd);
        s_async_response_fifo.Push(subq.absolute_frame_bcd);
      }

      const u8 channel = subq.absolute_second_bcd & 1u;
      const s16 peak_volume = std::min<s16>(GetPeakVolume(raw_sector, channel), 32767);
      const u16 peak_value = (ZeroExtend16(channel) << 15) | peak_volume;

      s_async_response_fifo.Push(Truncate8(peak_value));      // peak low
      s_async_response_fifo.Push(Truncate8(peak_value >> 8)); // peak high
      SetAsyncInterrupt(Interrupt::DataReady);

      Log_DevPrintf("CDDA report at track[%02x] index[%02x] rel[%02x:%02x:%02x] abs[%02x:%02x:%02x] peak[%u:%d]",
                    subq.track_number_bcd, subq.index_number_bcd, subq.relative_minute_bcd, subq.relative_second_bcd,
                    subq.relative_frame_bcd, subq.absolute_minute_bcd, subq.absolute_second_bcd,
                    subq.absolute_frame_bcd, channel, peak_volume);
    }
  }

  // Apply volume when pushing sectors to SPU.
  if (s_muted || g_settings.cdrom_mute_cd_audio)
    return;

  SPU::GeneratePendingSamples();

  constexpr bool is_stereo = true;
  constexpr u32 num_samples = CDImage::RAW_SECTOR_SIZE / sizeof(s16) / (is_stereo ? 2 : 1);
  const u32 remaining_space = s_audio_fifo.GetSpace();
  if (remaining_space < num_samples)
  {
    Log_WarningPrintf("Dropping %u frames from audio FIFO", num_samples - remaining_space);
    s_audio_fifo.Remove(num_samples - remaining_space);
  }

  const u8* sector_ptr = raw_sector;
  for (u32 i = 0; i < num_samples; i++)
  {
    s16 samp_left, samp_right;
    std::memcpy(&samp_left, sector_ptr, sizeof(samp_left));
    std::memcpy(&samp_right, sector_ptr + sizeof(s16), sizeof(samp_right));
    sector_ptr += sizeof(s16) * 2;
    AddCDAudioFrame(samp_left, samp_right);
  }
}

void CDROM::LoadDataFIFO()
{
  if (!s_data_fifo.IsEmpty())
  {
    Log_DevPrintf("Load data fifo when not empty");
    return;
  }

  // any data to load?
  SectorBuffer& sb = s_sector_buffers[s_current_read_sector_buffer];
  if (sb.size == 0)
  {
    Log_WarningPrintf("Attempting to load empty sector buffer");
    s_data_fifo.PushRange(sb.data.data(), RAW_SECTOR_OUTPUT_SIZE);
  }
  else
  {
    s_data_fifo.PushRange(sb.data.data(), sb.size);
    sb.size = 0;
  }

  Log_DebugPrintf("Loaded %u bytes to data FIFO from buffer %u", s_data_fifo.GetSize(), s_current_read_sector_buffer);

  SectorBuffer& next_sb = s_sector_buffers[s_current_write_sector_buffer];
  if (next_sb.size > 0)
  {
    Log_DevPrintf("Sending additional INT1 for missed sector in buffer %u", s_current_write_sector_buffer);
    s_async_response_fifo.Push(s_secondary_status.bits);
    SetAsyncInterrupt(Interrupt::DataReady);
  }
}

void CDROM::ClearSectorBuffers()
{
  for (u32 i = 0; i < NUM_SECTOR_BUFFERS; i++)
    s_sector_buffers[i].size = 0;
}

void CDROM::CreateFileMap()
{
  s_file_map.clear();
  s_file_map_created = true;

  if (!s_reader.HasMedia())
    return;

  s_reader.WaitForIdle();
  CDImage* media = s_reader.GetMedia();
  IsoReader iso;
  if (!iso.Open(media, 1))
  {
    Log_ErrorFmt("Failed to open ISO filesystem.");
    return;
  }

  Log_DevFmt("Creating file map for {}...", media->GetFileName());
  s_file_map.emplace(iso.GetPVDLBA(), std::make_pair(iso.GetPVDLBA(), std::string("PVD")));
  CreateFileMap(iso, std::string_view());
  Log_DevFmt("Found {} files", s_file_map.size());
}

void CDROM::CreateFileMap(IsoReader& iso, const std::string_view& dir)
{
  for (auto& [path, entry] : iso.GetEntriesInDirectory(dir))
  {
    if (entry.IsDirectory())
    {
      Log_DevFmt("{}-{} = {}", entry.location_le, entry.location_le + entry.GetSizeInSectors() - 1, path);
      s_file_map.emplace(entry.location_le, std::make_pair(entry.location_le + entry.GetSizeInSectors() - 1,
                                                           fmt::format("<DIR> {}", path)));

      CreateFileMap(iso, path);
      continue;
    }

    Log_DevFmt("{}-{} = {}", entry.location_le, entry.location_le + entry.GetSizeInSectors() - 1, path);
    s_file_map.emplace(entry.location_le,
                       std::make_pair(entry.location_le + entry.GetSizeInSectors() - 1, std::move(path)));
  }
}

const std::string* CDROM::LookupFileMap(u32 lba, u32* start_lba, u32* end_lba)
{
  if (s_file_map.empty())
    return nullptr;

  auto iter = s_file_map.lower_bound(lba);
  if (iter == s_file_map.end())
    iter = (++s_file_map.rbegin()).base();
  if (lba < iter->first)
  {
    // before first file
    if (iter == s_file_map.begin())
      return nullptr;

    --iter;
  }
  if (lba > iter->second.first)
    return nullptr;

  *start_lba = iter->first;
  *end_lba = iter->second.first;
  return &iter->second.second;
}

void CDROM::DrawDebugWindow()
{
  static const ImVec4 active_color{1.0f, 1.0f, 1.0f, 1.0f};
  static const ImVec4 inactive_color{0.4f, 0.4f, 0.4f, 1.0f};
  const float framebuffer_scale = Host::GetOSDScale();

  ImGui::SetNextWindowSize(ImVec2(800.0f * framebuffer_scale, 580.0f * framebuffer_scale), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("CDROM State", nullptr))
  {
    ImGui::End();
    return;
  }

  // draw voice states
  if (ImGui::CollapsingHeader("Media", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (s_reader.HasMedia())
    {
      const CDImage* media = s_reader.GetMedia();
      const CDImage::Position disc_position = CDImage::Position::FromLBA(s_current_lba);
      const float start_y = ImGui::GetCursorPosY();

      if (media->HasSubImages())
      {
        ImGui::Text("Filename: %s [Subimage %u of %u] [%u buffered sectors]", media->GetFileName().c_str(),
                    media->GetCurrentSubImage() + 1u, media->GetSubImageCount(), s_reader.GetBufferedSectorCount());
      }
      else
      {
        ImGui::Text("Filename: %s [%u buffered sectors]", media->GetFileName().c_str(),
                    s_reader.GetBufferedSectorCount());
      }

      ImGui::Text("Disc Position: MSF[%02u:%02u:%02u] LBA[%u]", disc_position.minute, disc_position.second,
                  disc_position.frame, disc_position.ToLBA());

      if (media->GetTrackNumber() > media->GetTrackCount())
      {
        ImGui::Text("Track Position: Lead-out");
      }
      else
      {
        const CDImage::Position track_position = CDImage::Position::FromLBA(
          s_current_lba - media->GetTrackStartPosition(static_cast<u8>(media->GetTrackNumber())));
        ImGui::Text("Track Position: Number[%u] MSF[%02u:%02u:%02u] LBA[%u]", media->GetTrackNumber(),
                    track_position.minute, track_position.second, track_position.frame, track_position.ToLBA());
      }

      ImGui::Text("Last Sector: %02X:%02X:%02X (Mode %u)", s_last_sector_header.minute, s_last_sector_header.second,
                  s_last_sector_header.frame, s_last_sector_header.sector_mode);

      if (s_show_current_file)
      {
        if (media->GetTrackNumber() == 1)
        {
          if (!s_file_map_created)
            CreateFileMap();

          u32 current_file_start_lba, current_file_end_lba;
          const u32 track_lba = s_current_lba - media->GetTrackStartPosition(static_cast<u8>(media->GetTrackNumber()));
          const std::string* current_file = LookupFileMap(track_lba, &current_file_start_lba, &current_file_end_lba);
          if (current_file)
          {
            static constexpr auto readable_size = [](u32 val) {
              // based on
              // https://stackoverflow.com/questions/1449805/how-to-format-a-number-using-comma-as-thousands-separator-in-c
              // don't want to use locale...
              TinyString ret;
              TinyString temp;
              temp.append_format("{}", val);

              u32 commas = 2u - (temp.length() % 3u);
              for (const char* p = temp.c_str(); *p != 0u; p++)
              {
                ret.append(*p);
                if (commas == 1)
                  ret.append(',');
                commas = (commas + 1) % 3;
              }

              DebugAssert(!ret.empty());
              ret.erase(-1);
              return ret;
            };
            ImGui::Text(
              "Current File: %s (%s of %s bytes)", current_file->c_str(),
              readable_size((track_lba - current_file_start_lba) * CDImage::DATA_SECTOR_SIZE).c_str(),
              readable_size((current_file_end_lba - current_file_start_lba + 1) * CDImage::DATA_SECTOR_SIZE).c_str());
          }
          else
          {
            ImGui::Text("Current File: <Unknown>");
          }
        }
        else
        {
          ImGui::Text("Current File: <Non-Data Track>");
        }

        ImGui::SameLine();
        ImGui::Text("[%u files on disc]", static_cast<u32>(s_file_map.size()));
      }
      else
      {
        const float end_y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 120.0f * framebuffer_scale);
        ImGui::SetCursorPosY(start_y);
        if (ImGui::Button("Show Current File"))
          s_show_current_file = true;

        ImGui::SetCursorPosY(end_y);
      }
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

    ImGui::TextColored(s_status.ADPBUSY ? active_color : inactive_color, "ADPBUSY: %s",
                       s_status.ADPBUSY ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_secondary_status.error ? active_color : inactive_color, "Error: %s",
                       s_secondary_status.error ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_mode.cdda ? active_color : inactive_color, "CDDA: %s", s_mode.cdda ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextColored(s_status.PRMEMPTY ? active_color : inactive_color, "PRMEMPTY: %s",
                       s_status.PRMEMPTY ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_secondary_status.motor_on ? active_color : inactive_color, "Motor On: %s",
                       s_secondary_status.motor_on ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_mode.auto_pause ? active_color : inactive_color, "Auto Pause: %s",
                       s_mode.auto_pause ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextColored(s_status.PRMWRDY ? active_color : inactive_color, "PRMWRDY: %s",
                       s_status.PRMWRDY ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_secondary_status.seek_error ? active_color : inactive_color, "Seek Error: %s",
                       s_secondary_status.seek_error ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_mode.report_audio ? active_color : inactive_color, "Report Audio: %s",
                       s_mode.report_audio ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextColored(s_status.RSLRRDY ? active_color : inactive_color, "RSLRRDY: %s",
                       s_status.RSLRRDY ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_secondary_status.id_error ? active_color : inactive_color, "ID Error: %s",
                       s_secondary_status.id_error ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_mode.xa_filter ? active_color : inactive_color, "XA Filter: %s (File %u Channel %u)",
                       s_mode.xa_filter ? "Yes" : "No", s_xa_filter_file_number, s_xa_filter_channel_number);
    ImGui::NextColumn();

    ImGui::TextColored(s_status.DRQSTS ? active_color : inactive_color, "DRQSTS: %s", s_status.DRQSTS ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_secondary_status.shell_open ? active_color : inactive_color, "Shell Open: %s",
                       s_secondary_status.shell_open ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_mode.ignore_bit ? active_color : inactive_color, "Ignore Bit: %s",
                       s_mode.ignore_bit ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextColored(s_status.BUSYSTS ? active_color : inactive_color, "BUSYSTS: %s",
                       s_status.BUSYSTS ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_secondary_status.reading ? active_color : inactive_color, "Reading: %s",
                       s_secondary_status.reading ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_mode.read_raw_sector ? active_color : inactive_color, "Read Raw Sectors: %s",
                       s_mode.read_raw_sector ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::NextColumn();
    ImGui::TextColored(s_secondary_status.seeking ? active_color : inactive_color, "Seeking: %s",
                       s_secondary_status.seeking ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_mode.xa_enable ? active_color : inactive_color, "XA Enable: %s",
                       s_mode.xa_enable ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::NextColumn();
    ImGui::TextColored(s_secondary_status.playing_cdda ? active_color : inactive_color, "Playing CDDA: %s",
                       s_secondary_status.playing_cdda ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_mode.double_speed ? active_color : inactive_color, "Double Speed: %s",
                       s_mode.double_speed ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::Columns(1);
    ImGui::NewLine();

    if (HasPendingCommand())
    {
      ImGui::TextColored(active_color, "Command: %s (0x%02X) (%d ticks remaining)",
                         s_command_info[static_cast<u8>(s_command)].name, static_cast<u8>(s_command),
                         s_command_event->IsActive() ? s_command_event->GetTicksUntilNextExecution() : 0);
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
                         s_drive_state_names[static_cast<u8>(s_drive_state)],
                         s_drive_event->IsActive() ? s_drive_event->GetTicksUntilNextExecution() : 0);
    }

    ImGui::Text("Interrupt Enable Register: 0x%02X", s_interrupt_enable_register);
    ImGui::Text("Interrupt Flag Register: 0x%02X", s_interrupt_flag_register);

    if (HasPendingAsyncInterrupt())
    {
      ImGui::SameLine();
      ImGui::TextColored(inactive_color, " (0x%02X pending)", s_pending_async_interrupt);
    }
  }

  if (ImGui::CollapsingHeader("CD Audio", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (s_drive_state == DriveState::Reading && s_mode.xa_enable)
    {
      ImGui::TextColored(active_color, "Playing: XA-ADPCM (File %u / Channel %u)", s_xa_current_file_number,
                         s_xa_current_channel_number);
    }
    else if (s_drive_state == DriveState::Playing)
    {
      ImGui::TextColored(active_color, "Playing: CDDA (Track %x)", s_last_subq.track_number_bcd);
    }
    else
    {
      ImGui::TextColored(inactive_color, "Playing: Inactive");
    }

    ImGui::TextColored(s_muted ? inactive_color : active_color, "Muted: %s", s_muted ? "Yes" : "No");
    ImGui::Text("Left Output: Left Channel=%02X (%u%%), Right Channel=%02X (%u%%)", s_cd_audio_volume_matrix[0][0],
                ZeroExtend32(s_cd_audio_volume_matrix[0][0]) * 100 / 0x80, s_cd_audio_volume_matrix[1][0],
                ZeroExtend32(s_cd_audio_volume_matrix[1][0]) * 100 / 0x80);
    ImGui::Text("Right Output: Left Channel=%02X (%u%%), Right Channel=%02X (%u%%)", s_cd_audio_volume_matrix[0][1],
                ZeroExtend32(s_cd_audio_volume_matrix[0][1]) * 100 / 0x80, s_cd_audio_volume_matrix[1][1],
                ZeroExtend32(s_cd_audio_volume_matrix[1][1]) * 100 / 0x80);

    ImGui::Text("Audio FIFO Size: %u frames", s_audio_fifo.GetSize());
  }

  ImGui::End();
}
