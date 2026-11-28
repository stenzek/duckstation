// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller_test_utils.h"

#include "core/guncon.h"
#include "core/justifier.h"
#include "core/lightgun_controller.h"

#include "util/ini_settings_interface.h"

#include <gtest/gtest.h>
#include <memory>
#include <string_view>
#include <vector>

// These tests check what is currently emulated. Comments starting with "Current behaviour:" mark things that look
// questionable, and may change. In core-tests the render window is surfaceless, so the pointer is always off-screen.

using namespace ControllerTestUtils;

namespace {

class TestLightgunController final : public LightgunController
{
public:
  TestLightgunController() : LightgunController(0, ControllerType::None, {{2, 4, 6, 0}}) {}

  void Reset() override {}
  u8 GetIDByte() const override { return 0x41; }
  bool BeginCommand(u8) override { return false; }

  bool GetLastShootOffscreenState() const { return m_last_shoot_offscreen_state; }
  u32 GetShootOffscreenCallCount() const { return m_shoot_offscreen_call_count; }

private:
  void SetShootOffscreen(bool pressed) override
  {
    m_last_shoot_offscreen_state = pressed;
    m_shoot_offscreen_call_count++;
  }

  bool m_last_shoot_offscreen_state = false;
  u32 m_shoot_offscreen_call_count = 0;
};

static constexpr u16 GUNCON_TRIGGER_BIT = 1u << 13;
static constexpr u16 GUNCON_A_BIT = 1u << 3;
static constexpr u16 GUNCON_B_BIT = 1u << 14;

static constexpr u16 JUSTIFIER_TRIGGER_BIT = 1u << 15;
static constexpr u16 JUSTIFIER_START_BIT = 1u << 3;
static constexpr u16 JUSTIFIER_BACK_BIT = 1u << 14;

/// GunCon poll reply, with the off-screen position (X=0001h, Y=000Ah).
static std::vector<u8> GunConReply(u16 buttons)
{
  return DeviceReply(0x63, buttons, {0x01, 0x00, 0x0A, 0x00});
}

static std::vector<u8> JustifierReply(u16 buttons)
{
  return DeviceReply(0x31, buttons);
}

static u16 Low(u16 bits)
{
  return static_cast<u16>(~bits);
}

static std::unique_ptr<GunCon> MakeGunCon()
{
  auto guncon = std::make_unique<GunCon>(0);
  LoadDefaultSettings(*guncon);
  return guncon;
}

static std::unique_ptr<Justifier> MakeJustifier()
{
  auto justifier = std::make_unique<Justifier>(0);
  LoadDefaultSettings(*justifier);
  return justifier;
}

static void PressBinding(Controller& controller, u32 index, float value = 1.0f)
{
  controller.SetBindState(index, value);
}

static bool IsEventActive(std::string_view name)
{
  for (TimingEvent* event = *TimingEvents::GetHeadEventPtr(); event; event = event->next)
  {
    if (name == event->GetName())
      return true;
  }
  return false;
}

/// Integer settings that can be negative must not be shown with %u.
static void ExpectSignedIntegerFormats(const Controller::ControllerInfo& info)
{
  for (const SettingInfo& si : info.settings)
  {
    if (si.type == SettingInfo::Type::Integer && si.IntegerMinValue() < 0)
      EXPECT_EQ(std::string_view(si.format), "%d") << si.name;
  }
}

} // namespace

TEST(LightgunController, ButtonBindings)
{
  TestLightgunController controller;

  EXPECT_EQ(controller.GetButtonStateBits(), 0u);
  EXPECT_EQ(controller.GetBindState(0), 0.0f);

  controller.SetBindState(0, 0.49f);
  EXPECT_EQ(controller.GetBindState(0), 0.0f);
  EXPECT_EQ(controller.GetButtonStateBits(), 0u);

  controller.SetBindState(0, 0.5f);
  EXPECT_EQ(controller.GetBindState(0), 1.0f);
  EXPECT_EQ(controller.GetButtonStateBits(), UINT16_C(0x0004));

  controller.SetBindState(0, 0.0f);
  EXPECT_EQ(controller.GetBindState(0), 0.0f);
  EXPECT_EQ(controller.GetButtonStateBits(), 0u);

  controller.SetBindState(8, 1.0f);
  EXPECT_EQ(controller.GetButtonStateBits(), 0u);
}

