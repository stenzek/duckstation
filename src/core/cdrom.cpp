// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cdrom.h"
#include "cdrom_async_reader.h"
#include "cdrom_subq_replacement.h"
#include "dma.h"
#include "fullscreen_ui.h"
#include "host.h"
#include "interrupt_controller.h"
#include "settings.h"
#include "spu.h"
#include "system.h"
#include "timing_event.h"

#include "util/cd_image.h"
#include "util/imgui_manager.h"
#include "util/iso_reader.h"
#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "common/file_system.h"
#include "common/gsvector.h"
#include "common/heap_array.h"
#include "common/log.h"

#include "fmt/format.h"
#include "imgui.h"

#include <cmath>
#include <map>
#include <vector>

LOG_CHANNEL(CDROM);

namespace CDROM {
namespace {

enum : u32
{
  RAW_SECTOR_OUTPUT_SIZE = CDImage::RAW_SECTOR_SIZE - CDImage::SECTOR_SYNC_SIZE,
  DATA_SECTOR_OUTPUT_SIZE = CDImage::DATA_SECTOR_SIZE,
  SECTOR_SYNC_SIZE = CDImage::SECTOR_SYNC_SIZE,
  SECTOR_HEADER_SIZE = CDImage::SECTOR_HEADER_SIZE,
  MODE1_HEADER_SIZE = CDImage::MODE1_HEADER_SIZE,
  MODE2_HEADER_SIZE = CDImage::MODE2_HEADER_SIZE,
  SUBQ_SECTOR_SKEW = 2,
  XA_ADPCM_SAMPLES_PER_SECTOR_4BIT = 4032, // 28 words * 8 nibbles per word * 18 chunks
  XA_ADPCM_SAMPLES_PER_SECTOR_8BIT = 2016, // 28 words * 4 bytes per word * 18 chunks
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

  CDDA_REPORT_START_DELAY = 60, // 60 frames
  MINIMUM_INTERRUPT_DELAY = 1000,
  INTERRUPT_DELAY_CYCLES = 500,
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

struct XASubHeader
{
  u8 file_number;
  u8 channel_number;

  union Submode
  {
    u8 bits;
    BitField<u8, bool, 0, 1> eor;
    BitField<u8, bool, 1, 1> video;
    BitField<u8, bool, 2, 1> audio;
    BitField<u8, bool, 3, 1> data;
    BitField<u8, bool, 4, 1> trigger;
    BitField<u8, bool, 5, 1> form2;
    BitField<u8, bool, 6, 1> realtime;
    BitField<u8, bool, 7, 1> eof;
  } submode;

  union Codinginfo
  {
    u8 bits;

    BitField<u8, bool, 0, 1> mono_stereo;
    BitField<u8, bool, 2, 1> sample_rate;
    BitField<u8, bool, 4, 1> bits_per_sample;
    BitField<u8, bool, 6, 1> emphasis;

    ALWAYS_INLINE bool IsStereo() const { return mono_stereo; }
    ALWAYS_INLINE bool IsHalfSampleRate() const { return sample_rate; }
    ALWAYS_INLINE bool Is8BitADPCM() const { return bits_per_sample; }
    u32 GetSamplesPerSector() const
    {
      return bits_per_sample ? XA_ADPCM_SAMPLES_PER_SECTOR_8BIT : XA_ADPCM_SAMPLES_PER_SECTOR_4BIT;
    }
  } codinginfo;
};

union XA_ADPCMBlockHeader
{
  u8 bits;

  BitField<u8, u8, 0, 4> shift;
  BitField<u8, u8, 4, 4> filter;

  // For both 4bit and 8bit ADPCM, reserved shift values 13..15 will act same as shift=9).
  u8 GetShift() const
  {
    const u8 shift_value = shift;
    return (shift_value > 12) ? 9 : shift_value;
  }

  u8 GetFilter() const { return filter; }
};
static_assert(sizeof(XA_ADPCMBlockHeader) == 1, "XA-ADPCM block header is one byte");

} // namespace

static TickCount SoftReset(TickCount ticks_late);

static const CDImage::SubChannelQ& GetSectorSubQ(u32 lba, const CDImage::SubChannelQ& real_subq);
static bool CanReadMedia();

static bool IsDriveIdle();
static bool IsMotorOn();
static bool IsSeeking();
static bool IsReading();
static bool IsReadingOrPlaying();
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
static TickCount GetTicksForPause();
static TickCount GetTicksForStop(bool motor_was_on);
static TickCount GetTicksForSpeedChange();
static TickCount GetTicksForTOCRead();
static CDImage::LBA GetNextSectorToBeRead();
static u32 GetSectorsPerTrack(CDImage::LBA lba);
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
static void UpdateSubQPositionWhileSeeking();
static void UpdateSubQPosition(bool update_logical);
static void EnsureLastSubQValid();
static void SetHoldPosition(CDImage::LBA lba, CDImage::LBA subq_lba);
static void ResetCurrentXAFile();
static void ResetAudioDecoder();
static void ClearSectorBuffers();
static void CheckForSectorBufferReadComplete();

// Decodes XA-ADPCM samples in an audio sector. Stereo samples are interleaved with left first.
template<bool IS_STEREO, bool IS_8BIT>
static void DecodeXAADPCMChunks(const u8* chunk_ptr, s16* samples);
template<bool STEREO>
static void ResampleXAADPCM(const s16* frames_in, u32 num_frames_in);
template<bool STEREO>
static void ResampleXAADPCM18900(const s16* frames_in, u32 num_frames_in);

static TinyString LBAToMSFString(CDImage::LBA lba);

static void CreateFileMap();
static void CreateFileMap(IsoReader& iso, std::string_view dir);
static const std::string* LookupFileMap(u32 lba, u32* start_lba, u32* end_lba);

namespace {
struct SectorBuffer
{
  FixedHeapArray<u8, RAW_SECTOR_OUTPUT_SIZE> data;
  u32 position;
  u32 size;
};

struct CDROMState
{
  TimingEvent command_event{"CDROM Command Event", 1, 1, &CDROM::ExecuteCommand, nullptr};
  TimingEvent command_second_response_event{"CDROM Command Second Response Event", 1, 1,
                                            &CDROM::ExecuteCommandSecondResponse, nullptr};
  TimingEvent async_interrupt_event{"CDROM Async Interrupt Event", INTERRUPT_DELAY_CYCLES, 1,
                                    &CDROM::DeliverAsyncInterrupt, nullptr};
  TimingEvent drive_event{"CDROM Drive Event", 1, 1, &CDROM::ExecuteDrive, nullptr};

  std::unique_ptr<CDROMSubQReplacement> subq_replacement;

  GlobalTicks subq_lba_update_tick = 0;
  GlobalTicks last_interrupt_time = 0;

  Command command = Command::None;
  Command command_second_response = Command::None;
  DriveState drive_state = DriveState::Idle;
  DiscRegion disc_region = DiscRegion::NonPS1;

  StatusRegister status = {};

  SecondaryStatusRegister secondary_status = {};
  ModeRegister mode = {};
  RequestRegister request_register = {};

  u8 interrupt_enable_register = INTERRUPT_REGISTER_MASK;
  u8 interrupt_flag_register = 0;
  u8 pending_async_interrupt = 0;

  bool setloc_pending = false;
  bool read_after_seek = false;
  bool play_after_seek = false;

  CDImage::Position setloc_position = {};
  CDImage::LBA requested_lba = 0;
  CDImage::LBA current_lba = 0;      // this is the hold position
  CDImage::LBA current_subq_lba = 0; // current position of the disc with respect to time
  CDImage::LBA seek_start_lba = 0;
  CDImage::LBA seek_end_lba = 0;
  u32 subq_lba_update_carry = 0;

  bool muted = false;
  bool adpcm_muted = false;

  u8 xa_filter_file_number = 0;
  u8 xa_filter_channel_number = 0;
  u8 xa_current_file_number = 0;
  u8 xa_current_channel_number = 0;
  bool xa_current_set = false;
  XASubHeader::Codinginfo xa_current_codinginfo = {};

  CDImage::SubChannelQ last_subq = {};
  CDImage::SectorHeader last_sector_header = {};
  XASubHeader last_sector_subheader = {};
  bool last_sector_header_valid = false; // TODO: Rename to "logical pause" or something.
  bool last_subq_needs_update = false;

  u8 cdda_report_start_delay = 0;
  u8 last_cdda_report_frame_nibble = 0xFF;
  u8 play_track_number_bcd = 0xFF;
  u8 async_command_parameter = 0x00;
  s8 fast_forward_rate = 0;

  std::array<std::array<u8, 2>, 2> cd_audio_volume_matrix{};
  std::array<std::array<u8, 2>, 2> next_cd_audio_volume_matrix{};

  std::array<s32, 4> xa_last_samples{};
  std::array<std::array<s16, XA_RESAMPLE_RING_BUFFER_SIZE>, 2> xa_resample_ring_buffer{};
  u8 xa_resample_p = 0;
  u8 xa_resample_sixstep = 6;

  InlineFIFOQueue<u8, PARAM_FIFO_SIZE> param_fifo;
  InlineFIFOQueue<u8, RESPONSE_FIFO_SIZE> response_fifo;
  InlineFIFOQueue<u8, RESPONSE_FIFO_SIZE> async_response_fifo;

  std::array<SectorBuffer, NUM_SECTOR_BUFFERS> sector_buffers;
  u32 current_read_sector_buffer = 0;
  u32 current_write_sector_buffer = 0;

  // two 16-bit samples packed in 32-bits
  HeapFIFOQueue<u32, AUDIO_FIFO_SIZE> audio_fifo;

  std::map<u32, std::pair<u32, std::string>> file_map;
  bool file_map_created = false;
  bool show_current_file = false;
};
} // namespace

ALIGN_TO_CACHE_LINE static CDROMState s_state;
ALIGN_TO_CACHE_LINE static CDROMAsyncReader s_reader;

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
  s_state.disc_region = DiscRegion::NonPS1;

  if (g_settings.cdrom_readahead_sectors > 0)
    s_reader.StartThread(g_settings.cdrom_readahead_sectors);

