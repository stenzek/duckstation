// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller_test_utils.h"

#include "core/negcon.h"
#include "core/negcon_rumble.h"
#include "core/settings.h"

#include "util/ini_settings_interface.h"

#include <array>
#include <gtest/gtest.h>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

// These tests check what is currently emulated. Comments starting with "Current behaviour:" mark things that look
// questionable, and may change.

static_assert(std::is_same_v<NeGcon::HalfAxis, NeGconInputs::HalfAxis>);
static_assert(std::is_same_v<NeGconRumble::HalfAxis, NeGconInputs::HalfAxis>);
static_assert(static_cast<u32>(NeGcon::Button::Count) == 8);
static_assert(static_cast<u32>(NeGconRumble::Button::Analog) == 8);
static_assert(static_cast<u32>(NeGconRumble::Button::Count) == 9);

using namespace ControllerTestUtils;

namespace {

static constexpr const char* SECTION = "Pad1";

/// Bit in the button state for each NeGconInputs::Button.
static constexpr u8 BUTTON_BITS[] = {3, 4, 5, 6, 7, 11, 12, 13};

template<typename E>
static u16 NegPressed(std::initializer_list<E> buttons)
{
  u16 ret = NO_BUTTONS;
  for (const E button : buttons)
    ret &= static_cast<u16>(~(1u << BUTTON_BITS[static_cast<u8>(button)]));
  return ret;
}

/// NeGcon poll reply, with the axes in the order they're sent.
static std::vector<u8> NegReply(u16 buttons, u8 steering = 0x80, u8 i = 0x00, u8 ii = 0x00, u8 l = 0x00, u8 id = 0x23)
{
  return DeviceReply(id, buttons, {steering, i, ii, l});
}

/// Reply with a different status byte.
static std::vector<u8> WithStatus(std::vector<u8> reply, u8 status)
{
  reply[2] = status;
  return reply;
}

} // namespace

TEST(NeGconInputs, BindingIndices)
{
  const Controller::ControllerBindingInfo* negcon_steering_left = FindBinding(NeGcon::INFO, "SteeringLeft");
  const Controller::ControllerBindingInfo* rumble_analog = FindBinding(NeGconRumble::INFO, "Analog");
  const Controller::ControllerBindingInfo* rumble_steering_left = FindBinding(NeGconRumble::INFO, "SteeringLeft");
  const Controller::ControllerBindingInfo* rumble_large_motor = FindBinding(NeGconRumble::INFO, "LargeMotor");
  const Controller::ControllerBindingInfo* rumble_mode_led = FindBinding(NeGconRumble::INFO, "ModeLED");

  ASSERT_NE(negcon_steering_left, nullptr);
  ASSERT_NE(rumble_analog, nullptr);
  ASSERT_NE(rumble_steering_left, nullptr);
  ASSERT_NE(rumble_large_motor, nullptr);
  ASSERT_NE(rumble_mode_led, nullptr);
  EXPECT_EQ(negcon_steering_left->bind_index, 8u);
  EXPECT_EQ(rumble_analog->bind_index, 8u);
  EXPECT_EQ(rumble_steering_left->bind_index, 9u);
  EXPECT_EQ(rumble_large_motor->bind_index, 14u);
  EXPECT_EQ(rumble_mode_led->bind_index, 16u);
}

TEST(NeGconInputs, CommonButtonMappings)
{
  NeGcon negcon(0);
  NeGconRumble rumble(0);

  negcon.SetBindState(static_cast<u32>(NeGcon::Button::Up), 1.0f);
  negcon.SetBindState(static_cast<u32>(NeGcon::Button::A), 1.0f);
  rumble.SetBindState(static_cast<u32>(NeGconRumble::Button::Up), 1.0f);
  rumble.SetBindState(static_cast<u32>(NeGconRumble::Button::A), 1.0f);

  EXPECT_EQ(negcon.GetButtonStateBits(), UINT16_C(0x2010));
  EXPECT_EQ(rumble.GetButtonStateBits(), UINT16_C(0x2010));
  EXPECT_EQ(negcon.GetBindState(static_cast<u32>(NeGcon::Button::Up)), 1.0f);
  EXPECT_EQ(rumble.GetBindState(static_cast<u32>(NeGconRumble::Button::Up)), 1.0f);
  EXPECT_EQ(rumble.GetBindState(static_cast<u32>(NeGconRumble::Button::Analog)), 0.0f);
}

TEST(NeGconInputs, DefaultAxisModifiers)
{
  NeGcon negcon(0);
  NeGconRumble rumble(0);
  LoadDefaultSettings(negcon);
  LoadDefaultSettings(rumble);
  constexpr u32 negcon_axis_start = static_cast<u32>(NeGcon::Button::Count);
  constexpr u32 rumble_axis_start = static_cast<u32>(NeGconRumble::Button::Count);

  negcon.SetBindState(negcon_axis_start + static_cast<u32>(NeGcon::HalfAxis::SteeringRight), 1.0f);
  rumble.SetBindState(rumble_axis_start + static_cast<u32>(NeGconRumble::HalfAxis::SteeringRight), 1.0f);
  negcon.SetBindState(negcon_axis_start + static_cast<u32>(NeGcon::HalfAxis::I), 0.5f);
  rumble.SetBindState(rumble_axis_start + static_cast<u32>(NeGconRumble::HalfAxis::I), 0.5f);

  ASSERT_TRUE(negcon.GetAnalogInputBytes().has_value());
  ASSERT_TRUE(rumble.GetAnalogInputBytes().has_value());
  EXPECT_EQ(negcon.GetAnalogInputBytes(), rumble.GetAnalogInputBytes());
  EXPECT_EQ(negcon.GetAnalogInputBytes().value(), UINT32_C(0x808080FF));
  EXPECT_EQ(TransferCommand(negcon, LONG_POLL), NegReply(NO_BUTTONS, 0xFF, 0x80, 0x00, 0x00));
  EXPECT_EQ(negcon.GetBindState(negcon_axis_start + static_cast<u32>(NeGcon::HalfAxis::SteeringRight)), 1.0f);
  EXPECT_EQ(rumble.GetBindState(rumble_axis_start + static_cast<u32>(NeGconRumble::HalfAxis::SteeringRight)), 1.0f);
}