TEST(LightgunController, ShootOffscreenDispatch)
{
  TestLightgunController controller;

  controller.SetBindState(3, 1.0f);
  EXPECT_TRUE(controller.GetLastShootOffscreenState());
  EXPECT_EQ(controller.GetShootOffscreenCallCount(), 1u);
  EXPECT_EQ(controller.GetBindState(3), 0.0f);
  EXPECT_EQ(controller.GetButtonStateBits(), 0u);

  controller.SetBindState(3, 0.0f);
  EXPECT_FALSE(controller.GetLastShootOffscreenState());
  EXPECT_EQ(controller.GetShootOffscreenCallCount(), 2u);
  EXPECT_EQ(controller.GetButtonStateBits(), 0u);
}

TEST(LightgunController, DeviceButtonMappings)
{
  GunCon guncon(0);
  guncon.SetBindState(static_cast<u32>(GunCon::Binding::Trigger), 1.0f);
  guncon.SetBindState(static_cast<u32>(GunCon::Binding::A), 1.0f);
  guncon.SetBindState(static_cast<u32>(GunCon::Binding::B), 1.0f);
  EXPECT_EQ(guncon.GetButtonStateBits(), UINT16_C(0x6008));

  Justifier justifier(0);
  justifier.SetBindState(static_cast<u32>(Justifier::Binding::Trigger), 1.0f);
  justifier.SetBindState(static_cast<u32>(Justifier::Binding::Start), 1.0f);
  justifier.SetBindState(static_cast<u32>(Justifier::Binding::Back), 1.0f);
  EXPECT_EQ(justifier.GetButtonStateBits(), UINT16_C(0xC008));
}

TEST(LightgunController, StateApplication)
{
  TestLightgunController controller;
  controller.SetBindState(1, 1.0f);

  const std::vector<u8> state = SaveState(controller);

  controller.SetBindState(1, 0.0f);
  LoadState(controller, state, false);
  EXPECT_EQ(controller.GetBindState(1), 0.0f);

  LoadState(controller, state, true);
  EXPECT_EQ(controller.GetBindState(1), 1.0f);
}

TEST(LightgunController, GunConOffscreenTrigger)
{
  GunCon controller(0);

  controller.SetBindState(static_cast<u32>(GunCon::Binding::ShootOffscreen), 1.0f);
  EXPECT_EQ(controller.GetBindState(static_cast<u32>(GunCon::Binding::Trigger)), 1.0f);

  controller.SetBindState(static_cast<u32>(GunCon::Binding::ShootOffscreen), 0.0f);
  EXPECT_EQ(controller.GetBindState(static_cast<u32>(GunCon::Binding::Trigger)), 0.0f);
}

TEST(GunCon, Poll)
{
  const auto guncon = MakeGunCon();
  EXPECT_EQ(guncon->GetType(), ControllerType::GunCon);
  EXPECT_FALSE(guncon->GetAnalogInputBytes().has_value());

  // Off-screen, the position is always X=0001h, Y=000Ah.
  EXPECT_EQ(TransferCommand(*guncon, LONG_POLL), GunConReply(NO_BUTTONS));

  // Back-to-back polls don't need a reset, and the bytes after 42h are ignored.
  PressBinding(*guncon, static_cast<u32>(GunCon::Binding::A));
  EXPECT_EQ(TransferCommand(*guncon, {0x01, 0x42, 0xFF, 0x5A, 0x01, 0x02, 0x03, 0x04, 0x05}),
            GunConReply(Low(GUNCON_A_BIT)));
  EXPECT_EQ(TransferCommand(*guncon, LONG_POLL), GunConReply(Low(GUNCON_A_BIT)));
}

TEST(GunCon, RejectedBytes)
{
  const auto guncon = MakeGunCon();
  ExpectRejectedBytes(*guncon, {0x00, 0x43, 0x44, 0x45, 0x4D}, 0x63, LONG_POLL, GunConReply(NO_BUTTONS));
}

TEST(GunCon, ResetTransferState)
{
  const auto guncon = MakeGunCon();
  PressBinding(*guncon, static_cast<u32>(GunCon::Binding::B));
  for (u32 i = 0; i < 2; i++)
  {
    for (size_t len = 1; len < LONG_POLL.size(); len++)
    {
      TransferBytes(*guncon, std::vector<u8>(LONG_POLL.begin(), LONG_POLL.begin() + len));
      if (i == 0)
        guncon->ResetTransferState();
      else
        guncon->Reset();

      EXPECT_EQ(TransferCommand(*guncon, LONG_POLL), GunConReply(Low(GUNCON_B_BIT)))
        << (i ? "Reset" : "ResetTransferState") << " after " << len;
    }
  }
}

