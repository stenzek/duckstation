#pragma once
#include "types.h"
#include <memory>
#include <optional>
#include <string_view>

class StateWrapper;

class Controller
{
public:
  Controller();
  virtual ~Controller();

  /// Returns the type of controller.
  virtual ControllerType GetType() const = 0;

  /// Gets the integer code for an axis in the specified controller type.
  virtual std::optional<s32> GetAxisCodeByName(std::string_view axis_name) const;

  /// Gets the integer code for a button in the specified controller type.
  virtual std::optional<s32> GetButtonCodeByName(std::string_view button_name) const;

  virtual void Reset();
  virtual bool DoState(StateWrapper& sw);

  // Resets all state for the transferring to/from the device.
  virtual void ResetTransferState();

  // Returns the value of ACK, as well as filling out_data.
  virtual bool Transfer(const u8 data_in, u8* data_out);

  /// Changes the specified axis state. Values are normalized from -1..1.
  virtual void SetAxisState(s32 axis_code, float value);

  /// Changes the specified button state.
  virtual void SetButtonState(s32 button_code, bool pressed);

  /// Returns the number of vibration motors.
  virtual u32 GetVibrationMotorCount() const;

  /// Queries the state of the specified vibration motor. Values are normalized from 0..1.
  virtual float GetVibrationMotorStrength(u32 motor);

  /// Creates a new controller of the specified type.
  static std::unique_ptr<Controller> Create(ControllerType type);

  /// Gets the integer code for an axis in the specified controller type.
  static std::optional<s32> GetAxisCodeByName(ControllerType type, std::string_view axis_name);

  /// Gets the integer code for a button in the specified controller type.
  static std::optional<s32> GetButtonCodeByName(ControllerType type, std::string_view button_name);
};
