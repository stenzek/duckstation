// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "controller.h"

#include <array>

/// Controllers with the DualShock config mode protocol (43h-4Dh), analog mode and rumble mapping.
/// DualShock values are used as the default for the per-type virtual methods.
class ConfigModeController : public Controller
{
public:
  ~ConfigModeController() override;

  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  void ResetTransferState() override;

protected:
  enum class Command : u8
  {
    ReadPad,           // 0x42
    ConfigModeSetMode, // 0x43
    SetAnalogMode,     // 0x44
    GetAnalogMode,     // 0x45
    Command46,         // 0x46
    Command47,         // 0x47
    Command4C,         // 0x4C
    GetSetRumble,      // 0x4D
    Command48,         // 0x48
    UnusedConfig,      // 0x40, 0x41, 0x49-0x4B, 0x4E, 0x4F
  };

  /// Motor numbers, as mapped by 4Dh.
  enum : u8
  {
    SmallMotor = 0,
    LargeMotor = 1,
    MAX_MOTORS = 2,
  };

  using MotorState = std::array<u8, MAX_MOTORS>;

  static constexpr s16 DEFAULT_VIBRATION_BIAS = 8;

  explicit ConfigModeController(u32 index, ControllerType type);

  /// Returns the high nibble of the ID byte in analog mode.
  virtual u8 GetAnalogModeID() const;

  /// Returns the number of motors that 4Dh can map.
  virtual u8 GetNumMotors() const;

  /// Returns the actuator count sent in byte 5 of 45h.
  virtual u8 GetCommand45Actuators() const;

  /// Patches the 4Ch reply for the parameter byte.
  virtual void PatchCommand4C(u8 param);

  /// Returns true if the motor bytes of 42h control the small motor before DualShock mode is enabled.
  virtual bool HasLegacyRumble() const;

  /// Returns true if 43h can enter config mode in the current mode.
  virtual bool CanEnterConfigMode() const;

  /// Returns the bind index for a motor's vibration.
  virtual u32 GetMotorBindIndex(u32 motor) const = 0;

  /// Sets a motor's state from the byte mapped to it.
  virtual void SetMotorState(u32 motor, u8 value);

  /// Shows the OSD message for an analog mode change.
  virtual void ShowModeMessage(bool enabled);

  /// Fills the reply to 42h after the status byte.
  virtual void Poll() = 0;

  u8 GetIDByte() const override;
  bool BeginCommand(u8 command) override;
  void ProcessCommandByte(u32 step, u8 data_in) override;
  void EndCommand() override;

  /// Resets the mode, config mode and rumble state, for Reset().
  void ResetConfigState();

  /// Applies the analog mode and motor state after loading a state.
  void FinishLoadState(bool analog_mode, const MotorState& motor_state);

  float GetMotorStrength(u32 motor) const;
  void ResetRumbleConfig();

  void SetAnalogMode(bool enabled, bool show_message);

  /// Toggles analog mode for the mode button, at the end of the transfer if one is in progress.
  void ToggleAnalogMode();
  void ProcessAnalogModeToggle();

  Command m_command = Command::ReadPad;

  bool m_analog_mode = false;
  bool m_analog_locked = false;
  bool m_dualshock_enabled = false;
  bool m_configuration_mode = false;
  bool m_analog_toggle_queued = false;
  u8 m_status_byte = 0x5A;

  std::array<u8, 6> m_rumble_config;
  MotorState m_motor_state{};
  std::array<s16, MAX_MOTORS> m_vibration_bias{DEFAULT_VIBRATION_BIAS, DEFAULT_VIBRATION_BIAS};

private:
  // Get number of response halfwords (excluding the initial controller info halfword)
  u8 GetResponseNumHalfwords() const;
};
