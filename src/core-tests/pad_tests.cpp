// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller_test_utils.h"

#include "core/save_state_version.h"

#include "util/state_wrapper.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

// These tests check what is currently emulated. Comments starting with "psx-spx:" mark places where that differs from
// the behaviour documented there, see the notes in pad.cpp and memory_card.cpp.

using namespace ControllerTestUtils;

namespace {

const std::vector<u8> GET_ID_REPLY = {0xFF, 0x08, 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80};

// Multitap all-controllers mode: what's sent and received for each sub-port.
const Block POLL_BLOCK = {0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const Block EMPTY_BLOCK = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const Block ZERO_BLOCK = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// psx-spx's "garbage" reply: the multitap ID, then Slot A's reply to the rejected command byte (its ID LSB), and no
// more ACKs.
const std::vector<u8> GARBAGE_TX = {0x01, 0x42, 0x01, 0x00};
const std::vector<u8> GARBAGE_REPLY = {0xFF, 0x80, 0x5A, 0x41};

static std::vector<u8> GetIDCommand(u8 first)
{
  return {first, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
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
  u8* const data = card->GetData().data() + sector * MemoryCardImage::FRAME_SIZE;
  u8 checksum = Truncate8(sector >> 8) ^ Truncate8(sector);

  std::vector<u8> ret = {
    0xFF, 0x08, 0x5A, 0x5D, 0x00, Truncate8(sector >> 8), 0x5C, 0x5D, Truncate8(sector >> 8), Truncate8(sector)};
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
  // psx-spx: it follows the /ACK line instead, and goes back to zero after ~100 cycles.
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

// psx-spx: memory card IRQs arrive around 1500 cycles after the byte starts, rather than 1088 + 170.
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
  Pad::SetMultitapEnabled(0, true);
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
  // psx-spx: it would be sent straight away, since the previous byte has finished.
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
  // psx-spx: the byte would finish being sent, there just wouldn't be a device to reply.
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
  Pad::SetMultitapEnabled(0, true);

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
  Pad::SetMultitapEnabled(0, true);

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

  std::vector<u8> expected = {0xFF, 0x80, 0x5A};
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

  // Every poll returns all four, including the first.
  for (u32 i = 0; i < 2; i++)
  {
    Select(0);
    EXPECT_EQ(Command(tx), expected) << "poll " << i;
    Deselect();
  }
}

TEST_F(PadTest, Multitap_MemoryCard)
{
  Pad::SetMultitapEnabled(0, true);
  PlugController(0);
  PlugMemoryCard(3);

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
  Pad::SetMultitapEnabled(1, true);

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
  // psx-spx: an empty FIFO returns the last byte received, then 00h.
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

// psx-spx: the TX interrupt is raised while TXRDY or TXDONE is set, not when JOY_DATA is written.
TEST_F(PadTest, TXInterrupt)
{
  Select(0, CTRL_TXINTEN);
  EXPECT_FALSE(PadIRQPending());
  Pad::WriteRegister(JOY_DATA, 0x01);
  EXPECT_TRUE(PadIRQPending());
  EXPECT_NE(Pad::ReadRegister(JOY_STAT) & STAT_INTR, 0u);
}

// psx-spx: only TXEN is needed to send, SELECT just asserts /CS.
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

TEST_F(PadTest, SaveStateWhileTransmitting)
{
  PlugController(0);
  Select(0);
  EXPECT_TRUE(Exchange(0x01).ack);

  // The byte being shifted out is saved.
  Pad::WriteRegister(JOY_DATA, 0x42);
  Run(TRANSFER_TICKS / 2);
  std::vector<u8> state(4096);
  StateWrapper write_sw(std::span<u8>(state), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  ASSERT_TRUE(Pad::DoState(write_sw, false));

  // Send something else before loading, so the byte isn't left over from before.
  ASSERT_NE(RunUntil(RXReady, TRANSFER_TICKS * 2), NOT_REACHED);
  EXPECT_EQ(Truncate8(Pad::ReadRegister(JOY_DATA)), 0x41);
  RunUntil(PadIRQPending, ACK_WINDOW);
  AckIRQ();
  Deselect();
  Select(0);
  EXPECT_TRUE(Exchange(0x01).ack);
  StateWrapper read_sw(std::span<const u8>(state), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
  ASSERT_TRUE(Pad::DoState(read_sw, false));

  ASSERT_NE(RunUntil(RXReady, TRANSFER_TICKS * 2), NOT_REACHED);
  EXPECT_EQ(Truncate8(Pad::ReadRegister(JOY_DATA)), 0x41);
  EXPECT_NE(RunUntil(PadIRQPending, ACK_WINDOW), NOT_REACHED);
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
  EXPECT_EQ(Command({0x01, 0x81}), (std::vector<u8>{0xFF, 0x41}));
  Deselect();
  Select(0);
  EXPECT_EQ(Command(GetIDCommand(0x81)), GET_ID_REPLY);
}

TEST_F(PadTest, InvalidCommands)
{
  PlugController(0);
  PlugMemoryCard(0);

  Select(0);
  EXPECT_EQ(Command({0x01, 0x43}), (std::vector<u8>{0xFF, 0x41}));
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

TEST_F(PadTest, MemoryCardInvalidSector)
{
  MemoryCard* const card = PlugMemoryCard(1);

  // Like Sony cards, sectors past 0x3FF are confirmed as FFFF, and the transfer ends there.
  for (const u16 sector : {0x400, 0xFFFF})
  {
    Select(1);
    EXPECT_EQ(Command({0x81, 0x52, 0x00, 0x00, Truncate8(sector >> 8), Truncate8(sector), 0x00, 0x00, 0x00, 0x00}),
              (std::vector<u8>{0xFF, 0x08, 0x5A, 0x5D, 0x00, Truncate8(sector >> 8), 0x5C, 0x5D, 0xFF, 0xFF}))
      << "sector 0x" << std::hex << sector;
    Deselect();
  }

  // The last valid sector still works.
  const std::vector<u8> expected = PrepareReadSector(card, 0x3FF);
  Select(1);
  EXPECT_EQ(Command(ReadSectorCommand(0x81, 0x3FF)), expected);
  Deselect();

  // Writes to invalid sectors don't change anything.
  const MemoryCardImage::DataArray before = card->GetData();
  std::vector<u8> tx = {0x81, 0x57, 0x00, 0x00, 0x04, 0x00};
  tx.resize(tx.size() + MemoryCardImage::FRAME_SIZE, 0xAA);
  tx.insert(tx.end(), {0x04, 0x00, 0x00, 0x00});
  Select(1);
  EXPECT_EQ(Command(tx).back(), 0xFF);
  EXPECT_EQ(card->GetData(), before);
}

TEST_F(PadTest, MemoryCardInvalidSector_SaveState)
{
  MemoryCard* const card = PlugMemoryCard(0);
  const MemoryCardImage::DataArray before = card->GetData();

  // Saving and loading a state partway through a command keeps the invalid sector, instead of wrapping it to sector 0.
  const auto round_trip = [card]() {
    std::vector<u8> data(2 * sizeof(MemoryCardImage::DataArray));
    StateWrapper write_sw(std::span<u8>(data), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
    ASSERT_TRUE(card->DoState(write_sw));
    StateWrapper read_sw(std::span<const u8>(data), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
    ASSERT_TRUE(card->DoState(read_sw));
  };

  Select(0);
  for (const u8 tx : {0x81, 0x52, 0x00, 0x00, 0x04, 0x00})
    EXPECT_TRUE(Exchange(tx).ack);
  round_trip();
  EXPECT_EQ(Command({0x00, 0x00, 0x00, 0x00}), (std::vector<u8>{0x5C, 0x5D, 0xFF, 0xFF}));
  Deselect();

  Select(0);
  for (const u8 tx : {0x81, 0x57, 0x00, 0x00, 0x04, 0x00})
    EXPECT_TRUE(Exchange(tx).ack);
  round_trip();
  std::vector<u8> tx(MemoryCardImage::FRAME_SIZE, 0xAA);
  tx.insert(tx.end(), {0x04, 0x00, 0x00, 0x00});
  EXPECT_EQ(Command(tx).back(), 0xFF);
  Deselect();
  EXPECT_EQ(card->GetData(), before);
}

TEST_F(PadTest, MemoryCardWrite_SaveState)
{
  static constexpr u16 SECTOR = 0x55;
  PlugController(0);
  MemoryCard* const card = PlugMemoryCard(0);

  std::vector<u8> state(4 * sizeof(MemoryCardImage::DataArray));
  Select(0);
  for (const u8 tx : std::vector<u8>{0x81, 0x57, 0x00, 0x00, Truncate8(SECTOR >> 8), Truncate8(SECTOR)})
    EXPECT_TRUE(Exchange(tx).ack);
  StateWrapper write_sw(std::span<u8>(state), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  ASSERT_TRUE(Pad::DoState(write_sw, false));

  // Deselecting resets the devices, so loading the state has to restore which one was being accessed. Otherwise the
  // idle controller would take the 01h data bytes.
  Deselect();
  StateWrapper read_sw(std::span<const u8>(state), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
  ASSERT_TRUE(Pad::DoState(read_sw, false));

  const std::vector<u8> data(MemoryCardImage::FRAME_SIZE, 0x01);
  std::vector<u8> tx = data;
  tx.insert(tx.end(), {static_cast<u8>(Truncate8(SECTOR >> 8) ^ Truncate8(SECTOR)), 0x00, 0x00, 0x00});
  EXPECT_EQ(Command(tx).back(), 0x47);
  Deselect();
  EXPECT_TRUE(std::equal(data.begin(), data.end(), card->GetData().begin() + SECTOR * MemoryCardImage::FRAME_SIZE));
}

TEST_F(PadTest, MemoryCardWriteBadChecksum)
{
  static constexpr u16 SECTOR = 0x3F;
  PlugMemoryCard(0);

  // Checksum is MSB ^ LSB ^ data, which is 0x3F for a zero-filled sector.
  for (const auto [checksum, end] :
       {std::pair<u8, u8>{0x00, 0x4E}, std::pair<u8, u8>{0x3F, 0x47}, std::pair<u8, u8>{0x3E, 0x4E}})
  {
    std::vector<u8> tx = {0x81, 0x57, 0x00, 0x00, Truncate8(SECTOR >> 8), Truncate8(SECTOR)};
    tx.resize(tx.size() + MemoryCardImage::FRAME_SIZE, 0x00);
    tx.insert(tx.end(), {checksum, 0x00, 0x00, 0x00});
    Select(0);
    const std::vector<u8> rx = Command(tx);
    ASSERT_EQ(rx.size(), tx.size());
    EXPECT_EQ(rx[rx.size() - 4], 0x00) << "checksum 0x" << std::hex << u32(checksum);
    EXPECT_EQ(rx[rx.size() - 3], 0x5C);
    EXPECT_EQ(rx[rx.size() - 2], 0x5D);
    EXPECT_EQ(rx.back(), end) << "checksum 0x" << std::hex << u32(checksum);
    Deselect();
  }
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
  Pad::SetMultitapEnabled(0, true);
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
  Pad::SetMultitapEnabled(0, true);
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
  Pad::SetMultitapEnabled(0, true);
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
  Pad::SetMultitapEnabled(0, true);
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
  Pad::SetMultitapEnabled(0, true);

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
  Pad::SetMultitapEnabled(0, true);
  PlugController(0);
  PlugAnalogController(2);

  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();

  // Each block's bytes go to that sub-port's controller in the next poll. Enter config mode on B.
  Select(0);
  EXPECT_EQ(Command(AllControllersCommand(
              0x01, 0x01, {POLL_BLOCK, {0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}, POLL_BLOCK, POLL_BLOCK})),
            AllControllersReply({DigitalBlock(NO_BUTTONS), DigitalBlock(NO_BUTTONS), EMPTY_BLOCK, EMPTY_BLOCK}));
  Deselect();

  // B gets the config mode command on the next poll, and replies with its config mode poll on the one after.
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
  Pad::SetMultitapEnabled(0, true);
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

  // The other sub-ports are always accessed on their own, even with the request queued.
  Select(0);
  EXPECT_EQ(Command({0x02, 0x42, 0x01, 0x00, 0x00}), PollReply(NO_BUTTONS & ~(1u << 12)));
  Deselect();
  Select(0);
  EXPECT_EQ(Command({0x04, 0x42, 0x01, 0x00, 0x00}), PollReply(NO_BUTTONS));
}

TEST_F(PadTest, Multitap_AllControllers_InvalidCommand)
{
  Pad::SetMultitapEnabled(0, true);
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
  Pad::SetMultitapEnabled(0, true);
  PlugController(0);

  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();

  // A request byte of 0x00 leaves all-controllers mode after this poll.
  const std::vector<u8> tx = AllControllersCommand(0x01, 0x00, {POLL_BLOCK, POLL_BLOCK, POLL_BLOCK, POLL_BLOCK});
  Select(0);
  EXPECT_EQ(Command(tx), AllControllersReply({DigitalBlock(NO_BUTTONS), EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK}));
  Deselect();

  Select(0);
  EXPECT_EQ(Command(POLL), PollReply(NO_BUTTONS));
}

TEST_F(PadTest, Multitap_AllControllers_MemoryCardKeepsMode)
{
  Pad::SetMultitapEnabled(0, true);
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
  EXPECT_EQ(Command(AllControllersPoll()),
            AllControllersReply({DigitalBlock(NO_BUTTONS), EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK}));
}

TEST_F(PadTest, Multitap_SetEnableResets)
{
  Pad::SetMultitapEnabled(0, true);
  PlugController(0);

  // Queue up all-controllers mode, and send 00h at the start of Slot A's block.
  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();
  Select(0);
  EXPECT_EQ(Command(AllControllersCommand(0x01, 0x01, {ZERO_BLOCK, POLL_BLOCK, POLL_BLOCK, POLL_BLOCK})),
            AllControllersReply({DigitalBlock(NO_BUTTONS), EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK}));
  Deselect();

  // Enabling again without a change keeps the queued mode and the bytes for the controllers.
  Pad::SetMultitapEnabled(0, true);
  Select(0);
  EXPECT_EQ(Command(GARBAGE_TX), GARBAGE_REPLY);
  Deselect();

  // Disabling and enabling clears both.
  Pad::SetMultitapEnabled(0, false);
  Pad::SetMultitapEnabled(0, true);
  Select(0);
  EXPECT_EQ(Command({0x01, 0x42, 0x01, 0x00, 0x00}), PollReply(NO_BUTTONS));
  Deselect();
  Select(0);
  EXPECT_EQ(Command(AllControllersPoll()),
            AllControllersReply({DigitalBlock(NO_BUTTONS), EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK}));
}

TEST_F(PadTest, Multitap_AllControllers_Garbage)
{
  Pad::SetMultitapEnabled(0, true);
  PlugController(0);

  const std::vector<u8> long_reply =
    AllControllersReply({DigitalBlock(NO_BUTTONS), EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK});
  const auto check = [this](const std::vector<u8>& tx, const std::vector<u8>& expected, const char* what) {
    Select(0);
    EXPECT_EQ(Command(tx), expected) << what;
    Deselect();
  };

  // Sending 00h at the start of each block returns all controllers once, then garbage. Each controller gets the
  // bytes from the previous access, so Slot A doesn't ACK the 00h command.
  check({0x01, 0x42, 0x01, 0x00, 0x00}, PollReply(NO_BUTTONS), "queue");
  check(AllControllersCommand(0x01, 0x01, {ZERO_BLOCK, ZERO_BLOCK, ZERO_BLOCK, ZERO_BLOCK}), long_reply, "00h");
  check(GARBAGE_TX, GARBAGE_REPLY, "garbage");

  // It stays that way while the request bit is set and 00h is sent at the start of the block.
  check(GARBAGE_TX, GARBAGE_REPLY, "garbage again");

  // An access with the request bit clear is still garbage, and the one after it is a normal access.
  check({0x01, 0x42, 0x00, 0x00}, GARBAGE_REPLY, "garbage with request clear");
  check(POLL, PollReply(NO_BUTTONS), "normal after garbage");

  // Requesting it again from a normal access works.
  check({0x01, 0x42, 0x01, 0x00, 0x00}, PollReply(NO_BUTTONS), "queue again");
  check(AllControllersPoll(), long_reply, "42h");

  // Sending 42h at the start of each block keeps returning all controllers.
  check(AllControllersPoll(), long_reply, "42h again");

  // Toggling the request bit alternates between the two.
  const std::vector<u8> long_exit = AllControllersCommand(0x01, 0x00, {POLL_BLOCK, POLL_BLOCK, POLL_BLOCK, POLL_BLOCK});
  check(long_exit, long_reply, "toggle long 1");
  check({0x01, 0x42, 0x01, 0x00, 0x00}, PollReply(NO_BUTTONS), "toggle normal 1");
  check(long_exit, long_reply, "toggle long 2");
  check(POLL, PollReply(NO_BUTTONS), "toggle normal 2");

  // The byte sent at the start of Slot A's block in a garbage access is still passed on, so sending 42h there makes
  // the next access valid. Quake II relies on this after probing a digital controller with 43h.
  check({0x01, 0x42, 0x01, 0x00, 0x00}, PollReply(NO_BUTTONS), "queue before 42h");
  check(AllControllersCommand(0x01, 0x01, {ZERO_BLOCK, POLL_BLOCK, POLL_BLOCK, POLL_BLOCK}), long_reply, "00h once");
  check({0x01, 0x42, 0x01, 0x42}, GARBAGE_REPLY, "garbage with 42h");
  check(AllControllersPoll(), long_reply, "valid after 42h");
}

TEST_F(PadTest, Multitap_AllControllers_SubportGarbage)
{
  Pad::SetMultitapEnabled(0, true);
  PlugController(0);
  PlugController(2);

  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();

  // Only Slot B's block starts with 00h, and an empty Slot C is still padded.
  Select(0);
  EXPECT_EQ(Command(AllControllersCommand(0x01, 0x01, {POLL_BLOCK, ZERO_BLOCK, POLL_BLOCK, POLL_BLOCK})),
            AllControllersReply({DigitalBlock(NO_BUTTONS), DigitalBlock(NO_BUTTONS), EMPTY_BLOCK, EMPTY_BLOCK}));
  Deselect();

  // Slot B not ACKing its command stops the access after its ID byte too.
  std::vector<u8> tx = AllControllersPoll();
  tx.resize(3 + 8 + 1);
  std::vector<u8> expected = AllControllersReply({DigitalBlock(NO_BUTTONS), EMPTY_BLOCK, EMPTY_BLOCK, EMPTY_BLOCK});
  expected.resize(3 + 8);
  expected.push_back(0x41);
  Select(0);
  EXPECT_EQ(Command(tx), expected);
}

TEST_F(PadTest, Multitap_SaveState)
{
  Pad::SetMultitapEnabled(0, true);
  PlugController(0);

  const auto round_trip = []() {
    std::vector<u8> data(64 * 1024);
    StateWrapper write_sw(std::span<u8>(data), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
    ASSERT_TRUE(Pad::DoState(write_sw, false));
    StateWrapper read_sw(std::span<const u8>(data), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
    ASSERT_TRUE(Pad::DoState(read_sw, false));
  };

  Select(0);
  Command({0x01, 0x42, 0x01, 0x00, 0x00});
  Deselect();
  Select(0);
  Command(AllControllersCommand(0x01, 0x01, {ZERO_BLOCK, ZERO_BLOCK, ZERO_BLOCK, ZERO_BLOCK}));
  Deselect();

  // The queued mode and the bytes for the controllers are kept, so the next access is garbage.
  round_trip();
  Select(0);
  EXPECT_EQ(Command(GARBAGE_TX), GARBAGE_REPLY);
}

TEST_F(PadTest, Multitap_SaveStateDifferentSetting)
{
  PlugMemoryCard(1);
  MemoryCard* const tap_card = PlugMemoryCard(4);

  // Save partway through a Get ID on port 2, without the multitap.
  std::vector<u8> state(4 * sizeof(MemoryCardImage::DataArray));
  Select(1);
  for (const u8 tx : {0x81, 0x53})
    EXPECT_TRUE(Exchange(tx).ack);
  StateWrapper write_sw(std::span<u8>(state), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  ASSERT_TRUE(Pad::DoState(write_sw, false));
  Deselect();

  // Loading it with the multitap on port 2 doesn't send the rest of the command to a card behind the multitap.
  Pad::SetMultitapEnabled(1, true);
  StateWrapper read_sw(std::span<const u8>(state), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
  ASSERT_TRUE(Pad::DoState(read_sw, false));
  Exchange(0x81);
  EXPECT_FALSE(tap_card->IsTransferring());
}

TEST_F(PadTest, Multitap_BothPorts)
{
  Pad::SetMultitapEnabled(0, true);
  Pad::SetMultitapEnabled(1, true);
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
  Pad::SetMultitapEnabled(0, true);
  Pad::SetMultitapEnabled(1, true);

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
  Pad::SetMultitapEnabled(1, true);
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
  Pad::SetMultitapEnabled(0, true);
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

TEST_F(PadTest, QueuedByteAfterNoACK)
{
  PlugController(0);
  Select(0);

  // The last byte of a poll isn't acknowledged, the byte queued behind it is sent straight after.
  for (size_t i = 0; i < POLL.size() - 1; i++)
    ASSERT_TRUE(Exchange(POLL[i]).ack);
  Pad::WriteRegister(JOY_DATA, POLL.back());
  Run(10);
  Pad::WriteRegister(JOY_DATA, 0x01);
  ASSERT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS - 10);
  EXPECT_EQ(Truncate8(Pad::ReadRegister(JOY_DATA)), 0xFF);
  EXPECT_TRUE(Pad::IsTransmitting());
  EXPECT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
  EXPECT_EQ(Truncate8(Pad::ReadRegister(JOY_DATA)), 0xFF);

  // That was the start of a new poll.
  EXPECT_EQ(RunUntil(PadIRQPending, ACK_WINDOW), CONTROLLER_ACK_TICKS);
  AckIRQ();
  EXPECT_EQ(Exchange(0x42).data, 0x41);
  Deselect();

  // Same with nothing connected.
  Select(1);
  Pad::WriteRegister(JOY_DATA, 0x01);
  Run(1);
  Pad::WriteRegister(JOY_DATA, 0x01);
  EXPECT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS - 1);
  Pad::ReadRegister(JOY_DATA);
  EXPECT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS);
  Pad::ReadRegister(JOY_DATA);
  EXPECT_FALSE(Pad::IsTransmitting());
  EXPECT_EQ(Pad::ReadRegister(JOY_STAT) & (STAT_TXRDY | STAT_TXDONE), STAT_TXRDY | STAT_TXDONE);
}

TEST_F(PadTest, RXENClearsAfterByte)
{
  static constexpr u32 CTRL_RXEN = (1u << 2);
  PlugController(0);

  // Transfers don't set it.
  Select(0);
  ASSERT_TRUE(Exchange(0x01).ack);
  EXPECT_EQ(Pad::ReadRegister(JOY_CTRL) & CTRL_RXEN, 0u);
  Deselect();

  // It stays set until a byte is received.
  Select(0, CTRL_ACKINTEN | CTRL_RXEN);
  EXPECT_NE(Pad::ReadRegister(JOY_CTRL) & CTRL_RXEN, 0u);
  Pad::WriteRegister(JOY_DATA, 0x01);
  Run(TRANSFER_TICKS - 1);
  EXPECT_NE(Pad::ReadRegister(JOY_CTRL) & CTRL_RXEN, 0u);
  Run(1);
  EXPECT_EQ(Pad::ReadRegister(JOY_CTRL) & CTRL_RXEN, 0u);
}