TEST(NeGconInputs, ConfiguredAxisModifiers)
{
  INISettingsInterface si;
  si.SetFloatValue("Controller", "SteeringDeadzone", 0.2f);
  si.SetFloatValue("Controller", "SteeringSaturation", 0.8f);
  si.SetFloatValue("Controller", "SteeringScaling", 0.5f);
  si.SetFloatValue("Controller", "IDeadzone", 0.1f);
  si.SetFloatValue("Controller", "ISaturation", 0.9f);
  si.SetFloatValue("Controller", "ILinearity", 0.69314718f);
  si.SetFloatValue("Controller", "IScaling", 0.5f);
  si.SetFloatValue("Controller", "IIDeadzone", 0.25f);
  si.SetFloatValue("Controller", "IISaturation", 0.75f);
  si.SetFloatValue("Controller", "LDeadzone", 0.0f);
  si.SetFloatValue("Controller", "LSaturation", 0.5f);
  si.SetFloatValue("Controller", "LScaling", 0.25f);

  NeGcon negcon(0);
  NeGconRumble rumble(0);
  negcon.LoadSettings(si, "Controller", true);
  rumble.LoadSettings(si, "Controller", true);

  constexpr u32 negcon_axis_start = static_cast<u32>(NeGcon::Button::Count);
  constexpr u32 rumble_axis_start = static_cast<u32>(NeGconRumble::Button::Count);
  negcon.SetBindState(negcon_axis_start + static_cast<u32>(NeGcon::HalfAxis::SteeringRight), 0.5f);
  rumble.SetBindState(rumble_axis_start + static_cast<u32>(NeGconRumble::HalfAxis::SteeringRight), 0.5f);
  negcon.SetBindState(negcon_axis_start + static_cast<u32>(NeGcon::HalfAxis::I), 0.5f);
  rumble.SetBindState(rumble_axis_start + static_cast<u32>(NeGconRumble::HalfAxis::I), 0.5f);
  negcon.SetBindState(negcon_axis_start + static_cast<u32>(NeGcon::HalfAxis::II), 0.5f);
  rumble.SetBindState(rumble_axis_start + static_cast<u32>(NeGconRumble::HalfAxis::II), 0.5f);
  negcon.SetBindState(negcon_axis_start + static_cast<u32>(NeGcon::HalfAxis::L), 0.5f);
  rumble.SetBindState(rumble_axis_start + static_cast<u32>(NeGconRumble::HalfAxis::L), 0.5f);

  ASSERT_TRUE(negcon.GetAnalogInputBytes().has_value());
  ASSERT_TRUE(rumble.GetAnalogInputBytes().has_value());
  EXPECT_EQ(negcon.GetAnalogInputBytes(), rumble.GetAnalogInputBytes());
  EXPECT_EQ(negcon.GetAnalogInputBytes().value(), UINT32_C(0x808080A0));
  EXPECT_EQ(TransferCommand(negcon, LONG_POLL), NegReply(NO_BUTTONS, 0xA0, 0x20, 0x80, 0x40));
}

TEST(NeGconInputs, RumbleSettings)
{
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "SteeringDeadzone"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "SteeringSaturation"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "SteeringLinearity"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "SteeringScaling"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "IDeadzone"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "ISaturation"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "ILinearity"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "IScaling"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "IIDeadzone"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "IISaturation"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "IILinearity"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "IIScaling"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "LDeadzone"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "LSaturation"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "LLinearity"));
  EXPECT_TRUE(HasSetting(NeGconRumble::INFO, "LScaling"));
  EXPECT_FALSE(HasSetting(NeGconRumble::INFO, "SteeringSensitivity"));
}

TEST(NeGconInputs, DeviceProtocols)
{
  NeGcon negcon(0);
  negcon.SetBindState(static_cast<u32>(NeGcon::Button::A), 1.0f);

  std::vector<bool> acks;
  EXPECT_EQ(TransferBytes(negcon, POLL, &acks), (std::vector<u8>{0xFF, 0x23, 0x5A, 0xFF, 0xDF}));
  EXPECT_EQ(acks, std::vector<bool>(POLL.size(), true));

  NeGconRumble rumble(0);
  rumble.SetBindState(static_cast<u32>(NeGconRumble::Button::A), 1.0f);
  EXPECT_EQ(TransferCommand(rumble, POLL), PollReply(0xDFFF));
}

namespace {

using NeGconButton = NeGcon::Button;

static constexpr u32 NeGconHalfAxis(NeGcon::HalfAxis axis)
{
  return static_cast<u32>(NeGconButton::Count) + static_cast<u32>(axis);
}

static std::unique_ptr<NeGcon> MakeNeGcon()
{
  auto negcon = std::make_unique<NeGcon>(0);
  LoadDefaultSettings(*negcon);
  return negcon;
}

} // namespace

