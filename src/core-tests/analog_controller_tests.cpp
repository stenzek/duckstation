// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller_test_utils.h"

#include "core/analog_controller.h"
#include "core/settings.h"

#include "util/ini_settings_interface.h"

#include <gtest/gtest.h>

#include <array>
#include <iterator>
#include <utility>
#include <vector>

// These tests check what is currently emulated. Comments starting with "Current behaviour:" mark things that look
// questionable, and may change.

using namespace ControllerTestUtils;

namespace {

using Button = AnalogController::Button;
using HalfAxis = AnalogController::HalfAxis;

static constexpr const char* SECTION = "Pad1";

static constexpr u32 HalfAxisIndex(HalfAxis axis)
{
  return static_cast<u32>(Button::Count) + static_cast<u32>(axis);
}

class AnalogControllerTest : public ControllerSettingsTest
{
protected:
  void SetUp() override
  {
    ControllerSettingsTest::SetUp();
    LoadDefaultSettings(m_controller);
  }

  void LoadSettings(const INISettingsInterface& si) { m_controller.LoadSettings(si, SECTION, false); }

  void Press(Button button, float value = 1.0f) { m_controller.SetBindState(static_cast<u32>(button), value); }
  void ToggleAnalog() { Press(Button::Analog); }
  void SetHalfAxis(HalfAxis axis, float value) { m_controller.SetBindState(HalfAxisIndex(axis), value); }

  float SmallMotor() const { return m_controller.GetBindState(GetBindIndex(AnalogController::INFO, "SmallMotor")); }
  float LargeMotor() const { return m_controller.GetBindState(GetBindIndex(AnalogController::INFO, "LargeMotor")); }
  bool AnalogLED() const
  {
    return m_controller.GetBindState(GetBindIndex(AnalogController::INFO, "AnalogLED")) != 0.0f;
  }

  std::vector<u8> Transfer(const std::vector<u8>& tx) { return TransferCommand(m_controller, tx); }

  /// Enters config mode from digital mode, sets the analog mode without locking it, and maps the motors.
  void SetupDualShock(bool analog)
  {
    EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
    EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, analog ? 0x01 : 0x00, 0x02)), ANALOG_CONFIG_REPLY);
    EXPECT_EQ(Transfer(MapMotorsCommand({0x00, 0x01})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  }

  AnalogController m_controller{0};
};

} // namespace

TEST_F(AnalogControllerTest, BindingIndices)
{
  EXPECT_EQ(m_controller.GetType(), ControllerType::AnalogController);

  static constexpr std::pair<const char*, u32> bindings[] = {
    {"Select", 0},    {"L3", 1},          {"R3", 2},          {"Start", 3},      {"Up", 4},      {"Right", 5},
    {"Down", 6},      {"Left", 7},        {"L2", 8},          {"R2", 9},         {"L1", 10},     {"R1", 11},
    {"Triangle", 12}, {"Circle", 13},     {"Cross", 14},      {"Square", 15},    {"Analog", 16}, {"LLeft", 17},
    {"LRight", 18},   {"LDown", 19},      {"LUp", 20},        {"RLeft", 21},     {"RRight", 22}, {"RDown", 23},
    {"RUp", 24},      {"SmallMotor", 25}, {"LargeMotor", 26}, {"AnalogLED", 27},
  };
  EXPECT_EQ(AnalogController::INFO.bindings.size(), std::size(bindings));
  for (const auto& [name, index] : bindings)
    EXPECT_EQ(GetBindIndex(AnalogController::INFO, name), index) << name;

  for (const char* name : {"ForceAnalogOnReset", "AnalogDPadInDigitalMode", "DisableSOCD", "AnalogShoulderButtons",
                           "AnalogTriggerButtons", "AnalogDeadzone", "AnalogSensitivity", "ButtonDeadzone",
                           "LargeMotorVibrationBias", "SmallMotorVibrationBias", "InvertLeftStick", "InvertRightStick"})
  {
    EXPECT_TRUE(HasSetting(AnalogController::INFO, name)) << name;
  }
}

TEST_F(AnalogControllerTest, DigitalModePoll)
{
  EXPECT_FALSE(AnalogLED());
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));

  // L3/R3 are only sent in analog mode, but are still reported.
  Press(Button::Cross);
  Press(Button::L3);
  Press(Button::R3);
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Cross})));
  EXPECT_EQ(m_controller.GetButtonStateBits(),
            static_cast<u32>(ButtonBit(Button::Cross) | ButtonBit(Button::L3) | ButtonBit(Button::R3)));

  // 43h that doesn't enter config mode is a poll too.
  EXPECT_EQ(Transfer({0x01, 0x43, 0x00, 0x00, 0x00}), PollReply(Pressed({Button::Cross})));
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Cross})));

  // The sticks aren't sent, but are still reported.
  SetHalfAxis(HalfAxis::RRight, 1.0f);
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Cross})));
  EXPECT_EQ(m_controller.GetAnalogInputBytes(), 0x7F7F7FFFu);

  // Config mode sends them, as well as the sticks.
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(Pressed({Button::Cross})));
  std::vector<u8> expected = AnalogPollReply(Pressed({Button::Cross, Button::L3, Button::R3}), 0xFF);
  expected[1] = 0xF3;
  EXPECT_EQ(Transfer(LONG_POLL), expected);

  // As does analog mode.
  EXPECT_EQ(Transfer(SetAnalogModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(Pressed({Button::Cross, Button::L3, Button::R3}), 0xFF));
}

