// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "input_manager.h"

#include "common/thirdparty/SmallVector.h"
#include "common/timer.h"

namespace InputManager {

// ------------------------------------------------------------------------
// Binding Type
// ------------------------------------------------------------------------
// This class tracks both the keys which make it up (for chords), as well
// as the state of all buttons. For button callbacks, it's fired when
// all keys go active, and for axis callbacks, when all are active and
// the value changes.

struct InputBinding
{
  InputBindingKey keys[MAX_KEYS_PER_BINDING] = {};
  InputEventHandler handler;
  u8 num_keys = 0;
  u8 full_mask = 0;
  u8 current_mask = 0;
  bool activate_when_captured = false;
};

struct PadVibrationBinding
{
  u64 pad_and_bind_index;        ///< Combined pad index and bind index for quick lookup.
  InputBindingKey binding;       ///< Binding key for this motor.
  Timer::Value last_update_time; ///< Last time this motor was updated.
  InputSource* source;           ///< Input source for this motor.
  float last_intensity;          ///< Last intensity we sent to the motor.

  ALWAYS_INLINE static u64 PackPadAndBindIndex(u32 pad_index, u32 bind_index)
  {
    return (static_cast<u64>(pad_index) << 32) | static_cast<u64>(bind_index);
  }

  ALWAYS_INLINE static std::tuple<u32, u32> UnpackPadAndBindIndex(u64 packed)
  {
    return {static_cast<u32>(packed >> 32), static_cast<u32>(packed)};
  }
};

struct PadLEDBinding
{
  InputBindingKey binding; ///< Binding key for this LED.
  InputSource* source;     ///< Input source for this LED.
  float last_intensity;    ///< Last intensity we sent to the LED.
  u32 pad_index;           ///< Pad index this LED is for.
};

struct MacroButton
{
  u16 pad_index;                     ///< Pad index this macro button is for.
  u16 macro_index;                   ///< Index of the macro button.
  llvm::SmallVector<u32, 2> buttons; ///< Buttons to activate.
  u16 toggle_frequency;              ///< Interval at which the buttons will be toggled, if not 0.
  u16 toggle_counter;                ///< When this counter reaches zero, buttons will be toggled.
  bool toggle_state;                 ///< Current state for turbo.
  bool trigger_state;                ///< Whether the macro button is active.
  bool trigger_toggle;               ///< Whether the macro is trigged by holding or press.
  u8 trigger_pressure;               ///< Pressure to apply when macro is active.
};

struct PointerAxisState
{
  float delta;
  float last_value;
};

struct KeyCodeData
{
  u32 usb_code;
  u32 native_code;
  const char* name;
  const char* icon_name;
};

/// This is a multimap containing any binds related to the specified key.
using BindingMap = std::unordered_multimap<InputBindingKey, std::shared_ptr<InputBinding>, InputBindingKeyHash>;

/// This is an array of all the pad vibration bindings, indexed by pad index.
using VibrationBindingArray = std::vector<PadVibrationBinding>;

/// This is an array of all the pad LED bindings, indexed by pad index.
using PadLEDBindingArray = std::vector<PadLEDBinding>;

/// Callback for pointer movement events. The key is the pointer key, and the value is the axis value.
using PointerMoveCallback = std::function<void(InputBindingKey key, float value)>;

/// Updates internal state for any binds for this key, and fires callbacks as needed.
/// Returns true if anything was bound to this key, otherwise false.
void InvokeEvents(InputBindingKey key, float value, GenericInputBinding generic_key = GenericInputBinding::Unknown);

/// Called when a new input device is connected.
void OnInputDeviceConnected(InputBindingKey key, std::string_view identifier, std::string_view device_name,
                            std::optional<GamepadButtonType> gamepad_button_type);

/// Called when an input device is disconnected.
void OnInputDeviceDisconnected(InputBindingKey key, std::string_view identifier);

} // namespace InputManager

namespace Host {

/// Called when a new input device is connected.
void OnInputDeviceConnected(InputBindingKey key, std::string_view identifier, std::string_view device_name);

/// Called when an input device is disconnected.
void OnInputDeviceDisconnected(InputBindingKey key, std::string_view identifier);

/// Enables "relative" mouse mode, locking the cursor position and returning relative coordinates.
void SetMouseMode(bool relative, bool hide_cursor);

/// Return the current window handle. Needed for DInput.
std::optional<WindowInfo> GetTopLevelWindowInfo();

} // namespace Host
