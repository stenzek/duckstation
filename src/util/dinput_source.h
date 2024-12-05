// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#define DIRECTINPUT_VERSION 0x0800
#include "common/windows_headers.h"
#include "core/types.h"
#include "input_source.h"
#include <array>
#include <dinput.h>
#include <functional>
#include <mutex>
#include <vector>
#include <wrl/client.h>

class DInputSource final : public InputSource
{
public:
  enum HAT_DIRECTION : u32
  {
    HAT_DIRECTION_UP = 0,
    HAT_DIRECTION_DOWN = 1,
    HAT_DIRECTION_LEFT = 2,
    HAT_DIRECTION_RIGHT = 3,
    NUM_HAT_DIRECTIONS = 4,
  };

  enum : u32
  {
    MAX_NUM_BUTTONS = 32,
  };

  DInputSource();
  ~DInputSource() override;

  bool Initialize(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void UpdateSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  bool ReloadDevices() override;
  void Shutdown() override;

  void PollEvents() override;
  std::vector<std::pair<std::string, std::string>> EnumerateDevices() override;
  std::vector<InputBindingKey> EnumerateMotors() override;
  bool GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping) override;
  void UpdateMotorState(InputBindingKey key, float intensity) override;
  void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                        float small_intensity) override;

  bool ContainsDevice(std::string_view device) const override;
  std::optional<InputBindingKey> ParseKeyString(std::string_view device, std::string_view binding) override;
  TinyString ConvertKeyToString(InputBindingKey key) override;
  TinyString ConvertKeyToIcon(InputBindingKey key) override;

  std::unique_ptr<ForceFeedbackDevice> CreateForceFeedbackDevice(std::string_view device, Error* error) override;

private:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  struct ControllerData
  {
    ComPtr<IDirectInputDevice8W> device;
    DIJOYSTATE last_state = {};
    GUID guid = {};
    std::vector<u32> axis_offsets;
    u32 num_buttons = 0;

    // NOTE: We expose hats as num_buttons + (hat_index * 4) + direction.
    u32 num_hats = 0;

    bool needs_poll = true;
  };

  using ControllerDataArray = std::vector<ControllerData>;

  static std::array<bool, NUM_HAT_DIRECTIONS> GetHatButtons(DWORD hat);
  static std::string GetDeviceIdentifier(u32 index);

  bool AddDevice(ControllerData& cd, const std::string& name);

  void CheckForStateChanges(size_t index, const DIJOYSTATE& new_state);

  ControllerDataArray m_controllers;

  HMODULE m_dinput_module{};
  ComPtr<IDirectInput8W> m_dinput;
  LPCDIDATAFORMAT m_joystick_data_format{};
  HWND m_toplevel_window = NULL;
};