TEST_F(AnalogControllerTest, AnalogModePoll)
{
  EnterAnalogMode(m_controller);
  EXPECT_TRUE(AnalogLED());
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));

  // RX, RY, LX, LY.
  Press(Button::L1);
  SetHalfAxis(HalfAxis::RRight, 1.0f);
  SetHalfAxis(HalfAxis::RUp, 1.0f);
  SetHalfAxis(HalfAxis::LLeft, 1.0f);
  SetHalfAxis(HalfAxis::LDown, 0.5f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(Pressed({Button::L1}), 0xFF, 0x00, 0x00, 0xD4));
  EXPECT_EQ(m_controller.GetAnalogInputBytes(), 0xD40000FFu);

  // 43h that doesn't enter config mode is a poll too.
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), AnalogPollReply(Pressed({Button::L1}), 0xFF, 0x00, 0x00, 0xD4));

  // A digital length poll still gets an ACK for its last byte, since more are expected.
  std::vector<bool> acks;
  EXPECT_EQ(TransferBytes(m_controller, POLL, &acks), (std::vector<u8>{0xFF, 0x73, 0x5A, 0xFF, 0xFB}));
  EXPECT_EQ(acks, std::vector<bool>(POLL.size(), true));
}

TEST_F(AnalogControllerTest, ConfigMode)
{
  // Entering is only applied on the last byte.
  std::vector<bool> acks;
  TransferBytes(m_controller, {0x01, 0x43, 0x00, 0x01}, &acks);
  EXPECT_EQ(acks, std::vector<bool>(4, true));
  m_controller.ResetTransferState();
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));

  // The command that enters it replies as a poll.
  Press(Button::Cross);
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(Pressed({Button::Cross})));
  EXPECT_FALSE(AnalogLED());

  // In config mode, 42h has the buttons and sticks, 43h doesn't.
  SetHalfAxis(HalfAxis::RLeft, 1.0f);
  EXPECT_EQ(Transfer(LONG_POLL), (std::vector<u8>{0xFF, 0xF3, 0x5A, 0xFF, 0xBF, 0x00, 0x7F, 0x7F, 0x7F}));
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(LONG_POLL), (std::vector<u8>{0xFF, 0xF3, 0x5A, 0xFF, 0xBF, 0x00, 0x7F, 0x7F, 0x7F}));

  // Any value other than 01h leaves.
  EXPECT_EQ(Transfer({0x01, 0x43, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00}), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Cross})));
  EXPECT_FALSE(AnalogLED());

  // Entering config mode enables DualShock mode, so the legacy rumble bytes don't do anything any more.
  EXPECT_EQ(Transfer(RumblePoll({0x40, 0x01}, 5)), PollReply(Pressed({Button::Cross})));
  EXPECT_EQ(SmallMotor(), 0.0f);
}

TEST_F(AnalogControllerTest, RejectedBytes)
{
  for (const u8 first : {0x00, 0x02, 0x42, 0x81, 0xFF})
  {
    EXPECT_EQ(Transfer({first}), std::vector<u8>{0xFF}) << "first byte 0x" << std::hex << u32(first);
    EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));
  }

  // Config mode commands are only accepted in config mode. The ID byte is still sent, but not ACKed.
  for (const u8 command :
       {0x00, 0x40, 0x41, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x53})
  {
    EXPECT_EQ(Transfer({0x01, command}), (std::vector<u8>{0xFF, 0x41})) << "command 0x" << std::hex << u32(command);
    m_controller.ResetTransferState();
  }

  // Current behaviour: a rejected command leaves the pad waiting for a command, until the transfer state is reset.
  EXPECT_EQ(Transfer({0x01, 0x44}), (std::vector<u8>{0xFF, 0x41}));
  EXPECT_EQ(Transfer({0x42, 0x00, 0x00, 0x00}), (std::vector<u8>{0x41, 0x5A, 0xFF, 0xFF}));

  // Commands that aren't config commands are still rejected in config mode.
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  for (const u8 command : {0x00, 0x3F, 0x50, 0x53, 0x81, 0xFF})
  {
    EXPECT_EQ(Transfer({0x01, command}), (std::vector<u8>{0xFF, 0xF3})) << "command 0x" << std::hex << u32(command);
    m_controller.ResetTransferState();
    EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), ANALOG_CONFIG_REPLY);
  }

  // And in analog mode.
  EXPECT_EQ(Transfer(SetAnalogModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer({0x01, 0x45}), (std::vector<u8>{0xFF, 0x73}));
}

TEST_F(AnalogControllerTest, SetAnalogMode)
{
  const auto get_mode = [this]() {
    const std::vector<u8> rx = Transfer(ConfigCommand(0x45, 0x00));
    return (rx.size() == 9) ? rx[5] : 0xFF;
  };

  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  EXPECT_EQ(get_mode(), 0x00);

  // 00h/01h set the mode, other values are ignored.
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x03)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(get_mode(), 0x01);
  EXPECT_TRUE(AnalogLED());
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x02, 0x03)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(get_mode(), 0x01);
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0xFF, 0x03)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(get_mode(), 0x01);

  // Locked.
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  ToggleAnalog();
  EXPECT_TRUE(AnalogLED());
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));

  // Unlock and switch to digital.
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x00, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(get_mode(), 0x00);
  EXPECT_FALSE(AnalogLED());
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));
  ToggleAnalog();
  EXPECT_TRUE(AnalogLED());
}

TEST_F(AnalogControllerTest, SetAnalogModeLock)
{
  // 00h-02h unlock and 03h locks, higher values act as the low two bits.
  for (const auto [lock, locked] :
       {std::pair<u8, bool>{0x00, false}, std::pair<u8, bool>{0x01, false}, std::pair<u8, bool>{0x02, false},
        std::pair<u8, bool>{0x03, true}, std::pair<u8, bool>{0x04, false}, std::pair<u8, bool>{0x07, true},
        std::pair<u8, bool>{0xFC, false}, std::pair<u8, bool>{0xFF, true}})
  {
    m_controller.Reset();
    EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
    EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, lock)), ANALOG_CONFIG_REPLY);
    EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
    ToggleAnalog();
    EXPECT_EQ(AnalogLED(), locked) << "lock 0x" << std::hex << u32(lock);
  }
}

