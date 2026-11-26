// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/analog_controller.h"
#include "core/controller.h"
#include "core/cpu_core.h"
#include "core/digital_controller.h"
#include "core/interrupt_controller.h"
#include "core/memory_card.h"
#include "core/multitap.h"
#include "core/pad.h"
#include "core/system.h"
#include "core/timing_event.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

namespace {

enum : u32
{
  JOY_DATA = 0x00,
  JOY_STAT = 0x04,
  JOY_MODE = 0x08,
  JOY_CTRL = 0x0A,
  JOY_BAUD = 0x0E,

  STAT_TXRDY = (1u << 0),
  STAT_RXFIFONEMPTY = (1u << 1),
  STAT_TXDONE = (1u << 2),
  STAT_ACKINPUT = (1u << 7),
  STAT_INTR = (1u << 9),

  CTRL_TXEN = (1u << 0),
  CTRL_SELECT = (1u << 1),
  CTRL_ACK = (1u << 4),
  CTRL_RESET = (1u << 6),
  CTRL_TXINTEN = (1u << 10),
  CTRL_RXINTEN = (1u << 11),
  CTRL_ACKINTEN = (1u << 12),
  CTRL_SLOT2 = (1u << 13),

  PAD_IRQ_BIT = (1u << static_cast<u32>(InterruptController::IRQ::PAD)),

  // What the BIOS uses: 8N1, MUL1, 0x88 reload.
  BIOS_JOY_MODE = 0x000D,
  BIOS_JOY_BAUD = 0x0088,

  TRANSFER_TICKS = BIOS_JOY_BAUD * 8,
  CONTROLLER_ACK_TICKS = 450,
  MEMORY_CARD_ACK_TICKS = 170,