TEST(GunCon, Buttons)
{
  const auto guncon = MakeGunCon();
  static constexpr std::pair<GunCon::Binding, u16> buttons[] = {{GunCon::Binding::Trigger, GUNCON_TRIGGER_BIT},
                                                                {GunCon::Binding::A, GUNCON_A_BIT},
                                                                {GunCon::Binding::B, GUNCON_B_BIT}};
  for (const auto& [binding, bit] : buttons)
  {
    PressBinding(*guncon, static_cast<u32>(binding));
    EXPECT_EQ(TransferCommand(*guncon, LONG_POLL), GunConReply(Low(bit))) << static_cast<u32>(binding);
    PressBinding(*guncon, static_cast<u32>(binding), 0.0f);
  }

  // Shoot Offscreen pulls the trigger.
  PressBinding(*guncon, static_cast<u32>(GunCon::Binding::ShootOffscreen));
  EXPECT_EQ(TransferCommand(*guncon, LONG_POLL), GunConReply(Low(GUNCON_TRIGGER_BIT)));
  PressBinding(*guncon, static_cast<u32>(GunCon::Binding::ShootOffscreen), 0.0f);
  EXPECT_EQ(TransferCommand(*guncon, LONG_POLL), GunConReply(NO_BUTTONS));

  // Current behaviour: releasing Shoot Offscreen also releases a held trigger.
  PressBinding(*guncon, static_cast<u32>(GunCon::Binding::Trigger));
  PressBinding(*guncon, static_cast<u32>(GunCon::Binding::ShootOffscreen));
  PressBinding(*guncon, static_cast<u32>(GunCon::Binding::ShootOffscreen), 0.0f);
  EXPECT_EQ(TransferCommand(*guncon, LONG_POLL), GunConReply(NO_BUTTONS));

  EXPECT_EQ(GetBindIndex(GunCon::INFO, "Trigger"), static_cast<u32>(GunCon::Binding::Trigger));
  EXPECT_EQ(GetBindIndex(GunCon::INFO, "ShootOffscreen"), static_cast<u32>(GunCon::Binding::ShootOffscreen));
  EXPECT_EQ(GetBindIndex(GunCon::INFO, "A"), static_cast<u32>(GunCon::Binding::A));
  EXPECT_EQ(GetBindIndex(GunCon::INFO, "B"), static_cast<u32>(GunCon::Binding::B));
  EXPECT_EQ(GetBindIndex(GunCon::INFO, "Pointer"), static_cast<u32>(GunCon::Binding::ButtonCount));
  EXPECT_EQ(GetBindIndex(GunCon::INFO, "RelativeLeft"), static_cast<u32>(GunCon::Binding::RelativeLeft));
  EXPECT_EQ(GetBindIndex(GunCon::INFO, "RelativeDown"), static_cast<u32>(GunCon::Binding::RelativeDown));
  EXPECT_EQ(GunCon::INFO.bindings.size(), 9u);
  for (const char* setting :
       {"CrosshairImagePath", "CrosshairScale", "CrosshairColor", "XScale", "GunConLineOffset", "GunConTickOffset"})
  {
    EXPECT_TRUE(HasSetting(GunCon::INFO, setting)) << setting;
  }
}

TEST(GunCon, SettingFormats)
{
  ExpectSignedIntegerFormats(GunCon::INFO);
}

TEST(GunCon, SaveState)
{
  const auto guncon = MakeGunCon();
  PressBinding(*guncon, static_cast<u32>(GunCon::Binding::A));

  // Partway through a poll, the rest of it continues after loading.
  ExpectResumesAfterLoad(*guncon, MakeGunCon, LONG_POLL, GunConReply(Low(GUNCON_A_BIT)));

  // Input state is only loaded if asked to.
  const std::vector<u8> state = SaveState(*guncon);
  const auto loaded = MakeGunCon();
  PressBinding(*loaded, static_cast<u32>(GunCon::Binding::B));
  LoadState(*loaded, state, false);
  EXPECT_EQ(TransferCommand(*loaded, LONG_POLL), GunConReply(Low(GUNCON_B_BIT)));
  LoadState(*loaded, state, true);
  EXPECT_EQ(TransferCommand(*loaded, LONG_POLL), GunConReply(Low(GUNCON_A_BIT)));
}