TEST_F(AnalogControllerTest, SetAnalogModeResetsRumble)
{
  SetupDualShock(true);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0xFF})), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(LargeMotor(), 1.0f);

  // The motors stop as soon as the command byte is received.
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), AnalogPollReply(NO_BUTTONS));
  TransferBytes(m_controller, {0x01, 0x44});
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_EQ(LargeMotor(), 0.0f);
  EXPECT_EQ(Transfer({0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00}),
            (std::vector<u8>{0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

  // And the mapping is gone.
  EXPECT_EQ(Transfer(MapMotorsCommand({0x00, 0x01})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST_F(AnalogControllerTest, GetAnalogMode)
{
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(ConfigCommand(0x45, 0x00)), ConfigReply({0x01, 0x02, 0x00, 0x02, 0x01, 0x00}));
  EXPECT_EQ(Transfer(SetAnalogModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(ConfigCommand(0x45, 0xFF)), ConfigReply({0x01, 0x02, 0x01, 0x02, 0x01, 0x00}));
}

TEST_F(AnalogControllerTest, ConfigTables)
{
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));

  static constexpr struct
  {
    u8 command;
    u8 param;
    std::array<u8, 6> reply;
  } cases[] = {
    {0x46, 0x00, {0x00, 0x00, 0x01, 0x02, 0x00, 0x0A}}, {0x46, 0x01, {0x00, 0x00, 0x01, 0x01, 0x01, 0x14}},
    {0x46, 0x02, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, {0x47, 0x00, {0x00, 0x00, 0x02, 0x00, 0x01, 0x00}},
    {0x47, 0x01, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, {0x4C, 0x00, {0x00, 0x00, 0x00, 0x04, 0x00, 0x00}},
    {0x4C, 0x01, {0x00, 0x00, 0x00, 0x07, 0x00, 0x00}}, {0x4C, 0x02, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x48, 0x00, {0x00, 0x00, 0x00, 0x00, 0x01, 0x00}}, {0x48, 0x01, {0x00, 0x00, 0x00, 0x00, 0x01, 0x00}},
    {0x48, 0x02, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, {0x48, 0xFF, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
  };

  for (const auto& c : cases)
  {
    std::vector<u8> expected = {0xFF, 0xF3, 0x5A};
    expected.insert(expected.end(), c.reply.begin(), c.reply.end());
    EXPECT_EQ(Transfer(ConfigCommand(c.command, c.param)), expected)
      << "command 0x" << std::hex << u32(c.command) << " param 0x" << u32(c.param);
  }

  // The DualShock 2 and unused commands only return zeros, whatever is sent.
  for (const u8 command : {0x40, 0x41, 0x49, 0x4A, 0x4B, 0x4E, 0x4F})
  {
    EXPECT_EQ(Transfer({0x01, command, 0x00, 0x01, 0xFF, 0x02, 0x03, 0x04, 0xFF}), ConfigReply({}))
      << "command 0x" << std::hex << u32(command);
  }

  // The tables are the same in analog mode, and the mode doesn't change.
  EXPECT_EQ(Transfer(SetAnalogModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(ConfigCommand(0x46, 0x01)), ConfigReply({0x00, 0x00, 0x01, 0x01, 0x01, 0x14}));
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));
}

TEST_F(AnalogControllerTest, MapMotors)
{
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));

  // The previous mapping is returned.
  EXPECT_EQ(Transfer(MapMotorsCommand({0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF})),
            ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Transfer(MapMotorsCommand({0x01, 0x00, 0x05, 0x40, 0x00, 0x01})),
            ConfigReply({0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Transfer(MapMotorsCommand({0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF})),
            ConfigReply({0x01, 0x00, 0x05, 0x40, 0x00, 0x01}));

  // The motors are driven by the mapped bytes. Other values don't map anything.
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0x80}, 5)), PollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(136));

  // A motor that's no longer mapped is stopped on the last byte.
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  const std::vector<u8> tx = MapMotorsCommand({0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  TransferBytes(m_controller, std::vector<u8>(tx.begin(), tx.end() - 1));
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(136));
  std::vector<bool> acks;
  EXPECT_EQ(TransferBytes(m_controller, {tx.back()}, &acks), std::vector<u8>{0xFF});
  EXPECT_EQ(acks, std::vector<bool>{false});
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(LargeMotor(), 0.0f);

  // Mapping it to the sixth byte instead keeps it running.
  EXPECT_EQ(Transfer(MapMotorsCommand({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00})),
            ConfigReply({0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(Transfer(MapMotorsCommand({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})),
            ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00}));
  EXPECT_EQ(SmallMotor(), 0.0f);
}

TEST_F(AnalogControllerTest, DualShockRumble)
{
  SetupDualShock(true);

  // Only bit 0 of the small motor's byte matters.
  for (const auto [value, on] : {std::pair<u8, bool>{0x01, true}, std::pair<u8, bool>{0xFE, false},
                                 std::pair<u8, bool>{0x03, true}, std::pair<u8, bool>{0x00, false}})
  {
    EXPECT_EQ(Transfer(RumblePoll({value, 0x00})), AnalogPollReply(NO_BUTTONS));
    EXPECT_EQ(SmallMotor(), on ? 1.0f : 0.0f) << "value 0x" << std::hex << u32(value);
  }

  // The large motor is scaled, including the default bias of 8.
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0x80})), AnalogPollReply(NO_BUTTONS));
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(136));
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0x01})), AnalogPollReply(NO_BUTTONS));
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(9));
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0xFF})), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(LargeMotor(), 1.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0x00})), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(LargeMotor(), 0.0f);

  // An aborted poll keeps the bytes that were received.
  TransferBytes(m_controller, RumblePoll({0x01, 0xFF}, 5));
  m_controller.ResetTransferState();
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(LargeMotor(), 1.0f);

  // Without a bias.
  INISettingsInterface si;
  si.SetIntValue(SECTION, "LargeMotorVibrationBias", 0);
  si.SetIntValue(SECTION, "SmallMotorVibrationBias", 0);
  LoadSettings(si);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0x80})), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(128));

  // Each motor uses its own bias.
  si.SetIntValue(SECTION, "LargeMotorVibrationBias", -1);
  LoadSettings(si);
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(127));
  si.SetIntValue(SECTION, "LargeMotorVibrationBias", 8);
  si.SetIntValue(SECTION, "SmallMotorVibrationBias", -128);
  LoadSettings(si);
  EXPECT_FLOAT_EQ(SmallMotor(), MotorStrength(127));
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(136));
}