TEST(NeGcon, Poll)
{
  const auto negcon = MakeNeGcon();
  EXPECT_EQ(negcon->GetType(), ControllerType::NeGcon);

  // Steering is centred at 80h, the others are released at 00h.
  EXPECT_EQ(TransferCommand(*negcon, LONG_POLL), NegReply(NO_BUTTONS));
  EXPECT_EQ(negcon->GetAnalogInputBytes(), 0x80808080u);

  // The axes are sent as steering, I, II, L.
  negcon->SetBindState(static_cast<u32>(NeGconButton::A), 1.0f);
  negcon->SetBindState(static_cast<u32>(NeGconButton::Start), 1.0f);
  negcon->SetBindState(NeGconHalfAxis(NeGcon::HalfAxis::SteeringLeft), 1.0f);
  negcon->SetBindState(NeGconHalfAxis(NeGcon::HalfAxis::I), 0.5f);
  negcon->SetBindState(NeGconHalfAxis(NeGcon::HalfAxis::II), 1.0f);
  negcon->SetBindState(NeGconHalfAxis(NeGcon::HalfAxis::L), 0.25f);
  const u16 buttons = NegPressed({NeGconButton::A, NeGconButton::Start});
  EXPECT_EQ(TransferCommand(*negcon, LONG_POLL), NegReply(buttons, 0x00, 0x80, 0xFF, 0x40));
  // Only steering is reported as analog input, the others are centred.
  EXPECT_EQ(negcon->GetAnalogInputBytes(), 0x80808000u);

  // Back-to-back polls don't need a reset, and the bytes after 42h are ignored.
  EXPECT_EQ(TransferCommand(*negcon, {0x01, 0x42, 0xFF, 0x5A, 0x01, 0x02, 0x03, 0x04, 0x05}),
            NegReply(buttons, 0x00, 0x80, 0xFF, 0x40));
}

TEST(NeGcon, Buttons)
{
  const auto negcon = MakeNeGcon();
  for (u32 i = 0; i < static_cast<u32>(NeGconButton::Count); i++)
  {
    negcon->SetBindState(i, 0.49f);
    EXPECT_EQ(negcon->GetBindState(i), 0.0f) << i;
    negcon->SetBindState(i, 0.5f);
    EXPECT_EQ(negcon->GetBindState(i), 1.0f) << i;
    EXPECT_EQ(negcon->GetButtonStateBits(), 1u << BUTTON_BITS[i]) << i;
    EXPECT_EQ(TransferCommand(*negcon, LONG_POLL), NegReply(NegPressed({static_cast<NeGconButton>(i)}))) << i;
    negcon->SetBindState(i, 0.0f);
  }

  static constexpr std::pair<const char*, u32> bindings[] = {
    {"Start", 0}, {"Up", 1},  {"Right", 2}, {"Down", 3},         {"Left", 4},          {"R", 5}, {"B", 6}, {"A", 7},
    {"I", 10},    {"II", 11}, {"L", 12},    {"SteeringLeft", 8}, {"SteeringRight", 9},
  };
  for (const auto& [name, index] : bindings)
    EXPECT_EQ(GetBindIndex(NeGcon::INFO, name), index) << name;
  EXPECT_EQ(NeGcon::INFO.bindings.size(), std::size(bindings));
  EXPECT_TRUE(HasSetting(NeGcon::INFO, "DisableSOCD"));
}

TEST(NeGcon, RejectedBytes)
{
  const auto negcon = MakeNeGcon();
  ExpectRejectedBytes(*negcon, {0x00, 0x43, 0x44, 0x45, 0x4D}, 0x23, LONG_POLL, NegReply(NO_BUTTONS));
}

TEST(NeGcon, ResetTransferState)
{
  const auto negcon = MakeNeGcon();
  negcon->SetBindState(static_cast<u32>(NeGconButton::B), 1.0f);
  for (u32 i = 0; i < 2; i++)
  {
    for (size_t len = 1; len < LONG_POLL.size(); len++)
    {
      TransferBytes(*negcon, std::vector<u8>(LONG_POLL.begin(), LONG_POLL.begin() + len));
      if (i == 0)
        negcon->ResetTransferState();
      else
        negcon->Reset();

      EXPECT_EQ(TransferCommand(*negcon, LONG_POLL), NegReply(NegPressed({NeGconButton::B})))
        << (i ? "Reset" : "ResetTransferState") << " after " << len;
    }
  }
}

TEST(NeGcon, DisableSOCD)
{
  const auto negcon = MakeNeGcon();
  for (const NeGconButton button : {NeGconButton::Up, NeGconButton::Down, NeGconButton::Left, NeGconButton::Right})
    negcon->SetBindState(static_cast<u32>(button), 1.0f);
  EXPECT_EQ(TransferCommand(*negcon, LONG_POLL),
            NegReply(NegPressed({NeGconButton::Up, NeGconButton::Down, NeGconButton::Left, NeGconButton::Right})));

  // Up is kept over Down, and Right over Left.
  INISettingsInterface si;
  si.SetBoolValue(SECTION, "DisableSOCD", true);
  negcon->LoadSettings(si, SECTION, false);
  EXPECT_EQ(TransferCommand(*negcon, LONG_POLL), NegReply(NegPressed({NeGconButton::Up, NeGconButton::Right})));
}

