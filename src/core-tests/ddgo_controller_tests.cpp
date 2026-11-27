// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller_test_utils.h"

#include "core/ddgo_controller.h"

#include "util/ini_settings_interface.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <iterator>
#include <memory>
#include <string_view>
#include <vector>

// These tests check what is currently emulated. Comments starting with "Current behaviour:" mark things that look
// questionable, and may change.

using namespace ControllerTestUtils;

namespace {

using Bind = DDGoController::Bind;

static constexpr const char* SECTION = "Pad1";

/// Bits 4 and 6 always read as pressed.
static constexpr u16 ALWAYS_LOW = (1u << 4) | (1u << 6);

/// Bits that are low for each power level (N, P1-P5), by bit number.
static const std::vector<u8> POWER_LEVEL_BITS[] = {
  {7, 5}, {12, 5}, {5}, {12, 7}, {7}, {12},
};

/// Bits that are low for each brake level (released, B1-B8, emergency), by bit number.
static const std::vector<u8> BRAKE_LEVEL_BITS[] = {
  {8, 11, 9}, {10, 11, 9}, {11, 9}, {10, 8, 9}, {8, 9}, {10, 9}, {9}, {10, 8, 11}, {8, 11}, {},
};

static const std::vector<u8> BRAKE_TRANSITION_BITS = {10, 8, 11, 9};

static constexpr u32 EMERGENCY = 9;

/// The button state sent for the given power and brake bits, plus any pressed buttons.
static u16 State(const std::vector<u8>& power_bits, const std::vector<u8>& brake_bits,
                 std::initializer_list<Bind> buttons = {})
{
  u16 ret = Pressed(buttons) & static_cast<u16>(~ALWAYS_LOW);
  for (const u8 bit : power_bits)
    ret &= static_cast<u16>(~(1u << bit));
  for (const u8 bit : brake_bits)
    ret &= static_cast<u16>(~(1u << bit));
  return ret;
}

static u16 LevelState(u32 power, u32 brake, std::initializer_list<Bind> buttons = {})
{
  return State(POWER_LEVEL_BITS[power], BRAKE_LEVEL_BITS[brake], buttons);
}

static u32 VirtualPower(u32 level)
{
  return static_cast<u32>(Bind::VirtualPowerOff) + level;
}

static u32 VirtualBrake(u32 level)
{
  return static_cast<u32>(Bind::VirtualBrakeReleased) + level;
}

static std::unique_ptr<DDGoController> MakeController(const INISettingsInterface* si = nullptr)
{
  auto controller = std::make_unique<DDGoController>(0);
  if (si)
    controller->LoadSettings(*si, SECTION, true);
  else
    LoadDefaultSettings(*controller);
  controller->Reset();
  return controller;
}

/// Without transition frames, so level changes are seen straight away.
static std::unique_ptr<DDGoController> MakeInstantController()
{
  INISettingsInterface si;
  si.SetIntValue(SECTION, "PowerTransitionFrames", 0);
  si.SetIntValue(SECTION, "BrakeTransitionFrames", 0);
  return MakeController(&si);
}

static void PressBind(Controller& controller, Bind bind, float value = 1.0f)
{
  controller.SetBindState(static_cast<u32>(bind), value);
}

} // namespace

TEST(DDGoController, Poll)
{
  const auto controller = MakeController();
  EXPECT_EQ(controller->GetType(), ControllerType::DDGoController);
  EXPECT_FALSE(controller->GetAnalogInputBytes().has_value());

  // After a reset, power is off and the emergency brake is on.
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(0, EMERGENCY)));
  EXPECT_EQ(controller->GetBindState(VirtualPower(0)), 1.0f);
  EXPECT_EQ(controller->GetBindState(VirtualBrake(EMERGENCY)), 1.0f);

  // GetButtonStateBits() includes the level bits, but not the always pressed bits.
  EXPECT_EQ(controller->GetButtonStateBits(), (1u << 7) | (1u << 5));

  // Back-to-back polls don't need a reset, and the bytes after 42h are ignored.
  EXPECT_EQ(TransferCommand(*controller, {0x01, 0x42, 0xFF, 0x5A, 0x01}), PollReply(LevelState(0, EMERGENCY)));
}