TEST_F(AnalogControllerTest, DualShockRumble_LastBytes)
{
  // The motors can be mapped to any of the six bytes.
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(MapMotorsCommand({0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x01})),
            ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0x00, 0x00, 0x00, 0x01, 0xFF})), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(LargeMotor(), 1.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0xFF, 0x00, 0x00, 0x00, 0x00})), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_EQ(LargeMotor(), 0.0f);
}

TEST_F(AnalogControllerTest, DualShockRumble_DigitalMode)
{
  // Only two motor bytes are received in digital mode.
  SetupDualShock(false);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0x80}, 5)), PollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(136));
}

TEST_F(AnalogControllerTest, DualShockRumble_DigitalModeLength)
{
  // Mapping a motor past the second byte lengthens the reply in digital mode, to include it.
  SetupDualShock(false);
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(MapMotorsCommand({0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF})),
            ConfigReply({0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0x00, 0x01}, 7)), (std::vector<u8>{0xFF, 0x42, 0x5A, 0xFF, 0xFF, 0x00, 0x00}));
  EXPECT_EQ(SmallMotor(), 1.0f);

  // Including 43h outside config mode.
  EXPECT_EQ(Transfer({0x01, 0x43, 0x00, 0x01, 0x00, 0x00, 0x00}),
            (std::vector<u8>{0xFF, 0x42, 0x5A, 0xFF, 0xFF, 0x00, 0x00}));
  EXPECT_EQ(Transfer(MapMotorsCommand({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01})),
            ConfigReply({0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0x00, 0x00, 0x00, 0x00, 0xFF})),
            (std::vector<u8>{0xFF, 0x43, 0x5A, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00}));
  EXPECT_EQ(LargeMotor(), 1.0f);

  // Analog mode is unchanged, and unmapping it goes back to the normal length.
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), (std::vector<u8>{0xFF, 0x43, 0x5A, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00}));
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(MapMotorsCommand({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01})),
            ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x00, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(MapMotorsCommand({0x00, 0x01})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));
}

TEST_F(AnalogControllerTest, LegacyRumble)
{
  // Without DualShock mode, the small motor is turned on with 40h-7Fh followed by an odd byte.
  static constexpr struct
  {
    u8 first;
    u8 second;
    bool on;
  } cases[] = {
    {0x40, 0x01, true},  {0x7F, 0x01, true},  {0x41, 0xFF, true},  {0x40, 0x00, false},
    {0x40, 0xFE, false}, {0x00, 0x01, false}, {0x80, 0x01, false}, {0xC0, 0x01, false},
  };

  for (const auto& c : cases)
  {
    EXPECT_EQ(Transfer(RumblePoll({c.first, c.second}, 5)), PollReply(NO_BUTTONS));
    EXPECT_EQ(SmallMotor(), c.on ? 1.0f : 0.0f) << std::hex << u32(c.first) << " " << u32(c.second);
    EXPECT_EQ(LargeMotor(), 0.0f);
  }

  // Same in analog mode, if it wasn't entered through config mode.
  ToggleAnalog();
  EXPECT_EQ(Transfer(RumblePoll({0x40, 0x01})), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x40, 0x00})), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 0.0f);
}

TEST_F(AnalogControllerTest, AnalogButton)
{
  // Toggles when pressed past the deadzone, nothing happens on release.
  Press(Button::Analog, 0.2f);
  EXPECT_FALSE(AnalogLED());
  ToggleAnalog();
  EXPECT_TRUE(AnalogLED());
  EXPECT_EQ(m_controller.GetBindState(static_cast<u32>(Button::Analog)), 0.0f);
  Press(Button::Analog, 0.0f);
  EXPECT_TRUE(AnalogLED());
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));
  ToggleAnalog();
  EXPECT_FALSE(AnalogLED());
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));

  // During a command, it waits until the transfer state is reset.
  TransferBytes(m_controller, {0x01, 0x42});
  ToggleAnalog();
  EXPECT_FALSE(AnalogLED());
  EXPECT_EQ(Transfer({0x00, 0x00, 0x00}), (std::vector<u8>{0x5A, 0xFF, 0xFF}));
  EXPECT_FALSE(AnalogLED());
  m_controller.ResetTransferState();
  EXPECT_TRUE(AnalogLED());
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));

  // Only once.
  m_controller.ResetTransferState();
  EXPECT_TRUE(AnalogLED());
  TransferBytes(m_controller, {0x01});
  ToggleAnalog();
  EXPECT_TRUE(AnalogLED());
  m_controller.ResetTransferState();
  EXPECT_FALSE(AnalogLED());

  // Locked by the game.
  EnterAnalogMode(m_controller);
  ToggleAnalog();
  EXPECT_TRUE(AnalogLED());
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));
}

TEST_F(AnalogControllerTest, AnalogButton_ResetsRumble)
{
  SetCanStartInAnalogMode(true);
  SetupDualShock(true);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0xFF})), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);

  // Both motors stop, and the mapping is gone.
  ToggleAnalog();
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_EQ(LargeMotor(), 0.0f);
  ToggleAnalog();
  Transfer(RumblePoll({0x01, 0xFF}));
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_EQ(LargeMotor(), 0.0f);
}

TEST_F(AnalogControllerTest, AnalogButton_StatusByte)
{
  // Without DualShock mode, the status byte stays at 5Ah.
  SetCanStartInAnalogMode(true);
  ToggleAnalog();
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));
  ToggleAnalog();
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));

  // In DualShock mode, it changes to 00h so the game notices.
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  ToggleAnalog();
  std::vector<u8> expected = AnalogPollReply(NO_BUTTONS);
  expected[2] = 0x00;
  EXPECT_EQ(Transfer(LONG_POLL), expected);
  ToggleAnalog();
  expected = PollReply(NO_BUTTONS);
  expected[2] = 0x00;
  EXPECT_EQ(Transfer(POLL), expected);

  // Until config mode is entered again.
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), expected);
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), ANALOG_CONFIG_REPLY);
}

