// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "common/small_string.h"
#include "common/types.h"
#include "input_manager.h"

class Error;
class SettingsInterface;

class ForceFeedbackDevice;

class InputSource
{
public:
  InputSource();
  virtual ~InputSource();

  // Sets up the input source.
  virtual bool Initialize(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) = 0;

  /// Updates the settings for this input source. This should be called when settings change.
  virtual void UpdateSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) = 0;

  /// Reloads the devices for this input source. This should be called when a device change is detected.
  virtual bool ReloadDevices() = 0;

  /// Shuts down the input source, releasing any resources it holds.
  virtual void Shutdown() = 0;

  /// Polls the input source for events. This should be called at a regular interval, such as every frame.
  virtual void PollEvents() = 0;

  /// Returns the current value for the specified device and key.
  virtual std::optional<float> GetCurrentValue(InputBindingKey key) = 0;

  /// Returns true if the source contains the specified device.
  virtual bool ContainsDevice(std::string_view device) const = 0;

  /// Parses a key string for the specified device. Returns std::nullopt if the key is not valid.
  virtual std::optional<InputBindingKey> ParseKeyString(std::string_view device, std::string_view binding) = 0;

  /// Converts a key to a string representation. The string should be suitable for parsing with ParseKeyString().
  virtual TinyString ConvertKeyToString(InputBindingKey key) = 0;

  /// Converts a key to an icon representation. The icon is suitable for display in the UI.
  virtual TinyString ConvertKeyToIcon(InputBindingKey key, InputManager::BindingIconMappingFunction mapper) = 0;

  /// Enumerates available devices. Returns a pair of the prefix (e.g. SDL-0) and the device name.
  virtual InputManager::DeviceList EnumerateDevices() = 0;

  /// Enumerates available vibration motors at the time of call.
  virtual InputManager::DeviceEffectList EnumerateEffects(std::optional<InputBindingInfo::Type> type,
                                                          std::optional<InputBindingKey> for_device) = 0;

  /// Returns the number of pollable devices managed by this source.
  virtual u32 GetPollableDeviceCount() const = 0;

  /// Retrieves bindings that match the generic bindings for the specified device.
  /// Returns false if it's not one of our devices.
  virtual bool GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping) = 0;

  /// Informs the source of a new vibration motor state. Changes may not take effect until the next PollEvents() call.
  virtual void UpdateMotorState(InputBindingKey key, float intensity) = 0;

  /// Concurrently update both motors where possible, to avoid redundant packets.
  virtual void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                                float small_intensity);

  /// Adjusts intensities of LEDs or other indicators on the device.
  virtual void UpdateLEDState(InputBindingKey key, float intensity) = 0;

  /// Creates a force-feedback device from this source.
  virtual std::unique_ptr<ForceFeedbackDevice> CreateForceFeedbackDevice(std::string_view device, Error* error) = 0;

  /// Creates a key for a generic controller device.
  static InputBindingKey MakeGenericControllerDeviceKey(InputSourceType clazz, u32 controller_index);

  /// Creates a key for a generic controller axis event.
  static InputBindingKey MakeGenericControllerAxisKey(InputSourceType clazz, u32 controller_index, s32 axis_index);

  /// Creates a key for a generic controller button event.
  static InputBindingKey MakeGenericControllerButtonKey(InputSourceType clazz, u32 controller_index, s32 button_index);

  /// Creates a key for a generic controller hat event.
  static InputBindingKey MakeGenericControllerHatKey(InputSourceType clazz, u32 controller_index, s32 hat_index,
                                                     u8 hat_direction, u32 num_directions);

  /// Creates a key for a generic controller sensor event.
  static InputBindingKey MakeGenericControllerSensorKey(InputSourceType clazz, u32 controller_index, u32 sensor_index);

  /// Creates a key for a generic controller motor event.
  static InputBindingKey MakeGenericControllerMotorKey(InputSourceType clazz, u32 controller_index, s32 motor_index);

  /// Parses a generic controller key string.
  static std::optional<InputBindingKey> ParseGenericControllerKey(InputSourceType clazz, std::string_view source,
                                                                  std::string_view sub_binding);

  /// Converts a generic controller key to a string.
  static std::string ConvertGenericControllerKeyToString(InputBindingKey key);

#ifdef _WIN32
  static std::unique_ptr<InputSource> CreateDInputSource();
  static std::unique_ptr<InputSource> CreateXInputSource();
  static std::unique_ptr<InputSource> CreateWin32RawInputSource();
#endif
#ifndef __ANDROID__
  static std::unique_ptr<InputSource> CreateSDLSource();
  static void CopySDLSourceSettings(SettingsInterface* dest_si, const SettingsInterface& src_si);
#else
  static std::unique_ptr<InputSource> CreateAndroidSource();
#endif
};
