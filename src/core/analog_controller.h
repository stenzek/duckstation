// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "controller.h"

#include <array>
#include <memory>
#include <optional>

class AnalogController final : public Controller
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

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool ignore_input_state) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;
  u32 GetButtonStateBits() const override;
  std::optional<u32> GetAnalogInputBytes() const override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

  void LoadSettings(const SettingsInterface& si, const char* section, bool initial) override;

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

  static constexpr s16 DEFAULT_SMALL_MOTOR_VIBRATION_BIAS = 8;
  static constexpr s16 DEFAULT_LARGE_MOTOR_VIBRATION_BIAS = 8;

  static constexpr u32 HALFAXIS_BIND_START_INDEX = static_cast<u32>(Button::Count);
  static constexpr u32 MOTOR_BIND_START_INDEX = HALFAXIS_BIND_START_INDEX + static_cast<u32>(HalfAxis::Count);
  static constexpr u32 LED_BIND_START_INDEX = MOTOR_BIND_START_INDEX + NUM_MOTORS;

  static const Controller::ControllerBindingInfo s_binding_info[];

  Command m_command = Command::Idle;
  u8 m_command_step = 0;
  u8 m_response_length = 0;

  // Transmit and receive buffers, not including the first Hi-Z/ack response byte
  static constexpr u32 MAX_RESPONSE_LENGTH = 8;
  std::array<u8, MAX_RESPONSE_LENGTH> m_rx_buffer{};
  std::array<u8, MAX_RESPONSE_LENGTH> m_tx_buffer{};

  // Get number of response halfwords (excluding the initial controller info halfword)
  u8 GetResponseNumHalfwords() const;

  u8 GetModeID() const;
  u8 GetIDByte() const;

  void SetAnalogMode(bool enabled, bool show_message);
  void ProcessAnalogModeToggle();
  void SetMotorState(u32 motor, u8 value);
  float GetMotorStrength(u32 motor) const;
  u16 GetExtraButtonMask() const;
  void ResetRumbleConfig();
  void Poll();

  float m_analog_deadzone = 0.0f;
  float m_analog_sensitivity = 1.33f;
  float m_button_deadzone = 0.0f;
  std::array<s16, NUM_MOTORS> m_vibration_bias{DEFAULT_LARGE_MOTOR_VIBRATION_BIAS, DEFAULT_SMALL_MOTOR_VIBRATION_BIAS};
  u8 m_invert_left_stick = 0;
  u8 m_invert_right_stick = 0;

  bool m_force_analog_on_reset = false;
  bool m_analog_dpad_in_digital_mode = false;
  u8 m_analog_shoulder_buttons = 0;
  u8 m_analog_trigger_buttons = 0;

  bool m_analog_mode = false;
  bool m_analog_locked = false;
  bool m_dualshock_enabled = false;
  bool m_configuration_mode = false;

  std::array<u8, static_cast<u8>(Axis::Count)> m_axis_state{};

  enum : u8
  {
    SmallMotor = 0,
    LargeMotor = 1,
  };

  std::array<u8, 6> m_rumble_config{};

  bool m_analog_toggle_queued = false;
  u8 m_status_byte = 0;

  // TODO: Set this with command 0x4D and increase response length in digital mode accordingly
  u8 m_digital_mode_extra_halfwords = 0;

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);

  MotorState m_motor_state{};

  // both directions of axis state, merged to m_axis_state
  std::array<u8, static_cast<u32>(HalfAxis::Count)> m_half_axis_state{};
};