  // Long enough for any ACK to have arrived.
  ACK_WINDOW = CONTROLLER_ACK_TICKS + 100,
  NOT_REACHED = 0xFFFFFFFFu,
};

const std::vector<u8> POLL = {0x01, 0x42, 0x00, 0x00, 0x00};
static constexpr u16 NO_BUTTONS = 0xFFFF;

const std::vector<u8> ANALOG_CONFIG_REPLY = {0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const std::vector<u8> GET_ID_REPLY = {0xFF, 0x08, 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80};

// Multitap all-controllers mode: what's sent and received for each sub-port.
using Block = std::vector<u8>;
const Block POLL_BLOCK = {0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const Block EMPTY_BLOCK = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static std::vector<u8> GetIDCommand(u8 first)
{
  return {first, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
}

static Block DigitalBlock(u16 buttons)
{
  return {0x41, 0x5A, Truncate8(buttons), Truncate8(buttons >> 8), 0xFF, 0xFF, 0xFF, 0xFF};
}

static Block AnalogBlock(u16 buttons)
{
  return {0x73, 0x5A, Truncate8(buttons), Truncate8(buttons >> 8), 0x7F, 0x7F, 0x7F, 0x7F};
}

static std::vector<u8> AllControllersCommand(u8 first, u8 request, const std::array<Block, 4>& blocks)
{
  std::vector<u8> ret = {first, 0x42, request};
  for (const Block& block : blocks)
    ret.insert(ret.end(), block.begin(), block.end());
  return ret;
}

static std::vector<u8> AllControllersReply(const std::array<Block, 4>& blocks)
{
  std::vector<u8> ret = {0xFF, 0x80, 0x5A};
  for (const Block& block : blocks)
    ret.insert(ret.end(), block.begin(), block.end());
  return ret;
}

static std::vector<u8> AllControllersPoll(u8 first = 0x01)
{
  return AllControllersCommand(first, 0x01, {POLL_BLOCK, POLL_BLOCK, POLL_BLOCK, POLL_BLOCK});
}

/// Fills a sector with a pattern, and returns the expected reply to reading it with ReadSectorCommand().
static std::vector<u8> PrepareReadSector(MemoryCard* card, u16 sector, u8 seed = 0)
{
  const u16 masked = sector & 0x3FF; // 1024 sectors
  u8* const data = card->GetData().data() + masked * MemoryCardImage::FRAME_SIZE;
  u8 checksum = Truncate8(masked >> 8) ^ Truncate8(masked);

  std::vector<u8> ret = {
    0xFF, 0x08, 0x5A, 0x5D, 0x00, Truncate8(sector >> 8), 0x5C, 0x5D, Truncate8(masked >> 8), Truncate8(masked)};
  for (u32 i = 0; i < MemoryCardImage::FRAME_SIZE; i++)
  {
    data[i] = static_cast<u8>(i * 5 + sector + seed);
    checksum ^= data[i];
    ret.push_back(data[i]);
  }
  ret.insert(ret.end(), {checksum, 0x47});
  return ret;
}

static std::vector<u8> ReadSectorCommand(u8 first, u16 sector)
{
  std::vector<u8> ret = {first, 0x52, 0x00, 0x00, Truncate8(sector >> 8), Truncate8(sector), 0x00, 0x00, 0x00, 0x00};
  ret.resize(ret.size() + MemoryCardImage::FRAME_SIZE + 2, 0x00);
  return ret;
}

class PadTest : public ::testing::Test
{
protected:
  struct Reply
  {
    u8 data;
    bool ack;
    u32 ack_ticks; // after RX, or NOT_REACHED
  };

  static void SetUpTestSuite()
  {
    TimingEvents::Initialize();
    System::UpdateOverclock();
    DeactivateAllEvents();
  }

  void SetUp() override
  {
    CPU::g_state.cop0_regs.sr.bits = 0;
    CPU::g_state.pending_ticks = 0;
    DeactivateAllEvents();
    TimingEvents::Reset();
    InterruptController::Reset();

    // Time can't advance without something to count down to.
    m_idle_event.Activate();

    Pad::Initialize();
    for (u32 i = 0; i < NUM_MULTITAPS; i++)
      Pad::GetMultitap(i)->SetEnable(false, i);

    Pad::WriteRegister(JOY_MODE, BIOS_JOY_MODE);
    Pad::WriteRegister(JOY_BAUD, BIOS_JOY_BAUD);
  }

  void TearDown() override
  {
    Pad::Shutdown();
    for (u32 i = 0; i < NUM_MULTITAPS; i++)
      Pad::GetMultitap(i)->SetEnable(false, i);
    DeactivateAllEvents();
  }

  static void DeactivateAllEvents()
  {
    while (TimingEvent* event = *TimingEvents::GetHeadEventPtr())
      event->Deactivate();
  }

  /// Runs the CPU for the specified number of ticks, without any register accesses.
  static void Run(u32 ticks)
  {
    for (u32 i = 0; i < ticks; i++)
    {
      CPU::AddPendingTicks(1);
      if (CPU::GetPendingTicks() >= CPU::g_state.downcount)
        TimingEvents::RunEvents();
    }
  }

  /// Runs one tick at a time until pred() is true, returning the number of ticks taken, or NOT_REACHED.
  template<typename F>
  static u32 RunUntil(const F& pred, u32 max_ticks)
  {
    for (u32 i = 1; i <= max_ticks; i++)
    {
      Run(1);
      if (pred())
        return i;
    }
    return NOT_REACHED;
  }

  /// Polls JOY_STAT like the BIOS does. Note this clears ACKINPUT.
  static bool RXReady() { return (Pad::ReadRegister(JOY_STAT) & STAT_RXFIFONEMPTY) != 0; }

  /// Checks the interrupt controller without touching the pad registers.
  static bool PadIRQPending() { return (InterruptController::ReadRegister(0x00) & PAD_IRQ_BIT) != 0; }

  static void PlugController(u32 index)
  {
    Pad::SetController(index, DigitalController::Create(index, ControllerType::DigitalController));
  }

  static void PlugAnalogController(u32 index) { Pad::SetController(index, AnalogController::Create(index)); }

  static MemoryCard* PlugMemoryCard(u32 index)
  {
    Pad::SetMemoryCard(index, MemoryCard::Create(index));
    return Pad::GetMemoryCard(index);
  }

  static void Press(u32 index, DigitalController::Button button)
  {
    Pad::GetController(index)->SetBindState(static_cast<u32>(button), 1.0f);
  }

  void Select(u32 slot, u32 extra_bits = CTRL_ACKINTEN)
  {
    m_ctrl = CTRL_TXEN | CTRL_SELECT | extra_bits | (slot ? CTRL_SLOT2 : 0u);
    Pad::WriteRegister(JOY_CTRL, m_ctrl);
  }

  static void Deselect() { Pad::WriteRegister(JOY_CTRL, 0); }

  /// Acknowledges the interrupt in both the pad and the interrupt controller, like the BIOS does.
  void AckIRQ()
  {
    Pad::WriteRegister(JOY_CTRL, m_ctrl | CTRL_ACK);
    InterruptController::WriteRegister(0x00, ~PAD_IRQ_BIT);
  }

  /// Sends one byte, waits for the reply and the ACK (if any).
  Reply Exchange(u8 value)
  {
    Pad::WriteRegister(JOY_DATA, value);
    EXPECT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS) << "TX of 0x" << std::hex << u32(value);

    Reply reply;
    reply.data = Truncate8(Pad::ReadRegister(JOY_DATA));
    reply.ack_ticks = RunUntil(PadIRQPending, ACK_WINDOW);
    reply.ack = (reply.ack_ticks != NOT_REACHED);
    AckIRQ();
    return reply;
  }

  /// Sends a whole command and returns the replies. Every byte except the last should ACK.
  /// The time from each RX to its ACK is left in m_ack_ticks.
  std::vector<u8> Command(const std::vector<u8>& tx)
  {
    std::vector<u8> rx;
    m_ack_ticks.clear();
    for (size_t i = 0; i < tx.size(); i++)
    {
      const Reply reply = Exchange(tx[i]);
      rx.push_back(reply.data);
      if (reply.ack)
        m_ack_ticks.push_back(reply.ack_ticks);

      const bool expected_ack = (i + 1) < tx.size();
      EXPECT_EQ(reply.ack, expected_ack) << "byte " << i;
      if (reply.ack != expected_ack)
        break;
    }

    EXPECT_FALSE(Pad::IsTransmitting());
    return rx;
  }

  /// Digital pad poll, with the replies for the given button state.
  static std::vector<u8> PollReply(u16 buttons)
  {
    return {0xFF, 0x41, 0x5A, Truncate8(buttons), Truncate8(buttons >> 8)};
  }

  /// Switches an analog controller to analog mode through config mode. first is 0x01, or the multitap sub-port.
  void EnterAnalogMode(u32 slot, u8 first)
  {
    Select(slot);
    EXPECT_EQ(Command({first, 0x43, 0x00, 0x01, 0x00}), (std::vector<u8>{0xFF, 0x41, 0x5A, 0xFF, 0xFF}));
    Deselect();
    Select(slot);
    EXPECT_EQ(Command({first, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00}), ANALOG_CONFIG_REPLY);
    Deselect();
    Select(slot);
    EXPECT_EQ(Command({first, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}), ANALOG_CONFIG_REPLY);
    Deselect();
  }

  TimingEvent m_idle_event{"Idle", 1000000, 1000000, [](void*, TickCount) {}, nullptr};
  u32 m_ctrl = 0;
  std::vector<u32> m_ack_ticks;
};

} // namespace

TEST_F(PadTest, TransferTiming_RX)
{
  PlugController(0);
  Select(0);

  // Eight bits of ((reload * factor) AND NOT 1), at least one cycle each. The factor is x1 for both 0 and 1.
  static constexpr struct
  {
    u32 mode;
    u32 baud;
    u32 ticks;
  } cases[] = {
    {BIOS_JOY_MODE, BIOS_JOY_BAUD, 0x88 * 8},
    {0x0C, 0x44, 0x44 * 8},
    {BIOS_JOY_MODE, 0x123, 0x122 * 8},
    {0x0E, 0x10, 0x10 * 16 * 8},
    {0x0F, 0x10, 0x10 * 64 * 8},
    {BIOS_JOY_MODE, 1, 8},
    {BIOS_JOY_MODE, 0, 8},
  };

  for (const auto& c : cases)
  {
    Deselect();
    Pad::WriteRegister(JOY_MODE, c.mode);
    Pad::WriteRegister(JOY_BAUD, c.baud);
    Select(0);

    Pad::WriteRegister(JOY_DATA, 0x01);
    EXPECT_EQ(Pad::ReadRegister(JOY_STAT) & (STAT_TXDONE | STAT_RXFIFONEMPTY), 0u);
    EXPECT_EQ(RunUntil(RXReady, c.ticks * 2), c.ticks) << "mode 0x" << std::hex << c.mode << " baud 0x" << c.baud;
    EXPECT_EQ(Truncate8(Pad::ReadRegister(JOY_DATA)), 0xFF);
    RunUntil(PadIRQPending, ACK_WINDOW);
    AckIRQ();
  }

  // Same time when nothing reads the registers, using the RX interrupt instead.
  Deselect();
  Pad::WriteRegister(JOY_MODE, BIOS_JOY_MODE);
  Pad::WriteRegister(JOY_BAUD, BIOS_JOY_BAUD);
  Select(0, CTRL_RXINTEN);
  Pad::WriteRegister(JOY_DATA, 0x01);
  EXPECT_EQ(RunUntil(PadIRQPending, TRANSFER_TICKS * 2), TRANSFER_TICKS);
}

TEST_F(PadTest, ACKTiming_Controller)
{
  PlugController(0);
  Select(0);

  Pad::WriteRegister(JOY_DATA, 0x01);
  ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
  EXPECT_EQ(Truncate8(Pad::ReadRegister(JOY_DATA)), 0xFF);
  EXPECT_FALSE(PadIRQPending());
  EXPECT_EQ(RunUntil(PadIRQPending, ACK_WINDOW), CONTROLLER_ACK_TICKS);

  // ACKINPUT is set with the interrupt, and cleared by reading JOY_STAT.
  const u32 stat = Pad::ReadRegister(JOY_STAT);
  EXPECT_NE(stat & STAT_ACKINPUT, 0u);
  EXPECT_NE(stat & STAT_INTR, 0u);
  EXPECT_NE(stat & STAT_TXDONE, 0u);
  EXPECT_EQ(Pad::ReadRegister(JOY_STAT) & STAT_ACKINPUT, 0u);
  EXPECT_FALSE(Pad::IsTransmitting());

  // Without ACKINTEN, ACKINPUT still gets set but there's no interrupt.
  AckIRQ();
  EXPECT_EQ(Pad::ReadRegister(JOY_STAT) & STAT_INTR, 0u);
  Select(0, 0);
  Pad::WriteRegister(JOY_DATA, 0x42);
  ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
  EXPECT_EQ(Truncate8(Pad::ReadRegister(JOY_DATA)), 0x41);
  Run(CONTROLLER_ACK_TICKS - 1);
  EXPECT_EQ(Pad::ReadRegister(JOY_STAT) & STAT_ACKINPUT, 0u);
  Run(1);
  EXPECT_NE(Pad::ReadRegister(JOY_STAT) & STAT_ACKINPUT, 0u);
  EXPECT_FALSE(PadIRQPending());
}

TEST_F(PadTest, ACKTiming_MemoryCard)
{
  PlugMemoryCard(0);
  Select(0);

  Pad::WriteRegister(JOY_DATA, 0x81);
  ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
  EXPECT_EQ(RunUntil(PadIRQPending, ACK_WINDOW), MEMORY_CARD_ACK_TICKS);
}

TEST_F(PadTest, ACKTiming_Multitap)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  PlugController(0);
  PlugMemoryCard(2);

  // Memory card behind sub-port B.
  Select(0);
  Pad::WriteRegister(JOY_DATA, 0x82);
  ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
  Pad::ReadRegister(JOY_DATA);
  EXPECT_EQ(RunUntil(PadIRQPending, ACK_WINDOW), MEMORY_CARD_ACK_TICKS);
  AckIRQ();
  Deselect();

  // Controller behind sub-port A.
  Select(0);
  Pad::WriteRegister(JOY_DATA, 0x01);
  ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
  EXPECT_EQ(RunUntil(PadIRQPending, ACK_WINDOW), CONTROLLER_ACK_TICKS);
}

TEST_F(PadTest, NoDevice_NoACK)
{
  Select(0);

  for (const u8 value : {0x01, 0x81})
  {
    Pad::WriteRegister(JOY_DATA, value);
    ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
    EXPECT_EQ(Truncate8(Pad::ReadRegister(JOY_DATA)), 0xFF);
    EXPECT_FALSE(Pad::IsTransmitting());
    EXPECT_EQ(RunUntil(PadIRQPending, ACK_WINDOW * 4), NOT_REACHED);

    const u32 stat = Pad::ReadRegister(JOY_STAT);
    EXPECT_EQ(stat & (STAT_TXRDY | STAT_TXDONE | STAT_RXFIFONEMPTY | STAT_ACKINPUT), STAT_TXRDY | STAT_TXDONE);
  }
}

TEST_F(PadTest, LastByte_NoACK)
{
  PlugController(0);
  Select(0);

  for (size_t i = 0; i < POLL.size() - 1; i++)
    ASSERT_TRUE(Exchange(POLL[i]).ack);

  Pad::WriteRegister(JOY_DATA, POLL.back());
  ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
  EXPECT_FALSE(Pad::IsTransmitting());
  EXPECT_EQ(RunUntil(PadIRQPending, ACK_WINDOW * 4), NOT_REACHED);
}

TEST_F(PadTest, QueuedByteDuringACKWait)
{
  PlugController(0);
  Select(0);

  Pad::WriteRegister(JOY_DATA, 0x01);
  ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
  EXPECT_EQ(Truncate8(Pad::ReadRegister(JOY_DATA)), 0xFF);

  // The next byte is written while waiting for the ACK, so it is sent once the ACK arrives.
  Pad::WriteRegister(JOY_DATA, 0x42);
  EXPECT_TRUE(Pad::IsTransmitting());
  EXPECT_EQ(Pad::ReadRegister(JOY_STAT) & STAT_TXRDY, 0u);
  EXPECT_EQ(RunUntil(PadIRQPending, ACK_WINDOW), CONTROLLER_ACK_TICKS);
  EXPECT_NE(Pad::ReadRegister(JOY_STAT) & STAT_TXRDY, 0u);
  EXPECT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
  EXPECT_EQ(Truncate8(Pad::ReadRegister(JOY_DATA)), 0x41);
}

TEST_F(PadTest, DeselectAbortsTransfer)
{
  PlugController(0);
  Select(0);

  // Deselect partway through a byte.
  Pad::WriteRegister(JOY_DATA, 0x01);
  Run(TRANSFER_TICKS / 2);
  Deselect();
  EXPECT_FALSE(Pad::IsTransmitting());
  EXPECT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 4), NOT_REACHED);
  EXPECT_FALSE(PadIRQPending());

  // Deselect partway through a command, the next one starts from the beginning.
  Select(0);
  ASSERT_TRUE(Exchange(0x01).ack);
  ASSERT_TRUE(Exchange(0x42).ack);
  Deselect();
  Select(0);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS));
}

