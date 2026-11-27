// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller_test_utils.h"

#include "core/analog_joystick.h"

#include "util/ini_settings_interface.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <utility>
#include <vector>

// These tests check what is currently emulated. Comments starting with "Current behaviour:" mark things that look
// questionable, and may change.

using namespace ControllerTestUtils;

namespace {

using Button = AnalogJoystick::Button;
using HalfAxis = AnalogJoystick::HalfAxis;

static constexpr const char* SECTION = "Pad1";
static constexpr u32 LED_INDEX = static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::Count);

static constexpr u32 HalfAxisIndex(HalfAxis axis)
{
  return static_cast<u32>(Button::Count) + static_cast<u32>(axis);
}

static std::vector<u8> JoystickReply(u16 buttons, u8 rx = Controller::AXIS_CENTER, u8 ry = Controller::AXIS_CENTER,
                                     u8 lx = Controller::AXIS_CENTER, u8 ly = Controller::AXIS_CENTER)
{
  return DeviceReply(0x53, buttons, {rx, ry, lx, ly});
}

class AnalogJoystickTest : public ::testing::Test
{
protected:
  void SetUp() override { LoadDefaultSettings(m_joystick); }

  void LoadSettings(const INISettingsInterface& si) { m_joystick.LoadSettings(si, SECTION, false); }
  void Press(Button button, float value = 1.0f) { m_joystick.SetBindState(static_cast<u32>(button), value); }
  void SetHalfAxis(HalfAxis axis, float value) { m_joystick.SetBindState(HalfAxisIndex(axis), value); }
  bool ModeLED() const { return m_joystick.GetBindState(LED_INDEX) != 0.0f; }
  std::vector<u8> Transfer(const std::vector<u8>& tx) { return TransferCommand(m_joystick, tx); }

  static std::unique_ptr<AnalogJoystick> MakeJoystick()
  {
    auto joystick = std::make_unique<AnalogJoystick>(0);
    LoadDefaultSettings(*joystick);
    return joystick;
  }

  AnalogJoystick m_joystick{0};
};

} // namespace

TEST_F(AnalogJoystickTest, BindingIndices)
{
  EXPECT_EQ(m_joystick.GetType(), ControllerType::AnalogJoystick);

  static constexpr std::pair<const char*, u32> bindings[] = {
    {"Select", 0}, {"L3", 1},      {"R3", 2},     {"Start", 3},  {"Up", 4},       {"Right", 5},     {"Down", 6},
    {"Left", 7},   {"L2", 8},      {"R2", 9},     {"L1", 10},    {"R1", 11},      {"Triangle", 12}, {"Circle", 13},
    {"Cross", 14}, {"Square", 15}, {"Mode", 16},  {"LLeft", 17}, {"LRight", 18},  {"LDown", 19},    {"LUp", 20},
    {"RLeft", 21}, {"RRight", 22}, {"RDown", 23}, {"RUp", 24},   {"ModeLED", 25},
  };
  for (const auto& [name, index] : bindings)
    EXPECT_EQ(GetBindIndex(AnalogJoystick::INFO, name), index) << name;
  EXPECT_EQ(AnalogJoystick::INFO.bindings.size(), std::size(bindings));

  for (const char* setting :
       {"DisableSOCD", "AnalogDeadzone", "AnalogSensitivity", "InvertLeftStick", "InvertRightStick"})
  {
    EXPECT_TRUE(HasSetting(AnalogJoystick::INFO, setting)) << setting;
  }
}

TEST_F(AnalogJoystickTest, AnalogModePoll)
{
  // Starts in analog mode, with the axes centred.
  EXPECT_TRUE(ModeLED());
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS));
  EXPECT_EQ(m_joystick.GetAnalogInputBytes(), 0x7F7F7F7Fu);

  // Back-to-back polls don't need a reset, and the bytes after 42h are ignored.
  Press(Button::Cross);
  EXPECT_EQ(Transfer({0x01, 0x42, 0xFF, 0x5A, 0x01, 0x02, 0x03, 0x04, 0x05}), JoystickReply(Pressed({Button::Cross})));
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(Pressed({Button::Cross})));
}

TEST_F(AnalogJoystickTest, DigitalModePoll)
{
  Press(Button::Mode);
  EXPECT_FALSE(ModeLED());
  Press(Button::Square);
  Press(Button::L3);
  Press(Button::R3);
  SetHalfAxis(HalfAxis::LRight, 1.0f);

  // Digital mode stops after the buttons, and doesn't send L3/R3.
  EXPECT_EQ(Transfer(POLL), PollReply(Pressed({Button::Square})));
  EXPECT_EQ(m_joystick.GetBindState(static_cast<u32>(Button::L3)), 1.0f);

  // Analog mode comes back on reset.
  m_joystick.Reset();
  EXPECT_TRUE(ModeLED());
  EXPECT_EQ(Transfer(LONG_POLL),
            JoystickReply(Pressed({Button::Square, Button::L3, Button::R3}), 0x7F, 0x7F, 0xFF, 0x7F));
}