  Reset();
}

void CDROM::Shutdown()
{
  s_state.file_map.clear();
  s_state.file_map_created = false;
  s_state.show_current_file = false;

  s_state.drive_event.Deactivate();
  s_state.async_interrupt_event.Deactivate();
  s_state.command_second_response_event.Deactivate();
  s_state.command_event.Deactivate();
  s_reader.StopThread();
  s_reader.RemoveMedia();
}

void CDROM::Reset()
{
  s_state.command = Command::None;
  s_state.command_event.Deactivate();
  ClearCommandSecondResponse();
  ClearDriveState();
  s_state.status.bits = 0;
  s_state.secondary_status.bits = 0;
  s_state.secondary_status.motor_on = CanReadMedia();
  s_state.secondary_status.shell_open = !CanReadMedia();
  s_state.mode.bits = 0;
  s_state.mode.read_raw_sector = true;
  s_state.interrupt_enable_register = INTERRUPT_REGISTER_MASK;
  s_state.interrupt_flag_register = 0;
  s_state.last_interrupt_time = System::GetGlobalTickCounter() - MINIMUM_INTERRUPT_DELAY;
  ClearAsyncInterrupt();
  s_state.setloc_position = {};
  s_state.seek_start_lba = 0;
  s_state.seek_end_lba = 0;
  s_state.setloc_pending = false;
  s_state.read_after_seek = false;
  s_state.play_after_seek = false;
  s_state.muted = false;
  s_state.adpcm_muted = false;
  s_state.xa_filter_file_number = 0;
  s_state.xa_filter_channel_number = 0;
  s_state.xa_current_file_number = 0;
  s_state.xa_current_channel_number = 0;
  s_state.xa_current_set = false;
  std::memset(&s_state.last_sector_header, 0, sizeof(s_state.last_sector_header));
  std::memset(&s_state.last_sector_subheader, 0, sizeof(s_state.last_sector_subheader));
  s_state.last_sector_header_valid = false;
  std::memset(&s_state.last_subq, 0, sizeof(s_state.last_subq));
  s_state.cdda_report_start_delay = 0;
  s_state.last_cdda_report_frame_nibble = 0xFF;

  s_state.next_cd_audio_volume_matrix[0][0] = 0x80;
  s_state.next_cd_audio_volume_matrix[0][1] = 0x00;
  s_state.next_cd_audio_volume_matrix[1][0] = 0x00;
  s_state.next_cd_audio_volume_matrix[1][1] = 0x80;
  s_state.cd_audio_volume_matrix = s_state.next_cd_audio_volume_matrix;

  ClearSectorBuffers();
  ResetAudioDecoder();

  s_state.param_fifo.Clear();
  s_state.response_fifo.Clear();
  s_state.async_response_fifo.Clear();

  UpdateStatusRegister();

  SetHoldPosition(0, 0);
}

TickCount CDROM::SoftReset(TickCount ticks_late)
{
  const bool was_double_speed = s_state.mode.double_speed;

  ClearCommandSecondResponse();
  ClearDriveState();
  s_state.secondary_status.bits = 0;
  s_state.secondary_status.motor_on = CanReadMedia();
  s_state.secondary_status.shell_open = !CanReadMedia();
  s_state.mode.bits = 0;
  s_state.mode.read_raw_sector = true;
  s_state.request_register.bits = 0;
  ClearAsyncInterrupt();
  s_state.setloc_position = {};
  s_state.setloc_pending = false;
  s_state.read_after_seek = false;
  s_state.play_after_seek = false;
  s_state.muted = false;
  s_state.adpcm_muted = false;
  s_state.cdda_report_start_delay = 0;
  s_state.last_cdda_report_frame_nibble = 0xFF;

  ClearSectorBuffers();
  ResetAudioDecoder();

  s_state.param_fifo.Clear();
  s_state.async_response_fifo.Clear();

  UpdateStatusRegister();

  TickCount total_ticks;
  if (HasMedia())
  {
    if (IsSeeking())
      UpdateSubQPositionWhileSeeking();
    else
      UpdateSubQPosition(false);

    const TickCount speed_change_ticks = was_double_speed ? GetTicksForSpeedChange() : 0;
    const TickCount seek_ticks = (s_state.current_lba != 0) ? GetTicksForSeek(0) : 0;
    total_ticks = std::max<TickCount>(speed_change_ticks + seek_ticks, INIT_TICKS) - ticks_late;
    DEV_LOG("CDROM init total disc ticks = {} (speed change = {}, seek = {})", total_ticks, speed_change_ticks,
            seek_ticks);

    if (s_state.current_lba != 0)
    {
      s_state.drive_state = DriveState::SeekingImplicit;
      s_state.drive_event.SetIntervalAndSchedule(total_ticks);
      s_state.requested_lba = 0;
      s_reader.QueueReadSector(s_state.requested_lba);
      s_state.seek_start_lba = s_state.current_lba;
      s_state.seek_end_lba = 0;
    }
    else
    {
      s_state.drive_state = DriveState::ChangingSpeedOrTOCRead;
      s_state.drive_event.Schedule(total_ticks);
    }
  }
  else
  {
    total_ticks = INIT_TICKS - ticks_late;
  }

  return total_ticks;
}

bool CDROM::DoState(StateWrapper& sw)
{
  sw.Do(&s_state.command);
  sw.DoEx(&s_state.command_second_response, 53, Command::None);
  sw.Do(&s_state.drive_state);
  sw.Do(&s_state.status.bits);
  sw.Do(&s_state.secondary_status.bits);
  sw.Do(&s_state.mode.bits);
  sw.DoEx(&s_state.request_register.bits, 65, static_cast<u8>(0));

  bool current_double_speed = s_state.mode.double_speed;
  sw.Do(&current_double_speed);

  sw.Do(&s_state.interrupt_enable_register);
  sw.Do(&s_state.interrupt_flag_register);

  if (sw.GetVersion() < 71) [[unlikely]]
  {
    u32 last_interrupt_time32 = 0;
    sw.DoEx(&last_interrupt_time32, 57, static_cast<u32>(System::GetGlobalTickCounter() - MINIMUM_INTERRUPT_DELAY));
    s_state.last_interrupt_time = last_interrupt_time32;
  }
  else
  {
    sw.Do(&s_state.last_interrupt_time);
  }

  sw.Do(&s_state.pending_async_interrupt);
  sw.DoPOD(&s_state.setloc_position);
  sw.Do(&s_state.current_lba);
  sw.Do(&s_state.seek_start_lba);
  sw.Do(&s_state.seek_end_lba);
  sw.DoEx(&s_state.current_subq_lba, 49, s_state.current_lba);

  if (sw.GetVersion() < 71) [[unlikely]]
  {
    u32 subq_lba_update_tick32 = 0;
    sw.DoEx(&subq_lba_update_tick32, 49, static_cast<u32>(0));
    s_state.subq_lba_update_tick = subq_lba_update_tick32;
  }
  else
  {
    sw.Do(&s_state.subq_lba_update_tick);
  }

  sw.DoEx(&s_state.subq_lba_update_carry, 54, static_cast<u32>(0));
  sw.Do(&s_state.setloc_pending);
  sw.Do(&s_state.read_after_seek);
  sw.Do(&s_state.play_after_seek);
  sw.Do(&s_state.muted);
  sw.Do(&s_state.adpcm_muted);
  sw.Do(&s_state.xa_filter_file_number);
  sw.Do(&s_state.xa_filter_channel_number);
  sw.Do(&s_state.xa_current_file_number);
  sw.Do(&s_state.xa_current_channel_number);
  sw.Do(&s_state.xa_current_set);
  sw.DoBytes(&s_state.last_sector_header, sizeof(s_state.last_sector_header));
  sw.DoBytes(&s_state.last_sector_subheader, sizeof(s_state.last_sector_subheader));
  sw.Do(&s_state.last_sector_header_valid);
  sw.DoBytes(&s_state.last_subq, sizeof(s_state.last_subq));
  sw.DoEx(&s_state.cdda_report_start_delay, 72, static_cast<u8>(0));
  sw.Do(&s_state.last_cdda_report_frame_nibble);
  sw.Do(&s_state.play_track_number_bcd);
  sw.Do(&s_state.async_command_parameter);

  sw.DoEx(&s_state.fast_forward_rate, 49, static_cast<s8>(0));

  sw.Do(&s_state.cd_audio_volume_matrix);
  sw.Do(&s_state.next_cd_audio_volume_matrix);
  sw.Do(&s_state.xa_last_samples);
  sw.Do(&s_state.xa_resample_ring_buffer);
  sw.Do(&s_state.xa_resample_p);
  sw.Do(&s_state.xa_resample_sixstep);
  sw.Do(&s_state.param_fifo);
  sw.Do(&s_state.response_fifo);
  sw.Do(&s_state.async_response_fifo);

  if (sw.GetVersion() < 65)
  {
    // Skip over the "copied out data", we don't care about it.
    u32 old_fifo_size = 0;
    sw.Do(&old_fifo_size);
    sw.SkipBytes(old_fifo_size);

    sw.Do(&s_state.current_read_sector_buffer);
    sw.Do(&s_state.current_write_sector_buffer);
    for (SectorBuffer& sb : s_state.sector_buffers)
    {
      sw.Do(&sb.data);
      sw.Do(&sb.size);
      sb.position = 0;
    }

    // Try to transplant the old "data fifo" into the current sector buffer's read position.
    // I doubt this is going to work well.... don't save state in the middle of loading, ya goon.
    if (old_fifo_size > 0)
    {
      SectorBuffer& sb = s_state.sector_buffers[s_state.current_read_sector_buffer];
      sb.size = s_state.mode.read_raw_sector ? RAW_SECTOR_OUTPUT_SIZE : DATA_SECTOR_OUTPUT_SIZE;
      sb.position = (sb.size > old_fifo_size) ? (sb.size - old_fifo_size) : 0;
      s_state.request_register.BFRD = (sb.position > 0);
    }

    UpdateStatusRegister();
  }
  else
  {
    sw.Do(&s_state.current_read_sector_buffer);
    sw.Do(&s_state.current_write_sector_buffer);

    for (SectorBuffer& sb : s_state.sector_buffers)
    {
      sw.Do(&sb.size);
      sw.Do(&sb.position);

      // We're never going to access data that has already been read out, so skip saving it.
      if (sb.position < sb.size)
      {
        sw.DoBytes(&sb.data[sb.position], sb.size - sb.position);

#ifdef _DEBUG
        // Sanity test in debug builds.
        if (sb.position > 0)
          std::memset(sb.data.data(), 0, sb.position);
#endif
      }
    }
  }

  sw.Do(&s_state.audio_fifo);
  sw.Do(&s_state.requested_lba);

  if (sw.IsReading())
  {
    s_state.last_subq_needs_update = true;
    if (s_reader.HasMedia())
      s_reader.QueueReadSector(s_state.requested_lba);
    UpdateCommandEvent();
    s_state.drive_event.SetState(!IsDriveIdle());

    // Time will get fixed up later.
    s_state.command_second_response_event.SetState(s_state.command_second_response != Command::None);
  }

  return !sw.HasError();
}

bool CDROM::HasMedia()
{
  return s_reader.HasMedia();
}

const std::string& CDROM::GetMediaPath()
{
  return s_reader.GetMediaPath();
}

u32 CDROM::GetCurrentSubImage()
{
  return s_reader.HasMedia() ? s_reader.GetMedia()->GetCurrentSubImage() : 0;
}

bool CDROM::HasNonStandardOrReplacementSubQ()
{
  return ((s_reader.HasMedia() ? s_reader.GetMedia()->HasSubchannelData() : false) || s_state.subq_replacement);
}

const CDImage* CDROM::GetMedia()
{
  return s_reader.GetMedia();
}

DiscRegion CDROM::GetDiscRegion()
{
  return s_state.disc_region;
}

bool CDROM::IsMediaPS1Disc()
{
  return (s_state.disc_region != DiscRegion::NonPS1);
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

  if (s_state.disc_region == DiscRegion::Other)
    return false;

  return System::GetRegion() == System::GetConsoleRegionForDiscRegion(s_state.disc_region);
}

bool CDROM::IsDriveIdle()
{
  return s_state.drive_state == DriveState::Idle;
}

bool CDROM::IsMotorOn()
{
  return s_state.secondary_status.motor_on;
}

bool CDROM::IsSeeking()
{
  return (s_state.drive_state == DriveState::SeekingLogical || s_state.drive_state == DriveState::SeekingPhysical ||
          s_state.drive_state == DriveState::SeekingImplicit);
}

bool CDROM::IsReading()
{
  return (s_state.drive_state == DriveState::Reading);
}

bool CDROM::IsReadingOrPlaying()
{
  return (s_state.drive_state == DriveState::Reading || s_state.drive_state == DriveState::Playing);
}

bool CDROM::CanReadMedia()
{
  return (s_state.drive_state != DriveState::ShellOpening && s_reader.HasMedia());
}

bool CDROM::InsertMedia(std::unique_ptr<CDImage> media, DiscRegion region, std::string_view serial,
                        std::string_view title, Error* error)
{
  // Load SBI/LSD first.
  std::unique_ptr<CDROMSubQReplacement> subq;
  if (!media->HasSubchannelData() && !CDROMSubQReplacement::LoadForImage(&subq, media.get(), serial, title, error))
    return false;

  if (CanReadMedia())
    RemoveMedia(true);

  INFO_LOG("Inserting new media, disc region: {}, console region: {}", Settings::GetDiscRegionName(region),
           Settings::GetConsoleRegionName(System::GetRegion()));

  s_state.subq_replacement = std::move(subq);
  s_state.disc_region = region;
  s_reader.SetMedia(std::move(media));
  SetHoldPosition(0, 0);

  // motor automatically spins up
  if (s_state.drive_state != DriveState::ShellOpening)
    StartMotor();

  if (s_state.show_current_file)
    CreateFileMap();

  return true;
}

std::unique_ptr<CDImage> CDROM::RemoveMedia(bool for_disc_swap)
{
  if (!HasMedia())
    return {};

  // Add an additional two seconds to the disc swap, some games don't like it happening too quickly.
  TickCount stop_ticks = GetTicksForStop(true);
  if (for_disc_swap)
    stop_ticks += System::ScaleTicksToOverclock(System::MASTER_CLOCK * 2);

  INFO_LOG("Removing CD...");
  std::unique_ptr<CDImage> image = s_reader.RemoveMedia();

  if (s_state.show_current_file)
    CreateFileMap();

  s_state.last_sector_header_valid = false;

  s_state.secondary_status.motor_on = false;
  s_state.secondary_status.shell_open = true;
  s_state.secondary_status.ClearActiveBits();
  s_state.disc_region = DiscRegion::NonPS1;
  s_state.subq_replacement.reset();

  // If the drive was doing anything, we need to abort the command.
  ClearDriveState();
  ClearCommandSecondResponse();
  s_state.command = Command::None;
  s_state.command_event.Deactivate();

  // The console sends an interrupt when the shell is opened regardless of whether a command was executing.
  ClearAsyncInterrupt();
  SendAsyncErrorResponse(STAT_ERROR, 0x08);

  // Begin spin-down timer, we can't swap the new disc in immediately for some games (e.g. Metal Gear Solid).
  if (for_disc_swap)
  {
    s_state.drive_state = DriveState::ShellOpening;
    s_state.drive_event.SetIntervalAndSchedule(stop_ticks);
  }

  return image;
}

bool CDROM::PrecacheMedia()
{
  if (!s_reader.HasMedia())
    return false;

  if (s_reader.GetMedia()->HasSubImages() && s_reader.GetMedia()->GetSubImageCount() > 1)
  {
    Host::AddOSDMessage(
      fmt::format(TRANSLATE_FS("OSDMessage", "CD image preloading not available for multi-disc image '{}'"),
                  FileSystem::GetDisplayNameFromPath(s_reader.GetMedia()->GetPath())),
      Host::OSD_ERROR_DURATION);
    return false;
  }

  LoadingScreenProgressCallback callback;
  if (!s_reader.Precache(&callback))
  {
    Host::AddOSDMessage(TRANSLATE_STR("OSDMessage", "Precaching CD image failed, it may be unreliable."),
                        Host::OSD_ERROR_DURATION);
    return false;
  }

  return true;
}

const CDImage::SubChannelQ& CDROM::GetSectorSubQ(u32 lba, const CDImage::SubChannelQ& real_subq)
{
  if (s_state.subq_replacement)
  {
    const CDImage::SubChannelQ* replacement_subq = s_state.subq_replacement->GetReplacementSubQ(lba);
    return replacement_subq ? *replacement_subq : real_subq;
  }

  return real_subq;
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
    s_reader.QueueReadSector(s_state.requested_lba);
}

void CDROM::CPUClockChanged()
{
  // reschedule the disc read event
  if (IsReadingOrPlaying())
    s_state.drive_event.SetInterval(GetTicksForRead());
}

u8 CDROM::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0: // status register
      TRACE_LOG("CDROM read status register -> 0x{:08X}", s_state.status.bits);
      return s_state.status.bits;

    case 1: // always response FIFO
    {
      if (s_state.response_fifo.IsEmpty())
      {
        DEV_LOG("Response FIFO empty on read");
        return 0x00;
      }

      const u8 value = s_state.response_fifo.Pop();
      UpdateStatusRegister();
      DEBUG_LOG("CDROM read response FIFO -> 0x{:08X}", ZeroExtend32(value));
      return value;
    }

    case 2: // always data FIFO
    {
      SectorBuffer& sb = s_state.sector_buffers[s_state.current_read_sector_buffer];
      u8 value = 0;
      if (s_state.request_register.BFRD && sb.position < sb.size)
      {
        value = (sb.position < sb.size) ? sb.data[sb.position++] : 0;
        CheckForSectorBufferReadComplete();
      }
      else
      {
        WARNING_LOG("Sector buffer overread (BDRD={}, buffer={}, pos={}, size={})",
                    s_state.request_register.BFRD.GetValue(), s_state.current_read_sector_buffer, sb.position, sb.size);
      }

      DEBUG_LOG("CDROM read data FIFO -> 0x{:02X}", value);
      return value;
    }

    case 3:
    {
      if (s_state.status.index & 1)
      {
        const u8 value = s_state.interrupt_flag_register | ~INTERRUPT_REGISTER_MASK;
        DEBUG_LOG("CDROM read interrupt flag register -> 0x{:02X}", value);
        return value;
      }
      else
      {
        const u8 value = s_state.interrupt_enable_register | ~INTERRUPT_REGISTER_MASK;
        DEBUG_LOG("CDROM read interrupt enable register -> 0x{:02X}", value);
        return value;
      }
    }
    break;

    default:
      [[unlikely]]
      {
        ERROR_LOG("Unknown CDROM register read: offset=0x{:02X}, index={}", offset,
                  ZeroExtend32(s_state.status.index.GetValue()));
        Panic("Unknown CDROM register");
      }
  }
}

void CDROM::WriteRegister(u32 offset, u8 value)
{
  if (offset == 0)
  {
    TRACE_LOG("CDROM status register <- 0x{:02X}", value);
    s_state.status.bits = (s_state.status.bits & static_cast<u8>(~3)) | (value & u8(3));
    return;
  }

  const u32 reg = (s_state.status.index * 3u) + (offset - 1);
  switch (reg)
  {
    case 0:
    {
      DEBUG_LOG("CDROM command register <- 0x{:02X} ({})", value, s_command_info[value].name);
      BeginCommand(static_cast<Command>(value));
      return;
    }

    case 1:
    {
      if (s_state.param_fifo.IsFull())
      {
        WARNING_LOG("Parameter FIFO overflow");
        s_state.param_fifo.RemoveOne();
      }

      s_state.param_fifo.Push(value);
      UpdateStatusRegister();
      return;
    }

    case 2:
    {
      DEBUG_LOG("Request register <- 0x{:02X}", value);
      const RequestRegister rr{value};

      // Sound map is not currently implemented, haven't found anything which uses it.
      if (rr.SMEN)
        ERROR_LOG("Sound map enable set");
      if (rr.BFWR)
        ERROR_LOG("Buffer write enable set");

      s_state.request_register.bits = rr.bits;

      SectorBuffer& sb = s_state.sector_buffers[s_state.current_read_sector_buffer];
      DEBUG_LOG("{} BFRD buffer={} pos={} size={}", s_state.request_register.BFRD ? "Set" : "Clear",
                s_state.current_read_sector_buffer, sb.position, sb.size);

      if (!s_state.request_register.BFRD)
      {
        // Clearing BFRD needs to reset the position of the current buffer.
        // Metal Gear Solid: Special Missions (PAL) clears BFRD inbetween two DMAs during its disc detection, and needs
        // the buffer to reset. But during the actual game, it doesn't clear, and needs the pointer to increment.
        sb.position = 0;
      }
      else
      {
        if (sb.size == 0)
          WARNING_LOG("Setting BFRD without a buffer ready.");
      }

      UpdateStatusRegister();
      return;
    }

    case 3:
    {
      ERROR_LOG("Sound map data out <- 0x{:02X}", value);
      return;
    }

    case 4:
    {
      DEBUG_LOG("Interrupt enable register <- 0x{:02X}", value);
      s_state.interrupt_enable_register = value & INTERRUPT_REGISTER_MASK;
      UpdateInterruptRequest();
      return;
    }

    case 5:
    {
      DEBUG_LOG("Interrupt flag register <- 0x{:02X}", value);

      const u8 prev_interrupt_flag_register = s_state.interrupt_flag_register;
      s_state.interrupt_flag_register &= ~(value & INTERRUPT_REGISTER_MASK);
      if (s_state.interrupt_flag_register == 0)
      {
        // Start the countdown from when the interrupt was cleared, not it being triggered.
        // Otherwise Ogre Battle, Crime Crackers, Lego Racers, etc have issues.
        if (prev_interrupt_flag_register != 0)
          s_state.last_interrupt_time = System::GetGlobalTickCounter();

        InterruptController::SetLineState(InterruptController::IRQ::CDROM, false);
        if (HasPendingAsyncInterrupt() && !HasPendingCommand())
          QueueDeliverAsyncInterrupt();
        else
          UpdateCommandEvent();
      }

      // Bit 6 clears the parameter FIFO.
      if (value & 0x40)
      {
        s_state.param_fifo.Clear();
        UpdateStatusRegister();
      }

      return;
    }

    case 6:
    {
      ERROR_LOG("Sound map coding info <- 0x{:02X}", value);
      return;
    }

    case 7:
    {
      DEBUG_LOG("Audio volume for left-to-left output <- 0x{:02X}", value);
      s_state.next_cd_audio_volume_matrix[0][0] = value;
      return;
    }

    case 8:
    {
      DEBUG_LOG("Audio volume for left-to-right output <- 0x{:02X}", value);
      s_state.next_cd_audio_volume_matrix[0][1] = value;
      return;
    }

    case 9:
    {
      DEBUG_LOG("Audio volume for right-to-right output <- 0x{:02X}", value);
      s_state.next_cd_audio_volume_matrix[1][1] = value;
      return;
    }

    case 10:
    {
      DEBUG_LOG("Audio volume for right-to-left output <- 0x{:02X}", value);
      s_state.next_cd_audio_volume_matrix[1][0] = value;
      return;
    }

    case 11:
    {
      DEBUG_LOG("Audio volume apply changes <- 0x{:02X}", value);

      const bool adpcm_muted = ConvertToBoolUnchecked(value & u8(0x01));
      if (adpcm_muted != s_state.adpcm_muted ||
          (value & 0x20 &&
           std::memcmp(s_state.cd_audio_volume_matrix.data(), s_state.next_cd_audio_volume_matrix.data(),
                       sizeof(s_state.cd_audio_volume_matrix)) != 0))
      {
        if (HasPendingDiscEvent())
          s_state.drive_event.InvokeEarly();
        SPU::GeneratePendingSamples();
      }

      s_state.adpcm_muted = adpcm_muted;
      if (value & 0x20)
        s_state.cd_audio_volume_matrix = s_state.next_cd_audio_volume_matrix;
      return;
    }

    default:
      [[unlikely]]
      {
        ERROR_LOG("Unknown CDROM register write: offset=0x{:02X}, index={}, reg={}, value=0x{:02X}", offset,
                  s_state.status.index.GetValue(), reg, value);
        return;
      }
  }
}

void CDROM::DMARead(u32* words, u32 word_count)
{
  SectorBuffer& sb = s_state.sector_buffers[s_state.current_read_sector_buffer];
  const u32 bytes_available = (s_state.request_register.BFRD && sb.position < sb.size) ? (sb.size - sb.position) : 0;
  u8* dst_ptr = reinterpret_cast<u8*>(words);
  u32 bytes_remaining = word_count * sizeof(u32);
  if (bytes_available > 0)
  {
    const u32 transfer_size = std::min(bytes_available, bytes_remaining);
    std::memcpy(dst_ptr, &sb.data[sb.position], transfer_size);
    sb.position += transfer_size;
    dst_ptr += transfer_size;
    bytes_remaining -= transfer_size;
  }

  if (bytes_remaining > 0)
  {
    ERROR_LOG("Sector buffer overread by {} bytes", bytes_remaining);
    std::memset(dst_ptr, 0, bytes_remaining);
  }

  CheckForSectorBufferReadComplete();
}

