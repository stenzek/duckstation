// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller_test_utils.h"

#include "core/jogcon.h"

#include "util/ini_settings_interface.h"

#include <gtest/gtest.h>

#include <array>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

// These tests check what is currently emulated. Comments starting with "Current behaviour:" mark things that look
// questionable, and may change.

using namespace ControllerTestUtils;

namespace {

using Button = JogCon::Button;
using HalfAxis = JogCon::HalfAxis;

static constexpr const char* SECTION = "Pad1";

static constexpr u32 HalfAxisIndex(HalfAxis axis)
{
  return static_cast<u32>(Button::MaxCount) + static_cast<u32>(axis);
}

static constexpr u32 MOTOR_INDEX = HalfAxisIndex(HalfAxis::MaxCount);
static constexpr u32 LED_INDEX = MOTOR_INDEX + 1;

/// JogCon mode poll reply: buttons, steering (sign extended), then the rotation and the last motor command.
static std::vector<u8> JogReply(u16 buttons, s8 steering = 0, u8 rotation = 0, u8 last_command = 0, u8 id = 0xE3)
{
  return DeviceReply(id, buttons,
                     {static_cast<u8>(steering), static_cast<u8>((steering < 0) ? 0xFF : 0x00),
                      static_cast<u8>(rotation | (last_command << 4)), 0x00});
}

/// Bind value that gives the raw half-axis value with the default sensitivity.
static float SteeringRaw(u8 raw)
{
  return (static_cast<float>(raw) + 0.5f) / (Controller::DEFAULT_STICK_SENSITIVITY * 255.0f);
}

/// Reply with the status byte replaced.
static std::vector<u8> WithStatus(std::vector<u8> reply, u8 status)
{
  reply[2] = status;
  return reply;
}

static std::vector<u8> ConfigPollReply(u16 buttons, s8 steering = 0, u8 rotation = 0, u8 last_command = 0)
{
  return JogReply(buttons, steering, rotation, last_command, 0xF3);
}

class JogConTest : public ControllerSettingsTest
{
protected:
  void SetUp() override
  {
    // So that the mode button doesn't leave DualShock mode, see ModeButton_LeavesDualShockMode.
    ControllerSettingsTest::SetUp();
    SetCanStartInAnalogMode(true);
    LoadDefaultSettings(m_jogcon);
    m_jogcon.Reset();
  }

  void LoadSettings(const INISettingsInterface& si) { m_jogcon.LoadSettings(si, SECTION, false); }
  void Press(Button button, float value = 1.0f) { m_jogcon.SetBindState(static_cast<u32>(button), value); }
  void ToggleMode() { Press(Button::Mode); }
  void SetHalfAxis(HalfAxis axis, float value) { m_jogcon.SetBindState(HalfAxisIndex(axis), value); }
  float Motor() const { return m_jogcon.GetBindState(MOTOR_INDEX); }
  bool ModeLED() const { return m_jogcon.GetBindState(LED_INDEX) != 0.0f; }
  std::vector<u8> Transfer(const std::vector<u8>& tx) { return TransferCommand(m_jogcon, tx); }

  /// 43h polls, so the reply includes the last motor command.
  void EnterConfigMode(u8 last_command = 0)
  {
    EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), JogReply(NO_BUTTONS, 0, 0, last_command));
  }
  void ExitConfigMode() { EXPECT_EQ(Transfer(ConfigCommand(0x43, 0x00)), ANALOG_CONFIG_REPLY); }

  /// Maps the motor to the first byte after the TAP byte in 42h.
  void MapMotor()
  {
    EnterConfigMode();
    EXPECT_EQ(Transfer(MapMotorsCommand({0x00})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    ExitConfigMode();
  }

  static std::unique_ptr<JogCon> MakeJogCon()
  {
    auto jogcon = std::make_unique<JogCon>(0);
    LoadDefaultSettings(*jogcon);
    jogcon->Reset();
    return jogcon;
  }

  JogCon m_jogcon{0};
};

} // namespace