TEST(DDGoController, RejectedBytes)
{
  const auto controller = MakeController();
  ExpectRejectedBytes(*controller, {0x00, 0x43, 0x44, 0x45, 0x4D}, 0x41, POLL, PollReply(LevelState(0, EMERGENCY)));
}

TEST(DDGoController, ResetTransferState)
{
  const auto controller = MakeController();
  PressBind(*controller, Bind::A);
  for (size_t len = 1; len < POLL.size(); len++)
  {
    TransferBytes(*controller, std::vector<u8>(POLL.begin(), POLL.begin() + len));
    controller->ResetTransferState();
    EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(0, EMERGENCY, {Bind::A}))) << len;
  }
}

TEST(DDGoController, Buttons)
{
  const auto controller = MakeController();
  for (const Bind button : {Bind::Select, Bind::Start, Bind::A, Bind::B, Bind::C})
  {
    const u32 index = static_cast<u32>(button);
    PressBind(*controller, button, 0.49f);
    EXPECT_EQ(controller->GetBindState(index), 0.0f) << index;

    PressBind(*controller, button, 0.5f);
    EXPECT_EQ(controller->GetBindState(index), 1.0f) << index;
    EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(0, EMERGENCY, {button}))) << index;

    PressBind(*controller, button, 0.0f);
    EXPECT_EQ(controller->GetBindState(index), 0.0f) << index;
  }

  // The level bits aren't reported as buttons.
  EXPECT_EQ(controller->GetBindState(static_cast<u32>(Bind::PowerBit1)), 0.0f);
  EXPECT_EQ(controller->GetBindState(static_cast<u32>(Bind::PowerBit2)), 0.0f);

  for (const char* name : {"Select", "Start", "A", "B", "C", "Power", "Brake"})
    EXPECT_NE(FindBinding(DDGoController::INFO, name), nullptr) << name;
  EXPECT_EQ(GetBindIndex(DDGoController::INFO, "Power"), static_cast<u32>(Bind::Power));
  EXPECT_EQ(GetBindIndex(DDGoController::INFO, "Brake"), static_cast<u32>(Bind::Brake));
  EXPECT_EQ(GetBindIndex(DDGoController::INFO, "VirtualPowerOff"), VirtualPower(0));
  EXPECT_EQ(GetBindIndex(DDGoController::INFO, "VirtualPower5"), VirtualPower(5));
  EXPECT_EQ(GetBindIndex(DDGoController::INFO, "VirtualBrakeReleased"), VirtualBrake(0));
  EXPECT_EQ(GetBindIndex(DDGoController::INFO, "VirtualBrake8"), VirtualBrake(8));
  EXPECT_EQ(GetBindIndex(DDGoController::INFO, "VirtualBrakeEmergency"), VirtualBrake(EMERGENCY));
  EXPECT_EQ(DDGoController::INFO.bindings.size(), 23u);
}

TEST(DDGoController, PowerLevels)
{
  const auto controller = MakeInstantController();
  for (u32 level = 0; level < std::size(POWER_LEVEL_BITS); level++)
  {
    controller->SetBindState(VirtualPower(level), 1.0f);
    EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(level, EMERGENCY))) << level;
    for (u32 i = 0; i < std::size(POWER_LEVEL_BITS); i++)
      EXPECT_EQ(controller->GetBindState(VirtualPower(i)), (i == level) ? 1.0f : 0.0f) << level << " " << i;

    // Only presses change the level.
    controller->SetBindState(VirtualPower(level), 0.0f);
    controller->SetBindState(VirtualPower(0), 0.49f);
    EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(level, EMERGENCY))) << level;
  }
}