TEST_F(AnalogControllerTest, AnalogButton_LeavesDualShockMode)
{
  // If the game can't start in analog mode, switching to digital mode leaves config and DualShock mode.
  SetCanStartInAnalogMode(false);
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);

  // Switching to analog mode still changes the status byte.
  ToggleAnalog();
  std::vector<u8> expected = AnalogPollReply(NO_BUTTONS);
  expected[2] = 0x00;
  EXPECT_EQ(Transfer(LONG_POLL), expected);

  ToggleAnalog();
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(RumblePoll({0x40, 0x01}, 5)), PollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);

  // Including from config mode.
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x02)), ANALOG_CONFIG_REPLY);
  ToggleAnalog();
  EXPECT_FALSE(AnalogLED());
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));
}

TEST_F(AnalogControllerTest, Reset)
{
  SetCanStartInAnalogMode(true);
  SetupDualShock(true);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0xFF})), AnalogPollReply(NO_BUTTONS));
  ToggleAnalog();
  ToggleAnalog();
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG)[2], 0x00);
  EXPECT_EQ(Transfer(MapMotorsCommand({0x00, 0x01})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  TransferBytes(m_controller, RumblePoll({0x01, 0xFF}, 5));
  EXPECT_EQ(SmallMotor(), 1.0f);

  // Config and DualShock mode are cleared, the motors stopped and the mapping removed. ForceAnalogOnReset is on by
  // default, so it starts in analog mode.
  Press(Button::Cross);
  m_controller.Reset();
  EXPECT_TRUE(AnalogLED());
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_EQ(LargeMotor(), 0.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x40, 0x01})), AnalogPollReply(Pressed({Button::Cross})));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), AnalogPollReply(Pressed({Button::Cross})));
  EXPECT_EQ(Transfer(MapMotorsCommand({0x00, 0x01})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));

  // Not if the game can't start in analog mode.
  SetCanStartInAnalogMode(false);
  m_controller.Reset();
  EXPECT_FALSE(AnalogLED());
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Cross})));

  // Or with the setting off.
  SetCanStartInAnalogMode(true);
  INISettingsInterface si;
  si.SetBoolValue(SECTION, "ForceAnalogOnReset", false);
  LoadSettings(si);
  m_controller.Reset();
  EXPECT_FALSE(AnalogLED());
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Cross})));

  // Partway through a command.
  TransferBytes(m_controller, {0x01, 0x42, 0x00});
  m_controller.Reset();
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Cross})));
}

TEST_F(AnalogControllerTest, Reset_ClearsLock)
{
  EnterAnalogMode(m_controller);
  m_controller.Reset();
  EXPECT_FALSE(AnalogLED());
  ToggleAnalog();
  EXPECT_TRUE(AnalogLED());
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));
}

TEST_F(AnalogControllerTest, ButtonDeadzone)
{
  Press(Button::Cross, 0.24f);
  EXPECT_EQ(m_controller.GetBindState(static_cast<u32>(Button::Cross)), 0.0f);
  EXPECT_EQ(m_controller.GetButtonStateBits(), 0u);
  Press(Button::Cross, 0.25f);
  EXPECT_EQ(m_controller.GetBindState(static_cast<u32>(Button::Cross)), 1.0f);
  EXPECT_EQ(m_controller.GetButtonStateBits(), ButtonBit(Button::Cross));
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Cross})));

  INISettingsInterface si;
  si.SetFloatValue(SECTION, "ButtonDeadzone", 0.5f);
  LoadSettings(si);
  Press(Button::Cross, 0.49f);
  EXPECT_EQ(m_controller.GetButtonStateBits(), 0u);
  Press(Button::Analog, 0.49f);
  EXPECT_FALSE(AnalogLED());
  Press(Button::Cross, 0.5f);
  Press(Button::Analog, 0.5f);
  EXPECT_EQ(m_controller.GetButtonStateBits(), ButtonBit(Button::Cross));
  EXPECT_TRUE(AnalogLED());

  // Every button.
  for (u32 i = 0; i < static_cast<u32>(Button::Analog); i++)
  {
    const Button button = static_cast<Button>(i);
    Press(Button::Cross, 0.0f);
    Press(button);
    EXPECT_EQ(m_controller.GetButtonStateBits(), ButtonBit(button)) << i;
    EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(Pressed({button}))) << i;
    Press(button, 0.0f);
  }
}

TEST_F(AnalogControllerTest, Sticks)
{
  EnterAnalogMode(m_controller);

  // With the default sensitivity of 1.33.
  SetHalfAxis(HalfAxis::LRight, 0.5f);
  EXPECT_FLOAT_EQ(m_controller.GetBindState(HalfAxisIndex(HalfAxis::LRight)), 169.0f / 255.0f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0x7F, 0x7F, 0xD4, 0x7F));
  SetHalfAxis(HalfAxis::LRight, 1.0f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0x7F, 0x7F, 0xFF, 0x7F));

  // Opposing directions cancel out.
  SetHalfAxis(HalfAxis::LLeft, 1.0f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0x7F, 0x7F, 0x7F, 0x7F));
  SetHalfAxis(HalfAxis::LRight, 0.0f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0x7F, 0x7F, 0x00, 0x7F));
  SetHalfAxis(HalfAxis::LLeft, 0.0f);

  // Up is negative.
  SetHalfAxis(HalfAxis::LUp, 1.0f);
  SetHalfAxis(HalfAxis::RDown, 1.0f);
  SetHalfAxis(HalfAxis::RLeft, 1.0f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0x00, 0xFF, 0x7F, 0x00));
  EXPECT_EQ(m_controller.GetAnalogInputBytes(), 0x007FFF00u);

  // Indices past the half axes are ignored.
  m_controller.SetBindState(HalfAxisIndex(HalfAxis::Count), 1.0f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0x00, 0xFF, 0x7F, 0x00));
  EXPECT_EQ(SmallMotor(), 0.0f);

  // Sensitivity of 1.
  INISettingsInterface si;
  si.SetFloatValue(SECTION, "AnalogSensitivity", 1.0f);
  LoadSettings(si);
  SetHalfAxis(HalfAxis::LUp, 0.0f);
  SetHalfAxis(HalfAxis::RDown, 0.0f);
  SetHalfAxis(HalfAxis::RLeft, 0.0f);
  SetHalfAxis(HalfAxis::LRight, 0.5f);
  SetHalfAxis(HalfAxis::RUp, 0.5f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0x7F, 0x40, 0xBF, 0x7F));
}