TEST_F(JogConTest, BindingIndices)
{
  EXPECT_EQ(m_jogcon.GetType(), ControllerType::JogCon);
  EXPECT_FALSE(m_jogcon.GetAnalogInputBytes().has_value());

  static constexpr std::pair<const char*, u32> bindings[] = {
    {"Select", 0},
    {"Start", 3},
    {"Up", 4},
    {"Right", 5},
    {"Down", 6},
    {"Left", 7},
    {"L2", 8},
    {"R2", 9},
    {"L1", 10},
    {"R1", 11},
    {"Triangle", 12},
    {"Circle", 13},
    {"Cross", 14},
    {"Square", 15},
    {"Mode", 16},
    {"SteeringLeft", 17},
    {"SteeringRight", 18},
    {"Motor", 19},
    {"ModeLED", 20},
    {"ForceFeedbackDevice", 21},
  };
  for (const auto& [name, index] : bindings)
    EXPECT_EQ(GetBindIndex(JogCon::INFO, name), index) << name;
  EXPECT_EQ(JogCon::INFO.bindings.size(), std::size(bindings));

  for (const char* setting : {"AnalogDeadzone", "AnalogSensitivity", "ButtonDeadzone", "SteeringHoldDeadzone"})
    EXPECT_TRUE(HasSetting(JogCon::INFO, setting)) << setting;
}

TEST_F(JogConTest, Modes)
{
  // Current behaviour: a new JogCon is in digital mode until it's reset.
  JogCon jogcon(0);
  LoadDefaultSettings(jogcon);
  EXPECT_EQ(jogcon.GetBindState(LED_INDEX), 0.0f);
  EXPECT_EQ(TransferCommand(jogcon, POLL), PollReply(NO_BUTTONS));
  jogcon.Reset();
  EXPECT_EQ(jogcon.GetBindState(LED_INDEX), 1.0f);
  EXPECT_EQ(TransferCommand(jogcon, LONG_POLL), JogReply(NO_BUTTONS));

  // The mode button switches to digital mode, where only the buttons are sent.
  EXPECT_TRUE(ModeLED());
  Press(Button::Cross);
  SetHalfAxis(HalfAxis::SteeringRight, 1.0f);
  ToggleMode();
  EXPECT_FALSE(ModeLED());
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Cross})));

  // 43h is only available in JogCon mode.
  EXPECT_EQ(Transfer({0x01, 0x43}), (std::vector<u8>{0xFF, 0x41}));
  m_jogcon.ResetTransferState();

  // Reset goes back to JogCon mode.
  m_jogcon.Reset();
  EXPECT_TRUE(ModeLED());
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(Pressed({Button::Cross}), 127, 0));
}

TEST_F(JogConTest, RejectedBytes)
{
  ExpectRejectedBytes(m_jogcon, {0x00, 0x40, 0x44, 0x45, 0x46, 0x47, 0x4C, 0x4D, 0x53}, 0xE3, LONG_POLL,
                      JogReply(NO_BUTTONS));

  EnterConfigMode();
  ExpectRejectedBytes(m_jogcon, {0x00, 0x3F, 0x50, 0x53}, 0xF3, LONG_POLL, ConfigPollReply(NO_BUTTONS));
}

TEST_F(JogConTest, ResetTransferState)
{
  Press(Button::Triangle);
  for (size_t len = 1; len < LONG_POLL.size(); len++)
  {
    TransferBytes(m_jogcon, std::vector<u8>(LONG_POLL.begin(), LONG_POLL.begin() + len));
    m_jogcon.ResetTransferState();
    EXPECT_EQ(Transfer(LONG_POLL), JogReply(Pressed({Button::Triangle}))) << len;
  }
}