TEST_F(AnalogJoystickTest, RejectedBytes)
{
  // The ID byte is still sent for other commands, but not ACKed.
  ExpectRejectedBytes(m_joystick, {0x00, 0x43, 0x44, 0x45, 0x4D}, 0x53, LONG_POLL, JoystickReply(NO_BUTTONS));
  Press(Button::Mode);
  ExpectRejectedBytes(m_joystick, {0x00, 0x43, 0x44, 0x45, 0x4D}, 0x41, POLL, PollReply(NO_BUTTONS));
}

TEST_F(AnalogJoystickTest, ResetTransferState)
{
  Press(Button::Triangle);
  for (u32 i = 0; i < 2; i++)
  {
    for (size_t len = 1; len < LONG_POLL.size(); len++)
    {
      TransferBytes(m_joystick, std::vector<u8>(LONG_POLL.begin(), LONG_POLL.begin() + len));
      if (i == 0)
        m_joystick.ResetTransferState();
      else
        m_joystick.Reset();

      EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(Pressed({Button::Triangle})))
        << (i ? "Reset" : "ResetTransferState") << " after " << len;
    }
  }
}

TEST_F(AnalogJoystickTest, ModeToggle)
{
  Press(Button::Mode, 0.49f);
  EXPECT_TRUE(ModeLED());
  Press(Button::Mode, 0.5f);
  EXPECT_FALSE(ModeLED());
  EXPECT_EQ(m_joystick.GetBindState(static_cast<u32>(Button::Mode)), 0.0f);

  // Releasing doesn't toggle. Current behaviour: every press-level update toggles, even without a release in between.
  Press(Button::Mode, 0.0f);
  EXPECT_FALSE(ModeLED());
  Press(Button::Mode, 1.0f);
  EXPECT_TRUE(ModeLED());
  Press(Button::Mode, 1.0f);
  EXPECT_FALSE(ModeLED());

  // Partway through a poll, the mode changes straight away, but the rest of the poll was built when it started.
  Press(Button::Mode, 1.0f);
  EXPECT_EQ(TransferBytes(m_joystick, {0x01, 0x42, 0x00}), (std::vector<u8>{0xFF, 0x53, 0x5A}));
  Press(Button::Mode, 1.0f);
  EXPECT_FALSE(ModeLED());
  EXPECT_EQ(Transfer({0x00, 0x00, 0x00, 0x00, 0x00, 0x00}), (std::vector<u8>{0xFF, 0xFF, 0x7F, 0x7F, 0x7F, 0x7F}));
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));
}

TEST_F(AnalogJoystickTest, Buttons)
{
  for (u32 i = 0; i < static_cast<u32>(Button::Mode); i++)
  {
    const Button button = static_cast<Button>(i);
    Press(button, 0.49f);
    EXPECT_EQ(m_joystick.GetBindState(i), 0.0f) << i;
    EXPECT_EQ(m_joystick.GetButtonStateBits(), 0u);

    Press(button, 0.5f);
    EXPECT_EQ(m_joystick.GetBindState(i), 1.0f) << i;
    EXPECT_EQ(m_joystick.GetButtonStateBits(), ButtonBit(button));
    EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(Pressed({button}))) << i;

    Press(button, 0.0f);
    EXPECT_EQ(m_joystick.GetButtonStateBits(), 0u);
  }
}

TEST_F(AnalogJoystickTest, Sticks)
{
  // With the default sensitivity of 1.33.
  SetHalfAxis(HalfAxis::LRight, 0.5f);
  EXPECT_FLOAT_EQ(m_joystick.GetBindState(HalfAxisIndex(HalfAxis::LRight)), 169.0f / 255.0f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, 0x7F, 0x7F, 0xD4, 0x7F));
  SetHalfAxis(HalfAxis::LRight, 1.0f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, 0x7F, 0x7F, 0xFF, 0x7F));

  // Opposing directions cancel out.
  SetHalfAxis(HalfAxis::LLeft, 1.0f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, 0x7F, 0x7F, 0x7F, 0x7F));
  SetHalfAxis(HalfAxis::LRight, 0.0f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, 0x7F, 0x7F, 0x00, 0x7F));
  SetHalfAxis(HalfAxis::LLeft, 0.0f);

  // Up is negative.
  SetHalfAxis(HalfAxis::LUp, 1.0f);
  SetHalfAxis(HalfAxis::RDown, 1.0f);
  SetHalfAxis(HalfAxis::RLeft, 1.0f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, 0x00, 0xFF, 0x7F, 0x00));
  EXPECT_EQ(m_joystick.GetAnalogInputBytes(), 0x007FFF00u);

  // Indices past the half axes are ignored.
  m_joystick.SetBindState(LED_INDEX, 0.0f);
  EXPECT_TRUE(ModeLED());
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, 0x00, 0xFF, 0x7F, 0x00));

  // Sensitivity of 1.
  INISettingsInterface si;
  si.SetFloatValue(SECTION, "AnalogSensitivity", 1.0f);
  LoadSettings(si);
  SetHalfAxis(HalfAxis::LUp, 0.0f);
  SetHalfAxis(HalfAxis::RDown, 0.0f);
  SetHalfAxis(HalfAxis::RLeft, 0.0f);
  SetHalfAxis(HalfAxis::LRight, 0.5f);
  SetHalfAxis(HalfAxis::RUp, 0.5f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, 0x7F, 0x40, 0xBF, 0x7F));
}