bool CDROM::HasPendingCommand()
{
  return s_state.command != Command::None;
}

bool CDROM::HasPendingInterrupt()
{
  return s_state.interrupt_flag_register != 0;
}

bool CDROM::HasPendingAsyncInterrupt()
{
  return s_state.pending_async_interrupt != 0;
}

void CDROM::SetInterrupt(Interrupt interrupt)
{
  s_state.interrupt_flag_register = static_cast<u8>(interrupt);
  UpdateInterruptRequest();
}

void CDROM::SetAsyncInterrupt(Interrupt interrupt)
{
  if (s_state.interrupt_flag_register == static_cast<u8>(interrupt))
  {
    DEV_LOG("Not setting async interrupt {} because there is already one unacknowledged", static_cast<u8>(interrupt));
    s_state.async_response_fifo.Clear();
    return;
  }

  Assert(s_state.pending_async_interrupt == 0);
  s_state.pending_async_interrupt = static_cast<u8>(interrupt);
  if (!HasPendingInterrupt())
  {
    // Pending interrupt should block INT1 from going through. But pending command needs to as well, for games like
    // Gokujou Parodius Da! Deluxe Pack that spam GetlocL while data is being played back, if they get an INT1 instead
    // of an INT3 during the small window of time that the INT3 is delayed, causes a lock-up.
    if (!HasPendingCommand())
      QueueDeliverAsyncInterrupt();
    else
      DEBUG_LOG("Delaying async interrupt {} because of pending command", s_state.pending_async_interrupt);
  }
  else
  {
    DEBUG_LOG("Delaying async interrupt {} because of pending interrupt {}", s_state.pending_async_interrupt,
              s_state.interrupt_flag_register);
  }
}

void CDROM::ClearAsyncInterrupt()
{
  s_state.pending_async_interrupt = 0;
  s_state.async_interrupt_event.Deactivate();
  s_state.async_response_fifo.Clear();
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
  DebugAssert(HasPendingAsyncInterrupt());

  const u32 diff = static_cast<u32>(System::GetGlobalTickCounter() - s_state.last_interrupt_time);
  if (diff >= MINIMUM_INTERRUPT_DELAY)
  {
    DeliverAsyncInterrupt(nullptr, 0, 0);
  }
  else
  {
    DEV_LOG("Delaying async interrupt {} because it's been {} cycles since last interrupt",
            s_state.pending_async_interrupt, diff);
    s_state.async_interrupt_event.Schedule(INTERRUPT_DELAY_CYCLES);
  }
}

void CDROM::DeliverAsyncInterrupt(void*, TickCount ticks, TickCount ticks_late)
{
  if (HasPendingInterrupt())
  {
    // This shouldn't really happen, because we should block command execution.. but just in case.
    if (!s_state.async_interrupt_event.IsActive())
      s_state.async_interrupt_event.Schedule(INTERRUPT_DELAY_CYCLES);
  }
  else
  {
    s_state.async_interrupt_event.Deactivate();

    Assert(s_state.pending_async_interrupt != 0 && !HasPendingInterrupt());
    DEBUG_LOG("Delivering async interrupt {}", s_state.pending_async_interrupt);

    // This is the HC05 setting the read position from the decoder.
    if (s_state.pending_async_interrupt == static_cast<u8>(Interrupt::DataReady))
      s_state.current_read_sector_buffer = s_state.current_write_sector_buffer;

    s_state.response_fifo.Clear();
    s_state.response_fifo.PushFromQueue(&s_state.async_response_fifo);
    s_state.interrupt_flag_register = s_state.pending_async_interrupt;
    s_state.pending_async_interrupt = 0;
    UpdateInterruptRequest();
    UpdateStatusRegister();
    UpdateCommandEvent();
  }
}

void CDROM::SendACKAndStat()
{
  s_state.response_fifo.Push(s_state.secondary_status.bits);
  SetInterrupt(Interrupt::ACK);
}

void CDROM::SendErrorResponse(u8 stat_bits /* = STAT_ERROR */, u8 reason /* = 0x80 */)
{
  s_state.response_fifo.Push(s_state.secondary_status.bits | stat_bits);
  s_state.response_fifo.Push(reason);
  SetInterrupt(Interrupt::Error);
}

void CDROM::SendAsyncErrorResponse(u8 stat_bits /* = STAT_ERROR */, u8 reason /* = 0x80 */)
{
  s_state.async_response_fifo.Push(s_state.secondary_status.bits | stat_bits);
  s_state.async_response_fifo.Push(reason);
  SetAsyncInterrupt(Interrupt::Error);
}

void CDROM::UpdateStatusRegister()
{
  s_state.status.ADPBUSY = false;
  s_state.status.PRMEMPTY = s_state.param_fifo.IsEmpty();
  s_state.status.PRMWRDY = !s_state.param_fifo.IsFull();
  s_state.status.RSLRRDY = !s_state.response_fifo.IsEmpty();
  s_state.status.DRQSTS = s_state.request_register.BFRD;
  s_state.status.BUSYSTS = HasPendingCommand();

  DMA::SetRequest(DMA::Channel::CDROM, s_state.status.DRQSTS);
}

void CDROM::UpdateInterruptRequest()
{
  InterruptController::SetLineState(InterruptController::IRQ::CDROM,
                                    (s_state.interrupt_flag_register & s_state.interrupt_enable_register) != 0);
}

bool CDROM::HasPendingDiscEvent()
{
  return (s_state.drive_event.IsActive() && s_state.drive_event.GetTicksUntilNextExecution() <= 0);
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
  if (s_state.drive_state == DriveState::SpinningUp)
    ticks += s_state.drive_event.GetTicksUntilNextExecution();

  return ticks;
}

TickCount CDROM::GetTicksForRead()
{
  const TickCount tps = System::GetTicksPerSecond();

  if (g_settings.cdrom_read_speedup > 1 && !s_state.mode.cdda && !s_state.mode.xa_enable && s_state.mode.double_speed)
    return tps / (150 * g_settings.cdrom_read_speedup);

  return s_state.mode.double_speed ? (tps / 150) : (tps / 75);
}

u32 CDROM::GetSectorsPerTrack(CDImage::LBA lba)
{
  using SPTTable = std::array<u8, 80>;

  static constexpr const SPTTable spt_table = []() constexpr -> SPTTable {
    // Based on mech behaviour, thanks rama for these numbers!
    // Note that minutes beyond 71 are buggy on the real mech, it uses the 71 minute table
    // regardless of the disc size. This matches the 71 minute table.
    SPTTable table = {};
    for (size_t mm = 0; mm < table.size(); mm++)
    {
      if (mm == 0) // 00 = 8
        table[mm] = 8;
      else if (mm <= 4) // 01-04 = 9
        table[mm] = 9;
      else if (mm <= 7) // 05-07 = 10
        table[mm] = 10;
      else if (mm <= 11) // 08-11 = 11
        table[mm] = 11;
      else if (mm <= 16) // 12-16 = 12
        table[mm] = 12;
      else if (mm <= 23) // 17-23 = 13
        table[mm] = 13;
      else if (mm <= 27) // 24-27 = 14
        table[mm] = 14;
      else if (mm <= 32) // 28-32 = 15
        table[mm] = 15;
      else if (mm <= 39) // 32-39 = 16
        table[mm] = 16;
      else if (mm <= 44) // 40-44 = 17
        table[mm] = 17;
      else if (mm <= 52) // 45-52 = 18
        table[mm] = 18;
      else if (mm <= 60) // 53-60 = 19
        table[mm] = 19;
      else if (mm <= 67) // 61-66 = 20
        table[mm] = 20;
      else if (mm <= 74) // 67-74 = 21
        table[mm] = 21;
      else // 75-80 = 22
        table[mm] = 22;
    }
    return table;
  }();

  const u32 mm = lba / CDImage::FRAMES_PER_MINUTE;
  return spt_table[std::min(mm, static_cast<u32>(spt_table.size()))];
}

TickCount CDROM::GetTicksForSeek(CDImage::LBA new_lba, bool ignore_speed_change)
{
  static constexpr TickCount MIN_TICKS = 30000;

  if (g_settings.cdrom_seek_speedup == 0)
    return MIN_TICKS;

  u32 ticks = 0;

  // Update start position for seek.
  if (IsSeeking())
    UpdateSubQPositionWhileSeeking();
  else
    UpdateSubQPosition(false);

  const CDImage::LBA current_lba = IsMotorOn() ? (IsSeeking() ? s_state.seek_end_lba : s_state.current_subq_lba) : 0;
  const CDImage::LBA lba_diff = ((new_lba > current_lba) ? (new_lba - current_lba) : (current_lba - new_lba));

  // Motor spin-up time.
  if (!IsMotorOn())
  {
    ticks += (s_state.drive_state == DriveState::SpinningUp) ? s_state.drive_event.GetTicksUntilNextExecution() :
                                                               GetTicksForSpinUp();
    if (s_state.drive_state == DriveState::ShellOpening || s_state.drive_state == DriveState::SpinningUp)
      ClearDriveState();
  }

  const TickCount ticks_per_sector =
    s_state.mode.double_speed ? (System::MASTER_CLOCK / 150) : (System::MASTER_CLOCK / 75);
  const CDImage::LBA sectors_per_track = GetSectorsPerTrack(current_lba);
  const CDImage::LBA tjump_position = (current_lba >= sectors_per_track) ? (current_lba - sectors_per_track) : 0;
  std::string_view seek_type;
  if (current_lba < new_lba && lba_diff <= sectors_per_track)
  {
    // If we're behind the current sector, and within a small distance, the mech just waits for the sector to come up
    // by reading normally. This timing is actually needed for Transformers - Beast Wars Transmetals, it gets very
    // unstable during loading if seeks are too fast.
    ticks += ticks_per_sector * std::max(lba_diff, 2u);
    seek_type = "forward";
  }
  else if (current_lba >= new_lba && tjump_position <= new_lba)
  {
    // Track jump back. We cap this at 8 sectors (~53ms), so it doesn't take longer than the medium seek below.
    ticks += ticks_per_sector * std::max(new_lba - tjump_position, 1u);
    seek_type = "1T back+forward";
  }
  else if (lba_diff < 7200)
  {
    // Not sled. The point at which we switch from faster to slower seeks varies across the disc. Around ~60 distance
    // towards the end, but ~330 at the beginning. Likely based on sectors per track, so we use a logarithmic curve.
    const u32 switch_point = static_cast<u32>(
      330.0f +
      (-63.1333f * std::log(std::clamp(static_cast<float>(current_lba) / static_cast<float>(CDImage::FRAMES_PER_MINUTE),
                                       1.0f, 72.0f))));
    const float seconds = (lba_diff < switch_point) ? 0.05f : 0.1f;
    ticks += static_cast<u32>(seconds * static_cast<float>(System::MASTER_CLOCK));
    seek_type = (new_lba > current_lba) ? "2N forward" : "2N backward";
  }
  else
  {
    // Sled seek. Minimum of approx. 200ms, up to 900ms or so. Mapped to a linear and logarithmic component, because
    // there is a fixed cost which ramps up quickly, but the very slow sled seeks are only when doing a full disc sweep.
    constexpr float SLED_FIXED_COST = 0.05f;
    constexpr float SLED_VARIABLE_COST = 0.9f - SLED_FIXED_COST;
    constexpr float LOG_WEIGHT = 0.4f;
    constexpr float MAX_SLED_LBA = static_cast<float>(72 * CDImage::FRAMES_PER_MINUTE);
    const float seconds =
      SLED_FIXED_COST +
      (((SLED_VARIABLE_COST * (std::log(static_cast<float>(lba_diff)) / std::log(MAX_SLED_LBA)))) * LOG_WEIGHT) +
      ((SLED_VARIABLE_COST * (lba_diff / MAX_SLED_LBA)) * (1.0f - LOG_WEIGHT));
    ticks += static_cast<u32>(seconds * static_cast<float>(System::MASTER_CLOCK));
    seek_type = (new_lba > current_lba) ? "sled forward" : "sled backward";
  }

  if (g_settings.cdrom_seek_speedup > 1)
    ticks = std::max<u32>(ticks / g_settings.cdrom_seek_speedup, MIN_TICKS);

  if (s_state.drive_state == DriveState::ChangingSpeedOrTOCRead && !ignore_speed_change)
  {
    // we're still reading the TOC, so add that time in
    const TickCount remaining_change_ticks = s_state.drive_event.GetTicksUntilNextExecution();
    ticks += remaining_change_ticks;

    DEV_LOG("Seek time for {}->{} ({} LBA): {} ({:.3f} ms) ({} for speed change/init) ({})",
            LBAToMSFString(current_lba), LBAToMSFString(new_lba), lba_diff, ticks,
            (static_cast<float>(ticks) / static_cast<float>(System::MASTER_CLOCK)) * 1000.0f, remaining_change_ticks,
            seek_type);
  }
  else
  {
    DEV_LOG("Seek time for {}->{} ({} LBA): {} ({:.3f} ms) ({})", LBAToMSFString(current_lba), LBAToMSFString(new_lba),
            lba_diff, ticks, (static_cast<float>(ticks) / static_cast<float>(System::MASTER_CLOCK)) * 1000.0f,
            seek_type);
  }

  return System::ScaleTicksToOverclock(static_cast<TickCount>(ticks));
}

TickCount CDROM::GetTicksForPause()
{
  if (!IsReadingOrPlaying())
    return 7000;

  const u32 sectors_per_track = GetSectorsPerTrack(s_state.current_lba);
  const TickCount ticks_per_read = GetTicksForRead();

  // Jump backwards one track, then the time to reach the target again.
  // Subtract another 2 in data mode, because holding is based on subq, not data.
  const TickCount ticks_to_reach_target =
    (static_cast<TickCount>(sectors_per_track - (IsReading() ? 2 : 0)) * ticks_per_read) -
    s_state.drive_event.GetTicksSinceLastExecution();

  // Clamp to a minimum time of 4 sectors or so, because otherwise read speedup is going to break things...
  const TickCount min_ticks = (s_state.mode.double_speed ? 1000000 : 2000000);
  return std::max(ticks_to_reach_target, min_ticks);
}

TickCount CDROM::GetTicksForStop(bool motor_was_on)
{
  return System::ScaleTicksToOverclock(motor_was_on ? (s_state.mode.double_speed ? 25000000 : 13000000) : 7000);
}

TickCount CDROM::GetTicksForSpeedChange()
{
  static constexpr u32 ticks_single_to_double = static_cast<u32>(0.6 * static_cast<double>(System::MASTER_CLOCK));
  static constexpr u32 ticks_double_to_single = static_cast<u32>(0.7 * static_cast<double>(System::MASTER_CLOCK));
  return System::ScaleTicksToOverclock(s_state.mode.double_speed ? ticks_single_to_double : ticks_double_to_single);
}

TickCount CDROM::GetTicksForTOCRead()
{
  if (!HasMedia())
    return 0;

  return System::GetTicksPerSecond() / 2u;
}

CDImage::LBA CDROM::GetNextSectorToBeRead()
{
  if (!IsReadingOrPlaying() && !IsSeeking())
    return s_state.current_lba;

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
    if (s_command_info[static_cast<u8>(s_state.command)].min_parameters >
        s_command_info[static_cast<u8>(command)].min_parameters)
    {
      WARNING_LOG("Ignoring command 0x{:02X} ({}) and emptying FIFO as 0x{:02X} ({}) is still pending",
                  static_cast<u8>(command), s_command_info[static_cast<u8>(command)].name,
                  static_cast<u8>(s_state.command), s_command_info[static_cast<u8>(s_state.command)].name);
      s_state.param_fifo.Clear();
      return;
    }

    WARNING_LOG("Cancelling pending command 0x{:02X} ({}) for new command 0x{:02X} ({})",
                static_cast<u8>(s_state.command), s_command_info[static_cast<u8>(s_state.command)].name,
                static_cast<u8>(command), s_command_info[static_cast<u8>(command)].name);

    // subtract the currently-elapsed ack ticks from the new command
    if (s_state.command_event.IsActive())
    {
      const TickCount elapsed_ticks =
        s_state.command_event.GetInterval() - s_state.command_event.GetTicksUntilNextExecution();
      ack_delay = std::max(ack_delay - elapsed_ticks, 1);
      s_state.command_event.Deactivate();

      // If there's a pending async interrupt, we need to deliver it now, since we've deactivated the command that was
      // blocking it from being delivered. Not doing so will cause lockups in Street Fighter Alpha 3, where it spams
      // multiple pause commands while an INT1 is scheduled, and there isn't much that can stop an INT1 once it's been
      // queued on real hardware.
      if (HasPendingAsyncInterrupt())
      {
        WARNING_LOG("Delivering pending interrupt after command {} cancellation for {}.",
                    s_command_info[static_cast<u8>(s_state.command)].name,
                    s_command_info[static_cast<u8>(command)].name);
        QueueDeliverAsyncInterrupt();
      }
    }
  }

  s_state.command = command;
  s_state.command_event.SetIntervalAndSchedule(ack_delay);
  UpdateCommandEvent();
  UpdateStatusRegister();
}

void CDROM::EndCommand()
{
  s_state.param_fifo.Clear();

  s_state.command = Command::None;
  s_state.command_event.Deactivate();
  UpdateStatusRegister();
}