TEST_F(JogConTest, Buttons)
{
  for (u32 i = 0; i < static_cast<u32>(Button::Mode); i++)
  {
    const Button button = static_cast<Button>(i);
    Press(button, 0.24f);
    EXPECT_EQ(m_jogcon.GetBindState(i), 0.0f) << i;
    EXPECT_EQ(m_jogcon.GetButtonStateBits(), 0u);

    Press(button, 0.25f);
    EXPECT_EQ(m_jogcon.GetBindState(i), 1.0f) << i;
    EXPECT_EQ(m_jogcon.GetButtonStateBits(), ButtonBit(button));
    EXPECT_EQ(Transfer(LONG_POLL), JogReply(Pressed({button}))) << i;

    Press(button, 0.0f);
    EXPECT_EQ(m_jogcon.GetButtonStateBits(), 0u);
  }
}

TEST_F(JogConTest, MemberDefaults)
{
  // Current behaviour: without LoadSettings(), the button deadzone is 0, so any value presses a button or toggles the
  // mode.
  JogCon jogcon(0);
  jogcon.Reset();
  jogcon.SetBindState(static_cast<u32>(Button::Cross), 0.0f);
  EXPECT_EQ(jogcon.GetButtonStateBits(), ButtonBit(Button::Cross));
  jogcon.SetBindState(static_cast<u32>(Button::Mode), 0.0f);
  EXPECT_EQ(jogcon.GetBindState(LED_INDEX), 0.0f);
}

TEST_F(JogConTest, Steering)
{
  // With the default sensitivity of 1.33. Right is value / 2, left is -(value + 1) / 2.
  SetHalfAxis(HalfAxis::SteeringRight, 0.5f);
  EXPECT_FLOAT_EQ(m_jogcon.GetBindState(HalfAxisIndex(HalfAxis::SteeringRight)), 169.0f / 255.0f);
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS, 84, 1));

  // The rotation is relative to the last poll.
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS, 84, 0));
  SetHalfAxis(HalfAxis::SteeringRight, 1.0f);
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS, 127, 1));
  SetHalfAxis(HalfAxis::SteeringRight, 0.0f);
  SetHalfAxis(HalfAxis::SteeringLeft, 0.5f);
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS, -85, 2));
  SetHalfAxis(HalfAxis::SteeringLeft, 1.0f);
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS, -128, 2));

  // Right wins over left.
  SetHalfAxis(HalfAxis::SteeringRight, 0.1f);
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS, 16, 1));
  SetHalfAxis(HalfAxis::SteeringRight, 0.0f);
  SetHalfAxis(HalfAxis::SteeringLeft, 0.0f);
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS, 0, 2));

  // A poll that's cut short still updates the last position.
  SetHalfAxis(HalfAxis::SteeringRight, 1.0f);
  TransferBytes(m_jogcon, {0x01, 0x42});
  m_jogcon.ResetTransferState();
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS, 127, 0));

  // The half axes don't press any buttons.
  EXPECT_EQ(m_jogcon.GetButtonStateBits(), 0u);

  // Sensitivity of 1 and a deadzone.
  INISettingsInterface si;
  si.SetFloatValue(SECTION, "AnalogSensitivity", 1.0f);
  si.SetFloatValue(SECTION, "AnalogDeadzone", 0.3f);
  LoadSettings(si);
  SetHalfAxis(HalfAxis::SteeringRight, 0.29f);
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS, 0, 2));
  SetHalfAxis(HalfAxis::SteeringRight, 0.5f);
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS, 63, 1));
}

TEST_F(JogConTest, ConfigMode)
{
  Press(Button::Circle);
  SetHalfAxis(HalfAxis::SteeringRight, 1.0f);

  // 43h polls like 42h, and entering is applied from the parameter byte.
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), JogReply(Pressed({Button::Circle}), 127, 1));
  EXPECT_EQ(Transfer(LONG_POLL), ConfigPollReply(Pressed({Button::Circle}), 127, 0));

  // In config mode 43h replies with zeros. Parameters other than 01h leave.
  EXPECT_EQ(Transfer(ConfigCommand(0x43, 0x01)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(ConfigCommand(0x43, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(Pressed({Button::Circle}), 127, 0));

  // An aborted 43h doesn't enter config mode, it's applied at the end of the command.
  TransferBytes(m_jogcon, {0x01, 0x43, 0x00, 0x01});
  m_jogcon.ResetTransferState();
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(Pressed({Button::Circle}), 127, 0));
  TransferBytes(m_jogcon, {0x01, 0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00});
  m_jogcon.ResetTransferState();
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(Pressed({Button::Circle}), 127, 0));
}