// The BIOS writes the first byte, then acknowledges the interrupt before waiting for RX. The ACK interrupt for the new
// byte must arrive after that, otherwise it thinks there's no controller.
TEST_F(PadTest, BIOSIRQSequence)
{
  PlugController(0);
  Select(0);

  Pad::WriteRegister(JOY_DATA, 0x01);
  Run(10);
  AckIRQ();
  EXPECT_FALSE(PadIRQPending());
  EXPECT_EQ(Pad::ReadRegister(JOY_STAT) & STAT_INTR, 0u);

  ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS - 10);
  Pad::ReadRegister(JOY_DATA);
  EXPECT_EQ(RunUntil(PadIRQPending, ACK_WINDOW), CONTROLLER_ACK_TICKS);
}

TEST_F(PadTest, DigitalPadPoll)
{
  PlugController(0);
  PlugController(1);
  Press(0, DigitalController::Button::Start);
  Press(0, DigitalController::Button::Cross);
  Press(1, DigitalController::Button::Up);

  Select(0);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS & ~((1u << 3) | (1u << 14))));
  Deselect();

  Select(1);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS & ~(1u << 4)));
  Deselect();

  // Only the selected port answers.
  Pad::SetController(1, nullptr);
  Select(1);
  EXPECT_EQ(Command({0x01}), std::vector<u8>{0xFF});
  Deselect();
  Select(0);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS & ~((1u << 3) | (1u << 14))));
}