TEST(NeGcon, SaveState)
{
  const auto negcon = MakeNeGcon();
  negcon->SetBindState(static_cast<u32>(NeGconButton::R), 1.0f);

  // Partway through a poll, the rest of it continues after loading.
  ExpectResumesAfterLoad(*negcon, MakeNeGcon, LONG_POLL, NegReply(NegPressed({NeGconButton::R})));

  // Input state, including the axes, is only loaded if asked to.
  negcon->SetBindState(NeGconHalfAxis(NeGcon::HalfAxis::I), 1.0f);
  const std::vector<u8> state = SaveState(*negcon);
  const auto loaded = MakeNeGcon();
  loaded->SetBindState(static_cast<u32>(NeGconButton::A), 1.0f);
  loaded->SetBindState(NeGconHalfAxis(NeGcon::HalfAxis::L), 1.0f);
  LoadState(*loaded, state, false);
  EXPECT_EQ(TransferCommand(*loaded, LONG_POLL), NegReply(NegPressed({NeGconButton::A}), 0x80, 0x00, 0x00, 0xFF));
  LoadState(*loaded, state, true);
  EXPECT_EQ(TransferCommand(*loaded, LONG_POLL), NegReply(NegPressed({NeGconButton::R}), 0x80, 0xFF, 0x00, 0x00));
}

namespace {

using RumbleButton = NeGconRumble::Button;

static constexpr u32 RumbleHalfAxis(NeGconRumble::HalfAxis axis)
{
  return static_cast<u32>(RumbleButton::Count) + static_cast<u32>(axis);
}

/// NeGconRumble analog mode poll reply.
static std::vector<u8> RumbleReply(u16 buttons, u8 steering = 0x80, u8 i = 0x00, u8 ii = 0x00, u8 l = 0x00)
{
  return NegReply(buttons, steering, i, ii, l);
}

static std::vector<u8> RumbleConfigPollReply(u16 buttons, u8 steering = 0x80, u8 i = 0x00, u8 ii = 0x00, u8 l = 0x00)
{
  return NegReply(buttons, steering, i, ii, l, 0xF3);
}

class NeGconRumbleTest : public ControllerSettingsTest
{
protected:
  void SetUp() override
  {
    ControllerSettingsTest::SetUp();
    LoadDefaultSettings(m_rumble);
  }

  void LoadSettings(const INISettingsInterface& si) { m_rumble.LoadSettings(si, SECTION, false); }
  void Press(RumbleButton button, float value = 1.0f) { m_rumble.SetBindState(static_cast<u32>(button), value); }
  void ToggleAnalog() { Press(RumbleButton::Analog); }

  float SmallMotor() const { return m_rumble.GetBindState(GetBindIndex(NeGconRumble::INFO, "SmallMotor")); }
  float LargeMotor() const { return m_rumble.GetBindState(GetBindIndex(NeGconRumble::INFO, "LargeMotor")); }
  bool ModeLED() const { return m_rumble.GetBindState(GetBindIndex(NeGconRumble::INFO, "ModeLED")) != 0.0f; }

  std::vector<u8> Transfer(const std::vector<u8>& tx) { return TransferCommand(m_rumble, tx); }

  /// Enters config mode from digital mode, sets the analog mode without locking it, and maps the motors.
  void SetupDualShock(bool analog)
  {
    EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
    EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, analog ? 0x01 : 0x00, 0x02)), ANALOG_CONFIG_REPLY);
    EXPECT_EQ(Transfer(MapMotorsCommand({0x00, 0x01})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  }

  static std::unique_ptr<NeGconRumble> MakeRumble()
  {
    auto rumble = std::make_unique<NeGconRumble>(0);
    LoadDefaultSettings(*rumble);
    return rumble;
  }

  NeGconRumble m_rumble{0};
};

} // namespace

TEST_F(NeGconRumbleTest, BindingIndices)
{
  EXPECT_EQ(m_rumble.GetType(), ControllerType::NeGconRumble);

  static constexpr std::pair<const char*, u32> bindings[] = {
    {"Start", 0},
    {"Up", 1},
    {"Right", 2},
    {"Down", 3},
    {"Left", 4},
    {"R", 5},
    {"B", 6},
    {"A", 7},
    {"Analog", 8},
    {"I", 11},
    {"II", 12},
    {"L", 13},
    {"SteeringLeft", 9},
    {"SteeringRight", 10},
    {"LargeMotor", 14},
    {"SmallMotor", 15},
    {"ModeLED", 16},
  };
  for (const auto& [name, index] : bindings)
    EXPECT_EQ(GetBindIndex(NeGconRumble::INFO, name), index) << name;
  EXPECT_EQ(NeGconRumble::INFO.bindings.size(), std::size(bindings));
  for (const char* setting : {"DisableSOCD", "LargeMotorVibrationBias", "SmallMotorVibrationBias"})
    EXPECT_TRUE(HasSetting(NeGconRumble::INFO, setting)) << setting;
}

TEST_F(NeGconRumbleTest, DigitalModePoll)
{
  // Starts in digital mode, where only the buttons are sent.
  EXPECT_FALSE(ModeLED());
  Press(RumbleButton::A);
  m_rumble.SetBindState(RumbleHalfAxis(NeGconRumble::HalfAxis::I), 1.0f);
  EXPECT_EQ(Transfer(POLL), PollReply(NegPressed({RumbleButton::A})));

  // 43h outside config mode also polls.
  EXPECT_EQ(Transfer({0x01, 0x43, 0x00, 0x00, 0x00}), PollReply(NegPressed({RumbleButton::A})));
}

