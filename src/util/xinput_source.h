// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/windows_headers.h"
#include "input_source.h"
#include <Xinput.h>
#include <array>
#include <functional>
#include <mutex>
#include <vector>

class SettingsInterface;

class XInputSource final : public InputSource
{
public:
  enum : u32
  {
    NUM_CONTROLLERS = XUSER_MAX_COUNT, // 4
    NUM_BUTTONS = 15,
  };

  enum : u32
  {
    AXIS_LEFTX,
    AXIS_LEFTY,
    AXIS_RIGHTX,
    AXIS_RIGHTY,
    AXIS_LEFTTRIGGER,
    AXIS_RIGHTTRIGGER,
    NUM_AXES,
  };

  XInputSource();
  ~XInputSource();

  bool Initialize(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void UpdateSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  bool ReloadDevices() override;
  void Shutdown() override;

  void PollEvents() override;
  std::optional<float> GetCurrentValue(InputBindingKey key) override;
  InputManager::DeviceList EnumerateDevices() override;
  InputManager::DeviceEffectList EnumerateEffects(std::optional<InputBindingInfo::Type> type,
                                                  std::optional<InputBindingKey> for_device) override;
  u32 GetPollableDeviceCount() const override;
  bool GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping) override;
  void UpdateMotorState(InputBindingKey key, float intensity) override;
  void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                        float small_intensity) override;
  void UpdateLEDState(InputBindingKey key, float intensity) override;

  bool ContainsDevice(std::string_view device) const override;
  std::optional<InputBindingKey> ParseKeyString(std::string_view device, std::string_view binding) override;
  TinyString ConvertKeyToString(InputBindingKey key) override;
  TinyString ConvertKeyToIcon(InputBindingKey key, InputManager::BindingIconMappingFunction mapper) override;
  void SetSubclassPollDeviceList(InputSubclass subclass, const std::span<const InputBindingKey>* devices) override;
  std::unique_ptr<ForceFeedbackDevice> CreateForceFeedbackDevice(std::string_view device, Error* error) override;

private:
  struct ControllerData
  {
    XINPUT_STATE last_state;
    XINPUT_VIBRATION last_vibration = {};
    bool connected = false;
    bool has_large_motor = false;
    bool has_small_motor = false;
  };

  using ControllerDataArray = std::array<ControllerData, NUM_CONTROLLERS>;

  void CheckForStateChanges(u32 index, const XINPUT_STATE& new_state);
  void HandleControllerConnection(u32 index, const XINPUT_STATE& state);
  void HandleControllerDisconnection(u32 index);

  static std::string GetDeviceIdentifier(u32 index);
  static std::string GetDeviceName(u32 index);

  ControllerDataArray m_controllers;

  HMODULE m_xinput_module{};
  DWORD(WINAPI* m_xinput_get_state)(DWORD, XINPUT_STATE*) = nullptr;
  DWORD(WINAPI* m_xinput_set_state)(DWORD, XINPUT_VIBRATION*) = nullptr;
  DWORD(WINAPI* m_xinput_get_capabilities)(DWORD, DWORD, XINPUT_CAPABILITIES*) = nullptr;
};
