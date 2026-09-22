// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/negcon.h"
#include "core/negcon_base.h"
#include "core/negcon_rumble.h"
#include "core/save_state_version.h"

#include "util/ini_settings_interface.h"
#include "util/state_wrapper.h"

#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <span>
#include <string_view>
#include <type_traits>

static_assert(std::is_base_of_v<NegConBase, NeGcon>);
static_assert(std::is_base_of_v<NegConBase, NeGconRumble>);
static_assert(static_cast<u32>(NeGcon::Button::Count) == 8);
static_assert(static_cast<u32>(NeGconRumble::Button::Analog) == 8);
static_assert(static_cast<u32>(NeGconRumble::Button::Count) == 9);

static const Controller::ControllerBindingInfo* FindBinding(const Controller::ControllerInfo& info,
                                                            std::string_view name);
static bool HasSetting(const Controller::ControllerInfo& info, std::string_view name);

TEST(NegConBase, BindingIndices)
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

TEST(NegConBase, CommonButtonMappings)
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

TEST(NegConBase, DefaultAxisModifiers)
{
  NeGcon negcon(0);
  NeGconRumble rumble(0);
  constexpr u32 negcon_axis_start = static_cast<u32>(NeGcon::Button::Count);
  constexpr u32 rumble_axis_start = static_cast<u32>(NeGconRumble::Button::Count);

  negcon.SetBindState(negcon_axis_start + static_cast<u32>(NeGcon::HalfAxis::SteeringRight), 1.0f);
  rumble.SetBindState(rumble_axis_start + static_cast<u32>(NeGconRumble::HalfAxis::SteeringRight), 1.0f);
  negcon.SetBindState(negcon_axis_start + static_cast<u32>(NeGcon::HalfAxis::I), 0.5f);
  rumble.SetBindState(rumble_axis_start + static_cast<u32>(NeGconRumble::HalfAxis::I), 0.5f);

  ASSERT_TRUE(negcon.GetAnalogInputBytes().has_value());
  ASSERT_TRUE(rumble.GetAnalogInputBytes().has_value());
  EXPECT_EQ(negcon.GetAnalogInputBytes(), rumble.GetAnalogInputBytes());
  EXPECT_EQ(negcon.GetAnalogInputBytes().value(), UINT32_C(0x000080FF));
  EXPECT_EQ(negcon.GetBindState(negcon_axis_start + static_cast<u32>(NeGcon::HalfAxis::SteeringRight)), 1.0f);
  EXPECT_EQ(rumble.GetBindState(rumble_axis_start + static_cast<u32>(NeGconRumble::HalfAxis::SteeringRight)), 1.0f);
}

TEST(NegConBase, ConfiguredAxisModifiers)
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
  EXPECT_EQ(negcon.GetAnalogInputBytes().value(), UINT32_C(0x408020A0));
}

TEST(NegConBase, RumbleSettings)
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

TEST(NegConBase, DeviceStateLayouts)
{
  std::array<u8, 64> state_data = {};

  NeGcon negcon(0);
  negcon.SetBindState(static_cast<u32>(NeGcon::Button::A), 1.0f);
  StateWrapper negcon_sw(std::span<u8>(state_data), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  ASSERT_TRUE(negcon.DoState(negcon_sw, true));
  ASSERT_FALSE(negcon_sw.HasError());
  EXPECT_EQ(negcon_sw.GetPosition(), 3u);

  u16 negcon_buttons;
  std::memcpy(&negcon_buttons, state_data.data(), sizeof(negcon_buttons));
  EXPECT_EQ(negcon_buttons, UINT16_C(0xDFFF));

  state_data.fill(0);
  NeGconRumble rumble(0);
  rumble.SetBindState(static_cast<u32>(NeGconRumble::Button::A), 1.0f);
  StateWrapper rumble_sw(std::span<u8>(state_data), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  ASSERT_TRUE(rumble.DoState(rumble_sw, true));
  ASSERT_FALSE(rumble_sw.HasError());
  EXPECT_EQ(rumble_sw.GetPosition(), 24u);

  u16 rumble_buttons;
  std::memcpy(&rumble_buttons, &state_data[4], sizeof(rumble_buttons));
  EXPECT_EQ(rumble_buttons, UINT16_C(0xDFFF));

  std::array<u8, 6> legacy_state_data = {};
  NeGconRumble legacy_rumble(0);
  legacy_rumble.SetBindState(static_cast<u32>(NeGconRumble::Button::A), 1.0f);
  StateWrapper legacy_rumble_sw(std::span<const u8>(legacy_state_data), StateWrapper::Mode::Read, 43);
  ASSERT_TRUE(legacy_rumble.DoState(legacy_rumble_sw, true));
  ASSERT_FALSE(legacy_rumble_sw.HasError());
  EXPECT_EQ(legacy_rumble_sw.GetPosition(), legacy_state_data.size());
  EXPECT_EQ(legacy_rumble.GetButtonStateBits(), 0u);
}

TEST(NegConBase, DeviceProtocols)
{
  NeGcon negcon(0);
  negcon.SetBindState(static_cast<u32>(NeGcon::Button::A), 1.0f);

  u8 data_out;
  EXPECT_TRUE(negcon.Transfer(0x01, &data_out));
  EXPECT_EQ(data_out, 0xFF);
  EXPECT_TRUE(negcon.Transfer(0x42, &data_out));
  EXPECT_EQ(data_out, 0x23);
  EXPECT_TRUE(negcon.Transfer(0x00, &data_out));
  EXPECT_EQ(data_out, 0x5A);
  EXPECT_TRUE(negcon.Transfer(0x00, &data_out));
  EXPECT_EQ(data_out, 0xFF);
  EXPECT_TRUE(negcon.Transfer(0x00, &data_out));
  EXPECT_EQ(data_out, 0xDF);

  NeGconRumble rumble(0);
  rumble.SetBindState(static_cast<u32>(NeGconRumble::Button::A), 1.0f);
  EXPECT_TRUE(rumble.Transfer(0x01, &data_out));
  EXPECT_EQ(data_out, 0xFF);
  EXPECT_TRUE(rumble.Transfer(0x42, &data_out));
  EXPECT_EQ(data_out, 0x41);
  EXPECT_TRUE(rumble.Transfer(0x00, &data_out));
  EXPECT_EQ(data_out, 0x5A);
  EXPECT_TRUE(rumble.Transfer(0x00, &data_out));
  EXPECT_EQ(data_out, 0xFF);
  EXPECT_FALSE(rumble.Transfer(0x00, &data_out));
  EXPECT_EQ(data_out, 0xDF);
}

const Controller::ControllerBindingInfo* FindBinding(const Controller::ControllerInfo& info, std::string_view name)
{
  for (const Controller::ControllerBindingInfo& binding : info.bindings)
  {
    if (name == binding.name)
      return &binding;
  }

  return nullptr;
}

bool HasSetting(const Controller::ControllerInfo& info, std::string_view name)
{
  for (const SettingInfo& setting : info.settings)
  {
    if (name == setting.name)
      return true;
  }

  return false;
}
