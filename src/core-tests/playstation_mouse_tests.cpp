// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller_test_utils.h"

#include "core/playstation_mouse.h"

#include "util/ini_settings_interface.h"

#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

// These tests check what is currently emulated. Comments starting with "Current behaviour:" mark things that look
// questionable, and may change.

using namespace ControllerTestUtils;

namespace {

using Binding = PlayStationMouse::Binding;

const std::vector<u8> MOUSE_POLL = {0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00};

static constexpr u16 LEFT_BIT = 1u << 11;
static constexpr u16 RIGHT_BIT = 1u << 10;

/// Bits 8-9 always read as 0.
static std::vector<u8> MouseReply(u16 buttons, s8 dx = 0, s8 dy = 0)
{
  return DeviceReply(0x12, buttons & 0xFCFF, {static_cast<u8>(dx), static_cast<u8>(dy)});
}

static void Move(Controller& controller, float dx, float dy)
{
  controller.SetBindState(static_cast<u32>(Binding::PointerX), dx);
  controller.SetBindState(static_cast<u32>(Binding::PointerY), dy);
}

static std::unique_ptr<PlayStationMouse> MakeMouse()
{
  auto mouse = std::make_unique<PlayStationMouse>(0);
  LoadDefaultSettings(*mouse);
  return mouse;
}

} // namespace

TEST(PlayStationMouse, Poll)
{
  PlayStationMouse mouse(0);
  LoadDefaultSettings(mouse);
  EXPECT_EQ(mouse.GetType(), ControllerType::PlayStationMouse);
  EXPECT_FALSE(mouse.GetAnalogInputBytes().has_value());

  // Unused button bits read as 1, except bits 8-9 which read as 0.
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), (std::vector<u8>{0xFF, 0x12, 0x5A, 0xFF, 0xFC, 0x00, 0x00}));

  // Back-to-back polls don't need a reset, and the bytes after 42h are ignored.
  EXPECT_EQ(TransferCommand(mouse, {0x01, 0x42, 0xFF, 0x5A, 0x01, 0x02, 0x03}), MouseReply(NO_BUTTONS));
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS));
}

TEST(PlayStationMouse, RejectedBytes)
{
  PlayStationMouse mouse(0);
  LoadDefaultSettings(mouse);
  ExpectRejectedBytes(mouse, {0x00, 0x43, 0x44, 0x45, 0x4D}, 0x12, MOUSE_POLL, MouseReply(NO_BUTTONS));
}

TEST(PlayStationMouse, ResetTransferState)
{
  PlayStationMouse mouse(0);
  LoadDefaultSettings(mouse);
  mouse.SetBindState(static_cast<u32>(Binding::Left), 1.0f);

  for (u32 i = 0; i < 2; i++)
  {
    for (size_t len = 1; len < MOUSE_POLL.size(); len++)
    {
      TransferBytes(mouse, std::vector<u8>(MOUSE_POLL.begin(), MOUSE_POLL.begin() + len));
      if (i == 0)
        mouse.ResetTransferState();
      else
        mouse.Reset();

      EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(static_cast<u16>(~LEFT_BIT)))
        << (i ? "Reset" : "ResetTransferState") << " after " << len;
    }
  }
}

TEST(PlayStationMouse, Buttons)
{
  PlayStationMouse mouse(0);
  LoadDefaultSettings(mouse);

  static constexpr std::pair<Binding, u16> buttons[] = {{Binding::Left, LEFT_BIT}, {Binding::Right, RIGHT_BIT}};
  for (const auto& [binding, bit] : buttons)
  {
    const u32 index = static_cast<u32>(binding);
    mouse.SetBindState(index, 0.49f);
    EXPECT_EQ(mouse.GetBindState(index), 0.0f);
    EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS));

    mouse.SetBindState(index, 0.5f);
    EXPECT_EQ(mouse.GetBindState(index), 1.0f);
    EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(static_cast<u16>(~bit)));

    EXPECT_EQ(mouse.GetButtonStateBits(), bit);

    mouse.SetBindState(index, 0.0f);
    EXPECT_EQ(mouse.GetBindState(index), 0.0f);
    EXPECT_EQ(mouse.GetButtonStateBits(), 0u);
  }

  // The pointer bindings don't read back.
  Move(mouse, 1.0f, 1.0f);
  EXPECT_EQ(mouse.GetBindState(static_cast<u32>(Binding::PointerX)), 0.0f);
  EXPECT_EQ(mouse.GetBindState(static_cast<u32>(Binding::PointerY)), 0.0f);

  EXPECT_EQ(GetBindIndex(PlayStationMouse::INFO, "Left"), static_cast<u32>(Binding::Left));
  EXPECT_EQ(GetBindIndex(PlayStationMouse::INFO, "Right"), static_cast<u32>(Binding::Right));
  EXPECT_EQ(GetBindIndex(PlayStationMouse::INFO, "Pointer"), static_cast<u32>(Binding::PointerX));
  EXPECT_EQ(PlayStationMouse::INFO.bindings.size(), 3u);
  EXPECT_TRUE(HasSetting(PlayStationMouse::INFO, "SensitivityX"));
  EXPECT_TRUE(HasSetting(PlayStationMouse::INFO, "SensitivityY"));
}