TEST_F(NeGconRumbleTest, AnalogModePoll)
{
  EnterAnalogMode(m_rumble);
  EXPECT_TRUE(ModeLED());
  EXPECT_EQ(Transfer(LONG_POLL), RumbleReply(NO_BUTTONS));

  Press(RumbleButton::B);
  m_rumble.SetBindState(RumbleHalfAxis(NeGconRumble::HalfAxis::SteeringRight), 1.0f);
  m_rumble.SetBindState(RumbleHalfAxis(NeGconRumble::HalfAxis::I), 0.5f);
  m_rumble.SetBindState(RumbleHalfAxis(NeGconRumble::HalfAxis::L), 1.0f);
  const u16 buttons = NegPressed({RumbleButton::B});
  EXPECT_EQ(Transfer(LONG_POLL), RumbleReply(buttons, 0xFF, 0x80, 0x00, 0xFF));
  EXPECT_EQ(m_rumble.GetAnalogInputBytes(), 0x808080FFu);

  // 43h outside config mode polls too.
  EXPECT_EQ(Transfer({0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
            RumbleReply(buttons, 0xFF, 0x80, 0x00, 0xFF));
}

TEST_F(NeGconRumbleTest, ConfigMode)
{
  Press(RumbleButton::Up);
  const u16 buttons = NegPressed({RumbleButton::Up});

  // Entering is applied on the last byte, so an aborted 43h does nothing.
  const std::vector<u8> enter = EnterConfigModeCommand();
  TransferBytes(m_rumble, std::vector<u8>(enter.begin(), enter.end() - 1));
  m_rumble.ResetTransferState();
  EXPECT_EQ(Transfer(POLL), PollReply(buttons));

  // In config mode, 42h sends the buttons and axes, and 43h sends zeros.
  EXPECT_EQ(Transfer(enter), PollReply(buttons));
  EXPECT_EQ(Transfer(LONG_POLL), RumbleConfigPollReply(buttons));
  EXPECT_EQ(Transfer(ConfigCommand(0x43, 0x01)), ConfigReply({}));
  EXPECT_EQ(Transfer(LONG_POLL), RumbleConfigPollReply(buttons));

  // Any parameter other than 01h leaves.
  EXPECT_EQ(Transfer(ConfigCommand(0x43, 0x02)), ConfigReply({}));
  EXPECT_EQ(Transfer(POLL), PollReply(buttons));
}

TEST_F(NeGconRumbleTest, RejectedBytes)
{
  // The ID byte is still sent for other commands, but not ACKed.
  ExpectRejectedBytes(m_rumble, {0x00, 0x40, 0x44, 0x45, 0x46, 0x47, 0x4C, 0x4D, 0x53}, 0x41, POLL,
                      PollReply(NO_BUTTONS));

  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  ExpectRejectedBytes(m_rumble, {0x00, 0x3F, 0x50, 0x53}, 0xF3, LONG_POLL, RumbleConfigPollReply(NO_BUTTONS));
}

TEST_F(NeGconRumbleTest, ResetTransferState)
{
  EnterAnalogMode(m_rumble);
  Press(RumbleButton::R);
  for (size_t len = 1; len < LONG_POLL.size(); len++)
  {
    TransferBytes(m_rumble, std::vector<u8>(LONG_POLL.begin(), LONG_POLL.begin() + len));
    m_rumble.ResetTransferState();
    EXPECT_EQ(Transfer(LONG_POLL), RumbleReply(NegPressed({RumbleButton::R}))) << len;
  }
}

TEST_F(NeGconRumbleTest, SetAnalogMode)
{
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));

  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_TRUE(ModeLED());
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x05, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_TRUE(ModeLED());
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x00, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_FALSE(ModeLED());
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0xFF, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_FALSE(ModeLED());

  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(LONG_POLL), RumbleReply(NO_BUTTONS));
}

TEST_F(NeGconRumbleTest, SetAnalogModeLock)
{
  // 03h locks the mode, so the button does nothing.
  SetCanStartInAnalogMode(true);
  EnterAnalogMode(m_rumble);
  ToggleAnalog();
  EXPECT_TRUE(ModeLED());

  // 00h-02h unlock and 03h locks, higher values act as the low two bits.
  // Toggling in DualShock mode clears the status byte, until config mode is entered again.
  u8 status = 0x5A;
  for (const auto [lock, locked] :
       {std::pair<u8, bool>{0x00, false}, std::pair<u8, bool>{0x03, true}, std::pair<u8, bool>{0x01, false},
        std::pair<u8, bool>{0x02, false}, std::pair<u8, bool>{0x04, false}, std::pair<u8, bool>{0x07, true},
        std::pair<u8, bool>{0xFC, false}, std::pair<u8, bool>{0xFF, true}})
  {
    EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), WithStatus(RumbleReply(NO_BUTTONS), status)) << u32(lock);
    EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, lock)), ANALOG_CONFIG_REPLY);
    EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
    status = 0x5A;
    ToggleAnalog();
    EXPECT_EQ(ModeLED(), locked) << u32(lock);
    if (!locked)
    {
      ToggleAnalog();
      status = 0x00;
    }
  }
}