TEST_F(JogConTest, SetMode)
{
  EnterConfigMode();

  // 03h locks the mode, so the button does nothing until it's unlocked.
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x03)), ANALOG_CONFIG_REPLY);
  ToggleMode();
  EXPECT_TRUE(ModeLED());
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x02)), ANALOG_CONFIG_REPLY);
  ToggleMode();
  EXPECT_FALSE(ModeLED());
  ToggleMode();
  EXPECT_TRUE(ModeLED());

  // Toggling the mode stayed in config mode, but cleared the status byte until config mode is entered again.
  EXPECT_EQ(Transfer(ConfigCommand(0x43, 0x01)), WithStatus(ANALOG_CONFIG_REPLY, 0x00));

  // Values other than 00h/01h are ignored.
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x02, 0x00)), ANALOG_CONFIG_REPLY);
  EXPECT_TRUE(ModeLED());

  // Switching to digital mode stays in config mode, with the full reply, and 43h leaves it.
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x00, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_FALSE(ModeLED());
  EXPECT_EQ(Transfer(LONG_POLL), ConfigPollReply(NO_BUTTONS));
  ExitConfigMode();
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));

  // 43h can't enter config mode from digital mode.
  EXPECT_EQ(Transfer({0x01, 0x43}), (std::vector<u8>{0xFF, 0x41}));
  m_jogcon.ResetTransferState();
}

