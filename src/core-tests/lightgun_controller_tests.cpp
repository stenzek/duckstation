// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/guncon.h"
#include "core/justifier.h"
#include "core/lightgun_controller.h"
#include "core/save_state_version.h"

#include "util/state_wrapper.h"

#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <span>

namespace {

class TestLightgunController final : public LightgunController
{
public:
  TestLightgunController() : LightgunController(0, {{2, 4, 6, 0}}) {}

  ControllerType GetType() const override { return ControllerType::None; }
  void Reset() override {}
  void ResetTransferState() override {}
  bool Transfer(const u8, u8*) override { return false; }

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

} // namespace

TEST(LightgunController, ButtonBindings)
{
  TestLightgunController controller;

  EXPECT_EQ(controller.GetButtonStateBits(), UINT16_C(0xFFFF));
  EXPECT_EQ(controller.GetBindState(0), 0.0f);

  controller.SetBindState(0, 0.49f);
  EXPECT_EQ(controller.GetBindState(0), 0.0f);
  EXPECT_EQ(controller.GetButtonStateBits(), UINT16_C(0xFFFF));

  controller.SetBindState(0, 0.5f);
  EXPECT_EQ(controller.GetBindState(0), 1.0f);
  EXPECT_EQ(controller.GetButtonStateBits(), UINT16_C(0xFFFB));

  controller.SetBindState(0, 0.0f);
  EXPECT_EQ(controller.GetBindState(0), 0.0f);
  EXPECT_EQ(controller.GetButtonStateBits(), UINT16_C(0xFFFF));

  controller.SetBindState(8, 1.0f);
  EXPECT_EQ(controller.GetButtonStateBits(), UINT16_C(0xFFFF));
}

TEST(LightgunController, ShootOffscreenDispatch)
{
  TestLightgunController controller;

  controller.SetBindState(3, 1.0f);
  EXPECT_TRUE(controller.GetLastShootOffscreenState());
  EXPECT_EQ(controller.GetShootOffscreenCallCount(), 1u);
  EXPECT_EQ(controller.GetBindState(3), 0.0f);
  EXPECT_EQ(controller.GetButtonStateBits(), UINT16_C(0xFFFF));

  controller.SetBindState(3, 0.0f);
  EXPECT_FALSE(controller.GetLastShootOffscreenState());
  EXPECT_EQ(controller.GetShootOffscreenCallCount(), 2u);
  EXPECT_EQ(controller.GetButtonStateBits(), UINT16_C(0xFFFF));
}

TEST(LightgunController, DeviceButtonMappings)
{
  GunCon guncon(0);
  guncon.SetBindState(static_cast<u32>(GunCon::Binding::Trigger), 1.0f);
  guncon.SetBindState(static_cast<u32>(GunCon::Binding::A), 1.0f);
  guncon.SetBindState(static_cast<u32>(GunCon::Binding::B), 1.0f);
  EXPECT_EQ(guncon.GetButtonStateBits(), UINT16_C(0x9FF7));

  Justifier justifier(0);
  justifier.SetBindState(static_cast<u32>(Justifier::Binding::Trigger), 1.0f);
  justifier.SetBindState(static_cast<u32>(Justifier::Binding::Start), 1.0f);
  justifier.SetBindState(static_cast<u32>(Justifier::Binding::Back), 1.0f);
  EXPECT_EQ(justifier.GetButtonStateBits(), UINT16_C(0x3FF7));
}

TEST(LightgunController, StateApplication)
{
  TestLightgunController controller;
  controller.SetBindState(1, 1.0f);

  std::array<u8, sizeof(u16)> state_data = {};
  StateWrapper write_sw(std::span<u8>(state_data), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  ASSERT_TRUE(controller.DoState(write_sw, true));
  ASSERT_FALSE(write_sw.HasError());
  EXPECT_EQ(write_sw.GetPosition(), sizeof(u16));

  controller.SetBindState(1, 0.0f);
  StateWrapper no_apply_sw(std::span<const u8>(state_data), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
  ASSERT_TRUE(controller.DoState(no_apply_sw, false));
  EXPECT_EQ(controller.GetBindState(1), 0.0f);

  StateWrapper apply_sw(std::span<const u8>(state_data), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
  ASSERT_TRUE(controller.DoState(apply_sw, true));
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

TEST(LightgunController, DeviceStateLayout)
{
  std::array<u8, 32> state_data = {};

  GunCon guncon(0);
  guncon.SetBindState(static_cast<u32>(GunCon::Binding::A), 1.0f);
  StateWrapper guncon_sw(std::span<u8>(state_data), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  ASSERT_TRUE(guncon.DoState(guncon_sw, true));
  EXPECT_EQ(guncon_sw.GetPosition(), 7u);

  u16 guncon_buttons;
  std::memcpy(&guncon_buttons, state_data.data(), sizeof(guncon_buttons));
  EXPECT_EQ(guncon_buttons, UINT16_C(0xFFF7));

  state_data.fill(0);
  Justifier justifier(0);
  justifier.SetBindState(static_cast<u32>(Justifier::Binding::Start), 1.0f);
  StateWrapper justifier_sw(std::span<u8>(state_data), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  ASSERT_TRUE(justifier.DoState(justifier_sw, true));
  EXPECT_EQ(justifier_sw.GetPosition(), 12u);

  u16 justifier_buttons;
  std::memcpy(&justifier_buttons, &state_data[sizeof(u16) * 3], sizeof(justifier_buttons));
  EXPECT_EQ(justifier_buttons, UINT16_C(0xFFF7));
}
