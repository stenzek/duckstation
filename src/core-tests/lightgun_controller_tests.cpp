// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller_test_utils.h"

#include "core/guncon.h"
#include "core/justifier.h"
#include "core/lightgun_controller.h"

#include <gtest/gtest.h>

using namespace ControllerTestUtils;

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