TEST_F(JogConTest, ConfigTables)
{
  EnterConfigMode();

  EXPECT_EQ(Transfer(ConfigCommand(0x45, 0x00)), ConfigReply({0x01, 0x02, 0x01, 0x01, 0x01, 0x00}));

  static constexpr struct
  {
    u8 command;
    u8 param;
    std::array<u8, 6> reply;
  } cases[] = {
    {0x46, 0x00, {0x00, 0x00, 0x01, 0x02, 0x00, 0x0A}}, {0x46, 0x01, {0x00, 0x00, 0x01, 0x01, 0x01, 0x14}},
    {0x46, 0x02, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, {0x47, 0x00, {0x00, 0x00, 0x02, 0x00, 0x01, 0x00}},
    {0x47, 0x01, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, {0x4C, 0x00, {0x00, 0x00, 0x00, 0x04, 0x00, 0x00}},
    {0x4C, 0x01, {0x00, 0x00, 0x03, 0x00, 0x00, 0x00}}, {0x4C, 0x02, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
  };
  for (const auto& c : cases)
  {
    EXPECT_EQ(Transfer(ConfigCommand(c.command, c.param)),
              ConfigReply({c.reply[0], c.reply[1], c.reply[2], c.reply[3], c.reply[4], c.reply[5]}))
      << std::hex << u32(c.command) << " " << u32(c.param);
  }

  // 45h can't report digital mode, since leaving JogCon mode also leaves config mode.
}

TEST_F(JogConTest, MapMotors)
{
  EnterConfigMode();

  // The previous mapping is sent back.
  EXPECT_EQ(Transfer(MapMotorsCommand({0x00, 0x01, 0x02, 0x03, 0x04, 0x05})),
            ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Transfer(MapMotorsCommand({0xFF, 0x00})), ConfigReply({0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));
  EXPECT_EQ(Transfer(MapMotorsCommand({})), ConfigReply({0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF}));

  // 44h resets the mapping.
  EXPECT_EQ(Transfer(MapMotorsCommand({0x00})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(MapMotorsCommand({})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST_F(JogConTest, MotorCommands)
{
  MapMotor();

  // The high nibble is the command and the low nibble the strength. The command is echoed in the next poll.
  EXPECT_EQ(Transfer(RumblePoll({0x1F})), JogReply(NO_BUTTONS));
  EXPECT_FLOAT_EQ(Motor(), 1.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x28})), JogReply(NO_BUTTONS, 0, 0, 0x1));
  EXPECT_FLOAT_EQ(Motor(), 8.0f / 15.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x20})), JogReply(NO_BUTTONS, 0, 0, 0x2));
  EXPECT_EQ(Motor(), 0.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x13})), JogReply(NO_BUTTONS, 0, 0, 0x2));
  EXPECT_FLOAT_EQ(Motor(), 3.0f / 15.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x0F})), JogReply(NO_BUTTONS, 0, 0, 0x1));
  EXPECT_EQ(Motor(), 0.0f);

  // Drop revolutions isn't handled, other than being echoed.
  EXPECT_EQ(Transfer(RumblePoll({0x8F})), JogReply(NO_BUTTONS, 0, 0, 0x0));
  EXPECT_EQ(Motor(), 0.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x00})), JogReply(NO_BUTTONS, 0, 0, 0x8));

  // Only the mapped byte is used.
  EXPECT_EQ(Transfer({0x01, 0x42, 0x00, 0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}), JogReply(NO_BUTTONS));
  EXPECT_EQ(Motor(), 0.0f);

  // Unmapping stops the motor.
  EXPECT_EQ(Transfer(RumblePoll({0x1F})), JogReply(NO_BUTTONS));
  EXPECT_FLOAT_EQ(Motor(), 1.0f);
  EnterConfigMode(0x1);
  EXPECT_EQ(Motor(), 1.0f);
  EXPECT_EQ(Transfer(MapMotorsCommand({0xFF})), ConfigReply({0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Motor(), 0.0f);

  // So does 44h.
  EXPECT_EQ(Transfer(MapMotorsCommand({0x00})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  ExitConfigMode();
  EXPECT_EQ(Transfer(RumblePoll({0x1F})), JogReply(NO_BUTTONS));
  EnterConfigMode(0x1);
  EXPECT_FLOAT_EQ(Motor(), 1.0f);
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Motor(), 0.0f);
}

TEST_F(JogConTest, MotorHold)
{
  MapMotor();

  // Holding at the default position (0) does nothing while the wheel is there, within the deadzone of 4.
  EXPECT_EQ(Transfer(RumblePoll({0x35})), JogReply(NO_BUTTONS));
  EXPECT_EQ(Motor(), 0.0f);
  SetHalfAxis(HalfAxis::SteeringRight, SteeringRaw(6));
  EXPECT_EQ(Transfer(RumblePoll({0x35})), JogReply(NO_BUTTONS, 3, 1, 0x3));
  EXPECT_EQ(Motor(), 0.0f);

  // Further away, the motor pushes back towards it. The direction is only updated by a poll.
  SetHalfAxis(HalfAxis::SteeringRight, 1.0f);
  EXPECT_EQ(Motor(), 0.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x35})), JogReply(NO_BUTTONS, 127, 1, 0x3));
  EXPECT_FLOAT_EQ(Motor(), 5.0f / 15.0f);
  SetHalfAxis(HalfAxis::SteeringRight, 0.0f);
  SetHalfAxis(HalfAxis::SteeringLeft, 1.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x37})), JogReply(NO_BUTTONS, -128, 2, 0x3));
  EXPECT_FLOAT_EQ(Motor(), 7.0f / 15.0f);

  // The hold follows the wheel.
  SetHalfAxis(HalfAxis::SteeringLeft, 0.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x37})), JogReply(NO_BUTTONS, 0, 1, 0x3));
  EXPECT_EQ(Motor(), 0.0f);
  SetHalfAxis(HalfAxis::SteeringLeft, 1.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x37})), JogReply(NO_BUTTONS, -128, 2, 0x3));
  EXPECT_FLOAT_EQ(Motor(), 7.0f / 15.0f);

  // Ch sets a new hold position at the current steering, without changing the motor.
  EXPECT_EQ(Transfer(RumblePoll({0xC0})), JogReply(NO_BUTTONS, -128, 0, 0x3));
  EXPECT_FLOAT_EQ(Motor(), 7.0f / 15.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x37})), JogReply(NO_BUTTONS, -128, 0, 0xC));
  EXPECT_EQ(Motor(), 0.0f);
  SetHalfAxis(HalfAxis::SteeringLeft, 0.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x37})), JogReply(NO_BUTTONS, 0, 1, 0x3));
  EXPECT_FLOAT_EQ(Motor(), 7.0f / 15.0f);

  // Stop ends the hold, and 00h in the mapped byte is a stop.
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS, 0, 0, 0x3));
  EXPECT_EQ(Motor(), 0.0f);
  SetHalfAxis(HalfAxis::SteeringRight, 1.0f);
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS, 127, 1, 0x0));
  EXPECT_EQ(Motor(), 0.0f);

  // A wider deadzone.
  INISettingsInterface si;
  si.SetFloatValue(SECTION, "SteeringHoldDeadzone", 0.5f);
  LoadSettings(si);
  SetHalfAxis(HalfAxis::SteeringRight, 0.0f);
  EXPECT_EQ(Transfer(RumblePoll({0xC0})), JogReply(NO_BUTTONS, 0, 2, 0x0));
  SetHalfAxis(HalfAxis::SteeringRight, SteeringRaw(126));
  EXPECT_EQ(Transfer(RumblePoll({0x3F})), JogReply(NO_BUTTONS, 63, 1, 0xC));
  EXPECT_EQ(Motor(), 0.0f);
  SetHalfAxis(HalfAxis::SteeringRight, SteeringRaw(128));
  EXPECT_EQ(Transfer(RumblePoll({0x3F})), JogReply(NO_BUTTONS, 64, 1, 0x3));
  EXPECT_FLOAT_EQ(Motor(), 1.0f);
}