TEST_F(NeGconRumbleTest, SetAnalogModeResetsRumble)
{
  SetupDualShock(true);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0xFF})), RumbleReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(LargeMotor(), 1.0f);

  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), RumbleReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_EQ(LargeMotor(), 0.0f);
  EXPECT_EQ(Transfer(MapMotorsCommand({})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST_F(NeGconRumbleTest, ConfigTables)
{
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));

  // 45h reports the mode.
  EXPECT_EQ(Transfer(ConfigCommand(0x45, 0x00)), ConfigReply({0x01, 0x02, 0x00, 0x02, 0x01, 0x00}));
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(ConfigCommand(0x45, 0x00)), ConfigReply({0x01, 0x02, 0x01, 0x02, 0x01, 0x00}));

  static constexpr struct
  {
    u8 command;
    u8 param;
    std::array<u8, 6> reply;
  } cases[] = {
    {0x46, 0x00, {0x00, 0x00, 0x01, 0x02, 0x00, 0x0A}}, {0x46, 0x01, {0x00, 0x00, 0x01, 0x01, 0x01, 0x14}},
    {0x46, 0x02, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, {0x47, 0x00, {0x00, 0x00, 0x02, 0x00, 0x01, 0x00}},
    {0x47, 0x01, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, {0x4C, 0x00, {0x00, 0x00, 0x00, 0x04, 0x00, 0x00}},
    {0x4C, 0x01, {0x00, 0x00, 0x00, 0x02, 0x00, 0x00}}, {0x4C, 0x02, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x48, 0x00, {0x00, 0x00, 0x00, 0x00, 0x01, 0x00}}, {0x48, 0x01, {0x00, 0x00, 0x00, 0x00, 0x01, 0x00}},
    {0x48, 0x02, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, {0x48, 0xFF, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
  };
  for (const auto& c : cases)
  {
    EXPECT_EQ(Transfer(ConfigCommand(c.command, c.param)),
              ConfigReply({c.reply[0], c.reply[1], c.reply[2], c.reply[3], c.reply[4], c.reply[5]}))
      << std::hex << u32(c.command) << " " << u32(c.param);
  }

  // The DualShock 2 and unused commands only return zeros, whatever is sent.
  for (const u8 command : {0x40, 0x41, 0x49, 0x4A, 0x4B, 0x4E, 0x4F})
  {
    EXPECT_EQ(Transfer({0x01, command, 0x00, 0x01, 0xFF, 0x02, 0x03, 0x04, 0xFF}), ConfigReply({}))
      << "command 0x" << std::hex << u32(command);
  }
}

TEST_F(NeGconRumbleTest, MapMotors)
{
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));

  // All six bytes are stored, and the previous mapping is sent back.
  EXPECT_EQ(Transfer(MapMotorsCommand({0x00, 0x01, 0x02, 0x03, 0x04, 0x05})),
            ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Transfer(MapMotorsCommand({0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x00})),
            ConfigReply({0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));

  // The last bytes work, and unmapping stops the motors. In digital mode the reply is lengthened to include them.
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), (std::vector<u8>{0xFF, 0x43, 0x5A, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00}));
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x02)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(MapMotorsCommand({0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00})),
            ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0x00, 0x00, 0x00, 0xFF, 0x01})), RumbleReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(LargeMotor(), 1.0f);

  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), RumbleReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(MapMotorsCommand({0xFF, 0xFF, 0xFF, 0xFF, 0x01})),
            ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00}));
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_EQ(LargeMotor(), 1.0f);
  EXPECT_EQ(Transfer(MapMotorsCommand({})), ConfigReply({0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0xFF}));
  EXPECT_EQ(LargeMotor(), 0.0f);
}

TEST_F(NeGconRumbleTest, DualShockRumble)
{
  SetupDualShock(true);

  // Only bit 0 of the small motor's byte matters.
  for (const auto [value, on] : {std::pair<u8, bool>{0x01, true}, std::pair<u8, bool>{0xFE, false},
                                 std::pair<u8, bool>{0x03, true}, std::pair<u8, bool>{0x00, false}})
  {
    EXPECT_EQ(Transfer(RumblePoll({value, 0x00})), RumbleReply(NO_BUTTONS));
    EXPECT_EQ(SmallMotor(), on ? 1.0f : 0.0f) << "value 0x" << std::hex << u32(value);
  }

  // The large motor is scaled, including the default bias of 8.
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0x80})), RumbleReply(NO_BUTTONS));
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(136));
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0x01})), RumbleReply(NO_BUTTONS));
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(9));
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0xFF})), RumbleReply(NO_BUTTONS));
  EXPECT_EQ(LargeMotor(), 1.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0x00})), RumbleReply(NO_BUTTONS));
  EXPECT_EQ(LargeMotor(), 0.0f);

  // An aborted poll keeps the bytes that were received.
  TransferBytes(m_rumble, RumblePoll({0x01, 0xFF}, 5));
  m_rumble.ResetTransferState();
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(LargeMotor(), 1.0f);

  // Each motor uses its own bias, which is applied when the strength is read.
  INISettingsInterface si;
  si.SetIntValue(SECTION, "LargeMotorVibrationBias", -1);
  si.SetIntValue(SECTION, "SmallMotorVibrationBias", -128);
  LoadSettings(si);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0x80})), RumbleReply(NO_BUTTONS));
  EXPECT_FLOAT_EQ(SmallMotor(), MotorStrength(127));
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(127));
  si.SetIntValue(SECTION, "LargeMotorVibrationBias", 0);
  LoadSettings(si);
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(128));
}