TEST_F(PadTest, MemoryCardReadWriteSector)
{
  static constexpr u16 SECTOR = 0x155;
  MemoryCard* const card = PlugMemoryCard(0);

  u8 checksum = Truncate8(SECTOR >> 8) ^ Truncate8(SECTOR);
  std::vector<u8> data(MemoryCardImage::FRAME_SIZE);
  for (size_t i = 0; i < data.size(); i++)
  {
    data[i] = static_cast<u8>(i * 7 + 3);
    checksum ^= data[i];
  }

  // Write.
  std::vector<u8> tx = {0x81, 0x57, 0x00, 0x00, Truncate8(SECTOR >> 8), Truncate8(SECTOR)};
  tx.insert(tx.end(), data.begin(), data.end());
  tx.insert(tx.end(), {checksum, 0x00, 0x00, 0x00});

  std::vector<u8> expected = {0xFF, 0x08, 0x5A, 0x5D, 0x00, Truncate8(SECTOR >> 8), Truncate8(SECTOR)};
  expected.insert(expected.end(), data.begin(), data.end() - 1);
  expected.insert(expected.end(), {data.back(), 0x5C, 0x5D, 0x47});

  Select(0);
  EXPECT_EQ(Command(tx), expected);
  Deselect();
  EXPECT_TRUE(std::equal(data.begin(), data.end(), card->GetData().begin() + SECTOR * MemoryCardImage::FRAME_SIZE));

  // Read it back, the no-write-yet flag is now clear.
  tx = {0x81, 0x52, 0x00, 0x00, Truncate8(SECTOR >> 8), Truncate8(SECTOR), 0x00, 0x00, 0x00, 0x00};
  tx.resize(tx.size() + data.size() + 2, 0x00);

  expected = {
    0xFF, 0x00, 0x5A, 0x5D, 0x00, Truncate8(SECTOR >> 8), 0x5C, 0x5D, Truncate8(SECTOR >> 8), Truncate8(SECTOR)};
  expected.insert(expected.end(), data.begin(), data.end());
  expected.insert(expected.end(), {checksum, 0x47});

  Select(0);
  EXPECT_EQ(Command(tx), expected);
}

TEST_F(PadTest, MemoryCardGetID)
{
  PlugController(0);
  PlugMemoryCard(0);

  // The controller doesn't answer 0x81, so the card does.
  Select(0);
  EXPECT_EQ(Command({0x81, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
            (std::vector<u8>{0xFF, 0x08, 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80}));
}

TEST_F(PadTest, Multitap_SingleController)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);

  // Sub-ports A-D on port 1 are pads 0, 2, 3, 4. Leave C empty.
  static constexpr u32 pads[] = {0, 2, 3, 4};
  for (u32 i = 0; i < 4; i++)
  {
    if (i != 2)
    {
      PlugController(pads[i]);
      Press(pads[i], static_cast<DigitalController::Button>(12 + i));
    }
  }

  for (u32 i = 0; i < 4; i++)
  {
    Select(0);
    if (i == 2)
      EXPECT_EQ(Command({0x03}), std::vector<u8>{0xFF});
    else
      EXPECT_EQ(Command({static_cast<u8>(0x01 + i), 0x42, 0x00, 0x00, 0x00}), PollReply(NO_BUTTONS & ~(1u << (12 + i))))
        << "sub-port " << i;
    Deselect();
  }

  // Port 2 has nothing behind it.
  Select(1);
  EXPECT_EQ(Command({0x01}), std::vector<u8>{0xFF});
}

