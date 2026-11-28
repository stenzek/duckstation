// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "config_mode_controller.h"

#include <array>
#include <memory>
#include <optional>

class AnalogController final : public ConfigModeController
{
public:
  enum class Axis : u8
  {
    LeftX,
    LeftY,
    RightX,
    RightY,
    Count
  };

  enum class Button : u8
  {
    Select = 0,
    L3 = 1,
    R3 = 2,
    Start = 3,
    Up = 4,
    Right = 5,
    Down = 6,
    Left = 7,
    L2 = 8,
    R2 = 9,
    L1 = 10,
    R1 = 11,
    Triangle = 12,
    Circle = 13,
    Cross = 14,
    Square = 15,
    Analog = 16,
    Count
  };

  enum class HalfAxis : u8
  {
    LLeft,
    LRight,
    LDown,
    LUp,
    RLeft,
    RRight,
    RDown,
    RUp,
    Count
  };

  static constexpr u8 NUM_MOTORS = 2;

  static const Controller::ControllerInfo INFO;

  explicit AnalogController(u32 index);
  ~AnalogController() override;

  static std::unique_ptr<AnalogController> Create(u32 index);

  void Reset() override;
  bool DoState(StateWrapper& sw, bool ignore_input_state) override;

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

  u32 GetMotorBindIndex(u32 motor) const override;
  void Poll() override;

  u16 GetExtraButtonMask() const;

  float m_analog_deadzone = 0.0f;
  float m_analog_sensitivity = 1.33f;
  float m_button_deadzone = DEFAULT_BUTTON_DEADZONE;
  u8 m_invert_left_stick = 0;
  u8 m_invert_right_stick = 0;

  bool m_force_analog_on_reset : 1 = true;
  bool m_analog_dpad_in_digital_mode : 1 = true;
  bool m_disable_socd : 1 = false;
  u8 m_analog_shoulder_buttons = 0;
  u8 m_analog_trigger_buttons = 0;

  std::array<u8, static_cast<u8>(Axis::Count)> m_axis_state{};

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);

  // both directions of axis state, merged to m_axis_state
  std::array<u8, static_cast<u32>(HalfAxis::Count)> m_half_axis_state{};
};