void CDROM::ExecuteCommand(void*, TickCount ticks, TickCount ticks_late)
{
  const CommandInfo& ci = s_command_info[static_cast<u8>(s_state.command)];
  if (s_state.param_fifo.GetSize() < ci.min_parameters || s_state.param_fifo.GetSize() > ci.max_parameters) [[unlikely]]
  {
    WARNING_LOG("Incorrect parameters for command 0x{:02X} ({}), expecting {}-{} got {}",
                static_cast<u8>(s_state.command), ci.name, ci.min_parameters, ci.max_parameters,
                s_state.param_fifo.GetSize());
    SendErrorResponse(STAT_ERROR, ERROR_REASON_INCORRECT_NUMBER_OF_PARAMETERS);
    EndCommand();
    return;
  }

  if (!s_state.response_fifo.IsEmpty())
  {
    DEBUG_LOG("Response FIFO not empty on command begin");
    s_state.response_fifo.Clear();
  }

  // Stop command event first, reduces our chances of ending up with out-of-order events.
  s_state.command_event.Deactivate();

  switch (s_state.command)
  {
    case Command::Getstat:
    {
      DEV_COLOR_LOG(StrongOrange, "Getstat    Stat=0x{:02X}", s_state.secondary_status.bits);

      // if bit 0 or 2 is set, send an additional byte
      SendACKAndStat();

      // shell open bit is cleared after sending the status
      if (CanReadMedia())
        s_state.secondary_status.shell_open = false;

      EndCommand();
      return;
    }

    case Command::Test:
    {
      const u8 subcommand = s_state.param_fifo.Pop();
      ExecuteTestCommand(subcommand);
      return;
    }

    case Command::GetID:
    {
      DEV_COLOR_LOG(StrongOrange, "GetID");
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
      DEV_COLOR_LOG(StrongOrange, "ReadTOC");
      ClearCommandSecondResponse();

      if (!CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        SendACKAndStat();
        SetHoldPosition(0, 0);
        QueueCommandSecondResponse(Command::ReadTOC, GetTicksForTOCRead());
      }

      EndCommand();
      return;
    }

    case Command::Setfilter:
    {
      const u8 file = s_state.param_fifo.Peek(0);
      const u8 channel = s_state.param_fifo.Peek(1);
      DEV_COLOR_LOG(StrongOrange, "Setfilter  File=0x{:02X} Channel=0x{:02X}", ZeroExtend32(file),
                    ZeroExtend32(channel));
      s_state.xa_filter_file_number = file;
      s_state.xa_filter_channel_number = channel;
      s_state.xa_current_set = false;
      SendACKAndStat();
      EndCommand();
      return;
    }

    case Command::Setmode:
    {
      const u8 mode = s_state.param_fifo.Peek(0);
      const bool speed_change = (mode & 0x80) != (s_state.mode.bits & 0x80);
      DEV_COLOR_LOG(StrongOrange, "Setmode    0x{:02X}", ZeroExtend32(mode));

      s_state.mode.bits = mode;
      SendACKAndStat();
      EndCommand();

      if (speed_change)
      {
        if (s_state.drive_state == DriveState::ChangingSpeedOrTOCRead)
        {
          // cancel the speed change if it's less than a quarter complete
          if (s_state.drive_event.GetTicksUntilNextExecution() >= (GetTicksForSpeedChange() / 4))
          {
            DEV_LOG("Cancelling speed change event");
            ClearDriveState();
          }
        }
        else if (s_state.drive_state != DriveState::SeekingImplicit && s_state.drive_state != DriveState::ShellOpening)
        {
          // if we're seeking or reading, we need to add time to the current seek/read
          const TickCount change_ticks = GetTicksForSpeedChange();
          if (s_state.drive_state != DriveState::Idle)
          {
            DEV_LOG("Drive is {}, delaying event by {} ticks for speed change to {}-speed",
                    s_drive_state_names[static_cast<u8>(s_state.drive_state)], change_ticks,
                    s_state.mode.double_speed ? "double" : "single");
            s_state.drive_event.Delay(change_ticks);

            if (IsReadingOrPlaying())
            {
              WARNING_LOG("Speed change while reading/playing, reads will be temporarily delayed.");
              s_state.drive_event.SetInterval(GetTicksForRead());
            }
          }
          else
          {
            DEV_LOG("Drive is idle, speed change takes {} ticks", change_ticks);
            s_state.drive_state = DriveState::ChangingSpeedOrTOCRead;
            s_state.drive_event.Schedule(change_ticks);
          }
        }
      }

      return;
    }

    case Command::Setloc:
    {
      const u8 mm = s_state.param_fifo.Peek(0);
      const u8 ss = s_state.param_fifo.Peek(1);
      const u8 ff = s_state.param_fifo.Peek(2);
      DEV_COLOR_LOG(StrongOrange, "Setloc     {:02X}:{:02X}:{:02X}", mm, ss, ff);

      // MM must be BCD, SS must be BCD and <0x60, FF must be BCD and <0x75
      if (((mm & 0x0F) > 0x09) || (mm > 0x99) || ((ss & 0x0F) > 0x09) || (ss >= 0x60) || ((ff & 0x0F) > 0x09) ||
          (ff >= 0x75))
      {
        ERROR_LOG("Invalid/out of range seek to {:02X}:{:02X}:{:02X}", mm, ss, ff);
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_ARGUMENT);
      }
      else
      {
        SendACKAndStat();

        s_state.setloc_position.minute = PackedBCDToBinary(mm);
        s_state.setloc_position.second = PackedBCDToBinary(ss);
        s_state.setloc_position.frame = PackedBCDToBinary(ff);
        s_state.setloc_pending = true;
      }

      EndCommand();
      return;
    }

    case Command::SeekL:
    case Command::SeekP:
    {
      const bool logical = (s_state.command == Command::SeekL);
      DEV_COLOR_LOG(StrongOrange, "{}      {:02d}:{:02d}:{:02d}", logical ? "SeekL" : "SeekP",
                    s_state.setloc_position.minute, s_state.setloc_position.second, s_state.setloc_position.frame);

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
      const u8 session = s_state.param_fifo.Peek(0);
      DEV_COLOR_LOG(StrongOrange, "ReadT      Session={}", session);

      if (!CanReadMedia() || s_state.drive_state == DriveState::Reading || s_state.drive_state == DriveState::Playing)
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

        s_state.async_command_parameter = session;
        s_state.drive_state = DriveState::ChangingSession;
        s_state.drive_event.Schedule(GetTicksForTOCRead());
      }

      EndCommand();
      return;
    }

    case Command::ReadN:
    case Command::ReadS:
    {
      DEV_COLOR_LOG(StrongOrange, "{}      {:02d}:{:02d}:{:02d}",
                    (s_state.command == Command::ReadN) ? "ReadN" : "ReadS", s_state.setloc_position.minute,
                    s_state.setloc_position.second, s_state.setloc_position.frame);
      if (!CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else if ((IsMediaAudioCD() || !DoesMediaRegionMatchConsole()) && !s_state.mode.cdda)
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_COMMAND);
      }
      else
      {
        SendACKAndStat();

        if ((!s_state.setloc_pending || s_state.setloc_position.ToLBA() == GetNextSectorToBeRead()) &&
            (s_state.drive_state == DriveState::Reading || (IsSeeking() && s_state.read_after_seek)))
        {
          DEV_LOG("Ignoring read command with {} setloc, already reading/reading after seek",
                  s_state.setloc_pending ? "pending" : "same");
          s_state.setloc_pending = false;
        }
        else
        {
          BeginReading();
        }
      }

      EndCommand();
      return;
    }

    case Command::Play:
    {
      const u8 track = s_state.param_fifo.IsEmpty() ? 0 : PackedBCDToBinary(s_state.param_fifo.Peek(0));
      DEV_COLOR_LOG(StrongOrange, "Play       Track={}", track);

      if (!CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        SendACKAndStat();

        if (track == 0 && (!s_state.setloc_pending || s_state.setloc_position.ToLBA() == GetNextSectorToBeRead()) &&
            (s_state.drive_state == DriveState::Playing || (IsSeeking() && s_state.play_after_seek)))
        {
          DEV_LOG("Ignoring play command with no/same setloc, already playing/playing after seek");
          s_state.fast_forward_rate = 0;
          s_state.setloc_pending = false;
        }
        else
        {
          BeginPlaying(track);
        }
      }

      EndCommand();
      return;
    }

    case Command::Forward:
    {
      DEV_COLOR_LOG(StrongOrange, "Forward");
      if (s_state.drive_state != DriveState::Playing || !CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        SendACKAndStat();

        if (s_state.fast_forward_rate < 0)
          s_state.fast_forward_rate = 0;

        s_state.fast_forward_rate += static_cast<s8>(FAST_FORWARD_RATE_STEP);
        s_state.fast_forward_rate = std::min<s8>(s_state.fast_forward_rate, static_cast<s8>(MAX_FAST_FORWARD_RATE));
      }

      EndCommand();
      return;
    }

    case Command::Backward:
    {
      DEV_COLOR_LOG(StrongOrange, "Backward");
      if (s_state.drive_state != DriveState::Playing || !CanReadMedia())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        SendACKAndStat();

        if (s_state.fast_forward_rate > 0)
          s_state.fast_forward_rate = 0;

        s_state.fast_forward_rate -= static_cast<s8>(FAST_FORWARD_RATE_STEP);
        s_state.fast_forward_rate = std::max<s8>(s_state.fast_forward_rate, -static_cast<s8>(MAX_FAST_FORWARD_RATE));
      }

      EndCommand();
      return;
    }

    case Command::Pause:
    {
      const TickCount pause_time = GetTicksForPause();
      if (IsReading() && s_state.last_subq.IsData())
      {
        // Hit target, immediately jump back in data mode.
        const u32 spt = GetSectorsPerTrack(s_state.current_lba);
        SetHoldPosition(s_state.current_lba, (spt <= s_state.current_lba) ? (s_state.current_lba - spt) : 0);
      }

      ClearCommandSecondResponse();
      SendACKAndStat();

      // This behaviour has been verified with hardware tests! The mech will reject pause commands if the game
      // just started a read/seek, and it hasn't processed the first sector yet. This makes some games go bananas
      // and spam pause commands until eventually it succeeds, but it is correct behaviour.
      if (s_state.drive_state == DriveState::SeekingLogical || s_state.drive_state == DriveState::SeekingPhysical ||
          ((s_state.drive_state == DriveState::Reading || s_state.drive_state == DriveState::Playing) &&
           s_state.secondary_status.seeking))
      {
        if (Log::GetLogLevel() >= Log::Level::Dev)
          DEV_COLOR_LOG(StrongRed, "Pause      Seeking => Error");
        else
          WARNING_LOG("CDROM Pause command while seeking - sending error response");

        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
        EndCommand();
        return;
      }

      if (Log::GetLogLevel() >= Log::Level::Dev)
      {
        const double pause_time_ms =
          static_cast<double>(pause_time) / (static_cast<double>(System::MASTER_CLOCK) / 1000.0);
        if (IsReadingOrPlaying())
          DEV_COLOR_LOG(StrongOrange, "Pause                  {:.2f}ms", pause_time_ms);
        else
          DEV_COLOR_LOG(Yellow, "Pause      Not Reading {:.2f}ms", pause_time_ms);
      }

      // Small window of time when another INT1 could sneak in, don't let it.
      ClearAsyncInterrupt();

      // Stop reading.
      s_state.drive_state = DriveState::Idle;
      s_state.drive_event.Deactivate();
      s_state.secondary_status.ClearActiveBits();

      // Reset audio buffer here - control room cutscene audio repeats in Dino Crisis otherwise.
      ResetAudioDecoder();

      QueueCommandSecondResponse(Command::Pause, pause_time);

      EndCommand();
      return;
    }

    case Command::Stop:
    {
      DEV_COLOR_LOG(StrongOrange, "Stop");

      const TickCount stop_time = GetTicksForStop(IsMotorOn());
      ClearAsyncInterrupt();
      ClearCommandSecondResponse();
      SendACKAndStat();

      StopMotor();
      QueueCommandSecondResponse(Command::Stop, stop_time);

      EndCommand();
      return;
    }

    case Command::Init:
    {
      if (s_state.command_second_response == Command::Init)
      {
        // still pending
        DEV_COLOR_LOG(StrongRed, "Init");
        EndCommand();
        return;
      }

      DEV_COLOR_LOG(StrongOrange, "Init");
      SendACKAndStat();

      const TickCount reset_ticks = SoftReset(ticks_late);
      QueueCommandSecondResponse(Command::Init, reset_ticks);
      EndCommand();
      return;
    }

    case Command::MotorOn:
    {
      DEV_COLOR_LOG(StrongOrange, "MotorOn");
      if (IsMotorOn())
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INCORRECT_NUMBER_OF_PARAMETERS);
      }
      else
      {
        SendACKAndStat();

        // still pending?
        if (s_state.command_second_response == Command::MotorOn)
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

    case Command::Mute:
    {
      DEV_COLOR_LOG(StrongOrange, "Mute");
      s_state.muted = true;
      SendACKAndStat();
      EndCommand();
      return;
    }

    case Command::Demute:
    {
      DEV_COLOR_LOG(StrongOrange, "Demute");
      s_state.muted = false;
      SendACKAndStat();
      EndCommand();
      return;
    }

    case Command::GetlocL:
    {
      if (!s_state.last_sector_header_valid)
      {
        DEV_COLOR_LOG(StrongRed, "GetlocL    Header invalid, status 0x{:02X}", s_state.secondary_status.bits);
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        UpdateSubQPosition(true);

        DEV_COLOR_LOG(StrongOrange, "GetlocL    {:02X}:{:02X}:{:02X}", s_state.last_sector_header.minute,
                      s_state.last_sector_header.second, s_state.last_sector_header.frame);

        s_state.response_fifo.PushRange(reinterpret_cast<const u8*>(&s_state.last_sector_header),
                                        sizeof(s_state.last_sector_header));
        s_state.response_fifo.PushRange(reinterpret_cast<const u8*>(&s_state.last_sector_subheader),
                                        sizeof(s_state.last_sector_subheader));
        SetInterrupt(Interrupt::ACK);
      }

      EndCommand();
      return;
    }

    case Command::GetlocP:
    {
      if (!CanReadMedia())
      {
        DEV_COLOR_LOG(StrongOrange, "GetlocP    Not Ready");
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }
      else
      {
        if (IsSeeking())
          UpdateSubQPositionWhileSeeking();
        else
          UpdateSubQPosition(false);

        EnsureLastSubQValid();

        DEV_COLOR_LOG(StrongOrange, "GetlocP    T{:02x} I{:02x} R[{:02x}:{:02x}:{:02x}] A[{:02x}:{:02x}:{:02x}]",
                      s_state.last_subq.track_number_bcd, s_state.last_subq.index_number_bcd,
                      s_state.last_subq.relative_minute_bcd, s_state.last_subq.relative_second_bcd,
                      s_state.last_subq.relative_frame_bcd, s_state.last_subq.absolute_minute_bcd,
                      s_state.last_subq.absolute_second_bcd, s_state.last_subq.absolute_frame_bcd);

        s_state.response_fifo.Push(s_state.last_subq.track_number_bcd);
        s_state.response_fifo.Push(s_state.last_subq.index_number_bcd);
        s_state.response_fifo.Push(s_state.last_subq.relative_minute_bcd);
        s_state.response_fifo.Push(s_state.last_subq.relative_second_bcd);
        s_state.response_fifo.Push(s_state.last_subq.relative_frame_bcd);
        s_state.response_fifo.Push(s_state.last_subq.absolute_minute_bcd);
        s_state.response_fifo.Push(s_state.last_subq.absolute_second_bcd);
        s_state.response_fifo.Push(s_state.last_subq.absolute_frame_bcd);
        SetInterrupt(Interrupt::ACK);
      }

      EndCommand();
      return;
    }

    case Command::GetTN:
    {
      if (CanReadMedia())
      {
        DEV_COLOR_LOG(StrongRed, "GetTN      {}, {}", s_reader.GetMedia()->GetFirstTrackNumber(),
                      s_reader.GetMedia()->GetLastTrackNumber());

        s_state.response_fifo.Push(s_state.secondary_status.bits);
        s_state.response_fifo.Push(BinaryToBCD(Truncate8(s_reader.GetMedia()->GetFirstTrackNumber())));
        s_state.response_fifo.Push(BinaryToBCD(Truncate8(s_reader.GetMedia()->GetLastTrackNumber())));
        SetInterrupt(Interrupt::ACK);
      }
      else
      {
        DEV_COLOR_LOG(StrongRed, "GetTN      Not Ready");
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
      }

      EndCommand();
      return;
    }

    case Command::GetTD:
    {
      Assert(s_state.param_fifo.GetSize() >= 1);

      if (!CanReadMedia())
      {
        DEV_COLOR_LOG(StrongRed, "GetTD      Not Ready");
        SendErrorResponse(STAT_ERROR, ERROR_REASON_NOT_READY);
        EndCommand();
        return;
      }

      const u8 track_bcd = s_state.param_fifo.Peek();
      if (!IsValidPackedBCD(track_bcd))
      {
        DEV_COLOR_LOG(StrongRed, "GetTD      Invalid Track {:02X}", track_bcd);
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_ARGUMENT);
        EndCommand();
        return;
      }

      const u8 track = PackedBCDToBinary(track_bcd);
      if (track > s_reader.GetMedia()->GetTrackCount())
      {
        DEV_COLOR_LOG(StrongRed, "GetTD      Out-of-range Track {:02d}", track);
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_ARGUMENT);
      }
      else
      {
        CDImage::Position pos;
        if (track == 0)
          pos = CDImage::Position::FromLBA(s_reader.GetMedia()->GetLBACount());
        else
          pos = s_reader.GetMedia()->GetTrackStartMSFPosition(track);

        s_state.response_fifo.Push(s_state.secondary_status.bits);
        s_state.response_fifo.Push(BinaryToBCD(Truncate8(pos.minute)));
        s_state.response_fifo.Push(BinaryToBCD(Truncate8(pos.second)));
        DEV_COLOR_LOG(StrongRed, "GetTD      Track {:02d}: {:02d}:{:02d}", track, pos.minute, pos.second);

        SetInterrupt(Interrupt::ACK);
      }

      EndCommand();
      return;
    }

    case Command::Getmode:
    {
      DEV_COLOR_LOG(StrongRed, "Getmode      {:02X} {:02X} {:02X} {:02X}", s_state.secondary_status.bits,
                    s_state.mode.bits, s_state.xa_filter_file_number, s_state.xa_filter_channel_number);

      s_state.response_fifo.Push(s_state.secondary_status.bits);
      s_state.response_fifo.Push(s_state.mode.bits);
      s_state.response_fifo.Push(0);
      s_state.response_fifo.Push(s_state.xa_filter_file_number);
      s_state.response_fifo.Push(s_state.xa_filter_channel_number);
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case Command::Sync:
    {
      ERROR_LOG("Invalid sync command");

      SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_COMMAND);
      EndCommand();
      return;
    }

    case Command::VideoCD:
    {
      ERROR_LOG("Invalid VideoCD command");
      SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_COMMAND);

      // According to nocash this doesn't clear the parameter FIFO.
      s_state.command = Command::None;
      s_state.command_event.Deactivate();
      UpdateStatusRegister();
      return;
    }

    default:
      [[unlikely]]
      {
        ERROR_LOG("Unknown CDROM command 0x{:04X} with {} parameters, please report", static_cast<u16>(s_state.command),
                  s_state.param_fifo.GetSize());
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_COMMAND);
        EndCommand();
        return;
      }
  }
}

