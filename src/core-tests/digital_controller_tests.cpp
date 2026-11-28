// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller_test_utils.h"

#include "core/digital_controller.h"

#include "util/ini_settings_interface.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

// These tests check what is currently emulated. Comments starting with "Current behaviour:" mark things that look
// questionable, and may change.

using namespace ControllerTestUtils;

namespace {

using Button = DigitalController::Button;

static void Press(Controller& controller, Button button, float value = 1.0f)
{
  controller.SetBindState(static_cast<u32>(button), value);
}

} // namespace

TEST(DigitalController, Poll)
{
  DigitalController controller(0, ControllerType::DigitalController, 0xFFFF);
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
  DigitalController controller(0, ControllerType::DigitalController, 0xFFFF);
  LoadDefaultSettings(controller);

  // The ID byte is still sent for other commands, but not ACKed.
  ExpectRejectedBytes(controller, {0x00, 0x43, 0x44, 0x45, 0x4D, 0x53}, 0x41, POLL, PollReply(NO_BUTTONS));

  // Current behaviour: a rejected command leaves the pad waiting for a command, until the transfer state is reset.
  EXPECT_EQ(TransferCommand(controller, {0x01, 0x43}), (std::vector<u8>{0xFF, 0x41}));
  EXPECT_EQ(TransferCommand(controller, {0x42, 0x00, 0x00, 0x00}), (std::vector<u8>{0x41, 0x5A, 0xFF, 0xFF}));
  EXPECT_EQ(TransferCommand(controller, {0x01, 0x43}), (std::vector<u8>{0xFF, 0x41}));
  controller.ResetTransferState();
  EXPECT_EQ(TransferCommand(controller, {0x42}), std::vector<u8>{0xFF});
}

TEST(DigitalController, ResetTransferState)
{
  DigitalController controller(0, ControllerType::DigitalController, 0xFFFF);
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
  DigitalController controller(0, ControllerType::DigitalController, 0xFFFF);
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
  DigitalController controller(0, ControllerType::DigitalController, 0xFFFF);
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

  EXPECT_EQ(controller->GetType(), ControllerType::PopnController);

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
  DigitalController controller(0, ControllerType::DigitalController, 0xFFFF);
  LoadDefaultSettings(controller);
  Press(controller, Button::Circle);

  // Partway through a poll, the rest of it continues after loading.
  ExpectResumesAfterLoad(
    controller,
    [] {
      auto loaded = std::make_unique<DigitalController>(0, ControllerType::DigitalController, 0xFFFF);
      LoadDefaultSettings(*loaded);
      return loaded;
    },
    POLL, PollReply(Pressed({Button::Circle})));

  // Input state is only loaded if asked to.
  const std::vector<u8> state = SaveState(controller);
  DigitalController loaded(0, ControllerType::DigitalController, 0xFFFF);
  LoadDefaultSettings(loaded);
  Press(loaded, Button::Square);
  LoadState(loaded, state, false);
  EXPECT_EQ(TransferCommand(loaded, POLL), PollReply(Pressed({Button::Square})));
  LoadState(loaded, state, true);
  EXPECT_EQ(TransferCommand(loaded, POLL), PollReply(Pressed({Button::Circle})));

  // A transfer step past the end of the reply restarts the transfer from idle.
  TransferBytes(controller, {0x01, 0x42});
  std::vector<u8> bad_state = SaveState(controller);
  bad_state[1] = 0xFF;
  LoadState(loaded, bad_state, true);
  EXPECT_EQ(TransferCommand(loaded, POLL), PollReply(Pressed({Button::Circle})));
}

TEST(DigitalController, LegacyState)
{
  // Saved by version 104 partway through a poll, with Circle pressed.
  static const std::vector<u8> state = {0xFF, 0xDF, 0x03};
  DigitalController controller(0, ControllerType::DigitalController, 0xFFFF);
  LoadDefaultSettings(controller);
  LoadState(controller, state, true, 104);

  // The transfer starts again from idle.
  EXPECT_EQ(TransferCommand(controller, POLL), PollReply(Pressed({Button::Circle})));
}
