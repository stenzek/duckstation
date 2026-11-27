// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller_test_utils.h"

#include "core/digital_controller.h"

#include "util/ini_settings_interface.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <memory>
#include <vector>

// These tests check what is currently emulated. Comments starting with "Current behaviour:" mark things that look
// questionable, and may change.

using namespace ControllerTestUtils;

namespace {

using Button = DigitalController::Button;

static constexpr u16 ButtonBit(Button button)
{
  return static_cast<u16>(1u << static_cast<u8>(button));
}

static u16 Pressed(std::initializer_list<Button> buttons)
{
  u16 ret = NO_BUTTONS;
  for (const Button button : buttons)
    ret &= ~ButtonBit(button);
  return ret;
}

static void Press(Controller& controller, Button button, float value = 1.0f)
{
  controller.SetBindState(static_cast<u32>(button), value);
}

} // namespace

TEST(DigitalController, Poll)
{
  DigitalController controller(0, 0xFFFF);
  LoadDefaultSettings(controller);
  EXPECT_EQ(controller.GetType(), ControllerType::DigitalController);
  EXPECT_FALSE(controller.GetAnalogInputBytes().has_value());

  EXPECT_EQ(TransferCommand(controller, POLL), PollReply(NO_BUTTONS));

  // Back-to-back polls don't need a reset, and the bytes after 42h are ignored.
  Press(controller, Button::Cross);
  Press(controller, Button::Start);
  EXPECT_EQ(TransferCommand(controller, POLL), PollReply(Pressed({Button::Cross, Button::Start})));
  EXPECT_EQ(TransferCommand(controller, {0x01, 0x42, 0xFF, 0x5A, 0x01}),
            PollReply(Pressed({Button::Cross, Button::Start})));
}

TEST(DigitalController, RejectedBytes)
{
  DigitalController controller(0, 0xFFFF);
  LoadDefaultSettings(controller);

  for (const u8 first : {0x00, 0x02, 0x42, 0x81, 0xFF})
  {
    EXPECT_EQ(TransferCommand(controller, {first}), std::vector<u8>{0xFF}) << "first byte 0x" << std::hex << u32(first);
    EXPECT_EQ(TransferCommand(controller, POLL), PollReply(NO_BUTTONS));
  }

  for (const u8 command : {0x00, 0x43, 0x44, 0x45, 0x4D, 0x53})
  {
    EXPECT_EQ(TransferCommand(controller, {0x01, command}), (std::vector<u8>{0xFF, 0xFF}))
      << "command 0x" << std::hex << u32(command);
    controller.ResetTransferState();
    EXPECT_EQ(TransferCommand(controller, POLL), PollReply(NO_BUTTONS));
  }

  // Current behaviour: a rejected command leaves the pad waiting for a command, until the transfer state is reset.
  EXPECT_EQ(TransferCommand(controller, {0x01, 0x43}), (std::vector<u8>{0xFF, 0xFF}));
  EXPECT_EQ(TransferCommand(controller, {0x42, 0x00, 0x00, 0x00}), (std::vector<u8>{0x41, 0x5A, 0xFF, 0xFF}));
  EXPECT_EQ(TransferCommand(controller, {0x01, 0x43}), (std::vector<u8>{0xFF, 0xFF}));
  controller.ResetTransferState();
  EXPECT_EQ(TransferCommand(controller, {0x42}), std::vector<u8>{0xFF});
}

TEST(DigitalController, ResetTransferState)
{
  DigitalController controller(0, 0xFFFF);
  LoadDefaultSettings(controller);
  Press(controller, Button::Triangle);

  // Partway through a poll, either reset starts a new command, and doesn't change the buttons.
  for (u32 i = 0; i < 2; i++)
  {
    for (size_t len = 1; len < POLL.size(); len++)
    {
      std::vector<bool> acks;
      TransferBytes(controller, std::vector<u8>(POLL.begin(), POLL.begin() + len), &acks);
      EXPECT_EQ(acks, std::vector<bool>(len, true));

      if (i == 0)
        controller.ResetTransferState();
      else
        controller.Reset();

      EXPECT_EQ(TransferCommand(controller, POLL), PollReply(Pressed({Button::Triangle})))
        << (i ? "Reset" : "ResetTransferState") << " after " << len;
    }
  }
}