TEST(Justifier, Poll)
{
  const auto justifier = MakeJustifier();
  EXPECT_EQ(justifier->GetType(), ControllerType::Justifier);
  EXPECT_FALSE(justifier->GetAnalogInputBytes().has_value());

  EXPECT_EQ(TransferCommand(*justifier, POLL), JustifierReply(NO_BUTTONS));
  PressBinding(*justifier, static_cast<u32>(Justifier::Binding::Start));
  EXPECT_EQ(TransferCommand(*justifier, {0x01, 0x42, 0xFF, 0x5A, 0x01}), JustifierReply(Low(JUSTIFIER_START_BIT)));
}

TEST(Justifier, RejectedBytes)
{
  const auto justifier = MakeJustifier();
  ExpectRejectedBytes(*justifier, {0x00, 0x43, 0x44, 0x4D}, 0x31, POLL, JustifierReply(NO_BUTTONS));
}

TEST(Justifier, ResetTransferState)
{
  const auto justifier = MakeJustifier();
  PressBinding(*justifier, static_cast<u32>(Justifier::Binding::Back));
  for (u32 i = 0; i < 2; i++)
  {
    for (size_t len = 1; len < POLL.size(); len++)
    {
      TransferBytes(*justifier, std::vector<u8>(POLL.begin(), POLL.begin() + len));
      if (i == 0)
        justifier->ResetTransferState();
      else
        justifier->Reset();

      EXPECT_EQ(TransferCommand(*justifier, POLL), JustifierReply(Low(JUSTIFIER_BACK_BIT)))
        << (i ? "Reset" : "ResetTransferState") << " after " << len;
    }
  }
}

TEST(Justifier, Buttons)
{
  const auto justifier = MakeJustifier();
  static constexpr std::pair<Justifier::Binding, u16> buttons[] = {{Justifier::Binding::Trigger, JUSTIFIER_TRIGGER_BIT},
                                                                   {Justifier::Binding::Start, JUSTIFIER_START_BIT},
                                                                   {Justifier::Binding::Back, JUSTIFIER_BACK_BIT}};
  for (const auto& [binding, bit] : buttons)
  {
    PressBinding(*justifier, static_cast<u32>(binding));
    EXPECT_EQ(TransferCommand(*justifier, POLL), JustifierReply(Low(bit))) << static_cast<u32>(binding);
    PressBinding(*justifier, static_cast<u32>(binding), 0.0f);
  }

  EXPECT_EQ(GetBindIndex(Justifier::INFO, "Trigger"), static_cast<u32>(Justifier::Binding::Trigger));
  EXPECT_EQ(GetBindIndex(Justifier::INFO, "ShootOffscreen"), static_cast<u32>(Justifier::Binding::ShootOffscreen));
  EXPECT_EQ(GetBindIndex(Justifier::INFO, "Start"), static_cast<u32>(Justifier::Binding::Start));
  EXPECT_EQ(GetBindIndex(Justifier::INFO, "Back"), static_cast<u32>(Justifier::Binding::Back));
  EXPECT_EQ(GetBindIndex(Justifier::INFO, "Pointer"), static_cast<u32>(Justifier::Binding::ButtonCount));
  EXPECT_EQ(Justifier::INFO.bindings.size(), 9u);
  for (const char* setting :
       {"CrosshairImagePath", "CrosshairScale", "CrosshairColor", "XScale", "FirstLineOffset", "LastLineOffset",
        "TickOffset", "OffscreenOOBFrames", "OffscreenTriggerFrames", "OffscreenReleaseFrames"})
  {
    EXPECT_TRUE(HasSetting(Justifier::INFO, setting)) << setting;
  }
}