TEST_F(AnalogControllerTest, InvertSticks)
{
  EnterAnalogMode(m_controller);

  static constexpr struct
  {
    u32 left;
    u32 right;
    std::array<u8, 4> axes; // RX, RY, LX, LY
  } cases[] = {
    {0, 0, {0xFF, 0xFF, 0xFF, 0xFF}}, {1, 0, {0xFF, 0xFF, 0x00, 0xFF}}, {2, 0, {0xFF, 0xFF, 0xFF, 0x00}},
    {3, 0, {0xFF, 0xFF, 0x00, 0x00}}, {0, 1, {0x00, 0xFF, 0xFF, 0xFF}}, {0, 2, {0xFF, 0x00, 0xFF, 0xFF}},
    {0, 3, {0x00, 0x00, 0xFF, 0xFF}},
  };

  for (const auto& c : cases)
  {
    INISettingsInterface si;
    si.SetUIntValue(SECTION, "InvertLeftStick", c.left);
    si.SetUIntValue(SECTION, "InvertRightStick", c.right);
    LoadSettings(si);

    for (const HalfAxis axis : {HalfAxis::LRight, HalfAxis::LDown, HalfAxis::RRight, HalfAxis::RDown})
    {
      SetHalfAxis(axis, 0.0f);
      SetHalfAxis(axis, 1.0f);
    }

    EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, c.axes[0], c.axes[1], c.axes[2], c.axes[3]))
      << "left " << c.left << " right " << c.right;
  }
}

TEST_F(AnalogControllerTest, StickDeadzone)
{
  EnterAnalogMode(m_controller);

  INISettingsInterface si;
  si.SetFloatValue(SECTION, "AnalogDeadzone", 0.5f);
  LoadSettings(si);

  // Inside the circular deadzone, both axes of the stick are centred.
  SetHalfAxis(HalfAxis::LDown, 0.2f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));
  SetHalfAxis(HalfAxis::LRight, 0.2f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));

  // Leaving it through one axis restores the other one.
  SetHalfAxis(HalfAxis::LRight, 0.5f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0x7F, 0x7F, 0xD4, 0xA1));
  SetHalfAxis(HalfAxis::LDown, 0.21f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0x7F, 0x7F, 0xD4, 0xA3));

  // The other stick is separate.
  SetHalfAxis(HalfAxis::RRight, 0.2f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0x7F, 0x7F, 0xD4, 0xA3));
  SetHalfAxis(HalfAxis::RRight, 0.5f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0xD4, 0x7F, 0xD4, 0xA3));

  // Back inside.
  SetHalfAxis(HalfAxis::LRight, 0.0f);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0xD4, 0x7F, 0x7F, 0x7F));
}

TEST_F(AnalogControllerTest, AnalogDPadInDigitalMode)
{
  INISettingsInterface si;
  si.SetFloatValue(SECTION, "AnalogSensitivity", 1.0f);
  LoadSettings(si);

  // On by default, at a quarter of the way past the centre.
  static constexpr struct
  {
    HalfAxis axis;
    u8 raw;
    u16 buttons;
  } cases[] = {
    {HalfAxis::LLeft, 126, static_cast<u16>(~(1u << 7))},
    {HalfAxis::LLeft, 124, NO_BUTTONS},
    {HalfAxis::LRight, 127, static_cast<u16>(~(1u << 5))},
    {HalfAxis::LRight, 125, NO_BUTTONS},
    {HalfAxis::LUp, 126, static_cast<u16>(~(1u << 4))},
    {HalfAxis::LUp, 124, NO_BUTTONS},
    {HalfAxis::LDown, 127, static_cast<u16>(~(1u << 6))},
    {HalfAxis::LDown, 125, NO_BUTTONS},
    {HalfAxis::RLeft, 255, NO_BUTTONS},
    {HalfAxis::RDown, 255, NO_BUTTONS},
  };

  for (const auto& c : cases)
  {
    SetHalfAxis(c.axis, RawHalfAxis(c.raw));
    EXPECT_EQ(Transfer(POLL), PollReply(c.buttons)) << static_cast<u32>(c.axis) << " " << u32(c.raw);
    EXPECT_EQ(m_controller.GetButtonStateBits(), 0u);
    SetHalfAxis(c.axis, 0.0f);
  }

  // Not in config or analog mode.
  SetHalfAxis(HalfAxis::LLeft, 1.0f);
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Left})));
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(Pressed({Button::Left})));
  EXPECT_EQ(Transfer(LONG_POLL), (std::vector<u8>{0xFF, 0xF3, 0x5A, 0xFF, 0xFF, 0x7F, 0x7F, 0x00, 0x7F}));
  EXPECT_EQ(Transfer(SetAnalogModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS, 0x7F, 0x7F, 0x00, 0x7F));

  // Or with the setting off.
  AnalogController controller(0);
  si.SetBoolValue(SECTION, "AnalogDPadInDigitalMode", false);
  controller.LoadSettings(si, SECTION, true);
  controller.SetBindState(HalfAxisIndex(HalfAxis::LLeft), 1.0f);
  EXPECT_EQ(TransferCommand(controller, POLL), PollReply(NO_BUTTONS));
}