TEST_F(JogConTest, ModeButton)
{
  // Toggles straight away when idle.
  Press(Button::Mode, 0.24f);
  EXPECT_TRUE(ModeLED());
  ToggleMode();
  EXPECT_FALSE(ModeLED());
  EXPECT_EQ(m_jogcon.GetBindState(static_cast<u32>(Button::Mode)), 0.0f);
  ToggleMode();
  EXPECT_TRUE(ModeLED());

  // Partway through a command, it waits for the transfer to be reset.
  TransferBytes(m_jogcon, {0x01});
  ToggleMode();
  EXPECT_TRUE(ModeLED());
  const std::vector<u8> reply = JogReply(NO_BUTTONS);
  EXPECT_EQ(Transfer({0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
            std::vector<u8>(reply.begin() + 1, reply.end()));
  EXPECT_TRUE(ModeLED());
  m_jogcon.ResetTransferState();
  EXPECT_FALSE(ModeLED());
  m_jogcon.ResetTransferState();
  EXPECT_FALSE(ModeLED());

  // Switching modes in config mode stays in it, and clears the status byte until config mode is entered again.
  ToggleMode();
  EnterConfigMode();
  ToggleMode();
  ToggleMode();
  EXPECT_EQ(Transfer(LONG_POLL), WithStatus(ConfigPollReply(NO_BUTTONS), 0x00));
  ToggleMode();
  EXPECT_EQ(Transfer(LONG_POLL), WithStatus(ConfigPollReply(NO_BUTTONS), 0x00));
  EXPECT_EQ(Transfer(ConfigCommand(0x43, 0x00)), WithStatus(ANALOG_CONFIG_REPLY, 0x00));
  EXPECT_EQ(Transfer(POLL), WithStatus(PollReply(NO_BUTTONS), 0x00));
}

TEST_F(JogConTest, ModeButton_LeavesDualShockMode)
{
  // If the game can't start in analog mode, switching to digital mode leaves config and DualShock mode.
  SetCanStartInAnalogMode(false);
  EnterConfigMode();
  ToggleMode();
  EXPECT_FALSE(ModeLED());
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));

  // Switching back doesn't change the status byte, since DualShock mode was left.
  ToggleMode();
  EXPECT_EQ(Transfer(LONG_POLL), JogReply(NO_BUTTONS));
}

