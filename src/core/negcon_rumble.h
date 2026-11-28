// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "config_mode_controller.h"
#include "negcon.h"

#include <array>
#include <memory>

class NeGconRumble final : public ConfigModeController
{
public:
  using Axis = NeGconInputs::Axis;
  using HalfAxis = NeGconInputs::HalfAxis;

  enum class Button : u8
  {
    Start = 0,
    Up = 1,
    Right = 2,
    Down = 3,
    Left = 4,
    R = 5,
    B = 6,
    A = 7,
    Analog = 8,
    Count
  };

  static constexpr u8 NUM_MOTORS = 2;

  static const Controller::ControllerInfo INFO;

  explicit NeGconRumble(u32 index);
  ~NeGconRumble() override;

  static std::unique_ptr<NeGconRumble> Create(u32 index);

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;

  u32 GetButtonStateBits() const override;
  std::optional<u32> GetAnalogInputBytes() const override;

  void LoadSettings(const SettingsInterface& si, const char* section, bool initial) override;

private:
  static constexpr u32 HALFAXIS_BIND_START_INDEX = static_cast<u32>(Button::Count);
  static constexpr u32 MOTOR_BIND_START_INDEX = HALFAXIS_BIND_START_INDEX + static_cast<u32>(HalfAxis::Count);
  static constexpr u32 LED_BIND_START_INDEX = MOTOR_BIND_START_INDEX + NUM_MOTORS;

  static const Controller::ControllerBindingInfo s_binding_info[];

  bool DoLegacyState(StateWrapper& sw);

  u8 GetAnalogModeID() const override;
  void PatchCommand4C(u8 param) override;
  u32 GetMotorBindIndex(u32 motor) const override;
  void Poll() override;

  NeGconInputs m_inputs{HALFAXIS_BIND_START_INDEX};
};