TEST(DigitalController, Buttons)
{
  DigitalController controller(0, 0xFFFF);
  LoadDefaultSettings(controller);

  for (u32 i = 0; i < static_cast<u32>(Button::Count); i++)
  {
    const Button button = static_cast<Button>(i);
    EXPECT_EQ(controller.GetBindState(i), 0.0f);

    Press(controller, button, 0.49f);
    EXPECT_EQ(controller.GetBindState(i), 0.0f) << i;
    EXPECT_EQ(controller.GetButtonStateBits(), 0u);

    Press(controller, button, 0.5f);
    EXPECT_EQ(controller.GetBindState(i), 1.0f) << i;
    EXPECT_EQ(controller.GetButtonStateBits(), ButtonBit(button));
    EXPECT_EQ(TransferCommand(controller, POLL), PollReply(Pressed({button}))) << i;

    Press(controller, button, 0.49f);
    EXPECT_EQ(controller.GetBindState(i), 0.0f) << i;
    EXPECT_EQ(controller.GetButtonStateBits(), 0u);
  }

  // Indices past the buttons are ignored.
  controller.SetBindState(static_cast<u32>(Button::Count), 1.0f);
  EXPECT_EQ(controller.GetBindState(static_cast<u32>(Button::Count)), 0.0f);
  EXPECT_EQ(controller.GetButtonStateBits(), 0u);
  EXPECT_EQ(TransferCommand(controller, POLL), PollReply(NO_BUTTONS));

  // Several at once.
  Press(controller, Button::Select);
  Press(controller, Button::Up);
  Press(controller, Button::L2);
  Press(controller, Button::Square);
  EXPECT_EQ(controller.GetButtonStateBits(), 0x8111u);
  EXPECT_EQ(TransferCommand(controller, POLL),
            PollReply(Pressed({Button::Select, Button::Up, Button::L2, Button::Square})));
}

TEST(DigitalController, BindingIndices)
{
  for (u32 i = 0; i < static_cast<u32>(Button::Count); i++)
  {
    static constexpr const char* names[] = {"Select",   nullptr,  nullptr, "Start", "Up", "Right",
                                            "Down",     "Left",   "L2",    "R2",    "L1", "R1",
                                            "Triangle", "Circle", "Cross", "Square"};
    if (names[i])
      EXPECT_EQ(GetBindIndex(DigitalController::INFO, names[i]), i) << names[i];
  }

  // There are no bindings for L3/R3.
  EXPECT_EQ(DigitalController::INFO.bindings.size(), 14u);
  EXPECT_TRUE(HasSetting(DigitalController::INFO, "DisableSOCD"));
}

TEST(DigitalController, DisableSOCD)
{
  DigitalController controller(0, 0xFFFF);
  LoadDefaultSettings(controller);
  Press(controller, Button::Up);
  Press(controller, Button::Down);
  Press(controller, Button::Left);
  Press(controller, Button::Right);
  Press(controller, Button::Cross);

  // Off by default.
  const u16 all = Pressed({Button::Up, Button::Down, Button::Left, Button::Right, Button::Cross});
  EXPECT_EQ(TransferCommand(controller, POLL), PollReply(all));

  // Up is kept over Down, and Right over Left. The bind state isn't affected.
  INISettingsInterface si;
  si.SetBoolValue("Pad1", "DisableSOCD", true);
  controller.LoadSettings(si, "Pad1", false);
  EXPECT_EQ(TransferCommand(controller, POLL), PollReply(Pressed({Button::Up, Button::Right, Button::Cross})));
  EXPECT_EQ(controller.GetButtonStateBits(), static_cast<u16>(~all));

  // Single directions pass through.
  Press(controller, Button::Up, 0.0f);
  Press(controller, Button::Right, 0.0f);
  EXPECT_EQ(TransferCommand(controller, POLL), PollReply(Pressed({Button::Down, Button::Left, Button::Cross})));
}