TEST_F(NeGconRumbleTest, DualShockRumble_DigitalMode)
{
  // Only two motor bytes are received in digital mode.
  SetupDualShock(false);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0x80}, 5)), PollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(136));

  // Mapping a motor past the second byte lengthens the reply, to include it.
  EXPECT_EQ(Transfer(RumblePoll({0x00, 0x00}, 5)), PollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(MapMotorsCommand({0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF})),
            ConfigReply({0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(SmallMotor(), 0.0f);
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
}

TEST_F(NeGconRumbleTest, LegacyRumble)
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
  EXPECT_EQ(Transfer(RumblePoll({0x40, 0x01})), RumbleReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x40, 0x00})), RumbleReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 0.0f);
}

TEST_F(NeGconRumbleTest, AnalogButton)
{
  // Toggles straight away when idle, and the LED follows.
  ToggleAnalog();
  EXPECT_TRUE(ModeLED());
  EXPECT_EQ(m_rumble.GetBindState(static_cast<u32>(RumbleButton::Analog)), 0.0f);
  EXPECT_EQ(Transfer(LONG_POLL), RumbleReply(NO_BUTTONS));
  Press(RumbleButton::Analog, 0.49f);
  EXPECT_TRUE(ModeLED());
  ToggleAnalog();
  EXPECT_FALSE(ModeLED());

  // Partway through a command, it waits for the transfer to be reset.
  TransferBytes(m_rumble, {0x01, 0x42});
  ToggleAnalog();
  EXPECT_FALSE(ModeLED());
  EXPECT_EQ(Transfer({0x00, 0x00, 0x00}), (std::vector<u8>{0x5A, 0xFF, 0xFF}));
  EXPECT_FALSE(ModeLED());
  m_rumble.ResetTransferState();
  EXPECT_TRUE(ModeLED());
  m_rumble.ResetTransferState();
  EXPECT_TRUE(ModeLED());
}

TEST_F(NeGconRumbleTest, AnalogButton_ResetsRumble)
{
  SetCanStartInAnalogMode(true);
  SetupDualShock(true);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0xFF})), RumbleReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);

  ToggleAnalog();
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_EQ(LargeMotor(), 0.0f);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0xFF}, 5)), WithStatus(PollReply(NO_BUTTONS), 0x00));
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_EQ(LargeMotor(), 0.0f);
}

TEST_F(NeGconRumbleTest, AnalogButton_StatusByte)
{
  // After DualShock mode is enabled, toggling with the button clears the status byte until config mode is entered.
  SetCanStartInAnalogMode(true);
  SetupDualShock(true);
  ToggleAnalog();
  EXPECT_EQ(Transfer(POLL), WithStatus(PollReply(NO_BUTTONS), 0x00));
  ToggleAnalog();
  EXPECT_EQ(Transfer(LONG_POLL), WithStatus(RumbleReply(NO_BUTTONS), 0x00));
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), WithStatus(RumbleReply(NO_BUTTONS), 0x00));
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(LONG_POLL), RumbleReply(NO_BUTTONS));

  // Not without DualShock mode.
  SetCanStartInAnalogMode(false);
  m_rumble.Reset();
  ToggleAnalog();
  EXPECT_EQ(Transfer(LONG_POLL), RumbleReply(NO_BUTTONS));
}

TEST_F(NeGconRumbleTest, AnalogButton_LeavesDualShockMode)
{
  // If the game can't start in analog mode, switching to digital mode leaves config and DualShock mode.
  SetupDualShock(true);
  ToggleAnalog();
  EXPECT_FALSE(ModeLED());
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));
  EXPECT_EQ(Transfer(RumblePoll({0x40, 0x01}, 5)), PollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
}

TEST_F(NeGconRumbleTest, Reset)
{
  SetCanStartInAnalogMode(true);
  SetupDualShock(true);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0xFF})), RumbleReply(NO_BUTTONS));
  ToggleAnalog();
  EXPECT_EQ(Transfer(EnterConfigModeCommand()), WithStatus(PollReply(NO_BUTTONS), 0x00));
  SetCanStartInAnalogMode(false);

  // Clears the modes, the motors, the mapping and the status byte. Without CanStartInAnalogMode(), it's digital.
  m_rumble.Reset();
  EXPECT_FALSE(ModeLED());
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_EQ(LargeMotor(), 0.0f);
  EXPECT_EQ(Transfer(POLL), PollReply(NO_BUTTONS));

  // DualShock mode is off, so legacy rumble works again.
  EXPECT_EQ(Transfer(RumblePoll({0x40, 0x01}, 5)), PollReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);

  // With CanStartInAnalogMode(), it's analog after a reset.
  SetCanStartInAnalogMode(true);
  m_rumble.Reset();
  EXPECT_TRUE(ModeLED());
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_EQ(Transfer(LONG_POLL), RumbleReply(NO_BUTTONS));
  SetCanStartInAnalogMode(false);
  m_rumble.Reset();
  EXPECT_FALSE(ModeLED());
}

TEST_F(NeGconRumbleTest, Reset_ClearsLock)
{
  EnterAnalogMode(m_rumble);
  m_rumble.Reset();
  EXPECT_FALSE(ModeLED());
  ToggleAnalog();
  EXPECT_TRUE(ModeLED());
}

