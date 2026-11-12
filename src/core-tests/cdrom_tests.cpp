// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cdrom.h"
#include "core/cdrom.inl"
#include "core/cpu_core.h"
#include "core/interrupt_controller.h"
#include "core/settings.h"
#include "core/system.h"
#include "core/timing_event.h"

#include "util/cd_image.h"

#include "common/types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <vector>

namespace {

using RingBuffer = std::array<s16, CDROM::XA_RESAMPLE_RING_BUFFER_SIZE>;

// Original scalar implementations, which the vectorized versions in cdrom.inl must match exactly.
static s16 ReferenceZigZagInterpolate(const s16* ringbuf, u32 table_index, u32 p)
{
  static constexpr std::array<std::array<s16, 29>, 7> tables = {
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
}

static s16 ReferenceInterpolate18900(const s16* ringbuf, u32 table_index, u32 p)
{
  static constexpr std::array<std::array<s16, 25>, 7> tables = {{
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
}

// Compares both filters against their reference for every table and ring buffer position.
static void ExpectMatchesReference(const RingBuffer& ringbuf)
{
  for (u32 table_index = 0; table_index < CDROM::XA_RESAMPLE_NUM_TABLES; table_index++)
  {
    for (u32 p = 0; p < CDROM::XA_RESAMPLE_RING_BUFFER_SIZE; p++)
    {
      SCOPED_TRACE(testing::Message() << "table_index=" << table_index << " p=" << p);
      EXPECT_EQ(CDROM::XAZigZagInterpolate(ringbuf.data(), table_index, p),
                ReferenceZigZagInterpolate(ringbuf.data(), table_index, p));
      EXPECT_EQ(CDROM::XAInterpolate18900(ringbuf.data(), table_index, p),
                ReferenceInterpolate18900(ringbuf.data(), table_index, p));
    }
  }
}

} // namespace

TEST(CDROM, XAResampleMatchesReferenceRandom)
{
  std::mt19937 rng(0x58414450u);
  std::uniform_int_distribution<s32> dist(std::numeric_limits<s16>::min(), std::numeric_limits<s16>::max());

  for (u32 iteration = 0; iteration < 1000; iteration++)
  {
    RingBuffer ringbuf;
    for (s16& sample : ringbuf)
      sample = static_cast<s16>(dist(rng));

    SCOPED_TRACE(testing::Message() << "iteration=" << iteration);
    ExpectMatchesReference(ringbuf);
  }
}

// Full-scale input, to cover the output clamp and the largest possible products.
TEST(CDROM, XAResampleMatchesReferenceFullScale)
{
  static constexpr s16 min = std::numeric_limits<s16>::min();
  static constexpr s16 max = std::numeric_limits<s16>::max();

  RingBuffer ringbuf;
  ringbuf.fill(min);
  ExpectMatchesReference(ringbuf);
  ringbuf.fill(max);
  ExpectMatchesReference(ringbuf);

  // Alternating signs at each period that fits in the ring buffer.
  for (u32 period = 1; period <= CDROM::XA_RESAMPLE_RING_BUFFER_SIZE / 2; period *= 2)
  {
    for (u32 i = 0; i < CDROM::XA_RESAMPLE_RING_BUFFER_SIZE; i++)
      ringbuf[i] = ((i / period) & 1) ? min : max;

    SCOPED_TRACE(testing::Message() << "period=" << period);
    ExpectMatchesReference(ringbuf);
  }
}

// A single non-zero sample picks out one coefficient at a time, catching any misaligned or missing tap.
TEST(CDROM, XAResampleMatchesReferenceImpulse)
{
  for (u32 i = 0; i < CDROM::XA_RESAMPLE_RING_BUFFER_SIZE; i++)
  {
    for (const s16 value : {std::numeric_limits<s16>::min(), std::numeric_limits<s16>::max()})
    {
      RingBuffer ringbuf = {};
      ringbuf[i] = value;

      SCOPED_TRACE(testing::Message() << "impulse=" << i << " value=" << value);
      ExpectMatchesReference(ringbuf);
    }
  }
}

namespace {

/// Single data track disc, with the 2 second pregap and the sectors themselves synthesized.
class TestDiscImage final : public CDImage
{
public:
  explicit TestDiscImage(u32 data_sectors)
  {
    static constexpr u32 PREGAP_FRAMES = 2 * FRAMES_PER_SECOND;

    SubChannelQ::Control control = {};
    control.data = true;

    Index pregap = {};
    pregap.track_number = 1;
    pregap.index_number = 0;
    pregap.start_lba_in_track = static_cast<LBA>(-static_cast<s32>(PREGAP_FRAMES));
    pregap.length = PREGAP_FRAMES;
    pregap.mode = TrackMode::Mode2Form1;
    pregap.control = control;
    pregap.is_pregap = true;
    m_indices.push_back(pregap);

    Index data = {};
    data.file_sector_size = DATA_SECTOR_SIZE;
    data.start_lba_on_disc = PREGAP_FRAMES;
    data.track_number = 1;
    data.index_number = 1;
    data.length = data_sectors;
    data.mode = TrackMode::Mode2Form1;
    data.control = control;
    m_indices.push_back(data);

    m_tracks.push_back(
      Track{1, PREGAP_FRAMES, 0, PREGAP_FRAMES + data_sectors, TrackMode::Mode2Form1, SubchannelMode::None, control});
    m_lba_count = PREGAP_FRAMES + data_sectors;
    AddLeadOutIndex();
  }

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override
  {
    std::memset(buffer, 0, index.file_sector_size);
    return true;
  }
};

/// Drives the CDROM through its registers only, the way a game does with interrupts masked.
class CDROMControllerTest : public ::testing::Test
{
protected:
  enum : u8
  {
    INT_DATA_READY = 1,
    INT_COMPLETE = 2,
    INT_ACK = 3,
    INT_ERROR = 5,

    STAT_MOTOR_ON = 0x02,
    STAT_READING = 0x20,
    STAT_SEEKING = 0x40,
  };

  /// One interrupt, as seen by the host after acknowledging it.
  struct Response
  {
    u8 cause;
    std::vector<u8> bytes;
    GlobalTicks time; // relative to the last command write
  };

  static void SetUpTestSuite()
  {
    TimingEvents::Initialize();
    System::UpdateOverclock();
    DeactivateAllEvents();
  }

  void SetUp() override
  {
    m_old_settings = g_settings;
    g_settings.cdrom_readahead_sectors = 0;
    g_settings.cdrom_read_speedup = 1;
    g_settings.cdrom_seek_speedup = 1;
    g_settings.cdrom_auto_disc_change = false;
    g_settings.cdrom_mechacon_version = CDROMMechaconVersion::VC1A;
    CPU::g_state.cop0_regs.sr.bits = 0;
    CPU::g_state.pending_ticks = 0;
    DeactivateAllEvents();
    TimingEvents::Reset();
    InterruptController::Reset();

    // Time can't advance without something to count down to.
    m_idle_event.Activate();
    ASSERT_TRUE(CDROM::Initialize(nullptr, DiscRegion::NonPS1, {}, {}, {}, nullptr));

    std::unique_ptr<CDImage> image = std::make_unique<TestDiscImage>(70 * CDImage::FRAMES_PER_MINUTE);
    ASSERT_TRUE(CDROM::InsertMedia(image, DiscRegion::NTSC_U, {}, {}, {}, nullptr));

    // Let the motor spin up, and clear the shell open bit.
    Run(Seconds(2.0));
    ASSERT_EQ(Command(0x01).cause, INT_ACK);
  }

  void TearDown() override
  {
    CDROM::Shutdown();
    DeactivateAllEvents();
    g_settings = m_old_settings;
  }

  static void DeactivateAllEvents()
  {
    while (TimingEvent* event = *TimingEvents::GetHeadEventPtr())
      event->Deactivate();
  }

  static GlobalTicks Seconds(double seconds)
  {
    return static_cast<GlobalTicks>(seconds * static_cast<double>(System::MASTER_CLOCK));
  }

  static GlobalTicks Milliseconds(double ms) { return Seconds(ms / 1000.0); }

  static double ToMilliseconds(GlobalTicks ticks)
  {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(System::MASTER_CLOCK);
  }

  static GlobalTicks Now() { return TimingEvents::GetGlobalTickCounter() + CPU::GetPendingTicks(); }

  /// Advances emulated time without any register accesses.
  static void Run(GlobalTicks ticks)
  {
    const GlobalTicks end = Now() + ticks;
    while (Now() < end)
    {
      const GlobalTicks remaining = end - Now();
      const TickCount until_event = std::max<TickCount>(CPU::g_state.downcount - CPU::GetPendingTicks(), 1);
      CPU::AddPendingTicks(static_cast<TickCount>(std::min<GlobalTicks>(remaining, until_event)));
      if (CPU::GetPendingTicks() >= CPU::g_state.downcount)
        TimingEvents::RunEvents();
    }
  }

  static u8 ReadCause()
  {
    CDROM::WriteRegister(0, 1);
    return CDROM::ReadRegister(3) & 0x1F;
  }

  void WriteCommand(u8 command, std::initializer_list<u8> params = {})
  {
    CDROM::WriteRegister(0, 0);
    for (const u8 param : params)
      CDROM::WriteRegister(2, param);
    CDROM::WriteRegister(1, command);
    m_command_time = Now();
  }

  /// Polls for an interrupt for up to the specified time, then acknowledges it before reading the response.
  std::optional<Response> WaitForInterrupt(GlobalTicks timeout)
  {
    static constexpr GlobalTicks POLL_TICKS = 300; // ~9 us

    const GlobalTicks end = Now() + timeout;
    while (Now() < end)
    {
      const u8 cause = ReadCause() & 0x07;
      if (cause != 0)
      {
        Response response{cause, {}, Now() - m_command_time};
        CDROM::WriteRegister(0, 1);
        CDROM::WriteRegister(3, 0x07);
        CDROM::WriteRegister(0, 0);
        while (CDROM::ReadRegister(0) & 0x20)
          response.bytes.push_back(CDROM::ReadRegister(1));
        return response;
      }

      Run(POLL_TICKS);
    }

    return std::nullopt;
  }

  /// Writes a command and returns its first response.
  Response Command(u8 command, std::initializer_list<u8> params = {})
  {
    WriteCommand(command, params);
    std::optional<Response> response = WaitForInterrupt(Milliseconds(50));
    EXPECT_TRUE(response.has_value()) << "no response to command " << static_cast<u32>(command);
    return response.value_or(Response{0, {0}, 0});
  }

  /// Seeks to the specified position (BCD) and reads the given number of sectors at single speed.
  void ReadSectors(std::initializer_list<u8> msf, u32 count)
  {
    ASSERT_EQ(Command(0x0E, {0x00}).cause, INT_ACK);
    ASSERT_EQ(Command(0x02, msf).cause, INT_ACK);
    ASSERT_EQ(Command(0x06).cause, INT_ACK);
    for (u32 i = 0; i < count; i++)
    {
      const std::optional<Response> sector = WaitForInterrupt(Seconds(2.0));
      ASSERT_TRUE(sector.has_value());
      ASSERT_EQ(sector->cause, INT_DATA_READY);
    }
  }

  Settings m_old_settings;
  TimingEvent m_idle_event{"Test Idle", 1000000, 1000000, [](void*, TickCount) {}, nullptr};
  GlobalTicks m_command_time = 0;
  GlobalTicks m_last_duration = 0;
};

} // namespace

TEST_F(CDROMControllerTest, StopCompletes)
{
  const Response ack = Command(0x08);
  EXPECT_EQ(ack.cause, INT_ACK);
  const std::optional<Response> complete = WaitForInterrupt(Seconds(1.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes.at(0), 0x00);
  EXPECT_EQ(Command(0x01).bytes.at(0), 0x00);
}

// Hardware: Stop's acknowledge no longer has the read bit, but the motor is still on.
TEST_F(CDROMControllerTest, StopWhileReadingAcknowledgesWithoutReadBit)
{
  ReadSectors({0x30, 0x22, 0x57}, 16);
  const Response ack = Command(0x08);
  EXPECT_EQ(ack.cause, INT_ACK);
  EXPECT_EQ(ack.bytes.at(0), STAT_MOTOR_ON);
}

// Hardware: GetID's INT2 arrives right after its acknowledge is cleared.
TEST_F(CDROMControllerTest, GetIDCompletesAfterAcknowledge)
{
  ASSERT_EQ(Command(0x1A).cause, INT_ACK);
  const std::optional<Response> complete = WaitForInterrupt(Milliseconds(0.2));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes, (std::vector<u8>{STAT_MOTOR_ON, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'}));
}

TEST_F(CDROMControllerTest, SeekCompletes)
{
  ASSERT_EQ(Command(0x02, {0x10, 0x00, 0x00}).cause, INT_ACK);
  ASSERT_EQ(Command(0x15).cause, INT_ACK);
  const std::optional<Response> complete = WaitForInterrupt(Seconds(3.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes.at(0), STAT_MOTOR_ON);
}

// Hardware: a GetStat during a long seek means the seek's INT2 is never sent, but the seek still finishes.
TEST_F(CDROMControllerTest, CommandDuringSeekSuppressesComplete)
{
  ASSERT_EQ(Command(0x02, {0x60, 0x00, 0x00}).cause, INT_ACK);
  ASSERT_EQ(Command(0x15).cause, INT_ACK);
  Run(Milliseconds(20));

  const Response stat = Command(0x01);
  EXPECT_EQ(stat.cause, INT_ACK);
  EXPECT_EQ(stat.bytes.at(0), STAT_SEEKING | STAT_MOTOR_ON);

  EXPECT_FALSE(WaitForInterrupt(Seconds(3.0)).has_value());
  EXPECT_EQ(Command(0x01).bytes.at(0), STAT_MOTOR_ON);
}

TEST_F(CDROMControllerTest, PauseCompletes)
{
  ReadSectors({0x10, 0x00, 0x00}, 4);
  ASSERT_EQ(Command(0x09).cause, INT_ACK);
  const std::optional<Response> complete = WaitForInterrupt(Seconds(1.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes.at(0), STAT_MOTOR_ON);
}

// Hardware: Pause still acknowledges with the read bit, and a GetStat straight after it drops Pause's INT2.
TEST_F(CDROMControllerTest, CommandAfterPauseSuppressesComplete)
{
  ReadSectors({0x10, 0x00, 0x00}, 4);
  const Response ack = Command(0x09);
  EXPECT_EQ(ack.cause, INT_ACK);
  EXPECT_EQ(ack.bytes.at(0), STAT_READING | STAT_MOTOR_ON);

  EXPECT_EQ(Command(0x01).cause, INT_ACK);
  EXPECT_FALSE(WaitForInterrupt(Seconds(3.0)).has_value());
  EXPECT_EQ(Command(0x01).bytes.at(0), STAT_MOTOR_ON);
}

// Hardware: a GetStat 8 frames after Stop drops Stop's INT2, the motor still stops.
TEST_F(CDROMControllerTest, CommandDuringStopSuppressesComplete)
{
  ReadSectors({0x30, 0x22, 0x57}, 16);
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  Run(Milliseconds(130));
  EXPECT_EQ(Command(0x01).cause, INT_ACK);
  EXPECT_FALSE(WaitForInterrupt(Seconds(3.0)).has_value());
  EXPECT_EQ(Command(0x01).bytes.at(0), 0x00);
}

// Hardware: a command with the wrong number of parameters still gets an error, verified on vC0 only.
TEST_F(CDROMControllerTest, RejectedCommandDuringStop)
{
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  Run(Milliseconds(130));
  const Response error = Command(0x02);
  EXPECT_EQ(error.cause, INT_ERROR);
  EXPECT_EQ(error.bytes.at(1), 0x20);
}

// Hardware: a completion that is already waiting for the host survives a new command, and comes out first.
TEST_F(CDROMControllerTest, QueuedCompleteSurvivesCommand)
{
  WriteCommand(0x08);
  Run(Seconds(1.5));

  const std::optional<Response> ack = WaitForInterrupt(Milliseconds(1));
  ASSERT_TRUE(ack.has_value());
  EXPECT_EQ(ack->cause, INT_ACK);

  WriteCommand(0x01);
  const std::optional<Response> complete = WaitForInterrupt(Milliseconds(50));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes.at(0), 0x00);

  const std::optional<Response> stat = WaitForInterrupt(Milliseconds(50));
  ASSERT_TRUE(stat.has_value());
  EXPECT_EQ(stat->cause, INT_ACK);
}

// Hardware: the motor still reports as on while braking, GetTN returns the cached TOC, and the motor still stops.
TEST_F(CDROMControllerTest, StopReportsMotorOnWhileBraking)
{
  ReadSectors({0x30, 0x22, 0x57}, 16);
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  Run(Milliseconds(125));

  const Response tn = Command(0x13);
  EXPECT_EQ(tn.cause, INT_ACK);
  EXPECT_EQ(tn.bytes, (std::vector<u8>{STAT_MOTOR_ON, 0x01, 0x01}));

  EXPECT_FALSE(WaitForInterrupt(Seconds(3.0)).has_value());
  EXPECT_EQ(Command(0x01).bytes.at(0), 0x00);
}

TEST_F(CDROMControllerTest, StopWhenStoppedCompletesQuickly)
{
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  ASSERT_TRUE(WaitForInterrupt(Seconds(1.0)).has_value());

  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  const std::optional<Response> complete = WaitForInterrupt(Milliseconds(5));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes.at(0), 0x00);
}

// A second Stop while braking makes the completion eligible again, without restarting the braking.
TEST_F(CDROMControllerTest, StopWhileBrakingCompletes)
{
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  Run(Milliseconds(100));
  ASSERT_EQ(Command(0x01).cause, INT_ACK);
  ASSERT_EQ(Command(0x08).cause, INT_ACK);

  const std::optional<Response> complete = WaitForInterrupt(Seconds(1.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes.at(0), 0x00);
  EXPECT_LT(ToMilliseconds(complete->time), 400.0);
}

// Pause doesn't interrupt the braking.
TEST_F(CDROMControllerTest, PauseWhileBrakingStillStops)
{
  ReadSectors({0x10, 0x00, 0x00}, 4);
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  Run(Milliseconds(50));
  ASSERT_EQ(Command(0x09).cause, INT_ACK);
  const std::optional<Response> pause = WaitForInterrupt(Seconds(1.0));
  ASSERT_TRUE(pause.has_value());
  EXPECT_EQ(pause->cause, INT_COMPLETE);

  Run(Seconds(1.0));
  EXPECT_EQ(Command(0x01).bytes.at(0), 0x00);
}

// A seek while braking waits for the motor to stop, then spins it up again.
TEST_F(CDROMControllerTest, SeekWhileBrakingSpinsUp)
{
  ReadSectors({0x10, 0x00, 0x00}, 4);
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  Run(Milliseconds(50));
  ASSERT_EQ(Command(0x02, {0x10, 0x00, 0x00}).cause, INT_ACK);
  ASSERT_EQ(Command(0x15).cause, INT_ACK);

  const std::optional<Response> complete = WaitForInterrupt(Seconds(5.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes.at(0), STAT_MOTOR_ON);
  EXPECT_GT(ToMilliseconds(complete->time), 1000.0);
}

// Hardware: Init with the motor stopped acknowledges as stopped, and completes ~1.82-1.85 s later with the motor on.
TEST_F(CDROMControllerTest, InitFromStopped)
{
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  ASSERT_TRUE(WaitForInterrupt(Seconds(1.0)).has_value());

  const Response ack = Command(0x0A);
  EXPECT_EQ(ack.cause, INT_ACK);
  EXPECT_EQ(ack.bytes.at(0), 0x00);

  const std::optional<Response> complete = WaitForInterrupt(Seconds(3.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes.at(0), STAT_MOTOR_ON);
  EXPECT_GT(ToMilliseconds(complete->time), 1800.0);
  EXPECT_LT(ToMilliseconds(complete->time), 1900.0);
}

// Init again during the spin-up from stopped completes when the spin-up does, not straight away.
TEST_F(CDROMControllerTest, InitWhileSpinningUp)
{
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  ASSERT_TRUE(WaitForInterrupt(Seconds(1.0)).has_value());
  ASSERT_EQ(Command(0x0A).cause, INT_ACK);
  const GlobalTicks init_time = m_command_time;
  Run(Milliseconds(50));
  ASSERT_EQ(Command(0x01).cause, INT_ACK);

  const Response ack = Command(0x0A);
  EXPECT_EQ(ack.cause, INT_ACK);
  EXPECT_EQ(ack.bytes.at(0), 0x00);

  const std::optional<Response> complete = WaitForInterrupt(Seconds(3.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes.at(0), STAT_MOTOR_ON);
  EXPECT_GT(ToMilliseconds(Now() - init_time), 1800.0);
  EXPECT_LT(ToMilliseconds(Now() - init_time), 1900.0);
  EXPECT_FALSE(WaitForInterrupt(Seconds(3.0)).has_value());
}

// A speed change set while stopped leaves the motor stopped, so Init still has to spin it up.
TEST_F(CDROMControllerTest, InitFromStoppedDuringSpeedChange)
{
  ASSERT_EQ(Command(0x0E, {0x80}).cause, INT_ACK);
  Run(Seconds(1.0));
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  ASSERT_TRUE(WaitForInterrupt(Seconds(1.0)).has_value());
  ASSERT_EQ(Command(0x0E, {0x00}).cause, INT_ACK);

  const Response ack = Command(0x0A);
  EXPECT_EQ(ack.cause, INT_ACK);
  EXPECT_EQ(ack.bytes.at(0), 0x00);

  const std::optional<Response> complete = WaitForInterrupt(Seconds(3.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes.at(0), STAT_MOTOR_ON);
  EXPECT_GT(ToMilliseconds(complete->time), 1800.0);
  EXPECT_LT(ToMilliseconds(complete->time), 1900.0);
}

// Hardware: the game's sequence, GetTN and Init 8 frames after Stop. Init acknowledges with the motor on, waits for
// braking to finish, and there is only one INT2, Init's.
TEST_F(CDROMControllerTest, InitWhileBraking)
{
  ReadSectors({0x30, 0x22, 0x57}, 16);
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  const GlobalTicks stop_time = m_command_time;
  Run(Milliseconds(125));

  EXPECT_EQ(Command(0x13).bytes.at(0), STAT_MOTOR_ON);
  const Response ack = Command(0x0A);
  EXPECT_EQ(ack.cause, INT_ACK);
  EXPECT_EQ(ack.bytes.at(0), STAT_MOTOR_ON);

  const std::optional<Response> complete = WaitForInterrupt(Seconds(3.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes.at(0), STAT_MOTOR_ON);
  EXPECT_GT(ToMilliseconds(complete->time), 1900.0);
  EXPECT_GT(ToMilliseconds(Now() - stop_time), 2000.0);

  EXPECT_FALSE(WaitForInterrupt(Seconds(3.0)).has_value());
  EXPECT_EQ(Command(0x01).bytes.at(0), STAT_MOTOR_ON);
}

// Init with the motor running is unchanged and fast.
TEST_F(CDROMControllerTest, InitWithMotorOn)
{
  ASSERT_EQ(Command(0x0A).cause, INT_ACK);
  const std::optional<Response> complete = WaitForInterrupt(Seconds(1.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);
  EXPECT_EQ(complete->bytes.at(0), STAT_MOTOR_ON);
  EXPECT_LT(ToMilliseconds(complete->time), 200.0);
}

// Hardware: Stop after reading at 30:24 at single speed completes in 258-298ms, median 279ms. Published averages from
// an unknown position are 409ms (single) and 763ms (double).
TEST_F(CDROMControllerTest, StopTimeDependsOnPosition)
{
  ReadSectors({0x30, 0x24, 0x50}, 16);
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  std::optional<Response> complete = WaitForInterrupt(Seconds(1.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_NEAR(ToMilliseconds(complete->time), 279.0, 5.0);

  ReadSectors({0x00, 0x02, 0x00}, 16);
  ASSERT_EQ(Command(0x08).cause, INT_ACK);
  complete = WaitForInterrupt(Seconds(1.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_NEAR(ToMilliseconds(complete->time), 409.0, 5.0);
}

TEST_F(CDROMControllerTest, ReadTOCOnlyOnVC1AndLater)
{
  ASSERT_EQ(Command(0x1E).cause, INT_ACK);
  const std::optional<Response> complete = WaitForInterrupt(Seconds(2.0));
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->cause, INT_COMPLETE);

  // Hardware: vC0 rejects it as an invalid command.
  g_settings.cdrom_mechacon_version = CDROMMechaconVersion::VC0B;
  const Response error = Command(0x1E);
  EXPECT_EQ(error.cause, INT_ERROR);
  EXPECT_EQ(error.bytes, (std::vector<u8>{0x03, 0x40}));
}

// Hardware: setting double speed while stopped costs nothing over the spin-up and seek.
TEST_F(CDROMControllerTest, DoubleSpeedWhileStoppedIsFree)
{
  const auto seek_from_stopped = [this](u8 mode) {
    ASSERT_EQ(Command(0x08).cause, INT_ACK);
    ASSERT_TRUE(WaitForInterrupt(Seconds(1.0)).has_value());
    ASSERT_EQ(Command(0x0E, {mode}).cause, INT_ACK);
    ASSERT_EQ(Command(0x02, {0x10, 0x00, 0x00}).cause, INT_ACK);
    WriteCommand(0x15);
  };

  // Reference, already at double speed.
  ASSERT_EQ(Command(0x0E, {0x80}).cause, INT_ACK);
  Run(Seconds(1.0));
  seek_from_stopped(0x80);
  ASSERT_EQ(WaitForInterrupt(Milliseconds(50))->cause, INT_ACK);
  const std::optional<Response> reference = WaitForInterrupt(Seconds(5.0));
  ASSERT_TRUE(reference.has_value());

  ASSERT_EQ(Command(0x0E, {0x00}).cause, INT_ACK);
  Run(Seconds(1.0));
  seek_from_stopped(0x80);
  ASSERT_EQ(WaitForInterrupt(Milliseconds(50))->cause, INT_ACK);
  const std::optional<Response> changed = WaitForInterrupt(Seconds(5.0));
  ASSERT_TRUE(changed.has_value());
  EXPECT_NEAR(ToMilliseconds(changed->time), ToMilliseconds(reference->time), 50.0);
}

// Hardware: setting single speed while stopped at double speed adds ~1.1 s over the spin-up and seek. The speed change
// is added on top, not overlapped.
TEST_F(CDROMControllerTest, SingleSpeedWhileStoppedAddsChange)
{
  const auto seek_from_stopped = [this]() {
    ASSERT_EQ(Command(0x08).cause, INT_ACK);
    ASSERT_TRUE(WaitForInterrupt(Seconds(1.0)).has_value());
    ASSERT_EQ(Command(0x0E, {0x00}).cause, INT_ACK);
    ASSERT_EQ(Command(0x02, {0x10, 0x00, 0x00}).cause, INT_ACK);
    WriteCommand(0x15);
  };

  // Reference, already at single speed.
  seek_from_stopped();
  ASSERT_EQ(WaitForInterrupt(Milliseconds(50))->cause, INT_ACK);
  const std::optional<Response> reference = WaitForInterrupt(Seconds(5.0));
  ASSERT_TRUE(reference.has_value());

  ASSERT_EQ(Command(0x0E, {0x80}).cause, INT_ACK);
  Run(Seconds(1.0));
  seek_from_stopped();
  ASSERT_EQ(WaitForInterrupt(Milliseconds(50))->cause, INT_ACK);
  const std::optional<Response> changed = WaitForInterrupt(Seconds(5.0));
  ASSERT_TRUE(changed.has_value());
  EXPECT_GT(ToMilliseconds(changed->time) - ToMilliseconds(reference->time), 700.0);
}

// Hardware: a short seek written straight after an idle speed change finishes with the change, at ~550ms for single to
// double, ~650-1160ms (typically 820-850ms) for double to single.
TEST_F(CDROMControllerTest, SeekOverlapsSpeedChange)
{
  const auto seek_after_speed_change = [this](u8 mode, u8 frame) {
    ASSERT_EQ(Command(0x0E, {mode}).cause, INT_ACK);
    const GlobalTicks setmode_time = m_command_time;
    ASSERT_EQ(Command(0x02, {0x10, 0x00, frame}).cause, INT_ACK);
    ASSERT_EQ(Command(0x15).cause, INT_ACK);
    const std::optional<Response> complete = WaitForInterrupt(Seconds(3.0));
    ASSERT_TRUE(complete.has_value());
    EXPECT_EQ(complete->cause, INT_COMPLETE);
    m_last_duration = Now() - setmode_time;
  };

  ASSERT_EQ(Command(0x02, {0x10, 0x00, 0x00}).cause, INT_ACK);
  ASSERT_EQ(Command(0x15).cause, INT_ACK);
  ASSERT_TRUE(WaitForInterrupt(Seconds(3.0)).has_value());

  seek_after_speed_change(0x80, 0x10);
  EXPECT_NEAR(ToMilliseconds(m_last_duration), 550.0, 15.0);
  seek_after_speed_change(0x00, 0x20);
  EXPECT_NEAR(ToMilliseconds(m_last_duration), 830.0, 15.0);
}

// Hardware: a speed change during a long seek overlaps with it instead of adding the full change time, and the
// Setmode replaces the seek's INT2. The seek's end is found by polling GetStat, as the hardware test did.
TEST_F(CDROMControllerTest, SpeedChangeOverlapsSeek)
{
  const auto seek_with_setmode = [this](u8 mode, u8 frame) {
    ASSERT_EQ(Command(0x02, {0x00, 0x02, 0x00}).cause, INT_ACK);
    ASSERT_EQ(Command(0x15).cause, INT_ACK);
    ASSERT_TRUE(WaitForInterrupt(Seconds(3.0)).has_value());

    ASSERT_EQ(Command(0x02, {0x60, 0x00, frame}).cause, INT_ACK);
    ASSERT_EQ(Command(0x15).cause, INT_ACK);
    const GlobalTicks seek_time = m_command_time;
    Run(Milliseconds(20));
    EXPECT_EQ(Command(0x0E, {mode}).bytes.at(0), STAT_SEEKING | STAT_MOTOR_ON);

    while (Command(0x01).bytes.at(0) & STAT_SEEKING)
      Run(Milliseconds(10));
    m_last_duration = Now() - seek_time;
  };

  seek_with_setmode(0x00, 0x00);
  const double plain_ms = ToMilliseconds(m_last_duration);
  seek_with_setmode(0x80, 0x10);
  EXPECT_NEAR(ToMilliseconds(m_last_duration), plain_ms + 160.0, 30.0);
}

// Hardware: switching the speed back during a change doesn't cancel it. A seek straight after completes 0.93-1.26s
// after the first Setmode.
TEST_F(CDROMControllerTest, SpeedChangeBackDoesNotCancel)
{
  const auto toggle_then_seek = [this](u8 first_mode, u8 second_mode, double delay_ms, u8 frame) {
    ASSERT_EQ(Command(0x0E, {first_mode}).cause, INT_ACK);
    const GlobalTicks setmode_time = m_command_time;
    Run(Milliseconds(delay_ms));
    ASSERT_EQ(Command(0x0E, {second_mode}).cause, INT_ACK);
    ASSERT_EQ(Command(0x02, {0x10, 0x00, frame}).cause, INT_ACK);
    ASSERT_EQ(Command(0x15).cause, INT_ACK);
    const std::optional<Response> complete = WaitForInterrupt(Seconds(3.0));
    ASSERT_TRUE(complete.has_value());
    EXPECT_EQ(complete->cause, INT_COMPLETE);
    m_last_duration = Now() - setmode_time;
    Run(Seconds(1.0));
  };

  ASSERT_EQ(Command(0x02, {0x10, 0x00, 0x00}).cause, INT_ACK);
  ASSERT_EQ(Command(0x15).cause, INT_ACK);
  ASSERT_TRUE(WaitForInterrupt(Seconds(3.0)).has_value());

  u8 frame = 0x10;
  for (const double delay_ms : {50.0, 150.0, 300.0})
  {
    SCOPED_TRACE(testing::Message() << "delay=" << delay_ms);
    toggle_then_seek(0x80, 0x00, delay_ms, frame);
    EXPECT_GE(ToMilliseconds(m_last_duration), 930.0);
    EXPECT_LE(ToMilliseconds(m_last_duration), 1260.0);
    frame += 0x10;

    // Start at double speed for the other direction.
    ASSERT_EQ(Command(0x0E, {0x80}).cause, INT_ACK);
    Run(Seconds(1.5));
    toggle_then_seek(0x00, 0x80, delay_ms, frame);
    EXPECT_GE(ToMilliseconds(m_last_duration), 930.0);
    EXPECT_LE(ToMilliseconds(m_last_duration), 1260.0);
    frame += 0x10;
    ASSERT_EQ(Command(0x0E, {0x00}).cause, INT_ACK);
    Run(Seconds(1.5));
  }
}
