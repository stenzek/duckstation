// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// Helpers shared by the controller, memory card and pad tests.

#pragma once

#include "core/analog_controller.h"
#include "core/controller.h"
#include "core/cpu_core.h"
#include "core/digital_controller.h"
#include "core/interrupt_controller.h"
#include "core/memory_card.h"
#include "core/pad.h"
#include "core/save_state_version.h"
#include "core/settings.h"
#include "core/system.h"
#include "core/timing_event.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string_view>
#include <vector>

namespace ControllerTestUtils {

using Block = std::vector<u8>;

inline constexpr u16 NO_BUTTONS = 0xFFFF;

inline const std::vector<u8> POLL = {0x01, 0x42, 0x00, 0x00, 0x00};

/// Reply to 43h/44h in config mode, padded to the full analog length.
inline const std::vector<u8> ANALOG_CONFIG_REPLY = {0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/// Full length commands, for analog and config mode.
inline const std::vector<u8> LONG_POLL = {0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
inline const std::vector<u8> ENTER_CONFIG_LONG = {0x01, 0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};

/// Rumble motor strength for a raw value plus the bias, using the same curve as the controllers.
constexpr float MotorStrength(u32 value)
{
  const double x = static_cast<double>(value);
  return static_cast<float>(
    (0.006474549734772402 * x * x * x - 1.258165252213538 * x * x + 156.82454281087692 * x + 3.637978807091713e-11) /
    65535.0);
}

/// Bit for a button, where the enum value is the bit number.
template<typename E>
constexpr u16 ButtonBit(E button)
{
  return static_cast<u16>(1u << static_cast<u8>(button));
}

/// Active low button state with the given buttons pressed.
template<typename E>
u16 Pressed(std::initializer_list<E> buttons)
{
  u16 ret = NO_BUTTONS;
  for (const E button : buttons)
    ret &= ~ButtonBit(button);
  return ret;
}

/// Bind value that gives the raw half-axis value with a sensitivity of 1.
float RawHalfAxis(u8 raw);

/// Reply to a poll: Hi-Z, the ID byte, 5Ah, the buttons, then any extra bytes.
std::vector<u8> DeviceReply(u8 id, u16 buttons, std::initializer_list<u8> extra = {});

/// Digital pad poll, with the replies for the given button state.
std::vector<u8> PollReply(u16 buttons);

/// Analog mode poll, with the replies for the given button state and axes (in the order they're sent).
std::vector<u8> AnalogPollReply(u16 buttons, u8 rx = Controller::AXIS_CENTER, u8 ry = Controller::AXIS_CENTER,
                                u8 lx = Controller::AXIS_CENTER, u8 ly = Controller::AXIS_CENTER);

/// Multitap all-controllers blocks for a digital pad and an analog pad in analog mode with centred sticks.
Block DigitalBlock(u16 buttons);
Block AnalogBlock(u16 buttons);

/// Analog controller config mode commands. first is 0x01, or the multitap sub-port.
/// Entering config mode from digital mode is a short command, the others are sent in config mode so are full length.
std::vector<u8> EnterConfigModeCommand(u8 first = 0x01);
std::vector<u8> ExitConfigModeCommand(u8 first = 0x01);
std::vector<u8> SetAnalogModeCommand(u8 first = 0x01, u8 analog = 0x01, u8 lock = 0x03);

/// Full length config mode command with one parameter.
std::vector<u8> ConfigCommand(u8 command, u8 param);

/// Full length config mode reply (ID F3h), with the given bytes after 5Ah, padded with 00h.
std::vector<u8> ConfigReply(std::initializer_list<u8> data);

/// 42h with the given bytes in the motor positions.
std::vector<u8> RumblePoll(std::initializer_list<u8> motor_bytes, size_t length = 9);

/// 4Dh with the given mapping, padded with FFh.
std::vector<u8> MapMotorsCommand(std::initializer_list<u8> mapping);

/// Sends bytes to a controller without checking the ACKs, optionally returning them.
std::vector<u8> TransferBytes(Controller& controller, const std::vector<u8>& tx, std::vector<bool>* acks = nullptr);

/// Sends a whole command and returns the replies. Every byte except the last should ACK.
std::vector<u8> TransferCommand(Controller& controller, const std::vector<u8>& tx);

/// Switches an analog controller in digital mode to analog mode through config mode, locking it.
void EnterAnalogMode(Controller& controller);

/// Checks that first bytes other than 01h are rejected with FFh, and that the given commands reply with
/// bad_command_reply without an ACK. The poll is sent after each, to check the controller still responds.
void ExpectRejectedBytes(Controller& controller, std::initializer_list<u8> bad_commands, u8 bad_command_reply,
                         const std::vector<u8>& poll, const std::vector<u8>& poll_reply);

/// Saves the controller state with the current version, returning only the bytes that were written.
std::vector<u8> SaveState(Controller& controller);

/// Loads a state written by SaveState(), or by an older version, checking that all of it is read.
void LoadState(Controller& controller, const std::vector<u8>& data, bool apply_input_state,
               u32 version = SAVE_STATE_VERSION);

/// Saves source partway through the command at every split point, loads the state into a fresh controller from
/// make_fresh(), and checks that the rest of the command gives the rest of the replies.
template<typename Make>
void ExpectResumesAfterLoad(Controller& source, const Make& make_fresh, const std::vector<u8>& tx,
                            const std::vector<u8>& rx)
{
  for (size_t len = 1; len < tx.size(); len++)
  {
    std::vector<bool> acks;
    TransferBytes(source, std::vector<u8>(tx.begin(), tx.begin() + len), &acks);
    EXPECT_EQ(acks, std::vector<bool>(len, true)) << "after " << len;
    const std::vector<u8> state = SaveState(source);
    source.ResetTransferState();

    const auto loaded = make_fresh();
    LoadState(*loaded, state, true);
    EXPECT_EQ(TransferCommand(*loaded, std::vector<u8>(tx.begin() + len, tx.end())),
              std::vector<u8>(rx.begin() + len, rx.end()))
      << "after " << len;
  }
}

/// Loads the settings with an empty settings interface, so the defaults from LoadSettings() are used.
void LoadDefaultSettings(Controller& controller);

const Controller::ControllerBindingInfo* FindBinding(const Controller::ControllerInfo& info, std::string_view name);
bool HasSetting(const Controller::ControllerInfo& info, std::string_view name);

/// Returns the bind index of a named binding, failing the test if it doesn't exist.
u32 GetBindIndex(const Controller::ControllerInfo& info, std::string_view name);

/// Saves and restores g_settings, for controllers that read it.
class ControllerSettingsTest : public ::testing::Test
{
protected:
  void SetUp() override;
  void TearDown() override;

  /// Without a game, CanStartInAnalogMode() is only true with compatibility settings off.
  static void SetCanStartInAnalogMode(bool enabled);

  Settings m_old_settings;
};

/// Drives the controller ports through the SIO0 registers.
class PadTest : public ::testing::Test
{
protected:
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

  struct Reply
  {
    u8 data;
    bool ack;
    u32 ack_ticks; // after RX, or NOT_REACHED
  };

  static void SetUpTestSuite();
  void SetUp() override;
  void TearDown() override;

  static void DeactivateAllEvents();

  /// Runs the CPU for the specified number of ticks, without any register accesses.
  static void Run(u32 ticks);

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
  static bool RXReady();

  /// Checks the interrupt controller without touching the pad registers.
  static bool PadIRQPending();

  static void PlugController(u32 index);
  static void PlugAnalogController(u32 index);
  static MemoryCard* PlugMemoryCard(u32 index);
  static void Press(u32 index, DigitalController::Button button);

  void Select(u32 slot, u32 extra_bits = CTRL_ACKINTEN);
  static void Deselect();

  /// Acknowledges the interrupt in both the pad and the interrupt controller, like the BIOS does.
  void AckIRQ();

  /// Sends one byte, waits for the reply and the ACK (if any).
  Reply Exchange(u8 value);

  /// Sends a whole command and returns the replies. Every byte except the last should ACK.
  /// The time from each RX to its ACK is left in m_ack_ticks.
  std::vector<u8> Command(const std::vector<u8>& tx);

  /// Switches an analog controller to analog mode through config mode. first is 0x01, or the multitap sub-port.
  void EnterAnalogMode(u32 slot, u8 first);

  TimingEvent m_idle_event{"Idle", 1000000, 1000000, [](void*, TickCount) {}, nullptr};
  u32 m_ctrl = 0;
  std::vector<u32> m_ack_ticks;
};

} // namespace ControllerTestUtils