TEST_F(AnalogControllerTest, AnalogShoulderAndTriggerButtons)
{
  static constexpr u16 L1 = static_cast<u16>(1u << 10), R1 = static_cast<u16>(1u << 11);
  static constexpr u16 L2 = static_cast<u16>(1u << 8), R2 = static_cast<u16>(1u << 9);

  // Pressed buttons for the right stick left and right, in digital and analog mode.
  static constexpr struct
  {
    u32 shoulder;
    u32 trigger;
    u16 digital_left, digital_right, analog_left, analog_right;
  } cases[] = {
    {0, 0, 0, 0, 0, 0},
    {1, 0, L1, R1, 0, 0},
    {2, 0, L1, R1, L1, R1},
    {0, 1, L2, R2, 0, 0},
    {0, 2, L2, R2, L2, R2},
    // Current behaviour: both use the right stick's X axis.
    {2, 2, L1 | L2, R1 | R2, L1 | L2, R1 | R2},
    {1, 2, L1 | L2, R1 | R2, L2, R2},
  };

  for (const auto& c : cases)
  {
    INISettingsInterface si;
    si.SetUIntValue(SECTION, "AnalogShoulderButtons", c.shoulder);
    si.SetUIntValue(SECTION, "AnalogTriggerButtons", c.trigger);

    AnalogController controller(0);
    controller.LoadSettings(si, SECTION, true);

    for (u32 analog = 0; analog < 2; analog++)
    {
      const std::vector<u8>& tx = analog ? LONG_POLL : POLL;
      const auto reply = [analog](u16 pressed) {
        return analog ? AnalogPollReply(static_cast<u16>(~pressed), 0x00, 0x7F, 0x7F, 0x7F) :
                        PollReply(static_cast<u16>(~pressed));
      };

      controller.SetBindState(HalfAxisIndex(HalfAxis::RLeft), 1.0f);
      EXPECT_EQ(TransferCommand(controller, tx), reply(analog ? c.analog_left : c.digital_left))
        << c.shoulder << " " << c.trigger << " " << analog;
      controller.SetBindState(HalfAxisIndex(HalfAxis::RLeft), 0.0f);
      controller.SetBindState(HalfAxisIndex(HalfAxis::RRight), 1.0f);
      std::vector<u8> expected = reply(analog ? c.analog_right : c.digital_right);
      if (analog)
        expected[5] = 0xFF;
      EXPECT_EQ(TransferCommand(controller, tx), expected) << c.shoulder << " " << c.trigger << " " << analog;
      controller.SetBindState(HalfAxisIndex(HalfAxis::RRight), 0.0f);

      // The Y axis doesn't do anything.
      controller.SetBindState(HalfAxisIndex(HalfAxis::RUp), 1.0f);
      expected = reply(0);
      if (analog)
      {
        expected[5] = 0x7F;
        expected[6] = 0x00;
      }
      EXPECT_EQ(TransferCommand(controller, tx), expected);
      controller.SetBindState(HalfAxisIndex(HalfAxis::RUp), 0.0f);

      controller.SetBindState(static_cast<u32>(Button::Analog), 1.0f);
    }
  }
}

TEST_F(AnalogControllerTest, DisableSOCD)
{
  Press(Button::Up);
  Press(Button::Down);
  Press(Button::Left);
  Press(Button::Right);

  // Off by default.
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Up, Button::Down, Button::Left, Button::Right})));

  // Up is kept over Down, and Right over Left. It's applied after the stick D-pad.
  INISettingsInterface si;
  si.SetBoolValue(SECTION, "DisableSOCD", true);
  LoadSettings(si);
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Up, Button::Right})));
  EXPECT_EQ(m_controller.GetButtonStateBits(), 0xF0u);

  Press(Button::Up, 0.0f);
  Press(Button::Right, 0.0f);
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Down, Button::Left})));
  SetHalfAxis(HalfAxis::LRight, 1.0f);
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Down, Button::Right})));

  ToggleAnalog();
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(Pressed({Button::Down, Button::Left}), 0x7F, 0x7F, 0xFF, 0x7F));
}

TEST_F(AnalogControllerTest, MemberDefaults)
{
  // Without LoadSettings(), the defaults are the same as the settings'.
  AnalogController controller(0);
  controller.SetBindState(static_cast<u32>(Button::Cross), 0.0f);
  controller.SetBindState(static_cast<u32>(Button::Circle), 0.24f);
  controller.SetBindState(static_cast<u32>(Button::Square), 0.25f);
  EXPECT_EQ(controller.GetButtonStateBits(), ButtonBit(Button::Square));
  controller.SetBindState(static_cast<u32>(Button::Analog), 0.24f);
  EXPECT_EQ(controller.GetBindState(GetBindIndex(AnalogController::INFO, "AnalogLED")), 0.0f);

  // Automatic analog mode and the stick D-pad are on.
  controller.SetBindState(HalfAxisIndex(HalfAxis::LLeft), 1.0f);
  EXPECT_EQ(TransferCommand(controller, POLL), PollReply(Pressed({Button::Square, Button::Left})));
  SetCanStartInAnalogMode(true);
  controller.Reset();
  EXPECT_EQ(controller.GetBindState(GetBindIndex(AnalogController::INFO, "AnalogLED")), 1.0f);
  EXPECT_EQ(TransferCommand(controller, LONG_POLL), AnalogPollReply(Pressed({Button::Square}), 0x7F, 0x7F, 0x00, 0x7F));
}

