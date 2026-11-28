// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "config_mode_controller.h"

#include <memory>

class ForceFeedbackDevice;

class JogCon final : public ConfigModeController
{
public:
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
    Mode = 16,
    MaxCount
  };

  enum class HalfAxis : u8
  {
    SteeringLeft,
    SteeringRight,
    MaxCount,
  };

  static const Controller::ControllerInfo INFO;

  explicit JogCon(u32 index);
  ~JogCon() override;

  static std::unique_ptr<JogCon> Create(u32 index);

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;
  u32 GetButtonStateBits() const override;

  void LoadSettings(const SettingsInterface& si, const char* section, bool initial) override;

private:
  enum : u8
  {
    MOTOR_COMMAND_STOP = 0x0,
    MOTOR_COMMAND_RIGHT = 0x1,
    MOTOR_COMMAND_LEFT = 0x2,
    MOTOR_COMMAND_HOLD = 0x3,
    MOTOR_COMMAND_DROP_REVOLUTIONS = 0x8,
    MOTOR_COMMAND_DROP_REVOLUTIONS_AND_HOLD = 0xB,
    MOTOR_COMMAND_NEW_HOLD = 0xC,
  };

  static constexpr float DEFAULT_STEERING_HOLD_DEADZONE = 0.03f;

  static constexpr u32 HALFAXIS_BIND_START_INDEX = static_cast<u32>(Button::MaxCount);
  static constexpr u32 MOTOR_BIND_START_INDEX = HALFAXIS_BIND_START_INDEX + static_cast<u32>(HalfAxis::MaxCount);
  static constexpr u32 LED_BIND_START_INDEX = MOTOR_BIND_START_INDEX + 1;
  static constexpr u32 FFDEVICE_BIND_START_INDEX = LED_BIND_START_INDEX + 1;

  static const Controller::ControllerBindingInfo s_binding_info[];

  u8 GetAnalogModeID() const override;
  u8 GetNumMotors() const override;
  u8 GetCommand45Actuators() const override;
  void PatchCommand4C(u8 param) override;
  bool HasLegacyRumble() const override;
  bool CanEnterConfigMode() const override;
  u32 GetMotorBindIndex(u32 motor) const override;
  void SetMotorState(u32 motor, u8 value) override;
  void Poll() override;

  void UpdateSteeringHold();
  void SetMotorDirection(u8 direction_command, u8 strength);

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);
  s8 m_steering_state = 0;

  // both directions of axis state, merged to m_steering_state
  std::array<u8, static_cast<u32>(HalfAxis::MaxCount)> m_half_axis_state{};

  s8 m_last_steering_state = 0;
  u8 m_last_motor_command = 0;
  s8 m_steering_hold_position = 0;
  u8 m_steering_hold_strength = 0;

  s8 m_steering_hold_deadzone = 0;

  float m_analog_deadzone = 0.0f;
  float m_analog_sensitivity = 1.33f;
  float m_button_deadzone = 0.0f;

  float m_last_strength = 0.0f;

  std::unique_ptr<ForceFeedbackDevice> m_force_feedback_device;

  std::string m_force_feedback_device_name;
};
