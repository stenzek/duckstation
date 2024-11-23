// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "controller.h"

#include <memory>

class ForceFeedbackDevice;

class JogCon final : public Controller
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

  JogCon(u32 index);
  ~JogCon() override;

  static std::unique_ptr<JogCon> Create(u32 index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;
  u32 GetButtonStateBits() const override;
  u32 GetInputOverlayIconColor() const override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

  void LoadSettings(const SettingsInterface& si, const char* section, bool initial) override;

private:
  enum class Command : u8
  {
    Idle,
    Ready,
    ReadPad,
    SetMode,
    GetAnalogMode,
    GetSetRumble,
    Command46,
    Command47,
    Command4C,
  };

  enum : u8
  {
    LargeMotor = 0,
    SmallMotor = 1
  };

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

  u8 GetIDByte() const;
  u8 GetModeID() const;

  // Get number of response halfwords (excluding the initial controller info halfword)
  u8 GetResponseNumHalfwords() const;

  void Poll();
  void UpdateSteeringHold();

  void SetMotorState(u8 value);
  void SetMotorDirection(u8 direction_command, u8 strength);
  void ResetMotorConfig();

  void SetJogConMode(bool enabled, bool show_message);

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);
  s8 m_steering_state = 0;

  // both directions of axis state, merged to m_steering_state
  std::array<u8, static_cast<u32>(HalfAxis::MaxCount)> m_half_axis_state{};

  Command m_command = Command::Idle;
  u8 m_command_step = 0;
  u8 m_response_length = 0;
  u8 m_status_byte = 0x5A;

  s8 m_last_steering_state = 0;
  u8 m_last_motor_command = 0;
  s8 m_steering_hold_position = 0;
  u8 m_steering_hold_strength = 0;

  bool m_configuration_mode = false;
  bool m_jogcon_mode = false;
  bool m_mode_toggle_queued = false;

  std::array<u8, 6> m_rumble_config{};

  // Transmit and receive buffers, not including the first Hi-Z/ack response byte
  static constexpr u32 MAX_RESPONSE_LENGTH = 8;
  std::array<u8, MAX_RESPONSE_LENGTH> m_rx_buffer;
  std::array<u8, MAX_RESPONSE_LENGTH> m_tx_buffer;

  s8 m_steering_hold_deadzone = 0;

  float m_analog_deadzone = 0.0f;
  float m_analog_sensitivity = 1.33f;
  float m_button_deadzone = 0.0f;

  std::string m_force_feedback_device_name;
  std::unique_ptr<ForceFeedbackDevice> m_force_feedback_device;
};