TEST(PlayStationMouse, Deltas)
{
  PlayStationMouse mouse(0);
  LoadDefaultSettings(mouse);

  // Movement accumulates until it's polled, and the fraction carries over.
  Move(mouse, 3.5f, -2.5f);
  Move(mouse, 1.0f, 0.0f);
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS, 4, -3));
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS, 0, 0));
  Move(mouse, 0.5f, 0.5f);
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS, 1, 1));
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS, 0, 0));

  // Clamped to a signed byte. Current behaviour: the excess is dropped, not carried over.
  Move(mouse, 300.0f, -300.0f);
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS, 127, -128));
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS, 0, 0));

  // The movement is consumed by 42h, so it's lost if the poll is aborted.
  Move(mouse, 2.0f, 3.0f);
  TransferBytes(mouse, {0x01, 0x42});
  mouse.ResetTransferState();
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS, 0, 0));
  Move(mouse, 2.0f, 3.0f);
  TransferBytes(mouse, {0x01});
  mouse.ResetTransferState();
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS, 2, 3));
}

TEST(PlayStationMouse, Sensitivity)
{
  PlayStationMouse mouse(0);
  INISettingsInterface si;
  si.SetFloatValue("Pad1", "SensitivityX", 2.0f);
  si.SetFloatValue("Pad1", "SensitivityY", 0.5f);
  mouse.LoadSettings(si, "Pad1", true);

  // The remainder is kept in unscaled units.
  Move(mouse, 3.25f, 5.0f);
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS, 6, 2));
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS, 0, 0));
  Move(mouse, 0.25f, 1.0f);
  EXPECT_EQ(TransferCommand(mouse, MOUSE_POLL), MouseReply(NO_BUTTONS, 1, 1));
}

TEST(PlayStationMouse, SaveState)
{
  PlayStationMouse mouse(0);
  LoadDefaultSettings(mouse);
  mouse.SetBindState(static_cast<u32>(Binding::Right), 1.0f);

  // Partway through a poll, the rest of it continues after loading.
  ExpectResumesAfterLoad(mouse, MakeMouse, MOUSE_POLL, MouseReply(static_cast<u16>(~RIGHT_BIT)));

  // The movement is kept, including the fraction.
  Move(mouse, 5.5f, -1.5f);
  std::vector<u8> state = SaveState(mouse);
  {
    const auto loaded = MakeMouse();
    LoadState(*loaded, state, true);
    EXPECT_EQ(TransferCommand(*loaded, MOUSE_POLL), MouseReply(static_cast<u16>(~RIGHT_BIT), 5, -2));
    Move(*loaded, 0.5f, 0.5f);
    EXPECT_EQ(TransferCommand(*loaded, MOUSE_POLL), MouseReply(static_cast<u16>(~RIGHT_BIT), 1, 1));
  }

  // Input state is only loaded if asked to.
  {
    const auto loaded = MakeMouse();
    loaded->SetBindState(static_cast<u32>(Binding::Left), 1.0f);
    Move(*loaded, 1.0f, 2.0f);
    LoadState(*loaded, state, false);
    EXPECT_EQ(TransferCommand(*loaded, MOUSE_POLL), MouseReply(static_cast<u16>(~LEFT_BIT), 1, 2));
  }
}

TEST(PlayStationMouse, LegacyState)
{
  // Saved by version 104 partway through a poll, with Right pressed and a movement of 5.5,-1.5.
  static const std::vector<u8> state = {0xFF, 0xFB, 0x00, 0x00, 0xB0, 0x40, 0x00, 0x00, 0xC0, 0xBF, 0x03};
  const auto mouse = MakeMouse();
  LoadState(*mouse, state, true, 104);

  // The transfer starts again from idle.
  EXPECT_EQ(TransferCommand(*mouse, MOUSE_POLL), MouseReply(static_cast<u16>(~RIGHT_BIT), 5, -2));
  Move(*mouse, 0.5f, 0.5f);
  EXPECT_EQ(TransferCommand(*mouse, MOUSE_POLL), MouseReply(static_cast<u16>(~RIGHT_BIT), 1, 1));
}