TEST_F(PadTest, Multitap_AllControllers)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);

  // A, B and D connected, C empty.
  static constexpr u32 pads[] = {0, 2, 3, 4};
  for (u32 i = 0; i < 4; i++)
  {
    if (i != 2)
    {
      PlugController(pads[i]);
      Press(pads[i], static_cast<DigitalController::Button>(12 + i));
    }
  }

  // Request byte bit 0 queues up the all-controllers mode for the next poll.
  Select(0);
  EXPECT_EQ(Command({0x01, 0x42, 0x01, 0x00, 0x00}), PollReply(NO_BUTTONS & ~(1u << 12)));
  Deselect();

  std::vector<u8> tx = {0x01, 0x42, 0x01};
  for (u32 i = 0; i < 4; i++)
    tx.insert(tx.end(), {0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

  // The replies are buffered from the previous poll, so this one is empty.
  std::vector<u8> expected = {0xFF, 0x80, 0x5A};
  expected.resize(tx.size(), 0xFF);
  Select(0);
  EXPECT_EQ(Command(tx), expected);
  Deselect();

  expected = {0xFF, 0x80, 0x5A};
  for (u32 i = 0; i < 4; i++)
  {
    if (i == 2)
    {
      expected.resize(expected.size() + 8, 0xFF);
      continue;
    }

    const u16 buttons = NO_BUTTONS & ~(1u << (12 + i));
    expected.insert(expected.end(), {0x41, 0x5A, Truncate8(buttons), Truncate8(buttons >> 8), 0xFF, 0xFF, 0xFF, 0xFF});
  }

  Select(0);
  EXPECT_EQ(Command(tx), expected);
}

TEST_F(PadTest, Multitap_MemoryCard)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  PlugController(0);
  PlugMemoryCard(3);

  static const std::vector<u8> GET_ID_REPLY = {0xFF, 0x08, 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80};

  // Sub-port C has the card, and only C answers.
  for (u8 sub = 0; sub < 4; sub++)
  {
    Select(0);
    if (sub == 2)
      EXPECT_EQ(Command({0x83, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}), GET_ID_REPLY);
    else
      EXPECT_EQ(Command({static_cast<u8>(0x81 + sub)}), std::vector<u8>{0xFF}) << "sub-port " << u32(sub);
    Deselect();
  }
}

TEST_F(PadTest, Multitap_Port2)
{
  Pad::GetMultitap(1)->SetEnable(true, 1);

  // Sub-ports A-D on port 2 are pads 1, 5, 6, 7.
  PlugController(1);
  PlugController(7);
  PlugMemoryCard(5);
  Press(1, DigitalController::Button::Circle);
  Press(7, DigitalController::Button::Square);

  Select(1);
  EXPECT_EQ(Command({0x01, 0x42, 0x00, 0x00, 0x00}), PollReply(NO_BUTTONS & ~(1u << 13)));
  Deselect();
  Select(1);
  EXPECT_EQ(Command({0x04, 0x42, 0x00, 0x00, 0x00}), PollReply(NO_BUTTONS & ~(1u << 15)));
  Deselect();
  Select(1);
  EXPECT_EQ(Command({0x82, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
            (std::vector<u8>{0xFF, 0x08, 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80}));
  Deselect();

  // Port 1 has nothing behind it.
  Select(0);
  EXPECT_EQ(Command({0x01}), std::vector<u8>{0xFF});
}

TEST_F(PadTest, Registers)
{
  EXPECT_EQ(Pad::ReadRegister(JOY_MODE), BIOS_JOY_MODE);
  Pad::WriteRegister(JOY_MODE, 0xFFFF);
  EXPECT_EQ(Pad::ReadRegister(JOY_MODE), 0x013Fu);
  Pad::WriteRegister(JOY_MODE, BIOS_JOY_MODE);

  Pad::WriteRegister(JOY_BAUD, 0x1234);
  EXPECT_EQ(Pad::ReadRegister(JOY_BAUD), 0x1234u);

  // Acknowledge, reset and bits 7/14/15 read as zero. Reset is left out, since it clears the register.
  Pad::WriteRegister(JOY_CTRL, 0xFFFF & ~CTRL_RESET);
  EXPECT_EQ(Pad::ReadRegister(JOY_CTRL), 0x3F2Fu);
  Pad::WriteRegister(JOY_CTRL, 0);
  EXPECT_EQ(Pad::ReadRegister(JOY_CTRL), 0u);

  // An empty RX buffer reads as 0xFF, and the byte is repeated across the word.
  EXPECT_EQ(Pad::ReadRegister(JOY_DATA), 0xFFFFFFFFu);
  PlugController(0);
  Pad::WriteRegister(JOY_BAUD, BIOS_JOY_BAUD);
  Select(0);
  ASSERT_TRUE(Exchange(0x01).ack);
  Pad::WriteRegister(JOY_DATA, 0x42);
  ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
  EXPECT_EQ(Pad::ReadRegister(JOY_DATA), 0x41414141u);
  EXPECT_EQ(Pad::ReadRegister(JOY_DATA), 0xFFFFFFFFu);
}

TEST_F(PadTest, TXInterrupt)
{
  Select(0, CTRL_TXINTEN);
  EXPECT_FALSE(PadIRQPending());
  Pad::WriteRegister(JOY_DATA, 0x01);
  EXPECT_TRUE(PadIRQPending());
  EXPECT_NE(Pad::ReadRegister(JOY_STAT) & STAT_INTR, 0u);
}

TEST_F(PadTest, TransferWaitsForSelectAndTXEN)
{
  PlugController(0);

  for (const u32 ctrl : {static_cast<u32>(CTRL_SELECT), static_cast<u32>(CTRL_TXEN)})
  {
    m_ctrl = ctrl | CTRL_ACKINTEN;
    Pad::WriteRegister(JOY_CTRL, m_ctrl);
    Pad::WriteRegister(JOY_DATA, 0x01);
    EXPECT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 4), NOT_REACHED);
    EXPECT_FALSE(Pad::IsTransmitting());
    EXPECT_EQ(Pad::ReadRegister(JOY_STAT) & (STAT_TXRDY | STAT_TXDONE), 0u);

    // The byte is sent as soon as both are set.
    Select(0);
    EXPECT_TRUE(Pad::IsTransmitting());
    EXPECT_EQ(Pad::ReadRegister(JOY_STAT) & (STAT_TXRDY | STAT_TXDONE), STAT_TXRDY);
    ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
    EXPECT_EQ(Truncate8(Pad::ReadRegister(JOY_DATA)), 0xFF);
    EXPECT_EQ(RunUntil(PadIRQPending, ACK_WINDOW), CONTROLLER_ACK_TICKS);
    AckIRQ();
    Deselect();
  }
}

TEST_F(PadTest, TXBufferOverwrite)
{
  PlugController(0);

  // Only the last byte written is sent. 0x42 alone wouldn't be acknowledged.
  m_ctrl = CTRL_SELECT | CTRL_ACKINTEN;
  Pad::WriteRegister(JOY_CTRL, m_ctrl);
  Pad::WriteRegister(JOY_DATA, 0x42);
  Pad::WriteRegister(JOY_DATA, 0x01);
  Select(0);
  ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
  EXPECT_EQ(Truncate8(Pad::ReadRegister(JOY_DATA)), 0xFF);
  EXPECT_EQ(RunUntil(PadIRQPending, ACK_WINDOW), CONTROLLER_ACK_TICKS);
}