TEST_F(NeGconRumbleTest, DisableSOCD)
{
  INISettingsInterface si;
  si.SetBoolValue(SECTION, "DisableSOCD", true);
  LoadSettings(si);
  for (const RumbleButton button : {RumbleButton::Up, RumbleButton::Down, RumbleButton::Left, RumbleButton::Right})
    Press(button);
  EXPECT_EQ(Transfer(POLL), PollReply(NegPressed({RumbleButton::Up, RumbleButton::Right})));
  ToggleAnalog();
  EXPECT_EQ(Transfer(LONG_POLL), RumbleReply(NegPressed({RumbleButton::Up, RumbleButton::Right})));
}

TEST_F(NeGconRumbleTest, SaveState)
{
  SetupDualShock(true);
  Press(RumbleButton::Start);

  // Partway through a command, the rest of it continues after loading.
  ExpectResumesAfterLoad(m_rumble, MakeRumble, RumblePoll({0x00, 0x00}),
                         RumbleReply(NegPressed({RumbleButton::Start})));
  ExpectResumesAfterLoad(m_rumble, MakeRumble, ENTER_CONFIG_LONG, RumbleReply(NegPressed({RumbleButton::Start})));
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), RumbleReply(NegPressed({RumbleButton::Start})));
  ExpectResumesAfterLoad(m_rumble, MakeRumble, ExitConfigModeCommand(), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(RumblePoll({0x01, 0x80})), RumbleReply(NegPressed({RumbleButton::Start})));

  const std::vector<u8> state = SaveState(m_rumble);
  {
    const auto loaded = MakeRumble();
    LoadState(*loaded, state, true);
    EXPECT_EQ(loaded->GetBindState(GetBindIndex(NeGconRumble::INFO, "SmallMotor")), 1.0f);
    EXPECT_FLOAT_EQ(loaded->GetBindState(GetBindIndex(NeGconRumble::INFO, "LargeMotor")), MotorStrength(136));
    EXPECT_EQ(TransferCommand(*loaded, RumblePoll({0x00, 0xFF})), RumbleReply(NegPressed({RumbleButton::Start})));
    EXPECT_EQ(loaded->GetBindState(GetBindIndex(NeGconRumble::INFO, "SmallMotor")), 0.0f);
    EXPECT_EQ(loaded->GetBindState(GetBindIndex(NeGconRumble::INFO, "LargeMotor")), 1.0f);
  }

  // Input state is only loaded if asked to, but the mode is always loaded.
  {
    const auto loaded = MakeRumble();
    loaded->SetBindState(static_cast<u32>(RumbleButton::B), 1.0f);
    LoadState(*loaded, state, false);
    EXPECT_EQ(loaded->GetBindState(GetBindIndex(NeGconRumble::INFO, "ModeLED")), 1.0f);
    EXPECT_EQ(TransferCommand(*loaded, RumblePoll({0x01, 0x00})), RumbleReply(NegPressed({RumbleButton::B})));
    EXPECT_EQ(loaded->GetBindState(GetBindIndex(NeGconRumble::INFO, "SmallMotor")), 1.0f);
  }

  // The lock is saved too.
  EXPECT_EQ(Transfer(ENTER_CONFIG_LONG), RumbleReply(NegPressed({RumbleButton::Start})));
  EXPECT_EQ(Transfer(SetAnalogModeCommand(0x01, 0x01, 0x03)), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(Transfer(ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
  const std::vector<u8> locked_state = SaveState(m_rumble);
  {
    const auto loaded = MakeRumble();
    LoadState(*loaded, locked_state, true);
    loaded->SetBindState(static_cast<u32>(RumbleButton::Analog), 1.0f);
    EXPECT_EQ(loaded->GetBindState(GetBindIndex(NeGconRumble::INFO, "ModeLED")), 1.0f);
  }
}

TEST(NeGcon, LegacyState)
{
  // Saved by version 104 partway through a poll, with R pressed. Inputs aren't loaded from older states.
  static const std::vector<u8> state = {0xFF, 0xF7, 0x03};
  const auto negcon = MakeNeGcon();
  LoadState(*negcon, state, true, 104);

  // The transfer starts again from idle.
  EXPECT_EQ(TransferCommand(*negcon, LONG_POLL), NegReply(NO_BUTTONS));
}

TEST_F(NeGconRumbleTest, LegacyState)
{
  // Saved by version 104 in analog mode with both motors mapped, Start pressed and the large motor on. Inputs aren't
  // loaded from older states.
  static const std::vector<u8> state = {0x01, 0x01, 0x00, 0x5A, 0xF7, 0xFF, 0x02, 0x00, 0x01, 0xFF, 0xFF, 0xFF,
                                        0xFF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00};
  LoadState(m_rumble, state, true, 104);
  EXPECT_TRUE(ModeLED());
  EXPECT_EQ(SmallMotor(), 0.0f);
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(0x80 + 8));

  EXPECT_EQ(Transfer(RumblePoll({0x01, 0x00})), RumbleReply(NO_BUTTONS));
  EXPECT_EQ(SmallMotor(), 1.0f);
  EXPECT_EQ(LargeMotor(), 0.0f);
}

TEST_F(NeGconRumbleTest, LegacyState_Version44)
{
  // Before version 45, without the status byte, the mapping, the motor indices or the queued toggle. The large motor
  // was first.
  static const std::vector<u8> state = {0x01, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x40, 0x00};
  LoadState(m_rumble, state, true, 44);
  EXPECT_TRUE(ModeLED());
  EXPECT_FLOAT_EQ(LargeMotor(), MotorStrength(0x40 + 8));
  EXPECT_EQ(Transfer(LONG_POLL), RumbleReply(NO_BUTTONS));
}
