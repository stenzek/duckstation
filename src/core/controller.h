#pragma once
#include "common/image.h"
#include "settings.h"
#include "types.h"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

class SettingsInterface;
class StateWrapper;
class HostInterface;

enum class GenericInputBinding : u8;

class Controller
{
public:
  enum class ControllerBindingType : u8
  {
    Unknown,
    Button,
    Axis,
    HalfAxis,
    Motor,
    Macro
  };

  enum class VibrationCapabilities : u8
  {
    NoVibration,
    LargeSmallMotors,
    SingleMotor,
    Count
  };

  struct ControllerBindingInfo
  {
    const char* name;
    const char* display_name;
    u32 bind_index;
    ControllerBindingType type;
    GenericInputBinding generic_mapping;
  };

  struct ControllerInfo
  {
    ControllerType type;
    const char* name;
    const char* display_name;
    const ControllerBindingInfo* bindings;
    u32 num_bindings;
    const SettingInfo* settings;
    u32 num_settings;
    VibrationCapabilities vibration_caps;
  };

  /// Default stick deadzone/sensitivity.
  static constexpr float DEFAULT_STICK_DEADZONE = 0.0f;
  static constexpr float DEFAULT_STICK_SENSITIVITY = 1.33f;

  Controller(u32 index);
  virtual ~Controller();

  /// Returns the type of controller.
  virtual ControllerType GetType() const = 0;

  virtual void Reset();
  virtual bool DoState(StateWrapper& sw, bool apply_input_state);

  // Resets all state for the transferring to/from the device.
  virtual void ResetTransferState();

  // Returns the value of ACK, as well as filling out_data.
  virtual bool Transfer(const u8 data_in, u8* data_out);

  /// Changes the specified axis state. Values are normalized from -1..1.
  virtual float GetBindState(u32 index) const;

  /// Changes the specified bind state. Values are normalized from -1..1.
  virtual void SetBindState(u32 index, float value);

  /// Returns a bitmask of the current button states, 1 = on.
  virtual u32 GetButtonStateBits() const;

  /// Returns analog input bytes packed as a u32. Values are specific to controller type.
  virtual std::optional<u32> GetAnalogInputBytes() const;

  /// Loads/refreshes any per-controller settings.
  virtual void LoadSettings(SettingsInterface& si, const char* section);

  /// Returns the software cursor to use for this controller, if any.
  virtual bool GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale, bool* relative_mode);

  /// Creates a new controller of the specified type.
  static std::unique_ptr<Controller> Create(ControllerType type, u32 index);

  /// Returns the default type for the specified port.
  static const char* GetDefaultPadType(u32 pad);

  /// Returns a list of controller type names. Pair of [name, display name].
  static std::vector<std::pair<std::string, std::string>> GetControllerTypeNames();

  /// Returns the list of binds for the specified controller type.
  static std::vector<std::string> GetControllerBinds(const std::string_view& type);
  static std::vector<std::string> GetControllerBinds(ControllerType type);

  /// Gets the integer code for an axis in the specified controller type.
  static std::optional<u32> GetBindIndex(ControllerType type, const std::string_view& bind_name);

  /// Returns the vibration configuration for the specified controller type.
  static VibrationCapabilities GetControllerVibrationCapabilities(const std::string_view& type);

  /// Returns general information for the specified controller type.
  static const ControllerInfo* GetControllerInfo(ControllerType type);
  static const ControllerInfo* GetControllerInfo(const std::string_view& name);

  /// Converts a global pad index to a multitap port and slot.
  static std::tuple<u32, u32> ConvertPadToPortAndSlot(u32 index);

  /// Converts a multitap port and slot to a global pad index.
  static u32 ConvertPortAndSlotToPad(u32 port, u32 slot);

  /// Returns true if the given pad index is a multitap slot.
  static bool PadIsMultitapSlot(u32 index);
  static bool PortAndSlotIsMultitap(u32 port, u32 slot);

  /// Returns the configuration section for the specified gamepad.
  static std::string GetSettingsSection(u32 pad);

  /// Applies an analog deadzone/sensitivity.
  static float ApplyAnalogDeadzoneSensitivity(float deadzone, float sensitivity, float value)
  {
    return (value < deadzone) ? 0.0f : ((value - deadzone) / (1.0f - deadzone) * sensitivity);
  }

protected:
  u32 m_index;
};
