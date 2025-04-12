// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "input_types.h"
#include "settings.h"
#include "types.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

class SettingsInterface;
class StateWrapper;
class HostInterface;

class Controller
{
public:
  struct ControllerBindingInfo
  {
    const char* name;
    const char* display_name;
    const char* icon_name;
    u32 bind_index;
    InputBindingInfo::Type type;
    GenericInputBinding generic_mapping;
  };

  struct ControllerInfo
  {
    ControllerType type;
    const char* name;
    const char* display_name;
    const char* icon_name;
    std::span<const ControllerBindingInfo> bindings;
    std::span<const SettingInfo> settings;

    /// Returns localized controller type name.
    std::string_view GetDisplayName() const;

    /// Returns localized controller type name.
    std::string_view GetBindingDisplayName(const ControllerBindingInfo& bi) const;
  };

  /// Default stick deadzone/sensitivity.
  static constexpr float DEFAULT_STICK_DEADZONE = 0.0f;
  static constexpr float DEFAULT_STICK_SENSITIVITY = 1.33f;
  static constexpr float DEFAULT_BUTTON_DEADZONE = 0.25f;

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

  /// Returns the current state of the specified vibration motor.
  virtual float GetVibrationMotorState(u32 index) const;

  /// Returns true if the controller supports analog mode, and it is active.
  virtual bool InAnalogMode() const;

  /// Returns analog input bytes packed as a u32. Values are specific to controller type.
  virtual std::optional<u32> GetAnalogInputBytes() const;

  /// Returns the colour to use in the input overlay.
  virtual u32 GetInputOverlayIconColor() const;

  /// Loads/refreshes any per-controller settings.
  virtual void LoadSettings(const SettingsInterface& si, const char* section, bool initial);

  /// Creates a new controller of the specified type.
  static std::unique_ptr<Controller> Create(ControllerType type, u32 index);

  /// Returns a list of all controller types.
  static const std::array<const ControllerInfo*, static_cast<size_t>(ControllerType::Count)>& GetControllerInfoList();

  /// Returns general information for the specified controller type.
  static const ControllerInfo& GetControllerInfo(ControllerType type);
  static const ControllerInfo* GetControllerInfo(std::string_view name);

  /// Applies an analog deadzone/sensitivity.
  static float ApplyAnalogDeadzoneSensitivity(float deadzone, float sensitivity, float value)
  {
    return (value < deadzone) ? 0.0f : ((value - deadzone) / (1.0f - deadzone) * sensitivity);
  }

  /// Returns true if the specified coordinates are inside a circular deadzone.
  static bool InCircularDeadzone(float deadzone, float pos_x, float pos_y);

  /// Converts a global pad index to a multitap port and slot.
  static std::tuple<u32, u32> ConvertPadToPortAndSlot(u32 index);

  /// Converts a multitap port and slot to a global pad index.
  static u32 ConvertPortAndSlotToPad(u32 port, u32 slot);

  /// Returns true if the given pad index is a multitap slot.
  static bool PadIsMultitapSlot(u32 index);
  static bool PortAndSlotIsMultitap(u32 port, u32 slot);

  /// Returns the configuration section for the specified gamepad.
  static std::string GetSettingsSection(u32 pad);

  /// Returns a printable label for a given port.
  static const char* GetPortDisplayName(u32 port, u32 slot, bool mtap);
  static const char* GetPortDisplayName(u32 index);

  /// List of controller indices in the order that they should be displayed.
  static const std::array<u32, NUM_CONTROLLER_AND_CARD_PORTS> PortDisplayOrder;

  /// Returns true if automatic analog mode can be used.
  static bool CanStartInAnalogMode(ControllerType ctype);

protected:
  u32 m_index;
};