TEST(Justifier, IRQEnable)
{
  const auto justifier = MakeJustifier();

  // Bit 4 of the TAP byte enables the IRQ. It doesn't change the reply, and off-screen the IRQ is never scheduled.
  EXPECT_EQ(TransferCommand(*justifier, {0x01, 0x42, 0x10, 0x00, 0x00}), JustifierReply(NO_BUTTONS));
  EXPECT_FALSE(IsEventActive("Justifier IRQ P0"));
  EXPECT_EQ(TransferCommand(*justifier, {0x01, 0x42, 0x10, 0x00, 0x00}), JustifierReply(NO_BUTTONS));
  EXPECT_FALSE(IsEventActive("Justifier IRQ P0"));
  EXPECT_EQ(TransferCommand(*justifier, POLL), JustifierReply(NO_BUTTONS));
  EXPECT_FALSE(IsEventActive("Justifier IRQ P0"));
}

TEST(Justifier, ShootOffscreen)
{
  // With the default 5 frames each: out of bounds, trigger held, then released while still out of bounds.
  const auto justifier = MakeJustifier();
  const u32 trigger = static_cast<u32>(Justifier::Binding::Trigger);
  const u32 offscreen = static_cast<u32>(Justifier::Binding::ShootOffscreen);
  PressBinding(*justifier, offscreen);
  EXPECT_EQ(justifier->GetBindState(offscreen), 0.0f);
  for (u32 poll = 1; poll <= 16; poll++)
  {
    // Pressing it again or releasing it partway through doesn't restart or stop it.
    if (poll == 3)
      PressBinding(*justifier, offscreen);
    else if (poll == 8)
      PressBinding(*justifier, offscreen, 0.0f);

    const bool pressed = (poll >= 6 && poll <= 10);
    EXPECT_EQ(TransferCommand(*justifier, POLL), JustifierReply(pressed ? Low(JUSTIFIER_TRIGGER_BIT) : NO_BUTTONS))
      << poll;
    EXPECT_EQ(justifier->GetBindState(trigger), pressed ? 1.0f : 0.0f) << poll;
  }

  // The frames count 42h, not 01h.
  PressBinding(*justifier, offscreen);
  for (u32 i = 0; i < 5; i++)
  {
    TransferBytes(*justifier, {0x01});
    justifier->ResetTransferState();
  }
  EXPECT_EQ(TransferCommand(*justifier, POLL), JustifierReply(NO_BUTTONS));

  // Out of bounds for the out-of-bounds frames, then the trigger is held for the trigger frames, and released for the
  // release frames.
  INISettingsInterface si;
  si.SetIntValue("Pad1", "OffscreenOOBFrames", 1);
  si.SetIntValue("Pad1", "OffscreenTriggerFrames", 2);
  si.SetIntValue("Pad1", "OffscreenReleaseFrames", 3);
  const auto short_justifier = MakeJustifier();
  short_justifier->LoadSettings(si, "Pad1", false);
  PressBinding(*short_justifier, offscreen);
  for (u32 poll = 1; poll <= 7; poll++)
  {
    const bool pressed = (poll >= 2 && poll <= 3);
    EXPECT_EQ(TransferCommand(*short_justifier, POLL),
              JustifierReply(pressed ? Low(JUSTIFIER_TRIGGER_BIT) : NO_BUTTONS))
      << poll;
  }

  // With no trigger frames, the trigger isn't pressed.
  si.SetIntValue("Pad1", "OffscreenTriggerFrames", 0);
  short_justifier->LoadSettings(si, "Pad1", false);
  PressBinding(*short_justifier, offscreen);
  for (u32 poll = 1; poll <= 5; poll++)
    EXPECT_EQ(TransferCommand(*short_justifier, POLL), JustifierReply(NO_BUTTONS)) << poll;
  EXPECT_EQ(short_justifier->GetBindState(trigger), 0.0f);

  // The release frames are at least 1, so the trigger is released.
  si.SetIntValue("Pad1", "OffscreenTriggerFrames", 2);
  si.SetIntValue("Pad1", "OffscreenReleaseFrames", 0);
  short_justifier->LoadSettings(si, "Pad1", false);
  PressBinding(*short_justifier, offscreen);
  for (u32 poll = 1; poll <= 5; poll++)
  {
    const bool pressed = (poll >= 2 && poll <= 3);
    EXPECT_EQ(TransferCommand(*short_justifier, POLL),
              JustifierReply(pressed ? Low(JUSTIFIER_TRIGGER_BIT) : NO_BUTTONS))
      << poll;
  }
  EXPECT_EQ(short_justifier->GetBindState(trigger), 0.0f);
}

