// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "controller.h"
#include <array>
#include <memory>
#include <optional>
#include <string_view>

class NeGconRumble final : public Controller
{
public:
  enum class Axis : u8
  {
    Steering = 0,
    I = 1,
    II = 2,
    L = 3,
    Count
  };

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

  enum class HalfAxis : u8
  {
    SteeringLeft,
    SteeringRight,
    I,
    II,
    L,
    Count
  };

  static constexpr u8 NUM_MOTORS = 2;

  static const Controller::ControllerInfo INFO;

  NeGconRumble(u32 index);
  ~NeGconRumble() override;

  static std::unique_ptr<NeGconRumble> Create(u32 index);

  ControllerType GetType() const override;
  bool InAnalogMode() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

  u32 GetButtonStateBits() const override;
  std::optional<u32> GetAnalogInputBytes() const override;

  void LoadSettings(SettingsInterface& si, const char* section) override;

private:
  using MotorState = std::array<u8, NUM_MOTORS>;

  enum class Command : u8
  {
    Idle,
    Ready,
    ReadPad,           // 0x42
    ConfigModeSetMode, // 0x43
    SetAnalogMode,     // 0x44
    GetAnalogMode,     // 0x45
    Command46,         // 0x46
    Command47,         // 0x47
    Command4C,         // 0x4C
    GetSetRumble       // 0x4D
  };

  bool m_force_analog_on_reset = true;
  bool m_analog_dpad_in_digital_mode = false;
  float m_analog_deadzone = 0.0f;
  float m_analog_sensitivity = 1.33f;
  float m_button_deadzone = 0.0f;
  u8 m_rumble_bias = 8;
  u8 m_invert_left_stick = 0;
  u8 m_invert_right_stick = 0;

  bool m_analog_mode = false;
  bool m_analog_locked = false;
  bool m_dualshock_enabled = false;
  bool m_configuration_mode = false;

  std::array<u8, static_cast<u8>(Axis::Count)> m_axis_state{};

  enum : u8
  {
    LargeMotor = 0,
    SmallMotor = 1
  };

  // steering, merged to m_axis_state
  std::array<u8, 2> m_half_axis_state{};

  // buttons are active low; bits 0-2, 8-10, 14-15 are not used and are always high
  u16 m_button_state = UINT16_C(0xFFFF);

  MotorState m_motor_state{};

  Command m_command = Command::Idle;
  int m_command_step = 0;

  // Transmit and receive buffers, not including the first Hi-Z/ack response byte
  static constexpr u32 MAX_RESPONSE_LENGTH = 8;
  std::array<u8, MAX_RESPONSE_LENGTH> m_rx_buffer;
  std::array<u8, MAX_RESPONSE_LENGTH> m_tx_buffer;
  u32 m_response_length = 0;

  std::array<u8, 6> m_rumble_config{};
  int m_rumble_config_large_motor_index = -1;
  int m_rumble_config_small_motor_index = -1;

  bool m_analog_toggle_queued = false;
  u8 m_status_byte = 0;

  // Get number of response halfwords (excluding the initial controller info halfword)
  u8 GetResponseNumHalfwords() const;

  u8 GetModeID() const;
  u8 GetIDByte() const;

  void SetAnalogMode(bool enabled, bool show_message);
  void ProcessAnalogModeToggle();
  void SetMotorState(u32 motor, u8 value);
  void UpdateHostVibration();
  u8 GetExtraButtonMaskLSB() const;
  void ResetRumbleConfig();
  void SetMotorStateForConfigIndex(int index, u8 value);

  float m_steering_deadzone = 0.00f;
  float m_steering_sensitivity = 1.00f;
};