TEST_F(JogConTest, SaveState)
{
  Press(Button::Start);

  // Partway through a poll, the rest of it continues after loading.
  ExpectResumesAfterLoad(m_jogcon, MakeJogCon, LONG_POLL, JogReply(Pressed({Button::Start})));
  Press(Button::Start, 0.0f);

  MapMotor();
  Press(Button::Start);
  SetHalfAxis(HalfAxis::SteeringLeft, 0.5f);
  EXPECT_EQ(Transfer(RumblePoll({0x13})), JogReply(Pressed({Button::Start}), -85, 2));
  EXPECT_EQ(Transfer(RumblePoll({0xC0})), JogReply(Pressed({Button::Start}), -85, 0, 0x1));

  const std::vector<u8> state = SaveState(m_jogcon);
  {
    const auto loaded = MakeJogCon();
    LoadState(*loaded, state, true);

    // The force is restored, without replaying the new hold position command.
    EXPECT_FLOAT_EQ(loaded->GetBindState(MOTOR_INDEX), 3.0f / 15.0f);
    EXPECT_EQ(TransferCommand(*loaded, RumblePoll({0x1A})), JogReply(Pressed({Button::Start}), -85, 0, 0xC));
    EXPECT_EQ(TransferCommand(m_jogcon, RumblePoll({0x1A})), JogReply(Pressed({Button::Start}), -85, 0, 0xC));
    EXPECT_FLOAT_EQ(loaded->GetBindState(MOTOR_INDEX), 10.0f / 15.0f);
    EXPECT_FLOAT_EQ(Motor(), 10.0f / 15.0f);
  }

  // Input state is only loaded if asked to.
  {
    const auto loaded = MakeJogCon();
    loaded->SetBindState(static_cast<u32>(Button::Square), 1.0f);
    loaded->SetBindState(HalfAxisIndex(HalfAxis::SteeringRight), 1.0f);
    LoadState(*loaded, state, false);
    EXPECT_EQ(TransferCommand(*loaded, LONG_POLL), JogReply(Pressed({Button::Square}), 127, 1, 0xC));
  }

  // The mode is always loaded, and the status byte cleared by toggling it in DualShock mode.
  ToggleMode();
  const std::vector<u8> digital_state = SaveState(m_jogcon);
  {
    const auto loaded = MakeJogCon();
    LoadState(*loaded, digital_state, false);
    EXPECT_EQ(loaded->GetBindState(LED_INDEX), 0.0f);
    EXPECT_EQ(TransferCommand(*loaded, POLL), WithStatus(PollReply(NO_BUTTONS), 0x00));
  }
}

TEST_F(JogConTest, LoadStateStopsHoldForce)
{
  // Loading a state holding with no strength, or with the motor stopped, stops a hold force.
  const std::vector<u8> stopped_state = SaveState(m_jogcon);
  MapMotor();
  Transfer(RumblePoll({0x30}));
  const std::vector<u8> zero_hold_state = SaveState(m_jogcon);
  for (const std::vector<u8>* state : {&zero_hold_state, &stopped_state})
  {
    SetHalfAxis(HalfAxis::SteeringRight, 0.0f);
    SetHalfAxis(HalfAxis::SteeringRight, 1.0f);
    Transfer(RumblePoll({0x3F}));
    EXPECT_FLOAT_EQ(Motor(), 1.0f);
    LoadState(m_jogcon, *state, true);
    EXPECT_EQ(Motor(), 0.0f);
  }

  // Holding with no strength stops the motor.
  MapMotor();
  Transfer(RumblePoll({0x1F}));
  EXPECT_FLOAT_EQ(Motor(), 1.0f);
  Transfer(RumblePoll({0x30}));
  EXPECT_EQ(Motor(), 0.0f);
}