TEST_F(AnalogJoystickTest, InvertSticks)
{
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

    EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, c.axes[0], c.axes[1], c.axes[2], c.axes[3]))
      << "left " << c.left << " right " << c.right;
  }
}

TEST_F(AnalogJoystickTest, StickDeadzone)
{
  INISettingsInterface si;
  si.SetFloatValue(SECTION, "AnalogDeadzone", 0.5f);
  LoadSettings(si);

  // Inside the circular deadzone, both axes of the stick are centred.
  SetHalfAxis(HalfAxis::LDown, 0.2f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS));
  SetHalfAxis(HalfAxis::LRight, 0.2f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS));

  // Leaving it through one axis restores the other one.
  SetHalfAxis(HalfAxis::LRight, 0.5f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, 0x7F, 0x7F, 0xD4, 0xA1));
  SetHalfAxis(HalfAxis::LDown, 0.21f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, 0x7F, 0x7F, 0xD4, 0xA3));

  // The other stick is separate.
  SetHalfAxis(HalfAxis::RRight, 0.2f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, 0x7F, 0x7F, 0xD4, 0xA3));
  SetHalfAxis(HalfAxis::RRight, 0.5f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, 0xD4, 0x7F, 0xD4, 0xA3));

  // Back inside.
  SetHalfAxis(HalfAxis::LRight, 0.0f);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(NO_BUTTONS, 0xD4, 0x7F, 0x7F, 0x7F));
}

TEST_F(AnalogJoystickTest, DisableSOCD)
{
  for (const Button button : {Button::Up, Button::Down, Button::Left, Button::Right, Button::L1})
    Press(button);

  // Off by default.
  EXPECT_EQ(Transfer(LONG_POLL),
            JoystickReply(Pressed({Button::Up, Button::Down, Button::Left, Button::Right, Button::L1})));

  // Up is kept over Down, and Right over Left.
  INISettingsInterface si;
  si.SetBoolValue(SECTION, "DisableSOCD", true);
  LoadSettings(si);
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(Pressed({Button::Up, Button::Right, Button::L1})));
}

TEST_F(AnalogJoystickTest, SaveState)
{
  Press(Button::Circle);
  SetHalfAxis(HalfAxis::RUp, 1.0f);
  SetHalfAxis(HalfAxis::LLeft, 1.0f);

  // Partway through a poll, the rest of it continues after loading.
  ExpectResumesAfterLoad(m_joystick, MakeJoystick, LONG_POLL,
                         JoystickReply(Pressed({Button::Circle}), 0x7F, 0x00, 0x00, 0x7F));

  // The mode is always loaded, but the buttons and axes only if asked to.
  Press(Button::Mode);
  const std::vector<u8> state = SaveState(m_joystick);
  {
    const auto loaded = MakeJoystick();
    loaded->SetBindState(static_cast<u32>(Button::Square), 1.0f);
    loaded->SetBindState(HalfAxisIndex(HalfAxis::LRight), 1.0f);
    LoadState(*loaded, state, false);
    EXPECT_EQ(loaded->GetBindState(LED_INDEX), 0.0f);
    EXPECT_EQ(TransferCommand(*loaded, POLL), PollReply(Pressed({Button::Square})));
    loaded->SetBindState(static_cast<u32>(Button::Mode), 1.0f);
    EXPECT_EQ(TransferCommand(*loaded, LONG_POLL), JoystickReply(Pressed({Button::Square}), 0x7F, 0x7F, 0xFF, 0x7F));
  }
  {
    const auto loaded = MakeJoystick();
    LoadState(*loaded, state, true);
    EXPECT_EQ(TransferCommand(*loaded, POLL), PollReply(Pressed({Button::Circle})));
    loaded->SetBindState(static_cast<u32>(Button::Mode), 1.0f);
    EXPECT_EQ(TransferCommand(*loaded, LONG_POLL), JoystickReply(Pressed({Button::Circle}), 0x7F, 0x00, 0x00, 0x7F));
  }
}

TEST_F(AnalogJoystickTest, LegacyState)
{
  // Saved by version 104 partway through a poll, in analog mode with Circle pressed and the right stick up.
  static const std::vector<u8> state = {0x01, 0xFF, 0xDF, 0x7F, 0x7F, 0x7F, 0x00, 0x03};
  Press(Button::Mode);
  LoadState(m_joystick, state, true, 104);
  EXPECT_TRUE(ModeLED());

  // The transfer starts again from idle.
  EXPECT_EQ(Transfer(LONG_POLL), JoystickReply(Pressed({Button::Circle}), 0x7F, 0x00));
}