TEST_F(PadTest, ControllerAndMemoryCardSharePort)
{
  PlugController(0);
  PlugMemoryCard(0);

  Select(0);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS));
  Deselect();
  Select(0);
  EXPECT_EQ(Command(GetIDCommand(0x81)), GET_ID_REPLY);
  Deselect();

  // Once the controller has answered, it gets the rest of the bytes, and the card isn't asked.
  Select(0);
  EXPECT_EQ(Command({0x01, 0x81}), (std::vector<u8>{0xFF, 0xFF}));
  Deselect();
  Select(0);
  EXPECT_EQ(Command(GetIDCommand(0x81)), GET_ID_REPLY);
}

TEST_F(PadTest, InvalidCommands)
{
  PlugController(0);
  PlugMemoryCard(0);

  Select(0);
  EXPECT_EQ(Command({0x01, 0x43}), (std::vector<u8>{0xFF, 0xFF}));
  Deselect();
  Select(0);
  EXPECT_EQ(Command({0x81, 0x00}), (std::vector<u8>{0xFF, 0x08}));
  Deselect();

  for (const u8 first : {0x00, 0x02, 0x42, 0x82, 0xFF})
  {
    Select(0);
    EXPECT_EQ(Command({first}), std::vector<u8>{0xFF}) << "first byte 0x" << std::hex << u32(first);
    Deselect();
  }

  // Everything still works afterwards.
  Select(0);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS));
  Deselect();
  Select(0);
  EXPECT_EQ(Command(GetIDCommand(0x81)), GET_ID_REPLY);
}

TEST_F(PadTest, MemoryCardAddressMask)
{
  MemoryCard* const card = PlugMemoryCard(1);

  // Sector 0xFFFF is masked to 0x3FF. The echoed LSB is the previous byte sent.
  const std::vector<u8> expected = PrepareReadSector(card, 0xFFFF);
  Select(1);
  EXPECT_EQ(Command(ReadSectorCommand(0x81, 0xFFFF)), expected);
}

TEST_F(PadTest, ACKTiming_WholeCommands)
{
  PlugController(0);
  MemoryCard* const card = PlugMemoryCard(0);

  Select(0);
  Command(POLL);
  EXPECT_EQ(m_ack_ticks, std::vector<u32>(POLL.size() - 1, CONTROLLER_ACK_TICKS));
  Deselect();

  Select(0);
  Command(ReadSectorCommand(0x81, 0x12));
  EXPECT_EQ(m_ack_ticks, std::vector<u32>(ReadSectorCommand(0x81, 0x12).size() - 1, MEMORY_CARD_ACK_TICKS));
  Deselect();

  // Same through the multitap.
  Pad::GetMultitap(0)->SetEnable(true, 0);
  Select(0);
  Command(POLL);
  EXPECT_EQ(m_ack_ticks, std::vector<u32>(POLL.size() - 1, CONTROLLER_ACK_TICKS));
  Deselect();

  Select(0);
  PrepareReadSector(card, 0x12);
  Command(ReadSectorCommand(0x81, 0x12));
  EXPECT_EQ(m_ack_ticks, std::vector<u32>(ReadSectorCommand(0x81, 0x12).size() - 1, MEMORY_CARD_ACK_TICKS));
  Deselect();

  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();
  Select(0);
  Command(AllControllersPoll());
  EXPECT_EQ(m_ack_ticks, std::vector<u32>(AllControllersPoll().size() - 1, CONTROLLER_ACK_TICKS));
}

TEST_F(PadTest, Multitap_Disabled)
{
  PlugController(0);
  PlugController(2);

  // Without the multitap, pads 2-4 can't be reached, and the first byte must be 0x01.
  Select(0);
  EXPECT_EQ(Command({0x02}), std::vector<u8>{0xFF});
  Deselect();
  Select(0);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS));
  Deselect();

  // All-controllers mode isn't a thing either.
  Select(0);
  EXPECT_EQ(Command({0x01, 0x42, 0x01, 0x00, 0x00}), PollReply(NO_BUTTONS));
  Deselect();
  Select(0);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS));
}

TEST_F(PadTest, Multitap_InvalidFirstByte)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  for (const u32 pad : {0u, 2u, 3u, 4u})
  {
    PlugController(pad);
    PlugMemoryCard(pad);
  }

  for (const u8 first : {0x00, 0x05, 0x11, 0x42, 0x80, 0x85, 0xFF})
  {
    Select(0);
    EXPECT_EQ(Command({first}), std::vector<u8>{0xFF}) << "first byte 0x" << std::hex << u32(first);
    Deselect();
  }
}

TEST_F(PadTest, Multitap_MemoryCardReadSector)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  PlugController(4);
  MemoryCard* const card = PlugMemoryCard(4);

  // Sub-port D is pad 4.
  const std::vector<u8> expected = PrepareReadSector(card, 0x2A5);
  Select(0);
  EXPECT_EQ(Command(ReadSectorCommand(0x84, 0x2A5)), expected);
  Deselect();

  // The controller on the same sub-port still answers 0x04.
  Select(0);
  EXPECT_EQ(Command({0x04, 0x42, 0x00, 0x00, 0x00}), PollReply(NO_BUTTONS));
}