TEST_F(AnalogControllerTest, SaveState_MapMotors)
{
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x02)), ANALOG_CONFIG_REPLY);

  // Partway through 4Dh, the rest of the command gives the same result.
  TransferBytes(m_controller, {0x01, 0x4D, 0x00, 0x00, 0x01});
  const std::vector<u8> state = SaveState(m_controller);

  AnalogController loaded(0);
  LoadDefaultSettings(loaded);
  LoadState(loaded, state, true);
  EXPECT_TRUE(loaded.GetBindState(GetBindIndex(AnalogController::INFO, "AnalogLED")) != 0.0f);

  for (AnalogController* controller : {&m_controller, &loaded})
  {
    EXPECT_EQ(TransferCommand(*controller, {0xFF, 0xFF, 0xFF, 0xFF}), (std::vector<u8>{0xFF, 0xFF, 0xFF, 0xFF}));
    EXPECT_EQ(TransferCommand(*controller, ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
    EXPECT_EQ(TransferCommand(*controller, RumblePoll({0x01, 0x80})), AnalogPollReply(NO_BUTTONS));
    EXPECT_EQ(controller->GetBindState(GetBindIndex(AnalogController::INFO, "SmallMotor")), 1.0f);
    EXPECT_FLOAT_EQ(controller->GetBindState(GetBindIndex(AnalogController::INFO, "LargeMotor")), MotorStrength(136));
  }

  // The motors are restored too.
  AnalogController loaded2(0);
  LoadDefaultSettings(loaded2);
  LoadState(loaded2, SaveState(m_controller), true);
  EXPECT_EQ(loaded2.GetBindState(GetBindIndex(AnalogController::INFO, "SmallMotor")), 1.0f);
  EXPECT_FLOAT_EQ(loaded2.GetBindState(GetBindIndex(AnalogController::INFO, "LargeMotor")), MotorStrength(136));
  EXPECT_EQ(TransferCommand(loaded2, RumblePoll({0x00, 0x00})), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(loaded2.GetBindState(GetBindIndex(AnalogController::INFO, "SmallMotor")), 0.0f);
}

TEST_F(AnalogControllerTest, SaveState_SetAnalogMode)
{
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));

  // Partway through 44h, after the mode but before the lock.
  TransferBytes(m_controller, {0x01, 0x44, 0x00, 0x01});
  EXPECT_TRUE(AnalogLED());
  const std::vector<u8> state = SaveState(m_controller);

  AnalogController loaded(0);
  LoadDefaultSettings(loaded);
  LoadState(loaded, state, true);
  EXPECT_EQ(loaded.GetBindState(GetBindIndex(AnalogController::INFO, "AnalogLED")), 1.0f);
  EXPECT_EQ(TransferCommand(loaded, {0x03, 0x00, 0x00, 0x00, 0x00}), (std::vector<u8>{0x00, 0x00, 0x00, 0x00, 0x00}));
  EXPECT_EQ(TransferCommand(loaded, ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);

  // Locked.
  loaded.SetBindState(static_cast<u32>(Button::Analog), 1.0f);
  EXPECT_EQ(loaded.GetBindState(GetBindIndex(AnalogController::INFO, "AnalogLED")), 1.0f);
  EXPECT_EQ(TransferCommand(loaded, LONG_POLL), AnalogPollReply(NO_BUTTONS));
}

TEST_F(AnalogControllerTest, SaveState_InputState)
{
  EnterAnalogMode(m_controller);
  Press(Button::Cross);
  SetHalfAxis(HalfAxis::LRight, 1.0f);
  const std::vector<u8> state = SaveState(m_controller);

  // Input state is only loaded if asked to.
  AnalogController loaded(0);
  LoadDefaultSettings(loaded);
  loaded.SetBindState(static_cast<u32>(Button::Circle), 1.0f);
  loaded.SetBindState(HalfAxisIndex(HalfAxis::RDown), 1.0f);
  LoadState(loaded, state, false);
  EXPECT_EQ(TransferCommand(loaded, LONG_POLL), AnalogPollReply(Pressed({Button::Circle}), 0x7F, 0xFF, 0x7F, 0x7F));
  LoadState(loaded, state, true);
  EXPECT_EQ(TransferCommand(loaded, LONG_POLL), AnalogPollReply(Pressed({Button::Cross}), 0x7F, 0x7F, 0xFF, 0x7F));
}

TEST_F(AnalogControllerTest, LegacyState)
{
  // Saved by version 104 partway through a poll, in locked analog mode with both motors mapped, Cross pressed, the
  // left stick left and the large motor on. Inputs aren't loaded from older states.
  static const std::vector<u8> state = {0x02, 0x03, 0x08, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x5A,
                                        0xFF, 0xBF, 0x7F, 0x7F, 0x00, 0x7F, 0x01, 0x01, 0x01, 0x00, 0x00, 0x01, 0xFF,
                                        0xFF, 0xFF, 0xFF, 0x5A, 0x00, 0x7F, 0x7F, 0x7F, 0xFF, 0xBF, 0x00, 0x80};
  LoadState(m_controller, state, true, 104);
  EXPECT_TRUE(AnalogLED());
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(0x80 + 8));

  // The transfer starts again from idle.
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0x00})), AnalogPollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(LargeMotor(), 0.0f);

  // The lock is kept.
  ToggleAnalog();
  EXPECT_TRUE(AnalogLED());
}

TEST_F(AnalogControllerTest, LegacyState_Version75)
{
  // Before version 76, without the transfer buffers: analog mode, DualShock, config mode, the status byte, Square
  // pressed (not loaded), then the mapping and motors.
  static const std::vector<u8> state = {0x01, 0x01, 0x00, 0x00, 0x00, 0x5A, 0xFF, 0x7F, 0x02, 0x00, 0x01, 0xFF, 0xFF,
                                        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x40};
  LoadState(m_controller, state, true, 75);
  EXPECT_TRUE(AnalogLED());
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(0x40 + 8));
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));
}

TEST_F(AnalogControllerTest, LegacyState_Version44)
{
  // Before version 45, without the status byte, the mapping, the motor indices or the queued toggle.
  static const std::vector<u8> state = {0x01, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x40};
  LoadState(m_controller, state, true, 44);
  EXPECT_TRUE(AnalogLED());
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(0x40 + 8));
  EXPECT_EQ(Transfer(LONG_POLL), AnalogPollReply(NO_BUTTONS));
}
