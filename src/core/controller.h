#pragma once
#include "common/image.h"
#include "settings.h"
#include "types.h"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class StateWrapper;
class HostInterface;

class Controller
{
public:
  enum class AxisType : u8
  {
    Full,
    Half
  };

  using ButtonList = std::vector<std::pair<std::string, s32>>;
  using AxisList = std::vector<std::tuple<std::string, s32, AxisType>>;
  using SettingList = std::vector<SettingInfo>;

  Controller();
  virtual ~Controller();

  /// Returns the type of controller.
  virtual ControllerType GetType() const = 0;

  /// Gets the integer code for an axis in the specified controller type.
  virtual std::optional<s32> GetAxisCodeByName(std::string_view axis_name) const;

  /// Gets the integer code for a button in the specified controller type.
  virtual std::optional<s32> GetButtonCodeByName(std::string_view button_name) const;

  virtual void Reset();
  virtual bool DoState(StateWrapper& sw, bool apply_input_state);

  // Resets all state for the transferring to/from the device.
  virtual void ResetTransferState();

  // Returns the value of ACK, as well as filling out_data.
  virtual bool Transfer(const u8 data_in, u8* data_out);

  /// Changes the specified axis state. Values are normalized from -1..1.
  virtual void SetAxisState(s32 axis_code, float value);

  /// Changes the specified button state.
  virtual void SetButtonState(s32 button_code, bool pressed);

  /// Returns a bitmask of the current button states, 1 = on.
  virtual u32 GetButtonStateBits() const;

  /// Returns analog input bytes packed as a u32. Values are specific to controller type.
  virtual std::optional<u32> GetAnalogInputBytes() const;

  /// Returns the number of vibration motors.
  virtual u32 GetVibrationMotorCount() const;

  /// Queries the state of the specified vibration motor. Values are normalized from 0..1.
  virtual float GetVibrationMotorStrength(u32 motor);

  /// Loads/refreshes any per-controller settings.
  virtual void LoadSettings(const char* section);

  /// Returns the software cursor to use for this controller, if any.
  virtual bool GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale, bool* relative_mode);

  /// Creates a new controller of the specified type.
  static std::unique_ptr<Controller> Create(ControllerType type, u32 index);

  /// Gets the integer code for an axis in the specified controller type.
  static std::optional<s32> GetAxisCodeByName(ControllerType type, std::string_view axis_name);

  /// Gets the integer code for a button in the specified controller type.
  static std::optional<s32> GetButtonCodeByName(ControllerType type, std::string_view button_name);

  /// Returns a list of axises for the specified controller type.
  static AxisList GetAxisNames(ControllerType type);

  /// Returns a list of buttons for the specified controller type.
  static ButtonList GetButtonNames(ControllerType type);

  /// Returns the number of vibration motors.
  static u32 GetVibrationMotorCount(ControllerType type);

  /// Returns settings for the controller.
  static SettingList GetSettings(ControllerType type);
};