TEST_F(PadTest, Multitap_SingleMode_Analog)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  PlugAnalogController(3);

  // Sub-port C, through config mode and back.
  EnterAnalogMode(0, 0x03);
  Press(3, DigitalController::Button::Square);
  Select(0);
  const u16 buttons = NO_BUTTONS & ~(1u << 15);
  EXPECT_EQ(Command({0x03, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
            (std::vector<u8>{0xFF, 0x73, 0x5A, Truncate8(buttons), Truncate8(buttons >> 8), 0x7F, 0x7F, 0x7F, 0x7F}));
}

TEST_F(PadTest, Multitap_AllControllers_Analog)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);

  // A: digital pad, B: analog in analog mode, C: empty, D: analog in digital mode.
  PlugController(0);
  PlugAnalogController(2);
  PlugAnalogController(4);
  EnterAnalogMode(0, 0x02);
  Press(0, DigitalController::Button::Cross);
  Press(2, DigitalController::Button::Circle);
  Press(4, DigitalController::Button::Start);

  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();
  Select(0);
  Command(AllControllersPoll());
  Deselect();

  // A full analog reply fills the whole block.
  Select(0);
  EXPECT_EQ(Command(AllControllersPoll()),
            AllControllersReply({DigitalBlock(NO_BUTTONS & ~(1u << 14)), AnalogBlock(NO_BUTTONS & ~(1u << 13)),
                                 EMPTY_BLOCK, DigitalBlock(NO_BUTTONS & ~(1u << 3))}));
}

TEST_F(PadTest, Multitap_AllControllers_CommandPassthrough)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  PlugController(0);
  PlugAnalogController(2);

  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();

  // Each block's bytes go to that sub-port's controller. Enter config mode on B.
  Select(0);
  EXPECT_EQ(Command(AllControllersCommand(
              0x01, 0x01, {POLL_BLOCK, {0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}, POLL_BLOCK, POLL_BLOCK})),
            AllControllersReply({EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK}));
  Deselect();

  // The reply to that shows up on the next poll, and B's config mode poll on the one after.
  Select(0);
  EXPECT_EQ(Command(AllControllersPoll()),
            AllControllersReply({DigitalBlock(NO_BUTTONS), DigitalBlock(NO_BUTTONS), EMPTY_BLOCK, EMPTY_BLOCK}));
  Deselect();
  Select(0);
  EXPECT_EQ(Command(AllControllersPoll()),
            AllControllersReply(
              {DigitalBlock(NO_BUTTONS), {0xF3, 0x5A, 0xFF, 0xFF, 0x7F, 0x7F, 0x7F, 0x7F}, EMPTY_BLOCK, EMPTY_BLOCK}));
}

TEST_F(PadTest, Multitap_AllControllers_SubportAEmpty)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  PlugController(2);
  PlugController(4);
  Press(2, DigitalController::Button::Triangle);

  Select(0);
  EXPECT_EQ(Command({0x02, 0x42, 0x01, 0x00, 0x00}), PollReply(NO_BUTTONS & ~(1u << 12)));
  Deselect();

  // Starting with 0x01 needs a controller on A.
  Select(0);
  EXPECT_EQ(Command({0x01}), std::vector<u8>{0xFF});
  Deselect();

  // Any connected sub-port can start it, the blocks still start from A.
  Select(0);
  EXPECT_EQ(Command(AllControllersPoll(0x02)),
            AllControllersReply({EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK}));
  Deselect();
  Select(0);
  EXPECT_EQ(Command(AllControllersPoll(0x04)), AllControllersReply({EMPTY_BLOCK, DigitalBlock(NO_BUTTONS & ~(1u << 12)),
                                                                    EMPTY_BLOCK, DigitalBlock(NO_BUTTONS)}));
}

TEST_F(PadTest, Multitap_AllControllers_InvalidCommand)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  PlugController(0);

  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();

  // Anything other than 0x42 still gets the multitap ID, but stops at the request byte. Bit 0 of that still queues
  // the mode for the next poll.
  Select(0);
  EXPECT_EQ(Command({0x01, 0x43, 0x01}), (std::vector<u8>{0xFF, 0x80, 0x5A}));
  Deselect();
  Select(0);
  EXPECT_EQ(Command({0x01, 0x4D, 0x00}), (std::vector<u8>{0xFF, 0x80, 0x5A}));
  Deselect();

  Select(0);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS));
}

TEST_F(PadTest, Multitap_AllControllers_Exit)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  PlugController(0);

  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();

  // A request byte of 0x00 leaves all-controllers mode after this poll.
  const std::vector<u8> tx = AllControllersCommand(0x01, 0x00, {POLL_BLOCK, POLL_BLOCK, POLL_BLOCK, POLL_BLOCK});
  Select(0);
  EXPECT_EQ(Command(tx), AllControllersReply({EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK}));
  Deselect();

  Select(0);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS));
}

TEST_F(PadTest, Multitap_AllControllers_MemoryCardKeepsMode)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  PlugController(0);
  PlugMemoryCard(0);

  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();

  // Memory card accesses in between don't change the queued mode.
  Select(0);
  EXPECT_EQ(Command(GetIDCommand(0x81)), GET_ID_REPLY);
  Deselect();

  Select(0);
  EXPECT_EQ(Command(AllControllersPoll()), AllControllersReply({EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK}));
}

TEST_F(PadTest, Multitap_SetEnableResets)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  PlugController(0);

  // Enabling again without a change keeps the queued mode and the buffered replies.
  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();
  Select(0);
  Command(AllControllersPoll());
  Deselect();
  Pad::GetMultitap(0)->SetEnable(true, 0);
  Select(0);
  EXPECT_EQ(Command(AllControllersPoll()),
            AllControllersReply({DigitalBlock(NO_BUTTONS), EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK}));
  Deselect();

  // Disabling and enabling clears both.
  Pad::GetMultitap(0)->SetEnable(false, 0);
  Pad::GetMultitap(0)->SetEnable(true, 0);
  Select(0);
  EXPECT_EQ(Command({0x01, 0x42, 0x01, 0x00, 0x00}), PollReply(NO_BUTTONS));
  Deselect();
  Select(0);
  EXPECT_EQ(Command(AllControllersPoll()), AllControllersReply({EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK}));
}

TEST_F(PadTest, Multitap_BothPorts)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  Pad::GetMultitap(1)->SetEnable(true, 1);
  PlugController(0);
  PlugController(1);
  PlugController(5);
  Press(0, DigitalController::Button::Up);
  Press(5, DigitalController::Button::Down);

  // Only port 1 goes into all-controllers mode.
  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();
  Select(1);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS));
  Deselect();
  Select(0);
  Command(AllControllersPoll());
  Deselect();

  Select(1);
  EXPECT_EQ(Command({0x02, 0x42, 0x00, 0x00, 0x00}), PollReply(NO_BUTTONS & ~(1u << 6)));
  Deselect();
  Select(0);
  EXPECT_EQ(Command(AllControllersPoll()),
            AllControllersReply({DigitalBlock(NO_BUTTONS & ~(1u << 4)), EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK}));
}