TEST(DigitalController, Popn)
{
  const std::unique_ptr<DigitalController> controller = DigitalController::Create(0, ControllerType::PopnController);
  LoadDefaultSettings(*controller);

  // Current behaviour: the type is still reported as a digital controller.
  EXPECT_EQ(controller->GetType(), ControllerType::DigitalController);

  // Right, Down and Left are grounded, so always read as pressed.
  const u16 grounded = Pressed({Button::Right, Button::Down, Button::Left});
  EXPECT_EQ(TransferCommand(*controller, POLL), PollReply(grounded));

  // The other buttons pass through, and the bind state isn't masked.
  Press(*controller, Button::Up);
  Press(*controller, Button::L1);
  Press(*controller, Button::Triangle);
  Press(*controller, Button::Right);
  EXPECT_EQ(TransferCommand(*controller, POLL),
            PollReply(grounded & Pressed({Button::Up, Button::L1, Button::Triangle})));
  EXPECT_EQ(controller->GetButtonStateBits(), static_cast<u16>(ButtonBit(Button::Up) | ButtonBit(Button::L1) |
                                                               ButtonBit(Button::Triangle) | ButtonBit(Button::Right)));

  // Bindings are the nine buttons plus Select/Start.
  EXPECT_EQ(DigitalController::INFO_POPN.type, ControllerType::PopnController);
  EXPECT_EQ(DigitalController::INFO_POPN.bindings.size(), 11u);
  EXPECT_EQ(GetBindIndex(DigitalController::INFO_POPN, "LeftWhite"), static_cast<u32>(Button::Triangle));
  EXPECT_EQ(GetBindIndex(DigitalController::INFO_POPN, "MiddleRed"), static_cast<u32>(Button::L1));
  EXPECT_EQ(GetBindIndex(DigitalController::INFO_POPN, "RightYellow"), static_cast<u32>(Button::Up));
  EXPECT_TRUE(DigitalController::INFO_POPN.settings.empty());

  // A plain digital controller isn't masked.
  const std::unique_ptr<DigitalController> digital = DigitalController::Create(0, ControllerType::DigitalController);
  Press(*digital, Button::Right);
  EXPECT_EQ(TransferCommand(*digital, POLL), PollReply(Pressed({Button::Right})));
}

TEST(DigitalController, SaveState)
{
  DigitalController controller(0, 0xFFFF);
  LoadDefaultSettings(controller);
  Press(controller, Button::Circle);

  std::vector<u8> state;

  // Partway through a poll, the rest of it continues after loading.
  const std::vector<u8> reply = PollReply(Pressed({Button::Circle}));
  for (size_t len = 1; len < POLL.size(); len++)
  {
    TransferBytes(controller, std::vector<u8>(POLL.begin(), POLL.begin() + len));
    state = SaveState(controller);
    controller.ResetTransferState();

    DigitalController loaded(0, 0xFFFF);
    LoadDefaultSettings(loaded);
    LoadState(loaded, state, true);
    EXPECT_EQ(TransferCommand(loaded, std::vector<u8>(POLL.begin() + len, POLL.end())),
              std::vector<u8>(reply.begin() + len, reply.end()))
      << "after " << len;
  }

  // Input state is only loaded if asked to.
  state = SaveState(controller);
  DigitalController loaded(0, 0xFFFF);
  LoadDefaultSettings(loaded);
  Press(loaded, Button::Square);
  LoadState(loaded, state, false);
  EXPECT_EQ(TransferCommand(loaded, POLL), PollReply(Pressed({Button::Square})));
  LoadState(loaded, state, true);
  EXPECT_EQ(TransferCommand(loaded, POLL), PollReply(Pressed({Button::Circle})));
}