TEST(DDGoController, BrakeLevels)
{
  const auto controller = MakeInstantController();
  for (u32 level = 0; level < std::size(BRAKE_LEVEL_BITS); level++)
  {
    controller->SetBindState(VirtualBrake(level), 1.0f);
    EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(0, level))) << level;
    for (u32 i = 0; i < std::size(BRAKE_LEVEL_BITS); i++)
      EXPECT_EQ(controller->GetBindState(VirtualBrake(i)), (i == level) ? 1.0f : 0.0f) << level << " " << i;

    controller->SetBindState(VirtualBrake(level), 0.0f);
    controller->SetBindState(VirtualBrake(0), 0.49f);
    EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(0, level))) << level;
  }
}

TEST(DDGoController, Axes)
{
  // With the default sensitivity of 1.33, the level is value * 1.33 * levels, rounded down.
  const auto controller = MakeInstantController();
  static constexpr struct
  {
    float value;
    u32 power;
    u32 brake;
  } cases[] = {
    {0.0f, 0, 0}, {0.08f, 0, 0}, {0.09f, 0, 1}, {0.15f, 0, 1}, {0.16f, 1, 1},
    {0.5f, 3, 5}, {0.6f, 3, 7},  {0.8f, 5, 9},  {1.0f, 5, 9},
  };
  for (const auto& c : cases)
  {
    PressBind(*controller, Bind::Power, c.value);
    PressBind(*controller, Bind::Brake, c.value);
    EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(c.power, c.brake))) << c.value;
    EXPECT_EQ(controller->GetBindState(static_cast<u32>(Bind::Power)), 0.0f);
    EXPECT_EQ(controller->GetBindState(static_cast<u32>(Bind::Brake)), 0.0f);
  }

  // The deadzone zeroes values below it, but doesn't rescale the rest.
  INISettingsInterface si;
  si.SetFloatValue(SECTION, "AnalogDeadzone", 0.5f);
  si.SetFloatValue(SECTION, "AnalogSensitivity", 1.0f);
  si.SetIntValue(SECTION, "PowerTransitionFrames", 0);
  si.SetIntValue(SECTION, "BrakeTransitionFrames", 0);
  controller->LoadSettings(si, SECTION, false);
  PressBind(*controller, Bind::Power, 0.45f);
  PressBind(*controller, Bind::Brake, 0.45f);
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(0, 0)));
  PressBind(*controller, Bind::Power, 0.5f);
  PressBind(*controller, Bind::Brake, 0.5f);
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(2, 4)));
}

TEST(DDGoController, TransitionFrames)
{
  INISettingsInterface si;
  si.SetIntValue(SECTION, "PowerTransitionFrames", 2);
  si.SetIntValue(SECTION, "BrakeTransitionFrames", 3);
  const auto controller = MakeController(&si);
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(0, EMERGENCY)));

  // The in-between state is reported straight away, for as many polls as the setting.
  controller->SetBindState(VirtualPower(1), 1.0f);
  controller->SetBindState(VirtualBrake(0), 1.0f);
  EXPECT_EQ(controller->GetBindState(VirtualPower(1)), 1.0f);
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(State({}, BRAKE_TRANSITION_BITS)));
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(State({}, BRAKE_TRANSITION_BITS)));
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(State(POWER_LEVEL_BITS[1], BRAKE_TRANSITION_BITS)));
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(1, 0)));

  // Only complete polls count.
  controller->SetBindState(VirtualPower(2), 1.0f);
  for (size_t len = 1; len < POLL.size(); len++)
  {
    TransferBytes(*controller, std::vector<u8>(POLL.begin(), POLL.begin() + len));
    controller->ResetTransferState();
  }
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(State({}, BRAKE_LEVEL_BITS[0])));
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(State({}, BRAKE_LEVEL_BITS[0])));
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(2, 0)));

  // A reset ends the transition.
  controller->SetBindState(VirtualBrake(3), 1.0f);
  controller->Reset();
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(2, 3)));
}