TEST_F(PadTest, Multitap_MemoryCardRouting)
{
  static constexpr u16 SECTOR = 0x77;
  Pad::GetMultitap(0)->SetEnable(true, 0);
  Pad::GetMultitap(1)->SetEnable(true, 1);

  // A card on every sub-port of both ports, each with different data in the same sector.
  std::array<std::vector<u8>, NUM_CONTROLLER_AND_CARD_PORTS> expected;
  for (u32 pad = 0; pad < NUM_CONTROLLER_AND_CARD_PORTS; pad++)
    expected[pad] = PrepareReadSector(PlugMemoryCard(pad), SECTOR, static_cast<u8>(pad * 37 + 1));

  for (u32 port = 0; port < 2; port++)
  {
    for (u32 sub = 0; sub < 4; sub++)
    {
      const u32 pad = Controller::ConvertPortAndSlotToPad(port, sub);
      Select(port);
      EXPECT_EQ(Command(ReadSectorCommand(static_cast<u8>(0x81 + sub), SECTOR)), expected[pad])
        << "port " << port << " sub-port " << sub << " (pad " << pad << ")";
      Deselect();
    }
  }
}

TEST_F(PadTest, Multitap_MemoryCardWriteSector)
{
  static constexpr u16 SECTOR = 0x1C3;
  Pad::GetMultitap(1)->SetEnable(true, 1);
  for (const u32 pad : {1u, 5u, 6u, 7u})
    PlugMemoryCard(pad);

  const MemoryCardImage::DataArray before = Pad::GetMemoryCard(1)->GetData();

  u8 checksum = Truncate8(SECTOR >> 8) ^ Truncate8(SECTOR);
  std::vector<u8> data(MemoryCardImage::FRAME_SIZE);
  for (size_t i = 0; i < data.size(); i++)
  {
    data[i] = static_cast<u8>(0xA5 ^ (i * 3));
    checksum ^= data[i];
  }

  // Sub-port C on port 2 is pad 6.
  std::vector<u8> tx = {0x83, 0x57, 0x00, 0x00, Truncate8(SECTOR >> 8), Truncate8(SECTOR)};
  tx.insert(tx.end(), data.begin(), data.end());
  tx.insert(tx.end(), {checksum, 0x00, 0x00, 0x00});

  std::vector<u8> expected = {0xFF, 0x08, 0x5A, 0x5D, 0x00, Truncate8(SECTOR >> 8), Truncate8(SECTOR)};
  expected.insert(expected.end(), data.begin(), data.end() - 1);
  expected.insert(expected.end(), {data.back(), 0x5C, 0x5D, 0x47});

  Select(1);
  EXPECT_EQ(Command(tx), expected);
  EXPECT_EQ(m_ack_ticks, std::vector<u32>(tx.size() - 1, MEMORY_CARD_ACK_TICKS));
  Deselect();

  for (const u32 pad : {1u, 5u, 6u, 7u})
  {
    const auto& card_data = Pad::GetMemoryCard(pad)->GetData();
    if (pad == 6)
      EXPECT_TRUE(std::equal(data.begin(), data.end(), card_data.begin() + SECTOR * MemoryCardImage::FRAME_SIZE));
    else
      EXPECT_EQ(card_data, before) << "pad " << pad;
  }

  // Only that card has its no-write-yet flag cleared.
  for (u8 sub = 0; sub < 4; sub++)
  {
    std::vector<u8> id_reply = GET_ID_REPLY;
    id_reply[1] = (sub == 2) ? 0x00 : 0x08;
    Select(1);
    EXPECT_EQ(Command(GetIDCommand(static_cast<u8>(0x81 + sub))), id_reply) << "sub-port " << u32(sub);
    Deselect();
  }
}

TEST_F(PadTest, Multitap_MemoryCardErrors)
{
  Pad::GetMultitap(0)->SetEnable(true, 0);
  PlugController(2);
  PlugMemoryCard(2);

  // An invalid card command through the tap ends the transfer, and the next one works.
  Select(0);
  EXPECT_EQ(Command({0x82, 0x00}), (std::vector<u8>{0xFF, 0x08}));
  EXPECT_EQ(Command(GetIDCommand(0x82)), GET_ID_REPLY);
  Deselect();

  // As does deselecting partway through a read.
  Select(0);
  for (const u8 value : {0x82, 0x52, 0x00, 0x00})
    ASSERT_TRUE(Exchange(value).ack);
  Deselect();
  Select(0);
  EXPECT_EQ(Command(GetIDCommand(0x82)), GET_ID_REPLY);
  Deselect();

  // The controller on the same sub-port isn't affected.
  Select(0);
  EXPECT_EQ(Command({0x02, 0x42, 0x00, 0x00, 0x00}), PollReply(NO_BUTTONS));
}

TEST_F(PadTest, ResetBit)
{
  PlugController(0);
  Select(0, CTRL_ACKINTEN | CTRL_RXINTEN);

  // Reset partway through a byte.
  Pad::WriteRegister(JOY_DATA, 0x01);
  Run(TRANSFER_TICKS / 2);
  Pad::WriteRegister(JOY_CTRL, CTRL_RESET | CTRL_TXEN | CTRL_SELECT);
  EXPECT_FALSE(Pad::IsTransmitting());

  // Everything reads back as zero, and the port is ready to send.
  EXPECT_EQ(Pad::ReadRegister(JOY_CTRL), 0u);
  EXPECT_EQ(Pad::ReadRegister(JOY_MODE), 0u);
  EXPECT_EQ(Pad::ReadRegister(JOY_BAUD), 0u);
  EXPECT_EQ(Pad::ReadRegister(JOY_STAT), STAT_TXRDY | STAT_TXDONE);
  EXPECT_EQ(RunUntil(PadIRQPending, TRANSFER_TICKS * 4), NOT_REACHED);
  EXPECT_EQ(Pad::ReadRegister(JOY_STAT) & STAT_RXFIFONEMPTY, 0u);

  // The controller starts a new command afterwards.
  Pad::WriteRegister(JOY_MODE, BIOS_JOY_MODE);
  Pad::WriteRegister(JOY_BAUD, BIOS_JOY_BAUD);
  Select(0);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS));
}