TEST_F(JogConTest, LegacyState)
{
  // Saved by version 104 partway through a poll, in JogCon mode with the motor mapped, Start pressed, the wheel
  // turned left, and a hold position set.
  static const std::vector<u8> state = {0xF7, 0xFF, 0xAB, 0x02, 0x03, 0x5A, 0xAB, 0x00, 0xAB, 0x00, 0x00, 0x01,
                                        0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE3, 0x5A, 0xF7, 0xFF,
                                        0xAB, 0xFF, 0xC0, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  ToggleMode();
  LoadState(m_jogcon, state, true, 104);
  EXPECT_TRUE(ModeLED());

  // The transfer starts again from idle, and the mapped motor works without entering DualShock mode again.
  EXPECT_EQ(Transfer(RumblePoll({0x1F})), JogReply(Pressed({Button::Start}), -85));
  EXPECT_FLOAT_EQ(Motor(), 1.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x00})), JogReply(Pressed({Button::Start}), -85, 0, 0x1));
  EXPECT_EQ(Motor(), 0.0f);

  // Holding uses the saved position, so the wheel is pushed back to the left when turned right.
  SetHalfAxis(HalfAxis::SteeringLeft, 0.0f);
  SetHalfAxis(HalfAxis::SteeringRight, 0.5f);
  EXPECT_EQ(Transfer(RumblePoll({0x3F})), JogReply(Pressed({Button::Start}), 84, 1));
  EXPECT_FLOAT_EQ(Motor(), 1.0f);

  // Only the first five motor bytes were stored, and there was no lock.
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), JogReply(Pressed({Button::Start}), 84, 0, 0x3));
  EXPECT_EQ(Transfer(MapMotorsCommand({})), ConfigReply({0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  ExitConfigMode();
  ToggleMode();
  EXPECT_FALSE(ModeLED());

  // Saved in config mode before the motor was mapped, the motor works after mapping it.
  std::vector<u8> config_state = state;
  config_state[10] = 0x01;
  config_state[28] = 0xFF;
  LoadState(m_jogcon, config_state, true, 104);
  EXPECT_EQ(Transfer(MapMotorsCommand({0x00})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  ExitConfigMode();
  EXPECT_EQ(Transfer(RumblePoll({0x1F})), JogReply(Pressed({Button::Start}), -85));
  EXPECT_FLOAT_EQ(Motor(), 1.0f);

  // A saved hold force is stopped by loading a state with the motor stopped.
  std::vector<u8> hold_state = state;
  hold_state[8] = 0x00;
  hold_state[9] = 0x0F;
  hold_state[28] = 0xFF;
  const auto held = MakeJogCon();
  LoadState(*held, hold_state, true, 104);
  TransferCommand(*held, LONG_POLL);
  EXPECT_FLOAT_EQ(held->GetBindState(MOTOR_INDEX), 1.0f);
  LoadState(*held, SaveState(*MakeJogCon()), true);
  EXPECT_EQ(held->GetBindState(MOTOR_INDEX), 0.0f);

  // Loading a state without a hold stops the motor, and saving it again keeps it stopped.
  LoadState(*held, hold_state, true, 104);
  TransferCommand(*held, LONG_POLL);
  EXPECT_FLOAT_EQ(held->GetBindState(MOTOR_INDEX), 1.0f);
  LoadState(*held, config_state, true, 104);
  EXPECT_EQ(held->GetBindState(MOTOR_INDEX), 0.0f);
  LoadState(*held, SaveState(*held), true);
  EXPECT_EQ(held->GetBindState(MOTOR_INDEX), 0.0f);
}