TEST(Justifier, MemberDefaults)
{
  // Current behaviour: without LoadSettings(), the off-screen frames are all 0, so Shoot Offscreen does nothing.
  Justifier justifier(0);
  justifier.SetBindState(static_cast<u32>(Justifier::Binding::ShootOffscreen), 1.0f);
  for (u32 poll = 1; poll <= 16; poll++)
    EXPECT_EQ(TransferCommand(justifier, POLL), JustifierReply(NO_BUTTONS)) << poll;
}

TEST(Justifier, Settings)
{
  ExpectSignedIntegerFormats(Justifier::INFO);

  for (const SettingInfo& si : Justifier::INFO.settings)
  {
    if (std::string_view(si.name) == "OffscreenReleaseFrames")
      EXPECT_EQ(std::string_view(si.display_name), "Off-Screen Release Frames");
  }
}

TEST(Justifier, SaveState)
{
  const auto justifier = MakeJustifier();
  PressBinding(*justifier, static_cast<u32>(Justifier::Binding::Start));

  // Partway through a poll, the rest of it continues after loading.
  ExpectResumesAfterLoad(*justifier, MakeJustifier, POLL, JustifierReply(Low(JUSTIFIER_START_BIT)));

  // Input state is only loaded if asked to.
  std::vector<u8> state = SaveState(*justifier);
  {
    const auto loaded = MakeJustifier();
    PressBinding(*loaded, static_cast<u32>(Justifier::Binding::Back));
    LoadState(*loaded, state, false);
    EXPECT_EQ(TransferCommand(*loaded, POLL), JustifierReply(Low(JUSTIFIER_BACK_BIT)));
    LoadState(*loaded, state, true);
    EXPECT_EQ(TransferCommand(*loaded, POLL), JustifierReply(Low(JUSTIFIER_START_BIT)));
  }

  // The off-screen countdown continues after loading.
  justifier->SetBindState(static_cast<u32>(Justifier::Binding::Start), 0.0f);
  justifier->SetBindState(static_cast<u32>(Justifier::Binding::ShootOffscreen), 1.0f);
  for (u32 poll = 1; poll <= 3; poll++)
    EXPECT_EQ(TransferCommand(*justifier, POLL), JustifierReply(NO_BUTTONS)) << poll;
  state = SaveState(*justifier);
  {
    const auto loaded = MakeJustifier();
    LoadState(*loaded, state, true);
    for (u32 poll = 4; poll <= 16; poll++)
    {
      const bool pressed = (poll >= 6 && poll <= 10);
      EXPECT_EQ(TransferCommand(*loaded, POLL), JustifierReply(pressed ? Low(JUSTIFIER_TRIGGER_BIT) : NO_BUTTONS))
        << poll;
    }

    // It's over, so a new shot works.
    loaded->SetBindState(static_cast<u32>(Justifier::Binding::ShootOffscreen), 1.0f);
    for (u32 poll = 1; poll <= 6; poll++)
    {
      EXPECT_EQ(TransferCommand(*loaded, POLL), JustifierReply((poll == 6) ? Low(JUSTIFIER_TRIGGER_BIT) : NO_BUTTONS))
        << poll;
    }
  }
}

TEST(GunCon, LegacyState)
{
  // Saved by version 104 partway through a poll, with the trigger pressed.
  static const std::vector<u8> state = {0xFF, 0xDF, 0x00, 0x00, 0x00, 0x00, 0x03};
  const auto guncon = MakeGunCon();
  LoadState(*guncon, state, true, 104);

  // The transfer starts again from idle.
  EXPECT_EQ(TransferCommand(*guncon, LONG_POLL), GunConReply(Low(GUNCON_TRIGGER_BIT)));
}

TEST(Justifier, LegacyState)
{
  // Saved by version 104 partway through a poll, with Start pressed, four polls into an off-screen shot.
  static const std::vector<u8> state = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7, 0xFF, 0x0B, 0x00, 0x00, 0x02};
  const auto justifier = MakeJustifier();
  LoadState(*justifier, state, true, 104);

  // The transfer starts again from idle, and the shot continues.
  for (u32 poll = 5; poll <= 12; poll++)
  {
    const bool pressed = (poll >= 6 && poll <= 10);
    EXPECT_EQ(TransferCommand(*justifier, POLL),
              JustifierReply(Low(JUSTIFIER_START_BIT | (pressed ? JUSTIFIER_TRIGGER_BIT : 0))))
      << poll;
  }
}