void CDROM::ExecuteTestCommand(u8 subcommand)
{
  switch (subcommand)
  {
    case 0x04: // Reset SCEx counters
    {
      DEBUG_LOG("Reset SCEx counters");
      s_state.secondary_status.motor_on = true;
      s_state.response_fifo.Push(s_state.secondary_status.bits);
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case 0x05: // Read SCEx counters
    {
      DEBUG_LOG("Read SCEx counters");
      s_state.response_fifo.Push(s_state.secondary_status.bits);
      s_state.response_fifo.Push(0); // # of TOC reads?
      s_state.response_fifo.Push(0); // # of SCEx strings received
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case 0x20: // Get CDROM BIOS Date/Version
    {
      DEBUG_LOG("Get CDROM BIOS Date/Version");

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

      s_state.response_fifo.PushRange(version_table[static_cast<u8>(g_settings.cdrom_mechacon_version)],
                                      countof(version_table[0]));
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case 0x22:
    {
      DEBUG_LOG("Get CDROM region ID string");

      switch (System::GetRegion())
      {
        case ConsoleRegion::NTSC_J:
        {
          static constexpr u8 response[] = {'f', 'o', 'r', ' ', 'J', 'a', 'p', 'a', 'n'};
          s_state.response_fifo.PushRange(response, countof(response));
        }
        break;

        case ConsoleRegion::PAL:
        {
          static constexpr u8 response[] = {'f', 'o', 'r', ' ', 'E', 'u', 'r', 'o', 'p', 'e'};
          s_state.response_fifo.PushRange(response, countof(response));
        }
        break;

        case ConsoleRegion::NTSC_U:
        default:
        {
          static constexpr u8 response[] = {'f', 'o', 'r', ' ', 'U', '/', 'C'};
          s_state.response_fifo.PushRange(response, countof(response));
        }
        break;
      }

      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    case 0x60:
    {
      if (s_state.param_fifo.GetSize() < 2) [[unlikely]]
      {
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INCORRECT_NUMBER_OF_PARAMETERS);
        EndCommand();
        return;
      }

      const u16 addr = ZeroExtend16(s_state.param_fifo.Peek(0)) | ZeroExtend16(s_state.param_fifo.Peek(1));
      WARNING_LOG("Read memory from 0x{:04X}, returning zero", addr);
      s_state.response_fifo.Push(0x00); // NOTE: No STAT here.
      SetInterrupt(Interrupt::ACK);
      EndCommand();
      return;
    }

    default:
      [[unlikely]]
      {
        ERROR_LOG("Unknown test command 0x{:02X}, {} parameters", subcommand, s_state.param_fifo.GetSize());
        SendErrorResponse(STAT_ERROR, ERROR_REASON_INVALID_COMMAND);
        EndCommand();
        return;
      }
  }
}

void CDROM::ExecuteCommandSecondResponse(void*, TickCount ticks, TickCount ticks_late)
{
  switch (s_state.command_second_response)
  {
    case Command::GetID:
      DoIDRead();
      break;

    case Command::Init:
    {
      // OpenBIOS spams Init, so we need to ensure the completion actually gets through.
      // If we have a pending command (which is probably init), cancel it.
      if (HasPendingCommand())
      {
        WARNING_LOG("Cancelling pending command 0x{:02X} ({}) due to init completion.",
                    static_cast<u8>(s_state.command), s_command_info[static_cast<u8>(s_state.command)].name);
        EndCommand();
      }
    }
      [[fallthrough]];

    case Command::ReadTOC:
    case Command::Pause:
    case Command::MotorOn:
    case Command::Stop:
      DoStatSecondResponse();
      break;

    default:
      break;
  }

  s_state.command_second_response = Command::None;
  s_state.command_second_response_event.Deactivate();
}

void CDROM::QueueCommandSecondResponse(Command command, TickCount ticks)
{
  ClearCommandSecondResponse();
  s_state.command_second_response = command;
  s_state.command_second_response_event.Schedule(ticks);
}

void CDROM::ClearCommandSecondResponse()
{
  if (s_state.command_second_response != Command::None)
  {
    DEV_LOG("Cancelling pending command 0x{:02X} ({}) second response",
            static_cast<u16>(s_state.command_second_response),
            s_command_info[static_cast<u16>(s_state.command_second_response)].name);
  }

  s_state.command_second_response_event.Deactivate();
  s_state.command_second_response = Command::None;
}

void CDROM::UpdateCommandEvent()
{
  // if there's a pending interrupt, we can't execute the command yet
  // so deactivate it until the interrupt is acknowledged
  if (!HasPendingCommand() || HasPendingInterrupt() || HasPendingAsyncInterrupt())
  {
    s_state.command_event.Deactivate();
    return;
  }
  else if (HasPendingCommand())
  {
    s_state.command_event.Activate();
  }
}

void CDROM::ExecuteDrive(void*, TickCount ticks, TickCount ticks_late)
{
  switch (s_state.drive_state)
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
      s_state.secondary_status.ClearActiveBits();
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
  s_state.drive_state = DriveState::Idle;
  s_state.drive_event.Deactivate();
}

void CDROM::BeginReading(TickCount ticks_late /* = 0 */, bool after_seek /* = false */)
{
  if (!after_seek && s_state.setloc_pending)
  {
    BeginSeeking(true, true, false);
    return;
  }

  // If we were seeking, we want to start reading from the seek target, not the current sector
  // Fixes crash in Disney's The Lion King - Simba's Mighty Adventure.
  if (IsSeeking())
  {
    DEV_LOG("Read command while seeking, scheduling read after seek {} -> {} finishes in {} ticks",
            s_state.seek_start_lba, s_state.seek_end_lba, s_state.drive_event.GetTicksUntilNextExecution());

    // Implicit seeks won't trigger the read, so swap it for a logical.
    if (s_state.drive_state == DriveState::SeekingImplicit)
      s_state.drive_state = DriveState::SeekingLogical;

    s_state.read_after_seek = true;
    s_state.play_after_seek = false;
    return;
  }

  DEBUG_LOG("Starting reading @ LBA {}", s_state.current_lba);

  const TickCount ticks = GetTicksForRead();
  const TickCount first_sector_ticks = ticks + (after_seek ? 0 : GetTicksForSeek(s_state.current_lba)) - ticks_late;

  ClearCommandSecondResponse();
  ClearAsyncInterrupt();
  ClearSectorBuffers();
  ResetAudioDecoder();

  // Even though this isn't "officially" a seek, we still need to jump back to the target sector unless we're
  // immediately following a seek from Play/Read. The seeking bit will get cleared after the first sector is processed.
  if (!after_seek)
    s_state.secondary_status.SetSeeking();

  s_state.drive_state = DriveState::Reading;
  s_state.drive_event.SetInterval(ticks);
  s_state.drive_event.Schedule(first_sector_ticks);

  s_state.requested_lba = s_state.current_lba;
  s_reader.QueueReadSector(s_state.requested_lba);
}

void CDROM::BeginPlaying(u8 track, TickCount ticks_late /* = 0 */, bool after_seek /* = false */)
{
  DEBUG_LOG("Starting playing CDDA track {}", track);
  s_state.play_track_number_bcd = track;
  s_state.fast_forward_rate = 0;

  // if track zero, start from current position
  if (track != 0)
  {
    // play specific track?
    if (track > s_reader.GetMedia()->GetTrackCount())
    {
      // restart current track
      track = Truncate8(s_reader.GetMedia()->GetTrackNumber());
    }

    s_state.setloc_position = s_reader.GetMedia()->GetTrackStartMSFPosition(track);
    s_state.setloc_pending = true;
  }

  if (s_state.setloc_pending)
  {
    BeginSeeking(false, false, true);
    return;
  }

  const TickCount ticks = GetTicksForRead();
  const TickCount first_sector_ticks =
    ticks + (after_seek ? 0 : GetTicksForSeek(s_state.current_lba, true)) - ticks_late;

  ClearCommandSecondResponse();
  ClearAsyncInterrupt();
  ClearSectorBuffers();
  ResetAudioDecoder();

  s_state.cdda_report_start_delay = CDDA_REPORT_START_DELAY;
  s_state.last_cdda_report_frame_nibble = 0xFF;

  s_state.drive_state = DriveState::Playing;
  s_state.drive_event.SetInterval(ticks);
  s_state.drive_event.Schedule(first_sector_ticks);

  s_state.requested_lba = s_state.current_lba;
  s_reader.QueueReadSector(s_state.requested_lba);
}

void CDROM::BeginSeeking(bool logical, bool read_after_seek, bool play_after_seek)
{
  if (!s_state.setloc_pending)
    WARNING_LOG("Seeking without setloc set");

  s_state.read_after_seek = read_after_seek;
  s_state.play_after_seek = play_after_seek;

  // TODO: Pending should stay set on seek command.
  s_state.setloc_pending = false;

  DEBUG_LOG("Seeking to [{:02d}:{:02d}:{:02d}] (LBA {}) ({})", s_state.setloc_position.minute,
            s_state.setloc_position.second, s_state.setloc_position.frame, s_state.setloc_position.ToLBA(),
            logical ? "logical" : "physical");

  const CDImage::LBA seek_lba = s_state.setloc_position.ToLBA();
  const TickCount seek_time = GetTicksForSeek(seek_lba, play_after_seek);

  ClearCommandSecondResponse();
  ClearAsyncInterrupt();
  ClearSectorBuffers();
  ResetAudioDecoder();

  s_state.secondary_status.SetSeeking();
  s_state.last_sector_header_valid = false;

  s_state.drive_state = logical ? DriveState::SeekingLogical : DriveState::SeekingPhysical;
  s_state.drive_event.SetIntervalAndSchedule(seek_time);

  s_state.seek_start_lba = s_state.current_lba;
  s_state.seek_end_lba = seek_lba;
  s_state.requested_lba = seek_lba;
  s_reader.QueueReadSector(s_state.requested_lba);
}

void CDROM::UpdateSubQPositionWhileSeeking()
{
  DebugAssert(IsSeeking());

  const float completed_frac = 1.0f - std::min(static_cast<float>(s_state.drive_event.GetTicksUntilNextExecution()) /
                                                 static_cast<float>(s_state.drive_event.GetInterval()),
                                               1.0f);

  CDImage::LBA current_lba;
  if (s_state.seek_end_lba > s_state.seek_start_lba)
  {
    current_lba =
      s_state.seek_start_lba +
      std::max<CDImage::LBA>(
        static_cast<CDImage::LBA>(static_cast<float>(s_state.seek_end_lba - s_state.seek_start_lba) * completed_frac),
        1);
  }
  else if (s_state.seek_end_lba < s_state.seek_start_lba)
  {
    current_lba =
      s_state.seek_start_lba -
      std::max<CDImage::LBA>(
        static_cast<CDImage::LBA>(static_cast<float>(s_state.seek_start_lba - s_state.seek_end_lba) * completed_frac),
        1);
  }
  else
  {
    // strange seek...
    return;
  }

  DEV_LOG("Update position while seeking from {} to {} - {} ({:.2f})", s_state.seek_start_lba, s_state.seek_end_lba,
          current_lba, completed_frac);

  s_state.last_subq_needs_update = (s_state.current_subq_lba != current_lba);
  s_state.current_lba = current_lba; // TODO: This is probably wrong... hold position shouldn't change.
  s_state.current_subq_lba = current_lba;
  s_state.subq_lba_update_tick = System::GetGlobalTickCounter();
  s_state.subq_lba_update_carry = 0;
}

void CDROM::UpdateSubQPosition(bool update_logical)
{
  const GlobalTicks ticks = System::GetGlobalTickCounter();
  if (IsSeeking() || IsReadingOrPlaying() || !IsMotorOn())
  {
    // If we're seeking+reading the first sector (no stat bits set), we need to return the set/current lba, not the last
    // SubQ LBA. Failing to do so may result in a track-jumped position getting returned in GetlocP, which causes
    // Mad Panic Coaster to go into a seek+play loop.
    if ((s_state.secondary_status.bits & (STAT_READING | STAT_PLAYING_CDDA | STAT_MOTOR_ON)) == STAT_MOTOR_ON &&
        s_state.current_lba != s_state.current_subq_lba)
    {
      WARNING_LOG("Jumping to hold position [{}->{}] while {} first sector", s_state.current_subq_lba,
                  s_state.current_lba, (s_state.drive_state == DriveState::Reading) ? "reading" : "playing");
      SetHoldPosition(s_state.current_lba, s_state.current_lba);
    }

    // Otherwise, this gets updated by the read event.
    return;
  }

  const u32 ticks_per_read = GetTicksForRead();
  const u32 diff = static_cast<u32>((ticks - s_state.subq_lba_update_tick) + s_state.subq_lba_update_carry);
  const u32 sector_diff = diff / ticks_per_read;
  const u32 carry = diff % ticks_per_read;
  if (sector_diff > 0)
  {
    // hardware tests show that it holds much closer to the target sector in logical mode
    const CDImage::LBA hold_offset = s_state.last_sector_header_valid ? 2 : 0;
    const CDImage::LBA sectors_per_track = GetSectorsPerTrack(s_state.current_lba);
    const CDImage::LBA hold_position = s_state.current_lba + hold_offset;
    const CDImage::LBA tjump_position = (hold_position >= sectors_per_track) ? (hold_position - sectors_per_track) : 0;
    const CDImage::LBA old_offset = s_state.current_subq_lba - tjump_position;
    const CDImage::LBA new_offset = (old_offset + sector_diff) % sectors_per_track;
    const CDImage::LBA new_subq_lba = tjump_position + new_offset;
#if defined(_DEBUG) || defined(_DEVEL)
    DEV_LOG("{} sectors @ {} SPT, old pos {}, hold pos {}, tjump pos {}, new pos {}", sector_diff, sectors_per_track,
            LBAToMSFString(s_state.current_subq_lba), LBAToMSFString(hold_position), LBAToMSFString(tjump_position),
            LBAToMSFString(new_subq_lba));
#endif
    if (s_state.current_subq_lba != new_subq_lba)
    {
      // we can defer this if we don't need the new sector header
      s_state.current_subq_lba = new_subq_lba;
      s_state.last_subq_needs_update = true;
      s_state.subq_lba_update_tick = ticks;
      s_state.subq_lba_update_carry = carry;

      if (update_logical)
      {
        CDImage::SubChannelQ real_subq = {};
        CDROMAsyncReader::SectorBuffer raw_sector;
        if (!s_reader.ReadSectorUncached(new_subq_lba, &real_subq, &raw_sector))
        {
          ERROR_LOG("Failed to read subq for sector {} for subq position", new_subq_lba);
        }
        else
        {
          s_state.last_subq_needs_update = false;

          const CDImage::SubChannelQ& subq = GetSectorSubQ(new_subq_lba, real_subq);
          if (subq.IsCRCValid())
            s_state.last_subq = subq;

          ProcessDataSectorHeader(raw_sector.data());
        }
      }
    }
  }
}

void CDROM::SetHoldPosition(CDImage::LBA lba, CDImage::LBA subq_lba)
{
  s_state.last_subq_needs_update |= (s_state.current_subq_lba != subq_lba);
  s_state.current_lba = lba;
  s_state.current_subq_lba = subq_lba;
  s_state.subq_lba_update_tick = System::GetGlobalTickCounter();
  s_state.subq_lba_update_carry = 0;
}

void CDROM::EnsureLastSubQValid()
{
  if (!s_state.last_subq_needs_update)
    return;

  s_state.last_subq_needs_update = false;

  CDImage::SubChannelQ real_subq = {};
  if (!s_reader.ReadSectorUncached(s_state.current_subq_lba, &real_subq, nullptr))
    ERROR_LOG("Failed to read subq for sector {} for subq position", s_state.current_subq_lba);

  const CDImage::SubChannelQ& subq = GetSectorSubQ(s_state.current_subq_lba, real_subq);
  if (subq.IsCRCValid())
    s_state.last_subq = subq;
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
  const bool logical = (s_state.drive_state == DriveState::SeekingLogical);
  ClearDriveState();

  bool seek_okay = s_reader.WaitForReadToComplete();

  s_state.current_subq_lba = s_reader.GetLastReadSector();
  s_state.last_subq_needs_update = false;
  s_state.subq_lba_update_tick = System::GetGlobalTickCounter();
  s_state.subq_lba_update_carry = 0;

  if (seek_okay)
  {
    const CDImage::SubChannelQ& subq = GetSectorSubQ(s_reader.GetLastReadSector(), s_reader.GetSectorSubQ());
    s_state.current_lba = s_reader.GetLastReadSector();

    if (subq.IsCRCValid())
    {
      // seek and update sub-q for ReadP command
      s_state.last_subq = subq;
      s_state.last_subq_needs_update = false;
      const auto [seek_mm, seek_ss, seek_ff] = CDImage::Position::FromLBA(s_reader.GetLastReadSector()).ToBCD();
      seek_okay = (subq.absolute_minute_bcd == seek_mm && subq.absolute_second_bcd == seek_ss &&
                   subq.absolute_frame_bcd == seek_ff);
      if (seek_okay)
      {
        if (subq.IsData())
        {
          if (logical)
          {
            ProcessDataSectorHeader(s_reader.GetSectorBuffer().data());
            seek_okay = (s_state.last_sector_header.minute == seek_mm && s_state.last_sector_header.second == seek_ss &&
                         s_state.last_sector_header.frame == seek_ff);

            if (seek_okay && !s_state.play_after_seek && !s_state.read_after_seek)
            {
              // This is pretty janky. The mech completes the seek when it "sees" a data header
              // 2 sectors before the seek target, so that a subsequent ReadN can complete nearly
              // immediately. Therefore when the seek completes, SubQ = Target, Data = Target - 2.
              // Hack the SubQ back by 2 frames so that following seeks will read forward. If we
              // ever properly handle SubQ versus data positions, this can be removed.
              s_state.current_subq_lba =
                (s_state.current_lba >= SUBQ_SECTOR_SKEW) ? (s_state.current_lba - SUBQ_SECTOR_SKEW) : 0;
              s_state.last_subq_needs_update = true;
            }
          }
        }
        else
        {
          if (logical)
          {
            WARNING_LOG("Logical seek to non-data sector [{:02x}:{:02x}:{:02x}]{}", seek_mm, seek_ss, seek_ff,
                        s_state.read_after_seek ? ", reading after seek" : "");

            // If CDDA mode isn't enabled and we're reading an audio sector, we need to fail the seek.
            // Test cases:
            //  - Wizard's Harmony does a logical seek to an audio sector, and expects it to succeed.
            //  - Vib-ribbon starts a read at an audio sector, and expects it to fail.
            if (s_state.read_after_seek)
              seek_okay = s_state.mode.cdda;
          }
        }

        if (subq.track_number_bcd == CDImage::LEAD_OUT_TRACK_NUMBER)
        {
          WARNING_LOG("Invalid seek to lead-out area (LBA {})", s_reader.GetLastReadSector());
          seek_okay = false;
        }
      }
    }
  }

  return seek_okay;
}

void CDROM::DoSeekComplete(TickCount ticks_late)
{
  const bool logical = (s_state.drive_state == DriveState::SeekingLogical);
  const bool seek_okay = CompleteSeek();
  if (seek_okay)
  {
    DEV_LOG("{} seek to [{}] complete{}", logical ? "Logical" : "Physical",
            LBAToMSFString(s_reader.GetLastReadSector()),
            s_state.read_after_seek ? ", now reading" : (s_state.play_after_seek ? ", now playing" : ""));

    // seek complete, transition to play/read if requested
    // INT2 is not sent on play/read
    if (s_state.read_after_seek)
    {
      BeginReading(ticks_late, true);
    }
    else if (s_state.play_after_seek)
    {
      BeginPlaying(0, ticks_late, true);
    }
    else
    {
      s_state.secondary_status.ClearActiveBits();
      s_state.async_response_fifo.Push(s_state.secondary_status.bits);
      SetAsyncInterrupt(Interrupt::Complete);
    }
  }
  else
  {
    WARNING_LOG("{} seek to [{}] failed", logical ? "Logical" : "Physical",
                LBAToMSFString(s_reader.GetLastReadSector()));
    s_state.secondary_status.ClearActiveBits();
    SendAsyncErrorResponse(STAT_SEEK_ERROR, 0x04);
    s_state.last_sector_header_valid = false;
  }

  s_state.setloc_pending = false;
  s_state.read_after_seek = false;
  s_state.play_after_seek = false;
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

  s_state.async_response_fifo.Clear();
  s_state.async_response_fifo.Push(s_state.secondary_status.bits);
  SetAsyncInterrupt(Interrupt::Complete);
}

void CDROM::DoChangeSessionComplete()
{
  DEBUG_LOG("Changing session complete");
  ClearDriveState();
  s_state.secondary_status.ClearActiveBits();
  s_state.secondary_status.motor_on = true;

  s_state.async_response_fifo.Clear();
  if (s_state.async_command_parameter == 0x01)
  {
    s_state.async_response_fifo.Push(s_state.secondary_status.bits);
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
  DEBUG_LOG("Spinup complete");
  s_state.drive_state = DriveState::Idle;
  s_state.drive_event.Deactivate();
  s_state.secondary_status.ClearActiveBits();
  s_state.secondary_status.motor_on = true;
}

void CDROM::DoSpeedChangeOrImplicitTOCReadComplete()
{
  DEBUG_LOG("Speed change/implicit TOC read complete");
  s_state.drive_state = DriveState::Idle;
  s_state.drive_event.Deactivate();
}

void CDROM::DoIDRead()
{
  DEBUG_LOG("ID read complete");
  s_state.secondary_status.ClearActiveBits();
  s_state.secondary_status.motor_on = CanReadMedia();

  // TODO: Audio CD.
  u8 stat_byte = s_state.secondary_status.bits;
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

  s_state.async_response_fifo.Clear();
  s_state.async_response_fifo.Push(stat_byte);
  s_state.async_response_fifo.Push(flags_byte);
  s_state.async_response_fifo.Push(0x20); // TODO: Disc type from TOC
  s_state.async_response_fifo.Push(0x00); // TODO: Session info?

  static constexpr u32 REGION_STRING_LENGTH = 4;
  static constexpr std::array<std::array<u8, REGION_STRING_LENGTH>, static_cast<size_t>(DiscRegion::Count)>
    region_strings = {{{'S', 'C', 'E', 'I'}, {'S', 'C', 'E', 'A'}, {'S', 'C', 'E', 'E'}, {0, 0, 0, 0}, {0, 0, 0, 0}}};
  s_state.async_response_fifo.PushRange(region_strings[static_cast<u8>(s_state.disc_region)].data(),
                                        REGION_STRING_LENGTH);

  SetAsyncInterrupt((flags_byte != 0) ? Interrupt::Error : Interrupt::Complete);
}

void CDROM::StopReadingWithDataEnd()
{
  ClearAsyncInterrupt();
  s_state.async_response_fifo.Push(s_state.secondary_status.bits);
  SetAsyncInterrupt(Interrupt::DataEnd);

  s_state.secondary_status.ClearActiveBits();
  ClearDriveState();
}

void CDROM::StartMotor()
{
  if (s_state.drive_state == DriveState::SpinningUp)
  {
    DEV_LOG("Starting motor - already spinning up");
    return;
  }

  DEV_LOG("Starting motor");
  s_state.drive_state = DriveState::SpinningUp;
  s_state.drive_event.Schedule(GetTicksForSpinUp());
}

void CDROM::StopMotor()
{
  s_state.secondary_status.ClearActiveBits();
  s_state.secondary_status.motor_on = false;
  ClearDriveState();
  SetHoldPosition(0, 0);
  s_state.last_sector_header_valid = false; // TODO: correct?
}

void CDROM::DoSectorRead()
{
  // TODO: Queue the next read here and swap the buffer.
  // TODO: Error handling
  if (!s_reader.WaitForReadToComplete())
    Panic("Sector read failed");

  s_state.current_lba = s_reader.GetLastReadSector();
  s_state.current_subq_lba = s_state.current_lba;
  s_state.last_subq_needs_update = false;
  s_state.subq_lba_update_tick = System::GetGlobalTickCounter();
  s_state.subq_lba_update_carry = 0;

  s_state.secondary_status.SetReadingBits(s_state.drive_state == DriveState::Playing);

  const CDImage::SubChannelQ& subq = GetSectorSubQ(s_state.current_lba, s_reader.GetSectorSubQ());
  const bool subq_valid = subq.IsCRCValid();
  if (subq_valid)
  {
    s_state.last_subq = subq;
    if (g_settings.cdrom_subq_skew) [[unlikely]]
    {
      // SubQ Skew Hack. It's horrible. Needed for Captain Commando.
      // Here's my previous rambling about the game:
      //
      //   So, there's two Getloc commands on the PS1 to retrieve the most-recent-read sector:
      //   GetlocL, which returns the timecode based on the data sector header, and GetlocP, which gets it from subq.
      //   Captain Commando would always corrupt the first boss sprite.
      //
      //   What the game does, is repeat the tile/texture data throughout the audio sectors for the background
      //   music when you reach the boss part of the level, it looks for a specific subq timecode coming in (by spamming
      //   GetlocP) then DMA's the data sector interleaved with the audio sectors out at the last possible moment
      //
      //   So, they hard coded it to look for a sector timecode +2 from the sector they actually wanted, then DMA that
      //   data out they do perform some validation on the data itself, so if you're not offsetting the timecode query,
      //   it never gets the right sector, and just keeps reading forever. Hence why the boss tiles are broken, because
      //   it never gets the data to upload. The most insane part is they should have just done what every other game
      //   does: use the raw read mode (2352 instead of 2048), and look at the data sector header. Instead they do this
      //   nonsense of repeating the data throughout the audio, and racing the DMA at the last possible minute.
      //
      // This hack just generates synthetic SubQ with a +2 offset. I'd planned on refactoring the CDImage interface
      // so that multiple sectors could be read in one back, in which case we could just "look ahead" to grab the
      // subq, but I haven't got around to it. It'll break libcrypt, but CC doesn't use it. One day I'll get around to
      // doing the refactor.... but given this is the only game that relies on it, priorities.
      s_reader.GetMedia()->GenerateSubChannelQ(&s_state.last_subq, s_state.current_lba + SUBQ_SECTOR_SKEW);
    }
  }
  else
  {
    DEV_LOG("Sector {} [{}] has invalid subchannel Q", s_state.current_lba, LBAToMSFString(s_state.current_lba));
  }

  if (subq.track_number_bcd == CDImage::LEAD_OUT_TRACK_NUMBER)
  {
    DEV_LOG("Read reached lead-out area of disc at LBA {}, stopping", s_reader.GetLastReadSector());
    StopReadingWithDataEnd();
    StopMotor();
    return;
  }

  const bool is_data_sector = subq.IsData();
  if (is_data_sector)
  {
    ProcessDataSectorHeader(s_reader.GetSectorBuffer().data());
  }
  else if (s_state.mode.auto_pause)
  {
    // Only update the tracked track-to-pause-after once auto pause is enabled. Pitball's menu music starts mid-second,
    // and there's no pregap, so the first couple of reports are for the previous track. It doesn't enable autopause
    // until receiving a couple, and it's actually playing the track it wants.
    if (s_state.play_track_number_bcd == 0)
    {
      // track number was not specified, but we've found the track now
      s_state.play_track_number_bcd = subq.track_number_bcd;
      DEBUG_LOG("Setting playing track number to {}", s_state.play_track_number_bcd);
    }
    else if (subq.track_number_bcd != s_state.play_track_number_bcd)
    {
      // we don't want to update the position if the track changes, so we check it before reading the actual sector.
      DEV_LOG("Auto pause at the start of track {:02x} (LBA {})", subq.track_number_bcd, s_state.current_lba);
      StopReadingWithDataEnd();
      return;
    }
  }

  u32 next_sector = s_state.current_lba + 1u;
  if (is_data_sector && s_state.drive_state == DriveState::Reading)
  {
    ProcessDataSector(s_reader.GetSectorBuffer().data(), subq);
  }
  else if (!is_data_sector && (s_state.drive_state == DriveState::Playing ||
                               (s_state.drive_state == DriveState::Reading && s_state.mode.cdda)))
  {
    ProcessCDDASector(s_reader.GetSectorBuffer().data(), subq, subq_valid);

    if (s_state.fast_forward_rate != 0)
      next_sector = s_state.current_lba + SignExtend32(s_state.fast_forward_rate);
  }
  else if (s_state.drive_state != DriveState::Reading && s_state.drive_state != DriveState::Playing)
  {
    Panic("Not reading or playing");
  }
  else
  {
    WARNING_LOG("Skipping sector {} as it is a {} sector and we're not {}", s_state.current_lba,
                is_data_sector ? "data" : "audio", is_data_sector ? "reading" : "playing");
  }

  s_state.requested_lba = next_sector;
  s_reader.QueueReadSector(s_state.requested_lba);
}

ALWAYS_INLINE_RELEASE void CDROM::ProcessDataSectorHeader(const u8* raw_sector)
{
  std::memcpy(&s_state.last_sector_header, &raw_sector[SECTOR_SYNC_SIZE], sizeof(s_state.last_sector_header));
  std::memcpy(&s_state.last_sector_subheader, &raw_sector[SECTOR_SYNC_SIZE + sizeof(s_state.last_sector_header)],
              sizeof(s_state.last_sector_subheader));
  s_state.last_sector_header_valid = true;
}

ALWAYS_INLINE_RELEASE void CDROM::ProcessDataSector(const u8* raw_sector, const CDImage::SubChannelQ& subq)
{
  const u32 sb_num = (s_state.current_write_sector_buffer + 1) % NUM_SECTOR_BUFFERS;
  DEV_COLOR_LOG(StrongMagenta, "DataSector {} LBA={} Mode={} Submode=0x{:02X} Buffer={}",
                LBAToMSFString(s_state.current_lba), s_state.current_lba, s_state.last_sector_header.sector_mode,
                ZeroExtend32(s_state.last_sector_subheader.submode.bits), sb_num);

  if (s_state.mode.xa_enable && s_state.last_sector_header.sector_mode == 2)
  {
    if (s_state.last_sector_subheader.submode.realtime && s_state.last_sector_subheader.submode.audio)
    {
      ProcessXAADPCMSector(raw_sector, subq);

      // Audio+realtime sectors aren't delivered to the CPU.
      return;
    }
  }

  // TODO: How does XA relate to this buffering?
  SectorBuffer* sb = &s_state.sector_buffers[sb_num];
  if (sb->position == 0 && sb->size > 0)
  {
    DEV_LOG("Sector buffer {} was not read, previous sector dropped",
            (s_state.current_write_sector_buffer - 1) % NUM_SECTOR_BUFFERS);
  }

  if (s_state.mode.ignore_bit)
    WARNING_LOG("SetMode.4 bit set on read of sector {}", s_state.current_lba);

  if (s_state.mode.read_raw_sector)
  {
    if (s_state.last_sector_header.sector_mode == 1)
    {
      // Raw reads in MODE1 appear to fill in a MODE2 header...
      std::memcpy(&sb->data[0], raw_sector + SECTOR_SYNC_SIZE, MODE1_HEADER_SIZE);
      std::memset(&sb->data[MODE1_HEADER_SIZE], 0, MODE2_HEADER_SIZE - MODE1_HEADER_SIZE);
      std::memcpy(&sb->data[MODE2_HEADER_SIZE], raw_sector + SECTOR_SYNC_SIZE + MODE1_HEADER_SIZE,
                  DATA_SECTOR_OUTPUT_SIZE);
      sb->size = MODE2_HEADER_SIZE + DATA_SECTOR_OUTPUT_SIZE;
    }
    else
    {
      std::memcpy(sb->data.data(), raw_sector + SECTOR_SYNC_SIZE, RAW_SECTOR_OUTPUT_SIZE);
      sb->size = RAW_SECTOR_OUTPUT_SIZE;
    }
  }
  else
  {
    if (s_state.last_sector_header.sector_mode != 1 && s_state.last_sector_header.sector_mode != 2)
    {
      WARNING_LOG("Ignoring non-MODE1/MODE2 sector at {}", s_state.current_lba);
      return;
    }

    const u32 offset = (s_state.last_sector_header.sector_mode == 1) ? (SECTOR_SYNC_SIZE + MODE1_HEADER_SIZE) :
                                                                       (SECTOR_SYNC_SIZE + MODE2_HEADER_SIZE);
    std::memcpy(sb->data.data(), raw_sector + offset, DATA_SECTOR_OUTPUT_SIZE);
    sb->size = DATA_SECTOR_OUTPUT_SIZE;
  }

  sb->position = 0;
  s_state.current_write_sector_buffer = sb_num;

  // Deliver to CPU
  if (HasPendingAsyncInterrupt())
  {
    WARNING_LOG("Data interrupt was not delivered");
    ClearAsyncInterrupt();
  }

  if (HasPendingInterrupt())
  {
    const u32 sectors_missed =
      (s_state.current_write_sector_buffer - s_state.current_read_sector_buffer) % NUM_SECTOR_BUFFERS;
    if (sectors_missed > 1)
      WARNING_LOG("Interrupt not processed in time, missed {} sectors", sectors_missed - 1);
  }

  s_state.async_response_fifo.Push(s_state.secondary_status.bits);
  SetAsyncInterrupt(Interrupt::DataReady);
}

std::tuple<s16, s16> CDROM::GetAudioFrame()
{
  const u32 frame = s_state.audio_fifo.IsEmpty() ? 0u : s_state.audio_fifo.Pop();
  const s16 left = static_cast<s16>(Truncate16(frame));
  const s16 right = static_cast<s16>(Truncate16(frame >> 16));
  const s16 left_out = SaturateVolume(ApplyVolume(left, s_state.cd_audio_volume_matrix[0][0]) +
                                      ApplyVolume(right, s_state.cd_audio_volume_matrix[1][0]));
  const s16 right_out = SaturateVolume(ApplyVolume(left, s_state.cd_audio_volume_matrix[0][1]) +
                                       ApplyVolume(right, s_state.cd_audio_volume_matrix[1][1]));
  return std::tuple<s16, s16>(left_out, right_out);
}

void CDROM::AddCDAudioFrame(s16 left, s16 right)
{
  s_state.audio_fifo.Push(ZeroExtend32(static_cast<u16>(left)) | (ZeroExtend32(static_cast<u16>(right)) << 16));
}

s32 CDROM::ApplyVolume(s16 sample, u8 volume)
{
  return s32(sample) * static_cast<s32>(ZeroExtend32(volume)) >> 7;
}

s16 CDROM::SaturateVolume(s32 volume)
{
  return static_cast<s16>((volume < -0x8000) ? -0x8000 : ((volume > 0x7FFF) ? 0x7FFF : volume));
}

template<bool IS_STEREO, bool IS_8BIT>
void CDROM::DecodeXAADPCMChunks(const u8* chunk_ptr, s16* samples)
{
  static constexpr std::array<s8, 16> filter_table_pos = {{0, 60, 115, 98, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
  static constexpr std::array<s8, 16> filter_table_neg = {{0, 0, -52, -55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};

  // The data layout is annoying here. Each word of data is interleaved with the other blocks, requiring multiple
  // passes to decode the whole chunk.
  constexpr u32 NUM_CHUNKS = 18;
  constexpr u32 CHUNK_SIZE_IN_BYTES = 128;
  constexpr u32 WORDS_PER_CHUNK = 28;
  constexpr u32 SAMPLES_PER_CHUNK = WORDS_PER_CHUNK * (IS_8BIT ? 4 : 8);
  constexpr u32 NUM_BLOCKS = IS_8BIT ? 4 : 8;
  constexpr u32 WORDS_PER_BLOCK = 28;

  for (u32 i = 0; i < NUM_CHUNKS; i++)
  {
    const u8* headers_ptr = chunk_ptr + 4;
    const u8* words_ptr = chunk_ptr + 16;

    for (u32 block = 0; block < NUM_BLOCKS; block++)
    {
      const XA_ADPCMBlockHeader block_header{headers_ptr[block]};
      const u8 shift = block_header.GetShift();
      const u8 filter = block_header.GetFilter();
      const s32 filter_pos = filter_table_pos[filter];
      const s32 filter_neg = filter_table_neg[filter];

      s16* out_samples_ptr =
        IS_STEREO ? &samples[(block / 2) * (WORDS_PER_BLOCK * 2) + (block % 2)] : &samples[block * WORDS_PER_BLOCK];
      constexpr u32 out_samples_increment = IS_STEREO ? 2 : 1;

      for (u32 word = 0; word < 28; word++)
      {
        // NOTE: assumes LE
        u32 word_data;
        std::memcpy(&word_data, &words_ptr[word * sizeof(u32)], sizeof(word_data));

        // extract nibble from block
        const u32 nibble = IS_8BIT ? ((word_data >> (block * 8)) & 0xFF) : ((word_data >> (block * 4)) & 0x0F);
        const s16 sample = static_cast<s16>(Truncate16(nibble << (IS_8BIT ? 8 : 12))) >> shift;

        // mix in previous values
        s32* prev = IS_STEREO ? &s_state.xa_last_samples[(block & 1) * 2] : &s_state.xa_last_samples[0];
        const s32 interp_sample = std::clamp<s32>(
          static_cast<s32>(sample) + ((prev[0] * filter_pos) >> 6) + ((prev[1] * filter_neg) >> 6), -32768, 32767);

        // update previous values
        prev[1] = prev[0];
        prev[0] = interp_sample;

        *out_samples_ptr = static_cast<s16>(interp_sample);
        out_samples_ptr += out_samples_increment;
      }
    }

    samples += SAMPLES_PER_CHUNK;
    chunk_ptr += CHUNK_SIZE_IN_BYTES;
  }
}

template<bool STEREO>
void CDROM::ResampleXAADPCM(const s16* frames_in, u32 num_frames_in)
{
  static constexpr auto zigzag_interpolate = [](const s16* ringbuf, u32 table_index, u32 p) -> s16 {
    static std::array<std::array<s16, 29>, 7> tables = {
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

    const s16* table = tables[table_index].data();
    s32 sum = 0;
    for (u32 i = 0; i < 29; i++)
      sum += (static_cast<s32>(ringbuf[(p - i) & 0x1F]) * static_cast<s32>(table[i])) >> 15;

    return static_cast<s16>(std::clamp<s32>(sum, -0x8000, 0x7FFF));
  };

  s16* const left_ringbuf = s_state.xa_resample_ring_buffer[0].data();
  [[maybe_unused]] s16* const right_ringbuf = s_state.xa_resample_ring_buffer[1].data();
  u32 p = s_state.xa_resample_p;
  u32 sixstep = s_state.xa_resample_sixstep;

  for (u32 in_sample_index = 0; in_sample_index < num_frames_in; in_sample_index++)
  {
    // TODO: We can vectorize the multiplications in zigzag_interpolate by duplicating the sample in the ringbuffer at
    // offset +32, allowing it to wrap once.
    left_ringbuf[p] = *(frames_in++);
    if constexpr (STEREO)
      right_ringbuf[p] = *(frames_in++);
    p = (p + 1) % 32;
    sixstep--;

    if (sixstep == 0)
    {
      sixstep = 6;
      for (u32 j = 0; j < 7; j++)
      {
        const s16 left_interp = zigzag_interpolate(left_ringbuf, j, p);
        const s16 right_interp = STEREO ? zigzag_interpolate(right_ringbuf, j, p) : left_interp;
        AddCDAudioFrame(left_interp, right_interp);
      }
    }
  }

  s_state.xa_resample_p = Truncate8(p);
  s_state.xa_resample_sixstep = Truncate8(sixstep);
}

template<bool STEREO>
void CDROM::ResampleXAADPCM18900(const s16* frames_in, u32 num_frames_in)
{
  // Weights originally from Mednafen's interpolator. It's unclear where these came from, perhaps it was calculated
  // somehow. This doesn't appear to use a zigzag pattern like psx-spx suggests, therefore it is restricted to only
  // 18900hz resampling. Duplicating the 18900hz samples to 37800hz sounds even more awful than lower sample rate audio
  // should, with a big spike at ~16KHz, especially with music in FMVs. Fortunately, few games actually use 18900hz XA.
  static constexpr auto interpolate = [](const s16* ringbuf, u32 table_index, u32 p) -> s16 {
    static std::array<std::array<s16, 25>, 7> tables = {{
      {{0x0,     -0x5,  0x11,   -0x23, 0x46,  -0x17, -0x44, 0x15b, -0x347, 0x80e, -0x1249, 0x3c07, 0x53e0,
        -0x16fa, 0xafa, -0x548, 0x27b, -0xeb, 0x1a,  0x2b,  -0x23, 0x10,   -0x8,  0x2,     0x0}},
      {{0x0,     -0x2,  0xa,    -0x22, 0x41,   -0x54, 0x34, 0x9,   -0x10a, 0x400, -0xa78, 0x234c, 0x6794,
        -0x1780, 0xbcd, -0x623, 0x350, -0x16d, 0x6b,  0xa,  -0x10, 0x11,   -0x8,  0x3,    -0x1}},
      {{-0x2,    0x0,   0x3,    -0x13, 0x3c,   -0x4b, 0xa2,  -0xe3, 0x132, -0x43, -0x267, 0xc9d, 0x74bb,
        -0x11b4, 0x9b8, -0x5bf, 0x372, -0x1a8, 0xa6,  -0x1b, 0x5,   0x6,   -0x8,  0x3,    -0x1}},
      {{-0x1,   0x3,   -0x2,   -0x5,  0x1f,   -0x4a, 0xb3,  -0x192, 0x2b1, -0x39e, 0x4f8, -0x5a6, 0x7939,
        -0x5a6, 0x4f8, -0x39e, 0x2b1, -0x192, 0xb3,  -0x4a, 0x1f,   -0x5,  -0x2,   0x3,   -0x1}},
      {{-0x1,  0x3,    -0x8,  0x6,   0x5,   -0x1b, 0xa6,  -0x1a8, 0x372, -0x5bf, 0x9b8, -0x11b4, 0x74bb,
        0xc9d, -0x267, -0x43, 0x132, -0xe3, 0xa2,  -0x4b, 0x3c,   -0x13, 0x3,    0x0,   -0x2}},
      {{-0x1,   0x3,    -0x8,  0x11,   -0x10, 0xa,  0x6b,  -0x16d, 0x350, -0x623, 0xbcd, -0x1780, 0x6794,
        0x234c, -0xa78, 0x400, -0x10a, 0x9,   0x34, -0x54, 0x41,   -0x22, 0xa,    -0x2,  0x0}},
      {{0x0,    0x2,     -0x8,  0x10,   -0x23, 0x2b,  0x1a,  -0xeb, 0x27b, -0x548, 0xafa, -0x16fa, 0x53e0,
        0x3c07, -0x1249, 0x80e, -0x347, 0x15b, -0x44, -0x17, 0x46,  -0x23, 0x11,   -0x5,  0x0}},
    }};

    const s16* table = tables[table_index].data();
    s32 sum = 0;
    for (u32 i = 0; i < 25; i++)
      sum += (static_cast<s32>(ringbuf[(p + 32 - 25 + i) & 0x1F]) * static_cast<s32>(table[i]));

    return static_cast<s16>(std::clamp<s32>(sum >> 15, -0x8000, 0x7FFF));
  };

  s16* const left_ringbuf = s_state.xa_resample_ring_buffer[0].data();
  [[maybe_unused]] s16* const right_ringbuf = s_state.xa_resample_ring_buffer[1].data();
  u32 p = s_state.xa_resample_p;
  u32 sixstep = s_state.xa_resample_sixstep;

  for (u32 in_sample_index = 0; in_sample_index < num_frames_in;)
  {
    if (sixstep >= 7)
    {
      sixstep -= 7;
      p = (p + 1) % 32;

      left_ringbuf[p] = *(frames_in++);
      if constexpr (STEREO)
        right_ringbuf[p] = *(frames_in++);

      in_sample_index++;
    }

    const s16 left_interp = interpolate(left_ringbuf, sixstep, p);
    const s16 right_interp = STEREO ? interpolate(right_ringbuf, sixstep, p) : left_interp;
    AddCDAudioFrame(left_interp, right_interp);
    sixstep += 3;
  }

  s_state.xa_resample_p = Truncate8(p);
  s_state.xa_resample_sixstep = Truncate8(sixstep);
}

void CDROM::ResetCurrentXAFile()
{
  s_state.xa_current_channel_number = 0;
  s_state.xa_current_file_number = 0;
  s_state.xa_current_set = false;
}

void CDROM::ResetAudioDecoder()
{
  ResetCurrentXAFile();

  s_state.xa_last_samples.fill(0);
  for (u32 i = 0; i < 2; i++)
  {
    s_state.xa_resample_ring_buffer[i].fill(0);
    s_state.xa_resample_p = 0;
    s_state.xa_resample_sixstep = 6;
  }
  s_state.audio_fifo.Clear();
}

ALWAYS_INLINE_RELEASE void CDROM::ProcessXAADPCMSector(const u8* raw_sector, const CDImage::SubChannelQ& subq)
{
  // Check for automatic ADPCM filter.
  if (s_state.mode.xa_filter && (s_state.last_sector_subheader.file_number != s_state.xa_filter_file_number ||
                                 s_state.last_sector_subheader.channel_number != s_state.xa_filter_channel_number))
  {
    DEBUG_LOG("Skipping sector due to filter mismatch (expected {}/{} got {}/{})", s_state.xa_filter_file_number,
              s_state.xa_filter_channel_number, s_state.last_sector_subheader.file_number,
              s_state.last_sector_subheader.channel_number);
    return;
  }

  // Track the current file being played. If this is not set by the filter, it'll be set by the first file/sector which
  // is read. Fixes audio in Tomb Raider III menu.
  if (!s_state.xa_current_set)
  {
    // Some games (Taxi 2 and Blues Clues) have junk audio sectors with a channel number of 255.
    // We need to skip them otherwise it ends up playing the incorrect file.
    // TODO: Verify with a hardware test.
    if (s_state.last_sector_subheader.channel_number == 255 &&
        (!s_state.mode.xa_filter || s_state.xa_filter_channel_number != 255))
    {
      WARNING_LOG("Skipping XA file with file number {} and channel number {} (submode 0x{:02X} coding 0x{:02X})",
                  s_state.last_sector_subheader.file_number, s_state.last_sector_subheader.channel_number,
                  s_state.last_sector_subheader.submode.bits, s_state.last_sector_subheader.codinginfo.bits);
      return;
    }

    s_state.xa_current_file_number = s_state.last_sector_subheader.file_number;
    s_state.xa_current_channel_number = s_state.last_sector_subheader.channel_number;
    s_state.xa_current_set = true;
  }
  else if (s_state.last_sector_subheader.file_number != s_state.xa_current_file_number ||
           s_state.last_sector_subheader.channel_number != s_state.xa_current_channel_number)
  {
    DEBUG_LOG("Skipping sector due to current file mismatch (expected {}/{} got {}/{})", s_state.xa_current_file_number,
              s_state.xa_current_channel_number, s_state.last_sector_subheader.file_number,
              s_state.last_sector_subheader.channel_number);
    return;
  }

  // Reset current file on EOF, and play the file in the next sector.
  if (s_state.last_sector_subheader.submode.eof)
    ResetCurrentXAFile();

  // Ensure the SPU is caught up for the test below.
  SPU::GeneratePendingSamples();

  // Since the disc reads and SPU are running at different speeds, we might be _slightly_ behind, which is fine, since
  // the SPU will over-read in the next batch to catch up. We also should not process the sector, because it'll affect
  // the previous samples used for interpolation/ADPCM. Not doing so causes crackling audio in Simple 1500 Series Vol.
  // 92 - The Tozan RPG - Ginrei no Hasha (Japan).
  const u32 num_frames = s_state.last_sector_subheader.codinginfo.GetSamplesPerSector() >>
                         BoolToUInt8(s_state.last_sector_subheader.codinginfo.IsStereo());
  if (s_state.audio_fifo.GetSize() > AUDIO_FIFO_LOW_WATERMARK)
  {
    DEV_LOG("Dropping {} XA frames because audio FIFO still has {} frames", num_frames, s_state.audio_fifo.GetSize());
    return;
  }

  // If muted, we still need to decode the data, to update the previous samples.
  std::array<s16, XA_ADPCM_SAMPLES_PER_SECTOR_4BIT> sample_buffer;
  const u8* xa_block_start =
    raw_sector + CDImage::SECTOR_SYNC_SIZE + sizeof(CDImage::SectorHeader) + sizeof(XASubHeader) * 2;
  s_state.xa_current_codinginfo.bits = s_state.last_sector_subheader.codinginfo.bits;

  if (s_state.last_sector_subheader.codinginfo.Is8BitADPCM())
  {
    if (s_state.last_sector_subheader.codinginfo.IsStereo())
      DecodeXAADPCMChunks<true, true>(xa_block_start, sample_buffer.data());
    else
      DecodeXAADPCMChunks<false, true>(xa_block_start, sample_buffer.data());
  }
  else
  {
    if (s_state.last_sector_subheader.codinginfo.IsStereo())
      DecodeXAADPCMChunks<true, false>(xa_block_start, sample_buffer.data());
    else
      DecodeXAADPCMChunks<false, false>(xa_block_start, sample_buffer.data());
  }

  // Only send to SPU if we're not muted.
  if (s_state.muted || s_state.adpcm_muted || g_settings.cdrom_mute_cd_audio)
    return;

  if (s_state.last_sector_subheader.codinginfo.IsStereo())
  {
    if (s_state.last_sector_subheader.codinginfo.IsHalfSampleRate())
      ResampleXAADPCM18900<true>(sample_buffer.data(), num_frames);
    else
      ResampleXAADPCM<true>(sample_buffer.data(), num_frames);
  }
  else
  {
    if (s_state.last_sector_subheader.codinginfo.IsHalfSampleRate())
      ResampleXAADPCM18900<false>(sample_buffer.data(), num_frames);
    else
      ResampleXAADPCM<false>(sample_buffer.data(), num_frames);
  }
}

static s16 GetPeakVolume(const u8* raw_sector, u8 channel)
{
  static constexpr u32 NUM_SAMPLES = CDImage::RAW_SECTOR_SIZE / sizeof(s16);

  static_assert(Common::IsAlignedPow2(NUM_SAMPLES, 8));
  const u8* current_ptr = raw_sector;
  GSVector4i v_peak = GSVector4i::zero();
  for (u32 i = 0; i < NUM_SAMPLES; i += 8)
  {
    v_peak = v_peak.max_s16(GSVector4i::load<false>(current_ptr));
    current_ptr += sizeof(v_peak);
  }

  // Convert 16->32bit, removing the unneeded channel.
  if (channel == 0)
    v_peak = v_peak.sll32<16>();
  v_peak = v_peak.sra32<16>();
  return static_cast<s16>(v_peak.maxv_s32());
}

ALWAYS_INLINE_RELEASE void CDROM::ProcessCDDASector(const u8* raw_sector, const CDImage::SubChannelQ& subq,
                                                    bool subq_valid)
{
  // For CDDA sectors, the whole sector contains the audio data.
  DEV_COLOR_LOG(StrongMagenta, "CDDASector {} LBA={} Track={:02x} Index={:02x} Rel={:02x}:{:02x}:{:02x} Ctrl={:02x}",
                LBAToMSFString(s_state.current_lba), s_state.current_lba, subq.track_number_bcd, subq.index_number_bcd,
                subq.relative_minute_bcd, subq.relative_second_bcd, subq.relative_frame_bcd, subq.control_bits);

  // The reporting doesn't happen if we're reading with the CDDA mode bit set.
  if (s_state.drive_state == DriveState::Playing && s_state.mode.report_audio && subq_valid)
  {
    if (s_state.cdda_report_start_delay == 0)
    {
      const u8 frame_nibble = subq.absolute_frame_bcd >> 4;

      if (s_state.last_cdda_report_frame_nibble != frame_nibble)
      {
        s_state.last_cdda_report_frame_nibble = frame_nibble;

        ClearAsyncInterrupt();
        s_state.async_response_fifo.Push(s_state.secondary_status.bits);
        s_state.async_response_fifo.Push(subq.track_number_bcd);
        s_state.async_response_fifo.Push(subq.index_number_bcd);
        if (subq.absolute_frame_bcd & 0x10)
        {
          s_state.async_response_fifo.Push(subq.relative_minute_bcd);
          s_state.async_response_fifo.Push(0x80 | subq.relative_second_bcd);
          s_state.async_response_fifo.Push(subq.relative_frame_bcd);
        }
        else
        {
          s_state.async_response_fifo.Push(subq.absolute_minute_bcd);
          s_state.async_response_fifo.Push(subq.absolute_second_bcd);
          s_state.async_response_fifo.Push(subq.absolute_frame_bcd);
        }

        const u8 channel = subq.absolute_second_bcd & 1u;
        const s16 peak_volume = std::min<s16>(GetPeakVolume(raw_sector, channel), 32767);
        const u16 peak_value = (ZeroExtend16(channel) << 15) | peak_volume;

        s_state.async_response_fifo.Push(Truncate8(peak_value));      // peak low
        s_state.async_response_fifo.Push(Truncate8(peak_value >> 8)); // peak high
        SetAsyncInterrupt(Interrupt::DataReady);

        DEV_COLOR_LOG(
          StrongCyan,
          "Report     Track[{:02x}] Index[{:02x}] Rel[{:02x}:{:02x}:{:02x}] Abs[{:02x}:{:02x}:{:02x}] Peak[{}:{}]",
          subq.track_number_bcd, subq.index_number_bcd, subq.relative_minute_bcd, subq.relative_second_bcd,
          subq.relative_frame_bcd, subq.absolute_minute_bcd, subq.absolute_second_bcd, subq.absolute_frame_bcd, channel,
          peak_volume);
      }
    }
    else
    {
      s_state.cdda_report_start_delay--;
    }
  }

  // Apply volume when pushing sectors to SPU.
  if (s_state.muted || g_settings.cdrom_mute_cd_audio)
    return;

  SPU::GeneratePendingSamples();

  // 2 samples per channel, always stereo.
  // Apparently in 2X mode, only half the samples in a sector get processed.
  // Test cast: Menu background sound in 360 Three Sixty.
  const u32 num_samples = (CDImage::RAW_SECTOR_SIZE / sizeof(s16)) / (s_state.mode.double_speed ? 4 : 2);
  const u32 remaining_space = s_state.audio_fifo.GetSpace();
  if (remaining_space < num_samples)
  {
    WARNING_LOG("Dropping {} frames from audio FIFO", num_samples - remaining_space);
    s_state.audio_fifo.Remove(num_samples - remaining_space);
  }

  const u8* sector_ptr = raw_sector;
  const size_t step = s_state.mode.double_speed ? (sizeof(s16) * 4) : (sizeof(s16) * 2);
  for (u32 i = 0; i < num_samples; i++)
  {
    s16 samp_left, samp_right;
    std::memcpy(&samp_left, sector_ptr, sizeof(samp_left));
    std::memcpy(&samp_right, sector_ptr + sizeof(s16), sizeof(samp_right));
    sector_ptr += step;
    AddCDAudioFrame(samp_left, samp_right);
  }
}

void CDROM::ClearSectorBuffers()
{
  s_state.current_read_sector_buffer = 0;
  s_state.current_write_sector_buffer = 0;

  for (SectorBuffer& sb : s_state.sector_buffers)
  {
    sb.position = 0;
    sb.size = 0;
  }

  s_state.request_register.BFRD = false;
  s_state.status.DRQSTS = false;
}

void CDROM::CheckForSectorBufferReadComplete()
{
  SectorBuffer& sb = s_state.sector_buffers[s_state.current_read_sector_buffer];

  // BFRD gets cleared on DMA completion.
  s_state.request_register.BFRD = (s_state.request_register.BFRD && sb.position < sb.size);
  s_state.status.DRQSTS = s_state.request_register.BFRD;

  // Buffer complete?
  if (sb.position >= sb.size)
  {
    sb.position = 0;
    sb.size = 0;
  }

  // Redeliver missed sector on DMA/read complete.
  // This would be the main loop checking when the DMA is complete, if there's another sector pending.
  // Normally, this would happen some time after the DMA actually completes, so we need to put it on a delay.
  // Otherwise, if games read the header then data out as two separate transfers (which is typical), they'll
  // get the header for one sector, and the header for the next in the second transfer.
  SectorBuffer& next_sb = s_state.sector_buffers[s_state.current_write_sector_buffer];
  if (next_sb.position == 0 && next_sb.size > 0 && !HasPendingAsyncInterrupt())
  {
    DEV_LOG("Sending additional INT1 for missed sector in buffer {}", s_state.current_write_sector_buffer);
    s_state.async_response_fifo.Push(s_state.secondary_status.bits);
    s_state.pending_async_interrupt = static_cast<u8>(Interrupt::DataReady);
    s_state.async_interrupt_event.Schedule(INTERRUPT_DELAY_CYCLES);
  }
}

void CDROM::CreateFileMap()
{
  s_state.file_map.clear();
  s_state.file_map_created = true;

  if (!s_reader.HasMedia())
    return;

  s_reader.WaitForIdle();
  CDImage* media = s_reader.GetMedia();
  IsoReader iso;
  if (!iso.Open(media, 1))
  {
    ERROR_LOG("Failed to open ISO filesystem.");
    return;
  }

  DEV_LOG("Creating file map for {}...", media->GetPath());
  s_state.file_map.emplace(iso.GetPVDLBA(), std::make_pair(iso.GetPVDLBA(), std::string("PVD")));
  CreateFileMap(iso, std::string_view());
  DEV_LOG("Found {} files", s_state.file_map.size());
}

void CDROM::CreateFileMap(IsoReader& iso, std::string_view dir)
{
  for (auto& [path, entry] : iso.GetEntriesInDirectory(dir))
  {
    if (entry.IsDirectory())
    {
      DEV_LOG("{}-{} = {}", entry.location_le, entry.location_le + entry.GetSizeInSectors() - 1, path);
      s_state.file_map.emplace(entry.location_le, std::make_pair(entry.location_le + entry.GetSizeInSectors() - 1,
                                                                 fmt::format("<DIR> {}", path)));

      CreateFileMap(iso, path);
      continue;
    }

    DEV_LOG("{}-{} = {}", entry.location_le, entry.location_le + entry.GetSizeInSectors() - 1, path);
    s_state.file_map.emplace(entry.location_le,
                             std::make_pair(entry.location_le + entry.GetSizeInSectors() - 1, std::move(path)));
  }
}

const std::string* CDROM::LookupFileMap(u32 lba, u32* start_lba, u32* end_lba)
{
  if (s_state.file_map.empty())
    return nullptr;

  auto iter = s_state.file_map.lower_bound(lba);
  if (iter == s_state.file_map.end())
    iter = (++s_state.file_map.rbegin()).base();
  if (lba < iter->first)
  {
    // before first file
    if (iter == s_state.file_map.begin())
      return nullptr;

    --iter;
  }
  if (lba > iter->second.first)
    return nullptr;

  *start_lba = iter->first;
  *end_lba = iter->second.first;
  return &iter->second.second;
}

void CDROM::DrawDebugWindow(float scale)
{
  static const ImVec4 active_color{1.0f, 1.0f, 1.0f, 1.0f};
  static const ImVec4 inactive_color{0.4f, 0.4f, 0.4f, 1.0f};

  // draw voice states
  if (ImGui::CollapsingHeader("Media", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (s_reader.HasMedia())
    {
      const CDImage* media = s_reader.GetMedia();
      const CDImage::Position disc_position = CDImage::Position::FromLBA(s_state.current_lba);
      const float start_y = ImGui::GetCursorPosY();

      if (media->HasSubImages())
      {
        ImGui::Text("Filename: %s [Subimage %u of %u] [%u buffered sectors]", media->GetPath().c_str(),
                    media->GetCurrentSubImage() + 1u, media->GetSubImageCount(), s_reader.GetBufferedSectorCount());
      }
      else
      {
        ImGui::Text("Filename: %s [%u buffered sectors]", media->GetPath().c_str(), s_reader.GetBufferedSectorCount());
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
          s_state.current_lba - media->GetTrackStartPosition(static_cast<u8>(media->GetTrackNumber())));
        ImGui::Text("Track Position: Number[%u] MSF[%02u:%02u:%02u] LBA[%u]", media->GetTrackNumber(),
                    track_position.minute, track_position.second, track_position.frame, track_position.ToLBA());
      }

      ImGui::Text("Last Sector: %02X:%02X:%02X (Mode %u)", s_state.last_sector_header.minute,
                  s_state.last_sector_header.second, s_state.last_sector_header.frame,
                  s_state.last_sector_header.sector_mode);

      if (s_state.show_current_file)
      {
        if (media->GetTrackNumber() == 1)
        {
          if (!s_state.file_map_created)
            CreateFileMap();

          u32 current_file_start_lba, current_file_end_lba;
          const u32 track_lba =
            s_state.current_lba - media->GetTrackStartPosition(static_cast<u8>(media->GetTrackNumber()));
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
        ImGui::Text("[%u files on disc]", static_cast<u32>(s_state.file_map.size()));
      }
      else
      {
        const float end_y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 120.0f * scale);
        ImGui::SetCursorPosY(start_y);
        if (ImGui::Button("Show Current File"))
          s_state.show_current_file = true;

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

    ImGui::TextColored(s_state.status.ADPBUSY ? active_color : inactive_color, "ADPBUSY: %s",
                       s_state.status.ADPBUSY ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.secondary_status.error ? active_color : inactive_color, "Error: %s",
                       s_state.secondary_status.error ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.mode.cdda ? active_color : inactive_color, "CDDA: %s", s_state.mode.cdda ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextColored(s_state.status.PRMEMPTY ? active_color : inactive_color, "PRMEMPTY: %s",
                       s_state.status.PRMEMPTY ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.secondary_status.motor_on ? active_color : inactive_color, "Motor On: %s",
                       s_state.secondary_status.motor_on ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.mode.auto_pause ? active_color : inactive_color, "Auto Pause: %s",
                       s_state.mode.auto_pause ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextColored(s_state.status.PRMWRDY ? active_color : inactive_color, "PRMWRDY: %s",
                       s_state.status.PRMWRDY ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.secondary_status.seek_error ? active_color : inactive_color, "Seek Error: %s",
                       s_state.secondary_status.seek_error ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.mode.report_audio ? active_color : inactive_color, "Report Audio: %s",
                       s_state.mode.report_audio ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextColored(s_state.status.RSLRRDY ? active_color : inactive_color, "RSLRRDY: %s",
                       s_state.status.RSLRRDY ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.secondary_status.id_error ? active_color : inactive_color, "ID Error: %s",
                       s_state.secondary_status.id_error ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.mode.xa_filter ? active_color : inactive_color, "XA Filter: %s (File %u Channel %u)",
                       s_state.mode.xa_filter ? "Yes" : "No", s_state.xa_filter_file_number,
                       s_state.xa_filter_channel_number);
    ImGui::NextColumn();

    ImGui::TextColored(s_state.status.DRQSTS ? active_color : inactive_color, "DRQSTS: %s",
                       s_state.status.DRQSTS ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.secondary_status.shell_open ? active_color : inactive_color, "Shell Open: %s",
                       s_state.secondary_status.shell_open ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.mode.ignore_bit ? active_color : inactive_color, "Ignore Bit: %s",
                       s_state.mode.ignore_bit ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextColored(s_state.status.BUSYSTS ? active_color : inactive_color, "BUSYSTS: %s",
                       s_state.status.BUSYSTS ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.secondary_status.reading ? active_color : inactive_color, "Reading: %s",
                       s_state.secondary_status.reading ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.mode.read_raw_sector ? active_color : inactive_color, "Read Raw Sectors: %s",
                       s_state.mode.read_raw_sector ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::NextColumn();
    ImGui::TextColored(s_state.secondary_status.seeking ? active_color : inactive_color, "Seeking: %s",
                       s_state.secondary_status.seeking ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.mode.xa_enable ? active_color : inactive_color, "XA Enable: %s",
                       s_state.mode.xa_enable ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::NextColumn();
    ImGui::TextColored(s_state.secondary_status.playing_cdda ? active_color : inactive_color, "Playing CDDA: %s",
                       s_state.secondary_status.playing_cdda ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.mode.double_speed ? active_color : inactive_color, "Double Speed: %s",
                       s_state.mode.double_speed ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::Columns(1);
    ImGui::NewLine();

    if (HasPendingCommand())
    {
      ImGui::TextColored(active_color, "Command: %s (0x%02X) (%d ticks remaining)",
                         s_command_info[static_cast<u8>(s_state.command)].name, static_cast<u8>(s_state.command),
                         s_state.command_event.IsActive() ? s_state.command_event.GetTicksUntilNextExecution() : 0);
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
                         s_drive_state_names[static_cast<u8>(s_state.drive_state)],
                         s_state.drive_event.IsActive() ? s_state.drive_event.GetTicksUntilNextExecution() : 0);
    }

    ImGui::Text("Interrupt Enable Register: 0x%02X", s_state.interrupt_enable_register);
    ImGui::Text("Interrupt Flag Register: 0x%02X", s_state.interrupt_flag_register);

    if (HasPendingAsyncInterrupt())
    {
      ImGui::SameLine();
      ImGui::TextColored(inactive_color, " (0x%02X pending)", s_state.pending_async_interrupt);
    }
  }

  if (ImGui::CollapsingHeader("CD Audio", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (s_state.drive_state == DriveState::Reading && s_state.mode.xa_enable)
    {
      ImGui::TextColored(active_color, "Playing: XA-ADPCM (File %u | Channel %u | %s | %s | %s)",
                         s_state.xa_current_file_number, s_state.xa_current_channel_number,
                         s_state.xa_current_codinginfo.IsStereo() ? "Stereo" : "Mono",
                         s_state.xa_current_codinginfo.Is8BitADPCM() ? "8-bit" : "4-bit",
                         s_state.xa_current_codinginfo.IsHalfSampleRate() ? "18900hz" : "37800hz");
    }
    else if (s_state.drive_state == DriveState::Playing)
    {
      ImGui::TextColored(active_color, "Playing: CDDA (Track %x)", s_state.last_subq.track_number_bcd);
    }
    else
    {
      ImGui::TextColored(inactive_color, "Playing: Inactive");
    }

    ImGui::TextColored(s_state.muted ? inactive_color : active_color, "Muted: %s", s_state.muted ? "Yes" : "No");
    ImGui::Text("Left Output: Left Channel=%02X (%u%%), Right Channel=%02X (%u%%)",
                s_state.cd_audio_volume_matrix[0][0], ZeroExtend32(s_state.cd_audio_volume_matrix[0][0]) * 100 / 0x80,
                s_state.cd_audio_volume_matrix[1][0], ZeroExtend32(s_state.cd_audio_volume_matrix[1][0]) * 100 / 0x80);
    ImGui::Text("Right Output: Left Channel=%02X (%u%%), Right Channel=%02X (%u%%)",
                s_state.cd_audio_volume_matrix[0][1], ZeroExtend32(s_state.cd_audio_volume_matrix[0][1]) * 100 / 0x80,
                s_state.cd_audio_volume_matrix[1][1], ZeroExtend32(s_state.cd_audio_volume_matrix[1][1]) * 100 / 0x80);

    ImGui::Text("Audio FIFO Size: %u frames", s_state.audio_fifo.GetSize());
  }
}