TEST(DDGoController, Defaults)
{
  // 10 transition frames by default.
  for (const SettingInfo& setting : DDGoController::INFO.settings)
  {
    if (std::string_view(setting.name) == "PowerTransitionFrames" ||
        std::string_view(setting.name) == "BrakeTransitionFrames")
    {
      EXPECT_STREQ(setting.default_value, "10") << setting.name;
    }
  }

  const auto loaded = MakeController();
  loaded->SetBindState(VirtualPower(1), 1.0f);
  for (u32 i = 0; i < 10; i++)
    EXPECT_EQ(TransferCommand(*loaded, POLL), PollReply(State({}, BRAKE_LEVEL_BITS[EMERGENCY]))) << i;
  EXPECT_EQ(TransferCommand(*loaded, POLL), PollReply(LevelState(1, EMERGENCY)));

  // Current behaviour: before Reset(), the power bits read as the in-between state.
  DDGoController controller(0);
  EXPECT_EQ(TransferCommand(controller, POLL), PollReply(State({}, BRAKE_LEVEL_BITS[EMERGENCY])));
  controller.Reset();
  EXPECT_EQ(TransferCommand(controller, POLL), PollReply(LevelState(0, EMERGENCY)));

  controller.SetBindState(VirtualPower(1), 1.0f);
  for (u32 i = 0; i < 10; i++)
    EXPECT_EQ(TransferCommand(controller, POLL), PollReply(State({}, BRAKE_LEVEL_BITS[EMERGENCY]))) << i;
  EXPECT_EQ(TransferCommand(controller, POLL), PollReply(LevelState(1, EMERGENCY)));
}

TEST(DDGoController, SaveState)
{
  const auto controller = MakeInstantController();
  PressBind(*controller, Bind::Start);
  controller->SetBindState(VirtualPower(3), 1.0f);
  controller->SetBindState(VirtualBrake(6), 1.0f);

  // Partway through a poll, the rest of it continues after loading.
  ExpectResumesAfterLoad(
    *controller, [] { return MakeInstantController(); }, POLL, PollReply(LevelState(3, 6, {Bind::Start})));

  // The levels and the transition countdown are input state, so only loaded if asked to.
  INISettingsInterface si;
  si.SetIntValue(SECTION, "PowerTransitionFrames", 3);
  si.SetIntValue(SECTION, "BrakeTransitionFrames", 0);
  const auto source = MakeController(&si);
  PressBind(*source, Bind::C);
  source->SetBindState(VirtualPower(4), 1.0f);
  source->SetBindState(VirtualBrake(2), 1.0f);
  const std::vector<u8> state = SaveState(*source);
  {
    const auto loaded = MakeInstantController();
    PressBind(*loaded, Bind::B);
    loaded->SetBindState(VirtualPower(5), 1.0f);
    LoadState(*loaded, state, false);
    EXPECT_EQ(TransferCommand(*loaded, POLL), PollReply(LevelState(5, EMERGENCY, {Bind::B})));
  }
  {
    const auto loaded = MakeInstantController();
    LoadState(*loaded, state, true);
    EXPECT_EQ(loaded->GetBindState(VirtualPower(4)), 1.0f);
    EXPECT_EQ(TransferCommand(*loaded, POLL), PollReply(State({}, BRAKE_LEVEL_BITS[2], {Bind::C})));
    EXPECT_EQ(TransferCommand(*loaded, POLL), PollReply(State({}, BRAKE_LEVEL_BITS[2], {Bind::C})));
    EXPECT_EQ(TransferCommand(*loaded, POLL), PollReply(State({}, BRAKE_LEVEL_BITS[2], {Bind::C})));
    EXPECT_EQ(TransferCommand(*loaded, POLL), PollReply(LevelState(4, 2, {Bind::C})));
  }
}

TEST(DDGoController, LegacyState)
{
  // Saved by version 104 partway through a poll, with C pressed, power 4 three frames into its transition, and
  // brake 2.
  static const std::vector<u8> state = {0xFF, 0xD5, 0x04, 0x03, 0x02, 0x00, 0x03};
  const auto controller = MakeInstantController();
  LoadState(*controller, state, true, 104);

  // The transfer starts again from idle, and the transition continues.
  for (u32 i = 0; i < 3; i++)
    EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(State({}, BRAKE_LEVEL_BITS[2], {Bind::C}))) << i;
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(LevelState(4, 2, {Bind::C})));
}
